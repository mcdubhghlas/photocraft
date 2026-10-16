/* PSD type layers and EngineData (src/text/engine_data.c, psd_*.c): the Rust tests of
 * crates/text/src/engine_data.rs and psd_styles.rs, the PSD tests of crates/text/src/tests.rs and
 * vertical_tests.rs, and never-crash tests for hostile input. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "doc/text_styles.h"
#include "harness.h"
#include "psd/descriptor.h"
#include "text/text.h"

static const uint8_t SAMPLE[] =
    "\n\n<<\n\t/EngineDict\n\t<<\n\t\t/Editor\n\t\t<<\n\t\t\t/Text (\xFE\xFF\x00H\x00i\x00\\)\x00\r)\n"
    "\t\t>>\n\t\t/Nums [ 1.0 .5 -2 0 ]\n\t\t/Flag true\n\t\t/Kind /Roman\n\t\t/Runs [\n\t\t<<\n"
    "\t\t\t/A 1\n\t\t>>\n\t\t]\n\t>>\n>>";
#define SAMPLE_LEN (sizeof SAMPLE - 1)

/* ---- helpers ---- */

static void set_text(pc_text_layer *t, const char *s)
{
    CHECK(pc_str_set_c(&t->text, s, NULL) == PC_OK);
}

/* styled(): text with one run of `style` (copied). */
static void styled(pc_text_layer *t, const char *text, const pc_char_style *style)
{
    pc_text_layer_default(t);
    set_text(t, text);
    t->runs = calloc(1, sizeof *t->runs);
    t->nruns = 1;
    t->runs[0].len = strlen(text);
    CHECK(pc_char_style_clone(style, &t->runs[0].style, NULL) == PC_OK);
}

static void build(const pc_text_layer *t, float dpi, const float *ink, pc_psd_bytes *out)
{
    CHECK(pc_text_build_tysh(t, dpi, ink, out, NULL) == PC_OK);
}

static bool import(const pc_psd_bytes *b, float dpi, pc_text_layer *out)
{
    return pc_text_layer_from_tysh(b->data, b->len, dpi, out, NULL) == PC_OK;
}

static const pc_ed_value *path3(const pc_ed_value *v, const char *a, const char *b, const char *c)
{
    return pc_ed_path(v, a, b, c, (const char *)NULL);
}

static bool str_is(const pc_ed_value *v, const char *s)
{
    const pc_str *x = pc_ed_as_str(v);
    return x != NULL && pc_str_eq_c(x, s);
}

/* ---- engine_data.rs ---- */

static void test_parses_sample(void)
{
    pc_ed_value v;
    const pc_ed_value *nums, *runs;
    size_t n = 0;
    double f;
    int64_t i;
    bool b;
    CHECK(pc_ed_parse(SAMPLE, SAMPLE_LEN, &v, NULL) == PC_OK);
    CHECK(str_is(path3(&v, "EngineDict", "Editor", "Text"), "Hi)\r"));
    nums = pc_ed_as_array(pc_ed_path(&v, "EngineDict", "Nums", (const char *)NULL), &n);
    CHECK(nums != NULL && n == 4);
    if (nums != NULL && n == 4) {
        CHECK(pc_ed_as_f64(&nums[0], &f) && f == 1.0);
        CHECK(pc_ed_as_f64(&nums[1], &f) && f == 0.5);
        CHECK(pc_ed_as_f64(&nums[2], &f) && f == -2.0);
        CHECK(pc_ed_as_f64(&nums[3], &f) && f == 0.0);
    }
    CHECK(pc_ed_as_bool(pc_ed_path(&v, "EngineDict", "Flag", (const char *)NULL), &b) && b);
    CHECK(pc_ed_path(&v, "EngineDict", "Kind", (const char *)NULL)->kind == PC_ED_NAME);
    CHECK(str_is(pc_ed_path(&v, "EngineDict", "Kind", (const char *)NULL), "Roman"));
    runs = pc_ed_as_array(pc_ed_path(&v, "EngineDict", "Runs", (const char *)NULL), &n);
    CHECK(runs != NULL && n == 1 && pc_ed_as_i64(pc_ed_get(&runs[0], "A"), &i) && i == 1);
    pc_ed_fini(&v);
}

static void test_write_parse_roundtrip(void)
{
    pc_ed_value v, back, again;
    pc_buf w = PC_BUF_INIT, w2 = PC_BUF_INIT;
    CHECK(pc_ed_parse(SAMPLE, SAMPLE_LEN, &v, NULL) == PC_OK);
    CHECK(pc_ed_write(&v, &w, NULL) == PC_OK);
    CHECK(pc_ed_parse(w.data, w.len, &back, NULL) == PC_OK);
    CHECK(pc_ed_eq(&back, &v));
    /* Stable: writing twice gives identical bytes. */
    CHECK(pc_ed_write(&back, &w2, NULL) == PC_OK);
    CHECK(w2.len == w.len && memcmp(w.data, w2.data, w.len) == 0);
    CHECK(pc_ed_clone(&v, &again, NULL) == PC_OK && pc_ed_eq(&again, &v));
    pc_ed_fini(&again);
    pc_ed_fini(&back);
    pc_ed_fini(&v);
    pc_buf_fini(&w);
    pc_buf_fini(&w2);
}

static void test_reals_format_like_photoshop(void)
{
    char b[PC_ED_REAL_MAX];
    pc_ed_format_real(1.0, b);
    CHECK_EQ_STR(b, "1.0");
    pc_ed_format_real(0.5, b);
    CHECK_EQ_STR(b, ".5");
    pc_ed_format_real(-0.25, b);
    CHECK_EQ_STR(b, "-.25");
    pc_ed_format_real(12.125, b);
    CHECK_EQ_STR(b, "12.125");
    pc_ed_format_real(NAN, b);
    CHECK_EQ_STR(b, "0.0");
    pc_ed_format_real(-INFINITY, b);
    CHECK_EQ_STR(b, "0.0");
    pc_ed_format_real(1.7976931348623157e308, b); /* the longest form fits */
    CHECK(strlen(b) == 310 && b[309] == '.');
}

static void test_tiny_reals_round_trip_as_numbers(void)
{
    double values[2] = {0.000004, -0.000004};
    int k;
    for (k = 0; k < 2; k++) {
        char b[PC_ED_REAL_MAX];
        pc_ed_value src = pc_ed_dict(), ed = pc_ed_dict(), runs = pc_ed_array(), run = pc_ed_dict(), v,
                    parsed;
        pc_buf w = PC_BUF_INIT;
        const pc_ed_value *a;
        size_t n = 0;
        double f = 1.0;
        pc_ed_format_real(values[k], b);
        CHECK_EQ_STR(b, "0.0");
        v = pc_ed_real(values[k]);
        CHECK(pc_ed_set(&run, "BaselineShift", &v, NULL) == PC_OK);
        CHECK(pc_ed_push(&runs, &run, NULL) == PC_OK);
        CHECK(pc_ed_set(&ed, "StyleRun", &runs, NULL) == PC_OK);
        CHECK(pc_ed_set(&src, "EngineDict", &ed, NULL) == PC_OK);
        CHECK(pc_ed_write(&src, &w, NULL) == PC_OK);
        CHECK(pc_ed_parse(w.data, w.len, &parsed, NULL) == PC_OK);
        a = pc_ed_as_array(pc_ed_path(&parsed, "EngineDict", "StyleRun", (const char *)NULL), &n);
        CHECK(a != NULL && n == 1 && pc_ed_as_f64(pc_ed_get(&a[0], "BaselineShift"), &f) && f == 0.0);
        pc_ed_fini(&parsed);
        pc_ed_fini(&src);
        pc_buf_fini(&w);
    }
}

