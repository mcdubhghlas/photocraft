/* U5 preset-panel state (port of the pure PresetUi tests from crates/ui-egui/src/preset_panels.rs:
 * moving_a_folder_reorders_like_the_dock_tabs, folder_order_persists_and_reads_leniently, plus the
 * Window-menu float/colour-tab helpers). The egui browser rendering is the host's job (§5/§7). */
#include "harness.h"

#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "ui/preset_ui.h"

static const char *const NAMES[4] = {"Grays", "Pastel", "Pure", "Dark"};

/* The group_order list for `panel`, or NULL. */
static const pc_ui_preset_group_order *go_of(const pc_ui_preset_ui *p, const char *panel)
{
    for (size_t i = 0; i < p->n_group_order; i++) {
        if (strcmp(p->group_order[i].panel, panel) == 0) {
            return &p->group_order[i];
        }
    }
    return NULL;
}

static bool order_is(const pc_ui_preset_ui *p, const char *panel, const char *const *want, size_t n)
{
    const pc_ui_preset_group_order *e = go_of(p, panel);
    if (e == NULL || e->n != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(e->names[i], want[i]) != 0) {
            return false;
        }
    }
    return true;
}

static bool display_is(const pc_ui_preset_ui *p, const size_t *want, size_t n)
{
    size_t out[4];
    size_t got = pc_ui_preset_ui_display_order(p, "swatches", NAMES, 4, out);
    if (got != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (out[i] != want[i]) {
            return false;
        }
    }
    return true;
}

/* #3658: a folder drag reorders by name, stores only a real reorder, and falls back to the
 * library's order when the saved order matches it or names a group that's gone. */
static void test_move_group(void)
{
    pc_ui_preset_ui st;
    pc_ui_preset_ui_init(&st);
    const size_t id0123[4] = {0, 1, 2, 3};
    CHECK(display_is(&st, id0123, 4));
    pc_ui_preset_ui_move_group(&st, "swatches", NAMES, 4, "Pastel", NULL);
    const char *w1[4] = {"Grays", "Pure", "Dark", "Pastel"};
    CHECK(order_is(&st, "swatches", w1, 4));
    pc_ui_preset_ui_move_group(&st, "swatches", NAMES, 4, "Pastel", "Grays");
    const char *w2[4] = {"Pastel", "Grays", "Pure", "Dark"};
    CHECK(order_is(&st, "swatches", w2, 4));
    const size_t id1023[4] = {1, 0, 2, 3};
    CHECK(display_is(&st, id1023, 4));
    /* Back to the library's own order: no entry stored. */
    pc_ui_preset_ui_move_group(&st, "swatches", NAMES, 4, "Pastel", "Pure");
    CHECK(go_of(&st, "swatches") == NULL);
    pc_ui_preset_ui_fini(&st);

    /* Out-of-range names, moves onto themselves and unknown `before` change nothing bad. */
    struct {
        const char *from, *before;
    } noops[2] = {{"Nope", NULL}, {"Grays", "Grays"}};
    for (size_t i = 0; i < 2; i++) {
        pc_ui_preset_ui m;
        pc_ui_preset_ui_init(&m);
        pc_ui_preset_ui_move_group(&m, "swatches", NAMES, 4, noops[i].from, noops[i].before);
        CHECK(m.n_group_order == 0);
        CHECK(display_is(&m, id0123, 4));
        pc_ui_preset_ui_fini(&m);
    }

    /* A missing group in a saved order follows the rest in library order. */
    pc_ui_preset_ui m;
    pc_ui_preset_ui_init(&m);
    const char *saved[2] = {"Dark", "Grays"};
    pc_ui_preset_ui_move_group(&m, "swatches", NAMES, 4, "Dark", "Grays"); /* build an order... */
    (void)saved;
    /* Directly exercise display_order with a saved {Dark, Grays} order via from_json-style set. */
    pc_ui_preset_ui_fini(&m);
    pc_ui_preset_ui_init(&m);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *obj = yyjson_mut_obj(d);
    yyjson_mut_val *go = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_strcpy(d, arr, "Dark");
    yyjson_mut_arr_add_strcpy(d, arr, "Grays");
    yyjson_mut_obj_add_val(d, go, "swatches", arr);
    yyjson_mut_obj_add_val(d, obj, "groupOrder", go);
    yyjson_mut_doc_set_root(d, obj);
    /* Serialize then parse (from_json takes an immutable yyjson_val). */
    pc_buf text = PC_BUF_INIT;
    CHECK(pc_json_write(obj, 0, &text, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse((const char *)text.data, text.len, &parsed, NULL) == PC_OK);
    pc_ui_preset_ui_from_json(&m, yyjson_doc_get_root(parsed));
    const size_t id3012[4] = {3, 0, 1, 2};
    CHECK(display_is(&m, id3012, 4));
    yyjson_doc_free(parsed);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    pc_ui_preset_ui_fini(&m);
}

/* Serialize a PresetUi, parse it back, re-serialize: byte-exact (round-trip over the state). */
static char *reserialize(const pc_ui_preset_ui *p)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_preset_ui_to_json(p, d, &root, NULL) == PC_OK);
    yyjson_mut_doc_set_root(d, root);
    pc_buf t = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &t, NULL) == PC_OK);
    char *s = strdup((const char *)t.data);
    pc_buf_fini(&t);
    yyjson_mut_doc_free(d);
    return s;
}

