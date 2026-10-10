/* Named type styles (doc/text_styles.h): ports of the reference's text_styles.rs tests plus
 * attribute-set handling, serde error texts and the unwrap_or fallbacks. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "doc/json.h"
#include "harness.h"

static pc_style_attrs attrs(const char *json)
{
    pc_style_attrs a = PC_JSON_VALUE_INIT;
    yyjson_doc *d = NULL;
    if (pc_json_parse(json, strlen(json), &d, NULL) != PC_OK ||
        pc_json_value_from_val(yyjson_doc_get_root(d), &a, NULL) != PC_OK) {
        abort(); /* test-only */
    }
    yyjson_doc_free(d);
    return a;
}

static char *attrs_text(const pc_style_attrs *a)
{
    pc_buf b = PC_BUF_INIT;
    char *s;
    yyjson_mut_val *r = pc_json_value_root(a);
    if (r == NULL) {
        s = malloc(3);
        memcpy(s, "{}", 3);
        return s;
    }
    if (pc_json_write(r, 0, &b, NULL) != PC_OK) {
        abort();
    }
    s = malloc(b.len + 1);
    memcpy(s, b.data, b.len);
    s[b.len] = '\0';
    pc_buf_fini(&b);
    return s;
}

static bool attrs_is(const pc_style_attrs *a, const char *json)
{
    char *s = attrs_text(a);
    bool ok = strcmp(s, json) == 0;
    if (!ok) {
        PC_T_FAIL("attrs %s, want %s", s, json);
    }
    free(s);
    return ok;
}

static pc_str S(const char *s)
{
    pc_str t = PC_STR_INIT;
    if (pc_str_set_c(&t, s, NULL) != PC_OK) {
        abort();
    }
    return t;
}

/* The reference tests' styles(): "Big" (40 pt, underline) and paragraph "Head". */
static pc_text_styles styles(void)
{
    pc_text_styles s;
    if (pc_text_styles_default(&s, NULL) != PC_OK) {
        abort();
    }
    s.character = calloc(1, sizeof *s.character);
    s.ncharacter = 1;
    s.character[0].id = 1;
    s.character[0].name = S("Big");
    s.character[0].attrs = attrs("{\"size_pt\": 40.0, \"underline\": true}");
    s.paragraph = calloc(1, sizeof *s.paragraph);
    s.nparagraph = 1;
    s.paragraph[0].id = 1;
    s.paragraph[0].name = S("Head");
    s.paragraph[0].para_attrs = attrs("{\"align\": \"Center\"}");
    s.paragraph[0].char_attrs = attrs("{\"size_pt\": 20.0, \"weight\": 700}");
    return s;
}

static const uint32_t ONE = 1;
static const uint32_t NINE = 9;

static void test_resolution_layers_styles(void)
{
    pc_text_styles s = styles();
    pc_char_style c;
    pc_paragraph_style p;
    CHECK_EQ_INT(pc_text_styles_resolve_char(&s, NULL, NULL, "Inter", &c, NULL), PC_OK);
    CHECK(pc_str_eq_c(&c.font_family, "Inter") && c.size_pt == 12.0f && !c.has_style_sheet);
    pc_char_style_fini(&c);
    CHECK_EQ_INT(pc_text_styles_resolve_char(&s, &ONE, NULL, "Inter", &c, NULL), PC_OK);
    CHECK(c.size_pt == 20.0f && c.weight == 700);
    pc_char_style_fini(&c);
    CHECK_EQ_INT(pc_text_styles_resolve_char(&s, &ONE, &ONE, "Inter", &c, NULL), PC_OK);
    CHECK(c.size_pt == 40.0f && c.weight == 700 && c.underline && c.has_style_sheet && c.style_sheet == 1);
    pc_char_style_fini(&c);
    /* Missing ids fall back. */
    CHECK_EQ_INT(pc_text_styles_resolve_char(&s, &NINE, &NINE, "Inter", &c, NULL), PC_OK);
    CHECK(!c.has_style_sheet && c.size_pt == 12.0f);
    pc_char_style_fini(&c);
    CHECK_EQ_INT(pc_text_styles_resolve_para(&s, &ONE, &p, NULL), PC_OK);
    CHECK(p.align == PC_TEXT_ALIGN_CENTER && p.has_style_sheet && p.style_sheet == 1);
    CHECK_EQ_INT(pc_text_styles_resolve_para(&s, NULL, &p, NULL), PC_OK);
    CHECK(p.align == PC_TEXT_ALIGN_LEFT && !p.has_style_sheet);
    pc_text_styles_fini(&s);
}

