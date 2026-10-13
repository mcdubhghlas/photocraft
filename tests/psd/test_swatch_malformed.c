/* Ports crates/psd/tests/swatch_malformed.rs: .grd, .ase and .aco hostile-input handling (every
 * single-bit flip, the caps driven over their limits, the number policy of the gradient reader,
 * surrogate-safe name caps, truncation, structurally hostile blocks). The Rust catch_unwind
 * wrappers become plain calls: a crash fails the executable, and ASan/UBSan runs catch the rest. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/bytes.h"
#include "harness.h"
#include "psd/aco.h"
#include "psd/ase.h"
#include "psd/descriptor.h"
#include "psd/grd.h"
#include "psd/internal.h"

static void set_str(pc_psd_str *s, const char *v)
{
    (void)pc_psd_str_set(s, v, strlen(v), NULL);
}

static void ase_sw(pc_psd_ase_swatch *s, const char *name)
{
    memset(s, 0, sizeof *s);
    set_str(&s->name, name);
    s->color.model = PC_PSD_ASE_RGB;
    s->color.c[0] = 1.0f;
    s->kind = PC_PSD_ASE_PROCESS;
}

static pc_psd_bytes sample_ase(void)
{
    pc_psd_ase f;
    pc_psd_ase_entry e[2];
    pc_psd_bytes out = {NULL, 0};
    memset(e, 0, sizeof e);
    e[0].group = true;
    set_str(&e[0].name, "Brand");
    e[0].swatches = calloc(2, sizeof *e[0].swatches);
    if (e[0].swatches != NULL) {
        ase_sw(&e[0].swatches[0], "Red");
        ase_sw(&e[0].swatches[1], "Blue");
        e[0].n = 2;
    }
    ase_sw(&e[1].swatch, "Loose");
    f.entries = e;
    f.n = 2;
    CHECK_EQ_INT(pc_psd_ase_write(&f, &out, NULL), PC_OK);
    pc_psd_ase_entry_fini(&e[0]);
    pc_psd_ase_entry_fini(&e[1]);
    return out;
}

static void aco_sw(pc_psd_aco_swatch *s, const char *name, pc_psd_aco_kind k, uint16_t a, uint16_t b,
                   uint16_t c, uint16_t d)
{
    memset(s, 0, sizeof *s);
    set_str(&s->name, name);
    s->color.kind = k;
    s->color.v[0] = a;
    s->color.v[1] = b;
    s->color.v[2] = c;
    s->color.v[3] = d;
}

#define NACO 5

static void sample_aco_swatches(pc_psd_aco_swatch s[NACO])
{
    aco_sw(&s[0], "Pure Red", PC_PSD_ACO_RGB, 65535, 0, 0, 0);
    aco_sw(&s[1], "Cyan ink", PC_PSD_ACO_CMYK, 65535, 0, 0, 1234);
    aco_sw(&s[2], "Lab", PC_PSD_ACO_LAB, 5321, (uint16_t)(int16_t)-12800, 12700, 0);
    aco_sw(&s[3], "Gray", PC_PSD_ACO_GRAY, 5000, 0, 0, 0);
    aco_sw(&s[4], "Book", PC_PSD_ACO_OTHER, 1, 2, 3, 4);
    s[4].color.space = 3;
}

static void free_aco(pc_psd_aco_swatch *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_psd_str_fini(&s[i].name);
    }
}

static pc_psd_bytes sample_aco(void)
{
    pc_psd_aco_swatch s[NACO];
    pc_psd_bytes out = {NULL, 0};
    sample_aco_swatches(s);
    CHECK_EQ_INT(pc_psd_aco_write(s, NACO, &out, NULL), PC_OK);
    free_aco(s, NACO);
    return out;
}

static pc_psd_bytes sample_grd(void)
{
    pc_psd_grd_gradient g[2];
    pc_psd_grd_stop stops[2];
    pc_psd_grd_opacity op[2] = {{0.0f, 1.0f, 0.5f}, {1.0f, 0.25f, 0.5f}};
    pc_psd_bytes out = {NULL, 0};
    memset(g, 0, sizeof g);
    memset(stops, 0, sizeof stops);
    stops[0].location = 0.0f;
    stops[0].midpoint = 0.5f;
    stops[0].color.model = PC_PSD_GRD_RGB;
    stops[0].color.c[0] = 1.0f;
    stops[1].location = 1.0f;
    stops[1].midpoint = 0.5f;
    stops[1].color.model = PC_PSD_GRD_BACKGROUND;
    set_str(&g[0].name, "Sunset");
    g[0].stops = stops;
    g[0].nstops = 2;
    g[0].opacity = op;
    g[0].nopacity = 2;
    set_str(&g[1].name, "Noisy");
    g[1].noise = true;
    CHECK_EQ_INT(pc_psd_grd_write(g, 2, &out, NULL), PC_OK);
    pc_psd_str_fini(&g[0].name);
    pc_psd_str_fini(&g[1].name);
    return out;
}

static size_t find(const pc_psd_bytes *b, const char *needle, size_t nlen, bool last)
{
    size_t i;
    size_t at = SIZE_MAX;
    for (i = 0; i + nlen <= b->len; i++) {
        if (memcmp(b->data + i, needle, nlen) == 0) {
            at = i;
            if (!last) {
                break;
            }
        }
    }
    return at;
}

/* ---- bit flips ---- */

typedef enum { K_ASE, K_ACO, K_GRD } kind;

static void parse_any(kind k, const uint8_t *d, size_t n)
{
    if (k == K_ASE) {
        pc_psd_ase f;
        if (pc_psd_ase_parse(d, n, &f, NULL) == PC_OK) {
            pc_psd_ase_fini(&f);
        }
    } else if (k == K_ACO) {
        pc_psd_aco f;
        if (pc_psd_aco_parse(d, n, &f, NULL) == PC_OK) {
            pc_psd_aco_fini(&f);
        }
    } else {
        pc_psd_grd_gradient *g = NULL;
        size_t ng = 0;
        if (pc_psd_grd_parse(d, n, &g, &ng, NULL) == PC_OK) {
            pc_psd_grd_free(g, ng);
        }
    }
}

