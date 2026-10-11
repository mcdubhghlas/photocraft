/* The save-time finite check (crates/format/src/finite.rs tests). The reference walks a
 * Serialize impl; here the same values are walked as the JSON tree serde_json produces for them
 * (externally tagged enums, Option as the value or null, BTreeMap as an object). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "format/format.h"
#include "harness.h"

typedef struct outer {
    float dpi;
    float gamma;
    double offsets[2];
    int exposure_layer; /* layers[1] is Exposure { exposure } */
    float exposure;
    double levels[2]; /* layers[0] is Levels(a, b) */
    float blur;       /* named["blur"] */
} outer;

static outer sample(void)
{
    outer o = {72.0f, 1.0f, {0.0, 1.5}, 1, 0.5f, {0.0, 1.0}, 2.0f};
    return o;
}

/* {"dpi":..,"inner":{"gamma":..,"offsets":[..]},"layers":[{"Levels":[a,b]},{"Exposure":{"exposure":..}}],
 *  "named":{"blur":..}} */
static yyjson_mut_val *tree(yyjson_mut_doc *d, const outer *o)
{
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *inner = yyjson_mut_obj(d);
    yyjson_mut_val *offsets = yyjson_mut_arr(d);
    yyjson_mut_val *layers = yyjson_mut_arr(d);
    yyjson_mut_val *lv = yyjson_mut_obj(d);
    yyjson_mut_val *lva = yyjson_mut_arr(d);
    yyjson_mut_val *ex = yyjson_mut_obj(d);
    yyjson_mut_val *exi = yyjson_mut_obj(d);
    yyjson_mut_val *named = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "dpi", yyjson_mut_float(d, o->dpi));
    yyjson_mut_obj_add_val(d, inner, "gamma", yyjson_mut_float(d, o->gamma));
    yyjson_mut_arr_add_real(d, offsets, o->offsets[0]);
    yyjson_mut_arr_add_real(d, offsets, o->offsets[1]);
    yyjson_mut_obj_add_val(d, inner, "offsets", offsets);
    yyjson_mut_obj_add_val(d, root, "inner", inner);
    yyjson_mut_arr_add_real(d, lva, o->levels[0]);
    yyjson_mut_arr_add_real(d, lva, o->levels[1]);
    yyjson_mut_obj_add_val(d, lv, "Levels", lva);
    yyjson_mut_obj_add_val(d, exi, "exposure", yyjson_mut_float(d, o->exposure));
    yyjson_mut_obj_add_val(d, ex, "Exposure", exi);
    yyjson_mut_arr_append(layers, lv);
    yyjson_mut_arr_append(layers, ex);
    yyjson_mut_obj_add_val(d, root, "layers", layers);
    yyjson_mut_obj_add_val(d, named, "blur", yyjson_mut_float(d, o->blur));
    yyjson_mut_obj_add_val(d, root, "named", named);
    return root;
}

static char g_path[1024];

static const char *path_of(const outer *o)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    pc_buf p = PC_BUF_INIT;
    pc_error e;
    pc_status s = pc_format_check_finite(tree(d, o), &p, &e);
    g_path[0] = '\0';
    if (s != PC_ERR_INVALID) {
        PC_T_FAIL("expected a non-finite report");
    } else {
        (void)snprintf(g_path, sizeof g_path, "%s", pc_buf_cstr(&p));
        CHECK(pc_format_error_kind(s, &e) == PC_FORMAT_ERR_NON_FINITE);
    }
    pc_buf_fini(&p);
    yyjson_mut_doc_free(d);
    return g_path;
}

