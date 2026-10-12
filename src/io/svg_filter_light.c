/* feDiffuseLighting / feSpecularLighting (port of resvg 0.45's filter/lighting.rs): surface
 * normals from the input's alpha (Sobel kernels with the edge variants), a distant, point or spot
 * light, f32 vector maths as resvg does it. */
#include <math.h>
#include <string.h>

#include "base/num.h"
#include "io/svg_filter.h"

typedef struct v3 {
    float x, y, z;
} v3;

typedef struct normal {
    float fx, fy; /* factor */
    float nx, ny; /* -normal */
} normal;

static float dot(v3 a, v3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float length(v3 a)
{
    return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

static v3 normalized_or_self(v3 v)
{
    float l = length(v);
    if (!sv_approx_eq_ulps(l, 0.0f, 4)) {
        v3 r = {v.x / l, v.y / l, v.z / l};
        return r;
    }
    return v;
}

static float to_radians(float deg)
{
    return deg * (3.14159265358979323846f / 180.0f);
}

typedef struct lctx {
    const sv_fprim *fe;
    sv_light light;
    const uint8_t *src;
    uint8_t *dst;
    uint32_t w, h;
    v3 lv;
    bool specular;
} lctx;

static int16_t alpha_at(const lctx *c, uint32_t x, uint32_t y)
{
    return c->src[((size_t)y * c->w + x) * 4 + 3];
}

static normal mk_normal(float fx, float fy, int nx, int ny)
{
    normal n;
    n.fx = fx;
    n.fy = fy;
    n.nx = (float)-nx;
    n.ny = (float)-ny;
    return n;
}

static float light_factor(const lctx *c, normal n, v3 lv)
{
    const sv_fprim *fe = c->fe;
    bool zero = sv_approx_eq_ulps(n.nx, 0.0f, 4) && sv_approx_eq_ulps(n.ny, 0.0f, 4);
    v3 nv;
    if (!zero) {
        float s = fe->surface_scale / 255.0f;
        nv.x = n.nx * s * n.fx;
        nv.y = n.ny * s * n.fy;
        nv.z = 1.0f;
    }
    if (!c->specular) {
        float k = zero ? lv.z : dot(nv, lv) / length(nv);
        return fe->constant * k;
    } else {
        v3 hv = {lv.x + 0.0f, lv.y + 0.0f, lv.z + 1.0f};
        float hl = length(hv);
        float ndh;
        float k;
        if (sv_approx_eq_ulps(hl, 0.0f, 4)) {
            return 0.0f;
        }
        ndh = zero ? hv.z / hl : dot(nv, hv) / length(nv) / hl;
        k = sv_approx_eq_ulps(fe->exponent, 1.0f, 4) ? ndh : powf(ndh, fe->exponent);
        return fe->constant * k;
    }
}

static uint8_t compute(uint8_t x, float factor)
{
    return pc_f32_to_u8(sv_f32_bound(0.0f, (float)x * factor, 255.0f) + 0.5f);
}

/* light_color: the colour reaching the surface along lv. */
static void light_color(const lctx *c, v3 lv, uint8_t out[3])
{
    const sv_light *l = &c->light;
    out[0] = c->fe->r;
    out[1] = c->fe->g;
    out[2] = c->fe->b;
    if (l->kind == LIGHT_SPOT) {
        v3 origin = {l->x, l->y, l->z};
        v3 dir = {l->px - origin.x, l->py - origin.y, l->pz - origin.z};
        float mls;
        float factor;
        dir = normalized_or_self(dir);
        mls = -dot(lv, dir);
        if (mls <= 0.0f || (l->has_cone && mls < cosf(to_radians(l->cone)))) {
            out[0] = out[1] = out[2] = 0;
            return;
        }
        factor = powf(mls, l->spec_exp);
        out[0] = compute(c->fe->r, factor);
        out[1] = compute(c->fe->g, factor);
        out[2] = compute(c->fe->b, factor);
    }
}

static void calc(lctx *c, uint32_t nx, uint32_t ny, normal n)
{
    uint8_t col[3];
    float factor;
    uint8_t *d;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    if (c->light.kind != LIGHT_DISTANT) {
        float nz = (float)alpha_at(c, nx, ny) / 255.0f * c->fe->surface_scale;
        v3 v = {c->light.x - (float)nx, c->light.y - (float)ny, c->light.z - nz};
        c->lv = normalized_or_self(v);
    }
    light_color(c, c->lv, col);
    factor = light_factor(c, n, c->lv);
    r = compute(col[0], factor);
    g = compute(col[1], factor);
    b = compute(col[2], factor);
    d = c->dst + ((size_t)ny * c->w + nx) * 4;
    d[0] = r;
    d[1] = g;
    d[2] = b;
    d[3] = c->specular ? (r > g ? (r > b ? r : b) : (g > b ? g : b)) : 255;
}

#define A(x, y) ((int)alpha_at(c, (x), (y)))

void sv_fx_lighting(const sv_fprim *fe, sv_light light, const uint8_t *src, uint8_t *dst, uint32_t w,
                    uint32_t h)
{
    const float F12 = 1.0f / 2.0f;
    const float F13 = 1.0f / 3.0f;
    const float F14 = 1.0f / 4.0f;
    const float F23 = 2.0f / 3.0f;
    lctx ctx;
    lctx *c = &ctx;
    uint32_t x;
    uint32_t y;
    if (w < 3 || h < 3) {
        return;
    }
    memset(c, 0, sizeof *c);
    c->fe = fe;
    c->light = light;
    c->src = src;
    c->dst = dst;
    c->w = w;
    c->h = h;
    c->specular = fe->kind == FK_SPECULAR_LIGHTING;
    c->lv.x = 1.0f;
    c->lv.y = 1.0f;
    c->lv.z = 1.0f;
    if (light.kind == LIGHT_DISTANT) {
        float az = to_radians(light.azimuth);
        float el = to_radians(light.elevation);
        c->lv.x = cosf(az) * cosf(el);
        c->lv.y = sinf(az) * cosf(el);
        c->lv.z = sinf(el);
    }
    /* corners */
    calc(c, 0, 0,
         mk_normal(F23, F23, -2 * A(0, 0) + 2 * A(1, 0) - A(0, 1) + A(1, 1),
                   -2 * A(0, 0) - A(1, 0) + 2 * A(0, 1) + A(1, 1)));
    calc(c, w - 1, 0,
         mk_normal(F23, F23, -2 * A(w - 2, 0) + 2 * A(w - 1, 0) - A(w - 2, 1) + A(w - 1, 1),
                   -A(w - 2, 0) - 2 * A(w - 1, 0) + A(w - 2, 1) + 2 * A(w - 1, 1)));
    calc(c, 0, h - 1,
         mk_normal(F23, F23, -A(0, h - 2) + A(1, h - 2) - 2 * A(0, h - 1) + 2 * A(1, h - 1),
                   -2 * A(0, h - 2) - A(1, h - 2) + 2 * A(0, h - 1) + A(1, h - 1)));
    calc(c, w - 1, h - 1,
         mk_normal(F23, F23, -A(w - 2, h - 2) + A(w - 1, h - 2) - 2 * A(w - 2, h - 1) + 2 * A(w - 1, h - 1),
                   -A(w - 2, h - 2) - 2 * A(w - 1, h - 2) + A(w - 2, h - 1) + 2 * A(w - 1, h - 1)));
    for (x = 1; x < w - 1; x++) {
        calc(c, x, 0,
             mk_normal(F13, F12, -2 * A(x - 1, 0) + 2 * A(x + 1, 0) - A(x - 1, 1) + A(x + 1, 1),
                       -A(x - 1, 0) - 2 * A(x, 0) - A(x + 1, 0) + A(x - 1, 1) + 2 * A(x, 1) + A(x + 1, 1)));
        calc(c, x, h - 1,
             mk_normal(F13, F12,
                       -A(x - 1, h - 2) + A(x + 1, h - 2) - 2 * A(x - 1, h - 1) + 2 * A(x + 1, h - 1),
                       -A(x - 1, h - 2) - 2 * A(x, h - 2) - A(x + 1, h - 2) + A(x - 1, h - 1) +
                           2 * A(x, h - 1) + A(x + 1, h - 1)));
    }
    for (y = 1; y < h - 1; y++) {
        calc(c, 0, y,
             mk_normal(F12, F13,
                       -A(0, y - 1) + A(1, y - 1) - 2 * A(0, y) + 2 * A(1, y) - A(0, y + 1) + A(1, y + 1),
                       -2 * A(0, y - 1) - A(1, y - 1) + 2 * A(0, y + 1) + A(1, y + 1)));
        calc(c, w - 1, y,
             mk_normal(F12, F13,
                       -A(w - 2, y - 1) + A(w - 1, y - 1) - 2 * A(w - 2, y) + 2 * A(w - 1, y) -
                           A(w - 2, y + 1) + A(w - 1, y + 1),
                       -A(w - 2, y - 1) - 2 * A(w - 1, y - 1) + A(w - 2, y + 1) + 2 * A(w - 1, y + 1)));
    }
    for (y = 1; y < h - 1; y++) {
        for (x = 1; x < w - 1; x++) {
            calc(c, x, y,
                 mk_normal(F14, F14,
                           -A(x - 1, y - 1) + A(x + 1, y - 1) - 2 * A(x - 1, y) + 2 * A(x + 1, y) -
                               A(x - 1, y + 1) + A(x + 1, y + 1),
                           -A(x - 1, y - 1) - 2 * A(x, y - 1) - A(x + 1, y - 1) + A(x - 1, y + 1) +
                               2 * A(x, y + 1) + A(x + 1, y + 1)));
        }
    }
}
