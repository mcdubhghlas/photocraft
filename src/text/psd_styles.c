/* Named character and paragraph styles in the PSD text engine data (port of
 * crates/text/src/psd_styles.rs).
 *
 * Every type layer's EngineData carries the document's style sheets in its ResourceDict (and
 * DocumentResources): StyleSheetSet (character sheets: Name, StyleSheetData) and
 * ParagraphSheetSet (Name, DefaultStyleSheet, Properties), with TheNormalStyleSheet /
 * TheNormalParagraphSheet naming the defaults. Runs refer to a sheet from their StyleSheet /
 * ParagraphSheet dictionaries. Named sheets are written with fully resolved attributes, so the
 * text looks the same whatever a reader makes of the reference. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "text/fonts.h"
#include "text/psd_internal.h"

void pc_psd_style_sheets_fini(pc_psd_style_sheets *s)
{
    size_t k;
    if (s == NULL) {
        return;
    }
    pc_char_style_fini(&s->normal_char);
    for (k = 0; k < s->nchar_sheets; k++) {
        pc_str_fini(&s->char_sheets[k].name);
        pc_char_style_fini(&s->char_sheets[k].style);
    }
    free(s->char_sheets);
    for (k = 0; k < s->npara_sheets; k++) {
        pc_str_fini(&s->para_sheets[k].name);
        pc_char_style_fini(&s->para_sheets[k].chr);
    }
    free(s->para_sheets);
    free(s->run_char);
    free(s->run_char_lens);
    free(s->run_para);
    memset(s, 0, sizeof *s);
}

/* Index a run's sheet dictionary refers to (`Parent` index, else `Name`), skipping `normal`. */
static pc_opt_index reference(const pc_ed_value *sheet, const psd_names *set_names, size_t normal)
{
    pc_opt_index r = {false, 0};
    int64_t i;
    size_t idx;
    if (sheet == NULL) {
        return r;
    }
    if (pc_ed_as_i64(pc_ed_get(sheet, "Parent"), &i)) {
        idx = (size_t)i;
    } else {
        const pc_str *n = pc_ed_as_str(pc_ed_get(sheet, "Name"));
        if (n == NULL) {
            return r;
        }
        idx = psd_names_find(set_names, pc_str_c(n), n->len);
        if (idx == set_names->n) {
            return r;
        }
    }
    if (idx != normal && idx < set_names->n) {
        r.has = true;
        r.v = idx;
    }
    return r;
}

static size_t normal_index(const pc_ed_value *res, const char *key)
{
    int64_t i;
    return pc_ed_as_i64(pc_ed_get(res, key), &i) ? (size_t)i : 0;
}

static pc_status run_refs(const pc_ed_value *e, const char *run_key, const char *sheet_key,
                          const psd_names *names, size_t normal, bool *has, pc_opt_index **out, size_t *n,
                          pc_error *err)
{
    size_t len = 0, k;
    const pc_ed_value *a =
        pc_ed_as_array(pc_ed_path(e, "EngineDict", run_key, "RunArray", (const char *)NULL), &len);
    pc_opt_index *r = NULL;
    *has = a != NULL;
    *out = NULL;
    *n = 0;
    if (a == NULL || len == 0) {
        return PC_OK;
    }
    r = pc_malloc_array(len, sizeof *r);
    if (r == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD style sheets");
    }
    for (k = 0; k < len; k++) {
        r[k] = reference(pc_ed_get(&a[k], sheet_key), names, normal);
    }
    *out = r;
    *n = len;
    return PC_OK;
}

