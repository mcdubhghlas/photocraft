/* SVG filters of the render tree (port of usvg 0.45's parser/filter.rs, the `filter` value list
 * parser of svgtypes 0.15's filter_functions.rs and svgtree's fix_recursive_fe_image).
 *
 * A group's `filter` attribute becomes a list of sv_filter (svg_filter.h): references to <filter>
 * elements with their primitives, and CSS filter functions (blur(), drop-shadow(), grayscale()...)
 * as one-primitive filters. As in usvg, an attribute whose every url() is invalid drops the
 * element, a malformed list is ignored, and filters with userSpaceOnUse units on both the region
 * and the primitives are shared between the groups that use them.
 *
 * Caps (the reference has none; each only bounds hostile input):
 *   - a <filter> being converted that is reached again through feImage -> element -> filter is
 *     invalid there (usvg only cuts the direct feImage -> element -> same filter loop and
 *     overflows its stack on longer ones);
 *   - at most SV_MAX_FE_IMAGE_ELEMENTS feImage element references are converted per drawing
 *     (more become usvg's transparent dummy primitive), so DAGs of filters stay linear;
 *   - feImage element conversion counts against the converter's recursion depth. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "base/num.h"
#include "io/svg_filter.h"

#define SV_MAX_FE_IMAGE_ELEMENTS 4096u

typedef struct fcache {
    const st_node *node;
    sv_filter *filter;
} fcache;

struct sv_fconv {
    fcache *cache;
    size_t ncache, capcache;
    const st_node **stack; /* filters being converted */
    size_t nstack, capstack;
    size_t images; /* feImage element references converted */
};

static sv_fconv *fstate(sv_conv *c)
{
    if (c->filt == NULL) {
        c->filt = sv_alloc(c->a, sizeof(sv_fconv));
        if (c->filt == NULL) {
            c->nomem = true;
        }
    }
    return c->filt;
}

/* ---------------------------------------------------------------- attribute helpers */

static bool attr_f32(const st_node *n, sv_aid aid, float *out)
{
    const char *v = st_attr(n, aid);
    double d;
    if (v == NULL || !sv_parse_number(v, &d)) {
        return false;
    }
    *out = pc_f64_to_f32(d);
    return true;
}

static float attr_f32_or(const st_node *n, sv_aid aid, float def)
{
    float v;
    return attr_f32(n, aid, &v) ? v : def;
}

static bool str_is(const char *v, const char *s)
{
    return v != NULL && strcmp(v, s) == 0;
}

/* attribute::<Vec<f32>>: false when missing or any number is malformed. */
static bool attr_list(sv_conv *c, const st_node *n, sv_aid aid, float **out, size_t *nout)
{
    const char *v = st_attr(n, aid);
    sv_stream s;
    float *list = NULL;
    size_t cnt = 0;
    size_t cap = 0;
    if (v == NULL) {
        return false;
    }
    ss_init(&s, v);
    while (!ss_at_end(&s)) {
        double d;
        void *p = list;
        if (!ss_parse_list_number(&s, &d)) {
            return false;
        }
        if (!sv_grow(c->a, &p, &cap, cnt + 1, sizeof(float))) {
            c->nomem = true;
            return false;
        }
        list = p;
        list[cnt++] = pc_f64_to_f32(d);
    }
    *out = list;
    *nout = cnt;
    return true;
}

static bool parse_opacity_attr(const st_node *n, sv_aid aid, float *out)
{
    return sv_parse_opacity(st_attr(n, aid), out);
}

/* svgtypes::Color with its alpha split off (Opacity::new_u8). */
static void color_attr(const st_node *n, sv_aid aid, uint8_t *r, uint8_t *g, uint8_t *b, float *alpha)
{
    sv_color col = {0, 0, 0, 255};
    const char *v = st_attr(n, aid);
    if (v == NULL || !sv_parse_color(v, &col)) {
        sv_color black = {0, 0, 0, 255};
        col = black;
    }
    *r = col.r;
    *g = col.g;
    *b = col.b;
    *alpha = (float)col.a / 255.0f;
}

/* find_attribute::<Color>(Color): black when missing or unparsable. */
static sv_color find_color(const st_node *n)
{
    const st_node *p = st_find_attr_node(n, A_COLOR);
    sv_color c = {0, 0, 0, 255};
    if (p != NULL && !sv_parse_color(st_attr(p, A_COLOR), &c)) {
        sv_color black = {0, 0, 0, 255};
        c = black;
    }
    return c;
}

static float opacity_mul(float a, float b)
{
    float x = a * b;
    return !isfinite(x) ? 0.0f : x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

/* resolve_filter_attr: the node of the filter's href chain (filters only) that has aid. */
static const st_node *resolve_attr(const st_doc *d, const st_node *n, sv_aid aid)
{
    const st_node *cur = n;
    size_t steps = 0;
    if (st_has(n, aid)) {
        return n;
    }
    /* HrefIter, bounded: a chain longer than this loops (the reference would never end). */
    while (steps++ < 256) {
        const st_node *link = st_node_attr(d, cur, A_HREF);
        if (link == NULL || link == cur || link == n || link->eid != E_FILTER) {
            break;
        }
        if (st_has(link, aid)) {
            return link;
        }
        cur = link;
    }
    return n;
}

static sv_units filter_units(sv_conv *c, const st_node *n, sv_aid aid, sv_units def)
{
    const char *v = st_attr(resolve_attr(c->doc, n, aid), aid);
    if (str_is(v, "userSpaceOnUse")) {
        return UNITS_USER;
    }
    if (str_is(v, "objectBoundingBox")) {
        return UNITS_OBB;
    }
    return def;
}

/* try_convert_length */
static bool try_length(sv_conv *c, const st_node *n, sv_aid aid, sv_units units, const sv_cstate *st,
                       float *out)
{
    const char *v = st_attr(n, aid);
    sv_length l;
    if (v == NULL || !sv_parse_length(v, &l)) {
        return false;
    }
    *out = sv_convert_length(c, l, n, aid, units, st);
    return true;
}

/* ---------------------------------------------------------------- result names */

typedef struct rmap {
    const char **names; /* open addressing; latest primitive index per name */
    size_t *idx;
    size_t cap; /* power of two */
    size_t n;
    /* gen_result */
    const char **explicit_names; /* explicitly named results (results.names) */
    size_t *explicit_idx;        /* unused slots: NULL names */
    size_t ecap, en;
    size_t counter; /* results.idx */
} rmap;

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603u;
    for (; *s != '\0'; s++) {
        h = (h ^ (uint8_t)*s) * 1099511628211u;
    }
    return h;
}

