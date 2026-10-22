#include "ui/brush_tip.h"

#include <math.h>

#define PC_UI_PI 3.14159265358979323846

static float clampf(float v, float lo, float hi)
{
    if (!(v == v)) { /* NaN */
        return lo;
    }
    return v < lo ? lo : (v > hi ? hi : v);
}

/* sin/cos of the angle in degrees, with the screen's y pointing down (as the reference does). */
static void angle_sincos(float angle_deg, float *s, float *c)
{
    float r = angle_deg * (float)(PC_UI_PI / 180.0);
    *s = sinf(r);
    *c = cosf(r);
}

void pc_ui_ellipse_handles(float angle_deg, float roundness, float out[2][2])
{
    if (!out) {
        return;
    }
    if (!isfinite(angle_deg)) {
        angle_deg = 0;
    }
    float rr = clampf(roundness, 0, 1);
    float s, c;
    angle_sincos(angle_deg, &s, &c);
    /* Minor axis = major axis turned 90deg counter-clockwise; screen y points down. */
    float mx = -s * PC_UI_ELLIPSE_R * rr, my = -c * PC_UI_ELLIPSE_R * rr;
    out[0][0] = mx;
    out[0][1] = my;
    out[1][0] = -mx;
    out[1][1] = -my;
}

pc_ui_ellipse_grab pc_ui_ellipse_grab_at(float rx, float ry, float angle_deg, float roundness)
{
    if (!isfinite(rx) || !isfinite(ry)) {
        return PC_UI_ELLIPSE_ANGLE;
    }
    float h[2][2];
    pc_ui_ellipse_handles(angle_deg, roundness, h);
    for (int i = 0; i < 2; i++) {
        float dx = h[i][0] - rx, dy = h[i][1] - ry;
        if (sqrtf(dx * dx + dy * dy) <= 7.0f) {
            return PC_UI_ELLIPSE_ROUNDNESS;
        }
    }
    return PC_UI_ELLIPSE_ANGLE;
}

void pc_ui_ellipse_drag(pc_ui_ellipse_grab grab, float rx, float ry, float *angle_deg, float *roundness)
{
    if (!isfinite(rx) || !isfinite(ry)) {
        return;
    }
    if (grab == PC_UI_ELLIPSE_ROUNDNESS) {
        if (!roundness) {
            return;
        }
        float cur_angle = (angle_deg && isfinite(*angle_deg)) ? *angle_deg : 0;
        float s, c;
        angle_sincos(cur_angle, &s, &c);
        float along = fabsf(rx * -s + ry * -c);
        *roundness = clampf(along / PC_UI_ELLIPSE_R, 0.01f, 1.0f);
        return;
    }
    /* Angle: point at the pointer, but a press at the centre must not spin the arrow. */
    if (!angle_deg) {
        return;
    }
    float len = sqrtf(rx * rx + ry * ry);
    if (len > 1.0f) {
        float deg = atan2f(-ry, rx) * (float)(180.0 / PC_UI_PI);
        *angle_deg = roundf(deg);
    }
}

float pc_ui_brush_size_slider(float size, float max)
{
    if (!isfinite(max) || max < 1) {
        max = 1;
    }
    float s = clampf(size, 1, max);
    return logf(s);
}

float pc_ui_brush_slider_size(float slider, float max)
{
    if (!isfinite(max) || max < 1) {
        max = 1;
    }
    float top = logf(max);
    float lv = clampf(slider, 0, top);
    float s = roundf(expf(lv));
    return clampf(s, 1, max);
}

float pc_ui_brush_thumb_scale(float size)
{
    if (!isfinite(size)) {
        return 0.30f;
    }
    return clampf(size / 40.0f, 0.30f, 1.0f);
}

float pc_ui_texture_brightness_to_ps(float brightness)
{
    if (!isfinite(brightness)) {
        brightness = 0;
    }
    return roundf(clampf(brightness, -1, 1) * 150.0f);
}

float pc_ui_texture_brightness_from_ps(float ps)
{
    if (!isfinite(ps)) {
        ps = 0;
    }
    return clampf(ps, -150, 150) / 150.0f;
}

float pc_ui_texture_contrast_to_ps(float contrast)
{
    if (!isfinite(contrast)) {
        contrast = 0;
    }
    float c = clampf(contrast, -1, 1);
    return roundf(c < 0 ? c * 50.0f : c * 100.0f);
}

float pc_ui_texture_contrast_from_ps(float ps)
{
    if (!isfinite(ps)) {
        ps = 0;
    }
    float p = clampf(ps, -50, 100);
    return p < 0 ? p / 50.0f : p / 100.0f;
}
