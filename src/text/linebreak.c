/* Greedy line breaking, alignment and glyph-run iteration of a paragraph layout: parley 0.11's
 * layout/line_break.rs, alignment.rs, line.rs and cluster.rs, as the reference layouter uses
 * them (no inline boxes, wrapping on, overflow-wrap normal, unquantised metrics). */
#include "text/playout.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"
#include "base/utf8.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

typedef struct line_state {
    float x;
    size_t items_start, items_end;
    size_t clusters_start, clusters_end;
    size_t num_spaces;
    float running_line_height;
    bool max_height_exceeded;
} line_state;

typedef struct prev_boundary {
    size_t item_idx, run_idx, cluster_idx;
    line_state state;
} prev_boundary;

typedef struct breaker {
    pl_layout *l;
    pl_line *lines; /* the lines being built (swapped into the layout at the end) */
    size_t nlines, cap_lines;
    pl_line_item *items;
    size_t nitems, cap_items;
    size_t item_idx, run_idx, cluster_idx;
    float line_x;
    double line_y;
    float layout_max_advance, line_max_advance, line_max_height;
    line_state line;
    bool has_prev;
    prev_boundary prev;
    bool done;
} breaker;

typedef enum yield { Y_NONE, Y_LINE, Y_MAX_HEIGHT } yield;

static const pl_cluster *run_get(const pl_layout *l, const pl_run *run, size_t index)
{
    /* Run::get without line data: clusters.get(cluster_range.start + index) (not limited to the
     * run, as in the reference). */
    size_t at = run->cluster_start + index;
    return at < l->nclusters ? &l->clusters[at] : NULL;
}

static bool is_ligature_continuation(const pl_cluster *c)
{
    return (c->flags & PL_CLUSTER_LIGATURE_COMPONENT) != 0;
}

static void add_line_height(breaker *b, float h)
{
    b->line.running_line_height = pc_rs_maxf(b->line.running_line_height, h);
    b->line.max_height_exceeded = b->line.running_line_height > b->line_max_height;
}

static void append_cluster(breaker *b, float next_x, float h)
{
    b->line.items_end = b->item_idx + 1;
    b->line.clusters_end = b->cluster_idx + 1;
    b->cluster_idx++;
    b->line.x = next_x;
    add_line_height(b, h);
}

static void mark_break(breaker *b)
{
    b->has_prev = true;
    b->prev = (prev_boundary){b->item_idx, b->run_idx, b->cluster_idx, b->line};
}

static size_t cluster_text_start(const pl_run *r, const pl_cluster *c)
{
    return r->text_start + c->text_offset;
}

