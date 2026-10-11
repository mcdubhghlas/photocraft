#include "rfmt.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Digits of |v| in scientific form: exact[0] is the leading digit, exact[1..] the following
 * ones (EXACT_DIGITS in all, enough for the exact value of any double, at most 767 significant
 * digits); *exp10 is the exponent of the leading digit. */
enum { EXACT_DIGITS = 800 };

static bool exact_digits(double v, char digits[EXACT_DIGITS], int *exp10)
{
    char buf[EXACT_DIGITS + 16];
    int n = snprintf(buf, sizeof buf, "%.*e", EXACT_DIGITS - 1, fabs(v));
    const char *e;
    if (n <= 0 || (size_t)n >= sizeof buf || buf[1] != '.') {
        return false;
    }
    e = strchr(buf, 'e');
    if (e == NULL || e - buf != EXACT_DIGITS + 1) {
        return false;
    }
    digits[0] = buf[0];
    memcpy(digits + 1, buf + 2, EXACT_DIGITS - 1);
    *exp10 = (int)strtol(e + 1, NULL, 10);
    return true;
}

/* Whether the k-digit decimal d[0..k) x 10^(exp10 - k + 1) parses back to exactly |v|. */
static bool round_trips(const char *d, size_t k, int exp10, double av)
{
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.*se%d", (int)k, d, exp10 - (int)k + 1);
    return n > 0 && (size_t)n < sizeof buf && strtod(buf, NULL) == av;
}

size_t pc_rust_display_f64(double v, char out[PC_RUST_F64_TEXT_MAX])
{
    char d[EXACT_DIGITS];
    char lo[24];
    char hi[24];
    const char *best = NULL;
    int e = 0;
    int ehi;
    int ebest = 0;
    size_t k;
    size_t nb = 0;
    size_t o = 0;
    size_t i;
    double av = fabs(v);

    if (isnan(v)) {
        memcpy(out, "NaN", 4);
        return 3;
    }
    if (isinf(v)) {
        memcpy(out, v < 0 ? "-inf" : "inf", v < 0 ? 5 : 4);
        return v < 0 ? 4 : 3;
    }
    if (signbit(v)) {
        out[o++] = '-';
    }
    if (av == 0.0 || !exact_digits(v, d, &e)) {
        out[o++] = '0';
        out[o] = '\0';
        return o;
    }
    for (k = 1; k <= 17 && best == NULL; k++) {
        bool lo_ok;
        bool hi_ok;
        size_t j;
        memcpy(lo, d, k);
        lo[k] = '\0';
        /* The k-digit neighbour above: lo plus one unit in the last place, with carry. */
        memcpy(hi, d, k);
        hi[k] = '\0';
        ehi = e;
        for (j = k; j > 0 && hi[j - 1] == '9'; j--) {
            hi[j - 1] = '0';
        }
        if (j == 0) {
            hi[0] = '1'; /* 99..9 + 1 = 100..0: one more power of ten, same digit count */
            ehi = e + 1;
        } else {
            hi[j - 1] = (char)(hi[j - 1] + 1);
        }
        lo_ok = round_trips(lo, k, e, av);
        hi_ok = round_trips(hi, k, ehi, av);
        if (lo_ok && hi_ok) {
            /* The closer one; remainder >= half (including an exact tie) rounds up. */
            bool up = d[k] >= '5';
            best = up ? hi : lo;
            ebest = up ? ehi : e;
        } else if (lo_ok) {
            best = lo;
            ebest = e;
        } else if (hi_ok) {
            best = hi;
            ebest = ehi;
        }
        nb = k;
    }
    if (best == NULL) {
        /* Unreachable: 17 significant digits always round-trip. */
        best = d;
        nb = 17;
        ebest = e;
    }
    while (nb > 1 && best[nb - 1] == '0') {
        nb--;
    }
    /* value = 0.best[0..nb) x 10^(ebest + 1) */
    {
        long p = (long)ebest + 1;
        if (p <= 0) {
            out[o++] = '0';
            out[o++] = '.';
            for (; p < 0; p++) {
                out[o++] = '0';
            }
            for (i = 0; i < nb; i++) {
                out[o++] = best[i];
            }
        } else {
            long q = 0;
            for (i = 0; i < nb; i++, q++) {
                if (q == p) {
                    out[o++] = '.';
                }
                out[o++] = best[i];
            }
            for (; q < p; q++) {
                out[o++] = '0';
            }
        }
    }
    out[o] = '\0';
    return o;
}
