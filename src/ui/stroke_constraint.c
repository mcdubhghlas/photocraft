#include "ui/stroke_constraint.h"
#include <math.h>

/* Screen points the pointer must travel before a Shift-constrained stroke locks its direction. */
#define PC_UI_STROKE_LOCK_DISTANCE 3.0
/* pi/4: the 45° snap step (M_PI is not ISO C). */
#define PC_UI_STROKE_FRAC_PI_4 0.78539816339744830961

bool pc_ui_stroke_constraint_applies(pc_ui_tool tool)
{
    return pc_ui_tool_is_brushlike(tool) || tool == PC_TOOL_GRADIENT;
}
bool pc_ui_stroke_constraint_connects(pc_ui_tool tool)
{
    return pc_ui_tool_is_brushlike(tool);
}
bool pc_ui_stroke_snapped_dir(const double origin[2], const double p[2], double step, double out[2])
{
    if (!origin || !p || !out)
        return false;
    double dx = p[0] - origin[0], dy = p[1] - origin[1];
    if (!isfinite(dx) || !isfinite(dy) || hypot(dx, dy) < 1e-9 || !(step > 0) || !isfinite(step))
        return false;
    double a = round(atan2(dy, dx) / step) * step;
    out[0] = cos(a);
    out[1] = sin(a);
    return true;
}
void pc_ui_stroke_project(const double origin[2], const double dir[2], const double p[2], double out[2])
{
    if (!origin || !dir || !p || !out)
        return;
    double t = (p[0] - origin[0]) * dir[0] + (p[1] - origin[1]) * dir[1];
    out[0] = origin[0] + dir[0] * t;
    out[1] = origin[1] + dir[1] * t;
}
void pc_ui_stroke_snap45(const double origin[2], const double p[2], double out[2])
{
    if (!p || !out)
        return;
    double dir[2];
    if (pc_ui_stroke_snapped_dir(origin, p, PC_UI_STROKE_FRAC_PI_4, dir))
        pc_ui_stroke_project(origin, dir, p, out);
    else {
        out[0] = p[0];
        out[1] = p[1];
    }
}
void pc_ui_stroke_constrain(pc_ui_tool tool, pc_ui_stroke_axis *axis, const double start[2],
                            const double last[2], const double p[2], bool shift, float zoom, double out[2])
{
    if (!axis || !p || !out)
        return;
    if (!shift || !pc_ui_stroke_constraint_applies(tool)) {
        *axis = (pc_ui_stroke_axis){0};
        out[0] = p[0];
        out[1] = p[1];
        return;
    }
    if (tool == PC_TOOL_GRADIENT) {
        pc_ui_stroke_snap45(start, p, out);
        return;
    }
    if (!axis->active) {
        axis->active = true;
        axis->locked = false;
        axis->origin[0] = last ? last[0] : p[0];
        axis->origin[1] = last ? last[1] : p[1];
    }
    if (!axis->locked) {
        double z = isfinite(zoom) && zoom > 1e-3f ? (double)zoom : 1e-3;
        double reach = PC_UI_STROKE_LOCK_DISTANCE / z;
        if (hypot(p[0] - axis->origin[0], p[1] - axis->origin[1]) < reach) {
            out[0] = axis->origin[0];
            out[1] = axis->origin[1];
            return;
        }
        double dir[2];
        if (pc_ui_stroke_snapped_dir(axis->origin, p, PC_UI_STROKE_FRAC_PI_4, dir)) {
            axis->locked = true;
            axis->dir[0] = dir[0];
            axis->dir[1] = dir[1];
        }
    }
    if (axis->locked)
        pc_ui_stroke_project(axis->origin, axis->dir, p, out);
    else {
        out[0] = axis->origin[0];
        out[1] = axis->origin[1];
    }
}
