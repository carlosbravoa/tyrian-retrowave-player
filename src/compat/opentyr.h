/* Minimal SDL-free stand-in for OpenTyrian's opentyr.h.
 * lds_play.c only needs these integer typedefs and <stdbool.h>. */
#ifndef OPENTYR_H
#define OPENTYR_H
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t  Uint8;  typedef int8_t  Sint8;
typedef uint16_t Uint16; typedef int16_t Sint16;
typedef uint32_t Uint32; typedef int32_t Sint32;
#endif
