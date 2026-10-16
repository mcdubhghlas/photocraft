/* E11 vector command specs (port of the command surface of crates/engine/src/vector_cmds.rs):
 * shape layers (shape.*), document paths (path.*), selection <-> path, and layer vector masks
 * (layer.vectorMask.*). The cross-module primitives (transform_shape/map_path/refresh_shape/...)
 * live in vector_cmds.c; this file adds the command parse/serialise helpers, the run() closures
 * and pc_engine_vector_specs(), registered through register_modules() in command.c.
 *
 * Coordinates are document pixels. Paths use the reference's compact JSON form:
 *   {"subpaths":[{"closed":bool,"op":"combine|subtract|intersect|exclude",
 *                 "knots":[[x,y] | {"anchor":[x,y],"in":[x,y],"out":[x,y],"smooth":bool}]}],
 *    "fillRule":"nonzero|evenodd","inverted":bool}
 * Colours are "#rrggbb"/"#rrggbbaa"/[r,g,b,a] (0..1); opacities are percentages (0..100).
 *
 * Deferred (not registered): path.stroke needs the still-unported stroke-command grouping helper
 * (analysis_cmds::compound) and the plain-brush paint apply; layer.rasterize.shape needs
 * extra_cmds::rasterize (E12). See REWRITE_PLAN.md §8.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "algo/selection_blur.h"
#include "algo/transform.h"
#include "base/blob.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/str.h"
#include "color/color.h"
#include "compose/masks.h"
#include "compose/psblend.h"
#include "geom/warp.h"
#include "ops/ops.h" /* pc_doc_arc_get */
#include "raster/pixel.h"
#include "vector/vector.h"

#include "color/blend.h" /* pc_blend_mode, PC_BLEND_NORMAL */
#include "doc/json.h" /* pc_live_shape_to_json */

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "extra_cmds.h" /* pc_engine_rasterize_selected_shapes (layer.rasterize.shape no-layer path) */
#include "layer_cmds.h" /* pc_engine_top_level / _selected / _set_selection */
#include "vector_cmds.h"
#include "warp_cmds.h" /* pc_engine_warp_from_params */

/* Keys PSD uses for a shape layer's vector data; dropped when a shape is rasterized. */
static const uint8_t SHAPE_BLOCKS[8][4] = {{'v', 'm', 's', 'k'}, {'v', 's', 'm', 's'}, {'v', 'o', 'g', 'k'},
                                           {'v', 's', 't', 'k'}, {'v', 's', 'c', 'g'}, {'S', 'o', 'C', 'o'},
                                           {'G', 'd', 'F', 'l'}, {'P', 't', 'F', 'l'}};

/* ---- shape-style clipboard lifetime ------------------------------------------------------- */

void pc_engine_vector_clipboard_fini(pc_engine_session *s)
{
    if (s == NULL) {
        return;
    }
    if (s->has_path_fill_clip) {
        pc_fill_fini(&s->path_fill_clip);
        s->has_path_fill_clip = false;
    }
    if (s->has_path_stroke_clip) {
        pc_shape_stroke_fini(&s->path_stroke_clip);
        s->has_path_stroke_clip = false;
    }
}

/* ---- param readers ------------------------------------------------------------------------ */

static bool get_f64(yyjson_val *p, const char *k, double *out) { return pc_json_as_f64(pc_json_get(p, k), out); }

static bool pbool(yyjson_val *p, const char *k, bool def)
{
    bool b;
    return pc_json_as_bool(pc_json_get(p, k), &b) ? b : def;
}

static bool val_point(yyjson_val *v, pc_point *out)
{
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_arr(v)) {
        if (yyjson_arr_size(v) < 2) {
            return false;
        }
        double x, y;
        if (!pc_json_as_f64(yyjson_arr_get(v, 0), &x) || !pc_json_as_f64(yyjson_arr_get(v, 1), &y)) {
            return false;
        }
        *out = pc_point_make(x, y);
        return true;
    }
    if (yyjson_is_obj(v)) {
        double x, y;
        if (!pc_json_as_f64(pc_json_get(v, "x"), &x) || !pc_json_as_f64(pc_json_get(v, "y"), &y)) {
            return false;
        }
        *out = pc_point_make(x, y);
        return true;
    }
    return false;
}

/* N numbers at key (array of at least N numbers). */
static bool nums_n(yyjson_val *p, const char *k, size_t n, double *out)
{
    yyjson_val *a = pc_json_get(p, k);
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!pc_json_as_f64(yyjson_arr_get(a, i), &out[i])) {
            return false;
        }
    }
    return true;
}

/* [tl, tr, br, bl] from "radii"/"radius" as an array or a single number. */
static bool read_radii(yyjson_val *p, double out[4])
{
    yyjson_val *v = pc_json_get(p, "radii");
    if (v == NULL) {
        v = pc_json_get(p, "radius");
    }
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_num(v)) {
        double r;
        if (!pc_json_as_f64(v, &r)) {
            return false;
        }
        out[0] = out[1] = out[2] = out[3] = r;
        return true;
    }
    return nums_n(p, "radii", 4, out);
}

