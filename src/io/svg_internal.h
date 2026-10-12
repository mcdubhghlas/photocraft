/* SVG internals shared by src/io/svg_*.c (port of what crates/io/src/svg.rs takes from usvg,
 * svgtypes, roxmltree, simplecss, kurbo and tiny-skia-path, and of resvg's rendering).
 *
 * The pipeline mirrors usvg's:
 *   svg_xml.c    bytes (UTF-8 or gzip) -> an XML DOM (entities, CDATA, namespaces)
 *   svg_css.c    <style> sheets and style="" declarations (simplecss)
 *   svg_tree.c   DOM -> svgtree: SVG elements with resolved presentation attributes, CSS,
 *                `inherit`, expanded <use> and an id map (usvg::parser::svgtree)
 *   svg_types.c  attribute value parsers (svgtypes) incl. the simplifying path parser, kurbo arcs
 *   svg_tsk.c    tiny-skia's f32 Transform, Rect and Path (geometry is f32 like usvg's)
 *   svg_conv.c   svgtree -> render tree: groups, paths, images with absolute transforms, paints,
 *   svg_paint.c  clip paths, masks, bounding boxes (usvg::parser::converter & co.)
 *   svg_filter*.c filters: conversion (usvg filter.rs) and effects (resvg's filter module)
 *   svg_render.c the render tree drawn as resvg does (groups, clips, masks, patterns, images)
 *   svg_raster.c path coverage with src/vector's rasteriser (stands in for tiny-skia's scan
 *                converters; strokes, dashes, hairlines)
 *   svg_pipe.c   tiny-skia's raster pipeline: shaders, blend modes, blitter, masks
 *   svg_filter*.c filter effects (resvg's filter module)
 *   svg.c        crates/io/src/svg.rs: the render tree as PhotoCraft layers.
 * Everything a parse allocates lives in one arena (sv_arena), released at once. Not reentrant
 * per arena; separate imports are independent. */
#ifndef PC_IO_SVG_INTERNAL_H
#define PC_IO_SVG_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "base/status.h"

/* ---------------------------------------------------------------- arena */

typedef struct sv_chunk sv_chunk;

typedef struct sv_arena {
    sv_chunk *head;
    size_t total; /* bytes allocated so far */
    bool nomem;   /* an allocation failed (out of memory or past the cap) */
} sv_arena;

/* Everything one SVG may allocate (the reference has no cap: memory for a pathological drawing). */
#define SV_ARENA_MAX ((size_t)1 << 31)

void sv_arena_init(sv_arena *a);
void sv_arena_fini(sv_arena *a);
/* Zeroed memory or NULL (sets a->nomem). */
void *sv_alloc(sv_arena *a, size_t n);
void *sv_alloc_array(sv_arena *a, size_t count, size_t size);
/* NUL-terminated copy of s[0..n) or NULL. */
char *sv_strndup(sv_arena *a, const char *s, size_t n);
/* Grows *v (an array of elem-sized items with *cap capacity) to hold need items. */
bool sv_grow(sv_arena *a, void **v, size_t *cap, size_t need, size_t elem);

/* ---------------------------------------------------------------- XML DOM (svg_xml.c) */

#define SV_NS_SVG "http://www.w3.org/2000/svg"
#define SV_NS_XLINK "http://www.w3.org/1999/xlink"
#define SV_NS_XML "http://www.w3.org/XML/1998/namespace"

typedef struct sx_attr {
    const char *ns; /* resolved namespace URI, NULL for none */
    const char *name;
    const char *value; /* normalised, entities expanded, NUL-terminated */
} sx_attr;

typedef struct sx_node sx_node;
struct sx_node {
    bool is_text;
    const char *ns;   /* element namespace URI or NULL */
    const char *name; /* local name */
    sx_attr *attrs;
    size_t nattrs;
    const char *text; /* text node */
    sx_node *parent, *first, *last, *next, *prev;
};

/* The document node (its children: the root element and nothing else that matters). */
typedef struct sx_doc {
    sx_node *root;
} sx_doc;

/* Parses XML. Errors: PC_ERR_FORMAT with roxmltree-like text in msg (cap bytes); out of memory
 * PC_ERR_NOMEM. */
pc_status sx_parse(sv_arena *a, const char *text, size_t len, sx_doc *out, char *msg, size_t cap);
/* The attribute with this local name and namespace (NULL = none), or NULL. */
const char *sx_attr_get(const sx_node *n, const char *ns, const char *name);
/* First child element, the next element sibling, the previous element sibling. */
const sx_node *sx_first_element(const sx_node *n);
const sx_node *sx_next_element(const sx_node *n);
const sx_node *sx_prev_element(const sx_node *n);
/* Concatenated text of the first text child (roxmltree's Node::text). */
const char *sx_text(const sx_node *n);

/* ---------------------------------------------------------------- CSS (svg_css.c) */

typedef struct sc_decl {
    const char *name;
    const char *value;
    bool important;
} sc_decl;

typedef struct sc_rule sc_rule;

typedef struct sc_sheet {
    sc_rule **rules; /* sorted by specificity (stable) */
    size_t n;
    size_t cap;
} sc_sheet;

/* Adds the rules of a style sheet (simplecss::StyleSheet::parse_more). */
bool sc_parse_more(sv_arena *a, sc_sheet *s, const char *text);
bool sc_rule_matches(const sc_rule *r, const sx_node *el);
const sc_decl *sc_rule_decls(const sc_rule *r, size_t *n);
/* The declarations of a style="" attribute (simplecss::DeclarationTokenizer). */
bool sc_parse_decls(sv_arena *a, const char *text, sc_decl **out, size_t *n);

/* ---------------------------------------------------------------- names */

