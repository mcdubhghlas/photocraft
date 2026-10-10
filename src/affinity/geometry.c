/* Outlines of curves (PCvD node lists), parametric shapes (Shpe objects) and compounds, in node
 * space (port of crates/affinity/src/geometry.rs). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/mem.h"
#include "base/rmath.h"

/* Nodes per curve object. */
#define MAX_CURVE_NODES 4000000u
/* Nested compounds followed (the reference recurses without a bound; a cycle overflowed its stack). */
#define MAX_COMPOUND_DEPTH 256

typedef struct anchor {
    pc_af_point p;
    bool has_in;
    pc_af_point inh;
    bool has_out;
    pc_af_point out;
    size_t record;
} anchor;

static bool record(const pc_af_value *v, pc_af_point *p, uint8_t *role)
{
    uint64_t ux = 0;
    uint64_t uy = 0;
    int i;
    double x;
    double y;
    if (v->kind != PC_AF_BYTES || v->as.bytes.n != 18) {
        return false;
    }
    for (i = 7; i >= 0; i--) {
        ux = ux << 8 | v->as.bytes.v[i];
        uy = uy << 8 | v->as.bytes.v[8 + i];
    }
    memcpy(&x, &ux, 8);
    memcpy(&y, &uy, 8);
    if (!isfinite(x) || !isfinite(y)) {
        return false;
    }
    p->x = x;
    p->y = y;
    *role = v->as.bytes.v[17];
    return true;
}

/* Anchors of a record array; false for an unreadable record. *out malloc'd. */
static bool anchors(const pc_af_value *records, anchor **out, size_t *n, bool *nomem)
{
    size_t cnt = records->as.arr.n;
    anchor *v = pc_malloc_array(cnt / 3 + 1 > cnt ? cnt / 3 + 1 : cnt + 1, sizeof *v);
    size_t k = 0;
    size_t i;
    bool pending = false;
    pc_af_point pin = {0.0, 0.0};
    if (v == NULL) {
        *nomem = true;
        return false;
    }
    for (i = 0; i < cnt; i++) {
        pc_af_point p;
        uint8_t role;
        if (!record(&records->as.arr.v[i], &p, &role)) {
            free(v);
            return false;
        }
        if (role == 0) {
            v[k].p = p;
            v[k].has_in = pending;
            v[k].inh = pin;
            v[k].has_out = false;
            v[k].record = i;
            pending = false;
            k++;
        } else if (role == 1) {
            /* A leading out-handle without an anchor is edit state only. */
            if (k > 0) {
                v[k - 1].has_out = true;
                v[k - 1].out = p;
            }
        } else if (role == 2) {
            pending = true;
            pin = p;
        } else {
            free(v);
            return false;
        }
    }
    *out = v;
    *n = k;
    return true;
}

typedef struct radius {
    size_t rec;
    double r;
} radius;

static bool same(pc_af_point a, pc_af_point b) { return a.x == b.x && a.y == b.y; }

/* A subpath from anchors; *has false when there are none. */
static pc_status subpath(const anchor *a, size_t n, bool closed, const radius *radii, size_t nradii, bool *has,
                         pc_af_subpath *out, pc_error *err)
{
    size_t i;
    *has = false;
    if (n == 0) {
        return PC_OK;
    }
    if (radii != NULL && nradii > 0) {
        bool polyline = true;
        for (i = 0; i < n && polyline; i++) {
            polyline = (!a[i].has_in || same(a[i].inh, a[i].p)) && (!a[i].has_out || same(a[i].out, a[i].p));
        }
        if (polyline) {
            ax_corner_pt *pts = pc_malloc_array(n, sizeof *pts);
            size_t np = n;
            pc_status st;
            if (pts == NULL) {
                return ax_nomem(err);
            }
            for (i = 0; i < n; i++) {
                double c = 0.0;
                size_t k;
                for (k = 0; k < nradii; k++) {
                    if (radii[k].rec == a[i].record) {
                        c = pc_rs_max(c, radii[k].r);
                    }
                }
                pts[i].p = a[i].p;
                pts[i].radius = c;
            }
            /* A closed subpath repeats its first anchor; its radius may be on either copy. */
            if (closed && np > 1 && same(pts[0].p, pts[np - 1].p)) {
                double r = pts[np - 1].radius;
                np--;
                pts[0].radius = pc_rs_max(pts[0].radius, r);
            }
            st = ax_fillet(pts, np, closed, out, err);
            free(pts);
            if (st == PC_OK) {
                *has = true;
            }
            return st;
        }
    }
    out->start = a[0].p;
    out->closed = closed;
    out->nsegments = n - 1;
    out->segments = pc_malloc_array(n > 1 ? n - 1 : 1, sizeof *out->segments);
    if (out->segments == NULL) {
        return ax_nomem(err);
    }
    for (i = 0; i + 1 < n; i++) {
        out->segments[i][0] = a[i].has_out ? a[i].out : a[i].p;
        out->segments[i][1] = a[i + 1].has_in ? a[i + 1].inh : a[i + 1].p;
        out->segments[i][2] = a[i + 1].p;
    }
    *has = true;
    return PC_OK;
}

