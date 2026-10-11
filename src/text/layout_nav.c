/* Carets, hit tests and caret navigation over a laid-out text (layout.rs TextLayout methods and
 * the free navigation functions). Text arguments are UTF-8; invalid bytes count as one char. */
#include "text/layout.h"

#include <math.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "base/utf8.h"

static bool is_boundary(const char *text, size_t len, size_t i)
{
    return i == 0 || i >= len || ((unsigned char)text[i] & 0xC0) != 0x80;
}

/* Steps over one char (an invalid byte counts as one). */
static uint32_t step(const char *text, size_t len, size_t *i)
{
    uint32_t cp;
    if (!pc_utf8_next((const uint8_t *)text, len, i, &cp)) {
        (*i)++;
        return 0xFFFD;
    }
    return cp;
}

/* f32::total_cmp. */
static int total_cmp(float a, float b)
{
    uint32_t ua, ub;
    int32_t ia, ib;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    ia = pc_u32_as_i32(ua ^ ((ua & 0x80000000u) != 0 ? 0x7FFFFFFFu : 0u));
    ib = pc_u32_as_i32(ub ^ ((ub & 0x80000000u) != 0 ? 0x7FFFFFFFu : 0u));
    return ia < ib ? -1 : ia > ib;
}

size_t pc_text_layout_hit_test_line(const pc_text_layout *l, float x, float y)
{
    size_t li = 0, i;
    float bestd = 0.0f, bestx = 3.40282347e+38f; /* f32::MAX */
    size_t off;
    const pc_line_info *line;
    if (l->nlines == 0) {
        return 0;
    }
    for (i = 0; i < l->nlines; i++) {
        const pc_line_info *ln = &l->lines[i];
        float top = ln->baseline - ln->ascent, bottom = ln->baseline + ln->descent;
        float d = y < top ? top - y : y > bottom ? y - bottom : 0.0f;
        if (i == 0 || total_cmp(d, bestd) < 0) {
            bestd = d;
            li = i;
        }
    }
    line = &l->lines[li];
    off = line->end;
    for (i = 0; i < l->nclusters; i++) {
        const pc_cluster_info *c = &l->clusters[i];
        float mid, dist;
        size_t before, after, o;
        if (c->line != li) {
            continue;
        }
        mid = c->x + c->advance / 2.0f;
        before = c->rtl ? c->end : c->start;
        after = c->rtl ? c->start : c->end;
        if (x < mid) {
            dist = fabsf(x - c->x);
            o = before;
        } else {
            dist = fabsf(x - c->x - c->advance);
            o = after;
        }
        if (dist < bestx) {
            bestx = dist;
            off = o;
        }
    }
    return off;
}

size_t pc_text_layout_hit_test(const pc_text_layout *l, float x, float y)
{
    float lx, ly;
    pc_text_layout_to_line(l, x, y, &lx, &ly);
    return pc_text_layout_hit_test_line(l, lx, ly);
}

void pc_text_layout_caret(const pc_text_layout *l, size_t offset, float *x, float *top, float *bottom)
{
    size_t i, li = l->nlines > 0 ? l->nlines - 1 : 0;
    for (i = 0; i < l->nclusters; i++) {
        const pc_cluster_info *c = &l->clusters[i];
        if (c->start == offset && c->line < l->nlines) {
            const pc_line_info *ln = &l->lines[c->line];
            *x = c->rtl ? c->x + c->advance : c->x;
            *top = ln->baseline - ln->ascent;
            *bottom = ln->baseline + ln->descent;
            return;
        }
    }
    for (i = 0; i < l->nlines; i++) {
        if (offset >= l->lines[i].start && offset <= l->lines[i].end) {
            li = i;
            break;
        }
    }
    if (li < l->nlines) {
        const pc_line_info *ln = &l->lines[li];
        float cx = ln->x1;
        for (i = 0; i < l->nclusters; i++) {
            const pc_cluster_info *c = &l->clusters[i];
            if (c->line == li && c->end == offset) {
                cx = c->rtl ? c->x : c->x + c->advance;
                break;
            }
        }
        *x = cx;
        *top = ln->baseline - ln->ascent;
        *bottom = ln->baseline + ln->descent;
        return;
    }
    *x = 0.0f;
    *top = 0.0f;
    *bottom = 0.0f;
}

void pc_text_layout_caret_segment(const pc_text_layout *l, size_t offset, float out[4])
{
    float x, top, bottom;
    pc_text_layout_caret(l, offset, &x, &top, &bottom);
    pc_text_layout_to_text(l, x, top, &out[0], &out[1]);
    pc_text_layout_to_text(l, x, bottom, &out[2], &out[3]);
}

