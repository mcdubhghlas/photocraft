/* Port of crates/doc/src/text.rs and lib.rs TextLayer: the typed style model of type layers. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "doc/doc.h"
#include "doc/tree.h"

/* ---- character style ---------------------------------------------------------------------- */

void pc_char_style_default(pc_char_style *out)
{
    memset(out, 0, sizeof *out);
    out->weight = 400;
    out->size_pt = 12.0f;
    out->color = PC_COLOR_BLACK;
    out->horizontal_scale = 1.0f;
    out->vertical_scale = 1.0f;
    out->kerning = PC_KERNING_METRICS;
    out->caps = PC_CAPS_NORMAL;
    out->ligatures = true;
}

static void feature_fini(pc_font_feature *f)
{
    pc_str_fini(&f->tag);
}

static pc_status feature_clone(const pc_font_feature *f, pc_font_feature *out, pc_error *err)
{
    pc_font_feature c = *f;
    pc_status s = pc_str_clone(&f->tag, &c.tag, err);
    if (s != PC_OK) {
        return s;
    }
    *out = c;
    return PC_OK;
}

static bool feature_eq(const pc_font_feature *a, const pc_font_feature *b)
{
    return pc_str_eq(&a->tag, &b->tag) && a->value == b->value;
}

static void variation_fini(pc_font_variation *v)
{
    pc_str_fini(&v->axis);
}

static pc_status variation_clone(const pc_font_variation *v, pc_font_variation *out, pc_error *err)
{
    pc_font_variation c = *v;
    pc_status s = pc_str_clone(&v->axis, &c.axis, err);
    if (s != PC_OK) {
        return s;
    }
    *out = c;
    return PC_OK;
}

static bool variation_eq(const pc_font_variation *a, const pc_font_variation *b)
{
    return pc_str_eq(&a->axis, &b->axis) && a->value == b->value;
}

void pc_char_style_fini(pc_char_style *s)
{
    if (s == NULL) {
        return;
    }
    pc_str_fini(&s->font_family);
    pc_str_fini(&s->font_style);
    pc_str_fini(&s->postscript_name);
    PC_DOC_FINI_VEC(s->features, s->nfeatures, feature_fini);
    PC_DOC_FINI_VEC(s->variations, s->nvariations, variation_fini);
    pc_str_fini(&s->language);
    memset(s, 0, sizeof *s);
}

pc_status pc_char_style_clone(const pc_char_style *s, pc_char_style *out, pc_error *err)
{
    pc_char_style c = *s;
    pc_status st;
    /* Owned fields start empty so the failure path can finalise c. */
    memset(&c.font_family, 0, sizeof c.font_family);
    memset(&c.font_style, 0, sizeof c.font_style);
    memset(&c.postscript_name, 0, sizeof c.postscript_name);
    memset(&c.language, 0, sizeof c.language);
    c.features = NULL;
    c.nfeatures = 0;
    c.variations = NULL;
    c.nvariations = 0;
    st = pc_str_clone(&s->font_family, &c.font_family, err);
    if (st == PC_OK) {
        st = pc_str_clone(&s->font_style, &c.font_style, err);
    }
    if (st == PC_OK) {
        st = pc_str_clone(&s->postscript_name, &c.postscript_name, err);
    }
    if (st == PC_OK) {
        st = pc_str_clone(&s->language, &c.language, err);
    }
    if (st == PC_OK) {
        PC_DOC_CLONE_VEC(st, s->features, s->nfeatures, c.features, feature_clone, feature_fini, err);
        if (st == PC_OK) {
            c.nfeatures = s->nfeatures;
        }
    }
    if (st == PC_OK) {
        PC_DOC_CLONE_VEC(st, s->variations, s->nvariations, c.variations, variation_clone, variation_fini,
                         err);
        if (st == PC_OK) {
            c.nvariations = s->nvariations;
        }
    }
    if (st != PC_OK) {
        pc_char_style_fini(&c);
        return st;
    }
    *out = c;
    return PC_OK;
}

