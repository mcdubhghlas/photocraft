/* Krita (.kra) import (port of crates/io/src/kra.rs, R10): the archive's mergedimage.png opens as
 * one editable raster layer, read-only source (Krita's layer tree is not imported, which a
 * warning says). Errors are "Krita: <reason>" (PC_IO_ERR_KRA). Thread-safe. */
#ifndef PC_IO_KRA_H
#define PC_IO_KRA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/core.h"
#include "raster/interrupt.h"

/* A ZIP whose first stored entry "mimetype" is application/x-krita (ASCII white space trimmed). */
bool pc_io_is_kra(const uint8_t *bytes, size_t len);
/* Imports a Krita document's merged composite. ctl may be NULL. *out initialised on success. */
pc_status pc_io_kra_import(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                           pc_io_import_result *out, pc_error *err);

#endif
