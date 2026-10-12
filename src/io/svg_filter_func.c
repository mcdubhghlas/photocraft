/* SVG filter functions in a `filter` value (port of svgtypes 0.15's filter_functions.rs, usvg's
 * convert_*_function and create_base_filter_func) and svgtree's fix_recursive_fe_image. */
#include <math.h>
#include <string.h>

#include "base/num.h"
#include "io/svg_filter.h"

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

/* ---------------------------------------------------------------- filter functions */

static bool starts_with(const sv_stream *s, const char *t)
{
    size_t n = strlen(t);
    return s->len - s->pos >= n && memcmp(s->s + s->pos, t, n) == 0;
}

static bool consume_byte(sv_stream *s, char ch)
{
    if (ss_at_end(s) || s->s[s->pos] != ch) {
        return false;
    }
    s->pos++;
    return true;
}

static bool is_ident(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' ||
           ch == '_';
}

/* parse_filter_length (positive: also no negative numbers). */
static bool filter_length(sv_stream *s, bool positive, sv_length *out)
{
    sv_length l;
    if (!ss_parse_length(s, &l)) {
        return false;
    }
    if ((positive && signbit(l.number)) || l.unit == U_PERCENT) {
        return false;
    }
    *out = l;
    return true;
}

/* try_parse_color */
static bool try_color(sv_stream *s, sv_color *out)
{
    sv_stream t = *s;
    if (ss_parse_color(&t, out)) {
        *s = t;
        return true;
    }
    return false;
}

