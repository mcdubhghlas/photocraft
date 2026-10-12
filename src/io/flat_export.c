/* Flat raster export through the codecs (port of the export half of crates/io/src/flat.rs). */
#include "io/flat.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "cms/transform.h"
#include "color/convert.h"
#include "compose/compose.h"
#include "raster/pixel.h"
#include "raster/surface.h"

pc_clayout pc_io_layout_for(pc_color_mode mode, bool alpha)
{
    if (mode == PC_COLOR_MODE_GRAYSCALE) {
        return alpha ? PC_CL_GRAYA : PC_CL_GRAY;
    }
    if (mode == PC_COLOR_MODE_CMYK) {
        return alpha ? PC_CL_CMYKA : PC_CL_CMYK;
    }
    return alpha ? PC_CL_RGBA : PC_CL_RGB;
}

pc_csample pc_io_csample(pc_sample_type s)
{
    return s == PC_SAMPLE_TYPE_U16 ? PC_CS_U16 : s == PC_SAMPLE_TYPE_F32 ? PC_CS_F32 : PC_CS_U8;
}

const pc_surface *pc_io_single_layer(const pc_document *doc)
{
    const pc_layer *l;
    float weights[3];
    bool ok;
    if (doc->nlayers != 1) {
        return NULL;
    }
    l = &doc->layers[0];
    ok = l->visible && l->opacity >= 1.0f && l->fill_opacity >= 1.0f && !l->has_mask &&
         !(l->has_vector_mask && l->vector_mask.enabled) && l->effects.nitems == 0 &&
         l->effects.psd_raw == NULL && (l->blend == PC_BLEND_NORMAL || l->blend == PC_BLEND_PASS_THROUGH) &&
         !pc_compose_channel_weights(l, doc->mode, weights) && !pc_compose_blend_if_active(l, doc->mode);
    if (ok && l->content.kind == PC_LAYER_RASTER &&
        pc_pixel_format_eq(pc_surface_format(&l->content.u.raster), pc_document_pixel_format(doc))) {
        return &l->content.u.raster;
    }
    return NULL;
}

pc_status pc_io_try_buffer(size_t pixels, size_t bytes_per_pixel, uint8_t **out, size_t *len, pc_error *err)
{
    size_t n;
    uint8_t *v;
    if (!pc_size_mul(pixels, bytes_per_pixel, &n)) {
        return pc_io_unsupported(err, "image too large");
    }
    v = n <= PC_MAX_ALLOC ? pc_malloc(n) : NULL;
    if (v == NULL) {
        pc_io_fail_set(err, PC_ERR_NOMEM, "unsupported: ", "not enough memory for a %zu MB image", n >> 20);
        return PC_ERR_NOMEM;
    }
    *out = v;
    *len = n;
    return PC_OK;
}

