/*
 * midi_out — General MIDI output for tyrian-retrowave, over an ALSA sequencer
 * port (e.g. "20:0" or a port name).
 *
 * Tyrian's music is FM (OPL3 register writes).  This module turns that register
 * stream back into General MIDI note/program events WITHOUT touching the
 * vendored LDS driver: it taps the same opl_write fan-out the board and emulator
 * already see.  It recovers
 *   - note on/off  from the key-on bit of registers 0xB0..0xB8,
 *   - pitch        from each channel's F-Number / block (regs 0xA0/0xB0),
 *   - GM program   by fingerprinting the patch registers the driver wrote and
 *                  matching them to the song's patch table (its `midinst`).
 *
 * The patch table (and its per-instrument `midinst`/`midkey`) is parsed straight
 * from the .mus song with midi_out_set_song(), so this stays self-contained and
 * lds_play.c remains byte-for-byte upstream.
 *
 * Build is optional: without ALSA (no -DHAVE_ALSA) every entry point is a safe
 * no-op and midi_out_open() reports unavailable, so the dependency-free CLI keeps
 * building with nothing but a C compiler.
 */
#ifndef MIDI_OUT_H
#define MIDI_OUT_H

#include <stdbool.h>
#include <stdio.h>

/* Open the ALSA sequencer client and output port.  port_spec may be:
 *   NULL / "" / "-"  open a port but don't auto-connect (connect with aconnect)
 *   "20:0", "name"   parse + connect to that destination (snd_seq address syntax)
 * Returns false if MIDI is unavailable (built without ALSA) or open failed. */
bool midi_out_open(const char *port_spec);

void midi_out_close(void);

/* True if this binary was built with ALSA MIDI support. */
bool midi_out_available(void);

/* Parse the patch table of the song at [offset, offset+size) in f, so note-on
 * fingerprinting can recover each channel's GM program.  Call after loading a
 * song (and before/at the first register writes). */
void midi_out_set_song(FILE *f, unsigned int offset, unsigned int size);

/* Tap point: feed every OPL register write here (from the opl_write fan-out). */
void midi_feed(unsigned int reg, unsigned char val);

/* All-notes-off + forget per-channel state.  Call on OPL reset / song change. */
void midi_reset(void);

/* Silence everything immediately (e.g. on quit or when muting MIDI). */
void midi_panic(void);

/* Mute/unmute output without closing the port; tracking continues so toggling
 * mid-song stays coherent.  Muting sends an all-notes-off. */
void midi_out_set_enabled(bool on);
bool midi_out_enabled(void);

#endif /* MIDI_OUT_H */
