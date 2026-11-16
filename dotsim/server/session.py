"""One browser tab, one sim process: the Session, and the hub that pairs a user's two devices.

The sim talks to the server over a unix socket (hostsim/components/khal/src/kdotsim_bridge.c, whose header
lists every record and line):

  sim -> server: [u8 type][u32 len LE][payload]
      F frame (u16 w, u16 h, RGB565), E led rgb, B brightness, H haptic ms,
      T tone (u32 hz, u32 ms, u8 vol), P pcm (u32 rate, s16...), L log, X exit
      N link message (text) for the paired device
  server -> sim: text lines, e.g. "b <mask>", "t <0|1> <x> <y>", "p <pct> <usb>", "i <motion>",
      "c <console line>", "s <0|1>" (microSD out/in), "n <text>" (link message),
      "m <w> <h> <base64>" (mirror picture), "q" (quit)

To the page (/ws): F becomes FB_JPEG (pump_frames, at most FPS a second, newest frame only); E, H, P, T, L go
through kdwire.sim_record_to_kd (L as JSON {"console": ...}); N and X are handled here. From the page: KD
frames (kdwire.kd_to_sim_lines) and JSON text (Session.on_browser).

Kode Link: two sessions of the same user (a "dot" and a "watch" role) are paired through the hub. On
hardware this would be BLE; the apps see the same thing: text messages both ways, plus "hub peer 0|1" and
"sys ..." from the hub itself.
"""
import asyncio
import base64
import datetime
import json
import logging
import os
import shutil
import struct
import tempfile
import time
from pathlib import Path

import numpy as np
from aiohttp import web

import kdwire
from config import BUILDS, DATA
from sandbox import SIM_PRLIMIT, data_open, data_save, data_seed, sd_child_pid, sim_cmd
from sdcard import sd_files, sd_open, sd_put, sd_read

log = logging.getLogger("kode")

PROBES = ("bat", "imu", "sd")    # "? <what>" typed in the console goes to the sim as its debug line
FPS = 30
JPEG_Q = 80

sessions = set()
hub = {}           # user -> {"dot": Session, "watch": Session}
MIRROR_GAP = 0.15  # the mirror picture at most ~6 times a second


def mirror_line(buf, w, h):
    px = np.frombuffer(buf, dtype="<u2", count=w * h).reshape(h, w)[::2, ::2]
    mh, mw = px.shape
    return f"m {mw} {mh} " + base64.b64encode(np.ascontiguousarray(px).astype("<u2").tobytes()).decode()


def wall_clock_line():
    now = datetime.datetime.now().astimezone()
    off = int(now.utcoffset().total_seconds()) if now.utcoffset() else 0
    try:
        from zoneinfo import ZoneInfo
        off = int(datetime.datetime.now(ZoneInfo("Europe/Amsterdam")).utcoffset().total_seconds())
    except Exception:  # noqa: BLE001
        pass
    return f"n sys time {int(time.time())} {off}"


def rgb565_to_jpeg(buf, w, h):
    return kdwire.rgb565_to_jpeg(buf, w, h, JPEG_Q)


class FrameSender:
    """Sends a socket's binary frames, keeping only the newest one waiting.

    put() never blocks and never queues: while a send is in flight a newer
    frame replaces the one still pending, so a slow browser sees fewer frames
    instead of falling behind.
    """

    def __init__(self, ws):
        self.ws = ws
        self.pending = None
        self.evt = asyncio.Event()
        self.task = None

    def start(self):
        self.task = asyncio.create_task(self._run())

    def put(self, data):
        self.pending = data
        self.evt.set()

    async def _run(self):
        while True:
            await self.evt.wait()
            self.evt.clear()
            data, self.pending = self.pending, None
            if data is not None and not self.ws.closed:
                await self.ws.send_bytes(data)

    async def stop(self):
        if self.task:
            self.task.cancel()
            try:
                await self.task
            except asyncio.CancelledError:
                pass


