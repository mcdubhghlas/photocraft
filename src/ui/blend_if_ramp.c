#include "ui/blend_if_ramp.h"

#include <math.h>

size_t pc_ui_blend_if_nearest(const uint8_t q[4], double x)
{
    if (!q)
        return 0;
    size_t near = 0;
    double best = fabs((double)q[0] - x);
    for (size_t i = 1; i < 4; i++) {
        double d = fabs((double)q[i] - x);
        if (d < best) {
            best = d;
            near = i;
        }
    }
    /* Between two coinciding halves, pick by the side of the pointer. */
    if ((near == 0 || near == 1) && q[0] == q[1])
        near = x > (double)q[0] ? 1 : 0;
    else if ((near == 2 || near == 3) && q[2] == q[3])
        near = x > (double)q[2] ? 3 : 2;
    return near;
}

static uint8_t clamp8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

bool pc_ui_blend_if_drag(uint8_t q[4], size_t h, int target, bool split)
{
    if (!q || h >= 4)
        return false;
    if (target < 0)
        target = 0;
    else if (target > 255)
        target = 255;
    uint8_t n[4] = {q[0], q[1], q[2], q[3]};
    size_t pair = h < 2 ? 0 : 2;
    if (split)
        n[h] = (uint8_t)target;
    else {
        int delta = target - (int)q[h];
        n[pair] = clamp8((int)n[pair] + delta);
        n[pair + 1] = clamp8((int)n[pair + 1] + delta);
    }
    /* Push the other handles so the dragged one keeps its place, then restore full order. */
    for (size_t i = 0; i < 4; i++) {
        if (i < h && n[i] > n[h])
            n[i] = n[h];
        else if (i > h && n[i] < n[h])
            n[i] = n[h];
    }
    for (size_t i = 1; i < 4; i++)
        if (n[i] < n[i - 1])
            n[i] = n[i - 1];
    bool changed = n[0] != q[0] || n[1] != q[1] || n[2] != q[2] || n[3] != q[3];
    if (changed) {
        q[0] = n[0];
        q[1] = n[1];
        q[2] = n[2];
        q[3] = n[3];
    }
    return changed;
}
