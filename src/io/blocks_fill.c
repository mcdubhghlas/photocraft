/* Fill layers, gradients, pattern references, type and smart-object blocks (port of the second
 * half of crates/io/src/blocks.rs). Parse failures of file data are "not found" (the
 * reference's None); only out of memory is an error. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/transform.h"
#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/str.h"
#include "base/utf8.h"
#include "io/blocks.h"
#include "io/desc_util.h"
#include "io/gradient_bake.h"

#define TRY(x)                                                                                               \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto fail;                                                                                       \
        }                                                                                                    \
    } while (0)

static float num_or(const pc_psd_desc *d, const char *k, double def)
{
    double v = def;
    (void)pc_io_desc_num(pc_psd_desc_get(d, k), &v);
    return pc_f64_to_f32(v);
}

static bool enum_is(const uint8_t *e, size_t n, const char *s)
{
    return e != NULL && n == strlen(s) && memcmp(e, s, n) == 0;
}

/* ---- gradients ---- */

pc_gradient_style pc_io_gradient_style(const pc_psd_desc *d)
{
    size_t n = 0;
    const uint8_t *e = pc_io_desc_enum(d, "Type", &n);
    if (enum_is(e, n, "Rdl ")) {
        return PC_GRADIENT_RADIAL;
    }
    if (enum_is(e, n, "Angl")) {
        return PC_GRADIENT_ANGLE;
    }
    if (enum_is(e, n, "Rflc")) {
        return PC_GRADIENT_REFLECTED;
    }
    if (enum_is(e, n, "Dmnd")) {
        return PC_GRADIENT_DIAMOND;
    }
    return PC_GRADIENT_LINEAR;
}

pc_status pc_io_gradient_style_value(pc_gradient_style s, pc_psd_value *out, pc_error *err)
{
    static const char *const v[5] = {"Lnr ", "Rdl ", "Angl", "Rflc", "Dmnd"};
    return pc_psd_value_enum("GrdT", v[(size_t)s < 5 ? (size_t)s : 0], out, err);
}

/* A Grdn descriptor's stops as stored (blocks::RawStops). */
typedef struct raw_stops {
    pc_color_stop *stops;
    size_t nstops;
    float *mids;
    size_t nmids;
    pc_opacity_stop *opacity;
    size_t nopacity;
    float smooth;
} raw_stops;

static void raw_stops_fini(raw_stops *r)
{
    free(r->stops);
    free(r->mids);
    free(r->opacity);
    memset(r, 0, sizeof *r);
}

static const pc_psd_value *list_of(const pc_psd_desc *d, const char *key)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    return v != NULL && v->kind == PC_PSD_V_LIST ? v : NULL;
}

/* Midpoint k applies to the segment after stop k (in location order); returns the sorted order
 * in *order (malloc'd). */
static pc_status sorted_order(const pc_color_stop *stops, size_t n, size_t **order, pc_error *err)
{
    size_t *o = pc_malloc_array(n > 0 ? n : 1, sizeof *o);
    float *keys = pc_malloc_array(n > 0 ? n : 1, sizeof *keys);
    if (o == NULL || keys == NULL) {
        free(o);
        free(keys);
        return pc_io_nomem(err, "sorting gradient stops");
    }
    for (size_t i = 0; i < n; i++) {
        o[i] = i;
        keys[i] = stops[i].pos;
    }
    if (!pc_io_sort_idx_f32(o, n, keys)) {
        free(o);
        free(keys);
        return pc_io_nomem(err, "sorting gradient stops");
    }
    free(keys);
    *order = o;
    return PC_OK;
}

