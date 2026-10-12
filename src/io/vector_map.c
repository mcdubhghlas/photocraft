/* Vector data <-> PSD (port of crates/io/src/vector_map.rs): path records (vector masks
 * vmsk / vsms, saved paths 2000-2997, the work path 1025), live-shape origination (vogk), shape
 * fill content (vscg) and shape strokes (vstk).
 *
 * Export keeps a preserved block byte-identical while it still decodes to the layer's model and
 * regenerates it otherwise. The reference's Option results are a *found flag here; statuses
 * other than PC_OK are resource failures (or I2's stubs while blocks.c is pending). */
#include "io/vector_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "io/blocks.h"
#include "io/maps_desc.h"

#define FLAG_INVERT 1u
#define FLAG_NOT_LINKED 2u
#define FLAG_DISABLED 4u

static pc_path_op op_from_psd(int16_t v)
{
    switch (v) {
    case 0:
        return PC_PATH_EXCLUDE;
    case 2:
        return PC_PATH_SUBTRACT;
    case 3:
        return PC_PATH_INTERSECT;
    case -1: /* continues the previous subpath's shape component */
        return PC_PATH_JOIN;
    default:
        return PC_PATH_COMBINE;
    }
}

static int16_t op_to_psd(pc_path_op op)
{
    switch (op) {
    case PC_PATH_COMBINE:
        return 1;
    case PC_PATH_SUBTRACT:
        return 2;
    case PC_PATH_INTERSECT:
        return 3;
    case PC_PATH_EXCLUDE:
        return 0;
    case PC_PATH_JOIN:
        return -1;
    }
    return 1;
}

pc_status pc_io_path_from_records(const pc_psd_path *p, uint32_t w, uint32_t h, pc_path *out, pc_error *err)
{
    double fw = (double)w;
    double fh = (double)h;
    pc_psd_subpath *subs = NULL;
    size_t n = 0;
    pc_path path;
    pc_status st = pc_psd_path_subpaths(p, &subs, &n, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&path, 0, sizeof path);
    if (n > 0) {
        path.subpaths = pc_calloc_array(n, sizeof *path.subpaths);
        if (path.subpaths == NULL) {
            pc_psd_subpaths_free(subs, n);
            return pc_io_nomem(err, "reading path records");
        }
    }
    for (size_t i = 0; i < n; i++) {
        pc_subpath *s = &path.subpaths[i];
        s->closed = subs[i].closed;
        s->op = op_from_psd(subs[i].operation);
        if (subs[i].n > 0) {
            s->knots = pc_malloc_array(subs[i].n, sizeof *s->knots);
            if (s->knots == NULL) {
                path.nsubpaths = i;
                pc_path_fini(&path);
                pc_psd_subpaths_free(subs, n);
                return pc_io_nomem(err, "reading path records");
            }
        }
        s->nknots = subs[i].n;
        for (size_t k = 0; k < subs[i].n; k++) {
            const pc_psd_knot *q = &subs[i].knots[k];
            pc_knot *kn = &s->knots[k];
            kn->anchor = (pc_point){q->anchor[0] * fw, q->anchor[1] * fh};
            kn->in_ctrl = (pc_point){q->pre[0] * fw, q->pre[1] * fh};
            kn->out_ctrl = (pc_point){q->post[0] * fw, q->post[1] * fh};
            kn->smooth = q->linked;
        }
    }
    path.nsubpaths = n;
    path.fill_rule = PC_FILL_RULE_NONZERO;
    path.inverted = pc_psd_path_initial_fill(p);
    pc_psd_subpaths_free(subs, n);
    *out = path;
    return PC_OK;
}

pc_status pc_io_path_to_records(const pc_path *p, uint32_t w, uint32_t h, pc_psd_path *out, pc_error *err)
{
    double fw = (double)(w > 1 ? w : 1);
    double fh = (double)(h > 1 ? h : 1);
    pc_psd_subpath *subs = NULL;
    pc_psd_path rec;
    pc_status st;
    if (p->nsubpaths > 0) {
        subs = pc_calloc_array(p->nsubpaths, sizeof *subs);
        if (subs == NULL) {
            return pc_io_nomem(err, "writing path records");
        }
    }
    for (size_t i = 0; i < p->nsubpaths; i++) {
        const pc_subpath *s = &p->subpaths[i];
        subs[i].closed = s->closed;
        subs[i].operation = op_to_psd(s->op);
        if (s->nknots > 0) {
            subs[i].knots = pc_malloc_array(s->nknots, sizeof *subs[i].knots);
            if (subs[i].knots == NULL) {
                pc_psd_subpaths_free(subs, i);
                return pc_io_nomem(err, "writing path records");
            }
        }
        subs[i].n = s->nknots;
        for (size_t k = 0; k < s->nknots; k++) {
            const pc_knot *kn = &s->knots[k];
            pc_psd_knot *q = &subs[i].knots[k];
            q->linked = kn->smooth;
            q->pre[0] = kn->in_ctrl.x / fw;
            q->pre[1] = kn->in_ctrl.y / fh;
            q->anchor[0] = kn->anchor.x / fw;
            q->anchor[1] = kn->anchor.y / fh;
            q->post[0] = kn->out_ctrl.x / fw;
            q->post[1] = kn->out_ctrl.y / fh;
        }
    }
    st = pc_psd_path_from_subpaths(subs, p->nsubpaths, p->inverted, &rec, err);
    pc_psd_subpaths_free(subs, p->nsubpaths);
    if (st == PC_OK) {
        *out = rec;
    }
    return st;
}

