/* Menu bar model: the Photoshop catalogue plus the shell's UI-level commands, liveness and the
 * menu-item builder. Port of the data-and-logic half of crates/ui-egui/src/menus.rs. */
#include "ui/menus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "ui/menu_catalog.h"
#include "ui/shortcuts.h"
#include "ui/view_cmds.h"

const char *const PC_UI_TOP_MENUS[10] = {"File", "Edit",   "Image",  "Layer",  "Type",
                                         "Select", "Filter", "View",   "Window", "Help"};

const char *const PC_UI_DOCUMENT_ITEM = "window.document.";

/* ---- UI-level commands (generated from menus::UI_COMMANDS) --------------------------------- */

const pc_ui_command PC_UI_COMMANDS[] = {
    {"ui.symmetryTransform", "Transform Symmetry", {NULL, NULL, NULL}, 0, NULL},
    {"tool.reset", "Reset Tool", {NULL, NULL, NULL}, 0, NULL},
    {"tool.resetAll", "Reset All Tools", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.group", "Change Panel Group", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.layout", "Change Panel Layout", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.move", "Move Panel", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.float", "Float Panel", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.dock", "Dock Panel", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.activate", "Activate Panel", {NULL, NULL, NULL}, 0, NULL},
    {"window.panel.close", "Close Panel", {NULL, NULL, NULL}, 0, NULL},
    {"file.open", "Open…", {"File", NULL, NULL}, 1, "Cmd+O"},
    {"file.save", "Save", {"File", NULL, NULL}, 1, "Cmd+S"},
    {"file.saveAs", "Save As…", {"File", NULL, NULL}, 1, "Cmd+Shift+S"},
    {"file.exit", "Exit", {"File", NULL, NULL}, 1, "Cmd+Q"},
    {"edit.freeTransform", "Free Transform", {"Edit", NULL, NULL}, 1, "Cmd+T"},
    {"file.export.exportAs", "Export As…", {"File", "Export", NULL}, 2, "Cmd+Alt+Shift+W"},
    {"file.export.quickExportAsPng", "Quick Export as PNG", {"File", "Export", NULL}, 2, NULL},
    {"edit.transform.scale", "Scale", {"Edit", "Transform", NULL}, 2, NULL},
    {"edit.transform.rotate", "Rotate", {"Edit", "Transform", NULL}, 2, NULL},
    {"edit.transform.skew", "Skew", {"Edit", "Transform", NULL}, 2, NULL},
    {"edit.transform.distort", "Distort", {"Edit", "Transform", NULL}, 2, NULL},
    {"edit.transform.perspective", "Perspective", {"Edit", "Transform", NULL}, 2, NULL},
    {"select.selectAndMask", "Select and Mask…", {"Select", NULL, NULL}, 1, "Cmd+Alt+R"},
    {"view.rulers", "Rulers", {"View", NULL, NULL}, 1, "Cmd+R"},
    {"view.show.grid", "Grid", {"View", "Show", NULL}, 2, "Cmd+'"},
    {"view.show.guides", "Guides", {"View", "Show", NULL}, 2, "Cmd+;"},
    {"view.snap", "Snap", {"View", NULL, NULL}, 1, "Cmd+Shift+;"},
    {"view.lockGuides", "Lock Guides", {"View", NULL, NULL}, 1, "Cmd+Alt+;"},
    {"view.extras", "Extras", {"View", NULL, NULL}, 1, "Cmd+H"},
    {"view.show.targetPath", "Target Path", {"View", "Show", NULL}, 2, "Cmd+Shift+H"},
    {"view.screenMode.cycle", "Cycle Screen Mode", {NULL, NULL, NULL}, 0, "F"},
    {"edit.freeTransformCopy", "Free Transform a Copy", {NULL, NULL, NULL}, 0, "Cmd+Alt+T"},
    {"type.editText", "Edit Type", {NULL, NULL, NULL}, 0, NULL},
    {"view.zoomIn", "Zoom In", {"View", NULL, NULL}, 1, "Cmd+="},
    {"view.zoomOut", "Zoom Out", {"View", NULL, NULL}, 1, "Cmd+-"},
    {"view.fitOnScreen", "Fit on Screen", {"View", NULL, NULL}, 1, "Cmd+0"},
    {"view.actualPixels", "100%", {"View", NULL, NULL}, 1, "Cmd+1"},
    {"view.rotateView", "Rotate View", {NULL, NULL, NULL}, 0, NULL},
    {"view.resetView", "Reset View", {"View", NULL, NULL}, 1, NULL},
    {"window.newWindowForDocument", "New Window for Document", {"Window", "Arrange", NULL}, 2, NULL},
    {"window.toggle.layers", "Layers", {"Window", NULL, NULL}, 1, "F7"},
    {"window.toggle.history", "History", {"Window", NULL, NULL}, 1, NULL},
    {"window.toggle.properties", "Properties", {"Window", NULL, NULL}, 1, NULL},
    {"window.toggle.color", "Color", {"Window", NULL, NULL}, 1, "F6"},
    {"window.toggle.brushSettings", "Brush Settings", {"Window", NULL, NULL}, 1, "F5"},
    {"window.toggle.navigator", "Navigator", {"Window", NULL, NULL}, 1, NULL},
    {"window.toggle.toolbar", "Tools", {"Window", NULL, NULL}, 1, NULL},
    {"window.togglePanels", "Show/Hide All Panels", {NULL, NULL, NULL}, 0, "Tab"},
    {"window.toggle.dock", "Show/Hide Panels", {NULL, NULL, NULL}, 0, "Shift+Tab"},
    {"window.toggle.options", "Options", {"Window", NULL, NULL}, 1, NULL},
    {"window.nextDocument", "Next Document", {NULL, NULL, NULL}, 0, "Ctrl+Tab"},
    {"window.previousDocument", "Previous Document", {NULL, NULL, NULL}, 0, "Ctrl+Shift+Tab"},
    {"window.theme.toggle", "Next Appearance Mode", {"Window", NULL, NULL}, 1, NULL},
    {"window.theme.pro", "Pro Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.proMedium", "Pro Medium Gray Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.proLightGray", "Pro Light Gray Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.proLight", "Pro Light Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.studio", "Studio Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.studioLight", "Studio Light Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.classic", "Classic Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.solarizedDark", "Solarized Dark Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.adwaita", "Adwaita Light Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.adwaitaDark", "Adwaita Dark Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.breezeLight", "Breeze Light Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.breezeDark", "Breeze Dark Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.softLight", "Soft Light Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.softDark", "Soft Dark Theme", {"Window", "Theme", NULL}, 2, NULL},
    {"window.theme.system", "Sync with system", {"Window", "Theme", NULL}, 2, NULL},
    {"edit.search", "Search…", {"Edit", NULL, NULL}, 1, "Cmd+F"},
    {"help.discord", "Join the ArtCraft Discord…", {"Help", NULL, NULL}, 1, NULL},
    {"help.website", "PhotoCraft Website", {"Help", NULL, NULL}, 1, NULL},
    {"help.artcraftWebsite", "ArtCraft Website", {"Help", NULL, NULL}, 1, NULL},
    {"help.github", "PhotoCraft on GitHub", {"Help", NULL, NULL}, 1, NULL},
    {"help.reportIssue", "Report an Issue…", {"Help", NULL, NULL}, 1, NULL},
    {"help.systemInfo", "System Info…", {"Help", NULL, NULL}, 1, NULL},
    {"help.about", "About PhotoCraft", {"Help", NULL, NULL}, 1, NULL},
};
const size_t PC_UI_COMMANDS_N = sizeof(PC_UI_COMMANDS) / sizeof(PC_UI_COMMANDS[0]);

