/* Second part of test_retouch_parity.c (included there, after its helpers): non-local completion,
 * the Remove Tool, masked / owned completion and the busy thread-independence cases, proximity
 * match over depths and channels, the seam carver's proxy-height search, and Content-Aware Fill's
 * sampling window, Auto sampling area, colour amount and level transform. Mirrors the functions
 * after paint_vectors in tools/rustref/src/algo_retouch_vectors.rs. */
#ifndef PC_TESTS_RETOUCH_PARITY_MORE_H
#define PC_TESTS_RETOUCH_PARITY_MORE_H

#include "algo/inpaint_int.h"
#include "algo/nonlocal.h"
#include "algo/remove.h"

static uint64_t hb(const bool *v, size_t n)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t b = v[i] ? 1 : 0;
        fnv(&h, &b, 1);
    }
    return h;
}

/* The result is passed by address: it is read after the call that fills it (argument evaluation
 * order is unspecified). */
static void show_opt(const char *group, const char *key, pc_status st, float *const *vp, size_t n)
{
    const float *v = *vp;
    CHECK(st == PC_OK);
    if (st != PC_OK) {
        line("%s %s ERR", group, key);
    } else if (v == NULL) {
        line("%s %s none", group, key);
    } else {
        line("%s %s %016llx", group, key, (unsigned long long)of(v, n));
    }
}

/* SplitMix64 `busy` image of the inpaint tests. */
static float *busy(size_t w, size_t h, size_t ch, uint64_t seed)
{
    float *v = malloc(w * h * ch * sizeof(float) + 4);
    pc_ip_rng rng = pc_ip_rng_new(seed);
    size_t i, c;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        bool flat = (x / 9 + y / 7) % 5 == 0;
        float nn = (float)(pc_ip_rng_next(&rng) % 1000) / 1000.0f;
        for (c = 0; c < ch; c++) {
            v[i * ch + c] =
                flat ? 0.25f + 0.1f * (float)c
                     : fminf(0.2f + 0.004f * (float)x + 0.1f * (float)((x + 2 * y + c) % 6) + 0.05f * nn,
                             1.0f);
        }
    }
    return v;
}

static uint8_t *roles(size_t w, size_t h, const bool *hole)
{
    uint8_t *px = malloc(w * h + 1);
    size_t i;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        px[i] = hole[i]                                 ? PC_ALGO_PX_HOLE
                : (x < 4 && y < 6)                      ? PC_ALGO_PX_VOID
                : (x >= w * 3 / 4 && x < w * 3 / 4 + 3) ? PC_ALGO_PX_KNOWN
                                                        : PC_ALGO_PX_SOURCE;
    }
    return px;
}

static void nl_run(const char *key, size_t w, size_t h, size_t ch, const float *im, const bool *hole,
                   const pc_algo_nonlocal_params *p)
{
    float *o = NULL;
    pc_status st = pc_algo_nonlocal_complete(w, h, ch, im, w * h * ch, hole, w * h, p, NULL, &o, NULL);
    show_opt("nonlocal", key, st, &o, w * h * ch);
    free(o);
}

static float nl_noise(size_t i)
{
    return (float)(((uint32_t)i * 2654435761u) >> 8) / (float)(1u << 24);
}

