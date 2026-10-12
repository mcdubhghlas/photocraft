/* SVG filter effects (port of resvg 0.45's filter/mod.rs): every filter of a group runs on the
 * group's layer in order; each primitive's result is kept (by index: usvg's `in` names are
 * resolved to the producing primitive at conversion) and the last one replaces the layer.
 *
 * Images keep resvg's sizes exactly: SourceGraphic is the whole layer, most primitives make an
 * image the size of the filter region, and drawing one onto another clips. Colour spaces convert
 * through resvg's 8-bit sRGB <-> linearRGB tables on demultiplied pixels.
 *
 * Every buffer is a scratch pixmap of the render (sv_rctx_pixmap), so the render's pixel budget
 * bounds hostile filters: a buffer that cannot be had fails the filter, which (as resvg does on
 * an error) leaves the layer transparent. Where resvg would panic (images of different sizes in
 * lighting, arithmetic composite or displacement maps; regions that overflow i32) the filter
 * fails the same way. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "io/svg_filter.h"

/* ---------------------------------------------------------------- colour tables */

static const uint8_t SRGB_TO_LINEAR[256] = {
    0,   0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   1,   2,   2,
    2,   2,   2,   2,   2,   2,   3,   3,   3,   3,   3,   3,   4,   4,   4,   4,   4,   5,   5,   5,
    5,   6,   6,   6,   6,   7,   7,   7,   8,   8,   8,   8,   9,   9,   9,   10,  10,  10,  11,  11,
    12,  12,  12,  13,  13,  13,  14,  14,  15,  15,  16,  16,  17,  17,  17,  18,  18,  19,  19,  20,
    20,  21,  22,  22,  23,  23,  24,  24,  25,  25,  26,  27,  27,  28,  29,  29,  30,  30,  31,  32,
    32,  33,  34,  35,  35,  36,  37,  37,  38,  39,  40,  41,  41,  42,  43,  44,  45,  45,  46,  47,
    48,  49,  50,  51,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,
    67,  68,  69,  70,  71,  72,  73,  74,  76,  77,  78,  79,  80,  81,  82,  84,  85,  86,  87,  88,
    90,  91,  92,  93,  95,  96,  97,  99,  100, 101, 103, 104, 105, 107, 108, 109, 111, 112, 114, 115,
    116, 118, 119, 121, 122, 124, 125, 127, 128, 130, 131, 133, 134, 136, 138, 139, 141, 142, 144, 146,
    147, 149, 151, 152, 154, 156, 157, 159, 161, 163, 164, 166, 168, 170, 171, 173, 175, 177, 179, 181,
    183, 184, 186, 188, 190, 192, 194, 196, 198, 200, 202, 204, 206, 208, 210, 212, 214, 216, 218, 220,
    222, 224, 226, 229, 231, 233, 235, 237, 239, 242, 244, 246, 248, 250, 253, 255,
};

static const uint8_t LINEAR_TO_SRGB[256] = {
    0,   13,  22,  28,  34,  38,  42,  46,  50,  53,  56,  59,  61,  64,  66,  69,  71,  73,  75,  77,
    79,  81,  83,  85,  86,  88,  90,  92,  93,  95,  96,  98,  99,  101, 102, 104, 105, 106, 108, 109,
    110, 112, 113, 114, 115, 117, 118, 119, 120, 121, 122, 124, 125, 126, 127, 128, 129, 130, 131, 132,
    133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 148, 149, 150, 151,
    152, 153, 154, 155, 155, 156, 157, 158, 159, 159, 160, 161, 162, 163, 163, 164, 165, 166, 167, 167,
    168, 169, 170, 170, 171, 172, 173, 173, 174, 175, 175, 176, 177, 178, 178, 179, 180, 180, 181, 182,
    182, 183, 184, 185, 185, 186, 187, 187, 188, 189, 189, 190, 190, 191, 192, 192, 193, 194, 194, 195,
    196, 196, 197, 197, 198, 199, 199, 200, 200, 201, 202, 202, 203, 203, 204, 205, 205, 206, 206, 207,
    208, 208, 209, 209, 210, 210, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216, 216, 217, 218, 218,
    219, 219, 220, 220, 221, 221, 222, 222, 223, 223, 224, 224, 225, 226, 226, 227, 227, 228, 228, 229,
    229, 230, 230, 231, 231, 232, 232, 233, 233, 234, 234, 235, 235, 236, 236, 237, 237, 238, 238, 238,
    239, 239, 240, 240, 241, 241, 242, 242, 243, 243, 244, 244, 245, 245, 246, 246, 246, 247, 247, 248,
    248, 249, 249, 250, 250, 251, 251, 251, 252, 252, 253, 253, 254, 254, 255, 255,
};

