# tyrian-retrowave — two players:
#   tyrian_rwplay : standalone CLI, no SDL, board-only output (self-contained)
#   tyrian_rwgui  : SDL2 GUI with software OPL3 audio + channel visualizer
#
# lds_play.c is compiled separately per target because the two builds use
# different opl.h headers (compat/ routes opl_write straight to the emulator
# stub; emu/ routes it through the GUI fan-out hook).

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config sdl2 --cflags)
SDL_LIBS   := $(shell sdl2-config --libs   2>/dev/null || pkg-config sdl2 --libs)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
COMMON  := -std=gnu11 -Isrc

CLI_INC := $(COMMON) -Isrc/compat
GUI_INC := $(COMMON) -Isrc/emu -Isrc/compat $(SDL_CFLAGS)

all: tyrian_rwplay tyrian_rwgui

# ---- CLI player (no SDL, no emulator) -------------------------------------
CLI_OBJS := build/cli/tyrian_rwplay.o build/cli/lds_play.o build/cli/retrowave_serial.o

tyrian_rwplay: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

build/cli/%.o: src/%.c | build/cli
	$(CC) $(CFLAGS) $(CLI_INC) -c -o $@ $<

# ---- GUI player (SDL2 + emulator + board) ---------------------------------
GUI_OBJS := build/gui/rwgui.o build/gui/lds_play.o build/gui/retrowave_serial.o build/gui/opl.o

tyrian_rwgui: $(GUI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(SDL_LIBS) -lm

build/gui/opl.o: src/emu/opl.c | build/gui
	$(CC) $(CFLAGS) $(GUI_INC) -c -o $@ $<

build/gui/%.o: src/%.c | build/gui
	$(CC) $(CFLAGS) $(GUI_INC) -c -o $@ $<

build/cli build/gui:
	mkdir -p $@

clean:
	rm -rf build tyrian_rwplay tyrian_rwgui

.PHONY: all clean
