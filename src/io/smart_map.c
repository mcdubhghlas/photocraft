/* Smart objects in PSD: the placed-layer blocks (SoLd, PlLd) and the FEid filter mask (port of
 * the placement half of crates/io/src/smart_map.rs; the filters are in smart_filters.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/transform.h"
#include "base/bytes.h"
#include "base/hash.h"
#include "base/mem.h"
#include "base/num.h"
#include "io/blocks.h"
#include "io/pixels.h"
#include "io/smart_internal.h"
#include "io/smart_map.h"

void pc_io_placed_fini(pc_io_placed *p)
{
    if (p == NULL) {
        return;
    }
    pc_psd_desc_fini(&p->descriptor);
    pc_str_fini(&p->idnt);
    pc_str_fini(&p->placed);
    if (p->has_stack) {
        pc_io_filter_stack_fini(&p->stack);
    }
    p->has_stack = false;
}

pc_status pc_io_sold_descriptor(const uint8_t *data, size_t len, bool *found, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    size_t used = 0;
    pc_error pe;
    pc_status st;
    *found = false;
    if (len < 8 || memcmp(data, "soLD", 4) != 0) {
        return PC_OK;
    }
    st = pc_psd_vdesc_parse_prefix(data + 8, len - 8, &d, &used, &pe);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
    }
    if (st != PC_OK) {
        return PC_OK;
    }
    *out = d;
    *found = true;
    return PC_OK;
}

pc_status pc_io_parse_sold(const uint8_t *data, size_t len, bool *found, pc_io_placed *out, pc_error *err)
{
    pc_io_placed p;
    bool has = false;
    char *placed = NULL;
    bool have_placed = false;
    const pc_psd_desc *fx;
    pc_status st;
    memset(&p, 0, sizeof p);
    st = pc_io_sold_descriptor(data, len, &has, &p.descriptor, err);
    if (st != PC_OK || !has) {
        *found = false;
        return st;
    }
    st = pc_io_parse_smart((const uint8_t *)"SoLd", data, len, &p.idnt, &p.transform, err);
    if (st == PC_OK) {
        st = pc_io3_text_of(&p.descriptor, "placed", &have_placed, &placed, err);
    }
    if (st == PC_OK && have_placed) {
        st = pc_str_set_c(&p.placed, placed, err);
    }
    free(placed);
    fx = pc_io3_get_desc(&p.descriptor, "filterFX");
    if (st == PC_OK && fx != NULL) {
        st = pc_io_filter_stack_read(fx, &p.stack, err);
        p.has_stack = st == PC_OK;
    }
    if (st != PC_OK) {
        pc_io_placed_fini(&p);
        return st;
    }
    *out = p;
    *found = true;
    return PC_OK;
}

/* ---- writing the placement ---- */

/* Where the source corners land (top-left, top-right, bottom-right, bottom-left), x, y pairs. */
static void quad_points(const pc_io_placed_spec *s, double out[8])
{
    const double w = s->size[0];
    const double h = s->size[1];
    const double *m = s->transform.m;
    double frame[4];
    double pts[4][2];
    if (s->has_perspective) {
        frame[0] = 0.0;
        frame[1] = 0.0;
        frame[2] = w;
        frame[3] = h;
    } else {
        pc_io_placed_frame(s->warp, w, h, frame);
    }
    /* Source corners: top-left, top-right, bottom-right, bottom-left of the control box. */
    pts[0][0] = frame[0];
    pts[0][1] = frame[1];
    pts[1][0] = frame[2];
    pts[1][1] = frame[1];
    pts[2][0] = frame[2];
    pts[2][1] = frame[3];
    pts[3][0] = frame[0];
    pts[3][1] = frame[3];
    for (size_t i = 0; i < 4; i++) {
        double x = pts[i][0];
        double y = pts[i][1];
        if (s->has_perspective) {
            pc_algo_homography hm;
            memcpy(hm.m, s->perspective, sizeof hm.m);
            pc_algo_homography_apply(&hm, x, y, &out[2 * i], &out[2 * i + 1]);
        } else {
            out[2 * i] = m[0] * x + m[2] * y + m[4];
            out[2 * i + 1] = m[1] * x + m[3] * y + m[5];
        }
    }
}

