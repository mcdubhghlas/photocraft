/* Internal: a small open-addressing hash map keyed by bundle paths (at most
 * PC_FORMAT_OBJECT_PATH_MAX - 1 bytes), for the store's object tables and caches. Values are a
 * fixed record; each table uses the fields it needs. Not synchronised. */
#ifndef PC_FORMAT_PATHMAP_H
#define PC_FORMAT_PATHMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "base/status.h"

enum { PC_PATHMAP_KEY_MAX = 80 };

/* What a file looked like when it was verified (store.rs FileSignature). Any change means
 * "verify again": an in-place rewrite changes the change time even when a tool restores the
 * modification time. */
typedef struct pc_format_sig {
    uint64_t len;
    int64_t mtime_s;
    long mtime_ns;
    uint64_t ino;
    uint64_t dev;
    int64_t ctime_s;
    long ctime_ns;
} pc_format_sig;

typedef struct pc_pathmap_ent {
    bool used;
    char key[PC_PATHMAP_KEY_MAX];
    void *ptr;         /* owned pointer (released by the table's owner) */
    size_t index;      /* an index into a parallel array */
    pc_format_sig sig; /* directory verification cache */
    uint64_t checked_at;
} pc_pathmap_ent;

typedef struct pc_pathmap {
    pc_pathmap_ent *slots;
    size_t cap; /* 0 or a power of two */
    size_t count;
} pc_pathmap;

#define PC_PATHMAP_INIT {NULL, 0, 0}

/* Frees the table (not the values: release ptrs first by iterating slots). */
void pc_pathmap_fini(pc_pathmap *m);
/* The entry for key, or NULL. Borrowed until the next put/remove. */
pc_pathmap_ent *pc_pathmap_get(const pc_pathmap *m, const char *key);
/* The entry for key, inserted zeroed (but for the key) when absent; *inserted says which.
 * PC_ERR_INVALID for a key that doesn't fit, PC_ERR_NOMEM. */
pc_status pc_pathmap_put(pc_pathmap *m, const char *key, pc_pathmap_ent **out, bool *inserted, pc_error *err);
/* Removes key; *removed (if non-NULL) receives the old entry. False when absent. */
bool pc_pathmap_remove(pc_pathmap *m, const char *key, pc_pathmap_ent *removed);

bool pc_format_sig_eq(const pc_format_sig *a, const pc_format_sig *b);

#endif
