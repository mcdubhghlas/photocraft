/* PSD layer records -> document layers (port of psd_import.rs's Ctx: record_surface,
 * record_mask, filter_mask, apply_common, layer_from_record, apply_vector_mask, build and
 * apply_link_groups). Problems in the file become warnings; failures are out of memory,
 * cancellation, and functions of later io chunks that are not ported yet. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "io/adjust_map.h"
#include "io/blocks.h"
#include "io/linked.h"
#include "io/comps_map.h"
#include "io/effects_map.h"
#include "io/pixels.h"
#include "io/psd_import.h"
#include "io/psd_import_internal.h"
#include "io/smart_map.h"
#include "io/vector_map.h"
#include "psd/layer.h"
#include "text/psd.h"
#include "vector/vector.h"

#define TRY(x)                                                                                               \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto fail;                                                                                       \
        }                                                                                                    \
    } while (0)

static const pc_psd_block *blk(const pc_psd_layer *rec, const char *key)
{
    return pc_psd_layer_block(rec, key);
}

/* Block lookup callback over a layer record (pc_io_block_lookup / advanced_from_blocks). */
static const uint8_t *rec_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    const pc_psd_block *b = pc_psd_layer_block(ctx, (const char *)key);
    if (b == NULL) {
        return NULL;
    }
    *len = b->data.len;
    return b->data.data != NULL ? b->data.data : (const uint8_t *)"";
}

static bool rect_unusable(pc_psd_rect r, size_t *w, size_t *h)
{
    return pc_psd_rect_is_empty(r) || pc_psd_rect_size(r, w, h, NULL) != PC_OK;
}

static pc_rect doc_rect(pc_psd_rect r)
{
    return pc_rect_make(r.left, r.top, r.right, r.bottom);
}

/* Real-mask metadata without a -3 channel still selects the synthetic -2 mask. */
static const pc_psd_real_mask *selected_real_mask(const pc_psd_layer *rec)
{
    const pc_psd_layer_mask *m;
    if (pc_psd_layer_channel(rec, PC_PSD_CH_REAL_USER_MASK) == NULL) {
        return NULL;
    }
    m = pc_psd_layer_get_mask(rec);
    return m != NULL && m->has_real ? &m->real : NULL;
}

/* A channel's decoded plane; *present false when the record lacks it or it can't be decoded
 * (a warning says so). */
static pc_status channel_plane(pc_io_imp *cx, const pc_psd_layer *rec, int16_t id, const char *name,
                               bool *present, pc_psd_bytes *out, pc_error *err)
{
    pc_error e;
    pc_status st;
    *present = false;
    if (pc_psd_layer_channel(rec, id) == NULL) {
        return PC_OK;
    }
    st = pc_psd_layer_decode_channel(rec, id, cx->file->header.depth, cx->file->header.version, out, &e);
    if (st == PC_ERR_NOMEM) {
        return pc_io_nomem(err, "decoding a layer channel");
    }
    if (st != PC_OK) {
        return pc_io_warn(cx->warnings, err,
                          "layer \"%s\": channel %d could not be decoded (%s); treated as empty", name,
                          (int)id, e.msg);
    }
    *present = true;
    return PC_OK;
}

static pc_status record_surface(pc_io_imp *cx, const pc_psd_layer *rec, const char *name, pc_surface *out,
                                pc_error *err)
{
    pc_psd_rect r = rec->rect;
    size_t w = 0;
    size_t h = 0;
    size_t ch = cx->cc + 1;
    pc_psd_bytes planes[8];
    bool present[8];
    pc_io_plane in[8];
    uint8_t fill[8][4];
    bool invert[8];
    bool missing = false;
    bool any = false;
    uint8_t *bytes = NULL;
    size_t len = 0;
    size_t npx;
    pc_status st = PC_OK;
    memset(planes, 0, sizeof planes);
    memset(present, 0, sizeof present);
    if (rect_unusable(r, &w, &h) || ch > 8) {
        pc_surface_init(out, cx->fmt);
        return PC_OK;
    }
    for (size_t c = 0; c < ch; c++) {
        int16_t id = c < cx->cc ? (int16_t)c : (int16_t)PC_PSD_CH_TRANSPARENCY;
        TRY(channel_plane(cx, rec, id, name, &present[c], &planes[c], err));
        if (!present[c] && pc_psd_layer_channel(rec, id) != NULL) {
            missing = true;
        }
        any = any || present[c];
    }
    /* A channel that is in the file but could not be decoded leaves the layer empty, as the
     * warning says: filling in for it would paint an opaque black layer over the document. */
    if (missing) {
        pc_surface_init(out, cx->fmt);
        goto done;
    }
    /* Only decoded channel data justifies a buffer: a bare rectangle can declare 300000^2
     * pixels in a few bytes (#755). */
    if (!any) {
        st = pc_io_warn(cx->warnings, err,
                        "layer \"%s\": no channel data for its %zux%zu bounds; treated as empty", name, w, h);
        if (st == PC_OK) {
            pc_surface_init(out, cx->fmt);
        }
        goto done;
    }
    for (size_t c = 0; c < ch; c++) {
        in[c] = (pc_io_plane){present[c], planes[c].data, planes[c].len};
        if (c < cx->cc) {
            (void)pc_io_zero_sample(cx->fmt.sample, fill[c]);
        } else {
            (void)pc_io_max_sample(cx->fmt.sample, fill[c]);
        }
        invert[c] = c < cx->cc && cx->cmyk;
    }
    if (!pc_size_mul(w, h, &npx)) {
        st = pc_fail(err, PC_ERR_LIMIT, "limit exceeded: layer of %zux%zu pixels", w, h);
        goto done;
    }
    TRY(pc_io_interleave(in, ch, (const uint8_t (*)[4])fill, npx, cx->fmt.sample, invert, &bytes, &len, err));
    if (cx->fmt.mode == PC_COLOR_MODE_LAB && cx->fmt.sample == PC_SAMPLE_TYPE_U16) {
        pc_io_lab16_chroma(bytes, len, ch, true);
    }
    TRY(pc_surface_from_interleaved(cx->fmt, doc_rect(r), bytes, len, out, err));
    pc_surface_prune(out);
    goto done;
fail:
done:
    free(bytes);
    for (size_t c = 0; c < 8; c++) {
        pc_psd_bytes_fini(&planes[c]);
    }
    return st;
}

