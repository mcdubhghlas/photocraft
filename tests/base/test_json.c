/* Expected text comes from serde_json 1.0.149 via tools/rustref (same FNV-1a and LCG). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/json.h"
#include "harness.h"

static void fnv(uint64_t *h, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= (unsigned char)b[i];
        *h *= 0x100000001b3u;
    }
}

static uint64_t lcg(uint64_t *s)
{
    *s = *s * 6364136223846793005u + 1442695040888963407u;
    return *s;
}

static void check_f64(double v, const char *want)
{
    char out[PC_JSON_NUM_MAX];
    size_t n = pc_json_format_f64(v, out);
    CHECK_EQ_STR(out, want);
    CHECK_EQ_UINT(n, strlen(want));
}

static void check_f32(float v, const char *want)
{
    char out[PC_JSON_NUM_MAX];
    (void)pc_json_format_f32(v, out);
    CHECK_EQ_STR(out, want);
}

static void test_float_format_samples(void)
{
    check_f64(0.0, "0.0");
    check_f64(-0.0, "-0.0");
    check_f64(1.0, "1.0");
    check_f64(0.1, "0.1");
    check_f64(1e20, "1e+20");
    check_f64(1e-7, "1e-7");
    check_f64(5e-324, "5e-324");
    check_f64(1e16, "1e+16");
    check_f64(1e15, "1000000000000000.0");
    check_f64(123.0, "123.0");
    check_f64(1.5e300, "1.5e+300");
    check_f64(0.000123, "0.000123");
    check_f64(1e-5, "0.00001");
    check_f64(1e-6, "1e-6");
    check_f64(1.2345e-6, "1.2345e-6");
    check_f64(9.999e15, "9999000000000000.0");
    check_f64(1.7976931348623157e308, "1.7976931348623157e+308");
    check_f64(3e-5, "0.00003");
    check_f64(1e100, "1e+100");
    check_f64(2.5e-100, "2.5e-100");
    check_f64(123456.789, "123456.789");
    check_f64(NAN, "null");
    check_f64(-INFINITY, "null");
    check_f32(0.1f, "0.1");
    check_f32(1e12f, "1000000000000.0");
    check_f32(1e13f, "1e+13");
    check_f32(1e-6f, "0.000001");
    check_f32(1e-7f, "1e-7");
    check_f32(3.4028235e38f, "3.4028235e+38");
    check_f32(1e-45f, "1e-45");
    check_f32(16777216.0f, "16777216.0");
    check_f32(0.3f, "0.3");
}

static void test_float_format_matches_serde_bulk(void)
{
    uint64_t e = 0xcbf29ce484222325u;
    uint64_t f = 0xcbf29ce484222325u;
    uint64_t s = 7;
    int n;
    for (n = 0; n < 200000; n++) {
        uint64_t r = lcg(&s);
        double x = pc_f64_from_bits(r);
        float y = pc_f32_from_bits((uint32_t)(r >> 32));
        char out[PC_JSON_NUM_MAX];
        size_t len;
        if (isfinite(x)) {
            len = pc_json_format_f64(x, out);
            fnv(&e, out, len);
            fnv(&e, "|", 1);
        }
        if (isfinite(y)) {
            len = pc_json_format_f32(y, out);
            fnv(&f, out, len);
            fnv(&f, "|", 1);
        }
    }
    CHECK_EQ_UINT(e, 0x0abffcde8a83a734u);
    CHECK_EQ_UINT(f, 0x3f9521546b4e5452u);
}

static const char SAMPLE[] = "{\"b\":1,\"a\":[1,2.5,{\"z\":null,\"y\":true}],\"a\":\"dup\","
                             "\"s\":\"q\\\"\\\\\\u0001\\u007f\\n\\t/\xc3\xa9\",\"e\":{},\"l\":[],\"n\":-12,"
                             "\"u\":18446744073709551615}";

static void test_value_serialisation_matches_serde(void)
{
    yyjson_doc *doc = NULL;
    pc_buf out = PC_BUF_INIT;
    pc_error err;
    CHECK_EQ_INT(pc_json_parse(SAMPLE, sizeof SAMPLE - 1, &doc, &err), PC_OK);
    CHECK_EQ_INT(pc_json_write_val(yyjson_doc_get_root(doc), PC_JSON_SORT_KEYS, &out, &err), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "{\"a\":\"dup\",\"b\":1,\"e\":{},\"l\":[],\"n\":-12,"
                                    "\"s\":\"q\\\"\\\\\\u0001\x7f\\n\\t/\xc3\xa9\",\"u\":18446744073709551615}");
    pc_buf_truncate(&out, 0);
    CHECK_EQ_INT(pc_json_write_val(yyjson_doc_get_root(doc), PC_JSON_SORT_KEYS | PC_JSON_PRETTY, &out, &err),
                 PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "{\n  \"a\": \"dup\",\n  \"b\": 1,\n  \"e\": {},\n  \"l\": [],\n  \"n\": -12,\n"
                                    "  \"s\": \"q\\\"\\\\\\u0001\x7f\\n\\t/\xc3\xa9\",\n"
                                    "  \"u\": 18446744073709551615\n}");
    pc_buf_fini(&out);
    yyjson_doc_free(doc);
}

static void test_struct_order_and_f32(void)
{
    /* Without SORT_KEYS, insertion order is kept (serde's derived struct serialisation). */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    pc_buf out = PC_BUF_INIT;
    yyjson_mut_doc_set_root(d, o);
    yyjson_mut_obj_add_uint(d, o, "zeta", 1);
    yyjson_mut_obj_add_float(d, o, "opacity", 0.1f);
    yyjson_mut_obj_add_real(d, o, "scale", 0.1f); /* an f32 widened to f64 shows its error */
    yyjson_mut_obj_add_val(d, o, "pts", a);
    yyjson_mut_arr_add_real(d, a, 2.0);
    yyjson_mut_arr_add_sint(d, a, -3);
    CHECK_EQ_INT(pc_json_write(o, 0, &out, NULL), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "{\"zeta\":1,\"opacity\":0.1,\"scale\":0.10000000149011612,\"pts\":[2.0,-3]}");
    pc_buf_truncate(&out, 0);
    CHECK_EQ_INT(pc_json_write(a, PC_JSON_PRETTY, &out, NULL), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "[\n  2.0,\n  -3\n]");
    pc_buf_fini(&out);
    yyjson_mut_doc_free(d);
}