/* Whether every pixel of s over r is fully opaque (read from the tiles, no copy). */
static bool opaque_surface(const pc_surface *s, pc_rect r)
{
    pc_pixel_format fmt = pc_surface_format(s);
    size_t ch = pc_pixel_format_channels(fmt);
    size_t bps = pc_sample_type_bytes(fmt.sample);
    size_t px = ch * bps;
    size_t a = ch - 1;
    const uint8_t *def = pc_surface_default_bytes(s);
    pc_tile first;
    pc_tile last;
    if (!pc_rect_tile_range(r, &first, &last)) {
        return true;
    }
    for (int64_t ty = first.ty; ty <= last.ty; ty++) {
        for (int64_t tx = first.tx; tx <= last.tx; tx++) {
            pc_tile tc = {(int32_t)tx, (int32_t)ty};
            pc_rect trect = pc_tile_rect(tc);
            pc_rect tr = pc_rect_intersect(trect, r);
            const pc_raster_tile *t = pc_surface_tile(s, tc);
            if (t == NULL) {
                if (!(pc_read_sample(def, px, fmt.sample, a) >= 1.0f)) {
                    return false;
                }
                continue;
            }
            for (int32_t y = tr.y0; y < tr.y1; y++) {
                size_t row = ((size_t)(y - trect.y0) * PC_TILE_SIZE + (size_t)(tr.x0 - trect.x0)) * px;
                const uint8_t *b = pc_raster_tile_bytes(t) + row;
                for (uint32_t x = 0; x < pc_rect_width(tr); x++) {
                    if (!(pc_read_sample(b + x * px, px, fmt.sample, a) >= 1.0f)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

/* Remove the trailing alpha bytes of every color + alpha byte pixel, in place. */
static size_t drop_alpha(uint8_t *data, size_t len, size_t color, size_t alpha)
{
    size_t stride = color + alpha;
    size_t n = len / stride;
    for (size_t i = 0; i < n; i++) {
        memmove(data + i * color, data + i * stride, color);
    }
    return n * color;
}

/* ---- the composite path: rendered and quantised in bands ---- */

typedef struct q_ctx {
    const float (*px)[4];
    uint8_t *out; /* the band's first pixel in the output */
    bool gray;
    pc_csample cs;
    size_t stride; /* output bytes per pixel */
} q_ctx;

static size_t put(uint8_t *o, float v, pc_csample cs)
{
    v = isnan(v) ? 0.0f : v;
    if (cs == PC_CS_U8) {
        o[0] = pc_f32_to_u8(roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f));
        return 1;
    }
    if (cs == PC_CS_U16) {
        uint16_t q = pc_f32_to_u16(roundf(pc_clampf(v, 0.0f, 1.0f) * 65535.0f));
        memcpy(o, &q, 2);
        return 2;
    }
    memcpy(o, &v, 4);
    return 4;
}

static pc_status q_chunk(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const q_ctx *c = vctx;
    (void)chunk;
    for (size_t i = begin; i < end; i++) {
        const float *p = c->px[i];
        uint8_t *o = c->out + i * c->stride;
        if (c->gray) {
            float rgb[3] = {p[0], p[1], p[2]};
            o += put(o, pc_rgb_to_gray(rgb), c->cs);
        } else {
            o += put(o, p[0], c->cs);
            o += put(o, p[1], c->cs);
            o += put(o, p[2], c->cs);
        }
        (void)put(o, p[3], c->cs);
    }
    return PC_OK;
}

typedef struct band_ctx {
    uint8_t *data;
    size_t len;
    size_t stride;
    bool gray;
    pc_csample cs;
    bool opaque;
    pc_rect canvas;
} band_ctx;

static pc_status band_sink(void *vctx, pc_compose_buffer *band, pc_error *err)
{
    band_ctx *b = vctx;
    q_ctx q;
    size_t start = (size_t)(band->rect.y0 - b->canvas.y0) * pc_rect_width(b->canvas);
    for (size_t i = 0; i < band->len && b->opaque; i++) {
        b->opaque = band->px[i][3] >= 1.0f;
    }
    if (start > b->len / b->stride || band->len > b->len / b->stride - start) {
        return pc_fail(err, PC_ERR_INVALID, "invalid image: a band lies outside the canvas");
    }
    q.px = (const float (*)[4])band->px;
    q.out = b->data + start * b->stride;
    q.gray = b->gray;
    q.cs = b->cs;
    q.stride = b->stride;
    return pc_pool_for(pc_pool_global(), band->len, 65536, q_chunk, &q);
}

/* Native path: the surface's encoded samples, a band of rows at a time (alpha dropped when
 * opaque). */
static pc_status native_image(const pc_document *doc, const pc_surface *s, pc_cimage *out, pc_error *err)
{
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    uint32_t w = doc->size.width;
    uint32_t h = doc->size.height;
    size_t ch = pc_pixel_format_channels(fmt);
    size_t bps = pc_sample_type_bytes(fmt.sample);
    bool opaque = opaque_surface(s, pc_document_bounds(doc));
    pc_clayout layout = pc_io_layout_for(fmt.mode, !opaque);
    size_t keep = opaque ? (ch - 1) * bps : ch * bps;
    size_t rowb = (size_t)w * ch * bps;
    size_t rb = PC_IO_BAND_BYTES / (rowb > 0 ? rowb : 1);
    int32_t rows = rb < 1 ? 1 : (rb > INT32_MAX ? INT32_MAX : (int32_t)rb);
    uint8_t *data = NULL;
    size_t len = 0;
    size_t at = 0;
    pc_status st = pc_io_try_buffer((size_t)w * h, keep, &data, &len, err);
    for (int32_t y = 0; st == PC_OK && y < pc_u32_as_i32(h);) {
        int32_t y1 = pc_i32_sat_add(y, rows);
        uint8_t *band = NULL;
        size_t blen = 0;
        y1 = y1 < pc_u32_as_i32(h) ? y1 : pc_u32_as_i32(h);
        st = pc_surface_to_interleaved(s, pc_rect_make(0, y, pc_u32_as_i32(w), y1), &band, &blen, err);
        if (st != PC_OK) {
            break;
        }
        if (opaque) {
            for (size_t p = 0; p + ch * bps <= blen && at + keep <= len; p += ch * bps) {
                memcpy(data + at, band + p, keep);
                at += keep;
            }
        } else if (blen <= len - at) {
            memcpy(data + at, band, blen);
            at += blen;
        }
        free(band);
        y = y1;
    }
    if (st != PC_OK) {
        free(data);
        return st;
    }
    st = pc_cimage_from_raw(w, h, layout, pc_io_csample(fmt.sample), data, at, out, err);
    return st == PC_OK ? PC_OK : pc_io_err_prefix(st, err, "codec: ");
}

static pc_status composite_image(const pc_document *doc, pc_io_warnings *warnings, pc_meta_blob *icc,
                                 pc_cimage *out, pc_error *err)
{
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t count = 0;
    band_ctx b;
    size_t colors;
    pc_status st = pc_document_layer_count(doc, &count, err);
    if (st == PC_OK) {
        st = pc_io_warn(warnings, err, "%zu layer(s) flattened; layers, masks and blend modes are not kept",
                        count);
    }
    if (st != PC_OK) {
        return st;
    }
    /* The compositor works in RGB; write RGB/gray. */
    b.gray = fmt.mode == PC_COLOR_MODE_GRAYSCALE;
    if (fmt.mode == PC_COLOR_MODE_CMYK || fmt.mode == PC_COLOR_MODE_LAB) {
        /* The compositor renders CMYK/Lab documents in sRGB (CMYK through the built-in profile),
         * so the file is tagged sRGB. */
        const pc_profile *srgb = pc_builtin_profile(PC_BUILTIN_SRGB);
        pc_buf bytes = PC_BUF_INIT;
        st = pc_io_warn(warnings, err, "%s composite written as sRGB RGB (colour-managed conversion)",
                        pc_color_mode_name(fmt.mode));
        if (st == PC_OK) {
            st = srgb != NULL ? pc_profile_to_bytes(srgb, &bytes, err) : pc_io_nomem(err, "building sRGB");
        }
        if (st == PC_OK) {
            st = pc_meta_blob_set(icc, bytes.data, bytes.len, err);
        }
        pc_buf_fini(&bytes);
        if (st != PC_OK) {
            return st;
        }
    }
    b.cs = pc_io_csample(fmt.sample);
    colors = b.gray ? 1 : 3;
    b.stride = (colors + 1) * pc_csample_bytes(b.cs);
    b.opaque = true;
    b.canvas = pc_document_bounds(doc);
    st = pc_io_try_buffer((size_t)doc->size.width * doc->size.height, b.stride, &b.data, &b.len, err);
    if (st != PC_OK) {
        return st;
    }
    /* The reference ignores a failed render and then fails on the short buffer; the render's own
     * error is clearer. */
    st = pc_compose_render_bands(doc, b.canvas, 0, NULL, band_sink, &b, err);
    if (st != PC_OK) {
        free(b.data);
        return st;
    }
    if (b.opaque) {
        b.len = drop_alpha(b.data, b.len, colors * pc_csample_bytes(b.cs), pc_csample_bytes(b.cs));
    }
    st = pc_cimage_from_raw(doc->size.width, doc->size.height,
                            pc_io_layout_for(b.gray ? PC_COLOR_MODE_GRAYSCALE : PC_COLOR_MODE_RGB, !b.opaque),
                            b.cs, b.data, b.len, out, err);
    return st == PC_OK ? PC_OK : pc_io_err_prefix(st, err, "codec: ");
}

pc_status pc_io_document_to_image(const pc_document *doc, pc_io_warnings *warnings, pc_cimage *out,
                                  pc_error *err)
{
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    const pc_surface *native = fmt.mode != PC_COLOR_MODE_LAB ? pc_io_single_layer(doc) : NULL;
    pc_meta_blob icc = {false, NULL, 0};
    pc_cimage img;
    pc_status st = PC_OK;
    if (doc->icc_profile != NULL) {
        st = pc_meta_blob_set(&icc, pc_blob_data(doc->icc_profile), pc_blob_len(doc->icc_profile), err);
    }
    if (st == PC_OK) {
        st = native != NULL ? native_image(doc, native, &img, err)
                            : composite_image(doc, warnings, &icc, &img, err);
    }
    if (st != PC_OK) {
        pc_meta_blob_fini(&icc);
        return st;
    }
    pc_meta_blob_fini(&img.icc);
    img.icc = icc;
    pc_cmeta_fini(&img.meta);
    if (doc->metadata.exif != NULL) {
        st = pc_meta_blob_set(&img.meta.exif, pc_blob_data(doc->metadata.exif),
                              pc_blob_len(doc->metadata.exif), err);
    }
    if (st == PC_OK && doc->metadata.has_xmp) {
        st = pc_meta_blob_set(&img.meta.xmp, doc->metadata.xmp.ptr, doc->metadata.xmp.len, err);
    }
    img.meta.has_dpi = true;
    img.meta.dpi_x = doc->resolution_dpi;
    img.meta.dpi_y = doc->resolution_dpi;
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    *out = img;
    return PC_OK;
}

pc_status pc_io_encode_image(const pc_cimage *img, pc_cformat format, const pc_io_export_opts *opts,
                             pc_io_warnings *warnings, pc_io_export_result *out, pc_error *err)
{
    pc_cfidelity fw[64];
    size_t n = pc_cfidelity_warnings_with(img, format, &opts->encode, fw, sizeof fw / sizeof fw[0]);
    pc_io_export_result r;
    pc_status st = PC_OK;
    memset(&r, 0, sizeof r);
    r.warnings = *warnings;
    pc_io_warnings_init(warnings);
    if (n > sizeof fw / sizeof fw[0]) {
        n = sizeof fw / sizeof fw[0]; /* there are about a dozen kinds; never reached */
    }
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        char text[512];
        pc_cfidelity_text(&fw[i], text, sizeof text);
        if (pc_cfidelity_is_fatal(&fw[i])) {
            st = pc_io_unsupported(err, "%s", text);
        } else {
            st = pc_io_warn(&r.warnings, err, "%s", text);
        }
    }
    if (st == PC_OK) {
        pc_buf b = PC_BUF_INIT;
        st = pc_cencode(img, format, &opts->encode, &b, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
        }
        r.bytes = pc_buf_take(&b, &r.len);
        pc_buf_fini(&b);
    }
    if (st != PC_OK) {
        pc_io_export_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

/* ---- conversions of flat images, a band of rows at a time ---- */

typedef pc_status (*band_fn)(void *ctx, float *vals, size_t n, float **out, size_t *out_n, pc_error *err);

/* img with every band of rows mapped by f (normalised samples in, normalised `layout` samples
 * out) and stored as sample. Profile and metadata are kept. */
static pc_status map_bands(const pc_cimage *img, pc_clayout layout, pc_csample sample, band_fn f, void *ctx,
                           pc_cimage *out, pc_error *err)
{
    uint32_t w = img->width;
    uint32_t h = img->height;
    size_t row = img->len / (h > 0 ? h : 1);
    size_t r1 = row > 0 ? row : 1;
    size_t band = r1 * (PC_IO_BAND_BYTES / r1 > 0 ? PC_IO_BAND_BYTES / r1 : 1);
    uint8_t *data = NULL;
    size_t cap = 0;
    size_t at = 0;
    pc_cimage res;
    pc_status st = pc_io_try_buffer(pc_cimage_pixel_count(img),
                                    pc_clayout_channels(layout) * pc_csample_bytes(sample), &data, &cap, err);
    for (size_t off = 0; st == PC_OK && off < img->len; off += band) {
        size_t bytes = img->len - off < band ? img->len - off : band;
        uint32_t n = (uint32_t)(bytes / r1);
        pc_cimage part;
        pc_cimage conv;
        float *vals = NULL;
        size_t nv = 0;
        float *mapped = NULL;
        size_t nm = 0;
        st = pc_cimage_from_copy(w, n, img->layout, img->sample, img->data + off, bytes, &part, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
            break;
        }
        st = pc_cimage_to_normalized(&part, &vals, &nv, err);
        pc_cimage_fini(&part);
        if (st == PC_OK) {
            st = f(ctx, vals, nv, &mapped, &nm, err);
        }
        if (st == PC_OK) {
            st = pc_cimage_from_normalized(w, n, layout, sample, mapped, nm, &conv, err);
            if (st != PC_OK) {
                st = pc_io_err_prefix(st, err, "codec: ");
            } else {
                if (conv.len <= cap - at) {
                    memcpy(data + at, conv.data, conv.len);
                    at += conv.len;
                }
                pc_cimage_fini(&conv);
            }
        }
        if (mapped != vals) {
            free(mapped);
        }
        free(vals);
    }
    if (st != PC_OK) {
        free(data);
        return st;
    }
    st = pc_cimage_from_raw(w, h, layout, sample, data, at, &res, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    st = pc_meta_blob_clone(&img->icc, &res.icc, err);
    if (st == PC_OK) {
        pc_cmeta_fini(&res.meta);
        st = pc_cmeta_clone(&img->meta, &res.meta, err);
    }
    if (st != PC_OK) {
        pc_cimage_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}

typedef struct matte_ctx {
    size_t channels;
    float white;
} matte_ctx;

static pc_status matte_band(void *vctx, float *vals, size_t n, float **out, size_t *out_n, pc_error *err)
{
    const matte_ctx *c = vctx;
    size_t px = n / c->channels;
    size_t cc = c->channels - 1;
    float *o = pc_malloc_array(px * cc > 0 ? px * cc : 1, sizeof *o);
    if (o == NULL) {
        return pc_io_nomem(err, "matting over white");
    }
    for (size_t i = 0; i < px; i++) {
        float a = pc_clampf(vals[i * c->channels + cc], 0.0f, 1.0f);
        for (size_t k = 0; k < cc; k++) {
            o[i * cc + k] = vals[i * c->channels + k] * a + c->white * (1.0f - a);
        }
    }
    *out = o;
    *out_n = px * cc;
    return PC_OK;
}

/* Straight-alpha pixels composited over white (no ink for CMYK), without the alpha channel. */
static pc_status matte_over_white(const pc_cimage *img, pc_cimage *out, pc_error *err)
{
    matte_ctx c = {pc_clayout_channels(img->layout), pc_clayout_is_cmyk(img->layout) ? 0.0f : 1.0f};
    return map_bands(img, pc_clayout_without_alpha(img->layout), img->sample, matte_band, &c, out, err);
}

typedef struct apply_ctx {
    const pc_transform *t;
    size_t stride;
} apply_ctx;

static pc_status apply_band(void *vctx, float *vals, size_t n, float **out, size_t *out_n, pc_error *err)
{
    const apply_ctx *c = vctx;
    pc_status st = pc_transform_apply(c->t, vals, n, c->stride, err);
    if (st != PC_OK) {
        return st;
    }
    *out = vals; /* mapped in place */
    *out_n = n;
    return PC_OK;
}

/* The profile an image's pixels mean: its embedded profile when it parses with the wanted colour
 * space, else the builtin. *own says whether *p must be finished. */
static const pc_profile *source_profile(const pc_meta_blob *icc, uint32_t space, pc_builtin fallback,
                                        pc_profile *tmp, bool *own)
{
    *own = false;
    if (icc->has && pc_profile_parse(icc->data, icc->len, tmp, NULL) == PC_OK) {
        if (tmp->space == space) {
            *own = true;
            return tmp;
        }
        pc_profile_fini(tmp);
    }
    return pc_builtin_profile(fallback);
}

/* RGB pixels converted from their profile (sRGB when untagged) to dst, unclamped, untagged and
 * stored as sample. *converted false when the image isn't RGB or already holds dst's colours. */
static pc_status convert_rgb(const pc_cimage *img, const pc_profile *dst, pc_intent intent, bool bpc,
                             pc_csample sample, bool *converted, pc_cimage *out, pc_error *err)
{
    pc_profile tmp;
    bool own = false;
    const pc_profile *src;
    pc_transform *t = NULL;
    pc_error e;
    apply_ctx c;
    pc_status st;
    *converted = false;
    if (!pc_clayout_is_rgb(img->layout)) {
        return PC_OK;
    }
    src = source_profile(&img->icc, PC_CS_RGB, PC_BUILTIN_SRGB, &tmp, &own);
    if (src == NULL || dst == NULL) {
        return pc_io_nomem(err, "building a colour profile");
    }
    if (pc_profile_same_colors(src, dst)) {
        if (own) {
            pc_profile_fini(&tmp);
        }
        return PC_OK;
    }
    st = pc_transform_new(src, dst, intent, bpc, &t, &e);
    if (own) {
        pc_profile_fini(&tmp);
    }
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? pc_io_nomem(err, "building a colour transform")
                                  : pc_io_unsupported(err, "%s", e.msg);
    }
    c.t = t;
    c.stride = pc_clayout_channels(img->layout);
    st = map_bands(img, img->layout, sample, apply_band, &c, out, err);
    pc_transform_release(t);
    if (st == PC_OK) {
        pc_meta_blob_fini(&out->icc);
        *converted = true;
    }
    return st;
}

pc_status pc_io_cmyk_image_to_srgb(const pc_cimage *img, pc_cimage *out, pc_error *err)
{
    pc_profile tmp;
    bool own = false;
    const pc_profile *src = source_profile(&img->icc, PC_CS_CMYK, PC_BUILTIN_COATED_CMYK, &tmp, &own);
    const pc_profile *dst = pc_builtin_profile(PC_BUILTIN_SRGB);
    pc_transform *t = NULL;
    pc_error e;
    bool alpha = pc_clayout_has_alpha(img->layout);
    size_t ss = alpha ? 5 : 4;
    size_t ds = alpha ? 4 : 3;
    uint32_t w = img->width;
    uint32_t h = img->height;
    pc_clayout layout = alpha ? PC_CL_RGBA : PC_CL_RGB;
    pc_csample sample = img->sample == PC_CS_F16 ? PC_CS_F32 : img->sample;
    size_t row = w;
    size_t src_bpp = pc_clayout_channels(img->layout) * pc_csample_bytes(img->sample);
    size_t h_rows = h;
    size_t band_rows;
    size_t total;
    float *res = NULL;
    float *dst_band = NULL;
    pc_cimage r;
    pc_buf icc = PC_BUF_INIT;
    pc_status st;
    if (src == NULL || dst == NULL) {
        return pc_io_nomem(err, "building a colour profile");
    }
    st = pc_transform_new(src, dst, PC_INTENT_RELATIVE, true, &t, &e);
    if (own) {
        pc_profile_fini(&tmp);
    }
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? pc_io_nomem(err, "building a colour transform")
                                  : pc_io_unsupported(err, "%s", e.msg);
    }
    /* Row bands through the transform: the normalised input, the working set and the sRGB output
     * stay band-height slices. max(1) keeps an empty image from dividing by zero or inverting
     * the clamp range; such an image simply has no bands. */
    band_rows = PC_IO_BAND_BYTES / (row * ss * 4 > 0 ? row * ss * 4 : 1);
    band_rows =
        band_rows < 1 ? 1 : (band_rows > (h_rows > 0 ? h_rows : 1) ? (h_rows > 0 ? h_rows : 1) : band_rows);
    if (!pc_size_mul3(row, h_rows, ds, &total) || total > PC_MAX_ALLOC / sizeof(float)) {
        pc_transform_release(t);
        return pc_io_unsupported(err, "image too large");
    }
    res = pc_malloc_array(total > 0 ? total : 1, sizeof *res);
    dst_band = pc_malloc_array(band_rows * row * ds > 0 ? band_rows * row * ds : 1, sizeof *dst_band);
    if (res == NULL || dst_band == NULL) {
        st = pc_io_nomem(err, "converting CMYK to sRGB");
    }
    for (size_t idx = 0; st == PC_OK && idx < h_rows;) {
        size_t n = band_rows < h_rows - idx ? band_rows : h_rows - idx;
        size_t a;
        size_t b;
        pc_cimage part;
        float *vals = NULL;
        size_t nv = 0;
        if (!pc_size_mul3(idx, row, src_bpp, &a) || !pc_size_mul3(idx + n, row, src_bpp, &b) ||
            b > img->len) {
            st = pc_io_unsupported(err, "CMYK image data is shorter than its dimensions");
            break;
        }
        idx += n;
        st = pc_cimage_from_copy(w, (uint32_t)n, img->layout, img->sample, img->data + a, b - a, &part, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
            break;
        }
        st = pc_cimage_to_normalized(&part, &vals, &nv, err);
        pc_cimage_fini(&part);
        if (st == PC_OK) {
            memset(dst_band, 0, n * row * ds * sizeof *dst_band);
            st = pc_transform_convert_f32(t, vals, nv, ss, dst_band, n * row * ds, ds, true, err);
        }
        free(vals);
        if (st == PC_OK) {
            memcpy(res + (idx - n) * row * ds, dst_band, n * row * ds * sizeof *dst_band);
        }
    }
    free(dst_band);
    pc_transform_release(t);
    if (st == PC_OK) {
        st = pc_cimage_from_normalized(w, h, layout, sample, res, total, &r, err);
        if (st != PC_OK) {
            st = pc_io_err_prefix(st, err, "codec: ");
        }
    }
    free(res);
    if (st != PC_OK) {
        return st;
    }
    st = pc_profile_to_bytes(dst, &icc, err);
    if (st == PC_OK) {
        st = pc_meta_blob_set(&r.icc, icc.data, icc.len, err);
    }
    pc_buf_fini(&icc);
    if (st == PC_OK) {
        pc_cmeta_fini(&r.meta);
        st = pc_cmeta_clone(&img->meta, &r.meta, err);
    }
    if (st != PC_OK) {
        pc_cimage_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

/* ---- mode-specific exports ---- */

typedef struct idx_ctx {
    const pc_color_table *table;
    uint8_t *idx;
    size_t len;
    size_t at;
} idx_ctx;

static pc_status idx_sink(void *vctx, pc_compose_buffer *band, pc_error *err)
{
    idx_ctx *c = vctx;
    (void)err;
    for (size_t i = 0; i < band->len && c->at < c->len; i++) {
        const float *p = band->px[i];
        if (c->table->has_transparent && p[3] < 0.5f) {
            c->idx[c->at++] = c->table->transparent;
        } else {
            float rgb[3] = {p[0], p[1], p[2]};
            c->idx[c->at++] = (uint8_t)pc_color_table_nearest(c->table, rgb);
        }
    }
    return PC_OK;
}

/* Indexed Color -> PNG-8 with its colour table; Duotone -> the inks rendered as RGB. *done
 * false when the document needs no special handling. */
static pc_status export_mode_specific(const pc_document *doc, pc_cformat format,
                                      const pc_io_export_opts *opts, bool *done, pc_io_export_result *out,
                                      pc_error *err)
{
    *done = false;
    if (doc->mode == PC_COLOR_MODE_INDEXED && format == PC_CF_PNG) {
        const pc_color_table *t = &doc->color_table;
        idx_ctx c;
        pc_io_export_result r;
        pc_status st;
        if (!doc->has_color_table || t->ncolors == 0 || t->ncolors > 256) {
            return PC_OK;
        }
        c.table = t;
        c.at = 0;
        st = pc_io_try_buffer((size_t)pc_size_area(doc->size), 1, &c.idx, &c.len, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_compose_render_bands(doc, pc_document_bounds(doc), 0, NULL, idx_sink, &c, err);
        memset(&r, 0, sizeof r);
        if (st == PC_OK) {
            pc_buf b = PC_BUF_INIT;
            st = pc_cencode_png_indexed(doc->size.width, doc->size.height, c.idx, c.at,
                                        (const uint8_t (*)[3])t->colors, t->ncolors,
                                        t->has_transparent ? (int)t->transparent : -1, &b, err);
            if (st != PC_OK) {
                st = pc_io_err_prefix(st, err, "codec: ");
            }
            r.bytes = pc_buf_take(&b, &r.len);
            pc_buf_fini(&b);
        }
        free(c.idx);
        if (st == PC_OK) {
            st = pc_io_warn(&r.warnings, err, "written as an 8-bit palette PNG (%zu colours)", t->ncolors);
        }
        if (st != PC_OK) {
            pc_io_export_result_fini(&r);
            return st;
        }
        *out = r;
        *done = true;
        return PC_OK;
    }
    if (doc->mode == PC_COLOR_MODE_DUOTONE && doc->has_duotone) {
        pc_document shown;
        pc_layer_content c;
        pc_layer layer;
        pc_io_export_result r;
        pc_status st = pc_document_clone(doc, &shown, err);
        if (st != PC_OK) {
            return st;
        }
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_ADJUSTMENT;
        st = pc_duotone_display_adjustment(&doc->duotone, &c.u.adjustment, err);
        if (st == PC_OK) {
            st = pc_layer_init(&layer, "Duotone", &c, err);
            if (st != PC_OK) {
                pc_adjustment_fini(&c.u.adjustment);
            }
        }
        if (st == PC_OK) {
            st = pc_io_push_layer(&shown, &layer, err);
            if (st != PC_OK) {
                pc_layer_fini(&layer);
            }
        }
        if (st == PC_OK) {
            shown.mode = PC_COLOR_MODE_RGB;
            pc_blob_release(shown.icc_profile);
            shown.icc_profile = NULL;
            pc_duotone_fini(&shown.duotone);
            shown.has_duotone = false;
            st = pc_io_export_flat(&shown, format, opts, &r, err);
        }
        pc_document_fini(&shown);
        if (st != PC_OK) {
            return st;
        }
        for (size_t i = r.warnings.n; i-- > 0;) {
            if (strstr(r.warnings.v[i], "flattened") != NULL) {
                pc_io_warnings_remove(&r.warnings, i);
            }
        }
        st = pc_io_warn(&r.warnings, err, "Duotone (%zu inks) written as RGB", doc->duotone.ninks);
        if (st != PC_OK) {
            pc_io_export_result_fini(&r);
            return st;
        }
        *out = r;
        *done = true;
    }
    return PC_OK;
}

/* Replaces *img with *next (moved). */
static void replace(pc_cimage *img, pc_cimage *next)
{
    pc_cimage_fini(img);
    *img = *next;
}

pc_status pc_io_export_flat(const pc_document *doc, pc_cformat format, const pc_io_export_opts *opts,
                            pc_io_export_result *out, pc_error *err)
{
    pc_io_warnings warnings = PC_IO_WARNINGS_INIT;
    pc_ccaps caps = pc_cformat_caps(format);
    pc_cimage img;
    pc_cimage next;
    bool done = false;
    bool converted = false;
    pc_status st = export_mode_specific(doc, format, opts, &done, out, err);
    if (st != PC_OK || done) {
        return st;
    }
    st = pc_io_document_to_image(doc, &warnings, &img, err);
    if (st != PC_OK) {
        pc_io_warnings_fini(&warnings);
        return st;
    }
    if (opts->xmp == PC_IO_XMP_NONE) {
        /* Export As's Metadata: None: the packet lists the text of every type layer and one id
         * per placed document (#647). */
        pc_meta_blob_fini(&img.meta.xmp);
    }
    if (st == PC_OK && pc_clayout_has_alpha(img.layout) && !caps.alpha) {
        /* Flattened over white, as saving a transparent document without transparency does. */
        st = matte_over_white(&img, &next, err);
        if (st == PC_OK) {
            replace(&img, &next);
            st = pc_io_warn(&warnings, err, "transparency composited over white for %s",
                            pc_cformat_debug(format));
        }
    }
    if (st == PC_OK && pc_clayout_is_cmyk(img.layout)) {
        bool cmyk_ok = false;
        for (size_t i = 0; i < caps.nlayouts; i++) {
            cmyk_ok = cmyk_ok || pc_clayout_is_cmyk(caps.layouts[i]);
        }
        if (!cmyk_ok) {
            st = pc_io_cmyk_image_to_srgb(&img, &next, err);
            if (st == PC_OK) {
                replace(&img, &next);
                st = pc_io_warn(&warnings, err,
                                "CMYK converted to sRGB for %s through the document's colour profile",
                                pc_cformat_debug(format));
            }
        }
    }
    if (st == PC_OK && (format == PC_CF_EXR || format == PC_CF_HDR)) {
        /* OpenEXR and Radiance HDR store linear light (read back as linear sRGB). */
        st = convert_rgb(&img, pc_builtin_profile(PC_BUILTIN_LINEAR_SRGB), PC_INTENT_RELATIVE, false,
                         PC_CS_F32, &converted, &next, err);
        if (st == PC_OK && converted) {
            replace(&img, &next);
        }
    } else if (st == PC_OK && !caps.icc) {
        /* Untagged files read back as sRGB: convert to it (as Quick Export does) rather than write
         * values that only mean something under the dropped profile. */
        st = convert_rgb(&img, pc_builtin_profile(PC_BUILTIN_SRGB), PC_INTENT_PERCEPTUAL, true, img.sample,
                         &converted, &next, err);
        if (st == PC_OK && converted) {
            replace(&img, &next);
            st = pc_io_warn(&warnings, err,
                            "colours converted to sRGB; %s can't embed the document's colour profile",
                            pc_cformat_debug(format));
        }
    }
    if (st == PC_OK) {
        st = pc_io_encode_image(&img, format, opts, &warnings, out, err);
    }
    pc_cimage_fini(&img);
    pc_io_warnings_fini(&warnings);
    return st;
}
