/* Smart Blur, Lens Blur and the shaped-kernel machinery (port of crates/algo/src/blur2.rs).
 *
 * Shaped kernels are applied as horizontal spans over per-row prefix sums (f64, to keep 16-bit
 * precision), so a kernel of radius r costs O(r) per pixel instead of O(r^2). */
#include "algo/blur_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define PI_F 3.14159265358979323846f
#define TAU_F 6.28318530717958647692f

/* ---- span kernels ---- */

void pc_algo_spans_fini(pc_algo_spans *s)
{
    if (s != NULL) {
        free(s->rows);
        s->rows = NULL;
        s->n = 0;
    }
}

static pc_status spans_push(pc_algo_spans *s, size_t *cap, int32_t dy, int32_t a, int32_t b, pc_error *err)
{
    if (s->n == *cap) {
        size_t nc = *cap > 0 ? *cap * 2 : 64;
        int32_t (*nr)[3] = realloc(s->rows, nc * sizeof *nr);
        if (nr == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a blur kernel");
        }
        s->rows = nr;
        *cap = nc;
    }
    s->rows[s->n][0] = dy;
    s->rows[s->n][1] = a;
    s->rows[s->n][2] = b;
    s->n++;
    return PC_OK;
}

pc_status pc_algo_spans_new(float r, pc_algo_inside_fn inside, const void *ictx, pc_algo_spans *out,
                            pc_error *err)
{
    pc_algo_spans s = {NULL, 0, 0.0};
    size_t cap = 0;
    int32_t ri = pc_f32_to_i32(ceilf(pc_rs_maxf_k(r, 0.0f))), dy, dx;
    pc_status st;
    if (r < 0.5f) {
        st = spans_push(&s, &cap, 0, 0, 0, err);
        if (st != PC_OK) {
            return st;
        }
        s.count = 1.0;
        *out = s;
        return PC_OK;
    }
    /* The rasterisation is O(r^2); radii beyond this are refused before allocating. */
    if (ri > 1 << 16) {
        return pc_fail(err, PC_ERR_LIMIT, "blur kernel radius %g is too large", (double)r);
    }
    for (dy = -ri; dy <= ri; dy++) {
        bool in_run = false;
        int32_t start = 0;
        for (dx = -ri; dx <= ri + 1; dx++) {
            bool hit = dx <= ri && inside(ictx, (float)dx / r, (float)dy / r);
            if (hit && !in_run) {
                in_run = true;
                start = dx;
            } else if (!hit && in_run) {
                st = spans_push(&s, &cap, dy, start, dx - 1, err);
                if (st != PC_OK) {
                    pc_algo_spans_fini(&s);
                    return st;
                }
                s.count += (double)(dx - start);
                in_run = false;
            }
        }
    }
    if (s.n == 0) {
        st = spans_push(&s, &cap, 0, 0, 0, err);
        if (st != PC_OK) {
            return st;
        }
        s.count = 1.0;
    }
    *out = s;
    return PC_OK;
}

/* Per-row prefix sums of an interleaved window: (w + 1) x h x n. */
typedef struct prefix {
    size_t w, h, n;
    double *data;
} prefix;

static pc_status prefix_new(const float *p, size_t w, size_t h, size_t n, prefix *out, pc_error *err)
{
    size_t len, y, x, c;
    double *d;
    if (!pc_size_mul3(w + 1, h, n, &len) || len > PC_MAX_ALLOC / sizeof(double)) {
        return pc_fail(err, PC_ERR_LIMIT, "prefix sums too large");
    }
    d = pc_calloc_array(len > 0 ? len : 1, sizeof(double));
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for prefix sums");
    }
    for (y = 0; y < h; y++) {
        const float *src = p + y * w * n;
        double *dst = d + y * (w + 1) * n;
        for (x = 0; x < w; x++) {
            for (c = 0; c < n; c++) {
                dst[(x + 1) * n + c] = dst[x * n + c] + (double)src[x * n + c];
            }
        }
    }
    out->w = w;
    out->h = h;
    out->n = n;
    out->data = d;
    return PC_OK;
}

