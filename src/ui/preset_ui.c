/* U5 preset panels state: the PresetUi data model and folder-reorder logic (see preset_ui.h). Port
 * of crates/ui-egui/src/preset_panels.rs's `PresetUi` state, display_order and move_group. */
#include "ui/preset_ui.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"

static char *dup_str(const char *s)
{
    return strdup(s != NULL ? s : "");
}

/* ------------------------------------------------------------------ lifecycle */

void pc_ui_preset_ui_init(pc_ui_preset_ui *p)
{
    if (p == NULL) {
        return;
    }
    memset(p, 0, sizeof *p);
    p->custom_shape = dup_str("");
}

static void free_names(char **names, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
}

void pc_ui_preset_ui_fini(pc_ui_preset_ui *p)
{
    if (p == NULL) {
        return;
    }
    for (size_t i = 0; i < p->n_sizes; i++) {
        free(p->sizes[i].panel);
    }
    free(p->sizes);
    free_names(p->toggled, p->n_toggled);
    for (size_t i = 0; i < p->n_selected; i++) {
        free(p->selected[i].panel);
        free(p->selected[i].name);
    }
    free(p->selected);
    for (size_t i = 0; i < p->n_group_order; i++) {
        free(p->group_order[i].panel);
        free_names(p->group_order[i].names, p->group_order[i].n);
    }
    free(p->group_order);
    free(p->custom_shape);
    free(p->rename_panel);
    free(p->rename_key);
    free(p->rename_text);
    memset(p, 0, sizeof *p);
}

/* ------------------------------------------------------------------ sorted maps/sets */

/* Insert/replace `panel`'s thumbnail size, keeping the array sorted by panel. */
static void sizes_set(pc_ui_preset_ui *p, const char *panel, float size)
{
    for (size_t i = 0; i < p->n_sizes; i++) {
        int c = strcmp(p->sizes[i].panel, panel);
        if (c == 0) {
            p->sizes[i].size = size;
            return;
        }
        if (c > 0) {
            pc_ui_preset_size *grown = realloc(p->sizes, (p->n_sizes + 1) * sizeof *grown);
            if (grown == NULL) {
                return;
            }
            p->sizes = grown;
            memmove(&p->sizes[i + 1], &p->sizes[i], (p->n_sizes - i) * sizeof *grown);
            p->sizes[i].panel = dup_str(panel);
            p->sizes[i].size = size;
            p->n_sizes++;
            return;
        }
    }
    pc_ui_preset_size *grown = realloc(p->sizes, (p->n_sizes + 1) * sizeof *grown);
    if (grown == NULL) {
        return;
    }
    p->sizes = grown;
    p->sizes[p->n_sizes].panel = dup_str(panel);
    p->sizes[p->n_sizes].size = size;
    p->n_sizes++;
}

/* Insert `name` into the sorted set `toggled` (no duplicates). */
static void toggled_insert(pc_ui_preset_ui *p, const char *name)
{
    for (size_t i = 0; i < p->n_toggled; i++) {
        int c = strcmp(p->toggled[i], name);
        if (c == 0) {
            return;
        }
        if (c > 0) {
            char **grown = realloc(p->toggled, (p->n_toggled + 1) * sizeof *grown);
            if (grown == NULL) {
                return;
            }
            p->toggled = grown;
            memmove(&p->toggled[i + 1], &p->toggled[i], (p->n_toggled - i) * sizeof *grown);
            p->toggled[i] = dup_str(name);
            p->n_toggled++;
            return;
        }
    }
    char **grown = realloc(p->toggled, (p->n_toggled + 1) * sizeof *grown);
    if (grown == NULL) {
        return;
    }
    p->toggled = grown;
    p->toggled[p->n_toggled++] = dup_str(name);
}

/* Insert/replace `panel`'s selected preset, sorted by panel. */
static void selected_set(pc_ui_preset_ui *p, const char *panel, const char *name)
{
    for (size_t i = 0; i < p->n_selected; i++) {
        int c = strcmp(p->selected[i].panel, panel);
        if (c == 0) {
            free(p->selected[i].name);
            p->selected[i].name = dup_str(name);
            return;
        }
        if (c > 0) {
            pc_ui_preset_sel *grown = realloc(p->selected, (p->n_selected + 1) * sizeof *grown);
            if (grown == NULL) {
                return;
            }
            p->selected = grown;
            memmove(&p->selected[i + 1], &p->selected[i], (p->n_selected - i) * sizeof *grown);
            p->selected[i].panel = dup_str(panel);
            p->selected[i].name = dup_str(name);
            p->n_selected++;
            return;
        }
    }
    pc_ui_preset_sel *grown = realloc(p->selected, (p->n_selected + 1) * sizeof *grown);
    if (grown == NULL) {
        return;
    }
    p->selected = grown;
    p->selected[p->n_selected].panel = dup_str(panel);
    p->selected[p->n_selected].name = dup_str(name);
    p->n_selected++;
}