static void bitflip_sweep(pc_psd_bytes b, kind k)
{
    size_t byte;
    int bit;
    for (byte = 0; byte < b.len; byte++) {
        for (bit = 0; bit < 8; bit++) {
            b.data[byte] ^= (uint8_t)(1u << bit);
            parse_any(k, b.data, b.len);
            b.data[byte] ^= (uint8_t)(1u << bit);
        }
    }
    CHECK(b.len > 0);
    pc_psd_bytes_fini(&b);
}

static void test_bitflips_never_crash(void)
{
    bitflip_sweep(sample_ase(), K_ASE);
    bitflip_sweep(sample_aco(), K_ACO);
    bitflip_sweep(sample_grd(), K_GRD);
}

/* ---- the gradient number policy ---- */

static bool in01(float v)
{
    return isfinite(v) && v >= 0.0f && v <= 1.0f;
}

static void check_gradient_sane(const pc_psd_grd_gradient *g)
{
    size_t i;
    int k;
    for (i = 0; i < g->nstops; i++) {
        const pc_psd_grd_stop *s = &g->stops[i];
        CHECK(in01(s->location));
        CHECK(in01(s->midpoint));
        for (k = 0; k < 4; k++) {
            CHECK(isfinite(s->color.c[k]));
        }
    }
    for (i = 0; i < g->nopacity; i++) {
        CHECK(in01(g->opacity[i].location));
        CHECK(g->opacity[i].opacity >= 0.0f && g->opacity[i].opacity <= 1.0f);
        CHECK(in01(g->opacity[i].midpoint));
    }
}

static void test_grd_parsed_values_are_sane(void)
{
    pc_psd_bytes b = sample_grd();
    pc_psd_grd_gradient *g = NULL;
    size_t n = 0;
    size_t i;
    CHECK_EQ_INT(pc_psd_grd_parse(b.data, b.len, &g, &n, NULL), PC_OK);
    for (i = 0; i < n; i++) {
        check_gradient_sane(&g[i]);
    }
    pc_psd_grd_free(g, n);
    pc_psd_bytes_fini(&b);
}

static void test_grd_gradient_count_is_capped(void)
{
    size_t over = PC_PSD_GRD_MAX_GRADIENTS + 1;
    pc_psd_grd_gradient *gs = calloc(over, sizeof *gs);
    pc_psd_bytes all = {NULL, 0};
    pc_psd_bytes capped = {NULL, 0};
    pc_psd_grd_gradient *back = NULL;
    size_t n = 0;
    size_t i;
    if (gs == NULL) {
        CHECK(gs != NULL);
        return;
    }
    for (i = 0; i < over; i++) {
        set_str(&gs[i].name, "g");
    }
    CHECK_EQ_INT(pc_psd_grd_write(gs, over, &all, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_grd_write(gs, over - 1, &capped, NULL), PC_OK);
    CHECK(all.len > capped.len);
    CHECK_EQ_INT(pc_psd_grd_parse(all.data, all.len, &back, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, PC_PSD_GRD_MAX_GRADIENTS);
    pc_psd_grd_free(back, n);
    for (i = 0; i < over; i++) {
        pc_psd_str_fini(&gs[i].name);
    }
    free(gs);
    pc_psd_bytes_fini(&all);
    pc_psd_bytes_fini(&capped);
}

static void test_grd_stop_counts_are_capped(void)
{
    size_t over = PC_PSD_GRD_MAX_STOPS + 1;
    pc_psd_grd_stop *stops = calloc(over, sizeof *stops);
    pc_psd_grd_opacity *op = calloc(over, sizeof *op);
    pc_psd_grd_gradient g;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes a = {NULL, 0};
    pc_psd_grd_gradient *back = NULL;
    size_t n = 0;
    size_t i;
    if (stops == NULL || op == NULL) {
        free(stops);
        free(op);
        CHECK(false);
        return;
    }
    for (i = 0; i < over; i++) {
        stops[i].location = 0.5f;
        stops[i].midpoint = 0.5f;
        stops[i].color.model = PC_PSD_GRD_RGB;
        stops[i].color.c[0] = 1.0f;
        op[i].location = 0.5f;
        op[i].opacity = 1.0f;
        op[i].midpoint = 0.5f;
    }
    memset(&g, 0, sizeof g);
    set_str(&g.name, "wide");
    g.stops = stops;
    g.opacity = op;
    /* Each list over its cap is larger than at the cap (the writer does not cap). */
    g.nstops = over;
    CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &b, NULL), PC_OK);
    g.nstops = over - 1;
    CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &a, NULL), PC_OK);
    CHECK(b.len > a.len);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
    g.nstops = 0;
    g.nopacity = over;
    CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &b, NULL), PC_OK);
    g.nopacity = over - 1;
    CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &a, NULL), PC_OK);
    CHECK(b.len > a.len);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
    g.nstops = over;
    g.nopacity = over;
    CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_grd_parse(b.data, b.len, &back, &n, NULL), PC_OK);
    if (n == 1) {
        CHECK_EQ_UINT(back[0].nstops, PC_PSD_GRD_MAX_STOPS);
        CHECK_EQ_UINT(back[0].nopacity, PC_PSD_GRD_MAX_STOPS);
    }
    pc_psd_grd_free(back, n);
    pc_psd_bytes_fini(&b);
    pc_psd_str_fini(&g.name);
    free(stops);
    free(op);
}

/* Patches the f64 stored for `key` (the first or last occurrence) and parses. */
static pc_status patch_grd(const char *key, double v, bool last, pc_psd_grd_gradient **g, size_t *n)
{
    pc_psd_bytes b = sample_grd();
    size_t at = find(&b, key, 4, last);
    size_t skip = 0;
    uint64_t bits;
    pc_status st;
    CHECK(at != SIZE_MAX);
    if (at == SIZE_MAX) {
        pc_psd_bytes_fini(&b);
        return PC_ERR_INVALID;
    }
    if (memcmp(b.data + at + 4, "doub", 4) == 0) {
        skip = 8;
    } else if (memcmp(b.data + at + 4, "UntF", 4) == 0) {
        skip = 12;
    }
    CHECK(skip != 0);
    memcpy(&bits, &v, 8);
    pc_store_be64(b.data + at + skip, bits);
    st = pc_psd_grd_parse(b.data, b.len, g, n, NULL);
    pc_psd_bytes_fini(&b);
    return st;
}