size_t pc_text_byte_index(const char *text, size_t len, size_t index)
{
    size_t i = 0, n = 0;
    while (i < len) {
        if (n == index) {
            return i;
        }
        (void)step(text, len, &i);
        n++;
    }
    return len;
}

size_t pc_text_char_index(const char *text, size_t len, size_t byte)
{
    size_t i = 0, n = 0;
    if (byte > len) {
        byte = len;
    }
    while (byte > 0 && !is_boundary(text, len, byte)) {
        byte--;
    }
    while (i < byte) {
        (void)step(text, byte, &i);
        n++;
    }
    return n;
}

size_t pc_text_line_index(const pc_text_layout *l, size_t byte)
{
    size_t i;
    for (i = 0; i < l->nlines; i++) {
        if (byte >= l->lines[i].start && byte <= l->lines[i].end) {
            return i;
        }
    }
    return l->nlines > 0 ? l->nlines - 1 : 0;
}

void pc_text_hit_char(const pc_text_layout *l, const char *text, size_t len, float x, float y, size_t *index,
                      size_t *line)
{
    size_t byte = pc_text_layout_hit_test(l, x, y);
    *index = pc_text_char_index(text, len, byte);
    *line = pc_text_line_index(l, byte);
}

bool pc_text_point_inside(const pc_text_layout *l, float x, float y, float slop)
{
    float b[4];
    slop = isfinite(slop) ? pc_rs_maxf_k(slop, 0.0f) : 0.0f;
    return pc_text_layout_bounds(l, b) && x >= b[0] - slop && x <= b[2] + slop && y >= b[1] - slop &&
           y <= b[3] + slop;
}

/* The char at character index i (callers keep i in range). */
static uint32_t char_at(const char *text, size_t len, size_t i)
{
    size_t at = pc_text_byte_index(text, len, i);
    return step(text, len, &at);
}

static size_t char_count(const char *text, size_t len)
{
    size_t i = 0, n = 0;
    while (i < len) {
        (void)step(text, len, &i);
        n++;
    }
    return n;
}

size_t pc_text_word_boundary(const char *text, size_t len, size_t idx, bool forward)
{
    size_t n = char_count(text, len), i = idx < n ? idx : n;
    /* Walk bytes alongside the char index so the scan stays linear. */
    if (forward) {
        size_t at = pc_text_byte_index(text, len, i);
        while (i < n) {
            size_t b = at;
            if (pc_unicode_is_alphanumeric(step(text, len, &b))) {
                break;
            }
            at = b;
            i++;
        }
        while (i < n) {
            size_t b = at;
            if (!pc_unicode_is_alphanumeric(step(text, len, &b))) {
                break;
            }
            at = b;
            i++;
        }
    } else {
        while (i > 0 && !pc_unicode_is_alphanumeric(char_at(text, len, i - 1))) {
            i--;
        }
        while (i > 0 && pc_unicode_is_alphanumeric(char_at(text, len, i - 1))) {
            i--;
        }
    }
    return i;
}

size_t pc_text_line_step(const pc_text_layout *l, const char *text, size_t len, size_t idx, float x, int dir)
{
    size_t n = char_count(text, len);
    float cx, top, bottom, h, y, b[4];
    idx = idx < n ? idx : n;
    pc_text_layout_caret(l, pc_text_byte_index(text, len, idx), &cx, &top, &bottom);
    h = pc_rs_maxf_k(bottom - top, 1.0f);
    y = dir < 0 ? top - h * 0.5f : bottom + h * 0.5f;
    if (!pc_text_layout_line_bounds(l, b)) {
        return idx;
    }
    if (y < b[1]) {
        return 0;
    }
    if (y > b[3]) {
        return n;
    }
    return pc_text_char_index(text, len, pc_text_layout_hit_test_line(l, x, y));
}

size_t pc_text_line_edge(const pc_text_layout *l, const char *text, size_t len, size_t idx, bool end)
{
    size_t n = char_count(text, len), byte, i;
    const pc_line_info *ln = NULL;
    idx = idx < n ? idx : n;
    byte = pc_text_byte_index(text, len, idx);
    for (i = 0; i < l->nlines; i++) {
        if (byte >= l->lines[i].start && byte <= l->lines[i].end) {
            ln = &l->lines[i];
            break;
        }
    }
    if (ln == NULL && l->nlines > 0) {
        ln = &l->lines[l->nlines - 1];
    }
    return ln == NULL ? idx : pc_text_char_index(text, len, end ? ln->end : ln->start);
}
