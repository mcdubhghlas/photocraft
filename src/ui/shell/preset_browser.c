/* U5 preset-browser rendering: see preset_browser.h. Draws against the PresetUi state model
 * (src/ui/preset_ui.c) with Nuklear, and dispatches generic library edits through the engine. */
#include "preset_browser.h"

#include "shell_internal.h"
#include "pattern_picker_ui.h"
#include "ui/i18n.h"
#include "ui/preset_swatch.h"
#include "ui/preset_thumb.h"
#include "ui/preset_ui.h"
#include "ui/widgets.h"
#include "engine/engine_internal.h"
#include "engine/presets.h"
#include "base/json.h"
#include "raster/pixel.h"

#include <blake3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Folders a browser will ever reorder in one frame (hostile-library guard; real libraries are
 * tiny). Beyond this the extra folders still draw and toggle; only reorder stops recording. */
#define PC_UI_BROWSER_MAX_HEADERS 512

/* ------------------------------------------------------------------ small helpers */

static pc_ui_preset_ui *presets(pc_ui_shell_app *app)
{
    return &pc_ui_shell_app_state(app)->presets_ui;
}

static void set_status(pc_ui_shell_app *app, const char *message)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    char *copy = pc_strndup(message ? message : "", message ? strlen(message) : 0);
    if (copy != NULL) {
        free(ui->status);
        ui->status = copy;
        ui->status_error = true;
    }
}

/* Execute a command synchronously (library edits do not touch the document tree being iterated, so
 * this is safe mid-frame, as the list query already is). Returns the result (caller fini) or NULL on
 * failure (status set). */
static bool run_sync(pc_ui_shell_app *app, const char *id, yyjson_mut_doc *pdoc, yyjson_mut_val *proot,
                     pc_json_value *out)
{
    pc_error err;
    yyjson_val *params = NULL;
    yyjson_doc *idoc = NULL;
    if (pdoc != NULL && proot != NULL) {
        /* Convert the mutable params into an immutable doc the command layer accepts. */
        yyjson_mut_doc_set_root(pdoc, proot);
        char *json = yyjson_mut_write(pdoc, 0, NULL);
        if (json != NULL) {
            idoc = yyjson_read(json, strlen(json), 0);
            free(json);
        }
        params = idoc != NULL ? yyjson_doc_get_root(idoc) : NULL;
    }
    pc_status st = pc_ui_shell_app_invoke(app, id, params, out, &err);
    if (idoc != NULL) {
        yyjson_doc_free(idoc);
    }
    if (st != PC_OK) {
        set_status(app, err.msg);
        return false;
    }
    return true;
}

static yyjson_mut_doc *new_params(yyjson_mut_val **root)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    *root = d != NULL ? yyjson_mut_obj(d) : NULL;
    if (*root == NULL) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    return d;
}

/* ------------------------------------------------------------------ group views from JSON */

void pc_ui_preset_group_data_fini(pc_ui_preset_group_data *d)
{
    if (d == NULL) {
        return;
    }
    free(d->groups);
    free(d->items);
    d->groups = NULL;
    d->items = NULL;
    d->ngroups = 0;
}

bool pc_ui_preset_groups_from_json(yyjson_mut_val *root, const char *items_key, const char *key_field,
                                   pc_ui_preset_group_data *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    yyjson_mut_val *groups = yyjson_mut_obj_get(root, "groups");
    if (!yyjson_mut_is_arr(groups)) {
        return false;
    }
    size_t ng = yyjson_mut_arr_size(groups);
    size_t total = 0;
    for (size_t g = 0; g < ng; g++) {
        total += yyjson_mut_arr_size(yyjson_mut_obj_get(yyjson_mut_arr_get(groups, g), items_key));
    }
    out->groups = ng > 0 ? calloc(ng, sizeof *out->groups) : NULL;
    out->items = total > 0 ? calloc(total, sizeof *out->items) : NULL;
    if ((ng > 0 && out->groups == NULL) || (total > 0 && out->items == NULL)) {
        pc_ui_preset_group_data_fini(out);
        return false;
    }
    out->ngroups = ng;
    size_t it = 0;
    for (size_t g = 0; g < ng; g++) {
        yyjson_mut_val *grp = yyjson_mut_arr_get(groups, g);
        const char *nm = yyjson_mut_get_str(yyjson_mut_obj_get(grp, "name"));
        out->groups[g].name = nm != NULL ? nm : "";
        yyjson_mut_val *arr = yyjson_mut_obj_get(grp, items_key);
        size_t n = yyjson_mut_arr_size(arr);
        out->groups[g].items = out->items + it;
        out->groups[g].n = 0;
        for (size_t i = 0; i < n; i++) {
            yyjson_mut_val *row = yyjson_mut_arr_get(arr, i);
            const char *key = yyjson_mut_get_str(yyjson_mut_obj_get(row, key_field));
            if (key == NULL) {
                continue;
            }
            const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(row, "name"));
            out->items[it].key = key;
            out->items[it].name = label != NULL ? label : key;
            it++;
            out->groups[g].n++;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ generic library edits */

/* <prefix>.edit with {action, ...}. Returns the result root in *out (may be NULL) when ok. */
static bool edit(pc_ui_shell_app *app, const char *prefix, const char *action, const char *k1,
                 const char *v1, const char *k2, const char *v2, pc_json_value *out)
{
    yyjson_mut_val *root;
    yyjson_mut_doc *d = new_params(&root);
    if (d == NULL) {
        set_status(app, "Not enough memory for this action");
        return false;
    }
    yyjson_mut_obj_add_str(d, root, "action", action);
    if (k1 != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, k1, v1 != NULL ? v1 : "");
    }
    if (k2 != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, k2, v2 != NULL ? v2 : "");
    }
    char id[128];
    (void)snprintf(id, sizeof id, "%s.edit", prefix);
    bool ok = run_sync(app, id, d, root, out);
    yyjson_mut_doc_free(d);
    return ok;
}

