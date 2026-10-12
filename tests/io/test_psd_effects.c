/* Unit tests of crates/io/src/effects_map.rs (7) and never-crash sweeps over the lfx2 / lrFX /
 * fill parsers (chunk I2). */
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

static void be32(pc_psd_bytes *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    cat(b, x, 4);
}

/* ---- effects_map.rs ---- */

static const char SAMPLE_FX[] =
    "[{\"DropShadow\":{\"common\":{\"enabled\":true,\"blend\":\"Multiply\",\"opacity\":0.75},\"color\":{"
    "\"mode\":"
    "\"Rgb\",\"c\":[0.0,0.0,0.0,0.0],\"alpha\":1.0},\"angle\":120.0,\"use_global_light\":true,\"distance\":9."
    "0,"
    "\"spread\":0.25,\"size\":5.0,\"contour\":{\"Custom\":{\"name\":\"Cone\",\"points\":[{\"input\":0.0,"
    "\"output\":"
    "0.0},{\"input\":0.5,\"output\":1.0},{\"input\":1.0,\"output\":0.0}]}},\"anti_alias\":false,\"noise\":0."
    "0,"
    "\"knocks_out\":true}},{\"OuterGlow\":{\"common\":{\"enabled\":true,\"blend\":\"Screen\",\"opacity\":0.5}"
    ","
    "\"paint\":{\"Color\":{\"mode\":\"Rgb\",\"c\":[1.0,1.0,0.0,0.0],\"alpha\":1.0}},\"technique\":"
    "\"Precise\","
    "\"spread\":0.1,\"size\":8.0,\"contour\":\"Linear\",\"anti_alias\":true,\"range\":0.5,\"jitter\":0.0,"
    "\"noise\":"
    "0.0,\"source\":\"Edge\"}},{\"Stroke\":{\"common\":{\"enabled\":true,\"blend\":\"Normal\",\"opacity\":1."
    "0},"
    "\"size\":3.0,\"position\":\"Inside\",\"paint\":{\"Color\":{\"mode\":\"Rgb\",\"c\":[0.0,1.0,0.0,0.0],"
    "\"alpha\":"
    "1.0}}}},{\"Stroke\":{\"common\":{\"enabled\":true,\"blend\":\"Multiply\",\"opacity\":0.5},\"size\":6.0,"
    "\"position\":\"Center\",\"paint\":{\"Color\":{\"mode\":\"Rgb\",\"c\":[1.0,0.0,0.0,0.0],\"alpha\":1.0}}}}"
    ","
    "{\"ColorOverlay\":{\"common\":{\"enabled\":true,\"blend\":\"Overlay\",\"opacity\":0.5},\"color\":{"
    "\"mode\":"
    "\"Rgb\",\"c\":[0.0,0.0,1.0,0.0],\"alpha\":1.0}}},{\"DropShadow\":{\"common\":{\"enabled\":true,"
    "\"blend\":"
    "\"Multiply\",\"opacity\":0.75},\"color\":{\"mode\":\"Rgb\",\"c\":[0.0,0.0,0.0,0.0],\"alpha\":1.0},"
    "\"angle\":"
    "120.0,\"use_global_light\":true,\"distance\":5.0,\"spread\":0.0,\"size\":5.0,\"contour\":\"Linear\","
    "\"anti_alias\":false,\"noise\":0.0,\"knocks_out\":true}}]";

typedef struct fxlist {
    pc_effect *v;
    size_t n;
} fxlist;

static fxlist fx_json(const char *json)
{
    yyjson_doc *d = NULL;
    yyjson_val *arr;
    fxlist l;
    must(pc_json_parse(json, strlen(json), &d, NULL));
    arr = yyjson_doc_get_root(d);
    l.n = yyjson_arr_size(arr);
    l.v = calloc(l.n + 1, sizeof *l.v);
    for (size_t i = 0; i < l.n; i++) {
        must(pc_effect_from_json(yyjson_arr_get(arr, i), &l.v[i], NULL));
    }
    yyjson_doc_free(d);
    return l;
}

