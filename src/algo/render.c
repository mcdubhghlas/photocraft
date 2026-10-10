/* Render: Fibers, Lens Flare, Lighting Effects (port of crates/algo/src/render.rs). */
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"

/* Smooth value noise in [0, 1). */
static float value_noise(float x, float y, uint32_t seed)
{
    float fx0 = floorf(x), fy0 = floorf(y);
    int32_t ix = pc_f32_to_i32(fx0), iy = pc_f32_to_i32(fy0);
    float tx = x - (float)ix, ty = y - (float)iy;
    float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    int32_t ix1 = pc_i32_wrapping_add(ix, 1), iy1 = pc_i32_wrapping_add(iy, 1);
    float h00 = pc_algo_hash01(ix, iy, 0, seed), h10 = pc_algo_hash01(ix1, iy, 0, seed);
    float h01 = pc_algo_hash01(ix, iy1, 0, seed), h11 = pc_algo_hash01(ix1, iy1, 0, seed);
    float top = h00 + (h10 - h00) * sx, bot = h01 + (h11 - h01) * sx;
    return top + (bot - top) * sy;
}

/* Vertical fibrous streaks between the foreground and background colours. Variance sets how
 * quickly the colour varies across fibres; strength stretches each fibre vertically. */
static pc_status fibers_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_fibers_params *fp = p;
    size_t n = src->ch, i;
    float var = pc_clampf(fp->variance, 1.0f, 64.0f), st = pc_clampf(fp->strength, 1.0f, 64.0f);
    /* Horizontal frequency rises with variance; vertical frequency falls with strength. */
    float fx = 0.08f + var / 64.0f * 0.6f;
    float fy = fx / (2.0f + st * 1.5f) * (0.5f + var / 32.0f);
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < pc_fx_npx(out); i++) {
        int32_t x, y;
        float xf, yf, sum = 0.0f, amp = 1.0f, norm = 0.0f, ax = fx, ay = fy, grain, t;
        uint32_t o;
        pc_algo_xy(out, i, &x, &y);
        xf = (float)x;
        yf = (float)y;
        for (o = 0; o < 4; o++) {
            sum += value_noise(xf * ax, yf * ay, fp->seed + o * 977u) * amp;
            norm += amp;
            amp *= 0.55f;
            ax *= 2.0f;
            ay *= 2.0f;
        }
        /* Fine per-column grain gives the hairline texture. */
        grain = value_noise(xf * 0.9f, yf * fy * 0.25f, fp->seed ^ 0x5bd1u) - 0.5f;
        t = pc_clampf((sum / norm - 0.5f) * (1.6f + var / 40.0f) + 0.5f + grain * 0.25f, 0.0f, 1.0f);
        {
            const float *fg = fp->foreground, *bg = fp->background;
            const float c[4] = {fg[0] + (bg[0] - fg[0]) * t, fg[1] + (bg[1] - fg[1]) * t,
                                fg[2] + (bg[2] - fg[2]) * t, 1.0f};
            pc_algo_set_rgba(ctx, dst + i * n, n, c);
        }
    }
    return PC_OK;
}

/* One lens-flare ghost: position along the flare->centre axis, radius, tint and strength. */
typedef struct ghost {
    float t, r, tint[3], k;
} ghost;

static const ghost GH_ZOOM[] = {
    {0.35f, 10.0f, {0.6f, 0.8f, 1.0f}, 0.18f}, {0.6f, 26.0f, {0.5f, 1.0f, 0.6f}, 0.10f},
    {0.85f, 6.0f, {1.0f, 0.7f, 0.4f}, 0.25f},  {1.25f, 40.0f, {0.7f, 0.5f, 1.0f}, 0.08f},
    {1.5f, 16.0f, {1.0f, 0.6f, 0.5f}, 0.15f},  {1.9f, 70.0f, {0.5f, 0.7f, 1.0f}, 0.06f},
};
static const ghost GH_35[] = {{0.7f, 18.0f, {0.6f, 1.0f, 0.7f}, 0.12f},
                              {1.4f, 30.0f, {1.0f, 0.7f, 0.5f}, 0.09f}};
