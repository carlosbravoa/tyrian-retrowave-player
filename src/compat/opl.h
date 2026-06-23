/* Minimal stand-in for OpenTyrian's opl.h.  Maps the opl_* macros that
 * lds_play.c uses onto adlib_* functions provided by tyrian_rwplay.c,
 * which forward to the RetroWave board instead of an emulator. */
#ifndef OPL_H
#define OPL_H
#include <stdint.h>
typedef uintptr_t Bitu;  typedef intptr_t Bits;
typedef uint32_t Bit32u; typedef int32_t Bit32s;
typedef uint16_t Bit16u; typedef int16_t Bit16s;
typedef uint8_t  Bit8u;  typedef int8_t  Bit8s;
void adlib_init(Bit32u samplerate);
void adlib_write(Bitu idx, Bit8u val);
void adlib_getsample(Bit16s *sndptr, Bits numsamples);
extern int audioSampleRate;
#define opl_init()           adlib_init(audioSampleRate)
#define opl_write(reg, val)  adlib_write(reg, val)
#define opl_update(buf, num) adlib_getsample(buf, num)
#endif