static int hexd(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool hex2(const char *s, size_t len, size_t i, float *out)
{
    if (i + 2 > len) {
        return false;
    }
    int hi = hexd(s[i]), lo = hexd(s[i + 1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    *out = (float)(hi * 16 + lo) / 255.0f;
    return true;
}

/* "#rrggbb" / "#rrggbbaa" / [r,g,b,a] (0..1). */
static bool vcolor(yyjson_val *v, pc_color *out)
{
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_arr(v)) {
        size_t n = yyjson_arr_size(v);
        if (n < 3) {
            return false;
        }
        double c[4] = {0.0, 0.0, 0.0, 1.0};
        for (size_t i = 0; i < n && i < 4; i++) {
            double x;
            c[i] = pc_json_as_f64(yyjson_arr_get(v, i), &x) ? x : 0.0;
        }
        *out = pc_color_rgba((float)c[0], (float)c[1], (float)c[2], n >= 4 ? (float)c[3] : 1.0f);
        return true;
    }
    if (yyjson_is_str(v)) {
        size_t len = 0;
        const char *s = pc_json_as_str(v, &len);
        if (s == NULL) {
            return false;
        }
        while (len > 0 && s[0] == '#') {
            s++;
            len--;
        }
        float r, g, b, a;
        if (len == 6 && hex2(s, len, 0, &r) && hex2(s, len, 2, &g) && hex2(s, len, 4, &b)) {
            *out = pc_color_rgb(r, g, b);
            return true;
        }
        if (len == 8 && hex2(s, len, 0, &r) && hex2(s, len, 2, &g) && hex2(s, len, 4, &b) && hex2(s, len, 6, &a)) {
            *out = pc_color_rgba(r, g, b, a);
            return true;
        }
    }
    return false;
}

static uint8_t quant(float v) { return (uint8_t)(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

/* "#rrggbb" or "#rrggbbaa" into out (>= 10 bytes). */
static void color_hex(const pc_color *c, char out[10])
{
    float rgb[3];
    pc_color_to_rgb(c, rgb);
    if (c->alpha < 1.0f) {
        snprintf(out, 10, "#%02x%02x%02x%02x", quant(rgb[0]), quant(rgb[1]), quant(rgb[2]), quant(c->alpha));
    } else {
        snprintf(out, 10, "#%02x%02x%02x", quant(rgb[0]), quant(rgb[1]), quant(rgb[2]));
    }
}

/* op name <-> enum. */
static bool op_from(const char *s, size_t len, pc_path_op *out)
{
    char b[16];
    if (len >= sizeof b) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        b[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    b[len] = '\0';
    if (!strcmp(b, "combine") || !strcmp(b, "add") || !strcmp(b, "union")) {
        *out = PC_PATH_COMBINE;
    } else if (!strcmp(b, "subtract") || !strcmp(b, "minus")) {
        *out = PC_PATH_SUBTRACT;
    } else if (!strcmp(b, "intersect")) {
        *out = PC_PATH_INTERSECT;
    } else if (!strcmp(b, "exclude") || !strcmp(b, "xor")) {
        *out = PC_PATH_EXCLUDE;
    } else if (!strcmp(b, "join")) {
        *out = PC_PATH_JOIN;
    } else {
        return false;
    }
    return true;
}

static const char *op_name(pc_path_op op)
{
    switch (op) {
    case PC_PATH_SUBTRACT:
        return "subtract";
    case PC_PATH_INTERSECT:
        return "intersect";
    case PC_PATH_EXCLUDE:
        return "exclude";
    case PC_PATH_JOIN:
        return "join";
    case PC_PATH_COMBINE:
    default:
        return "combine";
    }
}

static bool ieq(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return b[len] == '\0';
}

/* ---- path parse / serialise --------------------------------------------------------------- */

/* Parse the compact path form. On false, *emsg holds the reference's message. */
static bool parse_path(yyjson_val *v, pc_path *out, char *emsg, size_t ecap)
{
    pc_path path = {0};
    yyjson_val *subs = v != NULL ? pc_json_get(v, "subpaths") : NULL;
    if (subs == NULL || !yyjson_is_arr(subs)) {
        snprintf(emsg, ecap, "path needs `subpaths`");
        return false;
    }
    size_t nsub = yyjson_arr_size(subs);
    if (nsub > 0) {
        path.subpaths = calloc(nsub, sizeof *path.subpaths);
        if (path.subpaths == NULL) {
            snprintf(emsg, ecap, "out of memory");
            return false;
        }
    }
    for (size_t i = 0; i < nsub; i++) {
        yyjson_val *sv = yyjson_arr_get(subs, i);
        yyjson_val *kv = sv != NULL ? pc_json_get(sv, "knots") : NULL;
        if (kv == NULL || !yyjson_is_arr(kv)) {
            snprintf(emsg, ecap, "subpath %zu needs `knots`", i);
            pc_path_fini(&path);
            return false;
        }
        size_t nk = yyjson_arr_size(kv);
        pc_subpath sp = {0};
        if (nk > 0) {
            sp.knots = calloc(nk, sizeof *sp.knots);
            if (sp.knots == NULL) {
                snprintf(emsg, ecap, "out of memory");
                pc_path_fini(&path);
                return false;
            }
        }
        for (size_t j = 0; j < nk; j++) {
            yyjson_val *k = yyjson_arr_get(kv, j);
            pc_knot kn = {0};
            if (k != NULL && yyjson_is_arr(k)) {
                pc_point a;
                if (!val_point(k, &a)) {
                    snprintf(emsg, ecap, "knot %zu.%zu: expected [x, y]", i, j);
                    free(sp.knots);
                    pc_path_fini(&path);
                    return false;
                }
                kn.anchor = a;
                kn.in_ctrl = a;
                kn.out_ctrl = a;
                kn.smooth = false;
            } else {
                pc_point a;
                if (k == NULL || !val_point(pc_json_get(k, "anchor"), &a)) {
                    snprintf(emsg, ecap, "knot %zu.%zu: missing anchor", i, j);
                    free(sp.knots);
                    pc_path_fini(&path);
                    return false;
                }
                pc_point in = a, outc = a, t;
                if (val_point(pc_json_get(k, "in"), &t) || val_point(pc_json_get(k, "inCtrl"), &t) ||
                    val_point(pc_json_get(k, "in_ctrl"), &t)) {
                    in = t;
                }
                if (val_point(pc_json_get(k, "out"), &t) || val_point(pc_json_get(k, "outCtrl"), &t) ||
                    val_point(pc_json_get(k, "out_ctrl"), &t)) {
                    outc = t;
                }
                kn.anchor = a;
                kn.in_ctrl = in;
                kn.out_ctrl = outc;
                kn.smooth = pbool(k, "smooth", false);
            }
            sp.knots[j] = kn;
            sp.nknots = j + 1;
        }
        sp.closed = pbool(sv, "closed", true);
        sp.op = PC_PATH_COMBINE;
        size_t olen = 0;
        const char *os = pc_json_as_str(pc_json_get(sv, "op"), &olen);
        if (os != NULL) {
            if (!op_from(os, olen, &sp.op)) {
                snprintf(emsg, ecap, "subpath %zu: unknown op `%.*s`", i, (int)olen, os);
                pc_subpath_fini(&sp);
                pc_path_fini(&path);
                return false;
            }
        }
        path.subpaths[i] = sp;
        path.nsubpaths = i + 1;
    }
    size_t frlen = 0;
    const char *fr = pc_json_as_str(pc_json_get(v, "fillRule"), &frlen);
    if (fr == NULL) {
        fr = pc_json_as_str(pc_json_get(v, "fill_rule"), &frlen);
    }
    path.fill_rule = (fr != NULL && (ieq(fr, "evenodd", frlen) || ieq(fr, "even-odd", frlen) || ieq(fr, "even_odd", frlen)))
                         ? PC_FILL_RULE_EVENODD
                         : PC_FILL_RULE_NONZERO;
    path.inverted = pbool(v, "inverted", false);
    *out = path;
    return true;
}

static yyjson_mut_val *pt_json(yyjson_mut_doc *d, pc_point p)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, a, p.x);
    yyjson_mut_arr_add_real(d, a, p.y);
    return a;
}

static yyjson_mut_val *path_to_json(yyjson_mut_doc *d, const pc_path *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_val *subs = yyjson_mut_arr(d);
    for (size_t i = 0; i < p->nsubpaths; i++) {
        const pc_subpath *sp = &p->subpaths[i];
        yyjson_mut_val *so = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, so, "closed", sp->closed);
        yyjson_mut_obj_add_str(d, so, "op", op_name(sp->op));
        yyjson_mut_val *ks = yyjson_mut_arr(d);
        for (size_t j = 0; j < sp->nknots; j++) {
            const pc_knot *kn = &sp->knots[j];
            yyjson_mut_val *ko = yyjson_mut_obj(d);
            yyjson_mut_obj_add_val(d, ko, "anchor", pt_json(d, kn->anchor));
            yyjson_mut_obj_add_val(d, ko, "in", pt_json(d, kn->in_ctrl));
            yyjson_mut_obj_add_val(d, ko, "out", pt_json(d, kn->out_ctrl));
            yyjson_mut_obj_add_bool(d, ko, "smooth", kn->smooth);
            yyjson_mut_arr_add_val(ks, ko);
        }
        yyjson_mut_obj_add_val(d, so, "knots", ks);
        yyjson_mut_arr_add_val(subs, so);
    }
    yyjson_mut_obj_add_val(d, o, "subpaths", subs);
    yyjson_mut_obj_add_str(d, o, "fillRule", p->fill_rule == PC_FILL_RULE_EVENODD ? "evenodd" : "nonzero");
    yyjson_mut_obj_add_bool(d, o, "inverted", p->inverted);
    return o;
}

/* Exported wrappers so other command modules (the Shapes panel, shape.presets.*) reach the vector
 * path parser / serialiser (vector_cmds::{parse_path, path_json}). */
bool pc_engine_parse_path(yyjson_val *v, pc_path *out, char *emsg, size_t ecap)
{
    return parse_path(v, out, emsg, ecap);
}
yyjson_mut_val *pc_engine_path_to_json(yyjson_mut_doc *d, const pc_path *p)
{
    return path_to_json(d, p);
}

/* ---- fill parse / serialise --------------------------------------------------------------- */

static const char *gradient_style_name(pc_gradient_style s)
{
    switch (s) {
    case PC_GRADIENT_RADIAL:
        return "radial";
    case PC_GRADIENT_ANGLE:
        return "angle";
    case PC_GRADIENT_REFLECTED:
        return "reflected";
    case PC_GRADIENT_DIAMOND:
        return "diamond";
    case PC_GRADIENT_LINEAR:
    default:
        return "linear";
    }
}

static yyjson_mut_val *fill_to_json(yyjson_mut_doc *d, const pc_fill *f)
{
    char hex[10];
    if (f->kind == PC_FILL_SOLID) {
        color_hex(&f->u.solid, hex);
        return yyjson_mut_strcpy(d, hex);
    }
    if (f->kind == PC_FILL_GRADIENT) {
        yyjson_mut_val *g = yyjson_mut_obj(d);
        yyjson_mut_val *stops = yyjson_mut_arr(d);
        for (size_t i = 0; i < f->u.gradient.nstops; i++) {
            yyjson_mut_val *pair = yyjson_mut_arr(d);
            yyjson_mut_arr_add_real(d, pair, (double)f->u.gradient.stops[i].pos);
            color_hex(&f->u.gradient.stops[i].color, hex);
            yyjson_mut_arr_add_strcpy(d, pair, hex);
            yyjson_mut_arr_add_val(stops, pair);
        }
        yyjson_mut_obj_add_val(d, g, "stops", stops);
        yyjson_mut_obj_add_real(d, g, "angle", (double)f->u.gradient.angle);
        yyjson_mut_obj_add_real(d, g, "scale", (double)f->u.gradient.scale * 100.0);
        yyjson_mut_obj_add_str(d, g, "style", gradient_style_name(f->u.gradient.style));
        yyjson_mut_obj_add_bool(d, g, "reverse", f->u.gradient.reverse);
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_val(d, o, "gradient", g);
        return o;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "pattern", pc_str_c(&f->u.pattern.name));
    yyjson_mut_obj_add_real(d, o, "scale", (double)f->u.pattern.scale * 100.0);
    return o;
}

/* 0 = no fill (null), 1 = *out set, -1 = error (*emsg). */
static int parse_fill(yyjson_val *v, pc_fill *out, char *emsg, size_t ecap)
{
    if (v == NULL || yyjson_is_null(v)) {
        return 0;
    }
    pc_color c;
    if (vcolor(v, &c)) {
        *out = pc_fill_solid(c);
        return 1;
    }
    yyjson_val *g = pc_json_get(v, "gradient");
    if (g != NULL) {
        pc_color_stop fixed[2];
        pc_color_stop *stops = NULL;
        size_t nstops = 0, cap = 0;
        yyjson_val *sv = pc_json_get(g, "stops");
        if (sv != NULL && yyjson_is_arr(sv)) {
            size_t n = yyjson_arr_size(sv);
            if (n > 0) {
                stops = calloc(n, sizeof *stops);
                if (stops == NULL) {
                    snprintf(emsg, ecap, "out of memory");
                    return -1;
                }
                cap = n;
            }
            for (size_t i = 0; i < n; i++) {
                yyjson_val *e = yyjson_arr_get(sv, i);
                double t;
                pc_color col;
                if (e != NULL && yyjson_is_arr(e) && yyjson_arr_size(e) >= 2 && pc_json_as_f64(yyjson_arr_get(e, 0), &t) &&
                    vcolor(yyjson_arr_get(e, 1), &col)) {
                    stops[nstops].pos = (float)t;
                    stops[nstops].color = col;
                    nstops++;
                }
            }
        } else {
            fixed[0].pos = 0.0f;
            fixed[0].color = pc_color_rgb(0.0f, 0.0f, 0.0f);
            fixed[1].pos = 1.0f;
            fixed[1].color = pc_color_rgb(1.0f, 1.0f, 1.0f);
            stops = fixed;
            nstops = 2;
        }
        double angle = 90.0, scale = 100.0;
        (void)get_f64(g, "angle", &angle);
        (void)get_f64(g, "scale", &scale);
        size_t stlen = 0;
        const char *st = pc_json_as_str(pc_json_get(g, "style"), &stlen);
        pc_gradient_style style = PC_GRADIENT_LINEAR;
        if (st != NULL) {
            if (ieq(st, "radial", stlen)) {
                style = PC_GRADIENT_RADIAL;
            } else if (ieq(st, "angle", stlen)) {
                style = PC_GRADIENT_ANGLE;
            } else if (ieq(st, "reflected", stlen)) {
                style = PC_GRADIENT_REFLECTED;
            } else if (ieq(st, "diamond", stlen)) {
                style = PC_GRADIENT_DIAMOND;
            }
        }
        bool reverse = pbool(g, "reverse", false);
        pc_status rc = pc_fill_gradient(stops, nstops, (float)angle, (float)(scale / 100.0), style, reverse, out, NULL);
        if (cap > 0) {
            free(stops);
        }
        if (rc != PC_OK) {
            snprintf(emsg, ecap, "out of memory");
            return -1;
        }
        return 1;
    }
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(v, "pattern"), &nlen);
    if (name != NULL) {
        double scale = 100.0;
        (void)get_f64(v, "scale", &scale);
        memset(out, 0, sizeof *out);
        out->kind = PC_FILL_PATTERN;
        if (pc_str_set(&out->u.pattern.name, name, nlen, NULL) != PC_OK) {
            snprintf(emsg, ecap, "out of memory");
            return -1;
        }
        out->u.pattern.scale = (float)(scale / 100.0);
        out->u.pattern.angle = 0.0f;
        out->u.pattern.link = true;
        out->u.pattern.phase[0] = 0.0f;
        out->u.pattern.phase[1] = 0.0f;
        return 1;
    }
    snprintf(emsg, ecap, "unrecognised fill");
    return -1;
}

/* ---- stroke parse / serialise ------------------------------------------------------------- */

static const char *align_name(pc_stroke_align a)
{
    return a == PC_STROKE_ALIGN_INSIDE ? "inside" : a == PC_STROKE_ALIGN_OUTSIDE ? "outside" : "center";
}
static const char *cap_name(pc_line_cap c)
{
    return c == PC_LINE_CAP_ROUND ? "round" : c == PC_LINE_CAP_SQUARE ? "square" : "butt";
}
static const char *join_name(pc_line_join j)
{
    return j == PC_LINE_JOIN_ROUND ? "round" : j == PC_LINE_JOIN_BEVEL ? "bevel" : "miter";
}

static yyjson_mut_val *stroke_to_json(yyjson_mut_doc *d, const pc_shape_stroke *s)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, o, "width", (double)s->width);
    yyjson_mut_obj_add_val(d, o, "fill", fill_to_json(d, &s->paint));
    yyjson_mut_obj_add_real(d, o, "opacity", (double)s->opacity * 100.0);
    yyjson_mut_obj_add_str(d, o, "align", align_name(s->align));
    yyjson_mut_obj_add_str(d, o, "cap", cap_name(s->cap));
    yyjson_mut_obj_add_str(d, o, "join", join_name(s->join));
    yyjson_mut_obj_add_real(d, o, "miterLimit", (double)s->miter_limit);
    yyjson_mut_val *dashes = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->ndashes; i++) {
        yyjson_mut_arr_add_real(d, dashes, (double)s->dashes[i]);
    }
    yyjson_mut_obj_add_val(d, o, "dashes", dashes);
    yyjson_mut_obj_add_real(d, o, "dashOffset", (double)s->dash_offset);
    return o;
}

