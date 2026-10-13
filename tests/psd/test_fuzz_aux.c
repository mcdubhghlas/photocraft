/* Ports the swatch and auxiliary-parser properties added to crates/psd/tests/proptests.rs and the
 * new cargo-fuzz targets crates/psd/fuzz/fuzz_targets/{aco,ase,grd,metadata,patterns,
 * filter_effects,resources,tagged}.rs. Properties become seeded random loops; each fuzz target's
 * invariants run on random bytes and on damaged valid inputs (never-crash tests; run them under
 * ASan/UBSan). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/bytes.h"
#include "harness.h"
#include "psd/aco.h"
#include "psd/ase.h"
#include "psd/filter_effects.h"
#include "psd/grd.h"
#include "psd/internal.h"
#include "psd/misc.h"
#include "psd/patterns.h"
#include "psd/resources.h"
#include "psd/tagged.h"

static uint64_t rng_state = 0x9e3779b97f4a7c15ull;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

static float unit(void)
{
    return roundf((float)(rnd() % 10001) / 10000.0f * 4096.0f) / 4096.0f;
}

/* UTF-16 code units of UTF-8 text. */
static size_t utf16_len(const pc_psd_str *s)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < s->len; i++) {
        uint8_t b = (uint8_t)s->s[i];
        if ((b & 0xC0u) != 0x80u) {
            n += b >= 0xF0u ? 2u : 1u;
        }
    }
    return n;
}

static void ascii_name(char *buf, size_t max)
{
    size_t n = rnd() % (max + 1);
    size_t i;
    for (i = 0; i < n; i++) {
        buf[i] = (char)(' ' + rnd() % 95);
    }
    buf[n] = '\0';
}

/* ---- fuzz target invariants ---- */

static void inv_aco(const uint8_t *d, size_t n)
{
    pc_psd_aco f;
    pc_psd_aco back;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    size_t i;
    if (pc_psd_aco_parse(d, n, &f, NULL) != PC_OK) {
        return;
    }
    CHECK(f.n <= PC_PSD_ACO_MAX_SWATCHES);
    for (i = 0; i < f.n; i++) {
        CHECK(utf16_len(&f.v[i].name) <= PC_PSD_ACO_MAX_NAME_UNITS);
    }
    CHECK_EQ_INT(pc_psd_aco_write(f.v, f.n, &out, NULL), PC_OK);
    if (pc_psd_aco_parse(out.data, out.len, &back, NULL) == PC_OK) {
        CHECK_EQ_UINT(back.n, f.n);
        CHECK_EQ_INT(pc_psd_aco_write(back.v, back.n, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &out));
        pc_psd_aco_fini(&back);
    } else {
        CHECK(false);
    }
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&again);
    pc_psd_aco_fini(&f);
}

static void check_ase_swatch(const pc_psd_ase_swatch *s)
{
    int k;
    CHECK(utf16_len(&s->name) <= PC_PSD_ASE_MAX_NAME_UNITS);
    if (s->color.model != PC_PSD_ASE_OTHER) {
        for (k = 0; k < 4; k++) {
            CHECK(isfinite(s->color.c[k]));
        }
    }
}

static void inv_ase(const uint8_t *d, size_t n)
{
    pc_psd_ase f;
    pc_psd_ase back;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    size_t i;
    size_t k;
    if (pc_psd_ase_parse(d, n, &f, NULL) != PC_OK) {
        return;
    }
    CHECK(pc_psd_ase_swatch_count(&f) <= PC_PSD_ASE_MAX_SWATCHES);
    for (i = 0; i < f.n; i++) {
        if (f.entries[i].group) {
            for (k = 0; k < f.entries[i].n; k++) {
                check_ase_swatch(&f.entries[i].swatches[k]);
            }
        } else {
            check_ase_swatch(&f.entries[i].swatch);
        }
    }
    CHECK_EQ_INT(pc_psd_ase_write(&f, &out, NULL), PC_OK);
    if (pc_psd_ase_parse(out.data, out.len, &back, NULL) == PC_OK) {
        CHECK_EQ_UINT(pc_psd_ase_swatch_count(&back), pc_psd_ase_swatch_count(&f));
        CHECK_EQ_INT(pc_psd_ase_write(&back, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &out));
        pc_psd_ase_fini(&back);
    } else {
        CHECK(false);
    }
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&again);
    pc_psd_ase_fini(&f);
}

static bool in_range(float v, float lo, float hi)
{
    return isfinite(v) && v >= lo && v <= hi;
}