static const ghost GH_105[] = {{0.5f, 14.0f, {1.0f, 0.8f, 0.5f}, 0.12f},
                               {1.6f, 50.0f, {0.6f, 0.7f, 1.0f}, 0.07f}};
static const ghost GH_MOVIE[] = {
    {0.4f, 8.0f, {0.5f, 0.7f, 1.0f}, 0.2f},
    {1.0f, 20.0f, {0.4f, 0.6f, 1.0f}, 0.12f},
    {1.6f, 12.0f, {0.5f, 0.8f, 1.0f}, 0.16f},
};

/* x.powi(8) and x.powi(12) as LLVM expands them (binary exponentiation). */
static float powi8(float x)
{
    float x2 = x * x, x4 = x2 * x2;
    return x4 * x4;
}

static float powi12(float x)
{
    float x2 = x * x, x4 = x2 * x2, x8 = x4 * x4;
    return x4 * x8;
}

/* An additive (screen) glow, rays, halo ring and ghost discs along the line through the image
 * centre; lens types vary the elements. */
static pc_status flare_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_flare_params *fp = p;
    size_t n = src->ch, i, k, g, ng;
    pc_rect b = ctx->bounds;
    float bw = (float)pc_rect_width(b), bh = (float)pc_rect_height(b);
    float diag = pc_rs_maxf_k(hypotf(bw, bh), 1.0f);
    float cx = (float)b.x0 + fp->center_x * bw, cy = (float)b.y0 + fp->center_y * bh;
    float mx = (float)b.x0 + bw / 2.0f, my = (float)b.y0 + bh / 2.0f;
    float bright = pc_clampf(fp->brightness, 10.0f, 300.0f) / 100.0f, s = diag / 1000.0f;
    float core, glow, ring_r, ring_w, rays, streak;
    const ghost *ghosts;
    (void)prep, (void)err;
    switch (fp->lens) {
    case PC_ALGO_LENS_PRIME35:
        core = 12.0f, glow = 70.0f, ring_r = 90.0f, ring_w = 6.0f, rays = 0.3f, streak = 0.0f;
        ghosts = GH_35, ng = PC_FX_N(GH_35);
        break;
    case PC_ALGO_LENS_PRIME105:
        core = 22.0f, glow = 130.0f, ring_r = 0.0f, ring_w = 0.0f, rays = 0.8f, streak = 0.0f;
        ghosts = GH_105, ng = PC_FX_N(GH_105);
        break;
    case PC_ALGO_LENS_MOVIE_PRIME:
        core = 10.0f, glow = 60.0f, ring_r = 0.0f, ring_w = 0.0f, rays = 0.2f, streak = 1.0f;
        ghosts = GH_MOVIE, ng = PC_FX_N(GH_MOVIE);
        break;
    default:
        core = 14.0f, glow = 90.0f, ring_r = 0.0f, ring_w = 0.0f, rays = 0.5f, streak = 0.0f;
        ghosts = GH_ZOOM, ng = PC_FX_N(GH_ZOOM);
        break;
    }
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy, dx, dy, d, f[3] = {0.0f, 0.0f, 0.0f}, gl, c[4];
        const float warm[3] = {1.0f, 0.95f, 0.85f};
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        fx = (float)x + 0.5f;
        fy = (float)y + 0.5f;
        dx = fx - cx;
        dy = fy - cy;
        d = sqrtf(dx * dx + dy * dy);
        /* Hot core and wide glow (warm white). */
        gl = expf(-pc_fx_sq(d / (core * s))) + 0.35f * expf(-d / (glow * s));
        for (k = 0; k < 3; k++) {
            f[k] += gl * warm[k];
        }
        /* Rays: angular modulation decaying with distance. */
        if (rays > 0.0f) {
            float th = atan2f(dy, dx);
            float m =
                powi8(sinf(th * 12.0f) * 0.5f + 0.5f) + powi12(sinf(th * 7.0f + 1.3f) * 0.5f + 0.5f) * 0.6f;
            float rr = rays * m * expf(-d / (glow * 1.6f * s));
            for (k = 0; k < 3; k++) {
                f[k] += rr * 0.6f;
            }
        }
        /* Anamorphic horizontal streak (movie prime). */
        if (streak > 0.0f) {
            float st = expf(-(fabsf(dy) / (2.5f * s))) * expf(-(fabsf(dx) / (450.0f * s)));
            f[0] += st * 0.5f;
            f[1] += st * 0.7f;
            f[2] += st;
        }
        /* Halo ring with a slight chromatic spread. */
        if (ring_r > 0.0f) {
            const float offs[3] = {1.04f, 1.0f, 0.96f};
            for (k = 0; k < 3; k++) {
                float rd = (d - ring_r * s * offs[k]) / (ring_w * s);
                f[k] += 0.18f * expf(-rd * rd);
            }
        }
        /* Ghosts along the axis through the centre. */
        for (g = 0; g < ng; g++) {
            const ghost *gh = &ghosts[g];
            float gx = cx + (mx - cx) * gh->t * 2.0f, gy = cy + (my - cy) * gh->t * 2.0f;
            float gd = sqrtf(pc_fx_sq(fx - gx) + pc_fx_sq(fy - gy)) / (gh->r * s);
            if (gd < 1.2f) {
                /* Soft-edged disc, slightly brighter at the rim. */
                float e = pc_algo_smoothstep(1.0f, 0.85f, gd) * (0.6f + 0.4f * gd);
                for (k = 0; k < 3; k++) {
                    f[k] += e * gh->k * gh->tint[k];
                }
            }
        }
        pc_algo_rgba(ctx, px, n, c);
        for (k = 0; k < 3; k++) {
            float a = pc_rs_maxf_k(f[k] * bright, 0.0f);
            /* Screen keeps highlights below white for display-referred values. */
            c[k] = 1.0f - (1.0f - c[k]) * (1.0f - pc_rs_minf_k(a, 1.0f));
        }
        pc_algo_set_rgba(ctx, px, n, c);
    }
    return PC_OK;
}

