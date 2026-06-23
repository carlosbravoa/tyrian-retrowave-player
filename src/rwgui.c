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
		int base = op_table[ch];
		bool on = (fmchip[0xB0 + ch] & 0x20) != 0;
		int carrier_tl = fmchip[0x43 + base] & 0x3f;
		float amp = on ? (0x3f - carrier_tl) / 63.0f : 0.0f;

		int fnum = fmchip[0xA0 + ch] | ((fmchip[0xB0 + ch] & 3) << 8);
		int block = (fmchip[0xB0 + ch] >> 2) & 7;
		float pitch = (block * 1024 + fnum) / (8.0f * 1024.0f);   // 0..1

		VizChan *v = &viz[ch];
		v->on = on;
		if (amp > v->level) v->level = amp;                     // instant attack
		else v->level += (amp - v->level) * 0.12f;              // smooth release
		if (v->level < 0) v->level = 0;
		if (v->level > v->peak) v->peak = v->level;
		else v->peak -= 0.010f;
		if (v->peak < v->level) v->peak = v->level;
		if (on) v->hue = pitch;
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
static const char *STYLE_NAME[] = { "LED VU", "NEON GLOW", "SPECTRUM" };

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

static void cycle_output(void)
{
	// PC -> BOARD -> BOTH -> PC  (BOARD/BOTH only if a board is open)
	if (out_pc && !out_board)      { out_pc = false; out_board = true; }
	else if (!out_pc && out_board) { out_pc = true;  out_board = true; }
	else                           { out_pc = true;  out_board = false; }
	if ((out_board) && !retrowave_active) { out_board = false; out_pc = true; }  // no board
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
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 540,
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
	while (running)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_QUIT) running = false;
			else if (e.type == SDL_KEYDOWN)
			{
				switch (e.key.keysym.sym)
				{
				case SDLK_q: case SDLK_ESCAPE: running = false; break;
				case SDLK_SPACE: paused = !paused; break;
				case SDLK_n: case SDLK_RIGHT: play_index(cur_song + 1); break;
				case SDLK_p: case SDLK_LEFT:  play_index(cur_song - 1); break;
				case SDLK_r: SDL_LockAudioDevice(audio_dev); lds_rewind();
				             samples_until_update = 0; SDL_UnlockAudioDevice(audio_dev); break;
				case SDLK_l: loop = !loop; break;
				case SDLK_o: cycle_output(); break;
				case SDLK_v: style = (style + 1) % STYLE_COUNT; break;
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
		fill(ren, 0, 0, W, 70, (RGB){ 22, 22, 38 }, 255);

		// header: song + transport state
		char line[160], out[8];
		output_label(out, sizeof out);
		snprintf(line, sizeof line, "%02d/%02d  %s", cur_song + 1, song_count, song_name(cur_song));
		draw_text(ren, 16, 12, 3, (RGB){ 240, 240, 255 }, line);
		snprintf(line, sizeof line, "OUT:%s  TEMPO:%d%%  LOOP:%s  %s%s",
		         out, tempo_pct, loop ? "ON" : "OFF", STYLE_NAME[style],
		         paused ? "  -PAUSED-" : "");
		draw_text(ren, 16, 44, 2, (RGB){ 150, 170, 210 }, line);

		// visualizer area
		int ax = 24, ay = 96, aw = W - 48, ah = H - 96 - 56;
		fill(ren, ax - 6, ay - 6, aw + 12, ah + 12 + 26, (RGB){ 8, 8, 14 }, 255);
		draw_bars(ren, ax, ay, aw, ah);

		// footer: help
		draw_text(ren, 16, H - 26, 2, (RGB){ 110, 120, 150 },
		          "SPACE PLAY  N/P SONG  L LOOP  +/- TEMPO  O OUTPUT  V STYLE  Q QUIT");

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
