/* Shared helpers for tests/io: the integration tests' document generators (crates/io/tests/
 * common/mod.rs, Features::PIXELS) and the one-line summaries rustref io-core-vectors prints.
 * Setup failures abort (test code only). Later io chunks extend this header. */
#ifndef PC_TESTS_IO_COMMON_H
#define PC_TESTS_IO_COMMON_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/base64.h"
#include "base/blob.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/hash.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "codecs/orientation.h"
#include "compose/compose.h"
#include "io/io.h"
#include "raster/pixel.h"

static inline void must(pc_status st)
{
    if (st != PC_OK) {
        abort(); /* test setup only */
    }
}

static inline void *xmalloc(size_t n)
{
    void *p = malloc(n > 0 ? n : 1);
    if (p == NULL) {
        abort();
    }
    return p;
}

/* ---- documents ---- */

static inline float g255(int v)
{
    return (float)v / 255.0f;
}

static inline int64_t rem_euclid(int64_t a, int64_t m)
{
    int64_t r = a % m;
    return r < 0 ? r + m : r;
}

/* Pattern surface of fmt over r: values on the 8-bit grid. */
static inline pc_surface t_pattern(pc_pixel_format fmt, pc_rect r, uint32_t seed, bool alpha)
{
    pc_surface s;
    size_t n = pc_pixel_format_channels(fmt);
    size_t count = (size_t)pc_rect_width(r) * pc_rect_size(r).height * n;
    float *vals = xmalloc(count * sizeof *vals);
    size_t i = 0;
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            for (size_t c = 0; c < n; c++) {
                int64_t v;
                if (c == n - 1 && fmt.alpha) {
                    v = alpha ? rem_euclid((int64_t)(x + y) * 37 + seed, 256) : 255;
                } else {
                    v = rem_euclid((int64_t)(x * 13 + y * 7) + (int64_t)seed * 31 + (int64_t)c * 50, 256);
                }
                vals[i++] = (float)v / 255.0f;
            }
        }
    }
    pc_surface_init(&s, fmt);
    must(pc_surface_write_region(&s, r, vals, count, NULL));
    pc_surface_prune(&s);
    free(vals);
    return s;
}

static inline pc_layer t_raster(const char *name, pc_pixel_format fmt, pc_rect r, uint32_t seed, bool alpha)
{
    pc_surface s = t_pattern(fmt, r, seed, alpha);
    pc_layer l;
    must(pc_io_raster_layer(name, &s, &l, NULL));
    pc_surface_fini(&s);
    return l;
}

static inline pc_surface t_mask_surface(pc_sample_type sample, pc_rect r, float def, uint32_t seed)
{
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, sample, false};
    pc_surface s;
    size_t n = (size_t)pc_rect_width(r) * pc_rect_size(r).height;
    float *vals = xmalloc(n * sizeof *vals);
    for (size_t i = 0; i < n; i++) {
        vals[i] = (float)((i * 29 + seed) % 256) / 255.0f;
    }
    pc_surface_init_default(&s, fmt, &def, 1);
    must(pc_surface_write_region(&s, r, vals, n, NULL));
    pc_surface_prune(&s);
    free(vals);
    return s;
}

static inline void t_push(pc_document *d, pc_layer *l)
{
    must(pc_io_push_layer(d, l, NULL));
}

static inline pc_document t_doc(const char *name, uint32_t w, uint32_t h, pc_color_mode mode,
                                pc_sample_type depth)
{
    pc_document d;
    must(pc_document_init(&d, name, pc_size_make(w, h), mode, depth, NULL));
    return d;
}

