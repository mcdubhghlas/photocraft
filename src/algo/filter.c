/* FilterParams: the variant table, registry, filter objects, JSON, halos and output areas (port of
 * crates/algo/src/lib.rs; the apply driver is in apply.c). */
#include "algo/filter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/filter_tables.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

const char *const pc_algo_filter_tags[PC_ALGO_FILTER_VARIANTS] = {
    "gaussianBlur",
    "boxBlur",
    "motionBlur",
    "radialBlur",
    "surfaceBlur",
    "unsharpMask",
    "smartSharpen",
    "highPass",
    "addNoise",
    "median",
    "dustAndScratches",
    "minimum",
    "maximum",
    "offset",
    "mosaic",
    "emboss",
    "findEdges",
    "solarize",
    "invert",
    "desaturate",
    "twirl",
    "pinch",
    "spherize",
    "wave",
    "ripple",
    "polarCoordinates",
    "colorHalftone",
    "crystallize",
    "facet",
    "fragment",
    "mezzotint",
    "pointillize",
    "diffuse",
    "extrude",
    "oilPaint",
    "tiles",
    "traceContour",
    "wind",
    "displace",
    "shear",
    "zigZag",
    "fibers",
    "lensFlare",
    "lightingEffects",
    "relight",
    "reduceNoise",
    "smartBlur",
    "lensBlur",
    "shapeBlur",
    "tiltShift",
    "irisBlur",
    "fieldBlur",
    "spinBlur",
    "pathBlur",
    "custom",
    "hsbHsl",
    "colorToAlpha",
    "deInterlace",
    "ntscColors",
    "filterGallery",
};

const char *const pc_algo_filter_labels[PC_ALGO_FILTER_VARIANTS] = {
    "Gaussian Blur",
    "Box Blur",
    "Motion Blur",
    "Radial Blur",
    "Surface Blur",
    "Unsharp Mask",
    "Smart Sharpen",
    "High Pass",
    "Add Noise",
    "Median",
    "Dust & Scratches",
    "Minimum",
    "Maximum",
    "Offset",
    "Mosaic",
    "Emboss",
    "Find Edges",
    "Solarize",
    "Invert",
    "Desaturate",
    "Twirl",
    "Pinch",
    "Spherize",
    "Wave",
    "Ripple",
    "Polar Coordinates",
    "Color Halftone",
    "Crystallize",
    "Facet",
    "Fragment",
    "Mezzotint",
    "Pointillize",
    "Diffuse",
    "Extrude",
    "Oil Paint",
    "Tiles",
    "Trace Contour",
    "Wind",
    "Displace",
    "Shear",
    "ZigZag",
    "Fibers",
    "Lens Flare",
    "Lighting Effects",
    "Relight",
    "Reduce Noise",
    "Smart Blur",
    "Lens Blur",
    "Shape Blur",
    "Tilt-Shift",
    "Iris Blur",
    "Field Blur",
    "Spin Blur",
    "Path Blur",
    "Custom",
    "HSB/HSL",
    "Color to Alpha",
    "De-Interlace",
    "NTSC Colors",
    "Filter Gallery",
};

int pc_algo_filter_tag_index(const char *tag, size_t len)
{
    int i;
    for (i = 0; i < PC_ALGO_FILTER_VARIANTS; i++) {
        if (strlen(pc_algo_filter_tags[i]) == len && memcmp(pc_algo_filter_tags[i], tag, len) == 0) {
            return i;
        }
    }
    return -1;
}

static const pc_algo_filter_def *const *const TABLES[] = {pc_algo_filters_l5, pc_algo_filters_l6,
                                                          pc_algo_filters_l9};

const pc_algo_filter_def *pc_algo_filter_def_find(const char *tag)
{
    size_t t, i;
    if (tag == NULL) {
        return NULL;
    }
    for (t = 0; t < sizeof TABLES / sizeof TABLES[0]; t++) {
        for (i = 0; TABLES[t][i] != NULL; i++) {
            if (strcmp(TABLES[t][i]->tag, tag) == 0) {
                return TABLES[t][i];
            }
        }
    }
    return NULL;
}

pc_algo_halo pc_algo_halo_reach(float v)
{
    return pc_algo_halo_radius(pc_i32_wrapping_add(pc_f32_to_i32(ceilf(pc_rs_maxf_k(v, 0.0f))), 1));
}

/* ---- filter objects ---- */

static pc_status alloc_params(const pc_algo_filter_def *def, void **out, pc_error *err)
{
    *out = NULL;
    if (def->params_size == 0) {
        return PC_OK;
    }
    *out = calloc(1, def->params_size);
    if (*out == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for filter parameters");
    }
    return PC_OK;
}

static void release_params(const pc_algo_filter_def *def, void *p)
{
    if (p != NULL) {
        pc_algo_jfields_fini(def->fields, def->nfields, p);
        free(p);
    }
}