const pc_ui_command *pc_ui_command_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < PC_UI_COMMANDS_N; i++) {
        if (strcmp(PC_UI_COMMANDS[i].id, id) == 0) {
            return &PC_UI_COMMANDS[i];
        }
    }
    return NULL;
}

/* ---- aliases / presets -------------------------------------------------------------------- */

static const char *strip(const char *id, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(id, prefix, n) == 0 ? id + n : NULL;
}

const char *pc_ui_panel_alias(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    const char *k = strip(id, "window.panel.");
    if (k == NULL) {
        return NULL;
    }
    if (strcmp(k, "layers") == 0) {
        return "window.toggle.layers";
    }
    if (strcmp(k, "history") == 0) {
        return "window.toggle.history";
    }
    if (strcmp(k, "properties") == 0 || strcmp(k, "adjustments") == 0) {
        return "window.toggle.properties";
    }
    if (strcmp(k, "color") == 0 || strcmp(k, "swatches") == 0) {
        return "window.toggle.color";
    }
    if (strcmp(k, "navigator") == 0 || strcmp(k, "info") == 0 || strcmp(k, "histogram") == 0) {
        return "window.toggle.navigator";
    }
    if (strcmp(k, "tools") == 0) {
        return "window.toggle.toolbar";
    }
    if (strcmp(k, "options") == 0) {
        return "window.toggle.options";
    }
    if (strcmp(k, "brushSettings") == 0 || strcmp(k, "brushes") == 0) {
        return "window.toggle.brushSettings";
    }
    return NULL;
}

