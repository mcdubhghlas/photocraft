/* Fill-layer contents (port of lib.rs Fill): a solid colour, a gradient or a pattern.
 * Conventions: doc/adjust.h. */
#ifndef PC_DOC_FILL_H
#define PC_DOC_FILL_H

#include "doc/effects.h"

typedef enum pc_fill_kind { PC_FILL_SOLID, PC_FILL_GRADIENT, PC_FILL_PATTERN } pc_fill_kind;

typedef struct pc_fill {
    pc_fill_kind kind;
    union {
        pc_color solid;
        struct {
            pc_color_stop *stops;
            size_t nstops;
            float angle;
            float scale;
            pc_gradient_style style;
            bool reverse;
            pc_opacity_stop *opacity_stops; /* empty = each colour stop's alpha */
            size_t nopacity_stops;
            float *midpoints; /* one per segment between sorted colour stops; missing = 0.5 */
            size_t nmidpoints;
            float offset[2];
            bool dither;
            bool align; /* serde default true */
        } gradient;
        struct {
            pc_str name;
            float scale;
            pc_str id;
            float angle;
            bool link; /* serde default true */
            float phase[2];
        } pattern;
    } u;
} pc_fill;

pc_fill pc_fill_solid(pc_color c);
/* A gradient with the classic settings (no opacity stops or midpoints, centred, no dither,
 * aligned); copies the n stops. */
pc_status pc_fill_gradient(const pc_color_stop *stops, size_t n, float angle, float scale, pc_gradient_style style,
                           bool reverse, pc_fill *out, pc_error *err);
void pc_fill_fini(pc_fill *f);
pc_status pc_fill_clone(const pc_fill *f, pc_fill *out, pc_error *err);
bool pc_fill_eq(const pc_fill *a, const pc_fill *b);
/* Converts the fill's colours to mode's model in place (pc_color_in_mode): a fill layer's
 * colours are in its document's mode, which the compositors read them as. Never fails. */
void pc_fill_in_mode(pc_fill *f, pc_color_mode mode);

#endif
