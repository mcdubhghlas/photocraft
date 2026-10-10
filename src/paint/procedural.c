/* Procedural patterns and tips (procedural.h). Float expressions keep the reference's operation
 * order and f32 precision; `powi(2)` / `powi(4)` on variables are written as the products LLVM
 * emits for them. */
#include "paint/procedural.h"

#include <math.h>
#include <stdlib.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"
#include "paint/rng.h"

#define PI_F 3.14159265358979323846f
#define TAU_F 6.28318530717958647692f

static float smooth(float t)
{
    return t * t * (3.0f - 2.0f * t);
}

static float fract(float x)
{
    return x - truncf(x);
}

static int32_t rem_euclid_i32(int32_t a, int32_t m)
{
    int32_t r = a % m;
    return r < 0 ? r + m : r;
}

/* Tileable value noise with `period` lattice cells over the tile. */
static float value_noise(float x, float y, int32_t period, uint64_t salt)
{
    float x0 = floorf(x), y0 = floorf(y);
    float tx = smooth(x - x0), ty = smooth(y - y0);
    int32_t xi = pc_f32_to_i32(x0), yi = pc_f32_to_i32(y0);
    int32_t xj = pc_i32_wrapping_add(xi, 1), yj = pc_i32_wrapping_add(yi, 1);
    float h00 = pc_paint_hash2(rem_euclid_i32(xi, period), rem_euclid_i32(yi, period), salt);
    float h10 = pc_paint_hash2(rem_euclid_i32(xj, period), rem_euclid_i32(yi, period), salt);
    float h01 = pc_paint_hash2(rem_euclid_i32(xi, period), rem_euclid_i32(yj, period), salt);
    float h11 = pc_paint_hash2(rem_euclid_i32(xj, period), rem_euclid_i32(yj, period), salt);
    float a = h00 + (h10 - h00) * tx;
    float b = h01 + (h11 - h01) * tx;
    return a + (b - a) * ty;
}

/* Tileable fractal noise in 0..1 (`cells` lattice cells across at the base octave). */
static float fbm(float x, float y, float size, int32_t cells, uint32_t octaves, uint64_t salt)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    int32_t c = cells;
    for (uint32_t o = 0; o < octaves; o++) {
        sum += value_noise(x / size * (float)c, y / size * (float)c, c, salt + (uint64_t)o * 7919u) * amp;
        norm += amp;
        amp *= 0.5f;
        c *= 2;
    }
    return sum / norm;
}

pc_status pc_paint_pattern_generate(pc_paint_pattern_style style, uint32_t size, uint32_t seed,
                                    pc_paint_pattern_image **out, pc_error *err)
{
    size_t n = size < 8 ? 8 : (size > 1024 ? 1024 : size);
    float s = (float)n;
    uint64_t salt = (uint64_t)seed * 0x0100000001B3ull + 17u; /* wraps like the release build */
    pc_paint_pattern_image *p = NULL;
    pc_status st = pc_paint_pattern_image_new(n, n, &p, err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t y = 0; y < n; y++) {
        for (size_t x = 0; x < n; x++) {
            float fx = (float)x + 0.5f, fy = (float)y + 0.5f, v;
            int32_t xi = (int32_t)x, yi = (int32_t)y;
            switch (style) {
            case PC_PAINT_STYLE_NOISE:
                v = fbm(fx, fy, s, 8, 4, salt);
                break;
            case PC_PAINT_STYLE_PAPER: {
                float base = fbm(fx, fy, s, 16, 4, salt);
                float grain = pc_paint_hash2(xi, yi, salt ^ 0x55u);
                v = pc_clampf((base - 0.5f) * 2.2f + 0.5f + (grain - 0.5f) * 0.35f, 0.0f, 1.0f);
                break;
            }
            case PC_PAINT_STYLE_CANVAS: {
                /* Over/under weave: threads every 8 px, alternating which direction is on top. */
                const float period = 8.0f;
                int32_t cx = pc_f32_to_i32(floorf(fx / period)), cy = pc_f32_to_i32(floorf(fy / period));
                float u = fract(fx / period), w = fract(fy / period);
                float warp = sinf(PI_F * u), weft = sinf(PI_F * w);
                float top = (cx + cy) % 2 == 0 ? warp : weft;
                v = pc_clampf(0.25f + 0.65f * top + (pc_paint_hash2(xi, yi, salt) - 0.5f) * 0.15f, 0.0f,
                              1.0f);
                break;
            }
            default: { /* PC_PAINT_STYLE_DOTS */
                const float period = 8.0f;
                float u = fract(fx / period) - 0.5f, w = fract(fy / period) - 0.5f;
                float d = sqrtf(u * u + w * w);
                v = 1.0f - smooth(pc_clampf((0.35f - d) / 0.1f + 0.5f, 0.0f, 1.0f));
                break;
            }
            }
            p->data[y * n + x] = v;
        }
    }
    *out = p;
    return PC_OK;
}

