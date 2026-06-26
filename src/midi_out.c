/* See midi_out.h for the design.  This file holds both the ALSA sequencer
 * backend and the OPL-register -> General MIDI tracker. */
#include "midi_out.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
static snd_seq_t *seq = NULL;
static int        my_port = -1;
#endif

/* The OPL channel -> first-operator register offset, same table lds_play.c uses. */
static const unsigned char op_table[9] =
	{0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12};

/* The OPL master sample clock (Hz) used to turn F-Number/block into a frequency. */
#define OPL_FSAMP 49716.0

/* Percussion convention: an LDS patch whose midinst >= 128 is a drum; the GM
 * channel-10 note is (midinst - 128) (e.g. 168 -> 40 snare, 170 -> 42 hi-hat). */
#define DRUM_BASE     128
#define MIDI_DRUM_CH  9
#define DEFAULT_VELO  100

/* ------------------------------------------------------------------ */
/* Patch table parsed from the .mus song (for note-on instrument ID).  */
/* ------------------------------------------------------------------ */

#define MAX_PATCH 256

typedef struct {
	/* Fingerprint: the stable (non-volume) operator bytes the driver writes in
	 * lds_playsound — mod_misc, mod_ad, mod_sr, mod_wave, car_misc, car_ad,
	 * car_sr, car_wave, feedback.  Volume/KSL (0x40/0x43) are excluded: tremolo
	 * and master fade modulate them, so they aren't stable per instrument. */
	unsigned char fp[9];
	unsigned char midinst, midvelo, midkey;
	signed char   midtrans;
} Patch;

static Patch patches[MAX_PATCH];
static int   num_patch = 0;

/* ------------------------------------------------------------------ */
/* Tracker state.                                                      */
/* ------------------------------------------------------------------ */

static unsigned char shadow[256];   /* every register the driver has written  */
static bool          opened  = false;
static bool          enabled = true;

typedef struct {
	bool active;
	int  midi_ch;    /* where the sounding note was sent (so note-off matches)  */
	int  note;
} Voice;

static Voice voice[9];
static int   chan_program[16];      /* last program-change sent per MIDI channel */

/* ------------------------------------------------------------------ */
/* ALSA emit primitives (no-ops without ALSA, or while muted/closed).  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_ALSA
static void send_ev(snd_seq_event_t *ev)
{
	snd_seq_ev_set_source(ev, my_port);
	snd_seq_ev_set_subs(ev);
	snd_seq_ev_set_direct(ev);
	snd_seq_event_output(seq, ev);
	snd_seq_drain_output(seq);
}
#endif

static void emit_note_on(int ch, int note, int vel)
{
	if (!opened || !enabled) return;
#ifdef HAVE_ALSA
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);
	snd_seq_ev_set_noteon(&ev, ch, note, vel);
	send_ev(&ev);
#else
	(void)ch; (void)note; (void)vel;
#endif
}

static void emit_note_off(int ch, int note)
{
	if (!opened || !enabled) return;
#ifdef HAVE_ALSA
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);
	snd_seq_ev_set_noteoff(&ev, ch, note, 0);
	send_ev(&ev);
#else
	(void)ch; (void)note;
#endif
}

static void emit_program(int ch, int prog)
{
	if (!opened || !enabled) return;
#ifdef HAVE_ALSA
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);
	snd_seq_ev_set_pgmchange(&ev, ch, prog);
	send_ev(&ev);
#else
	(void)ch; (void)prog;
#endif
}

static void emit_all_notes_off(void)
{
	if (!opened) return;
#ifdef HAVE_ALSA
	for (int ch = 0; ch < 16; ch++) {
		snd_seq_event_t ev;
		snd_seq_ev_clear(&ev);
		snd_seq_ev_set_controller(&ev, ch, 123 /* All Notes Off */, 0);
		send_ev(&ev);
	}
#endif
}

/* ------------------------------------------------------------------ */
/* OPL pitch -> MIDI note number.                                      */
/* ------------------------------------------------------------------ */

static int fnum_block_to_midi(unsigned int fnum, unsigned int block)
{
	if (fnum == 0) return -1;
	double hz = (double)fnum * OPL_FSAMP / (double)(1u << (20 - block));
	long note = lround(69.0 + 12.0 * log2(hz / 440.0));
	if (note < 0)   note = 0;
	if (note > 127) note = 127;
	return (int)note;
}

