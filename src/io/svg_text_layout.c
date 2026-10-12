/* Text layout: a port of usvg 0.45.1 text/layout.rs (layout_text and everything it calls): font
 * selection with fontdb's query and the default fallback scan, shaping per chunk with fallback,
 * glyph clusters, writing mode, letter and word spacing, textLength, anchors, x/y/dx/dy/rotate,
 * baselines, text on a path and decoration lines.
 *
 * The reference's per-cluster searches (code_point_at, chunk_span_at, position by byte index) are
 * linear and make large texts quadratic; here they are index lookups with the same results. The
 * cluster merge of process_chunk visits only the glyphs of each span and charges the per-import
 * work budget (SVT_MAX_WORK); a text past the budget or the glyph cap is dropped. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "io/svg_text.h"

/* ---------------------------------------------------------------- transforms */

static sv_ts rotate_ts(float deg)
{
    float v = deg * (3.14159265358979323846f / 180.0f);
    float a = cosf(v);
    float b = sinf(v);
    return ts_row(a, b, -b, a, 0.0f, 0.0f);
}

static sv_ts pre_rotate(sv_ts t, float deg)
{
    return ts_pre_concat(t, rotate_ts(deg));
}

static sv_ts pre_rotate_at(sv_ts t, float deg, float x, float y)
{
    sv_ts r = ts_identity();
    r = ts_pre_translate(r, x, y);
    r = ts_pre_concat(r, rotate_ts(deg));
    r = ts_pre_translate(r, -x, -y);
    return ts_pre_concat(t, r);
}

static sv_ts pre_scale(sv_ts t, float sx, float sy)
{
    return ts_pre_concat(t, ts_scale(sx, sy));
}

static bool approx_zero(float v)
{
    return sv_approx_eq_ulps(v, 0.0f, 4);
}

sv_ts svt_outline_ts(const svt_pglyph *g)
{
    float sx = g->font_size / (float)g->upem;
    sv_ts t = ts_pre_concat(g->span_ts, g->cluster_ts);
    t = ts_pre_concat(t, ts_scale(sx, sx));
    t = ts_pre_concat(t, g->glyph_ts);
    return ts_pre_concat(t, ts_scale(1.0f, -1.0f));
}

/* ---------------------------------------------------------------- per-chunk adjustments */

static void apply_writing_mode(bool vertical, svt_cluster *cl, size_t n)
{
    size_t i;
    if (!vertical) {
        return;
    }
    for (i = 0; i < n; i++) {
        svt_cluster *c = &cl[i];
        if (svt_is_upright(c->cp)) {
            sv_ts ts = ts_identity();
            ts = ts_pre_translate(ts, 0.0f, (c->ascent + c->descent) / 2.0f);
            ts = pre_rotate_at(ts, -90.0f, c->width / 2.0f, -(c->ascent + c->descent) / 2.0f);
            c->path_transform = ts;
            c->ascent = c->width / 2.0f;
            c->descent = -c->width / 2.0f;
        } else {
            c->transform = ts_pre_translate(c->transform, 0.0f, (c->ascent + c->descent) / 2.0f);
        }
    }
}

static void apply_letter_spacing(const svt_chunk *ch, svt_cluster *cl, size_t n)
{
    size_t i;
    bool any = false;
    for (i = 0; i < ch->nspans; i++) {
        any = any || !approx_zero(ch->spans[i].letter_spacing);
    }
    if (!any) {
        return;
    }
    for (i = 0; i < n; i++) {
        svt_cluster *c = &cl[i];
        const svt_span *sp;
        if (!svt_script_supports_letter_spacing(c->cp)) {
            continue;
        }
        sp = svt_span_at(ch, c->byte_idx, NULL);
        if (sp == NULL) {
            continue;
        }
        if (i != n - 1) {
            c->advance += sp->letter_spacing;
        }
        if (!sv_valid_length(c->advance)) {
            c->width = 0.0f;
            c->advance = 0.0f;
            c->nglyphs = 0;
        }
    }
}

