/* The plug-in ABI end to end (port of crates/plugins/tests/abi.rs): the example Invert plug-in,
 * and hostile modules (infinite loops, memory bombs, out-of-bounds access, bad manifests...),
 * which must all fail with an error, quickly. Plus: concurrent applies from several threads,
 * cancellation of sibling bands, huge areas, the registry and folder loading. With
 * WITH_PLUGINS=0 only the "unavailable" contract is checked. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/json.h"
#include "common.h"
#include "harness.h"
#include "plugins/plugins.h"
#include "wasm_enc.h"

static pc_buf g_invert;

static double now_s(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static pc_buf ok_module(const uint8_t *body, size_t n, uint32_t nlocals)
{
    return wm_abi_bytes(WM_MANIFEST, 1, nlocals, body, n);
}

/* Loads and applies on a 32 x 24 RGBA8 surface; returns the status (err filled) and the time. */
static pc_status run_err(const uint8_t *bytes, size_t len, const pc_plugin_limits *lim, pc_error *err,
                         double *secs)
{
    double t = now_s();
    pc_plugin *p = NULL;
    pc_status st = pc_plugin_load(bytes, len, lim, &p, err);
    if (st == PC_OK) {
        pc_surface s, out;
        t_surface(&s, PC_PIXEL_FORMAT_RGBA8, 32, 24);
        st = pc_plugin_apply(p, &s, pc_rect_make(0, 0, 32, 24), NULL, NULL, &out, err);
        if (st == PC_OK) {
            pc_surface_fini(&out);
        }
        pc_surface_fini(&s);
        pc_plugin_release(p);
    }
    *secs = now_s() - t;
    return st;
}

static void test_example_invert_loads_and_reports_its_manifest(void)
{
    pc_plugin *p = NULL;
    const pc_plugin_manifest *m;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, NULL, &p, NULL), PC_OK);
    if (p == NULL) {
        return;
    }
    m = pc_plugin_get_manifest(p);
    CHECK_EQ_STR(pc_str_c(&m->id), "org.photocraft.example.invert");
    CHECK_EQ_STR(pc_str_c(&m->name), "Invert (WebAssembly)");
    CHECK(m->kind == PC_PLUGIN_KIND_FILTER);
    CHECK_EQ_UINT(m->n_params, 0);
    CHECK(pc_plugin_size(p) < 4096);
    CHECK(pc_plugin_source(p) == NULL);
    pc_plugin_release(p);
}

static void test_example_invert_inverts_colour_at_every_depth(void)
{
    static const int XY[4][2] = {{1, 1}, {0, 0}, {17, 3}, {39, 29}};
    pc_plugin *p = NULL;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, NULL, &p, NULL), PC_OK);
    for (int sample = 0; p != NULL && sample < PC_SAMPLE_TYPE_COUNT; sample++) {
        pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
        pc_surface s, out;
        fmt.sample = (pc_sample_type)sample;
        t_surface(&s, fmt, 40, 30);
        CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 40, 30), NULL, NULL, &out, NULL), PC_OK);
        for (int k = 0; k < 4; k++) {
            float a[4], b[4];
            pc_surface_read_pixel(&s, XY[k][0], XY[k][1], a);
            pc_surface_read_pixel(&out, XY[k][0], XY[k][1], b);
            if (a[3] <= 0.0f) {
                CHECK(memcmp(a, b, sizeof a) == 0); /* transparent pixels are left alone */
                continue;
            }
            for (int c = 0; c < 3; c++) {
                CHECK_NEAR(b[c], 1.0f - a[c], 1e-6);
            }
            CHECK(a[3] == b[3]); /* alpha is kept */
        }
        pc_surface_fini(&out);
        pc_surface_fini(&s);
    }
    pc_plugin_release(p);
}