typedef enum sv_eid {
    E_NONE = 0,
    E_A,
    E_CIRCLE,
    E_CLIP_PATH,
    E_DEFS,
    E_ELLIPSE,
    E_FE_IMAGE,
    E_FILTER,
    E_G,
    E_IMAGE,
    E_LINE,
    E_LINEAR_GRADIENT,
    E_MARKER,
    E_MASK,
    E_PATH,
    E_PATTERN,
    E_POLYGON,
    E_POLYLINE,
    E_RADIAL_GRADIENT,
    E_RECT,
    E_STOP,
    E_STYLE,
    E_SVG,
    E_SWITCH,
    E_SYMBOL,
    E_TEXT,
    E_TEXT_PATH,
    E_TREF,
    E_TSPAN,
    E_USE,
    E_FE_OTHER, /* unused (kept for older code) */
    E_FE_BLEND, /* filter primitives and their children (I5b filters) */
    E_FE_COLOR_MATRIX,
    E_FE_COMPONENT_TRANSFER,
    E_FE_COMPOSITE,
    E_FE_CONVOLVE_MATRIX,
    E_FE_DIFFUSE_LIGHTING,
    E_FE_DISPLACEMENT_MAP,
    E_FE_DISTANT_LIGHT,
    E_FE_DROP_SHADOW,
    E_FE_FLOOD,
    E_FE_FUNC_A,
    E_FE_FUNC_B,
    E_FE_FUNC_G,
    E_FE_FUNC_R,
    E_FE_GAUSSIAN_BLUR,
    E_FE_MERGE,
    E_FE_MERGE_NODE,
    E_FE_MORPHOLOGY,
    E_FE_OFFSET,
    E_FE_POINT_LIGHT,
    E_FE_SPECULAR_LIGHTING,
    E_FE_SPOT_LIGHT,
    E_FE_TILE,
    E_FE_TURBULENCE,
    E_COUNT
} sv_eid;

typedef enum sv_aid {
    A_NONE = 0,
    /* presentation attributes (usvg's is_presentation list) */
    A_ALIGNMENT_BASELINE,
    A_BASELINE_SHIFT,
    A_BACKGROUND_COLOR,
    A_CLIP_PATH,
    A_CLIP_RULE,
    A_COLOR,
    A_COLOR_INTERPOLATION,
    A_COLOR_INTERPOLATION_FILTERS,
    A_COLOR_RENDERING,
    A_DIRECTION,
    A_DISPLAY,
    A_DOMINANT_BASELINE,
    A_FILL,
    A_FILL_OPACITY,
    A_FILL_RULE,
    A_FILTER,
    A_FLOOD_COLOR,
    A_FLOOD_OPACITY,
    A_FONT_FAMILY,
    A_FONT_KERNING,
    A_FONT_SIZE,
    A_FONT_SIZE_ADJUST,
    A_FONT_STRETCH,
    A_FONT_STYLE,
    A_FONT_VARIANT,
    A_FONT_WEIGHT,
    A_GLYPH_ORIENTATION_HORIZONTAL,
    A_GLYPH_ORIENTATION_VERTICAL,
    A_IMAGE_RENDERING,
    A_ISOLATION,
    A_LETTER_SPACING,
    A_LIGHTING_COLOR,
    A_MARKER_END,
    A_MARKER_MID,
    A_MARKER_START,
    A_MASK,
    A_MASK_TYPE,
    A_MIX_BLEND_MODE,
    A_OPACITY,
    A_OVERFLOW,
    A_PAINT_ORDER,
    A_SHAPE_RENDERING,
    A_STOP_COLOR,
    A_STOP_OPACITY,
    A_STROKE,
    A_STROKE_DASHARRAY,
    A_STROKE_DASHOFFSET,
    A_STROKE_LINECAP,
    A_STROKE_LINEJOIN,
    A_STROKE_MITERLIMIT,
    A_STROKE_OPACITY,
    A_STROKE_WIDTH,
    A_TEXT_ANCHOR,
    A_TEXT_DECORATION,
    A_TEXT_OVERFLOW,
    A_TEXT_RENDERING,
    A_TRANSFORM,
    A_TRANSFORM_ORIGIN,
    A_UNICODE_BIDI,
    A_VECTOR_EFFECT,
    A_VISIBILITY,
    A_WHITE_SPACE,
    A_WORD_SPACING,
    A_WRITING_MODE,
    A_LAST_PRESENTATION = A_WRITING_MODE,
    /* other attributes the conversion reads */
    A_CLASS,
    A_CLIP_PATH_UNITS,
    A_CX,
    A_CY,
    A_D,
    A_DX,
    A_DY,
    A_FILTER_UNITS,
    A_FONT,
    A_FX,
    A_FY,
    A_GRADIENT_TRANSFORM,
    A_GRADIENT_UNITS,
    A_HEIGHT,
    A_HREF,
    A_ID,
    A_KERNING,
    A_LINE_HEIGHT,
    A_MARKER_HEIGHT,
    A_MARKER_WIDTH,
    A_MASK_CONTENT_UNITS,
    A_MASK_UNITS,
    A_OFFSET,
    A_PATTERN_CONTENT_UNITS,
    A_PATTERN_TRANSFORM,
    A_PATTERN_UNITS,
    A_POINTS,
    A_PRESERVE_ASPECT_RATIO,
    A_PRIMITIVE_UNITS,
    A_R,
    A_REF_X,
    A_REF_Y,
    A_REQUIRED_EXTENSIONS,
    A_REQUIRED_FEATURES,
    A_RX,
    A_RY,
    A_SPACE,
    A_SPREAD_METHOD,
    A_STYLE,
    A_SYSTEM_LANGUAGE,
    A_VIEW_BOX,
    A_WIDTH,
    A_X,
    A_X1,
    A_X2,
    A_Y,
    A_Y1,
    A_Y2,
    A_FONT_VARIANT_CAPS,
    A_FONT_VARIANT_LIGATURES,
    A_FONT_VARIANT_NUMERIC,
    A_FONT_VARIANT_EAST_ASIAN,
    A_FONT_VARIANT_POSITION,
    A_LENGTH_ADJUST, /* text (I5b) */
    A_ROTATE,
    A_START_OFFSET,
    A_TEXT_LENGTH,
    A_MARKER_UNITS, /* markers (I5b) */
    A_ORIENT,
    A_AMPLITUDE, /* filter primitives (I5b filters) */
    A_AZIMUTH,
    A_BASE_FREQUENCY,
    A_BIAS,
    A_DIFFUSE_CONSTANT,
    A_DIVISOR,
    A_EDGE_MODE,
    A_ELEVATION,
    A_EXPONENT,
    A_IN,
    A_IN2,
    A_INTERCEPT,
    A_K1,
    A_K2,
    A_K3,
    A_K4,
    A_KERNEL_MATRIX,
    A_LIMITING_CONE_ANGLE,
    A_MODE,
    A_NUM_OCTAVES,
    A_OPERATOR,
    A_ORDER,
    A_POINTS_AT_X,
    A_POINTS_AT_Y,
    A_POINTS_AT_Z,
    A_PRESERVE_ALPHA,
    A_RADIUS,
    A_RESULT,
    A_SCALE,
    A_SEED,
    A_SLOPE,
    A_SPECULAR_CONSTANT,
    A_SPECULAR_EXPONENT,
    A_STD_DEVIATION,
    A_STITCH_TILES,
    A_SURFACE_SCALE,
    A_TABLE_VALUES,
    A_TARGET_X,
    A_TARGET_Y,
    A_TYPE,
    A_VALUES,
    A_X_CHANNEL_SELECTOR,
    A_Y_CHANNEL_SELECTOR,
    A_Z,
    A_COUNT
} sv_aid;

