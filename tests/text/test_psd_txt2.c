/* PSD type-layer tests added upstream after L15 (crates/text/src/tests.rs): the document Txt2 block
 * (OpenType settings, building it for export, frames), TySh trailing bounds and translation,
 * EngineData anti-aliasing numbers, tabs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "harness.h"
#include "psd/descriptor.h"
#include "text/text.h"

/* ---- helpers ---- */

static void set_text(pc_text_layer *t, const char *s)
{
    CHECK(pc_str_set_c(&t->text, s, NULL) == PC_OK);
}

static void point(pc_text_layer *t, const char *text, float size)
{
    pc_text_layer_default(t);
    set_text(t, text);
    CHECK(pc_str_set_c(&t->font_family, "Inter", NULL) == PC_OK);
    t->size_pt = size;
}

static void styled(pc_text_layer *t, const char *text, const pc_char_style *style)
{
    pc_text_layer_default(t);
    set_text(t, text);
    t->runs = calloc(1, sizeof *t->runs);
    t->nruns = 1;
    t->runs[0].len = strlen(text);
    CHECK(pc_char_style_clone(style, &t->runs[0].style, NULL) == PC_OK);
}

static pc_char_style kstyle(pc_kerning k, float kern)
{
    pc_char_style s;
    pc_char_style_default(&s);
    s.kerning = k;
    s.kern = kern;
    return s;
}

/* runs_of(text, [(len, style)]) (styles are taken over). */
static void runs_of(pc_text_layer *t, const char *text, size_t n, const size_t *lens, pc_char_style *styles)
{
    size_t i;
    pc_text_layer_default(t);
    set_text(t, text);
    t->runs = calloc(n, sizeof *t->runs);
    t->nruns = n;
    for (i = 0; i < n; i++) {
        t->runs[i].len = lens[i];
        t->runs[i].style = styles[i];
    }
}

static void build(const pc_text_layer *t, const float *ink, pc_psd_bytes *out)
{
    CHECK(pc_text_build_tysh(t, 72.0f, ink, out, NULL) == PC_OK);
}

static bool import(const pc_psd_bytes *b, pc_text_layer *out)
{
    return pc_text_layer_from_tysh(b->data, b->len, 72.0f, out, NULL) == PC_OK;
}

/* with_text_index(tysh, index) (replaces *b). */
static void with_text_index(pc_psd_bytes *b, int32_t index)
{
    pc_tysh t;
    pc_psd_value v = pc_psd_value_int(index);
    size_t r, w = 0;
    pc_psd_bytes out = {NULL, 0};
    CHECK(pc_tysh_parse(b->data, b->len, &t, NULL) == PC_OK);
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
    CHECK(pc_tysh_write(&t, &out, NULL) == PC_OK);
    pc_tysh_fini(&t);
    pc_psd_bytes_fini(b);
    *b = out;
}

/* txt2_with_styles: Txt2 as Photoshop writes it for the layer text `text` (ASCII), style runs
 * (len, keys as EngineData text). */
static void txt2_with_styles(pc_buf *v, const char *text, const size_t *lens, const char *const *keys,
                             size_t n)
{
    size_t k;
    pc_buf_append_str(v, "\n\n/98 << /0 14 >> /0 << >> /1 << /1 [ << /0 << /0 (\xfe\xff", NULL);
    for (k = 0; k <= strlen(text); k++) {
        uint8_t c = k < strlen(text) ? (uint8_t)text[k] : '\r';
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
        pc_buf_appendf(v, NULL, "<< /0 << /0 << /0 (\xc3\xbe\xc3\xbf) /6 << /0 0 %s >> >> >> /1 %zu >> ",
                       keys[k], lens[k]);
    }
    pc_buf_append_str(v, "] >> >> >> ] >>", NULL);
}

static void txt2_parse_buf(const pc_buf *b, pc_ed_value *out)
{
    CHECK(pc_txt2_parse(b->data, b->len, out, NULL) == PC_OK);
}

/* photoshop_txt2: a Txt2 shaped like Photoshop's for "AVA\r" with (UTF-16 length, mode) runs. */
static void photoshop_txt2(pc_buf *v, const size_t *lens, const int *modes, size_t n)
{
    static const char head[] =
        "\n\n/98 << /0 14 >> /0 << /1 << /0 [ << /0 << /0 (MyriadPro-Regular) /2 0 >> >> ] "
        ">> >> /1 << /0 << /0 1 /1 .5 >> /1 [ << /0 << /0 (\xfe\xff\0A\0V\0A\0\r) /6 << /0 [ ";
    static const char tail[] =
        "] >> /7 << /0 1 >> >> /21 << /1 [ 1.25 2.5 ] >> >> << /0 << /0 (\xfe\xff\0B\0\r) >> "
        "/21 << /1 [ 3.0 ] >> >> ] /2 << /0 7 >> >> /3 << /0 .25 >>";
    size_t k;
    pc_buf_append(v, head, sizeof head - 1, NULL);
    for (k = 0; k < n; k++) {
        pc_buf_appendf(
            v, NULL,
            "<< /0 << /0 << /0 (\xc3\xbe\xc3\xbf) /6 << /0 0 /1 12.0 /2 14.5 /11 %d >> >> >> /1 %zu >> ",
            modes[k], lens[k]);
    }
    pc_buf_append(v, tail, sizeof tail - 1, NULL);
}

static size_t nruns_of(const pc_text_layer *l, size_t *lens, pc_kerning *ks, size_t cap)
{
    pc_text_run *runs = NULL;
    size_t n = 0, i;
    CHECK(pc_text_layer_char_runs(l, &runs, &n, NULL) == PC_OK);
    for (i = 0; i < n && i < cap; i++) {
        lens[i] = runs[i].len;
        ks[i] = runs[i].style.kerning;
    }
    pc_text_runs_free(runs, n);
    return n;
}

/* The features of every run as "tag=value,..;" */
static void feats(const pc_text_layer *l, char *out, size_t cap)
{
    pc_text_run *runs = NULL;
    size_t n = 0, i, k, at = 0;
    CHECK(pc_text_layer_char_runs(l, &runs, &n, NULL) == PC_OK);
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        for (k = 0; k < runs[i].style.nfeatures; k++) {
            at += (size_t)snprintf(out + at, at < cap ? cap - at : 0, "%s=%u,",
                                   pc_str_c(&runs[i].style.features[k].tag), runs[i].style.features[k].value);
        }
        at += (size_t)snprintf(out + at, at < cap ? cap - at : 0, ";");
    }
    pc_text_runs_free(runs, n);
}

