/* svgtree (port of usvg 0.45's parser/svgtree/{mod,parse,names}.rs): the SVG elements of the XML
 * DOM with their attributes resolved: presentation attributes, then matching CSS rules (by
 * specificity), then style="" declarations (!important respected), `inherit` replaced by the
 * ancestor's value or the default, <use> targets copied under their <use> (ids dropped), and an
 * id map. Recursive pattern, clip-path, mask and filter references are cut to "none" so the
 * conversion terminates. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io/svg_internal.h"

/* ---------------------------------------------------------------- names */

typedef struct ename {
    const char *name;
    sv_eid eid;
} ename;

/* Sorted by name. */
static const ename ENAMES[] = {
    {"a", E_A},
    {"circle", E_CIRCLE},
    {"clipPath", E_CLIP_PATH},
    {"defs", E_DEFS},
    {"ellipse", E_ELLIPSE},
    {"feBlend", E_FE_BLEND},
    {"feColorMatrix", E_FE_COLOR_MATRIX},
    {"feComponentTransfer", E_FE_COMPONENT_TRANSFER},
    {"feComposite", E_FE_COMPOSITE},
    {"feConvolveMatrix", E_FE_CONVOLVE_MATRIX},
    {"feDiffuseLighting", E_FE_DIFFUSE_LIGHTING},
    {"feDisplacementMap", E_FE_DISPLACEMENT_MAP},
    {"feDistantLight", E_FE_DISTANT_LIGHT},
    {"feDropShadow", E_FE_DROP_SHADOW},
    {"feFlood", E_FE_FLOOD},
    {"feFuncA", E_FE_FUNC_A},
    {"feFuncB", E_FE_FUNC_B},
    {"feFuncG", E_FE_FUNC_G},
    {"feFuncR", E_FE_FUNC_R},
    {"feGaussianBlur", E_FE_GAUSSIAN_BLUR},
    {"feImage", E_FE_IMAGE},
    {"feMerge", E_FE_MERGE},
    {"feMergeNode", E_FE_MERGE_NODE},
    {"feMorphology", E_FE_MORPHOLOGY},
    {"feOffset", E_FE_OFFSET},
    {"fePointLight", E_FE_POINT_LIGHT},
    {"feSpecularLighting", E_FE_SPECULAR_LIGHTING},
    {"feSpotLight", E_FE_SPOT_LIGHT},
    {"feTile", E_FE_TILE},
    {"feTurbulence", E_FE_TURBULENCE},
    {"filter", E_FILTER},
    {"g", E_G},
    {"image", E_IMAGE},
    {"line", E_LINE},
    {"linearGradient", E_LINEAR_GRADIENT},
    {"marker", E_MARKER},
    {"mask", E_MASK},
    {"path", E_PATH},
    {"pattern", E_PATTERN},
    {"polygon", E_POLYGON},
    {"polyline", E_POLYLINE},
    {"radialGradient", E_RADIAL_GRADIENT},
    {"rect", E_RECT},
    {"stop", E_STOP},
    {"style", E_STYLE},
    {"svg", E_SVG},
    {"switch", E_SWITCH},
    {"symbol", E_SYMBOL},
    {"text", E_TEXT},
    {"textPath", E_TEXT_PATH},
    {"tref", E_TREF},
    {"tspan", E_TSPAN},
    {"use", E_USE},
};

typedef struct aname {
    const char *name;
    sv_aid aid;
} aname;