static void nonlocal_vectors(void)
{
    pc_algo_nonlocal_params d = pc_algo_nonlocal_params_default(), p;
    size_t i;
    {
        size_t w = 96, h = 64;
        float *flat = malloc(w * h * sizeof(float));
        bool *hole = rect_mask(w, h, 60, 20, 84, 44);
        for (i = 0; i < w * h; i++) {
            flat[i] = i % w < 48 ? 0.5f : 0.25f + 0.5f * nl_noise(i);
        }
        nl_run("flat", w, h, 1, flat, hole, &d);
        free(flat), free(hole);
    }
    {
        size_t w = 80, h = 60;
        float *grad = malloc(w * h * 2 * sizeof(float));
        bool *hole = rect_mask(w, h, 30, 20, 50, 40);
        for (i = 0; i < w * h; i++) {
            grad[i * 2] = (float)(i % w) / (float)w, grad[i * 2 + 1] = 1.0f;
        }
        nl_run("gradient", w, h, 2, grad, hole, &d);
        free(grad), free(hole);
    }
    {
        float *tex = texture(48, 40);
        bool *hole = disc(48, 40, 24, 20, 7);
        nl_run("tex", 48, 40, 3, tex, hole, &d);
        p = d, p.patch_radius = 2, p.lambda = 0.0f, p.seed = 5;
        nl_run("tex/r2", 48, 40, 3, tex, hole, &p);
        p = d, p.max_iters = 3, p.pm_passes = 1, p.tolerance = 0.0f;
        nl_run("tex/iters", 48, 40, 3, tex, hole, &p);
        p = d, p.patch_radius = 1, p.lambda = 7.5f;
        nl_run("tex/r1", 48, 40, 3, tex, hole, &p);
        free(tex), free(hole);
    }
    {
        float *a = smooth(120, 100, 4, 2), *b = noise(64 * 48, 5, 3), *c = smooth(40, 30, 3, 1);
        float *e = busy(70, 56, 3, 2), *f = img(12, 12, 1, 1);
        bool *ha = disc(120, 100, 60, 50, 22), *hbm = rect_mask(64, 48, 20, 10, 36, 30);
        bool *hc = rect_mask(40, 30, 0, 0, 10, 12), *he = disc(70, 56, 33, 27, 9);
        bool *all = fill_mask(144, true), *no = fill_mask(144, false);
        nl_run("pyramid", 120, 100, 4, a, ha, &d);
        nl_run("ties", 64, 48, 1, b, hbm, &d);
        nl_run("edge", 40, 30, 3, c, hc, &d);
        nl_run("busy", 70, 56, 3, e, he, &d);
        nl_run("allhole", 12, 12, 1, f, all, &d);
        nl_run("nohole", 12, 12, 1, f, no, &d);
        free(a), free(b), free(c), free(e), free(f), free(ha), free(hbm), free(hc), free(he), free(all),
            free(no);
    }
}

static void bool_line(const char *key, pc_status st, bool *const *vp, size_t n)
{
    const bool *v = *vp;
    CHECK(st == PC_OK);
    line("%s %016llx", key, st == PC_OK ? (unsigned long long)hb(v, n) : 0ull);
}

