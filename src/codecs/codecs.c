/* Decode/encode dispatch, limits, encode planning and fidelity warnings (port of
 * crates/codecs/src/{lib,options,fidelity,error}.rs). */
#include "codecs/codecs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codecs/internal.h"
#include "codecs/orientation.h"

/* ---- errors and writers ---- */

void cx_fail_fmt_set(pc_error *err, pc_status code, pc_cformat f, const char *prefix, const char *fmt, ...)
{
    char msg[PC_ERROR_MSG_MAX];
    va_list ap;
    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    pc_fail_set(err, code, "%s: %s%s", pc_cformat_debug(f), prefix, msg);
}

void cx_w_put(cx_w *w, const void *d, size_t n)
{
    if (w->st == PC_OK && n > 0) {
        w->st = pc_buf_append(w->b, d, n, w->err);
    }
}

void cx_w_byte(cx_w *w, uint8_t v) { cx_w_put(w, &v, 1); }

void cx_w_le16(cx_w *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    cx_w_put(w, b, 2);
}

void cx_w_le32(cx_w *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    cx_w_put(w, b, 4);
}

void cx_w_be16(cx_w *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    cx_w_put(w, b, 2);
}

void cx_w_be32(cx_w *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    cx_w_put(w, b, 4);
}

/* ---- limits and options ---- */

pc_climits pc_climits_default(void)
{
    pc_climits l;
    l.max_width = 1u << 18;
    l.max_height = 1u << 18;
    l.max_pixels = 1ull << 30;
    l.max_alloc = SIZE_MAX > 0xFFFFFFFFu ? 8ull << 30 : 1ull << 30;
    return l;
}

pc_climits pc_climits_none(void)
{
    pc_climits l;
    l.max_width = UINT32_MAX;
    l.max_height = UINT32_MAX;
    l.max_pixels = UINT64_MAX;
    l.max_alloc = UINT64_MAX;
    return l;
}

pc_status cx_check_bytes(const pc_climits *l, uint32_t w, uint32_t h, uint64_t bpp, pc_error *err)
{
    uint64_t pixels;
    uint64_t bytes;
    if (w == 0 || h == 0) {
        return cx_invalid(err, "zero-sized image %ux%u", (unsigned)w, (unsigned)h);
    }
    if (w > l->max_width || h > l->max_height) {
        return cx_limit(err, "dimensions %ux%u exceed max %ux%u", (unsigned)w, (unsigned)h, (unsigned)l->max_width,
                        (unsigned)l->max_height);
    }
    pixels = (uint64_t)w * h;
    if (pixels > l->max_pixels) {
        return cx_limit(err, "%llu pixels exceed max %llu", (unsigned long long)pixels,
                        (unsigned long long)l->max_pixels);
    }
    bytes = bpp != 0 && pixels > UINT64_MAX / bpp ? UINT64_MAX : pixels * bpp;
    if (bytes > l->max_alloc) {
        return cx_limit(err, "%llu bytes exceed max_alloc %llu", (unsigned long long)bytes,
                        (unsigned long long)l->max_alloc);
    }
    if (bytes > SIZE_MAX) {
        return cx_limit(err, "%llu bytes do not fit in memory", (unsigned long long)bytes);
    }
    return PC_OK;
}

pc_status pc_climits_check(const pc_climits *l, uint32_t w, uint32_t h, pc_clayout layout, pc_csample s,
                           pc_error *err)
{
    return cx_check_bytes(l, w, h, (uint64_t)(pc_clayout_channels(layout) * pc_csample_bytes(s)), err);
}

pc_status cx_check_plausible(uint64_t declared_bytes, size_t file_len, uint64_t max_ratio, pc_error *err)
{
    uint64_t cap = max_ratio != 0 && (uint64_t)file_len > UINT64_MAX / max_ratio
                       ? UINT64_MAX
                       : (uint64_t)file_len * max_ratio;
    cap = cap > UINT64_MAX - (1u << 20) ? UINT64_MAX : cap + (1u << 20);
    if (declared_bytes > cap) {
        return cx_limit(err, "a %zu byte file cannot hold %llu bytes of image data", file_len,
                        (unsigned long long)declared_bytes);
    }
    return PC_OK;
}

