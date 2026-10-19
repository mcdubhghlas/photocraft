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
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        for (size_t j = 0; j < src->hidden_tabs[i].n; j++) {
            name_list_push(&dst->hidden_tabs[i], src->hidden_tabs[i].names[j]);
        }
        for (size_t j = 0; j < src->tab_order[i].n; j++) {
            name_list_push(&dst->tab_order[i], src->tab_order[i].names[j]);
        }
    }
}

bool pc_ui_dock_layout_eq(const pc_ui_dock_layout *a, const pc_ui_dock_layout *b)
{
    if (a->n_order != b->n_order || a->n_collapsed != b->n_collapsed) {
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
    for (size_t g = 0; g < PC_UI_GROUP_N; g++) {
        if (a->has_height[g] != b->has_height[g]) {
            return false;
        }
        if (a->has_height[g] && a->heights[g] != b->heights[g]) {
            return false;
        }
        if (!name_list_eq(&a->hidden_tabs[g], &b->hidden_tabs[g]) ||
            !name_list_eq(&a->tab_order[g], &b->tab_order[g])) {
            return false;
        }
    }
    return true;
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
    pc_ui_dock_layout_fini(out);
    *out = next;
    return PC_OK;
}
void pc_ui_dock_layout_from_json(pc_ui_dock_layout *out, yyjson_val *object)
{
    (void)pc_ui_dock_layout_parse(out, object, NULL);
}
