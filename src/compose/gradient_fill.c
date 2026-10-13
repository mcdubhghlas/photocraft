/* Gradient Fill layers (port of crates/compose/src/gradient_fill.rs). */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "compose/internal.h"
#include "compose/stable_sort.h"

/* Smallest gradient length a handle drag produces (pixels), so the fill stays invertible. */
#define MIN_LEN 0.5f

/* Rust's f32::to_radians / to_degrees constants. */
#define RADS_PER_DEG (3.14159265358979323846f / 180.0f)
#define DEGS_PER_RAD 57.2957795130823208767981548141051703f

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* f32::total_cmp as a signed key. */
static int32_t total_key(float v)
{
    int32_t i;
    memcpy(&i, &v, sizeof i);
    return i ^ (int32_t)((uint32_t)(i >> 31) >> 1);
}

static bool color_less(const void *a, const void *b)
{
    return total_key(((const pc_compose_ramp_stop *)a)->pos) <
           total_key(((const pc_compose_ramp_stop *)b)->pos);
}

static bool opacity_less(const void *a, const void *b)
{
    return total_key(((const pc_opacity_stop *)a)->pos) < total_key(((const pc_opacity_stop *)b)->pos);
}

static pc_status ramp_init(const pc_fill *f, bool cmyk, bool *found, pc_compose_ramp *out, pc_error *err)
{
    pc_compose_ramp r;
    size_t i;
    pc_status st;
    if (f->kind != PC_FILL_GRADIENT) {
        *found = false;
        return PC_OK;
    }
    memset(&r, 0, sizeof r);
    r.nch = cmyk ? 5 : 4;
    if (f->u.gradient.nstops > 0) {
        r.color = pc_malloc_array(f->u.gradient.nstops, sizeof *r.color);
        if (r.color == NULL) {
            return nomem(err);
        }
        r.ncolor = f->u.gradient.nstops;
    }
    for (i = 0; i < r.ncolor; i++) {
        const pc_color_stop *s = &f->u.gradient.stops[i];
        r.color[i].pos = s->pos;
        if (cmyk) {
            pc_color k = pc_color_in_mode(&s->color, PC_COLOR_MODE_CMYK);
            memcpy(r.color[i].c, k.c, 4 * sizeof(float));
            r.color[i].c[4] = k.alpha;
        } else {
            float rgb[3];
            pc_color_to_rgb(&s->color, rgb);
            r.color[i].c[0] = rgb[0];
            r.color[i].c[1] = rgb[1];
            r.color[i].c[2] = rgb[2];
            r.color[i].c[3] = s->color.alpha;
            r.color[i].c[4] = 0.0f;
        }
    }
    if (f->u.gradient.nopacity_stops > 0) {
        r.opacity = pc_malloc_array(f->u.gradient.nopacity_stops, sizeof *r.opacity);
        if (r.opacity == NULL) {
            pc_compose_ramp_fini(&r);
            return nomem(err);
        }
        r.nopacity = f->u.gradient.nopacity_stops;
        memcpy(r.opacity, f->u.gradient.opacity_stops, r.nopacity * sizeof *r.opacity);
    }
    st = pc_cmp_stable_sort(r.color, r.ncolor, sizeof *r.color, color_less, err);
    if (st == PC_OK) {
        st = pc_cmp_stable_sort(r.opacity, r.nopacity, sizeof *r.opacity, opacity_less, err);
    }
    if (st != PC_OK) {
        pc_compose_ramp_fini(&r);
        return st;
    }
    r.mids = f->u.gradient.midpoints;
    r.nmids = f->u.gradient.midpoints != NULL ? f->u.gradient.nmidpoints : 0;
    *out = r;
    *found = true;
    return PC_OK;
}

pc_status pc_compose_ramp_init(const pc_fill *f, bool *found, pc_compose_ramp *out, pc_error *err)
{
    return ramp_init(f, false, found, out, err);
}