/* Rename (preset or "group:<name>") committed from the inline field. */
static void commit_rename(pc_ui_shell_app *app, const char *panel, const char *prefix, const char *key,
                          const char *text)
{
    /* Trim surrounding ASCII space (reference trims and rejects an empty result). */
    char buf[256];
    (void)snprintf(buf, sizeof buf, "%s", text != NULL ? text : "");
    char *s = buf;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    if (len == 0) {
        return;
    }
    if (strncmp(key, "group:", 6) == 0) {
        (void)edit(app, prefix, "renameGroup", "group", key + 6, "name", s, NULL);
    } else {
        pc_json_value out = PC_JSON_VALUE_INIT;
        if (edit(app, prefix, "rename", "preset", key, "name", s, &out)) {
            /* Keep the selection on the renamed preset. */
            const char *sel = pc_ui_preset_ui_selected(presets(app), panel);
            if (sel != NULL && strcmp(sel, key) == 0) {
                pc_ui_preset_ui_set_selected(presets(app), panel, s);
            }
        }
        pc_json_value_fini(&out);
    }
}

static void delete_preset(pc_ui_shell_app *app, const char *panel, const char *prefix, const char *key)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (edit(app, prefix, "delete", "preset", key, NULL, NULL, &out)) {
        const char *sel = pc_ui_preset_ui_selected(presets(app), panel);
        if (sel != NULL && strcmp(sel, key) == 0) {
            pc_ui_preset_ui_clear_selected(presets(app), panel);
        }
    }
    pc_json_value_fini(&out);
}

static void new_group(pc_ui_shell_app *app, const char *panel, const char *prefix)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (edit(app, prefix, "newGroup", "name", "Group", NULL, NULL, &out)) {
        const char *g = yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "group"));
        if (g != NULL) {
            char key[256];
            (void)snprintf(key, sizeof key, "group:%s", g);
            pc_ui_preset_ui_set_rename(presets(app), panel, key, g);
        }
    }
    pc_json_value_fini(&out);
}

static void new_preset(pc_ui_shell_app *app, const pc_ui_preset_browser *info)
{
    yyjson_mut_val *root;
    yyjson_mut_doc *d = new_params(&root);
    if (d == NULL) {
        set_status(app, "Not enough memory for this action");
        return;
    }
    if (info->new_name != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "name", info->new_name);
    }
    char id[128];
    (void)snprintf(id, sizeof id, "%s.new", info->prefix);
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (run_sync(app, id, d, root, &out)) {
        yyjson_mut_val *r = pc_json_value_root(&out);
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(r, "name"));
        if (name == NULL) {
            name = yyjson_mut_get_str(yyjson_mut_obj_get(r, "pattern"));
        }
        if (name != NULL) {
            pc_ui_preset_ui_set_selected(presets(app), info->panel, name);
            if (info->rename_after_new) {
                pc_ui_preset_ui_set_rename(presets(app), info->panel, name, name);
            }
        }
    }
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
}

/* ------------------------------------------------------------------ folder reorder */

typedef struct header_rec {
    size_t gi; /* library index */
    float cy;  /* centre y (screen) */
    const char *name;
} header_rec;

/* The folder a header dragged to screen-y `y` lands before (library index, or (size_t)-1 == last).
 * `recs` is in display order; mirrors preset_panels.rs::group_drop_before / dock::drop_before. */
static size_t group_drop_before(const header_rec *recs, size_t n, size_t from, float y)
{
    size_t hit = (size_t)-1, hit_pos = 0, from_pos = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].gi == from) {
            from_pos = i;
        }
    }
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (y < recs[i].cy) {
            hit = recs[i].gi;
            hit_pos = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return (size_t)-1;
    }
    if (hit == from) {
        return from;
    }
    if (hit_pos == from_pos + 1) {
        return from; /* dropping just after itself is a no-op */
    }
    return hit;
}

/* ------------------------------------------------------------------ the browser */