/* ---- tips ---- */

typedef struct tipctx {
    float c, s;
    uint64_t salt;
    uint32_t n;
    const float *items; /* per-item parameters for spatter / grass / rake */
    uint32_t count;
} tipctx;

static float chalk_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float dx = (float)x + 0.5f - t->c, dy = (float)y + 0.5f - t->c;
    float r = sqrtf(dx * dx + dy * dy) / t->c;
    float ang = atan2f(dy, dx);
    float edge = 0.8f + 0.15f * value_noise((ang + PI_F) * 3.0f, 0.5f, 19, t->salt);
    float inside = pc_clampf((edge - r) * 8.0f, 0.0f, 1.0f);
    float grain = pc_paint_hash2(pc_u32_as_i32(x), pc_u32_as_i32(y), t->salt);
    return inside * (grain < 0.2f ? 0.2f : 0.7f + 0.3f * grain);
}

pc_status pc_paint_chalk_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err)
{
    tipctx t = {(float)n / 2.0f, 0.0f, (uint64_t)seed + 101u, n, NULL, 0};
    return pc_paint_gray_tile_from_fn(n, n, chalk_fn, &t, out, err);
}

/* Allocates count * per floats for the per-item tables. */
static pc_status items_alloc(uint32_t count, size_t per, float **out, pc_error *err)
{
    float *v = pc_malloc_array(count > 0 ? count : 1, per * sizeof(float));
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a brush tip");
    }
    *out = v;
    return PC_OK;
}

static float spatter_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float px = (float)x + 0.5f, py = (float)y + 0.5f, acc = 0.0f;
    for (uint32_t i = 0; i < t->count; i++) {
        const float *b = &t->items[i * 3];
        float ex = px - b[0], ey = py - b[1];
        float d = sqrtf(ex * ex + ey * ey);
        acc = pc_rs_maxf(acc, pc_clampf(b[2] + 0.5f - d, 0.0f, 1.0f));
    }
    return acc;
}

pc_status pc_paint_spatter_tip(uint32_t n, uint32_t seed, uint32_t drops, pc_paint_gray_tile *out,
                               pc_error *err)
{
    tipctx t = {0.0f, (float)n, (uint64_t)seed + 202u, n, NULL, drops};
    float *blobs = NULL;
    pc_status st = items_alloc(drops, 3, &blobs, err);
    if (st != PC_OK) {
        return st;
    }
    for (uint32_t i = 0; i < drops; i++) {
        int32_t ii = pc_u32_as_i32(i);
        float a = pc_paint_hash2(ii, 0, t.salt) * TAU_F;
        float d = sqrtf(pc_paint_hash2(ii, 1, t.salt)) * 0.38f * t.s;
        float h2 = pc_paint_hash2(ii, 2, t.salt);
        float r = (0.03f + 0.09f * (h2 * h2)) * t.s;
        blobs[i * 3] = t.s / 2.0f + cosf(a) * d;
        blobs[i * 3 + 1] = t.s / 2.0f + sinf(a) * d;
        blobs[i * 3 + 2] = r;
    }
    t.items = blobs;
    st = pc_paint_gray_tile_from_fn(n, n, spatter_fn, &t, out, err);
    free(blobs);
    return st;
}

static float bristle_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float dx = ((float)x + 0.5f - t->c) / t->c, dy = ((float)y + 0.5f - t->c) / t->c;
    float dy2 = dy * 1.8f, r = sqrtf(dx * dx + dy2 * dy2), r2, hair, density;
    if (r > 1.0f) {
        return 0.0f;
    }
    hair = pc_paint_hash2(0, pc_f32_to_i32((float)y / 1.5f), t->salt);
    density = hair < 0.25f ? 0.0f : 0.5f + 0.5f * hair;
    r2 = r * r;
    return density * (1.0f - r2 * r2);
}

