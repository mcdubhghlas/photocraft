/* Private parts of the layouter shared by layout.c (paragraph building) and layout_line.c (line
 * stacking, kerning, tabs, placement). Not a public header. */
#ifndef PC_TEXT_LAYOUT_INTERNAL_H
#define PC_TEXT_LAYOUT_INTERNAL_H

#include "text/layout.h"
#include "text/optical.h"
#include "text/playout.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

struct pc_text_layouter {
    pl_builder *b;
    pc_optical_cache *optical;
};

/* Photoshop's default tab stops: every half inch (36 pt), measured from the anchor of point text
 * or the left edge of a paragraph box. PSD type carries no custom tab stops. */
#define PC_TEXT_DEFAULT_TAB_STOP_PT 36.0f

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

typedef struct para_ctx {
    const pc_text_layer *t;
    float k;
    const pc_text_run *runs;
    size_t nruns;
    const size_t *run_starts;
    pc_small_caps_mode *small_caps;
} para_ctx;

static inline size_t style_at(const para_ctx *c, size_t off)
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
    /* Tabs: offsets in ptext of the spaces standing in for tabs (ascending), the stop interval
     * (px) and the paragraph's first-line indent (px). */
    const size_t *tabs;
    size_t ntabs;
    float tab_px, first_indent;
} line_ctx;

/* Stacks, kerns, scales and places line `li` of a paragraph. */
pc_status pc_text_lay_line(line_ctx *c, size_t li, pc_error *err);

/* next_tab_stop: the first stop after pos (px); stops are `interval` apart. */
float pc_text_next_tab_stop(float pos, float interval);

/* A tab as line breaking saw it: its shaped advance (before horizontal scale), the horizontal
 * scale, and the width that takes it to its tab stop on its line. */
typedef struct pc_text_tab_width {
    float advance, scale, width;
} pc_text_tab_width;

/* tab_widths: each tab's width on the broken lines of pl (before kerning and alignment); out has
 * ntabs entries (tabs not on any line keep {interval, 1, interval}). */
pc_status pc_text_tab_widths(const pl_layout *pl, const pc_text_layout *styles, const size_t *tabs,
                             size_t ntabs, float indent, float first_indent, float interval,
                             pc_text_tab_width *out, pc_error *err);

#endif