bool pc_ui_shell_preset_browser(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                                const pc_ui_preset_browser *info, pc_ui_preset_event *ev)
{
    (void)runtime;
    if (ev != NULL) {
        ev->kind = PC_UI_PRESET_EV_NONE;
    }
    if (app == NULL || info == NULL || info->groups == NULL) {
        return false;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_input *in = &ctx->input;
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    pc_ui_preset_ui *st = presets(app);
    const char *panel = info->panel;

    float size = pc_ui_preset_ui_size(st, panel);
    bool list = size <= PC_UI_PRESET_MIN_THUMB + 0.5f;
    const char *selected = pc_ui_preset_ui_selected(st, panel);
    pc_ui_preset_event out = {PC_UI_PRESET_EV_NONE, {0}, {0, 0}};

    /* Canvas rect + transform for drag-to-canvas drops. */
    pc_ui_canvas_xform xf;
    bool have_canvas = pc_ui_shell_app_canvas_mapping(app, &xf);
    struct nk_rect canvas = have_canvas ? nk_rect(xf.x, xf.y, xf.width, xf.height) : nk_rect(0, 0, 0, 0);

    /* Inline rename field for this panel (preset or group). */
    const char *rtext = NULL;
    const char *rkey = pc_ui_preset_ui_rename_key(st, panel, &rtext);
    if (rkey != NULL) {
        char key_copy[256], text[256];
        (void)snprintf(key_copy, sizeof key_copy, "%s", rkey);
        (void)snprintf(text, sizeof text, "%s", rtext != NULL ? rtext : "");
        nk_layout_row_begin(ctx, NK_DYNAMIC, 22, 2);
        nk_layout_row_push(ctx, 0.18f);
        nk_label(ctx, pc_i18n_t("Name:"), NK_TEXT_LEFT);
        nk_layout_row_push(ctx, 0.82f);
        nk_flags flags = nk_edit_string_zero_terminated(
            ctx, (nk_flags)NK_EDIT_FIELD | (nk_flags)NK_EDIT_SIG_ENTER | (nk_flags)NK_EDIT_AUTO_SELECT, text,
            sizeof text, nk_filter_default);
        nk_layout_row_end(ctx);
        if (flags & NK_EDIT_COMMITED) {
            pc_ui_preset_ui_clear_rename(st);
            commit_rename(app, panel, info->prefix, key_copy, text);
            st = presets(app); /* edits may have reallocated; re-fetch */
            selected = pc_ui_preset_ui_selected(st, panel);
        } else {
            pc_ui_preset_ui_set_rename_text(st, text);
        }
    }

    /* Group display order (stored order first, then library order). */
    size_t ng = info->ngroups;
    const char **names = ng > 0 ? malloc(ng * sizeof *names) : NULL;
    size_t *order = ng > 0 ? malloc(ng * sizeof *order) : NULL;
    if (ng > 0 && (names == NULL || order == NULL)) {
        free(names);
        free(order);
        return false;
    }
    for (size_t i = 0; i < ng; i++) {
        names[i] = info->groups[i].name != NULL ? info->groups[i].name : "";
    }
    size_t n_order = order != NULL ? pc_ui_preset_ui_display_order(st, panel, names, ng, order) : 0;

    header_rec headers[PC_UI_BROWSER_MAX_HEADERS];
    size_t nheaders = 0;
    size_t drag_from = (size_t)-1; /* header currently dragged (library index) */
    size_t reorder_from = (size_t)-1; /* header released outside itself: resolve after all headers */
    float reorder_y = 0;
    float avail = nk_window_get_content_region(ctx).w;

    for (size_t oi = 0; oi < n_order; oi++) {
        size_t gi = order[oi];
        if (gi >= ng) {
            continue;
        }
        const pc_ui_preset_group *g = &info->groups[gi];
        const char *gname = names[gi];
        bool open = pc_ui_preset_ui_group_open(st, panel, gi, gname, info->swatches_all);

        /* Folder header row. */
        nk_layout_row_dynamic(ctx, 22, 1);
        struct nk_rect hr;
        enum nk_widget_layout_states hs = nk_widget(&hr, ctx);
        if (hs != NK_WIDGET_INVALID) {
            bool hover = nk_input_is_mouse_hovering_rect(in, hr);
            if (hover) {
                nk_fill_rect(cv, hr, 2, pc_ui_nk_color(t->hover));
            }
            /* Disclosure triangle. */
            float cx = hr.x + 9, cy = hr.y + hr.h / 2;
            if (open) {
                nk_fill_triangle(cv, cx - 5, cy - 3, cx + 5, cy - 3, cx, cy + 4, pc_ui_nk_color(t->text_dim));
            } else {
                nk_fill_triangle(cv, cx - 3, cy - 5, cx - 3, cy + 5, cx + 4, cy, pc_ui_nk_color(t->text_dim));
            }
            struct nk_image icon;
            if (pc_ui_shell_icon_image(app, open ? "folder-open" : "folder", 14, &icon)) {
                nk_draw_image(cv, nk_rect(hr.x + 18, cy - 7, 14, 14), &icon, pc_ui_nk_color(t->icon));
            }
            nk_draw_text(cv, nk_rect(hr.x + 36, hr.y, hr.w - 38, hr.h), gname, (int)strlen(gname),
                         ctx->style.font, pc_ui_nk_color(t->hover), pc_ui_nk_color(t->text));

            bool down_in = in->mouse.buttons[NK_BUTTON_LEFT].down &&
                           NK_INBOX(in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.x,
                                    in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.y, hr.x, hr.y, hr.w, hr.h);
            if (down_in) {
                drag_from = gi;
            }
            bool released = nk_input_is_mouse_released(in, NK_BUTTON_LEFT) &&
                            NK_INBOX(in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.x,
                                     in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.y, hr.x, hr.y, hr.w, hr.h);
            if (released) {
                if (NK_INBOX(in->mouse.pos.x, in->mouse.pos.y, hr.x, hr.y, hr.w, hr.h)) {
                    pc_ui_preset_ui_toggle_group(st, panel, gname);
                } else {
                    /* Reorder: resolve once every header rect is known (the target may be below). */
                    reorder_from = gi;
                    reorder_y = in->mouse.pos.y;
                }
            }
            /* Right-click context menu (folder). */
            if (nk_contextual_begin(ctx, 0, nk_vec2(180, 68), hr)) {
                nk_layout_row_dynamic(ctx, 26, 1);
                if (nk_contextual_item_label(ctx, pc_i18n_t("Rename Group..."), NK_TEXT_LEFT)) {
                    char key[256];
                    (void)snprintf(key, sizeof key, "group:%s", gname);
                    pc_ui_preset_ui_set_rename(st, panel, key, gname);
                }
                if (nk_contextual_item_label(ctx, pc_i18n_t("Delete Group"), NK_TEXT_LEFT)) {
                    (void)edit(app, info->prefix, "deleteGroup", "group", gname, NULL, NULL, NULL);
                    st = presets(app);
                }
                nk_contextual_end(ctx);
            }
            if (nheaders < PC_UI_BROWSER_MAX_HEADERS) {
                headers[nheaders].gi = gi;
                headers[nheaders].cy = hr.y + hr.h / 2;
                headers[nheaders].name = gname;
                nheaders++;
            }
        }
        if (!open) {
            continue;
        }

        /* Items: a grid of thumbnails, or a list when the slider is at its minimum. */
        size_t cols = 1;
        if (!list) {
            int c = (int)floorf((avail - 8 + 4) / (size + 4));
            cols = c > 0 ? (size_t)c : 1;
        }
        for (size_t ii = 0; ii < g->n; ii++) {
            size_t col = ii % cols;
            if (col == 0) {
                if (list) {
                    nk_layout_row_dynamic(ctx, 24, 1);
                } else {
                    nk_layout_row_static(ctx, size, (int)size, (int)cols);
                }
            }
            struct nk_rect r;
            enum nk_widget_layout_states ws = nk_widget(&r, ctx);
            if (ws == NK_WIDGET_INVALID) {
                continue;
            }
            const pc_ui_preset_item *it = &g->items[ii];
            bool is_sel = selected != NULL && it->key != NULL && strcmp(selected, it->key) == 0;
            bool hover = nk_input_is_mouse_hovering_rect(in, r);
            if (list) {
                if (is_sel) {
                    nk_fill_rect(cv, r, 0, pc_ui_nk_color(t->row_selected));
                } else if (hover) {
                    nk_fill_rect(cv, r, 0, pc_ui_nk_color(t->hover));
                }
                struct nk_rect tr = nk_rect(r.x + 24, r.y + 2, 20, 20);
                if (info->thumb != NULL) {
                    info->thumb(app, info->user, tr, gi, ii, true);
                }
                const char *nm = it->name != NULL ? it->name : "";
                nk_draw_text(cv, nk_rect(tr.x + tr.w + 8, r.y, r.w - (tr.x + tr.w + 8 - r.x), r.h), nm,
                             (int)strlen(nm), ctx->style.font,
                             pc_ui_nk_color(is_sel ? t->row_selected : t->canvas), pc_ui_nk_color(t->text));
            } else {
                if (info->thumb != NULL) {
                    info->thumb(app, info->user, r, gi, ii, false);
                }
                struct nk_color sc = is_sel ? pc_ui_nk_color(t->accent)
                                            : hover ? pc_ui_nk_color(t->text_dim) : pc_ui_nk_color(t->card_border);
                nk_stroke_rect(cv, r, 2, is_sel ? 2.0f : 1.0f, sc);
                if (hover) {
                    nk_tooltip(ctx, it->name != NULL ? it->name : "");
                }
            }

            /* Gestures: double-click = activate, drag out onto canvas = drop, click = select. */
            bool dbl = nk_input_is_mouse_pressed(in, NK_BUTTON_DOUBLE) && hover;
            bool released = nk_input_is_mouse_released(in, NK_BUTTON_LEFT) &&
                            NK_INBOX(in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.x,
                                     in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.y, r.x, r.y, r.w, r.h);
            bool pos_in = NK_INBOX(in->mouse.pos.x, in->mouse.pos.y, r.x, r.y, r.w, r.h);
            if (it->key != NULL && out.kind == PC_UI_PRESET_EV_NONE) {
                if (dbl) {
                    out.kind = PC_UI_PRESET_EV_ACTIVATE;
                    (void)snprintf(out.key, sizeof out.key, "%s", it->key);
                } else if (released && !pos_in && have_canvas &&
                           NK_INBOX(in->mouse.pos.x, in->mouse.pos.y, canvas.x, canvas.y, canvas.w, canvas.h)) {
                    float doc[2];
                    if (pc_ui_canvas_to_doc(&xf, in->mouse.pos.x, in->mouse.pos.y, doc) == PC_OK) {
                        out.kind = PC_UI_PRESET_EV_DROP;
                        out.drop[0] = doc[0];
                        out.drop[1] = doc[1];
                        (void)snprintf(out.key, sizeof out.key, "%s", it->key);
                    }
                } else if (released && pos_in) {
                    out.kind = PC_UI_PRESET_EV_SELECT;
                    (void)snprintf(out.key, sizeof out.key, "%s", it->key);
                }
            }
            /* Drag ghost. */
            if (it->key != NULL && in->mouse.buttons[NK_BUTTON_LEFT].down && !pos_in &&
                NK_INBOX(in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.x,
                         in->mouse.buttons[NK_BUTTON_LEFT].clicked_pos.y, r.x, r.y, r.w, r.h)) {
                struct nk_rect gr = nk_rect(in->mouse.pos.x - 14, in->mouse.pos.y - 14, 28, 28);
                nk_fill_rect(cv, gr, 3, pc_ui_nk_color(t->card));
                nk_stroke_rect(cv, gr, 3, 1, pc_ui_nk_color(t->accent));
            }
            /* Right-click context menu (preset). */
            if (nk_contextual_begin(ctx, 0, nk_vec2(160, 68), r)) {
                nk_layout_row_dynamic(ctx, 26, 1);
                if (nk_contextual_item_label(ctx, pc_i18n_t("Rename..."), NK_TEXT_LEFT) && it->key != NULL) {
                    pc_ui_preset_ui_set_rename(st, panel, it->key, it->name != NULL ? it->name : "");
                }
                if (nk_contextual_item_label(ctx, pc_i18n_t("Delete"), NK_TEXT_LEFT) && it->key != NULL) {
                    delete_preset(app, panel, info->prefix, it->key);
                    st = presets(app);
                    selected = pc_ui_preset_ui_selected(st, panel);
                }
                nk_contextual_end(ctx);
            }
        }
    }
    (void)drag_from;
    /* Resolve a folder-header reorder now every header rect is known (the next frame draws the new
     * order). `reorder_from` is a library index; move_group stores only a real reorder. */
    if (reorder_from != (size_t)-1 && reorder_from < ng) {
        size_t before = group_drop_before(headers, nheaders, reorder_from, reorder_y);
        const char *before_name = before != (size_t)-1 && before < ng ? names[before] : NULL;
        pc_ui_preset_ui_move_group(st, panel, names, ng, names[reorder_from], before_name);
    }

    /* Footer: thumbnail-size slider | New Group · New · Delete. */
    nk_layout_row_begin(ctx, NK_STATIC, 22, 4);
    nk_layout_row_push(ctx, 100);
    float slider = size;
    if (nk_slider_float(ctx, PC_UI_PRESET_MIN_THUMB, &slider, 96.0f, 1.0f) && fabsf(slider - size) > 0.01f) {
        pc_ui_preset_ui_set_size(st, panel, slider);
    }
    float btn = fmaxf(28, (nk_window_get_content_region(ctx).w - 108) / 3);
    nk_layout_row_push(ctx, btn);
    if (nk_button_label(ctx, pc_i18n_t("New Group"))) {
        new_group(app, panel, info->prefix);
    }
    nk_layout_row_push(ctx, btn);
    if (nk_button_label(ctx, pc_i18n_t("New"))) {
        new_preset(app, info);
    }
    nk_layout_row_push(ctx, btn);
    if (nk_button_label(ctx, pc_i18n_t("Delete")) && selected != NULL) {
        char k[256];
        (void)snprintf(k, sizeof k, "%s", selected);
        delete_preset(app, panel, info->prefix, k);
    }
    nk_layout_row_end(ctx);

    free(names);
    free(order);
    if (ev != NULL && out.kind != PC_UI_PRESET_EV_NONE) {
        *ev = out;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ floating windows */

/* Synchronous list query; returns the result root (held in *data until fini) or NULL. */
static yyjson_mut_val *list_query(pc_ui_shell_app *app, const char *id, pc_json_value *data)
{
    pc_error err;
    if (pc_ui_shell_app_invoke(app, id, NULL, data, &err) != PC_OK) {
        set_status(app, err.msg);
        return NULL;
    }
    return pc_json_value_root(data);
}

/* Style preset thumbnail into a GL image (cached by group/item/revision/size), via the engine. */
static bool style_thumb_image(pc_ui_shell_app *app, size_t gi, size_t ii, uint32_t px,
                              struct nk_image *out)
{
    const pc_engine_session *s = pc_ui_shell_app_session(app);
    if (gi >= s->presets.styles.n) {
        return false;
    }
    const pc_preset_group *grp = &s->presets.styles.v[gi];
    if (ii >= grp->n) {
        return false;
    }
    const pc_style_preset *style = &((const pc_style_preset *)grp->items)[ii];
    blake3_hasher h;
    uint8_t key[32];
    blake3_hasher_init(&h);
    const char category[] = "preset-browser-style-v1";
    uint64_t rev = s->presets.rev;
    blake3_hasher_update(&h, category, sizeof category);
    blake3_hasher_update(&h, &rev, sizeof rev);
    blake3_hasher_update(&h, &gi, sizeof gi);
    blake3_hasher_update(&h, &ii, sizeof ii);
    blake3_hasher_update(&h, &px, sizeof px);
    blake3_hasher_finalize(&h, key, sizeof key);
    if (pc_ui_shell_bitmap(app, key, (uint16_t)px, (uint16_t)px, NULL, out, NULL) == PC_OK) {
        return true;
    }
    pc_rgba8_image img;
    memset(&img, 0, sizeof img);
    pc_error err = {0};
    bool ok = pc_engine_style_preset_thumbnail(s, style, px, &img, &err) == PC_OK && img.pixels &&
              pc_ui_shell_bitmap(app, key, (uint16_t)img.width, (uint16_t)img.height, img.pixels, out, NULL) ==
                  PC_OK;
    pc_rgba8_image_fini(&img);
    return ok;
}

static void styles_thumb(pc_ui_shell_app *app, void *user, struct nk_rect r, size_t gi, size_t ii,
                         bool list)
{
    (void)user;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_fill_rect(cv, r, 2, pc_ui_nk_color(t->field));
    uint32_t px = pc_ui_preset_thumb_px(list ? 20 : fmaxf(8, r.w), (double)pc_ui_shell_app_ui_scale(app));
    struct nk_image img;
    if (style_thumb_image(app, gi, ii, px, &img)) {
        nk_draw_image(cv, r, &img, nk_rgba(255, 255, 255, 255));
    }
}

static void shapes_thumb(pc_ui_shell_app *app, void *user, struct nk_rect r, size_t gi, size_t ii,
                         bool list)
{
    (void)user;
    (void)gi;
    (void)ii;
    (void)list;
    /* Shapes have no public rasterised-coverage thumbnail primitive yet (the engine's shape
     * thumbnail is not exposed from src/ui's layer); draw a neutral swatch so the name reads. */
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_fill_rect(cv, r, 2, pc_ui_nk_color(t->field));
}

static unsigned shift_mod(pc_ui_shell_app *app)
{
    return pc_ui_shell_input_modifiers(app) & PC_UI_MOD_SHIFT;
}

static void dispatch_style_event(pc_ui_shell_app *app, const pc_ui_preset_event *ev)
{
    pc_ui_preset_ui_set_selected(presets(app), "styles", ev->key);
    yyjson_mut_val *root;
    yyjson_mut_doc *d = new_params(&root);
    if (d == NULL) {
        return;
    }
    yyjson_mut_obj_add_strcpy(d, root, "preset", ev->key);
    if (ev->kind != PC_UI_PRESET_EV_DROP && shift_mod(app)) {
        yyjson_mut_obj_add_bool(d, root, "add", true);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    (void)run_sync(app, "style.presets.apply", d, root, &out);
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
}

static void dispatch_shape_event(pc_ui_shell_app *app, const pc_ui_preset_event *ev)
{
    pc_ui_preset_ui *st = presets(app);
    /* Picking a shape sets it for the Custom Shape tool and switches to that tool. */
    char *copy = pc_strndup(ev->key, strlen(ev->key));
    if (copy != NULL) {
        free(st->custom_shape);
        st->custom_shape = copy;
    }
    if (ev->kind == PC_UI_PRESET_EV_SELECT) {
        pc_ui_preset_ui_set_selected(st, "shapes", ev->key);
        pc_ui_shell_app_state(app)->tool = PC_TOOL_CUSTOM_SHAPE;
        return;
    }
    yyjson_mut_val *root;
    yyjson_mut_doc *d = new_params(&root);
    if (d == NULL) {
        return;
    }
    yyjson_mut_obj_add_strcpy(d, root, "preset", ev->key);
    if (ev->kind == PC_UI_PRESET_EV_DROP) {
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
        if (doc != NULL) {
            double side = fmax(8.0, (double)(doc->size.width < doc->size.height ? doc->size.width
                                                                                : doc->size.height) *
                                        0.3);
            yyjson_mut_val *rect = yyjson_mut_arr(d);
            yyjson_mut_arr_add_real(d, rect, ev->drop[0] - side / 2);
            yyjson_mut_arr_add_real(d, rect, ev->drop[1] - side / 2);
            yyjson_mut_arr_add_real(d, rect, side);
            yyjson_mut_arr_add_real(d, rect, side);
            yyjson_mut_obj_add_val(d, root, "rect", rect);
        }
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    (void)run_sync(app, "shape.presets.place", d, root, &out);
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
}

/* Begin a floating preset window keyed off its PresetUi flag. Returns true when the body should be
 * drawn (the window is open and visible); the caller must nk_end + call window_float_end. */
static bool window_float_begin(pc_ui_shell_app *app, const char *name, const char *title, float x,
                               float y, float w, float h, float avail_w, float avail_h)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (w > avail_w - 16) {
        w = avail_w - 16;
    }
    if (h > avail_h - 16) {
        h = avail_h - 16;
    }
    if (w < 180) {
        w = 180;
    }
    if (h < 120) {
        h = 120;
    }
    return nk_begin_titled(ctx, name, title, nk_rect(x, y, w, h),
                           NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                               NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR);
}

/* After nk_end: if the window was closed via its title bar, clear the flag and re-arm. */
static void window_float_closed(pc_ui_shell_app *app, const char *name, bool *flag)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (nk_window_is_closed(ctx, name)) {
        *flag = false;
        nk_window_show(ctx, name, NK_SHOWN);
    }
}

static void styles_window(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = list_query(app, "style.presets.list", &data);
    pc_ui_preset_group_data gd;
    if (root != NULL && pc_ui_preset_groups_from_json(root, "presets", "name", &gd)) {
        pc_ui_preset_browser info = {.panel = "styles",
                                     .prefix = "style.presets",
                                     .new_tip = pc_i18n_t("Create new style from the selected layer"),
                                     .new_name = "Style",
                                     .rename_after_new = true,
                                     .groups = gd.groups,
                                     .ngroups = gd.ngroups,
                                     .thumb = styles_thumb};
        pc_ui_preset_event ev;
        if (pc_ui_shell_preset_browser(app, runtime, &info, &ev)) {
            dispatch_style_event(app, &ev);
        }
        pc_ui_preset_group_data_fini(&gd);
    } else {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("No styles."), NK_TEXT_LEFT);
    }
    pc_json_value_fini(&data);
}

static void shapes_window(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = list_query(app, "shape.presets.list", &data);
    pc_ui_preset_group_data gd;
    if (root != NULL && pc_ui_preset_groups_from_json(root, "shapes", "name", &gd)) {
        pc_ui_preset_browser info = {.panel = "shapes",
                                     .prefix = "shape.presets",
                                     .new_tip = pc_i18n_t("Create new shape from the current path"),
                                     .new_name = "Shape",
                                     .rename_after_new = true,
                                     .groups = gd.groups,
                                     .ngroups = gd.ngroups,
                                     .thumb = shapes_thumb};
        pc_ui_preset_event ev;
        if (pc_ui_shell_preset_browser(app, runtime, &info, &ev)) {
            dispatch_shape_event(app, &ev);
        }
        pc_ui_preset_group_data_fini(&gd);
    } else {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("No shapes."), NK_TEXT_LEFT);
    }
    pc_json_value_fini(&data);
}

/* Tool Presets: a flat list (name + tool icon), Current Tool Only, New / Delete / select. */
static void tool_presets_window(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    (void)runtime;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    pc_ui_preset_ui *st = presets(app);
    const char *selected = pc_ui_preset_ui_selected(st, "toolPresets");

    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = list_query(app, "tool.presets.list", &data);
    yyjson_mut_val *arr = yyjson_mut_obj_get(root, "presets");
    size_t n = yyjson_mut_arr_size(arr);
    if (n == 0) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("No tool presets."), NK_TEXT_LEFT);
    }
    const char *picked = NULL;
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(arr, i);
        const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(row, "name"));
        if (name == NULL) {
            continue;
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_bool chosen = selected != NULL && strcmp(selected, name) == 0;
        if (nk_selectable_label(ctx, name, NK_TEXT_LEFT, &chosen)) {
            picked = name;
        }
        if (nk_contextual_begin(ctx, 0, nk_vec2(200, 68), nk_widget_bounds(ctx))) {
            nk_layout_row_dynamic(ctx, 26, 1);
            if (nk_contextual_item_label(ctx, pc_i18n_t("Rename Tool Preset..."), NK_TEXT_LEFT)) {
                pc_ui_preset_ui_set_rename(st, "toolPresets", name, name);
            }
            if (nk_contextual_item_label(ctx, pc_i18n_t("Delete Tool Preset"), NK_TEXT_LEFT)) {
                pc_json_value o = PC_JSON_VALUE_INIT;
                (void)edit(app, "tool.presets", "delete", "preset", name, NULL, NULL, &o);
                pc_json_value_fini(&o);
                st = presets(app);
            }
            nk_contextual_end(ctx);
        }
    }
    (void)t;
    nk_layout_row_dynamic(ctx, 22, 2);
    nk_bool only = st->current_tool_only;
    if (nk_checkbox_label(ctx, pc_i18n_t("Current Tool Only"), &only)) {
        st->current_tool_only = only;
    }
    if (nk_button_label(ctx, pc_i18n_t("Delete")) && selected != NULL) {
        pc_json_value o = PC_JSON_VALUE_INIT;
        char k[256];
        (void)snprintf(k, sizeof k, "%s", selected);
        (void)edit(app, "tool.presets", "delete", "preset", k, NULL, NULL, &o);
        pc_json_value_fini(&o);
        st = presets(app);
    }
    if (picked != NULL) {
        char k[256];
        (void)snprintf(k, sizeof k, "%s", picked);
        pc_ui_preset_ui_set_selected(st, "toolPresets", k);
        yyjson_mut_val *proot;
        yyjson_mut_doc *d = new_params(&proot);
        if (d != NULL) {
            yyjson_mut_obj_add_strcpy(d, proot, "preset", k);
            pc_json_value o = PC_JSON_VALUE_INIT;
            (void)run_sync(app, "tool.presets.select", d, proot, &o);
            pc_json_value_fini(&o);
            yyjson_mut_doc_free(d);
        }
    }
    pc_json_value_fini(&data);
}

