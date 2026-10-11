/* Area-averaged reductions of the composite (lib.rs render_reduced*), rendered in bands so no
 * full-size composite is held. Every output pixel sums its source pixels premultiplied, rows top
 * to bottom and columns left to right, whatever the band height and thread count. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "compose/internal.h"

/* Output pixel t of n covers source [t f / n, (t + 1) f / n) (at least one pixel). */
static void span(size_t t, size_t f, size_t n, size_t *a, size_t *z)
{
    size_t lo = t * f / n;
    size_t hi = (t + 1) * f / n;
    if (hi < lo + 1) {
        hi = lo + 1;
    }
    *a = lo;
    *z = hi < f ? hi : f;
}

/* The output pixel whose span holds source column / row x. */
static size_t cell(size_t x, size_t f, size_t n)
{
    size_t v = (x + 1) * n;
    return (v > 0 ? v - 1 : 0) / f;
}

typedef struct reduce_job {
    pc_rect b; /* the reduced area */
    size_t fw, fh, w, h;
    size_t ox0, oy0, ow;
    size_t sx0, sx1;
    const uint32_t *cols; /* output column of each source column in [sx0, sx1) */
    const uint32_t *rows; /* output row of each source row */
    float (*acc)[4];
    /* current band */
    const pc_compose_buffer *band;
    size_t by0, by1, t0;
} reduce_job;

/* Each output row sums its source rows in this band (in order, top to bottom). */
static pc_status sum_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    reduce_job *j = ctx;
    size_t sw = j->sx1 - j->sx0;
    size_t r;
    (void)chunk;
    for (r = begin; r < end; r++) {
        size_t t = j->t0 + r;
        float (*out)[4] = j->acc + (t - j->oy0) * j->ow;
        size_t a;
        size_t z;
        size_t y;
        span(t, j->fh, j->h, &a, &z);
        for (y = (a > j->by0 ? a : j->by0); y < (z < j->by1 ? z : j->by1); y++) {
            const float (*src)[4] = (const float (*)[4])j->band->px + (y - j->by0) * sw;
            size_t x;
            for (x = 0; x < sw; x++) {
                const float *p = src[x];
                float *o = out[j->cols[x] - j->ox0];
                int c;
                for (c = 0; c < 3; c++) {
                    o[c] += p[c] * p[3];
                }
                o[3] += p[3];
            }
        }
    }
    return PC_OK;
}

static pc_status reduce_sink(void *ctx, pc_compose_buffer *band, pc_error *err)
{
    reduce_job *j = ctx;
    size_t t1;
    if (band->len != (size_t)pc_rect_height(band->rect) * (j->sx1 - j->sx0) || pc_rect_is_empty(band->rect)) {
        return pc_fail(err, PC_ERR_INVALID, "compose: unexpected band");
    }
    j->band = band;
    j->by0 = (size_t)((int64_t)band->rect.y0 - j->b.y0);
    j->by1 = (size_t)((int64_t)band->rect.y1 - j->b.y0);
    j->t0 = j->rows[j->by0];
    t1 = (size_t)j->rows[j->by1 - 1] + 1;
    return pc_pool_for(pc_pool_global(), t1 - j->t0, 1, sum_rows, j);
}

static uint32_t *map_spans(size_t f, size_t n, pc_error *err)
{
    uint32_t *m = pc_malloc_array(f, sizeof *m);
    size_t t;
    if (m == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
        return NULL;
    }
    for (t = 0; t < n; t++) {
        size_t a;
        size_t z;
        size_t i;
        span(t, f, n, &a, &z);
        for (i = a; i < z; i++) {
            m[i] = (uint32_t)t;
        }
    }
    return m;
}