sv_eid sv_eid_from(const char *name);
sv_aid sv_aid_from(const char *name);
const char *sv_aid_name(sv_aid a);
bool sv_aid_is_presentation(sv_aid a);
bool sv_aid_is_inheritable(sv_aid a);
bool sv_aid_allows_inherit(sv_aid a);
bool sv_eid_is_graphic(sv_eid e);

/* ---------------------------------------------------------------- svgtree (svg_tree.c) */

typedef struct st_attr {
    sv_aid aid;
    const char *value;
    bool important;
} st_attrv;

typedef struct st_node st_node;
struct st_node {
    int kind; /* ST_ROOT, ST_ELEMENT, ST_TEXT */
    sv_eid eid;
    st_attrv *attrs;
    size_t nattrs;
    size_t capattrs;
    const char *text;
    st_node *parent, *first, *last, *next;
};

enum { ST_ROOT, ST_ELEMENT, ST_TEXT };

typedef struct st_link {
    const char *id;
    st_node *node;
} st_link;

typedef struct st_doc {
    st_node *root;
    st_link *links; /* sorted by id */
    size_t nlinks;
    size_t nodes;
} st_doc;

/* Builds the svgtree; PC_ERR_FORMAT "the root node was not found" etc. in msg. */
pc_status st_build(sv_arena *a, const sx_doc *xml, st_doc *out, char *msg, size_t cap);
const char *st_attr(const st_node *n, sv_aid aid);
bool st_has(const st_node *n, sv_aid aid);
const char *st_id(const st_node *n); /* "" when none */
st_node *st_by_id(const st_doc *d, const char *id, size_t len);
/* Nearest ancestor-or-self element with aid (inheritable) / self or parent (otherwise). */
const st_node *st_find_attr_node(const st_node *n, sv_aid aid);
const st_node *st_parent_element(const st_node *n);
const st_node *st_first_element(const st_node *n);
/* The element an href (#id) or a FuncIRI url(#id) attribute links to, or NULL. */
st_node *st_node_attr(const st_doc *d, const st_node *n, sv_aid aid);

/* ---------------------------------------------------------------- svgtypes (svg_types.c) */

typedef enum { U_NONE, U_EM, U_EX, U_PX, U_IN, U_CM, U_MM, U_PT, U_PC, U_PERCENT } sv_unit;

typedef struct sv_length {
    double number;
    sv_unit unit;
} sv_length;

typedef struct sv_color {
    uint8_t r, g, b, a;
} sv_color;

typedef struct sv_stream {
    const char *s;
    size_t len;
    size_t pos;
} sv_stream;

void ss_init(sv_stream *st, const char *s);
bool ss_at_end(const sv_stream *st);
void ss_skip_spaces(sv_stream *st);
bool ss_parse_number(sv_stream *st, double *out);
bool ss_parse_list_number(sv_stream *st, double *out);
bool ss_parse_length(sv_stream *st, sv_length *out);
bool ss_parse_list_length(sv_stream *st, sv_length *out);
bool ss_parse_color(sv_stream *st, sv_color *out);

bool sv_parse_number(const char *s, double *out);    /* Number::from_str (whole string) */
bool sv_parse_length(const char *s, sv_length *out); /* Length::from_str */
bool sv_parse_color(const char *s, sv_color *out);   /* Color::from_str */
/* svgtypes::Transform::from_str as f64 a..f. */
bool sv_parse_transform(const char *s, double m[6]);
/* IRI "#id" / FuncIRI "url(#id)": the id range in s. */
bool sv_parse_iri(const char *s, const char **id, size_t *len);
bool sv_parse_func_iri(const char *s, const char **id, size_t *len);

typedef enum {
    P_NONE,
    P_INHERIT,
    P_CURRENT_COLOR,
    P_COLOR,
    P_FUNC_IRI,
    P_CONTEXT_FILL,
    P_CONTEXT_STROKE
} sv_pkind;
typedef enum { PF_NONE_SET, PF_NONE, PF_CURRENT_COLOR, PF_COLOR } sv_pfkind;

typedef struct sv_spaint {
    sv_pkind kind;
    sv_color color;
    const char *iri; /* P_FUNC_IRI: id range */
    size_t iri_len;
    sv_pfkind fallback;
    sv_color fallback_color;
} sv_spaint;

bool sv_parse_paint(const char *s, sv_spaint *out);

typedef struct sv_viewbox {
    double x, y, w, h;
} sv_viewbox;

bool sv_parse_viewbox(const char *s, sv_viewbox *out);

typedef enum {
    AL_NONE,
    AL_XMINYMIN,
    AL_XMIDYMIN,
    AL_XMAXYMIN,
    AL_XMINYMID,
    AL_XMIDYMID,
    AL_XMAXYMID,
    AL_XMINYMAX,
    AL_XMIDYMAX,
    AL_XMAXYMAX
} sv_align;