/* Clone Source: the five source slots + a compact transform and overlay toggle. */
static void clone_source_window(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    (void)runtime;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = list_query(app, "cloneSource.list", &data);
    if (root == NULL) {
        /* No list command available: still offer the five source buttons. */
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("Clone Source"), NK_TEXT_LEFT);
    }
    size_t active = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "active"));
    nk_layout_row_dynamic(ctx, 28, 5);
    for (size_t i = 0; i < 5; i++) {
        char label[8];
        (void)snprintf(label, sizeof label, "%zu", i + 1);
        nk_bool on = i == active;
        if (nk_selectable_label(ctx, label, NK_TEXT_CENTERED, &on) && on != (i == active)) {
            yyjson_mut_val *proot;
            yyjson_mut_doc *d = new_params(&proot);
            if (d != NULL) {
                yyjson_mut_obj_add_uint(d, proot, "index", i);
                pc_json_value o = PC_JSON_VALUE_INIT;
                (void)run_sync(app, "cloneSource.select", d, proot, &o);
                pc_json_value_fini(&o);
                yyjson_mut_doc_free(d);
            }
        }
    }
    yyjson_mut_val *sources = yyjson_mut_obj_get(root, "sources");
    yyjson_mut_val *slot = active < yyjson_mut_arr_size(sources) ? yyjson_mut_arr_get(sources, active) : NULL;
    bool have = yyjson_mut_is_obj(slot) && !yyjson_mut_is_null(yyjson_mut_obj_get(slot, "source"));
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, have ? pc_i18n_t("Source: set") : pc_i18n_t("Source: not set (Alt-click with Clone Stamp)"),
             NK_TEXT_LEFT);
    /* Scale/rotation controls drive cloneSource.set. */
    float w = (float)yyjson_mut_get_num(yyjson_mut_obj_get(slot, "width"));
    float hh = (float)yyjson_mut_get_num(yyjson_mut_obj_get(slot, "height"));
    float rot = (float)yyjson_mut_get_num(yyjson_mut_obj_get(slot, "rotation"));
    if (!(w > 0)) {
        w = 100;
    }
    if (!(hh > 0)) {
        hh = 100;
    }
    float w0 = w, h0 = hh, r0 = rot;
    nk_layout_row_dynamic(ctx, 22, 2);
    nk_property_float(ctx, "W %", -1000, &w, 1000, 1, 0.5f);
    nk_property_float(ctx, "H %", -1000, &hh, 1000, 1, 0.5f);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_property_float(ctx, "Angle", -180, &rot, 180, 1, 0.5f);
    if (w != w0 || hh != h0 || rot != r0) {
        yyjson_mut_val *proot;
        yyjson_mut_doc *d = new_params(&proot);
        if (d != NULL) {
            if (w != w0) {
                yyjson_mut_obj_add_real(d, proot, "width", w);
            }
            if (hh != h0) {
                yyjson_mut_obj_add_real(d, proot, "height", hh);
            }
            if (rot != r0) {
                yyjson_mut_obj_add_real(d, proot, "rotation", rot);
            }
            pc_json_value o = PC_JSON_VALUE_INIT;
            (void)run_sync(app, "cloneSource.set", d, proot, &o);
            pc_json_value_fini(&o);
            yyjson_mut_doc_free(d);
        }
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    if (nk_button_label(ctx, pc_i18n_t("Reset Transform"))) {
        pc_json_value o = PC_JSON_VALUE_INIT;
        (void)run_sync(app, "cloneSource.resetTransform", NULL, NULL, &o);
        pc_json_value_fini(&o);
    }
    pc_json_value_fini(&data);
}

