/* Minimal RGBA8 PNG writer for spike snapshots (the real codec is chunk F11). */
#ifndef PC_UI_SPIKE_PNG_H
#define PC_UI_SPIKE_PNG_H

#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* Write w x h RGBA pixels (rows top to bottom, `stride` bytes apart) to path, atomically. */
pc_status spike_png_write(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h, size_t stride, pc_error *err);

#endif
