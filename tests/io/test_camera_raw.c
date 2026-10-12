/* Camera Raw Filter smart filters (port of the #[test]s of crates/io/src/camera_raw_map.rs; the
 * fixture test that needs a user-supplied PSD is not ported, see REWRITE_PLAN.md section 7):
 * verified develop controls round-trip through filterFX items, an edit patches only its key, and
 * anything unverified stays an opaque PC_IO_UNSUPPORTED_FILTER that writes back byte for byte. */
#include "harness.h"
#include "io/common.h"

#include "doc/json.h"
#include "algo/camera_raw.h"
#include "io/camera_raw_map.h"
#include "io/smart_map.h"
#include "psd/descriptor.h"

/* ---------------------------------------------------------------- descriptors */

static void d_set(pc_psd_desc *d, const char *key, pc_psd_value v)
{
    for (size_t i = 0; i < d->n; i++) {
        if (pc_psd_id_is(&d->items[i].key, key)) {
            pc_psd_value_fini(&d->items[i].value);
            d->items[i].value = v;
            return;
        }
    }
    must(pc_psd_desc_add(d, key, &v, NULL));
}

static pc_psd_value v_text(const char *s, bool nul)
{
    pc_psd_value v;
    must(pc_psd_value_text(s, nul, &v, NULL));
    return v;
}

static pc_psd_value v_enum(const char *t, const char *val)
{
    pc_psd_value v;
    must(pc_psd_value_enum(t, val, &v, NULL));
    return v;
}

static pc_psd_value v_obj(pc_psd_desc *d)
{
    pc_psd_value v;
    must(pc_psd_value_object(d, &v, NULL));
    return v;
}

static pc_psd_value v_ints(const int32_t *a, size_t n)
{
    pc_psd_value l;
    memset(&l, 0, sizeof l);
    l.kind = PC_PSD_V_LIST;
    for (size_t i = 0; i < n; i++) {
        pc_psd_value x = pc_psd_value_int(a[i]);
        must(pc_psd_value_list_push(&l, &x, NULL));
    }
    return l;
}

static pc_psd_value rgbc(double v)
{
    pc_psd_desc d;
    must(pc_psd_desc_init(&d, "RGBC", NULL));
    d_set(&d, "Rd  ", pc_psd_value_double(v));
    d_set(&d, "Grn ", pc_psd_value_double(v));
    d_set(&d, "Bl  ", pc_psd_value_double(v));
    return v_obj(&d);
}

static pc_psd_value blend_options(void)
{
    pc_psd_desc d;
    pc_psd_value o;
    memset(&o, 0, sizeof o);
    o.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(o.unit, "#Prc", 4);
    o.d = 100.0;
    must(pc_psd_desc_init(&d, "blendOptions", NULL));
    d_set(&d, "Opct", o);
    d_set(&d, "Md  ", v_enum("BlnM", "normal"));
    return v_obj(&d);
}

static const char *const HSL[3][8] = {
    {"HA_R", "HA_O", "HA_Y", "HA_G", "HA_A", "HA_B", "HA_P", "HA_M"},
    {"SA_R", "SA_O", "SA_Y", "SA_G", "SA_A", "SA_B", "SA_P", "SA_M"},
    {"LA_R", "LA_O", "LA_Y", "LA_G", "LA_A", "LA_B", "LA_P", "LA_M"},
};

static const struct {
    const char *key;
    int32_t value;
} NEUTRAL[] = {{"LNRC", 0},   {"CNRS", 50}, {"MDis", 0}, {"VigA", 0}, {"VigM", 50},
               {"crfs", 100}, {"TMMs", 0},  {"PGTM", 0}, {"RGBt", 0}};

