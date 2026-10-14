/* Shared io types (port of the type parts of crates/io/src/lib.rs): warnings, IoError kinds,
 * import and export results, export options, and the small containers every io file and every
 * later io chunk (I2-I5) uses. See io.h for the module overview and the chunk plan.
 *
 * Errors (IoError and its Display text). io functions return pc_status with err->msg holding the
 * reference's text:
 *   Psd           the psd status            "PSD: <psd error>"
 *   Codec         the codecs status         "codec: <codec error>"
 *   UnknownFormat PC_ERR_NOT_FOUND          "unknown file format: <name>"
 *   Unsupported   PC_ERR_UNSUPPORTED        "unsupported: <reason>"
 *   Pcraft        the format status         "pcraft: <format error>"
 *   Raw           the raw status            "<raw error>" (no prefix)
 *   Svg           PC_ERR_FORMAT             "SVG: <reason>"
 *   Cancelled     PC_ERR_CANCELLED          "cancelled"
 *   Pdn           PC_ERR_FORMAT             "PDN: <reason>"          (Paint.NET, R10)
 *   Ora           PC_ERR_FORMAT             "OpenRaster: <reason>"
 *   Kra           PC_ERR_FORMAT             "Krita: <reason>"
 * Out of memory is PC_ERR_NOMEM with a plain message (the reference aborts); it is never
 * prefixed. pc_io_error_kind recovers the variant from status and text.
 *
 * Warnings are owned NUL-terminated UTF-8 strings without a length cap (a warning that quotes
 * file data stops at an embedded U+0000). Thread safety: plain data, no shared state. */
#ifndef PC_IO_CORE_H
#define PC_IO_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "base/str.h"
#include "codecs/codecs.h"
#include "doc/doc.h"

#if defined(__GNUC__)
#define PC_IO_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define PC_IO_PRINTF(f, a)
#endif

/* ---- warnings (Vec<String>) ---- */

typedef struct pc_io_warnings {
    char **v;
    size_t n;
    size_t cap;
} pc_io_warnings;

#define PC_IO_WARNINGS_INIT {NULL, 0, 0}

void pc_io_warnings_init(pc_io_warnings *w);
void pc_io_warnings_fini(pc_io_warnings *w); /* NULL-safe; leaves w empty */
/* Appends a formatted warning (no length cap). */
pc_status pc_io_warn(pc_io_warnings *w, pc_error *err, const char *fmt, ...) PC_IO_PRINTF(3, 4);
/* Inserts a formatted warning at index `at` (clamped to n). */
pc_status pc_io_warn_at(pc_io_warnings *w, size_t at, pc_error *err, const char *fmt, ...) PC_IO_PRINTF(4, 5);
/* Appends prefix followed by each string of v[0..n) (prefix may be ""). */
pc_status pc_io_warnings_extend(pc_io_warnings *w, const char *prefix, char *const *v, size_t n,
                                pc_error *err);
/* Moves every warning of *from to the end of *w; *from is left empty (also on failure, when
 * the moved strings are freed). */
pc_status pc_io_warnings_append(pc_io_warnings *w, pc_io_warnings *from, pc_error *err);
void pc_io_warnings_remove(pc_io_warnings *w, size_t i);
/* Some warning contains needle. */
bool pc_io_warnings_any(const pc_io_warnings *w, const char *needle);

/* The import note for a file whose horizontal and vertical resolutions (pixels per inch) differ
 * by 0.01 ppi or more (#1018); appended to w. Nothing is added for equal axes. */
pc_status pc_io_unequal_resolution_warning(double x, double y, pc_io_warnings *w, pc_error *err);

/* ---- errors ---- */

typedef enum pc_io_err_kind {
    PC_IO_ERR_PSD,
    PC_IO_ERR_CODEC,
    PC_IO_ERR_UNKNOWN_FORMAT,
    PC_IO_ERR_UNSUPPORTED,
    PC_IO_ERR_PCRAFT,
    PC_IO_ERR_RAW,
    PC_IO_ERR_SVG,
    PC_IO_ERR_CANCELLED,
    PC_IO_ERR_PDN,
    PC_IO_ERR_ORA,
    PC_IO_ERR_KRA,
    PC_IO_ERR_OTHER /* out of memory, invalid arguments */
} pc_io_err_kind;

