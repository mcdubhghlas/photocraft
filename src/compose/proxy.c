/* Downsampled proxy documents (port of crates/compose/src/proxy.rs). */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"

static int64_t div_euclid(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (a % b < 0) {
        q -= 1; /* b > 0 here */
    }
    return q;
}

static int64_t rem_euclid(int64_t a, int64_t b)
{
    int64_t r = a % b;
    return r < 0 ? r + b : r;
}

pc_status pc_compose_downsample(const pc_surface *s, uint32_t k, pc_surface *out, pc_error *err)
{
    pc_pixel_format f = pc_surface_format(s);
    float dp[PC_RASTER_MAX_CHANNELS];
    pc_surface o;
    pc_rect b;
    int64_t kk;
    int64_t x0;
    int64_t x1;
    int64_t y0;
    int64_t y1;
    int64_t oy;
    size_t bpp;
    size_t w;
    uint8_t *row = NULL;
    pc_status st = PC_OK;
    if (k <= 1) {
        return pc_surface_clone(s, out, err);
    }
    pc_surface_default_pixel(s, dp);
    pc_surface_init_default(&o, f, dp, pc_surface_channels(s));
    b = pc_surface_content_bounds(s);
    if (pc_rect_is_empty(b)) {
        *out = o;
        return PC_OK;
    }
    /* Surfaces span at most +-2^30 px; larger factors reduce everything to one pixel anyway. */
    kk = k < (1u << 20) ? (int64_t)k : (int64_t)1 << 20;
    bpp = pc_pixel_format_bytes_per_pixel(f);
    x0 = div_euclid(b.x0, kk);
    x1 = div_euclid(b.x1, kk) + (rem_euclid(b.x1, kk) != 0);
    y0 = div_euclid(b.y0, kk);
    y1 = div_euclid(b.y1, kk) + (rem_euclid(b.y1, kk) != 0);
    if (x1 * kk > INT32_MAX || y1 * kk + 1 > INT32_MAX || x0 * kk < INT32_MIN || y0 * kk < INT32_MIN) {
        pc_surface_fini(&o);
        return pc_fail(err, PC_ERR_LIMIT, "compose: surface too large to downsample");
    }
    w = (size_t)(x1 - x0 > 0 ? x1 - x0 : 0);
    if (w > 0) {
        row = pc_malloc_array(w, bpp);
        if (row == NULL) {
            pc_surface_fini(&o);
            return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        }
    }
    for (oy = y0; oy < y1 && st == PC_OK && row != NULL; oy++) {
        uint8_t *src = NULL;
        size_t len = 0;
        size_t i;
        pc_rect r =
            pc_rect_make((int32_t)(x0 * kk), (int32_t)(oy * kk), (int32_t)(x1 * kk), (int32_t)(oy * kk + 1));
        st = pc_surface_to_interleaved(s, r, &src, &len, err);
        if (st != PC_OK) {
            break;
        }
        for (i = 0; i < w; i++) {
            size_t at = i * (size_t)kk * bpp;
            if (at + bpp <= len) {
                memcpy(row + i * bpp, src + at, bpp);
            }
        }
        free(src);
        st = pc_surface_write_interleaved(
            &o, pc_rect_make((int32_t)x0, (int32_t)oy, (int32_t)x1, (int32_t)oy + 1), row, w * bpp, err);
    }
    free(row);
    if (st != PC_OK) {
        pc_surface_fini(&o);
        return st;
    }
    *out = o;
    return PC_OK;
}

/* Replaces *s by its downsample. */
static pc_status shrink_surface(pc_surface *s, uint32_t k, pc_error *err)
{
    pc_surface d;
    pc_status st = pc_compose_downsample(s, k, &d, err);
    if (st == PC_OK) {
        pc_surface_fini(s);
        *s = d;
    }
    return st;
}

static int32_t edge(int32_t v, uint32_t k)
{
    /* Pixel x samples source x*k: both half-open edges round up, including negatives. */
    int64_t d = (int64_t)k;
    return (int32_t)(div_euclid(v, d) + (rem_euclid(v, d) != 0));
}

