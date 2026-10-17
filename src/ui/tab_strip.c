/* Tab-fit policy: see tab_strip.h. */
#include "ui/tab_strip.h"

#include <math.h>

static float clean_w(float w) { return (isfinite(w) && w > 0.0f) ? w : 0.0f; }

/* Water-fill: lay out the tabs in `set` (indices into natural[]) so the total is at most `target`,
 * shrinking the widest first but never below each tab's floor (min(w, min_w), so a tab already
 * narrower than the cap keeps its width). Writes each tab's width into out[] indexed by set order. */
static void water_fill(const float *natural, const size_t *set, size_t k, float target, float min_w,
                       float *out)
{
    float sum = 0.0f;
    for (size_t i = 0; i < k; i++) {
        sum += clean_w(natural[set[i]]);
    }
    if (sum <= target) {
        for (size_t i = 0; i < k; i++) {
            out[i] = clean_w(natural[set[i]]);
        }
        return;
    }
    /* Find level L in [min_w, maxw] with sum_i final(L) == target, final = w if w<=min_w else
     * min(L, w). Monotonic in L -> bisection. */
    float maxw = 0.0f;
    for (size_t i = 0; i < k; i++) {
        float w = clean_w(natural[set[i]]);
        if (w > maxw) {
            maxw = w;
        }
    }
    float lo = min_w < 0.0f ? 0.0f : min_w;
    float hi = maxw > lo ? maxw : lo;
    for (int it = 0; it < 60; it++) {
        float mid = (lo + hi) * 0.5f;
        float s = 0.0f;
        for (size_t i = 0; i < k; i++) {
            float w = clean_w(natural[set[i]]);
            s += (w <= min_w) ? w : (mid < w ? mid : w);
        }
        if (s > target) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    float level = (lo + hi) * 0.5f;
    for (size_t i = 0; i < k; i++) {
        float w = clean_w(natural[set[i]]);
        out[i] = (w <= min_w) ? w : (level < w ? level : w);
        if (out[i] < 0.0f) {
            out[i] = 0.0f;
        }
    }
}

pc_ui_tab_fit pc_ui_tab_fit_compute(const float *natural, size_t n, size_t selected, float avail,
                                    float min_w, float chevron_w)
{
    pc_ui_tab_fit f;
    f.n_shown = 0;
    f.n_overflow = 0;
    f.overflow_width = 0.0f;
    if (n > PC_UI_TAB_MAX) {
        n = PC_UI_TAB_MAX;
    }
    if (!isfinite(avail)) {
        avail = avail > 0.0f ? 1e30f : 0.0f;
    } else if (avail < 0.0f) {
        avail = 0.0f;
    }
    if (!(min_w >= 0.0f)) {
        min_w = 0.0f;
    }
    if (n == 0) {
        return f;
    }

    /* Do all tabs fit at their natural widths? */
    float natsum = 0.0f, minsum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float w = clean_w(natural[i]);
        natsum += w;
        minsum += (w < min_w) ? w : min_w;
    }
    if (natsum <= avail) {
        for (size_t i = 0; i < n; i++) {
            f.shown[f.n_shown].index = i;
            f.shown[f.n_shown].width = clean_w(natural[i]);
            f.n_shown++;
        }
        return f;
    }

    /* Do all tabs fit if we shrink the widest down to the floor? Then no chevron. */
    if (minsum <= avail) {
        size_t set[PC_UI_TAB_MAX];
        for (size_t i = 0; i < n; i++) {
            set[i] = i;
        }
        float out[PC_UI_TAB_MAX];
        water_fill(natural, set, n, avail, min_w, out);
        for (size_t i = 0; i < n; i++) {
            f.shown[i].index = i;
            f.shown[i].width = out[i];
        }
        f.n_shown = n;
        return f;
    }

    /* Overflow: show as many as fit in (avail - chevron), keeping the selected tab and tab order,
     * dropping the widest non-selected tab until the rest fit at their floors. */
    float budget = avail - (chevron_w > 0.0f ? chevron_w : 0.0f);
    if (budget < 0.0f) {
        budget = 0.0f;
    }
    bool shown_flag[PC_UI_TAB_MAX];
    for (size_t i = 0; i < n; i++) {
        shown_flag[i] = true;
    }
    for (;;) {
        float ms = 0.0f;
        for (size_t i = 0; i < n; i++) {
            if (shown_flag[i]) {
                float w = clean_w(natural[i]);
                ms += (w < min_w) ? w : min_w;
            }
        }
        if (ms <= budget) {
            break;
        }
        /* Drop the widest non-selected shown tab (highest index on a tie). */
        bool found = false;
        size_t drop = 0;
        float dropw = -1.0f;
        for (size_t i = 0; i < n; i++) {
            if (!shown_flag[i] || i == selected) {
                continue;
            }
            float w = clean_w(natural[i]);
            if (w >= dropw) {
                dropw = w;
                drop = i;
                found = true;
            }
        }
        if (!found) {
            break; /* only the selected tab remains */
        }
        shown_flag[drop] = false;
    }
    /* Collect shown (in order) and water-fill them into the budget. */
    size_t set[PC_UI_TAB_MAX];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (shown_flag[i]) {
            set[k++] = i;
        }
    }
    float out[PC_UI_TAB_MAX];
    water_fill(natural, set, k, budget, min_w, out);
    for (size_t i = 0; i < k; i++) {
        f.shown[i].index = set[i];
        f.shown[i].width = out[i];
    }
    f.n_shown = k;
    for (size_t i = 0; i < n; i++) {
        if (!shown_flag[i]) {
            f.overflow[f.n_overflow++] = i;
        }
    }
    f.overflow_width = f.n_overflow > 0 ? (chevron_w > 0.0f ? chevron_w : 0.0f) : 0.0f;
    return f;
}

bool pc_ui_tab_drop_slot(const float *tab_cx, size_t n_tabs, float x0, float y0, float x1, float y1,
                         float px, float py, size_t *slot)
{
    float h = y1 - y0;
    /* area.expand2(vec2(24, area.height())): 24 px each side, one height above and below. */
    if (px < x0 - 24.0f || px > x1 + 24.0f || py < y0 - h || py > y1 + h) {
        return false;
    }
    size_t count = 0;
    for (size_t i = 0; i < n_tabs; i++) {
        if (tab_cx[i] < px) {
            count++;
        }
    }
    *slot = count;
    return true;
}
