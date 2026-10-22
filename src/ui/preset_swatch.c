#include "preset_swatch.h"
#include <string.h>

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool pc_ui_preset_stop_color(const char *token, const float fg[4], const float bg[4], float out_rgb[3])
{
    if (!token || !out_rgb) {
        return false;
    }
    if (strcmp(token, "foreground") == 0) {
        if (!fg) {
            return false;
        }
        out_rgb[0] = fg[0];
        out_rgb[1] = fg[1];
        out_rgb[2] = fg[2];
        return true;
    }
    if (strcmp(token, "background") == 0) {
        if (!bg) {
            return false;
        }
        out_rgb[0] = bg[0];
        out_rgb[1] = bg[1];
        out_rgb[2] = bg[2];
        return true;
    }
    const char *hex = token[0] == '#' ? token + 1 : token;
    if (strlen(hex) != 6) {
        return false;
    }
    int v[6];
    for (size_t i = 0; i < 6; i++) {
        v[i] = hex_nibble(hex[i]);
        if (v[i] < 0) {
            return false;
        }
    }
    out_rgb[0] = (float)(v[0] * 16 + v[1]) / 255.0f;
    out_rgb[1] = (float)(v[2] * 16 + v[3]) / 255.0f;
    out_rgb[2] = (float)(v[4] * 16 + v[5]) / 255.0f;
    return true;
}

void pc_ui_preset_gradient_sample(const float *locs, const float (*rgb)[3], size_t n, float t,
                                  float out_rgb[3])
{
    if (!locs || !rgb || !out_rgb || n == 0) {
        return;
    }
    /* End clamps: below the lowest stop -> its colour, above the highest -> its
     * colour. Scanned, so the stop array need not be sorted. */
    size_t lo = 0, hi = 0;
    for (size_t i = 1; i < n; i++) {
        if (locs[i] < locs[lo]) {
            lo = i;
        }
        if (locs[i] > locs[hi]) {
            hi = i;
        }
    }
    if (t <= locs[lo]) {
        memcpy(out_rgb, rgb[lo], 3 * sizeof(float));
        return;
    }
    if (t >= locs[hi]) {
        memcpy(out_rgb, rgb[hi], 3 * sizeof(float));
        return;
    }
    /* Bracket t: prev = the largest location <= t (ties: later index), next = the
     * smallest location > t (ties: earlier index). locs[next] > t >= locs[prev]
     * so the span is strictly positive and the interpolation cannot divide by 0. */
    bool have_prev = false, have_next = false;
    size_t prev = 0, next = 0;
    for (size_t i = 0; i < n; i++) {
        if (locs[i] <= t) {
            if (!have_prev || locs[i] > locs[prev] || (locs[i] == locs[prev] && i > prev)) {
                prev = i;
                have_prev = true;
            }
        } else {
            if (!have_next || locs[i] < locs[next] || (locs[i] == locs[next] && i < next)) {
                next = i;
                have_next = true;
            }
        }
    }
    if (!have_prev || !have_next) {
        memcpy(out_rgb, rgb[have_prev ? prev : next], 3 * sizeof(float));
        return;
    }
    float d = locs[next] - locs[prev];
    float u = d > 0 ? (t - locs[prev]) / d : 1.0f;
    for (size_t c = 0; c < 3; c++) {
        out_rgb[c] = rgb[prev][c] + (rgb[next][c] - rgb[prev][c]) * u;
    }
}
