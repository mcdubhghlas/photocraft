/* The layouter (port of crates/text/src/layout.rs Layouter::layout and its helpers). */
#include "text/layout.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/rmath.h"
#include "base/utf8.h"
#include "text/cjk.h"
#include "text/craft_fonts.h"
#include "text/fmetrics.h"
#include "text/optical.h"
#include "text/playout.h"
#include "text/unicode.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

/* Photoshop synthesizes small caps for faces without `smcp` at this scale. */
#define SYNTHETIC_SMALL_CAPS_SCALE 0.7f

struct pc_text_layouter {
    pl_builder *b;
    pc_optical_cache *optical;
};

pc_status pc_text_layouter_new(pc_text_layouter **out, pc_error *err)
{
    pc_text_layouter *lo = calloc(1, sizeof *lo);
    pc_status st;
    if (lo == NULL) {
        return NOMEM(err);
    }
    st = pl_builder_new(&lo->b, err);
    if (st == PC_OK) {
        st = pc_optical_cache_new(&lo->optical, err);
    }
    if (st != PC_OK) {
        pc_text_layouter_free(lo);
        return st;
    }
    *out = lo;
    return PC_OK;
}

void pc_text_layouter_free(pc_text_layouter *lo)
{
    if (lo == NULL) {
        return;
    }
    pl_builder_free(lo->b);
    pc_optical_cache_free(lo->optical);
    free(lo);
}

/* ------------------------------------------------------------------------------------------ */
/* Small pure helpers */

/* ------------------------------------------------------------------------------------------ */
/* Style properties */

static uint32_t tag4(const char *s)
{
    return PC_SFNT_TAG(s[0], s[1], s[2], s[3]);
}

/* A tag the CSS settings parser accepts inside double quotes. */
static bool tag_parses(const pc_str *t)
{
    size_t i;
    if (t->len != 4) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)t->ptr[i];
        if (c == '"' || c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool tag_is_ascii4(const pc_str *t)
{
    size_t i;
    if (t->len != 4) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        if ((unsigned char)t->ptr[i] >= 0x80) {
            return false;
        }
    }
    return true;
}

size_t pc_text_style_features(const pc_char_style *st, pc_small_caps_mode mode, pc_text_feature *out,
                              size_t cap)
{
    size_t n = 0, i;
#define ADD(t, v)                                                                                            \
    do {                                                                                                     \
        if (n < cap) {                                                                                       \
            out[n] = (pc_text_feature){(t), (uint16_t)(v)};                                                  \
        }                                                                                                    \
        n++;                                                                                                 \
    } while (0)
    if (st->kerning != PC_KERNING_METRICS || st->kern != 0.0f) {
        ADD(tag4("kern"), 0);
    }
    if (!st->ligatures) {
        ADD(tag4("liga"), 0);
        ADD(tag4("clig"), 0);
    }
    if (st->discretionary_ligatures) {
        ADD(tag4("dlig"), 1);
    }
    if (mode == PC_SMALL_CAPS_OPENTYPE) {
        ADD(tag4("smcp"), 1);
    }
    for (i = 0; i < st->nfeatures; i++) {
        const pc_font_feature *f = &st->features[i];
        if (!tag_is_ascii4(&f->tag)) {
            continue;
        }
        if (!tag_parses(&f->tag)) {
            break; /* the CSS list stops parsing at the first bad entry */
        }
        ADD(tag4(f->tag.ptr), f->value);
    }
#undef ADD
    return n;
}

/* Everything one style_props call pushes, resolved for the builder. */
typedef struct style_props {
    size_t families, features, variations;
    float size, weight, letter_spacing;
    bool italic, underline, strikethrough, has_locale;
    pl_lang locale;
} style_props;

static pc_status features_id(pl_builder *b, const pc_char_style *st, pc_small_caps_mode mode, bool vert,
                             size_t *id, pc_error *err)
{
    size_t n = pc_text_style_features(st, mode, NULL, 0), k;
    pl_feature *f = pc_malloc_array(n + 1, sizeof *f);
    pc_text_feature *t = pc_malloc_array(n + 1, sizeof *t);
    pc_status r;
    if (f == NULL || t == NULL) {
        free(f);
        free(t);
        return NOMEM(err);
    }
    n = pc_text_style_features(st, mode, t, n);
    for (k = 0; k < n; k++) {
        f[k] = (pl_feature){t[k].tag, t[k].value};
    }
    /* "vert" follows the style's list; after a bad entry the parser stops before it too. */
    if (vert) {
        bool stopped = false;
        for (k = 0; k < st->nfeatures; k++) {
            if (tag_is_ascii4(&st->features[k].tag) && !tag_parses(&st->features[k].tag)) {
                stopped = true;
            }
        }
        if (!stopped) {
            f[n++] = (pl_feature){tag4("vert"), 1};
        }
    }
    free(t);
    r = pl_builder_features(b, f, n, id, err);
    free(f);
    return r;
}

