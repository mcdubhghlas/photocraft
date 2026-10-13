/* JPEG XL-compressed DNG strips and tiles (Adobe DNG Specification 1.7, Compression 52546; port of
 * crates/raw/src/jxl.rs, REWRITE_PLAN.md D30). Each strip or tile holds one JPEG XL codestream
 * (bare or in the ISO/IEC 18181-2 container) whose image is the segment's samples: one channel for
 * CFA data, three for LinearRaw (Samsung Expert RAW). The reference decodes with jxl-oxide; here
 * libjxl (BSD-3) does, optional via WITH_JXL. Without it (PC_NO_JXL) these segments report the
 * reference's "unsupported" text for builds without a decoder.
 *
 * Every allocation libjxl makes goes through a budgeted memory manager (a few times the output,
 * never more than the caller's per-buffer limit), like jxl-oxide's AllocTracker. libjxl runs
 * single-threaded here: segments are already decoded in parallel. */
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "raw/internal.h"

#ifdef PC_NO_JXL

pc_status rx_jxl_decode(const uint8_t *src, size_t n, size_t w, size_t h, size_t samples, uint32_t bits,
                        const pc_raw_limits *limits, uint16_t **out, pc_error *err)
{
    (void)src;
    (void)n;
    (void)w;
    (void)h;
    (void)samples;
    (void)bits;
    (void)limits;
    (void)out;
    return rx_unsupported(err, "JPEG XL-compressed DNG (DNG 1.7) can't be opened in the browser version of "
                               "PhotoCraft; open it in the desktop app");
}

#else

#include <math.h>

#include <jxl/decode.h>

#include "base/num.h"

/* A budget shared by every allocation of one decoder; each block carries its size in front. */
typedef struct budget {
    size_t left;
    bool refused;
} budget;

typedef union block_head {
    size_t size;
    max_align_t align;
} block_head;

static void *budget_alloc(void *opaque, size_t size)
{
    budget *b = opaque;
    block_head *h;
    if (size > b->left || size > SIZE_MAX - sizeof *h) {
        b->refused = true;
        return NULL;
    }
    h = malloc(sizeof *h + size);
    if (h == NULL) {
        return NULL;
    }
    h->size = size;
    b->left -= size;
    return h + 1;
}

static void budget_free(void *opaque, void *address)
{
    budget *b = opaque;
    block_head *h;
    if (address == NULL) {
        return;
    }
    h = (block_head *)address - 1;
    b->left += h->size;
    free(h);
}

