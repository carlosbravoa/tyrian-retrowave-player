# CLAUDE.md

Guidance for working in this repo. See `README.md` for the user-facing overview.

## What this is

Two players for Tyrian's music, both running OpenTyrian's **actual** LDS driver
(`src/lds_play.c`) and forwarding the OPL register writes it produces:

- `tyrian_rwplay` — dependency-free CLI, streams to a RetroWave OPL3 Express
  board over USB-serial (board-only audio).
- `tyrian_rwgui` — SDL2 GUI: software OPL3 emulator (PC audio) + board + a
  channel visualizer.

Both can additionally emit **General MIDI** to an ALSA sequencer port (`--midi`).

## Build

```sh
make              # CLI always; GUI too if SDL2 present; MIDI in both if ALSA present
make tyrian_rwplay   # force CLI only
make tyrian_rwgui    # force GUI
```

SDL2 and ALSA (`libasound2-dev`) are **both optional**, auto-detected via
pkg-config. Keep it that way: the CLI must stay buildable with nothing but a C
compiler (plus `-lm`). ALSA support is gated on `-DHAVE_ALSA`; without it
`src/midi_out.c` compiles to safe no-ops and pulls in no link dependency.

`lds_play.c` is compiled **separately per target** because the two builds route
the `opl_write` macro differently (CLI → emulator stub via `compat/opl.h`;
GUI → fan-out hook via `emu/opl.h`).

## Architecture / where things live

| File | Role |
|---|---|
| `src/lds_play.{c,h}` | Tyrian's LDS driver, **vendored unmodified** from OpenTyrian2000. Do not edit — re-vendor by copying from upstream. |
| `src/retrowave_serial.c` | RetroWave 7-of-8 wire framing + OPL packets. 2 Mbaud is mandatory; wrong baud fails silently. |
| `src/emu/opl.c` | DOSBox OPL2/OPL3 emulator (GUI only) + an appended `opl_channel_level()` probe for the visualizer. |
| `src/midi_out.{c,h}` | Optional General MIDI: ALSA seq client + OPL-register→GM tracker. |
| `src/tyrian_rwplay.c` | CLI: drives `lds_update()` at 69.5 Hz against monotonic deadlines. |
| `src/rwgui.c` | GUI: SDL audio callback clocks the driver; fans the register stream to emulator + board + MIDI + visualizer. |
| `src/compat/` | Tiny stand-ins for OpenTyrian headers so the CLI needs no SDL. |

### The register fan-out is the integration seam

Everything downstream of the driver taps the single `opl_write` stream:
- CLI: `adlib_write()` in `tyrian_rwplay.c`.
- GUI: `rwgui_opl_write()` in `rwgui.c`.

New output sinks hook in **there**, which is exactly how MIDI was added — so
`lds_play.c` stays byte-for-byte upstream.

### How MIDI works (important context)

OPL is 4-operator FM with no concept of notes, so `midi_out.c` reconstructs GM
from the register stream:
- **note on/off** ← rising/falling edge of the key-on bit (0x20) in regs 0xB0–0xB8;
- **pitch** ← F-Number (regs 0xA0/0xB0) + block → Hz → MIDI note;
- **GM program** ← each LDS patch carries a `midinst` (Tyrian's format is
  MIDI-aware). The tracker parses the song's patch table itself
  (`midi_out_set_song`) and identifies the active instrument per note by
  **fingerprinting** the stable patch registers (mod/car misc/ad/sr/wave +
  feedback; volume regs 0x40/0x43 are excluded — tremolo/fade modulate them).
- Patches with `midinst >= 128` are percussion → GM channel 10, note `midinst-128`.

Known limitation: continuous FM effects (vibrato/glide/arpeggio) keep the key on
and only rewrite frequency, so they don't produce new MIDI events — a glided note
holds its starting pitch. Adding pitch-bend would go in the 0xB0 same-key-held
branch of `midi_feed()`.

Thread-safety: in the GUI, ALSA writes happen from the audio thread
(`midi_feed`). Any MIDI control from the main thread (e.g. the `m` toggle) must
hold `SDL_LockAudioDevice` so it can't race the callback — see `toggle_midi()`.

## Testing MIDI without hardware

```sh
aseqdump &                        # creates a capture port; note its client:port
./tyrian_rwplay -d - -m <addr> -l -s 1 MUSIC.MUS   # send GM there, no board
```
`MUSIC.MUS` is the user's own file from a legitimate Tyrian copy — not in the
repo. A local copy is at `~/dosbox/games/tyrian/MUSIC.MUS`.

## Conventions

- Match the surrounding C style: tabs for indentation, terse comments that
  explain *why*, `snake_case`.
- Commit/push only when asked. Don't introduce required dependencies — keep
  SDL2 and ALSA optional.
