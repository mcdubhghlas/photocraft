/* Displace smart-filter descriptor mapping (port of crates/io/src/displace_map.rs `mod tests`,
 * R10c): a filterFX item carrying an embedded displacement map round-trips to the
 * `filter.distort.displace` command and back; edited scales/fit/undefined-areas write a patched
 * item; the two planes build the compositor's map; and malformed, truncated or mutated maps keep
 * the filter opaque (PC_IO_UNSUPPORTED_FILTER) without a crash. The expected values are those
 * displace_map.rs asserts on the Rust side, so this is parity with the reference. */
#include "harness.h"

#include "common.h"

#include "algo/image.h"
#include "base/json.h"
#include "io/displace_map.h"
#include "io/smart_map.h"
#include "psd/descriptor.h"

/* ---- synthetic descriptors (mirror the Rust test's `tdta` and `item`) ---- */

typedef uint8_t (*plane_fn)(size_t);
static uint8_t p_idx(size_t i) { return (uint8_t)i; }
static uint8_t p_idx20(size_t i) { return (uint8_t)(i * 20); }
static uint8_t p_255(size_t i) { (void)i; return 255; }
static uint8_t p_0(size_t i) { (void)i; return 0; }

/* A `tdta` blob: w x h, horizontal plane h_of(i), vertical v_of(i), then `pad` zero bytes. */
static pc_buf tdta(uint32_t w, uint32_t h, plane_fn h_of, plane_fn v_of, size_t pad)
{
    pc_buf b = PC_BUF_INIT;
    size_t n = (size_t)w * h;
    uint8_t head[10];
    size_t i;
    head[0] = (uint8_t)h;
    head[1] = (uint8_t)(h >> 8);
    head[2] = (uint8_t)(h >> 16);
    head[3] = (uint8_t)(h >> 24);
    head[4] = (uint8_t)w;
    head[5] = (uint8_t)(w >> 8);
    head[6] = (uint8_t)(w >> 16);
    head[7] = (uint8_t)(w >> 24);
    head[8] = 3; /* mode: two 8-bit planes */
    head[9] = 0;
    must(pc_buf_append(&b, head, sizeof head, NULL));
    for (i = 0; i < n; i++) {
        uint8_t v = h_of(i);
        must(pc_buf_append(&b, &v, 1, NULL));
    }
    for (i = 0; i < n; i++) {
        uint8_t v = v_of(i);
        must(pc_buf_append(&b, &v, 1, NULL));
    }
    for (i = 0; i < pad; i++) {
        uint8_t z = 0;
        must(pc_buf_append(&b, &z, 1, NULL));
    }
    return b;
}

static pc_psd_value raw_value(pc_psd_vkind kind, const uint8_t *data, size_t len)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = kind;
    v.raw.data = xmalloc(len > 0 ? len : 1);
    memcpy(v.raw.data, data, len);
    v.raw.len = len;
    return v;
}

static pc_status add(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_status st, pc_error *err)
{
    if (st != PC_OK) {
        pc_psd_value_fini(v);
        return st;
    }
    return pc_psd_desc_add(d, key, v, err);
}

/* blend_options of a neutral filter (Opct #Prc 100, Md BlnM "normal"). */
static pc_status neutral_blend(pc_psd_value *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "blendOptions", err);
    if (st != PC_OK) {
        return st;
    }
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, "#Prc", 4);
    v.d = 100.0;
    st = add(&d, "Opct", &v, st, err);
    if (st == PC_OK) {
        st = pc_psd_value_enum("BlnM", "normal", &v, err);
    }
    st = add(&d, "Md  ", &v, st, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return pc_psd_value_object(&d, out, err);
}

/* A filterFXList item as Photoshop writes a Displace smart filter. `map`/`map_len` NULL omits the
 * embedded map (DspD); `dsp_m` overrides the DspM enum value (NULL = "StrF"). */
