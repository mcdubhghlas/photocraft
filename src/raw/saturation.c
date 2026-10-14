/* Sensors that clip below the stated white level (port of crates/raw/src/saturation.rs).
 *
 * A DNG's WhiteLevel is what the writer says the samples top out at, not always where the sensor
 * really saturates: a camera app can state 4095 for 12-bit data that piles up at 3567 (85 % of the
 * range above black). Development clips each channel at 1 after white balance, so samples stuck 15 %
 * below the white level are not clipped together: red and blue (multiplied by more than green) reach
 * 1, green stays at 0.85, and blown highlights turn magenta.
 *
 * rx_sensor_saturation_points finds the real saturation point of each colour from the samples (a
 * pile-up of identical values near the top of the range); rx_sensor_lift_clipped_samples raises the
 * samples at or above it to the stated white level. Only CFA data without a linearization table is
 * examined. A file whose data reaches its white level, or has no pile-up, is left alone. */
#include <math.h>
#include <stdlib.h>

#include "base/mem.h"
#include "base/num.h"
#include "raw/internal.h"

/* A saturation plateau must hold at least this share of its colour's samples (and 100 of them), as
 * many must lie below it (a scene, not a flat field)... */
#define RX_SAT_MIN_SHARE 1e-4
/* ...be at least this many times as populated as the average value just below it... */
#define RX_SAT_MIN_SPIKE 16.0
/* ...and have next to nothing above it: at most this share of the plateau lies more than
 * RX_SAT_ABOVE_MARGIN of the range over it (noise spilling past the plateau). */
#define RX_SAT_MAX_ABOVE 0.02
#define RX_SAT_ABOVE_MARGIN 0.01f
/* Plateaus within this share of the range from the white level are the white level (no lift). */
#define RX_SAT_NEAR_WHITE 0.03f
/* How far below the plateau the "average value just below it" reaches. */
#define RX_SAT_NEIGHBOURHOOD 64

/* start + len (checked) <= max. */
static bool rx_sat_fits(size_t start, size_t len, size_t max)
{
    return len <= max - start ? start + len <= max : false;
}

/* The value each colour's samples saturate at, when that is clearly below the stated white level;
 * out[c] gets the point (and *have_c true) only when found. Only CFA data without a linearization
 * table is examined: with a table the stored values are not on the white level's scale. */