static pc_status resolve_props(pl_builder *b, pc_fontdb *db, const pc_char_style *st, float k,
                               const char *const *fallback, size_t nfallback, pc_small_caps_mode mode,
                               style_props *out, pc_error *err)
{
    const pc_fontdb_family **fams = NULL;
    size_t nf = 0, i;
    pc_status r;
    pl_variation *vars = NULL;
    size_t nv = 0;
    const char *sans;
    memset(out, 0, sizeof *out);
    fams = pc_malloc_array(nfallback + 3, sizeof *fams);
    vars = pc_malloc_array(st->nvariations + 1, sizeof *vars);
    if (fams == NULL || vars == NULL) {
        free(fams);
        free(vars);
        return NOMEM(err);
    }
    /* quote() drops '\' and '"'; the name then matches as a family (unknown ones are skipped). */
#define ADD_FAMILY(name, len)                                                                                \
    do {                                                                                                     \
        char *q_ = malloc((len) + 1);                                                                        \
        size_t a_, z_ = 0;                                                                                   \
        const pc_fontdb_family *f_;                                                                          \
        if (q_ == NULL) {                                                                                    \
            free(fams);                                                                                      \
            free(vars);                                                                                      \
            return NOMEM(err);                                                                               \
        }                                                                                                    \
        for (a_ = 0; a_ < (len); a_++) {                                                                     \
            if ((name)[a_] != '\\' && (name)[a_] != '"') {                                                   \
                q_[z_++] = (name)[a_];                                                                       \
            }                                                                                                \
        }                                                                                                    \
        f_ = pc_fontdb_find_family(db, q_, z_);                                                              \
        free(q_);                                                                                            \
        if (f_ != NULL) {                                                                                    \
            fams[nf++] = f_;                                                                                 \
        }                                                                                                    \
    } while (0)
    if (st->font_family.len > 0) {
        ADD_FAMILY(st->font_family.ptr, st->font_family.len);
    }
    for (i = 0; i < nfallback; i++) {
        ADD_FAMILY(fallback[i], strlen(fallback[i]));
    }
#undef ADD_FAMILY
    sans = pc_fontdb_generic(db, "sans-serif");
    if (sans != NULL) {
        const pc_fontdb_family *f = pc_fontdb_find_family(db, sans, strlen(sans));
        if (f != NULL) {
            fams[nf++] = f;
        }
    }
    r = pl_builder_families(b, fams, nf, &out->families, err);
    free(fams);
    if (r == PC_OK) {
        r = features_id(b, st, mode, false, &out->features, err);
    }
    if (r == PC_OK) {
        for (i = 0; i < st->nvariations; i++) {
            const pc_font_variation *v = &st->variations[i];
            if (!tag_is_ascii4(&v->axis)) {
                continue;
            }
            if (!tag_parses(&v->axis)) {
                break;
            }
            vars[nv++] = (pl_variation){tag4(v->axis.ptr), v->value};
        }
        r = pl_builder_variations(b, vars, nv, &out->variations, err);
    }
    free(vars);
    if (r != PC_OK) {
        return r;
    }
    out->size = pc_rs_maxf_k(st->size_pt * k, 0.01f);
    out->weight = (float)(st->weight < 1 ? 1 : st->weight > 1000 ? 1000 : st->weight);
    out->italic = st->italic;
    out->letter_spacing = st->tracking / 1000.0f * out->size;
    out->underline = st->underline;
    out->strikethrough = st->strikethrough;
    out->has_locale = st->has_language && pl_lang_parse(st->language.ptr, st->language.len, &out->locale);
    return PC_OK;
}

static pc_status push_props(pl_builder *b, const style_props *p, uint32_t idx, bool dflt, size_t start,
                            size_t end, pc_error *err)
{
    pl_prop v[11];
    size_t i;
    pc_status r = PC_OK;
    memset(v, 0, sizeof v);
    v[0].kind = PL_PROP_FAMILIES;
    v[0].id = p->families;
    v[1].kind = PL_PROP_SIZE;
    v[1].f = p->size;
    v[2].kind = PL_PROP_WEIGHT;
    v[2].f = p->weight;
    v[3].kind = PL_PROP_ITALIC;
    v[3].b = p->italic;
    v[4].kind = PL_PROP_FEATURES;
    v[4].id = p->features;
    v[5].kind = PL_PROP_VARIATIONS;
    v[5].id = p->variations;
    v[6].kind = PL_PROP_LETTER_SPACING;
    v[6].f = p->letter_spacing;
    v[7].kind = PL_PROP_UNDERLINE;
    v[7].b = p->underline;
    v[8].kind = PL_PROP_STRIKETHROUGH;
    v[8].b = p->strikethrough;
    v[9].kind = PL_PROP_BRUSH;
    v[9].brush_run = idx;
    v[10].kind = PL_PROP_LOCALE;
    v[10].b = p->has_locale;
    v[10].lang = p->locale;
    for (i = 0; i < 11 && r == PC_OK; i++) {
        v[i].start = start;
        v[i].end = end;
        r = dflt ? pl_builder_push_default(b, &v[i], err) : pl_builder_push(b, &v[i], err);
    }
    return r;
}

/* ------------------------------------------------------------------------------------------ */
/* Vertical metrics (layout.rs VMetrics) */

typedef struct vmetrics {
    pc_font *font;
    const int16_t *coords;
    size_t ncoords;
    float scale;     /* px per font unit */
    float em_origin; /* em-box fallback origin (px below the cell top) */
} vmetrics;

static void vmetrics_init(vmetrics *v, pc_font *f, const int16_t *coords, size_t n, float size)
{
    pl_metrics m;
    float upem, asc, desc, r;
    pl_font_metrics(f, NULL, coords, n, &m);
    upem = pc_rs_maxf_k((float)m.units_per_em, 1.0f);
    asc = m.ascent;
    desc = -m.descent;
    if (asc + desc > 0.0f && isfinite(asc) && isfinite(desc)) {
        r = asc / (asc + desc);
        r = r < 0.0f ? 0.0f : r > 1.0f ? 1.0f : r;
    } else {
        r = 0.88f;
    }
    v->font = f;
    v->coords = coords;
    v->ncoords = n;
    v->scale = size / upem;
    v->em_origin = size * r;
}

static float vmetrics_origin(const vmetrics *v, uint32_t gid)
{
    float u;
    if (pl_vertical_origin_units(v->font, v->coords, v->ncoords, gid, &u)) {
        float px = u * v->scale;
        if (isfinite(px)) {
            return px;
        }
    }
    return v->em_origin;
}

/* Height of the lowercase ascender ('d') of the tallest run on the line. */
static bool first_ascent(const pl_layout *pl, const pl_line *line, float *out)
{
    bool have = false;
    float best = 0.0f;
    size_t k;
    for (k = line->item_start; k < line->item_end; k++) {
        const pl_run *run = &pl->runs[pl->line_items[k].run];
        pc_font *f = pl->fonts[run->font_index];
        const int16_t *coords = &pl->coords[run->coords_start];
        size_t n = run->coords_end - run->coords_start;
        uint32_t g = pl_skrifa_cmap(f, 'd');
        float h;
        float size = run->font_size;
        if (g == 0 || !pl_glyph_bounds_ymax(f, size, coords, n, g, &h)) {
            pl_metrics m;
            pl_font_metrics(f, &size, coords, n, &m);
            h = m.has_cap_height ? m.cap_height : m.ascent * 0.75f;
        }
        best = have ? pc_rs_maxf(best, h) : h;
        have = true;
    }
    if (have) {
        *out = best;
    }
    return have;
}

/* ------------------------------------------------------------------------------------------ */
/* Output arrays */

typedef struct outv {
    pc_text_layout *l;
    size_t cap_glyphs, cap_decos, cap_lines, cap_clusters;
} outv;