size_t cx_alloc_usize(const pc_climits *l) { return l->max_alloc > SIZE_MAX ? SIZE_MAX : (size_t)l->max_alloc; }

pc_cdecode_opts pc_cdecode_opts_default(void)
{
    pc_cdecode_opts o;
    o.limits = pc_climits_default();
    o.keep_orientation = false;
    return o;
}

pc_cencode_opts pc_cencode_opts_default(void)
{
    pc_cencode_opts o;
    o.jpeg_quality = 90;
    o.jpeg_chroma_subsampling = true;
    o.png_compression = PC_PNG_DEFAULT;
    o.png_interlaced = false;
    o.webp_lossless = true;
    o.webp_quality = 80;
    o.tiff_compression = PC_TIFF_DEFLATE;
    o.tiff_bigtiff = false;
    o.exr_compression = PC_EXR_ZIP16;
    o.dpx_bits = PC_DPX_BITS_AUTO;
    o.tga_bits = 0;
    o.embed_icc = true;
    o.embed_metadata = true;
    return o;
}

/* ---- plan ---- */

static bool has_depth(const pc_ccaps *c, pc_csample s)
{
    size_t i;
    for (i = 0; i < c->ndepths; i++) {
        if (c->depths[i] == s) {
            return true;
        }
    }
    return false;
}

static bool has_layout(const pc_clayout *ls, size_t n, pc_clayout l)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (ls[i] == l) {
            return true;
        }
    }
    return false;
}

static pc_csample pick_sample(pc_csample src, const pc_ccaps *c)
{
    static const pc_csample PREF[PC_CS_COUNT][3] = {
        {PC_CS_U16, PC_CS_F32, PC_CS_F16}, /* integers widen into float losslessly */
        {PC_CS_F32, PC_CS_U8, PC_CS_F16},
        {PC_CS_F32, PC_CS_U16, PC_CS_U8},
        {PC_CS_F16, PC_CS_U16, PC_CS_U8},
    };
    size_t i;
    if (has_depth(c, src)) {
        return src;
    }
    for (i = 0; i < 3; i++) {
        if (has_depth(c, PREF[src][i])) {
            return PREF[src][i];
        }
    }
    return c->depths[0];
}

static pc_clayout pick_layout(pc_clayout src, const pc_clayout *ls, size_t n, bool alpha)
{
    pc_clayout cand = src;
    if (!alpha) {
        cand = pc_clayout_without_alpha(cand);
    }
    if (has_layout(ls, n, cand)) {
        return cand;
    }
    /* CMYK falls back to RGB; gray expands to RGB. */
    if (pc_clayout_is_cmyk(cand) || pc_clayout_is_gray(cand)) {
        cand = pc_clayout_has_alpha(cand) ? PC_CL_RGBA : PC_CL_RGB;
    }
    if (has_layout(ls, n, cand)) {
        return cand;
    }
    if (has_layout(ls, n, pc_clayout_with_alpha(cand))) {
        return pc_clayout_with_alpha(cand);
    }
    if (has_layout(ls, n, pc_clayout_without_alpha(cand))) {
        return pc_clayout_without_alpha(cand);
    }
    return ls[0];
}