static void test_malformed_input_never_panics(void)
{
    uint8_t deep[200];
    size_t cut;
    pc_ed_value v;
    pc_error err;
    for (cut = 0; cut < SAMPLE_LEN; cut++) {
        if (pc_ed_parse(SAMPLE, cut, &v, NULL) == PC_OK) {
            pc_ed_fini(&v);
        }
    }
    CHECK(pc_ed_parse((const uint8_t *)"<< /A", 5, &v, &err) == PC_ERR_FORMAT);
    CHECK_EQ_STR(err.msg, "EngineData: unexpected end at byte 5");
    memset(deep, '[', sizeof deep);
    CHECK(pc_ed_parse(deep, sizeof deep, &v, &err) == PC_ERR_FORMAT);
    CHECK_EQ_STR(err.msg, "EngineData: nesting too deep at byte 65");
}

/* ---- never crash: EngineData ---- */

static void test_engine_data_hostile(void)
{
    static const char *const bad[] = {"",
                                      "]",
                                      ">>",
                                      "<",
                                      "(abc",
                                      "(ab\\",
                                      "<< 1 >>",
                                      "1.2.3",
                                      "\xff",
                                      "--1",
                                      "<< /A [ 1 2 >>",
                                      "<< /A << /B (x) >>",
                                      "99999999999999999999999999e99999"};
    static const char *const msgs[] = {"EngineData: unexpected end at byte 0",
                                       "EngineData: unexpected byte at byte 1",
                                       "EngineData: unexpected byte at byte 1",
                                       "EngineData: unexpected byte at byte 1",
                                       "EngineData: unterminated string at byte 4",
                                       "EngineData: unterminated string at byte 5",
                                       "EngineData: expected key at byte 3",
                                       "EngineData: bad number at byte 5",
                                       "EngineData: unexpected byte at byte 2",
                                       "EngineData: bad token at byte 3",
                                       "EngineData: unexpected byte at byte 13",
                                       "EngineData: unterminated dictionary at byte 18",
                                       NULL};
    size_t k;
    pc_ed_value v;
    pc_error err;
    uint64_t seed = 12345;
    uint8_t buf[256];
    for (k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        pc_status st = pc_ed_parse((const uint8_t *)bad[k], strlen(bad[k]), &v, &err);
        if (msgs[k] != NULL) {
            CHECK(st == PC_ERR_FORMAT);
            CHECK_EQ_STR(err.msg, msgs[k]);
        } else {
            /* Huge exponents parse to infinity (Rust's f64 parser does the same). */
            CHECK(st == PC_OK && v.kind == PC_ED_REAL && isinf(v.r));
            pc_ed_fini(&v);
        }
    }
    CHECK(pc_ed_parse(NULL, 10, &v, &err) == PC_ERR_FORMAT);
    /* Random bytes from the EngineData alphabet never crash (ASan/UBSan runs check memory). */
    for (k = 0; k < 20000; k++) {
        size_t n = 1 + (size_t)(seed >> 33) % sizeof buf, i;
        static const char alpha[] = "<<>>[]()/\\ 0123456789.-+eEtrufalsnN\xfe\xff\x00\n";
        for (i = 0; i < n; i++) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            buf[i] = (uint8_t)alpha[(seed >> 33) % (sizeof alpha - 1)];
        }
        if (pc_ed_parse(buf, n, &v, NULL) == PC_OK) {
            pc_buf w = PC_BUF_INIT;
            CHECK(pc_ed_write(&v, &w, NULL) == PC_OK);
            pc_buf_fini(&w);
            pc_ed_fini(&v);
        }
    }
}

static void test_engine_data_limits_and_accessors(void)
{
    pc_ed_value root = pc_ed_array(), *cur = &root, v, c;
    pc_buf w = PC_BUF_INIT;
    pc_error err;
    size_t k, n = 9;
    double f = 9;
    int64_t i = 9;
    bool b = false;
    /* A programmatic tree deeper than the writer's limit is refused, not overflowed. */
    for (k = 0; k < PC_ED_MAX_TREE_DEPTH + 10; k++) {
        pc_ed_value child = pc_ed_array();
        CHECK(pc_ed_push(cur, &child, NULL) == PC_OK);
        cur = &cur->items[0];
    }
    CHECK(pc_ed_write(&root, &w, &err) == PC_ERR_LIMIT);
    CHECK(w.len == 0);
    CHECK(pc_ed_clone(&root, &c, NULL) == PC_ERR_LIMIT);
    CHECK(!pc_ed_eq(&root, &root));
    pc_ed_fini(&root);
    /* Accessors are NULL-safe and type-checked. */
    CHECK(pc_ed_get(NULL, "x") == NULL && pc_ed_as_str(NULL) == NULL && pc_ed_as_array(NULL, &n) == NULL);
    CHECK(!pc_ed_as_f64(NULL, &f) && !pc_ed_as_i64(NULL, &i) && !pc_ed_as_bool(NULL, &b));
    v = pc_ed_real(NAN);
    CHECK(pc_ed_as_i64(&v, &i) && i == 0);
    v = pc_ed_real(1e300);
    CHECK(pc_ed_as_i64(&v, &i) && i == INT64_MAX);
    v = pc_ed_real(-1e300);
    CHECK(pc_ed_as_i64(&v, &i) && i == INT64_MIN);
    v = pc_ed_int(3);
    CHECK(pc_ed_as_bool(&v, &b) && b && pc_ed_get(&v, "a") == NULL);
    /* set/push on the wrong kind drop the value (nothing leaks; ASan checks). */
    c = pc_ed_dict();
    CHECK(pc_ed_string("abc", 3, &v, NULL) == PC_OK);
    CHECK(pc_ed_push(&c, &v, NULL) == PC_OK && c.n == 0);
    CHECK(pc_ed_string("abc", 3, &v, NULL) == PC_OK);
    root = pc_ed_int(1);
    CHECK(pc_ed_set(&root, "k", &v, NULL) == PC_OK && root.kind == PC_ED_INT);
    /* Duplicate keys: get/set take the first; remove drops all. */
    v = pc_ed_int(1);
    CHECK(pc_ed_set(&c, "a", &v, NULL) == PC_OK);
    CHECK(pc_ed_parse((const uint8_t *)"<< /a 1 /b 2 /a 3 >>", 20, &root, NULL) == PC_OK);
    CHECK(pc_ed_as_i64(pc_ed_get(&root, "a"), &i) && i == 1);
    v = pc_ed_int(7);
    CHECK(pc_ed_set(&root, "a", &v, NULL) == PC_OK && root.n == 3);
    CHECK(pc_ed_as_i64(&root.entries[2].value, &i) && i == 3);
    pc_ed_remove(&root, "a");
    CHECK(root.n == 1 && pc_str_eq_c(&root.entries[0].key, "b"));
    pc_ed_fini(&root);
    pc_ed_fini(&c);
    /* Invalid UTF-8 in a constructed string writes as U+FFFD; a lone surrogate reads as U+FFFD. */
    CHECK(pc_ed_string("a\xff", 2, &v, NULL) == PC_OK);
    CHECK(pc_ed_write(&v, &w, NULL) == PC_OK);
    CHECK(w.len == 11 && memcmp(w.data,
                                "\n\n(\xfe\xff\x00"
                                "a\xff\xfd)\n",
                                11) == 0);
    pc_ed_fini(&v);
    pc_buf_fini(&w);
    CHECK(pc_ed_parse((const uint8_t *)"(\xfe\xff\xd8\x00\x00"
                                       "a)",
                      8, &v, NULL) == PC_OK);
    CHECK(v.kind == PC_ED_STRING && pc_str_eq_c(&v.s, "\xef\xbf\xbd"
                                                      "a"));
    pc_ed_fini(&v);
}

/* ---- psd_styles.rs ---- */

