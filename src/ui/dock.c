/* Dock layout data model: see dock.h. Port of crates/ui-egui/src/dock.rs (the DockLayout + Group
 * logic; the egui rendering is rebuilt in the shell). Pure logic, never crashes on hostile input. */
#include "ui/dock.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const pc_ui_group PC_UI_GROUP_ALL[PC_UI_GROUP_N] = {PC_UI_GROUP_COLOR,     PC_UI_GROUP_PROPERTIES,
                                                    PC_UI_GROUP_CHARACTER, PC_UI_GROUP_NAVIGATOR,
                                                    PC_UI_GROUP_HISTORY,   PC_UI_GROUP_LAYERS};

const char *pc_ui_group_key(pc_ui_group g)
{
    switch (g) {
    case PC_UI_GROUP_COLOR:
        return "color";
    case PC_UI_GROUP_PROPERTIES:
        return "properties";
    case PC_UI_GROUP_CHARACTER:
        return "character";
    case PC_UI_GROUP_NAVIGATOR:
        return "navigator";
    case PC_UI_GROUP_HISTORY:
        return "history";
    case PC_UI_GROUP_LAYERS:
        return "layers";
    default:
        return "";
    }
}

bool pc_ui_group_from_key(const char *key, pc_ui_group *out)
{
    if (key == NULL) {
        return false;
    }
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        if (strcmp(pc_ui_group_key(PC_UI_GROUP_ALL[i]), key) == 0) {
            *out = PC_UI_GROUP_ALL[i];
            return true;
        }
    }
    return false;
}

float pc_ui_group_default_height(pc_ui_group g)
{
    switch (g) {
    case PC_UI_GROUP_COLOR:
        return 190.0f;
    case PC_UI_GROUP_PROPERTIES:
        return 250.0f;
    case PC_UI_GROUP_CHARACTER:
        return 270.0f;
    case PC_UI_GROUP_NAVIGATOR:
        return 210.0f;
    case PC_UI_GROUP_HISTORY:
        return 200.0f;
    case PC_UI_GROUP_LAYERS:
        return 320.0f;
    default:
        return 0.0f;
    }
}

float pc_ui_group_compact_height(pc_ui_group g)
{
    switch (g) {
    case PC_UI_GROUP_COLOR:
        return 130.0f;
    case PC_UI_GROUP_PROPERTIES:
        return 160.0f;
    case PC_UI_GROUP_CHARACTER:
        return 160.0f;
    case PC_UI_GROUP_NAVIGATOR:
        return 140.0f;
    case PC_UI_GROUP_HISTORY:
        return 130.0f;
    case PC_UI_GROUP_LAYERS:
        return 200.0f;
    default:
        return 0.0f;
    }
}

float pc_ui_group_min_height(pc_ui_group g)
{
    return g == PC_UI_GROUP_LAYERS ? 140.0f : 80.0f;
}

float pc_ui_group_preferred_fill(pc_ui_group g)
{
    return g == PC_UI_GROUP_LAYERS ? 500.0f : pc_ui_group_min_height(g);
}

const char *const *pc_ui_group_tabs(pc_ui_group g, bool pro, size_t *n_out)
{
    static const char *const color_pro[] = {"Color", "Swatches", "Gradients", "Patterns"};
    static const char *const color_std[] = {"Swatches", "Color", "Gradients", "Patterns"};
    static const char *const properties[] = {"Properties", "Adjustments"};
    static const char *const character[] = {"Character", "Paragraph"};
    static const char *const navigator[] = {"Navigator", "Histogram", "Info"};
    static const char *const history[] = {"History", "Actions", "Layer Comps"};
    static const char *const layers[] = {"Layers", "Channels", "Paths"};
    switch (g) {
    case PC_UI_GROUP_COLOR:
        *n_out = 4;
        return pro ? color_pro : color_std;
    case PC_UI_GROUP_PROPERTIES:
        *n_out = 2;
        return properties;
    case PC_UI_GROUP_CHARACTER:
        *n_out = 2;
        return character;
    case PC_UI_GROUP_NAVIGATOR:
        *n_out = 3;
        return navigator;
    case PC_UI_GROUP_HISTORY:
        *n_out = 3;
        return history;
    case PC_UI_GROUP_LAYERS:
        *n_out = 3;
        return layers;
    default:
        *n_out = 0;
        return NULL;
    }
}

bool pc_ui_group_scrolls_itself(pc_ui_group g, size_t tab)
{
    return (g == PC_UI_GROUP_LAYERS && tab == 0) || (g == PC_UI_GROUP_HISTORY && tab == 0) ||
           (g == PC_UI_GROUP_HISTORY && tab == 1) || (g == PC_UI_GROUP_NAVIGATOR && tab == 0);
}

/* ---- name lists ---- */

static void name_list_fini(pc_ui_name_list *nl)
{
    if (nl->names) {
        for (size_t i = 0; i < nl->n; i++) {
            free(nl->names[i]);
        }
        free(nl->names);
    }
    nl->names = NULL;
    nl->n = 0;
}

