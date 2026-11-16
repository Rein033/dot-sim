# Dot Sim Pro Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn Dot Sim into a public, queue-protected simulator on kode-sim's page, with Beginner and Advanced modes, apps from a GitHub link or a zip, and (if the spike allows) an "exact" esp-emu engine next to the fast host-sim.

**Architecture:** kode-sim's page (`web/`) is the one front end; our additions load through one hook script. Both engines speak kode-sim's `KD` wire to the page; the fast engine's server translates the host-sim's records to it and adds a compressed frame type. A front service queues sessions and builds and spreads them over worker processes/containers.

**Tech Stack:** Python 3.11 + aiohttp (server), plain JS (page), ESP-IDF v6.1 linux target (host-sim), LVGL 9.6 + kui, kode-sim 0.2 + esp-emu 1.0.0 (exact engine), pytest, Playwright for page checks.

**Spec:** `docs/superpowers/specs/2026-11-16-dotsim-pro-design.md`

## Global Constraints

- Public, no accounts. Per IP: 1 build and 1 session at a time.
- Sessions end after 2 min idle; total 20 min (fast) / 10 min (exact).
- Builds: 3 min, 2 GB address space, no network (bwrap `--unshare-all`); zip ≤ 20 MB; repo shallow clone ≤ 200 MB; no submodules from hosts other than github.com.
- Start capacity: 2 fast workers × 6 slots, 2 build workers; exact slots from the spike result.
- Our code in `dotsim/`; changes to kode-sim's own files limited to one `<script>` hook line in `web/index.html`, marked `<!-- dotsim -->`.
- All branding in `dotsim/web/theme/` (name "Dot Sim", mascot Dash, docs.kode.diy colours). No Kode name as our name, no Kody as our face, until Kode's written permission.
- Every page shows: "Unofficial. Not made, approved or supported by Kode."
- Commits as Rein033, no Claude co-author line. Each milestone ends with a push to `main` (live within 5 minutes).

## Review Focus

1. A visitor opens two tabs (or reconnects): the second must queue or take over, never get a second slot. → Task 5 test `test_same_ip_second_session_waits`.
2. A worker dies mid-session: the page must show "connection lost, back in the queue", not hang. → Task 5 test `test_worker_gone_session_ends`.
3. A zip with `../` paths or symlinks out of the folder: must be refused. → Task 7 test `test_zip_path_traversal_refused`.
4. A repo whose build never ends or prints gigabytes of log: stopped at 3 min, log capped. → Task 7 test `test_build_log_capped`.
5. A phone on a slow link: frames must not pile up; the server drops old frames when the socket is behind. → Task 1 test `test_frame_dropped_when_socket_busy`.

---

## Milestone 1: kode-sim's page on the fast engine (live)

### Task 1: KD wire for the fast engine

**Files:**
- Create: `dotsim/server/kdwire.py`
- Modify: `dotsim/server/server.py` (new `/ws` handler speaking KD; old JSON handler kept at `/ws-legacy` until Task 3 is live)
- Test: `dotsim/tests/test_kdwire.py`

**Interfaces:**
- Produces: `encode(type: int, payload: bytes) -> bytes`; `decode(buf: bytes) -> Iterator[tuple[int, memoryview]]` (resyncs on `KD`, refuses length > 600000); constants copied from `crates/kode-sim/src/proto.rs` plus `FB_JPEG = 0x13` (payload: u16 x, u16 y, u16 w, u16 h, JPEG bytes); `sim_record_to_kd(typ: str, body: bytes) -> list[bytes]` (F→FB_JPEG, E→LED, H→HAPTIC, P→PCM_OUT, T→PCM_OUT tone synthesised, L→console JSON); `kd_to_sim_lines(type: int, payload: bytes) -> list[str]` (TOUCH, BUTTONS with kode-sim's bit order top=1 bot=2 left=4 right=8 up=16 down=32 mapped to the sim's mask, BATTERY, MOTION, CONSOLE_IN).

- [ ] **Step 1: Write failing tests** `test_roundtrip`, `test_resync_after_garbage`, `test_too_long_refused`, `test_buttons_bit_order`, `test_frame_dropped_when_socket_busy` (a fake socket with a pending send; a second frame replaces the first, never queues).
- [ ] **Step 2: Run** `cd dotsim && python3 -m pytest tests/test_kdwire.py -v` → FAIL (module missing).
- [ ] **Step 3: Implement `kdwire.py`**, and in `server.py` a `FrameSender` that keeps only the newest pending frame per socket.
- [ ] **Step 4: Run tests** → PASS.
- [ ] **Step 5: Commit** `dotsim: KD wire for the fast engine`.

### Task 2: Battery, USB, motion and console in the host-sim

