# Dot Sim Pro: a public Kode Dot simulator for testing your own apps

Status: approved by Rein · 2026-11-16

## Intent

Rein and about 400 other Kode Dot owners want to **test their own Kode Dot apps** in a
browser, without a board and without installing anything. Today's Dot Sim
(kode.uselesssite.nl) is a playground: it runs our own kodekit apps fast, has a
small C editor, and builds from Git only for Rein.

The upgrade turns it into a professional tool with two modes:

- **Beginner**: play, edit a starter app, short step-by-step lessons, errors in plain words.
- **Advanced**: bring your own app (a GitHub link or a zip), see it run like on the
  board, with the console, sensors, battery and USB to control, performance numbers,
  and the build to download.

It stays **public and without accounts**, and must never be easy to knock over.

It stays on **Kode's own kode-sim as its base** (its page, its layout, its wire
protocol, its controls), so that when Kode gives written permission to use the Kode
name and Kody, switching the look to Kode's is a swap of one theme, not a rewrite.

## Goals

1. A visitor pastes a GitHub link (or uploads a zip) of a kode-sdk app and sees it running
   within a few minutes, without an account.
2. Apps that draw with LVGL/kui (the kode-sdk way) work, not only apps that paint frames.
3. Advanced mode has kode-sim's controls: battery %, USB cable, SD card, motion (pitch,
   roll, heading, magnet, shake), the console, reset and power.
4. An "exact" option runs the real firmware (kodeOS or the app, unchanged) on esp-emu,
   as kode-sim does, for when the fast engine is not enough. *(Depends on the spike.)*
5. Load is spread over several workers with a queue that shows your place; one visitor
   can never take more than their share.
6. Beginner mode teaches: a few lessons that each change one thing and run it.
7. All branding (name, mascot, colours, logo, wording about Kode) sits in one theme, so the
   Kode look can replace Dot Sim's on the day permission arrives.

## Non-goals

- Accounts, saved projects in the cloud, sharing links to someone's app (later, maybe).
- Running binaries from apps.kode.diy (kode-sim itself cannot yet).
- Bluetooth, NFC, RFID, infrared.
- Rewriting kode-sim's Rust code; we use it as it is and only add.

## Anti-goals (a failure even if it "works")

- A visitor's code reaching the network, the host, other visitors' builds or sessions.
- One person (or a script) filling every slot so nobody else gets in.
- Looking like Kode made it before Kode says so (no Kode name as our name, no Kody as our
  face, until written permission).
- Drifting away from kode-sim so far that upstream updates no longer merge.
- Beginners hitting a wall of jargon or a blank error.

## Constraints

- Hardware: pve2, 20 threads, 31 GB RAM (about 11 GB free), shared with other services.
  Today one container (CT 123: 6 cores, 4 GB) runs everything.
- Public through Cloudflare tunnel → Caddy (CT 110) → workers. Optional Authelia login stays
  for Rein's own extras.
- kode-sim is GPL-3.0; Kody art CC BY-SA 4.0; names and character are Kode's trademarks.
- The fork must keep merging cleanly from kodediy/kode-sim: our code lives in `dotsim/`, and
  the few changes to their files are small and marked.
- Bandwidth: raw RGB565 frames (kode-sim's `FB_RECT`) are ~400 KB each; over the internet the
  fast engine must send JPEG (or another compressed frame) instead.

## Approaches considered

| | What | Trade-off |
|---|---|---|
| A | kode-sim's page and controls, our fast host-sim underneath | Fast (an app at full speed on ~1 core), many sessions; not cycle-exact, no kodeOS; LVGL/kui and the sensor/battery models must be added to the host-sim. |
| B | kode-sim itself on the server (esp-emu) | The real firmware, kodeOS, everything exact; esp-emu is tested on Macs only, runs at half to full real time on an M-series core and likely slower here, so few sessions and long queues. |
| **C (chosen)** | **kode-sim's page for both; two engines: "fast" (A) by default, "exact" (B) as an option in advanced mode, in its own stricter queue** | Most work, but each visitor gets speed by default and exactness when they need it. If the spike shows esp-emu is too slow on pve2, advanced runs on A with LVGL, and B waits. |

Rein picked C.

## Design

### 1. One page, kode-sim's

The page is kode-sim's `web/` (index.html, sim.js, audio-worklet.js), extended in our fork:

- The Dot in the middle, kode-sim's "Ajustes" panel on the side (battery, USB, SD, motion,
  console, install, reset, power).
- **Mode switch** top right: Beginner / Advanced. Beginner hides Ajustes and shows the
  lesson panel and the simple editor; Advanced shows Ajustes, the project panel (link, zip,
  build log, downloads) and the performance strip.
- **Engine switch** in Advanced: Fast / Exact, with the queue length of each.
- **Home**: Dash's face and the app grid (today's launcher) when no app runs, in the fast
  engine; in the exact engine, kodeOS's own home.
- Our additions to kode-sim's files are kept in separate files where possible
  (`dotsim/web/…` loaded by a small hook in their index.html), so upstream merges stay easy.

### 2. Two engines behind one wire

Both engines speak **kode-sim's wire** (`KD` frames: FB_RECT, LED, HAPTIC, PCM, TOUCH,
BUTTONS, BATTERY, MOTION, CONSOLE, …) to the page.

