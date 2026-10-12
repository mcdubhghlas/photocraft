/* Affinity documents (.af, .afdesign, .afphoto, .afpub). Detection is I1's (lib.c, because lib.rs
 * dispatches on it); the import is chunk I5's (crates/io/src/affinity.rs; stub in stub_i5.c,
 * PC_ERR_UNSUPPORTED "unsupported: Affinity import is not ported yet (I5)"). I5 calls back into
 * I1 (pc_io_image_to_document for the preview) and I4 (pc_io_text_prepare). Thread-safe. */
#ifndef PC_IO_AFFINITY_H
#define PC_IO_AFFINITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/core.h"

/* Extensions recognised as Affinity documents ("af", "afphoto", "afdesign", "afpub"). */
#define PC_IO_AFFINITY_EXTENSION_COUNT 4
extern const char *const PC_IO_AFFINITY_EXTENSIONS[PC_IO_AFFINITY_EXTENSION_COUNT];

/* The Affinity container magic (I1). */
bool pc_io_is_affinity(const uint8_t *bytes, size_t len);
/* The file name's extension (after the last '/' or '\\', then the last '.') is an Affinity one,
 * ASCII case-insensitively (I1). */
bool pc_io_affinity_has_extension(const char *name);

/* Opens an Affinity document natively (source_read_only set), or its embedded preview
 * (preview_only) when the native data can't be read (I5). *out initialised on success. */
pc_status pc_io_affinity_import(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                                pc_error *err);

#endif
