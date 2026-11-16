"""The Kode Dot's KD wire (crates/kode-sim/src/proto.rs) for the fast engine.

A frame is b"KD", a type byte, a u32 LE length and the payload. This module is
only encode, decode and translation between the wire and the sim process's own
record/line protocol (see server.py); the sockets live in server.py.
"""
import io
import json
import struct

import numpy as np
from PIL import Image

MAGIC = b"KD"
HEADER = 7
MAX_LEN = 600_000

# Copied from proto.rs.
HELLO = 0x01
FB_RECT = 0x10
PANEL = 0x11
FB_PACKED = 0x12
FB_JPEG = 0x13          # ours: u16 x, u16 y, u16 w, u16 h, JPEG bytes
LED = 0x20
HAPTIC = 0x21
PCM_OUT = 0x30          # rate u32, ch u8, then s16le
MIC_START = 0x31
MIC_STOP = 0x32
POWER_OFF = 0x40
TOUCH = 0x80            # u8 down, u16 x, u16 y
BUTTONS = 0x81
PCM_IN = 0x82
BATTERY = 0x83          # u8 pct, u8 usb
CONSOLE_IN = 0x84
MOTION = 0x8D           # ten i32 LE

# kode-sim's button bits -> the sim process's mask (server.BTN_BITS).
_KD_BTN = ((1, 16), (2, 32), (4, 4), (8, 8), (16, 1), (32, 2))  # top bot left right up down

TONE_RATE = 16000
TONE_MAX_MS = 5000


def encode(type, payload=b""):
    return MAGIC + bytes([type]) + struct.pack("<I", len(payload)) + bytes(payload)


def decode(buf):
    """Yield (type, payload memoryview) for each whole frame in buf.

    Resyncs on b"KD"; a length over MAX_LEN is corruption (step past that
    magic); a frame cut short at the end is ignored.
    """
    mv = memoryview(buf)
    raw = bytes(buf)
    i = 0
    while True:
        i = raw.find(MAGIC, i)
        if i < 0 or i + HEADER > len(raw):
            return
        typ = raw[i + 2]
        (n,) = struct.unpack_from("<I", raw, i + 3)
        if n > MAX_LEN:
            i += 2
            continue
        end = i + HEADER + n
        if end > len(raw):
            return
        yield typ, mv[i + HEADER:end]
        i = end


def rgb565_to_jpeg(buf, w, h, quality=80):
    px = np.frombuffer(buf, dtype="<u2", count=w * h).reshape(h, w)
    rgb = np.empty((h, w, 3), dtype=np.uint8)
    rgb[..., 0] = ((px >> 11) & 0x1F) * 255 // 31
    rgb[..., 1] = ((px >> 5) & 0x3F) * 255 // 63
    rgb[..., 2] = (px & 0x1F) * 255 // 31
    bio = io.BytesIO()
    Image.fromarray(rgb, "RGB").save(bio, "JPEG", quality=quality)
    return bio.getvalue()


def _tone_pcm(hz, ms, vol):
    n = TONE_RATE * min(ms, TONE_MAX_MS) // 1000
    if n <= 0 or hz <= 0:
        return b""
    amp = int(32767 * min(vol, 100) / 100 * 0.5)
    t = np.arange(n)
    square = np.where((t * hz * 2 // TONE_RATE) % 2 == 0, amp, -amp)
    return square.astype("<i2").tobytes()


def sim_record_to_kd(typ, body):
    """One sim record (type letter, body) as KD frames for the page.

    A returned item starting with b"{" is JSON text for a text message, not a
    KD frame. B, X and N have no wire form here and give [].
    """
    if typ == "F":
        w, h = struct.unpack("<HH", body[:4])
        jpg = rgb565_to_jpeg(body[4:], w, h)
        return [encode(FB_JPEG, struct.pack("<4H", 0, 0, w, h) + jpg)]
    if typ == "E":
        return [encode(LED, body[:3])]
    if typ == "H":
        (ms,) = struct.unpack("<I", body[:4])
        return [encode(HAPTIC, b"\xff" + struct.pack("<H", min(ms, 0xFFFF)))]
    if typ == "P":
        return [encode(PCM_OUT, body[:4] + b"\x01" + body[4:])]
    if typ == "T":
        hz, ms = struct.unpack("<II", body[:8])
        pcm = _tone_pcm(hz, ms, body[8])
        return [encode(PCM_OUT, struct.pack("<IB", TONE_RATE, 1) + pcm)] if pcm else []
    if typ == "L":
        return [json.dumps({"console": body.decode(errors="replace")}).encode()]
    return []


def kd_to_sim_lines(type, payload):
    """One KD input as the sim's text lines; [] if unknown or malformed."""
    p = bytes(payload)
    if type == TOUCH and len(p) >= 5:
        down, x, y = struct.unpack("<BHH", p[:5])
        return [f"t {1 if down else 0} {x} {y}"]
    if type == BUTTONS and len(p) >= 1:
        return ["b %d" % sum(s for k, s in _KD_BTN if p[0] & k)]
    if type == BATTERY and len(p) >= 2:
        return [f"p {p[0]} {1 if p[1] else 0}"]
    if type == MOTION and len(p) >= 40:
        return ["m " + " ".join(str(v) for v in struct.unpack("<10i", p[:40]))]
    if type == CONSOLE_IN:
        return ["c " + p.decode(errors="replace").replace("\r", " ").replace("\n", " ").strip()]
    return []
