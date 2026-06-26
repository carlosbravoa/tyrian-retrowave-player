# tyrian-retrowave — two players:
#   tyrian_rwplay : standalone CLI, no SDL, board-only output (self-contained)
#   tyrian_rwgui  : SDL2 GUI with software OPL3 audio + channel visualizer
#
# lds_play.c is compiled separately per target because the two builds use
# different opl.h headers (compat/ routes opl_write straight to the emulator
# stub; emu/ routes it through the GUI fan-out hook).

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs   2>/dev/null || pkg-config --libs   sdl2 2>/dev/null)

# ALSA is optional too: with it, both players gain General MIDI output (--midi);
# without it, --midi is unavailable but everything else still builds. The CLI
# thus stays dependency-free (libm aside) when ALSA is absent.
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA_LIBS   := $(shell pkg-config --libs   alsa 2>/dev/null)
ifneq ($(strip $(ALSA_LIBS)),)
ALSA_DEF := -DHAVE_ALSA
endif

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
COMMON  := -std=gnu11 -Isrc

CLI_INC := $(COMMON) -Isrc/compat $(ALSA_DEF) $(ALSA_CFLAGS)
GUI_INC := $(COMMON) -Isrc/emu -Isrc/compat $(SDL_CFLAGS) $(ALSA_DEF) $(ALSA_CFLAGS)

# The CLI player (tyrian_rwplay) needs no SDL. The GUI (tyrian_rwgui) needs SDL2;
# it is built by default only if SDL2 is detected, so a terminal-only user with
# no SDL2 can just run `make` and get the CLI. `make tyrian_rwgui` forces it.
ifeq ($(strip $(SDL_LIBS)),)
all: tyrian_rwplay
	@echo "Note: SDL2 not found — built CLI only. Install libsdl2-dev for the GUI (tyrian_rwgui)."
else
all: tyrian_rwplay tyrian_rwgui
endif

# ---- CLI player (no SDL, no emulator) -------------------------------------
CLI_OBJS := build/cli/tyrian_rwplay.o build/cli/lds_play.o build/cli/retrowave_serial.o build/cli/midi_out.o

tyrian_rwplay: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(ALSA_LIBS) -lm

build/cli/%.o: src/%.c | build/cli
	$(CC) $(CFLAGS) $(CLI_INC) -c -o $@ $<

# ---- GUI player (SDL2 + emulator + board) ---------------------------------
GUI_OBJS := build/gui/rwgui.o build/gui/lds_play.o build/gui/retrowave_serial.o build/gui/opl.o build/gui/midi_out.o

tyrian_rwgui: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS) $(ALSA_LIBS) -lm

build/gui/opl.o: src/emu/opl.c | build/gui
	$(CC) $(CFLAGS) $(GUI_INC) -c -o $@ $<

build/gui/%.o: src/%.c | build/gui
	$(CC) $(CFLAGS) $(GUI_INC) -c -o $@ $<

build/cli build/gui:
	mkdir -p $@

clean:
	rm -rf build tyrian_rwplay tyrian_rwgui

.PHONY: all clean
