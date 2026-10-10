/* Image stack statistics (port of crates/algo/src/stack.rs); see stack.h. */
#include "algo/stack.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

static const char *const NAMES[PC_ALGO_STACK_STAT_COUNT] = {
    "Entropy", "Kurtosis", "Maximum",           "Mean",      "Median",  "Minimum",
    "Range",   "Skewness", "StandardDeviation", "Summation", "Variance"};

const char *pc_algo_stack_stat_name(pc_algo_stack_stat s)
{
    return (unsigned)s < PC_ALGO_STACK_STAT_COUNT ? NAMES[s] : NULL;
}

bool pc_algo_stack_stat_from_name(const char *s, size_t len, pc_algo_stack_stat *out)
{
    size_t i;
    for (i = 0; s != NULL && i < PC_ALGO_STACK_STAT_COUNT; i++) {
        if (strlen(NAMES[i]) == len && memcmp(NAMES[i], s, len) == 0) {
            *out = (pc_algo_stack_stat)i;
            return true;
        }
    }
    return false;
}

/* f32::total_cmp. */
static int cmp_total(const void *a, const void *b)
{
    uint32_t x, y;
    memcpy(&x, a, 4);
    memcpy(&y, b, 4);
    x = (x & 0x80000000u) ? ~x : (x | 0x80000000u);
    y = (y & 0x80000000u) ? ~y : (y | 0x80000000u);
    return x < y ? -1 : (x > y ? 1 : 0);
}

static float sum(const float *v, size_t n)
{
    float s = -0.0f; /* Iterator::sum starts at -0.0 */
    size_t i;
    for (i = 0; i < n; i++) {
        s += v[i];
    }
    return s;
}

/* Central moment k (2, 3 or 4); powi(k) with a constant k is repeated squaring. */
static float moment(const float *v, size_t n, float mean, int k)
{
    float s = -0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        float d = v[i] - mean, d2 = d * d;
        s += k == 2 ? d2 : (k == 3 ? d2 * d : d2 * d2);
    }
    return s / (float)n;
}

static float fold_min(const float *v, size_t n)
{
    float m = FLT_MAX;
    size_t i;
    for (i = 0; i < n; i++) {
        m = pc_rs_minf(m, v[i]);
    }
    return m;
}

static float fold_max(const float *v, size_t n)
{
    float m = -FLT_MAX;
    size_t i;
    for (i = 0; i < n; i++) {
        m = pc_rs_maxf(m, v[i]);
    }
    return m;
}

float pc_algo_stack_stat_of(float *v, size_t n, pc_algo_stack_stat s)
{
    float nf = (float)n, mean;
    if (n == 0) {
        return 0.0f;
    }
    mean = sum(v, n) / nf;
    switch (s) {
    case PC_ALGO_STACK_MEAN:
        return mean;
    case PC_ALGO_STACK_SUMMATION:
        return sum(v, n);
    case PC_ALGO_STACK_MINIMUM:
        return fold_min(v, n);
    case PC_ALGO_STACK_MAXIMUM:
        return fold_max(v, n);
    case PC_ALGO_STACK_RANGE:
        return fold_max(v, n) - fold_min(v, n);
    case PC_ALGO_STACK_MEDIAN:
        qsort(v, n, sizeof *v, cmp_total);
        return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
    case PC_ALGO_STACK_VARIANCE:
        return moment(v, n, mean, 2);
    case PC_ALGO_STACK_STANDARD_DEVIATION:
        return sqrtf(moment(v, n, mean, 2));
    case PC_ALGO_STACK_SKEWNESS: {
        /* Map -2..2 to 0..1 (0.5 = symmetric). */
        float sd = sqrtf(moment(v, n, mean, 2));
        return sd < 1e-6f ? 0.5f
                          : pc_clampf(moment(v, n, mean, 3) / (sd * sd * sd) / 4.0f + 0.5f, 0.0f, 1.0f);
    }
    case PC_ALGO_STACK_KURTOSIS: {
        /* 0..10 -> 0..1. */
        float var = moment(v, n, mean, 2);
        return var < 1e-12f ? 0.0f : pc_clampf(moment(v, n, mean, 4) / (var * var) / 10.0f, 0.0f, 1.0f);
    }
    case PC_ALGO_STACK_ENTROPY: {
        /* The reference sums a HashMap's counts in its (per-process random) iteration order; this
         * sums in ascending byte order (plan section 7). */
        uint32_t counts[256];
        float h = -0.0f;
        size_t i;
        if (n < 2) {
            return 0.0f;
        }
        memset(counts, 0, sizeof counts);
        for (i = 0; i < n; i++) {
            counts[pc_f32_to_u8(roundf(pc_clampf(v[i], 0.0f, 1.0f) * 255.0f))]++;
        }
        for (i = 0; i < 256; i++) {
            if (counts[i] > 0) {
                float p = (float)counts[i] / nf;
                h += -p * log2f(p);
            }
        }
        return h / log2f(nf);
    }
    }
    return 0.0f;
}

pc_status pc_algo_stack_combine(const float (*const *layers)[4], const size_t *lens, size_t count,
                                pc_algo_stack_stat s, float (**out)[4], size_t *out_len, pc_error *err)
{
    size_t len = 0, i, k, c;
    float (*o)[4];
    float *samples;
    if (count > 0 && (layers == NULL || lens == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "missing layers");
    }
    if ((unsigned)s >= PC_ALGO_STACK_STAT_COUNT) {
        return pc_fail(err, PC_ERR_INVALID, "unknown stack statistic");
    }
    for (k = 0; k < count; k++) {
        if (layers[k] == NULL && lens[k] > 0) {
            return pc_fail(err, PC_ERR_INVALID, "missing layer %zu", k);
        }
        len = lens[k] > len ? lens[k] : len;
    }
    o = pc_calloc_array(len, sizeof *o);
    samples = pc_malloc_array(count, 3 * sizeof(float));
    if (o == NULL || samples == NULL) {
        free(o);
        free(samples);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < len; i++) {
        float alpha = 0.0f;
        size_t n = 0;
        for (k = 0; k < count; k++) {
            if (i < lens[k] && layers[k][i][3] > 0.0f) {
                for (c = 0; c < 3; c++) {
                    samples[c * count + n] = layers[k][i][c];
                }
                alpha = pc_rs_maxf(alpha, layers[k][i][3]);
                n++;
            }
        }
        if (alpha <= 0.0f) {
            continue;
        }
        for (c = 0; c < 3; c++) {
            o[i][c] = pc_clampf(pc_algo_stack_stat_of(&samples[c * count], n, s), 0.0f, 1.0f);
        }
        o[i][3] = alpha;
    }
    free(samples);
    *out = o;
    *out_len = len;
    return PC_OK;
}
