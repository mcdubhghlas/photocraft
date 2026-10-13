/* Internals of the patch-based completion (src/algo/inpaint_int.h): the crates/algo/src/inpaint.rs
 * tests that reach into the module (onion peel, least_sum, valid sources, full against clipped
 * patch measures) and crates/algo/src/inpaint/proximity_integral/tests.rs (the hole-bounded
 * summed-area table and the proximity match it serves, against the reference's former full-table
 * search). Its ignored release timing comparison is a benchmark, not ported. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/inpaint_int.h"
#include "harness.h"

/* Smooth gradient plus deterministic fine noise (inpaint.rs `texture`). */
static float *texture(size_t w, size_t h)
{
    float *v = malloc(w * h * 3 * sizeof(float));
    pc_ip_rng rng = pc_ip_rng_new(7);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w), y = (float)(i / w);
        float n = (float)(pc_ip_rng_next(&rng) % 1000) / 1000.0f * 0.04f - 0.02f;
        v[i * 3] = 0.3f + 0.004f * x + n;
        v[i * 3 + 1] = 0.5f + 0.002f * y + n;
        v[i * 3 + 2] = 0.4f + n;
    }
    return v;
}

static bool *disc(size_t w, size_t h, float cx, float cy, float r)
{
    bool *m = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = hypotf((float)(i % w) - cx, (float)(i / w) - cy) < r;
    }
    return m;
}

/* Structure, fine noise and flat patches (inpaint.rs `busy`). */
static float *busy(size_t w, size_t h, size_t ch, uint64_t seed)
{
    float *v = malloc(w * h * ch * sizeof(float));
    pc_ip_rng rng = pc_ip_rng_new(seed);
    size_t i, c;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        bool flat = (x / 9 + y / 7) % 5 == 0;
        float n = (float)(pc_ip_rng_next(&rng) % 1000) / 1000.0f;
        for (c = 0; c < ch; c++) {
            v[i * ch + c] =
                flat
                    ? 0.25f + 0.1f * (float)c
                    : fminf(0.2f + 0.004f * (float)x + 0.1f * (float)((x + 2 * y + c) % 6) + 0.05f * n, 1.0f);
        }
    }
    return v;
}

/* Roles: a hole, a band that is context only, a corner that is no image. */
static uint8_t *roles(size_t w, size_t h, const bool *hole)
{
    uint8_t *px = malloc(w * h);
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

static bool same_bits(const float *a, const float *b, size_t n)
{
    return memcmp(a, b, n * sizeof(float)) == 0;
}

/* ---- onion peel ---------------------------------------------------------------------------------- */

/* The onion peel as first written: every source in order, every known pixel summed. */
static void onion_peel_plain(size_t w, size_t h, size_t ch, int32_t r, float *img, const uint8_t *px,
                             const pc_ip_pt *sources, size_t ns)
{
    size_t n = w * h, i, nf, fi, k, c;
    bool *known = malloc(n);
    size_t *front = malloc(n * sizeof *front);
    float *vals = malloc(n * ch * sizeof(float));
    for (i = 0; i < n; i++) {
        known[i] = px[i] == PC_ALGO_PX_KNOWN || px[i] == PC_ALGO_PX_SOURCE;
    }
    for (;;) {
        nf = 0;
        for (i = 0; i < n; i++) {
            int32_t x = (int32_t)(i % w), y = (int32_t)(i / w), dx, dy;
            bool edge = false;
            if (known[i] || px[i] != PC_ALGO_PX_HOLE) {
                continue;
            }
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    int32_t nx = x + dx, ny = y + dy;
                    edge = edge || (nx >= 0 && ny >= 0 && (size_t)nx < w && (size_t)ny < h &&
                                    known[(size_t)ny * w + (size_t)nx]);
                }
            }
            if (edge) {
                front[nf++] = i;
            }
        }
        if (nf == 0) {
            break;
        }
        for (fi = 0; fi < nf; fi++) {
            int32_t x = (int32_t)(front[fi] % w), y = (int32_t)(front[fi] / w), dx, dy;
            float best = INFINITY;
            pc_ip_pt bp = {0, 0};
            for (k = 0; k < ns; k++) {
                int32_t sx = sources[k].x, sy = sources[k].y;
                float e = 0.0f;
                size_t cnt = 0;
                for (dy = -r; dy <= r; dy++) {
                    for (dx = -r; dx <= r; dx++) {
                        int32_t tx = x + dx, ty = y + dy;
                        size_t ti, si;
                        if (tx < 0 || ty < 0 || (size_t)tx >= w || (size_t)ty >= h ||
                            !known[(size_t)ty * w + (size_t)tx]) {
                            continue;
                        }
                        ti = ((size_t)ty * w + (size_t)tx) * ch;
                        si = ((size_t)(sy + dy) * w + (size_t)(sx + dx)) * ch;
                        for (c = 0; c < ch; c++) {
                            float d = img[ti + c] - img[si + c];
                            e += d * d;
                        }
                        cnt++;
                    }
                }
                if (cnt > 0 && e / (float)cnt < best) {
                    best = e / (float)cnt;
                    bp = sources[k];
                }
            }
            memcpy(&vals[fi * ch], &img[((size_t)bp.y * w + (size_t)bp.x) * ch], ch * sizeof(float));
        }
        for (fi = 0; fi < nf; fi++) {
            memcpy(&img[front[fi] * ch], &vals[fi * ch], ch * sizeof(float));
            known[front[fi]] = true;
        }
    }
    free(known), free(front), free(vals);
}

