/* File > Automate > Crop and Straighten Photos (port of crates/algo/src/scancrop.rs): find the
 * photos on a flatbed scan, measure each one's rotation, and return their rectangles.
 *
 *  1. Background: the per-channel median of the (downsampled, <= 800 px) image border.
 *  2. Foreground: colour distance to the background above an Otsu threshold (at least 0.04),
 *     closed then opened morphologically, split into 8-connected components; components under
 *     1 % of the scan are dropped.
 *  3. Rotation: the minimum-area rectangle of each component's convex hull (Freeman & Shapira
 *     1975), shrunk by one downsampled pixel per side, in full-resolution px.
 * Pure; thread-safe. */
#ifndef PC_ALGO_SCANCROP_H
#define PC_ALGO_SCANCROP_H

#include <stddef.h>

#include "base/status.h"

/* A found photo: centre, size along its own axes, the angle of its x axis in the scan (degrees,
 * -45..45) and its area as a fraction of the scan. */
typedef struct pc_algo_found_photo {
    double center[2];
    double width, height;
    double angle;
    double area;
} pc_algo_found_photo;

/* The four corners in the scan, clockwise from the photo's top-left. */
void pc_algo_found_photo_corners(const pc_algo_found_photo *p, double out[4][2]);

/* Finds the photos on a scan of straight RGBA pixels (w * h of them; n is the buffer's pixel
 * count and must be at least w * h). Photos come in reading order (rows by centre y in bands of a
 * third of the smallest photo, then x). *out is malloc'd (NULL when none; caller frees).
 * PC_ERR_INVALID for a short buffer; NOMEM / LIMIT. */
pc_status pc_algo_scancrop_find_photos(size_t w, size_t h, const float (*rgba)[4], size_t n,
                                       pc_algo_found_photo **out, size_t *count, pc_error *err);

#endif
