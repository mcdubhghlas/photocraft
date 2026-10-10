/* Bit-exact parity with the L10 part of crates/algo (exif, features, panorama, photo_util,
 * quantize, scancrop, vectorscope): replays `rustref algo-analysis-vectors`
 * (tools/rustref/src/algo_analysis_vectors.rs) and compares every line of
 * tests/golden/algo-analysis-vectors.txt. Hashes are FNV-1a over little-endian bit patterns. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "analysis_gen.h"
#include "base/buf.h"
#include "base/bytes.h"
#include "harness.h"

#define GOLDEN "tests/golden/algo-analysis-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define PI 3.141592653589793

static pc_buf OUT;

static void line(const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    (void)pc_buf_append_str(&OUT, tmp, NULL);
    (void)pc_buf_append_byte(&OUT, '\n', NULL);
}

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}
static void hu(uint64_t *h, uint64_t u)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 8);
}
static void hf(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &v[i], 4);
        b[0] = (uint8_t)u, b[1] = (uint8_t)(u >> 8), b[2] = (uint8_t)(u >> 16), b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
    }
}
static void hf1(uint64_t *h, float v)
{
    hf(h, &v, 1);
}
static void hd(uint64_t *h, const double *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint64_t u;
        memcpy(&u, &v[i], 8);
        hu(h, u);
    }
}
static void hd1(uint64_t *h, double v)
{
    hd(h, &v, 1);
}
static void hh(uint64_t *h, const pc_algo_homography *m)
{
    hd(h, m->m, 9);
}
static void hi32(uint64_t *h, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}

/* rgba(n, seed) of the Rust module. */
static float (*rgba(size_t n, uint64_t seed))[4]
{
    pc_algo_photo_rng rng = {seed};
    float (*px)[4] = malloc((n > 0 ? n : 1) * sizeof *px);
    size_t i;
    for (i = 0; i < n; i++) {
        px[i][3] = i % 11 == 0 ? 0.2f : (i % 13 == 0 ? 0.0f : 1.0f);
        px[i][0] = (float)(pc_algo_photo_rng_next(&rng) % 1001) / 1000.0f;
        px[i][1] = (float)(pc_algo_photo_rng_next(&rng) % 1001) / 1000.0f;
        px[i][2] = (float)(pc_algo_photo_rng_next(&rng) % 1001) / 1000.0f;
    }
    return px;
}

/* ---------------------------------------------------------------------------------------- */

static void photo_util_vectors(void)
{
    uint64_t h = H0;
    size_t n, k;
    int it, i;
    for (n = 1; n < 7; n++) {
        pc_algo_photo_rng rng = {n * 31};
        for (it = 0; it < 20; it++) {
            double a[36], b[6], x[6];
            for (k = 0; k < n * n; k++) {
                a[k] = pc_algo_photo_rng_unit(&rng) * 2.0 - 1.0;
            }
            for (k = 0; k < n; k++) {
                b[k] = pc_algo_photo_rng_unit(&rng) * 10.0 - 5.0;
            }
            if (pc_algo_photo_solve_dense(n, a, b, x)) {
                hd(&h, x, n);
            } else {
                hu(&h, 7);
            }
        }
    }
    {
        double a[9] = {1.0, 2.0, 3.0, 2.0, 4.0, 6.0, 1.0, 0.0, 1.0}, b[3] = {1.0, 2.0, 3.0}, x[3];
        if (pc_algo_photo_solve_dense(3, a, b, x)) {
            hd(&h, x, 3);
        }
    }
    line("pu_solve_dense %016llx", (unsigned long long)h);
    h = H0;
    {
        const size_t iters[5] = {0, 1, 3, 10, 50};
        for (it = 0; it < 5; it++) {
            pc_algo_photo_sparse_ls sp;
            pc_algo_photo_rng rng = {5};
            const double x0[4] = {0.0, 1.0, 0.0, -1.0};
            double x[4];
            pc_algo_photo_sparse_ls_init(&sp, 4);
            for (k = 0; k < 30; k++) {
                pc_algo_photo_term row[3];
                double rhs;
                row[0].i = k % 4, row[0].c = pc_algo_photo_rng_unit(&rng);
                row[1].i = (k + 1) % 4, row[1].c = pc_algo_photo_rng_unit(&rng) - 0.5;
                row[2].i = (k * 3) % 4, row[2].c = 0.25;
                rhs = pc_algo_photo_rng_unit(&rng) * 3.0;
                CHECK(pc_algo_photo_sparse_ls_add(&sp, row, 3, rhs, 0.5 + (double)(k % 3), NULL) == PC_OK);
            }
            CHECK(pc_algo_photo_sparse_ls_solve(&sp, x0, iters[it], x, NULL) == PC_OK);
            hd(&h, x, 4);
            pc_algo_photo_sparse_ls_fini(&sp);
        }
    }
    line("pu_sparse %016llx", (unsigned long long)h);
    h = H0;
    for (i = -20; i < 20; i++) {
        float v[2];
        v[0] = pc_algo_photo_hash01((int64_t)i * 37, (int64_t)-i * 11, (uint64_t)(int64_t)i * 1000003u);
        v[1] = pc_algo_photo_hash01(i, i, UINT64_MAX - (uint64_t)(int64_t)i);
        hf(&h, v, 2);
    }
    for (i = 0; i <= 200; i++) {
        float v = (float)i / 100.0f - 0.5f, w[4], s[2];
        s[0] = pc_algo_photo_srgb_to_linear(v), s[1] = pc_algo_photo_linear_to_srgb(v);
        hf(&h, s, 2);
        pc_algo_photo_catmull_rom(v, w);
        hf(&h, w, 4);
    }
    {
        float buf[40];
        for (i = 0; i < 40; i++) {
            buf[i] = (float)(i * 7 % 11) / 10.0f;
        }
        for (i = 0; i < 60; i++) {
            hf1(&h, pc_algo_photo_bilinear(buf, 40, 5, 4, 2, (size_t)(i % 2), (float)i * 0.17f - 1.0f,
                                           (float)i * 0.09f - 0.5f));
        }
    }
    line("pu_misc %016llx", (unsigned long long)h);
}

/* ---------------------------------------------------------------------------------------- */

static void info_hash(uint64_t *h, const pc_algo_camera_info *i)
{
    const pc_algo_exif_str *ss[3] = {&i->make, &i->model, &i->lens};
    const pc_algo_exif_num *ns[5] = {&i->exposure_time, &i->f_number, &i->iso, &i->focal_length,
                                     &i->focal_length_35mm};
    int k;
    for (k = 0; k < 3; k++) {
        if (ss[k]->s != NULL) {
            hu(h, ss[k]->len);
            fnv(h, ss[k]->s, ss[k]->len);
        } else {
            hu(h, UINT64_MAX);
        }
    }
    for (k = 0; k < 5; k++) {
        if (ns[k]->set) {
            hd1(h, ns[k]->v);
        } else {
            hu(h, UINT64_MAX - 1);
        }
    }
}

