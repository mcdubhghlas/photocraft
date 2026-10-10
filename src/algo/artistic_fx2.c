/* Filter Gallery pixel work, part 2: the Distort, Sketch, Stylize and Texture effects (port of
 * crates/algo/src/artistic_fx.rs). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/artistic_fx.h"
#include "algo/fx_defs.h"
#include "algo/fxutil.h"
#include "base/mem.h"

#define G(k) pc_algo_gallery_effect_get(e, (k))
#define NOMEM() pc_fail(err, PC_ERR_NOMEM, "out of memory for the Filter Gallery")
#define TRY(x)                                                                                               \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto done;                                                                                       \
        }                                                                                                    \
    } while (0)

static gpx scale_px(gpx p, float k)
{
    gpx r = {{p.c[0] * k, p.c[1] * k, p.c[2] * k}};
    return r;
}

static float crack(float x, float y, float cell, float width)
{
    float d1, d2, px, py;
    pc_gfx_voronoi(x, y, cell, 201, &d1, &d2, &px, &py);
    return 1.0f - pc_gfx_smoothstep(0.0f, width, d2 - d1);
}

static float tile_edge(float x, float y, float ts)
{
    float jx = x + (pc_gfx_vnoise(x / ts * 0.7f, y / ts * 0.7f, 221) - 0.5f) * ts * 0.3f;
    float jy = y + (pc_gfx_vnoise(x / ts * 0.7f, y / ts * 0.7f, 222) - 0.5f) * ts * 0.3f;
    float u = pc_algo_rem_euclidf(jx, ts), v = pc_algo_rem_euclidf(jy, ts);
    return pc_rs_minf(pc_rs_minf(u, ts - u), pc_rs_minf(v, ts - v));
}

static float patch_height(float x, float y, float size)
{
    int32_t bx = pc_f32_to_i32(floorf(x / size)), by = pc_f32_to_i32(floorf(y / size));
    float u = pc_algo_rem_euclidf(x, size), v = pc_algo_rem_euclidf(y, size);
    float bevel = pc_gfx_smoothstep(0.0f, 1.5f, pc_rs_minf(pc_rs_minf(u, size - u), pc_rs_minf(v, size - v)));
    return pc_algo_hash01(bx, by, 0, 231) * bevel;
}

pc_status pc_gfx_apply2(const pc_algo_gallery_effect *e, gwin *w, const pc_algo_ctx *ctx, pc_error *err)
{
    size_t W = w->w, H = w->h, N = W * H, i, c;
    gpx fg = pc_gfx_rgb(e->foreground), bg = pc_gfx_rgb(e->background);
    float *l = NULL, *m = NULL, *gx = NULL, *gy = NULL;
    gpx *tmp = NULL;
    char cb[PC_ALGO_GALLERY_NAME_MAX], cb2[PC_ALGO_GALLERY_NAME_MAX];
    pc_status st = PC_OK;
    int32_t x, y;
    switch (e->filter) {
    /* ---------------- Distort ---------------- */
    case PC_ALGO_GAL_DIFFUSE_GLOW: {
        float gr = G("graininess") / 10.0f, ga = G("glowAmount") / 20.0f, ca = G("clearAmount") / 20.0f;
        float thr = 0.2f + ca * 0.7f;
        for (i = 0; i < N; i++) {
            float lv = pc_gfx_lum(w->c[i]), mm;
            pc_gfx_xy(w, i, &x, &y);
            mm = pc_gfx_smoothstep(thr - 0.3f, thr + 0.1f, lv) * ga * 1.4f;
            if (pc_algo_hash01(x, y, 0, 101) < gr * 0.3f * (mm + 0.15f)) {
                mm += 0.5f;
            }
            w->c[i] = pc_gfx_mix(w->c[i], bg, pc_gfx_clamp01(mm));
        }
        break;
    }
    case PC_ALGO_GAL_GLASS: {
        float dist = G("distortion"), step = 0.5f + G("smoothness") * 0.5f;
        const char *kind = pc_algo_gallery_effect_choice(e, "texture", cb);
        float s = pc_clampf(G("scaling") / 100.0f, 0.5f, 2.0f),
              sign = pc_algo_gallery_effect_flag(e, "invert") ? -1.0f : 1.0f;
        float mx = dist * 2.0f;
        gwin src;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        src = *w;
        src.c = tmp;
        for (i = 0; i < N; i++) {
            float fx, fy, tx, ty, dx, dy;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x;
            fy = (float)y;
            tx = pc_gfx_texture(kind, fx + step, fy, s) - pc_gfx_texture(kind, fx - step, fy, s);
            ty = pc_gfx_texture(kind, fx, fy + step, s) - pc_gfx_texture(kind, fx, fy - step, s);
            dx = pc_clampf(tx * sign * dist * 3.0f, -mx, mx);
            dy = pc_clampf(ty * sign * dist * 3.0f, -mx, mx);
            w->c[i] = pc_gfx_sample(&src, fx + 0.5f + dx, fy + 0.5f + dy);
        }
        break;
    }
    case PC_ALGO_GAL_OCEAN_RIPPLE: {
        float s = 2.0f + G("rippleSize") * 1.2f, mag = G("rippleMagnitude") * 0.9f;
        gwin src;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        src = *w;
        src.c = tmp;
        for (i = 0; i < N; i++) {
            float fx, fy, dx, dy;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x / s;
            fy = (float)y / s;
            dx = (pc_gfx_vnoise(fx, fy, 111) - 0.5f) * 2.0f * mag;
            dy = (pc_gfx_vnoise(fx, fy, 112) - 0.5f) * 2.0f * mag;
            w->c[i] = pc_gfx_sample(&src, (float)x + 0.5f + dx, (float)y + 0.5f + dy);
        }
        break;
    }
    /* ---------------- Sketch ---------------- */
    case PC_ALGO_GAL_BAS_RELIEF: {
        float lx, ly, k;
        l = pc_gfx_win_lum(w);
        m = pc_gfx_win_lum(w);
        gx = pc_malloc_array(N, sizeof *gx);
        gy = pc_malloc_array(N, sizeof *gy);
        if (l == NULL || m == NULL || gx == NULL || gy == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, pc_gfx_basrelief_sigma(e), err));
        pc_gfx_sobel(l, W, H, gx, gy);
        pc_gfx_light_dir(pc_algo_gallery_effect_choice(e, "light", cb), &lx, &ly);
        k = 3.0f + G("detail") * 0.4f;
        for (i = 0; i < N; i++) {
            float t = pc_gfx_clamp01(m[i] * 0.5f + 0.25f + k * -(gx[i] * lx + gy[i] * ly));
            w->c[i] = pc_gfx_mix(fg, bg, t);
        }
        break;
    }
    case PC_ALGO_GAL_CHALK_CHARCOAL: {
        float ca = G("charcoalArea") / 20.0f, ch = G("chalkArea") / 20.0f, pr = G("strokePressure") / 5.0f;
        gpx mid = pc_gfx_mix(fg, bg, 0.5f);
        for (i = 0; i < N; i++) {
            float lv = pc_gfx_lum(w->c[i]), cn, kn, cc, kc;
            pc_gfx_xy(w, i, &x, &y);
            cn = pc_gfx_strokes(x, y, 1, 1, 9.0f, 0.9f, 121) - 0.5f;
            kn = pc_gfx_strokes(x, y, 1, -1, 9.0f, 0.9f, 122) - 0.5f;
            cc = pc_gfx_smoothstep(0.25f, 0.6f,
                                   (1.0f - lv) * (0.4f + ca * 1.2f) + cn * (0.6f + pr * 0.4f) - 0.15f);
            kc = pc_gfx_smoothstep(0.25f, 0.6f, lv * (0.4f + ch * 1.2f) + kn * (0.6f + pr * 0.4f) - 0.15f);
            w->c[i] = pc_gfx_mix(pc_gfx_mix(mid, fg, cc), bg, kc * (1.0f - cc));
        }
        break;
    }
    case PC_ALGO_GAL_CHARCOAL: {
        float th = G("charcoalThickness"), det = G("detail") / 5.0f, bal = G("lightDarkBalance") / 100.0f;
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 1.0f, err));
        m = pc_gfx_grad_mag(l, W, H);
        if (m == NULL) {
            TRY(NOMEM());
        }
        for (i = 0; i < N; i++) {
            float sn, cov;
            pc_gfx_xy(w, i, &x, &y);
            sn = pc_gfx_strokes(x, y, 1, 1, 8.0f + th * 3.0f, 0.6f + th * 0.5f, 131) - 0.5f;
            cov = pc_gfx_smoothstep(1.0f - bal - 0.2f, 1.0f - bal + 0.2f, 1.0f - l[i] + sn * 0.6f) +
                  pc_gfx_clamp01(m[i] * det * 6.0f);
            w->c[i] = pc_gfx_mix(bg, fg, pc_gfx_clamp01(cov));
        }
        break;
    }
    case PC_ALGO_GAL_CHROME: {
        float det = G("detail") / 10.0f, sm = G("smoothness") / 10.0f;
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 1.0f + sm * 4.0f, err));
        m = pc_gfx_grad_mag(l, W, H);
        if (m == NULL) {
            TRY(NOMEM());
        }
        for (i = 0; i < N; i++) {
            float v = 0.5f + 0.5f * sinf((l[i] * (2.5f + det * 3.0f) + m[i] * 6.0f) * 2.0f * PC_FX_PI);
            w->c[i] = pc_gfx_grey(pc_gfx_smoothstep(0.05f, 0.95f, v));
        }
        break;
    }
    case PC_ALGO_GAL_CONTE_CRAYON: {
        float fl = G("foregroundLevel") / 15.0f, bl = G("backgroundLevel") / 15.0f;
        gpx mid = pc_gfx_mix(fg, bg, 0.5f);
        for (i = 0; i < N; i++) {
            float lv = pc_gfx_lum(w->c[i]), sn, cf, cbk;
            pc_gfx_xy(w, i, &x, &y);
            sn = pc_gfx_strokes(x, y, 1, -1, 7.0f, 1.0f, 141) - 0.5f;
            cf = pc_gfx_clamp01((0.6f - lv) * 2.0f * fl * 1.4f + sn * 0.5f);
            cbk = pc_gfx_clamp01((lv - 0.4f) * 2.0f * bl * 1.4f - sn * 0.5f);
            w->c[i] = pc_gfx_mix(pc_gfx_mix(mid, fg, cf), bg, cbk * (1.0f - cf));
        }
        pc_gfx_texturize(w, pc_algo_gallery_effect_choice(e, "texture", cb), G("scaling"), G("relief"),
                         pc_algo_gallery_effect_choice(e, "light", cb2),
                         pc_algo_gallery_effect_flag(e, "invert"), 0);
        break;
    }
    case PC_ALGO_GAL_GRAPHIC_PEN: {
        size_t r = pc_f32_to_size(G("strokeLength") / 2.0f);
        float bal = G("lightDarkBalance") / 100.0f, sd;
        int32_t dx, dy;
        l = pc_gfx_win_lum(w);
        m = pc_malloc_array(N, sizeof *m);
        if (l == NULL || m == NULL) {
            TRY(NOMEM());
        }
        for (i = 0; i < N; i++) {
            pc_gfx_xy(w, i, &x, &y);
            m[i] = pc_algo_hash01(x, y, 0, 151);
        }
        pc_gfx_direction(pc_algo_gallery_effect_choice(e, "strokeDirection", cb), &dx, &dy);
        TRY(pc_gfx_line_blur(m, 1, W, H, dx, dy, r, err));
        sd = 0.2887f / sqrtf((float)(2 * r + 1));
        for (i = 0; i < N; i++) {
            float z = (m[i] - 0.5f) / sd, t = l[i] + z * 0.16f;
            w->c[i] = pc_gfx_mix(bg, fg, pc_gfx_smoothstep(bal + 0.04f, bal - 0.04f, t));
        }
        break;
    }
    case PC_ALGO_GAL_HALFTONE_PATTERN: {
        float size = G("size"), con = G("contrast") / 50.0f, cell = 3.0f + size * 2.0f, cx, cy;
        pc_rect b = ctx->bounds;
        const char *ty = pc_algo_gallery_effect_choice(e, "patternType", cb);
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 0.5f + size * 0.3f, err));
        cx = (float)pc_i32_wrapping_add(b.x0, b.x1) / 2.0f;
        cy = (float)pc_i32_wrapping_add(b.y0, b.y1) / 2.0f;
        for (i = 0; i < N; i++) {
            float fx, fy, d, aa, ink;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x + 0.5f;
            fy = (float)y + 0.5f;
            d = pc_gfx_clamp01((1.0f - l[i] - 0.5f) * (1.0f + con * 3.0f) + 0.5f);
            aa = 1.0f / cell;
            if (strcmp(ty, "circle") == 0) {
                float r = sqrtf(pc_fx_sq(fx - cx) + pc_fx_sq(fy - cy)) / cell;
                float q = fabsf(pc_fx_fract(r) - 0.5f) * 2.0f;
                ink = pc_gfx_smoothstep(d + aa, d - aa, q);
            } else if (strcmp(ty, "line") == 0) {
                float q = fabsf(pc_fx_fract(fy / cell) - 0.5f) * 2.0f;
                ink = pc_gfx_smoothstep(d + aa, d - aa, q);
            } else {
                float u = pc_fx_fract(fx / cell) - 0.5f, v = pc_fx_fract(fy / cell) - 0.5f;
                float q = sqrtf(u * u + v * v) * 2.0f, rr = sqrtf(d) * 1.15f;
                ink = pc_gfx_smoothstep(rr + aa, rr - aa, q);
            }
            w->c[i] = pc_gfx_mix(bg, fg, ink);
        }
        break;
    }
    case PC_ALGO_GAL_NOTE_PAPER: {
        float bal = G("imageBalance") / 50.0f, gr = G("graininess") / 20.0f, rel = G("relief") / 25.0f, lx,
              ly;
        l = pc_gfx_win_lum(w);
        m = pc_malloc_array(N, sizeof *m);
        gx = pc_malloc_array(N, sizeof *gx);
        gy = pc_malloc_array(N, sizeof *gy);
        if (l == NULL || m == NULL || gx == NULL || gy == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 1.0f, err));
        for (i = 0; i < N; i++) {
            m[i] = pc_gfx_smoothstep(bal - 0.05f, bal + 0.05f, l[i]);
        }
        TRY(pc_gfx_blur1(m, W, H, 1.2f, err));
        pc_gfx_sobel(m, W, H, gx, gy);
        pc_gfx_light_dir("topLeft", &lx, &ly);
        for (i = 0; i < N; i++) {
            float nz, f;
            gpx p;
            pc_gfx_xy(w, i, &x, &y);
            nz = (pc_algo_hash01(x, y, 0, 161) - 0.5f) * gr * 0.3f;
            f = pc_clampf(pc_gfx_shade(gx[i], gy[i], lx, ly, rel * 6.0f), 0.0f, 2.0f);
            p = pc_gfx_mix(fg, bg, m[i]);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = (p.c[c] + nz) * f;
            }
        }
        break;
    }
    case PC_ALGO_GAL_PHOTOCOPY: {
        float det = G("detail"), dk = G("darkness") / 50.0f;
        l = pc_gfx_win_lum(w);
        m = pc_gfx_win_lum(w);
        if (l == NULL || m == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(m, W, H, 1.0f + det * 0.5f, err));
        for (i = 0; i < N; i++) {
            float hp = l[i] - m[i];
            float ink = pc_gfx_smoothstep(0.0f, 0.06f, -hp * (1.0f + dk * 8.0f)) +
                        pc_gfx_smoothstep(0.35f, 0.05f, l[i]) * dk * 1.5f;
            w->c[i] = pc_gfx_mix(bg, fg, pc_gfx_clamp01(ink));
        }
        break;
    }
    case PC_ALGO_GAL_PLASTER: {
        float bal = G("imageBalance") / 50.0f, lx, ly;
        l = pc_gfx_win_lum(w);
        m = pc_malloc_array(N, sizeof *m);
        gx = pc_malloc_array(N, sizeof *gx);
        gy = pc_malloc_array(N, sizeof *gy);
        if (l == NULL || m == NULL || gx == NULL || gy == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 0.5f + G("smoothness") * 0.8f, err));
        for (i = 0; i < N; i++) {
            m[i] = pc_gfx_smoothstep(bal + 0.1f, bal - 0.1f, l[i]);
        }
        TRY(pc_gfx_blur1(m, W, H, 1.5f, err));
        pc_gfx_sobel(m, W, H, gx, gy);
        pc_gfx_light_dir(pc_algo_gallery_effect_choice(e, "light", cb), &lx, &ly);
        for (i = 0; i < N; i++) {
            float f = pc_clampf(pc_gfx_shade(gx[i], gy[i], lx, ly, 6.0f), 0.0f, 2.0f);
            w->c[i] = scale_px(pc_gfx_mix(bg, fg, m[i] * 0.6f), f);
        }
        break;
    }
    case PC_ALGO_GAL_RETICULATION: {
        float den = G("density") / 50.0f, fl = G("foregroundLevel") / 50.0f,
              bl = G("backgroundLevel") / 50.0f;
        gpx ink_c = pc_gfx_mix(bg, fg, 0.3f + fl * 0.7f), paper = pc_gfx_mix(bg, fg, bl * 0.6f);
        for (i = 0; i < N; i++) {
            float nz, t;
            pc_gfx_xy(w, i, &x, &y);
            nz = 0.5f * pc_algo_hash01(x, y, 0, 171) +
                 0.5f * pc_gfx_vnoise((float)x / 1.8f, (float)y / 1.8f, 172);
            t = (1.0f - pc_gfx_lum(w->c[i])) + (nz - 0.5f) * (0.4f + den * 1.2f);
            w->c[i] = pc_gfx_mix(paper, ink_c, pc_gfx_smoothstep(0.45f, 0.55f, t));
        }
        break;
    }
    case PC_ALGO_GAL_STAMP: {
        float bal = G("lightDarkBalance") / 50.0f;
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 0.3f + G("smoothness") * 0.3f, err));
        for (i = 0; i < N; i++) {
            w->c[i] = pc_gfx_mix(bg, fg, pc_gfx_smoothstep(bal + 0.02f, bal - 0.02f, l[i]));
        }
        break;
    }
    case PC_ALGO_GAL_TORN_EDGES: {
        float bal = G("imageBalance") / 50.0f, sm = G("smoothness"), con = G("contrast"), rough, aa;
        l = pc_gfx_win_lum(w);
        if (l == NULL) {
            TRY(NOMEM());
        }
        TRY(pc_gfx_blur1(l, W, H, 1.0f, err));
        rough = (16.0f - sm) / 15.0f * 0.35f;
        aa = 0.25f / con;
        for (i = 0; i < N; i++) {
            float t;
            pc_gfx_xy(w, i, &x, &y);
            t = l[i] + (pc_gfx_fbm((float)x / 2.0f, (float)y / 2.0f, 181) - 0.5f) * rough;
            w->c[i] = pc_gfx_mix(bg, fg, pc_gfx_smoothstep(bal + aa, bal - aa, t));
        }
        break;
    }
    case PC_ALGO_GAL_WATER_PAPER: {
        float fl = G("fiberLength"), br = G("brightness") / 100.0f, co = G("contrast") / 100.0f;
        size_t r = pc_f32_to_size(fl / 6.0f);
        TRY(pc_gfx_line_blur3(w->c, W, H, 1, 0, r, err));
        TRY(pc_gfx_line_blur3(w->c, W, H, 0, 1, r, err));
        for (i = 0; i < N; i++) {
            float fib;
            pc_gfx_xy(w, i, &x, &y);
            fib = pc_rs_maxf(pc_gfx_strokes(x, y, 1, 0, fl, 1.0f, 191),
                             pc_gfx_strokes(x, y, 0, 1, fl, 1.0f, 192));
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = (w->c[i].c[c] * (0.85f + 0.25f * fib) - 0.5f) * (co * 1.6f + 0.2f) + 0.5f +
                               (br - 0.5f) * 0.8f;
            }
        }
        break;
    }
    /* ---------------- Stylize ---------------- */
    case PC_ALGO_GAL_GLOWING_EDGES: {
        float k;
        TRY(pc_gfx_blur3(w->c, W, H, 0.3f + G("smoothness") * 0.25f, err));
        tmp = pc_malloc_array(N, sizeof *tmp);
        m = pc_malloc_array(N, sizeof *m);
        if (tmp == NULL || m == NULL) {
            TRY(NOMEM());
        }
        for (c = 0; c < 3; c++) {
            float *g;
            for (i = 0; i < N; i++) {
                m[i] = w->c[i].c[c];
            }
            g = pc_gfx_grad_mag(m, W, H);
            if (g == NULL) {
                TRY(NOMEM());
            }
            for (i = 0; i < N; i++) {
                tmp[i].c[c] = g[i];
            }
            free(g);
        }
        TRY(pc_gfx_blur3(tmp, W, H, G("edgeWidth") * 0.4f, err));
        k = 2.0f + G("edgeBrightness") * 1.2f;
        for (i = 0; i < N; i++) {
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = pc_gfx_clamp01(tmp[i].c[c] * k);
            }
        }
        break;
    }
    /* ---------------- Texture ---------------- */
    case PC_ALGO_GAL_CRAQUELURE: {
        float sp = G("crackSpacing"), dep = G("crackDepth") / 10.0f, bri = G("crackBrightness") / 10.0f;
        float cell = sp * 1.3f + 1.0f, width = 0.8f + dep * 0.8f, lx, ly;
        pc_gfx_light_dir("topLeft", &lx, &ly);
        for (i = 0; i < N; i++) {
            float fx, fy, c0, tx, ty, f;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x + 0.5f;
            fy = (float)y + 0.5f;
            c0 = crack(fx, fy, cell, width);
            tx = crack(fx + 1.0f, fy, cell, width) - c0;
            ty = crack(fx, fy + 1.0f, cell, width) - c0;
            f = pc_clampf(pc_gfx_shade(-tx, -ty, lx, ly, dep * 2.0f), 0.0f, 2.0f);
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = w->c[i].c[c] * (0.55f + bri * 0.5f) * (1.0f - c0 * dep * 0.6f) * f;
            }
        }
        break;
    }
    case PC_ALGO_GAL_GRAIN: {
        float it = G("intensity") / 100.0f, co = G("contrast") / 100.0f;
        const char *ty = pc_algo_gallery_effect_choice(e, "grainType", cb);
        for (i = 0; i < N; i++) {
            gpx p = w->c[i], q = p;
            pc_gfx_xy(w, i, &x, &y);
#define HC(k) (pc_algo_hash01(x, y, (k), 211) - 0.5f)
            if (strcmp(ty, "soft") == 0) {
                for (c = 0; c < 3; c++) {
                    q.c[c] = p.c[c] + HC((uint32_t)c) * it * 0.4f;
                }
            } else if (strcmp(ty, "sprinkles") == 0) {
                q = pc_algo_hash01(x, y, 3, 211) < it * 0.15f ? bg : p;
            } else if (strcmp(ty, "clumped") == 0) {
                float nz = (pc_gfx_vnoise((float)x / 2.0f, (float)y / 2.0f, 212) - 0.5f) * it * 1.2f;
                for (c = 0; c < 3; c++) {
                    q.c[c] = p.c[c] + nz;
                }
            } else if (strcmp(ty, "contrasty") == 0) {
                for (c = 0; c < 3; c++) {
                    q.c[c] = (p.c[c] - 0.5f) * (1.0f + it * 2.0f) + 0.5f + HC(4) * it * 0.5f;
                }
            } else if (strcmp(ty, "enlarged") == 0) {
                float fx = (float)x / 3.0f, fy = (float)y / 3.0f;
                for (c = 0; c < 3; c++) {
                    q.c[c] = p.c[c] + (pc_gfx_vnoise(fx, fy, 213 + (uint32_t)c) - 0.5f) * it;
                }
            } else if (strcmp(ty, "stippled") == 0) {
                gpx stp = pc_gfx_lum(p) > pc_algo_hash01(x, y, 5, 211) ? bg : fg;
                q = pc_gfx_mix(p, stp, 0.3f + it * 0.7f);
            } else if (strcmp(ty, "horizontal") == 0) {
                float nz = (pc_gfx_vnoise((float)x / 25.0f, (float)y, 216) - 0.5f) * it;
                for (c = 0; c < 3; c++) {
                    q.c[c] = p.c[c] + nz;
                }
            } else if (strcmp(ty, "vertical") == 0) {
                float nz = (pc_gfx_vnoise((float)x, (float)y / 25.0f, 217) - 0.5f) * it;
                for (c = 0; c < 3; c++) {
                    q.c[c] = p.c[c] + nz;
                }
            } else if (strcmp(ty, "speckle") == 0) {
                q = pc_algo_hash01(x, y, 6, 211) < it * 0.1f ? pc_gfx_mix(p, fg, 0.85f) : p;
            } else {
                for (c = 0; c < 3; c++) {
                    q.c[c] = p.c[c] + HC((uint32_t)c) * it * 0.8f;
                }
            }
#undef HC
            for (c = 0; c < 3; c++) {
                w->c[i].c[c] = (q.c[c] - 0.5f) * (0.5f + co) + 0.5f;
            }
        }
        break;
    }
    case PC_ALGO_GAL_MOSAIC_TILES: {
        float ts = G("tileSize"), gw = G("groutWidth"), lg = G("lightenGrout") / 10.0f;
        for (i = 0; i < N; i++) {
            float fx, fy, de, grout, s, lv;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x + 0.5f;
            fy = (float)y + 0.5f;
            de = tile_edge(fx, fy, ts);
            grout = pc_gfx_smoothstep(gw * 0.5f + 0.7f, gw * 0.5f - 0.3f, de);
            s = (pc_gfx_smoothstep(gw * 0.5f, gw * 0.5f + 2.5f, tile_edge(fx - 1.0f, fy - 1.0f, ts)) -
                 pc_gfx_smoothstep(gw * 0.5f, gw * 0.5f + 2.5f, tile_edge(fx + 1.0f, fy + 1.0f, ts))) *
                0.35f;
            lv = pc_gfx_lum(w->c[i]);
            w->c[i] = pc_gfx_mix(scale_px(w->c[i], 1.0f + s),
                                 pc_gfx_grey(pc_gfx_clamp01(lv * 0.5f + lg * 0.45f)), grout);
        }
        break;
    }
    case PC_ALGO_GAL_PATCHWORK: {
        float size = pc_gfx_patch_size(e), rel = G("relief") / 25.0f;
        gwin src;
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        src = *w;
        src.c = tmp;
        for (i = 0; i < N; i++) {
            float fx, fy, bx, by, s;
            gpx p;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x + 0.5f;
            fy = (float)y + 0.5f;
            bx = floorf(fx / size);
            by = floorf(fy / size);
            p = pc_gfx_at(&src, pc_f32_to_i32((bx + 0.5f) * size), pc_f32_to_i32((by + 0.5f) * size));
            s = (patch_height(fx - 1.0f, fy - 1.0f, size) - patch_height(fx + 1.0f, fy + 1.0f, size)) * rel *
                1.5f;
            w->c[i] = scale_px(p, 1.0f + s);
        }
        break;
    }
    case PC_ALGO_GAL_STAINED_GLASS: {
        float cell = pc_gfx_glass_cell(e), bt = G("borderThickness"), li = G("lightIntensity") / 10.0f, cx,
              cy, half;
        pc_rect b = ctx->bounds;
        uint32_t bw = pc_rect_width(b), bh = pc_rect_height(b);
        gwin src;
        cx = (float)pc_i32_wrapping_add(b.x0, b.x1) / 2.0f;
        cy = (float)pc_i32_wrapping_add(b.y0, b.y1) / 2.0f;
        half = pc_rs_maxf_k((float)(bw > bh ? bw : bh) / 2.0f, 1.0f);
        tmp = pc_gfx_clone(w);
        if (tmp == NULL) {
            TRY(NOMEM());
        }
        src = *w;
        src.c = tmp;
        for (i = 0; i < N; i++) {
            float fx, fy, d1, d2, px, py, border, dn;
            gpx col;
            pc_gfx_xy(w, i, &x, &y);
            fx = (float)x + 0.5f;
            fy = (float)y + 0.5f;
            pc_gfx_voronoi(fx, fy, cell, 241, &d1, &d2, &px, &py);
            border = pc_gfx_smoothstep(bt * 0.5f + 0.6f, bt * 0.5f - 0.4f, (d2 - d1) * 0.5f);
            dn = pc_rs_minf_k(sqrtf(pc_fx_sq(fx - cx) + pc_fx_sq(fy - cy)) / half, 1.0f);
            col = scale_px(pc_gfx_at(&src, pc_f32_to_i32(px), pc_f32_to_i32(py)),
                           1.0f + li * 0.6f * (1.0f - dn));
            w->c[i] = pc_gfx_mix(col, fg, border);
        }
        break;
    }
    case PC_ALGO_GAL_TEXTURIZER:
        pc_gfx_texturize(w, pc_algo_gallery_effect_choice(e, "texture", cb), G("scaling"), G("relief"),
                         pc_algo_gallery_effect_choice(e, "light", cb2),
                         pc_algo_gallery_effect_flag(e, "invert"), 0);
        break;
    default:
        break;
    }
done:
    free(l);
    free(m);
    free(gx);
    free(gy);
    free(tmp);
    return st;
}