static void remove_vectors(void)
{
    size_t i, r;
    bool *o = NULL;
    {
        bool *ring = malloc(1600), *sq = malloc(400), *speck = malloc(23 * 17);
        float *sn = noise(23 * 17, 10, 4);
        for (i = 0; i < 1600; i++) {
            float dd = hypotf((float)(i % 40) - 20.0f, (float)(i / 40) - 20.0f);
            ring[i] = dd >= 10.0f && dd < 12.0f;
        }
        for (i = 0; i < 400; i++) {
            size_t x = i % 20, y = i / 20;
            sq[i] = (x == 5 || x == 14 || y == 5 || y == 14) && x >= 5 && x <= 14 && y >= 5 && y <= 14;
        }
        for (i = 0; i < 23 * 17; i++) {
            speck[i] = sn[i] > 0.8f;
        }
        bool_line("remove close/ring", pc_algo_remove_close_loops(40, 40, ring, 1600, &o, NULL), &o, 1600);
        free(o), o = NULL;
        bool_line("remove close/square", pc_algo_remove_close_loops(20, 20, sq, 400, &o, NULL), &o, 400);
        free(o), o = NULL;
        sq[5 * 20 + 9] = false;
        bool_line("remove close/open", pc_algo_remove_close_loops(20, 20, sq, 400, &o, NULL), &o, 400);
        free(o), o = NULL;
        bool_line("remove close/speck", pc_algo_remove_close_loops(23, 17, speck, 391, &o, NULL), &o, 391);
        free(o), o = NULL;
        for (r = 0; r < 5; r++) {
            char key[64];
            (void)snprintf(key, sizeof key, "remove dilate/%zu", r);
            bool_line(key, pc_algo_remove_dilate(23, 17, speck, 391, r, &o, NULL), &o, 391);
            free(o), o = NULL;
        }
        {
            bool *ln = malloc(900), *blk = malloc(900), *all = fill_mask(900, true),
                 *dsc = disc(40, 40, 20, 20, 13);
            uint32_t v[5] = {0, 0, 0, 0, 0};
            for (i = 0; i < 900; i++) {
                size_t x = i % 30, y = i / 30;
                ln[i] = y == 10 && x >= 5 && x < 25;
                blk[i] = x >= 5 && x < 16 && y >= 5 && y < 16;
            }
            CHECK(pc_algo_remove_hole_radius(30, 30, ln, 900, &v[0], NULL) == PC_OK);
            CHECK(pc_algo_remove_hole_radius(30, 30, blk, 900, &v[1], NULL) == PC_OK);
            CHECK(pc_algo_remove_hole_radius(30, 30, all, 900, &v[2], NULL) == PC_OK);
            CHECK(pc_algo_remove_hole_radius(23, 17, speck, 391, &v[3], NULL) == PC_OK);
            CHECK(pc_algo_remove_hole_radius(40, 40, dsc, 1600, &v[4], NULL) == PC_OK);
            line("remove radius %u %u %u %u %u", v[0], v[1], v[2], v[3], v[4]);
            free(ln), free(blk), free(all), free(dsc);
        }
        free(ring), free(sq), free(speck), free(sn);
    }
    {
        size_t w = 64, h = 48, ch = 2, n = w * h, k;
        float *truth = malloc(n * ch * sizeof(float)), *im = malloc(n * ch * sizeof(float)), *out = NULL;
        bool *hole = malloc(n);
        for (i = 0; i < n; i++) {
            truth[i * 2] = (float)(i % w) / (float)w, truth[i * 2 + 1] = 1.0f;
        }
        for (k = 0; k < 2; k++) {
            for (i = 0; i < n; i++) {
                size_t x = i % w, y = i / w;
                hole[i] = k == 0 ? (y == 24 && x >= 10 && x < 54) : (x >= 24 && x < 40 && y >= 16 && y < 32);
            }
            memcpy(im, truth, n * ch * sizeof(float));
            for (i = 0; i < n; i++) {
                if (hole[i]) {
                    im[i * ch] = 1.0f;
                }
            }
            show_opt("remove", k == 0 ? "gradient/thin" : "gradient/wide",
                     pc_algo_remove(w, h, ch, im, n * ch, hole, n, 7, NULL, &out, NULL), &out, n * ch);
            free(out), out = NULL;
        }
        free(truth), free(im), free(hole);
    }
    {
        float *tex = texture(48, 40), *out = NULL, *im2 = img(10, 8, 2, 1);
        bool *hole = disc(48, 40, 24, 20, 6), *all = fill_mask(80, true);
        show_opt("remove", "tex",
                 pc_algo_remove(48, 40, 3, tex, 48 * 40 * 3, hole, 48 * 40, 3, NULL, &out, NULL), &out,
                 48 * 40 * 3);
        free(out), out = NULL;
        {
            size_t w = 900, h = 700, n = w * h;
            float *st = malloc(n * sizeof(float));
            bool *dg = malloc(n);
            for (i = 0; i < n; i++) {
                size_t x = i % w, y = i / w;
                st[i] = 0.3f + 0.4f * (float)((x / 7 + y / 5) % 2);
                dg[i] = fabsf((float)x - (float)y * 1.2f - 20.0f) < 3.0f && y >= 40 && y < 680;
            }
            show_opt("remove", "tiles", pc_algo_remove(w, h, 1, st, n, dg, n, 3, NULL, &out, NULL), &out, n);
            free(out), out = NULL;
            free(st), free(dg);
        }
        show_opt("remove", "allhole", pc_algo_remove(10, 8, 2, im2, 160, all, 80, 1, NULL, &out, NULL), &out,
                 160);
        free(out);
        free(tex), free(hole), free(im2), free(all);
    }
}

static void masked_line(const char *key, size_t w, size_t h, size_t ch, const float *im, const uint8_t *px,
                        const pc_algo_complete_params *p)
{
    float *o = NULL;
    pc_status st = pc_algo_inpaint_complete_masked(w, h, ch, im, w * h * ch, px, w * h, p, NULL, &o, NULL);
    show_opt("inpaint", key, st, &o, w * h * ch);
    free(o);
}

static void owned_line(const char *key, size_t w, size_t h, size_t ch, const float *im, const uint8_t *px,
                       const pc_algo_complete_params *p)
{
    float *v = malloc(w * h * ch * sizeof(float));
    bool filled = false;
    pc_status st;
    memcpy(v, im, w * h * ch * sizeof(float));
    st = pc_algo_inpaint_complete_masked_inplace(w, h, ch, v, w * h * ch, px, w * h, p, NULL, &filled, NULL);
    CHECK(st == PC_OK);
    line("inpaint %s %s%016llx", key, filled ? "" : "back ", (unsigned long long)of(v, w * h * ch));
    free(v);
}