/* 0 = no stroke (null/false), 1 = *out set, -1 = error (*emsg). base may be NULL. */
static int parse_stroke(yyjson_val *v, const pc_shape_stroke *base, const float fg[4], pc_shape_stroke *out,
                        char *emsg, size_t ecap)
{
    bool bv;
    if (v == NULL || yyjson_is_null(v) || (pc_json_as_bool(v, &bv) && !bv)) {
        return 0;
    }
    if (!yyjson_is_obj(v)) {
        snprintf(emsg, ecap, "stroke must be an object or null");
        return -1;
    }
    static const struct {
        const char *key;
        double lo, hi;
    } ranges[] = {{"width", 0.0, 1000000.0}, {"opacity", 0.0, 100.0}, {"miterLimit", 1.0, 500.0},
                  {"dashOffset", -1000000.0, 1000000.0}};
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *rv = pc_json_get(v, ranges[i].key);
        if (rv != NULL) {
            double n;
            if (!pc_json_as_f64(rv, &n) || !isfinite(n) || n < ranges[i].lo || n > ranges[i].hi) {
                snprintf(emsg, ecap, "`%s` must be a finite number between %g and %g", ranges[i].key, ranges[i].lo,
                         ranges[i].hi);
                return -1;
            }
        }
    }
    static const char *strkeys[] = {"align", "cap", "join"};
    for (size_t i = 0; i < 3; i++) {
        yyjson_val *sv = pc_json_get(v, strkeys[i]);
        if (sv != NULL && !yyjson_is_str(sv)) {
            snprintf(emsg, ecap, "`%s` must be a string", strkeys[i]);
            return -1;
        }
    }
    pc_shape_stroke s;
    if (base != NULL) {
        if (pc_shape_stroke_clone(base, &s, NULL) != PC_OK) {
            snprintf(emsg, ecap, "out of memory");
            return -1;
        }
    } else {
        pc_shape_stroke_default(&s);
        pc_fill_fini(&s.paint);
        s.paint = pc_fill_solid(pc_color_rgba(fg[0], fg[1], fg[2], fg[3]));
    }
    double w;
    if (get_f64(v, "width", &w)) {
        s.width = (float)w;
    }
    yyjson_val *cv = pc_json_get(v, "color");
    if (cv == NULL) {
        cv = pc_json_get(v, "fill");
    }
    if (cv != NULL) {
        pc_fill nf;
        int r = parse_fill(cv, &nf, emsg, ecap);
        if (r < 0) {
            pc_shape_stroke_fini(&s);
            return -1;
        }
        if (r == 0) {
            snprintf(emsg, ecap, "stroke colour cannot be null");
            pc_shape_stroke_fini(&s);
            return -1;
        }
        pc_fill_fini(&s.paint);
        s.paint = nf;
    }
    double o;
    if (get_f64(v, "opacity", &o)) {
        s.opacity = (float)(o / 100.0);
    }
    size_t alen = 0;
    const char *av = pc_json_as_str(pc_json_get(v, "align"), &alen);
    if (av != NULL) {
        if (ieq(av, "inside", alen)) {
            s.align = PC_STROKE_ALIGN_INSIDE;
        } else if (ieq(av, "outside", alen)) {
            s.align = PC_STROKE_ALIGN_OUTSIDE;
        } else if (ieq(av, "center", alen) || ieq(av, "centre", alen)) {
            s.align = PC_STROKE_ALIGN_CENTER;
        } else {
            snprintf(emsg, ecap, "unknown align `%.*s`", (int)alen, av);
            pc_shape_stroke_fini(&s);
            return -1;
        }
    }
    size_t clen = 0;
    const char *cp = pc_json_as_str(pc_json_get(v, "cap"), &clen);
    if (cp != NULL) {
        if (ieq(cp, "butt", clen)) {
            s.cap = PC_LINE_CAP_BUTT;
        } else if (ieq(cp, "round", clen)) {
            s.cap = PC_LINE_CAP_ROUND;
        } else if (ieq(cp, "square", clen)) {
            s.cap = PC_LINE_CAP_SQUARE;
        } else {
            snprintf(emsg, ecap, "unknown cap `%.*s`", (int)clen, cp);
            pc_shape_stroke_fini(&s);
            return -1;
        }
    }
    size_t jlen = 0;
    const char *jp = pc_json_as_str(pc_json_get(v, "join"), &jlen);
    if (jp != NULL) {
        if (ieq(jp, "miter", jlen)) {
            s.join = PC_LINE_JOIN_MITER;
        } else if (ieq(jp, "round", jlen)) {
            s.join = PC_LINE_JOIN_ROUND;
        } else if (ieq(jp, "bevel", jlen)) {
            s.join = PC_LINE_JOIN_BEVEL;
        } else {
            snprintf(emsg, ecap, "unknown join `%.*s`", (int)jlen, jp);
            pc_shape_stroke_fini(&s);
            return -1;
        }
    }
    double m;
    if (get_f64(v, "miterLimit", &m)) {
        s.miter_limit = (float)m;
    }
    yyjson_val *dv = pc_json_get(v, "dashes");
    if (dv != NULL) {
        if (yyjson_is_null(dv)) {
            free(s.dashes);
            s.dashes = NULL;
            s.ndashes = 0;
        } else if (yyjson_is_arr(dv) && yyjson_arr_size(dv) <= 32) {
            size_t n = yyjson_arr_size(dv);
            float *nd = n > 0 ? malloc(n * sizeof *nd) : NULL;
            if (n > 0 && nd == NULL) {
                snprintf(emsg, ecap, "out of memory");
                pc_shape_stroke_fini(&s);
                return -1;
            }
            for (size_t i = 0; i < n; i++) {
                double x;
                if (!pc_json_as_f64(yyjson_arr_get(dv, i), &x) || !isfinite(x) || x < 0.0 || x > 10000.0) {
                    snprintf(emsg, ecap, "dash lengths must be finite numbers between 0 and 10000 widths");
                    free(nd);
                    pc_shape_stroke_fini(&s);
                    return -1;
                }
                nd[i] = (float)x;
            }
            free(s.dashes);
            s.dashes = nd;
            s.ndashes = n;
        } else {
            snprintf(emsg, ecap, "`dashes` must be an array (multiples of the width)");
            pc_shape_stroke_fini(&s);
            return -1;
        }
    }
    double dof;
    if (get_f64(v, "dashOffset", &dof)) {
        s.dash_offset = (float)dof;
    }
    pc_error ve = {0};
    if (pc_shape_stroke_validate(&s, &ve) != PC_OK) {
        snprintf(emsg, ecap, "%s", ve.msg);
        pc_shape_stroke_fini(&s);
        return -1;
    }
    *out = s;
    return 1;
}