static bool name_list_has(const pc_ui_name_list *nl, const char *name)
{
    for (size_t i = 0; i < nl->n; i++) {
        if (strcmp(nl->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* Append a copy of `name`, capped at `cap`; ignored (no crash) above the cap. False only on OOM. */
static bool name_list_push_cap(pc_ui_name_list *nl, const char *name, size_t cap)
{
    if (nl->n >= cap) {
        return true;
    }
    char *copy = strdup(name);
    if (!copy) {
        return false;
    }
    char **grown = realloc(nl->names, (nl->n + 1) * sizeof *grown);
    if (!grown) {
        free(copy);
        return false;
    }
    nl->names = grown;
    nl->names[nl->n++] = copy;
    return true;
}

/* Append a copy of `name`; ignored (no crash) on allocation failure or above the cap. */
static bool name_list_push(pc_ui_name_list *nl, const char *name)
{
    if (nl->n >= PC_UI_DOCK_MAX_ORDER_NAMES) {
        return true;
    }
    char *copy = strdup(name);
    if (!copy) {
        return false;
    }
    char **grown = realloc(nl->names, (nl->n + 1) * sizeof *grown);
    if (!grown) {
        free(copy);
        return false;
    }
    nl->names = grown;
    nl->names[nl->n++] = copy;
    return true;
}

static void name_list_remove(pc_ui_name_list *nl, const char *name)
{
    size_t w = 0;
    for (size_t i = 0; i < nl->n; i++) {
        if (strcmp(nl->names[i], name) == 0) {
            free(nl->names[i]);
        } else {
            nl->names[w++] = nl->names[i];
        }
    }
    nl->n = w;
}

static bool name_list_eq(const pc_ui_name_list *a, const pc_ui_name_list *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (size_t i = 0; i < a->n; i++) {
        if (strcmp(a->names[i], b->names[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* ---- lifecycle ---- */

void pc_ui_dock_layout_init(pc_ui_dock_layout *l)
{
    memset(l, 0, sizeof *l);
}

void pc_ui_dock_layout_fini(pc_ui_dock_layout *l)
{
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        name_list_fini(&l->hidden_tabs[i]);
        name_list_fini(&l->tab_order[i]);
        name_list_fini(&l->members[i]);
    }
    pc_ui_dock_layout_init(l);
}

void pc_ui_dock_layout_copy(pc_ui_dock_layout *dst, const pc_ui_dock_layout *src)
{
    pc_ui_dock_layout_fini(dst);
    dst->n_order = src->n_order;
    memcpy(dst->order, src->order, sizeof dst->order);
    memcpy(dst->has_height, src->has_height, sizeof dst->has_height);
    memcpy(dst->heights, src->heights, sizeof dst->heights);
    dst->n_collapsed = src->n_collapsed;
    memcpy(dst->collapsed, src->collapsed, sizeof dst->collapsed);
    dst->n_floating = src->n_floating;
    memcpy(dst->floating, src->floating, sizeof dst->floating);
    memcpy(dst->has_float_rect, src->has_float_rect, sizeof dst->has_float_rect);
    memcpy(dst->float_rects, src->float_rects, sizeof dst->float_rects);
    dst->customized = src->customized;
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        for (size_t j = 0; j < src->hidden_tabs[i].n; j++) {
            name_list_push(&dst->hidden_tabs[i], src->hidden_tabs[i].names[j]);
        }
        for (size_t j = 0; j < src->tab_order[i].n; j++) {
            name_list_push(&dst->tab_order[i], src->tab_order[i].names[j]);
        }
        for (size_t j = 0; j < src->members[i].n; j++) {
            name_list_push_cap(&dst->members[i], src->members[i].names[j], PC_UI_DOCK_MAX_MEMBERS);
        }
    }
}

bool pc_ui_dock_layout_eq(const pc_ui_dock_layout *a, const pc_ui_dock_layout *b)
{
    if (a->n_order != b->n_order || a->n_collapsed != b->n_collapsed ||
        a->n_floating != b->n_floating) {
        return false;
    }
    for (size_t i = 0; i < a->n_order; i++) {
        if (a->order[i] != b->order[i]) {
            return false;
        }
    }
    for (size_t i = 0; i < a->n_collapsed; i++) {
        if (a->collapsed[i] != b->collapsed[i]) {
            return false;
        }
    }
    for (size_t i = 0; i < a->n_floating; i++) {
        if (a->floating[i] != b->floating[i]) {
            return false;
        }
    }
    for (size_t g = 0; g < PC_UI_GROUP_N; g++) {
        if (a->has_height[g] != b->has_height[g]) {
            return false;
        }
        if (a->has_height[g] && a->heights[g] != b->heights[g]) {
            return false;
        }
        if (a->has_float_rect[g] != b->has_float_rect[g]) {
            return false;
        }
        if (a->has_float_rect[g] &&
            memcmp(a->float_rects[g], b->float_rects[g], sizeof a->float_rects[g]) != 0) {
            return false;
        }
        if (!name_list_eq(&a->hidden_tabs[g], &b->hidden_tabs[g]) ||
            !name_list_eq(&a->tab_order[g], &b->tab_order[g]) ||
            !name_list_eq(&a->members[g], &b->members[g])) {
            return false;
        }
    }
    return a->customized == b->customized;
}

/* ---- order ---- */

static bool contains_group(const pc_ui_group *xs, size_t n, pc_ui_group g)
{
    for (size_t i = 0; i < n; i++) {
        if (xs[i] == g) {
            return true;
        }
    }
    return false;
}

static size_t position_group(const pc_ui_group *xs, size_t n, pc_ui_group g, size_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (xs[i] == g) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

void pc_ui_dock_order(const pc_ui_dock_layout *l, pc_ui_group out[PC_UI_GROUP_N])
{
    size_t n = 0;
    for (size_t i = 0; i < l->n_order; i++) {
        if (!contains_group(out, n, l->order[i])) {
            out[n++] = l->order[i];
        }
    }
    /* Missing groups slot in just below their default predecessor. */
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        pc_ui_group g = PC_UI_GROUP_ALL[i];
        if (contains_group(out, n, g)) {
            continue;
        }
        size_t at;
        bool found = false;
        for (size_t k = i; k-- > 0;) { /* ALL[0..i] reversed */
            if (position_group(out, n, PC_UI_GROUP_ALL[k], &at)) {
                found = true;
                break;
            }
        }
        if (found) {
            for (size_t m = n; m > at + 1; m--) {
                out[m] = out[m - 1];
            }
            out[at + 1] = g;
            n++;
        } else {
            out[n++] = g;
        }
    }
}

bool pc_ui_dock_is_collapsed(const pc_ui_dock_layout *l, pc_ui_group g)
{
    return contains_group(l->collapsed, l->n_collapsed, g);
}

void pc_ui_dock_set_collapsed(pc_ui_dock_layout *l, pc_ui_group g, bool on)
{
    size_t w = 0;
    for (size_t i = 0; i < l->n_collapsed; i++) {
        if (l->collapsed[i] != g) {
            l->collapsed[w++] = l->collapsed[i];
        }
    }
    l->n_collapsed = w;
    if (on && l->n_collapsed < PC_UI_GROUP_N) {
        l->collapsed[l->n_collapsed++] = g;
    }
}

float pc_ui_dock_height(const pc_ui_dock_layout *l, pc_ui_group g)
{
    if (l->has_height[g] && isfinite(l->heights[g])) {
        float h = l->heights[g];
        float lo = pc_ui_group_min_height(g);
        if (h < lo) {
            h = lo;
        }
        if (h > PC_UI_DOCK_MAX_HEIGHT) {
            h = PC_UI_DOCK_MAX_HEIGHT;
        }
        return h;
    }
    return pc_ui_group_default_height(g);
}

void pc_ui_dock_move_group(pc_ui_dock_layout *l, pc_ui_group g, const pc_ui_group *before)
{
    if (before != NULL && *before == g) {
        return;
    }
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(l, order);
    /* Remove g. */
    size_t n = 0;
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        if (order[i] != g) {
            order[n++] = order[i];
        }
    }
    size_t at = n;
    if (before != NULL) {
        size_t pos;
        if (position_group(order, n, *before, &pos)) {
            at = pos;
        }
    }
    for (size_t m = n; m > at; m--) {
        order[m] = order[m - 1];
    }
    order[at] = g;
    n++;
    memcpy(l->order, order, n * sizeof(pc_ui_group));
    l->n_order = n;
}

/* ---- floating ---- */

bool pc_ui_dock_is_floating(const pc_ui_dock_layout *l, pc_ui_group g)
{
    return contains_group(l->floating, l->n_floating, g);
}

static void sanitize_float_rect(float r[4])
{
    for (int i = 0; i < 4; i++) {
        if (!isfinite(r[i])) {
            r[i] = 0.0f;
        }
    }
    float lo = -PC_UI_DOCK_MAX_HEIGHT, hi = PC_UI_DOCK_MAX_HEIGHT;
    for (int i = 0; i < 2; i++) {
        if (r[i] < lo) {
            r[i] = lo;
        }
        if (r[i] > hi) {
            r[i] = hi;
        }
    }
    if (r[2] < PC_UI_DOCK_FLOAT_MIN_W) {
        r[2] = PC_UI_DOCK_FLOAT_MIN_W;
    }
    if (r[3] < PC_UI_DOCK_FLOAT_MIN_H) {
        r[3] = PC_UI_DOCK_FLOAT_MIN_H;
    }
    if (r[2] > hi) {
        r[2] = hi;
    }
    if (r[3] > hi) {
        r[3] = hi;
    }
}

void pc_ui_dock_set_float_rect(pc_ui_dock_layout *l, pc_ui_group g, const float rect[4])
{
    if ((unsigned)g >= PC_UI_GROUP_N || rect == NULL) {
        return;
    }
    float r[4] = {rect[0], rect[1], rect[2], rect[3]};
    sanitize_float_rect(r);
    l->has_float_rect[g] = true;
    memcpy(l->float_rects[g], r, sizeof r);
}

void pc_ui_dock_float(pc_ui_dock_layout *l, pc_ui_group g, const float rect[4])
{
    if ((unsigned)g >= PC_UI_GROUP_N) {
        return;
    }
    if (!contains_group(l->floating, l->n_floating, g) && l->n_floating < PC_UI_GROUP_N) {
        l->floating[l->n_floating++] = g;
    }
    if (rect != NULL) {
        pc_ui_dock_set_float_rect(l, g, rect);
    }
}

void pc_ui_dock_unfloat(pc_ui_dock_layout *l, pc_ui_group g)
{
    size_t w = 0;
    for (size_t i = 0; i < l->n_floating; i++) {
        if (l->floating[i] != g) {
            l->floating[w++] = l->floating[i];
        }
    }
    l->n_floating = w;
    if ((unsigned)g < PC_UI_GROUP_N) {
        l->has_float_rect[g] = false;
    }
}

bool pc_ui_dock_float_rect(const pc_ui_dock_layout *l, pc_ui_group g, float out[4])
{
    if ((unsigned)g >= PC_UI_GROUP_N || out == NULL || !l->has_float_rect[g] ||
        !contains_group(l->floating, l->n_floating, g)) {
        return false;
    }
    memcpy(out, l->float_rects[g], sizeof l->float_rects[g]);
    sanitize_float_rect(out);
    return true;
}

/* ---- tabs ---- */

static bool index_in(const size_t *xs, size_t n, size_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (xs[i] == v) {
            return true;
        }
    }
    return false;
}

size_t pc_ui_dock_ordered_tabs(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                               size_t out[PC_UI_DOCK_MAX_TABS])
{
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    size_t n = 0;
    const pc_ui_name_list *order = &l->tab_order[g];
    for (size_t i = 0; i < order->n; i++) {
        for (size_t t = 0; t < ntabs; t++) {
            if (strcmp(tabs[t], order->names[i]) == 0) {
                if (!index_in(out, n, t)) {
                    out[n++] = t;
                }
                break;
            }
        }
    }
    for (size_t t = 0; t < ntabs; t++) {
        if (!index_in(out, n, t)) {
            out[n++] = t;
        }
    }
    return n;
}

size_t pc_ui_dock_visible_tabs(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                               size_t out[PC_UI_DOCK_MAX_TABS])
{
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    size_t ordered[PC_UI_DOCK_MAX_TABS];
    size_t no = pc_ui_dock_ordered_tabs(l, g, pro, ordered);
    size_t n = 0;
    for (size_t i = 0; i < no; i++) {
        size_t t = ordered[i];
        if (t < ntabs && !name_list_has(&l->hidden_tabs[g], tabs[t])) {
            out[n++] = t;
        }
    }
    return n;
}

void pc_ui_dock_move_tab(pc_ui_dock_layout *l, pc_ui_group g, size_t tab, const size_t *before, bool pro)
{
    if (before != NULL && *before == tab) {
        return;
    }
    size_t order[PC_UI_DOCK_MAX_TABS];
    size_t n = pc_ui_dock_ordered_tabs(l, g, pro, order);
    size_t at = n;
    for (size_t i = 0; i < n; i++) {
        if (order[i] == tab) {
            at = i;
            break;
        }
    }
    if (at == n) {
        return;
    }
    /* remove at. */
    for (size_t i = at; i + 1 < n; i++) {
        order[i] = order[i + 1];
    }
    n--;
    size_t to = n;
    if (before != NULL) {
        for (size_t i = 0; i < n; i++) {
            if (order[i] == *before) {
                to = i;
                break;
            }
        }
    }
    for (size_t m = n; m > to; m--) {
        order[m] = order[m - 1];
    }
    order[to] = tab;
    n++;
    /* Identity order stores no entry. */
    bool identity = true;
    for (size_t i = 0; i < n; i++) {
        if (order[i] != i) {
            identity = false;
            break;
        }
    }
    name_list_fini(&l->tab_order[g]);
    if (!identity) {
        size_t ntabs = 0;
        const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
        for (size_t i = 0; i < n; i++) {
            if (order[i] < ntabs) {
                name_list_push(&l->tab_order[g], tabs[order[i]]);
            }
        }
    }
}

void pc_ui_dock_hide_tab(pc_ui_dock_layout *l, pc_ui_group g, size_t tab, bool pro)
{
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    if (tab >= ntabs) {
        return;
    }
    if (!name_list_has(&l->hidden_tabs[g], tabs[tab])) {
        name_list_push(&l->hidden_tabs[g], tabs[tab]);
    }
}

void pc_ui_dock_show_tab(pc_ui_dock_layout *l, pc_ui_group g, size_t tab, bool pro)
{
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    if (tab >= ntabs) {
        return;
    }
    name_list_remove(&l->hidden_tabs[g], tabs[tab]);
}

/* ---- mutable panel -> group membership ---- */

bool pc_ui_dock_label_native_group(const char *label, pc_ui_group *out)
{
    if (label == NULL) {
        return false;
    }
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        pc_ui_group g = PC_UI_GROUP_ALL[i];
        size_t n = 0;
        /* Labels are identical across pro/std (only the Color group's order swaps), so `false` lists
         * every panel of the group exactly once. */
        const char *const *tabs = pc_ui_group_tabs(g, false, &n);
        for (size_t t = 0; t < n; t++) {
            if (strcmp(tabs[t], label) == 0) {
                if (out != NULL) {
                    *out = g;
                }
                return true;
            }
        }
    }
    return false;
}

bool pc_ui_dock_label_scrolls_itself(const char *label)
{
    if (label == NULL) {
        return false;
    }
    return strcmp(label, "Layers") == 0 || strcmp(label, "History") == 0 ||
           strcmp(label, "Actions") == 0 || strcmp(label, "Navigator") == 0;
}

size_t pc_ui_dock_group_labels(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                               const char *out[PC_UI_DOCK_MAX_MEMBERS])
{
    if ((unsigned)g >= PC_UI_GROUP_N) {
        return 0;
    }
    if (l->customized) {
        const pc_ui_name_list *m = &l->members[g];
        size_t n = 0;
        for (size_t i = 0; i < m->n && n < PC_UI_DOCK_MAX_MEMBERS; i++) {
            out[n++] = m->names[i];
        }
        return n;
    }
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    size_t n = 0;
    for (size_t t = 0; t < ntabs && n < PC_UI_DOCK_MAX_MEMBERS; t++) {
        out[n++] = tabs[t];
    }
    return n;
}

size_t pc_ui_dock_visible_labels(const pc_ui_dock_layout *l, pc_ui_group g, bool pro,
                                 const char *out[PC_UI_DOCK_MAX_MEMBERS])
{
    const char *all[PC_UI_DOCK_MAX_MEMBERS];
    size_t na = pc_ui_dock_group_labels(l, g, pro, all);
    size_t n = 0;
    for (size_t i = 0; i < na; i++) {
        if (!name_list_has(&l->hidden_tabs[g], all[i])) {
            out[n++] = all[i];
        }
    }
    return n;
}

bool pc_ui_dock_label_group(const pc_ui_dock_layout *l, const char *label, bool pro, pc_ui_group *out)
{
    if (label == NULL) {
        return false;
    }
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        pc_ui_group g = PC_UI_GROUP_ALL[i];
        const char *labels[PC_UI_DOCK_MAX_MEMBERS];
        size_t n = pc_ui_dock_group_labels(l, g, pro, labels);
        for (size_t t = 0; t < n; t++) {
            if (strcmp(labels[t], label) == 0) {
                if (out != NULL) {
                    *out = g;
                }
                return true;
            }
        }
    }
    return false;
}

/* Freeze the current native (uncustomized) layout into explicit per-group member lists. */
static void ensure_customized(pc_ui_dock_layout *l, bool pro)
{
    if (l->customized) {
        return;
    }
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        pc_ui_group g = PC_UI_GROUP_ALL[i];
        name_list_fini(&l->members[g]);
        size_t ntabs = 0;
        const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
        for (size_t t = 0; t < ntabs; t++) {
            name_list_push_cap(&l->members[g], tabs[t], PC_UI_DOCK_MAX_MEMBERS);
        }
    }
    l->customized = true;
}

void pc_ui_dock_move_panel(pc_ui_dock_layout *l, const char *panel, pc_ui_group target,
                           const char *before, bool pro)
{
    if (l == NULL || panel == NULL || (unsigned)target >= PC_UI_GROUP_N) {
        return;
    }
    if (!pc_ui_dock_label_native_group(panel, NULL)) {
        return; /* not a registered panel */
    }
    /* `before` must be a real label in the target group to anchor the insert; otherwise append. */
    const char *anchor = NULL;
    if (before != NULL && strcmp(before, panel) != 0 &&
        pc_ui_dock_label_native_group(before, NULL)) {
        anchor = before;
    }
    ensure_customized(l, pro);
    /* Remove the panel from every group. */
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        name_list_remove(&l->members[i], panel);
    }
    /* Rebuild the target group, inserting `panel` just before `anchor` (or at the end). */
    pc_ui_name_list *m = &l->members[target];
    pc_ui_name_list rebuilt = {0};
    bool placed = false;
    for (size_t i = 0; i < m->n; i++) {
        if (!placed && anchor != NULL && strcmp(m->names[i], anchor) == 0) {
            name_list_push_cap(&rebuilt, panel, PC_UI_DOCK_MAX_MEMBERS);
            placed = true;
        }
        name_list_push_cap(&rebuilt, m->names[i], PC_UI_DOCK_MAX_MEMBERS);
    }
    if (!placed) {
        name_list_push_cap(&rebuilt, panel, PC_UI_DOCK_MAX_MEMBERS);
    }
    name_list_fini(m);
    *m = rebuilt;
}

/* ---- heights ---- */

static float clamp_give(float h, float floor, float deficit)
{
    float give = h - floor;
    if (give < 0.0f) {
        give = 0.0f;
    }
    if (give > deficit) {
        give = deficit;
    }
    return give;
}

static float sum_heights(const float *hs, size_t n)
{
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) {
        s += hs[i];
    }
    return s;
}

