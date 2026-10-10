/* Artistic and frame text (port of crates/affinity/src/text.rs): the story's characters with their
 * font, size, colour, tracking and leading, the paragraph alignment and the first baseline. Layout
 * is left to the importing application. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/buf.h"
#include "base/mem.h"
#include "base/utf8.h"

/* Characters read per text node. */
#define MAX_CHARS ((size_t)1 << 20)
#define FFFC 0xFFFCu

typedef struct cps {
    uint32_t *v;
    size_t n;
    size_t cap;
} cps;

static bool cps_push(cps *c, uint32_t cp)
{
    if (c->n == c->cap) {
        size_t cap = c->cap > 0 ? c->cap * 2 : 64;
        uint32_t *v = realloc(c->v, cap * sizeof *v);
        if (v == NULL) {
            return false;
        }
        c->v = v;
        c->cap = cap;
    }
    c->v[c->n++] = cp;
    return true;
}

static bool cps_utf8(cps *c, const char *s)
{
    size_t i = 0;
    size_t n = s != NULL ? strlen(s) : 0;
    uint32_t cp;
    while (i < n && pc_utf8_next((const uint8_t *)s, n, &i, &cp)) {
        if (!cps_push(c, cp)) {
            return false;
        }
    }
    return true;
}

static const pc_af_value *slot(const pc_af_value *v, size_t i)
{
    return v != NULL && v->kind == PC_AF_ARRAY && i < v->as.arr.n ? &v->as.arr.v[i] : NULL;
}

static bool sfloat(const pc_af_value *v, size_t i, double *out)
{
    const pc_af_value *e = slot(v, i);
    if (e == NULL || e->kind != PC_AF_FLOAT || !isfinite(e->as.f)) {
        return false;
    }
    *out = e->as.f;
    return true;
}

static bool sint(const pc_af_value *v, size_t i, int64_t *out)
{
    const pc_af_value *e = slot(v, i);
    if (e == NULL) {
        return false;
    }
    if (e->kind == PC_AF_INT) {
        *out = e->as.i;
        return true;
    }
    if (e->kind == PC_AF_UINT && e->as.u <= (uint64_t)INT64_MAX) {
        *out = (int64_t)e->as.u;
        return true;
    }
    return false;
}

static char *dup_or_empty(const char *s) { return pc_strndup(s != NULL ? s : "", s != NULL ? strlen(s) : 0); }

static pc_status run_attrs(ax_reader *r, size_t attrs, char *text, pc_af_affine world, pc_af_text_run *out,
                           pc_error *err)
{
    const pc_af_stream *s = r->s;
    const pc_af_value *doubles = ax_field(s, attrs, "Doub");
    const pc_af_value *ints = ax_field(s, attrs, "Ints");
    const pc_af_value *objs = ax_field(s, attrs, "Objs");
    pc_af_text_run run;
    double v;
    int64_t iv;
    size_t font;
    bool has_font;
    pc_status st;
    memset(&run, 0, sizeof run);
    run.text = text;
    run.size = sfloat(doubles, 0, &v) && v > 0.0 && v < 1e6 ? v : 12.0 * r->dpi / 72.0;
    run.tracking = sfloat(doubles, 4, &v) ? v : 0.0;
    if (sint(ints, 4, &iv) && iv == 1 && sfloat(doubles, 13, &v) && v > 0.0) {
        run.has_leading = true;
        run.leading = v;
    }
    if (objs != NULL && objs->kind != PC_AF_ARRAY) {
        objs = NULL;
    }
    run.fill.kind = PC_AF_PAINT_NONE;
    if (objs != NULL && objs->as.arr.n > 0 && objs->as.arr.v[0].kind == PC_AF_OBJ && objs->as.arr.v[0].as.obj.some) {
        bool ok;
        pc_af_paint p;
        if ((st = ax_descriptor(r, objs->as.arr.v[0].as.obj.id, world, &ok, &p, err)) != PC_OK) {
            free(text);
            return st;
        }
        if (ok) {
            run.fill = p;
        }
    }
    if (objs != NULL && objs->as.arr.n > 1 && objs->as.arr.v[1].kind == PC_AF_OBJ && objs->as.arr.v[1].as.obj.some) {
        bool ok;
        pc_af_paint p;
        if ((st = ax_descriptor(r, objs->as.arr.v[1].as.obj.id, world, &ok, &p, err)) != PC_OK) {
            ax_paint_fini(&run.fill);
            free(text);
            return st;
        }
        if (ok && p.kind != PC_AF_PAINT_NONE) {
            ax_warn(r, "outlined (stroked) text");
        }
        if (ok) {
            ax_paint_fini(&p);
        }
    }
    if (sfloat(doubles, 10, &v) && fabs(v - 1.0) > 1e-3) {
        ax_warn(r, "horizontally scaled text");
    }
    has_font = ax_obj(s, attrs, "DFnt", &font);
    run.postscript = dup_or_empty(has_font ? ax_str(s, font, "Post") : NULL);
    run.family = dup_or_empty(has_font ? ax_str(s, font, "Famy") : NULL);
    run.weight = has_font && ax_int(s, font, "Wegt", &iv) ? iv : 400;
    {
        bool it;
        run.italic = has_font && ax_bool(s, font, "Ital", &it) && it;
    }
    if (run.postscript == NULL || run.family == NULL) {
        free(run.postscript);
        free(run.family);
        ax_paint_fini(&run.fill);
        free(text);
        return ax_nomem(err);
    }
    *out = run;
    return PC_OK;
}