static pc_status quad(const pc_io_placed_spec *s, pc_psd_value *out, pc_error *err)
{
    double q[8];
    pc_psd_value list;
    pc_status st = PC_OK;
    quad_points(s, q);
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (size_t i = 0; i < 8 && st == PC_OK; i++) {
        pc_psd_value v = pc_psd_value_double(q[i]);
        st = pc_psd_value_list_push(&list, &v, err);
    }
    if (st != PC_OK) {
        pc_psd_value_fini(&list);
        return st;
    }
    *out = list;
    return PC_OK;
}

bool pc_io_quad_moved(const pc_psd_desc *tmpl, const pc_io_placed_spec *s)
{
    const pc_psd_value *pts = pc_psd_desc_get(tmpl, "Trnf");
    double old[8];
    double now[8];
    size_t n = 0;
    if (pts == NULL || pts->kind != PC_PSD_V_LIST) {
        return true;
    }
    for (size_t i = 0; i < pts->nlist; i++) {
        double v;
        if (pc_io3_num(&pts->list[i], &v)) {
            if (n == 8) {
                return true;
            }
            old[n++] = v;
        }
    }
    if (n != 8) {
        return true;
    }
    quad_points(s, now);
    for (size_t i = 0; i < 8; i++) {
        if (fabs(old[i] - now[i]) > 1e-6 * (1.0 + fabs(old[i]))) {
            return true;
        }
    }
    return false;
}

static pc_status enumv(const char *t, const char *v, pc_psd_value *out, pc_error *err)
{
    return pc_psd_value_enum(t, v, out, err);
}

static pc_status unit_floats(const pc_mesh *m, size_t axis, pc_psd_value *out, pc_error *err)
{
    double *f = pc_malloc_array(m->npoints > 0 ? m->npoints : 1, sizeof *f);
    if (f == NULL) {
        return pc_io_nomem(err, "writing a warp");
    }
    for (size_t i = 0; i < m->npoints; i++) {
        f[i] = axis == 0 ? m->points[i].x : m->points[i].y;
    }
    memset(out, 0, sizeof *out);
    out->kind = PC_PSD_V_UNIT_FLOATS;
    memcpy(out->unit, "#Pxl", 4);
    out->floats = f;
    out->nfloats = m->npoints;
    return PC_OK;
}

/* The warp descriptor (also the PlLd warp). Multi-patch meshes have no 1 x 1 envelope and are
 * written as no warp, with a message. */
