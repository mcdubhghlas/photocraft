#include "psd/grd.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "psd/descriptor.h"
#include "psd/internal.h"

void pc_psd_grd_gradient_fini(pc_psd_grd_gradient *g)
{
    if (g == NULL) {
        return;
    }
    pc_psd_str_fini(&g->name);
    free(g->stops);
    free(g->opacity);
    memset(g, 0, sizeof *g);
}

void pc_psd_grd_free(pc_psd_grd_gradient *g, size_t n)
{
    size_t i;
    for (i = 0; g != NULL && i < n; i++) {
        pc_psd_grd_gradient_fini(&g[i]);
    }
    free(g);
}

static size_t ncomp(pc_psd_grd_model m)
{
    switch (m) {
    case PC_PSD_GRD_CMYK:
        return 4;
    case PC_PSD_GRD_GRAY:
        return 1;
    case PC_PSD_GRD_FOREGROUND:
    case PC_PSD_GRD_BACKGROUND:
        return 0;
    default:
        return 3;
    }
}

bool pc_psd_grd_gradient_eq(const pc_psd_grd_gradient *a, const pc_psd_grd_gradient *b)
{
    size_t i;
    size_t k;
    if (!pc_psd_str_eq(&a->name, &b->name) || a->noise != b->noise || a->nstops != b->nstops ||
        a->nopacity != b->nopacity) {
        return false;
    }
    for (i = 0; i < a->nstops; i++) {
        const pc_psd_grd_stop *x = &a->stops[i];
        const pc_psd_grd_stop *y = &b->stops[i];
        if (!(x->location == y->location && x->midpoint == y->midpoint) || x->color.model != y->color.model) {
            return false;
        }
        for (k = 0; k < ncomp(x->color.model); k++) {
            if (!(x->color.c[k] == y->color.c[k])) {
                return false;
            }
        }
    }
    for (i = 0; i < a->nopacity; i++) {
        const pc_psd_grd_opacity *x = &a->opacity[i];
        const pc_psd_grd_opacity *y = &b->opacity[i];
        if (!(x->location == y->location && x->opacity == y->opacity && x->midpoint == y->midpoint)) {
            return false;
        }
    }
    return true;
}

/* ---- reading ---- */

static bool num(const pc_psd_desc *d, const char *k, double *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, k);
    double x;
    if (v == NULL) {
        return false;
    }
    switch (v->kind) {
    case PC_PSD_V_DOUBLE:
    case PC_PSD_V_UNIT_FLOAT:
        x = v->d;
        break;
    case PC_PSD_V_INTEGER:
        x = (double)v->i;
        break;
    case PC_PSD_V_LARGE_INTEGER:
        x = (double)v->l;
        break;
    default:
        return false;
    }
    /* Deliberately not filtered: every caller passes the result through `finite`. */
    *out = x;
    return true;
}

/* The single guard every number passes through on its way to a float: only NaN has no value
 * (false; the caller falls back to its field default); everything else, +-inf included, saturates
 * to the float range so the mapping stays monotonic. */
static bool finite(double v, float *out)
{
    if (isnan(v)) {
        return false;
    }
    if (v < -(double)FLT_MAX) {
        v = -(double)FLT_MAX;
    } else if (v > (double)FLT_MAX) {
        v = (double)FLT_MAX;
    }
    *out = (float)v;
    return true;
}

static float f(const pc_psd_desc *d, const char *k)
{
    double x = 0.0;
    float r = 0.0f;
    (void)num(d, k, &x);
    (void)finite(x, &r);
    return r;
}

static float clamp01(float v)
{
    return pc_clampf(v, 0.0f, 1.0f);
}

/* A hue in degrees: in range (0..=360, 360 included) kept verbatim, otherwise wrapped like
 * f32::rem_euclid (370 -> 10, -10 -> 350). */
static float hue(float h)
{
    float r;
    if (h >= 0.0f && h <= 360.0f) {
        return h;
    }
    r = fmodf(h, 360.0f);
    return r < 0.0f ? r + 360.0f : r;
}

static const pc_psd_desc *obj(const pc_psd_desc *d, const char *k)
{
    const pc_psd_value *v = pc_psd_desc_get(d, k);
    if (v != NULL && (v->kind == PC_PSD_V_DESCRIPTOR || v->kind == PC_PSD_V_GLOBAL_OBJECT)) {
        return v->desc;
    }
    return NULL;
}

static const pc_psd_value *list(const pc_psd_desc *d, const char *k, size_t *n)
{
    const pc_psd_value *v = pc_psd_desc_get(d, k);
    if (v != NULL && v->kind == PC_PSD_V_LIST) {
        *n = v->nlist;
        return v->list;
    }
    *n = 0;
    return NULL;
}