static void test_grd_non_finite_opacity_and_colour_do_not_survive(void)
{
    static const char *const keys[4] = {"Opct", "Rd  ", "Grn ", "Bl  "};
    const double bad[5] = {NAN, INFINITY, -INFINITY, 1e300, 3.0e38};
    int k;
    int i;
    for (k = 0; k < 4; k++) {
        for (i = 0; i < 5; i++) {
            pc_psd_grd_gradient *g = NULL;
            size_t n = 0;
            size_t j;
            CHECK_EQ_INT(patch_grd(keys[k], bad[i], false, &g, &n), PC_OK);
            for (j = 0; j < n; j++) {
                check_gradient_sane(&g[j]);
            }
            pc_psd_grd_free(g, n);
        }
    }
}

/* Builds an 8BGR file around one gradient descriptor. */
static pc_psd_bytes grd_file(pc_psd_desc *grad)
{
    pc_psd_desc outer;
    pc_psd_desc file;
    pc_psd_value v;
    pc_psd_value list;
    pc_psd_bytes body = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    pc_psd_bytes out;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    (void)pc_psd_desc_init(&outer, "Grdn", NULL);
    (void)pc_psd_value_object(grad, &v, NULL);
    (void)pc_psd_desc_add(&outer, "Grad", &v, NULL);
    (void)pc_psd_value_object(&outer, &v, NULL);
    (void)pc_psd_value_list_push(&list, &v, NULL);
    (void)pc_psd_desc_init(&file, "null", NULL);
    (void)pc_psd_desc_add(&file, "GrdL", &list, NULL);
    (void)pc_psd_vdesc_to_bytes(&file, &body, NULL);
    pc_psd_desc_fini(&file);
    (void)pc_buf_append(&b, "8BGR\0\5", 6, NULL);
    (void)pc_buf_append(&b, body.data, body.len, NULL);
    pc_psd_bytes_fini(&body);
    out.data = b.data;
    out.len = b.len;
    return out;
}

static void test_grd_midpoint_stored_as_a_double_is_still_sanitised(void)
{
    const double vals[4] = {NAN, INFINITY, 1e300, 3.0e38};
    int i;
    for (i = 0; i < 4; i++) {
        pc_psd_desc trns;
        pc_psd_desc grad;
        pc_psd_value v = pc_psd_value_double(vals[i]);
        pc_psd_value list;
        pc_psd_bytes f;
        pc_psd_grd_gradient *g = NULL;
        size_t n = 0;
        float want = isnan(vals[i]) ? 0.5f : vals[i] > 0.0 ? 1.0f : 0.0f;
        memset(&list, 0, sizeof list);
        list.kind = PC_PSD_V_LIST;
        (void)pc_psd_desc_init(&trns, "TrnS", NULL);
        (void)pc_psd_desc_add(&trns, "Mdpn", &v, NULL);
        (void)pc_psd_value_object(&trns, &v, NULL);
        (void)pc_psd_value_list_push(&list, &v, NULL);
        (void)pc_psd_desc_init(&grad, "Grdn", NULL);
        (void)pc_psd_desc_add(&grad, "Trns", &list, NULL);
        f = grd_file(&grad);
        CHECK_EQ_INT(pc_psd_grd_parse(f.data, f.len, &g, &n, NULL), PC_OK);
        CHECK(n == 1 && g[0].nopacity == 1);
        if (n == 1 && g[0].nopacity == 1) {
            CHECK(g[0].opacity[0].midpoint == want);
        }
        pc_psd_grd_free(g, n);
        pc_psd_bytes_fini(&f);
    }
}

static void test_aco_cmyk_is_stored_inverted_and_reads_back_as_ink(void)
{
    pc_psd_aco_swatch s;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_aco back;
    aco_sw(&s, "k", PC_PSD_ACO_CMYK, 65535, 0, 0, 1234);
    CHECK_EQ_INT(pc_psd_aco_write(&s, 1, &b, NULL), PC_OK);
    CHECK(b.len >= 14);
    if (b.len >= 14) {
        CHECK_EQ_UINT(pc_load_be16(b.data + 4), PC_PSD_ACO_SPACE_CMYK);
        CHECK_EQ_UINT(pc_load_be16(b.data + 6), 0);
        CHECK_EQ_UINT(pc_load_be16(b.data + 8), 65535);
        CHECK_EQ_UINT(pc_load_be16(b.data + 10), 65535);
        CHECK_EQ_UINT(pc_load_be16(b.data + 12), 65535 - 1234);
    }
    CHECK_EQ_INT(pc_psd_aco_parse(b.data, b.len, &back, NULL), PC_OK);
    CHECK(back.n == 1 && pc_psd_aco_swatch_eq(&back.v[0], &s));
    pc_psd_aco_fini(&back);
    pc_psd_bytes_fini(&b);
    pc_psd_str_fini(&s.name);
}

