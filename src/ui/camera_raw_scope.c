/* Camera Raw scope + preview navigation model. See camera_raw_scope.h. Pure; headless-testable. */
#include "ui/camera_raw_scope.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_UI_CR_PI 3.14159265358979323846f

/* ---- small helpers ---- */
static float clampf(float v, float lo, float hi)
{
    if (!(v > lo)) /* also catches NaN -> lo */
        v = lo;
    if (v > hi)
        v = hi;
    return v;
}
static float finite_or(float v, float fallback)
{
    return isfinite(v) ? v : fallback;
}

/* ---- tone zones ---- */
pc_ui_cr_tone_zone pc_ui_cr_tone_zone_at(float x)
{
    if (!isfinite(x) || x < 0.1f)
        return PC_UI_CR_ZONE_BLACKS;
    if (x < 0.25f)
        return PC_UI_CR_ZONE_SHADOWS;
    if (x < 0.75f)
        return PC_UI_CR_ZONE_EXPOSURE;
    if (x < 0.9f)
        return PC_UI_CR_ZONE_HIGHLIGHTS;
    return PC_UI_CR_ZONE_WHITES;
}
const char *pc_ui_cr_tone_zone_name(pc_ui_cr_tone_zone zone)
{
    switch (zone) {
    case PC_UI_CR_ZONE_BLACKS:
        return "Blacks";
    case PC_UI_CR_ZONE_SHADOWS:
        return "Shadows";
    case PC_UI_CR_ZONE_EXPOSURE:
        return "Exposure";
    case PC_UI_CR_ZONE_HIGHLIGHTS:
        return "Highlights";
    case PC_UI_CR_ZONE_WHITES:
        return "Whites";
    }
    return "Exposure";
}
const char *pc_ui_cr_tone_zone_key(pc_ui_cr_tone_zone zone)
{
    switch (zone) {
    case PC_UI_CR_ZONE_BLACKS:
        return "blacks";
    case PC_UI_CR_ZONE_SHADOWS:
        return "shadows";
    case PC_UI_CR_ZONE_EXPOSURE:
        return "exposure";
    case PC_UI_CR_ZONE_HIGHLIGHTS:
        return "highlights";
    case PC_UI_CR_ZONE_WHITES:
        return "whites";
    }
    return "exposure";
}
void pc_ui_cr_tone_zone_range(pc_ui_cr_tone_zone zone, float out[2])
{
    static const float ranges[5][2] = {
        {0.0f, 0.1f}, {0.1f, 0.25f}, {0.25f, 0.75f}, {0.75f, 0.9f}, {0.9f, 1.0f}};
    size_t i = (size_t)zone <= 4 ? (size_t)zone : 2;
    out[0] = ranges[i][0];
    out[1] = ranges[i][1];
}
float pc_ui_cr_tone_zone_limit(pc_ui_cr_tone_zone zone)
{
    return zone == PC_UI_CR_ZONE_EXPOSURE ? 5.0f : 100.0f;
}
float pc_ui_cr_tone_zone_clamp(pc_ui_cr_tone_zone zone, float value)
{
    float limit = pc_ui_cr_tone_zone_limit(zone);
    return clampf(finite_or(value, 0.0f), -limit, limit);
}

