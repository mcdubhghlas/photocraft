/* PSD type layers: the document's Txt2 block (port of the Txt2 half of crates/text/src/psd.rs):
 * reading auto-kern modes, OpenType settings and frames back into layers, and building the block
 * for export. See psd.h. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/psd_internal.h"

static pc_status forward(pc_error *err, const pc_error *tmp, pc_status st)
{
    if (st == PC_ERR_NOMEM && err != NULL) {
        *err = *tmp;
    }
    return st;
}

/* ---- run lists (shared with psd_read.c) ---- */

void psd_run_list_fini(psd_run_list *l)
{
    pc_text_runs_free(l->v, l->n);
    memset(l, 0, sizeof *l);
}

/* Appends {len, style}, taking over *style (on success). */
pc_status psd_run_list_push(psd_run_list *l, size_t len, pc_char_style *style, pc_error *err)
{
    if (l->n == l->cap) {
        size_t cap = l->cap;
        pc_text_run *g = pc_grow_array(l->v, &cap, l->n + 1, sizeof *g);
        if (g == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading PSD text");
        }
        l->v = g;
        l->cap = cap;
    }
    l->v[l->n].len = len;
    l->v[l->n].style = *style;
    memset(style, 0, sizeof *style);
    l->n++;
    return PC_OK;
}

/* Replaces the layer's runs with *l (taken over) and syncs the summary; on failure nothing
 * changes and *l is still the caller's. */
pc_status psd_set_runs(pc_text_layer *layer, psd_run_list *l, pc_error *err)
{
    pc_str family = PC_STR_INIT;
    if (l->n > 0) {
        pc_status st = pc_str_clone(&l->v[0].style.font_family, &family, err);
        if (st != PC_OK) {
            return st;
        }
    }
    pc_text_runs_free(layer->runs, layer->nruns);
    layer->runs = l->v;
    layer->nruns = l->n;
    memset(l, 0, sizeof *l);
    if (layer->nruns > 0) {
        pc_str_fini(&layer->font_family);
        layer->font_family = family;
        layer->size_pt = layer->runs[0].style.size_pt;
        layer->color = layer->runs[0].style.color;
    }
    return PC_OK;
}

pc_status pc_txt2_parse(const uint8_t *data, size_t len, pc_ed_value *out, pc_error *err)
{
    pc_buf w = PC_BUF_INIT;
    pc_ed_value v;
    pc_error tmp;
    pc_status st = pc_buf_append(&w, "<<", 2, err);
    if (st == PC_OK && len > 0) {
        st = pc_buf_append(&w, data, len, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&w, ">>", 2, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&w);
        return st;
    }
    st = pc_ed_parse(w.data, w.len, &v, &tmp);
    pc_buf_fini(&w);
    if (st != PC_OK) {
        if (st == PC_ERR_NOMEM) {
            return forward(err, &tmp, st);
        }
        if (err != NULL) {
            *err = tmp;
        }
        return PC_ERR_FORMAT;
    }
    if (v.kind != PC_ED_DICT) {
        pc_ed_fini(&v);
        return pc_fail(err, PC_ERR_FORMAT, "Txt2 is not a dictionary");
    }
    *out = v;
    return PC_OK;
}

/* ---- Txt2 ---- */

static bool text_index(const uint8_t *tysh, size_t len, size_t *out, pc_status *st, pc_error *err)
{
    pc_tysh t;
    pc_error tmp;
    const pc_psd_value *v;
    bool ok = false;
    *st = pc_tysh_parse(tysh, len, &t, &tmp);
    if (*st != PC_OK) {
        if (*st == PC_ERR_NOMEM) {
            (void)forward(err, &tmp, *st);
        } else {
            *st = PC_OK;
        }
        return false;
    }
    v = pc_psd_desc_get(&t.text, "TextIndex");
    if (v != NULL && v->kind == PC_PSD_V_INTEGER && v->i >= 0) {
        *out = (size_t)v->i;
        ok = true;
    }
    pc_tysh_fini(&t);
    return ok;
}

/* text.strip_suffix('\r').unwrap_or(text).replace('\r', "\n") == layer */
static bool same_text_cr(const pc_str *text, const pc_str *layer)
{
    const char *s = pc_str_c(text);
    const char *l = pc_str_c(layer);
    size_t n = text->len, k;
    if (n > 0 && s[n - 1] == '\r') {
        n--;
    }
    if (n != layer->len) {
        return false;
    }
    for (k = 0; k < n; k++) {
        char c = s[k] == '\r' ? '\n' : s[k];
        if (c != l[k]) {
            return false;
        }
    }
    return true;
}