/* The last expanded group in `shown`, or SIZE_MAX if none. */
static size_t filler_index(const pc_ui_dock_layout *l, const pc_ui_group *shown, size_t n)
{
    for (size_t i = n; i-- > 0;) {
        if (!pc_ui_dock_is_collapsed(l, shown[i])) {
            return i;
        }
    }
    return (size_t)-1;
}

void pc_ui_dock_heights_for(const pc_ui_dock_layout *l, const pc_ui_group *shown, size_t n, float avail,
                            float strip, float *out)
{
    if (n == 0) {
        return;
    }
    avail = isfinite(avail) ? (avail > 0.0f ? avail : 0.0f) : 0.0f;
    size_t filler = filler_index(l, shown, n);
    for (size_t i = 0; i < n; i++) {
        if (pc_ui_dock_is_collapsed(l, shown[i])) {
            out[i] = strip;
        } else if (i == filler) {
            out[i] = 0.0f;
        } else {
            out[i] = pc_ui_dock_height(l, shown[i]);
        }
    }
    if (filler == (size_t)-1) {
        return;
    }
    float gaps = PC_UI_DOCK_GAP * (float)(n - 1);
    float min_fill = pc_ui_group_min_height(shown[filler]);
    float pref_fill = pc_ui_group_preferred_fill(shown[filler]);
    float used = sum_heights(out, n) + gaps;
    float deficit = used + pref_fill - avail;
    if (deficit < 0.0f) {
        deficit = 0.0f;
    }
    for (size_t i = filler; i-- > 0;) {
        if (deficit <= 0.0f) {
            break;
        }
        pc_ui_group g = shown[i];
        if (pc_ui_dock_is_collapsed(l, g) || l->has_height[g]) {
            continue;
        }
        float give = clamp_give(out[i], pc_ui_group_compact_height(g), deficit);
        out[i] -= give;
        deficit -= give;
    }
    used = sum_heights(out, n) + gaps;
    deficit = used + min_fill - avail;
    if (deficit < 0.0f) {
        deficit = 0.0f;
    }
    for (size_t i = filler; i-- > 0;) {
        if (deficit <= 0.0f) {
            break;
        }
        pc_ui_group g = shown[i];
        if (pc_ui_dock_is_collapsed(l, g)) {
            continue;
        }
        float give = clamp_give(out[i], pc_ui_group_min_height(g), deficit);
        out[i] -= give;
        deficit -= give;
    }
    float rest = avail - sum_heights(out, n) - gaps;
    out[filler] = rest > min_fill ? rest : min_fill;
}

