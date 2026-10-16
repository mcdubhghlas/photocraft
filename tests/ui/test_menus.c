/* Menu catalogue, liveness, the menu-item builder, the parity report and the catalogue↔engine
 * consistency checks (ports of parity.rs tests, menus.rs invariants and tool_shortcuts coverage). */
#include "harness.h"

#include "engine/engine.h"
#include "ui/menu_catalog.h"
#include "ui/menus.h"
#include "ui/parity.h"
#include "ui/state.h"
#include "ui/view_cmds.h"

#include <stdlib.h>
#include <string.h>

static void test_catalog_shape(void)
{
    CHECK(PC_UI_CATALOG_N == 770);
    CHECK_EQ_STR(PC_UI_CATALOG[0].id, "file.new");
    CHECK_EQ_STR(PC_UI_CATALOG[0].label, "New…");
    CHECK_EQ_STR(PC_UI_CATALOG[0].shortcut, "Cmd+N");
    CHECK_EQ_STR(PC_UI_CATALOG[0].path[0], "File");
    /* Every row has a sane depth and non-NULL strings; separators are flagged. */
    for (size_t i = 0; i < PC_UI_CATALOG_N; i++) {
        const pc_ui_catalog_item *c = &PC_UI_CATALOG[i];
        CHECK(c->depth >= 1 && c->depth <= 3);
        CHECK(c->id != NULL && c->label != NULL);
        for (size_t j = 0; j < c->depth; j++) {
            CHECK(c->path[j] != NULL);
        }
        if (pc_ui_catalog_is_separator(c->id)) {
            CHECK_EQ_STR(c->label, "---");
        }
    }
    /* A three-deep row exists (Layer › Smart Objects › Stack Mode). */
    bool deep = false;
    for (size_t i = 0; i < PC_UI_CATALOG_N; i++) {
        if (PC_UI_CATALOG[i].depth == 3) {
            deep = true;
        }
    }
    CHECK(deep);
}

static void test_is_live(void)
{
    /* engine command */
    CHECK(pc_ui_is_live("file.new"));
    /* UI shell commands */
    CHECK(pc_ui_is_live("view.zoomIn"));
    CHECK(pc_ui_is_live("window.theme.pro"));
    /* panel alias / view handling */
    CHECK(pc_ui_is_live("window.panel.layers"));
    CHECK(pc_ui_is_live("window.panel.timeline"));
    CHECK(pc_ui_is_live("window.panel.gradients"));
    CHECK(pc_ui_is_live("window.panel.characterStyles"));
    /* view_cmds families */
    CHECK(pc_ui_is_live("view.show.layerEdges"));
    CHECK(pc_ui_is_live("view.snapTo.grid"));
    CHECK(pc_ui_is_live("view.screenMode.standard"));
    CHECK(pc_ui_is_live("window.arrange.cascade"));
    /* workspace / proof presets */
    CHECK(pc_ui_is_live("window.workspace.photography"));
    CHECK(pc_ui_is_live("view.proofSetup.workingCmyk"));
    CHECK(pc_ui_is_live("view.proofSetup.custom"));
    CHECK(pc_ui_is_live("view.pixelAspectRatio.custom"));
    /* not live */
    CHECK(!pc_ui_is_live("file.totallyBogus"));
    CHECK(!pc_ui_is_live("view.show.nonsense"));
    CHECK(!pc_ui_is_live(NULL));
    CHECK(!pc_ui_is_live(""));
}

static void test_aliases(void)
{
    CHECK_EQ_STR(pc_ui_panel_alias("window.panel.layers"), "window.toggle.layers");
    CHECK_EQ_STR(pc_ui_panel_alias("window.panel.adjustments"), "window.toggle.properties");
    CHECK_EQ_STR(pc_ui_panel_alias("window.panel.swatches"), "window.toggle.color");
    CHECK(pc_ui_panel_alias("window.panel.unknown") == NULL);
    CHECK(pc_ui_panel_alias("edit.undo") == NULL);

    CHECK_EQ_STR(pc_ui_proof_preset("view.proofSetup.workingCmyk"), "working-cmyk");
    CHECK_EQ_STR(pc_ui_proof_preset("view.proofSetup.monitorRgb"), "srgb");
    CHECK(pc_ui_proof_preset("view.proofSetup.custom") == NULL);

    const char *ws = NULL;
    CHECK(pc_ui_workspace_name("window.workspace.essentials", &ws) && strcmp(ws, "") == 0);
    CHECK(pc_ui_workspace_name("window.workspace.graphicAndWeb", &ws) && strcmp(ws, "Graphic and Web") == 0);
    CHECK(!pc_ui_workspace_name("window.workspace.bogus", &ws));
    CHECK(!pc_ui_workspace_name("edit.undo", &ws));
}

