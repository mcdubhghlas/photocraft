/* Minimal EXIF reader for the camera settings the photography commands use (exposure, aperture,
 * ISO, focal length, camera and lens names) (port of crates/algo/src/exif.rs).
 *
 * Input is the raw TIFF-structured EXIF payload (`II*\0` / `MM\0*`, optionally behind an
 * `Exif\0\0` prefix), as stored in the document metadata. It is untrusted: every read is
 * bounds-checked, malformed data leaves fields unset, at most nine IFDs are visited. Tag numbers
 * are from EXIF 2.32 (CIPA DC-008). Thread-safe; no global state. */
#ifndef PC_ALGO_EXIF_H
#define PC_ALGO_EXIF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* An optional string: s == NULL for None, else malloc'd, NUL-terminated, `len` bytes of valid
 * UTF-8 (may contain interior NULs, as the reference keeps them). */
typedef struct pc_algo_exif_str {
    char *s;
    size_t len;
} pc_algo_exif_str;

typedef struct pc_algo_exif_num {
    bool set;
    double v;
} pc_algo_exif_num;

/* CameraInfo. Zero-initialise ({0}) for "all None". */
typedef struct pc_algo_camera_info {
    pc_algo_exif_str make, model, lens;
    pc_algo_exif_num exposure_time; /* seconds */
    pc_algo_exif_num f_number;
    pc_algo_exif_num iso;
    pc_algo_exif_num focal_length;      /* mm */
    pc_algo_exif_num focal_length_35mm; /* 35 mm-equivalent, mm */
} pc_algo_camera_info;

void pc_algo_camera_info_fini(pc_algo_camera_info *info);
/* Field-by-field equality (the reference's PartialEq; NaN != NaN). */
bool pc_algo_camera_info_equal(const pc_algo_camera_info *a, const pc_algo_camera_info *b);

/* Linear exposure factor t * ISO / N^2 (ISO 100 and f/1 when missing, N at least 0.1); false
 * when the exposure time is missing. */
bool pc_algo_exif_exposure_factor(const pc_algo_camera_info *info, double *out);

/* Reads the camera settings. Malformed input gives unset fields, never an error; the only
 * failure is PC_ERR_NOMEM. *out is initialised on success (free with pc_algo_camera_info_fini). */
pc_status pc_algo_exif_read(const uint8_t *exif, size_t len, pc_algo_camera_info *out, pc_error *err);

/* Builds a little-endian EXIF payload (IFD0 with Make, Model and the Exif IFD pointer; the Exif
 * IFD with the rest), appended to `out`. NOMEM / LIMIT for strings past 4 GiB. */
pc_status pc_algo_exif_build(const pc_algo_camera_info *info, pc_buf *out, pc_error *err);

#endif