static void norm3(const float v[3], float out[3])
{
    float m = pc_rs_maxf_k(sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-9f);
    out[0] = v[0] / m, out[1] = v[1] / m, out[2] = v[2] / m;
}

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Bump height of the texture channel at (x, y) (clamped to the bounds). */
static float tex_at(const pc_algo_lighting_params *lp, const pc_algo_image *src, const pc_algo_ctx *ctx,
                    int32_t x, int32_t y)
{
    pc_rect b = ctx->bounds;
    size_t n = src->ch, c;
    float tmp[PC_ALGO_MAXC] = {0}, v;
    int32_t xx = pc_clamp_i32(x, b.x0, b.x1 - 1), yy = pc_clamp_i32(y, b.y0, b.y1 - 1);
    for (c = 0; c < n && c < PC_ALGO_MAXC; c++) {
        tmp[c] = pc_algo_image_get(src, xx, yy, c);
    }
    switch (lp->texture) {
    case PC_ALGO_TEX_ALPHA:
        v = ctx->alpha ? tmp[n - 1] : 1.0f;
        break;
    case PC_ALGO_TEX_LUMINANCE:
        v = pc_algo_luma(ctx, tmp, n);
        break;
    case PC_ALGO_TEX_RED:
    case PC_ALGO_TEX_GREEN:
    case PC_ALGO_TEX_BLUE: {
        size_t k = lp->texture == PC_ALGO_TEX_RED ? 0 : (lp->texture == PC_ALGO_TEX_GREEN ? 1 : 2);
        if (ctx->mode == PC_COLOR_MODE_RGB) {
            v = tmp[k];
        } else {
            float rgba[4];
            pc_algo_rgba(ctx, tmp, n, rgba);
            v = rgba[k];
        }
        break;
    }
    default:
        v = 0.0f;
        break;
    }
    return lp->white_is_high ? v : 1.0f - v;
}

/* Blinn-Phong shading of the image as a surface (bumped by a texture channel) under spot, point
 * and infinite lights. */