void pc_ui_dock_resize(pc_ui_dock_layout *l, const pc_ui_group *shown, const float *heights, size_t n,
                       size_t i, float dy)
{
    if (!isfinite(dy) || dy == 0.0f || i >= n) {
        return;
    }
    size_t filler = filler_index(l, shown, n);
    /* The next expanded group below i. */
    size_t j = n;
    for (size_t k = i + 1; k < n; k++) {
        if (!pc_ui_dock_is_collapsed(l, shown[k])) {
            j = k;
            break;
        }
    }
    if (j == n || pc_ui_dock_is_collapsed(l, shown[i])) {
        return;
    }
    size_t grow;
    size_t shrink[PC_UI_GROUP_N];
    size_t n_shrink = 0;
    if (dy > 0.0f) {
        grow = i;
        for (size_t k = j; k < n; k++) {
            if (!pc_ui_dock_is_collapsed(l, shown[k])) {
                shrink[n_shrink++] = k;
            }
        }
    } else {
        grow = j;
        for (size_t k = i + 1; k-- > 0;) {
            if (!pc_ui_dock_is_collapsed(l, shown[k])) {
                shrink[n_shrink++] = k;
            }
            if (k == 0) {
                break;
            }
        }
    }
    /* Pin the other expanded groups at the heights they show (never the filler). */
    for (size_t k = 0; k < n; k++) {
        pc_ui_group o = shown[k];
        if (k != filler && !pc_ui_dock_is_collapsed(l, o) && !l->has_height[o]) {
            l->has_height[o] = true;
            l->heights[o] = heights[k];
        }
    }
    float left = fabsf(dy);
    for (size_t s = 0; s < n_shrink; s++) {
        if (left <= 0.0f) {
            break;
        }
        size_t k = shrink[s];
        pc_ui_group o = shown[k];
        float give = clamp_give(heights[k], pc_ui_group_min_height(o), left);
        left -= give;
        if (k != filler) {
            l->has_height[o] = true;
            l->heights[o] = heights[k] - give;
        }
    }
    if (grow != filler) {
        pc_ui_group o = shown[grow];
        l->has_height[o] = true;
        l->heights[o] = heights[grow] + (fabsf(dy) - left);
    }
}

