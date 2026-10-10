/* Bit-exact parity with the L6 part of crates/algo (distort, stylize, pixelate, render, Blur
 * Gallery, other, video, Filter Gallery, render2, trap, warp): replays tests/golden/algo-fx-vectors.txt
 * (`rustref algo-fx-vectors`, tools/rustref/src/algo_fx_vectors.rs). Filter cases are data-driven:
 * the golden lines carry the parameters as JSON, which this test parses with the C registry and
 * runs through the same apply sequence. Hashes are FNV-1a over little-endian f32 bits. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/fx.h"
#include "base/buf.h"
#include "base/json.h"
#include "fxparity.h"
#include "fxparity2.h"
#include "harness.h"
#include "raster/pixel.h"

#define GOLDEN "tests/golden/algo-fx-vectors.txt"

static const pc_rect R = {0, 0, 40, 30};
static pc_surface SEL_GRAD;

/* The apply sequence every filter case hashes (rustref: run_case). */
static uint64_t run_case(pc_pixel_format fmt, const pc_algo_filter *f)
{
    pc_surface s, opaque, o;
    uint64_t h = H0;
    pc_rect area, ext2 = pc_rect_make(-2, -2, 30, 31);
    pc_algo_halo halo = pc_algo_filter_halo(f);
    int32_t tiles[3] = {256, (!halo.bounds && halo.radius > 12) ? 37 : 7, 64};
    size_t t;
    surf(&s, fmt, pc_rect_make(-4, 2, 37, 33), 9, false);
    surf(&opaque, fmt, R, 1, true);
    area = pc_algo_output_area(f, pc_surface_content_bounds(&s), R, NULL);
    hrect(&h, area);
    for (t = 0; t < 3; t++) {
        hh(&h, hs_st(pc_algo_apply_tiled(&s, f, area, R, NULL, tiles[t], NULL, &o, NULL), &o));
    }
    hh(&h, hs_st(pc_algo_apply(&s, f, area, R, &SEL_GRAD, &o, NULL), &o));
    hh(&h,
       hs_st(pc_algo_apply_in(&opaque, f, pc_algo_output_area(f, pc_surface_content_bounds(&opaque), R, NULL),
                              R, NULL, R, &o, NULL),
             &o));
    hh(&h, hs_st(pc_algo_apply_tiled(&s, f, pc_rect_make(-9, -9, 50, 50), pc_rect_make(5, 5, 25, 20), NULL, 9,
                                     &ext2, &o, NULL),
                 &o));
    pc_surface_fini(&s);
    pc_surface_fini(&opaque);
    return h;
}

/* ---- Displace maps (rustref: maps) ---- */

static void map_solid(pc_algo_image *m, const float v[4])
{
    size_t i;
    CHECK(pc_algo_image_init(m, pc_rect_make(0, 0, 8, 8), 4, NULL) == PC_OK);
    for (i = 0; i < 64; i++) {
        memcpy(m->data + i * 4, v, 4 * sizeof(float));
    }
}

static void map_grad(pc_algo_image *m, pc_rect r, size_t ch)
{
    size_t k = 0, c;
    int32_t x, y;
    CHECK(pc_algo_image_init(m, r, ch, NULL) == PC_OK);
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            for (c = 0; c < ch; c++) {
                m->data[k++] = pat(x, y, c, 5);
            }
        }
    }
}

static bool map_named(const char *name, pc_algo_image *m)
{
    if (strcmp(name, "solid") == 0) {
        const float v[4] = {0.8f, 0.3f, 0.5f, 1.0f};
        map_solid(m, v);
    } else if (strcmp(name, "shift3") == 0) {
        const float v[4] = {0.5f + 3.0f / 64.0f, 0.5f, 0.5f, 1.0f};
        map_solid(m, v);
    } else if (strcmp(name, "grad4") == 0) {
        map_grad(m, pc_rect_make(0, 0, 13, 9), 4);
    } else if (strcmp(name, "grad1") == 0) {
        map_grad(m, pc_rect_make(-3, 2, 7, 11), 1);
    } else if (strcmp(name, "grad2") == 0) {
        map_grad(m, pc_rect_make(5, 5, 6, 6), 2);
    } else if (strcmp(name, "grad3") == 0) {
        map_grad(m, pc_rect_make(0, 0, 50, 40), 3);
    } else {
        return false;
    }
    return true;
}

