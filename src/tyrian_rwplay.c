/*
 * tyrian_rwplay — stream Tyrian's music to a RetroWave OPL3 Express.
 *
 * Tyrian's music is stored in MUSIC.MUS as a bank of LDS (Loudness) songs.
 * This player reuses OpenTyrian's actual LDS driver (src/lds_play.c) — the
 * same code the game uses — but instead of feeding the synthesized OPL
 * register writes to an emulator, it forwards them to a real Yamaha YMF262
 * on a RetroWave OPL3 Express board over USB-serial (src/retrowave_serial.c).
 *
 * Because we run the game's own driver, the output is reference-grade: exact
 * instruments, exact note timing, exact loop points — no reimplementation.
 *
 * The LDS driver has no timing of its own; the game clocks it from the audio
 * sample counter at ~69.5 Hz.  The board has no timing either, so here WE
 * drive lds_update() at 69.5 Hz in real wall-clock time (monotonic deadlines).
 *
 * Transport: n/space next, p prev, r restart, l loop, +/- tempo, q quit.
 */

// clock_nanosleep / CLOCK_MONOTONIC / cfmakeraw
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "lds_play.h"
#include "file.h"
#include "opl.h"
#include "retrowave_serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/* Shim: satisfy the few symbols src/lds_play.c expects from the game. */
/* ------------------------------------------------------------------ */

// lds_play.c uses fread_*_die (static inlines in file.h) which call fread_die.
int audioSampleRate = 49716;  // referenced via opl_init(); value is irrelevant here

void fread_die(void *buffer, size_t size, size_t count, FILE *stream)
{
	size_t got = fread(buffer, size, count, stream);
	if (got != count)
	{
		fprintf(stderr, "error: unexpected end of file while reading music\n");
		exit(EXIT_FAILURE);
	}
}

// opl.h maps opl_init/opl_write/opl_update onto these.  We bypass the emulator
// entirely and forward straight to the board.
void adlib_init(Bit32u samplerate)
{
	(void)samplerate;
	retrowave_reset();
}

void adlib_write(Bitu idx, Bit8u val)
{
	retrowave_write((unsigned int)idx, val);
}

void adlib_getsample(Bit16s *sndptr, Bits numsamples)
{
	(void)sndptr; (void)numsamples;  // no PC audio in this player
}

/* ------------------------------------------------------------------ */
/* Song bank (MUSIC.MUS): u16 count, then u32 offset per song.        */
/* ------------------------------------------------------------------ */

static FILE *music_file;
static Uint32 *song_offset;
static Uint16 song_count;

static long ftell_eof_local(FILE *f)
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
	if (!music_file)
	{
		fprintf(stderr, "error: cannot open music bank '%s'\n", path);
		return false;
	}

	fread_u16_die(&song_count, 1, music_file);
	song_offset = malloc((song_count + 1) * sizeof(*song_offset));
	fread_u32_die(song_offset, song_count, music_file);
	song_offset[song_count] = ftell_eof_local(music_file);
	return true;
}

static void load_song(unsigned int n)
{
	unsigned int size = song_offset[n + 1] - song_offset[n];
	lds_load(music_file, song_offset[n], size);
}

/* ------------------------------------------------------------------ */
/* Raw terminal input (non-blocking).                                 */
/* ------------------------------------------------------------------ */

static struct termios saved_termios;

static void term_raw(void)
{
	tcgetattr(STDIN_FILENO, &saved_termios);
	struct termios t = saved_termios;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &t);
	fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO, F_GETFL) | O_NONBLOCK);
}