/* ---- live shapes -------------------------------------------------------------------------- */

/* [x,y,w,h] from "rect", else prev's rect; error otherwise. */
static bool live_rect(const char *cmd, yyjson_val *p, const pc_live_shape *prev, double out[4], char *emsg,
                      size_t ecap)
{
    if (nums_n(p, "rect", 4, out)) {
        return true;
    }
    if (prev != NULL &&
        (prev->kind == PC_LIVE_RECT || prev->kind == PC_LIVE_ELLIPSE || prev->kind == PC_LIVE_POLYGON)) {
        memcpy(out, prev->rect, sizeof(double[4]));
        return true;
    }
    (void)cmd;
    snprintf(emsg, ecap, "`rect`: [x, y, w, h] is required");
    return false;
}

/* Build a live shape from kind + geometry. False on error (*emsg carries the bad-params text,
 * already prefixed as `invalid parameters for \`cmd\`: msg` is applied by the caller). */
static bool live_from_params(const char *cmd, const char *kind, yyjson_val *p, const pc_live_shape *prev,
                             pc_live_shape *out, char *emsg, size_t ecap)
{
    pc_live_shape l = {0};
    double rect[4];
    if (!strcmp(kind, "rect") || !strcmp(kind, "rectangle")) {
        if (!live_rect(cmd, p, prev, rect, emsg, ecap)) {
            return false;
        }
        l.kind = PC_LIVE_RECT;
        memcpy(l.rect, rect, sizeof rect);
        double r[4] = {0, 0, 0, 0};
        if (prev != NULL && prev->kind == PC_LIVE_RECT) {
            memcpy(r, prev->radii, sizeof r);
        }
        double rd[4];
        if (read_radii(p, rd)) {
            memcpy(l.radii, rd, sizeof rd);
        } else {
            memcpy(l.radii, r, sizeof r);
        }
    } else if (!strcmp(kind, "roundedRect") || !strcmp(kind, "roundRect")) {
        if (!live_rect(cmd, p, prev, rect, emsg, ecap)) {
            return false;
        }
        l.kind = PC_LIVE_RECT;
        memcpy(l.rect, rect, sizeof rect);
        double r[4] = {10.0, 10.0, 10.0, 10.0};
        if (prev != NULL && prev->kind == PC_LIVE_RECT &&
            (prev->radii[0] > 0.0 || prev->radii[1] > 0.0 || prev->radii[2] > 0.0 || prev->radii[3] > 0.0)) {
            memcpy(r, prev->radii, sizeof r);
        }
        double rd[4];
        if (read_radii(p, rd)) {
            memcpy(l.radii, rd, sizeof rd);
        } else {
            memcpy(l.radii, r, sizeof r);
        }
    } else if (!strcmp(kind, "ellipse")) {
        if (!live_rect(cmd, p, prev, rect, emsg, ecap)) {
            return false;
        }
        l.kind = PC_LIVE_ELLIPSE;
        memcpy(l.rect, rect, sizeof rect);
    } else if (!strcmp(kind, "polygon") || !strcmp(kind, "star")) {
        if (!live_rect(cmd, p, prev, rect, emsg, ecap)) {
            return false;
        }
        bool is_star = !strcmp(kind, "star");
        uint32_t ps = 5;
        double pr = is_star ? 0.5 : 1.0;
        if (prev != NULL && prev->kind == PC_LIVE_POLYGON) {
            ps = prev->sides;
            pr = prev->star_ratio;
        }
        l.kind = PC_LIVE_POLYGON;
        memcpy(l.rect, rect, sizeof rect);
        double sd;
        if (get_f64(p, "sides", &sd)) {
            double cl = round(sd);
            if (cl < 3.0) {
                cl = 3.0;
            }
            if (cl > 100.0) {
                cl = 100.0;
            }
            l.sides = (uint32_t)cl;
        } else {
            l.sides = ps;
        }
        double ratio;
        bool has_sr = get_f64(p, "starRatio", &ratio);
        double rv;
        if (has_sr) {
            rv = ratio < 0.01 ? 0.01 : ratio > 1.0 ? 1.0 : ratio;
        } else {
            rv = (is_star && pr >= 1.0) ? 0.5 : pr;
        }
        l.star_ratio = (!strcmp(kind, "polygon") && !has_sr) ? 1.0 : rv;
    } else if (!strcmp(kind, "line")) {
        double from[2], to[2];
        if (nums_n(p, "from", 2, from)) {
            /* ok */
        } else if (prev != NULL && prev->kind == PC_LIVE_LINE) {
            from[0] = prev->from[0];
            from[1] = prev->from[1];
        } else {
            snprintf(emsg, ecap, "`from`: [x, y] is required");
            return false;
        }
        if (nums_n(p, "to", 2, to)) {
            /* ok */
        } else if (prev != NULL && prev->kind == PC_LIVE_LINE) {
            to[0] = prev->to[0];
            to[1] = prev->to[1];
        } else {
            snprintf(emsg, ecap, "`to`: [x, y] is required");
            return false;
        }
        double w;
        double pw = (prev != NULL && prev->kind == PC_LIVE_LINE) ? prev->weight : 1.0;
        l.kind = PC_LIVE_LINE;
        l.from[0] = from[0];
        l.from[1] = from[1];
        l.to[0] = to[0];
        l.to[1] = to[1];
        l.weight = get_f64(p, "weight", &w) ? (w > 0.0 ? w : 0.0) : pw;
    } else {
        snprintf(emsg, ecap, "unknown kind `%s`", kind);
        return false;
    }
    *out = l;
    return true;
}