/* gen_doc(mode, depth, Features::PIXELS). */
static inline pc_document t_gen_doc(pc_color_mode mode, pc_sample_type depth)
{
    pc_document d = t_doc("gen", 24, 16, mode, depth);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_layer l;
    pc_layer *kids;
    size_t ni = 0;
    l = t_raster("Background", fmt, pc_document_bounds(&d), 1, false);
    l.locks.transparency = true;
    l.locks.position = true;
    t_push(&d, &l);
    l = t_raster("Negative \xF0\x9F\x98\x80", fmt, pc_rect_make(-5, -3, 7, 6), 2, true);
    l.opacity = g255(200);
    l.fill_opacity = g255(128);
    l.blend = PC_BLEND_MULTIPLY;
    l.label = PC_LABEL_BLUE;
    l.advanced.blend_interior = true;
    l.advanced.transparency_shapes = false;
    t_push(&d, &l);
    l = t_raster("Hidden", fmt, pc_rect_make(3, 3, 9, 9), 3, true);
    l.visible = false;
    t_push(&d, &l);
    l = t_raster("Clipped", fmt, pc_rect_make(0, 0, 10, 10), 4, true);
    l.clipped = true;
    l.blend = PC_BLEND_SCREEN;
    t_push(&d, &l);
    must(pc_layer_raster(&l, "Empty", fmt, NULL));
    t_push(&d, &l);
    l = t_raster("Masked", fmt, pc_rect_make(2, 2, 14, 12), 5, true);
    l.has_mask = true;
    l.mask.surface = t_mask_surface(depth, pc_rect_make(4, 4, 10, 9), 1.0f, 1);
    l.mask.enabled = true;
    l.mask.linked = true;
    l.mask.density = 1.0f;
    l.mask.feather = 0.0f;
    l.advanced.layer_mask_hides_effects = true;
    l.advanced.vector_mask_hides_effects = true;
    t_push(&d, &l);
    l = t_raster("Masked2", fmt, pc_rect_make(-2, 1, 6, 5), 6, true);
    l.has_mask = true;
    l.mask.surface = t_mask_surface(depth, pc_rect_make(-1, 0, 3, 4), 0.0f, 2);
    l.mask.enabled = false;
    l.mask.linked = false;
    l.mask.density = g255(128);
    l.mask.feather = 2.5f;
    t_push(&d, &l);
    {
        pc_layer inner;
        pc_layer outer;
        kids = xmalloc(sizeof *kids);
        kids[0] = t_raster("Inner child", fmt, pc_rect_make(1, 1, 5, 5), 7, true);
        must(pc_layer_group(&inner, "Inner", kids, 1, NULL));
        inner.blend = PC_BLEND_MULTIPLY;
        inner.opacity = g255(180);
        inner.content.u.group.expanded = false;
        inner.advanced.knockout = PC_KNOCKOUT_SHALLOW;
        kids = xmalloc(2 * sizeof *kids);
        kids[0] = t_raster("Outer child", fmt, pc_rect_make(6, 2, 12, 8), 8, true);
        kids[1] = inner;
        must(pc_layer_group(&outer, "Outer", kids, 2, NULL));
        outer.locks.position = true;
        outer.label = PC_LABEL_RED;
        outer.advanced.knockout = PC_KNOCKOUT_DEEP;
        outer.advanced.blend_clipped = false;
        t_push(&d, &outer);
        must(pc_layer_group(&l, "Empty group", NULL, 0, NULL));
        t_push(&d, &l);
    }
    for (size_t i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
        pc_blend_mode m = PC_BLEND_LAYER_MODES[i];
        char name[64];
        if (m == PC_BLEND_DISSOLVE) {
            continue;
        }
        (void)snprintf(name, sizeof name, "blend %s", pc_blend_label(m));
        l = t_raster(name, fmt, pc_rect_from_xywh((int32_t)(i % 20), (int32_t)(i % 12), 3, 2),
                     10 + (uint32_t)i, true);
        l.blend = m;
        t_push(&d, &l);
        ni++;
    }
    (void)ni;
    return d;
}

/* A 9x6 one-layer document (tests/flat.rs single). */
static inline pc_document t_single(pc_color_mode mode, pc_sample_type depth, bool alpha)
{
    pc_document d = t_doc("s", 9, 6, mode, depth);
    pc_layer l = t_raster("Background", pc_document_pixel_format(&d), pc_document_bounds(&d), 3, alpha);
    t_push(&d, &l);
    return d;
}

/* 16-pixel-wide columns of straight colours; cols holds ncols pixels of ch values. */
static inline pc_document t_columns(pc_color_mode mode, pc_sample_type depth, const float *cols, size_t ncols,
                                    size_t ch)
{
    pc_document d = t_doc("c", 16 * (uint32_t)ncols, 16, mode, depth);
    pc_surface s;
    pc_layer l;
    pc_surface_init(&s, pc_document_pixel_format(&d));
    for (size_t i = 0; i < ncols; i++) {
        int32_t x = 16 * (int32_t)i;
        must(pc_surface_fill_rect(&s, pc_rect_make(x, 0, x + 16, 16), cols + i * ch, ch, NULL));
    }
    must(pc_io_raster_layer("Layer", &s, &l, NULL));
    pc_surface_fini(&s);
    t_push(&d, &l);
    return d;
}