static bool is_word_separator(uint32_t c)
{
    return c == 0x20 || c == 0xA0 || c == 0x1361 || c == 0x10100 || c == 0x10101 || c == 0x1039F ||
           c == 0x1091F;
}

static void apply_word_spacing(const svt_chunk *ch, svt_cluster *cl, size_t n)
{
    size_t i;
    bool any = false;
    for (i = 0; i < ch->nspans; i++) {
        any = any || !approx_zero(ch->spans[i].word_spacing);
    }
    if (!any) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (is_word_separator(cl[i].cp)) {
            const svt_span *sp = svt_span_at(ch, cl[i].byte_idx, NULL);
            if (sp != NULL) {
                cl[i].advance += sp->word_spacing;
            }
        }
    }
}

/* first cluster index per byte (SIZE_MAX: none) */
static size_t *cluster_index(sv_arena *a, const svt_chunk *ch, const svt_cluster *cl, size_t n)
{
    size_t *m = sv_alloc_array(a, ch->len + 1, sizeof *m);
    size_t i;
    if (m == NULL) {
        return NULL;
    }
    for (i = 0; i <= ch->len; i++) {
        m[i] = SIZE_MAX;
    }
    for (i = n; i-- > 0;) {
        if (cl[i].byte_idx <= ch->len) {
            m[cl[i].byte_idx] = i;
        }
    }
    return m;
}

static int cmp_size(const void *x, const void *y)
{
    size_t a = *(const size_t *)x;
    size_t b = *(const size_t *)y;
    return a < b ? -1 : a > b ? 1 : 0;
}

static bool apply_length_adjust(svt_lctx *l, const svt_chunk *ch, svt_cluster *cl, size_t n,
                                const size_t *first)
{
    bool horizontal = ch->path == NULL;
    size_t s;
    for (s = 0; s < ch->nspans; s++) {
        const svt_span *sp = &ch->spans[s];
        size_t *idx;
        size_t k = 0;
        size_t b;
        float width = 0.0f;
        size_t i;
        if (!sp->has_length) {
            continue;
        }
        idx = sv_alloc_array(l->c->a, sp->end - sp->start + 1, sizeof *idx);
        if (idx == NULL) {
            l->fail = true;
            return false;
        }
        for (b = sp->start; b < sp->end && b <= ch->len; b++) {
            if (first[b] != SIZE_MAX) {
                idx[k++] = first[b];
            }
        }
        qsort(idx, k, sizeof *idx, cmp_size);
        {
            size_t w = 0;
            for (i = 0; i < k; i++) {
                if (w == 0 || idx[w - 1] != idx[i]) {
                    idx[w++] = idx[i];
                }
            }
            k = w;
        }
        for (i = 0; i < k; i++) {
            width += cl[idx[i]].width;
        }
        if (k == 0) {
            continue;
        }
        if (!sp->glyphs_adjust) {
            float factor = k > 1 ? (sp->text_length - width) / (float)(k - 1) : 0.0f;
            for (i = 0; i < k; i++) {
                cl[idx[i]].advance = cl[idx[i]].width + factor;
            }
        } else {
            float factor = sp->text_length / width;
            if (factor < 0.001f) {
                continue;
            }
            for (i = 0; i < k; i++) {
                cl[idx[i]].transform = pre_scale(cl[idx[i]].transform, factor, 1.0f);
                if (!horizontal) {
                    cl[idx[i]].advance *= factor;
                    cl[idx[i]].width *= factor;
                }
            }
        }
    }
    (void)n;
    return true;
}

/* ---------------------------------------------------------------- positions */

static float process_anchor(svt_anchor a, float w)
{
    return a == SVT_MIDDLE ? -w / 2.0f : a == SVT_END ? -w : 0.0f;
}

static float clusters_length(const svt_cluster *cl, size_t n)
{
    float w = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        w += cl[i].advance;
    }
    return w;
}