static pc_status record_mask(pc_io_imp *cx, const pc_psd_layer *rec, const char *name, bool *has,
                             pc_layer_mask *out, pc_error *err)
{
    const pc_psd_layer_mask *m = pc_psd_layer_get_mask(rec);
    const pc_psd_real_mask *real = selected_real_mask(rec);
    int16_t id;
    pc_psd_rect rect;
    uint8_t def;
    uint8_t flags;
    pc_layer_mask lm;
    float dv;
    size_t w;
    size_t h;
    pc_status st = PC_OK;
    *has = false;
    if (m == NULL) {
        return PC_OK;
    }
    if (real != NULL) {
        id = PC_PSD_CH_REAL_USER_MASK;
        rect = real->rect;
        def = real->background;
        flags = real->flags;
    } else {
        if (pc_psd_layer_channel(rec, PC_PSD_CH_USER_MASK) == NULL) {
            return PC_OK;
        }
        id = PC_PSD_CH_USER_MASK;
        rect = m->rect;
        def = m->default_color;
        flags = m->flags;
    }
    memset(&lm, 0, sizeof lm);
    dv = (float)def / 255.0f;
    pc_surface_init_default(&lm.surface, cx->mask_fmt, &dv, 1);
    if (!rect_unusable(rect, &w, &h)) {
        pc_psd_bytes plane = {NULL, 0};
        bool present = false;
        st = channel_plane(cx, rec, id, name, &present, &plane, err);
        if (st == PC_OK && present) {
            pc_io_plane in = {true, plane.data, plane.len};
            uint8_t fill[1][4];
            bool inv = false;
            uint8_t *bytes = NULL;
            size_t len = 0;
            size_t npx = 0;
            (void)pc_io_zero_sample(cx->mask_fmt.sample, fill[0]);
            if (!pc_size_mul(w, h, &npx)) {
                st = pc_fail(err, PC_ERR_LIMIT, "limit exceeded: mask of %zux%zu pixels", w, h);
            }
            st = st != PC_OK ? st
                             : pc_io_interleave(&in, 1, (const uint8_t (*)[4])fill, npx, cx->mask_fmt.sample,
                                                &inv, &bytes, &len, err);
            st =
                st != PC_OK ? st : pc_surface_write_interleaved(&lm.surface, doc_rect(rect), bytes, len, err);
            free(bytes);
            pc_surface_prune(&lm.surface);
        }
        pc_psd_bytes_fini(&plane);
    }
    if (st != PC_OK) {
        pc_surface_fini(&lm.surface);
        return st;
    }
    lm.enabled = (flags & 2) == 0;
    lm.linked = (flags & 1) == 0;
    lm.density = m->has_params && m->params.has_user_density ? (float)m->params.user_density / 255.0f : 1.0f;
    lm.feather = m->has_params && m->params.has_user_feather ? pc_f64_to_f32(m->params.user_feather) : 0.0f;
    *out = lm;
    *has = true;
    return PC_OK;
}

/* The filter mask of the smart object whose placed id is `placed`, from the FEid cache. */
static pc_status filter_mask(pc_io_imp *cx, const pc_str *placed, const pc_io_filter_stack *stack,
                             const char *name, bool *has, pc_layer_mask *out, pc_error *err)
{
    const pc_psd_fx_item *item = NULL;
    pc_error e;
    pc_status st;
    *has = false;
    for (size_t i = 0; i < cx->nfx; i++) {
        const char *id = cx->fx[i].id != NULL ? cx->fx[i].id : "";
        if (strlen(id) == placed->len && memcmp(id, pc_str_c(placed), placed->len) == 0) {
            item = &cx->fx[i];
            break;
        }
    }
    if (item == NULL) {
        return PC_OK;
    }
    st = pc_io_mask_from_item(item, cx->mask_fmt.sample, stack, has, out, &e);
    if (st == PC_ERR_FORMAT) {
        *has = false;
        return pc_io_warn(cx->warnings, err, "layer \"%s\": %s; the filter mask was ignored", name, e.msg);
    }
    if (st != PC_OK && err != NULL) {
        *err = e;
    }
    return st;
}