typedef struct sv_aspect {
    bool defer;
    sv_align align;
    bool slice;
} sv_aspect;

sv_aspect sv_aspect_default(void); /* xMidYMid meet */
bool sv_parse_aspect(const char *s, sv_aspect *out);

/* SimplifyingPathParser: absolute M/L/C/Q/Z with arcs as cubics. */
typedef enum { SEG_MOVE, SEG_LINE, SEG_CURVE, SEG_QUAD, SEG_CLOSE } sv_segkind;

typedef struct sv_seg {
    sv_segkind kind;
    double x1, y1, x2, y2, x, y;
} sv_seg;

/* Calls f for each simplified segment until the data ends or has an error (then stops, as
 * usvg's `break`). Returns false only when f returns false (out of memory). */
bool sv_simplify_path(const char *d, bool (*f)(void *ctx, const sv_seg *seg), void *ctx);
/* kurbo::Arc::from_svg_arc(..).to_cubic_beziers(0.1, f); false when it is a straight line (the
 * caller draws a line). *ok false when f failed. */
bool sv_arc_to_cubics(double fx, double fy, double rx, double ry, double x_rot_deg, bool large, bool sweep,
                      double tx, double ty, bool (*f)(void *ctx, const double c[6]), void *ctx, bool *ok);

/* ---------------------------------------------------------------- tiny-skia geometry (svg_tsk.c) */

typedef struct sv_ts {
    float sx, ky, kx, sy, tx, ty;
} sv_ts;

typedef struct sv_rect {
    float l, t, r, b;
} sv_rect;

sv_ts ts_identity(void);
sv_ts ts_row(float sx, float ky, float kx, float sy, float tx, float ty);
sv_ts ts_translate(float tx, float ty);
sv_ts ts_scale(float sx, float sy);
bool ts_is_identity(sv_ts t);
bool ts_has_skew(sv_ts t);
bool ts_is_valid(sv_ts t);
bool ts_is_finite(sv_ts t);
sv_ts ts_pre_concat(sv_ts a, sv_ts b);  /* a.pre_concat(b) */
sv_ts ts_post_concat(sv_ts a, sv_ts b); /* a.post_concat(b) */
sv_ts ts_pre_translate(sv_ts a, float tx, float ty);
bool ts_invert(sv_ts t, sv_ts *out);
void ts_map_point(sv_ts t, float *x, float *y);
void ts_get_scale(sv_ts t, float *sx, float *sy);
sv_ts ts_from_bbox(sv_rect r); /* Transform::from_bbox */

bool rect_from_ltrb(float l, float t, float r, float b, sv_rect *out);
bool rect_from_xywh(float x, float y, float w, float h, sv_rect *out);
bool nzrect_from_ltrb(float l, float t, float r, float b, sv_rect *out);
bool nzrect_from_xywh(float x, float y, float w, float h, sv_rect *out);
float rect_w(sv_rect r);
float rect_h(sv_rect r);
bool rect_transform(sv_rect r, sv_ts t, sv_rect *out);   /* Rect::transform */
bool nzrect_transform(sv_rect r, sv_ts t, sv_rect *out); /* NonZeroRect::transform */
sv_rect rect_bbox_transform(sv_rect r, sv_rect bbox);    /* NonZeroRect::bbox_transform */
bool rect_to_nz(sv_rect r, sv_rect *out);

/* usvg's BBox accumulator. */
typedef struct sv_bbox {
    float l, t, r, b;
} sv_bbox;

sv_bbox bbox_default(void);
sv_bbox bbox_expand(sv_bbox b, sv_rect r);
bool bbox_to_rect(sv_bbox b, sv_rect *out);
bool bbox_to_nz(sv_bbox b, sv_rect *out);

typedef enum { V_MOVE, V_LINE, V_QUAD, V_CUBIC, V_CLOSE } sv_verb;

typedef struct sv_pt {
    float x, y;
} sv_pt;

/* tiny_skia_path::Path (immutable once finished). */
typedef struct sv_path {
    uint8_t *verbs;
    size_t nverbs;
    sv_pt *pts;
    size_t npts;
    sv_rect bounds;
} sv_path;

typedef struct sv_pb {
    sv_arena *a;
    uint8_t *verbs;
    size_t nverbs, capverbs;
    sv_pt *pts;
    size_t npts, cappts;
    size_t last_move_to;
    bool move_to_required;
    bool failed;
} sv_pb;

void pb_init(sv_pb *b, sv_arena *a);
void pb_move_to(sv_pb *b, float x, float y);
void pb_line_to(sv_pb *b, float x, float y);
void pb_quad_to(sv_pb *b, float x1, float y1, float x, float y);
void pb_cubic_to(sv_pb *b, float x1, float y1, float x2, float y2, float x, float y);
void pb_close(sv_pb *b);
bool pb_last_point(const sv_pb *b, sv_pt *out);
/* PathBuilder::finish: NULL for fewer than two verbs, bad bounds or out of memory (b->failed). */
sv_path *pb_finish(sv_pb *b);
sv_path *path_from_rect(sv_arena *a, sv_rect r);
/* Path::transform: a new path, NULL when the bounds become invalid or out of memory. */
const sv_path *path_transform(sv_arena *a, const sv_path *p, sv_ts t);
bool path_tight_bounds(const sv_path *p, sv_rect *out);

/* One segment of PathSegmentsIter (without auto-close). */
typedef struct sv_pseg {
    sv_verb verb;
    sv_pt p[3];
} sv_pseg;

typedef struct sv_piter {
    const sv_path *path;
    size_t vi, pi;
    sv_pt last_move_to, last_point;
} sv_piter;

void piter_init(sv_piter *it, const sv_path *p);
bool piter_next(sv_piter *it, sv_pseg *out);

/* ---------------------------------------------------------------- render tree (usvg::Tree) */

