/*
 * rwgui.c — graphical Tyrian music player for the RetroWave OPL3 Express.
 *
 * Runs Tyrian's real LDS driver (lds_play.c), the software OPL emulator
 * (emu/opl.c), and streams to the board (retrowave_serial.c) -- all fed from a
 * single register stream via the rwgui_opl_* hooks. The same stream drives a
 * channel-activity visualizer (bars, like an old synth).
 *
 * Audio + timing follow the game's loudness.c: the SDL audio callback clocks
 * the LDS sequencer at 69.5 Hz from the sample counter and renders emulator
 * samples for PC output. Output is selectable: PC / Board / Both.
 *
 * SDL2 only (+ an embedded bitmap font). No other dependencies.
 */
#include <SDL.h>

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lds_play.h"
#include "file.h"
#include "opl.h"                 // emu/opl.h (hooked): opl_init/opl_write -> rwgui_opl_*
#include "retrowave_serial.h"
#include "font5x7.h"

/* ------------------------------------------------------------------ */
/* Shims the LDS driver expects.                                       */
/* ------------------------------------------------------------------ */

int audioSampleRate = 44100;
extern const unsigned char op_table[9];   // operator offsets, from lds_play.c

void fread_die(void *buffer, size_t size, size_t count, FILE *stream)
{
	if (fread(buffer, size, count, stream) != count)
	{
		fprintf(stderr, "error: unexpected end of file while reading music\n");
		exit(EXIT_FAILURE);
	}
}

/* ------------------------------------------------------------------ */
/* Output routing + shared state (audio thread <-> render thread).     */
/* ------------------------------------------------------------------ */

static bool out_pc = true;       // play emulator on PC speakers
static bool out_board = false;   // stream to the RetroWave board

static volatile uint8_t fmchip[256];          // register shadow for the visualizer
static volatile int g_song_ended = 0;         // set by audio thread, cleared by main

// OPL fan-out: emulator (always, for timing + PC audio) + board + shadow.
void rwgui_opl_init(void)
{
	adlib_init((Bit32u)audioSampleRate);
	memset((void *)fmchip, 0, sizeof(fmchip));
}

void rwgui_opl_write(unsigned int reg, Bit8u val)
{
	adlib_write(reg, val);
	if (out_board && retrowave_active)
		retrowave_write(reg, val);
	if (reg < 256)
		fmchip[reg] = val;
}

/* ------------------------------------------------------------------ */
/* Song bank (MUSIC.MUS).                                              */
/* ------------------------------------------------------------------ */

static FILE *music_file;
static Uint32 *song_offset;
static Uint16 song_count;

// Tyrian music titles (from OpenTyrian2000's musmast.c); cosmetic.
static const char *const SONG_TITLE[] = {
	"Asteroid Dance Part 2", "Asteroid Dance Part 1", "Buy/Sell Music",
	"CAMANIS", "CAMANISE", "Deli Shop Quartet", "Deli Shop Quartet No. 2",
	"Ending Number 1", "Ending Number 2", "End of Level", "Game Over Solo",
	"Gryphons of the West", "Somebody pick up the Gryphone",
	"Gyges, Will You Please Help Me?", "I speak Gygese", "Halloween Ramble",
	"Tunneling Trolls", "Tyrian, The Level", "The MusicMan", "The Navigator",
	"Come Back to Me, Savara", "Come Back again to Savara", "Space Journey 1",
	"Space Journey 2", "The final edge", "START5", "Parlance",
	"Torm - The Gathering", "TRANSON", "Tyrian: The Song", "ZANAC3", "ZANACS",
	"Return me to Savara", "High Score Table", "One Mustn't Fall",
	"Sarah's Song", "A Field for Mag", "Rock Garden", "Quest for Peace",
	"Composition in Q", "BEER",
};
#define SONG_TITLE_COUNT ((int)(sizeof(SONG_TITLE) / sizeof(SONG_TITLE[0])))