/* #2805's case (upstream's own speed-up of the onion peel, superseded here): repeating texture with
 * many exact ties, two holes; the result must be the plain scan's to the bit. */
static void test_onion_peel_picks_exactly_what_the_plain_scan_picks(void)
{
    size_t w = 48, h = 40, ch = 3, x, y, c, k, n = w * h;
    int32_t r = 3;
    float *img = texture(w, h), *fast = malloc(n * ch * sizeof(float)),
          *plain = malloc(n * ch * sizeof(float));
    for (y = 0; y < h; y++) {
        for (x = 6; x < w / 2; x++) {
            for (c = 0; c < ch; c++) {
                img[(y * w + x) * ch + c] = img[(y * w + x % 6) * ch + c];
            }
        }
    }
    for (k = 0; k < 2; k++) {
        bool *hole = k == 0 ? disc(w, h, 20.0f, 18.0f, 7.5f) : disc(w, h, 34.0f, 22.0f, 5.0f);
        uint8_t *px = malloc(n);
        uint32_t *near = pc_ip_integral(w, h, hole);
        pc_ip_pt *sources = malloc(n * sizeof *sources);
        pc_ip_bounds hb;
        size_t ns = 0, i;
        int32_t sx, sy;
        for (i = 0; i < n; i++) {
            px[i] = hole[i] ? PC_ALGO_PX_HOLE : PC_ALGO_PX_SOURCE;
        }
        for (sy = r; sy < (int32_t)h - r; sy++) {
            for (sx = r; sx < (int32_t)w - r; sx++) {
                if (pc_ip_window_count(near, w, h, sx - r, sy - r, sx + r + 1, sy + r + 1) == 0) {
                    sources[ns++] = (pc_ip_pt){sx, sy};
                }
            }
        }
        memcpy(fast, img, n * ch * sizeof(float));
        memcpy(plain, img, n * ch * sizeof(float));
        CHECK(pc_ip_hole_bounds(w, h, px, &hb));
        CHECK(pc_ip_onion_peel(w, h, ch, r, fast, px, hb, sources, ns, NULL, NULL) == PC_OK);
        onion_peel_plain(w, h, ch, r, plain, px, sources, ns);
        CHECK(same_bits(fast, plain, n * ch));
        CHECK(!same_bits(fast, img, n * ch));
        free(hole), free(px), free(near), free(sources);
    }
    free(img), free(fast), free(plain);
}