/* commit_line. */
static pc_status commit_line(breaker *b, float max_advance, pl_break_reason reason, float indent,
                             pc_error *err)
{
    pl_layout *l = b->l;
    line_state *s = &b->line;
    size_t start_item = b->nitems, i, n;
    bool committed = false;
    size_t num_spaces;
    pl_line line;
    if (s->clusters_end > l->nclusters) {
        s->clusters_end = l->nclusters;
    }
    if (s->items_end > l->nitems) {
        s->items_end = l->nitems;
    }
    n = s->items_end > s->items_start ? s->items_end - s->items_start : 0;
    for (i = 0; i < n; i++) {
        size_t ri = l->items[s->items_start + i];
        const pl_run *run = &l->runs[ri];
        size_t cs = run->cluster_start, ce = run->cluster_end;
        pl_line_item it;
        if (i == 0) {
            cs = s->clusters_start;
        }
        if (i + 1 == n) {
            ce = s->clusters_end;
        }
        if (cs >= run->cluster_end) {
            continue;
        }
        committed = true;
        memset(&it, 0, sizeof it);
        it.run = ri;
        it.bidi_level = run->bidi_level;
        it.cluster_start = cs;
        it.cluster_end = ce;
        if (run->cluster_end > run->cluster_start && cs >= run->cluster_start) {
            const pl_cluster *first = run_get(l, run, cs - run->cluster_start);
            size_t li = ce > run->cluster_start ? ce - run->cluster_start : 0;
            const pl_cluster *last = run_get(l, run, li > 0 ? li - 1 : 0);
            if (first != NULL && last != NULL) {
                it.text_start = cluster_text_start(run, first);
                it.text_end = cluster_text_start(run, last) + last->text_len;
            }
        }
        if (b->nitems >= b->cap_items) {
            void *g = pc_grow_array(b->items, &b->cap_items, b->nitems + 1, sizeof *b->items);
            if (g == NULL) {
                return NOMEM(err);
            }
            b->items = g;
        }
        b->items[b->nitems++] = it;
    }
    num_spaces = s->num_spaces;
    if (reason == PL_BREAK_REGULAR && s->clusters_start < s->clusters_end &&
        s->clusters_end <= l->nclusters) {
        int ws = pl_whitespace(l->clusters[s->clusters_end - 1].source_char);
        if ((ws == PL_WS_SPACE || ws == PL_WS_NBSP) && num_spaces > 0) {
            num_spaces--;
        }
    }
    memset(&line, 0, sizeof line);
    line.item_start = start_item;
    line.item_end = b->nitems;
    line.max_advance = max_advance;
    line.break_reason = reason;
    line.num_spaces = num_spaces;
    line.indent = indent;
    line.metrics.advance = s->x;
    if (b->nlines >= b->cap_lines) {
        void *g = pc_grow_array(b->lines, &b->cap_lines, b->nlines + 1, sizeof *b->lines);
        if (g == NULL) {
            return NOMEM(err);
        }
        b->lines = g;
    }
    b->lines[b->nlines++] = line;
    s->num_spaces = 0;
    if (committed) {
        s->clusters_start = s->clusters_end;
    }
    s->items_start = s->items_end > 0 ? s->items_end - 1 : 0;
    return PC_OK;
}

static void reorder_line_items(pl_line_item *runs, size_t n)
{
    uint8_t max_level = 0, lowest_odd = 255;
    size_t i;
    int level;
    for (i = 0; i < n; i++) {
        uint8_t lv = runs[i].bidi_level;
        if (lv > max_level) {
            max_level = lv;
        }
        if ((lv & 1) != 0 && lv < lowest_odd) {
            lowest_odd = lv;
        }
    }
    for (level = max_level; level >= lowest_odd; level--) {
        i = 0;
        while (i < n) {
            if (runs[i].bidi_level >= level) {
                size_t end = i + 1, j, k;
                while (end < n && runs[end].bidi_level >= level) {
                    end++;
                }
                for (j = i, k = end - 1; j < k; j++, k--) {
                    pl_line_item t = runs[j];
                    runs[j] = runs[k];
                    runs[k] = t;
                }
                i = end;
            }
            i++;
        }
    }
}

static float whitespace_advance(const pl_layout *l, size_t a, size_t z, bool forward)
{
    float sum = -0.0f; /* Iterator::sum */
    size_t i;
    for (i = 0; i < z - a; i++) {
        const pl_cluster *c = &l->clusters[forward ? a + i : z - 1 - i];
        if (pl_whitespace(c->source_char) == PL_WS_NONE) {
            break;
        }
        sum += c->advance;
    }
    return sum;
}

