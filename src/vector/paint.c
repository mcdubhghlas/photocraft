/* Colour sources for shape fills and strokes (port of the Paint half of crates/vector/src/lib.rs):
 * solid colours and gradients with midpoints, opacity stops, centre offsets, dithering and
 * Photoshop's whole-pixel end-point snapping. Mirrors the compositor's fill layers (its parity
 * tests keep the two in step). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "vector/internal.h"

void pc_vec_paint_fini(pc_vec_paint *p)
{
    if (p == NULL) {
        return;
    }
    free(p->stops);
    free(p->midpoints);
    free(p->opacity_stops);
    memset(p, 0, sizeof *p);
}

static void *dup_array(const void *src, size_t n, size_t size, bool *fail)
{
    void *d;
    if (n == 0) {
        return NULL;
    }
    d = pc_malloc_array(n, size);
    if (d == NULL) {
        *fail = true;
        return NULL;
    }
    memcpy(d, src, n * size);
    return d;
}

pc_status pc_vec_paint_clone(const pc_vec_paint *p, pc_vec_paint *out, pc_error *err)
{
    pc_vec_paint c = *p;
    bool fail = false;
    c.stops = dup_array(p->stops, p->nstops, sizeof *p->stops, &fail);
    c.midpoints = dup_array(p->midpoints, p->nmidpoints, sizeof *p->midpoints, &fail);
    c.opacity_stops = dup_array(p->opacity_stops, p->nopacity_stops, sizeof *p->opacity_stops, &fail);
    if (fail) {
        pc_vec_paint_fini(&c);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    *out = c;
    return PC_OK;
}

/* (chord, diagonal) gradient lengths over a w x h frame at angle (radians): Linear and Reflected
 * gradients span the chord, the other styles the half-diagonal. */
static void gradient_units(double angle, double w, double h, double *chord, double *len)
{
    double s = sin(angle);
    double c = cos(angle);
    double a = c * w;
    double b = s * h;
    *len = pc_rs_max_k(sqrt(a * a + b * b), 1.0);
    *chord = pc_rs_max_k(pc_rs_min(w / pc_rs_max_k(fabs(c), 1e-6), h / pc_rs_max_k(fabs(s), 1e-6)), 1.0);
}

static double chord_of(double a, double w, double h)
{
    double chord;
    double len;
    gradient_units(a, w, h, &chord, &len);
    return chord;
}

/* Truncate to the pixel grid (with a little slack for rounding error in exact cases). */
static double snap(double v)
{
    return floor(v + 1e-4);
}

/* Effective (angle, scale, offset) for a gradient laid out in frame, snapping Linear and
 * Reflected end points to whole pixels as Photoshop does. */
static void gradient_layout(pc_gradient_style style, float angle, float scale, const float offset[2],
                            const double frame[4], float *oangle, float *oscale, float ooffset[2])
{
    double w;
    double h;
    double cx;
    double cy;
    double a;
    double half;
    double s;
    double c;
    double dx;
    double dy;
    double end[2];
    double start[2];
    double mid[2];
    double v[2];
    double l;
    double a2;
    double span;
    double scale2;
    float shift[2];
    *oangle = angle;
    *oscale = scale;
    ooffset[0] = offset[0];
    ooffset[1] = offset[1];
    if (!(style == PC_GRADIENT_LINEAR || style == PC_GRADIENT_REFLECTED) || !isfinite(angle) ||
        !isfinite(scale) || !isfinite(offset[0]) || !isfinite(offset[1])) {
        return;
    }
    w = pc_rs_max_k(frame[2] - frame[0], 1.0);
    h = pc_rs_max_k(frame[3] - frame[1], 1.0);
    cx = frame[0] + w / 2.0 + (double)offset[0] * w;
    cy = frame[1] + h / 2.0 + (double)offset[1] * h;
    a = pc_vec_to_radians((double)angle);
    half = chord_of(a, w, h) * (double)pc_rs_maxf_k(scale, 1e-3f) / 2.0;
    s = sin(a);
    c = cos(a);
    dx = c * half;
    dy = -s * half;
    end[0] = snap(cx + dx);
    end[1] = snap(cy + dy);
    if (style == PC_GRADIENT_REFLECTED) {
        start[0] = cx;
        start[1] = cy;
        mid[0] = cx;
        mid[1] = cy;
    } else {
        start[0] = snap(cx - dx);
        start[1] = snap(cy - dy);
        mid[0] = (start[0] + end[0]) / 2.0;
        mid[1] = (start[1] + end[1]) / 2.0;
    }
    v[0] = end[0] - start[0];
    v[1] = end[1] - start[1];
    l = hypot(v[0], v[1]);
    if (l < 0.5) {
        return;
    }
    a2 = atan2(-v[1], v[0]);
    /* Reflected spans half the chord from the centre; Linear the whole chord. */
    span = style == PC_GRADIENT_REFLECTED ? 2.0 * l : l;
    scale2 = span / chord_of(a2, w, h);
    shift[0] = pc_f64_to_f32((mid[0] - cx) / w);
    shift[1] = pc_f64_to_f32((mid[1] - cy) / h);
    *oangle = pc_f64_to_f32(pc_vec_to_degrees(a2));
    *oscale = pc_f64_to_f32(scale2);
    ooffset[0] = offset[0] + shift[0];
    ooffset[1] = offset[1] + shift[1];
}

