/* JSON on top of yyjson (REWRITE_PLAN.md D7), behaving like the reference's serde_json 1.0.
 *
 * Modules build and walk documents with yyjson's own API (yyjson_mut_obj_add_*, yyjson_arr_foreach,
 * ...); this header adds what must match serde_json:
 *   - parsing: strict RFC 8259, valid UTF-8, at most 127 nested arrays/objects (serde's recursion
 *     limit), errors as "<what> at line L column C";
 *   - serde_json::Value accessors (as_u64/as_i64/as_f64/as_bool/as_str, `v["key"]` with the last
 *     duplicate key winning);
 *   - writing: serde_json's compact or pretty layout, its string escaping, and its float format
 *     (shortest round-trip digits, "1.0", "1e+20", "1e-7"); f32 values (yyjson_mut_float) are
 *     formatted as f32. Non-finite floats are written as null, as serde_json does.
 * Thread-safe; documents themselves are not synchronised. */
#ifndef PC_BASE_JSON_H
#define PC_BASE_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "buf.h"
#include "status.h"

/* serde_json's default recursion limit: deeper input is rejected. */
enum { PC_JSON_MAX_DEPTH = 127, PC_JSON_NUM_MAX = 40 };

/* Parse text into a new document (caller frees with yyjson_doc_free). */
pc_status pc_json_parse(const char *text, size_t len, yyjson_doc **out, pc_error *err);

/* As pc_json_parse with another nesting limit: max_depth arrays/objects (0 = no limit, like
 * serde_json's disable_recursion_limit; yyjson itself never recurses, so any depth is safe).
 * Deeper input fails with the same "recursion limit exceeded at line L column C" text. */
pc_status pc_json_parse_depth(const char *text, size_t len, size_t max_depth, yyjson_doc **out, pc_error *err);

/* obj[key] for an object (last duplicate wins); NULL if obj isn't an object or lacks key. */
yyjson_val *pc_json_get(yyjson_val *obj, const char *key);

/* Value accessors: return false (leaving *out untouched) when v is NULL or the wrong kind. */
bool pc_json_as_u64(yyjson_val *v, uint64_t *out); /* non-negative integers */
bool pc_json_as_i64(yyjson_val *v, int64_t *out);  /* integers within i64 */
bool pc_json_as_f64(yyjson_val *v, double *out);   /* any number */
bool pc_json_as_bool(yyjson_val *v, bool *out);
/* String contents, borrowed from the document; *len (if non-NULL) gets the byte length. */
const char *pc_json_as_str(yyjson_val *v, size_t *len);

enum {
    PC_JSON_PRETTY = 1u,   /* serde_json::to_string_pretty layout (2-space indent) */
    PC_JSON_SORT_KEYS = 2u /* serialise objects the way serde_json::Value does: keys sorted
                              bytewise, duplicates collapsed to the last one */
};

/* Append the serialisation of root to *out. On failure *out is unchanged. */
pc_status pc_json_write(const yyjson_mut_val *root, unsigned flags, pc_buf *out, pc_error *err);
pc_status pc_json_write_val(yyjson_val *root, unsigned flags, pc_buf *out, pc_error *err);

/* serde_json's text for one f64/f32 ("null" if not finite). Returns the length written into
 * out (always < PC_JSON_NUM_MAX, NUL-terminated). */
size_t pc_json_format_f64(double v, char out[PC_JSON_NUM_MAX]);
size_t pc_json_format_f32(float v, char out[PC_JSON_NUM_MAX]);

/* An owned serde_json::Value (doc NULL, or a doc without a root, is JSON null).
 *
 * Values are kept normalised the way serde_json::Value holds them: object keys sorted bytewise
 * with the last duplicate kept (BTreeMap), non-negative integers unsigned, and f32-flagged reals
 * widened to plain f64 (serde_json::to_value of an f32). Writing a normalised value without
 * PC_JSON_SORT_KEYS therefore prints what serde_json prints for that Value. */
typedef struct pc_json_value {
    yyjson_mut_doc *doc;
} pc_json_value;

#define PC_JSON_VALUE_INIT {NULL}

/* The root, or NULL for null. Borrowed. */
yyjson_mut_val *pc_json_value_root(const pc_json_value *v);
void pc_json_value_fini(pc_json_value *v); /* NULL-safe; leaves v null */
pc_status pc_json_value_clone(const pc_json_value *v, pc_json_value *out, pc_error *err);
/* serde_json::Value equality (1 != 1.0; objects compare as maps). */
bool pc_json_value_eq(const pc_json_value *a, const pc_json_value *b);
/* *out = a normalised copy of src (NULL src = null). out is overwritten, not finalised. */
pc_status pc_json_value_from_val(yyjson_val *src, pc_json_value *out, pc_error *err);
pc_status pc_json_value_from_mut(const yyjson_mut_val *src, pc_json_value *out, pc_error *err);
/* A normalised copy of src (NULL = null) owned by doc; NULL on NOMEM or nesting past 1024. */
yyjson_mut_val *pc_json_normalized_copy(yyjson_mut_doc *doc, const yyjson_mut_val *src);
yyjson_mut_val *pc_json_normalized_copy_val(yyjson_mut_doc *doc, yyjson_val *src);
/* Equality of two normalised values (NULL = null). */
bool pc_json_mut_equal(const yyjson_mut_val *a, const yyjson_mut_val *b);

#endif
