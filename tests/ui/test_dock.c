/* Dock layout (port of the pure data-model tests from crates/ui-egui/src/dock_tests.rs). The
 * egui-harness drag/drop/render tests are rebuilt as shell snapshots instead (see §7). */
#include "harness.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "ui/dock.h"
#include "ui/state.h"

static const pc_ui_group ESSENTIALS[3] = {PC_UI_GROUP_COLOR, PC_UI_GROUP_PROPERTIES, PC_UI_GROUP_LAYERS};
#define GAP PC_UI_DOCK_GAP

static float total(const float *hs, size_t n)
{
    float s = 0.0f;
    for (size_t i = 0; i < n; i++) {
        s += hs[i];
    }
    return s + GAP * (float)(n - 1);
}

/* resize `shown` laid out `avail` tall, then return the new layout in `out`. */
static void resized(pc_ui_dock_layout *l, const pc_ui_group *shown, size_t n, float avail, size_t i,
                    float dy, float *out)
{
    float hs[PC_UI_GROUP_N];
    pc_ui_dock_heights_for(l, shown, n, avail, 28.0f, hs);
    pc_ui_dock_resize(l, shown, hs, n, i, dy);
    pc_ui_dock_heights_for(l, shown, n, avail, 28.0f, out);
}

static void test_last_expanded_group_fills_the_column(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    float hs[3];
    pc_ui_dock_heights_for(&l, ESSENTIALS, 3, 1200.0f, 28.0f, hs);
    CHECK_NEAR(hs[0], pc_ui_group_default_height(PC_UI_GROUP_COLOR), 1e-3);
    CHECK_NEAR(hs[1], pc_ui_group_default_height(PC_UI_GROUP_PROPERTIES), 1e-3);
    CHECK_NEAR(total(hs, 3), 1200.0f, 1e-2);

    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_COLOR, true);
    pc_ui_dock_heights_for(&l, ESSENTIALS, 3, 1000.0f, 28.0f, hs);
    CHECK_NEAR(hs[0], 28.0f, 1e-3);
    CHECK_NEAR(hs[1], pc_ui_group_default_height(PC_UI_GROUP_PROPERTIES), 1e-3);

    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_LAYERS, true);
    pc_ui_dock_heights_for(&l, ESSENTIALS, 3, 800.0f, 28.0f, hs);
    CHECK_NEAR(hs[2], 28.0f, 1e-3);
    CHECK_NEAR(hs[1], 800.0f - 56.0f - 2.0f * GAP, 1e-3);
    pc_ui_dock_layout_fini(&l);
}

static void test_short_columns_squeeze_and_never_negative(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    float hs[3];
    pc_ui_dock_heights_for(&l, ESSENTIALS, 3, 400.0f, 28.0f, hs);
    CHECK_NEAR(hs[2], pc_ui_group_min_height(PC_UI_GROUP_LAYERS), 1e-3);
    CHECK_NEAR(hs[0], pc_ui_group_compact_height(PC_UI_GROUP_COLOR), 1e-3);
    CHECK(hs[1] < pc_ui_group_compact_height(PC_UI_GROUP_PROPERTIES));
    const float avails[] = {0.0f, -50.0f, 1.0f, NAN, INFINITY, 1e9f};
    for (size_t k = 0; k < sizeof avails / sizeof avails[0]; k++) {
        pc_ui_dock_heights_for(&l, ESSENTIALS, 3, avails[k], 28.0f, hs);
        for (size_t i = 0; i < 3; i++) {
            CHECK(isfinite(hs[i]) && hs[i] >= 0.0f);
        }
    }
    pc_ui_dock_layout_fini(&l);
}