/* Sorted by name (strcmp). */
static const aname ANAMES[] = {
    {"alignment-baseline", A_ALIGNMENT_BASELINE},
    {"amplitude", A_AMPLITUDE},
    {"azimuth", A_AZIMUTH},
    {"background-color", A_BACKGROUND_COLOR},
    {"baseFrequency", A_BASE_FREQUENCY},
    {"baseline-shift", A_BASELINE_SHIFT},
    {"bias", A_BIAS},
    {"class", A_CLASS},
    {"clip-path", A_CLIP_PATH},
    {"clip-rule", A_CLIP_RULE},
    {"clipPathUnits", A_CLIP_PATH_UNITS},
    {"color", A_COLOR},
    {"color-interpolation", A_COLOR_INTERPOLATION},
    {"color-interpolation-filters", A_COLOR_INTERPOLATION_FILTERS},
    {"color-rendering", A_COLOR_RENDERING},
    {"cx", A_CX},
    {"cy", A_CY},
    {"d", A_D},
    {"diffuseConstant", A_DIFFUSE_CONSTANT},
    {"direction", A_DIRECTION},
    {"display", A_DISPLAY},
    {"divisor", A_DIVISOR},
    {"dominant-baseline", A_DOMINANT_BASELINE},
    {"dx", A_DX},
    {"dy", A_DY},
    {"edgeMode", A_EDGE_MODE},
    {"elevation", A_ELEVATION},
    {"exponent", A_EXPONENT},
    {"fill", A_FILL},
    {"fill-opacity", A_FILL_OPACITY},
    {"fill-rule", A_FILL_RULE},
    {"filter", A_FILTER},
    {"filterUnits", A_FILTER_UNITS},
    {"flood-color", A_FLOOD_COLOR},
    {"flood-opacity", A_FLOOD_OPACITY},
    {"font", A_FONT},
    {"font-family", A_FONT_FAMILY},
    {"font-kerning", A_FONT_KERNING},
    {"font-size", A_FONT_SIZE},
    {"font-size-adjust", A_FONT_SIZE_ADJUST},
    {"font-stretch", A_FONT_STRETCH},
    {"font-style", A_FONT_STYLE},
    {"font-variant", A_FONT_VARIANT},
    {"font-variant-caps", A_FONT_VARIANT_CAPS},
    {"font-variant-east-asian", A_FONT_VARIANT_EAST_ASIAN},
    {"font-variant-ligatures", A_FONT_VARIANT_LIGATURES},
    {"font-variant-numeric", A_FONT_VARIANT_NUMERIC},
    {"font-variant-position", A_FONT_VARIANT_POSITION},
    {"font-weight", A_FONT_WEIGHT},
    {"fx", A_FX},
    {"fy", A_FY},
    {"glyph-orientation-horizontal", A_GLYPH_ORIENTATION_HORIZONTAL},
    {"glyph-orientation-vertical", A_GLYPH_ORIENTATION_VERTICAL},
    {"gradientTransform", A_GRADIENT_TRANSFORM},
    {"gradientUnits", A_GRADIENT_UNITS},
    {"height", A_HEIGHT},
    {"href", A_HREF},
    {"id", A_ID},
    {"image-rendering", A_IMAGE_RENDERING},
    {"in", A_IN},
    {"in2", A_IN2},
    {"intercept", A_INTERCEPT},
    {"isolation", A_ISOLATION},
    {"k1", A_K1},
    {"k2", A_K2},
    {"k3", A_K3},
    {"k4", A_K4},
    {"kernelMatrix", A_KERNEL_MATRIX},
    {"kerning", A_KERNING},
    {"lengthAdjust", A_LENGTH_ADJUST},
    {"letter-spacing", A_LETTER_SPACING},
    {"lighting-color", A_LIGHTING_COLOR},
    {"limitingConeAngle", A_LIMITING_CONE_ANGLE},
    {"line-height", A_LINE_HEIGHT},
    {"marker-end", A_MARKER_END},
    {"marker-mid", A_MARKER_MID},
    {"marker-start", A_MARKER_START},
    {"markerHeight", A_MARKER_HEIGHT},
    {"markerUnits", A_MARKER_UNITS},
    {"markerWidth", A_MARKER_WIDTH},
    {"mask", A_MASK},
    {"mask-type", A_MASK_TYPE},
    {"maskContentUnits", A_MASK_CONTENT_UNITS},
    {"maskUnits", A_MASK_UNITS},
    {"mix-blend-mode", A_MIX_BLEND_MODE},
    {"mode", A_MODE},
    {"numOctaves", A_NUM_OCTAVES},
    {"offset", A_OFFSET},
    {"opacity", A_OPACITY},
    {"operator", A_OPERATOR},
    {"order", A_ORDER},
    {"orient", A_ORIENT},
    {"overflow", A_OVERFLOW},
    {"paint-order", A_PAINT_ORDER},
    {"patternContentUnits", A_PATTERN_CONTENT_UNITS},
    {"patternTransform", A_PATTERN_TRANSFORM},
    {"patternUnits", A_PATTERN_UNITS},
    {"points", A_POINTS},
    {"pointsAtX", A_POINTS_AT_X},
    {"pointsAtY", A_POINTS_AT_Y},
    {"pointsAtZ", A_POINTS_AT_Z},
    {"preserveAlpha", A_PRESERVE_ALPHA},
    {"preserveAspectRatio", A_PRESERVE_ASPECT_RATIO},
    {"primitiveUnits", A_PRIMITIVE_UNITS},
    {"r", A_R},
    {"radius", A_RADIUS},
    {"refX", A_REF_X},
    {"refY", A_REF_Y},
    {"requiredExtensions", A_REQUIRED_EXTENSIONS},
    {"requiredFeatures", A_REQUIRED_FEATURES},
    {"result", A_RESULT},
    {"rotate", A_ROTATE},
    {"rx", A_RX},
    {"ry", A_RY},
    {"scale", A_SCALE},
    {"seed", A_SEED},
    {"shape-rendering", A_SHAPE_RENDERING},
    {"slope", A_SLOPE},
    {"space", A_SPACE},
    {"specularConstant", A_SPECULAR_CONSTANT},
    {"specularExponent", A_SPECULAR_EXPONENT},
    {"spreadMethod", A_SPREAD_METHOD},
    {"startOffset", A_START_OFFSET},
    {"stdDeviation", A_STD_DEVIATION},
    {"stitchTiles", A_STITCH_TILES},
    {"stop-color", A_STOP_COLOR},
    {"stop-opacity", A_STOP_OPACITY},
    {"stroke", A_STROKE},
    {"stroke-dasharray", A_STROKE_DASHARRAY},
    {"stroke-dashoffset", A_STROKE_DASHOFFSET},
    {"stroke-linecap", A_STROKE_LINECAP},
    {"stroke-linejoin", A_STROKE_LINEJOIN},
    {"stroke-miterlimit", A_STROKE_MITERLIMIT},
    {"stroke-opacity", A_STROKE_OPACITY},
    {"stroke-width", A_STROKE_WIDTH},
    {"style", A_STYLE},
    {"surfaceScale", A_SURFACE_SCALE},
    {"systemLanguage", A_SYSTEM_LANGUAGE},
    {"tableValues", A_TABLE_VALUES},
    {"targetX", A_TARGET_X},
    {"targetY", A_TARGET_Y},
    {"text-anchor", A_TEXT_ANCHOR},
    {"text-decoration", A_TEXT_DECORATION},
    {"text-overflow", A_TEXT_OVERFLOW},
    {"text-rendering", A_TEXT_RENDERING},
    {"textLength", A_TEXT_LENGTH},
    {"transform", A_TRANSFORM},
    {"transform-origin", A_TRANSFORM_ORIGIN},
    {"type", A_TYPE},
    {"unicode-bidi", A_UNICODE_BIDI},
    {"values", A_VALUES},
    {"vector-effect", A_VECTOR_EFFECT},
    {"viewBox", A_VIEW_BOX},
    {"visibility", A_VISIBILITY},
    {"white-space", A_WHITE_SPACE},
    {"width", A_WIDTH},
    {"word-spacing", A_WORD_SPACING},
    {"writing-mode", A_WRITING_MODE},
    {"x", A_X},
    {"x1", A_X1},
    {"x2", A_X2},
    {"xChannelSelector", A_X_CHANNEL_SELECTOR},
    {"y", A_Y},
    {"y1", A_Y1},
    {"y2", A_Y2},
    {"yChannelSelector", A_Y_CHANNEL_SELECTOR},
    {"z", A_Z},
};

