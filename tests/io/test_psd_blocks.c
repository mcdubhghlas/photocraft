/* Unit tests of chunk I2's mapping modules: every #[test] of crates/io/src/blocks.rs (12) and
 * adjust_map.rs (14). effects_map.rs is in test_psd_effects.c. */
#include "harness.h"
#include "common.h"
#include "psd_dump.h"
#include "../psd/testgen.h"

#include "cms/lutfile.h"
#include "compose/gradient_fill.h"
#include "io/adjust_map.h"
#include "io/blocks.h"
#include "io/effects_map.h"

/* ---- helpers ---- */

static pc_psd_desc desc(const char *cls)
{
    pc_psd_desc d;
    must(pc_psd_desc_init(&d, cls, NULL));
    return d;
}

static pc_psd_value dbl(double v)
{
    return pc_psd_value_double(v);
}

static pc_psd_value en(const char *t, const char *v)
{
    pc_psd_value x;
    must(pc_psd_value_enum(t, v, &x, NULL));
    return x;
}

/* Versioned-descriptor bytes of d (finalised). */
static pc_psd_bytes vbytes(pc_psd_desc *d)
{
    pc_psd_bytes b = {NULL, 0};
    must(pc_psd_vdesc_to_bytes(d, &b, NULL));
    pc_psd_desc_fini(d);
    return b;
}

static void cat(pc_psd_bytes *b, const void *p, size_t n)
{
    uint8_t *v = xmalloc(b->len + n + 1);
    if (b->len > 0) {
        memcpy(v, b->data, b->len);
    }
    if (n > 0) {
        memcpy(v + b->len, p, n);
    }
    free(b->data);
    b->data = v;
    b->len += n;
}

static void json_of_adj(const pc_adjustment *a, pc_buf *out)
{
    DJ(out, pc_adjustment, a);
}

static bool adj_eq_json(const pc_adjustment *a, const pc_adjustment *b)
{
    pc_buf x = PC_BUF_INIT;
    pc_buf y = PC_BUF_INIT;
    bool eq;
    json_of_adj(a, &x);
    json_of_adj(b, &y);
    eq = x.len == y.len && memcmp(x.data, y.data, x.len) == 0;
    if (!eq) {
        (void)fprintf(stderr, "  %s\n  %s\n", (const char *)x.data, (const char *)y.data);
    }
    pc_buf_fini(&x);
    pc_buf_fini(&y);
    return eq;
}

static pc_adjustment parse_adj(const char *key, const uint8_t *d, size_t n, const uint8_t *cged, size_t cn,
                               pc_io_adj_channels ch)
{
    pc_adjustment a;
    must(pc_io_adjust_parse((const uint8_t *)key, d != NULL ? d : (const uint8_t *)"", n, cged, cn, ch, &a,
                            NULL));
    return a;
}

static pc_io_raw_blocks write_adj(const pc_adjustment *a)
{
    pc_io_raw_blocks r = {NULL, 0, 0};
    must(pc_io_adjust_write(a, &r, NULL));
    return r;
}

static pc_adjustment adj_json(const char *json)
{
    yyjson_doc *d = NULL;
    pc_adjustment a;
    must(pc_json_parse(json, strlen(json), &d, NULL));
    must(pc_adjustment_from_json(yyjson_doc_get_root(d), &a, NULL));
    yyjson_doc_free(d);
    return a;
}

/* write -> parse (RGB, CgEd when written) equals a. */
static void rt(const char *json)
{
    pc_adjustment a = adj_json(json);
    pc_io_raw_blocks w = write_adj(&a);
    const pc_io_raw_block *cg = pc_io_raw_blocks_find(&w, (const uint8_t *)"CgEd");
    pc_adjustment back;
    char key[5] = {0};
    CHECK(w.n > 0);
    memcpy(key, w.v[0].key, 4);
    back = parse_adj(key, w.v[0].data, w.v[0].len, cg != NULL ? cg->data : NULL, cg != NULL ? cg->len : 0,
                     PC_IO_ADJ_RGB);
    if (!adj_eq_json(&a, &back)) {
        PC_T_FAIL("round trip of %s", json);
    }
    pc_adjustment_fini(&a);
    pc_adjustment_fini(&back);
    pc_io_raw_blocks_fini(&w);
}

static void i16s(pc_psd_bytes *b, const int16_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t x[2] = {(uint8_t)((uint16_t)v[i] >> 8), (uint8_t)((uint16_t)v[i] & 0xff)};
        cat(b, x, 2);
    }
}

static void be32(pc_psd_bytes *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    cat(b, x, 4);
}

/* ---- blocks.rs ---- */

static pc_fill fill_rt(const pc_fill *f)
{
    uint8_t key[4];
    pc_buf data = PC_BUF_INIT;
    bool found = false;
    pc_fill back;
    memset(&back, 0, sizeof back);
    must(pc_io_write_fill(f, key, &data, NULL));
    must(pc_io_parse_fill(key, data.data, data.len, &found, &back, NULL));
    CHECK(found);
    pc_buf_fini(&data);
    return back;
}

static void test_gradient_fill_opacity_stops_become_alpha(void)
{
    pc_color red = pc_color_rgba(1.0f, 0.0f, 0.0f, 1.0f);
    pc_color clear = red;
    pc_color_stop stops[2];
    pc_fill f;
    pc_fill back;
    pc_compose_ramp ramp;
    bool found = false;
    float px[4];
    clear.alpha = 0.0f;
    stops[0] = (pc_color_stop){0.0f, red};
    stops[1] = (pc_color_stop){1.0f, clear};
    must(pc_fill_gradient(stops, 2, 90.0f, 1.0f, PC_GRADIENT_LINEAR, false, &f, NULL));
    back = fill_rt(&f);
    CHECK(back.kind == PC_FILL_GRADIENT);
    CHECK_EQ_UINT(back.u.gradient.nopacity_stops, 2);
    CHECK(back.u.gradient.opacity_stops[0].pos == 0.0f && back.u.gradient.opacity_stops[0].opacity == 1.0f);
    CHECK(back.u.gradient.opacity_stops[1].pos == 1.0f && back.u.gradient.opacity_stops[1].opacity == 0.0f);
    for (size_t i = 0; i < back.u.gradient.nstops; i++) {
        CHECK(back.u.gradient.stops[i].color.c[0] == 1.0f && back.u.gradient.stops[i].color.c[1] == 0.0f);
    }
    must(pc_compose_ramp_init(&back, &found, &ramp, NULL));
    CHECK(found);
    pc_compose_ramp_sample(&ramp, 0.0f, px);
    CHECK(px[3] == 1.0f);
    pc_compose_ramp_sample(&ramp, 1.0f, px);
    CHECK(px[3] < 0.01f);
    pc_compose_ramp_fini(&ramp);
    pc_fill_fini(&f);
    pc_fill_fini(&back);
    /* Opaque gradients come back without opacity stops. */
    stops[1] = (pc_color_stop){1.0f, PC_COLOR_WHITE};
    must(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &f, NULL));
    back = fill_rt(&f);
    CHECK_EQ_UINT(back.u.gradient.nopacity_stops, 0);
    pc_fill_fini(&f);
    pc_fill_fini(&back);
}

static void test_lab_descriptor_colours_round_trip(void)
{
    pc_psd_desc d = desc("LbCl");
    pc_psd_desc d2;
    pc_color c;
    pc_color back;
    tg_add(&d, "Lmnc", dbl(19.07));
    tg_add(&d, "A   ", dbl(52.29));
    tg_add(&d, "B   ", dbl(-85.08));
    CHECK(pc_io_color_from_desc(&d, &c));
    CHECK(c.mode == PC_COLOR_MODE_LAB);
    CHECK(fabsf(c.c[0] - 0.1907f) < 1e-5f && fabsf(c.c[1] * 255.0f - 128.0f - 52.29f) < 1e-3f &&
          fabsf(c.c[2] * 255.0f - 128.0f + 85.08f) < 1e-3f);
    must(pc_io_color_to_desc(&c, &d2, NULL));
    CHECK(pc_io_color_from_desc(&d2, &back));
    CHECK(back.mode == PC_COLOR_MODE_LAB);
    for (size_t i = 0; i < 3; i++) {
        CHECK(fabsf(back.c[i] - c.c[i]) < 1e-6f);
    }
    pc_psd_desc_fini(&d);
    pc_psd_desc_fini(&d2);
    /* Missing components are rejected, not defaulted. */
    d = desc("LbCl");
    tg_add(&d, "Lmnc", dbl(50.0));
    CHECK(!pc_io_color_from_desc(&d, &c));
    pc_psd_desc_fini(&d);
}

