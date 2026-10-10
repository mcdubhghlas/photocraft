/* pc_str, pc_blob, pc_json_value (serde_json::Value semantics) and pc_unicode_is_alphanumeric. */
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/str.h"
#include "base/utf8.h"
#include "harness.h"

static void test_str_basics(void)
{
    pc_str a = PC_STR_INIT;
    pc_str b = PC_STR_INIT;
    CHECK_EQ_STR(pc_str_c(&a), "");
    CHECK(pc_str_eq(&a, &b));
    CHECK(pc_str_set(&a, "a\0b", 3, NULL) == PC_OK);
    CHECK_EQ_UINT(a.len, 3);
    CHECK(pc_str_eq_mem(&a, "a\0b", 3));
    CHECK(!pc_str_eq_c(&a, "a"));
    CHECK(pc_str_clone(&a, &b, NULL) == PC_OK);
    CHECK(pc_str_eq(&a, &b));
    CHECK(a.ptr != b.ptr);
    CHECK(pc_str_append(&b, "c", 1, NULL) == PC_OK);
    CHECK_EQ_UINT(b.len, 4);
    CHECK(pc_str_cmp(&a, &b) < 0);
    CHECK(pc_str_cmp(&b, &a) > 0);
    CHECK(pc_str_set(&a, NULL, 0, NULL) == PC_OK);
    CHECK(a.ptr == NULL && a.len == 0);
    CHECK(pc_str_set_c(&a, "xyz", NULL) == PC_OK);
    CHECK(pc_str_eq_c(&a, "xyz"));
    pc_str_fini(&a);
    pc_str_fini(&b);
    pc_str_fini(&b);
    pc_str_fini(NULL);
}

static void test_blob_sharing(void)
{
    pc_blob *a = NULL;
    pc_blob *b = NULL;
    pc_blob *e = NULL;
    CHECK(pc_blob_new("abc", 3, &a, NULL) == PC_OK);
    CHECK(pc_blob_new("abc", 3, &b, NULL) == PC_OK);
    CHECK(pc_blob_new(NULL, 0, &e, NULL) == PC_OK);
    CHECK(pc_blob_eq(a, b));
    CHECK(!pc_blob_eq(a, e));
    CHECK(!pc_blob_eq(NULL, e)); /* None != Some(empty) */
    CHECK(pc_blob_eq(NULL, NULL));
    CHECK_EQ_UINT(pc_blob_len(e), 0);
    CHECK(pc_blob_retain(a) == a);
    pc_blob_release(a);
    CHECK_EQ_MEM(pc_blob_data(a), "abc", 3);
    CHECK(((uintptr_t)pc_blob_data(a) % sizeof(double)) == 0);
    pc_blob_release(a);
    pc_blob_release(b);
    pc_blob_release(e);
    pc_blob_release(NULL);
}

static pc_status value_from_text(const char *text, pc_json_value *out)
{
    yyjson_doc *d = NULL;
    pc_status st = pc_json_parse(text, strlen(text), &d, NULL);
    if (st == PC_OK) {
        st = pc_json_value_from_val(yyjson_doc_get_root(d), out, NULL);
        yyjson_doc_free(d);
    }
    return st;
}

static void check_written(const pc_json_value *v, const char *want)
{
    pc_buf b = PC_BUF_INIT;
    yyjson_mut_doc *tmp = NULL;
    yyjson_mut_val *root = pc_json_value_root(v);
    if (root == NULL) {
        tmp = yyjson_mut_doc_new(NULL);
        root = yyjson_mut_null(tmp);
    }
    CHECK(pc_json_write(root, 0, &b, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&b), want);
    pc_buf_fini(&b);
    yyjson_mut_doc_free(tmp);
}

