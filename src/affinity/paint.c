/* Fills, strokes and colours (port of crates/affinity/src/paint.rs). Two schemas: Affinity 1
 * (BFil fill, PFil stroke paint, LSty style) and Affinity 2/3 (BFFl, LIFl and LILn lists with a
 * current index). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Gradient stops per gradient; spot-colour indirections followed. */
#define MAX_STOPS 1024u
#define MAX_COLOR_DEPTH 64

double pc_af_stop_half_point(const pc_af_stop *s)
{
    double k = 1.0 + 8.0 * fabs(s->bias - 0.5);
    /* LLVM rewrites 0.5f64.powf(x) as exp2(x * -1.0). */
    double h = exp2(-(1.0 / k));
    return s->bias >= 0.5 ? h : 1.0 - h;
}

/* A value as floats: Floats, or Bytes of f32 (malloc'd). */
static bool floats_of(const pc_af_value *v, double **out, size_t *n, bool *nomem)
{
    if (v->kind == PC_AF_FLOATS) {
        *out = pc_malloc_array(v->as.floats.n > 0 ? v->as.floats.n : 1, sizeof **out);
        if (*out == NULL) {
            *nomem = true;
            return false;
        }
        if (v->as.floats.n > 0) {
            memcpy(*out, v->as.floats.v, v->as.floats.n * sizeof **out);
        }
        *n = v->as.floats.n;
        return true;
    }
    if (v->kind == PC_AF_BYTES && v->as.bytes.n % 4 == 0) {
        size_t k = v->as.bytes.n / 4;
        size_t i;
        *out = pc_malloc_array(k > 0 ? k : 1, sizeof **out);
        if (*out == NULL) {
            *nomem = true;
            return false;
        }
        for (i = 0; i < k; i++) {
            const uint8_t *b = v->as.bytes.v + 4 * i;
            uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
            float f;
            memcpy(&f, &u, 4);
            (*out)[i] = (double)f;
        }
        *n = k;
        return true;
    }
    return false;
}

static double rem_euclid(double a, double b)
{
    double r = fmod(a, b);
    return r < 0.0 ? r + fabs(b) : r;
}

static void hsl(double h, double s, double l, double *r, double *g, double *b)
{
    double c = (1.0 - fabs(2.0 * l - 1.0)) * s;
    double hp = rem_euclid(h, 1.0) * 6.0;
    double x = c * (1.0 - fabs(fmod(hp, 2.0) - 1.0));
    double m;
    double rr;
    double gg;
    double bb;
    switch (pc_f64_to_u32(hp)) {
    case 0:
        rr = c;
        gg = x;
        bb = 0.0;
        break;
    case 1:
        rr = x;
        gg = c;
        bb = 0.0;
        break;
    case 2:
        rr = 0.0;
        gg = c;
        bb = x;
        break;
    case 3:
        rr = 0.0;
        gg = x;
        bb = c;
        break;
    case 4:
        rr = x;
        gg = 0.0;
        bb = c;
        break;
    default:
        rr = c;
        gg = 0.0;
        bb = x;
    }
    m = l - c / 2.0;
    *r = rr + m;
    *g = gg + m;
    *b = bb + m;
}

