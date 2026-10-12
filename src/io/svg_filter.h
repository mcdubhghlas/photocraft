/* SVG filter effects: the data model (port of usvg 0.45's tree/filter.rs) shared by the
 * conversion (svg_filter_conv.c, usvg's parser/filter.rs) and the rendering (svg_filter.c,
 * svg_filter_fx.c, svg_filter_light.c: resvg 0.45's filter module).
 *
 * Everything here is allocated in the parse's arena (sv_arena) and immutable once the render
 * tree is built, except `refs`, which counts the groups sharing a filter (usvg's Arc strong
 * count: paint servers inside feImage content are only moved to user space when it is 1). */
#ifndef PC_IO_SVG_FILTER_H
#define PC_IO_SVG_FILTER_H

#include "io/svg_internal.h"

/* filter::Input */
typedef enum { FIN_SOURCE_GRAPHIC, FIN_SOURCE_ALPHA, FIN_REF } sv_finkind;

typedef struct sv_fin {
    sv_finkind kind;
    const char *name; /* FIN_REF: a primitive's result name */
    size_t ref;       /* FIN_REF: index of the latest earlier primitive with that result */
} sv_fin;

/* filter::Kind */
typedef enum {
    FK_BLEND,
    FK_COLOR_MATRIX,
    FK_COMPONENT_TRANSFER,
    FK_COMPOSITE,
    FK_CONVOLVE_MATRIX,
    FK_DIFFUSE_LIGHTING,
    FK_DISPLACEMENT_MAP,
    FK_DROP_SHADOW,
    FK_FLOOD,
    FK_GAUSSIAN_BLUR,
    FK_IMAGE,
    FK_MERGE,
    FK_MORPHOLOGY,
    FK_OFFSET,
    FK_SPECULAR_LIGHTING,
    FK_TILE,
    FK_TURBULENCE
} sv_fkind;

typedef enum { CM_MATRIX, CM_SATURATE, CM_HUE_ROTATE, CM_LUMINANCE_TO_ALPHA } sv_cmkind;
typedef enum { TF_IDENTITY, TF_TABLE, TF_DISCRETE, TF_LINEAR, TF_GAMMA } sv_tfkind;
typedef enum { CO_OVER, CO_IN, CO_OUT, CO_ATOP, CO_XOR, CO_ARITHMETIC } sv_coop;
typedef enum { EDGE_NONE, EDGE_DUPLICATE, EDGE_WRAP } sv_edge;
typedef enum { CH_R, CH_G, CH_B, CH_A } sv_channel;
typedef enum { LIGHT_DISTANT, LIGHT_POINT, LIGHT_SPOT } sv_lightkind;

/* filter::TransferFunction */
typedef struct sv_tfunc {
    sv_tfkind kind;
    const float *v; /* table / discrete values */
    size_t n;
    float slope, intercept;            /* linear */
    float amplitude, exponent, offset; /* gamma */
} sv_tfunc;

/* filter::LightSource (all three kinds in one) */
typedef struct sv_light {
    sv_lightkind kind;
    float azimuth, elevation; /* distant */
    float x, y, z;            /* point, spot */
    float px, py, pz;         /* spot: pointsAt */
    float spec_exp;           /* spot: specularExponent (positive) */
    bool has_cone;            /* spot: limitingConeAngle set */
    float cone;
} sv_light;

/* filter::Primitive: the fields a kind does not use stay zero. */
typedef struct sv_fprim {
    sv_rect rect; /* the primitive subregion (user space) */
    bool linear;  /* color-interpolation-filters: linearRGB (else sRGB) */
    const char *result;
    sv_fkind kind;
    sv_fin in1, in2;
    /* merge */
    sv_fin *inputs;
    size_t ninputs;
    /* blend */
    sv_blend mode;
    /* composite */
    sv_coop op;
    float k1, k2, k3, k4;
    /* color matrix */
    sv_cmkind cm;
    float m[20];    /* CM_MATRIX */
    float cm_value; /* saturate amount / hue-rotate degrees */
    /* component transfer: r, g, b, a */
    sv_tfunc func[4];
    /* convolve matrix */
    uint32_t target_x, target_y, columns, rows;
    const float *matrix; /* columns * rows */
    float divisor, bias;
    sv_edge edge;
    bool preserve_alpha;
    /* displacement map */
    float scale;
    sv_channel xch, ych;
    /* drop shadow, offset */
    float dx, dy;
    /* drop shadow, gaussian blur */
    float sdx, sdy;
    /* drop shadow, flood, lighting colour */
    uint8_t r, g, b;
    float opacity;
    /* image */
    sv_unode *root;
    /* morphology */
    bool dilate;
    float rx, ry;
    /* lighting */
    float surface_scale, constant, exponent;
    sv_light light;
    /* turbulence */
    float bfx, bfy;
    uint32_t octaves;
    int32_t seed;
    bool stitch, fractal;
} sv_fprim;

