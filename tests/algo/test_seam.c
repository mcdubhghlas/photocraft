/* Seam carving (src/algo/seam.c): the crates/algo/src/seam.rs tests (including the straightforward
 * reference carver as the oracle and the pinned output bits) plus never-crash cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

#define PROTECT_ENERGY 1.0e4f

/* ---- the reference implementation the in-place carver replaced ---------------------------------- */

static float *ref_energy(size_t w, size_t h, size_t ch, const float *img)
{
    float *e = calloc(w * h + 1, sizeof(float));
    size_t x, y, c;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t xl = x > 0 ? x - 1 : 0, xr = x + 1 < w - 1 ? x + 1 : w - 1;
            size_t yu = y > 0 ? y - 1 : 0, yd = y + 1 < h - 1 ? y + 1 : h - 1;
            float s = 0.0f;
            for (c = 0; c < ch; c++) {
                float v = img[(y * w + x) * ch + c];
                s += fabsf(img[(y * w + xr) * ch + c] - v) + fabsf(v - img[(y * w + xl) * ch + c]);
                s += fabsf(img[(yd * w + x) * ch + c] - v) + fabsf(v - img[(yu * w + x) * ch + c]);
            }
            e[y * w + x] = s * 0.5f;
        }
    }
    return e;
}

static uint32_t total_key(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

static size_t *ref_find_seam(size_t w, size_t h, const float *energy)
{
    float *cost = malloc(w * sizeof(float)), *next = malloc(w * sizeof(float)), *t;
    uint8_t *from = calloc(w * h, 1);
    size_t *seam = calloc(h, sizeof(size_t)), x, y, best = 0;
    memcpy(cost, energy, w * sizeof(float));
    for (y = 1; y < h; y++) {
        for (x = 0; x < w; x++) {
            float b = cost[x];
            uint8_t d = 1;
            if (x > 0 && cost[x - 1] < b) {
                b = cost[x - 1], d = 0;
            }
            if (x + 1 < w && cost[x + 1] < b) {
                b = cost[x + 1], d = 2;
            }
            next[x] = b + energy[y * w + x];
            from[y * w + x] = d;
        }
        t = cost, cost = next, next = t;
    }
    for (x = 1; x < w; x++) {
        if (total_key(cost[x]) < total_key(cost[best])) {
            best = x;
        }
    }
    x = best;
    for (y = h; y-- > 0;) {
        seam[y] = x;
        if (y > 0) {
            x = from[y * w + x] == 0 ? x - 1 : (from[y * w + x] == 2 ? x + 1 : x);
        }
    }
    free(cost), free(next), free(from);
    return seam;
}

static float *ref_remove_seam(size_t w, size_t h, size_t ch, const float *img, const size_t *seam)
{
    float *o = malloc(((w - 1) * h * ch + 1) * sizeof(float));
    size_t y, k = 0;
    for (y = 0; y < h; y++) {
        const float *row = &img[y * w * ch];
        memcpy(&o[k], row, seam[y] * ch * sizeof(float));
        k += seam[y] * ch;
        memcpy(&o[k], row + (seam[y] + 1) * ch, (w - seam[y] - 1) * ch * sizeof(float));
        k += (w - seam[y] - 1) * ch;
    }
    return o;
}

static void protect_e(float *e, const float *p, size_t n)
{
    size_t i;
    for (i = 0; p != NULL && i < n; i++) {
        e[i] += fminf(fmaxf(p[i], 0.0f), 1.0f) * PROTECT_ENERGY;
    }
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

static float *ref_carve_width(size_t w, size_t h, size_t ch, const float *img, const float *protect,
                              size_t new_w, size_t *out_w)
{
    float *cur = malloc((w * h * ch + 1) * sizeof(float)), *prot = NULL;
    size_t cw = w;
    new_w = new_w > 1 ? new_w : 1;
    memcpy(cur, img, w * h * ch * sizeof(float));
    *out_w = w;
    if (new_w == w || w == 0 || h == 0) {
        return cur;
    }
    if (protect != NULL) {
        prot = malloc(w * h * sizeof(float));
        memcpy(prot, protect, w * h * sizeof(float));
    }
    if (new_w < w) {
        while (cw > new_w) {
            float *e = ref_energy(cw, h, ch, cur), *n;
            size_t *seam;
            protect_e(e, prot, cw * h);
            seam = ref_find_seam(cw, h, e);
            n = ref_remove_seam(cw, h, ch, cur, seam);
            free(cur), cur = n;
            if (prot != NULL) {
                n = ref_remove_seam(cw, h, 1, prot, seam);
                free(prot), prot = n;
            }
            free(e), free(seam);
            cw--;
        }
        free(prot);
        *out_w = cw;
        return cur;
    }
    while (cw < new_w) {
        size_t k = new_w - cw < (cw / 2 > 1 ? cw / 2 : 1) ? new_w - cw : (cw / 2 > 1 ? cw / 2 : 1);
        size_t *idx = malloc(h * cw * sizeof(size_t)), *dup = malloc(h * k * sizeof(size_t)), added = 0,
               tw = cw;
        size_t x, y, q, nw;
        float *tmp = malloc((cw * h * ch + 1) * sizeof(float)), *tprot = NULL, *out, *nprot = NULL, *o, *np;
        memcpy(tmp, cur, cw * h * ch * sizeof(float));
        if (prot != NULL) {
            tprot = malloc(cw * h * sizeof(float));
            memcpy(tprot, prot, cw * h * sizeof(float));
        }
        for (y = 0; y < h; y++) {
            for (x = 0; x < cw; x++) {
                idx[y * cw + x] = x;
            }
        }
        for (q = 0; q < k && tw > 0; q++) {
            float *e = ref_energy(tw, h, ch, tmp), *n;
            size_t *seam;
            protect_e(e, tprot, tw * h);
            seam = ref_find_seam(tw, h, e);
            for (y = 0; y < h; y++) {
                /* idx[y].remove(sx): the row holds tw live entries. */
                size_t *row = &idx[y * cw];
                dup[y * k + added] = row[seam[y]];
                memmove(row + seam[y], row + seam[y] + 1, (tw - seam[y] - 1) * sizeof(size_t));
            }
            n = ref_remove_seam(tw, h, ch, tmp, seam);
            free(tmp), tmp = n;
            if (tprot != NULL) {
                n = ref_remove_seam(tw, h, 1, tprot, seam);
                free(tprot), tprot = n;
            }
            free(e), free(seam);
            tw--;
            added++;
        }
        free(idx), free(tmp), free(tprot);
        if (added == 0) {
            free(dup);
            break;
        }
        nw = cw + added;
        out = malloc((nw * h * ch + 1) * sizeof(float));
        if (prot != NULL) {
            nprot = malloc(nw * h * sizeof(float));
        }
        o = out, np = nprot;
        for (y = 0; y < h; y++) {
            size_t *d = &dup[y * k], di = 0, c;
            const float *row = &cur[y * cw * ch];
            qsort(d, added, sizeof *d, cmp_size);
            for (x = 0; x < cw; x++) {
                memcpy(o, &row[x * ch], ch * sizeof(float));
                o += ch;
                if (np != NULL) {
                    *np++ = prot[y * cw + x];
                }
                while (di < added && d[di] == x) {
                    size_t xr = x + 1 < cw - 1 ? x + 1 : cw - 1;
                    for (c = 0; c < ch; c++) {
                        *o++ = (row[x * ch + c] + row[xr * ch + c]) * 0.5f;
                    }
                    if (np != NULL) {
                        *np++ = prot[y * cw + x];
                    }
                    di++;
                }
            }
        }
        free(dup), free(cur), free(prot);
        cur = out, prot = nprot, cw = nw;
    }
    free(prot);
    *out_w = cw;
    return cur;
}

static float *transpose(size_t w, size_t h, size_t ch, const float *img)
{
    float *o = malloc((w * h * ch + 1) * sizeof(float));
    size_t x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            memcpy(&o[(x * h + y) * ch], &img[(y * w + x) * ch], ch * sizeof(float));
        }
    }
    return o;
}