static size_t glyph_ids(pc_text_engine *e, const pc_text_layer *t, uint32_t *ids, size_t cap)
{
    pc_text_layout l;
    size_t i, n;
    CHECK(pc_text_engine_layout(e, t, 72.0f, &l, NULL) == PC_OK);
    n = l.nglyphs;
    for (i = 0; i < n && i < cap; i++) {
        ids[i] = l.glyphs[i].id;
    }
    pc_text_layout_fini(&l);
    return n;
}

static void build_txt2(const pc_text_layer *t, int32_t index, const pc_buf *prev, pc_buf *out)
{
    pc_txt2_object o;
    o.index = index;
    o.layer = t;
    CHECK(pc_txt2_build(&o, 1, prev != NULL ? prev->data : NULL, prev != NULL ? prev->len : 0, prev != NULL,
                        out, NULL) == PC_OK);
}

/* ---- tests ---- */

/* The tab stays a tab in the document: PSD type round-trips it unchanged. */
static void test_psd_round_trips_tabs(void)
{
    int box;
    for (box = 0; box < 2; box++) {
        pc_text_layer t, back;
        pc_psd_bytes b = {NULL, 0};
        point(&t, "Name\tValue\n\tIndented", 20.0f);
        if (box) {
            t.shape.is_box = true;
            t.shape.width = 300.0f;
            t.shape.height = 100.0f;
        }
        build(&t, NULL, &b);
        CHECK(import(&b, &back));
        CHECK_EQ_STR(pc_str_c(&back.text), pc_str_c(&t.text));
        pc_text_layer_fini(&back);
        pc_text_layer_fini(&t);
        pc_psd_bytes_fini(&b);
    }
}

/* #3617: Stylistic Sets, Contextual Alternates, Swash and the other OpenType settings live only in
 * Txt2; import reads them, so editing keeps Photoshop's glyphs. */