/* SCALARS of the reference: param, key. */
static const char *const SCALARS[][2] = {
    {"temperature", "Temp"},
    {"tint", "Tint"},
    {"exposure", "Ex12"},
    {"contrast", "Cr12"},
    {"highlights", "Hi12"},
    {"shadows", "Sh12"},
    {"whites", "Wh12"},
    {"blacks", "Bk12"},
    {"texture", "CrTx"},
    {"clarity", "Cl12"},
    {"dehaze", "Dhze"},
    {"vibrance", "Vibr"},
    {"saturation", "Strt"},
    {"curveShadows", "PC_S"},
    {"curveDarks", "PC_D"},
    {"curveLights", "PC_L"},
    {"curveHighlights", "PC_H"},
    {"sharpenAmount", "Shrp"},
    {"sharpenRadius", "ShpR"},
    {"sharpenDetail", "ShpD"},
    {"sharpenMasking", "ShpM"},
    {"noiseLuminance", "LNR "},
    {"noiseLuminanceDetail", "LNRD"},
    {"noiseColor", "CNR "},
    {"noiseColorDetail", "CNRD"},
    {"gradeShadows.hue", "STSH"},
    {"gradeShadows.sat", "STSS"},
    {"gradeShadows.lum", "CgSL"},
    {"gradeMidtones.hue", "CgMH"},
    {"gradeMidtones.sat", "CgMS"},
    {"gradeMidtones.lum", "CgML"},
    {"gradeHighlights.hue", "STHH"},
    {"gradeHighlights.sat", "STHS"},
    {"gradeHighlights.lum", "CgHL"},
    {"gradeGlobal.hue", "CgGH"},
    {"gradeGlobal.sat", "CgGS"},
    {"gradeGlobal.lum", "CgGL"},
    {"gradeBlending", "CgBl"},
    {"gradeBalance", "STB "},
    {"grainAmount", "GRNA"},
    {"grainSize", "GRNS"},
    {"grainRoughness", "GRNF"},
    {"vignetteAmount", "PCVA"},
    {"vignetteMidpoint", "PCVM"},
    {"vignetteFeather", "PCVF"},
    {"vignetteRoundness", "PCVR"},
};
#define NSCALARS (sizeof SCALARS / sizeof SCALARS[0])

static const char *const CURVES[4][2] = {{"pointCurve", "Crv "},
                                         {"pointCurveRed", "CrvR"},
                                         {"pointCurveGreen", "CrvG"},
                                         {"pointCurveBlue", "CrvB"}};

static pc_psd_desc source_item(void)
{
    static const struct {
        const char *k;
        int32_t v;
    } INTS[] = {{"Cr12", -38}, {"Hi12", 35}, {"Sh12", -50}, {"Wh12", 49}, {"Bk12", -34}, {"PC_S", 0},
                {"PC_D", -21}, {"PC_L", 38}, {"PC_H", 0},   {"PC_1", 25}, {"PC_2", 50},  {"PC_3", 75}};
    pc_psd_desc d;
    pc_psd_desc item;
    must(pc_psd_desc_init(&d, PC_IO_CAMERA_RAW_CLASS, NULL));
    d_set(&d, "CrVe", v_text("18.4", true));
    d_set(&d, "PrVN", pc_psd_value_int(6));
    d_set(&d, "PrVe", pc_psd_value_int(251920384));
    d_set(&d, "Ex12", pc_psd_value_double(1.15));
    for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
        d_set(&d, INTS[i].k, pc_psd_value_int(INTS[i].v));
    }
    for (size_t a = 0; a < 3; a++) {
        for (size_t k = 0; k < 8; k++) {
            const char *key = HSL[a][k];
            int32_t v = strcmp(key, "HA_R") == 0   ? -50
                        : strcmp(key, "HA_Y") == 0 ? 29
                        : strcmp(key, "HA_G") == 0 ? 1
                                                   : 0;
            d_set(&d, key, pc_psd_value_int(v));
        }
    }
    must(pc_psd_desc_init(&item, "filterFX", NULL));
    d_set(&item, "Nm  ", v_text("Camera Raw Filter", true));
    d_set(&item, "blendOptions", blend_options());
    d_set(&item, "enab", pc_psd_value_bool(true));
    d_set(&item, "hasoptions", pc_psd_value_bool(true));
    d_set(&item, "FrgC", rgbc(0.0));
    d_set(&item, "BckC", rgbc(255.0));
    d_set(&item, "Fltr", v_obj(&d));
    d_set(&item, "filterID", pc_psd_value_int(2783));
    return item;
}

