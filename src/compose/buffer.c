/* pc_compose_buffer (lib.rs Buffer). */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "compose/internal.h"

pc_status pc_cmp_rect_pixels(pc_rect rect, size_t *out, pc_error *err)
{
    uint64_t n = (uint64_t)pc_rect_width(rect) * (uint64_t)pc_rect_height(rect);
    if (n > PC_COMPOSE_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: %ux%u pixels is too large to render", pc_rect_width(rect),
                       pc_rect_height(rect));
    }
    *out = (size_t)n;
    return PC_OK;
}

static pc_status alloc_px(pc_rect rect, bool zero, pc_compose_buffer *out, pc_error *err)
{
    size_t n;
    pc_status st = pc_cmp_rect_pixels(rect, &n, err);
    float (*px)[4] = NULL;
    if (st != PC_OK) {
        return st;
    }
    if (n > 0) {
        px = zero ? pc_calloc_array(n, sizeof *px) : pc_malloc_array(n, sizeof *px);
        if (px == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        }
    }
    out->rect = rect;
    out->px = px;
    out->len = n;
    return PC_OK;
}

pc_status pc_compose_buffer_transparent(pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    return alloc_px(rect, true, out, err);
}

pc_status pc_compose_buffer_filled(pc_rect rect, const float c[4], pc_compose_buffer *out, pc_error *err)
{
    pc_compose_buffer b;
    size_t i;
    pc_status st = alloc_px(rect, false, &b, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < b.len; i++) {
        memcpy(b.px[i], c, sizeof b.px[i]);
    }
    *out = b;
    return PC_OK;
}

pc_status pc_compose_buffer_clone(const pc_compose_buffer *b, pc_compose_buffer *out, pc_error *err)
{
    pc_compose_buffer c = {b->rect, NULL, b->len};
    if (b->len > 0) {
        c.px = pc_malloc_array(b->len, sizeof *c.px);
        if (c.px == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        }
        memcpy(c.px, b->px, b->len * sizeof *c.px);
    }
    *out = c;
    return PC_OK;
}

void pc_compose_buffer_fini(pc_compose_buffer *b)
{
    if (b == NULL) {
        return;
    }
    free(b->px);
    b->px = NULL;
    b->len = 0;
    b->rect = PC_RECT_EMPTY;
}

const float *pc_compose_buffer_get(const pc_compose_buffer *b, int32_t x, int32_t y)
{
    size_t i;
    if (!pc_rect_contains(b->rect, x, y)) {
        return NULL;
    }
    i = (size_t)((int64_t)y - b->rect.y0) * pc_cmp_w(b->rect) + (size_t)((int64_t)x - b->rect.x0);
    return i < b->len ? b->px[i] : NULL;
}

pc_status pc_compose_buffer_to_rgba8(const pc_compose_buffer *b, pc_rgba8_image *out, pc_error *err)
{
    pc_rgba8_image img;
    size_t i;
    int c;
    pc_status st = pc_rgba8_image_init(&img, pc_rect_width(b->rect), pc_rect_height(b->rect), err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < b->len && img.pixels != NULL; i++) {
        for (c = 0; c < 4; c++) {
            img.pixels[i * 4 + (size_t)c] = pc_f32_to_u8(pc_clampf(b->px[i][c], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    *out = img;
    return PC_OK;
}

void pc_compose_buffer_over_background(pc_compose_buffer *b, const float bg[3])
{
    size_t i;
    int c;
    for (i = 0; i < b->len; i++) {
        float *p = b->px[i];
        float a = p[3];
        for (c = 0; c < 3; c++) {
            p[c] = p[c] * a + bg[c] * (1.0f - a);
        }
        p[3] = 1.0f;
    }
}

bool pc_compose_buffer_eq(const pc_compose_buffer *a, const pc_compose_buffer *b)
{
    return pc_rect_eq(a->rect, b->rect) && a->len == b->len &&
           (a->len == 0 || memcmp(a->px, b->px, a->len * sizeof *a->px) == 0);
}