static void test_named_sheets_round_trip(void)
{
    pc_text_styles styles, empty;
    pc_char_style big, def, base, emph;
    pc_paragraph_style centered = pc_paragraph_style_default(), pdef = pc_paragraph_style_default();
    pc_text_layer layer, back;
    pc_psd_bytes tysh = {NULL, 0}, out = {NULL, 0}, plain = {NULL, 0};
    pc_psd_style_sheets r;
    pc_text_run *runs = NULL;
    size_t nruns = 0, k;
    bool named = true;
    uint32_t two = 2, four = 4;
    CHECK(pc_text_styles_default(&styles, NULL) == PC_OK);
    styles.character = calloc(1, sizeof *styles.character);
    styles.ncharacter = 1;
    styles.character[0].id = 4;
    CHECK(pc_str_set_c(&styles.character[0].name, "Emphasis", NULL) == PC_OK);
    pc_char_style_default(&big);
    pc_char_style_default(&def);
    big.size_pt = 30.0f;
    big.underline = true;
    CHECK(pc_char_style_diff_attrs(&big, &def, &styles.character[0].attrs, NULL) == PC_OK);
    styles.paragraph = calloc(1, sizeof *styles.paragraph);
    styles.nparagraph = 1;
    styles.paragraph[0].id = 2;
    CHECK(pc_str_set_c(&styles.paragraph[0].name, "Centered", NULL) == PC_OK);
    centered.align = PC_TEXT_ALIGN_CENTER;
    CHECK(pc_paragraph_style_diff_attrs(&centered, &pdef, &styles.paragraph[0].para_attrs, NULL) == PC_OK);

    pc_char_style_default(&base);
    CHECK(pc_str_set_c(&base.font_family, PC_TEXT_DEFAULT_FAMILY, NULL) == PC_OK);
    CHECK(pc_text_styles_resolve_char(&styles, &two, &four, PC_TEXT_DEFAULT_FAMILY, &emph, NULL) == PC_OK);
    pc_text_layer_default(&layer);
    set_text(&layer, "plain bold");
    layer.runs = calloc(2, sizeof *layer.runs);
    layer.nruns = 2;
    layer.runs[0].len = 6;
    layer.runs[0].style = base;
    layer.runs[1].len = 4;
    layer.runs[1].style = emph;
    layer.paragraphs = calloc(1, sizeof *layer.paragraphs);
    layer.nparagraphs = 1;
    layer.paragraphs[0].len = 10;
    CHECK(pc_text_styles_resolve_para(&styles, &two, &layer.paragraphs[0].style, NULL) == PC_OK);
    CHECK(pc_text_layer_sync_summary(&layer, NULL) == PC_OK);
    build(&layer, 72.0f, NULL, &tysh);
    CHECK(pc_text_has_named_sheets(tysh.data, tysh.len, &named, NULL) == PC_OK && !named);
    CHECK(pc_text_write_style_sheets(tysh.data, tysh.len, &layer, &styles, 72.0f, &out, NULL) == PC_OK);
    CHECK(pc_text_has_named_sheets(out.data, out.len, &named, NULL) == PC_OK && named);
    CHECK(pc_text_read_style_sheets(out.data, out.len, 72.0f, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r.nchar_sheets, 1);
    if (r.nchar_sheets == 1) {
        CHECK(pc_str_eq_c(&r.char_sheets[0].name, "Emphasis"));
        CHECK(r.char_sheets[0].style.size_pt == 30.0f && r.char_sheets[0].style.underline);
    }
    CHECK(r.npara_sheets >= 1 && pc_str_eq_c(&r.para_sheets[0].name, "Centered"));
    CHECK(r.npara_sheets >= 1 && r.para_sheets[0].style.align == PC_TEXT_ALIGN_CENTER);
    CHECK(r.nrun_char == 2 && !r.run_char[0].has && r.run_char[1].has && r.run_char[1].v == 1);
    CHECK(r.nrun_para == 1 && r.run_para[0].has && r.run_para[0].v == 1);
    pc_psd_style_sheets_fini(&r);
    /* The text itself still reads back the same. */
    CHECK(import(&out, 72.0f, &back));
    CHECK(pc_str_eq(&back.text, &layer.text));
    CHECK(pc_text_layer_char_runs(&back, &runs, &nruns, NULL) == PC_OK);
    CHECK(nruns >= 2 && runs[1].style.size_pt == 30.0f);
    pc_text_runs_free(runs, nruns);
    pc_text_layer_fini(&back);
    /* No styles: the sets shrink back to the normal sheets. */
    CHECK(pc_text_styles_default(&empty, NULL) == PC_OK);
    CHECK(pc_text_write_style_sheets(out.data, out.len, &layer, &empty, 72.0f, &plain, NULL) == PC_OK);
    CHECK(pc_text_has_named_sheets(plain.data, plain.len, &named, NULL) == PC_OK && !named);
    CHECK(pc_text_read_style_sheets(plain.data, plain.len, 72.0f, &r, NULL) == PC_OK);
    for (k = 0; k < r.nrun_char; k++) {
        CHECK(!r.run_char[k].has);
    }
    pc_psd_style_sheets_fini(&r);
    pc_text_styles_fini(&empty);
    pc_text_styles_fini(&styles);
    pc_char_style_fini(&big);
    pc_char_style_fini(&def);
    pc_text_layer_fini(&layer);
    pc_psd_bytes_fini(&tysh);
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&plain);
}

static void test_styles_malformed_input(void)
{
    static const uint8_t zeros[60] = {0};
    pc_psd_style_sheets r;
    pc_text_styles styles;
    pc_text_layer t;
    pc_psd_bytes out = {NULL, 0};
    bool named = true;
    CHECK(pc_text_read_style_sheets(NULL, 0, 72.0f, &r, NULL) == PC_ERR_FORMAT);
    CHECK(pc_text_read_style_sheets(zeros, sizeof zeros, 72.0f, &r, NULL) == PC_ERR_FORMAT);
    CHECK(pc_text_has_named_sheets((const uint8_t *)"junk", 4, &named, NULL) == PC_OK && !named);
    CHECK(pc_text_styles_default(&styles, NULL) == PC_OK);
    pc_text_layer_default(&t);
    CHECK(pc_text_write_style_sheets((const uint8_t *)"\x01\x02\x03", 3, &t, &styles, 72.0f, &out, NULL) ==
          PC_ERR_FORMAT);
    pc_text_styles_fini(&styles);
}

/* ---- tests.rs / vertical_tests.rs (PSD) ---- */

/* Txt2 as Photoshop writes it, for the layer text `text` (style runs (len, mode)). */
static void txt2_with_modes(pc_buf *v, const char *text, const size_t *lens, const int *modes, size_t n)
{
    size_t k;
    const uint8_t *s = (const uint8_t *)text;
    pc_buf_append_str(v, "\n\n/98 << /0 14 >> /0 << >> /1 << /1 [ << /0 << /0 (\xfe\xff", NULL);
    for (k = 0; k <= strlen(text); k++) {
        uint8_t c = k < strlen(text) ? s[k] : '\r'; /* ASCII test texts only */
        uint8_t u[2] = {0, c};
        int j;
        for (j = 0; j < 2; j++) {
            if (u[j] == '(' || u[j] == ')' || u[j] == '\\') {
                pc_buf_append_byte(v, '\\', NULL);
            }
            pc_buf_append_byte(v, u[j], NULL);
        }
    }
    pc_buf_append_str(v, ") /6 << /0 [ ", NULL);
    for (k = 0; k < n; k++) {
        pc_buf_appendf(v, NULL, "<< /0 << /0 << /0 (\xc3\xbe\xc3\xbf) /6 << /0 0 /11 %d >> >> >> /1 %zu >> ",
                       modes[k], lens[k]);
    }
    pc_buf_append_str(v, "] >> >> >> ] >>", NULL);
}