/* code_point_at per byte of the chunk text. */
static size_t *cp_index(sv_arena *a, const svt_chunk *ch)
{
    size_t *m = sv_alloc_array(a, ch->len + 1, sizeof *m);
    size_t i;
    size_t k = 0;
    if (m == NULL) {
        return NULL;
    }
    for (i = 0; i <= ch->len; i++) {
        m[i] = k;
        if (i < ch->len && ((unsigned char)ch->text[i] & 0xC0) != 0x80) {
            k++;
        }
    }
    /* code_point_at counts the characters before the byte (all of them when it is not a
     * character start) */
    for (i = 0; i <= ch->len; i++) {
        if (i < ch->len && ((unsigned char)ch->text[i] & 0xC0) == 0x80) {
            m[i] = k;
        }
    }
    m[ch->len] = k;
    return m;
}

static const float *at(const float *v, size_t n, size_t i)
{
    return i < n ? &v[i] : NULL;
}

static void positions_horizontal(const svt_text *t, const svt_chunk *ch, size_t off, const size_t *cpi,
                                 svt_cluster *cl, size_t n, float *ox, float *oy)
{
    float x = process_anchor(ch->anchor, clusters_length(cl, n));
    float y = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        svt_cluster *c = &cl[i];
        size_t cp = off + cpi[c->byte_idx <= ch->len ? c->byte_idx : ch->len];
        const float *dx = at(t->dx, t->nchars, cp);
        const float *dy = at(t->dy, t->nchars, cp);
        const float *r = at(t->rotate, t->nchars, cp);
        if (dx != NULL && dy != NULL) {
            if (!t->vertical) {
                x += *dx;
                y += *dy;
            } else {
                y -= *dx;
                x += *dy;
            }
            c->has_relative_shift = !approx_zero(*dx) || !approx_zero(*dy);
        }
        c->transform = ts_pre_translate(c->transform, x, y);
        if (r != NULL && !approx_zero(*r)) {
            c->transform = pre_rotate(c->transform, *r);
            c->has_relative_shift = true;
        }
        x += c->advance;
    }
    *ox = x;
    *oy = y;
}

static float baseline_shift_of(const svt_span *sp, const svt_rfont *f)
{
    float shift = 0.0f;
    size_t i = sp->nshift;
    while (i-- > 0) {
        switch (sp->shift[i].kind) {
        case SVT_SHIFT_SUB:
            shift -= (float)f->sub_off * svt_scale(f, sp->font_size);
            break;
        case SVT_SHIFT_SUPER:
            shift += (float)f->super_off * svt_scale(f, sp->font_size);
            break;
        case SVT_SHIFT_NUMBER:
            shift += sp->shift[i].n;
            break;
        case SVT_SHIFT_BASELINE:
        default:
            break;
        }
    }
    return shift;
}

static float alignment_shift(const svt_rfont *f, svt_baseline b, float fs)
{
    float s = svt_scale(f, fs);
    float asc = (float)f->ascent * s;
    float desc = (float)f->descent * s;
    switch (b) {
    case SVT_BL_BEFORE_EDGE:
    case SVT_BL_TEXT_BEFORE_EDGE:
        return asc;
    case SVT_BL_MIDDLE:
        return (float)f->x_height * s * 0.5f;
    case SVT_BL_CENTRAL:
        return asc - (asc - desc) * 0.5f;
    case SVT_BL_AFTER_EDGE:
    case SVT_BL_TEXT_AFTER_EDGE:
    case SVT_BL_IDEOGRAPHIC:
        return desc;
    case SVT_BL_HANGING:
        return asc * 0.8f;
    case SVT_BL_MATHEMATICAL:
        return asc * 0.5f;
    default:
        return 0.0f;
    }
}

static float resolve_baseline(const svt_span *sp, const svt_rfont *f, bool vertical)
{
    float shift = -baseline_shift_of(sp, f);
    if (!vertical) {
        if (sp->alignment == SVT_BL_AUTO || sp->alignment == SVT_BL_BASELINE) {
            svt_baseline d = sp->dominant;
            if (d == SVT_BL_USE_SCRIPT || d == SVT_BL_NO_CHANGE || d == SVT_BL_RESET_SIZE) {
                d = SVT_BL_AUTO;
            }
            shift += alignment_shift(f, d, sp->font_size);
        } else {
            shift += alignment_shift(f, sp->alignment, sp->font_size);
        }
    }
    return shift;
}