static void masked_vectors(void)
{
    static const size_t CHS[3] = {1, 3, 4};
    const pc_algo_adapt adapts[3] = {PC_ALGO_ADAPT_NONE, {0.9f, 1.1f, 0.04f}, {0.67f, 1.5f, 0.14f}};
    pc_algo_complete_params d = pc_algo_complete_params_default(), p;
    size_t w = 64, h = 52, ci, ai, i;
    bool *hole = disc(w, h, 30, 26, 8);
    uint8_t *px = roles(w, h, hole), *known = malloc(w * h);
    char key[64];
    for (ci = 0; ci < 3; ci++) {
        size_t ch = CHS[ci];
        float *im = busy(w, h, ch, 6);
        for (ai = 0; ai < 3; ai++) {
            p = d, p.adapt = adapts[ai];
            (void)snprintf(key, sizeof key, "masked/%zu/%zu", ch, ai);
            masked_line(key, w, h, ch, im, px, &p);
        }
        p = d, p.adapt = (pc_algo_adapt){0.8f, 1.25f, 0.08f}, p.patch_radius = 2, p.seed = 9;
        (void)snprintf(key, sizeof key, "owned/%zu", ch);
        owned_line(key, w, h, ch, im, px, &p);
        free(im);
    }
    {
        float *im = busy(w, h, 3, 6);
        for (i = 0; i < w * h; i++) {
            known[i] = px[i] == PC_ALGO_PX_SOURCE ? PC_ALGO_PX_KNOWN : px[i];
        }
        masked_line("masked/known", w, h, 3, im, known, &d);
        owned_line("owned/known", w, h, 3, im, known, &d);
        free(im);
    }
    free(hole), free(px), free(known);
    /* The thread-independence cases (results_do_not_depend_on_the_thread_count). */
    for (ci = 0; ci < 2; ci++) {
        size_t bw = 300, bh = 260, ch = ci == 0 ? 3 : 4;
        float *im = busy(bw, bh, ch, 3);
        bool *bhole = disc(bw, bh, 140, 130, 40);
        uint8_t *bpx = roles(bw, bh, bhole);
        p = d, p.adapt = ci == 0 ? PC_ALGO_ADAPT_NONE : (pc_algo_adapt){0.9f, 1.1f, 0.04f};
        (void)snprintf(key, sizeof key, "busy/%zu", ch);
        masked_line(key, bw, bh, ch, im, bpx, &p);
        free(im), free(bhole), free(bpx);
    }
}

static void proximity_vectors(void)
{
    static const size_t CHS[4] = {1, 3, 4, 5}, RINGS[6] = {1, 3, 5, 1, 0, 2};
    static const int DEPTHS[3] = {8, 16, 32};
    static const int32_t RADII[6] = {5, 17, 30, 49, 2, 1000};
    size_t w = 61, h = 49, ci, i, ri;
    int di, kind;
    for (ci = 0; ci < 4; ci++) {
        size_t ch = CHS[ci];
        float *im = malloc(w * h * ch * sizeof(float));
        for (di = 0; di < 3; di++) {
            uint64_t hh = H0;
            for (i = 0; i < w * h * ch; i++) {
                float value = (float)((i * 101 + i / (w * ch) * 37) % 251) / 250.0f;
                im[i] = DEPTHS[di] == 8    ? roundf(value * 255.0f) / 255.0f
                        : DEPTHS[di] == 16 ? roundf(value * 65535.0f) / 65535.0f
                                           : value * 2.0f - 0.5f;
            }
            for (kind = 0; kind < 5; kind++) {
                bool *hole = malloc(w * h);
                for (i = 0; i < w * h; i++) {
                    size_t x = i % w, y = i / w;
                    hole[i] = kind == 0   ? (x >= 27 && x < 32 && y >= 20 && y < 25)
                              : kind == 1 ? (x < 2 && y < 3)
                              : kind == 2 ? ((x == 11 && y == 12) || (x == 38 && y == 30))
                              : kind == 3 ? (x == 30 && y == 24)
                                          : false;
                }
                for (ri = 0; ri < 6; ri++) {
                    bool found = false;
                    int32_t dx = 0, dy = 0;
                    CHECK(pc_algo_inpaint_best_offset(w, h, ch, im, w * h * ch, hole, w * h, RINGS[ri],
                                                      RADII[ri], &found, &dx, &dy, NULL) == PC_OK);
                    if (found) {
                        hi32(&hh, 1), hi32(&hh, dx), hi32(&hh, dy);
                    } else {
                        hi32(&hh, 0);
                    }
                }
                free(hole);
            }
            line("inpaint proximity/%zu/%d %016llx", ch, DEPTHS[di], (unsigned long long)hh);
        }
        free(im);
    }
    {
        size_t pw = 40, ph = 35, ch = 3, k;
        float vals[4] = {0.5f, NAN, INFINITY, -INFINITY}, *im = malloc(pw * ph * ch * sizeof(float));
        bool *hole = rect_mask(pw, ph, 18, 16, 22, 20), found;
        for (k = 0; k < 4; k++) {
            char s[32];
            int32_t dx = 0, dy = 0;
            for (i = 0; i < pw * ph * ch; i++) {
                im[i] = vals[k];
            }
            CHECK(pc_algo_inpaint_best_offset(pw, ph, ch, im, pw * ph * ch, hole, pw * ph, 3, 20, &found, &dx,
                                              &dy, NULL) == PC_OK);
            offset_str(s, sizeof s, found, dx, dy);
            line("inpaint proximity/value%zu %s", k, s);
        }
        free(im), free(hole);
    }
}