static int ename_cmp(const void *k, const void *e)
{
    return strcmp((const char *)k, ((const ename *)e)->name);
}

static int aname_cmp(const void *k, const void *e)
{
    return strcmp((const char *)k, ((const aname *)e)->name);
}

sv_eid sv_eid_from(const char *name)
{
    const ename *e = bsearch(name, ENAMES, sizeof ENAMES / sizeof ENAMES[0], sizeof ENAMES[0], ename_cmp);
    return e != NULL ? e->eid : E_NONE;
}

sv_aid sv_aid_from(const char *name)
{
    const aname *e = bsearch(name, ANAMES, sizeof ANAMES / sizeof ANAMES[0], sizeof ANAMES[0], aname_cmp);
    return e != NULL ? e->aid : A_NONE;
}

const char *sv_aid_name(sv_aid a)
{
    size_t i;
    for (i = 0; i < sizeof ANAMES / sizeof ANAMES[0]; i++) {
        if (ANAMES[i].aid == a) {
            return ANAMES[i].name;
        }
    }
    return "";
}

bool sv_aid_is_presentation(sv_aid a)
{
    return a >= A_ALIGNMENT_BASELINE && a <= A_LAST_PRESENTATION;
}

bool sv_aid_is_inheritable(sv_aid a)
{
    if (!sv_aid_is_presentation(a)) {
        return false;
    }
    switch (a) {
    case A_ALIGNMENT_BASELINE:
    case A_BASELINE_SHIFT:
    case A_CLIP_PATH:
    case A_DISPLAY:
    case A_DOMINANT_BASELINE:
    case A_FILTER:
    case A_FLOOD_COLOR:
    case A_FLOOD_OPACITY:
    case A_MASK:
    case A_OPACITY:
    case A_OVERFLOW:
    case A_LIGHTING_COLOR:
    case A_STOP_COLOR:
    case A_STOP_OPACITY:
    case A_TEXT_DECORATION:
    case A_TRANSFORM:
    case A_TRANSFORM_ORIGIN:
        return false;
    default:
        return true;
    }
}

bool sv_aid_allows_inherit(sv_aid a)
{
    switch (a) {
    case A_ALIGNMENT_BASELINE:
    case A_BASELINE_SHIFT:
    case A_CLIP_PATH:
    case A_CLIP_RULE:
    case A_COLOR:
    case A_COLOR_INTERPOLATION_FILTERS:
    case A_DIRECTION:
    case A_DISPLAY:
    case A_DOMINANT_BASELINE:
    case A_FILL:
    case A_FILL_OPACITY:
    case A_FILL_RULE:
    case A_FILTER:
    case A_FLOOD_COLOR:
    case A_FLOOD_OPACITY:
    case A_FONT_FAMILY:
    case A_FONT_KERNING:
    case A_FONT_SIZE:
    case A_FONT_STRETCH:
    case A_FONT_STYLE:
    case A_FONT_VARIANT:
    case A_FONT_WEIGHT:
    case A_IMAGE_RENDERING:
    case A_KERNING:
    case A_LETTER_SPACING:
    case A_MARKER_END:
    case A_MARKER_MID:
    case A_MARKER_START:
    case A_MASK:
    case A_OPACITY:
    case A_OVERFLOW:
    case A_SHAPE_RENDERING:
    case A_STOP_COLOR:
    case A_STOP_OPACITY:
    case A_STROKE:
    case A_STROKE_DASHARRAY:
    case A_STROKE_DASHOFFSET:
    case A_STROKE_LINECAP:
    case A_STROKE_LINEJOIN:
    case A_STROKE_MITERLIMIT:
    case A_STROKE_OPACITY:
    case A_STROKE_WIDTH:
    case A_TEXT_ANCHOR:
    case A_TEXT_DECORATION:
    case A_TEXT_RENDERING:
    case A_VISIBILITY:
    case A_WORD_SPACING:
    case A_WRITING_MODE:
        return true;
    default:
        return false;
    }
}

bool sv_eid_is_graphic(sv_eid e)
{
    switch (e) {
    case E_CIRCLE:
    case E_ELLIPSE:
    case E_IMAGE:
    case E_LINE:
    case E_PATH:
    case E_POLYGON:
    case E_POLYLINE:
    case E_RECT:
    case E_TEXT:
    case E_USE:
        return true;
    default:
        return false;
    }
}

/* ---------------------------------------------------------------- access */

const char *st_attr(const st_node *n, sv_aid aid)
{
    size_t i;
    for (i = 0; n != NULL && i < n->nattrs; i++) {
        if (n->attrs[i].aid == aid) {
            return n->attrs[i].value;
        }
    }
    return NULL;
}

bool st_has(const st_node *n, sv_aid aid)
{
    return st_attr(n, aid) != NULL;
}

const char *st_id(const st_node *n)
{
    const char *id = st_attr(n, A_ID);
    return id != NULL ? id : "";
}

static int link_cmp(const void *k, const void *e)
{
    return strcmp((const char *)k, ((const st_link *)e)->id);
}