static void test_bands_are_seamless_and_selection_blends(void)
{
    /* Tiny bands: many instances, same result as one band. */
    pc_plugin_limits small = pc_plugin_limits_default();
    pc_plugin *p1 = NULL, *p2 = NULL;
    pc_surface s, o1, o2, sel, out;
    pc_rect canvas = pc_rect_make(0, 0, 40, 30);
    float a[4], b[4], k[1];
    small.band_bytes = 40 * 4 * 4 * 3;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, NULL, &p1, NULL), PC_OK);
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, &small, &p2, NULL), PC_OK);
    if (p1 == NULL || p2 == NULL) {
        pc_plugin_release(p1);
        pc_plugin_release(p2);
        return;
    }
    t_surface(&s, PC_PIXEL_FORMAT_RGBA16, 40, 30);
    CHECK_EQ_INT(pc_plugin_apply(p1, &s, canvas, NULL, NULL, &o1, NULL), PC_OK);
    CHECK_EQ_INT(pc_plugin_apply(p2, &s, canvas, NULL, NULL, &o2, NULL), PC_OK);
    CHECK(pc_surface_eq(&o1, &o2));
    pc_surface_fini(&o1);
    pc_surface_fini(&o2);
    /* A half-strength selection over the left half. */
    t_selection(&sel);
    CHECK_EQ_INT(pc_plugin_apply(p1, &s, canvas, &sel, NULL, &out, NULL), PC_OK);
    pc_surface_read_pixel(&s, 30, 20, a);
    pc_surface_read_pixel(&out, 30, 20, b);
    CHECK(memcmp(a, b, sizeof a) == 0); /* outside the selection */
    pc_surface_read_pixel(&s, 4, 20, a);
    pc_surface_read_pixel(&out, 4, 20, b);
    CHECK_NEAR(b[0], 1.0f - a[0], 1e-4); /* fully selected */
    pc_surface_read_pixel(&s, 5, 5, a);
    pc_surface_read_pixel(&out, 5, 5, b);
    pc_surface_read_pixel(&sel, 5, 5, k);
    CHECK_NEAR(b[0], a[0] + (1.0f - a[0] - a[0]) * k[0], 2.0 / 65535.0); /* half selected */
    pc_surface_fini(&out);
    pc_surface_fini(&sel);
    pc_surface_fini(&s);
    pc_plugin_release(p1);
    pc_plugin_release(p2);
}

/* for (i = 0; i < len; i += 4) f32.store(buf + i, VALUE) */
static size_t fill_body(uint8_t *out, const uint8_t *value, size_t nv)
{
    static const uint8_t HEAD[] = {0x02, 0x40, 0x03, 0x40, 0x20, 0x08, 0x20, 0x01,
                                   0x4f, 0x0d, 0x01, 0x20, 0x00, 0x20, 0x08, 0x6a};
    static const uint8_t TAIL[] = {0x38, 0x02, 0x00, 0x20, 0x08, 0x41, 0x04, 0x6a, 0x21,
                                   0x08, 0x0c, 0x00, 0x0b, 0x0b, 0x41, 0x00, 0x0b};
    size_t n = 0;
    memcpy(out + n, HEAD, sizeof HEAD);
    n += sizeof HEAD;
    memcpy(out + n, value, nv);
    n += nv;
    memcpy(out + n, TAIL, sizeof TAIL);
    return n + sizeof TAIL;
}

static void test_params_and_image_context_reach_the_plugin(void)
{
    /* Writes params_len / 1000 into every sample, so the host must pass the JSON through. */
    static const uint8_t VAL[] = {0x20, 0x07, 0xb3, 0x43, 0x00, 0x00, 0x7a, 0x44, 0x95};
    static const char MAN[] =
        "{\"id\":\"test.params\",\"name\":\"P\",\"kind\":\"filter\",\"params\":{\"amount\":"
        "{\"type\":\"number\",\"min\":0,\"max\":10,\"default\":5}}}";
    uint8_t body[128];
    size_t n = fill_body(body, VAL, sizeof VAL);
    pc_buf b = wm_abi_bytes(MAN, 1, 1, body, n), nt = PC_BUF_INIT;
    pc_plugin *p = NULL;
    pc_surface s, out;
    yyjson_doc *d3 = NULL, *dx = NULL;
    pc_error e;
    float px[4];
    CHECK_EQ_INT(pc_plugin_load(b.data, b.len, NULL, &p, NULL), PC_OK);
    if (p == NULL) {
        pc_buf_fini(&b);
        return;
    }
    (void)pc_plugin_manifest_params_notation(pc_plugin_get_manifest(p), &nt, NULL);
    CHECK_EQ_STR(pc_buf_cstr(&nt), "{\"amount\":0..10=5}");
    t_surface(&s, PC_PIXEL_FORMAT_RGBA32F, 8, 8);
    (void)pc_json_parse("{\"amount\": 3}", 13, &d3, NULL);
    CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 8, 8), NULL, yyjson_doc_get_root(d3), &out, NULL),
                 PC_OK);
    pc_surface_read_pixel(&out, 1, 1, px);
    CHECK(px[0] * 1000.0f > 100.0f && px[0] * 1000.0f < 600.0f);
    pc_surface_fini(&out);
    /* Wrong-typed params are refused before the plug-in runs. */
    (void)pc_json_parse("{\"amount\": \"x\"}", 15, &dx, NULL);
    CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 8, 8), NULL, yyjson_doc_get_root(dx), &out, &e),
                 PC_ERR_INVALID);
    CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_PARAMS);
    yyjson_doc_free(d3);
    yyjson_doc_free(dx);
    pc_surface_fini(&s);
    pc_buf_fini(&nt);
    pc_buf_fini(&b);
    pc_plugin_release(p);
}

