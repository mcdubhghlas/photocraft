/* HEIF / HEIC, read-only (port of crates/codecs/src/codecs/heif.rs and crates/heif): libheif
 * (linked dynamically, REWRITE_PLAN.md D18) decodes the primary image, 8-bit or (above 8 bits)
 * 16-bit RGB(A), with its ICC profile, EXIF (as a TIFF structure, without HEIF's offset header)
 * and XMP. HEIF records orientation in the container (irot/imir/clap), which the decoder applies
 * unless keep_orientation; the EXIF orientation is then rewritten to 1 (it only mirrors the
 * container). Built when libheif is found (WITH_HEIF); without it HEIF is detected, and opening
 * one says the build has no HEIF support. */
#include <stdlib.h>
#include <string.h>

#include "base/utf8.h"
#include "codecs/internal.h"
#include "codecs/orientation.h"

#define F PC_CF_HEIF
#define SEQUENCE "this file holds an image sequence; only HEIF still images can be opened"

#ifndef PC_NO_HEIF

#include <libheif/heif.h>
#include <pthread.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static bool have_hevc;

static void init_once(void)
{
    (void)heif_init(NULL);
    have_hevc = heif_have_decoder_for_format(heif_compression_HEVC) != 0;
}

bool cx_heif_available(void)
{
    (void)pthread_once(&once, init_once);
    return have_hevc;
}

static pc_status map_err(struct heif_error e, pc_error *err)
{
    /* Some libheif messages end in a newline; it is not part of ours. */
    const char *m = e.message != NULL ? e.message : "";
    int n = (int)strnlen(m, 1024);
    while (n > 0 && (m[n - 1] == '\n' || m[n - 1] == '\r')) {
        n--;
    }
    switch (e.code) {
    case heif_error_Unsupported_feature:
    case heif_error_Unsupported_filetype:
    case heif_error_Plugin_loading_error:
        return cx_unsupported(err, F, "%.*s", n, m);
    case heif_error_Memory_allocation_error:
        return cx_limit(err, "%.*s", n, m);
    default:
        if (e.subcode == heif_suberror_Security_limit_exceeded) {
            return cx_limit(err, "%.*s", n, m);
        }
        return cx_malformed(err, F, "%.*s", n, m);
    }
}

/* A top-level box of this type (the file has a meta box unless it is only a sequence). */
static bool has_box(const uint8_t *b, size_t n, const char *type)
{
    size_t at = 0;
    while (n - at >= 8) {
        uint64_t size = cx_be32(b + at);
        size_t hdr = 8;
        if (memcmp(b + at + 4, type, 4) == 0) {
            return true;
        }
        if (size == 1) {
            if (n - at < 16) {
                return false;
            }
            size = ((uint64_t)cx_be32(b + at + 8) << 32) | cx_be32(b + at + 12);
            hdr = 16;
        } else if (size == 0) {
            return false;
        }
        if (size < hdr || size > n - at) {
            return false;
        }
        at += (size_t)size;
    }
    return false;
}

