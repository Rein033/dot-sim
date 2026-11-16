"""Dot Sim: an unofficial Kode Dot in the browser, served from one machine.

Every WebSocket session starts its own sim process (a Kode app built for
ESP-IDF's linux target on the real kode-sdk, with the board simulated under
it, see ../hostsim), in a bubblewrap sandbox without network. The process
talks to this server over a unix socket (hostsim/components/khal/src/kdotsim_bridge.c):

  sim -> server: [u8 type][u32 len LE][payload]
      F frame (u16 w, u16 h, RGB565), E led rgb, B brightness, H haptic ms,
      T tone (u32 hz, u32 ms, u8 vol), P pcm (u32 rate, s16...), L log, X exit
      N link message (text) for the paired device
  server -> sim: text lines "b <mask>", "t <0|1> <x> <y>", "s <0|1>" (microSD out/in), "q",
      "n <text>" (link message), "m <w> <h> <base64>" (mirror picture)

Kode Link: two sessions of the same user (a "dot" and a "watch" role) are
paired through the hub below. On hardware this would be BLE; the apps see the
same thing: text messages both ways, plus "hub peer 0|1" and "sys ..." from
the hub itself.

To the browser, on /ws (kode-sim's page, web/ at /, with dotsim/web/ext.js on it):
KD frames (kdwire.py; the screen as FB_JPEG) and JSON text. On /ws-legacy (no
page uses it now): binary 'J'+jpeg or 'P'+rate(u32)+pcm, and JSON text.

Not made, approved or supported by Kode.
"""
import asyncio
import base64
import datetime
import io
import json
import logging
import os
import re
import shutil
import stat
import hashlib
import uuid
import struct
import tempfile
import time
from pathlib import Path

import numpy as np
from aiohttp import web, WSMsgType
from PIL import Image

import kdwire

ROOT = Path(__file__).parent
STATIC = Path(os.environ.get("KODE_STATIC", ROOT.parent / "web"))       # ours, at /dotsim/
KSWEB = Path(os.environ.get("KODE_KSWEB", ROOT.parent.parent / "web"))   # kode-sim's page, at /
KSWEB_FILES = ("sim.js", "audio-worklet.js", "kode-dot.png", "kode-dot.json")
PROBES = ("bat", "imu", "sd")    # "? <what>" typed in the console goes to the sim as its debug line
PORT = int(os.environ.get("KODE_PORT", "8000"))
BUILDS = Path(os.environ.get("KODE_BUILDS", "/opt/kode/builds"))
DATA = Path(os.environ.get("KODE_DATA", "/opt/kode/data"))
MAX_SESSIONS = int(os.environ.get("KODE_MAX_SESSIONS", "4"))
FPS = 30
JPEG_Q = 80

log = logging.getLogger("kode")
BTN_BITS = {"up": 1, "down": 2, "left": 4, "right": 8, "top": 16, "bot": 32}
sessions = set()
hub = {}           # user -> {"dot": Session, "watch": Session}
MIRROR_GAP = 0.15  # spiegelbeeld hooguit ~6 keer per seconde


@web.middleware
async def no_cache(request, handler):
    resp = await handler(request)
    if request.path.endswith((".html", ".js", ".css")) or request.path in ("/", "/docs"):
        resp.headers["Cache-Control"] = "no-cache"
    return resp


def git_version():
    """The commit this server runs, for the page's footer: KODE_VERSION, else the checkout's HEAD."""
    v = os.environ.get("KODE_VERSION", "")
    if v:
        return v
    try:
        import subprocess
        return subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


VERSION = git_version()


def app_list():
    out = []
    for d in sorted(BUILDS.glob("*/kode_sim_app.elf")):
        name = d.parent.name
        if name.startswith(EDIT_PREFIX):
            continue
        meta = {}
        mf = d.parent / "kode-sim.json"
        if mf.exists():
            try:
                meta = json.loads(mf.read_text())
            except ValueError:
                meta = {}
        look = {}
        aj = LOCAL_APPS / name / "appdata" / "app.json"
        if aj.exists():
            try:
                look = json.loads(aj.read_text())
            except ValueError:
                look = {}
        out.append({"id": name, "name": meta.get("name", name), "repo": meta.get("repo", ""),
                    "color": look.get("color", ""), "desc": look.get("description", ""),
                    "icon": (d.parent / "icon.png").exists() or (LOCAL_APPS / name / "appdata" / "icon.png").exists(),
                    "built": int(d.stat().st_mtime)})
    return out


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


