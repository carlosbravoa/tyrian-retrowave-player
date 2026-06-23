# License

This repository is a combination of works under compatible copyleft licenses.
The combined work is distributed under the **GNU General Public License,
version 3 or later (GPL-3.0-or-later)**.

## Components

- **`src/lds_play.c`, `src/lds_play.h`**
  Vendored unmodified from [OpenTyrian](https://github.com/opentyrian/opentyrian)
  / OpenTyrian2000.
  Copyright (C) 2007-2009 The OpenTyrian Development Team, and contributors.
  Portions adapted from AdPlug.
  Licensed under the **GNU GPL, version 2 or later**.

- **`src/emu/opl.c`**
  DOSBox OPL2/OPL3 software emulator, vendored from OpenTyrian / DOSBox.
  Copyright (C) 2002-2010 The DOSBox Team; based on ADLIBEMU.C by Ken
  Silverman (C) 1998-2001.  Licensed under the **GNU LGPL, version 2.1 or
  later**.  (`src/emu/opl.h` is the project's own hooked interface header.)

- **`src/retrowave_serial.c`, `src/retrowave_serial.h`**
  RetroWave OPL3 Express serial framing. The wire protocol originates from
  [SudoMaker's RetroWave](https://github.com/SudoMaker/RetroWave) project,
  licensed **AGPL-3.0**. Keep this attribution if you redistribute.

- **`src/tyrian_rwplay.c`, `src/rwgui.c`, `src/font5x7.h`, `src/emu/opl.h`,
  `src/compat/*`, `Makefile`**
  Copyright (C) 2026 Carlos Bravo.
  Licensed under the **GNU GPL, version 3 or later**.

## Note

You must supply your own `MUSIC.MUS` from a legitimate copy of Tyrian. The game
data is not covered by this license and is not redistributed here.

The full text of the GNU GPL v3 is in [`COPYING`](COPYING) (from
<https://www.gnu.org/licenses/gpl-3.0.txt>).