static bool enum_is(const pc_psd_desc *d, const char *k, const char *val)
{
    const pc_psd_value *v = pc_psd_desc_get(d, k);
    return v != NULL && v->kind == PC_PSD_V_ENUMERATED && pc_psd_id_is(&v->value, val);
}

static bool class_is(const pc_psd_desc *c, const char *id)
{
    size_t n;
    const uint8_t *b = pc_psd_id_bytes(&c->class_id, &n);
    return n == 4 && memcmp(b, id, 4) == 0;
}

static bool color(const pc_psd_desc *c, pc_psd_grd_color *out)
{
    memset(out, 0, sizeof *out);
    if (class_is(c, "RGBC")) {
        out->model = PC_PSD_GRD_RGB;
        out->c[0] = clamp01(f(c, "Rd  ") / 255.0f);
        out->c[1] = clamp01(f(c, "Grn ") / 255.0f);
        out->c[2] = clamp01(f(c, "Bl  ") / 255.0f);
    } else if (class_is(c, "HSBC")) {
        out->model = PC_PSD_GRD_HSB;
        out->c[0] = hue(f(c, "H   "));
        out->c[1] = clamp01(f(c, "Strt") / 100.0f);
        out->c[2] = clamp01(f(c, "Brgh") / 100.0f);
    } else if (class_is(c, "CMYC")) {
        out->model = PC_PSD_GRD_CMYK;
        out->c[0] = clamp01(f(c, "Cyn ") / 100.0f);
        out->c[1] = clamp01(f(c, "Mgnt") / 100.0f);
        out->c[2] = clamp01(f(c, "Ylw ") / 100.0f);
        out->c[3] = clamp01(f(c, "Blck") / 100.0f);
    } else if (class_is(c, "LbCl")) {
        out->model = PC_PSD_GRD_LAB;
        /* L* 0..=100; a*, b* bounded to the CIELAB gamut. */
        out->c[0] = pc_clampf(f(c, "Lmnc"), 0.0f, 100.0f);
        out->c[1] = pc_clampf(f(c, "A   "), -128.0f, 127.0f);
        out->c[2] = pc_clampf(f(c, "B   "), -128.0f, 127.0f);
    } else if (class_is(c, "Grsc")) {
        out->model = PC_PSD_GRD_GRAY;
        out->c[0] = clamp01(f(c, "Gry ") / 100.0f);
    } else {
        return false;
    }
    return true;
}

static float location(const pc_psd_desc *d)
{
    return pc_clampf(f(d, "Lctn") / 4096.0f, 0.0f, 1.0f);
}

/* A percentage key as 0..1: divided in f64, NaN falls back to dflt / 100. */
static float percent(const pc_psd_desc *d, const char *k, double dflt)
{
    double x = dflt;
    float r = (float)(dflt / 100.0);
    (void)num(d, k, &x);
    (void)finite(x / 100.0, &r);
    return pc_clampf(r, 0.0f, 1.0f);
}