typedef enum { UNITS_USER, UNITS_OBB } sv_units;
typedef enum { SPREAD_PAD, SPREAD_REFLECT, SPREAD_REPEAT } sv_spread;
typedef enum {
    BM_NORMAL,
    BM_MULTIPLY,
    BM_SCREEN,
    BM_OVERLAY,
    BM_DARKEN,
    BM_LIGHTEN,
    BM_COLOR_DODGE,
    BM_COLOR_BURN,
    BM_HARD_LIGHT,
    BM_SOFT_LIGHT,
    BM_DIFFERENCE,
    BM_EXCLUSION,
    BM_HUE,
    BM_SATURATION,
    BM_COLOR,
    BM_LUMINOSITY
} sv_blend;
typedef enum { CAP_BUTT, CAP_ROUND, CAP_SQUARE } sv_cap;
typedef enum { JOIN_MITER, JOIN_MITER_CLIP, JOIN_ROUND, JOIN_BEVEL } sv_join;

typedef struct sv_stop {
    float offset;
    uint8_t r, g, b;
    float opacity;
} sv_stop;

typedef struct sv_grad {
    bool radial;
    float x1, y1, x2, y2;    /* linear */
    float cx, cy, r, fx, fy; /* radial */
    sv_units units;
    sv_ts transform;
    sv_spread spread;
    sv_stop *stops;
    size_t nstops;
} sv_grad;

typedef struct sv_unode sv_unode;
typedef struct sv_filter sv_filter; /* svg_filter.h */

typedef struct sv_pattern {
    sv_units units, content_units;
    sv_ts transform;
    sv_rect rect;
    bool has_vb;
    sv_rect vb;
    sv_aspect aspect;
    sv_unode *root;
} sv_pattern;

typedef enum { PAINT_COLOR, PAINT_LINEAR, PAINT_RADIAL, PAINT_PATTERN } sv_paint_kind;

typedef struct sv_paint {
    sv_paint_kind kind;
    uint8_t r, g, b;
    sv_grad *grad;
    sv_pattern *pat;
} sv_paint;

/* context-fill / context-stroke: from a <use> element, or from the path a marker is drawn on
 * (resolved against sv_cstate's ctx_path_* when the marker content is converted) */
typedef enum { CTX_NONE, CTX_USE, CTX_PATH } sv_ctx;

typedef struct sv_fill {
    sv_paint paint;
    float opacity;
    bool evenodd;
    sv_ctx context;
} sv_fill;

typedef struct sv_stroke {
    sv_paint paint;
    float *dash; /* NULL = none */
    size_t ndash;
    float dashoffset;
    float miterlimit;
    float opacity;
    float width;
    sv_cap cap;
    sv_join join;
    sv_ctx context;
} sv_stroke;

typedef struct sv_clip sv_clip;
struct sv_clip {
    sv_ts transform;
    sv_clip *clip;
    sv_unode *root;
};

typedef struct sv_mask sv_mask;
struct sv_mask {
    sv_rect rect;
    bool alpha; /* mask-type: alpha (else luminance) */
    sv_mask *mask;
    sv_unode *root;
};

typedef enum { UN_GROUP, UN_PATH, UN_IMAGE, UN_TEXT } sv_ukind;

struct sv_unode {
    sv_ukind kind;
    const char *id;
    sv_ts transform;     /* groups; images: their own placement */
    sv_ts abs_transform; /* groups: including transform; paths: the parent's */
    /* group */
    float opacity;
    sv_blend blend;
    bool isolate;
    bool is_context;
    sv_clip *clip;
    sv_mask *mask;
    sv_filter **filters; /* usvg's Group::filters (svg_filter.h) */
    size_t nfilters;
    sv_unode **children;
    size_t nchildren, capchildren;
    /* bounding boxes (as usvg) */
    sv_rect bbox, abs_bbox, stroke_bbox, abs_stroke_bbox;
    sv_rect layer_bbox, abs_layer_bbox; /* groups (non-zero) */
    /* path */
    bool visible;
    sv_fill *fill;
    sv_stroke *stroke;
    bool stroke_first; /* paint-order: stroke before fill */
    bool crisp;        /* shape-rendering optimizeSpeed / crispEdges: no anti-aliasing */
    const sv_path *data;
    /* image: straight RGBA8, or a nested SVG drawing (svg != NULL) */
    uint32_t iw, ih;
    uint8_t *rgba;
    float size_w, size_h; /* the image's own size */
    bool pixelated;
    uint8_t quality; /* sv_quality from image-rendering */
    const struct sv_tree *svg;
    /* text */
    const char *text; /* concatenated character data */
    sv_unode *flat;   /* text: the flattened group (usvg Text::flattened, svg_text*.c) */
};

typedef struct sv_tree {
    float width, height;
    sv_unode *root;
} sv_tree;

/* Parses an SVG (or gzip-compressed .svgz) into a render tree. Errors: PC_ERR_FORMAT with the
 * reference's Display text in msg ("provided data has not an UTF-8 encoding", "SVG has an
 * invalid size", "SVG data parsing failed cause ..."), PC_ERR_NOMEM. */
pc_status sv_parse_tree(sv_arena *a, const uint8_t *bytes, size_t len, sv_tree *out, char *msg, size_t cap);

/* svg_conv.c internals shared with svg_paint.c */
sv_unode *sv_group_empty(sv_arena *a);
bool sv_push_child(sv_arena *a, sv_unode *g, sv_unode *c);
bool sv_group_bboxes(sv_unode *g); /* Group::calculate_bounding_boxes */

/* ---------------------------------------------------------------- converter internals */

typedef struct sv_cstate {
    const st_node *parent_clip_path; /* inside a clipPath */
    bool has_context;                /* a <use> context element (context-fill / -stroke) */
    sv_fill *ctx_fill;
    sv_stroke *ctx_stroke;
    sv_rect view_box;
    bool use_w_set, use_h_set;
    float use_w, use_h;
    /* markers (svg_marker.c): the markers being converted (recursion guard; ids are dropped
     * inside them) and the path they are drawn on (its context paint transform and bbox) */
    const struct sv_mlink *markers;
    sv_ts ctx_path_ts;
    bool ctx_path_has_bbox;
    sv_rect ctx_path_bbox;
} sv_cstate;