static void test_onion_peel_takes_the_plain_scans_sources(void)
{
    static const size_t CASES[3][4] = {{44, 36, 1, 1}, {50, 40, 3, 2}, {40, 40, 4, 3}};
    size_t ci;
    for (ci = 0; ci < 3; ci++) {
        size_t w = CASES[ci][0], h = CASES[ci][1], ch = CASES[ci][2], seed = CASES[ci][3], n = w * h, i,
               ns = 0;
        float *img = busy(w, h, ch, seed), *a = malloc(n * ch * sizeof(float)),
              *b = malloc(n * ch * sizeof(float));
        bool *hole = disc(w, h, (float)w * 0.45f, (float)h * 0.5f, 7.5f), *valid = NULL;
        uint8_t *px = roles(w, h, hole);
        pc_ip_sources srcs;
        pc_ip_pt *list = NULL;
        pc_ip_bounds hb;
        /* Also an image of a few exact values, where many sources tie. */
        if (seed == 3) {
            for (i = 0; i < n * ch; i++) {
                img[i] = roundf(img[i] * 3.0f) / 3.0f;
            }
        }
        CHECK(pc_ip_valid_sources(w, h, 3, px, NULL, &valid, NULL) == PC_OK);
        CHECK(pc_ip_sources_new(valid, w, h, &srcs, NULL) == PC_OK);
        CHECK(pc_ip_sources_all(&srcs, valid, w, &list, NULL) == PC_OK);
        ns = pc_ip_sources_len(&srcs);
        memcpy(a, img, n * ch * sizeof(float));
        memcpy(b, img, n * ch * sizeof(float));
        CHECK(pc_ip_hole_bounds(w, h, px, &hb));
        CHECK(pc_ip_onion_peel(w, h, ch, 3, a, px, hb, list, ns, NULL, NULL) == PC_OK);
        onion_peel_plain(w, h, ch, 3, b, px, list, ns);
        CHECK(same_bits(a, b, n * ch));
        pc_ip_sources_free(&srcs);
        free(img), free(a), free(b), free(hole), free(valid), free(px), free(list);
    }
}

static bool holds(float e, float v, float n, bool strict)
{
    return strict ? e / n > v : e / n >= v;
}

static void test_least_sum_is_where_the_quotient_crosses(void)
{
    pc_ip_rng rng = pc_ip_rng_new(5);
    int i, s;
    bool ok = true;
    for (i = 0; i < 4000; i++) {
        float n = (float)(1 + pc_ip_rng_next(&rng) % 49), v;
        switch (i % 4) {
        case 0:
            v = 0.0f;
            break;
        case 1:
            v = (float)(pc_ip_rng_next(&rng) % 1000) * 1e-9f;
            break;
        default:
            v = (float)(pc_ip_rng_next(&rng) % 1000000) / 1000000.0f;
            break;
        }
        for (s = 0; s < 2; s++) {
            bool strict = s == 0;
            float e = pc_ip_least_sum(v, n, strict);
            if (!holds(e, v, n, strict) || !(e == 0.0f || !holds(nextafterf(e, -INFINITY), v, n, strict))) {
                if (ok) {
                    PC_T_FAIL("least_sum(%g, %g, %d) = %g", (double)v, (double)n, strict, (double)e);
                }
                ok = false;
            }
        }
    }
    CHECK(ok);
    CHECK(pc_ip_least_sum(INFINITY, 4.0f, true) == INFINITY);
}