static bool range_eq(pc_blend_range a, uint8_t b0, uint8_t b1, uint8_t w0, uint8_t w1)
{
    return a.black[0] == b0 && a.black[1] == b1 && a.white[0] == w0 && a.white[1] == w1;
}

static void test_blending_ranges_map_to_blend_if(void)
{
    pc_psd_bytes full = {NULL, 0};
    pc_blend_if b;
    pc_blend_range pair[2];
    pc_psd_bytes back = {NULL, 0};
    must(pc_psd_blending_ranges_full(3, &full, NULL));
    must(pc_io_blend_if_from_ranges(full.data, full.len, &b, NULL));
    CHECK(pc_blend_if_is_default(&b) && b.nranges == 0);
    must(pc_io_blend_if_from_ranges(NULL, 0, &b, NULL));
    CHECK(b.nranges == 0);
    /* Gray: This Layer black split 10/40; Blue (entry 3): Underlying white at 200. */
    memcpy(full.data, (const uint8_t[4]){10, 40, 255, 255}, 4);
    memcpy(full.data + 3 * 8 + 4, (const uint8_t[4]){0, 0, 200, 200}, 4);
    must(pc_io_blend_if_from_ranges(full.data, full.len, &b, NULL));
    pc_blend_if_get(&b, 0, pair);
    CHECK(range_eq(pair[0], 10, 40, 255, 255) && pc_blend_range_is_full(pair[1]));
    pc_blend_if_get(&b, 3, pair);
    CHECK(pc_blend_range_is_full(pair[0]) && range_eq(pair[1], 0, 0, 200, 200));
    CHECK_EQ_UINT(b.nranges, 4);
    must(pc_io_ranges_from_blend_if(&b, 3, &back, NULL));
    CHECK(pc_psd_bytes_eq(&back, &full));
    pc_psd_bytes_fini(&back);
    pc_blend_if_fini(&b);
    /* The default writes full ranges for gray + each channel. */
    {
        pc_psd_bytes f4 = {NULL, 0};
        must(pc_psd_blending_ranges_full(4, &f4, NULL));
        must(pc_io_ranges_from_blend_if(&b, 4, &back, NULL));
        CHECK(pc_psd_bytes_eq(&back, &f4));
        pc_psd_bytes_fini(&back);
        pc_psd_bytes_fini(&f4);
    }
    /* A trimmed setting is padded to the document's channel count. */
    {
        pc_blend_range p[2] = {{{5, 5}, {255, 255}}, PC_BLEND_RANGE_FULL};
        pc_psd_bytes f2 = {NULL, 0};
        must(pc_blend_if_set(&b, 0, p, NULL));
        must(pc_io_ranges_from_blend_if(&b, 3, &back, NULL));
        CHECK_EQ_UINT(back.len, 32);
        CHECK_EQ_MEM(back.data, ((const uint8_t[8]){5, 5, 255, 255, 0, 0, 255, 255}), 8);
        must(pc_psd_blending_ranges_full(2, &f2, NULL));
        CHECK_EQ_MEM(back.data + 8, f2.data, 24);
        pc_psd_bytes_fini(&f2);
        pc_psd_bytes_fini(&back);
        pc_blend_if_fini(&b);
    }
    pc_psd_bytes_fini(&full);
}

static const uint8_t *raw_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    const pc_io_raw_block *b = pc_io_raw_blocks_find(ctx, key);
    if (b == NULL) {
        return NULL;
    }
    *len = b->len;
    return b->data != NULL ? b->data : (const uint8_t *)"";
}

static const uint8_t *none_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    (void)ctx;
    (void)key;
    (void)len;
    return NULL;
}

static const uint8_t *empty_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    (void)ctx;
    (void)key;
    *len = 0;
    return (const uint8_t *)"";
}

static const uint8_t *knko1_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    (void)ctx;
    if (memcmp(key, "knko", 4) != 0) {
        return NULL;
    }
    *len = 1;
    return (const uint8_t *)"\1";
}

static bool has_block(const pc_io_raw_blocks *r, const char *key, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < r->n; i++) {
        if (memcmp(r->v[i].key, key, 4) == 0 && r->v[i].len == len && memcmp(r->v[i].data, data, len) == 0) {
            return true;
        }
    }
    return false;
}

static void test_advanced_blending_blocks_round_trip(void)
{
    pc_advanced_blending a = {PC_KNOCKOUT_DEEP, true, false, false, true, true};
    pc_advanced_blending back;
    pc_io_raw_blocks raw = {NULL, 0, 0};
    pc_advanced_blending def = pc_advanced_blending_default();
    must(pc_io_put_advanced(&a, &raw, NULL));
    CHECK_EQ_UINT(raw.n, 6);
    CHECK(has_block(&raw, "knko", (const uint8_t[4]){2, 0, 0, 0}, 4));
    CHECK(has_block(&raw, "clbl", (const uint8_t[4]){0, 0, 0, 0}, 4));
    back = pc_io_advanced_from_blocks(raw_lookup, &raw);
    CHECK(pc_advanced_blending_eq(&back, &a));
    pc_io_raw_blocks_fini(&raw);
    /* Defaults: nothing added to a plain layer; existing blocks rewritten in place, in order. */
    must(pc_io_put_advanced(&def, &raw, NULL));
    CHECK_EQ_UINT(raw.n, 0);
    must(pc_io_raw_blocks_push(&raw, (const uint8_t *)"luni", (const uint8_t[4]){0}, 4, NULL));
    must(pc_io_raw_blocks_push(&raw, (const uint8_t *)"knko", (const uint8_t[4]){1, 0, 0, 0}, 4, NULL));
    must(pc_io_raw_blocks_push(&raw, (const uint8_t *)"clbl", (const uint8_t[1]){1}, 1, NULL));
    must(pc_io_put_advanced(&def, &raw, NULL));
    CHECK_EQ_UINT(raw.n, 3);
    CHECK(memcmp(raw.v[0].key, "luni", 4) == 0 && raw.v[0].len == 4);
    CHECK(memcmp(raw.v[1].key, "knko", 4) == 0 && raw.v[1].len == 4 &&
          memcmp(raw.v[1].data, (const uint8_t[4]){0, 0, 0, 0}, 4) == 0);
    CHECK(memcmp(raw.v[2].key, "clbl", 4) == 0 && raw.v[2].len == 4 &&
          memcmp(raw.v[2].data, (const uint8_t[4]){1, 0, 0, 0}, 4) == 0);
    pc_io_raw_blocks_fini(&raw);
    /* Missing and empty blocks read as Photoshop's defaults. */
    back = pc_io_advanced_from_blocks(none_lookup, NULL);
    CHECK(pc_advanced_blending_is_default(&back));
    back = pc_io_advanced_from_blocks(empty_lookup, NULL);
    CHECK(pc_advanced_blending_is_default(&back));
    CHECK(pc_io_advanced_from_blocks(knko1_lookup, NULL).knockout == PC_KNOCKOUT_SHALLOW);
}

static void test_brst_round_trip(void)
{
    bool has = true;
    pc_buf d = PC_BUF_INIT;
    CHECK_EQ_UINT(pc_io_parse_brst((const uint8_t[4]){0, 0, 0, 2}, 4), 4);
    CHECK_EQ_UINT(pc_io_parse_brst((const uint8_t[8]){0, 0, 0, 0, 0, 0, 0, 1}, 8), 3);
    must(pc_io_brst_data(5, &has, &d, NULL));
    CHECK(has && d.len == 8 && memcmp(d.data, (const uint8_t[8]){0, 0, 0, 0, 0, 0, 0, 2}, 8) == 0);
    must(pc_io_brst_data(0, &has, &d, NULL));
    CHECK(!has);
    pc_buf_fini(&d);
}

/* soLD + version 4 + versioned descriptor null { warp: w } (takes w). */
static pc_psd_bytes sold_with(const char *key, pc_psd_desc *inner)
{
    pc_psd_desc d = desc("null");
    pc_psd_bytes v = {NULL, 0};
    pc_psd_bytes body;
    cat(&v, "soLD", 4);
    be32(&v, 4);
    tg_add(&d, key, tg_obj(inner));
    body = vbytes(&d);
    cat(&v, body.data, body.len);
    pc_psd_bytes_fini(&body);
    return v;
}

