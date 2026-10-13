/* Helpers of I3's PSD export tests (test_psd_export.c, test_psd_layered.c): the integration
 * tests' roundtrip, assert_docs_eq / assert_layers_eq, max_diff and strict_block_errors
 * (crates/io/tests/common/mod.rs, roundtrip.rs). Setup failures abort (test code only). */
#ifndef PC_TESTS_IO_PSD_RT_H
#define PC_TESTS_IO_PSD_RT_H

#include "common.h"

#include "io/psd_import.h"

/* export(doc, name) as bytes (aborts on failure). */
static inline pc_buf rt_export(const pc_document *d, const char *name, const pc_io_export_opts *o)
{
    pc_io_export_result r;
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    if (pc_io_export(d, name, o, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "export %s: %s\n", name, e.msg);
        abort();
    }
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    pc_io_export_result_fini(&r);
    return b;
}

static inline pc_document rt_import(const char *name, const pc_buf *b)
{
    pc_io_import_result r;
    pc_error e;
    pc_document d;
    if (pc_io_import(name, b->data, b->len, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "import %s: %s\n", name, e.msg);
        abort();
    }
    d = r.document;
    memset(&r.document, 0, sizeof r.document);
    pc_io_warnings_fini(&r.warnings);
    return d;
}

/* roundtrip(): export to PSD, check the bytes are stable through the psd model, import. */
static inline pc_document rt_roundtrip(const pc_document *d)
{
    pc_buf b = rt_export(d, "x.psd", NULL);
    pc_psd_file f;
    pc_psd_bytes again = {NULL, 0};
    pc_document back;
    must(pc_psd_file_parse(b.data, b.len, &f, NULL));
    must(pc_psd_file_to_bytes(&f, &again, NULL));
    CHECK(again.len == b.len && memcmp(again.data, b.data, b.len) == 0);
    pc_psd_bytes_fini(&again);
    pc_psd_file_fini(&f);
    back = rt_import("x.psd", &b);
    pc_buf_fini(&b);
    return back;
}

static inline void rt_surface_eq(const pc_surface *a0, const pc_surface *b0, const char *ctx)
{
    pc_surface a;
    pc_surface b;
    pc_rect r;
    must(pc_surface_clone(a0, &a, NULL));
    must(pc_surface_clone(b0, &b, NULL));
    pc_surface_prune(&a);
    pc_surface_prune(&b);
    if (!pc_pixel_format_eq(pc_surface_format(&a), pc_surface_format(&b))) {
        PC_T_FAIL("%s: format", ctx);
    } else {
        float da[PC_RASTER_MAX_CHANNELS];
        float db[PC_RASTER_MAX_CHANNELS];
        pc_surface_default_pixel(&a, da);
        pc_surface_default_pixel(&b, db);
        if (memcmp(da, db, pc_surface_channels(&a) * sizeof(float)) != 0) {
            PC_T_FAIL("%s: default pixel", ctx);
        }
        if (!pc_rect_eq(pc_surface_content_bounds(&a), pc_surface_content_bounds(&b))) {
            PC_T_FAIL("%s: bounds", ctx);
        }
        r = pc_rect_union(pc_surface_content_bounds(&a), pc_surface_content_bounds(&b));
        if (!pc_rect_is_empty(r)) {
            uint8_t *x = NULL;
            uint8_t *y = NULL;
            size_t nx = 0;
            size_t ny = 0;
            must(pc_surface_to_interleaved(&a, r, &x, &nx, NULL));
            must(pc_surface_to_interleaved(&b, r, &y, &ny, NULL));
            if (nx != ny || memcmp(x, y, nx) != 0) {
                PC_T_FAIL("%s: pixels differ", ctx);
            }
            free(x);
            free(y);
        }
    }
    pc_t_checks++;
    pc_surface_fini(&a);
    pc_surface_fini(&b);
}

static inline bool rt_has_block(const pc_layer *l, const pc_doc_psd_block *blk)
{
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        if (memcmp(l->psd_blocks[i].key, blk->key, 4) == 0 && pc_blob_eq(l->psd_blocks[i].data, blk->data)) {
            return true;
        }
    }
    return false;
}