/* Status of a src/psd parse whose failure is the reference's None: only NOMEM is an error. */
static pc_status soft(pc_status st, bool *found)
{
    *found = st == PC_OK;
    return st == PC_ERR_NOMEM ? st : PC_OK;
}

pc_status pc_io_path_from_resource(const uint8_t *data, size_t len, uint32_t w, uint32_t h, bool *found,
                                   pc_path *out, pc_error *err)
{
    pc_psd_path p;
    bool ok;
    pc_status st = soft(pc_psd_path_parse(data, len, &p, err), &ok);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    st = pc_io_path_from_records(&p, w, h, out, err);
    pc_psd_path_fini(&p);
    if (st == PC_OK) {
        *found = true;
    }
    return st;
}

/* Layer vector data with Photoshop's reading of the records (matching its rendering of the
 * psd-tools corpus): without subpaths the initial-fill record decides coverage; with subpaths it
 * is ignored and a first component that subtracts starts from a full canvas (the shape is
 * inverted, later operations replaced by their complement duals: combine <-> subtract; exclude
 * is self-dual; intersect has no dual and stays); -1 continues the previous component. */
static void layer_path_fixup(pc_path *path)
{
    if (path->nsubpaths == 0) {
        return;
    }
    path->inverted = path->subpaths[0].op == PC_PATH_SUBTRACT;
    if (path->inverted) {
        for (size_t i = 1; i < path->nsubpaths; i++) {
            pc_path_op *op = &path->subpaths[i].op;
            if (*op == PC_PATH_COMBINE) {
                *op = PC_PATH_SUBTRACT;
            } else if (*op == PC_PATH_SUBTRACT) {
                *op = PC_PATH_COMBINE;
            }
        }
        path->subpaths[0].op = PC_PATH_COMBINE;
    }
}

pc_status pc_io_path_from_vmsk(const uint8_t *data, size_t len, uint32_t w, uint32_t h, bool *found,
                               pc_path *out, uint32_t *flags, pc_error *err)
{
    pc_psd_vector_mask b;
    pc_path p;
    bool ok;
    pc_status st = soft(pc_psd_vector_mask_parse(data, len, &b, err), &ok);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    st = pc_io_path_from_records(&b.path, w, h, &p, err);
    pc_psd_path_fini(&b.path);
    if (st != PC_OK) {
        return st;
    }
    layer_path_fixup(&p);
    if ((b.flags & FLAG_INVERT) != 0) {
        p.inverted = !p.inverted;
    }
    *out = p;
    if (flags != NULL) {
        *flags = b.flags;
    }
    *found = true;
    return PC_OK;
}