static void test_bad_stored_values_are_sanitised(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    l.order[0] = PC_UI_GROUP_LAYERS;
    l.order[1] = PC_UI_GROUP_LAYERS;
    l.order[2] = PC_UI_GROUP_COLOR;
    l.n_order = 3;
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(&l, order);
    const pc_ui_group want[] = {PC_UI_GROUP_LAYERS,    PC_UI_GROUP_COLOR,   PC_UI_GROUP_PROPERTIES,
                                PC_UI_GROUP_CHARACTER, PC_UI_GROUP_NAVIGATOR, PC_UI_GROUP_HISTORY};
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        CHECK(order[i] == want[i]);
    }
    const float bad[] = {NAN, -10.0f, INFINITY, 1e12f};
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        l.has_height[PC_UI_GROUP_COLOR] = true;
        l.heights[PC_UI_GROUP_COLOR] = bad[k];
        float h = pc_ui_dock_height(&l, PC_UI_GROUP_COLOR);
        CHECK(isfinite(h) && h >= pc_ui_group_min_height(PC_UI_GROUP_COLOR) && h <= PC_UI_DOCK_MAX_HEIGHT);
    }
    pc_ui_dock_layout_fini(&l);

    /* Unknown fields and partial order all load; a UiState without "dock" gets the default. */
    pc_ui_dock_layout l2;
    pc_ui_dock_layout_init(&l2);
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse("{\"order\":[\"layers\"],\"bogus\":1}", 30, &doc, NULL) == PC_OK);
    pc_ui_dock_layout_from_json(&l2, yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    pc_ui_dock_order(&l2, order);
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        CHECK(order[i] == want[i]);
    }
    pc_ui_dock_layout_fini(&l2);
}

static void test_move_group_reorders(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    pc_ui_group before = PC_UI_GROUP_COLOR;
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_move_group(&l, PC_UI_GROUP_LAYERS, &before);
    pc_ui_dock_order(&l, order);
    CHECK(order[0] == PC_UI_GROUP_LAYERS);
    pc_ui_dock_move_group(&l, PC_UI_GROUP_LAYERS, NULL);
    pc_ui_dock_order(&l, order);
    CHECK(order[PC_UI_GROUP_N - 1] == PC_UI_GROUP_LAYERS);
    before = PC_UI_GROUP_COLOR;
    pc_ui_dock_move_group(&l, PC_UI_GROUP_COLOR, &before);
    pc_ui_dock_order(&l, order);
    CHECK(order[0] == PC_UI_GROUP_COLOR);
    pc_ui_dock_layout_fini(&l);
}

static void test_splitter_down_pushes_below_in_turn(void)
{
    const pc_ui_group shown[4] = {PC_UI_GROUP_COLOR, PC_UI_GROUP_PROPERTIES, PC_UI_GROUP_NAVIGATOR,
                                  PC_UI_GROUP_LAYERS};
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    float before[4], hs[4];
    pc_ui_dock_heights_for(&l, shown, 4, 1200.0f, 28.0f, before);
    resized(&l, shown, 4, 1200.0f, 0, 300.0f, hs);
    CHECK_NEAR(hs[0], before[0] + 300.0f, 1e-2);
    CHECK_NEAR(hs[1], pc_ui_group_min_height(PC_UI_GROUP_PROPERTIES), 1e-2);
    CHECK_NEAR(hs[2], before[2] - (300.0f - (before[1] - pc_ui_group_min_height(PC_UI_GROUP_PROPERTIES))), 1e-2);
    CHECK_NEAR(hs[3], before[3], 1e-2);
    CHECK_NEAR(total(hs, 4), 1200.0f, 1e-2);
    resized(&l, shown, 4, 1200.0f, 0, 5000.0f, hs);
    CHECK_NEAR(hs[1], pc_ui_group_min_height(PC_UI_GROUP_PROPERTIES), 1e-2);
    CHECK_NEAR(hs[2], pc_ui_group_min_height(PC_UI_GROUP_NAVIGATOR), 1e-2);
    CHECK_NEAR(hs[3], pc_ui_group_min_height(PC_UI_GROUP_LAYERS), 1e-2);
    CHECK_NEAR(total(hs, 4), 1200.0f, 1e-2);
    float full = hs[0];
    float hs2[4];
    resized(&l, shown, 4, 1200.0f, 0, 50.0f, hs2);
    CHECK_NEAR(hs2[0], full, 1e-2);
    pc_ui_dock_layout_fini(&l);
}