/* ---- clipping ---- */
uint8_t pc_ui_cr_shadow_mask(const pc_algo_rgb_histogram *h)
{
    uint8_t mask = 0;
    if (!h)
        return 0;
    for (uint8_t c = 0; c < 3; c++)
        if (h->shadows[c] > 0)
            mask |= (uint8_t)(1u << c);
    return mask;
}
uint8_t pc_ui_cr_highlight_mask(const pc_algo_rgb_histogram *h)
{
    uint8_t mask = 0;
    if (!h)
        return 0;
    for (uint8_t c = 0; c < 3; c++)
        if (h->highlights[c] > 0)
            mask |= (uint8_t)(1u << c);
    return mask;
}
bool pc_ui_cr_clip_color(const float p[4], pc_ui_cr_clip_mode mode, uint8_t warn, const pc_ui_tokens *t,
                         pc_ui_color *out)
{
    if (!p || !t || !out || mode == PC_UI_CR_CLIP_NONE)
        return false;
    bool transparent = p[3] <= 0.0f || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) ||
                       !isfinite(p[3]);
    if (transparent)
        return false;
    uint8_t lo = 0, hi = 0;
    pc_algo_clipping(p, &lo, &hi);
    switch (mode) {
    case PC_UI_CR_CLIP_SHADOW_CHANNELS:
        *out = pc_ui_tokens_histogram_color(t, (uint8_t)(7u ^ lo));
        return true;
    case PC_UI_CR_CLIP_HIGHLIGHT_CHANNELS:
        *out = hi == 0 ? t->histogram_bg : pc_ui_tokens_histogram_color(t, hi);
        return true;
    case PC_UI_CR_CLIP_WARNINGS:
        if ((warn & 2u) && hi != 0) {
            *out = pc_ui_tokens_histogram_color(t, 1); /* highlight clip -> red */
            return true;
        }
        if ((warn & 1u) && lo != 0) {
            *out = pc_ui_tokens_histogram_color(t, 4); /* shadow clip -> blue */
            return true;
        }
        return false;
    case PC_UI_CR_CLIP_NONE:
        return false;
    }
    return false;
}

/* ---- vectorscope geometry ---- */
pc_ui_color pc_ui_cr_scope_hue(const pc_ui_tokens *t, float h, float s)
{
    float v = (float)(t ? t->histogram_level : 255) / 255.0f;
    if (!isfinite(h))
        h = 0.0f;
    s = clampf(finite_or(s, 0.0f), 0.0f, 1.0f);
    pc_ui_color c = {0, 0, 0, 255};
    uint8_t *ch[3] = {&c.r, &c.g, &c.b};
    const float offsets[3] = {5.0f, 3.0f, 1.0f};
    for (size_t i = 0; i < 3; i++) {
        float k = fmodf(offsets[i] + h * 6.0f, 6.0f);
        if (k < 0.0f)
            k += 6.0f;
        float m = fminf(k, 4.0f - k);
        m = clampf(m, 0.0f, 1.0f);
        float value = v * (1.0f - s * m) * 255.0f;
        *ch[i] = (uint8_t)clampf(value, 0.0f, 255.0f);
    }
    return c;
}
float pc_ui_cr_scope_radius(const float rect[4])
{
    float side = fminf(rect[2], rect[3]);
    float r = side * 0.5f - 14.0f;
    return r > 1.0f ? r : 1.0f;
}
void pc_ui_cr_scope_point(const float rect[4], bool red_right, float hue, float saturation, float out[2])
{
    float cx = rect[0] + rect[2] * 0.5f, cy = rect[1] + rect[3] * 0.5f;
    float radius = pc_ui_cr_scope_radius(rect);
    hue = finite_or(hue, 0.0f);
    saturation = finite_or(saturation, 0.0f);
    float offset = red_right ? 0.0f : -100.0f * PC_UI_CR_PI / 180.0f;
    float angle = offset - hue * 2.0f * PC_UI_CR_PI;
    out[0] = cx + cosf(angle) * saturation * radius;
    out[1] = cy + sinf(angle) * saturation * radius;
    if (!isfinite(out[0]))
        out[0] = cx;
    if (!isfinite(out[1]))
        out[1] = cy;
}

/* ---- readouts ---- */
void pc_ui_cr_readout_rgb(const float rgb[3], double out[3])
{
    for (size_t i = 0; i < 3; i++)
        out[i] = (double)finite_or(rgb[i], 0.0f) * 255.0;
}
void pc_ui_cr_readout_lab(const float lab[3], double out[3])
{
    out[0] = (double)finite_or(lab[0], 0.0f) * 100.0;
    out[1] = (double)finite_or(lab[1], 0.0f) * 255.0 - 128.0;
    out[2] = (double)finite_or(lab[2], 0.0f) * 255.0 - 128.0;
}
void pc_ui_cr_readout_text(const double values[3], bool lab, char *out, size_t n)
{
    if (!out || n == 0)
        return;
    double a = values ? values[0] : 0, b = values ? values[1] : 0, c = values ? values[2] : 0;
    int written;
    if (lab)
        written = snprintf(out, n, "L %.1f  a %.1f  b %.1f", a, b, c);
    else
        written = snprintf(out, n, "R %.0f  G %.0f  B %.0f", a, b, c);
    if (written < 0)
        out[0] = '\0';
}