static void test_grd_unrepresentable_numbers_follow_the_documented_policy(void)
{
    const double vals[7] = {NAN, INFINITY, -INFINITY, 1e300, -1e300, 3.0e38, -3.0e38};
    const float opct[7] = {1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    const float blue[7] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    int i;
    for (i = 0; i < 7; i++) {
        pc_psd_grd_gradient *g = NULL;
        size_t n = 0;
        /* The second transparency stop carries 0.25, so a fallback is visible. */
        CHECK_EQ_INT(patch_grd("Opct", vals[i], true, &g, &n), PC_OK);
        if (n > 0 && g[0].nopacity == 2) {
            CHECK(g[0].opacity[1].opacity == opct[i]);
        }
        pc_psd_grd_free(g, n);
        CHECK_EQ_INT(patch_grd("Bl  ", vals[i], false, &g, &n), PC_OK);
        if (n > 0 && g[0].nstops > 0) {
            CHECK(g[0].stops[0].color.model == PC_PSD_GRD_RGB && g[0].stops[0].color.c[2] == blue[i]);
        }
        pc_psd_grd_free(g, n);
    }
}

static void test_aco_and_ase_never_split_a_surrogate_pair(void)
{
    /* "a" plus 600 emoji: 1 + 1200 units, so a 1024-unit cap lands mid-pair. */
    static const char emoji[] = "\xF0\x9F\x98\x80";
    pc_buf name = PC_BUF_INIT;
    pc_buf want = PC_BUF_INIT;
    pc_psd_aco_swatch s;
    pc_psd_bytes once = {NULL, 0};
    pc_psd_bytes twice = {NULL, 0};
    pc_psd_aco back;
    size_t i;
    (void)pc_buf_append(&name, "a", 1, NULL);
    (void)pc_buf_append(&want, "a", 1, NULL);
    for (i = 0; i < 600; i++) {
        (void)pc_buf_append(&name, emoji, 4, NULL);
        if (i < 511) {
            (void)pc_buf_append(&want, emoji, 4, NULL);
        }
    }
    aco_sw(&s, "", PC_PSD_ACO_GRAY, 5000, 0, 0, 0);
    (void)pc_psd_str_set(&s.name, (const char *)name.data, name.len, NULL);
    CHECK_EQ_INT(pc_psd_aco_write(&s, 1, &once, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_aco_parse(once.data, once.len, &back, NULL), PC_OK);
    if (back.n == 1) {
        CHECK(back.v[0].name.len == want.len && memcmp(back.v[0].name.s, want.data, want.len) == 0);
        CHECK_EQ_INT(pc_psd_aco_write(back.v, back.n, &twice, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&twice, &once));
        pc_psd_bytes_fini(&twice);
    }
    pc_psd_aco_fini(&back);
    pc_psd_bytes_fini(&once);
    pc_psd_str_fini(&s.name);
    {
        pc_psd_ase f;
        pc_psd_ase ab;
        pc_psd_ase_entry e;
        memset(&e, 0, sizeof e);
        ase_sw(&e.swatch, "");
        (void)pc_psd_str_set(&e.swatch.name, (const char *)name.data, name.len, NULL);
        f.entries = &e;
        f.n = 1;
        CHECK_EQ_INT(pc_psd_ase_write(&f, &once, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_ase_parse(once.data, once.len, &ab, NULL), PC_OK);
        if (ab.n == 1 && !ab.entries[0].group) {
            const pc_psd_str *got = &ab.entries[0].swatch.name;
            CHECK(got->len == want.len && memcmp(got->s, want.data, want.len) == 0);
            CHECK_EQ_INT(pc_psd_ase_write(&ab, &twice, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&twice, &once));
            pc_psd_bytes_fini(&twice);
        }
        pc_psd_ase_fini(&ab);
        pc_psd_bytes_fini(&once);
        pc_psd_ase_entry_fini(&e);
    }
    pc_buf_fini(&name);
    pc_buf_fini(&want);
}

static void test_grd_out_of_range_numbers_are_clamped(void)
{
    pc_psd_grd_stop stops[2];
    pc_psd_grd_opacity op[2] = {{9.0f, 5.0f, 7.0f}, {-2.0f, -1.0f, 42.0f}};
    pc_psd_grd_gradient g;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_grd_gradient *back = NULL;
    size_t n = 0;
    memset(stops, 0, sizeof stops);
    memset(&g, 0, sizeof g);
    stops[0].location = 2.0f;
    stops[0].midpoint = 5.0f;
    stops[0].color.model = PC_PSD_GRD_RGB;
    stops[0].color.c[0] = 1.0f;
    stops[1].location = -3.0f;
    stops[1].midpoint = -1.0f;
    stops[1].color.model = PC_PSD_GRD_RGB;
    stops[1].color.c[1] = 1.0f;
    set_str(&g.name, "out of range");
    g.stops = stops;
    g.nstops = 2;
    g.opacity = op;
    g.nopacity = 2;
    CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_grd_parse(b.data, b.len, &back, &n, NULL), PC_OK);
    if (n == 1 && back[0].nstops == 2 && back[0].nopacity == 2) {
        check_gradient_sane(&back[0]);
        CHECK(back[0].stops[0].location == 1.0f && back[0].stops[0].midpoint == 1.0f);
        CHECK(back[0].stops[1].location == 0.0f && back[0].stops[1].midpoint == 0.0f);
        CHECK(back[0].opacity[0].location == 1.0f && back[0].opacity[0].opacity == 1.0f &&
              back[0].opacity[0].midpoint == 1.0f);
        CHECK(back[0].opacity[1].location == 0.0f && back[0].opacity[1].opacity == 0.0f &&
              back[0].opacity[1].midpoint == 1.0f);
    } else {
        CHECK(false);
    }
    pc_psd_grd_free(back, n);
    pc_psd_bytes_fini(&b);
    pc_psd_str_fini(&g.name);
}

static void test_grd_huge_and_nan_components(void)
{
    static const char *const keys[3] = {"Rd  ", "Grn ", "Bl  "};
    int k;
    for (k = 0; k < 4; k++) {
        pc_psd_grd_gradient *g = NULL;
        size_t n = 0;
        size_t j;
        CHECK_EQ_INT(patch_grd(k < 3 ? keys[k] : "Rd  ", k < 3 ? 1e300 : NAN, false, &g, &n), PC_OK);
        CHECK(n > 0);
        for (j = 0; j < n; j++) {
            check_gradient_sane(&g[j]);
        }
        pc_psd_grd_free(g, n);
    }
}

/* ---- .ase / .aco caps and round trips ---- */

static void test_ase_round_trip_preserves_swatch_count(void)
{
    pc_psd_bytes b = sample_ase();
    pc_psd_bytes out = {NULL, 0};
    pc_psd_ase f;
    pc_psd_ase back;
    CHECK_EQ_INT(pc_psd_ase_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_ase_write(&f, &out, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_ase_parse(out.data, out.len, &back, NULL), PC_OK);
    CHECK_EQ_UINT(pc_psd_ase_swatch_count(&back), pc_psd_ase_swatch_count(&f));
    CHECK_EQ_UINT(pc_psd_ase_swatch_count(&f), 3);
    pc_psd_ase_fini(&f);
    pc_psd_ase_fini(&back);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&out);
}

static void ase_header(pc_buf *f, uint32_t count)
{
    uint8_t h[12] = {'A', 'S', 'E', 'F', 0, 1, 0, 0, 0, 0, 0, 0};
    pc_store_be32(h + 8, count);
    (void)pc_buf_append(f, h, 12, NULL);
}

static void ase_block(pc_buf *f, uint16_t type, const uint8_t *body, size_t len)
{
    uint8_t h[6];
    pc_store_be16(h, type);
    pc_store_be32(h + 2, (uint32_t)len);
    (void)pc_buf_append(f, h, 6, NULL);
    (void)pc_buf_append(f, body, len, NULL);
}

static void test_ase_swatch_cap_is_enforced_on_hostile_input(void)
{
    static const uint8_t body[12] = {0, 0, 'G', 'r', 'a', 'y', 0, 0, 0, 0, 0, 2};
    uint32_t total = PC_PSD_ASE_MAX_SWATCHES + 1;
    pc_buf f = PC_BUF_INIT;
    pc_psd_ase a;
    uint32_t i;
    ase_header(&f, total);
    for (i = 0; i < total; i++) {
        ase_block(&f, 1, body, sizeof body);
    }
    CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_ERR_LIMIT);
    /* One block under the cap is accepted, so the cap is what refuses it. */
    pc_buf_truncate(&f, f.len - (6 + sizeof body));
    pc_store_be32(f.data + 8, total - 1);
    CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_OK);
    pc_psd_ase_fini(&a);
    pc_buf_fini(&f);
}