/* A colour object (RGBA, HSLA, CMYK, GRAY, LABA, Pant, RegC); ok false when unreadable. */
static pc_status color(ax_reader *r, size_t id, bool *ok, pc_af_color *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    pc_af_tag cls;
    double *v = NULL;
    size_t n = 0;
    bool have = false;
    bool nomem = false;
    const pc_af_value *cv;
    pc_af_color c;
    size_t i;
    *ok = false;
    memset(&c, 0, sizeof c);
    if (!ax_class(s, id, &cls)) {
        return PC_OK;
    }
    cv = ax_field(s, id, "_col");
    if (cls == PC_AF_TAG("RGBA") || cls == PC_AF_TAG("HSLA") || cls == PC_AF_TAG("CMYK") ||
        cls == PC_AF_TAG("GRAY")) {
        if (cv != NULL && floats_of(cv, &v, &n, &nomem)) {
            have = true;
            for (i = 0; i < n; i++) {
                have = have && isfinite(v[i]);
            }
        }
        if (nomem) {
            return ax_nomem(err);
        }
        if (!have) {
            free(v);
            return PC_OK;
        }
    }
    if (cls == PC_AF_TAG("RGBA")) {
        if (n < 4) {
            free(v);
            return PC_OK;
        }
        c.kind = PC_AF_RGB;
        c.v[0] = v[0];
        c.v[1] = v[1];
        c.v[2] = v[2];
        c.alpha = pc_clampd(v[3], 0.0, 1.0);
    } else if (cls == PC_AF_TAG("HSLA")) {
        if (n < 4) {
            free(v);
            return PC_OK;
        }
        c.kind = PC_AF_RGB;
        hsl(v[0], v[1], v[2], &c.v[0], &c.v[1], &c.v[2]);
        c.alpha = pc_clampd(v[3], 0.0, 1.0);
    } else if (cls == PC_AF_TAG("CMYK")) {
        if (n < 5) {
            free(v);
            return PC_OK;
        }
        c.kind = PC_AF_CMYK;
        memcpy(c.v, v, 4 * sizeof *v);
        c.alpha = pc_clampd(v[4], 0.0, 1.0);
    } else if (cls == PC_AF_TAG("GRAY")) {
        if (n < 2) {
            free(v);
            return PC_OK;
        }
        c.kind = PC_AF_GRAY;
        c.v[0] = v[0];
        c.alpha = pc_clampd(v[1], 0.0, 1.0);
    } else if (cls == PC_AF_TAG("LABA")) {
        size_t bn;
        const uint8_t *b = ax_bytes(s, id, "_col", &bn);
        double u[4];
        if (b == NULL || bn < 8) {
            return PC_OK;
        }
        for (i = 0; i < 4; i++) {
            u[i] = (double)(uint16_t)(b[2 * i] | b[2 * i + 1] << 8) / 65535.0;
        }
        c.kind = PC_AF_LAB;
        c.v[0] = 100.0 * u[0];
        c.v[1] = 255.0 * u[1] - 128.0;
        c.v[2] = 255.0 * u[2] - 128.0;
        c.alpha = u[3];
    } else if (cls == PC_AF_TAG("Pant")) {
        size_t o;
        pc_status st;
        ax_warn(r, "spot colours (imported as their RGB equivalent)");
        if (r->color_depth >= MAX_COLOR_DEPTH) {
            return PC_OK;
        }
        r->color_depth++;
        st = PC_OK;
        if (ax_obj(s, id, "srgb", &o)) {
            st = color(r, o, ok, out, err);
        }
        if (st == PC_OK && !*ok && ax_obj(s, id, "base", &o)) {
            st = color(r, o, ok, out, err);
        }
        r->color_depth--;
        return st;
    } else if (cls == PC_AF_TAG("RegC")) {
        c.kind = PC_AF_CMYK;
        c.v[0] = c.v[1] = c.v[2] = c.v[3] = 1.0;
        c.alpha = 1.0;
    } else {
        ax_warn(r, "colours in an unknown model");
        return PC_OK;
    }
    free(v);
    *out = c;
    *ok = true;
    return PC_OK;
}