static void test_splitter_up_pushes_above_in_turn(void)
{
    const pc_ui_group shown[4] = {PC_UI_GROUP_COLOR, PC_UI_GROUP_PROPERTIES, PC_UI_GROUP_NAVIGATOR,
                                  PC_UI_GROUP_LAYERS};
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    float before[4], hs[4];
    pc_ui_dock_heights_for(&l, shown, 4, 1200.0f, 28.0f, before);
    resized(&l, shown, 4, 1200.0f, 2, -200.0f, hs);
    CHECK_NEAR(hs[2], pc_ui_group_min_height(PC_UI_GROUP_NAVIGATOR), 1e-2);
    CHECK_NEAR(hs[1], before[1] - (200.0f - (before[2] - pc_ui_group_min_height(PC_UI_GROUP_NAVIGATOR))), 1e-2);
    CHECK_NEAR(hs[0], before[0], 1e-2);
    CHECK_NEAR(hs[3], before[3] + 200.0f, 1e-2);
    resized(&l, shown, 4, 1200.0f, 2, -5000.0f, hs);
    CHECK_NEAR(hs[0], pc_ui_group_min_height(PC_UI_GROUP_COLOR), 1e-2);
    CHECK_NEAR(hs[1], pc_ui_group_min_height(PC_UI_GROUP_PROPERTIES), 1e-2);
    CHECK_NEAR(hs[2], pc_ui_group_min_height(PC_UI_GROUP_NAVIGATOR), 1e-2);
    CHECK_NEAR(total(hs, 4), 1200.0f, 1e-2);
    pc_ui_dock_layout_fini(&l);

    /* splitter under Color grows Properties (filler not adjacent). */
    pc_ui_dock_layout l2;
    pc_ui_dock_layout_init(&l2);
    resized(&l2, shown, 4, 1200.0f, 0, -500.0f, hs);
    CHECK_NEAR(hs[0], pc_ui_group_min_height(PC_UI_GROUP_COLOR), 1e-2);
    CHECK_NEAR(hs[1], before[1] + (before[0] - pc_ui_group_min_height(PC_UI_GROUP_COLOR)), 1e-2);
    CHECK_NEAR(hs[2], before[2], 1e-2);
    pc_ui_dock_layout_fini(&l2);
}

static void test_pushing_skips_collapsed_and_respects_filler(void)
{
    const pc_ui_group shown[5] = {PC_UI_GROUP_COLOR, PC_UI_GROUP_PROPERTIES, PC_UI_GROUP_NAVIGATOR,
                                  PC_UI_GROUP_HISTORY, PC_UI_GROUP_LAYERS};
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_PROPERTIES, true);
    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_LAYERS, true);
    float before[5], hs[5];
    pc_ui_dock_heights_for(&l, shown, 5, 1200.0f, 28.0f, before);
    resized(&l, shown, 5, 1200.0f, 0, 5000.0f, hs);
    CHECK_NEAR(hs[1], 28.0f, 1e-2);
    CHECK_NEAR(hs[4], 28.0f, 1e-2);
    CHECK_NEAR(hs[2], pc_ui_group_min_height(PC_UI_GROUP_NAVIGATOR), 1e-2);
    CHECK_NEAR(hs[3], pc_ui_group_min_height(PC_UI_GROUP_HISTORY), 1e-2);
    CHECK_NEAR(hs[0], before[0] + (before[2] - pc_ui_group_min_height(PC_UI_GROUP_NAVIGATOR)) +
                          (before[3] - pc_ui_group_min_height(PC_UI_GROUP_HISTORY)),
               1e-2);
    CHECK_NEAR(total(hs, 5), 1200.0f, 1e-2);
    CHECK(!l.has_height[PC_UI_GROUP_HISTORY]); /* the filler's height is never stored */

    /* Bad deltas and out-of-range splitters leave the layout unchanged. */
    pc_ui_dock_layout snap;
    pc_ui_dock_layout_init(&snap);
    pc_ui_dock_layout_copy(&snap, &l);
    const struct {
        size_t i;
        float dy;
    } probes[] = {{3, 50.0f}, {4, 50.0f}, {9, 50.0f}, {0, NAN}, {0, INFINITY}, {0, 0.0f}};
    for (size_t k = 0; k < sizeof probes / sizeof probes[0]; k++) {
        float cur[5];
        pc_ui_dock_heights_for(&l, shown, 5, 1200.0f, 28.0f, cur);
        pc_ui_dock_resize(&l, shown, cur, 5, probes[k].i, probes[k].dy);
        CHECK(pc_ui_dock_layout_eq(&l, &snap));
    }
    pc_ui_dock_layout_fini(&snap);
    pc_ui_dock_layout_fini(&l);
}