static pc_psd_desc displace_item(const uint8_t *map, size_t map_len, const char *dsp_m)
{
    pc_psd_desc d;
    pc_psd_desc f;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "filterFX", NULL);
    must(st);
    st = pc_psd_value_text("Displace...", true, &v, NULL);
    st = add(&d, "Nm  ", &v, st, NULL);
    if (st == PC_OK) {
        st = neutral_blend(&v, NULL);
        st = add(&d, "blendOptions", &v, st, NULL);
    }
    v = pc_psd_value_bool(true);
    st = add(&d, "enab", &v, st, NULL);
    v = pc_psd_value_bool(true);
    st = add(&d, "hasoptions", &v, st, NULL);
    must(st);
    must(pc_psd_desc_init(&f, "Dspl", NULL));
    v = pc_psd_value_int(10);
    st = add(&f, "HrzS", &v, PC_OK, NULL);
    v = pc_psd_value_int(10);
    st = add(&f, "VrtS", &v, st, NULL);
    if (st == PC_OK) {
        st = pc_psd_value_enum("DspM", dsp_m != NULL ? dsp_m : "StrF", &v, NULL);
        st = add(&f, "DspM", &v, st, NULL);
    }
    if (st == PC_OK) {
        st = pc_psd_value_enum("UndA", "RptE", &v, NULL);
        st = add(&f, "UndA", &v, st, NULL);
    }
    if (st == PC_OK) {
        static const char alias[] = "Macintosh HD:maps:displace.psd";
        v = raw_value(PC_PSD_V_ALIAS, (const uint8_t *)alias, sizeof alias - 1);
        st = add(&f, "DspF", &v, st, NULL);
    }
    v = pc_psd_value_bool(true);
    st = add(&f, "EmbF", &v, st, NULL);
    if (st == PC_OK && map != NULL) {
        v = raw_value(PC_PSD_V_RAW_DATA, map, map_len);
        st = add(&f, "DspD", &v, st, NULL);
    }
    must(st);
    must(pc_psd_value_object(&f, &v, NULL));
    must(pc_psd_desc_add(&d, "Fltr", &v, NULL));
    v = pc_psd_value_int((int32_t)0x4473706c); /* code(b"Dspl") */
    must(pc_psd_desc_add(&d, "filterID", &v, NULL));
    return d;
}

static const char *cmd_of(const pc_smart_filter *f) { return pc_str_c(&f->command); }

/* A number field of the params, or NAN. */
static double param_num(const pc_smart_filter *f, const char *key)
{
    yyjson_mut_val *root = pc_json_value_root(&f->params);
    yyjson_mut_val *v = root != NULL ? yyjson_mut_obj_get(root, key) : NULL;
    return v != NULL && yyjson_mut_is_num(v) ? yyjson_mut_get_num(v) : (double)NAN;
}

static const char *param_str(const pc_smart_filter *f, const char *key)
{
    yyjson_mut_val *root = pc_json_value_root(&f->params);
    yyjson_mut_val *v = root != NULL ? yyjson_mut_obj_get(root, key) : NULL;
    return v != NULL && yyjson_mut_is_str(v) ? yyjson_mut_get_str(v) : "";
}

/* ---- tests ---- */

static void test_filter_id_is_dspl(void)
{
    CHECK_EQ_INT((int32_t)0x4473706c, 1148416108);
}

static void test_displace_items_map_and_round_trip(void)
{
    pc_buf map = tdta(4, 3, p_idx20, p_idx20, 4);
    pc_psd_desc src = displace_item(map.data, map.len, NULL);
    pc_smart_filter f;
    pc_psd_desc back_item;
    bool found = false;
    pc_algo_image img;
    must(pc_io_filter_from_item(&src, &f, NULL));
    CHECK_EQ_STR(cmd_of(&f), PC_IO_DISPLACE_COMMAND);
    CHECK_NEAR(param_num(&f, "horizontal"), 10.0, 1e-9);
    CHECK_EQ_STR(param_str(&f, "fit"), "stretch");
    CHECK_EQ_STR(param_str(&f, "undefinedAreas"), "repeat");
    /* Unchanged settings write the original item back byte for byte. */
    must(pc_io_item_for_filter(&f, &back_item, NULL));
    CHECK(pc_psd_desc_eq(&back_item, &src));
    pc_psd_desc_fini(&back_item);
    /* The embedded map: identical planes collapse to one channel. */
    must(pc_io_displace_embedded_map(&f.params, &found, &img, NULL));
    CHECK(found);
    CHECK_EQ_INT(img.rect.x0, 0);
    CHECK_EQ_INT(img.rect.y0, 0);
    CHECK_EQ_INT(img.rect.x1, 4);
    CHECK_EQ_INT(img.rect.y1, 3);
    CHECK_EQ_UINT(img.ch, 1);
    CHECK_NEAR(img.data[1], 20.0f / 255.0f, 1e-6f);
    pc_algo_image_fini(&img);
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&src);
    pc_buf_fini(&map);
}