static pc_status read_metadata(const struct heif_image_handle *h, pc_cimage *img, bool keep, pc_error *err)
{
    heif_item_id ids[16];
    int n;
    int i;
    pc_status st = PC_OK;
    size_t sz = heif_image_handle_get_raw_color_profile_size(h);
    if (sz > 0) {
        uint8_t *p = malloc(sz);
        if (p == NULL) {
            return cx_nomem(err);
        }
        if (heif_image_handle_get_raw_color_profile(h, p).code == heif_error_Ok) {
            st = pc_meta_blob_set(&img->icc, p, sz, err);
        }
        free(p);
        if (st != PC_OK) {
            return st;
        }
    }
    n = heif_image_handle_get_list_of_metadata_block_IDs(h, "Exif", ids, 16);
    for (i = 0; i < n && !img->meta.exif.has; i++) {
        size_t len = heif_image_handle_get_metadata_size(h, ids[i]);
        uint8_t *p = len >= 4 ? malloc(len) : NULL;
        if (p != NULL && heif_image_handle_get_metadata(h, ids[i], p).code == heif_error_Ok) {
            /* a 4-byte offset to the TIFF header comes first */
            uint32_t off = cx_be32(p);
            if (off <= len - 4) {
                const uint8_t *t = p + 4 + off;
                size_t tn = len - 4 - off;
                uint8_t *up = NULL;
                size_t un = 0;
                if (!keep) {
                    st = pc_upright_exif(t, tn, &up, &un, err);
                }
                if (st == PC_OK) {
                    st = pc_meta_blob_set(&img->meta.exif, up != NULL ? up : t, up != NULL ? un : tn, err);
                }
                free(up);
            }
        }
        free(p);
        if (st != PC_OK) {
            return st;
        }
    }
    n = heif_image_handle_get_list_of_metadata_block_IDs(h, "mime", ids, 16);
    for (i = 0; i < n && !img->meta.xmp.has; i++) {
        const char *ct = heif_image_handle_get_metadata_content_type(h, ids[i]);
        size_t len = heif_image_handle_get_metadata_size(h, ids[i]);
        uint8_t *p;
        if (ct == NULL || strcmp(ct, "application/rdf+xml") != 0 || len == 0) {
            continue;
        }
        p = malloc(len);
        if (p != NULL && heif_image_handle_get_metadata(h, ids[i], p).code == heif_error_Ok) {
            size_t k = 0;
            uint32_t cp;
            bool ok = true;
            while (k < len) {
                if (!pc_utf8_next(p, len, &k, &cp)) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                char *up = NULL;
                size_t un = 0;
                while (len > 0 && p[len - 1] == 0) {
                    len--;
                }
                if (!keep) {
                    st = pc_upright_xmp((const char *)p, len, &up, &un, err);
                }
                if (st == PC_OK) {
                    st = pc_meta_blob_set(&img->meta.xmp, up != NULL ? (const uint8_t *)up : p, up != NULL ? un : len, err);
                }
                free(up);
            }
        }
        free(p);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status cx_heif_decode(const uint8_t *b, size_t n, const pc_climits *l, bool keep_orientation, pc_cimage *out,
                         pc_error *err)
{
    struct heif_context *ctx;
    struct heif_image_handle *h = NULL;
    struct heif_image *im = NULL;
    struct heif_decoding_options *opts = NULL;
    struct heif_error e;
    bool alpha;
    bool sixteen;
    int bits;
    uint32_t w;
    uint32_t hh;
    pc_clayout layout;
    pc_csample sample;
    pc_cimage img;
    pc_status st;
    if (!cx_heif_available()) {
        return cx_unsupported(err, F, "this build's libheif has no HEVC decoder");
    }
    if (n >= 8 && has_box(b, n, "ftyp") && !has_box(b, n, "meta")) {
        return cx_unsupported(err, F, SEQUENCE);
    }
    ctx = heif_context_alloc();
    if (ctx == NULL) {
        return cx_nomem(err);
    }
    e = heif_context_read_from_memory_without_copy(ctx, b, n, NULL);
    if (e.code != heif_error_Ok) {
        st = map_err(e, err);
        heif_context_free(ctx);
        return st;
    }
    e = heif_context_get_primary_image_handle(ctx, &h);
    if (e.code != heif_error_Ok) {
        st = map_err(e, err);
        heif_context_free(ctx);
        return st;
    }
    alpha = heif_image_handle_has_alpha_channel(h) != 0;
    bits = heif_image_handle_get_luma_bits_per_pixel(h);
    sixteen = bits > 8;
    layout = alpha ? PC_CL_RGBA : PC_CL_RGB;
    sample = sixteen ? PC_CS_U16 : PC_CS_U8;
    if (keep_orientation) {
        w = (uint32_t)heif_image_handle_get_ispe_width(h);
        hh = (uint32_t)heif_image_handle_get_ispe_height(h);
    } else {
        w = (uint32_t)heif_image_handle_get_width(h);
        hh = (uint32_t)heif_image_handle_get_height(h);
    }
    /* The declared size is checked before any pixel is decoded. */
    if ((st = pc_climits_check(l, w, hh, layout, sample, err)) != PC_OK) {
        heif_image_handle_release(h);
        heif_context_free(ctx);
        return st;
    }
    opts = heif_decoding_options_alloc();
    if (opts != NULL) {
        opts->ignore_transformations = keep_orientation ? 1 : 0;
        /* A truncated or damaged file is an error, as in the reference (libheif would otherwise
         * fill missing grid tiles and only warn). */
        opts->strict_decoding = 1;
    }
    {
        enum heif_chroma chroma;
        bool le;
        const uint16_t one = 1;
        uint8_t first;
        memcpy(&first, &one, 1);
        le = first == 1;
        if (sixteen) {
            chroma = alpha ? (le ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE)
                           : (le ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE);
        } else {
            chroma = alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB;
        }
        e = heif_decode_image(h, &im, heif_colorspace_RGB, chroma, opts);
    }
    heif_decoding_options_free(opts);
    if (e.code != heif_error_Ok) {
        st = map_err(e, err);
        heif_image_handle_release(h);
        heif_context_free(ctx);
        return st;
    }
    {
        int stride = 0;
        const uint8_t *plane = heif_image_get_plane_readonly(im, heif_channel_interleaved, &stride);
        uint32_t iw = (uint32_t)heif_image_get_width(im, heif_channel_interleaved);
        uint32_t ih = (uint32_t)heif_image_get_height(im, heif_channel_interleaved);
        int dbits = heif_image_get_bits_per_pixel_range(im, heif_channel_interleaved);
        size_t row;
        size_t y;
        if (plane == NULL || stride <= 0) {
            heif_image_release(im);
            heif_image_handle_release(h);
            heif_context_free(ctx);
            return cx_malformed(err, F, "the decoder returned no image");
        }
        /* trust the decoded image's shape, not the container's */
        if ((st = pc_climits_check(l, iw, ih, layout, sample, err)) != PC_OK ||
            (st = pc_cimage_new(iw, ih, layout, sample, &img, err)) != PC_OK) {
            heif_image_release(im);
            heif_image_handle_release(h);
            heif_context_free(ctx);
            return st;
        }
        row = (size_t)iw * pc_clayout_channels(layout) * pc_csample_bytes(sample);
        if ((size_t)stride < row) {
            pc_cimage_fini(&img);
            heif_image_release(im);
            heif_image_handle_release(h);
            heif_context_free(ctx);
            return cx_malformed(err, F, "short image rows");
        }
        for (y = 0; y < ih; y++) {
            memcpy(img.data + y * row, plane + y * (size_t)stride, row);
        }
        if (sixteen && dbits > 0 && dbits < 16) {
            /* widen e.g. 10-bit samples to the 16-bit range */
            uint32_t max = (1u << dbits) - 1;
            size_t i;
            for (i = 0; i < img.len / 2; i++) {
                uint16_t v;
                memcpy(&v, img.data + 2 * i, 2);
                v = (uint16_t)(((uint32_t)(v > max ? max : v) * 65535u + max / 2) / max);
                memcpy(img.data + 2 * i, &v, 2);
            }
        }
    }
    heif_image_release(im);
    st = read_metadata(h, &img, keep_orientation, err);
    heif_image_handle_release(h);
    heif_context_free(ctx);
    if (st != PC_OK) {
        pc_cimage_fini(&img);
        return st;
    }
    *out = img;
    return PC_OK;
}

#else

bool cx_heif_available(void) { return false; }

pc_status cx_heif_decode(const uint8_t *b, size_t n, const pc_climits *l, bool keep_orientation, pc_cimage *out,
                         pc_error *err)
{
    (void)b;
    (void)n;
    (void)l;
    (void)keep_orientation;
    (void)out;
    return cx_unsupported(err, F, "HEIC/HEIF support isn't included in this build of PhotoCraft");
}

#endif
