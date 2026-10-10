/* serde JSON forms of the doc types (doc/json.h): the reference's vectors (every type and
 * variant, defaults, serde's error texts), idempotent round trips, and never-crash sweeps
 * (wrong types at every level, removed keys, hostile numbers, deep nesting) under ASan. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "doc/json.h"
#include "harness.h"
#include "json_types.h"
#include "json_vectors.h"

/* ---- helpers ---- */

static yyjson_doc *parse(const char *text)
{
    yyjson_doc *d = NULL;
    if (pc_json_parse(text, strlen(text), &d, NULL) != PC_OK) {
        return NULL;
    }
    return d;
}

/* Writes v (flags: PC_JSON_SORT_KEYS for a to_value comparison) into a malloc'd string. */
static char *write_str(yyjson_mut_val *v, unsigned flags)
{
    pc_buf b = PC_BUF_INIT;
    char *s;
    if (v == NULL || pc_json_write(v, flags, &b, NULL) != PC_OK) {
        pc_buf_fini(&b);
        return NULL;
    }
    s = malloc(b.len + 1);
    if (s != NULL) {
        memcpy(s, b.data, b.len);
        s[b.len] = '\0';
    }
    pc_buf_fini(&b);
    return s;
}

/* from_json(text) as type t; on success *ser / *val get to_string / to_value text. */
static pc_status run_type(const jt_entry *t, const char *text, char **ser, char **val, pc_error *err)
{
    yyjson_doc *doc = parse(text);
    void *obj;
    pc_status st;
    if (doc == NULL) {
        return pc_fail(err, PC_ERR_FORMAT, "unparsable test input");
    }
    obj = calloc(1, t->size);
    if (obj == NULL) {
        yyjson_doc_free(doc);
        return PC_ERR_NOMEM;
    }
    st = t->from(yyjson_doc_get_root(doc), obj, err);
    if (st == PC_OK) {
        yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
        if (ser != NULL) {
            *ser = write_str(t->to(md, obj, 0), 0);
        }
        if (val != NULL) {
            *val = write_str(t->to(md, obj, PC_DOC_JSON_VALUE), PC_JSON_SORT_KEYS);
        }
        yyjson_mut_doc_free(md);
        t->fini(obj);
    }
    free(obj);
    yyjson_doc_free(doc);
    return st;
}

static char *dupstr(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p != NULL) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

/* ---- the reference's vectors ---- */

static void test_reference_vectors(void)
{
    size_t i;
    size_t n = sizeof JSON_VECS / sizeof JSON_VECS[0];
    for (i = 0; i < n; i++) {
        const json_vec *v = &JSON_VECS[i];
        const jt_entry *t = jt_find(v->type);
        char *ser = NULL;
        char *val = NULL;
        pc_error err = {PC_OK, 0, ""};
        pc_status st;
        CHECK(t != NULL);
        if (t == NULL) {
            continue;
        }
        st = run_type(t, v->input, &ser, &val, &err);
        if (strncmp(v->expect, "err\t", 4) == 0) {
            const char *msg = v->expect + 4;
            size_t cmp = strlen(msg);
            /* pc_error keeps 255 bytes of a longer message. */
            cmp = cmp > PC_ERROR_MSG_MAX - 1 ? PC_ERROR_MSG_MAX - 1 : cmp;
            CHECK_EQ_INT(st, PC_ERR_INVALID);
            if (st != PC_ERR_INVALID || strncmp(err.msg, msg, cmp) != 0) {
                PC_T_FAIL("%s %s: got \"%s\", want \"%s\"", v->type, v->input, err.msg, msg);
            }
        } else {
            const char *tab = strchr(v->expect + 3, '\t');
            char *want_ser = tab != NULL ? dupstr(v->expect + 3, (size_t)(tab - (v->expect + 3))) : NULL;
            CHECK_EQ_INT(st, PC_OK);
            CHECK(tab != NULL);
            if (st == PC_OK && tab != NULL) {
                if (ser == NULL || strcmp(ser, want_ser) != 0) {
                    PC_T_FAIL("%s %s: to_string %s, want %s", v->type, v->input, ser ? ser : "(null)",
                              want_ser);
                }
                if (val == NULL || strcmp(val, tab + 1) != 0) {
                    PC_T_FAIL("%s %s: to_value %s, want %s", v->type, v->input, val ? val : "(null)",
                              tab + 1);
                }
            } else if (st != PC_OK) {
                PC_T_FAIL("%s %s: %s", v->type, v->input, err.msg);
            }
            free(want_ser);
        }
        free(ser);
        free(val);
    }
}