- **Fast engine** (today's host-sim, `dotsim/hostsim`): the server translates its frames to the
  KD wire, plus one new frame type of ours, `FB_JPEG`, so the page gets compressed frames.
  New in the host-sim: LVGL 9.6 + kui (the kode-sdk UI kit), battery/USB/SD/motion driven
  from the page through the chip models it already has (BQ25896, CW2217, QMI8658, QMC5883P),
  the console (stdin/stdout of the app) to the page.
- **Exact engine** (kode-sim + esp-emu): one `kode-sim run` per session, headless, its page
  traffic proxied by our server; frames recompressed to `FB_JPEG` on the way out. Runs
  kodeOS with the visitor's app installed, or the app alone.

### 3. Workers, queue and limits

- **Front** (one small service): serves the page, takes builds and sessions, keeps the queue,
  hands each session to a worker, and proxies its WebSocket.
- **Workers**: containers on pve2, each with a fixed number of slots. Start: 2 fast workers
  × 6 slots, 1 exact worker × slots per spike result, 2 build workers. Adding a worker is a
  container from a template plus one line of config.
- **Queue**: first come, first served, per engine; the page shows the place and an estimate,
  and keeps the place while the tab is open.
- **Fair share without accounts**, per IP: 1 build and 1 session at a time; builds 3 min and
  2 GB; sessions end after 2 min idle or 20 min (fast) / 10 min (exact); the same source is
  built once (cache by content hash). Cloudflare rate limit on the build and upload API.
- **Sandbox**: as today (bwrap, no network, prlimit), plus a size limit on zips (20 MB) and
  repos (shallow clone, 200 MB), and no submodules from other hosts.

### 4. Bringing an app

- **GitHub link** (public repos, optional branch/tag) or **zip upload** of a project folder.
- The build log streams to the page; compiler errors link to the file and line.
- Advanced: download the built image for the exact engine (`.bin`), and the build log.

### 5. Beginner mode

- The editor (today's), with the starter and the games' code.
- **Lessons**: 6–8 short ones, each a card with one goal ("make the ball red", "blink the
  LED on TOP", "add a score"), a hint, and a check that runs the app and looks (for example
  at the LED colour or the screen) to say "done".
- Compiler errors explained in plain words for the most common ones (missing `;`, unknown
  name, wrong type).

### 6. Theme

- `dotsim/web/theme/` holds the name, the mascot, colours, fonts, logo and the "unofficial"
  wording. Today: Dot Sim + Dash in docs.kode.diy's style. After permission: a Kode theme with
  Kody's official animations (Lottie, CC BY-SA credit) and Kode's name.

## Board parts in the fast engine (added 2026-11-16, approved by Rein)

Rein asked to focus on kode-sim's status list, in our own fast engine (option 1 of three: our
sim / upstream kode-sim / both). Each part becomes a simulated device, controlled from Settings
(Advanced), inside the sandbox, never with real network:

1. **microSD**: a card per session as a folder; insert/remove from Settings (kode-sim's
   "SD" switch); put files on it and take them off.
2. **USB**: the cable (VBUS, done in Task 2) plus a simulated computer: what the Dot types as a
   USB keyboard shows in Settings; a Dot acting as a USB disk shows its files.
3. **NFC / RFID / IR**: pick a virtual NFC tag (NTAG with a URL or text, a MIFARE UID) and hold
   it to the Dot; an EM4100 card with an ID of your choice; IR codes the Dot sends (NEC) are
   shown, and you can fire a remote's code at it.
4. **WiFi / BLE (fake)**: a simulated network "DotSim" that always connects; scans return a
   fixed list of fake networks and fake BLE devices; real internet requests get a clean
   "offline in Dot Sim" answer; nothing leaves the sandbox.
5. **Speed meter**: fps, frame time, the sim's CPU and heap in Advanced, so you can judge
   whether the app would run smoothly on a Dot.

These come before the queue and the lessons.

## Decided by Rein

- Public, no accounts; protected by a queue and spreading over workers.
- Both a GitHub link and a zip.
- Approach C, kode-sim as the base, branding as a swappable theme.

## Left to the builder

- The exact queue and slot numbers (start with the ones above, tune from load).
- How the front talks to workers (a small internal API is enough).
- The lessons' texts and checks.
- Whether the exact engine shows kodeOS's home or the app directly by default.

## Spike result

Spike on pve2 (CT 123, 6 cores), 2026-11-16:

- kode-sim 0.2.0 builds on Linux x86 with Rust stable; `rsync` and `mtools` are needed.
- esp-emu 1.0.0 installs itself (x86_64-unknown-linux-gnu release) and boots
  kode-app-template from the ROM reset vector; the page is up within 6 s; the KD wire,
  battery, SD and USB messages arrive as on the Mac.
- A first build of an app takes ~100 s (managed components fetched), later builds less.
- Our kodekit apps (Snake) crash under esp-emu: the task watchdog fires because the
  display task holds CPU 0 (kodekit's frame loop never yields while it waits for the
  flip). kodekit must yield (vTaskDelay(1) or the flush-done event) before the exact engine
  can run them; kode-sdk's own apps do not have this problem.
- Speed is not measured yet: the template draws once and Snake crashed. Milestone 5 of the
  plan stays gated on a measured ≥ 15 fps at ≤ 1.5 cores.

Also found: `dotsim/apps/assemble.py` wrote an old app shape without kbsp's board settings,
so the apps had no apps partition for a real Dot or kode-sim. Fixed by taking
kode-app-template's CMakeLists.txt and sdkconfig.defaults as they are.