/* ---- line handlers ---- */

static bool parse_filter(const char *json, pc_algo_filter *f, pc_error *err)
{
    return pc_algo_filter_from_json_str(json, strlen(json), f, err) == PC_OK;
}

static const pc_algo_jfield *field_named(const pc_algo_filter_def *d, const char *name)
{
    size_t i;
    for (i = 0; i < d->nfields; i++) {
        if (strcmp(d->fields[i].name, name) == 0) {
            return &d->fields[i];
        }
    }
    return NULL;
}

static void halo_text(pc_algo_halo h, char *out, size_t cap)
{
    if (h.bounds) {
        (void)snprintf(out, cap, "bounds");
    } else {
        (void)snprintf(out, cap, "r%d", (int)h.radius);
    }
}

/* Computes the C version of one golden line into `got`; false when the prefix is not handled. */
static bool replay(const char *want, char *got, size_t cap)
{
    char buf[8192], *tok[8];
    size_t nt;
    pc_algo_filter f;
    pc_error err;
    (void)snprintf(buf, sizeof buf, "%s", want);
    got[0] = '\0';
    if (strncmp(want, "fx ", 3) == 0 || strncmp(want, "fxg ", 4) == 0) {
        bool g = want[2] == 'g';
        nt = split(buf, tok, g ? 3 : 4);
        if (nt < (g ? 3u : 4u)) {
            return false;
        }
        if (!parse_filter(tok[g ? 1 : 2], &f, &err)) {
            (void)snprintf(got, cap, "%s ERR %s", tok[g ? 1 : 2], err.msg);
            return true;
        }
        if (g) {
            pc_surface s, o;
            pc_rect area;
            uint64_t h = H0;
            pc_pixel_format f32 = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true};
            surf(&s, f32, pc_rect_make(-4, 2, 37, 33), 9, false);
            area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
            hrect(&h, area);
            hh(&h, hs_st(pc_algo_apply_tiled(&s, &f, area, R, NULL, 256, NULL, &o, NULL), &o));
            hh(&h, hs_st(pc_algo_apply_tiled(&s, &f, area, R, NULL, 16, &R, &o, NULL), &o));
            pc_surface_fini(&s);
            (void)snprintf(got, cap, "fxg %s %016llx", tok[1], (unsigned long long)h);
        } else {
            (void)snprintf(got, cap, "fx %s %s %016llx", tok[1], tok[2],
                           (unsigned long long)run_case(fmt_named(tok[1]), &f));
        }
        pc_algo_filter_fini(&f);
        return true;
    }
    if (strncmp(want, "fxk ", 4) == 0) {
        const pc_algo_jfield *fl;
        nt = split(buf, tok, 6);
        if (nt < 6 || !parse_filter(tok[4], &f, &err)) {
            return false;
        }
        fl = field_named(f.def, tok[2]);
        if (fl == NULL || fl->kind != PC_ALGO_JF_F32) {
            pc_algo_filter_fini(&f);
            return false;
        }
        *(float *)(void *)((char *)f.params + fl->off) = strcmp(tok[3], "nan") == 0 ? NAN : INFINITY;
        (void)snprintf(got, cap, "fxk %s %s %s %s %016llx", tok[1], tok[2], tok[3], tok[4],
                       (unsigned long long)run_case(fmt_named(tok[1]), &f));
        pc_algo_filter_fini(&f);
        return true;
    }
    if (strncmp(want, "fxmap ", 6) == 0) {
        pc_algo_image map;
        pc_algo_displace_params *dp;
        nt = split(buf, tok, 5);
        if (nt < 5 || !parse_filter(tok[3], &f, &err) || !map_named(tok[1], &map)) {
            return false;
        }
        dp = f.params;
        dp->map = &map;
        (void)snprintf(got, cap, "fxmap %s %s %s %016llx", tok[1], tok[2], tok[3],
                       (unsigned long long)run_case(fmt_named(tok[2]), &f));
        pc_algo_filter_fini(&f);
        pc_algo_image_fini(&map);
        return true;
    }
    if (strncmp(want, "label ", 6) == 0 || strncmp(want, "halo ", 5) == 0) {
        bool lab = want[0] == 'l';
        nt = split(buf, tok, 3);
        if (nt < 2 || !parse_filter(tok[1], &f, &err)) {
            return false;
        }
        if (lab) {
            (void)snprintf(got, cap, "label %s %s", tok[1], pc_algo_filter_label(&f));
        } else {
            char ht[32];
            halo_text(pc_algo_filter_halo(&f), ht, sizeof ht);
            (void)snprintf(got, cap, "halo %s %s", tok[1], ht);
        }
        pc_algo_filter_fini(&f);
        return true;
    }
    if (strncmp(want, "de ", 3) == 0) {
        char *tab = strchr(buf, '\t');
        const char *in = buf + 3;
        if (tab == NULL) {
            return false;
        }
        *tab = '\0';
        if (parse_filter(in, &f, &err)) {
            pc_buf b;
            pc_buf_init(&b);
            if (pc_algo_filter_to_json_text(&f, &b, NULL) == PC_OK) {
                (void)snprintf(got, cap, "de %s\t%s | %s", in, pc_buf_cstr(&b), pc_algo_filter_label(&f));
            }
            pc_buf_fini(&b);
            pc_algo_filter_fini(&f);
        } else {
            (void)snprintf(got, cap, "de %s\tERR %s", in, err.msg);
        }
        return true;
    }
    return fx_replay_more(want, got, cap);
}