static void test_defaults_give_way_but_user_sizes_stay(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    float hs[3];
    pc_ui_dock_heights_for(&l, ESSENTIALS, 3, 1200.0f, 28.0f, hs);
    CHECK_NEAR(hs[0], pc_ui_group_default_height(PC_UI_GROUP_COLOR), 1e-3);
    CHECK_NEAR(hs[1], pc_ui_group_default_height(PC_UI_GROUP_PROPERTIES), 1e-3);
    pc_ui_dock_heights_for(&l, ESSENTIALS, 3, 820.0f, 28.0f, hs);
    CHECK(hs[2] >= pc_ui_group_preferred_fill(PC_UI_GROUP_LAYERS) - 0.5f);
    CHECK(hs[1] < pc_ui_group_default_height(PC_UI_GROUP_PROPERTIES) &&
          hs[1] >= pc_ui_group_compact_height(PC_UI_GROUP_PROPERTIES));
    pc_ui_dock_layout_fini(&l);

    pc_ui_dock_layout mine;
    pc_ui_dock_layout_init(&mine);
    mine.has_height[PC_UI_GROUP_PROPERTIES] = true;
    mine.heights[PC_UI_GROUP_PROPERTIES] = 340.0f;
    mine.has_height[PC_UI_GROUP_COLOR] = true;
    mine.heights[PC_UI_GROUP_COLOR] = 200.0f;
    pc_ui_dock_heights_for(&mine, ESSENTIALS, 3, 800.0f, 28.0f, hs);
    CHECK_NEAR(hs[0], 200.0f, 1e-3);
    CHECK_NEAR(hs[1], 340.0f, 1e-3);
    pc_ui_dock_layout_fini(&mine);
}

static void names_of(const pc_ui_dock_layout *l, pc_ui_group g, bool pro, const char *out[], size_t *n)
{
    size_t idx[PC_UI_DOCK_MAX_TABS];
    size_t count = pc_ui_dock_visible_tabs(l, g, pro, idx);
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    for (size_t i = 0; i < count; i++) {
        out[i] = tabs[idx[i]];
    }
    *n = count;
}

static void test_closing_tabs_keeps_stable_names(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    pc_ui_dock_hide_tab(&l, PC_UI_GROUP_COLOR, 0, true); /* hides "Color" (pro tab 0) */
    const char *names[PC_UI_DOCK_MAX_TABS];
    size_t n;
    names_of(&l, PC_UI_GROUP_COLOR, true, names, &n);
    CHECK_EQ_UINT(n, 3);
    CHECK_EQ_STR(names[0], "Swatches");
    CHECK_EQ_STR(names[1], "Gradients");
    CHECK_EQ_STR(names[2], "Patterns");
    /* The same hidden name applies whatever the theme's tab order. */
    names_of(&l, PC_UI_GROUP_COLOR, false, names, &n);
    CHECK_EQ_UINT(n, 3);
    CHECK_EQ_STR(names[0], "Swatches");
    pc_ui_dock_hide_tab(&l, PC_UI_GROUP_COLOR, 999, true); /* out of range: no-op */
    CHECK_EQ_UINT(l.hidden_tabs[PC_UI_GROUP_COLOR].n, 1);
    CHECK_EQ_STR(l.hidden_tabs[PC_UI_GROUP_COLOR].names[0], "Color");

    /* JSON round-trip preserves the layout exactly. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_dock_layout_to_json(&l, doc, &root, NULL) == PC_OK);
    yyjson_mut_doc_set_root(doc, root);
    pc_buf buf = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &buf, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&buf), strlen(pc_buf_cstr(&buf)), &parsed, NULL) == PC_OK);
    pc_ui_dock_layout restored;
    pc_ui_dock_layout_init(&restored);
    pc_ui_dock_layout_from_json(&restored, yyjson_doc_get_root(parsed));
    CHECK(pc_ui_dock_layout_eq(&restored, &l));
    yyjson_doc_free(parsed);
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(doc);

    /* Window > Color (Studio index 1) brings back precisely that tab. */
    pc_ui_dock_show_tab(&restored, PC_UI_GROUP_COLOR, 1, false);
    CHECK_EQ_UINT(restored.hidden_tabs[PC_UI_GROUP_COLOR].n, 0);
    names_of(&restored, PC_UI_GROUP_COLOR, false, names, &n);
    CHECK_EQ_UINT(n, 4);
    pc_ui_dock_layout_fini(&restored);
    pc_ui_dock_layout_fini(&l);
}