static void test_finite_values_pass_and_each_position_is_named(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    outer o = sample();
    CHECK(pc_format_check_finite(tree(d, &o), NULL, NULL) == PC_OK);
    yyjson_mut_doc_free(d);
    o.dpi = NAN;
    CHECK_EQ_STR(path_of(&o), "dpi");
    o = sample();
    o.offsets[1] = INFINITY;
    CHECK_EQ_STR(path_of(&o), "inner.offsets[1]");
    o = sample();
    o.exposure = -INFINITY;
    CHECK_EQ_STR(path_of(&o), "layers[1].Exposure.exposure");
    o = sample();
    o.levels[1] = NAN;
    CHECK_EQ_STR(path_of(&o), "layers[0].Levels[1]");
    o = sample();
    o.blur = NAN;
    CHECK_EQ_STR(path_of(&o), "named.blur");
    /* The first one in serialisation order wins. */
    o = sample();
    o.blur = NAN;
    o.gamma = NAN;
    CHECK_EQ_STR(path_of(&o), "inner.gamma");
    /* A bare non-finite value has the empty path. */
    d = yyjson_mut_doc_new(NULL);
    {
        pc_buf p = PC_BUF_INIT;
        pc_error e;
        CHECK(pc_format_check_finite(yyjson_mut_real(d, NAN), &p, &e) == PC_ERR_INVALID);
        CHECK_EQ_STR(pc_buf_cstr(&p), "");
        CHECK_EQ_STR(
            e.msg,
            "`` is NaN or infinite, which a .pcraft manifest cannot hold; reset that value and save again");
        pc_buf_fini(&p);
    }
    yyjson_mut_doc_free(d);
}

static void test_message_and_key_spelling(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *m = yyjson_mut_obj(d);
    pc_buf p = PC_BUF_INIT;
    pc_error e;
    /* serde_json::to_string(key) with the quotes trimmed: escapes stay, a trailing escaped quote
     * loses its quote too. */
    yyjson_mut_obj_add_val(d, m, "ok", yyjson_mut_real(d, 1.0));
    yyjson_mut_obj_add_val(d, m, "a\"b\\c\n\x01x\"", yyjson_mut_real(d, INFINITY));
    yyjson_mut_obj_add_val(d, root, "map", m);
    CHECK(pc_format_check_finite(root, &p, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(pc_buf_cstr(&p), "map.a\\\"b\\\\c\\n\\u0001x\\");
    CHECK_EQ_STR(e.msg,
                 "`map.a\\\"b\\\\c\\n\\u0001x\\` is NaN or infinite, which a .pcraft manifest cannot hold; "
                 "reset that value and save again");
    /* Integers, strings, bools, nulls and empty containers are never a problem. */
    root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_int(d, root, "i", -3);
    yyjson_mut_obj_add_uint(d, root, "u", 3);
    yyjson_mut_obj_add_str(d, root, "s", "NaN");
    yyjson_mut_obj_add_null(d, root, "n");
    yyjson_mut_obj_add_val(d, root, "a", yyjson_mut_arr(d));
    yyjson_mut_obj_add_val(d, root, "o", yyjson_mut_obj(d));
    CHECK(pc_format_check_finite(root, NULL, NULL) == PC_OK);
    CHECK(pc_format_check_finite(NULL, NULL, NULL) == PC_OK);
    /* A leading-dot key at the top level loses its dots like the reference's trim. */
    root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "..x", yyjson_mut_real(d, NAN));
    CHECK(pc_format_check_finite(root, &p, NULL) == PC_ERR_INVALID);
    CHECK_EQ_STR(pc_buf_cstr(&p), "x");
    pc_buf_fini(&p);
    yyjson_mut_doc_free(d);
}

/* A very deep tree is walked without recursion. */
static void test_deep_trees(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_arr(d);
    yyjson_mut_val *cur = root;
    pc_buf p = PC_BUF_INIT;
    for (int i = 0; i < 200000; i++) {
        yyjson_mut_val *next = yyjson_mut_arr(d);
        yyjson_mut_arr_append(cur, yyjson_mut_int(d, i));
        yyjson_mut_arr_append(cur, next);
        cur = next;
    }
    CHECK(pc_format_check_finite(root, NULL, NULL) == PC_OK);
    yyjson_mut_arr_append(cur, yyjson_mut_real(d, NAN));
    CHECK(pc_format_check_finite(root, &p, NULL) == PC_ERR_INVALID);
    CHECK_EQ_UINT(p.len, 200000 * 3 + 3); /* "[1]" per level, then "[0]" */
    pc_buf_fini(&p);
    yyjson_mut_doc_free(d);
}

int main(int argc, char **argv)
{
    RUN(test_finite_values_pass_and_each_position_is_named);
    RUN(test_message_and_key_spelling);
    RUN(test_deep_trees);
    return pc_t_finish();
}