void pc_ui_shell_preset_windows_draw(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, float w, float h)
{
    if (app == NULL || w < 200 || h < 160) {
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_preset_ui *st = presets(app);
    struct {
        const char *name, *title;
        bool *flag;
        float dw, dh, off;
        void (*body)(pc_ui_shell_app *, pc_ui_panel_runtime *);
    } wins[] = {
        {"preset-styles", pc_i18n_t("Styles"), &st->styles, 250, 320, 0, styles_window},
        {"preset-shapes", pc_i18n_t("Shapes"), &st->shapes, 250, 320, 30, shapes_window},
        {"preset-toolPresets", pc_i18n_t("Tool Presets"), &st->tool_presets, 250, 300, 60,
         tool_presets_window},
        {"preset-cloneSource", pc_i18n_t("Clone Source"), &st->clone_source, 270, 300, 90,
         clone_source_window},
    };
    for (size_t i = 0; i < sizeof wins / sizeof wins[0]; i++) {
        if (!*wins[i].flag) {
            continue;
        }
        float x = fmaxf(16, w - wins[i].dw - 24 - wins[i].off);
        float y = 56 + wins[i].off;
        if (window_float_begin(app, wins[i].name, wins[i].title, x, y, wins[i].dw, wins[i].dh, w, h)) {
            wins[i].body(app, runtime);
        }
        nk_end(ctx);
        window_float_closed(app, wins[i].name, wins[i].flag);
    }
}