static void read_hash(uint64_t *h, const uint8_t *b, size_t n)
{
    pc_algo_camera_info r;
    uint8_t *copy = malloc(n > 0 ? n : 1); /* exact size, so sanitizers see any over-read */
    memcpy(copy, b, n);
    CHECK(pc_algo_exif_read(copy, n, &r, NULL) == PC_OK);
    info_hash(h, &r);
    pc_algo_camera_info_fini(&r);
    free(copy);
}

static pc_algo_exif_str xs(char *s, size_t n)
{
    pc_algo_exif_str r;
    r.s = s, r.len = n;
    return r;
}

static void exif_vectors(void)
{
    static char lens40[41], model2[] = " x \xc3\xa9\0y ";
    pc_algo_camera_info infos[5];
    pc_algo_photo_rng rng = {99};
    uint64_t h;
    int k;
    memset(infos, 0, sizeof infos);
    memset(lens40, 'L', 40);
    infos[0].make = xs("Generic", 7), infos[0].model = xs("Camera 1", 8), infos[0].lens = xs("24-70mm", 7);
    infos[0].exposure_time = (pc_algo_exif_num){true, 0.004},
    infos[0].f_number = (pc_algo_exif_num){true, 8.0};
    infos[0].iso = (pc_algo_exif_num){true, 200.0}, infos[0].focal_length = (pc_algo_exif_num){true, 35.0};
    infos[0].focal_length_35mm = (pc_algo_exif_num){true, 52.0};
    infos[1].make = xs("Ab", 2), infos[1].iso = (pc_algo_exif_num){true, 70000.0};
    infos[1].exposure_time = (pc_algo_exif_num){true, 1.0 / 3.0};
    infos[2].model = xs(model2, 8), infos[2].f_number = (pc_algo_exif_num){true, -2.0};
    infos[2].focal_length_35mm = (pc_algo_exif_num){true, 0.0};
    infos[4].lens = xs(lens40, 40), infos[4].focal_length = (pc_algo_exif_num){true, 1e12};
    infos[4].iso = (pc_algo_exif_num){true, NAN};
    for (k = 0; k < 5; k++) {
        pc_buf b = PC_BUF_INIT, pre = PC_BUF_INIT;
        pc_algo_camera_info r;
        double f;
        size_t i, cut;
        uint8_t *m;
        unsigned long long fb = 0xffffffffffffffffull;
        static const uint8_t vals[7] = {0, 1, 2, 5, 0x7f, 0x80, 0xff};
        CHECK(pc_algo_exif_build(&infos[k], &b, NULL) == PC_OK);
        h = H0;
        fnv(&h, b.data, b.len);
        line("exif_build_%d %zu %016llx", k, b.len, (unsigned long long)h);
        CHECK(pc_algo_exif_read(b.data, b.len, &r, NULL) == PC_OK);
        h = H0;
        info_hash(&h, &r);
        if (pc_algo_exif_exposure_factor(&r, &f)) {
            memcpy(&fb, &f, 8);
        }
        pc_algo_camera_info_fini(&r);
        line("exif_read_%d %016llx %016llx", k, (unsigned long long)h, fb);
        h = H0;
        for (cut = 0; cut <= b.len; cut++) {
            read_hash(&h, b.data, cut);
        }
        line("exif_cut_%d %016llx", k, (unsigned long long)h);
        h = H0;
        m = malloc(b.len);
        for (i = 0; i < b.len; i++) {
            int v;
            for (v = 0; v < 7; v++) {
                memcpy(m, b.data, b.len);
                m[i] = vals[v];
                read_hash(&h, m, b.len);
            }
        }
        memcpy(m, b.data, b.len);
        if (b.len >= 4) {
            memcpy(m, "MM\0*", 4);
        }
        line("exif_mut_%d %016llx", k, (unsigned long long)h);
        h = H0;
        read_hash(&h, m, b.len);
        (void)pc_buf_append(&pre, "Exif\0\0", 6, NULL);
        (void)pc_buf_append(&pre, b.data, b.len, NULL);
        read_hash(&h, pre.data, pre.len);
        line("exif_misc_%d %016llx", k, (unsigned long long)h);
        free(m);
        pc_buf_fini(&b);
        pc_buf_fini(&pre);
    }
    h = H0;
    for (k = 0; k < 400; k++) {
        size_t n = (size_t)(pc_algo_photo_rng_next(&rng) % 200), i;
        uint8_t *b = malloc(n > 0 ? n : 1);
        for (i = 0; i < n; i++) {
            b[i] = (uint8_t)(pc_algo_photo_rng_next(&rng) % 256);
        }
        if (n >= 8) {
            uint32_t off;
            memcpy(b, k % 2 == 0 ? "II*\0" : "MM\0*", 4);
            off = (uint32_t)(pc_algo_photo_rng_next(&rng) % 24);
            if (k % 2 == 0) {
                pc_store_le32(b + 4, off);
            } else {
                pc_store_be32(b + 4, off);
            }
        }
        read_hash(&h, b, n);
        free(b);
    }
    line("exif_garbage %016llx", (unsigned long long)h);
}

static void vectorscope_vectors(void)
{
    float (*px)[4] = rgba(5000, 3), c[3], hs[2];
    pc_algo_vectorscope *v = malloc(sizeof *v);
    pc_algo_photo_rng rng = {8};
    uint64_t h = H0;
    size_t i;
    /* rgba(5000) + three extra pixels. */
    px = realloc(px, 5003 * sizeof *px);
    px[5000][0] = NAN, px[5000][1] = 0.0f, px[5000][2] = 0.0f, px[5000][3] = 1.0f;
    px[5001][0] = 2.0f, px[5001][1] = -1.0f, px[5001][2] = 0.5f, px[5001][3] = 1.0f;
    px[5002][0] = INFINITY, px[5002][1] = 0.0f, px[5002][2] = 0.0f, px[5002][3] = 1.0f;
    pc_algo_vectorscope_from_rgba((const float (*)[4])px, 5003, v);
    for (i = 0; i < PC_ALGO_VECTORSCOPE_HUES * PC_ALGO_VECTORSCOPE_SATURATIONS; i++) {
        hi32(&h, (int32_t)v->counts[i]);
    }
    line("vectorscope %llu %016llx", (unsigned long long)v->samples, (unsigned long long)h);
    h = H0;
    for (i = 0; i < 2000; i++) {
        c[0] = (float)(pc_algo_photo_rng_next(&rng) % 2001) / 1000.0f - 0.5f;
        c[1] = (float)(pc_algo_photo_rng_next(&rng) % 1001) / 1000.0f;
        c[2] = (float)(pc_algo_photo_rng_next(&rng) % 1001) / 1000.0f;
        pc_algo_vectorscope_hue_saturation(c, &hs[0], &hs[1]);
        hf(&h, hs, 2);
    }
    line("hue_saturation %016llx", (unsigned long long)h);
    free(px);
    free(v);
}

/* ---------------------------------------------------------------------------------------- */