static void test_nan_and_out_of_range_output_is_sanitised(void)
{
    static const uint8_t NAN_[] = {0x43, 0x00, 0x00, 0xc0, 0x7f};
    uint8_t body[128];
    size_t n = fill_body(body, NAN_, sizeof NAN_);
    pc_buf b = ok_module(body, n, 1);
    pc_plugin *p = NULL;
    pc_surface s, out;
    CHECK_EQ_INT(pc_plugin_load(b.data, b.len, NULL, &p, NULL), PC_OK);
    if (p != NULL) {
        t_surface(&s, PC_PIXEL_FORMAT_RGBA32F, 8, 8);
        CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 8, 8), NULL, NULL, &out, NULL), PC_OK);
        CHECK(pc_surface_eq(&out, &s));
        pc_surface_fini(&out);
        pc_surface_fini(&s);
        pc_plugin_release(p);
    }
    pc_buf_fini(&b);
}

typedef struct hostile {
    const char *name;
    pc_buf bytes;
} hostile;

static void test_hostile_modules_fail_quickly(void)
{
    static const uint8_t SPIN[] = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b};
    static const uint8_t BOMB[] = {0x03, 0x40, 0x41, 0x80, 0x04, 0x40, 0x00, 0x41,
                                   0x7f, 0x47, 0x0d, 0x00, 0x0b, 0x00, 0x0b};
    static const uint8_t GROW[] = {0x03, 0x40, 0x41, 0x01, 0x40, 0x00, 0x1a,
                                   0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b};
    static const uint8_t OOB[] = {0x41, 0x7c, 0x41, 0x00, 0x36, 0x02, 0x00, 0x41, 0x00, 0x0b};
    static const uint8_t TRAP[] = {0x00, 0x0b};
    static const uint8_t DIV[] = {0x41, 0x01, 0x20, 0x04, 0x6e, 0x1a, 0x41, 0x01, 0x41, 0x00, 0x6e, 0x0b};
    static const uint8_t CODE7[] = {0x41, 0x07, 0x0b};
    static const uint8_t NOOP[] = {0x41, 0x00, 0x0b};
    static const uint8_t CALL4[] = {0x10, 0x04, 0x41, 0x00, 0x0b};
    static const uint8_t RECUR[] = {0x10, 0x04, 0x0b};
    static const uint8_t LOOP_END[] = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b};
    static const uint8_t I32x4[4] = {WI32, WI32, WI32, WI32}, I32_[1] = {WI32};
    hostile cases[14];
    wm *m = calloc(1, sizeof *m);
    size_t nc = 0;
    if (m == NULL) {
        CHECK(false);
        return;
    }
    cases[nc++] = (hostile){"infinite loop", ok_module(SPIN, sizeof SPIN, 0)};
    cases[nc++] = (hostile){"memory.grow bomb, then trap", ok_module(BOMB, sizeof BOMB, 0)};
    cases[nc++] = (hostile){"memory.grow forever", ok_module(GROW, sizeof GROW, 0)};
    cases[nc++] = (hostile){"out-of-bounds write", ok_module(OOB, sizeof OOB, 0)};
    cases[nc++] = (hostile){"trap", ok_module(TRAP, sizeof TRAP, 0)};
    cases[nc++] = (hostile){"division by zero", ok_module(DIV, sizeof DIV, 0)};
    wm_abi(m, WM_MANIFEST, 1, 0, CALL4, sizeof CALL4);
    (void)wm_func(m, NULL, 0, NULL, 0, NULL, 0, RECUR, sizeof RECUR);
    cases[nc] = (hostile){"deep recursion", PC_BUF_INIT};
    wm_encode(m, &cases[nc++].bytes);
    cases[nc++] = (hostile){"error code", ok_module(CODE7, sizeof CODE7, 0)};
    cases[nc++] = (hostile){"wrong ABI version", wm_abi_bytes(WM_MANIFEST, 2, 0, NOOP, sizeof NOOP)};
    cases[nc++] = (hostile){"bad manifest", wm_abi_bytes("not json", 1, 0, NOOP, sizeof NOOP)};
    cases[nc++] = (hostile){
        "manifest with a bad id",
        wm_abi_bytes("{\"id\":\"a b\",\"name\":\"x\",\"kind\":\"filter\"}", 1, 0, NOOP, sizeof NOOP)};
    cases[nc++] =
        (hostile){"manifest of the wrong kind",
                  wm_abi_bytes("{\"id\":\"a\",\"name\":\"x\",\"kind\":\"panel\"}", 1, 0, NOOP, sizeof NOOP)};
    wm_abi(m, WM_MANIFEST, 1, 0, NOOP, sizeof NOOP);
    m->start = wm_func(m, NULL, 0, NULL, 0, NULL, 0, LOOP_END, sizeof LOOP_END);
    m->has_start = 1;
    cases[nc] = (hostile){"start function loops", PC_BUF_INIT};
    wm_encode(m, &cases[nc++].bytes);
    wm_abi(m, WM_MANIFEST, 1, 0, NOOP, sizeof NOOP);
    {
        uint8_t desc[2] = {0, (uint8_t)wm_type(m, I32x4, 4, I32_, 1)};
        wm_import(m, "wasi_snapshot_preview1", "fd_write", desc, 2);
    }
    cases[nc] = (hostile){"WASI import", PC_BUF_INIT};
    wm_encode(m, &cases[nc++].bytes);
    free(m);
    for (size_t i = 0; i < nc; i++) {
        pc_error e;
        double t;
        pc_error_clear(&e);
        CHECK(run_err(cases[i].bytes.data, cases[i].bytes.len, NULL, &e, &t) != PC_OK);
        CHECK(t < 5.0);
        CHECK(pc_plugin_error_kind_of(&e) != PC_PLUGIN_E_NONE);
        fprintf(stderr, "  %s: %s (%.3f s)\n", cases[i].name, e.msg, t);
        pc_buf_fini(&cases[i].bytes);
    }
}