#define OUT_PUSH(arr, n, cap, v)                                                                             \
    do {                                                                                                     \
        if ((n) >= (cap)) {                                                                                  \
            void *g_ = pc_grow_array((arr), &(cap), (n) + 1, sizeof *(arr));                                 \
            if (g_ == NULL) {                                                                                \
                st = NOMEM(err);                                                                             \
                goto cleanup;                                                                                \
            }                                                                                                \
            (arr) = g_;                                                                                      \
        }                                                                                                    \
        (arr)[(n)++] = (v);                                                                                  \
    } while (0)

/* A cluster of a line in visual order, for kerning. */
typedef struct kern_slot {
    size_t run; /* the run's index on the line */
    size_t start;
    bool has_first;
    uint32_t first, last;
    pc_font *font;
    const int16_t *coords;
    size_t ncoords;
    float size;
    bool rtl, blank, upright;
} kern_slot;

typedef struct cursor {
    size_t run;
    size_t *g2s; /* glyph -> slot */
    size_t n;
    size_t emitted;
    size_t next_slot;
} cursor;

typedef struct vglyph {
    bool upright;
    float advance, origin, centre;
} vglyph;

static bool coords_eq(const kern_slot *a, const kern_slot *b)
{
    return a->ncoords == b->ncoords && (a->ncoords == 0 || memcmp(a->coords, b->coords, a->ncoords * 2) == 0);
}

typedef struct para_ctx {
    const pc_text_layer *t;
    float k;
    const pc_text_run *runs;
    size_t nruns;
    const size_t *run_starts;
    pc_small_caps_mode *small_caps;
} para_ctx;

