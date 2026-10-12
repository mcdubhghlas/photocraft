/* Private helpers shared by I3's files (smart_filters.c, smart_map.c, psd_export*.c): small
 * descriptor accessors and value constructors. The descriptor accessors are local copies of
 * blocks::{num, get_desc, enum_of} (I2's pc_io_desc_* behave the same) so smart-object parsing
 * does not depend on another chunk's progress. Not part of the io API. */
#ifndef PC_IO_SMART_INTERNAL_H
#define PC_IO_SMART_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "io/core.h"
#include "psd/descriptor.h"

/* ---- reading (blocks::num / get_desc / enum_of, Descriptor::get) ---- */

bool pc_io3_num(const pc_psd_value *v, double *out);
/* num(d.get(key)) */
bool pc_io3_num_of(const pc_psd_desc *d, const char *key, double *out);
/* A Descriptor or GlobalObject value under key (borrowed), or NULL. */
const pc_psd_desc *pc_io3_get_desc(const pc_psd_desc *d, const char *key);
/* An enumerated value's bytes under key (borrowed), or NULL. */
const uint8_t *pc_io3_enum_of(const pc_psd_desc *d, const char *key, size_t *len);
/* A Text value under key as lossy UTF-8 without trailing NULs (malloc'd); *found false when the
 * key is absent or not text. */
pc_status pc_io3_text_of(const pc_psd_desc *d, const char *key, bool *found, char **out, pc_error *err);

/* ---- building ---- */

/* UntF value. */
pc_psd_value pc_io3_unit(const char unit[4], double v);
/* Appends key -> *v to d, taking *v over (also on failure); does nothing but release *v when st
 * is already an error, so calls chain: st = pc_io3_put(d, "Key", &v, st, err). */
pc_status pc_io3_put(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_status st, pc_error *err);
/* Replaces the first item with key in place, or appends it (smart_map::set); takes *v over. */
pc_status pc_io3_set(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_error *err);
/* Removes every item with key. */
void pc_io3_remove(pc_psd_desc *d, const char *key);
/* Appends key -> Descriptor(*sub), taking *sub over (also on failure); chains like put. */
pc_status pc_io3_put_obj(pc_psd_desc *d, const char *key, pc_psd_desc *sub, pc_status st, pc_error *err);

/* Lowercase hex of bytes (malloc'd, NUL-terminated). */
pc_status pc_io3_to_hex(const uint8_t *b, size_t n, char **out, pc_error *err);
/* Decodes even-length hex; false (nothing allocated) when malformed. NOMEM is reported. */
pc_status pc_io3_from_hex(const char *s, size_t n, bool *ok, uint8_t **out, size_t *len, pc_error *err);

#endif
