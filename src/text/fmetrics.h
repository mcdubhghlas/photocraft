/* Private to src/text: font metrics the layouter reads, ported from skrifa 0.44 (`Metrics::new`,
 * `GlyphMetrics::bounds`, `Charmap::map`) and read-fonts 0.41 (`VORG`, `vhea`/`vmtx`), with the
 * reference's treatment of damaged tables (a table whose fixed part is cut off is missing).
 * MVAR deltas of variable fonts come from HarfBuzz.
 *
 * Pure functions apart from FreeType's per-font lock (pc_font_draw); thread-safe. */
#ifndef PC_TEXT_FMETRICS_H
#define PC_TEXT_FMETRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "text/font.h"

/* skrifa's Metrics (the fields the layouter uses), scaled by size / upem (1 when unscaled). */
typedef struct pl_metrics {
    uint16_t units_per_em;
    float ascent, descent, leading; /* descent is negative (font convention) */
    bool has_cap_height;
    float cap_height;
    bool has_underline;
    float underline_offset, underline_thickness;
    bool has_strikeout;
    float strikeout_offset, strikeout_thickness;
} pl_metrics;

/* Metrics::new(font, size, coords): `size` NULL for Size::unscaled(). */
void pl_font_metrics(pc_font *f, const float *size, const int16_t *coords, size_t ncoords, pl_metrics *out);

/* GlyphMetrics::new(font, Size::new(size), coords).bounds(gid).y_max; false for None. */
bool pl_glyph_bounds_ymax(pc_font *f, float size, const int16_t *coords, size_t ncoords, uint32_t gid,
                          float *ymax);

/* skrifa Charmap::map: the nominal glyph of cp, 0 for none. */
uint32_t pl_skrifa_cmap(pc_font *f, uint32_t cp);

/* layout.rs VMetrics::vertical_origin (unscaled font units): VORG, else the glyph's top from
 * its outline plus its vmtx top side bearing. False when unavailable. */
bool pl_vertical_origin_units(pc_font *f, const int16_t *coords, size_t ncoords, uint32_t gid, float *out);

#endif
