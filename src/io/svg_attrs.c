/* Render-tree geometry and attribute helpers (part of the port of usvg 0.45's converter.rs,
 * units.rs and the bounding boxes of tree/mod.rs; see svg_conv.c). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "io/svg_filter.h"
#include "io/svg_internal.h"

#define DPI 96.0f
#define FONT_SIZE 12.0f

/* ---------------------------------------------------------------- bounding boxes */

static bool node_bbox(const sv_unode *n, sv_rect *r)
{
    if (n->kind == UN_IMAGE) {
        return rect_from_xywh(0.0f, 0.0f, n->size_w, n->size_h, r);
    }
    *r = n->bbox;
    return true;
}

static bool node_stroke_bbox(const sv_unode *n, sv_rect *r)
{
    if (n->kind == UN_IMAGE) {
        return rect_from_xywh(0.0f, 0.0f, n->size_w, n->size_h, r);
    }
    *r = n->stroke_bbox;
    return true;
}

static bool node_abs_bbox(const sv_unode *n, sv_rect *r)
{
    *r = n->abs_bbox;
    return true;
}

static bool node_abs_stroke_bbox(const sv_unode *n, sv_rect *r)
{
    *r = n->kind == UN_IMAGE ? n->abs_bbox : n->abs_stroke_bbox;
    return true;
}

bool sv_object_bbox(const sv_unode *g, sv_rect *out)
{
    sv_bbox b = bbox_default();
    size_t i;
    for (i = 0; i < g->nchildren; i++) {
        const sv_unode *ch = g->children[i];
        sv_rect r;
        if (!node_bbox(ch, &r)) {
            continue;
        }
        if (ch->kind == UN_GROUP) {
            sv_rect t;
            if (rect_transform(r, ch->transform, &t)) {
                r = t;
            }
        }
        b = bbox_expand(b, r);
    }
    return bbox_to_nz(b, out);
}

bool sv_group_bboxes(sv_unode *g)
{
    sv_bbox bbox = bbox_default();
    sv_bbox abs_bbox = bbox_default();
    sv_bbox stroke_bbox = bbox_default();
    sv_bbox abs_stroke_bbox = bbox_default();
    sv_bbox layer_bbox = bbox_default();
    sv_rect r;
    size_t i;
    for (i = 0; i < g->nchildren; i++) {
        const sv_unode *ch = g->children[i];
        sv_rect t;
        if (node_bbox(ch, &r)) {
            if (ch->kind == UN_GROUP && rect_transform(r, ch->transform, &t)) {
                r = t;
            }
            bbox = bbox_expand(bbox, r);
        }
        if (node_abs_bbox(ch, &r)) {
            abs_bbox = bbox_expand(abs_bbox, r);
        }
        if (node_stroke_bbox(ch, &r)) {
            if (ch->kind == UN_GROUP && rect_transform(r, ch->transform, &t)) {
                r = t;
            }
            stroke_bbox = bbox_expand(stroke_bbox, r);
        }
        if (node_abs_stroke_bbox(ch, &r)) {
            abs_stroke_bbox = bbox_expand(abs_stroke_bbox, r);
        }
        if (ch->kind == UN_GROUP) {
            if (nzrect_transform(ch->layer_bbox, ch->transform, &t)) {
                layer_bbox = bbox_expand(layer_bbox, t);
            }
        } else if (node_stroke_bbox(ch, &r)) {
            layer_bbox = bbox_expand(layer_bbox, r);
        }
    }
    if (bbox_to_rect(bbox, &r)) {
        g->bbox = r;
        if (!bbox_to_rect(abs_bbox, &g->abs_bbox) || !bbox_to_rect(stroke_bbox, &g->stroke_bbox) ||
            !bbox_to_rect(abs_stroke_bbox, &g->abs_stroke_bbox)) {
            return false;
        }
    }
    if (g->nfilters > 0) {
        sv_bbox fb = bbox_default();
        for (i = 0; i < g->nfilters; i++) {
            fb = bbox_expand(fb, g->filters[i]->rect);
        }
        if (!bbox_to_nz(fb, &g->layer_bbox)) {
            return false;
        }
    } else if (!bbox_to_nz(layer_bbox, &g->layer_bbox)) {
        return false;
    }
    return nzrect_transform(g->layer_bbox, g->abs_transform, &g->abs_layer_bbox);
}