/* to_string output reads back to the same value (and so the same text). */
static void test_round_trips_are_idempotent(void)
{
    size_t i;
    size_t n = sizeof JSON_VECS / sizeof JSON_VECS[0];
    size_t done = 0;
    for (i = 0; i < n; i++) {
        const json_vec *v = &JSON_VECS[i];
        const jt_entry *t = jt_find(v->type);
        const char *tab;
        char *first;
        char *again = NULL;
        char *val = NULL;
        pc_error err;
        if (t == NULL || strncmp(v->expect, "ok\t", 3) != 0 || (tab = strchr(v->expect + 3, '\t')) == NULL) {
            continue;
        }
        first = dupstr(v->expect + 3, (size_t)(tab - (v->expect + 3)));
        if (run_type(t, first, &again, &val, &err) != PC_OK) {
            /* Only a non-finite float (written as null) doesn't read back, as in the reference. */
            CHECK(strstr(first, "null") != NULL && strstr(err.msg, "invalid type: null") != NULL);
        } else {
            CHECK(again != NULL && strcmp(again, first) == 0);
            CHECK(val != NULL && strcmp(val, tab + 1) == 0);
        }
        free(first);
        free(again);
        free(val);
        done++;
    }
    CHECK(done > 200);
}

/* Every type answers garbage with an error, never a crash. */
static void test_every_type_rejects_garbage(void)
{
    static const char *const garbage[] = {"5",    "-1",   "1.5",       "true",
                                          "\"\"", "[[]]", "{\"\":{}}", "[null,null]"};
    size_t i;
    size_t g;
    for (i = 0; i < sizeof JT_TYPES / sizeof JT_TYPES[0]; i++) {
        for (g = 0; g < sizeof garbage / sizeof garbage[0]; g++) {
            pc_error err;
            char *ser = NULL;
            pc_status st = run_type(&JT_TYPES[i], garbage[g], &ser, NULL, &err);
            CHECK(st == PC_OK || st == PC_ERR_INVALID);
            free(ser);
        }
    }
}

/* ---- never-crash sweep: every node of every valid sample replaced, keys removed ---- */

typedef enum mut_op { MUT_NONE, MUT_REPLACE, MUT_REMOVE } mut_op;

typedef struct mut_ctx {
    size_t counter; /* value nodes visited so far */
    size_t target;
    mut_op op;
    int garbage;
} mut_ctx;

static yyjson_mut_val *garbage_val(yyjson_mut_doc *d, int g)
{
    switch (g) {
    case 0:
        return yyjson_mut_null(d);
    case 1:
        return yyjson_mut_bool(d, true);
    case 2:
        return yyjson_mut_uint(d, 4294967296u);
    case 3:
        return yyjson_mut_sint(d, -129);
    case 4:
        return yyjson_mut_real(d, 1.5);
    case 5:
        return yyjson_mut_real(d, 1e300);
    case 6:
        return yyjson_mut_str(d, "Linear");
    case 7:
        return yyjson_mut_arr(d);
    case 8:
        return yyjson_mut_obj(d);
    default:
        return yyjson_mut_uint(d, 300);
    }
}
#define GARBAGE_COUNT 10

/* Copy of v with node `target` (pre-order) replaced or removed from its parent. */
static yyjson_mut_val *mut_copy(yyjson_mut_doc *d, yyjson_val *v, mut_ctx *c)
{
    size_t me = c->counter++;
    if (me == c->target && c->op == MUT_REPLACE) {
        return garbage_val(d, c->garbage);
    }
    if (yyjson_is_arr(v)) {
        yyjson_mut_val *a = yyjson_mut_arr(d);
        yyjson_arr_iter it;
        yyjson_val *e;
        (void)yyjson_arr_iter_init(v, &it);
        while ((e = yyjson_arr_iter_next(&it)) != NULL) {
            bool drop = c->op == MUT_REMOVE && c->counter == c->target;
            yyjson_mut_val *x = mut_copy(d, e, c);
            if (!drop) {
                (void)yyjson_mut_arr_append(a, x);
            }
        }
        return a;
    }
    if (yyjson_is_obj(v)) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_obj_iter it;
        yyjson_val *k;
        (void)yyjson_obj_iter_init(v, &it);
        while ((k = yyjson_obj_iter_next(&it)) != NULL) {
            bool drop = c->op == MUT_REMOVE && c->counter == c->target;
            yyjson_mut_val *x = mut_copy(d, yyjson_obj_iter_get_val(k), c);
            if (!drop) {
                (void)yyjson_mut_obj_add(o, yyjson_mut_strncpy(d, yyjson_get_str(k), yyjson_get_len(k)), x);
            }
        }
        return o;
    }
    return yyjson_val_mut_copy(d, v);
}

static size_t sweep_one(const jt_entry *t, yyjson_val *root, mut_op op, size_t target, int g, size_t *nodes)
{
    mut_ctx c = {0, target, op, g};
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_doc *imut;
    void *obj = calloc(1, t->size);
    pc_error err;
    pc_status st;
    yyjson_mut_doc_set_root(md, mut_copy(md, root, &c));
    *nodes = c.counter;
    imut = yyjson_mut_doc_imut_copy(md, NULL);
    yyjson_mut_doc_free(md);
    st = t->from(yyjson_doc_get_root(imut), obj, &err);
    CHECK(st == PC_OK || st == PC_ERR_INVALID);
    if (st == PC_OK) {
        yyjson_mut_doc *w = yyjson_mut_doc_new(NULL);
        CHECK(t->to(w, obj, 0) != NULL);
        CHECK(t->to(w, obj, PC_DOC_JSON_VALUE) != NULL);
        yyjson_mut_doc_free(w);
        t->fini(obj);
    } else {
        /* Out-params are only written on success. */
        static const unsigned char zero[4096];
        CHECK(t->size > sizeof zero || memcmp(obj, zero, t->size) == 0);
    }
    free(obj);
    yyjson_doc_free(imut);
    return st == PC_OK ? 1u : 0u;
}