/* CnrD: positional [u8, i32[] record indices, f64[] radii, u8[] types]. */
static bool corner_radii(const pc_af_object *o, radius **out, size_t *n, bool *nomem)
{
    const pc_af_value *idx;
    const pc_af_value *rad;
    size_t m;
    size_t i;
    size_t k = 0;
    radius *v;
    if (o->nfields < 3 || o->fields[1].v.kind != PC_AF_ARRAY || o->fields[2].v.kind != PC_AF_ARRAY) {
        return false;
    }
    idx = &o->fields[1].v;
    rad = &o->fields[2].v;
    m = idx->as.arr.n < rad->as.arr.n ? idx->as.arr.n : rad->as.arr.n;
    v = pc_malloc_array(m > 0 ? m : 1, sizeof *v);
    if (v == NULL) {
        *nomem = true;
        return false;
    }
    for (i = 0; i < m; i++) {
        const pc_af_value *a = &idx->as.arr.v[i];
        const pc_af_value *b = &rad->as.arr.v[i];
        if (a->kind == PC_AF_INT && b->kind == PC_AF_FLOAT && isfinite(b->as.f) && b->as.f > 0.0 && a->as.i >= 0) {
            v[k].rec = (size_t)a->as.i;
            v[k].r = b->as.f;
            k++;
        }
    }
    *out = v;
    *n = k;
    return true;
}

/* PCvD.Data: positional [u8, u32 count, then per subpath: bool closed, 18-byte records]. */
static pc_status curves(ax_reader *r, size_t crvs, bool *ok, pc_af_path *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t data;
    const pc_af_object *d;
    size_t fi = 0;
    uint64_t count;
    size_t cid;
    bool has_corners = false;
    radius *corners = NULL;
    size_t ncorners = 0;
    pc_af_path path = {NULL, 0};
    size_t seen = 0;
    uint64_t index;
    pc_status st = PC_OK;
    bool nomem = false;
    *ok = false;
    if (!ax_obj(s, crvs, "Data", &data) || (d = ax_object(s, data)) == NULL) {
        return PC_OK;
    }
    if (fi >= d->nfields) {
        return PC_OK;
    }
    fi++;
    if (fi >= d->nfields || d->fields[fi].v.kind != PC_AF_UINT) {
        return PC_OK;
    }
    count = d->fields[fi++].v.as.u;
    if (ax_obj(s, crvs, "CnrD", &cid) && ax_object(s, cid) != NULL) {
        has_corners = corner_radii(ax_object(s, cid), &corners, &ncorners, &nomem);
        if (nomem) {
            return ax_nomem(err);
        }
    }
    for (index = 0; index < count; index++) {
        bool closed;
        const pc_af_value *records;
        anchor *an;
        size_t nan;
        bool has;
        pc_af_subpath sp;
        if (fi >= d->nfields || d->fields[fi].v.kind != PC_AF_BOOL) {
            goto none;
        }
        closed = d->fields[fi++].v.as.b;
        if (fi >= d->nfields || d->fields[fi].v.kind != PC_AF_ARRAY) {
            goto none;
        }
        records = &d->fields[fi++].v;
        if (seen > SIZE_MAX - records->as.arr.n) {
            goto none;
        }
        seen += records->as.arr.n;
        if (seen > MAX_CURVE_NODES) {
            ax_warn(r, "curves with more than four million nodes");
            goto none;
        }
        if (!anchors(records, &an, &nan, &nomem)) {
            if (nomem) {
                st = ax_nomem(err);
            }
            goto none;
        }
        st = subpath(an, nan, closed, index == 0 && has_corners ? corners : NULL, index == 0 ? ncorners : 0, &has, &sp,
                     err);
        free(an);
        if (st != PC_OK) {
            goto none;
        }
        if (has && (st = ax_path_push(&path, sp, err)) != PC_OK) {
            goto none;
        }
    }
    if (has_corners && count > 1) {
        ax_warn(r, "rounded corners on curves with several subpaths (only the first is rounded)");
    }
    free(corners);
    *out = path;
    *ok = true;
    return PC_OK;
none:
    free(corners);
    ax_path_fini(&path);
    return st;
}