st_node *st_by_id(const st_doc *d, const char *id, size_t len)
{
    char buf[256];
    const st_link *l;
    if (len >= sizeof buf) {
        /* Ids this long are compared without the stack copy. */
        size_t i;
        for (i = 0; i < d->nlinks; i++) {
            if (strlen(d->links[i].id) == len && memcmp(d->links[i].id, id, len) == 0) {
                return d->links[i].node;
            }
        }
        return NULL;
    }
    if (d->nlinks == 0) {
        return NULL;
    }
    memcpy(buf, id, len);
    buf[len] = '\0';
    l = bsearch(buf, d->links, d->nlinks, sizeof(st_link), link_cmp);
    return l != NULL ? l->node : NULL;
}

const st_node *st_parent_element(const st_node *n)
{
    const st_node *p = n->parent;
    while (p != NULL && p->kind != ST_ELEMENT) {
        p = p->parent;
    }
    return p;
}

const st_node *st_first_element(const st_node *n)
{
    const st_node *c = n->first;
    while (c != NULL && c->kind != ST_ELEMENT) {
        c = c->next;
    }
    return c;
}

const st_node *st_find_attr_node(const st_node *n, sv_aid aid)
{
    if (sv_aid_is_inheritable(aid)) {
        const st_node *p;
        for (p = n; p != NULL; p = p->parent) {
            if (st_has(p, aid)) {
                return p;
            }
        }
        return NULL;
    }
    if (st_has(n, aid)) {
        return n;
    }
    {
        const st_node *p = st_parent_element(n);
        return p != NULL && st_has(p, aid) ? p : NULL;
    }
}

st_node *st_node_attr(const st_doc *d, const st_node *n, sv_aid aid)
{
    const char *v = st_attr(n, aid);
    const char *id;
    size_t len;
    if (v == NULL) {
        return NULL;
    }
    if (aid == A_HREF ? !sv_parse_iri(v, &id, &len) : !sv_parse_func_iri(v, &id, &len)) {
        return NULL;
    }
    return st_by_id(d, id, len);
}

/* ---------------------------------------------------------------- building */

#define MAX_NODES 1000000u
#define MAX_DEPTH 1024u

typedef struct xid {
    const char *id;
    const sx_node *node;
    size_t order;
} xid;

typedef struct st_builder {
    sv_arena *a;
    st_doc *doc;
    sc_sheet sheet;
    xid *ids; /* first occurrence of every XML id, sorted */
    size_t nids;
    char *msg;
    size_t cap;
    bool limit;
    size_t tref_budget; /* bytes of <tref> text left (svg_text_tree.c) */
} builder;

static bool tag_of(const sx_node *x, sv_eid *out)
{
    sv_eid e;
    if (x->is_text || x->parent == NULL) {
        return false;
    }
    if (x->ns != NULL && strcmp(x->ns, SV_NS_SVG) != 0) {
        return false;
    }
    e = sv_eid_from(x->name);
    if (e == E_NONE) {
        return false;
    }
    *out = e;
    return true;
}

static st_node *append_node(builder *b, st_node *parent, sv_eid eid)
{
    st_node *n;
    if (b->doc->nodes >= MAX_NODES) {
        b->limit = true;
        return NULL;
    }
    n = sv_alloc(b->a, sizeof *n);
    if (n == NULL) {
        return NULL;
    }
    n->kind = ST_ELEMENT;
    n->eid = eid;
    n->parent = parent;
    if (parent->last != NULL) {
        parent->last->next = n;
    } else {
        parent->first = n;
    }
    parent->last = n;
    b->doc->nodes++;
    return n;
}

static bool push_attr(builder *b, st_node *n, sv_aid aid, const char *value, bool important)
{
    void *v = n->attrs;
    if (!sv_grow(b->a, &v, &n->capattrs, n->nattrs + 1, sizeof(st_attrv))) {
        return false;
    }
    n->attrs = v;
    n->attrs[n->nattrs].aid = aid;
    n->attrs[n->nattrs].value = value;
    n->attrs[n->nattrs].important = important;
    n->nattrs++;
    return true;
}

static const char *inherit_default(sv_aid aid)
{
    switch (aid) {
    case A_IMAGE_RENDERING:
    case A_SHAPE_RENDERING:
    case A_TEXT_RENDERING:
        return "auto";
    case A_CLIP_PATH:
    case A_FILTER:
    case A_MARKER_END:
    case A_MARKER_MID:
    case A_MARKER_START:
    case A_MASK:
    case A_STROKE:
    case A_STROKE_DASHARRAY:
    case A_TEXT_DECORATION:
        return "none";
    case A_FONT_STRETCH:
    case A_FONT_STYLE:
    case A_FONT_VARIANT:
    case A_FONT_WEIGHT:
    case A_LETTER_SPACING:
    case A_WORD_SPACING:
        return "normal";
    case A_FILL:
    case A_FLOOD_COLOR:
    case A_STOP_COLOR:
        return "black";
    case A_FILL_OPACITY:
    case A_FLOOD_OPACITY:
    case A_OPACITY:
    case A_STOP_OPACITY:
    case A_STROKE_OPACITY:
        return "1";
    case A_CLIP_RULE:
    case A_FILL_RULE:
        return "nonzero";
    case A_BASELINE_SHIFT:
        return "baseline";
    case A_COLOR_INTERPOLATION_FILTERS:
        return "linearRGB";
    case A_DIRECTION:
        return "ltr";
    case A_DISPLAY:
        return "inline";
    case A_FONT_SIZE:
        return "medium";
    case A_OVERFLOW:
        return "visible";
    case A_STROKE_DASHOFFSET:
        return "0";
    case A_STROKE_LINECAP:
        return "butt";
    case A_STROKE_LINEJOIN:
        return "miter";
    case A_STROKE_MITERLIMIT:
        return "4";
    case A_STROKE_WIDTH:
        return "1";
    case A_TEXT_ANCHOR:
        return "start";
    case A_VISIBILITY:
        return "visible";
    case A_WRITING_MODE:
        return "lr-tb";
    default:
        return NULL;
    }
}