static void test_valid_sources_are_the_wholly_copyable_patches(void)
{
    size_t w = 37, h = 29, r = 2, n = w * h, i, k, len;
    int32_t ri = (int32_t)r, x, y;
    uint32_t full = (uint32_t)((2 * r + 1) * (2 * r + 1));
    pc_ip_rng rng = pc_ip_rng_new(9);
    uint8_t *px = malloc(n);
    bool *valid = NULL, *source = malloc(n), all_match = true, nth_ok = true;
    uint32_t *ss;
    pc_ip_sources srcs;
    pc_ip_pt *list = NULL;
    for (i = 0; i < n; i++) {
        switch (pc_ip_rng_next(&rng) % 23) {
        case 0:
            px[i] = PC_ALGO_PX_HOLE;
            break;
        case 1:
            px[i] = PC_ALGO_PX_VOID;
            break;
        case 2:
            px[i] = PC_ALGO_PX_KNOWN;
            break;
        default:
            px[i] = PC_ALGO_PX_SOURCE;
            break;
        }
        source[i] = px[i] == PC_ALGO_PX_SOURCE;
    }
    CHECK(pc_ip_valid_sources(w, h, r, px, NULL, &valid, NULL) == PC_OK);
    if (valid == NULL) {
        free(px), free(source);
        return;
    }
    ss = pc_ip_integral(w, h, source);
    for (y = 0; y < (int32_t)h; y++) {
        for (x = 0; x < (int32_t)w; x++) {
            bool inside = x >= ri && y >= ri && x + ri < (int32_t)w && y + ri < (int32_t)h;
            bool want =
                inside && pc_ip_window_count(ss, w, h, x - ri, y - ri, x + ri + 1, y + ri + 1) == full;
            all_match = all_match && valid[(size_t)y * w + (size_t)x] == want;
        }
    }
    CHECK(all_match);
    /* Counting per row and block finds the centres in order. */
    CHECK(pc_ip_sources_new(valid, w, h, &srcs, NULL) == PC_OK);
    CHECK(pc_ip_sources_all(&srcs, valid, w, &list, NULL) == PC_OK);
    len = pc_ip_sources_len(&srcs);
    CHECK(len > 10);
    for (k = 0; k < len; k++) {
        pc_ip_pt p = pc_ip_sources_nth(&srcs, k, valid, w);
        nth_ok = nth_ok && p.x == list[k].x && p.y == list[k].y;
    }
    CHECK(nth_ok);
    pc_ip_sources_free(&srcs);
    free(list), free(valid), free(ss), free(px), free(source);
    /* And on a grid wider than a block, with empty blocks and rows. */
    {
        size_t ww = 300, wh = 9, wn = ww * wh;
        bool *wide = malloc(wn);
        list = NULL;
        for (i = 0; i < wn; i++) {
            wide[i] = (i % ww) % 7 == 3 && (i % ww) / 64 != 2 && i / ww != 4;
        }
        CHECK(pc_ip_sources_new(wide, ww, wh, &srcs, NULL) == PC_OK);
        CHECK(pc_ip_sources_all(&srcs, wide, ww, &list, NULL) == PC_OK);
        len = pc_ip_sources_len(&srcs);
        CHECK(len > 0);
        nth_ok = true;
        for (k = 0; k < len; k++) {
            pc_ip_pt p = pc_ip_sources_nth(&srcs, k, wide, ww);
            nth_ok = nth_ok && p.x == list[k].x && p.y == list[k].y;
        }
        CHECK(nth_ok);
        pc_ip_sources_free(&srcs);
        free(list), free(wide);
    }
}

static pc_ip_bounds grow(pc_ip_bounds b, size_t r, size_t w, size_t h)
{
    pc_ip_bounds o;
    o.x0 = b.x0 > r ? b.x0 - r : 0;
    o.y0 = b.y0 > r ? b.y0 - r : 0;
    o.x1 = b.x1 + r < w ? b.x1 + r : w;
    o.y1 = b.y1 + r < h ? b.y1 + r : h;
    return o;
}