typedef struct seg {
    size_t len;
    const pc_ed_value *style; /* the run's Txt2 style dictionary (`/0 /0 /6`), or NULL */
} seg;

const char *const PC_TXT2_FEATURES[PC_TXT2_FEATURE_COUNT][2] = {
    {"20", "calt"}, {"23", "frac"}, {"24", "ordn"}, {"25", "swsh"},
    {"26", "titl"}, {"28", "salt"}, {"29", "ornm"},
};

/* Appends feature (tag, value) unless the style already sets tag. */
static pc_status set_feature(pc_char_style *style, const char *tag, uint16_t value, pc_error *err)
{
    size_t k, len = strlen(tag), cap = style->nfeatures;
    pc_font_feature *g;
    for (k = 0; k < style->nfeatures; k++) {
        if (style->features[k].tag.len == len && memcmp(pc_str_c(&style->features[k].tag), tag, len) == 0) {
            return PC_OK;
        }
    }
    g = pc_grow_array(style->features, &cap, style->nfeatures + 1, sizeof *g);
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading Txt2");
    }
    style->features = g;
    memset(&g[style->nfeatures], 0, sizeof g[style->nfeatures]);
    if (pc_str_set_c(&g[style->nfeatures].tag, tag, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    g[style->nfeatures].value = value;
    style->nfeatures++;
    return PC_OK;
}

/* Sets in `style` the OpenType features a Txt2 style dictionary turns on (PC_TXT2_FEATURES and
 * the Stylistic Sets mask). A feature the style already sets keeps its value. A flag that is off
 * changes nothing, except Contextual Alternates: shapers apply `calt` by default, Photoshop only
 * when it is on (its default is off), so off is written as `calt` 0. */
static pc_status add_txt2_features(pc_char_style *style, const pc_ed_value *t, pc_error *err)
{
    size_t k;
    int64_t sets = 0;
    pc_status st = PC_OK;
    for (k = 0; k < PC_TXT2_FEATURE_COUNT && st == PC_OK; k++) {
        bool on;
        if (!pc_ed_as_bool(pc_ed_get(t, PC_TXT2_FEATURES[k][0]), &on)) {
            continue;
        }
        if (on) {
            st = set_feature(style, PC_TXT2_FEATURES[k][1], 1, err);
        } else if (strcmp(PC_TXT2_FEATURES[k][1], "calt") == 0) {
            st = set_feature(style, "calt", 0, err);
        }
    }
    /* A mask beyond the 20 sets is not Photoshop's: ignore it. */
    if (!pc_ed_as_i64(pc_ed_get(t, PC_TXT2_STYLISTIC_SETS), &sets) || sets < 0 || sets >= (1 << 20)) {
        sets = 0;
    }
    for (k = 0; k < 20 && st == PC_OK; k++) {
        if ((sets & ((int64_t)1 << k)) != 0) {
            char tag[8];
            (void)snprintf(tag, sizeof tag, "ss%02u", (unsigned)(k + 1));
            st = set_feature(style, tag, 1, err);
        }
    }
    return st;
}

pc_status pc_txt2_apply(pc_text_layer *layer, const uint8_t *tysh, size_t len, const pc_ed_value *txt2,
                        pc_error *err)
{
    size_t index = 0, nobj = 0, nstyle = 0, k, nseg = 0, nruns = 0, si = 0, seg_end, at = 0, ri;
    const pc_ed_value *objs, *model, *styles;
    const pc_str *text;
    double *lens = NULL;
    size_t *bytes = NULL;
    seg *segs = NULL;
    pc_text_run *runs = NULL;
    psd_run_list out = {NULL, 0, 0};
    pc_status st = PC_OK;
    if (!text_index(tysh, len, &index, &st, err)) {
        return st;
    }
    objs = pc_ed_as_array(pc_ed_path(txt2, "1", "1", (const char *)NULL), &nobj);
    if (objs == NULL || index >= nobj) {
        return PC_OK;
    }
    model = pc_ed_get(&objs[index], "0");
    text = pc_ed_as_str(pc_ed_get(model, "0"));
    if (model == NULL || text == NULL || !same_text_cr(text, &layer->text)) {
        return PC_OK;
    }
    styles = pc_ed_as_array(pc_ed_path(model, "6", "0", (const char *)NULL), &nstyle);
    if (styles == NULL) {
        return PC_OK;
    }
    if (nstyle > 0) {
        lens = pc_malloc_array(nstyle, sizeof *lens);
        bytes = pc_malloc_array(nstyle, sizeof *bytes);
        segs = pc_malloc_array(nstyle, sizeof *segs);
        if (lens == NULL || bytes == NULL || segs == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory reading Txt2");
            goto cleanup;
        }
    }
    for (k = 0; k < nstyle; k++) {
        if (!pc_ed_as_f64(pc_ed_get(&styles[k], "1"), &lens[k])) {
            lens[k] = 0.0;
        }
    }
    pc_text_utf16_to_byte_lengths(pc_str_c(text), text->len, lens, nstyle, bytes);
    for (k = 0; k < nstyle; k++) {
        if (bytes[k] > 0) {
            segs[nseg].len = bytes[k];
            segs[nseg].style = pc_ed_path(&styles[k], "0", "0", "6", (const char *)NULL);
            nseg++;
        }
    }
    st = pc_text_layer_char_runs(layer, &runs, &nruns, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    seg_end = nseg > 0 ? segs[0].len : SIZE_MAX;
    for (ri = 0; ri < nruns && st == PC_OK; ri++) {
        size_t end = at + runs[ri].len;
        while (at < end && st == PC_OK) {
            size_t piece_end;
            pc_char_style style;
            while (at >= seg_end && si + 1 < nseg) {
                si++;
                seg_end = seg_end > SIZE_MAX - segs[si].len ? SIZE_MAX : seg_end + segs[si].len;
            }
            piece_end = at < seg_end ? (end < seg_end ? end : seg_end) : end;
            st = pc_char_style_clone(&runs[ri].style, &style, err);
            if (st != PC_OK) {
                break;
            }
            /* Txt2 has the exact mode (EngineData's reading also marks neighbours of manual
             * kerns manual, like Photoshop's legacy reader). */
            if (si < nseg && segs[si].style != NULL && at < seg_end) {
                int64_t mode;
                if (pc_ed_as_i64(pc_ed_get(segs[si].style, "11"), &mode)) {
                    if (mode == 0) {
                        style.kerning = PC_KERNING_OFF;
                    } else if (mode == 1) {
                        style.kerning = PC_KERNING_METRICS;
                    } else if (mode == 2) {
                        style.kerning = PC_KERNING_OPTICAL;
                    }
                }
                st = add_txt2_features(&style, segs[si].style, err);
                if (st != PC_OK) {
                    pc_char_style_fini(&style);
                    break;
                }
            }
            if (out.n > 0 && pc_char_style_eq(&out.v[out.n - 1].style, &style)) {
                out.v[out.n - 1].len += piece_end - at;
                pc_char_style_fini(&style);
            } else {
                st = psd_run_list_push(&out, piece_end - at, &style, err);
                if (st != PC_OK) {
                    pc_char_style_fini(&style);
                }
            }
            at = piece_end;
        }
    }
    if (st == PC_OK) {
        st = psd_set_runs(layer, &out, err);
    }
cleanup:
    psd_run_list_fini(&out);
    pc_text_runs_free(runs, nruns);
    free(lens);
    free(bytes);
    free(segs);
    return st;
}

/* ---- frames (#1317) ---- */

/* The frame (`Txt2 /0 /8 /0` entry, its `/0`) of text object `index`: its rendered frame's `/10`
 * names it. */
static const pc_ed_value *txt2_frame(const pc_ed_value *txt2, size_t index)
{
    size_t nobj = 0, nf = 0, nr = 0, nframes = 0, k;
    const pc_ed_value *objs = pc_ed_as_array(pc_ed_path(txt2, "1", "1", (const char *)NULL), &nobj);
    const pc_ed_value *rendered, *frames, *list;
    int64_t frame = -1;
    bool found = false;
    if (objs == NULL || index >= nobj) {
        return NULL;
    }
    rendered = pc_ed_as_array(pc_ed_path(&objs[index], "1", "2", (const char *)NULL), &nr);
    if (rendered == NULL || nr == 0) {
        return NULL;
    }
    frames = pc_ed_as_array(pc_ed_get(&rendered[0], "6"), &nf);
    for (k = 0; frames != NULL && k < nf && !found; k++) {
        found = pc_ed_as_i64(pc_ed_get(&frames[k], "10"), &frame);
    }
    if (!found || frame < 0) {
        return NULL;
    }
    list = pc_ed_as_array(pc_ed_path(txt2, "0", "8", "0", (const char *)NULL), &nframes);
    if (list == NULL || (uint64_t)frame >= nframes) {
        return NULL;
    }
    return pc_ed_get(&list[frame], "0");
}

/* The numbers of an array value, at most `max` (the count of all of them in *n). */
static size_t arr_nums(const pc_ed_value *v, double *out, size_t max)
{
    size_t n = 0, len = 0, k;
    const pc_ed_value *a = pc_ed_as_array(v, &len);
    for (k = 0; a != NULL && k < len; k++) {
        double x;
        if (pc_ed_as_f64(&a[k], &x)) {
            if (n < max) {
                out[n] = x;
            }
            n++;
        }
    }
    return n;
}

static bool affine_is_identity(pc_affine a)
{
    return a.m[0] == 1.0 && a.m[1] == 0.0 && a.m[2] == 0.0 && a.m[3] == 1.0 && a.m[4] == 0.0 && a.m[5] == 0.0;
}

pc_status pc_txt2_apply_frame(pc_text_layer *layer, const uint8_t *tysh, size_t len, const pc_ed_value *txt2,
                              pc_error *err)
{
    pc_tysh t;
    pc_error tmp;
    const pc_psd_value *ti;
    const pc_ed_value *frame, *obj_text, *objs;
    size_t index, nobj = 0;
    double m[6], p[2], rect[4];
    pc_affine matrix = PC_AFFINE_IDENTITY, at = PC_AFFINE_IDENTITY, to_text;
    pc_point origin = {0.0, 0.0}, moved;
    pc_status st = pc_tysh_parse(tysh, len, &t, &tmp);
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? forward(err, &tmp, st) : PC_OK;
    }
    ti = pc_psd_desc_get(&t.text, "TextIndex");
    if (ti == NULL || ti->kind != PC_PSD_V_INTEGER || ti->i < 0) {
        goto done;
    }
    index = (size_t)ti->i;
    objs = pc_ed_as_array(pc_ed_path(txt2, "1", "1", (const char *)NULL), &nobj);
    obj_text = objs != NULL && index < nobj ? pc_ed_path(&objs[index], "0", "0", (const char *)NULL) : NULL;
    if (pc_ed_as_str(obj_text) == NULL || !same_text_cr(pc_ed_as_str(obj_text), &layer->text)) {
        goto done;
    }
    frame = txt2_frame(txt2, index);
    if (frame == NULL) {
        goto done;
    }
    if (arr_nums(pc_ed_path(frame, "2", "2", (const char *)NULL), m, 6) == 6 && isfinite(m[0]) &&
        isfinite(m[1]) && isfinite(m[2]) && isfinite(m[3]) && isfinite(m[4]) && isfinite(m[5])) {
        memcpy(matrix.m, m, sizeof m);
    }
    if (layer->shape.kind != PC_TEXT_SHAPE_BOX && arr_nums(pc_ed_get(frame, "0"), p, 2) == 2 && isfinite(p[0]) &&
        isfinite(p[1])) {
        at = pc_affine_translate(p[0], p[1]);
    }
    to_text = pc_affine_mul(matrix, at);
    if (affine_is_identity(to_text) || fabs(pc_affine_determinant(to_text)) <= 1e-9) {
        goto done;
    }
    /* The frame's origin (point text: the anchor; box text: the box's top left) must land in the
     * text-space `bounds` Photoshop wrote beside it, and not already be there without it. A TySh
     * PhotoCraft rewrote already has the frame folded into its transform and its `bounds` near
     * the origin: applying the frame again would move it twice. */
    if (layer->shape.kind == PC_TEXT_SHAPE_BOX) {
        pc_ed_value e;
        double b[2];
        size_t nb = 0;
        bool have = false;
        st = pc_tysh_engine_data(&t.text, &e, &tmp);
        if (st == PC_ERR_NOMEM) {
            (void)forward(err, &tmp, st);
            goto done;
        }
        if (st == PC_OK) {
            size_t nc = 0;
            const pc_ed_value *ch = pc_ed_as_array(
                pc_ed_path(&e, "EngineDict", "Rendered", "Shapes", "Children", (const char *)NULL), &nc);
            if (ch != NULL && nc > 0) {
                nb = arr_nums(pc_ed_path(&ch[0], "Cookie", "Photoshop", "BoxBounds", (const char *)NULL), b,
                              2);
            }
            have = nb >= 2;
            pc_ed_fini(&e);
        }
        st = PC_OK;
        if (!have) {
            goto done;
        }
        origin.x = b[0];
        origin.y = b[1];
    }
    if (!psd_descriptor_rect(&t.text, "bounds", rect)) {
        goto done;
    }
    moved = pc_affine_apply(to_text, origin);
#define INSIDE(q)                                                                                            \
    ((q).x >= rect[0] - 1.0 && (q).x <= rect[2] + 1.0 && (q).y >= rect[1] - 1.0 && (q).y <= rect[3] + 1.0)
    if (INSIDE(origin) || !INSIDE(moved)) {
        goto done;
    }
#undef INSIDE
    /* From the file's own transform: the frame's anchor is exact, so it replaces the ink-box
     * estimate legacy_point_origin made for such point text (#1469). */
    layer->transform = pc_affine_mul(t.transform, to_text);
done:
    pc_tysh_fini(&t);
    return st;
}

/* ---- moving and numbering TySh blocks ---- */

pc_status pc_tysh_translate(const uint8_t *tysh, size_t len, double dx, double dy, pc_psd_bytes *out,
                            pc_error *err)
{
    uint8_t *b;
    if (tysh == NULL || len < 50) {
        return pc_fail(err, PC_ERR_FORMAT, "TySh block too short to move");
    }
    b = malloc(len);
    if (b == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory moving TySh");
    }
    memcpy(b, tysh, len);
    /* Version (2 bytes), then xx, xy, yx, yy, tx, ty as big-endian f64: tx at 34, ty at 42. */
    pc_store_be64(b + 34, pc_f64_to_bits(pc_f64_from_bits(pc_load_be64(b + 34)) + dx));
    pc_store_be64(b + 42, pc_f64_to_bits(pc_f64_from_bits(pc_load_be64(b + 42)) + dy));
    out->data = b;
    out->len = len;
    return PC_OK;
}

pc_status pc_tysh_text_index(const uint8_t *tysh, size_t len, bool *has, int32_t *index, pc_error *err)
{
    pc_tysh t;
    pc_error tmp;
    const pc_psd_value *v;
    pc_status st = pc_tysh_parse(tysh, len, &t, &tmp);
    *has = false;
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? forward(err, &tmp, st) : PC_OK;
    }
    v = pc_psd_desc_get(&t.text, "TextIndex");
    if (v != NULL && v->kind == PC_PSD_V_INTEGER && v->i >= 0 && v->i <= PC_TXT2_MAX_TEXT_INDEX) {
        *has = true;
        *index = v->i;
    }
    pc_tysh_fini(&t);
    return PC_OK;
}