static pc_status gradient(const pc_psd_desc *g, pc_psd_grd_gradient *out, pc_error *err)
{
    pc_psd_grd_gradient r;
    const pc_psd_value *v = pc_psd_desc_get(g, "Nm  ");
    const pc_psd_value *items;
    size_t n;
    size_t i;
    pc_status st = PC_OK;
    memset(&r, 0, sizeof r);
    if (v != NULL && v->kind == PC_PSD_V_TEXT) {
        size_t end = v->text.n;
        while (end > 0 && v->text.u[end - 1] == 0) {
            end--;
        }
        CK(psd_str_from_units(v->text.u, end, &r.name, err));
    }
    r.noise = enum_is(g, "GrdF", "ClNs");
    items = list(g, "Clrs", &n);
    if (n > PC_PSD_GRD_MAX_STOPS) {
        n = PC_PSD_GRD_MAX_STOPS;
    }
    if (n > 0 && (r.stops = pc_calloc_array(n, sizeof *r.stops)) == NULL) {
        st = psd_nomem(err);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        const pc_psd_desc *s;
        pc_psd_grd_stop stop;
        if (items[i].kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        s = items[i].desc;
        memset(&stop, 0, sizeof stop);
        if (enum_is(s, "Type", "FrgC")) {
            stop.color.model = PC_PSD_GRD_FOREGROUND;
        } else if (enum_is(s, "Type", "BckC")) {
            stop.color.model = PC_PSD_GRD_BACKGROUND;
        } else {
            const pc_psd_desc *c = obj(s, "Clr ");
            if (c == NULL || !color(c, &stop.color)) {
                continue;
            }
        }
        stop.location = location(s);
        stop.midpoint = percent(s, "Mdpn", 50.0);
        r.stops[r.nstops++] = stop;
    }
    items = list(g, "Trns", &n);
    if (n > PC_PSD_GRD_MAX_STOPS) {
        n = PC_PSD_GRD_MAX_STOPS;
    }
    if (n > 0 && (r.opacity = pc_calloc_array(n, sizeof *r.opacity)) == NULL) {
        st = psd_nomem(err);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        const pc_psd_desc *s;
        if (items[i].kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        s = items[i].desc;
        r.opacity[r.nopacity].location = location(s);
        r.opacity[r.nopacity].opacity = percent(s, "Opct", 100.0);
        r.opacity[r.nopacity].midpoint = percent(s, "Mdpn", 50.0);
        r.nopacity++;
    }
    *out = r;
    return PC_OK;
fail:
    pc_psd_grd_gradient_fini(&r);
    return st;
}

pc_status pc_psd_grd_parse(const uint8_t *data, size_t len, pc_psd_grd_gradient **out, size_t *nout, pc_error *err)
{
    uint16_t version;
    pc_psd_desc vd;
    size_t used;
    const pc_psd_value *items;
    size_t n;
    size_t i;
    pc_psd_grd_gradient *v = NULL;
    size_t nv = 0;
    pc_status st;
    if (len < 4) {
        return psd_eof(err, 0, 4);
    }
    if (memcmp(data, "8BGR", 4) != 0) {
        return psd_bad_sig(err, "8BGR", data);
    }
    if (len < 6) {
        return psd_eof(err, 4, 2);
    }
    version = (uint16_t)((data[4] << 8) | data[5]);
    if (version != 5) {
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "unsupported: gradient file version %u (only version 5, Photoshop 6 and later)",
                       (unsigned)version);
    }
    st = pc_psd_vdesc_parse_prefix(data + 6, len - 6, &vd, &used, err);
    if (st != PC_OK) {
        return st;
    }
    items = list(&vd, "GrdL", &n);
    if (n > PC_PSD_GRD_MAX_GRADIENTS) {
        n = PC_PSD_GRD_MAX_GRADIENTS;
    }
    if (n > 0 && (v = pc_calloc_array(n, sizeof *v)) == NULL) {
        st = psd_nomem(err);
        goto fail;
    }
    for (i = 0; i < n; i++) {
        const pc_psd_desc *g;
        if (items[i].kind != PC_PSD_V_DESCRIPTOR || (g = obj(items[i].desc, "Grad")) == NULL) {
            continue;
        }
        CK(gradient(g, &v[nv], err));
        nv++;
    }
    if (nv == 0) {
        st = psd_invalid(err, "the gradient file holds no gradients");
        goto fail;
    }
    pc_psd_desc_fini(&vd);
    *out = v;
    *nout = nv;
    return PC_OK;
fail:
    pc_psd_grd_free(v, nv);
    pc_psd_desc_fini(&vd);
    return st;
}

/* ---- writing ---- */

static int32_t scaled(float x, float by)
{
    return pc_f32_to_i32(roundf(x * by));
}

static pc_status add_int(pc_psd_desc *d, const char *k, int32_t i, pc_error *err)
{
    pc_psd_value v = pc_psd_value_int(i);
    return pc_psd_desc_add(d, k, &v, err);
}

static pc_status add_double(pc_psd_desc *d, const char *k, double x, pc_error *err)
{
    pc_psd_value v = pc_psd_value_double(x);
    return pc_psd_desc_add(d, k, &v, err);
}

static pc_status add_enum(pc_psd_desc *d, const char *k, const char *t, const char *val, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_enum(t, val, &v, err);
    return st == PC_OK ? pc_psd_desc_add(d, k, &v, err) : st;
}

/* Moves *o into d under k. */
static pc_status add_obj(pc_psd_desc *d, const char *k, pc_psd_desc *o, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_object(o, &v, err);
    return st == PC_OK ? pc_psd_desc_add(d, k, &v, err) : st;
}

static pc_status push_obj(pc_psd_value *l, pc_psd_desc *o, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_object(o, &v, err);
    return st == PC_OK ? pc_psd_value_list_push(l, &v, err) : st;
}

static pc_status stop_desc(const pc_psd_grd_stop *s, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_desc rgb;
    const char *ty = "UsrS";
    pc_status st = pc_psd_desc_init(&d, "Clrt", err);
    if (st != PC_OK) {
        return st;
    }
    if (s->color.model == PC_PSD_GRD_FOREGROUND) {
        ty = "FrgC";
    } else if (s->color.model == PC_PSD_GRD_BACKGROUND) {
        ty = "BckC";
    } else {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        if (s->color.model == PC_PSD_GRD_RGB) {
            r = s->color.c[0];
            g = s->color.c[1];
            b = s->color.c[2];
        }
        st = pc_psd_desc_init(&rgb, "RGBC", err);
        if (st != PC_OK) {
            goto fail;
        }
        if ((st = add_double(&rgb, "Rd  ", (double)r * 255.0, err)) != PC_OK ||
            (st = add_double(&rgb, "Grn ", (double)g * 255.0, err)) != PC_OK ||
            (st = add_double(&rgb, "Bl  ", (double)b * 255.0, err)) != PC_OK) {
            pc_psd_desc_fini(&rgb);
            goto fail;
        }
        CK(add_obj(&d, "Clr ", &rgb, err));
    }
    CK(add_enum(&d, "Type", "Clry", ty, err));
    CK(add_int(&d, "Lctn", scaled(s->location, 4096.0f), err));
    CK(add_int(&d, "Mdpn", scaled(s->midpoint, 100.0f), err));
    *out = d;
    return PC_OK;
fail:
    pc_psd_desc_fini(&d);
    return st;
}

static pc_status opacity_desc(const pc_psd_grd_opacity *o, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "TrnS", err);
    if (st != PC_OK) {
        return st;
    }
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, "#Prc", 4);
    v.d = (double)o->opacity * 100.0;
    CK(pc_psd_desc_add(&d, "Opct", &v, err));
    CK(add_int(&d, "Lctn", scaled(o->location, 4096.0f), err));
    CK(add_int(&d, "Mdpn", scaled(o->midpoint, 100.0f), err));
    *out = d;
    return PC_OK;