static long file_eof(FILE *f)
{
	long pos = ftell(f);
	fseek(f, 0, SEEK_END);
	long eof = ftell(f);
	fseek(f, pos, SEEK_SET);
	return eof;
}

static bool load_bank(const char *path)
{
	music_file = fopen(path, "rb");
	if (!music_file) { fprintf(stderr, "error: cannot open '%s'\n", path); return false; }
	fread_u16_die(&song_count, 1, music_file);
	song_offset = malloc((song_count + 1) * sizeof(*song_offset));
	fread_u32_die(song_offset, song_count, music_file);
	song_offset[song_count] = file_eof(music_file);
	return true;
}

static void load_song(unsigned int n)
{
	lds_load(music_file, song_offset[n], song_offset[n + 1] - song_offset[n]);
}

static const char *song_name(int i)
{
	return (i < SONG_TITLE_COUNT) ? SONG_TITLE[i] : "Unknown";
}

/* ------------------------------------------------------------------ */
/* Playback / timing (mirrors loudness.c, tempo-scalable).             */
/* ------------------------------------------------------------------ */

static SDL_AudioDeviceID audio_dev;
static bool music_stopped = true;
static bool paused = false;
static bool loop = false;
static int tempo_pct = 100;
static int cur_song = 0;

static double samples_per_update;   // recomputed on tempo change
static double samples_until_update;

static void recompute_timing(void)
{
	double rate = 69.5 * (tempo_pct / 100.0);
	samples_per_update = audioSampleRate / rate;
}

static void audio_callback(void *ud, Uint8 *stream, int len)
{
	(void)ud;
	Sint16 *out = (Sint16 *)stream;
	int n = len / (int)sizeof(Sint16);

	if (music_stopped || paused)
	{
		memset(stream, 0, len);
		return;
	}

	Sint16 *p = out;
	int rem = n;
	while (rem > 0)
	{
		if (samples_until_update < 1.0)
		{
			lds_update();   // emits register writes through the hook
			if (!playing || (!loop && songlooped))
				g_song_ended = 1;
			samples_until_update += samples_per_update;
		}
		int count = (int)samples_until_update;
		if (count > rem) count = rem;
		if (count < 1) count = 1;
		adlib_getsample(p, count);
		p += count;
		rem -= count;
		samples_until_update -= count;
	}

	if (!out_pc)
	{
		memset(stream, 0, len);   // board-only: silence PC, keep timing/visualizer
	}
	else
	{
		for (int i = 0; i < n; ++i)   // emulator is quiet; ~2x gain like the game
		{
			int s = out[i] * 2;
			out[i] = (Sint16)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
		}
	}
}

static void play_index(int i)
{
	if (i < 0) i = song_count - 1;
	if (i >= song_count) i = 0;
	cur_song = i;
	SDL_LockAudioDevice(audio_dev);
	load_song(i);
	samples_until_update = 0;
	g_song_ended = 0;
	music_stopped = false;
	paused = false;
	SDL_UnlockAudioDevice(audio_dev);
}

/* ------------------------------------------------------------------ */
/* Visualizer.                                                         */
/* ------------------------------------------------------------------ */

#define NCH 9

typedef struct { float level, peak, hue; bool on; } VizChan;
static VizChan viz[NCH];

static void viz_update(void)
{
	for (int ch = 0; ch < NCH; ++ch)
	{
		// Real per-channel output level (peak |cval| since last frame): tracks
		// loudness, envelope and silence, so bars move with the actual sound
		// and a keyed-but-inaudible channel reads ~0.
		float amp = (float)opl_channel_level(ch);
		amp = powf(amp, 0.55f);   // perceptual curve: use the meter's full range

		int fnum = fmchip[0xA0 + ch] | ((fmchip[0xB0 + ch] & 3) << 8);
		int block = (fmchip[0xB0 + ch] >> 2) & 7;
		float pitch = (block * 1024 + fnum) / (8.0f * 1024.0f);   // 0..1

		VizChan *v = &viz[ch];
		v->on = amp > 0.02f;
		v->level += (amp - v->level) * (amp > v->level ? 0.6f : 0.20f);   // snappy
		if (v->level < 0) v->level = 0;
		if (v->level > v->peak) v->peak = v->level;
		else v->peak -= 0.012f;
		if (v->peak < v->level) v->peak = v->level;
		if (v->on) v->hue = pitch;
	}
}