static pc_psd_desc fltr_of(const pc_psd_desc *item)
{
    pc_psd_desc d;
    const pc_psd_value *v = pc_psd_desc_get(item, "Fltr");
    must(pc_psd_desc_clone(v->desc, &d, NULL));
    return d;
}

static void set_fltr(pc_psd_desc *item, pc_psd_desc *d)
{
    d_set(item, "Fltr", v_obj(d));
}

static pc_psd_desc all_controls_item(void)
{
    static const struct {
        const char *k;
        int32_t v;
    } INTS[] = {{"Temp", -26}, {"Tint", -25}, {"CrTx", -31}, {"Cl12", 30},  {"Dhze", -34}, {"Vibr", 33},
                {"Strt", 33},  {"Shrp", 43},  {"ShpD", 36},  {"ShpM", 30},  {"LNR ", 47},  {"LNRD", 50},
                {"CNR ", 25},  {"CNRD", 50},  {"STSH", 117}, {"STSS", 52},  {"STHH", 316}, {"STHS", 70},
                {"STB ", 35},  {"CgMH", 51},  {"CgMS", 51},  {"CgSL", -24}, {"CgML", 41},  {"CgHL", 28},
                {"CgBl", 36},  {"CgGH", 20},  {"CgGS", 61},  {"CgGL", 45},  {"GRNA", 45},  {"GRNS", 25},
                {"GRNF", 50},  {"PCVA", 31},  {"PCVM", 50},  {"PCVF", 50},  {"PCVR", 0}};
    static const struct {
        const char *k;
        int32_t x, y;
    } CRV[] = {{"Crv ", 146, 102}, {"CrvR", 90, 174}, {"CrvG", 162, 122}, {"CrvB", 129, 202}};
    pc_psd_desc item = source_item();
    pc_psd_desc d = fltr_of(&item);
    d_set(&d, "WBal", v_enum("WBal", "Cst "));
    d_set(&d, "ShpR", pc_psd_value_double(1.2));
    for (size_t i = 0; i < sizeof INTS / sizeof INTS[0]; i++) {
        d_set(&d, INTS[i].k, pc_psd_value_int(INTS[i].v));
    }
    for (size_t i = 0; i < sizeof NEUTRAL / sizeof NEUTRAL[0]; i++) {
        d_set(&d, NEUTRAL[i].key, pc_psd_value_int(NEUTRAL[i].value));
    }
    for (size_t i = 0; i < 4; i++) {
        int32_t pts[6] = {0, 0, CRV[i].x, CRV[i].y, 255, 255};
        d_set(&d, CRV[i].k, v_ints(pts, 6));
    }
    set_fltr(&item, &d);
    return item;
}

/* ---------------------------------------------------------------- filters and params */

static pc_smart_filter from_item(const pc_psd_desc *item)
{
    pc_smart_filter f;
    must(pc_io_filter_from_item(item, &f, NULL));
    return f;
}

static pc_psd_desc for_filter(const pc_smart_filter *f)
{
    pc_psd_desc d;
    pc_error e;
    if (pc_io_item_for_filter(f, &d, &e) != PC_OK) {
        (void)fprintf(stderr, "item_for_filter: %s\n", e.msg);
        abort();
    }
    return d;
}

/* params[path] where path is "a", "a.b" or "a[i]"; NULL when missing. */
static yyjson_mut_val *param(const pc_smart_filter *f, const char *path)
{
    yyjson_mut_val *root = pc_json_value_root(&f->params);
    char key[64];
    const char *dot = strchr(path, '.');
    const char *br = strchr(path, '[');
    if (dot != NULL) {
        (void)snprintf(key, sizeof key, "%.*s", (int)(dot - path), path);
        return yyjson_mut_obj_get(yyjson_mut_obj_get(root, key), dot + 1);
    }
    if (br != NULL) {
        (void)snprintf(key, sizeof key, "%.*s", (int)(br - path), path);
        return yyjson_mut_arr_get(yyjson_mut_obj_get(root, key), (size_t)atoi(br + 1));
    }
    return yyjson_mut_obj_get(root, path);
}