**Files:**
- Modify: `dotsim/hostsim/components/khal/src/kdotsim_bridge.c` (new input lines `p <pct> <usb>`, `i <ax> <ay> <az> <gx> <gy> <gz> <mx> <my> <mz> <temp>` (`m` is taken by the mirror picture), `c <text>`; console output as `L` records)
- Modify: `dotsim/hostsim/components/khal/src/khal_i2c_sim.c` (BQ25896 VBUS/charge state, CW2217 SOC/voltage, QMI8658 and QMC5883P registers from the latest `m` line)
- Test: `dotsim/tests/test_hostsim_inputs.py` (builds `apps/slang` once, starts the ELF with a socket, sends lines, reads the I2C model state through a `?` debug line)

**Interfaces:**
- Consumes: Task 1's `kd_to_sim_lines` output format.
- Produces: the sim answers `? bat` with `bat <pct> <usb>` and `? imu` with the ten values, for tests.

- [ ] **Step 1: Write failing tests** `test_battery_line_sets_gauge` (send `p 15 0`, expect `bat 15 0`), `test_motion_line_sets_imu` (an `i` line), `test_console_roundtrip` (send `c help`, expect an `L` record containing `help`).
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** in the two C files; values in the chips' own register formats from kode-sdk's drivers.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `dotsim/hostsim: battery, USB, motion and console from the page`.

### Task 3: kode-sim's page with our hook, theme and home