static void term_restore(void)
{
	tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

static int read_key(void)
{
	unsigned char c;
	if (read(STDIN_FILENO, &c, 1) == 1)
		return c;
	return -1;
}

/* ------------------------------------------------------------------ */

static const long LDS_PERIOD_NS = 14388489;  // 1e9 / 69.5 Hz

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void usage(const char *argv0)
{
	printf("Usage: %s [OPTION...] [MUSIC.MUS]\n\n"
	       "Stream Tyrian's music to a RetroWave OPL3 Express.\n\n"
	       "  -d DEV    serial device (default ttyACM0; '-' = dry run, no hardware)\n"
	       "  -s N      start at song N (1-based)\n"
	       "  -l        loop the current song instead of advancing\n"
	       "  -h        show this help\n\n"
	       "Transport keys: n/space next, p prev, r restart, l loop, +/- tempo, q quit\n",
	       argv0);
}

int main(int argc, char *argv[])
{
	const char *dev = "ttyACM0";
	const char *bank_path = NULL;
	int start_song = 1;
	bool loop = false;

	for (int i = 1; i < argc; ++i)
	{
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			start_song = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-l"))
			loop = true;
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
		{
			usage(argv[0]);
			return 0;
		}
		else if (argv[i][0] != '-')
			bank_path = argv[i];
		else
		{
			fprintf(stderr, "unknown option '%s'\n", argv[i]);
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!bank_path)
	{
		// Common locations, then current directory.
		const char *candidates[] = { "music.mus", "MUSIC.MUS", NULL };
		for (int i = 0; candidates[i]; ++i)
			if (access(candidates[i], R_OK) == 0) { bank_path = candidates[i]; break; }
		if (!bank_path)
		{
			fprintf(stderr, "error: no music bank given and none found in current directory\n\n");
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (!load_bank(bank_path))
		return EXIT_FAILURE;

	if (!retrowave_open(dev))
		return EXIT_FAILURE;

	if (start_song < 1) start_song = 1;
	if (start_song > song_count) start_song = song_count;

	int song = start_song - 1;
	int tempo_pct = 100;
	bool quit = false;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	term_raw();
	printf("Loaded %d songs from %s\n", song_count, bank_path);
	fflush(stdout);

	while (!quit && !g_stop)
	{
		load_song(song);
		bool prev_looped = false;
		bool advance = false;
		int advance_dir = +1;

		printf("\r\033[K[%02d/%02d] playing  loop:%s  tempo:%d%%   (n/p/r/l/+/-/q)\n",
		       song + 1, song_count, loop ? "on" : "off", tempo_pct);
		fflush(stdout);

		struct timespec next;
		clock_gettime(CLOCK_MONOTONIC, &next);

		while (!advance && !quit && !g_stop)
		{
			lds_update();

			// End of a non-looping song, or one full loop in play-once mode.
			if (!playing || (!loop && songlooped && !prev_looped))
			{
				advance = true;
				advance_dir = +1;
				break;
			}
			prev_looped = songlooped;

			// Schedule the next tick at an absolute monotonic deadline so we
			// don't drift.  Tempo scales the period.
			long period = LDS_PERIOD_NS * 100 / (tempo_pct > 0 ? tempo_pct : 1);
			next.tv_nsec += period;
			while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
			clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

			int k;
			while ((k = read_key()) != -1)
			{
				switch (k)
				{
				case 'q': quit = true; break;
				case 'n': case ' ': advance = true; advance_dir = +1; break;
				case 'p': advance = true; advance_dir = -1; break;
				case 'r': lds_rewind(); prev_looped = false; break;
				case 'l':
					loop = !loop;
					printf("\rloop:%s          \n", loop ? "on" : "off");
					fflush(stdout);
					break;
				case '+': case '=':
					if (tempo_pct < 300) tempo_pct += 10;
					printf("\rtempo:%d%%        \n", tempo_pct); fflush(stdout);
					break;
				case '-': case '_':
					if (tempo_pct > 20) tempo_pct -= 10;
					printf("\rtempo:%d%%        \n", tempo_pct); fflush(stdout);
					break;
				}
			}
		}

		if (advance)
		{
			song += advance_dir;
			if (song < 0) song = song_count - 1;
			if (song >= song_count) song = 0;
		}
	}

	retrowave_reset();   // silence any hanging notes
	retrowave_close();
	term_restore();
	printf("\nbye.\n");
	return 0;
}