pc_status pc_tysh_set_text_index(const uint8_t *tysh, size_t len, int32_t index, pc_psd_bytes *out,
                                 pc_error *err)
{
    pc_tysh t;
    size_t k;
    pc_status st = pc_tysh_parse(tysh, len, &t, err);
    bool set = false;
    if (st != PC_OK) {
        return st;
    }
    for (k = 0; k < t.text.n && !set; k++) {
        if (pc_psd_id_is(&t.text.items[k].key, "TextIndex")) {
            pc_psd_value_fini(&t.text.items[k].value);
            memset(&t.text.items[k].value, 0, sizeof t.text.items[k].value);
            t.text.items[k].value.kind = PC_PSD_V_INTEGER;
            t.text.items[k].value.i = index;
            set = true;
        }
    }
    if (!set) {
        pc_psd_value v;
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_INTEGER;
        v.i = index;
        st = pc_psd_desc_add(&t.text, "TextIndex", &v, err);
    }
    if (st == PC_OK) {
        st = pc_tysh_write(&t, out, err);
    }
    pc_tysh_fini(&t);
    return st;
}

/* ---- building Txt2 for export ---- */

pc_status pc_txt2_needed(const pc_txt2_object *objects, size_t n, bool *out, pc_error *err)
{
    size_t i, k;
    *out = false;
    for (i = 0; i < n && !*out; i++) {
        pc_text_run *runs = NULL;
        size_t nruns = 0;
        pc_status st;
        if (objects[i].layer == NULL) {
            continue;
        }
        st = pc_text_layer_char_runs(objects[i].layer, &runs, &nruns, err);
        if (st != PC_OK) {
            return st;
        }
        for (k = 0; k < nruns && !*out; k++) {
            *out = runs[k].style.kerning == PC_KERNING_OPTICAL;
        }
        pc_text_runs_free(runs, nruns);
    }
    return PC_OK;
}