static void corners_hash(uint64_t *h, const pc_algo_corner *c, size_t n)
{
    size_t i;
    hu(h, n);
    for (i = 0; i < n; i++) {
        float v[3] = {c[i].x, c[i].y, c[i].score};
        hf(h, v, 3);
    }
}

static void ransac_hash(uint64_t *h, pc_status st, bool found, const pc_algo_homography *m, size_t *inl,
                        size_t n, bool indices, uint64_t none)
{
    size_t i;
    CHECK(st == PC_OK);
    if (found) {
        hh(h, m);
        hu(h, n);
        for (i = 0; indices && i < n; i++) {
            hu(h, inl[i]);
        }
        free(inl);
    } else {
        hu(h, none);
    }
}

static void features_vectors(void)
{
    const size_t w = 96, hh_ = 80;
    float *img = gen_texture(w, hh_), *mov;
    bool *valid = malloc(w * hh_ * sizeof *valid);
    const size_t maxs[5] = {50, 600, 0, 10, 1000};
    const float mds[5] = {6.0f, 4.0f, 3.0f, 0.5f, 30.0f};
    const char *mdl[5] = {"6", "4", "3", "0.5", "30"};
    pc_algo_corner *c = NULL, *cm = NULL, *t = NULL;
    size_t nc = 0, ncm = 0, nt = 0, i, k;
    pc_algo_descriptor *da = NULL, *db = NULL;
    pc_algo_homography shift = {{1.0, 0.0, 5.0, 0.0, 1.0, -3.0, 0.0, 0.0, 1.0}}, inv;
    uint64_t h;
    int u;
    for (i = 0; i < w * hh_; i++) {
        valid[i] = (i % w) < 70 || (i / w) > 40;
    }
    for (k = 0; k < 5; k++) {
        h = H0;
        CHECK(pc_algo_features_harris(w, hh_, img, maxs[k], mds[k], NULL, &t, &nt, NULL) == PC_OK);
        corners_hash(&h, t, nt);
        free(t);
        CHECK(pc_algo_features_harris(w, hh_, img, maxs[k], mds[k], valid, &t, &nt, NULL) == PC_OK);
        corners_hash(&h, t, nt);
        free(t);
        line("harris_%zu_%s %016llx", maxs[k], mdl[k], (unsigned long long)h);
    }
    CHECK(pc_algo_features_harris(w, hh_, img, 60, 5.0f, NULL, &c, &nc, NULL) == PC_OK);
    for (u = 0; u < 2; u++) {
        pc_algo_descriptor *d = NULL;
        h = H0;
        CHECK(pc_algo_features_describe(w, hh_, img, c, nc, u == 1, &d, NULL) == PC_OK);
        for (i = 0; i < nc; i++) {
            for (k = 0; k < 4; k++) {
                hu(&h, d[i].bits[k]);
            }
        }
        free(d);
        line("describe_%s %016llx", u == 1 ? "true" : "false", (unsigned long long)h);
    }
    if (!pc_algo_homography_inverse(&shift, &inv)) {
        inv = PC_ALGO_HOMOGRAPHY_IDENTITY;
    }
    mov = gen_warp(w, hh_, img, &inv);
    CHECK(pc_algo_features_harris(w, hh_, mov, 60, 5.0f, NULL, &cm, &ncm, NULL) == PC_OK);
    CHECK(pc_algo_features_describe(w, hh_, img, c, nc, false, &da, NULL) == PC_OK);
    CHECK(pc_algo_features_describe(w, hh_, mov, cm, ncm, false, &db, NULL) == PC_OK);
    {
        const float ratios[3] = {0.5f, 0.8f, 1.0f};
        const char *rl[3] = {"0.5", "0.8", "1"};
        for (k = 0; k < 3; k++) {
            pc_algo_match *m = NULL;
            size_t nm = 0;
            h = H0;
            CHECK(pc_algo_features_match(db, ncm, da, nc, ratios[k], &m, &nm, NULL) == PC_OK);
            for (i = 0; i < nm; i++) {
                hu(&h, m[i].a);
                hu(&h, m[i].b);
            }
            free(m);
            line("match_%s %zu %016llx", rl[k], nm, (unsigned long long)h);
        }
    }
    free(c), free(cm), free(da), free(db), free(mov), free(valid), free(img);
    /* Fits on noisy point sets. */
    {
        pc_algo_photo_rng rng = {77};
        const size_t ns[8] = {0, 1, 2, 3, 4, 5, 8, 20};
        pc_algo_homography tt = {{1.02, 0.04, 3.0, -0.03, 0.97, -6.0, 0.0002, -0.0001, 1.0}}, m;
        h = H0;
        for (k = 0; k < 8; k++) {
            double src[20][2], dst[20][2];
            size_t n = ns[k];
            int q;
            for (i = 0; i < n; i++) {
                src[i][0] = pc_algo_photo_rng_unit(&rng) * 100.0;
                src[i][1] = pc_algo_photo_rng_unit(&rng) * 80.0;
            }
            for (i = 0; i < n; i++) {
                double x, y;
                pc_algo_homography_apply(&tt, src[i][0], src[i][1], &x, &y);
                dst[i][0] = x + pc_algo_photo_rng_unit(&rng) * 0.1;
                dst[i][1] = y - pc_algo_photo_rng_unit(&rng) * 0.1;
            }
            for (q = 0; q < 3; q++) {
                if (pc_algo_features_fit((pc_algo_model)q, (const double (*)[2])src, (const double (*)[2])dst,
                                         n, &m)) {
                    hh(&h, &m);
                } else {
                    hu(&h, 3);
                }
            }
            for (q = 0; q < 4; q++) {
                if (pc_algo_panorama_fit_motion((pc_algo_motion)q, (const double (*)[2])src,
                                                (const double (*)[2])dst, n, &m)) {
                    hh(&h, &m);
                } else {
                    hu(&h, 4);
                }
            }
        }
        line("fits %016llx", (unsigned long long)h);
    }
    /* RANSAC. */
    {
        pc_algo_photo_rng rng = {12};
        double src[60][2], dst[60][2];
        const uint64_t seeds[4] = {0, 1, 7, 12345};
        const size_t iters[3] = {1, 50, 300}, ks[4] = {1, 2, 4, 60};
        const double thrs[3] = {1.0, 2.0, 0.5};
        int s, q, j;
        for (i = 0; i < 60; i++) {
            double p0 = pc_algo_photo_rng_unit(&rng) * 200.0, p1 = pc_algo_photo_rng_unit(&rng) * 150.0;
            src[i][0] = p0, src[i][1] = p1;
            if (i % 5 == 0) {
                dst[i][0] = pc_algo_photo_rng_unit(&rng) * 200.0;
                dst[i][1] = pc_algo_photo_rng_unit(&rng) * 150.0;
            } else {
                dst[i][0] = p0 * 0.99 - p1 * 0.02 + 7.0;
                dst[i][1] = p0 * 0.02 + p1 * 0.99 - 3.0;
            }
        }
        h = H0;
        for (s = 0; s < 4; s++) {
            for (q = 0; q < 3; q++) {
                for (j = 0; j < 3; j++) {
                    pc_algo_homography m;
                    size_t *inl = NULL, ninl = 0;
                    bool found = false;
                    pc_status st = pc_algo_features_ransac((pc_algo_model)q, (const double (*)[2])src,
                                                           (const double (*)[2])dst, 60, iters[j], thrs[j],
                                                           seeds[s], &found, &m, &inl, &ninl, NULL);
                    ransac_hash(&h, st, found, &m, inl, ninl, true, 9);
                }
            }
            for (q = 0; q < 4; q++) {
                for (j = 0; j < 4; j++) {
                    pc_algo_homography m;
                    size_t *inl = NULL, ninl = 0;
                    bool found = false;
                    pc_status st = pc_algo_panorama_ransac_motion((pc_algo_motion)q, (const double (*)[2])src,
                                                                  (const double (*)[2])dst, ks[j], 40, 1.5,
                                                                  seeds[s], &found, &m, &inl, &ninl, NULL);
                    ransac_hash(&h, st, found, &m, inl, ninl, false, 10);
                }
            }
            for (j = 0; j < 3; j++) {
                for (q = 0; q < 3; q++) {
                    pc_algo_homography m;
                    size_t *inl = NULL, ninl = 0;
                    bool found = false;
                    pc_status st = pc_algo_features_ransac((pc_algo_model)q, (const double (*)[2])src,
                                                           (const double (*)[2])dst, ks[j], 5, 1.5, seeds[s],
                                                           &found, &m, &inl, &ninl, NULL);
                    ransac_hash(&h, st, found, &m, inl, ninl, false, 11);
                }
            }
        }
        line("ransac %016llx", (unsigned long long)h);
    }
    /* Registration. */
    {
        const size_t rw = 192, rh = 160;
        float *ref = gen_texture(rw, rh);
        double a = 4.0 * (PI / 180.0), c4 = cos(a) * 1.03, s4 = sin(a) * 1.03, cx = 96.0, cy = 80.0;
        pc_algo_homography tfs[3] = {
            {{1.0, 0.0, 9.0, 0.0, 1.0, -6.0, 0.0, 0.0, 1.0}},
            {{c4, -s4, cx - c4 * cx + s4 * cy, s4, c4, cy - s4 * cx - c4 * cy, 0.0, 0.0, 1.0}},
            {{1.03, 0.02, -4.0, -0.01, 0.98, 3.0, 0.0001, 0.00005, 1.0}}};
        const char *mn[3] = {"Translation", "Similarity", "Homography"};
        bool *mask = malloc(rw * rh * sizeof *mask);
        int q, kk;
        for (i = 0; i < rw * rh; i++) {
            mask[i] = (i % rw) > 10;
        }
        for (kk = 0; kk < 3; kk++) {
            pc_algo_homography iv;
            float *mv;
            if (!pc_algo_homography_inverse(&tfs[kk], &iv)) {
                iv = PC_ALGO_HOMOGRAPHY_IDENTITY;
            }
            mv = gen_warp(rw, rh, ref, &iv);
            for (q = 0; q < 3; q++) {
                pc_algo_homography m;
                size_t n = 0;
                bool found = false;
                h = H0;
                CHECK(pc_algo_features_register(rw, rh, ref, mv, NULL, NULL, (pc_algo_model)q, &found, &m, &n,
                                                NULL) == PC_OK);
                if (found) {
                    hh(&h, &m);
                    hu(&h, n);
                } else {
                    hu(&h, 1);
                }
                CHECK(pc_algo_features_register(rw, rh, ref, mv, mask, mask, (pc_algo_model)q, &found, &m, &n,
                                                NULL) == PC_OK);
                if (found) {
                    hh(&h, &m);
                    hu(&h, n);
                } else {
                    hu(&h, 1);
                }
                line("register_%d_%s %016llx", kk, mn[q], (unsigned long long)h);
            }
            free(mv);
        }
        free(mask), free(ref);
    }
}

