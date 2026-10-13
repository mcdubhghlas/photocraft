/* Internal to src/format: the hardened file operations of directory bundle saves (store.rs
 * bundle_root, validate_bundle_file, write_bundle_file, remove_bundle_file, plain_*_or_missing).
 *
 * A caller may choose a symlinked bundle root, but every managed entry below its resolved target
 * (the tiles/, blobs/ and composite/ folders and the files in them, manifest.json, thumb.png)
 * must be a real directory or regular file: a symlink planted there is refused before anything is
 * written, so a save never writes or deletes outside the bundle. Refusals are FormatError::Io
 * "I/O: refusing directory bundle save: <path> is a symlink or not a regular file" (or "... not a
 * directory"). Not synchronised. */
#ifndef PC_FORMAT_BUNDLE_DIR_H
#define PC_FORMAT_BUNDLE_DIR_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"

/* Creates dir (and missing parents) and its tiles/, blobs/ and composite/ folders, and returns
 * its canonical path in *root (caller frees). */
pc_status pc_bundle_root(const char *dir, char **root, pc_error *err);
/* The refusal for path (a symlink or not a regular file). */
pc_status pc_bundle_unsafe_entry(const char *path, pc_error *err);
/* OK when path is a regular file (not a symlink) or missing. */
pc_status pc_bundle_plain_file_or_missing(const char *path, pc_error *err);
/* OK when path is a real directory (not a symlink) or missing. */
pc_status pc_bundle_plain_directory_or_missing(const char *path, pc_error *err);
/* path's parent must be root or one of its managed folders (unchanged since bundle_root) and
 * path a regular file or missing. */
pc_status pc_bundle_validate_file(const char *root, const char *path, pc_error *err);
/* Atomically replaces path (below root) with data: a fresh temporary beside it, checked again
 * around every step; rename replaces a symlink itself, never its referent. */
pc_status pc_bundle_write_file(const char *root, const char *path, const void *data, size_t len,
                               pc_error *err);
/* Validates, then unlinks path. *missing (if non-NULL) says the file was already gone. */
pc_status pc_bundle_remove_file(const char *root, const char *path, bool *missing, pc_error *err);
/* A directory's entries under it: *is_link / *is_file from lstat; *exists false when it vanished. */
pc_status pc_bundle_lstat(const char *path, bool *exists, bool *is_link, bool *is_file, pc_error *err);

#endif