/* The Txt2 auto-kern mode of a style: 0 manual, 1 metrics, 2 optical. */
static int64_t txt2_mode(const pc_char_style *s)
{
    if (psd_manual(s)) {
        return 0;
    }
    return s->kerning == PC_KERNING_OPTICAL ? 2 : 1;
}

typedef struct mode_run {
    int64_t mode;
    size_t len; /* UTF-16 units */
} mode_run;

/* txt2_mode_runs: (mode, UTF-16 length) over the engine text (`\r` breaks plus the trailing
 * `\r`, which takes the last run's mode), split where the auto-kern mode changes. */
static pc_status txt2_mode_runs(const pc_text_layer *layer, mode_run **out, size_t *n, pc_error *err)
{
    pc_text_run *runs = NULL;
    size_t nruns = 0, ri = 0, run_end, b = 0, cap = 0, m = 0;
    const uint8_t *text = (const uint8_t *)pc_str_c(&layer->text);
    size_t tlen = layer->text.len;
    mode_run *v = NULL;
    pc_status st = pc_text_layer_char_runs(layer, &runs, &nruns, err);
    if (st != PC_OK) {
        return st;
    }
    run_end = nruns > 0 ? runs[0].len : 0;
    /* The text plus the trailing `\r` (one byte, one unit). */
    while (b <= tlen && st == PC_OK) {
        size_t at = b, units;
        int64_t mode;
        if (b < tlen) {
            uint32_t cp = psd_next_char(text, tlen, &at);
            units = psd_utf16_len(cp);
        } else {
            at = b + 1;
            units = 1;
        }
        while (b >= run_end && ri + 1 < nruns) {
            ri++;
            run_end = run_end > SIZE_MAX - runs[ri].len ? SIZE_MAX : run_end + runs[ri].len;
        }
        mode = ri < nruns ? txt2_mode(&runs[ri].style) : 1;
        if (m > 0 && v[m - 1].mode == mode) {
            v[m - 1].len += units;
        } else {
            mode_run *g = pc_grow_array(v, &cap, m + 1, sizeof *g);
            if (g == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory building Txt2");
                break;
            }
            v = g;
            v[m].mode = mode;
            v[m].len = units;
            m++;
        }
        b = at;
    }
    pc_text_runs_free(runs, nruns);
    if (st != PC_OK) {
        free(v);
        return st;
    }
    *out = v;
    *n = m;
    return PC_OK;
}