static bool set_grow(sv_conv *c, const char ***names, size_t **vals, size_t *cap, size_t n)
{
    size_t ncap;
    const char **nn;
    size_t *nv;
    size_t i;
    if ((n + 1) * 2 <= *cap) {
        return true;
    }
    ncap = *cap == 0 ? 16 : *cap * 2;
    nn = sv_alloc_array(c->a, ncap, sizeof *nn);
    nv = sv_alloc_array(c->a, ncap, sizeof *nv);
    if (nn == NULL || nv == NULL) {
        c->nomem = true;
        return false;
    }
    for (i = 0; i < *cap; i++) {
        if ((*names)[i] != NULL) {
            size_t k = (size_t)hash_str((*names)[i]) & (ncap - 1);
            while (nn[k] != NULL) {
                k = (k + 1) & (ncap - 1);
            }
            nn[k] = (*names)[i];
            nv[k] = (*vals)[i];
        }
    }
    *names = nn;
    *vals = nv;
    *cap = ncap;
    return true;
}

/* Inserts or updates; false when out of memory. */
static bool set_put(sv_conv *c, const char ***names, size_t **vals, size_t *cap, size_t *n, const char *key,
                    size_t val)
{
    size_t k;
    if (!set_grow(c, names, vals, cap, *n)) {
        return false;
    }
    k = (size_t)hash_str(key) & (*cap - 1);
    while ((*names)[k] != NULL) {
        if (strcmp((*names)[k], key) == 0) {
            (*vals)[k] = val;
            return true;
        }
        k = (k + 1) & (*cap - 1);
    }
    (*names)[k] = key;
    (*vals)[k] = val;
    (*n)++;
    return true;
}

static bool set_get(const char *const *names, const size_t *vals, size_t cap, const char *key, size_t *out)
{
    size_t k;
    if (cap == 0) {
        return false;
    }
    k = (size_t)hash_str(key) & (cap - 1);
    while (names[k] != NULL) {
        if (strcmp(names[k], key) == 0) {
            if (out != NULL) {
                *out = vals[k];
            }
            return true;
        }
        k = (k + 1) & (cap - 1);
    }
    return false;
}

/* gen_result */
static const char *gen_result(sv_conv *c, const st_node *n, rmap *m)
{
    const char *s = st_attr(n, A_RESULT);
    if (s != NULL) {
        if (!set_put(c, &m->explicit_names, &m->explicit_idx, &m->ecap, &m->en, s, 0)) {
            return NULL;
        }
        m->counter++;
        return s;
    }
    for (;;) {
        char buf[32];
        (void)snprintf(buf, sizeof buf, "result%zu", m->counter);
        m->counter++;
        if (!set_get(m->explicit_names, m->explicit_idx, m->ecap, buf, NULL)) {
            char *r = sv_strndup(c->a, buf, strlen(buf));
            if (r == NULL) {
                c->nomem = true;
            }
            return r;
        }
    }
}

/* resolve_input */
static sv_fin resolve_input(const st_node *n, sv_aid aid, const sv_fprim *prims, size_t nprims, const rmap *m)
{
    const char *s = st_attr(n, aid);
    sv_fin in;
    memset(&in, 0, sizeof in);
    in.kind = FIN_SOURCE_GRAPHIC;
    if (s == NULL) {
        if (nprims > 0) {
            in.kind = FIN_REF;
            in.name = prims[nprims - 1].result;
            in.ref = nprims - 1;
        }
        return in;
    }
    if (strcmp(s, "SourceGraphic") == 0 || strcmp(s, "BackgroundImage") == 0 ||
        strcmp(s, "BackgroundAlpha") == 0 || strcmp(s, "FillPaint") == 0 || strcmp(s, "StrokePaint") == 0) {
        return in;
    }
    if (strcmp(s, "SourceAlpha") == 0) {
        in.kind = FIN_SOURCE_ALPHA;
        return in;
    }
    {
        size_t idx;
        if (set_get(m->names, m->idx, m->cap, s, &idx)) {
            in.kind = FIN_REF;
            in.name = s;
            in.ref = idx;
        } else if (nprims > 0) {
            /* an unknown reference: the previous result */
            in.kind = FIN_REF;
            in.name = prims[nprims - 1].result;
            in.ref = nprims - 1;
        }
    }
    return in;
}

/* ---------------------------------------------------------------- primitives */

/* create_dummy_primitive: a transparent flood. */
static void dummy(sv_fprim *p)
{
    p->kind = FK_FLOOD;
    p->r = p->g = p->b = 0;
    p->opacity = 0.0f;
}

static sv_blend blend_mode(const char *v)
{
    static const struct {
        const char *s;
        sv_blend b;
    } M[] = {{"normal", BM_NORMAL},
             {"multiply", BM_MULTIPLY},
             {"screen", BM_SCREEN},
             {"overlay", BM_OVERLAY},
             {"darken", BM_DARKEN},
             {"lighten", BM_LIGHTEN},
             {"color-dodge", BM_COLOR_DODGE},
             {"color-burn", BM_COLOR_BURN},
             {"hard-light", BM_HARD_LIGHT},
             {"soft-light", BM_SOFT_LIGHT},
             {"difference", BM_DIFFERENCE},
             {"exclusion", BM_EXCLUSION},
             {"hue", BM_HUE},
             {"saturation", BM_SATURATION},
             {"color", BM_COLOR},
             {"luminosity", BM_LUMINOSITY}};
    size_t i;
    for (i = 0; v != NULL && i < sizeof M / sizeof M[0]; i++) {
        if (strcmp(v, M[i].s) == 0) {
            return M[i].b;
        }
    }
    return BM_NORMAL;
}

