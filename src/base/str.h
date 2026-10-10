/* pc_str: an owned UTF-8 string with an explicit length (Rust `String`).
 *
 * Unlike a plain C string it may hold NUL bytes (JSON "\u0000", PSD names with trailing NULs).
 * `ptr` is NUL-terminated whenever it is non-NULL, so pc_str_c() is usable as a C string up to
 * the first NUL. A zero-initialised pc_str (PC_STR_INIT) is the valid empty string. Contents are
 * not validated as UTF-8 here; producers (JSON, PSD, UI) hand in valid UTF-8.
 * Not synchronised; one owner at a time. */
#ifndef PC_BASE_STR_H
#define PC_BASE_STR_H

#include <stdbool.h>
#include <stddef.h>

#include "status.h"

typedef struct pc_str {
    char *ptr; /* NULL or len bytes + '\0'; owned */
    size_t len;
} pc_str;

#define PC_STR_INIT {NULL, 0}

/* The contents as a C string ("" when empty). Borrowed until the next mutation. */
static inline const char *pc_str_c(const pc_str *s) { return s->ptr != NULL ? s->ptr : ""; }

void pc_str_fini(pc_str *s); /* NULL-safe; leaves s empty */
/* Replace the contents with len bytes from p (p may be NULL when len is 0). On failure s is
 * unchanged. */
pc_status pc_str_set(pc_str *s, const char *p, size_t len, pc_error *err);
pc_status pc_str_set_c(pc_str *s, const char *cstr, pc_error *err);
/* *out = a copy of s (out is overwritten, not finalised). */
pc_status pc_str_clone(const pc_str *s, pc_str *out, pc_error *err);
/* Takes ownership of a malloc'd NUL-terminated buffer of len bytes. */
pc_str pc_str_adopt(char *p, size_t len);
/* Append bytes. On failure s is unchanged. */
pc_status pc_str_append(pc_str *s, const char *p, size_t len, pc_error *err);

bool pc_str_eq(const pc_str *a, const pc_str *b);
bool pc_str_eq_mem(const pc_str *a, const char *p, size_t len);
bool pc_str_eq_c(const pc_str *a, const char *cstr);
/* Bytewise ordering (Rust's str Ord): negative, zero or positive. */
int pc_str_cmp(const pc_str *a, const pc_str *b);

#endif