/* with_modes: the file's own style runs with the auto-kern modes set at `/0 /0 /6 /11`, keeping
 * every other style key; a run a mode change falls inside is split there. *ok is false (None)
 * when the runs don't cover the text or lack a style dictionary. */
static pc_status with_modes(const pc_ed_value *runs, size_t nruns, const mode_run *modes, size_t nmodes,
                            pc_ed_value *out, bool *ok, pc_error *err)
{
    size_t k, total = 0, sum = 0, mi = 0, mode_end, at = 0;
    pc_ed_value arr = pc_ed_array();
    pc_status st = PC_OK;
    *ok = false;
    for (k = 0; k < nruns; k++) {
        int64_t l;
        if (!pc_ed_as_i64(pc_ed_get(&runs[k], "1"), &l) || l < 0 || (uint64_t)l > SIZE_MAX ||
            sum > SIZE_MAX - (size_t)l) {
            return PC_OK;
        }
        sum += (size_t)l;
    }
    for (k = 0; k < nmodes; k++) {
        if (total > SIZE_MAX - modes[k].len) {
            return PC_OK;
        }
        total += modes[k].len;
    }
    if (sum != total || nmodes == 0) {
        return PC_OK;
    }
    mode_end = modes[0].len;
    for (k = 0; k < nruns && st == PC_OK; k++) {
        int64_t l = 0;
        size_t len, end;
        (void)pc_ed_as_i64(pc_ed_get(&runs[k], "1"), &l);
        len = (size_t)l;
        if (len == 0) {
            pc_ed_value c;
            st = pc_ed_clone(&runs[k], &c, err);
            if (st == PC_OK) {
                st = pc_ed_push(&arr, &c, err);
            }
            continue;
        }
        end = at + len;
        while (at < end && st == PC_OK) {
            size_t piece_end;
            pc_ed_value piece, *style;
            int64_t have;
            while (at >= mode_end && mi + 1 < nmodes) {
                mi++;
                mode_end = mode_end > SIZE_MAX - modes[mi].len ? SIZE_MAX : mode_end + modes[mi].len;
            }
            piece_end = end < mode_end ? end : mode_end;
            if (piece_end <= at) {
                pc_ed_fini(&arr);
                return PC_OK;
            }
            st = pc_ed_clone(&runs[k], &piece, err);
            if (st != PC_OK) {
                break;
            }
            if (piece_end - at != len) {
                pc_ed_value v = pc_ed_int((int64_t)(piece_end - at));
                st = pc_ed_set(&piece, "1", &v, err);
            }
            style = pc_ed_get_mut(pc_ed_get_mut(pc_ed_get_mut(&piece, "0"), "0"), "6");
            if (st == PC_OK && (style == NULL || style->kind != PC_ED_DICT)) {
                pc_ed_fini(&piece);
                pc_ed_fini(&arr);
                return PC_OK;
            }
            /* Keep the file's value (and its number type) when the mode is the same. */
            if (st == PC_OK && !(pc_ed_as_i64(pc_ed_get(style, "11"), &have) && have == modes[mi].mode)) {
                pc_ed_value v = pc_ed_int(modes[mi].mode);
                st = pc_ed_set(style, "11", &v, err);
            }
            if (st == PC_OK) {
                st = pc_ed_push(&arr, &piece, err);
            }
            if (st != PC_OK) {
                pc_ed_fini(&piece);
                break;
            }
            at = piece_end;
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&arr);
        return st;
    }
    *out = arr;
    *ok = true;
    return PC_OK;
}