bool pc_char_style_eq(const pc_char_style *a, const pc_char_style *b)
{
    bool f;
    bool v;
    if (!pc_str_eq(&a->font_family, &b->font_family) || !pc_str_eq(&a->font_style, &b->font_style) ||
        a->has_postscript_name != b->has_postscript_name ||
        (a->has_postscript_name && !pc_str_eq(&a->postscript_name, &b->postscript_name)) ||
        a->weight != b->weight || a->italic != b->italic || !(a->size_pt == b->size_pt) ||
        !pc_doc_color_eq(&a->color, &b->color) || !(a->tracking == b->tracking) ||
        a->has_leading != b->has_leading || (a->has_leading && !(a->leading_pt == b->leading_pt)) ||
        !(a->baseline_shift_pt == b->baseline_shift_pt) || !(a->horizontal_scale == b->horizontal_scale) ||
        !(a->vertical_scale == b->vertical_scale) || a->underline != b->underline ||
        a->strikethrough != b->strikethrough || a->faux_bold != b->faux_bold ||
        a->faux_italic != b->faux_italic || a->kerning != b->kerning || !(a->kern == b->kern) ||
        a->caps != b->caps || a->ligatures != b->ligatures ||
        a->discretionary_ligatures != b->discretionary_ligatures || a->has_language != b->has_language ||
        (a->has_language && !pc_str_eq(&a->language, &b->language)) ||
        a->has_style_sheet != b->has_style_sheet ||
        (a->has_style_sheet && a->style_sheet != b->style_sheet)) {
        return false;
    }
    PC_DOC_EQ_VEC(f, a->features, a->nfeatures, b->features, b->nfeatures, feature_eq);
    PC_DOC_EQ_VEC(v, a->variations, a->nvariations, b->variations, b->nvariations, variation_eq);
    return f && v;
}

/* ---- paragraph style ---------------------------------------------------------------------- */

bool pc_text_align_is_justified(pc_text_align a)
{
    return a == PC_TEXT_ALIGN_JUSTIFY_LEFT || a == PC_TEXT_ALIGN_JUSTIFY_CENTER ||
           a == PC_TEXT_ALIGN_JUSTIFY_RIGHT || a == PC_TEXT_ALIGN_JUSTIFY_ALL;
}

pc_paragraph_style pc_paragraph_style_default(void)
{
    pc_paragraph_style p;
    memset(&p, 0, sizeof p);
    p.align = PC_TEXT_ALIGN_LEFT;
    p.auto_leading = 1.2f;
    p.direction = PC_TEXT_DIR_AUTO;
    return p;
}

bool pc_paragraph_style_eq(const pc_paragraph_style *a, const pc_paragraph_style *b)
{
    return a->align == b->align && a->first_line_indent_pt == b->first_line_indent_pt &&
           a->start_indent_pt == b->start_indent_pt && a->end_indent_pt == b->end_indent_pt &&
           a->space_before_pt == b->space_before_pt && a->space_after_pt == b->space_after_pt &&
           a->auto_leading == b->auto_leading && a->direction == b->direction &&
           a->hyphenate == b->hyphenate && a->has_style_sheet == b->has_style_sheet &&
           (!a->has_style_sheet || a->style_sheet == b->style_sheet);
}

/* ---- warp --------------------------------------------------------------------------------- */

void pc_text_warp_fini(pc_text_warp *w)
{
    if (w == NULL) {
        return;
    }
    pc_str_fini(&w->style);
    memset(w, 0, sizeof *w);
}

pc_status pc_text_warp_clone(const pc_text_warp *w, pc_text_warp *out, pc_error *err)
{
    pc_text_warp c = *w;
    pc_status s = pc_str_clone(&w->style, &c.style, err);
    if (s != PC_OK) {
        return s;
    }
    *out = c;
    return PC_OK;
}

