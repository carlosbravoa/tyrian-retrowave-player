# tyrian-retrowave

Play [Tyrian](https://en.wikipedia.org/wiki/Tyrian_(video_game))'s music on a
real Yamaha **YMF262 (OPL3)** via a
[RetroWave OPL3 Express](https://github.com/SudoMaker/RetroWave) board over
USB-serial (default `/dev/ttyACM0`, 2,000,000 baud).

It runs Tyrian's **actual** LDS music driver (`src/lds_play.c`, from OpenTyrian)
and forwards the OPL register writes it produces straight to the board instead
of a software emulator — so the output is reference-grade: exact instruments,
timing, and loop points.

No SDL, no external libraries — just a C compiler.

## Build

```sh
make
```

## Run

```sh
./tyrian_rwplay -d ttyACM0 /path/to/MUSIC.MUS
```

Options:

- `-d DEV`  serial device (default `ttyACM0`; `-` = dry run, no hardware)
- `-s N`    start at song N (1-based)
- `-l`      loop the current song instead of advancing
- `-h`      help

Transport keys while playing: `n`/space next, `p` prev, `r` restart,
`l` toggle loop, `+`/`-` tempo, `q` quit.

`MUSIC.MUS` is your own file from a legitimate copy of Tyrian. It is **not**
redistributable and is not included here.

## How it works

- `src/retrowave_serial.c` — the RetroWave 7-of-8 wire framing plus OPL
  register-write / reset packets. 2 Mbaud is mandatory; a wrong baud rate fails
  silently (garbled / no sound).
- `src/tyrian_rwplay.c` — loads the `MUSIC.MUS` song bank, drives the LDS driver
  at 69.5 Hz against absolute monotonic deadlines (the OPL chip has no timing of
  its own), and provides the `adlib_*` / `fread_die` shims the driver expects.
- `src/lds_play.c` — Tyrian's LDS driver, vendored unmodified from OpenTyrian.
- `src/compat/` — tiny SDL-free stand-ins for the four OpenTyrian headers that
  `lds_play.c` includes (just typedefs, the `opl_*` macros, and the little-endian
  `fread_*_die` readers).

To re-vendor the driver after an upstream change, copy `src/lds_play.{c,h}` from
OpenTyrian2000 again — nothing else here depends on the game.

## License

See [LICENSE.md](LICENSE.md). In short: `lds_play.{c,h}` is GPL-2.0-or-later
(OpenTyrian); the RetroWave framing is from SudoMaker (AGPL-3.0); the combined
work is distributed under the GNU GPL v3 (or later).