static void inv_grd(const uint8_t *d, size_t n)
{
    pc_psd_grd_gradient *g = NULL;
    pc_psd_grd_gradient *back = NULL;
    size_t ng = 0;
    size_t nb = 0;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    size_t i;
    size_t s;
    if (pc_psd_grd_parse(d, n, &g, &ng, NULL) != PC_OK) {
        return;
    }
    CHECK(ng <= PC_PSD_GRD_MAX_GRADIENTS);
    for (i = 0; i < ng; i++) {
        CHECK(g[i].nstops <= PC_PSD_GRD_MAX_STOPS && g[i].nopacity <= PC_PSD_GRD_MAX_STOPS);
        for (s = 0; s < g[i].nopacity; s++) {
            CHECK(in_range(g[i].opacity[s].location, 0, 1) && in_range(g[i].opacity[s].opacity, 0, 1) &&
                  in_range(g[i].opacity[s].midpoint, 0, 1));
        }
        for (s = 0; s < g[i].nstops; s++) {
            const pc_psd_grd_stop *st = &g[i].stops[s];
            const float *c = st->color.c;
            CHECK(in_range(st->location, 0, 1) && in_range(st->midpoint, 0, 1));
            switch (st->color.model) {
            case PC_PSD_GRD_RGB:
                CHECK(in_range(c[0], 0, 1) && in_range(c[1], 0, 1) && in_range(c[2], 0, 1));
                break;
            case PC_PSD_GRD_CMYK:
                CHECK(in_range(c[0], 0, 1) && in_range(c[1], 0, 1) && in_range(c[2], 0, 1) &&
                      in_range(c[3], 0, 1));
                break;
            case PC_PSD_GRD_HSB:
                CHECK(in_range(c[0], 0, 360) && in_range(c[1], 0, 1) && in_range(c[2], 0, 1));
                break;
            case PC_PSD_GRD_LAB:
                CHECK(in_range(c[0], 0, 100) && in_range(c[1], -128, 127) && in_range(c[2], -128, 127));
                break;
            case PC_PSD_GRD_GRAY:
                CHECK(in_range(c[0], 0, 1));
                break;
            default:
                break;
            }
        }
    }
    CHECK_EQ_INT(pc_psd_grd_write(g, ng, &out, NULL), PC_OK);
    if (pc_psd_grd_parse(out.data, out.len, &back, &nb, NULL) == PC_OK) {
        CHECK_EQ_UINT(nb, ng);
        CHECK_EQ_INT(pc_psd_grd_write(back, nb, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &out));
        pc_psd_grd_free(back, nb);
    } else {
        CHECK(false);
    }
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&again);
    pc_psd_grd_free(g, ng);
}

static void inv_shmd(const uint8_t *d, size_t n)
{
    pc_psd_meta_item *items = NULL;
    pc_psd_meta_item *back = NULL;
    size_t ni = 0;
    size_t nb = 0;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    size_t i;
    if (pc_psd_parse_shmd(d, n, &items, &ni, NULL) != PC_OK) {
        return;
    }
    CHECK_EQ_INT(pc_psd_write_shmd(items, ni, &out, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_parse_shmd(out.data, out.len, &back, &nb, NULL), PC_OK);
    CHECK_EQ_UINT(nb, ni);
    for (i = 0; i < ni && i < nb; i++) {
        CHECK(pc_psd_meta_item_eq(&items[i], &back[i]));
    }
    CHECK_EQ_INT(pc_psd_write_shmd(back, nb, &again, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&again, &out));
    pc_psd_meta_items_free(items, ni);
    pc_psd_meta_items_free(back, nb);
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&again);
}

static void check_patterns_equal(const pc_psd_pattern *a, size_t na, const pc_psd_pattern *b, size_t nb)
{
    size_t i;
    CHECK_EQ_UINT(na, nb);
    for (i = 0; i < na && i < nb; i++) {
        CHECK(pc_psd_pattern_eq(&a[i], &b[i]));
    }
}

static void inv_patterns(const uint8_t *d, size_t n)
{
    pc_psd_pattern *p = NULL;
    pc_psd_pattern *back = NULL;
    size_t np = 0;
    size_t nb = 0;
    pc_psd_bytes out = {NULL, 0};
    if (pc_psd_parse_pattern_block(d, n, &p, &np, NULL) == PC_OK) {
        if (pc_psd_write_pattern_block(p, np, &out, NULL) == PC_OK) {
            CHECK_EQ_INT(pc_psd_parse_pattern_block(out.data, out.len, &back, &nb, NULL), PC_OK);
            check_patterns_equal(back, nb, p, np);
            pc_psd_patterns_free(back, nb);
        } else {
            CHECK(false);
        }
        pc_psd_bytes_fini(&out);
        pc_psd_patterns_free(p, np);
    }
    if (pc_psd_parse_pat_file(d, n, &p, &np, NULL) == PC_OK) {
        if (pc_psd_write_pat_file(p, np, &out, NULL) == PC_OK) {
            CHECK_EQ_INT(pc_psd_parse_pat_file(out.data, out.len, &back, &nb, NULL), PC_OK);
            check_patterns_equal(back, nb, p, np);
            pc_psd_patterns_free(back, nb);
        } else {
            CHECK(false);
        }
        pc_psd_bytes_fini(&out);
        pc_psd_patterns_free(p, np);
    }
}