static bool generic_amount(sv_stream *s, double *out)
{
    double v;
    if (!ss_at_end(s) && s->s[s->pos] == ')') {
        *out = 1.0;
        return true;
    }
    ss_skip_spaces(s);
    if (!ss_parse_number(s, &v)) {
        return false;
    }
    if (starts_with(s, "%")) {
        s->pos++;
        v /= 100.0;
    }
    if (signbit(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_angle(sv_stream *s, double *deg)
{
    double n;
    ss_skip_spaces(s);
    if (!ss_parse_number(s, &n)) {
        return false;
    }
    if (starts_with(s, "deg")) {
        s->pos += 3;
        *deg = n;
    } else if (starts_with(s, "grad")) {
        s->pos += 4;
        *deg = n * 180.0 / 200.0;
    } else if (starts_with(s, "rad")) {
        s->pos += 3;
        *deg = n * (180.0 / 3.14159265358979323846);
    } else if (starts_with(s, "turn")) {
        s->pos += 4;
        *deg = n * 360.0;
    } else if (n == 0.0) {
        *deg = n;
    } else {
        return false;
    }
    return true;
}

/* FilterValueListParser::parse_next */
static bool parse_next(sv_stream *s, sv_ffunc *v)
{
    size_t start = s->pos;
    size_t nlen;
    const char *name = s->s + start;
    memset(v, 0, sizeof *v);
    while (!ss_at_end(s) && is_ident(s->s[s->pos])) {
        s->pos++;
    }
    nlen = s->pos - start;
    ss_skip_spaces(s);
    if (!consume_byte(s, '(')) {
        return false;
    }
    ss_skip_spaces(s);
#define NAME_IS(lit) (nlen == sizeof(lit) - 1 && memcmp(name, lit, nlen) == 0)
    if (NAME_IS("blur")) {
        v->kind = FV_BLUR;
        v->std_dev = sv_len_px(0.0);
        if (!(!ss_at_end(s) && s->s[s->pos] == ')') && !filter_length(s, true, &v->std_dev)) {
            return false;
        }
    } else if (NAME_IS("drop-shadow")) {
        bool cur_color = false;
        sv_length l;
        v->kind = FV_DROP_SHADOW;
        if (!ss_at_end(s) && s->s[s->pos] == ')') {
            return false;
        }
        if (try_color(s, &v->color)) {
            v->has_color = true;
            ss_skip_spaces(s);
        } else if (starts_with(s, "currentColor")) {
            cur_color = true;
            s->pos += 12;
            ss_skip_spaces(s);
        }
        if (!filter_length(s, false, &v->dx)) {
            return false;
        }
        ss_skip_spaces(s);
        if (!filter_length(s, false, &v->dy)) {
            return false;
        }
        ss_skip_spaces(s);
        v->std_dev = sv_len_px(0.0);
        if (filter_length(s, true, &l)) {
            v->std_dev = l;
            ss_skip_spaces(s);
        }
        if (!v->has_color && !cur_color) {
            if (try_color(s, &v->color)) {
                v->has_color = true;
                ss_skip_spaces(s);
            } else if (starts_with(s, "currentColor")) {
                s->pos += 12;
            }
        }
    } else if (NAME_IS("hue-rotate")) {
        v->kind = FV_HUE_ROTATE;
        if (!(!ss_at_end(s) && s->s[s->pos] == ')') && !parse_angle(s, &v->amount)) {
            return false;
        }
    } else if (NAME_IS("brightness") || NAME_IS("contrast") || NAME_IS("grayscale") || NAME_IS("invert") ||
               NAME_IS("opacity") || NAME_IS("saturate") || NAME_IS("sepia")) {
        v->kind = NAME_IS("brightness")  ? FV_BRIGHTNESS
                  : NAME_IS("contrast")  ? FV_CONTRAST
                  : NAME_IS("grayscale") ? FV_GRAYSCALE
                  : NAME_IS("invert")    ? FV_INVERT
                  : NAME_IS("opacity")   ? FV_OPACITY
                  : NAME_IS("saturate")  ? FV_SATURATE
                                         : FV_SEPIA;
        if (!generic_amount(s, &v->amount)) {
            return false;
        }
    } else if (NAME_IS("url")) {
        size_t l0;
        v->kind = FV_URL;
        if (!consume_byte(s, '#')) {
            return false;
        }
        l0 = s->pos;
        while (!ss_at_end(s) && s->s[s->pos] != ' ' && s->s[s->pos] != ')') {
            s->pos++;
        }
        if (s->pos == l0) {
            return false;
        }
        v->url = s->s + l0;
        v->url_len = s->pos - l0;
    } else {
        return false;
    }
#undef NAME_IS
    ss_skip_spaces(s);
    if (!consume_byte(s, ')')) {
        return false;
    }
    ss_skip_spaces(s);
    return true;
}

/* FilterValueListParser::next: 1 a value, 0 the end, -1 an error (the stream jumps to its end). */
int sv_ffunc_next(sv_stream *s, sv_ffunc *v)
{
    ss_skip_spaces(s);
    if (ss_at_end(s)) {
        return 0;
    }
    if (starts_with(s, "none")) {
        s->pos += 4;
        ss_skip_spaces(s);
        if (ss_at_end(s)) {
            return 0;
        }
        s->pos = s->len;
        return -1;
    }
    if (!parse_next(s, v)) {
        s->pos = s->len;
        return -1;
    }
    return 1;
}

static void set_matrix(sv_fprim *p, const float m[20])
{
    p->kind = FK_COLOR_MATRIX;
    p->cm = CM_MATRIX;
    memcpy(p->m, m, sizeof p->m);
}

/* The primitive of a filter function (convert_*_function). */
static void function_primitive(sv_conv *c, const st_node *n, const sv_cstate *st, const sv_ffunc *v,
                               sv_fprim *p)
{
    float am = (float)(v->amount < 1.0 ? v->amount : 1.0); /* amount.min(1.0) as f32 */
    float k = 1.0f - am;
    int i;
    p->in1.kind = FIN_SOURCE_GRAPHIC;
    for (i = 0; i < 4; i++) {
        p->func[i].kind = TF_IDENTITY;
    }
    switch (v->kind) {
    case FV_BLUR: {
        float sd = sv_convert_length(c, v->std_dev, n, A_DX, UNITS_USER, st);
        p->kind = FK_GAUSSIAN_BLUR;
        p->sdx = p->sdy = isfinite(sd) && sd >= 0.0f ? sd : 0.0f;
        break;
    }
    case FV_DROP_SHADOW: {
        float sd = sv_convert_length(c, v->std_dev, n, A_DX, UNITS_USER, st);
        sv_color col = v->has_color ? v->color : find_color(n);
        p->kind = FK_DROP_SHADOW;
        p->sdx = p->sdy = isfinite(sd) && sd >= 0.0f ? sd : 0.0f;
        p->r = col.r;
        p->g = col.g;
        p->b = col.b;
        p->opacity = (float)col.a / 255.0f;
        p->dx = sv_convert_length(c, v->dx, n, A_DX, UNITS_USER, st);
        p->dy = sv_convert_length(c, v->dy, n, A_DY, UNITS_USER, st);
        break;
    }
    case FV_GRAYSCALE: {
        const float m[20] = {0.2126f + 0.7874f * k,
                             0.7152f - 0.7152f * k,
                             0.0722f - 0.0722f * k,
                             0.0f,
                             0.0f,
                             0.2126f - 0.2126f * k,
                             0.7152f + 0.2848f * k,
                             0.0722f - 0.0722f * k,
                             0.0f,
                             0.0f,
                             0.2126f - 0.2126f * k,
                             0.7152f - 0.7152f * k,
                             0.0722f + 0.9278f * k,
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             1.0f,
                             0.0f};
        set_matrix(p, m);
        break;
    }
    case FV_SEPIA: {
        const float m[20] = {0.393f + 0.607f * k,
                             0.769f - 0.769f * k,
                             0.189f - 0.189f * k,
                             0.0f,
                             0.0f,
                             0.349f - 0.349f * k,
                             0.686f + 0.314f * k,
                             0.168f - 0.168f * k,
                             0.0f,
                             0.0f,
                             0.272f - 0.272f * k,
                             0.534f - 0.534f * k,
                             0.131f + 0.869f * k,
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             0.0f,
                             1.0f,
                             0.0f};
        set_matrix(p, m);
        break;
    }
    case FV_SATURATE: {
        float a = pc_f64_to_f32(v->amount);
        p->kind = FK_COLOR_MATRIX;
        p->cm = CM_SATURATE;
        p->cm_value = isfinite(a) && a >= 0.0f ? a : 0.0f;
        break;
    }
    case FV_HUE_ROTATE:
        p->kind = FK_COLOR_MATRIX;
        p->cm = CM_HUE_ROTATE;
        p->cm_value = pc_f64_to_f32(v->amount);
        break;
    case FV_INVERT:
    case FV_OPACITY: {
        float *t = sv_alloc_array(c->a, 2, sizeof(float));
        if (t == NULL) {
            c->nomem = true;
            return;
        }
        p->kind = FK_COMPONENT_TRANSFER;
        if (v->kind == FV_INVERT) {
            t[0] = am;
            t[1] = 1.0f - am;
            for (i = 0; i < 3; i++) {
                p->func[i].kind = TF_TABLE;
                p->func[i].v = t;
                p->func[i].n = 2;
            }
        } else {
            t[0] = 0.0f;
            t[1] = am;
            p->func[3].kind = TF_TABLE;
            p->func[3].v = t;
            p->func[3].n = 2;
        }
        break;
    }
    case FV_BRIGHTNESS:
    case FV_CONTRAST: {
        float a = pc_f64_to_f32(v->amount);
        p->kind = FK_COMPONENT_TRANSFER;
        for (i = 0; i < 3; i++) {
            p->func[i].kind = TF_LINEAR;
            p->func[i].slope = a;
            p->func[i].intercept = v->kind == FV_BRIGHTNESS ? 0.0f : -(0.5f * a) + 0.5f;
        }
        break;
    }
    case FV_URL:
    default:
        break;
    }
}

/* create_base_filter_func: false when out of memory. */
bool sv_ffunc_filter(sv_conv *c, const st_node *n, const sv_cstate *st, const sv_ffunc *v,
                     const sv_rect *object_bbox, sv_filter ***list, size_t *cnt, size_t *cap)
{
    sv_rect rect;
    sv_filter *f;
    void *pv = *list;
    if (v->kind == FV_DROP_SHADOW || v->kind == FV_BLUR) {
        (void)nzrect_from_xywh(-0.5f, -0.5f, 2.0f, 2.0f, &rect);
    } else {
        (void)nzrect_from_xywh(-0.1f, -0.1f, 1.2f, 1.2f, &rect);
    }
    if (object_bbox == NULL) {
        return true; /* "an invalid region": skipped */
    }
    rect = rect_bbox_transform(rect, *object_bbox);
    f = sv_alloc(c->a, sizeof *f);
    if (f != NULL) {
        f->prims = sv_alloc(c->a, sizeof(sv_fprim));
    }
    if (f == NULL || f->prims == NULL || !sv_grow(c->a, &pv, cap, *cnt + 1, sizeof(sv_filter *))) {
        c->nomem = true;
        return false;
    }
    *list = pv;
    f->rect = rect;
    f->nprims = 1;
    f->prims[0].rect = rect;
    f->prims[0].linear = false; /* filter functions use sRGB */
    f->prims[0].result = "result";
    function_primitive(c, n, st, v, &f->prims[0]);
    (*list)[(*cnt)++] = f;
    return !c->nomem;
}

/* ---------------------------------------------------------------- recursion */

static st_node *next_desc(const st_node *root, st_node *n)
{
    if (n->first != NULL) {
        return n->first;
    }
    while (n != NULL && n != root && n->next == NULL) {
        n = n->parent;
    }
    if (n == NULL || n == root) {
        return NULL;
    }
    return n->next;
}

/* fix_recursive_fe_image: an feImage linking to an element whose filter list names the
 * feImage's own filter cuts that element's filter to "none". */
void sv_fix_recursive_fe_image(st_doc *d)
{
    st_node *fe;
    for (fe = d->root->first; fe != NULL; fe = next_desc(d->root, fe)) {
        st_node *link;
        const char *fid;
        const char *uri;
        sv_stream s;
        sv_ffunc fv;
        if (fe->eid != E_FE_IMAGE) {
            continue;
        }
        link = st_node_attr(d, fe, A_HREF);
        if (link == NULL || (uri = st_attr(link, A_FILTER)) == NULL) {
            continue;
        }
        fid = fe->parent != NULL ? st_id(fe->parent) : "";
        ss_init(&s, uri);
        while (sv_ffunc_next(&s, &fv) > 0) {
            if (fv.kind == FV_URL && strlen(fid) == fv.url_len && memcmp(fid, fv.url, fv.url_len) == 0) {
                size_t i;
                for (i = 0; i < link->nattrs; i++) {
                    if (link->attrs[i].aid == A_FILTER) {
                        link->attrs[i].value = "none";
                    }
                }
                break;
            }
        }
    }
}