static void carve_line(const char *key, pc_status st, float *const *op, const size_t *np)
{
    const float *o = *op;
    size_t n = *np;
    CHECK(st == PC_OK);
    line("seam %s %016llx", key, st == PC_OK ? (unsigned long long)of(o, n) : 0ull);
}

static void seam_proxy_vectors(void)
{
    size_t w = 96, h = 2100, n = 0;
    float *sm = smooth(w, h, 3, 1), *prot = noise(w * h, 3, 5), *n1 = noise(80 * 2048, 7, 9);
    float *tall = smooth(2100, 100, 1, 2), *o = NULL;
    carve_line("proxy/plain", pc_algo_seam_carve_width(w, h, 3, sm, w * h * 3, NULL, 30, NULL, &o, &n, NULL),
               &o, &n);
    free(o), o = NULL;
    carve_line("proxy/protect",
               pc_algo_seam_carve_width(w, h, 3, sm, w * h * 3, prot, 31, NULL, &o, &n, NULL), &o, &n);
    free(o), o = NULL;
    carve_line("proxy/noise",
               pc_algo_seam_carve_width(80, 2048, 1, n1, 80 * 2048, NULL, 16, NULL, &o, &n, NULL), &o, &n);
    free(o), o = NULL;
    carve_line("proxy/height",
               pc_algo_seam_carve(2100, 100, 1, tall, 2100 * 100, NULL, 2100, 20, NULL, &o, &n, NULL), &o,
               &n);
    free(o);
    free(sm), free(prot), free(n1), free(tall);
}

static uint32_t WSEED;
static int32_t wnext(uint32_t n)
{
    WSEED ^= WSEED << 13;
    WSEED ^= WSEED >> 17;
    WSEED ^= WSEED << 5;
    return (int32_t)(WSEED % n);
}