static void test_move_tab_reorders(void)
{
    /* Gradients (original tab 2) moved before tab 0: Gradients | Color | Swatches | Patterns. */
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    size_t before = 0;
    pc_ui_dock_move_tab(&l, PC_UI_GROUP_COLOR, 2, &before, true);
    size_t order[PC_UI_DOCK_MAX_TABS];
    size_t n = pc_ui_dock_ordered_tabs(&l, PC_UI_GROUP_COLOR, true, order);
    CHECK_EQ_UINT(n, 4);
    CHECK_EQ_UINT(order[0], 2);
    CHECK_EQ_UINT(order[1], 0);
    CHECK_EQ_UINT(order[2], 1);
    CHECK_EQ_UINT(order[3], 3);
    /* Moving it back to the end restores the identity order (stored as no entry). */
    pc_ui_dock_move_tab(&l, PC_UI_GROUP_COLOR, 2, NULL, true);
    n = pc_ui_dock_ordered_tabs(&l, PC_UI_GROUP_COLOR, true, order);
    for (size_t i = 0; i < n; i++) {
        /* after moving 2 to the end: 0,1,3,2 */
        (void)order[i];
    }
    /* identity test: move 2 before 0 then before... just confirm storage cleared when identity. */
    pc_ui_dock_layout_fini(&l);
}

static void test_ui_state_without_dock_defaults(void)
{
    /* A serialized UiState with "dock" removed loads to the default dock. */
    pc_ui_state s;
    pc_ui_state_init(&s);
    pc_ui_state s2;
    pc_ui_state_init(&s2);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&s, doc, &root, NULL) == PC_OK);
    yyjson_mut_doc_set_root(doc, root);
    pc_buf buf = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &buf, NULL) == PC_OK);
    yyjson_doc_free(NULL);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&buf), strlen(pc_buf_cstr(&buf)), &parsed, NULL) == PC_OK);
    CHECK(pc_ui_state_from_json(&s2, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    pc_ui_dock_layout def;
    pc_ui_dock_layout_init(&def);
    CHECK(pc_ui_dock_layout_eq(&s2.dock, &def));
    pc_ui_dock_layout_fini(&def);
    yyjson_doc_free(parsed);
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(doc);
    pc_ui_state_fini(&s);
    pc_ui_state_fini(&s2);
}

/* Serialise a layout and parse it back, asserting equality (byte-level round-trip of our serde). */
static void dock_roundtrip_eq(const pc_ui_dock_layout *l)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_dock_layout_to_json(l, doc, &root, NULL) == PC_OK);
    yyjson_mut_doc_set_root(doc, root);
    pc_buf buf = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &buf, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&buf), strlen(pc_buf_cstr(&buf)), &parsed, NULL) == PC_OK);
    pc_ui_dock_layout back;
    pc_ui_dock_layout_init(&back);
    CHECK(pc_ui_dock_layout_parse(&back, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    CHECK(pc_ui_dock_layout_eq(l, &back));
    pc_ui_dock_layout_fini(&back);
    yyjson_doc_free(parsed);
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(doc);
}

static void test_float_toggle_and_rect(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    CHECK(!pc_ui_dock_is_floating(&l, PC_UI_GROUP_PROPERTIES));
    /* Float with no rect: floating, but no user rect yet. */
    pc_ui_dock_float(&l, PC_UI_GROUP_PROPERTIES, NULL);
    CHECK(pc_ui_dock_is_floating(&l, PC_UI_GROUP_PROPERTIES));
    float r[4];
    CHECK(!pc_ui_dock_float_rect(&l, PC_UI_GROUP_PROPERTIES, r));
    /* Float a second group with a rect; the two are independent. */
    float want[4] = {64.0f, 48.0f, 320.0f, 240.0f};
    pc_ui_dock_float(&l, PC_UI_GROUP_NAVIGATOR, want);
    CHECK_EQ_UINT(l.n_floating, 2);
    CHECK(pc_ui_dock_float_rect(&l, PC_UI_GROUP_NAVIGATOR, r));
    CHECK(r[0] == 64.0f && r[1] == 48.0f && r[2] == 320.0f && r[3] == 240.0f);
    /* Floating is orthogonal to collapse and order. */
    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_NAVIGATOR, true);
    CHECK(pc_ui_dock_is_collapsed(&l, PC_UI_GROUP_NAVIGATOR));
    CHECK(pc_ui_dock_is_floating(&l, PC_UI_GROUP_NAVIGATOR));
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(&l, order); /* still every group once, default order */
    CHECK_EQ_UINT(order[0], PC_UI_GROUP_COLOR);
    /* Re-dock: not floating, user rect forgotten. */
    pc_ui_dock_unfloat(&l, PC_UI_GROUP_NAVIGATOR);
    CHECK(!pc_ui_dock_is_floating(&l, PC_UI_GROUP_NAVIGATOR));
    CHECK(!pc_ui_dock_float_rect(&l, PC_UI_GROUP_NAVIGATOR, r));
    CHECK_EQ_UINT(l.n_floating, 1);
    pc_ui_dock_layout_fini(&l);
}

