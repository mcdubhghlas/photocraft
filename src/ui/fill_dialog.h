/* Pure helpers for the Edit > Fill dialog (port of the field model in crates/ui-egui/src/fill_ui.rs):
 * the default field values and the ordered Contents list. No GUI dependency, so it is unit-tested
 * headlessly. The dialog chrome/body lives in ui/shell/fill_ui.c. */
#ifndef PC_UI_FILL_DIALOG_H
#define PC_UI_FILL_DIALOG_H

#include <stddef.h>

#include <yyjson.h>

/* The Contents values (param keys) in the dialog's display order, and their labels. */
extern const char *const PC_UI_FILL_CONTENTS[9];
extern const char *const PC_UI_FILL_CONTENT_LABELS[9];

/* Build the default Fill dialog field object (contents, color, colorAdaptation, mode, opacity,
 * preserveTransparency) into `doc` and return it (NULL on OOM). `foreground` (rgba, may be NULL)
 * seeds the default Color. */
yyjson_mut_val *pc_ui_fill_default_fields(yyjson_mut_doc *doc, const float foreground[4]);

#endif /* PC_UI_FILL_DIALOG_H */
