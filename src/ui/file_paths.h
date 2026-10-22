/* File-path helpers for the file dialogs (U10): basename, natural ordering, extension lowercasing,
 * the open dialog's initial folder, and TIFF detection. Pure and dependency-free (clean-room ports
 * of ui-egui's file_open.rs / file_dialog.rs / tiff_options_ui.rs helpers), so they are unit-tested
 * in isolation. Every entry is NULL-safe and bounds its output. */
#ifndef PC_UI_FILE_PATHS_H
#define PC_UI_FILE_PATHS_H
#include <stdbool.h>
#include <stddef.h>

/* Order two names the way a file browser does: runs of ASCII digits compare by value (leading
 * zeros ignored, then by length, then text), everything else case-insensitively; names equal that
 * way fall back to the exact bytes so the order is total. Returns <0, 0 or >0. NULL sorts as "". */
int pc_ui_natural_cmp(const char *a, const char *b);

/* The file name of `path` (its display name): the segment after the last '/', or `path` itself
 * when there is none or it would be empty. Returns a pointer into `path` (never NULL for a
 * non-NULL argument); "" for NULL. */
const char *pc_ui_display_name(const char *path);

/* Copy `path` into `out` (capacity `cap`, always NUL-terminated when cap>0) with only its file
 * extension lowercased, and only when the extension has an uppercase letter. The stem and the
 * folders keep their spelling; a name with no extension, or a leading-dot name like ".bashrc", is
 * copied unchanged. Returns true when anything was lowercased. On a too-small buffer nothing is
 * written and false is returned. */
bool pc_ui_lowercase_extension(const char *path, char *out, size_t cap);

/* The folder an Open dialog should start in: the parent directory of the most-recent file
 * `first`. Writes it to `out` (NUL-terminated) and returns true, or returns false (leaving `out`
 * untouched) when there is no usable parent — a bare file name ("cat.psd"), the root ("/"), or a
 * NULL/empty/over-long input. */
bool pc_ui_last_used_dir(const char *first, char *out, size_t cap);

/* Whether `path` names a TIFF file: its base name (after the last '/' or '\\') ends in ".tif" or
 * ".tiff" (case-insensitive) with a non-empty stem. */
bool pc_ui_is_tiff_path(const char *path);

#endif
