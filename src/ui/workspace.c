#include "ui/workspace.h"
#include "base/mem.h"
#include <stdlib.h>
#include <string.h>
bool pc_ui_workspace_builtin(const char *name)
{
    const char *names[] = {"Essentials", "Photography", "Painting", "Pixel Art", "Graphic and Web", "Motion"};
    for (size_t i = 0; name && i < 6; i++)
        if (!strcmp(name, names[i]))
            return true;
    return false;
}
pc_status pc_ui_workspace_snapshot(const pc_ui_state *s, yyjson_mut_doc *d, yyjson_mut_val **out,
                                   pc_error *err)
{
    if (!s || !d || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing workspace state");
    yyjson_mut_val *state = NULL, *root = yyjson_mut_obj(d);
    pc_status status = pc_ui_state_to_json(s, d, &state, err);
    const char *from[] = {"panels", "dock", "dock_tabs", "dock_width"},
               *to[] = {"panels", "dock", "dockTabs", "dockWidth"};
    for (size_t i = 0; status == PC_OK && i < 4; i++) {
        yyjson_mut_val *v = yyjson_mut_obj_get(state, from[i]), *copy = yyjson_mut_val_mut_copy(d, v);
        if (!root || !copy || !yyjson_mut_obj_add_val(d, root, to[i], copy))
            status = pc_fail(err, PC_ERR_NOMEM, "workspace layout");
    }
    if (status == PC_OK) {
        yyjson_mut_val *panels = yyjson_mut_obj_get(root, "panels");
        (void)yyjson_mut_obj_remove_str(panels, "rail");
        (void)yyjson_mut_obj_remove_str(panels, "menu_bar");
        if (!yyjson_mut_obj_add_bool(d, root, "timelineOpen", s->timeline.open))
            return pc_fail(err, PC_ERR_NOMEM, "workspace timeline visibility");
        *out = root;
    }
    return status;
}
pc_status pc_ui_workspace_apply(pc_ui_state *s, const char *name, yyjson_val *saved, pc_error *err)
{
    if (!s || !name || !*name || (!saved && !pc_ui_workspace_builtin(name)) ||
        (saved && !yyjson_is_obj(saved)))
        return pc_fail(err, PC_ERR_INVALID, "unknown or invalid workspace");
    char *owned = pc_strndup(name, strlen(name));
    if (!owned)
        return pc_fail(err, PC_ERR_NOMEM, "workspace name");
    pc_ui_state layout;
    pc_ui_state_init(&layout);
    pc_status status = PC_OK;
    bool timeline_open = s->timeline.open;
    if (saved && yyjson_obj_get(saved, "timelineOpen")) {
        yyjson_val *value = yyjson_obj_get(saved, "timelineOpen");
        if (!yyjson_is_bool(value)) {
            free(owned);
            pc_ui_state_fini(&layout);
            return pc_fail(err, PC_ERR_INVALID, "workspace timelineOpen must be a boolean");
        }
        timeline_open = yyjson_get_bool(value);
    }
    if (saved) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
        const char *from[] = {"panels", "dock", "dockTabs", "dockWidth"},
                   *to[] = {"panels", "dock", "dock_tabs", "dock_width"};
        if (!root)
            status = pc_fail(err, PC_ERR_NOMEM, "workspace layout");
        for (size_t i = 0; status == PC_OK && i < 4; i++) {
            yyjson_val *v = yyjson_obj_get(saved, from[i]);
            if (!v)
                continue;
            if (i < 3 && !yyjson_is_obj(v)) {
                status = pc_fail(err, PC_ERR_INVALID, "invalid workspace layout");
                break;
            }
            yyjson_mut_val *copy = yyjson_val_mut_copy(d, v);
            if (!copy || !yyjson_mut_obj_add_val(d, root, to[i], copy))
                status = pc_fail(err, PC_ERR_NOMEM, "workspace layout");
        }
        yyjson_doc *read = status == PC_OK ? yyjson_mut_val_imut_copy(root, NULL) : NULL;
        if (status == PC_OK)
            status = read ? pc_ui_state_from_json(&layout, yyjson_doc_get_root(read), err)
                          : pc_fail(err, PC_ERR_NOMEM, "workspace layout");
        yyjson_doc_free(read);
        yyjson_mut_doc_free(d);
    } else {
        layout.panels = s->panels;
        bool photo = !strcmp(name, "Photography"), paint = !strcmp(name, "Painting"),
             pixel = !strcmp(name, "Pixel Art"), graphic = !strcmp(name, "Graphic and Web"),
             motion = !strcmp(name, "Motion");
        layout.panels.navigator = photo || pixel;
        layout.panels.color = !photo && !graphic && !motion;
        layout.panels.layers = true;
        layout.panels.history = photo;
        layout.panels.properties = !paint && !pixel;
        layout.panels.character = graphic;
        timeline_open = motion;
    }
    if (status == PC_OK) {
        bool rail = s->panels.rail, menu = s->panels.menu_bar;
        s->panels = layout.panels;
        s->panels.rail = rail;
        s->panels.menu_bar = menu;
        s->dock_tabs = layout.dock_tabs;
        s->timeline = (pc_ui_timeline){.open = timeline_open};
        if (saved && yyjson_obj_get(saved, "dockWidth"))
            s->dock_width = layout.dock_width;
        pc_ui_dock_layout_fini(&s->dock);
        s->dock = layout.dock;
        layout.dock = (pc_ui_dock_layout){0};
        free(s->workspace);
        s->workspace = owned;
        owned = NULL;
    }
    pc_ui_state_fini(&layout);
    free(owned);
    return status;
}

void pc_ui_workspace_take(pc_ui_state *target, pc_ui_state *prepared)
{
    if (!target || !prepared || target == prepared)
        return;
    target->panels = prepared->panels;
    target->dock_tabs = prepared->dock_tabs;
    target->timeline = prepared->timeline;
    target->dock_width = prepared->dock_width;
    pc_ui_dock_layout_fini(&target->dock);
    target->dock = prepared->dock;
    prepared->dock = (pc_ui_dock_layout){0};
    free(target->workspace);
    target->workspace = prepared->workspace;
    prepared->workspace = NULL;
}
