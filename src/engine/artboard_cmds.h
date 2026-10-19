/* Artboard creation, properties, layout presets and raster PDF export. */
#ifndef PC_ENGINE_ARTBOARD_CMDS_H
#define PC_ENGINE_ARTBOARD_CMDS_H

#include <stdbool.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "engine.h" /* pc_engine_command, pc_engine_session */

typedef struct pc_engine_artboard_preset {
    const char *name;
    uint32_t w, h;
} pc_engine_artboard_preset;
/* Borrowed immutable preset table. */
const pc_engine_artboard_preset *pc_engine_artboard_presets(size_t *count);
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

/* Command registry, including artboard creation, properties and file export. */
const pc_engine_command *pc_engine_artboard_specs(size_t *n);

#endif /* PC_ENGINE_ARTBOARD_CMDS_H */