/* The engine text of a layer: `\n` as `\r`, plus a trailing `\r`. */
static pc_status engine_text(const pc_text_layer *layer, pc_str *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    size_t k;
    pc_status st = PC_OK;
    for (k = 0; k < layer->text.len && st == PC_OK; k++) {
        char c = pc_str_c(&layer->text)[k];
        st = pc_buf_append_byte(&b, (uint8_t)(c == '\n' ? '\r' : c), err);
    }
    if (st == PC_OK) {
        st = pc_buf_append_byte(&b, '\r', err);
    }
    if (st == PC_OK) {
        st = pc_str_set(out, pc_buf_cstr(&b), b.len, err);
    }
    pc_buf_fini(&b);
    return st;
}

/* `<< /0 << /0 << /0 () /6 << /0 0 /11 mode >> >> >> /1 len >>`. */
static pc_status generated_run(int64_t mode, size_t len, pc_ed_value *out, pc_error *err)
{
    pc_ed_value style = pc_ed_dict(), data = pc_ed_dict(), inner = pc_ed_dict(), run = pc_ed_dict(), v;
    pc_status st;
    v = pc_ed_int(0);
    st = pc_ed_set(&style, "0", &v, err);
    if (st == PC_OK) {
        v = pc_ed_int(mode);
        st = pc_ed_set(&style, "11", &v, err);
    }
    if (st == PC_OK) {
        st = pc_ed_string("", 0, &v, err);
        if (st == PC_OK) {
            st = pc_ed_set(&data, "0", &v, err);
        }
    }
    if (st == PC_OK) {
        st = pc_ed_set(&data, "6", &style, err);
    }
    if (st == PC_OK) {
        st = pc_ed_set(&inner, "0", &data, err);
    }
    if (st == PC_OK) {
        st = pc_ed_set(&run, "0", &inner, err);
    }
    if (st == PC_OK) {
        v = pc_ed_int((int64_t)len);
        st = pc_ed_set(&run, "1", &v, err);
    }
    pc_ed_fini(&style);
    pc_ed_fini(&data);
    pc_ed_fini(&inner);
    if (st != PC_OK) {
        pc_ed_fini(&run);
        return st;
    }
    *out = run;
    return PC_OK;
}