/* ------------------------------------------------------------------ */
/* Drawing helpers.                                                    */
/* ------------------------------------------------------------------ */

typedef struct { Uint8 r, g, b; } RGB;

static RGB hsv(float h, float s, float v)   // h,s,v in 0..1
{
	h = fmodf(h, 1.0f); if (h < 0) h += 1.0f;
	float i = floorf(h * 6.0f);
	float f = h * 6.0f - i;
	float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
	float r, g, b;
	switch (((int)i) % 6)
	{
	case 0: r = v; g = t; b = p; break;
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	default: r = v; g = p; b = q; break;
	}
	RGB c = { (Uint8)(r * 255), (Uint8)(g * 255), (Uint8)(b * 255) };
	return c;
}

static void draw_text(SDL_Renderer *ren, int x, int y, int s, RGB c, const char *txt)
{
	SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
	int cx = x;
	for (const char *p = txt; *p; ++p)
	{
		const uint8_t *g = font_glyph(*p);
		if (g)
			for (int ry = 0; ry < FONT_H; ++ry)
				for (int rx = 0; rx < FONT_W; ++rx)
					if (g[ry] & (1 << (FONT_W - 1 - rx)))
					{
						SDL_Rect q = { cx + rx * s, y + ry * s, s, s };
						SDL_RenderFillRect(ren, &q);
					}
		cx += (FONT_W + 1) * s;
	}
}

static int text_w(const char *t, int s) { return (int)strlen(t) * (FONT_W + 1) * s; }

/* ------------------------------------------------------------------ */
/* Visualizer styles.                                                  */
/* ------------------------------------------------------------------ */

enum { STYLE_LED, STYLE_NEON, STYLE_SPECTRUM, STYLE_COUNT };
static int style = STYLE_LED;

static void fill(SDL_Renderer *r, int x, int y, int w, int h, RGB c, Uint8 a)
{
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, a);
	SDL_Rect q = { x, y, w, h };
	SDL_RenderFillRect(r, &q);
}