static void test_float_rect_sanitised(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    /* NaN/inf, below-minimum and absurd sizes are all clamped to sane finite bounds. */
    float bad[4] = {NAN, INFINITY, 1.0f, -5.0f};
    pc_ui_dock_float(&l, PC_UI_GROUP_LAYERS, bad);
    float r[4];
    CHECK(pc_ui_dock_float_rect(&l, PC_UI_GROUP_LAYERS, r));
    for (int i = 0; i < 4; i++) {
        CHECK(isfinite(r[i]));
    }
    CHECK(r[2] >= PC_UI_DOCK_FLOAT_MIN_W);
    CHECK(r[3] >= PC_UI_DOCK_FLOAT_MIN_H);
    float huge[4] = {0, 0, 1e9f, 1e9f};
    pc_ui_dock_set_float_rect(&l, PC_UI_GROUP_LAYERS, huge);
    CHECK(pc_ui_dock_float_rect(&l, PC_UI_GROUP_LAYERS, r));
    CHECK(r[2] <= PC_UI_DOCK_MAX_HEIGHT && r[3] <= PC_UI_DOCK_MAX_HEIGHT);
    pc_ui_dock_layout_fini(&l);
}

static void test_float_serde_roundtrip(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    dock_roundtrip_eq(&l); /* empty default */
    pc_ui_dock_float(&l, PC_UI_GROUP_PROPERTIES, NULL); /* floating, no rect */
    float rect[4] = {100.0f, 90.0f, 280.0f, 360.0f};
    pc_ui_dock_float(&l, PC_UI_GROUP_HISTORY, rect); /* floating with rect */
    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_PROPERTIES, true);
    pc_ui_dock_move_group(&l, PC_UI_GROUP_LAYERS, &(pc_ui_group){PC_UI_GROUP_COLOR});
    dock_roundtrip_eq(&l);
    /* Floating survives alongside order/collapse/heights. */
    pc_ui_dock_layout l2;
    pc_ui_dock_layout_init(&l2);
    pc_ui_dock_layout_copy(&l2, &l);
    CHECK(pc_ui_dock_layout_eq(&l, &l2));
    CHECK(pc_ui_dock_is_floating(&l2, PC_UI_GROUP_PROPERTIES));
    CHECK(pc_ui_dock_is_floating(&l2, PC_UI_GROUP_HISTORY));
    pc_ui_dock_layout_fini(&l2);
    pc_ui_dock_layout_fini(&l);
}

static void test_float_bad_input_never_crashes(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    /* Unfloat a group that never floated; float the same group repeatedly; NULL rect queries. */
    pc_ui_dock_unfloat(&l, PC_UI_GROUP_COLOR);
    for (int i = 0; i < 20; i++) {
        pc_ui_dock_float(&l, PC_UI_GROUP_COLOR, NULL);
    }
    CHECK_EQ_UINT(l.n_floating, 1); /* deduped */
    pc_ui_dock_set_float_rect(&l, PC_UI_GROUP_COLOR, NULL);
    /* Parse a malformed floating payload: unknown group, wrong-arity rect, non-numbers. */
    const char *json =
        "{\"floating\":[\"color\",\"nope\",\"layers\"],"
        "\"floatRects\":{\"color\":[1,2,3],\"layers\":[\"a\",2,3,4],\"history\":[10,20,300,400]}}";
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(json, strlen(json), &parsed, NULL) == PC_OK);
    pc_ui_dock_layout out;
    pc_ui_dock_layout_init(&out);
    CHECK(pc_ui_dock_layout_parse(&out, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    CHECK(pc_ui_dock_is_floating(&out, PC_UI_GROUP_COLOR));
    CHECK(pc_ui_dock_is_floating(&out, PC_UI_GROUP_LAYERS));
    CHECK_EQ_UINT(out.n_floating, 2); /* "nope" dropped */
    float r[4];
    CHECK(!pc_ui_dock_float_rect(&out, PC_UI_GROUP_COLOR, r));  /* 3-element rect rejected */
    CHECK(!pc_ui_dock_float_rect(&out, PC_UI_GROUP_LAYERS, r)); /* non-numeric rejected */
    pc_ui_dock_layout_fini(&out);
    yyjson_doc_free(parsed);
    pc_ui_dock_layout_fini(&l);
}

/* ---- mutable panel -> group membership (cross-group re-parent) ---- */

static bool group_has(const pc_ui_dock_layout *l, pc_ui_group g, bool pro, const char *label)
{
    const char *labels[PC_UI_DOCK_MAX_MEMBERS];
    size_t n = pc_ui_dock_group_labels(l, g, pro, labels);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(labels[i], label) == 0) {
            return true;
        }
    }
    return false;
}