/* Rust's f64 rem_euclid. */
static double rem_euclid(double a, double b)
{
    double r = fmod(a, b);
    return r < 0.0 ? r + fabs(b) : r;
}

/* Gradient parameter t (0..=1) at pixel centre (x, y) in frame. */
static float gradient_t(const pc_vec_paint *p, double x, double y)
{
    const double *frame = p->frame;
    double w = pc_rs_max_k(frame[2] - frame[0], 1.0);
    double h = pc_rs_max_k(frame[3] - frame[1], 1.0);
    double cx = frame[0] + w / 2.0 + (double)p->layout_offset[0] * w;
    double cy = frame[1] + h / 2.0 + (double)p->layout_offset[1] * h;
    double a = pc_vec_to_radians((double)p->layout_angle);
    double s = sin(a);
    double c = cos(a);
    double dx = x - cx;
    double dy = y - cy;
    double along = dx * c - dy * s;
    double across = dx * s + dy * c;
    double chord;
    double len;
    double k = (double)pc_rs_maxf_k(p->layout_scale, 1e-3f);
    double corner;
    double t;
    gradient_units(a, w, h, &chord, &len);
    chord = chord * k;
    len = len * k;
    corner = 0.5 * (c - s);
    switch (p->style) {
    case PC_GRADIENT_REFLECTED:
        t = fabs((along - corner) / (chord / 2.0));
        break;
    case PC_GRADIENT_RADIAL:
        t = hypot(dx, dy) / (len / 2.0);
        break;
    case PC_GRADIENT_DIAMOND:
        t = (fabs(along) + fabs(across)) / (len / 2.0);
        break;
    case PC_GRADIENT_ANGLE:
        /* Clockwise sweep starting at the gradient angle. */
        t = rem_euclid((a - atan2(-dy, dx)) / PC_VEC_TAU, 1.0);
        break;
    case PC_GRADIENT_LINEAR:
    default:
        t = (along - corner) / chord + 0.5;
        break;
    }
    t = pc_clampd(t, 0.0, 1.0);
    return pc_f64_to_f32(p->reverse ? 1.0 - t : t);
}

/* Where a segment of relative position u lands once its midpoint is m (0.5 = unchanged). */
static float midpoint_remap(float u, float m)
{
    m = pc_clampf(m, 0.05f, 0.95f);
    if (fabsf(m - 0.5f) < 1e-6f) {
        return u;
    }
    if (u <= m) {
        return 0.5f * u / m;
    }
    return 0.5f + 0.5f * (u - m) / (1.0f - m);
}

static void sample_color(const pc_vec_paint *p, float t, float out[4])
{
    size_t i;
    const pc_vec_rgba_stop *s = p->stops;
    size_t n = p->nstops;
    if (n == 0) {
        memset(out, 0, 4 * sizeof *out);
        return;
    }
    if (t <= s[0].pos) {
        memcpy(out, s[0].rgba, 4 * sizeof *out);
        return;
    }
    for (i = 0; i + 1 < n; i++) {
        const pc_vec_rgba_stop *a = &s[i];
        const pc_vec_rgba_stop *b = &s[i + 1];
        if (t <= b->pos) {
            float u = b->pos > a->pos ? (t - a->pos) / (b->pos - a->pos) : 0.0f;
            float k = midpoint_remap(u, i < p->nmidpoints ? p->midpoints[i] : 0.5f);
            int ch;
            for (ch = 0; ch < 4; ch++) {
                out[ch] = a->rgba[ch] + (b->rgba[ch] - a->rgba[ch]) * k;
            }
            return;
        }
    }
    memcpy(out, s[n - 1].rgba, 4 * sizeof *out);
}

static float sample_opacity(const pc_opacity_stop *s, size_t n, float t)
{
    size_t i;
    if (n == 0) {
        return 1.0f;
    }
    if (t <= s[0].pos) {
        return s[0].opacity;
    }
    for (i = 0; i + 1 < n; i++) {
        if (t <= s[i + 1].pos) {
            float u = s[i + 1].pos > s[i].pos ? (t - s[i].pos) / (s[i + 1].pos - s[i].pos) : 0.0f;
            return s[i].opacity + (s[i + 1].opacity - s[i].opacity) * u;
        }
    }
    return s[n - 1].opacity;
}