static void test_ase_over_long_name_is_truncated_on_read(void)
{
    size_t units = PC_PSD_ASE_MAX_NAME_UNITS + 976;
    pc_buf body = PC_BUF_INIT;
    pc_buf f = PC_BUF_INIT;
    pc_psd_ase a;
    uint8_t u[2];
    size_t i;
    pc_store_be16(u, (uint16_t)units);
    (void)pc_buf_append(&body, u, 2, NULL);
    for (i = 0; i < units; i++) {
        (void)pc_buf_append(&body, "\0x", 2, NULL);
    }
    (void)pc_buf_append(&body, "Gray\0\0\0\0\0\2", 10, NULL);
    ase_header(&f, 1);
    ase_block(&f, 1, body.data, body.len);
    CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_OK);
    if (a.n == 1 && !a.entries[0].group) {
        CHECK_EQ_UINT(a.entries[0].swatch.name.len, PC_PSD_ASE_MAX_NAME_UNITS);
    }
    pc_psd_ase_fini(&a);
    pc_buf_fini(&body);
    pc_buf_fini(&f);
}

static void test_aco_round_trip_preserves_swatch_count(void)
{
    pc_psd_bytes b = sample_aco();
    pc_psd_bytes out = {NULL, 0};
    pc_psd_aco f;
    pc_psd_aco back;
    CHECK_EQ_INT(pc_psd_aco_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_aco_write(f.v, f.n, &out, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_aco_parse(out.data, out.len, &back, NULL), PC_OK);
    CHECK_EQ_UINT(back.n, f.n);
    pc_psd_aco_fini(&f);
    pc_psd_aco_fini(&back);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&out);
}

static void test_aco_over_long_name_is_truncated_on_read(void)
{
    static const uint8_t head[14] = {0, 2, 0, 1, 0, 0, 0, 1, 0, 2, 0, 3, 0, 0};
    pc_buf f = PC_BUF_INIT;
    pc_psd_aco a;
    uint8_t n4[4];
    size_t i;
    (void)pc_buf_append(&f, head, sizeof head, NULL);
    pc_store_be32(n4, 2001);
    (void)pc_buf_append(&f, n4, 4, NULL);
    for (i = 0; i < 2000; i++) {
        (void)pc_buf_append(&f, "\0x", 2, NULL);
    }
    (void)pc_buf_append(&f, "\0\0", 2, NULL);
    CHECK_EQ_INT(pc_psd_aco_parse(f.data, f.len, &a, NULL), PC_OK);
    if (a.n == 1) {
        CHECK_EQ_UINT(a.v[0].name.len, PC_PSD_ACO_MAX_NAME_UNITS);
    }
    pc_psd_aco_fini(&a);
    pc_buf_fini(&f);
}