/* Kernel mean at window pixel (x, y) into acc (n values). */
static void prefix_mean(const prefix *p, const pc_algo_spans *spans, int64_t x, int64_t y, double *acc)
{
    size_t n = p->n, k, c;
    for (c = 0; c < n; c++) {
        acc[c] = 0.0;
    }
    for (k = 0; k < spans->n; k++) {
        int64_t yy = y + spans->rows[k][0], x0, x1;
        const double *row, *hi, *lo;
        if (yy < 0 || yy >= (int64_t)p->h) {
            continue;
        }
        x0 = x + spans->rows[k][1];
        x1 = x + spans->rows[k][2] + 1;
        x0 = x0<0 ? 0 : x0>(int64_t) p->w ? (int64_t)p->w : x0;
        x1 = x1<0 ? 0 : x1>(int64_t) p->w ? (int64_t)p->w : x1;
        row = p->data + (size_t)yy * (p->w + 1) * n;
        hi = row + (size_t)x1 * n;
        lo = row + (size_t)x0 * n;
        for (c = 0; c < n; c++) {
            acc[c] += hi[c] - lo[c];
        }
    }
    for (c = 0; c < n; c++) {
        acc[c] /= spans->count;
    }
}

/* ---- shapes ---- */

/* Point-in-polygon (even-odd). */
static bool in_poly(float u, float v, const float (*pts)[2], size_t n)
{
    bool inside = false;
    size_t i, j = n - 1;
    for (i = 0; i < n; i++) {
        float xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1];
        if ((yi > v) != (yj > v) && u < (xj - xi) * (v - yi) / (yj - yi) + xi) {
            inside = !inside;
        }
        j = i;
    }
    return inside;
}

/* Radius of a regular n-gon (circumradius 1) in direction theta. */
static float polygon_radius(float theta, uint32_t n, float rot)
{
    float seg = TAU_F / (float)n;
    float a = pc_algo_rem_euclidf(theta - rot, seg) - seg / 2.0f;
    return cosf(PI_F / (float)n) / cosf(a);
}

void pc_algo_shape_ctx_init(pc_algo_shape_ctx *k, pc_algo_blur_shape shape)
{
    int i;
    k->shape = shape;
    for (i = 0; i < 10; i++) {
        float r = i % 2 == 0 ? 1.0f : 0.42f;
        float a = -PI_F / 2.0f + (float)i * PI_F / 5.0f;
        k->star[i][0] = r * cosf(a);
        k->star[i][1] = r * sinf(a);
    }
}

bool pc_algo_shape_inside(const void *ictx, float u, float v)
{
    const pc_algo_shape_ctx *k = ictx;
    float d, x, y, q;
    switch (k->shape) {
    case PC_ALGO_SHAPE_CIRCLE:
        return u * u + v * v <= 1.0f;
    case PC_ALGO_SHAPE_RING:
        d = u * u + v * v;
        return d >= 0.36f && d <= 1.0f;
    case PC_ALGO_SHAPE_SQUARE:
        return true;
    case PC_ALGO_SHAPE_DIAMOND:
        return fabsf(u) + fabsf(v) <= 1.0f;
    case PC_ALGO_SHAPE_TRIANGLE:
        return v <= 0.75f && fabsf(u) <= (v + 1.0f) / 1.75f;
    case PC_ALGO_SHAPE_HEXAGON:
        return sqrtf(u * u + v * v) <= polygon_radius(atan2f(v, u), 6, 0.0f);
    case PC_ALGO_SHAPE_STAR:
        return in_poly(u, v, (const float (*)[2])k->star, 10);
    case PC_ALGO_SHAPE_HEART:
        /* (x^2 + y^2 - 1)^3 - x^2 y^3 <= 0, scaled into the unit square and flipped (y up). */
        x = u * 1.2f;
        y = -v * 1.2f + 0.15f;
        q = x * x + y * y - 1.0f;
        return q * q * q - x * x * y * y * y <= 0.0f;
    case PC_ALGO_SHAPE_CROSS:
        return fabsf(u) <= 0.3f || fabsf(v) <= 0.3f;
    }
    return false;
}