static void test_malformed_and_oversized_modules_are_refused(void)
{
    static const uint8_t GARBAGE[] = "\0asm\x01\0\0\0\xff\xff\xff";
    static const uint8_t NOTWASM[] = "MZ\x90\0";
    static const uint8_t I32_[1] = {WI32}, I64_[1] = {WI64},
                         I32x8[8] = {WI32, WI32, WI32, WI32, WI32, WI32, WI32, WI32};
    static const uint8_t ONE[] = {0x41, 0x01, 0x0b}, ZERO64[] = {0x42, 0x00, 0x0b},
                         I16[] = {0x41, 0x10, 0x0b}, M16[] = {0x41, 0x70, 0x0b}, ZERO[] = {0x41, 0x00, 0x0b};
    struct {
        const uint8_t *b;
        size_t n;
    } bad[4] = {{(const uint8_t *)"", 0},
                {GARBAGE, sizeof GARBAGE - 1},
                {NOTWASM, 4},
                {g_invert.data, g_invert.len / 2}};
    pc_plugin_limits tiny = pc_plugin_limits_default();
    pc_plugin *p = NULL;
    pc_error e;
    wm *m = calloc(1, sizeof *m);
    pc_buf b = PC_BUF_INIT;
    double t;
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_INT(pc_plugin_load(bad[i].b, bad[i].n, NULL, &p, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_MODULE);
    }
    tiny.max_module_bytes = 100;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, &tiny, &p, &e), PC_ERR_FORMAT);
    CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_MODULE);
    if (m == NULL) {
        CHECK(false);
        return;
    }
    /* Huge initial memory. */
    wm_memory(m, 60000);
    wm_export(m, "memory", 2, 0);
    wm_encode(m, &b);
    CHECK(pc_plugin_load(b.data, b.len, NULL, &p, NULL) != PC_OK);
    pc_buf_truncate(&b, 0);
    /* Missing exports. */
    memset(m, 0, sizeof *m);
    wm_memory(m, 1);
    wm_export(m, "memory", 2, 0);
    wm_export(m, "pc_abi_version", 0, wm_func(m, NULL, 0, I32_, 1, NULL, 0, ONE, sizeof ONE));
    wm_export(m, "pc_manifest", 0, wm_func(m, NULL, 0, I64_, 1, NULL, 0, ZERO64, sizeof ZERO64));
    wm_encode(m, &b);
    CHECK(pc_plugin_load(b.data, b.len, NULL, &p, NULL) != PC_OK);
    pc_buf_truncate(&b, 0);
    /* Manifest outside memory. */
    memset(m, 0, sizeof *m);
    wm_memory(m, 1);
    wm_export(m, "memory", 2, 0);
    {
        wm_bytes out;
        memset(&out, 0, sizeof out);
        wm_byte(&out, 0x42);
        wm_sleb(&out, 0x00000064ffffff00LL); /* 100 bytes at 0xffffff00 */
        wm_byte(&out, 0x0b);
        wm_export(m, "pc_abi_version", 0, wm_func(m, NULL, 0, I32_, 1, NULL, 0, ONE, sizeof ONE));
        wm_export(m, "pc_manifest", 0, wm_func(m, NULL, 0, I64_, 1, NULL, 0, out.b, out.n));
        wm_export(m, "pc_alloc", 0, wm_func(m, I32_, 1, I32_, 1, NULL, 0, I16, sizeof I16));
        wm_export(m, "pc_filter", 0, wm_func(m, I32x8, 8, I32_, 1, NULL, 0, ZERO, sizeof ZERO));
    }
    wm_encode(m, &b);
    CHECK_EQ_INT(pc_plugin_load(b.data, b.len, NULL, &p, &e), PC_ERR_FORMAT);
    CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_ABI);
    pc_buf_truncate(&b, 0);
    /* Bogus allocations. */
    wm_abi(m, WM_MANIFEST, 1, 0, ZERO, sizeof ZERO);
    {
        wm_bytes body;
        memset(&body, 0, sizeof body);
        wm_uleb(&body, 0);
        wm_put(&body, M16, sizeof M16);
        m->func_body[2] = body; /* pc_alloc returns -16 */
    }
    wm_encode(m, &b);
    CHECK(run_err(b.data, b.len, NULL, &e, &t) != PC_OK);
    CHECK_EQ_INT(pc_plugin_error_kind_of(&e), PC_PLUGIN_E_ABI);
    pc_buf_fini(&b);
    free(m);
}