cx_plan cx_plan_for(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o)
{
    static const pc_clayout PFM[] = {PC_CL_GRAY, PC_CL_RGB};
    pc_ccaps c = pc_cformat_caps(f);
    cx_plan p;
    if (f == PC_CF_TGA && (o->tga_bits == 24 || o->tga_bits == 32)) {
        p.layout = o->tga_bits == 24 ? PC_CL_RGB : PC_CL_RGBA;
        p.sample = PC_CS_U8;
    } else if (f == PC_CF_PNM && pc_csample_is_float(img->sample)) {
        /* PNM: integer data can use any layout (PAM); float goes to PFM (gray and RGB only). */
        p.layout = pick_layout(img->layout, PFM, 2, false);
        p.sample = PC_CS_F32;
    } else if (f == PC_CF_DPX) {
        /* DPX has no luma + alpha descriptor: gray with alpha is written as luma, alpha dropped.
         * The sample type follows the chosen bits (U8 for 8, U16 for 10, 12 and 16). */
        p.layout = img->layout == PC_CL_GRAYA ? PC_CL_GRAY : pick_layout(img->layout, c.layouts, c.nlayouts, c.alpha);
        p.sample = cx_dpx_plan_sample(o->dpx_bits, img->sample);
    } else {
        p.layout = pick_layout(img->layout, c.layouts, c.nlayouts, c.alpha);
        p.sample = pick_sample(img->sample, &c);
    }
    return p;
}

/* ---- fidelity warnings ---- */

static const char *depth_name(pc_csample s)
{
    static const char *const N[PC_CS_COUNT] = {"8-bit", "16-bit", "16-bit float", "32-bit float"};
    return N[s];
}

bool pc_cfidelity_is_fatal(const pc_cfidelity *w)
{
    return w->kind == PC_FW_WRITE_UNSUPPORTED || w->kind == PC_FW_DIMENSIONS_EXCEEDED;
}

bool pc_cfidelity_eq(const pc_cfidelity *a, const pc_cfidelity *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_FW_WRITE_UNSUPPORTED:
        return a->format == b->format;
    case PC_FW_DEPTH_REDUCED:
        return a->from == b->from && a->to == b->to;
    case PC_FW_CMYK_CONVERTED:
        return a->to_layout == b->to_layout;
    case PC_FW_METADATA_TOO_LARGE:
        return strcmp(a->what, b->what) == 0 && a->bytes == b->bytes;
    case PC_FW_DIMENSIONS_EXCEEDED:
        return a->max_width == b->max_width && a->max_height == b->max_height;
    default:
        return true;
    }
}

void pc_cfidelity_text(const pc_cfidelity *w, char *out, size_t cap)
{
    switch (w->kind) {
    case PC_FW_WRITE_UNSUPPORTED:
        (void)snprintf(out, cap, "%s cannot be written in this build", pc_cformat_name(w->format));
        break;
    case PC_FW_DEPTH_REDUCED:
        (void)snprintf(out, cap, "%s will be reduced to %s", depth_name(w->from), depth_name(w->to));
        break;
    case PC_FW_RANGE_CLIPPED:
        (void)snprintf(out, cap, "HDR values outside 0..1 will be clipped");
        break;
    case PC_FW_ALPHA_DISCARDED:
        (void)snprintf(out, cap, "alpha will be discarded");
        break;
    case PC_FW_ALPHA_BINARIZED:
        (void)snprintf(out, cap, "alpha will be reduced to on/off transparency");
        break;
    case PC_FW_CMYK_CONVERTED:
        (void)snprintf(out, cap, "CMYK will be converted to %s (not colour-managed)", pc_clayout_name(w->to_layout));
        break;
    case PC_FW_COLOR_TO_GRAY:
        (void)snprintf(out, cap, "colour will be converted to grayscale");
        break;
    case PC_FW_PALETTE_QUANTIZED:
        (void)snprintf(out, cap, "colours will be quantized to a 256-colour palette");
        break;
    case PC_FW_LOSSY_COMPRESSION:
        (void)snprintf(out, cap, "lossy compression");
        break;
    case PC_FW_ICC_DROPPED:
        (void)snprintf(out, cap, "ICC profile not supported; it will be dropped");
        break;
    case PC_FW_EXIF_DROPPED:
        (void)snprintf(out, cap, "EXIF not supported; it will be dropped");
        break;
    case PC_FW_XMP_DROPPED:
        (void)snprintf(out, cap, "XMP not supported; it will be dropped");
        break;
    case PC_FW_DPI_DROPPED:
        (void)snprintf(out, cap, "resolution (DPI) not supported; it will be dropped");
        break;
    case PC_FW_TEXT_DROPPED:
        (void)snprintf(out, cap, "text metadata not supported; it will be dropped");
        break;
    case PC_FW_METADATA_TOO_LARGE:
        (void)snprintf(out, cap, "%s (%zu bytes) is too large for the format; it will be dropped", w->what, w->bytes);
        break;
    default:
        (void)snprintf(out, cap, "image exceeds the format maximum of %ux%u", (unsigned)w->max_width,
                       (unsigned)w->max_height);
    }
}

