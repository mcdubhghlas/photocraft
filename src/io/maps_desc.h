/* Descriptor helpers shared by chunk I4's maps (vector, comps, annotations, abr): the small
 * readers the reference takes from blocks.rs (num, get_desc, enum_of, bool_of,
 * parse_prefix_versioned) plus builders. I4 keeps its own copies so its maps don't wait on I2's
 * blocks.c; they are a few lines each and identical in behaviour. Internal to io. Thread-safe. */
/* The descriptor readers (num, obj, enum, bool_of, vdesc_prefix) are thin wrappers over
 * blocks.h's (I2); I4 kept these names so its files did not wait for I2. */
#ifndef PC_IO_MAPS_DESC_H
#define PC_IO_MAPS_DESC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/str.h"
#include "psd/descriptor.h"

/* blocks::num: Double, UnitFloat or Integer as f64 (v may be NULL). */
bool pc_io4_num(const pc_psd_value *v, double *out);
/* blocks::num over d.get(key). */
bool pc_io4_num_of(const pc_psd_desc *d, const char *key, double *out);
/* The Descriptor or GlobalObject under key (borrowed) or NULL. */
const pc_psd_desc *pc_io4_obj(const pc_psd_desc *d, const char *key);
/* An enumerated value's bytes (borrowed) or NULL. */
const uint8_t *pc_io4_enum(const pc_psd_desc *d, const char *key, size_t *len);
/* The enumerated value equals s. */
bool pc_io4_enum_is(const pc_psd_desc *d, const char *key, const char *s);
/* Some(Boolean(true)). */
bool pc_io4_bool_of(const pc_psd_desc *d, const char *key);
/* A versioned descriptor at the start of data; *found false when it doesn't parse (fails only
 * on out of memory). */
pc_status pc_io4_vdesc_prefix(const uint8_t *data, size_t len, bool *found, pc_psd_desc *out, pc_error *err);
/* UnicodeString::to_string_lossy (trailing NULs dropped, lone surrogates U+FFFD); *out replaced. */
pc_status pc_io4_ustr_lossy(const pc_psd_ustr *s, pc_str *out, pc_error *err);

/* ---- builders with a sticky status: each does nothing once *st != PC_OK (releasing what it
 * would have taken over), so a descriptor is written as a straight sequence of calls. ---- */

pc_psd_value pc_io4_unit(const char unit[4], double v);
void pc_io4_put(pc_psd_desc *d, const char *key, pc_psd_value v, pc_status *st, pc_error *err);
/* TEXT value of s[0..len) (nul: with a trailing NUL unit, UnicodeString::new_nul). */
void pc_io4_put_text(pc_psd_desc *d, const char *key, const char *s, size_t len, bool nul, pc_status *st,
                     pc_error *err);
void pc_io4_put_enum(pc_psd_desc *d, const char *key, const char *type_id, const char *value, pc_status *st,
                     pc_error *err);
/* Takes over *child (zeroed afterwards, also when nothing is added). */
void pc_io4_put_obj(pc_psd_desc *d, const char *key, pc_psd_desc *child, pc_status *st, pc_error *err);
/* An empty descriptor of class_id (Descriptor::new); on failure *st is set and d is empty. */
void pc_io4_desc(pc_psd_desc *d, const char *class_id, pc_status *st, pc_error *err);
/* Appends v to a LIST value (released when *st already failed). */
void pc_io4_list_push(pc_psd_value *list, pc_psd_value v, pc_status *st, pc_error *err);
pc_psd_value pc_io4_list(void);

#endif