static void inv_fx(const uint8_t *d, size_t n)
{
    pc_psd_filter_effects fx;
    pc_psd_filter_effects back;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes again = {NULL, 0};
    size_t i;
    size_t s;
    if (pc_psd_filter_effects_parse(d, n, &fx, NULL) != PC_OK) {
        return;
    }
    for (i = 0; i < fx.n; i++) {
        const pc_psd_fx_item *it = &fx.items[i];
        size_t w;
        size_t h;
        pc_psd_bytes dec = {NULL, 0};
        if (it->depth <= 0xffffu && pc_psd_rect_size(it->rect, &w, &h, NULL) == PC_OK) {
            for (s = 0; s < it->nslots; s++) {
                if (it->slot_written[s] &&
                    pc_psd_fx_plane_decode(&it->slots[s], w, h, (uint16_t)it->depth, &dec, NULL) == PC_OK) {
                    pc_psd_bytes_fini(&dec);
                }
            }
        }
        if (it->has_mask && pc_psd_rect_size(it->mask_rect, &w, &h, NULL) == PC_OK &&
            pc_psd_fx_plane_decode(&it->mask, w, h, 8, &dec, NULL) == PC_OK) {
            pc_psd_bytes_fini(&dec);
        }
    }
    CHECK_EQ_INT(pc_psd_filter_effects_to_bytes(&fx, &out, NULL), PC_OK);
    if (pc_psd_filter_effects_parse(out.data, out.len, &back, NULL) == PC_OK) {
        CHECK(back.version == fx.version && back.n == fx.n);
        for (i = 0; i < fx.n && i < back.n; i++) {
            CHECK(pc_psd_fx_item_eq(&back.items[i], &fx.items[i]));
        }
        CHECK_EQ_INT(pc_psd_filter_effects_to_bytes(&back, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &out));
        pc_psd_filter_effects_fini(&back);
    } else {
        CHECK(false);
    }
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&again);
    pc_psd_filter_effects_fini(&fx);
}

static void inv_resources(const uint8_t *d, size_t n)
{
    pc_psd_resource r;
    pc_psd_resolution ri;
    pc_psd_version_info vi;
    uint16_t u16;
    int32_t i32;
    uint16_t *ids = NULL;
    size_t nids = 0;
    size_t xl = 0;
    memset(&r, 0, sizeof r);
    memcpy(r.signature, "8BIM", 4);
    r.id = n >= 2 ? pc_load_be16(d) : 0;
    r.data.data = (uint8_t *)(uintptr_t)(n >= 2 ? d + 2 : d);
    r.data.len = n >= 2 ? n - 2 : n;
    (void)pc_psd_res_layer_state(&r, &u16, NULL);
    (void)pc_psd_res_i32(&r, &i32, NULL);
    if (pc_psd_res_group_ids(&r, &ids, &nids, NULL) == PC_OK) {
        free(ids);
    }
    (void)pc_psd_res_is_known(r.id);
    (void)pc_psd_res_xmp(&r, &xl);
    if (pc_psd_resolution_parse(r.data.data, r.data.len, &ri, NULL) == PC_OK) {
        uint8_t b[16];
        pc_psd_resolution round;
        pc_psd_resolution_write(&ri, b);
        CHECK_EQ_INT(pc_psd_resolution_parse(b, 16, &round, NULL), PC_OK);
        CHECK(memcmp(&round, &ri, sizeof ri) == 0);
    }
    if (pc_psd_version_info_parse(r.data.data, r.data.len, &vi, NULL) == PC_OK) {
        pc_psd_bytes out = {NULL, 0};
        if (pc_psd_version_info_to_bytes(&vi, &out, NULL) == PC_OK) {
            pc_psd_bytes_fini(&out);
        }
        pc_psd_version_info_fini(&vi);
    }
}

static const char *const TAGGED_KEYS[20] = {"luni", "lsct", "lsdk", "lyid", "lnsr", "clbl", "infx",
                                            "knko", "tsly", "lmgm", "vmgm", "lspf", "lclr", "iOpa",
                                            "shmd", "PlLd", "SoLd", "lnk2", "lfx2", "FEid"};

static void tagged_views(const char key[4], const uint8_t *d, size_t n)
{
    pc_psd_block b;
    if (pc_psd_block_init(&b, key, d, n, NULL) != PC_OK) {
        return;
    }
    if (pc_psd_block_has_typed(&b)) {
        (void)pc_psd_block_check_typed(&b, NULL);
    }
    (void)pc_psd_block_check_structure(&b, NULL);
    pc_psd_block_fini(&b);
}

static void inv_tagged(const uint8_t *d, size_t n)
{
    if (n == 0) {
        tagged_views("luni", d, 0);
    } else {
        tagged_views(TAGGED_KEYS[d[0] % 20], d + 1, n - 1);
    }
}

typedef void (*target_fn)(const uint8_t *, size_t);