static void test_placed_layer_warp_is_read(void)
{
    pc_psd_desc bounds = desc("Rctn");
    pc_psd_desc arc = desc("warp");
    pc_psd_desc b2;
    pc_psd_bytes data;
    pc_warp w;
    bool found = false;
    tg_add(&bounds, "Top ", tg_unit("#Pxl", 0.0));
    tg_add(&bounds, "Left", tg_unit("#Pxl", 0.0));
    tg_add(&bounds, "Btom", tg_unit("#Pxl", 30.0));
    tg_add(&bounds, "Rght", tg_unit("#Pxl", 60.0));
    must(pc_psd_desc_clone(&bounds, &b2, NULL));
    tg_add(&arc, "warpStyle", en("warpStyle", "warpArc"));
    tg_add(&arc, "warpValue", dbl(40.0));
    tg_add(&arc, "warpPerspective", dbl(0.0));
    tg_add(&arc, "warpPerspectiveOther", dbl(0.0));
    tg_add(&arc, "warpRotate", en("Ornt", "Hrzn"));
    tg_add(&arc, "bounds", tg_obj(&bounds));
    data = sold_with("warp", &arc);
    must(pc_io_parse_placed_warp((const uint8_t *)"SoLd", data.data, data.len, &found, &w, NULL));
    CHECK(found && w.style == PC_WARP_ARC && w.bend == 40.0);
    CHECK(w.bounds[0] == 0.0 && w.bounds[1] == 0.0 && w.bounds[2] == 60.0 && w.bounds[3] == 30.0);
    pc_warp_fini(&w);
    pc_psd_bytes_fini(&data);
    {
        double xs[16];
        double ys[16];
        pc_psd_desc mesh = desc("null");
        pc_psd_desc env = desc("customEnvelopeWarp");
        pc_psd_desc custom = desc("warp");
        pc_psd_value hv;
        pc_psd_value vv;
        pc_psd_value arr;
        for (int j = 0; j < 4; j++) {
            for (int i = 0; i < 4; i++) {
                xs[j * 4 + i] = (double)i * 20.0 + (i == 3 && j == 3 ? 7.0 : 0.0);
                ys[j * 4 + i] = (double)j * 10.0;
            }
        }
        memset(&hv, 0, sizeof hv);
        hv.kind = PC_PSD_V_UNIT_FLOATS;
        memcpy(hv.unit, "#Pxl", 4);
        hv.floats = xmalloc(sizeof xs);
        memcpy(hv.floats, xs, sizeof xs);
        hv.nfloats = 16;
        vv = hv;
        vv.floats = xmalloc(sizeof ys);
        memcpy(vv.floats, ys, sizeof ys);
        tg_add(&mesh, "Hrzn", hv);
        tg_add(&mesh, "Vrtc", vv);
        memset(&arr, 0, sizeof arr);
        arr.kind = PC_PSD_V_OBJECT_ARRAY;
        arr.prefix = 16;
        arr.desc = xmalloc(sizeof *arr.desc);
        *arr.desc = mesh;
        tg_add(&env, "meshPoints", arr);
        tg_add(&custom, "warpStyle", en("warpStyle", "warpCustom"));
        tg_add(&custom, "bounds", tg_obj(&b2));
        tg_add(&custom, "customEnvelopeWarp", tg_obj(&env));
        data = sold_with("warp", &custom);
        must(pc_io_parse_placed_warp((const uint8_t *)"SoLd", data.data, data.len, &found, &w, NULL));
        CHECK(found && w.style == PC_WARP_CUSTOM && w.has_mesh);
        if (found && w.has_mesh) {
            CHECK(w.mesh.points[15].x == 67.0 && w.mesh.points[15].y == 30.0);
            pc_warp_fini(&w);
        }
        pc_psd_bytes_fini(&data);
    }
    must(pc_io_parse_placed_warp((const uint8_t *)"PlLd", (const uint8_t[20]){0}, 20, &found, &w, NULL));
    CHECK(!found);
}

static void test_locks_and_labels(void)
{
    /* Reference values round-trip byte for byte; Background layers carry 0x0D. */
    static const uint32_t bits[] = {0, 1, 2, 4, 8, 0x0D, 0x80000000u, 0x8000000Fu};
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        pc_locks l = pc_io_locks_from_lspf(bits[i]);
        CHECK_EQ_UINT(pc_io_lspf_from_locks(&l), bits[i]);
    }
    /* Older PhotoCraft files wrote the artboard lock at 0x10; rewritten at 0x08. */
    {
        pc_locks l = pc_io_locks_from_lspf(0x10);
        CHECK(l.artboard);
        CHECK_EQ_UINT(pc_io_lspf_from_locks(&l), 0x08);
    }
    for (uint16_t i = 0; i < 8; i++) {
        CHECK_EQ_UINT(pc_io_label_index(pc_io_label_from_index(i)), i);
    }
}

static void test_fills_roundtrip(void)
{
    static const char *const fills[] = {
        "{\"Solid\":{\"mode\":\"Rgb\",\"c\":[1.0,0.5,0.0,0.0],\"alpha\":1.0}}",
        "{\"Solid\":{\"mode\":\"Cmyk\",\"c\":[0.1,0.2,0.3,0.4],\"alpha\":1.0}}",
        "{\"Solid\":{\"mode\":\"Grayscale\",\"c\":[0.25,0.0,0.0,0.0],\"alpha\":1.0}}",
        ("{\"Gradient\":{\"stops\":[[0.0,{\"mode\":\"Rgb\",\"c\":[1.0,0.0,0.0,0.0],\"alpha\":1.0}],[1.0,{"
         "\"mode\":\"Rgb\","
         "\"c\":[0.0,0.0,1.0,0.0],\"alpha\":1.0}]],\"angle\":45.0,\"scale\":1.0,\"style\":\"Reflected\","
         "\"reverse\":true}}"),
        ("{\"Gradient\":{\"stops\":[[0.0,{\"mode\":\"Rgb\",\"c\":[1.0,0.0,0.0,0.0],\"alpha\":1.0}],[0.5,{"
         "\"mode\":\"Rgb\","
         "\"c\":[0.0,1.0,0.0,0.0],\"alpha\":1.0}],[1.0,{\"mode\":\"Rgb\",\"c\":[0.0,0.0,1.0,0.0],\"alpha\":1."
         "0}]],"
         "\"angle\":-33.5,\"scale\":2.25,\"style\":\"Radial\",\"reverse\":false,\"opacity_stops\":[[0.0,1.0],"
         "[0.25,"
         "0.5],[1.0,0.0]],\"midpoints\":[0.25,0.75],\"offset\":[0.125,-0.375],\"dither\":true,\"align\":"
         "false}}"),
        ("{\"Pattern\":{\"name\":\"Bubbles\",\"scale\":0.5,\"id\":\"abc\",\"angle\":30.0,\"link\":false,"
         "\"phase\":[3.0,"
         "-2.0]}}"),
    };
    for (size_t i = 0; i < sizeof fills / sizeof fills[0]; i++) {
        yyjson_doc *d = NULL;
        pc_fill f;
        pc_fill back;
        must(pc_json_parse(fills[i], strlen(fills[i]), &d, NULL));
        if (pc_fill_from_json(yyjson_doc_get_root(d), &f, NULL) != PC_OK) {
            PC_T_FAIL("fill JSON %zu", i);
            yyjson_doc_free(d);
            continue;
        }
        yyjson_doc_free(d);
        back = fill_rt(&f);
        if (f.kind == PC_FILL_SOLID) {
            CHECK(back.kind == PC_FILL_SOLID && back.u.solid.mode == f.u.solid.mode);
            for (size_t k = 0; k < 4; k++) {
                CHECK(fabsf(back.u.solid.c[k] - f.u.solid.c[k]) < 1e-5f);
            }
        } else {
            CHECK(pc_fill_eq(&back, &f));
        }
        pc_fill_fini(&f);
        pc_fill_fini(&back);
    }
}

static void test_float_rgb_colors_parse(void)
{
    pc_psd_desc d = desc("RGBC");
    pc_color c;
    float rgb[3];
    tg_add(&d, "redFloat", dbl(1.0));
    tg_add(&d, "greenFloat", dbl(0.5));
    tg_add(&d, "blueFloat", dbl(0.25));
    CHECK(pc_io_color_from_desc(&d, &c));
    pc_color_to_rgb(&c, rgb);
    CHECK(rgb[0] == 1.0f && rgb[1] == 0.5f && rgb[2] == 0.25f);
    pc_psd_desc_fini(&d);
}

