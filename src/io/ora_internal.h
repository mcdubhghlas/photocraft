/* Internals shared by the OpenRaster importer (ora.c) and exporter (ora_export.c): the archive
 * constants, the SVG compositing-op table and the grouped-warning collector (a port of the Rust
 * Warnings struct). Not a public header; included only by src/io/ora*.c. */
#ifndef PC_IO_ORA_INTERNAL_H
#define PC_IO_ORA_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "color/blend.h"
#include "io/core.h"

/* The `mimetype` entry of an OpenRaster archive ("image/openraster", 16 bytes). */
extern const char ORA_MIMETYPE[];
#define ORA_MIMETYPE_LEN 16u

/* Decoded pixels across every layer (and the stack.xml) are capped, as for PDN. */
#define ORA_MAX_PIXEL_BYTES ((size_t)1 << 30)
/* Layers and groups in one document. */
#define ORA_MAX_NODES ((size_t)8192)
#define ORA_MAX_STACK_XML ((size_t)16 << 20)
#define ORA_MAX_ENTRY ((size_t)512 << 20)
#define ORA_THUMBNAIL_SIDE 256u

/* The SVG compositing names of the OpenRaster specification (shared both ways). */
typedef struct ora_svg_op {
    const char *name;
    pc_blend_mode mode;
} ora_svg_op;
extern const ora_svg_op ORA_SVG_OPS[];
extern const size_t ORA_SVG_OP_COUNT;

/* Warnings gathered during import/export: every distinct general message once, per-layer ones
 * grouped by message with the first five layer names listed (port of ora.rs's Warnings). */
typedef struct {
    char *msg;
    char **layers;
    size_t n;
    size_t cap;
} ora_warn_group;

typedef struct {
    char **general;
    size_t ng;
    size_t cg;
    ora_warn_group *per;
    size_t np;
    size_t cp;
} ora_warnings;

void ora_warnings_init(ora_warnings *w);
void ora_warnings_fini(ora_warnings *w);
/* A distinct general message (deduplicated). */
pc_status ora_note(ora_warnings *w, const char *msg, pc_error *err);
/* A per-layer message, grouped: the same message lists every layer it applies to (once). */
pc_status ora_layer_warn(ora_warnings *w, const char *layer, const char *msg, pc_error *err);
/* Flattens into the result list: general messages first, then one grouped line per per-layer
 * message with its first five quoted layer names. */
pc_status ora_warnings_finish(const ora_warnings *w, pc_io_warnings *out, pc_error *err);

#endif