pc_status pc_algo_filter_make(const pc_algo_filter_def *def, const void *params, pc_algo_filter *out,
                              pc_error *err)
{
    void *p;
    pc_status st;
    if (def == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no filter");
    }
    st = alloc_params(def, &p, err);
    if (st != PC_OK) {
        return st;
    }
    if (p != NULL) {
        if (params != NULL) {
            st = pc_algo_jfields_copy(def->fields, def->nfields, p, params, def->params_size, err);
            if (st != PC_OK) {
                free(p);
                return st;
            }
        } else if (def->init != NULL) {
            def->init(p);
        }
    }
    out->def = def;
    out->params = p;
    return PC_OK;
}

void pc_algo_filter_fini(pc_algo_filter *f)
{
    if (f == NULL) {
        return;
    }
    if (f->def != NULL) {
        release_params(f->def, f->params);
    }
    f->params = NULL;
    f->def = NULL;
}

pc_status pc_algo_filter_clone(const pc_algo_filter *f, pc_algo_filter *out, pc_error *err)
{
    return pc_algo_filter_make(f->def, f->params, out, err);
}

/* The tag value of an internally tagged enum: a variant name (serde's variant identifier). */
static pc_status read_tag(yyjson_val *v, int *idx, pc_error *err)
{
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        size_t len = yyjson_get_len(v);
        *idx = pc_algo_filter_tag_index(s, len);
        if (*idx < 0) {
            (void)pc_algo_jfail_variant(s, len, pc_algo_filter_tags, PC_ALGO_FILTER_VARIANTS, err);
            return PC_ERR_INVALID;
        }
        return PC_OK;
    }
    (void)pc_algo_jfail_type(v, "variant identifier", err);
    return PC_ERR_INVALID;
}

static pc_status from_json(yyjson_val *v, const pc_algo_filter_def *const *extra, pc_algo_filter *out,
                           pc_error *err)
{
    yyjson_val *tag;
    int idx = -1;
    const pc_algo_filter_def *def = NULL;
    void *p;
    size_t i;
    pc_status st;
    if (yyjson_is_arr(v)) {
        /* serde reads the tag from the first element and the fields from the rest, in order. */
        if (yyjson_arr_size(v) == 0) {
            return pc_fail(err, PC_ERR_INVALID, "missing field `filter`");
        }
        st = read_tag(yyjson_arr_get_first(v), &idx, err);
        if (st != PC_OK) {
            return st;
        }
        return pc_fail(err, PC_ERR_UNSUPPORTED, "filter parameters given as a sequence are not supported");
    }
    if (!yyjson_is_obj(v)) {
        return pc_algo_jfail_type(v, "internally tagged enum FilterParams", err);
    }
    tag = pc_json_get(v, "filter"); /* the last duplicate wins, as in serde_json::Value */
    if (tag == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing field `filter`");
    }
    st = read_tag(tag, &idx, err);
    if (st != PC_OK || idx < 0 || idx >= PC_ALGO_FILTER_VARIANTS) {
        return PC_ERR_INVALID;
    }
    for (i = 0; extra != NULL && extra[i] != NULL; i++) {
        if (strcmp(extra[i]->tag, pc_algo_filter_tags[idx]) == 0) {
            def = extra[i];
            break;
        }
    }
    if (def == NULL) {
        def = pc_algo_filter_def_find(pc_algo_filter_tags[idx]);
    }
    if (def == NULL) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "filter `%s` is not available", pc_algo_filter_tags[idx]);
    }
    st = alloc_params(def, &p, err);
    if (st != PC_OK) {
        return st;
    }
    if (p != NULL) {
        if (def->init != NULL) {
            def->init(p);
        }
        st = pc_algo_jfields_read(def->fields, def->nfields, v, p, pc_algo_filter_tags[idx], "filter", err);
        if (st == PC_OK && def->check != NULL) {
            st = def->check(p, err);
            if (st != PC_OK) {
                pc_algo_jfields_fini(def->fields, def->nfields, p);
            }
        }
        if (st != PC_OK) {
            free(p);
            return st;
        }
    }
    out->def = def;
    out->params = p;
    return PC_OK;
}

pc_status pc_algo_filter_from_json(yyjson_val *v, pc_algo_filter *out, pc_error *err)
{
    return from_json(v, NULL, out, err);
}

pc_status pc_algo_filter_from_json_in(yyjson_val *v, const pc_algo_filter_def *const *extra,
                                      pc_algo_filter *out, pc_error *err)
{
    return from_json(v, extra, out, err);
}