/* Set an existing object key to a JSON string / number, mutating the value's doc in place. */
static void set_str(pc_json_value *p, const char *key, const char *s)
{
    yyjson_mut_val *root = pc_json_value_root(p);
    CHECK(root != NULL);
    CHECK(yyjson_mut_obj_put(root, yyjson_mut_strcpy(p->doc, key), yyjson_mut_strcpy(p->doc, s)));
}

static void test_edited_settings_write_a_patched_item(void)
{
    pc_buf map = tdta(4, 3, p_idx20, p_idx20, 4);
    pc_psd_desc src = displace_item(map.data, map.len, NULL);
    pc_smart_filter f;
    pc_smart_filter back;
    pc_psd_desc item;
    bool found = false;
    pc_algo_image emb;
    must(pc_io_filter_from_item(&src, &f, NULL));
    /* edited.params: horizontal -25.4, fit tile, undefinedAreas wrap; opacity 0.5. */
    {
        yyjson_mut_val *root = pc_json_value_root(&f.params);
        CHECK(yyjson_mut_obj_put(root, yyjson_mut_strcpy(f.params.doc, "horizontal"),
                                 yyjson_mut_real(f.params.doc, -25.4)));
    }
    set_str(&f.params, "fit", "tile");
    set_str(&f.params, "undefinedAreas", "wrap");
    f.opacity = 0.5f;
    must(pc_io_item_for_filter(&f, &item, NULL));
    must(pc_io_filter_from_item(&item, &back, NULL));
    CHECK_EQ_STR(cmd_of(&back), PC_IO_DISPLACE_COMMAND);
    CHECK_NEAR(param_num(&back, "horizontal"), -25.0, 1e-9); /* round(-25.4) = -25 */
    CHECK_EQ_STR(param_str(&back, "fit"), "tile");
    CHECK_EQ_STR(param_str(&back, "undefinedAreas"), "wrap");
    CHECK_NEAR(back.opacity, 0.5f, 1e-6f);
    must(pc_io_displace_embedded_map(&back.params, &found, &emb, NULL));
    CHECK(found); /* the map survives */
    pc_algo_image_fini(&emb);
    pc_smart_filter_fini(&back);
    pc_psd_desc_fini(&item);
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&src);
    pc_buf_fini(&map);
}

static void test_different_planes_drive_each_direction(void)
{
    pc_buf map = tdta(2, 1, p_255, p_0, 0);
    pc_psd_desc src = displace_item(map.data, map.len, NULL);
    pc_smart_filter f;
    pc_algo_image img;
    bool found = false;
    must(pc_io_filter_from_item(&src, &f, NULL));
    must(pc_io_displace_embedded_map(&f.params, &found, &img, NULL));
    CHECK(found);
    CHECK_EQ_UINT(img.ch, 3);
    CHECK_NEAR(img.data[0], 1.0f, 1e-6f);
    CHECK_NEAR(img.data[1], 0.0f, 1e-6f);
    CHECK_NEAR(img.data[2], 0.5f, 1e-6f);
    pc_algo_image_fini(&img);
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&src);
    pc_buf_fini(&map);
}

/* A filterFXList item with its map removed (only the DspF alias left). */
static pc_psd_desc item_without_map(void)
{
    return displace_item(NULL, 0, NULL);
}

static void test_unreadable_filters_stay_opaque(void)
{
    pc_buf m2 = tdta(2, 2, p_0, p_0, 0);
    /* wrong_mode: the 8-byte header of a 2x2 map, mode 1, then 8 zero bytes. */
    uint8_t wrong_mode[18];
    pc_psd_desc srcs[4];
    size_t i;
    memcpy(wrong_mode, m2.data, 8);
    wrong_mode[8] = 1;
    wrong_mode[9] = 0;
    memset(wrong_mode + 10, 0, 8);
    srcs[0] = item_without_map();
    srcs[1] = displace_item(m2.data, m2.len, "Huh?"); /* bad DspM enum */
    srcs[2] = displace_item(wrong_mode, sizeof wrong_mode, NULL);
    {
        static const uint8_t junk[3] = {1, 2, 3};
        srcs[3] = displace_item(junk, sizeof junk, NULL);
    }
    for (i = 0; i < 4; i++) {
        pc_smart_filter f;
        pc_psd_desc item;
        must(pc_io_filter_from_item(&srcs[i], &f, NULL));
        CHECK_EQ_STR(cmd_of(&f), PC_IO_UNSUPPORTED_FILTER);
        must(pc_io_item_for_filter(&f, &item, NULL));
        CHECK(pc_psd_desc_eq(&item, &srcs[i]));
        pc_psd_desc_fini(&item);
        pc_smart_filter_fini(&f);
        pc_psd_desc_fini(&srcs[i]);
    }
    pc_buf_fini(&m2);
}