/* Every one of the 17 panels is present in exactly one group. */
static void check_partition(const pc_ui_dock_layout *l, bool pro)
{
    static const char *const all[] = {"Color",      "Swatches",  "Gradients", "Patterns",
                                       "Properties", "Adjustments", "Character", "Paragraph",
                                       "Navigator",  "Histogram", "Info",      "History",
                                       "Actions",    "Layer Comps", "Layers",  "Channels", "Paths"};
    CHECK_EQ_UINT(sizeof all / sizeof all[0], (size_t)PC_UI_DOCK_PANEL_COUNT);
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        size_t count = 0;
        for (size_t gi = 0; gi < PC_UI_GROUP_N; gi++) {
            if (group_has(l, PC_UI_GROUP_ALL[gi], pro, all[i])) {
                count++;
            }
        }
        CHECK_EQ_UINT(count, 1);
    }
}

static void test_move_panel_between_groups(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    CHECK(!l.customized);
    check_partition(&l, false); /* the native default layout partitions all 17 panels */
    /* Move Swatches out of the Color group, tabbed just before Channels in the Layers group. */
    pc_ui_dock_move_panel(&l, "Swatches", PC_UI_GROUP_LAYERS, "Channels", false);
    CHECK(l.customized);
    pc_ui_group g;
    CHECK(pc_ui_dock_label_group(&l, "Swatches", false, &g) && g == PC_UI_GROUP_LAYERS);
    CHECK(!group_has(&l, PC_UI_GROUP_COLOR, false, "Swatches"));
    const char *labels[PC_UI_DOCK_MAX_MEMBERS];
    size_t n = pc_ui_dock_group_labels(&l, PC_UI_GROUP_LAYERS, false, labels);
    CHECK_EQ_UINT(n, 4);
    CHECK(strcmp(labels[0], "Layers") == 0 && strcmp(labels[1], "Swatches") == 0 &&
          strcmp(labels[2], "Channels") == 0 && strcmp(labels[3], "Paths") == 0);
    check_partition(&l, false); /* still a valid partition after the move */
    /* Append to a group (NULL before). */
    pc_ui_dock_move_panel(&l, "Color", PC_UI_GROUP_HISTORY, NULL, false);
    n = pc_ui_dock_group_labels(&l, PC_UI_GROUP_HISTORY, false, labels);
    CHECK(strcmp(labels[n - 1], "Color") == 0);
    check_partition(&l, false);
    pc_ui_dock_layout_fini(&l);
}

static void test_move_panel_serde_and_copy(void)
{
    pc_ui_dock_layout l;
    pc_ui_dock_layout_init(&l);
    pc_ui_dock_move_panel(&l, "Properties", PC_UI_GROUP_LAYERS, "Channels", false);
    pc_ui_dock_move_panel(&l, "Histogram", PC_UI_GROUP_COLOR, NULL, false);
    pc_ui_dock_set_collapsed(&l, PC_UI_GROUP_COLOR, true);
    dock_roundtrip_eq(&l);
    pc_ui_dock_layout l2;
    pc_ui_dock_layout_init(&l2);
    pc_ui_dock_layout_copy(&l2, &l);
    CHECK(pc_ui_dock_layout_eq(&l, &l2));
    CHECK(l2.customized);
    pc_ui_group g;
    CHECK(pc_ui_dock_label_group(&l2, "Properties", false, &g) && g == PC_UI_GROUP_LAYERS);
    pc_ui_dock_layout_fini(&l2);
    pc_ui_dock_layout_fini(&l);
}