static inline void t_set_icc(pc_document *d, pc_builtin b)
{
    pc_buf icc = PC_BUF_INIT;
    must(pc_profile_to_bytes(pc_builtin_profile(b), &icc, NULL));
    must(pc_io_set_blob(&d->icc_profile, icc.data, icc.len, NULL));
    pc_buf_fini(&icc);
}

static inline void t_set_xmp(pc_document *d, const char *xmp)
{
    must(pc_str_set_c(&d->metadata.xmp, xmp, NULL));
    d->metadata.has_xmp = true;
}

/* EXIF with only Orientation = o. */
static inline size_t t_exif(uint16_t o, uint8_t out[26])
{
    static const uint8_t head[18] = {'I', 'I', '*', 0, 8, 0, 0, 0, 1, 0, 0x12, 1, 3, 0, 1, 0, 0, 0};
    memcpy(out, head, 18);
    out[18] = (uint8_t)o;
    out[19] = (uint8_t)(o >> 8);
    memset(out + 20, 0, 6);
    return 26;
}

/* ---- summaries (rustref io_core_vectors.rs) ---- */

static inline void h16(const void *data, size_t len, char out[17])
{
    char hex[PC_BLAKE3_HEX_LEN + 1];
    pc_blake3_hex(data, len, hex);
    memcpy(out, hex, 16);
    out[16] = '\0';
}

static inline void sb(pc_buf *b, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static inline void sb(pc_buf *b, const char *fmt, ...)
{
    va_list ap;
    char tmp[1024];
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) {
        abort();
    }
    if ((size_t)n < sizeof tmp) {
        must(pc_buf_append(b, tmp, (size_t)n, NULL));
    } else {
        char *big = xmalloc((size_t)n + 1);
        va_start(ap, fmt);
        (void)vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        must(pc_buf_append(b, big, (size_t)n, NULL));
        free(big);
    }
}

/* Rust's {:?} of a string. */
static inline void sq(pc_buf *b, const char *s, size_t len)
{
    size_t cap = len * 10 + 16;
    char *q = xmalloc(cap);
    pc_utf8_debug_quote((const uint8_t *)s, len, q, cap);
    must(pc_buf_append_str(b, q, NULL));
    free(q);
}

static inline void sopt(pc_buf *b, const void *data, size_t len, bool has)
{
    char h[17];
    if (!has) {
        sb(b, "-");
        return;
    }
    h16(data, len, h);
    sb(b, "%zu:%s", len, h);
}