static void fx_free(fxlist *l)
{
    for (size_t i = 0; i < l->n; i++) {
        pc_effect_fini(&l->v[i]);
    }
    free(l->v);
}

static fxlist parse_fx(const uint8_t *d, size_t n, bool *found, bool *master)
{
    fxlist l = {NULL, 0};
    must(pc_io_parse_lfx2(d, n, found, master, &l.v, &l.n, NULL));
    return l;
}

static pc_buf write_fx(bool master, const fxlist *l)
{
    pc_buf b = PC_BUF_INIT;
    must(pc_io_write_lfx2(master, l->v, l->n, &b, NULL));
    return b;
}

static void test_lfx2_roundtrip_all_kinds(void)
{
    fxlist fx = fx_json(SAMPLE_FX);
    pc_buf data = write_fx(false, &fx);
    bool found = false;
    bool master = true;
    fxlist back = parse_fx(data.data, data.len, &found, &master);
    pc_buf a = write_fx(true, &fx);
    pc_buf b = write_fx(true, &back);
    CHECK(found && !master);
    CHECK_EQ_UINT(back.n, fx.n);
    /* Colours go through 0..255 doubles: compare via re-serialization. */
    CHECK(a.len == b.len && memcmp(a.data, b.data, a.len) == 0);
    /* Kind order is canonical: shadows first ... strokes last. */
    CHECK(back.n > 0 && back.v[0].kind == PC_FX_DROP_SHADOW && back.v[back.n - 1].kind == PC_FX_STROKE);
    fx_free(&fx);
    fx_free(&back);
    pc_buf_fini(&data);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
}

static pc_psd_bytes lfx2_of(pc_psd_desc *d)
{
    pc_psd_bytes v = {NULL, 0};
    pc_psd_bytes body = vbytes(d);
    cat(&v, (const uint8_t[4]){0}, 4);
    cat(&v, body.data, body.len);
    pc_psd_bytes_fini(&body);
    return v;
}

static void test_lfx2_ignores_absent_effects_but_keeps_disabled_configured_effects(void)
{
    pc_psd_desc d = desc("null");
    pc_psd_desc ds = desc("DrSh");
    pc_psd_desc fr = desc("FrFX");
    pc_psd_desc so = desc("SoFi");
    pc_psd_bytes data;
    fxlist fx;
    bool found = false;
    bool master = false;
    bool stroke = false;
    bool overlay = false;
    tg_add(&ds, "present", pc_psd_value_bool(false));
    tg_add(&ds, "enab", pc_psd_value_bool(false));
    tg_add(&fr, "present", pc_psd_value_bool(true));
    tg_add(&fr, "enab", pc_psd_value_bool(false));
    tg_add(&so, "enab", pc_psd_value_bool(false));
    tg_add(&d, "DrSh", tg_obj(&ds));
    tg_add(&d, "FrFX", tg_obj(&fr));
    tg_add(&d, "SoFi", tg_obj(&so));
    data = lfx2_of(&d);
    fx = parse_fx(data.data, data.len, &found, &master);
    CHECK_EQ_UINT(fx.n, 2);
    for (size_t i = 0; i < fx.n; i++) {
        CHECK(!pc_effect_enabled(&fx.v[i]));
        stroke = stroke || strcmp(pc_effect_label(&fx.v[i]), "Stroke") == 0;
        overlay = overlay || strcmp(pc_effect_label(&fx.v[i]), "Color Overlay") == 0;
    }
    CHECK(stroke && overlay);
    fx_free(&fx);
    pc_psd_bytes_fini(&data);
}

static void test_multi_instances_use_multi_keys(void)
{
    fxlist fx = fx_json(SAMPLE_FX);
    pc_buf data = write_fx(true, &fx);
    pc_psd_desc d;
    size_t used = 0;
    must(pc_psd_vdesc_parse_prefix(data.data + 4, data.len - 4, &d, &used, NULL));
    CHECK(pc_psd_desc_get(&d, "dropShadowMulti") != NULL);
    CHECK(pc_psd_desc_get(&d, "frameFXMulti") != NULL);
    CHECK(pc_psd_desc_get(&d, "SoFi") != NULL);
    pc_psd_desc_fini(&d);
    fx_free(&fx);
    pc_buf_fini(&data);
}