static void test_value_normalises_like_serde(void)
{
    pc_json_value v = PC_JSON_VALUE_INIT;
    pc_json_value w = PC_JSON_VALUE_INIT;
    pc_json_value n = PC_JSON_VALUE_INIT;
    /* serde_json::Value: BTreeMap order, last duplicate wins, nested objects sorted too. */
    CHECK(value_from_text("{\"b\":1,\"a\":[2.5,{\"z\":null,\"y\":true}],\"b\":-3}", &v) == PC_OK);
    check_written(&v, "{\"a\":[2.5,{\"y\":true,\"z\":null}],\"b\":-3}");
    CHECK(value_from_text("{\"a\":[2.5,{\"y\":true,\"z\":null}],\"b\":-3}", &w) == PC_OK);
    CHECK(pc_json_value_eq(&v, &w));
    pc_json_value_fini(&w);
    CHECK(pc_json_value_clone(&v, &w, NULL) == PC_OK);
    CHECK(pc_json_value_eq(&v, &w));
    pc_json_value_fini(&w);
    /* 1 and 1.0 differ, as serde's Number does. */
    CHECK(value_from_text("1", &w) == PC_OK);
    CHECK(value_from_text("1.0", &n) == PC_OK);
    CHECK(!pc_json_value_eq(&w, &n));
    pc_json_value_fini(&w);
    pc_json_value_fini(&n);
    /* Null: a NULL doc and a parsed null are equal. */
    CHECK(value_from_text("null", &w) == PC_OK);
    CHECK(pc_json_value_eq(&w, &n));
    check_written(&n, "null");
    pc_json_value_fini(&w);
    pc_json_value_fini(&v);
    pc_json_value_fini(&v);
    /* serde_json parses the integer text -0 as the float -0.0. */
    CHECK(value_from_text("[-0,0,{\"a\":-0}]", &v) == PC_OK);
    check_written(&v, "[-0.0,0,{\"a\":-0.0}]");
    pc_json_value_fini(&v);
}

static void test_value_widens_f32(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    pc_json_value v = PC_JSON_VALUE_INIT;
    yyjson_mut_val *i = yyjson_mut_sint(d, 7);
    (void)yyjson_mut_obj_add_float(d, o, "f", 0.1f);
    (void)yyjson_mut_obj_add_val(d, o, "i", i);
    CHECK(pc_json_value_from_mut(o, &v, NULL) == PC_OK);
    /* to_value(0.1f32) is the f64 0.10000000149011612; a non-negative sint becomes unsigned. */
    check_written(&v, "{\"f\":0.10000000149011612,\"i\":7}");
    CHECK(yyjson_mut_is_uint(yyjson_mut_obj_get(pc_json_value_root(&v), "i")));
    pc_json_value_fini(&v);
    yyjson_mut_doc_free(d);
}

static void test_alphanumeric(void)
{
    CHECK(pc_unicode_is_alphanumeric('a'));
    CHECK(pc_unicode_is_alphanumeric('Z'));
    CHECK(pc_unicode_is_alphanumeric('7'));
    CHECK(!pc_unicode_is_alphanumeric('_'));
    CHECK(!pc_unicode_is_alphanumeric(' '));
    CHECK(!pc_unicode_is_alphanumeric('.'));
    CHECK(pc_unicode_is_alphanumeric(0xE9));   /* é */
    CHECK(pc_unicode_is_alphanumeric(0x65E5)); /* 日 */
    CHECK(pc_unicode_is_alphanumeric(0x0661)); /* Arabic-Indic one (Nd) */
    CHECK(pc_unicode_is_alphanumeric(0x2167)); /* Roman numeral eight (Nl) */
    CHECK(pc_unicode_is_alphanumeric(0x00B2)); /* superscript two (No) */
    CHECK(!pc_unicode_is_alphanumeric(0x1F600));
    CHECK(!pc_unicode_is_alphanumeric(0x110000));
}

int main(int argc, char **argv)
{
    RUN(test_str_basics);
    RUN(test_blob_sharing);
    RUN(test_value_normalises_like_serde);
    RUN(test_value_widens_f32);
    RUN(test_alphanumeric);
    return pc_t_finish();
}