static float *ref_carve(size_t w, size_t h, size_t ch, const float *img, const float *protect, size_t new_w,
                        size_t new_h)
{
    size_t ww, hh;
    float *wide, *prot_t = NULL, *t, *tall, *res;
    new_w = new_w > 1 ? new_w : 1;
    new_h = new_h > 1 ? new_h : 1;
    wide = ref_carve_width(w, h, ch, img, protect, new_w, &ww);
    if (new_h == h) {
        return wide;
    }
    if (protect != NULL) {
        float *r = NULL;
        CHECK(pc_algo_seam_resize_bilinear(w, h, 1, protect, w * h, new_w, h, &r, NULL) == PC_OK);
        prot_t = transpose(new_w, h, 1, r);
        free(r);
    }
    t = transpose(new_w, h, ch, wide);
    tall = ref_carve_width(h, new_w, ch, t, prot_t, new_h, &hh);
    res = transpose(new_h, new_w, ch, tall);
    free(wide), free(prot_t), free(t), free(tall);
    return res;
}

/* ---- tests ---------------------------------------------------------------------------------------- */

static float *noise(size_t len, uint32_t levels, uint32_t seed)
{
    float *v = malloc((len + 1) * sizeof(float));
    uint32_t s = seed | 1u;
    size_t i;
    for (i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        v[i] = (float)(s % levels) / (float)(levels - 1);
    }
    return v;
}

