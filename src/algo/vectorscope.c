/* Hue/saturation distribution (port of crates/algo/src/vectorscope.rs). */
#include "algo/vectorscope.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/num.h"
#include "base/rmath.h"

void pc_algo_vectorscope_hue_saturation(const float rgb[3], float *hue, float *saturation)
{
    float r = rgb[0], g = rgb[1], b = rgb[2];
    float high = pc_rs_maxf(pc_rs_maxf(r, g), b);
    float low = pc_rs_minf(pc_rs_minf(r, g), b);
    float delta = high - low, h;
    if (delta <= FLT_EPSILON || high <= 0.0f) {
        *hue = 0.0f;
        *saturation = 0.0f;
        return;
    }
    if (high == r) {
        h = (g - b) / delta;
    } else if (high == g) {
        h = 2.0f + (b - r) / delta;
    } else {
        h = 4.0f + (r - g) / delta;
    }
    *hue = pc_algo_rem_euclidf(h / 6.0f, 1.0f);
    *saturation = delta / high;
}

void pc_algo_vectorscope_from_rgba(const float (*px)[4], size_t n, pc_algo_vectorscope *out)
{
    size_t i;
    memset(out, 0, sizeof *out);
    for (i = 0; i < n; i++) {
        const float *p = px[i];
        float rgb[3], h, s;
        size_t hue, sat, k;
        if (p[3] <= 0.0f || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]) || !isfinite(p[3])) {
            continue;
        }
        for (k = 0; k < 3; k++) {
            rgb[k] = pc_clampf(p[k], 0.0f, 1.0f);
        }
        pc_algo_vectorscope_hue_saturation(rgb, &h, &s);
        hue = pc_f32_to_size(h * (float)PC_ALGO_VECTORSCOPE_HUES);
        if (hue > PC_ALGO_VECTORSCOPE_HUES - 1) {
            hue = PC_ALGO_VECTORSCOPE_HUES - 1;
        }
        sat = pc_f32_to_size(roundf(s * (float)(PC_ALGO_VECTORSCOPE_SATURATIONS - 1)));
        if (sat > PC_ALGO_VECTORSCOPE_SATURATIONS - 1) {
            sat = PC_ALGO_VECTORSCOPE_SATURATIONS - 1;
        }
        k = hue * PC_ALGO_VECTORSCOPE_SATURATIONS + sat;
        if (out->counts[k] < UINT32_MAX) {
            out->counts[k]++;
        }
        if (out->samples < UINT64_MAX) {
            out->samples++;
        }
    }
}