/* resolve_inherit: *added false when nothing was appended. */
static bool resolve_inherit(builder *b, st_node *elem, const st_node *parent, sv_aid aid, bool *added)
{
    const char *d;
    *added = false;
    if (sv_aid_is_inheritable(aid)) {
        const st_node *p;
        for (p = parent; p != NULL; p = p->parent) {
            size_t i;
            for (i = 0; i < p->nattrs; i++) {
                if (p->attrs[i].aid == aid) {
                    *added = true;
                    return push_attr(b, elem, aid, p->attrs[i].value, p->attrs[i].important);
                }
            }
        }
    } else {
        size_t i;
        for (i = 0; parent != NULL && i < parent->nattrs; i++) {
            if (parent->attrs[i].aid == aid) {
                *added = true;
                return push_attr(b, elem, aid, parent->attrs[i].value, parent->attrs[i].important);
            }
        }
    }
    d = inherit_default(aid);
    if (d == NULL) {
        return true;
    }
    *added = true;
    return push_attr(b, elem, aid, d, false);
}

/* append_attribute; *added as the reference's bool. */
static bool append_attribute(builder *b, st_node *elem, const st_node *parent, sv_eid tag, sv_aid aid,
                             const char *value, bool important, bool *added)
{
    *added = false;
    if (aid == A_STYLE || aid == A_CLASS) {
        return true;
    }
    if (tag == E_TSPAN && aid == A_HREF) {
        return true;
    }
    if (sv_aid_allows_inherit(aid) && strcmp(value, "inherit") == 0) {
        return resolve_inherit(b, elem, parent, aid, added);
    }
    *added = true;
    return push_attr(b, elem, aid, value, important);
}

static bool insert_attribute(builder *b, st_node *elem, const st_node *parent, sv_eid tag, sv_aid aid,
                             const char *value, bool important)
{
    size_t idx = SIZE_MAX;
    size_t i;
    bool added;
    for (i = 0; i < elem->nattrs; i++) {
        if (elem->attrs[i].aid == aid) {
            idx = i;
            break;
        }
    }
    if (!append_attribute(b, elem, parent, tag, aid, value, important, &added)) {
        return false;
    }
    if (added && idx != SIZE_MAX) {
        size_t last = elem->nattrs - 1;
        /* An important existing value keeps precedence; otherwise the new one replaces it. */
        if (!elem->attrs[idx].important) {
            elem->attrs[idx] = elem->attrs[last];
        }
        elem->nattrs--;
    }
    return true;
}

static bool write_decl(builder *b, st_node *elem, const st_node *parent, sv_eid tag, const sc_decl *d)
{
    if (strcmp(d->name, "marker") == 0) {
        return insert_attribute(b, elem, parent, tag, A_MARKER_START, d->value, d->important) &&
               insert_attribute(b, elem, parent, tag, A_MARKER_MID, d->value, d->important) &&
               insert_attribute(b, elem, parent, tag, A_MARKER_END, d->value, d->important);
    }
    if (strcmp(d->name, "font") == 0) {
        /* svgtypes::FontShorthand: reset every font property, then set the declared ones. */
        sv_font_shorthand f;
        bool imp = d->important;
        if (!sv_parse_font_shorthand(b->a, d->value, &f)) {
            return !b->a->nomem;
        }
        return insert_attribute(b, elem, parent, tag, A_FONT_STYLE, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_VARIANT, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_WEIGHT, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_STRETCH, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_LINE_HEIGHT, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_SIZE_ADJUST, "none", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_KERNING, "auto", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_VARIANT_CAPS, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_VARIANT_LIGATURES, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_VARIANT_NUMERIC, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_VARIANT_EAST_ASIAN, "normal", imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_VARIANT_POSITION, "normal", imp) &&
               (f.stretch == NULL ||
                insert_attribute(b, elem, parent, tag, A_FONT_STRETCH, f.stretch, imp)) &&
               (f.weight == NULL || insert_attribute(b, elem, parent, tag, A_FONT_WEIGHT, f.weight, imp)) &&
               (f.variant == NULL ||
                insert_attribute(b, elem, parent, tag, A_FONT_VARIANT, f.variant, imp)) &&
               (f.style == NULL || insert_attribute(b, elem, parent, tag, A_FONT_STYLE, f.style, imp)) &&
               insert_attribute(b, elem, parent, tag, A_FONT_SIZE, f.size, imp) &&
               insert_attribute(b, elem, parent, tag, A_FONT_FAMILY, f.family, imp);
    }
    {
        sv_aid aid = sv_aid_from(d->name);
        if (aid != A_NONE && sv_aid_is_presentation(aid)) {
            return insert_attribute(b, elem, parent, tag, aid, d->value, d->important);
        }
    }
    return true;
}