/* ---------------------------------------------------------------------------------------- */

static void placement_hash(uint64_t *h, const pc_algo_placement *p)
{
    double v[4] = {p->focal, p->center[0], p->center[1], p->k1};
    hd(h, v, 4);
    hh(h, &p->h);
    hu(h, (uint64_t)p->projection);
}

static void placements_vectors(void)
{
    uint64_t h = H0;
    const double k1s[3] = {0.0, 0.05, -0.2}, focals[2] = {50.0, 400.0};
    int pr, a, b, i;
    for (pr = 0; pr < 3; pr++) {
        for (a = 0; a < 3; a++) {
            for (b = 0; b < 2; b++) {
                pc_algo_placement p = {(pc_algo_projection)pr,
                                       focals[b],
                                       {200.0, 150.0},
                                       k1s[a],
                                       {{0.98, -0.1, 30.0, 0.1, 0.98, -12.0, 0.0001, 0.0, 1.0}}},
                                  s;
                double bd[4];
                for (i = 0; i < 40; i++) {
                    double x = (double)i * 11.0 - 20.0, y = (double)i * 7.5 - 10.0, q[2], r[2], u, v;
                    pc_algo_placement_project(&p, x, y, q);
                    hd(&h, q, 2);
                    if (pc_algo_placement_unproject(&p, q[0], q[1], &r[0], &r[1])) {
                        hd(&h, r, 2);
                    } else {
                        hu(&h, 2);
                    }
                    pc_algo_placement_forward(&p, x, y, &u, &v);
                    hd1(&h, u), hd1(&h, v);
                    if (pc_algo_placement_inverse(&p, u, v, &r[0], &r[1])) {
                        hd(&h, r, 2);
                    } else {
                        hu(&h, 3);
                    }
                    if (pc_algo_placement_unproject(&p, x * 3.0, y * 3.0, &r[0], &r[1])) {
                        hd(&h, r, 2);
                    } else {
                        hu(&h, 4);
                    }
                }
                s = pc_algo_placement_scaled(&p, 2.5);
                placement_hash(&h, &s);
                s = pc_algo_placement_translated(&p, -3.0, 4.5);
                placement_hash(&h, &s);
                pc_algo_placement_bounds(&p, 400.0, 300.0, bd);
                hd(&h, bd, 4);
            }
        }
    }
    line("placements %016llx", (unsigned long long)h);
    h = H0;
    {
        pc_algo_photo_rng rng = {4};
        const double ca[2] = {160.0, 120.0}, cb[2] = {150.0, 100.0};
        for (i = 0; i < 200; i++) {
            double m[9], f;
            pc_algo_homography hm;
            int k;
            for (k = 0; k < 9; k++) {
                m[k] = k == 8 ? 1.0 : pc_algo_photo_rng_unit(&rng) * 2.0 - 1.0;
            }
            hm = (pc_algo_homography){
                {m[0], m[1], m[2] * 50.0, m[3], m[4], m[5] * 50.0, m[6] * 0.01, m[7] * 0.01, m[8]}};
            if (pc_algo_panorama_focal_from_homography(&hm, ca, cb, &f)) {
                hd1(&h, f);
            } else {
                hu(&h, 5);
            }
        }
    }
    line("focal_from_h %016llx", (unsigned long long)h);
}

