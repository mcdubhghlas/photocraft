/* Unit tests for the Print dialog's portable helpers (src/ui/print_layout.c): unit conversion,
 * paper presets, printer-name prettifying and the preview page layout (a port of the maths in
 * print_cmds::layout). */
#include <math.h>
#include <string.h>

#include "harness.h"
#include "ui/print_layout.h"

static void test_unit_pt(void)
{
    CHECK(fabs(pc_ui_print_unit_pt("in") - 72.0) < 1e-9);
    CHECK(fabs(pc_ui_print_unit_pt("mm") - 72.0 / 25.4) < 1e-9);
    CHECK(fabs(pc_ui_print_unit_pt("cm") - 72.0 / 2.54) < 1e-9);
    CHECK(fabs(pc_ui_print_unit_pt("furlong") - 72.0 / 2.54) < 1e-9); /* unknown -> cm */
    CHECK(fabs(pc_ui_print_unit_pt(NULL) - 72.0 / 2.54) < 1e-9);
}

static void test_paper_size(void)
{
    double w = 0, h = 0;
    CHECK(pc_ui_print_paper_size("letter", &w, &h));
    CHECK(w == 612.0 && h == 792.0);
    CHECK(pc_ui_print_paper_size("A4", &w, &h)); /* case-insensitive */
    CHECK(fabs(w - 595.28) < 1e-6 && fabs(h - 841.89) < 1e-6);
    CHECK(!pc_ui_print_paper_size("bogus", &w, &h)); /* falls back to Letter */
    CHECK(w == 612.0 && h == 792.0);
    CHECK(PC_UI_PRINT_NPAPERS == 8);
}

static void test_printer_label(void)
{
    char out[64];
    pc_ui_print_printer_label("HP_Laser_Jet", out, sizeof out);
    CHECK(!strcmp(out, "HP Laser Jet"));
    pc_ui_print_printer_label(NULL, out, sizeof out);
    CHECK(!strcmp(out, ""));
    char small[4];
    pc_ui_print_printer_label("abcdef", small, sizeof small);
    CHECK(!strcmp(small, "abc")); /* truncates, always NUL-terminated */
}

static void test_layout_center_scale_orientation(void)
{
    pc_ui_print_layout l;
    /* Letter portrait, 300x150 px @ 72 dpi, 100% scale, centred, no marks. */
    CHECK(pc_ui_print_compute_layout(612, 792, false, 300, 150, 72, false, false, 100, true, 0, 0, &l));
    CHECK(l.pw == 612.0 && l.ph == 792.0);
    CHECK(fabs(l.w - 300.0) < 1e-9 && fabs(l.h - 150.0) < 1e-9);
    CHECK(fabs(l.x - 156.0) < 1e-9 && fabs(l.y - 321.0) < 1e-9);
    CHECK(fabs(l.scale - 1.0) < 1e-9);

    /* Landscape swaps the page. */
    CHECK(pc_ui_print_compute_layout(612, 792, true, 300, 150, 72, false, false, 100, true, 0, 0, &l));
    CHECK(l.pw == 792.0 && l.ph == 612.0);

    /* 50% scale halves the image. */
    CHECK(pc_ui_print_compute_layout(612, 792, false, 300, 150, 72, false, false, 50, true, 0, 0, &l));
    CHECK(fabs(l.w - 150.0) < 1e-9 && fabs(l.scale - 0.5) < 1e-9);
}

static void test_layout_fit_and_position(void)
{
    pc_ui_print_layout l;
    /* Scale to fit uses the smaller axis with an 18pt margin (no marks). */
    CHECK(pc_ui_print_compute_layout(612, 792, false, 300, 150, 72, false, true, 100, true, 0, 0, &l));
    CHECK(fabs(l.scale - (612.0 - 36.0) / 300.0) < 1e-9);

    /* Non-centred placement uses top/left inches from the top-left corner. */
    CHECK(pc_ui_print_compute_layout(612, 792, false, 300, 150, 72, false, false, 100, false, 1, 2, &l));
    CHECK(fabs(l.x - 144.0) < 1e-9);          /* left 2in = 144pt */
    CHECK(fabs(l.y - (792.0 - 72.0 - 150.0)) < 1e-9); /* ph - top(1in) - h */
}

static void test_layout_hostile_inputs(void)
{
    pc_ui_print_layout l;
    /* A zero document reports invalid but does not crash. */
    CHECK(!pc_ui_print_compute_layout(612, 792, false, 0, 0, 72, false, false, 100, true, 0, 0, &l));
    /* A non-finite scale is treated as 100%, still a usable page. */
    CHECK(pc_ui_print_compute_layout(612, 792, false, 100, 100, 72, false, false, NAN, true, 0, 0, &l));
    /* Zero paper falls back to Letter. */
    CHECK(pc_ui_print_compute_layout(0, 0, false, 100, 100, 72, false, false, 100, true, 0, 0, &l));
    /* A NULL output is rejected, not dereferenced. */
    CHECK(!pc_ui_print_compute_layout(612, 792, false, 100, 100, 72, false, false, 100, true, 0, 0, NULL));
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_unit_pt);
    RUN(test_paper_size);
    RUN(test_printer_label);
    RUN(test_layout_center_scale_orientation);
    RUN(test_layout_fit_and_position);
    RUN(test_layout_hostile_inputs);
    return pc_t_finish();
}