pc_status pc_algo_filter_from_json_str(const char *text, size_t len, pc_algo_filter *out, pc_error *err)
{
    yyjson_doc *doc = NULL;
    pc_status st = pc_json_parse(text, len, &doc, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_algo_filter_from_json(yyjson_doc_get_root(doc), out, err);
    yyjson_doc_free(doc);
    return st;
}

yyjson_mut_val *pc_algo_filter_to_json(yyjson_mut_doc *d, const pc_algo_filter *f)
{
    yyjson_mut_val *o;
    if (d == NULL || f == NULL || f->def == NULL) {
        return NULL;
    }
    o = yyjson_mut_obj(d);
    if (o == NULL || !yyjson_mut_obj_add_str(d, o, "filter", f->def->tag)) {
        return NULL;
    }
    if (f->params != NULL && !pc_algo_jfields_write(f->def->fields, f->def->nfields, d, o, f->params)) {
        return NULL;
    }
    return o;
}

pc_status pc_algo_filter_to_json_text(const pc_algo_filter *f, pc_buf *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v;
    pc_status st;
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory writing filter parameters");
    }
    v = pc_algo_filter_to_json(d, f);
    if (v == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory writing filter parameters");
    }
    st = pc_json_write(v, 0, out, err);
    yyjson_mut_doc_free(d);
    return st;
}

bool pc_algo_filter_eq(const pc_algo_filter *a, const pc_algo_filter *b)
{
    pc_buf x, y;
    bool eq;
    if (a->def != b->def) {
        return false;
    }
    pc_buf_init(&x);
    pc_buf_init(&y);
    eq = pc_algo_filter_to_json_text(a, &x, NULL) == PC_OK &&
         pc_algo_filter_to_json_text(b, &y, NULL) == PC_OK && x.len == y.len &&
         (x.len == 0 || memcmp(x.data, y.data, x.len) == 0);
    pc_buf_fini(&x);
    pc_buf_fini(&y);
    return eq;
}

pc_algo_halo pc_algo_filter_halo(const pc_algo_filter *f)
{
    return f->def->halo != NULL ? f->def->halo(f->params, NULL) : pc_algo_halo_radius(0);
}

pc_algo_halo pc_algo_filter_halo_for(const pc_algo_filter *f, pc_rect bounds)
{
    return f->def->halo != NULL ? f->def->halo(f->params, &bounds) : pc_algo_halo_radius(0);
}

bool pc_algo_filter_is_global(const pc_algo_filter *f)
{
    return pc_algo_filter_halo(f).bounds;
}

bool pc_algo_filter_makes_transparency(const pc_algo_filter *f)
{
    return f->def != NULL && strcmp(f->def->tag, "colorToAlpha") == 0;
}

const char *pc_algo_filter_label(const pc_algo_filter *f)
{
    int i;
    if (f->def->label != NULL) {
        return f->def->label(f->params);
    }
    i = pc_algo_filter_tag_index(f->def->tag, strlen(f->def->tag));
    return i >= 0 ? pc_algo_filter_labels[i] : f->def->tag;
}

pc_status pc_algo_kernel(const pc_algo_filter *f, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float **data, size_t *len, pc_error *err)
{
    size_t n;
    float *d = NULL;
    void *prep = NULL;
    pc_status st;
    if (f->def->kernel == NULL) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "filter `%s` has no kernel", f->def->tag);
    }
    if (pc_rect_is_empty(out)) {
        out = PC_RECT_EMPTY;
    }
    if (!pc_algo_float_count(out, src->ch, &n)) {
        return pc_fail(err, PC_ERR_LIMIT, "filter output of %ux%u is too large", pc_rect_width(out),
                       pc_rect_height(out));
    }
    if (f->def->prepare != NULL) {
        st = f->def->prepare(f->params, &prep, err);
        if (st != PC_OK) {
            return st;
        }
    }
    d = pc_calloc_array(n > 0 ? n : 1, sizeof(float));
    if (d == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for filter output");
    } else {
        st = f->def->kernel(f->params, prep, src, out, ctx, d, err);
    }
    if (prep != NULL && f->def->prep_free != NULL) {
        f->def->prep_free(prep);
    }
    if (st != PC_OK) {
        free(d);
        return st;
    }
    if (n == 0) {
        free(d);
        d = NULL;
    }
    *data = d;
    *len = n;
    return PC_OK;
}

pc_rect pc_algo_output_area(const pc_algo_filter *f, pc_rect content, pc_rect bounds,
                            const pc_rect *selection_bounds)
{
    pc_algo_halo h = pc_algo_filter_halo_for(f, bounds);
    pc_rect area;
    if (h.bounds) {
        area = bounds;
    } else if (pc_rect_is_empty(content)) {
        area = PC_RECT_EMPTY;
    } else {
        area = pc_rect_inflate(content, h.radius);
    }
    if (selection_bounds != NULL) {
        area = pc_rect_intersect(area, *selection_bounds);
    }
    return area;
}

int32_t pc_algo_auto_tile(const pc_algo_filter *f, pc_rect bounds)
{
    pc_algo_halo h = pc_algo_filter_halo_for(f, bounds);
    int32_t t;
    if (h.bounds) {
        return PC_ALGO_TILE;
    }
    /* TILE.max((2 * r + 63) / 64 * 64).min(2048), wrapping like the reference's release build. */
    t = pc_i32_wrapping_mul(pc_i32_wrapping_add(pc_i32_wrapping_mul(2, h.radius), 63) / 64, 64);
    if (t < PC_ALGO_TILE) {
        t = PC_ALGO_TILE;
    }
    return t > 2048 ? 2048 : t;
}