static void test_never_crash_mutation_sweep(void)
{
    size_t i;
    size_t runs = 0;
    size_t oks = 0;
    for (i = 0; i < sizeof JSON_VECS / sizeof JSON_VECS[0]; i++) {
        const json_vec *v = &JSON_VECS[i];
        const jt_entry *t = jt_find(v->type);
        yyjson_doc *doc;
        size_t nodes = 0;
        size_t n;
        int g;
        /* The valid samples are the inputs whose expectation is a success. */
        if (t == NULL || strncmp(v->expect, "ok\t", 3) != 0 || (doc = parse(v->input)) == NULL) {
            continue;
        }
        (void)sweep_one(t, yyjson_doc_get_root(doc), MUT_NONE, SIZE_MAX, 0, &nodes);
        for (n = 0; n < nodes; n++) {
            size_t dummy;
            for (g = 0; g < GARBAGE_COUNT; g++) {
                oks += sweep_one(t, yyjson_doc_get_root(doc), MUT_REPLACE, n, g, &dummy);
                runs++;
            }
            if (n > 0) {
                oks += sweep_one(t, yyjson_doc_get_root(doc), MUT_REMOVE, n, 0, &dummy);
                runs++;
            }
        }
        yyjson_doc_free(doc);
    }
    CHECK(runs > 20000);
    CHECK(oks > 1000 && oks < runs);
}

/* ---- the reference's serde #[test]s ---- */

/* lib.rs layer_color_ids_and_existing_serialized_names_stay_stable */
static void test_label_color_serde_names(void)
{
    static const char *const names[] = {"None",   "Red",  "Orange",  "Yellow", "Green",   "Blue",
                                        "Violet", "Gray", "Seafoam", "Indigo", "Magenta", "Fuchsia"};
    size_t i;
    for (i = 0; i < PC_LABEL_COLOR_COUNT; i++) {
        pc_label_color c = (pc_label_color)i;
        pc_label_color back = PC_LABEL_NONE;
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *v = pc_label_color_to_json(d, &c, PC_DOC_JSON_VALUE);
        char text[64];
        yyjson_doc *doc;
        CHECK(v != NULL && yyjson_mut_is_str(v) && strcmp(yyjson_mut_get_str(v), names[i]) == 0);
        (void)snprintf(text, sizeof text, "\"%s\"", names[i]);
        doc = parse(text);
        CHECK_EQ_INT(pc_label_color_from_json(yyjson_doc_get_root(doc), &back, NULL), PC_OK);
        CHECK_EQ_INT(back, c);
        yyjson_doc_free(doc);
        yyjson_mut_doc_free(d);
    }
    {
        pc_error err;
        pc_label_color back = PC_LABEL_RED;
        yyjson_doc *doc = parse("\"Unknown\"");
        CHECK_EQ_INT(pc_label_color_from_json(yyjson_doc_get_root(doc), &back, &err), PC_ERR_INVALID);
        CHECK_EQ_INT(back, PC_LABEL_RED);
        CHECK_CONTAINS(err.msg, "unknown variant `Unknown`, expected one of `None`, `Red`");
        yyjson_doc_free(doc);
    }
}

static pc_status adj_from(const char *text, pc_adjustment *a, pc_error *err)
{
    yyjson_doc *doc = parse(text);
    pc_status st = pc_adjustment_from_json(yyjson_doc_get_root(doc), a, err);
    yyjson_doc_free(doc);
    return st;
}

/* adjust.rs old_files_get_neutral_new_fields */
static void test_old_files_get_neutral_new_fields(void)
{
    pc_adjustment a;
    pc_hue_range def[6];
    size_t i;
    pc_hue_range_defaults(def);
    CHECK_EQ_INT(
        adj_from("{\"HueSaturation\":{\"hue\":1.0,\"saturation\":2.0,\"lightness\":3.0,\"colorize\":false}}",
                 &a, NULL),
        PC_OK);
    CHECK_EQ_INT(a.kind, PC_ADJ_HUE_SATURATION);
    for (i = 0; i < 6; i++) {
        CHECK(memcmp(&a.u.hue_saturation.ranges[i], &def[i], sizeof def[i]) == 0);
    }
    pc_adjustment_fini(&a);
    CHECK_EQ_INT(adj_from("{\"Curves\":{\"master\":[],\"per_channel\":[[],[],[]]}}", &a, NULL), PC_OK);
    CHECK(a.kind == PC_ADJ_CURVES && a.u.curves.space == PC_TONE_RGB && a.u.curves.black.len == 0);
    pc_adjustment_fini(&a);
    CHECK_EQ_INT(adj_from("{\"GradientMap\":{\"stops\":[],\"reverse\":true}}", &a, NULL), PC_OK);
    CHECK(a.kind == PC_ADJ_GRADIENT_MAP && !a.u.gradient_map.dither && a.u.gradient_map.reverse);
    pc_adjustment_fini(&a);
    /* Levels' black record defaults to the neutral channel. */
    CHECK_EQ_INT(
        adj_from(
            "{\"Levels\":{\"master\":[0,1,1,0,1],\"per_channel\":[[0,1,1,0,1],[0,1,1,0,1],[0,1,1,0,1]]}}", &a,
            NULL),
        PC_OK);
    CHECK(a.u.levels.black.in_white == 1.0f && a.u.levels.black.gamma == 1.0f &&
          a.u.levels.space == PC_TONE_RGB);
    pc_adjustment_fini(&a);
}