bool pc_ui_dock_drop_before(const pc_ui_group *order, size_t n_order, const pc_ui_group *rect_group,
                            const float *rect_top, const float *rect_bottom, size_t n_rects,
                            pc_ui_group dragged, float y, pc_ui_group *out)
{
    bool hit_found = false;
    pc_ui_group hit = PC_UI_GROUP_COLOR;
    for (size_t i = 0; i < n_rects; i++) {
        float center = (rect_top[i] + rect_bottom[i]) * 0.5f;
        if (y < center) {
            hit = rect_group[i];
            hit_found = true;
            break;
        }
    }
    if (!hit_found) {
        return false; /* last */
    }
    if (hit == dragged) {
        *out = dragged;
        return true;
    }
    size_t a, b;
    if (position_group(order, n_order, dragged, &a) && position_group(order, n_order, hit, &b) &&
        b == a + 1) {
        *out = dragged;
        return true;
    }
    *out = hit;
    return true;
}

/* ---- serde ---- */

pc_status pc_ui_dock_layout_to_json(const pc_ui_dock_layout *l, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                    pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *order = yyjson_mut_arr(doc);
    for (size_t i = 0; i < l->n_order; i++) {
        yyjson_mut_arr_add_str(doc, order, pc_ui_group_key(l->order[i]));
    }
    yyjson_mut_obj_add_val(doc, o, "order", order);
    /* heights: a map keyed by group, in enum (BTreeMap) order. */
    yyjson_mut_val *heights = yyjson_mut_obj(doc);
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        pc_ui_group g = PC_UI_GROUP_ALL[i];
        if (l->has_height[g]) {
            yyjson_mut_obj_add_real(doc, heights, pc_ui_group_key(g), (double)l->heights[g]);
        }
    }
    yyjson_mut_obj_add_val(doc, o, "heights", heights);
    yyjson_mut_val *collapsed = yyjson_mut_arr(doc);
    for (size_t i = 0; i < l->n_collapsed; i++) {
        yyjson_mut_arr_add_str(doc, collapsed, pc_ui_group_key(l->collapsed[i]));
    }
    yyjson_mut_obj_add_val(doc, o, "collapsed", collapsed);
    /* floating: C-native extension (the reference's `docking` Layout tree is unported, §7); skipped
     * when empty. `floatRects` carries user-placed window rects for floating groups. */
    if (l->n_floating > 0) {
        yyjson_mut_val *floating = yyjson_mut_arr(doc);
        for (size_t i = 0; i < l->n_floating; i++) {
            yyjson_mut_arr_add_str(doc, floating, pc_ui_group_key(l->floating[i]));
        }
        yyjson_mut_obj_add_val(doc, o, "floating", floating);
        bool any_rect = false;
        for (size_t i = 0; i < l->n_floating; i++) {
            if (l->has_float_rect[l->floating[i]]) {
                any_rect = true;
                break;
            }
        }
        if (any_rect) {
            yyjson_mut_val *rects = yyjson_mut_obj(doc);
            for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
                pc_ui_group g = PC_UI_GROUP_ALL[i];
                if (!l->has_float_rect[g] || !contains_group(l->floating, l->n_floating, g)) {
                    continue;
                }
                yyjson_mut_val *arr = yyjson_mut_arr(doc);
                for (int k = 0; k < 4; k++) {
                    yyjson_mut_arr_add_real(doc, arr, (double)l->float_rects[g][k]);
                }
                yyjson_mut_obj_add_val(doc, rects, pc_ui_group_key(g), arr);
            }
            yyjson_mut_obj_add_val(doc, o, "floatRects", rects);
        }
    }
    /* hidden_tabs: skipped when empty. */
    bool any_hidden = false;
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        if (l->hidden_tabs[i].n > 0) {
            any_hidden = true;
            break;
        }
    }
    if (any_hidden) {
        yyjson_mut_val *ht = yyjson_mut_obj(doc);
        for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
            pc_ui_group g = PC_UI_GROUP_ALL[i];
            if (l->hidden_tabs[g].n == 0) {
                continue;
            }
            yyjson_mut_val *names = yyjson_mut_arr(doc);
            for (size_t j = 0; j < l->hidden_tabs[g].n; j++) {
                yyjson_mut_arr_add_strcpy(doc, names, l->hidden_tabs[g].names[j]);
            }
            yyjson_mut_obj_add_val(doc, ht, pc_ui_group_key(g), names);
        }
        yyjson_mut_obj_add_val(doc, o, "hidden_tabs", ht);
    }
    /* tabOrder: always present (may be empty). */
    yyjson_mut_val *to = yyjson_mut_obj(doc);
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        pc_ui_group g = PC_UI_GROUP_ALL[i];
        if (l->tab_order[g].n == 0) {
            continue;
        }
        yyjson_mut_val *names = yyjson_mut_arr(doc);
        for (size_t j = 0; j < l->tab_order[g].n; j++) {
            yyjson_mut_arr_add_strcpy(doc, names, l->tab_order[g].names[j]);
        }
        yyjson_mut_obj_add_val(doc, to, pc_ui_group_key(g), names);
    }
    yyjson_mut_obj_add_val(doc, o, "tabOrder", to);
    /* groups: the mutable panel -> group membership, present only once the layout is customized by a
     * cross-group move (C-native extension; the reference drives this through its craft_ui tree, §7).
     * Absent means "native" so uncustomized layouts round-trip byte-exact with the legacy format. */
    if (l->customized) {
        yyjson_mut_obj_add_bool(doc, o, "customized", true);
        yyjson_mut_val *groups = yyjson_mut_obj(doc);
        for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
            pc_ui_group g = PC_UI_GROUP_ALL[i];
            yyjson_mut_val *names = yyjson_mut_arr(doc);
            for (size_t j = 0; j < l->members[g].n; j++) {
                yyjson_mut_arr_add_strcpy(doc, names, l->members[g].names[j]);
            }
            yyjson_mut_obj_add_val(doc, groups, pc_ui_group_key(g), names);
        }
        yyjson_mut_obj_add_val(doc, o, "groups", groups);
    }
    *out = o;
    return PC_OK;
}