void pc_vec_paint_at(const pc_vec_paint *p, double x, double y, float out[4])
{
    switch (p->kind) {
    case PC_VEC_PAINT_SOLID:
        memcpy(out, p->solid, 4 * sizeof *out);
        return;
    case PC_VEC_PAINT_GRADIENT: {
        float t = gradient_t(p, x, y);
        sample_color(p, t, out);
        if (p->nopacity_stops > 0) {
            out[3] *= sample_opacity(p->opacity_stops, p->nopacity_stops, t);
        }
        if (p->dither) {
            /* One level of the shared dither noise, as the Gradient tool and fill layers. */
            float n = (pc_dither_noise(pc_f64_to_i32(floor(x)), pc_f64_to_i32(floor(y))) - 0.5f) / 255.0f;
            int ch;
            for (ch = 0; ch < 3; ch++) {
                out[ch] = pc_clampf(out[ch] + n, 0.0f, 1.0f);
            }
        }
        return;
    }
    case PC_VEC_PAINT_NONE:
    default:
        memset(out, 0, 4 * sizeof *out);
        return;
    }
}

static void rgba_of(const pc_color *c, float out[4])
{
    float v[3];
    pc_color_to_rgb(c, v);
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    out[3] = c->alpha;
}

static int stop_cmp(const void *a, const void *b)
{
    return pc_vec_total_cmp_f32(((const pc_vec_rgba_stop *)a)->pos, ((const pc_vec_rgba_stop *)b)->pos);
}

static int opacity_cmp(const void *a, const void *b)
{
    return pc_vec_total_cmp_f32(((const pc_opacity_stop *)a)->pos, ((const pc_opacity_stop *)b)->pos);
}

pc_status pc_vec_paint_from_fill(const pc_fill *f, const double bounds[4], const double canvas[4],
                                 pc_vec_paint *out, pc_error *err)
{
    pc_vec_paint p;
    memset(&p, 0, sizeof p);
    if (f->kind == PC_FILL_SOLID) {
        p.kind = PC_VEC_PAINT_SOLID;
        rgba_of(&f->u.solid, p.solid);
    } else if (f->kind == PC_FILL_GRADIENT) {
        const double *frame = f->u.gradient.align ? bounds : canvas;
        size_t n = f->u.gradient.nstops;
        size_t no = f->u.gradient.nopacity_stops;
        size_t nm = f->u.gradient.nmidpoints;
        size_t big = n > no ? n : no;
        void *scratch = NULL;
        size_t i;
        p.kind = PC_VEC_PAINT_GRADIENT;
        if (n > 0) {
            p.stops = pc_malloc_array(n, sizeof *p.stops);
        }
        if (no > 0) {
            p.opacity_stops = pc_malloc_array(no, sizeof *p.opacity_stops);
        }
        if (nm > 0) {
            p.midpoints = pc_malloc_array(nm, sizeof *p.midpoints);
        }
        if (big > 0) {
            scratch = pc_malloc_array(big, sizeof(pc_vec_rgba_stop));
        }
        if ((n > 0 && p.stops == NULL) || (no > 0 && p.opacity_stops == NULL) ||
            (nm > 0 && p.midpoints == NULL) || (big > 0 && scratch == NULL)) {
            free(scratch);
            pc_vec_paint_fini(&p);
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        for (i = 0; i < n; i++) {
            p.stops[i].pos = f->u.gradient.stops[i].pos;
            rgba_of(&f->u.gradient.stops[i].color, p.stops[i].rgba);
        }
        p.nstops = n;
        pc_vec_stable_sort(p.stops, n, sizeof *p.stops, scratch, stop_cmp);
        if (no > 0) {
            memcpy(p.opacity_stops, f->u.gradient.opacity_stops, no * sizeof *p.opacity_stops);
        }
        p.nopacity_stops = no;
        pc_vec_stable_sort(p.opacity_stops, no, sizeof *p.opacity_stops, scratch, opacity_cmp);
        free(scratch);
        if (nm > 0) {
            memcpy(p.midpoints, f->u.gradient.midpoints, nm * sizeof *p.midpoints);
        }
        p.nmidpoints = nm;
        p.style = f->u.gradient.style;
        gradient_layout(f->u.gradient.style, f->u.gradient.angle, f->u.gradient.scale, f->u.gradient.offset,
                        frame, &p.layout_angle, &p.layout_scale, p.layout_offset);
        p.reverse = f->u.gradient.reverse;
        p.dither = f->u.gradient.dither;
        memcpy(p.frame, frame, sizeof p.frame);
    } else {
        p.kind = PC_VEC_PAINT_NONE;
    }
    *out = p;
    return PC_OK;
}