static void with_text_index(const pc_psd_bytes *in, int32_t index, pc_psd_bytes *out)
{
    pc_tysh t;
    pc_psd_value v = pc_psd_value_int(index);
    size_t r, w = 0;
    CHECK(pc_tysh_parse(in->data, in->len, &t, NULL) == PC_OK);
    for (r = 0; r < t.text.n; r++) {
        if (pc_psd_id_is(&t.text.items[r].key, "TextIndex")) {
            pc_psd_id_fini(&t.text.items[r].key);
            pc_psd_value_fini(&t.text.items[r].value);
        } else {
            t.text.items[w++] = t.text.items[r];
        }
    }
    t.text.n = w;
    CHECK(pc_psd_desc_add(&t.text, "TextIndex", &v, NULL) == PC_OK);
    CHECK(pc_tysh_write(&t, out, NULL) == PC_OK);
    pc_tysh_fini(&t);
}

static bool all_kerning(const pc_text_layer *l, pc_kerning k)
{
    pc_text_run *runs = NULL;
    size_t n = 0, i;
    bool ok = true;
    CHECK(pc_text_layer_char_runs(l, &runs, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        ok = ok && runs[i].style.kerning == k;
    }
    pc_text_runs_free(runs, n);
    return ok;
}

static void test_txt2_carries_optical_kerning(void)
{
    pc_char_style def;
    pc_text_layer t, back;
    pc_psd_bytes raw = {NULL, 0}, tysh = {NULL, 0};
    pc_buf data = PC_BUF_INIT;
    pc_ed_value txt2;
    pc_text_run *runs = NULL;
    size_t nruns = 0, k;
    static const size_t l1[] = {2, 2};
    static const int m1[] = {2, 0};
    pc_char_style_default(&def);
    styled(&t, "AVA", &def);
    build(&t, 72.0f, NULL, &raw);
    with_text_index(&raw, 0, &tysh);
    CHECK(import(&tysh, 72.0f, &back));
    CHECK(all_kerning(&back, PC_KERNING_METRICS));
    /* "AVA\r": two optical characters, then manual ("0") for the last and the break. */
    txt2_with_modes(&data, "AVA", l1, m1, 2);
    CHECK(pc_txt2_parse(data.data, data.len, &txt2, NULL) == PC_OK);
    CHECK(pc_txt2_apply(&back, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    CHECK(pc_text_layer_char_runs(&back, &runs, &nruns, NULL) == PC_OK);
    CHECK(nruns == 2 && runs[0].len == 2 && runs[0].style.kerning == PC_KERNING_OPTICAL && runs[1].len == 1 &&
          runs[1].style.kerning == PC_KERNING_OFF);
    pc_text_runs_free(runs, nruns);
    pc_ed_fini(&txt2);
    pc_text_layer_fini(&back);
    /* Stale Txt2 (other text), another index or garbage: nothing changes, nothing crashes. */
    for (k = 0; k < 4; k++) {
        static const size_t l4[] = {4};
        static const int m4[] = {2};
        pc_psd_bytes x = {NULL, 0};
        pc_text_layer l;
        pc_buf_truncate(&data, 0);
        if (k == 0) {
            txt2_with_modes(&data, "AVX", l4, m4, 1);
        } else if (k == 1) {
            txt2_with_modes(&data, "AVA", l4, m4, 1);
        } else if (k == 2) {
            pc_buf_append_str(&data, "<< /1 [ (x", NULL);
        }
        with_text_index(&raw, k == 1 ? 3 : 0, &x);
        CHECK(import(&x, 72.0f, &l));
        if (pc_txt2_parse(data.data, data.len, &txt2, NULL) == PC_OK) {
            CHECK(pc_txt2_apply(&l, x.data, x.len, &txt2, NULL) == PC_OK);
            pc_ed_fini(&txt2);
        }
        CHECK(all_kerning(&l, PC_KERNING_METRICS));
        pc_text_layer_fini(&l);
        pc_psd_bytes_fini(&x);
    }
    /* Run lengths longer than the text, zero lengths and odd modes are tolerated. */
    {
        static const size_t l3[] = {0, 100, 5};
        static const int m3[] = {2, 2, 9};
        pc_text_layer l;
        pc_buf_truncate(&data, 0);
        txt2_with_modes(&data, "AVA", l3, m3, 3);
        CHECK(pc_txt2_parse(data.data, data.len, &txt2, NULL) == PC_OK);
        CHECK(import(&tysh, 72.0f, &l));
        CHECK(pc_txt2_apply(&l, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
        CHECK(all_kerning(&l, PC_KERNING_OPTICAL));
        pc_text_layer_fini(&l);
        pc_ed_fini(&txt2);
    }
    pc_buf_fini(&data);
    pc_psd_bytes_fini(&raw);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&t);
}

static void check_fill_flags(const pc_ed_value *data, const char *const *keys, size_t nkeys)
{
    size_t n = 0, k;
    bool b;
    const pc_ed_value *runs = pc_ed_as_array(path3(data, "EngineDict", "StyleRun", "RunArray"), &n);
    CHECK(runs != NULL && n > 0);
    for (k = 0; runs != NULL && k < n; k++) {
        CHECK(pc_ed_as_bool(path3(&runs[k], "StyleSheet", "StyleSheetData", "FillFlag"), &b) && b);
    }
    for (k = 0; k < nkeys; k++) {
        size_t ns = 0;
        const pc_ed_value *sheets =
            pc_ed_as_array(pc_ed_path(data, keys[k], "StyleSheetSet", (const char *)NULL), &ns);
        CHECK(sheets != NULL && ns > 0 &&
              pc_ed_as_bool(pc_ed_path(&sheets[0], "StyleSheetData", "FillFlag", (const char *)NULL), &b) &&
              b);
    }
}

static void test_psd_text_fill_is_enabled_in_runs_and_new_default_styles(void)
{
    static const char *const keys[] = {"ResourceDict", "DocumentResources"};
    pc_text_layer t;
    int s;
    pc_text_layer_default(&t);
    set_text(&t, "AB");
    CHECK(pc_str_set_c(&t.font_family, "Inter", NULL) == PC_OK);
    t.size_pt = 100.0f;
    t.runs = calloc(2, sizeof *t.runs);
    t.nruns = 2;
    t.runs[0].len = 1;
    pc_char_style_default(&t.runs[0].style);
    t.runs[0].style.color = pc_color_rgb(0.2f, 0.4f, 0.6f);
    t.runs[1].len = 1;
    pc_char_style_default(&t.runs[1].style);
    t.runs[1].style.faux_bold = true;
    t.runs[1].style.size_pt = 100.0f;
    for (s = 0; s < 2; s++) {
        pc_psd_bytes b = {NULL, 0}, w = {NULL, 0};
        pc_tysh ty;
        pc_ed_value data;
        pc_text_layer back;
        size_t k;
        t.shape.kind = s == 1 ? PC_TEXT_SHAPE_BOX : PC_TEXT_SHAPE_POINT;
        t.shape.width = 300.0f;
        t.shape.height = 200.0f;
        build(&t, 72.0f, NULL, &b);
        CHECK(pc_tysh_parse(b.data, b.len, &ty, NULL) == PC_OK);
        CHECK(pc_tysh_engine_data(&ty.text, &data, NULL) == PC_OK);
        check_fill_flags(&data, keys, 2);
        CHECK(pc_tysh_write(&ty, &w, NULL) == PC_OK);
        CHECK(w.len == b.len && memcmp(w.data, b.data, b.len) == 0);
        CHECK(import(&w, 72.0f, &back));
        CHECK(pc_str_eq(&back.text, &t.text));
        CHECK_EQ_UINT(back.nruns, t.nruns);
        for (k = 0; k < back.nruns && k < t.nruns; k++) {
            CHECK(memcmp(&back.runs[k].style.color, &t.runs[k].style.color, sizeof t.runs[k].style.color) ==
                  0);
            CHECK(back.runs[k].style.size_pt == t.runs[k].style.size_pt);
            CHECK(back.runs[k].style.faux_bold == t.runs[k].style.faux_bold);
        }
        pc_text_layer_fini(&back);
        pc_ed_fini(&data);
        pc_tysh_fini(&ty);
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&w);
    }
    pc_text_layer_fini(&t);
}

static void remove_fill_flags(pc_ed_value *v)
{
    size_t k;
    if (v->kind == PC_ED_DICT) {
        pc_ed_remove(v, "FillFlag");
        for (k = 0; k < v->n; k++) {
            remove_fill_flags(&v->entries[k].value);
        }
    } else if (v->kind == PC_ED_ARRAY) {
        for (k = 0; k < v->n; k++) {
            remove_fill_flags(&v->items[k]);
        }
    }
}

static void test_psd_text_fill_is_enabled_after_editing_a_legacy_export(void)
{
    pc_text_layer t;
    pc_psd_bytes b = {NULL, 0}, legacy_bytes = {NULL, 0}, out = {NULL, 0};
    pc_tysh legacy, ty;
    pc_ed_value data;
    pc_buf w = PC_BUF_INIT;
    size_t k;
    pc_text_layer_default(&t);
    set_text(&t, "Before");
    CHECK(pc_str_set_c(&t.font_family, "Inter", NULL) == PC_OK);
    t.size_pt = 40.0f;
    build(&t, 72.0f, NULL, &b);
    CHECK(pc_tysh_parse(b.data, b.len, &legacy, NULL) == PC_OK);
    CHECK(pc_tysh_engine_data(&legacy.text, &data, NULL) == PC_OK);
    remove_fill_flags(&data);
    CHECK(pc_ed_write(&data, &w, NULL) == PC_OK);
    for (k = 0; k < legacy.text.n; k++) {
        if (pc_psd_id_is(&legacy.text.items[k].key, "EngineData")) {
            CHECK(pc_psd_bytes_set(&legacy.text.items[k].value.raw, w.data, w.len, NULL) == PC_OK);
        }
    }
    pc_ed_fini(&data);
    CHECK(pc_tysh_write(&legacy, &legacy_bytes, NULL) == PC_OK);
    CHECK(pc_blob_new(legacy_bytes.data, legacy_bytes.len, &t.psd_raw, NULL) == PC_OK);
    set_text(&t, "After");
    build(&t, 72.0f, NULL, &out);
    CHECK(pc_tysh_parse(out.data, out.len, &ty, NULL) == PC_OK);
    CHECK(pc_tysh_engine_data(&ty.text, &data, NULL) == PC_OK);
    check_fill_flags(&data, NULL, 0);
    pc_ed_fini(&data);
    pc_tysh_fini(&ty);
    pc_tysh_fini(&legacy);
    pc_buf_fini(&w);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&legacy_bytes);
    pc_psd_bytes_fini(&out);
    pc_text_layer_fini(&t);
}

typedef struct kspec {
    size_t len;
    pc_kerning kerning;
    float kern;
} kspec;

static void test_psd_round_trips_manual_kerning(void)
{
    static const kspec cases[5][4] = {
        {{1, PC_KERNING_OFF, 100.0f}, {1, PC_KERNING_OFF, -50.0f}, {2, PC_KERNING_METRICS, 0.0f}, {0}},
        {{1, PC_KERNING_METRICS, 0.0f}, {1, PC_KERNING_OFF, 0.0f}, {2, PC_KERNING_METRICS, 0.0f}, {0}},
        {{4, PC_KERNING_OFF, 0.0f}, {0}, {0}, {0}},
        {{2, PC_KERNING_METRICS, 0.0f}, {1, PC_KERNING_OFF, 25.0f}, {1, PC_KERNING_METRICS, 0.0f}, {0}},
        {{3, PC_KERNING_METRICS, 0.0f}, {1, PC_KERNING_OFF, 300.0f}, {0}, {0}},
    };
    size_t c;
    for (c = 0; c < 5; c++) {
        pc_text_layer t, back;
        pc_psd_bytes b = {NULL, 0};
        pc_kerning want_k[4], got_k[4];
        float want_v[4], got_v[4];
        size_t nr = 0, i, j, at;
        pc_text_run *runs = NULL;
        size_t n = 0;
        pc_text_layer_default(&t);
        set_text(&t, "AVAT");
        while (nr < 4 && cases[c][nr].len > 0) {
            nr++;
        }
        t.runs = calloc(nr, sizeof *t.runs);
        t.nruns = nr;
        for (i = 0, at = 0; i < nr; i++) {
            t.runs[i].len = cases[c][i].len;
            pc_char_style_default(&t.runs[i].style);
            t.runs[i].style.kerning = cases[c][i].kerning;
            t.runs[i].style.kern = cases[c][i].kern;
            for (j = 0; j < cases[c][i].len; j++, at++) {
                want_k[at] = cases[c][i].kerning;
                want_v[at] = cases[c][i].kern;
            }
        }
        /* The last character's mode has no EngineData slot (Metrics unless manually kerned). */
        if (want_v[3] == 0.0f) {
            want_k[3] = PC_KERNING_METRICS;
        }
        build(&t, 72.0f, NULL, &b);
        CHECK(import(&b, 72.0f, &back));
        CHECK(pc_text_layer_char_runs(&back, &runs, &n, NULL) == PC_OK);
        for (i = 0, at = 0; i < n; i++) {
            for (j = 0; j < runs[i].len && at < 4; j++, at++) {
                got_k[at] = runs[i].style.kerning;
                got_v[at] = runs[i].style.kern;
            }
        }
        CHECK_EQ_UINT(at, 4);
        for (i = 0; i < 4 && i < at; i++) {
            CHECK(got_k[i] == want_k[i] && got_v[i] == want_v[i]);
        }
        pc_text_runs_free(runs, n);
        pc_text_layer_fini(&back);
        pc_text_layer_fini(&t);
        pc_psd_bytes_fini(&b);
    }
}

static void test_psd_round_trips_antialias_opentype_and_warp(void)
{
    static const pc_antialias aas[] = {PC_AA_WINDOWS, PC_AA_WINDOWS_LCD, PC_AA_CRISP, PC_AA_NONE};
    pc_char_style style;
    size_t a;
    pc_char_style_default(&style);
    CHECK(pc_str_set_c(&style.font_family, "Inter", NULL) == PC_OK);
    style.size_pt = 20.0f;
    style.features = calloc(2, sizeof *style.features);
    style.nfeatures = 2;
    CHECK(pc_str_set_c(&style.features[0].tag, "swsh", NULL) == PC_OK);
    style.features[0].value = 1;
    CHECK(pc_str_set_c(&style.features[1].tag, "frac", NULL) == PC_OK);
    style.features[1].value = 1;
    style.discretionary_ligatures = true;
    for (a = 0; a < 4; a++) {
        pc_text_layer t, back;
        pc_psd_bytes b = {NULL, 0};
        bool swsh = false, frac = false;
        size_t k;
        styled(&t, "1/2 Swash", &style);
        t.antialias = aas[a];
        t.has_warp = true;
        CHECK(pc_str_set_c(&t.warp.style, "warpFlag", NULL) == PC_OK);
        t.warp.value = -35.0f;
        t.warp.horizontal_distortion = 10.0f;
        t.warp.horizontal = false;
        build(&t, 72.0f, NULL, &b);
        CHECK(import(&b, 72.0f, &back));
        CHECK(back.antialias == aas[a]);
        CHECK(back.has_warp && pc_text_warp_eq(&back.warp, &t.warp));
        CHECK(back.nruns >= 1 && back.runs[0].style.discretionary_ligatures);
        for (k = 0; back.nruns >= 1 && k < back.runs[0].style.nfeatures; k++) {
            swsh = swsh || pc_str_eq_c(&back.runs[0].style.features[k].tag, "swsh");
            frac = frac || pc_str_eq_c(&back.runs[0].style.features[k].tag, "frac");
        }
        CHECK(swsh && frac && back.runs[0].style.nfeatures == 2);
        pc_text_layer_fini(&back);
        pc_text_layer_fini(&t);
        pc_psd_bytes_fini(&b);
    }
    pc_char_style_fini(&style);
}

static pc_status rect_value(const char *cls, const double b[4], pc_psd_value *out)
{
    static const char *const keys[4] = {"Left", "Top ", "Rght", "Btom"};
    pc_psd_desc d;
    int i;
    CHECK(pc_psd_desc_init(&d, cls, NULL) == PC_OK);
    for (i = 0; i < 4; i++) {
        pc_psd_value v;
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_UNIT_FLOAT;
        memcpy(v.unit, "#Pnt", 4);
        v.d = b[i];
        CHECK(pc_psd_desc_add(&d, keys[i], &v, NULL) == PC_OK);
    }
    return pc_psd_value_object(&d, out, NULL);
}

/* #1469: a zero PointBase with an enormous ink origin imports on the canvas and round-trips. */
static void test_legacy_tysh_point_origin_imports_and_round_trips_on_canvas(void)
{
    static const double expected[6] = {4.1667, 0.0, 0.0, 4.1667, 1166.935, 1112.772};
    static const double ink[4] = {8050.0, 8160.0, 8220.0, 8300.0};
    static const char *const rects[2] = {"bounds", "boundingBox"};
    pc_char_style style;
    pc_text_layer src, imported, rt;
    pc_psd_bytes b = {NULL, 0}, data = {NULL, 0}, b2 = {NULL, 0};
    pc_tysh t;
    size_t r, w, k;
    float inkf[4];
    int i;
    pc_char_style_default(&style);
    CHECK(pc_str_set_c(&style.font_family, "Inter", NULL) == PC_OK);
    style.size_pt = 120.0f;
    styled(&src, "Name", &style);
    build(&src, 72.0f, NULL, &b);
    CHECK(pc_tysh_parse(b.data, b.len, &t, NULL) == PC_OK);
    t.transform.m[0] = 4.1667;
    t.transform.m[3] = 4.1667;
    t.transform.m[4] = -32375.0;
    t.transform.m[5] = -32887.5;
    for (r = 0, w = 0; r < t.text.n; r++) {
        if (pc_psd_id_is(&t.text.items[r].key, "bounds") ||
            pc_psd_id_is(&t.text.items[r].key, "boundingBox")) {
            pc_psd_id_fini(&t.text.items[r].key);
            pc_psd_value_fini(&t.text.items[r].value);
        } else {
            t.text.items[w++] = t.text.items[r];
        }
    }
    t.text.n = w;
    for (k = 0; k < 2; k++) {
        pc_psd_value v;
        CHECK(rect_value(rects[k], ink, &v) == PC_OK);
        CHECK(pc_psd_desc_add(&t.text, rects[k], &v, NULL) == PC_OK);
    }
    CHECK(pc_tysh_write(&t, &data, NULL) == PC_OK);
    CHECK(import(&data, 72.0f, &imported));
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(imported.transform.m[i], expected[i], 0.001);
    }
    {
        pc_text_engine *e = NULL;
        pc_rendered rd;
        CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
        if (e != NULL &&
            pc_text_engine_render(e, &imported, 72.0f, PC_PIXEL_FORMAT_RGBA8, NULL, &rd, NULL) == PC_OK) {
            pc_rect placed = pc_surface_content_bounds(&rd.surface);
            /* The normalised layer renders on the canvas. */
            CHECK(placed.x1 > placed.x0 && placed.y1 > placed.y0);
            CHECK(placed.x0 >= 1000 && placed.x0 < 3000 && placed.y0 >= 500 && placed.y0 < 2000);
            pc_rendered_fini(&rd);
        } else {
            PC_T_FAIL("render failed");
        }
        pc_text_engine_free(e);
    }
    for (i = 0; i < 4; i++) {
        inkf[i] = (float)ink[i];
    }
    CHECK(pc_text_build_tysh(&imported, 72.0f, inkf, &b2, NULL) == PC_OK);
    CHECK(import(&b2, 72.0f, &rt));
    for (i = 0; i < 6; i++) {
        CHECK_NEAR(rt.transform.m[i], expected[i], 0.001);
    }
    pc_text_layer_fini(&rt);
    pc_text_layer_fini(&imported);
    pc_text_layer_fini(&src);
    pc_char_style_fini(&style);
    pc_tysh_fini(&t);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&b2);
    pc_psd_bytes_fini(&data);
}

