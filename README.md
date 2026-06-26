# tyrian-retrowave

Play [Tyrian](https://en.wikipedia.org/wiki/Tyrian_(video_game))'s music on a
real Yamaha **YMF262 (OPL3)** via a
[RetroWave OPL3 Express](https://github.com/SudoMaker/RetroWave) board over
USB-serial (default `/dev/ttyACM0`, 2,000,000 baud) — or on your PC's speakers
via a software OPL3 emulator — or as **General MIDI** out an ALSA sequencer port
(e.g. to a hardware GM module or a soft synth).

Both players run Tyrian's **actual** LDS music driver (`src/lds_play.c`, from
OpenTyrian) and forward the OPL register writes it produces. Output is
reference-grade: exact instruments, timing, and loop points.

![Neon glow visualizer](docs/visualizer-neon.png)

There are two programs:

| | `tyrian_rwgui` | `tyrian_rwplay` |
|---|---|---|
| Interface | graphical app + channel visualizer | terminal |
| Audio | PC (emulated) and/or the board | board only |
| MIDI out | `--midi` (if built with ALSA) | `--midi` (if built with ALSA) |
| Dependencies | SDL2 (+ ALSA for MIDI) | none (ALSA only for MIDI) |

## Build

```sh
make            # builds the GUI too if SDL2 is present; otherwise CLI only
```

**SDL2 and ALSA are both optional.** The CLI player (`tyrian_rwplay`) needs
nothing but a C compiler and is fully independent of the GUI. `make`
auto-detects SDL2: with it, both players build; without it, you get the CLI only
(and a note). `make tyrian_rwplay` always builds just the CLI; `make
tyrian_rwgui` forces the GUI.

ALSA (`libasound2-dev`) is auto-detected too: with it, both players gain
General MIDI output (`--midi`); without it everything else still builds and
`--midi` simply reports it's unavailable.

## GUI player

```sh
./tyrian_rwgui -d ttyACM0 /path/to/MUSIC.MUS
```

A window opens with a synth-style **channel-activity visualizer** — one bar per
OPL channel, driven directly by the register stream (key-on, note pitch, and
carrier level). Three switchable styles:

| LED VU | Neon glow | Spectrum |
|---|---|---|
| ![](docs/visualizer-led.png) | ![](docs/visualizer-neon.png) | ![](docs/visualizer-spectrum.png) |

The bars track each channel's **real output level** read from the emulator
(peak of the carrier's actual output), so they rise and fall with the actual
sound, not just note on/off — a silent channel reads near zero even if a note
is technically held. Output is selectable at runtime: **PC**, **Board**, or **Both**;
if the board isn't connected it falls back to PC audio, so the app works on any
machine.

A clickable button bar along the bottom mirrors every control. The same actions
are on the keyboard:

Options: `-d DEV` (serial device, `-` = no board), `-m PORT` (also send General
MIDI to an ALSA seq port, e.g. `20:0`; `-` = open a port but don't auto-connect),
`-s N` (start song), `-l` (loop).

| key | action | key | action |
|---|---|---|---|
| `space` | play / pause | `o` | cycle output (PC / Board / Both) |
| `n` / `→` | next song | `v` | cycle visualizer style |
| `p` / `←` | previous song | `+` / `-` | tempo |
| `r` | restart | `l` | toggle loop |
| `q` / `Esc` | quit | `m` | toggle MIDI (with `--midi`) |

## CLI player

A dependency-free terminal player that streams to the board:

```sh
./tyrian_rwplay -d ttyACM0 /path/to/MUSIC.MUS
```

Options: `-d DEV` (`-` = dry run), `-m PORT` (General MIDI to an ALSA seq port,
e.g. `20:0`), `-s N`, `-l`, `-h`. Transport keys: `n`/space next, `p` prev, `r`
restart, `l` loop, `+`/`-` tempo, `q` quit.

## General MIDI output

With `--midi PORT`, either player also emits **General MIDI** to an ALSA
sequencer port — a hardware GM module, a USB-MIDI interface, or a soft synth
like FluidSynth. `PORT` is anything `aconnect`/`snd_seq` understands: a
`client:port` address (`20:0`) or a port name. Use `-` to create the port
without auto-connecting, then wire it up later with `aconnect`.

```sh
./tyrian_rwplay -d - -m 20:0 MUSIC.MUS      # MIDI only, straight to client 20:0
./tyrian_rwgui  -m 20:0 MUSIC.MUS           # board/PC as usual, plus GM; 'm' toggles
aplaymidi -l                                # list destination ports
```

This isn't a recording of the FM — it's a real-time reconstruction. The OPL is a
4-operator FM synth with no notion of "notes," so the converter taps the same
register stream the board sees and rebuilds GM events from it: note on/off from
each channel's key-on bit, pitch from the F-Number/block, and the **GM program**
from Tyrian's own per-instrument `midinst` field (recovered by fingerprinting the
patch registers the driver writes). Patches marked as percussion land on GM
channel 10. Continuous FM effects (vibrato, glide, arpeggio) aren't translated to
pitch-bend yet, so a glided note holds its starting pitch on the MIDI side.

---

`MUSIC.MUS` is your own file from a legitimate copy of Tyrian. It is **not**
redistributable and is not included here.

## How it works

- `src/lds_play.c` — Tyrian's LDS driver, vendored unmodified from OpenTyrian.
- `src/retrowave_serial.c` — RetroWave 7-of-8 wire framing + OPL register-write
  / reset packets. 2 Mbaud is mandatory; a wrong baud rate fails silently.
- `src/tyrian_rwplay.c` — CLI: loads the `MUSIC.MUS` bank, drives the driver at
  69.5 Hz against monotonic deadlines (the OPL chip has no timing of its own),
  and shims `adlib_*` / `fread_die`.
- `src/rwgui.c` — GUI: an SDL2 app whose audio callback clocks the driver from
  the sample counter (as the original game does), fans the register stream out
  to the emulator (PC), the board, and the visualizer, and renders the bars.
- `src/midi_out.c` — optional General MIDI backend: an ALSA sequencer client plus
  an OPL-register→GM tracker. Taps the same `opl_write` fan-out (so `lds_play.c`
  stays untouched), recovers notes/pitch/program, and parses each song's patch
  table itself to map `midinst` → GM program.
- `src/emu/opl.c` — DOSBox software OPL2/OPL3 emulator (GUI/PC audio only), with
  a small appended `opl_channel_level()` probe (and a per-operator peak field)
  that exposes each channel's real output level for the visualizer.
- `src/compat/` — tiny stand-ins for the OpenTyrian headers `lds_play.c`
  includes, so the CLI needs no SDL.

`lds_play.c` is compiled separately per target because the two builds route the
`opl_write` macro differently (CLI → emulator stub; GUI → a fan-out hook). To
re-vendor after an upstream change, copy `src/lds_play.{c,h}` (and, for the
emulator, `src/emu/opl.{c,h}`) from OpenTyrian2000 again.

## License

See [LICENSE.md](LICENSE.md). In short: `lds_play.{c,h}` is GPL-2.0-or-later and
`emu/opl.c` is LGPL-2.1-or-later (both from OpenTyrian/DOSBox); the RetroWave
framing is from SudoMaker (AGPL-3.0); the combined work is distributed under the
GNU GPL v3 (or later).
