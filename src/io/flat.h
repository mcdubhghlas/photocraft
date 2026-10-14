/* Flat raster formats through the codecs (port of crates/io/src/flat.rs): a decoded image
 * becomes a one-layer document; a document is rendered (or copied natively) to a codec image
 * and encoded with the fidelity warnings. Thread-safe (no shared state). */
#ifndef PC_IO_FLAT_H
#define PC_IO_FLAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codecs/codecs.h"
#include "doc/doc.h"
#include "io/core.h"

/* Bytes per band when converting or exporting a band of rows at a time. */
#define PC_IO_BAND_BYTES ((size_t)32 << 20)

/* The quality a lossy WebP is saved at when its own can't be read (DEFAULT_LOSSY_WEBP_QUALITY). */
#define PC_IO_DEFAULT_LOSSY_WEBP_QUALITY 75

/* ---- public ---- */

/* Opens one page of a TIFF or BigTIFF file: has_page false is the page Photoshop opens (the
 * first full-resolution one), else an index into pc_ctiff_info's pages (every IFD and SubIFD).
 * A page with Photoshop layer data opens layered (pc_io_import_layered); otherwise the TIFF
 * orientation is applied. *out initialised on success. */
pc_status pc_io_import_tiff_page(const char *name, const uint8_t *bytes, size_t len, bool has_page,
                                 size_t page, pc_io_import_result *out, pc_error *err);
/* Renders the document to a flat codec image: a single plain raster layer keeps its model and
 * depth (alpha dropped when opaque); anything else is composited (RGB or gray; CMYK and Lab
 * as sRGB) with a "flattened" warning. EXIF, XMP and the resolution go into the metadata.
 * *out initialised on success. */
pc_status pc_io_document_to_image(const pc_document *doc, pc_io_warnings *warnings, pc_cimage *out,
                                  pc_error *err);

/* As pc_io_document_to_image, but a CMYK document is reseparated to a 4-channel CMYK image (kept,
 * tagged with the document profile or built-in coated CMYK) only when cmyk_target; otherwise its
 * sRGB composite is written. The public entry above passes cmyk_target = true. */
pc_status pc_io_document_to_image_for(const pc_document *doc, pc_io_warnings *warnings, bool cmyk_target,
                                      pc_cimage *out, pc_error *err);

/* ---- internal to io (used by lib, raw, tiff_layers and I5's affinity) ---- */

/* Decodes a flat image into a one-layer document (TIFFs through pc_io_import_tiff_page). OpenEXR
 * and Radiance HDR RGB files without a profile are tagged linear sRGB. */
pc_status pc_io_import_flat(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                            pc_error *err);
/* A decoded image as a one-layer "Background" document (Gray / RGB / CMYK with alpha at the
 * image's depth; F16 stored as F32; locked when the image has no alpha). The decoder's warnings
 * come first. */
pc_status pc_io_image_to_document(const char *name, const pc_cimage *img, pc_io_import_result *out,
                                  pc_error *err);
/* image_surface: the pixels of a decoded image as a new surface in the target layout and sample
 * (Gray, RGB or CMYK layouts -> that mode, U8/U16 kept, anything else F32; always with alpha), converted a
 * band of rows at a time. Not pruned. *out initialised on success. */
pc_status pc_io_image_surface(const pc_cimage *img, pc_clayout target, pc_csample cs, pc_surface *out,
                              pc_error *err);
/* The one visible, unmasked, Normal, opaque raster layer whose pixels can be written natively,
 * or NULL. Borrowed. */
const pc_surface *pc_io_single_layer(const pc_document *doc);
pc_clayout pc_io_layout_for(pc_color_mode mode, bool alpha);
pc_csample pc_io_csample(pc_sample_type s);
/* malloc(pixels * bytes_per_pixel) or the reference's errors: "unsupported: image too large"
 * (overflow), "unsupported: not enough memory for a N MB image" (PC_ERR_NOMEM). */
pc_status pc_io_try_buffer(size_t pixels, size_t bytes_per_pixel, uint8_t **out, size_t *len, pc_error *err);
/* Fidelity warnings (a fatal one is "unsupported: ...") then the codec. Takes over *warnings
 * (emptied, also on failure). *out initialised on success. */
pc_status pc_io_encode_image(const pc_cimage *img, pc_cformat format, const pc_io_export_opts *opts,
                             pc_io_warnings *warnings, pc_io_export_result *out, pc_error *err);
/* Flattens and encodes as format (Indexed -> palette PNG, Duotone -> RGB first). */
pc_status pc_io_export_flat(const pc_document *doc, pc_cformat format, const pc_io_export_opts *opts,
                            pc_io_export_result *out, pc_error *err);
/* Colour-managed CMYK -> sRGB of a codec image (the embedded CMYK profile when it parses, else
 * the built-in coated CMYK), tagged sRGB. Exposed for tests. */
pc_status pc_io_cmyk_image_to_srgb(const pc_cimage *img, pc_cimage *out, pc_error *err);

#endif
