#include "crop_overlay.h"
#include <math.h>
static void line(float out[PC_UI_CROP_GUIDE_MAX][4], size_t *count, float x, float y, float a, float b)
{
    if (*count >= PC_UI_CROP_GUIDE_MAX)
        return;
    out[*count][0] = x;
    out[*count][1] = y;
    out[*count][2] = a;
    out[*count][3] = b;
    (*count)++;
}
size_t pc_ui_crop_guides(pc_ui_crop_overlay kind, unsigned orientation, float w, float h,
                         float out[PC_UI_CROP_GUIDE_MAX][4])
{
    if (!out || !isfinite(w) || !isfinite(h) || w < 1 || h < 1 || (unsigned)kind > PC_UI_CROP_GOLDEN_SPIRAL)
        return 0;
    size_t count = 0;
    if (kind == PC_UI_CROP_THIRDS || kind == PC_UI_CROP_GOLDEN_RATIO) {
        float a = kind == PC_UI_CROP_THIRDS ? 1.f / 3 : 1 - .618034f, b = 1 - a;
        line(out, &count, a, 0, a, 1);
        line(out, &count, 0, a, 1, a);
        line(out, &count, b, 0, b, 1);
        line(out, &count, 0, b, 1, b);
    } else if (kind == PC_UI_CROP_GRID) {
        for (unsigned axis = 0; axis < 2; axis++) {
            size_t n = (size_t)fminf(96, fmaxf(2, (axis ? h : w) / 32));
            for (size_t i = 1; i < n; i++) {
                float f = (float)i / (float)n;
                if (axis)
                    line(out, &count, 0, f, 1, f);
                else
                    line(out, &count, f, 0, f, 1);
            }
        }
    } else if (kind == PC_UI_CROP_DIAGONAL) {
        float m = fminf(w, h), u = m / w, v = m / h;
        line(out, &count, 0, 0, u, v);
        line(out, &count, 1, 0, 1 - u, v);
        line(out, &count, 0, 1, u, 1 - v);
        line(out, &count, 1, 1, 1 - u, 1 - v);
    } else if (kind == PC_UI_CROP_TRIANGLE) {
        float t = w >= h ? 1 / (1 + (h / w) * (h / w)) : 1 - 1 / (1 + (w / h) * (w / h)), s = 1 - t;
        line(out, &count, 0, 0, 1, 1);
        line(out, &count, 1, 0, t, t);
        line(out, &count, 0, 1, s, s);
    } else {
        const float phi = 1.618034f, pi = 3.14159265358979323846f;
        float r[4] = {0, 0, phi, 1}, point[2] = {0, 1};
        for (unsigned i = 0; i < 12; i++) {
            float size = fminf(r[2] - r[0], r[3] - r[1]), center[2], end[2];
            if (size < 1e-4f)
                break;
            switch (i % 4) {
            case 0:
                r[0] += size;
                center[0] = r[0];
                center[1] = r[3];
                end[0] = r[0];
                end[1] = r[1];
                break;
            case 1:
                r[1] += size;
                center[0] = r[0];
                center[1] = r[1];
                end[0] = r[2];
                end[1] = r[1];
                break;
            case 2:
                r[2] -= size;
                center[0] = r[2];
                center[1] = r[1];
                end[0] = r[2];
                end[1] = r[3];
                break;
            default:
                r[3] -= size;
                center[0] = r[2];
                center[1] = r[3];
                end[0] = r[0];
                end[1] = r[3];
                break;
            }
            float a0 = atan2f(point[1] - center[1], point[0] - center[0]),
                  da = atan2f(end[1] - center[1], end[0] - center[0]) - a0;
            if (da > pi)
                da -= 2 * pi;
            else if (da < -pi)
                da += 2 * pi;
            for (unsigned k = 1; k <= 12; k++) {
                float a = a0 + da * (float)k / 12,
                      next[2] = {center[0] + size * cosf(a), center[1] + size * sinf(a)};
                line(out, &count, point[0] / phi, point[1], next[0] / phi, next[1]);
                point[0] = next[0];
                point[1] = next[1];
            }
            point[0] = end[0];
            point[1] = end[1];
        }
    }
    unsigned o = orientation % pc_ui_crop_overlay_orientations(kind);
    for (size_t i = 0; i < count; i++)
        for (size_t a = 0; a < 4; a++) {
            float p = fminf(1, fmaxf(0, out[i][a]));
            out[i][a] = (o & (a % 2 ? 2u : 1u)) ? 1 - p : p;
        }
    return count;
}