static size_t x_run(const pc_psd_bytes *b)
{
    static const char run[16] = {0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x', 0, 'x'};
    return find(b, run, 16, false);
}

static void test_aco_and_ase_write_cap_the_name(void)
{
    char *longname = malloc(70001);
    pc_psd_aco_swatch s;
    pc_psd_bytes b = {NULL, 0};
    size_t at;
    if (longname == NULL) {
        CHECK(false);
        return;
    }
    memset(longname, 'x', 70000);
    longname[70000] = '\0';
    aco_sw(&s, "", PC_PSD_ACO_GRAY, 5000, 0, 0, 0);
    (void)pc_psd_str_set(&s.name, longname, 5000, NULL);
    CHECK_EQ_INT(pc_psd_aco_write(&s, 1, &b, NULL), PC_OK);
    at = x_run(&b);
    CHECK(at != SIZE_MAX && at >= 4);
    if (at != SIZE_MAX && at >= 4) {
        CHECK_EQ_UINT(pc_load_be32(b.data + at - 4), PC_PSD_ACO_MAX_NAME_UNITS + 1);
    }
    pc_psd_bytes_fini(&b);
    pc_psd_str_fini(&s.name);
    {
        pc_psd_ase f;
        pc_psd_ase back;
        pc_psd_ase_entry e;
        memset(&e, 0, sizeof e);
        ase_sw(&e.swatch, "");
        (void)pc_psd_str_set(&e.swatch.name, longname, 70000, NULL);
        f.entries = &e;
        f.n = 1;
        CHECK_EQ_INT(pc_psd_ase_write(&f, &b, NULL), PC_OK);
        at = x_run(&b);
        CHECK(at != SIZE_MAX && at >= 2);
        if (at != SIZE_MAX && at >= 2) {
            CHECK_EQ_UINT(pc_load_be16(b.data + at - 2), PC_PSD_ASE_MAX_NAME_UNITS + 1);
        }
        CHECK_EQ_INT(pc_psd_ase_parse(b.data, b.len, &back, NULL), PC_OK);
        if (back.n == 1) {
            CHECK_EQ_UINT(back.entries[0].swatch.name.len, PC_PSD_ASE_MAX_NAME_UNITS);
        }
        pc_psd_ase_fini(&back);
        pc_psd_bytes_fini(&b);
        pc_psd_ase_entry_fini(&e);
    }
    free(longname);
}

/* ---- truncation ---- */

static void test_truncation(void)
{
    pc_psd_bytes g = sample_grd();
    pc_psd_bytes a = sample_ase();
    pc_psd_bytes c = sample_aco();
    size_t first = 4 + NACO * 10;
    size_t cut;
    pc_psd_grd_gradient *gg = NULL;
    size_t n = 0;
    pc_psd_ase af;
    pc_psd_aco cf;
    for (cut = 0; cut < g.len; cut++) {
        CHECK(pc_psd_grd_parse(g.data, cut, &gg, &n, NULL) != PC_OK);
    }
    CHECK_EQ_INT(pc_psd_grd_parse(g.data, g.len, &gg, &n, NULL), PC_OK);
    pc_psd_grd_free(gg, n);
    for (cut = 0; cut < a.len; cut++) {
        CHECK(pc_psd_ase_parse(a.data, cut, &af, NULL) != PC_OK);
    }
    CHECK_EQ_INT(pc_psd_ase_parse(a.data, a.len, &af, NULL), PC_OK);
    pc_psd_ase_fini(&af);
    CHECK(first <= c.len);
    for (cut = 0; cut < first; cut++) {
        CHECK(pc_psd_aco_parse(c.data, cut, &cf, NULL) != PC_OK);
    }
    CHECK_EQ_INT(pc_psd_aco_parse(c.data, first, &cf, NULL), PC_OK);
    pc_psd_aco_fini(&cf);
    CHECK_EQ_INT(pc_psd_aco_parse(c.data, c.len, &cf, NULL), PC_OK);
    pc_psd_aco_fini(&cf);
    pc_psd_bytes_fini(&g);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&c);
}

/* ---- structurally hostile .ase ---- */

static void group_start(pc_buf *f, const char *name)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t u[2];
    size_t i;
    pc_store_be16(u, (uint16_t)strlen(name));
    (void)pc_buf_append(&b, u, 2, NULL);
    for (i = 0; name[i] != '\0'; i++) {
        u[0] = 0;
        u[1] = (uint8_t)name[i];
        (void)pc_buf_append(&b, u, 2, NULL);
    }
    ase_block(f, 0xC001, b.data, b.len);
    pc_buf_fini(&b);
}

static void colour(pc_buf *f, const char model[4], const uint8_t *floats, size_t n)
{
    pc_buf b = PC_BUF_INIT;
    (void)pc_buf_append(&b, "\0\0", 2, NULL);
    (void)pc_buf_append(&b, model, 4, NULL);
    (void)pc_buf_append(&b, floats, n, NULL);
    (void)pc_buf_append(&b, "\0\2", 2, NULL);
    ase_block(f, 1, b.data, b.len);
    pc_buf_fini(&b);
}

static void test_ase_structurally_hostile_blocks(void)
{
    static const uint8_t one[4] = {0x3f, 0x80, 0, 0};
    static const uint8_t payload[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    static const uint8_t short_body[12] = {0, 0, 'G', 'r', 'a', 'y', 0, 0, 0, 0, 0, 2};
    pc_buf f = PC_BUF_INIT;
    pc_psd_ase a;
    size_t i;
    bool inner = false;
    /* A group end with no start parses and yields nothing. */
    ase_header(&f, 1);
    ase_block(&f, 0xC002, NULL, 0);
    CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_OK);
    CHECK_EQ_UINT(a.n, 0);
    pc_psd_ase_fini(&a);
    pc_buf_fini(&f);
    /* A group started inside another: the outer one closes, both are emitted. */
    ase_header(&f, 3);
    group_start(&f, "outer");
    group_start(&f, "inner");
    colour(&f, "Gray", one, 4);
    CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_OK);
    CHECK_EQ_UINT(pc_psd_ase_swatch_count(&a), 1);
    CHECK_EQ_UINT(a.n, 2);
    for (i = 0; i < a.n; i++) {
        CHECK(a.entries[i].group);
        if (a.entries[i].group && strcmp(pc_psd_str_c(&a.entries[i].name), "inner") == 0 &&
            a.entries[i].n == 1) {
            inner = true;
        }
    }
    CHECK(inner);
    if (a.n == 2) {
        CHECK_EQ_STR(pc_psd_str_c(&a.entries[0].name), "outer");
        CHECK_EQ_STR(pc_psd_str_c(&a.entries[1].name), "inner");
    }
    pc_psd_ase_fini(&a);
    pc_buf_fini(&f);
    /* A block length shorter than its body cuts the component: refused. */
    {
        uint8_t h[6];
        ase_header(&f, 1);
        pc_store_be16(h, 1);
        pc_store_be32(h + 2, (uint32_t)(sizeof short_body - 4));
        (void)pc_buf_append(&f, h, 6, NULL);
        (void)pc_buf_append(&f, short_body, sizeof short_body, NULL);
        CHECK(pc_psd_ase_parse(f.data, f.len, &a, NULL) != PC_OK);
        pc_buf_fini(&f);
        /* No colour type at all: accepted by design (defaults to process). */
        ase_header(&f, 1);
        ase_block(&f, 1, short_body, sizeof short_body - 2);
        CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_OK);
        CHECK_EQ_UINT(pc_psd_ase_swatch_count(&a), 1);
        pc_psd_ase_fini(&a);
        pc_buf_fini(&f);
    }
    /* An unknown colour model keeps its bytes verbatim. */
    ase_header(&f, 1);
    colour(&f, "XYZ ", payload, sizeof payload);
    CHECK_EQ_INT(pc_psd_ase_parse(f.data, f.len, &a, NULL), PC_OK);
    CHECK(a.n == 1 && !a.entries[0].group);
    if (a.n == 1 && !a.entries[0].group) {
        const pc_psd_ase_color *c = &a.entries[0].swatch.color;
        CHECK(c->model == PC_PSD_ASE_OTHER && memcmp(c->tag, "XYZ ", 4) == 0);
        CHECK(c->data.len == sizeof payload && memcmp(c->data.data, payload, sizeof payload) == 0);
    }
    pc_psd_ase_fini(&a);
    pc_buf_fini(&f);
}

