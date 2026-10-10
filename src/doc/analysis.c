/* Image > Analysis data and Notes (port of crates/doc/src/analysis.rs). */
#include "doc/analysis.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

static bool color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

/* ---- Rust float formatting ---- */

/* Large enough for any f64 in plain decimal: 5e-324 is "0." + 323 zeros + "5", f64::MAX has 309
 * integer digits, and %.4f adds 5 characters. */
enum { NUM_TEXT_MAX = 400 };

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

/* Rust's `{}` (Display) for an f64: the shortest digits that parse back to v, never in exponent
 * form ("0.0000001", "1000000000000000000000"), "NaN", "inf", "-inf"; no ".0" on integers.
 *
 * Rust's flt2dec (Grisu with a Dragon4 fallback) picks the shortest length k, then of the two
 * k-digit neighbours of v the one inside the rounding interval, the closer when both are, and
 * on an exact tie the one ABOVE (ryu, and so serde's text, rounds such ties to even: Rust prints
 * 3578819939694.65625 as ...6563 where ryu gives ...6562). This reproduces that from glibc's
 * exact printf digits and its correctly rounded strtod. */
static size_t rust_display_f64(double v, char out[NUM_TEXT_MAX])
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

/* Rust's `{:.4}` for an f64. Rust prints the exact decimal value rounded half-to-even, as glibc's
 * printf does in the default rounding mode (checked against rustc 1.95 for ties such as 0.125 at
 * two places and 0.00025 at four); only the non-finite spellings differ. */
static size_t rust_fixed4_f64(double v, char out[NUM_TEXT_MAX])
{
    int n;
    if (isnan(v)) {
        memcpy(out, "NaN", 4);
        return 3;
    }
    if (isinf(v)) {
        if (v < 0) {
            memcpy(out, "-inf", 5);
            return 4;
        }
        memcpy(out, "inf", 4);
        return 3;
    }
    n = snprintf(out, NUM_TEXT_MAX, "%.4f", v);
    if (n < 0) {
        out[0] = '\0';
        return 0;
    }
    return (size_t)n < NUM_TEXT_MAX ? (size_t)n : NUM_TEXT_MAX - 1;
}

/* The reference's trim(): an integral value as `v as i64` (saturating), else Display. */
static size_t trim_f64(double v, char out[NUM_TEXT_MAX])
{
    double fract = v - trunc(v); /* f64::fract; NaN for infinities */
    if (fract == 0.0) {
        int n = snprintf(out, NUM_TEXT_MAX, "%" PRId64, pc_f64_to_i64(v));
        return n > 0 ? (size_t)n : 0;
    }
    return rust_display_f64(v, out);
}

/* ---- measurement scale ---- */

pc_status pc_measurement_scale_default(pc_measurement_scale *out, pc_error *err)
{
    pc_measurement_scale t;
    pc_status st;
    memset(&t, 0, sizeof t);
    t.pixel_length = 1.0;
    t.logical_length = 1.0;
    st = pc_str_set_c(&t.units, "pixels", err);
    if (st != PC_OK) {
        return st;
    }
    *out = t;
    return PC_OK;
}

void pc_measurement_scale_fini(pc_measurement_scale *s)
{
    if (s == NULL) {
        return;
    }
    pc_str_fini(&s->units);
    memset(s, 0, sizeof *s);
}