static uint32_t SEED;
static size_t next_rand(size_t n)
{
    SEED ^= SEED << 13;
    SEED ^= SEED >> 17;
    SEED ^= SEED << 5;
    return (size_t)SEED % n;
}

static float *carve(size_t w, size_t h, size_t ch, const float *img, const float *protect, size_t nw,
                    size_t nh, size_t *len)
{
    float *o = NULL;
    CHECK(pc_algo_seam_carve(w, h, ch, img, w * h * ch, protect, nw, nh, NULL, &o, len, NULL) == PC_OK);
    return o;
}

static void test_carver_matches_reference(void)
{
    static const size_t CHS[3] = {1, 3, 4};
    static const uint32_t LEVELS[4] = {2, 5, 255, 65536}, ELEVELS[3] = {2, 3, 1000};
    uint32_t c;
    int bad = 0;
    SEED = 1;
    for (c = 0; c < 400; c++) {
        size_t w = 1 + next_rand(24), h = 1 + next_rand(18), ch = CHS[next_rand(3)], nw, nh, len = 0;
        uint32_t lv = LEVELS[next_rand(4)];
        float *im = noise(w * h * ch, lv, 11 + c), *prot = NULL, *got, *want;
        if (next_rand(3) == 0) {
            prot = noise(w * h, 3, 101 + c);
        }
        nw = 1 + next_rand(2 * w + 2);
        nh = 1 + next_rand(2 * h + 2);
        got = carve(w, h, ch, im, prot, nw, nh, &len);
        want = ref_carve(w, h, ch, im, prot, nw, nh);
        if (len != nw * nh * ch || got == NULL || memcmp(got, want, len * sizeof(float)) != 0) {
            if (bad++ < 5) {
                PC_T_FAIL("case %u: %zux%zux%zu -> %zux%zu", c, w, h, ch, nw, nh);
            }
        } else {
            pc_t_checks++;
        }
        free(im), free(prot), free(got), free(want);
    }
    /* The seam search alone, on raw energy maps full of ties. */
    for (c = 0; c < 200; c++) {
        size_t w = 1 + next_rand(40), h = 1 + next_rand(30), *s = NULL, *r;
        float *e = noise(w * h, ELEVELS[next_rand(3)], 7 + c), *im = noise(w * h * 3, 7, 3 + c), *en = NULL,
              *ren;
        CHECK(pc_algo_seam_find(w, h, e, w * h, &s, NULL) == PC_OK);
        r = ref_find_seam(w, h, e);
        CHECK(memcmp(s, r, h * sizeof(size_t)) == 0);
        CHECK(pc_algo_seam_energy(w, h, 3, im, w * h * 3, &en, NULL) == PC_OK);
        ren = ref_energy(w, h, 3, im);
        CHECK(memcmp(en, ren, w * h * sizeof(float)) == 0);
        free(s), free(r), free(e), free(im), free(en), free(ren);
    }
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static void test_cancelled_carve_stops(void)
{
    size_t w = 40, h = 20, i, len = 0, len2 = 0;
    float *im = malloc(w * h * 3 * sizeof(float)), *o = (float *)im, *a, *b;
    pc_interrupt stop = {yes, NULL, NULL};
    pc_interrupt none = PC_INTERRUPT_NONE;
    for (i = 0; i < w * h * 3; i++) {
        im[i] = 0.5f;
    }
    CHECK(pc_algo_seam_carve(w, h, 3, im, w * h * 3, NULL, 30, 15, &stop, &o, &len, NULL) ==
          PC_ERR_CANCELLED);
    CHECK(o == (float *)im);
    CHECK(pc_algo_seam_carve(w, h, 3, im, w * h * 3, NULL, 30, 15, &none, &a, &len, NULL) == PC_OK);
    b = carve(w, h, 3, im, NULL, 30, 15, &len2);
    CHECK(len == len2 && memcmp(a, b, len * sizeof(float)) == 0);
    free(im), free(a), free(b);
}

/* A flat grey image with a salient vertical bar at x = 10..14 (red/blue columns). */
static float *bar(size_t w, size_t h)
{
    float *v = malloc(w * h * 3 * sizeof(float));
    size_t i, x, y;
    for (i = 0; i < w * h * 3; i++) {
        v[i] = 0.5f;
    }
    for (y = 0; y < h; y++) {
        for (x = 10; x < 14; x++) {
            float *p = &v[(y * w + x) * 3];
            p[0] = x % 2 == 0 ? 1.0f : 0.0f, p[1] = 0.0f, p[2] = x % 2 == 0 ? 0.0f : 1.0f;
        }
    }
    return v;
}

/* Columns that are pure red or pure blue top to bottom. */
static size_t red_columns(size_t w, size_t h, const float *im)
{
    size_t x, y, n = 0;
    for (x = 0; x < w; x++) {
        bool all = true;
        for (y = 0; y < h && all; y++) {
            const float *p = &im[(y * w + x) * 3];
            all = p[1] < 0.05f && ((p[0] > 0.95f && p[2] < 0.05f) || (p[2] > 0.95f && p[0] < 0.05f));
        }
        n += all ? 1u : 0u;
    }
    return n;
}

static void test_seam_follows_low_energy(void)
{
    float e[20];
    size_t i, *s = NULL;
    for (i = 0; i < 20; i++) {
        e[i] = i % 5 == 2 ? 0.0f : 9.0f;
    }
    CHECK(pc_algo_seam_find(5, 4, e, 20, &s, NULL) == PC_OK);
    for (i = 0; i < 4; i++) {
        CHECK_EQ_UINT(s[i], 2);
    }
    free(s);
}

static void test_shrinking_keeps_the_salient_bar(void)
{
    size_t w = 40, h = 12, len = 0;
    float *im = bar(w, h), *out = carve(w, h, 3, im, NULL, 24, h, &len), *plain = NULL;
    CHECK_EQ_UINT(len, 24 * h * 3);
    CHECK_EQ_UINT(red_columns(24, h, out), 4);
    /* Plain resampling squashes it instead. */
    CHECK(pc_algo_seam_resize_bilinear(w, h, 3, im, w * h * 3, 24, h, &plain, NULL) == PC_OK);
    CHECK(red_columns(24, h, plain) < 4);
    free(im), free(out), free(plain);
}

static void test_enlarging_duplicates_flat_areas(void)
{
    size_t w = 30, h = 8, len = 0;
    float *im = bar(w, h), *out = carve(w, h, 3, im, NULL, 50, h, &len);
    CHECK_EQ_UINT(len, 50 * h * 3);
    CHECK_EQ_UINT(red_columns(50, h, out), 4);
    free(im), free(out);
}

static void test_height_and_protection(void)
{
    size_t w = 20, h = 30, x, y, len = 0, rows = 0;
    float im[20 * 30], flat[16 * 4], protect[16 * 4], *out;
    for (x = 0; x < w * h; x++) {
        im[x] = x / w == 12 ? 1.0f : 0.2f;
    }
    /* Horizontal bar: carve height. */
    out = carve(w, h, 1, im, NULL, w, 18, &len);
    CHECK_EQ_UINT(len, w * 18);
    for (y = 0; y < 18; y++) {
        bool all = true;
        for (x = 0; x < w; x++) {
            all = all && out[y * w + x] > 0.9f;
        }
        rows += all ? 1u : 0u;
    }
    CHECK_EQ_UINT(rows, 1);
    free(out);
    /* Protecting a flat region keeps it: the seams go elsewhere. */
    for (x = 0; x < 64; x++) {
        flat[x] = x % 16 == 3 ? 0.51f : 0.5f;
        protect[x] = x % 16 == 3 ? 1.0f : 0.0f;
    }
    out = carve(16, 4, 1, flat, protect, 8, 4, &len);
    for (y = 0; y < 4; y++) {
        bool any = false;
        for (x = 0; x < 8; x++) {
            any = any || fabsf(out[y * 8 + x] - 0.51f) < 1e-6f;
        }
        CHECK(any);
    }
    free(out);
}

static uint64_t fnv(const float *v, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &v[i], 4);
        for (k = 0; k < 4; k++) {
            h ^= (u >> (8 * k)) & 0xffu;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

static void test_output_bits_are_pinned(void)
{
    static const struct {
        size_t w, h, ch;
        uint32_t levels;
        bool prot;
        size_t nw, nh;
    } CASES[8] = {{61, 47, 3, 2, false, 40, 47},   {61, 47, 3, 255, false, 61, 30},
                  {61, 47, 4, 4, true, 35, 29},    {33, 20, 1, 3, false, 80, 20},
                  {33, 20, 3, 1000, true, 50, 45}, {48, 36, 4, 7, false, 70, 18},
                  {5, 3, 1, 2, false, 1, 1},       {40, 40, 3, 2, true, 39, 41}};
    static const uint64_t PINNED[8] = {0x24ea935302c27c58ull, 0x5a6e8554a0cbebb1ull, 0x8b18d63f396e21c9ull,
                                       0x1db223ad3e6aa7c5ull, 0x721d6cc534e03fdfull, 0x961aea6de512ec7dull,
                                       0x4d25767f9dce13f5ull, 0x21579f8627d6b835ull};
    size_t i;
    for (i = 0; i < 8; i++) {
        size_t len = 0;
        float *im = noise(CASES[i].w * CASES[i].h * CASES[i].ch, CASES[i].levels, 7 + (uint32_t)i);
        float *prot = CASES[i].prot ? noise(CASES[i].w * CASES[i].h, 3, 99 + (uint32_t)i) : NULL;
        float *out = carve(CASES[i].w, CASES[i].h, CASES[i].ch, im, prot, CASES[i].nw, CASES[i].nh, &len);
        CHECK_EQ_UINT(len, CASES[i].nw * CASES[i].nh * CASES[i].ch);
        CHECK_EQ_UINT(fnv(out, len), PINNED[i]);
        free(im), free(prot), free(out);
    }
}

static void test_one_pixel_lines_enlarge_by_repeating(void)
{
    static const size_t SIZES[5][4] = {
        {1, 5, 6, 10}, {1, 17, 6, 34}, {16, 1, 21, 2}, {7, 1, 3, 8}, {2, 1, 1, 8}};
    const float col[5] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f}, px[3] = {0.7f, 0.8f, 0.9f};
    size_t len = 0, i, k;
    float *o = carve(1, 5, 1, col, NULL, 4, 5, &len);
    bool ok = len == 20;
    for (i = 0; ok && i < 20; i++) {
        ok = o[i] == col[i / 4];
    }
    CHECK(ok);
    free(o);
    o = carve(5, 1, 1, col, NULL, 5, 3, &len);
    ok = len == 15;
    for (i = 0; ok && i < 15; i++) {
        ok = o[i] == col[i % 5];
    }
    CHECK(ok);
    free(o);
    o = carve(1, 1, 3, px, NULL, 3, 2, &len);
    ok = len == 18;
    for (i = 0; ok && i < 18; i++) {
        ok = o[i] == px[i % 3];
    }
    CHECK(ok);
    free(o);
    for (k = 0; k < 5; k++) {
        size_t w = SIZES[k][0], h = SIZES[k][1], n = w * h;
        float *im = malloc(n * 4 * sizeof(float)), *prot = malloc(n * sizeof(float));
        for (i = 0; i < n * 4; i++) {
            im[i] = 0.5f;
        }
        for (i = 0; i < n; i++) {
            prot[i] = 1.0f;
        }
        o = carve(w, h, 4, im, prot, SIZES[k][2], SIZES[k][3], &len);
        CHECK_EQ_UINT(len, SIZES[k][2] * SIZES[k][3] * 4);
        free(o), free(im), free(prot);
    }
}

static void test_skin_rule(void)
{
    const float px[6] = {0.86f, 0.62f, 0.5f, 0.2f, 0.4f, 0.8f};
    float *m = NULL;
    CHECK(pc_algo_seam_skin_mask(2, 1, 3, px, 6, &m, NULL) == PC_OK);
    CHECK(m[0] == 1.0f && m[1] == 0.0f);
    free(m);
}

static void test_never_crash(void)
{
    float im[36], *o = NULL;
    size_t len = 0, *s = NULL, i;
    for (i = 0; i < 36; i++) {
        im[i] = i % 4 == 0 ? NAN : (i % 5 == 0 ? INFINITY : (float)i / 36.0f);
    }
    /* NaN / infinite pixels and protection, shrink and grow on both axes. */
    o = carve(6, 6, 1, im, im, 2, 11, &len);
    CHECK_EQ_UINT(len, 22);
    free(o);
    o = carve(6, 6, 1, im, NULL, 0, 0, &len); /* targets below 1 count as 1 */
    CHECK_EQ_UINT(len, 1);
    free(o);
    /* Empty input stays empty (the reference indexes past it). */
    CHECK(pc_algo_seam_carve(0, 5, 3, NULL, 0, NULL, 4, 9, NULL, &o, &len, NULL) == PC_OK);
    CHECK_EQ_UINT(len, 0);
    free(o);
    CHECK(pc_algo_seam_carve_width(5, 0, 3, NULL, 0, NULL, 4, NULL, &o, &len, NULL) == PC_OK);
    CHECK_EQ_UINT(len, 0);
    free(o);
    /* Bad lengths, channel counts and absurd targets. */
    CHECK(pc_algo_seam_carve(6, 6, 1, im, 35, NULL, 3, 3, NULL, &o, &len, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_seam_carve(6, 6, 0, im, 0, NULL, 3, 3, NULL, &o, &len, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_seam_carve(6, 6, 1, im, 36, NULL, SIZE_MAX, 3, NULL, &o, &len, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_seam_carve(6, 6, 1, im, 36, NULL, 3, (size_t)1 << 25, NULL, &o, &len, NULL) ==
          PC_ERR_LIMIT);
    CHECK(pc_algo_seam_carve(6, 6, 1, im, 36, NULL, (size_t)1 << 24, (size_t)1 << 24, NULL, &o, &len, NULL) ==
          PC_ERR_LIMIT);
    CHECK(pc_algo_seam_resize_bilinear(6, 6, 1, im, 36, SIZE_MAX, 2, &o, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_seam_energy(6, 6, 1, im, 3, &o, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_seam_skin_mask(6, 6, 1, im, 3, &o, NULL) == PC_ERR_INVALID);
    /* A short energy map or zero width gives all-zero seams, as the reference does. */
    CHECK(pc_algo_seam_find(6, 6, im, 3, &s, NULL) == PC_OK);
    CHECK(s[5] == 0);
    free(s);
    CHECK(pc_algo_seam_find(0, 4, NULL, 0, &s, NULL) == PC_OK);
    free(s);
    CHECK(pc_algo_seam_resize_bilinear(0, 0, 2, NULL, 0, 3, 3, &o, NULL) == PC_OK);
    CHECK(o[0] == 0.0f && o[17] == 0.0f);
    free(o);
}

int main(int argc, char **argv)
{
    RUN(test_carver_matches_reference);
    RUN(test_cancelled_carve_stops);
    RUN(test_seam_follows_low_energy);
    RUN(test_shrinking_keeps_the_salient_bar);
    RUN(test_enlarging_duplicates_flat_areas);
    RUN(test_height_and_protection);
    RUN(test_output_bits_are_pinned);
    RUN(test_one_pixel_lines_enlarge_by_repeating);
    RUN(test_skin_rule);
    RUN(test_never_crash);
    return pc_t_finish();
}