static void draw_bars(SDL_Renderer *ren, int ax, int ay, int aw, int ah)
{
	int gap = aw / (NCH * 6);
	if (gap < 2) gap = 2;
	int slot = (aw - gap) / NCH;
	int bw = slot - gap;

	for (int ch = 0; ch < NCH; ++ch)
	{
		int x = ax + gap + ch * slot;
		float lv = viz[ch].level; if (lv > 1) lv = 1;
		float pk = viz[ch].peak;  if (pk > 1) pk = 1;
		int bh = (int)(lv * ah);
		int by = ay + ah - bh;

		if (style == STYLE_LED)
		{
			int segs = 20, seg_h = ah / segs;
			int lit = (int)(lv * segs + 0.5f);
			int pkseg = (int)(pk * segs + 0.5f);
			for (int s = 0; s < segs; ++s)
			{
				int sy = ay + ah - (s + 1) * seg_h + 1;
				RGB col = (s < segs * 0.6f) ? (RGB){ 40, 220, 70 }
				        : (s < segs * 0.85f) ? (RGB){ 240, 200, 40 }
				                             : (RGB){ 240, 60, 40 };
				bool islit = s < lit;
				bool ispk = (s + 1 == pkseg);
				if (ispk)        fill(ren, x, sy, bw, seg_h - 2, (RGB){ 255, 255, 255 }, 255);
				else if (islit)  fill(ren, x, sy, bw, seg_h - 2, col, 255);
				else             fill(ren, x, sy, bw, seg_h - 2, col, 28);
			}
		}
		else if (style == STYLE_NEON)
		{
			// soft additive glow behind the bar
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_ADD);
			for (int g = 3; g >= 1; --g)
				fill(ren, x - g * 4, by - g * 4, bw + g * 8, bh + g * 4,
				     (RGB){ 60, 120, 255 }, (Uint8)(18 * viz[ch].level));
			// vertical gradient cyan(bottom) -> magenta(top)
			int slices = bh / 3 + 1;
			for (int s = 0; s < slices; ++s)
			{
				float t = (float)s / (slices > 1 ? slices - 1 : 1);
				RGB c = { (Uint8)(40 + t * 215), (Uint8)(220 - t * 140), (Uint8)(255) };
				fill(ren, x, by + bh - (s + 1) * 3, bw, 3, c, 255);
			}
			SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
			fill(ren, x, ay + ah - (int)(pk * ah) - 2, bw, 3, (RGB){ 255, 255, 255 }, 220);
		}
		else /* STYLE_SPECTRUM */
		{
			int tw = bw * 6 / 10, tx = x + (bw - tw) / 2;
			RGB c = hsv(0.62f - viz[ch].hue * 0.62f, 0.85f, 1.0f);   // pitch -> color
			fill(ren, tx, by, tw, bh, c, 255);
			fill(ren, tx, ay + ah - (int)(pk * ah) - 2, tw, 3, (RGB){ 255, 255, 255 }, 230);
		}

		// channel number label under each bar
		char lbl[4]; snprintf(lbl, sizeof lbl, "%d", ch + 1);
		draw_text(ren, x + bw / 2 - text_w(lbl, 2) / 2, ay + ah + 8, 2,
		          viz[ch].on ? (RGB){ 230, 230, 230 } : (RGB){ 90, 90, 110 }, lbl);
	}
}

/* ------------------------------------------------------------------ */

static void output_label(char *buf, size_t n)
{
	const char *m = out_pc && out_board ? "BOTH" : out_board ? "BOARD" : "PC";
	snprintf(buf, n, "%s", m);
}

// Key-off everything on the board so notes don't hang when we stop feeding it.
static void board_silence(void)
{
	if (!retrowave_active) return;
	for (int ch = 0; ch < 9; ++ch)
		retrowave_write(0xB0 + ch, fmchip[0xB0 + ch] & ~0x20);
	retrowave_write(0xBD, fmchip[0xBD] & ~0x1f);   // percussion off
}

// Push the full current register state to the board (used when re-enabling it).
static void board_resync(void)
{
	if (!retrowave_active) return;
	for (int r = 0; r < 256; ++r)
		retrowave_write(r, fmchip[r]);
}

static void cycle_output(void)
{
	// PC -> BOARD -> BOTH -> PC  (BOARD/BOTH only if a board is open).
	// Lock the audio device so the callback can't write to the board while we
	// silence/resync it (avoids interleaved serial frames).
	SDL_LockAudioDevice(audio_dev);
	bool was_board = out_board;
	if (out_pc && !out_board)      { out_pc = false; out_board = true; }
	else if (!out_pc && out_board) { out_pc = true;  out_board = true; }
	else                           { out_pc = true;  out_board = false; }
	if (out_board && !retrowave_active) { out_board = false; out_pc = true; }  // no board

	if (was_board && !out_board) board_silence();      // stopping feed -> kill notes
	else if (!was_board && out_board) board_resync();  // resuming feed -> restore state
	SDL_UnlockAudioDevice(audio_dev);
}

/* ------------------------------------------------------------------ */
/* Actions (shared by keyboard and on-screen buttons).                 */
/* ------------------------------------------------------------------ */

enum { ACT_PREV, ACT_PLAY, ACT_NEXT, ACT_RESTART, ACT_LOOP,
       ACT_OUTPUT, ACT_STYLE, ACT_TDN, ACT_TUP };