static st_node *parse_element(builder *b, const sx_node *x, st_node *parent, sv_eid tag, bool ignore_ids)
{
    st_node *n = append_node(b, parent, tag);
    size_t i;
    const char *style;
    if (n == NULL) {
        return NULL;
    }
    for (i = 0; i < x->nattrs; i++) {
        const sx_attr *at = &x->attrs[i];
        sv_aid aid;
        bool added;
        if (at->ns != NULL && strcmp(at->ns, SV_NS_SVG) != 0 && strcmp(at->ns, SV_NS_XLINK) != 0 &&
            strcmp(at->ns, SV_NS_XML) != 0) {
            continue;
        }
        aid = sv_aid_from(at->name);
        if (aid == A_NONE || (ignore_ids && aid == A_ID)) {
            continue;
        }
        if (aid == A_MIX_BLEND_MODE || aid == A_ISOLATION || aid == A_FONT_KERNING) {
            continue;
        }
        if (aid == A_IMAGE_RENDERING &&
            (strcmp(at->value, "smooth") == 0 || strcmp(at->value, "high-quality") == 0 ||
             strcmp(at->value, "crisp-edges") == 0 || strcmp(at->value, "pixelated") == 0)) {
            continue;
        }
        if (!append_attribute(b, n, parent, tag, aid, at->value, false, &added)) {
            return NULL;
        }
    }
    for (i = 0; i < b->sheet.n; i++) {
        if (sc_rule_matches(b->sheet.rules[i], x)) {
            size_t nd;
            size_t k;
            const sc_decl *d = sc_rule_decls(b->sheet.rules[i], &nd);
            for (k = 0; k < nd; k++) {
                if (!write_decl(b, n, parent, tag, &d[k])) {
                    return NULL;
                }
            }
        }
    }
    style = sx_attr_get(x, NULL, "style");
    if (style != NULL) {
        sc_decl *d;
        size_t nd;
        size_t k;
        if (!sc_parse_decls(b->a, style, &d, &nd)) {
            return NULL;
        }
        for (k = 0; k < nd; k++) {
            if (!write_decl(b, n, parent, tag, &d[k])) {
                return NULL;
            }
        }
    }
    return n;
}

static int xid_cmp(const void *a, const void *b)
{
    return strcmp(((const xid *)a)->id, ((const xid *)b)->id);
}

