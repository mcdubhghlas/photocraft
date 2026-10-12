/* <text> conversion: a port of usvg 0.45.1 parser/text.rs (chunks, spans, character positions,
 * rotations, decorations, baseline shifts, fonts, textPath) and text/mod.rs convert (layout, then
 * flatten). The layout is svg_text_layout.c, the outlines svg_text_flat.c.
 *
 * Bounded: an element with more than SVT_MAX_CHARS characters is dropped; subtree character
 * counts are computed once (the reference recounts per element, quadratic in nesting). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "io/svg_text.h"

#define DEFAULT_FAMILY "Times New Roman"

/* ---------------------------------------------------------------- helpers */

static size_t utf8_count(const char *s)
{
    size_t n = 0;
    for (; *s != '\0'; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/* SvgNode::text for a text node. */
static const char *node_text(const st_node *n)
{
    return n->text != NULL ? n->text : "";
}

/* Pre-order walk of the subtree below root (root excluded). */
static const st_node *next_desc(const st_node *root, const st_node *c)
{
    if (c->first != NULL) {
        return c->first;
    }
    while (c != NULL && c != root && c->next == NULL) {
        c = c->parent;
    }
    if (c == NULL || c == root) {
        return NULL;
    }
    return c->next;
}

static size_t count_chars(const st_node *n)
{
    size_t k = 0;
    const st_node *c;
    for (c = n->first; c != NULL; c = next_desc(n, c)) {
        if (c->kind == ST_TEXT) {
            k += utf8_count(node_text(c));
        }
    }
    return k;
}

/* find_attribute's node, then its value. */
static const char *find_str(const st_node *n, sv_aid aid)
{
    const st_node *p = st_find_attr_node(n, aid);
    return p != NULL ? st_attr(p, aid) : NULL;
}

static bool eq(const char *a, const char *b)
{
    return a != NULL && strcmp(a, b) == 0;
}

/* SvgNode::resolve_length (ancestors-or-self with the attribute; a bad value gives def). */
static float resolve_length(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st, float def)
{
    const st_node *p;
    for (p = n; p != NULL; p = p->parent) {
        const char *v = st_attr(p, aid);
        if (v != NULL) {
            sv_length l;
            if (sv_parse_length(v, &l)) {
                return sv_convert_length(c, l, p, aid, UNITS_USER, st);
            }
            return def;
        }
    }
    return def;
}

static svt_baseline parse_baseline(const char *v, bool dominant)
{
    static const struct {
        const char *s;
        svt_baseline b;
        int which; /* 1 alignment, 2 dominant, 3 both */
    } T[] = {{"auto", SVT_BL_AUTO, 3},
             {"baseline", SVT_BL_BASELINE, 1},
             {"before-edge", SVT_BL_BEFORE_EDGE, 1},
             {"text-before-edge", SVT_BL_TEXT_BEFORE_EDGE, 3},
             {"middle", SVT_BL_MIDDLE, 3},
             {"central", SVT_BL_CENTRAL, 3},
             {"after-edge", SVT_BL_AFTER_EDGE, 1},
             {"text-after-edge", SVT_BL_TEXT_AFTER_EDGE, 3},
             {"ideographic", SVT_BL_IDEOGRAPHIC, 3},
             {"alphabetic", SVT_BL_ALPHABETIC, 3},
             {"hanging", SVT_BL_HANGING, 3},
             {"mathematical", SVT_BL_MATHEMATICAL, 3},
             {"use-script", SVT_BL_USE_SCRIPT, 2},
             {"no-change", SVT_BL_NO_CHANGE, 2},
             {"reset-size", SVT_BL_RESET_SIZE, 2}};
    size_t i;
    if (v == NULL) {
        return SVT_BL_AUTO;
    }
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (strcmp(v, T[i].s) == 0 && (T[i].which & (dominant ? 2 : 1)) != 0) {
            return T[i].b;
        }
    }
    return SVT_BL_AUTO;
}

/* ---------------------------------------------------------------- fonts */

static svf_font *convert_font(sv_conv *c, const st_node *n)
{
    svf_font *f = sv_alloc(c->a, sizeof *f);
    const char *v;
    const st_node *p;
    const st_node *chain[1100];
    size_t k = 0;
    int w = 400;
    if (f == NULL) {
        return NULL;
    }
    v = find_str(n, A_FONT_STYLE);
    f->style = eq(v, "italic") ? SVF_STYLE_ITALIC : eq(v, "oblique") ? SVF_STYLE_OBLIQUE : SVF_STYLE_NORMAL;
    f->stretch = 5;
    for (p = n; p != NULL; p = p->parent) {
        v = st_attr(p, A_FONT_STRETCH);
        if (v != NULL) {
            f->stretch = eq(v, "narrower") || eq(v, "condensed") ? 3
                         : eq(v, "ultra-condensed")              ? 1
                         : eq(v, "extra-condensed")              ? 2
                         : eq(v, "semi-condensed")               ? 4
                         : eq(v, "semi-expanded")                ? 6
                         : eq(v, "wider") || eq(v, "expanded")   ? 7
                         : eq(v, "extra-expanded")               ? 8
                         : eq(v, "ultra-expanded")               ? 9
                                                                 : 5;
            break;
        }
    }
    /* resolve_font_weight: from the outermost element down to n */
    for (p = n; p != NULL && p->kind != ST_ROOT && k < sizeof chain / sizeof chain[0]; p = p->parent) {
        chain[k++] = p;
    }
    while (k > 0) {
        const char *x = st_attr(chain[--k], A_FONT_WEIGHT);
        if (x == NULL) {
            continue;
        }
        if (strcmp(x, "normal") == 0) {
            w = 400;
        } else if (strcmp(x, "bold") == 0) {
            w = 700;
        } else if (strlen(x) == 3 && x[0] >= '1' && x[0] <= '9' && x[1] == '0' && x[2] == '0') {
            w = (x[0] - '0') * 100;
        } else if (strcmp(x, "bolder") == 0) {
            int step = w == 400 ? 300 : 100;
            w = w + step > 900 ? 900 : w + step;
        } else if (strcmp(x, "lighter") == 0) {
            int step = w == 400 ? 200 : 100;
            w = w - step < 100 ? 100 : w - step;
        }
    }
    f->weight = (uint16_t)w;
    v = NULL;
    for (p = n; p != NULL; p = p->parent) {
        if (st_has(p, A_FONT_FAMILY)) {
            v = st_attr(p, A_FONT_FAMILY);
            break;
        }
    }
    if (!svf_parse_families(c->a, v != NULL ? v : "", &f->fams, &f->nfams)) {
        if (c->a->nomem) {
            c->nomem = true;
            return NULL;
        }
        f->fams = NULL;
        f->nfams = 0;
    }
    if (f->nfams == 0) {
        f->fams = sv_alloc(c->a, sizeof *f->fams);
        if (f->fams == NULL) {
            c->nomem = true;
            return NULL;
        }
        f->fams[0].kind = SVF_NAMED;
        f->fams[0].name = DEFAULT_FAMILY;
        f->nfams = 1;
    }
    return f;
}

bool svf_font_eq(const svf_font *a, const svf_font *b)
{
    size_t i;
    if (a == b) {
        return true;
    }
    if (a->nfams != b->nfams || a->style != b->style || a->stretch != b->stretch || a->weight != b->weight) {
        return false;
    }
    for (i = 0; i < a->nfams; i++) {
        if (a->fams[i].kind != b->fams[i].kind ||
            (a->fams[i].kind == SVF_NAMED && strcmp(a->fams[i].name, b->fams[i].name) != 0)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- span attributes */

static void decoration(sv_conv *c, const st_node *n, const sv_cstate *st, const char *what, svt_deco *out)
{
    const st_node *p;
    bool any = false;
    size_t wl = strlen(what);
    memset(out, 0, sizeof *out);
    for (p = n; p != NULL && !any; p = p->parent) {
        const char *v = st_attr(p, A_TEXT_DECORATION);
        if (v != NULL) {
            const char *s = v;
            for (;;) {
                const char *e = strchr(s, ' ');
                size_t l = e != NULL ? (size_t)(e - s) : strlen(s);
                if (l == wl && memcmp(s, what, l) == 0) {
                    any = true;
                    break;
                }
                if (e == NULL) {
                    break;
                }
                s = e + 1;
            }
        }
    }
    if (!any) {
        return;
    }
    out->on = true;
    for (p = n; p != NULL; p = p->parent) {
        bool has = false;
        const char *v = st_attr(p, A_TEXT_DECORATION);
        if (v != NULL) {
            const char *s = v;
            for (;;) {
                const char *e = strchr(s, ' ');
                size_t l = e != NULL ? (size_t)(e - s) : strlen(s);
                if (l == wl && memcmp(s, what, l) == 0) {
                    has = true;
                    break;
                }
                if (e == NULL) {
                    break;
                }
                s = e + 1;
            }
        }
        if (has || (p->kind == ST_ELEMENT && p->eid == E_TEXT)) {
            out->fill = sv_resolve_fill(c, p, true, st);
            out->stroke = sv_resolve_stroke(c, p, true, st);
            return;
        }
    }
}

static void baseline_shift(sv_conv *c, const st_node *n, const sv_cstate *st, const svt_shift **out,
                           size_t *nout)
{
    svt_shift *v = NULL;
    size_t cnt = 0;
    size_t cap = 0;
    const st_node *p;
    bool all_baseline = true;
    *out = NULL;
    *nout = 0;
    for (p = n; p != NULL && p->kind == ST_ELEMENT && p->eid != E_TEXT; p = p->parent) {
        const char *s = st_attr(p, A_BASELINE_SHIFT);
        svt_shift sh;
        sv_length l;
        void *vv;
        if (s == NULL) {
            continue;
        }
        if (sv_parse_length(s, &l)) {
            sh.kind = SVT_SHIFT_NUMBER;
            if (l.unit == U_PERCENT) {
                sh.n = sv_resolve_font_size(p) * ((float)l.number / 100.0f);
            } else {
                sh.n = sv_convert_length(c, l, p, A_BASELINE_SHIFT, UNITS_OBB, st);
            }
        } else {
            sh.kind = strcmp(s, "sub") == 0     ? SVT_SHIFT_SUB
                      : strcmp(s, "super") == 0 ? SVT_SHIFT_SUPER
                                                : SVT_SHIFT_BASELINE;
            sh.n = 0.0f;
        }
        all_baseline = all_baseline && sh.kind == SVT_SHIFT_BASELINE;
        vv = v;
        if (!sv_grow(c->a, &vv, &cap, cnt + 1, sizeof *v)) {
            c->nomem = true;
            return;
        }
        v = vv;
        v[cnt++] = sh;
    }
    if (!all_baseline) {
        *out = v;
        *nout = cnt;
    }
}

/* ---------------------------------------------------------------- textPath */

typedef struct plen {
    double len;
} plen;

static void add_len(void *ctx, const svt_cubic *cb)
{
    ((plen *)ctx)->len += svt_cubic_arclen(cb, 0.5);
}

static const svt_tpath *text_flow(sv_conv *c, const st_node *tp, const sv_cstate *st)
{
    st_node *linked = st_node_attr(c->doc, tp, A_HREF);
    const sv_path *path;
    sv_ts t;
    sv_length so = {0.0, U_NONE};
    const char *v;
    svt_tpath *r;
    if (linked == NULL) {
        return NULL;
    }
    path = sv_shape_path(c, linked, st);
    if (path == NULL) {
        return NULL;
    }
    t = sv_resolve_transform(c, linked, A_TRANSFORM, st);
    if (!ts_is_identity(t)) {
        path = path_transform(c->a, path, t);
        if (path == NULL) {
            return NULL;
        }
    }
    r = sv_alloc(c->a, sizeof *r);
    if (r == NULL) {
        c->nomem = true;
        return NULL;
    }
    v = st_attr(tp, A_START_OFFSET);
    if (v != NULL && !sv_parse_length(v, &so)) {
        so.number = 0.0;
        so.unit = U_NONE;
    }
    if (so.unit == U_PERCENT) {
        plen pl = {0.0};
        svt_path_cubics(path, add_len, &pl);
        r->start_offset = (float)(pl.len * (so.number / 100.0));
    } else {
        r->start_offset = resolve_length(c, tp, A_START_OFFSET, st, 0.0f);
    }
    r->id = st_id(linked);
    if (r->id[0] == '\0') {
        return NULL;
    }
    r->path = path;
    return r;
}

/* ---------------------------------------------------------------- chunks */

typedef struct cpos {
    bool has_x, has_y, has_dx, has_dy;
    float x, y, dx, dy;
} cpos;

typedef struct iter {
    sv_conv *c;
    const sv_cstate *st;
    const cpos *pos;
    size_t npos;
    size_t chars;
    size_t chunk_bytes;
    bool split;
    const svt_tpath *flow;
    svt_chunk *chunks;
    size_t nchunks, cap;
    bool fail;
} iter;

static bool chunk_text_push(iter *it, svt_chunk *ch, const char *s, size_t n)
{
    void *v = ch->text;
    if (!sv_grow(it->c->a, &v, &ch->cap, ch->len + n + 1, 1)) {
        return false;
    }
    ch->text = v;
    memcpy(ch->text + ch->len, s, n);
    ch->len += n;
    ch->text[ch->len] = '\0';
    return true;
}

static bool chunk_span_push(iter *it, svt_chunk *ch, const svt_span *sp)
{
    void *v = ch->spans;
    if (!sv_grow(it->c->a, &v, &ch->capspans, ch->nspans + 1, sizeof(svt_span))) {
        return false;
    }
    ch->spans = v;
    ch->spans[ch->nspans++] = *sp;
    return true;
}

/* The TextSpan for the text-node children of element p (false: skip this text node). */
static bool make_span(iter *it, const st_node *p, svt_span *sp, svt_anchor *anchor)
{
    sv_conv *c = it->c;
    const sv_cstate *st = it->st;
    const char *v;
    float fs;
    memset(sp, 0, sizeof *sp);
    v = find_str(p, A_TEXT_ANCHOR);
    *anchor = eq(v, "middle") ? SVT_MIDDLE : eq(v, "end") ? SVT_END : SVT_START;
    fs = sv_resolve_font_size(p);
    if (!(fs > 0.0f) || !isfinite(fs)) {
        return false;
    }
    sp->font_size = fs;
    sp->font = convert_font(c, p);
    if (sp->font == NULL) {
        it->fail = true;
        return false;
    }
    sp->stroke_first = sv_stroke_first(p);
    sp->dominant = parse_baseline(find_str(p, A_DOMINANT_BASELINE), true);
    if (sp->dominant == SVT_BL_NO_CHANGE) {
        const st_node *pe = st_parent_element(p);
        sp->dominant = pe != NULL ? parse_baseline(find_str(pe, A_DOMINANT_BASELINE), true) : SVT_BL_AUTO;
    }
    sp->kerning = true;
    if (resolve_length(c, p, A_KERNING, st, -1.0f) == 0.0f) {
        sp->kerning = false;
    } else if (eq(find_str(p, A_FONT_KERNING), "none")) {
        sp->kerning = false;
    }
    v = st_attr(p, A_TEXT_LENGTH);
    if (v != NULL) {
        sv_length l;
        if (sv_parse_length(v, &l)) {
            float tl = sv_convert_length(c, l, p, A_TEXT_LENGTH, UNITS_USER, st);
            if (!(tl < 0.0f)) {
                sp->has_length = true;
                sp->text_length = tl;
            }
        }
    }
    v = find_str(p, A_VISIBILITY);
    sp->visible = !(eq(v, "hidden") || eq(v, "collapse"));
    sp->fill = sv_resolve_fill(c, p, true, st);
    sp->stroke = sv_resolve_stroke(c, p, true, st);
    sp->small_caps = eq(find_str(p, A_FONT_VARIANT), "small-caps");
    decoration(c, p, st, "underline", &sp->under);
    decoration(c, p, st, "overline", &sp->over);
    decoration(c, p, st, "line-through", &sp->through);
    sp->alignment = parse_baseline(find_str(p, A_ALIGNMENT_BASELINE), false);
    baseline_shift(c, p, st, &sp->shift, &sp->nshift);
    sp->letter_spacing = resolve_length(c, p, A_LETTER_SPACING, st, 0.0f);
    sp->word_spacing = resolve_length(c, p, A_WORD_SPACING, st, 0.0f);
    sp->glyphs_adjust = eq(find_str(p, A_LENGTH_ADJUST), "spacingAndGlyphs");
    if (c->nomem) {
        it->fail = true;
        return false;
    }
    return true;
}

static void collect_impl(iter *it, const st_node *parent, size_t depth)
{
    const st_node *ch;
    if (depth > 1100) {
        return;
    }
    for (ch = parent->first; ch != NULL && !it->fail; ch = ch->next) {
        svt_span span;
        svt_anchor anchor;
        const char *t;
        size_t i;
        bool new_span = true;
        if (ch->kind == ST_ELEMENT) {
            if (ch->eid == E_TEXT_PATH) {
                if (parent->eid != E_TEXT) {
                    it->chars += count_chars(ch);
                    continue;
                }
                it->flow = text_flow(it->c, ch, it->st);
                if (it->flow == NULL) {
                    if (it->c->nomem) {
                        it->fail = true;
                        return;
                    }
                    it->chars += count_chars(ch);
                    continue;
                }
                it->split = true;
            }
            collect_impl(it, ch, depth + 1);
            it->flow = NULL;
            if (ch->eid == E_TEXT_PATH) {
                it->split = true;
            }
            continue;
        }
        if (ch->kind != ST_TEXT) {
            continue;
        }
        t = node_text(ch);
        if (!sv_is_visible_element(parent) || !make_span(it, parent, &span, &anchor)) {
            it->chars += utf8_count(t);
            continue;
        }
        for (i = 0; t[i] != '\0';) {
            size_t cl = 1;
            unsigned char b = (unsigned char)t[i];
            const cpos *pp;
            bool is_new_chunk;
            cl = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
            if (strnlen(t + i, cl) < cl) {
                cl = strnlen(t + i, cl);
            }
            if (it->chars >= it->npos) {
                it->fail = true;
                return;
            }
            pp = &it->pos[it->chars];
            is_new_chunk = pp->has_x || pp->has_y || it->split || it->nchunks == 0;
            it->split = false;
            if (is_new_chunk) {
                svt_chunk *c;
                void *v = it->chunks;
                if (!sv_grow(it->c->a, &v, &it->cap, it->nchunks + 1, sizeof(svt_chunk))) {
                    it->fail = true;
                    return;
                }
                it->chunks = v;
                c = &it->chunks[it->nchunks++];
                memset(c, 0, sizeof *c);
                it->chunk_bytes = 0;
                c->has_x = pp->has_x;
                c->x = pp->x;
                c->has_y = pp->has_y;
                c->y = pp->y;
                c->anchor = anchor;
                c->path = it->flow;
                span.start = 0;
                span.end = cl;
                if (!chunk_span_push(it, c, &span) || !chunk_text_push(it, c, t + i, cl)) {
                    it->fail = true;
                    return;
                }
            } else if (new_span) {
                svt_chunk *c = &it->chunks[it->nchunks - 1];
                span.start = it->chunk_bytes;
                span.end = it->chunk_bytes + cl;
                if (!chunk_text_push(it, c, t + i, cl) || !chunk_span_push(it, c, &span)) {
                    it->fail = true;
                    return;
                }
            } else {
                svt_chunk *c = &it->chunks[it->nchunks - 1];
                if (!chunk_text_push(it, c, t + i, cl)) {
                    it->fail = true;
                    return;
                }
                c->spans[c->nspans - 1].end += cl;
            }
            new_span = false;
            it->chars++;
            it->chunk_bytes += cl;
            i += cl;
        }
    }
}

/* ---------------------------------------------------------------- positions and rotations */

/* Number lists of x/y/dx/dy (units::convert_list: bad items skipped... the parser stops). */
static size_t convert_list(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st, float **out)
{
    const char *v = st_attr(n, aid);
    sv_stream s;
    float *r = NULL;
    size_t k = 0;
    size_t cap = 0;
    *out = NULL;
    if (v == NULL) {
        return SIZE_MAX;
    }
    ss_init(&s, v);
    while (!ss_at_end(&s)) {
        sv_length l;
        void *vv;
        if (!ss_parse_list_length(&s, &l)) {
            break;
        }
        vv = r;
        if (!sv_grow(c->a, &vv, &cap, k + 1, sizeof(float))) {
            c->nomem = true;
            return SIZE_MAX;
        }
        r = vv;
        r[k++] = sv_convert_length(c, l, n, aid, UNITS_USER, st);
    }
    *out = r;
    return k;
}

/* Vec<f32> from a number list: None when any item fails. */
static bool number_list(sv_conv *c, const char *v, float **out, size_t *n)
{
    sv_stream s;
    float *r = NULL;
    size_t k = 0;
    size_t cap = 0;
    ss_init(&s, v);
    while (!ss_at_end(&s)) {
        double d;
        void *vv;
        if (!ss_parse_list_number(&s, &d)) {
            return false;
        }
        vv = r;
        if (!sv_grow(c->a, &vv, &cap, k + 1, sizeof(float))) {
            c->nomem = true;
            return false;
        }
        r = vv;
        r[k++] = (float)d;
    }
    *out = r;
    *n = k;
    return true;
}

/* count_chars of every element of the subtree (text included), in pre-order. Recursion depth is
 * the XML nesting (at most 1024). */
static size_t count_rec(const st_node *e, size_t *cnt, size_t *idx)
{
    size_t me = (*idx)++;
    size_t sum = 0;
    const st_node *ch;
    for (ch = e->first; ch != NULL; ch = ch->next) {
        if (ch->kind == ST_TEXT) {
            sum += utf8_count(node_text(ch));
        } else if (ch->kind == ST_ELEMENT) {
            sum += count_rec(ch, cnt, idx);
        }
    }
    cnt[me] = sum;
    return sum;
}

static size_t *subtree_counts(sv_conv *c, const st_node *text, size_t *nel)
{
    const st_node *e;
    size_t n = 1;
    size_t idx = 0;
    size_t *cnt;
    for (e = text->first; e != NULL; e = next_desc(text, e)) {
        if (e->kind == ST_ELEMENT) {
            n++;
        }
    }
    cnt = sv_alloc_array(c->a, n, sizeof *cnt);
    if (cnt == NULL) {
        c->nomem = true;
        return NULL;
    }
    (void)count_rec(text, cnt, &idx);
    *nel = n;
    return cnt;
}

static bool positions(sv_conv *c, const st_node *text, const sv_cstate *st, const size_t *cnt, size_t total,
                      cpos **out, float **rot)
{
    cpos *list = sv_alloc_array(c->a, total + 1, sizeof *list);
    float *r = sv_alloc_array(c->a, total + 1, sizeof *r);
    const st_node *e;
    size_t offset = 0;
    size_t idx = 0;
    float last = 0.0f;
    if (list == NULL || r == NULL) {
        c->nomem = true;
        return false;
    }
    for (e = text; e != NULL; e = e == text ? text->first : next_desc(text, e)) {
        if (e->kind == ST_ELEMENT) {
            size_t cc = cnt[idx++];
            const char *rv;
            if (e->eid == E_TEXT || e->eid == E_TSPAN) {
                static const sv_aid A[4] = {A_X, A_Y, A_DX, A_DY};
                int k;
                for (k = 0; k < 4; k++) {
                    float *nums;
                    size_t nn = convert_list(c, e, A[k], st, &nums);
                    size_t i;
                    if (c->nomem) {
                        return false;
                    }
                    if (nn == SIZE_MAX) {
                        continue;
                    }
                    for (i = 0; i < nn && i < cc && offset + i < total; i++) {
                        cpos *p = &list[offset + i];
                        switch (k) {
                        case 0:
                            p->has_x = true;
                            p->x = nums[i];
                            break;
                        case 1:
                            p->has_y = true;
                            p->y = nums[i];
                            break;
                        case 2:
                            p->has_dx = true;
                            p->dx = nums[i];
                            break;
                        default:
                            p->has_dy = true;
                            p->dy = nums[i];
                            break;
                        }
                    }
                }
            }
            rv = st_attr(e, A_ROTATE);
            if (rv != NULL) {
                float *nums = NULL;
                size_t nn = 0;
                if (number_list(c, rv, &nums, &nn)) {
                    size_t i;
                    for (i = 0; i < cc && offset + i < total; i++) {
                        if (i < nn) {
                            r[offset + i] = nums[i];
                            last = nums[i];
                        } else {
                            r[offset + i] = last;
                        }
                    }
                } else if (c->nomem) {
                    return false;
                }
            }
        } else if (e->kind == ST_TEXT) {
            offset += utf8_count(node_text(e));
        }
    }
    *out = list;
    *rot = r;
    return true;
}

/* ---------------------------------------------------------------- convert */

void sv_convert_text(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    size_t total = count_chars(n);
    size_t nel = 0;
    size_t *cnt;
    cpos *pos;
    float *rot;
    iter it;
    svt_text t;
    const char *wm = NULL;
    const st_node *p;
    svt_fonts fonts;
    svt_budget bud;
    svt_lspan *spans = NULL;
    size_t nspans = 0;
    sv_rect bbox;
    sv_rect sbbox;
    sv_unode *flat;
    sv_unode *u;
    size_t i;
    size_t tl = 0;
    char *alltext;
    if (total > SVT_MAX_CHARS || c->text_glyphs >= SVT_MAX_GLYPHS) {
        return;
    }
    cnt = subtree_counts(c, n, &nel);
    if (cnt == NULL || !positions(c, n, st, cnt, total, &pos, &rot)) {
        return;
    }
    memset(&it, 0, sizeof it);
    it.c = c;
    it.st = st;
    it.pos = pos;
    it.npos = total;
    collect_impl(&it, n, 0);
    if (it.fail) {
        return;
    }
    memset(&t, 0, sizeof t);
    t.id = st->markers != NULL ? "" : st_id(n); /* nodes generated by markers have no id */
    t.nchars = total;
    t.dx = sv_alloc_array(c->a, total + 1, sizeof(float));
    t.dy = sv_alloc_array(c->a, total + 1, sizeof(float));
    if (t.dx == NULL || t.dy == NULL) {
        c->nomem = true;
        return;
    }
    for (i = 0; i < total; i++) {
        t.dx[i] = pos[i].has_dx ? pos[i].dx : 0.0f;
        t.dy[i] = pos[i].has_dy ? pos[i].dy : 0.0f;
    }
    t.rotate = rot;
    for (p = n; p != NULL; p = p->parent) {
        if (st_has(p, A_WRITING_MODE)) {
            wm = st_attr(p, A_WRITING_MODE);
            break;
        }
    }
    t.vertical = eq(wm, "tb") || eq(wm, "tb-rl") || eq(wm, "vertical-rl") || eq(wm, "vertical-lr");
    t.chunks = it.chunks;
    t.nchunks = it.nchunks;
    t.abs = parent->abs_transform;
    t.crisp = eq(find_str(n, A_TEXT_RENDERING), "optimizeSpeed");
    memset(&fonts, 0, sizeof fonts);
    fonts.db = NULL;
    bud.glyphs = c->text_glyphs;
    bud.work = c->text_work;
    if (!svt_layout(c, &t, &fonts, &bud, &spans, &nspans, &bbox)) {
        c->text_glyphs = bud.glyphs;
        c->text_work = bud.work;
        svt_fonts_fini(&fonts);
        return;
    }
    c->text_glyphs = bud.glyphs;
    c->text_work = bud.work;
    flat = svt_flatten(c, &t, spans, nspans, &sbbox);
    svt_fonts_fini(&fonts);
    if (flat == NULL) {
        return;
    }
    u = sv_alloc(c->a, sizeof *u);
    if (u == NULL) {
        c->nomem = true;
        return;
    }
    u->kind = UN_TEXT;
    u->id = t.id;
    u->transform = ts_identity();
    u->abs_transform = parent->abs_transform;
    u->opacity = 1.0f;
    u->flat = flat;
    u->bbox = bbox;
    if (!nzrect_transform(bbox, t.abs, &u->abs_bbox)) {
        return;
    }
    u->stroke_bbox = sbbox;
    if (!nzrect_transform(sbbox, t.abs, &u->abs_stroke_bbox)) {
        return;
    }
    for (i = 0; i < t.nchunks; i++) {
        tl += t.chunks[i].len;
    }
    alltext = sv_alloc(c->a, tl + 1);
    if (alltext == NULL) {
        c->nomem = true;
        return;
    }
    tl = 0;
    for (i = 0; i < t.nchunks; i++) {
        memcpy(alltext + tl, t.chunks[i].text, t.chunks[i].len);
        tl += t.chunks[i].len;
    }
    alltext[tl] = '\0';
    u->text = alltext;
    if (!sv_push_child(c->a, parent, u)) {
        c->nomem = true;
    }
}

/* char::is_whitespace (White_Space) for str::trim. */
static size_t ws_len(const unsigned char *s)
{
    if (*s == ' ' || (*s >= 0x09 && *s <= 0x0D)) {
        return 1;
    }
    if (s[0] == 0xC2 && (s[1] == 0x85 || s[1] == 0xA0)) {
        return 2;
    }
    if (s[0] == 0xE1 && s[1] == 0x9A && s[2] == 0x80) {
        return 3;
    }
    if (s[0] == 0xE2 && s[1] == 0x80 &&
        ((s[2] >= 0x80 && s[2] <= 0x8A) || s[2] == 0xA8 || s[2] == 0xA9 || s[2] == 0xAF)) {
        return 3;
    }
    if (s[0] == 0xE2 && s[1] == 0x81 && s[2] == 0x9F) {
        return 3;
    }
    if (s[0] == 0xE3 && s[1] == 0x80 && s[2] == 0x80) {
        return 3;
    }
    return 0;
}

bool sv_text_layer_name(const char *text, char *out, size_t cap)
{
    const unsigned char *s = (const unsigned char *)(text != NULL ? text : "");
    size_t n;
    size_t end;
    size_t i;
    size_t chars = 0;
    size_t k;
    while (*s != '\0' && ws_len(s) > 0) {
        s += ws_len(s);
    }
    n = strlen((const char *)s);
    /* trim the end: the last non-white-space character */
    end = 0;
    for (i = 0; i < n;) {
        size_t w = ws_len(s + i);
        if (w > 0) {
            i += w;
        } else {
            i++;
            while (i < n && (s[i] & 0xC0) == 0x80) {
                i++;
            }
            end = i;
        }
    }
    if (end == 0 || cap == 0) {
        return false;
    }
    for (k = 0; k < end && chars < 31;) {
        size_t cl = 1;
        while (k + cl < end && (s[k + cl] & 0xC0) == 0x80) {
            cl++;
        }
        chars++;
        k += cl;
    }
    if (k >= cap) {
        k = cap - 1;
        while (k > 0 && (s[k] & 0xC0) == 0x80) {
            k--;
        }
    }
    memcpy(out, s, k);
    out[k] = '\0';
    return true;
}
