/* Artboards: the portable primitives of crates/engine/src/artboard_cmds.rs (E6).
 *
 * The Layer > New > Artboard, artboard-properties and File > Export > Artboards commands are
 * DEFERRED (reported to the coordinator): they need whole-document translation
 * (image_cmds::translate_doc), per-layer translation with its vectors (commands::translate_layer /
 * layer_copy, E4), the native/flat save path (file_cmds, E3) and canvas-size growth. This header
 * exposes only the pieces with no such dependency: the device/page size presets and the
 * raster-PDF writer (one JPEG page per artboard), so File > Export > Artboards to PDF can be wired
 * when its deps land. None of this is the public engine API (engine.h). */
#ifndef PC_ENGINE_ARTBOARD_CMDS_H
#define PC_ENGINE_ARTBOARD_CMDS_H

#include <stdbool.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "engine.h" /* pc_engine_command, pc_engine_session */

/* The size preset named `name` (case-insensitive), e.g. "iPhone 14", "A4", "Web 1920". Returns
 * false when there is no such preset. */
bool pc_engine_artboard_preset_size(const char *name, uint32_t *w, uint32_t *h);

/* One raster PDF page: a JPEG image `w` x `h` pixels at `dpi`, sized so it prints at resolution. */
typedef struct pc_engine_pdf_page {
    uint32_t w;
    uint32_t h;
    float dpi;
    const uint8_t *jpeg;
    size_t jpeg_len;
} pc_engine_pdf_page;

/* A minimal PDF with one page per JPEG image (raster_pdf). *out is initialised and filled; the
 * caller frees it with pc_buf_fini. */
pc_status pc_engine_artboard_raster_pdf(const pc_engine_pdf_page *pages, size_t n, pc_buf *out,
                                        pc_error *err);

/* The artboard command specs (register_modules() in command.c). Only the self-contained ones are
 * live here: View > Clear Selected Artboard Guides. The Layer > New > Artboard family, artboard
 * properties and the File > Export > Artboards commands are deferred (they need image_cmds
 * translate_doc + the canvas-grow shape refresh, E4 layer_copy, and E3 file I/O). */
const pc_engine_command *pc_engine_artboard_specs(size_t *n);

#endif /* PC_ENGINE_ARTBOARD_CMDS_H */
