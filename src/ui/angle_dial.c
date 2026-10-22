#include "ui/angle_dial.h"

#include <math.h>

/* M_PI is not ISO C. */
static const double PC_UI_PI = 3.14159265358979323846;

static double clamp_range(double x, double lo, double hi)
{
    /* max(lo) then min(hi), as the reference does, so a reversed [lo,hi] still terminates. */
    if (x < lo)
        x = lo;
    if (x > hi)
        x = hi;
    return x;
}

bool pc_ui_angle_from_point(double cx, double cy, double px, double py, double lo, double hi,
                            double *out)
{
    double dx = px - cx, dy = py - cy;
    if (!isfinite(dx) || !isfinite(dy) || dx * dx + dy * dy <= 1.0)
        return false;
    double deg = atan2(-dy, dx) * 180.0 / PC_UI_PI;
    deg = round(deg);
    if (out)
        *out = clamp_range(deg, lo, hi);
    return true;
}

void pc_ui_angle_tip(double angle_deg, double cx, double cy, double radius, double *tx, double *ty)
{
    double r = angle_deg * PC_UI_PI / 180.0;
    if (tx)
        *tx = cx + cos(r) * radius;
    if (ty)
        *ty = cy - sin(r) * radius;
}

double pc_ui_angle_step(double angle_deg, double step, double lo, double hi)
{
    if (!isfinite(angle_deg))
        angle_deg = 0;
    double stepped;
    if (lo >= -180.0 && hi <= 180.0) {
        double wrapped = fmod(angle_deg + step + 180.0, 360.0);
        if (wrapped < 0)
            wrapped += 360.0;
        stepped = wrapped - 180.0;
    } else
        stepped = angle_deg + step;
    return clamp_range(stepped, lo, hi);
}