static void test_accessors(void)
{
    static const char t[] = "{\"u\":5,\"big\":18446744073709551615,\"neg\":-7,\"f\":2.5,\"b\":true,"
                            "\"s\":\"hi\",\"k\":1,\"k\":2,\"huge\":18446744073709551616}";
    yyjson_doc *doc = NULL;
    yyjson_val *root;
    uint64_t u = 0;
    int64_t i = 0;
    double f = 0;
    bool b = false;
    size_t len = 0;
    CHECK_EQ_INT(pc_json_parse(t, sizeof t - 1, &doc, NULL), PC_OK);
    root = yyjson_doc_get_root(doc);
    CHECK(pc_json_as_u64(pc_json_get(root, "u"), &u) && u == 5);
    CHECK(pc_json_as_u64(pc_json_get(root, "big"), &u) && u == UINT64_MAX);
    CHECK(!pc_json_as_i64(pc_json_get(root, "big"), &i));
    CHECK(pc_json_as_i64(pc_json_get(root, "neg"), &i) && i == -7);
    CHECK(!pc_json_as_u64(pc_json_get(root, "neg"), &u));
    CHECK(!pc_json_as_u64(pc_json_get(root, "f"), &u)); /* floats are not u64, as in serde */
    CHECK(pc_json_as_f64(pc_json_get(root, "f"), &f) && f == 2.5);
    CHECK(pc_json_as_f64(pc_json_get(root, "neg"), &f) && f == -7.0);
    CHECK(pc_json_as_f64(pc_json_get(root, "huge"), &f) && f == 18446744073709551616.0);
    CHECK(!pc_json_as_u64(pc_json_get(root, "huge"), &u));
    CHECK(pc_json_as_bool(pc_json_get(root, "b"), &b) && b);
    CHECK_EQ_STR(pc_json_as_str(pc_json_get(root, "s"), &len), "hi");
    CHECK_EQ_UINT(len, 2);
    CHECK(pc_json_as_u64(pc_json_get(root, "k"), &u) && u == 2); /* last duplicate wins */
    CHECK(pc_json_get(root, "missing") == NULL);
    CHECK(pc_json_get(pc_json_get(root, "u"), "x") == NULL); /* not an object */
    CHECK(!pc_json_as_u64(NULL, &u));
    CHECK(pc_json_as_str(NULL, NULL) == NULL);
    yyjson_doc_free(doc);
}