/* collect_normals of text on a path. */
typedef struct normal {
    bool some;
    float x, y, angle;
} normal;

typedef struct nctx {
    const double *offsets;
    size_t noff;
    normal *normals;
    size_t nn;
    double length;
    double acc;
} nctx;

static double d_eval(double p0, double p1, double p2, double p3, double t)
{
    double mt = 1.0 - t;
    return p0 * (mt * mt * mt) + (p1 * (mt * mt * 3.0) + (p2 * (mt * 3.0) + p3 * t) * t) * t;
}

static void normals_seg(void *ctx, const svt_cubic *c)
{
    nctx *n = ctx;
    double len = svt_cubic_arclen(c, n->acc);
    size_t i;
    size_t from = n->nn;
    for (i = from; i < n->noff; i++) {
        double off = n->offsets[i];
        if (n->nn >= n->noff) {
            break;
        }
        if (off >= n->length && off <= n->length + len) {
            double t = svt_cubic_inv_arclen(c, off - n->length, n->acc);
            double px, py, d0x, d0y, d1x, d1y, d2x, d2y, mt, dx, dy, ang;
            t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
            px = d_eval(c->x0, c->x1, c->x2, c->x3, t);
            py = d_eval(c->y0, c->y1, c->y2, c->y3, t);
            d0x = 3.0 * (c->x1 - c->x0);
            d0y = 3.0 * (c->y1 - c->y0);
            d1x = 3.0 * (c->x2 - c->x1);
            d1y = 3.0 * (c->y2 - c->y1);
            d2x = 3.0 * (c->x3 - c->x2);
            d2y = 3.0 * (c->y3 - c->y2);
            mt = 1.0 - t;
            dx = d0x * (mt * mt) + (d1x * (mt * 2.0) + d2x * t) * t;
            dy = d0y * (mt * mt) + (d1y * (mt * 2.0) + d2y * t) * t;
            /* tangent rotated: Vec2(-d.y, d.x) */
            ang = atan2(dx, -dy) * (180.0 / 3.14159265358979323846) - 90.0;
            n->normals[n->nn].some = true;
            n->normals[n->nn].x = (float)px;
            n->normals[n->nn].y = (float)py;
            n->normals[n->nn].angle = (float)ang;
            n->nn++;
            if (n->nn == n->noff) {
                break;
            }
        }
    }
    n->length += len;
}