/* The bounds of the stroke outline (dashes ignored). The reference strokes the path and takes
 * the outline's tight bounds; this pads the path's tight bounds by the farthest the outline can
 * reach (half the width, times the miter limit for miter joins, times sqrt 2 for square caps).
 * It only sizes rasterised layers, which stay large enough. */
static bool stroke_bbox(const sv_stroke *s, const sv_path *p, sv_rect *out)
{
    sv_rect r;
    float k = 1.0f;
    float pad;
    if (s == NULL || !path_tight_bounds(p, &r)) {
        return false;
    }
    if (s->join == JOIN_MITER || s->join == JOIN_MITER_CLIP) {
        k = s->miterlimit > k ? s->miterlimit : k;
    }
    if (s->cap == CAP_SQUARE && k < 1.41421356f) {
        k = 1.41421356f;
    }
    pad = s->width / 2.0f * k;
    return rect_from_ltrb(r.l - pad, r.t - pad, r.r + pad, r.b + pad, out);
}

sv_unode *sv_new_path(sv_conv *c, const char *id, bool visible, sv_fill *fill, sv_stroke *stroke,
                      bool stroke_first, const sv_path *data, sv_ts abs)
{
    sv_unode *n;
    sv_rect bb;
    sv_rect sbb;
    if (!path_tight_bounds(data, &bb)) {
        return NULL;
    }
    n = sv_alloc(c->a, sizeof *n);
    if (n == NULL) {
        c->nomem = true;
        return NULL;
    }
    n->kind = UN_PATH;
    n->id = id;
    n->visible = visible;
    n->fill = fill;
    n->stroke = stroke;
    n->stroke_first = stroke_first;
    n->data = data;
    n->abs_transform = abs;
    n->transform = ts_identity();
    n->opacity = 1.0f;
    n->bbox = bb;
    if (!stroke_bbox(stroke, data, &sbb)) {
        sbb = bb;
    }
    n->stroke_bbox = sbb;
    if (ts_has_skew(abs)) {
        const sv_path *p2 = path_transform(c->a, data, abs);
        if (p2 == NULL || !path_tight_bounds(p2, &n->abs_bbox)) {
            if (c->a->nomem) {
                c->nomem = true;
            }
            return NULL;
        }
        if (!stroke_bbox(stroke, p2, &n->abs_stroke_bbox)) {
            n->abs_stroke_bbox = n->abs_bbox;
        }
    } else if (!rect_transform(bb, abs, &n->abs_bbox) || !rect_transform(sbb, abs, &n->abs_stroke_bbox)) {
        return NULL;
    }
    return n;
}

/* ---------------------------------------------------------------- units */

static float named_font_size(const char *name, float parent)
{
    int f;
    if (strcmp(name, "xx-small") == 0) {
        f = -3;
    } else if (strcmp(name, "x-small") == 0) {
        f = -2;
    } else if (strcmp(name, "small") == 0 || strcmp(name, "smaller") == 0) {
        f = -1;
    } else if (strcmp(name, "large") == 0 || strcmp(name, "larger") == 0) {
        f = 1;
    } else if (strcmp(name, "x-large") == 0) {
        f = 2;
    } else if (strcmp(name, "xx-large") == 0) {
        f = 3;
    } else {
        f = 0;
    }
    {
        /* f32::powi (compiler-rt __powisf2: square-and-multiply in f32) */
        float r = 1.0f;
        float b = 1.2f;
        int e = f < 0 ? -f : f;
        for (;;) {
            if ((e & 1) != 0) {
                r *= b;
            }
            e /= 2;
            if (e == 0) {
                break;
            }
            b *= b;
        }
        return parent * (f < 0 ? 1.0f / r : r);
    }
}