pc_status pc_algo_conv_spans(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                             const pc_algo_spans *spans, float *dst, pc_error *err)
{
    size_t n = src->ch, ww = pc_rect_width(src->rect), wh = pc_rect_height(src->rect), len, k = 0;
    float *p = NULL, px[PC_ALGO_MAXC];
    double acc[PC_ALGO_MAXC];
    prefix pre;
    pc_status st;
    int32_t x, y;
    if (n == 0 || n > PC_ALGO_MAXC) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported channel count %zu", n);
    }
    st = pc_algo_premul_window(src, src->rect, ctx->alpha, &p, &len, err);
    if (st != PC_OK) {
        return st;
    }
    st = prefix_new(p, ww, wh, n, &pre, err);
    free(p);
    if (st != PC_OK) {
        return st;
    }
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            size_t c;
            prefix_mean(&pre, spans, (int64_t)x - src->rect.x0, (int64_t)y - src->rect.y0, acc);
            for (c = 0; c < n; c++) {
                px[c] = (float)acc[c];
            }
            pc_algo_unpremul_px(px, n, ctx->alpha);
            memcpy(dst + k, px, n * sizeof(float));
            k += n;
        }
    }
    free(pre.data);
    return PC_OK;
}

/* ---- smart blur ---- */

pc_status pc_algo_smart_blur_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                               const pc_algo_smart_blur_params *p, float *dst, pc_error *err)
{
    static const int32_t nb[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), npx = (size_t)pc_rect_width(out) * pc_rect_height(out);
    size_t noffs = 0, i, c, q;
    float r = pc_clampf(p->radius, 0.1f, 100.0f), t = pc_clampf(p->threshold, 0.1f, 100.0f) / 255.0f;
    int32_t ri = pc_f32_to_i32(ceilf(r)), target, stride, dx, dy;
    int32_t (*offs)[2];
    float white[PC_ALGO_MAXC], black[PC_ALGO_MAXC], qv[PC_ALGO_MAXC] = {0}, acc[PC_ALGO_MAXC];
    const float wc[4] = {1.0f, 1.0f, 1.0f, 1.0f}, bc[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (n > PC_ALGO_MAXC) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported channel count %zu", n);
    }
    target = p->quality == PC_ALGO_BLURQ_LOW ? 4 : p->quality == PC_ALGO_BLURQ_MEDIUM ? 7 : 12;
    stride = (ri + target - 1) / target;
    stride = stride > 1 ? stride : 1;
    offs = pc_malloc_array((size_t)(2 * ri / stride + 1) * (size_t)(2 * ri / stride + 1) + 1, sizeof *offs);
    if (offs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Smart Blur");
    }
    for (dy = -ri; dy <= ri; dy += stride) {
        for (dx = -ri; dx <= ri; dx += stride) {
            if ((float)(dx * dx + dy * dy) <= r * r + 0.5f) {
                offs[noffs][0] = dx;
                offs[noffs][1] = dy;
                noffs++;
            }
        }
    }
    pc_algo_native(ctx, wc, white);
    pc_algo_native(ctx, bc, black);
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx && n > 0; i++) {
        float *px = dst + i * n, me[PC_ALGO_MAXC], wsum = 0.0f;
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        for (c = 0; c < PC_ALGO_MAXC; c++) {
            me[c] = c < n ? px[c] : 0.0f;
        }
        if (p->mode != PC_ALGO_SMART_NORMAL) {
            bool edge = false;
            float a = ctx->alpha ? px[n - 1] : 1.0f;
            for (q = 0; q < 4 && !edge; q++) {
                for (c = 0; c < n; c++) {
                    qv[c] = pc_algo_image_get(src, x + nb[q][0], y + nb[q][1], c);
                }
                for (c = 0; c < cc; c++) {
                    if (fabsf(qv[c] - me[c]) > t) {
                        edge = true;
                        break;
                    }
                }
            }
            if (edge) {
                memcpy(px, white, cc * sizeof(float));
            } else if (p->mode == PC_ALGO_SMART_EDGE_ONLY) {
                memcpy(px, black, cc * sizeof(float));
            }
            if (ctx->alpha) {
                px[n - 1] = a;
            }
            continue;
        }
        for (c = 0; c < n; c++) {
            acc[c] = 0.0f;
        }
        for (q = 0; q < noffs; q++) {
            bool differs = false;
            float a;
            for (c = 0; c < n; c++) {
                qv[c] = pc_algo_image_get(src, x + offs[q][0], y + offs[q][1], c);
            }
            for (c = 0; c < cc; c++) {
                if (fabsf(qv[c] - me[c]) > t) {
                    differs = true;
                    break;
                }
            }
            if (differs) {
                continue;
            }
            a = ctx->alpha ? qv[n - 1] : 1.0f;
            for (c = 0; c < n; c++) {
                acc[c] += ctx->alpha && c < n - 1 ? qv[c] * a : qv[c];
            }
            wsum += 1.0f;
        }
        if (wsum > 0.0f) {
            for (c = 0; c < n; c++) {
                px[c] = acc[c] / wsum;
            }
            pc_algo_unpremul_px(px, n, ctx->alpha);
        }
    }
    free(offs);
    return PC_OK;
}