pc_status ax_descriptor(ax_reader *r, size_t id, pc_af_affine local, bool *ok, pc_af_paint *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t fill;
    pc_af_affine unit = AX_IDENTITY;
    pc_af_tag cls;
    pc_af_paint p;
    pc_status st;
    *ok = false;
    memset(&p, 0, sizeof p);
    if (ax_is(s, id, "FDsc")) {
        double m[6];
        if (ax_floats(s, id, "FDeX", 6, m)) {
            unit = ax_affine_from_xfrm(m);
        }
        if (!ax_obj(s, id, "FDeF", &fill)) {
            return PC_OK;
        }
    } else {
        fill = id;
    }
    if (!ax_class(s, fill, &cls)) {
        return PC_OK;
    }
    if (cls == PC_AF_TAG("FilN")) {
        p.kind = PC_AF_PAINT_NONE;
    } else if (cls == PC_AF_TAG("FilS")) {
        size_t co;
        bool cok;
        if (!ax_obj(s, fill, "Colr", &co)) {
            return PC_OK;
        }
        if ((st = color(r, co, &cok, &p.color, err)) != PC_OK || !cok) {
            return st;
        }
        p.kind = PC_AF_SOLID;
    } else if (cls == PC_AF_TAG("FilG")) {
        uint16_t ty;
        uint16_t ver;
        size_t grad;
        const pc_af_value *cols;
        const pc_af_value *posn;
        size_t ci = 0;
        size_t pi = 0;
        size_t c;
        if (!ax_enum(s, fill, "Type", &ty, &ver) || ty == 0) {
            p.gkind = PC_AF_LINEAR;
        } else if (ty == 1 || ty == 2) {
            p.gkind = PC_AF_RADIAL;
        } else if (ty == 3) {
            p.gkind = PC_AF_CONICAL;
        } else {
            ax_warn(r, "gradients of an unknown kind (imported as linear)");
            p.gkind = PC_AF_LINEAR;
        }
        if (!ax_obj(s, fill, "Grad", &grad)) {
            return PC_OK;
        }
        cols = ax_objs(s, grad, "Cols");
        posn = ax_field(s, grad, "Posn");
        if (posn != NULL && posn->kind != PC_AF_ARRAY) {
            posn = NULL;
        }
        p.stops = pc_malloc_array(MAX_STOPS, sizeof *p.stops);
        if (p.stops == NULL) {
            return ax_nomem(err);
        }
        while (p.nstops < MAX_STOPS && ax_next_obj(cols, &ci, &c)) {
            double *pv = NULL;
            size_t pn = 0;
            bool nomem = false;
            bool cok;
            pc_af_stop stop;
            /* The next position that reads as floats (others are skipped). */
            bool found = false;
            while (posn != NULL && pi < posn->as.arr.n) {
                if (floats_of(&posn->as.arr.v[pi++], &pv, &pn, &nomem)) {
                    found = true;
                    break;
                }
                if (nomem) {
                    free(p.stops);
                    return ax_nomem(err);
                }
            }
            if (!found) {
                break;
            }
            if ((st = color(r, c, &cok, &stop.color, err)) != PC_OK || !cok) {
                free(pv);
                free(p.stops);
                return st;
            }
            stop.offset = pn > 0 && isfinite(pv[0]) ? pc_clampd(pv[0], 0.0, 1.0) : 0.0;
            stop.bias = pn > 1 && isfinite(pv[1]) ? pc_clampd(pv[1], 0.0, 1.0) : 0.5;
            free(pv);
            p.stops[p.nstops++] = stop;
        }
        if (p.nstops == 0) {
            free(p.stops);
            p.stops = NULL;
            p.kind = PC_AF_PAINT_NONE;
        } else {
            p.kind = PC_AF_GRADIENT;
            p.transform = ax_affine_then(unit, local);
        }
    } else if (cls == PC_AF_TAG("FilB")) {
        ax_warn(r, "bitmap and pattern fills (left empty)");
        p.kind = PC_AF_PAINT_NONE;
    } else {
        ax_warn(r, "fills of an unknown kind (left empty)");
        p.kind = PC_AF_PAINT_NONE;
    }
    *out = p;
    *ok = true;
    return PC_OK;
}

/* The current item of a list field: list[index], or the first. */
static bool current(const ax_reader *r, size_t id, const char *list, const char *index, size_t *out)
{
    const pc_af_value *arr = ax_objs(r->s, id, list);
    int64_t i64;
    size_t want = ax_int(r->s, id, index, &i64) && i64 >= 0 ? (size_t)i64 : 0;
    size_t i = 0;
    size_t k = 0;
    size_t o;
    bool first = false;
    size_t fo = 0;
    while (ax_next_obj(arr, &i, &o)) {
        if (!first) {
            first = true;
            fo = o;
        }
        if (k == want) {
            *out = o;
            return true;
        }
        k++;
    }
    if (first) {
        *out = fo;
    }
    return first;
}