pc_status pc_compose_render_reduced_in_bands(const pc_document *doc, pc_rect b, uint32_t w32, uint32_t h32,
                                             const pc_rect *damage, int32_t band_rows, pc_compose_buffer *out,
                                             pc_error *err)
{
    size_t fw = pc_cmp_w(b);
    size_t fh = (size_t)pc_rect_height(b);
    size_t w = w32 < 1 ? 1 : w32;
    size_t h = h32 < 1 ? 1 : h32;
    pc_rect full;
    pc_rect o;
    reduce_job j;
    size_t oh;
    size_t sy0;
    size_t sy1;
    size_t a;
    size_t z;
    size_t i;
    pc_rect src;
    uint32_t *cols = NULL;
    uint32_t *rows = NULL;
    pc_status st;
    if (w > (fw > 1 ? fw : 1)) {
        w = fw > 1 ? fw : 1;
    }
    if (h > (fh > 1 ? fh : 1)) {
        h = fh > 1 ? fh : 1;
    }
    full = pc_rect_from_xywh(0, 0, (uint32_t)w, (uint32_t)h);
    if (fw == 0 || fh == 0) {
        return pc_compose_buffer_transparent(damage != NULL ? PC_RECT_EMPTY : full, out, err);
    }
    /* Bands are at least a tile of rows (or the whole area) high: refuse before allocating the
     * span maps when no band could be rendered anyway. */
    if (fw > PC_COMPOSE_MAX_PIXELS || fh > PC_COMPOSE_MAX_PIXELS ||
        (uint64_t)fw * (uint64_t)(fh < PC_COMPOSE_RENDER_TILE ? fh : PC_COMPOSE_RENDER_TILE) >
            PC_COMPOSE_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "compose: %zux%zu is too large to reduce", fw, fh);
    }
    /* Output pixels to produce and the document area they cover. */
    if (damage == NULL) {
        o = full;
    } else {
        pc_rect d = pc_rect_intersect(*damage, b);
        size_t x0;
        size_t x1;
        size_t y0;
        size_t y1;
        if (pc_rect_is_empty(d)) {
            return pc_compose_buffer_transparent(PC_RECT_EMPTY, out, err);
        }
        x0 = (size_t)((int64_t)d.x0 - b.x0);
        x1 = (size_t)((int64_t)d.x1 - b.x0);
        y0 = (size_t)((int64_t)d.y0 - b.y0);
        y1 = (size_t)((int64_t)d.y1 - b.y0);
        o = pc_rect_make((int32_t)cell(x0, fw, w), (int32_t)cell(y0, fh, h), (int32_t)cell(x1 - 1, fw, w) + 1,
                         (int32_t)cell(y1 - 1, fh, h) + 1);
    }
    memset(&j, 0, sizeof j);
    j.b = b;
    j.fw = fw;
    j.fh = fh;
    j.w = w;
    j.h = h;
    j.ox0 = (size_t)o.x0;
    j.oy0 = (size_t)o.y0;
    j.ow = pc_cmp_w(o);
    oh = (size_t)pc_rect_height(o);
    span(j.ox0, fw, w, &a, &z);
    j.sx0 = a;
    span(j.ox0 + j.ow - 1, fw, w, &a, &z);
    j.sx1 = z;
    span(j.oy0, fh, h, &a, &z);
    sy0 = a;
    span(j.oy0 + oh - 1, fh, h, &a, &z);
    sy1 = z;
    src = pc_rect_make((int32_t)((int64_t)b.x0 + (int64_t)j.sx0), (int32_t)((int64_t)b.y0 + (int64_t)sy0),
                       (int32_t)((int64_t)b.x0 + (int64_t)j.sx1), (int32_t)((int64_t)b.y0 + (int64_t)sy1));
    if (w == fw && h == fh) {
        pc_compose_buffer px;
        st = pc_compose_render(doc, src, &px, err);
        if (st == PC_OK) {
            px.rect = o;
            *out = px;
        }
        return st;
    }
    cols = map_spans(fw, w, err);
    rows = cols != NULL ? map_spans(fh, h, err) : NULL;
    if (rows == NULL) {
        free(cols);
        return PC_ERR_NOMEM;
    }
    j.cols = cols + j.sx0;
    j.rows = rows;
    st = pc_compose_buffer_transparent(o, out, err);
    if (st == PC_OK) {
        j.acc = out->px;
        st = pc_compose_render_bands(doc, src, band_rows, NULL, reduce_sink, &j, err);
        if (st != PC_OK) {
            pc_compose_buffer_fini(out);
        }
    }
    free(cols);
    free(rows);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < out->len; i++) {
        float *p = out->px[i];
        size_t x0;
        size_t x1;
        size_t y0;
        size_t y1;
        float n;
        float al = p[3];
        span(j.ox0 + i % j.ow, fw, w, &x0, &x1);
        span(j.oy0 + i / j.ow, fh, h, &y0, &y1);
        n = (float)((y1 - y0) * (x1 - x0) > 0 ? (y1 - y0) * (x1 - x0) : 1);
        if (al > 0.0f) {
            p[0] = p[0] / al;
            p[1] = p[1] / al;
            p[2] = p[2] / al;
            p[3] = al / n;
        } else {
            memset(p, 0, 4 * sizeof(float));
        }
    }
    return PC_OK;
}

pc_status pc_compose_render_reduced(const pc_document *doc, uint32_t w, uint32_t h, pc_compose_buffer *out,
                                    pc_error *err)
{
    return pc_compose_render_reduced_in_bands(doc, pc_document_bounds(doc), w, h, NULL, 0, out, err);
}

pc_status pc_compose_render_reduced_rect(const pc_document *doc, pc_rect rect, uint32_t w, uint32_t h,
                                         pc_compose_buffer *out, pc_error *err)
{
    return pc_compose_render_reduced_in_bands(doc, rect, w, h, NULL, 0, out, err);
}

pc_status pc_compose_render_reduced_damage(const pc_document *doc, uint32_t w, uint32_t h, pc_rect damage,
                                           pc_compose_buffer *out, pc_error *err)
{
    return pc_compose_render_reduced_in_bands(doc, pc_document_bounds(doc), w, h, &damage, 0, out, err);
}