static void test_aco_mismatched_section_counts_do_not_crash(void)
{
    static const uint8_t rec[10] = {0, 0, 0, 1, 0, 2, 0, 3, 0, 0};
    pc_buf f = PC_BUF_INIT;
    pc_psd_aco a;
    size_t i;
    (void)pc_buf_append(&f, "\0\1\0\2", 4, NULL);
    for (i = 0; i < 2; i++) {
        (void)pc_buf_append(&f, rec, 10, NULL);
    }
    (void)pc_buf_append(&f, "\0\2\0\5", 4, NULL);
    for (i = 0; i < 5; i++) {
        (void)pc_buf_append(&f, rec, 10, NULL);
        (void)pc_buf_append(&f, "\0\0\0\2\0N\0\0", 8, NULL);
    }
    CHECK_EQ_INT(pc_psd_aco_parse(f.data, f.len, &a, NULL), PC_OK);
    CHECK_EQ_UINT(a.n, 5);
    for (i = 0; i < a.n; i++) {
        CHECK_EQ_STR(pc_psd_str_c(&a.v[i].name), "N");
    }
    pc_psd_aco_fini(&a);
    pc_buf_fini(&f);
}

static pc_psd_bytes nested_grd(size_t levels)
{
    pc_psd_desc d;
    pc_psd_bytes body = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    pc_psd_bytes out;
    size_t i;
    (void)pc_psd_desc_init(&d, "null", NULL);
    for (i = 0; i < levels; i++) {
        pc_psd_desc o;
        pc_psd_value v;
        (void)pc_psd_desc_init(&o, "null", NULL);
        (void)pc_psd_value_object(&d, &v, NULL);
        (void)pc_psd_desc_add(&o, "in  ", &v, NULL);
        d = o;
    }
    (void)pc_psd_vdesc_to_bytes(&d, &body, NULL);
    pc_psd_desc_fini(&d);
    (void)pc_buf_append(&b, "8BGR\0\5", 6, NULL);
    (void)pc_buf_append(&b, body.data, body.len, NULL);
    pc_psd_bytes_fini(&body);
    out.data = b.data;
    out.len = b.len;
    return out;
}

static void test_grd_deeply_nested_descriptor_hits_the_depth_limit(void)
{
    pc_psd_grd_gradient *g = NULL;
    size_t n = 0;
    pc_psd_bytes deep = nested_grd(PC_PSD_DESC_MAX_DEPTH + 5);
    pc_psd_bytes shallow = nested_grd(4);
    pc_psd_bytes edge = nested_grd(PC_PSD_DESC_MAX_DEPTH + 1);
    CHECK_EQ_INT(pc_psd_grd_parse(deep.data, deep.len, &g, &n, NULL), PC_ERR_LIMIT);
    CHECK(pc_psd_grd_parse(shallow.data, shallow.len, &g, &n, NULL) != PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_psd_grd_parse(edge.data, edge.len, &g, &n, NULL), PC_ERR_LIMIT);
    pc_psd_bytes_fini(&deep);
    pc_psd_bytes_fini(&shallow);
    pc_psd_bytes_fini(&edge);
}

/* One-stop gradient whose colour is class_id with every key set to x. */
static pc_psd_bytes colour_grd(const char *class_id, const char *const *keys, size_t nk, double x)
{
    pc_psd_desc colour;
    pc_psd_desc stop;
    pc_psd_desc grad;
    pc_psd_value v;
    pc_psd_value list;
    size_t i;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    (void)pc_psd_desc_init(&colour, class_id, NULL);
    for (i = 0; i < nk; i++) {
        v = pc_psd_value_double(x);
        (void)pc_psd_desc_add(&colour, keys[i], &v, NULL);
    }
    (void)pc_psd_desc_init(&stop, "Clrt", NULL);
    (void)pc_psd_value_object(&colour, &v, NULL);
    (void)pc_psd_desc_add(&stop, "Clr ", &v, NULL);
    (void)pc_psd_value_enum("Clry", "UsrS", &v, NULL);
    (void)pc_psd_desc_add(&stop, "Type", &v, NULL);
    v = pc_psd_value_int(0);
    (void)pc_psd_desc_add(&stop, "Lctn", &v, NULL);
    v = pc_psd_value_int(50);
    (void)pc_psd_desc_add(&stop, "Mdpn", &v, NULL);
    (void)pc_psd_value_object(&stop, &v, NULL);
    (void)pc_psd_value_list_push(&list, &v, NULL);
    (void)pc_psd_desc_init(&grad, "Grdn", NULL);
    (void)pc_psd_desc_add(&grad, "Clrs", &list, NULL);
    return grd_file(&grad);
}

static bool first_colour(const pc_psd_bytes *b, pc_psd_grd_color *out)
{
    pc_psd_grd_gradient *g = NULL;
    size_t n = 0;
    bool ok = pc_psd_grd_parse(b->data, b->len, &g, &n, NULL) == PC_OK && n == 1 && g[0].nstops == 1;
    if (ok) {
        *out = g[0].stops[0].color;
    }
    pc_psd_grd_free(g, n);
    return ok;
}

