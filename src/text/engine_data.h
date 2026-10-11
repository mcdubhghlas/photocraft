/* Photoshop's EngineData: the PostScript-like text engine document stored in a type layer's TySh
 * block and in the document's Txt2 block (port of crates/text/src/engine_data.rs).
 *
 * Syntax (from observation of real files and public notes): dictionaries `<< /Key value ... >>`,
 * arrays `[ ... ]`, strings `( ... )` holding UTF-16BE with a FE FF byte-order mark and `\`
 * escapes (other strings are Latin-1), numbers (`12`, `-1.5`, `.5`), `true`/`false`, and `/Name`
 * values.
 *
 * EngineData comes from files and is untrusted: parsing is bounded by PC_ED_MAX_DEPTH, never
 * reads outside the input, and fails with PC_ERR_FORMAT and the reference's message
 * ("EngineData: <what> at byte <offset>"). Writing reproduces the reference byte for byte (tab
 * indentation, Photoshop's `.5` reals), so parse -> write -> parse is stable.
 *
 * A pc_ed_value is a tagged union (`kind`); dictionaries keep their entries in order and may hold
 * duplicate keys (lookups take the first, like the reference). PC_ED_DICT is 0, so a
 * zero-initialised value is the empty dictionary and valid to finalise. Values own everything
 * they point to. Trees built by this module
 * nest at most a few levels deeper than PC_ED_MAX_DEPTH; finalising, cloning, comparing and
 * writing recurse once per level (clone/compare/write refuse trees deeper than
 * PC_ED_MAX_TREE_DEPTH). Not synchronised: one owner at a time; const access is thread-safe. */
#ifndef PC_TEXT_ENGINE_DATA_H
#define PC_TEXT_ENGINE_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "base/str.h"

#define PC_ED_MAX_DEPTH 64        /* parser nesting limit ("nesting too deep") */
#define PC_ED_MAX_TREE_DEPTH 1024 /* clone / eq / write refuse deeper trees */
#define PC_ED_REAL_MAX 352        /* buffer size for pc_ed_format_real */

typedef enum pc_ed_kind {
    PC_ED_DICT,
    PC_ED_ARRAY,
    PC_ED_STRING,
    PC_ED_INT,
    PC_ED_REAL,
    PC_ED_BOOL,
    PC_ED_NAME
} pc_ed_kind;

typedef struct pc_ed_entry pc_ed_entry;

typedef struct pc_ed_value {
    pc_ed_kind kind;
    pc_str s;                  /* STRING, NAME: UTF-8 */
    int64_t i;                 /* INT */
    double r;                  /* REAL */
    bool b;                    /* BOOL */
    struct pc_ed_value *items; /* ARRAY elements */
    pc_ed_entry *entries;      /* DICT entries, in order */
    size_t n;                  /* elements or entries */
    size_t cap;
} pc_ed_value;

struct pc_ed_entry {
    pc_str key;
    pc_ed_value value;
};

/* ---- construction and ownership ---- */

void pc_ed_fini(pc_ed_value *v); /* NULL-safe; leaves an empty dictionary */
pc_status pc_ed_clone(const pc_ed_value *v, pc_ed_value *out, pc_error *err);
/* The reference's PartialEq (reals compare with ==). False for trees deeper than the limit. */
bool pc_ed_eq(const pc_ed_value *a, const pc_ed_value *b);

pc_ed_value pc_ed_dict(void);
pc_ed_value pc_ed_array(void);
pc_ed_value pc_ed_int(int64_t i);
pc_ed_value pc_ed_real(double r);
pc_ed_value pc_ed_bool(bool b);
pc_status pc_ed_string(const char *s, size_t len, pc_ed_value *out, pc_error *err);
pc_status pc_ed_name(const char *s, size_t len, pc_ed_value *out, pc_error *err);

/* ---- access ---- */

/* First entry with `key` of a dictionary (borrowed), or NULL (also for non-dictionaries). */
const pc_ed_value *pc_ed_get(const pc_ed_value *v, const char *key);
pc_ed_value *pc_ed_get_mut(pc_ed_value *v, const char *key);
/* Follows dictionary keys; the list ends with NULL: pc_ed_path(v, "EngineDict", "Editor", NULL). */
const pc_ed_value *pc_ed_path(const pc_ed_value *v, ...);
/* Sets the first entry with `key` (or appends one), taking over *val (left as an empty
 * dictionary) on success. On a non-dictionary nothing is stored and *val is finalised. */
pc_status pc_ed_set(pc_ed_value *dict, const char *key, pc_ed_value *val, pc_error *err);
/* Appends to an array (taking over *val on success); a non-array finalises *val. */
pc_status pc_ed_push(pc_ed_value *array, pc_ed_value *val, pc_error *err);
/* Removes every entry with `key` (no-op on non-dictionaries). */
void pc_ed_remove(pc_ed_value *dict, const char *key);

/* as_f64 (Int or Real), as_i64 (Int, or Real cast like Rust `as`), as_bool (Bool, or Int != 0). */
bool pc_ed_as_f64(const pc_ed_value *v, double *out);
bool pc_ed_as_i64(const pc_ed_value *v, int64_t *out);
bool pc_ed_as_bool(const pc_ed_value *v, bool *out);
/* String or Name contents (borrowed), or NULL. NULL-safe. */
const pc_str *pc_ed_as_str(const pc_ed_value *v);
/* Array elements (borrowed; *n set), or NULL for non-arrays. NULL-safe. */
const pc_ed_value *pc_ed_as_array(const pc_ed_value *v, size_t *n);

/* ---- text form ---- */

/* Parses an EngineData document (the first value; trailing bytes are ignored). */
pc_status pc_ed_parse(const uint8_t *data, size_t len, pc_ed_value *out, pc_error *err);
/* Appends Photoshop's layout ("\n\n" + value + "\n", tab-indented, one key per line) to out.
 * On failure out is unchanged. */
pc_status pc_ed_write(const pc_ed_value *v, pc_buf *out, pc_error *err);
/* A real the way Photoshop writes it: `1.0`, `.5`, `-.25`; non-finite values as `0.0`. Returns
 * the length written to out (NUL-terminated). */
size_t pc_ed_format_real(double r, char out[PC_ED_REAL_MAX]);

#endif