static void test_overrides_survive_restyling(void)
{
    pc_text_styles old = styles();
    pc_text_styles nw;
    pc_char_style run;
    pc_char_style r;
    pc_style_attrs ov = PC_JSON_VALUE_INIT;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK_EQ_INT(pc_text_styles_resolve_char(&old, NULL, &ONE, "Inter", &run, NULL), PC_OK);
    CHECK_EQ_INT(pc_text_styles_char_overrides(&old, &run, NULL, "Inter", &ov, NULL), PC_OK);
    CHECK(pc_style_attrs_is_empty(&ov));
    pc_json_value_fini(&ov);
    run.italic = true;
    CHECK_EQ_INT(pc_text_styles_char_overrides(&old, &run, NULL, "Inter", &ov, NULL), PC_OK);
    CHECK(attrs_is(&ov, "{\"italic\":true}"));
    pc_json_value_fini(&ov);
    /* Redefine "Big" to 50 pt: the run follows and keeps its italic override. */
    CHECK_EQ_INT(pc_text_styles_clone(&old, &nw, NULL), PC_OK);
    CHECK(pc_text_styles_eq(&old, &nw));
    CHECK_EQ_INT(pc_style_attrs_insert(&pc_text_styles_char_style_mut(&nw, 1)->attrs, "size_pt",
                                       yyjson_mut_real(d, 50.0), NULL),
                 PC_OK);
    CHECK(!pc_text_styles_eq(&old, &nw));
    CHECK_EQ_INT(pc_text_styles_restyle_char(&nw, &old, &run, NULL, NULL, &ONE, "Inter", &r, NULL), PC_OK);
    CHECK(r.size_pt == 50.0f && r.italic && r.underline);
    pc_char_style_fini(&r);
    /* Clearing the character style keeps overrides on the paragraph basis. */
    CHECK_EQ_INT(pc_text_styles_restyle_char(&nw, &old, &run, NULL, NULL, NULL, "Inter", &r, NULL), PC_OK);
    CHECK(r.size_pt == 12.0f && r.italic && !r.underline && !r.has_style_sheet);
    pc_char_style_fini(&r);
    pc_char_style_fini(&run);
    pc_text_styles_fini(&old);
    pc_text_styles_fini(&nw);
    yyjson_mut_doc_free(d);
}