/* advanced.rs partial_json_keeps_photoshop_defaults */
static void test_advanced_partial_json_keeps_defaults(void)
{
    pc_advanced_blending a;
    pc_advanced_blending want = pc_advanced_blending_default();
    yyjson_doc *doc = parse("{\"knockout\":\"deep\"}");
    yyjson_mut_doc *d;
    char *s;
    CHECK_EQ_INT(pc_advanced_blending_from_json(yyjson_doc_get_root(doc), &a, NULL), PC_OK);
    want.knockout = PC_KNOCKOUT_DEEP;
    CHECK(pc_advanced_blending_eq(&a, &want));
    yyjson_doc_free(doc);
    doc = parse("{}");
    CHECK_EQ_INT(pc_advanced_blending_from_json(yyjson_doc_get_root(doc), &a, NULL), PC_OK);
    CHECK(pc_advanced_blending_is_default(&a));
    yyjson_doc_free(doc);
    a = pc_advanced_blending_default();
    a.layer_mask_hides_effects = true;
    d = yyjson_mut_doc_new(NULL);
    s = write_str(pc_advanced_blending_to_json(d, &a, 0), 0);
    CHECK_CONTAINS(s, "\"layerMaskHidesEffects\":true");
    free(s);
    yyjson_mut_doc_free(d);
}

/* vector.rs serde_roundtrip */
static void test_vector_serde_roundtrip(void)
{
    static const double pts[2][2] = {{0.0, 0.0}, {3.0, 4.0}};
    pc_path p = {0};
    pc_path p2;
    pc_shape_stroke s;
    pc_shape_stroke s2;
    pc_live_shape ls = {0};
    pc_live_shape ls2;
    float dashes[2] = {2.0f, 1.0f};
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_doc *back;
    char *text;
    size_t i;
    p.subpaths = calloc(1, sizeof *p.subpaths);
    p.nsubpaths = 1;
    CHECK_EQ_INT(pc_subpath_polygon(pts, 2, &p.subpaths[0], NULL), PC_OK);
    p.subpaths[0].op = PC_PATH_EXCLUDE;
    pc_shape_stroke_default(&s);
    s.dashes = malloc(sizeof dashes);
    memcpy(s.dashes, dashes, sizeof dashes);
    s.ndashes = 2;
    ls.kind = PC_LIVE_RECT;
    for (i = 0; i < 4; i++) {
        ls.radii[i] = 1.0;
    }
    (void)yyjson_mut_arr_append(arr, pc_path_to_json(d, &p, 0));
    (void)yyjson_mut_arr_append(arr, pc_shape_stroke_to_json(d, &s, 0));
    (void)yyjson_mut_arr_append(arr, pc_live_shape_to_json(d, &ls, 0));
    text = write_str(arr, 0);
    back = parse(text);
    CHECK_EQ_INT(pc_path_from_json(yyjson_arr_get(yyjson_doc_get_root(back), 0), &p2, NULL), PC_OK);
    CHECK_EQ_INT(pc_shape_stroke_from_json(yyjson_arr_get(yyjson_doc_get_root(back), 1), &s2, NULL), PC_OK);
    CHECK_EQ_INT(pc_live_shape_from_json(yyjson_arr_get(yyjson_doc_get_root(back), 2), &ls2, NULL), PC_OK);
    CHECK(pc_path_eq(&p2, &p));
    CHECK(pc_shape_stroke_eq(&s2, &s));
    CHECK(pc_live_shape_eq(&ls2, &ls));
    CHECK_CONTAINS(text, "{\"kind\":\"rect\",\"rect\":[0.0,0.0,0.0,0.0],\"radii\":[1.0,1.0,1.0,1.0]}");
    pc_path_fini(&p2);
    pc_shape_stroke_fini(&s2);
    yyjson_doc_free(back);
    free(text);
    back = parse("{\"subpaths\":[{\"closed\":true,\"knots\":[]}]}");
    CHECK_EQ_INT(pc_path_from_json(yyjson_doc_get_root(back), &p2, NULL), PC_OK);
    CHECK(p2.nsubpaths == 1 && p2.subpaths[0].op == PC_PATH_COMBINE && p2.subpaths[0].closed);
    pc_path_fini(&p2);
    yyjson_doc_free(back);
    pc_path_fini(&p);
    pc_shape_stroke_fini(&s);
    yyjson_mut_doc_free(d);
}