typedef struct wlist {
    pc_cfidelity *out;
    size_t cap;
    size_t n;
} wlist;

static void push(wlist *l, pc_cfidelity w)
{
    if (l->n < l->cap) {
        l->out[l->n] = w;
    }
    l->n++;
}

static pc_cfidelity W(pc_cfidelity_kind k)
{
    pc_cfidelity w;
    memset(&w, 0, sizeof w);
    w.kind = k;
    return w;
}

size_t pc_cfidelity_warnings(const pc_cimage *img, pc_cformat f, pc_cfidelity *out, size_t cap)
{
    pc_cencode_opts o = pc_cencode_opts_default();
    return pc_cfidelity_warnings_with(img, f, &o, out, cap);
}

size_t pc_cfidelity_warnings_with(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o, pc_cfidelity *out,
                                  size_t cap)
{
    pc_ccaps c = pc_cformat_caps(f);
    wlist l = {out, cap, 0};
    uint32_t mw;
    uint32_t mh;
    pc_cencode_opts defaults;
    if (o == NULL) {
        defaults = pc_cencode_opts_default();
        o = &defaults;
    }
    cx_plan p;
    pc_clayout sl = img->layout;
    pc_csample ss = img->sample;
    bool lossy_depth;
    if (!c.write) {
        pc_cfidelity w = W(PC_FW_WRITE_UNSUPPORTED);
        w.format = f;
        push(&l, w);
        return l.n;
    }
    if (pc_cformat_max_dimensions(f, &mw, &mh) && (img->width > mw || img->height > mh)) {
        pc_cfidelity w = W(PC_FW_DIMENSIONS_EXCEEDED);
        w.max_width = mw;
        w.max_height = mh;
        push(&l, w);
    }
    p = cx_plan_for(img, f, o);
    /* Depth */
    if (ss == p.sample || ss == PC_CS_U8 || (ss == PC_CS_U16 && p.sample == PC_CS_F32) ||
        (ss == PC_CS_F16 && p.sample == PC_CS_F32)) {
        lossy_depth = false;
    } else {
        lossy_depth = true;
    }
    if (lossy_depth) {
        pc_cfidelity w = W(PC_FW_DEPTH_REDUCED);
        w.from = ss;
        w.to = p.sample;
        push(&l, w);
    }
    if (pc_csample_is_float(ss) && !pc_csample_is_float(p.sample) && pc_cimage_has_out_of_range(img)) {
        push(&l, W(PC_FW_RANGE_CLIPPED));
    }
    /* Layout */
    if (pc_clayout_has_alpha(sl) && !pc_clayout_has_alpha(p.layout) && pc_cimage_has_translucency(img)) {
        push(&l, W(PC_FW_ALPHA_DISCARDED));
    }
    if (f == PC_CF_GIF && pc_clayout_has_alpha(p.layout) && pc_cimage_has_translucency(img)) {
        push(&l, W(PC_FW_ALPHA_BINARIZED));
    }
    if (pc_clayout_is_cmyk(sl) && !pc_clayout_is_cmyk(p.layout)) {
        pc_cfidelity w = W(PC_FW_CMYK_CONVERTED);
        w.to_layout = p.layout;
        push(&l, w);
    }
    if (!pc_clayout_is_gray(sl) && pc_clayout_is_gray(p.layout)) {
        push(&l, W(PC_FW_COLOR_TO_GRAY));
    }
    /* Compression */
    if (f == PC_CF_GIF) {
        push(&l, W(PC_FW_PALETTE_QUANTIZED));
    } else if (c.lossy || (f == PC_CF_WEBP && !o->webp_lossless)) {
        push(&l, W(PC_FW_LOSSY_COMPRESSION));
    }
    /* Metadata */
    if (img->icc.has && o->embed_icc && (!c.icc || !pc_clayout_same_model(sl, p.layout))) {
        push(&l, W(PC_FW_ICC_DROPPED));
    }
    if (o->embed_metadata) {
        const pc_cmeta *m = &img->meta;
        if (m->exif.has && !c.exif) {
            push(&l, W(PC_FW_EXIF_DROPPED));
        }
        if (m->xmp.has && !c.xmp) {
            push(&l, W(PC_FW_XMP_DROPPED));
        }
        if (f == PC_CF_JPEG) {
            pc_buf seg = PC_BUF_INIT;
            if (m->exif.has && !cx_jpeg_exif_segment(m->exif.data, m->exif.len, NULL, &seg)) {
                pc_cfidelity w = W(PC_FW_METADATA_TOO_LARGE);
                w.what = "EXIF";
                w.bytes = m->exif.len;
                push(&l, w);
            }
            if (m->xmp.has && !cx_jpeg_xmp_segment(m->xmp.data, m->xmp.len, NULL, &seg)) {
                pc_cfidelity w = W(PC_FW_METADATA_TOO_LARGE);
                w.what = "XMP";
                w.bytes = m->xmp.len;
                push(&l, w);
            }
            pc_buf_fini(&seg);
        }
        if (m->has_dpi && !c.dpi) {
            push(&l, W(PC_FW_DPI_DROPPED));
        }
        /* DPX keeps its own header fields, not free-form text. */
        bool any_dropped = false;
        for (size_t i = 0; i < m->ntext; i++) {
            bool kept = c.text && (f != PC_CF_DPX || cx_dpx_is_header_key(m->text[i].key));
            any_dropped = any_dropped || !kept;
        }
        if (any_dropped) {
            push(&l, W(PC_FW_TEXT_DROPPED));
        }
    }
    return l.n;
}