/* ---- lens blur ---- */

/* Depth levels the per-pixel iris radius is interpolated between. */
#define LENS_LEVELS 8

typedef struct iris_ctx {
    uint32_t blades;
    float rot, curv;
} iris_ctx;

static bool iris_inside(const void *ictx, float u, float v)
{
    const iris_ctx *k = ictx;
    float rho = sqrtf(u * u + v * v);
    float pr = polygon_radius(atan2f(v, u), k->blades, k->rot - PI_F / 2.0f);
    return rho <= pr + (1.0f - pr) * k->curv;
}

pc_status pc_algo_lens_blur_k(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx,
                              const pc_algo_lens_blur_params *p, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), ww = pc_rect_width(src->rect),
           wh = pc_rect_height(src->rect);
    size_t len, i, k = 0, c;
    float rmax = pc_clampf(p->radius, 0.0f, 100.0f), boost, focal, amount;
    pc_algo_spans levels[LENS_LEVELS + 1];
    iris_ctx ic;
    float *pw = NULL;
    prefix pre = {0, 0, 0, NULL};
    pc_status st = PC_OK;
    int32_t x, y;
    memset(levels, 0, sizeof levels);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported channel count %zu", n);
    }
    ic.blades = p->blades < 3 ? 3 : p->blades > 8 ? 8 : p->blades;
    ic.curv = pc_clampf(p->curvature, 0.0f, 100.0f) / 100.0f;
    ic.rot = pc_algo_to_radians(p->rotation);
    for (i = 0; i <= LENS_LEVELS; i++) {
        st = pc_algo_spans_new(rmax * (float)i / (float)LENS_LEVELS, iris_inside, &ic, &levels[i], err);
        if (st != PC_OK) {
            goto done;
        }
    }
    /* Specular highlight boost before blurring. */
    st = pc_algo_premul_window(src, src->rect, ctx->alpha, &pw, &len, err);
    if (st != PC_OK) {
        goto done;
    }
    boost = pc_clampf(p->brightness, 0.0f, 100.0f) / 100.0f * 4.0f;
    if (boost > 0.0f) {
        float th = pc_clampf(p->threshold, 0.0f, 255.0f) / 255.0f;
        bool sub = pc_algo_subtractive(ctx);
        for (i = 0; i + n <= len; i += n) {
            float *px = pw + i, a = ctx->alpha ? px[n - 1] : 1.0f, stv[PC_ALGO_MAXC] = {0}, l, kk;
            if (a <= 0.0f) {
                continue;
            }
            for (c = 0; c < n; c++) {
                stv[c] = ctx->alpha && c < n - 1 ? px[c] / a : px[c];
            }
            l = pc_algo_luma(ctx, stv, n);
            kk = 1.0f + boost * pc_algo_smoothstep(th, th + 0.02f, l);
            for (c = 0; c < cc; c++) {
                px[c] = sub ? a - (a - px[c]) / kk : px[c] * kk;
            }
        }
    }
    st = prefix_new(pw, ww, wh, n, &pre, err);
    if (st != PC_OK) {
        goto done;
    }
    focal = pc_clampf(p->focal_distance, 0.0f, 255.0f) / 255.0f;
    amount = pc_clampf(p->noise, 0.0f, 100.0f) / 100.0f * 0.25f;
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            int64_t wx = (int64_t)x - src->rect.x0, wy = (int64_t)y - src->rect.y0;
            double acc[PC_ALGO_MAXC], acc2[PC_ALGO_MAXC];
            float px[PC_ALGO_MAXC], level, f;
            bool have_depth = false;
            float d = 0.0f;
            size_t lo;
            switch (p->depth) {
            case PC_ALGO_DEPTH_NONE:
                break;
            case PC_ALGO_DEPTH_TRANSPARENCY:
                if (ctx->alpha) {
                    d = pc_algo_image_get(src, x, y, n - 1);
                    have_depth = true;
                }
                break;
            case PC_ALGO_DEPTH_LAYER_MASK:
                if (p->depth_map != NULL) {
                    d = pc_algo_image_get(p->depth_map, x, y, 0);
                    have_depth = true;
                }
                break;
            }
            if (have_depth && p->invert_depth) {
                d = 1.0f - d;
            }
            level = have_depth ? pc_rs_minf_k(fabsf(d - focal) * (float)LENS_LEVELS, (float)LENS_LEVELS)
                               : (float)LENS_LEVELS;
            lo = pc_f32_to_size(floorf(level));
            if (lo > LENS_LEVELS) {
                lo = LENS_LEVELS;
            }
            f = level - (float)lo;
            prefix_mean(&pre, &levels[lo], wx, wy, acc);
            if (f > 1e-4f && lo < LENS_LEVELS) {
                prefix_mean(&pre, &levels[lo + 1], wx, wy, acc2);
                for (c = 0; c < n; c++) {
                    acc[c] += (acc2[c] - acc[c]) * (double)f;
                }
            }
            for (c = 0; c < n; c++) {
                px[c] = (float)acc[c];
            }
            pc_algo_unpremul_px(px, n, ctx->alpha);
            if (amount > 0.0f && (!ctx->alpha || px[n - 1] > 0.0f)) {
                for (c = 0; c < cc; c++) {
                    uint32_t ch = p->monochromatic ? 0u : (uint32_t)c;
                    if (p->distribution == PC_ALGO_DIST_UNIFORM) {
                        px[c] += (pc_algo_hash01(x, y, ch + 200u, p->seed) - 0.5f) * 2.0f * amount;
                    } else {
                        float u1 = pc_rs_maxf_k(pc_algo_hash01(x, y, ch * 2u + 301u, p->seed), 1e-7f);
                        float u2 = pc_algo_hash01(x, y, ch * 2u + 302u, p->seed);
                        px[c] += sqrtf(-2.0f * logf(u1)) * cosf(TAU_F * u2) * amount * 0.5f;
                    }
                }
            }
            memcpy(dst + k, px, n * sizeof(float));
            k += n;
        }
    }
done:
    for (i = 0; i <= LENS_LEVELS; i++) {
        pc_algo_spans_fini(&levels[i]);
    }
    free(pw);
    free(pre.data);
    return st;
}