static bool positions_path(svt_lctx *l, const svt_text *t, const svt_chunk *ch, size_t off, const size_t *cpi,
                           svt_cluster *cl, size_t n, float *ox, float *oy)
{
    float last_x = 0.0f;
    float last_y = 0.0f;
    float dy = 0.0f;
    float chunk_off = !t->vertical ? (ch->has_x ? ch->x : 0.0f) : (ch->has_y ? ch->y : 0.0f);
    float start = chunk_off + ch->path->start_offset + process_anchor(ch->anchor, clusters_length(cl, n));
    double *offs = sv_alloc_array(l->c->a, n + 1, sizeof *offs);
    normal *norms = sv_alloc_array(l->c->a, n + 1, sizeof *norms);
    nctx nc;
    size_t i;
    float sx, sy;
    if (offs == NULL || norms == NULL) {
        l->fail = true;
        return false;
    }
    memset(&nc, 0, sizeof nc);
    {
        float adv = start;
        for (i = 0; i < n; i++) {
            float half = cl[i].width / 2.0f;
            size_t cp = off + cpi[cl[i].byte_idx <= ch->len ? cl[i].byte_idx : ch->len];
            const float *d = at(t->dx, t->nchars, cp);
            float o;
            adv += d != NULL ? *d : 0.0f;
            o = adv + half;
            if (o < 0.0f) {
                norms[nc.nn++].some = false;
            }
            offs[i] = (double)o;
            adv += cl[i].advance;
        }
    }
    ts_get_scale(t->abs, &sx, &sy);
    {
        float k = sqrtf(sx * sy);
        nc.acc = (double)(0.5f / (k > 1.0f ? k : 1.0f));
    }
    nc.offsets = offs;
    nc.noff = n;
    nc.normals = norms;
    if (!svt_charge(l, n + ch->path->path->nverbs)) {
        return false;
    }
    svt_path_cubics(ch->path->path, normals_seg, &nc);
    while (nc.nn < n) {
        norms[nc.nn++].some = false;
    }
    for (i = 0; i < n; i++) {
        svt_cluster *c = &cl[i];
        sv_ts orig;
        float half;
        size_t cp;
        const svt_span *sp;
        float bshift = 0.0f;
        const float *r;
        const float *ddy;
        if (!norms[i].some) {
            c->visible = false;
            continue;
        }
        c->has_relative_shift = true;
        orig = c->transform;
        half = c->width / 2.0f;
        c->transform = ts_identity();
        c->transform = ts_pre_translate(c->transform, norms[i].x - half, norms[i].y);
        c->transform = pre_rotate_at(c->transform, norms[i].angle, half, 0.0f);
        cp = off + cpi[c->byte_idx <= ch->len ? c->byte_idx : ch->len];
        ddy = at(t->dy, t->nchars, cp);
        dy += ddy != NULL ? *ddy : 0.0f;
        sp = svt_span_at(ch, c->byte_idx, NULL);
        if (sp != NULL) {
            const svt_rfont *f = svt_fonts_get(l->fonts, sp->font);
            bshift = f != NULL ? -resolve_baseline(sp, f, t->vertical) : 0.0f;
        }
        if (!approx_zero(dy) || !approx_zero(bshift)) {
            double shift_y = (double)(dy - bshift);
            c->transform = ts_pre_translate(c->transform, 0.0f, (float)shift_y);
        }
        r = at(t->rotate, t->nchars, cp);
        if (r != NULL && !approx_zero(*r)) {
            c->transform = pre_rotate(c->transform, *r);
        }
        c->transform = ts_pre_concat(c->transform, orig);
        last_x = norms[i].x + c->advance;
        last_y = norms[i].y;
    }
    *ox = last_x;
    *oy = last_y;
    return true;
}

/* ---------------------------------------------------------------- decorations and spans */

typedef struct dspan {
    float width;
    sv_ts ts;
} dspan;

static bool pb_push_path(sv_pb *b, const sv_path *p)
{
    void *v = b->verbs;
    void *q;
    if (!sv_grow(b->a, &v, &b->capverbs, b->nverbs + p->nverbs, 1)) {
        b->failed = true;
        return false;
    }
    b->verbs = v;
    q = b->pts;
    if (!sv_grow(b->a, &q, &b->cappts, b->npts + p->npts, sizeof(sv_pt))) {
        b->failed = true;
        return false;
    }
    b->pts = q;
    b->last_move_to = b->npts;
    memcpy(b->verbs + b->nverbs, p->verbs, p->nverbs);
    memcpy(b->pts + b->npts, p->pts, p->npts * sizeof(sv_pt));
    b->nverbs += p->nverbs;
    b->npts += p->npts;
    return true;
}

static sv_unode *convert_decoration(svt_lctx *l, float dy, const svt_span *sp, const svt_rfont *f,
                                    const svt_deco *d, const dspan *ds, size_t nds, sv_ts transform)
{
    sv_arena *a = l->c->a;
    float th = (float)f->underline_thick * svt_scale(f, sp->font_size);
    sv_pb pb;
    size_t i;
    sv_path *data;
    const sv_path *td;
    sv_unode *u;
    pb_init(&pb, a);
    for (i = 0; i < nds; i++) {
        sv_rect r;
        const sv_path *p;
        if (!nzrect_from_xywh(0.0f, -th / 2.0f, ds[i].width, th, &r)) {
            continue;
        }
        p = path_from_rect(a, r);
        if (p == NULL) {
            l->fail = true;
            return NULL;
        }
        p = path_transform(a, p, ts_pre_translate(ds[i].ts, 0.0f, dy));
        if (p == NULL) {
            if (a->nomem) {
                l->fail = true;
                return NULL;
            }
            continue;
        }
        if (!pb_push_path(&pb, p)) {
            l->fail = true;
            return NULL;
        }
    }
    data = pb_finish(&pb);
    if (data == NULL) {
        if (pb.failed) {
            l->fail = true;
        }
        return NULL;
    }
    td = path_transform(a, data, transform);
    if (td == NULL) {
        return NULL;
    }
    u = sv_new_path(l->c, "", sp->visible, d->fill, d->stroke, false, td, ts_identity());
    if (u != NULL) {
        u->crisp = l->t->crisp;
    }
    return u;
}