static void test_budgets_stop_runaway_plugins(void)
{
    static const uint8_t SPIN[] = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b};
    pc_buf b = ok_module(SPIN, sizeof SPIN, 0);
    pc_plugin_limits l = pc_plugin_limits_default();
    pc_error e;
    double t;
    l.fuel_base = 1000000;
    l.fuel_per_sample = 10;
    CHECK_EQ_INT(run_err(b.data, b.len, &l, &e, &t), PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "plug-in exceeded its instruction budget");
    pc_buf_fini(&b);
}

/* Spins unless the band's first sample is above 0.5, then runs `then` (5 bytes max). */
static pc_buf split_module(const uint8_t *then, size_t nthen)
{
    static const uint8_t HEAD[] = {0x20, 0x00, 0x2a, 0x02, 0x00, 0x43, 0x00,
                                   0x00, 0x00, 0x3f, 0x5e, 0x04, 0x7f};
    static const uint8_t ELSE[] = {0x05, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b, 0x0b};
    uint8_t body[64];
    size_t n = 0;
    memcpy(body, HEAD, sizeof HEAD);
    n += sizeof HEAD;
    memcpy(body + n, then, nthen);
    n += nthen;
    memcpy(body + n, ELSE, sizeof ELSE);
    n += sizeof ELSE;
    return ok_module(body, n, 0);
}

static void test_parallel_bands_preserve_the_originating_error(void)
{
    static const uint8_t CODE7[] = {0x41, 0x07};
    static const uint8_t UNREACH[] = {0x00};
    static const float BLACK[4] = {0, 0, 0, 1}, RED[4] = {1, 0, 0, 1};
    struct {
        const uint8_t *then;
        size_t n;
        const char *expected;
    } cases[2] = {{CODE7, 2, "plug-in failed: pc_filter returned error code 7"},
                  {UNREACH, 1, "plug-in trapped: wasm `unreachable` instruction executed"}};
    pc_surface s, before;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    (void)pc_surface_fill_rect(&s, pc_rect_make(0, 0, 256, 256), BLACK, 4, NULL);
    (void)pc_surface_fill_rect(&s, pc_rect_make(0, 256, 256, 512), RED, 4, NULL);
    for (int c = 0; c < 2; c++) {
        /* The later band fails immediately while the earlier one is still running (or hasn't
         * started). Collecting in band order must not turn that failure into a cancellation. */
        pc_buf b = split_module(cases[c].then, cases[c].n);
        pc_plugin_limits l = pc_plugin_limits_default();
        pc_plugin *p = NULL;
        pc_surface out;
        pc_error e;
        double t = now_s();
        l.band_bytes = 1;
        l.fuel_base = 100000000;
        l.fuel_per_sample = 0;
        CHECK_EQ_INT(pc_plugin_load(b.data, b.len, &l, &p, NULL), PC_OK);
        CHECK_EQ_INT(pc_surface_clone(&s, &before, NULL), PC_OK);
        if (p != NULL) {
            CHECK(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 256, 512), NULL, NULL, &out, &e) != PC_OK);
            CHECK_EQ_STR(e.msg, cases[c].expected);
        }
        CHECK(pc_surface_eq(&s, &before)); /* a failed run must not change the input */
        CHECK(now_s() - t < 30.0);
        pc_surface_fini(&before);
        pc_plugin_release(p);
        pc_buf_fini(&b);
    }
    pc_surface_fini(&s);
}

static void test_parallel_bands_preserve_instruction_budget(void)
{
    static const uint8_t SPIN[] = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b};
    static const float PX[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    pc_buf b = ok_module(SPIN, sizeof SPIN, 0);
    pc_plugin_limits l = pc_plugin_limits_default();
    pc_plugin *p = NULL;
    pc_surface s;
    l.band_bytes = 1;
    l.fuel_base = 1000000;
    l.fuel_per_sample = 0;
    CHECK_EQ_INT(pc_plugin_load(b.data, b.len, &l, &p, NULL), PC_OK);
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    (void)pc_surface_fill_rect(&s, pc_rect_make(0, 0, 768, 512), PX, 4, NULL);
    for (int i = 0; p != NULL && i < 8; i++) {
        pc_surface out;
        pc_error e;
        CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 768, 512), NULL, NULL, &out, &e),
                     PC_ERR_LIMIT);
        CHECK_EQ_STR(e.msg, "plug-in exceeded its instruction budget");
    }
    pc_surface_fini(&s);
    pc_plugin_release(p);
    pc_buf_fini(&b);
}

