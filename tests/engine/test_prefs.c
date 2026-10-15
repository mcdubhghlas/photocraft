/* Preferences, keyboard-shortcut / menu / toolbar customisation, units, pressure curve
 * (ported from the behavioural assertions of crates/engine/src/prefs/tests.rs). The colour-settings
 * (E8) and .kys import (unported crate::kys) paths are deferred and not exercised here. */
#include <math.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "harness.h"
#include "ops/ops.h"

static pc_engine_session *white(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1.0f, 1.0f, 1.0f, 1.0f),
                                      NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

/* Run a command with JSON params; returns true and fills *out on success. */
static bool exec(pc_engine_session *s, const char *id, const char *params, pc_json_value *out)
{
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, out, &err);
    return rc == PC_OK;
}

static bool exec_err(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &err);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
        return false;
    }
    return true;
}

static yyjson_mut_val *root_of(pc_json_value *v) { return pc_json_value_root(v); }

/* ---------------------------------------------------------------- defaults */

static void test_defaults(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.get", "{\"path\":\"performance.historyStates\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == 50);
    pc_json_value_fini(&out);

    CHECK(exec(s, "prefs.get", "{\"path\":\"guidesGridAndSlices.subdivisions\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == 4);
    pc_json_value_fini(&out);

    CHECK(exec(s, "prefs.get", "{\"path\":\"guidesGridAndSlices.guideColor\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "#4affff");
    pc_json_value_fini(&out);

    CHECK(exec(s, "prefs.get", "{\"path\":\"cursors.painting\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "normalTip");
    pc_json_value_fini(&out);

    CHECK(exec(s, "prefs.get", "{\"path\":\"tools.rightClickWithPaintingTools\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "brushPicker");
    pc_json_value_fini(&out);

    /* Every dialog section is an object key of the whole JSON form. */
    CHECK(exec(s, "prefs.get", "{}", &out));
    yyjson_mut_val *all = root_of(&out);
    static const char *const sections[] = {"general",     "interface",  "workspace",
                                           "performance", "cursors",    "unitsAndRulers"};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        yyjson_mut_val *sec = yyjson_mut_obj_get(all, sections[i]);
        CHECK(sec != NULL && yyjson_mut_is_obj(sec));
    }
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---------------------------------------------------------------- get/set/reset */

static void test_get_set_reset(void)
{
    pc_engine_session *s = white(32, 32);
    pc_json_value out = PC_JSON_VALUE_INIT;

    CHECK(exec(s, "prefs.set", "{\"path\":\"performance.historyStates\",\"value\":3}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"performance.historyStates\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == 3);
    pc_json_value_fini(&out);
    /* Applied to the open document's history limit. */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(pc_history_max_states(d->history) == 3);

    /* A new document inherits the preference too. */
    pc_document doc2;
    CHECK(pc_document_with_background(&doc2, "D2", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc2, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_history_max_states(pc_engine_session_active(s)->history) == 3);

    CHECK(exec(s, "prefs.reset", "{\"path\":\"performance\"}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"performance.historyStates\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == 50);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_set_validates_all_or_nothing(void)
{
    pc_engine_session *s = white(16, 16);
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"cursors.painting\",\"value\":\"huge\"}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"performance.historyStates\",\"value\":0}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"performance.historyStates\",\"value\":\"ten\"}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"guidesGridAndSlices.guideColor\",\"value\":\"cyan\"}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"nope.nothing\",\"value\":1}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"general.notAField\",\"value\":true}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"shortcuts.edit.undo\",\"value\":\"Cmd+Banana+Q\"}"));
    /* A batch with one bad value changes nothing. */
    CHECK(exec_err(s, "prefs.set",
                   "{\"values\":{\"cursors.painting\":\"precise\",\"performance.historyStates\":-4}}"));
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.get", "{\"path\":\"cursors.painting\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "normalTip");
    pc_json_value_fini(&out);
    /* A fully valid batch applies. */
    CHECK(exec(s, "prefs.set",
               "{\"values\":{\"cursors.painting\":\"precise\",\"unitsAndRulers.rulers\":\"cm\"}}",
               &out));
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root_of(&out), "cursors.painting")), "precise");
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"unitsAndRulers.rulers\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "cm");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_whole_section_merge(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.set",
               "{\"path\":\"transparencyAndGamut\",\"value\":{\"gridSize\":\"large\","
               "\"gridColors\":\"custom\",\"customDark\":\"#336699\"}}",
               &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"transparencyAndGamut.gridSize\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "large");
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"transparencyAndGamut.customDark\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "#336699");
    pc_json_value_fini(&out);
    /* Untouched keys keep their values. */
    CHECK(exec(s, "prefs.get", "{\"path\":\"transparencyAndGamut.gamutWarningColor\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "#808080");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_theme_side_effect(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.set", "{\"path\":\"interface.theme\",\"value\":\"studioLight\"}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"interface.appearanceMode\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "light");
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"interface.lightTheme\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "studioLight");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---------------------------------------------------------------- persistence */

static void test_json_round_trip(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.set",
               "{\"values\":{\"interface.theme\":\"studioLight\",\"fileHandling.autosaveMinutes\":5,"
               "\"performance.gpuBackend\":\"dx12\",\"shortcuts.edit.undo\":\"Cmd+Shift+F\"}}",
               &out));
    pc_json_value_fini(&out);
    char *text = pc_engine_session_prefs_to_json(s);
    CHECK(text != NULL);
    size_t tlen = text != NULL ? strlen(text) : 0;

    pc_engine_session *t = NULL;
    CHECK(pc_engine_session_new(&t, NULL) == PC_OK);
    CHECK(pc_engine_session_load_prefs_json(t, text, tlen, NULL) == PC_OK);
    CHECK(exec(t, "prefs.get", "{\"path\":\"performance.gpuBackend\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "dx12");
    pc_json_value_fini(&out);
    CHECK(exec(t, "prefs.get", "{\"path\":\"fileHandling.autosaveMinutes\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == 5);
    pc_json_value_fini(&out);
    CHECK(exec(t, "prefs.get", "{\"path\":\"interface.appearanceMode\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "light");
    pc_json_value_fini(&out);
    free(text);

    /* Older/newer files: unknown keys ignored, missing ones default. */
    pc_engine_session *u = NULL;
    CHECK(pc_engine_session_new(&u, NULL) == PC_OK);
    const char *older = "{\"general\": {\"beepWhenDone\": true, \"futureThing\": 1}, "
                        "\"someNewSection\": {}}";
    CHECK(pc_engine_session_load_prefs_json(u, older, strlen(older), NULL) == PC_OK);
    CHECK(exec(u, "prefs.get", "{\"path\":\"general.beepWhenDone\"}", &out));
    CHECK(yyjson_mut_get_bool(root_of(&out)) == true);
    pc_json_value_fini(&out);
    CHECK(exec(u, "prefs.get", "{\"path\":\"performance.historyStates\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == 50);
    pc_json_value_fini(&out);
    /* Not JSON is an error, never a crash. */
    pc_error err = {0};
    CHECK(pc_engine_session_load_prefs_json(u, "not json", 8, &err) == PC_ERR_INVALID);

    pc_engine_session_free(s);
    pc_engine_session_free(t);
    pc_engine_session_free(u);
}

static void test_legacy_theme_migration(void)
{
    /* A preferences file from before appearance modes keeps its appearance (dark here). */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const char *legacy = "{\"interface\": {\"theme\": \"solarizedDark\"}}";
    CHECK(pc_engine_session_load_prefs_json(s, legacy, strlen(legacy), NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.get", "{\"path\":\"interface.appearanceMode\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "dark");
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"interface.darkTheme\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "solarizedDark");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---------------------------------------------------------------- units & formatting */

static void test_units(void)
{
    pc_pref_unit u[] = {PC_UNIT_PIXELS,  PC_UNIT_INCHES, PC_UNIT_CENTIMETERS, PC_UNIT_MILLIMETERS,
                        PC_UNIT_POINTS,  PC_UNIT_PICAS,  PC_UNIT_PERCENT};
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) {
        double v = pc_pref_unit_from_px(u[i], 450.0, 300.0, 900.0, 72.0);
        double back = pc_pref_unit_to_px(u[i], v, 300.0, 900.0, 72.0);
        CHECK(fabs(back - 450.0) < 1e-9);
    }
    CHECK(pc_pref_unit_from_px(PC_UNIT_INCHES, 600.0, 300.0, 0.0, 72.0) == 2.0);
    CHECK(pc_pref_unit_from_px(PC_UNIT_POINTS, 300.0, 300.0, 0.0, 72.0) == 72.0);
    CHECK(pc_pref_unit_from_px(PC_UNIT_PERCENT, 450.0, 300.0, 900.0, 72.0) == 50.0);
    CHECK(pc_pref_unit_decimals(PC_UNIT_PIXELS) == 0);
    CHECK(pc_pref_unit_decimals(PC_UNIT_POINTS) == 1);
    CHECK(pc_pref_unit_decimals(PC_UNIT_INCHES) == 3);
}

static void test_fmt_decimals(void)
{
    char buf[64];
    pc_pref_fmt_decimals(0.7995, 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.8");
    pc_pref_fmt_decimals(0.799, 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.799");
    pc_pref_fmt_decimals(0.799, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.8");
    pc_pref_fmt_decimals(2.0, 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "2");
    pc_pref_fmt_decimals(1234.5678, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1234.57");
    pc_pref_fmt_decimals(0.125, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.13");
    pc_pref_fmt_decimals(-0.125, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-0.13");
    pc_pref_fmt_decimals(-0.004, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0");
    pc_pref_fmt_decimals(550.0, 0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "550");
    /* 1500 px at 300 dpi = 5 in / 12.7 cm. */
    double in = pc_pref_unit_from_px(PC_UNIT_INCHES, 1500.0, 300.0, 1500.0, 72.0);
    pc_pref_fmt_decimals(in, pc_pref_unit_decimals(PC_UNIT_INCHES), buf, sizeof buf);
    CHECK_EQ_STR(buf, "5");
    double cm = pc_pref_unit_from_px(PC_UNIT_CENTIMETERS, 1500.0, 300.0, 1500.0, 72.0);
    pc_pref_fmt_decimals(cm, pc_pref_unit_decimals(PC_UNIT_CENTIMETERS), buf, sizeof buf);
    CHECK_EQ_STR(buf, "12.7");
}

/* ---------------------------------------------------------------- shortcuts */

static void test_normalize_shortcut(void)
{
    char buf[64];
    CHECK(pc_pref_normalize_shortcut("shift+cmd+n", 11, buf, sizeof buf));
    CHECK_EQ_STR(buf, "Cmd+Shift+N");
    CHECK(pc_pref_normalize_shortcut("Alt+Cmd+f7", 10, buf, sizeof buf));
    CHECK_EQ_STR(buf, "Cmd+Alt+F7");
    CHECK(pc_pref_normalize_shortcut("Cmd++", 5, buf, sizeof buf));
    CHECK_EQ_STR(buf, "Cmd++");
    CHECK(!pc_pref_normalize_shortcut("Cmd+Shift", 9, buf, sizeof buf));
    CHECK(!pc_pref_normalize_shortcut("Cmd+A+B", 7, buf, sizeof buf));
}

static void test_keyboard_shortcuts_command(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out = PC_JSON_VALUE_INIT;
    /* edit.undo defaults to Cmd+Z. */
    CHECK(exec(s, "edit.keyboardShortcuts", "{\"list\":true}", &out));
    pc_json_value_fini(&out);
    /* Give Cmd+Z to edit.redo (default removeConflicts=true): edit.undo loses it. */
    CHECK(exec(s, "edit.keyboardShortcuts", "{\"set\":{\"edit.redo\":\"Cmd+Z\"}}", &out));
    yyjson_mut_val *conflicts = yyjson_mut_obj_get(root_of(&out), "conflicts");
    CHECK(conflicts != NULL && yyjson_mut_arr_size(conflicts) == 0);
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"shortcuts.edit.undo\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), ""); /* stripped (disabled) */
    pc_json_value_fini(&out);

    /* Keeping the duplicate reports a conflict. */
    CHECK(exec(s, "edit.keyboardShortcuts",
               "{\"set\":{\"edit.undo\":\"Cmd+Z\"},\"removeConflicts\":false}", &out));
    conflicts = yyjson_mut_obj_get(root_of(&out), "conflicts");
    CHECK(conflicts != NULL && yyjson_mut_arr_size(conflicts) >= 1);
    yyjson_mut_val *c0 = yyjson_mut_arr_get(conflicts, 0);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(c0, "shortcut")), "Cmd+Z");
    pc_json_value_fini(&out);

    /* An unknown command is rejected. */
    CHECK(exec_err(s, "edit.keyboardShortcuts", "{\"set\":{\"no.such.command\":\"F9\"}}"));
    /* A non-string value is rejected. */
    CHECK(exec_err(s, "edit.keyboardShortcuts", "{\"set\":{\"tools.temporary.hand\":7}}"));

    /* reset:true clears overrides; the default comes back. */
    CHECK(exec(s, "edit.keyboardShortcuts", "{\"reset\":true}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"shortcuts\"}", &out));
    CHECK(yyjson_mut_obj_size(root_of(&out)) == 0);
    pc_json_value_fini(&out);

    /* Temporary tools are listed (held keys) and are bindable. */
    CHECK(exec(s, "edit.keyboardShortcuts", "{\"filter\":\"tools.temporary\"}", &out));
    yyjson_mut_val *cmds = yyjson_mut_obj_get(root_of(&out), "commands");
    bool saw_hand = false, all_hold = true;
    size_t idx, max;
    yyjson_mut_val *row;
    yyjson_mut_arr_foreach(cmds, idx, max, row) {
        const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(row, "id"));
        if (id != NULL && strcmp(id, "tools.temporary.hand") == 0) {
            saw_hand = true;
        }
        yyjson_mut_val *hold = yyjson_mut_obj_get(row, "hold");
        if (hold == NULL || !yyjson_mut_get_bool(hold)) {
            all_hold = false;
        }
    }
    CHECK(saw_hand && all_hold);
    pc_json_value_fini(&out);
    CHECK(exec(s, "edit.keyboardShortcuts", "{\"set\":{\"tools.temporary.zoomIn\":\"Ctrl+Space\"}}",
               &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"shortcuts.tools.temporary.zoomIn\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "Ctrl+Space");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---------------------------------------------------------------- menus / toolbar */

static void test_menus_and_toolbar(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "edit.menus", "{\"hide\":[\"edit.fade\"],\"color\":{\"edit.fill\":\"red\"}}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"menus.hidden\"}", &out));
    CHECK(yyjson_mut_arr_size(root_of(&out)) == 1);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(root_of(&out), 0)), "edit.fade");
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"menus.colors.edit.fill\"}", &out));
    CHECK_EQ_STR(yyjson_mut_get_str(root_of(&out)), "red");
    pc_json_value_fini(&out);
    /* An unknown colour is rejected. */
    CHECK(exec_err(s, "edit.menus", "{\"color\":{\"edit.fill\":\"plaid\"}}"));
    /* show + colour none clears. */
    CHECK(exec(s, "edit.menus", "{\"show\":[\"edit.fade\"],\"color\":{\"edit.fill\":\"none\"}}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"menus.hidden\"}", &out));
    CHECK(yyjson_mut_arr_size(root_of(&out)) == 0);
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.get", "{\"path\":\"menus.colors\"}", &out));
    CHECK(yyjson_mut_obj_size(root_of(&out)) == 0);
    pc_json_value_fini(&out);

    /* Toolbar persists through prefs_to_json. */
    CHECK(exec(s, "edit.toolbar", "{\"hidden\":[\"Sponge\"]}", &out));
    pc_json_value_fini(&out);
    char *text = pc_engine_session_prefs_to_json(s);
    CHECK(text != NULL);
    size_t tlen = text != NULL ? strlen(text) : 0;
    pc_engine_session *t = NULL;
    CHECK(pc_engine_session_new(&t, NULL) == PC_OK);
    CHECK(pc_engine_session_load_prefs_json(t, text, tlen, NULL) == PC_OK);
    free(text);
    CHECK(exec(t, "prefs.get", "{\"path\":\"toolbar.hidden\"}", &out));
    CHECK(yyjson_mut_arr_size(root_of(&out)) == 1);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(root_of(&out), 0)), "Sponge");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
    pc_engine_session_free(t);
}

static void test_sections_are_commands(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    static const struct {
        const char *id, *title;
    } sec[] = {
        {"general", "General"}, {"interface", "Interface"}, {"performance", "Performance"},
        {"rawDefaults", "Camera Raw Defaults"}, {"integrations", "Integrations"},
    };
    pc_json_value out = PC_JSON_VALUE_INIT;
    for (size_t i = 0; i < sizeof sec / sizeof sec[0]; i++) {
        char id[64];
        snprintf(id, sizeof id, "edit.preferences.%s", sec[i].id);
        CHECK(exec(s, id, "{}", &out));
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root_of(&out), "section")), sec[i].id);
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root_of(&out), "title")), sec[i].title);
        CHECK(yyjson_mut_is_obj(yyjson_mut_obj_get(root_of(&out), "values")));
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void test_choice_and_range_cover_defaults(void)
{
    /* Every enum/range/colour default is valid for its own table (ported coverage check). */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value whole = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.get", "{}", &whole));
    static const char *const ids[] = {"general",     "interface",  "performance", "cursors",
                                      "transparencyAndGamut", "unitsAndRulers",
                                      "guidesGridAndSlices", "rawDefaults"};
    for (size_t si = 0; si < sizeof ids / sizeof ids[0]; si++) {
        yyjson_mut_val *sec = yyjson_mut_obj_get(root_of(&whole), ids[si]);
        CHECK(sec != NULL && yyjson_mut_is_obj(sec));
        if (sec == NULL || !yyjson_mut_is_obj(sec)) {
            continue;
        }
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(sec, &it);
        yyjson_mut_val *key;
        while ((key = yyjson_mut_obj_iter_next(&it)) != NULL) {
            yyjson_mut_val *val = yyjson_mut_obj_iter_get_val(key);
            char path[192];
            snprintf(path, sizeof path, "%s.%s", ids[si], yyjson_mut_get_str(key));
            size_t cn;
            const char *const *choices = pc_pref_choices(path, &cn);
            if (choices != NULL && !yyjson_mut_is_null(val)) {
                const char *s2 = yyjson_mut_get_str(val);
                bool ok = false;
                for (size_t i = 0; i < cn; i++) {
                    if (s2 != NULL && strcmp(choices[i], s2) == 0) {
                        ok = true;
                    }
                }
                CHECK(ok);
            }
            double lo, hi;
            if (pc_pref_range(path, &lo, &hi)) {
                double x = yyjson_mut_get_num(val);
                CHECK(x >= lo && x <= hi);
            }
            if (pc_pref_is_color(path)) {
                uint8_t rgb[3];
                const char *s2 = yyjson_mut_get_str(val);
                CHECK(s2 != NULL && pc_pref_parse_hex(s2, strlen(s2), rgb));
            }
        }
    }
    pc_json_value_fini(&whole);
    pc_engine_session_free(s);
}

static void test_rasterize_svg_bound(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out = PC_JSON_VALUE_INIT;
    const char *pref = "fileHandling.rasterizeSvgGroupsDeeperThan";
    CHECK(exec(s, "prefs.get", "{\"path\":\"fileHandling.rasterizeSvgGroupsDeeperThan\"}", &out));
    CHECK(yyjson_mut_get_uint(root_of(&out)) == (uint64_t)PC_MAX_GROUP_DEPTH);
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.set",
               "{\"path\":\"fileHandling.rasterizeSvgGroupsDeeperThan\",\"value\":4}", &out));
    pc_json_value_fini(&out);
    CHECK(exec(s, "prefs.set",
               "{\"path\":\"fileHandling.rasterizeSvgGroupsDeeperThan\",\"value\":0}", &out));
    pc_json_value_fini(&out);
    char over[96];
    snprintf(over, sizeof over, "{\"path\":\"%s\",\"value\":%d}", pref, PC_MAX_GROUP_DEPTH + 1);
    CHECK(exec_err(s, "prefs.set", over));
    pc_engine_session_free(s);
}

/* ---------------------------------------------------------------- pressure curve */

static void test_pressure_curve(void)
{
    pc_pressure_curve c;
    float linear[2][2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    pc_pressure_curve_init(&c, (const float(*)[2])linear, 2);
    CHECK(pc_pressure_curve_is_linear(&c));
    float xs[] = {0.0f, 0.1f, 0.37f, 0.5f, 0.99f, 1.0f};
    for (size_t i = 0; i < sizeof xs / sizeof xs[0]; i++) {
        CHECK(fabsf(pc_pressure_curve_eval(&c, xs[i]) - xs[i]) < 1e-6f);
    }
    /* A soft curve passes through its points and stays monotone. */
    float soft[4][2] = {{0.0f, 0.0f}, {0.25f, 0.5f}, {0.6f, 0.8f}, {1.0f, 1.0f}};
    pc_pressure_curve_init(&c, (const float(*)[2])soft, 4);
    for (int i = 0; i < 4; i++) {
        CHECK(fabsf(pc_pressure_curve_eval(&c, soft[i][0]) - soft[i][1]) < 1e-5f);
    }
    float prev = -1.0f;
    for (int i = 0; i <= 200; i++) {
        float y = pc_pressure_curve_eval(&c, (float)i / 200.0f);
        CHECK(y >= 0.0f && y <= 1.0f);
        CHECK(y >= prev - 1e-6f);
        prev = y;
    }
    /* Flat outside the points. */
    float ends[2][2] = {{0.2f, 0.3f}, {0.8f, 0.9f}};
    pc_pressure_curve_init(&c, (const float(*)[2])ends, 2);
    CHECK(fabsf(pc_pressure_curve_eval(&c, 0.0f) - 0.3f) < 1e-6f);
    CHECK(fabsf(pc_pressure_curve_eval(&c, 1.0f) - 0.9f) < 1e-6f);
    /* Two points at the same input: the later output wins. */
    float dup[4][2] = {{0.0f, 0.0f}, {0.5f, 0.2f}, {0.5f, 0.7f}, {1.0f, 1.0f}};
    pc_pressure_curve_init(&c, (const float(*)[2])dup, 4);
    CHECK(fabsf(pc_pressure_curve_eval(&c, 0.5f) - 0.7f) < 1e-6f);
}

static void test_hostile_pressure_curve(void)
{
    /* NaN/inf, out-of-range, reversed, too many, empty: always finite, in 0..1, monotone. */
    float nanf_ = (float)NAN, inf = (float)INFINITY;
    float a[3][2] = {{nanf_, 0.5f}, {inf, 1.0f}, {0.3f, -inf}};
    float b[3][2] = {{2.0f, -1.0f}, {-3.0f, 5.0f}, {0.5f, 0.2f}};
    float cc[2][2] = {{0.9f, 0.1f}, {0.1f, 0.9f}};
    float single[1][2] = {{0.5f, 0.5f}};
    float many[100][2];
    for (int i = 0; i < 100; i++) {
        many[i][0] = (float)((i * 37) % 100) / 99.0f;
        many[i][1] = (float)((i * 53) % 100) / 99.0f;
    }
    struct {
        const float (*pts)[2];
        size_t n;
    } cases[] = {{NULL, 0},
                 {(const float(*)[2])single, 1},
                 {(const float(*)[2])a, 3},
                 {(const float(*)[2])b, 3},
                 {(const float(*)[2])cc, 2},
                 {(const float(*)[2])many, 100}};
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        pc_pressure_curve c;
        pc_pressure_curve_init(&c, cases[k].pts, cases[k].n);
        float prev = -1.0f;
        for (int i = 0; i <= 100; i++) {
            float y = pc_pressure_curve_eval(&c, (float)i / 100.0f);
            CHECK(isfinite(y) && y >= 0.0f && y <= 1.0f);
            CHECK(y >= prev - 1e-6f);
            prev = y;
        }
        float hostile_x[] = {nanf_, -1.0f, 2.0f, inf};
        for (size_t i = 0; i < sizeof hostile_x / sizeof hostile_x[0]; i++) {
            float y = pc_pressure_curve_eval(&c, hostile_x[i]);
            CHECK(y >= 0.0f && y <= 1.0f);
        }
    }
    /* prefs.set validates the curve shape. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(exec(s, "prefs.set",
               "{\"path\":\"tools.pressureCurve\",\"value\":[[0.0,0.1],[0.5,0.7],[1.0,1.0]]}", &out));
    pc_json_value_fini(&out);
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"tools.pressureCurve\",\"value\":\"linear\"}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"tools.pressureCurve\",\"value\":[[0.0,0.0]]}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"tools.pressureCurve\",\"value\":[[0.0,0.0],[1.5,1.0]]}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"tools.pressureCurve\",\"value\":[[0.0,0.0],[1.0]]}"));
    CHECK(exec_err(s, "prefs.set", "{\"path\":\"tools.pressureCurve\",\"value\":[[0.0,0.0],[\"a\",1.0]]}"));
    pc_engine_session_free(s);
}

/* ---------------------------------------------------------------- never-crash / hostile */

static void test_hostile_params_never_crash(void)
{
    pc_engine_session *s = white(16, 16);
    /* Adversarial params to every command E2 adds: all must return without crashing. */
    static const char *const ids[] = {"prefs.get",  "prefs.set", "prefs.reset", "edit.menus",
                                      "edit.toolbar", "edit.keyboardShortcuts",
                                      "edit.preferences.general"};
    static const char *const params[] = {
        "{}",
        "{\"path\":123}",
        "{\"path\":\"\"}",
        "{\"value\":null}",
        "{\"values\":[]}",
        "{\"values\":{\"a.b.c.d.e\":1}}",
        "{\"path\":\"performance.historyStates\",\"value\":1e300}",
        "{\"set\":42}",
        "{\"color\":{\"x\":5}}",
        "{\"hide\":\"notarray\"}",
        "{\"reset\":{}}",
        "{\"path\":\"interface.favoriteThemes\",\"value\":[\"pro\",\"pro\"]}",
        "{\"path\":\"interface.language\",\"value\":\"this is not a code!!\"}",
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status rc = pc_engine_session_execute_json(s, ids[i], params[j], &out, &err);
            if (rc == PC_OK) {
                pc_json_value_fini(&out);
            }
        }
    }
    /* Corrupt/hostile preference files are ignored or error, never crash. */
    static const char *const files[] = {
        "",
        "null",
        "[]",
        "{\"interface\":42}",
        "{\"performance\":{\"historyStates\":\"x\"}}",
        "{\"tools\":{\"pressureCurve\":\"nope\"}}",
        "{\"interface\":{\"theme\":99}}",
        "{\"shortcuts\":{\"edit.undo\":[1,2,3]}}",
        "{\"menus\":{\"hidden\":\"x\",\"colors\":5}}",
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        pc_engine_session *t = NULL;
        CHECK(pc_engine_session_new(&t, NULL) == PC_OK);
        pc_error err = {0};
        (void)pc_engine_session_load_prefs_json(t, files[i], strlen(files[i]), &err);
        /* After any load the session is still usable, including commands that edit the maps a
         * corrupt file may have left mis-shaped. */
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(exec(t, "prefs.get", "{\"path\":\"performance.historyStates\"}", &out));
        pc_json_value_fini(&out);
        pc_error e2 = {0};
        pc_json_value o2 = PC_JSON_VALUE_INIT;
        if (pc_engine_session_execute_json(t, "edit.menus", "{\"hide\":[\"x\"]}", &o2, &e2) == PC_OK) {
            pc_json_value_fini(&o2);
        }
        if (pc_engine_session_execute_json(t, "edit.toolbar", "{\"hidden\":[\"y\"]}", &o2, &e2) ==
            PC_OK) {
            pc_json_value_fini(&o2);
        }
        pc_engine_session_free(t);
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_defaults);
    RUN(test_get_set_reset);
    RUN(test_set_validates_all_or_nothing);
    RUN(test_whole_section_merge);
    RUN(test_theme_side_effect);
    RUN(test_json_round_trip);
    RUN(test_legacy_theme_migration);
    RUN(test_units);
    RUN(test_fmt_decimals);
    RUN(test_normalize_shortcut);
    RUN(test_keyboard_shortcuts_command);
    RUN(test_menus_and_toolbar);
    RUN(test_sections_are_commands);
    RUN(test_choice_and_range_cover_defaults);
    RUN(test_rasterize_svg_bound);
    RUN(test_pressure_curve);
    RUN(test_hostile_pressure_curve);
    RUN(test_hostile_params_never_crash);
    return pc_t_finish();
}