static void random_and_damaged(target_fn f, const pc_psd_bytes *valid, int rounds)
{
    uint8_t buf[400];
    int k;
    size_t i;
    for (k = 0; k < rounds; k++) {
        size_t n = rnd() % sizeof buf;
        for (i = 0; i < n; i++) {
            buf[i] = (uint8_t)rnd();
        }
        f(buf, n);
    }
    if (valid != NULL && valid->len > 0) {
        uint8_t *copy = malloc(valid->len);
        if (copy == NULL) {
            return;
        }
        f(valid->data, valid->len);
        for (k = 0; k < rounds; k++) {
            size_t at = rnd() % valid->len;
            memcpy(copy, valid->data, valid->len);
            copy[at] = (uint8_t)((copy[at] ^ (1u << (rnd() % 8))) + rnd() % 256);
            f(copy, valid->len);
            f(copy, rnd() % (valid->len + 1));
        }
        free(copy);
    }
}

/* ---- sample inputs ---- */

static pc_psd_bytes grd_sample(void)
{
    pc_psd_grd_gradient g;
    pc_psd_grd_stop s;
    pc_psd_grd_opacity o = {0.5f, 1.0f, 0.5f};
    pc_psd_bytes out = {NULL, 0};
    memset(&g, 0, sizeof g);
    memset(&s, 0, sizeof s);
    (void)pc_psd_str_set(&g.name, "d", 1, NULL);
    s.location = 0.25f;
    s.midpoint = 0.5f;
    s.color.model = PC_PSD_GRD_RGB;
    s.color.c[0] = 1.0f;
    s.color.c[1] = 0.5f;
    g.stops = &s;
    g.nstops = 1;
    g.opacity = &o;
    g.nopacity = 1;
    (void)pc_psd_grd_write(&g, 1, &out, NULL);
    pc_psd_str_fini(&g.name);
    return out;
}

static pc_psd_bytes ase_sample(void)
{
    pc_psd_ase f;
    pc_psd_ase_entry e[2];
    pc_psd_bytes out = {NULL, 0};
    memset(e, 0, sizeof e);
    e[0].group = true;
    (void)pc_psd_str_set(&e[0].name, "g", 1, NULL);
    e[0].swatches = calloc(1, sizeof *e[0].swatches);
    if (e[0].swatches != NULL) {
        (void)pc_psd_str_set(&e[0].swatches[0].name, "d", 1, NULL);
        e[0].swatches[0].color.c[0] = 1.0f;
        e[0].swatches[0].kind = PC_PSD_ASE_PROCESS;
        e[0].n = 1;
    }
    (void)pc_psd_str_set(&e[1].swatch.name, "d", 1, NULL);
    e[1].swatch.color.c[0] = 1.0f;
    e[1].swatch.kind = PC_PSD_ASE_PROCESS;
    f.entries = e;
    f.n = 2;
    (void)pc_psd_ase_write(&f, &out, NULL);
    pc_psd_ase_entry_fini(&e[0]);
    pc_psd_ase_entry_fini(&e[1]);
    return out;
}

static pc_psd_bytes aco_sample(void)
{
    pc_psd_aco_swatch s;
    pc_psd_bytes out = {NULL, 0};
    memset(&s, 0, sizeof s);
    (void)pc_psd_str_set(&s.name, "d", 1, NULL);
    s.color.kind = PC_PSD_ACO_RGB;
    s.color.v[0] = 1;
    s.color.v[1] = 2;
    s.color.v[2] = 3;
    (void)pc_psd_aco_write(&s, 1, &out, NULL);
    pc_psd_str_fini(&s.name);
    return out;
}

static pc_psd_bytes shmd_sample(void)
{
    pc_psd_meta_item it[2];
    pc_psd_bytes out = {NULL, 0};
    static const uint8_t a[4] = {1, 2, 3, 4};
    static const uint8_t b[6] = {9, 9, 9, 9, 9, 9};
    (void)pc_psd_meta_item_make("cmls", a, 4, &it[0], NULL);
    (void)pc_psd_meta_item_make("cust", b, 6, &it[1], NULL);
    (void)pc_psd_write_shmd(it, 2, &out, NULL);
    pc_psd_bytes_fini(&it[0].data);
    pc_psd_bytes_fini(&it[1].data);
    return out;
}

/* A small valid pattern (pattern_strategy in proptests.rs). */
static void make_pattern(pc_psd_pattern *p, uint32_t mode, uint32_t w, uint32_t h, uint16_t depth, bool alpha,
                         uint64_t seed)
{
    size_t nc = pc_psd_pattern_mode_channels(mode);
    size_t len = (size_t)w * h * (depth / 8u);
    size_t c;
    size_t i;
    memset(p, 0, sizeof *p);
    p->mode = mode;
    p->width = w;
    p->height = h;
    p->depth = depth;
    (void)pc_psd_str_set(&p->name, "p", 1, NULL);
    (void)pc_psd_str_set(&p->id, "0bd2d3ba-1234-11d4-8f8f-aabbccddeeff", 36, NULL);
    p->channels = calloc(nc, sizeof *p->channels);
    if (p->channels == NULL) {
        return;
    }
    p->nchannels = nc;
    for (c = 0; c <= nc; c++) {
        pc_psd_bytes *dst = c < nc ? &p->channels[c] : &p->alpha.v;
        uint64_t s = c < nc ? seed + c + 1 : seed ^ 0xffffu;
        if (c == nc && !alpha) {
            break;
        }
        dst->data = malloc(len);
        dst->len = dst->data != NULL ? len : 0;
        for (i = 0; i < dst->len; i++) {
            dst->data[i] = (uint8_t)(i * 31 + s);
        }
        if (c == nc) {
            p->alpha.has = true;
        }
    }
}