pc_status pc_measurement_scale_clone(const pc_measurement_scale *s, pc_measurement_scale *out, pc_error *err)
{
    pc_measurement_scale t = *s;
    pc_status st = pc_str_clone(&s->units, &t.units, err);
    if (st != PC_OK) {
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_measurement_scale_eq(const pc_measurement_scale *a, const pc_measurement_scale *b)
{
    return a->pixel_length == b->pixel_length && a->logical_length == b->logical_length &&
           pc_str_eq(&a->units, &b->units);
}

double pc_measurement_scale_factor(const pc_measurement_scale *s)
{
    return s->pixel_length > 0.0 ? s->logical_length / s->pixel_length : 1.0;
}

bool pc_measurement_scale_is_default(const pc_measurement_scale *s)
{
    return s->pixel_length == 1.0 && s->logical_length == 1.0 && pc_str_eq_c(&s->units, "pixels");
}

pc_status pc_measurement_scale_describe(const pc_measurement_scale *s, pc_str *out, pc_error *err)
{
    char a[NUM_TEXT_MAX];
    char b[NUM_TEXT_MAX];
    size_t na = trim_f64(s->pixel_length, a);
    size_t nb = rust_fixed4_f64(s->logical_length, b);
    pc_str t = PC_STR_INIT;
    pc_status st = pc_str_append(&t, a, na, err);
    if (st == PC_OK) {
        st = pc_str_append(&t, " pixels = ", 10, err);
    }
    if (st == PC_OK) {
        st = pc_str_append(&t, b, nb, err);
    }
    if (st == PC_OK) {
        st = pc_str_append(&t, " ", 1, err);
    }
    if (st == PC_OK) {
        st = pc_str_append(&t, s->units.ptr, s->units.len, err);
    }
    if (st != PC_OK) {
        pc_str_fini(&t);
        return st;
    }
    *out = t;
    return PC_OK;
}

/* ---- count groups ---- */

pc_status pc_count_group_default(pc_count_group *out, pc_error *err)
{
    pc_count_group t;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_str_set_c(&t.name, "Count Group 1", err);
    if (st != PC_OK) {
        return st;
    }
    t.color = pc_color_rgb(1.0f, 0.0f, 0.0f);
    t.marker_size = 1;
    t.label_size = 8;
    t.visible = true;
    *out = t;
    return PC_OK;
}

void pc_count_group_fini(pc_count_group *g)
{
    if (g == NULL) {
        return;
    }
    pc_str_fini(&g->name);
    free(g->points);
    memset(g, 0, sizeof *g);
}

pc_status pc_count_group_clone(const pc_count_group *g, pc_count_group *out, pc_error *err)
{
    pc_count_group t = *g;
    pc_status st;
    t.points = NULL;
    if (g->npoints > 0) {
        t.points = pc_malloc_array(g->npoints, sizeof *t.points);
        if (t.points == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(t.points, g->points, g->npoints * sizeof *t.points);
    }
    st = pc_str_clone(&g->name, &t.name, err);
    if (st != PC_OK) {
        free(t.points);
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_count_group_eq(const pc_count_group *a, const pc_count_group *b)
{
    size_t i;
    if (!pc_str_eq(&a->name, &b->name) || !color_eq(&a->color, &b->color) || a->marker_size != b->marker_size ||
        a->label_size != b->label_size || a->visible != b->visible || a->npoints != b->npoints) {
        return false;
    }
    for (i = 0; i < a->npoints; i++) {
        if (a->points[i][0] != b->points[i][0] || a->points[i][1] != b->points[i][1]) {
            return false;
        }
    }
    return true;
}

bool pc_ruler_eq(const pc_ruler *a, const pc_ruler *b)
{
    if (a->start[0] != b->start[0] || a->start[1] != b->start[1] || a->end[0] != b->end[0] ||
        a->end[1] != b->end[1] || a->has_protractor != b->has_protractor) {
        return false;
    }
    return !a->has_protractor || (a->protractor[0] == b->protractor[0] && a->protractor[1] == b->protractor[1]);
}

/* ---- measurement ---- */

pc_status pc_measurement_default(pc_measurement *out, pc_error *err)
{
    pc_measurement t;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_measurement_scale_default(&t.scale, err);
    if (st != PC_OK) {
        return st;
    }
    *out = t;
    return PC_OK;
}

void pc_measurement_fini(pc_measurement *m)
{
    size_t i;
    if (m == NULL) {
        return;
    }
    pc_measurement_scale_fini(&m->scale);
    for (i = 0; i < m->ncount_groups; i++) {
        pc_count_group_fini(&m->count_groups[i]);
    }
    free(m->count_groups);
    memset(m, 0, sizeof *m);
}

pc_status pc_measurement_clone(const pc_measurement *m, pc_measurement *out, pc_error *err)
{
    pc_measurement t = *m;
    size_t i;
    pc_status st;
    t.count_groups = NULL;
    t.ncount_groups = 0;
    st = pc_measurement_scale_clone(&m->scale, &t.scale, err);
    if (st != PC_OK) {
        return st;
    }
    if (m->ncount_groups > 0) {
        t.count_groups = pc_calloc_array(m->ncount_groups, sizeof *t.count_groups);
        if (t.count_groups == NULL) {
            pc_measurement_scale_fini(&t.scale);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < m->ncount_groups; i++) {
            st = pc_count_group_clone(&m->count_groups[i], &t.count_groups[i], err);
            if (st != PC_OK) {
                pc_measurement_fini(&t); /* ncount_groups counts the clones made so far */
                return st;
            }
            t.ncount_groups = i + 1;
        }
    }
    *out = t;
    return PC_OK;
}

bool pc_measurement_eq(const pc_measurement *a, const pc_measurement *b)
{
    size_t i;
    if (!pc_measurement_scale_eq(&a->scale, &b->scale) || a->ncount_groups != b->ncount_groups ||
        a->active_count_group != b->active_count_group || a->has_ruler != b->has_ruler) {
        return false;
    }
    for (i = 0; i < a->ncount_groups; i++) {
        if (!pc_count_group_eq(&a->count_groups[i], &b->count_groups[i])) {
            return false;
        }
    }
    return !a->has_ruler || pc_ruler_eq(&a->ruler, &b->ruler);
}

bool pc_measurement_is_empty(const pc_measurement *m)
{
    /* == Measurement::default(), compared field by field so it needs no allocation. */
    return pc_measurement_scale_is_default(&m->scale) && m->ncount_groups == 0 && m->active_count_group == 0 &&
           !m->has_ruler;
}

size_t pc_measurement_count_total(const pc_measurement *m)
{
    size_t total = 0;
    size_t i;
    for (i = 0; i < m->ncount_groups; i++) {
        total += m->count_groups[i].npoints;
    }
    return total;
}

/* ---- notes ---- */

void pc_note_default(pc_note *out)
{
    memset(out, 0, sizeof *out);
    out->color = pc_color_rgb(1.0f, 1.0f, 0.51f);
    out->open = false;
}

void pc_note_fini(pc_note *n)
{
    if (n == NULL) {
        return;
    }
    pc_str_fini(&n->author);
    pc_str_fini(&n->text);
    pc_str_fini(&n->modified);
    memset(n, 0, sizeof *n);
}

pc_status pc_note_clone(const pc_note *n, pc_note *out, pc_error *err)
{
    pc_note t = *n;
    pc_status st;
    t.author = (pc_str)PC_STR_INIT;
    t.text = (pc_str)PC_STR_INIT;
    t.modified = (pc_str)PC_STR_INIT;
    st = pc_str_clone(&n->author, &t.author, err);
    if (st == PC_OK) {
        st = pc_str_clone(&n->text, &t.text, err);
    }
    if (st == PC_OK) {
        st = pc_str_clone(&n->modified, &t.modified, err);
    }
    if (st != PC_OK) {
        pc_str_fini(&t.author);
        pc_str_fini(&t.text);
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_note_eq(const pc_note *a, const pc_note *b)
{
    return pc_str_eq(&a->author, &b->author) && color_eq(&a->color, &b->color) && pc_str_eq(&a->text, &b->text) &&
           a->position[0] == b->position[0] && a->position[1] == b->position[1] && a->popup[0] == b->popup[0] &&
           a->popup[1] == b->popup[1] && a->popup[2] == b->popup[2] && a->popup[3] == b->popup[3] &&
           a->open == b->open && pc_str_eq(&a->modified, &b->modified);
}