static void test_blend_names_are_unique(void)
{
    /* Every mode's four-character / long id maps back to that mode only. */
    for (int m = 0; m < PC_BLEND_COUNT; m++) {
        pc_psd_value v;
        pc_blend_mode back;
        size_t n = 0;
        const uint8_t *id;
        must(pc_io_blend_long_value((pc_blend_mode)m, &v, NULL));
        id = pc_psd_id_bytes(&v.value, &n);
        CHECK(pc_io_blend_from_id(id, n, &back) && back == (pc_blend_mode)m);
        pc_psd_value_fini(&v);
    }
}

static void test_long_string_blend_ids_parse(void)
{
    static const char *const longs[17] = {"normal",    "dissolve",   "darken",     "multiply", "colorBurn",
                                          "lighten",   "screen",     "colorDodge", "overlay",  "softLight",
                                          "hardLight", "difference", "exclusion",  "hue",      "saturation",
                                          "color",     "luminosity"};
    static const pc_blend_mode modes[17] = {PC_BLEND_NORMAL,     PC_BLEND_DISSOLVE,    PC_BLEND_DARKEN,
                                            PC_BLEND_MULTIPLY,   PC_BLEND_COLOR_BURN,  PC_BLEND_LIGHTEN,
                                            PC_BLEND_SCREEN,     PC_BLEND_COLOR_DODGE, PC_BLEND_OVERLAY,
                                            PC_BLEND_SOFT_LIGHT, PC_BLEND_HARD_LIGHT,  PC_BLEND_DIFFERENCE,
                                            PC_BLEND_EXCLUSION,  PC_BLEND_HUE,         PC_BLEND_SATURATION,
                                            PC_BLEND_COLOR,      PC_BLEND_LUMINOSITY};
    pc_psd_desc d = desc("null");
    pc_psd_desc ds = desc("DrSh");
    pc_psd_desc eb = desc("ebbl");
    pc_psd_desc so = desc("SoFi");
    pc_psd_bytes data;
    fxlist fx;
    bool found = false;
    bool master = false;
    tg_add(&d, "masterFXSwitch", pc_psd_value_bool(true));
    tg_add(&ds, "enab", pc_psd_value_bool(true));
    tg_add(&ds, "Md  ", en("BlnM", "linearBurn"));
    tg_add(&eb, "hglM", en("BlnM", "colorDodge"));
    tg_add(&eb, "sdwM", en("BlnM", "colorBurn"));
    tg_add(&so, "Md  ", en("BlnM", "hue"));
    tg_add(&d, "DrSh", tg_obj(&ds));
    tg_add(&d, "ebbl", tg_obj(&eb));
    tg_add(&d, "SoFi", tg_obj(&so));
    data = lfx2_of(&d);
    fx = parse_fx(data.data, data.len, &found, &master);
    CHECK_EQ_UINT(fx.n, 3);
    if (fx.n == 3) {
        CHECK(fx.v[0].kind == PC_FX_DROP_SHADOW && fx.v[0].u.shadow.common.blend == PC_BLEND_LINEAR_BURN);
        CHECK(fx.v[1].kind == PC_FX_BEVEL_EMBOSS && fx.v[1].u.bevel.highlight.blend == PC_BLEND_COLOR_DODGE &&
              fx.v[1].u.bevel.shadow.blend == PC_BLEND_COLOR_BURN);
        CHECK(fx.v[2].kind == PC_FX_COLOR_OVERLAY && fx.v[2].u.color_overlay.common.blend == PC_BLEND_HUE);
    }
    for (size_t i = 0; i < 17; i++) {
        pc_blend_mode m;
        CHECK(pc_io_blend_from_id((const uint8_t *)longs[i], strlen(longs[i]), &m) && m == modes[i]);
    }
    fx_free(&fx);
    pc_psd_bytes_fini(&data);
}