static void test_wall_clock_budget_stops_runaway_plugins(void)
{
    static const uint8_t SPIN[] = {0x03, 0x40, 0x0c, 0x00, 0x0b, 0x41, 0x00, 0x0b};
    pc_buf b = ok_module(SPIN, sizeof SPIN, 0);
    pc_plugin_limits l = pc_plugin_limits_default();
    pc_error e;
    double t;
    /* Wall-clock budget, with an effectively unlimited instruction budget. */
    l.fuel_base = UINT64_MAX / 4;
    l.wall_time_ms = 300;
    CHECK_EQ_INT(run_err(b.data, b.len, &l, &e, &t), PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "plug-in exceeded its time budget");
    CHECK(t < 3.0);
    pc_buf_fini(&b);
}

static void test_memory_cap_limits_bands(void)
{
    /* Memory cap: a band that can't fit is refused up front. */
    pc_surface s, out;
    pc_plugin_limits l = pc_plugin_limits_default();
    pc_plugin *p = NULL;
    t_surface(&s, PC_PIXEL_FORMAT_RGBA8, 64, 8);
    l.max_memory_bytes = (size_t)4 << 20;
    l.band_bytes = (size_t)64 << 20;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, &l, &p, NULL), PC_OK);
    if (p != NULL) {
        CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 64, 8), NULL, NULL, &out, NULL),
                     PC_OK); /* small bands fit */
        pc_surface_fini(&out);
        pc_plugin_release(p);
        p = NULL;
    }
    l = pc_plugin_limits_default();
    l.max_memory_bytes = 1024;
    if (pc_plugin_load(g_invert.data, g_invert.len, &l, &p, NULL) == PC_OK) {
        CHECK(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 64, 8), NULL, NULL, &out, NULL) != PC_OK);
        pc_plugin_release(p);
    }
    pc_surface_fini(&s);
}

static void test_registry_install_list_remove(void)
{
    static const uint8_t NOOP[] = {0x41, 0x00, 0x0b};
    pc_buf b = wm_abi_bytes("{\"id\":\"test.registry\",\"name\":\"R\",\"kind\":\"filter\"}", 1, 0, NOOP,
                            sizeof NOOP);
    pc_plugin *p = NULL, *g;
    pc_plugin **list = NULL;
    size_t n = 0;
    bool found = false;
    uint64_t rev;
    CHECK_EQ_INT(pc_plugins_install_bytes(b.data, b.len, &p, NULL), PC_OK);
    if (p != NULL) {
        CHECK_EQ_STR(pc_plugin_id(p), "test.registry");
        pc_plugin_release(p);
    }
    rev = pc_plugins_revision();
    CHECK_EQ_INT(pc_plugins_list(&list, &n, NULL), PC_OK);
    for (size_t i = 0; i < n; i++) {
        found = found || strcmp(pc_plugin_id(list[i]), "test.registry") == 0;
    }
    pc_plugins_list_free(list, n);
    CHECK(found);
    g = pc_plugins_get("test.registry");
    CHECK(g != NULL);
    pc_plugin_release(g);
    CHECK(pc_plugins_remove("test.registry"));
    CHECK(!pc_plugins_remove("test.registry"));
    CHECK(pc_plugins_get("test.registry") == NULL);
    CHECK(pc_plugins_revision() > rev);
    pc_buf_fini(&b);
}

static void test_folder_loading_skips_bad_files(void)
{
    char dir[512], path[600];
    pc_plugins_folder_report r;
    pc_plugin *p = NULL;
    if (t_mkdtemp("abi", dir, sizeof dir) != 0) {
        CHECK(false);
        return;
    }
    (void)snprintf(path, sizeof path, "%s/a-invert.wasm", dir);
    CHECK_EQ_INT(t_write_file(path, g_invert.data, g_invert.len), 0);
    (void)snprintf(path, sizeof path, "%s/b-broken.wasm", dir);
    CHECK_EQ_INT(t_write_file(path, "\0asm junk", 9), 0);
    (void)snprintf(path, sizeof path, "%s/notes.txt", dir);
    CHECK_EQ_INT(t_write_file(path, "ignored", 7), 0);
    CHECK_EQ_INT(pc_plugins_load_folder(dir, &r, NULL), PC_OK);
    CHECK_EQ_UINT(r.n_loaded, 1);
    if (r.n_loaded == 1) {
        CHECK_EQ_STR(pc_str_c(&r.loaded[0]), "org.photocraft.example.invert");
    }
    CHECK_EQ_UINT(r.n_failed, 1);
    if (r.n_failed == 1) {
        size_t fl = strlen(r.failed[0].file);
        CHECK(fl >= 13 && strcmp(r.failed[0].file + fl - 13, "b-broken.wasm") == 0);
    }
    pc_plugins_folder_report_fini(&r);
    (void)snprintf(path, sizeof path, "%s/missing", dir);
    CHECK(pc_plugins_load_folder(path, &r, NULL) != PC_OK);
    CHECK(pc_plugins_load_file(dir, NULL, &p, NULL) != PC_OK); /* a directory is not a module */
    (void)pc_plugins_remove("org.photocraft.example.invert");
    t_rm_rf(dir);
}

