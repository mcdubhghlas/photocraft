/* The layouter's line pass (port of the line loop of crates/text/src/layout.rs Layouter::layout):
 * leading and baselines, kerning slots (manual, optical, tabs), placement, decorations, vertical
 * type and justification. See layout_internal.h. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"
#include "base/utf8.h"
#include "text/fmetrics.h"
#include "text/layout_internal.h"
#include "text/unicode.h"

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
    float advance; /* along the line (px, horizontal scale included) */
    bool tab;      /* a tab, shaped as a space; it advances to the next tab stop */
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
float pc_text_next_tab_stop(float pos, float interval)
{
    return interval > 0.0f && isfinite(pos) ? floorf((pos + 1e-3f) / interval) * interval + interval : pos;
}

static bool is_tab(const size_t *tabs, size_t n, size_t at)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (tabs[mid] < at) {
            lo = mid + 1;
        } else if (tabs[mid] > at) {
            hi = mid;
        } else {
            return true;
        }
    }
    return false;
}

static float hscale_of(const pc_char_style *s)
{
    return s->horizontal_scale > 0.0f ? s->horizontal_scale : 1.0f;
}

pc_status pc_text_tab_widths(const pl_layout *pl, const pc_text_layout *styles, const size_t *tabs,
                             size_t ntabs, float indent, float first_indent, float interval,
                             pc_text_tab_width *out, pc_error *err)
{
    size_t li, i, q;
    pc_status st = PC_OK;
    for (i = 0; i < ntabs; i++) {
        out[i].advance = interval;
        out[i].scale = 1.0f;
        out[i].width = interval;
    }
    for (li = 0; li < pl->nlines && st == PC_OK; li++) {
        pl_glyph_run *grs = NULL;
        size_t ngr = 0, *seen = NULL, nseen = 0;
        float pos = indent + (li == 0 ? first_indent : 0.0f);
        st = pl_line_glyph_runs(pl, li, &grs, &ngr, err);
        if (st == PC_OK && ngr > 0 && (seen = pc_malloc_array(ngr, sizeof *seen)) == NULL) {
            st = NOMEM(err);
        }
        for (i = 0; st == PC_OK && i < ngr; i++) {
            const pl_line_item *it = &pl->line_items[grs[i].item];
            const pl_run *run = &pl->runs[it->run];
            size_t cs = it->cluster_start,
                   ce = it->cluster_end < pl->nclusters ? it->cluster_end : pl->nclusters;
            size_t n = ce > cs ? ce - cs : 0, k;
            bool rtl = (run->bidi_level & 1) != 0, dup = false;
            float scale;
            uint32_t r = grs[i].style_index < pl->nstyles ? pl->styles[grs[i].style_index].brush_run : 0;
            for (k = 0; k < nseen && !dup; k++) {
                dup = seen[k] == grs[i].index;
            }
            if (dup) {
                continue;
            }
            seen[nseen++] = grs[i].index;
            scale = r < styles->nstyles ? hscale_of(&styles->styles[r]) : 1.0f;
            for (q = 0; q < n; q++) {
                const pl_cluster *cl = &pl->clusters[rtl ? ce - 1 - q : cs + q];
                size_t start = pl_cluster_start(run, cl), lo = 0, hi = ntabs;
                bool found = false;
                while (lo < hi && !found) {
                    size_t mid = lo + (hi - lo) / 2;
                    if (tabs[mid] < start) {
                        lo = mid + 1;
                    } else if (tabs[mid] > start) {
                        hi = mid;
                    } else {
                        float stop = pc_text_next_tab_stop(pos, interval);
                        out[mid].advance = cl->advance;
                        out[mid].scale = scale;
                        out[mid].width = stop - pos;
                        pos = stop;
                        found = true;
                    }
                }
                if (!found) {
                    pos += cl->advance * scale;
                }
            }
        }
        free(seen);
        free(grs);
    }
    return st;
}

pc_status pc_text_lay_line(line_ctx *c, size_t li, pc_error *err)
{
    const pl_layout *pl = c->pl;
    const pl_line *line = &pl->lines[li];
    const pl_line_metrics *m = &line->metrics;
    pc_text_layout *out = c->ov->l;
    const pc_paragraph_style *ps = c->ps;
    float k = c->pc->k, leading = 0.0f, col_half = 0.0f, ascent, descent, baseline, adv, line_kern = -0.0f;
    float kern_align = 0.0f, dx, extra = 0.0f, trailing_tabs = 0.0f;
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
    /* Photoshop keeps a horizontal line whose baseline is inside the box and lets its descenders
     * hang below the bottom edge (#1317). */
    if (c->is_box &&
        (c->vertical ? baseline + descent > -c->box[0] + 0.5f : baseline > c->box[1] + c->box[3] + 0.5f)) {
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
            ks.advance = cl->advance * hscale_of(brush_style(out, pl, grs[i].style_index));
            ks.tab = is_tab(c->tabs, c->ntabs, ks.start);
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
    /* Tabs: the "kerning" after a tab takes the pen to the next stop, measured from the anchor or
     * the box edge as if the line were left-aligned (alignment then moves the whole line). */
    {
        float pos = c->indent_start + (li == 0 ? c->first_indent : 0.0f);
        for (j = 0; j < nslots; j++) {
            if (slots[j].tab) {
                size_t q;
                bool rest_blank = true;
                kern_px[j] = pc_text_next_tab_stop(pos, c->tab_px) - pos - slots[j].advance;
                /* Trailing whitespace is outside the line's extent (adv). */
                for (q = j + 1; q < nslots && rest_blank; q++) {
                    rest_blank = slots[q].blank;
                }
                if (rest_blank) {
                    trailing_tabs += kern_px[j];
                }
            }
            pos += slots[j].advance + kern_px[j];
        }
    }
    for (j = 0; j < nslots; j++) {
        line_kern += kern_px[j];
    }
    line_kern -= trailing_tabs;
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
        pc_line_info info = {ra,
                             rb < c->content_end ? rb : c->content_end,
                             baseline,
                             x0,
                             x0 + adv + extra - trailing_tabs,
                             ascent,
                             descent,
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