/* finish_line. */
static pc_status finish_line(breaker *b, size_t li, float line_height, pc_error *err)
{
    pl_layout *l = b->l;
    pl_line *line = &b->lines[li];
    bool have_metrics = false, needs_reorder = false;
    size_t i, count;
    float ascent, descent;
    line->metrics.ascent = 0.0f;
    line->metrics.descent = 0.0f;
    line->metrics.leading = 0.0f;
    line->metrics.offset = 0.0f;
    line->text_start = SIZE_MAX;
    line->text_end = 0;
    line->metrics.line_height = line_height;
    if (line->item_end <= line->item_start) {
        line->text_start = l->text_len;
        line->text_end = l->text_len;
    }
    for (i = line->item_end; i > line->item_start; i--) {
        pl_line_item *it = &b->items[i - 1];
        const pl_run *run = &l->runs[it->run];
        size_t c, cs = it->cluster_start, ce = it->cluster_end;
        float adv = -0.0f;
        if (ce > l->nclusters) {
            ce = l->nclusters;
        }
        if (ce < cs) {
            ce = cs; /* the reference would panic slicing a reversed range */
        }
        /* compute_whitespace_properties */
        it->is_whitespace = true;
        if ((it->bidi_level & 1) != 0) {
            for (c = cs; c < ce; c++) {
                if (pc_unicode_is_space(l->clusters[c].source_char)) {
                    it->has_trailing_whitespace = true;
                } else {
                    it->is_whitespace = false;
                    break;
                }
            }
        } else {
            for (c = ce; c > cs; c--) {
                if (pc_unicode_is_space(l->clusters[c - 1].source_char)) {
                    it->has_trailing_whitespace = true;
                } else {
                    it->is_whitespace = false;
                    break;
                }
            }
        }
        line->text_end = line->text_end > it->text_end ? line->text_end : it->text_end;
        line->text_start = line->text_start < it->text_start ? line->text_start : it->text_start;
        if (it->bidi_level != 0) {
            needs_reorder = true;
        }
        for (c = cs; c < ce; c++) {
            adv += l->clusters[c].advance;
        }
        it->advance = adv;
        if (!have_metrics && it->is_whitespace) {
            continue;
        }
        line->metrics.ascent = pc_rs_maxf(line->metrics.ascent, run->metrics.ascent);
        line->metrics.descent = pc_rs_maxf(line->metrics.descent, run->metrics.descent);
        have_metrics = true;
    }
    count = line->item_end - line->item_start;
    if (needs_reorder && count > 1) {
        reorder_line_items(&b->items[line->item_start], count);
    }
    line->metrics.trailing_whitespace = 0.0f;
    if (count > 0) {
        const pl_line_item *it =
            (l->base_level & 1) != 0 ? &b->items[line->item_start] : &b->items[line->item_end - 1];
        if (it->has_trailing_whitespace) {
            size_t cs = it->cluster_start,
                   ce = it->cluster_end < l->nclusters ? it->cluster_end : l->nclusters;
            if (ce > cs) {
                line->metrics.trailing_whitespace = whitespace_advance(l, cs, ce, (it->bidi_level & 1) != 0);
            } else {
                line->metrics.trailing_whitespace = -0.0f;
            }
        }
    }
    if (!have_metrics) {
        if (count > 0) {
            const pl_run *run = &l->runs[b->items[line->item_start].run];
            line->metrics.ascent = run->metrics.ascent;
            line->metrics.descent = run->metrics.descent;
        } else if (li > 0) {
            size_t r;
            line->metrics = b->lines[li - 1].metrics;
            for (r = l->nruns; r > 0; r--) {
                const pl_run *run = &l->runs[r - 1];
                if (run->text_end > run->text_start) {
                    pl_line_item it;
                    memset(&it, 0, sizeof it);
                    it.run = r - 1;
                    it.cluster_start = it.cluster_end = run->cluster_end;
                    it.text_start = it.text_end = run->text_end;
                    if (b->nitems >= b->cap_items) {
                        void *g = pc_grow_array(b->items, &b->cap_items, b->nitems + 1, sizeof *b->items);
                        if (g == NULL) {
                            return NOMEM(err);
                        }
                        b->items = g;
                    }
                    line = &b->lines[li];
                    b->items[b->nitems] = it;
                    line->item_start = b->nitems;
                    line->item_end = b->nitems + 1;
                    b->nitems++;
                    break;
                }
            }
        }
    }
    line->metrics.leading = line->metrics.line_height - (line->metrics.ascent + line->metrics.descent);
    ascent = line->metrics.ascent;
    descent = line->metrics.descent;
    {
        float above = line->metrics.leading * 0.5f, below = line->metrics.leading * 0.5f;
        line->metrics.baseline = ascent + above + (float)b->line_y;
        line->metrics.block_min_coord = line->metrics.baseline - ascent - pc_rs_maxf_k(above, 0.0f);
        line->metrics.block_max_coord = line->metrics.baseline + descent + pc_rs_maxf_k(below, 0.0f);
    }
    line->metrics.inline_min_coord = b->line_x;
    line->metrics.inline_max_coord = b->line_x + b->line_max_advance;
    return PC_OK;
}

