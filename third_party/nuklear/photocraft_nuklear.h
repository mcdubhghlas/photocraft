/* PhotoCraft's Nuklear configuration. Every file that uses Nuklear includes this header, never
 * nuklear.h directly, so the NK_* options always match the implementation in nuklear.c. */
#ifndef PHOTOCRAFT_NUKLEAR_H
#define PHOTOCRAFT_NUKLEAR_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_UINT_DRAW_INDEX /* 32-bit indices: a full editor UI exceeds 65,535 vertices */
#define NK_KEYSTATE_BASED_INPUT

#include "nuklear.h"

#endif