pc_status pc_compose_ramp_init_cmyk(const pc_fill *f, bool *found, pc_compose_ramp *out, pc_error *err)
{
    return ramp_init(f, true, found, out, err);
}

void pc_compose_ramp_fini(pc_compose_ramp *r)
{
    if (r == NULL) {
        return;
    }
    free(r->color);
    free(r->opacity);
    memset(r, 0, sizeof *r);
}

float pc_compose_midpoint_remap(float u, float m)
{
    float mm = pc_clampf(m, 0.05f, 0.95f);
    if (fabsf(mm - 0.5f) < 1e-6f) {
        return u;
    }
    if (u <= mm) {
        return 0.5f * u / mm;
    }
    return 0.5f + 0.5f * (u - mm) / (1.0f - mm);
}

static void sample_color(const pc_compose_ramp *r, float t, float *out)
{
    size_t n = r->nch == 5 ? 5 : 4;
    size_t i;
    size_t c;
    if (r->ncolor == 0) {
        for (c = 0; c < n; c++) {
            out[c] = 0.0f;
        }
        return;
    }
    if (t <= r->color[0].pos) {
        memcpy(out, r->color[0].c, n * sizeof(float));
        return;
    }
    for (i = 0; i + 1 < r->ncolor; i++) {
        const pc_compose_ramp_stop *a = &r->color[i];
        const pc_compose_ramp_stop *b = &r->color[i + 1];
        if (t <= b->pos) {
            float u = b->pos > a->pos ? (t - a->pos) / (b->pos - a->pos) : 0.0f;
            float k = pc_compose_midpoint_remap(u, i < r->nmids ? r->mids[i] : 0.5f);
            for (c = 0; c < n; c++) {
                out[c] = a->c[c] + (b->c[c] - a->c[c]) * k;
            }
            return;
        }
    }
    memcpy(out, r->color[r->ncolor - 1].c, n * sizeof(float));
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

void pc_compose_ramp_sample(const pc_compose_ramp *r, float t, float *out)
{
    size_t n = r->nch == 5 ? 5 : 4;
    sample_color(r, t, out);
    out[n - 1] *= sample_opacity(r->opacity, r->nopacity, t);
}

void pc_compose_gradient_dither_to(float *c, size_t n, int32_t x, int32_t y, bool has_quantum, float quantum)
{
    float d = (pc_dither_noise(x, y) - 0.5f) / 255.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        c[i] = pc_clampf(c[i] + d, 0.0f, 1.0f);
        if (has_quantum) {
            c[i] = floorf(c[i] * quantum + 0.5f) / quantum;
        }
    }
}

void pc_compose_gradient_dither(float c[4], int32_t x, int32_t y)
{
    pc_compose_gradient_dither_to(c, 3, x, y, false, 0.0f);
}

typedef struct render_job {
    const pc_compose_ramp *ramp;
    pc_gradient_style style;
    pc_compose_gradient_geom g;
    bool reverse;
    bool dither;
    bool has_quantum;
    float quantum;
    pc_rect rect;
    pc_rect frame;
    size_t w;
    size_t nch; /* floats per pixel in px */
    float *px;
} render_job;

static pc_status render_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const render_job *j = ctx;
    size_t row;
    size_t i;
    (void)chunk;
    for (row = begin; row < end; row++) {
        int32_t y = pc_i32_wrapping_add(j->rect.y0, (int32_t)row);
        float *out = j->px + row * j->w * j->nch;
        for (i = 0; i < j->w; i++) {
            int32_t x = pc_i32_wrapping_add(j->rect.x0, (int32_t)i);
            float t = pc_compose_gradient_t(j->style, j->g.angle, j->g.scale, j->reverse, j->g.offset,
                                            j->frame, (float)x + 0.5f, (float)y + 0.5f);
            pc_compose_ramp_sample(j->ramp, t, out + i * j->nch);
            if (j->dither) {
                pc_compose_gradient_dither_to(out + i * j->nch, j->nch - 1, x, y, j->has_quantum, j->quantum);
            }
        }
    }
    return PC_OK;
}