static float resolve_font_size(const st_node *n)
{
    const st_node *chain[1100];
    size_t k = 0;
    const st_node *p;
    float fs = FONT_SIZE;
    for (p = n; p != NULL && k < sizeof chain / sizeof chain[0]; p = p->parent) {
        chain[k++] = p;
    }
    /* Root first, the root node itself skipped: chain[k - 1] is the root. */
    while (k > 1) {
        const st_node *q;
        const char *v;
        sv_length l;
        k--;
        q = chain[k - 1];
        v = st_attr(q, A_FONT_SIZE);
        if (v != NULL && sv_parse_length(v, &l)) {
            float num = (float)l.number;
            switch (l.unit) {
            case U_EM:
                fs = num * fs;
                break;
            case U_EX:
                fs = num * fs / 2.0f;
                break;
            case U_IN:
                fs = num * DPI;
                break;
            case U_CM:
                fs = num * DPI / 2.54f;
                break;
            case U_MM:
                fs = num * DPI / 25.4f;
                break;
            case U_PT:
                fs = num * DPI / 72.0f;
                break;
            case U_PC:
                fs = num * DPI / 6.0f;
                break;
            case U_PERCENT:
                fs = (float)l.number * fs * 0.01f;
                break;
            case U_NONE:
            case U_PX:
            default:
                fs = num;
                break;
            }
        } else if (v != NULL) {
            fs = named_font_size(v, fs);
        }
    }
    return fs;
}

float sv_resolve_font_size(const st_node *n)
{
    return resolve_font_size(n);
}

float sv_convert_length(sv_conv *c, sv_length l, const st_node *n, sv_aid aid, sv_units units,
                        const sv_cstate *st)
{
    float num = (float)l.number;
    (void)c;
    switch (l.unit) {
    case U_EM:
        return num * resolve_font_size(n);
    case U_EX:
        return num * resolve_font_size(n) / 2.0f;
    case U_IN:
        return num * DPI;
    case U_CM:
        return num * DPI / 2.54f;
    case U_MM:
        return num * DPI / 25.4f;
    case U_PT:
        return num * DPI / 72.0f;
    case U_PC:
        return num * DPI / 6.0f;
    case U_PERCENT:
        if (units == UNITS_OBB) {
            return num / 100.0f;
        }
        switch (aid) {
        case A_CX:
        case A_DX:
        case A_FX:
        case A_MARKER_WIDTH:
        case A_REF_X:
        case A_RX:
        case A_WIDTH:
        case A_X:
        case A_X1:
        case A_X2:
            return rect_w(st->view_box) * num / 100.0f;
        case A_CY:
        case A_DY:
        case A_FY:
        case A_HEIGHT:
        case A_MARKER_HEIGHT:
        case A_REF_Y:
        case A_RY:
        case A_Y:
        case A_Y1:
        case A_Y2:
            return rect_h(st->view_box) * num / 100.0f;
        default: {
            float w = rect_w(st->view_box);
            float h = rect_h(st->view_box);
            float len = sqrtf((w * w + h * h) / 2.0f);
            return len * num / 100.0f;
        }
        }
    case U_NONE:
    case U_PX:
    default:
        return num;
    }
}

float sv_node_length(sv_conv *c, const st_node *n, sv_aid aid, sv_units units, const sv_cstate *st,
                     sv_length def)
{
    const char *v = st_attr(n, aid);
    sv_length l = def;
    if (v != NULL && !sv_parse_length(v, &l)) {
        l = def;
    }
    return sv_convert_length(c, l, n, aid, units, st);
}

float sv_user_length(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st, sv_length def)
{
    return sv_node_length(c, n, aid, UNITS_USER, st, def);
}