/* ---- dispatch ---- */

pc_status pc_cdecode(const uint8_t *b, size_t n, pc_cimage *out, pc_error *err)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    return pc_cdecode_with(b, n, &o, out, err);
}

pc_status pc_cdecode_with(const uint8_t *b, size_t n, const pc_cdecode_opts *o, pc_cimage *out, pc_error *err)
{
    pc_cformat f;
    if (!pc_cformat_detect(b, n, &f)) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "unrecognized image format");
    }
    return pc_cdecode_as_with(f, b, n, o, out, err);
}

pc_status pc_cdecode_as(pc_cformat f, const uint8_t *b, size_t n, pc_cimage *out, pc_error *err)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    return pc_cdecode_as_with(f, b, n, &o, out, err);
}

/* The limits check and the orientation, shared by every decoder. Takes ownership of *img. */
static pc_status finish_decode(pc_cimage *img, uint16_t o, const pc_cdecode_opts *opts, pc_cimage *out, pc_error *err)
{
    const pc_climits *l = &opts->limits;
    pc_status st = pc_climits_check(l, img->width, img->height, img->layout, img->sample, err);
    if (st == PC_OK && !opts->keep_orientation && o >= 5 && o <= 8) {
        /* Orientations 5-8 swap width and height: check the upright size too. */
        st = pc_climits_check(l, img->height, img->width, img->layout, img->sample, err);
    }
    if (st != PC_OK) {
        pc_cimage_fini(img);
        return st;
    }
    if (opts->keep_orientation) {
        *out = *img;
        return PC_OK;
    }
    return pc_cimage_oriented(img, o, out, err);
}

