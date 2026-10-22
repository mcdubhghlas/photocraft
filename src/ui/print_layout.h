/* Print dialog — portable helpers (clean-room port of the layout maths in
 * crates/ui-egui/src/file_ui.rs and crates/engine/src/print_cmds.rs: paper sizes, unit conversion,
 * printer-name prettifying and the page layout used for the live preview). The real print is done
 * by the engine's file.print command; this is only the dialog's own preview geometry, kept free of
 * SDL/Nuklear so it is unit-tested headless. Thin; NULL-safe; never crashes (REWRITE_PLAN.md
 * rule 9). */
#ifndef PC_UI_PRINT_LAYOUT_H
#define PC_UI_PRINT_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Named paper sizes in points (portrait), in menu order. */
#define PC_UI_PRINT_NPAPERS 8
extern const char *const PC_UI_PRINT_PAPER_NAMES[PC_UI_PRINT_NPAPERS];
extern const double PC_UI_PRINT_PAPER_W[PC_UI_PRINT_NPAPERS];
extern const double PC_UI_PRINT_PAPER_H[PC_UI_PRINT_NPAPERS];

/* Points per unit for the Position and Size fields: "mm", "in" (inches), otherwise centimetres. */
double pc_ui_print_unit_pt(const char *units);

/* Portrait paper dimensions in points for a preset `name` (case-insensitive). Returns false and
 * leaves the outputs at Letter (612 x 792) when the name is not a known preset. */
bool pc_ui_print_paper_size(const char *name, double *w, double *h);

/* Prettify a CUPS queue name for display: underscores become spaces. Always NUL-terminates when
 * cap > 0. */
void pc_ui_print_printer_label(const char *name, char *out, size_t cap);

/* The page layout for the preview. */
typedef struct pc_ui_print_layout {
    double pw, ph;   /* paper size in points, after orientation */
    double x, y;     /* image origin in points, from the bottom-left */
    double w, h;     /* image size in points */
    double scale;    /* applied scale factor (1.0 == 100%) */
} pc_ui_print_layout;

/* Compute the page layout, mirroring print_cmds::layout. `paper_w`/`paper_h` are the portrait paper
 * size in points; `landscape` swaps them. `doc_w`/`doc_h` are the document size in pixels at `dpi`.
 * `marks` is true when any printing mark is on (a larger margin). With `scale_to_fit` the image is
 * fitted inside the margins; otherwise `scale_pct` (percent) is used. When `center` is false the
 * image is placed at `top_in`/`left_in` inches from the top-left. Returns false (and still fills a
 * best-effort layout) when the inputs are not a usable page. */
bool pc_ui_print_compute_layout(double paper_w, double paper_h, bool landscape, uint32_t doc_w,
                                uint32_t doc_h, double dpi, bool marks, bool scale_to_fit,
                                double scale_pct, bool center, double top_in, double left_in,
                                pc_ui_print_layout *out);

#endif /* PC_UI_PRINT_LAYOUT_H */