static const char *live_kind(const pc_live_shape *l)
{
    switch (l->kind) {
    case PC_LIVE_RECT:
        return (l->radii[0] > 0.0 || l->radii[1] > 0.0 || l->radii[2] > 0.0 || l->radii[3] > 0.0) ? "roundedRect"
                                                                                                   : "rect";
    case PC_LIVE_ELLIPSE:
        return "ellipse";
    case PC_LIVE_POLYGON:
        return l->star_ratio < 1.0 ? "star" : "polygon";
    case PC_LIVE_LINE:
    default:
        return "line";
    }
}

static const char *default_name(const char *kind)
{
    if (!strcmp(kind, "rect") || !strcmp(kind, "rectangle")) {
        return "Rectangle";
    }
    if (!strcmp(kind, "roundedRect") || !strcmp(kind, "roundRect")) {
        return "Rounded Rectangle";
    }
    if (!strcmp(kind, "ellipse")) {
        return "Ellipse";
    }
    if (!strcmp(kind, "polygon")) {
        return "Polygon";
    }
    if (!strcmp(kind, "star")) {
        return "Star";
    }
    if (!strcmp(kind, "line")) {
        return "Line";
    }
    return "Shape";
}

/* ---- session / document helpers ----------------------------------------------------------- */

static pc_status need_doc(const pc_engine_session *s, const pc_document **out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    *out = pc_doc_arc_get(d->doc);
    return PC_OK;
}