fail:
    pc_psd_desc_fini(&d);
    return st;
}

static pc_status gradient_desc(const pc_psd_grd_gradient *g, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc outer;
    pc_psd_desc grad;
    pc_psd_desc one;
    pc_psd_value stops;
    pc_psd_value trns;
    pc_psd_value name;
    size_t i;
    pc_status st;
    memset(&stops, 0, sizeof stops);
    stops.kind = PC_PSD_V_LIST;
    trns = stops;
    memset(&outer, 0, sizeof outer);
    st = pc_psd_desc_init(&grad, "Grdn", err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < g->nstops; i++) {
        CK(stop_desc(&g->stops[i], &one, err));
        CK(push_obj(&stops, &one, err));
    }
    for (i = 0; i < g->nopacity; i++) {
        CK(opacity_desc(&g->opacity[i], &one, err));
        CK(push_obj(&trns, &one, err));
    }
    memset(&name, 0, sizeof name);
    name.kind = PC_PSD_V_TEXT;
    CK(pc_psd_ustr_from_utf8n(pc_psd_str_c(&g->name), g->name.len, true, &name.text, err));
    CK(pc_psd_desc_add(&grad, "Nm  ", &name, err));
    CK(add_enum(&grad, "GrdF", "GrdF", g->noise ? "ClNs" : "CstS", err));
    CK(add_double(&grad, "Intr", 4096.0, err));
    CK(pc_psd_desc_add(&grad, "Clrs", &stops, err));
    CK(pc_psd_desc_add(&grad, "Trns", &trns, err));
    CK(pc_psd_desc_init(&outer, "Grdn", err));
    CK(add_obj(&outer, "Grad", &grad, err));
    *out = outer;
    return PC_OK;
fail:
    pc_psd_value_fini(&stops);
    pc_psd_value_fini(&trns);
    pc_psd_desc_fini(&grad);
    pc_psd_desc_fini(&outer);
    return st;
}

pc_status pc_psd_grd_write(const pc_psd_grd_gradient *g, size_t n, pc_psd_bytes *out, pc_error *err)
{
    pc_psd_desc root;
    pc_psd_desc one;
    pc_psd_value items;
    pc_psd_bytes body = {NULL, 0};
    psd_w w;
    size_t i;
    pc_status st;
    memset(&items, 0, sizeof items);
    items.kind = PC_PSD_V_LIST;
    st = pc_psd_desc_init(&root, "null", err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        CK(gradient_desc(&g[i], &one, err));
        CK(push_obj(&items, &one, err));
    }
    CK(pc_psd_desc_add(&root, "GrdL", &items, err));
    CK(pc_psd_vdesc_to_bytes(&root, &body, err));
    pc_psd_desc_fini(&root);
    psd_w_init(&w, err);
    psd_put(&w, "8BGR", 4);
    psd_put_u16(&w, 5);
    psd_put(&w, body.data, body.len);
    pc_psd_bytes_fini(&body);
    return psd_w_finish(&w, out);
fail:
    pc_psd_value_fini(&items);
    pc_psd_desc_fini(&root);
    return st;
}
