"""Builds: an app from Git (logged in), a local app, or code edited in the browser (anyone).

One build at a time (build_lock), each in its own sandbox (sandbox.build_cmd). The ELF goes to
BUILDS/<app>/kode_sim_app.elf with its name, repo and icon next to it. Editor builds are cached by a
hash of their code (app id EDIT_PREFIX + hash) and deleted after GUEST_KEEP_S unless a session runs them.
"""
import asyncio
import json
import logging
import os
import re
import shutil
import time
import uuid

from config import BUILDS, DATA, EDIT_PREFIX, LOCAL_APPS, PRIVATE_APPS, WORK
from sandbox import BUILD_PRLIMIT, build_cmd
from session import sessions

log = logging.getLogger("kode")

REPO_RE = re.compile(r"^https://(github\.com|gitlab\.com|codeberg\.org)/[\w.-]+/[\w.-]+?(\.git)?/?$")
build_lock = asyncio.Lock()
jobs = {}                  # job id -> Job (the newest few, see api_build and api_play)
MAX_CODE = 64 * 1024       # one editor build's app_main.c
MAX_GUEST_QUEUE = 4        # guest builds waiting or running at once
GUEST_KEEP_S = 6 * 3600    # an editor build (and its saved data) is deleted after this, unless it runs


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
            rc = await run_logged(job, BUILD_PRLIMIT + build_cmd(app_dir, out), 180 if job.guest else 900)
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


def clean_guest_builds():
    cutoff = time.time() - GUEST_KEEP_S
    for root in (BUILDS, DATA):
        for d in root.glob(EDIT_PREFIX + "*"):
            try:
                if d.stat().st_mtime < cutoff and not any(s.app_id == d.name for s in sessions):
                    shutil.rmtree(d, ignore_errors=True)
            except OSError:
                pass


# The code the editor opens for a new app (GET /api/source/new); it builds on the slang skeleton.
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