static pc_psd_fx_plane fx_plane(uint8_t v)
{
    uint8_t px[16];
    pc_psd_fx_plane p;
    memset(px, v, sizeof px);
    memset(&p, 0, sizeof p);
    (void)pc_psd_fx_plane_encode(PC_PSD_RLE, px, 16, 4, 4, 8, &p, NULL);
    return p;
}

static pc_psd_bytes fx_bytes(size_t nitems, const bool *masks, char ids[][9])
{
    pc_psd_filter_effects fx;
    pc_psd_bytes out = {NULL, 0};
    pc_psd_rect rect = {1, 2, 5, 6};
    size_t i;
    fx.version = 3;
    fx.n = nitems;
    fx.items = calloc(nitems > 0 ? nitems : 1, sizeof *fx.items);
    if (fx.items == NULL) {
        return out;
    }
    for (i = 0; i < nitems; i++) {
        pc_psd_fx_item *it = &fx.items[i];
        it->id = malloc(strlen(ids[i]) + 1);
        if (it->id != NULL) {
            memcpy(it->id, ids[i], strlen(ids[i]) + 1);
        }
        it->version = 1;
        it->rect = rect;
        it->depth = 8;
        it->max_channels = 24;
        it->nslots = 26;
        it->slots = calloc(26, sizeof *it->slots);
        it->slot_written = calloc(26, sizeof *it->slot_written);
        if (it->slots != NULL && it->slot_written != NULL) {
            it->slots[0] = fx_plane(10);
            it->slots[25] = fx_plane(255);
            it->slot_written[0] = true;
            it->slot_written[25] = true;
        }
        if (masks[i]) {
            it->has_mask = true;
            it->mask_rect = rect;
            it->mask = fx_plane(128);
        }
    }
    (void)pc_psd_filter_effects_to_bytes(&fx, &out, NULL);
    pc_psd_filter_effects_fini(&fx);
    return out;
}

/* ---- properties ---- */