/* #3658: the folder order persists and reads leniently (an old state has no field; garbage in one
 * field never fails the whole state). */
static void test_serde_lenient(void)
{
    /* An older state without the field still loads. */
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse("{\"sizes\":{\"swatches\":22.0}}", 27, &doc, NULL) == PC_OK);
    pc_ui_preset_ui old;
    pc_ui_preset_ui_init(&old);
    pc_ui_preset_ui_from_json(&old, yyjson_doc_get_root(doc));
    CHECK(old.n_group_order == 0);
    CHECK(old.n_sizes == 1 && old.sizes[0].size == 22.0f);
    pc_ui_preset_ui_fini(&old);
    yyjson_doc_free(doc);

    /* A real order round-trips. */
    pc_ui_preset_ui st;
    pc_ui_preset_ui_init(&st);
    const char *names[2] = {"Dark", "Grays"};
    pc_ui_preset_ui_move_group(&st, "swatches", NAMES, 4, "Dark", "Grays");
    (void)names;
    char *a = reserialize(&st);
    yyjson_doc *p2 = NULL;
    CHECK(pc_json_parse(a, strlen(a), &p2, NULL) == PC_OK);
    pc_ui_preset_ui back;
    pc_ui_preset_ui_init(&back);
    pc_ui_preset_ui_from_json(&back, yyjson_doc_get_root(p2));
    char *b = reserialize(&back);
    CHECK_EQ_STR(a, b);
    CHECK(pc_ui_preset_ui_eq(&st, &back));
    free(a);
    free(b);
    yyjson_doc_free(p2);
    pc_ui_preset_ui_fini(&st);
    pc_ui_preset_ui_fini(&back);

    /* Garbage in groupOrder never fails: the panel ends up with no (or empty) order. */
    const char *bads[4] = {"{\"groupOrder\":\"x\"}", "{\"groupOrder\":7}",
                           "{\"groupOrder\":{\"swatches\":\"paths\"}}",
                           "{\"groupOrder\":{\"swatches\":[1,null,{\"a\":1}]}}"};
    for (size_t i = 0; i < 4; i++) {
        yyjson_doc *bd = NULL;
        CHECK(pc_json_parse(bads[i], strlen(bads[i]), &bd, NULL) == PC_OK);
        pc_ui_preset_ui m;
        pc_ui_preset_ui_init(&m);
        pc_ui_preset_ui_from_json(&m, yyjson_doc_get_root(bd));
        const pc_ui_preset_group_order *e = go_of(&m, "swatches");
        CHECK(e == NULL || e->n == 0);
        pc_ui_preset_ui_fini(&m);
        yyjson_doc_free(bd);
    }
}

/* Window-menu helpers: the six panel ids, the Color-card tabs and the floating-window flags. */
static void test_window_helpers(void)
{
    CHECK(pc_ui_preset_ui_handles("window.panel.gradients"));
    CHECK(pc_ui_preset_ui_handles("window.panel.cloneSource"));
    CHECK(!pc_ui_preset_ui_handles("window.panel.layers"));
    CHECK(!pc_ui_preset_ui_handles(NULL));
    size_t tab = 99;
    CHECK(pc_ui_preset_ui_color_tab("window.panel.gradients", &tab) && tab == 2);
    CHECK(pc_ui_preset_ui_color_tab("window.panel.patterns", &tab) && tab == 3);
    CHECK(!pc_ui_preset_ui_color_tab("window.panel.styles", &tab));

    pc_ui_preset_ui p;
    pc_ui_preset_ui_init(&p);
    CHECK(pc_ui_preset_ui_float_flag(&p, "window.panel.styles") == &p.styles);
    CHECK(pc_ui_preset_ui_float_flag(&p, "window.panel.shapes") == &p.shapes);
    CHECK(pc_ui_preset_ui_float_flag(&p, "window.panel.toolPresets") == &p.tool_presets);
    CHECK(pc_ui_preset_ui_float_flag(&p, "window.panel.cloneSource") == &p.clone_source);
    CHECK(pc_ui_preset_ui_float_flag(&p, "window.panel.gradients") == NULL);
    /* The default shape is Heart; a chosen one wins. */
    CHECK_EQ_STR(pc_ui_preset_ui_shape(&p), "Heart");
    free(p.custom_shape);
    p.custom_shape = strdup("Star");
    CHECK_EQ_STR(pc_ui_preset_ui_shape(&p), "Star");
    pc_ui_preset_ui_fini(&p);
}