static pc_status read_sheets(const pc_ed_value *e, const pc_ed_value *res, float k, pc_psd_style_sheets *o,
                             pc_error *err)
{
    static const pc_ed_value empty = {PC_ED_DICT, PC_STR_INIT, 0, 0.0, false, NULL, NULL, 0, 0};
    psd_names fonts = {NULL, 0, 0}, snames = {NULL, 0, 0}, pnames = {NULL, 0, 0};
    size_t ns = 0, np = 0, i, normal_c, normal_p;
    const pc_ed_value *sset = pc_ed_as_array(pc_ed_get(res, "StyleSheetSet"), &ns);
    const pc_ed_value *pset = pc_ed_as_array(pc_ed_get(res, "ParagraphSheetSet"), &np);
    const pc_ed_value *base_data = NULL, *base_props = NULL;
    bool has;
    pc_status st;
    if (sset == NULL) {
        ns = 0;
    }
    if (pset == NULL) {
        np = 0;
    }
    normal_c = normal_index(res, "TheNormalStyleSheet");
    normal_p = normal_index(res, "TheNormalParagraphSheet");
    st = psd_names_of(pc_ed_get(res, "FontSet"), &fonts, err);
    if (st != PC_OK) {
        return st;
    }
    if (normal_c < ns) {
        base_data = pc_ed_get(&sset[normal_c], "StyleSheetData");
    }
    st = psd_char_style(NULL, base_data != NULL ? base_data : &empty, &fonts, k, &o->normal_char, err);
    if (np > 0 && normal_p < np) {
        base_props = pc_ed_get(&pset[normal_p], "Properties");
    }
    o->normal_para = psd_para_style(NULL, base_props != NULL ? base_props : &empty, k);
    if (st == PC_OK && ns > 0) {
        o->char_sheets = pc_calloc_array(ns, sizeof *o->char_sheets);
        if (o->char_sheets == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD style sheets");
        }
    }
    for (i = 0; i < ns && st == PC_OK; i++) {
        pc_psd_char_sheet *c;
        const pc_str *name;
        const pc_ed_value *data;
        if (i == normal_c) {
            continue;
        }
        c = &o->char_sheets[o->nchar_sheets];
        name = pc_ed_as_str(pc_ed_get(&sset[i], "Name"));
        data = pc_ed_get(&sset[i], "StyleSheetData");
        c->index = i;
        st = pc_str_set(&c->name, name != NULL ? pc_str_c(name) : "", name != NULL ? name->len : 0, err);
        if (st == PC_OK) {
            st = psd_char_style(base_data, data != NULL ? data : &empty, &fonts, k, &c->style, err);
            if (st != PC_OK) {
                pc_str_fini(&c->name);
            }
        }
        if (st == PC_OK) {
            o->nchar_sheets++;
        }
    }
    if (st == PC_OK && np > 0) {
        o->para_sheets = pc_calloc_array(np, sizeof *o->para_sheets);
        if (o->para_sheets == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD style sheets");
        }
    }
    for (i = 0; i < np && st == PC_OK; i++) {
        pc_psd_para_sheet *p;
        const pc_str *name;
        const pc_ed_value *props;
        int64_t d;
        if (i == normal_p) {
            continue;
        }
        p = &o->para_sheets[o->npara_sheets];
        name = pc_ed_as_str(pc_ed_get(&pset[i], "Name"));
        props = pc_ed_get(&pset[i], "Properties");
        p->index = i;
        p->style = psd_para_style(base_props, props != NULL ? props : &empty, k);
        st = pc_str_set(&p->name, name != NULL ? pc_str_c(name) : "", name != NULL ? name->len : 0, err);
        if (st == PC_OK && pc_ed_as_i64(pc_ed_get(&pset[i], "DefaultStyleSheet"), &d) &&
            (size_t)d != normal_c && (size_t)d < ns) {
            const pc_ed_value *data = pc_ed_get(&sset[(size_t)d], "StyleSheetData");
            st = psd_char_style(base_data, data != NULL ? data : &empty, &fonts, k, &p->chr, err);
            p->has_char = st == PC_OK;
        }
        if (st == PC_OK) {
            o->npara_sheets++;
        } else {
            pc_str_fini(&p->name);
        }
    }
    if (st == PC_OK) {
        st = psd_names_of(pc_ed_get(res, "StyleSheetSet"), &snames, err);
    }
    if (st == PC_OK) {
        st = psd_names_of(pc_ed_get(res, "ParagraphSheetSet"), &pnames, err);
    }
    if (st == PC_OK) {
        st = run_refs(e, "StyleRun", "StyleSheet", &snames, normal_c, &has, &o->run_char, &o->nrun_char, err);
    }
    if (st == PC_OK && has) {
        const pc_str *ed_text =
            pc_ed_as_str(pc_ed_path(e, "EngineDict", "Editor", "Text", (const char *)NULL));
        static const pc_str none = PC_STR_INIT;
        st = psd_byte_lengths(ed_text != NULL ? ed_text : &none,
                              pc_ed_path(e, "EngineDict", "StyleRun", "RunLengthArray", (const char *)NULL),
                              &o->run_char_lens, &o->nrun_char_lens, err);
    }
    if (st == PC_OK) {
        st = run_refs(e, "ParagraphRun", "ParagraphSheet", &pnames, normal_p, &has, &o->run_para,
                      &o->nrun_para, err);
    }
    psd_names_fini(&fonts);
    psd_names_fini(&snames);
    psd_names_fini(&pnames);
    return st;
}