static void test_garbage_is_none(void)
{
    bool found = true;
    bool master;
    fxlist l = parse_fx((const uint8_t[2]){0, 0}, 2, &found, &master);
    CHECK(!found);
    found = true;
    must(pc_io_parse_lrfx((const uint8_t[5]){0, 0, 0, 1, 'X'}, 5, &found, &master, &l.v, &l.n, NULL));
    CHECK(!found);
}

static void test_lrfx_drop_shadow(void)
{
    pc_psd_bytes d = {NULL, 0};
    pc_psd_bytes rec = {NULL, 0};
    fxlist fx = {NULL, 0};
    bool found = false;
    bool master = false;
    cat(&d, (const uint8_t[4]){0, 0, 0, 1}, 4);
    cat(&d, "8BIMdsdw", 8);
    be32(&rec, 2);
    be32(&rec, 7u << 16);
    be32(&rec, 0);
    be32(&rec, 90u << 16);
    be32(&rec, 4u << 16);
    cat(&rec, (const uint8_t[10]){0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0}, 10);
    cat(&rec, "8BIMmul ", 8);
    cat(&rec, (const uint8_t[3]){1, 1, 128}, 3);
    cat(&rec, (const uint8_t[10]){0}, 10);
    be32(&d, (uint32_t)rec.len);
    cat(&d, rec.data, rec.len);
    must(pc_io_parse_lrfx(d.data, d.len, &found, &master, &fx.v, &fx.n, NULL));
    CHECK(found && fx.n == 1 && fx.v[0].kind == PC_FX_DROP_SHADOW);
    if (fx.n == 1) {
        const pc_shadow *s = &fx.v[0].u.shadow;
        CHECK(s->size == 7.0f && s->distance == 4.0f && s->common.blend == PC_BLEND_MULTIPLY);
        CHECK(fabsf(s->common.opacity - 128.0f / 255.0f) < 1e-6f && s->color.c[0] == 1.0f);
    }
    fx_free(&fx);
    pc_psd_bytes_fini(&d);
    pc_psd_bytes_fini(&rec);
}

/* Never crash: every byte flip and truncation of the per-function parsers' inputs. */
static void test_parsers_never_crash(void)
{
    fxlist fx = fx_json(SAMPLE_FX);
    pc_buf lfx = write_fx(true, &fx);
    uint32_t s = 7;
    for (size_t len = 0; len <= lfx.len; len += 3) {
        bool found;
        bool master;
        fxlist back = {NULL, 0};
        pc_fill f;
        must(pc_io_parse_lfx2(lfx.data, len, &found, &master, &back.v, &back.n, NULL));
        if (found) {
            fx_free(&back);
        }
        must(pc_io_parse_lrfx(lfx.data, len, &found, &master, &back.v, &back.n, NULL));
        if (found) {
            fx_free(&back);
        }
        must(
            pc_io_parse_fill((const uint8_t *)"GdFl", lfx.data + 4, len > 4 ? len - 4 : 0, &found, &f, NULL));
        if (found) {
            pc_fill_fini(&f);
        }
    }
    for (int k = 0; k < 400; k++) {
        uint8_t *m = xmalloc(lfx.len);
        bool found;
        bool master;
        fxlist back = {NULL, 0};
        memcpy(m, lfx.data, lfx.len);
        s = s * 1103515245u + 12345u;
        m[(s >> 8) % lfx.len] = (uint8_t)(s >> 24);
        must(pc_io_parse_lfx2(m, lfx.len, &found, &master, &back.v, &back.n, NULL));
        if (found) {
            fx_free(&back);
        }
        free(m);
    }
    fx_free(&fx);
    pc_buf_fini(&lfx);
}

int main(int argc, char **argv)
{
    RUN(test_lfx2_roundtrip_all_kinds);
    RUN(test_lfx2_ignores_absent_effects_but_keeps_disabled_configured_effects);
    RUN(test_multi_instances_use_multi_keys);
    RUN(test_blend_names_are_unique);
    RUN(test_long_string_blend_ids_parse);
    RUN(test_garbage_is_none);
    RUN(test_lrfx_drop_shadow);
    RUN(test_parsers_never_crash);
    return pc_t_finish();
}
