#include "canvas_ink.h"
#include <math.h>
void pc_ui_canvas_ink_line(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, float x0, float y0,
                           float x1, float y1, float width, struct nk_color color)
{
    float a[2] = {x0, y0}, b[2] = {x1, y1};
    if (pc_ui_canvas_clip_line(xf, a, b))
        nk_stroke_line(canvas, a[0], a[1], b[0], b[1], width, color);
}
bool pc_ui_canvas_ink_visible(const pc_ui_canvas_xform *xf, float x, float y, float padding)
{
    return isfinite(x) && isfinite(y) && x >= -32740 && y >= -32740 && x <= 32740 && y <= 32740 &&
           x >= xf->x - padding && y >= xf->y - padding && x <= xf->x + xf->width + padding &&
           y <= xf->y + xf->height + padding;
}

void pc_ui_canvas_ink_dash(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, float x0, float y0,
                           float x1, float y1, float width, struct nk_color color, double dash, double gap,
                           double phase, size_t *budget)
{
    float a[2] = {x0, y0}, b[2] = {x1, y1};
    size_t local_budget = 4096;
    if (!budget)
        budget = &local_budget;
    if (!*budget || !isfinite(dash) || !isfinite(gap) || !isfinite(phase) || dash < 1 || gap < 1 ||
        dash + gap > 4096 || !pc_ui_canvas_clip_line(xf, a, b))
        return;
    double dx = (double)b[0] - a[0], dy = (double)b[1] - a[1], length = hypot(dx, dy);
    if (length < .001)
        return;
    double offset = fmod(phase + hypot((double)a[0] - x0, (double)a[1] - y0), dash + gap);
    if (offset < 0)
        offset += dash + gap;
    for (double at = -offset; at < length && *budget; at += dash + gap) {
        double start = fmax(0, at) / length, end = fmin(length, at + dash) / length;
        if (end <= start)
            continue;
        nk_stroke_line(canvas, (float)(a[0] + dx * start), (float)(a[1] + dy * start),
                       (float)(a[0] + dx * end), (float)(a[1] + dy * end), width, color);
        --*budget;
    }
}
