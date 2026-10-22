/* Camera raw files through src/raw (port of crates/io/src/raw.rs): developed to a 16-bit RGB
 * document in ProPhoto RGB (tagged with the built-in ProPhoto-compatible profile), or, for raw
 * variants not decoded yet, the camera's embedded JPEG preview with a warning. Errors are the
 * raw module's (IoError::Raw: its text without a prefix). Thread-safe. */
#ifndef PC_IO_RAW_IMPORT_H
#define PC_IO_RAW_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "io/core.h"
#include "raw/raw.h"

/* In the import note of a raw developed from its sensor data with the default settings (not of a
 * preview fallback): what the open-time Camera Raw dialog looks for. */
#define PC_IO_DEVELOPED_NOTE "developed with default settings"

/* Camera Raw's Basic-panel sliders applied while developing the sensor data (the open-time Camera
 * Raw dialog), as pc_algo_camera_raw's: Temperature / Tint (-100..100), Exposure (EV), and the
 * raw-stage tone sliders Shadows / Highlights / Clarity / Texture / Dehaze (-100..100), evaluated
 * in linear light before the tone curve, as Camera Raw does for raw files. All-zero (the default)
 * develops as the plain default open. */
typedef struct pc_io_raw_tuning {
    float temperature;
    float tint;
    float exposure;
    float shadows;
    float highlights;
    float clarity;
    float texture;
    float dehaze;
} pc_io_raw_tuning;

/* A camera raw file src/raw recognises. */
bool pc_io_is_raw(const uint8_t *bytes, size_t len);
/* The decode limits shared with the flat codecs. */
pc_raw_limits pc_io_raw_limits(void);
/* Develops with the default settings (pc_io_import_raw_with). */
pc_status pc_io_import_raw(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                           pc_error *err);
/* Develops with explicit settings; an unsupported variant opens its embedded JPEG preview
 * (turned upright) with a warning first, or fails when it has none. */
pc_status pc_io_import_raw_with(const char *name, const uint8_t *bytes, size_t len,
                                const pc_raw_develop_options *opts, pc_io_import_result *out, pc_error *err);
/* Develops with tuning applied at the raw stage: Exposure as develop exposure, Temperature / Tint
 * as gains on the as-shot white balance. *wb_applied is false when the file has no as-shot
 * white balance (Temperature / Tint were then not applied). An exposure outside -10..10 EV or
 * not finite is "malformed camera raw: exposure N EV is out of range". */
pc_status pc_io_import_raw_tuned(const char *name, const uint8_t *bytes, size_t len,
                                 const pc_io_raw_tuning *tuning, pc_io_import_result *out, bool *wb_applied,
                                 pc_error *err);
/* "Canon" + "Canon EOS 80D" reads as the model alone (the model starts with the make's first
 * word); otherwise make and model joined by a space. NULL parts are skipped. Returns the length
 * written (cut to cap - 1). */
size_t pc_io_camera_name(const char *make, const char *model, char *out, size_t cap);

/* The tone curve `bytes` develop with by default, for previews of the open-time raw-stage tone
 * sliders (pc_raw_retone): the sensor's camera-profile curve in linear-light [input, output] knots.
 * At most `cap` knots are written into `out`; *n is set to how many. A file that does not decode (or
 * carries no tone curve) yields *n = 0 (PC_OK), matching the reference's empty result. Never panics
 * on malformed input. PC_ERR_NOMEM only. */
pc_status pc_io_raw_tone_curve(const uint8_t *bytes, size_t len, float (*out)[2], size_t cap, size_t *n,
                               pc_error *err);

#endif