const char *pc_ui_proof_preset(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    const char *k = strip(id, "view.proofSetup.");
    if (k == NULL) {
        return NULL;
    }
    if (strcmp(k, "workingCmyk") == 0) {
        return "working-cmyk";
    }
    if (strcmp(k, "internetStandardRgb") == 0 || strcmp(k, "monitorRgb") == 0) {
        return "srgb";
    }
    return NULL;
}

bool pc_ui_workspace_name(const char *id, const char **out)
{
    if (id == NULL) {
        return false;
    }
    const char *k = strip(id, "window.workspace.");
    if (k == NULL) {
        return false;
    }
    const char *name = NULL;
    if (strcmp(k, "essentials") == 0 || strcmp(k, "resetWorkspace") == 0) {
        name = "";
    } else if (strcmp(k, "photography") == 0) {
        name = "Photography";
    } else if (strcmp(k, "painting") == 0) {
        name = "Painting";
    } else if (strcmp(k, "graphicAndWeb") == 0) {
        name = "Graphic and Web";
    } else if (strcmp(k, "pixelArt") == 0) {
        name = "Pixel Art";
    } else if (strcmp(k, "motion") == 0) {
        name = "Motion";
    } else {
        return false;
    }
    if (out != NULL) {
        *out = name;
    }
    return true;
}

/* ---- toolbar tool select ids (port of tool_shortcuts::id / tool) -------------------------- */

size_t pc_ui_tool_select_id(pc_ui_tool tool, char *out, size_t cap)
{
    const char *serde = pc_ui_tool_serde(tool);
    char lower[64];
    size_t n = 0;
    if (serde != NULL) {
        n = strlen(serde);
        if (n >= sizeof lower) {
            n = sizeof lower - 1;
        }
        memcpy(lower, serde, n);
        lower[n] = '\0';
        if (n > 0 && lower[0] >= 'A' && lower[0] <= 'Z') {
            lower[0] = (char)(lower[0] - 'A' + 'a');
        }
    } else {
        lower[0] = '\0';
    }
    return (size_t)snprintf(out, cap, "tools.select.%s", lower);
}

bool pc_ui_tool_from_select_id(const char *id, pc_ui_tool *out)
{
    if (id == NULL) {
        return false;
    }
    const char *name = strip(id, "tools.select.");
    if (name == NULL) {
        return false;
    }
    pc_ui_tool t;
    if (!pc_ui_tool_from_name(name, &t)) {
        return false;
    }
    char canon[96];
    pc_ui_tool_select_id(t, canon, sizeof canon);
    if (strcmp(canon, id) != 0) {
        return false;
    }
    if (out != NULL) {
        *out = t;
    }
    return true;
}

/* ---- liveness ----------------------------------------------------------------------------- */

/* Ids made live by the panel/type/timeline UI modules' handles() (analysis_ui, workspace_ui,
 * preset_panels, type_panels_ui, timeline_ui). Those modules land in later U-chunks; their
 * liveness contract is kept here so the parity report is accurate now. */
static const char *const EXTRA_LIVE[] = {
    /* analysis_ui::PANELS */
    "window.panel.measurementLog", "window.panel.notes",
    /* workspace_ui::IDS */
    "window.panel.modifierKeys", "window.workspace.newWorkspace", "window.workspace.deleteWorkspace",
    "window.workspace.lockWorkspace", "window.workspace.select", "view.pixelAspectRatio.custom",
    "view.show.showExtrasOptions",
    /* preset_panels::PANELS */
    "window.panel.gradients", "window.panel.patterns", "window.panel.styles", "window.panel.shapes",
    "window.panel.toolPresets", "window.panel.cloneSource",
    /* type_panels_ui::IDS */
    "type.panels.characterStyles", "window.panel.characterStyles", "type.panels.paragraphStyles",
    "window.panel.paragraphStyles", "type.panels.glyphs", "window.panel.glyphs",
    /* timeline_ui */
    "window.panel.timeline",
};
enum { EXTRA_LIVE_N = (int)(sizeof(EXTRA_LIVE) / sizeof(EXTRA_LIVE[0])) };

