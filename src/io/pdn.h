/* Paint.NET (.pdn, "PDN3") import (port of crates/io/src/pdn.rs and pdn/nrbf.rs, R10): the .NET
 * binary-serialised document graph (NRBF), its bitmap layers (gzip-compressed pixel chunks),
 * blend modes, opacity and visibility, as an editable, read-only-source document. Untrusted
 * input: graph items, strings, depth, layers and pixel bytes are capped; errors are
 * "PDN: <reason>" (PC_IO_ERR_PDN). Thread-safe. */
#ifndef PC_IO_PDN_H
#define PC_IO_PDN_H

#include <stddef.h>
#include <stdint.h>

#include "io/core.h"
#include "raster/interrupt.h"

/* Imports a Paint.NET document. ctl may be NULL. *out initialised on success. */
pc_status pc_io_pdn_import(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                           pc_io_import_result *out, pc_error *err);

#endif