static pc_status start_new_line(breaker *b, pl_break_reason reason, float max_advance, float indent,
                                pc_error *err)
{
    float h;
    pc_status st = commit_line(b, max_advance, reason, indent, err);
    if (st != PC_OK) {
        return st;
    }
    h = b->line.running_line_height;
    b->line.x = 0.0f;
    b->line.running_line_height = 0.0f;
    b->has_prev = false;
    st = finish_line(b, b->nlines - 1, h, err);
    b->line_y += (double)h;
    return st;
}

static float resolve_indent(const breaker *b)
{
    return b->nlines == 0 ? b->l->indent_amount : 0.0f;
}

/* break_next_line_or_box. */
static pc_status break_next(breaker *b, yield *y, pc_error *err)
{
    pl_layout *l = b->l;
    float max_advance, indent;
    *y = Y_NONE;
    if (b->done) {
        return PC_OK;
    }
    max_advance = l->text_len == 0 ? FLT_MAX : b->line_max_advance;
    indent = resolve_indent(b);
    max_advance = max_advance - indent;
    while (b->item_idx < l->nitems) {
        const pl_run *run = &l->runs[l->items[b->item_idx]];
        while (b->cluster_idx < run->cluster_end) {
            const pl_cluster *c = run_get(l, run, b->cluster_idx - run->cluster_start);
            bool lig_cont, is_newline, is_space, exceeded;
            int ws;
            float advance, next_x, lh;
            if (c == NULL) {
                b->cluster_idx = run->cluster_end; /* the reference would panic */
                break;
            }
            lig_cont = is_ligature_continuation(c);
            ws = pl_whitespace(c->source_char);
            is_newline = ws == PL_WS_NEWLINE;
            is_space = ws == PL_WS_SPACE || ws == PL_WS_NBSP;
            lh = run->metrics.line_height;
            exceeded = b->line.max_height_exceeded;
            if (c->boundary == PL_BOUNDARY_LINE) {
                if (!lig_cont && b->line.x != 0.0f) {
                    mark_break(b);
                }
            } else if (is_newline) {
                if (exceeded) {
                    *y = Y_MAX_HEIGHT;
                    return PC_OK;
                }
                append_cluster(b, b->line.x, lh);
                *y = Y_LINE;
                return start_new_line(b, PL_BREAK_EXPLICIT, max_advance, indent, err);
            }
            advance = c->advance;
            if ((c->flags & PL_CLUSTER_LIGATURE_START) != 0) {
                const pl_cluster *n2;
                /* run.get(cluster_idx + 1): an absolute index used as a relative one, as in the
                 * reference. */
                while ((n2 = run_get(l, run, b->cluster_idx + 1)) != NULL) {
                    if (!is_ligature_continuation(n2)) {
                        break;
                    }
                    advance += n2->advance;
                    b->cluster_idx++;
                }
            }
            next_x = b->line.x + advance;
            if (next_x <= max_advance) {
                if (exceeded) {
                    *y = Y_MAX_HEIGHT;
                    return PC_OK;
                }
                append_cluster(b, next_x, lh);
                if (is_space) {
                    b->line.num_spaces++;
                }
            } else if (is_space) {
                if (exceeded) {
                    *y = Y_MAX_HEIGHT;
                    return PC_OK;
                }
                append_cluster(b, next_x, lh);
                *y = Y_LINE;
                return start_new_line(b, PL_BREAK_REGULAR, max_advance, indent, err);
            } else if (b->has_prev) {
                b->has_prev = false;
                b->item_idx = b->prev.item_idx;
                b->run_idx = b->prev.run_idx;
                b->cluster_idx = b->prev.cluster_idx;
                b->line = b->prev.state;
                *y = Y_LINE;
                return start_new_line(b, PL_BREAK_REGULAR, max_advance, indent, err);
            } else {
                if (exceeded) {
                    *y = Y_MAX_HEIGHT;
                    return PC_OK;
                }
                append_cluster(b, next_x, lh);
            }
        }
        b->run_idx++;
        b->item_idx++;
    }
    if (b->line.items_end == 0) {
        b->line.items_end = 1;
    }
    b->done = true;
    *y = Y_LINE;
    return start_new_line(b, PL_BREAK_NONE, max_advance, indent, err);
}