bool pc_text_warp_eq(const pc_text_warp *a, const pc_text_warp *b)
{
    return pc_str_eq(&a->style, &b->style) && a->value == b->value &&
           a->horizontal_distortion == b->horizontal_distortion &&
           a->vertical_distortion == b->vertical_distortion && a->horizontal == b->horizontal;
}

/* ---- text layer --------------------------------------------------------------------------- */

void pc_text_layer_default(pc_text_layer *out)
{
    memset(out, 0, sizeof *out);
    out->size_pt = 12.0f;
    out->color = PC_COLOR_BLACK;
    out->transform = PC_AFFINE_IDENTITY;
    out->orientation = PC_ORIENT_HORIZONTAL;
    out->antialias = PC_AA_SMOOTH;
}

static void text_run_fini(pc_text_run *r)
{
    pc_char_style_fini(&r->style);
}

static pc_status text_run_clone(const pc_text_run *r, pc_text_run *out, pc_error *err)
{
    pc_text_run c;
    pc_status s = pc_char_style_clone(&r->style, &c.style, err);
    if (s != PC_OK) {
        return s;
    }
    c.len = r->len;
    *out = c;
    return PC_OK;
}

static bool text_run_eq(const pc_text_run *a, const pc_text_run *b)
{
    return a->len == b->len && pc_char_style_eq(&a->style, &b->style);
}

static bool paragraph_run_eq(const pc_paragraph_run *a, const pc_paragraph_run *b)
{
    return a->len == b->len && pc_paragraph_style_eq(&a->style, &b->style);
}

void pc_text_runs_free(pc_text_run *runs, size_t n)
{
    PC_DOC_FINI_VEC(runs, n, text_run_fini);
}

void pc_text_layer_fini(pc_text_layer *t)
{
    if (t == NULL) {
        return;
    }
    pc_str_fini(&t->text);
    pc_str_fini(&t->font_family);
    if (t->has_cache) {
        pc_surface_fini(&t->cache);
    }
    pc_blob_release(t->psd_raw);
    PC_DOC_FINI_VEC(t->runs, t->nruns, text_run_fini);
    free(t->paragraphs);
    if (t->has_warp) {
        pc_text_warp_fini(&t->warp);
    }
    memset(t, 0, sizeof *t);
}

pc_status pc_text_layer_clone(const pc_text_layer *t, pc_text_layer *out, pc_error *err)
{
    pc_text_layer c;
    pc_status s;
    memset(&c, 0, sizeof c);
    c.size_pt = t->size_pt;
    c.color = t->color;
    c.transform = t->transform;
    c.shape = t->shape;
    c.orientation = t->orientation;
    c.antialias = t->antialias;
    s = pc_str_clone(&t->text, &c.text, err);
    if (s == PC_OK) {
        s = pc_str_clone(&t->font_family, &c.font_family, err);
    }
    if (s == PC_OK && t->has_cache) {
        s = pc_surface_clone(&t->cache, &c.cache, err);
        c.has_cache = s == PC_OK;
    }
    if (s == PC_OK) {
        PC_DOC_CLONE_VEC(s, t->runs, t->nruns, c.runs, text_run_clone, text_run_fini, err);
        if (s == PC_OK) {
            c.nruns = t->nruns;
        }
    }
    if (s == PC_OK && t->nparagraphs > 0) {
        c.paragraphs = pc_malloc_array(t->nparagraphs, sizeof *c.paragraphs);
        if (c.paragraphs == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory copying paragraph runs");
        } else {
            memcpy(c.paragraphs, t->paragraphs, t->nparagraphs * sizeof *c.paragraphs);
            c.nparagraphs = t->nparagraphs;
        }
    }
    if (s == PC_OK && t->has_warp) {
        s = pc_text_warp_clone(&t->warp, &c.warp, err);
        c.has_warp = s == PC_OK;
    }
    if (s != PC_OK) {
        pc_text_layer_fini(&c);
        return s;
    }
    c.psd_raw = pc_blob_retain(t->psd_raw);
    *out = c;
    return PC_OK;
}