/* text.rs styles_serialize */
static void test_text_styles_serialize(void)
{
    pc_char_style s;
    pc_char_style back;
    pc_paragraph_style p;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_doc *doc;
    char *text;
    pc_char_style_default(&s);
    s.features = calloc(1, sizeof *s.features);
    s.nfeatures = 1;
    CHECK_EQ_INT(pc_str_set_c(&s.features[0].tag, "ss01", NULL), PC_OK);
    s.features[0].value = 1;
    text = write_str(pc_char_style_to_json(d, &s, PC_DOC_JSON_VALUE), 0);
    doc = parse(text);
    CHECK_EQ_INT(pc_char_style_from_json(yyjson_doc_get_root(doc), &back, NULL), PC_OK);
    CHECK(pc_char_style_eq(&back, &s));
    pc_char_style_fini(&back);
    yyjson_doc_free(doc);
    free(text);
    /* Missing keys take defaults. */
    doc = parse("{\"align\":\"Center\"}");
    CHECK_EQ_INT(pc_paragraph_style_from_json(yyjson_doc_get_root(doc), &p, NULL), PC_OK);
    CHECK(p.align == PC_TEXT_ALIGN_CENTER && p.auto_leading == 1.2f && !p.has_style_sheet);
    yyjson_doc_free(doc);
    pc_char_style_fini(&s);
    yyjson_mut_doc_free(d);
}

/* variables.rs round_trips_through_json */
static void test_variables_round_trip(void)
{
    static const char *text =
        "{\"defs\":[{\"name\":\"show\",\"layer\":1,\"kind\":{\"type\":\"visibility\"}},"
        "{\"name\":\"title\",\"layer\":2,\"kind\":{\"type\":\"textReplacement\"}},"
        "{\"name\":\"photo\",\"layer\":3,\"kind\":{\"type\":\"pixelReplacement\",\"method\":\"fill\","
        "\"align\":\"topLeft\",\"clip\":true}}],\"data_sets\":[{\"name\":\"row1\",\"values\":["
        "{\"variable\":\"show\",\"kind\":\"visibility\",\"value\":false},"
        "{\"variable\":\"title\",\"kind\":\"text\",\"value\":\"Hello\"},"
        "{\"variable\":\"photo\",\"kind\":\"pixels\",\"value\":\"/tmp/a.png\"}]}],\"active\":0}";
    pc_variables v;
    pc_variables v2;
    yyjson_doc *doc = parse(text);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    char *out;
    CHECK_EQ_INT(pc_variables_from_json(yyjson_doc_get_root(doc), &v, NULL), PC_OK);
    CHECK(v.ndefs == 3 && v.ndata_sets == 1 && v.has_active && v.active == 0);
    CHECK(v.defs[2].kind.type == PC_VAR_PIXEL_REPLACEMENT && v.defs[2].kind.method == PC_PIXEL_FILL &&
          v.defs[2].kind.align == PC_ALIGN_TOP_LEFT && v.defs[2].kind.clip);
    CHECK(v.data_sets[0].nvalues == 3 && v.data_sets[0].values[0].kind == PC_DATA_VISIBILITY &&
          !v.data_sets[0].values[0].visibility);
    CHECK(pc_str_eq_c(&v.data_sets[0].values[1].text, "Hello"));
    out = write_str(pc_variables_to_json(d, &v, 0), 0);
    CHECK_EQ_STR(out, text);
    yyjson_doc_free(doc);
    doc = parse(out);
    CHECK_EQ_INT(pc_variables_from_json(yyjson_doc_get_root(doc), &v2, NULL), PC_OK);
    CHECK(pc_variables_eq(&v, &v2));
    pc_variables_fini(&v);
    pc_variables_fini(&v2);
    free(out);
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(d);
}

/* video.rs round_trips */
static void test_timeline_round_trip(void)
{
    pc_timeline t = pc_timeline_new(48, 25.0f);
    pc_timeline back;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    char *text = write_str(pc_timeline_to_json(d, &t, 0), 0);
    yyjson_doc *doc = parse(text);
    CHECK_EQ_STR(text, "{\"fps\":25.0,\"duration\":48,\"current\":0,\"work_start\":0,\"work_end\":48}");
    CHECK_EQ_INT(pc_timeline_from_json(yyjson_doc_get_root(doc), &back, NULL), PC_OK);
    CHECK(pc_timeline_eq(&t, &back));
    free(text);
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(d);
}