pc_status pl_break_all_lines(pl_layout *l, float max_advance, pc_error *err)
{
    breaker b;
    pc_status st = PC_OK;
    size_t i;
    float width = 0.0f;
    memset(&b, 0, sizeof b);
    b.l = l;
    b.line_max_height = FLT_MAX;
    b.layout_max_advance = max_advance;
    b.line_max_advance = max_advance;
    for (;;) {
        yield y;
        st = break_next(&b, &y, err);
        if (st != PC_OK) {
            break;
        }
        /* MaxHeightExceeded would repeat forever (a line of infinite height): stop there. */
        if (y != Y_LINE) {
            break;
        }
    }
    if (st != PC_OK) {
        free(b.lines);
        free(b.items);
        return st;
    }
    /* finish() */
    if (l->text_len == 0 && b.nitems > 0) {
        b.items[0].text_start = b.items[0].text_end = 0;
        b.items[0].cluster_start = b.items[0].cluster_end = 0;
    }
    /* Drop: widths, and infinite line widths replaced by the widest line. */
    for (i = 0; i < b.nlines; i++) {
        const pl_line *ln = &b.lines[i];
        float extra = pc_rs_maxf_k(ln->indent, 0.0f);
        float line_max = ln->metrics.inline_min_coord + ln->metrics.advance + extra;
        width = pc_rs_maxf(width, line_max - ln->metrics.trailing_whitespace);
    }
    if (b.layout_max_advance >= FLT_MAX) {
        for (i = 0; i < b.nlines; i++) {
            if (b.lines[i].metrics.inline_max_coord >= FLT_MAX) {
                b.lines[i].metrics.inline_max_coord = width;
            }
        }
    }
    free(l->lines);
    free(l->line_items);
    l->lines = b.lines;
    l->nlines = b.nlines;
    l->cap_lines = b.cap_lines;
    l->line_items = b.items;
    l->nline_items = b.nitems;
    l->cap_line_items = b.cap_items;
    return PC_OK;
}