/* One text object for `layer` at slot `at` (kept: the file's object there holds the same text). */
static pc_status build_object(const pc_text_layer *layer, const pc_ed_value *prev_objects, size_t nprev,
                              size_t at, pc_ed_value *out, pc_error *err)
{
    pc_str text = PC_STR_INIT;
    mode_run *modes = NULL;
    size_t nmodes = 0, k;
    const pc_ed_value *kept = NULL, *pm;
    pc_ed_value object = pc_ed_dict(), model = pc_ed_dict(), runs = pc_ed_array(), v;
    bool have_runs = false;
    pc_status st = engine_text(layer, &text, err);
    if (st == PC_OK) {
        st = txt2_mode_runs(layer, &modes, &nmodes, err);
    }
    if (st == PC_OK && at < nprev) {
        const pc_str *t = pc_ed_as_str(pc_ed_path(&prev_objects[at], "0", "0", (const char *)NULL));
        if (t != NULL && t->len == text.len && memcmp(pc_str_c(t), pc_str_c(&text), t->len) == 0) {
            kept = &prev_objects[at];
        }
    }
    if (st == PC_OK && kept != NULL) {
        pc_ed_fini(&object);
        st = pc_ed_clone(kept, &object, err);
    }
    pm = pc_ed_get(&object, "0");
    if (st == PC_OK && pm != NULL && pm->kind == PC_ED_DICT) {
        pc_ed_fini(&model);
        st = pc_ed_clone(pm, &model, err);
    }
    if (st == PC_OK && kept != NULL) {
        size_t nfr = 0;
        const pc_ed_value *fr = pc_ed_as_array(pc_ed_path(&model, "6", "0", (const char *)NULL), &nfr);
        if (fr != NULL) {
            pc_ed_fini(&runs);
            st = with_modes(fr, nfr, modes, nmodes, &runs, &have_runs, err);
            if (!have_runs) {
                runs = pc_ed_array();
            }
        }
    }
    for (k = 0; st == PC_OK && !have_runs && k < nmodes; k++) {
        st = generated_run(modes[k].mode, modes[k].len, &v, err);
        if (st == PC_OK) {
            st = pc_ed_push(&runs, &v, err);
        }
    }
    if (st == PC_OK) {
        st = pc_ed_string(pc_str_c(&text), text.len, &v, err);
        if (st == PC_OK) {
            st = pc_ed_set(&model, "0", &v, err);
        }
    }
    if (st == PC_OK) {
        pc_ed_value *sr = pc_ed_get_mut(&model, "6");
        if (sr != NULL && sr->kind == PC_ED_DICT) {
            st = pc_ed_set(sr, "0", &runs, err);
        } else {
            pc_ed_value d = pc_ed_dict();
            st = pc_ed_set(&d, "0", &runs, err);
            if (st == PC_OK) {
                st = pc_ed_set(&model, "6", &d, err);
            }
            pc_ed_fini(&d);
        }
    }
    if (st == PC_OK) {
        st = pc_ed_set(&object, "0", &model, err);
    }
    pc_ed_fini(&runs);
    pc_ed_fini(&model);
    pc_str_fini(&text);
    free(modes);
    if (st != PC_OK) {
        pc_ed_fini(&object);
        return st;
    }
    *out = object;
    return PC_OK;
}

