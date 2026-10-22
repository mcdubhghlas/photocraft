/* Nuklear drawing + input routing for the Camera Raw scopes and preview interactions. The pure
 * geometry/decision logic lives in src/ui/camera_raw_scope.c (headless-tested); this file maps it
 * onto Nuklear draw commands and reads the engine preview/compose/vectorscope primitives. Only
 * camera_raw_ui.c calls these. */
#ifndef PC_UI_SHELL_CAMERA_RAW_SCOPE_DRAW_H
#define PC_UI_SHELL_CAMERA_RAW_SCOPE_DRAW_H

#include "dialogs.h"
#include "photocraft_nuklear.h"

/* True when the dialog is currently showing the "before" (unprocessed) image. */
bool pc_ui_shell_cr_show_before(pc_ui_dialog *d);

/* The zoom/pan/before-after preview with colour-sampler probes and the clipping overlay, drawn and
 * interacted inside `box`. `layout` is the host widget state (valid/rom) from nk_widget. */
void pc_ui_shell_cr_preview(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d,
                            struct nk_rect box, enum nk_widget_layout_states layout);

/* The tone histogram ribbon with shadow/highlight clipping-warning triangles, tone-zone drag and
 * the hover RGB badge, inside `graph`. */
void pc_ui_shell_cr_histogram(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d,
                              struct nk_rect graph);

/* The vectorscope plot (hue/saturation cloud, spokes, skin-tone line, sample target) inside `box`. */
void pc_ui_shell_cr_vectorscope(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d,
                                struct nk_rect box);

/* The preview toolbar (zoom tool/hand/fit/100%, sampler, before-after) laid out on the current
 * Nuklear row. */
void pc_ui_shell_cr_toolbar(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d);

/* Scope display options (Lab readouts, vectorscope and its sub-options, floating panel). */
void pc_ui_shell_cr_scope_options(pc_ui_shell_app *app, pc_ui_dialog *d);

/* Release the bounded scope caches (proxy pixels, vectorscope, colour transforms). Safe to call
 * when no dialog is open. */
void pc_ui_shell_cr_scope_fini(void);

#endif