/* Regression: a template without EngineDict (or not a dictionary) panicked in the reference. */
static void test_engine_data_template_without_engine_dict(void)
{
    pc_char_style def;
    pc_text_layer t;
    int k;
    pc_char_style_default(&def);
    styled(&t, "Hi", &def);
    for (k = 0; k < 4; k++) {
        pc_ed_value tmpl, e;
        if (k == 0) {
            CHECK(pc_ed_parse((const uint8_t *)"<< /ResourceDict << >> >>", 25, &tmpl, NULL) == PC_OK);
        } else if (k == 1) {
            tmpl = pc_ed_dict();
        } else {
            tmpl = pc_ed_int(3);
        }
        CHECK(pc_text_build_engine_data(&t, k == 3 ? NULL : &tmpl, 72.0f, &e, NULL) == PC_OK);
        CHECK(str_is(path3(&e, "EngineDict", "Editor", "Text"), "Hi\r"));
        CHECK(path3(&e, "EngineDict", "Editor", "Text")->kind == PC_ED_STRING);
        pc_ed_fini(&e);
        pc_ed_fini(&tmpl);
    }
    pc_text_layer_fini(&t);
}

static void test_psd_tysh_round_trips_orientation_and_writing_direction(void)
{
    int o;
    pc_char_style style;
    pc_char_style_default(&style);
    CHECK(pc_str_set_c(&style.font_family, "Inter", NULL) == PC_OK);
    style.size_pt = 20.0f;
    for (o = 0; o < 2; o++) {
        pc_text_layer t, back;
        pc_psd_bytes b = {NULL, 0};
        pc_tysh ty;
        pc_ed_value e;
        double wd = -1;
        styled(&t,
               "\xe7\xb8\xa6\xe6\x9b\xb8\xe3\x81\x8d"
               "AB",
               &style);
        t.orientation = o == 0 ? PC_ORIENT_VERTICAL : PC_ORIENT_HORIZONTAL;
        build(&t, 72.0f, NULL, &b);
        CHECK(import(&b, 72.0f, &back));
        CHECK(back.orientation == t.orientation);
        CHECK(pc_tysh_parse(b.data, b.len, &ty, NULL) == PC_OK);
        CHECK(pc_tysh_engine_data(&ty.text, &e, NULL) == PC_OK);
        CHECK(pc_ed_as_f64(
            pc_ed_path(&e, "EngineDict", "Rendered", "Shapes", "WritingDirection", (const char *)NULL), &wd));
        CHECK(wd == (o == 0 ? 2.0 : 0.0));
        pc_ed_fini(&e);
        pc_tysh_fini(&ty);
        pc_text_layer_fini(&back);
        pc_text_layer_fini(&t);
        pc_psd_bytes_fini(&b);
    }
    /* A file without `Ornt` but with a vertical writing direction imports as vertical. */
    {
        pc_text_layer t, back;
        pc_psd_bytes b = {NULL, 0}, w = {NULL, 0};
        pc_tysh ty;
        size_t r, k = 0;
        styled(&t, "ab", &style);
        t.orientation = PC_ORIENT_VERTICAL;
        build(&t, 72.0f, NULL, &b);
        CHECK(pc_tysh_parse(b.data, b.len, &ty, NULL) == PC_OK);
        for (r = 0; r < ty.text.n; r++) {
            if (pc_psd_id_is(&ty.text.items[r].key, "Ornt")) {
                pc_psd_id_fini(&ty.text.items[r].key);
                pc_psd_value_fini(&ty.text.items[r].value);
            } else {
                ty.text.items[k++] = ty.text.items[r];
            }
        }
        ty.text.n = k;
        CHECK(pc_tysh_write(&ty, &w, NULL) == PC_OK);
        CHECK(import(&w, 72.0f, &back));
        CHECK(back.orientation == PC_ORIENT_VERTICAL);
        pc_text_layer_fini(&back);
        pc_text_layer_fini(&t);
        pc_tysh_fini(&ty);
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&w);
    }
    pc_char_style_fini(&style);
}