/* filter::Filter */
struct sv_filter {
    sv_rect rect; /* the filter region (user space) */
    sv_fprim *prims;
    size_t nprims;
    uint32_t refs; /* groups using it */
};

/* svg_filter_func.c: one item of a `filter` value list (svgtypes::FilterValue). */
typedef enum {
    FV_BLUR,
    FV_DROP_SHADOW,
    FV_BRIGHTNESS,
    FV_CONTRAST,
    FV_GRAYSCALE,
    FV_HUE_ROTATE,
    FV_INVERT,
    FV_OPACITY,
    FV_SEPIA,
    FV_SATURATE,
    FV_URL
} sv_ffkind;

typedef struct sv_ffunc {
    sv_ffkind kind;
    sv_length std_dev, dx, dy;
    bool has_color;
    sv_color color;
    double amount; /* generic functions; hue-rotate: degrees */
    const char *url;
    size_t url_len;
} sv_ffunc;

/* FilterValueListParser::next: 1 a value, 0 the end, -1 an error (the stream jumps to its end). */
int sv_ffunc_next(sv_stream *s, sv_ffunc *v);
/* create_base_filter_func: appends the filter of a filter function to *list (nothing without an
 * object bounding box); false when out of memory. */
bool sv_ffunc_filter(sv_conv *c, const st_node *n, const sv_cstate *st, const sv_ffunc *v,
                     const sv_rect *object_bbox, sv_filter ***list, size_t *cnt, size_t *cap);

/* svg_filter_conv.c state hung off sv_conv (cache of shareable filters, cycle guard, budgets). */
typedef struct sv_fconv sv_fconv;

/* ---- svg_filter_fx.c / svg_filter_light.c: the pixel kernels (resvg's filter modules) ----
 * Images are RGBA8 w * h; "premultiplied" as resvg says per function. */

/* box_blur::apply / iir_blur::apply. false when out of memory. */
bool sv_fx_box_blur(double sx, double sy, uint8_t *px, uint32_t w, uint32_t h);
bool sv_fx_iir_blur(double sx, double sy, uint8_t *px, uint32_t w, uint32_t h);
/* color_matrix::apply (demultiplied pixels). */
void sv_fx_color_matrix(const sv_fprim *fe, uint8_t *px, size_t n);
/* component_transfer::apply (demultiplied pixels). */
void sv_fx_component_transfer(const sv_fprim *fe, uint8_t *px, size_t n);
/* composite::arithmetic (all three the same size). */
void sv_fx_arithmetic(float k1, float k2, float k3, float k4, const uint8_t *a, const uint8_t *b,
                      uint8_t *dst, size_t n);
/* convolve_matrix::apply: false when out of memory or past the work cap. */
bool sv_fx_convolve(const sv_fprim *fe, uint8_t *px, uint32_t w, uint32_t h);
/* morphology::apply: false when out of memory. */
bool sv_fx_morphology(bool dilate, float rx, float ry, uint8_t *px, uint32_t w, uint32_t h);
/* displacement_map::apply (all three the same size). */
void sv_fx_displacement(const sv_fprim *fe, float sx, float sy, const uint8_t *src, const uint8_t *map,
                        uint8_t *dst, uint32_t w, uint32_t h);
/* turbulence::apply: false past the work cap. */
bool sv_fx_turbulence(const sv_fprim *fe, double off_x, double off_y, double sx, double sy, uint8_t *px,
                      uint32_t w, uint32_t h);
/* lighting::{diffuse,specular}_lighting (src and dst the same size). */
void sv_fx_lighting(const sv_fprim *fe, sv_light light, const uint8_t *src, uint8_t *dst, uint32_t w,
                    uint32_t h);

/* resvg's f32_bound: min / max without NaN handling (NaN passes through). */
static inline float sv_f32_bound(float lo, float v, float hi)
{
    return v > hi ? hi : v < lo ? lo : v;
}

#endif