static void test_smart_transform_from_sold(void)
{
    static const double pts[8] = {10.0, 20.0, 110.0, 20.0, 110.0, 70.0, 10.0, 70.0};
    pc_psd_desc d = desc("null");
    pc_psd_desc sz = desc("Pnt ");
    pc_psd_value list;
    pc_psd_bytes data = {NULL, 0};
    pc_psd_bytes body;
    pc_str id = PC_STR_INIT;
    pc_affine a;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (size_t i = 0; i < 8; i++) {
        pc_psd_value v = dbl(pts[i]);
        must(pc_psd_value_list_push(&list, &v, NULL));
    }
    tg_add(&d, "Idnt", tg_text("uuid-1", true));
    tg_add(&d, "Trnf", list);
    tg_add(&sz, "Wdth", dbl(50.0));
    tg_add(&sz, "Hght", dbl(25.0));
    tg_add(&d, "Sz  ", tg_obj(&sz));
    cat(&data, "soLD", 4);
    be32(&data, 4);
    body = vbytes(&d);
    cat(&data, body.data, body.len);
    cat(&data, (const uint8_t[2]){0, 0}, 2);
    pc_psd_bytes_fini(&body);
    must(pc_io_parse_smart((const uint8_t *)"SoLd", data.data, data.len, &id, &a, NULL));
    CHECK_EQ_STR(pc_str_c(&id), "uuid-1");
    CHECK(a.m[0] == 2.0 && a.m[1] == 0.0 && a.m[2] == 0.0 && a.m[3] == 2.0 && a.m[4] == 10.0 &&
          a.m[5] == 20.0);
    must(pc_io_parse_smart((const uint8_t *)"PlLd", data.data, data.len, &id, &a, NULL));
    CHECK_EQ_UINT(id.len, 0);
    pc_str_fini(&id);
    pc_psd_bytes_fini(&data);
}

static void test_effects_switch(void)
{
    pc_psd_desc d = desc("null");
    pc_psd_bytes v = {NULL, 0};
    pc_psd_bytes body;
    bool on = true;
    tg_add(&d, "masterFXSwitch", pc_psd_value_bool(false));
    cat(&v, (const uint8_t[4]){0}, 4);
    body = vbytes(&d);
    cat(&v, body.data, body.len);
    must(pc_io_effects_enabled(v.data, v.len, &on, NULL));
    CHECK(!on);
    must(pc_io_effects_enabled((const uint8_t[4]){0}, 4, &on, NULL));
    CHECK(on);
    pc_psd_bytes_fini(&body);
    pc_psd_bytes_fini(&v);
}

static void test_tysh_garbage_is_none(void)
{
    bool found = true;
    pc_str t = PC_STR_INIT;
    pc_affine a;
    must(pc_io_parse_tysh((const uint8_t[10]){0}, 10, &found, &t, &a, NULL));
    CHECK(!found);
}

/* ---- adjust_map.rs ---- */

static pc_buf grdm_v1(const float (*stops)[4], size_t n)
{
    pc_adjustment a;
    pc_io_raw_blocks w;
    pc_buf b = PC_BUF_INIT;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_GRADIENT_MAP;
    a.u.gradient_map.stops = xmalloc(n * sizeof *a.u.gradient_map.stops);
    a.u.gradient_map.nstops = n;
    for (size_t i = 0; i < n; i++) {
        a.u.gradient_map.stops[i] =
            (pc_gradient_map_stop){stops[i][0], {stops[i][1], stops[i][2], stops[i][3]}};
    }
    w = write_adj(&a);
    must(pc_buf_append(&b, w.v[0].data, w.v[0].len, NULL));
    pc_io_raw_blocks_fini(&w);
    pc_adjustment_fini(&a);
    return b;
}

/* Colour of a parsed gradient map at t (linear between baked stops). */
static void map_at(const pc_buf *data, float t, float out[3])
{
    pc_adjustment a = parse_adj("grdm", data->data, data->len, NULL, 0, PC_IO_ADJ_RGB);
    out[0] = out[1] = out[2] = -1.0f;
    if (a.kind != PC_ADJ_GRADIENT_MAP) {
        PC_T_FAIL("not a gradient map");
        pc_adjustment_fini(&a);
        return;
    }
    for (size_t i = 0; i + 1 < a.u.gradient_map.nstops; i++) {
        const pc_gradient_map_stop *x = &a.u.gradient_map.stops[i];
        const pc_gradient_map_stop *y = &a.u.gradient_map.stops[i + 1];
        if (x->pos <= t && t <= y->pos) {
            float u = (t - x->pos) / fmaxf(y->pos - x->pos, 1e-6f);
            for (size_t k = 0; k < 3; k++) {
                out[k] = x->rgb[k] + (y->rgb[k] - x->rgb[k]) * u;
            }
            break;
        }
    }
    pc_adjustment_fini(&a);
}

static pc_buf v3(const pc_buf *v1, const char *m)
{
    pc_buf b = PC_BUF_INIT;
    must(pc_buf_append(&b, v1->data, 4, NULL));
    b.data[1] = 3;
    must(pc_buf_append(&b, m, 4, NULL));
    must(pc_buf_append(&b, v1->data + 4, v1->len - 4, NULL));
    return b;
}

static void test_gradient_map_v3_methods_and_smoothness(void)
{
    static const float classic[2][4] = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.0f}};
    static const float three[4][4] = {{0.0f, 0.0f, 0.0f, 0.0f},
                                      {0.25f, 1.0f, 0.0f, 0.0f},
                                      {0.75f, 0.0f, 0.0f, 1.0f},
                                      {1.0f, 1.0f, 1.0f, 1.0f}};
    pc_buf v1 = grdm_v1(classic, 2);
    pc_buf g = v3(&v1, "Gcls");
    pc_buf p = v3(&v1, "Perc");
    float c[3];
    float q[3];
    map_at(&g, 0.5f, c);
    CHECK(fabsf(c[0] - 0.5f) < 1e-3f && fabsf(c[2] - 0.5f) < 1e-3f);
    map_at(&p, 0.5f, q);
    CHECK(fabsf(q[0] - c[0]) > 0.02f);
    /* Smoothness 100 % (4096) bends a three-stop ramp. */
    {
        pc_buf plain = grdm_v1(three, 4);
        pc_buf smooth = grdm_v1(three, 4);
        size_t at = smooth.len - 2 - 16 - 2 - 4 - 2 - 2 - 4 - 2 - 2 - 2;
        float a[3];
        float b[3];
        smooth.data[at] = 0x10;
        smooth.data[at + 1] = 0;
        map_at(&plain, 0.4f, a);
        map_at(&smooth, 0.4f, b);
        CHECK(fabsf(a[0] - b[0]) > 1e-3f || fabsf(a[1] - b[1]) > 1e-3f || fabsf(a[2] - b[2]) > 1e-3f);
        pc_buf_fini(&plain);
        pc_buf_fini(&smooth);
    }
    /* Noise gradients (no colour stops) stay unsupported. */
    {
        pc_adjustment a =
            parse_adj("grdm", (const uint8_t[10]){0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, 10, NULL, 0, PC_IO_ADJ_RGB);
        CHECK(a.kind == PC_ADJ_UNSUPPORTED);
        pc_adjustment_fini(&a);
    }
    pc_buf_fini(&v1);
    pc_buf_fini(&g);
    pc_buf_fini(&p);
}

