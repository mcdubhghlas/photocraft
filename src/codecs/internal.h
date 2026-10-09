/* Internal helpers of the codecs module. Not public. */
#ifndef PC_CODECS_INTERNAL_H
#define PC_CODECS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "base/buf.h"
#include "base/status.h"
#include "codecs/codecs.h"

/* ---- errors with the reference's messages ---- */

#define cx_malformed(err, f, ...) (cx_fail_fmt_set((err), PC_ERR_FORMAT, (f), "malformed data: ", __VA_ARGS__), (pc_status)PC_ERR_FORMAT)
#define cx_unsupported(err, f, ...) (cx_fail_fmt_set((err), PC_ERR_UNSUPPORTED, (f), "", __VA_ARGS__), (pc_status)PC_ERR_UNSUPPORTED)
#define cx_encode(err, f, ...) (cx_fail_fmt_set((err), PC_ERR_UNSUPPORTED, (f), "encode failed: ", __VA_ARGS__), (pc_status)PC_ERR_UNSUPPORTED)
#define cx_limit(err, ...) pc_fail((err), PC_ERR_LIMIT, "limit exceeded: " __VA_ARGS__)
#define cx_invalid(err, ...) pc_fail((err), PC_ERR_INVALID, "invalid image: " __VA_ARGS__)
#define cx_nomem(err) pc_fail((err), PC_ERR_NOMEM, "out of memory")
/* "<Fmt>: <prefix><message>"; evaluates to code. */
/* Sets err (may be NULL) to "<Fmt>: <prefix><fmt>"; the macros above evaluate to the code, like pc_fail. */
void cx_fail_fmt_set(pc_error *err, pc_status code, pc_cformat f, const char *prefix, const char *fmt, ...);

#define CK(x)                                                                                      \
    do {                                                                                           \
        if ((st = (x)) != PC_OK) {                                                                 \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

/* ---- encode plan ---- */

typedef struct cx_plan {
    pc_clayout layout;
    pc_csample sample;
} cx_plan;

cx_plan cx_plan_for(const pc_cimage *img, pc_cformat f, const pc_cencode_opts *o);

/* Limits::check_bytes and alloc_usize. */
pc_status cx_check_bytes(const pc_climits *l, uint32_t w, uint32_t h, uint64_t bytes_per_pixel, pc_error *err);
size_t cx_alloc_usize(const pc_climits *l);

/* ---- sample conversion ---- */

float cx_read_normalized(const uint8_t *data, pc_csample s, size_t i);
void cx_quantize(const float *values, size_t n, pc_csample s, uint8_t *out);
/* Converts n pixels of normalised samples between layouts (out holds n * to channels). */
void cx_convert_layout(const float *src, size_t n, pc_clayout from, pc_clayout to, float *out);

/* ---- per-format codecs ---- */

pc_status cx_png_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_png_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err);
pc_status cx_jpeg_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_jpeg_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err);
/* JPEG's EXIF / XMP APP1 segments, or false when they don't fit one segment. *seg replaced. */
bool cx_jpeg_exif_segment(const uint8_t *exif, size_t n, pc_buf *seg);
bool cx_jpeg_xmp_segment(const uint8_t *xmp, size_t n, pc_buf *seg);
pc_status cx_tiff_decode_page(const uint8_t *b, size_t n, bool has_page, size_t page, const pc_climits *l,
                              pc_cimage *out, pc_error *err);
uint16_t cx_tiff_orientation(const uint8_t *b, size_t n, bool has_page, size_t page);
pc_status cx_tiff_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err);
pc_status cx_webp_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_webp_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err);
pc_status cx_pnm_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_pnm_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err);
pc_status cx_exr_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_exr_encode(const pc_cimage *img, cx_plan p, const pc_cencode_opts *o, pc_buf *out, pc_error *err);
pc_status cx_heif_decode(const uint8_t *b, size_t n, const pc_climits *l, bool keep_orientation, pc_cimage *out,
                         pc_error *err);
bool cx_heif_available(void);
bool cx_exr_available(void);
pc_status cx_gif_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_gif_encode(const pc_cimage *img, cx_plan p, pc_buf *out, pc_error *err);
pc_status cx_bmp_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_bmp_encode(const pc_cimage *img, cx_plan p, pc_buf *out, pc_error *err);
/* A BMP DIB without its file header (ICO entries): `ico` halves the height and applies the
 * AND mask. */
pc_status cx_bmp_decode_dib(const uint8_t *b, size_t n, bool ico, const pc_climits *l, pc_cimage *out,
                            pc_error *err);
pc_status cx_tga_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_tga_encode(const pc_cimage *img, cx_plan p, pc_buf *out, pc_error *err);
pc_status cx_ico_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_ico_encode(const pc_cimage *img, cx_plan p, pc_buf *out, pc_error *err);
pc_status cx_qoi_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_qoi_encode(const pc_cimage *img, cx_plan p, pc_buf *out, pc_error *err);
pc_status cx_hdr_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err);
pc_status cx_hdr_encode(const pc_cimage *img, cx_plan p, pc_buf *out, pc_error *err);

/* GIF LZW (web.c): compresses indices (each < 2^min_code_size) and appends to out. */
pc_status cx_gif_lzw_encode(const uint8_t *indices, size_t n, uint8_t min_code_size, pc_buf *out, pc_error *err);

/* ---- small helpers ---- */

static inline uint16_t cx_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t cx_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t cx_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t cx_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
/* A sticky-error byte writer on a pc_buf. */
typedef struct cx_w {
    pc_buf *b;
    pc_status st;
    pc_error *err;
} cx_w;
void cx_w_put(cx_w *w, const void *d, size_t n);
void cx_w_byte(cx_w *w, uint8_t v);
void cx_w_le16(cx_w *w, uint16_t v);
void cx_w_le32(cx_w *w, uint32_t v);
void cx_w_be16(cx_w *w, uint16_t v);
void cx_w_be32(cx_w *w, uint32_t v);

/* Native-endian u16 at sample index i of data. */
static inline uint16_t cx_ne16(const uint8_t *d, size_t i)
{
    uint16_t v;
    memcpy(&v, d + 2 * i, 2);
    return v;
}

#endif
