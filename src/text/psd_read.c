/* PSD type layers, reading side: TySh parsing/writing, EngineData -> pc_text_layer, Txt2 (port
 * of the reading half of crates/text/src/psd.rs). See psd.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/fonts.h"
#include "text/psd_internal.h"

/* ---- small shared helpers ---- */

uint32_t psd_next_char(const uint8_t *s, size_t len, size_t *i)
{
    uint32_t cp;
    if (pc_utf8_next(s, len, i, &cp)) {
        return cp;
    }
    (*i)++;
    return 0xFFFD;
}

pc_status psd_replace_byte(const char *s, size_t len, char from, char to, pc_str *out, pc_error *err)
{
    pc_str r = PC_STR_INIT;
    size_t k;
    pc_status st = pc_str_set(&r, s, len, err);
    if (st != PC_OK) {
        return st;
    }
    for (k = 0; k < r.len; k++) {
        if (r.ptr[k] == from) {
            r.ptr[k] = to;
        }
    }
    *out = r;
    return PC_OK;
}

void psd_names_fini(psd_names *l)
{
    size_t k;
    for (k = 0; k < l->n; k++) {
        pc_str_fini(&l->v[k]);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

pc_status psd_names_push(psd_names *l, const char *s, size_t len, pc_error *err)
{
    pc_str v = PC_STR_INIT;
    pc_status st;
    if (l->n == l->cap) {
        size_t cap = l->cap;
        pc_str *g = pc_grow_array(l->v, &cap, l->n + 1, sizeof *g);
        if (g == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
        l->v = g;
        l->cap = cap;
    }
    st = pc_str_set(&v, s, len, err);
    if (st != PC_OK) {
        return st;
    }
    l->v[l->n++] = v;
    return PC_OK;
}

size_t psd_names_find(const psd_names *l, const char *s, size_t len)
{
    size_t k;
    for (k = 0; k < l->n; k++) {
        if (pc_str_eq_mem(&l->v[k], s, len)) {
            return k;
        }
    }
    return l->n;
}

pc_status psd_names_of(const pc_ed_value *set, psd_names *out, pc_error *err)
{
    psd_names l = {NULL, 0, 0};
    size_t n = 0, k;
    const pc_ed_value *a = pc_ed_as_array(set, &n);
    for (k = 0; a != NULL && k < n; k++) {
        const pc_str *name = pc_ed_as_str(pc_ed_get(&a[k], "Name"));
        pc_status st =
            psd_names_push(&l, name != NULL ? pc_str_c(name) : "", name != NULL ? name->len : 0, err);
        if (st != PC_OK) {
            psd_names_fini(&l);
            return st;
        }
    }
    *out = l;
    return PC_OK;
}

pc_status psd_names_index(psd_names *l, const pc_str *s, size_t *out, pc_error *err)
{
    size_t i = psd_names_find(l, pc_str_c(s), s->len);
    if (i == l->n) {
        pc_status st = psd_names_push(l, pc_str_c(s), s->len, err);
        if (st != PC_OK) {
            return st;
        }
    }
    *out = i;
    return PC_OK;
}

float psd_k_of(float dpi)
{
    return 72.0f / (isfinite(dpi) && dpi > 0.0f ? dpi : 72.0f);
}

pc_status psd_arr_f(const pc_ed_value *v, double **out, size_t *n, pc_error *err)
{
    size_t len = 0, k, m = 0;
    const pc_ed_value *a = pc_ed_as_array(v, &len);
    double *r = NULL;
    if (a != NULL && len > 0) {
        r = pc_malloc_array(len, sizeof *r);
        if (r == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
        for (k = 0; k < len; k++) {
            if (pc_ed_as_f64(&a[k], &r[m])) {
                m++;
            }
        }
    }
    *out = r;
    *n = m;
    return PC_OK;
}

void pc_text_utf16_to_byte_lengths(const char *text, size_t len, const double *lens, size_t n, size_t *out)
{
    const uint8_t *s = (const uint8_t *)text;
    size_t i = 0, k;
    for (k = 0; k < n; k++) {
        /* `l.max(0.0) as usize`: NaN and negatives are 0, huge values saturate. */
        size_t units = pc_f64_to_size(lens[k] > 0.0 ? lens[k] : 0.0);
        size_t bytes = 0;
        while (units > 0 && i < len) {
            size_t at = i;
            uint32_t cp = psd_next_char(s, len, &i);
            size_t u = psd_utf16_len(cp);
            units = units > u ? units - u : 0;
            bytes += i - at;
        }
        out[k] = bytes;
    }
}

pc_status psd_byte_lengths(const pc_str *text, const pc_ed_value *lens_value, size_t **out, size_t *n,
                           pc_error *err)
{
    double *lens;
    size_t nl;
    size_t *r = NULL;
    pc_status st = psd_arr_f(lens_value, &lens, &nl, err);
    if (st != PC_OK) {
        return st;
    }
    if (nl > 0) {
        r = pc_malloc_array(nl, sizeof *r);
        if (r == NULL) {
            free(lens);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
        pc_text_utf16_to_byte_lengths(pc_str_c(text), text->len, lens, nl, r);
    }
    free(lens);
    *out = r;
    *n = nl;
    return PC_OK;
}

/* Keeps a nested call's error text only when it is a resource failure; "None" outcomes leave
 * the caller's error untouched. */
static pc_status forward(pc_error *err, const pc_error *tmp, pc_status st)
{
    if (st == PC_ERR_NOMEM && err != NULL) {
        *err = *tmp;
    }
    return st;
}

/* ---- TySh ---- */

void pc_tysh_fini(pc_tysh *t)
{
    if (t == NULL) {
        return;
    }
    pc_psd_desc_fini(&t->text);
    pc_psd_desc_fini(&t->warp);
    memset(t, 0, sizeof *t);
}

pc_status pc_tysh_parse(const uint8_t *data, size_t len, pc_tysh *out, pc_error *err)
{
    pc_tysh t;
    pc_error tmp;
    size_t used = 0, wused = 0, rlen, k;
    const uint8_t *rest;
    pc_status st;
    memset(&t, 0, sizeof t);
    if (data == NULL || len < 52) {
        return pc_fail(err, PC_ERR_FORMAT, "invalid TySh block");
    }
    for (k = 0; k < 6; k++) {
        t.transform.m[k] = pc_f64_from_bits(pc_load_be64(data + 2 + 8 * k));
    }
    st = pc_psd_vdesc_parse_prefix(data + 52, len - 52, &t.text, &used, &tmp);
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? forward(err, &tmp, st)
                                  : pc_fail(err, PC_ERR_FORMAT, "invalid TySh block");
    }
    rest = data + 52 + used;
    rlen = len - 52 - used;
    if (rlen >= 2) {
        size_t n = 0;
        st = pc_psd_vdesc_parse_prefix(rest + 2, rlen - 2, &t.warp, &n, &tmp);
        if (st == PC_OK) {
            t.has_warp = true;
            wused = n + 2;
        } else if (st == PC_ERR_NOMEM) {
            pc_psd_desc_fini(&t.text);
            return forward(err, &tmp, st);
        }
    }
    for (k = 0; k < 4; k++) {
        if (wused + k * 4 + 4 <= rlen) {
            t.bounds[k] = pc_f32_from_bits(pc_load_be32(rest + wused + k * 4));
        }
    }
    *out = t;
    return PC_OK;
}

static pc_status set_enum(pc_psd_value *v, const char *type_id, const char *value, pc_error *err)
{
    return pc_psd_value_enum(type_id, value, v, err);
}

/* default_warp: no warp, horizontal. */
static pc_status default_warp(pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "warp", err);
    if (st != PC_OK) {
        return st;
    }
    st = set_enum(&v, "warpStyle", "warpNone", err);
    if (st == PC_OK) {
        st = pc_psd_desc_add(&d, "warpStyle", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_double(0.0);
        st = pc_psd_desc_add(&d, "warpValue", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_double(0.0);
        st = pc_psd_desc_add(&d, "warpPerspective", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_double(0.0);
        st = pc_psd_desc_add(&d, "warpPerspectiveOther", &v, err);
    }
    if (st == PC_OK) {
        st = set_enum(&v, "Ornt", "Hrzn", err);
    }
    if (st == PC_OK) {
        st = pc_psd_desc_add(&d, "warpRotate", &v, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status pc_tysh_write(const pc_tysh *t, pc_psd_bytes *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_psd_bytes text = {NULL, 0}, warp = {NULL, 0};
    pc_psd_desc dw;
    uint8_t tmp[8];
    size_t k;
    pc_status st;
    size_t n;
    st = pc_psd_vdesc_to_bytes(&t->text, &text, err);
    if (st == PC_OK) {
        if (t->has_warp) {
            st = pc_psd_vdesc_to_bytes(&t->warp, &warp, err);
        } else {
            st = default_warp(&dw, err);
            if (st == PC_OK) {
                st = pc_psd_vdesc_to_bytes(&dw, &warp, err);
                pc_psd_desc_fini(&dw);
            }
        }
    }
    if (st == PC_OK) {
        pc_store_be16(tmp, 1);
        st = pc_buf_append(&b, tmp, 2, err);
    }
    for (k = 0; k < 6 && st == PC_OK; k++) {
        pc_store_be64(tmp, pc_f64_to_bits(t->transform.m[k]));
        st = pc_buf_append(&b, tmp, 8, err);
    }
    if (st == PC_OK) {
        pc_store_be16(tmp, 50);
        st = pc_buf_append(&b, tmp, 2, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, text.data, text.len, err);
    }
    if (st == PC_OK) {
        pc_store_be16(tmp, 1);
        st = pc_buf_append(&b, tmp, 2, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, warp.data, warp.len, err);
    }
    for (k = 0; k < 4 && st == PC_OK; k++) {
        pc_store_be32(tmp, pc_f32_to_bits(t->bounds[k]));
        st = pc_buf_append(&b, tmp, 4, err);
    }
    pc_psd_bytes_fini(&text);
    pc_psd_bytes_fini(&warp);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    out->data = pc_buf_take(&b, &n);
    out->len = n;
    return PC_OK;
}

/* enum_value: the value id of an enumerated item, lossy UTF-8 (NULL-free buffer in *out).
 * False when the item is missing or not an enumeration. */
static pc_status enum_value(const pc_psd_desc *d, const char *key, bool *has, pc_str *out, pc_error *err)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    const uint8_t *b;
    size_t n = 0, len;
    pc_buf buf = PC_BUF_INIT;
    uint8_t *data;
    pc_status st;
    *has = false;
    if (v == NULL || v->kind != PC_PSD_V_ENUMERATED) {
        return PC_OK;
    }
    b = pc_psd_id_bytes(&v->value, &n);
    st = pc_utf8_lossy(b, n, &buf, err);
    if (st != PC_OK) {
        pc_buf_fini(&buf);
        return st;
    }
    data = pc_buf_take(&buf, &len);
    *out = data != NULL ? pc_str_adopt((char *)data, len) : (pc_str)PC_STR_INIT;
    *has = true;
    return PC_OK;
}

/* Whether enum_value(d, key) == Some(want). */
static bool enum_is(const pc_psd_desc *d, const char *key, const char *want)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    const uint8_t *b;
    size_t n = 0;
    if (v == NULL || v->kind != PC_PSD_V_ENUMERATED) {
        return false;
    }
    b = pc_psd_id_bytes(&v->value, &n);
    /* Lossy decoding changes only invalid UTF-8, which never equals an ASCII literal. */
    return n == strlen(want) && (n == 0 || memcmp(b, want, n) == 0);
}

bool psd_dnum(const pc_psd_value *v, double *out)
{
    if (v == NULL) {
        return false;
    }
    if (v->kind == PC_PSD_V_DOUBLE || v->kind == PC_PSD_V_UNIT_FLOAT) {
        *out = v->d;
        return true;
    }
    if (v->kind == PC_PSD_V_INTEGER) {
        *out = (double)v->i;
        return true;
    }
    return false;
}

pc_status pc_tysh_engine_data(const pc_psd_desc *text, pc_ed_value *out, pc_error *err)
{
    const pc_psd_value *v = pc_psd_desc_get(text, "EngineData");
    pc_error tmp;
    pc_status st;
    if (v == NULL || v->kind != PC_PSD_V_RAW_DATA) {
        return pc_fail(err, PC_ERR_FORMAT, "no EngineData");
    }
    st = pc_ed_parse(v->raw.data, v->raw.len, out, &tmp);
    if (st != PC_OK && err != NULL) {
        *err = tmp;
    }
    return st == PC_OK || st == PC_ERR_NOMEM ? st : PC_ERR_FORMAT;
}

/* parse_tysh + engine_data. */
static pc_status tysh_engine(const uint8_t *data, size_t len, pc_tysh *t, pc_ed_value *e, pc_error *err)
{
    pc_status st = pc_tysh_parse(data, len, t, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_tysh_engine_data(&t->text, e, err);
    if (st != PC_OK) {
        pc_tysh_fini(t);
    }
    return st;
}

pc_status pc_tysh_engine_text_is_blank(const uint8_t *tysh, size_t len, bool *out, pc_error *err)
{
    pc_tysh t;
    pc_ed_value e;
    const pc_str *s;
    size_t start = 0;
    pc_error tmp;
    pc_status st = tysh_engine(tysh, len, &t, &e, &tmp);
    if (st == PC_ERR_NOMEM) {
        return forward(err, &tmp, st);
    }
    if (st != PC_OK) {
        *out = false;
        return PC_OK;
    }
    s = pc_ed_as_str(pc_ed_path(&e, "EngineDict", "Editor", "Text", (const char *)NULL));
    *out = s != NULL && pc_utf8_trim(pc_str_c(s), s->len, &start) == 0;
    pc_ed_fini(&e);
    pc_tysh_fini(&t);
    return PC_OK;
}

/* ---- run lists ---- */

/* ---- EngineData -> model ---- */

static const pc_ed_value *lookup(const pc_ed_value *run, const pc_ed_value *base, const char *key)
{
    const pc_ed_value *v = pc_ed_get(run, key);
    return v != NULL ? v : pc_ed_get(base, key);
}

static bool num(const pc_ed_value *run, const pc_ed_value *base, const char *key, double *out)
{
    return pc_ed_as_f64(lookup(run, base, key), out);
}

/* flag(): -1 None, 0 false, 1 true. */
static int flag(const pc_ed_value *run, const pc_ed_value *base, const char *key)
{
    bool b;
    if (!pc_ed_as_bool(lookup(run, base, key), &b)) {
        return -1;
    }
    return b ? 1 : 0;
}

static float f32_of(double v)
{
    return pc_f64_to_f32(v);
}

pc_status psd_char_style(const pc_ed_value *base, const pc_ed_value *d, const psd_names *fonts, float k,
                         pc_char_style *out, pc_error *err)
{
    pc_char_style s;
    pc_status st = PC_OK;
    int64_t i;
    double v;
    size_t f;
    const pc_ed_value *c;
    pc_char_style_default(&s);
    if (pc_ed_as_i64(lookup(d, base, "Font"), &i) && (size_t)i < fonts->n) {
        const pc_str *ps = &fonts->v[(size_t)i];
        pc_resolved_font r;
        st = pc_text_guess_from_postscript(pc_str_c(ps), ps->len, &r, err);
        if (st == PC_OK) {
            st = pc_str_set_c(&s.font_family, r.family != NULL ? r.family : "", err);
            s.weight = r.weight;
            s.italic = r.italic;
            pc_resolved_font_fini(&r);
        }
        if (st == PC_OK) {
            st = pc_str_clone(ps, &s.postscript_name, err);
            s.has_postscript_name = st == PC_OK;
        }
    }
    if (num(d, base, "FontSize", &v)) {
        s.size_pt = f32_of(v) * k;
    }
    if (flag(d, base, "AutoLeading") == 0 && num(d, base, "Leading", &v)) {
        s.has_leading = true;
        s.leading_pt = f32_of(v) * k;
    }
    s.tracking = num(d, base, "Tracking", &v) ? f32_of(v) : 0.0f;
    s.baseline_shift_pt = (num(d, base, "BaselineShift", &v) ? f32_of(v) : 0.0f) * k;
    s.horizontal_scale = num(d, base, "HorizontalScale", &v) ? f32_of(v) : 1.0f;
    s.vertical_scale = num(d, base, "VerticalScale", &v) ? f32_of(v) : 1.0f;
    s.faux_bold = flag(d, base, "FauxBold") == 1;
    s.faux_italic = flag(d, base, "FauxItalic") == 1;
    s.underline = flag(d, base, "Underline") == 1;
    s.strikethrough = flag(d, base, "Strikethrough") == 1;
    s.ligatures = flag(d, base, "Ligatures") != 0;
    s.discretionary_ligatures = flag(d, base, "DLigatures") == 1;
    for (f = 0; f < PC_TEXT_OPENTYPE_KEY_COUNT && st == PC_OK; f++) {
        if (flag(d, base, PC_TEXT_OPENTYPE_KEYS[f][0]) == 1) {
            pc_font_feature *g = realloc(s.features, (s.nfeatures + 1) * sizeof *g);
            if (g == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
                break;
            }
            s.features = g;
            memset(&g[s.nfeatures], 0, sizeof *g);
            st = pc_str_set_c(&g[s.nfeatures].tag, PC_TEXT_OPENTYPE_KEYS[f][1], err);
            g[s.nfeatures].value = 1;
            s.nfeatures++;
        }
    }
    s.kerning = flag(d, base, "AutoKerning") == 0 ? PC_KERNING_OFF : PC_KERNING_METRICS;
    s.kern = num(d, base, "Kerning", &v) && isfinite(v) ? f32_of(v) : 0.0f;
    s.caps = PC_CAPS_NORMAL;
    if (pc_ed_as_i64(lookup(d, base, "FontCaps"), &i)) {
        s.caps = i == 1 ? PC_CAPS_SMALL_CAPS : i == 2 ? PC_CAPS_ALL_CAPS : PC_CAPS_NORMAL;
    }
    c = lookup(d, base, "FillColor");
    if (c != NULL && st == PC_OK) {
        double *vals;
        size_t nv;
        st = psd_arr_f(pc_ed_get(c, "Values"), &vals, &nv, err);
        if (st == PC_OK) {
            float a = nv > 0 ? f32_of(vals[0]) : 1.0f;
            if (nv == 5) {
                s.color.mode = PC_COLOR_MODE_CMYK;
                s.color.c[0] = f32_of(vals[1]);
                s.color.c[1] = f32_of(vals[2]);
                s.color.c[2] = f32_of(vals[3]);
                s.color.c[3] = f32_of(vals[4]);
                s.color.alpha = a;
            } else if (nv == 2) {
                s.color.mode = PC_COLOR_MODE_GRAYSCALE;
                s.color.c[0] = f32_of(vals[1]);
                s.color.c[1] = s.color.c[2] = s.color.c[3] = 0.0f;
                s.color.alpha = a;
            } else if (nv == 4) {
                s.color = pc_color_rgba(f32_of(vals[1]), f32_of(vals[2]), f32_of(vals[3]), a);
            } else {
                s.color = PC_COLOR_BLACK;
            }
            free(vals);
        }
    }
    if (st != PC_OK) {
        pc_char_style_fini(&s);
        return st;
    }
    *out = s;
    return PC_OK;
}

pc_paragraph_style psd_para_style(const pc_ed_value *base, const pc_ed_value *d, float k)
{
    pc_paragraph_style p = pc_paragraph_style_default();
    double v;
    p.align = PC_TEXT_ALIGN_LEFT;
    if (num(d, base, "Justification", &v)) {
        switch (pc_f64_to_i64(v)) {
        case 1:
            p.align = PC_TEXT_ALIGN_RIGHT;
            break;
        case 2:
            p.align = PC_TEXT_ALIGN_CENTER;
            break;
        case 3:
            p.align = PC_TEXT_ALIGN_JUSTIFY_LEFT;
            break;
        case 4:
            p.align = PC_TEXT_ALIGN_JUSTIFY_RIGHT;
            break;
        case 5:
            p.align = PC_TEXT_ALIGN_JUSTIFY_CENTER;
            break;
        case 6:
            p.align = PC_TEXT_ALIGN_JUSTIFY_ALL;
            break;
        default:
            break;
        }
    }
    p.first_line_indent_pt = (num(d, base, "FirstLineIndent", &v) ? f32_of(v) : 0.0f) * k;
    p.start_indent_pt = (num(d, base, "StartIndent", &v) ? f32_of(v) : 0.0f) * k;
    p.end_indent_pt = (num(d, base, "EndIndent", &v) ? f32_of(v) : 0.0f) * k;
    p.space_before_pt = (num(d, base, "SpaceBefore", &v) ? f32_of(v) : 0.0f) * k;
    p.space_after_pt = (num(d, base, "SpaceAfter", &v) ? f32_of(v) : 0.0f) * k;
    p.auto_leading = num(d, base, "AutoLeading", &v) ? f32_of(v) : f32_of(1.2);
    p.hyphenate = flag(d, base, "AutoHyphenate") == 1;
    return p;
}

/* Whether two styles differ at most in their kerning. */
static bool same_but_kerning(const pc_char_style *a, const pc_char_style *b)
{
    pc_char_style t = *a; /* shallow: only compared */
    t.kerning = b->kerning;
    t.kern = b->kern;
    return pc_char_style_eq(&t, b);
}

typedef struct incoming {
    size_t len;
    bool autok;
    float kern;
} incoming;

/* kerning_from_pairs: reads EngineData's pair fields back into per-character kerning (see the
 * notes in psd_write.c). Takes over *runs (freed); the result goes to *out. */
static pc_status kerning_from_pairs(const pc_str *text, pc_text_run *runs, size_t nruns, const incoming *inc,
                                    size_t ninc, psd_run_list *out, pc_error *err)
{
    const uint8_t *s = (const uint8_t *)pc_str_c(text);
    size_t *starts = NULL, nstarts = 0, n, j, si = 0, seg_end, ri = 0, run_end, c;
    float *kern = NULL;
    bool *off = NULL;
    psd_run_list r = {NULL, 0, 0};
    pc_status st = PC_OK;
    if (text->len == 0 || nruns == 0) {
        out->v = runs;
        out->n = out->cap = nruns;
        return PC_OK;
    }
    starts = pc_malloc_array(text->len + 1, sizeof *starts);
    kern = pc_calloc_array(text->len, sizeof *kern);
    off = pc_calloc_array(text->len, sizeof *off);
    if (starts == NULL || kern == NULL || off == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        goto cleanup;
    }
    for (j = 0; j < text->len;) {
        starts[nstarts++] = j;
        (void)psd_next_char(s, text->len, &j);
    }
    starts[nstarts++] = text->len;
    n = nstarts - 1;
    seg_end = ninc > 0 ? inc[0].len : 0;
    for (j = 0; j < nstarts; j++) {
        size_t b = starts[j];
        while (b >= seg_end && si < ninc) {
            si++;
            if (si < ninc) {
                seg_end = seg_end > SIZE_MAX - inc[si].len ? SIZE_MAX : seg_end + inc[si].len;
            }
        }
        if (si >= ninc) {
            break;
        }
        if (inc[si].kern != 0.0f && j >= 1 && j - 1 < n) {
            kern[j - 1] = inc[si].kern;
            off[j - 1] = true;
        }
        if (!inc[si].autok && j >= 2 && j - 2 < n) {
            off[j - 2] = true;
        }
    }
    run_end = runs[0].len;
    for (c = 0; c + 1 < nstarts && st == PC_OK; c++) {
        size_t b = starts[c], len = starts[c + 1] - starts[c];
        float k;
        pc_kerning kerning;
        while (b >= run_end && ri + 1 < nruns) {
            ri++;
            run_end = run_end > SIZE_MAX - runs[ri].len ? SIZE_MAX : run_end + runs[ri].len;
        }
        k = kern[c];
        kerning = off[c] ? PC_KERNING_OFF : PC_KERNING_METRICS;
        if (r.n > 0 && r.v[r.n - 1].style.kern == k && r.v[r.n - 1].style.kerning == kerning &&
            same_but_kerning(&r.v[r.n - 1].style, &runs[ri].style)) {
            r.v[r.n - 1].len += len;
        } else {
            pc_char_style style;
            st = pc_char_style_clone(&runs[ri].style, &style, err);
            if (st == PC_OK) {
                style.kerning = kerning;
                style.kern = k;
                st = psd_run_list_push(&r, len, &style, err);
                if (st != PC_OK) {
                    pc_char_style_fini(&style);
                }
            }
        }
    }
cleanup:
    free(starts);
    free(kern);
    free(off);
    pc_text_runs_free(runs, nruns);
    if (st != PC_OK) {
        psd_run_list_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

/* Box shape from the first rendered shape (ShapeType 1 with four BoxBounds). */
static pc_status read_shape(const pc_ed_value *e, pc_text_layer *layer, pc_error *err)
{
    size_t n = 0, nb;
    const pc_ed_value *children =
        pc_ed_as_array(pc_ed_path(e, "EngineDict", "Rendered", "Shapes", "Children", (const char *)NULL), &n);
    int64_t type;
    double *b;
    pc_status st;
    if (children == NULL || n == 0 || !pc_ed_as_i64(pc_ed_get(&children[0], "ShapeType"), &type) ||
        type != 1) {
        return PC_OK;
    }
    st = psd_arr_f(pc_ed_path(&children[0], "Cookie", "Photoshop", "BoxBounds", (const char *)NULL), &b, &nb,
                   err);
    if (st != PC_OK) {
        return st;
    }
    if (nb == 4) {
        layer->shape.kind = PC_TEXT_SHAPE_BOX;
        layer->shape.x = f32_of(b[0]);
        layer->shape.y = f32_of(b[1]);
        layer->shape.width = f32_of(b[2] - b[0]);
        layer->shape.height = f32_of(b[3] - b[1]);
    }
    free(b);
    return PC_OK;
}

/* The dictionaries in `levels` (NULL entries skipped, non-dictionaries ignored) as one: a key comes
 * from the first level that has it. */
static pc_status merged(const pc_ed_value *const *levels, size_t n, pc_ed_value *out, pc_error *err)
{
    pc_ed_value d = pc_ed_dict();
    size_t l, k, q;
    for (l = 0; l < n; l++) {
        const pc_ed_value *lv = levels[l];
        if (lv == NULL || lv->kind != PC_ED_DICT) {
            continue;
        }
        for (k = 0; k < lv->n; k++) {
            const pc_ed_entry *e = &lv->entries[k];
            bool have = false;
            pc_ed_entry *g;
            for (q = 0; q < d.n && !have; q++) {
                have = d.entries[q].key.len == e->key.len &&
                       memcmp(pc_str_c(&d.entries[q].key), pc_str_c(&e->key), e->key.len) == 0;
            }
            if (have) {
                continue;
            }
            g = d.n >= d.cap || d.entries == NULL ? pc_grow_array(d.entries, &d.cap, d.n + 1, sizeof *g)
                                                  : d.entries;
            if (g == NULL) {
                pc_ed_fini(&d);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
            }
            d.entries = g;
            memset(&d.entries[d.n], 0, sizeof d.entries[d.n]);
            if (pc_str_clone(&e->key, &d.entries[d.n].key, err) != PC_OK) {
                pc_ed_fini(&d);
                return PC_ERR_NOMEM;
            }
            if (pc_ed_clone(&e->value, &d.entries[d.n].value, err) != PC_OK) {
                pc_str_fini(&d.entries[d.n].key);
                pc_ed_fini(&d);
                return PC_ERR_NOMEM;
            }
            d.n++;
        }
    }
    *out = d;
    return PC_OK;
}

/* The `sub` value of every element of run["RunArray"] that has one (filter_map). */
static pc_status run_items(const pc_ed_value *run, const char *sub, const pc_ed_value ***out, size_t *n,
                           pc_error *err)
{
    size_t len = 0, k, m = 0;
    const pc_ed_value *arr = pc_ed_as_array(pc_ed_get(run, "RunArray"), &len);
    const pc_ed_value **r = NULL;
    if (arr != NULL && len > 0) {
        r = pc_malloc_array(len, sizeof *r);
        if (r == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
        for (k = 0; k < len; k++) {
            const pc_ed_value *x = pc_ed_get(&arr[k], sub);
            if (x != NULL) {
                r[m++] = x;
            }
        }
    }
    *out = r;
    *n = m;
    return PC_OK;
}

/* A dictionary key holding a usize (an Int, or a Real cast like Rust `as`, within range). */
static bool ed_index(const pc_ed_value *d, const char *key, size_t *out)
{
    int64_t i;
    if (!pc_ed_as_i64(pc_ed_get(d, key), &i) || i < 0) {
        return false;
    }
    *out = (size_t)i;
    return true;
}

/* The sheet of `set` a run's sheet names: by its `Parent` index (when in range), else by `Name`. */
static const pc_ed_value *named_sheet(const pc_ed_value *set, size_t nset, const pc_ed_value *sheet)
{
    size_t i;
    const pc_str *name;
    if (ed_index(sheet, "Parent", &i) && i < nset) {
        return &set[i];
    }
    name = pc_ed_as_str(pc_ed_get(sheet, "Name"));
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < nset; i++) {
        const pc_str *n = pc_ed_as_str(pc_ed_get(&set[i], "Name"));
        if (n != NULL && n->len == name->len && memcmp(pc_str_c(n), pc_str_c(name), n->len) == 0) {
            return &set[i];
        }
    }
    return NULL;
}

/* The normal sheet: set[res[key]] when in range, else the first. */
static const pc_ed_value *normal_sheet(const pc_ed_value *res, const char *key, const pc_ed_value *set,
                                       size_t nset)
{
    size_t i;
    if (ed_index(res, key, &i) && i < nset) {
        return &set[i];
    }
    return nset > 0 ? &set[0] : NULL;
}

typedef struct para_base {
    pc_ed_value base;            /* merged paragraph properties */
    const pc_ed_value *char_def; /* the paragraph's default character style, or NULL */
} para_base;

/* Style resolution (#3795). A run's own `StyleSheetData` / `Properties` hold only what it
 * overrides; the rest comes from its sheets, as in Photoshop:
 * - a paragraph run: its `Properties` -> the `ParagraphSheetSet` entry its `Name` names (or its
 *   `Parent` index) -> `TheNormalParagraphSheet`;
 * - a character run: its `StyleSheetData` -> the `StyleSheetSet` entry its `Parent` or `Name`
 *   names -> the default character style of its paragraph (the paragraph run's
 *   `DefaultStyleSheet`, else its paragraph style's) -> `TheNormalStyleSheet`.
 * Missing or out-of-range indices fall through to the next level, then to the defaults. */
static pc_status apply_engine_data(pc_text_layer *layer, const pc_ed_value *e, const pc_str *txt, float k,
                                   pc_error *err)
{
    static const pc_str empty_str = PC_STR_INIT;
    static const pc_ed_value empty_dict = {PC_ED_DICT, PC_STR_INIT, 0, 0.0, false, NULL, NULL, 0, 0};
    const pc_str *ed_text = pc_ed_as_str(pc_ed_path(e, "EngineDict", "Editor", "Text", (const char *)NULL));
    const pc_ed_value *res, *srun, *prun, *style_set, *para_set, *normal_style, *normal_para;
    const pc_ed_value **ssheets = NULL, **psheets = NULL;
    size_t nss = 0, nps = 0, nslens = 0, nplens = 0, k2, nz, nnorm = 0, nstyle_set = 0, npara_set = 0;
    size_t nslens16 = 0, nplens16 = 0;
    double *slens16 = NULL, *plens16 = NULL, at16 = 0.0;
    size_t *slens = NULL, *plens = NULL;
    para_base *pbases = NULL;
    pc_ed_value *bases = NULL;
    incoming *inc = NULL;
    psd_names fonts = {NULL, 0, 0};
    psd_run_list runs = {NULL, 0, 0};
    pc_text_run *norm = NULL;
    pc_paragraph_run *paras = NULL;
    pc_str text = PC_STR_INIT;
    pc_status st;
    if (ed_text == NULL) {
        ed_text = &empty_str;
    }
    /* Photoshop always ends the engine text with a paragraph break; the descriptor text doesn't. */
    if (txt != NULL) {
        st = psd_replace_byte(pc_str_c(txt), txt->len, '\r', '\n', &text, err);
    } else {
        size_t n = ed_text->len;
        if (n > 0 && pc_str_c(ed_text)[n - 1] == '\r') {
            n--;
        }
        st = psd_replace_byte(pc_str_c(ed_text), n, '\r', '\n', &text, err);
    }
    if (st != PC_OK) {
        return st;
    }
    pc_str_fini(&layer->text);
    layer->text = text;
    res = pc_ed_get(e, "ResourceDict");
    if (res == NULL) {
        res = pc_ed_get(e, "DocumentResources");
    }
    st = psd_names_of(pc_ed_get(res, "FontSet"), &fonts, err);
    if (st != PC_OK) {
        return st;
    }
    style_set = pc_ed_as_array(pc_ed_get(res, "StyleSheetSet"), &nstyle_set);
    para_set = pc_ed_as_array(pc_ed_get(res, "ParagraphSheetSet"), &npara_set);
    normal_style = normal_sheet(res, "TheNormalStyleSheet", style_set, nstyle_set);
    normal_para = normal_sheet(res, "TheNormalParagraphSheet", para_set, npara_set);

    /* Paragraph runs first: a character run's defaults depend on its paragraph. */
    prun = pc_ed_path(e, "EngineDict", "ParagraphRun", (const char *)NULL);
    st = run_items(prun, "ParagraphSheet", &psheets, &nps, err);
    if (st == PC_OK) {
        st = psd_arr_f(pc_ed_get(prun, "RunLengthArray"), &plens16, &nplens16, err);
    }
    if (st == PC_OK) {
        st = psd_byte_lengths(ed_text, pc_ed_get(prun, "RunLengthArray"), &plens, &nplens, err);
    }
    if (st == PC_OK && nps > 0 && (pbases = calloc(nps, sizeof *pbases)) == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
    }
    for (k2 = 0; k2 < nps && st == PC_OK; k2++) {
        const pc_ed_value *style = named_sheet(para_set, npara_set, psheets[k2]);
        const pc_ed_value *levels[2];
        const pc_ed_value *cand[2];
        size_t c, i;
        levels[0] = style != NULL ? pc_ed_get(style, "Properties") : NULL;
        levels[1] = normal_para != NULL ? pc_ed_get(normal_para, "Properties") : NULL;
        st = merged(levels, 2, &pbases[k2].base, err);
        cand[0] = psheets[k2];
        cand[1] = style;
        for (c = 0; c < 2 && pbases[k2].char_def == NULL; c++) {
            if (cand[c] != NULL && ed_index(cand[c], "DefaultStyleSheet", &i) && i < nstyle_set) {
                pbases[k2].char_def = &style_set[i];
            }
        }
    }
    nz = nps < nplens ? nps : nplens;
    if (st == PC_OK && nz > 0) {
        paras = pc_malloc_array(nz, sizeof *paras);
        if (paras == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
    }
    if (st == PC_OK) {
        for (k2 = 0; k2 < nz; k2++) {
            const pc_ed_value *props = pc_ed_get(psheets[k2], "Properties");
            paras[k2].len = plens[k2];
            paras[k2].style = psd_para_style(&pbases[k2].base, props != NULL ? props : &empty_dict, k);
        }
        free(layer->paragraphs);
        layer->paragraphs = paras;
        layer->nparagraphs = nz;
        paras = NULL;
    }

    /* Character runs. */
    srun = pc_ed_path(e, "EngineDict", "StyleRun", (const char *)NULL);
    if (st == PC_OK) {
        st = run_items(srun, "StyleSheet", &ssheets, &nss, err);
    }
    if (st == PC_OK) {
        st = psd_arr_f(pc_ed_get(srun, "RunLengthArray"), &slens16, &nslens16, err);
    }
    if (st == PC_OK) {
        st = psd_byte_lengths(ed_text, pc_ed_get(srun, "RunLengthArray"), &slens, &nslens, err);
    }
    if (st == PC_OK && nss > 0 && (bases = calloc(nss, sizeof *bases)) == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
    }
    for (k2 = 0; k2 < nss && st == PC_OK; k2++) {
        const pc_ed_value *own = named_sheet(style_set, nstyle_set, ssheets[k2]);
        const pc_ed_value *para_default = NULL, *levels[3];
        /* The paragraph run this character run starts in (UTF-16 offsets). */
        double end = 0.0;
        size_t p, pi = SIZE_MAX;
        for (p = 0; p < nplens16; p++) {
            end += plens16[p];
            if (at16 < end) {
                pi = p;
                break;
            }
        }
        if (pi == SIZE_MAX && nplens16 > 0) {
            pi = nplens16 - 1;
        }
        if (pi != SIZE_MAX && pi < nps) {
            para_default = pbases[pi].char_def;
        }
        at16 += k2 < nslens16 ? slens16[k2] : 0.0;
        levels[0] = own != NULL ? pc_ed_get(own, "StyleSheetData") : NULL;
        levels[1] = para_default != NULL ? pc_ed_get(para_default, "StyleSheetData") : NULL;
        levels[2] = normal_style != NULL ? pc_ed_get(normal_style, "StyleSheetData") : NULL;
        st = merged(levels, 3, &bases[k2], err);
    }
    nz = nss < nslens ? nss : nslens;
    if (st == PC_OK && nz > 0) {
        inc = pc_malloc_array(nz, sizeof *inc);
        if (inc == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
    }
    for (k2 = 0; k2 < nz && st == PC_OK; k2++) {
        double v;
        pc_char_style cs;
        const pc_ed_value *d = pc_ed_get(ssheets[k2], "StyleSheetData");
        if (d == NULL) {
            d = &empty_dict;
        }
        inc[k2].len = slens[k2];
        inc[k2].autok = flag(d, &bases[k2], "AutoKerning") != 0;
        inc[k2].kern = num(d, &bases[k2], "Kerning", &v) && isfinite(v) ? f32_of(v) : 0.0f;
        st = psd_char_style(&bases[k2], d, &fonts, k, &cs, err);
        if (st == PC_OK) {
            st = psd_run_list_push(&runs, slens[k2], &cs, err);
            if (st != PC_OK) {
                pc_char_style_fini(&cs);
            }
        }
    }
    if (st == PC_OK) {
        pc_text_runs_free(layer->runs, layer->nruns);
        layer->runs = runs.v;
        layer->nruns = runs.n;
        memset(&runs, 0, sizeof runs);
        st = pc_text_layer_char_runs(layer, &norm, &nnorm, err);
    }
    if (st == PC_OK) {
        st = kerning_from_pairs(&layer->text, norm, nnorm, inc, nz, &runs, err);
        norm = NULL;
    }
    if (st == PC_OK) {
        pc_text_runs_free(layer->runs, layer->nruns);
        layer->runs = runs.v;
        layer->nruns = runs.n;
        memset(&runs, 0, sizeof runs);
        st = read_shape(e, layer, err);
    }
    if (pbases != NULL) {
        for (k2 = 0; k2 < nps; k2++) {
            pc_ed_fini(&pbases[k2].base);
        }
    }
    if (bases != NULL) {
        for (k2 = 0; k2 < nss; k2++) {
            pc_ed_fini(&bases[k2]);
        }
    }
    free(pbases);
    free(bases);
    free(paras);
    free(ssheets);
    free(psheets);
    free(slens);
    free(plens);
    free(slens16);
    free(plens16);
    free(inc);
    pc_text_runs_free(norm, nnorm);
    psd_run_list_fini(&runs);
    psd_names_fini(&fonts);
    return st;
}

bool psd_descriptor_rect(const pc_psd_desc *text, const char *key, double out[4])
{
    const pc_psd_value *v = pc_psd_desc_get(text, key);
    double b[4];
    int k;
    if (v == NULL || v->kind != PC_PSD_V_DESCRIPTOR || v->desc == NULL) {
        return false;
    }
    if (!psd_dnum(pc_psd_desc_get(v->desc, "Left"), &b[0]) ||
        !psd_dnum(pc_psd_desc_get(v->desc, "Top "), &b[1]) ||
        !psd_dnum(pc_psd_desc_get(v->desc, "Rght"), &b[2]) ||
        !psd_dnum(pc_psd_desc_get(v->desc, "Btom"), &b[3])) {
        return false;
    }
    for (k = 0; k < 4; k++) {
        if (!isfinite(b[k])) {
            return false;
        }
    }
    if (!(b[2] > b[0] && b[3] > b[1])) {
        return false;
    }
    memcpy(out, b, sizeof b);
    return true;
}

/* legacy_point_origin (#1469): the local origin to fold into a legacy point-text transform.
 * Old writers used a zero PointBase plus a very large boundingBox origin, with an equally remote
 * transform translation that cancels it out. Ordinary point text is left alone. */
static bool legacy_point_origin(const pc_tysh *t, const pc_ed_value *e, double out[2])
{
    size_t n = 0, np = 0;
    const pc_ed_value *children =
        pc_ed_as_array(pc_ed_path(e, "EngineDict", "Rendered", "Shapes", "Children", (const char *)NULL), &n);
    const pc_ed_value *pb;
    double px, py, b[4], vals[8], shifted[2], origin_distance, shifted_distance;
    const double *m = t->transform.m;
    int k;
    if (children == NULL || n == 0) {
        return false;
    }
    pb =
        pc_ed_as_array(pc_ed_path(&children[0], "Cookie", "Photoshop", "PointBase", (const char *)NULL), &np);
    if (pb == NULL || np != 2) {
        return false;
    }
    if (!pc_ed_as_f64(&pb[0], &px) || px != 0.0 || !pc_ed_as_f64(&pb[1], &py) || py != 0.0) {
        return false;
    }
    if (!psd_descriptor_rect(&t->text, "boundingBox", b)) {
        return false;
    }
    memcpy(vals, m, 6 * sizeof *m);
    vals[6] = b[0];
    vals[7] = b[1];
    for (k = 0; k < 8; k++) {
        if (!isfinite(vals[k])) {
            return false;
        }
    }
    shifted[0] = m[0] * b[0] + m[2] * b[1] + m[4];
    shifted[1] = m[1] * b[0] + m[3] * b[1] + m[5];
    origin_distance = fmax(fabs(m[4]), fabs(m[5]));
    shifted_distance = fmax(fabs(shifted[0]), fabs(shifted[1]));
    /* A local bounding-box origin in the thousands and a transform origin beyond a normal canvas
     * are this old writer's signature; requiring the shifted origin to be closer keeps the
     * correction from applying again after export/import. */
    if (fmax(fabs(b[0]), fabs(b[1])) > 1024.0 && origin_distance > 4096.0 &&
        shifted_distance < origin_distance) {
        out[0] = b[0];
        out[1] = b[1];
        return true;
    }
    return false;
}

/* `Txt ` as the reference reads it: trailing NUL units dropped, lossy UTF-8. */
static pc_status ustr_lossy(const pc_psd_ustr *s, pc_str *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    size_t end = s->n, i = 0, len;
    pc_status st = PC_OK;
    uint8_t *data;
    while (end > 0 && s->u[end - 1] == 0) {
        end--;
    }
    while (i < end && st == PC_OK) {
        uint32_t u = s->u[i++], cp = u;
        if (u >= 0xD800 && u <= 0xDBFF) {
            uint32_t lo = i < end ? s->u[i] : 0;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            } else {
                cp = 0xFFFD;
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            cp = 0xFFFD;
        }
        st = pc_utf8_put(&b, cp, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    data = pc_buf_take(&b, &len);
    *out = data != NULL ? pc_str_adopt((char *)data, len) : (pc_str)PC_STR_INIT;
    return PC_OK;
}

static pc_antialias antialias_of(const pc_psd_desc *text)
{
    static const struct {
        const char *name;
        pc_antialias aa;
    } map[] = {{"Anno", PC_AA_NONE},
               {"antiAliasNone", PC_AA_NONE},
               {"antiAliasSharp", PC_AA_SHARP},
               {"AnSh", PC_AA_SHARP},
               {"AnCr", PC_AA_CRISP},
               {"antiAliasCrisp", PC_AA_CRISP},
               {"AnSt", PC_AA_STRONG},
               {"antiAliasStrong", PC_AA_STRONG},
               {"antiAliasPlatformGray", PC_AA_WINDOWS},
               {"antiAliasPlatformLCD", PC_AA_WINDOWS_LCD}};
    size_t k;
    for (k = 0; k < sizeof map / sizeof map[0]; k++) {
        if (enum_is(text, "AntA", map[k].name)) {
            return map[k].aa;
        }
    }
    return PC_AA_SMOOTH;
}

/* Whether the text descriptor has an `AntA` enumeration (enum_value is Some). */
static bool has_anta(const pc_psd_desc *text)
{
    const pc_psd_value *v = pc_psd_desc_get(text, "AntA");
    return v != NULL && v->kind == PC_PSD_V_ENUMERATED;
}

/* The anti-aliasing mode for EngineData's `AntiAlias` number, as Photoshop writes it: 0 None,
 * 1 Crisp, 2 Strong, 3 Smooth, 4 Sharp. Other numbers (platform modes) give Smooth. */
static pc_antialias engine_antialias(const pc_ed_value *e)
{
    double n;
    if (e == NULL || !pc_ed_as_f64(pc_ed_path(e, "EngineDict", "AntiAlias", (const char *)NULL), &n)) {
        return PC_AA_SMOOTH;
    }
    switch (pc_f64_to_i64(n)) {
    case 0:
        return PC_AA_NONE;
    case 1:
        return PC_AA_CRISP;
    case 2:
        return PC_AA_STRONG;
    case 3:
        return PC_AA_SMOOTH;
    case 4:
        return PC_AA_SHARP;
    default:
        return PC_AA_SMOOTH;
    }
}

static pc_status read_warp(const pc_psd_desc *w, pc_text_layer *layer, pc_error *err)
{
    pc_str style = PC_STR_INIT;
    bool has;
    double v;
    pc_status st = enum_value(w, "warpStyle", &has, &style, err);
    if (st != PC_OK) {
        return st;
    }
    if (!has) {
        st = pc_str_set_c(&style, "warpNone", err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (pc_str_eq_c(&style, "warpNone")) {
        pc_str_fini(&style);
        return PC_OK;
    }
    layer->has_warp = true;
    layer->warp.style = style;
    layer->warp.value = psd_dnum(pc_psd_desc_get(w, "warpValue"), &v) ? f32_of(v) : 0.0f;
    layer->warp.horizontal_distortion =
        psd_dnum(pc_psd_desc_get(w, "warpPerspective"), &v) ? f32_of(v) : 0.0f;
    layer->warp.vertical_distortion =
        psd_dnum(pc_psd_desc_get(w, "warpPerspectiveOther"), &v) ? f32_of(v) : 0.0f;
    layer->warp.horizontal = !enum_is(w, "warpRotate", "Vrtc");
    return PC_OK;
}

pc_status pc_text_layer_from_tysh(const uint8_t *data, size_t len, float dpi, pc_text_layer *out,
                                  pc_error *err)
{
    pc_tysh t;
    pc_text_layer layer;
    pc_str txt = PC_STR_INIT;
    bool has_txt = false;
    const pc_psd_value *tv;
    pc_ed_value e;
    pc_error tmp;
    double origin[2];
    float k = psd_k_of(dpi);
    bool has_e;
    pc_status st = pc_tysh_parse(data, len, &t, err);
    if (st != PC_OK) {
        return st;
    }
    pc_text_layer_default(&layer);
    layer.transform = t.transform;
    tv = pc_psd_desc_get(&t.text, "Txt ");
    if (tv != NULL && tv->kind == PC_PSD_V_TEXT) {
        st = ustr_lossy(&tv->text, &txt, err);
        has_txt = st == PC_OK;
    }
    layer.orientation = enum_is(&t.text, "Ornt", "Vrtc") ? PC_ORIENT_VERTICAL : PC_ORIENT_HORIZONTAL;
    layer.antialias = antialias_of(&t.text);
    if (st == PC_OK && t.has_warp) {
        st = read_warp(&t.warp, &layer, err);
    }
    has_e = false;
    if (st == PC_OK) {
        st = pc_tysh_engine_data(&t.text, &e, &tmp);
        if (st == PC_OK) {
            has_e = true;
        } else if (st == PC_ERR_NOMEM) {
            (void)forward(err, &tmp, st);
        } else {
            st = PC_OK;
        }
    }
    if (st == PC_OK && !has_anta(&t.text)) {
        /* No `AntA`: the engine data's mode says it. */
        layer.antialias = engine_antialias(has_e ? &e : NULL);
    }
    if (st == PC_OK && has_e) {
        double wd;
        const pc_psd_value *ornt = pc_psd_desc_get(&t.text, "Ornt");
        /* Without an `Ornt` key, the engine data's writing direction says it (2 = vertical). */
        if ((ornt == NULL || ornt->kind != PC_PSD_V_ENUMERATED) &&
            pc_ed_as_f64(
                pc_ed_path(&e, "EngineDict", "Rendered", "Shapes", "WritingDirection", (const char *)NULL),
                &wd) &&
            wd == 2.0) {
            layer.orientation = PC_ORIENT_VERTICAL;
        }
        st = apply_engine_data(&layer, &e, has_txt ? &txt : NULL, k, err);
    } else if (st == PC_OK) {
        pc_str text = PC_STR_INIT;
        st = psd_replace_byte(pc_str_c(&txt), txt.len, '\r', '\n', &text, err);
        if (st == PC_OK) {
            pc_str_fini(&layer.text);
            layer.text = text;
        }
    }
    /* Some legacy PSDs store point text at a large local origin with a zero PointBase; move the
     * imported layout to that origin so editing, rendering and writing agree (#1469). */
    if (st == PC_OK && has_e && legacy_point_origin(&t, &e, origin)) {
        double *m = layer.transform.m;
        double a = m[0], b = m[1], c = m[2], d = m[3], ee = m[4], f = m[5];
        m[4] = a * origin[0] + c * origin[1] + ee;
        m[5] = b * origin[0] + d * origin[1] + f;
    }
    if (st == PC_OK) {
        st = pc_text_layer_sync_summary(&layer, err);
    }
    if (has_e) {
        pc_ed_fini(&e);
    }
    pc_str_fini(&txt);
    pc_tysh_fini(&t);
    if (st != PC_OK) {
        pc_text_layer_fini(&layer);
        return st;
    }
    *out = layer;
    return PC_OK;
}