static void test_grd_every_colour_model_is_clamped_and_finite(void)
{
    static const char *const rgb[3] = {"Rd  ", "Grn ", "Bl  "};
    static const char *const hsb[3] = {"H   ", "Strt", "Brgh"};
    static const char *const cmyk[4] = {"Cyn ", "Mgnt", "Ylw ", "Blck"};
    static const char *const lab[3] = {"Lmnc", "A   ", "B   "};
    static const char *const gray[1] = {"Gry "};
    const double hostile[6] = {1e300, 3.0e38, -3.0e38, NAN, INFINITY, -INFINITY};
    int i;
    int k;
    for (i = 0; i < 6; i++) {
        double x = hostile[i];
        float unit = isnan(x) || x < 0.0 ? 0.0f : 1.0f;
        float lab_l = isnan(x) || x < 0.0 ? 0.0f : 100.0f;
        float lab_ab = isnan(x) ? 0.0f : x > 0.0 ? 127.0f : -128.0f;
        pc_psd_grd_color c;
        pc_psd_bytes b = colour_grd("RGBC", rgb, 3, x);
        CHECK(first_colour(&b, &c) && c.model == PC_PSD_GRD_RGB);
        for (k = 0; k < 3; k++) {
            CHECK(c.c[k] == unit);
        }
        pc_psd_bytes_fini(&b);
        b = colour_grd("HSBC", hsb, 3, x);
        CHECK(first_colour(&b, &c) && c.model == PC_PSD_GRD_HSB);
        CHECK(isfinite(c.c[0]) && c.c[0] >= 0.0f && c.c[0] <= 360.0f);
        CHECK(c.c[1] == unit && c.c[2] == unit);
        pc_psd_bytes_fini(&b);
        b = colour_grd("CMYC", cmyk, 4, x);
        CHECK(first_colour(&b, &c) && c.model == PC_PSD_GRD_CMYK);
        for (k = 0; k < 4; k++) {
            CHECK(c.c[k] == unit);
        }
        pc_psd_bytes_fini(&b);
        b = colour_grd("LbCl", lab, 3, x);
        CHECK(first_colour(&b, &c) && c.model == PC_PSD_GRD_LAB);
        CHECK(c.c[0] == lab_l && c.c[1] == lab_ab && c.c[2] == lab_ab);
        pc_psd_bytes_fini(&b);
        b = colour_grd("Grsc", gray, 1, x);
        CHECK(first_colour(&b, &c) && c.model == PC_PSD_GRD_GRAY);
        CHECK(c.c[0] == unit);
        pc_psd_bytes_fini(&b);
    }
}

static float hue_of(double h)
{
    static const char *const keys[3] = {"H   ", "Strt", "Brgh"};
    pc_psd_grd_color c;
    pc_psd_bytes b = colour_grd("HSBC", keys, 3, h);
    float r = first_colour(&b, &c) ? c.c[0] : -1.0f;
    pc_psd_bytes_fini(&b);
    return r;
}

static void test_grd_hue_wraps_rather_than_clamping(void)
{
    CHECK_NEAR(hue_of(370.0), 10.0, 0.01);
    CHECK_NEAR(hue_of(-10.0), 350.0, 0.01);
    CHECK_NEAR(hue_of(123.0), 123.0, 0.01);
    CHECK(hue_of(360.0) == 360.0f);
    CHECK(hue_of(0.0) == 0.0f);
}

static void test_empty_and_signature_only_inputs_error(void)
{
    pc_psd_grd_gradient *g = NULL;
    size_t n = 0;
    pc_psd_ase a;
    pc_psd_aco c;
    pc_error e;
    CHECK(pc_psd_grd_parse((const uint8_t *)"", 0, &g, &n, NULL) != PC_OK);
    CHECK(pc_psd_ase_parse((const uint8_t *)"", 0, &a, NULL) != PC_OK);
    CHECK(pc_psd_aco_parse((const uint8_t *)"", 0, &c, NULL) != PC_OK);
    CHECK(pc_psd_grd_parse((const uint8_t *)"8BGR", 4, &g, &n, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "unexpected end");
    CHECK(pc_psd_grd_parse((const uint8_t *)"8BG", 3, &g, &n, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "unexpected end");
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEF", 4, &a, NULL) != PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_bitflips_never_crash);
    RUN(test_grd_parsed_values_are_sane);
    RUN(test_grd_gradient_count_is_capped);
    RUN(test_grd_stop_counts_are_capped);
    RUN(test_grd_non_finite_opacity_and_colour_do_not_survive);
    RUN(test_grd_midpoint_stored_as_a_double_is_still_sanitised);
    RUN(test_aco_cmyk_is_stored_inverted_and_reads_back_as_ink);
    RUN(test_grd_unrepresentable_numbers_follow_the_documented_policy);
    RUN(test_aco_and_ase_never_split_a_surrogate_pair);
    RUN(test_grd_out_of_range_numbers_are_clamped);
    RUN(test_grd_huge_and_nan_components);
    RUN(test_ase_round_trip_preserves_swatch_count);
    RUN(test_ase_swatch_cap_is_enforced_on_hostile_input);
    RUN(test_ase_over_long_name_is_truncated_on_read);
    RUN(test_aco_round_trip_preserves_swatch_count);
    RUN(test_aco_over_long_name_is_truncated_on_read);
    RUN(test_aco_and_ase_write_cap_the_name);
    RUN(test_truncation);
    RUN(test_ase_structurally_hostile_blocks);
    RUN(test_aco_mismatched_section_counts_do_not_crash);
    RUN(test_grd_deeply_nested_descriptor_hits_the_depth_limit);
    RUN(test_grd_every_colour_model_is_clamped_and_finite);
    RUN(test_grd_hue_wraps_rather_than_clamping);
    RUN(test_empty_and_signature_only_inputs_error);
    return pc_t_finish();
}