/* ---------------------------------------------------------------- attributes */

bool sv_parse_opacity(const char *v, float *out)
{
    sv_length l;
    float x;
    if (v == NULL || !sv_parse_length(v, &l)) {
        return false;
    }
    if (l.unit == U_PERCENT) {
        x = (float)l.number / 100.0f;
    } else if (l.unit == U_NONE) {
        x = (float)l.number;
    } else {
        return false;
    }
    *out = isnan(x) ? 0.0f : x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return true;
}

float sv_attr_opacity(const st_node *n, sv_aid aid)
{
    float o;
    return sv_parse_opacity(st_attr(n, aid), &o) ? o : 1.0f;
}

static bool parse_ts(const char *v, sv_ts *out, bool *valid)
{
    double m[6];
    sv_ts t;
    if (v == NULL || !sv_parse_transform(v, m)) {
        return false;
    }
    t = ts_row((float)m[0], (float)m[1], (float)m[2], (float)m[3], (float)m[4], (float)m[5]);
    *valid = ts_is_valid(t);
    *out = *valid ? t : ts_identity();
    return true;
}

/* transform-origin: one or two of left/center/right/top/bottom or lengths. */
static bool transform_origin(const char *v, sv_length *x, sv_length *y)
{
    static const char *const KW[5] = {"left", "right", "top", "bottom", "center"};
    sv_stream st;
    sv_length part[2];
    int kind[2]; /* -1 length, else keyword index */
    int n = 0;
    ss_init(&st, v);
    if (ss_at_end(&st)) {
        return false;
    }
    while (n < 2) {
        int k;
        bool got = false;
        ss_skip_spaces(&st);
        for (k = 0; k < 5; k++) {
            size_t l = strlen(KW[k]);
            if (st.len - st.pos >= l && memcmp(st.s + st.pos, KW[k], l) == 0) {
                st.pos += l;
                kind[n] = k;
                part[n].number = k == 0 || k == 2 ? 0.0 : k == 4 ? 50.0 : 100.0;
                part[n].unit = U_PERCENT;
                got = true;
                break;
            }
        }
        if (!got) {
            if (!ss_parse_length(&st, &part[n])) {
                if (n == 0) {
                    return false;
                }
                return false;
            }
            kind[n] = -1;
        }
        n++;
        if (ss_at_end(&st)) {
            break;
        }
        ss_skip_spaces(&st);
        if (!ss_at_end(&st) && st.s[st.pos] == ',') {
            st.pos++;
        }
        if (ss_at_end(&st)) {
            break;
        }
    }
    ss_skip_spaces(&st);
    if (!ss_at_end(&st)) {
        sv_length z;
        if (!ss_parse_length(&st, &z)) {
            return false;
        }
        ss_skip_spaces(&st);
        if (!ss_at_end(&st)) {
            return false;
        }
    }
    if (n == 1) {
        bool vertical = kind[0] == 2 || kind[0] == 3;
        sv_length center = {50.0, U_PERCENT};
        *x = vertical ? center : part[0];
        *y = vertical ? part[0] : center;
        return true;
    }
    {
        bool h0 = kind[0] == -1 || kind[0] == 0 || kind[0] == 1 || kind[0] == 4;
        bool v1 = kind[1] == -1 || kind[1] == 2 || kind[1] == 3 || kind[1] == 4;
        if (h0 && v1) {
            *x = part[0];
            *y = part[1];
        } else {
            *x = part[1];
            *y = part[0];
        }
    }
    return true;
}

sv_ts sv_resolve_transform(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st)
{
    sv_ts t = ts_identity();
    bool valid;
    sv_length ox;
    sv_length oy;
    const char *origin;
    if (!parse_ts(st_attr(n, aid), &t, &valid)) {
        t = ts_identity();
    }
    origin = st_attr(n, A_TRANSFORM_ORIGIN);
    if (origin != NULL && transform_origin(origin, &ox, &oy)) {
        float dx = sv_convert_length(c, ox, n, A_WIDTH, UNITS_USER, st);
        float dy = sv_convert_length(c, oy, n, A_HEIGHT, UNITS_USER, st);
        t = ts_pre_translate(ts_pre_concat(ts_translate(dx, dy), t), -dx, -dy);
    }
    return t;
}