pc_status pc_txt2_build(const pc_txt2_object *objects, size_t n, const uint8_t *previous, size_t prev_len,
                        bool has_previous, pc_buf *out, pc_error *err)
{
    pc_ed_value prev, outer, slots = pc_ed_array();
    bool has_prev = false;
    const pc_ed_value *prev_objects = NULL;
    size_t nprev = 0, i;
    pc_error tmp;
    pc_status st = PC_OK;
    if (has_previous) {
        st = pc_txt2_parse(previous, prev_len, &prev, &tmp);
        if (st == PC_ERR_NOMEM) {
            return forward(err, &tmp, st);
        }
        has_prev = st == PC_OK;
        st = PC_OK;
    }
    if (has_prev) {
        prev_objects = pc_ed_as_array(pc_ed_path(&prev, "1", "1", (const char *)NULL), &nprev);
        if (prev_objects == NULL) {
            nprev = 0;
        }
    }
    for (i = 0; i < nprev && st == PC_OK; i++) {
        pc_ed_value c;
        st = pc_ed_clone(&prev_objects[i], &c, err);
        if (st == PC_OK) {
            st = pc_ed_push(&slots, &c, err);
        }
    }
    for (i = 0; i < n && st == PC_OK; i++) {
        int32_t index = objects[i].index;
        size_t at;
        pc_ed_value object;
        if (index < 0 || index > PC_TXT2_MAX_TEXT_INDEX || objects[i].layer == NULL) {
            continue;
        }
        at = (size_t)index;
        st = build_object(objects[i].layer, prev_objects, nprev, at, &object, err);
        while (st == PC_OK && slots.n <= at) {
            pc_ed_value d = pc_ed_dict();
            st = pc_ed_push(&slots, &d, err);
        }
        if (st == PC_OK) {
            pc_ed_fini(&slots.items[at]);
            slots.items[at] = object;
        } else if (st != PC_OK) {
            pc_ed_fini(&object);
        }
    }
    /* The outer dictionary keeps whatever the block held (`/98` version, block extras, ...), and
     * `/1` whatever it holds beside the text objects. */
    if (st == PC_OK) {
        if (has_prev) {
            st = pc_ed_clone(&prev, &outer, err);
        } else {
            pc_ed_value ver = pc_ed_dict(), v = pc_ed_int(14), z = pc_ed_dict();
            outer = pc_ed_dict();
            st = pc_ed_set(&ver, "0", &v, err);
            if (st == PC_OK) {
                st = pc_ed_set(&outer, "98", &ver, err);
            }
            if (st == PC_OK) {
                st = pc_ed_set(&outer, "0", &z, err);
            }
            pc_ed_fini(&ver);
            if (st != PC_OK) {
                pc_ed_fini(&outer);
            }
        }
    }
    if (st == PC_OK) {
        pc_ed_value *objs = pc_ed_get_mut(&outer, "1");
        if (objs != NULL && objs->kind == PC_ED_DICT) {
            st = pc_ed_set(objs, "1", &slots, err);
        } else {
            pc_ed_value d = pc_ed_dict();
            st = pc_ed_set(&d, "1", &slots, err);
            if (st == PC_OK) {
                st = pc_ed_set(&outer, "1", &d, err);
            }
            pc_ed_fini(&d);
        }
        if (st == PC_OK) {
            /* Nothing changed: the file's own bytes (writing them back would re-encode strings and
             * numbers). */
            if (has_prev && pc_ed_eq(&prev, &outer)) {
                st = pc_buf_append(out, previous, prev_len, err);
            } else {
                st = pc_ed_write_bare(&outer, out, err);
            }
        }
        pc_ed_fini(&outer);
    }
    pc_ed_fini(&slots);
    if (has_prev) {
        pc_ed_fini(&prev);
    }
    return st;
}
