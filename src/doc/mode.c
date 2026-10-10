/* Colour-mode side data (port of crates/doc/src/mode.rs). */
#include "doc/mode.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- color table ---- */

void pc_color_table_fini(pc_color_table *t)
{
    if (t == NULL) {
        return;
    }
    free(t->colors);
    memset(t, 0, sizeof *t);
}

pc_status pc_color_table_clone(const pc_color_table *t, pc_color_table *out, pc_error *err)
{
    pc_color_table c = *t;
    c.colors = NULL;
    if (t->ncolors > 0) {
        c.colors = pc_malloc_array(t->ncolors, sizeof *c.colors);
        if (c.colors == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(c.colors, t->colors, t->ncolors * sizeof *c.colors);
    }
    *out = c;
    return PC_OK;
}

bool pc_color_table_eq(const pc_color_table *a, const pc_color_table *b)
{
    if (a->ncolors != b->ncolors || a->has_transparent != b->has_transparent) {
        return false;
    }
    if (a->has_transparent && a->transparent != b->transparent) {
        return false;
    }
    return a->ncolors == 0 || memcmp(a->colors, b->colors, a->ncolors * sizeof *a->colors) == 0;
}

size_t pc_color_table_nearest(const pc_color_table *t, const float c[3])
{
    float best_d = FLT_MAX;
    size_t best = 0;
    size_t i;
    int k;
    for (i = 0; i < t->ncolors; i++) {
        /* Rust's float Iterator::sum starts from -0.0. */
        float d = -0.0f;
        for (k = 0; k < 3; k++) {
            float diff = c[k] - (float)t->colors[i][k] / 255.0f;
            d += diff * diff;
        }
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

void pc_color_table_rgb(const pc_color_table *t, size_t i, float out[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        out[k] = i < t->ncolors ? (float)t->colors[i][k] / 255.0f : 0.0f;
    }
}

/* ---- duotone ---- */

float pc_duotone_curve_eval(const pc_curve_point *pts, size_t n, float x)
{
    size_t i;
    if (n == 0) {
        return x;
    }
    if (n == 1) {
        return pts[0].output;
    }
    if (x <= pts[0].input) {
        return pts[0].output;
    }
    for (i = 0; i + 1 < n; i++) {
        const pc_curve_point *a = &pts[i];
        const pc_curve_point *b = &pts[i + 1];
        if (x <= b->input) {
            float span = pc_rs_maxf_k(b->input - a->input, 1e-6f);
            return a->output + (b->output - a->output) * (x - a->input) / span;
        }
    }
    return pts[n - 1].output;
}

pc_status pc_duotone_ink_new(const char *name, const float color[3], pc_duotone_ink *out, pc_error *err)
{
    pc_duotone_ink t;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_str_set_c(&t.name, name, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_curve_identity(&t.curve, err);
    if (st != PC_OK) {
        pc_str_fini(&t.name);
        return st;
    }
    t.color[0] = color[0];
    t.color[1] = color[1];
    t.color[2] = color[2];
    *out = t;
    return PC_OK;
}

void pc_duotone_ink_fini(pc_duotone_ink *i)
{
    if (i == NULL) {
        return;
    }
    pc_str_fini(&i->name);
    pc_curve_fini(&i->curve);
    memset(i, 0, sizeof *i);
}

pc_status pc_duotone_ink_clone(const pc_duotone_ink *i, pc_duotone_ink *out, pc_error *err)
{
    pc_duotone_ink t = *i;
    pc_status st = pc_str_clone(&i->name, &t.name, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_curve_clone(&i->curve, &t.curve, err);
    if (st != PC_OK) {
        pc_str_fini(&t.name);
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_duotone_ink_eq(const pc_duotone_ink *a, const pc_duotone_ink *b)
{
    return pc_str_eq(&a->name, &b->name) && a->color[0] == b->color[0] && a->color[1] == b->color[1] &&
           a->color[2] == b->color[2] && pc_curve_eq(&a->curve, &b->curve);
}

float pc_duotone_ink_density(const pc_duotone_ink *i, float gray_density)
{
    float v = pc_duotone_curve_eval(i->curve.pts, i->curve.len, pc_clampf(gray_density, 0.0f, 1.0f));
    return pc_clampf(v, 0.0f, 1.0f);
}

void pc_duotone_fini(pc_duotone *d)
{
    size_t i;
    if (d == NULL) {
        return;
    }
    for (i = 0; i < d->ninks; i++) {
        pc_duotone_ink_fini(&d->inks[i]);
    }
    free(d->inks);
    pc_blob_release(d->psd_raw);
    memset(d, 0, sizeof *d);
}

pc_status pc_duotone_clone(const pc_duotone *d, pc_duotone *out, pc_error *err)
{
    pc_duotone t;
    size_t i;
    memset(&t, 0, sizeof t);
    if (d->ninks > 0) {
        t.inks = pc_calloc_array(d->ninks, sizeof *t.inks);
        if (t.inks == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < d->ninks; i++) {
            pc_status st = pc_duotone_ink_clone(&d->inks[i], &t.inks[i], err);
            if (st != PC_OK) {
                pc_duotone_fini(&t); /* ninks counts the clones made so far */
                return st;
            }
            t.ninks = i + 1;
        }
    }
    t.psd_raw = pc_blob_retain(d->psd_raw);
    *out = t;
    return PC_OK;
}

bool pc_duotone_eq(const pc_duotone *a, const pc_duotone *b)
{
    size_t i;
    if (a->ninks != b->ninks) {
        return false;
    }
    for (i = 0; i < a->ninks; i++) {
        if (!pc_duotone_ink_eq(&a->inks[i], &b->inks[i])) {
            return false;
        }
    }
    return pc_blob_eq(a->psd_raw, b->psd_raw);
}

void pc_duotone_render(const pc_duotone *d, float gray, float out[3])
{
    float density = 1.0f - pc_clampf(gray, 0.0f, 1.0f);
    size_t i;
    int k;
    out[0] = out[1] = out[2] = 1.0f;
    for (i = 0; i < d->ninks; i++) {
        const pc_duotone_ink *ink = &d->inks[i];
        float v = pc_duotone_curve_eval(ink->curve.pts, ink->curve.len, density);
        for (k = 0; k < 3; k++) {
            /* Each ink filters light: full density transmits the ink colour. */
            out[k] *= 1.0f - v * (1.0f - ink->color[k]);
        }
    }
}

pc_status pc_duotone_display_adjustment(const pc_duotone *d, pc_adjustment *out, pc_error *err)
{
    enum { NSTOPS = 33 };
    pc_gradient_map_stop *stops = pc_malloc_array(NSTOPS, sizeof *stops);
    int i;
    if (stops == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < NSTOPS; i++) {
        float t = (float)i / 32.0f;
        stops[i].pos = t;
        pc_duotone_render(d, t, stops[i].rgb);
    }
    memset(out, 0, sizeof *out);
    out->kind = PC_ADJ_GRADIENT_MAP;
    out->u.gradient_map.stops = stops;
    out->u.gradient_map.nstops = NSTOPS;
    out->u.gradient_map.reverse = false;
    out->u.gradient_map.dither = false;
    return PC_OK;
}

/* ---- stack modes ---- */

static const char *const STACK_IDS[PC_STACK_MODE_COUNT] = {
    "entropy", "kurtosis", "maximum",           "mean",      "median",   "minimum",
    "range",   "skewness", "standardDeviation", "summation", "variance",
};

static const char *const STACK_LABELS[PC_STACK_MODE_COUNT] = {
    "Entropy", "Kurtosis", "Maximum",            "Mean",      "Median",   "Minimum",
    "Range",   "Skewness", "Standard Deviation", "Summation", "Variance",
};

const char *pc_stack_mode_id(pc_stack_mode m)
{
    return (size_t)m < PC_STACK_MODE_COUNT ? STACK_IDS[m] : "";
}

const char *pc_stack_mode_label(pc_stack_mode m)
{
    return (size_t)m < PC_STACK_MODE_COUNT ? STACK_LABELS[m] : "";
}

bool pc_stack_mode_from_id(const char *id, size_t len, pc_stack_mode *out)
{
    size_t i;
    if (id == NULL && len > 0) {
        return false;
    }
    for (i = 0; i < PC_STACK_MODE_COUNT; i++) {
        if (strlen(STACK_IDS[i]) == len && (len == 0 || memcmp(STACK_IDS[i], id, len) == 0)) {
            *out = (pc_stack_mode)i;
            return true;
        }
    }
    return false;
}