/* One layer's own surfaces (not its children). */
static pc_status shrink_one(pc_layer *l, uint32_t k, pc_error *err)
{
    pc_status st = PC_OK;
    if (l->has_mask) {
        st = shrink_surface(&l->mask.surface, k, err);
    }
    if (st == PC_OK && l->has_fill_cache) {
        st = shrink_surface(&l->fill_cache.surface, k, err);
    }
    if (st != PC_OK) {
        return st;
    }
    switch (l->content.kind) {
    case PC_LAYER_RASTER:
        return shrink_surface(&l->content.u.raster, k, err);
    case PC_LAYER_GROUP:
        if (l->content.u.group.has_artboard) {
            pc_rect *r = &l->content.u.group.artboard.rect;
            *r = pc_rect_make(edge(r->x0, k), edge(r->y0, k), edge(r->x1, k), edge(r->y1, k));
        }
        return PC_OK;
    case PC_LAYER_TEXT:
        return l->content.u.text.has_cache ? shrink_surface(&l->content.u.text.cache, k, err) : PC_OK;
    case PC_LAYER_SHAPE:
        return l->content.u.shape.has_cache ? shrink_surface(&l->content.u.shape.cache, k, err) : PC_OK;
    case PC_LAYER_SMART:
        return l->content.u.smart.has_cache ? shrink_surface(&l->content.u.smart.cache, k, err) : PC_OK;
    default:
        return PC_OK;
    }
}

typedef struct frame {
    pc_layer *arr;
    size_t n;
} frame;

pc_status pc_compose_proxy_document(const pc_document *doc, uint32_t k, pc_document *out, pc_error *err)
{
    pc_document p;
    pc_status st = pc_document_clone(doc, &p, err);
    frame *stack = NULL;
    size_t sp = 0;
    size_t cap = 0;
    size_t i;
    if (st != PC_OK) {
        return st;
    }
    if (k <= 1) {
        *out = p;
        return PC_OK;
    }
    p.size = pc_size_make(doc->size.width / k + (doc->size.width % k != 0),
                          doc->size.height / k + (doc->size.height % k != 0));
    /* Every layer of the tree, iteratively (documents can be deep). */
    if (p.nlayers > 0) {
        stack = pc_grow_array(NULL, &cap, 1, sizeof *stack);
        if (stack == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        } else {
            stack[sp++] = (frame){p.layers, p.nlayers};
        }
    }
    while (st == PC_OK && sp > 0) {
        frame f = stack[--sp];
        for (i = 0; i < f.n && st == PC_OK; i++) {
            pc_layer *l = &f.arr[i];
            st = shrink_one(l, k, err);
            if (st == PC_OK && l->content.kind == PC_LAYER_GROUP && l->content.u.group.nchildren > 0) {
                frame *ns = pc_grow_array(stack, &cap, sp + 1, sizeof *stack);
                if (ns == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
                    break;
                }
                stack = ns;
                stack[sp++] = (frame){l->content.u.group.children, l->content.u.group.nchildren};
            }
        }
    }
    free(stack);
    /* Spot channels are printed under Multichannel composites. */
    for (i = 0; i < p.nchannels && st == PC_OK; i++) {
        st = shrink_surface(&p.channels[i].surface, k, err);
    }
    if (st == PC_OK && p.has_selection) {
        st = shrink_surface(&p.selection, k, err);
    }
    if (st != PC_OK) {
        pc_document_fini(&p);
        return st;
    }
    *out = p;
    return PC_OK;
}

bool pc_compose_proxy_faithful(const pc_document *doc)
{
    pc_walk w;
    size_t i;
    bool ok = true;
    if (pc_document_walk(doc, &w, NULL) != PC_OK) {
        return false;
    }
    for (i = 0; i < w.len && ok; i++) {
        const pc_layer *l = w.items[i].layer;
        ok = !pc_compose_has_effects(l) && !(l->has_vector_mask && l->vector_mask.enabled);
    }
    pc_walk_fini(&w);
    return ok;
}