static inline uint32_t fb(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

static inline void s_surf(pc_buf *b, const pc_surface *s, bool lossy)
{
    pc_pixel_format fmt = pc_surface_format(s);
    pc_rect r = pc_surface_content_bounds(s);
    const uint8_t *def = pc_surface_default_bytes(s);
    size_t dl = pc_pixel_format_bytes_per_pixel(fmt);
    char h[17];
    sb(b, "%s/%s/%d:%d,%d,%d,%d:", pc_color_mode_name(fmt.mode), pc_sample_type_name(fmt.sample),
       fmt.alpha ? 1 : 0, r.x0, r.y0, r.x1, r.y1);
    for (size_t i = 0; i < dl; i++) {
        sb(b, "%02x", def[i]);
    }
    if (lossy) {
        sb(b, ":lossy");
        return;
    }
    if (pc_rect_is_empty(r)) {
        h16("", 0, h);
    } else {
        uint8_t *px = NULL;
        size_t n = 0;
        must(pc_surface_to_interleaved(s, r, &px, &n, NULL));
        h16(px, n, h);
        free(px);
    }
    sb(b, ":%s", h);
}

static inline void s_layer(pc_buf *b, const pc_layer *l, bool lossy)
{
    const pc_locks *k = &l->locks;
    sq(b, pc_str_c(&l->name), l->name.len);
    sb(b, "|%d%d%d%d%d%d|%s|%08x|%08x|%d|%s|", l->visible, k->transparency, k->pixels, k->position,
       k->artboard, k->all, pc_blend_name(l->blend), fb(l->opacity), fb(l->fill_opacity), l->clipped,
       pc_layer_content_kind_name(&l->content));
    if (l->content.kind == PC_LAYER_GROUP) {
        sb(b, "{");
        for (size_t i = 0; i < l->content.u.group.nchildren; i++) {
            if (i > 0) {
                sb(b, ";");
            }
            s_layer(b, &l->content.u.group.children[i], lossy);
        }
        sb(b, "}");
    } else if (pc_layer_surface(l) != NULL) {
        s_surf(b, pc_layer_surface(l), lossy);
    } else {
        sb(b, "-");
    }
}

static inline void s_color(pc_buf *b, const pc_color *c)
{
    sb(b, "%s:%08x:%08x:%08x:%08x:%08x", pc_color_mode_name(c->mode), fb(c->c[0]), fb(c->c[1]), fb(c->c[2]),
       fb(c->c[3]), fb(c->alpha));
}

static inline void s_chan(pc_buf *b, const pc_alpha_channel *c)
{
    sq(b, pc_str_c(&c->name), c->name.len);
    sb(b, "|");
    if (c->has_spot) {
        s_color(b, &c->spot_color);
        sb(b, "@%08x", fb(c->spot_solidity));
    } else {
        sb(b, "-");
    }
    sb(b, "|");
    s_color(b, &c->color);
    sb(b, "|%08x|%s|", fb(c->opacity), c->indicates == PC_INDICATES_SELECTED_AREAS ? "sel" : "mask");
    s_surf(b, &c->surface, false);
}

static inline void s_doc(pc_buf *b, const pc_document *d, bool lossy)
{
    sb(b, "%ux%u %s %s dpi=%08x icc=", d->size.width, d->size.height, pc_color_mode_name(d->mode),
       pc_sample_type_name(d->depth), fb(d->resolution_dpi));
    sopt(b, pc_blob_data(d->icc_profile), pc_blob_len(d->icc_profile), d->icc_profile != NULL);
    sb(b, " exif=");
    sopt(b, pc_blob_data(d->metadata.exif), pc_blob_len(d->metadata.exif), d->metadata.exif != NULL);
    sb(b, " xmp=");
    sopt(b, d->metadata.xmp.ptr, d->metadata.xmp.len, d->metadata.has_xmp);
    sb(b, " L[");
    for (size_t i = 0; i < d->nlayers; i++) {
        if (i > 0) {
            sb(b, ";");
        }
        s_layer(b, &d->layers[i], lossy);
    }
    sb(b, "] C[");
    for (size_t i = 0; i < d->nchannels; i++) {
        if (i > 0) {
            sb(b, ";");
        }
        s_chan(b, &d->channels[i]);
    }
    sb(b, "]");
}

static inline void s_warn(pc_buf *b, char *const *v, size_t n)
{
    sb(b, "W[");
    for (size_t i = 0; i < n; i++) {
        if (i > 0) {
            sb(b, ", ");
        }
        sq(b, v[i], strlen(v[i]));
    }
    sb(b, "]");
}

static inline const char *kind_name(pc_io_err_kind k)
{
    static const char *const names[] = {"psd", "codec", "unknown",   "unsupported", "pcraft",
                                        "raw", "svg",   "cancelled", "pdn",
                                        "ora", "kra",   "other"};
    return names[k];
}

static inline void s_err(pc_buf *b, pc_status st, const pc_error *e)
{
    sb(b, "err %s ", kind_name(pc_io_error_kind(st, e)));
    sq(b, e->msg, strlen(e->msg));
}

static inline void s_imp(pc_buf *b, pc_status st, const pc_io_import_result *r, const pc_error *e, bool lossy)
{
    if (st != PC_OK) {
        s_err(b, st, e);
        return;
    }
    sb(b, "ok ");
    s_doc(b, &r->document, lossy);
    sb(b, " ");
    s_warn(b, r->warnings.v, r->warnings.n);
    sb(b, " %d %d", r->source_read_only, r->preview_only);
}

static inline void s_img(pc_buf *b, const pc_cimage *img, bool lossy)
{
    const pc_cmeta *m = &img->meta;
    char h[17];
    sb(b, "%s %s %ux%u px=", pc_clayout_name(img->layout), pc_csample_name(img->sample), img->width,
       img->height);
    if (lossy) {
        sb(b, "lossy");
    } else {
        h16(img->data, img->len, h);
        sb(b, "%s", h);
    }
    sb(b, " icc=");
    sopt(b, img->icc.data, img->icc.len, img->icc.has);
    sb(b, " exif=");
    sopt(b, m->exif.data, m->exif.len, m->exif.has);
    sb(b, " o=%u xmp=", m->exif.has ? pc_exif_orientation(m->exif.data, m->exif.len) : 0u);
    sopt(b, m->xmp.data, m->xmp.len, m->xmp.has);
    if (m->has_dpi) {
        sb(b, " dpi=%08x/%08x", fb(m->dpi_x), fb(m->dpi_y));
    } else {
        sb(b, " dpi=-");
    }
    sb(b, " text=%zu psr=", m->ntext);
    sopt(b, m->photoshop_resources.data, m->photoshop_resources.len, m->photoshop_resources.has);
    sb(b, " psl=");
    sopt(b, m->photoshop_layers.data, m->photoshop_layers.len, m->photoshop_layers.has);
}

/* ---- scratch directories ---- */

static char t_tmpdir[256];

static inline const char *t_tmp(void)
{
    if (t_tmpdir[0] == '\0') {
        const char *base = getenv("TMPDIR");
        (void)snprintf(t_tmpdir, sizeof t_tmpdir, "%s/pc-io-XXXXXX", base != NULL ? base : "/tmp");
        if (mkdtemp(t_tmpdir) == NULL) {
            abort();
        }
    }
    return t_tmpdir;
}


/* ---- gen_doc(mode, depth, Features::ALL) (added by I3) ---- */

static inline pc_layer t_adj_layer(size_t i, pc_adjustment *a)
{
    pc_layer_content c;
    pc_layer l;
    char name[32];
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment = *a;
    (void)snprintf(name, sizeof name, "adj %zu", i);
    must(pc_layer_init(&l, name, &c, NULL));
    l.opacity = g255(i % 2 == 0 ? 255 : 128);
    return l;
}

static inline pc_tone_space t_tone_space(pc_color_mode m)
{
    return m == PC_COLOR_MODE_CMYK ? PC_TONE_CMYK : m == PC_COLOR_MODE_LAB ? PC_TONE_LAB : PC_TONE_RGB;
}

static inline void t_curve(pc_curve *c, const float (*pts)[2], size_t n)
{
    c->pts = xmalloc(n * sizeof *c->pts);
    c->len = n;
    for (size_t i = 0; i < n; i++) {
        c->pts[i].input = pts[i][0];
        c->pts[i].output = pts[i][1];
    }
}

/* Features::ALL: PIXELS plus adjustments, a solid fill and the extras (metadata, guides, a
 * channel). */
static inline pc_document t_gen_doc_all(pc_color_mode mode, pc_sample_type depth)
{
    static const float ID2[2][2] = {{0, 0}, {1, 1}};
    pc_document d = t_gen_doc(mode, depth);
    pc_adjustment a[14];
    size_t n = 0;
    memset(a, 0, sizeof a);
    a[n++].kind = PC_ADJ_INVERT;
    a[n].kind = PC_ADJ_THRESHOLD;
    a[n++].u.threshold.level = g255(100);
    a[n].kind = PC_ADJ_POSTERIZE;
    a[n++].u.posterize.levels = 5;
    a[n].kind = PC_ADJ_BRIGHTNESS_CONTRAST;
    a[n].u.brightness_contrast.brightness = 20.0f;
    a[n++].u.brightness_contrast.contrast = -10.0f;
    a[n].kind = PC_ADJ_HUE_SATURATION;
    a[n].u.hue_saturation.hue = 30.0f;
    a[n].u.hue_saturation.saturation = -20.0f;
    a[n].u.hue_saturation.lightness = 5.0f;
    pc_hue_range_defaults(a[n++].u.hue_saturation.ranges);
    a[n].kind = PC_ADJ_EXPOSURE;
    a[n].u.exposure.exposure = 0.5f;
    a[n++].u.exposure.gamma = 1.0f;
    a[n].kind = PC_ADJ_LEVELS;
    a[n].u.levels.master = (pc_levels_channel){g255(10), g255(240), 1.2f, 0.0f, 1.0f};
    for (int i = 0; i < 3; i++) {
        a[n].u.levels.per_channel[i] = pc_levels_channel_default();
    }
    a[n].u.levels.space = t_tone_space(mode);
    a[n++].u.levels.black = pc_levels_channel_default();
    {
        const float m[3][2] = {{0, 0}, {g255(128), g255(150)}, {1, 1}};
        a[n].kind = PC_ADJ_CURVES;
        t_curve(&a[n].u.curves.master, m, 3);
        for (int i = 0; i < 3; i++) {
            t_curve(&a[n].u.curves.per_channel[i], ID2, 2);
        }
        a[n++].u.curves.space = t_tone_space(mode);
    }
    a[n].kind = PC_ADJ_UNSUPPORTED;
    must(pc_str_set_c(&a[n].u.unsupported.psd_key, "selc", NULL));
    must(pc_blob_new((const uint8_t[4]){0, 1, 0, 0}, 4, &a[n++].u.unsupported.raw, NULL));
    /* Colour adjustments (Photoshop offers them for colour documents only). */
    if (mode != PC_COLOR_MODE_GRAYSCALE) {
        const float mix[3][4] = {{0.8f, 0.2f, 0, 0}, {0, 1, 0, 0.1f}, {0.1f, 0, 0.9f, 0}};
        const float wts[6] = {50, 60, 40, 60, 20, 70};
        a[n].kind = PC_ADJ_VIBRANCE;
        a[n].u.vibrance.vibrance = 40.0f;
        a[n++].u.vibrance.saturation = -10.0f;
        a[n].kind = PC_ADJ_COLOR_BALANCE;
        a[n].u.color_balance.shadows[0] = 10.0f;
        a[n].u.color_balance.shadows[2] = -5.0f;
        a[n].u.color_balance.midtones[1] = 20.0f;
        a[n].u.color_balance.highlights[0] = -30.0f;
        a[n].u.color_balance.highlights[2] = 15.0f;
        a[n++].u.color_balance.preserve_luminosity = true;
        a[n].kind = PC_ADJ_BLACK_WHITE;
        memcpy(a[n].u.black_white.weights, wts, sizeof wts);
        a[n].u.black_white.has_tint = true;
        a[n].u.black_white.tint[0] = 1.0f;
        a[n].u.black_white.tint[1] = 0.8f;
        a[n++].u.black_white.tint[2] = 0.6f;
        a[n].kind = PC_ADJ_PHOTO_FILTER;
        a[n].u.photo_filter.color[0] = 1.0f;
        a[n].u.photo_filter.color[1] = 32768.0f / 65535.0f;
        a[n++].u.photo_filter.density = 0.25f;
        a[n].kind = PC_ADJ_CHANNEL_MIXER;
        memcpy(a[n++].u.channel_mixer.matrix, mix, sizeof mix);
    }
    for (size_t i = 0; i < n; i++) {
        pc_layer l = t_adj_layer(i, &a[i]);
        t_push(&d, &l);
    }
    {
        pc_layer_content c;
        pc_layer l;
        c.kind = PC_LAYER_FILL;
        memset(&c.u.fill, 0, sizeof c.u.fill);
        c.u.fill.kind = PC_FILL_SOLID;
        c.u.fill.u.solid = mode == PC_COLOR_MODE_CMYK ? (pc_color){PC_COLOR_MODE_CMYK, {0.2f, 0.4f, 0.6f, 0.1f}, 1.0f}
                           : mode == PC_COLOR_MODE_GRAYSCALE ? pc_color_gray(0.25f)
                                                             : pc_color_rgb(1.0f, 0.5f, 0.25f);
        must(pc_layer_init(&l, "Solid fill", &c, NULL));
        l.opacity = g255(64);
        t_push(&d, &l);
    }
    d.resolution_dpi = 300.0f;
    must(pc_io_set_blob(&d.icc_profile, (const uint8_t[4]){1, 2, 3, 4}, 4, NULL));
    d.metadata.has_xmp = true;
    must(pc_str_set_c(&d.metadata.xmp, "<x:xmpmeta/>", NULL));
    must(pc_io_set_blob(&d.metadata.exif, "MM\0*\0\0\0\x08", 8, NULL));
    d.metadata.psd_resources = xmalloc(sizeof *d.metadata.psd_resources);
    memset(d.metadata.psd_resources, 0, sizeof *d.metadata.psd_resources);
    d.metadata.psd_resources[0].id = 4000;
    must(pc_blob_new((const uint8_t[3]){9, 9, 9}, 3, &d.metadata.psd_resources[0].data, NULL));
    d.metadata.npsd_resources = 1;
    d.guides.horizontal = xmalloc(sizeof(double));
    d.guides.horizontal[0] = 5.5f;
    d.guides.nhorizontal = 1;
    d.guides.vertical = xmalloc(sizeof(double));
    d.guides.vertical[0] = 12.0f;
    d.guides.nvertical = 1;
    {
        pc_surface s = t_mask_surface(depth, pc_document_bounds(&d), 0.0f, 3);
        pc_alpha_channel ch;
        must(pc_alpha_channel_init(&ch, "Selection", &s, NULL));
        d.channels = xmalloc(sizeof *d.channels);
        d.channels[0] = ch;
        d.nchannels = 1;
    }
    return d;
}

#endif