static void test_roundtrips(void)
{
    static const char *const cases[] = {
        "\"Invert\"",
        "{\"Threshold\":{\"level\":0.5019608}}",
        "{\"Posterize\":{\"levels\":4}}",
        "{\"BrightnessContrast\":{\"brightness\":20.0,\"contrast\":-10.0,\"legacy\":false}}",
        "{\"BrightnessContrast\":{\"brightness\":-150.0,\"contrast\":100.0,\"legacy\":true}}",
        "{\"Exposure\":{\"exposure\":1.5,\"offset\":-0.01,\"gamma\":0.9}}",
        "{\"Unsupported\":{\"psd_key\":\"selc\",\"raw\":[1,2,3]}}",
        ("{\"GradientMap\":{\"stops\":[[0.0,[0.0,0.0,0.0]],[0.5,[1.0,0.0,0.0]],[1.0,[1.0,1.0,1.0]]],"
         "\"reverse\":true,"
         "\"dither\":false}}"),
        "{\"GradientMap\":{\"stops\":[[0.0,[0.0,0.0,0.0]],[1.0,[1.0,1.0,1.0]]],\"reverse\":false,\"dither\":"
        "true}}",
        ("{\"SelectiveColor\":{\"relative\":false,\"adjustments\":[[0.0,0.0,0.0,0.0],[0.0,0.0,0.0,0.0],[0.0,"
         "0.0,0.0,"
         "0.0],[0.0,0.0,0.0,0.0],[0.0,0.0,0.0,0.0],[0.0,0.0,0.0,0.0],[0.0,0.0,0.0,0.0],[0.0,0.0,0.0,0.0],[0."
         "0,0.0,"
         "0.0,0.0]]}}"),
        "{\"Vibrance\":{\"vibrance\":35.0,\"saturation\":-12.0}}",
        "{\"Vibrance\":{\"vibrance\":0.0,\"saturation\":0.0}}",
        ("{\"ColorBalance\":{\"shadows\":[20.0,-10.0,5.0],\"midtones\":[-15.0,10.0,30.0],\"highlights\":[0.0,"
         "5.0,"
         "-100.0],\"preserve_luminosity\":true}}"),
        "{\"BlackWhite\":{\"weights\":[40.0,60.0,40.0,60.0,20.0,80.0],\"tint\":null}}",
        "{\"BlackWhite\":{\"weights\":[-200.0,300.0,0.0,10.0,90.0,30.0],\"tint\":[0.9,0.7,0.5]}}",
        ("{\"ChannelMixer\":{\"matrix\":[[0.5,0.3,0.2,0.0],[0.1,0.8,0.1,0.05],[0.0,0.2,0.9,-0.05]],"
         "\"monochrome\":false}}"),
        "{\"ChannelMixer\":{\"matrix\":[[0.4,0.4,0.2,0.1],[0.0,1.0,0.0,0.0],[-2.0,0.0,2.0,0.0]],"
        "\"monochrome\":true}}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        rt(cases[i]);
    }
    {
        /* Hue/Saturation with the default ranges and with an edited range. */
        pc_adjustment a;
        pc_buf j = PC_BUF_INIT;
        pc_adjustment_default_hue_saturation(&a);
        a.u.hue_saturation.hue = 30.0f;
        a.u.hue_saturation.saturation = -20.0f;
        a.u.hue_saturation.lightness = 5.0f;
        json_of_adj(&a, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
        a.u.hue_saturation.hue = -10.0f;
        a.u.hue_saturation.saturation = 0.0f;
        a.u.hue_saturation.lightness = 0.0f;
        a.u.hue_saturation.ranges[0] = (pc_hue_range){15.0f, -40.0f, 10.0f, {-60.0f, -20.0f, 10.0f, 30.0f}};
        a.u.hue_saturation.ranges[4].saturation = 25.0f;
        json_of_adj(&a, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
        a.u.hue_saturation.colorize = true;
        a.u.hue_saturation.hue = 200.0f;
        a.u.hue_saturation.saturation = 50.0f;
        pc_hue_range_defaults(a.u.hue_saturation.ranges);
        json_of_adj(&a, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
    }
    {
        /* Levels and curves (RGB); CMYK ink curves and levels keep their black record. */
        pc_adjustment lv;
        pc_buf j = PC_BUF_INIT;
        pc_levels_channel lc[4] = {{10.0f / 255.0f, 240.0f / 255.0f, 1.2f, 0.0f, 1.0f},
                                   {0.0f, 1.0f, 1.2f, 0.0f, 1.0f},
                                   {5.0f / 255.0f, 250.0f / 255.0f, 1.2f, 0.0f, 1.0f},
                                   {20.0f / 255.0f, 200.0f / 255.0f, 1.2f, 0.0f, 1.0f}};
        memset(&lv, 0, sizeof lv);
        lv.kind = PC_ADJ_LEVELS;
        lv.u.levels.master = lc[0];
        memcpy(lv.u.levels.per_channel, lc + 1, sizeof(lc[0]) * 3);
        lv.u.levels.black = pc_levels_channel_default();
        json_of_adj(&lv, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
        lv.u.levels.space = PC_TONE_CMYK;
        lv.u.levels.black = lc[2];
        {
            pc_io_raw_blocks w = write_adj(&lv);
            pc_adjustment back = parse_adj("levl", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_CMYK);
            CHECK(adj_eq_json(&lv, &back));
            pc_adjustment_fini(&back);
            pc_io_raw_blocks_fini(&w);
        }
    }
    rt("{\"Curves\":{\"master\":[{\"input\":0.0,\"output\":0.0},{\"input\":0.5019608,\"output\":0.5882353},"
       "{\"input\":1.0,\"output\":1.0}],\"per_channel\":[[{\"input\":0.0,\"output\":0.039215688},{\"input\":"
       "1.0,"
       "\"output\":1.0}],[{\"input\":0.0,\"output\":0.0},{\"input\":1.0,\"output\":0.9607843}],[{\"input\":0."
       "0,"
       "\"output\":0.0},{\"input\":0.2509804,\"output\":0.1254902},{\"input\":1.0,\"output\":1.0}]],"
       "\"space\":"
       "\"Rgb\",\"black\":[]}}");
    {
        pc_adjustment ink = adj_json(
            "{\"Curves\":{\"master\":[{\"input\":0.0,\"output\":0.0},{\"input\":1.0,\"output\":1.0}],"
            "\"per_channel\":[[{\"input\":0.0,\"output\":0.0},{\"input\":1.0,\"output\":1.0}],[{\"input\":0."
            "0,"
            "\"output\":0.11764706},{\"input\":1.0,\"output\":1.0}],[{\"input\":0.0,\"output\":0.0},{"
            "\"input\":1.0,"
            "\"output\":1.0}]],\"space\":\"Cmyk\",\"black\":[{\"input\":0.0,\"output\":0.0},{\"input\":0."
            "5019608,"
            "\"output\":0.39215687},{\"input\":1.0,\"output\":1.0}]}}");
        pc_io_raw_blocks w = write_adj(&ink);
        pc_adjustment back = parse_adj("curv", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_CMYK);
        CHECK(adj_eq_json(&ink, &back));
        pc_adjustment_fini(&back);
        pc_adjustment_fini(&ink);
        pc_io_raw_blocks_fini(&w);
    }
    {
        /* Selective colour with values, photo filter, colour lookup. */
        pc_adjustment s;
        pc_buf j = PC_BUF_INIT;
        memset(&s, 0, sizeof s);
        s.kind = PC_ADJ_SELECTIVE_COLOR;
        s.u.selective_color.relative = true;
        for (size_t r = 0; r < 9; r++) {
            s.u.selective_color.adjustments[r][0] = (float)r * 10.0f - 40.0f;
            s.u.selective_color.adjustments[r][1] = 5.0f;
            s.u.selective_color.adjustments[r][2] = -100.0f;
            s.u.selective_color.adjustments[r][3] = 100.0f;
        }
        json_of_adj(&s, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
        memset(&s, 0, sizeof s);
        s.kind = PC_ADJ_PHOTO_FILTER;
        s.u.photo_filter.color[0] = 60000.0f / 65535.0f;
        s.u.photo_filter.color[1] = 30000.0f / 65535.0f;
        s.u.photo_filter.density = 0.25f;
        s.u.photo_filter.preserve_luminosity = true;
        json_of_adj(&s, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
        s.u.photo_filter.color[0] = 0.0f;
        s.u.photo_filter.color[1] = 40000.0f / 65535.0f;
        s.u.photo_filter.color[2] = 1.0f;
        s.u.photo_filter.density = 1.0f;
        s.u.photo_filter.preserve_luminosity = false;
        json_of_adj(&s, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
    }
    {
        pc_lutfile id;
        pc_adjustment cl;
        pc_buf j = PC_BUF_INIT;
        must(pc_lutfile_identity(5, &id, NULL));
        memset(&cl, 0, sizeof cl);
        cl.kind = PC_ADJ_COLOR_LOOKUP;
        must(pc_str_set_c(&cl.u.color_lookup.name, "Look.cube", NULL));
        must(pc_blob_new(id.data, id.ndata * sizeof(float), &cl.u.color_lookup.lut, NULL));
        cl.u.color_lookup.size = 5;
        cl.u.color_lookup.dither = true;
        json_of_adj(&cl, &j);
        rt((const char *)j.data);
        pc_buf_fini(&j);
        pc_adjustment_fini(&cl);
        pc_lutfile_fini(&id);
    }
}

static void test_selective_color_synthetic_block(void)
{
    pc_psd_bytes d = {NULL, 0};
    pc_adjustment a;
    pc_io_raw_blocks w;
    cat(&d, (const uint8_t[4]){0, 1, 0, 1}, 4);
    cat(&d, (const uint8_t[8]){0}, 8);
    for (int r = 0; r < 9; r++) {
        int16_t rec[4] = {0, 0, 0, 0};
        if (r == 0) {
            rec[0] = 10;
            rec[1] = -20;
            rec[2] = 30;
            rec[3] = -40;
        } else if (r == 8) {
            rec[3] = 25;
        }
        i16s(&d, rec, 4);
    }
    a = parse_adj("selc", d.data, d.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_SELECTIVE_COLOR && !a.u.selective_color.relative);
    CHECK(a.u.selective_color.adjustments[0][0] == 10.0f && a.u.selective_color.adjustments[0][1] == -20.0f &&
          a.u.selective_color.adjustments[0][2] == 30.0f && a.u.selective_color.adjustments[0][3] == -40.0f);
    CHECK(a.u.selective_color.adjustments[8][3] == 25.0f);
    /* Written back byte-exact. */
    w = write_adj(&a);
    CHECK(w.v[0].len == d.len && memcmp(w.v[0].data, d.data, d.len) == 0);
    pc_io_raw_blocks_fini(&w);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&d);
}

static void lookup_block(pc_psd_desc *d, pc_psd_bytes *out)
{
    pc_psd_bytes body = vbytes(d);
    memset(out, 0, sizeof *out);
    cat(out, (const uint8_t[2]){0, 1}, 2);
    cat(out, body.data, body.len);
    pc_psd_bytes_fini(&body);
}

static void test_color_lookup_synthetic_block(void)
{
    static const char cube[] =
        "TITLE \"t\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
    pc_psd_desc d = desc("null");
    pc_psd_value raw;
    pc_psd_bytes data;
    pc_adjustment a;
    memset(&raw, 0, sizeof raw);
    raw.kind = PC_PSD_V_RAW_DATA;
    must(pc_psd_bytes_set(&raw.raw, cube, strlen(cube), NULL));
    tg_add(&d, "lookupType", en("colorLookupType", "3DLUT"));
    tg_add(&d, "NM  ", tg_text("Id", true));
    tg_add(&d, "Dthr", pc_psd_value_bool(true));
    tg_add(&d, "LUTFormat", en("LUTFormatType", "LUTFormatCUBE"));
    tg_add(&d, "LUT3DFileData", raw);
    lookup_block(&d, &data);
    a = parse_adj("clrL", data.data, data.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_COLOR_LOOKUP && a.u.color_lookup.size == 2 && a.u.color_lookup.dither);
    if (a.kind == PC_ADJ_COLOR_LOOKUP) {
        size_t n = 0;
        const float *t = pc_adjustment_lut(&a, &n);
        CHECK_EQ_STR(pc_str_c(&a.u.color_lookup.name), "Id");
        CHECK(t != NULL && n >= 6 && t[0] == 0.0f && t[3] == 1.0f && t[4] == 0.0f);
    }
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&data);
    /* A profile-based lookup (no table) is preserved raw. */
    d = desc("null");
    tg_add(&d, "lookupType", en("colorLookupType", "abstractProfile"));
    lookup_block(&d, &data);
    a = parse_adj("clrL", data.data, data.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_UNSUPPORTED);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&data);
    a = parse_adj("clrL", (const uint8_t[3]){0, 1, 9}, 3, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_UNSUPPORTED);
    pc_adjustment_fini(&a);
    a = parse_adj("selc", (const uint8_t[4]){0, 1, 0, 0}, 4, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_UNSUPPORTED);
    pc_adjustment_fini(&a);
}

static bool unsupported(const char *key, const uint8_t *d, size_t n, pc_io_adj_channels ch)
{
    pc_adjustment a = parse_adj(key, d, n, NULL, 0, ch);
    bool u = a.kind == PC_ADJ_UNSUPPORTED;
    pc_adjustment_fini(&a);
    return u;
}

static void test_malformed_falls_back_to_unsupported(void)
{
    CHECK(unsupported("levl", (const uint8_t[3]){0, 2, 1}, 3, PC_IO_ADJ_RGB));
    CHECK(unsupported("curv", (const uint8_t[3]){0, 0, 9}, 3, PC_IO_ADJ_RGB));
    CHECK(unsupported("thrs", NULL, 0, PC_IO_ADJ_RGB));
    CHECK(unsupported("blnc", (const uint8_t[18]){0}, 18, PC_IO_ADJ_RGB));
    CHECK(unsupported("vibA", (const uint8_t[5]){0, 0, 0, 15, 0}, 5, PC_IO_ADJ_RGB));
    CHECK(unsupported("blwh", NULL, 0, PC_IO_ADJ_RGB));
    CHECK(unsupported("mixr", (const uint8_t[4]){0, 2, 0, 0}, 4, PC_IO_ADJ_RGB));
    {
        /* A CMYK mixer (black source used) stays raw in CMYK documents; our own layout maps. */
        pc_psd_bytes cm = {NULL, 0};
        int16_t head[2] = {1, 0};
        pc_adjustment ours;
        pc_io_raw_blocks w;
        i16s(&cm, head, 2);
        for (int r = 0; r < 4; r++) {
            int16_t rec[5] = {0, 0, 0, 0, 0};
            rec[r] = 100;
            i16s(&cm, rec, 5);
        }
        CHECK(unsupported("mixr", cm.data, cm.len, PC_IO_ADJ_CMYK));
        CHECK(unsupported("mixr", cm.data, cm.len, PC_IO_ADJ_OTHER));
        ours =
            adj_json("{\"ChannelMixer\":{\"matrix\":[[1.0,0.0,0.0,0.0],[0.0,1.0,0.0,0.0],[0.0,0.0,1.0,0.0]],"
                     "\"monochrome\":false}}");
        w = write_adj(&ours);
        CHECK(!unsupported("mixr", w.v[0].data, w.v[0].len, PC_IO_ADJ_CMYK));
        CHECK(!unsupported("mixr", w.v[0].data, w.v[0].len, PC_IO_ADJ_OTHER));
        pc_io_raw_blocks_fini(&w);
        pc_adjustment_fini(&ours);
        pc_psd_bytes_fini(&cm);
    }
    CHECK(unsupported("mixr", (const uint8_t[6]){0, 1, 0, 0, 0, 100}, 6, PC_IO_ADJ_RGB));
    /* Photo Filter: unknown version, HSB colour space, out-of-range version-3 Lab, truncated. */
    CHECK(unsupported("phfl", (const uint8_t[4]){0, 4, 0, 0}, 4, PC_IO_ADJ_RGB));
    CHECK(unsupported("phfl", (const uint8_t[17]){0, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 25, 1}, 17,
                      PC_IO_ADJ_RGB));
    {
        pc_psd_bytes lab = {NULL, 0};
        cat(&lab, (const uint8_t[2]){0, 3}, 2);
        be32(&lab, 20000);
        be32(&lab, 0);
        be32(&lab, 0);
        cat(&lab, (const uint8_t[5]){0, 0, 0, 25, 1}, 5);
        CHECK(unsupported("phfl", lab.data, lab.len, PC_IO_ADJ_RGB));
        pc_psd_bytes_fini(&lab);
    }
    CHECK(unsupported("phfl", (const uint8_t[6]){0, 2, 0, 0, 0, 0}, 6, PC_IO_ADJ_RGB));
    /* Arbitrary bytes never crash, whatever the key. */
    for (size_t k = 0; k < 16; k++) {
        for (size_t len = 0; len < 64; len++) {
            uint8_t d[64];
            pc_adjustment a;
            for (size_t i = 0; i < len; i++) {
                d[i] = (uint8_t)(i * 37 + len);
            }
            a = parse_adj(PC_IO_ADJUSTMENT_KEYS[k], d, len, d, len, PC_IO_ADJ_RGB);
            pc_adjustment_fini(&a);
            a = parse_adj(PC_IO_ADJUSTMENT_KEYS[k], d, len, NULL, 0, PC_IO_ADJ_OTHER);
            pc_adjustment_fini(&a);
        }
    }
}

static void test_color_balance_golden_block(void)
{
    static const int16_t v[9] = {-30, 20, 0, 10, -5, 100, 0, 0, -100};
    pc_psd_bytes d = {NULL, 0};
    pc_adjustment a;
    pc_adjustment b;
    pc_io_raw_blocks w;
    i16s(&d, v, 9);
    cat(&d, (const uint8_t[2]){1, 0}, 2);
    a = parse_adj("blnc", d.data, d.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_COLOR_BALANCE && a.u.color_balance.preserve_luminosity);
    CHECK(a.u.color_balance.shadows[0] == -30.0f && a.u.color_balance.midtones[2] == 100.0f &&
          a.u.color_balance.highlights[2] == -100.0f);
    w = write_adj(&a);
    CHECK(w.n == 1 && w.v[0].len == d.len && memcmp(w.v[0].data, d.data, d.len) == 0);
    /* The 19-byte form (no trailing pad) parses the same. */
    b = parse_adj("blnc", d.data, 19, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(pc_adjustment_eq(&a, &b));
    pc_adjustment_fini(&a);
    pc_adjustment_fini(&b);
    pc_io_raw_blocks_fini(&w);
    pc_psd_bytes_fini(&d);
}

static void test_photo_filter_golden_blocks(void)
{
    pc_psd_bytes v2 = {NULL, 0};
    pc_adjustment a;
    pc_adjustment b;
    pc_io_raw_blocks w;
    cat(&v2, (const uint8_t[12]){0, 2, 0, 0, 0xff, 0xff, 0x80, 0x00, 0, 0, 0, 0}, 12);
    be32(&v2, 40);
    cat(&v2, (const uint8_t[4]){1, 0, 0, 0}, 4);
    CHECK_EQ_UINT(v2.len, 20);
    a = parse_adj("phfl", v2.data, v2.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_PHOTO_FILTER && a.u.photo_filter.preserve_luminosity);
    CHECK(a.u.photo_filter.color[0] == 1.0f && a.u.photo_filter.color[1] == 32768.0f / 65535.0f &&
          a.u.photo_filter.color[2] == 0.0f && a.u.photo_filter.density == 0.4f);
    w = write_adj(&a);
    CHECK(w.v[0].len == 20 && memcmp(w.v[0].data, v2.data, 20) == 0);
    pc_io_raw_blocks_fini(&w);
    /* Older PhotoCraft files used one padding byte (18 bytes): still read. */
    b = parse_adj("phfl", v2.data, 18, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(pc_adjustment_eq(&a, &b));
    pc_adjustment_fini(&a);
    pc_adjustment_fini(&b);
    for (int pl = 0; pl < 2; pl++) {
        static const float dens[3] = {0.0f, 0.14f, 1.0f};
        for (size_t di = 0; di < 3; di++) {
            pc_adjustment x;
            memset(&x, 0, sizeof x);
            x.kind = PC_ADJ_PHOTO_FILTER;
            x.u.photo_filter.color[0] = 0.8f;
            x.u.photo_filter.color[1] = 0.4f;
            x.u.photo_filter.color[2] = 0.2f;
            x.u.photo_filter.density = dens[di];
            x.u.photo_filter.preserve_luminosity = pl != 0;
            w = write_adj(&x);
            CHECK(w.n == 1 && memcmp(w.v[0].key, "phfl", 4) == 0 && w.v[0].len == 20);
            CHECK(memcmp(w.v[0].data + 17, (const uint8_t[3]){0, 0, 0}, 3) == 0);
            b = parse_adj("phfl", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_RGB);
            CHECK(b.kind == PC_ADJ_PHOTO_FILTER);
            CHECK(fabsf(b.u.photo_filter.color[0] - 0.8f) <= 1.0f / 65535.0f &&
                  fabsf(b.u.photo_filter.color[1] - 0.4f) <= 1.0f / 65535.0f &&
                  fabsf(b.u.photo_filter.color[2] - 0.2f) <= 1.0f / 65535.0f);
            CHECK(fabsf(b.u.photo_filter.density - dens[di]) < 0.00001f);
            CHECK(b.u.photo_filter.preserve_luminosity == (pl != 0));
            pc_adjustment_fini(&b);
            pc_io_raw_blocks_fini(&w);
        }
    }
    /* Version 2, Lab colour structure: L 50, a 0, b 0 is mid gray. */
    {
        static const int16_t lab[6] = {2, 7, 5000, 0, 0, 0};
        pc_psd_bytes d = {NULL, 0};
        i16s(&d, lab, 6);
        be32(&d, 25);
        cat(&d, (const uint8_t[1]){0}, 1);
        a = parse_adj("phfl", d.data, d.len, NULL, 0, PC_IO_ADJ_RGB);
        CHECK(a.kind == PC_ADJ_PHOTO_FILTER && !a.u.photo_filter.preserve_luminosity);
        for (size_t k = 0; k < 3; k++) {
            CHECK(fabsf(a.u.photo_filter.color[k] - 0.466f) < 0.01f);
        }
        CHECK(a.u.photo_filter.density == 0.25f);
        pc_adjustment_fini(&a);
        pc_psd_bytes_fini(&d);
    }
    /* Version 3: Lab x100 as i32 (L 100 = white), density 100 %; and a negative a* (green). */
    {
        static const int32_t cases[2][3] = {{10000, 0, 0}, {5000, -6000, 0}};
        for (size_t c = 0; c < 2; c++) {
            pc_psd_bytes d = {NULL, 0};
            cat(&d, (const uint8_t[2]){0, 3}, 2);
            for (size_t k = 0; k < 3; k++) {
                be32(&d, (uint32_t)cases[c][k]);
            }
            be32(&d, c == 0 ? 100 : 30);
            cat(&d, (const uint8_t[1]){c == 0 ? 1 : 0}, 1);
            a = parse_adj("phfl", d.data, d.len, NULL, 0, PC_IO_ADJ_RGB);
            CHECK(a.kind == PC_ADJ_PHOTO_FILTER);
            if (c == 0) {
                CHECK(a.u.photo_filter.preserve_luminosity && a.u.photo_filter.density == 1.0f);
                for (size_t k = 0; k < 3; k++) {
                    CHECK(fabsf(a.u.photo_filter.color[k] - 1.0f) < 0.01f);
                }
            } else {
                CHECK(a.u.photo_filter.color[1] > a.u.photo_filter.color[0] &&
                      a.u.photo_filter.color[1] > a.u.photo_filter.color[2]);
            }
            pc_adjustment_fini(&a);
            pc_psd_bytes_fini(&d);
        }
    }
    pc_psd_bytes_fini(&v2);
}

static void test_channel_mixer_golden_blocks(void)
{
    static const int16_t head[2] = {1, 0};
    static const int16_t recs[15] = {100, 0, 0, 0, 20, 50, 50, 0, 0, 0, 0, -50, 150, 0, -10};
    static const float want[3][4] = {
        {1.0f, 0.0f, 0.0f, 0.2f}, {0.5f, 0.5f, 0.0f, 0.0f}, {0.0f, -0.5f, 1.5f, -0.1f}};
    pc_psd_bytes d = {NULL, 0};
    pc_psd_bytes tail = {NULL, 0};
    pc_adjustment a;
    pc_io_raw_blocks w;
    i16s(&d, head, 2);
    i16s(&d, recs, 15);
    a = parse_adj("mixr", d.data, d.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_CHANNEL_MIXER && !a.u.channel_mixer.monochrome);
    CHECK(memcmp(a.u.channel_mixer.matrix, want, sizeof want) == 0);
    /* Written with the trailing gray record (a copy of the first row). */
    w = write_adj(&a);
    i16s(&tail, recs, 5);
    CHECK(w.v[0].len == d.len + 10 && memcmp(w.v[0].data, d.data, d.len) == 0 &&
          memcmp(w.v[0].data + d.len, tail.data, 10) == 0);
    pc_io_raw_blocks_fini(&w);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&d);
    pc_psd_bytes_fini(&tail);
    /* Monochrome with only the gray record: 30/59/11. */
    {
        static const int16_t m[7] = {1, 1, 30, 59, 11, 0, 0};
        pc_psd_bytes md = {NULL, 0};
        i16s(&md, m, 7);
        a = parse_adj("mixr", md.data, md.len, NULL, 0, PC_IO_ADJ_RGB);
        CHECK(a.kind == PC_ADJ_CHANNEL_MIXER && a.u.channel_mixer.monochrome);
        CHECK(a.u.channel_mixer.matrix[0][0] == 0.3f && a.u.channel_mixer.matrix[0][1] == 0.59f &&
              a.u.channel_mixer.matrix[0][2] == 0.11f && a.u.channel_mixer.matrix[0][3] == 0.0f);
        pc_adjustment_fini(&a);
        pc_psd_bytes_fini(&md);
    }
}

static void test_vibrance_golden_block(void)
{
    pc_psd_desc d = desc("null");
    pc_psd_bytes b;
    pc_adjustment a;
    tg_add(&d, "vibrance", pc_psd_value_int(-40));
    tg_add(&d, "Strt", pc_psd_value_int(25));
    b = vbytes(&d);
    CHECK(memcmp(b.data, (const uint8_t[4]){0, 0, 0, 16}, 4) == 0);
    a = parse_adj("vibA", b.data, b.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_VIBRANCE && a.u.vibrance.vibrance == -40.0f && a.u.vibrance.saturation == 25.0f);
    pc_adjustment_fini(&a);
    /* Other descriptor versions are rejected (kept raw). */
    b.data[3] = 17;
    CHECK(unsupported("vibA", b.data, b.len, PC_IO_ADJ_RGB));
    pc_psd_bytes_fini(&b);
    /* Photoshop omits zero values. */
    d = desc("null");
    tg_add(&d, "Strt", pc_psd_value_int(10));
    b = vbytes(&d);
    a = parse_adj("vibA", b.data, b.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_VIBRANCE && a.u.vibrance.vibrance == 0.0f && a.u.vibrance.saturation == 10.0f);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&b);
}

static void test_black_white_golden_block(void)
{
    static const char *const keys[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
    pc_psd_desc tint = desc("RGBC");
    pc_psd_desc d = desc("null");
    pc_psd_desc t2;
    pc_psd_desc d2;
    pc_psd_bytes b;
    pc_adjustment a;
    tg_add(&tint, "Rd  ", dbl(255.0));
    tg_add(&tint, "Grn ", dbl(127.5));
    tg_add(&tint, "Bl  ", dbl(0.0));
    must(pc_psd_desc_clone(&tint, &t2, NULL));
    for (int i = 0; i < 6; i++) {
        tg_add(&d, keys[i], pc_psd_value_int((i + 1) * 10));
    }
    must(pc_psd_desc_clone(&d, &d2, NULL));
    tg_add(&d, "useTint", pc_psd_value_bool(true));
    tg_add(&d, "tintColor", tg_obj(&tint));
    tg_add(&d, "bwPresetKind", pc_psd_value_int(1));
    b = vbytes(&d);
    a = parse_adj("blwh", b.data, b.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_BLACK_WHITE && a.u.black_white.has_tint);
    for (int i = 0; i < 6; i++) {
        CHECK(a.u.black_white.weights[i] == (float)((i + 1) * 10));
    }
    CHECK(a.u.black_white.tint[0] == 1.0f && a.u.black_white.tint[1] == 0.5f &&
          a.u.black_white.tint[2] == 0.0f);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&b);
    /* useTint off: no tint, even with a stored colour. */
    tg_add(&d2, "useTint", pc_psd_value_bool(false));
    tg_add(&d2, "tintColor", tg_obj(&t2));
    b = vbytes(&d2);
    a = parse_adj("blwh", b.data, b.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_BLACK_WHITE && !a.u.black_white.has_tint);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&b);
    /* Missing weights take Photoshop's defaults. */
    d = desc("null");
    b = vbytes(&d);
    a = parse_adj("blwh", b.data, b.len, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_BLACK_WHITE && !a.u.black_white.has_tint && a.u.black_white.weights[0] == 40.0f &&
          a.u.black_white.weights[5] == 80.0f);
    pc_adjustment_fini(&a);
    pc_psd_bytes_fini(&b);
}

static void test_degenerate_adjustments_still_get_an_encoding(void)
{
    /* Gradient maps with fewer than two stops render as gray (none) or one colour. */
    for (int c = 0; c < 2; c++) {
        pc_adjustment g;
        pc_io_raw_blocks w;
        pc_adjustment back;
        memset(&g, 0, sizeof g);
        g.kind = PC_ADJ_GRADIENT_MAP;
        g.u.gradient_map.reverse = true;
        if (c == 1) {
            g.u.gradient_map.stops = xmalloc(sizeof *g.u.gradient_map.stops);
            g.u.gradient_map.stops[0] = (pc_gradient_map_stop){0.3f, {0.2f, 0.4f, 0.6f}};
            g.u.gradient_map.nstops = 1;
        }
        w = write_adj(&g);
        back = parse_adj("grdm", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_RGB);
        CHECK(back.kind == PC_ADJ_GRADIENT_MAP && back.u.gradient_map.reverse &&
              back.u.gradient_map.nstops == 2);
        if (back.kind == PC_ADJ_GRADIENT_MAP && back.u.gradient_map.nstops == 2) {
            for (size_t i = 0; i < 2; i++) {
                const pc_gradient_map_stop *s = &back.u.gradient_map.stops[i];
                CHECK(fabsf(s->pos - (float)i) < 1e-6f);
                for (size_t k = 0; k < 3; k++) {
                    float want = c == 0 ? (float)i : (float)(k + 1) * 0.2f;
                    CHECK(fabsf(s->rgb[k] - want) < 1e-4f);
                }
            }
        }
        pc_adjustment_fini(&back);
        pc_adjustment_fini(&g);
        pc_io_raw_blocks_fini(&w);
    }
    /* A colour lookup without a (usable) table becomes an identity cube, without dither. */
    for (int c = 0; c < 2; c++) {
        pc_adjustment cl;
        pc_io_raw_blocks w;
        pc_adjustment back;
        pc_lutfile id;
        memset(&cl, 0, sizeof cl);
        cl.kind = PC_ADJ_COLOR_LOOKUP;
        must(pc_str_set_c(&cl.u.color_lookup.name, "L", NULL));
        cl.u.color_lookup.size = 3;
        cl.u.color_lookup.dither = true;
        if (c == 1) {
            float v[7] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
            must(pc_blob_new(v, sizeof v, &cl.u.color_lookup.lut, NULL));
        }
        w = write_adj(&cl);
        back = parse_adj("clrL", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_RGB);
        must(pc_lutfile_identity(2, &id, NULL));
        CHECK(back.kind == PC_ADJ_COLOR_LOOKUP && back.u.color_lookup.size == 2 &&
              !back.u.color_lookup.dither);
        if (back.kind == PC_ADJ_COLOR_LOOKUP) {
            size_t n = 0;
            const float *t = pc_adjustment_lut(&back, &n);
            CHECK_EQ_STR(pc_str_c(&back.u.color_lookup.name), "L");
            CHECK(n == id.ndata && memcmp(t, id.data, n * sizeof *t) == 0);
        }
        pc_lutfile_fini(&id);
        pc_adjustment_fini(&back);
        pc_adjustment_fini(&cl);
        pc_io_raw_blocks_fini(&w);
    }
    /* Every kind except a malformed Unsupported key has an encoding. */
    {
        pc_adjustment u = adj_json("{\"Unsupported\":{\"psd_key\":\"abc\",\"raw\":[]}}");
        pc_io_raw_blocks w = write_adj(&u);
        CHECK_EQ_UINT(w.n, 0);
        pc_adjustment_fini(&u);
        pc_io_raw_blocks_fini(&w);
    }
}

static void test_non_rgb_ignores_channel_records(void)
{
    pc_adjustment a;
    pc_io_raw_blocks w;
    pc_adjustment back;
    pc_adjustment_identity_levels(&a);
    for (size_t i = 0; i < 3; i++) {
        a.u.levels.per_channel[i].in_black = 0.1f;
    }
    w = write_adj(&a);
    back = parse_adj("levl", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_OTHER);
    CHECK(back.kind == PC_ADJ_LEVELS);
    for (size_t i = 0; i < 3; i++) {
        pc_levels_channel d = pc_levels_channel_default();
        CHECK(memcmp(&back.u.levels.per_channel[i], &d, sizeof d) == 0);
    }
    pc_adjustment_fini(&back);
    pc_io_raw_blocks_fini(&w);
}

static void test_gray_uses_first_channel_record_for_all(void)
{
    pc_adjustment a;
    pc_io_raw_blocks w;
    pc_adjustment back;
    pc_adjustment_identity_levels(&a);
    a.u.levels.per_channel[0].in_black = 44.0f / 255.0f;
    w = write_adj(&a);
    back = parse_adj("levl", w.v[0].data, w.v[0].len, NULL, 0, PC_IO_ADJ_GRAY);
    CHECK(back.kind == PC_ADJ_LEVELS);
    for (size_t i = 0; i < 3; i++) {
        CHECK(memcmp(&back.u.levels.per_channel[i], &a.u.levels.per_channel[0], sizeof(pc_levels_channel)) ==
              0);
    }
    pc_adjustment_fini(&back);
    pc_io_raw_blocks_fini(&w);
}

static void test_legacy_brit_without_cged(void)
{
    pc_adjustment a =
        parse_adj("brit", (const uint8_t[7]){0, 10, 0xff, 0xf6, 0, 127, 0}, 7, NULL, 0, PC_IO_ADJ_RGB);
    CHECK(a.kind == PC_ADJ_BRIGHTNESS_CONTRAST && a.u.brightness_contrast.brightness == 10.0f &&
          a.u.brightness_contrast.contrast == -10.0f && a.u.brightness_contrast.legacy);
    pc_adjustment_fini(&a);
}

int main(int argc, char **argv)
{
    RUN(test_gradient_fill_opacity_stops_become_alpha);
    RUN(test_lab_descriptor_colours_round_trip);
    RUN(test_blending_ranges_map_to_blend_if);
    RUN(test_advanced_blending_blocks_round_trip);
    RUN(test_brst_round_trip);
    RUN(test_placed_layer_warp_is_read);
    RUN(test_locks_and_labels);
    RUN(test_fills_roundtrip);
    RUN(test_float_rgb_colors_parse);
    RUN(test_smart_transform_from_sold);
    RUN(test_effects_switch);
    RUN(test_tysh_garbage_is_none);
    RUN(test_gradient_map_v3_methods_and_smoothness);
    RUN(test_roundtrips);
    RUN(test_selective_color_synthetic_block);
    RUN(test_color_lookup_synthetic_block);
    RUN(test_malformed_falls_back_to_unsupported);
    RUN(test_color_balance_golden_block);
    RUN(test_photo_filter_golden_blocks);
    RUN(test_channel_mixer_golden_blocks);
    RUN(test_vibrance_golden_block);
    RUN(test_black_white_golden_block);
    RUN(test_degenerate_adjustments_still_get_an_encoding);
    RUN(test_non_rgb_ignores_channel_records);
    RUN(test_gray_uses_first_channel_record_for_all);
    RUN(test_legacy_brit_without_cged);
    return pc_t_finish();
}