static void do_action(int a)
{
	switch (a)
	{
	case ACT_PREV:    play_index(cur_song - 1); break;
	case ACT_NEXT:    play_index(cur_song + 1); break;
	case ACT_PLAY:    paused = !paused; break;
	case ACT_RESTART: SDL_LockAudioDevice(audio_dev); lds_rewind();
	                  samples_until_update = 0; SDL_UnlockAudioDevice(audio_dev); break;
	case ACT_LOOP:    loop = !loop; break;
	case ACT_OUTPUT:  cycle_output(); break;
	case ACT_STYLE:   style = (style + 1) % STYLE_COUNT; break;
	case ACT_TUP:     if (tempo_pct < 300) tempo_pct += 10; recompute_timing(); break;
	case ACT_TDN:     if (tempo_pct > 20)  tempo_pct -= 10; recompute_timing(); break;
	}
}

// On-screen buttons (immediate-mode): laid out + drawn each frame, hit-tested
// against the previous frame's rects (layout only changes on resize).
typedef struct { SDL_Rect r; int act; } Button;
static Button g_btn[16];
static int g_nbtn;

static int add_button(SDL_Renderer *ren, int x, int y, int h, const char *label,
                      int act, int mx, int my)
{
	int s = 2, pad = 6, w = text_w(label, s) + pad * 2;
	bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
	fill(ren, x, y, w, h, hover ? (RGB){ 58, 64, 96 } : (RGB){ 34, 36, 54 }, 255);
	SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
	SDL_Rect b = { x, y, w, h };
	SDL_RenderDrawRect(ren, &b);
	draw_text(ren, x + pad, y + (h - FONT_H * s) / 2, s, (RGB){ 225, 230, 250 }, label);
	if (g_nbtn < (int)(sizeof(g_btn) / sizeof(g_btn[0])))
	{
		g_btn[g_nbtn].r = b;
		g_btn[g_nbtn].act = act;
		g_nbtn++;
	}
	return x + w + 6;
}

// Filled triangle within (x,y,w,h); dir>0 points right, dir<0 points left.
static void fill_tri(SDL_Renderer *r, int x, int y, int w, int h, int dir, RGB c)
{
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
	for (int ry = 0; ry < h; ++ry)
	{
		float t = 1.0f - fabsf((ry - h / 2.0f) / (h / 2.0f));
		int len = (int)(w * t + 0.5f);
		if (len < 1) len = 1;
		int lx = (dir > 0) ? x : x + (w - len);
		SDL_Rect q = { lx, y + ry, len, 1 };
		SDL_RenderFillRect(r, &q);
	}
}

enum { ICON_PREV, ICON_PLAY, ICON_PAUSE, ICON_NEXT };

// Draw a transport icon inside a centered square of side sz at (x,y).
static void draw_icon(SDL_Renderer *ren, int type, int x, int y, int sz, RGB c)
{
	int g = sz * 6 / 10, gx = x + (sz - g) / 2, gy = y + (sz - g) / 2;
	int bar = g / 4; if (bar < 2) bar = 2;
	switch (type)
	{
	case ICON_PLAY:  fill_tri(ren, gx + g / 8, gy, g - g / 8, g, +1, c); break;
	case ICON_PAUSE: fill(ren, gx + g / 8, gy, bar, g, c, 255);
	                 fill(ren, gx + g - g / 8 - bar, gy, bar, g, c, 255); break;
	case ICON_PREV:  fill(ren, gx, gy, bar, g, c, 255);
	                 fill_tri(ren, gx + bar + 1, gy, g - bar - 1, g, -1, c); break;
	case ICON_NEXT:  fill_tri(ren, gx, gy, g - bar - 1, g, +1, c);
	                 fill(ren, gx + g - bar, gy, bar, g, c, 255); break;
	}
}

