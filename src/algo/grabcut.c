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
