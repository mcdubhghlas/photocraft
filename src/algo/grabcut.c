/* GrabCut and rectangle Object Selection (port of crates/algo/src/segment/grabcut.rs).
 *
 * Rother, Kolmogorov & Blake, "GrabCut", SIGGRAPH 2004: a trimap, 5-component full-covariance
 * GMMs per side, and iterated "assign components, learn GMMs, min cut" steps with the
 * contrast-sensitive smoothness gamma e^(-beta |dz|^2) / dist (gamma 50). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define MAX_SAMPLES 40000

static bool is_fg(uint8_t t)
{
    return t == PC_ALGO_GRABCUT_FG || t == PC_ALGO_GRABCUT_PR_FG;
}

pc_status pc_algo_grabcut(const pc_algo_rgb_image *img, uint8_t *trimap, size_t iters, pc_algo_gmm *fg_out,
                          pc_algo_gmm *bg_out, bool *some, pc_error *err)
{
    size_t n = img->w * img->h, i, it, nf, nb;
    float beta = pc_algo_segment_contrast_beta(img);
    uint8_t *fixed = pc_malloc_array(n ? n : 1, 1);
    float (*fs)[3] = pc_malloc_array(n ? n : 1, sizeof *fs),
          (*bs)[3] = pc_malloc_array(n ? n : 1, sizeof *bs);
    float *cf = pc_malloc_array(n ? n : 1, sizeof(float)), *cb = pc_malloc_array(n ? n : 1, sizeof(float));
    bool *cut = pc_malloc_array(n ? n : 1, sizeof(bool)), have = false;
    pc_algo_gmm fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f};
    pc_status st = PC_OK;
    *some = false;
    if (fixed == NULL || fs == NULL || bs == NULL || cf == NULL || cb == NULL || cut == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for GrabCut");
        goto done;
    }
    for (i = 0; i < n; i++) {
        fixed[i] = trimap[i] == PC_ALGO_GRABCUT_FG   ? PC_ALGO_SEGMENT_HARD_FG
                   : trimap[i] == PC_ALGO_GRABCUT_BG ? PC_ALGO_SEGMENT_HARD_BG
                                                     : PC_ALGO_SEGMENT_FREE;
    }
    for (it = 0; it < (iters > 1 ? iters : 1); it++) {
        float (*sf)[3] = NULL, (*sb)[3] = NULL;
        size_t nsf = 0, nsb = 0, changed = 0;
        pc_algo_gmm nfg, nbg;
        bool okf = false, okb = false;
        nf = nb = 0;
        for (i = 0; i < n; i++) {
            float (*dst)[3] = is_fg(trimap[i]) ? &fs[nf++] : &bs[nb++];
            memcpy(*dst, img->px[i], sizeof *dst);
        }
        if ((st = pc_algo_segment_subsample((const float (*)[3])fs, nf, MAX_SAMPLES, &sf, &nsf, err)) !=
            PC_OK) {
            goto done;
        }
        if ((st = pc_algo_segment_subsample((const float (*)[3])bs, nb, MAX_SAMPLES, &sb, &nsb, err)) !=
            PC_OK) {
            free(sf);
            goto done;
        }
        if (!have) {
            st = pc_algo_gmm_fit((const float (*)[3])sf, nsf, PC_ALGO_GRABCUT_K, PC_ALGO_GMM_DEFAULT_REG,
                                 &nfg, &okf, err);
            if (st == PC_OK && okf) {
                st = pc_algo_gmm_fit((const float (*)[3])sb, nsb, PC_ALGO_GRABCUT_K, PC_ALGO_GMM_DEFAULT_REG,
                                     &nbg, &okb, err);
                if (st != PC_OK || !okb) {
                    pc_algo_gmm_fini(&nfg);
                }
            }
            free(sf);
            free(sb);
            if (st != PC_OK || !okf || !okb) {
                /* Either side without pixels: no result (the trimap is untouched). */
                goto done;
            }
        } else {
            st = pc_algo_gmm_refit(&fg, (const float (*)[3])sf, nsf, &nfg, &okf, err);
            if (st == PC_OK && !okf) {
                st = pc_algo_gmm_clone(&fg, &nfg, err);
            }
            if (st == PC_OK) {
                st = pc_algo_gmm_refit(&bg, (const float (*)[3])sb, nsb, &nbg, &okb, err);
                if (st == PC_OK && !okb) {
                    st = pc_algo_gmm_clone(&bg, &nbg, err);
                }
                if (st != PC_OK) {
                    pc_algo_gmm_fini(&nfg);
                }
            }
            free(sf);
            free(sb);
            if (st != PC_OK) {
                goto done;
            }
        }
        st = pc_algo_segment_data_costs(img, &nfg, &nbg, cf, cb, err);
        if (st == PC_OK) {
            st = pc_algo_segment_grid_cut(img, cf, cb, fixed, PC_ALGO_GRABCUT_GAMMA, beta, cut, err);
        }
        if (st != PC_OK) {
            pc_algo_gmm_fini(&nfg);
            pc_algo_gmm_fini(&nbg);
            goto done;
        }
        for (i = 0; i < n; i++) {
            if (trimap[i] == PC_ALGO_GRABCUT_PR_BG || trimap[i] == PC_ALGO_GRABCUT_PR_FG) {
                uint8_t nt = cut[i] ? PC_ALGO_GRABCUT_PR_FG : PC_ALGO_GRABCUT_PR_BG;
                changed += nt != trimap[i];
                trimap[i] = nt;
            }
        }
        pc_algo_gmm_fini(&fg);
        pc_algo_gmm_fini(&bg);
        fg = nfg;
        bg = nbg;
        have = true;
        if (changed * 2000 < n) {
            break;
        }
    }
    if (have) {
        *fg_out = fg;
        *bg_out = bg;
        fg.comps = NULL;
        bg.comps = NULL;
        *some = true;
    }