static pc_status apply_common(pc_io_imp *cx, pc_layer *l, const pc_psd_layer *rec,
                              const uint8_t *blend_override, pc_error *err)
{
    const uint8_t *key = blend_override != NULL ? blend_override : rec->blend;
    const pc_psd_block *b;
    const pc_psd_block *single;
    const pc_psd_block *fx;
    uint32_t u32;
    uint16_t u16;
    pc_status st = PC_OK;
    l->visible = pc_psd_layer_is_visible(rec);
    l->opacity = (float)rec->opacity / 255.0f;
    l->fill_opacity = (float)pc_psd_layer_fill_opacity(rec) / 255.0f;
    l->clipped = rec->clipping != 0;
    if (!pc_blend_from_psd_key((const char *)key, &l->blend)) {
        char q[64];
        pc_utf8_debug_quote(key, 4, q, sizeof q);
        l->blend = PC_BLEND_NORMAL;
        TRY(pc_io_warn(cx->warnings, err, "layer \"%s\": unknown blend mode %s; using Normal",
                       pc_str_c(&l->name), q));
    }
    if ((b = blk(rec, "lspf")) != NULL && pc_psd_block_u32(b, &u32, NULL) == PC_OK) {
        l->locks = pc_io_locks_from_lspf(u32);
    }
    if (rec->flags & PC_PSD_LF_TRANSPARENCY_PROTECTED) {
        l->locks.transparency = true;
    }
    if ((b = blk(rec, "lclr")) != NULL && pc_psd_block_u16(b, &u16, NULL) == PC_OK) {
        l->label = pc_io_label_from_index(u16);
    }
    /* lmfx (multiple instances per kind) supersedes lfx2 when present; it stays in psd_blocks,
     * lfx2 in Effects::psd_raw. Groups store their effects under lfxs (same layout). */
    single = blk(rec, "lfx2") != NULL ? blk(rec, "lfx2") : blk(rec, "lfxs");
    fx = blk(rec, "lmfx") != NULL ? blk(rec, "lmfx") : single;
    if (fx != NULL) {
        pc_effects e;
        bool found = false;
        memset(&e, 0, sizeof e);
        TRY(pc_io_parse_lfx2(fx->data.data, fx->data.len, &found, &e.enabled, &e.items, &e.nitems, err));
        if (!found) {
            TRY(pc_io_effects_enabled(fx->data.data, fx->data.len, &e.enabled, err));
        }
        if (single != NULL) {
            st = pc_blob_new(single->data.data, single->data.len, &e.psd_raw, err);
            if (st != PC_OK) {
                pc_effects_fini(&e);
                goto fail;
            }
        }
        pc_effects_fini(&l->effects);
        l->effects = e;
    } else if ((b = blk(rec, "lrFX")) != NULL) {
        uint32_t sec = pc_psd_layer_section_type(rec);
        /* Legacy effects only on non-group layers (Photoshop ignores them on groups). */
        if (sec != PC_PSD_SECTION_OPEN && sec != PC_PSD_SECTION_CLOSED) {
            pc_effects e;
            bool found = false;
            memset(&e, 0, sizeof e);
            TRY(pc_io_parse_lrfx(b->data.data, b->data.len, &found, &e.enabled, &e.items, &e.nitems, err));
            if (found) {
                pc_effects_fini(&l->effects);
                l->effects = e;
            }
        }
    }
    /* Effects reference point (fxrp: two f64, x then y); also left in psd_blocks. */
    if ((b = blk(rec, "fxrp")) != NULL && b->data.len >= 16) {
        for (size_t k = 0; k < 2; k++) {
            uint64_t u = 0;
            double v;
            for (size_t i = 0; i < 8; i++) {
                u = u << 8 | b->data.data[k * 8 + i];
            }
            memcpy(&v, &u, 8);
            l->effects.reference[k] = v;
        }
        l->effects.has_reference = true;
    }
    /* Advanced Blending channel restrictions (brst); also kept in psd_blocks. */
    if ((b = blk(rec, "brst")) != NULL) {
        l->excluded_channels = pc_io_parse_brst(b->data.data, b->data.len);
    }
    l->advanced = pc_io_advanced_from_blocks(rec_lookup, (void *)(uintptr_t)rec);
    /* Blend If lives in the layer record's blending ranges. */
    pc_blend_if_fini(&l->blend_if);
    TRY(pc_io_blend_if_from_ranges(rec->blending_ranges.data, rec->blending_ranges.len, &l->blend_if, err));
    l->has_psd_id = pc_psd_layer_id(rec, &l->psd_id);
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
        l->has_mask = false;
    }
    TRY(record_mask(cx, rec, pc_str_c(&l->name), &l->has_mask, &l->mask, err));
fail:
    return st;
}

/* Blocks regenerated from document fields on export are not kept in psd_blocks. */
const char PC_IO_REGENERATED[9][5] = {"luni", "lyid", "lsct", "lsdk", "iOpa", "lspf", "lclr", "lfx2", "lfxs"};