static double num(const pc_smart_filter *f, const char *path)
{
    yyjson_mut_val *v = param(f, path);
    return v != NULL ? yyjson_mut_get_num(v) : NAN;
}

static void renormalise(pc_smart_filter *f)
{
    pc_json_value v;
    must(pc_json_value_from_mut(pc_json_value_root(&f->params), &v, NULL));
    pc_json_value_fini(&f->params);
    f->params = v;
}

/* params[path] = JSON text. */
static void set_param(pc_smart_filter *f, const char *path, const char *json)
{
    yyjson_mut_doc *d = f->params.doc;
    yyjson_mut_val *root = pc_json_value_root(&f->params);
    yyjson_doc *src;
    yyjson_mut_val *val;
    char key[64];
    const char *dot = strchr(path, '.');
    const char *br = strchr(path, '[');
    must(pc_json_parse(json, strlen(json), &src, NULL));
    val = yyjson_val_mut_copy(d, yyjson_doc_get_root(src));
    yyjson_doc_free(src);
    if (dot != NULL) {
        (void)snprintf(key, sizeof key, "%.*s", (int)(dot - path), path);
        (void)yyjson_mut_obj_put(yyjson_mut_obj_get(root, key), yyjson_mut_strcpy(d, dot + 1), val);
    } else if (br != NULL) {
        (void)snprintf(key, sizeof key, "%.*s", (int)(br - path), path);
        (void)yyjson_mut_arr_replace(yyjson_mut_obj_get(root, key), (size_t)atoi(br + 1), val);
    } else {
        (void)yyjson_mut_obj_put(root, yyjson_mut_strcpy(d, path), val);
    }
    renormalise(f);
}

static bool json_eq_text(yyjson_mut_val *v, const char *json)
{
    yyjson_doc *src;
    pc_json_value a;
    pc_json_value b;
    bool eq;
    must(pc_json_parse(json, strlen(json), &src, NULL));
    must(pc_json_value_from_val(yyjson_doc_get_root(src), &a, NULL));
    yyjson_doc_free(src);
    must(pc_json_value_from_mut(v, &b, NULL));
    eq = pc_json_value_eq(&a, &b);
    pc_json_value_fini(&a);
    pc_json_value_fini(&b);
    return eq;
}

static const pc_psd_value *fltr_get(const pc_psd_desc *item, const char *key)
{
    return pc_psd_desc_get(pc_psd_desc_get(item, "Fltr")->desc, key);
}

static bool fltr_eq(const pc_psd_desc *item, const pc_psd_desc *expected_fltr)
{
    return pc_psd_desc_eq(pc_psd_desc_get(item, "Fltr")->desc, expected_fltr);
}

/* ---------------------------------------------------------------- tests */