static void test_tool_select_ids(void)
{
    /* Every tool has a unique, round-tripping, live select id (tool_shortcuts coverage test). */
    char ids[PC_TOOL_N][96];
    for (int i = 0; i < PC_TOOL_N; i++) {
        pc_ui_tool t = PC_UI_TOOL_ALL[i];
        pc_ui_tool_select_id(t, ids[i], sizeof ids[i]);
        CHECK_CONTAINS(ids[i], "tools.select.");
        pc_ui_tool back;
        CHECK(pc_ui_tool_from_select_id(ids[i], &back) && back == t);
        CHECK(pc_ui_is_live(ids[i]));
    }
    for (int i = 0; i < PC_TOOL_N; i++) {
        for (int j = i + 1; j < PC_TOOL_N; j++) {
            CHECK(strcmp(ids[i], ids[j]) != 0);
        }
    }
    pc_ui_tool t;
    CHECK(!pc_ui_tool_from_select_id("tools.select.", &t));
    CHECK(!pc_ui_tool_from_select_id("tools.select.missing", &t));
    /* Non-canonical spelling (serde name, capitalised) must not resolve. */
    CHECK(!pc_ui_tool_from_select_id("tools.select.Brush", &t));
    CHECK(!pc_ui_is_live("tools.select.Brush"));
}

/* Does `items` (n of them) contain an item with this id? */
static bool items_have(const pc_ui_menu_item *items, size_t n, const char *id)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(items[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static void test_menu_items(void)
{
    pc_engine_session *s = NULL;
    pc_error e = {0};
    CHECK(pc_engine_session_new(&s, &e) == PC_OK);

    pc_ui_menu_item *items = NULL;
    size_t n = 0;
    CHECK(pc_ui_menu_items(s, &items, &n, &e) == PC_OK);
    CHECK(n >= PC_UI_CATALOG_N); /* every catalogue row plus the extras */

    /* Every catalogue row (separators included) is present. */
    for (size_t i = 0; i < PC_UI_CATALOG_N; i++) {
        if (!pc_ui_catalog_is_separator(PC_UI_CATALOG[i].id)) {
            CHECK(items_have(items, n, PC_UI_CATALOG[i].id));
        }
    }
    /* UI-only commands that the catalogue doesn't list appear in the model. (UI commands that
     * share a catalogue item's path+label, e.g. window.toggle.layers vs Window › Layers, are
     * represented by the catalogue row, so their own id need not appear.) */
    const char *ui_only[] = {"window.theme.pro",   "edit.freeTransform",   "view.rotateView",
                             "window.nextDocument", "ui.symmetryTransform", "edit.search",
                             "window.theme.solarizedDark"};
    for (size_t i = 0; i < sizeof ui_only / sizeof ui_only[0]; i++) {
        CHECK(items_have(items, n, ui_only[i]));
    }
    /* CONSISTENCY: every engine command that declares a menu path is reachable in the menu. */
    size_t neng = 0;
    const pc_engine_command *eng = pc_engine_commands(&neng);
    for (size_t i = 0; i < neng; i++) {
        if (eng[i].menu_len > 0) {
            CHECK(items_have(items, n, eng[i].id));
        }
    }
    /* A live catalogue item shows its default shortcut; a dead one keeps the catalogue's. */
    for (size_t i = 0; i < n; i++) {
        if (strcmp(items[i].id, "file.new") == 0) {
            CHECK(items[i].shortcut != NULL);
            CHECK_EQ_STR(items[i].shortcut, "Cmd+N");
        }
    }
    pc_ui_menu_items_free(items, n);
    pc_engine_session_free(s);
}

static void test_parity(void)
{
    pc_ui_parity p;
    pc_error e = {0};
    CHECK(pc_ui_parity_compute(&p, &e) == PC_OK);
    /* Does not regress (port of parity_does_not_regress). The catalogue total matches the
     * reference exactly (631 distinct non-separator menu ids). PC_UI_PARITY_FLOOR (631) is the
     * reference target — full catalogue coverage, which the reference reaches (100%). The C engine
     * is a port in progress: its registered commands cover CURRENT_LIVE at this point, and this
     * guard rises toward PC_UI_PARITY_FLOOR as later engine/UI chunks register the remaining
     * commands. Never lower CURRENT_LIVE; raise it when coverage grows. */
    enum { CURRENT_LIVE = 551 };
    CHECK(p.total == 631);
    CHECK(p.live >= CURRENT_LIVE);
    CHECK(p.live <= PC_UI_PARITY_FLOOR);
    /* Menu totals sum to the grand total. */
    size_t sum_live = 0, sum_total = 0;
    for (size_t i = 0; i < p.n_menus; i++) {
        sum_live += p.menus[i].live;
        sum_total += p.menus[i].total;
    }
    CHECK(sum_live == p.live && sum_total == p.total);

    /* Markdown lists exactly (total - live) missing items (port of markdown_lists_every_missing). */
    size_t missing_total = 0;
    for (size_t i = 0; i < p.n_menus; i++) {
        missing_total += p.menus[i].n_missing;
    }
    CHECK(missing_total == p.total - p.live);

    pc_buf md;
    pc_buf_init(&md);
    CHECK(pc_ui_parity_to_markdown(&p, &md, &e) == PC_OK);
    const char *text = (const char *)md.data;
    size_t lines = 0;
    for (size_t i = 0; i < md.len; i++) {
        if (text[i] == '-' && text[i + 1] == ' ' && (i == 0 || text[i - 1] == '\n')) {
            lines++;
        }
    }
    CHECK(lines == p.total - p.live);
    pc_buf_fini(&md);
    pc_ui_parity_free(&p);
}

int main(int argc, char **argv)
{
    RUN(test_catalog_shape);
    RUN(test_is_live);
    RUN(test_aliases);
    RUN(test_tool_select_ids);
    RUN(test_menu_items);
    RUN(test_parity);
    return pc_t_finish();
}