static bool regenerated(const uint8_t key[4])
{
    for (size_t i = 0; i < 9; i++) {
        if (memcmp(key, PC_IO_REGENERATED[i], 4) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status preserved_blocks(const pc_psd_layer *rec, pc_doc_psd_block **out, size_t *n, pc_error *err)
{
    pc_doc_psd_block *v = pc_malloc_array(rec->nblocks > 0 ? rec->nblocks : 1, sizeof *v);
    size_t k = 0;
    if (v == NULL) {
        return pc_io_nomem(err, "keeping a layer's blocks");
    }
    for (size_t i = 0; i < rec->nblocks; i++) {
        const pc_psd_block *b = &rec->blocks[i];
        pc_status st;
        if (regenerated(b->key)) {
            continue;
        }
        memcpy(v[k].key, b->key, 4);
        v[k].data = NULL;
        st = pc_io_set_blob(&v[k].data, b->data.data, b->data.len, err);
        if (st != PC_OK) {
            for (size_t j = 0; j < k; j++) {
                pc_blob_release(v[j].data);
            }
            free(v);
            return st;
        }
        k++;
    }
    if (k == 0) {
        free(v);
        v = NULL;
    }
    *out = v;
    *n = k;
    return PC_OK;
}

/* psd_raw shares the blob of the matching psd_blocks entry. */
static pc_blob *principal(const pc_doc_psd_block *blocks, size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++) {
        if (memcmp(blocks[i].key, key, 4) == 0) {
            return pc_blob_retain(blocks[i].data);
        }
    }
    return NULL;
}

static const char *first_key(const pc_psd_layer *rec, const char *const *keys, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (blk(rec, keys[i]) != NULL) {
            return keys[i];
        }
    }
    return NULL;
}

/* The fill of a plain shape layer (fill block + vector path, no stroke) to import as a fill layer
 * with a vector mask: when its mask parameters give the vector mask a density below 100 % or a
 * feather, or when it stores no pixels and fills with a pattern. */
static pc_status soft_shape_fill(const pc_psd_layer *rec, bool has_vector, const char *fill_key, bool *found,
                                 bool *soft_out, pc_fill *out, pc_error *err)
{
    const pc_psd_block *b;
    const pc_psd_layer_mask *m;
    pc_fill f;
    bool ok = false;
    bool soft;
    bool pattern;
    pc_status st;
    *found = false;
    *soft_out = false;
    if (!has_vector || blk(rec, "vstk") != NULL || blk(rec, "vscg") != NULL || fill_key == NULL) {
        return PC_OK;
    }
    b = blk(rec, fill_key);
    st = pc_io_parse_fill((const uint8_t *)fill_key, b->data.data, b->data.len, &ok, &f, err);
    if (st != PC_OK || !ok) {
        return st;
    }
    m = pc_psd_layer_get_mask(rec);
    soft = m != NULL && m->has_params &&
           ((m->params.has_vector_density && m->params.vector_density < 255) ||
            (m->params.has_vector_feather && m->params.vector_feather > 0.0));
    /* A pattern-filled shape can't be rasterized on its own (the compositor resolves the
     * document's patterns for fill layers), and its stored pixels may be the unclipped fill: it
     * is a pattern fill layer seen through its vector mask (matches the reference's #1907). */
    pattern = f.kind == PC_FILL_PATTERN;
    if (soft || pattern) {
        *out = f;
        *found = true;
        *soft_out = soft;
    } else {
        pc_fill_fini(&f);
    }
    return PC_OK;
}

static pc_io_adj_channels adj_channels(pc_color_mode m)
{
    switch (m) {
    case PC_COLOR_MODE_RGB:
        return PC_IO_ADJ_RGB;
    case PC_COLOR_MODE_GRAYSCALE:
        return PC_IO_ADJ_GRAY;
    case PC_COLOR_MODE_CMYK:
        return PC_IO_ADJ_CMYK;
    case PC_COLOR_MODE_LAB:
        return PC_IO_ADJ_LAB;
    default:
        return PC_IO_ADJ_OTHER;
    }
}

static pc_status text_content(pc_io_imp *cx, const pc_psd_layer *rec, const char *name,
                              const pc_doc_psd_block *blocks, size_t nb, pc_layer_content *c, pc_error *err)
{
    const pc_psd_block *b = blk(rec, "TySh");
    pc_text_layer t;
    pc_surface cache;
    pc_rect bounds;
    bool drawn;
    pc_status st = pc_text_layer_from_tysh(b->data.data, b->data.len, cx->dpi, &t, NULL);
    if (st == PC_ERR_NOMEM) {
        return pc_io_nomem(err, "reading a type layer");
    }
    if (st != PC_OK) {
        bool found = false;
        pc_text_layer_default(&t);
        st = pc_io_parse_tysh(b->data.data, b->data.len, &found, &t.text, &t.transform, err);
        if (st != PC_OK) {
            pc_text_layer_fini(&t);
            return st;
        }
    }
    if (cx->has_txt2) {
        st = pc_txt2_apply(&t, b->data.data, b->data.len, &cx->txt2, err);
        if (st != PC_OK) {
            pc_text_layer_fini(&t);
            return st;
        }
    }
    /* Photoshop's pixels are the cache only when the file has some: other writers leave type
     * layers without image data (Photoshop re-renders them on open), and the import renders such
     * a layer from its model instead (text_import). When the engine text itself is blank,
     * Photoshop draws nothing either: keep the empty pixels. */
    st = record_surface(cx, rec, name, &cache, err);
    if (st != PC_OK) {
        pc_text_layer_fini(&t);
        return st;
    }
    bounds = pc_surface_content_bounds(&cache);
    drawn = !pc_rect_is_empty(bounds);
    if (!drawn) {
        st = pc_tysh_engine_text_is_blank(b->data.data, b->data.len, &drawn, err);
        if (st != PC_OK) {
            pc_surface_fini(&cache);
            pc_text_layer_fini(&t);
            return st;
        }
    }
    if (drawn) {
        t.has_cache = true;
        t.cache = cache;
    } else {
        pc_surface_fini(&cache);
    }
    t.psd_raw = principal(blocks, nb, "TySh");
    c->kind = PC_LAYER_TEXT;
    c->u.text = t;
    return PC_OK;
}

/* Source id, transform, projective placement and warp of a placed layer. A PlLd-only record
 * (Photoshop CS2, #2465) stores the placed corners but not the source size, which comes from the
 * embedded file. An empty id leaves the contents unavailable (the stored pixels stay). */
static pc_status read_placement(pc_io_imp *cx, const char *name, const char *k, const pc_psd_block *b,
                                pc_str *id, pc_affine *transform, bool *has_warp, pc_warp *warp,
                                bool *has_persp, double persp[9], pc_error *err)
{
    pc_status st;
    *has_warp = false;
    *has_persp = false;
    *transform = (pc_affine){{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    if (memcmp(k, "PlLd", 4) == 0) {
        bool found = false;
        double corners[8];
        size_t warp_at = 0;
        pc_io_plld_warp_kind kind = PC_IO_PLLD_WARP_NONE;
        bool has_size = false;
        double size[2] = {0.0, 0.0};
        st = pc_io_parse_plld(b->data.data, b->data.len, &found, id, corners, &warp_at, err);
        if (st != PC_OK) {
            return st;
        }
        if (!found) {
            pc_str_fini(id);
            *id = (pc_str)PC_STR_INIT;
            return pc_io_warn(cx->warnings, err,
                              "layer \"%s\": its placed-layer data (PlLd) could not be read; the smart "
                              "object's contents are unavailable",
                              name);
        }
        st = pc_io_parse_plld_warp(b->data.data, b->data.len, &kind, warp, err);
        if (st != PC_OK) {
            pc_str_fini(id);
            *id = (pc_str)PC_STR_INIT;
            return st;
        }
        if (kind == PC_IO_PLLD_WARP_UNREADABLE) {
            /* Re-rendering from the source would unwarp the layer: keep it as saved. */
            pc_str_fini(id);
            *id = (pc_str)PC_STR_INIT;
            return pc_io_warn(cx->warnings, err,
                              "layer \"%s\": its smart object warp (PlLd) could not be read; the layer "
                              "keeps its warped pixels but its contents are unavailable",
                              name);
        }
        *has_warp = kind == PC_IO_PLLD_WARP_SOME;
        st = pc_io_source_size_in_blocks(cx->file->global_blocks, cx->file->nglobal, pc_str_c(id), id->len,
                                         &has_size, size, err);
        if (st != PC_OK) {
            if (*has_warp) {
                pc_warp_fini(warp);
                *has_warp = false;
            }
            pc_str_fini(id);
            *id = (pc_str)PC_STR_INIT;
            return st;
        }
        pc_io_plld_transform(corners, has_size, size[0], size[1], *has_warp ? warp : NULL, transform,
                             has_persp, persp);
        return PC_OK;
    }
    st = pc_io_parse_smart((const uint8_t *)k, b->data.data, b->data.len, id, transform, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_io_parse_placed_warp((const uint8_t *)k, b->data.data, b->data.len, has_warp, warp, err);
    if (st != PC_OK) {
        pc_str_fini(id);
        *id = (pc_str)PC_STR_INIT;
        return st;
    }
    /* Distort / Perspective: the fourth corner (the affine transform drops it). */
    st = pc_io_parse_smart_perspective((const uint8_t *)k, b->data.data, b->data.len, has_persp, persp, err);
    if (st != PC_OK) {
        if (*has_warp) {
            pc_warp_fini(warp);
            *has_warp = false;
        }
        pc_str_fini(id);
        *id = (pc_str)PC_STR_INIT;
    }
    return st;
}

static pc_status smart_content(pc_io_imp *cx, const pc_psd_layer *rec, const char *name, const char *k,
                               const pc_doc_psd_block *blocks, size_t nb, pc_layer_content *c, pc_error *err)
{
    const pc_psd_block *b = blk(rec, k);
    const pc_psd_block *pb = blk(rec, "SoLd") != NULL ? blk(rec, "SoLd") : blk(rec, "SoLE");
    pc_smart_source src;
    pc_smart_object so;
    pc_affine transform;
    pc_io_placed placed;
    pc_io_filter_stack stack;
    pc_surface cache;
    pc_smart_contents_id cid;
    bool has_placed = false;
    bool pl_has_warp = false;
    bool pl_has_persp = false;
    pc_warp pl_warp;
    double pl_persp[9];
    pc_status st;
    memset(&src, 0, sizeof src);
    memset(&placed, 0, sizeof placed);
    memset(&pl_warp, 0, sizeof pl_warp);
    pc_io_filter_stack_init(&stack);
    src.linked = true;
    st = read_placement(cx, name, k, b, &src.path, &transform, &pl_has_warp, &pl_warp, &pl_has_persp, pl_persp,
                         err);
    if (st != PC_OK) {
        if (pl_has_warp) {
            pc_warp_fini(&pl_warp);
        }
        pc_str_fini(&src.path);
        return st;
    }
    /* Placed layers that name the same source identity share one contents id (as crates/io does),
     * so a re-export embeds the shared source once instead of once per layer. */
    st = pc_io_imp_contents_id(cx, pc_str_c(&src.path), src.path.len, &cid, err);
    if (st != PC_OK) {
        if (pl_has_warp) {
            pc_warp_fini(&pl_warp);
        }
        pc_str_fini(&src.path);
        return st;
    }
    /* Smart filters (filterFX in the placed-layer data) and their mask (FEid). */
    if (pb != NULL) {
        st = pc_io_parse_sold(pb->data.data, pb->data.len, &has_placed, &placed, err);
        if (st != PC_OK) {
            if (pl_has_warp) {
                pc_warp_fini(&pl_warp);
            }
            pc_str_fini(&src.path);
            return st;
        }
        if (has_placed && placed.has_stack) {
            stack = placed.stack;
            pc_io_filter_stack_init(&placed.stack);
            placed.has_stack = false;
        }
    }
    pc_smart_object_init(&so, &src, transform, NULL);
    so.contents_id = cid;
    if (has_placed && stack.nfilters > 0) {
        st = filter_mask(cx, &placed.placed, &stack, name, &so.has_filter_mask, &so.filter_mask, err);
    }
    if (has_placed) {
        pc_io_placed_fini(&placed);
    }
    so.smart_filters = stack.filters;
    so.nsmart_filters = stack.nfilters;
    so.filters_enabled = stack.enabled;
    stack.filters = NULL;
    stack.nfilters = 0;
    if (st == PC_OK) {
        st = record_surface(cx, rec, name, &cache, err);
        if (st == PC_OK) {
            so.has_cache = true;
            so.cache = cache;
        }
    }
    so.psd_raw = principal(blocks, nb, k);
    if (pl_has_warp) {
        so.warp = pl_warp;
        so.has_warp = true;
        pl_has_warp = false;
    }
    if (pl_has_persp) {
        memcpy(so.perspective, pl_persp, sizeof so.perspective);
        so.has_perspective = true;
    }
    if (st != PC_OK) {
        if (pl_has_warp) {
            pc_warp_fini(&pl_warp);
        }
        pc_smart_object_fini(&so);
        return st;
    }
    c->kind = PC_LAYER_SMART;
    c->u.smart = so;
    return PC_OK;
}

static pc_status shape_content(pc_io_imp *cx, const pc_psd_layer *rec, const char *name, const char *fill_key,
                               const char *vector_key, const pc_doc_psd_block *blocks, size_t nb,
                               pc_layer_content *c, pc_error *err)
{
    pc_shape_layer sh;
    size_t w;
    size_t h;
    pc_status st = PC_OK;
    memset(&sh, 0, sizeof sh);
    if (fill_key != NULL) {
        const pc_psd_block *b = blk(rec, fill_key);
        TRY(pc_io_parse_fill((const uint8_t *)fill_key, b->data.data, b->data.len, &sh.has_fill, &sh.fill,
                             err));
    }
    TRY(record_surface(cx, rec, name, &sh.cache, err));
    sh.has_cache = true;
    sh.psd_raw = vector_key != NULL ? principal(blocks, nb, vector_key) : NULL;
    TRY(pc_io_shape_from_blocks(&sh, rec_lookup, (void *)(uintptr_t)rec, cx->file->header.width,
                                cx->file->header.height, cx->dpi, err));
    /* No stored pixels (32-bit documents, files saved without layer pixels): render the shape
     * from its path and fill, as Photoshop does when it opens the file. */
    if (rect_unusable(rec->rect, &w, &h) && (sh.path.nsubpaths > 0 || sh.path.inverted)) {
        pc_rect canvas =
            pc_rect_make(0, 0, pc_u32_as_i32(cx->file->header.width), pc_u32_as_i32(cx->file->header.height));
        pc_surface s;
        TRY(pc_vec_render_shape(&sh, cx->fmt, canvas, &s, err));
        pc_surface_fini(&sh.cache);
        sh.cache = s;
    }
    c->kind = PC_LAYER_SHAPE;
    c->u.shape = sh;
    return PC_OK;
fail:
    pc_shape_layer_fini(&sh);
    return st;
}

/* Typed vector mask from vsms / vmsk (non-shape layers). A user mask Photoshop rendered from the
 * vector data (flag bit 3, no separate real mask) is dropped: the compositor rasterizes the
 * vector mask itself. */
static pc_status apply_vector_mask(pc_io_imp *cx, pc_layer *l, const pc_psd_layer *rec, pc_error *err)
{
    const pc_psd_block *b = blk(rec, "vsms") != NULL ? blk(rec, "vsms") : blk(rec, "vmsk");
    const pc_psd_layer_mask *m;
    pc_vector_mask vm;
    bool found = false;
    pc_status st;
    if (b == NULL) {
        return PC_OK;
    }
    st = pc_io_vector_mask_from_block(b->data.data, b->data.len, cx->file->header.width,
                                      cx->file->header.height, &found, &vm, err);
    if (st != PC_OK || !found) {
        return st;
    }
    m = pc_psd_layer_get_mask(rec);
    if (m != NULL) {
        if (m->has_params) {
            vm.density = m->params.has_vector_density ? (float)m->params.vector_density / 255.0f : 1.0f;
            vm.feather = m->params.has_vector_feather ? pc_f64_to_f32(m->params.vector_feather) : 0.0f;
        }
        if ((m->flags & 8) != 0 && selected_real_mask(rec) == NULL && l->has_mask) {
            pc_layer_mask_fini(&l->mask);
            l->has_mask = false;
        }
    }
    if (l->has_vector_mask) {
        pc_vector_mask_fini(&l->vector_mask);
    }
    l->vector_mask = vm;
    l->has_vector_mask = true;
    return PC_OK;
}

/* LayerRecord::name with the exact length (names may hold U+0000): the first luni block that
 * reads, else the legacy name (UTF-8 when valid, else Latin-1). */
static pc_status layer_name(const pc_psd_layer *rec, pc_str *out, pc_error *err)
{
    pc_buf o = PC_BUF_INIT;
    pc_status st = PC_OK;
    for (size_t i = 0; i < rec->nblocks; i++) {
        const pc_psd_block *b = &rec->blocks[i];
        size_t count;
        size_t end;
        if (!pc_psd_block_is(b, "luni") || b->data.len < 4) {
            continue;
        }
        count = (size_t)b->data.data[0] << 24 | (size_t)b->data.data[1] << 16 | (size_t)b->data.data[2] << 8 |
                b->data.data[3];
        if (count > (b->data.len - 4) / 2) {
            continue;
        }
        /* Trailing NULs are stripped (UnicodeString::to_string_lossy). */
        end = count;
        while (end > 0 && b->data.data[4 + 2 * end - 2] == 0 && b->data.data[4 + 2 * end - 1] == 0) {
            end--;
        }
        st = pc_utf16be_lossy(b->data.data + 4, end, &o, err);
        goto done;
    }
    {
        const uint8_t *d = rec->name.data;
        size_t len = rec->name.len;
        size_t i = 0;
        uint32_t cp;
        bool valid = true;
        while (i < len) {
            if (!pc_utf8_next(d, len, &i, &cp)) {
                valid = false;
                break;
            }
        }
        if (valid) {
            st = len > 0 ? pc_buf_append(&o, d, len, err) : PC_OK;
        } else {
            for (i = 0; i < len && st == PC_OK; i++) {
                st = pc_utf8_put(&o, d[i], err); /* Latin-1 */
            }
        }
    }
done:
    if (st == PC_OK) {
        st = pc_str_set(out, (const char *)o.data, o.len, err);
    }
    pc_buf_fini(&o);
    return st;
}

static pc_status init_named(pc_layer *l, const pc_psd_layer *rec, pc_layer_content *c, pc_error *err)
{
    pc_str nm = PC_STR_INIT;
    pc_status st = layer_name(rec, &nm, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_layer_init(l, "", c, err);
    if (st != PC_OK) {
        pc_str_fini(&nm);
        return st;
    }
    pc_str_fini(&l->name);
    l->name = nm;
    return PC_OK;
}

static const char *const FILL_KEYS[3] = {"SoCo", "GdFl", "PtFl"};
static const char *const VECTOR_KEYS[2] = {"vsms", "vmsk"};
/* SoLE (a linked smart object) before PlLd: a PlLd alone is Photoshop CS2's record. */
static const char *const SMART_KEYS[3] = {"SoLd", "SoLE", "PlLd"};

static pc_status layer_from_record(pc_io_imp *cx, const pc_psd_layer *rec, pc_layer *out, pc_error *err)
{
    const char *fill_key = first_key(rec, FILL_KEYS, 3);
    const char *vector_key = first_key(rec, VECTOR_KEYS, 2);
    const char *smart_key = first_key(rec, SMART_KEYS, 3);
    const char *adj_key = NULL;
    char *name = NULL;
    pc_doc_psd_block *blocks = NULL;
    size_t nb = 0;
    pc_layer_content c;
    pc_layer l;
    bool has_fill_cache = false;
    pc_fill_cache fc;
    bool found = false;
    bool soft = false;
    pc_fill sf;
    bool rendered;
    pc_status st;
    memset(&c, 0, sizeof c);
    memset(&l, 0, sizeof l);
    memset(&fc, 0, sizeof fc);
    for (size_t i = 0; i < 16 && adj_key == NULL; i++) {
        if (blk(rec, PC_IO_ADJUSTMENT_KEYS[i]) != NULL) {
            adj_key = PC_IO_ADJUSTMENT_KEYS[i];
        }
    }
    st = pc_psd_layer_name(rec, &name, err);
    if (st != PC_OK) {
        return st;
    }
    TRY(preserved_blocks(rec, &blocks, &nb, err));
    if (adj_key != NULL) {
        const pc_psd_block *b = blk(rec, adj_key);
        const pc_psd_block *cg = blk(rec, "CgEd");
        c.kind = PC_LAYER_ADJUSTMENT;
        TRY(pc_io_adjust_parse(
            (const uint8_t *)adj_key, b->data.data, b->data.len,
            cg != NULL ? (cg->data.data != NULL ? cg->data.data : (const uint8_t *)"") : NULL,
            cg != NULL ? cg->data.len : 0, adj_channels(cx->fmt.mode), &c.u.adjustment, err));
        /* Kept verbatim for saving, but it renders as nothing: say so rather than open the layer
         * silently without its effect (#1763). */
        if (c.u.adjustment.kind == PC_ADJ_UNSUPPORTED) {
            pc_buf reason = PC_BUF_INIT;
            const char *lbl = pc_io_adjust_label((const uint8_t *)adj_key);
            bool why = pc_io_adjust_unreadable_reason((const uint8_t *)adj_key, b->data.data,
                                                      b->data.len, &reason);
            st = why ? pc_io_warn(cx->warnings, err,
                                  "layer \"%s\": its %s settings could not be read (%s); the layer "
                                  "is kept as saved but has no effect",
                                  name, lbl, pc_buf_cstr(&reason))
                     : pc_io_warn(cx->warnings, err,
                                  "layer \"%s\": its %s settings could not be read; the layer is "
                                  "kept as saved but has no effect",
                                  name, lbl);
            pc_buf_fini(&reason);
            if (st != PC_OK) {
                goto fail;
            }
        }
    } else if (blk(rec, "TySh") != NULL) {
        TRY(text_content(cx, rec, name, blocks, nb, &c, err));
    } else if (smart_key != NULL) {
        TRY(smart_content(cx, rec, name, smart_key, blocks, nb, &c, err));
    } else if ((st = soft_shape_fill(rec, vector_key != NULL, fill_key, &found, &soft, &sf, err)) != PC_OK) {
        goto fail;
    } else if (found) {
        /* A shape whose vector mask has a density or feather is a fill layer seen through a soft
         * vector mask: the fill shows beyond the path, which the stored pixels (the shape alone)
         * lack. A non-soft pattern fill keeps its stored pixels as its rendering (a soft mask's
         * pixels lack the fill beyond the path). */
        if (!soft && sf.kind == PC_FILL_PATTERN && !pc_psd_rect_is_empty(rec->rect)) {
            st = pc_fill_clone(&sf, &fc.fill, err);
            if (st == PC_OK) {
                st = record_surface(cx, rec, name, &fc.surface, err);
                if (st != PC_OK) {
                    pc_fill_fini(&fc.fill);
                }
            }
            if (st != PC_OK) {
                pc_fill_fini(&sf);
                goto fail;
            }
            has_fill_cache = true;
        }
        c.kind = PC_LAYER_FILL;
        c.u.fill = sf;
    } else if ((vector_key != NULL && (fill_key != NULL || blk(rec, "vstk") != NULL)) ||
               blk(rec, "vscg") != NULL) {
        TRY(shape_content(cx, rec, name, fill_key, vector_key, blocks, nb, &c, err));
    } else if (fill_key != NULL) {
        const pc_psd_block *b = blk(rec, fill_key);
        bool ok = false;
        pc_fill f;
        TRY(pc_io_parse_fill((const uint8_t *)fill_key, b->data.data, b->data.len, &ok, &f, err));
        if (ok) {
            /* Keep Photoshop's rendering of the fill so it composites exactly. */
            if (!pc_psd_rect_is_empty(rec->rect)) {
                st = pc_fill_clone(&f, &fc.fill, err);
                if (st == PC_OK) {
                    st = record_surface(cx, rec, name, &fc.surface, err);
                    if (st != PC_OK) {
                        pc_fill_fini(&fc.fill);
                    }
                }
                if (st != PC_OK) {
                    pc_fill_fini(&f);
                    goto fail;
                }
                has_fill_cache = true;
            }
            c.kind = PC_LAYER_FILL;
            c.u.fill = f;
        } else {
            char q[8] = {0};
            pc_buf kb = PC_BUF_INIT;
            TRY(pc_utf8_lossy((const uint8_t *)fill_key, 4, &kb, err));
            (void)snprintf(q, sizeof q, "%s", pc_buf_cstr(&kb));
            pc_buf_fini(&kb);
            TRY(pc_io_warn(cx->warnings, err, "layer \"%s\": unreadable %s fill; imported as pixels", name,
                           q));
            c.kind = PC_LAYER_RASTER;
            TRY(record_surface(cx, rec, name, &c.u.raster, err));
        }
    } else {
        c.kind = PC_LAYER_RASTER;
        TRY(record_surface(cx, rec, name, &c.u.raster, err));
    }
    rendered = c.kind == PC_LAYER_SHAPE || c.kind == PC_LAYER_TEXT || c.kind == PC_LAYER_SMART;
    TRY(init_named(&l, rec, &c, err));
    l.psd_blocks = blocks;
    l.npsd_blocks = nb;
    blocks = NULL;
    nb = 0;
    if (has_fill_cache) {
        l.has_fill_cache = true;
        l.fill_cache = fc;
        has_fill_cache = false;
    }
    TRY(apply_common(cx, &l, rec, NULL, err));
    /* Mask flag bit 3: the user mask was rendered from vector data. For shape / text / smart
     * layers the cached pixels already include that coverage: applying it again would
     * double-mask. */
    if (rendered && selected_real_mask(rec) == NULL && pc_psd_layer_get_mask(rec) != NULL &&
        (pc_psd_layer_get_mask(rec)->flags & 8) != 0 && l.has_mask) {
        pc_layer_mask_fini(&l.mask);
        l.has_mask = false;
    }
    if (l.content.kind != PC_LAYER_SHAPE) {
        TRY(apply_vector_mask(cx, &l, rec, err));
    }
    free(name);
    *out = l;
    return PC_OK;
fail:
    pc_layer_fini(&l);
    pc_layer_content_fini(&c);
    if (has_fill_cache) {
        pc_fill_fini(&fc.fill);
        pc_surface_fini(&fc.surface);
    }
    for (size_t i = 0; i < nb; i++) {
        pc_blob_release(blocks[i].data);
    }
    free(blocks);
    free(name);
    return st;
}

/* The group record's artboard (first artboard block present). */
static pc_status group_artboard(const pc_psd_layer *rec, bool *has, pc_artboard *out, pc_error *err)
{
    *has = false;
    for (size_t i = 0; i < 3; i++) {
        const pc_psd_block *b = blk(rec, PC_IO_ARTBOARD_KEYS[i]);
        if (b != NULL) {
            return pc_io_parse_artboard(b->data.data, b->data.len, has, out, err);
        }
    }
    return PC_OK;
}

static void free_layers(pc_layer *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer_fini(&v[i]);
    }
    free(v);
}

static pc_status group_layer(pc_io_imp *cx, const pc_psd_node *node, size_t depth, pc_layer *out,
                             pc_error *err)
{
    const pc_psd_layer *rec = &cx->layers[node->index];
    pc_layer *children = NULL;
    size_t nchildren = 0;
    char *name = NULL;
    pc_psd_section_divider sd;
    bool has_sd;
    bool has_ab = false;
    pc_artboard ab;
    pc_layer l;
    pc_status st = pc_psd_layer_name(rec, &name, err);
    memset(&l, 0, sizeof l);
    memset(&ab, 0, sizeof ab);
    if (st != PC_OK) {
        return st;
    }
    /* The recursion here (and in every later consumer of the tree) is bounded by the document
     * model's nesting cap; a deeper subtree is not imported. */
    if (depth >= PC_MAX_GROUP_DEPTH) {
        TRY(pc_io_warn(cx->warnings, err,
                       "group `%s` nests deeper than %d groups; its contents were not imported", name,
                       PC_MAX_GROUP_DEPTH));
    } else {
        TRY(pc_io_imp_build(cx, node->children, node->n, depth + 1, &children, &nchildren, err));
    }
    has_sd = pc_psd_layer_section_divider(rec, &sd);
    st = group_artboard(rec, &has_ab, &ab, err);
    if (st != PC_OK) {
        free_layers(children, nchildren);
        goto fail;
    }
    st = pc_layer_group(&l, name, children, nchildren, err);
    if (st != PC_OK) {
        free_layers(children, nchildren);
        goto fail;
    }
    {
        pc_str nm = PC_STR_INIT;
        TRY(layer_name(rec, &nm, err));
        pc_str_fini(&l.name);
        l.name = nm;
    }
    l.content.u.group.expanded = !has_sd || sd.kind != PC_PSD_SECTION_CLOSED;
    l.content.u.group.has_artboard = has_ab;
    if (has_ab) {
        l.content.u.group.artboard = ab;
        memset(&ab, 0, sizeof ab);
        has_ab = false;
    }
    TRY(preserved_blocks(rec, &l.psd_blocks, &l.npsd_blocks, err));
    TRY(apply_common(cx, &l, rec, has_sd && sd.has_blend ? sd.blend : NULL, err));
    TRY(apply_vector_mask(cx, &l, rec, err));
    free(name);
    *out = l;
    return PC_OK;
fail:
    if (has_ab) {
        pc_artboard_fini(&ab);
    }
    pc_layer_fini(&l);
    free(name);
    return st;
}

pc_status pc_io_imp_build(pc_io_imp *cx, const pc_psd_node *nodes, size_t nnodes, size_t depth,
                          pc_layer **layers, size_t *n, pc_error *err)
{
    pc_layer *v = NULL;
    size_t k = 0;
    pc_status st = PC_OK;
    if (nnodes > 0) {
        v = pc_malloc_array(nnodes, sizeof *v);
        if (v == NULL) {
            return pc_io_nomem(err, "importing layers");
        }
    }
    for (size_t i = 0; i < nnodes; i++) {
        const pc_psd_node *node = &nodes[i];
        /* A cancelled open stops decoding; the caller discards the partial document. */
        if (pc_interrupt_cancelled(cx->ctl)) {
            st = pc_io_cancelled(err);
            break;
        }
        if (node->index >= cx->nlayers) {
            continue;
        }
        if (node->is_group) {
            st = group_layer(cx, node, depth, &v[k], err);
        } else {
            st = layer_from_record(cx, &cx->layers[node->index], &v[k], err);
            if (st == PC_OK) {
                size_t total = cx->nlayers > 0 ? cx->nlayers : 1;
                cx->done++;
                /* Layers are 10-95 % of an open (the merged image and channels the rest). */
                pc_interrupt_progress(cx->ctl, 0.1f + 0.85f * (float)cx->done / (float)total);
            }
        }
        if (st != PC_OK) {
            break;
        }
        k++;
    }
    if (st != PC_OK) {
        free_layers(v, k);
        return st;
    }
    if (k == 0) {
        free(v);
        v = NULL;
    }
    *layers = v;
    *n = k;
    return PC_OK;
}

void pc_io_imp_link_groups(const pc_psd_node *nodes, size_t nnodes, pc_layer *layers, size_t nl,
                           const uint16_t *ids, size_t nids, size_t depth)
{
    for (size_t i = 0; i < nnodes && i < nl; i++) {
        const pc_psd_node *node = &nodes[i];
        pc_layer *l = &layers[i];
        /* Matches the importer's nesting cap: deeper children were not imported. */
        if (node->is_group && depth < PC_MAX_GROUP_DEPTH && l->content.kind == PC_LAYER_GROUP) {
            pc_io_imp_link_groups(node->children, node->n, l->content.u.group.children,
                                  l->content.u.group.nchildren, ids, nids, depth + 1);
        }
        l->has_link_group = node->index < nids && ids[node->index] != 0;
        l->link_group = l->has_link_group ? ids[node->index] : 0;
    }
}