static size_t style_at(const para_ctx *c, size_t off)
{
    size_t lo = 0, hi = c->nruns;
    /* rposition(start <= off), else 0 */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (c->run_starts[mid] <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo > 0 ? lo - 1 : 0;
}

typedef struct line_ctx {
    pc_text_layouter *lo;
    const para_ctx *pc;
    const pc_paragraph_style *ps;
    const pl_layout *pl;
    size_t pi;
    const char *text; /* the layer text */
    size_t tlen;
    const char *ptext; /* the paragraph as shaped (direction mark, case mapped, breaks) */
    size_t plen;
    size_t pstart, content_end, prefix_len;
    float first_px;
    bool is_box;
    const float *box;
    float line_origin, avail, indent_start;
    bool vertical;
    bool *has_prev;
    float *prev_baseline, *pending_space;
    bool *stop;
    outv *ov;
} line_ctx;

static size_t map_off(const line_ctx *c, size_t o)
{
    size_t v = o > c->prefix_len ? o - c->prefix_len : 0;
    v = c->pstart + v < c->pstart ? SIZE_MAX : c->pstart + v;
    return v < c->content_end ? v : c->content_end;
}

static const pc_char_style *brush_style(const pc_text_layout *out, const pl_layout *pl, uint16_t style_index)
{
    uint32_t r = style_index < pl->nstyles ? pl->styles[style_index].brush_run : 0;
    return r < out->nstyles ? &out->styles[r] : &out->styles[0];
}

/* Stacks, kerns, scales and places one line of a paragraph (the body of Layouter::layout's line
 * loop). */
static pc_status lay_line(line_ctx *c, size_t li, pc_error *err)
{
    const pl_layout *pl = c->pl;
    const pl_line *line = &pl->lines[li];
    const pl_line_metrics *m = &line->metrics;
    pc_text_layout *out = c->ov->l;
    const pc_paragraph_style *ps = c->ps;
    float k = c->pc->k, leading = 0.0f, col_half = 0.0f, ascent, descent, baseline, adv, line_kern = -0.0f;
    float kern_align = 0.0f, dx, extra = 0.0f;
    pl_glyph_run *grs = NULL;
    size_t ngr = 0, i, j, nitems = line->item_end - line->item_start;
    pl_item_glyphs *ig = NULL;
    kern_slot *slots = NULL;
    size_t nslots = 0, cap_slots = 0;
    float *kern_px = NULL;
    cursor *cursors = NULL;
    size_t ncursors = 0;
    bool *seen = NULL, last_line = li + 1 == pl->nlines, justify_all;
    vglyph *vinfo = NULL;
    size_t nvinfo = 0, cap_vinfo = 0, line_index = out->nlines, g0 = out->nglyphs, c0 = out->nclusters;
    uint32_t *plain = NULL;
    size_t nplain = 0, cap_plain = 0;
    pc_status st = PC_OK;
    st = pl_line_glyph_runs(pl, li, &grs, &ngr, err);
    if (st != PC_OK) {
        return st;
    }
    ig = calloc(nitems > 0 ? nitems : 1, sizeof *ig);
    seen = calloc(nitems > 0 ? nitems : 1, sizeof *seen);
    if (ig == NULL || seen == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < nitems; i++) {
        st = pl_item_glyphs_get(pl, line->item_start + i, &ig[i], err);
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    /* Largest leading among the glyph runs; the column's half width (vertical type). */
    for (i = 0; i < ngr; i++) {
        const pl_run *run = &pl->runs[pl->line_items[grs[i].item].run];
        const pc_char_style *s = brush_style(out, pl, grs[i].style_index);
        float px = run->font_size;
        leading = pc_rs_maxf(leading, s->has_leading ? s->leading_pt * k : ps->auto_leading * px);
        col_half = pc_rs_maxf(col_half, run->font_size * 0.5f);
    }
    if (leading == 0.0f) {
        const pc_char_style *s = &out->styles[style_at(c->pc, c->pstart)];
        leading = s->has_leading ? s->leading_pt * k : ps->auto_leading * s->size_pt * k;
    }
    if (!(col_half > 0.0f)) {
        col_half = c->first_px * 0.5f;
    }
    if (c->vertical) {
        ascent = col_half;
        descent = col_half;
    } else if (m->ascent > 0.0f || m->descent > 0.0f) {
        ascent = m->ascent;
        descent = m->descent;
    } else {
        ascent = c->first_px * 0.8f;
        descent = c->first_px * 0.2f;
    }
    if (!*c->has_prev) {
        if (c->is_box && c->vertical) {
            baseline = -(c->box[0] + c->box[2]) + col_half;
        } else if (c->is_box) {
            float fa;
            baseline = c->box[1] + (first_ascent(pl, line, &fa) ? fa : ascent);
        } else {
            baseline = 0.0f;
        }
    } else {
        baseline = *c->prev_baseline + leading + *c->pending_space;
    }
    *c->pending_space = 0.0f;
    if (c->is_box && (c->vertical ? baseline + descent > -c->box[0] + 0.5f
                                  : baseline + descent > c->box[1] + c->box[3] + 0.5f)) {
        *c->stop = true;
        goto cleanup;
    }
    *c->has_prev = true;
    *c->prev_baseline = baseline;
    adv = m->advance - m->trailing_whitespace;

    /* Kerning slots: the clusters of each run in visual order. */
    for (i = 0; i < ngr; i++) {
        size_t item = grs[i].item - line->item_start;
        const pl_line_item *it = &pl->line_items[grs[i].item];
        const pl_run *run = &pl->runs[it->run];
        size_t cs = it->cluster_start, ce = it->cluster_end < pl->nclusters ? it->cluster_end : pl->nclusters;
        size_t n = ce > cs ? ce - cs : 0, q;
        bool rtl = (run->bidi_level & 1) != 0;
        if (seen[item]) {
            continue;
        }
        seen[item] = true;
        for (q = 0; q < n; q++) {
            const pl_cluster *cl = &pl->clusters[rtl ? ce - 1 - q : cs + q];
            pl_glyph tmp[256];
            size_t ng = pl_cluster_glyphs(pl, run, cl, tmp, 256);
            kern_slot ks;
            int ws = pl_whitespace(cl->source_char);
            memset(&ks, 0, sizeof ks);
            ks.run = grs[i].index;
            ks.start = pl_cluster_start(run, cl);
            ks.has_first = ng > 0;
            if (ng > 0) {
                ks.first = tmp[0].id;
                ks.last = tmp[(ng < 256 ? ng : 256) - 1].id;
            }
            ks.font = pl->fonts[run->font_index];
            ks.coords = &pl->coords[run->coords_start];
            ks.ncoords = run->coords_end - run->coords_start;
            ks.size = run->font_size;
            ks.rtl = rtl;
            ks.blank =
                ng == 0 || ws == PL_WS_SPACE || ws == PL_WS_NBSP || ks.start + cl->text_len <= c->prefix_len;
            ks.upright = c->vertical && cl->style_index < pl->nstyles &&
                         pl->styles[cl->style_index].brush_class != PC_VCLASS_ROTATE;
            OUT_PUSH(slots, nslots, cap_slots, ks);
        }
    }
    kern_px = calloc(nslots > 0 ? nslots : 1, sizeof *kern_px);
    if (kern_px == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (j = 0; j + 1 < nslots; j++) {
        const kern_slot *a = &slots[j], *b = &slots[j + 1];
        const pc_char_style *s, *nx;
        float units;
        if (a->start < c->prefix_len) {
            continue;
        }
        s = &out->styles[style_at(c->pc, map_off(c, a->start))];
        nx = &out->styles[style_at(c->pc, map_off(c, b->start))];
        units = isfinite(s->kern) ? s->kern : 0.0f;
        if (s->kerning == PC_KERNING_OPTICAL && nx->kerning == PC_KERNING_OPTICAL && s->kern == 0.0f &&
            nx->kern == 0.0f && !a->rtl && !b->rtl && !a->blank && !b->blank && !a->upright && !b->upright &&
            a->font == b->font && coords_eq(a, b) && fabsf(a->size - b->size) < 1e-3f && a->has_first &&
            b->has_first) {
            float kk;
            if (pc_optical_cache_pair(c->lo->optical, a->font, a->coords, a->ncoords, a->last, b->first,
                                      &kk)) {
                units += kk + pc_optical_size_adjust(s->size_pt);
            }
        }
        kern_px[j] = units / 1000.0f * a->size;
    }
    for (j = 0; j < nslots; j++) {
        line_kern += kern_px[j];
    }
    /* Per run: glyph -> slot, glyphs emitted, first slot whose kerning isn't applied yet. */
    cursors = calloc(nitems > 0 ? nitems : 1, sizeof *cursors);
    if (cursors == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    memset(seen, 0, nitems * sizeof *seen);
    for (i = 0; i < ngr; i++) {
        size_t item = grs[i].item - line->item_start, base = 0, q;
        cursor *cu;
        if (seen[item]) {
            continue;
        }
        seen[item] = true;
        for (q = 0; q < nslots; q++) {
            if (slots[q].run == grs[i].index) {
                base = q;
                break;
            }
        }
        cu = &cursors[ncursors++];
        cu->run = grs[i].index;
        cu->g2s = pc_malloc_array(ig[item].n > 0 ? ig[item].n : 1, sizeof *cu->g2s);
        if (cu->g2s == NULL) {
            st = NOMEM(err);
            goto cleanup;
        }
        {
            /* The visual cluster index of each glyph (clusters without glyphs count too). */
            {
                const pl_line_item *it = &pl->line_items[grs[i].item];
                const pl_run *run = &pl->runs[it->run];
                size_t cs = it->cluster_start,
                       ce = it->cluster_end < pl->nclusters ? it->cluster_end : pl->nclusters;
                size_t n = ce > cs ? ce - cs : 0, g = 0, ci;
                bool rtl = (run->bidi_level & 1) != 0;
                for (ci = 0; ci < n; ci++) {
                    size_t idx = rtl ? ce - 1 - ci : cs + ci;
                    while (g < ig[item].n && ig[item].cluster[g] == idx) {
                        cu->g2s[g++] = base + ci;
                    }
                }
            }
        }
        cu->n = ig[item].n;
        cu->next_slot = base;
    }
    if (c->is_box) {
        kern_align = ps->align == PC_TEXT_ALIGN_CENTER  ? line_kern / 2.0f
                     : ps->align == PC_TEXT_ALIGN_RIGHT ? line_kern
                                                        : 0.0f;
    }
    if (c->is_box) {
        float base = c->line_origin + c->indent_start - kern_align;
        float slack = c->avail - adv - line_kern, more = 0.0f;
        if (last_line) {
            more = ps->align == PC_TEXT_ALIGN_JUSTIFY_CENTER  ? slack * 0.5f - m->offset
                   : ps->align == PC_TEXT_ALIGN_JUSTIFY_RIGHT ? slack - m->offset
                                                              : 0.0f;
        }
        dx = base + more;
    } else {
        float target = ps->align == PC_TEXT_ALIGN_CENTER || ps->align == PC_TEXT_ALIGN_JUSTIFY_CENTER
                           ? -(adv + line_kern) / 2.0f
                       : ps->align == PC_TEXT_ALIGN_RIGHT || ps->align == PC_TEXT_ALIGN_JUSTIFY_RIGHT
                           ? -(adv + line_kern)
                           : c->indent_start;
        dx = target - m->offset;
    }
    justify_all = c->is_box && last_line && ps->align == PC_TEXT_ALIGN_JUSTIFY_ALL;
    memset(seen, 0, nitems * sizeof *seen);
    for (i = 0; i < ngr; i++) {
        const pl_glyph_run *gr = &grs[i];
        size_t item = gr->item - line->item_start, q;
        const pl_line_item *it = &pl->line_items[gr->item];
        const pl_run *run = &pl->runs[it->run];
        uint32_t si = gr->style_index < pl->nstyles ? pl->styles[gr->style_index].brush_run : 0;
        const pc_char_style *s = si < out->nstyles ? &out->styles[si] : &out->styles[0];
        float hs = s->horizontal_scale > 0.0f ? s->horizontal_scale : 1.0f, run_x0, run_x1, shift, pen;
        uint32_t face;
        pc_font *font = pl->fonts[run->font_index];
        const int16_t *coords = &pl->coords[run->coords_start];
        size_t ncoords = run->coords_end - run->coords_start;
        uint8_t cls = gr->style_index < pl->nstyles ? pl->styles[gr->style_index].brush_class : 0;
        vmetrics vm = {NULL, NULL, 0, 0.0f, 0.0f};
        cursor *cu = NULL;
        const pl_style *gst = gr->style_index < pl->nstyles ? &pl->styles[gr->style_index] : NULL;
        bool rtl = (run->bidi_level & 1) != 0;
        st = pc_text_layout_add_face(out, font, coords, ncoords, run->font_size,
                                     pc_text_synthetic_bold(run->synth.embolden, s->weight, run->attr_weight),
                                     run->synth.skew_deg, &face, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        run_x0 = dx + gr->offset + extra;
        if (!seen[item]) {
            size_t cs = it->cluster_start,
                   ce = it->cluster_end < pl->nclusters ? it->cluster_end : pl->nclusters;
            size_t n = ce > cs ? ce - cs : 0, base = 0;
            float cx = run_x0;
            seen[item] = true;
            for (q = 0; q < nslots; q++) {
                if (slots[q].run == gr->index) {
                    base = q;
                    break;
                }
            }
            for (q = 0; q < n; q++) {
                const pl_cluster *cl = &pl->clusters[rtl ? ce - 1 - q : cs + q];
                float a = cl->advance * hs + (base + q < nslots ? kern_px[base + q] : 0.0f);
                size_t rs = pl_cluster_start(run, cl), re = rs + cl->text_len;
                if (re > c->prefix_len || c->prefix_len == 0) {
                    pc_cluster_info ci = {map_off(c, rs), map_off(c, re), cx, a, line_index, rtl};
                    OUT_PUSH(out->clusters, out->nclusters, c->ov->cap_clusters, ci);
                }
                cx += a;
            }
        }
        if (c->vertical) {
            vmetrics_init(&vm, font, coords, ncoords, run->font_size);
        }
        nplain = 0;
        if (c->vertical && cls == PC_VCLASS_ALTERNATE_OR_ROTATE && it->text_start <= it->text_end &&
            it->text_end <= c->plen) {
            size_t at = it->text_start;
            while (at < it->text_end) {
                uint32_t ch, g;
                if (!pc_utf8_next((const uint8_t *)c->ptext, it->text_end, &at, &ch)) {
                    nplain = 0; /* str::get on a bad range: "" */
                    break;
                }
                g = pl_skrifa_cmap(font, ch);
                if (g != 0) {
                    OUT_PUSH(plain, nplain, cap_plain, g);
                }
            }
        }
        for (q = 0; q < ncursors; q++) {
            if (cursors[q].run == gr->index) {
                cu = &cursors[q];
                break;
            }
        }
        pen = gr->offset;
        for (q = 0; q < gr->glyph_count && gr->glyph_start + q < ig[item].n; q++) {
            const pl_glyph *g = &ig[item].glyphs[gr->glyph_start + q];
            pc_placed_glyph pg;
            if (cu != NULL) {
                if (cu->emitted < cu->n) {
                    size_t slot = cu->g2s[cu->emitted];
                    while (cu->next_slot < slot) {
                        extra += cu->next_slot < nslots ? kern_px[cu->next_slot] : 0.0f;
                        cu->next_slot++;
                    }
                }
                cu->emitted++;
            }
            pg.face = face;
            pg.id = g->id;
            pg.x = dx + pen + g->x + extra;
            pg.y = baseline + g->y;
            pg.style = si;
            pg.orient = PC_GLYPH_HORIZONTAL;
            OUT_PUSH(out->glyphs, out->nglyphs, c->ov->cap_glyphs, pg);
            if (c->vertical) {
                vglyph v;
                size_t pq;
                bool in_plain = false;
                for (pq = 0; pq < nplain; pq++) {
                    in_plain = in_plain || plain[pq] == g->id;
                }
                v.upright = cls == PC_VCLASS_UPRIGHT || (cls == PC_VCLASS_ALTERNATE_OR_ROTATE && !in_plain);
                v.advance = g->advance * hs;
                v.origin = v.upright ? vmetrics_origin(&vm, g->id) : 0.0f;
                v.centre = (run->metrics.ascent - run->metrics.descent) * 0.5f;
                OUT_PUSH(vinfo, nvinfo, cap_vinfo, v);
            }
            extra += g->advance * (hs - 1.0f);
            pen += g->advance;
        }
        if (cu != NULL && cu->emitted >= cu->n) {
            size_t end = cu->next_slot;
            for (q = nslots; q > 0; q--) {
                if (slots[q - 1].run == cu->run) {
                    end = q;
                    break;
                }
            }
            while (cu->next_slot < end) {
                extra += cu->next_slot < nslots ? kern_px[cu->next_slot] : 0.0f;
                cu->next_slot++;
            }
        }
        run_x1 = dx + gr->offset + gr->advance + extra;
        shift = s->baseline_shift_pt * k;
        if (c->vertical) {
            /* Underline to the right of the column, strikethrough through its centre (stored
             * directly in text space). */
            float half = run->font_size * 0.5f;
            if (gst != NULL && gst->underline) {
                float v1 = baseline - half, v0 = v1 - pc_rs_maxf_k(run->metrics.underline_size, 1.0f);
                pc_decoration_rect d = {-v1, run_x0, -v0, run_x1, si};
                OUT_PUSH(out->decorations, out->ndecorations, c->ov->cap_decos, d);
            }
            if (gst != NULL && gst->strikethrough) {
                float sz = pc_rs_maxf_k(run->metrics.strikethrough_size, 1.0f);
                float v0 = baseline - sz * 0.5f, v1 = baseline + sz * 0.5f;
                pc_decoration_rect d = {-v1, run_x0, -v0, run_x1, si};
                OUT_PUSH(out->decorations, out->ndecorations, c->ov->cap_decos, d);
            }
        } else {
            if (gst != NULL && gst->underline) {
                float y0 = baseline - run->metrics.underline_offset - shift;
                pc_decoration_rect d = {run_x0, y0, run_x1,
                                        y0 + pc_rs_maxf_k(run->metrics.underline_size, 1.0f), si};
                OUT_PUSH(out->decorations, out->ndecorations, c->ov->cap_decos, d);
            }
            if (gst != NULL && gst->strikethrough) {
                float y0 = baseline - run->metrics.strikethrough_offset - shift;
                pc_decoration_rect d = {run_x0, y0, run_x1,
                                        y0 + pc_rs_maxf_k(run->metrics.strikethrough_size, 1.0f), si};
                OUT_PUSH(out->decorations, out->ndecorations, c->ov->cap_decos, d);
            }
        }
    }
    if (c->vertical) {
        extra -= pc_text_squeeze_punctuation(c->text, c->tlen, &out->clusters[c0], out->nclusters - c0,
                                             &out->glyphs[g0], out->nglyphs - g0);
    }
    if (justify_all) {
        float slack = c->avail - (adv + extra);
        size_t n = out->nclusters - c0;
        if (n > 1 && slack > 0.0f) {
            float step = slack / (float)(n - 1);
            float *starts = pc_malloc_array(n, sizeof *starts);
            if (starts == NULL) {
                st = NOMEM(err);
                goto cleanup;
            }
            for (i = 0; i < n; i++) {
                starts[i] = out->clusters[c0 + i].x;
                out->clusters[c0 + i].x += step * (float)i;
            }
            for (i = g0; i < out->nglyphs; i++) {
                size_t at = 0, q;
                for (q = n; q > 0; q--) {
                    if (starts[q - 1] <= out->glyphs[i].x + 1e-3f) {
                        at = q - 1;
                        break;
                    }
                }
                out->glyphs[i].x += step * (float)at;
            }
            free(starts);
            extra += slack;
        }
    }
    if (c->vertical) {
        /* Line space -> text space (90 degrees clockwise); upright glyphs centred on the column. */
        for (i = 0; i < nvinfo && g0 + i < out->nglyphs; i++) {
            pc_placed_glyph *g = &out->glyphs[g0 + i];
            const vglyph *v = &vinfo[i];
            float u = g->x, cross = g->y;
            if (v->upright) {
                g->x = -baseline - v->advance * 0.5f;
                g->y = u + v->origin;
                g->orient = PC_GLYPH_UPRIGHT;
            } else {
                g->x = -(cross + v->centre);
                g->y = u;
                g->orient = PC_GLYPH_ROTATED;
            }
        }
    }
    {
        float x0 = dx + m->offset;
        size_t ra = map_off(c, line->text_start), rb = map_off(c, line->text_end);
        pc_line_info info = {
            ra,   rb < c->content_end ? rb : c->content_end, baseline, x0, x0 + adv + extra, ascent, descent,
            c->pi};
        OUT_PUSH(out->lines, out->nlines, c->ov->cap_lines, info);
    }
cleanup:
    if (ig != NULL) {
        for (i = 0; i < nitems; i++) {
            pl_item_glyphs_fini(&ig[i]);
        }
    }
    if (cursors != NULL) {
        for (i = 0; i < ncursors; i++) {
            free(cursors[i].g2s);
        }
    }
    free(ig);
    free(seen);
    free(grs);
    free(slots);
    free(kern_px);
    free(cursors);
    free(vinfo);
    free(plain);
    return st;
}

/* ------------------------------------------------------------------------------------------ */
/* The layout */

pc_status pc_text_layouter_layout(pc_text_layouter *lo, pc_fontdb *db, const pc_text_layer *t, float dpi,
                                  pc_text_layout *out, pc_error *err)
{
    const char *text = pc_str_c(&t->text);
    size_t tlen = t->text.len;
    float k = dpi > 0.0f ? dpi / 72.0f : 1.0f;
    pc_text_run *runs = NULL;
    size_t nruns = 0, npar_runs = 0, i;
    pc_paragraph_run *pruns = NULL;
    size_t *run_starts = NULL, *para_starts = NULL, *paras = NULL, nparas = 0;
    pc_small_caps_mode *small_caps = NULL;
    const char **fallback = NULL, **serif_fallback = NULL, **craft = NULL;
    size_t nfallback = 0, ncraft = 0;
    bool vertical = t->orientation == PC_ORIENT_VERTICAL, is_box = t->shape.is_box;
    float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool has_prev = false, stop = false;
    float prev_baseline = 0.0f, pending_space = 0.0f;
    pc_buf ptext = PC_BUF_INIT;
    pl_layout pl;
    outv ov;
    para_ctx pc;
    pc_status st = PC_OK;
    size_t pi;
    pc_text_layout_init(out);
    memset(&pl, 0, sizeof pl);
    memset(&ov, 0, sizeof ov);
    ov.l = out;
    out->px_per_pt = k;
    out->vertical = vertical;
    if (tlen > PC_TEXT_MAX_LAYOUT_BYTES) {
        return pc_fail(err, PC_ERR_LIMIT, "text layout: text longer than %zu bytes",
                       PC_TEXT_MAX_LAYOUT_BYTES);
    }
    {
        size_t at = 0;
        uint32_t cp;
        while (at < tlen) {
            if (!pc_utf8_next((const uint8_t *)text, tlen, &at, &cp)) {
                return pc_fail(err, PC_ERR_INVALID, "text layout: text is not valid UTF-8 (byte %zu)", at);
            }
        }
    }
    if (is_box) {
        box[0] = t->shape.x;
        box[1] = t->shape.y;
        box[2] = t->shape.width;
        box[3] = t->shape.height;
    }
    st = pc_text_layer_char_runs(t, &runs, &nruns, err);
    if (st == PC_OK) {
        st = pc_text_layer_paragraph_runs(t, &pruns, &npar_runs, err);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    /* Styles, with PostScript names resolved (an exact face wins; a guess fills a missing
     * family only). */
    out->styles = calloc(nruns > 0 ? nruns : 1, sizeof *out->styles);
    run_starts = pc_malloc_array(nruns + 1, sizeof *run_starts);
    small_caps = pc_malloc_array(nruns + 1, sizeof *small_caps);
    para_starts = pc_malloc_array(npar_runs + 1, sizeof *para_starts);
    if (out->styles == NULL || run_starts == NULL || small_caps == NULL || para_starts == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < nruns; i++) {
        pc_char_style *s = &out->styles[i];
        st = pc_char_style_clone(&runs[i].style, s, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        out->nstyles++;
        if (s->has_postscript_name) {
            pc_resolved_font f;
            memset(&f, 0, sizeof f);
            st = pc_fontdb_resolve_postscript(db, s->postscript_name.ptr, s->postscript_name.len, &f, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            if (f.exact || (!pc_fontdb_has_family(db, s->font_family.ptr, s->font_family.len) &&
                            pc_fontdb_has_family(db, f.family, strlen(f.family)))) {
                st = pc_str_set_c(&s->font_family, f.family, err);
                s->weight = f.weight;
                s->italic = f.italic;
            }
            pc_resolved_font_fini(&f);
            if (st != PC_OK) {
                goto cleanup;
            }
        }
        run_starts[i] = i == 0 ? 0 : run_starts[i - 1] + runs[i - 1].len;
        small_caps[i] = pc_text_small_caps_mode(
            s->caps,
            pc_fontdb_selected_face_has_feature(db, s->font_family.ptr, s->font_family.len, s->weight,
                                                s->italic, PC_SFNT_TAG('s', 'm', 'c', 'p')));
    }
    for (i = 0; i < npar_runs; i++) {
        para_starts[i] = i == 0 ? 0 : para_starts[i - 1] + pruns[i - 1].len;
    }
    /* The fallback families (the default family, then the installed candidates); serif runs
     * put the craft Mincho families first. */
    nfallback = pc_fontdb_fallback_count(db);
    fallback = pc_malloc_array(nfallback + 1, sizeof *fallback);
    serif_fallback = pc_malloc_array(nfallback + 1, sizeof *serif_fallback);
    if (fallback == NULL || serif_fallback == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < nfallback; i++) {
        fallback[i] = pc_fontdb_fallback(db, i);
        serif_fallback[i] = fallback[i];
    }
    {
        size_t ncf;
        const pc_craft_font *cf = pc_craft_fonts(&ncf);
        st = pc_craft_japanese_families(cf, ncf, &craft, &ncraft, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        pc_craft_mincho_first(serif_fallback, nfallback, craft, ncraft);
    }
    pc.t = t;
    pc.k = k;
    pc.runs = runs;
    pc.nruns = nruns;
    pc.run_starts = run_starts;
    pc.small_caps = small_caps;
    st = pc_text_split_paragraphs(text, tlen, &paras, &nparas, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    for (pi = 0; pi < nparas && !stop; pi++) {
        size_t pstart = paras[2 * pi], pend = paras[2 * pi + 1], content_end = pend, prefix_len, si0;
        const pc_paragraph_style *ps;
        float indent_start, indent_end, line_origin, line_len, avail = 0.0f, first_px;
        size_t *sc_ranges = NULL, nsc = 0, cap_sc = 0; /* synthetic small caps: start, end */
        float *sc_sizes = NULL;
        size_t li;
        {
            size_t lo2 = 0, hi2 = npar_runs;
            while (lo2 < hi2) {
                size_t mid = lo2 + (hi2 - lo2) / 2;
                if (para_starts[mid] <= pstart) {
                    lo2 = mid + 1;
                } else {
                    hi2 = mid;
                }
            }
            ps = &pruns[lo2 > 0 ? lo2 - 1 : 0].style;
        }
        while (content_end > pstart && (text[content_end - 1] == '\r' || text[content_end - 1] == '\n')) {
            content_end--;
        }
        pc_buf_truncate(&ptext, 0);
        st = pc_buf_append_str(&ptext,
                               ps->direction == PC_TEXT_DIR_LTR   ? "\xE2\x80\x8E"
                               : ps->direction == PC_TEXT_DIR_RTL ? "\xE2\x80\x8F"
                                                                  : "",
                               err);
        prefix_len = ptext.len;
        {
            size_t at = pstart;
            while (st == PC_OK && at < content_end) {
                size_t b0 = at;
                uint32_t ch;
                size_t si;
                const pc_char_style *s;
                (void)pc_utf8_next((const uint8_t *)text, content_end, &at, &ch);
                if (ch == PC_TEXT_FORCED_LINE_BREAK) {
                    st = pc_buf_append_byte(&ptext, '\n', err);
                    continue;
                }
                si = style_at(&pc, b0);
                s = &out->styles[si];
                if (s->caps == PC_CAPS_ALL_CAPS || small_caps[si] == PC_SMALL_CAPS_SYNTHETIC) {
                    uint32_t up[3];
                    size_t nu = pc_text_to_upper(ch, up), ulen = 0, j;
                    for (j = 0; j < nu; j++) {
                        ulen += up[j] < 0x80 ? 1 : up[j] < 0x800 ? 2 : up[j] < 0x10000 ? 3 : 4;
                    }
                    if (ulen == at - b0) {
                        for (j = 0; j < nu && st == PC_OK; j++) {
                            st = pc_utf8_put(&ptext, up[j], err);
                        }
                        if (st == PC_OK && s->caps == PC_CAPS_SMALL_CAPS && pc_text_is_lowercase(ch)) {
                            if (2 * nsc + 2 > cap_sc) {
                                size_t c2 = cap_sc;
                                void *g = pc_grow_array(sc_ranges, &cap_sc, 2 * nsc + 2, sizeof *sc_ranges);
                                void *h = g != NULL
                                              ? pc_grow_array(sc_sizes, &c2, 2 * nsc + 2, sizeof *sc_sizes)
                                              : NULL;
                                if (g != NULL) {
                                    sc_ranges = g;
                                }
                                if (h == NULL) {
                                    st = NOMEM(err);
                                    break;
                                }
                                sc_sizes = h;
                            }
                            sc_ranges[2 * nsc] = ptext.len - ulen;
                            sc_ranges[2 * nsc + 1] = ptext.len;
                            sc_sizes[nsc] = s->size_pt * k * SYNTHETIC_SMALL_CAPS_SCALE;
                            nsc++;
                        }
                        continue;
                    }
                }
                st = pc_buf_append(&ptext, text + b0, at - b0, err);
            }
        }
        if (st != PC_OK) {
            free(sc_ranges);
            free(sc_sizes);
            goto cleanup;
        }
        si0 = style_at(&pc, pstart);
        first_px = out->styles[si0].size_pt * k;
        /* Build the paragraph's styled text. */
        pl_builder_begin(lo->b, db);
        {
            style_props p;
            const pc_char_style *s0 = &out->styles[si0];
            bool serif = pc_craft_is_serif_family(s0->font_family.ptr, s0->font_family.len);
            st = resolve_props(lo->b, db, s0, k, serif ? serif_fallback : fallback, nfallback,
                               small_caps[si0], &p, err);
            if (st == PC_OK) {
                st = push_props(lo->b, &p, (uint32_t)si0, true, 0, 0, err);
            }
        }
        for (i = style_at(&pc, pstart); st == PC_OK && i < nruns && run_starts[i] < content_end; i++) {
            const pc_char_style *s = &out->styles[i];
            size_t rs = run_starts[i], a = rs > pstart ? rs : pstart;
            size_t z = rs + runs[i].len < content_end ? rs + runs[i].len : content_end;
            size_t ra, rz;
            style_props p;
            bool serif;
            if (a >= z) {
                continue;
            }
            ra = a - pstart + prefix_len;
            rz = z - pstart + prefix_len;
            serif = pc_craft_is_serif_family(s->font_family.ptr, s->font_family.len);
            st = resolve_props(lo->b, db, s, k, serif ? serif_fallback : fallback, nfallback, small_caps[i],
                               &p, err);
            if (st == PC_OK) {
                st = push_props(lo->b, &p, (uint32_t)i, false, ra, rz, err);
            }
            if (st == PC_OK && vertical) {
                /* Upright pieces get the vertical alternates and their class in the brush. */
                const char *piece = (const char *)ptext.data + ra;
                size_t plen = rz - ra, at = 0, from = 0;
                bool has_seg = false;
                pc_vclass cls = PC_VCLASS_ROTATE;
                size_t vid = SIZE_MAX;
                while (st == PC_OK && at <= plen) {
                    size_t b0 = at;
                    uint32_t ch = 0;
                    pc_vclass c2 = PC_VCLASS_ROTATE;
                    bool end = at == plen;
                    if (!end) {
                        (void)pc_utf8_next((const uint8_t *)piece, plen, &at, &ch);
                        c2 = pc_text_vertical_class(ch);
                    }
                    if (has_seg && (end || c2 != cls)) {
                        if (cls != PC_VCLASS_ROTATE && from < b0) {
                            pl_prop fp, bp;
                            if (vid == SIZE_MAX) {
                                st = features_id(lo->b, s, small_caps[i], true, &vid, err);
                            }
                            memset(&fp, 0, sizeof fp);
                            memset(&bp, 0, sizeof bp);
                            fp.kind = PL_PROP_FEATURES;
                            fp.id = vid;
                            fp.start = ra + from;
                            fp.end = ra + b0;
                            bp.kind = PL_PROP_BRUSH;
                            bp.brush_run = (uint32_t)i;
                            bp.brush_class = (uint8_t)cls;
                            bp.start = ra + from;
                            bp.end = ra + b0;
                            if (st == PC_OK) {
                                st = pl_builder_push(lo->b, &fp, err);
                            }
                            if (st == PC_OK) {
                                st = pl_builder_push(lo->b, &bp, err);
                            }
                        }
                        from = b0;
                    }
                    if (end) {
                        break;
                    }
                    if (!has_seg) {
                        from = b0;
                        has_seg = true;
                    }
                    cls = c2;
                }
            }
        }
        for (i = 0; st == PC_OK && i < nsc; i++) {
            pl_prop p;
            memset(&p, 0, sizeof p);
            p.kind = PL_PROP_SIZE;
            p.f = sc_sizes[i];
            p.start = sc_ranges[2 * i];
            p.end = sc_ranges[2 * i + 1];
            st = pl_builder_push(lo->b, &p, err);
        }
        free(sc_ranges);
        free(sc_sizes);
        if (st == PC_OK) {
            pl_layout_fini(&pl);
            st = pl_builder_build(lo->b, (const char *)ptext.data, ptext.len, &pl, err);
        }
        if (st != PC_OK) {
            goto cleanup;
        }
        indent_start = ps->start_indent_pt * k;
        indent_end = ps->end_indent_pt * k;
        pl.indent_amount = ps->first_line_indent_pt != 0.0f ? ps->first_line_indent_pt * k : 0.0f;
        line_origin = vertical ? box[1] : box[0];
        line_len = vertical ? box[3] : box[2];
        if (is_box) {
            avail = pc_rs_maxf_k(line_len - indent_start - indent_end, 1.0f);
        }
        st = pl_break_all_lines(&pl, is_box ? avail : FLT_MAX, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        {
            pl_align a = PL_ALIGN_LEFT;
            if (is_box) {
                a = ps->align == PC_TEXT_ALIGN_LEFT     ? PL_ALIGN_LEFT
                    : ps->align == PC_TEXT_ALIGN_CENTER ? PL_ALIGN_CENTER
                    : ps->align == PC_TEXT_ALIGN_RIGHT  ? PL_ALIGN_RIGHT
                                                        : PL_ALIGN_JUSTIFY;
            }
            pl_align_lines(&pl, a, !is_box);
        }
        if (has_prev) {
            pending_space += ps->space_before_pt * k;
        }
        for (li = 0; li < pl.nlines; li++) {
            line_ctx lc = {lo,
                           &pc,
                           ps,
                           &pl,
                           pi,
                           text,
                           tlen,
                           (const char *)ptext.data,
                           ptext.len,
                           pstart,
                           content_end,
                           prefix_len,
                           first_px,
                           is_box,
                           box,
                           line_origin,
                           avail,
                           indent_start,
                           vertical,
                           &has_prev,
                           &prev_baseline,
                           &pending_space,
                           &stop,
                           &ov};
            st = lay_line(&lc, li, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            if (stop) {
                break;
            }
        }
        pending_space += ps->space_after_pt * k;
    }
cleanup:
    pl_layout_fini(&pl);
    pc_buf_fini(&ptext);
    pc_text_runs_free(runs, nruns);
    free(pruns);
    free(run_starts);
    free(para_starts);
    free(paras);
    free(small_caps);
    free(fallback);
    free(serif_fallback);
    free(craft);
    if (st != PC_OK) {
        pc_text_layout_fini(out);
        pc_text_layout_init(out);
    }
    return st;
}