/* ---- never crash: TySh and layers ---- */

/* Every entry point on `data` (any outcome but a crash; ASan/UBSan check memory). */
static void poke(const uint8_t *data, size_t len, const pc_ed_value *txt2)
{
    pc_text_layer l;
    pc_psd_style_sheets s;
    pc_tysh t;
    bool b;
    pc_text_styles styles;
    pc_psd_bytes out = {NULL, 0};
    CHECK(pc_tysh_engine_text_is_blank(data, len, &b, NULL) == PC_OK);
    CHECK(pc_text_has_named_sheets(data, len, &b, NULL) == PC_OK);
    if (pc_tysh_parse(data, len, &t, NULL) == PC_OK) {
        CHECK(pc_tysh_write(&t, &out, NULL) == PC_OK);
        pc_psd_bytes_fini(&out);
        pc_tysh_fini(&t);
    }
    if (pc_text_read_style_sheets(data, len, 300.0f, &s, NULL) == PC_OK) {
        pc_psd_style_sheets_fini(&s);
    }
    if (pc_text_layer_from_tysh(data, len, 72.0f, &l, NULL) == PC_OK) {
        if (txt2 != NULL) {
            CHECK(pc_txt2_apply(&l, data, len, txt2, NULL) == PC_OK);
        }
        CHECK(pc_text_styles_default(&styles, NULL) == PC_OK);
        if (pc_text_write_style_sheets(data, len, &l, &styles, 72.0f, &out, NULL) == PC_OK) {
            pc_psd_bytes_fini(&out);
        }
        pc_text_styles_fini(&styles);
        CHECK(pc_blob_new(data, len, &l.psd_raw, NULL) == PC_OK);
        CHECK(pc_text_build_tysh(&l, 72.0f, NULL, &out, NULL) == PC_OK);
        pc_psd_bytes_fini(&out);
        pc_text_layer_fini(&l);
    }
}

