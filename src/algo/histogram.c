/* Bounded RGB histograms (port of crates/algo/src/histogram.rs). */
#include "algo/histogram.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "base/num.h"

static uint64_t sat_inc(uint64_t v)
{
    return v == UINT64_MAX ? v : v + 1;
}

static bool all_finite(const float p[4])
{
    return isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]) && isfinite(p[3]);
}

void pc_algo_clipping(const float p[4], uint8_t *shadows, uint8_t *highlights)
{
    unsigned s = 0, h = 0, c;
    if (!(p[3] > 0.0f) || !all_finite(p)) {
        *shadows = 0;
        *highlights = 0;
        return;
    }
    for (c = 0; c < 3; c++) {
        if (p[c] <= 0.0f) {
            s |= 1u << c;
        }
        if (p[c] >= 1.0f) {
            h |= 1u << c;
        }
    }
    *shadows = (uint8_t)s;
    *highlights = (uint8_t)h;
}

void pc_algo_rgb_histogram_from_rgba(const float (*pixels)[4], size_t n, pc_algo_rgb_histogram *out)
{
    size_t i;
    int c;
    memset(out, 0, sizeof *out);
    for (i = 0; i < n; i++) {
        const float *p = pixels[i];
        uint8_t low, high;
        if (!isfinite(p[3])) {
            out->invalid = sat_inc(out->invalid);
            continue;
        }
        if (p[3] <= 0.0f) {
            out->transparent = sat_inc(out->transparent);
            continue;
        }
        if (!all_finite(p)) {
            out->invalid = sat_inc(out->invalid);
            continue;
        }
        out->samples = sat_inc(out->samples);
        pc_algo_clipping(p, &low, &high);
        for (c = 0; c < 3; c++) {
            float v = p[c];
            size_t bin;
            if ((low & (1u << c)) != 0) {
                out->shadows[c] = sat_inc(out->shadows[c]);
            }
            if ((high & (1u << c)) != 0) {
                out->highlights[c] = sat_inc(out->highlights[c]);
            }
            if (v < 0.0f) {
                out->underflow[c] = sat_inc(out->underflow[c]);
            } else if (v > 1.0f) {
                out->overflow[c] = sat_inc(out->overflow[c]);
            }
            /* The preview encoder's nearest-bin rule, before it loses float precision. */
            bin = pc_f32_to_size(pc_clampf(v, 0.0f, 1.0f) * (float)(PC_ALGO_HIST_BINS - 1) + 0.5f);
            if (bin < PC_ALGO_HIST_BINS) {
                out->channels[c][bin] = sat_inc(out->channels[c][bin]);
            }
        }
    }
}
