/* Fill-layer contents (port of lib.rs Fill). */
#include "doc/fill.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"

static bool color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

/* Copy n elements of size bytes; *out is NULL when n is 0. */
static pc_status dup_array(const void *src, size_t n, size_t size, void **out, pc_error *err)
{
    void *p;
    if (n == 0) {
        *out = NULL;
        return PC_OK;
    }
    p = pc_malloc_array(n, size);
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(p, src, n * size);
    *out = p;
    return PC_OK;
}

pc_fill pc_fill_solid(pc_color c)
{
    pc_fill f;
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_SOLID;
    f.u.solid = c;
    return f;
}

pc_status pc_fill_gradient(const pc_color_stop *stops, size_t n, float angle, float scale, pc_gradient_style style,
                           bool reverse, pc_fill *out, pc_error *err)
{
    pc_fill f;
    void *s;
    pc_status st = dup_array(stops, n, sizeof *stops, &s, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_GRADIENT;
    f.u.gradient.stops = s;
    f.u.gradient.nstops = n;
    f.u.gradient.angle = angle;
    f.u.gradient.scale = scale;
    f.u.gradient.style = style;
    f.u.gradient.reverse = reverse;
    f.u.gradient.offset[0] = 0.0f;
    f.u.gradient.offset[1] = 0.0f;
    f.u.gradient.dither = false;
    f.u.gradient.align = true;
    *out = f;
    return PC_OK;
}

void pc_fill_fini(pc_fill *f)
{
    if (f == NULL) {
        return;
    }
    switch (f->kind) {
    case PC_FILL_GRADIENT:
        free(f->u.gradient.stops);
        free(f->u.gradient.opacity_stops);
        free(f->u.gradient.midpoints);
        break;
    case PC_FILL_PATTERN:
        pc_str_fini(&f->u.pattern.name);
        pc_str_fini(&f->u.pattern.id);
        break;
    default:
        break;
    }
    memset(f, 0, sizeof *f);
}

pc_status pc_fill_clone(const pc_fill *f, pc_fill *out, pc_error *err)
{
    pc_fill t = *f;
    pc_status st = PC_OK;
    switch (f->kind) {
    case PC_FILL_GRADIENT: {
        void *s = NULL;
        void *o = NULL;
        void *m = NULL;
        st = dup_array(f->u.gradient.stops, f->u.gradient.nstops, sizeof *f->u.gradient.stops, &s, err);
        if (st == PC_OK) {
            st = dup_array(f->u.gradient.opacity_stops, f->u.gradient.nopacity_stops,
                           sizeof *f->u.gradient.opacity_stops, &o, err);
        }
        if (st == PC_OK) {
            st = dup_array(f->u.gradient.midpoints, f->u.gradient.nmidpoints, sizeof *f->u.gradient.midpoints, &m,
                           err);
        }
        if (st != PC_OK) {
            free(s);
            free(o);
            return st;
        }
        t.u.gradient.stops = s;
        t.u.gradient.opacity_stops = o;
        t.u.gradient.midpoints = m;
        break;
    }
    case PC_FILL_PATTERN:
        st = pc_str_clone(&f->u.pattern.name, &t.u.pattern.name, err);
        if (st != PC_OK) {
            return st;
        }
        st = pc_str_clone(&f->u.pattern.id, &t.u.pattern.id, err);
        if (st != PC_OK) {
            pc_str_fini(&t.u.pattern.name);
            return st;
        }
        break;
    default:
        break;
    }
    *out = t;
    return PC_OK;
}

bool pc_fill_eq(const pc_fill *a, const pc_fill *b)
{
    size_t i;
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_FILL_SOLID:
        return color_eq(&a->u.solid, &b->u.solid);
    case PC_FILL_GRADIENT:
        if (a->u.gradient.nstops != b->u.gradient.nstops ||
            a->u.gradient.nopacity_stops != b->u.gradient.nopacity_stops ||
            a->u.gradient.nmidpoints != b->u.gradient.nmidpoints) {
            return false;
        }
        for (i = 0; i < a->u.gradient.nstops; i++) {
            if (a->u.gradient.stops[i].pos != b->u.gradient.stops[i].pos ||
                !color_eq(&a->u.gradient.stops[i].color, &b->u.gradient.stops[i].color)) {
                return false;
            }
        }
        for (i = 0; i < a->u.gradient.nopacity_stops; i++) {
            if (a->u.gradient.opacity_stops[i].pos != b->u.gradient.opacity_stops[i].pos ||
                a->u.gradient.opacity_stops[i].opacity != b->u.gradient.opacity_stops[i].opacity) {
                return false;
            }
        }
        for (i = 0; i < a->u.gradient.nmidpoints; i++) {
            if (a->u.gradient.midpoints[i] != b->u.gradient.midpoints[i]) {
                return false;
            }
        }
        return a->u.gradient.angle == b->u.gradient.angle && a->u.gradient.scale == b->u.gradient.scale &&
               a->u.gradient.style == b->u.gradient.style && a->u.gradient.reverse == b->u.gradient.reverse &&
               a->u.gradient.offset[0] == b->u.gradient.offset[0] &&
               a->u.gradient.offset[1] == b->u.gradient.offset[1] && a->u.gradient.dither == b->u.gradient.dither &&
               a->u.gradient.align == b->u.gradient.align;
    case PC_FILL_PATTERN:
        return pc_str_eq(&a->u.pattern.name, &b->u.pattern.name) && a->u.pattern.scale == b->u.pattern.scale &&
               pc_str_eq(&a->u.pattern.id, &b->u.pattern.id) && a->u.pattern.angle == b->u.pattern.angle &&
               a->u.pattern.link == b->u.pattern.link && a->u.pattern.phase[0] == b->u.pattern.phase[0] &&
               a->u.pattern.phase[1] == b->u.pattern.phase[1];
    }
    return false;
}
