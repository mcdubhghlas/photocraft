/* Photoshop brushes (.abr) -> brush presets (chunk I4: crates/io/src/abr_map.rs).
 *
 * src/psd/abr.h reads the file; this maps its tips and brushPreset descriptors onto
 * pc_paint_brush (Brush Tip Shape, Shape Dynamics, Scattering, Texture, Dual Brush, Color
 * Dynamics, Transfer, Brush Pose, Noise, Wet Edges, Build-up, Smoothing and the captured tool
 * options). Settings without an equivalent are listed in the warnings instead of being dropped
 * silently. Descriptor key names follow the publicly documented ABR v6+ layout.
 *
 * Ownership: a pc_io_abr_import owns its presets and warnings (pc_io_abr_import_fini).
 * Out-params are written on success only. Thread-safe. */
#ifndef PC_IO_ABR_MAP_H
#define PC_IO_ABR_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "io/core.h"
#include "paint/presets.h"
#include "psd/abr.h"
#include "raster/interrupt.h"

/* Largest sampled tip edge kept as is; bigger tips are downsampled to it. */
#define PC_IO_ABR_MAX_TIP_EDGE 2500u
/* Largest texture pattern edge kept as is; bigger patterns are downsampled to it. */
#define PC_IO_ABR_MAX_PATTERN_EDGE 1024u

/* The presets read from one file plus everything that could not be mapped. */
typedef struct pc_io_abr_import {
    pc_paint_preset_list presets;
    pc_io_warnings warnings;
    uint16_t version; /* file version (1, 2, 6, ...) */
} pc_io_abr_import;

void pc_io_abr_import_fini(pc_io_abr_import *i); /* NULL-safe */

/* Parses an .abr file into presets in group `group` (usually the file name; NUL-terminated
 * UTF-8). An unreadable file keeps the psd status with "not a readable Photoshop brush file: ...". */
pc_status pc_io_read_abr(const uint8_t *bytes, size_t len, const char *group, pc_io_abr_import *out,
                         pc_error *err);
/* pc_io_read_abr checking ctl (may be NULL) before each brush and reporting progress (parsing
 * 0-10 %, mapping 10-100 %): a cancelled import fails with PC_ERR_CANCELLED "cancelled". */
pc_status pc_io_read_abr_with(const uint8_t *bytes, size_t len, const char *group, const pc_interrupt *ctl,
                              pc_io_abr_import *out, pc_error *err);
/* Maps a parsed file (fails only on out of memory). */
pc_status pc_io_abr_map_file(const pc_psd_abr *f, const char *group, pc_io_abr_import *out, pc_error *err);
/* pc_io_abr_map_file checking ctl (may be NULL) before each brush. */
pc_status pc_io_abr_map_file_with(const pc_psd_abr *f, const char *group, const pc_interrupt *ctl,
                                  pc_io_abr_import *out, pc_error *err);

#endif