/* assert_layers_eq. */
static inline void rt_layers_eq(const pc_layer *a, const pc_layer *b, const char *path, int depth)
{
    char ctx[512];
    (void)snprintf(ctx, sizeof ctx, "%s/%s", path, pc_str_c(&a->name));
    pc_t_checks++;
    if (!pc_str_eq(&a->name, &b->name) || a->visible != b->visible || !pc_locks_eq(a->locks, b->locks) ||
        a->blend != b->blend || a->opacity != b->opacity || a->fill_opacity != b->fill_opacity ||
        a->clipped != b->clipped || a->label != b->label) {
        PC_T_FAIL("%s: basic fields", ctx);
    }
    if (!pc_advanced_blending_eq(&a->advanced, &b->advanced) ||
        a->excluded_channels != b->excluded_channels || !pc_blend_if_eq(&a->blend_if, &b->blend_if)) {
        PC_T_FAIL("%s: blending options", ctx);
    }
    if (!pc_blob_eq(a->effects.psd_raw, b->effects.psd_raw) || a->effects.enabled != b->effects.enabled) {
        PC_T_FAIL("%s: effects", ctx);
    }
    if (a->has_psd_id && (!b->has_psd_id || a->psd_id != b->psd_id)) {
        PC_T_FAIL("%s: psd id", ctx);
    }
    for (size_t i = 0; i < a->npsd_blocks; i++) {
        if (!rt_has_block(b, &a->psd_blocks[i])) {
            PC_T_FAIL("%s: preserved block %.4s missing", ctx, (const char *)a->psd_blocks[i].key);
        }
    }
    if (a->has_fill_cache) {
        if (!b->has_fill_cache) {
            PC_T_FAIL("%s: fill cache lost", ctx);
        } else {
            rt_surface_eq(&a->fill_cache.surface, &b->fill_cache.surface, ctx);
        }
    }
    if (a->has_mask != b->has_mask) {
        PC_T_FAIL("%s: mask presence differs", ctx);
    } else if (a->has_mask) {
        if (a->mask.enabled != b->mask.enabled || a->mask.linked != b->mask.linked ||
            a->mask.density != b->mask.density || a->mask.feather != b->mask.feather) {
            PC_T_FAIL("%s: mask fields", ctx);
        }
        rt_surface_eq(&a->mask.surface, &b->mask.surface, ctx);
    }
    if (a->content.kind != b->content.kind) {
        PC_T_FAIL("%s: kind %s vs %s", ctx, pc_layer_content_kind_name(&a->content),
                  pc_layer_content_kind_name(&b->content));
        return;
    }
    switch (a->content.kind) {
    case PC_LAYER_RASTER:
        rt_surface_eq(&a->content.u.raster, &b->content.u.raster, ctx);
        break;
    case PC_LAYER_GROUP: {
        const pc_group *x = &a->content.u.group;
        const pc_group *y = &b->content.u.group;
        if (x->expanded != y->expanded || x->nchildren != y->nchildren) {
            PC_T_FAIL("%s: group", ctx);
            break;
        }
        for (size_t i = 0; i < x->nchildren && depth < 64; i++) {
            rt_layers_eq(&x->children[i], &y->children[i], ctx, depth + 1);
        }
        break;
    }
    case PC_LAYER_ADJUSTMENT:
        if (!pc_adjustment_eq(&a->content.u.adjustment, &b->content.u.adjustment)) {
            PC_T_FAIL("%s: adjustment", ctx);
        }
        break;
    case PC_LAYER_FILL:
        if (a->content.u.fill.kind == PC_FILL_SOLID && b->content.u.fill.kind == PC_FILL_SOLID) {
            const pc_color *x = &a->content.u.fill.u.solid;
            const pc_color *y = &b->content.u.fill.u.solid;
            bool close = x->mode == y->mode;
            for (int i = 0; i < 4 && close; i++) {
                close = fabsf(x->c[i] - y->c[i]) < 1e-4f;
            }
            if (!close) {
                PC_T_FAIL("%s: colour", ctx);
            }
        } else if (!pc_fill_eq(&a->content.u.fill, &b->content.u.fill)) {
            PC_T_FAIL("%s: fill", ctx);
        }
        break;
    case PC_LAYER_TEXT:
        if (!pc_str_eq(&a->content.u.text.text, &b->content.u.text.text)) {
            PC_T_FAIL("%s: text", ctx);
        }
        rt_surface_eq(&a->content.u.text.cache, &b->content.u.text.cache, ctx);
        break;
    case PC_LAYER_SHAPE:
        rt_surface_eq(&a->content.u.shape.cache, &b->content.u.shape.cache, ctx);
        break;
    case PC_LAYER_SMART:
        rt_surface_eq(&a->content.u.smart.cache, &b->content.u.smart.cache, ctx);
        break;
    default:
        break;
    }
}

