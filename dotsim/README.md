# Dot Sim: the browser layer

`dotsim/` is what this fork adds to kode-sim: a Kode Dot that runs in any browser, served
from one Linux machine, for many visitors at once. It is the code behind the public Dot Sim.

Unofficial. Not made, approved or supported by Kode.

**Live:** [kode.uselesssite.nl](https://kode.uselesssite.nl) runs the latest `main` of this repository; a new commit is
live within about five minutes. The page's footer names the commit it runs.

> **Status: PAUSED on 2026-11-16.** Everything below works and is live. The open work is
> listed at the end ([Open work](#open-work)).

## What it is

- **kode-sim's page is the front.** `web/index.html` is kode-sim's own page, unchanged but
  for one line (`<script src="/dotsim/ext.js"></script> <!-- dotsim -->`). Everything Dot Sim
  adds happens at run time in `dotsim/web/`, so upstream merges of `web/` stay clean.
- **The fast engine (host-sim).** It is **not** kode-sim's emulator. kode-sim runs the real
  firmware instruction by instruction on esp-emu, on your own computer. Dot Sim builds an app
  for ESP-IDF's `linux` target on the real kode-sdk, with the board simulated under it
  (`hostsim/`), and runs it as a normal sandboxed process on the server: fast (an app runs at
  full speed on one small core) but not cycle-exact, and without kodeOS for now.
- **The KD wire.** The page and the server speak kode-sim's own KD frames
  (`crates/kode-sim/src/proto.rs`) on `/ws`, plus one type of ours: `FB_JPEG` (`0x13`:
  u16 x, y, w, h, then a JPEG) for the screen. Small JSON text messages carry the console,
  the microSD and the session's state. `server/kdwire.py` translates between the wire and the
  sim process's own records and lines (`hostsim/components/khal/src/kdotsim_bridge.c`).

## Layout

| Path | What it is |
|---|---|
| `hostsim/` | The ESP-IDF project that builds one app for the host: the SDK's own components, and sim versions of `khal`, `kdrv` and `kbsp` underneath (a virtual I2C bus with models of the board's chips, a frame buffer, audio, LED, haptic, NVS, microSD, battery, IMU). `build.sh <app> <out>` builds `kode_sim_app.elf`. |
| `server/server.py` | The aiohttp app: routes, HTTP handlers, the `/ws` WebSocket, `main`. Run as `python3 server.py`. |
| `server/config.py` | The `KODE_*` settings, read once at start. |
| `server/session.py` | `Session`: one sim process per WebSocket, the sim's socket protocol, frames to the page, the Kode Link hub. |
| `server/sandbox.py` | The sim's and the build's `bwrap`/`prlimit` command lines and caps; seeding and saving the sim's `/data`. |
| `server/sdcard.py` | The microSD: listing, upload and download through a directory fd on the sandbox's card. |
| `server/builds.py` | Builds from Git, from a local app and from the editor; jobs; guest build cleanup. |
| `server/kdwire.py` | The KD wire: encode, decode, and translation to and from the sim. |
| `web/` | Our layer on kode-sim's page: `ext.js` (the hook: wire, `window.DotSim`, keys, loader), `shell.js` (header, mode switch, footer), `home.js` (Dash and the app grid), `editor.js`, `parts.js` (the board's parts, today the microSD), `english.js` (kode-sim's Spanish to English), `theme/` (the look and Dash), `docs.html`. |
| `apps/` | Ten small apps on *kodekit* (a frame-painting kit on the public SDK), their pixel-art icons, and `assemble.py`, which turns each into a normal kode-app-template folder. |
| `tests/` | pytest: the wire, the sandbox caps, the microSD, the host-sim's inputs, and the page in a real browser (Playwright). `run-in-ct.sh` runs them. |
| `deploy/` | A systemd unit and a Caddy example. |

## What works

- **Apps:** the ten apps below, built once on the server and started from the home screen's grid.
- **Editor:** edit an app's `app_main.c` (or start a new one) in the browser, Build & run; compiler
  errors become red lines. Anyone may build; one build per visitor, at most four guest builds queued.
- **Beginner / Advanced:** Beginner shows the apps, the editor and the battery; Advanced also the
  motion card, the console and the microSD card (the editor is there, folded).
- **Inputs:** buttons, touch, battery level and USB power, motion (IMU) and the console, all through
  kode-sim's own controls; the sim reads them through the SDK's real drivers.
- **microSD:** put the card in and out; list, upload (700 KB a file) and download its files.
- **Kode Link:** two sessions of one user (`?role=dot` and `?role=watch`) are paired; apps exchange text.
- **Sandbox caps**, per sim:
  - no network, read-only system, `prlimit` 1.5 GB address space and 1 h CPU;
  - `/tmp` 32 MB, `/data` 16 MB, the card 16 MB (each a size-capped tmpfs);
  - the app's saved data is copied in at start and back at the end only if it is at most
    1 MB and 200 files (regular files and folders only; never through a link);
  - the run folder with the bridge's socket is read-only;
  - at most four sims at once (`KODE_MAX_SESSIONS`).

## Dash

Dash, the dog on the home screen (`web/theme/dash.js`), is Dot Sim's own mascot: drawn on a
canvas from plain shapes, with moods (idle, happy, sleep, clock, notify, charge, wake) and
eyes that follow the pointer. He is not Kody and not related to Kode.

## The apps

| id | Name | |
|---|---|---|
| `slang` | Snake | Eat apples, grow, don't bite yourself. |
| `muurbreker` | Wall Breaker | Bounce the ball and break every brick. |
| `blokkenregen` | Block Rain | Spin and stack falling blocks. |
| `schuif` | Slide | Slide tiles, add them up to 2048. |
| `fladder` | Flutter | Flap between the pillars. |
| `tafeltennis` | Table Tennis | Pong against the Dot: first to 7 wins. |
| `kleurgeheugen` | Color Memory | Remember the colors and tones. |
| `krabbel` | Doodle | Draw on the glass with your finger. |
| `stopwatch` | Stopwatch | Stopwatch with lap times. |
| `reflex` | Reflex | How fast do you react to green? |

```bash
cd dotsim/apps
python3 make_icons.py        # icons/<id>.png, 512x512 pixel art
python3 assemble.py          # <id>/: a full app folder for each, with appdata/icon.png
```

Each assembled folder is an ordinary kode-sdk app, so it also runs in kode-sim itself:

```bash
kode-sim run dotsim/apps/slang
```

## Run it

Tested on Debian 12 (x86-64). The sim is built 32-bit, because the SDK's event pools
assert their sizes for a 32-bit target.

1. ESP-IDF v6.1 (`IDF_PATH`, `IDF_TOOLS_PATH`; defaults `/opt/esp/esp-idf`, `/opt/esp/tools`).
2. `apt install bubblewrap gcc-multilib libbsd-dev:i386 ruby python3-aiohttp python3-pil python3-numpy`
3. kode-sdk next to it, and `hostsim/sdk` pointing at it:
   ```bash
   git clone https://github.com/kodediy/kode-sdk.git /opt/kode/src/kode-sdk
   ln -sfn /opt/kode/src/kode-sdk dotsim/hostsim/sdk
   ```
4. Build one app by hand to check the tool chain:
   ```bash
   dotsim/hostsim/build.sh dotsim/apps/slang /tmp/build-slang
   ```
5. Start the server (`deploy/dotsim.service` lists every `KODE_*` setting it reads):
   ```bash
   KODE_SIM=$PWD/dotsim/hostsim KODE_LOCAL_APPS=$PWD/dotsim/apps python3 dotsim/server/server.py
   ```
   Then build the apps once through it (`POST /api/build {"local": "<id>"}`) and open
   http://localhost:8000.

## Test it

```bash
dotsim/tests/run-in-ct.sh -q -rs
```

`run-in-ct.sh` runs on the Proxmox host: it copies this checkout (without `.git`) into CT 123
(`/opt/kode/pro-test`), the container with ESP-IDF, kode-sdk and bubblewrap, and runs pytest there
with `/opt/kode/pro-venv`. That venv needs aiohttp, numpy, Pillow, pytest, pytest-asyncio and
Playwright (with its Chromium). All 68 tests should pass with none skipped; tests skip themselves
when bwrap or ESP-IDF is missing, so check the `-rs` summary.

## Safety

Visitors may edit and build C code. Builds and sims run under `bwrap --unshare-all`
(no network, read-only system) with `prlimit` memory and CPU limits (see *Sandbox caps* above
and `server/sandbox.py`); one build per visitor, at most four sim processes at once
(`KODE_MAX_SESSIONS`). Guest builds are deleted after six hours. Put the server behind a reverse
proxy; `deploy/Caddyfile.example` shows one that lets everyone play and edit, and keeps building
from Git for a logged-in user.

## Open work

The plan is `docs/superpowers/plans/2026-11-16-dotsim-pro.md` (spec:
`docs/superpowers/specs/2026-11-16-dotsim-pro-design.md`). Done: Milestone 1 (Tasks 1-3) and
Tasks H1 and H1b. Open:

- **Milestone 1b:** Task H2 (USB computer), H3 (NFC, RFID, IR), H4 (fake WiFi and BLE), H5 (speed meter).
- **Milestone 2:** queue and workers (Tasks 4-5).
- **Milestone 3:** bring your own app: GitHub link and zip for everyone, LVGL and kui in the host-sim (Tasks 6-7).
- **Milestone 4:** Beginner and Advanced (Tasks 8-9).
- **Milestone 5:** the exact engine, only if the spike says it is usable (Task 10).
- **GUI redesign**, agreed with Rein: Beginner with lessons, Advanced as an IDE.

Known follow-ups:

- A memory cgroup per sim: bwrap 0.8 cannot cap tmpfs inodes, so an app can still spend server
  RAM on inodes and dentries (many empty files) until then.
- `build_job`'s sandbox has an uncapped `--tmpfs /tmp` (the sim's is capped; the build's is not).
- Open the sandbox through a pidfd: today the server reads `/proc/<pid>/task/<pid>/children` and
  then opens `/proc/<child>/root/...`, a small PID-reuse window.
- Smaller deferred review items are in the working notes, which are not in git
  (`.superpowers/sdd/2026-11-16-dotsim-pro/progress.md` on the dev machine, lines starting "minor (deferred)").
