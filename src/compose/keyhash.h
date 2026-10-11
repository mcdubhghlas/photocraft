/* Private to src/compose: cache-key hashing (keyhash.c). FNV-1a over the bytes, in order. */
#ifndef PC_COMPOSE_KEYHASH_H
#define PC_COMPOSE_KEYHASH_H

#include <stddef.h>
#include <stdint.h>

#include "base/json.h"
#include "doc/doc.h"

#define PC_CMP_HASH0 0xcbf29ce484222325ull

void pc_cmp_hash_bytes(uint64_t *h, const void *p, size_t n);
void pc_cmp_hash_u64(uint64_t *h, uint64_t v);
void pc_cmp_hash_f32(uint64_t *h, float v); /* by bits */
void pc_cmp_hash_f64(uint64_t *h, double v);
void pc_cmp_hash_rect(uint64_t *h, pc_rect r);
void pc_cmp_hash_path(uint64_t *h, const pc_path *p);
void pc_cmp_hash_vector_mask(uint64_t *h, const pc_vector_mask *m); /* NULL = None */
/* Identity of a surface's pixels: format, default pixel and its tiles' addresses (only valid
 * while something keeps those tiles alive). */
uint64_t pc_cmp_surface_fp(const pc_surface *s);

typedef yyjson_mut_val *(*pc_cmp_json_fn)(yyjson_mut_doc *d, const void *v);
/* Hash of a value's serde JSON (PC_ERR_NOMEM when it can't be built). */
pc_status pc_cmp_hash_json(uint64_t *h, pc_cmp_json_fn fn, const void *v, pc_error *err);
pc_status pc_cmp_hash_fill(uint64_t *h, const pc_fill *f, pc_error *err);
pc_status pc_cmp_hash_adjustment(uint64_t *h, const pc_adjustment *a, pc_error *err);
pc_status pc_cmp_hash_artboard(uint64_t *h, const pc_group *g, pc_error *err);
pc_status pc_cmp_hash_effects(uint64_t *h, const pc_effects *e, pc_error *err);

#endif
