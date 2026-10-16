/* Nuklear styling driven by the theme tokens (partial port of crates/ui-egui/src/widgets.rs).
 *
 * U1 establishes the token -> Nuklear-style mapping so every theme applies to the immediate-mode
 * widgets (the egui crate's widgets.rs builds its visuals from theme::Tokens the same way). The
 * individual widget wrappers (value_field, dropdown, checkbox, primary_button, hairline, ...) are
 * reimplemented against Nuklear by the later UI chunks that use them.
 *
 * This file uses only Nuklear (no SDL/GL), so it links into the library; a GL context is not
 * needed to apply a style, which keeps it unit-testable headlessly. */
#ifndef PC_UI_WIDGETS_H
#define PC_UI_WIDGETS_H

#include "photocraft_nuklear.h"
#include "ui/theme.h"

/* A theme token colour as a Nuklear colour. */
struct nk_color pc_ui_nk_color(pc_ui_color c);

/* Apply the theme's colour table and corner radii to `ctx`'s style (nk_style_from_table plus
 * rounding). Safe before any frame; no font or GL state is touched. */
void pc_ui_apply_nk_style(struct nk_context *ctx, const pc_ui_tokens *t);

#endif