/* analysis.rs serde_defaults_fill_missing_fields */
static void test_analysis_defaults_fill_missing_fields(void)
{
    pc_measurement m;
    pc_note n;
    yyjson_doc *doc = parse("{}");
    CHECK_EQ_INT(pc_measurement_from_json(yyjson_doc_get_root(doc), &m, NULL), PC_OK);
    CHECK(pc_measurement_is_empty(&m));
    CHECK(pc_str_eq_c(&m.scale.units, "pixels"));
    pc_measurement_fini(&m);
    yyjson_doc_free(doc);
    doc = parse("{\"text\":\"hi\"}");
    CHECK_EQ_INT(pc_note_from_json(yyjson_doc_get_root(doc), &n, NULL), PC_OK);
    CHECK(pc_str_eq_c(&n.text, "hi"));
    CHECK(n.color.c[0] == 1.0f && n.color.c[1] == 1.0f && n.color.c[2] == 0.51f && n.color.alpha == 1.0f);
    pc_note_fini(&n);
    yyjson_doc_free(doc);
    doc = parse("{\"points\":[[1,2]]}");
    {
        pc_count_group g;
        CHECK_EQ_INT(pc_count_group_from_json(yyjson_doc_get_root(doc), &g, NULL), PC_OK);
        CHECK(pc_str_eq_c(&g.name, "Count Group 1") && g.marker_size == 1 && g.label_size == 8 && g.visible);
        CHECK(g.npoints == 1 && g.points[0][0] == 1.0 && g.points[0][1] == 2.0);
        pc_count_group_fini(&g);
    }
    yyjson_doc_free(doc);
}

/* slices.rs exhausted_slice_id_survives_serialization */
static void test_exhausted_slice_id_survives_serialization(void)
{
    pc_slices s = {0};
    pc_slices back;
    uint32_t next = 0;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v;
    yyjson_doc *doc;
    s.list = calloc(1, sizeof *s.list);
    s.len = 1;
    pc_slice_default(&s.list[0]);
    s.list[0].id = UINT32_MAX;
    v = pc_slices_to_json(d, &s, PC_DOC_JSON_VALUE);
    CHECK(v != NULL);
    yyjson_mut_doc_set_root(d, v);
    doc = yyjson_mut_doc_imut_copy(d, NULL);
    CHECK_EQ_INT(pc_slices_from_json(yyjson_doc_get_root(doc), &back, NULL), PC_OK);
    CHECK(!pc_slices_next_id(&back, &next));
    CHECK(pc_slices_eq(&s, &back));
    pc_slices_fini(&back);
    pc_slices_fini(&s);
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(d);
}

/* ---- numbers ---- */