# ---- The microSD: names and sizes are checked here, never trusted from the page ----
#
# The card is a size-limited tmpfs inside the sandbox (bwrap --size, SD_MOUNT_ARGS): whatever the app
# writes there stops at 16 MB. The server reaches it through /proc/<sandbox pid>/root, opened ONCE as a
# directory fd (sd_open); every later call works relative to that fd, so nothing the app does to the
# files (links, folders, renames) can lead the server outside the card.

SD_FILE_MAX = 700 * 1024        # one upload
SD_TOTAL_MAX = 16 * 1024 * 1024  # everything on the card, per session (also the tmpfs size)
SD_NAME_MAX = 64
SD_LIST_MAX = 500
SD_MOUNT_ARGS = ["--size", str(SD_TOTAL_MAX), "--tmpfs", "/data/sd"]
SD_TMP = ".put-"


def sd_name_ok(name):
    """A plain file name: no path, no '..', not empty, at most 64 characters and 255 bytes of UTF-8."""
    if not (isinstance(name, str) and 0 < len(name) <= SD_NAME_MAX):
        return False
    try:
        if len(name.encode("utf-8")) > 255:
            return False
    except UnicodeError:       # a lone surrogate from the page's JSON
        return False
    return ("/" not in name and "\\" not in name and name != "." and ".." not in name
            and not any(ord(c) < 32 or ord(c) == 127 for c in name))


def sd_open(pid):
    """A directory fd on the card of the sandbox whose init (or app) is @p pid, or None."""
    try:
        return os.open(f"/proc/{pid}/root/data/sd", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return None


def sd_child_pid(pid):
    """The first process inside the sandbox that @p pid (bwrap) started, or None."""
    try:
        kids = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
        return int(kids[0]) if kids else None
    except (OSError, ValueError):
        return None


def sd_files(dfd):
    """[{name, size}] of the regular files on the card, at most SD_LIST_MAX (links and folders are not files
    here: the app that runs on the card can make them)."""
    out = []
    try:
        with os.scandir(dfd) as it:
            for e in it:
                if e.name.startswith(SD_TMP):
                    continue
                st = e.stat(follow_symlinks=False)
                if stat.S_ISREG(st.st_mode):
                    out.append({"name": e.name, "size": st.st_size})
    except OSError:
        pass
    out.sort(key=lambda f: f["name"])
    return out[:SD_LIST_MAX]


def sd_put(dfd, name, b64):
    """Write one uploaded file onto the card; returns an error text, or None when it is stored."""
    try:
        return _sd_put(dfd, name, b64)
    except (OSError, UnicodeError) as e:
        return "the card refused it (%s)" % (getattr(e, "strerror", None) or "bad name")


def _sd_put(dfd, name, b64):
    if not sd_name_ok(name):
        return "that file name is not allowed (no / or .., at most 64 characters)"
    if not isinstance(b64, str) or len(b64) > SD_FILE_MAX * 4 // 3 + 8:
        return "the file is bigger than 700 KB"
    try:
        data = base64.b64decode(b64, validate=True)
    except ValueError:
        return "the file did not arrive whole"
    if len(data) > SD_FILE_MAX:
        return "the file is bigger than 700 KB"
    files = sd_files(dfd)
    old = next((f["size"] for f in files if f["name"] == name), 0)
    if sum(f["size"] for f in files) - old + len(data) > SD_TOTAL_MAX:
        return "the card would hold more than 16 MB"
    try:
        st = os.stat(name, dir_fd=dfd, follow_symlinks=False)
        if not stat.S_ISREG(st.st_mode):
            return "that name is taken by something that is not a file"
    except FileNotFoundError:
        pass
    tmp = SD_TMP + uuid.uuid4().hex
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o666, dir_fd=dfd)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.rename(tmp, name, src_dir_fd=dfd, dst_dir_fd=dfd)
    except BaseException:
        try:
            os.unlink(tmp, dir_fd=dfd)
        except OSError:
            pass
        raise
    return None


def sd_read(dfd, name):
    """(bytes, None) of one file on the card, or (None, error text)."""
    if not sd_name_ok(name):
        return None, "that file name is not allowed"
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=dfd)
        with os.fdopen(fd, "rb") as f:
            st = os.fstat(f.fileno())
            if not stat.S_ISREG(st.st_mode):
                return None, "no such file"
            if st.st_size > SD_FILE_MAX:
                return None, "the file is bigger than 700 KB"
            return f.read(SD_FILE_MAX + 1), None
    except (OSError, UnicodeError):
        return None, "no such file"