static void test_all_controls_roundtrip_and_patch_only_the_edited_field(void)
{
    pc_psd_desc source = all_controls_item();
    pc_smart_filter f = from_item(&source);
    pc_psd_desc back;
    pc_algo_camera_raw def;
    yyjson_mut_doc *dd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *defaults;
    pc_algo_camera_raw_default(&def);
    defaults = pc_algo_camera_raw_to_json(dd, &def);
    CHECK_EQ_STR(pc_str_c(&f.command), PC_IO_CAMERA_RAW_COMMAND);
    CHECK(num(&f, "temperature") == -26.0);
    CHECK(num(&f, "texture") == -31.0);
    CHECK(fabs(num(&f, "sharpenRadius") - 1.2) < 1e-6);
    CHECK(json_eq_text(param(&f, "gradeHighlights"), "{\"hue\": 316.0, \"sat\": 70.0, \"lum\": 28.0}"));
    CHECK(json_eq_text(param(&f, "pointCurveBlue"), "[[0.0, 0.0], [129.0, 202.0], [255.0, 255.0]]"));
    back = for_filter(&f);
    CHECK(pc_psd_desc_eq(&back, &source));
    pc_psd_desc_fini(&back);
    for (size_t i = 0; i < NSCALARS; i++) {
        pc_smart_filter edited;
        const char *p = SCALARS[i][0];
        const char *key = SCALARS[i][1];
        const char *dot = strchr(p, '.');
        double number;
        char text[64];
        pc_psd_desc expected = fltr_of(&source);
        pc_psd_desc item;
        if (dot != NULL) {
            char w[32];
            (void)snprintf(w, sizeof w, "%.*s", (int)(dot - p), p);
            number = yyjson_mut_get_num(yyjson_mut_obj_get(yyjson_mut_obj_get(defaults, w), dot + 1));
        } else {
            number = yyjson_mut_get_num(yyjson_mut_obj_get(defaults, p));
        }
        if (num(&f, p) == number) {
            number += 1.0;
        }
        must(pc_smart_filter_clone(&f, &edited, NULL));
        (void)snprintf(text, sizeof text, "%.17g", number);
        set_param(&edited, p, text);
        if (strcmp(key, "Ex12") == 0 || strcmp(key, "ShpR") == 0) {
            d_set(&expected, key, pc_psd_value_double(number));
        } else {
            d_set(&expected, key, pc_psd_value_int((int32_t)number));
        }
        item = for_filter(&edited);
        if (!fltr_eq(&item, &expected)) {
            PC_T_FAIL("%s", p);
        }
        pc_psd_desc_fini(&item);
        pc_psd_desc_fini(&expected);
        pc_smart_filter_fini(&edited);
    }
    for (size_t i = 0; i < 4; i++) {
        pc_smart_filter edited;
        pc_psd_desc item;
        pc_psd_desc expected = fltr_of(&source);
        pc_smart_filter again;
        int32_t lin[4] = {0, 0, 255, 255};
        must(pc_smart_filter_clone(&f, &edited, NULL));
        set_param(&edited, CURVES[i][0], "[]");
        item = for_filter(&edited);
        d_set(&expected, CURVES[i][1], v_ints(lin, 4));
        CHECK(fltr_eq(&item, &expected));
        again = from_item(&item);
        CHECK(json_eq_text(param(&again, CURVES[i][0]), "[]"));
        pc_smart_filter_fini(&again);
        pc_psd_desc_fini(&item);
        pc_psd_desc_fini(&expected);
        pc_smart_filter_fini(&edited);
    }
    yyjson_mut_doc_free(dd);
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&source);
}

static void check_opaque(pc_psd_desc *item, const char *what)
{
    pc_smart_filter f = from_item(item);
    pc_psd_desc back;
    if (strcmp(pc_str_c(&f.command), PC_IO_UNSUPPORTED_FILTER) != 0) {
        PC_T_FAIL("%s: %s", what, pc_str_c(&f.command));
    }
    back = for_filter(&f);
    if (!pc_psd_desc_eq(&back, item)) {
        PC_T_FAIL("%s: not written back as read", what);
    }
    pc_psd_desc_fini(&back);
    pc_smart_filter_fini(&f);
}

static void test_active_unimplemented_controls_and_malformed_curves_stay_opaque(void)
{
    for (size_t i = 0; i < sizeof NEUTRAL / sizeof NEUTRAL[0]; i++) {
        pc_psd_desc item = all_controls_item();
        pc_psd_desc d = fltr_of(&item);
        d_set(&d, NEUTRAL[i].key, pc_psd_value_int(NEUTRAL[i].value + 1));
        set_fltr(&item, &d);
        check_opaque(&item, NEUTRAL[i].key);
        pc_psd_desc_fini(&item);
    }
    for (int k = 0; k < 5; k++) {
        pc_psd_desc item = all_controls_item();
        pc_psd_desc d = fltr_of(&item);
        pc_psd_value v;
        static const int32_t A[] = {0};
        static const int32_t B[] = {0, 0, 0, 255};
        static const int32_t C[] = {0, 0, 255, 256};
        int32_t z34[34] = {0};
        switch (k) {
        case 0:
            v = v_ints(A, 1);
            break;
        case 1:
            v = v_ints(B, 4);
            break;
        case 2:
            v = v_ints(C, 4);
            break;
        case 3:
            memset(&v, 0, sizeof v);
            v.kind = PC_PSD_V_LIST;
            for (int j = 0; j < 4; j++) {
                pc_psd_value x = pc_psd_value_double(0.0);
                must(pc_psd_value_list_push(&v, &x, NULL));
            }
            break;
        default:
            v = v_ints(z34, 34);
            break;
        }
        d_set(&d, "Crv ", v);
        set_fltr(&item, &d);
        check_opaque(&item, "curve");
        pc_psd_desc_fini(&item);
    }
}