/* ------------------------------------------------------------------ */
/* Instrument identification by register fingerprint.                  */
/* ------------------------------------------------------------------ */

static int identify_patch(int ch)
{
	unsigned char op = op_table[ch];
	unsigned char fp[9] = {
		shadow[0x20 + op], shadow[0x60 + op], shadow[0x80 + op], shadow[0xe0 + op],
		shadow[0x23 + op], shadow[0x63 + op], shadow[0x83 + op], shadow[0xe3 + op],
		shadow[0xc0 + ch],
	};
	for (int i = 0; i < num_patch; i++)
		if (memcmp(fp, patches[i].fp, sizeof fp) == 0)
			return i;
	return -1;   /* unknown — keep whatever program is current on the channel */
}

/* ------------------------------------------------------------------ */
/* Note on/off, driven by 0xB0 key-bit edges.                          */
/* ------------------------------------------------------------------ */

static void note_off(int ch)
{
	if (!voice[ch].active) return;
	emit_note_off(voice[ch].midi_ch, voice[ch].note);
	voice[ch].active = false;
}

static void note_on(int ch)
{
	/* A retrigger arrives as key-off then key-on; if a previous note is somehow
	 * still marked active, release it first so nothing hangs. */
	note_off(ch);

	int idx = identify_patch(ch);
	int midi_ch, note, vel = DEFAULT_VELO;

	if (idx >= 0 && patches[idx].midinst >= DRUM_BASE) {
		/* Percussion: fixed GM channel-10 note, pitch from the OPL is ignored. */
		midi_ch = MIDI_DRUM_CH;
		note = patches[idx].midinst - DRUM_BASE;
		if (patches[idx].midvelo) vel = patches[idx].midvelo;
	} else {
		unsigned int fnum  = ((shadow[0xb0 + ch] & 0x03) << 8) | shadow[0xa0 + ch];
		unsigned int block = (shadow[0xb0 + ch] >> 2) & 0x07;
		note = fnum_block_to_midi(fnum, block);
		if (note < 0) return;                    /* no pitch latched yet */
		midi_ch = ch;                            /* OPL ch 0..8 -> MIDI ch 0..8 */

		if (idx >= 0) {
			/* Tyrian's per-patch MIDI transpose (`midtrans`).  The original DOS
			 * game applied it only in General-MIDI mode, never to the OPL pitch
			 * (the OPL path ignores it), so it's purely additive on top of the
			 * sounding pitch we derived above.  It's mostly octave multiples that
			 * drop the GM instrument into the register the composer intended. */
			note += patches[idx].midtrans;
			if (note < 0)   note = 0;
			if (note > 127) note = 127;
			if (patches[idx].midvelo) vel = patches[idx].midvelo;
			if (chan_program[midi_ch] != patches[idx].midinst) {
				emit_program(midi_ch, patches[idx].midinst);
				chan_program[midi_ch] = patches[idx].midinst;
			}
		}
	}

	if (vel > 127) vel = 127;
	emit_note_on(midi_ch, note, vel);
	voice[ch].active  = true;
	voice[ch].midi_ch = midi_ch;
	voice[ch].note    = note;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

void midi_feed(unsigned int reg, unsigned char val)
{
	if (!opened || reg > 0xff) return;

	unsigned char old = shadow[reg];
	shadow[reg] = val;

	if (reg >= 0xb0 && reg <= 0xb8) {
		int ch = (int)(reg - 0xb0);
		bool was_on = old & 0x20;
		bool now_on = val & 0x20;
		if (now_on && !was_on)      note_on(ch);
		else if (!now_on && was_on) note_off(ch);
		/* key held with a new F-Number (vibrato/glide/arpeggio): left as-is —
		 * the MIDI note keeps its original pitch (no pitch-bend in this version). */
	}
}

void midi_reset(void)
{
	emit_all_notes_off();
	for (int i = 0; i < 9; i++) voice[i].active = false;
	for (int i = 0; i < 16; i++) chan_program[i] = -1;
	memset(shadow, 0, sizeof shadow);
}

void midi_panic(void)
{
	emit_all_notes_off();
	for (int i = 0; i < 9; i++) voice[i].active = false;
}

/* ------------------------------------------------------------------ */
/* Patch-table parse (mirrors the header lds_load reads, fields only).  */
/* ------------------------------------------------------------------ */

#define PATCH_RECORD_BYTES 46

void midi_out_set_song(FILE *f, unsigned int offset, unsigned int size)
{
	(void)size;
	num_patch = 0;
	if (!f) return;

	long saved = ftell(f);

	/* Header: mode(1) speed(2) tempo(1) pattlen(1) chandelay(9) regbd(1)
	 * numpatch(2), then numpatch * 46-byte patch records. */
	if (fseek(f, (long)offset + 1 + 2 + 1 + 1 + 9 + 1, SEEK_SET) != 0) goto done;

	unsigned char nb[2];
	if (fread(nb, 1, 2, f) != 2) goto done;
	int n = nb[0] | (nb[1] << 8);
	if (n < 0) n = 0;
	if (n > MAX_PATCH) n = MAX_PATCH;

	for (int i = 0; i < n; i++) {
		unsigned char r[PATCH_RECORD_BYTES];
		if (fread(r, 1, sizeof r, f) != sizeof r) { num_patch = i; goto done; }
		Patch *p = &patches[i];
		p->fp[0] = r[0];   /* mod_misc */
		p->fp[1] = r[2];   /* mod_ad   */
		p->fp[2] = r[3];   /* mod_sr   */
		p->fp[3] = r[4];   /* mod_wave */
		p->fp[4] = r[5];   /* car_misc */
		p->fp[5] = r[7];   /* car_ad   */
		p->fp[6] = r[8];   /* car_sr   */
		p->fp[7] = r[9];   /* car_wave */
		p->fp[8] = r[10];  /* feedback */
		p->midinst  = r[40];
		p->midvelo  = r[41];
		p->midkey   = r[42];
		p->midtrans = (signed char)r[43];
	}
	num_patch = n;

done:
	fseek(f, saved, SEEK_SET);
	/* New song: drop stale per-channel program/voice state. */
	for (int i = 0; i < 9; i++)  voice[i].active = false;
	for (int i = 0; i < 16; i++) chan_program[i] = -1;
}

/* ------------------------------------------------------------------ */
/* Open / close / status.                                             */
/* ------------------------------------------------------------------ */

bool midi_out_available(void)
{
#ifdef HAVE_ALSA
	return true;
#else
	return false;
#endif
}

void midi_out_set_enabled(bool on)
{
	if (!on && enabled) emit_all_notes_off();
	enabled = on;
}

bool midi_out_enabled(void)
{
	return opened && enabled;
}

bool midi_out_open(const char *port_spec)
{
#ifdef HAVE_ALSA
	if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
		fprintf(stderr, "MIDI: cannot open ALSA sequencer\n");
		seq = NULL;
		return false;
	}
	snd_seq_set_client_name(seq, "tyrian-retrowave");
	my_port = snd_seq_create_simple_port(
		seq, "MIDI out",
		SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
		SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
	if (my_port < 0) {
		fprintf(stderr, "MIDI: cannot create output port\n");
		snd_seq_close(seq);
		seq = NULL;
		return false;
	}

	if (port_spec && *port_spec && strcmp(port_spec, "-") != 0) {
		snd_seq_addr_t addr;
		if (snd_seq_parse_address(seq, &addr, port_spec) == 0) {
			if (snd_seq_connect_to(seq, my_port, addr.client, addr.port) < 0)
				fprintf(stderr, "MIDI: could not connect to '%s' "
				                "(connect manually with aconnect)\n", port_spec);
			else
				fprintf(stderr, "MIDI: connected to %d:%d (%s)\n",
				        addr.client, addr.port, port_spec);
		} else {
			fprintf(stderr, "MIDI: '%s' is not a valid port; leaving unconnected\n",
			        port_spec);
		}
	}

	opened = true;
	enabled = true;
	midi_reset();
	return true;
#else
	(void)port_spec;
	fprintf(stderr, "MIDI: this build has no ALSA support "
	                "(rebuild with libasound2-dev installed)\n");
	return false;
#endif
}

void midi_out_close(void)
{
	if (!opened) return;
	midi_panic();
#ifdef HAVE_ALSA
	if (seq) {
		snd_seq_close(seq);
		seq = NULL;
	}
	my_port = -1;
#endif
	opened = false;
}