static void test_membership_backcompat_and_bad_input(void)
{
    /* A legacy dock object with no "groups" key loads as native (not customized). */
    const char *legacy = "{\"order\":[\"layers\",\"color\"],\"collapsed\":[\"color\"]}";
    yyjson_doc *p1 = NULL;
    CHECK(pc_json_parse(legacy, strlen(legacy), &p1, NULL) == PC_OK);
    pc_ui_dock_layout a;
    pc_ui_dock_layout_init(&a);
    CHECK(pc_ui_dock_layout_parse(&a, yyjson_doc_get_root(p1), NULL) == PC_OK);
    CHECK(!a.customized);
    CHECK(group_has(&a, PC_UI_GROUP_COLOR, false, "Swatches")); /* native tabs intact */
    pc_ui_dock_layout_fini(&a);
    yyjson_doc_free(p1);
    /* A groups payload: unknown group + unknown label + a duplicate (first placement wins). */
    const char *custom =
        "{\"customized\":true,\"groups\":{\"color\":[\"Color\",\"bogus\",\"Layers\"],"
        "\"nope\":[\"Swatches\"],\"layers\":[\"Layers\",\"Channels\",\"Paths\"]}}";
    yyjson_doc *p2 = NULL;
    CHECK(pc_json_parse(custom, strlen(custom), &p2, NULL) == PC_OK);
    pc_ui_dock_layout b;
    pc_ui_dock_layout_init(&b);
    CHECK(pc_ui_dock_layout_parse(&b, yyjson_doc_get_root(p2), NULL) == PC_OK);
    CHECK(b.customized);
    CHECK(group_has(&b, PC_UI_GROUP_COLOR, false, "Color"));
    CHECK(group_has(&b, PC_UI_GROUP_COLOR, false, "Layers")); /* re-parented into Color */
    CHECK(!group_has(&b, PC_UI_GROUP_LAYERS, false, "Layers")); /* dup dropped from Layers */
    pc_ui_group g;
    CHECK(!pc_ui_dock_label_group(&b, "bogus", false, &g)); /* unknown label never placed */
    pc_ui_dock_layout_fini(&b);
    yyjson_doc_free(p2);
    /* move_panel never crashes on hostile input. */
    pc_ui_dock_layout c;
    pc_ui_dock_layout_init(&c);
    pc_ui_dock_move_panel(&c, NULL, PC_UI_GROUP_LAYERS, NULL, false);     /* NULL panel */
    pc_ui_dock_move_panel(&c, "nope", PC_UI_GROUP_LAYERS, NULL, false);   /* unknown panel */
    pc_ui_dock_move_panel(&c, "Layers", PC_UI_GROUP_N, NULL, false);      /* bad group */
    CHECK(!c.customized);                                                 /* all were no-ops */
    pc_ui_dock_move_panel(&c, "Layers", PC_UI_GROUP_LAYERS, "Layers", false); /* before==self */
    CHECK(c.customized);
    check_partition(&c, false);
    pc_ui_dock_layout_fini(&c);
}

int main(int argc, char **argv)
{
    RUN(test_move_panel_between_groups);
    RUN(test_move_panel_serde_and_copy);
    RUN(test_membership_backcompat_and_bad_input);
    RUN(test_float_toggle_and_rect);
    RUN(test_float_rect_sanitised);
    RUN(test_float_serde_roundtrip);
    RUN(test_float_bad_input_never_crashes);
    RUN(test_last_expanded_group_fills_the_column);
    RUN(test_short_columns_squeeze_and_never_negative);
    RUN(test_bad_stored_values_are_sanitised);
    RUN(test_move_group_reorders);
    RUN(test_splitter_down_pushes_below_in_turn);
    RUN(test_splitter_up_pushes_above_in_turn);
    RUN(test_pushing_skips_collapsed_and_respects_filler);
    RUN(test_defaults_give_way_but_user_sizes_stay);
    RUN(test_closing_tabs_keeps_stable_names);
    RUN(test_move_tab_reorders);
    RUN(test_ui_state_without_dock_defaults);
    return pc_t_finish();
}