static int add_icon_button(SDL_Renderer *ren, int x, int y, int h, int type, int act, int mx, int my)
{
	int w = h + 6;
	bool hover = mx >= x && mx < x + w && my >= y && my < y + h;
	fill(ren, x, y, w, h, hover ? (RGB){ 58, 64, 96 } : (RGB){ 34, 36, 54 }, 255);
	SDL_SetRenderDrawColor(ren, 95, 105, 150, 255);
	SDL_Rect b = { x, y, w, h };
	SDL_RenderDrawRect(ren, &b);
	draw_icon(ren, type, x + (w - h) / 2, y, h, (RGB){ 225, 230, 250 });
	if (g_nbtn < (int)(sizeof(g_btn) / sizeof(g_btn[0])))
	{
		g_btn[g_nbtn].r = b; g_btn[g_nbtn].act = act; g_nbtn++;
	}
	return x + w + 6;
}

static void draw_controls(SDL_Renderer *ren, int W, int H)
{
	(void)W;
	static const char *STYLE_SHORT[] = { "LED", "NEON", "SPEC" };
	int mx, my;
	SDL_GetMouseState(&mx, &my);
	int by = H - 38, bh = 28, bx = 12;
	char b[32];
	g_nbtn = 0;
	bx = add_icon_button(ren, bx, by, bh, ICON_PREV, ACT_PREV, mx, my);
	bx = add_icon_button(ren, bx, by, bh, paused ? ICON_PLAY : ICON_PAUSE, ACT_PLAY, mx, my);
	bx = add_icon_button(ren, bx, by, bh, ICON_NEXT, ACT_NEXT, mx, my);
	bx = add_button(ren, bx, by, bh, "RESTART", ACT_RESTART, mx, my);
	snprintf(b, sizeof b, "LOOP:%s", loop ? "ON" : "OFF");
	bx = add_button(ren, bx, by, bh, b, ACT_LOOP, mx, my);
	char out[8]; output_label(out, sizeof out);
	bx = add_button(ren, bx, by, bh, out, ACT_OUTPUT, mx, my);
	bx = add_button(ren, bx, by, bh, STYLE_SHORT[style], ACT_STYLE, mx, my);
	bx = add_button(ren, bx, by, bh, "T-", ACT_TDN, mx, my);
	snprintf(b, sizeof b, "%d%%", tempo_pct);
	draw_text(ren, bx + 2, by + (bh - FONT_H * 2) / 2, 2, (RGB){ 150, 170, 210 }, b);
	bx += text_w(b, 2) + 8;
	add_button(ren, bx, by, bh, "T+", ACT_TUP, mx, my);
}

static volatile sig_atomic_t g_quit = 0;
static void on_signal(int s) { (void)s; g_quit = 1; }

static void handle_click(int x, int y)
{
	for (int i = 0; i < g_nbtn; ++i)
		if (x >= g_btn[i].r.x && x < g_btn[i].r.x + g_btn[i].r.w &&
		    y >= g_btn[i].r.y && y < g_btn[i].r.y + g_btn[i].r.h)
		{
			do_action(g_btn[i].act);
			return;
		}
}