**Files:**
- Modify: `web/index.html` (one line before `</body>`: `<script src="/dotsim/ext.js"></script> <!-- dotsim -->`)
- Create: `dotsim/web/ext.js` (FB_JPEG drawing on kode-sim's canvas, mode switch, Dash home and app grid on the glass when no app runs, Docs link, footer with commit), `dotsim/web/theme/theme.css`, `dotsim/web/theme/theme.js` (`window.DOTSIM_THEME = {name, mascot, colours, legal}`), move `dash.js` to `dotsim/web/theme/`
- Modify: `dotsim/server/server.py` (serve kode-sim's `web/` at `/`, ours at `/dotsim/`)
- Test: `dotsim/tests/test_page.py` (Playwright: the page loads without JS errors, Dash's face shows, TOP opens the grid, starting Snake gives > 20 fps, the Ajustes battery slider changes `bat` through Task 2's debug line)

**Interfaces:**
- Consumes: Task 1 (`FB_JPEG`, `/ws`), Task 2 (`? bat`).
- Produces: `DotSim.mode` (`'beginner'|'advanced'`, kept in localStorage), `DotSim.engine` (`'fast'|'exact'`), `DotSim.start(appId)`, `DotSim.home()` for later tasks.

- [ ] **Step 1: Write failing Playwright tests** `test_loads_clean`, `test_home_then_grid`, `test_snake_fps`, `test_battery_slider`.
- [ ] **Step 2: Run** `python3 -m pytest tests/test_page.py -v` (against a local server on port 8899) → FAIL.
- [ ] **Step 3: Implement**; kode-sim's Spanish labels stay in their file, `ext.js` replaces their text with English at load, so upstream merges stay clean.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit and push** `dotsim: kode-sim's page as the front, with modes, home and theme`. Check https://kode.uselesssite.nl shows it after the deploy timer.


## Milestone 1b: the board's parts in the fast engine (live after each task)

Common rules for Tasks H1-H5: model each part at the lowest SDK layer the sim already
replaces (dotsim/hostsim/components: khal_*_sim.c, kdrv/src/*_sim.c — read the real driver in
kode-sdk, linked in the container at dotsim/hostsim/sdk, to see what the layers above call).
Page ↔ server: JSON text messages on the KD WebSocket, `{"part": {...}}`; server ↔ sim: one new
text line prefix per part (listed per task; must not reuse `b t n m q p i c ?`). Settings UI
in dotsim/web (a card per part, shown in Advanced; reuse kode-sim's own card where it exists).
Each task: tests in dotsim/tests (C-level through the sim socket like test_hostsim_inputs.py,
plus a Playwright check of its card), then merge to main and push (live).

### Task H1: microSD

- Sim line `s <0|1>` (card out/in); the card is the folder `<KODE_SIM_DATA>/sd`.
- Implement `khal_sdmmc_*` for the sim (`dotsim/hostsim/components/khal/src/khal_sdmmc_sim.c`):
  mounted/hosted/info (1 GB card, used/free from the folder), mount point the folder.
- Page: kode-sim's "SD" checkbox drives it; Advanced card lists the card's files, uploads
  (≤ 700 KB each, ≤ 16 MB total per session) and downloads them. Server JSON:
  `{"sd": {"in": bool}}`, `{"sd_ls": {}}` → `{"sd_files": [{"name","size"}]}`,
  `{"sd_put": {"name","b64"}}`, `{"sd_get": {"name"}}` → `{"sd_file": {"name","b64"}}`.
  Names: no `/`, no `..`, ≤ 64 chars.
- Tests: `test_sd_insert_remove` (`? sd` → `sd <0|1> <files>` through kdrv_sd), `test_sd_file_seen_by_app`
  (put a file, `? sd` counts it), `test_sd_name_traversal_refused`.

### Task H2: USB computer

- The Dot as a USB device seen by a simulated computer: HID keyboard reports the app sends
  become text in Settings ("typed"); a mass-storage disk the app exposes is listed.
  Model at the layer the SDK's USB code calls (find it: khal/TinyUSB); sim line `u <0|1>`
  for the data connection (cable already in Task 2's `p`).
- Server JSON: `{"usb_typed": "text"}` (sim → page, as `L`-like record or a new record type),
  `{"usb_files": [...]}`.
- Tests: an app that types "hi" as a keyboard → page receives "hi"; disk listing appears.
  If no SDK app uses USB device classes yet, build a tiny test app in dotsim/tests/apps/.

### Task H3: NFC, RFID, IR

- RFID: sim line `r <hex10>` (an EM4100 card held) / `r -` (taken away); `kdrv_rfid_lf_read_window`
  returns it.
- NFC: sim line `f <type> <uid-hex> [ndef-b64]` (`ntag`, `mifare`) / `f -`; modelled at
  the ST25R3916 driver or knfc_tag level, whichever is lower and still small.
- IR: what the Dot transmits (kdrv_ir / knec) becomes `{"ir_tx": {"proto":"nec","addr","cmd"}}`;
  sim line `x nec <addr> <cmd>` fires a code at the Dot.
- Page: an Advanced card with presets (a URL tag, a text tag, a MIFARE UID, an EM4100 ID, an IR remote).
- Tests: one per part through the sim socket (`? rfid`, `? nfc`, IR tx record), plus the card.

### Task H4: fake WiFi and BLE

- WiFi: start → connected to SSID "DotSim" (IP 10.0.0.2); scan → fixed list
  (DotSim, Cafe-Free, Home-5G, Office); HTTP/stream/MQTT/UDP calls end with the SDK's own
  "no network" status and a log line "offline in Dot Sim"; nothing opens a socket.
- BLE: scan → fixed fake devices (Dot Watch, Heart Rate, Speaker); connects succeed to a
  fake peer that echoes writes.
- Model in `kdrv_radio_sim.c` / a new `kdrv_net_sim.c` at the kdrv_net / kdrv_ble API.
- Tests: wifi status and scan list through `? wifi`; HTTP GET ends offline; BLE scan list
  through `? ble`; a test that the sim process has no AF_INET socket open.

### Task H5: speed meter

- Sim answers `? perf` with `perf <cpu-permille> <heap-free> <heap-min>` (CPU of the sim
  process over the last second, FreeRTOS heap figures).
- Advanced: a strip with fps, frame time, CPU and heap, updated each second, with a hint
  when fps < 25 ("may stutter on a Dot").
- Tests: `? perf` answers within range; the strip appears in Advanced and updates.

## Milestone 2: queue and workers (live)

### Task 4: Front and worker roles

**Files:**
- Create: `dotsim/server/front.py` (queue, worker table, WebSocket proxy), `dotsim/server/worker.py` (today's session code moved out of server.py)
- Modify: `dotsim/server/server.py` (`KODE_ROLE=front|worker|all`; `all` keeps today's single-process mode)
- Test: `dotsim/tests/test_front.py`

**Interfaces:**
- Produces: worker HTTP API `GET /w/load -> {"slots": int, "used": int, "engine": "fast"|"exact"}`, `GET /w/ws?app=…` (KD WebSocket); front `GET /api/queue -> {"fast": {"waiting": int, "eta_s": int}, "exact": {...}}`; KD text message `{"queue": {"place": int, "eta_s": int}}` while waiting.

- [ ] **Step 1: Write failing tests** `test_picks_least_loaded_worker`, `test_queue_place_reported`, `test_same_ip_second_session_waits`, `test_idle_session_closed_after_120s` (clock injected), `test_worker_gone_session_ends`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement**; client IP from `Cf-Connecting-Ip`, else `X-Forwarded-For`, else peer.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `dotsim: front and worker roles with a fair queue`.

### Task 5: Deploy the workers

**Files:**
- Create: `dotsim/deploy/dotsim-worker@.service`, `dotsim/deploy/workers.example.json`
- Modify: `dotsim/deploy/dotsim.service` (front), `dotsim/README.md` (how to add a worker)
- Infra (not in the repo): CT 123 runs the front and one fast worker; a new CT cloned from 123 runs the second fast worker and a build worker; firewall: workers accept only the front's IP.

- [ ] **Step 1:** Clone CT 123 to a new CT (6 cores, 4 GB), give it the next free id and IP, start `dotsim-worker@fast`.
- [ ] **Step 2:** Front's `workers.json` lists both; `curl /api/queue` shows 12 fast slots.
- [ ] **Step 3:** Load test: 14 Playwright clients from pve2; expect 12 running, 2 with a queue place, none failing.
- [ ] **Step 4: Commit and push** `dotsim: worker units and how to add one`.

## Milestone 3: bring your own app (live)

### Task 6: GitHub link and zip for everyone

**Files:**
- Create: `dotsim/server/sources.py` (fetch a repo or unpack a zip into a clean folder, content hash)
- Modify: `dotsim/server/server.py` (`POST /api/app` with `{repo, ref}` or multipart `zip`; public; per-IP one build; cache by hash)
- Modify: `deploy/Caddyfile.example` (the new path public)
- Test: `dotsim/tests/test_sources.py`

**Interfaces:**
- Produces: `fetch_repo(url: str, ref: str, dest: Path) -> str` (hash), `unpack_zip(data: bytes, dest: Path) -> str` (hash); `POST /api/app -> {"job": str, "app": str}`.

- [ ] **Step 1: Write failing tests** `test_zip_path_traversal_refused`, `test_zip_symlink_refused`, `test_zip_too_big_refused` (20 MB + 1), `test_repo_other_host_refused`, `test_same_source_same_hash`, `test_build_log_capped` (log kept to the last 3000 lines, 1 MB).
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `dotsim: build any app from a GitHub link or a zip`.

### Task 7: LVGL and kui in the host-sim

**Files:**
- Modify: `dotsim/hostsim/CMakeLists.txt` (add `kui`, LVGL 9.6 and cJSON when the app requires `kui` or `lvgl__lvgl`), `dotsim/hostsim/build.sh` (take kbsp's `sdkconfig.board` LV_ settings, as the kodeOS spike does)
- Create: `dotsim/hostsim/components/khal/force/khal_task_psram.h` (no-op), `dotsim/hostsim/ext/README.md` (where LVGL 9.6.0 and cJSON 1.7.19 come from)
- Test: `dotsim/tests/test_lvgl_app.py` (builds kode-sdk's `examples/display_lvgl`, starts it, expects a non-black frame within 10 s)

- [ ] **Step 1: Write failing test** `test_display_lvgl_draws`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS; also Snake still builds and runs.
- [ ] **Step 5: Commit and push** `dotsim/hostsim: LVGL and kui apps`.

## Milestone 4: Beginner and Advanced (live)

### Task 8: Beginner mode: lessons and plain errors

**Files:**
- Create: `dotsim/web/lessons.json` (8 lessons: change a colour, move faster, LED on TOP, beep on BOT, a score, a timer, touch to move, a high score), `dotsim/web/lessons.js`, `dotsim/web/errors.js` (gcc message → plain sentence, for: expected ';', undeclared identifier, implicit declaration, incompatible types, too few arguments)
- Test: `dotsim/tests/test_lessons.py` (each lesson's solution builds, and its check passes: LED colour, a pixel's colour at a point, or a console line)

- [ ] **Step 1: Write failing test** `test_every_lesson_solution_passes`, `test_error_messages_explained`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `dotsim: beginner lessons and plain-language errors`.

### Task 9: Advanced mode: project panel and performance

**Files:**
- Modify: `dotsim/web/ext.js` (project panel: link, zip, build log with file:line, download of the log and of the exact engine's `.bin`; performance strip: fps, frame time, heap from a `? heap` debug line)
- Modify: `dotsim/hostsim/components/khal/src/kdotsim_bridge.c` (`? heap`)
- Test: `dotsim/tests/test_page.py` (`test_advanced_panels`, `test_error_link_jumps_to_line`)

- [ ] **Step 1–4:** test, fail, implement, pass.
- [ ] **Step 5: Commit and push** `dotsim: advanced mode panels`.

## Milestone 5: the exact engine (only if the spike says it is usable)

Gate: the spike's Snake run on pve2 reaches ≥ 15 fps at ≤ 1.5 cores. Below that, skip this milestone and write the numbers in the spec.

### Task 10: Exact worker

**Files:**
- Create: `dotsim/server/exact.py` (one `kode-sim run --port N` per session with `KODE_SIM_BROWSER` set to a no-op, its `/ws` proxied; FB_RECT/FB_PACKED recompressed to FB_JPEG; the visitor's app built with `kode-sim build`, cached by hash)
- Create: `dotsim/deploy/dotsim-worker@exact` settings (Rust toolchain, esp-emu 1.0.0 in `/opt/dotsim/tools`)
- Test: `dotsim/tests/test_exact.py` (`test_exact_session_draws` on the template app; `test_exact_session_killed_at_limit`)

- [ ] **Step 1–4:** test, fail, implement, pass.
- [ ] **Step 5: Commit and push** `dotsim: exact engine on esp-emu`.