static void test_full_patches_measure_as_clipped_ones(void)
{
    size_t w = 46, h = 38, ch = 4, r = 3, n = w * h, nall, npick = 0, k, ai, keep;
    float *img = busy(w, h, ch, 4);
    bool *hole = disc(w, h, 20, 18, 6), *valid = NULL;
    uint8_t *px = roles(w, h, hole);
    pc_ip_bounds hb, tb;
    pc_ip_sources srcs;
    pc_ip_pt *all = NULL, *picks;
    pc_algo_adapt adapts[2] = {PC_ALGO_ADAPT_NONE, {0.8f, 1.25f, 0.08f}};
    CHECK(pc_ip_hole_bounds(w, h, px, &hb));
    tb = grow(hb, r, w, h);
    CHECK(pc_ip_valid_sources(w, h, r, px, NULL, &valid, NULL) == PC_OK);
    CHECK(pc_ip_sources_new(valid, w, h, &srcs, NULL) == PC_OK);
    CHECK(pc_ip_sources_all(&srcs, valid, w, &all, NULL) == PC_OK);
    nall = pc_ip_sources_len(&srcs);
    picks = malloc((nall + 1) * sizeof *picks);
    for (k = 0; k < nall; k += 37) {
        picks[npick++] = all[k];
    }
    CHECK(npick > 5);
    for (ai = 0; ai < 2 && npick > 0; ai++) {
        for (keep = 0; keep < 2; keep++) {
            pc_ip_solver s;
            size_t area = pc_ip_barea(tb), checked = 0, pi;
            pc_ip_pt *nnf = malloc(area * sizeof *nnf);
            float *cost = malloc(area * sizeof *cost);
            bool ok = true;
            memset(&s, 0, sizeof s);
            s.w = w, s.h = h, s.ch = ch, s.r = (int32_t)r, s.img = img, s.px = px;
            s.valid = malloc(n);
            memcpy(s.valid, valid, n);
            s.targets = pc_ip_targets_around(w, r, px, hb, tb);
            s.hole_box = hb, s.tbox = tb, s.adapt = adapts[ai], s.ctl = NULL;
            CHECK(s.targets != NULL);
            CHECK(pc_ip_solver_prepare(&s, keep == 1, NULL) == PC_OK);
            for (k = 0; k < area; k++) {
                nnf[k] = picks[0];
                cost[k] = INFINITY;
            }
            CHECK(pc_ip_costs(&s, nnf, cost, NULL) == PC_OK);
            for (k = 0; k < area; k++) {
                pc_ip_pt t;
                if (!s.targets[k] || !s.full[k]) {
                    continue;
                }
                t = (pc_ip_pt){(int32_t)(tb.x0 + k % pc_ip_bw(tb)), (int32_t)(tb.y0 + k / pc_ip_bw(tb))};
                for (pi = 0; pi < npick; pi++) {
                    pc_ip_pt src = picks[pi];
                    if (pc_algo_adapt_is_none(s.adapt)) {
                        float exact = pc_ip_dist_clipped(&s, t, src, INFINITY), cuts[3];
                        float a = pc_ip_dist_full(&s, t, src, INFINITY);
                        int ci;
                        ok = ok && memcmp(&a, &exact, 4) == 0;
                        cuts[0] = exact * 0.5f, cuts[1] = exact, cuts[2] = exact * 2.0f;
                        for (ci = 0; ci < 3; ci++) {
                            float fa = pc_ip_dist_full(&s, t, src, cuts[ci]);
                            float fb = pc_ip_dist_clipped(&s, t, src, cuts[ci]);
                            ok = ok && (fa < cuts[ci]) == (fb < cuts[ci]);
                        }
                    } else {
                        float m1[4][5], m2[4][5], d1, d2;
                        memset(m1, 0, sizeof m1);
                        memset(m2, 0, sizeof m2);
                        pc_ip_moments_full(&s, k, t, src, m1, 4);
                        ok = ok && pc_ip_moments(&s, t, src, m2, 4) == (2 * r + 1) * (2 * r + 1);
                        ok = ok && memcmp(m1, m2, sizeof m1) == 0;
                        d1 = pc_ip_dist_adapted_full(&s, k, t, src);
                        d2 = pc_ip_dist_adapted(&s, t, src);
                        ok = ok && memcmp(&d1, &d2, 4) == 0;
                    }
                    checked++;
                }
            }
            CHECK(ok);
            CHECK(checked > 100);
            pc_ip_solver_fini(&s);
            free(nnf), free(cost);
        }
    }
    pc_ip_sources_free(&srcs);
    free(img), free(hole), free(valid), free(px), free(all), free(picks);
}

static void test_nth_total_and_sigma2(void)
{
    float v[7] = {3.0f, -0.0f, NAN, 0.0f, -1.0f, INFINITY, 2.0f}, out = 0.0f, s2 = 0.0f;
    float sorted[7] = {-1.0f, -0.0f, 0.0f, 2.0f, 3.0f, INFINITY, NAN};
    size_t k;
    for (k = 0; k < 7; k++) {
        CHECK(pc_ip_nth_total(v, 7, k, &out, NULL) == PC_OK);
        CHECK(memcmp(&out, &sorted[k], 4) == 0);
    }
    CHECK(pc_ip_nth_total(v, 7, 7, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_ip_sigma2(v, 0, &s2, NULL) == PC_OK && s2 == 1.0f);
    CHECK(pc_ip_sigma2(sorted, 2, &s2, NULL) == PC_OK && s2 == 1e-6f);
}

/* ---- proximity integral ------------------------------------------------------------------------- */

static bool *kind_mask(size_t w, size_t h, int kind, bool second_set)
{
    bool *m = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        if (!second_set) {
            switch (kind) {
            case 0:
                m[i] = x >= 11 && x < 16 && y >= 8 && y < 14;
                break;
            case 1:
                m[i] = x < 3 && y < 4;
                break;
            case 2:
                m[i] = x >= w - 3 && y >= h - 4;
                break;
            case 3:
                m[i] = (x == 3 && y == 8) || (x == 31 && y == 25);
                break;
            default:
                m[i] = (x * 37 + y * 71) % 17 == 0;
                break;
            }
        } else {
            switch (kind) {
            case 0:
                m[i] = x >= 27 && x < 32 && y >= 20 && y < 25;
                break;
            case 1:
                m[i] = x < 2 && y < 3;
                break;
            case 2:
                m[i] = (x == 11 && y == 12) || (x == 38 && y == 30);
                break;
            case 3:
                m[i] = x == 30 && y == 24;
                break;
            default:
                m[i] = false;
                break;
            }
        }
    }
    return m;
}

