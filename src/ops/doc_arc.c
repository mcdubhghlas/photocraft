/* pc_doc_arc: a reference-counted immutable document (Rust Arc<Document>), and layer targets. */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ops/ops_internal.h"

struct pc_doc_arc {
    atomic_size_t refs;
    pc_document doc;
};

pc_status pc_doc_arc_new(pc_document *doc, pc_doc_arc **out, pc_error *err)
{
    pc_doc_arc *a;
    if (doc == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document");
    }
    a = pc_ops_malloc(sizeof *a);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    atomic_init(&a->refs, 1);
    a->doc = *doc;
    memset(doc, 0, sizeof *doc);
    *out = a;
    return PC_OK;
}

pc_status pc_doc_arc_from_clone(const pc_document *doc, pc_doc_arc **out, pc_error *err)
{
    pc_document copy;
    pc_status st;
    if (doc == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document");
    }
    st = pc_document_clone(doc, &copy, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_doc_arc_new(&copy, out, err);
    if (st != PC_OK) {
        pc_document_fini(&copy);
    }
    return st;
}

pc_doc_arc *pc_doc_arc_retain(pc_doc_arc *a)
{
    if (a != NULL) {
        atomic_fetch_add(&a->refs, 1);
    }
    return a;
}

void pc_doc_arc_release(pc_doc_arc *a)
{
    if (a != NULL && atomic_fetch_sub(&a->refs, 1) == 1) {
        pc_document_fini(&a->doc);
        free(a);
    }
}

const pc_document *pc_doc_arc_get(const pc_doc_arc *a)
{
    return a != NULL ? &a->doc : NULL;
}

size_t pc_doc_arc_refs(const pc_doc_arc *a)
{
    return a != NULL ? atomic_load(&a->refs) : 0;
}

pc_status pc_doc_arc_make_mut(pc_doc_arc **a, pc_document **out, pc_error *err)
{
    pc_doc_arc *copy;
    pc_status st;
    if (a == NULL || *a == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document");
    }
    /* Sole owner: nobody else can retain it concurrently (they'd need a reference). */
    if (atomic_load(&(*a)->refs) == 1) {
        *out = &(*a)->doc;
        return PC_OK;
    }
    st = pc_doc_arc_from_clone(&(*a)->doc, &copy, err);
    if (st != PC_OK) {
        return st;
    }
    pc_doc_arc_release(*a);
    *a = copy;
    *out = &copy->doc;
    return PC_OK;
}

/* ---- layer targets ---- */

void pc_layer_target_fini(pc_layer_target *t)
{
    if (t != NULL) {
        free(t->selected);
        memset(t, 0, sizeof *t);
    }
}

pc_status pc_layer_target_clone(const pc_layer_target *t, pc_layer_target *out, pc_error *err)
{
    if (t == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no layer target");
    }
    return pc_ops_layer_target_clone(t, out, err);
}

bool pc_layer_target_eq(const pc_layer_target *a, const pc_layer_target *b)
{
    if (a->has_active != b->has_active || (a->has_active && a->active != b->active) ||
        a->nselected != b->nselected) {
        return false;
    }
    for (size_t i = 0; i < a->nselected; i++) {
        if (a->selected[i] != b->selected[i]) {
            return false;
        }
    }
    return true;
}