static pc_status warp_desc(const pc_warp *wp, const double size[2], pc_io_warnings *warnings,
                           pc_psd_desc *out, pc_error *err)
{
    const double nb[4] = {0.0, 0.0, size[0], size[1]};
    pc_warp none;
    const pc_warp *w = wp;
    const pc_mesh *mesh = NULL;
    pc_psd_desc bounds;
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = PC_OK;
    pc_warp_none(&none, nb);
    if (w == NULL) {
        w = &none;
    }
    if (w->style == PC_WARP_CUSTOM) {
        if (w->has_mesh && w->mesh.npoints == 16) {
            mesh = &w->mesh;
        } else {
            st = pc_io_warn(warnings, err,
                            "a custom warp with more than one patch was written to PSD without its warp");
            w = &none;
        }
    }
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_desc_init(&bounds, "classFloatRect", err);
    if (st != PC_OK) {
        return st;
    }
    v = pc_psd_value_double(w->bounds[1]);
    st = pc_io3_put(&bounds, "Top ", &v, st, err);
    v = pc_psd_value_double(w->bounds[0]);
    st = pc_io3_put(&bounds, "Left", &v, st, err);
    v = pc_psd_value_double(w->bounds[3]);
    st = pc_io3_put(&bounds, "Btom", &v, st, err);
    v = pc_psd_value_double(w->bounds[2]);
    st = pc_io3_put(&bounds, "Rght", &v, st, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&bounds);
        return st;
    }
    st = pc_psd_desc_init(&d, "warp", err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&bounds);
        return st;
    }
    st = enumv("warpStyle", pc_warp_style_psd_name(w->style), &v, err);
    st = pc_io3_put(&d, "warpStyle", &v, st, err);
    v = pc_psd_value_double(w->bend);
    st = pc_io3_put(&d, "warpValue", &v, st, err);
    v = pc_psd_value_double(w->h_distort);
    st = pc_io3_put(&d, "warpPerspective", &v, st, err);
    v = pc_psd_value_double(w->v_distort);
    st = pc_io3_put(&d, "warpPerspectiveOther", &v, st, err);
    if (st == PC_OK) {
        st = enumv("Ornt", w->vertical ? "Vrtc" : "Hrzn", &v, err);
        st = pc_io3_put(&d, "warpRotate", &v, st, err);
    }
    st = pc_io3_put_obj(&d, "bounds", &bounds, st, err);
    v = pc_psd_value_int(4);
    st = pc_io3_put(&d, "uOrder", &v, st, err);
    v = pc_psd_value_int(4);
    st = pc_io3_put(&d, "vOrder", &v, st, err);
    if (st == PC_OK && mesh != NULL) {
        pc_psd_desc *body = calloc(1, sizeof *body);
        pc_psd_desc env;
        if (body == NULL) {
            st = pc_io_nomem(err, "writing a warp");
        } else {
            st = pc_psd_desc_init(body, "rationalPoint", err);
            if (st == PC_OK) {
                st = unit_floats(mesh, 0, &v, err);
                st = pc_io3_put(body, "Hrzn", &v, st, err);
            }
            if (st == PC_OK) {
                st = unit_floats(mesh, 1, &v, err);
                st = pc_io3_put(body, "Vrtc", &v, st, err);
            }
            if (st != PC_OK) {
                pc_psd_desc_free(body);
            }
        }
        if (st == PC_OK) {
            st = pc_psd_desc_init(&env, "customEnvelopeWarp", err);
            if (st != PC_OK) {
                pc_psd_desc_free(body);
            } else {
                memset(&v, 0, sizeof v);
                v.kind = PC_PSD_V_OBJECT_ARRAY;
                v.prefix = 16;
                v.desc = body;
                st = pc_io3_put(&env, "meshPoints", &v, st, err);
                st = pc_io3_put_obj(&d, "customEnvelopeWarp", &env, st, err);
            }
        }
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

static pc_status ratio(int32_t n, int32_t den, pc_psd_desc *parent, const char *key, pc_status st,
                       pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_desc_init(&d, "null", err);
    if (st != PC_OK) {
        return st;
    }
    v = pc_psd_value_int(n);
    st = pc_io3_put(&d, "numerator", &v, st, err);
    v = pc_psd_value_int(den);
    st = pc_io3_put(&d, "denominator", &v, st, err);
    return pc_io3_put_obj(parent, key, &d, st, err);
}

static pc_status pad4(pc_buf *b, pc_error *err)
{
    while (b->len % 4 != 0) {
        pc_status st = pc_buf_append_byte(b, 0, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* A fresh soLD descriptor in Photoshop's key order. */
static pc_status fresh_sold(const pc_io_placed_spec *s, pc_psd_desc *warp, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_desc sz;
    pc_psd_desc ci;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "null", err);
    if (st != PC_OK) {
        pc_psd_desc_fini(warp);
        return st;
    }
    st = pc_psd_value_text(s->idnt, true, &v, err);
    st = pc_io3_put(&d, "Idnt", &v, st, err);
    if (st == PC_OK) {
        st = pc_psd_value_text(s->placed, true, &v, err);
        st = pc_io3_put(&d, "placed", &v, st, err);
    }
    v = pc_psd_value_int(1);
    st = pc_io3_put(&d, "PgNm", &v, st, err);
    v = pc_psd_value_int(1);
    st = pc_io3_put(&d, "totalPages", &v, st, err);
    v = pc_psd_value_int(1);
    st = pc_io3_put(&d, "Crop", &v, st, err);
    st = ratio(0, 600, &d, "frameStep", st, err);
    st = ratio(0, 600, &d, "duration", st, err);
    v = pc_psd_value_int(1);
    st = pc_io3_put(&d, "frameCount", &v, st, err);
    v = pc_psd_value_int(16);
    st = pc_io3_put(&d, "Annt", &v, st, err);
    v = pc_psd_value_int(2);
    st = pc_io3_put(&d, "Type", &v, st, err);
    if (st == PC_OK) {
        st = quad(s, &v, err);
        st = pc_io3_put(&d, "Trnf", &v, st, err);
    }
    if (st == PC_OK) {
        st = quad(s, &v, err);
        st = pc_io3_put(&d, "nonAffineTransform", &v, st, err);
    }
    st = pc_io3_put_obj(&d, "warp", warp, st, err);
    if (st == PC_OK) {
        st = pc_psd_desc_init(&sz, "Pnt ", err);
        if (st == PC_OK) {
            v = pc_psd_value_double(s->size[0]);
            st = pc_io3_put(&sz, "Wdth", &v, st, err);
            v = pc_psd_value_double(s->size[1]);
            st = pc_io3_put(&sz, "Hght", &v, st, err);
            st = pc_io3_put_obj(&d, "Sz  ", &sz, st, err);
        }
    }
    v = pc_io3_unit("#Rsl", s->dpi);
    st = pc_io3_put(&d, "Rslt", &v, st, err);
    if (st == PC_OK && s->filter_fx != NULL) {
        pc_psd_desc fx;
        st = pc_psd_desc_clone(s->filter_fx, &fx, err);
        if (st == PC_OK) {
            st = pc_io3_put_obj(&d, "filterFX", &fx, st, err);
        }
    }
    v = pc_psd_value_int(-1);
    st = pc_io3_put(&d, "comp", &v, st, err);
    if (st == PC_OK) {
        st = pc_psd_desc_init(&ci, "null", err);
        if (st == PC_OK) {
            v = pc_psd_value_int(-1);
            st = pc_io3_put(&ci, "compID", &v, st, err);
            v = pc_psd_value_int(-1);
            st = pc_io3_put(&ci, "originalCompID", &v, st, err);
            st = pc_io3_put_obj(&d, "compInfo", &ci, st, err);
        }
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status pc_io_sold_bytes(const pc_psd_desc *tmpl, const pc_io_placed_spec *s, pc_io_warnings *warnings,
                           pc_buf *out, pc_error *err)
{
    pc_psd_desc warp;
    pc_psd_desc d;
    pc_psd_bytes vb = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    uint8_t hdr[8] = {'s', 'o', 'L', 'D', 0, 0, 0, 4};
    pc_status st = warp_desc(s->warp, s->size, warnings, &warp, err);
    if (st != PC_OK) {
        return st;
    }
    if (tmpl != NULL) {
        pc_psd_value v;
        st = pc_psd_desc_clone(tmpl, &d, err);
        if (st != PC_OK) {
            pc_psd_desc_fini(&warp);
            return st;
        }
        if (pc_io_quad_moved(tmpl, s)) {
            st = quad(s, &v, err);
            st = st == PC_OK ? pc_io3_set(&d, "Trnf", &v, err) : st;
            if (st == PC_OK) {
                st = quad(s, &v, err);
                st = st == PC_OK ? pc_io3_set(&d, "nonAffineTransform", &v, err) : st;
            }
        }
        if (st == PC_OK) {
            st = pc_psd_value_object(&warp, &v, err);
            st = st == PC_OK ? pc_io3_set(&d, "warp", &v, err) : st;
        } else {
            pc_psd_desc_fini(&warp);
        }
        if (st == PC_OK && s->filter_fx != NULL) {
            pc_psd_desc fx;
            st = pc_psd_desc_clone(s->filter_fx, &fx, err);
            if (st == PC_OK) {
                st = pc_psd_value_object(&fx, &v, err);
            }
            st = st == PC_OK ? pc_io3_set(&d, "filterFX", &v, err) : st;
        } else if (st == PC_OK) {
            pc_io3_remove(&d, "filterFX");
        }
        if (st != PC_OK) {
            pc_psd_desc_fini(&d);
            return st;
        }
    } else {
        st = fresh_sold(s, &warp, &d, err);
        if (st != PC_OK) {
            return st;
        }
    }
    st = pc_psd_vdesc_to_bytes(&d, &vb, err);
    pc_psd_desc_fini(&d);
    if (st == PC_OK) {
        st = pc_buf_append(&b, hdr, sizeof hdr, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, vb.data, vb.len, err);
    }
    pc_psd_bytes_fini(&vb);
    if (st == PC_OK) {
        st = pad4(&b, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

pc_status pc_io_plld_bytes(const pc_io_placed_spec *s, pc_io_warnings *warnings, pc_buf *out, pc_error *err)
{
    static const int32_t FIELDS[4] = {1, 1, 16, 2};
    pc_buf b = PC_BUF_INIT;
    uint8_t tmp[8];
    double q[8];
    size_t n = strlen(s->idnt);
    pc_psd_desc warp;
    pc_psd_bytes vb = {NULL, 0};
    pc_status st = pc_buf_append(&b, "plcL\0\0\0\3", 8, err);
    n = n < 255 ? n : 255;
    if (st == PC_OK) {
        st = pc_buf_append_byte(&b, (uint8_t)n, err);
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, s->idnt, n, err);
    }
    for (size_t i = 0; i < 4 && st == PC_OK; i++) {
        pc_store_be32(tmp, (uint32_t)FIELDS[i]);
        st = pc_buf_append(&b, tmp, 4, err);
    }
    quad_points(s, q);
    for (size_t i = 0; i < 8 && st == PC_OK; i++) {
        uint64_t bits;
        memcpy(&bits, &q[i], 8);
        pc_store_be64(tmp, bits);
        st = pc_buf_append(&b, tmp, 8, err);
    }
    if (st == PC_OK) {
        memset(tmp, 0, 4);
        st = pc_buf_append(&b, tmp, 4, err);
    }
    if (st == PC_OK) {
        st = warp_desc(s->warp, s->size, warnings, &warp, err);
        if (st == PC_OK) {
            st = pc_psd_vdesc_to_bytes(&warp, &vb, err);
            pc_psd_desc_fini(&warp);
        }
    }
    if (st == PC_OK) {
        st = pc_buf_append(&b, vb.data, vb.len, err);
    }
    pc_psd_bytes_fini(&vb);
    if (st == PC_OK) {
        st = pad4(&b, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

bool pc_io_stored_size(const pc_psd_desc *d, double out[2])
{
    const pc_psd_desc *sz = pc_io3_get_desc(d, "Sz  ");
    double w;
    double h;
    if (sz == NULL || !pc_io3_num_of(sz, "Wdth", &w) || !pc_io3_num_of(sz, "Hght", &h) || !(w > 0.0) ||
        !(h > 0.0)) {
        return false;
    }
    out[0] = w;
    out[1] = h;
    return true;
}

/* ---- the filter mask (FEid) ---- */

static const char *sample_debug(pc_sample_type s)
{
    return pc_sample_type_name(s);
}

pc_status pc_io_mask_from_item(const pc_psd_fx_item *item, pc_sample_type sample,
                               const pc_io_filter_stack *stack, bool *has, pc_layer_mask *out, pc_error *err)
{
    size_t w;
    size_t h;
    size_t n;
    pc_psd_bytes plane = {NULL, 0};
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, sample, false};
    const float one = 1.0f;
    pc_surface surface;
    pc_io_plane src;
    /* Zero encodes as all-zero bytes at every depth. */
    static const uint8_t fill[1][4] = {{0, 0, 0, 0}};
    bool inv = false;
    uint8_t *bytes = NULL;
    size_t blen = 0;
    pc_error pe;
    pc_status st;
    if (!item->has_mask) {
        *has = false;
        return PC_OK;
    }
    st = pc_psd_rect_size(item->mask_rect, &w, &h, &pe);
    if (st == PC_OK && item->depth > UINT16_MAX) {
        st = PC_ERR_FORMAT;
    }
    if (st == PC_OK) {
        st = pc_psd_fx_plane_decode(&item->mask, w, h, (uint16_t)item->depth, &plane, &pe);
    }
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
    }
    if (st != PC_OK) {
        return pc_fail(err, PC_ERR_FORMAT, "the smart filter mask could not be decoded");
    }
    if (item->depth != pc_io_psd_depth(sample)) {
        pc_psd_bytes_fini(&plane);
        return pc_fail(err, PC_ERR_FORMAT, "the smart filter mask is %u-bit in a %s document",
                       (unsigned)item->depth, sample_debug(sample));
    }
    if (!pc_size_mul(w, h, &n)) {
        pc_psd_bytes_fini(&plane);
        return pc_fail(err, PC_ERR_FORMAT, "the smart filter mask is too large");
    }
    src.present = true;
    src.data = plane.data;
    src.len = plane.len;
    st = pc_io_interleave(&src, 1, fill, n, sample, &inv, &bytes, &blen, err);
    pc_psd_bytes_fini(&plane);
    if (st != PC_OK) {
        return st;
    }
    pc_surface_init_default(&surface, fmt, &one, 1);
    st = pc_surface_write_interleaved(&surface,
                                      pc_rect_make(item->mask_rect.left, item->mask_rect.top,
                                                   item->mask_rect.right, item->mask_rect.bottom),
                                      bytes, blen, err);
    free(bytes);
    if (st != PC_OK) {
        pc_surface_fini(&surface);
        return st;
    }
    pc_surface_prune(&surface);
    if (pc_rect_is_empty(pc_surface_content_bounds(&surface))) {
        pc_surface_fini(&surface);
        *has = false;
        return PC_OK;
    }
    out->surface = surface;
    out->enabled = stack->mask_enabled;
    out->linked = stack->mask_linked;
    out->density = 1.0f;
    out->feather = 0.0f;
    *has = true;
    return PC_OK;
}

/* One plane encoded with compression, falling back to raw, then to the bytes as they are. */
static pc_status encode_plane(uint16_t compression, const uint8_t *plane, size_t len, size_t w, size_t h,
                              uint16_t depth, pc_psd_fx_plane *out, pc_error *err)
{
    pc_status st = pc_psd_fx_plane_encode(compression, plane, len, w, h, depth, out, err);
    if (st == PC_OK || st == PC_ERR_NOMEM) {
        return st;
    }
    st = pc_psd_fx_plane_encode(PC_PSD_RAW, plane, len, w, h, depth, out, err);
    if (st == PC_OK || st == PC_ERR_NOMEM) {
        return st;
    }
    out->compression = PC_PSD_RAW;
    out->data.data = NULL;
    out->data.len = 0;
    return pc_psd_bytes_set(&out->data, plane, len, err);
}

/* An FEid item with 26 empty slots and the filter mask over bounds (white when mask is NULL). */
static pc_status filter_mask_item(const char *placed, const pc_layer_mask *mask, pc_rect bounds,
                                  pc_sample_type sample, pc_psd_fx_item *out, pc_error *err)
{
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, sample, false};
    pc_rect area = mask != NULL ? pc_rect_union(pc_surface_content_bounds(&mask->surface), bounds) : bounds;
    size_t w;
    size_t h;
    size_t n;
    size_t bps = pc_sample_type_bytes(sample);
    uint8_t *plane = NULL;
    size_t plen = 0;
    uint16_t depth = pc_io_psd_depth(sample);
    pc_psd_fx_item it;
    pc_status st = PC_OK;
    if (pc_rect_is_empty(area)) {
        area = pc_rect_make(0, 0, 1, 1);
    }
    w = pc_rect_width(area);
    h = pc_rect_height(area);
    if (!pc_size_mul3(w, h, bps, &n) || n > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: smart filter mask of %zux%zu", w, h);
    }
    if (mask != NULL) {
        pc_surface conv;
        const pc_surface *s = &mask->surface;
        uint8_t *inter = NULL;
        size_t ilen = 0;
        bool inv = false;
        bool owned = false;
        if (!pc_pixel_format_eq(pc_surface_format(s), fmt)) {
            st = pc_surface_convert(s, fmt, &conv, err);
            s = &conv;
            owned = st == PC_OK;
        }
        if (st == PC_OK) {
            st = pc_surface_to_interleaved(s, area, &inter, &ilen, err);
        }
        if (owned) {
            pc_surface_fini(&conv);
        }
        if (st == PC_OK) {
            st = pc_io_deinterleave(inter, ilen, 1, sample, &inv, &plane, &plen, err);
        }
        free(inter);
    } else {
        plane = pc_malloc(n > 0 ? n : 1);
        if (plane == NULL) {
            st = pc_io_nomem(err, "writing a smart filter mask");
        }
        for (size_t i = 0; st == PC_OK && i < w * h; i++) {
            plen += pc_io_encode_be(1.0f, sample, plane + plen);
        }
    }
    if (st != PC_OK) {
        free(plane);
        return st;
    }
    memset(&it, 0, sizeof it);
    it.id = pc_strndup(placed, strlen(placed));
    it.slots = pc_calloc_array(26, sizeof *it.slots);
    it.slot_written = pc_calloc_array(26, sizeof *it.slot_written);
    if (it.id == NULL || it.slots == NULL || it.slot_written == NULL) {
        free(plane);
        pc_psd_fx_item_fini(&it);
        return pc_io_nomem(err, "writing a smart filter mask");
    }
    it.nslots = 26;
    it.version = 1;
    it.rect = (pc_psd_rect){area.y0, area.x0, area.y1, area.x1};
    it.depth = depth;
    it.max_channels = 24;
    it.has_mask = true;
    it.mask_rect = it.rect;
    st = encode_plane(sample == PC_SAMPLE_TYPE_F32 ? PC_PSD_ZIP_PREDICT : PC_PSD_RLE, plane, plen, w, h,
                      depth, &it.mask, err);
    free(plane);
    if (st != PC_OK) {
        pc_psd_fx_item_fini(&it);
        return st;
    }
    *out = it;
    return PC_OK;
}

pc_status pc_io_feid_item(const char *placed, const pc_surface *unfiltered, const pc_layer_mask *mask,
                          pc_rect bounds, pc_pixel_format doc_fmt, bool *has, pc_psd_fx_item *out,
                          pc_error *err)
{
    pc_sample_type sample = doc_fmt.sample;
    pc_surface conv;
    const pc_surface *px = unfiltered;
    bool owned = false;
    pc_rect content;
    pc_rect r;
    size_t cc = pc_color_mode_channels(doc_fmt.mode);
    uint16_t depth = pc_io_psd_depth(sample);
    uint16_t compression = sample == PC_SAMPLE_TYPE_F32 ? PC_PSD_ZIP_PREDICT : PC_PSD_RLE;
    bool invert[6];
    float *vals = NULL;
    size_t nvals = 0;
    float white[PC_RASTER_MAX_CHANNELS];
    const float opaque_white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_surface filled;
    uint8_t *bytes = NULL;
    size_t blen = 0;
    uint8_t *planes = NULL;
    size_t plane_len = 0;
    size_t w;
    size_t h;
    pc_psd_fx_item it;
    pc_status st = PC_OK;
    if (!pc_pixel_format_eq(pc_surface_format(unfiltered), doc_fmt)) {
        st = pc_surface_convert(unfiltered, doc_fmt, &conv, err);
        if (st != PC_OK) {
            return st;
        }
        px = &conv;
        owned = true;
    }
    content = pc_surface_content_bounds(px);
    if (pc_rect_is_empty(content)) {
        if (owned) {
            pc_surface_fini(&conv);
        }
        *has = false;
        return PC_OK;
    }
    /* Over the whole area the filters may draw into: Photoshop clips the filtered result to these
     * planes, so the content bounds alone would cut blurs off at the source's edge. */
    r = pc_rect_union(content, bounds);
    for (size_t c = 0; c < cc && c < 5; c++) {
        invert[c] = doc_fmt.mode == PC_COLOR_MODE_CMYK;
    }
    invert[cc < 5 ? cc : 5] = false;
    /* Fully transparent pixels carry white, as in Photoshop's own cache: filters that look at
     * colour regardless of alpha (Shadows/Highlights) read it. */
    st = pc_surface_read_region(px, r, &vals, &nvals, err);
    if (owned) {
        pc_surface_fini(&conv);
    }
    if (st != PC_OK) {
        return st;
    }
    (void)pc_from_rgba(doc_fmt, opaque_white, white);
    for (size_t i = 0; i + cc < nvals; i += cc + 1) {
        if (vals[i + cc] <= 0.0f) {
            memcpy(&vals[i], white, cc * sizeof *white);
        }
    }
    pc_surface_init(&filled, doc_fmt);
    st = pc_surface_write_region(&filled, r, vals, nvals, err);
    free(vals);
    if (st == PC_OK) {
        st = pc_surface_to_interleaved(&filled, r, &bytes, &blen, err);
    }
    pc_surface_fini(&filled);
    if (st == PC_OK && doc_fmt.mode == PC_COLOR_MODE_LAB && sample == PC_SAMPLE_TYPE_U16) {
        /* 16-bit Lab a and b in Photoshop's scale, as in layer channels. */
        pc_io_lab16_chroma(bytes, blen, cc + 1, false);
    }
    if (st == PC_OK) {
        st = pc_io_deinterleave(bytes, blen, cc + 1, sample, invert, &planes, &plane_len, err);
    }
    free(bytes);
    if (st != PC_OK) {
        return st;
    }
    w = pc_rect_width(r);
    h = pc_rect_height(r);
    st = filter_mask_item(placed, mask, bounds, sample, &it, err);
    for (size_t i = 0; st == PC_OK && i <= cc; i++) {
        size_t slot = i == cc ? 25 : i;
        if (slot < it.nslots) {
            st = encode_plane(compression, planes + i * plane_len, plane_len, w, h, depth, &it.slots[slot],
                              err);
            it.slot_written[slot] = st == PC_OK;
        }
    }
    free(planes);
    if (st != PC_OK) {
        pc_psd_fx_item_fini(&it);
        return st;
    }
    it.rect = (pc_psd_rect){r.y0, r.x0, r.y1, r.x1};
    *out = it;
    *has = true;
    return PC_OK;
}

pc_status pc_io_fx_item_clone(const pc_psd_fx_item *src, pc_psd_fx_item *out, pc_error *err)
{
    pc_psd_fx_item it = *src;
    pc_status st = PC_OK;
    it.id = pc_strndup(src->id != NULL ? src->id : "", src->id != NULL ? strlen(src->id) : 0);
    it.slots = src->nslots > 0 ? pc_calloc_array(src->nslots, sizeof *it.slots) : NULL;
    it.slot_written = src->nslots > 0 ? pc_calloc_array(src->nslots, sizeof *it.slot_written) : NULL;
    it.mask.data.data = NULL;
    it.mask.data.len = 0;
    if (it.id == NULL || (src->nslots > 0 && (it.slots == NULL || it.slot_written == NULL))) {
        it.nslots = 0;
        pc_psd_fx_item_fini(&it);
        return pc_io_nomem(err, "copying a filter cache");
    }
    for (size_t i = 0; i < src->nslots && st == PC_OK; i++) {
        it.slots[i].compression = src->slots[i].compression;
        it.slot_written[i] = src->slot_written[i];
        st = pc_psd_bytes_clone(&src->slots[i].data, &it.slots[i].data, err);
    }
    if (st == PC_OK) {
        st = pc_psd_bytes_clone(&src->mask.data, &it.mask.data, err);
    }
    if (st != PC_OK) {
        pc_psd_fx_item_fini(&it);
        return st;
    }
    *out = it;
    return PC_OK;
}

void pc_io_uuid_from(const void *seed, size_t len, char out[37])
{
    uint8_t h[PC_BLAKE3_LEN];
    static const char H[] = "0123456789abcdef";
    char x[33];
    pc_blake3(seed, len, h);
    for (size_t i = 0; i < 16; i++) {
        x[2 * i] = H[h[i] >> 4];
        x[2 * i + 1] = H[h[i] & 15];
    }
    x[32] = '\0';
    (void)snprintf(out, 37, "%.8s-%.4s-%.4s-%.4s-%.12s", x, x + 8, x + 12, x + 16, x + 20);
}
