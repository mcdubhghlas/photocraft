#include "format/pathmap.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"

static size_t hash_key(const char *k)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *k != '\0'; k++) {
        h ^= (unsigned char)*k;
        h *= 0x100000001b3ull;
    }
    return (size_t)(h ^ (h >> 32));
}

void pc_pathmap_fini(pc_pathmap *m)
{
    if (m != NULL) {
        free(m->slots);
        m->slots = NULL;
        m->cap = 0;
        m->count = 0;
    }
}

pc_pathmap_ent *pc_pathmap_get(const pc_pathmap *m, const char *key)
{
    size_t i;
    if (m->cap == 0) {
        return NULL;
    }
    for (i = hash_key(key) & (m->cap - 1); m->slots[i].used; i = (i + 1) & (m->cap - 1)) {
        if (strcmp(m->slots[i].key, key) == 0) {
            return &m->slots[i];
        }
    }
    return NULL;
}

static pc_status grow(pc_pathmap *m, pc_error *err)
{
    size_t cap = m->cap == 0 ? 16 : m->cap * 2;
    pc_pathmap_ent *slots;
    if (cap > SIZE_MAX / 2 / sizeof *slots) {
        return pc_fail(err, PC_ERR_LIMIT, "table too large");
    }
    slots = pc_calloc_array(cap, sizeof *slots);
    if (slots == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t j = 0; j < m->cap; j++) {
        if (m->slots[j].used) {
            size_t i = hash_key(m->slots[j].key) & (cap - 1);
            while (slots[i].used) {
                i = (i + 1) & (cap - 1);
            }
            slots[i] = m->slots[j];
        }
    }
    free(m->slots);
    m->slots = slots;
    m->cap = cap;
    return PC_OK;
}

pc_status pc_pathmap_put(pc_pathmap *m, const char *key, pc_pathmap_ent **out, bool *inserted, pc_error *err)
{
    size_t klen = strlen(key);
    size_t i;
    pc_pathmap_ent *e = pc_pathmap_get(m, key);
    if (e != NULL) {
        *out = e;
        *inserted = false;
        return PC_OK;
    }
    if (klen >= PC_PATHMAP_KEY_MAX) {
        return pc_fail(err, PC_ERR_INVALID, "bundle path too long");
    }
    if ((m->count + 1) * 4 > m->cap * 3) { /* keep the load factor under 3/4 */
        pc_status s = grow(m, err);
        if (s != PC_OK) {
            return s;
        }
    }
    for (i = hash_key(key) & (m->cap - 1); m->slots[i].used; i = (i + 1) & (m->cap - 1)) {
    }
    e = &m->slots[i];
    memset(e, 0, sizeof *e);
    e->used = true;
    memcpy(e->key, key, klen + 1);
    m->count++;
    *out = e;
    *inserted = true;
    return PC_OK;
}

bool pc_pathmap_remove(pc_pathmap *m, const char *key, pc_pathmap_ent *removed)
{
    pc_pathmap_ent *e = pc_pathmap_get(m, key);
    size_t i;
    size_t j;
    if (e == NULL) {
        return false;
    }
    if (removed != NULL) {
        *removed = *e;
    }
    /* Backward-shift deletion keeps every probe chain intact without tombstones. */
    i = (size_t)(e - m->slots);
    m->slots[i].used = false;
    j = i;
    for (;;) {
        size_t home;
        j = (j + 1) & (m->cap - 1);
        if (!m->slots[j].used) {
            break;
        }
        home = hash_key(m->slots[j].key) & (m->cap - 1);
        /* Move j into the hole at i unless its home lies cyclically in (i, j]. */
        if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
            m->slots[i] = m->slots[j];
            m->slots[j].used = false;
            i = j;
        }
    }
    m->count--;
    return true;
}

bool pc_format_sig_eq(const pc_format_sig *a, const pc_format_sig *b)
{
    return a->len == b->len && a->mtime_s == b->mtime_s && a->mtime_ns == b->mtime_ns && a->ino == b->ino &&
           a->dev == b->dev && a->ctime_s == b->ctime_s && a->ctime_ns == b->ctime_ns;
}