/* By id, then document order (so the first occurrence sorts first). */
static int xid_order_cmp(const void *a, const void *b)
{
    const xid *x = a;
    const xid *y = b;
    int c = strcmp(x->id, y->id);
    if (c != 0) {
        return c;
    }
    return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

static const sx_node *resolve_href(const builder *b, const sx_node *x)
{
    const char *v = sx_attr_get(x, SV_NS_XLINK, "href");
    const char *id;
    size_t len;
    char buf[256];
    xid key;
    const xid *r;
    if (v == NULL) {
        v = sx_attr_get(x, NULL, "href");
    }
    if (v == NULL || b->nids == 0 || !sv_parse_iri(v, &id, &len) || len >= sizeof buf) {
        return NULL;
    }
    memcpy(buf, id, len);
    buf[len] = '\0';
    key.id = buf;
    key.node = NULL;
    r = bsearch(&key, b->ids, b->nids, sizeof(xid), xid_cmp);
    return r != NULL ? r->node : NULL;
}

/* ---- hooks for svg_text_tree.c (svg_internal.h, text section) */

st_node *st_build_element(st_builder *b, const sx_node *x, st_node *parent, sv_eid tag)
{
    return parse_element(b, x, parent, tag, false);
}

st_node *st_build_text(st_builder *b, st_node *parent, const char *text)
{
    st_node *n = append_node(b, parent, E_NONE);
    if (n != NULL) {
        n->kind = ST_TEXT;
        n->text = text;
    }
    return n;
}

const sx_node *st_build_xml_by_id(const st_builder *b, const char *id, size_t len)
{
    char buf[256];
    xid key;
    const xid *r;
    if (b->nids == 0 || len >= sizeof buf) {
        return NULL;
    }
    memcpy(buf, id, len);
    buf[len] = '\0';
    key.id = buf;
    key.node = NULL;
    key.order = 0;
    r = bsearch(&key, b->ids, b->nids, sizeof(xid), xid_cmp);
    return r != NULL ? r->node : NULL;
}

bool st_build_tag(const sx_node *x, sv_eid *out)
{
    return tag_of(x, out);
}

sv_arena *st_build_arena(st_builder *b)
{
    return b->a;
}

size_t *st_build_tref_budget(st_builder *b)
{
    return &b->tref_budget;
}

/* An explicit work stack instead of the reference's recursion (depth up to 1024 and <use>
 * expansion would otherwise use the C stack). */
typedef struct frame {
    const sx_node *x; /* next XML child to visit */
    st_node *parent;
    const sx_node *origin; /* the <use> being expanded, or the document */
    bool ignore_ids;
    size_t depth;
    bool single; /* visit only x itself (a <use> target), not its siblings */
} frame;

typedef struct stack {
    frame *v;
    size_t n, cap;
} stack;

static bool push_frame(builder *b, stack *s, frame f)
{
    void *v = s->v;
    if (!sv_grow(b->a, &v, &s->cap, s->n + 1, sizeof(frame))) {
        return false;
    }
    s->v = v;
    s->v[s->n++] = f;
    return true;
}

static bool is_svg_use(const sx_node *x)
{
    return !x->is_text && x->ns != NULL && strcmp(x->ns, SV_NS_SVG) == 0 && strcmp(x->name, "use") == 0;
}

/* Some <use> inside link (not link itself) refers back to node or link. */
static bool use_is_recursive(const builder *b, const sx_node *link, const sx_node *node)
{
    const sx_node *c = link->first;
    size_t budget = MAX_NODES;
    while (c != NULL && budget-- > 0) {
        if (is_svg_use(c)) {
            const sx_node *l2 = resolve_href(b, c);
            if (l2 == node || l2 == link) {
                return true;
            }
        }
        if (c->first != NULL) {
            c = c->first;
            continue;
        }
        while (c != NULL && c != link && c->next == NULL) {
            c = c->parent;
        }
        if (c == NULL || c == link) {
            break;
        }
        c = c->next;
    }
    return false;
}

static bool visit(builder *b, stack *s, const sx_node *x, frame *f)
{
    sv_eid tag;
    st_node *n;
    if (f->depth > MAX_DEPTH) {
        b->limit = true;
        return false;
    }
    if (!tag_of(x, &tag) || tag == E_STYLE) {
        return true;
    }
    if (tag == E_A) {
        tag = E_G;
    }
    n = parse_element(b, x, f->parent, tag, f->ignore_ids);
    if (n == NULL) {
        return false;
    }
    if (tag == E_USE) {
        const sx_node *link = resolve_href(b, x);
        sv_eid lt;
        frame nf;
        if (link == NULL || link == x || link == f->origin || !tag_of(link, &lt) ||
            use_is_recursive(b, link, x)) {
            return true;
        }
        nf.x = link;
        nf.parent = n;
        nf.origin = x;
        nf.ignore_ids = true;
        nf.depth = f->depth + 2;
        nf.single = true;
        return push_frame(b, s, nf);
    }
    if (tag == E_TEXT) {
        return st_text_parse(b, x, n);
    }
    if (x->first != NULL) {
        frame nf;
        nf.x = x->first;
        nf.parent = n;
        nf.origin = f->origin;
        nf.ignore_ids = f->ignore_ids;
        nf.depth = f->depth + 1;
        nf.single = false;
        return push_frame(b, s, nf);
    }
    return true;
}

static bool build_nodes(builder *b, const sx_node *docnode)
{
    stack s = {NULL, 0, 0};
    frame f;
    f.x = docnode->first;
    f.parent = b->doc->root;
    f.origin = docnode;
    f.ignore_ids = false;
    f.depth = 0;
    f.single = false;
    if (!push_frame(b, &s, f)) {
        return false;
    }
    while (s.n > 0) {
        frame *top = &s.v[s.n - 1];
        frame cur;
        if (top->x == NULL) {
            s.n--;
            continue;
        }
        cur = *top;
        top->x = top->single ? NULL : top->x->next;
        if (!visit(b, &s, cur.x, &cur)) {
            return false;
        }
    }
    return true;
}

static bool collect_ids(builder *b, const sx_node *docnode)
{
    const sx_node *c = docnode->first;
    size_t cap = 0;
    size_t i;
    size_t k;
    while (c != NULL) {
        if (!c->is_text) {
            const char *id = sx_attr_get(c, NULL, "id");
            if (id != NULL) {
                void *v = b->ids;
                if (!sv_grow(b->a, &v, &cap, b->nids + 1, sizeof(xid))) {
                    return false;
                }
                b->ids = v;
                b->ids[b->nids].id = id;
                b->ids[b->nids].node = c;
                b->ids[b->nids].order = b->nids;
                b->nids++;
            }
        }
        if (c->first != NULL) {
            c = c->first;
            continue;
        }
        while (c != NULL && c->next == NULL) {
            c = c->parent;
        }
        if (c == NULL) {
            break;
        }
        c = c->next;
    }
    /* Keep the first occurrence of each id, as the reference's map does. */
    if (b->nids > 1) {
        qsort(b->ids, b->nids, sizeof(xid), xid_order_cmp);
    }
    for (i = 0, k = 0; i < b->nids; i++) {
        if (k > 0 && strcmp(b->ids[k - 1].id, b->ids[i].id) == 0) {
            continue;
        }
        b->ids[k++] = b->ids[i];
    }
    b->nids = k;
    return true;
}

static bool collect_css(builder *b, const sx_node *docnode)
{
    const sx_node *c = docnode->first;
    while (c != NULL) {
        if (!c->is_text && strcmp(c->name, "style") == 0) {
            const char *type = sx_attr_get(c, NULL, "type");
            const char *text = sx_text(c);
            if ((type == NULL || strcmp(type, "text/css") == 0) && text != NULL &&
                !sc_parse_more(b->a, &b->sheet, text)) {
                return false;
            }
        }
        if (c->first != NULL) {
            c = c->first;
            continue;
        }
        while (c != NULL && c->next == NULL) {
            c = c->parent;
        }
        if (c == NULL) {
            break;
        }
        c = c->next;
    }
    return true;
}

/* Pre-order traversal of the svgtree below root (root excluded). */
static st_node *next_desc(const st_node *root, st_node *c)
{
    if (c->first != NULL) {
        return c->first;
    }
    while (c != NULL && c != root && c->next == NULL) {
        c = c->parent;
    }
    if (c == NULL || c == root) {
        return NULL;
    }
    return c->next;
}

typedef struct link_tmp {
    st_link l;
    size_t order;
} link_tmp;

static int link_tmp_cmp(const void *a, const void *b)
{
    const link_tmp *x = a;
    const link_tmp *y = b;
    int c = strcmp(x->l.id, y->l.id);
    if (c != 0) {
        return c;
    }
    return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

/* Every element with an id; a later duplicate replaces an earlier one (HashMap::insert). */
static bool collect_links(builder *b)
{
    st_doc *d = b->doc;
    link_tmp *t = NULL;
    size_t n = 0;
    size_t cap = 0;
    size_t i;
    size_t k;
    st_node *c;
    for (c = d->root->first; c != NULL; c = next_desc(d->root, c)) {
        const char *id = st_attr(c, A_ID);
        if (id != NULL) {
            void *v = t;
            if (!sv_grow(b->a, &v, &cap, n + 1, sizeof(link_tmp))) {
                return false;
            }
            t = v;
            t[n].l.id = id;
            t[n].l.node = c;
            t[n].order = n;
            n++;
        }
    }
    if (n > 1) {
        qsort(t, n, sizeof(link_tmp), link_tmp_cmp);
    }
    d->links = sv_alloc_array(b->a, n > 0 ? n : 1, sizeof(st_link));
    if (d->links == NULL) {
        return false;
    }
    for (i = 0, k = 0; i < n; i++) {
        if (i + 1 < n && strcmp(t[i].l.id, t[i + 1].l.id) == 0) {
            continue; /* keep the last occurrence */
        }
        d->links[k++] = t[i].l;
    }
    d->nlinks = k;
    return true;
}

static st_attrv *attr_slot(st_node *n, sv_aid aid)
{
    size_t i;
    for (i = 0; i < n->nattrs; i++) {
        if (n->attrs[i].aid == aid) {
            return &n->attrs[i];
        }
    }
    return NULL;
}

static bool paint_iri(const char *v, const char **id, size_t *len)
{
    sv_spaint p;
    if (v == NULL || !sv_parse_paint(v, &p) || p.kind != P_FUNC_IRI) {
        return false;
    }
    *id = p.iri;
    *len = p.iri_len;
    return true;
}

static bool id_is(const st_node *n, const char *id, size_t len)
{
    const char *e = st_id(n);
    return strlen(e) == len && memcmp(e, id, len) == 0;
}

/* A node whose fill/stroke leads back to the pattern containing it, or NULL. */
static st_node *find_recursive_pattern(const st_doc *d, sv_aid aid, size_t *budget)
{
    st_node *pat;
    for (pat = d->root->first; pat != NULL; pat = next_desc(d->root, pat)) {
        st_node *n;
        if (pat->eid != E_PATTERN) {
            continue;
        }
        for (n = pat; n != NULL; n = n == pat ? pat->first : next_desc(pat, n)) {
            const char *id;
            size_t len;
            st_node *linked;
            if (*budget == 0) {
                return NULL;
            }
            (*budget)--;
            if (!paint_iri(st_attr(n, aid), &id, &len)) {
                if (n == pat && pat->first == NULL) {
                    break;
                }
                continue;
            }
            if (id_is(pat, id, len)) {
                return n;
            }
            linked = st_by_id(d, id, len);
            if (linked != NULL) {
                st_node *n2;
                for (n2 = linked; n2 != NULL; n2 = n2 == linked ? linked->first : next_desc(linked, n2)) {
                    const char *id2;
                    size_t len2;
                    if (*budget == 0) {
                        return NULL;
                    }
                    (*budget)--;
                    if (paint_iri(st_attr(n2, aid), &id2, &len2) && id_is(pat, id2, len2)) {
                        return n2;
                    }
                    if (n2 == linked && linked->first == NULL) {
                        break;
                    }
                }
            }
            if (n == pat && pat->first == NULL) {
                break;
            }
        }
    }
    return NULL;
}

static st_node *find_recursive_link(const st_doc *d, sv_eid eid, sv_aid aid, size_t *budget)
{
    st_node *node;
    for (node = d->root->first; node != NULL; node = next_desc(d->root, node)) {
        st_node *child;
        if (node->eid != eid) {
            continue;
        }
        for (child = node; child != NULL; child = child == node ? node->first : next_desc(node, child)) {
            st_node *link;
            if (*budget == 0) {
                return NULL;
            }
            (*budget)--;
            link = st_node_attr(d, child, aid);
            if (link != NULL) {
                st_node *n2;
                if (link == node) {
                    return child;
                }
                for (n2 = link; n2 != NULL; n2 = n2 == link ? link->first : next_desc(link, n2)) {
                    if (*budget == 0) {
                        return NULL;
                    }
                    (*budget)--;
                    if (st_node_attr(d, n2, aid) == node) {
                        return n2;
                    }
                    if (n2 == link && link->first == NULL) {
                        break;
                    }
                }
            }
            if (child == node && node->first == NULL) {
                break;
            }
        }
    }
    return NULL;
}

/* Cuts reference cycles (pattern paints, clip paths, masks, filters) to "none". The search is
 * quadratic in the worst case, so it has a work budget; past it the converter's own recursion
 * limits still stop a cycle. */
static void fix_recursion(st_doc *d)
{
    static const sv_aid PAINTS[2] = {A_FILL, A_STROKE};
    static const struct {
        sv_eid e;
        sv_aid a;
    } LINKS[3] = {{E_CLIP_PATH, A_CLIP_PATH}, {E_MASK, A_MASK}, {E_FILTER, A_FILTER}};
    size_t budget = (size_t)50 * 1000 * 1000;
    size_t i;
    for (i = 0; i < 2; i++) {
        st_node *n;
        while ((n = find_recursive_pattern(d, PAINTS[i], &budget)) != NULL) {
            st_attrv *at = attr_slot(n, PAINTS[i]);
            if (at == NULL) {
                break;
            }
            at->value = "none";
        }
    }
    for (i = 0; i < 3; i++) {
        st_node *n;
        while ((n = find_recursive_link(d, LINKS[i].e, LINKS[i].a, &budget)) != NULL) {
            st_attrv *at = attr_slot(n, LINKS[i].a);
            if (at == NULL) {
                break;
            }
            at->value = "none";
        }
    }
}

pc_status st_build(sv_arena *a, const sx_doc *xml, st_doc *out, char *msg, size_t cap)
{
    builder b;
    st_doc d;
    const st_node *first;
    memset(&b, 0, sizeof b);
    memset(&d, 0, sizeof d);
    b.a = a;
    b.doc = &d;
    b.tref_budget = (size_t)16 << 20;
    b.msg = msg;
    b.cap = cap;
    d.root = sv_alloc(a, sizeof *d.root);
    if (d.root == NULL) {
        return PC_ERR_NOMEM;
    }
    d.root->kind = ST_ROOT;
    if (!collect_ids(&b, xml->root) || !collect_css(&b, xml->root)) {
        return PC_ERR_NOMEM;
    }
    if (!build_nodes(&b, xml->root)) {
        if (b.limit) {
            (void)snprintf(msg, cap, "nodes limit reached");
            return PC_ERR_FORMAT;
        }
        return PC_ERR_NOMEM;
    }
    first = st_first_element(d.root);
    if (first == NULL || first->eid != E_SVG) {
        (void)snprintf(msg, cap, "the document does not have a root node");
        return PC_ERR_FORMAT;
    }
    if (!collect_links(&b)) {
        return PC_ERR_NOMEM;
    }
    fix_recursion(&d);
    sv_fix_recursive_fe_image(&d);
    *out = d;
    return PC_OK;
}