pc_status pc_io_vmsk_bytes(const pc_path *path, uint32_t flags, uint32_t w, uint32_t h, pc_buf *out,
                           pc_error *err)
{
    pc_path p;
    pc_psd_vector_mask m;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_path_clone(path, &p, err);
    if (st != PC_OK) {
        return st;
    }
    flags &= ~FLAG_INVERT;
    if (p.inverted && p.nsubpaths > 0) {
        flags |= FLAG_INVERT;
        p.inverted = false;
    }
    /* The model draws the first subpath as "combine"; a stored leading subtract would read back
     * as an inverted shape. */
    if (p.nsubpaths > 0 && p.subpaths[0].op == PC_PATH_SUBTRACT) {
        p.subpaths[0].op = PC_PATH_COMBINE;
    }
    m.version = 3;
    m.flags = flags;
    st = pc_io_path_to_records(&p, w, h, &m.path, err);
    pc_path_fini(&p);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_vector_mask_to_bytes(&m, &bytes, err);
    pc_psd_path_fini(&m.path);
    if (st != PC_OK) {
        return st;
    }
    st = pc_buf_append(&b, bytes.data, bytes.len, err);
    pc_psd_bytes_fini(&bytes);
    if (st != PC_OK) {
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

pc_status pc_io_vector_mask_from_block(const uint8_t *data, size_t len, uint32_t w, uint32_t h, bool *found,
                                       pc_vector_mask *out, pc_error *err)
{
    pc_path path;
    uint32_t flags = 0;
    bool ok = false;
    pc_vector_mask m;
    pc_status st = pc_io_path_from_vmsk(data, len, w, h, &ok, &path, &flags, err);
    if (st != PC_OK) {
        return st;
    }
    if (!ok) {
        *found = false;
        return PC_OK;
    }
    /* A vector mask without subpaths reveals everything unless inverted, the opposite of a
     * shape's coverage, hence the flip. */
    if (path.nsubpaths == 0) {
        path.inverted = !path.inverted;
    }
    pc_vector_mask_init(&m, &path);
    m.enabled = (flags & FLAG_DISABLED) == 0;
    m.linked = (flags & FLAG_NOT_LINKED) == 0;
    m.density = 1.0f;
    m.feather = 0.0f;
    *out = m;
    *found = true;
    return PC_OK;
}

pc_status pc_io_vector_mask_bytes(const pc_vector_mask *m, uint32_t w, uint32_t h, pc_buf *out, pc_error *err)
{
    uint32_t flags = 0;
    pc_path path;
    pc_status st;
    if (!m->enabled) {
        flags |= FLAG_DISABLED;
    }
    if (!m->linked) {
        flags |= FLAG_NOT_LINKED;
    }
    st = pc_path_clone(&m->path, &path, err);
    if (st != PC_OK) {
        return st;
    }
    if (path.nsubpaths == 0) {
        path.inverted = !path.inverted;
    }
    st = pc_io_vmsk_bytes(&path, flags, w, h, out, err);
    pc_path_fini(&path);
    return st;
}

pc_status pc_io_vector_mask_block_matches(const uint8_t *data, size_t len, const pc_vector_mask *m,
                                          uint32_t w, uint32_t h, bool *out, pc_error *err)
{
    pc_vector_mask b;
    bool ok = false;
    pc_status st = pc_io_vector_mask_from_block(data, len, w, h, &ok, &b, err);
    if (st != PC_OK) {
        return st;
    }
    *out = ok && pc_path_eq(&b.path, &m->path) && b.enabled == m->enabled && b.linked == m->linked;
    if (ok) {
        pc_vector_mask_fini(&b);
    }
    return PC_OK;
}

/* ---- vscg (shape fill content): content key + version-16 descriptor ---- */

pc_status pc_io_fill_from_vscg(const uint8_t *data, size_t len, bool *found, pc_fill *out, pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    pc_status st;
    if (len < 4) {
        *found = false;
        return PC_OK;
    }
    st = pc_io4_vdesc_prefix(data + 4, len - 4, &ok, &d, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    st = pc_io_fill_from_desc(data, &d, found, out, err);
    pc_psd_desc_fini(&d);
    return st;
}

pc_status pc_io_vscg_bytes(const pc_fill *f, pc_buf *out, pc_error *err)
{
    uint8_t key[4];
    pc_psd_desc d;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_io_fill_to_desc(f, key, &d, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_vdesc_to_bytes(&d, &bytes, err);
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        return st;
    }
    st = pc_buf_append(&b, key, 4, err);
    if (st == PC_OK) {
        st = pc_buf_append(&b, bytes.data, bytes.len, err);
    }
    pc_psd_bytes_fini(&bytes);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---- vstk (shape stroke) ---- */

void pc_io_vstk_fini(pc_io_vstk *v)
{
    if (v != NULL && v->has_stroke) {
        pc_shape_stroke_fini(&v->stroke);
        v->has_stroke = false;
    }
}

static pc_status content_fill(const pc_psd_desc *d, bool *found, pc_fill *out, pc_error *err)
{
    size_t n = 0;
    const uint8_t *c = pc_psd_id_bytes(&d->class_id, &n);
    const char *key;
    if (n == 15 && memcmp(c, "solidColorLayer", 15) == 0) {
        key = "SoCo";
    } else if (n == 13 && memcmp(c, "gradientLayer", 13) == 0) {
        key = "GdFl";
    } else if (n == 12 && memcmp(c, "patternLayer", 12) == 0) {
        key = "PtFl";
    } else if (pc_psd_desc_get(d, "Clr ") != NULL) {
        key = "SoCo";
    } else if (pc_psd_desc_get(d, "Grad") != NULL) {
        key = "GdFl";
    } else {
        key = "PtFl";
    }
    return pc_io_fill_from_desc((const uint8_t *)key, d, found, out, err);
}

static bool unit_px(const pc_psd_value *v, float dpi, double *out)
{
    if (v == NULL) {
        return false;
    }
    if (v->kind == PC_PSD_V_UNIT_FLOAT && memcmp(v->unit, "#Pnt", 4) == 0) {
        *out = v->d * (double)dpi / 72.0;
        return true;
    }
    if (v->kind == PC_PSD_V_UNIT_FLOAT && memcmp(v->unit, "#Mlm", 4) == 0) {
        *out = v->d * (double)dpi / 25.4;
        return true;
    }
    return pc_io4_num(v, out);
}

pc_status pc_io_parse_vstk(const uint8_t *data, size_t len, float dpi, bool *found, pc_io_vstk *out,
                           pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    pc_io_vstk r;
    const pc_psd_value *fe;
    const pc_psd_value *dash;
    const pc_psd_desc *content;
    pc_shape_stroke *s;
    double v;
    pc_status st = pc_io4_vdesc_prefix(data, len, &ok, &d, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    memset(&r, 0, sizeof r);
    fe = pc_psd_desc_get(&d, "fillEnabled");
    r.fill_enabled = !(fe != NULL && fe->kind == PC_PSD_V_BOOLEAN && !fe->b);
    if (!pc_io4_bool_of(&d, "strokeEnabled")) {
        pc_psd_desc_fini(&d);
        *out = r;
        *found = true;
        return PC_OK;
    }
    s = &r.stroke;
    pc_shape_stroke_default(s);
    s->cap = pc_io4_enum_is(&d, "strokeStyleLineCapType", "strokeStyleRoundCap")    ? PC_LINE_CAP_ROUND
             : pc_io4_enum_is(&d, "strokeStyleLineCapType", "strokeStyleSquareCap") ? PC_LINE_CAP_SQUARE
                                                                                    : PC_LINE_CAP_BUTT;
    s->join = pc_io4_enum_is(&d, "strokeStyleLineJoinType", "strokeStyleRoundJoin")   ? PC_LINE_JOIN_ROUND
              : pc_io4_enum_is(&d, "strokeStyleLineJoinType", "strokeStyleBevelJoin") ? PC_LINE_JOIN_BEVEL
                                                                                      : PC_LINE_JOIN_MITER;
    s->align =
        pc_io4_enum_is(&d, "strokeStyleLineAlignment", "strokeStyleAlignInside")    ? PC_STROKE_ALIGN_INSIDE
        : pc_io4_enum_is(&d, "strokeStyleLineAlignment", "strokeStyleAlignOutside") ? PC_STROKE_ALIGN_OUTSIDE
                                                                                    : PC_STROKE_ALIGN_CENTER;
    dash = pc_psd_desc_get(&d, "strokeStyleLineDashSet");
    if (dash != NULL && dash->kind == PC_PSD_V_LIST && dash->nlist > 0) {
        s->dashes = pc_malloc_array(dash->nlist, sizeof *s->dashes);
        if (s->dashes == NULL) {
            st = pc_io_nomem(err, "reading a shape stroke");
            goto fail;
        }
        for (size_t i = 0; i < dash->nlist; i++) {
            if (pc_io4_num(&dash->list[i], &v)) {
                s->dashes[s->ndashes++] = pc_f64_to_f32(v);
            }
        }
    }
    content = pc_io4_obj(&d, "strokeStyleContent");
    if (content != NULL) {
        bool got = false;
        pc_fill f;
        st = content_fill(content, &got, &f, err);
        if (st != PC_OK) {
            goto fail;
        }
        if (got) {
            pc_fill_fini(&s->paint);
            s->paint = f;
        }
    }
    s->width = unit_px(pc_psd_desc_get(&d, "strokeStyleLineWidth"), dpi, &v) ? pc_f64_to_f32(v) : 1.0f;
    s->opacity = pc_io4_num_of(&d, "strokeStyleOpacity", &v) ? pc_f64_to_f32(v) / 100.0f : 1.0f;
    s->miter_limit = pc_io4_num_of(&d, "strokeStyleMiterLimit", &v) ? pc_f64_to_f32(v) : 100.0f;
    s->dash_offset = pc_io4_num_of(&d, "strokeStyleLineDashOffset", &v) ? pc_f64_to_f32(v) : 0.0f;
    r.has_stroke = true;
    pc_psd_desc_fini(&d);
    *out = r;
    *found = true;
    return PC_OK;
fail:
    pc_shape_stroke_fini(s);
    pc_psd_desc_fini(&d);
    return st;
}

pc_status pc_io_vstk_bytes(const pc_shape_stroke *stroke, bool fill_enabled, float dpi, pc_buf *out,
                           pc_error *err)
{
    pc_shape_stroke def;
    const pc_shape_stroke *s;
    uint8_t key[4];
    pc_psd_desc content;
    pc_psd_desc d;
    pc_psd_value dashes = pc_io4_list();
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    const char *cls;
    pc_status st;
    pc_shape_stroke_default(&def);
    s = stroke != NULL ? stroke : &def;
    st = pc_io_fill_to_desc(&s->paint, key, &content, err);
    if (st != PC_OK) {
        return st;
    }
    cls = s->paint.kind == PC_FILL_SOLID      ? "solidColorLayer"
          : s->paint.kind == PC_FILL_GRADIENT ? "gradientLayer"
                                              : "patternLayer";
    pc_psd_id_fini(&content.class_id);
    st = pc_psd_id_make(cls, &content.class_id, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&content);
        return st;
    }
    pc_io4_desc(&d, "strokeStyle", &st, err);
    pc_io4_put(&d, "strokeStyleVersion", pc_psd_value_int(2), &st, err);
    pc_io4_put(&d, "strokeEnabled", pc_psd_value_bool(stroke != NULL), &st, err);
    pc_io4_put(&d, "fillEnabled", pc_psd_value_bool(fill_enabled), &st, err);
    pc_io4_put(&d, "strokeStyleLineWidth", pc_io4_unit("#Pxl", (double)s->width), &st, err);
    pc_io4_put(&d, "strokeStyleLineDashOffset", pc_io4_unit("#Pnt", (double)s->dash_offset), &st, err);
    pc_io4_put(&d, "strokeStyleMiterLimit", pc_psd_value_double((double)s->miter_limit), &st, err);
    pc_io4_put_enum(&d, "strokeStyleLineCapType", "strokeStyleLineCapType",
                    s->cap == PC_LINE_CAP_BUTT    ? "strokeStyleButtCap"
                    : s->cap == PC_LINE_CAP_ROUND ? "strokeStyleRoundCap"
                                                  : "strokeStyleSquareCap",
                    &st, err);
    pc_io4_put_enum(&d, "strokeStyleLineJoinType", "strokeStyleLineJoinType",
                    s->join == PC_LINE_JOIN_MITER   ? "strokeStyleMiterJoin"
                    : s->join == PC_LINE_JOIN_ROUND ? "strokeStyleRoundJoin"
                                                    : "strokeStyleBevelJoin",
                    &st, err);
    pc_io4_put_enum(&d, "strokeStyleLineAlignment", "strokeStyleLineAlignment",
                    s->align == PC_STROKE_ALIGN_INSIDE   ? "strokeStyleAlignInside"
                    : s->align == PC_STROKE_ALIGN_CENTER ? "strokeStyleAlignCenter"
                                                         : "strokeStyleAlignOutside",
                    &st, err);
    pc_io4_put(&d, "strokeStyleScaleLock", pc_psd_value_bool(false), &st, err);
    pc_io4_put(&d, "strokeStyleStrokeAdjust", pc_psd_value_bool(false), &st, err);
    for (size_t i = 0; i < s->ndashes; i++) {
        pc_io4_list_push(&dashes, pc_io4_unit("#Nne", (double)s->dashes[i]), &st, err);
    }
    pc_io4_put(&d, "strokeStyleLineDashSet", dashes, &st, err);
    pc_io4_put_enum(&d, "strokeStyleBlendMode", "BlnM", "Nrml", &st, err);
    pc_io4_put(&d, "strokeStyleOpacity", pc_io4_unit("#Prc", (double)(s->opacity * 100.0f)), &st, err);
    pc_io4_put_obj(&d, "strokeStyleContent", &content, &st, err);
    pc_io4_put(&d, "strokeStyleResolution", pc_psd_value_double((double)dpi), &st, err);
    if (st == PC_OK) {
        st = pc_psd_vdesc_to_bytes(&d, &bytes, err);
        if (st == PC_OK) {
            st = pc_buf_append(&b, bytes.data, bytes.len, err);
            pc_psd_bytes_fini(&bytes);
        }
    }
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---- vogk (live shape origination) ---- */

static bool rect_of(const pc_psd_desc *d, double out[4])
{
    const pc_psd_desc *b = pc_io4_obj(d, "keyOriginShapeBBox");
    double t;
    double l;
    double bt;
    double r;
    if (b == NULL || !pc_io4_num_of(b, "Top ", &t) || !pc_io4_num_of(b, "Left", &l) ||
        !pc_io4_num_of(b, "Btom", &bt) || !pc_io4_num_of(b, "Rght", &r)) {
        return false;
    }
    out[0] = l;
    out[1] = t;
    out[2] = r - l;
    out[3] = bt - t;
    return true;
}

static bool point_of(const pc_psd_desc *d, const char *key, double out[2])
{
    const pc_psd_desc *p = pc_io4_obj(d, key);
    return p != NULL && pc_io4_num_of(p, "Hrzn", &out[0]) && pc_io4_num_of(p, "Vrtc", &out[1]);
}

static bool live_from_desc(const pc_psd_desc *d, pc_live_shape *out)
{
    const pc_psd_value *list = pc_psd_desc_get(d, "keyDescriptorList");
    const pc_psd_desc *o;
    const pc_psd_desc *t;
    const pc_psd_value *ty;
    pc_live_shape s;
    if (list == NULL || list->kind != PC_PSD_V_LIST || list->nlist != 1 ||
        list->list[0].kind != PC_PSD_V_DESCRIPTOR) {
        return false;
    }
    o = list->list[0].desc;
    if (pc_io4_bool_of(o, "keyShapeInvalidated")) {
        return false;
    }
    t = pc_io4_obj(o, "Trnf");
    if (t != NULL) {
        static const char *const keys[6] = {"xx", "xy", "yx", "yy", "tx", "ty"};
        static const double ident[6] = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
        for (size_t i = 0; i < 6; i++) {
            double g = ident[i];
            (void)pc_io4_num_of(t, keys[i], &g);
            if (!(fabs(g - ident[i]) < 1e-9)) {
                return false;
            }
        }
    }
    ty = pc_psd_desc_get(o, "keyOriginType");
    if (ty == NULL || ty->kind != PC_PSD_V_INTEGER) {
        return false;
    }
    memset(&s, 0, sizeof s);
    switch (ty->i) {
    case 1:
        s.kind = PC_LIVE_RECT;
        if (!rect_of(o, s.rect)) {
            return false;
        }
        break;
    case 2: {
        const pc_psd_desc *r = pc_io4_obj(o, "keyOriginRRectRadii");
        static const char *const corners[4] = {"topLeft", "topRight", "bottomRight", "bottomLeft"};
        if (r == NULL) {
            return false;
        }
        s.kind = PC_LIVE_RECT;
        if (!rect_of(o, s.rect)) {
            return false;
        }
        for (size_t i = 0; i < 4; i++) {
            double v = 0.0;
            s.radii[i] = pc_io4_num_of(r, corners[i], &v) ? v : 0.0;
        }
        break;
    }
    case 5:
        s.kind = PC_LIVE_ELLIPSE;
        if (!rect_of(o, s.rect)) {
            return false;
        }
        break;
    case 4:
        s.kind = PC_LIVE_LINE;
        if (!point_of(o, "keyOriginLineStart", s.from) || !point_of(o, "keyOriginLineEnd", s.to)) {
            return false;
        }
        s.weight = 1.0;
        (void)pc_io4_num_of(o, "keyOriginLineWeight", &s.weight);
        break;
    default:
        return false;
    }
    *out = s;
    return true;
}

pc_status pc_io_live_from_vogk(const uint8_t *data, size_t len, bool *found, pc_live_shape *out,
                               pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    pc_status st;
    if (len < 4) {
        *found = false;
        return PC_OK;
    }
    st = pc_io4_vdesc_prefix(data + 4, len - 4, &ok, &d, err);
    if (st != PC_OK || !ok) {
        if (st == PC_OK) {
            *found = false;
        }
        return st;
    }
    *found = live_from_desc(&d, out);
    pc_psd_desc_fini(&d);
    return PC_OK;
}

static pc_psd_value px(double v)
{
    return pc_io4_unit("#Pxl", v);
}

static void put_bbox(pc_psd_desc *o, const double r[4], pc_status *st, pc_error *err)
{
    pc_psd_desc b;
    pc_io4_desc(&b, "unitRect", st, err);
    pc_io4_put(&b, "unitValueQuadVersion", pc_psd_value_int(1), st, err);
    pc_io4_put(&b, "Top ", px(r[1]), st, err);
    pc_io4_put(&b, "Left", px(r[0]), st, err);
    pc_io4_put(&b, "Btom", px(r[1] + r[3]), st, err);
    pc_io4_put(&b, "Rght", px(r[0] + r[2]), st, err);
    pc_io4_put_obj(o, "keyOriginShapeBBox", &b, st, err);
}

static void put_point(pc_psd_desc *o, const char *key, const double p[2], pc_status *st, pc_error *err)
{
    pc_psd_desc d;
    pc_io4_desc(&d, "Pnt ", st, err);
    pc_io4_put(&d, "Hrzn", pc_psd_value_double(p[0]), st, err);
    pc_io4_put(&d, "Vrtc", pc_psd_value_double(p[1]), st, err);
    pc_io4_put_obj(o, key, &d, st, err);
}

pc_status pc_io_vogk_bytes(const pc_live_shape *s, float dpi, bool *has, pc_buf *out, pc_error *err)
{
    pc_status st = PC_OK;
    pc_psd_desc o;
    pc_psd_desc d;
    pc_psd_value list = pc_io4_list();
    pc_psd_value item;
    pc_psd_bytes bytes;
    pc_buf b = PC_BUF_INIT;
    int32_t ty;
    if (s->kind == PC_LIVE_POLYGON) {
        *has = false;
        return PC_OK;
    }
    ty = s->kind == PC_LIVE_ELLIPSE                                                               ? 5
         : s->kind == PC_LIVE_LINE                                                                ? 4
         : (s->radii[0] == 0.0 && s->radii[1] == 0.0 && s->radii[2] == 0.0 && s->radii[3] == 0.0) ? 1
                                                                                                  : 2;
    pc_io4_desc(&o, "null", &st, err);
    pc_io4_put(&o, "keyOriginType", pc_psd_value_int(ty), &st, err);
    pc_io4_put(&o, "keyOriginResolution", pc_psd_value_double((double)dpi), &st, err);
    if (ty == 2) {
        pc_psd_desc r;
        pc_io4_desc(&r, "radii", &st, err);
        pc_io4_put(&r, "unitValueQuadVersion", pc_psd_value_int(1), &st, err);
        pc_io4_put(&r, "topRight", px(s->radii[1]), &st, err);
        pc_io4_put(&r, "topLeft", px(s->radii[0]), &st, err);
        pc_io4_put(&r, "bottomLeft", px(s->radii[3]), &st, err);
        pc_io4_put(&r, "bottomRight", px(s->radii[2]), &st, err);
        pc_io4_put_obj(&o, "keyOriginRRectRadii", &r, &st, err);
    }
    if (ty == 4) {
        double r[4];
        r[0] = pc_rs_min(s->from[0], s->to[0]);
        r[1] = pc_rs_min(s->from[1], s->to[1]);
        r[2] = fabs(s->from[0] - s->to[0]);
        r[3] = fabs(s->from[1] - s->to[1]);
        put_bbox(&o, r, &st, err);
        put_point(&o, "keyOriginLineEnd", s->to, &st, err);
        put_point(&o, "keyOriginLineStart", s->from, &st, err);
        pc_io4_put(&o, "keyOriginLineWeight", pc_psd_value_double(s->weight), &st, err);
        pc_io4_put(&o, "keyOriginLineArrowSt", pc_psd_value_bool(false), &st, err);
        pc_io4_put(&o, "keyOriginLineArrowEnd", pc_psd_value_bool(false), &st, err);
        pc_io4_put(&o, "keyOriginLineArrWdth", pc_psd_value_double(0.0), &st, err);
        pc_io4_put(&o, "keyOriginLineArrLngth", pc_psd_value_double(0.0), &st, err);
        pc_io4_put(&o, "keyOriginLineArrConc", pc_psd_value_int(0), &st, err);
    } else {
        put_bbox(&o, s->rect, &st, err);
    }
    pc_io4_put(&o, "keyOriginIndex", pc_psd_value_int(0), &st, err);
    if (st == PC_OK) {
        st = pc_psd_value_object(&o, &item, err);
    } else {
        pc_psd_desc_fini(&o);
    }
    if (st == PC_OK) {
        pc_io4_list_push(&list, item, &st, err);
    }
    pc_io4_desc(&d, "null", &st, err);
    pc_io4_put(&d, "keyDescriptorList", list, &st, err);
    if (st == PC_OK) {
        static const uint8_t one[4] = {0, 0, 0, 1};
        st = pc_psd_vdesc_to_bytes(&d, &bytes, err);
        if (st == PC_OK) {
            st = pc_buf_append(&b, one, 4, err);
            if (st == PC_OK) {
                st = pc_buf_append(&b, bytes.data, bytes.len, err);
            }
            pc_psd_bytes_fini(&bytes);
        }
    }
    pc_psd_desc_fini(&d);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    *has = true;
    return PC_OK;
}

/* ---- shape layers ---- */

pc_status pc_io_shape_from_blocks(pc_shape_layer *sh, pc_io_block_lookup block, void *ctx, uint32_t w,
                                  uint32_t h, float dpi, pc_error *err)
{
    static const uint8_t vsms[4] = {'v', 's', 'm', 's'};
    static const uint8_t vmsk[4] = {'v', 'm', 's', 'k'};
    static const uint8_t vscg[4] = {'v', 's', 'c', 'g'};
    static const uint8_t vstk[4] = {'v', 's', 't', 'k'};
    static const uint8_t vogk[4] = {'v', 'o', 'g', 'k'};
    size_t len = 0;
    const uint8_t *d = block(ctx, vsms, &len);
    bool ok = false;
    pc_status st;
    if (d == NULL) {
        d = block(ctx, vmsk, &len);
    }
    if (d != NULL) {
        pc_path p;
        st = pc_io_path_from_vmsk(d, len, w, h, &ok, &p, NULL, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            pc_path_fini(&sh->path);
            sh->path = p;
        }
    }
    d = block(ctx, vscg, &len);
    if (d != NULL) {
        pc_fill f;
        st = pc_io_fill_from_vscg(d, len, &ok, &f, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            pc_fill_fini(&sh->fill);
            sh->fill = f;
            sh->has_fill = true;
        }
    }
    d = block(ctx, vstk, &len);
    if (d != NULL) {
        pc_io_vstk v;
        st = pc_io_parse_vstk(d, len, dpi, &ok, &v, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            pc_shape_stroke_fini(&sh->stroke);
            sh->has_stroke = v.has_stroke;
            if (v.has_stroke) {
                sh->stroke = v.stroke;
            }
            if (!v.fill_enabled) {
                pc_fill_fini(&sh->fill);
                sh->has_fill = false;
            }
        }
    }
    d = block(ctx, vogk, &len);
    ok = false;
    if (d != NULL) {
        pc_live_shape live;
        st = pc_io_live_from_vogk(d, len, &ok, &live, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            sh->live = live;
        }
    }
    sh->has_live = ok;
    return PC_OK;
}

static bool key_is(const pc_io_raw_block *b, const char *k)
{
    return memcmp(b->key, k, 4) == 0;
}

static bool find(const pc_io_raw_blocks *raw, const char *k, size_t *at)
{
    for (size_t i = 0; i < raw->n; i++) {
        if (key_is(&raw->v[i], k)) {
            *at = i;
            return true;
        }
    }
    return false;
}

static void remove_at(pc_io_raw_blocks *raw, size_t i)
{
    free(raw->v[i].data);
    memmove(&raw->v[i], &raw->v[i + 1], (raw->n - i - 1) * sizeof *raw->v);
    raw->n--;
}

/* Replaces raw[i]'s data with *b (taken over). */
static void replace_data(pc_io_raw_blocks *raw, size_t i, pc_buf *b)
{
    size_t n = 0;
    uint8_t *p = pc_buf_take(b, &n);
    free(raw->v[i].data);
    raw->v[i].data = p;
    raw->v[i].len = n;
}

static pc_status push_buf(pc_io_raw_blocks *raw, const char *key, pc_buf *b, pc_error *err)
{
    size_t n = 0;
    uint8_t *p = pc_buf_take(b, &n);
    return pc_io_raw_blocks_push_take(raw, (const uint8_t *)key, &p, n, err);
}

static pc_status has_fill_block(const pc_io_raw_blocks *raw, bool *out, pc_error *err)
{
    *out = false;
    for (size_t i = 0; i < raw->n && !*out; i++) {
        const pc_io_raw_block *b = &raw->v[i];
        bool ok = false;
        pc_fill f;
        pc_status st = PC_OK;
        if (key_is(b, "vscg")) {
            st = pc_io_fill_from_vscg(b->data, b->len, &ok, &f, err);
        } else if (key_is(b, "SoCo") || key_is(b, "GdFl") || key_is(b, "PtFl")) {
            st = pc_io_parse_fill(b->key, b->data, b->len, &ok, &f, err);
        }
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            pc_fill_fini(&f);
            *out = true;
        }
    }
    return PC_OK;
}

/* path_from_vmsk(d) == sh.path. */
static pc_status path_keeps(const uint8_t *d, size_t len, const pc_path *want, uint32_t w, uint32_t h,
                            bool *out, pc_error *err)
{
    pc_path p;
    bool ok = false;
    pc_status st = pc_io_path_from_vmsk(d, len, w, h, &ok, &p, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    *out = ok && pc_path_eq(&p, want);
    if (ok) {
        pc_path_fini(&p);
    }
    return PC_OK;
}

static pc_status shape_path_block(const pc_shape_layer *sh, pc_io_raw_blocks *raw, uint32_t w, uint32_t h,
                                  pc_error *err)
{
    size_t i = 0;
    bool pos = find(raw, "vsms", &i) || find(raw, "vmsk", &i);
    bool keep = false;
    pc_buf data = PC_BUF_INIT;
    pc_status st = PC_OK;
    if (pos) {
        st = path_keeps(raw->v[i].data, raw->v[i].len, &sh->path, w, h, &keep, err);
        if (st != PC_OK || keep) {
            return st;
        }
    }
    /* The layer's own raw copy may still be valid (psd_raw is the principal block). */
    if (sh->psd_raw != NULL) {
        st = path_keeps(pc_blob_data(sh->psd_raw), pc_blob_len(sh->psd_raw), &sh->path, w, h, &keep, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (keep) {
        st = pc_buf_append(&data, pc_blob_data(sh->psd_raw), pc_blob_len(sh->psd_raw), err);
    } else {
        st = pc_io_vmsk_bytes(&sh->path, 0, w, h, &data, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&data);
        return st;
    }
    if (pos) {
        replace_data(raw, i, &data);
        return PC_OK;
    }
    return push_buf(raw, "vmsk", &data, err);
}

static bool opt_stroke_eq(bool ha, const pc_shape_stroke *a, bool hb, const pc_shape_stroke *b)
{
    return ha == hb && (!ha || pc_shape_stroke_eq(a, b));
}

pc_status pc_io_shape_blocks(const pc_shape_layer *sh, pc_io_raw_blocks *raw, uint32_t w, uint32_t h,
                             float dpi, pc_error *err)
{
    size_t i = 0;
    bool present = false;
    bool decodable = false;
    bool undecodable_fill;
    pc_buf b = PC_BUF_INIT;
    pc_status st = shape_path_block(sh, raw, w, h, err);
    if (st != PC_OK) {
        return st;
    }
    /* Fill content. */
    if (sh->has_fill && find(raw, "vscg", &i)) {
        bool ok = false;
        pc_fill f;
        st = pc_io_fill_from_vscg(raw->v[i].data, raw->v[i].len, &ok, &f, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            bool same = pc_fill_eq(&f, &sh->fill);
            pc_fill_fini(&f);
            ok = same;
        }
        if (!ok) {
            st = pc_io_vscg_bytes(&sh->fill, &b, err);
            if (st != PC_OK) {
                return st;
            }
            replace_data(raw, i, &b);
        }
    }
    /* Photoshop shapes always carry fill content, even with the fill turned off. */
    present =
        find(raw, "vscg", &i) || find(raw, "SoCo", &i) || find(raw, "GdFl", &i) || find(raw, "PtFl", &i);
    if (!sh->has_fill && !present) {
        pc_fill black = pc_fill_solid(PC_COLOR_BLACK);
        st = pc_io_vscg_bytes(&black, &b, err);
        if (st == PC_OK) {
            st = push_buf(raw, "vscg", &b, err);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    /* Stroke (+ fill enabled flag). A fill we could not decode counts as unchanged. */
    st = has_fill_block(raw, &decodable, err);
    if (st != PC_OK) {
        return st;
    }
    undecodable_fill = !sh->has_fill && present && !decodable;
    if (find(raw, "vstk", &i)) {
        bool ok = false;
        bool same = false;
        pc_io_vstk v;
        st = pc_io_parse_vstk(raw->v[i].data, raw->v[i].len, dpi, &ok, &v, err);
        if (st != PC_OK) {
            return st;
        }
        if (ok) {
            same = opt_stroke_eq(v.has_stroke, &v.stroke, sh->has_stroke, &sh->stroke) &&
                   (v.fill_enabled == sh->has_fill || (v.fill_enabled && undecodable_fill));
            pc_io_vstk_fini(&v);
        }
        if (!same) {
            st = pc_io_vstk_bytes(sh->has_stroke ? &sh->stroke : NULL, sh->has_fill, dpi, &b, err);
            if (st != PC_OK) {
                return st;
            }
            replace_data(raw, i, &b);
        }
    } else if (sh->has_stroke || (!sh->has_fill && decodable)) {
        /* "No fill" needs a vstk only when a fill block would otherwise paint. */
        st = pc_io_vstk_bytes(sh->has_stroke ? &sh->stroke : NULL, sh->has_fill, dpi, &b, err);
        if (st == PC_OK) {
            st = push_buf(raw, "vstk", &b, err);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    /* Live shape origination. */
    {
        bool pos = find(raw, "vogk", &i);
        bool has_cur = false;
        pc_live_shape cur;
        bool has_gen = false;
        if (pos) {
            st = pc_io_live_from_vogk(raw->v[i].data, raw->v[i].len, &has_cur, &cur, err);
            if (st != PC_OK) {
                return st;
            }
        }
        if (has_cur == sh->has_live && (!has_cur || pc_live_shape_eq(&cur, &sh->live)) &&
            !(!pos && sh->has_live)) {
            return PC_OK;
        }
        if (sh->has_live) {
            st = pc_io_vogk_bytes(&sh->live, dpi, &has_gen, &b, err);
            if (st != PC_OK) {
                return st;
            }
        }
        if (pos && has_gen) {
            replace_data(raw, i, &b);
        } else if (pos) {
            remove_at(raw, i);
        } else if (has_gen) {
            return push_buf(raw, "vogk", &b, err);
        }
    }
    return PC_OK;
}