done:
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    free(fixed);
    free(fs);
    free(bs);
    free(cf);
    free(cb);
    free(cut);
    return st;
}

/* 3x3 box blur (clamped at the edges); out != px, both w*h (grabcut.rs box3). */
static void box3(const float (*px)[3], size_t w, size_t h, float (*out)[3])
{
    for (size_t y = 0; y < h; y++) {
        size_t y0 = y > 0 ? y - 1 : 0, y1 = (y + 2 < h) ? y + 2 : h;
        for (size_t x = 0; x < w; x++) {
            size_t x0 = x > 0 ? x - 1 : 0, x1 = (x + 2 < w) ? x + 2 : w;
            float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f, n = 0.0f;
            for (size_t yy = y0; yy < y1; yy++) {
                for (size_t xx = x0; xx < x1; xx++) {
                    const float *p = px[yy * w + xx];
                    acc0 += p[0];
                    acc1 += p[1];
                    acc2 += p[2];
                    n += 1.0f;
                }
            }
            float inv = n > 0.0f ? 1.0f / n : 1.0f;
            out[y * w + x][0] = acc0 * inv;
            out[y * w + x][1] = acc1 * inv;
            out[y * w + x][2] = acc2 * inv;
        }
    }
}

static float lab_dist(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* Region growing (4-connected) from (sx, sy) over pixels of similar CIELAB colour, bounded by the
 * distance to the seed area's running mean and by the local edge step (grabcut.rs grow_similar).
 * out: caller buffer of w*h bools. */
static pc_status grow_similar(const float (*lab)[3], size_t w, size_t h, size_t sx, size_t sy, bool *out,
                              pc_error *err)
{
    const float MEAN_DIST = 32.0f, EDGE_STEP = 10.0f;
    size_t n = w * h;
    memset(out, 0, n * sizeof(bool));
    size_t seed = sy * w + sx;
    if (seed >= n) {
        return PC_OK;
    }
    size_t *stack = pc_malloc_array(n > 0 ? n : 1, sizeof *stack);
    if (stack == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the Object Finder");
    }
    double sum[3] = {(double)lab[seed][0], (double)lab[seed][1], (double)lab[seed][2]};
    double cnt = 1.0;
    size_t sp = 0;
    stack[sp++] = seed;
    out[seed] = true;
    while (sp > 0) {
        size_t i = stack[--sp];
        size_t x = i % w, y = i / w;
        const float *c = lab[i];
        float mean[3] = {(float)(sum[0] / cnt), (float)(sum[1] / cnt), (float)(sum[2] / cnt)};
        size_t nxs[4] = {x - 1, x + 1, x, x};
        size_t nys[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; k++) {
            size_t nx = nxs[k], ny = nys[k];
            if (nx >= w || ny >= h) {
                continue;
            }
            size_t j = ny * w + nx;
            if (out[j]) {
                continue;
            }
            const float *q = lab[j];
            if (lab_dist(q, c) <= EDGE_STEP && lab_dist(q, mean) <= MEAN_DIST) {
                out[j] = true;
                sum[0] += (double)q[0];
                sum[1] += (double)q[1];
                sum[2] += (double)q[2];
                cnt += 1.0;
                stack[sp++] = j;
            }
        }
    }
    free(stack);
    return PC_OK;
}

pc_status pc_algo_grabcut_object_at(const pc_algo_sampler *s, pc_rect canvas, float x, float y,
                                    size_t work_px, pc_algo_region *out, pc_error *err)
{
    pc_algo_rgb_image img = {0, 0, NULL};
    float (*lab)[3] = NULL, (*tmp)[3] = NULL;
    bool *grown = NULL, *low = NULL, *seeds = NULL, *kept = NULL, *clean = NULL;
    uint8_t *trimap = NULL;
    pc_algo_gmm fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f};
    pc_status st = PC_OK;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    if (pc_rect_width(canvas) < 4 || pc_rect_height(canvas) < 4 || !isfinite(x) || !isfinite(y)) {
        return PC_OK;
    }
    int32_t ipx = (int32_t)floorf(x), ipy = (int32_t)floorf(y);
    if (!pc_rect_contains(canvas, ipx, ipy)) {
        return PC_OK;
    }
    size_t budget = work_px > 1024 ? work_px : 1024;
    size_t step = pc_algo_segment_scale_for(canvas, budget);
    st = pc_algo_sampler_rgb_scaled(s, canvas, step, &img, err);
    if (st != PC_OK) {
        return st;
    }
    size_t w = img.w, h = img.h, n = w * h;
    if (w < 2 || h < 2) {
        goto done;
    }
    lab = pc_malloc_array(n, sizeof *lab);
    tmp = pc_malloc_array(n, sizeof *tmp);
    grown = pc_malloc_array(n, sizeof(bool));
    low = pc_malloc_array(n, sizeof(bool));
    seeds = pc_malloc_array(n, sizeof(bool));
    kept = pc_malloc_array(n, sizeof(bool));
    clean = pc_malloc_array(n, sizeof(bool));
    trimap = pc_malloc_array(n, 1);
    if (lab == NULL || tmp == NULL || grown == NULL || low == NULL || seeds == NULL || kept == NULL ||
        clean == NULL || trimap == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the Object Finder");
        goto done;
    }
    /* Lightly smoothed (two box blurs), so noise and texture don't stop the growth; edges survive. */
    box3((const float (*)[3])img.px, w, h, tmp);
    box3((const float (*)[3])tmp, w, h, lab); /* lab holds the twice-blurred RGB for now */
    for (size_t i = 0; i < n; i++) {
        float rgb[3] = {lab[i][0], lab[i][1], lab[i][2]};
        pc_algo_slic_rgb_to_lab(rgb, lab[i]);
    }
    size_t sx = (size_t)((ipx - canvas.x0)) / step;
    size_t sy = (size_t)((ipy - canvas.y0)) / step;
    if (sx > w - 1) {
        sx = w - 1;
    }
    if (sy > h - 1) {
        sy = h - 1;
    }
    st = grow_similar((const float (*)[3])lab, w, h, sx, sy, grown, err);
    if (st != PC_OK) {
        goto done;
    }
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        count += grown[i] ? 1u : 0u;
    }
    /* Background (or a flat image): no single object under the cursor. */
    if (count * 10 > n * 6 || count < 4) {
        goto done;
    }
    size_t gx0 = w, gy0 = h, gx1 = 0, gy1 = 0;
    for (size_t i = 0; i < n; i++) {
        if (grown[i]) {
            size_t gx = i % w, gy = i / w;
            if (gx < gx0) gx0 = gx;
            if (gy < gy0) gy0 = gy;
            if (gx + 1 > gx1) gx1 = gx + 1;
            if (gy + 1 > gy1) gy1 = gy + 1;
        }
    }
    size_t spanx = gx1 - gx0, spany = gy1 - gy0;
    size_t pad = (spanx > spany ? spanx : spany) / 6;
    if (pad < 2) {
        pad = 2;
    }
    size_t bx0 = gx0 > pad ? gx0 - pad : 0, by0 = gy0 > pad ? gy0 - pad : 0;
    size_t bx1 = (gx1 + pad < w) ? gx1 + pad : w, by1 = (gy1 + pad < h) ? gy1 + pad : h;
    bool any_bg = false;
    for (size_t yy = 0; yy < h; yy++) {
        for (size_t xx = 0; xx < w; xx++) {
            size_t i = yy * w + xx;
            bool inside = xx >= bx0 && xx < bx1 && yy >= by0 && yy < by1;
            if (grown[i]) {
                trimap[i] = PC_ALGO_GRABCUT_PR_FG;
            } else if (inside) {
                trimap[i] = PC_ALGO_GRABCUT_PR_BG;
            } else {
                trimap[i] = PC_ALGO_GRABCUT_BG;
                any_bg = true;
            }
        }
    }
    if (!any_bg) {
        /* The box is the whole canvas: the frame is background. */
        for (size_t yy = 0; yy < h; yy++) {
            for (size_t xx = 0; xx < w; xx++) {
                if ((xx == 0 || yy == 0 || xx + 1 == w || yy + 1 == h) && !(xx == sx && yy == sy)) {
                    trimap[yy * w + xx] = PC_ALGO_GRABCUT_BG;
                }
            }
        }
    }
    trimap[sy * w + sx] = PC_ALGO_GRABCUT_FG;
    bool some = false;
    st = pc_algo_grabcut(&img, trimap, 5, &fg, &bg, &some, err);
    if (st != PC_OK || !some) {
        goto done;
    }
    for (size_t i = 0; i < n; i++) {
        low[i] = is_fg(trimap[i]);
        seeds[i] = false;
    }
    seeds[sy * w + sx] = true;
    st = pc_algo_segment_keep_seeded(low, seeds, w, h, kept, err);
    if (st != PC_OK) {
        goto done;
    }
    st = pc_algo_segment_clean_mask(kept, w, h, 0.1f, 0.01f, clean, err);
    if (st != PC_OK) {
        goto done;
    }
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        any = any || clean[i];
    }
    if (!any) {
        goto done;
    }
    st = pc_algo_segment_finish_region(s, canvas, step, clean, w, h, &fg, &bg, out, err);
