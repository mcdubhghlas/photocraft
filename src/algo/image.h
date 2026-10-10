/* Interleaved float image regions and sampling helpers (port of crates/algo/src/image.rs).
 *
 * A pc_algo_image is a rectangle of interleaved, normalised f32 samples read from a surface in
 * the surface's own colour model and depth (alpha last when the surface has alpha). Filter
 * kernels read their input through it; most return their output as a plain float buffer.
 *
 * Ownership: init/fini; `data` is owned (free()) and NULL when the rect is empty. All readers
 * are const and thread-safe; out-of-range reads return 0 (the reference panics only for indices
 * past the whole buffer, which C reports as 0 too). */
#ifndef PC_ALGO_IMAGE_H
#define PC_ALGO_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* How samples outside an image are read. */
typedef enum pc_algo_edge {
    PC_ALGO_EDGE_TRANSPARENT, /* zero */
    PC_ALGO_EDGE_REPEAT,      /* clamp to the nearest edge pixel */
    PC_ALGO_EDGE_WRAP         /* wrap around */
} pc_algo_edge;

typedef struct pc_algo_image {
    pc_rect rect;
    size_t ch;   /* channels per pixel */
    float *data; /* rect.w * rect.h * ch samples; NULL when empty */
    size_t len;  /* number of floats in data */
} pc_algo_image;

/* A zeroed image of rect x ch. PC_ERR_LIMIT past PC_MAX_ALLOC. `img` is written on success. */
pc_status pc_algo_image_init(pc_algo_image *img, pc_rect rect, size_t ch, pc_error *err);
/* Takes ownership of `data` (len must be rect.w * rect.h * ch, else PC_ERR_INVALID and data is
 * NOT freed). */
pc_status pc_algo_image_wrap(pc_algo_image *img, pc_rect rect, size_t ch, float *data, size_t len,
                             pc_error *err);
void pc_algo_image_fini(pc_algo_image *img); /* NULL-safe; leaves an empty image */
pc_status pc_algo_image_clone(const pc_algo_image *src, pc_algo_image *out, pc_error *err);

/* Image::read: `rect` of a surface. */
pc_status pc_algo_image_read(const pc_surface *s, pc_rect rect, pc_algo_image *out, pc_error *err);
/* Image::read_clamped: `rect`, repeating the nearest pixel of `extent` for samples outside it
 * (Photoshop treats the canvas edge this way when filtering a layer). */
pc_status pc_algo_image_read_clamped(const pc_surface *s, pc_rect rect, pc_rect extent, pc_algo_image *out,
                                     pc_error *err);

static inline int32_t pc_algo_image_w(const pc_algo_image *img)
{
    return (int32_t)pc_rect_width(img->rect);
}

/* Sample at integer coordinates; 0 outside. */
float pc_algo_image_get(const pc_algo_image *img, int32_t x, int32_t y, size_t c);
/* Pointer to the pixel at (x, y) (ch samples), or NULL outside. Borrowed. */
const float *pc_algo_image_px(const pc_algo_image *img, int32_t x, int32_t y);
/* Pointer to the samples of the one-pixel-high rect r (must lie inside), or NULL. */
const float *pc_algo_image_row(const pc_algo_image *img, pc_rect r);
/* Sample with an edge policy relative to `area` (0 when area is empty). */
float pc_algo_image_get_edge(const pc_algo_image *img, int32_t x, int32_t y, size_t c, pc_algo_edge edge,
                             pc_rect area);
/* Bilinear sample of the premultiplied pixel at (x, y) (pixel centres at .5), written to out[0..out_len)
 * as straight colour with alpha last (out_len should be ch). */
void pc_algo_image_sample(const pc_algo_image *img, float x, float y, pc_algo_edge edge, pc_rect area,
                          bool alpha, float *out, size_t out_len);
/* Copy of the samples inside `out` (zero outside the image) into dst of out.w * out.h * ch. */
void pc_algo_image_crop_into(const pc_algo_image *img, pc_rect out, float *dst);
/* The same, newly allocated (free()); *data is NULL for an empty rect. */
pc_status pc_algo_image_crop(const pc_algo_image *img, pc_rect out, float **data, size_t *len, pc_error *err);

/* Premultiplies colour channels by alpha (the last of ch) in place; no-op without alpha. */
void pc_algo_premultiply(float *data, size_t len, size_t ch, bool alpha);
/* Inverse of pc_algo_premultiply (alpha <= 1e-7 gives 0 colour). */
void pc_algo_unpremultiply(float *data, size_t len, size_t ch, bool alpha);

/* Floats for a w x h x ch buffer, checked against PC_MAX_ALLOC (false on overflow). */
bool pc_algo_float_count(pc_rect r, size_t ch, size_t *out);

#endif