/* ---- samplers ---- */
bool pc_ui_cr_point_valid(const float p[2])
{
    return p && isfinite(p[0]) && isfinite(p[1]) && p[0] >= 0.0f && p[0] <= 1.0f && p[1] >= 0.0f &&
           p[1] <= 1.0f;
}
void pc_ui_cr_scope_sanitize(pc_ui_camera_raw_scope *view)
{
    if (!view)
        return;
    if (view->n_samplers > PC_UI_CR_MAX_SAMPLERS)
        view->n_samplers = PC_UI_CR_MAX_SAMPLERS;
    if (view->samplers) {
        size_t w = 0;
        for (size_t i = 0; i < view->n_samplers; i++)
            if (pc_ui_cr_point_valid(view->samplers[i])) {
                if (w != i) {
                    view->samplers[w][0] = view->samplers[i][0];
                    view->samplers[w][1] = view->samplers[i][1];
                }
                w++;
            }
        view->n_samplers = w;
    } else {
        view->n_samplers = 0;
    }
    if (view->has_floating_rect && !pc_ui_cr_floating_rect_valid(view->floating_rect))
        view->has_floating_rect = false;
}
bool pc_ui_cr_scope_add_sampler(pc_ui_camera_raw_scope *view, const float p[2])
{
    if (!view || !pc_ui_cr_point_valid(p) || view->n_samplers >= PC_UI_CR_MAX_SAMPLERS)
        return false;
    float(*grown)[2] = realloc(view->samplers, (view->n_samplers + 1) * sizeof *grown);
    if (!grown)
        return false;
    view->samplers = grown;
    view->samplers[view->n_samplers][0] = p[0];
    view->samplers[view->n_samplers][1] = p[1];
    view->n_samplers++;
    return true;
}
bool pc_ui_cr_scope_remove_sampler(pc_ui_camera_raw_scope *view, size_t i)
{
    if (!view || i >= view->n_samplers)
        return false;
    for (size_t j = i + 1; j < view->n_samplers; j++) {
        view->samplers[j - 1][0] = view->samplers[j][0];
        view->samplers[j - 1][1] = view->samplers[j][1];
    }
    view->n_samplers--;
    return true;
}
void pc_ui_cr_scope_clear_samplers(pc_ui_camera_raw_scope *view)
{
    if (view)
        view->n_samplers = 0;
}
bool pc_ui_cr_scope_move_sampler(pc_ui_camera_raw_scope *view, size_t i, const float p[2])
{
    if (!view || i >= view->n_samplers || !pc_ui_cr_point_valid(p))
        return false;
    view->samplers[i][0] = p[0];
    view->samplers[i][1] = p[1];
    return true;
}
bool pc_ui_cr_floating_rect_valid(const float r[4])
{
    if (!r)
        return false;
    for (int i = 0; i < 4; i++)
        if (!isfinite(r[i]))
            return false;
    float w = r[2] - r[0], h = r[3] - r[1];
    return w >= 200.0f && w <= 4096.0f && h >= 150.0f && h <= 4096.0f;
}