/* Build a pc_json_value from JSON text (NULL text -> JSON null). */
static pc_json_value jv_parse(const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    yyjson_doc *doc = NULL;
    if (json == NULL) {
        return out;
    }
    must(pc_json_parse(json, strlen(json), &doc, NULL));
    must(pc_json_value_from_val(yyjson_doc_get_root(doc), &out, NULL));
    yyjson_doc_free(doc);
    return out;
}

static bool export_errs(const pc_json_value *p)
{
    pc_psd_desc d;
    pc_error e;
    pc_status st = pc_io_displace_export_item(p, &d, &e);
    if (st == PC_OK) {
        pc_psd_desc_fini(&d);
        return false;
    }
    CHECK(e.msg[0] != '\0');
    return true;
}

static void test_bad_exports_report_errors(void)
{
    pc_buf map = tdta(2, 2, p_0, p_0, 0);
    pc_psd_desc src = displace_item(map.data, map.len, NULL);
    pc_smart_filter ok;
    char *only_id_hex = NULL;
    size_t i;
    must(pc_io_filter_from_item(&src, &ok, NULL));
    /* The hex of a filterFX descriptor that carries filterID but no map. */
    {
        pc_psd_desc d;
        pc_psd_value v;
        pc_psd_bytes bytes = {NULL, 0};
        must(pc_psd_desc_init(&d, "filterFX", NULL));
        v = pc_psd_value_int((int32_t)0x4473706c);
        must(pc_psd_desc_add(&d, "filterID", &v, NULL));
        must(pc_psd_desc_to_bytes(&d, &bytes, NULL));
        only_id_hex = xmalloc(bytes.len * 2 + 1);
        for (i = 0; i < bytes.len; i++) {
            (void)snprintf(only_id_hex + i * 2, 3, "%02x", bytes.data[i]);
        }
        pc_psd_bytes_fini(&bytes);
        pc_psd_desc_fini(&d);
    }
    /* Standalone bad params. */
    {
        pc_json_value cases[6];
        char only_id_json[64];
        /* cases built below; sized to fit only_id_hex. */
        char *big = xmalloc(strlen("{\"__displacePsd\":\"\"}") + strlen(only_id_hex) + 1);
        (void)snprintf(big, strlen("{\"__displacePsd\":\"\"}") + strlen(only_id_hex) + 1,
                       "{\"__displacePsd\":\"%s\"}", only_id_hex);
        (void)only_id_json;
        cases[0] = jv_parse(NULL);                               /* JSON null */
        cases[1] = jv_parse("{\"horizontal\":10}");             /* no template */
        cases[2] = jv_parse("{\"__displacePsd\":\"zz\"}");      /* invalid hex */
        cases[3] = jv_parse("{\"__displacePsd\":12}");          /* not hex text */
        cases[4] = jv_parse(big);                                /* filterFX without a map */
        cases[5] = jv_parse("{\"mapPath\":\"/tmp/map.psd\"}");  /* extra key */
        for (i = 0; i < 6; i++) {
            CHECK(export_errs(&cases[i]));
            pc_json_value_fini(&cases[i]);
        }
        free(big);
    }
    /* Edited copies of the valid params with a bad field. */
    {
        pc_json_value bad_h;
        pc_json_value bad_fit;
        must(pc_json_value_clone(&ok.params, &bad_h, NULL));
        set_str(&bad_h, "horizontal", "big");
        CHECK(export_errs(&bad_h));
        pc_json_value_fini(&bad_h);
        must(pc_json_value_clone(&ok.params, &bad_fit, NULL));
        set_str(&bad_fit, "fit", "squash");
        CHECK(export_errs(&bad_fit));
        pc_json_value_fini(&bad_fit);
    }
    free(only_id_hex);
    pc_smart_filter_fini(&ok);
    pc_psd_desc_fini(&src);
    pc_buf_fini(&map);
}