static pc_status layer_id_of(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out, pc_error *err)
{
    yyjson_val *lv = pc_json_get(p, "layer");
    uint64_t id;
    if (lv != NULL && pc_json_as_u64(lv, &id)) {
        *out = id;
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && d->has_active_layer) {
        *out = d->active_layer;
        return PC_OK;
    }
    return pc_engine_err_other(err, "no active layer");
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status en_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status en_has_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!d->has_active_layer || pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    return PC_OK;
}

static pc_status en_has_selection(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return pc_doc_arc_get(d->doc)->has_selection ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no selection");
}

/* The active shape layer, or NULL with reason set (active_shape). */
static const pc_shape_layer *active_shape(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        (void)pc_fail(reason, PC_ERR_INVALID, "no document open");
        return NULL;
    }
    if (!d->has_active_layer) {
        (void)pc_fail(reason, PC_ERR_INVALID, "no active layer");
        return NULL;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l == NULL) {
        (void)pc_fail(reason, PC_ERR_INVALID, "no active layer");
        return NULL;
    }
    if (l->content.kind != PC_LAYER_SHAPE) {
        (void)pc_fail(reason, PC_ERR_INVALID, "active layer is not a shape layer");
        return NULL;
    }
    return &l->content.u.shape;
}

static pc_status en_has_shape_fill(const pc_engine_session *s, pc_error *reason)
{
    const pc_shape_layer *sh = active_shape(s, reason);
    if (sh == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    return sh->has_fill ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "shape has no fill");
}

static pc_status en_has_shape_stroke(const pc_engine_session *s, pc_error *reason)
{
    const pc_shape_layer *sh = active_shape(s, reason);
    if (sh == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    return sh->has_stroke ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "shape has no stroke");
}

