/* Type layers read from a file, made ready for the canvas and the panels (chunk I4:
 * crates/io/src/text_import.rs). Thread-safe (holds the shared text engine's lock while it
 * works; documents without type layers never touch the engine). */
#ifndef PC_IO_TEXT_IMPORT_H
#define PC_IO_TEXT_IMPORT_H

#include "doc/doc.h"

/* Largest type raster the import draws on its own (pixels). */
#define PC_IO_TEXT_MAX_RENDER_PIXELS ((uint64_t)64 * 1024 * 1024)

/* Resolves PostScript font names to installed families and renders type layers that came
 * without pixels (ag-psd, GIMP and other writers leave them out). A layer whose ink would exceed
 * PC_IO_TEXT_MAX_RENDER_PIXELS gets an empty cache; one whose text the layout refuses keeps none.
 * Fails only on out of memory (the document may then be partly prepared). */
pc_status pc_io_text_prepare(pc_document *doc, pc_error *err);

#endif