/* multiply_alpha / demultiply_alpha (f32 like resvg; a zero alpha divides to NaN or inf, which
 * the saturating cast turns into 0 or 255 as Rust's `as u8` does) */
static void multiply_alpha(uint8_t *px, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t *p = px + i * 4;
        float a = (float)p[3] / 255.0f;
        p[2] = pc_f32_to_u8((float)p[2] * a + 0.5f);
        p[1] = pc_f32_to_u8((float)p[1] * a + 0.5f);
        p[0] = pc_f32_to_u8((float)p[0] * a + 0.5f);
    }
}

static void demultiply_alpha(uint8_t *px, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t *p = px + i * 4;
        float a = (float)p[3] / 255.0f;
        p[2] = pc_f32_to_u8((float)p[2] / a + 0.5f);
        p[1] = pc_f32_to_u8((float)p[1] / a + 0.5f);
        p[0] = pc_f32_to_u8((float)p[0] / a + 0.5f);
    }
}

static void convert_space(sv_pixmap *pm, bool to_linear)
{
    const uint8_t *t = to_linear ? SRGB_TO_LINEAR : LINEAR_TO_SRGB;
    size_t n = (size_t)pm->w * pm->h;
    size_t i;
    demultiply_alpha(pm->px, n);
    for (i = 0; i < n; i++) {
        uint8_t *p = pm->px + i * 4;
        p[0] = t[p[0]];
        p[1] = t[p[1]];
        p[2] = t[p[2]];
    }
    multiply_alpha(pm->px, n);
}

/* ---------------------------------------------------------------- int rects */

typedef struct irect {
    int64_t x, y, w, h;
} irect;

/* IntRect::from_xywh */
static bool irect_make(int64_t x, int64_t y, int64_t w, int64_t h, irect *out)
{
    if (w <= 0 || h <= 0 || w > INT32_MAX || h > INT32_MAX || x < INT32_MIN || y < INT32_MIN ||
        x + w > INT32_MAX || y + h > INT32_MAX) {
        return false;
    }
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    return true;
}

/* rect.transform(ts).to_int_rect() (NonZeroRect) */
static bool to_int_rect(sv_rect r, sv_ts ts, irect *out)
{
    sv_rect t;
    int64_t w;
    int64_t h;
    if (!nzrect_transform(r, ts, &t)) {
        return false;
    }
    w = (int64_t)pc_f32_to_u32(ceilf(rect_w(t)));
    h = (int64_t)pc_f32_to_u32(ceilf(rect_h(t)));
    return irect_make(pc_f32_to_i32(floorf(t.l)), pc_f32_to_i32(floorf(t.t)), w > 1 ? w : 1, h > 1 ? h : 1,
                      out);
}