static void test_grd_roundtrip_keeps_stops_in_range(void)
{
    int k;
    for (k = 0; k < 128; k++) {
        size_t ns = rnd() % 12;
        size_t no = rnd() % 12;
        pc_psd_grd_stop stops[12];
        pc_psd_grd_opacity op[12];
        uint8_t tones[12];
        pc_psd_grd_gradient g;
        pc_psd_grd_gradient *back = NULL;
        size_t n = 0;
        pc_psd_bytes b = {NULL, 0};
        size_t i;
        memset(stops, 0, sizeof stops);
        memset(&g, 0, sizeof g);
        for (i = 0; i < ns; i++) {
            stops[i].location = unit();
            stops[i].midpoint = unit();
            tones[i] = (uint8_t)rnd();
            stops[i].color.model = PC_PSD_GRD_RGB;
            stops[i].color.c[0] = (float)tones[i] / 255.0f;
        }
        for (i = 0; i < no; i++) {
            op[i].location = unit();
            op[i].opacity = unit();
            op[i].midpoint = unit();
        }
        (void)pc_psd_str_set(&g.name, "p", 1, NULL);
        g.stops = stops;
        g.nstops = ns;
        g.opacity = op;
        g.nopacity = no;
        CHECK_EQ_INT(pc_psd_grd_write(&g, 1, &b, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_grd_parse(b.data, b.len, &back, &n, NULL), PC_OK);
        if (n == 1 && back[0].nstops == ns && back[0].nopacity == no) {
            for (i = 0; i < ns; i++) {
                const pc_psd_grd_stop *a = &back[0].stops[i];
                CHECK(in_range(a->location, 0, 1) && fabsf(a->location - stops[i].location) < 1.0f / 4096.0f);
                CHECK(in_range(a->midpoint, 0, 1) && fabsf(a->midpoint - stops[i].midpoint) < 0.01f);
                CHECK(a->color.model == PC_PSD_GRD_RGB);
                CHECK(fabsf(a->color.c[0] - (float)tones[i] / 255.0f) < 1.0f / 255.0f);
                CHECK(fabsf(a->color.c[1]) < 1.0f / 255.0f && fabsf(a->color.c[2]) < 1.0f / 255.0f);
            }
            for (i = 0; i < no; i++) {
                const pc_psd_grd_opacity *a = &back[0].opacity[i];
                CHECK(fabsf(a->location - op[i].location) < 1.0f / 4096.0f);
                CHECK(fabsf(a->opacity - op[i].opacity) < 0.01f);
                CHECK(fabsf(a->midpoint - op[i].midpoint) < 0.01f);
            }
        } else {
            CHECK(false);
        }
        pc_psd_grd_free(back, n);
        pc_psd_bytes_fini(&b);
        pc_psd_str_fini(&g.name);
    }
}

static void test_ase_and_aco_roundtrips_are_byte_stable(void)
{
    int k;
    for (k = 0; k < 128; k++) {
        size_t n = rnd() % 10;
        pc_psd_aco_swatch s[10];
        pc_psd_ase_entry e[2];
        pc_psd_ase f;
        pc_psd_bytes b = {NULL, 0};
        pc_psd_bytes again = {NULL, 0};
        size_t i;
        memset(s, 0, sizeof s);
        memset(e, 0, sizeof e);
        e[0].group = true;
        e[1].group = true;
        (void)pc_psd_str_set(&e[0].name, "g", 1, NULL);
        (void)pc_psd_str_set(&e[1].name, "h", 1, NULL);
        e[0].swatches = calloc(10, sizeof *e[0].swatches);
        e[1].swatches = calloc(10, sizeof *e[1].swatches);
        for (i = 0; i < n && e[0].swatches != NULL && e[1].swatches != NULL; i++) {
            char name[21];
            uint8_t tone = (uint8_t)rnd();
            uint16_t v = (uint16_t)rnd();
            pc_psd_ase_entry *dst = i < 3 ? &e[0] : &e[1];
            pc_psd_ase_swatch *sw = &dst->swatches[dst->n++];
            ascii_name(name, 20);
            (void)pc_psd_str_set(&sw->name, name, strlen(name), NULL);
            sw->color.c[0] = (float)tone / 255.0f;
            sw->kind = PC_PSD_ASE_PROCESS;
            (void)pc_psd_str_set(&s[i].name, name, strlen(name), NULL);
            switch (tone % 3) {
            case 0:
                s[i].color.kind = PC_PSD_ACO_RGB;
                s[i].color.v[0] = v;
                break;
            case 1:
                s[i].color.kind = PC_PSD_ACO_GRAY;
                s[i].color.v[0] = v;
                break;
            default:
                s[i].color.kind = PC_PSD_ACO_LAB;
                s[i].color.v[0] = v;
                s[i].color.v[1] = (uint16_t)(int16_t)-12800;
                s[i].color.v[2] = 12700;
                break;
            }
        }
        f.entries = e;
        f.n = 2;
        CHECK_EQ_INT(pc_psd_ase_write(&f, &b, NULL), PC_OK);
        {
            pc_psd_ase back;
            CHECK_EQ_INT(pc_psd_ase_parse(b.data, b.len, &back, NULL), PC_OK);
            CHECK_EQ_UINT(pc_psd_ase_swatch_count(&back), n);
            CHECK_EQ_INT(pc_psd_ase_write(&back, &again, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&again, &b));
            pc_psd_ase_fini(&back);
        }
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&again);
        CHECK_EQ_INT(pc_psd_aco_write(s, n, &b, NULL), PC_OK);
        {
            pc_psd_aco back;
            CHECK_EQ_INT(pc_psd_aco_parse(b.data, b.len, &back, NULL), PC_OK);
            CHECK_EQ_UINT(back.n, n);
            for (i = 0; i < n && i < back.n; i++) {
                CHECK(pc_psd_aco_swatch_eq(&back.v[i], &s[i]));
            }
            CHECK_EQ_INT(pc_psd_aco_write(back.v, back.n, &again, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&again, &b));
            pc_psd_aco_fini(&back);
        }
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&again);
        for (i = 0; i < 10; i++) {
            pc_psd_str_fini(&s[i].name);
        }
        pc_psd_ase_entry_fini(&e[0]);
        pc_psd_ase_entry_fini(&e[1]);
    }
}