static size_t collect_deco(const svt_cluster *cl, size_t lo, size_t hi, const svt_span *sp, dspan *out)
{
    size_t n = 0;
    bool started = false;
    float width = 0.0f;
    sv_ts ts = ts_identity();
    size_t i;
    for (i = lo; i < hi; i++) {
        const svt_cluster *c = &cl[i];
        if (svt_span_contains(sp, c->byte_idx)) {
            if (started && c->has_relative_shift) {
                started = false;
                out[n].width = width;
                out[n++].ts = ts;
            }
            if (!started) {
                width = c->advance;
                started = true;
                ts = c->transform;
            } else {
                width += c->advance;
            }
        } else if (started) {
            out[n].width = width;
            out[n++].ts = ts;
            started = false;
        }
    }
    if (started) {
        out[n].width = width;
        out[n++].ts = ts;
    }
    return n;
}

/* ---------------------------------------------------------------- layout_text */

bool svt_layout(sv_conv *c, const svt_text *t, svt_fonts *fonts, svt_budget *bud, svt_lspan **out,
                size_t *nout, sv_rect *bbox_out)
{
    svt_lctx l;
    sv_arena *a = c->a;
    size_t ci;
    size_t char_offset = 0;
    float last_x = 0.0f;
    float last_y = 0.0f;
    sv_bbox bbox = bbox_default();
    svt_lspan *spans = NULL;
    size_t ns = 0;
    size_t cap = 0;
    memset(&l, 0, sizeof l);
    l.c = c;
    l.t = t;
    l.fonts = fonts;
    l.bud = bud;
    fonts->db = c->text_fonts ? svf_db_get() : NULL;
    if (fonts->db == NULL) {
        return false;
    }
    for (ci = 0; ci < t->nchunks; ci++) {
        size_t s;
        for (s = 0; s < t->chunks[ci].nspans; s++) {
            const svf_font *f = t->chunks[ci].spans[s].font;
            if (!svt_fonts_has(fonts, f) && !svt_select_font(a, fonts, f)) {
                c->nomem = true;
                return false;
            }
        }
    }
    for (ci = 0; ci < t->nchunks; ci++) {
        const svt_chunk *ch = &t->chunks[ci];
        bool linear = ch->path == NULL;
        float x = linear ? (ch->has_x ? ch->x : last_x) : 0.0f;
        float y = linear ? (ch->has_y ? ch->y : last_y) : 0.0f;
        svt_cluster *cl;
        size_t n;
        size_t *first;
        size_t *cpi;
        float cx = 0.0f, cy = 0.0f;
        sv_ts text_ts = ts_identity();
        size_t s;
        size_t *lo;
        size_t *hi;
        size_t i;
        const sv_path *bpath = NULL;
        bool have_bbox_cache = false;
        sv_ts cached_ts = ts_identity();
        bool cached_ok = false;
        sv_rect cached_bb = {0, 0, 0, 0};
        size_t nchars = 0;
        for (i = 0; i < ch->len; i++) {
            nchars += ((unsigned char)ch->text[i] & 0xC0) != 0x80 ? 1u : 0u;
        }
        if (!svt_process_chunk(&l, ch, &cl, &n)) {
            if (l.fail) {
                c->nomem = true;
            }
            return false;
        }
        if (n == 0) {
            char_offset += nchars;
            continue;
        }
        apply_writing_mode(t->vertical, cl, n);
        apply_letter_spacing(ch, cl, n);
        apply_word_spacing(ch, cl, n);
        first = cluster_index(a, ch, cl, n);
        cpi = cp_index(a, ch);
        if (first == NULL || cpi == NULL) {
            c->nomem = true;
            return false;
        }
        if (!apply_length_adjust(&l, ch, cl, n, first)) {
            c->nomem = true;
            return false;
        }
        if (linear) {
            positions_horizontal(t, ch, char_offset, cpi, cl, n, &cx, &cy);
        } else if (!positions_path(&l, t, ch, char_offset, cpi, cl, n, &cx, &cy)) {
            if (l.fail) {
                c->nomem = true;
            }
            return false;
        }
        if (t->vertical && linear) {
            text_ts = pre_rotate_at(text_ts, 90.0f, x, y);
        }
        /* each span's clusters lie in [lo, hi) */
        lo = sv_alloc_array(a, ch->nspans, sizeof *lo);
        hi = sv_alloc_array(a, ch->nspans, sizeof *hi);
        if (lo == NULL || hi == NULL) {
            c->nomem = true;
            return false;
        }
        for (s = 0; s < ch->nspans; s++) {
            lo[s] = SIZE_MAX;
            hi[s] = 0;
        }
        for (i = 0; i < n; i++) {
            size_t si;
            if (svt_span_at(ch, cl[i].byte_idx, &si) != NULL) {
                lo[si] = lo[si] < i ? lo[si] : i;
                hi[si] = hi[si] > i + 1 ? hi[si] : i + 1;
            }
        }
        /* the metrics boxes of every visible cluster (convert_span), before the span transform */
        {
            sv_pb pb;
            pb_init(&pb, a);
            for (i = 0; i < n; i++) {
                sv_rect r;
                sv_rect tr;
                float adv = cl[i].advance;
                if (!cl[i].visible) {
                    continue;
                }
                if (adv <= 0.0f) {
                    adv = 1.0f;
                }
                if (nzrect_from_xywh(0.0f, -cl[i].ascent, adv, cl[i].ascent - cl[i].descent, &r) &&
                    nzrect_transform(r, svt_cluster_ts(&cl[i]), &tr)) {
                    pb_move_to(&pb, tr.l, tr.t);
                    pb_line_to(&pb, tr.r, tr.t);
                    pb_line_to(&pb, tr.r, tr.b);
                    pb_line_to(&pb, tr.l, tr.b);
                    pb_close(&pb);
                }
            }
            bpath = pb_finish(&pb);
            if (bpath == NULL && pb.failed) {
                c->nomem = true;
                return false;
            }
            have_bbox_cache = bpath != NULL;
        }
        for (s = 0; s < ch->nspans; s++) {
            const svt_span *sp = &ch->spans[s];
            const svt_rfont *f = svt_fonts_get(fonts, sp->font);
            sv_ts span_ts;
            svt_lspan ls;
            dspan *ds = NULL;
            size_t nds = 0;
            size_t slo = lo[s] == SIZE_MAX ? 0 : lo[s];
            size_t shi = lo[s] == SIZE_MAX ? 0 : hi[s];
            sv_rect sbb = {0.0f, 0.0f, 0.0f, 0.0f};
            bool has_sbb = false;
            if (f == NULL) {
                continue;
            }
            if (sp->under.on || sp->over.on || sp->through.on) {
                ds = sv_alloc_array(a, shi - slo + 1, sizeof *ds);
                if (ds == NULL) {
                    c->nomem = true;
                    return false;
                }
                nds = collect_deco(cl, slo, shi, sp, ds);
            }
            span_ts = ts_pre_translate(text_ts, x, y);
            if (linear) {
                span_ts = ts_pre_translate(span_ts, 0.0f, resolve_baseline(sp, f, t->vertical));
            }
            memset(&ls, 0, sizeof ls);
            {
                float sc = svt_scale(f, sp->font_size);
                float asc = (float)f->ascent * sc;
                float desc = (float)f->descent * sc;
                if (sp->under.on) {
                    float off = !t->vertical ? -((float)f->underline_pos * sc) : (asc - desc) / 2.0f;
                    ls.underline = convert_decoration(&l, off, sp, f, &sp->under, ds, nds, span_ts);
                    if (ls.underline != NULL) {
                        bbox = bbox_expand(bbox, ls.underline->data->bounds);
                    }
                }
                if (sp->over.on) {
                    float off = !t->vertical ? -asc : -(asc - desc) / 2.0f;
                    ls.overline = convert_decoration(&l, off, sp, f, &sp->over, ds, nds, span_ts);
                    if (ls.overline != NULL) {
                        bbox = bbox_expand(bbox, ls.overline->data->bounds);
                    }
                }
                if (sp->through.on) {
                    float off = !t->vertical ? -((float)f->line_through_pos * sc) : 0.0f;
                    ls.line_through = convert_decoration(&l, off, sp, f, &sp->through, ds, nds, span_ts);
                    if (ls.line_through != NULL) {
                        bbox = bbox_expand(bbox, ls.line_through->data->bounds);
                    }
                }
                if (l.fail || c->nomem) {
                    c->nomem = true;
                    return false;
                }
            }
            if (sp->fill != NULL) {
                sv_fill *fl = sv_alloc(a, sizeof *fl);
                if (fl == NULL) {
                    c->nomem = true;
                    return false;
                }
                *fl = *sp->fill;
                fl->evenodd = false;
                ls.fill = fl;
            }
            ls.stroke = sp->stroke;
            ls.stroke_first = sp->stroke_first;
            ls.visible = sp->visible;
            /* convert_span */
            if (have_bbox_cache) {
                if (cached_ok && memcmp(&cached_ts, &span_ts, sizeof span_ts) == 0) {
                    sbb = cached_bb;
                    has_sbb = true;
                } else {
                    const sv_path *tp = path_transform(a, bpath, span_ts);
                    sv_rect tb;
                    if (tp != NULL && path_tight_bounds(tp, &tb) && rect_to_nz(tb, &sbb)) {
                        has_sbb = true;
                    }
                    cached_ts = span_ts;
                    cached_ok = has_sbb;
                    cached_bb = sbb;
                    if (!has_sbb) {
                        cached_ok = false;
                    }
                }
            }
            if (!has_sbb) {
                continue;
            }
            bbox = bbox_expand(bbox, sbb);
            {
                size_t total = 0;
                size_t k = 0;
                for (i = slo; i < shi; i++) {
                    if (cl[i].visible && svt_span_contains(sp, cl[i].byte_idx)) {
                        total += cl[i].nglyphs;
                    }
                }
                ls.glyphs = sv_alloc_array(a, total + 1, sizeof *ls.glyphs);
                if (ls.glyphs == NULL) {
                    c->nomem = true;
                    return false;
                }
                for (i = slo; i < shi; i++) {
                    if (cl[i].visible && svt_span_contains(sp, cl[i].byte_idx)) {
                        sv_ts cts = svt_cluster_ts(&cl[i]);
                        size_t g;
                        for (g = 0; g < cl[i].nglyphs; g++) {
                            ls.glyphs[k] = cl[i].glyphs[g];
                            ls.glyphs[k].cluster_ts = cts;
                            ls.glyphs[k].span_ts = span_ts;
                            k++;
                        }
                    }
                }
                ls.nglyphs = k;
            }
            {
                void *v = spans;
                if (!sv_grow(a, &v, &cap, ns + 1, sizeof *spans)) {
                    c->nomem = true;
                    return false;
                }
                spans = v;
                spans[ns++] = ls;
            }
        }
        char_offset += nchars;
        if (t->vertical && linear) {
            float tmp = cx;
            cx = cy;
            cy = tmp;
        }
        last_x = x + cx;
        last_y = y + cy;
    }
    if (!bbox_to_nz(bbox, bbox_out)) {
        return false;
    }
    *out = spans;
    *nout = ns;
    return true;
}