done:
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    pc_algo_rgb_image_fini(&img);
    free(lab);
    free(tmp);
    free(grown);
    free(low);
    free(seeds);
    free(kept);
    free(clean);
    free(trimap);
    return st;
}

pc_status pc_algo_grabcut_object_select(const pc_algo_sampler *s, pc_rect canvas, pc_rect rect,
                                        size_t work_px, pc_algo_region *out, pc_error *err)
{
    pc_rect window;
    int32_t margin;
    size_t step, lw, lh, x, y, ring;
    uint32_t mx;
    pc_algo_rgb_image img;
    uint8_t *trimap = NULL;
    bool *low = NULL, *clean = NULL, any_bg = false, some = false;
    pc_algo_gmm fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f};
    pc_status st;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    rect = pc_rect_intersect(rect, canvas);
    if (pc_rect_width(rect) < 2 || pc_rect_height(rect) < 2) {
        return PC_OK;
    }
    mx = pc_rect_width(rect) > pc_rect_height(rect) ? pc_rect_width(rect) : pc_rect_height(rect);
    margin = pc_f32_to_i32(ceilf((float)mx * 0.15f));
    margin = margin > 8 ? margin : 8;
    window = pc_rect_intersect(pc_rect_inflate(rect, margin), canvas);
    step = pc_algo_segment_scale_for(window, work_px);
    st = pc_algo_sampler_rgb_scaled(s, window, step, &img, err);
    if (st != PC_OK) {
        return st;
    }
    lw = img.w;
    lh = img.h;
    trimap = pc_malloc_array((lw * lh) > 0 ? lw * lh : 1, 1);
    low = pc_malloc_array((lw * lh) > 0 ? lw * lh : 1, sizeof(bool));
    clean = pc_malloc_array((lw * lh) > 0 ? lw * lh : 1, sizeof(bool));
    if (trimap == NULL || low == NULL || clean == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Object Selection");
        goto done;
    }
    /* Working pixel (x, y) covers doc pixels [x0 + x step, ...); its centre decides. */
    for (y = 0; y < lh; y++) {
        for (x = 0; x < lw; x++) {
            int32_t cx = window.x0 + (int32_t)(x * step + step / 2),
                    cy = window.y0 + (int32_t)(y * step + step / 2);
            if (pc_rect_contains(rect, cx, cy)) {
                trimap[y * lw + x] = PC_ALGO_GRABCUT_PR_FG;
            } else {
                trimap[y * lw + x] = PC_ALGO_GRABCUT_BG;
                any_bg = true;
            }
        }
    }
    if (!any_bg) {
        /* The rectangle covers the whole canvas: use a thin outer ring as background. */
        ring = (lw < lh ? lw : lh) / 50;
        ring = ring > 1 ? ring : 1;
        for (y = 0; y < lh; y++) {
            for (x = 0; x < lw; x++) {
                if (x < ring || y < ring || x + ring >= lw || y + ring >= lh) {
                    trimap[y * lw + x] = PC_ALGO_GRABCUT_BG;
                }
            }
        }
    }
    st = pc_algo_grabcut(&img, trimap, 8, &fg, &bg, &some, err);
    if (st != PC_OK || !some) {
        goto done;
    }
    for (x = 0; x < lw * lh; x++) {
        low[x] = is_fg(trimap[x]);
    }
    st = pc_algo_segment_clean_mask(low, lw, lh, 0.1f, 0.002f, clean, err);
    if (st == PC_OK) {
        st = pc_algo_segment_finish_region(s, window, step, clean, lw, lh, &fg, &bg, out, err);
    }
done:
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    pc_algo_rgb_image_fini(&img);
    free(trimap);
    free(low);
    free(clean);
    return st;
}
