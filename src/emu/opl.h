/*
 *  OPL2/OPL3 emulation library — interface header.
 *  Copyright (C) 2002-2010  The DOSBox Team.  GNU LGPL 2.1+.
 *  Originally based on ADLIBEMU.C by Ken Silverman (C) 1998-2001.
 *
 * This is the GUI build's opl.h.  The emulator implementation (opl.c) is
 * vendored unmodified and provides adlib_init / adlib_write / adlib_getsample.
 *
 * Unlike the original header, opl_init() and opl_write() here route through
 * GUI hooks (rwgui_opl_*) instead of calling adlib_* directly, so a single
 * register stream can fan out to: the software emulator (PC audio), the
 * RetroWave board, and the channel-activity visualizer — without touching
 * lds_play.c or opl.c.
 */
#ifndef OPL_H
#define OPL_H

#include <stdint.h>

typedef uintptr_t Bitu;
typedef intptr_t  Bits;
typedef uint32_t  Bit32u;
typedef int32_t   Bit32s;
typedef uint16_t  Bit16u;
typedef int16_t   Bit16s;
typedef uint8_t   Bit8u;
typedef int8_t    Bit8s;

// Software OPL emulator (opl.c), unmodified.
void adlib_init(Bit32u samplerate);
void adlib_write(Bitu idx, Bit8u val);
void adlib_getsample(Bit16s *sndptr, Bits numsamples);

Bitu adlib_reg_read(Bitu port);
void adlib_write_index(Bitu port, Bit8u val);

// Set by the GUI before opl_init() is first reached (used by opl_init macro).
extern int audioSampleRate;

// GUI fan-out hooks, defined in rwgui.c.
void rwgui_opl_init(void);
void rwgui_opl_write(unsigned int reg, Bit8u val);

#define opl_init()           rwgui_opl_init()
#define opl_write(reg, val)  rwgui_opl_write((reg), (val))
#define opl_update(buf, num) adlib_getsample((buf), (num))

#endif /* OPL_H */