static void test_light_curve_and_hsl_editable_without_rewriting(void)
{
    pc_psd_desc source = source_item();
    pc_smart_filter f;
    pc_psd_desc back;
    pc_psd_desc edited;
    pc_psd_desc expected;
    pc_smart_filter again;
    pc_psd_value raw;
    memset(&raw, 0, sizeof raw);
    raw.kind = PC_PSD_V_RAW_DATA;
    raw.raw.data = xmalloc(4);
    memcpy(raw.raw.data, "\x01\x02\x03\x04", 4);
    raw.raw.len = 4;
    d_set(&source, "futureOuterData", raw);
    f = from_item(&source);
    CHECK_EQ_STR(pc_str_c(&f.command), PC_IO_CAMERA_RAW_COMMAND);
    CHECK(fabs(num(&f, "exposure") - 1.15) < 1e-6);
    CHECK(num(&f, "contrast") == -38.0);
    CHECK(num(&f, "curveDarks") == -21.0);
    CHECK(num(&f, "curveLights") == 38.0);
    CHECK(json_eq_text(param(&f, "curveSplits"), "[25.0, 50.0, 75.0]"));
    CHECK(json_eq_text(param(&f, "hslHue"), "[-50.0, 0.0, 29.0, 1.0, 0.0, 0.0, 0.0, 0.0]"));
    back = for_filter(&f);
    CHECK(pc_psd_desc_eq(&back, &source));
    pc_psd_desc_fini(&back);
    set_param(&f, "exposure", "0.0");
    set_param(&f, "hslHue[2]", "-20.0");
    f.visible = false;
    f.opacity = 0.4f;
    f.blend = PC_BLEND_MULTIPLY;
    edited = for_filter(&f);
    expected = fltr_of(&source);
    d_set(&expected, "Ex12", pc_psd_value_double(0.0));
    d_set(&expected, "HA_Y", pc_psd_value_int(-20));
    CHECK(fltr_eq(&edited, &expected));
    CHECK(pc_psd_value_eq(pc_psd_desc_get(&edited, "futureOuterData"),
                          pc_psd_desc_get(&source, "futureOuterData")));
    again = from_item(&edited);
    CHECK(!again.visible && again.blend == PC_BLEND_MULTIPLY && again.opacity == 0.4f);
    CHECK(num(&again, "exposure") == 0.0);
    CHECK(num(&again, "hslHue[2]") == -20.0);
    pc_smart_filter_fini(&again);
    pc_psd_desc_fini(&expected);
    pc_psd_desc_fini(&edited);
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&source);
}

