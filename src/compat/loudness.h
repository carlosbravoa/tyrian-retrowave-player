/* lds_play.c includes loudness.h only for opl.h + the audioSampleRate extern. */
#ifndef LOUDNESS_H
#define LOUDNESS_H
#include "opentyr.h"
/* Angle-bracket so this resolves by -I order (emu/opl.h in the GUI build,
 * compat/opl.h in the CLI build) rather than picking up the sibling
 * compat/opl.h that sits next to this file. */
#include <opl.h>
#endif