/* render_ramp: the ramp over rect (npx pixels of ramp->nch floats at px, zeroed). */
static pc_status render_ramp(const pc_fill *f, pc_rect rect, pc_rect frame, const pc_compose_ramp *ramp,
                             bool has_quantum, float quantum, float *px, size_t npx)
{
    render_job j;
    /* Linear and Reflected end points on whole pixels, as Photoshop (fill_layout). */
    j.ramp = ramp;
    j.style = f->u.gradient.style;
    j.g = pc_compose_gradient_layout(f->u.gradient.style, f->u.gradient.angle, f->u.gradient.scale,
                                     f->u.gradient.offset, frame);
    j.reverse = f->u.gradient.reverse;
    j.dither = f->u.gradient.dither;
    j.has_quantum = has_quantum;
    j.quantum = quantum;
    j.rect = rect;
    j.frame = frame;
    j.w = pc_cmp_w(rect);
    j.nch = ramp->nch == 5 ? 5 : 4;
    j.px = px;
    return pc_pool_for(pc_pool_global(), npx / j.w, 1, render_rows, &j);
}

pc_status pc_compose_gradient_fill_render_quantized(const pc_fill *f, pc_rect rect, pc_rect frame,
                                                    bool has_quantum, float quantum, pc_compose_buffer *out,
                                                    pc_error *err)
{
    pc_compose_buffer b;
    pc_compose_ramp ramp;
    bool found = false;
    pc_status st = pc_compose_buffer_transparent(rect, &b, err);
    if (st != PC_OK) {
        return st;
    }
    if (f->kind != PC_FILL_GRADIENT || pc_rect_width(rect) == 0 || b.len == 0) {
        *out = b;
        return PC_OK;
    }
    st = pc_compose_ramp_init(f, &found, &ramp, err);
    if (st != PC_OK) {
        pc_compose_buffer_fini(&b);
        return st;
    }
    st = render_ramp(f, rect, frame, &ramp, has_quantum, quantum, &b.px[0][0], b.len);
    pc_compose_ramp_fini(&ramp);
    if (st != PC_OK) {
        pc_compose_buffer_fini(&b);
        return st;
    }
    *out = b;
    return PC_OK;
}

pc_status pc_compose_gradient_fill_render(const pc_fill *f, pc_rect rect, pc_rect frame,
                                          pc_compose_buffer *out, pc_error *err)
{
    return pc_compose_gradient_fill_render_quantized(f, rect, frame, false, 0.0f, out, err);
}

pc_status pc_compose_render_cmyk_fill(const pc_fill *f, pc_rect rect, pc_rect frame, pc_sample_type depth,
                                      bool *found, pc_surface *out, pc_error *err)
{
    pc_surface s;
    pc_status st = PC_OK;
    if (f->kind != PC_FILL_SOLID && f->kind != PC_FILL_GRADIENT) {
        *found = false;
        return PC_OK;
    }
    pc_surface_init(&s, (pc_pixel_format){PC_COLOR_MODE_CMYK, depth, true});
    if (f->kind == PC_FILL_SOLID) {
        pc_color k = pc_color_in_mode(&f->u.solid, PC_COLOR_MODE_CMYK);
        const float px[5] = {k.c[0], k.c[1], k.c[2], k.c[3], k.alpha};
        st = pc_surface_fill_rect(&s, rect, px, 5, err);
    } else {
        size_t n;
        st = pc_cmp_rect_pixels(rect, &n, err);
        if (st == PC_OK && n > 0 && pc_rect_width(rect) > 0) {
            pc_compose_ramp ramp;
            bool has = false;
            float *px = pc_calloc_array(n, 5 * sizeof *px);
            if (px == NULL) {
                st = nomem(err);
            } else {
                st = ramp_init(f, true, &has, &ramp, err);
                if (st == PC_OK) {
                    st = render_ramp(f, rect, frame, &ramp, false, 0.0f, px, n);
                    pc_compose_ramp_fini(&ramp);
                }
                if (st == PC_OK) {
                    st = pc_surface_write_region(&s, rect, px, n * 5, err);
                }
                free(px);
            }
        }
    }
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    pc_surface_prune(&s);
    *out = s;
    *found = true;
    return PC_OK;
}