static void feats_hash(uint64_t *h, const pc_algo_image_features *f)
{
    size_t i, k;
    hu(h, f->w), hu(h, f->h), hu(h, f->n);
    for (i = 0; i < f->n; i++) {
        hd(h, f->points[i], 2);
    }
    for (i = 0; i < f->n; i++) {
        for (k = 0; k < 4; k++) {
            hu(h, f->desc[i].bits[k]);
        }
    }
}

static void alignment_line(const char *name, pc_status st, bool found, pc_algo_alignment *a)
{
    uint64_t h = H0;
    size_t i;
    CHECK(st == PC_OK);
    if (!found) {
        line("%s none", name);
        return;
    }
    hu(&h, (uint64_t)a->layout);
    hu(&h, a->reference);
    for (i = 0; i < a->n; i++) {
        if (a->placed[i]) {
            placement_hash(&h, &a->placements[i]);
        } else {
            hu(&h, 6);
        }
    }
    hd1(&h, a->focal), hd1(&h, a->k1), hd1(&h, a->rms);
    for (i = 0; i < a->npairs; i++) {
        hu(&h, a->pairs[i].a), hu(&h, a->pairs[i].b), hu(&h, a->pairs[i].inliers);
    }
    line("%s %s %016llx", name, pc_algo_layout_name(a->layout), (unsigned long long)h);
    pc_algo_alignment_fini(a);
}

static void panorama_vectors(void)
{
    size_t sw = 640, sh = 300, tw = 260, th = 220, i, nm = 0, k;
    float *sc = gen_scene(sw, sh, 3), *imgs[3];
    const double offs[3][2] = {{20.0, 30.0}, {190.0, 40.0}, {360.0, 25.0}};
    pc_algo_prepared_image prep[3];
    pc_algo_image_features feats[3], feats2[3];
    pc_algo_pair_match *m = NULL;
    uint64_t h = H0;
    const char *refl[3] = {"Some(1)", "None", "Some(9)"};
    int l, c;
    for (i = 0; i < 3; i++) {
        imgs[i] = gen_view(sc, sw, sh, tw, th, gen_map_offset, offs[i]);
        hf(&h, imgs[i], tw * th);
        prep[i].w = tw, prep[i].h = th, prep[i].luma = imgs[i], prep[i].valid = NULL;
    }
    line("pano_views %016llx", (unsigned long long)h);
    CHECK(pc_algo_panorama_detect_many(prep, 3, 700, feats, NULL) == PC_OK);
    h = H0;
    for (i = 0; i < 3; i++) {
        feats_hash(&h, &feats[i]);
    }
    line("pano_detect %016llx", (unsigned long long)h);
    CHECK(pc_algo_panorama_match_pairs(feats, 3, &m, &nm, NULL) == PC_OK);
    h = H0;
    for (i = 0; i < nm; i++) {
        hu(&h, m[i].a), hu(&h, m[i].b);
        for (k = 0; k < m[i].n; k++) {
            hd(&h, m[i].pts_a[k], 2);
        }
        for (k = 0; k < m[i].n; k++) {
            hd(&h, m[i].pts_b[k], 2);
        }
        hh(&h, &m[i].homography);
    }
    line("pano_match %zu %016llx", nm, (unsigned long long)h);
    for (l = 0; l < 6; l++) {
        for (c = 0; c < 3; c++) {
            pc_algo_align_options o;
            pc_algo_alignment a;
            bool found = false;
            char name[128];
            pc_status st;
            memset(&o, 0, sizeof o);
            o.layout = (pc_algo_layout)l;
            o.has_reference = c != 1, o.reference = c == 0 ? 1 : 9;
            o.has_focal = c != 0, o.focal = c == 1 ? 300.0 : 0.5;
            o.geometric = c == 1;
            st = pc_algo_panorama_align(feats, 3, m, nm, &o, &found, &a, NULL);
            (void)snprintf(name, sizeof name, "pano_align_%s_%s_%s", pc_algo_layout_name(o.layout), refl[c],
                           o.geometric ? "true" : "false");
            alignment_line(name, st, found, &a);
        }
    }
    pc_algo_pair_matches_free(m, nm);
    for (i = 0; i < 3; i++) {
        pc_algo_image_features_fini(&feats[i]);
        free(imgs[i]);
    }
    free(sc);
    /* Rotating camera. */
    {
        const double f = 300.0, pans[3] = {-25.0, 0.0, 25.0};
        const pc_algo_layout ls[4] = {PC_ALGO_LAYOUT_AUTO, PC_ALGO_LAYOUT_CYLINDRICAL,
                                      PC_ALGO_LAYOUT_SPHERICAL, PC_ALGO_LAYOUT_PERSPECTIVE};
        size_t rsw = 1400, rsh = 500, rtw = 320, rth = 240;
        float *rsc = gen_scene(rsw, rsh, 9), *v;
        bool *valid = malloc(rtw * rth * sizeof *valid);
        pc_algo_image_features fv;
        const double off[2] = {30.0, 20.0};
        for (i = 0; i < 3; i++) {
            double ctx[6] = {f, (double)rtw, (double)rth, (double)rsw, (double)rsh, pans[i]};
            v = gen_view(rsc, rsw, rsh, rtw, rth, gen_map_pan, ctx);
            CHECK(pc_algo_panorama_detect(rtw, rth, v, NULL, 700, &feats2[i], NULL) == PC_OK);
            free(v);
        }
        CHECK(pc_algo_panorama_match_pairs(feats2, 3, &m, &nm, NULL) == PC_OK);
        line("pano_rot_match %zu", nm);
        for (l = 0; l < 4; l++) {
            for (c = 0; c < 2; c++) {
                pc_algo_align_options o;
                pc_algo_alignment a;
                bool found = false;
                char name[128];
                pc_status st;
                memset(&o, 0, sizeof o);
                o.layout = ls[l];
                o.geometric = c == 1;
                st = pc_algo_panorama_align(feats2, 3, m, nm, &o, &found, &a, NULL);
                (void)snprintf(name, sizeof name, "pano_rot_%s_%s", pc_algo_layout_name(o.layout),
                               o.geometric ? "true" : "false");
                alignment_line(name, st, found, &a);
            }
        }
        pc_algo_pair_matches_free(m, nm);
        for (i = 0; i < 3; i++) {
            pc_algo_image_features_fini(&feats2[i]);
        }
        /* Wide pan: perspective is too stretched, so Auto falls back to the cylinder. */
        {
            const double wp[5] = {-70.0, -35.0, 0.0, 35.0, 70.0};
            const pc_algo_layout wl[3] = {PC_ALGO_LAYOUT_AUTO, PC_ALGO_LAYOUT_PERSPECTIVE,
                                          PC_ALGO_LAYOUT_SPHERICAL};
            pc_algo_image_features f3[5];
            for (i = 0; i < 5; i++) {
                double ctx[6] = {250.0, (double)rtw, (double)rth, (double)rsw, (double)rsh, wp[i]};
                v = gen_view(rsc, rsw, rsh, rtw, rth, gen_map_pan, ctx);
                CHECK(pc_algo_panorama_detect(rtw, rth, v, NULL, 700, &f3[i], NULL) == PC_OK);
                free(v);
            }
            CHECK(pc_algo_panorama_match_pairs(f3, 5, &m, &nm, NULL) == PC_OK);
            line("pano_wide_match %zu", nm);
            for (l = 0; l < 3; l++) {
                for (c = 0; c < 2; c++) {
                    pc_algo_align_options o;
                    pc_algo_alignment a;
                    bool found = false;
                    char name[128];
                    pc_status st;
                    memset(&o, 0, sizeof o);
                    o.layout = wl[l];
                    o.geometric = c == 1;
                    st = pc_algo_panorama_align(f3, 5, m, nm, &o, &found, &a, NULL);
                    (void)snprintf(name, sizeof name, "pano_wide_%s_%s", pc_algo_layout_name(o.layout),
                                   o.geometric ? "true" : "false");
                    alignment_line(name, st, found, &a);
                }
            }
            pc_algo_pair_matches_free(m, nm);
            for (i = 0; i < 5; i++) {
                pc_algo_image_features_fini(&f3[i]);
            }
        }
        for (i = 0; i < rtw * rth; i++) {
            valid[i] = (i % rtw) > 40;
        }
        v = gen_view(rsc, rsw, rsh, rtw, rth, gen_map_offset, off);
        CHECK(pc_algo_panorama_detect(rtw, rth, v, valid, 300, &fv, NULL) == PC_OK);
        h = H0;
        feats_hash(&h, &fv);
        line("pano_detect_valid %016llx", (unsigned long long)h);
        pc_algo_image_features_fini(&fv);
        free(v), free(valid), free(rsc);
    }
}

