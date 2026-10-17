#include "canvas.h"
#include <math.h>

bool pc_ui_scroll_span_make(float length, float center, float size, float zoom, pc_ui_scroll_span *out)
{
    if (!out || !isfinite(zoom) || zoom <= 0.f || !isfinite(size) || size <= 0.f) {
        return false;
    }
    float half = size / zoom / 2.f;
    pc_ui_scroll_span s = {length, center - half, center + half};
    if (!isfinite(length) || length <= 0.f || !isfinite(s.first) || !isfinite(s.last) || s.last <= s.first) {
        return false;
    }
    *out = s;
    return true;
}

void pc_ui_scroll_extent(pc_ui_scroll_span s, bool overscroll, float out[2])
{
    out[0] = fminf(s.first, 0.f);
    out[1] = fmaxf(s.last, s.length);
    if (overscroll) {
        float visible = s.last - s.first;
        out[0] = fminf(out[0], s.length - visible);
        out[1] = fmaxf(out[1], visible);
    }
}

void pc_ui_scroll_thumb(pc_ui_scroll_span s, const float e[2], float track, float out[2])
{
    out[0] = 0;
    out[1] = 0;
    if (!isfinite(track) || track <= 0.f || !isfinite(e[0]) || !isfinite(e[1]) ||
        !isfinite(s.last - s.first) || s.last <= s.first || e[1] < e[0]) {
        return;
    }
    float total = fmaxf(e[1] - e[0], 1e-6f), visible = fminf(s.last - s.first, total);
    float length = fminf(track, fmaxf(fminf(24.f, track), visible / total * track));
    float travel = fmaxf(track - length, 0.f), room = total - visible;
    out[0] = room > 1e-6f ? fminf(1.f, fmaxf(0.f, (s.first - e[0]) / room)) * travel : 0.f;
    out[1] = length;
}

float pc_ui_scroll_doc_per_point(pc_ui_scroll_span s, const float e[2], float track)
{
    float thumb[2];
    pc_ui_scroll_thumb(s, e, track, thumb);
    float travel = track - thumb[1], room = e[1] - e[0] - (s.last - s.first);
    return isfinite(travel) && isfinite(room) && travel > 1e-3f && room > 0.f ? room / travel : 0.f;
}

float pc_ui_scroll_clamp(float center, float length, float size, float zoom)
{
    if (!isfinite(zoom) || zoom <= 0.f || !isfinite(length) || length <= 0.f || !isfinite(size) ||
        size <= 0.f) {
        return center;
    }
    if (!isfinite(center)) {
        return length / 2.f;
    }
    float half = size / zoom / 2.f, other = length - half;
    return fminf(fmaxf(half, other), fmaxf(fminf(half, other), center));
}
