/* Pure geometry/number maths for the Brush Settings panel's bespoke panes (port of the widget
 * maths in crates/ui-egui/src/brush_sections.rs): the angle/roundness ellipse control, the
 * logarithmic size slider, the tip-grid thumbnail scale, and Photoshop's texture Brightness and
 * Contrast range mappings. No GUI dependency, so it is unit-tested headlessly; the Nuklear drawing
 * lives in ui/shell/brush_panel_ui.c.
 *
 * Never crash: all inputs may be hostile (NaN/inf coordinates, out-of-range sizes). Every function
 * is total and clamps to a sane range; NULL out-pointers are ignored. */
#ifndef PC_UI_BRUSH_TIP_H
#define PC_UI_BRUSH_TIP_H

#include <stdbool.h>

/* Outer radius of the angle/roundness widget's ellipse (screen pixels), matching the reference. */
#define PC_UI_ELLIPSE_R 34.0f

/* Which part of the angle/roundness widget a press grabbed. */
typedef enum pc_ui_ellipse_grab {
    PC_UI_ELLIPSE_ANGLE,
    PC_UI_ELLIPSE_ROUNDNESS
} pc_ui_ellipse_grab;

/* Screen offsets (relative to the widget centre, y pointing down) of the two roundness handles for
 * `angle_deg` (counter-clockwise degrees) and `roundness` (clamped 0..1). out[0]/out[1] receive
 * the two {x,y} offsets; a NULL out is ignored. */
void pc_ui_ellipse_handles(float angle_deg, float roundness, float out[2][2]);

/* What a press at offset (rx, ry) from the centre grabs: a roundness handle when within 7 px of
 * one, else the angle. Non-finite coordinates count as the angle. */
pc_ui_ellipse_grab pc_ui_ellipse_grab_at(float rx, float ry, float angle_deg, float roundness);

/* Apply a drag to (rx, ry) for the given grab, updating *angle_deg and/or *roundness in place:
 * the angle points at the pointer (degrees, counter-clockwise, rounded, -180..180) once the drag
 * leaves the 1 px dead zone at the centre; the roundness is the pointer's distance along the minor
 * axis (clamped 0.01..1). Non-finite coordinates leave the values unchanged. NULL pointers are
 * ignored. */
void pc_ui_ellipse_drag(pc_ui_ellipse_grab grab, float rx, float ry, float *angle_deg, float *roundness);

/* Logarithmic size slider: position for a size, and the size for a slider position. The slider
 * runs 0..ln(max); small sizes get most of the travel. Both clamp size to 1..max and tolerate a
 * non-finite or non-positive max (treated as 1). The size result is rounded to a whole pixel. */
float pc_ui_brush_size_slider(float size, float max);
float pc_ui_brush_slider_size(float slider, float max);

/* Thumbnail scale for a tip of `size` pixels in the tip grid: size/40 clamped to 0.30..1.00, so
 * small tips draw smaller and 40 px and up fill the cell. Non-finite size -> 0.30. */
float pc_ui_brush_thumb_scale(float size);

/* Photoshop's texture Brightness (slider ±150) <-> the model's ±1 fraction. */
float pc_ui_texture_brightness_to_ps(float brightness);   /* model -> slider, rounded */
float pc_ui_texture_brightness_from_ps(float ps);         /* slider -> model */

/* Photoshop's texture Contrast (slider -50..100) <-> the model's -1..1 fraction: negative maps
 * through 50, positive through 100 (matching the reference's asymmetric range). */
float pc_ui_texture_contrast_to_ps(float contrast);       /* model -> slider, rounded */
float pc_ui_texture_contrast_from_ps(float ps);           /* slider -> model */

#endif /* PC_UI_BRUSH_TIP_H */