static bool irect_eq(irect a, irect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

/* ---------------------------------------------------------------- images */

typedef struct fimage {
    sv_pixmap pm;
    bool owned; /* else borrowed from a result */
    irect region;
    bool linear;
} fimage;

typedef struct fctx {
    sv_rctx *r;
    const sv_pixmap *source;
    irect region;
    sv_ts ts;
    fimage *results;
    size_t nres;
} fctx;

static bool img_alloc(fctx *f, int64_t w, int64_t h, fimage *out)
{
    memset(out, 0, sizeof *out);
    if (w <= 0 || h <= 0 || w > UINT32_MAX || h > UINT32_MAX ||
        !sv_rctx_pixmap(f->r, (uint32_t)w, (uint32_t)h, &out->pm)) {
        return false;
    }
    out->owned = true;
    (void)irect_make(0, 0, w, h, &out->region);
    return true;
}

static void img_release(fctx *f, fimage *im)
{
    if (im->owned && im->pm.px != NULL) {
        sv_rctx_release(f->r, &im->pm);
    }
    memset(im, 0, sizeof *im);
}

/* Image::take: an owned pixmap (a copy of a borrowed one). */
static bool img_take(fctx *f, fimage *im)
{
    sv_pixmap copy;
    if (im->owned) {
        return true;
    }
    if (!sv_rctx_pixmap(f->r, im->pm.w, im->pm.h, &copy)) {
        return false;
    }
    memcpy(copy.px, im->pm.px, (size_t)im->pm.w * im->pm.h * 4);
    im->pm = copy;
    im->owned = true;
    return true;
}

/* Image::into_color_space */
static bool img_space(fctx *f, fimage *im, bool linear)
{
    if (im->linear == linear) {
        return true;
    }
    if (!img_take(f, im)) {
        return false;
    }
    convert_space(&im->pm, linear);
    im->linear = linear;
    return true;
}

/* Image::from_image */
static void from_image(fimage *im, bool linear)
{
    (void)irect_make(0, 0, im->pm.w, im->pm.h, &im->region);
    im->linear = linear;
}

static bool source_copy(fctx *f, fimage *out)
{
    if (!img_alloc(f, f->source->w, f->source->h, out)) {
        return false;
    }
    memcpy(out->pm.px, f->source->px, (size_t)f->source->w * f->source->h * 4);
    out->region = f->region;
    out->linear = false;
    return true;
}

/* get_input */
static bool get_input(fctx *f, const sv_fin *in, fimage *out)
{
    if (in->kind == FIN_REF && in->ref < f->nres) {
        *out = f->results[in->ref];
        out->owned = false;
        return true;
    }
    if (!source_copy(f, out)) {
        return false;
    }
    if (in->kind == FIN_SOURCE_ALPHA) {
        size_t n = (size_t)out->pm.w * out->pm.h;
        size_t i;
        for (i = 0; i < n; i++) {
            memset(out->pm.px + i * 4, 0, 3);
        }
    }
    return true;
}

/* Pixmap::fill with Color::from_rgba8(r, g, b, a) */
static void fill_color(sv_pixmap *pm, uint8_t r, uint8_t g, uint8_t b, uint8_t a8)
{
    float a = (float)a8 / 255.0f;
    float c[3];
    uint8_t px[4];
    size_t n = (size_t)pm->w * pm->h;
    size_t i;
    c[0] = (float)r / 255.0f;
    c[1] = (float)g / 255.0f;
    c[2] = (float)b / 255.0f;
    for (i = 0; i < 3; i++) {
        float v = a == 1.0f ? c[i] : pc_clampf(c[i] * a, 0.0f, 1.0f);
        px[i] = pc_f32_to_u8(v * 255.0f + 0.5f);
    }
    px[3] = pc_f32_to_u8(a * 255.0f + 0.5f);
    for (i = 0; i < n; i++) {
        memcpy(pm->px + i * 4, px, 4);
    }
}

static void draw(sv_pixmap *dst, int32_t x, int32_t y, const sv_pixmap *src, sv_tsblend m)
{
    sv_draw_pixmap(dst, x, y, src, 1.0f, m);
}

static sv_tsblend ts_blend_of(sv_blend b)
{
    static const sv_tsblend M[] = {TSB_SOURCE_OVER, TSB_MULTIPLY,   TSB_SCREEN,      TSB_OVERLAY,
                                   TSB_DARKEN,      TSB_LIGHTEN,    TSB_COLOR_DODGE, TSB_COLOR_BURN,
                                   TSB_HARD_LIGHT,  TSB_SOFT_LIGHT, TSB_DIFFERENCE,  TSB_EXCLUSION,
                                   TSB_HUE,         TSB_SATURATION, TSB_COLOR,       TSB_LUMINOSITY};
    return (size_t)b < sizeof M / sizeof M[0] ? M[b] : TSB_SOURCE_OVER;
}

/* scale_coordinates */
static void scale_xy(float x, float y, sv_ts ts, float *ox, float *oy)
{
    float sx;
    float sy;
    ts_get_scale(ts, &sx, &sy);
    *ox = x * sx;
    *oy = y * sy;
}

/* resolve_std_dev: false = no blur. */
static bool resolve_std_dev(float sdx, float sdy, sv_ts ts, double *ox, double *oy, bool *box)
{
    float x;
    float y;
    scale_xy(sdx, sdy, ts, &x, &y);
    if (sv_approx_eq_ulps(x, 0.0f, 4) && sv_approx_eq_ulps(y, 0.0f, 4)) {
        return false;
    }
    if (x < 0.05f) {
        x = 0.0f;
    }
    if (y < 0.05f) {
        y = 0.0f;
    }
    *box = x >= 2.0f || y >= 2.0f;
    *ox = (double)x;
    *oy = (double)y;
    return true;
}

static bool blur(double sx, double sy, bool box, sv_pixmap *pm)
{
    return box ? sv_fx_box_blur(sx, sy, pm->px, pm->w, pm->h) : sv_fx_iir_blur(sx, sy, pm->px, pm->w, pm->h);
}

/* ---------------------------------------------------------------- primitives */

static bool apply_drop_shadow(fctx *f, const sv_fprim *fe, bool cs, fimage *input, fimage *out)
{
    float dx;
    float dy;
    double sx;
    double sy;
    bool box;
    fimage shadow;
    size_t n;
    size_t i;
    float base[4];
    scale_xy(fe->dx, fe->dy, f->ts, &dx, &dy);
    if (!img_alloc(f, input->pm.w, input->pm.h, out)) {
        return false;
    }
    if (!img_space(f, input, cs) || !img_take(f, input)) {
        return false;
    }
    shadow = *input;
    shadow.owned = false;
    if (!img_take(f, &shadow)) {
        return false;
    }
    if (resolve_std_dev(fe->sdx, fe->sdy, f->ts, &sx, &sy, &box) && !blur(sx, sy, box, &shadow.pm)) {
        img_release(f, &shadow);
        return false;
    }
    base[0] = (float)fe->r / 255.0f;
    base[1] = (float)fe->g / 255.0f;
    base[2] = (float)fe->b / 255.0f;
    base[3] = (float)sv_opacity_u8(fe->opacity) / 255.0f;
    n = (size_t)shadow.pm.w * shadow.pm.h;
    for (i = 0; i < n; i++) {
        uint8_t *p = shadow.pm.px + i * 4;
        float o = pc_clampf((float)p[3] / 255.0f, 0.0f, 1.0f);
        float a = base[3] * o;
        int k;
        a = isfinite(a) ? pc_clampf(a, 0.0f, 1.0f) : 0.0f;
        for (k = 0; k < 3; k++) {
            float v = a == 1.0f ? base[k] : pc_clampf(base[k] * a, 0.0f, 1.0f);
            p[k] = pc_f32_to_u8(v * 255.0f + 0.5f);
        }
        p[3] = pc_f32_to_u8(a * 255.0f + 0.5f);
    }
    /* resvg converts the flood colour as if it were in the other space (both ways) */
    convert_space(&shadow.pm, cs);
    draw(&out->pm, pc_f32_to_i32(dx), pc_f32_to_i32(dy), &shadow.pm, TSB_SOURCE_OVER);
    draw(&out->pm, 0, 0, &input->pm, TSB_SOURCE_OVER);
    img_release(f, &shadow);
    from_image(out, cs);
    return true;
}

static bool apply_blur(fctx *f, const sv_fprim *fe, bool cs, fimage *input, fimage *out)
{
    double sx;
    double sy;
    bool box;
    if (!resolve_std_dev(fe->sdx, fe->sdy, f->ts, &sx, &sy, &box)) {
        *out = *input;
        memset(input, 0, sizeof *input);
        return true;
    }
    if (!img_space(f, input, cs) || !img_take(f, input) || !blur(sx, sy, box, &input->pm)) {
        return false;
    }
    *out = *input;
    memset(input, 0, sizeof *input);
    from_image(out, cs);
    return true;
}

static bool apply_offset(fctx *f, const sv_fprim *fe, fimage *input, fimage *out)
{
    float dx;
    float dy;
    scale_xy(fe->dx, fe->dy, f->ts, &dx, &dy);
    if (sv_approx_eq_ulps(dx, 0.0f, 4) && sv_approx_eq_ulps(dy, 0.0f, 4)) {
        *out = *input;
        memset(input, 0, sizeof *input);
        return true;
    }
    if (!img_alloc(f, input->pm.w, input->pm.h, out)) {
        return false;
    }
    draw(&out->pm, pc_f32_to_i32(dx), pc_f32_to_i32(dy), &input->pm, TSB_SOURCE_OVER);
    from_image(out, input->linear);
    return true;
}

static bool apply_blend_composite(fctx *f, const sv_fprim *fe, bool cs, fimage *in1, fimage *in2, fimage *out)
{
    sv_tsblend m;
    if (!img_space(f, in1, cs) || !img_space(f, in2, cs) || !img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    if (fe->kind == FK_COMPOSITE && fe->op == CO_ARITHMETIC) {
        /* composite::arithmetic asserts that the three images have one size */
        if (in1->pm.w != in2->pm.w || in1->pm.w != out->pm.w || in1->pm.h != in2->pm.h ||
            in1->pm.h != out->pm.h) {
            return false;
        }
        sv_fx_arithmetic(fe->k1, fe->k2, fe->k3, fe->k4, in1->pm.px, in2->pm.px, out->pm.px,
                         (size_t)out->pm.w * out->pm.h);
        from_image(out, cs);
        return true;
    }
    draw(&out->pm, 0, 0, &in2->pm, TSB_SOURCE_OVER);
    if (fe->kind == FK_BLEND) {
        m = ts_blend_of(fe->mode);
    } else {
        m = fe->op == CO_IN     ? TSB_SOURCE_IN
            : fe->op == CO_OUT  ? TSB_SOURCE_OUT
            : fe->op == CO_ATOP ? TSB_SOURCE_ATOP
            : fe->op == CO_XOR  ? TSB_XOR
                                : TSB_SOURCE_OVER;
    }
    draw(&out->pm, 0, 0, &in1->pm, m);
    from_image(out, cs);
    return true;
}

static bool apply_merge(fctx *f, const sv_fprim *fe, bool cs, fimage *out)
{
    size_t i;
    if (!img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    for (i = 0; i < fe->ninputs; i++) {
        fimage in;
        if (!get_input(f, &fe->inputs[i], &in)) {
            return false;
        }
        if (!img_space(f, &in, cs)) {
            img_release(f, &in);
            return false;
        }
        draw(&out->pm, 0, 0, &in.pm, TSB_SOURCE_OVER);
        img_release(f, &in);
    }
    from_image(out, cs);
    return true;
}

static bool apply_flood(fctx *f, const sv_fprim *fe, fimage *out)
{
    if (!img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    fill_color(&out->pm, fe->r, fe->g, fe->b, sv_opacity_u8(fe->opacity));
    from_image(out, false);
    return true;
}

static bool apply_tile(fctx *f, fimage *input, fimage *out)
{
    irect sub;
    int64_t l;
    int64_t t;
    int64_t r;
    int64_t b;
    sv_pixmap tile;
    sv_shader s;
    uint32_t y;
    if (!irect_make(input->region.x - f->region.x, input->region.y - f->region.y, input->region.w,
                    input->region.h, &sub)) {
        return false;
    }
    /* copy_region: Pixmap::clone_rect of the part inside the image */
    l = sub.x > 0 ? sub.x : 0;
    t = sub.y > 0 ? sub.y : 0;
    r = sub.x + sub.w < (int64_t)input->pm.w ? sub.x + sub.w : (int64_t)input->pm.w;
    b = sub.y + sub.h < (int64_t)input->pm.h ? sub.y + sub.h : (int64_t)input->pm.h;
    if (r <= l || b <= t || !sv_rctx_pixmap(f->r, (uint32_t)(r - l), (uint32_t)(b - t), &tile)) {
        return false;
    }
    for (y = 0; y < tile.h; y++) {
        memcpy(tile.px + (size_t)y * tile.w * 4,
               input->pm.px + ((size_t)(t + y) * input->pm.w + (size_t)l) * 4, (size_t)tile.w * 4);
    }
    if (!img_alloc(f, f->region.w, f->region.h, out)) {
        sv_rctx_release(f->r, &tile);
        return false;
    }
    if (sv_shader_pattern(&s, &tile, true, SQ_BICUBIC, 1.0f, ts_translate((float)sub.x, (float)sub.y)) == 1) {
        sv_blit_rect(&out->pm, 0, 0, out->pm.w, out->pm.h, &s, TSB_SOURCE_OVER);
        sv_shader_fini(&s);
    }
    sv_rctx_release(f->r, &tile);
    from_image(out, false);
    return true;
}

static bool apply_image(fctx *f, const sv_fprim *fe, irect sub, fimage *out)
{
    float sx;
    float sy;
    int32_t save[4];
    sv_rctx *r = f->r;
    if (!img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    ts_get_scale(f->ts, &sx, &sy);
    save[0] = r->max_l;
    save[1] = r->max_t;
    save[2] = r->max_r;
    save[3] = r->max_b;
    r->max_l = 0;
    r->max_t = 0;
    r->max_r = (int32_t)f->region.w;
    r->max_b = (int32_t)f->region.h;
    if (fe->root != NULL) {
        sv_render_children(r, fe->root, ts_row(sx, 0.0f, 0.0f, sy, (float)sub.x, (float)sub.y), &out->pm);
    }
    r->max_l = save[0];
    r->max_t = save[1];
    r->max_r = save[2];
    r->max_b = save[3];
    from_image(out, false);
    return !r->nomem;
}

/* component transfer, colour matrix, convolve matrix, morphology: in place on the input */
static bool apply_pixelwise(fctx *f, const sv_fprim *fe, bool cs, fimage *input, fimage *out)
{
    size_t n;
    if (!img_space(f, input, cs) || !img_take(f, input)) {
        return false;
    }
    n = (size_t)input->pm.w * input->pm.h;
    switch (fe->kind) {
    case FK_COMPONENT_TRANSFER:
        demultiply_alpha(input->pm.px, n);
        sv_fx_component_transfer(fe, input->pm.px, n);
        multiply_alpha(input->pm.px, n);
        break;
    case FK_COLOR_MATRIX:
        demultiply_alpha(input->pm.px, n);
        sv_fx_color_matrix(fe, input->pm.px, n);
        multiply_alpha(input->pm.px, n);
        break;
    case FK_CONVOLVE_MATRIX:
        if (fe->preserve_alpha) {
            demultiply_alpha(input->pm.px, n);
        }
        if (!sv_fx_convolve(fe, input->pm.px, input->pm.w, input->pm.h)) {
            return false;
        }
        break;
    case FK_MORPHOLOGY: {
        float rx;
        float ry;
        scale_xy(fe->rx, fe->ry, f->ts, &rx, &ry);
        if (!(rx > 0.0f && ry > 0.0f)) {
            memset(input->pm.px, 0, n * 4);
        } else if (!sv_fx_morphology(fe->dilate, rx, ry, input->pm.px, input->pm.w, input->pm.h)) {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    *out = *input;
    memset(input, 0, sizeof *input);
    from_image(out, cs);
    return true;
}

static bool apply_displacement(fctx *f, const sv_fprim *fe, bool cs, fimage *in1, fimage *in2, fimage *out)
{
    float sx;
    float sy;
    if (!img_space(f, in1, cs) || !img_take(f, in1) || !img_space(f, in2, cs) || !img_take(f, in2) ||
        !img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    scale_xy(fe->scale, fe->scale, f->ts, &sx, &sy);
    /* displacement_map::apply asserts one size for all three */
    if (in1->pm.w != in2->pm.w || in1->pm.w != out->pm.w || in1->pm.h != in2->pm.h ||
        in1->pm.h != out->pm.h) {
        return false;
    }
    sv_fx_displacement(fe, sx, sy, in1->pm.px, in2->pm.px, out->pm.px, out->pm.w, out->pm.h);
    from_image(out, cs);
    return true;
}

static bool apply_turbulence(fctx *f, const sv_fprim *fe, bool cs, fimage *out)
{
    float sx;
    float sy;
    if (!img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    from_image(out, cs);
    ts_get_scale(f->ts, &sx, &sy);
    if (sv_approx_eq_ulps(sx, 0.0f, 4) || sv_approx_eq_ulps(sy, 0.0f, 4)) {
        return true;
    }
    if (!sv_fx_turbulence(fe, (double)f->region.x - (double)f->ts.tx, (double)f->region.y - (double)f->ts.ty,
                          (double)sx, (double)sy, out->pm.px, out->pm.w, out->pm.h)) {
        return false;
    }
    multiply_alpha(out->pm.px, (size_t)out->pm.w * out->pm.h);
    return true;
}

/* transform_light_source (the spot light's y uses region.x(), as resvg does) */
static sv_light transform_light(sv_light l, irect region, sv_ts ts)
{
    const float SQRT_2 = 1.41421356237309504880f;
    if (l.kind == LIGHT_POINT) {
        float x = l.x;
        float y = l.y;
        ts_map_point(ts, &x, &y);
        l.x = x - (float)region.x;
        l.y = y - (float)region.y;
        l.z = l.z * sqrtf(ts.sx * ts.sx + ts.sy * ts.sy) / SQRT_2;
    } else if (l.kind == LIGHT_SPOT) {
        float sz = sqrtf(ts.sx * ts.sx + ts.sy * ts.sy) / SQRT_2;
        float x = l.x;
        float y = l.y;
        ts_map_point(ts, &x, &y);
        l.x = x - (float)region.x;
        l.y = y - (float)region.x;
        l.z *= sz;
        x = l.px;
        y = l.py;
        ts_map_point(ts, &x, &y);
        l.px = x - (float)region.x;
        l.py = y - (float)region.x;
        l.pz *= sz;
    }
    return l;
}

static bool apply_lighting(fctx *f, const sv_fprim *fe, bool cs, fimage *input, fimage *out)
{
    if (!img_alloc(f, f->region.w, f->region.h, out)) {
        return false;
    }
    /* lighting asserts that the input and the output have one size */
    if (input->pm.w != out->pm.w || input->pm.h != out->pm.h) {
        return false;
    }
    sv_fx_lighting(fe, transform_light(fe->light, f->region, f->ts), input->pm.px, out->pm.px, out->pm.w,
                   out->pm.h);
    from_image(out, cs);
    return true;
}

/* ---------------------------------------------------------------- filter */

/* Clears the pixels of pm in the rect (x, y, w, h) given as f32 (fill_rect with BlendMode::Clear;
 * a negative size is no rect). */
static void clear_rect(sv_pixmap *pm, float x, float y, float w, float h)
{
    float r = x + w;
    float b = y + h;
    double l;
    double t;
    double rr;
    double bb;
    int64_t y0;
    int64_t y1;
    int64_t x0;
    int64_t x1;
    int64_t j;
    if (!(w >= 0.0f && h >= 0.0f) || !isfinite(r) || !isfinite(b)) {
        return;
    }
    l = x < 0.0f ? 0.0 : (double)x;
    t = y < 0.0f ? 0.0 : (double)y;
    rr = (double)r > (double)pm->w ? (double)pm->w : (double)r;
    bb = (double)b > (double)pm->h ? (double)pm->h : (double)b;
    if (rr <= l || bb <= t) {
        return;
    }
    x0 = (int64_t)l;
    x1 = (int64_t)rr;
    y0 = (int64_t)t;
    y1 = (int64_t)bb;
    for (j = y0; j < y1; j++) {
        memset(pm->px + ((size_t)j * pm->w + (size_t)x0) * 4, 0, (size_t)(x1 - x0) * 4);
    }
}

static bool apply_primitive(fctx *f, const sv_fprim *fe, irect sub, fimage *out)
{
    bool cs = fe->linear;
    fimage in1;
    fimage in2;
    bool ok;
    memset(&in1, 0, sizeof in1);
    memset(&in2, 0, sizeof in2);
    memset(out, 0, sizeof *out);
    switch (fe->kind) {
    case FK_FLOOD:
        return apply_flood(f, fe, out);
    case FK_MERGE:
        return apply_merge(f, fe, cs, out);
    case FK_IMAGE:
        return apply_image(f, fe, sub, out);
    case FK_TURBULENCE:
        return apply_turbulence(f, fe, cs, out);
    default:
        break;
    }
    if (!get_input(f, &fe->in1, &in1)) {
        return false;
    }
    switch (fe->kind) {
    case FK_BLEND:
    case FK_COMPOSITE:
    case FK_DISPLACEMENT_MAP:
        if (!get_input(f, &fe->in2, &in2)) {
            img_release(f, &in1);
            return false;
        }
        ok = fe->kind == FK_DISPLACEMENT_MAP ? apply_displacement(f, fe, cs, &in1, &in2, out)
                                             : apply_blend_composite(f, fe, cs, &in1, &in2, out);
        break;
    case FK_DROP_SHADOW:
        ok = apply_drop_shadow(f, fe, cs, &in1, out);
        break;
    case FK_GAUSSIAN_BLUR:
        ok = apply_blur(f, fe, cs, &in1, out);
        break;
    case FK_OFFSET:
        ok = apply_offset(f, fe, &in1, out);
        break;
    case FK_TILE:
        ok = apply_tile(f, &in1, out);
        break;
    case FK_DIFFUSE_LIGHTING:
    case FK_SPECULAR_LIGHTING:
        ok = apply_lighting(f, fe, cs, &in1, out);
        break;
    default:
        ok = apply_pixelwise(f, fe, cs, &in1, out);
        break;
    }
    img_release(f, &in1);
    img_release(f, &in2);
    if (!ok) {
        img_release(f, out);
    }
    return ok;
}

static bool apply_inner(fctx *f, const sv_filter *flt, fimage *out)
{
    size_t i;
    if (!to_int_rect(flt->rect, f->ts, &f->region)) {
        return false;
    }
    for (i = 0; i < flt->nprims; i++) {
        const sv_fprim *fe = &flt->prims[i];
        irect sub;
        fimage res;
        if (!to_int_rect(fe->rect, f->ts, &sub)) {
            return false;
        }
        if (fe->kind == FK_OFFSET && fe->in1.kind == FIN_REF && fe->in1.ref < f->nres) {
            sub = f->results[fe->in1.ref].region;
        }
        if (!apply_primitive(f, fe, sub, &res)) {
            return false;
        }
        if (!irect_eq(f->region, sub)) {
            irect s2;
            float w;
            float h;
            bool ok = fe->kind == FK_OFFSET
                          ? irect_make(0, 0, f->region.w, f->region.h, &s2)
                          : irect_make(sub.x - f->region.x, sub.y - f->region.y, sub.w, sub.h, &s2);
            if (!ok || !img_take(f, &res)) {
                img_release(f, &res);
                return false;
            }
            w = (float)res.pm.w;
            h = (float)res.pm.h;
            clear_rect(&res.pm, 0.0f, 0.0f, w, (float)s2.y);
            clear_rect(&res.pm, 0.0f, 0.0f, (float)s2.x, h);
            clear_rect(&res.pm, (float)(s2.x + s2.w), 0.0f, w, h);
            clear_rect(&res.pm, 0.0f, (float)(s2.y + s2.h), w, h);
            res.region = sub;
        }
        f->results[f->nres++] = res;
    }
    if (f->nres == 0) {
        return false;
    }
    /* results.pop(): the last result leaves the list */
    *out = f->results[--f->nres];
    return true;
}

static void apply_filter(sv_rctx *r, const sv_filter *flt, sv_ts ts, sv_pixmap *pm)
{
    fctx f;
    fimage res;
    bool ok;
    size_t i;
    memset(&f, 0, sizeof f);
    f.r = r;
    f.source = pm;
    f.ts = ts;
    f.results = calloc(flt->nprims > 0 ? flt->nprims : 1, sizeof(fimage));
    if (f.results == NULL) {
        r->nomem = true;
        return;
    }
    memset(&res, 0, sizeof res);
    ok = apply_inner(&f, flt, &res);
    if (ok) {
        /* apply_to_canvas */
        ok = img_space(&f, &res, false);
    }
    memset(pm->px, 0, (size_t)pm->w * pm->h * 4);
    if (ok) {
        draw(pm, 0, 0, &res.pm, TSB_SOURCE_OVER);
    }
    /* Borrowed images point into owned results; release the owned ones once. */
    img_release(&f, &res);
    for (i = 0; i < f.nres; i++) {
        img_release(&f, &f.results[i]);
    }
    free(f.results);
}

void sv_filters_apply(sv_rctx *r, const sv_unode *g, sv_ts ts, sv_pixmap *pm)
{
    size_t i;
    for (i = 0; i < g->nfilters && !r->nomem; i++) {
        apply_filter(r, g->filters[i], ts, pm);
    }
}