/* ---- beyond abi.rs ---- */

typedef struct thread_job {
    const pc_plugin *p;
    const pc_surface *s;
    pc_surface out;
    pc_status st;
} thread_job;

static void *apply_thread(void *arg)
{
    thread_job *j = arg;
    j->st = pc_plugin_apply(j->p, j->s, pc_rect_make(0, 0, 300, 200), NULL, NULL, &j->out, NULL);
    return NULL;
}

static void test_concurrent_applies_agree(void)
{
    pc_plugin *p = NULL;
    pc_plugin_limits l = pc_plugin_limits_default();
    pc_surface s, ref;
    thread_job jobs[4];
    pthread_t th[4];
    l.band_bytes = 1;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, &l, &p, NULL), PC_OK);
    if (p == NULL) {
        return;
    }
    t_surface(&s, PC_PIXEL_FORMAT_RGBA16, 300, 200);
    CHECK_EQ_INT(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 300, 200), NULL, NULL, &ref, NULL), PC_OK);
    for (int i = 0; i < 4; i++) {
        memset(&jobs[i], 0, sizeof jobs[i]);
        jobs[i].p = p;
        jobs[i].s = &s;
        jobs[i].st = PC_ERR_INVALID;
        CHECK_EQ_INT(pthread_create(&th[i], NULL, apply_thread, &jobs[i]), 0);
    }
    for (int i = 0; i < 4; i++) {
        (void)pthread_join(th[i], NULL);
        CHECK_EQ_INT(jobs[i].st, PC_OK);
        if (jobs[i].st == PC_OK) {
            CHECK(pc_surface_eq(&jobs[i].out, &ref));
            pc_surface_fini(&jobs[i].out);
        }
    }
    pc_surface_fini(&ref);
    pc_surface_fini(&s);
    pc_plugin_release(p);
}

static void test_failure_cancels_long_sibling_bands(void)
{
    /* The first band spins on a huge budget; another fails at once and must stop it. */
    static const uint8_t CODE7[] = {0x41, 0x07};
    static const float BLACK[4] = {0, 0, 0, 1}, RED[4] = {1, 0, 0, 1};
    pc_buf b = split_module(CODE7, 2);
    pc_plugin_limits l = pc_plugin_limits_default();
    pc_plugin *p = NULL;
    pc_surface s, out;
    pc_error e;
    double t;
    l.band_bytes = 1;
    l.fuel_base = UINT64_MAX / 4;
    l.fuel_per_sample = 0;
    CHECK_EQ_INT(pc_plugin_load(b.data, b.len, &l, &p, NULL), PC_OK);
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    (void)pc_surface_fill_rect(&s, pc_rect_make(0, 0, 256, 256), BLACK, 4, NULL);
    (void)pc_surface_fill_rect(&s, pc_rect_make(256, 0, 512, 256), RED, 4, NULL);
    t = now_s();
    if (p != NULL) {
        CHECK(pc_plugin_apply(p, &s, pc_rect_make(0, 0, 512, 256), NULL, NULL, &out, &e) != PC_OK);
        CHECK_EQ_STR(e.msg, "plug-in failed: pc_filter returned error code 7");
    }
    CHECK(now_s() - t < 10.0);
    pc_surface_fini(&s);
    pc_plugin_release(p);
    pc_buf_fini(&b);
}