pc_status pc_cdecode_as_with(pc_cformat f, const uint8_t *b, size_t n, const pc_cdecode_opts *o, pc_cimage *out,
                             pc_error *err)
{
    const pc_climits *l = &o->limits;
    pc_cimage img;
    pc_status st;
    uint16_t orient;
    /* HEIF without libheif: its decoder says what is missing from this build. */
    if (!pc_cformat_caps(f).read && f != PC_CF_HEIF) {
        return cx_unsupported(err, f, "decoding is not available for this format");
    }
    pc_cimage_init(&img);
    switch (f) {
    case PC_CF_PNG:
        st = cx_png_decode(b, n, l, &img, err);
        break;
    case PC_CF_JPEG:
        st = cx_jpeg_decode(b, n, l, &img, err);
        break;
    case PC_CF_TIFF:
        st = cx_tiff_decode_page(b, n, false, 0, l, &img, err);
        break;
    case PC_CF_WEBP:
        st = cx_webp_decode(b, n, l, &img, err);
        break;
    case PC_CF_PNM:
        st = cx_pnm_decode(b, n, l, &img, err);
        break;
    case PC_CF_PCX:
        st = cx_pcx_decode(b, n, l, &img, err);
        break;
    case PC_CF_FITS:
        st = cx_fits_decode(b, n, l, &img, err);
        break;
    case PC_CF_EXR:
        st = cx_exr_decode(b, n, l, &img, err);
        break;
    case PC_CF_HEIF:
        st = cx_heif_decode(b, n, l, o->keep_orientation, &img, err);
        break;
    case PC_CF_GIF:
        st = cx_gif_decode(b, n, l, &img, err);
        break;
    case PC_CF_BMP:
        st = cx_bmp_decode(b, n, l, &img, err);
        break;
    case PC_CF_TGA:
        st = cx_tga_decode(b, n, l, &img, err);
        break;
    case PC_CF_ICO:
        st = cx_ico_decode(b, n, l, &img, err);
        break;
    case PC_CF_QOI:
        st = cx_qoi_decode(b, n, l, &img, err);
        break;
    case PC_CF_HDR:
        st = cx_hdr_decode(b, n, l, &img, err);
        break;
    case PC_CF_DPX:
        st = cx_dpx_decode(b, n, l, &img, err);
        break;
    default:
        st = cx_unsupported(err, f, "decoding is not available for this format");
    }
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    /* Without a resolution of the format's own (PNG pHYs, TIFF tags, JPEG's metadata), the XMP or
     * EXIF one counts, like Photoshop: a WebP, HEIF or PNG from a camera opens at its recorded
     * resolution, not at the 72 ppi default. */
    if (!img.meta.has_dpi && f != PC_CF_TIFF) {
        img.meta.has_dpi = cx_resolution_from_metadata(&img.meta, &img.meta.dpi_x, &img.meta.dpi_y);
    }
    /* Turn the pixels upright: a TIFF records it in the decoded page's directory, the others in
     * their EXIF block. HEIF's decoder has already applied its container's. */
    if (f == PC_CF_TIFF) {
        orient = cx_tiff_orientation(b, n, false, 0);
    } else if (f == PC_CF_HEIF) {
        orient = 1;
    } else if (f == PC_CF_DPX) {
        orient = cx_dpx_orientation(b, n);
    } else {
        orient = img.meta.exif.has ? pc_exif_orientation(img.meta.exif.data, img.meta.exif.len) : 1;
    }
    return finish_decode(&img, orient, o, out, err);
}

