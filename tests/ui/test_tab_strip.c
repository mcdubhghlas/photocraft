/* Tab-fit policy (port of the logic tests from crates/ui-egui/src/tab_strip.rs; the egui paint
 * tests are rebuilt as shell snapshots). The fit policy is a reimplementation of the external
 * craft_ui::tabs::fit from its documented behaviour, so widths match the contract, not byte-exact
 * craft_ui (§7). */
#include "harness.h"

#include <math.h>

#include "ui/tab_strip.h"

static float total(const pc_ui_tab_fit *f)
{
    float s = 0.0f;
    for (size_t i = 0; i < f->n_shown; i++) {
        s += f->shown[i].width;
    }
    return s;
}

static bool shows(const pc_ui_tab_fit *f, size_t idx)
{
    for (size_t i = 0; i < f->n_shown; i++) {
        if (f->shown[i].index == idx) {
            return true;
        }
    }
    return false;
}

static bool overflows(const pc_ui_tab_fit *f, size_t idx)
{
    for (size_t i = 0; i < f->n_overflow; i++) {
        if (f->overflow[i] == idx) {
            return true;
        }
    }
    return false;
}

static void test_tabs_that_fit_keep_widths(void)
{
    float nat[] = {60.0f, 70.0f, 80.0f};
    pc_ui_tab_fit f = pc_ui_tab_fit_compute(nat, 3, 0, 400.0f, 40.0f, 18.0f);
    CHECK_EQ_UINT(f.n_shown, 3);
    CHECK_EQ_UINT(f.n_overflow, 0);
    CHECK_NEAR(f.shown[0].width, 60.0f, 1e-4);
    CHECK_NEAR(f.shown[1].width, 70.0f, 1e-4);
    CHECK_NEAR(f.shown[2].width, 80.0f, 1e-4);
}

static void test_narrow_shrinks_widest_first(void)
{
    float nat[] = {50.0f, 70.0f, 120.0f};
    pc_ui_tab_fit f = pc_ui_tab_fit_compute(nat, 3, 0, 200.0f, 40.0f, 18.0f);
    CHECK_EQ_UINT(f.n_overflow, 0);
    CHECK_NEAR(total(&f), 200.0f, 0.1);
    CHECK_NEAR(f.shown[0].width, 50.0f, 1e-3); /* below the cap: keeps its width */
    CHECK_NEAR(f.shown[1].width, 70.0f, 1e-3);
    CHECK_NEAR(f.shown[2].width, 80.0f, 0.1); /* only the widest shrank */
}

static void test_overflow_keeps_selected_and_chevron_room(void)
{
    float nat[] = {80.0f, 90.0f, 85.0f, 95.0f};
    for (size_t sel = 0; sel < 4; sel++) {
        pc_ui_tab_fit f = pc_ui_tab_fit_compute(nat, 4, sel, 120.0f, 40.0f, 18.0f);
        CHECK(shows(&f, sel));
        CHECK(f.n_overflow > 0 && !overflows(&f, sel));
        CHECK(total(&f) <= 120.0f - 18.0f + 1e-3);
        CHECK_EQ_UINT(f.n_shown + f.n_overflow, 4);
        for (size_t i = 1; i < f.n_shown; i++) {
            CHECK(f.shown[i - 1].index < f.shown[i].index); /* tab order kept */
        }
    }
}

static void test_hostile_inputs_never_negative(void)
{
    const float avails[] = {0.0f, -10.0f, 5.0f, NAN, INFINITY};
    float a[] = {0}, b[] = {NAN, 50.0f}, c[] = {1e9f, -5.0f, 30.0f};
    const struct {
        const float *nat;
        size_t n;
    } sets[] = {{a, 0}, {b, 2}, {c, 3}};
    const size_t sels[] = {0, 1, 99};
    for (size_t ai = 0; ai < sizeof avails / sizeof avails[0]; ai++) {
        for (size_t si = 0; si < 3; si++) {
            for (size_t k = 0; k < 3; k++) {
                pc_ui_tab_fit f =
                    pc_ui_tab_fit_compute(sets[si].nat, sets[si].n, sels[k], avails[ai], 40.0f, 18.0f);
                for (size_t i = 0; i < f.n_shown; i++) {
                    CHECK(isfinite(f.shown[i].width) && f.shown[i].width >= 0.0f);
                }
                CHECK_EQ_UINT(f.n_shown + f.n_overflow, sets[si].n);
            }
        }
    }
}

static void test_drop_slot(void)
{
    float cx[] = {20.0f, 60.0f, 100.0f};
    size_t slot;
    CHECK(pc_ui_tab_drop_slot(cx, 3, 0.0f, 0.0f, 120.0f, 26.0f, 70.0f, 13.0f, &slot) && slot == 2);
    CHECK(pc_ui_tab_drop_slot(cx, 3, 0.0f, 0.0f, 120.0f, 26.0f, 10.0f, 13.0f, &slot) && slot == 0);
    /* Past the last tab but still within the strip (+24 px): after the last. */
    CHECK(pc_ui_tab_drop_slot(cx, 3, 0.0f, 0.0f, 120.0f, 26.0f, 130.0f, 13.0f, &slot) && slot == 3);
    /* Far outside the strip: no slot. */
    CHECK(!pc_ui_tab_drop_slot(cx, 3, 0.0f, 0.0f, 120.0f, 26.0f, 400.0f, 13.0f, &slot));
    CHECK(!pc_ui_tab_drop_slot(cx, 3, 0.0f, 0.0f, 120.0f, 26.0f, 60.0f, 200.0f, &slot));
}

int main(int argc, char **argv)
{
    RUN(test_tabs_that_fit_keep_widths);
    RUN(test_narrow_shrinks_widest_first);
    RUN(test_overflow_keeps_selected_and_chevron_room);
    RUN(test_hostile_inputs_never_negative);
    RUN(test_drop_slot);
    return pc_t_finish();
}
