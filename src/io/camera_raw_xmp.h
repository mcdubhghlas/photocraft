/* Camera Raw Filter > Save Settings... / Load Settings... (gap B9): Adobe Camera Raw settings
 * files (.xmp), the crs: namespace (http://ns.adobe.com/camera-raw-settings/1.0/) on an
 * x:xmpmeta/rdf:RDF/rdf:Description, as attributes or as child elements (port of
 * crates/io/src/camera_raw_xmp.rs).
 *
 * Loading applies only the settings the file holds, over the current ones, as Camera Raw's Load
 * Settings does; settings PhotoCraft has no control for (lens corrections, calibration, geometry,
 * defringe, ...) are ignored and named in the report. Saving writes every supported control, so
 * loading the file in Photoshop reproduces the dialog.
 *
 * White balance: a Camera Raw Filter on a rendered image uses the relative IncrementalTemperature /
 * IncrementalTint (-100..100), which are PhotoCraft's Temperature and Tint. The absolute
 * Temperature (kelvin) / Tint of raw-file settings have no counterpart and are ignored.
 *
 * Never panics on malformed input (REWRITE_PLAN.md rule 9). Thread-safe (no shared state). */
#ifndef PC_IO_CAMERA_RAW_XMP_H
#define PC_IO_CAMERA_RAW_XMP_H

#include <stddef.h>
#include <stdint.h>

#include "algo/camera_raw.h"
#include "base/buf.h"
#include "base/status.h"

/* The Camera Raw settings namespace. */
#define PC_IO_CR_XMP_NS "http://ns.adobe.com/camera-raw-settings/1.0/"
/* Largest settings file read; real ones are a few kilobytes. */
#define PC_IO_CR_XMP_MAX_BYTES (4u * 1024u * 1024u)

/* What pc_io_camera_raw_xmp_load applied. */
typedef struct pc_io_cr_xmp_report {
    size_t applied;   /* how many settings were applied */
    char **ignored;   /* owned: crs: settings with no PhotoCraft control (or an unusable value), in
                       * file order; each entry and the array are owned */
    size_t nignored;
} pc_io_cr_xmp_report;

void pc_io_cr_xmp_report_fini(pc_io_cr_xmp_report *r); /* NULL-safe */

/* Apply a Camera Raw settings file over *params. The report (optional) is written on success only.
 * PC_ERR_FORMAT for input that isn't XMP or holds no Camera Raw settings (params unchanged);
 * PC_ERR_NOMEM on out of memory (params unchanged). Never panics on malformed input. */
pc_status pc_io_camera_raw_xmp_load(pc_algo_camera_raw *params, const uint8_t *bytes, size_t len,
                                    pc_io_cr_xmp_report *report, pc_error *err);

/* Write *params as a Camera Raw settings file Photoshop's Load Settings... reads. *out is replaced
 * (released first) with the UTF-8 XML text (no trailing NUL in the buffer length). PC_ERR_NOMEM
 * only. */
pc_status pc_io_camera_raw_xmp_save(const pc_algo_camera_raw *params, pc_buf *out, pc_error *err);

#endif /* PC_IO_CAMERA_RAW_XMP_H */