static pc_status outline_at(ax_reader *r, size_t id, pc_af_tag cls, pc_af_affine world, int depth, bool *ok,
                            pc_af_path *out, bool *even_odd, pc_error *err)
{
    const pc_af_stream *s = r->s;
    *ok = false;
    if (cls == PC_AF_TAG("PCrv") || cls == PC_AF_TAG("TxtC")) {
        size_t crvs;
        size_t m;
        pc_status st;
        if (!ax_obj(s, id, "Crvs", &crvs) && !(ax_obj(s, id, "MCrM", &m) && ax_obj(s, m, "Crvs", &crvs))) {
            return PC_OK;
        }
        if ((st = curves(r, crvs, ok, out, err)) != PC_OK) {
            return st;
        }
        if (*ok) {
            *even_odd = out->nsubpaths > 1;
        }
        return PC_OK;
    }
    if (cls == PC_AF_TAG("Comp")) {
        /* All operands' contours in the compound's space, filled even-odd. */
        pc_af_path path = {NULL, 0};
        const pc_af_value *arr = ax_objs(s, id, "Chld");
        size_t i = 0;
        size_t c;
        if (depth > MAX_COMPOUND_DEPTH) {
            return PC_OK;
        }
        while (ax_next_obj(arr, &i, &c)) {
            pc_af_affine local = ax_xfrm(s, c);
            pc_af_tag ccls;
            uint16_t op;
            uint16_t ver;
            bool cok;
            bool ceo;
            pc_af_path p = {NULL, 0};
            pc_status st;
            if (!ax_class(s, c, &ccls)) {
                ax_path_fini(&path);
                return PC_OK;
            }
            if (ax_enum(s, c, "ComO", &op, &ver) && op != 0 && op != 2) {
                ax_warn(r, "compound shapes that intersect or exclude (imported as add)");
            }
            if ((st = outline_at(r, c, ccls, ax_affine_then(local, world), depth + 1, &cok, &p, &ceo, err)) != PC_OK) {
                ax_path_fini(&path);
                return st;
            }
            if (cok) {
                st = ax_path_append_transformed(&path, &p, local, err);
                ax_path_fini(&p);
                if (st != PC_OK) {
                    ax_path_fini(&path);
                    return st;
                }
            }
        }
        *out = path;
        *even_odd = true;
        *ok = true;
        return PC_OK;
    }
    {
        size_t shape;
        size_t owner;
        double b[4];
        size_t master;
        if (ax_obj(s, id, "Shpe", &shape)) {
            owner = id;
        } else if (ax_obj(s, id, "ShpM", &master) && ax_obj(s, master, "Shpe", &shape)) {
            owner = master;
        } else {
            return PC_OK;
        }
        if (!ax_floats(s, owner, "ShpB", 4, b)) {
            return PC_OK;
        }
        *even_odd = false;
        return ax_shape(r, shape, b, world, ok, out, err);
    }
}

pc_status ax_outline(ax_reader *r, size_t id, pc_af_tag cls, pc_af_affine world, bool *ok, pc_af_path *out,
                     bool *even_odd, pc_error *err)
{
    memset(out, 0, sizeof *out);
    *even_odd = false;
    return outline_at(r, id, cls, world, 0, ok, out, even_odd, err);
}