static bool extra_live(const char *id)
{
    for (int i = 0; i < EXTRA_LIVE_N; i++) {
        if (strcmp(EXTRA_LIVE[i], id) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_ui_is_live(const char *id)
{
    if (id == NULL) {
        return false;
    }
    pc_ui_tool t;
    return pc_engine_command_find(id) != NULL || pc_ui_tool_from_select_id(id, &t) ||
           pc_ui_command_find(id) != NULL || pc_ui_panel_alias(id) != NULL ||
           pc_ui_workspace_name(id, NULL) || pc_ui_proof_preset(id) != NULL ||
           strcmp(id, "view.proofSetup.custom") == 0 || pc_ui_view_cmds_handles(id) ||
           extra_live(id);
}

/* ---- menu-item builder -------------------------------------------------------------------- */

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

/* Compare labels ignoring a single trailing "…" (U+2026, UTF-8 E2 80 A6). */
static size_t trimmed_len(const char *s)
{
    size_t n = strlen(s);
    if (n >= 3 && (unsigned char)s[n - 3] == 0xE2 && (unsigned char)s[n - 2] == 0x80 &&
        (unsigned char)s[n - 1] == 0xA6) {
        return n - 3;
    }
    return n;
}

static bool labels_eq_trimmed(const char *a, const char *b)
{
    size_t la = trimmed_len(a), lb = trimmed_len(b);
    return la == lb && memcmp(a, b, la) == 0;
}

static bool paths_eq(char *const ap[3], size_t ad, const char *const bp[3], size_t bd)
{
    if (ad != bd) {
        return false;
    }
    for (size_t i = 0; i < ad; i++) {
        if (strcmp(ap[i] ? ap[i] : "", bp[i] ? bp[i] : "") != 0) {
            return false;
        }
    }
    return true;
}

/* Whether a live menu item is enabled now (model level): engine commands use engine enablement;
 * other live commands (UI/shell/view) default to enabled, the shell refines per view state. */
static bool item_enabled(const pc_engine_session *s, const char *id)
{
    if (!pc_ui_is_live(id)) {
        return false;
    }
    if (pc_engine_command_find(id) != NULL) {
        return s != NULL && pc_engine_session_is_enabled(s, id);
    }
    return true;
}

/* Commands outside the catalogue that belong right after a catalogue item: (id, after). */
static const struct {
    const char *id;
    const char *after;
} PLACE_AFTER[] = {
    {"file.newFromClipboard", "file.new"},
    {"filter.render.relight", "filter.render.lightingEffects"},
    {"view.resetView", "view.flipHorizontal"},
    {"filter.other.colorToAlpha", "filter.other.offset"},
};
enum { PLACE_AFTER_N = (int)(sizeof(PLACE_AFTER) / sizeof(PLACE_AFTER[0])) };

static void item_fini(pc_ui_menu_item *it)
{
    free(it->id);
    free(it->label);
    for (int i = 0; i < 3; i++) {
        free(it->path[i]);
    }
    free(it->shortcut);
    memset(it, 0, sizeof *it);
}

void pc_ui_menu_items_free(pc_ui_menu_item *items, size_t n)
{
    if (items == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        item_fini(&items[i]);
    }
    free(items);
}

/* Set an item's path from `src` path components; returns false on OOM. */
static bool set_path(pc_ui_menu_item *it, const char *const src[3], size_t depth)
{
    for (size_t i = 0; i < depth; i++) {
        it->path[i] = dup_str(src[i]);
        if (src[i] != NULL && it->path[i] == NULL) {
            return false;
        }
    }
    it->depth = depth;
    return true;
}

pc_status pc_ui_menu_items(const pc_engine_session *s, pc_ui_menu_item **out, size_t *n,
                           pc_error *err)
{
    if (out == NULL || n == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "pc_ui_menu_items: NULL out");
    }
    *out = NULL;
    *n = 0;

    size_t neng = 0;
    const pc_engine_command *eng = pc_engine_commands(&neng);
    size_t cap = PC_UI_CATALOG_N + PC_UI_COMMANDS_N + neng + 4;
    pc_ui_menu_item *items = pc_calloc_array(cap, sizeof *items);
    if (items == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
    }
    size_t count = 0;
    pc_status st = PC_OK;

    /* 1) Photoshop's full menu tree, in order. */
    for (size_t i = 0; i < PC_UI_CATALOG_N; i++) {
        const pc_ui_catalog_item *c = &PC_UI_CATALOG[i];
        bool live = pc_ui_is_live(c->id);
        pc_ui_menu_item *it = &items[count];
        it->id = dup_str(c->id);
        it->label = dup_str(c->label);
        it->checked = -1;
        if (!set_path(it, c->path, c->depth) || it->id == NULL || it->label == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
            goto fail;
        }
        if (live) {
            char sc[128];
            if (pc_ui_default_shortcut(c->id, sc, sizeof sc)) {
                it->shortcut = dup_str(sc);
                if (it->shortcut == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
                    goto fail;
                }
            }
            it->enabled = item_enabled(s, c->id);
        } else {
            it->shortcut = dup_str(c->shortcut);
            if (c->shortcut != NULL && it->shortcut == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
                goto fail;
            }
            it->enabled = false;
        }
        count++;
    }

    /* 2) Shell UI commands and engine commands with a menu path, not already present, inserted
     * after the item they belong next to (else after the last item of the same top menu). */
    size_t n_extra = PC_UI_COMMANDS_N + neng;
    for (size_t e = 0; e < n_extra; e++) {
        const char *eid;
        const char *elabel;
        const char *const *epath;
        size_t edepth;
        const char *esc;
        bool from_ui = e < PC_UI_COMMANDS_N;
        if (from_ui) {
            const pc_ui_command *u = &PC_UI_COMMANDS[e];
            eid = u->id;
            elabel = u->label;
            epath = u->path;
            edepth = u->depth;
            esc = u->shortcut;
        } else {
            const pc_engine_command *c = &eng[e - PC_UI_COMMANDS_N];
            if (c->menu_len == 0) {
                continue;
            }
            eid = c->id;
            elabel = c->label;
            epath = c->menu;
            edepth = c->menu_len > 3 ? 3 : c->menu_len;
            esc = c->shortcut;
        }

        /* Duplicate? same id, or same path and label (ignoring trailing "…"). */
        bool dup = false;
        for (size_t i = 0; i < count && !dup; i++) {
            if (strcmp(items[i].id, eid) == 0 ||
                (paths_eq(items[i].path, items[i].depth, epath, edepth) &&
                 labels_eq_trimmed(items[i].label, elabel))) {
                dup = true;
            }
        }
        if (dup) {
            continue;
        }

        /* Anchor from PLACE_AFTER. */
        long anchor = -1;
        const char *anchor_id = NULL;
        for (int p = 0; p < PLACE_AFTER_N; p++) {
            if (strcmp(PLACE_AFTER[p].id, eid) == 0) {
                anchor_id = PLACE_AFTER[p].after;
                break;
            }
        }
        if (anchor_id != NULL) {
            for (size_t i = 0; i < count; i++) {
                if (strcmp(items[i].id, anchor_id) == 0) {
                    anchor = (long)i;
                    break;
                }
            }
        }

        /* Placement index. */
        size_t at;
        const char *const *use_path = epath;
        size_t use_depth = edepth;
        char *anchor_path_copy[3] = {NULL, NULL, NULL};
        size_t anchor_depth = 0;
        if (anchor >= 0) {
            /* e takes the anchor's path. */
            anchor_depth = items[anchor].depth;
            for (size_t i = 0; i < anchor_depth; i++) {
                anchor_path_copy[i] = items[anchor].path[i]; /* borrowed for set below */
            }
            at = (size_t)anchor + 1;
        } else {
            long last = -1;
            const char *top = edepth > 0 ? epath[0] : NULL;
            for (size_t i = 0; i < count; i++) {
                const char *itop = items[i].depth > 0 ? items[i].path[0] : NULL;
                if ((itop == NULL && top == NULL) ||
                    (itop != NULL && top != NULL && strcmp(itop, top) == 0)) {
                    last = (long)i;
                }
            }
            at = last >= 0 ? (size_t)last + 1 : count;
        }

        /* Build the item. */
        pc_ui_menu_item ni;
        memset(&ni, 0, sizeof ni);
        ni.id = dup_str(eid);
        ni.label = dup_str(elabel);
        ni.checked = -1;
        ni.enabled = item_enabled(s, eid);
        if (esc != NULL) {
            ni.shortcut = dup_str(esc);
        }
        if (ni.id == NULL || ni.label == NULL || (esc != NULL && ni.shortcut == NULL)) {
            item_fini(&ni);
            st = pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
            goto fail;
        }
        if (anchor >= 0) {
            if (!set_path(&ni, (const char *const *)anchor_path_copy, anchor_depth)) {
                item_fini(&ni);
                st = pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
                goto fail;
            }
        } else {
            if (!set_path(&ni, use_path, use_depth)) {
                item_fini(&ni);
                st = pc_fail(err, PC_ERR_NOMEM, "pc_ui_menu_items: out of memory");
                goto fail;
            }
        }

        /* Insert at `at` (shift the tail right by one; cap guarantees room). */
        if (count >= cap) {
            item_fini(&ni);
            st = pc_fail(err, PC_ERR_LIMIT, "pc_ui_menu_items: capacity exceeded");
            goto fail;
        }
        memmove(&items[at + 1], &items[at], (count - at) * sizeof *items);
        items[at] = ni;
        count++;
    }

    *out = items;
    *n = count;
    return PC_OK;

fail:
    pc_ui_menu_items_free(items, count);
    return st;
}
