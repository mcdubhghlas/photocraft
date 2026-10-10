/* Internal to src/doc: an iterative pre-order walk over a layer tree.
 *
 * The walk keeps an explicit stack of (sibling array, next index) frames: the first
 * PC_DOC_ITER_INLINE live in the iterator itself, deeper levels on the heap. After
 * pc_doc_iter_next returns a layer, frames[0 .. len) describe its path: the layer at depth k is
 * frames[k].arr[frames[k].i - 1], so the path is (frames[k].i - 1) for k in 0 .. len and the
 * depth is len - 1. Not part of the public API. */
#ifndef PC_DOC_TREE_H
#define PC_DOC_TREE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "doc/doc.h"

#define PC_DOC_ITER_INLINE 128

typedef struct pc_doc_iter_frame {
    const pc_layer *arr;
    size_t n;
    size_t i; /* index of the next sibling to visit */
} pc_doc_iter_frame;

typedef struct pc_doc_iter {
    pc_doc_iter_frame *frames; /* inline_frames or a heap copy */
    size_t len;
    size_t cap;
    const pc_layer *pending; /* group returned last: its children are entered on the next call */
    pc_doc_iter_frame inline_frames[PC_DOC_ITER_INLINE];
} pc_doc_iter;

/* Starts a walk over layers[0 .. n). Never fails. The iterator must not be copied (frames may
 * point into it). */
void pc_doc_iter_init(pc_doc_iter *it, const pc_layer *layers, size_t n);
/* 1: *out is the next layer in pre-order (bottom to top, parents before children); 0: done;
 * -1: out of memory growing the stack past PC_DOC_ITER_INLINE levels (the walk is over). */
int pc_doc_iter_next(pc_doc_iter *it, const pc_layer **out);
/* Depth of the layer returned last. */
static inline size_t pc_doc_iter_depth(const pc_doc_iter *it)
{
    return it->len > 0 ? it->len - 1 : 0;
}
/* Index at level k of the path of the layer returned last (k <= depth). */
static inline size_t pc_doc_iter_index(const pc_doc_iter *it, size_t k)
{
    return it->frames[k].i - 1;
}
void pc_doc_iter_fini(pc_doc_iter *it);

/* Finds the first layer with id in pre-order. 1 found (the iterator then holds its path; the
 * caller still finalises it), 0 not found, -1 out of memory. */
int pc_doc_iter_find(pc_doc_iter *it, const pc_layer *layers, size_t n, pc_layer_id id, const pc_layer **out);

/* Whole layer arrays (subtrees included), without recursion. clone: a deep copy (NULL for
 * n == 0), fresh ids and no PSD ids with `fresh`. fini: finalises and frees the array, never
 * allocates. eq: the derived PartialEq (false also if the walk can't grow its stack). */
pc_status pc_doc_clone_layers(const pc_layer *src, size_t n, bool fresh, pc_layer **out, pc_error *err);
void pc_doc_fini_layers(pc_layer *arr, size_t n);
bool pc_doc_layers_eq(const pc_layer *a, size_t na, const pc_layer *b, size_t nb);

/* Field-wise PartialEq helpers shared by the src/doc ports (floats compare with ==). */
static inline bool pc_doc_color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

static inline bool pc_doc_affine_eq(const pc_affine *a, const pc_affine *b)
{
    return a->m[0] == b->m[0] && a->m[1] == b->m[1] && a->m[2] == b->m[2] && a->m[3] == b->m[3] &&
           a->m[4] == b->m[4] && a->m[5] == b->m[5];
}

/* Blob equality where NULL stands for an empty Arc<Vec<u8>> (not for None). */
static inline bool pc_doc_bytes_eq(const pc_blob *a, const pc_blob *b)
{
    size_t n = pc_blob_len(a);
    return n == pc_blob_len(b) && (n == 0 || memcmp(pc_blob_data(a), pc_blob_data(b), n) == 0);
}

/* Vec<T> clone into a fresh calloc'd array (NULL when n is 0) with the element clone and fini
 * functions; st receives the status, and on failure every cloned element is finalised and
 * out is NULL. */
#define PC_DOC_CLONE_VEC(st, src, n, out, clone_fn, fini_fn, err)                                            \
    do {                                                                                                     \
        (out) = NULL;                                                                                        \
        (st) = PC_OK;                                                                                        \
        if ((n) > 0) {                                                                                       \
            (out) = pc_calloc_array((n), sizeof *(out));                                                     \
            if ((out) == NULL) {                                                                             \
                (st) = pc_fail((err), PC_ERR_NOMEM, "out of memory");                                        \
            } else {                                                                                         \
                size_t pc_k_;                                                                                \
                for (pc_k_ = 0; pc_k_ < (n); pc_k_++) {                                                      \
                    (st) = clone_fn(&(src)[pc_k_], &(out)[pc_k_], (err));                                    \
                    if ((st) != PC_OK) {                                                                     \
                        break;                                                                               \
                    }                                                                                        \
                }                                                                                            \
                if ((st) != PC_OK) {                                                                         \
                    while (pc_k_ > 0) {                                                                      \
                        pc_k_--;                                                                             \
                        fini_fn(&(out)[pc_k_]);                                                              \
                    }                                                                                        \
                    free(out);                                                                               \
                    (out) = NULL;                                                                            \
                }                                                                                            \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

/* Vec<T> equality with the element eq function. */
#define PC_DOC_EQ_VEC(res, a, na, b, nb, eq_fn)                                                              \
    do {                                                                                                     \
        (res) = (na) == (nb);                                                                                \
        if (res) {                                                                                           \
            size_t pc_k_;                                                                                    \
            for (pc_k_ = 0; pc_k_ < (na); pc_k_++) {                                                         \
                if (!eq_fn(&(a)[pc_k_], &(b)[pc_k_])) {                                                      \
                    (res) = false;                                                                           \
                    break;                                                                                   \
                }                                                                                            \
            }                                                                                                \
        }                                                                                                    \
    } while (0)

/* Vec<T> fini: each element, then the array (pointer and count reset). */
#define PC_DOC_FINI_VEC(arr, n, fini_fn)                                                                     \
    do {                                                                                                     \
        size_t pc_k_;                                                                                        \
        if ((arr) != NULL) {                                                                                 \
            for (pc_k_ = 0; pc_k_ < (n); pc_k_++) {                                                          \
                fini_fn(&(arr)[pc_k_]);                                                                      \
            }                                                                                                \
        }                                                                                                    \
        free(arr);                                                                                           \
        (arr) = NULL;                                                                                        \
        (n) = 0;                                                                                             \
    } while (0)

/* Every layer id in the tree, sorted ascending (malloc'd; NULL when empty). */
pc_status pc_doc_sorted_ids(const pc_layer *layers, size_t n, uint64_t **out, size_t *count, pc_error *err);

#endif