static void test_new_filters_write_every_verified_control(void)
{
    pc_algo_camera_raw def;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    pc_smart_filter f;
    pc_psd_desc item;
    pc_psd_desc reread;
    pc_psd_bytes by;
    pc_smart_filter back;
    pc_algo_camera_raw typed;
    yyjson_mut_doc *ed = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *expected;
    pc_algo_camera_raw_default(&def);
    memset(&f, 0, sizeof f);
    must(pc_json_value_from_mut(pc_algo_camera_raw_to_json(d, &def), &f.params, NULL));
    yyjson_mut_doc_free(d);
    must(pc_str_set_c(&f.command, PC_IO_CAMERA_RAW_COMMAND, NULL));
    set_param(&f, "seed", "0");
    for (size_t i = 0; i < NSCALARS; i++) {
        const char *key = SCALARS[i][1];
        set_param(&f, SCALARS[i][0],
                  strcmp(key, "Ex12") == 0   ? "0.75"
                  : strcmp(key, "ShpR") == 0 ? "1.2"
                                             : "12.0");
    }
    set_param(&f, "curveSplits", "[20, 45, 80]");
    set_param(&f, "hslHue", "[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]");
    set_param(&f, "hslSat", "[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]");
    set_param(&f, "hslLum", "[1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]");
    f.blend = PC_BLEND_SCREEN;
    f.opacity = 0.5f;
    f.visible = false;
    item = for_filter(&f);
    must(pc_psd_desc_to_bytes(&item, &by, NULL));
    must(pc_psd_desc_parse(by.data, by.len, &reread, NULL));
    free(by.data);
    back = from_item(&reread);
    CHECK_EQ_STR(pc_str_c(&back.command), pc_str_c(&f.command));
    CHECK(!back.visible && back.blend == PC_BLEND_SCREEN && back.opacity == 0.5f);
    {
        yyjson_doc *imut = yyjson_mut_val_imut_copy(pc_json_value_root(&f.params), NULL);
        must(pc_algo_camera_raw_from_json(yyjson_doc_get_root(imut), &typed, NULL));
        yyjson_doc_free(imut);
    }
    expected = pc_algo_camera_raw_to_json(ed, &typed);
    {
        pc_json_value ev;
        must(pc_json_value_from_mut(expected, &ev, NULL));
        for (size_t i = 0; i < NSCALARS; i++) {
            pc_smart_filter tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.params = ev;
            CHECK(num(&back, SCALARS[i][0]) == num(&tmp, SCALARS[i][0]));
        }
        CHECK(pc_json_mut_equal(yyjson_mut_obj_get(pc_json_value_root(&back.params), "hslLum"),
                                yyjson_mut_obj_get(pc_json_value_root(&ev), "hslLum")));
        CHECK(pc_json_mut_equal(yyjson_mut_obj_get(pc_json_value_root(&back.params), "curveSplits"),
                                yyjson_mut_obj_get(pc_json_value_root(&ev), "curveSplits")));
        pc_json_value_fini(&ev);
    }
    pc_algo_camera_raw_fini(&typed);
    yyjson_mut_doc_free(ed);
    pc_smart_filter_fini(&back);
    pc_psd_desc_fini(&reread);
    pc_psd_desc_fini(&item);
    pc_smart_filter_fini(&f);
}

static void test_unsupported_processing_settings_remain_opaque(void)
{
    for (int k = 0; k < 5; k++) {
        pc_psd_desc item = source_item();
        pc_psd_desc d = fltr_of(&item);
        switch (k) {
        case 0:
            d_set(&d, "LCs ", v_text("unknown masks", false));
            break;
        case 1:
            d_set(&d, "PrVN", pc_psd_value_int(7));
            break;
        case 2:
            d_set(&d, "PrVe", pc_psd_value_int(123));
            break;
        case 3:
            d_set(&d, "Ex12", pc_psd_value_double(INFINITY));
            break;
        default:
            d_set(&d, "Cr12", v_text("bad type", false));
            break;
        }
        set_fltr(&item, &d);
        check_opaque(&item, "setting");
        pc_psd_desc_fini(&item);
    }
}

