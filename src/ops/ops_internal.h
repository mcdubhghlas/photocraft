/* Internal to src/ops (and its tests): allocation wrappers with fault injection, the tile
 * reference counter behind the history's byte budget (port of crates/ops/src/tiles.rs), and
 * a document surface walker. Not part of the public API.
 *
 * Every allocation src/ops makes itself goes through pc_ops_malloc / pc_ops_grow_array, so the
 * tests can fail the n-th one (pc_ops_test_fail_alloc) and check that each public function
 * either succeeds or leaves the history exactly as it was. Allocations made inside src/doc
 * (document clones) are not injected. */
#ifndef PC_OPS_OPS_INTERNAL_H
#define PC_OPS_OPS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "doc/doc.h"
#include "ops/ops.h"

/* ---- allocation ---- */

/* Test hook: the n-th (0-based) later allocation through the wrappers below fails, once; a
 * negative n disables injection. Process-wide; meant for single-threaded tests. */
void pc_ops_test_fail_alloc(long n);
/* True when injection was armed and has not fired yet. */
bool pc_ops_test_fail_pending(void);

void *pc_ops_malloc(size_t n);
void *pc_ops_calloc_array(size_t count, size_t size);
void *pc_ops_grow_array(void *ptr, size_t *cap, size_t need, size_t elem);
/* *out = a copy of len bytes (NULL p allowed when len is 0). */
pc_status pc_ops_str_copy(const char *p, size_t len, pc_str *out, pc_error *err);
pc_status pc_ops_layer_target_clone(const pc_layer_target *t, pc_layer_target *out, pc_error *err);

/* ---- walking a document's pixel surfaces ---- */

/* Called once per surface: every layer's pc_layer_surface (raster content or the cache of a
 * text, shape or smart object layer) and mask, at any depth, then the alpha channels and the
 * quick mask. A non-OK return stops the walk and is returned. */
typedef pc_status (*pc_ops_surface_fn)(const pc_surface *s, void *ctx);
/* PC_ERR_NOMEM when the explicit walk stack can't grow (past 64 levels of nesting). */
pc_status pc_ops_surfaces(const pc_document *d, pc_ops_surface_fn fn, void *ctx);

/* ---- sets of tile addresses ---- */

/* Open-addressing set of non-zero addresses (a tile's identity is its address: retained
 * snapshots keep it alive, and copy-on-write never changes a shared tile in place). */
typedef struct pc_ops_ptrset {
    uintptr_t *slots; /* 0 marks an empty slot */
    size_t cap;       /* 0 or a power of two */
    size_t count;
} pc_ops_ptrset;

void pc_ops_ptrset_fini(pc_ops_ptrset *s);
/* *inserted is true when key was new. PC_ERR_NOMEM leaves the set unchanged. */
pc_status pc_ops_ptrset_insert(pc_ops_ptrset *s, uintptr_t key, bool *inserted);

/* Bytes of the tiles of doc not already in seen (adding them), saturating. */
pc_status pc_ops_tile_bytes(const pc_document *doc, pc_ops_ptrset *seen, size_t *out);

/* ---- the tile counter ---- */

typedef struct pc_ops_tile_ref {
    uintptr_t key; /* 0 marks an empty slot */
    uint64_t count;
    size_t len;
} pc_ops_tile_ref;

/* Tile references owned by history snapshots, counted per occurrence (aliases within one
 * document included), so removing a state releases a tile only after its last reference.
 * `bytes` is the sum of the distinct tiles' lengths. */
typedef struct pc_ops_tiles {
    pc_ops_tile_ref *slots;
    size_t cap;
    size_t count;
    uint64_t bytes;
} pc_ops_tiles;

void pc_ops_tiles_fini(pc_ops_tiles *t);
pc_status pc_ops_tiles_clone(const pc_ops_tiles *t, pc_ops_tiles *out);
/* Count every tile of doc once more. On failure the counts are partly updated: the caller
 * must discard the counter (it is a cache; trim rebuilds it). */
pc_status pc_ops_tiles_add(pc_ops_tiles *t, const pc_document *doc);
/* Count every tile of doc once less (unknown tiles are ignored). Fails only when the walk
 * stack can't grow; the caller then discards the counter as for add. */
pc_status pc_ops_tiles_remove(pc_ops_tiles *t, const pc_document *doc);
/* Retained bytes plus the tiles of current the counter does not hold (each once). */
pc_status pc_ops_tiles_bytes_with(const pc_ops_tiles *t, const pc_document *current, uint64_t *out);

/* Test access to a history's counter: NULL when it has none. */
const pc_ops_tiles *pc_ops_history_counter(const pc_history *h);

#endif