/* ---- preview navigation ---- */
static void viewport_center(const float viewport[4], float out[2])
{
    out[0] = viewport[0] + viewport[2] * 0.5f;
    out[1] = viewport[1] + viewport[3] * 0.5f;
}
float pc_ui_cr_preview_scale(const pc_ui_camera_raw_preview *p, const float viewport[4],
                             const float size[2])
{
    if (p && p->has_zoom && isfinite(p->zoom) && p->zoom > 0.0f)
        return clampf(p->zoom, PC_UI_CR_MIN_ZOOM, PC_UI_CR_MAX_ZOOM);
    float sx = size[0] > FLT_MIN ? size[0] : FLT_MIN, sy = size[1] > FLT_MIN ? size[1] : FLT_MIN;
    float vw = viewport[2] > 1.0f ? viewport[2] : 1.0f, vh = viewport[3] > 1.0f ? viewport[3] : 1.0f;
    float fit = fminf(vw / sx, vh / sy);
    return fminf(fit, PC_UI_CR_MAX_ZOOM);
}
void pc_ui_cr_preview_image_rect(pc_ui_camera_raw_preview *p, const float viewport[4], const float size[2],
                                 float out[4])
{
    float scale = pc_ui_cr_preview_scale(p, viewport, size);
    float ex = fmaxf(size[0], FLT_MIN) * scale, ey = fmaxf(size[1], FLT_MIN) * scale;
    float image[2] = {ex, ey}, visible[2] = {viewport[2], viewport[3]};
    if (p) {
        for (size_t i = 0; i < 2; i++) {
            float margin = fminf((fmaxf(visible[i], 1.0f) / fmaxf(image[i], 1.0f)) * 0.5f, 0.5f);
            p->center[i] = isfinite(p->center[i]) ? clampf(p->center[i], margin, 1.0f - margin) : 0.5f;
        }
    }
    float vc[2];
    viewport_center(viewport, vc);
    float c0 = p ? p->center[0] : 0.5f, c1 = p ? p->center[1] : 0.5f;
    out[0] = vc[0] - c0 * ex;
    out[1] = vc[1] - c1 * ey;
    out[2] = ex;
    out[3] = ey;
}
void pc_ui_cr_preview_zoom_at(pc_ui_camera_raw_preview *p, float zoom, const float at[2],
                              const float viewport[4], const float size[2])
{
    if (!p || !isfinite(zoom) || !at || !isfinite(at[0]) || !isfinite(at[1]))
        return;
    float old[4];
    pc_ui_cr_preview_image_rect(p, viewport, size, old);
    float pw = old[2] > FLT_MIN ? old[2] : FLT_MIN, ph = old[3] > FLT_MIN ? old[3] : FLT_MIN;
    float point[2] = {(at[0] - old[0]) / pw, (at[1] - old[1]) / ph};
    p->has_zoom = true;
    p->zoom = clampf(zoom, PC_UI_CR_MIN_ZOOM, PC_UI_CR_MAX_ZOOM);
    float scale = pc_ui_cr_preview_scale(p, viewport, size);
    float ex = fmaxf(size[0], FLT_MIN) * scale, ey = fmaxf(size[1], FLT_MIN) * scale;
    float vc[2];
    viewport_center(viewport, vc);
    p->center[0] = point[0] - (at[0] - vc[0]) / ex;
    p->center[1] = point[1] - (at[1] - vc[1]) / ey;
    float rect[4];
    pc_ui_cr_preview_image_rect(p, viewport, size, rect);
}
void pc_ui_cr_preview_pan(pc_ui_camera_raw_preview *p, const float delta[2], const float viewport[4],
                          const float size[2])
{
    if (!p || !delta || !isfinite(delta[0]) || !isfinite(delta[1]))
        return;
    float rect[4];
    pc_ui_cr_preview_image_rect(p, viewport, size, rect);
    float rw = rect[2] != 0.0f ? rect[2] : 1.0f, rh = rect[3] != 0.0f ? rect[3] : 1.0f;
    p->center[0] -= delta[0] / rw;
    p->center[1] -= delta[1] / rh;
    pc_ui_cr_preview_image_rect(p, viewport, size, rect);
}
void pc_ui_cr_preview_fit(pc_ui_camera_raw_preview *p)
{
    if (!p)
        return;
    p->has_zoom = false;
    p->zoom = 0.0f;
    p->center[0] = 0.5f;
    p->center[1] = 0.5f;
}