static bool has_valid_transform(const st_node *n)
{
    sv_ts t;
    bool valid;
    if (!parse_ts(st_attr(n, A_TRANSFORM), &t, &valid)) {
        return true;
    }
    return valid;
}

static bool feature_known(const char *f, size_t n)
{
    static const char *const FEATURES[] = {
        "http://www.w3.org/TR/SVG11/feature#SVGDOM-static",
        "http://www.w3.org/TR/SVG11/feature#SVG-static",
        "http://www.w3.org/TR/SVG11/feature#CoreAttribute",
        "http://www.w3.org/TR/SVG11/feature#Structure",
        "http://www.w3.org/TR/SVG11/feature#BasicStructure",
        "http://www.w3.org/TR/SVG11/feature#ContainerAttribute",
        "http://www.w3.org/TR/SVG11/feature#ConditionalProcessing",
        "http://www.w3.org/TR/SVG11/feature#Image",
        "http://www.w3.org/TR/SVG11/feature#Style",
        "http://www.w3.org/TR/SVG11/feature#Shape",
        "http://www.w3.org/TR/SVG11/feature#Text",
        "http://www.w3.org/TR/SVG11/feature#BasicText",
        "http://www.w3.org/TR/SVG11/feature#PaintAttribute",
        "http://www.w3.org/TR/SVG11/feature#BasicPaintAttribute",
        "http://www.w3.org/TR/SVG11/feature#OpacityAttribute",
        "http://www.w3.org/TR/SVG11/feature#GraphicsAttribute",
        "http://www.w3.org/TR/SVG11/feature#BasicGraphicsAttribute",
        "http://www.w3.org/TR/SVG11/feature#Marker",
        "http://www.w3.org/TR/SVG11/feature#Gradient",
        "http://www.w3.org/TR/SVG11/feature#Pattern",
        "http://www.w3.org/TR/SVG11/feature#Clip",
        "http://www.w3.org/TR/SVG11/feature#BasicClip",
        "http://www.w3.org/TR/SVG11/feature#Mask",
        "http://www.w3.org/TR/SVG11/feature#Filter",
        "http://www.w3.org/TR/SVG11/feature#BasicFilter",
        "http://www.w3.org/TR/SVG11/feature#XlinkAttribute",
    };
    size_t i;
    for (i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++) {
        if (strlen(FEATURES[i]) == n && memcmp(FEATURES[i], f, n) == 0) {
            return true;
        }
    }
    return false;
}