/* ---------------------------------------------------------------------------------------- */

typedef struct roi_buf {
    pc_algo_roi_image im;
    float *px, *alpha;
} roi_buf;

static void make_roi(roi_buf *r, int32_t x0, int32_t y0, size_t w, size_t h, size_t ch, uint64_t seed,
                     float gain)
{
    pc_algo_photo_rng rng = {seed};
    size_t i;
    r->px = malloc(w * h * ch * sizeof *r->px);
    r->alpha = malloc(w * h * sizeof *r->alpha);
    for (i = 0; i < w * h * ch; i++) {
        size_t p = i / ch;
        int32_t x = (int32_t)(p % w) + x0, y = (int32_t)(p / w) + y0;
        r->px[i] =
            (0.3f + 0.2f * fabsf(sinf((float)x * 0.05f) * cosf((float)y * 0.07f)) + 0.05f * (float)(i % ch)) *
            gain;
    }
    for (i = 0; i < w * h; i++) {
        r->alpha[i] = i % 97 == 0 ? 0.5f : (pc_algo_photo_rng_next(&rng) % 50 == 0 ? 0.0f : 1.0f);
    }
    r->im.x0 = x0, r->im.y0 = y0, r->im.w = w, r->im.h = h, r->im.ch = ch;
    r->im.px = r->px, r->im.alpha = r->alpha;
}

static void blend_vectors(void)
{
    size_t ch, i;
    int k;
    for (ch = 1; ch <= 3; ch += 2) {
        roi_buf rb[3];
        pc_algo_roi_image imgs[3];
        pc_algo_placement pls[3];
        size_t sizes[3][2];
        const bool combos[5][3] = {{true, false, true},
                                   {true, true, true},
                                   {false, true, true},
                                   {true, true, false},
                                   {false, false, true}};
        make_roi(&rb[0], 0, 0, 200, 100, ch, 1, 1.0f);
        make_roi(&rb[1], 100, 10, 200, 100, ch, 2, 1.3f);
        make_roi(&rb[2], 50, 60, 150, 90, ch, 3, 0.8f);
        for (k = 0; k < 3; k++) {
            const pc_algo_roi_image *im = &rb[k].im;
            imgs[k] = *im;
            pls[k].projection = PC_ALGO_PROJECTION_PLANE, pls[k].focal = 200.0;
            pls[k].center[0] = (double)im->w / 2.0, pls[k].center[1] = (double)im->h / 2.0, pls[k].k1 = 0.0;
            pls[k].h = (pc_algo_homography){{1.0, 0.0, (double)im->x0 + (double)im->w / 2.0, 0.0, 1.0,
                                             (double)im->y0 + (double)im->h / 2.0, 0.0, 0.0, 1.0}};
            sizes[k][0] = im->w, sizes[k][1] = im->h;
        }
        for (k = 0; k < 5; k++) {
            pc_algo_photometric p;
            uint64_t h = H0;
            float fs[2];
            CHECK(pc_algo_panorama_photometric(imgs, 3, combos[k][2] ? pls : NULL,
                                               combos[k][2] ? (const size_t (*)[2])sizes : NULL, combos[k][0],
                                               combos[k][1], &p, NULL) == PC_OK);
            hf(&h, p.gains, 3 * ch);
            hd(&h, p.vignette, 2);
            fs[0] = pc_algo_photometric_factor(&p, 1, 0, 0.3);
            fs[1] = pc_algo_photometric_factor(&p, 2, ch - 1, 0.9);
            hf(&h, fs, 2);
            line("photometric_%zu_%s_%s_%s %016llx", ch, combos[k][0] ? "true" : "false",
                 combos[k][1] ? "true" : "false", combos[k][2] ? "true" : "false", (unsigned long long)h);
            pc_algo_photometric_fini(&p);
        }
        for (k = 0; k < 3; k++) {
            free(rb[k].px), free(rb[k].alpha);
        }
    }
    /* Seams. */
    {
        const size_t w = 150, h = 60;
        float *luma[3];
        bool *cover[3], *vcover[2];
        const size_t orders[3][3] = {{0, 1, 2}, {2, 0, 1}, {1, 1, 0}};
        const char *ol[3] = {"[0, 1, 2]", "[2, 0, 1]", "[1, 1, 0]"};
        const size_t vo[2] = {0, 1};
        int32_t *lab = NULL;
        uint64_t hs;
        for (k = 0; k < 3; k++) {
            luma[k] = malloc(w * h * sizeof *luma[k]);
            cover[k] = malloc(w * h * sizeof *cover[k]);
            for (i = 0; i < w * h; i++) {
                luma[k][i] = 0.4f + 0.1f * (float)k + ((i % w) % 17 == (size_t)k * 3 ? 0.4f : 0.0f) +
                             (float)(i / w) * 0.01f;
                cover[k][i] =
                    k == 0 ? i % w < 70 : (k == 1 ? i % w >= 40 && i % w < 120 : i % w >= 90 || i / w > 45);
            }
        }
        for (k = 0; k < 3; k++) {
            CHECK(pc_algo_panorama_seam_labels(w, h, (const float *const *)luma, (const bool *const *)cover,
                                               3, orders[k], 3, &lab, NULL) == PC_OK);
            hs = H0;
            for (i = 0; i < w * h; i++) {
                hi32(&hs, lab[i]);
            }
            free(lab);
            line("seams_%s %016llx", ol[k], (unsigned long long)hs);
        }
        for (k = 0; k < 2; k++) {
            vcover[k] = malloc(w * h * sizeof *vcover[k]);
            for (i = 0; i < w * h; i++) {
                vcover[k][i] = k == 0 ? i / w < 35 : i / w >= 20;
            }
        }
        CHECK(pc_algo_panorama_seam_labels(w, h, (const float *const *)luma, (const bool *const *)vcover, 2,
                                           vo, 2, &lab, NULL) == PC_OK);
        hs = H0;
        for (i = 0; i < w * h; i++) {
            hi32(&hs, lab[i]);
        }
        free(lab);
        line("seams_vertical %016llx", (unsigned long long)hs);
        for (k = 0; k < 3; k++) {
            free(luma[k]), free(cover[k]);
        }
        free(vcover[0]), free(vcover[1]);
    }
    /* Multi-band. */
    for (ch = 1; ch <= 3; ch += 2) {
        roi_buf rb[3];
        pc_algo_roi_image imgs[3];
        float *wts[3];
        const size_t levels[5] = {0, 1, 2, 4, 7};
        make_roi(&rb[0], 0, 0, 80, 40, ch, 4, 1.0f);
        make_roi(&rb[1], 40, 5, 80, 40, ch, 5, 1.2f);
        make_roi(&rb[2], -10, 20, 50, 30, ch, 6, 0.9f);
        for (k = 0; k < 3; k++) {
            imgs[k] = rb[k].im;
            wts[k] = malloc(imgs[k].w * imgs[k].h * sizeof *wts[k]);
            for (i = 0; i < imgs[k].w * imgs[k].h; i++) {
                wts[k][i] = (float)((i * (size_t)(k + 3)) % 7) / 6.0f;
            }
        }
        for (k = 0; k < 5; k++) {
            float *out = NULL, *cov = NULL;
            uint64_t h = H0;
            CHECK(pc_algo_panorama_multiband(120, 50, imgs, 3, (const float *const *)wts, levels[k], &out,
                                             &cov, NULL) == PC_OK);
            if (out != NULL) {
                hf(&h, out, 120 * 50 * ch);
                hf(&h, cov, 120 * 50);
            }
            free(out), free(cov);
            line("multiband_%zu_%zu %016llx", ch, levels[k], (unsigned long long)h);
        }
        for (k = 0; k < 3; k++) {
            free(rb[k].px), free(rb[k].alpha), free(wts[k]);
        }
    }
    line("blend_levels %zu %zu %zu %zu", pc_algo_panorama_blend_levels(10, 10),
         pc_algo_panorama_blend_levels(100, 40), pc_algo_panorama_blend_levels(5000, 3000),
         pc_algo_panorama_blend_levels(33, 2000));
}