static void test_fractional_slider_levels_round(void)
{
    pc_psd_desc source = source_item();
    pc_smart_filter f = from_item(&source);
    pc_psd_desc item;
    pc_smart_filter back;
    static const int32_t CRV[] = {0, 0, 100, 121, 201, 180, 255, 255};
    pc_psd_value want = v_ints(CRV, 8);
    set_param(&f, "contrast", "-12.4");
    set_param(&f, "hslHue[0]", "10.5");
    set_param(&f, "gradeShadows.hue", "359.7");
    set_param(&f, "pointCurve", "[[0, 0], [100.2, 120.6], [200.6, 180.4], [255, 255]]");
    item = for_filter(&f);
    CHECK(fltr_get(&item, "Cr12")->kind == PC_PSD_V_INTEGER && fltr_get(&item, "Cr12")->i == -12);
    CHECK(fltr_get(&item, "HA_R")->i == 11);
    CHECK(fltr_get(&item, "STSH")->i == 0);
    CHECK(pc_psd_value_eq(fltr_get(&item, "Crv "), &want));
    back = from_item(&item);
    CHECK_EQ_STR(pc_str_c(&back.command), PC_IO_CAMERA_RAW_COMMAND);
    CHECK(num(&back, "contrast") == -12.0);
    pc_psd_value_fini(&want);
    pc_smart_filter_fini(&back);
    pc_psd_desc_fini(&item);
    pc_smart_filter_fini(&f);
    pc_psd_desc_fini(&source);
}

static void test_unsupported_or_malformed_exports_report_errors(void)
{
    static const char *const P[] = {
        "null",
        "{\"exposure\": \"bright\"}",
        "{\"exposure\": 1e40}",
        "{\"exposure\": 6}",
        "{\"contrast\": 101}",
        "{\"hslHue\": [1, 2]}",
        "{\"curveSplits\": [80, 50, 20]}",
        "{\"curveSplits\": [25, 25, 75]}",
        "{\"pointCurve\": [[0, 0], [0, 255]]}",
        "{\"sharpenRadius\": 4}",
        "{\"gradeShadows\": {\"hue\": 361}}",
        "{\"gradeShadows\": {\"unknown\": 1}}",
        "{\"seed\": 1}",
        "{\"vignetteHighlights\": 10}",
        "{\"futureSetting\": 1}",
        "{\"__cameraRawPsd\": \"zz\"}",
        "{\"__cameraRawPsd\": \"00\"}",
        "{\"__cameraRawPsd\": 12}",
        /* filterFX with filterID 1, as hex */
        "{\"__cameraRawPsd\": \"000000010000000000000008"
        "66696c7465724658"
        "00000001000000086669"
        "6c7465724944"
        "6c6f6e6700000001\"}",
    };
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) {
        yyjson_doc *jd;
        pc_json_value v;
        pc_psd_desc out;
        pc_error e;
        must(pc_json_parse(P[i], strlen(P[i]), &jd, NULL));
        must(pc_json_value_from_val(yyjson_doc_get_root(jd), &v, NULL));
        yyjson_doc_free(jd);
        if (pc_io_camera_raw_export_item(&v, &out, &e) == PC_OK) {
            PC_T_FAIL("%s exported", P[i]);
            pc_psd_desc_fini(&out);
        }
        pc_json_value_fini(&v);
    }
    {
        /* A template far past the size limit. */
        size_t n = (size_t)1024 * 1024 * 2 + 1;
        char *big = xmalloc(n + 32);
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *o = yyjson_mut_obj(d);
        pc_json_value v;
        pc_psd_desc out;
        memset(big, '0', n);
        big[n] = '\0';
        (void)yyjson_mut_obj_add_strn(d, o, "__cameraRawPsd", big, n);
        must(pc_json_value_from_mut(o, &v, NULL));
        CHECK(pc_io_camera_raw_export_item(&v, &out, NULL) != PC_OK);
        pc_json_value_fini(&v);
        yyjson_mut_doc_free(d);
        free(big);
    }
}

int main(int argc, char **argv)
{
    RUN(test_all_controls_roundtrip_and_patch_only_the_edited_field);
    RUN(test_active_unimplemented_controls_and_malformed_curves_stay_opaque);
    RUN(test_light_curve_and_hsl_editable_without_rewriting);
    RUN(test_new_filters_write_every_verified_control);
    RUN(test_unsupported_processing_settings_remain_opaque);
    RUN(test_fractional_slider_levels_round);
    RUN(test_unsupported_or_malformed_exports_report_errors);
    return pc_t_finish();
}