/* The group_order entry for `panel`, or NULL. */
static pc_ui_preset_group_order *group_order_find(const pc_ui_preset_ui *p, const char *panel)
{
    for (size_t i = 0; i < p->n_group_order; i++) {
        if (strcmp(p->group_order[i].panel, panel) == 0) {
            return &p->group_order[i];
        }
    }
    return NULL;
}

/* Remove `panel`'s group_order entry, if any. */
static void group_order_remove(pc_ui_preset_ui *p, const char *panel)
{
    for (size_t i = 0; i < p->n_group_order; i++) {
        if (strcmp(p->group_order[i].panel, panel) == 0) {
            free(p->group_order[i].panel);
            free_names(p->group_order[i].names, p->group_order[i].n);
            memmove(&p->group_order[i], &p->group_order[i + 1],
                    (p->n_group_order - i - 1) * sizeof p->group_order[0]);
            p->n_group_order--;
            return;
        }
    }
}

/* Set `panel`'s folder order to a copy of `names` (n entries), keeping the array sorted by panel.
 * Takes copies; the caller keeps ownership of `names`. */
static void group_order_set(pc_ui_preset_ui *p, const char *panel, const char *const *names, size_t n)
{
    char **copy = NULL;
    if (n > 0) {
        copy = malloc(n * sizeof *copy);
        if (copy == NULL) {
            return;
        }
        for (size_t i = 0; i < n; i++) {
            copy[i] = dup_str(names[i]);
        }
    }
    pc_ui_preset_group_order *e = group_order_find(p, panel);
    if (e != NULL) {
        free_names(e->names, e->n);
        e->names = copy;
        e->n = n;
        return;
    }
    size_t at = p->n_group_order;
    for (size_t i = 0; i < p->n_group_order; i++) {
        if (strcmp(p->group_order[i].panel, panel) > 0) {
            at = i;
            break;
        }
    }
    pc_ui_preset_group_order *grown = realloc(p->group_order, (p->n_group_order + 1) * sizeof *grown);
    if (grown == NULL) {
        free_names(copy, n);
        return;
    }
    p->group_order = grown;
    memmove(&p->group_order[at + 1], &p->group_order[at],
            (p->n_group_order - at) * sizeof *grown);
    p->group_order[at].panel = dup_str(panel);
    p->group_order[at].names = copy;
    p->group_order[at].n = n;
    p->n_group_order++;
}

/* ------------------------------------------------------------------ copy / eq */

void pc_ui_preset_ui_copy(pc_ui_preset_ui *dst, const pc_ui_preset_ui *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    pc_ui_preset_ui_fini(dst);
    pc_ui_preset_ui_init(dst);
    dst->styles = src->styles;
    dst->shapes = src->shapes;
    dst->tool_presets = src->tool_presets;
    dst->clone_source = src->clone_source;
    dst->current_tool_only = src->current_tool_only;
    free(dst->custom_shape);
    dst->custom_shape = dup_str(src->custom_shape);
    for (size_t i = 0; i < src->n_sizes; i++) {
        sizes_set(dst, src->sizes[i].panel, src->sizes[i].size);
    }
    for (size_t i = 0; i < src->n_toggled; i++) {
        toggled_insert(dst, src->toggled[i]);
    }
    for (size_t i = 0; i < src->n_selected; i++) {
        selected_set(dst, src->selected[i].panel, src->selected[i].name);
    }
    for (size_t i = 0; i < src->n_group_order; i++) {
        group_order_set(dst, src->group_order[i].panel,
                        (const char *const *)src->group_order[i].names, src->group_order[i].n);
    }
    dst->has_renaming = src->has_renaming;
    if (src->has_renaming) {
        dst->rename_panel = dup_str(src->rename_panel);
        dst->rename_key = dup_str(src->rename_key);
        dst->rename_text = dup_str(src->rename_text);
    }
}