static void identity_matrix(sv_fprim *p)
{
    memset(p->m, 0, sizeof p->m);
    p->m[0] = p->m[6] = p->m[12] = p->m[18] = 1.0f;
}

static void convert_color_matrix(sv_conv *c, const st_node *fe, sv_fprim *p)
{
    const char *type = st_attr(fe, A_TYPE);
    float *list;
    size_t n;
    p->cm = CM_MATRIX;
    identity_matrix(p);
    if (str_is(type, "saturate")) {
        if (attr_list(c, fe, A_VALUES, &list, &n)) {
            p->cm = CM_SATURATE;
            p->cm_value = n > 0 ? sv_f32_bound(0.0f, list[0], 1.0f) : 1.0f;
        }
    } else if (str_is(type, "hueRotate")) {
        if (attr_list(c, fe, A_VALUES, &list, &n)) {
            p->cm = CM_HUE_ROTATE;
            p->cm_value = n > 0 ? list[0] : 0.0f;
        }
    } else if (str_is(type, "luminanceToAlpha")) {
        p->cm = CM_LUMINANCE_TO_ALPHA;
    } else if (attr_list(c, fe, A_VALUES, &list, &n) && n == 20) {
        memcpy(p->m, list, sizeof p->m);
    }
}

static bool transfer_function(sv_conv *c, const st_node *n, sv_tfunc *f)
{
    const char *type = st_attr(n, A_TYPE);
    float *list;
    size_t cnt;
    if (type == NULL) {
        return false;
    }
    memset(f, 0, sizeof *f);
    if (strcmp(type, "identity") == 0) {
        f->kind = TF_IDENTITY;
    } else if (strcmp(type, "table") == 0 || strcmp(type, "discrete") == 0) {
        f->kind = type[0] == 't' ? TF_TABLE : TF_DISCRETE;
        if (attr_list(c, n, A_TABLE_VALUES, &list, &cnt)) {
            f->v = list;
            f->n = cnt;
        }
    } else if (strcmp(type, "linear") == 0) {
        f->kind = TF_LINEAR;
        f->slope = attr_f32_or(n, A_SLOPE, 1.0f);
        f->intercept = attr_f32_or(n, A_INTERCEPT, 0.0f);
    } else if (strcmp(type, "gamma") == 0) {
        f->kind = TF_GAMMA;
        f->amplitude = attr_f32_or(n, A_AMPLITUDE, 1.0f);
        f->exponent = attr_f32_or(n, A_EXPONENT, 1.0f);
        f->offset = attr_f32_or(n, A_OFFSET, 0.0f);
    } else {
        return false;
    }
    return true;
}

static void convert_component_transfer(sv_conv *c, const st_node *fe, sv_fprim *p)
{
    const st_node *ch;
    int i;
    for (i = 0; i < 4; i++) {
        p->func[i].kind = TF_IDENTITY;
    }
    for (ch = fe->first; ch != NULL; ch = ch->next) {
        sv_tfunc f;
        int k;
        if (ch->kind != ST_ELEMENT || !transfer_function(c, ch, &f)) {
            continue;
        }
        k = ch->eid == E_FE_FUNC_R   ? 0
            : ch->eid == E_FE_FUNC_G ? 1
            : ch->eid == E_FE_FUNC_B ? 2
            : ch->eid == E_FE_FUNC_A ? 3
                                     : -1;
        if (k >= 0) {
            p->func[k] = f;
        }
    }
}

static void convert_composite(const st_node *fe, sv_fprim *p)
{
    const char *op = st_attr(fe, A_OPERATOR);
    if (op == NULL) {
        op = "over";
    }
    p->op = strcmp(op, "in") == 0           ? CO_IN
            : strcmp(op, "out") == 0        ? CO_OUT
            : strcmp(op, "atop") == 0       ? CO_ATOP
            : strcmp(op, "xor") == 0        ? CO_XOR
            : strcmp(op, "arithmetic") == 0 ? CO_ARITHMETIC
                                            : CO_OVER;
    if (p->op == CO_ARITHMETIC) {
        p->k1 = attr_f32_or(fe, A_K1, 0.0f);
        p->k2 = attr_f32_or(fe, A_K2, 0.0f);
        p->k3 = attr_f32_or(fe, A_K3, 0.0f);
        p->k4 = attr_f32_or(fe, A_K4, 0.0f);
    }
}

