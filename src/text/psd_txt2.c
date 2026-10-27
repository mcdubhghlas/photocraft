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
    /* `bytes`/`segs` are non-NULL whenever the original nstyle > 0, and the produced count is at
     * most that, so the guard only reassures the analyzer. */
    nstyle = bytes != NULL ? pc_text_utf16_to_byte_lengths(pc_str_c(text), text->len, lens, nstyle, bytes) : 0;
    for (k = 0; bytes != NULL && k < nstyle; k++) {
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
        /* Where Photoshop writes it: right before EngineData (else at the end). */
        size_t at = t.text.n;
        pc_psd_value v;
        for (k = 0; k < t.text.n; k++) {
            if (pc_psd_id_is(&t.text.items[k].key, "EngineData")) {
                at = k;
                break;
            }
        }
        memset(&v, 0, sizeof v);
        v.kind = PC_PSD_V_INTEGER;
        v.i = index;
        st = pc_psd_desc_add(&t.text, "TextIndex", &v, err);
        if (st == PC_OK && at + 1 < t.text.n) {
            pc_psd_item ins = t.text.items[t.text.n - 1];
            memmove(&t.text.items[at + 1], &t.text.items[at], (t.text.n - 1 - at) * sizeof(pc_psd_item));
            t.text.items[at] = ins;
        }
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

/* ---- refit budgets and shape tests (port of psd.rs fits/fits_budget/is_stub/...) ---- */

/* The most values a Txt2 object may hold to serve as the template for new objects. */
#define TXT2_TEMPLATE_NODES 4096u
/* Cumulative node and byte budgets across all run dictionaries cloned while refitting one object. */
#define TXT2_MAX_REFIT_RUN_NODES 16384u
#define TXT2_MAX_REFIT_RUN_BYTES (1u << 20)

/* Sets dict[key] = *v (taken over, also on failure). */
static pc_status edset(pc_ed_value *d, const char *key, pc_ed_value *v, pc_error *err)
{
    pc_status st = pc_ed_set(d, key, v, err);
    pc_ed_fini(v);
    return st;
}

/* Appends *v to an array (taken over, also on failure). */
static pc_status edpush(pc_ed_value *a, pc_ed_value *v, pc_error *err)
{
    pc_status st = pc_ed_push(a, v, err);
    pc_ed_fini(v);
    return st;
}

static pc_status edpush_real(pc_ed_value *a, double r, pc_error *err)
{
    pc_ed_value v = pc_ed_real(r);
    return pc_ed_push(a, &v, err);
}

/* fits: whether v holds at most *budget values, counting down (stops at the first one over). */
static bool txt2_fits(const pc_ed_value *v, size_t *budget)
{
    size_t k;
    if (*budget == 0) {
        return false;
    }
    (*budget)--;
    if (v->kind == PC_ED_DICT) {
        for (k = 0; k < v->n; k++) {
            if (!txt2_fits(&v->entries[k].value, budget)) {
                return false;
            }
        }
    } else if (v->kind == PC_ED_ARRAY) {
        for (k = 0; k < v->n; k++) {
            if (!txt2_fits(&v->items[k], budget)) {
                return false;
            }
        }
    }
    return true;
}

/* fits_budget: whether v fits within the remaining (nodes, bytes) budget, deducting its nodes and
 * string/key bytes as it walks. */
static bool txt2_fits_budget(const pc_ed_value *v, size_t *nodes, size_t *bytes)
{
    size_t k;
    if (*nodes == 0) {
        return false;
    }
    (*nodes)--;
    switch (v->kind) {
    case PC_ED_DICT:
        for (k = 0; k < v->n; k++) {
            size_t kl = v->entries[k].key.len;
            if (*bytes < kl) {
                return false;
            }
            *bytes -= kl;
            if (!txt2_fits_budget(&v->entries[k].value, nodes, bytes)) {
                return false;
            }
        }
        return true;
    case PC_ED_ARRAY:
        for (k = 0; k < v->n; k++) {
            if (!txt2_fits_budget(&v->items[k], nodes, bytes)) {
                return false;
            }
        }
        return true;
    case PC_ED_STRING:
    case PC_ED_NAME: {
        size_t sl = v->s.len;
        if (*bytes < sl) {
            return false;
        }
        *bytes -= sl;
        return true;
    }
    default:
        return true;
    }
}

/* only(v, keys): v is a dictionary whose every key is in `keys`. */
static bool txt2_only_keys(const pc_ed_value *v, const char *const *keys, size_t nk)
{
    size_t i, j;
    if (v->kind != PC_ED_DICT) {
        return false;
    }
    for (i = 0; i < v->n; i++) {
        bool ok = false;
        for (j = 0; j < nk && !ok; j++) {
            ok = pc_str_eq_c(&v->entries[i].key, keys[j]);
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* is_stub: the bare `<< /0 << /0 text /6 runs >> >>` object (no view, no paragraph runs). */
static bool txt2_is_stub(const pc_ed_value *object)
{
    static const char *const k0[] = {"0"};
    static const char *const k06[] = {"0", "6"};
    const pc_ed_value *m;
    if (!txt2_only_keys(object, k0, 1)) {
        return false;
    }
    m = pc_ed_get(object, "0");
    return m != NULL && txt2_only_keys(m, k06, 2);
}

/* names_one_frame: the object has a view naming one frame (`/1 /0 [ << /0 n >> ]`). */
static bool txt2_names_one_frame(const pc_ed_value *object)
{
    size_t nf = 0;
    const pc_ed_value *f = pc_ed_as_array(pc_ed_path(object, "1", "0", (const char *)NULL), &nf);
    return f != NULL && nf == 1 && pc_ed_get(&f[0], "0") != NULL;
}

/* UTF-16 unit count of UTF-8 text s[0..len). */
static size_t txt2_utf16_len(const char *s, size_t len)
{
    const uint8_t *b = (const uint8_t *)s;
    size_t i = 0, n = 0;
    while (i < len) {
        size_t at = i;
        uint32_t cp = psd_next_char(b, len, &at);
        n += psd_utf16_len(cp);
        i = at;
    }
    return n;
}

/* UTF-16 length of each paragraph of `text` (split_inclusive('\r'); each keeps its '\r'). */
static pc_status txt2_paragraph_lens(const char *s, size_t len, size_t **out, size_t *n, pc_error *err)
{
    const uint8_t *b = (const uint8_t *)s;
    size_t i = 0, cur = 0, cnt = 0, cap = 0;
    size_t *v = NULL;
    bool started = false;
    while (i < len) {
        size_t at = i;
        uint32_t cp = psd_next_char(b, len, &at);
        cur += psd_utf16_len(cp);
        started = true;
        if (cp == '\r') {
            size_t *g = pc_grow_array(v, &cap, cnt + 1, sizeof *g);
            if (g == NULL) {
                free(v);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory building Txt2");
            }
            v = g;
            v[cnt++] = cur;
            cur = 0;
            started = false;
        }
        i = at;
    }
    if (started) {
        size_t *g = pc_grow_array(v, &cap, cnt + 1, sizeof *g);
        if (g == NULL) {
            free(v);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory building Txt2");
        }
        v = g;
        v[cnt++] = cur;
    }
    *out = v;
    *n = cnt;
    return PC_OK;
}

/* runs_per: `runs` remade with the lengths `lens` (run i keeps content of run i, else of the last).
 * *ok false (None) for empty runs or a cumulative nodes/bytes budget overrun. */
static pc_status runs_per(const pc_ed_value *runs, size_t nruns, const size_t *lens, size_t nlens,
                          const char *len_key, size_t *nodes, size_t *bytes, pc_ed_value *out, bool *ok,
                          pc_error *err)
{
    pc_ed_value arr = pc_ed_array();
    size_t i;
    pc_status st = PC_OK;
    *ok = false;
    if (nruns == 0) {
        return PC_OK; /* runs.last()? -> None */
    }
    for (i = 0; i < nlens; i++) {
        const pc_ed_value *src = i < nruns ? &runs[i] : &runs[nruns - 1];
        pc_ed_value r;
        if (!txt2_fits_budget(src, nodes, bytes)) {
            pc_ed_fini(&arr);
            return PC_OK;
        }
        st = pc_ed_clone(src, &r, err);
        if (st != PC_OK) {
            pc_ed_fini(&arr);
            return st;
        }
        if (lens[i] > (size_t)INT64_MAX) {
            pc_ed_fini(&r);
            pc_ed_fini(&arr);
            return PC_OK;
        }
        st = psd_set_int(&r, len_key, (int64_t)lens[i], err);
        if (st == PC_OK) {
            st = edpush(&arr, &r, err);
        } else {
            pc_ed_fini(&r);
        }
        if (st != PC_OK) {
            pc_ed_fini(&arr);
            return st;
        }
    }
    *out = arr;
    *ok = true;
    return PC_OK;
}

/* runs_to_length: `runs` covering `total` UTF-16 units (leading runs kept, cut where `total` ends,
 * the last kept run extended when longer; empty runs dropped). *ok false for empty runs, a length
 * that isn't a count, or a budget overrun. */
static pc_status runs_to_length(const pc_ed_value *runs, size_t nruns, size_t total, const char *len_key,
                                size_t *nodes, size_t *bytes, pc_ed_value *out, bool *ok, pc_error *err)
{
    pc_ed_value arr = pc_ed_array();
    size_t *takes = NULL, tcap = 0, cnt = 0, i, left = total;
    pc_status st = PC_OK;
    *ok = false;
    for (i = 0; i < nruns; i++) {
        int64_t l;
        size_t len, take;
        if (!pc_ed_as_i64(pc_ed_get(&runs[i], len_key), &l) || l < 0 || (uint64_t)l > SIZE_MAX) {
            goto none;
        }
        len = (size_t)l;
        take = len < left ? len : left;
        if (take > 0) {
            pc_ed_value r;
            size_t *g;
            if (!txt2_fits_budget(&runs[i], nodes, bytes)) {
                goto none;
            }
            st = pc_ed_clone(&runs[i], &r, err);
            if (st != PC_OK) {
                goto fail;
            }
            st = edpush(&arr, &r, err);
            if (st != PC_OK) {
                pc_ed_fini(&r);
                goto fail;
            }
            g = pc_grow_array(takes, &tcap, cnt + 1, sizeof *g);
            if (g == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory building Txt2");
                goto fail;
            }
            takes = g;
            takes[cnt++] = take;
            left -= take;
        }
    }
    if (left > 0) {
        if (cnt > 0) {
            if (takes[cnt - 1] > SIZE_MAX - left) {
                goto none; /* checked_add -> None */
            }
            takes[cnt - 1] += left;
        } else {
            const pc_ed_value *last;
            pc_ed_value r;
            size_t *g;
            if (nruns == 0) {
                goto none;
            }
            last = &runs[nruns - 1];
            if (!txt2_fits_budget(last, nodes, bytes)) {
                goto none;
            }
            st = pc_ed_clone(last, &r, err);
            if (st != PC_OK) {
                goto fail;
            }
            st = edpush(&arr, &r, err);
            if (st != PC_OK) {
                pc_ed_fini(&r);
                goto fail;
            }
            g = pc_grow_array(takes, &tcap, cnt + 1, sizeof *g);
            if (g == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory building Txt2");
                goto fail;
            }
            takes = g;
            takes[cnt++] = left;
        }
    }
    for (i = 0; i < cnt; i++) {
        if (takes[i] > (size_t)INT64_MAX) {
            goto none;
        }
        st = psd_set_int(&arr.items[i], len_key, (int64_t)takes[i], err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    free(takes);
    *out = arr;
    *ok = true;
    return PC_OK;
none:
    free(takes);
    pc_ed_fini(&arr);
    return PC_OK;
fail:
    free(takes);
    pc_ed_fini(&arr);
    return st;
}

/* edited_object: the file's Txt2 text object refitted to `text` (its text replaced, paragraph runs
 * `/5` one per paragraph, every other run array cut/extended to the new total; `/21` glyph pen
 * positions dropped). *ok false (None) when the object isn't shaped as Photoshop writes it. */
static pc_status edited_object(const pc_ed_value *object, const char *text, size_t tlen, pc_ed_value *out,
                               bool *ok, pc_error *err)
{
    pc_ed_value obj;
    pc_ed_value *model;
    size_t total, nodes = TXT2_MAX_REFIT_RUN_NODES, bytes = TXT2_MAX_REFIT_RUN_BYTES, k, *paras = NULL,
                  nparas = 0;
    pc_status st;
    *ok = false;
    st = pc_ed_clone(object, &obj, err);
    if (st != PC_OK) {
        return st;
    }
    pc_ed_remove(&obj, "21");
    model = pc_ed_get_mut(&obj, "0");
    if (model == NULL || model->kind != PC_ED_DICT) {
        pc_ed_fini(&obj);
        return PC_OK;
    }
    total = txt2_utf16_len(text, tlen);
    st = txt2_paragraph_lens(text, tlen, &paras, &nparas, err);
    if (st != PC_OK) {
        pc_ed_fini(&obj);
        return st;
    }
    for (k = 0; k < model->n; k++) {
        pc_ed_entry *e = &model->entries[k];
        const pc_ed_value *runs;
        size_t nr = 0, j;
        bool shaped;
        pc_ed_value fitted;
        bool fok = false;
        if (pc_str_eq_c(&e->key, "0")) {
            pc_ed_value sv;
            st = pc_ed_string(text, tlen, &sv, err);
            if (st != PC_OK) {
                goto fail;
            }
            pc_ed_fini(&e->value);
            e->value = sv;
            continue;
        }
        runs = pc_ed_as_array(pc_ed_get(&e->value, "0"), &nr);
        shaped = runs != NULL;
        for (j = 0; shaped && j < nr; j++) {
            shaped = runs[j].kind == PC_ED_DICT && pc_ed_get(&runs[j], "1") != NULL;
        }
        if (!shaped) {
            continue;
        }
        if (pc_str_eq_c(&e->key, "5")) {
            st = runs_per(runs, nr, paras, nparas, "1", &nodes, &bytes, &fitted, &fok, err);
        } else {
            st = runs_to_length(runs, nr, total, "1", &nodes, &bytes, &fitted, &fok, err);
        }
        if (st != PC_OK) {
            goto fail;
        }
        if (!fok) {
            pc_ed_fini(&obj); /* None */
            free(paras);
            return PC_OK;
        }
        st = edset(&e->value, "0", &fitted, err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    free(paras);
    *out = obj;
    *ok = true;
    return PC_OK;
fail:
    free(paras);
    pc_ed_fini(&obj);
    return st;
}

/* td_edited_object: the older-Photoshop object (`<< /Model << /Text .. /ParagraphRun .. >> >>`,
 * runs numbered by `/Length` in UTF-16 units) refitted to `text`. *ok false when it isn't shaped
 * as Photoshop writes it (the caller then leaves the file's own entry). */
static pc_status td_edited_object(const pc_ed_value *object, const char *text, size_t tlen,
                                  pc_ed_value *out, bool *ok, pc_error *err)
{
    pc_ed_value obj;
    pc_ed_value *model;
    size_t total, nodes = TXT2_MAX_REFIT_RUN_NODES, bytes = TXT2_MAX_REFIT_RUN_BYTES, k, *paras = NULL,
                  nparas = 0;
    bool has_text = false;
    pc_status st;
    *ok = false;
    st = pc_ed_clone(object, &obj, err);
    if (st != PC_OK) {
        return st;
    }
    model = pc_ed_get_mut(&obj, "Model");
    if (model == NULL || model->kind != PC_ED_DICT) {
        pc_ed_fini(&obj);
        return PC_OK;
    }
    for (k = 0; k < model->n; k++) {
        if (pc_str_eq_c(&model->entries[k].key, "Text") && model->entries[k].value.kind == PC_ED_STRING) {
            has_text = true;
        }
    }
    if (!has_text) {
        pc_ed_fini(&obj);
        return PC_OK;
    }
    total = txt2_utf16_len(text, tlen);
    st = txt2_paragraph_lens(text, tlen, &paras, &nparas, err);
    if (st != PC_OK) {
        pc_ed_fini(&obj);
        return st;
    }
    for (k = 0; k < model->n; k++) {
        pc_ed_entry *e = &model->entries[k];
        const pc_ed_value *runs;
        size_t nr = 0, j;
        bool shaped;
        pc_ed_value fitted;
        bool fok = false;
        if (pc_str_eq_c(&e->key, "Text")) {
            pc_ed_value sv;
            st = pc_ed_string(text, tlen, &sv, err);
            if (st != PC_OK) {
                goto fail;
            }
            pc_ed_fini(&e->value);
            e->value = sv;
            continue;
        }
        if (e->value.kind != PC_ED_DICT) {
            continue;
        }
        if (pc_ed_get(&e->value, "RunArray") == NULL) {
            continue;
        }
        runs = pc_ed_as_array(pc_ed_get(&e->value, "RunArray"), &nr);
        shaped = runs != NULL;
        for (j = 0; shaped && j < nr; j++) {
            shaped = runs[j].kind == PC_ED_DICT && pc_ed_get(&runs[j], "Length") != NULL;
        }
        if (!shaped) {
            pc_ed_fini(&obj); /* a named RunArray that is malformed -> None */
            free(paras);
            return PC_OK;
        }
        if (pc_str_eq_c(&e->key, "ParagraphRun")) {
            st = runs_per(runs, nr, paras, nparas, "Length", &nodes, &bytes, &fitted, &fok, err);
        } else {
            st = runs_to_length(runs, nr, total, "Length", &nodes, &bytes, &fitted, &fok, err);
        }
        if (st != PC_OK) {
            goto fail;
        }
        if (!fok) {
            pc_ed_fini(&obj);
            free(paras);
            return PC_OK;
        }
        st = edset(&e->value, "RunArray", &fitted, err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    free(paras);
    *out = obj;
    *ok = true;
    return PC_OK;
fail:
    free(paras);
    pc_ed_fini(&obj);
    return st;
}

/* new_object: a text object for slot `at` (the file's block has none) from `template`, refitted by
 * edited_object with its view naming frame `at` and its view runs remade for the new paragraphs. */
static pc_status new_object(const pc_ed_value *template_, size_t at, const char *text, size_t tlen,
                            pc_ed_value *out, bool *ok, pc_error *err)
{
    pc_ed_value obj, frameval, *view, *strikes, *vruns_slot;
    const pc_ed_value *fp;
    bool have_frame = false;
    size_t nparas = 0, *paras = NULL, ns = 0, s, nvr = 0;
    const pc_ed_value *vruns;
    pc_status st;
    *ok = false;
    if (at > (uint64_t)INT64_MAX) {
        return PC_OK; /* i64::try_from(at) -> None */
    }
    fp = pc_ed_path(template_, "1", "0", (const char *)NULL);
    if (pc_ed_as_array(fp, &ns) != NULL && ns > 0) {
        const pc_ed_value *f0 = pc_ed_get(&pc_ed_as_array(fp, &ns)[0], "0");
        if (f0 != NULL) {
            st = pc_ed_clone(f0, &frameval, err);
            if (st != PC_OK) {
                return st;
            }
            have_frame = true;
        }
    }
    st = edited_object(template_, text, tlen, &obj, ok, err);
    if (st != PC_OK || !*ok) {
        if (have_frame) {
            pc_ed_fini(&frameval);
        }
        *ok = false;
        return st;
    }
    *ok = false;
    view = pc_ed_get_mut(&obj, "1");
    if (view == NULL) {
        goto none;
    }
    /* view.set("0", [ << /0 at >> ]) */
    {
        pc_ed_value arr = pc_ed_array(), d = pc_ed_dict(), iv = pc_ed_int((int64_t)at);
        st = pc_ed_set(&d, "0", &iv, err);
        if (st == PC_OK) {
            st = edpush(&arr, &d, err);
        } else {
            pc_ed_fini(&d);
        }
        if (st == PC_OK) {
            st = edset(view, "0", &arr, err);
        } else {
            pc_ed_fini(&arr);
        }
        if (st != PC_OK) {
            goto fail;
        }
    }
    /* The rendered strikes' frame strike (`/F`) names the template's frame too. */
    strikes = pc_ed_get_mut(view, "2");
    if (strikes != NULL && strikes->kind == PC_ED_ARRAY) {
        for (s = 0; s < strikes->n; s++) {
            pc_ed_value *parts = pc_ed_get_mut(&strikes->items[s], "6");
            size_t p;
            if (parts == NULL || parts->kind != PC_ED_ARRAY) {
                continue;
            }
            for (p = 0; p < parts->n; p++) {
                pc_ed_value *part = &parts->items[p];
                const pc_str *tag = pc_ed_as_str(pc_ed_get(part, "99"));
                const pc_ed_value *p10 = pc_ed_get(part, "10");
                bool match10 = have_frame ? (p10 != NULL && pc_ed_eq(p10, &frameval)) : (p10 == NULL);
                if (tag != NULL && pc_str_eq_c(tag, "F") && match10) {
                    pc_ed_value iv = pc_ed_int((int64_t)at);
                    st = pc_ed_set(part, "10", &iv, err);
                    pc_ed_fini(&iv);
                    if (st != PC_OK) {
                        goto fail;
                    }
                }
            }
        }
    }
    vruns = pc_ed_as_array(pc_ed_path(view, "1", "0", (const char *)NULL), &nvr);
    if (vruns != NULL) {
        size_t nodes = TXT2_MAX_REFIT_RUN_NODES, bytes = TXT2_MAX_REFIT_RUN_BYTES;
        pc_ed_value fitted;
        bool fok = false;
        st = txt2_paragraph_lens(text, tlen, &paras, &nparas, err);
        if (st != PC_OK) {
            goto fail;
        }
        st = runs_per(vruns, nvr, paras, nparas, "1", &nodes, &bytes, &fitted, &fok, err);
        free(paras);
        paras = NULL;
        if (st != PC_OK) {
            goto fail;
        }
        if (!fok) {
            goto none;
        }
        vruns_slot = pc_ed_get_mut(view, "1");
        if (vruns_slot == NULL) {
            pc_ed_fini(&fitted);
            goto none;
        }
        st = edset(vruns_slot, "0", &fitted, err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    if (have_frame) {
        pc_ed_fini(&frameval);
    }
    *out = obj;
    *ok = true;
    return PC_OK;
none:
    if (have_frame) {
        pc_ed_fini(&frameval);
    }
    pc_ed_fini(&obj);
    free(paras);
    return PC_OK;
fail:
    if (have_frame) {
        pc_ed_fini(&frameval);
    }
    pc_ed_fini(&obj);
    free(paras);
    return st;
}

/* pad_frames: give `outer` a frame (`/0 /8 /0`) for each of its `count` text objects, filling
 * missing ones with the small point-text frame Photoshop writes. A block without a frame list is
 * left as it is. */
static pc_status pad_frames(pc_ed_value *outer, size_t count, pc_error *err)
{
    pc_ed_value *frames =
        pc_ed_get_mut(pc_ed_get_mut(pc_ed_get_mut(outer, "0"), "8"), "0");
    pc_status st = PC_OK;
    if (frames == NULL || frames->kind != PC_ED_ARRAY) {
        return PC_OK;
    }
    if (count > (size_t)PC_TXT2_MAX_TEXT_INDEX + 1) {
        count = (size_t)PC_TXT2_MAX_TEXT_INDEX + 1;
    }
    while (frames->n < count && st == PC_OK) {
        /* << /0 << /2 << /6 [ -1.0 -1.0 ] /11 << /4 -1 /7 false >> >> >> >> */
        pc_ed_value frame = pc_ed_dict(), lvl0 = pc_ed_dict(), data = pc_ed_dict(), rect = pc_ed_array(),
                    kind = pc_ed_dict();
        st = edpush_real(&rect, -1.0, err);
        if (st == PC_OK) {
            st = edpush_real(&rect, -1.0, err);
        }
        if (st == PC_OK) {
            st = psd_set_int(&kind, "4", -1, err);
        }
        if (st == PC_OK) {
            st = psd_set_bool(&kind, "7", false, err);
        }
        if (st == PC_OK) {
            st = edset(&data, "6", &rect, err);
        } else {
            pc_ed_fini(&rect);
        }
        if (st == PC_OK) {
            st = edset(&data, "11", &kind, err);
        } else {
            pc_ed_fini(&kind);
        }
        if (st == PC_OK) {
            st = edset(&lvl0, "2", &data, err);
        } else {
            pc_ed_fini(&data);
        }
        if (st == PC_OK) {
            st = edset(&frame, "0", &lvl0, err);
        } else {
            pc_ed_fini(&lvl0);
        }
        if (st == PC_OK) {
            st = edpush(frames, &frame, err);
        } else {
            pc_ed_fini(&frame);
        }
    }
    return st;
}

/* txt2_font: the index of the PostScript font `name` in the font list `fonts` (`/0 /1 /0`),
 * appending it (shaped like the first entry) when missing. *found false for an empty list. */
static pc_status txt2_font(pc_ed_value *fonts, const char *name, size_t namelen, int64_t *out, bool *found,
                           pc_error *err)
{
    size_t k, newidx;
    pc_ed_value entry, *font;
    const pc_ed_value *kp;
    bool have_kind = false;
    pc_ed_value kindc;
    pc_status st;
    *found = false;
    for (k = 0; k < fonts->n; k++) {
        const pc_str *nm = pc_ed_as_str(pc_ed_path(&fonts->items[k], "0", "0", "0", (const char *)NULL));
        if (nm != NULL && nm->len == namelen && memcmp(pc_str_c(nm), name, namelen) == 0) {
            if (k > (size_t)INT64_MAX) {
                return PC_OK;
            }
            *out = (int64_t)k;
            *found = true;
            return PC_OK;
        }
    }
    if (fonts->n == 0) {
        return PC_OK; /* fonts.first()? -> None */
    }
    st = pc_ed_clone(&fonts->items[0], &entry, err);
    if (st != PC_OK) {
        return st;
    }
    font = pc_ed_get_mut(pc_ed_get_mut(&entry, "0"), "0");
    if (font == NULL) {
        pc_ed_fini(&entry); /* None */
        return PC_OK;
    }
    kp = pc_ed_get(font, "2");
    if (kp != NULL) {
        st = pc_ed_clone(kp, &kindc, err);
        if (st != PC_OK) {
            pc_ed_fini(&entry);
            return st;
        }
        have_kind = true;
    }
    pc_ed_fini(font);
    *font = pc_ed_dict();
    {
        pc_ed_value sv;
        st = pc_ed_string(name, namelen, &sv, err);
        if (st == PC_OK) {
            st = edset(font, "0", &sv, err);
        }
    }
    if (st == PC_OK && have_kind) {
        st = edset(font, "2", &kindc, err);
        have_kind = false;
    }
    if (have_kind) {
        pc_ed_fini(&kindc);
    }
    if (st != PC_OK) {
        pc_ed_fini(&entry);
        return st;
    }
    newidx = fonts->n;
    st = edpush(fonts, &entry, err);
    if (st != PC_OK) {
        return st;
    }
    if (newidx > (size_t)INT64_MAX) {
        return PC_OK;
    }
    *out = (int64_t)newidx;
    *found = true;
    return PC_OK;
}

/* The fill colour (type + alpha-first values) a Txt2 style stores. */
static pc_status txt2_fill(const pc_color *c, int64_t *kind, pc_ed_value *out, pc_error *err)
{
    pc_ed_value values = pc_ed_array();
    size_t f;
    pc_status st;
    *kind = c->mode == PC_COLOR_MODE_CMYK ? 2 : c->mode == PC_COLOR_MODE_GRAYSCALE ? 0 : 1;
    st = edpush_real(&values, (double)c->alpha, err);
    if (c->mode == PC_COLOR_MODE_CMYK) {
        for (f = 0; f < 4 && st == PC_OK; f++) {
            st = edpush_real(&values, (double)c->c[f], err);
        }
    } else if (c->mode == PC_COLOR_MODE_GRAYSCALE) {
        if (st == PC_OK) {
            st = edpush_real(&values, (double)c->c[0], err);
        }
    } else {
        float rgb[3];
        pc_color_to_rgb(c, rgb);
        for (f = 0; f < 3 && st == PC_OK; f++) {
            st = edpush_real(&values, (double)rgb[f], err);
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&values);
        return st;
    }
    *out = values;
    return PC_OK;
}

/* Whether a style turns the OpenType feature `tag` on (*present set when the run names it). */
static bool txt2_feature_on(const pc_char_style *s, const char *tag, bool *present)
{
    size_t g;
    *present = false;
    for (g = 0; g < s->nfeatures; g++) {
        if (pc_str_eq_c(&s->features[g].tag, tag)) {
            *present = true;
            return s->features[g].value > 0;
        }
    }
    return false;
}

/* txt2_style_runs: one Txt2 style run per character run, `base` (one of the file's style runs)
 * restyled from the run's own style (font appended to `fonts`). *ok false (None) when `base` has
 * no style dictionary or is larger than a template may be. k from the document dpi. */
static pc_status txt2_style_runs(const pc_text_layer *layer, const pc_ed_value *base, pc_ed_value *fonts,
                                 float dpi, pc_ed_value *out, bool *ok, pc_error *err)
{
    pc_ed_value arr = pc_ed_array();
    pc_text_run *runs = NULL;
    pc_str edtext = PC_STR_INIT;
    pc_char_style defstyle;
    struct {
        size_t ri;
        size_t n;
    } *spans = NULL;
    size_t nruns = 0, nspans = 0, scap = 0, budget = TXT2_TEMPLATE_NODES, ri = 0, run_end, b, i;
    float k;
    pc_status st;
    *ok = false;
    pc_char_style_default(&defstyle);
    if (pc_ed_path(base, "0", "0", "6", (const char *)NULL) == NULL ||
        pc_ed_path(base, "0", "0", "6", (const char *)NULL)->kind != PC_ED_DICT || !txt2_fits(base, &budget)) {
        pc_char_style_fini(&defstyle);
        return PC_OK;
    }
    k = (dpi > 0.0f && isfinite(dpi)) ? dpi / 72.0f : 1.0f;
    st = pc_text_layer_char_runs(layer, &runs, &nruns, err);
    if (st != PC_OK) {
        pc_char_style_fini(&defstyle);
        return st;
    }
    st = engine_text(layer, &edtext, err);
    if (st != PC_OK) {
        goto done;
    }
    run_end = nruns > 0 ? runs[0].len : 0;
    b = 0;
    while (b < edtext.len) {
        size_t at = b, units;
        uint32_t cp = psd_next_char((const uint8_t *)pc_str_c(&edtext), edtext.len, &at);
        units = psd_utf16_len(cp);
        while (b >= run_end && ri + 1 < nruns) {
            ri++;
            run_end = run_end > SIZE_MAX - runs[ri].len ? SIZE_MAX : run_end + runs[ri].len;
        }
        if (nspans > 0 && spans[nspans - 1].ri == ri) {
            spans[nspans - 1].n += units;
        } else {
            void *g = pc_grow_array(spans, &scap, nspans + 1, sizeof *spans);
            if (g == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory building Txt2");
                goto done;
            }
            spans = g;
            spans[nspans].ri = ri;
            spans[nspans].n = units;
            nspans++;
        }
        b = at;
    }
    for (i = 0; i < nspans && st == PC_OK; i++) {
        const pc_char_style *s = spans[i].ri < nruns ? &runs[spans[i].ri].style : &defstyle;
        pc_ed_value run, *style, *paint;
        pc_str ps = PC_STR_INIT;
        int64_t font = 0, sets = 0, kind;
        bool found = false, bval;
        size_t fi;
        st = pc_ed_clone(base, &run, err);
        if (st != PC_OK) {
            break;
        }
        style = pc_ed_get_mut(pc_ed_get_mut(pc_ed_get_mut(&run, "0"), "0"), "6");
        if (style == NULL) {
            pc_ed_fini(&run);
            continue;
        }
        st = pc_text_postscript_for(s, &ps, err);
        if (st == PC_OK) {
            st = txt2_font(fonts, pc_str_c(&ps), ps.len, &font, &found, err);
        }
        pc_str_fini(&ps);
        if (st == PC_OK && found) {
            st = psd_set_int(style, "0", font, err);
        }
        if (st == PC_OK) {
            st = psd_set_real(style, "1", (double)(float)(s->size_pt * k), err);
        }
        if (st == PC_OK) {
            st = psd_set_bool(style, "2", s->faux_bold, err);
        }
        if (st == PC_OK) {
            st = psd_set_bool(style, "3", s->faux_italic, err);
        }
        if (st == PC_OK) {
            st = psd_set_bool(style, "4", !s->has_leading, err);
        }
        if (st == PC_OK) {
            st = psd_set_real(style, "5", (double)((s->has_leading ? s->leading_pt : 0.0f) * k), err);
        }
        if (st == PC_OK) {
            st = psd_set_real(style, "6", (double)s->horizontal_scale, err);
        }
        if (st == PC_OK) {
            st = psd_set_real(style, "7", (double)s->vertical_scale, err);
        }
        if (st == PC_OK) {
            st = psd_set_int(style, "8", pc_f32_to_i64(roundf(s->tracking)), err);
        }
        paint = pc_ed_get_mut(pc_ed_get_mut(style, "53"), "0");
        if (st == PC_OK && paint != NULL && paint->kind == PC_ED_DICT) {
            pc_ed_value values;
            st = txt2_fill(&s->color, &kind, &values, err);
            if (st == PC_OK) {
                st = psd_set_int(paint, "0", kind, err);
            }
            if (st == PC_OK) {
                st = edset(paint, "1", &values, err);
            } else {
                pc_ed_fini(&values);
            }
        }
        for (fi = 0; fi < PC_TXT2_FEATURE_COUNT && st == PC_OK; fi++) {
            bool present;
            bool on = txt2_feature_on(s, PC_TXT2_FEATURES[fi][1], &present);
            bval = present ? on : (strcmp(PC_TXT2_FEATURES[fi][1], "calt") == 0);
            st = psd_set_bool(style, PC_TXT2_FEATURES[fi][0], bval, err);
        }
        for (fi = 0; fi < 20; fi++) {
            char tag[8];
            bool present;
            (void)snprintf(tag, sizeof tag, "ss%02u", (unsigned)(fi + 1));
            if (txt2_feature_on(s, tag, &present) && present) {
                sets |= (int64_t)1 << fi;
            }
        }
        if (st == PC_OK) {
            st = psd_set_int(style, PC_TXT2_STYLISTIC_SETS, sets, err);
        }
        if (st == PC_OK) {
            st = psd_set_int(style, "11", txt2_mode(s), err);
        }
        if (st == PC_OK) {
            st = psd_set_int(&run, "1", (int64_t)spans[i].n, err);
        }
        if (st == PC_OK) {
            st = edpush(&arr, &run, err);
        } else {
            pc_ed_fini(&run);
        }
    }
done:
    pc_char_style_fini(&defstyle);
    pc_text_runs_free(runs, nruns);
    pc_str_fini(&edtext);
    free(spans);
    if (st != PC_OK) {
        pc_ed_fini(&arr);
        return st;
    }
    *out = arr;
    *ok = true;
    return PC_OK;
}

/* One text object for `layer` at slot `at`: kept (same text) keeps everything but its regenerated
 * text and style runs; an edited object keeps the file's structure refitted; a new object takes
 * the template's shape; otherwise a bare generated object. */
static pc_status build_slot(const pc_text_layer *layer, const pc_ed_value *slot_raw, size_t at,
                            const pc_ed_value *template_, bool all_stubs, pc_ed_value *fonts, float dpi,
                            pc_ed_value *out, pc_error *err)
{
    pc_str text = PC_STR_INIT;
    mode_run *modes = NULL;
    size_t nmodes = 0;
    const pc_ed_value *slot = slot_raw, *kept = NULL, *pm;
    pc_ed_value object, model, runs;
    bool have_object = false, have_edited = false, have_fresh = false, have_runs = false, unchanged, reuse;
    pc_status st = engine_text(layer, &text, err);
    if (st == PC_OK) {
        st = txt2_mode_runs(layer, &modes, &nmodes, err);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    /* A stub an earlier save wrote beside Photoshop's objects counts as no object (replaced like a
     * missing one) unless every object is a stub. */
    if (slot != NULL && txt2_is_stub(slot) && !all_stubs) {
        slot = NULL;
    }
    if (slot != NULL) {
        const pc_str *t = pc_ed_as_str(pc_ed_path(slot, "0", "0", (const char *)NULL));
        if (t != NULL && t->len == text.len && memcmp(pc_str_c(t), pc_str_c(&text), t->len) == 0) {
            kept = slot;
        }
    }
    if (kept != NULL) {
        st = pc_ed_clone(kept, &object, err);
        have_object = st == PC_OK;
    } else {
        pc_ed_value edited_v, fresh_v;
        if (slot != NULL) {
            st = edited_object(slot, pc_str_c(&text), text.len, &edited_v, &have_edited, err);
        }
        if (st == PC_OK && !have_edited && template_ != NULL) {
            st = new_object(template_, at, pc_str_c(&text), text.len, &fresh_v, &have_fresh, err);
        }
        if (st == PC_OK) {
            if (have_edited) {
                object = edited_v;
            } else if (have_fresh) {
                object = fresh_v;
            } else {
                object = pc_ed_dict();
            }
            have_object = true;
        } else {
            if (have_edited) {
                pc_ed_fini(&edited_v);
            }
            if (have_fresh) {
                pc_ed_fini(&fresh_v);
            }
        }
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    unchanged = kept != NULL;
    reuse = kept != NULL || have_edited;
    pm = pc_ed_get(&object, "0");
    if (pm != NULL && pm->kind == PC_ED_DICT) {
        st = pc_ed_clone(pm, &model, err);
    } else {
        model = pc_ed_dict();
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    /* Styled runs: a new or edited object takes the layer's own per-run style, built on one of the
     * object's own style runs. */
    if (!unchanged && !txt2_is_stub(&object)) {
        size_t nb = 0;
        const pc_ed_value *arr = pc_ed_as_array(pc_ed_path(&model, "6", "0", (const char *)NULL), &nb);
        if (arr != NULL && nb > 0) {
            st = txt2_style_runs(layer, &arr[0], fonts, dpi, &runs, &have_runs, err);
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&model);
        goto cleanup;
    }
    /* The file's own style runs with the auto-kern modes set. */
    if (!have_runs && reuse) {
        size_t nb = 0;
        const pc_ed_value *arr = pc_ed_as_array(pc_ed_path(&model, "6", "0", (const char *)NULL), &nb);
        if (arr != NULL) {
            st = with_modes(arr, nb, modes, nmodes, &runs, &have_runs, err);
        }
    }
    if (st != PC_OK) {
        pc_ed_fini(&model);
        goto cleanup;
    }
    if (!have_runs) {
        size_t m;
        runs = pc_ed_array();
        for (m = 0; st == PC_OK && m < nmodes; m++) {
            pc_ed_value v;
            st = generated_run(modes[m].mode, modes[m].len, &v, err);
            if (st == PC_OK) {
                st = edpush(&runs, &v, err);
            }
        }
        have_runs = true;
    }
    if (st == PC_OK) {
        pc_ed_value sv;
        st = pc_ed_string(pc_str_c(&text), text.len, &sv, err);
        if (st == PC_OK) {
            st = edset(&model, "0", &sv, err);
        }
    }
    if (st == PC_OK) {
        pc_ed_value *sr = pc_ed_get_mut(&model, "6");
        if (sr != NULL && sr->kind == PC_ED_DICT) {
            st = edset(sr, "0", &runs, err);
        } else {
            pc_ed_value d = pc_ed_dict();
            st = pc_ed_set(&d, "0", &runs, err);
            pc_ed_fini(&runs);
            if (st == PC_OK) {
                st = edset(&model, "6", &d, err);
            } else {
                pc_ed_fini(&d);
            }
        }
        have_runs = false;
    } else {
        pc_ed_fini(&runs);
    }
    if (st == PC_OK) {
        st = edset(&object, "0", &model, err);
    } else {
        pc_ed_fini(&model);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    *out = object;
    have_object = false;
cleanup:
    if (have_object) {
        pc_ed_fini(&object);
    }
    pc_str_fini(&text);
    free(modes);
    return st;
}

pc_status pc_txt2_build(const pc_txt2_object *objects, size_t n, const uint8_t *previous, size_t prev_len,
                        bool has_previous, pc_buf *out, pc_error *err)
{
    pc_ed_value prev, outer, slots = pc_ed_array(), fonts = pc_ed_array(), td = pc_ed_array();
    bool has_prev = false, have_td = false, td_changed = false, all_stubs = true;
    const pc_ed_value *prev_objects = NULL, *template_ = NULL;
    size_t nprev = 0, fonts_before = 0, i;
    const float dpi = 72.0f;
    bool have_outer = false;
    pc_error tmp;
    pc_status st = PC_OK;
    if (has_previous) {
        st = pc_txt2_parse(previous, prev_len, &prev, &tmp);
        if (st == PC_ERR_NOMEM) {
            pc_ed_fini(&slots);
            pc_ed_fini(&fonts);
            pc_ed_fini(&td);
            return forward(err, &tmp, st);
        }
        has_prev = st == PC_OK;
        st = PC_OK;
    }
    if (has_prev) {
        size_t ntd = 0, nf = 0;
        const pc_ed_value *tdp, *fp;
        prev_objects = pc_ed_as_array(pc_ed_path(&prev, "1", "1", (const char *)NULL), &nprev);
        if (prev_objects == NULL) {
            nprev = 0;
        }
        for (i = 0; i < nprev && st == PC_OK; i++) {
            pc_ed_value c;
            st = pc_ed_clone(&prev_objects[i], &c, err);
            if (st == PC_OK) {
                st = edpush(&slots, &c, err);
            } else {
                pc_ed_fini(&c);
            }
        }
        tdp = pc_ed_as_array(pc_ed_path(&prev, "DocumentObjects", "TextObjects", (const char *)NULL), &ntd);
        if (tdp != NULL) {
            have_td = true;
            for (i = 0; i < ntd && st == PC_OK; i++) {
                pc_ed_value c;
                st = pc_ed_clone(&tdp[i], &c, err);
                if (st == PC_OK) {
                    st = edpush(&td, &c, err);
                } else {
                    pc_ed_fini(&c);
                }
            }
        }
        fp = pc_ed_as_array(pc_ed_path(&prev, "0", "1", "0", (const char *)NULL), &nf);
        for (i = 0; i < nf && st == PC_OK; i++) {
            pc_ed_value c;
            st = pc_ed_clone(&fp[i], &c, err);
            if (st == PC_OK) {
                st = edpush(&fonts, &c, err);
            } else {
                pc_ed_fini(&c);
            }
        }
        fonts_before = fonts.n;
        for (i = 0; i < nprev; i++) {
            if (!txt2_is_stub(&prev_objects[i])) {
                all_stubs = false;
            }
        }
        /* A template is copied once per new layer, so a large one is skipped. */
        for (i = nprev; st == PC_OK && i-- > 0;) {
            const pc_ed_value *o = &prev_objects[i];
            size_t budget = TXT2_TEMPLATE_NODES;
            pc_ed_value tmpobj;
            bool feasible = false;
            if (!txt2_names_one_frame(o) || !txt2_fits(o, &budget)) {
                continue;
            }
            st = edited_object(o, "\r", 1, &tmpobj, &feasible, err);
            if (st == PC_OK && feasible) {
                pc_ed_fini(&tmpobj);
                template_ = o;
                break;
            }
        }
    }
    for (i = 0; i < n && st == PC_OK; i++) {
        int32_t index = objects[i].index;
        size_t at;
        const pc_ed_value *slot_raw;
        pc_ed_value object;
        pc_str etext = PC_STR_INIT;
        if (index < 0 || index > PC_TXT2_MAX_TEXT_INDEX || objects[i].layer == NULL) {
            continue;
        }
        at = (size_t)index;
        /* The older-Photoshop copy of this object, refitted when its text is stale (#6971). */
        st = engine_text(objects[i].layer, &etext, err);
        if (st == PC_OK && have_td && at < td.n) {
            const pc_str *mt = pc_ed_as_str(pc_ed_path(&td.items[at], "Model", "Text", (const char *)NULL));
            bool same = mt != NULL && mt->len == etext.len && memcmp(pc_str_c(mt), pc_str_c(&etext), mt->len) == 0;
            if (!same) {
                pc_ed_value fixed;
                bool fok = false;
                st = td_edited_object(&td.items[at], pc_str_c(&etext), etext.len, &fixed, &fok, err);
                if (st == PC_OK && fok) {
                    pc_ed_fini(&td.items[at]);
                    td.items[at] = fixed;
                    td_changed = true;
                }
            }
        }
        pc_str_fini(&etext);
        if (st != PC_OK) {
            break;
        }
        slot_raw = at < nprev ? &prev_objects[at] : NULL;
        st = build_slot(objects[i].layer, slot_raw, at, template_, all_stubs, &fonts, dpi, &object, err);
        if (st != PC_OK) {
            break;
        }
        while (st == PC_OK && slots.n <= at) {
            pc_ed_value d = pc_ed_dict();
            st = edpush(&slots, &d, err);
        }
        if (st == PC_OK) {
            pc_ed_fini(&slots.items[at]);
            slots.items[at] = object;
        } else {
            pc_ed_fini(&object);
        }
    }
    /* The outer dictionary keeps whatever the block held (`/98` version, block extras, ...). */
    if (st == PC_OK) {
        if (has_prev) {
            st = pc_ed_clone(&prev, &outer, err);
        } else {
            pc_ed_value ver = pc_ed_dict(), v = pc_ed_int(14), z = pc_ed_dict();
            outer = pc_ed_dict();
            st = pc_ed_set(&ver, "0", &v, err);
            if (st == PC_OK) {
                st = edset(&outer, "98", &ver, err);
            } else {
                pc_ed_fini(&ver);
            }
            if (st == PC_OK) {
                st = edset(&outer, "0", &z, err);
            } else {
                pc_ed_fini(&z);
            }
            if (st != PC_OK) {
                pc_ed_fini(&outer);
            }
        }
        have_outer = st == PC_OK;
    }
    if (st == PC_OK) {
        st = pad_frames(&outer, slots.n, err);
    }
    if (st == PC_OK && fonts.n != fonts_before) {
        pc_ed_value *list = pc_ed_get_mut(pc_ed_get_mut(&outer, "0"), "1");
        if (list != NULL && list->kind == PC_ED_DICT) {
            st = pc_ed_set(list, "0", &fonts, err);
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
                st = edset(&outer, "1", &d, err);
            } else {
                pc_ed_fini(&d);
            }
        }
    }
    if (st == PC_OK && td_changed && have_td) {
        pc_ed_value *dobj = pc_ed_get_mut(&outer, "DocumentObjects");
        if (dobj != NULL && dobj->kind == PC_ED_DICT) {
            st = pc_ed_set(dobj, "TextObjects", &td, err);
        }
    }
    if (st == PC_OK) {
        if (has_prev && pc_ed_eq(&prev, &outer)) {
            st = pc_buf_append(out, previous, prev_len, err);
        } else {
            st = pc_ed_write_bare(&outer, out, err);
        }
    }
    if (have_outer) {
        pc_ed_fini(&outer);
    }
    pc_ed_fini(&slots);
    pc_ed_fini(&fonts);
    pc_ed_fini(&td);
    if (has_prev) {
        pc_ed_fini(&prev);
    }
    return st;
}
