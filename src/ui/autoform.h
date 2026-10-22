/* Document-independent "automate"/"scripts" form dialogs (U10 B5).
 *
 * These menu items (Contact Sheet II, Image Statistics, Script Events Manager, Package, Export
 * Paths to File, Scripts > Browse) front an engine command behind the generic command-form
 * mechanism. Unlike filters, most do not operate on the active document, so they cannot reuse the
 * filter-dialog schema path (which binds to a document + revision). This module supplies, as pure
 * data, the parameter-schema string and the "Browse…" button behaviour for each; the shell glue
 * (command_form.c) turns that into a __form + __autoform dialog.
 *
 * Pure (no GUI, no engine state): unit-testable. Port of crates/ui-egui/src/file_ui.rs form(). */
#ifndef PC_UI_AUTOFORM_H
#define PC_UI_AUTOFORM_H

#include <stdbool.h>
#include <stddef.h>

/* What a command's "Browse…" button opens, and which field it fills. */
typedef enum {
    PC_UI_AUTOFORM_BROWSE_NONE = 0,
    PC_UI_AUTOFORM_BROWSE_OPEN_FOLDER, /* an input/output folder */
    PC_UI_AUTOFORM_BROWSE_SAVE_FILE,   /* a file to create (e.g. the .ai output) */
    PC_UI_AUTOFORM_BROWSE_OPEN_FILE    /* an existing file (e.g. a script) */
} pc_ui_autoform_browse;

/* True when `id` is one of the document-independent automate/scripts form commands. */
bool pc_ui_autoform_command(const char *id);

/* The dialog title for `id` (English; the UI translates it), or NULL when not an autoform. */
const char *pc_ui_autoform_label(const char *id);

/* The static parameter-schema string for `id`, or NULL when it is built dynamically
 * (file.export.pathsToIllustrator) or `id` is not an autoform command. */
const char *pc_ui_autoform_spec(const char *id);

/* The "Browse…" target: the field name it writes (returned), and *kind (never NULL) set to how to
 * pick it. Returns NULL (and *kind = NONE) when the command has no browse button. */
const char *pc_ui_autoform_browse_for(const char *id, pc_ui_autoform_browse *kind);

/* Build the file.export.pathsToIllustrator schema with a "paths" choice drawn from the document's
 * named paths. `names` are the document's path names (top to bottom); `has_work` adds "work".
 * Choices are "all", then "work" (if any), then up to 62 sanitised names (any '|', quote, or
 * control byte becomes '_'; over-long names truncated). Writes a NUL-terminated spec into
 * out[0..cap) and returns true, or false when out/cap are unusable. */
bool pc_ui_autoform_paths_spec(const char *const *names, size_t count, bool has_work, char *out,
                               size_t cap);

#endif
