/* Character and paragraph styles <-> type layers' EngineData (chunk I4:
 * crates/io/src/text_styles_map.rs). Failures are out of memory only (unreadable engine data is
 * skipped); on failure the document may be partly updated. Thread-safe. */
#ifndef PC_IO_TEXT_STYLES_MAP_H
#define PC_IO_TEXT_STYLES_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "doc/doc.h"
#include "io/core.h"

/* Reads the style sheets of doc's type layers into doc's text styles (merged by name) and links
 * the runs to them; notes appended to warnings (may be NULL). */
pc_status pc_io_text_styles_import(pc_document *doc, pc_io_warnings *warnings, pc_error *err);
/* A TySh block (data = the preserved one) with the document styles written in; *has false when
 * neither the document nor the data has named styles, or the data can't be rewritten (write it
 * as is). *out replaced when *has. */
pc_status pc_io_export_tysh(const uint8_t *data, size_t len, const pc_text_layer *t,
                            const pc_text_styles *styles, float dpi, bool *has, pc_buf *out, pc_error *err);

#endif