pc_status ax_fills(ax_reader *r, size_t id, pc_af_affine local, const char *list, pc_af_paint **out, size_t *n,
                   pc_error *err)
{
    size_t d;
    bool ok;
    pc_af_paint p;
    pc_status st;
    *out = NULL;
    *n = 0;
    if (!current(r, id, list, "BFCr", &d) && !ax_obj(r->s, id, "BFil", &d)) {
        return PC_OK;
    }
    if ((st = ax_descriptor(r, d, local, &ok, &p, err)) != PC_OK || !ok) {
        return st;
    }
    if (p.kind == PC_AF_PAINT_NONE) {
        ax_paint_fini(&p);
        return PC_OK;
    }
    *out = malloc(sizeof **out);
    if (*out == NULL) {
        ax_paint_fini(&p);
        return ax_nomem(err);
    }
    **out = p;
    *n = 1;
    return PC_OK;
}

static pc_status stroke(ax_reader *r, size_t pen, size_t ldsc, pc_af_affine local, bool behind, bool *ok,
                        pc_af_stroke *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    pc_af_stroke k;
    size_t lsty;
    double weight;
    bool scales;
    size_t dn = 0;
    const uint8_t *data;
    int64_t la;
    const pc_af_value *patn;
    bool pok;
    size_t o;
    pc_status st;
    *ok = false;
    memset(&k, 0, sizeof k);
    if ((st = ax_descriptor(r, pen, local, &pok, &k.paint, err)) != PC_OK || !pok) {
        return st;
    }
    if (k.paint.kind == PC_AF_PAINT_NONE) {
        ax_paint_fini(&k.paint);
        return PC_OK;
    }
    if (ax_is(s, ldsc, "LDsc")) {
        if (!ax_obj(s, ldsc, "LDeL", &lsty)) {
            ax_paint_fini(&k.paint);
            return PC_OK;
        }
    } else {
        lsty = ldsc;
    }
    if (!ax_f64(s, lsty, "Wght", &weight) || !(weight > 0.0 && weight < 1e7)) {
        ax_paint_fini(&k.paint);
        return PC_OK;
    }
    /* "Scale with object": the weight follows the object's transform; otherwise it is absolute. */
    scales = ax_bool(s, ldsc, "LDSc", &scales) ? scales : true;
    k.width = scales ? weight * ax_affine_scale(&local) : weight;
    data = ax_bytes(s, lsty, "Data", &dn);
    if (data == NULL) {
        dn = 0;
    }
    k.miter_limit = 4.0;
    if (dn >= 8) {
        uint64_t u = 0;
        double m;
        int i;
        for (i = 7; i >= 0; i--) {
            u = u << 8 | data[i];
        }
        memcpy(&m, &u, 8);
        if (isfinite(m) && m >= 1.0) {
            k.miter_limit = m;
        }
    }
    {
        int b8 = dn > 8 ? data[8] : -1;
        int b9 = dn > 9 ? data[9] : -1;
        int b10 = dn > 10 ? data[10] : -1;
        if (b8 == 0 && b9 == 0) {
            k.cap = PC_AF_BUTT;
            k.join = PC_AF_MITER;
        } else if (b8 == 0 && b9 >= 0) {
            k.cap = PC_AF_BUTT;
            k.join = PC_AF_ROUND_JOIN;
        } else if (b8 == 2 && b9 == 2 && b10 == 3) {
            k.cap = PC_AF_ROUND_CAP;
            k.join = PC_AF_ROUND_JOIN;
        } else if (b8 == 2 && b9 == 2) {
            k.cap = PC_AF_SQUARE;
            k.join = PC_AF_ROUND_JOIN;
        } else {
            k.cap = PC_AF_ROUND_CAP;
            k.join = PC_AF_ROUND_JOIN;
        }
        k.align = ax_int(s, ldsc, "LDSa", &la) && la == 1   ? PC_AF_INSIDE
                  : ax_int(s, ldsc, "LDSa", &la) && la == 2 ? PC_AF_OUTSIDE
                                                            : PC_AF_CENTER;
        patn = ax_field(s, lsty, "Patn");
        if (patn != NULL && patn->kind == PC_AF_ARRAY && (b10 == 2 || patn->as.arr.n > 0)) {
            double *v = pc_malloc_array(patn->as.arr.n > 0 ? patn->as.arr.n : 1, sizeof *v);
            size_t nv = 0;
            size_t i;
            bool valid;
            bool any = false;
            if (v == NULL) {
                ax_paint_fini(&k.paint);
                return ax_nomem(err);
            }
            for (i = 0; i < patn->as.arr.n; i++) {
                if (patn->as.arr.v[i].kind == PC_AF_FLOAT) {
                    v[nv++] = patn->as.arr.v[i].as.f;
                }
            }
            while (nv >= 2 && v[nv - 1] == 0.0 && v[nv - 2] == 0.0) {
                nv -= 2;
            }
            valid = nv > 0;
            for (i = 0; i < nv; i++) {
                valid = valid && isfinite(v[i]) && v[i] >= 0.0;
                any = any || v[i] > 0.0;
            }
            if (valid && any) {
                double ph;
                for (i = 0; i < nv; i++) {
                    v[i] *= k.width;
                }
                k.has_dash = true;
                k.dash = v;
                k.ndash = nv;
                k.dash_phase = (ax_f64(s, lsty, "Phse", &ph) ? ph : 0.0) * k.width;
            } else {
                free(v);
            }
        }
    }
    if (ax_obj(s, lsty, "Brus", &o) || ax_obj(s, ldsc, "LDeP", &o)) {
        ax_warn(r, "brush and pressure strokes (imported as plain strokes)");
    }
    k.behind = behind;
    *out = k;
    *ok = true;
    return PC_OK;
}