static void read_group_arr(yyjson_val *a, pc_ui_group *out, size_t cap, size_t *n_out)
{
    *n_out = 0;
    if (a == NULL || !yyjson_is_arr(a)) {
        return;
    }
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(a, idx, max, v)
    {
        pc_ui_group g;
        if (*n_out < cap && pc_ui_group_from_key(pc_json_as_str(v, NULL), &g)) {
            out[(*n_out)++] = g;
        }
    }
}

/* Read a { group: [names] } map into per-group name lists, dropping unknown groups / non-strings. */
static pc_status read_name_map(yyjson_val *m, pc_ui_name_list lists[PC_UI_GROUP_N], pc_error *err)
{
    if (m == NULL || !yyjson_is_obj(m)) {
        return PC_OK;
    }
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(m, idx, max, key, val)
    {
        pc_ui_group g;
        if (!pc_ui_group_from_key(yyjson_get_str(key), &g) || !yyjson_is_arr(val)) {
            continue;
        }
        size_t j, jmax;
        yyjson_val *name;
        yyjson_arr_foreach(val, j, jmax, name)
        {
            const char *s = pc_json_as_str(name, NULL);
            if (s != NULL) {
                if (strlen(s) > 65536) {
                    return pc_fail(err, PC_ERR_LIMIT, "dock tab name is too long");
                }
                if (!name_list_push(&lists[g], s)) {
                    return pc_fail(err, PC_ERR_NOMEM, "dock tab names");
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_ui_dock_layout_parse(pc_ui_dock_layout *out, yyjson_val *obj, pc_error *err)
{
    pc_ui_dock_layout next = {0};
    pc_ui_dock_layout *l = &next;
    if (obj == NULL || !yyjson_is_obj(obj)) {
        pc_ui_dock_layout_fini(out);
        *out = next;
        return PC_OK;
    }
    read_group_arr(pc_json_get(obj, "order"), l->order, PC_UI_DOCK_MAX_ORDER, &l->n_order);
    read_group_arr(pc_json_get(obj, "collapsed"), l->collapsed, PC_UI_GROUP_N, &l->n_collapsed);
    /* floating + floatRects (C-native, §7); lenient: drop unknown groups / malformed rects. */
    {
        pc_ui_group raw[PC_UI_GROUP_N];
        size_t nraw = 0;
        read_group_arr(pc_json_get(obj, "floating"), raw, PC_UI_GROUP_N, &nraw);
        for (size_t i = 0; i < nraw; i++) {
            if (!contains_group(l->floating, l->n_floating, raw[i])) {
                l->floating[l->n_floating++] = raw[i];
            }
        }
    }
    yyjson_val *rects = pc_json_get(obj, "floatRects");
    if (rects != NULL && yyjson_is_obj(rects)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(rects, idx, max, key, val)
        {
            pc_ui_group g;
            if (!pc_ui_group_from_key(yyjson_get_str(key), &g) || !yyjson_is_arr(val) ||
                yyjson_arr_size(val) != 4) {
                continue;
            }
            float r[4];
            bool ok = true;
            for (int k = 0; k < 4; k++) {
                double d;
                if (!pc_json_as_f64(yyjson_arr_get(val, (size_t)k), &d)) {
                    ok = false;
                    break;
                }
                r[k] = (float)d;
            }
            if (ok) {
                sanitize_float_rect(r);
                l->has_float_rect[g] = true;
                memcpy(l->float_rects[g], r, sizeof r);
            }
        }
    }
    yyjson_val *heights = pc_json_get(obj, "heights");
    if (heights != NULL && yyjson_is_obj(heights)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(heights, idx, max, key, val)
        {
            pc_ui_group g;
            double h;
            if (pc_ui_group_from_key(yyjson_get_str(key), &g) && pc_json_as_f64(val, &h)) {
                l->has_height[g] = true;
                l->heights[g] = (float)h;
            }
        }
    }
    pc_status status = read_name_map(pc_json_get(obj, "hidden_tabs"), l->hidden_tabs, err);
    if (status != PC_OK) {
        pc_ui_dock_layout_fini(&next);
        return status;
    }
    /* tabOrder, with the Rust "tab_order" alias; names capped at 16 (lenient_tab_order). */
    yyjson_val *to = pc_json_get(obj, "tabOrder");
    if (to == NULL) {
        to = pc_json_get(obj, "tab_order");
    }
    status = read_name_map(to, l->tab_order, err);
    if (status != PC_OK) {
        pc_ui_dock_layout_fini(&next);
        return status;
    }
    /* groups + customized (C-native): a flexible per-group panel membership. Lenient: drop unknown
     * groups and panel labels, keep each panel in at most one group (first wins). Absent/garbage
     * leaves the layout native. */
    yyjson_val *cust = pc_json_get(obj, "customized");
    yyjson_val *groups = pc_json_get(obj, "groups");
    if ((cust != NULL && yyjson_is_true(cust)) || (groups != NULL && yyjson_is_obj(groups))) {
        l->customized = true;
        if (groups != NULL && yyjson_is_obj(groups)) {
            size_t idx, max;
            yyjson_val *key, *val;
            yyjson_obj_foreach(groups, idx, max, key, val)
            {
                pc_ui_group g;
                if (!pc_ui_group_from_key(yyjson_get_str(key), &g) || !yyjson_is_arr(val)) {
                    continue;
                }
                size_t j, jmax;
                yyjson_val *name;
                yyjson_arr_foreach(val, j, jmax, name)
                {
                    const char *s = pc_json_as_str(name, NULL);
                    if (s == NULL || !pc_ui_dock_label_native_group(s, NULL)) {
                        continue;
                    }
                    if (pc_ui_dock_label_group(l, s, false, NULL)) {
                        continue; /* already placed in some group: a panel lives in exactly one */
                    }
                    if (!name_list_push_cap(&l->members[g], s, PC_UI_DOCK_MAX_MEMBERS)) {
                        pc_ui_dock_layout_fini(&next);
                        return pc_fail(err, PC_ERR_NOMEM, "dock group members");
                    }
                }
            }
        }
    }
    pc_ui_dock_layout_fini(out);
    *out = next;
    return PC_OK;
}
void pc_ui_dock_layout_from_json(pc_ui_dock_layout *out, yyjson_val *object)
{
    (void)pc_ui_dock_layout_parse(out, object, NULL);
}