static pc_af_align paragraph_align(ax_reader *r, size_t p)
{
    int64_t v;
    if (!sint(ax_field(r->s, p, "Ints"), 0, &v) || v == 0) {
        return PC_AF_LEFT;
    }
    if (v == 1) {
        return PC_AF_ACENTER;
    }
    if (v == 2) {
        return PC_AF_RIGHT;
    }
    if (v == 3) {
        return PC_AF_JUSTIFY;
    }
    ax_warn(r, "an unknown paragraph alignment (imported as left)");
    return PC_AF_LEFT;
}

pc_status ax_text_read(ax_reader *r, size_t id, pc_af_affine world, bool *ok, pc_af_text *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t story;
    size_t frame;
    bool artistic;
    double bv[4];
    pc_af_rect bounds;
    pc_af_text t;
    size_t cap = 0;
    bool has_align = false;
    pc_af_align align = PC_AF_LEFT;
    size_t total = 0;
    bool placeholders = false;
    const pc_af_value *blocks;
    size_t bi = 0;
    size_t block;
    double artv;
    pc_af_tag cls;
    pc_status st = PC_OK;
    *ok = false;
    memset(&t, 0, sizeof t);
    if (!ax_obj(s, id, "StSt", &story) || !ax_obj(s, id, "TxtH", &frame)) {
        return PC_OK;
    }
    artistic = ax_is(s, frame, "ArFr");
    if (!ax_floats(s, frame, "FrmB", 4, bv)) {
        return PC_OK;
    }
    bounds = ax_rect(bv);
    blocks = ax_objs(s, story, "Blok");
    while (ax_next_obj(blocks, &bi, &block)) {
        size_t glyphs;
        cps chars = {NULL, 0, 0};
        const char *utf8;
        size_t start = 0;
        size_t g;
        size_t ri = 0;
        size_t run;
        const pc_af_value *runs = NULL;
        if (!ax_obj(s, block, "Glyp", &glyphs)) {
            goto none;
        }
        utf8 = ax_str(s, glyphs, "Utf8");
        if (utf8 != NULL) {
            if (!cps_utf8(&chars, utf8)) {
                goto oom_chars;
            }
        } else {
            /* Inline objects (page numbers, anchors) each take one index before the segment text. */
            const pc_af_value *mixd = ax_objs(s, glyphs, "Mixd");
            size_t mi = 0;
            size_t seg;
            while (ax_next_obj(mixd, &mi, &seg)) {
                size_t inl = ax_objs_count(ax_objs(s, seg, "Glys"));
                size_t k;
                for (k = 0; k < inl; k++) {
                    if (!cps_push(&chars, FFFC)) {
                        goto oom_chars;
                    }
                }
                placeholders = placeholders || inl > 0;
                if (!cps_utf8(&chars, ax_str(s, seg, "Utf8"))) {
                    goto oom_chars;
                }
            }
        }
        if (total > SIZE_MAX - chars.n) {
            free(chars.v);
            goto none;
        }
        total += chars.n;
        if (total > MAX_CHARS) {
            ax_warn(r, "text longer than a million characters (truncated)");
            free(chars.v);
            break;
        }
        if (ax_obj(s, block, "GAtt", &g)) {
            runs = ax_objs(s, g, "Runs");
        }
        while (ax_next_obj(runs, &ri, &run)) {
            int64_t e64;
            size_t end;
            size_t attrs;
            pc_buf text = PC_BUF_INIT;
            size_t k;
            if (!ax_int(s, run, "Indx", &e64) || e64 < 0) {
                free(chars.v);
                goto none;
            }
            end = (uint64_t)e64 < chars.n ? (size_t)e64 : chars.n;
            if (end <= start) {
                continue;
            }
            for (k = start; k < end; k++) {
                if (chars.v[k] != 0 && chars.v[k] != FFFC && pc_utf8_put(&text, chars.v[k], NULL) != PC_OK) {
                    pc_buf_fini(&text);
                    goto oom_chars;
                }
            }
            start = end;
            if (!ax_obj(s, run, "Item", &attrs)) {
                pc_buf_fini(&text);
                continue;
            }
            if (text.len > 0) {
                char *str;
                pc_af_text_run tr;
                if (pc_buf_append_byte(&text, 0, NULL) != PC_OK) {
                    pc_buf_fini(&text);
                    goto oom_chars;
                }
                str = (char *)pc_buf_take(&text, NULL);
                if (t.nruns == cap) {
                    size_t c = cap > 0 ? cap * 2 : 4;
                    pc_af_text_run *rv = realloc(t.runs, c * sizeof *rv);
                    if (rv == NULL) {
                        free(str);
                        goto oom_chars;
                    }
                    t.runs = rv;
                    cap = c;
                }
                if ((st = run_attrs(r, attrs, str, world, &tr, err)) != PC_OK) {
                    free(chars.v);
                    goto fail;
                }
                t.runs[t.nruns++] = tr;
            } else {
                pc_buf_fini(&text);
            }
        }
        free(chars.v);
        if (!has_align) {
            size_t pa;
            const pc_af_value *pruns;
            size_t pi = 0;
            size_t first;
            size_t item;
            if (ax_obj(s, block, "PAtt", &pa) && (pruns = ax_objs(s, pa, "Runs")) != NULL &&
                ax_next_obj(pruns, &pi, &first) && ax_obj(s, first, "Item", &item)) {
                align = paragraph_align(r, item);
                has_align = true;
            }
        }
        continue;
    oom_chars:
        free(chars.v);
        st = ax_nomem(err);
        goto fail;
    }
    if (placeholders) {
        ax_warn(r, "text fields such as page numbers (left out of the text)");
    }
    t.align = has_align ? align : PC_AF_LEFT;
    t.anchor.y = bounds.y0 + (ax_f64(s, frame, "ArtV", &artv) ? artv : 0.0);
    t.anchor.x = t.align == PC_AF_ACENTER ? (bounds.x0 + bounds.x1) / 2.0
                 : t.align == PC_AF_RIGHT ? bounds.x1
                                           : bounds.x0;
    if (!artistic) {
        const pc_af_value *cw = ax_field(s, frame, "ColW");
        if (ax_objs_count(ax_objs(s, frame, "ColW")) > 1 || (cw != NULL && cw->kind == PC_AF_ARRAY && cw->as.arr.n > 1)) {
            ax_warn(r, "text frames with several columns (imported as one column)");
        }
        if (ax_class(s, id, &cls) && cls == PC_AF_TAG("TxtC")) {
            ax_warn(r, "text frames with a curved outline (imported as rectangular frames)");
        }
    }
    t.has_frame = !artistic;
    t.frame = bounds;
    t.transform = world;
    *out = t;
    *ok = true;
    return PC_OK;
none:
    ax_text_fini(&t);
    return PC_OK;
fail:
    ax_text_fini(&t);
    return st;
}