bool sv_condition_passed(const st_node *n)
{
    const char *f;
    const char *lang;
    if (n->kind != ST_ELEMENT || st_has(n, A_REQUIRED_EXTENSIONS)) {
        return false;
    }
    f = st_attr(n, A_REQUIRED_FEATURES);
    if (f != NULL) {
        const char *p = f;
        for (;;) {
            const char *sp = strchr(p, ' ');
            size_t len = sp != NULL ? (size_t)(sp - p) : strlen(p);
            if (!feature_known(p, len)) {
                return false;
            }
            if (sp == NULL) {
                break;
            }
            p = sp + 1;
        }
    }
    lang = st_attr(n, A_SYSTEM_LANGUAGE);
    if (lang != NULL) {
        const char *p = lang;
        for (;;) {
            const char *cm = strchr(p, ',');
            size_t len = cm != NULL ? (size_t)(cm - p) : strlen(p);
            size_t s0 = 0;
            const char *dash;
            while (s0 < len && (p[s0] == ' ' || p[s0] == '\t' || p[s0] == '\n' || p[s0] == '\r')) {
                s0++;
            }
            while (len > s0 &&
                   (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n' || p[len - 1] == '\r')) {
                len--;
            }
            if (len - s0 == 2 && memcmp(p + s0, "en", 2) == 0) {
                return true;
            }
            dash = memchr(p + s0, '-', len - s0);
            if (dash != NULL && dash - (p + s0) == 2 && memcmp(p + s0, "en", 2) == 0) {
                return true;
            }
            if (cm == NULL) {
                return false;
            }
            p = cm + 1;
        }
    }
    return true;
}

bool sv_is_visible_element(const st_node *n)
{
    const char *d = st_attr(n, A_DISPLAY);
    return !(d != NULL && strcmp(d, "none") == 0) && has_valid_transform(n) && sv_condition_passed(n);
}

sv_blend sv_blend_of(const st_node *n)
{
    static const char *const NAMES[16] = {"normal",     "multiply",   "screen",      "overlay",
                                          "darken",     "lighten",    "color-dodge", "color-burn",
                                          "hard-light", "soft-light", "difference",  "exclusion",
                                          "hue",        "saturation", "color",       "luminosity"};
    const char *v = st_attr(n, A_MIX_BLEND_MODE);
    int i;
    for (i = 0; v != NULL && i < 16; i++) {
        if (strcmp(v, NAMES[i]) == 0) {
            return (sv_blend)i;
        }
    }
    return BM_NORMAL;
}

bool sv_viewbox_of(const st_node *n, sv_rect *out)
{
    sv_viewbox vb;
    const char *v = st_attr(n, A_VIEW_BOX);
    if (v == NULL || !sv_parse_viewbox(v, &vb)) {
        return false;
    }
    return nzrect_from_xywh((float)vb.x, (float)vb.y, (float)vb.w, (float)vb.h, out);
}

sv_aspect sv_aspect_of(const st_node *n)
{
    sv_aspect a = sv_aspect_default();
    const char *v = st_attr(n, A_PRESERVE_ASPECT_RATIO);
    if (v != NULL && !sv_parse_aspect(v, &a)) {
        a = sv_aspect_default();
    }
    return a;
}

void sv_aligned_pos(sv_align al, float x, float y, float w, float h, float *ox, float *oy)
{
    *ox = x;
    *oy = y;
    switch (al) {
    case AL_XMIDYMIN:
        *ox = x + w / 2.0f;
        break;
    case AL_XMAXYMIN:
        *ox = x + w;
        break;
    case AL_XMINYMID:
        *oy = y + h / 2.0f;
        break;
    case AL_XMIDYMID:
        *ox = x + w / 2.0f;
        *oy = y + h / 2.0f;
        break;
    case AL_XMAXYMID:
        *ox = x + w;
        *oy = y + h / 2.0f;
        break;
    case AL_XMINYMAX:
        *oy = y + h;
        break;
    case AL_XMIDYMAX:
        *ox = x + w / 2.0f;
        *oy = y + h;
        break;
    case AL_XMAXYMAX:
        *ox = x + w;
        *oy = y + h;
        break;
    case AL_NONE:
    case AL_XMINYMIN:
    default:
        break;
    }
}

sv_ts sv_viewbox_transform(sv_rect vr, sv_aspect aspect, float w, float h)
{
    float sx = w / rect_w(vr);
    float sy = h / rect_h(vr);
    float x;
    float y;
    float tx;
    float ty;
    if (aspect.align != AL_NONE) {
        float s = aspect.slice ? (sx < sy ? sy : sx) : (sx > sy ? sy : sx);
        sx = s;
        sy = s;
    }
    x = -vr.l * sx;
    y = -vr.t * sy;
    sv_aligned_pos(aspect.align, x, y, w - rect_w(vr) * sx, h - rect_h(vr) * sy, &tx, &ty);
    return ts_row(sx, 0.0f, 0.0f, sy, tx, ty);
}