static pc_status gradient_stops_raw(const pc_psd_desc *grad, raw_stops *out, pc_error *err)
{
    raw_stops r;
    const pc_psd_value *clrs = list_of(grad, "Clrs");
    const pc_psd_value *trns = list_of(grad, "Trns");
    size_t ncl = clrs != NULL ? clrs->nlist : 0;
    size_t ntr = trns != NULL ? trns->nlist : 0;
    float *mids_raw = NULL;
    size_t *order = NULL;
    pc_status st = PC_OK;
    memset(&r, 0, sizeof r);
    r.stops = pc_malloc_array(ncl > 0 ? ncl : 1, sizeof *r.stops);
    mids_raw = pc_malloc_array(ncl > 0 ? ncl : 1, sizeof *mids_raw);
    r.mids = pc_malloc_array(ncl > 0 ? ncl : 1, sizeof *r.mids);
    r.opacity = pc_malloc_array(ntr > 0 ? ntr : 1, sizeof *r.opacity);
    if (r.stops == NULL || mids_raw == NULL || r.mids == NULL || r.opacity == NULL) {
        st = pc_io_nomem(err, "reading gradient stops");
        goto fail;
    }
    for (size_t i = 0; i < ncl; i++) {
        const pc_psd_value *it = &clrs->list[i];
        const pc_psd_desc *c;
        pc_color col;
        if (it->kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        c = pc_io_desc_get(it->desc, "Clr ");
        if (c != NULL && pc_io_color_from_desc(c, &col)) {
            r.stops[r.nstops].pos = num_or(it->desc, "Lctn", 0.0) / 4096.0f;
            r.stops[r.nstops].color = col;
            mids_raw[r.nstops] = num_or(it->desc, "Mdpn", 50.0) / 100.0f;
            r.nstops++;
        }
    }
    TRY(sorted_order(r.stops, r.nstops, &order, err));
    for (size_t k = 1; k < r.nstops; k++) {
        r.mids[r.nmids++] = mids_raw[order[k]];
    }
    {
        double v;
        r.smooth = pc_io_desc_num(pc_psd_desc_get(grad, "Intr"), &v) ? pc_f64_to_f32(v / 4096.0) : 0.0f;
    }
    {
        pc_color_stop *sorted = pc_malloc_array(r.nstops > 0 ? r.nstops : 1, sizeof *sorted);
        if (sorted == NULL) {
            st = pc_io_nomem(err, "reading gradient stops");
            goto fail;
        }
        for (size_t k = 0; k < r.nstops; k++) {
            sorted[k] = r.stops[order[k]];
        }
        free(r.stops);
        r.stops = sorted;
    }
    for (size_t i = 0; i < ntr; i++) {
        const pc_psd_value *it = &trns->list[i];
        if (it->kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        r.opacity[r.nopacity].pos = num_or(it->desc, "Lctn", 0.0) / 4096.0f;
        r.opacity[r.nopacity].opacity = num_or(it->desc, "Opct", 100.0) / 100.0f;
        r.nopacity++;
    }
    free(mids_raw);
    free(order);
    *out = r;
    return PC_OK;
fail:
    free(mids_raw);
    free(order);
    raw_stops_fini(&r);
    return st;
}

/* gradient_bake::bake on pc_color_stops (replaces *stops). */
static pc_status bake(pc_color_stop **stops, size_t *n, const float *mids, size_t nmids, float smooth,
                      pc_io_bake_method m, pc_error *err)
{
    pc_io_color_stop *in = pc_malloc_array(*n > 0 ? *n : 1, sizeof *in);
    pc_io_color_stop *res = NULL;
    size_t nres = 0;
    pc_color_stop *out;
    pc_status st;
    if (in == NULL) {
        return pc_io_nomem(err, "baking a gradient");
    }
    for (size_t i = 0; i < *n; i++) {
        in[i].at = (*stops)[i].pos;
        in[i].color = (*stops)[i].color;
    }
    st = pc_io_gradient_bake(in, *n, mids, nmids, smooth, m, &res, &nres, err);
    free(in);
    if (st != PC_OK) {
        return st;
    }
    out = pc_malloc_array(nres > 0 ? nres : 1, sizeof *out);
    if (out == NULL) {
        free(res);
        return pc_io_nomem(err, "baking a gradient");
    }
    for (size_t i = 0; i < nres; i++) {
        out[i].pos = res[i].at;
        out[i].color = res[i].color;
    }
    free(res);
    free(*stops);
    *stops = out;
    *n = nres;
    return PC_OK;
}

const uint8_t *pc_io_desc_gradient_method(const pc_psd_desc *d, size_t *len)
{
    const uint8_t *m = pc_io_desc_enum(d, "gradientsInterpolationMethod", len);
    return m != NULL ? m : pc_io_desc_enum(d, "gs99", len);
}

pc_status pc_io_gradient_stops_with(const pc_psd_desc *grad, const uint8_t *method, size_t method_len,
                                    pc_color_stop **stops, size_t *nstops, pc_opacity_stop **opacity,
                                    size_t *nopacity, pc_error *err)
{
    raw_stops r;
    pc_status st = gradient_stops_raw(grad, &r, err);
    if (st != PC_OK) {
        return st;
    }
    if (r.nstops >= 2) {
        st = bake(&r.stops, &r.nstops, r.mids, r.nmids, r.smooth,
                  pc_io_bake_method_from_code(method, method_len), err);
        if (st != PC_OK) {
            raw_stops_fini(&r);
            return st;
        }
    }
    free(r.mids);
    if (r.nstops == 0) {
        free(r.stops);
        r.stops = NULL;
    }
    if (r.nopacity == 0) {
        free(r.opacity);
        r.opacity = NULL;
    }
    *stops = r.stops;
    *nstops = r.nstops;
    *opacity = r.opacity;
    *nopacity = r.nopacity;
    return PC_OK;
}

/* blocks::gradient_stops_editable: plain interpolation keeps the stops and midpoints, anything
 * else is baked (midpoints folded in); 0.5 midpoints and fully opaque stops come back empty. */
static pc_status gradient_stops_editable(const pc_psd_desc *grad, const uint8_t *method, size_t method_len,
                                         raw_stops *out, pc_error *err)
{
    raw_stops r;
    pc_io_bake_method m = pc_io_bake_method_from_code(method, method_len);
    bool lab = true;
    bool all_half = true;
    bool opaque = true;
    pc_status st = gradient_stops_raw(grad, &r, err);
    if (st != PC_OK) {
        return st;
    }
    /* Lab stops (Lab documents) interpolate in L*a*b*, which only baking reproduces. */
    for (size_t i = 0; i < r.nstops; i++) {
        lab = lab && r.stops[i].color.mode == PC_COLOR_MODE_LAB;
    }
    if (r.nstops >= 2 && (r.smooth > 0.0f || m != PC_IO_BAKE_CLASSIC || lab)) {
        st = bake(&r.stops, &r.nstops, r.mids, r.nmids, r.smooth, m, err);
        if (st != PC_OK) {
            raw_stops_fini(&r);
            return st;
        }
        r.nmids = 0;
    }
    for (size_t i = 0; i < r.nmids; i++) {
        all_half = all_half && fabsf(r.mids[i] - 0.5f) < 1e-3f;
    }
    if (all_half) {
        r.nmids = 0;
    }
    for (size_t i = 0; i < r.nopacity; i++) {
        opaque = opaque && fabsf(r.opacity[i].opacity - 1.0f) < 1e-4f;
    }
    if (opaque) {
        r.nopacity = 0;
    }
    *out = r;
    return PC_OK;
}

/* blocks::smoothed_opacity_stops: opacity stops with smoothness and midpoints applied (Classic). */
static pc_status smoothed_opacity_stops(const pc_psd_desc *grad, pc_opacity_stop **out, size_t *n,
                                        pc_error *err)
{
    const pc_psd_value *trns = list_of(grad, "Trns");
    size_t nt = trns != NULL ? trns->nlist : 0;
    pc_color_stop *stops = pc_malloc_array(nt > 0 ? nt : 1, sizeof *stops);
    float *mids_raw = pc_malloc_array(nt > 0 ? nt : 1, sizeof *mids_raw);
    float *mids = pc_malloc_array(nt > 0 ? nt : 1, sizeof *mids);
    size_t *order = NULL;
    size_t ns = 0;
    size_t nm = 0;
    float smooth = 0.0f;
    pc_opacity_stop *res;
    pc_status st = PC_OK;
    if (stops == NULL || mids_raw == NULL || mids == NULL) {
        st = pc_io_nomem(err, "reading gradient opacity");
        goto fail;
    }
    for (size_t i = 0; i < nt; i++) {
        const pc_psd_value *it = &trns->list[i];
        float a;
        if (it->kind != PC_PSD_V_DESCRIPTOR) {
            continue;
        }
        a = num_or(it->desc, "Opct", 100.0) / 100.0f;
        stops[ns].pos = num_or(it->desc, "Lctn", 0.0) / 4096.0f;
        stops[ns].color = pc_color_rgba(a, a, a, 1.0f);
        mids_raw[ns] = num_or(it->desc, "Mdpn", 50.0) / 100.0f;
        ns++;
    }
    TRY(sorted_order(stops, ns, &order, err));
    for (size_t k = 1; k < ns; k++) {
        mids[nm++] = mids_raw[order[k]];
    }
    {
        double v;
        smooth = pc_io_desc_num(pc_psd_desc_get(grad, "Intr"), &v) ? pc_f64_to_f32(v / 4096.0) : 0.0f;
    }
    if (ns >= 2) {
        TRY(bake(&stops, &ns, mids, nm, smooth, PC_IO_BAKE_CLASSIC, err));
    }
    res = pc_malloc_array(ns > 0 ? ns : 1, sizeof *res);
    if (res == NULL) {
        st = pc_io_nomem(err, "reading gradient opacity");
        goto fail;
    }
    for (size_t i = 0; i < ns; i++) {
        float rgb[3];
        pc_color_to_rgb(&stops[i].color, rgb);
        res[i].pos = stops[i].pos;
        res[i].opacity = rgb[0];
    }
    free(stops);
    free(mids_raw);
    free(mids);
    free(order);
    *out = res;
    *n = ns;
    return PC_OK;
fail:
    free(stops);
    free(mids_raw);
    free(mids);
    free(order);
    return st;
}

static pc_status add_value(pc_psd_desc *d, const char *key, pc_psd_value v, pc_error *err)
{
    return pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_enum(pc_psd_desc *d, const char *key, const char *type, const char *value, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_enum(type, value, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

/* Adds key -> Descriptor(*sub) (taken). */
static pc_status add_desc(pc_psd_desc *d, const char *key, pc_psd_desc *sub, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_object(sub, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

pc_status pc_io_gradient_desc(const pc_color_stop *stops_in, size_t nstops, const pc_opacity_stop *opacity,
                              size_t nopacity, const float *mids, size_t nmids, pc_psd_desc *out,
                              pc_error *err)
{
    pc_psd_desc g;
    pc_psd_value clrs;
    pc_psd_value trns;
    size_t *order = NULL;
    pc_status st;
    memset(&g, 0, sizeof g);
    memset(&clrs, 0, sizeof clrs);
    memset(&trns, 0, sizeof trns);
    clrs.kind = PC_PSD_V_LIST;
    trns.kind = PC_PSD_V_LIST;
    /* With midpoints the stops are sorted first (each midpoint belongs to a sorted segment). */
    if (nmids > 0) {
        TRY(sorted_order(stops_in, nstops, &order, err));
    }
    for (size_t i = 0; i < nstops; i++) {
        const pc_color_stop *s = &stops_in[order != NULL ? order[i] : i];
        float mid = i >= 1 && i - 1 < nmids ? mids[i - 1] : 0.5f;
        pc_psd_desc c;
        pc_psd_desc col;
        pc_psd_value item;
        TRY(pc_psd_desc_init(&c, "Clrt", err));
        st = pc_io_color_to_desc(&s->color, &col, err);
        st = st != PC_OK ? st : add_desc(&c, "Clr ", &col, err);
        st = st != PC_OK ? st : add_enum(&c, "Type", "Clry", "UsrS", err);
        st = st != PC_OK
                 ? st
                 : add_value(&c, "Lctn", pc_psd_value_int(pc_f32_to_i32(roundf(s->pos * 4096.0f))), err);
        st = st != PC_OK
                 ? st
                 : add_value(&c, "Mdpn",
                             pc_psd_value_int(pc_f32_to_i32(roundf(pc_clampf(mid, 0.05f, 0.95f) * 100.0f))),
                             err);
        st = st != PC_OK ? st : pc_psd_value_object(&c, &item, err);
        if (st != PC_OK) {
            pc_psd_desc_fini(&c);
            goto fail;
        }
        TRY(pc_psd_value_list_push(&clrs, &item, err));
    }
    {
        size_t nop = nopacity > 0 ? nopacity : nstops;
        for (size_t i = 0; i < nop; i++) {
            float t;
            float a;
            pc_psd_desc c;
            pc_psd_value item;
            if (nopacity > 0) {
                t = opacity[i].pos;
                a = opacity[i].opacity;
            } else {
                const pc_color_stop *s = &stops_in[order != NULL ? order[i] : i];
                t = s->pos;
                a = s->color.alpha;
            }
            TRY(pc_psd_desc_init(&c, "TrnS", err));
            st = add_value(&c, "Opct", pc_io_unit_float("#Prc", (double)(a * 100.0f)), err);
            st = st != PC_OK
                     ? st
                     : add_value(&c, "Lctn", pc_psd_value_int(pc_f32_to_i32(roundf(t * 4096.0f))), err);
            st = st != PC_OK ? st : add_value(&c, "Mdpn", pc_psd_value_int(50), err);
            st = st != PC_OK ? st : pc_psd_value_object(&c, &item, err);
            if (st != PC_OK) {
                pc_psd_desc_fini(&c);
                goto fail;
            }
            TRY(pc_psd_value_list_push(&trns, &item, err));
        }
    }
    TRY(pc_psd_desc_init(&g, "Grdn", err));
    {
        pc_psd_value nm;
        TRY(pc_io_text_value("Custom", 6, &nm, err));
        TRY(pc_psd_desc_add(&g, "Nm  ", &nm, err));
    }
    TRY(add_enum(&g, "GrdF", "GrdF", "CstS", err));
    /* Our stops interpolate linearly (Photoshop smoothness is baked into them on import). */
    TRY(add_value(&g, "Intr", pc_psd_value_double(0.0), err));
    TRY(pc_psd_desc_add(&g, "Clrs", &clrs, err));
    TRY(pc_psd_desc_add(&g, "Trns", &trns, err));
    free(order);
    *out = g;
    return PC_OK;
fail:
    free(order);
    pc_psd_value_fini(&clrs);
    pc_psd_value_fini(&trns);
    pc_psd_desc_fini(&g);
    return st;
}

/* ---- patterns ---- */

pc_status pc_io_pattern_ref(const pc_psd_desc *d, pc_str *name, pc_str *id, pc_error *err)
{
    const pc_psd_desc *p = pc_io_desc_get(d, "Ptrn");
    pc_str n = PC_STR_INIT;
    pc_str i = PC_STR_INIT;
    bool found;
    pc_status st = pc_io_desc_text(p, "Nm  ", &found, &n, err);
    if (st == PC_OK) {
        st = pc_io_desc_text(p, "Idnt", &found, &i, err);
    }
    if (st != PC_OK) {
        pc_str_fini(&n);
        pc_str_fini(&i);
        return st;
    }
    pc_str_fini(name);
    pc_str_fini(id);
    *name = n;
    *id = i;
    return PC_OK;
}

pc_status pc_io_pattern_ref_desc(const char *name, size_t name_len, const char *id, size_t id_len,
                                 pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc p;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&p, "Ptrn", err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_io_text_value(name, name_len, &v, err);
    st = st != PC_OK ? st : pc_psd_desc_add(&p, "Nm  ", &v, err);
    /* The id is omitted when unknown. */
    if (st == PC_OK && id_len > 0) {
        st = pc_io_text_value(id, id_len, &v, err);
        st = st != PC_OK ? st : pc_psd_desc_add(&p, "Idnt", &v, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&p);
        return st;
    }
    *out = p;
    return PC_OK;
}

void pc_io_pattern_placement(const pc_psd_desc *d, float *angle, bool *link, float phase[2])
{
    const pc_psd_value *al = pc_psd_desc_get(d, "Algn");
    const pc_psd_desc *ph = pc_io_desc_get(d, "phase");
    *angle = num_or(d, "Angl", 0.0);
    *link = !(al != NULL && al->kind == PC_PSD_V_BOOLEAN && !al->b);
    phase[0] = ph != NULL ? num_or(ph, "Hrzn", 0.0) : 0.0f;
    phase[1] = ph != NULL ? num_or(ph, "Vrtc", 0.0) : 0.0f;
}

pc_status pc_io_with_pattern_placement(pc_psd_desc *d, float angle, bool link, const float phase[2],
                                       pc_error *err)
{
    pc_psd_desc pnt;
    pc_status st = PC_OK;
    if (angle != 0.0f) {
        st = add_value(d, "Angl", pc_io_unit_float("#Ang", (double)angle), err);
    }
    st = st != PC_OK ? st : add_value(d, "Algn", pc_psd_value_bool(link), err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_desc_init(&pnt, "Pnt ", err);
    if (st != PC_OK) {
        return st;
    }
    st = add_value(&pnt, "Hrzn", pc_psd_value_double((double)phase[0]), err);
    st = st != PC_OK ? st : add_value(&pnt, "Vrtc", pc_psd_value_double((double)phase[1]), err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&pnt);
        return st;
    }
    return add_desc(d, "phase", &pnt, err);
}

/* ---- fills ---- */

pc_status pc_io_parse_fill(const uint8_t key[4], const uint8_t *data, size_t len, bool *found, pc_fill *out,
                           pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    /* Block data may carry trailing padding after the descriptor. */
    pc_status st = pc_io_parse_prefix_versioned(data, len, &ok, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (!ok) {
        *found = false;
        return PC_OK;
    }
    st = pc_io_fill_from_desc(key, &d, found, out, err);
    pc_psd_desc_fini(&d);
    return st;
}

static float pct0(const pc_psd_desc *o, const char *k)
{
    double v;
    return pc_io_desc_num(pc_psd_desc_get(o, k), &v) ? pc_f64_to_f32(v) / 100.0f : 0.0f;
}

static pc_status gradient_fill_from_desc(const pc_psd_desc *d, pc_fill *out, pc_error *err)
{
    pc_fill f;
    raw_stops raw;
    const pc_psd_desc *grad = pc_io_desc_get(d, "Grad");
    const pc_psd_desc *ofs = pc_io_desc_get(d, "Ofst");
    const pc_psd_value *al = pc_psd_desc_get(d, "Algn");
    pc_opacity_stop *op = NULL;
    size_t nop = 0;
    bool opaque = true;
    double v;
    pc_status st = PC_OK;
    memset(&f, 0, sizeof f);
    memset(&raw, 0, sizeof raw);
    f.kind = PC_FILL_GRADIENT;
    f.u.gradient.angle = num_or(d, "Angl", 90.0);
    f.u.gradient.scale = pc_io_desc_num(pc_psd_desc_get(d, "Scl "), &v) ? pc_f64_to_f32(v) / 100.0f : 1.0f;
    if (grad != NULL) {
        size_t n = 0;
        const uint8_t *m = pc_io_desc_gradient_method(d, &n);
        st = gradient_stops_editable(grad, m, n, &raw, err);
        /* Opacity stops with Photoshop's smoothness and opacity midpoints applied; none when
         * fully opaque. */
        st = st != PC_OK ? st : smoothed_opacity_stops(grad, &op, &nop, err);
        if (st != PC_OK) {
            raw_stops_fini(&raw);
            return st;
        }
    }
    free(raw.opacity);
    raw.opacity = NULL;
    for (size_t i = 0; i < nop; i++) {
        opaque = opaque && op[i].opacity >= 1.0f - 1e-4f;
    }
    if (opaque) {
        free(op);
        op = NULL;
        nop = 0;
    }
    if (raw.nstops == 0) {
        pc_color_stop *s = pc_malloc_array(2, sizeof *s);
        if (s == NULL) {
            free(op);
            raw_stops_fini(&raw);
            return pc_io_nomem(err, "reading a gradient fill");
        }
        s[0] = (pc_color_stop){0.0f, PC_COLOR_BLACK};
        s[1] = (pc_color_stop){1.0f, PC_COLOR_WHITE};
        free(raw.stops);
        raw.stops = s;
        raw.nstops = 2;
    }
    if (raw.nmids == 0) {
        free(raw.mids);
        raw.mids = NULL;
    }
    f.u.gradient.stops = raw.stops;
    f.u.gradient.nstops = raw.nstops;
    f.u.gradient.midpoints = raw.mids;
    f.u.gradient.nmidpoints = raw.nmids;
    f.u.gradient.opacity_stops = op;
    f.u.gradient.nopacity_stops = nop;
    f.u.gradient.offset[0] = ofs != NULL ? pct0(ofs, "Hrzn") : 0.0f;
    f.u.gradient.offset[1] = ofs != NULL ? pct0(ofs, "Vrtc") : 0.0f;
    f.u.gradient.style = pc_io_gradient_style(d);
    f.u.gradient.reverse = pc_io_desc_bool(d, "Rvrs");
    f.u.gradient.dither = pc_io_desc_bool(d, "Dthr");
    f.u.gradient.align = !(al != NULL && al->kind == PC_PSD_V_BOOLEAN && !al->b);
    *out = f;
    return PC_OK;
}

pc_status pc_io_fill_from_desc(const uint8_t key[4], const pc_psd_desc *d, bool *found, pc_fill *out,
                               pc_error *err)
{
    if (memcmp(key, "SoCo", 4) == 0) {
        const pc_psd_desc *c = pc_io_desc_get(d, "Clr ");
        pc_color col;
        if (c == NULL || !pc_io_color_from_desc(c, &col)) {
            *found = false;
            return PC_OK;
        }
        *out = pc_fill_solid(col);
        *found = true;
        return PC_OK;
    }
    if (memcmp(key, "GdFl", 4) == 0) {
        pc_status st = gradient_fill_from_desc(d, out, err);
        if (st == PC_OK) {
            *found = true;
        }
        return st;
    }
    if (memcmp(key, "PtFl", 4) == 0) {
        pc_fill f;
        double v;
        pc_status st;
        memset(&f, 0, sizeof f);
        f.kind = PC_FILL_PATTERN;
        st = pc_io_pattern_ref(d, &f.u.pattern.name, &f.u.pattern.id, err);
        if (st != PC_OK) {
            return st;
        }
        f.u.pattern.scale = pc_io_desc_num(pc_psd_desc_get(d, "Scl "), &v) ? pc_f64_to_f32(v) / 100.0f : 1.0f;
        pc_io_pattern_placement(d, &f.u.pattern.angle, &f.u.pattern.link, f.u.pattern.phase);
        *out = f;
        *found = true;
        return PC_OK;
    }
    *found = false;
    return PC_OK;
}

static pc_psd_value prc(float v)
{
    return pc_io_unit_float("#Prc", (double)(v * 100.0f));
}

pc_status pc_io_fill_to_desc(const pc_fill *f, uint8_t key[4], pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_status st;
    const char *k;
    memset(&d, 0, sizeof d);
    TRY(pc_psd_desc_init(&d, "null", err));
    switch (f->kind) {
    case PC_FILL_SOLID: {
        pc_psd_desc c;
        k = "SoCo";
        TRY(pc_io_color_to_desc(&f->u.solid, &c, err));
        TRY(add_desc(&d, "Clr ", &c, err));
        break;
    }
    case PC_FILL_GRADIENT: {
        pc_psd_value style;
        pc_psd_desc g;
        k = "GdFl";
        TRY(add_value(&d, "Angl", pc_io_unit_float("#Ang", (double)f->u.gradient.angle), err));
        TRY(pc_io_gradient_style_value(f->u.gradient.style, &style, err));
        TRY(pc_psd_desc_add(&d, "Type", &style, err));
        TRY(add_value(&d, "Scl ", prc(f->u.gradient.scale), err));
        TRY(pc_io_gradient_desc(f->u.gradient.stops, f->u.gradient.nstops, f->u.gradient.opacity_stops,
                                f->u.gradient.nopacity_stops, f->u.gradient.midpoints,
                                f->u.gradient.nmidpoints, &g, err));
        TRY(add_desc(&d, "Grad", &g, err));
        if (f->u.gradient.reverse) {
            TRY(add_value(&d, "Rvrs", pc_psd_value_bool(true), err));
        }
        if (f->u.gradient.dither) {
            TRY(add_value(&d, "Dthr", pc_psd_value_bool(true), err));
        }
        if (!f->u.gradient.align) {
            TRY(add_value(&d, "Algn", pc_psd_value_bool(false), err));
        }
        if (f->u.gradient.offset[0] != 0.0f || f->u.gradient.offset[1] != 0.0f) {
            pc_psd_desc p;
            TRY(pc_psd_desc_init(&p, "Pnt ", err));
            st = add_value(&p, "Hrzn", prc(f->u.gradient.offset[0]), err);
            st = st != PC_OK ? st : add_value(&p, "Vrtc", prc(f->u.gradient.offset[1]), err);
            if (st != PC_OK) {
                pc_psd_desc_fini(&p);
                goto fail;
            }
            TRY(add_desc(&d, "Ofst", &p, err));
        }
        break;
    }
    case PC_FILL_PATTERN:
    default: {
        pc_psd_desc p;
        k = "PtFl";
        TRY(add_value(&d, "Scl ", pc_io_unit_float("#Prc", (double)(f->u.pattern.scale * 100.0f)), err));
        TRY(pc_io_pattern_ref_desc(f->u.pattern.name.ptr, f->u.pattern.name.len, f->u.pattern.id.ptr,
                                   f->u.pattern.id.len, &p, err));
        TRY(add_desc(&d, "Ptrn", &p, err));
        TRY(pc_io_with_pattern_placement(&d, f->u.pattern.angle, f->u.pattern.link, f->u.pattern.phase, err));
        break;
    }
    }
    memcpy(key, k, 4);
    *out = d;
    return PC_OK;
fail:
    pc_psd_desc_fini(&d);
    return st;
}

pc_status pc_io_write_fill(const pc_fill *f, uint8_t key[4], pc_buf *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_bytes b = {NULL, 0};
    uint8_t k[4];
    pc_status st = pc_io_fill_to_desc(f, k, &d, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_vdesc_to_bytes(&d, &b, err);
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        return st;
    }
    {
        pc_buf nb = PC_BUF_INIT;
        st = pc_buf_append(&nb, b.data, b.len, err);
        pc_psd_bytes_fini(&b);
        if (st != PC_OK) {
            return st;
        }
        pc_buf_fini(out);
        *out = nb;
    }
    memcpy(key, k, 4);
    return PC_OK;
}

/* ---- type and smart-object blocks ---- */

static double be_f64(const uint8_t *p)
{
    uint64_t u = 0;
    double v;
    for (size_t i = 0; i < 8; i++) {
        u = u << 8 | p[i];
    }
    memcpy(&v, &u, 8);
    return v;
}

pc_status pc_io_parse_tysh(const uint8_t *data, size_t len, bool *found, pc_str *text, pc_affine *transform,
                           pc_error *err)
{
    pc_affine a;
    pc_str t = PC_STR_INIT;
    pc_psd_desc d;
    bool ok = false;
    pc_status st;
    if (data == NULL || len < 52) {
        *found = false;
        return PC_OK;
    }
    for (size_t i = 0; i < 6; i++) {
        a.m[i] = be_f64(data + 2 + i * 8);
    }
    /* text version (2) at 50, descriptor version (4) at 52, descriptor at 56. */
    st = pc_io_parse_prefix_versioned(data + 52, len - 52, &ok, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (ok) {
        bool has = false;
        st = pc_io_desc_text(&d, "Txt ", &has, &t, err);
        pc_psd_desc_fini(&d);
        if (st != PC_OK) {
            pc_str_fini(&t);
            return st;
        }
    }
    pc_str_fini(text);
    *text = t;
    *transform = a;
    *found = true;
    return PC_OK;
}

static bool smart_key(const uint8_t key[4])
{
    return memcmp(key, "SoLd", 4) == 0 || memcmp(key, "SoLE", 4) == 0;
}

/* The soLD descriptor after the 8-byte 'soLD' + version header; *found false when unreadable. */
static pc_status sold_desc(const uint8_t key[4], const uint8_t *data, size_t len, bool *found,
                           pc_psd_desc *out, pc_error *err)
{
    if (!smart_key(key) || data == NULL || len < 8) {
        *found = false;
        return PC_OK;
    }
    return pc_io_parse_prefix_versioned(data + 8, len - 8, found, out, err);
}

/* num() of every list item, in order (non-numbers skipped). *out malloc'd. */
static pc_status list_nums(const pc_psd_value *list, double **out, size_t *n, pc_error *err)
{
    double *v = pc_malloc_array(list->nlist > 0 ? list->nlist : 1, sizeof *v);
    size_t k = 0;
    if (v == NULL) {
        return pc_io_nomem(err, "reading a smart-object transform");
    }
    for (size_t i = 0; i < list->nlist; i++) {
        if (pc_io_desc_num(&list->list[i], &v[k])) {
            k++;
        }
    }
    *out = v;
    *n = k;
    return PC_OK;
}

void pc_io_placed_frame(const pc_warp *warp, double w, double h, double out[4])
{
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = w;
    out[3] = h;
    if (warp == NULL || warp->style != PC_WARP_CUSTOM || !warp->has_mesh || warp->mesh.npoints != 16) {
        return;
    }
    {
        double b[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
        bool ok;
        for (size_t i = 0; i < 16; i++) {
            double x = warp->mesh.points[i].x;
            double y = warp->mesh.points[i].y;
            b[0] = x < b[0] ? x : b[0];
            b[1] = y < b[1] ? y : b[1];
            b[2] = x > b[2] ? x : b[2];
            b[3] = y > b[3] ? y : b[3];
        }
        ok = isfinite(b[0]) && isfinite(b[1]) && isfinite(b[2]) && isfinite(b[3]) && b[2] - b[0] > 1e-9 &&
             b[3] - b[1] > 1e-9;
        if (ok) {
            memcpy(out, b, sizeof b);
        }
    }
}

bool pc_io_corners_affine(const double p[8], const double frame[4], pc_affine *out)
{
    double x0 = frame[0];
    double y0 = frame[1];
    double bw = frame[2] - frame[0];
    double bh = frame[3] - frame[1];
    double a;
    double b;
    double c;
    double d;
    pc_affine m;
    bool finite = isfinite(frame[0]) && isfinite(frame[1]) && isfinite(frame[2]) && isfinite(frame[3]);
    for (size_t i = 0; i < 8; i++) {
        finite = finite && isfinite(p[i]);
    }
    if (!(finite && bw > 0.0 && bh > 0.0)) {
        return false;
    }
    a = (p[2] - p[0]) / bw;
    b = (p[3] - p[1]) / bw;
    c = (p[6] - p[0]) / bh;
    d = (p[7] - p[1]) / bh;
    m = (pc_affine){{a, b, c, d, p[0] - a * x0 - c * y0, p[1] - b * x0 - d * y0}};
    for (size_t i = 0; i < 6; i++) {
        if (!isfinite(m.m[i])) {
            return false;
        }
    }
    *out = m;
    return true;
}

pc_status pc_io_parse_smart(const uint8_t key[4], const uint8_t *data, size_t len, pc_str *idnt,
                            pc_affine *transform, pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    pc_str id = PC_STR_INIT;
    pc_affine a = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    pc_status st = sold_desc(key, data, len, &ok, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (ok) {
        bool has = false;
        const pc_psd_value *trnf = list_of(&d, "Trnf");
        const pc_psd_desc *sz = pc_io_desc_get(&d, "Sz  ");
        st = pc_io_desc_text(&d, "Idnt", &has, &id, err);
        if (st == PC_OK && trnf != NULL && sz != NULL) {
            double *p = NULL;
            size_t n = 0;
            double w = 0.0;
            double h = 0.0;
            (void)pc_io_desc_num(pc_psd_desc_get(sz, "Wdth"), &w);
            (void)pc_io_desc_num(pc_psd_desc_get(sz, "Hght"), &h);
            st = list_nums(trnf, &p, &n, err);
            if (st == PC_OK && n == 8 && w > 0.0 && h > 0.0) {
                pc_warp warp;
                bool has_warp = false;
                double frame[4];
                pc_status ws = pc_io_parse_placed_warp(key, data, len, &has_warp, &warp, err);
                if (ws != PC_OK) {
                    free(p);
                    pc_psd_desc_fini(&d);
                    pc_str_fini(&id);
                    return ws;
                }
                pc_io_placed_frame(has_warp ? &warp : NULL, w, h, frame);
                if (has_warp) {
                    pc_warp_fini(&warp);
                }
                /* A non-finite or empty result leaves the identity transform. */
                (void)pc_io_corners_affine(p, frame, &a);
            }
            free(p);
        }
        pc_psd_desc_fini(&d);
        if (st != PC_OK) {
            pc_str_fini(&id);
            return st;
        }
    }
    pc_str_fini(idnt);
    *idnt = id;
    *transform = a;
    return PC_OK;
}

/* A list of exactly 8 finite numbers as 4 corners. */
static pc_status quad_of(const pc_psd_desc *d, const char *key, bool *ok, double q[4][2], pc_error *err)
{
    const pc_psd_value *l = list_of(d, key);
    double *p = NULL;
    size_t n = 0;
    pc_status st;
    *ok = false;
    if (l == NULL) {
        return PC_OK;
    }
    st = list_nums(l, &p, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n == 8) {
        bool finite = true;
        for (size_t i = 0; i < 8; i++) {
            finite = finite && isfinite(p[i]);
        }
        if (finite) {
            for (size_t i = 0; i < 4; i++) {
                q[i][0] = p[2 * i];
                q[i][1] = p[2 * i + 1];
            }
            *ok = true;
        }
    }
    free(p);
    return PC_OK;
}

pc_status pc_io_parse_smart_perspective(const uint8_t key[4], const uint8_t *data, size_t len, bool *found,
                                        double out[9], pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    const pc_psd_desc *sz;
    double w;
    double h;
    double q[4][2];
    pc_status st = sold_desc(key, data, len, &ok, &d, err);
    *found = false;
    if (st != PC_OK || !ok) {
        return st;
    }
    sz = pc_io_desc_get(&d, "Sz  ");
    if (sz == NULL || !pc_io_desc_num(pc_psd_desc_get(sz, "Wdth"), &w) ||
        !pc_io_desc_num(pc_psd_desc_get(sz, "Hght"), &h)) {
        pc_psd_desc_fini(&d);
        return PC_OK;
    }
    st = quad_of(&d, "nonAffineTransform", &ok, q, err);
    if (st == PC_OK && !ok) {
        st = quad_of(&d, "Trnf", &ok, q, err);
    }
    pc_psd_desc_fini(&d);
    if (st != PC_OK || !ok) {
        return st;
    }
    return pc_io_corners_perspective((const double (*)[2])q, w, h, found, out, err);
}

pc_status pc_io_corners_perspective(const double q[4][2], double w, double h, bool *found, double out[9],
                                    pc_error *err)
{
    /* A parallelogram (top-left + bottom-right = top-right + bottom-left) is affine. */
    double m = 0.0;
    double tol;
    bool affine = true;
    pc_algo_homography hm;
    double r[4] = {0.0, 0.0, w, h};
    (void)err;
    *found = false;
    for (size_t i = 0; i < 4; i++) {
        m = pc_rs_max(m, fabs(q[i][0]));
        m = pc_rs_max(m, fabs(q[i][1]));
    }
    tol = 1e-6 * (1.0 + m);
    for (size_t i = 0; i < 2; i++) {
        affine = affine && fabs(q[0][i] + q[2][i] - q[1][i] - q[3][i]) <= tol;
    }
    if (affine || w <= 0.0 || h <= 0.0) {
        return PC_OK;
    }
    if (pc_algo_homography_rect_to_quad(r, q, &hm)) {
        memcpy(out, hm.m, sizeof hm.m);
        *found = true;
    }
    return PC_OK;
}

/* UnitFloats values under key of an object array body, or NULL. */
static const pc_psd_value *unit_floats(const pc_psd_desc *body, const char *key)
{
    const pc_psd_value *v = pc_psd_desc_get(body, key);
    return v != NULL && v->kind == PC_PSD_V_UNIT_FLOATS ? v : NULL;
}

/* The custom envelope mesh (16 points); *ok false when absent or invalid. */
static pc_status custom_mesh(const pc_psd_desc *w, bool *ok, pc_mesh *out, pc_error *err)
{
    const pc_psd_desc *env = pc_io_desc_get(w, "customEnvelopeWarp");
    const pc_psd_value *arr = env != NULL ? pc_psd_desc_get(env, "meshPoints") : NULL;
    const pc_psd_value *xs;
    const pc_psd_value *ys;
    pc_mesh m;
    *ok = false;
    if (arr == NULL || arr->kind != PC_PSD_V_OBJECT_ARRAY || arr->desc == NULL) {
        return PC_OK;
    }
    xs = unit_floats(arr->desc, "Hrzn");
    ys = unit_floats(arr->desc, "Vrtc");
    if (xs == NULL || ys == NULL || xs->nfloats != 16 || ys->nfloats != 16) {
        return PC_OK;
    }
    memset(&m, 0, sizeof m);
    m.us = pc_malloc_array(2, sizeof *m.us);
    m.vs = pc_malloc_array(2, sizeof *m.vs);
    m.points = pc_malloc_array(16, sizeof *m.points);
    if (m.us == NULL || m.vs == NULL || m.points == NULL) {
        pc_mesh_fini(&m);
        return pc_io_nomem(err, "reading a placed-layer warp");
    }
    m.us[0] = 0.0;
    m.us[1] = 1.0;
    m.vs[0] = 0.0;
    m.vs[1] = 1.0;
    m.nus = 2;
    m.nvs = 2;
    for (size_t i = 0; i < 16; i++) {
        m.points[i] = (pc_point){xs->floats[i], ys->floats[i]};
    }
    m.npoints = 16;
    if (!pc_mesh_is_valid(&m)) {
        pc_mesh_fini(&m);
        return PC_OK;
    }
    *out = m;
    *ok = true;
    return PC_OK;
}

/* A `warp` descriptor as a warp. kind: NONE for no warp (warpNone, or one that moves nothing),
 * SOME for a warp (in *out), UNREADABLE when it can't be read (an unknown style, missing bounds,
 * a mesh that isn't 4x4). Mirrors crates/io blocks::warp_of. */
static pc_status warp_of_desc(const pc_psd_desc *w, pc_io_plld_warp_kind *kind, pc_warp *out, pc_error *err)
{
    const uint8_t *e;
    const pc_psd_desc *b;
    size_t en = 0;
    pc_warp_style style;
    double bounds[4];
    double v;
    pc_warp warp;
    pc_status st;
    *kind = PC_IO_PLLD_WARP_UNREADABLE;
    e = w != NULL ? pc_io_desc_enum(w, "warpStyle", &en) : NULL;
    if (e == NULL || !pc_warp_style_parse((const char *)e, en, &style)) {
        return PC_OK;
    }
    if (style == PC_WARP_NONE) {
        *kind = PC_IO_PLLD_WARP_NONE;
        return PC_OK;
    }
    b = pc_io_desc_get(w, "bounds");
    if (b == NULL || !pc_io_desc_num(pc_psd_desc_get(b, "Left"), &bounds[0]) ||
        !pc_io_desc_num(pc_psd_desc_get(b, "Top "), &bounds[1]) ||
        !pc_io_desc_num(pc_psd_desc_get(b, "Rght"), &bounds[2]) ||
        !pc_io_desc_num(pc_psd_desc_get(b, "Btom"), &bounds[3])) {
        return PC_OK;
    }
    v = 0.0;
    (void)pc_io_desc_num(pc_psd_desc_get(w, "warpValue"), &v);
    pc_warp_preset(&warp, style, v, bounds);
    v = 0.0;
    (void)pc_io_desc_num(pc_psd_desc_get(w, "warpPerspective"), &v);
    warp.h_distort = v;
    v = 0.0;
    (void)pc_io_desc_num(pc_psd_desc_get(w, "warpPerspectiveOther"), &v);
    warp.v_distort = v;
    e = pc_io_desc_enum(w, "warpRotate", &en);
    warp.vertical = enum_is(e, en, "Vrtc");
    if (style == PC_WARP_CUSTOM) {
        bool ok = false;
        pc_mesh m;
        st = custom_mesh(w, &ok, &m, err);
        if (st != PC_OK) {
            pc_warp_fini(&warp);
            return st;
        }
        if (!ok) {
            pc_warp_fini(&warp); /* a mesh that isn't 4x4: unreadable */
            return PC_OK;
        }
        warp.has_mesh = true;
        warp.mesh = m;
    }
    if (pc_warp_is_identity(&warp)) {
        pc_warp_fini(&warp);
        *kind = PC_IO_PLLD_WARP_NONE;
        return PC_OK;
    }
    *out = warp;
    *kind = PC_IO_PLLD_WARP_SOME;
    return PC_OK;
}

pc_status pc_io_parse_placed_warp(const uint8_t key[4], const uint8_t *data, size_t len, bool *found,
                                  pc_warp *out, pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    pc_io_plld_warp_kind kind;
    pc_status st = sold_desc(key, data, len, &ok, &d, err);
    *found = false;
    if (st != PC_OK || !ok) {
        return st;
    }
    st = warp_of_desc(pc_io_desc_get(&d, "warp"), &kind, out, err);
    pc_psd_desc_fini(&d);
    *found = st == PC_OK && kind == PC_IO_PLLD_WARP_SOME;
    return st;
}

/* The record layout of a PlLd block: magic 'plcL', version, Pascal id (unpadded), 4 x u32 (page,
 * total pages, anti-alias policy, layer type), then 8 doubles (the corners). *found false for a
 * non-plcL, truncated or non-finite record. */
static pc_status plld_record(const uint8_t *data, size_t len, bool *found, pc_str *idnt, double corners[8],
                             size_t *warp_at, pc_error *err)
{
    size_t n;
    size_t at;
    pc_buf id = PC_BUF_INIT;
    pc_status st;
    *found = false;
    if (data == NULL || len < 9 || memcmp(data, "plcL", 4) != 0) {
        return PC_OK;
    }
    n = data[8];
    if (n > len - 9) {
        return PC_OK;
    }
    at = 9 + n + 16;
    if (at > len || 64 > len - at) {
        return PC_OK;
    }
    for (size_t i = 0; i < 8; i++) {
        uint64_t bits = pc_load_be64(data + at + i * 8);
        memcpy(&corners[i], &bits, 8);
        if (!isfinite(corners[i])) {
            return PC_OK;
        }
    }
    st = pc_utf8_lossy(data + 9, n, &id, err);
    if (st != PC_OK) {
        pc_buf_fini(&id);
        return st;
    }
    pc_str_fini(idnt);
    st = pc_str_set(idnt, (const char *)id.data, id.len, err);
    pc_buf_fini(&id);
    if (st != PC_OK) {
        return st;
    }
    *warp_at = at + 64;
    *found = true;
    return PC_OK;
}

pc_status pc_io_parse_plld(const uint8_t *data, size_t len, bool *found, pc_str *idnt, double corners[8],
                           size_t *warp_at, pc_error *err)
{
    return plld_record(data, len, found, idnt, corners, warp_at, err);
}

pc_status pc_io_parse_plld_warp(const uint8_t *data, size_t len, pc_io_plld_warp_kind *kind, pc_warp *out,
                                pc_error *err)
{
    pc_str id = PC_STR_INIT;
    double corners[8];
    size_t end = 0;
    bool found = false;
    bool all_zero = true;
    bool ok = false;
    pc_psd_desc d;
    pc_status st = plld_record(data, len, &found, &id, corners, &end, err);
    pc_str_fini(&id);
    *kind = PC_IO_PLLD_WARP_UNREADABLE;
    if (st != PC_OK) {
        return st;
    }
    if (!found || end > len) {
        return PC_OK; /* the record itself can't be read */
    }
    for (size_t i = end; i < len; i++) {
        all_zero = all_zero && data[i] == 0;
    }
    if (all_zero) {
        *kind = PC_IO_PLLD_WARP_NONE; /* only padding after the corners */
        return PC_OK;
    }
    /* A u32 warp version, then a versioned warp descriptor. */
    if (len - end < 4) {
        return PC_OK;
    }
    st = pc_io_parse_prefix_versioned(data + end + 4, len - end - 4, &ok, &d, err);
    if (st != PC_OK) {
        return st;
    }
    if (!ok) {
        return PC_OK; /* unreadable */
    }
    st = warp_of_desc(&d, kind, out, err);
    pc_psd_desc_fini(&d);
    return st;
}

void pc_io_plld_transform(const double p[8], bool has_size, double w, double h, const pc_warp *warp,
                          pc_affine *transform, bool *has_persp, double persp[9])
{
    double q[4][2];
    bool found = false;
    double frame[4];
    if (!has_size) {
        w = hypot(p[2] - p[0], p[3] - p[1]);
        h = hypot(p[6] - p[0], p[7] - p[1]);
    }
    for (size_t i = 0; i < 4; i++) {
        q[i][0] = p[2 * i];
        q[i][1] = p[2 * i + 1];
    }
    (void)pc_io_corners_perspective((const double (*)[2])q, w, h, &found, persp, NULL);
    *has_persp = found;
    if (found) {
        frame[0] = 0.0;
        frame[1] = 0.0;
        frame[2] = w;
        frame[3] = h;
    } else {
        pc_io_placed_frame(warp, w, h, frame);
    }
    *transform = (pc_affine){{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};
    (void)pc_io_corners_affine(p, frame, transform);
}