/* ---------------------------------------------------------------------------------------- */

static void pal_hash(uint64_t *h, const uint8_t *p, size_t n)
{
    hu(h, n);
    fnv(h, p, 3 * n);
}

static void quantize_vectors(void)
{
    static const char *kn[8] = {"Exact",   "SystemMac",  "SystemWindows", "Web",
                                "Uniform", "Perceptual", "Selective",     "Adaptive"};
    static const char *dn[4] = {"None", "Diffusion", "Pattern", "Noise"};
    pc_algo_palette p;
    uint64_t h = H0;
    const size_t us[11] = {0, 1, 7, 8, 26, 27, 64, 100, 216, 256, 1000};
    size_t i, k;
    float (*grad)[4] = malloc(1600 * sizeof *grad), (*noisy)[4] = rgba(3000, 21),
          (*few)[4] = malloc(500 * sizeof *few);
    const float fc[3][3] = {{0.1f, 0.2f, 0.3f}, {0.9f, 0.1f, 0.1f}, {0.5f, 0.5f, 0.5f}};
    float (*imgs[3])[4];
    const size_t ns[3] = {1600, 3000, 500};
    const char *names[3] = {"grad", "noisy", "few"};
    pc_algo_quantize_web_palette(&p);
    pal_hash(&h, &p.c[0][0], p.n);
    pc_algo_quantize_mac_palette(&p);
    pal_hash(&h, &p.c[0][0], p.n);
    pc_algo_quantize_windows_palette(&p);
    pal_hash(&h, &p.c[0][0], p.n);
    for (i = 0; i < 11; i++) {
        uint8_t *u = NULL;
        size_t nu = 0;
        CHECK(pc_algo_quantize_uniform_palette(us[i], &u, &nu, NULL) == PC_OK);
        pal_hash(&h, u, nu);
        free(u);
    }
    line("fixed_palettes %016llx", (unsigned long long)h);
    for (i = 0; i < 1600; i++) {
        grad[i][0] = (float)(i % 40) / 39.0f, grad[i][1] = (float)(i / 40) / 39.0f, grad[i][2] = 0.3f;
        grad[i][3] = 1.0f;
    }
    for (i = 0; i < 500; i++) {
        few[i][0] = fc[i % 3][0], few[i][1] = fc[i % 3][1], few[i][2] = fc[i % 3][2], few[i][3] = 1.0f;
    }
    imgs[0] = grad, imgs[1] = noisy, imgs[2] = few;
    for (i = 0; i < 3; i++) {
        for (k = 0; k < 8; k++) {
            const size_t colors[9] = {0, 2, 3, 8, 16, 64, 255, 256, 999};
            size_t c;
            int f;
            h = H0;
            for (c = 0; c < 9; c++) {
                for (f = 0; f < 4; f++) {
                    pc_error e;
                    if (pc_algo_quantize_build_palette((const float (*)[4])imgs[i], ns[i],
                                                       (pc_algo_palette_kind)k, colors[c], (pc_algo_forced)f,
                                                       &p, &e) == PC_OK) {
                        pal_hash(&h, &p.c[0][0], p.n);
                    } else {
                        fnv(&h, e.msg, strlen(e.msg));
                    }
                }
            }
            line("palette_%s_%s %016llx", names[i], kn[k], (unsigned long long)h);
        }
    }
    CHECK(pc_algo_quantize_build_palette((const float (*)[4])noisy, 3000, PC_ALGO_PALETTE_ADAPTIVE, 16,
                                         PC_ALGO_FORCED_NONE, &p, NULL) == PC_OK);
    {
        const float amounts[4] = {0.0f, 0.5f, 1.0f, 7.0f};
        const char *al[4] = {"0", "0.5", "1", "7"};
        const size_t ws[5] = {60, 60, 0, 7, 1};
        const bool ht[5] = {false, true, true, false, true};
        const size_t tv[5] = {0, 3, 300, 0, 15};
        float (*px)[4] = malloc(3000 * sizeof *px);
        uint8_t *idx = malloc(3000);
        int d, a, j;
        for (d = 0; d < 4; d++) {
            for (a = 0; a < 4; a++) {
                for (j = 0; j < 5; j++) {
                    char tl[32];
                    memcpy(px, noisy, 3000 * sizeof *px);
                    CHECK(pc_algo_quantize(px, 3000, ws[j], &p.c[0][0], p.n, (pc_algo_dither)d, amounts[a],
                                           ht[j], tv[j], idx, NULL) == PC_OK);
                    h = H0;
                    fnv(&h, idx, 3000);
                    hf(&h, &px[0][0], 3000 * 4);
                    if (ht[j]) {
                        (void)snprintf(tl, sizeof tl, "Some(%zu)", tv[j]);
                    } else {
                        (void)snprintf(tl, sizeof tl, "None");
                    }
                    line("quantize_%s_%s_%zu_%s %016llx", dn[d], al[a], ws[j], tl, (unsigned long long)h);
                }
            }
        }
        free(px), free(idx);
    }
    {
        static const char *shapes[7] = {"round", "ellipse", "line", "square", "diamond", "cross", "bogus"};
        const float cells[4] = {6.0f, 1.0f, 13.5f, 4.0f}, angles[4] = {45.0f, -30.0f, 0.0f, 400.0f};
        const size_t ws[3] = {50, 37, 1};
        pc_algo_bitmap_method ms[3 + 28];
        float *gray = malloc(50 * 37 * sizeof *gray), *g = malloc(50 * 37 * sizeof *g);
        size_t nmeth = 0, s, c, w;
        memset(ms, 0, sizeof ms);
        ms[nmeth++].kind = PC_ALGO_BITMAP_THRESHOLD;
        ms[nmeth++].kind = PC_ALGO_BITMAP_PATTERN;
        ms[nmeth++].kind = PC_ALGO_BITMAP_DIFFUSION;
        for (s = 0; s < 7; s++) {
            for (c = 0; c < 4; c++) {
                ms[nmeth].kind = PC_ALGO_BITMAP_HALFTONE;
                ms[nmeth].cell = cells[c], ms[nmeth].angle = angles[c];
                ms[nmeth].shape = pc_algo_halftone_shape_from_id(shapes[s]);
                nmeth++;
            }
        }
        for (i = 0; i < 50 * 37; i++) {
            gray[i] = ((float)(i % 50) / 49.0f) * 0.8f + (float)((i / 50) % 5) * 0.05f;
        }
        for (k = 0; k < nmeth; k++) {
            for (w = 0; w < 3; w++) {
                memcpy(g, gray, 50 * 37 * sizeof *g);
                CHECK(pc_algo_quantize_to_bitmap(g, 50 * 37, ws[w], ms[k], NULL) == PC_OK);
                h = H0;
                hf(&h, g, 50 * 37);
                line("bitmap_%zu_%zu %016llx", k, ws[w], (unsigned long long)h);
            }
        }
        free(gray), free(g);
    }
    h = H0;
    for (i = 0; i < 9; i++) {
        for (k = 0; k < 9; k++) {
            hf1(&h, pc_algo_quantize_bayer8(k, i));
        }
    }
    line("bayer8 %016llx", (unsigned long long)h);
    free(grad), free(noisy), free(few);
}