static bool names_eq(char *const *a, size_t na, char *const *b, size_t nb)
{
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; i++) {
        if (strcmp(a[i], b[i]) != 0) {
            return false;
        }
    }
    return true;
}

bool pc_ui_preset_ui_eq(const pc_ui_preset_ui *a, const pc_ui_preset_ui *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    if (a->styles != b->styles || a->shapes != b->shapes || a->tool_presets != b->tool_presets ||
        a->clone_source != b->clone_source || a->current_tool_only != b->current_tool_only ||
        strcmp(a->custom_shape, b->custom_shape) != 0 || a->n_sizes != b->n_sizes ||
        a->n_toggled != b->n_toggled || a->n_selected != b->n_selected ||
        a->n_group_order != b->n_group_order) {
        return false;
    }
    for (size_t i = 0; i < a->n_sizes; i++) {
        if (strcmp(a->sizes[i].panel, b->sizes[i].panel) != 0 || a->sizes[i].size != b->sizes[i].size) {
            return false;
        }
    }
    for (size_t i = 0; i < a->n_toggled; i++) {
        if (strcmp(a->toggled[i], b->toggled[i]) != 0) {
            return false;
        }
    }
    for (size_t i = 0; i < a->n_selected; i++) {
        if (strcmp(a->selected[i].panel, b->selected[i].panel) != 0 ||
            strcmp(a->selected[i].name, b->selected[i].name) != 0) {
            return false;
        }
    }
    for (size_t i = 0; i < a->n_group_order; i++) {
        if (strcmp(a->group_order[i].panel, b->group_order[i].panel) != 0 ||
            !names_eq(a->group_order[i].names, a->group_order[i].n, b->group_order[i].names,
                      b->group_order[i].n)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ logic */

const char *pc_ui_preset_ui_shape(const pc_ui_preset_ui *p)
{
    if (p == NULL || p->custom_shape == NULL || p->custom_shape[0] == '\0') {
        return "Heart";
    }
    return p->custom_shape;
}

static size_t name_index(const char *const *names, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], name) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static bool contains_idx(const size_t *out, size_t n, size_t v)
{
    for (size_t i = 0; i < n; i++) {
        if (out[i] == v) {
            return true;
        }
    }
    return false;
}

size_t pc_ui_preset_ui_display_order(const pc_ui_preset_ui *p, const char *panel,
                                     const char *const *names, size_t n_names, size_t *out)
{
    size_t n_out = 0;
    const pc_ui_preset_group_order *e = p != NULL ? group_order_find(p, panel) : NULL;
    if (e != NULL) {
        for (size_t k = 0; k < e->n; k++) {
            size_t i = name_index(names, n_names, e->names[k]);
            if (i != (size_t)-1 && !contains_idx(out, n_out, i)) {
                out[n_out++] = i;
            }
        }
    }
    for (size_t i = 0; i < n_names; i++) {
        if (!contains_idx(out, n_out, i)) {
            out[n_out++] = i;
        }
    }
    return n_out;
}

void pc_ui_preset_ui_move_group(pc_ui_preset_ui *p, const char *panel, const char *const *names,
                                size_t n_names, const char *from, const char *before)
{
    if (p == NULL || from == NULL || n_names == 0) {
        return;
    }
    if (before != NULL && strcmp(before, from) == 0) {
        return;
    }
    /* The current display order, as folder names. With n_names > 0 display_order always returns
     * n_names entries, so idx and order below are non-empty and never NULL. */
    size_t *idx = malloc(n_names * sizeof *idx);
    if (idx == NULL) {
        return;
    }
    size_t n = pc_ui_preset_ui_display_order(p, panel, names, n_names, idx);
    const char **order = malloc(n * sizeof *order);
    if (order == NULL) {
        free(idx);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        order[i] = names[idx[i]];
    }
    free(idx);
    size_t at = name_index(order, n, from);
    if (at == (size_t)-1) {
        free(order);
        return;
    }
    /* Remove `from`. */
    const char *moved = order[at];
    memmove(&order[at], &order[at + 1], (n - at - 1) * sizeof *order);
    n--;
    /* Insert before `before` (or at the end). */
    size_t to = n;
    if (before != NULL) {
        size_t bi = name_index(order, n, before);
        if (bi != (size_t)-1) {
            to = bi;
        }
    }
    memmove(&order[to + 1], &order[to], (n - to) * sizeof *order);
    order[to] = moved;
    n++;
    /* Store only a real reorder; the library's own order is "no entry". */
    bool same = (n == n_names);
    for (size_t i = 0; same && i < n; i++) {
        if (strcmp(order[i], names[i]) != 0) {
            same = false;
        }
    }
    if (same) {
        group_order_remove(p, panel);
    } else {
        group_order_set(p, panel, order, n);
    }
    free(order);
}

/* ------------------------------------------------------------------ browser accessors */

float pc_ui_preset_ui_size(const pc_ui_preset_ui *p, const char *panel)
{
    if (p == NULL || panel == NULL) {
        return PC_UI_PRESET_DEFAULT_SIZE;
    }
    for (size_t i = 0; i < p->n_sizes; i++) {
        if (strcmp(p->sizes[i].panel, panel) == 0) {
            return p->sizes[i].size;
        }
    }
    return PC_UI_PRESET_DEFAULT_SIZE;
}

void pc_ui_preset_ui_set_size(pc_ui_preset_ui *p, const char *panel, float size)
{
    if (p == NULL || panel == NULL) {
        return;
    }
    if (!(size >= PC_UI_PRESET_MIN_THUMB)) { /* also catches NaN */
        size = PC_UI_PRESET_MIN_THUMB;
    }
    if (size > 96.0f) {
        size = 96.0f;
    }
    sizes_set(p, panel, size);
}

const char *pc_ui_preset_ui_selected(const pc_ui_preset_ui *p, const char *panel)
{
    if (p == NULL || panel == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < p->n_selected; i++) {
        if (strcmp(p->selected[i].panel, panel) == 0) {
            return p->selected[i].name;
        }
    }
    return NULL;
}

void pc_ui_preset_ui_set_selected(pc_ui_preset_ui *p, const char *panel, const char *name)
{
    if (p == NULL || panel == NULL || name == NULL) {
        return;
    }
    selected_set(p, panel, name);
}

void pc_ui_preset_ui_clear_selected(pc_ui_preset_ui *p, const char *panel)
{
    if (p == NULL || panel == NULL) {
        return;
    }
    for (size_t i = 0; i < p->n_selected; i++) {
        if (strcmp(p->selected[i].panel, panel) == 0) {
            free(p->selected[i].panel);
            free(p->selected[i].name);
            memmove(&p->selected[i], &p->selected[i + 1],
                    (p->n_selected - i - 1) * sizeof p->selected[0]);
            p->n_selected--;
            return;
        }
    }
}

/* key "<panel>/<name>" used by the toggled set. */
static bool group_key(char *buf, size_t cap, const char *panel, const char *name)
{
    int n = snprintf(buf, cap, "%s/%s", panel != NULL ? panel : "", name != NULL ? name : "");
    return n > 0 && (size_t)n < cap;
}

static bool toggled_has(const pc_ui_preset_ui *p, const char *key)
{
    for (size_t i = 0; i < p->n_toggled; i++) {
        if (strcmp(p->toggled[i], key) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_ui_preset_ui_group_open(const pc_ui_preset_ui *p, const char *panel, size_t gi,
                                const char *name, bool swatches_all)
{
    bool def = (gi == 0 || swatches_all);
    if (p == NULL || panel == NULL || name == NULL) {
        return def;
    }
    char key[512];
    if (!group_key(key, sizeof key, panel, name)) {
        return def;
    }
    return def != toggled_has(p, key);
}

void pc_ui_preset_ui_toggle_group(pc_ui_preset_ui *p, const char *panel, const char *name)
{
    if (p == NULL || panel == NULL || name == NULL) {
        return;
    }
    char key[512];
    if (!group_key(key, sizeof key, panel, name)) {
        return;
    }
    for (size_t i = 0; i < p->n_toggled; i++) {
        if (strcmp(p->toggled[i], key) == 0) {
            free(p->toggled[i]);
            memmove(&p->toggled[i], &p->toggled[i + 1], (p->n_toggled - i - 1) * sizeof p->toggled[0]);
            p->n_toggled--;
            return;
        }
    }
    toggled_insert(p, key);
}

void pc_ui_preset_ui_set_rename(pc_ui_preset_ui *p, const char *panel, const char *key,
                                const char *text)
{
    if (p == NULL) {
        return;
    }
    pc_ui_preset_ui_clear_rename(p);
    p->has_renaming = true;
    p->rename_panel = dup_str(panel);
    p->rename_key = dup_str(key);
    p->rename_text = dup_str(text);
}

void pc_ui_preset_ui_clear_rename(pc_ui_preset_ui *p)
{
    if (p == NULL) {
        return;
    }
    free(p->rename_panel);
    free(p->rename_key);
    free(p->rename_text);
    p->rename_panel = p->rename_key = p->rename_text = NULL;
    p->has_renaming = false;
}

const char *pc_ui_preset_ui_rename_key(const pc_ui_preset_ui *p, const char *panel,
                                       const char **text_out)
{
    if (p == NULL || !p->has_renaming || panel == NULL || p->rename_panel == NULL ||
        strcmp(p->rename_panel, panel) != 0) {
        return NULL;
    }
    if (text_out != NULL) {
        *text_out = p->rename_text != NULL ? p->rename_text : "";
    }
    return p->rename_key != NULL ? p->rename_key : "";
}

void pc_ui_preset_ui_set_rename_text(pc_ui_preset_ui *p, const char *text)
{
    if (p == NULL || !p->has_renaming) {
        return;
    }
    char *copy = dup_str(text);
    free(p->rename_text);
    p->rename_text = copy;
}

/* ------------------------------------------------------------------ Window-menu helpers */

static const char *const PRESET_PANELS[] = {
    "window.panel.gradients", "window.panel.patterns",    "window.panel.styles",
    "window.panel.shapes",    "window.panel.toolPresets", "window.panel.cloneSource"};

bool pc_ui_preset_ui_handles(const char *id)
{
    if (id == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof PRESET_PANELS / sizeof PRESET_PANELS[0]; i++) {
        if (strcmp(id, PRESET_PANELS[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_ui_preset_ui_color_tab(const char *id, size_t *tab)
{
    if (id == NULL) {
        return false;
    }
    if (strcmp(id, "window.panel.gradients") == 0) {
        if (tab != NULL) {
            *tab = 2;
        }
        return true;
    }
    if (strcmp(id, "window.panel.patterns") == 0) {
        if (tab != NULL) {
            *tab = 3;
        }
        return true;
    }
    return false;
}

bool *pc_ui_preset_ui_float_flag(pc_ui_preset_ui *p, const char *id)
{
    if (p == NULL || id == NULL) {
        return NULL;
    }
    if (strcmp(id, "window.panel.styles") == 0) {
        return &p->styles;
    }
    if (strcmp(id, "window.panel.shapes") == 0) {
        return &p->shapes;
    }
    if (strcmp(id, "window.panel.toolPresets") == 0) {
        return &p->tool_presets;
    }
    if (strcmp(id, "window.panel.cloneSource") == 0) {
        return &p->clone_source;
    }
    return NULL;
}

/* ------------------------------------------------------------------ serde */

pc_status pc_ui_preset_ui_to_json(const pc_ui_preset_ui *p, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err)
{
    if (p == NULL || doc == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "preset_ui_to_json: null");
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "preset_ui");
    }
    yyjson_mut_obj_add_bool(doc, o, "styles", p->styles);
    yyjson_mut_obj_add_bool(doc, o, "shapes", p->shapes);
    yyjson_mut_obj_add_bool(doc, o, "tool_presets", p->tool_presets);
    yyjson_mut_obj_add_bool(doc, o, "clone_source", p->clone_source);
    yyjson_mut_val *sizes = yyjson_mut_obj(doc);
    for (size_t i = 0; i < p->n_sizes; i++) {
        yyjson_mut_obj_add_real(doc, sizes, p->sizes[i].panel, p->sizes[i].size);
    }
    yyjson_mut_obj_add_val(doc, o, "sizes", sizes);
    yyjson_mut_val *toggled = yyjson_mut_arr(doc);
    for (size_t i = 0; i < p->n_toggled; i++) {
        yyjson_mut_arr_add_strcpy(doc, toggled, p->toggled[i]);
    }
    yyjson_mut_obj_add_val(doc, o, "toggled", toggled);
    yyjson_mut_val *selected = yyjson_mut_obj(doc);
    for (size_t i = 0; i < p->n_selected; i++) {
        yyjson_mut_obj_add_strcpy(doc, selected, p->selected[i].panel, p->selected[i].name);
    }
    yyjson_mut_obj_add_val(doc, o, "selected", selected);
    yyjson_mut_obj_add_bool(doc, o, "current_tool_only", p->current_tool_only);
    yyjson_mut_obj_add_strcpy(doc, o, "custom_shape", p->custom_shape);
    if (p->n_group_order > 0) {
        yyjson_mut_val *go = yyjson_mut_obj(doc);
        for (size_t i = 0; i < p->n_group_order; i++) {
            yyjson_mut_val *names = yyjson_mut_arr(doc);
            for (size_t j = 0; j < p->group_order[i].n; j++) {
                yyjson_mut_arr_add_strcpy(doc, names, p->group_order[i].names[j]);
            }
            yyjson_mut_obj_add_val(doc, go, p->group_order[i].panel, names);
        }
        yyjson_mut_obj_add_val(doc, o, "groupOrder", go);
    }
    *out = o;
    return PC_OK;
}

void pc_ui_preset_ui_from_json(pc_ui_preset_ui *p, yyjson_val *obj)
{
    if (p == NULL) {
        return;
    }
    pc_ui_preset_ui_fini(p);
    pc_ui_preset_ui_init(p);
    if (obj == NULL || !yyjson_is_obj(obj)) {
        return;
    }
    (void)pc_json_as_bool(pc_json_get(obj, "styles"), &p->styles);
    (void)pc_json_as_bool(pc_json_get(obj, "shapes"), &p->shapes);
    (void)pc_json_as_bool(pc_json_get(obj, "tool_presets"), &p->tool_presets);
    (void)pc_json_as_bool(pc_json_get(obj, "clone_source"), &p->clone_source);
    (void)pc_json_as_bool(pc_json_get(obj, "current_tool_only"), &p->current_tool_only);
    const char *shape = pc_json_as_str(pc_json_get(obj, "custom_shape"), NULL);
    if (shape != NULL) {
        free(p->custom_shape);
        p->custom_shape = dup_str(shape);
    }
    yyjson_val *sizes = pc_json_get(obj, "sizes");
    if (sizes != NULL && yyjson_is_obj(sizes)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(sizes, idx, max, key, val)
        {
            double v;
            const char *k = yyjson_get_str(key);
            if (k != NULL && pc_json_as_f64(val, &v)) {
                sizes_set(p, k, (float)v);
            }
        }
    }
    yyjson_val *toggled = pc_json_get(obj, "toggled");
    if (toggled != NULL && yyjson_is_arr(toggled)) {
        size_t j, jmax;
        yyjson_val *name;
        yyjson_arr_foreach(toggled, j, jmax, name)
        {
            const char *s = pc_json_as_str(name, NULL);
            if (s != NULL) {
                toggled_insert(p, s);
            }
        }
    }
    yyjson_val *selected = pc_json_get(obj, "selected");
    if (selected != NULL && yyjson_is_obj(selected)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(selected, idx, max, key, val)
        {
            const char *k = yyjson_get_str(key);
            const char *s = pc_json_as_str(val, NULL);
            if (k != NULL && s != NULL) {
                selected_set(p, k, s);
            }
        }
    }
    /* groupOrder (lenient), with the "group_order" alias. */
    yyjson_val *go = pc_json_get(obj, "groupOrder");
    if (go == NULL) {
        go = pc_json_get(obj, "group_order");
    }
    if (go != NULL && yyjson_is_obj(go)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(go, idx, max, key, val)
        {
            const char *panel = yyjson_get_str(key);
            if (panel == NULL || !yyjson_is_arr(val)) {
                continue;
            }
            const char **names = NULL;
            size_t n = 0, cap = 0;
            size_t j, jmax;
            yyjson_val *name;
            yyjson_arr_foreach(val, j, jmax, name)
            {
                if (n >= PC_UI_PRESET_MAX_GROUP_ORDER) {
                    break;
                }
                const char *s = pc_json_as_str(name, NULL);
                if (s == NULL) {
                    continue;
                }
                if (n == cap) {
                    size_t ncap = cap == 0 ? 8 : cap * 2;
                    const char **grown = realloc(names, ncap * sizeof *grown);
                    if (grown == NULL) {
                        break;
                    }
                    names = grown;
                    cap = ncap;
                }
                names[n++] = s;
            }
            if (n > 0) {
                group_order_set(p, panel, names, n);
            }
            free(names);
        }
    }
}