static void test_non_finite_floats_write_null_and_null_is_rejected(void)
{
    pc_color c = {PC_COLOR_MODE_RGB, {NAN, INFINITY, -INFINITY, 0.5f}, 1.0f};
    pc_point p = {NAN, -INFINITY};
    pc_color back;
    pc_error err;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    char *s = write_str(pc_color_to_json(d, &c, 0), 0);
    char *pv = write_str(pc_point_to_json(d, &p, PC_DOC_JSON_VALUE), 0);
    yyjson_doc *doc = parse(s);
    CHECK_EQ_STR(s, "{\"mode\":\"Rgb\",\"c\":[null,null,null,0.5],\"alpha\":1.0}");
    CHECK_EQ_STR(pv, "{\"x\":null,\"y\":null}");
    back.alpha = 7.0f;
    CHECK_EQ_INT(pc_color_from_json(yyjson_doc_get_root(doc), &back, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "invalid type: null, expected f32");
    CHECK(back.alpha == 7.0f); /* untouched */
    free(s);
    free(pv);
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(d);
}

static void test_f32_text_and_value_forms(void)
{
    pc_curve_point cp = {0.1f, 1e-7f};
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    char *s = write_str(pc_curve_point_to_json(d, &cp, 0), 0);
    char *v = write_str(pc_curve_point_to_json(d, &cp, PC_DOC_JSON_VALUE), 0);
    yyjson_mut_val *jv = pc_curve_point_to_json(d, &cp, PC_DOC_JSON_VALUE);
    CHECK_EQ_STR(s, "{\"input\":0.1,\"output\":1e-7}");
    /* serde_json::to_value widens f32 to f64. */
    CHECK_EQ_STR(v, "{\"input\":0.10000000149011612,\"output\":1.0000000116860974e-7}");
    CHECK(yyjson_mut_is_real(yyjson_mut_obj_get(jv, "input")));
    free(s);
    free(v);
    yyjson_mut_doc_free(d);
}

static void expect_err(const char *type, const char *text, const char *msg)
{
    const jt_entry *t = jt_find(type);
    pc_error err = {PC_OK, 0, ""};
    char *ser = NULL;
    pc_status st = run_type(t, text, &ser, NULL, &err);
    CHECK_EQ_INT(st, PC_ERR_INVALID);
    if (strcmp(err.msg, msg) != 0) {
        PC_T_FAIL("%s %s: got \"%s\", want \"%s\"", type, text, err.msg, msg);
    }
    free(ser);
}

static void test_hostile_integers(void)
{
    expect_err("size", "{\"width\":-1,\"height\":2}", "invalid value: integer `-1`, expected u32");
    expect_err("size", "{\"width\":4294967296,\"height\":2}",
               "invalid value: integer `4294967296`, expected u32");
    expect_err("size", "{\"width\":1.0,\"height\":2}", "invalid type: floating point `1.0`, expected u32");
    expect_err("size", "{\"width\":1e20,\"height\":2}", "invalid type: floating point `1e+20`, expected u32");
    expect_err("size", "{\"width\":18446744073709551616,\"height\":2}",
               "invalid type: floating point `1.8446744073709552e+19`, expected u32");
    expect_err("size", "{\"width\":-0,\"height\":2}", "invalid type: floating point `-0.0`, expected u32");
    expect_err("rect", "{\"x0\":-2147483649,\"y0\":0,\"x1\":0,\"y1\":0}",
               "invalid value: integer `-2147483649`, expected i32");
    expect_err("blend_range", "{\"black\":[0,256],\"white\":[0,0]}",
               "invalid value: integer `256`, expected u8");
    expect_err("font_feature", "{\"tag\":\"a\",\"value\":65536}",
               "invalid value: integer `65536`, expected u16");
    expect_err("text_run", "{\"len\":-1,\"style\":{}}", "invalid value: integer `-1`, expected usize");
    expect_err("timeline", "{\"fps\":1,\"duration\":-9223372036854775808,\"current\":0,\"work_end\":2}",
               "invalid value: integer `-9223372036854775808`, expected usize");
    expect_err("variable_def", "{\"name\":\"a\",\"layer\":-5,\"kind\":{\"type\":\"visibility\"}}",
               "invalid value: integer `-5`, expected u64");
    expect_err("adjustment", "{\"Unsupported\":{\"psd_key\":\"a\",\"raw\":[1,2,300]}}",
               "invalid value: integer `300`, expected u8");
}

static void test_shape_errors(void)
{
    expect_err("size", "[1]", "invalid length 1, expected struct Size with 2 elements");
    expect_err("size", "[1,2,3]", "invalid length 3, expected fewer elements in array");
    expect_err("size", "\"x\"", "invalid type: string \"x\", expected struct Size");
    expect_err("size", "{\"width\":1}", "missing field `height`");
    /* Fields are deserialised in key order: height before width. */
    expect_err("size", "{\"width\":true,\"height\":\"a\"}", "invalid type: string \"a\", expected u32");
    expect_err("color", "{\"mode\":\"Rgb\",\"c\":[1,2,3],\"alpha\":1}",
               "invalid length 3, expected an array of length 4");
    expect_err("color_indicates", "\"x\"", "unknown variant `x`, expected `maskedAreas` or `selectedAreas`");
    expect_err("color_indicates", "{}", "invalid value: map, expected map with a single key");
    expect_err("color_indicates", "[1]", "invalid type: sequence, expected string or map");
    expect_err("adjustment", "{\"Invert\":[]}", "invalid type: sequence, expected unit");
    expect_err("adjustment", "\"Levels\"", "invalid type: unit variant, expected struct variant");
    expect_err("adjustment", "{\"Threshold\":[5]}", "invalid type: sequence, expected struct variant");
    expect_err("effect", "\"DropShadow\"", "invalid type: unit variant, expected newtype variant");
    expect_err(
        "fill",
        "{\"Gradient\":{\"stops\":[[0]],\"angle\":1,\"scale\":1,\"style\":\"Linear\",\"reverse\":false}}",
        "invalid length 1, expected a tuple of size 2");
    expect_err("live_shape", "{\"kind\":\"rect\",\"rect\":[0,0,1,1,5],\"radii\":[0,0,0,0]}",
               "invalid length 5, expected 4 elements in sequence");
    expect_err("live_shape", "[\"ellipse\"]",
               "invalid length 0, expected struct variant LiveShape::Ellipse with 1 element");
    expect_err("live_shape", "{\"kind\":1}", "invalid type: integer `1`, expected variant identifier");
    expect_err("live_shape", "5", "invalid type: integer `5`, expected internally tagged enum LiveShape");
    expect_err("var_kind", "[]", "missing field `type`");
    expect_err("var_kind", "[\"visibility\",1]", "invalid length 1, expected 0 elements in sequence");
    expect_err("data_value", "{\"variable\":\"x\",\"kind\":\"visibility\"}", "missing field `value`");
    expect_err("data_value", "{\"variable\":\"x\",\"kind\":1,\"value\":\"a\"}",
               "invalid type: integer `1`, expected string or map");
    expect_err("data_value", "[\"x\",\"text\",\"a\"]", "invalid type: sequence, expected struct DataValue");
    expect_err("character_style_def", "{\"attrs\":5}", "invalid type: integer `5`, expected a map");
    expect_err("smart_filter", "{\"command\":\"x\",\"blend\":\"Normal\",\"opacity\":0.5,\"visible\":true}",
               "missing field `params`");
}

/* Structs read from arrays (serde's visit_seq), with defaults for missing trailing fields. */
static void test_structs_from_sequences(void)
{
    const jt_entry *t = jt_find("char_style");
    char *ser = NULL;
    pc_error err;
    CHECK_EQ_INT(run_type(t, "[\"a\",\"b\",null,5]", &ser, NULL, &err), PC_OK);
    CHECK_CONTAINS(ser, "\"font_family\":\"a\",\"font_style\":\"b\",\"postscript_name\":null,\"weight\":5,");
    CHECK_CONTAINS(ser, "\"size_pt\":12.0");
    free(ser);
    ser = NULL;
    CHECK_EQ_INT(run_type(jt_find("var_kind"), "[\"pixelReplacement\",\"fill\"]", &ser, NULL, &err), PC_OK);
    CHECK_EQ_STR(ser,
                 "{\"type\":\"pixelReplacement\",\"method\":\"fill\",\"align\":\"center\",\"clip\":false}");
    free(ser);
}

/* serde_json::Value fields keep any JSON, normalised (sorted keys, last duplicate wins). */
static void test_value_fields_and_deep_nesting(void)
{
    char text[2048];
    size_t i;
    size_t n = 0;
    char *ser = NULL;
    pc_error err;
    const jt_entry *t = jt_find("smart_filter");
    CHECK_EQ_INT(run_type(t,
                          "{\"command\":\"x\",\"params\":{\"b\":1,\"a\":2,\"b\":[1.0,-0.0,\"\\u0000\"]},"
                          "\"blend\":\"Normal\",\"opacity\":0.5,\"visible\":true}",
                          &ser, NULL, &err),
                 PC_OK);
    CHECK_EQ_STR(ser,
                 "{\"command\":\"x\",\"params\":{\"a\":2,\"b\":[1.0,-0.0,\"\\u0000\"]},\"blend\":\"Normal\","
                 "\"opacity\":0.5,\"visible\":true}");
    free(ser);
    /* 126 nested arrays inside the 127-level limit, as a Value and as a wrong-typed field. */
    n += (size_t)snprintf(text + n, sizeof text - n, "{\"command\":\"x\",\"params\":");
    for (i = 0; i < 125; i++) {
        text[n++] = '[';
    }
    for (i = 0; i < 125; i++) {
        text[n++] = ']';
    }
    (void)snprintf(text + n, sizeof text - n, ",\"blend\":\"Normal\",\"opacity\":0.5,\"visible\":true}");
    ser = NULL;
    CHECK_EQ_INT(run_type(t, text, &ser, NULL, &err), PC_OK);
    free(ser);
    n = 0;
    n += (size_t)snprintf(text + n, sizeof text - n, "{\"command\":\"x\",\"params\":null,\"opacity\":");
    for (i = 0; i < 125; i++) {
        text[n++] = '[';
    }
    for (i = 0; i < 125; i++) {
        text[n++] = ']';
    }
    (void)snprintf(text + n, sizeof text - n, ",\"blend\":\"Normal\",\"visible\":true}");
    expect_err("smart_filter", text, "invalid type: sequence, expected f32");
}

static void test_null_arguments(void)
{
    pc_size s = {1, 2};
    pc_error err;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK_EQ_INT(pc_size_from_json(NULL, &s, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "invalid type: null, expected struct Size");
    CHECK(s.width == 1 && s.height == 2);
    CHECK(pc_size_to_json(NULL, &s, 0) == NULL);
    CHECK(pc_size_to_json(d, NULL, 0) == NULL);
    yyjson_mut_doc_free(d);
}

/* Corrupt C values (discriminants out of range) never crash the writers. */
static void test_corrupt_values_write(void)
{
    pc_adjustment a;
    pc_live_shape ls;
    pc_data_value dv;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    memset(&a, 0, sizeof a);
    memset(&ls, 0, sizeof ls);
    memset(&dv, 0, sizeof dv);
    a.kind = (pc_adjustment_kind)99;
    ls.kind = (pc_live_shape_kind)99;
    dv.kind = (pc_data_value_kind)99;
    CHECK(yyjson_mut_is_null(pc_adjustment_to_json(d, &a, 0)));
    CHECK(yyjson_mut_is_null(pc_live_shape_to_json(d, &ls, 0)));
    CHECK(yyjson_mut_is_null(pc_data_value_to_json(d, &dv, 0)));
    yyjson_mut_doc_free(d);
}

int main(int argc, char **argv)
{
    RUN(test_reference_vectors);
    RUN(test_round_trips_are_idempotent);
    RUN(test_every_type_rejects_garbage);
    RUN(test_never_crash_mutation_sweep);
    RUN(test_label_color_serde_names);
    RUN(test_old_files_get_neutral_new_fields);
    RUN(test_advanced_partial_json_keeps_defaults);
    RUN(test_vector_serde_roundtrip);
    RUN(test_text_styles_serialize);
    RUN(test_variables_round_trip);
    RUN(test_timeline_round_trip);
    RUN(test_analysis_defaults_fill_missing_fields);
    RUN(test_exhausted_slice_id_survives_serialization);
    RUN(test_non_finite_floats_write_null_and_null_is_rejected);
    RUN(test_f32_text_and_value_forms);
    RUN(test_hostile_integers);
    RUN(test_shape_errors);
    RUN(test_structs_from_sequences);
    RUN(test_value_fields_and_deep_nesting);
    RUN(test_null_arguments);
    RUN(test_corrupt_values_write);
    return pc_t_finish();
}