pc_status rx_jxl_decode(const uint8_t *src, size_t n, size_t w, size_t h, size_t samples, uint32_t bits,
                        const pc_raw_limits *limits, uint16_t **out, pc_error *err)
{
    budget bud;
    JxlMemoryManager mm;
    JxlDecoder *dec;
    JxlBasicInfo info;
    JxlPixelFormat fmt;
    uint16_t *px = NULL;
    size_t total = 0;
    bool have_info = false;
    bool done = false;
    pc_status st;
    uint64_t cap;
    if ((st = pc_raw_limits_check(limits, w, h, (uint64_t)samples * 2, err)) != PC_OK) {
        return st;
    }
    /* libjxl decodes into float / int planes: allow a few times the output size, and never more
     * than the caller's per-buffer limit. */
    cap = (uint64_t)w * h * samples; /* fits: checked against the limits above */
    cap = cap > UINT64_MAX / 16 ? UINT64_MAX : cap * 16;
    cap = cap < limits->max_alloc ? cap : limits->max_alloc;
    bud.left = cap > SIZE_MAX ? SIZE_MAX : (size_t)cap;
    bud.refused = false;
    mm.opaque = &bud;
    mm.alloc = budget_alloc;
    mm.free = budget_free;
    dec = JxlDecoderCreate(&mm);
    if (dec == NULL) {
        return rx_malformed(err, "JPEG XL raw data: the decoder could not be created");
    }
    if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(dec, src, n) != JXL_DEC_SUCCESS) {
        JxlDecoderDestroy(dec);
        return rx_malformed(err, "JPEG XL raw data: the decoder could not be set up");
    }
    JxlDecoderCloseInput(dec);
    memset(&fmt, 0, sizeof fmt);
    fmt.data_type = JXL_TYPE_UINT16;
    fmt.endianness = JXL_NATIVE_ENDIAN;
    while (!done) {
        JxlDecoderStatus s = JxlDecoderProcessInput(dec);
        switch (s) {
        case JXL_DEC_BASIC_INFO: {
            size_t jw;
            size_t jh;
            if (JxlDecoderGetBasicInfo(dec, &info) != JXL_DEC_SUCCESS) {
                st = rx_malformed(err, "JPEG XL raw data: unreadable image header");
                goto fail;
            }
            /* The size with the orientation applied (as jxl-oxide reports it). */
            jw = info.orientation >= JXL_ORIENT_TRANSPOSE ? info.ysize : info.xsize;
            jh = info.orientation >= JXL_ORIENT_TRANSPOSE ? info.xsize : info.ysize;
            if (jw != w || jh != h) {
                st = rx_malformed(err, "JPEG XL raw tile is %zux%zu, expected %zux%zu", jw, jh, w, h);
                goto fail;
            }
            if ((size_t)info.num_color_channels != samples) {
                st = rx_unsupported(err, "JPEG XL raw tile with %u channels for %zu samples per pixel",
                                    (unsigned)info.num_color_channels, samples);
                goto fail;
            }
            have_info = true;
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            size_t need = 0;
            if (!have_info) {
                st = rx_malformed(err, "JPEG XL raw data: no image header");
                goto fail;
            }
            fmt.num_channels = (uint32_t)samples;
            total = w * h * samples;
            if (JxlDecoderImageOutBufferSize(dec, &fmt, &need) != JXL_DEC_SUCCESS ||
                need != total * sizeof *px) {
                st = rx_malformed(err, "JPEG XL raw data: unexpected output size");
                goto fail;
            }
            px = pc_malloc_array(total > 0 ? total : 1, sizeof *px);
            if (px == NULL) {
                st = rx_nomem(err);
                goto fail;
            }
            if (JxlDecoderSetImageOutBuffer(dec, &fmt, px, need) != JXL_DEC_SUCCESS) {
                st = rx_malformed(err, "JPEG XL raw data: the output buffer was refused");
                goto fail;
            }
            break;
        }
        case JXL_DEC_FULL_IMAGE:
            done = px != NULL;
            if (!done) {
                st = rx_malformed(err, "JPEG XL raw data is truncated");
                goto fail;
            }
            break;
        case JXL_DEC_SUCCESS:
            if (px == NULL) {
                st = rx_malformed(err, "JPEG XL raw data is truncated");
                goto fail;
            }
            done = true;
            break;
        case JXL_DEC_NEED_MORE_INPUT:
            st = rx_malformed(err, "JPEG XL raw data is truncated");
            goto fail;
        case JXL_DEC_ERROR:
        default:
            st = rx_malformed(err, "JPEG XL raw data: %s",
                              bud.refused ? "allocation budget exceeded" : "the codestream is not valid");
            goto fail;
        }
    }
    JxlDecoderDestroy(dec);
    /* 16-bit samples come back as stored; other depths are scaled to 0..65535, so scale them back to
     * the TIFF's range (exact for lossless data). */
    if (bits < 16) {
        double max = (double)((1u << bits) - 1);
        size_t i;
        for (i = 0; i < total; i++) {
            px[i] = pc_f64_to_u16(round((double)px[i] * max / 65535.0));
        }
    }
    *out = px;
    return PC_OK;
fail:
    JxlDecoderDestroy(dec);
    free(px);
    return st;
}

#endif