pc_status pc_paint_bristle_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err)
{
    tipctx t = {(float)n / 2.0f, 0.0f, (uint64_t)seed + 303u, n, NULL, 0};
    return pc_paint_gray_tile_from_fn(n, n, bristle_fn, &t, out, err);
}

static float charcoal_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float dx = ((float)x + 0.5f - t->c) / t->c, dy = ((float)y + 0.5f - t->c) / t->c;
    float dy2 = dy * 1.6f, r = sqrtf(dx * dx + dy2 * dy2);
    float edge = 0.85f + 0.15f * value_noise(dx * 6.0f + 9.0f, dy * 6.0f + 9.0f, 64, t->salt);
    float row, along, grit, v;
    if (r > edge) {
        return 0.0f;
    }
    /* Streaks: per-row density modulated along x so they break up. */
    row = pc_paint_hash2(0, pc_u32_as_i32(y), t->salt);
    along = value_noise((float)x / 6.0f, (float)y / 1.5f, 1 << 12, t->salt ^ 0x77u);
    grit = pc_paint_hash2(pc_u32_as_i32(x), pc_u32_as_i32(y), t->salt ^ 0x13u);
    v = (0.35f + 0.65f * row) * (0.4f + 0.6f * along) * (grit < 0.15f ? 0.2f : 1.0f);
    return v * pc_clampf((edge - r) * 6.0f, 0.0f, 1.0f);
}

pc_status pc_paint_charcoal_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err)
{
    tipctx t = {(float)n / 2.0f, 0.0f, (uint64_t)seed + 404u, n, NULL, 0};
    return pc_paint_gray_tile_from_fn(n, n, charcoal_fn, &t, out, err);
}

static float leaf_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float c = t->c;
    float u = ((float)x + 0.5f - c) / c, v = ((float)y + 0.5f - c) / c;
    /* Half-width of the blade at u: widest a little behind the middle, pointed at both ends. */
    float tt = pc_clampf((u + 1.0f) / 2.0f, 0.0f, 1.0f);
    float half = 0.42f * sinf(PI_F * tt) * (1.0f - 0.25f * tt);
    float inside = pc_clampf((half - fabsf(v)) * c, 0.0f, 1.0f);
    float rib = 1.0f - 0.45f * (1.0f - pc_clampf(fabsf(v) * c / 1.2f, 0.0f, 1.0f));
    float veins =
        1.0f - 0.2f * (1.0f - pc_clampf(fabsf(fract(u * 7.0f - fabsf(v) * 5.0f) - 0.5f) * 8.0f, 0.0f, 1.0f));
    float stem = (u < -0.75f && fabsf(v) * c < 0.9f) ? 1.0f : 0.0f;
    return pc_rs_maxf(inside * rib * veins, stem);
}

pc_status pc_paint_leaf_tip(uint32_t n, pc_paint_gray_tile *out, pc_error *err)
{
    tipctx t = {(float)n / 2.0f, 0.0f, 0, n, NULL, 0};
    return pc_paint_gray_tile_from_fn(n, n, leaf_fn, &t, out, err);
}

static float grass_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float px = (float)x + 0.5f, py = (float)y + 0.5f, up = t->s - py, acc = 0.0f;
    for (uint32_t i = 0; i < t->count; i++) {
        const float *b = &t->items[i * 4];
        float base = b[0], height = b[1], lean = b[2], width = b[3], tt, cx, w;
        if (up < 0.0f || up > height) {
            continue;
        }
        tt = up / height;
        cx = base + lean * height * tt * tt;
        w = width * (1.0f - tt) + 0.3f;
        acc = pc_rs_maxf(acc, pc_clampf(w - fabsf(px - cx), 0.0f, 1.0f));
    }
    return acc;
}

