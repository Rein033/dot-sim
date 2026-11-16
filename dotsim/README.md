# Dot Sim: the browser layer

`dotsim/` is what this fork adds to kode-sim: a Kode Dot that runs in any browser, served
from one Linux machine, for many visitors at once. It is the code behind the public Dot Sim.

It is **not** kode-sim's emulator. kode-sim runs the real firmware instruction by
instruction on esp-emu, on your own computer. Dot Sim builds an app for ESP-IDF's
`linux` target on the real kode-sdk, with the board simulated under it, and runs it as
a normal process on the server: fast (an app runs at full speed on one small core) but
not cycle-exact, and without kodeOS for now.

Dot Sim is unofficial. It is not made, approved or supported by Kode.

| Folder | What it is |
|---|---|
| `hostsim/` | The ESP-IDF project that builds one app for the host: the SDK's own components, and sim versions of `khal`, `kdrv` and `kbsp` underneath (a virtual I2C bus with models of the board's chips, a frame buffer for the screen, audio, LED, haptic, storage, NVS). `build.sh <app> <out>` builds `kode_sim_app.elf`. |
| `server/` | `server.py` (aiohttp): one sandboxed sim process per WebSocket, frames to the browser as JPEG, buttons and touch back. Builds apps from Git, and from code edited in the browser. |
| `web/` | The page: the Dot, the home screen with Dash and the app grid, the code editor, the docs. |
| `apps/` | Ten small apps on *kodekit* (a frame-painting kit on the public SDK), their pixel-art icons, and `assemble.py`, which turns each into a normal kode-app-template folder. |
| `deploy/` | A systemd unit and a reverse-proxy example. |

## Dash

Dash, the dog on the home screen (`web/dash.js`), is Dot Sim's own mascot: drawn on a
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
5. Start the server (see `deploy/dotsim.service` for the paths it reads):
   ```bash
   KODE_SIM=$PWD/dotsim/hostsim KODE_LOCAL_APPS=$PWD/dotsim/apps python3 dotsim/server/server.py
   ```
   Then build the apps once through it (`POST /api/build {"local": "<id>"}`) and open
   http://localhost:8000.

## Safety

Visitors may edit and build C code. Builds and sims run under `bwrap --unshare-all`
(no network, read-only system) with `prlimit` memory and CPU limits; one build per visitor,
at most four sim processes at once (`KODE_MAX_SESSIONS`). Guest builds are deleted after
six hours. Put the server behind a reverse proxy; `deploy/Caddyfile.example` shows one that
lets everyone play and edit, and keeps building from Git for a logged-in user.