pc_status pc_cdecode_tiff_page(const uint8_t *b, size_t n, size_t page, const pc_cdecode_opts *o, pc_cimage *out,
                               pc_error *err)
{
    pc_cimage img;
    pc_status st;
    pc_cimage_init(&img);
    st = cx_tiff_decode_page(b, n, true, page, &o->limits, &img, err);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    return finish_decode(&img, cx_tiff_orientation(b, n, true, page), o, out, err);
}

uint16_t pc_ctiff_orientation(const uint8_t *b, size_t n) { return cx_tiff_orientation(b, n, false, 0); }

pc_status pc_cdecode_exr_part(const uint8_t *b, size_t n, size_t part, const pc_cdecode_opts *o,
                              pc_cimage *out, pc_error *err)
{
    pc_cimage img;
    pc_status st;
    pc_cimage_init(&img);
    st = cx_exr_decode_part(b, n, part, &o->limits, &img, err);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    return finish_decode(&img, 1, o, out, err);
}

pc_status pc_cdecode_exr_channels(const uint8_t *b, size_t n, size_t part, const char *const *names,
                                  size_t nnames, const pc_cdecode_opts *o, pc_cimage *out, pc_error *err)
{
    pc_cimage img;
    pc_status st;
    pc_cimage_init(&img);
    st = cx_exr_decode_channels(b, n, part, names, nnames, &o->limits, &img, err);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    return finish_decode(&img, 1, o, out, err);
}

uint16_t pc_ctiff_page_orientation(const uint8_t *b, size_t n, size_t page)
{
    return cx_tiff_orientation(b, n, true, page);
}

pc_status pc_cencode(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o, pc_buf *out, pc_error *err)
{
    uint32_t mw;
    uint32_t mh;
    cx_plan p;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    pc_cencode_opts defaults;
    if (o == NULL) {
        defaults = pc_cencode_opts_default();
        o = &defaults;
    }
    if (!pc_cformat_caps(f).write) {
        return cx_unsupported(err, f, "encoding is not available in this build");
    }
    if (img->width == 0 || img->height == 0) {
        return cx_invalid(err, "cannot encode an empty image");
    }
    if (pc_cformat_max_dimensions(f, &mw, &mh) && (img->width > mw || img->height > mh)) {
        return cx_encode(err, f, "dimensions exceed %ux%u", (unsigned)mw, (unsigned)mh);
    }
    p = cx_plan_for(img, f, o);
    switch (f) {
    case PC_CF_PNG:
        st = cx_png_encode(img, p, o, &b, err);
        break;
    case PC_CF_JPEG:
        st = cx_jpeg_encode(img, p, o, &b, err);
        break;
    case PC_CF_TIFF:
        st = cx_tiff_encode(img, p, o, &b, err);
        break;
    case PC_CF_WEBP:
        st = cx_webp_encode(img, p, o, &b, err);
        break;
    case PC_CF_PNM:
        st = cx_pnm_encode(img, p, o, &b, err);
        break;
    case PC_CF_FITS:
        st = cx_fits_encode(img, p, o, &b, err);
        break;
    case PC_CF_EXR:
        st = cx_exr_encode(img, p, o, &b, err);
        break;
    case PC_CF_GIF:
        st = cx_gif_encode(img, p, &b, err);
        break;
    case PC_CF_BMP:
        st = cx_bmp_encode(img, p, &b, err);
        break;
    case PC_CF_TGA:
        st = cx_tga_encode(img, p, &b, err);
        break;
    case PC_CF_ICO:
        st = cx_ico_encode(img, p, &b, err);
        break;
    case PC_CF_QOI:
        st = cx_qoi_encode(img, p, &b, err);
        break;
    case PC_CF_HDR:
        st = cx_hdr_encode(img, p, &b, err);
        break;
    case PC_CF_DPX:
        st = cx_dpx_encode(img, p, o, &b, err);
        break;
    default:
        st = cx_unsupported(err, f, "encoding is not available in this build");
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