/* ---------------------------------------------------------------------------------------- */

static void photo_line(const char *name, size_t w, size_t h, const float (*px)[4])
{
    pc_algo_found_photo *f = NULL;
    size_t n = 0, i;
    uint64_t hs = H0;
    CHECK(pc_algo_scancrop_find_photos(w, h, px, w * h, &f, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        double v[6] = {f[i].center[0], f[i].center[1], f[i].width, f[i].height, f[i].angle, f[i].area},
               c[4][2];
        int k;
        hd(&hs, v, 6);
        pc_algo_found_photo_corners(&f[i], c);
        for (k = 0; k < 4; k++) {
            hd(&hs, c[k], 2);
        }
    }
    free(f);
    line("%s %zu %016llx", name, n, (unsigned long long)hs);
}

static void scancrop_vectors(void)
{
    const pc_algo_found_photo truth[3] = {{{150.0, 120.0}, 180.0, 120.0, 7.0, 0.0},
                                          {{450.0, 130.0}, 160.0, 130.0, -12.0, 0.0},
                                          {{300.0, 330.0}, 220.0, 110.0, 3.0, 0.0}};
    const pc_algo_found_photo big[3] = {{{500.0, 400.0}, 700.0, 500.0, 30.0, 0.0},
                                        {{1300.0, 600.0}, 500.0, 800.0, -44.0, 0.0},
                                        {{900.0, 1000.0}, 300.0, 200.0, 89.0, 0.0}};
    float (*px)[4];
    size_t i;
    px = gen_scan(600, 440, truth, 3);
    photo_line("scancrop_basic", 600, 440, (const float (*)[4])px);
    free(px);
    px = gen_scan(1700, 1200, big, 3);
    photo_line("scancrop_big", 1700, 1200, (const float (*)[4])px);
    free(px);
    px = gen_scan(300, 200, truth, 1);
    photo_line("scancrop_one", 300, 200, (const float (*)[4])px);
    free(px);
    px = rgba(120 * 90, 5);
    photo_line("scancrop_noise", 120, 90, (const float (*)[4])px);
    free(px);
    px = malloc(300 * 200 * sizeof *px);
    for (i = 0; i < 300 * 200; i++) {
        px[i][0] = px[i][1] = px[i][2] = px[i][3] = 1.0f;
    }
    photo_line("scancrop_blank", 300, 200, (const float (*)[4])px);
    free(px);
}

/* ---------------------------------------------------------------------------------------- */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    long n;
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t)n + 2);
        if (data != NULL) {
            *len = fread(data, 1, (size_t)n, f);
            data[*len] = '\0';
        }
    }
    (void)fclose(f);
    return data;
}

static void test_analysis_parity(void)
{
    size_t glen = 0, lines = 0;
    char *golden = read_file(GOLDEN, &glen), *want, *got, *wsave = NULL, *gsave = NULL;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    pc_buf_init(&OUT);
    photo_util_vectors();
    exif_vectors();
    vectorscope_vectors();
    features_vectors();
    placements_vectors();
    panorama_vectors();
    blend_vectors();
    quantize_vectors();
    scancrop_vectors();
    want = strtok_r(golden, "\n", &wsave);
    got = strtok_r((char *)OUT.data, "\n", &gsave);
    while (want != NULL || got != NULL) {
        lines++;
        if (want == NULL || got == NULL || strcmp(want, got) != 0) {
            PC_T_FAIL("line %zu:\n  want %.400s\n  got  %.400s", lines, want != NULL ? want : "(end)",
                      got != NULL ? got : "(end)");
        } else {
            pc_t_checks++;
        }
        want = want != NULL ? strtok_r(NULL, "\n", &wsave) : NULL;
        got = got != NULL ? strtok_r(NULL, "\n", &gsave) : NULL;
    }
    pc_buf_fini(&OUT);
    free(golden);
}

int main(int argc, char **argv)
{
    RUN(test_analysis_parity);
    return pc_t_finish();
}