static void rx_sensor_saturation_points(const pc_raw_sensor *s, uint16_t out[3], bool have[3])
{
    const pc_raw_cfa *cfa = &s->cfa;
    float white;
    float black = 0.0f;
    pc_raw_rect a = s->active;
    size_t lo;
    size_t hi;
    uint32_t (*hist)[3];
    uint64_t total[3] = {0, 0, 0};
    size_t y;
    int c;

    have[0] = have[1] = have[2] = false;
    out[0] = out[1] = out[2] = 0;

    if (!s->has_cfa) {
        return;
    }
    /* samples != 1, a linearization table, or a data length that is not width * height */
    if (s->samples != 1 || s->nlinearization > 0) {
        return;
    }
    {
        size_t wh;
        if (s->height != 0 && s->width > SIZE_MAX / s->height) {
            return; /* width * height overflows: cannot equal ndata */
        }
        wh = s->width * s->height;
        if (wh != s->ndata) {
            return;
        }
    }

    white = s->white[0];
    if (s->black.nvalues > 0) {
        float sum = 0.0f;
        size_t i;
        for (i = 0; i < s->black.nvalues; i++) {
            sum += s->black.values[i];
        }
        black = sum / (float)s->black.nvalues;
    }
    /* a hostile file can state any levels: NaN, negative or inverted ones are left alone */
    if (!(isfinite(black) && isfinite(white) && black >= 0.0f && white > black && white <= 65535.0f)) {
        return;
    }
    if (!rx_sat_fits(a.x, a.width, s->width) || !rx_sat_fits(a.y, a.height, s->height)) {
        return;
    }
    lo = pc_f32_to_size(ceilf(black + 0.5f * (white - black)));
    hi = pc_f32_to_size(white);
    if (lo + RX_SAT_NEIGHBOURHOOD >= hi) {
        return;
    }

    hist = pc_calloc_array(65536, sizeof *hist);
    if (hist == NULL) {
        return;
    }
    for (y = a.y; y < a.y + a.height; y++) {
        const uint16_t *row = &s->data[y * s->width + a.x];
        size_t x;
        for (x = 0; x < a.width; x++) {
            uint16_t v = row[x];
            unsigned col = pc_raw_cfa_color(cfa, a.x + x, y);
            if (col > 2) {
                col = 2;
            }
            total[col]++;
            hist[v][col]++;
        }
    }

    for (c = 0; c < 3; c++) {
        /* the most populated value of the upper half of the range (ties: the higher value) */
        size_t v;
        uint32_t n = 0;
        bool found = false;
        double min_n;
        uint64_t darker = 0;
        uint64_t below = 0;
        uint64_t above = 0;
        size_t from;
        size_t best_v = 0;
        uint32_t best_n = 0;
        for (v = lo; v < hi; v++) {
            uint32_t hv = hist[v][c];
            if (!found || hv >= best_n) {
                best_n = hv;
                best_v = v;
                found = true;
            }
        }
        if (!found) {
            continue;
        }
        v = best_v;
        n = best_n;
        min_n = (double)total[c] * RX_SAT_MIN_SHARE;
        if (min_n < 100.0) {
            min_n = 100.0;
        }
        if ((double)n < min_n || (float)v >= white - RX_SAT_NEAR_WHITE * (white - black)) {
            continue;
        }
        /* a colour whose samples all sit on one value is a flat field, not a scene that clipped */
        {
            size_t u;
            for (u = 0; u < v; u++) {
                darker += hist[u][c];
            }
        }
        if ((double)darker < min_n) {
            continue;
        }
        {
            size_t start = v > RX_SAT_NEIGHBOURHOOD ? v - RX_SAT_NEIGHBOURHOOD : 0;
            size_t u;
            for (u = start; u < v; u++) {
                below += hist[u][c];
            }
        }
        {
            size_t margin = pc_f32_to_size(RX_SAT_ABOVE_MARGIN * (white - black));
            if (margin < 1) {
                margin = 1;
            }
            from = v + margin + 1;
            if (from > 65536) {
                from = 65536;
            }
        }
        {
            size_t u;
            for (u = from; u < 65536; u++) {
                above += hist[u][c];
            }
        }
        {
            double below_avg = (double)below / (double)RX_SAT_NEIGHBOURHOOD;
            if (below_avg < 1.0) {
                below_avg = 1.0;
            }
            if ((double)n >= RX_SAT_MIN_SPIKE * below_avg && (double)above <= RX_SAT_MAX_ABOVE * (double)n) {
                out[c] = (uint16_t)v;
                have[c] = true;
            }
        }
    }

    free(hist);
}

size_t rx_sensor_lift_clipped_samples(pc_raw_sensor *s)
{
    uint16_t points[3];
    bool have[3];
    pc_raw_rect a;
    uint16_t white;
    size_t changed = 0;
    size_t y;

    rx_sensor_saturation_points(s, points, have);
    if (!have[0] && !have[1] && !have[2]) {
        return 0;
    }
    if (!s->has_cfa) {
        return 0;
    }
    a = s->active;
    /* the white level itself, rounded up: (white - black) / (white - black) is exactly 1 */
    white = pc_f32_to_u16(ceilf(s->white[0]));
    for (y = a.y; y < a.y + a.height; y++) {
        uint16_t *row = &s->data[y * s->width + a.x];
        size_t x;
        for (x = 0; x < a.width; x++) {
            unsigned col = pc_raw_cfa_color(&s->cfa, a.x + x, y);
            if (col > 2) {
                col = 2;
            }
            if (have[col] && row[x] >= points[col] && row[x] < white) {
                row[x] = white;
                changed++;
            }
        }
    }
    return changed;
}