/* parse_target: false when out of range. */
static bool parse_target(const st_node *fe, sv_aid aid, uint32_t order, uint32_t *out)
{
    float def = floorf((float)order / 2.0f);
    float t;
    int32_t v;
    if (!attr_f32(fe, aid, &t)) {
        t = (float)pc_f32_to_u32(def);
    }
    v = pc_f32_to_i32(t);
    if (v < 0 || (int64_t)v >= (int64_t)order) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

/* convert_convolve_matrix: false gives the dummy primitive. */
static bool convert_convolve(sv_conv *c, const st_node *fe, sv_fprim *p)
{
    uint32_t ox = 3;
    uint32_t oy = 3;
    const char *v = st_attr(fe, A_ORDER);
    float *list;
    size_t n;
    float *matrix = NULL;
    size_t nmatrix = 0;
    float sum = -0.0f;
    float divisor;
    size_t i;
    const char *edge;
    if (v != NULL) {
        sv_stream s;
        double d;
        int32_t x = 3;
        int32_t y;
        ss_init(&s, v);
        if (!ss_at_end(&s)) {
            if (ss_parse_list_number(&s, &d)) {
                x = pc_f64_to_i32(d);
            } else {
                s.pos = s.len;
            }
        }
        y = x;
        if (!ss_at_end(&s) && ss_parse_list_number(&s, &d)) {
            y = pc_f64_to_i32(d);
        }
        if (x > 0 && y > 0) {
            ox = (uint32_t)x;
            oy = (uint32_t)y;
        }
    }
    /* (order_x * order_y) as usize: a u32 product (wrapping) */
    if (attr_list(c, fe, A_KERNEL_MATRIX, &list, &n) && (uint64_t)n == (uint64_t)(uint32_t)(ox * oy)) {
        matrix = list;
        nmatrix = n;
    }
    for (i = 0; i < nmatrix; i++) {
        sum += matrix[i];
    }
    sum = roundf(sum * 1000000.0f) / 1000000.0f;
    if (sv_approx_eq_ulps(sum, 0.0f, 4)) {
        sum = 1.0f;
    }
    divisor = attr_f32_or(fe, A_DIVISOR, sum);
    if (sv_approx_eq_ulps(divisor, 0.0f, 4)) {
        return false;
    }
    p->bias = attr_f32_or(fe, A_BIAS, 0.0f);
    if (!parse_target(fe, A_TARGET_X, ox, &p->target_x) || !parse_target(fe, A_TARGET_Y, oy, &p->target_y)) {
        return false;
    }
    /* ConvolveMatrixData::new; a product that overflows u32 is refused (the reference would
     * index past the matrix). */
    if ((uint64_t)ox * oy != (uint64_t)nmatrix) {
        return false;
    }
    p->columns = ox;
    p->rows = oy;
    p->matrix = matrix;
    p->divisor = divisor;
    edge = st_attr(fe, A_EDGE_MODE);
    p->edge = str_is(edge, "none") ? EDGE_NONE : str_is(edge, "wrap") ? EDGE_WRAP : EDGE_DUPLICATE;
    p->preserve_alpha = str_is(st_attr(fe, A_PRESERVE_ALPHA), "true");
    return true;
}

static sv_channel channel_of(const st_node *fe, sv_aid aid)
{
    const char *v = st_attr(fe, aid);
    return str_is(v, "R") ? CH_R : str_is(v, "G") ? CH_G : str_is(v, "B") ? CH_B : CH_A;
}

/* convert_std_dev_attr */
static void std_dev(const st_node *fe, float sw, float sh, const char *def, float *ox, float *oy)
{
    const char *text = st_attr(fe, A_STD_DEVIATION);
    sv_stream s;
    double n[3];
    bool ok[3];
    int i;
    double x = 0.0;
    double y = 0.0;
    float fx;
    float fy;
    ss_init(&s, text != NULL ? text : def);
    for (i = 0; i < 3; i++) {
        ok[i] = false;
        if (!ss_at_end(&s)) {
            ok[i] = ss_parse_list_number(&s, &n[i]);
            if (!ok[i]) {
                s.pos = s.len;
            }
        }
    }
    if (ok[0] && ok[1] && !ok[2]) {
        x = n[0];
        y = n[1];
    } else if (ok[0] && !ok[1] && !ok[2]) {
        x = n[0];
        y = n[0];
    }
    fx = pc_f64_to_f32(x) * sw;
    fy = pc_f64_to_f32(y) * sh;
    *ox = isfinite(fx) && fx >= 0.0f ? fx : 0.0f;
    *oy = isfinite(fy) && fy >= 0.0f ? fy : 0.0f;
}

static void convert_flood_like(const st_node *fe, sv_fprim *p)
{
    float alpha;
    float fo = 1.0f;
    color_attr(fe, A_FLOOD_COLOR, &p->r, &p->g, &p->b, &alpha);
    (void)parse_opacity_attr(fe, A_FLOOD_OPACITY, &fo);
    p->opacity = opacity_mul(alpha, fo);
}

/* convert_lighting_color (alpha ignored) */
static void lighting_color(const st_node *fe, sv_fprim *p)
{
    const char *v = st_attr(fe, A_LIGHTING_COLOR);
    sv_color col = {255, 255, 255, 255};
    if (str_is(v, "currentColor")) {
        col = find_color(fe);
    } else if (v != NULL && !sv_parse_color(v, &col)) {
        sv_color white = {255, 255, 255, 255};
        col = white;
    }
    p->r = col.r;
    p->g = col.g;
    p->b = col.b;
}

static bool light_source(const st_node *fe, sv_light *l)
{
    const st_node *ch;
    memset(l, 0, sizeof *l);
    for (ch = fe->first; ch != NULL; ch = ch->next) {
        if (ch->kind != ST_ELEMENT) {
            continue;
        }
        if (ch->eid == E_FE_DISTANT_LIGHT) {
            l->kind = LIGHT_DISTANT;
            l->azimuth = attr_f32_or(ch, A_AZIMUTH, 0.0f);
            l->elevation = attr_f32_or(ch, A_ELEVATION, 0.0f);
            return true;
        }
        if (ch->eid == E_FE_POINT_LIGHT || ch->eid == E_FE_SPOT_LIGHT) {
            l->kind = ch->eid == E_FE_POINT_LIGHT ? LIGHT_POINT : LIGHT_SPOT;
            l->x = attr_f32_or(ch, A_X, 0.0f);
            l->y = attr_f32_or(ch, A_Y, 0.0f);
            l->z = attr_f32_or(ch, A_Z, 0.0f);
            if (l->kind == LIGHT_SPOT) {
                float e = attr_f32_or(ch, A_SPECULAR_EXPONENT, 1.0f);
                l->px = attr_f32_or(ch, A_POINTS_AT_X, 0.0f);
                l->py = attr_f32_or(ch, A_POINTS_AT_Y, 0.0f);
                l->pz = attr_f32_or(ch, A_POINTS_AT_Z, 0.0f);
                l->spec_exp = isfinite(e) && e >= 0.0f ? e : 1.0f;
                l->has_cone = attr_f32(ch, A_LIMITING_CONE_ANGLE, &l->cone);
            }
            return true;
        }
    }
    return false;
}

static bool convert_lighting(const st_node *fe, bool specular, sv_fprim *p)
{
    if (!light_source(fe, &p->light)) {
        return false;
    }
    p->surface_scale = attr_f32_or(fe, A_SURFACE_SCALE, 1.0f);
    if (specular) {
        float e = attr_f32_or(fe, A_SPECULAR_EXPONENT, 1.0f);
        if (!(e >= 1.0f && e <= 128.0f)) {
            return false;
        }
        p->exponent = e;
        p->constant = attr_f32_or(fe, A_SPECULAR_CONSTANT, 1.0f);
    } else {
        p->constant = attr_f32_or(fe, A_DIFFUSE_CONSTANT, 1.0f);
    }
    lighting_color(fe, p);
    return true;
}

static void convert_morphology(sv_conv *c, const st_node *fe, float sw, float sh, sv_fprim *p)
{
    float *list;
    size_t n;
    p->dilate = str_is(st_attr(fe, A_OPERATOR), "dilate");
    p->rx = sw;
    p->ry = sh;
    if (attr_list(c, fe, A_RADIUS, &list, &n)) {
        float rx = 0.0f;
        float ry = 0.0f;
        if (n == 2) {
            rx = list[0];
            ry = list[1];
        } else if (n == 1) {
            rx = list[0];
            ry = list[0];
        }
        if (sv_approx_eq_ulps(rx, 0.0f, 4) && sv_approx_eq_ulps(ry, 0.0f, 4)) {
            rx = 1.0f;
            ry = 1.0f;
        }
        if (sv_approx_eq_ulps(rx, 0.0f, 4) && !sv_approx_eq_ulps(ry, 0.0f, 4)) {
            rx = 1.0f;
        }
        if (!sv_approx_eq_ulps(rx, 0.0f, 4) && sv_approx_eq_ulps(ry, 0.0f, 4)) {
            ry = 1.0f;
        }
        if (!signbit(rx) && !signbit(ry)) {
            float x = rx * sw;
            float y = ry * sh;
            /* An infinite radius panics in the reference (PositiveF32::new(..).unwrap()); here
             * it keeps the default. */
            if (isfinite(x) && isfinite(y)) {
                p->rx = x;
                p->ry = y;
            }
        }
    }
}

static void convert_turbulence(sv_conv *c, const st_node *fe, sv_fprim *p)
{
    float *list;
    size_t n;
    float oct = attr_f32_or(fe, A_NUM_OCTAVES, 1.0f);
    float seed;
    if (attr_list(c, fe, A_BASE_FREQUENCY, &list, &n)) {
        float x = 0.0f;
        float y = 0.0f;
        if (n == 2) {
            x = list[0];
            y = list[1];
        } else if (n == 1) {
            x = list[0];
            y = list[0];
        }
        /* infinite frequencies panic in the reference; here they stay zero */
        if (!signbit(x) && !signbit(y) && isfinite(x) && isfinite(y)) {
            p->bfx = x;
            p->bfy = y;
        }
    }
    if (signbit(oct)) {
        oct = 0.0f;
    }
    p->octaves = pc_f32_to_u32(roundf(oct));
    seed = attr_f32_or(fe, A_SEED, 0.0f);
    p->seed = pc_f32_to_i32(truncf(seed));
    p->stitch = str_is(st_attr(fe, A_STITCH_TILES), "stitch");
    p->fractal = str_is(st_attr(fe, A_TYPE), "fractalNoise");
}

/* ---------------------------------------------------------------- feImage */

static const char *find_str(const st_node *n, sv_aid aid)
{
    const st_node *p = st_find_attr_node(n, aid);
    return p != NULL ? st_attr(p, aid) : NULL;
}

static void fit_view_box(float sw, float sh, sv_rect r, sv_aspect a, float *ow, float *oh)
{
    float tw = rect_w(r);
    float th = rect_h(r);
    float rw;
    bool with_h;
    if (a.align == AL_NONE) {
        *ow = tw;
        *oh = th;
        return;
    }
    rw = th * sw / sh;
    with_h = a.slice ? rw <= tw : rw >= tw;
    if (!with_h) {
        *ow = rw;
        *oh = th;
    } else {
        *ow = tw;
        *oh = tw * sh / sw;
    }
}

/* image::convert_inner for an feImage's data (the group and image under root). */
static bool image_inner(sv_conv *c, const sv_href_image *li, const st_node *fe, sv_rect rect, sv_unode *root)
{
    sv_aspect aspect = sv_aspect_default();
    float vw;
    float vh;
    float vx;
    float vy;
    sv_ts image_ts;
    sv_unode *img;
    sv_unode *g;
    const char *rend;
    const char *pa = st_attr(fe, A_PRESERVE_ASPECT_RATIO);
    if (pa != NULL && !sv_parse_aspect(pa, &aspect)) {
        aspect = sv_aspect_default();
    }
    fit_view_box(li->w, li->h, rect, aspect, &vw, &vh);
    sv_aligned_pos(aspect.align, rect.l, rect.t, rect_w(rect) - vw, rect_h(rect) - vh, &vx, &vy);
    image_ts = ts_row(vw / li->w, 0.0f, 0.0f, vh / li->h, vx, vy);
    img = sv_alloc(c->a, sizeof *img);
    g = sv_group_empty(c->a);
    if (img == NULL || g == NULL) {
        c->nomem = true;
        return false;
    }
    img->kind = UN_IMAGE;
    img->id = "";
    img->visible = true;
    rend = find_str(fe, A_IMAGE_RENDERING);
    img->pixelated = rend != NULL && (strcmp(rend, "optimizeSpeed") == 0 ||
                                      strcmp(rend, "crisp-edges") == 0 || strcmp(rend, "pixelated") == 0);
    img->quality = img->pixelated ? SQ_NEAREST : str_is(rend, "smooth") ? SQ_BILINEAR : SQ_BICUBIC;
    img->iw = li->iw;
    img->ih = li->ih;
    img->rgba = li->rgba;
    img->svg = li->svg;
    img->size_w = li->w;
    img->size_h = li->h;
    img->transform = ts_identity();
    img->abs_transform = ts_pre_concat(root->abs_transform, image_ts);
    if (!rect_transform(rect, img->abs_transform, &img->abs_bbox)) {
        return false;
    }
    (void)rect_from_xywh(0.0f, 0.0f, li->w, li->h, &img->bbox);
    img->stroke_bbox = img->bbox;
    img->abs_stroke_bbox = img->abs_bbox;
    if (!sv_push_child(c->a, g, img)) {
        c->nomem = true;
        return false;
    }
    g->transform = image_ts;
    g->abs_transform = img->abs_transform;
    (void)sv_group_bboxes(g);
    if (aspect.slice) {
        sv_unode *g2 = sv_group_empty(c->a);
        sv_clip *clip = sv_alloc(c->a, sizeof *clip);
        sv_unode *croot = sv_group_empty(c->a);
        sv_fill *fill = sv_alloc(c->a, sizeof *fill);
        const sv_path *data = path_from_rect(c->a, rect);
        sv_unode *p;
        if (g2 == NULL || clip == NULL || croot == NULL || fill == NULL || data == NULL) {
            c->nomem = true;
            return false;
        }
        fill->paint.kind = PAINT_COLOR;
        fill->opacity = 1.0f;
        p = sv_new_path(c, "", true, fill, NULL, false, data, ts_identity());
        if (p == NULL || !sv_push_child(c->a, croot, p)) {
            c->nomem = true;
            return false;
        }
        clip->transform = ts_identity();
        clip->root = croot;
        g2->abs_transform = root->abs_transform;
        g2->clip = clip;
        if (!sv_push_child(c->a, g2, g)) {
            c->nomem = true;
            return false;
        }
        (void)sv_group_bboxes(g2);
        g = g2;
    }
    if (!sv_push_child(c->a, root, g)) {
        c->nomem = true;
        return false;
    }
    return true;
}

/* convert_image_inner: false gives the dummy primitive. */
static bool convert_image(sv_conv *c, const st_node *fe, sv_rect subregion, const sv_cstate *st, sv_fprim *p)
{
    const st_node *node = st_node_attr(c->doc, fe, A_HREF);
    sv_unode *root = sv_group_empty(c->a);
    sv_fconv *fs = fstate(c);
    if (root == NULL || fs == NULL) {
        c->nomem = true;
        return false;
    }
    if (node != NULL) {
        if (fs->images >= SV_MAX_FE_IMAGE_ELEMENTS || c->depth > SV_MAX_CONV_DEPTH) {
            return false;
        }
        fs->images++;
        c->depth++;
        sv_convert_element(c, node, st, root);
        c->depth--;
        if (root->nchildren == 0) {
            return false;
        }
        (void)sv_group_bboxes(root);
        p->root = root;
        return true;
    }
    {
        const char *href = st_attr(fe, A_HREF);
        sv_href_image li;
        sv_rect r;
        if (href == NULL || !sv_load_href(c, href, &li) || !(li.w > 0.0f) || !(li.h > 0.0f)) {
            return false;
        }
        if (!nzrect_from_xywh(0.0f, 0.0f, rect_w(subregion), rect_h(subregion), &r)) {
            return false;
        }
        if (!image_inner(c, &li, fe, r, root)) {
            return false;
        }
        (void)sv_group_bboxes(root);
        p->root = root;
        return true;
    }
}

/* ---------------------------------------------------------------- filter elements */

/* resolve_primitive_region: false stops the collection. */
static bool primitive_region(sv_conv *c, const st_node *fe, sv_units units, const sv_cstate *st,
                             const sv_rect *bbox, sv_rect filter_region, sv_rect *out)
{
    float x;
    float y;
    float w;
    float h;
    bool hx = try_length(c, fe, A_X, units, st, &x);
    bool hy = try_length(c, fe, A_Y, units, st, &y);
    bool hw = try_length(c, fe, A_WIDTH, units, st, &w);
    bool hh = try_length(c, fe, A_HEIGHT, units, st, &h);
    sv_rect region = filter_region;
    sv_rect sub;
    if ((fe->eid == E_FE_FLOOD || fe->eid == E_FE_IMAGE) && units == UNITS_OBB) {
        sv_rect r;
        if (bbox == NULL ||
            !nzrect_from_xywh(hx ? x : 0.0f, hy ? y : 0.0f, hw ? w : 1.0f, hh ? h : 1.0f, &r)) {
            return false;
        }
        *out = rect_bbox_transform(r, *bbox);
        return true;
    }
    if (units == UNITS_OBB) {
        if (!nzrect_from_xywh(hx ? x : 0.0f, hy ? y : 0.0f, hw ? w : 1.0f, hh ? h : 1.0f, &sub)) {
            return false;
        }
        *out = rect_bbox_transform(region, sub);
        return true;
    }
    return nzrect_from_xywh(hx ? x : region.l, hy ? y : region.t, hw ? w : rect_w(region),
                            hh ? h : rect_h(region), out);
}

static bool is_primitive(sv_eid e)
{
    switch (e) {
    case E_FE_DROP_SHADOW:
    case E_FE_GAUSSIAN_BLUR:
    case E_FE_OFFSET:
    case E_FE_BLEND:
    case E_FE_FLOOD:
    case E_FE_COMPOSITE:
    case E_FE_MERGE:
    case E_FE_TILE:
    case E_FE_IMAGE:
    case E_FE_COMPONENT_TRANSFER:
    case E_FE_COLOR_MATRIX:
    case E_FE_CONVOLVE_MATRIX:
    case E_FE_MORPHOLOGY:
    case E_FE_DISPLACEMENT_MAP:
    case E_FE_TURBULENCE:
    case E_FE_DIFFUSE_LIGHTING:
    case E_FE_SPECULAR_LIGHTING:
        return true;
    default:
        return false;
    }
}

/* One primitive of kind fe->eid (convert_*). */
static bool convert_kind(sv_conv *c, const st_node *fe, float sw, float sh, sv_rect subregion,
                         const sv_cstate *st, const sv_fprim *prims, size_t np, const rmap *m, sv_fprim *p)
{
    switch (fe->eid) {
    case E_FE_DROP_SHADOW: {
        float alpha;
        float fo = 1.0f;
        p->kind = FK_DROP_SHADOW;
        std_dev(fe, sw, sh, "2 2", &p->sdx, &p->sdy);
        color_attr(fe, A_FLOOD_COLOR, &p->r, &p->g, &p->b, &alpha);
        (void)parse_opacity_attr(fe, A_FLOOD_OPACITY, &fo);
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        p->dx = attr_f32_or(fe, A_DX, 2.0f) * sw;
        p->dy = attr_f32_or(fe, A_DY, 2.0f) * sh;
        p->opacity = opacity_mul(alpha, fo);
        break;
    }
    case E_FE_GAUSSIAN_BLUR:
        p->kind = FK_GAUSSIAN_BLUR;
        std_dev(fe, sw, sh, "0 0", &p->sdx, &p->sdy);
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        break;
    case E_FE_OFFSET:
        p->kind = FK_OFFSET;
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        p->dx = attr_f32_or(fe, A_DX, 0.0f) * sw;
        p->dy = attr_f32_or(fe, A_DY, 0.0f) * sh;
        break;
    case E_FE_BLEND:
        p->kind = FK_BLEND;
        p->mode = blend_mode(st_attr(fe, A_MODE));
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        p->in2 = resolve_input(fe, A_IN2, prims, np, m);
        break;
    case E_FE_FLOOD:
        p->kind = FK_FLOOD;
        convert_flood_like(fe, p);
        break;
    case E_FE_COMPOSITE:
        p->kind = FK_COMPOSITE;
        convert_composite(fe, p);
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        p->in2 = resolve_input(fe, A_IN2, prims, np, m);
        break;
    case E_FE_MERGE: {
        const st_node *ch;
        size_t n = 0;
        size_t i = 0;
        p->kind = FK_MERGE;
        for (ch = fe->first; ch != NULL; ch = ch->next) {
            n += ch->kind == ST_ELEMENT;
        }
        if (n > 0) {
            p->inputs = sv_alloc_array(c->a, n, sizeof(sv_fin));
            if (p->inputs == NULL) {
                c->nomem = true;
                return false;
            }
            for (ch = fe->first; ch != NULL; ch = ch->next) {
                if (ch->kind == ST_ELEMENT) {
                    p->inputs[i++] = resolve_input(ch, A_IN, prims, np, m);
                }
            }
        }
        p->ninputs = n;
        break;
    }
    case E_FE_TILE:
        p->kind = FK_TILE;
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        break;
    case E_FE_IMAGE:
        p->kind = FK_IMAGE;
        if (!convert_image(c, fe, subregion, st, p)) {
            dummy(p);
        }
        break;
    case E_FE_COMPONENT_TRANSFER:
        p->kind = FK_COMPONENT_TRANSFER;
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        convert_component_transfer(c, fe, p);
        break;
    case E_FE_COLOR_MATRIX:
        p->kind = FK_COLOR_MATRIX;
        convert_color_matrix(c, fe, p);
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        break;
    case E_FE_CONVOLVE_MATRIX:
        p->kind = FK_CONVOLVE_MATRIX;
        if (convert_convolve(c, fe, p)) {
            p->in1 = resolve_input(fe, A_IN, prims, np, m);
        } else {
            dummy(p);
        }
        break;
    case E_FE_MORPHOLOGY:
        p->kind = FK_MORPHOLOGY;
        convert_morphology(c, fe, sw, sh, p);
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        break;
    case E_FE_DISPLACEMENT_MAP:
        p->kind = FK_DISPLACEMENT_MAP;
        p->in1 = resolve_input(fe, A_IN, prims, np, m);
        p->in2 = resolve_input(fe, A_IN2, prims, np, m);
        p->scale = attr_f32_or(fe, A_SCALE, 0.0f) * ((sw + sh) / 2.0f);
        p->xch = channel_of(fe, A_X_CHANNEL_SELECTOR);
        p->ych = channel_of(fe, A_Y_CHANNEL_SELECTOR);
        break;
    case E_FE_TURBULENCE:
        p->kind = FK_TURBULENCE;
        convert_turbulence(c, fe, p);
        break;
    case E_FE_DIFFUSE_LIGHTING:
    case E_FE_SPECULAR_LIGHTING: {
        bool spec = fe->eid == E_FE_SPECULAR_LIGHTING;
        p->kind = spec ? FK_SPECULAR_LIGHTING : FK_DIFFUSE_LIGHTING;
        if (convert_lighting(fe, spec, p)) {
            p->in1 = resolve_input(fe, A_IN, prims, np, m);
        } else {
            memset(&p->light, 0, sizeof p->light);
            dummy(p);
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

/* collect_children: NULL list (count 0) when nothing converts. */
static void collect_children(sv_conv *c, const st_node *filter, sv_units units, const sv_cstate *st,
                             const sv_rect *bbox, sv_rect region, sv_fprim **out, size_t *nout)
{
    sv_fprim *prims = NULL;
    size_t np = 0;
    size_t cap = 0;
    rmap m;
    float sw = 1.0f;
    float sh = 1.0f;
    const st_node *ch;
    *out = NULL;
    *nout = 0;
    memset(&m, 0, sizeof m);
    m.counter = 1;
    if (units == UNITS_OBB) {
        if (bbox == NULL) {
            return;
        }
        sw = rect_w(*bbox);
        sh = rect_h(*bbox);
    }
    for (ch = filter->first; ch != NULL && !c->nomem; ch = ch->next) {
        sv_rect sub;
        sv_fprim p;
        const st_node *ci;
        if (ch->kind != ST_ELEMENT || ch->eid == E_NONE) {
            continue;
        }
        if (!primitive_region(c, ch, units, st, bbox, region, &sub)) {
            break;
        }
        if (!is_primitive(ch->eid)) {
            continue;
        }
        memset(&p, 0, sizeof p);
        p.rect = sub;
        if (!convert_kind(c, ch, sw, sh, sub, st, prims, np, &m, &p)) {
            continue;
        }
        ci = st_find_attr_node(ch, A_COLOR_INTERPOLATION_FILTERS);
        p.linear = ci == NULL || !str_is(st_attr(ci, A_COLOR_INTERPOLATION_FILTERS), "sRGB");
        p.result = gen_result(c, ch, &m);
        if (p.result == NULL) {
            return;
        }
        {
            void *v = prims;
            if (!sv_grow(c->a, &v, &cap, np + 1, sizeof(sv_fprim))) {
                c->nomem = true;
                return;
            }
            prims = v;
        }
        prims[np] = p;
        if (!set_put(c, &m.names, &m.idx, &m.cap, &m.n, p.result, np)) {
            return;
        }
        np++;
    }
    *out = prims;
    *nout = np;
}

static bool in_progress(const sv_fconv *fs, const st_node *n)
{
    size_t i;
    for (i = 0; i < fs->nstack; i++) {
        if (fs->stack[i] == n) {
            return true;
        }
    }
    return false;
}

/* convert_url: 1 = *out, -1 = invalid. */
static int convert_url(sv_conv *c, const st_node *node, const sv_cstate *st, const sv_rect *object_bbox,
                       sv_filter **out)
{
    sv_units units = filter_units(c, node, A_FILTER_UNITS, UNITS_OBB);
    sv_units punits = filter_units(c, node, A_PRIMITIVE_UNITS, UNITS_USER);
    bool cacheable = units == UNITS_USER && punits == UNITS_USER;
    sv_fconv *fs = fstate(c);
    sv_rect rect;
    const st_node *with = NULL;
    const st_node *cur = node;
    sv_filter *f;
    size_t i;
    if (fs == NULL) {
        return -1;
    }
    if (cacheable) {
        for (i = 0; i < fs->ncache; i++) {
            if (fs->cache[i].node == node) {
                *out = fs->cache[i].filter;
                return 1;
            }
        }
    }
    if (!nzrect_from_xywh(
            sv_node_length(c, resolve_attr(c->doc, node, A_X), A_X, units, st, sv_len_pct(-10.0)),
            sv_node_length(c, resolve_attr(c->doc, node, A_Y), A_Y, units, st, sv_len_pct(-10.0)),
            sv_node_length(c, resolve_attr(c->doc, node, A_WIDTH), A_WIDTH, units, st, sv_len_pct(120.0)),
            sv_node_length(c, resolve_attr(c->doc, node, A_HEIGHT), A_HEIGHT, units, st, sv_len_pct(120.0)),
            &rect)) {
        return -1;
    }
    if (units == UNITS_OBB) {
        if (object_bbox == NULL) {
            return -1;
        }
        rect = rect_bbox_transform(rect, *object_bbox);
    }
    /* find_filter_with_primitives (HrefIter, bounded) */
    for (i = 0; i < 257 && cur != NULL; i++) {
        const st_node *next;
        if (cur->eid != E_FILTER) {
            return -1;
        }
        if (cur->first != NULL) {
            with = cur;
            break;
        }
        next = st_node_attr(c->doc, cur, A_HREF);
        if (next == cur || next == node) {
            break;
        }
        cur = next;
    }
    if (with == NULL || in_progress(fs, node)) {
        return -1;
    }
    f = sv_alloc(c->a, sizeof *f);
    {
        void *v = fs->stack;
        if (f == NULL || !sv_grow(c->a, &v, &fs->capstack, fs->nstack + 1, sizeof(const st_node *))) {
            c->nomem = true;
            return -1;
        }
        fs->stack = v;
    }
    fs->stack[fs->nstack++] = node;
    collect_children(c, with, punits, st, object_bbox, rect, &f->prims, &f->nprims);
    fs->nstack--;
    if (f->nprims == 0 || st_id(node)[0] == '\0') {
        return -1;
    }
    f->rect = rect;
    {
        void *v = fs->cache;
        if (!sv_grow(c->a, &v, &fs->capcache, fs->ncache + 1, sizeof(fcache))) {
            c->nomem = true;
            return -1;
        }
        fs->cache = v;
        fs->cache[fs->ncache].node = node;
        fs->cache[fs->ncache].filter = f;
        fs->ncache++;
    }
    *out = f;
    return 1;
}

bool sv_convert_filters(sv_conv *c, const st_node *n, const sv_cstate *st, const sv_rect *object_bbox,
                        sv_filter ***out, size_t *nout)
{
    const char *v = st_attr(n, A_FILTER);
    sv_stream s;
    sv_filter **list = NULL;
    size_t cnt = 0;
    size_t cap = 0;
    bool invalid_urls = false;
    size_t i;
    *out = NULL;
    *nout = 0;
    if (v == NULL) {
        return true;
    }
    ss_init(&s, v);
    for (;;) {
        sv_ffunc fv;
        int r = sv_ffunc_next(&s, &fv);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            return true; /* a malformed list: no filters, the element stays */
        }
        if (fv.kind == FV_URL) {
            const st_node *link = st_by_id(c->doc, fv.url, fv.url_len);
            sv_filter *f = NULL;
            if (link == NULL || convert_url(c, link, st, object_bbox, &f) < 0) {
                invalid_urls = true;
                if (c->nomem) {
                    return false;
                }
                continue;
            }
            {
                void *pv = list;
                if (!sv_grow(c->a, &pv, &cap, cnt + 1, sizeof(sv_filter *))) {
                    c->nomem = true;
                    return false;
                }
                list = pv;
            }
            list[cnt++] = f;
        } else if (!sv_ffunc_filter(c, n, st, &fv, object_bbox, &list, &cnt, &cap)) {
            return false;
        }
    }
    if (cnt == 0 && invalid_urls) {
        return false;
    }
    for (i = 0; i < cnt; i++) {
        list[i]->refs++;
    }
    *out = list;
    *nout = cnt;
    return true;
}

/* ---------------------------------------------------------------- paint servers, recursion */

void sv_filters_update_paint_servers(sv_conv *c, sv_unode *g, sv_ts ctx_ts, const sv_rect *ctx_bbox)
{
    size_t i;
    size_t k;
    for (i = 0; i < g->nfilters; i++) {
        sv_filter *f = g->filters[i];
        if (f->refs != 1) {
            continue; /* Arc::get_mut fails on a shared filter */
        }
        for (k = 0; k < f->nprims; k++) {
            if (f->prims[k].kind == FK_IMAGE && f->prims[k].root != NULL) {
                sv_update_paint_servers(c, f->prims[k].root, ctx_ts, ctx_bbox);
            }
        }
    }
}