class Session:
    """One browser tab, one sim process."""

    def __init__(self, ws, app_id, role="dot", user=""):
        self.ws = ws
        self.sender = FrameSender(ws)   # the screen's KD frames, newest only
        self.app_id = app_id
        self.role = role
        self.user = user
        self.mirror = False
        self.last_mirror = 0.0
        self.proc = None
        self.reader = None
        self.writer = None
        self.tmp = None
        self.frame = None          # latest RGB565 frame (w, h, bytes)
        self.frame_evt = asyncio.Event()
        self.closed = False
        self.sd_fd = None          # the card's directory, once the sim has started (sd_open)
        self.sd_lock = asyncio.Lock()
        self.kid = None
        self.seed_ok = True        # False: the saved data was too big to load, so it is not replaced
        self.data_fd = None        # the sandbox's /data, for the copy-back at the end

    @property
    def peer(self):
        slot = hub.get(self.user, {})
        if self.role == "dot":
            for other in ("watch",):
                p = slot.get(other)
                if p is not None and not p.closed:
                    return p
            return None
        p = slot.get("dot")
        return p if p is not None and not p.closed else None

    def join_hub(self):
        old = hub.setdefault(self.user, {}).get(self.role)
        hub[self.user][self.role] = self
        if old is not None and old is not self:
            old.send_line("n hub peer 0")
        p = self.peer
        self.send_line(wall_clock_line())
        self.send_line(f"n hub peer {1 if p else 0}")
        if p:
            p.send_line("n hub peer 1")

    def leave_hub(self):
        slot = hub.get(self.user, {})
        if slot.get(self.role) is self:
            del slot[self.role]
            p = self.peer
            if p:
                p.mirror = False
                p.send_line("n hub peer 0")

    async def on_link(self, text):
        """A link message from this sim: for the hub itself, or on to the other device."""
        if text.startswith("hub mirror "):
            self.mirror = text.endswith("1")
            return
        if not text.startswith("st "):
            await self.send_json(t="link", dir="uit", s=text)
        p = self.peer
        if p:
            p.send_line("n " + text)
            if not text.startswith("st "):
                await p.send_json(t="link", dir="in", s=text)

    async def send_mirror(self, buf, w, h):
        p = self.peer
        if not p or not p.mirror:
            return
        now = time.monotonic()
        if now - self.last_mirror < MIRROR_GAP:
            return
        self.last_mirror = now
        line = await asyncio.get_running_loop().run_in_executor(None, mirror_line, buf, w, h)
        p.send_line(line)

    async def send_json(self, **kw):
        if not self.ws.closed:
            await self.ws.send_str(json.dumps(kw))

    async def start(self):
        elf = BUILDS / self.app_id / "kode_sim_app.elf"
        if not elf.exists():
            raise web.HTTPNotFound(text="app not built")
        self.tmp = Path(tempfile.mkdtemp(prefix="kode-"))
        os.chmod(self.tmp, 0o777)
        sock_path = self.tmp / "sim.sock"
        data = DATA / self.app_id
        data.mkdir(parents=True, exist_ok=True)

        connected = asyncio.get_running_loop().create_future()

        async def on_conn(reader, writer):
            if connected.done():
                writer.close()
                return
            connected.set_result((reader, writer))

        server = await asyncio.start_unix_server(on_conn, path=str(sock_path))
        os.chmod(sock_path, 0o777)
        seed = self.tmp / "seed"
        self.seed_ok = await asyncio.get_running_loop().run_in_executor(None, data_seed, data, seed)
        cmd = sim_cmd(elf, self.tmp, seed)
        self.proc = await asyncio.create_subprocess_exec(
            *SIM_PRLIMIT, *cmd,
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        try:
            self.reader, self.writer = await asyncio.wait_for(connected, 10)
        except asyncio.TimeoutError:
            err = b""
            if self.proc.stderr:
                try:
                    err = await asyncio.wait_for(self.proc.stderr.read(2000), 1)
                except asyncio.TimeoutError:
                    pass
            raise RuntimeError("sim did not start: " + err.decode(errors="replace"))
        finally:
            server.close()
        # The card, as a directory fd that stays this one directory whatever the app does to its files.
        kid = self.kid = sd_child_pid(self.proc.pid)
        self.sd_fd = sd_open(kid) if kid else None
        if self.sd_fd is None:
            log.warning("sd: no access to the card of pid %s", self.proc.pid)
        self.data_fd = data_open(kid) if kid else None
        if self.data_fd is None:
            log.warning("data: no access to /data of pid %s, nothing will be saved", self.proc.pid)

    async def pump_sim(self):
        """Records from the sim to the browser."""
        try:
            while True:
                hdr = await self.reader.readexactly(5)
                typ = chr(hdr[0])
                (n,) = struct.unpack("<I", hdr[1:])
                body = await self.reader.readexactly(n)
                if typ == "F":
                    w, h = struct.unpack("<HH", body[:4])
                    self.frame = (w, h, body[4:])
                    self.frame_evt.set()
                    if self.role == "watch":
                        await self.send_mirror(body[4:], w, h)
                elif typ == "N":
                    await self.on_link(body.decode(errors="replace").replace("\n", " "))
                elif typ == "X":
                    await self.send_json(t="exit", why=body.decode(errors="replace"))
                else:
                    await self.send_kd(typ, body)
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        rc = await self.proc.wait() if self.proc else None
        err = b""
        if self.proc and self.proc.stderr:
            try:
                err = await asyncio.wait_for(self.proc.stderr.read(4000), 1)
            except asyncio.TimeoutError:
                pass
        await self.send_json(t="ended", rc=rc, err=err.decode(errors="replace")[-1500:])

    async def send_kd(self, typ, body):
        """A sim record to the page, as KD frames (JSON text for the console)."""
        if self.ws.closed:
            return
        for out in kdwire.sim_record_to_kd(typ, body):
            if out[:1] == b"{":
                await self.ws.send_str(out.decode())
            else:
                await self.ws.send_bytes(out)

    async def pump_frames(self):
        """Latest frame to the browser, at most FPS a second, dropping what it cannot keep up with."""
        loop = asyncio.get_running_loop()
        gap = 1.0 / FPS
        last = 0.0
        while not self.closed:
            await self.frame_evt.wait()
            self.frame_evt.clear()
            wait = gap - (time.monotonic() - last)
            if wait > 0:
                await asyncio.sleep(wait)
            w, h, buf = self.frame
            jpg = await loop.run_in_executor(None, rgb565_to_jpeg, buf, w, h)
            last = time.monotonic()
            if self.ws.closed:
                break
            self.sender.put(kdwire.encode(kdwire.FB_JPEG, struct.pack("<4H", 0, 0, w, h) + jpg))

    def on_kd(self, data):
        """Binary KD frames from the page, to the sim's lines."""
        for typ, payload in kdwire.decode(data):
            for line in kdwire.kd_to_sim_lines(typ, payload):
                self.send_line(line)

    def send_line(self, line):
        if self.writer and not self.writer.is_closing():
            self.writer.write((line + "\n").encode())

    async def sd_job(self, what, msg):
        """One card request, its file work off the event loop; the answer goes to the page."""
        loop = asyncio.get_running_loop()
        err = None
        try:
            async with self.sd_lock:
                if self.sd_fd is None:
                    err = "the card is not available"
                elif what == "put":
                    err = await loop.run_in_executor(None, sd_put, self.sd_fd, msg.get("name"), msg.get("b64"))
                elif what == "get":
                    data, err = await loop.run_in_executor(None, sd_read, self.sd_fd, msg.get("name"))
                    if not err:
                        await self.send_json(sd_file={"name": msg["name"], "b64": base64.b64encode(data).decode()})
                        return
                kw = {"sd_files": await loop.run_in_executor(None, sd_files, self.sd_fd) if self.sd_fd is not None else []}
        except (OSError, UnicodeError, ValueError, TypeError) as e:
            err = "the card refused it (%s)" % e.__class__.__name__
            kw = {"sd_files": []}
        if err:
            kw["sd_error"] = err
        if not self.closed:
            await self.send_json(**kw)

    def on_browser(self, msg):
        """A JSON text message from the page: the console line, the microSD switch and its files."""
        if "console_in" in msg:
            text = str(msg["console_in"]).strip()
            if text.startswith("?") and text[1:].strip() in PROBES:
                self.send_line("? " + text[1:].strip())
                return
            for line in kdwire.kd_to_sim_lines(kdwire.CONSOLE_IN, str(msg["console_in"]).encode()):
                self.send_line(line)
        elif "sd" in msg:
            v = msg["sd"]
            self.send_line("s " + ("1" if (v.get("in") if isinstance(v, dict) else v) else "0"))
        elif "sd_ls" in msg:
            asyncio.ensure_future(self.sd_job("ls", {}))
        elif "sd_put" in msg and isinstance(msg["sd_put"], dict):
            asyncio.ensure_future(self.sd_job("put", msg["sd_put"]))
        elif "sd_get" in msg and isinstance(msg["sd_get"], dict):
            asyncio.ensure_future(self.sd_job("get", msg["sd_get"]))

    async def stop(self):
        self.leave_hub()
        self.closed = True
        if self.sd_fd is not None:
            try:
                os.close(self.sd_fd)
            except OSError:
                pass
            self.sd_fd = None
        await self.sender.stop()
        self.frame_evt.set()
        if self.writer:
            try:
                self.send_line("q")
                self.writer.close()
            except Exception:
                pass
        if self.kid:     # the sandbox's init: killing it ends every process in the sandbox at once
            try:
                os.kill(self.kid, 9)
            except OSError:
                pass
        if self.proc and self.proc.returncode is None:
            try:
                self.proc.kill()
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(self.proc.wait(), 3)
            except asyncio.TimeoutError:
                pass
        # the app is dead now, so the copy cannot catch a half-written file
        if self.data_fd is not None:
            fd, self.data_fd = self.data_fd, None
            try:
                if self.seed_ok:
                    await asyncio.get_running_loop().run_in_executor(None, data_save, fd, DATA / self.app_id)
            except Exception:
                log.exception("data: copy-back failed")
            finally:
                os.close(fd)
        if self.tmp:
            shutil.rmtree(self.tmp, ignore_errors=True)