static bool text_shape_eq(const pc_text_shape *a, const pc_text_shape *b)
{
    if (a->is_box != b->is_box) {
        return false;
    }
    return !a->is_box || (a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height);
}

bool pc_text_layer_eq(const pc_text_layer *a, const pc_text_layer *b)
{
    bool r;
    bool p;
    if (!pc_str_eq(&a->text, &b->text) || !pc_str_eq(&a->font_family, &b->font_family) ||
        !(a->size_pt == b->size_pt) || !pc_doc_color_eq(&a->color, &b->color) ||
        !pc_doc_affine_eq(&a->transform, &b->transform) || a->has_cache != b->has_cache ||
        (a->has_cache && !pc_surface_eq(&a->cache, &b->cache)) || !pc_blob_eq(a->psd_raw, b->psd_raw) ||
        !text_shape_eq(&a->shape, &b->shape) || a->orientation != b->orientation ||
        a->antialias != b->antialias || a->has_warp != b->has_warp ||
        (a->has_warp && !pc_text_warp_eq(&a->warp, &b->warp))) {
        return false;
    }
    PC_DOC_EQ_VEC(r, a->runs, a->nruns, b->runs, b->nruns, text_run_eq);
    PC_DOC_EQ_VEC(p, a->paragraphs, a->nparagraphs, b->paragraphs, b->nparagraphs, paragraph_run_eq);
    return r && p;
}

/* ---- run normalisation -------------------------------------------------------------------- */

/* One output run: `len` bytes styled like input run `src` (NO_SRC: the base style). */
typedef struct norm_seg {
    size_t src;
    size_t len;
} norm_seg;

#define NO_SRC SIZE_MAX

static bool is_char_boundary(const pc_str *s, size_t i)
{
    return i == 0 || i >= s->len || ((unsigned char)s->ptr[i] & 0xC0u) != 0x80u;
}

/* normalize_runs over the run lengths lens[k * stride] (bytes of a run array): stretches or
 * truncates the runs to cover text exactly, snapping ends forward to char boundaries and
 * dropping empty runs; empty text keeps one run with the first run's style. */
static pc_status normalize(const pc_str *text, const void *runs, size_t nruns, size_t stride, size_t len_off,
                           norm_seg **out, size_t *nout, pc_error *err)
{
    size_t total = text->len;
    size_t at = 0;
    size_t n = 0;
    size_t k;
    size_t cap;
    norm_seg *segs;
    if (!pc_size_add(nruns, 1, &cap)) {
        return pc_fail(err, PC_ERR_LIMIT, "too many text runs");
    }
    segs = pc_malloc_array(cap, sizeof *segs);
    if (segs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory normalising text runs");
    }
    if (total == 0) {
        segs[0].src = nruns > 0 ? 0 : NO_SRC;
        segs[0].len = 0;
        *out = segs;
        *nout = 1;
        return PC_OK;
    }
    for (k = 0; k < nruns; k++) {
        size_t len;
        size_t end;
        if (at >= total) {
            break;
        }
        memcpy(&len, (const unsigned char *)runs + k * stride + len_off, sizeof len);
        end = (len > SIZE_MAX - at) ? SIZE_MAX : at + len;
        if (end > total) {
            end = total;
        }
        while (!is_char_boundary(text, end)) {
            end++;
        }
        if (end > at) {
            segs[n].src = k;
            segs[n].len = end - at;
            n++;
            at = end;
        }
    }
    if (at < total) {
        if (n > 0) {
            segs[n - 1].len += total - at;
        } else {
            segs[n].src = NO_SRC;
            segs[n].len = total;
            n++;
        }
    }
    if (n == 0) {
        segs[0].src = NO_SRC;
        segs[0].len = 0;
        n = 1;
    }
    *out = segs;
    *nout = n;
    return PC_OK;
}