/* Which IoError variant a failure is (from its status and message prefix). */
pc_io_err_kind pc_io_error_kind(pc_status s, const pc_error *err);

/* Prefix err's message with `prefix` (e.g. "codec: ") and return s; out of memory is left as
 * it is. err may be NULL. */
pc_status pc_io_err_prefix(pc_status s, pc_error *err, const char *prefix);

/* Fill err with code and prefix + printf text. Use the macros below. */
void pc_io_fail_set(pc_error *err, pc_status code, const char *prefix, const char *fmt, ...)
    PC_IO_PRINTF(4, 5);

/* IoError::Unsupported("..."): PC_ERR_UNSUPPORTED "unsupported: ...". */
#define pc_io_unsupported(err, ...)                                                                          \
    (pc_io_fail_set((err), PC_ERR_UNSUPPORTED, "unsupported: ", __VA_ARGS__), (pc_status)PC_ERR_UNSUPPORTED)
/* IoError::Cancelled: PC_ERR_CANCELLED "cancelled". */
#define pc_io_cancelled(err)                                                                                 \
    (pc_io_fail_set((err), PC_ERR_CANCELLED, "", "cancelled"), (pc_status)PC_ERR_CANCELLED)
/* IoError::Svg("..."): PC_ERR_FORMAT "SVG: ...". */
#define pc_io_svg_error(err, ...)                                                                            \
    (pc_io_fail_set((err), PC_ERR_FORMAT, "SVG: ", __VA_ARGS__), (pc_status)PC_ERR_FORMAT)
/* IoError::Pdn / Ora / Kra("..."): PC_ERR_FORMAT "PDN: ...", "OpenRaster: ...", "Krita: ...". */
#define pc_io_pdn_error(err, ...)                                                                            \
    (pc_io_fail_set((err), PC_ERR_FORMAT, "PDN: ", __VA_ARGS__), (pc_status)PC_ERR_FORMAT)
#define pc_io_ora_error(err, ...)                                                                            \
    (pc_io_fail_set((err), PC_ERR_FORMAT, "OpenRaster: ", __VA_ARGS__), (pc_status)PC_ERR_FORMAT)
#define pc_io_kra_error(err, ...)                                                                            \
    (pc_io_fail_set((err), PC_ERR_FORMAT, "Krita: ", __VA_ARGS__), (pc_status)PC_ERR_FORMAT)
/* Out of memory while doing `what`. */
#define pc_io_nomem(err, what)                                                                               \
    (pc_io_fail_set((err), PC_ERR_NOMEM, "", "out of memory %s", (what)), (pc_status)PC_ERR_NOMEM)

/* ---- results ---- */

/* Result of an import. */
typedef struct pc_io_import_result {
    pc_document document;
    pc_io_warnings warnings; /* notes about anything approximated or dropped */
    bool source_read_only;   /* Save must not write back to the source (Affinity) */
    bool preview_only;       /* only a stand-in picture of the file (Affinity preview) */
} pc_io_import_result;

void pc_io_import_result_fini(pc_io_import_result *r); /* NULL-safe */

/* Result of an export. */
typedef struct pc_io_export_result {
    uint8_t *bytes; /* the encoded file (malloc'd; NULL when empty) */
    size_t len;
    pc_io_warnings warnings;
} pc_io_export_result;

void pc_io_export_result_fini(pc_io_export_result *r); /* NULL-safe */

/* ---- options ---- */

/* Which part of the document's XMP packet a flat export embeds (layered saves keep it all). */
typedef enum pc_io_xmp_embed { PC_IO_XMP_ALL, PC_IO_XMP_NONE } pc_io_xmp_embed;