int main(int argc, char *argv[])
{
	const char *dev = "ttyACM0";
	const char *bank = NULL;
	int start = 1;
	const char *shot_path = NULL;   // --shot FILE: save a BMP then quit (testing)
	int shot_frame = 160;

	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "-d") && i + 1 < argc) dev = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) start = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!strcmp(argv[i], "--shot-frame") && i + 1 < argc) shot_frame = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--style") && i + 1 < argc) style = atoi(argv[++i]) % STYLE_COUNT;
		else if (!strcmp(argv[i], "-l")) loop = true;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
		{
			printf("Usage: %s [-d DEV] [-s N] [-l] [MUSIC.MUS]\n"
			       "  -d DEV   RetroWave serial device (default ttyACM0; - = no board)\n"
			       "  -s N     start at song N\n  -l       loop\n", argv[0]);
			return 0;
		}
		else if (argv[i][0] != '-') bank = argv[i];
	}
	if (!bank)
	{
		if      (access("music.mus", R_OK) == 0) bank = "music.mus";
		else if (access("MUSIC.MUS", R_OK) == 0) bank = "MUSIC.MUS";
		else { fprintf(stderr, "error: no MUSIC.MUS given/found\n"); return 1; }
	}

	font_init();
	if (!load_bank(bank)) return 1;

	// Open the board (optional). Default to board-only output if present.
	if (strcmp(dev, "-") != 0 && retrowave_open(dev)) { out_board = true; out_pc = false; }
	else { out_board = false; out_pc = true; }

	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
	{
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 1;
	want.samples = 1024; want.callback = audio_callback;
	audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!audio_dev) { fprintf(stderr, "SDL audio: %s\n", SDL_GetError()); return 1; }
	audioSampleRate = have.freq;
	recompute_timing();

	SDL_Window *win = SDL_CreateWindow("Tyrian \xc2\xb7 RetroWave OPL3",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 373,
		SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
		SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

	if (start < 1) start = 1;
	if (start > song_count) start = song_count;
	play_index(start - 1);
	SDL_PauseAudioDevice(audio_dev, 0);

	bool running = true;
	long frame = 0;
	while (running && !g_quit)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_QUIT) running = false;
			else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT)
				handle_click(e.button.x, e.button.y);
			else if (e.type == SDL_KEYDOWN)
			{
				switch (e.key.keysym.sym)
				{
				case SDLK_q: case SDLK_ESCAPE: running = false; break;
				case SDLK_SPACE: do_action(ACT_PLAY); break;
				case SDLK_n: case SDLK_RIGHT: do_action(ACT_NEXT); break;
				case SDLK_p: case SDLK_LEFT:  do_action(ACT_PREV); break;
				case SDLK_r: SDL_LockAudioDevice(audio_dev); lds_rewind();
				             samples_until_update = 0; SDL_UnlockAudioDevice(audio_dev); break;
				case SDLK_l: do_action(ACT_LOOP); break;
				case SDLK_o: do_action(ACT_OUTPUT); break;
				case SDLK_v: do_action(ACT_STYLE); break;
				case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS:
					if (tempo_pct < 300) tempo_pct += 10;
					recompute_timing(); break;
				case SDLK_MINUS: case SDLK_KP_MINUS:
					if (tempo_pct > 20) tempo_pct -= 10;
					recompute_timing(); break;
				}
			}
		}

		if (g_song_ended)
		{
			g_song_ended = 0;
			if (!loop) play_index(cur_song + 1);
		}

		viz_update();

		int W, H;
		SDL_GetRendererOutputSize(ren, &W, &H);

		// background
		fill(ren, 0, 0, W, H, (RGB){ 12, 12, 20 }, 255);
		fill(ren, 0, 0, W, 38, (RGB){ 22, 22, 38 }, 255);

		// header: song number + title
		char line[160];
		snprintf(line, sizeof line, "%02d/%02d  %s%s", cur_song + 1, song_count,
		         song_name(cur_song), paused ? "   -PAUSED-" : "");
		draw_text(ren, 12, 12, 2, (RGB){ 240, 240, 255 }, line);

		// visualizer area
		int ax = 18, ay = 50, aw = W - 36, ah = H - 50 - 64;
		fill(ren, ax - 6, ay - 6, aw + 12, ah + 12 + 26, (RGB){ 8, 8, 14 }, 255);
		draw_bars(ren, ax, ay, aw, ah);

		// clickable control bar
		draw_controls(ren, W, H);

		SDL_RenderPresent(ren);

		if (shot_path && ++frame >= shot_frame)
		{
			SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
			if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
			{
				SDL_SaveBMP(s, shot_path);
				fprintf(stderr, "saved screenshot: %s\n", shot_path);
			}
			if (s) SDL_FreeSurface(s);
			running = false;
		}
	}

	SDL_PauseAudioDevice(audio_dev, 1);
	if (retrowave_active) retrowave_reset();
	retrowave_close();
	SDL_CloseAudioDevice(audio_dev);
	SDL_DestroyRenderer(ren);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