static void test_cropped_counts_match_full_image_for_clipped_and_disjoint_windows(void)
{
    static const int64_t X0[] = {-50, -1, 0, 3, 11, 15, 36, 40}, X1[] = {-2, 0, 3, 13, 16, 37, 50};
    static const int64_t Y0[] = {-30, -1, 0, 8, 13, 28, 35}, Y1[] = {-1, 0, 10, 14, 29, 40};
    size_t w = 37, h = 29;
    int kind;
    for (kind = 0; kind < 5; kind++) {
        bool *hole = kind_mask(w, h, kind, false), ok = true;
        size_t b[4], a, bb, c, d;
        pc_ip_hole_integral cropped;
        uint32_t *full = pc_ip_integral(w, h, hole);
        CHECK(pc_ip_hole_bbox(w, h, hole, b));
        CHECK(pc_ip_hole_integral_new(w, hole, w * h, b, &cropped, NULL) == PC_OK && cropped.data != NULL);
        for (a = 0; a < 8 && cropped.data != NULL; a++) {
            for (bb = 0; bb < 7; bb++) {
                for (c = 0; c < 7; c++) {
                    for (d = 0; d < 6; d++) {
                        ok = ok && pc_ip_hole_integral_count(&cropped, X0[a], Y0[c], X1[bb], Y1[d]) ==
                                       pc_ip_window_count(full, w, h, X0[a], Y0[c], X1[bb], Y1[d]);
                    }
                }
            }
        }
        CHECK(ok);
        pc_ip_hole_integral_free(&cropped);
        free(hole), free(full);
    }
}

static void test_table_size_depends_on_the_hole_bounds(void)
{
    size_t w = 1000, h = 700, i, b[4];
    bool *hole = malloc(w * h);
    pc_ip_hole_integral t;
    for (i = 0; i < w * h; i++) {
        hole[i] = i % w >= 503 && i % w < 514 && i / w >= 307 && i / w < 320;
    }
    CHECK(pc_ip_hole_bbox(w, h, hole, b));
    CHECK(pc_ip_hole_integral_new(w, hole, w * h, b, &t, NULL) == PC_OK);
    CHECK(t.data != NULL && t.w == 11 && t.h == 13 && t.len == 12 * 14);
    if (t.data != NULL) {
        CHECK_EQ_UINT(pc_ip_hole_integral_count(&t, 0, 0, 1000, 700), 143);
        CHECK_EQ_UINT(pc_ip_hole_integral_count(&t, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), 143);
        CHECK_EQ_UINT(
            pc_ip_hole_integral_count(&t, INT64_MIN / 2, INT64_MIN / 2, INT64_MAX / 2, INT64_MAX / 2), 143);
    }
    pc_ip_hole_integral_free(&t);
    {
        size_t inv[4] = {10, 5, 9, 6}, small[4] = {3, 4, 5, 6}, huge[4] = {0, 0, SIZE_MAX, 2};
        size_t far[4] = {0, SIZE_MAX - 1, 2, SIZE_MAX};
        CHECK(pc_ip_hole_integral_new(w, hole, w * h, inv, &t, NULL) == PC_OK && t.data == NULL);
        CHECK(pc_ip_hole_integral_new(w, NULL, 0, small, &t, NULL) == PC_OK && t.data == NULL);
        CHECK(pc_ip_hole_integral_new(w, NULL, 0, huge, &t, NULL) == PC_OK && t.data == NULL);
        CHECK(pc_ip_hole_integral_new(w, hole, w * h, far, &t, NULL) == PC_OK && t.data == NULL);
        CHECK(pc_ip_hole_integral_new(SIZE_MAX, hole, w * h, small, &t, NULL) == PC_OK && t.data == NULL);
    }
    free(hole);
}

/* The reference's proximity match before the hole-bounded table (old_best_offset): a full-image
 * summed-area table, otherwise the same search. */
typedef struct old_ctx {
    size_t w, h, ch;
    const float *img;
    const uint32_t *hs;
    pc_ip_pt *pts;
    size_t npts;
    int32_t x0, y0, x1, y1, max_radius;
    bool have;
    int32_t bx, by;
    float be;
} old_ctx;