# ---- Caps on the sim's /tmp and /data ----
#
# /tmp and /data are size-limited tmpfs mounts inside the sandbox, so whatever an app writes stops at the cap.
# /data starts as a copy of the app's saved data (NVS, high scores, storage/) and is copied back when the
# session ends, only if it is small. bwrap 0.8 cannot cap inodes, so the copy-back caps the file count.
# Saved data is never trusted: both copies go through tree_copy, which uses lstat/O_NOFOLLOW directory fds
# and copies only regular files and folders. Copy-back reads the sandbox's /data through a directory fd opened
# once at start (via /proc/<pid>/root, like the card); the fd keeps the tmpfs readable after the app has died.

TMP_MOUNT_ARGS = ["--size", str(32 * 1024 * 1024), "--tmpfs", "/tmp"]
DATA_MOUNT_ARGS = ["--size", str(16 * 1024 * 1024), "--tmpfs", "/data"]
SAVE_MAX_BYTES = 1024 * 1024
SAVE_MAX_FILES = 200
SAVE_MAX_DEPTH = 8


class TreeTooBig(Exception):
    pass


def _tree_copy(src_fd, dest, state, depth, skip=()):
    if depth > SAVE_MAX_DEPTH:
        raise TreeTooBig("too deep")
    dest.mkdir(exist_ok=True)
    for name in os.listdir(src_fd):
        if name in skip or name in (".", ".."):
            continue
        try:
            st = os.stat(name, dir_fd=src_fd, follow_symlinks=False)
        except OSError:
            continue
        if stat.S_ISDIR(st.st_mode):
            state[0] += 1
            if state[0] > SAVE_MAX_FILES:
                raise TreeTooBig("more than %d files" % SAVE_MAX_FILES)
            try:
                sub = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=src_fd)
            except OSError:
                continue
            try:
                _tree_copy(sub, dest / name, state, depth + 1)
            finally:
                os.close(sub)
        elif stat.S_ISREG(st.st_mode):
            state[0] += 1
            if state[0] > SAVE_MAX_FILES:
                raise TreeTooBig("more than %d files" % SAVE_MAX_FILES)
            try:
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=src_fd)
            except OSError:
                continue
            try:
                if not stat.S_ISREG(os.fstat(fd).st_mode):
                    continue
                with open(dest / name, "wb") as f:
                    while True:
                        data = os.read(fd, 65536)
                        if not data:
                            break
                        state[1] += len(data)
                        if state[1] > SAVE_MAX_BYTES:
                            raise TreeTooBig("more than %d bytes" % SAVE_MAX_BYTES)
                        f.write(data)
            finally:
                os.close(fd)
        # anything else (links, devices, sockets, fifos) is skipped


def tree_copy(src_fd, dest, skip=()):
    """Copy the regular files and folders below the directory fd to @p dest (<=1 MB, <=200 files); raises TreeTooBig."""
    _tree_copy(src_fd, Path(dest), [0, 0], 0, skip)


