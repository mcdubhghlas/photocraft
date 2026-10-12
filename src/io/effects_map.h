/* Layer effects <-> lfx2 / lrfx blocks (chunk I2: crates/io/src/effects_map.rs). Unreadable data is
 * "not found"; only out of memory fails. Thread-safe. */
#ifndef PC_IO_EFFECTS_MAP_H
#define PC_IO_EFFECTS_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "psd/descriptor.h"

/* A blend mode from its descriptor enum id; false when unknown. */
bool pc_io_blend_from_id(const uint8_t *id, size_t len, pc_blend_mode *out);
/* The BlnM enum value for a blend mode. */
pc_status pc_io_blend_long_value(pc_blend_mode m, pc_psd_value *out, pc_error *err);
/* lfx2 data -> (master switch, effects); *found false when unreadable. *items malloc'd
 * (pc_effect_fini each, then free). */
pc_status pc_io_parse_lfx2(const uint8_t *data, size_t len, bool *found, bool *master, pc_effect **items,
                           size_t *n, pc_error *err);
/* (master switch, effects) -> lfx2 data; *out replaced. */
pc_status pc_io_write_lfx2(bool master, const pc_effect *items, size_t n, pc_buf *out, pc_error *err);

/* Legacy lrFX data (drop / inner shadows, glows, solid fill; bevels skipped) -> (true, effects);
 * *found false when unreadable. */
pc_status pc_io_parse_lrfx(const uint8_t *data, size_t len, bool *found, bool *master, pc_effect **items,
                           size_t *n, pc_error *err);

#endif