static void test_txt2_carries_opentype_features(void)
{
    static const char *text = "Il->36";
    static const size_t lens[2] = {2, 5};
    static const char *const keys[2] = {"/11 1 /18 true /19 false /20 true /25 true /85 2",
                                        "/11 1 /20 false /23 true /85 1"};
    static const size_t odd_lens[4] = {1, 1, 1, 4};
    static const char *const odd[4] = {"/85 1", "/85 (x) /20 (x) /25 (true)", "/85 -1",
                                       "/85 99999999999 /999 true"};
    pc_char_style s;
    pc_text_layer plain, back, expected, t;
    pc_psd_bytes tysh = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    pc_ed_value txt2;
    char f[256];
    size_t rl[4], n;
    pc_kerning rk[4];
    pc_text_engine *e = NULL;
    uint32_t shown[16], want[16], defaults[16];
    size_t nshown, nwant, ndef, i;
    pc_char_style_default(&s);
    CHECK(pc_str_set_c(&s.font_family, "Inter", NULL) == PC_OK);
    styled(&plain, text, &s);
    build(&plain, NULL, &tysh);
    with_text_index(&tysh, 0);
    txt2_with_styles(&b, text, lens, keys, 2);
    txt2_parse_buf(&b, &txt2);
    CHECK(import(&tysh, &back));
    feats(&back, f, sizeof f);
    CHECK_EQ_STR(f, ";"); /* EngineData alone doesn't have them */
    CHECK(pc_txt2_apply(&back, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    feats(&back, f, sizeof f);
    CHECK_EQ_STR(f, "calt=1,swsh=1,ss02=1,;calt=0,frac=1,ss01=1,;");
    n = nruns_of(&back, rl, rk, 4);
    CHECK(n == 2 && rl[0] == 2 && rl[1] == 4);
    /* What editing shows has Photoshop's glyphs: Inter's ss02 draws another I and l, ss01 another
     * 3 and 6, and `->` stays two glyphs without calt (with it, an arrow). */
    CHECK(pc_text_engine_new(&e, NULL) == PC_OK);
    CHECK(import(&tysh, &expected));
    pc_text_runs_free(expected.runs, expected.nruns);
    expected.runs = calloc(2, sizeof *expected.runs);
    expected.nruns = 2;
    for (i = 0; i < 2; i++) {
        pc_char_style *st = &expected.runs[i].style;
        CHECK(pc_char_style_clone(&back.runs[0].style, st, NULL) == PC_OK);
        for (n = 0; n < st->nfeatures; n++) {
            pc_str_fini(&st->features[n].tag);
        }
        free(st->features);
        st->features = calloc(2, sizeof *st->features);
        st->nfeatures = i == 0 ? 1 : 2;
    }
    expected.runs[0].len = 2;
    CHECK(pc_str_set_c(&expected.runs[0].style.features[0].tag, "ss02", NULL) == PC_OK);
    expected.runs[0].style.features[0].value = 1;
    expected.runs[1].len = 4;
    CHECK(pc_str_set_c(&expected.runs[1].style.features[0].tag, "calt", NULL) == PC_OK);
    expected.runs[1].style.features[0].value = 0;
    CHECK(pc_str_set_c(&expected.runs[1].style.features[1].tag, "ss01", NULL) == PC_OK);
    expected.runs[1].style.features[1].value = 1;
    nshown = glyph_ids(e, &back, shown, 16);
    nwant = glyph_ids(e, &expected, want, 16);
    CHECK(nshown == nwant && memcmp(shown, want, nshown * sizeof *shown) == 0);
    CHECK(import(&tysh, &t));
    ndef = glyph_ids(e, &t, defaults, 16);
    pc_text_layer_fini(&t);
    CHECK_EQ_UINT(nshown, 6);
    CHECK_EQ_UINT(ndef, 5);
    CHECK(nshown > 0 && ndef > 0 && shown[0] != defaults[0] && shown[nshown - 1] != defaults[ndef - 1]);
    /* A feature the style already sets keeps its value; flags and masks of the wrong type, masks
     * beyond the 20 sets and unknown keys change nothing. */
    CHECK(import(&tysh, &t));
    pc_text_runs_free(t.runs, t.nruns);
    t.runs = calloc(1, sizeof *t.runs);
    t.nruns = 1;
    t.runs[0].len = strlen(text);
    pc_char_style_default(&t.runs[0].style);
    t.runs[0].style.features = calloc(1, sizeof *t.runs[0].style.features);
    t.runs[0].style.nfeatures = 1;
    CHECK(pc_str_set_c(&t.runs[0].style.features[0].tag, "ss01", NULL) == PC_OK);
    pc_ed_fini(&txt2);
    pc_buf_fini(&b);
    txt2_with_styles(&b, text, odd_lens, odd, 4);
    txt2_parse_buf(&b, &txt2);
    CHECK(pc_txt2_apply(&t, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    feats(&t, f, sizeof f);
    CHECK_EQ_STR(f, "ss01=0,;");
    pc_text_layer_fini(&t);
    pc_ed_fini(&txt2);
    pc_buf_fini(&b);
    pc_text_engine_free(e);
    pc_text_layer_fini(&expected);
    pc_text_layer_fini(&back);
    pc_text_layer_fini(&plain);
    pc_psd_bytes_fini(&tysh);
    pc_char_style_fini(&s);
}

/* A Txt2 written by pc_txt2_build restores every auto-kern mode on import (#1348). */
static void test_build_txt2_round_trips_kerning_modes(void)
{
    static const size_t l3[3] = {2, 1, 1}, l2[2] = {1, 1};
    pc_char_style st3[3], st2[2];
    pc_text_layer t, back, manual;
    pc_psd_bytes tysh = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    pc_ed_value txt2;
    size_t rl[4], n;
    pc_kerning rk[4];
    st3[0] = kstyle(PC_KERNING_OPTICAL, 0.0f);
    st3[1] = kstyle(PC_KERNING_METRICS, 0.0f);
    st3[2] = kstyle(PC_KERNING_OFF, 0.0f);
    runs_of(&t, "AVAT", 3, l3, st3);
    build(&t, NULL, &tysh);
    with_text_index(&tysh, 0);
    CHECK(import(&tysh, &back));
    n = nruns_of(&back, rl, rk, 4);
    while (n > 0) {
        n--;
        CHECK(rk[n] != PC_KERNING_OPTICAL); /* EngineData alone can't say Optical */
    }
    build_txt2(&t, 0, NULL, &b);
    txt2_parse_buf(&b, &txt2);
    CHECK(pc_txt2_apply(&back, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    n = nruns_of(&back, rl, rk, 4);
    CHECK(n == 3 && rl[0] == 2 && rk[0] == PC_KERNING_OPTICAL && rl[1] == 1 && rk[1] == PC_KERNING_METRICS &&
          rl[2] == 1 && rk[2] == PC_KERNING_OFF);
    pc_ed_fini(&txt2);
    pc_buf_fini(&b);
    pc_text_layer_fini(&back);
    pc_psd_bytes_fini(&tysh);
    /* A manual kern (a nonzero pair value) is the manual mode. */
    st2[0] = kstyle(PC_KERNING_METRICS, 50.0f);
    st2[1] = kstyle(PC_KERNING_METRICS, 0.0f);
    runs_of(&manual, "AB", 2, l2, st2);
    build(&manual, NULL, &tysh);
    with_text_index(&tysh, 0);
    CHECK(import(&tysh, &back));
    build_txt2(&manual, 0, NULL, &b);
    txt2_parse_buf(&b, &txt2);
    CHECK(pc_txt2_apply(&back, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    n = nruns_of(&back, rl, rk, 4);
    CHECK(n == 2 && rl[0] == 1 && rk[0] == PC_KERNING_OFF && rl[1] == 1 && rk[1] == PC_KERNING_METRICS);
    pc_ed_fini(&txt2);
    pc_buf_fini(&b);
    pc_text_layer_fini(&back);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&manual);
    pc_text_layer_fini(&t);
}

static bool needs(const pc_text_layer *t)
{
    pc_txt2_object o;
    bool out = true;
    o.index = 0;
    o.layer = t;
    CHECK(pc_txt2_needed(&o, 1, &out, NULL) == PC_OK);
    return out;
}

/* #3514: Txt2 is only needed for Optical. */
static void test_needs_txt2_only_for_optical(void)
{
    static const size_t l1[1] = {2}, l2[2] = {1, 1};
    pc_char_style a[1], b[1], c[2];
    pc_text_layer metrics, off, optical;
    bool out = true;
    a[0] = kstyle(PC_KERNING_METRICS, 0.0f);
    runs_of(&metrics, "AV", 1, l1, a);
    CHECK(!needs(&metrics));
    b[0] = kstyle(PC_KERNING_OFF, 0.0f);
    runs_of(&off, "AV", 1, l1, b);
    CHECK(!needs(&off)); /* Off lives in EngineData, not Txt2 */
    c[0] = kstyle(PC_KERNING_OPTICAL, 0.0f);
    pc_char_style_default(&c[1]);
    runs_of(&optical, "AV", 2, l2, c);
    CHECK(needs(&optical));
    CHECK(pc_txt2_needed(NULL, 0, &out, NULL) == PC_OK && !out);
    pc_text_layer_fini(&metrics);
    pc_text_layer_fini(&off);
    pc_text_layer_fini(&optical);
}

static pc_ed_value str_v(const char *s)
{
    pc_ed_value v;
    CHECK(pc_ed_string(s, strlen(s), &v, NULL) == PC_OK);
    return v;
}

static void dset(pc_ed_value *d, const char *k, pc_ed_value v)
{
    CHECK(pc_ed_set(d, k, &v, NULL) == PC_OK);
}

/* A kept object (same text) keeps its extras; a changed text drops them; block extras survive. */
static void test_txt2_keeps_a_kept_objects_extras_and_drops_them_with_its_text(void)
{
    static const size_t l2[2] = {1, 1};
    pc_ed_value outer = pc_ed_dict(), obj = pc_ed_dict(), model = pc_ed_dict(), six = pc_ed_dict(),
                pens = pc_ed_dict(), arr = pc_ed_array(), ver = pc_ed_dict(), objs = pc_ed_dict(),
                list = pc_ed_array(), out, v;
    pc_buf prev = PC_BUF_INIT, b = PC_BUF_INIT;
    pc_char_style s[2];
    pc_text_layer t;
    const pc_ed_value *objects;
    size_t n = 0;
    int64_t i;
    dset(&model, "0", str_v("AB\r"));
    dset(&six, "0", pc_ed_array());
    dset(&model, "6", six);
    dset(&model, "keep", pc_ed_int(7));
    dset(&obj, "0", model);
    v = pc_ed_real(1.5);
    CHECK(pc_ed_push(&arr, &v, NULL) == PC_OK);
    v = pc_ed_real(2.5);
    CHECK(pc_ed_push(&arr, &v, NULL) == PC_OK);
    dset(&pens, "1", arr);
    dset(&obj, "21", pens);
    dset(&ver, "0", pc_ed_int(14));
    dset(&outer, "98", ver);
    dset(&outer, "0", pc_ed_dict());
    CHECK(pc_ed_push(&list, &obj, NULL) == PC_OK);
    dset(&objs, "1", list);
    dset(&outer, "1", objs);
    dset(&outer, "extra", pc_ed_int(3));
    CHECK(pc_ed_write_bare(&outer, &prev, NULL) == PC_OK);
    pc_ed_fini(&outer);
    /* Same text: the extras survive, the style runs are ours. */
    s[0] = kstyle(PC_KERNING_OPTICAL, 0.0f);
    s[1] = kstyle(PC_KERNING_METRICS, 0.0f);
    runs_of(&t, "AB", 2, l2, s);
    build_txt2(&t, 0, &prev, &b);
    txt2_parse_buf(&b, &out);
    CHECK(pc_ed_as_i64(pc_ed_get(&out, "extra"), &i) && i == 3);
    objects = pc_ed_as_array(pc_ed_path(&out, "1", "1", (const char *)NULL), &n);
    CHECK(objects != NULL && n == 1);
    if (objects != NULL && n == 1) {
        CHECK(pc_ed_as_array(pc_ed_path(&objects[0], "21", "1", (const char *)NULL), &n) != NULL && n == 2);
        CHECK(pc_ed_as_i64(pc_ed_path(&objects[0], "0", "keep", (const char *)NULL), &i) && i == 7);
        CHECK(pc_ed_as_array(pc_ed_path(&objects[0], "0", "6", "0", (const char *)NULL), &n) != NULL &&
              n == 2);
    }
    pc_ed_fini(&out);
    pc_buf_fini(&b);
    pc_text_layer_fini(&t);
    /* Changed text: the extras go stale with the old runs and are dropped. */
    s[0] = kstyle(PC_KERNING_OPTICAL, 0.0f);
    s[1] = kstyle(PC_KERNING_METRICS, 0.0f);
    runs_of(&t, "XY", 2, l2, s);
    build_txt2(&t, 0, &prev, &b);
    txt2_parse_buf(&b, &out);
    CHECK(pc_ed_as_i64(pc_ed_get(&out, "extra"), &i) && i == 3);
    objects = pc_ed_as_array(pc_ed_path(&out, "1", "1", (const char *)NULL), &n);
    CHECK(objects != NULL && n == 1);
    if (objects != NULL && n == 1) {
        CHECK(pc_ed_get(&objects[0], "21") == NULL);
        CHECK(pc_ed_path(&objects[0], "0", "keep", (const char *)NULL) == NULL);
    }
    pc_ed_fini(&out);
    pc_buf_fini(&b);
    pc_text_layer_fini(&t);
    pc_buf_fini(&prev);
}

/* #2374: an unedited save hands Photoshop back its own Txt2, byte for byte. */
static void test_unchanged_txt2_is_written_back_byte_for_byte(void)
{
    static const size_t la[2] = {2, 2}, lb[2] = {1, 3};
    static const int ma[2] = {1, 1}, mb[2] = {2, 1};
    pc_char_style s;
    pc_text_layer src, t;
    pc_psd_bytes tysh = {NULL, 0};
    pc_buf prev = PC_BUF_INIT, b = PC_BUF_INIT;
    pc_ed_value txt2;
    size_t rl[4], n;
    pc_kerning rk[4];
    pc_char_style_default(&s);
    styled(&src, "AVA", &s);
    build(&src, NULL, &tysh);
    with_text_index(&tysh, 0);
    photoshop_txt2(&prev, la, ma, 2);
    CHECK(import(&tysh, &t));
    txt2_parse_buf(&prev, &txt2);
    CHECK(pc_txt2_apply(&t, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    pc_ed_fini(&txt2);
    n = nruns_of(&t, rl, rk, 4);
    while (n > 0) {
        n--;
        CHECK(rk[n] == PC_KERNING_METRICS);
    }
    build_txt2(&t, 0, &prev, &b);
    CHECK(b.len == prev.len && memcmp(b.data, prev.data, b.len) == 0);
    pc_buf_fini(&b);
    pc_buf_fini(&prev);
    /* An optical run reads back and writes back the same way. */
    photoshop_txt2(&prev, lb, mb, 2);
    txt2_parse_buf(&prev, &txt2);
    CHECK(pc_txt2_apply(&t, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    pc_ed_fini(&txt2);
    n = nruns_of(&t, rl, rk, 4);
    CHECK(n > 0 && rk[0] == PC_KERNING_OPTICAL);
    build_txt2(&t, 0, &prev, &b);
    CHECK(b.len == prev.len && memcmp(b.data, prev.data, b.len) == 0);
    pc_buf_fini(&b);
    pc_buf_fini(&prev);
    pc_text_layer_fini(&t);
    pc_text_layer_fini(&src);
    pc_psd_bytes_fini(&tysh);
    pc_char_style_fini(&s);
}

/* An edited auto-kern mode changes only /11 (splitting the run it falls inside). */
static void test_edited_txt2_keeps_the_files_styles_and_objects(void)
{
    static const size_t l1[1] = {4}, l2[2] = {1, 2};
    static const int m1[1] = {1};
    static const char *const keys[4][3] = {
        {"0", NULL, NULL}, {"1", "0", NULL}, {"1", "2", NULL}, {"3", NULL, NULL}};
    pc_buf prev = PC_BUF_INIT, b = PC_BUF_INIT;
    pc_char_style s[2];
    pc_text_layer t, back;
    pc_ed_value out, before;
    const pc_ed_value *objects, *bobjects, *runs;
    size_t n = 0, bn = 0, k, rl[4];
    pc_kerning rk[4];
    pc_psd_bytes tysh = {NULL, 0};
    int64_t i;
    photoshop_txt2(&prev, l1, m1, 1);
    s[0] = kstyle(PC_KERNING_OPTICAL, 0.0f);
    s[1] = kstyle(PC_KERNING_METRICS, 0.0f);
    runs_of(&t, "AVA", 2, l2, s);
    build_txt2(&t, 0, &prev, &b);
    txt2_parse_buf(&b, &out);
    txt2_parse_buf(&prev, &before);
    for (k = 0; k < 4; k++) {
        const pc_ed_value *x = pc_ed_path(&out, keys[k][0], keys[k][1], keys[k][2]);
        const pc_ed_value *y = pc_ed_path(&before, keys[k][0], keys[k][1], keys[k][2]);
        CHECK(x != NULL && y != NULL && pc_ed_eq(x, y));
    }
    objects = pc_ed_as_array(pc_ed_path(&out, "1", "1", (const char *)NULL), &n);
    bobjects = pc_ed_as_array(pc_ed_path(&before, "1", "1", (const char *)NULL), &bn);
    CHECK(objects != NULL && n == 2 && bobjects != NULL && bn == 2);
    if (objects != NULL && n == 2 && bobjects != NULL && bn == 2) {
        CHECK(pc_ed_eq(&objects[1], &bobjects[1]));
        CHECK(pc_ed_as_i64(pc_ed_path(&objects[0], "0", "7", "0", (const char *)NULL), &i) && i == 1);
        CHECK(pc_ed_path(&objects[0], "21", "1", (const char *)NULL) != NULL);
        runs = pc_ed_as_array(pc_ed_path(&objects[0], "0", "6", "0", (const char *)NULL), &n);
        CHECK(runs != NULL && n == 2);
        for (k = 0; runs != NULL && k < n; k++) {
            int64_t len = -1, mode = -1;
            double v = 0.0;
            CHECK(pc_ed_as_i64(pc_ed_get(&runs[k], "1"), &len));
            CHECK(pc_ed_as_i64(pc_ed_path(&runs[k], "0", "0", "6", "11", (const char *)NULL), &mode));
            CHECK(k == 0 ? (len == 1 && mode == 2) : (len == 3 && mode == 1));
            CHECK(pc_ed_as_f64(pc_ed_path(&runs[k], "0", "0", "6", "1", (const char *)NULL), &v) &&
                  v == 12.0);
            CHECK(pc_ed_as_f64(pc_ed_path(&runs[k], "0", "0", "6", "2", (const char *)NULL), &v) &&
                  v == 14.5);
        }
    }
    /* And it reads back with the edited modes. */
    build(&t, NULL, &tysh);
    with_text_index(&tysh, 0);
    CHECK(import(&tysh, &back));
    CHECK(pc_txt2_apply(&back, tysh.data, tysh.len, &out, NULL) == PC_OK);
    n = nruns_of(&back, rl, rk, 4);
    CHECK(n == 2 && rl[0] == 1 && rk[0] == PC_KERNING_OPTICAL && rl[1] == 2 && rk[1] == PC_KERNING_METRICS);
    pc_text_layer_fini(&back);
    pc_psd_bytes_fini(&tysh);
    pc_ed_fini(&out);
    pc_ed_fini(&before);
    pc_buf_fini(&b);
    pc_buf_fini(&prev);
    pc_text_layer_fini(&t);
}

/* The file's runs are reused only when they cover the text; hostile lengths regenerate them. */
static void test_txt2_runs_that_dont_cover_the_text_are_regenerated(void)
{
    static const size_t a[1] = {3}, b1[1] = {9}, c[2] = {(size_t)INT64_MAX, 2};
    static const int m1[2] = {1, 1};
    static const size_t l1[1] = {3};
    const size_t *cases[4] = {a, b1, c, NULL};
    const size_t ns[4] = {1, 1, 2, 0};
    pc_char_style s[1];
    pc_text_layer t;
    size_t k;
    pc_char_style_default(&s[0]);
    runs_of(&t, "AVA", 1, l1, s);
    for (k = 0; k < 4; k++) {
        pc_buf prev = PC_BUF_INIT, b = PC_BUF_INIT;
        pc_ed_value out;
        const pc_ed_value *objs, *runs;
        size_t n = 0, nr = 0, r;
        int64_t total = 0;
        photoshop_txt2(&prev, cases[k], m1, ns[k]);
        build_txt2(&t, 0, &prev, &b);
        txt2_parse_buf(&b, &out);
        objs = pc_ed_as_array(pc_ed_path(&out, "1", "1", (const char *)NULL), &n);
        runs = objs != NULL && n > 0
                   ? pc_ed_as_array(pc_ed_path(&objs[0], "0", "6", "0", (const char *)NULL), &nr)
                   : NULL;
        for (r = 0; runs != NULL && r < nr; r++) {
            int64_t l;
            if (pc_ed_as_i64(pc_ed_get(&runs[r], "1"), &l)) {
                total += l;
            }
        }
        CHECK(runs != NULL);
        CHECK(total == 4);
        CHECK(pc_ed_path(&out, "1", "2", (const char *)NULL) != NULL);
        pc_ed_fini(&out);
        pc_buf_fini(&b);
        pc_buf_fini(&prev);
    }
    pc_text_layer_fini(&t);
}

/* A file-controlled TextIndex must not size the save. */
static void test_hostile_text_index_is_ignored_not_sized(void)
{
    static const size_t l1[1] = {2};
    static const int32_t hostile[2] = {PC_TXT2_MAX_TEXT_INDEX + 1, INT32_MAX};
    pc_char_style s[1];
    pc_text_layer t;
    pc_psd_bytes tysh = {NULL, 0}, x = {NULL, 0};
    bool has = false;
    int32_t ix = -1;
    size_t k, n = 0;
    pc_char_style_default(&s[0]);
    runs_of(&t, "AB", 1, l1, s);
    build(&t, NULL, &tysh);
    CHECK(pc_tysh_set_text_index(tysh.data, tysh.len, 0, &x, NULL) == PC_OK);
    CHECK(pc_tysh_text_index(x.data, x.len, &has, &ix, NULL) == PC_OK && has && ix == 0);
    pc_psd_bytes_fini(&x);
    CHECK(pc_tysh_set_text_index(tysh.data, tysh.len, PC_TXT2_MAX_TEXT_INDEX, &x, NULL) == PC_OK);
    CHECK(pc_tysh_text_index(x.data, x.len, &has, &ix, NULL) == PC_OK && has && ix == PC_TXT2_MAX_TEXT_INDEX);
    pc_psd_bytes_fini(&x);
    for (k = 0; k < 2; k++) {
        pc_buf b = PC_BUF_INIT;
        pc_ed_value out;
        CHECK(pc_tysh_set_text_index(tysh.data, tysh.len, hostile[k], &x, NULL) == PC_OK);
        CHECK(pc_tysh_text_index(x.data, x.len, &has, &ix, NULL) == PC_OK && !has);
        pc_psd_bytes_fini(&x);
        build_txt2(&t, hostile[k], NULL, &b);
        txt2_parse_buf(&b, &out);
        CHECK(pc_ed_as_array(pc_ed_path(&out, "1", "1", (const char *)NULL), &n) != NULL && n == 0);
        pc_ed_fini(&out);
        pc_buf_fini(&b);
    }
    /* The bound is real but generous: the last honoured number lands at its slot. */
    {
        pc_buf b = PC_BUF_INIT;
        pc_ed_value out;
        build_txt2(&t, PC_TXT2_MAX_TEXT_INDEX, NULL, &b);
        txt2_parse_buf(&b, &out);
        CHECK(pc_ed_as_array(pc_ed_path(&out, "1", "1", (const char *)NULL), &n) != NULL &&
              n == (size_t)PC_TXT2_MAX_TEXT_INDEX + 1);
        pc_ed_fini(&out);
        pc_buf_fini(&b);
    }
    CHECK(pc_tysh_set_text_index((const uint8_t *)"xx", 2, 0, &x, NULL) == PC_ERR_FORMAT);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&t);
}

/* txt2_with_frame: one text object for `text` whose rendered frame is frame 0, and that frame. */
static void txt2_with_frame(pc_buf *v, const char *text, const double *pt, const double m[6])
{
    static const char tail[] = "] >> >> >> ] >>";
    static const size_t one = 0;
    const char *keys[1] = {"/11 1"};
    size_t lens[1];
    pc_buf frames = PC_BUF_INIT, out = PC_BUF_INIT;
    size_t at;
    (void)one;
    lens[0] = strlen(text) + 1;
    txt2_with_styles(v, text, lens, keys, 1);
    CHECK(v->len >= sizeof tail - 1 &&
          memcmp(v->data + v->len - (sizeof tail - 1), tail, sizeof tail - 1) == 0);
    pc_buf_truncate(v, v->len - (sizeof tail - 1));
    pc_buf_append_str(v, "] >> >> /1 << /2 [ << /6 [ << /10 0 >> ] >> ] >> >> ] >>", NULL);
    pc_buf_append_str(&frames, "/0 << /8 << /0 [ << /0 << ", NULL);
    if (pt != NULL) {
        pc_buf_appendf(&frames, NULL, "/0 [ %.5f %.5f ] ", pt[0], pt[1]);
    }
    pc_buf_appendf(&frames, NULL, "/2 << /2 [ %.5f %.5f %.5f %.5f %.5f %.5f ] >> >> >> ] >> >> ", m[0], m[1],
                   m[2], m[3], m[4], m[5]);
    for (at = 0; at + 9 <= v->len && memcmp(v->data + at, "/0 << >> ", 9) != 0; at++) {
    }
    CHECK(at + 9 <= v->len);
    pc_buf_append(&out, v->data, at, NULL);
    pc_buf_append(&out, frames.data, frames.len, NULL);
    pc_buf_append(&out, v->data + at + 9, v->len - at - 9, NULL);
    pc_buf_fini(v);
    *v = out;
    pc_buf_fini(&frames);
}

static pc_point origin_of(const pc_text_layer *l, double x, double y)
{
    pc_point p = {x, y};
    return pc_affine_apply(l->transform, p);
}

/* #1317: point text whose anchor lives only in its Txt2 frame is placed at that anchor. */
static void test_txt2_frame_places_point_text_at_its_anchor(void)
{
    static const double anchor[2] = {1000.0, 2100.0}, m[6] = {1.0, 0.0, 0.0, 1.0, 100.0, -50.0};
    static const float far_ink[4] = {1100.0f, 1980.0f, 1160.0f, 2050.0f},
                       near_ink[4] = {0.0f, -70.0f, 60.0f, 0.0f};
    pc_text_layer t, l;
    pc_psd_bytes tysh = {NULL, 0};
    pc_buf b = PC_BUF_INIT, b2 = PC_BUF_INIT;
    pc_ed_value txt2, other;
    pc_affine before;
    pc_point o;
    point(&t, "2", 100.0f);
    t.transform = pc_affine_translate(-1000.0, -2000.0);
    build(&t, far_ink, &tysh);
    with_text_index(&tysh, 0);
    txt2_with_frame(&b, "2", anchor, m);
    txt2_parse_buf(&b, &txt2);
    CHECK(import(&tysh, &l));
    CHECK(pc_txt2_apply_frame(&l, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    o = origin_of(&l, 0.0, 0.0);
    CHECK_NEAR(o.x, 100.0, 1e-3);
    CHECK_NEAR(o.y, 50.0, 1e-3);
    pc_text_layer_fini(&l);
    pc_psd_bytes_fini(&tysh);
    /* Already folded in (bounds near the origin): unchanged. */
    build(&t, near_ink, &tysh);
    with_text_index(&tysh, 0);
    CHECK(import(&tysh, &l));
    before = l.transform;
    CHECK(pc_txt2_apply_frame(&l, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    CHECK(memcmp(&before, &l.transform, sizeof before) == 0);
    pc_text_layer_fini(&l);
    pc_psd_bytes_fini(&tysh);
    /* Another text: unchanged. */
    build(&t, far_ink, &tysh);
    with_text_index(&tysh, 0);
    txt2_with_frame(&b2, "3", anchor, m);
    txt2_parse_buf(&b2, &other);
    CHECK(import(&tysh, &l));
    before = l.transform;
    CHECK(pc_txt2_apply_frame(&l, tysh.data, tysh.len, &other, NULL) == PC_OK);
    CHECK(memcmp(&before, &l.transform, sizeof before) == 0);
    pc_text_layer_fini(&l);
    pc_psd_bytes_fini(&tysh);
    pc_ed_fini(&txt2);
    pc_ed_fini(&other);
    pc_buf_fini(&b);
    pc_buf_fini(&b2);
    pc_text_layer_fini(&t);
}

/* with_bounds(tysh, r): the text descriptor's `bounds` set to r. */
static void with_bounds(pc_psd_bytes *b, const double r[4])
{
    static const char *const keys[4] = {"Left", "Top ", "Rght", "Btom"};
    pc_tysh t;
    pc_psd_desc d;
    pc_psd_value v;
    pc_psd_bytes out = {NULL, 0};
    size_t k, w = 0;
    CHECK(pc_tysh_parse(b->data, b->len, &t, NULL) == PC_OK);
    CHECK(pc_psd_desc_init(&d, "null", NULL) == PC_OK);
    for (k = 0; k < 4; k++) {
        pc_psd_value x = pc_psd_value_double(r[k]);
        CHECK(pc_psd_desc_add(&d, keys[k], &x, NULL) == PC_OK);
    }
    for (k = 0; k < t.text.n; k++) {
        if (pc_psd_id_is(&t.text.items[k].key, "bounds")) {
            pc_psd_id_fini(&t.text.items[k].key);
            pc_psd_value_fini(&t.text.items[k].value);
        } else {
            t.text.items[w++] = t.text.items[k];
        }
    }
    t.text.n = w;
    CHECK(pc_psd_value_object(&d, &v, NULL) == PC_OK);
    CHECK(pc_psd_desc_add(&t.text, "bounds", &v, NULL) == PC_OK);
    CHECK(pc_tysh_write(&t, &out, NULL) == PC_OK);
    pc_tysh_fini(&t);
    pc_psd_bytes_fini(b);
    *b = out;
}

/* #1317: a text box whose BoxBounds are in its frame's space is moved onto its bounds. */
static void test_txt2_frame_moves_box_text_onto_its_bounds(void)
{
    static const double r[4] = {960.0, 300.0, 1260.0, 390.0}, m[6] = {1.0, 0.0, 0.0, 1.0, 460.0, -300.0};
    pc_char_style s;
    pc_text_layer t, l;
    pc_psd_bytes built = {NULL, 0}, tysh = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    pc_ed_value txt2;
    pc_affine before;
    pc_point o;
    pc_char_style_default(&s);
    s.size_pt = 25.0f;
    styled(&t, "Casemiro 45", &s);
    t.shape.is_box = true;
    t.shape.x = 500.0f;
    t.shape.y = 600.0f;
    t.shape.width = 300.0f;
    t.shape.height = 90.0f;
    t.transform = pc_affine_translate(-700.0, -300.0);
    build(&t, NULL, &built);
    with_text_index(&built, 0);
    tysh.data = malloc(built.len);
    tysh.len = built.len;
    memcpy(tysh.data, built.data, built.len);
    with_bounds(&tysh, r);
    txt2_with_frame(&b, "Casemiro 45", NULL, m);
    txt2_parse_buf(&b, &txt2);
    CHECK(import(&tysh, &l));
    CHECK(l.shape.is_box);
    CHECK(pc_txt2_apply_frame(&l, tysh.data, tysh.len, &txt2, NULL) == PC_OK);
    o = origin_of(&l, (double)l.shape.x, (double)l.shape.y);
    CHECK_NEAR(o.x, 260.0, 1e-3);
    CHECK_NEAR(o.y, 0.0, 1e-3);
    pc_text_layer_fini(&l);
    /* PhotoCraft's own TySh (bounds = the box, frame already folded in): unchanged. */
    CHECK(import(&built, &l));
    before = l.transform;
    CHECK(pc_txt2_apply_frame(&l, built.data, built.len, &txt2, NULL) == PC_OK);
    CHECK(memcmp(&before, &l.transform, sizeof before) == 0);
    pc_text_layer_fini(&l);
    pc_ed_fini(&txt2);
    pc_buf_fini(&b);
    pc_psd_bytes_fini(&tysh);
    pc_psd_bytes_fini(&built);
    pc_text_layer_fini(&t);
    pc_char_style_fini(&s);
}

static void test_translate_tysh_moves_only_the_translation(void)
{
    static const float ink[4] = {0.0f, -20.0f, 300.0f, 4.0f};
    pc_text_layer t;
    pc_psd_bytes tysh = {NULL, 0}, moved = {NULL, 0};
    pc_tysh a, b;
    point(&t, "Hello!", 26.0f);
    t.transform.m[0] = 1.5;
    t.transform.m[3] = 1.5;
    t.transform.m[4] = 80.0;
    t.transform.m[5] = 340.0;
    build(&t, ink, &tysh);
    CHECK(pc_tysh_translate(tysh.data, tysh.len, -8.0, -279.0, &moved, NULL) == PC_OK);
    CHECK(pc_tysh_parse(moved.data, moved.len, &b, NULL) == PC_OK);
    CHECK(b.transform.m[0] == 1.5 && b.transform.m[1] == 0.0 && b.transform.m[2] == 0.0 &&
          b.transform.m[3] == 1.5 && b.transform.m[4] == 72.0 && b.transform.m[5] == 61.0);
    /* Everything but the 16 translation bytes is unchanged. */
    CHECK_EQ_UINT(moved.len, tysh.len);
    CHECK(memcmp(moved.data, tysh.data, 34) == 0 &&
          memcmp(moved.data + 50, tysh.data + 50, tysh.len - 50) == 0);
    CHECK(pc_tysh_parse(tysh.data, tysh.len, &a, NULL) == PC_OK);
    CHECK(memcmp(a.bounds, b.bounds, sizeof a.bounds) == 0); /* bounds are in text space */
    pc_tysh_fini(&a);
    pc_tysh_fini(&b);
    pc_psd_bytes_fini(&moved);
    CHECK(pc_tysh_translate(tysh.data, 40, 1.0, 1.0, &moved, NULL) == PC_ERR_FORMAT);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&t);
}

/* The four values after the warp descriptor are big-endian f32: zeros for unwarped type, the
 * text-space bounds for warped type. */
static void test_tysh_trailing_bounds_are_f32(void)
{
    static const float ink[4] = {0.0f, -34.316406f, 288.90625f, 15.058594f};
    pc_text_layer plain, warped;
    pc_psd_bytes a = {NULL, 0}, b = {NULL, 0}, w = {NULL, 0};
    pc_tysh t;
    size_t k;
    point(&plain, "Type", 20.0f);
    build(&plain, ink, &a);
    for (k = 0; k < 4; k++) {
        CHECK(pc_f32_from_bits(pc_load_be32(a.data + a.len - 16 + 4 * k)) == 0.0f);
    }
    point(&warped, "Type", 20.0f);
    warped.has_warp = true;
    CHECK(pc_str_set_c(&warped.warp.style, "warpArc", NULL) == PC_OK);
    warped.warp.value = 50.0f;
    warped.warp.horizontal = true;
    build(&warped, ink, &b);
    for (k = 0; k < 4; k++) {
        CHECK(pc_f32_from_bits(pc_load_be32(b.data + b.len - 16 + 4 * k)) == ink[k]);
    }
    CHECK(pc_tysh_parse(b.data, b.len, &t, NULL) == PC_OK);
    CHECK(memcmp(t.bounds, ink, sizeof ink) == 0);
    CHECK(pc_tysh_write(&t, &w, NULL) == PC_OK && w.len == b.len && memcmp(w.data, b.data, b.len) == 0);
    pc_tysh_fini(&t);
    pc_psd_bytes_fini(&w);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
    pc_text_layer_fini(&plain);
    pc_text_layer_fini(&warped);
}

/* EngineData's AntiAlias numbers: 0 None, 1 Crisp, 2 Strong, 3 Smooth, 4 Sharp; read back from
 * EngineData alone (no AntA), every mode survives. */
static void test_engine_data_antialias_numbers_match_photoshop(void)
{
    static const pc_antialias modes[5] = {PC_AA_NONE, PC_AA_CRISP, PC_AA_STRONG, PC_AA_SMOOTH, PC_AA_SHARP};
    size_t m;
    for (m = 0; m < 5; m++) {
        pc_text_layer t, back;
        pc_psd_bytes b = {NULL, 0}, w = {NULL, 0};
        pc_tysh ty;
        pc_ed_value e;
        double n = -1.0;
        size_t r, k = 0;
        point(&t, "Type", 20.0f);
        t.antialias = modes[m];
        build(&t, NULL, &b);
        CHECK(pc_tysh_parse(b.data, b.len, &ty, NULL) == PC_OK);
        CHECK(pc_tysh_engine_data(&ty.text, &e, NULL) == PC_OK);
        CHECK(pc_ed_as_f64(pc_ed_path(&e, "EngineDict", "AntiAlias", (const char *)NULL), &n) &&
              n == (double)m);
        pc_ed_fini(&e);
        for (r = 0; r < ty.text.n; r++) {
            if (pc_psd_id_is(&ty.text.items[r].key, "AntA")) {
                pc_psd_id_fini(&ty.text.items[r].key);
                pc_psd_value_fini(&ty.text.items[r].value);
            } else {
                ty.text.items[k++] = ty.text.items[r];
            }
        }
        ty.text.n = k;
        CHECK(pc_tysh_write(&ty, &w, NULL) == PC_OK);
        CHECK(import(&w, &back));
        CHECK_EQ_INT((int)back.antialias, (int)modes[m]);
        pc_text_layer_fini(&back);
        pc_tysh_fini(&ty);
        pc_psd_bytes_fini(&w);
        pc_psd_bytes_fini(&b);
        pc_text_layer_fini(&t);
    }
}

/* Hostile Txt2 blocks: frames and builds never crash. */
static void test_txt2_hostile(void)
{
    static const char *const blocks[] = {
        "/1 << /1 [ << /0 << /0 (\xfe\xff\0x\0\r) >> /1 << /2 [ << /6 [ << /10 -1 >> ] >> ] >> >> ] >>",
        "/1 << /1 [ << /0 << /0 (\xfe\xff\0x\0\r) >> /1 << /2 [ << /6 [ << /10 99 >> ] >> ] >> >> ] >>",
        ("/0 << /8 << /0 [ << /0 << /2 << /2 [ 0 0 0 0 0 0 ] >> >> >> ] >> >> /1 << /1 [ << /1 << /2 [ << /6 "
         "[ << /10 0 >> ] >> ] >> >> ] >>"),
        "/1 << /1 [ 1 2 3 ] >>",
        "/1 5",
    };
    pc_text_layer t;
    pc_psd_bytes tysh = {NULL, 0};
    size_t k;
    point(&t, "x", 12.0f);
    build(&t, NULL, &tysh);
    with_text_index(&tysh, 0);
    for (k = 0; k < sizeof blocks / sizeof blocks[0]; k++) {
        pc_ed_value v;
        pc_buf out = PC_BUF_INIT;
        pc_txt2_object o = {0, &t};
        if (pc_txt2_parse((const uint8_t *)blocks[k], strlen(blocks[k]), &v, NULL) == PC_OK) {
            CHECK(pc_txt2_apply_frame(&t, tysh.data, tysh.len, &v, NULL) == PC_OK);
            CHECK(pc_txt2_apply(&t, tysh.data, tysh.len, &v, NULL) == PC_OK);
            pc_ed_fini(&v);
        }
        CHECK(pc_txt2_build(&o, 1, (const uint8_t *)blocks[k], strlen(blocks[k]), true, &out, NULL) == PC_OK);
        pc_buf_fini(&out);
    }
    CHECK(pc_txt2_apply_frame(&t, (const uint8_t *)"abc", 3, NULL, NULL) == PC_OK);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&t);
}

int main(int argc, char **argv)
{
    RUN(test_psd_round_trips_tabs);
    RUN(test_txt2_carries_opentype_features);
    RUN(test_build_txt2_round_trips_kerning_modes);
    RUN(test_needs_txt2_only_for_optical);
    RUN(test_txt2_keeps_a_kept_objects_extras_and_drops_them_with_its_text);
    RUN(test_unchanged_txt2_is_written_back_byte_for_byte);
    RUN(test_edited_txt2_keeps_the_files_styles_and_objects);
    RUN(test_txt2_runs_that_dont_cover_the_text_are_regenerated);
    RUN(test_hostile_text_index_is_ignored_not_sized);
    RUN(test_txt2_frame_places_point_text_at_its_anchor);
    RUN(test_txt2_frame_moves_box_text_onto_its_bounds);
    RUN(test_translate_tysh_moves_only_the_translation);
    RUN(test_tysh_trailing_bounds_are_f32);
    RUN(test_engine_data_antialias_numbers_match_photoshop);
    RUN(test_txt2_hostile);
    return pc_t_finish();
}