static void old_consider(old_ctx *o, int32_t dx, int32_t dy)
{
    int32_t hx0 = o->x0 + dx, hy0 = o->y0 + dy, hx1 = o->x1 + dx, hy1 = o->y1 + dy;
    bool inside = hx0 >= 0 && hy0 >= 0 && hx1 <= (int32_t)o->w && hy1 <= (int32_t)o->h;
    float e = 0.0f;
    size_t i, c;
    if ((dx == 0 && dy == 0) || !inside || pc_ip_window_count(o->hs, o->w, o->h, hx0, hy0, hx1, hy1) != 0) {
        return;
    }
    for (i = 0; i < o->npts; i++) {
        size_t a = ((size_t)o->pts[i].y * o->w + (size_t)o->pts[i].x) * o->ch;
        size_t b = ((size_t)(o->pts[i].y + dy) * o->w + (size_t)(o->pts[i].x + dx)) * o->ch;
        for (c = 0; c < o->ch; c++) {
            float d = o->img[a + c] - o->img[b + c];
            e += d * d;
        }
    }
    e = e / (float)(o->npts > 1 ? o->npts : 1) *
        (1.0f + 1e-3f * (hypotf((float)dx, (float)dy) / (float)o->max_radius));
    if (!o->have || e < o->be) {
        o->have = true, o->bx = dx, o->by = dy, o->be = e;
    }
}

static bool old_best_offset(size_t w, size_t h, size_t ch, const float *img, const bool *hole, size_t ring_in,
                            int32_t max_radius, int32_t *ox, int32_t *oy)
{
    size_t b[4], np = 0, stride, i, k;
    int32_t extent, ring, x, y, step, dx, dy;
    old_ctx o;
    pc_ip_pt *all;
    if (!pc_ip_hole_bbox(w, h, hole, b)) {
        return false;
    }
    extent = (int32_t)((w > h ? w : h) < (size_t)(INT32_MAX / 4) ? (w > h ? w : h) : (size_t)(INT32_MAX / 4));
    max_radius = max_radius > 0 ? max_radius : 0;
    max_radius = max_radius < extent ? max_radius : extent;
    if (max_radius == 0) {
        return false;
    }
    ring = (int32_t)(ring_in > 1 ? (ring_in < (size_t)extent ? ring_in : (size_t)extent) : 1);
    memset(&o, 0, sizeof o);
    o.w = w, o.h = h, o.ch = ch, o.img = img, o.max_radius = max_radius;
    o.hs = pc_ip_integral(w, h, hole);
    o.x0 = (int32_t)b[0] - ring, o.y0 = (int32_t)b[1] - ring;
    o.x1 = (int32_t)b[2] + ring, o.y1 = (int32_t)b[3] + ring;
    all = malloc(w * h * sizeof *all + 1);
    for (y = o.y0 > 0 ? o.y0 : 0; y < (o.y1 < (int32_t)h ? o.y1 : (int32_t)h); y++) {
        for (x = o.x0 > 0 ? o.x0 : 0; x < (o.x1 < (int32_t)w ? o.x1 : (int32_t)w); x++) {
            if (hole[(size_t)y * w + (size_t)x]) {
                continue;
            }
            if (pc_ip_window_count(o.hs, w, h, x - ring, y - ring, x + ring + 1, y + ring + 1) > 0) {
                all[np++] = (pc_ip_pt){x, y};
            }
        }
    }
    stride = np / 2000 > 1 ? np / 2000 : 1;
    o.pts = malloc((np + 1) * sizeof *o.pts);
    for (i = 0, k = 0; i < np; i += stride) {
        o.pts[k++] = all[i];
    }
    o.npts = k;
    step = max_radius / 24 > 1 ? max_radius / 24 : 1;
    for (dy = -max_radius; dy <= max_radius; dy += step) {
        for (dx = -max_radius; dx <= max_radius; dx += step) {
            old_consider(&o, dx, dy);
        }
    }
    if (step > 1 && o.have) {
        int32_t bx = o.bx, by = o.by;
        for (dy = by - step; dy <= by + step; dy++) {
            for (dx = bx - step; dx <= bx + step; dx++) {
                old_consider(&o, dx, dy);
            }
        }
    }
    free((void *)(uintptr_t)o.hs), free(o.pts), free(all);
    if (o.have) {
        *ox = o.bx, *oy = o.by;
    }
    return o.have;
}