typedef struct sv_mlink {
    const st_node *marker;
    const struct sv_mlink *next;
} sv_mlink;

typedef struct sv_centry {
    const st_node *node;
    void *val;
} sv_centry;

typedef struct sv_conv {
    sv_arena *a;
    const st_doc *doc;
    sv_centry *clips, *masks, *paints;
    size_t nclips, nmasks, npaints;
    size_t capclips, capmasks, cappaints;
    size_t depth;            /* conversion recursion */
    size_t marker_instances; /* markers drawn so far (svg_marker.c caps them) */
    bool no_images;          /* a drawing nested in an <image>: its own images are ignored */
    bool nomem;
    size_t text_glyphs, text_work; /* text budgets spent (svg_text.h caps) */
    struct sv_fconv *filt;         /* svg_filter_conv.c state (filter cache, guards) */
    bool text_fonts; /* svg.rs options(): fonts only when the top-level bytes are gzip or contain "text" */
} sv_conv;

/* Conversion nesting past which elements are dropped (bounded C stack). */
#define SV_MAX_CONV_DEPTH 400

float sv_convert_length(sv_conv *c, sv_length l, const st_node *n, sv_aid aid, sv_units units,
                        const sv_cstate *st);
float sv_node_length(sv_conv *c, const st_node *n, sv_aid aid, sv_units units, const sv_cstate *st,
                     sv_length def);
float sv_user_length(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st, sv_length def);
sv_ts sv_resolve_transform(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st);
bool sv_viewbox_of(const st_node *n, sv_rect *out);
sv_ts sv_viewbox_transform(sv_rect vb, sv_aspect aspect, float w, float h);
void sv_aligned_pos(sv_align al, float x, float y, float w, float h, float *ox, float *oy);
bool sv_parse_opacity(const char *v, float *out);
float sv_attr_opacity(const st_node *n, sv_aid aid);
/* The children of node converted into parent (converter::convert_children). */
void sv_convert_children(sv_conv *c, const st_node *node, const sv_cstate *st, sv_unode *parent);
void sv_convert_clip_children(sv_conv *c, const st_node *node, const sv_cstate *st, sv_unode *parent);
sv_unode *sv_new_path(sv_conv *c, const char *id, bool visible, sv_fill *fill, sv_stroke *stroke,
                      bool stroke_first, const sv_path *data, sv_ts abs);

/* A drawing embedded in an <image> of c's drawing (usvg load_sub_svg: same arena, no images of
 * its own); false when it doesn't parse (or c is itself nested). */
bool sv_parse_subtree(sv_conv *c, const uint8_t *bytes, size_t len, sv_tree *out);

/* svg_image.c: an <image> / feImage href as usvg resolves it (data: URLs only, never files). */
typedef struct sv_href_image {
    float w, h;                /* actual size (the image header's, or the nested drawing's) */
    uint32_t iw, ih;           /* decoded raster (0 x 0 and rgba NULL when it doesn't decode) */
    uint8_t *rgba;             /* straight RGBA8 in the arena */
    const struct sv_tree *svg; /* a nested drawing */
} sv_href_image;

bool sv_load_href(sv_conv *c, const char *href, sv_href_image *out);

/* svg_marker.c: usvg's marker::is_valid / marker::convert (markers of the shape n drawn along
 * path into parent). */
bool sv_marker_is_valid(const sv_conv *c, const st_node *n);
void sv_marker_convert(sv_conv *c, const st_node *n, const sv_path *path, const sv_cstate *st,
                       sv_unode *parent);

/* svg_attrs.c */
bool sv_object_bbox(const sv_unode *g, sv_rect *out); /* Group::calculate_object_bbox */
bool sv_condition_passed(const st_node *n);           /* switch: requiredFeatures, systemLanguage */
bool sv_is_visible_element(const st_node *n);         /* display, a valid transform, conditions */
sv_blend sv_blend_of(const st_node *n);
sv_aspect sv_aspect_of(const st_node *n);

static inline sv_length sv_len_px(double v)
{
    sv_length l = {v, U_NONE};
    return l;
}

static inline sv_length sv_len_pct(double v)
{
    sv_length l = {v, U_PERCENT};
    return l;
}

static inline bool sv_valid_length(float v)
{
    return v > 0.0f && isfinite(v);
}

/* strict_num's approx_eq_ulps for f32. */
static inline bool sv_approx_eq_ulps(float a, float b, int32_t ulps)
{
    int32_t ia;
    int32_t ib;
    int64_t d;
    if (a == b) {
        return true;
    }
    if (signbit(a) != signbit(b) || isnan(a) || isnan(b)) {
        return false;
    }
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    d = (int64_t)ia - (int64_t)ib;
    return (d < 0 ? -d : d) <= ulps;
}

/* svg_paint.c */
sv_fill *sv_resolve_fill(sv_conv *c, const st_node *n, bool has_bbox, const sv_cstate *st);
sv_stroke *sv_resolve_stroke(sv_conv *c, const st_node *n, bool has_bbox, const sv_cstate *st);
/* *skip true when the element must be dropped (an invalid clip path / mask / filter link). */
sv_clip *sv_convert_clip(sv_conv *c, const st_node *link, const sv_cstate *st, const sv_rect *object_bbox);
sv_mask *sv_convert_mask(sv_conv *c, const st_node *link, const sv_cstate *st, const sv_rect *object_bbox);
bool sv_convert_filters(sv_conv *c, const st_node *n, const sv_cstate *st, const sv_rect *object_bbox,
                        sv_filter ***out, size_t *nout);
/* svg_filter_conv.c: usvg's node_to_user_coordinates for the feImage content of g's filters;
 * svgtree's fix_recursive_fe_image. svg_conv.c: converter::convert_element (for feImage). */