static void ca_more_vectors(void)
{
    uint64_t hh = H0;
    size_t i, k;
    WSEED = 12345;
    for (k = 0; k < 400; k++) {
        pc_rect canvas, hole;
        int32_t x0, y0;
        canvas.x0 = wnext(50) - 25;
        canvas.y0 = wnext(50) - 25;
        canvas.x1 = 100 + wnext(2000);
        canvas.y1 = 100 + wnext(1500);
        x0 = wnext(2200) - 100;
        y0 = wnext(1700) - 100;
        hole.x0 = x0, hole.y0 = y0;
        hole.x1 = x0 + wnext(400);
        hole.y1 = y0 + wnext(300);
        hrect(&hh, pc_algo_ca_sampling_window(hole, canvas));
    }
    hrect(&hh, pc_algo_ca_sampling_window((pc_rect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX},
                                          (pc_rect){INT32_MIN, 0, INT32_MAX, 10}));
    hrect(&hh, pc_algo_ca_sampling_window((pc_rect){0, 0, 3, 3},
                                          (pc_rect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX}));
    hrect(&hh, pc_algo_ca_sampling_window((pc_rect){-5, -5, 0, 0}, (pc_rect){0, 0, 10, 10}));
    hrect(&hh, pc_algo_ca_sampling_window((pc_rect){100, 100, 101, 101}, (pc_rect){0, 0, 1, 1}));
    line("ca window %016llx", (unsigned long long)hh);
    {
        size_t w = 200, h = 200, n = w * h;
        float *tex = malloc(n * 3 * sizeof(float)), *im = malloc(n * 3 * sizeof(float));
        bool *hole = rect_mask(w, h, 75, 75, 125, 125), *area = NULL;
        uint64_t z = 3;
        int b;
        for (i = 0; i < n * 3; i++) {
            z = z * 6364136223846793005ull + 1442695040888963407ull;
            tex[i] = 0.2f + 0.6f * ((float)(z >> 40) / (float)(1ull << 24));
        }
        for (b = 0; b < 2; b++) {
            for (i = 0; i < n * 3; i++) {
                size_t x = (i / 3) % w;
                float sh = b ? (x >= 130 && x < 150 ? 0.55f : 0.35f) : (x < 135 ? 0.35f : 0.55f);
                im[i] = sh + (tex[i] - 0.5f) * 0.1f;
            }
            bool_line(b ? "ca auto/true" : "ca auto/false",
                      pc_algo_ca_auto_sampling(w, h, 3, im, n * 3, hole, n, NULL, &area, NULL), &area, n);
            free(area), area = NULL;
        }
        free(tex), free(im), free(hole);
    }
    {
        static const size_t CHS[3] = {1, 2, 4};
        bool *area = NULL, *hole = disc(90, 70, 40, 30, 12);
        for (k = 0; k < 3; k++) {
            float *im = smooth(90, 70, CHS[k], 3);
            char key[64];
            (void)snprintf(key, sizeof key, "ca auto/smooth%zu", CHS[k]);
            bool_line(
                key,
                pc_algo_ca_auto_sampling(90, 70, CHS[k], im, 90 * 70 * CHS[k], hole, 6300, NULL, &area, NULL),
                &area, 6300);
            free(area), area = NULL;
            free(im);
        }
        free(hole);
    }
    {
        float *big = img(500, 420, 3, 2), *tiny = img(12, 10, 3, 1);
        bool *bh = rect_mask(500, 420, 200, 150, 260, 230), *th = rect_mask(12, 10, 5, 4, 7, 6);
        bool *no = fill_mask(120, false), *area = NULL;
        bool_line("ca auto/big",
                  pc_algo_ca_auto_sampling(500, 420, 3, big, 500 * 420 * 3, bh, 500 * 420, NULL, &area, NULL),
                  &area, 500 * 420);
        free(area), area = NULL;
        bool_line("ca auto/tiny", pc_algo_ca_auto_sampling(12, 10, 3, tiny, 360, th, 120, NULL, &area, NULL),
                  &area, 120);
        free(area), area = NULL;
        bool_line("ca auto/nohole",
                  pc_algo_ca_auto_sampling(12, 10, 3, tiny, 360, no, 120, NULL, &area, NULL), &area, 120);
        free(area);
        free(big), free(tiny), free(bh), free(th), free(no);
    }
    hh = H0;
    for (k = 0; k <= 12; k++) {
        pc_algo_adapt a = pc_algo_ca_color_amount((uint8_t)k);
        hf1(&hh, a.gain_lo), hf1(&hh, a.gain_hi), hf1(&hh, a.bias);
    }
    line("ca amount %016llx", (unsigned long long)hh);
    {
        static const char *const KEYS[4] = {"none", "c3", "c10", "wide"};
        pc_algo_adapt as[4];
        float *from = smooth(30, 10, 3, 1), *to = img(25, 10, 3, 4);
        size_t nf = 30 * 10 * 3, nt = 25 * 10 * 3, ch, pi;
        as[0] = PC_ALGO_ADAPT_NONE, as[1] = pc_algo_ca_color_amount(3), as[2] = pc_algo_ca_color_amount(10);
        as[3] = (pc_algo_adapt){0.1f, 9.0f, 2.0f};
        for (i = 0; i < nt; i++) {
            to[i] = to[i] * 0.5f + 0.1f;
        }
        for (k = 0; k < 4; k++) {
            hh = H0;
            for (ch = 1; ch <= 3; ch++) {
                const float *fs[4] = {from, to, from, from}, *ts[4] = {to, from, to, to};
                size_t fl[4] = {nf, nt, 7, 0}, tl[4] = {nt, nf, nt, nt};
                for (pi = 0; pi < 4; pi++) {
                    float gb[3][2];
                    CHECK(pc_algo_ca_level_transform(ch, fs[pi], fl[pi], ts[pi], tl[pi], as[k], gb, NULL) ==
                          PC_OK);
                    hf(&hh, &gb[0][0], ch * 2);
                }
            }
            line("ca level/%s %016llx", KEYS[k], (unsigned long long)hh);
        }
        free(from), free(to);
    }
}

#endif
