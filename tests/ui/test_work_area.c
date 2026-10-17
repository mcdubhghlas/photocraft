/* Visible work area geometry (port of crates/ui-egui/src/work_area.rs tests). */
#include "harness.h"

#include "ui/work_area.h"

#define MON_W 1366.0f
#define MON_H 768.0f
#define TB PC_UI_TASKBAR

static pc_ui_rect R(float x, float y, float w, float h)
{
    pc_ui_rect r = {x, y, x + w, y + h};
    return r;
}

/* Like the Rust test's window(): content at the origin, inner at (x,y), both w x h. */
static pc_ui_rect content_of(float w, float h) { return R(0, 0, w, h); }
static pc_ui_rect inner_of(float x, float y, float w, float h) { return R(x, y, w, h); }

static bool eq(pc_ui_rect a, pc_ui_rect b)
{
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

static void test_taller_than_display_cut_at_taskbar(void)
{
    pc_ui_rect content = content_of(1440, 900);
    pc_ui_rect inner = inner_of(0, 0, 1440, 900);
    pc_ui_rect v = pc_ui_visible_part(content, true, MON_W, MON_H, true, inner, PC_UI_WIN_NORMAL);
    pc_ui_rect want = {0, 0, 1366, 768 - TB};
    CHECK(eq(v, want));

    content = content_of(1366, 737);
    inner = inner_of(0, 31, 1366, 737);
    v = pc_ui_visible_part(content, true, MON_W, MON_H, true, inner, PC_UI_WIN_NORMAL);
    CHECK_NEAR(v.y1, 768 - TB - 31, 1e-3);

    content = content_of(800, 500);
    inner = inner_of(100, 80, 800, 500);
    v = pc_ui_visible_part(content, true, MON_W, MON_H, true, inner, PC_UI_WIN_NORMAL);
    CHECK(eq(v, content));

    content = content_of(1440, 900);
    inner = inner_of(1366 + 100, 0, 1440, 900);
    v = pc_ui_visible_part(content, true, MON_W, MON_H, true, inner, PC_UI_WIN_NORMAL);
    CHECK_NEAR(v.y1, 768 - TB, 1e-3);
}

static void test_maximized_unknown_or_nonsense_keeps_window(void)
{
    pc_ui_rect content = content_of(1440, 900);
    pc_ui_rect inner = inner_of(0, 0, 1440, 900);
    CHECK(eq(pc_ui_visible_part(content, true, MON_W, MON_H, true, inner, PC_UI_WIN_FULLSCREEN), content));
    CHECK(eq(pc_ui_visible_part(content, false, MON_W, MON_H, true, inner, PC_UI_WIN_NORMAL), content));
    CHECK(eq(pc_ui_visible_part(content, true, 0.0f / 0.0f, MON_H, true, inner, PC_UI_WIN_NORMAL), content));
    CHECK(eq(pc_ui_visible_part(content, true, 0.0f, 0.0f, true, inner, PC_UI_WIN_NORMAL), content));
    pc_ui_rect bad = R(1.0f / 0.0f, 0, 10, 10);
    CHECK(eq(pc_ui_visible_part(content, true, MON_W, MON_H, true, bad, PC_UI_WIN_NORMAL), content));
    content = content_of(1000, 300);
    inner = inner_of(0, 600, 1000, 300);
    CHECK(eq(pc_ui_visible_part(content, true, MON_W, MON_H, true, inner, PC_UI_WIN_NORMAL), content));
}

static void test_maximized_under_a_panel_still_cut(void)
{
    const float scales[] = {1.0f, 1.5f};
    for (size_t k = 0; k < 2; k++) {
        float mw = 1920.0f / scales[k], mh = 1080.0f / scales[k];
        pc_ui_rect content = R(0, 0, mw, mh);
        float bottom = mh - TB;
        pc_ui_rect v = pc_ui_visible_part(content, true, mw, mh, true, content, PC_UI_WIN_MAXIMIZED);
        CHECK_NEAR(v.y1, bottom, 1e-2);
        pc_ui_rect fitted = R(0, 0, mw, bottom);
        v = pc_ui_visible_part(fitted, true, mw, mh, true, fitted, PC_UI_WIN_MAXIMIZED);
        CHECK(eq(v, fitted));
        CHECK(eq(pc_ui_visible_part(content, true, mw, mh, true, content, PC_UI_WIN_FULLSCREEN), content));
    }
}

static void test_unknown_position_clears_taskbar(void)
{
    pc_ui_rect content = content_of(1366, 740);
    pc_ui_rect v = pc_ui_visible_part(content, true, MON_W, MON_H, false, content, PC_UI_WIN_NORMAL);
    pc_ui_rect want = {0, 0, 1366, 768 - TB};
    CHECK(eq(v, want));
    pc_ui_rect big = content_of(1440, 900);
    v = pc_ui_visible_part(big, true, MON_W, MON_H, false, big, PC_UI_WIN_NORMAL);
    CHECK(eq(v, want));
    pc_ui_rect small = content_of(1000, 600);
    CHECK(eq(pc_ui_visible_part(small, true, MON_W, MON_H, false, small, PC_UI_WIN_NORMAL), small));
    CHECK(eq(pc_ui_visible_part(content, true, MON_W, MON_H, false, content, PC_UI_WIN_MAXIMIZED), content));
    CHECK(eq(pc_ui_visible_part(content, true, MON_W, MON_H, false, content, PC_UI_WIN_FULLSCREEN), content));
}

static void test_fits(void)
{
    CHECK(pc_ui_window_fits(1366, 720, MON_W, MON_H));
    CHECK(!pc_ui_window_fits(1366, 740, MON_W, MON_H));
    CHECK(!pc_ui_window_fits(1440, 700, MON_W, MON_H));
}

int main(int argc, char **argv)
{
    RUN(test_taller_than_display_cut_at_taskbar);
    RUN(test_maximized_unknown_or_nonsense_keeps_window);
    RUN(test_maximized_under_a_panel_still_cut);
    RUN(test_unknown_position_clears_taskbar);
    RUN(test_fits);
    return pc_t_finish();
}