void sv_filters_update_paint_servers(sv_conv *c, sv_unode *g, sv_ts ctx_ts, const sv_rect *ctx_bbox);
void sv_fix_recursive_fe_image(st_doc *d);
void sv_convert_element(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent);
void sv_convert_image(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent);
void sv_update_paint_servers(sv_conv *c, sv_unode *g, sv_ts ctx_ts, const sv_rect *ctx_bbox);
/* paint_server::process_paint (object-bbox units to user space, pattern content, context paint). */
bool sv_process_paint(sv_conv *c, sv_paint *paint, bool has_context, sv_ts ctx_ts, const sv_rect *ctx_bbox,
                      sv_ts path_ts, sv_rect bbox);

/* ---------------------------------------------------------------- rendering (svg_render.c) */

/* A premultiplied RGBA8 pixmap (tiny-skia's layout), malloc'd. */
typedef struct sv_pixmap {
    uint32_t w, h;
    uint8_t *px; /* w * h * 4 */
} sv_pixmap;

/* ---- path drawing (svg_raster.c; tiny-skia's painter / scan converters, see svg_ts.h) ---- */

/* A span sink: pixels [x, x + w) of row y (target coordinates, possibly past its edges) covered
 * with alpha a (1..255), in the order tiny-skia's blitters produce them. */
typedef struct sv_spans {
    void (*span)(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint8_t a);
    void *ctx;
} sv_spans;

/* Pixmap::fill_path / fill_rect / stroke_path of p drawn with ts into a w x h target (aa false:
 * aliased). PC_ERR_NOMEM; PC_ERR_LIMIT for geometry past the rasteriser's caps (the path is
 * then not drawn, or only partly). stroke_path dashes, then draws a hairline (strokes at most a
 * device pixel wide, or width 0) or fills the stroke outline. */
pc_status sv_fill_path(const sv_path *p, sv_ts ts, bool evenodd, bool aa, uint32_t w, uint32_t h,
                       const sv_spans *out);
pc_status sv_fill_rect(sv_rect r, sv_ts ts, bool aa, uint32_t w, uint32_t h, const sv_spans *out);
pc_status sv_stroke_path(const sv_path *p, sv_ts ts, const sv_stroke *s, bool aa, uint32_t w, uint32_t h,
                         const sv_spans *out);
/* tiny-skia's treat_as_hairline (anti-aliased): the coverage factor of a stroke at most one
 * device pixel wide, or a negative value for a regular stroke. */
float sv_hairline_coverage(float width, sv_ts ts);

/* 8-bit coverage of a w x h rectangle at (x0, y0) of the target (a NULL = nothing covered). */
typedef struct sv_cov {
    int32_t x0, y0;
    uint32_t w, h;
    uint8_t *a; /* malloc'd */
} sv_cov;

void sv_cov_fini(sv_cov *c);
/* Mask::fill_path: the coverage of p over the whole w x h target. PC_ERR_NOMEM / PC_ERR_LIMIT. */
pc_status sv_fill_cov(const sv_path *p, sv_ts ts, bool evenodd, bool aa, uint32_t w, uint32_t h, sv_cov *out);

/* ---- pixel pipeline (svg_pipe.c): tiny-skia's shaders, blend modes and blitter ---- */

/* tiny_skia::BlendMode */
typedef enum {
    TSB_CLEAR,
    TSB_SOURCE,
    TSB_DESTINATION,
    TSB_SOURCE_OVER,
    TSB_DESTINATION_OVER,
    TSB_SOURCE_IN,
    TSB_DESTINATION_IN,
    TSB_SOURCE_OUT,
    TSB_DESTINATION_OUT,
    TSB_SOURCE_ATOP,
    TSB_DESTINATION_ATOP,
    TSB_XOR,
    TSB_PLUS,
    TSB_MODULATE,
    TSB_SCREEN,
    TSB_OVERLAY,
    TSB_DARKEN,
    TSB_LIGHTEN,
    TSB_COLOR_DODGE,
    TSB_COLOR_BURN,
    TSB_HARD_LIGHT,
    TSB_SOFT_LIGHT,
    TSB_DIFFERENCE,
    TSB_EXCLUSION,
    TSB_MULTIPLY,
    TSB_HUE,
    TSB_SATURATION,
    TSB_COLOR,
    TSB_LUMINOSITY
} sv_tsblend;

typedef enum { SQ_NEAREST, SQ_BILINEAR, SQ_BICUBIC } sv_quality;
typedef enum { SH_SOLID, SH_LINEAR, SH_RADIAL, SH_PATTERN } sv_shkind;

typedef struct sv_gstop {
    float t;
    float c[4]; /* straight RGBA 0..1 */
} sv_gstop;

/* A paint shader (tiny_skia::Shader with its pipeline context); fields are private to
 * svg_pipe.c. Release with sv_shader_fini. */
typedef struct sv_shader {
    sv_shkind kind;
    float color[4]; /* solid: straight RGBA */
    /* gradients */
    sv_gstop *stops;
    size_t nstops;
    sv_spread spread;
    sv_ts transform, unit, inv;
    bool opaque, uniform, two_stop, lowp;
    int focal; /* 0 none, 1 on circle, 2 well behaved, 3 greater */
    float r1;
    float factor[4], bias[4];
    float *t_values;
    float (*factors)[4];
    float (*biases)[4];
    size_t len;
    /* patterns (the pixmap is borrowed) */
    const sv_pixmap *pm;
    bool repeat;
    sv_quality quality;
    float popacity;
} sv_shader;

/* Opacity::to_u8 */
uint8_t sv_opacity_u8(float o);
void sv_shader_solid(sv_shader *s, uint8_t r, uint8_t g, uint8_t b, float opacity);
/* Linear / radial gradient (convert_*_gradient + Shader::transform(path_ts)): 1 ready, 0 nothing
 * to draw, -1 out of memory. A one-stop or degenerate gradient becomes a solid colour. */
