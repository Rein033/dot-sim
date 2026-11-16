"""Dot Sim: an unofficial Kode Dot in the browser, served from one machine.

This is the aiohttp app: kode-sim's page (web/) at /, our layer on it (dotsim/web) at /dotsim/, the docs
at /docs, the JSON API under /api/, and the sim's WebSocket at /ws. Every WebSocket gets its own sim
process (a Kode app built for ESP-IDF's linux target on the real kode-sdk, with the board simulated under
it, see ../hostsim) in a sandbox without network.

  config.py   the KODE_* settings
  session.py  Session: one sim per WebSocket, the sim's socket protocol, the Kode Link hub
  sandbox.py  the sim's and the build's sandbox command lines, the sim's saved /data
  sdcard.py   the microSD files the page uploads and downloads
  builds.py   builds from Git and from the editor
  kdwire.py   the KD wire to and from kode-sim's page

Run it as `python3 server.py` from this folder (deploy/dotsim.service).

Not made, approved or supported by Kode.
"""
import asyncio
import hashlib
import json
import logging
import os
import re
import subprocess

from aiohttp import web, WSMsgType

from builds import (MAX_CODE, MAX_GUEST_QUEUE, REPO_RE, STARTER, Job, build_job, clean_guest_builds,
                    editable_apps, jobs)
from config import BUILDS, EDIT_PREFIX, KSWEB, LOCAL_APPS, MAX_SESSIONS, PORT, PRIVATE_APPS, ROOT, STATIC
from session import Session, hub, sessions

KSWEB_FILES = ("sim.js", "audio-worklet.js", "kode-dot.png", "kode-dot.json")

log = logging.getLogger("kode")


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


def client_of(request):
    return (request.headers.get("Cf-Connecting-Ip")
            or request.headers.get("X-Forwarded-For", "").split(",")[0].strip()
            or (request.remote or ""))


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


async def ws_kd_handler(request):
    """kode-sim's page: the session over the KD wire, binary frames both ways."""
    return await serve_ws(request)


async def serve_ws(request):
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
        s.sender.start()
        tasks = [asyncio.create_task(s.pump_sim()), asyncio.create_task(s.pump_frames())]
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    s.on_browser(json.loads(msg.data))
                except (ValueError, TypeError):
                    pass
            elif msg.type == WSMsgType.BINARY:
                s.on_kd(msg.data)
            elif msg.type in (WSMsgType.ERROR, WSMsgType.CLOSE):
                break
    except Exception as e:  # noqa: BLE001 - the browser gets the reason
        log.exception("session %s", app_id)
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