typedef struct pc_io_export_opts {
    pc_cencode_opts encode; /* codec options for flat formats */
    bool force_psb;         /* write PSB even for .psd names */
    bool tiff_layers;       /* TIFF: keep the layers (tag 37724); off = Discard Layers */
    pc_io_xmp_embed xmp;
    bool image_previews;    /* PSD/PSB: write the 1036 thumbnail (Preferences default: on) */
    /* PSD/PSB: write the real merged image, Preferences > File Handling > Maximize PSD and PSB
     * File Compatibility (see pc_io_psd_export_opts::maximize_compatibility). On by default. */
    bool psd_maximize_compatibility;
} pc_io_export_opts;

pc_io_export_opts pc_io_export_opts_default(void); /* codec defaults, false, false, All */

/* Options of the PSD exporter (PsdExportOptions; implemented by I3's psd_export). */
typedef struct pc_io_psd_export_opts {
    bool force_psb;
    bool merged_matte;           /* matte the merged image against white (false: straight) */
    uint16_t merged_compression; /* PC_PSD_RAW / PC_PSD_RLE / ... (32-bit always raw) */
    /* Write the real merged image (Preferences > File Handling > Maximize PSD and PSB File
     * Compatibility). false writes Photoshop's white placeholder instead and records
     * has_real_merged_data = false in resource 1057: smaller file, faster save, but readers
     * that only show the merged image see white. A document without layer records always gets
     * the real image, since its pixels live nowhere else. */
    bool maximize_compatibility;
} pc_io_psd_export_opts;

pc_io_psd_export_opts pc_io_psd_export_opts_default(void); /* false, true, RLE, max-compat true */

/* ---- small containers ---- */

/* Vec<([u8; 4], Vec<u8>)>: a layer's tagged blocks while they are built (psd_export, shape and
 * artboard writers). Each data array is owned (malloc). */
typedef struct pc_io_raw_block {
    uint8_t key[4];
    uint8_t *data;
    size_t len;
} pc_io_raw_block;

typedef struct pc_io_raw_blocks {
    pc_io_raw_block *v;
    size_t n;
    size_t cap;
} pc_io_raw_blocks;

void pc_io_raw_blocks_fini(pc_io_raw_blocks *b); /* NULL-safe; leaves b empty */
/* Appends a copy of data. */
pc_status pc_io_raw_blocks_push(pc_io_raw_blocks *b, const uint8_t key[4], const void *data, size_t len,
                                pc_error *err);
/* Appends, taking ownership of *data (malloc'd; set to NULL, also on failure, when it is freed). */
pc_status pc_io_raw_blocks_push_take(pc_io_raw_blocks *b, const uint8_t key[4], uint8_t **data, size_t len,
                                     pc_error *err);
/* First block with key, borrowed, or NULL. */
const pc_io_raw_block *pc_io_raw_blocks_find(const pc_io_raw_blocks *b, const uint8_t key[4]);
/* Removes every block with key; returns how many. */
size_t pc_io_raw_blocks_remove_key(pc_io_raw_blocks *b, const uint8_t key[4]);

/* Vec<String> of length-carrying strings (may hold U+0000). */
typedef struct pc_io_strs {
    pc_str *v;
    size_t n;
    size_t cap;
} pc_io_strs;

void pc_io_strs_fini(pc_io_strs *s); /* NULL-safe; leaves s empty */
pc_status pc_io_strs_push(pc_io_strs *s, const char *p, size_t len, pc_error *err);

/* ---- document helpers ---- */

/* Appends layer at the top of the document's root (takes it over on success). */
pc_status pc_io_push_layer(pc_document *d, pc_layer *layer, pc_error *err);
/* A raster layer named name, taking over *s (emptied on success). */
pc_status pc_io_raster_layer(const char *name, pc_surface *s, pc_layer *out, pc_error *err);
/* Replace *slot with a new blob holding a copy of data (len 0 gives an empty blob). */
pc_status pc_io_set_blob(pc_blob **slot, const void *data, size_t len, pc_error *err);

#endif
