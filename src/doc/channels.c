/* Port of crates/doc/src/lib.rs, part 3: guides, alpha channels and metadata. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "doc/doc.h"
#include "doc/tree.h"

/* ---- guides, channels, metadata ----------------------------------------------------------- */

static float *dup_floats(const float *src, size_t n)
{
    float *d = pc_malloc_array(n, sizeof *d);
    if (d != NULL && n > 0) {
        memcpy(d, src, n * sizeof *d);
    }
    return d;
}

static bool floats_eq(const float *a, size_t na, const float *b, size_t nb)
{
    size_t i;
    if (na != nb) {
        return false;
    }
    for (i = 0; i < na; i++) {
        if (!(a[i] == b[i])) {
            return false;
        }
    }
    return true;
}

void pc_guides_fini(pc_guides *g)
{
    if (g == NULL) {
        return;
    }
    free(g->horizontal);
    free(g->vertical);
    memset(g, 0, sizeof *g);
}

pc_status pc_guides_clone(const pc_guides *g, pc_guides *out, pc_error *err)
{
    pc_guides c;
    memset(&c, 0, sizeof c);
    if (g->nhorizontal > 0) {
        c.horizontal = dup_floats(g->horizontal, g->nhorizontal);
        c.nhorizontal = g->nhorizontal;
    }
    if (g->nvertical > 0) {
        c.vertical = dup_floats(g->vertical, g->nvertical);
        c.nvertical = g->nvertical;
    }
    if ((g->nhorizontal > 0 && c.horizontal == NULL) || (g->nvertical > 0 && c.vertical == NULL)) {
        pc_guides_fini(&c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying guides");
    }
    *out = c;
    return PC_OK;
}

bool pc_guides_eq(const pc_guides *a, const pc_guides *b)
{
    return floats_eq(a->horizontal, a->nhorizontal, b->horizontal, b->nhorizontal) &&
           floats_eq(a->vertical, a->nvertical, b->vertical, b->nvertical);
}

pc_status pc_alpha_channel_init(pc_alpha_channel *out, const char *name, pc_surface *surface, pc_error *err)
{
    pc_alpha_channel c;
    pc_status s;
    memset(&c, 0, sizeof c);
    s = pc_str_set_c(&c.name, name != NULL ? name : "", err);
    if (s != PC_OK) {
        return s;
    }
    c.surface = *surface;
    memset(surface, 0, sizeof *surface);
    c.color = PC_ALPHA_CHANNEL_DEFAULT_COLOR;
    c.opacity = 0.5f;
    c.indicates = PC_INDICATES_MASKED_AREAS;
    *out = c;
    return PC_OK;
}

void pc_alpha_channel_fini(pc_alpha_channel *c)
{
    if (c == NULL) {
        return;
    }
    pc_str_fini(&c->name);
    pc_surface_fini(&c->surface);
    memset(c, 0, sizeof *c);
}

pc_status pc_alpha_channel_clone(const pc_alpha_channel *c, pc_alpha_channel *out, pc_error *err)
{
    pc_alpha_channel r = *c;
    pc_status s = pc_str_clone(&c->name, &r.name, err);
    if (s != PC_OK) {
        return s;
    }
    s = pc_surface_clone(&c->surface, &r.surface, err);
    if (s != PC_OK) {
        pc_str_fini(&r.name);
        return s;
    }
    *out = r;
    return PC_OK;
}

bool pc_alpha_channel_eq(const pc_alpha_channel *a, const pc_alpha_channel *b)
{
    return pc_str_eq(&a->name, &b->name) && pc_surface_eq(&a->surface, &b->surface) &&
           a->has_spot == b->has_spot &&
           (!a->has_spot ||
            (pc_doc_color_eq(&a->spot_color, &b->spot_color) && a->spot_solidity == b->spot_solidity)) &&
           pc_doc_color_eq(&a->color, &b->color) && a->opacity == b->opacity && a->indicates == b->indicates;
}

float pc_alpha_channel_overlay_coverage(const pc_alpha_channel *c, float v)
{
    return (c->has_spot || c->indicates == PC_INDICATES_SELECTED_AREAS) ? v : 1.0f - v;
}

static void psd_resource_fini(pc_psd_resource *r)
{
    pc_str_fini(&r->name);
    pc_blob_release(r->data);
    r->data = NULL;
}

static pc_status psd_resource_clone(const pc_psd_resource *r, pc_psd_resource *out, pc_error *err)
{
    pc_psd_resource c = *r;
    pc_status s = pc_str_clone(&r->name, &c.name, err);
    if (s != PC_OK) {
        return s;
    }
    c.data = pc_blob_retain(r->data);
    *out = c;
    return PC_OK;
}

static bool psd_resource_eq(const pc_psd_resource *a, const pc_psd_resource *b)
{
    return a->id == b->id && pc_str_eq(&a->name, &b->name) && pc_blob_eq(a->data, b->data);
}

static void global_block_fini(pc_psd_global_block *g)
{
    pc_blob_release(g->data);
    g->data = NULL;
}

static pc_status global_block_clone(const pc_psd_global_block *g, pc_psd_global_block *out, pc_error *err)
{
    (void)err;
    *out = *g;
    out->data = pc_blob_retain(g->data);
    return PC_OK;
}

static bool global_block_eq(const pc_psd_global_block *a, const pc_psd_global_block *b)
{
    return memcmp(a->signature, b->signature, 4) == 0 && memcmp(a->key, b->key, 4) == 0 &&
           pc_blob_eq(a->data, b->data);
}

void pc_metadata_fini(pc_metadata *m)
{
    if (m == NULL) {
        return;
    }
    pc_str_fini(&m->xmp);
    pc_blob_release(m->exif);
    PC_DOC_FINI_VEC(m->psd_resources, m->npsd_resources, psd_resource_fini);
    PC_DOC_FINI_VEC(m->psd_global_blocks, m->npsd_global_blocks, global_block_fini);
    memset(m, 0, sizeof *m);
}

pc_status pc_metadata_clone(const pc_metadata *m, pc_metadata *out, pc_error *err)
{
    pc_metadata c;
    pc_status s;
    memset(&c, 0, sizeof c);
    c.has_xmp = m->has_xmp;
    s = pc_str_clone(&m->xmp, &c.xmp, err);
    if (s != PC_OK) {
        goto fail;
    }
    PC_DOC_CLONE_VEC(s, m->psd_resources, m->npsd_resources, c.psd_resources, psd_resource_clone,
                     psd_resource_fini, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.npsd_resources = m->npsd_resources;
    PC_DOC_CLONE_VEC(s, m->psd_global_blocks, m->npsd_global_blocks, c.psd_global_blocks, global_block_clone,
                     global_block_fini, err);
    if (s != PC_OK) {
        goto fail;
    }
    c.npsd_global_blocks = m->npsd_global_blocks;
    c.exif = pc_blob_retain(m->exif);
    *out = c;
    return PC_OK;
fail:
    pc_metadata_fini(&c);
    return s;
}

bool pc_metadata_eq(const pc_metadata *a, const pc_metadata *b)
{
    bool r1;
    bool r2;
    if (a->has_xmp != b->has_xmp || (a->has_xmp && !pc_str_eq(&a->xmp, &b->xmp)) ||
        !pc_blob_eq(a->exif, b->exif)) {
        return false;
    }
    PC_DOC_EQ_VEC(r1, a->psd_resources, a->npsd_resources, b->psd_resources, b->npsd_resources,
                  psd_resource_eq);
    PC_DOC_EQ_VEC(r2, a->psd_global_blocks, a->npsd_global_blocks, b->psd_global_blocks,
                  b->npsd_global_blocks, global_block_eq);
    return r1 && r2;
}
