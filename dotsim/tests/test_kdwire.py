"""KD wire: framing, record translation and the newest-frame-only sender."""
import asyncio
import json
import re
import struct
from pathlib import Path

import pytest

import kdwire
import server


def test_roundtrip():
    buf = kdwire.encode(kdwire.LED, b"\xff\x00\x00") + kdwire.encode(kdwire.HAPTIC, b"\x01\x10\x00")
    got = [(t, bytes(p)) for t, p in kdwire.decode(buf)]
    assert got == [(kdwire.LED, b"\xff\x00\x00"), (kdwire.HAPTIC, b"\x01\x10\x00")]
    assert kdwire.encode(0x20, b"ab") == b"KD\x20\x02\x00\x00\x00ab"


def test_resync_after_garbage():
    f = kdwire.encode(kdwire.LED, b"abc")
    buf = b"\x00\xffKX" + f + b"junk" + f
    assert [(t, bytes(p)) for t, p in kdwire.decode(buf)] == [(kdwire.LED, b"abc")] * 2


def test_too_long_refused():
    bad = b"KD" + bytes([kdwire.FB_RECT]) + struct.pack("<I", kdwire.MAX_LEN + 1) + b"\0" * 16
    buf = bad + kdwire.encode(kdwire.LED, b"abc")
    assert [(t, bytes(p)) for t, p in kdwire.decode(buf)] == [(kdwire.LED, b"abc")]


def test_truncated_frame_yields_nothing():
    assert list(kdwire.decode(kdwire.encode(kdwire.LED, b"abc")[:-1])) == []


def test_buttons_bit_order():
    # kd: top=1 bot=2 left=4 right=8 up=16 down=32; sim: up=1 down=2 left=4 right=8 top=16 bot=32
    cases = {1: 16, 2: 32, 4: 4, 8: 8, 16: 1, 32: 2, 0: 0, 1 | 16: 17}
    for kd, sim in cases.items():
        assert kdwire.kd_to_sim_lines(kdwire.BUTTONS, bytes([kd])) == [f"b {sim}"]
    # the sim side of the map is the bridge's own (KDOTSIM_BTN_* in kdotsim.h)
    h = (Path(__file__).resolve().parents[1] / "hostsim/components/khal/include/kdotsim.h").read_text()
    for name, bit in {"UP": 1, "DOWN": 2, "LEFT": 4, "RIGHT": 8, "TOP": 16, "BOT": 32}.items():
        m = re.search(r"#define KDOTSIM_BTN_%s\s+\(1U << (\d)\)" % name, h)
        assert m and 1 << int(m.group(1)) == bit, name


def test_inputs_to_lines():
    f = kdwire.kd_to_sim_lines
    assert f(kdwire.TOUCH, struct.pack("<BHH", 1, 10, 20)) == ["t 1 10 20"]
    assert f(kdwire.TOUCH, struct.pack("<BHH", 0, 10, 20)) == ["t 0 10 20"]
    assert f(kdwire.BATTERY, bytes([77, 1])) == ["p 77 1"]
    m = struct.pack("<10i", 1, 2, 3, 4, 5, 6, 7, 8, 9, -10)
    assert f(kdwire.MOTION, m) == ["i 1 2 3 4 5 6 7 8 9 -10"]
    assert f(kdwire.CONSOLE_IN, b"help\nrm\n") == ["c help rm"]
    # short or unknown payloads are ignored, never raise
    assert f(kdwire.TOUCH, b"\0") == []
    assert f(kdwire.MOTION, b"\0") == []
    assert f(0x7F, b"") == []


def test_sim_records():
    r = kdwire.sim_record_to_kd
    (led,) = r("E", b"\x01\x02\x03")
    assert [(t, bytes(p)) for t, p in kdwire.decode(led)] == [(kdwire.LED, b"\x01\x02\x03")]
    (h,) = r("H", struct.pack("<I", 300))
    assert [(t, bytes(p)) for t, p in kdwire.decode(h)] == [(kdwire.HAPTIC, b"\xff" + struct.pack("<H", 300))]
    pcm = struct.pack("<I", 16000) + b"\x01\x00\x02\x00"
    (p,) = r("P", pcm)
    assert [(t, bytes(x)) for t, x in kdwire.decode(p)] == [(kdwire.PCM_OUT, struct.pack("<IB", 16000, 1) + pcm[4:])]
    (tone,) = r("T", struct.pack("<IIB", 440, 100, 50))
    (t, body), = kdwire.decode(tone)
    rate, ch = struct.unpack("<IB", body[:5])
    assert t == kdwire.PCM_OUT and ch == 1 and len(body) - 5 == rate * 100 // 1000 * 2
    assert any(body[5:])
    (log,) = r("L", b"hi \xc3\xa9")
    assert json.loads(log) == {"console": "hi é"}
    assert r("B", b"\x40") == [] and r("X", b"") == [] and r("N", b"x") == []


def test_frame_to_jpeg_rect():
    w, h = 4, 2
    (f,) = kdwire.sim_record_to_kd("F", struct.pack("<HH", w, h) + b"\xff\xff" * w * h)
    ((t, body),) = kdwire.decode(f)
    assert t == kdwire.FB_JPEG
    assert struct.unpack("<4H", body[:8]) == (0, 0, w, h)
    assert bytes(body[8:10]) == b"\xff\xd8"


class FakeWs:
    closed = False

    def __init__(self):
        self.gate = asyncio.Event()
        self.sent = []

    async def send_bytes(self, data):
        await self.gate.wait()
        self.sent.append(data)


@pytest.mark.asyncio
async def test_frame_dropped_when_socket_busy():
    ws = FakeWs()
    fs = server.FrameSender(ws)
    fs.start()
    fs.put(b"one")
    await asyncio.sleep(0.01)   # sender is now stuck inside send_bytes(one)
    fs.put(b"two")
    fs.put(b"three")            # replaces two; nothing queues
    ws.gate.set()
    await asyncio.sleep(0.01)
    assert ws.sent == [b"one", b"three"]
    await fs.stop()


def test_kd_socket_routed():
    paths = {r.resource.canonical for r in server.make_app().router.routes() if r.method == "GET"}
    assert "/ws" in paths and "/ws-legacy" not in paths