int sv_shader_gradient(sv_shader *s, const sv_grad *g, float opacity, sv_ts path_ts);
/* Pattern of pm (pattern space -> device: ts): 1 ready, 0 when ts is not invertible. */
int sv_shader_pattern(sv_shader *s, const sv_pixmap *pm, bool repeat, sv_quality q, float opacity, sv_ts ts);
/* Shader::apply_opacity; false when out of memory. */
bool sv_shader_apply_opacity(sv_shader *s, float o);
bool sv_shader_is_opaque(const sv_shader *s);
void sv_shader_fini(sv_shader *s);
/* The pipeline as an sv_spans sink (RasterPipelineBlitter::new): false when nothing would be
 * drawn (Destination; DestinationIn with an opaque solid colour). ps must outlive out's use. */
typedef struct sv_pipe_spans {
    sv_pixmap *pm;
    const sv_shader *s;
    sv_tsblend m;
    bool lowp, source;
} sv_pipe_spans;
bool sv_pipe_spans_init(sv_pipe_spans *ps, sv_pixmap *pm, const sv_shader *s, sv_tsblend m, sv_spans *out);
/* Full-coverage rectangle [x0, x1) x [y0, y1) (clipped to pm). */
void sv_blit_rect(sv_pixmap *pm, int64_t x0, int64_t y0, int64_t x1, int64_t y1, const sv_shader *s,
                  sv_tsblend m);
/* Pixmap::draw_pixmap(x, y, src, opacity, mode) with nearest sampling and an identity transform. */
void sv_draw_pixmap(sv_pixmap *dst, int32_t x, int32_t y, const sv_pixmap *src, float opacity, sv_tsblend m);
/* Mask::from_pixmap (alpha or luminance) into out (w * h bytes); apply_mask (destination-in). */
void sv_mask_from_pixmap(const sv_pixmap *pm, bool luminance, uint8_t *out);
void sv_pixmap_apply_mask(sv_pixmap *pm, const uint8_t *mask);

/* A transparent pixmap; false when w * h is 0, over 2^28 or out of memory. */
bool sv_pixmap_new(uint32_t w, uint32_t h, sv_pixmap *out);
void sv_pixmap_fini(sv_pixmap *pm);

/* resvg::render: the whole tree with transform onto pm. Fails only on out of memory. */
pc_status sv_render_tree(const sv_tree *t, sv_ts transform, sv_pixmap *pm, pc_error *err);
/* resvg::render_node: node (its data in its parent's space) drawn with transform. */
pc_status sv_render_node(const sv_unode *n, sv_ts transform, sv_pixmap *pm, pc_error *err);

/* Renderer context shared with the filters (resvg's render::Context plus our guards). */
typedef struct sv_rctx {
    int32_t max_l, max_t, max_r, max_b; /* resvg's max_bbox: 4x the canvas around it */
    bool nomem;
    size_t depth;  /* nesting of groups, clips, masks, patterns, images */
    size_t pixels; /* scratch pixmap pixels alive (bounded) */
    size_t total;  /* scratch pixmap pixels allocated so far (bounded per render) */
} sv_rctx;

/* render_nodes with a context (filters render feImage content and use this). */
void sv_render_children(sv_rctx *r, const sv_unode *g, sv_ts ts, sv_pixmap *pm);
/* A scratch pixmap counted against the render's budget; false (r->nomem set on allocation
 * failure) when it can't be had. */
bool sv_rctx_pixmap(sv_rctx *r, uint32_t w, uint32_t h, sv_pixmap *out);
/* Frees a pixmap from sv_rctx_pixmap and returns its pixels to the live budget. */
void sv_rctx_release(sv_rctx *r, sv_pixmap *pm);

/* svg_filter.c: resvg's filter::apply for every filter of group g, in order, on the group's layer
 * pm (transform: the layer's). Sets r->nomem when out of memory. */
void sv_filters_apply(sv_rctx *r, const sv_unode *g, sv_ts ts, sv_pixmap *pm);

/* ================================================================ text (I5b; svg_text*.c, svg_font*.c)
 * Private details are in svg_text.h. */

/* svg_tree.c's builder, for the text preprocessing of svg_text_tree.c. */
typedef struct st_builder st_builder;
/* parse_svg_element (CSS and style applied, ids kept) of x as a child of parent. */
st_node *st_build_element(st_builder *b, const sx_node *x, st_node *parent, sv_eid tag);
/* A text node with this (arena) string as the last child of parent. */
st_node *st_build_text(st_builder *b, st_node *parent, const char *text);
/* The first XML element with this id (roxmltree descendants().find), or NULL. */
const sx_node *st_build_xml_by_id(const st_builder *b, const char *id, size_t len);
/* parse_tag_name: an SVG element's id (false for other nodes). */
bool st_build_tag(const sx_node *x, sv_eid *out);
sv_arena *st_build_arena(st_builder *b);
/* Bytes of tref text still allowed (shared budget per document). */
size_t *st_build_tref_budget(st_builder *b);
/* parse_svg_text_element: the tspan/tref/textPath/text-node children of <text> element n (built
 * from x) with xml:space handling and trimming. False on out of memory or the node limit. */
bool st_text_parse(st_builder *b, const sx_node *x, st_node *n);

/* svgtypes::FontShorthand parts (NUL-terminated arena copies; NULL when absent). */
typedef struct sv_font_shorthand {
    const char *style, *variant, *weight, *stretch;
    const char *size, *family;
} sv_font_shorthand;

bool sv_parse_font_shorthand(sv_arena *a, const char *s, sv_font_shorthand *out);

/* svg_conv.c helpers the text converter shares. */
const sv_path *sv_shape_path(sv_conv *c, const st_node *n, const sv_cstate *st); /* shapes::convert */
bool sv_stroke_first(const st_node *n);                                          /* paint-order */
/* units::resolve_font_size (svg_attrs.c). */
float sv_resolve_font_size(const st_node *n);
/* text::convert: lays <text> n out and appends a UN_TEXT node (with its flattened group) to
 * parent, or nothing when it has no glyphs (svg_text.c). */
void sv_convert_text(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent);

/* svg.rs's text layer name: the text trimmed, its first 31 characters (false when empty). */
bool sv_text_layer_name(const char *text, char *out, size_t cap);

/* ================================================================ end of text */

#endif
