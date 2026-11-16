"""Dot Sim: an unofficial Kode Dot in the browser, served from one machine.

Every WebSocket session starts its own sim process (a Kode app built for
ESP-IDF's linux target on the real kode-sdk, with the board simulated under
it, see ../hostsim), in a bubblewrap sandbox without network. The process
talks to this server over a unix socket (hostsim/components/khal/src/kdotsim_bridge.c):

  sim -> server: [u8 type][u32 len LE][payload]
      F frame (u16 w, u16 h, RGB565), E led rgb, B brightness, H haptic ms,
      T tone (u32 hz, u32 ms, u8 vol), P pcm (u32 rate, s16...), L log, X exit
      N link message (text) for the paired device
  server -> sim: text lines "b <mask>", "t <0|1> <x> <y>", "q",
      "n <text>" (link message), "m <w> <h> <base64>" (mirror picture)

Kode Link: two sessions of the same user (a "dot" and a "watch" role) are
paired through the hub below. On hardware this would be BLE; the apps see the
same thing: text messages both ways, plus "hub peer 0|1" and "sys ..." from
the hub itself.

To the browser: binary 'J'+jpeg or 'P'+rate(u32)+pcm, and JSON text for the rest.

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
import hashlib
import uuid
import struct
import tempfile
import time
from pathlib import Path

import numpy as np
from aiohttp import web, WSMsgType
from PIL import Image

ROOT = Path(__file__).parent
STATIC = Path(os.environ.get("KODE_STATIC", ROOT.parent / "web"))
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
    px = np.frombuffer(buf, dtype="<u2", count=w * h).reshape(h, w)
    rgb = np.empty((h, w, 3), dtype=np.uint8)
    rgb[..., 0] = ((px >> 11) & 0x1F) * 255 // 31
    rgb[..., 1] = ((px >> 5) & 0x3F) * 255 // 63
    rgb[..., 2] = (px & 0x1F) * 255 // 31
    bio = io.BytesIO()
    Image.fromarray(rgb, "RGB").save(bio, "JPEG", quality=JPEG_Q)
    return bio.getvalue()


class Session:
    """One browser tab, one sim process."""

    def __init__(self, ws, app_id, role="dot", user=""):
        self.ws = ws
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
        cmd = [
            "bwrap", "--ro-bind", "/usr", "/usr", "--ro-bind", "/lib", "/lib",
            "--ro-bind-try", "/lib64", "/lib64", "--ro-bind-try", "/lib32", "/lib32",
            "--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache",
            "--ro-bind", str(elf), "/app.elf",
            "--bind", str(self.tmp), "/run/sim",
            "--bind", str(data), "/data",
            "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
            "--unshare-all", "--die-with-parent", "--new-session",
            "--setenv", "KODE_SIM_SOCK", "/run/sim/sim.sock",
            "--setenv", "KODE_SIM_DATA", "/data",
            "--chdir", "/data",
            "/app.elf",
        ]
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
            await self.ws.send_bytes(b"J" + jpg)

    def send_line(self, line):
        if self.writer and not self.writer.is_closing():
            self.writer.write((line + "\n").encode())

    def on_browser(self, msg):
        t = msg.get("t")
        if t == "btn":
            bit = BTN_BITS.get(msg.get("b"))
            if bit:
                self.buttons = (self.buttons | bit) if msg.get("d") else (self.buttons & ~bit)
                self.send_line(f"b {self.buttons}")
        elif t == "touch":
            down = 0 if msg.get("k") == "up" else 1
            x = int(msg.get("x", 0))
            y = int(msg.get("y", 0))
            self.send_line(f"t {down} {x} {y}")
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
    return web.FileResponse(STATIC / "index.html")


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
        "sessions": len(sessions),
        "apps": [a for a in app_list()
                 if request.headers.get("Remote-User") or a["id"] not in PRIVATE_APPS],
    })


async def ws_handler(request):
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
    s = Session(ws, app_id, role, request.headers.get("Remote-User", "") or f"anon-{id(ws):x}")
    sessions.add(s)
    tasks = []
    try:
        await s.start()
        s.join_hub()
        await s.send_json(t="started", app=app_id, role=role)
        tasks = [asyncio.create_task(s.pump_sim()), asyncio.create_task(s.pump_frames())]
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    s.on_browser(json.loads(msg.data))
                except (ValueError, TypeError):
                    pass
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
    app.router.add_get("/ws", ws_handler)
    app.router.add_post("/api/build", api_build)
    app.router.add_get("/api/build/{id}", api_job)
    app.router.add_get("/api/sources", api_sources)
    app.router.add_get("/api/icon/{id}", api_icon)
    app.router.add_get("/api/source/{id}", api_source)
    app.router.add_post("/api/play", api_play)
    app.router.add_static("/static/", STATIC)
    return app


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    web.run_app(make_app(), host="0.0.0.0", port=PORT, access_log=None)