static void test_huge_areas_and_canvases(void)
{
    static const uint8_t FILL[] = {0x43, 0x00, 0x00, 0x00, 0x3f};
    static const char MAN[] =
        "{\"id\":\"test.clouds\",\"name\":\"Clouds\",\"kind\":\"filter\",\"area\":\"canvas\"}";
    uint8_t body[128];
    size_t n = fill_body(body, FILL, sizeof FILL);
    pc_buf b = wm_abi_bytes(MAN, 1, 1, body, n);
    pc_plugin *p = NULL, *inv = NULL;
    pc_surface s, out;
    pc_error e;
    CHECK_EQ_INT(pc_plugin_load(b.data, b.len, NULL, &p, NULL), PC_OK);
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, NULL, &inv, NULL), PC_OK);
    t_surface(&s, PC_PIXEL_FORMAT_RGBA8, 8, 8);
    if (p != NULL) {
        /* A canvas spanning the whole coordinate space is refused, not attempted. */
        pc_status st = pc_plugin_apply(p, &s, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), NULL,
                                       NULL, &out, &e);
        CHECK_EQ_INT(st, PC_ERR_LIMIT);
    }
    if (inv != NULL) {
        /* Content-area filters only touch the content, whatever the canvas. */
        CHECK_EQ_INT(pc_plugin_apply(inv, &s, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), NULL,
                                     NULL, &out, NULL),
                     PC_OK);
        pc_surface_fini(&out);
        /* Content near the edge of the coordinate space. */
        pc_surface_fini(&s);
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        {
            static const float PX[4] = {0.5f, 0.5f, 0.5f, 1.0f};
            (void)pc_surface_fill_rect(&s, pc_rect_make(INT32_MAX - 10, INT32_MAX - 10, INT32_MAX, INT32_MAX),
                                       PX, 4, NULL);
        }
        CHECK_EQ_INT(pc_plugin_apply(inv, &s, pc_rect_make(0, 0, 10, 10), NULL, NULL, &out, NULL), PC_OK);
        pc_surface_fini(&out);
    }
    pc_surface_fini(&s);
    pc_plugin_release(p);
    pc_plugin_release(inv);
    pc_buf_fini(&b);
}

static void test_hostile_bytes_never_crash(void)
{
    /* Every truncation and many byte flips of a real plug-in: an error or a working plug-in. */
    uint8_t *t = malloc(g_invert.len);
    uint32_t s = 99;
    if (t == NULL) {
        return;
    }
    for (size_t cut = 0; cut < g_invert.len; cut += 7) {
        pc_plugin *p = NULL;
        if (pc_plugin_load(g_invert.data, cut, NULL, &p, NULL) == PC_OK) {
            pc_plugin_release(p);
        }
    }
    for (int round = 0; round < 400; round++) {
        pc_plugin *p = NULL;
        memcpy(t, g_invert.data, g_invert.len);
        for (int f = 0; f < 2; f++) {
            s = s * 1103515245u + 12345u;
            t[8 + (s >> 8) % (g_invert.len - 8)] = (uint8_t)(s >> 16);
        }
        if (pc_plugin_load(t, g_invert.len, NULL, &p, NULL) == PC_OK) {
            pc_surface sf, out;
            pc_plugin_limits l = *pc_plugin_get_limits(p);
            (void)l;
            t_surface(&sf, PC_PIXEL_FORMAT_RGBA8, 16, 16);
            if (pc_plugin_apply(p, &sf, pc_rect_make(0, 0, 16, 16), NULL, NULL, &out, NULL) == PC_OK) {
                pc_surface_fini(&out);
            }
            pc_surface_fini(&sf);
            pc_plugin_release(p);
        }
    }
    free(t);
}

static void test_unavailable_build(void)
{
    pc_plugin *p = NULL;
    pc_error e;
    CHECK_EQ_INT(pc_plugin_load(g_invert.data, g_invert.len, NULL, &p, &e), PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "plug-ins unavailable in this build");
    CHECK_EQ_INT(pc_plugins_install_bytes(g_invert.data, g_invert.len, NULL, &e), PC_ERR_UNSUPPORTED);
}

int main(int argc, char **argv)
{
    if (t_invert(&g_invert) != 0) {
        fprintf(stderr, "cannot read tests/fixtures/plugins/invert.wasm\n");
        return 1;
    }
    if (!pc_plugins_available()) {
        RUN(test_unavailable_build);
        fprintf(stderr, "  plug-ins unavailable in this build: ABI tests skipped\n");
        pc_buf_fini(&g_invert);
        return pc_t_finish();
    }
    RUN(test_example_invert_loads_and_reports_its_manifest);
    RUN(test_example_invert_inverts_colour_at_every_depth);
    RUN(test_bands_are_seamless_and_selection_blends);
    RUN(test_params_and_image_context_reach_the_plugin);
    RUN(test_nan_and_out_of_range_output_is_sanitised);
    RUN(test_hostile_modules_fail_quickly);
    RUN(test_malformed_and_oversized_modules_are_refused);
    RUN(test_budgets_stop_runaway_plugins);
    RUN(test_parallel_bands_preserve_the_originating_error);
    RUN(test_parallel_bands_preserve_instruction_budget);
    RUN(test_wall_clock_budget_stops_runaway_plugins);
    RUN(test_memory_cap_limits_bands);
    RUN(test_registry_install_list_remove);
    RUN(test_folder_loading_skips_bad_files);
    RUN(test_concurrent_applies_agree);
    RUN(test_failure_cancels_long_sibling_bands);
    RUN(test_huge_areas_and_canvases);
    RUN(test_hostile_bytes_never_crash);
    pc_buf_fini(&g_invert);
    return pc_t_finish();
}
