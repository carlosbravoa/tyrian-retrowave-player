/* Minimal stand-in for OpenTyrian's file.h.  Tyrian data is little-endian,
 * so the *_die readers are plain sized reads here (no byte swapping).
 * fread_die is provided by tyrian_rwplay.c. */
#ifndef FILE_H
#define FILE_H
#include "opentyr.h"
#include <stdio.h>
void fread_die(void *buffer, size_t size, size_t count, FILE *stream);
static inline void fread_u8_die(Uint8 *b, size_t n, FILE *f)  { fread_die(b, 1, n, f); }
static inline void fread_u16_die(Uint16 *b, size_t n, FILE *f){ fread_die(b, 2, n, f); }
static inline void fread_u32_die(Uint32 *b, size_t n, FILE *f){ fread_die(b, 4, n, f); }
#endif