static void frame_dims(pc_rect frame, float *w, float *h)
{
    uint32_t fw = pc_rect_width(frame);
    uint32_t fh = pc_rect_height(frame);
    *w = (float)(fw > 1 ? fw : 1);
    *h = (float)(fh > 1 ? fh : 1);
}

/* The gradient's reach from its centre at scale 1: the full chord for Linear, half the chord for
 * Reflected, the radius for the others. */
static float unit_reach(pc_gradient_style style, float angle, pc_rect frame)
{
    float w;
    float h;
    float chord;
    float len;
    frame_dims(frame, &w, &h);
    pc_compose_gradient_units(angle, w, h, &chord, &len);
    switch (style) {
    case PC_GRADIENT_LINEAR:
        return chord;
    case PC_GRADIENT_REFLECTED:
        return chord / 2.0f;
    default:
        return len / 2.0f;
    }
}

void pc_compose_gradient_handles(pc_gradient_style style, float angle, float scale, const float offset[2],
                                 pc_rect frame, float from[2], float to[2])
{
    float w;
    float h;
    float c[2];
    float a = angle * RADS_PER_DEG;
    float s = sinf(a);
    float co = cosf(a);
    float dir[2];
    float reach;
    frame_dims(frame, &w, &h);
    /* The centre: the frame's centre moved by offset. */
    c[0] = (float)frame.x0 + w / 2.0f + offset[0] * w;
    c[1] = (float)frame.y0 + h / 2.0f + offset[1] * h;
    dir[0] = co;
    dir[1] = -s;
    reach = unit_reach(style, angle, frame) * pc_rs_maxf_k(scale, 1e-3f);
    if (style == PC_GRADIENT_LINEAR) {
        float hh = reach / 2.0f;
        from[0] = c[0] - dir[0] * hh;
        from[1] = c[1] - dir[1] * hh;
        to[0] = c[0] + dir[0] * hh;
        to[1] = c[1] + dir[1] * hh;
        return;
    }
    from[0] = c[0];
    from[1] = c[1];
    to[0] = c[0] + dir[0] * reach;
    to[1] = c[1] + dir[1] * reach;
}

bool pc_compose_gradient_fill_handles(const pc_fill *f, pc_rect frame, float from[2], float to[2])
{
    if (f->kind != PC_FILL_GRADIENT) {
        return false;
    }
    pc_compose_gradient_handles(f->u.gradient.style, f->u.gradient.angle, f->u.gradient.scale,
                                f->u.gradient.offset, frame, from, to);
    return true;
}

void pc_compose_gradient_from_handles(pc_gradient_style style, const float from[2], const float to[2],
                                      pc_rect frame, float fallback_angle, float *angle, float *scale,
                                      float offset[2])
{
    float dx = to[0] - from[0];
    float dy = to[1] - from[1];
    float len = hypotf(dx, dy);
    float a = len > 1e-4f ? atan2f(-dy, dx) * DEGS_PER_RAD : fallback_angle;
    float c[2];
    float w;
    float h;
    len = pc_rs_maxf_k(len, MIN_LEN);
    if (style == PC_GRADIENT_LINEAR) {
        c[0] = (from[0] + to[0]) / 2.0f;
        c[1] = (from[1] + to[1]) / 2.0f;
    } else {
        c[0] = from[0];
        c[1] = from[1];
    }
    frame_dims(frame, &w, &h);
    offset[0] = (c[0] - (float)frame.x0 - w / 2.0f) / w;
    offset[1] = (c[1] - (float)frame.y0 - h / 2.0f) / h;
    *scale = len / unit_reach(style, a, frame);
    *angle = a;
}