/* A TySh block carrying `ed` as its EngineData. */
static void tysh_with_ed(const char *ed, size_t len, pc_psd_bytes *out)
{
    pc_char_style def;
    pc_text_layer t;
    pc_psd_bytes b = {NULL, 0};
    pc_tysh ty;
    size_t k;
    pc_char_style_default(&def);
    styled(&t, "x", &def);
    build(&t, 72.0f, NULL, &b);
    CHECK(pc_tysh_parse(b.data, b.len, &ty, NULL) == PC_OK);
    for (k = 0; k < ty.text.n; k++) {
        if (pc_psd_id_is(&ty.text.items[k].key, "EngineData")) {
            CHECK(pc_psd_bytes_set(&ty.text.items[k].value.raw, ed, len, NULL) == PC_OK);
        }
    }
    CHECK(pc_tysh_write(&ty, out, NULL) == PC_OK);
    pc_tysh_fini(&ty);
    pc_psd_bytes_fini(&b);
    pc_text_layer_fini(&t);
}

/* run lengths that don't add up, absurd sizes, NaN, wrong types */
/* Hostile EngineData: run lengths that don't add up, absurd sizes, NaN, wrong types. */
static const char ED0[] =
    "<< /EngineDict << /Editor << /Text (\xfe\xff\x00"
    "a\x00\r) >> /StyleRun << /RunArray [ << /StyleSheet "
    "<< /StyleSheetData << /FontSize 1e300 /Leading nan /AutoLeading false /Kerning inf /Tracking -1e300 "
    "/HorizontalScale nan /Font 9223372036854775807 /FontCaps -9223372036854775808 >> >> >> << >> 5 ] "
    "/RunLengthArray [ 1e300 -1e300 nan inf 18446744073709551616 ] >> /ParagraphRun << /RunArray [ << "
    "/ParagraphSheet << /Properties << /Justification 1e300 /AutoLeading nan >> >> >> ] /RunLengthArray [ "
    "-inf ] >> /Rendered << /Shapes << /Children [ << /ShapeType 1 /Cookie << /Photoshop << /BoxBounds [ inf "
    "-inf nan 1e308 ] >> >> >> ] >> >> >> /ResourceDict << /FontSet [ << /Name 5 >> ] /TheNormalStyleSheet "
    "-9223372036854775808 /StyleSheetSet [ 1 ] /ParagraphSheetSet [ << /Properties [ ] >> ] >> >>";
/* lone surrogates in the text, odd string bytes, empty everything */
static const char ED1[] = "<< /EngineDict << /Editor << /Text (\xfe\xff\xd8\x00\xdc\x00\xdc\x00\xd8\x3d) >> "
                          "/StyleRun << /RunArray [ "
                          "<< /StyleSheet << /StyleSheetData << >> >> >> ] /RunLengthArray [ 3 ] >> >> >>";
static const char ED2[] = "<< /EngineDict 1 /ResourceDict 2 /DocumentResources 3 >>";
static const char ED3[] = "<< >>";
static const char ED4[] = "[ ]";
static const char ED5[] = "(abc)";
static const char ED6[] =
    "<< /EngineDict << /Editor << /Text 5 >> /StyleRun 4 /ParagraphRun [ ] /Rendered (x) >> >>";

static const uint8_t TXT2[] = "/1 << /1 [ << /0 << /0 (\xfe\xff\x00x\x00\r) /6 << /0 [ << /1 1e300 /0 << /0 "
                              "<< /6 << /11 2 >> >> >> >> << /1 -5 >> 7 ] >> >> >> ] >>";

static void test_tysh_hostile(void)
{
    static const struct {
        const char *s;
        size_t n;
    } eds[] = {{ED0, sizeof ED0 - 1}, {ED1, sizeof ED1 - 1}, {ED2, sizeof ED2 - 1}, {ED3, sizeof ED3 - 1},
               {ED4, sizeof ED4 - 1}, {ED5, sizeof ED5 - 1}, {ED6, sizeof ED6 - 1}};
    static const uint8_t junk[] = {0, 1, 2, 3};
    pc_buf deep = PC_BUF_INIT;
    pc_ed_value txt2;
    size_t k, cut;
    uint64_t seed = 99;
    pc_psd_bytes b = {NULL, 0};
    poke(NULL, 0, NULL);
    poke(junk, sizeof junk, NULL);
    for (k = 0; k < sizeof eds / sizeof eds[0]; k++) {
        tysh_with_ed(eds[k].s, eds[k].n, &b);
        poke(b.data, b.len, NULL);
        pc_psd_bytes_fini(&b);
    }
    /* EngineData nested past the parser's limit is simply unreadable. */
    for (k = 0; k < 300; k++) {
        pc_buf_append_str(&deep, "<< /a ", NULL);
    }
    tysh_with_ed(pc_buf_cstr(&deep), deep.len, &b);
    poke(b.data, b.len, NULL);
    pc_psd_bytes_fini(&b);
    pc_buf_fini(&deep);
    /* Truncations and mutations of a full block, with a hostile Txt2 applied. */
    CHECK(pc_txt2_parse(TXT2, sizeof TXT2 - 1, &txt2, NULL) == PC_OK);
    tysh_with_ed(eds[0].s, eds[0].n, &b);
    for (cut = 0; cut <= b.len; cut += cut < 100 ? 1 : 17) {
        poke(b.data, cut, &txt2);
    }
    for (k = 0; k < 400; k++) {
        uint8_t *m = malloc(b.len);
        size_t pos;
        CHECK(m != NULL);
        if (m == NULL) {
            break;
        }
        memcpy(m, b.data, b.len);
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        pos = (size_t)(seed >> 33) % b.len;
        m[pos] = (uint8_t)(seed >> 13);
        poke(m, b.len, &txt2);
        free(m);
    }
    pc_psd_bytes_fini(&b);
    pc_ed_fini(&txt2);
}