void pl_align_lines(pl_layout *l, pl_align a, bool align_when_overflowing)
{
    bool rtl = (l->base_level & 1) != 0;
    size_t li;
    for (li = 0; li < l->nlines; li++) {
        pl_line *line = &l->lines[li];
        float indent = line->indent, width, free_space;
        line->metrics.offset = rtl ? -line->metrics.trailing_whitespace : indent;
        width = line->metrics.inline_max_coord - line->metrics.inline_min_coord;
        free_space = width - indent - line->metrics.advance + line->metrics.trailing_whitespace;
        if (!align_when_overflowing && free_space <= 0.0f) {
            if (rtl) {
                line->metrics.offset += free_space;
            }
            continue;
        }
        switch (a) {
        case PL_ALIGN_LEFT:
            break;
        case PL_ALIGN_RIGHT:
            line->metrics.offset += free_space;
            break;
        case PL_ALIGN_CENTER:
            line->metrics.offset += free_space * 0.5f;
            break;
        case PL_ALIGN_JUSTIFY: {
            float adjustment;
            size_t applied = 0, k, n;
            if (free_space <= 0.0f) {
                break;
            }
            if (line->break_reason == PL_BREAK_NONE || line->break_reason == PL_BREAK_EXPLICIT ||
                line->num_spaces == 0) {
                if (rtl) {
                    line->metrics.offset += free_space;
                }
                break;
            }
            adjustment = free_space / (float)line->num_spaces;
            n = line->item_end - line->item_start;
            for (k = 0; k < n; k++) {
                const pl_line_item *it = &l->line_items[rtl ? line->item_end - 1 - k : line->item_start + k];
                size_t cs = it->cluster_start,
                       ce = it->cluster_end < l->nclusters ? it->cluster_end : l->nclusters;
                size_t m, cn = ce > cs ? ce - cs : 0;
                bool irtl = (it->bidi_level & 1) != 0;
                for (m = 0; m < cn; m++) {
                    pl_cluster *c = &l->clusters[irtl ? ce - 1 - m : cs + m];
                    int ws;
                    if (applied == line->num_spaces) {
                        continue;
                    }
                    ws = pl_whitespace(c->source_char);
                    if (ws == PL_WS_SPACE || ws == PL_WS_NBSP) {
                        c->advance += adjustment;
                        applied++;
                    }
                }
            }
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Glyph runs */

size_t pl_cluster_glyphs(const pl_layout *l, const pl_run *run, const pl_cluster *c, pl_glyph *out,
                         size_t cap)
{
    size_t start, n, i;
    if (c->glyph_len == 0xFF) {
        if (cap > 0) {
            out[0] = (pl_glyph){c->glyph_offset, c->style_index, 0.0f, 0.0f, c->advance};
        }
        return 1;
    }
    start = run->glyph_start + c->glyph_offset;
    n = c->glyph_len;
    if (start > l->nglyphs || n > l->nglyphs - start) {
        return 0; /* the reference would panic */
    }
    for (i = 0; i < n && i < cap; i++) {
        out[i] = l->glyphs[start + i];
    }
    return n;
}

void pl_item_glyphs_fini(pl_item_glyphs *g)
{
    free(g->glyphs);
    free(g->cluster);
    memset(g, 0, sizeof *g);
}

pc_status pl_item_glyphs_get(const pl_layout *l, size_t item, pl_item_glyphs *out, pc_error *err)
{
    const pl_line_item *it = &l->line_items[item];
    const pl_run *run = &l->runs[it->run];
    size_t cs = it->cluster_start, ce = it->cluster_end < l->nclusters ? it->cluster_end : l->nclusters;
    size_t n = ce > cs ? ce - cs : 0, k;
    bool rtl = (run->bidi_level & 1) != 0;
    pl_glyph tmp[256];
    out->n = 0;
    for (k = 0; k < n; k++) {
        size_t ci = rtl ? ce - 1 - k : cs + k;
        size_t ng = pl_cluster_glyphs(l, run, &l->clusters[ci], tmp, 256), j;
        for (j = 0; j < ng && j < 256; j++) {
            if (out->n >= out->cap) {
                size_t cap = out->cap, cap2 = out->cap;
                pl_glyph *g = pc_grow_array(out->glyphs, &cap, out->n + 1, sizeof *g);
                size_t *c;
                if (g == NULL) {
                    return NOMEM(err);
                }
                out->glyphs = g;
                c = pc_grow_array(out->cluster, &cap2, out->n + 1, sizeof *c);
                if (c == NULL) {
                    return NOMEM(err);
                }
                out->cluster = c;
                out->cap = cap < cap2 ? cap : cap2;
            }
            out->glyphs[out->n] = tmp[j];
            out->cluster[out->n] = ci;
            out->n++;
        }
    }
    return PC_OK;
}

pc_status pl_line_glyph_runs(const pl_layout *l, size_t li, pl_glyph_run **out, size_t *nout, pc_error *err)
{
    const pl_line *line = &l->lines[li];
    pl_glyph_run *v = NULL;
    size_t nv = 0, cap = 0, k;
    float offset = 0.0f;
    pl_item_glyphs g;
    pc_status st = PC_OK;
    memset(&g, 0, sizeof g);
    for (k = line->item_start; k < line->item_end; k++) {
        size_t start = 0;
        st = pl_item_glyphs_get(l, k, &g, err);
        if (st != PC_OK) {
            break;
        }
        while (start < g.n) {
            pl_glyph_run r;
            uint16_t style = g.glyphs[start].style_index;
            float adv = g.glyphs[start].advance;
            size_t count = 1;
            while (start + count < g.n && g.glyphs[start + count].style_index == style) {
                adv += g.glyphs[start + count].advance;
                count++;
            }
            if (style >= l->nstyles) {
                goto done; /* Line::items() ends at an unknown style */
            }
            r.item = k;
            r.index = k - line->item_start;
            r.glyph_start = start;
            r.glyph_count = count;
            r.style_index = style;
            r.offset = offset + line->metrics.inline_min_coord + line->metrics.offset;
            r.baseline = line->metrics.baseline;
            r.advance = adv;
            if (nv >= cap) {
                void *gr = pc_grow_array(v, &cap, nv + 1, sizeof *v);
                if (gr == NULL) {
                    st = NOMEM(err);
                    goto done;
                }
                v = gr;
            }
            v[nv++] = r;
            start += count;
            offset += adv;
        }
    }
done:
    pl_item_glyphs_fini(&g);
    if (st != PC_OK) {
        free(v);
        return st;
    }
    *out = v;
    *nout = nv;
    return PC_OK;
}