static pc_status lighting_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                            const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_lighting_params *lp = p;
    size_t n = src->ch, i, k, li;
    pc_rect b = ctx->bounds;
    float bw = (float)pc_rect_width(b), bh = (float)pc_rect_height(b);
    float shortside = pc_rs_maxf_k(pc_rs_minf(bw, bh), 1.0f);
    float hscale = pc_clampf(lp->height, 0.0f, 100.0f) / 100.0f * 0.06f * shortside;
    float gloss = pc_clampf(lp->gloss, -100.0f, 100.0f);
    float shininess = exp2f(1.0f + (gloss + 100.0f) / 200.0f * 7.0f);
    float spec_k = (gloss + 100.0f) / 200.0f * 0.7f;
    float metal = (pc_clampf(lp->metallic, -100.0f, 100.0f) + 100.0f) / 200.0f;
    float expo = exp2f(pc_clampf(lp->exposure, -100.0f, 100.0f) / 50.0f);
    float ambient = pc_clampf(lp->ambience, -100.0f, 100.0f) / 100.0f * 0.5f;
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy, nrm[3] = {0.0f, 0.0f, 1.0f}, c[4], base[3], lit[3],
              spc[3] = {0, 0, 0};
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        fx = (float)x + 0.5f;
        fy = (float)y + 0.5f;
        if (lp->texture != PC_ALGO_TEX_NONE && hscale > 0.0f) {
            float hx = (tex_at(lp, src, ctx, x + 1, y) - tex_at(lp, src, ctx, x - 1, y)) * 0.5f * hscale;
            float hy = (tex_at(lp, src, ctx, x, y + 1) - tex_at(lp, src, ctx, x, y - 1)) * 0.5f * hscale;
            const float v[3] = {-hx, -hy, 1.0f};
            norm3(v, nrm);
        }
        pc_algo_rgba(ctx, px, n, c);
        base[0] = c[0], base[1] = c[1], base[2] = c[2];
        lit[0] = lit[1] = lit[2] = ambient;
        for (li = 0; li < lp->lights.n; li++) {
            const pc_algo_light *l = &lp->lights.v[li];
            float lpos[3] = {(float)b.x0 + l->x * bw, (float)b.y0 + l->y * bh, l->z * shortside};
            float ldir[3], atten, inten, ndl, hv[3], s;
            if (l->kind == PC_ALGO_LIGHT_INFINITE) {
                float a = pc_fx_rad(l->angle), sn = sinf(a), co = cosf(a);
                float e = pc_fx_rad(pc_clampf(l->elevation, 0.0f, 90.0f));
                const float v[3] = {co * cosf(e), -sn * cosf(e), sinf(e)};
                norm3(v, ldir);
                atten = 1.0f;
            } else if (l->kind == PC_ALGO_LIGHT_POINT) {
                const float v[3] = {lpos[0] - fx, lpos[1] - fy, lpos[2]};
                float dist = sqrtf(v[0] * v[0] + v[1] * v[1]),
                      reach = pc_rs_maxf_k(l->radius * shortside, 1.0f);
                norm3(v, ldir);
                atten = pc_fx_sq(pc_clampf(1.0f - dist / reach, 0.0f, 1.0f));
            } else {
                const float v[3] = {lpos[0] - fx, lpos[1] - fy, lpos[2]};
                float tgt[3] = {(float)b.x0 + l->target_x * bw, (float)b.y0 + l->target_y * bh, 0.0f};
                float axv[3] = {tgt[0] - lpos[0], tgt[1] - lpos[1], tgt[2] - lpos[2]}, axis[3], cosang, outer,
                      inner;
                float cone = pc_clampf(l->cone, 1.0f, 89.0f);
                norm3(v, ldir);
                norm3(axv, axis);
                cosang = -dot3(ldir, axis);
                outer = cosf(pc_fx_rad(cone));
                inner = cosf(pc_fx_rad(cone * (pc_clampf(l->hotspot, 0.0f, 100.0f) / 100.0f)));
                atten = pc_algo_smoothstep(outer, pc_rs_maxf(inner, outer + 1e-4f), cosang);
            }
            if (atten <= 0.0f) {
                continue;
            }
            inten = pc_clampf(l->intensity, -100.0f, 100.0f) / 50.0f * atten;
            ndl = pc_rs_maxf_k(dot3(nrm, ldir), 0.0f);
            {
                const float v[3] = {ldir[0], ldir[1], ldir[2] + 1.0f};
                norm3(v, hv);
            }
            s = powf(pc_rs_maxf_k(dot3(nrm, hv), 0.0f), shininess) * spec_k;
            for (k = 0; k < 3; k++) {
                float sc;
                lit[k] += ndl * inten * l->color[k];
                /* Metallic highlights take the surface colour; plastic ones the light's. */
                sc = l->color[k] * (1.0f - metal) + base[k] * metal;
                spc[k] += s * pc_rs_maxf_k(inten, 0.0f) * sc;
            }
        }
        for (k = 0; k < 3; k++) {
            c[k] = pc_rs_maxf_k((base[k] * lit[k] + spc[k]) * expo, 0.0f);
        }
        pc_algo_set_rgba(ctx, px, n, c);
    }
    return PC_OK;
}