/* aco_long_names_settle_after_one_truncation + aco_non_ascii_names_are_capped_and_still_round_trip */
static void test_aco_long_names_settle(void)
{
    int k;
    for (k = 0; k < 32; k++) {
        bool emoji = (k & 1) != 0;
        size_t len = emoji ? 600 + rnd() % 2000 : 1024 + rnd() % 2976;
        pc_buf name = PC_BUF_INIT;
        pc_psd_aco_swatch s;
        pc_psd_bytes once = {NULL, 0};
        pc_psd_bytes twice = {NULL, 0};
        pc_psd_aco back;
        size_t i;
        if (emoji) {
            (void)pc_buf_append(&name, "a", 1, NULL);
            for (i = 0; i < len / 2 + 1; i++) {
                (void)pc_buf_append(&name, "\xF0\x9F\x98\x80", 4, NULL);
            }
        } else {
            for (i = 0; i < len; i++) {
                (void)pc_buf_append(&name, "x", 1, NULL);
            }
        }
        memset(&s, 0, sizeof s);
        (void)pc_psd_str_set(&s.name, (const char *)name.data, name.len, NULL);
        s.color.kind = PC_PSD_ACO_GRAY;
        s.color.v[0] = 5000;
        CHECK_EQ_INT(pc_psd_aco_write(&s, 1, &once, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_aco_parse(once.data, once.len, &back, NULL), PC_OK);
        if (back.n == 1) {
            size_t units = utf16_len(&back.v[0].name);
            if (emoji) {
                CHECK(units <= PC_PSD_ACO_MAX_NAME_UNITS);
                CHECK(back.v[0].name.len <= name.len &&
                      memcmp(back.v[0].name.s, name.data, back.v[0].name.len) == 0);
            } else {
                CHECK_EQ_UINT(units, PC_PSD_ACO_MAX_NAME_UNITS);
            }
            CHECK_EQ_INT(pc_psd_aco_write(back.v, 1, &twice, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&twice, &once));
        }
        pc_psd_aco_fini(&back);
        pc_psd_bytes_fini(&once);
        pc_psd_bytes_fini(&twice);
        pc_psd_str_fini(&s.name);
        pc_buf_fini(&name);
    }
}

static void test_metadata_shmd_round_trip_is_exact(void)
{
    int k;
    for (k = 0; k < 128; k++) {
        size_t n = rnd() % 8;
        pc_psd_meta_item items[8];
        pc_psd_meta_item *back = NULL;
        size_t nb = 0;
        pc_psd_bytes b = {NULL, 0};
        pc_psd_bytes again = {NULL, 0};
        size_t i;
        size_t j;
        for (i = 0; i < n; i++) {
            char key[4];
            uint8_t data[40];
            size_t dl = rnd() % 40;
            for (j = 0; j < 4; j++) {
                key[j] = (char)rnd();
            }
            for (j = 0; j < dl; j++) {
                data[j] = (uint8_t)rnd();
            }
            CHECK_EQ_INT(pc_psd_meta_item_make(key, data, dl, &items[i], NULL), PC_OK);
        }
        CHECK_EQ_INT(pc_psd_write_shmd(items, n, &b, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_parse_shmd(b.data, b.len, &back, &nb, NULL), PC_OK);
        CHECK_EQ_UINT(nb, n);
        for (i = 0; i < n && i < nb; i++) {
            CHECK(pc_psd_meta_item_eq(&back[i], &items[i]));
        }
        CHECK_EQ_INT(pc_psd_write_shmd(back, nb, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &b));
        for (i = 0; i < n; i++) {
            pc_psd_bytes_fini(&items[i].data);
        }
        pc_psd_meta_items_free(back, nb);
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&again);
    }
}

static void test_pattern_blocks_round_trip(void)
{
    static const uint32_t modes[3] = {1, 3, 4};
    static const uint16_t depths[3] = {8, 16, 32};
    int k;
    for (k = 0; k < 128; k++) {
        size_t n = rnd() % 4;
        pc_psd_pattern ps[4];
        pc_psd_pattern *back = NULL;
        size_t nb = 0;
        pc_psd_bytes blk = {NULL, 0};
        pc_psd_bytes again = {NULL, 0};
        pc_psd_bytes file = {NULL, 0};
        size_t i;
        for (i = 0; i < n; i++) {
            uint64_t seed = ((uint64_t)rnd() << 32) | rnd();
            make_pattern(&ps[i], modes[rnd() % 3], 1 + rnd() % 7, 1 + rnd() % 5, depths[rnd() % 3],
                         rnd() % 2 != 0, seed);
        }
        CHECK_EQ_INT(pc_psd_write_pattern_block(ps, n, &blk, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_parse_pattern_block(blk.data, blk.len, &back, &nb, NULL), PC_OK);
        check_patterns_equal(back, nb, ps, n);
        CHECK_EQ_INT(pc_psd_write_pattern_block(back, nb, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &blk));
        pc_psd_patterns_free(back, nb);
        CHECK_EQ_INT(pc_psd_write_pat_file(ps, n, &file, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_parse_pat_file(file.data, file.len, &back, &nb, NULL), PC_OK);
        check_patterns_equal(back, nb, ps, n);
        pc_psd_patterns_free(back, nb);
        for (i = 0; i < n; i++) {
            pc_psd_pattern_fini(&ps[i]);
        }
        pc_psd_bytes_fini(&blk);
        pc_psd_bytes_fini(&again);
        pc_psd_bytes_fini(&file);
    }
}

static void test_filter_effects_round_trip(void)
{
    int k;
    for (k = 0; k < 128; k++) {
        size_t n = rnd() % 4;
        bool masks[4];
        char ids[4][9];
        pc_psd_bytes b;
        pc_psd_filter_effects back;
        pc_psd_bytes again = {NULL, 0};
        size_t i;
        for (i = 0; i < n; i++) {
            masks[i] = rnd() % 2 != 0;
            ascii_name(ids[i], 8);
        }
        b = fx_bytes(n, masks, ids);
        CHECK(b.len > 0);
        CHECK_EQ_INT(pc_psd_filter_effects_parse(b.data, b.len, &back, NULL), PC_OK);
        CHECK_EQ_UINT(back.n, n);
        CHECK_EQ_INT(pc_psd_filter_effects_to_bytes(&back, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &b));
        for (i = 0; i < back.n; i++) {
            pc_psd_bytes m = {NULL, 0};
            CHECK_EQ_UINT(back.items[i].has_mask, masks[i]);
            if (back.items[i].has_mask) {
                CHECK_EQ_INT(pc_psd_fx_plane_decode(&back.items[i].mask, 4, 4, 8, &m, NULL), PC_OK);
                CHECK(m.len == 16 && m.data[0] == 128 && m.data[15] == 128);
                pc_psd_bytes_fini(&m);
            }
        }
        pc_psd_filter_effects_fini(&back);
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&again);
    }
}

static void test_resolution_info_round_trips(void)
{
    int k;
    for (k = 0; k < 128; k++) {
        double dpi = 1.0 + (double)(rnd() % 9999000) / 1000.0;
        pc_psd_resolution ri = pc_psd_resolution_from_dpi(dpi);
        pc_psd_resolution back;
        uint8_t b[16];
        pc_psd_resolution_write(&ri, b);
        CHECK_EQ_INT(pc_psd_resolution_parse(b, 16, &back, NULL), PC_OK);
        CHECK(memcmp(&back, &ri, sizeof ri) == 0);
    }
}

/* resource_parsed / tagged views / structural keys never crash on arbitrary data. */
static void test_views_never_crash(void)
{
    static const char *const known[13] = {"luni", "lsct", "lsdk", "lyid", "lnsr", "clbl", "infx",
                                          "knko", "tsly", "lmgm", "vmgm", "lspf", "lclr"};
    static const char *const structural[8] = {"PlLd", "SoLd", "SoLE", "lnk2", "lnk3", "lnkD", "lfx2", "FEid"};
    uint8_t buf[202];
    int k;
    size_t i;
    for (k = 0; k < 512; k++) {
        size_t n = rnd() % 200;
        char key[4];
        for (i = 0; i < n + 2; i++) {
            buf[i] = (uint8_t)rnd();
        }
        inv_resources(buf, n % 66);
        for (i = 0; i < 4; i++) {
            key[i] = (char)rnd();
        }
        tagged_views(key, buf, n);
        tagged_views(known[rnd() % 13], buf, n % 64);
        tagged_views(structural[rnd() % 8], buf, n);
    }
    CHECK(true);
}

/* ---- fuzz targets on random bytes and damaged valid inputs ---- */

static void test_fuzz_targets(void)
{
    pc_psd_bytes grd = grd_sample();
    pc_psd_bytes ase = ase_sample();
    pc_psd_bytes aco = aco_sample();
    pc_psd_bytes shmd = shmd_sample();
    pc_psd_pattern p;
    pc_psd_bytes pat_block = {NULL, 0};
    pc_psd_bytes pat_file = {NULL, 0};
    bool mask = true;
    char ids[1][9] = {"x"};
    pc_psd_bytes fx = fx_bytes(1, &mask, ids);
    pc_psd_bytes res = {NULL, 0};
    uint8_t tagged[17] = {15, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    pc_psd_bytes tg = {tagged, sizeof tagged};
    make_pattern(&p, 3, 4, 3, 8, false, 7);
    (void)pc_psd_write_pattern_block(&p, 1, &pat_block, NULL);
    (void)pc_psd_write_pat_file(&p, 1, &pat_file, NULL);
    {
        pc_psd_resolution ri = pc_psd_resolution_from_dpi(300.0);
        uint8_t b[18] = {0x03, 0xed};
        pc_psd_resolution_write(&ri, b + 2);
        (void)pc_psd_bytes_set(&res, b, 18, NULL);
    }
    random_and_damaged(inv_grd, &grd, 512);
    random_and_damaged(inv_ase, &ase, 512);
    random_and_damaged(inv_aco, &aco, 512);
    random_and_damaged(inv_shmd, &shmd, 512);
    random_and_damaged(inv_patterns, &pat_block, 512);
    random_and_damaged(inv_patterns, &pat_file, 512);
    random_and_damaged(inv_fx, &fx, 512);
    random_and_damaged(inv_resources, &res, 512);
    random_and_damaged(inv_tagged, &tg, 512);
    pc_psd_pattern_fini(&p);
    pc_psd_bytes_fini(&grd);
    pc_psd_bytes_fini(&ase);
    pc_psd_bytes_fini(&aco);
    pc_psd_bytes_fini(&shmd);
    pc_psd_bytes_fini(&pat_block);
    pc_psd_bytes_fini(&pat_file);
    pc_psd_bytes_fini(&fx);
    pc_psd_bytes_fini(&res);
}

int main(int argc, char **argv)
{
    RUN(test_grd_roundtrip_keeps_stops_in_range);
    RUN(test_ase_and_aco_roundtrips_are_byte_stable);
    RUN(test_aco_long_names_settle);
    RUN(test_metadata_shmd_round_trip_is_exact);
    RUN(test_pattern_blocks_round_trip);
    RUN(test_filter_effects_round_trip);
    RUN(test_resolution_info_round_trips);
    RUN(test_views_never_crash);
    RUN(test_fuzz_targets);
    return pc_t_finish();
}