/* ---- never-crash: truncation, mutation, deep/huge headers ---- */

static void test_hostile_maps_never_crash(void)
{
    pc_buf good = tdta(4, 4, p_idx, p_idx, 0);
    uint32_t state = 0x12345679u;
    size_t len;
    int iter;
    /* Every truncated map blob, imported through a full item (parse must reject, never crash). */
    for (len = 0; len <= good.len; len++) {
        pc_psd_desc item = displace_item(good.data, len, NULL);
        pc_smart_filter f;
        must(pc_io_filter_from_item(&item, &f, NULL));
        /* A truncated map keeps the filter opaque. */
        if (len < good.len) {
            CHECK_EQ_STR(cmd_of(&f), PC_IO_UNSUPPORTED_FILTER);
        }
        pc_smart_filter_fini(&f);
        pc_psd_desc_fini(&item);
    }
    /* Mutated copies of a good map. */
    for (iter = 0; iter < 512; iter++) {
        pc_buf b = PC_BUF_INIT;
        pc_psd_desc item;
        pc_smart_filter f;
        bool found = false;
        pc_algo_image img;
        size_t i;
        must(pc_buf_append(&b, good.data, good.len, NULL));
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        i = (size_t)state % b.len;
        b.data[i] = (uint8_t)(b.data[i] + ((state >> 24) | 1u));
        item = displace_item(b.data, b.len, NULL);
        must(pc_io_filter_from_item(&item, &f, NULL));
        /* embedded_map must never crash, whatever filter_from_item decided. */
        (void)pc_io_displace_embedded_map(&f.params, &found, &img, NULL);
        if (found) {
            pc_algo_image_fini(&img);
        }
        pc_smart_filter_fini(&f);
        pc_psd_desc_fini(&item);
        pc_buf_fini(&b);
    }
    /* Headers declaring an empty or vast map are rejected (no allocation). */
    {
        const struct {
            uint32_t h;
            uint32_t w;
            uint16_t mode;
            bool ok;
        } H[] = {
            {0xffffffffu, 0xffffffffu, 3, false}, {1u << 16, 1u << 16, 3, false}, {0, 4, 3, false},
            {2, 2, 2, false},                     {2, 2, 4, false},               {2, 2, 3, true},
        };
        size_t k;
        for (k = 0; k < sizeof H / sizeof H[0]; k++) {
            uint8_t hdr[74];
            pc_psd_desc item;
            pc_smart_filter f;
            memset(hdr, 0, sizeof hdr);
            hdr[0] = (uint8_t)H[k].h;
            hdr[1] = (uint8_t)(H[k].h >> 8);
            hdr[2] = (uint8_t)(H[k].h >> 16);
            hdr[3] = (uint8_t)(H[k].h >> 24);
            hdr[4] = (uint8_t)H[k].w;
            hdr[5] = (uint8_t)(H[k].w >> 8);
            hdr[6] = (uint8_t)(H[k].w >> 16);
            hdr[7] = (uint8_t)(H[k].w >> 24);
            hdr[8] = (uint8_t)H[k].mode;
            hdr[9] = (uint8_t)(H[k].mode >> 8);
            item = displace_item(hdr, sizeof hdr, NULL);
            must(pc_io_filter_from_item(&item, &f, NULL));
            CHECK_EQ_STR(cmd_of(&f), H[k].ok ? PC_IO_DISPLACE_COMMAND : PC_IO_UNSUPPORTED_FILTER);
            pc_smart_filter_fini(&f);
            pc_psd_desc_fini(&item);
        }
    }
    pc_buf_fini(&good);
}

int main(int argc, char **argv)
{
    RUN(test_filter_id_is_dspl);
    RUN(test_displace_items_map_and_round_trip);
    RUN(test_edited_settings_write_a_patched_item);
    RUN(test_different_planes_drive_each_direction);
    RUN(test_unreadable_filters_stay_opaque);
    RUN(test_bad_exports_report_errors);
    RUN(test_hostile_maps_never_crash);
    return pc_t_finish();
}
