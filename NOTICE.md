# Licences of kode-sim

This file says which licence applies to each part of this repository. Read it before you distribute kode-sim, or a firmware that you built with it.

| Part | Owner | Licence |
|---|---|---|
| everything that this file does not name below | Kode | GPL-3.0-or-later |
| `web/kode-dot.png`, `dotsim/web/kode-dot.png` and the screenshots in `.github/img/` | Kode | the pictures of the Kode Dot and of Kody: see the trademarks below |
| `dotsim/` (except `dotsim/web/kode-dot.png`) | the dot-sim contributors | GPL-3.0-or-later; Dash's drawing (`dotsim/web/dash.js`) and the app icons (`dotsim/apps/icons/`, made by `make_icons.py`) are also offered under Creative Commons Attribution-ShareAlike 4.0 |

## The code of Kode

Copyright (C) 2026 Kode.

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

The file `LICENSE` holds the full text of the GNU General Public License, version 3.

### Trademarks, under GNU GPL version 3 section 7(e)

This licence gives no right to use the names Kode, Kode Dot, kodeOS and Kody, or the character Kody, as trademarks. `TRADEMARKS.md` in the repository [`kodediy/.github`](https://github.com/kodediy/.github/blob/main/TRADEMARKS.md) says how you can use them.

## What kode-sim downloads

These are not in this repository, and each has its own licence:

- **esp-emu**, Espressif's emulator. kode-sim installs the pinned version on its first run with Espressif's installer, in its own cache. Its terms are Espressif's ([espressif/esp-emulator](https://github.com/espressif/esp-emulator)).
- **The Rust crates** in `Cargo.lock`, downloaded by `cargo build`. Each one says its licence on crates.io.
- **The firmware** that kode-sim builds and runs: [kode-sdk](https://github.com/kodediy/kode-sdk), [kodeOS](https://github.com/kodediy/KodeOS) and the apps. Their `NOTICE.md` files say which licence applies to each part. `firmware/kemu/` is linked into that firmware, under GPL-3.0-or-later like the rest of this repository.

## This fork

dot-sim is a fork of kode-sim by the dot-sim contributors. It adds the folder `dotsim/` and
this section, and puts a short note above the README; the rest of the repository is Kode's
kode-sim as it was at the commit the fork started from, or as later merged from upstream.
dot-sim is not made, approved or supported by Kode, and its name and its mascot, Dash, are
its own.