static pc_algo_halo lighting_halo(const void *p, const pc_rect *b)
{
    (void)p, (void)b;
    return pc_algo_halo_reach(1.0f);
}

static const pc_algo_jfield FIBERS_F[] = {
    PC_ALGO_JF("variance", pc_algo_fibers_params, variance, PC_ALGO_JF_F32),
    PC_ALGO_JF("strength", pc_algo_fibers_params, strength, PC_ALGO_JF_F32),
    PC_ALGO_JF("seed", pc_algo_fibers_params, seed, PC_ALGO_JF_U32),
    PC_ALGO_JFA("foreground", pc_algo_fibers_params, foreground, 4, false),
    PC_ALGO_JFA("background", pc_algo_fibers_params, background, 4, false),
};
static const pc_algo_jfield FLARE_F[] = {
    PC_ALGO_JF("brightness", pc_algo_flare_params, brightness, PC_ALGO_JF_F32),
    PC_ALGO_JF("centerX", pc_algo_flare_params, center_x, PC_ALGO_JF_F32),
    PC_ALGO_JF("centerY", pc_algo_flare_params, center_y, PC_ALGO_JF_F32),
    PC_ALGO_JFE("lens", pc_algo_flare_params, lens, PC_ALGO_ENUM_LENS_TYPE, false),
};
static const pc_algo_jfield LIGHTING_F[] = {
    PC_ALGO_JF("lights", pc_algo_lighting_params, lights, PC_ALGO_JF_LIGHTS),
    PC_ALGO_JF("gloss", pc_algo_lighting_params, gloss, PC_ALGO_JF_F32),
    PC_ALGO_JF("metallic", pc_algo_lighting_params, metallic, PC_ALGO_JF_F32),
    PC_ALGO_JF("exposure", pc_algo_lighting_params, exposure, PC_ALGO_JF_F32),
    PC_ALGO_JF("ambience", pc_algo_lighting_params, ambience, PC_ALGO_JF_F32),
    PC_ALGO_JFE("texture", pc_algo_lighting_params, texture, PC_ALGO_ENUM_TEXTURE_CHANNEL, false),
    PC_ALGO_JF("height", pc_algo_lighting_params, height, PC_ALGO_JF_F32),
    PC_ALGO_JF("whiteIsHigh", pc_algo_lighting_params, white_is_high, PC_ALGO_JF_BOOL),
};

PC_FX_DEF(pc_algo_def_fibers, "fibers", pc_algo_fibers_params, FIBERS_F, NULL, NULL, fibers_k);
PC_FX_DEF(pc_algo_def_lens_flare, "lensFlare", pc_algo_flare_params, FLARE_F, NULL, NULL, flare_k);
PC_FX_DEF(pc_algo_def_lighting, "lightingEffects", pc_algo_lighting_params, LIGHTING_F, NULL, lighting_halo,
          lighting_k);