pc_status pc_paint_grass_tip(uint32_t n, uint32_t seed, uint32_t blades, pc_paint_gray_tile *out,
                             pc_error *err)
{
    tipctx t = {0.0f, (float)n, (uint64_t)seed + 505u, n, NULL, blades};
    float *bl = NULL;
    pc_status st = items_alloc(blades, 4, &bl, err);
    if (st != PC_OK) {
        return st;
    }
    for (uint32_t i = 0; i < blades; i++) {
        int32_t ii = pc_u32_as_i32(i);
        bl[i * 4] = (0.3f + 0.4f * pc_paint_hash2(ii, 0, t.salt)) * t.s;
        bl[i * 4 + 1] = (0.55f + 0.42f * pc_paint_hash2(ii, 1, t.salt)) * t.s;
        bl[i * 4 + 2] = (pc_paint_hash2(ii, 2, t.salt) - 0.5f) * 0.9f;
        bl[i * 4 + 3] = (0.025f + 0.025f * pc_paint_hash2(ii, 3, t.salt)) * t.s;
    }
    t.items = bl;
    st = pc_paint_gray_tile_from_fn(n, n, grass_fn, &t, out, err);
    free(bl);
    return st;
}

static float sponge_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float dx = (float)x + 0.5f - t->c, dy = (float)y + 0.5f - t->c;
    float r = sqrtf(dx * dx + dy * dy) / t->c;
    float edge = 0.78f + 0.2f * value_noise((atan2f(dy, dx) + PI_F) * 2.5f, 0.5f, 16, t->salt);
    float disc = pc_clampf((edge - r) * 6.0f, 0.0f, 1.0f);
    float pores = fbm((float)x, (float)y, (float)t->n, 6, 3, t->salt);
    return disc * pc_clampf((pores - 0.42f) * 6.0f, 0.0f, 1.0f);
}

pc_status pc_paint_sponge_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err)
{
    tipctx t = {(float)n / 2.0f, 0.0f, (uint64_t)seed + 606u, n, NULL, 0};
    return pc_paint_gray_tile_from_fn(n, n, sponge_fn, &t, out, err);
}

static float ray(float a, float b)
{
    return pc_paint_maxf_k((1.0f - a) * (1.0f - pc_paint_minf_k(b / (0.08f * (1.0f - a) + 0.005f), 1.0f)),
                           0.0f);
}

static float star_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float u = fabsf(((float)x + 0.5f - t->c) / t->c), v = fabsf(((float)y + 0.5f - t->c) / t->c);
    float g = pc_paint_maxf_k(1.0f - sqrtf(u * u + v * v) * 2.5f, 0.0f);
    float glow = g * g;
    return pc_paint_minf_k(pc_rs_maxf(pc_rs_maxf(ray(u, v), ray(v, u)), glow), 1.0f);
}

pc_status pc_paint_star_tip(uint32_t n, pc_paint_gray_tile *out, pc_error *err)
{
    tipctx t = {(float)n / 2.0f, 0.0f, 0, n, NULL, 0};
    return pc_paint_gray_tile_from_fn(n, n, star_fn, &t, out, err);
}

static float rake_fn(void *vp, uint32_t x, uint32_t y)
{
    const tipctx *t = vp;
    float px = (float)x + 0.5f, py = (float)y + 0.5f, acc = 0.0f;
    for (uint32_t i = 0; i < t->count; i++) {
        const float *b = &t->items[i * 3];
        float ex = px - b[0], ey = py - b[1];
        float d = sqrtf(ex * ex + ey * ey);
        acc = pc_rs_maxf(acc, pc_clampf(b[2] + 0.5f - d, 0.0f, 1.0f) * 0.9f);
    }
    return acc;
}

pc_status pc_paint_rake_tip(uint32_t n, uint32_t seed, uint32_t bristles, pc_paint_gray_tile *out,
                            pc_error *err)
{
    uint32_t k = bristles > 1 ? bristles : 1;
    tipctx t = {0.0f, (float)n, (uint64_t)seed + 707u, n, NULL, k};
    float *dots = NULL;
    pc_status st = items_alloc(k, 3, &dots, err);
    if (st != PC_OK) {
        return st;
    }
    for (uint32_t i = 0; i < k; i++) {
        int32_t ii = pc_u32_as_i32(i);
        dots[i * 3] = t.s * ((float)i + 0.5f) / (float)k;
        dots[i * 3 + 1] = t.s / 2.0f + (pc_paint_hash2(ii, 0, t.salt) - 0.5f) * t.s * 0.08f;
        dots[i * 3 + 2] = t.s / (float)k * (0.22f + 0.18f * pc_paint_hash2(ii, 1, t.salt));
    }
    t.items = dots;
    st = pc_paint_gray_tile_from_fn(n, n, rake_fn, &t, out, err);
    free(dots);
    return st;
}