static void test_layer_hostile(void)
{
    pc_char_style def;
    pc_text_layer t, back;
    pc_psd_bytes b = {NULL, 0};
    size_t k;
    float ink[4] = {NAN, INFINITY, -INFINITY, 3e38f};
    pc_char_style_default(&def);
    styled(&t,
           "a\xf0\x9f\x98\x80"
           "b\n\xe6\x97\xa5",
           &def);
    /* Non-finite and out-of-range style values, odd dpi, runs that don't fit the text. */
    t.runs[0].len = 1000;
    t.runs[0].style.kern = NAN;
    t.runs[0].style.tracking = INFINITY;
    t.runs[0].style.size_pt = 0.0f;
    t.runs[0].style.weight = 65535;
    t.runs[0].style.has_leading = true;
    t.runs[0].style.leading_pt = -INFINITY;
    t.paragraphs = calloc(2, sizeof *t.paragraphs);
    t.nparagraphs = 2;
    t.paragraphs[0].len = 2; /* ends inside the emoji: normalised to a char boundary */
    t.paragraphs[0].style = pc_paragraph_style_default();
    t.paragraphs[1].len = SIZE_MAX;
    t.paragraphs[1].style = pc_paragraph_style_default();
    t.shape.kind = PC_TEXT_SHAPE_BOX;
    t.shape.width = INFINITY;
    t.shape.height = NAN;
    t.has_warp = true;
    CHECK(pc_str_set(&t.warp.style, "warp\0Arc", 8, NULL) == PC_OK);
    t.warp.value = NAN;
    for (k = 0; k < 4; k++) {
        float dpi = k == 0 ? NAN : k == 1 ? -INFINITY : k == 2 ? 1e-30f : 1e30f;
        build(&t, dpi, k == 3 ? ink : NULL, &b);
        CHECK(import(&b, dpi, &back));
        CHECK(pc_str_eq(&back.text, &t.text));
        CHECK(back.has_warp && back.warp.style.len == 8);
        pc_text_layer_fini(&back);
        pc_psd_bytes_fini(&b);
    }
    /* A psd_raw that isn't a TySh block is ignored (a fresh block is written). */
    CHECK(pc_blob_new("junk", 4, &t.psd_raw, NULL) == PC_OK);
    build(&t, 72.0f, NULL, &b);
    CHECK(import(&b, 72.0f, &back));
    pc_text_layer_fini(&back);
    pc_psd_bytes_fini(&b);
    pc_text_layer_fini(&t);
}

static void test_utf16_to_byte_lengths(void)
{
    static const double lens[] = {1, 2, -1, NAN, 0.5, 1e300, 3};
    size_t out[7];
    /* "a😀é" + U+FFFD for a stray byte: 1 + 4 + 2 bytes, then the invalid byte counts as 1. */
    pc_text_utf16_to_byte_lengths("a\xf0\x9f\x98\x80\xc3\xa9\xff", 8, lens, 7, out);
    CHECK(out[0] == 1 && out[1] == 4 && out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 3 &&
          out[6] == 0);
    pc_text_utf16_to_byte_lengths(NULL, 0, lens, 2, out);
    CHECK(out[0] == 0 && out[1] == 0);
    /* One unit of a surrogate pair takes the whole character. */
    pc_text_utf16_to_byte_lengths("\xf0\x9f\x98\x80x", 5, lens, 1, out);
    CHECK(out[0] == 4);
}

static void test_postscript_for(void)
{
    static const struct {
        uint16_t w;
        bool italic;
        const char *want;
    } cases[] = {{100, false, "Inter-Thin"},       {150, true, "Inter-ThinItalic"},
                 {151, false, "Inter-ExtraLight"}, {300, false, "Inter-Light"},
                 {400, false, "Inter-Regular"},    {400, true, "Inter-Italic"},
                 {500, false, "Inter-Medium"},     {600, true, "Inter-SemiBoldItalic"},
                 {700, false, "Inter-Bold"},       {800, false, "Inter-ExtraBold"},
                 {900, true, "Inter-BlackItalic"}, {65535, false, "Inter-Black"}};
    pc_char_style s;
    pc_str out = PC_STR_INIT;
    size_t k;
    pc_char_style_default(&s);
    for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        s.weight = cases[k].w;
        s.italic = cases[k].italic;
        CHECK(pc_text_postscript_for(&s, &out, NULL) == PC_OK);
        CHECK_EQ_STR(pc_str_c(&out), cases[k].want);
        pc_str_fini(&out);
    }
    CHECK(pc_str_set_c(&s.font_family, "Times New Roman", NULL) == PC_OK);
    s.weight = 700;
    s.italic = false;
    CHECK(pc_text_postscript_for(&s, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&out), "TimesNewRoman-Bold");
    pc_str_fini(&out);
    s.has_postscript_name = true;
    CHECK(pc_str_set_c(&s.postscript_name, "ArialMT", NULL) == PC_OK);
    CHECK(pc_text_postscript_for(&s, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&out), "ArialMT");
    pc_str_fini(&out);
    pc_char_style_fini(&s);
}

static void test_tysh_layout(void)
{
    pc_char_style def;
    pc_text_layer t;
    pc_psd_bytes b = {NULL, 0}, w = {NULL, 0};
    pc_tysh ty;
    float ink[4] = {1.5f, -2.25f, 40.75f, 20.0f};
    pc_char_style_default(&def);
    styled(&t, "x", &def);
    build(&t, 72.0f, ink, &b);
    CHECK(b.len > 60 && pc_load_be16(b.data) == 1 && pc_load_be16(b.data + 50) == 50);
    CHECK(pc_tysh_parse(b.data, b.len, &ty, NULL) == PC_OK);
    /* Unwarped type: Photoshop writes zero trailing bounds (f32). */
    CHECK(ty.has_warp && ty.bounds[0] == 0.0f && ty.bounds[1] == 0.0f && ty.bounds[2] == 0.0f &&
          ty.bounds[3] == 0.0f);
    /* write(parse(b)) == b, and a missing warp writes the default one. */
    CHECK(pc_tysh_write(&ty, &w, NULL) == PC_OK && w.len == b.len && memcmp(w.data, b.data, b.len) == 0);
    pc_psd_bytes_fini(&w);
    pc_psd_desc_fini(&ty.warp);
    ty.has_warp = false;
    CHECK(pc_tysh_write(&ty, &w, NULL) == PC_OK && w.len == b.len && memcmp(w.data, b.data, b.len) == 0);
    pc_psd_bytes_fini(&w);
    pc_tysh_fini(&ty);
    CHECK(pc_tysh_parse(b.data, 51, &ty, NULL) == PC_ERR_FORMAT);
    pc_psd_bytes_fini(&b);
    pc_text_layer_fini(&t);
}

int main(int argc, char **argv)
{
    RUN(test_parses_sample);
    RUN(test_write_parse_roundtrip);
    RUN(test_reals_format_like_photoshop);
    RUN(test_tiny_reals_round_trip_as_numbers);
    RUN(test_malformed_input_never_panics);
    RUN(test_engine_data_hostile);
    RUN(test_engine_data_limits_and_accessors);
    RUN(test_named_sheets_round_trip);
    RUN(test_styles_malformed_input);
    RUN(test_txt2_carries_optical_kerning);
    RUN(test_psd_text_fill_is_enabled_in_runs_and_new_default_styles);
    RUN(test_psd_text_fill_is_enabled_after_editing_a_legacy_export);
    RUN(test_psd_round_trips_manual_kerning);
    RUN(test_psd_round_trips_antialias_opentype_and_warp);
    RUN(test_legacy_tysh_point_origin_imports_and_round_trips_on_canvas);
    RUN(test_engine_data_template_without_engine_dict);
    RUN(test_psd_tysh_round_trips_orientation_and_writing_direction);
    RUN(test_tysh_hostile);
    RUN(test_layer_hostile);
    RUN(test_utf16_to_byte_lengths);
    RUN(test_postscript_for);
    RUN(test_tysh_layout);
    return pc_t_finish();
}