/* parse_tysh + engine_data, mapping "None" outcomes to PC_ERR_FORMAT without touching err. */
static pc_status load(const uint8_t *tysh, size_t len, pc_tysh *t, pc_ed_value *e, pc_error *err)
{
    pc_error tmp;
    pc_status st = pc_tysh_parse(tysh, len, t, &tmp);
    if (st == PC_OK) {
        st = pc_tysh_engine_data(&t->text, e, &tmp);
        if (st != PC_OK) {
            pc_tysh_fini(t);
        }
    }
    if (st != PC_OK && err != NULL) {
        *err = tmp;
    }
    return st == PC_OK || st == PC_ERR_NOMEM ? st : PC_ERR_FORMAT;
}

pc_status pc_text_read_style_sheets(const uint8_t *tysh, size_t len, float dpi, pc_psd_style_sheets *out,
                                    pc_error *err)
{
    pc_tysh t;
    pc_ed_value e;
    pc_psd_style_sheets o;
    const pc_ed_value *res;
    pc_status st = load(tysh, len, &t, &e, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&o, 0, sizeof o);
    res = pc_ed_get(&e, "ResourceDict");
    if (res == NULL) {
        res = pc_ed_get(&e, "DocumentResources");
    }
    if (res == NULL) {
        st = pc_fail(err, PC_ERR_FORMAT, "no ResourceDict");
    } else {
        st = read_sheets(&e, res, psd_k_of(dpi), &o, err);
    }
    pc_ed_fini(&e);
    pc_tysh_fini(&t);
    if (st != PC_OK) {
        pc_psd_style_sheets_fini(&o);
        return st;
    }
    *out = o;
    return PC_OK;
}

pc_status pc_text_has_named_sheets(const uint8_t *tysh, size_t len, bool *out, pc_error *err)
{
    pc_tysh t;
    pc_ed_value e;
    const pc_ed_value *res;
    size_t ns = 0, np = 0;
    pc_status st = load(tysh, len, &t, &e, err);
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    *out = false;
    if (st != PC_OK) {
        return PC_OK;
    }
    res = pc_ed_get(&e, "ResourceDict");
    if (pc_ed_as_array(pc_ed_get(res, "StyleSheetSet"), &ns) == NULL) {
        ns = 0;
    }
    if (pc_ed_as_array(pc_ed_get(res, "ParagraphSheetSet"), &np) == NULL) {
        np = 0;
    }
    *out = ns > 1 || np > 1;
    pc_ed_fini(&e);
    pc_tysh_fini(&t);
    return PC_OK;
}

/* The sheet to keep: set[normal] or set[0], cloned; *has false when there is none. */
static pc_status keep_sheet(const pc_ed_value *rd, const char *set_key, size_t normal, pc_ed_value *set,
                            pc_error *err)
{
    size_t n = 0;
    const pc_ed_value *a = pc_ed_as_array(pc_ed_get(rd, set_key), &n);
    const pc_ed_value *keep = NULL;
    pc_ed_value c;
    pc_status st;
    if (a != NULL && n > 0) {
        keep = normal < n ? &a[normal] : &a[0];
    }
    if (keep == NULL) {
        return PC_OK;
    }
    st = pc_ed_clone(keep, &c, err);
    if (st == PC_OK) {
        st = pc_ed_push(set, &c, err);
        if (st != PC_OK) {
            pc_ed_fini(&c);
        }
    }
    return st;
}