pc_status pc_text_layer_char_runs(const pc_text_layer *t, pc_text_run **out, size_t *n, pc_error *err)
{
    norm_seg *segs;
    size_t nseg;
    size_t k;
    pc_text_run *runs;
    pc_status s = normalize(&t->text, t->runs, t->runs != NULL ? t->nruns : 0, sizeof *t->runs,
                            offsetof(pc_text_run, len), &segs, &nseg, err);
    if (s != PC_OK) {
        return s;
    }
    runs = pc_calloc_array(nseg, sizeof *runs);
    if (runs == NULL) {
        free(segs);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory normalising text runs");
    }
    for (k = 0; k < nseg; k++) {
        runs[k].len = segs[k].len;
        if (segs[k].src == NO_SRC || t->runs == NULL) {
            /* The summary fields over the default style. */
            pc_char_style_default(&runs[k].style);
            s = pc_str_clone(&t->font_family, &runs[k].style.font_family, err);
            runs[k].style.size_pt = t->size_pt;
            runs[k].style.color = t->color;
        } else {
            s = pc_char_style_clone(&t->runs[segs[k].src].style, &runs[k].style, err);
        }
        if (s != PC_OK) {
            pc_text_runs_free(runs, k + 1);
            free(segs);
            return s;
        }
    }
    free(segs);
    *out = runs;
    *n = nseg;
    return PC_OK;
}

/* The final empty paragraph after a trailing line break owns PSD text's synthetic terminal
 * paragraph marker: its formatting has no source bytes, so normalising must not drop it. */
static bool keeps_terminal_paragraph(const pc_text_layer *t)
{
    size_t sum = 0;
    char lastc;
    if (t->text.len == 0 || t->paragraphs == NULL || t->nparagraphs == 0) {
        return false;
    }
    lastc = t->text.ptr[t->text.len - 1];
    if ((lastc != '\r' && lastc != '\n') || t->paragraphs[t->nparagraphs - 1].len > 1) {
        return false;
    }
    for (size_t k = 0; k + 1 < t->nparagraphs; k++) {
        if (!pc_size_add(sum, t->paragraphs[k].len, &sum)) {
            return false;
        }
    }
    return sum == t->text.len;
}

pc_status pc_text_layer_paragraph_runs(const pc_text_layer *t, pc_paragraph_run **out, size_t *n,
                                       pc_error *err)
{
    norm_seg *segs;
    size_t nseg;
    size_t k;
    pc_paragraph_run *runs;
    bool terminal = keeps_terminal_paragraph(t);
    pc_status s = normalize(&t->text, t->paragraphs, t->paragraphs != NULL ? t->nparagraphs : 0,
                            sizeof *t->paragraphs, offsetof(pc_paragraph_run, len), &segs, &nseg, err);
    if (s != PC_OK) {
        return s;
    }
    runs = pc_malloc_array(nseg + 1, sizeof *runs);
    if (runs == NULL) {
        free(segs);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory normalising paragraph runs");
    }
    for (k = 0; k < nseg; k++) {
        runs[k].len = segs[k].len;
        runs[k].style = (segs[k].src == NO_SRC || t->paragraphs == NULL) ? pc_paragraph_style_default()
                                                                         : t->paragraphs[segs[k].src].style;
    }
    free(segs);
    if (terminal) {
        runs[nseg].len = 0;
        runs[nseg].style = t->paragraphs[t->nparagraphs - 1].style;
        nseg++;
    }
    *out = runs;
    *n = nseg;
    return PC_OK;
}

pc_status pc_text_layer_sync_summary(pc_text_layer *t, pc_error *err)
{
    pc_str family;
    pc_status s;
    if (t->nruns == 0 || t->runs == NULL) {
        return PC_OK;
    }
    s = pc_str_clone(&t->runs[0].style.font_family, &family, err);
    if (s != PC_OK) {
        return s;
    }
    pc_str_fini(&t->font_family);
    t->font_family = family;
    t->size_pt = t->runs[0].style.size_pt;
    t->color = t->runs[0].style.color;
    return PC_OK;
}