static void check_parse_err(const char *t, size_t len, const char *msg)
{
    yyjson_doc *doc = NULL;
    pc_error e;
    CHECK_EQ_INT(pc_json_parse(t, len, &doc, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, msg);
    CHECK(doc == NULL);
}

static void test_parse_errors(void)
{
    char deep[300];
    yyjson_doc *doc = NULL;
    /* Same text as serde_json: */
    check_parse_err("[1] x", 5, "trailing characters at line 1 column 5");
    check_parse_err("tru", 3, "EOF while parsing a value at line 1 column 3");
    check_parse_err("", 0, "EOF while parsing a value at line 1 column 0");
    check_parse_err("  \n ", 4, "EOF while parsing a value at line 2 column 1");
    /* Same kind as serde_json; serde words some of these more specifically (REWRITE_PLAN §7). */
    {
        pc_error e;
        CHECK_EQ_INT(pc_json_parse("\"\\ud800\"", 8, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("01", 2, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("[1e]", 4, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("\"a\x01\"", 4, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("[1,]", 4, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("{\"a\" 1}", 7, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("", 0, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse("\"\xff\"", 3, &doc, &e), PC_ERR_FORMAT); /* invalid UTF-8 */
        CHECK_EQ_INT(pc_json_parse("[1] // c", 8, &doc, &e), PC_ERR_FORMAT);
        CHECK_EQ_INT(pc_json_parse(NULL, 5, &doc, &e), PC_ERR_INVALID);
    }
    /* Depth: 127 levels parse, the 128th is refused exactly where serde refuses it. */
    memset(deep, '[', 127);
    memset(deep + 127, ']', 127);
    CHECK_EQ_INT(pc_json_parse(deep, 254, &doc, NULL), PC_OK);
    yyjson_doc_free(doc);
    memset(deep, '[', 128);
    check_parse_err(deep, 128, "recursion limit exceeded at line 1 column 128");
    /* Brackets inside strings don't count. */
    memset(deep, '[', 200);
    deep[0] = '"';
    deep[199] = '"';
    CHECK_EQ_INT(pc_json_parse(deep, 200, &doc, NULL), PC_OK);
    yyjson_doc_free(doc);
}

static void test_write_rejects_runaway_depth(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_arr(d);
    yyjson_mut_val *cur = root;
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    int i;
    for (i = 0; i < 2000; i++) {
        yyjson_mut_val *next = yyjson_mut_arr(d);
        yyjson_mut_arr_append(cur, next);
        cur = next;
    }
    CHECK_EQ_INT(pc_buf_append_str(&out, "keep", NULL), PC_OK);
    CHECK_EQ_INT(pc_json_write(root, 0, &out, &e), PC_ERR_LIMIT);
    CHECK_EQ_STR(pc_buf_cstr(&out), "keep");
    CHECK_EQ_INT(pc_json_write(NULL, 0, &out, &e), PC_ERR_INVALID);
    pc_buf_fini(&out);
    yyjson_mut_doc_free(d);
}

int main(int argc, char **argv)
{
    RUN(test_float_format_samples);
    RUN(test_float_format_matches_serde_bulk);
    RUN(test_value_serialisation_matches_serde);
    RUN(test_struct_order_and_f32);
    RUN(test_accessors);
    RUN(test_parse_errors);
    RUN(test_write_rejects_runaway_depth);
    return pc_t_finish();
}