def data_seed(saved, dest):
    """Stage the app's saved data into @p dest for the new sandbox. Empty (and logged) if it is unusable."""
    dest = Path(dest)
    dest.mkdir(exist_ok=True)
    try:
        fd = os.open(saved, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return
    try:
        tree_copy(fd, dest, skip=("sd",))
    except (TreeTooBig, OSError) as e:
        log.warning("data: saved data of %s not loaded: %s", saved, e)
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir()
    finally:
        os.close(fd)


def data_open(pid):
    """A directory fd on the sandbox's /data, or None."""
    try:
        return os.open(f"/proc/{pid}/root/data", os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return None


def data_save(fd, saved):
    """Copy the sandbox's /data (not the card) over the saved data; keep the old one and log why if it is too big."""
    saved = Path(saved)
    new = saved.with_name(saved.name + ".new")
    old = saved.with_name(saved.name + ".old")
    shutil.rmtree(new, ignore_errors=True)
    try:
        tree_copy(fd, new, skip=("sd",))
    except (TreeTooBig, OSError) as e:
        log.warning("data: not saved for %s, the old data stays: %s", saved.name, e)
        shutil.rmtree(new, ignore_errors=True)
        return False
    shutil.rmtree(old, ignore_errors=True)
    try:
        if saved.exists():
            saved.rename(old)
        new.rename(saved)
    except OSError as e:
        log.warning("data: not saved for %s: %s", saved.name, e)
        return False
    finally:
        shutil.rmtree(old, ignore_errors=True)
    return True


def sim_cmd(elf, run_dir, seed):
    """The sandbox command line of a sim. /data starts as a copy of @p seed, which is mounted read-only at /seed."""
    return [
        "bwrap", "--ro-bind", "/usr", "/usr", "--ro-bind", "/lib", "/lib",
        "--ro-bind-try", "/lib64", "/lib64", "--ro-bind-try", "/lib32", "/lib32",
        "--ro-bind-try", "/bin", "/bin",
        "--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache",
        "--ro-bind", str(elf), "/app.elf",
        "--bind", str(run_dir), "/run/sim",
        "--ro-bind", str(seed), "/seed",
        *DATA_MOUNT_ARGS,
        *SD_MOUNT_ARGS,
        "--dev", "/dev", "--proc", "/proc", *TMP_MOUNT_ARGS,
        "--unshare-all", "--die-with-parent", "--new-session",
        "--setenv", "KODE_SIM_SOCK", "/run/sim/sim.sock",
        "--setenv", "KODE_SIM_DATA", "/data",
        "--chdir", "/data",
        "/bin/sh", "-c", "cp -R /seed/. /data/ 2>/dev/null; exec /app.elf",
    ]


class Session:
    """One browser tab, one sim process."""

    def __init__(self, ws, app_id, role="dot", user="", sender=None):
        self.ws = ws
        self.sender = sender   # set on the KD socket (/ws), None on the JSON one (/ws-legacy)
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
        self.buttons = 0
        self.sd_fd = None          # the card's directory, once the sim has started (sd_open)
        self.sd_lock = asyncio.Lock()
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
        await asyncio.get_running_loop().run_in_executor(None, data_seed, data, seed)
        cmd = sim_cmd(elf, self.tmp, seed)
        self.proc = await asyncio.create_subprocess_exec(
            "prlimit", "--as=1500000000", "--cpu=3600", "--", *cmd,
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
        kid = sd_child_pid(self.proc.pid)
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
                if self.sender and typ not in "FNX":
                    await self.send_kd(typ, body)
                elif typ == "F":
                    w, h = struct.unpack("<HH", body[:4])
                    self.frame = (w, h, body[4:])
                    self.frame_evt.set()
                    if self.role == "watch":
                        await self.send_mirror(body[4:], w, h)
                elif typ == "E":
                    await self.send_json(t="led", r=body[0], g=body[1], b=body[2])
                elif typ == "B":
                    await self.send_json(t="bright", v=body[0])
                elif typ == "H":
                    await self.send_json(t="haptic", ms=struct.unpack("<I", body)[0])
                elif typ == "T":
                    hz, ms = struct.unpack("<II", body[:8])
                    await self.send_json(t="tone", hz=hz, ms=ms, vol=body[8])
                elif typ == "P":
                    if not self.ws.closed:
                        await self.ws.send_bytes(b"P" + body)
                elif typ == "L":
                    await self.send_json(t="log", s=body.decode(errors="replace"))
                elif typ == "N":
                    await self.on_link(body.decode(errors="replace").replace("\n", " "))
                elif typ == "X":
                    await self.send_json(t="exit", why=body.decode(errors="replace"))
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
            if self.sender:
                self.sender.put(kdwire.encode(kdwire.FB_JPEG, struct.pack("<4H", 0, 0, w, h) + jpg))
            else:
                await self.ws.send_bytes(b"J" + jpg)

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
        t = msg.get("t")
        if "console_in" in msg:
            text = str(msg["console_in"]).strip()
            if text.startswith("?") and text[1:].strip() in PROBES:
                self.send_line("? " + text[1:].strip())
                return
            for line in kdwire.kd_to_sim_lines(kdwire.CONSOLE_IN, str(msg["console_in"]).encode()):
                self.send_line(line)
        elif t == "btn":
            bit = BTN_BITS.get(msg.get("b"))
            if bit:
                self.buttons = (self.buttons | bit) if msg.get("d") else (self.buttons & ~bit)
                self.send_line(f"b {self.buttons}")
        elif t == "touch":
            down = 0 if msg.get("k") == "up" else 1
            x = int(msg.get("x", 0))
            y = int(msg.get("y", 0))
            self.send_line(f"t {down} {x} {y}")
        elif "sd" in msg:
            v = msg["sd"]
            self.send_line("s " + ("1" if (v.get("in") if isinstance(v, dict) else v) else "0"))
        elif "sd_ls" in msg:
            asyncio.ensure_future(self.sd_job("ls", {}))
        elif "sd_put" in msg and isinstance(msg["sd_put"], dict):
            asyncio.ensure_future(self.sd_job("put", msg["sd_put"]))
        elif "sd_get" in msg and isinstance(msg["sd_get"], dict):
            asyncio.ensure_future(self.sd_job("get", msg["sd_get"]))
        elif t == "dock":
            self.send_line(f"n sys dock {1 if msg.get('v') else 0}")
        elif t == "notify":
            text = str(msg.get("s", ""))[:200].replace("\n", " ")
            target = self if self.role == "watch" else self.peer
            if text and target:
                target.send_line("n notify " + text)

    async def stop(self):
        self.leave_hub()
        self.closed = True
        if self.sd_fd is not None:
            try:
                os.close(self.sd_fd)
            except OSError:
                pass
            self.sd_fd = None
        if self.data_fd is not None:
            fd, self.data_fd = self.data_fd, None
            try:
                await asyncio.get_running_loop().run_in_executor(None, data_save, fd, DATA / self.app_id)
            except Exception:
                log.exception("data: copy-back failed")
            finally:
                os.close(fd)
        if self.sender:
            await self.sender.stop()
        self.frame_evt.set()
        if self.writer:
            try:
                self.send_line("q")
                self.writer.close()
            except Exception:
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
        if self.tmp:
            shutil.rmtree(self.tmp, ignore_errors=True)


# ---- build from Git ------------------------------------------------------------

WORK = Path(os.environ.get("KODE_WORK", "/opt/kode/work"))
SIM = Path(os.environ.get("KODE_SIM", "/opt/kode/sim"))
SDK = Path(os.environ.get("KODE_SDK", "/opt/kode/src/kode-sdk"))
REPO_RE = re.compile(r"^https://(github\.com|gitlab\.com|codeberg\.org)/[\w.-]+/[\w.-]+?(\.git)?/?$")
LOCAL_APPS = Path(os.environ.get("KODE_LOCAL_APPS", "/opt/kode/apps"))
build_lock = asyncio.Lock()
jobs = {}


def app_id_for(repo):
    if repo.startswith("local:"):
        return repo[6:]
    name = repo.rstrip("/").rsplit("/", 1)[-1]
    name = re.sub(r"\.git$", "", name)
    name = re.sub(r"^kode-app-", "", name)
    name = re.sub(r"[^a-z0-9_]", "_", name.lower())[:31].strip("_")
    return name or "app"


class Job:
    def __init__(self, repo, ref, app=None, code=None, guest=False, client=""):
        self.id = uuid.uuid4().hex[:10]
        self.repo = repo
        self.ref = ref
        self.app = app or app_id_for(repo)
        self.code = code          # edited app_main.c (repo "edit:<base>")
        self.guest = guest
        self.client = client
        self.state = "waiting"
        self.lines = []
        self.started = time.time()

    def add(self, text):
        for line in text.splitlines():
            self.lines.append(line)
        if len(self.lines) > 3000:
            self.lines = self.lines[-3000:]


async def run_logged(job, cmd, timeout):
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdin=asyncio.subprocess.DEVNULL,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    try:
        async def pump():
            while True:
                line = await proc.stdout.readline()
                if not line:
                    break
                job.add(line.decode(errors="replace").rstrip("\n"))
        await asyncio.wait_for(pump(), timeout)
        return await proc.wait()
    except asyncio.TimeoutError:
        proc.kill()
        job.add(f"!! took too long (> {timeout} s), stopped")
        return -1


async def build_job(job):
    async with build_lock:
        job.state = "building"
        app_dir = WORK / "app"
        out = WORK / "build"
        try:
            shutil.rmtree(app_dir, ignore_errors=True)
            if job.repo.startswith("edit:"):
                src = LOCAL_APPS / job.repo[5:]
                job.add("== your code, on the " + job.repo[5:] + " app skeleton")
                shutil.copytree(src, app_dir, copy_function=shutil.copy)
                (app_dir / "main" / "app_main.c").write_text(job.code)
            elif job.repo.startswith("local:"):
                src = LOCAL_APPS / job.repo[6:]
                job.add(f"== local app {job.repo[6:]}")
                shutil.copytree(src, app_dir, copy_function=shutil.copy)
            else:
                job.add(f"== fetching {job.repo}" + (f" ({job.ref})" if job.ref else ""))
                cmd = ["git", "clone", "--depth", "1", "--recurse-submodules", "--shallow-submodules"]
                if job.ref:
                    cmd += ["--branch", job.ref]
                env_cmd = ["env", "GIT_TERMINAL_PROMPT=0"] + cmd + [job.repo, str(app_dir)]
                rc = await run_logged(job, env_cmd, 180)
                if rc != 0:
                    job.state = "failed"
                    job.add("!! git clone failed (private repo? wrong branch?)")
                    return
            (WORK / "home").mkdir(exist_ok=True)
            sandbox = [
                "bwrap", "--ro-bind", "/usr", "/usr",
                "--symlink", "usr/lib", "/lib", "--symlink", "usr/lib32", "/lib32",
                "--symlink", "usr/lib64", "/lib64", "--symlink", "usr/bin", "/bin",
                "--symlink", "usr/sbin", "/sbin", "--ro-bind", "/etc", "/etc",
                "--ro-bind", "/opt/esp", "/opt/esp",
                "--ro-bind", str(SIM), str(SIM), "--ro-bind", str(SDK), str(SDK),
                "--bind", str(WORK), str(WORK),
                "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
                "--unshare-all", "--die-with-parent", "--new-session",
                "--setenv", "HOME", str(WORK / "home"),
                str(SIM / "build.sh"), str(app_dir), str(out),
            ]
            rc = await run_logged(job, ["prlimit", "--as=4000000000", "--cpu=300", "--"] + sandbox, 180 if job.guest else 900)
            elf = out / "kode_sim_app.elf"
            if rc != 0 or not elf.exists():
                job.state = "failed"
                return
            dest = BUILDS / job.app
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copy2(elf, dest / "kode_sim_app.elf.new")
            os.replace(dest / "kode_sim_app.elf.new", dest / "kode_sim_app.elf")
            name = job.app.replace("_", " ").title()
            m = re.search(r'KODE_APP_MANIFEST\(\s*"([^"]+)"', "".join(
                p.read_text(errors="replace") for p in (app_dir / "main").glob("*.c")))
            if m:
                name = m.group(1)
            icon = app_dir / "appdata" / "icon.png"
            if icon.exists() and icon.stat().st_size < 2_000_000:
                shutil.copy(icon, dest / "icon.png")
            (dest / "kode-sim.json").write_text(json.dumps(
                {"name": name, "repo": job.repo, "ref": job.ref, "guest": job.guest}))
            job.state = "done"
            job.add(f"== {name} is ready")
        except Exception as e:  # noqa: BLE001
            log.exception("build")
            job.state = "failed"
            job.add(f"!! {e}")


async def api_build(request):
    body = await request.json()
    repo = str(body.get("repo", "")).strip()
    ref = str(body.get("ref", "")).strip()
    local = str(body.get("local", "")).strip()
    if local:
        if not re.match(r"^[a-z0-9_]{1,31}$", local) or not (LOCAL_APPS / local / "main").is_dir():
            return web.json_response({"error": "unknown local app"}, status=400)
        repo = "local:" + local
    elif not REPO_RE.match(repo):
        return web.json_response({"error": "only https URLs from GitHub, GitLab or Codeberg"}, status=400)
    if ref and not re.match(r"^[\w./-]{1,100}$", ref):
        return web.json_response({"error": "invalid branch/tag"}, status=400)
    job = Job(repo, ref)
    jobs[job.id] = job
    for old in sorted(jobs.values(), key=lambda j: j.started)[:-20]:
        if old.state not in ("waiting", "building"):
            jobs.pop(old.id, None)
    asyncio.create_task(build_job(job))
    return web.json_response({"job": job.id, "app": job.app})


async def api_job(request):
    job = jobs.get(request.match_info["id"])
    if not job or not (job.guest or request.headers.get("Remote-User")):
        raise web.HTTPNotFound()
    since = int(request.query.get("since", "0"))
    return web.json_response({"state": job.state, "app": job.app,
                              "lines": job.lines[since:], "next": len(job.lines)})


# ---- edit the code in the browser (public) ----------------------------------

EDIT_PREFIX = "g_"
PRIVATE_APPS = set()   # app ids that only a logged-in user (Remote-User) may see and start
MAX_CODE = 64 * 1024
MAX_GUEST_QUEUE = 4
GUEST_KEEP_S = 6 * 3600
STARTER = r"""/* My first Kode app.
 * Edit this code and press "Build & run" (Ctrl+Enter).
 * The drawing kit is kodekit: see the kk.h tab for every function.
 */
#include <stdio.h>

#include "kode.h"
#include "kk.h"

KODE_APP_MANIFEST("My App", "games");

void app_main(void)
{
    kk_init();
    int x = KK_W / 2, y = KK_H / 2, presses = 0;
    for ( ;; )
    {
        kk_begin();                       /* read the buttons, start a frame */

        uint32_t const h = kk_held();     /* move with the d-pad */
        if ( h & KK_LEFT )  { x -= 4; }
        if ( h & KK_RIGHT ) { x += 4; }
        if ( h & KK_UP )    { y -= 4; }
        if ( h & KK_DOWN )  { y += 4; }

        int tx, ty;                       /* or tap the glass */
        if ( kk_tapped(&tx, &ty) ) { x = tx; y = ty; }

        if ( kk_pressed(KK_TOP) ) { presses++; kk_tone(880, 60); kk_led(0, 40, 80); }
        if ( kk_pressed(KK_BOT) ) { kk_led(0, 0, 0); kk_buzz(40); }

        if ( x < 60 ) { x = 60; }
        if ( x > KK_W - 60 ) { x = KK_W - 60; }
        if ( y < 140 ) { y = 140; }
        if ( y > KK_H - 70 ) { y = KK_H - 70; }

        kk_vgrad(0, 0, KK_W, KK_H, KK_RGB(20, 24, 32), KK_RGB(6, 8, 12));
        kk_text_c(56, "HELLO KODE", 3, KK_WHITE);
        char b[32];
        (void)snprintf(b, sizeof(b), "TOP PRESSED %d", presses);
        kk_text_c(96, b, 1, KK_GREY);
        kk_disc(x, y, 26, KK_ORANGE);
        kk_text_c(462, "D-PAD OR TAP TO MOVE", 1, KK_GREY);

        kk_end();                         /* show the frame */
    }
}
"""


def editable_apps():
    out = []
    for d in sorted(LOCAL_APPS.glob("*/main/app_main.c")):
        app = d.parent.parent.name
        if app in PRIVATE_APPS:
            continue
        name = app
        m = re.search(r'KODE_APP_MANIFEST\(\s*"([^"]+)"', d.read_text(errors="replace"))
        if m:
            name = m.group(1)
        out.append({"id": app, "name": name})
    return out


def client_of(request):
    return (request.headers.get("Cf-Connecting-Ip")
            or request.headers.get("X-Forwarded-For", "").split(",")[0].strip()
            or (request.remote or ""))


def clean_guest_builds():
    cutoff = time.time() - GUEST_KEEP_S
    for root in (BUILDS, DATA):
        for d in root.glob(EDIT_PREFIX + "*"):
            try:
                if d.stat().st_mtime < cutoff and not any(s.app_id == d.name for s in sessions):
                    shutil.rmtree(d, ignore_errors=True)
            except OSError:
                pass


async def api_icon(request):
    app = request.match_info["id"]
    if not re.match(r"^[a-z0-9_]{1,31}$", app):
        raise web.HTTPNotFound()
    for f in (BUILDS / app / "icon.png", LOCAL_APPS / app / "appdata" / "icon.png"):
        if f.exists():
            return web.FileResponse(f, headers={"Cache-Control": "max-age=3600"})
    raise web.HTTPNotFound()


async def api_sources(request):
    return web.json_response({"apps": editable_apps()})


async def api_source(request):
    app = request.match_info["id"]
    kk_h = (LOCAL_APPS / "slang" / "main" / "kk.h").read_text(errors="replace")
    if app == "new":
        return web.json_response({"id": "new", "name": "My App", "code": STARTER, "kk_h": kk_h})
    if app in PRIVATE_APPS or not re.match(r"^[a-z0-9_]{1,31}$", app):
        raise web.HTTPNotFound()
    f = LOCAL_APPS / app / "main" / "app_main.c"
    if not f.exists():
        raise web.HTTPNotFound()
    return web.json_response({"id": app, "name": app, "code": f.read_text(errors="replace"), "kk_h": kk_h})


async def api_play(request):
    """Build the code from the editor on a local app's skeleton. Anyone may do this."""
    try:
        body = await request.json()
    except ValueError:
        body = {}
    base = str(body.get("base", "new"))
    code = body.get("code", "")
    if not isinstance(code, str) or not code.strip():
        return web.json_response({"error": "no code"}, status=400)
    if len(code.encode()) > MAX_CODE:
        return web.json_response({"error": "code too long (max 64 KB)"}, status=400)
    if base == "new" or base in PRIVATE_APPS or not (LOCAL_APPS / base / "main").is_dir() \
            or not re.match(r"^[a-z0-9_]{1,31}$", base):
        base = "slang"
    app = EDIT_PREFIX + hashlib.sha1((base + "\0" + code).encode()).hexdigest()[:12]
    if (BUILDS / app / "kode_sim_app.elf").exists():
        os.utime(BUILDS / app)
        return web.json_response({"job": None, "app": app, "cached": True})
    user = request.headers.get("Remote-User", "")
    client = user or client_of(request)
    busy = [j for j in jobs.values() if j.state in ("waiting", "building")]
    if any(j.client == client for j in busy):
        return web.json_response({"error": "you already have a build running"}, status=429)
    if not user and sum(1 for j in busy if j.guest) >= MAX_GUEST_QUEUE:
        return web.json_response({"error": "the build queue is full, try again in a minute"}, status=429)
    clean_guest_builds()
    job = Job("edit:" + base, "", app=app, code=code, guest=not user, client=client)
    jobs[job.id] = job
    for old in sorted(jobs.values(), key=lambda j: j.started)[:-40]:
        if old.state not in ("waiting", "building"):
            jobs.pop(old.id, None)
    asyncio.create_task(build_job(job))
    return web.json_response({"job": job.id, "app": app})


async def index(request):
    return web.FileResponse(KSWEB / "index.html")


def ksweb_file(name):
    async def handler(request):
        return web.FileResponse(KSWEB / name)
    return handler


async def docs_page(request):
    return web.FileResponse(STATIC / "docs.html")


async def api_notify(request):
    """A notification for every open watch session: POST {"text": "..."}."""
    try:
        body = await request.json()
    except ValueError:
        body = {}
    text = str(body.get("text", "")).strip()[:200].replace("\n", " ")
    if not text:
        return web.json_response({"error": "no text"}, status=400)
    sent = 0
    for slot in hub.values():
        w = slot.get("watch")
        if w and not w.closed:
            w.send_line("n notify " + text)
            sent += 1
    return web.json_response({"sent": sent})


async def status(request):
    return web.json_response({
        "user": request.headers.get("Remote-User", ""),
        "version": VERSION,
        "sessions": len(sessions),
        "apps": [a for a in app_list()
                 if request.headers.get("Remote-User") or a["id"] not in PRIVATE_APPS],
    })


async def ws_legacy_handler(request):
    """The JSON socket of the old page: 'J'+jpeg, 'P'+pcm and JSON text."""
    return await serve_ws(request, kd=False)


async def ws_kd_handler(request):
    """kode-sim's page: the session over the KD wire, binary frames both ways."""
    return await serve_ws(request, kd=True)


async def serve_ws(request, kd):
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=1 << 20, compress=False)
    await ws.prepare(request)
    app_id = request.query.get("app", "")
    if not app_id or "/" in app_id or app_id.startswith("."):
        await ws.send_str(json.dumps({"t": "error", "s": "no app chosen"}))
        await ws.close()
        return ws
    if app_id in PRIVATE_APPS and not request.headers.get("Remote-User"):
        await ws.send_str(json.dumps({"t": "error", "s": "this app needs a login"}))
        await ws.close()
        return ws
    if len(sessions) >= MAX_SESSIONS:
        await ws.send_str(json.dumps({"t": "error", "s": "too many sims running right now, try again in a minute"}))
        await ws.close()
        return ws
    role = request.query.get("role", "dot")
    role = role if role in ("dot", "watch") else "dot"
    # Guests (no Remote-User from the proxy) each get a hub of their own, so they never see each other's Link.
    sender = FrameSender(ws) if kd else None
    s = Session(ws, app_id, role, request.headers.get("Remote-User", "") or f"anon-{id(ws):x}", sender)
    sessions.add(s)
    tasks = []
    try:
        await s.start()
        s.join_hub()
        await s.send_json(t="started", app=app_id, role=role)
        if sender:
            sender.start()
        tasks = [asyncio.create_task(s.pump_sim()), asyncio.create_task(s.pump_frames())]
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    s.on_browser(json.loads(msg.data))
                except (ValueError, TypeError):
                    pass
            elif msg.type == WSMsgType.BINARY and kd:
                s.on_kd(msg.data)
            elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                break
    except Exception as e:  # noqa: BLE001 - the browser gets the reason
        log.exception("sessie %s", app_id)
        if not ws.closed:
            await ws.send_str(json.dumps({"t": "error", "s": str(e)}))
    finally:
        await s.stop()
        for t in tasks:
            t.cancel()
        sessions.discard(s)
        if not ws.closed:
            await ws.close()
    return ws


def make_app():
    app = web.Application(middlewares=[no_cache])
    app.router.add_get("/", index)
    app.router.add_get("/api/status", status)
    app.router.add_get("/docs", docs_page)
    app.router.add_post("/api/notify", api_notify)
    app.router.add_get("/ws", ws_kd_handler)
    app.router.add_get("/ws-legacy", ws_legacy_handler)
    app.router.add_post("/api/build", api_build)
    app.router.add_get("/api/build/{id}", api_job)
    app.router.add_get("/api/sources", api_sources)
    app.router.add_get("/api/icon/{id}", api_icon)
    app.router.add_get("/api/source/{id}", api_source)
    app.router.add_post("/api/play", api_play)
    for name in KSWEB_FILES:
        app.router.add_get("/" + name, ksweb_file(name))
    app.router.add_static("/dotsim/", STATIC)
    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    web.run_app(make_app(), host="0.0.0.0", port=PORT, access_log=None)
