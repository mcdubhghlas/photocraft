/* Photoshop gradient files (.grd, version 5: Photoshop 6 and later; port of
 * crates/psd/src/grd.rs).
 *
 * Layout: "8BGR", u16 version (5), then a versioned ActionDescriptor whose GrdL list holds Grdn
 * objects, each with a Grad object: "Nm  " name, GrdF form (CstS custom stops or ClNs noise),
 * Clrs colour stops (Clr colour, Type user/foreground/background, Lctn 0..4096, Mdpn midpoint %)
 * and Trns transparency stops (Opct %, Lctn, Mdpn). Version 3 files are rejected. Colours are
 * returned in the model they were stored in. Thread-safe. */
#ifndef PC_PSD_GRD_H
#define PC_PSD_GRD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

#define PC_PSD_GRD_MAX_GRADIENTS 10000u /* most gradients read from one file */
#define PC_PSD_GRD_MAX_STOPS 1000u      /* most stops read per gradient */

typedef enum pc_psd_grd_model {
    PC_PSD_GRD_RGB,        /* c[0..3] 0..1 (from 0..255 components) */
    PC_PSD_GRD_HSB,        /* hue degrees, saturation and brightness 0..1 */
    PC_PSD_GRD_CMYK,       /* c[0..4] ink 0..1 */
    PC_PSD_GRD_LAB,        /* L 0..100, a, b */
    PC_PSD_GRD_GRAY,       /* c[0] ink 0..1 (0 = white) */
    PC_PSD_GRD_FOREGROUND, /* the live foreground colour */
    PC_PSD_GRD_BACKGROUND  /* the live background colour */
} pc_psd_grd_model;

typedef struct pc_psd_grd_color {
    pc_psd_grd_model model;
    float c[4]; /* unused components are 0 */
} pc_psd_grd_color;

typedef struct pc_psd_grd_stop {
    float location; /* 0..1 */
    float midpoint; /* 0..1, 0.5 = centred */
    pc_psd_grd_color color;
} pc_psd_grd_stop;

typedef struct pc_psd_grd_opacity {
    float location; /* 0..1 */
    float opacity;  /* 0..1 */
    float midpoint; /* 0..1 */
} pc_psd_grd_opacity;

typedef struct pc_psd_grd_gradient {
    pc_psd_str name;
    bool noise; /* ClNs: no stops; callers usually skip or approximate it */
    pc_psd_grd_stop *stops;
    size_t nstops;
    pc_psd_grd_opacity *opacity;
    size_t nopacity;
} pc_psd_grd_gradient;

void pc_psd_grd_gradient_fini(pc_psd_grd_gradient *g);
void pc_psd_grd_free(pc_psd_grd_gradient *g, size_t n); /* fini each + free(g) */
bool pc_psd_grd_gradient_eq(const pc_psd_grd_gradient *a, const pc_psd_grd_gradient *b); /* f32 == */

/* Parses a .grd file into a new array (*n >= 1; no gradients is an error). */
pc_status pc_psd_grd_parse(const uint8_t *data, size_t len, pc_psd_grd_gradient **out, size_t *n, pc_error *err);
/* Writes a version 5 file (RGB / foreground / background stops; other models write black). */
pc_status pc_psd_grd_write(const pc_psd_grd_gradient *g, size_t n, pc_psd_bytes *out, pc_error *err);

#endif
