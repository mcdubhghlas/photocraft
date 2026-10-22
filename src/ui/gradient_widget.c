#include "ui/gradient_widget.h"

#include <math.h>

/* M_PI is not ISO C. */
static const double PC_UI_PI = 3.14159265358979323846;

static double clamp01(double v)
{
    if (!(v > 0.0)) { /* also catches NaN */
        return 0.0;
    }
    return v > 1.0 ? 1.0 : v;
}

static bool finite2(const double p[2])
{
    return p != NULL && isfinite(p[0]) && isfinite(p[1]);
}

/* Gradient location -> fraction u along from->to (0 at from, 1 at to), honouring reverse. */
static double u_of(const pc_ui_grad_widget *w, double t)
{
    t = clamp01(t);
    return w->reverse ? 1.0 - t : t;
}

void pc_ui_grad_widget_at(const pc_ui_grad_widget *w, double t, double out[2])
{
    if (out == NULL) {
        return;
    }
    out[0] = out[1] = 0.0;
    if (w == NULL || !finite2(w->from) || !finite2(w->to)) {
        return;
    }
    double u = u_of(w, t);
    out[0] = w->from[0] + (w->to[0] - w->from[0]) * u;
    out[1] = w->from[1] + (w->to[1] - w->from[1]) * u;
}

void pc_ui_grad_widget_project(const pc_ui_grad_widget *w, double px, double py, double *out_u,
                               double *out_perp)
{
    double u = 0.0, perp = INFINITY;
    if (w != NULL && finite2(w->from) && finite2(w->to) && isfinite(px) && isfinite(py)) {
        double dx = w->to[0] - w->from[0], dy = w->to[1] - w->from[1];
        double len2 = dx * dx + dy * dy;
        double rx = px - w->from[0], ry = py - w->from[1];
        if (len2 > 1e-12) {
            u = clamp01((rx * dx + ry * dy) / len2);
            double cx = w->from[0] + dx * u, cy = w->from[1] + dy * u;
            perp = hypot(px - cx, py - cy);
        } else {
            perp = hypot(rx, ry);
        }
    }
    if (out_u != NULL) {
        *out_u = u;
    }
    if (out_perp != NULL) {
        *out_perp = perp;
    }
}

pc_ui_grad_grab pc_ui_grad_widget_hit(const pc_ui_grad_widget *w, double px, double py, double tol)
{
    pc_ui_grad_grab grab = {PC_UI_GRAD_GRAB_NONE, -1, 0.0};
    if (w == NULL || !finite2(w->from) || !finite2(w->to) || !isfinite(px) || !isfinite(py) ||
        !isfinite(tol) || tol < 0.0) {
        return grab;
    }
    /* 1. end, 2. start. */
    if (hypot(px - w->to[0], py - w->to[1]) <= tol) {
        grab.kind = PC_UI_GRAD_GRAB_END;
        return grab;
    }
    if (hypot(px - w->from[0], py - w->from[1]) <= tol) {
        grab.kind = PC_UI_GRAD_GRAB_START;
        return grab;
    }
    /* 3. colour stops. */
    size_t ns = w->nstops <= PC_UI_GRAD_WIDGET_MAX ? w->nstops : PC_UI_GRAD_WIDGET_MAX;
    for (size_t i = 0; i < ns; i++) {
        double at[2];
        pc_ui_grad_widget_at(w, w->stops[i], at);
        if (hypot(px - at[0], py - at[1]) <= tol) {
            grab.kind = PC_UI_GRAD_GRAB_STOP;
            grab.index = (int)i;
            return grab;
        }
    }
    /* 4. segment midpoints (between consecutive colour stops). */
    size_t nm = w->nmids <= PC_UI_GRAD_WIDGET_MAX ? w->nmids : PC_UI_GRAD_WIDGET_MAX;
    for (size_t i = 0; i + 1 < ns && i < nm; i++) {
        double a = w->stops[i], b = w->stops[i + 1];
        if (b - a <= 1e-4) {
            continue;
        }
        double frac = clamp01(w->mids[i]);
        double loc = a + (b - a) * frac;
        double at[2];
        pc_ui_grad_widget_at(w, loc, at);
        if (hypot(px - at[0], py - at[1]) <= tol) {
            grab.kind = PC_UI_GRAD_GRAB_MID;
            grab.index = (int)i;
            return grab;
        }
    }
    /* 5. the line body. */
    double u, perp;
    pc_ui_grad_widget_project(w, px, py, &u, &perp);
    if (perp <= tol) {
        grab.kind = PC_UI_GRAD_GRAB_LINE;
        grab.t = w->reverse ? 1.0 - u : u; /* gradient location at the hit */
    }
    return grab;
}

void pc_ui_grad_snap45(const double anchor[2], const double p[2], bool shift, double out[2])
{
    if (out == NULL) {
        return;
    }
    if (!finite2(p)) {
        out[0] = out[1] = 0.0;
        return;
    }
    out[0] = p[0];
    out[1] = p[1];
    if (!shift || !finite2(anchor)) {
        return;
    }
    double dx = p[0] - anchor[0], dy = p[1] - anchor[1];
    double len = hypot(dx, dy);
    if (len < 1e-9) {
        return;
    }
    double angle = atan2(dy, dx);
    double step = PC_UI_PI / 4.0;
    double snapped = round(angle / step) * step;
    out[0] = anchor[0] + cos(snapped) * len;
    out[1] = anchor[1] + sin(snapped) * len;
}

bool pc_ui_grad_decor_circle(const pc_ui_grad_widget *w, double center[2], double *radius)
{
    if (w == NULL || !finite2(w->from) || !finite2(w->to)) {
        return false;
    }
    double r = hypot(w->to[0] - w->from[0], w->to[1] - w->from[1]);
    if (!(r > 1e-6)) {
        return false;
    }
    if (center != NULL) {
        center[0] = w->from[0];
        center[1] = w->from[1];
    }
    if (radius != NULL) {
        *radius = r;
    }
    return true;
}

bool pc_ui_grad_decor_diamond(const pc_ui_grad_widget *w, double corners[4][2])
{
    if (w == NULL || corners == NULL || !finite2(w->from) || !finite2(w->to)) {
        return false;
    }
    double dx = w->to[0] - w->from[0], dy = w->to[1] - w->from[1];
    if (!(hypot(dx, dy) > 1e-6)) {
        return false;
    }
    double nx = -dy, ny = dx; /* perpendicular, same length as d */
    corners[0][0] = w->from[0] + dx; /* +d (the `to` tip) */
    corners[0][1] = w->from[1] + dy;
    corners[1][0] = w->from[0] + nx; /* +n */
    corners[1][1] = w->from[1] + ny;
    corners[2][0] = w->from[0] - dx; /* -d */
    corners[2][1] = w->from[1] - dy;
    corners[3][0] = w->from[0] - nx; /* -n */
    corners[3][1] = w->from[1] - ny;
    return true;
}

bool pc_ui_grad_decor_reflected(const pc_ui_grad_widget *w, double mirror[2])
{
    if (w == NULL || mirror == NULL || !finite2(w->from) || !finite2(w->to)) {
        return false;
    }
    double dx = w->to[0] - w->from[0], dy = w->to[1] - w->from[1];
    if (!(hypot(dx, dy) > 1e-6)) {
        return false;
    }
    mirror[0] = 2.0 * w->from[0] - w->to[0]; /* from - (to - from) */
    mirror[1] = 2.0 * w->from[1] - w->to[1];
    return true;
}
