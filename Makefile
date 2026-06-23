# tyrian-retrowave — fully self-contained, no SDL, no external libraries.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += -std=gnu11 -Isrc -Isrc/compat

TARGET  := tyrian_rwplay
OBJS    := src/tyrian_rwplay.o src/lds_play.o src/retrowave_serial.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