static void test_fx_parity(void)
{
    size_t glen = 0, lines = 0, skipped = 0, pos = 0;
    const char *path = getenv("FX_GOLDEN") != NULL ? getenv("FX_GOLDEN") : GOLDEN;
    char *golden = read_file(path, &glen);
    static char got[16384];
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    surf(&SEL_GRAD, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(-5, -5, 45, 35), 3, false);
    while (pos < glen) {
        char *want = golden + pos, *nl = strchr(want, '\n');
        if (nl != NULL) {
            *nl = '\0';
            pos = (size_t)(nl - golden) + 1;
        } else {
            pos = glen;
        }
        if (want[0] == '\0') {
            continue;
        }
        lines++;
        if (!replay(want, got, sizeof got)) {
            skipped++;
            if (getenv("FX_VERBOSE") != NULL) {
                (void)fprintf(stderr, "skip: %.120s\n", want);
            }
            continue;
        }
        if (strncmp(got, "de ", 3) == 0 && strstr(got, "\tERR ") != NULL &&
            strlen(strstr(got, "\tERR ") + 5) == PC_ERROR_MSG_MAX - 1 &&
            strncmp(want, got, strlen(got)) == 0) {
            pc_t_checks++; /* pc_error keeps the first 255 bytes of a long message (plan section 7) */
        } else if (strcmp(want, got) != 0) {
            PC_T_FAIL("line %zu differs", lines);
            (void)fprintf(stderr, "  want %s\n  got  %s\n", want, got);
        } else {
            pc_t_checks++;
        }
    }
    CHECK_EQ_UINT(skipped, 0);
    pc_surface_fini(&SEL_GRAD);
    free(golden);
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    RUN(test_fx_parity);
    return pc_t_finish();
}