/* U5 browser accessors (size, selection, folder open-state, inline rename) the Nuklear browser
 * drives. All NULL-safe; sizes clamp; the first folder starts open and `toggled` flips it. */
static void test_browser_accessors(void)
{
    pc_ui_preset_ui p;
    pc_ui_preset_ui_init(&p);

    /* Size: default, set, clamp, NULL-safe. */
    CHECK(pc_ui_preset_ui_size(&p, "gradients") == PC_UI_PRESET_DEFAULT_SIZE);
    pc_ui_preset_ui_set_size(&p, "gradients", 64.0f);
    CHECK(pc_ui_preset_ui_size(&p, "gradients") == 64.0f);
    pc_ui_preset_ui_set_size(&p, "gradients", 5.0f); /* below MIN -> clamped */
    CHECK(pc_ui_preset_ui_size(&p, "gradients") == PC_UI_PRESET_MIN_THUMB);
    pc_ui_preset_ui_set_size(&p, "gradients", 9999.0f); /* above 96 -> clamped */
    CHECK(pc_ui_preset_ui_size(&p, "gradients") == 96.0f);
    CHECK(pc_ui_preset_ui_size(NULL, "x") == PC_UI_PRESET_DEFAULT_SIZE);
    pc_ui_preset_ui_set_size(NULL, "x", 10); /* no crash */

    /* Selection: set, read per-panel, clear. */
    CHECK(pc_ui_preset_ui_selected(&p, "gradients") == NULL);
    pc_ui_preset_ui_set_selected(&p, "gradients", "Spectrum");
    pc_ui_preset_ui_set_selected(&p, "patterns", "pat-1");
    CHECK_EQ_STR(pc_ui_preset_ui_selected(&p, "gradients"), "Spectrum");
    CHECK_EQ_STR(pc_ui_preset_ui_selected(&p, "patterns"), "pat-1");
    pc_ui_preset_ui_clear_selected(&p, "gradients");
    CHECK(pc_ui_preset_ui_selected(&p, "gradients") == NULL);
    CHECK_EQ_STR(pc_ui_preset_ui_selected(&p, "patterns"), "pat-1"); /* other panel intact */

    /* Folder open-state: first folder open by default; swatches-all opens all; toggle flips. */
    CHECK(pc_ui_preset_ui_group_open(&p, "gradients", 0, "Basics", false));
    CHECK(!pc_ui_preset_ui_group_open(&p, "gradients", 1, "More", false));
    CHECK(pc_ui_preset_ui_group_open(&p, "swatches", 2, "Pure", true)); /* swatches: all open */
    pc_ui_preset_ui_toggle_group(&p, "gradients", "Basics");
    CHECK(!pc_ui_preset_ui_group_open(&p, "gradients", 0, "Basics", false)); /* now closed */
    pc_ui_preset_ui_toggle_group(&p, "gradients", "More");
    CHECK(pc_ui_preset_ui_group_open(&p, "gradients", 1, "More", false)); /* now open */
    pc_ui_preset_ui_toggle_group(&p, "gradients", "Basics"); /* back to default */
    CHECK(pc_ui_preset_ui_group_open(&p, "gradients", 0, "Basics", false));

    /* Inline rename: active only for its panel; text updates; clear ends it. */
    CHECK(pc_ui_preset_ui_rename_key(&p, "gradients", NULL) == NULL);
    pc_ui_preset_ui_set_rename(&p, "gradients", "Spectrum", "Spectrum");
    const char *text = NULL;
    const char *key = pc_ui_preset_ui_rename_key(&p, "gradients", &text);
    CHECK(key != NULL && strcmp(key, "Spectrum") == 0);
    CHECK_EQ_STR(text, "Spectrum");
    CHECK(pc_ui_preset_ui_rename_key(&p, "patterns", NULL) == NULL); /* other panel: none */
    pc_ui_preset_ui_set_rename_text(&p, "Rainbow");
    CHECK(pc_ui_preset_ui_rename_key(&p, "gradients", &text) != NULL);
    CHECK_EQ_STR(text, "Rainbow");
    pc_ui_preset_ui_clear_rename(&p);
    CHECK(pc_ui_preset_ui_rename_key(&p, "gradients", NULL) == NULL);

    pc_ui_preset_ui_fini(&p);
}

int main(int argc, char **argv)
{
    RUN(test_move_group);
    RUN(test_serde_lenient);
    RUN(test_window_helpers);
    RUN(test_browser_accessors);
    return pc_t_finish();
}