pc_status ax_node_paint(ax_reader *r, size_t id, pc_af_affine local, pc_af_paint **fills, size_t *nfills,
                        pc_af_stroke **strokes, size_t *nstrokes, pc_error *err)
{
    const pc_af_stream *s = r->s;
    double fo;
    size_t o;
    bool has_pen;
    bool has_style;
    size_t pen = 0;
    size_t style = 0;
    const pc_af_value *dw;
    bool behind;
    pc_status st;
    *strokes = NULL;
    *nstrokes = 0;
    if ((st = ax_fills(r, id, local, "BFFl", fills, nfills, err)) != PC_OK) {
        return st;
    }
    if (ax_objs_count(ax_objs(s, id, "BFFl")) > 1 || ax_objs_count(ax_objs(s, id, "LIFl")) > 1) {
        ax_warn(r, "several fills or strokes on one object (only the active one is imported)");
    }
    if (ax_f64(s, id, "FOpc", &fo) && fo < 0.999) {
        ax_warn(r, "fill opacity (imported at full fill opacity)");
    }
    if (ax_obj(s, id, "Trns", &o)) {
        ax_warn(r, "transparency gradients (imported without them)");
    }
    if (ax_field(s, id, "LIFl") != NULL) {
        has_pen = current(r, id, "LIFl", "LICr", &pen);
        has_style = current(r, id, "LILn", "LICr", &style);
    } else {
        has_pen = ax_obj(s, id, "PFil", &pen);
        has_style = ax_obj(s, id, "LSty", &style);
    }
    dw = ax_field(s, id, "DrwO");
    behind = dw != NULL && dw->kind == PC_AF_ARRAY && dw->as.arr.n > 0 && dw->as.arr.v[0].kind == PC_AF_ENUM &&
             dw->as.arr.v[0].as.e.id == 1 && dw->as.arr.v[0].as.e.version == 0;
    if (has_pen && has_style) {
        pc_af_stroke k;
        bool ok;
        if ((st = stroke(r, pen, style, local, behind, &ok, &k, err)) != PC_OK) {
            return st;
        }
        if (ok) {
            *strokes = malloc(sizeof **strokes);
            if (*strokes == NULL) {
                ax_stroke_fini(&k);
                return ax_nomem(err);
            }
            **strokes = k;
            *nstrokes = 1;
        }
    }
    return PC_OK;
}