static pc_status en_can_paste_fill(const pc_engine_session *s, pc_error *reason)
{
    if (active_shape(s, reason) == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    return s->has_path_fill_clip ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no shape fill copied");
}

static pc_status en_can_paste_stroke(const pc_engine_session *s, pc_error *reason)
{
    if (active_shape(s, reason) == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    return s->has_path_stroke_clip ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no shape stroke copied");
}

/* ---- shape info --------------------------------------------------------------------------- */

/* "layer N is {article} {kind} layer, not a shape layer" into out. */
static pc_status not_shape(pc_error *err, pc_layer_id id, const pc_layer *l)
{
    return pc_engine_err_other(err, "layer %llu is %s %s layer, not a shape layer", (unsigned long long)id,
                               pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static pc_status shape_info(const pc_engine_session *s, pc_layer_id id, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SHAPE) {
        return not_shape(err, id, l);
    }
    const pc_shape_layer *sh = &l->content.u.shape;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_strcpy(rd, root, "name", pc_str_c(&l->name));
    yyjson_mut_obj_add_val(rd, root, "path", path_to_json(rd, &sh->path));
    if (sh->has_fill) {
        yyjson_mut_obj_add_val(rd, root, "fill", fill_to_json(rd, &sh->fill));
    } else {
        yyjson_mut_obj_add_null(rd, root, "fill");
    }
    if (sh->has_stroke) {
        yyjson_mut_obj_add_val(rd, root, "stroke", stroke_to_json(rd, &sh->stroke));
    } else {
        yyjson_mut_obj_add_null(rd, root, "stroke");
    }
    if (sh->has_live) {
        yyjson_mut_obj_add_val(rd, root, "live", pc_live_shape_to_json(rd, &sh->live, 0));
        yyjson_mut_obj_add_str(rd, root, "kind", live_kind(&sh->live));
    } else {
        yyjson_mut_obj_add_null(rd, root, "live");
        yyjson_mut_obj_add_str(rd, root, "kind", "path");
    }
    if (sh->has_cache) {
        pc_rect b = pc_surface_content_bounds(&sh->cache);
        if (!pc_rect_is_empty(b)) {
            yyjson_mut_val *ba = yyjson_mut_arr(rd);
            yyjson_mut_arr_add_int(rd, ba, b.x0);
            yyjson_mut_arr_add_int(rd, ba, b.y0);
            yyjson_mut_arr_add_uint(rd, ba, pc_rect_width(b));
            yyjson_mut_arr_add_uint(rd, ba, pc_rect_height(b));
            yyjson_mut_obj_add_val(rd, root, "bounds", ba);
        } else {
            yyjson_mut_obj_add_null(rd, root, "bounds");
        }
    } else {
        yyjson_mut_obj_add_null(rd, root, "bounds");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- with_shape: mutate the shape of a locked-checked layer in one edit step -------------- */

typedef pc_status (*shape_fn)(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err);

typedef struct {
    pc_layer_id id;
    shape_fn f;
    void *ud;
} with_shape_ctx;

static pc_status edit_with_shape(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    with_shape_ctx *ctx = vctx;
    pc_locks locks = pc_document_effective_locks(doc, ctx->id);
    if (locks.all || locks.pixels) {
        return pc_engine_err_other(err, "shape layer is locked");
    }
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (l->content.kind != PC_LAYER_SHAPE) {
        return not_shape(err, ctx->id, l);
    }
    pc_status rc = ctx->f(&l->content.u.shape, l, ctx->ud, err);
    if (rc == PC_OK) {
        pc_engine_refresh_shape(doc, &l->content.u.shape);
    }
    return rc;
}

static pc_status with_shape(pc_engine_session *s, pc_layer_id id, const char *label, shape_fn f, void *ud,
                            pc_error *err)
{
    with_shape_ctx ctx = {id, f, ud};
    return pc_engine_session_edit(s, label, edit_with_shape, &ctx, err);
}

#include "vector_specs_shape.inc"
#include "vector_specs_path.inc"
#include "vector_specs_mask.inc"
#include "path_edit_cmds.inc" /* E11: point-level path edits (reuses the helpers above) */

/* layer.rasterize.shape (vector_cmds.rs): one shape layer goes straight to shape.rasterize; with
 * no `layer`, extra_cmds rasterizes every selected shape layer (calling back here per id). */
static pc_status run_layer_rasterize_shape(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                           pc_error *err)
{
    if (pc_json_get(p, "layer") != NULL) {
        return run_shape_rasterize(s, p, out, err);
    }
    return pc_engine_rasterize_selected_shapes(s, p, out, err);
}

/* ---- registry ----------------------------------------------------------------------------- */

#define FILLDOC                                                                                                        \
    "\"fill\":\"#rrggbb\"|[r,g,b,a]|{\"gradient\":{\"stops\":[[t,\"#hex\"]],\"angle\":deg,\"scale\":%,\"style\":"       \
    "\"linear|radial|angle|reflected|diamond\",\"reverse\":bool}}|{\"pattern\":name}|null (no fill)"
#define STROKEDOC                                                                                                      \
    "\"stroke\":{\"width\":px,\"color\":\"#rrggbb\"|fill,\"opacity\":0..100,\"align\":\"inside|center|outside\","      \
    "\"cap\":\"butt|round|square\",\"join\":\"miter|round|bevel\",\"miterLimit\":n,\"dashes\":[multiples of "          \
    "width],\"dashOffset\":n}|null"
#define PATHFORM                                                                                                       \
    "path: {\"subpaths\":[{\"closed\":bool=true,\"op\":\"combine|subtract|intersect|exclude\",\"knots\":[[x,y] | "     \
    "{\"anchor\":[x,y],\"in\":[x,y],\"out\":[x,y],\"smooth\":bool}]}],\"fillRule\":\"nonzero|evenodd\",\"inverted\":"   \
    "bool}"
#define COMBINE_PARAMS                                                                                                 \
    "{\"layer\":id?,\"subpath\":index? (default all but the first)} (no layer/subpath and two or more shape layers "   \
    "selected: merges them into the top-most, whose components above the bottom layer's take the operation)"

static const char *const MENU_LAYER_NEW[] = {"Layer", "New"};
static const char *const MENU_LAYER_RASTERIZE[] = {"Layer", "Rasterize"};
static const char *const MENU_LAYER_VMASK[] = {"Layer", "Vector Mask"};

static const pc_engine_command SPECS[] = {
    {"shape.create", "New Shape Layer", MENU_LAYER_NEW, 2, NULL,
     "{\"kind\":\"rect|roundedRect|ellipse|polygon|star|line|path\"=\"rect\",\"rect\":[x,y,w,h] "
     "(rect/roundedRect/ellipse/polygon/star),\"radii\":[tl,tr,br,bl]|r (roundedRect=10),\"sides\":3..100=5,"
     "\"starRatio\":0..1 (star=0.5),\"from\":[x,y],\"to\":[x,y],\"weight\":px=1 (line),\"path\":{\xe2\x80\xa6} (kind "
     "path)," FILLDOC "=foreground," STROKEDOC "=none,\"name\":str?,\"addTo\":layerId? + "
     "\"op\":\"combine|subtract|intersect|exclude\" (append to an existing shape layer)} \xe2\x86\x92 shape.info. " PATHFORM,
     en_has_doc, run_shape_create, true},
    {"shape.edit", "Edit Shape", NULL, 0, NULL,
     "{\"layer\":id?,\"path\":{\xe2\x80\xa6}? (replaces; drops live shape),\"kind\":str?,\"rect\":[x,y,w,h]?,"
     "\"radii\":[tl,tr,br,bl]|r?,\"sides\":n?,\"starRatio\":r?,\"from\":[x,y]?,\"to\":[x,y]?,\"weight\":px? "
     "(live-shape params regenerate the path),\"move\":[dx,dy]?,\"transform\":[a,b,c,d,e,f]?,\"op\":\"combine|"
     "\xe2\x80\xa6\"? + \"subpath\":index? (default all but the first),\"fillRule\":\"nonzero|evenodd\"?," FILLDOC
     "?," STROKEDOC "? (merged into the current stroke),\"name\":str?} \xe2\x86\x92 shape.info",
     en_has_layer, run_shape_edit, true},
    {"shape.info", "Shape Layer Info", NULL, 0, NULL,
     "{\"layer\":id?} \xe2\x86\x92 {layer,name,kind,path,fill,stroke,live,bounds:[x,y,w,h]}", en_has_layer,
     run_shape_info, false},
    {"path.list", "List Paths", NULL, 0, NULL,
     "{} \xe2\x86\x92 {paths:[{name,knots,subpaths}],workPath,clippingPath,layerPath (active layer's shape path / "
     "vector mask)}",
     en_has_doc, run_path_list, false},
    {"path.info", "Path Info", NULL, 0, NULL, "{\"name\":str|\"work\"|\"layer\"=\"work\"} \xe2\x86\x92 {name,path}",
     en_has_doc, run_path_info, false},
    {"path.set", "Set Path", NULL, 0, NULL,
     "{\"name\":str|\"work\"=\"work\",\"path\":{\xe2\x80\xa6},\"op\":\"combine|subtract|intersect|exclude\"? (append "
     "to the existing path with this op instead of replacing)}. " PATHFORM,
     en_has_doc, run_path_set, true},
    {"path.delete", "Delete Path", NULL, 0, NULL, "{\"name\":str|\"work\"=\"work\"}", en_has_doc, run_path_delete,
     true},
    {"path.transform", "Free Transform Path", NULL, 0, NULL,
     "{\"name\":str|\"work\"|\"layer\"=\"work\",\"layer\":id? (with layer target),\"matrix\":[a,b,c,d,e,f] | "
     "\"quad\":[[x,y]\xc3\x97" "4] (Distort/Perspective: where the frame's corners go, clockwise from top-left) | "
     "\"style\"|\"mesh\"|\"warp\":\xe2\x80\xa6 (Warp, as edit.transform.warp) | "
     "\"translateX\":px?,\"translateY\":px?,\"scaleX\":factor?,\"scaleY\":factor?,\"angle\":degrees?,\"rect\":[x0,y0,"
     "x1,y1]? (the quad's or warp's frame; default = path control bounds)} (numeric transforms pivot on path bounds "
     "center)",
     en_has_doc, run_path_transform, true},
    {"path.clippingPath.set", "Clipping Path", NULL, 0, NULL,
     "{\"name\":savedPathName,\"flatness\":0..100=0} (PSD export clipping path)", en_has_doc, run_clipping_path_set,
     true},
    {"path.clippingPath.clear", "Clear Clipping Path", NULL, 0, NULL, "{}", en_has_doc, run_clipping_path_clear, true},
    {"path.style.copyFill", "Copy Fill", NULL, 0, NULL, "{} (copies active shape layer fill)", en_has_shape_fill,
     run_copy_fill, true},
    {"path.style.copyStroke", "Copy Complete Stroke", NULL, 0, NULL, "{} (copies active shape layer stroke)",
     en_has_shape_stroke, run_copy_stroke, true},
    {"path.style.pasteFill", "Paste Fill", NULL, 0, NULL,
     "{\"layer\":id?} (pastes copied fill onto active shape layer)", en_can_paste_fill, run_paste_fill, true},
    {"path.style.pasteStroke", "Paste Complete Stroke", NULL, 0, NULL,
     "{\"layer\":id?} (pastes copied stroke onto active shape layer)", en_can_paste_stroke, run_paste_stroke, true},
    {"path.rename", "Rename Path", NULL, 0, NULL,
     "{\"name\":str|\"work\"=\"work\",\"to\":str} (renaming the work path saves it)", en_has_doc, run_path_rename,
     true},
    {"path.toSelection", "Make Selection from Path", NULL, 0, "Cmd+Enter",
     "{\"name\":str|\"work\"|\"layer\"=\"work\",\"feather\":px=0,\"antiAlias\":bool=true,\"mode\":\"replace|add|"
     "subtract|intersect\"=\"replace\"}",
     en_has_doc, run_path_to_selection, true},
    {"select.toWorkPath", "Make Work Path", NULL, 0, NULL,
     "{\"tolerance\":0.5..10 px=2} \xe2\x86\x92 {subpaths,knots}", en_has_selection, run_select_to_work_path, true},
    {"path.fill", "Fill Path", NULL, 0, NULL,
     "{\"name\":str|\"work\"|\"layer\"=\"work\",\"layer\":id? (pixel layer, default "
     "active),\"color\":\"#rrggbb\"=foreground,\"opacity\":0..100=100,\"mode\":\"normal|multiply|\xe2\x80\xa6\"="
     "\"normal\",\"feather\":px=0,\"antiAlias\":bool=true}",
     en_has_layer, run_path_fill, true},
    {"shape.rasterize", "Rasterize Shape", MENU_LAYER_RASTERIZE, 2, NULL, "{\"layer\":id?}", en_has_layer,
     run_shape_rasterize, true},
    {"layer.rasterize.shape", "Rasterize Shape", NULL, 0, NULL,
     "{\"layer\":id?} (no layer: every selected shape layer)", en_has_layer, run_layer_rasterize_shape,
     true},
    {"layer.vectorMask.add", "Add Vector Mask", MENU_LAYER_VMASK, 2, NULL,
     "{\"layer\":id?,\"path\":{\xe2\x80\xa6}? | \"name\":str|\"work\"? (copy a document path),\"hide\":bool=false "
     "(Hide All = inverted)} \xe2\x86\x92 vector mask info. Without a path the mask reveals all.",
     en_has_layer, run_vmask_add, true},
    {"layer.vectorMask.fromPath", "Vector Mask from Current Path", MENU_LAYER_VMASK, 2, NULL,
     "{\"layer\":id?,\"name\":str|\"work\"=\"work\"}", en_has_layer, run_vmask_from_path, true},
    {"layer.vectorMask.edit", "Edit Vector Mask", NULL, 0, NULL,
     "{\"layer\":id?,\"path\":{\xe2\x80\xa6}?,\"enabled\":bool?,\"linked\":bool?,\"density\":0..100?,\"feather\":0.."
     "1000? (pixels),\"invert\":true?}. " PATHFORM,
     en_has_layer, run_vmask_edit, true},
    {"layer.vectorMask.delete", "Delete Vector Mask", MENU_LAYER_VMASK, 2, NULL, "{\"layer\":id?}", en_has_layer,
     run_vmask_delete, true},
    {"layer.rasterize.vectorMask", "Rasterize Vector Mask", NULL, 0, NULL,
     "{\"layer\":id?} (multiplied into the layer mask)", en_has_layer, run_vmask_rasterize, true},
    {"layer.vectorMask.revealAll", "Vector Mask: Reveal All", NULL, 0, NULL, "{\"layer\":id?}", en_has_layer,
     run_vmask_reveal_all, true},
    {"layer.vectorMask.hideAll", "Vector Mask: Hide All", NULL, 0, NULL, "{\"layer\":id?}", en_has_layer,
     run_vmask_hide_all, true},
    {"layer.vectorMask.currentPath", "Vector Mask: Current Path", NULL, 0, NULL,
     "{\"layer\":id?,\"name\":str|\"work\"=\"work\"}", en_has_layer, run_vmask_current_path, true},
    {"layer.vectorMask.enabled", "Enable Vector Mask", NULL, 0, NULL,
     "{\"layer\":id?,\"enabled\":bool? (default: toggle)}", en_has_layer, run_vmask_toggle_enabled, true},
    {"layer.vectorMask.linked", "Link Vector Mask", NULL, 0, NULL, "{\"layer\":id?,\"linked\":bool? (default: toggle)}",
     en_has_layer, run_vmask_toggle_linked, true},
    {"layer.combineShapes.unite", "Unite Shapes", NULL, 0, NULL, COMBINE_PARAMS, en_has_layer, run_combine_unite, true},
    {"layer.combineShapes.subtractFrontShape", "Subtract Front Shape", NULL, 0, NULL, COMBINE_PARAMS, en_has_layer,
     run_combine_subtract, true},
    {"layer.combineShapes.intersectShapeAreas", "Intersect Shape Areas", NULL, 0, NULL, COMBINE_PARAMS, en_has_layer,
     run_combine_intersect, true},
    {"layer.combineShapes.excludeOverlappingShapes", "Exclude Overlapping Shapes", NULL, 0, NULL, COMBINE_PARAMS,
     en_has_layer, run_combine_exclude, true},
    {"layer.combineShapes.mergeShapeComponents", "Merge Shape Components", NULL, 0, NULL,
     "{\"layer\":id?,\"tolerance\":px=0.1} (bakes the path operations into plain combined outlines, traced from the "
     "exact coverage; no layer and several shape layers selected: merges them into the top-most first)",
     en_has_layer, run_merge_components, true},
    {"select.convertToShape", "Convert to Shape", NULL, 0, NULL,
     "{\"tolerance\":0.5..10 px=2} \xe2\x86\x92 shape.info", en_has_selection, run_selection_to_shape, true},
    {"layer.vectorMask.info", "Vector Mask Info", NULL, 0, NULL,
     "{\"layer\":id?} \xe2\x86\x92 {layer,path,enabled,linked,density,feather}", en_has_layer, run_vmask_info, false},
};

const pc_engine_command *pc_engine_vector_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