static void test_attrs_validate_and_diff(void)
{
    pc_style_attrs a = attrs("{\"size_pt\": 3.0}");
    pc_style_attrs b = attrs("{\"size_pt\": \"big\"}");
    pc_style_attrs c = attrs("{\"nope\": 1}");
    pc_style_attrs d = PC_JSON_VALUE_INIT;
    pc_style_attrs all = PC_JSON_VALUE_INIT;
    pc_char_style x;
    pc_char_style def;
    pc_error err;
    CHECK_EQ_INT(pc_char_style_validate_attrs(&a, NULL), PC_OK);
    CHECK_EQ_INT(pc_char_style_validate_attrs(&b, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "invalid type: string \"big\", expected f32");
    CHECK_EQ_INT(pc_char_style_validate_attrs(&c, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "unknown style attribute \"nope\"");
    pc_char_style_default(&x);
    pc_char_style_default(&def);
    x.size_pt = 30.0f;
    x.has_style_sheet = true;
    x.style_sheet = 3;
    CHECK_EQ_INT(pc_char_style_diff_attrs(&x, &def, &d, NULL), PC_OK);
    CHECK(attrs_is(&d, "{\"size_pt\":30.0}"));
    CHECK_EQ_INT(pc_char_style_all_attrs(&x, &all, NULL), PC_OK);
    CHECK(pc_style_attrs_get(&all, "style_sheet") == NULL);
    CHECK(pc_style_attrs_get(&all, "size_pt") != NULL);
    CHECK_EQ_UINT(pc_style_attrs_len(&all), 24);
    pc_json_value_fini(&a);
    pc_json_value_fini(&b);
    pc_json_value_fini(&c);
    pc_json_value_fini(&d);
    pc_json_value_fini(&all);
}

static pc_status styles_from(const char *json, pc_text_styles *out)
{
    yyjson_doc *doc = NULL;
    pc_status st = pc_json_parse(json, strlen(json), &doc, NULL);
    if (st == PC_OK) {
        st = pc_text_styles_from_json(yyjson_doc_get_root(doc), out, NULL);
    }
    yyjson_doc_free(doc);
    return st;
}

static pc_status styles_round_trip(const pc_text_styles *s, pc_text_styles *out)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_doc *imut;
    pc_status st;
    yyjson_mut_doc_set_root(d, pc_text_styles_to_json(d, s, PC_DOC_JSON_VALUE));
    imut = yyjson_mut_doc_imut_copy(d, NULL);
    st = pc_text_styles_from_json(yyjson_doc_get_root(imut), out, NULL);
    yyjson_doc_free(imut);
    yyjson_mut_doc_free(d);
    return st;
}

static void test_names_and_ids(void)
{
    pc_text_styles s = styles();
    pc_text_styles back;
    pc_str n = PC_STR_INIT;
    uint32_t id = 0;
    CHECK_EQ_INT(pc_text_styles_unique_name(&s, false, "Character Style", &n, NULL), PC_OK);
    CHECK(pc_str_eq_c(&n, "Character Style 1"));
    pc_str_fini(&n);
    CHECK(pc_text_styles_next_char_id(&s, &id) && id == 2);
    CHECK(pc_text_styles_next_para_id(&s, &id) && id == 2);
    CHECK(pc_str_eq_c(&pc_text_styles_para_style(&s, 0)->name, PC_BASIC_PARAGRAPH));
    CHECK_EQ_INT(styles_round_trip(&s, &back), PC_OK);
    CHECK(pc_text_styles_eq(&back, &s));
    pc_text_styles_fini(&back);
    CHECK_EQ_INT(styles_from("{}", &back), PC_OK);
    CHECK(pc_str_eq_c(&back.basic.name, PC_BASIC_PARAGRAPH));
    CHECK(pc_text_styles_is_empty(&back));
    pc_text_styles_fini(&back);
    pc_text_styles_fini(&s);
}

static void test_exhausted_style_ids_survive_serialization(void)
{
    pc_text_styles s;
    pc_text_styles loaded;
    uint32_t id = 0;
    CHECK_EQ_INT(pc_text_styles_default(&s, NULL), PC_OK);
    s.character = calloc(1, sizeof *s.character);
    s.ncharacter = 1;
    s.character[0].id = UINT32_MAX;
    s.paragraph = calloc(1, sizeof *s.paragraph);
    s.nparagraph = 1;
    s.paragraph[0].id = UINT32_MAX;
    CHECK_EQ_INT(styles_round_trip(&s, &loaded), PC_OK);
    CHECK(!pc_text_styles_next_char_id(&loaded, &id));
    CHECK(!pc_text_styles_next_para_id(&loaded, &id));
    pc_text_styles_fini(&loaded);
    pc_text_styles_fini(&s);
}

/* ---- beyond the reference's tests ---- */

static void test_attribute_sets(void)
{
    pc_style_attrs a = PC_JSON_VALUE_INIT;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(pc_style_attrs_is_empty(&a));
    CHECK_EQ_UINT(pc_style_attrs_len(&a), 0);
    CHECK(pc_style_attrs_get(&a, "x") == NULL);
    CHECK_EQ_INT(pc_style_attrs_insert(&a, "weight", yyjson_mut_uint(d, 700), NULL), PC_OK);
    CHECK_EQ_INT(pc_style_attrs_insert(&a, "align", yyjson_mut_str(d, "Center"), NULL), PC_OK);
    CHECK_EQ_INT(pc_style_attrs_insert(&a, "size_pt", yyjson_mut_float(d, 0.1f), NULL), PC_OK);
    CHECK_EQ_INT(pc_style_attrs_insert(&a, "weight", NULL, NULL), PC_OK);
    /* Sorted keys, f32 widened (Value semantics), replaced key, NULL = null. */
    CHECK(attrs_is(&a, "{\"align\":\"Center\",\"size_pt\":0.10000000149011612,\"weight\":null}"));
    CHECK_EQ_UINT(pc_style_attrs_len(&a), 3);
    CHECK(yyjson_mut_is_null(pc_style_attrs_get(&a, "weight")));
    CHECK_EQ_INT(pc_style_attrs_insert(NULL, "x", NULL, NULL), PC_ERR_INVALID);
    pc_json_value_fini(&a);
    yyjson_mut_doc_free(d);
}

static void test_apply_errors_are_serde_messages(void)
{
    pc_char_style base;
    pc_char_style out;
    pc_paragraph_style p = pc_paragraph_style_default();
    pc_paragraph_style po;
    pc_style_attrs a;
    pc_error err;
    pc_char_style_default(&base);
    /* Unknown keys are reported first (in key order), then type errors in key order. */
    a = attrs("{\"zz\":1, \"aa\":2, \"size_pt\":\"x\"}");
    CHECK_EQ_INT(pc_char_style_apply_attrs(&base, &a, &out, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "unknown style attribute \"aa\"");
    pc_json_value_fini(&a);
    a = attrs("{\"weight\":1.5, \"size_pt\":\"x\"}");
    CHECK_EQ_INT(pc_char_style_apply_attrs(&base, &a, &out, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "invalid type: string \"x\", expected f32");
    pc_json_value_fini(&a);
    a = attrs("{\"\\u0001\\\"q\":1}");
    CHECK_EQ_INT(pc_char_style_apply_attrs(&base, &a, &out, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "unknown style attribute \"\\u{1}\\\"q\"");
    pc_json_value_fini(&a);
    /* The style reference is never an attribute; the base's survives. */
    a = attrs("{\"style_sheet\":\"junk\",\"align\":\"Right\"}");
    p.has_style_sheet = true;
    p.style_sheet = 7;
    CHECK_EQ_INT(pc_paragraph_style_apply_attrs(&p, &a, &po, NULL), PC_OK);
    CHECK(po.align == PC_TEXT_ALIGN_RIGHT && po.has_style_sheet && po.style_sheet == 7);
    pc_json_value_fini(&a);
    a = attrs("{\"align\":\"center\"}");
    CHECK_EQ_INT(pc_paragraph_style_validate_attrs(&a, &err), PC_ERR_INVALID);
    CHECK_CONTAINS(err.msg, "unknown variant `center`, expected one of `Left`, `Center`");
    pc_json_value_fini(&a);
    /* A NaN in the base becomes null in its Value, which doesn't read back as f32. */
    base.size_pt = NAN;
    CHECK_EQ_INT(pc_char_style_apply_attrs(&base, NULL, &out, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "invalid type: null, expected f32");
    a = attrs("{\"size_pt\":9}");
    CHECK_EQ_INT(pc_char_style_apply_attrs(&base, &a, &out, NULL), PC_OK);
    CHECK(out.size_pt == 9.0f);
    pc_char_style_fini(&out);
    pc_json_value_fini(&a);
    pc_char_style_fini(&base);
}

/* A style whose attributes don't apply is skipped (the reference's unwrap_or). */
static void test_bad_styles_fall_back(void)
{
    pc_text_styles s = styles();
    pc_char_style c;
    pc_paragraph_style p;
    pc_json_value_fini(&s.character[0].attrs);
    s.character[0].attrs = attrs("{\"size_pt\":\"huge\"}");
    pc_json_value_fini(&s.paragraph[0].para_attrs);
    s.paragraph[0].para_attrs = attrs("{\"nope\":1}");
    CHECK_EQ_INT(pc_text_styles_resolve_char(&s, &ONE, &ONE, "Inter", &c, NULL), PC_OK);
    CHECK(c.size_pt == 20.0f && c.weight == 700 && c.has_style_sheet && c.style_sheet == 1);
    pc_char_style_fini(&c);
    CHECK_EQ_INT(pc_text_styles_resolve_para(&s, &ONE, &p, NULL), PC_OK);
    CHECK(p.align == PC_TEXT_ALIGN_LEFT && p.has_style_sheet);
    pc_text_styles_fini(&s);
}

static void test_restyle_para_and_basic(void)
{
    pc_text_styles old = styles();
    pc_text_styles nw;
    pc_paragraph_style p;
    pc_paragraph_style r;
    pc_style_attrs ov = PC_JSON_VALUE_INIT;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK_EQ_INT(pc_text_styles_resolve_para(&old, &ONE, &p, NULL), PC_OK);
    p.hyphenate = true;
    CHECK_EQ_INT(pc_text_styles_para_overrides(&old, &p, &ov, NULL), PC_OK);
    CHECK(attrs_is(&ov, "{\"hyphenate\":true}"));
    pc_json_value_fini(&ov);
    CHECK_EQ_INT(pc_text_styles_clone(&old, &nw, NULL), PC_OK);
    CHECK_EQ_INT(pc_style_attrs_insert(&nw.basic.para_attrs, "space_after_pt", yyjson_mut_real(d, 6.0), NULL),
                 PC_OK);
    CHECK(!pc_text_styles_is_empty(&nw));
    /* Move the paragraph to Basic Paragraph: it gains Basic's spacing and keeps hyphenation. */
    CHECK_EQ_INT(pc_text_styles_restyle_para(&nw, &old, &p, NULL, &r, NULL), PC_OK);
    CHECK(r.align == PC_TEXT_ALIGN_LEFT && r.hyphenate && r.space_after_pt == 6.0f && !r.has_style_sheet);
    CHECK_EQ_INT(pc_text_styles_restyle_para(&nw, &old, &p, &NINE, &r, NULL), PC_OK);
    CHECK(!r.has_style_sheet);
    CHECK_EQ_INT(pc_text_styles_restyle_para(&nw, &old, &p, &ONE, &r, NULL), PC_OK);
    CHECK(r.align == PC_TEXT_ALIGN_CENTER && r.hyphenate && r.has_style_sheet && r.style_sheet == 1);
    pc_text_styles_fini(&old);
    pc_text_styles_fini(&nw);
    yyjson_mut_doc_free(d);
}

static void test_lookups_and_names(void)
{
    pc_text_styles s = styles();
    pc_str n = PC_STR_INIT;
    uint32_t id = 0;
    CHECK(pc_text_styles_char_style(&s, 1) == &s.character[0]);
    CHECK(pc_text_styles_char_style(&s, 0) == NULL);
    CHECK(pc_text_styles_para_style_mut(&s, 0) == &s.basic);
    CHECK(pc_text_styles_para_style(&s, 2) == NULL);
    pc_str_fini(&s.paragraph[0].name);
    s.paragraph[0].name = S("Paragraph Style 1");
    CHECK_EQ_INT(pc_text_styles_unique_name(&s, true, "Paragraph Style", &n, NULL), PC_OK);
    CHECK(pc_str_eq_c(&n, "Paragraph Style 2"));
    pc_str_fini(&n);
    CHECK_EQ_INT(pc_text_styles_unique_name(&s, true, "Basic", &n, NULL), PC_OK);
    CHECK(pc_str_eq_c(&n, "Basic 1"));
    pc_str_fini(&n);
    pc_text_styles_fini(&s);
    CHECK_EQ_INT(pc_text_styles_default(&s, NULL), PC_OK);
    CHECK(pc_text_styles_is_empty(&s));
    CHECK(pc_text_styles_next_char_id(&s, &id) && id == 1);
    pc_text_styles_fini(&s);
    pc_text_styles_fini(&s); /* idempotent */
}

static void test_styles_json_defaults_and_errors(void)
{
    pc_text_styles s;
    pc_error err;
    yyjson_doc *doc = NULL;
    CHECK_EQ_INT(styles_from("{\"basic\":{}}", &s), PC_OK);
    CHECK(s.basic.name.len == 0); /* a present Basic Paragraph takes ParagraphStyleDef's defaults */
    pc_text_styles_fini(&s);
    CHECK_EQ_INT(styles_from("{\"psd_raw\":{\"b\":1,\"a\":[null]},\"character\":[{\"attrs\":{}}]}", &s),
                 PC_OK);
    CHECK(s.ncharacter == 1 && pc_style_attrs_is_empty(&s.character[0].attrs));
    CHECK(pc_json_value_root(&s.psd_raw) != NULL);
    pc_text_styles_fini(&s);
    {
        static const char text[] = "{\"character\":[{\"attrs\":[]}]}";
        CHECK_EQ_INT(pc_json_parse(text, sizeof text - 1, &doc, NULL), PC_OK);
    }
    CHECK_EQ_INT(pc_text_styles_from_json(yyjson_doc_get_root(doc), &s, &err), PC_ERR_INVALID);
    CHECK_EQ_STR(err.msg, "invalid type: sequence, expected a map");
    yyjson_doc_free(doc);
}

int main(int argc, char **argv)
{
    RUN(test_resolution_layers_styles);
    RUN(test_overrides_survive_restyling);
    RUN(test_attrs_validate_and_diff);
    RUN(test_names_and_ids);
    RUN(test_exhausted_style_ids_survive_serialization);
    RUN(test_attribute_sets);
    RUN(test_apply_errors_are_serde_messages);
    RUN(test_bad_styles_fall_back);
    RUN(test_restyle_para_and_basic);
    RUN(test_lookups_and_names);
    RUN(test_styles_json_defaults_and_errors);
    return pc_t_finish();
}
