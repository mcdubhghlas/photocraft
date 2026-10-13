/* Gradient interpolation methods baked into classic stops (chunk I4:
 * crates/io/src/gradient_bake.rs), used by I2's blocks and adjust_map. Thread-safe. */
#ifndef PC_IO_GRADIENT_BAKE_H
#define PC_IO_GRADIENT_BAKE_H

#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"

typedef enum pc_io_bake_method {
    PC_IO_BAKE_CLASSIC,
    PC_IO_BAKE_PERCEPTUAL,
    PC_IO_BAKE_LINEAR,
    PC_IO_BAKE_SMOOTH
} pc_io_bake_method;

/* "Perc" -> Perceptual, "Lnr " -> Linear, anything else (or NULL) -> Classic. */
pc_io_bake_method pc_io_bake_method_from_code(const uint8_t *code, size_t len);

typedef struct pc_io_color_stop {
    float at;
    pc_color color;
} pc_io_color_stop;

/* Stops (with midpoints and smoothness) resampled into 97 dense stops so linear sRGB
 * interpolation reproduces Photoshop's (Classic Lab stops stay Lab). A copy of the input when it
 * already interpolates linearly in sRGB (fewer than two stops, or Classic without smoothness or
 * midpoints). *out malloc'd (free()). Fails only on out of memory. */
pc_status pc_io_gradient_bake(const pc_io_color_stop *stops, size_t n, const float *midpoints, size_t nmid,
                              float smoothness, pc_io_bake_method method, pc_io_color_stop **out,
                              size_t *nout, pc_error *err);

#endif