static pc_status named_char_sheet(const pc_text_styles *styles, const pc_character_style_def *d,
                                  psd_names *fonts, float k, pc_ed_value *out, pc_error *err)
{
    pc_char_style s;
    pc_str ps = PC_STR_INIT;
    size_t fi = 0;
    pc_ed_value sheet = pc_ed_dict(), data;
    pc_status st = pc_text_styles_resolve_char(styles, NULL, &d->id, PC_TEXT_DEFAULT_FAMILY, &s, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_text_postscript_for(&s, &ps, err);
    if (st == PC_OK) {
        st = psd_names_index(fonts, &ps, &fi, err);
    }
    if (st == PC_OK) {
        st = psd_set_str(&sheet, "Name", pc_str_c(&d->name), d->name.len, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&sheet, "Parent", 0, err);
    }
    if (st == PC_OK) {
        st = psd_style_sheet_data(&s, fi, k, &data, err);
        if (st == PC_OK) {
            st = pc_ed_set(&sheet, "StyleSheetData", &data, err);
            pc_ed_fini(&data);
        }
    }
    pc_str_fini(&ps);
    pc_char_style_fini(&s);
    if (st != PC_OK) {
        pc_ed_fini(&sheet);
        return st;
    }
    *out = sheet;
    return PC_OK;
}

static pc_status named_para_sheet(const pc_text_styles *styles, const pc_paragraph_style_def *d, float k,
                                  pc_ed_value *out, pc_error *err)
{
    pc_paragraph_style p;
    pc_ed_value sheet = pc_ed_dict(), props;
    pc_status st = pc_text_styles_resolve_para(styles, &d->id, &p, err);
    if (st == PC_OK) {
        st = psd_set_str(&sheet, "Name", pc_str_c(&d->name), d->name.len, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&sheet, "DefaultStyleSheet", 0, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(&sheet, "Parent", 0, err);
    }
    if (st == PC_OK) {
        st = psd_paragraph_properties(&p, k, &props, err);
        if (st == PC_OK) {
            st = pc_ed_set(&sheet, "Properties", &props, err);
            pc_ed_fini(&props);
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&sheet);
        return st;
    }
    *out = sheet;
    return PC_OK;
}

static pc_status rewrite_resources(pc_ed_value *rd, const pc_text_styles *styles, float k, pc_error *err)
{
    psd_names fonts = {NULL, 0, 0};
    size_t normal_c = normal_index(rd, "TheNormalStyleSheet");
    size_t normal_p = normal_index(rd, "TheNormalParagraphSheet");
    pc_ed_value sset = pc_ed_array(), pset = pc_ed_array(), fontset = pc_ed_array(), v;
    size_t i;
    pc_status st = psd_names_of(pc_ed_get(rd, "FontSet"), &fonts, err);
    if (st == PC_OK) {
        st = keep_sheet(rd, "StyleSheetSet", normal_c, &sset, err);
    }
    if (st == PC_OK) {
        st = keep_sheet(rd, "ParagraphSheetSet", normal_p, &pset, err);
    }
    for (i = 0; i < styles->ncharacter && st == PC_OK; i++) {
        st = named_char_sheet(styles, &styles->character[i], &fonts, k, &v, err);
        if (st == PC_OK) {
            st = pc_ed_push(&sset, &v, err);
            pc_ed_fini(&v);
        }
    }
    for (i = 0; i < styles->nparagraph && st == PC_OK; i++) {
        st = named_para_sheet(styles, &styles->paragraph[i], k, &v, err);
        if (st == PC_OK) {
            st = pc_ed_push(&pset, &v, err);
            pc_ed_fini(&v);
        }
    }
    if (st == PC_OK) {
        st = pc_ed_set(rd, "StyleSheetSet", &sset, err);
    }
    if (st == PC_OK) {
        st = pc_ed_set(rd, "ParagraphSheetSet", &pset, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(rd, "TheNormalStyleSheet", 0, err);
    }
    if (st == PC_OK) {
        st = psd_set_int(rd, "TheNormalParagraphSheet", 0, err);
    }
    if (st == PC_OK) {
        st = psd_font_set(&fonts, &fontset, err);
    }
    if (st == PC_OK) {
        st = pc_ed_set(rd, "FontSet", &fontset, err);
    }
    pc_ed_fini(&sset);
    pc_ed_fini(&pset);
    pc_ed_fini(&fontset);
    psd_names_fini(&fonts);
    return st;
}

/* 1-based position of the style with `id` (has_id false -> none). */
static bool char_ix(const pc_text_styles *styles, bool has_id, uint32_t id, int64_t *out)
{
    size_t i;
    for (i = 0; has_id && i < styles->ncharacter; i++) {
        if (styles->character[i].id == id) {
            *out = (int64_t)i + 1;
            return true;
        }
    }
    return false;
}

static bool para_ix(const pc_text_styles *styles, bool has_id, uint32_t id, int64_t *out)
{
    size_t i;
    for (i = 0; has_id && i < styles->nparagraph; i++) {
        if (styles->paragraph[i].id == id) {
            *out = (int64_t)i + 1;
            return true;
        }
    }
    return false;
}

/* The model run containing byte `off` (else the last one), or NULL. */
static const pc_text_run *run_at(const pc_text_run *runs, size_t n, size_t off)
{
    size_t end = 0, i;
    for (i = 0; i < n; i++) {
        end = end > SIZE_MAX - runs[i].len ? SIZE_MAX : end + runs[i].len;
        if (off < end) {
            return &runs[i];
        }
    }
    return n > 0 ? &runs[n - 1] : NULL;
}

/* Re-points one run's sheet: drops Parent and Name, then sets Parent when it has a style. */
static pc_status repoint(pc_ed_value *sheet, bool has, int64_t ix, pc_error *err)
{
    pc_ed_remove(sheet, "Parent");
    pc_ed_remove(sheet, "Name");
    return has ? psd_set_int(sheet, "Parent", ix, err) : PC_OK;
}

static pc_status relink_runs(pc_ed_value *e, const pc_text_layer *layer, const pc_text_styles *styles,
                             pc_error *err)
{
    pc_text_run *runs = NULL;
    pc_paragraph_run *paras = NULL;
    size_t nruns = 0, nparas = 0, *lens = NULL, nlens = 0, i, off = 0;
    pc_ed_value *dict = pc_ed_get_mut(e, "EngineDict");
    pc_ed_value *a;
    const pc_str *ed_text = pc_ed_as_str(pc_ed_path(e, "EngineDict", "Editor", "Text", (const char *)NULL));
    static const pc_str none = PC_STR_INIT;
    pc_status st = pc_text_layer_char_runs(layer, &runs, &nruns, err);
    if (st == PC_OK) {
        st = pc_text_layer_paragraph_runs(layer, &paras, &nparas, err);
    }
    /* Each entry lies inside one model run: find it by the entry's start offset. */
    if (st == PC_OK) {
        st = psd_byte_lengths(ed_text != NULL ? ed_text : &none,
                              pc_ed_path(e, "EngineDict", "StyleRun", "RunLengthArray", (const char *)NULL),
                              &lens, &nlens, err);
    }
    a = pc_ed_get_mut(pc_ed_get_mut(dict, "StyleRun"), "RunArray");
    if (st == PC_OK && a != NULL && a->kind == PC_ED_ARRAY) {
        for (i = 0; i < a->n && st == PC_OK; i++) {
            size_t start = off;
            pc_ed_value *sheet = pc_ed_get_mut(&a->items[i], "StyleSheet");
            size_t l = i < nlens ? lens[i] : 0;
            off = off > SIZE_MAX - l ? SIZE_MAX : off + l;
            if (sheet != NULL) {
                const pc_text_run *r = run_at(runs, nruns, start);
                int64_t ix = 0;
                bool has = r != NULL && char_ix(styles, r->style.has_style_sheet, r->style.style_sheet, &ix);
                st = repoint(sheet, has, ix, err);
            }
        }
    }
    a = pc_ed_get_mut(pc_ed_get_mut(dict, "ParagraphRun"), "RunArray");
    if (st == PC_OK && a != NULL && a->kind == PC_ED_ARRAY) {
        for (i = 0; i < a->n && st == PC_OK; i++) {
            pc_ed_value *sheet = pc_ed_get_mut(&a->items[i], "ParagraphSheet");
            if (sheet != NULL) {
                int64_t ix = 0;
                bool has = i < nparas &&
                           para_ix(styles, paras[i].style.has_style_sheet, paras[i].style.style_sheet, &ix);
                st = repoint(sheet, has, ix, err);
            }
        }
    }
    pc_text_runs_free(runs, nruns);
    free(paras);
    free(lens);
    return st;
}

pc_status pc_text_write_style_sheets(const uint8_t *tysh, size_t len, const pc_text_layer *layer,
                                     const pc_text_styles *styles, float dpi, pc_psd_bytes *out,
                                     pc_error *err)
{
    static const char *const keys[2] = {"ResourceDict", "DocumentResources"};
    pc_tysh t;
    pc_ed_value e;
    pc_buf raw = PC_BUF_INIT;
    float k = psd_k_of(dpi);
    size_t i;
    pc_psd_item *item = NULL;
    pc_status st = load(tysh, len, &t, &e, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < 2 && st == PC_OK; i++) {
        pc_ed_value *rd = pc_ed_get_mut(&e, keys[i]);
        if (rd != NULL) {
            st = rewrite_resources(rd, styles, k, err);
        }
    }
    if (st == PC_OK) {
        st = relink_runs(&e, layer, styles, err);
    }
    if (st == PC_OK) {
        st = pc_ed_write(&e, &raw, err);
    }
    for (i = 0; st == PC_OK && i < t.text.n && item == NULL; i++) {
        if (pc_psd_id_is(&t.text.items[i].key, "EngineData")) {
            item = &t.text.items[i];
        }
    }
    if (st == PC_OK && item == NULL) {
        st = pc_fail(err, PC_ERR_FORMAT, "no EngineData item");
    }
    if (st == PC_OK) {
        pc_psd_value v;
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_RAW_DATA;
        st = pc_psd_bytes_set(&v.raw, raw.data, raw.len, err);
        if (st == PC_OK) {
            pc_psd_value_fini(&item->value);
            item->value = v;
            st = pc_tysh_write(&t, out, err);
        }
    }
    pc_buf_fini(&raw);
    pc_ed_fini(&e);
    pc_tysh_fini(&t);
    return st;
}