/* assert_docs_eq. */
static inline void rt_docs_eq(const pc_document *a, const pc_document *b)
{
    pc_t_checks++;
    if (a->size.width != b->size.width || a->size.height != b->size.height || a->mode != b->mode ||
        a->depth != b->depth || fabsf(a->resolution_dpi - b->resolution_dpi) >= 1e-3f) {
        PC_T_FAIL("document header");
    }
    if (!pc_blob_eq(a->icc_profile, b->icc_profile)) {
        PC_T_FAIL("icc");
    }
    if (!pc_metadata_eq(&a->metadata, &b->metadata)) {
        PC_T_FAIL("metadata");
    }
    if (!pc_guides_eq(&a->guides, &b->guides)) {
        PC_T_FAIL("guides");
    }
    if (a->nchannels != b->nchannels) {
        PC_T_FAIL("alpha channels %zu vs %zu", a->nchannels, b->nchannels);
    } else {
        for (size_t i = 0; i < a->nchannels; i++) {
            CHECK(pc_str_eq(&a->channels[i].name, &b->channels[i].name));
            rt_surface_eq(&a->channels[i].surface, &b->channels[i].surface, pc_str_c(&a->channels[i].name));
        }
    }
    if (a->nlayers != b->nlayers) {
        PC_T_FAIL("root layer count %zu vs %zu", a->nlayers, b->nlayers);
        return;
    }
    for (size_t i = 0; i < a->nlayers; i++) {
        rt_layers_eq(&a->layers[i], &b->layers[i], "", 0);
    }
}

/* max_diff: largest per-channel difference (premultiplied colour, alpha) where either pixel
 * shows. */
static inline float rt_max_diff(const pc_compose_buffer *a, const pc_compose_buffer *b)
{
    float m = 0.0f;
    for (size_t i = 0; i < a->len && i < b->len; i++) {
        const float *p = a->px[i];
        const float *q = b->px[i];
        if (p[3] <= 0.0f && q[3] <= 0.0f) {
            continue;
        }
        for (int c = 0; c < 4; c++) {
            float x = c < 3 ? p[c] * p[3] : p[c];
            float y = c < 3 ? q[c] * q[3] : q[c];
            m = fmaxf(m, fabsf(x - y));
        }
    }
    return m;
}

/* strict_block_errors: the number of layout problems (#200) of a written PSD/PSB; each is
 * reported on stderr. */
static inline size_t rt_strict_block_errors(const uint8_t *bytes, size_t len)
{
    pc_psd_file f;
    pc_error e;
    size_t errs = 0;
    pc_psd_version v;
    if (pc_psd_file_parse(bytes, len, &f, &e) != PC_OK) {
        (void)fprintf(stderr, "parse: %s\n", e.msg);
        return 1;
    }
    v = f.header.version;
    if (f.layer_mask_trailing.len > 0) {
        (void)fprintf(stderr, "%zu unparsed bytes in the layer and mask section\n",
                      f.layer_mask_trailing.len);
        errs++;
    }
    if (f.has_layer_info) {
        uint64_t body = 1;
        uint64_t pad;
        if (pc_psd_layer_info_unpadded_len(&f.layer_info, v, &body, NULL) != PC_OK) {
            body = 1;
        }
        pad = f.layer_info.padding.has ? f.layer_info.padding.v.len : body % 2;
        if ((body + pad) % 4 != 0) {
            (void)fprintf(stderr, "layer info: %llu + %llu not a multiple of 4\n", (unsigned long long)body,
                          (unsigned long long)pad);
            errs++;
        }
    }
    for (size_t i = 0; i < f.nglobal; i++) {
        const pc_psd_block *b = &f.global_blocks[i];
        size_t pad = b->padding.has ? b->padding.v.len : (4 - b->data.len % 4) % 4;
        if ((b->data.len + pad) % 4 != 0 || pc_psd_block_check_structure(b, &e) != PC_OK) {
            (void)fprintf(stderr, "global %.4s: %zu + %zu (%s)\n", (const char *)b->key, b->data.len, pad,
                          e.msg);
            errs++;
        }
    }
    for (size_t i = 0; f.has_layer_info && i < f.layer_info.n; i++) {
        const pc_psd_layer *l = &f.layer_info.layers[i];
        for (size_t k = 0; k < l->nblocks; k++) {
            const pc_psd_block *b = &l->blocks[k];
            if (b->data.len % 2 != 0 || (b->padding.has && b->padding.v.len > 0) ||
                pc_psd_block_check_structure(b, &e) != PC_OK) {
                (void)fprintf(stderr, "layer %zu %.4s: %zu bytes\n", i, (const char *)b->key, b->data.len);
                errs++;
            }
        }
    }
    pc_psd_file_fini(&f);
    return errs;
}

/* The first layer record named name, or NULL. */
static inline const pc_psd_layer *rt_record(const pc_psd_file *f, const char *name)
{
    for (size_t i = 0; f->has_layer_info && i < f->layer_info.n; i++) {
        char *n = NULL;
        bool same;
        must(pc_psd_layer_name(&f->layer_info.layers[i], &n, NULL));
        same = strcmp(n, name) == 0;
        free(n);
        if (same) {
            return &f->layer_info.layers[i];
        }
    }
    return NULL;
}

static inline bool rt_block_is(const pc_psd_block *b, const uint8_t *data, size_t len)
{
    return b != NULL && b->data.len == len && (len == 0 || memcmp(b->data.data, data, len) == 0);
}

#endif
