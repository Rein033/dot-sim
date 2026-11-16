# Roadmap

The aim: **the same binary runs on the board and in the simulator** - the
official kodeOS, the apps from apps.kode.diy, an Arduino upload through the
mailbox - with nothing linked in for the simulator. Each part that
Espressif's esp-emu gains takes its piece out of kemu (`firmware/kemu`);
when kemu is empty, `kode-sim build --board` and `kode-sim build` are the
same build.

## Where each part stands (after esp-emu 1.0.0)

| Part of the Kode Dot | Today | Waits for |
|---|---|---|
| ESP32-P4, flash, PSRAM | esp-emu | - |
| I2C chips, buttons, IRQs | esp-emu's virtual parts, answered by kode-sim | - |
| MIPI-DSI panel (410×502) | kemu (`kemu_panel.c`) | DSI in esp-emu - [esp-emulator#5](https://github.com/espressif/esp-emulator/issues/5) |
| JPEG and PPA (kodeOS's images) | in software | [esp-emulator#5](https://github.com/espressif/esp-emulator/issues/5) |
| I2S (speaker, microphone) | esp-emu (1.0.0); kode-sim is the ES8311's audio end over `--cosim-tcp`. kemu keeps the word clock (WS, GPIO 37) off its pad: esp-emu drove it there and sent kode-sim every edge | WS kept off the pad - [esp-emulator#52](https://github.com/espressif/esp-emulator/issues/52) |
| microSD (SDMMC) | kemu (`kemu_sd.c`) | [esp-emulator#33](https://github.com/espressif/esp-emulator/issues/33) |
| ESP32-C5 over SDIO (WiFi, BLE) | kemu: the P4's Ethernet behind the radio's signals | the C5 as an SDIO slave - [esp-emulator#48](https://github.com/espressif/esp-emulator/issues/48) (DAT1 on our own D1 pin is in 1.0.0) |
| Light sleep, the 32 kHz crystal | missing in esp-emu | [esp-emulator#5](https://github.com/espressif/esp-emulator/issues/5) |
| esp-emu's source and terms | - | [esp-emulator#10](https://github.com/espressif/esp-emulator/issues/10) |
| IMU, magnetometer | `kodedot-chips` (QMI8658A, QMC5883P) posed from the page (`MOTION`); no FIFO or IMU events | - |
| USB-OTG (dock port) | kemu (`kemu_usb.c`, `models/usbdcd.c`) and a host in kode-sim: mass storage and keyboard; no isochronous (UVC) yet. esp-emu 1.0.0 has the controller, with a host that only takes a CDC console | a host on a socket - [esp-emulator#51](https://github.com/espressif/esp-emulator/issues/51) |
| NFC, RFID | absent, as on a board without them | not asked for: nothing to see on a desk |

## Next releases

- **0.3.0 - the display.** When esp-emu models MIPI-DSI, the panel leaves
  kemu: the board's own `esp_lcd` path draws, and kode-sim takes the frames
  from esp-emu instead of from UART1. A board binary then shows its screen.
- **0.4.0 - the card.** SDMMC in esp-emu: `kemu_sd` goes, and the card
  image is esp-emu's. (The sound already left kemu, on esp-emu 1.0.0's I2S.)
- **0.5.0 - the C5.** With an ESP32-C5 slave over SDIO, the real ESP-Hosted
  path replaces kemu's Ethernet stand-in: scans, joins, the vault, BLE.
- **1.0.0 - no kemu.** The official images and apps.kode.diy's binaries
  boot unchanged. `--board` is the only build.

Alongside these, whenever they are ready:

- **The web.** Today kode-sim runs on your own computer. A hosted
  simulator, for apps.kode.diy, will be designed again when it is needed.
- **Speed.** esp-emu runs on one core: about real time on an Apple
  M-series Mac, a third of it on a small x86 server - [esp-emulator#53](https://github.com/espressif/esp-emulator/issues/53).
  `kode-sim bench` measures it on a given computer, for a given firmware.
- **`--cosim-wait N`** (to propose to Espressif): hold the chip in reset
  until N devices have registered, which saves the double boot at start.