static bool offsets_agree(size_t w, size_t h, size_t ch, const float *img, const bool *hole, size_t ring,
                          int32_t radius)
{
    bool found = false, old;
    int32_t dx = 0, dy = 0, ox = 0, oy = 0;
    if (pc_algo_inpaint_best_offset(w, h, ch, img, w * h * ch, hole, w * h, ring, radius, &found, &dx, &dy,
                                    NULL) != PC_OK) {
        return false;
    }
    old = old_best_offset(w, h, ch, img, hole, ring, radius, &ox, &oy);
    return found == old && (!found || (dx == ox && dy == oy));
}

static void test_proximity_offsets_match_full_tables_at_several_depths_and_channels(void)
{
    static const size_t CHS[] = {1, 3, 4, 5};
    static const int DEPTHS[] = {8, 16, 32};
    static const size_t RINGS[] = {1, 3, 5, 1};
    static const int32_t RADII[] = {5, 17, 30, 49};
    size_t w = 61, h = 49, ci, i, ri;
    int di, kind;
    bool ok = true;
    for (ci = 0; ci < 4; ci++) {
        size_t ch = CHS[ci];
        float *img = malloc(w * h * ch * sizeof(float));
        for (di = 0; di < 3; di++) {
            for (i = 0; i < w * h * ch; i++) {
                float value = (float)((i * 101 + i / (w * ch) * 37) % 251) / 250.0f;
                img[i] = DEPTHS[di] == 8    ? roundf(value * 255.0f) / 255.0f
                         : DEPTHS[di] == 16 ? roundf(value * 65535.0f) / 65535.0f
                                            : value * 2.0f - 0.5f;
            }
            for (kind = 0; kind < 5; kind++) {
                bool *hole = kind_mask(w, h, kind, true);
                for (ri = 0; ri < 4; ri++) {
                    if (!offsets_agree(w, h, ch, img, hole, RINGS[ri], RADII[ri])) {
                        if (ok) {
                            PC_T_FAIL("ch %zu depth %d hole %d ring %zu radius %d", ch, DEPTHS[di], kind,
                                      RINGS[ri], RADII[ri]);
                        }
                        ok = false;
                    }
                }
                free(hole);
            }
        }
        free(img);
    }
    CHECK(ok);
}

static void test_ties_nonfinite_samples_and_full_holes_keep_the_previous_result(void)
{
    size_t w = 40, h = 35, ch = 3, n = w * h, i;
    float values[4] = {0.5f, NAN, INFINITY, -INFINITY}, *img = malloc(n * ch * sizeof(float));
    bool *hole = malloc(n), *full = malloc(n), found = true;
    int32_t dx, dy;
    int vi;
    for (i = 0; i < n; i++) {
        hole[i] = i % w >= 18 && i % w < 22 && i / w >= 16 && i / w < 20;
        full[i] = true;
    }
    for (vi = 0; vi < 4; vi++) {
        for (i = 0; i < n * ch; i++) {
            img[i] = values[vi];
        }
        CHECK(offsets_agree(w, h, ch, img, hole, 3, 20));
    }
    for (i = 0; i < n * ch; i++) {
        img[i] = 0.5f;
    }
    CHECK(pc_algo_inpaint_best_offset(w, h, ch, img, n * ch, full, n, 3, 20, &found, &dx, &dy, NULL) ==
          PC_OK);
    CHECK(!found);
    free(img), free(hole), free(full);
}

int main(int argc, char **argv)
{
    RUN(test_onion_peel_picks_exactly_what_the_plain_scan_picks);
    RUN(test_onion_peel_takes_the_plain_scans_sources);
    RUN(test_least_sum_is_where_the_quotient_crosses);
    RUN(test_valid_sources_are_the_wholly_copyable_patches);
    RUN(test_full_patches_measure_as_clipped_ones);
    RUN(test_nth_total_and_sigma2);
    RUN(test_cropped_counts_match_full_image_for_clipped_and_disjoint_windows);
    RUN(test_table_size_depends_on_the_hole_bounds);
    RUN(test_proximity_offsets_match_full_tables_at_several_depths_and_channels);
    RUN(test_ties_nonfinite_samples_and_full_holes_keep_the_previous_result);
    return pc_t_finish();
}
