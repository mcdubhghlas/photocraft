/* Layer effects <-> PSD lfx2 (object-based effects descriptor) and legacy lrFX (port of
 * crates/io/src/effects_map.rs). Key names follow the descriptors Photoshop writes (documented
 * by reverse-engineering in MIT-licensed psd-tools / ag-psd). Unreadable data is "not found";
 * only out of memory is an error. */
#include "io/effects_map.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "io/blocks.h"
#include "io/desc_util.h"

#define TRY(x)                                                                                               \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto fail;                                                                                       \
        }                                                                                                    \
    } while (0)

typedef struct blend_name {
    pc_blend_mode mode;
    const char *id;
} blend_name;

static const blend_name BLEND_NAMES[28] = {
    {PC_BLEND_NORMAL, "Nrml"},
    {PC_BLEND_DISSOLVE, "Dslv"},
    {PC_BLEND_DARKEN, "Drkn"},
    {PC_BLEND_MULTIPLY, "Mltp"},
    {PC_BLEND_COLOR_BURN, "CBrn"},
    {PC_BLEND_LINEAR_BURN, "linearBurn"},
    {PC_BLEND_DARKER_COLOR, "darkerColor"},
    {PC_BLEND_LIGHTEN, "Lghn"},
    {PC_BLEND_SCREEN, "Scrn"},
    {PC_BLEND_COLOR_DODGE, "CDdg"},
    {PC_BLEND_LINEAR_DODGE, "linearDodge"},
    {PC_BLEND_LIGHTER_COLOR, "lighterColor"},
    {PC_BLEND_OVERLAY, "Ovrl"},
    {PC_BLEND_SOFT_LIGHT, "SftL"},
    {PC_BLEND_HARD_LIGHT, "HrdL"},
    {PC_BLEND_VIVID_LIGHT, "vividLight"},
    {PC_BLEND_LINEAR_LIGHT, "linearLight"},
    {PC_BLEND_PIN_LIGHT, "pinLight"},
    {PC_BLEND_HARD_MIX, "hardMix"},
    {PC_BLEND_DIFFERENCE, "Dfrn"},
    {PC_BLEND_EXCLUSION, "Xclu"},
    {PC_BLEND_SUBTRACT, "blendSubtraction"},
    {PC_BLEND_DIVIDE, "blendDivide"},
    {PC_BLEND_HUE, "H   "},
    {PC_BLEND_SATURATION, "Strt"},
    {PC_BLEND_COLOR, "Clr "},
    {PC_BLEND_LUMINOSITY, "Lmns"},
    {PC_BLEND_PASS_THROUGH, "passThrough"},
};

/* Long (string-ID) names some writers use instead of the four-character codes, for the modes
 * whose code is a short ID (BlnM multiply = Mltp). The others already use their long name. */
static const blend_name BLEND_LONG_NAMES[17] = {
    {PC_BLEND_NORMAL, "normal"},         {PC_BLEND_DISSOLVE, "dissolve"},
    {PC_BLEND_DARKEN, "darken"},         {PC_BLEND_MULTIPLY, "multiply"},
    {PC_BLEND_COLOR_BURN, "colorBurn"},  {PC_BLEND_LIGHTEN, "lighten"},
    {PC_BLEND_SCREEN, "screen"},         {PC_BLEND_COLOR_DODGE, "colorDodge"},
    {PC_BLEND_OVERLAY, "overlay"},       {PC_BLEND_SOFT_LIGHT, "softLight"},
    {PC_BLEND_HARD_LIGHT, "hardLight"},  {PC_BLEND_DIFFERENCE, "difference"},
    {PC_BLEND_EXCLUSION, "exclusion"},   {PC_BLEND_HUE, "hue"},
    {PC_BLEND_SATURATION, "saturation"}, {PC_BLEND_COLOR, "color"},
    {PC_BLEND_LUMINOSITY, "luminosity"},
};

static bool id_eq(const uint8_t *id, size_t len, const char *s)
{
    return id != NULL && len == strlen(s) && memcmp(id, s, len) == 0;
}

bool pc_io_blend_from_id(const uint8_t *id, size_t len, pc_blend_mode *out)
{
    for (size_t i = 0; i < 28; i++) {
        if (id_eq(id, len, BLEND_NAMES[i].id)) {
            *out = BLEND_NAMES[i].mode;
            return true;
        }
    }
    for (size_t i = 0; i < 17; i++) {
        if (id_eq(id, len, BLEND_LONG_NAMES[i].id)) {
            *out = BLEND_LONG_NAMES[i].mode;
            return true;
        }
    }
    return false;
}

static pc_blend_mode blend_of(const pc_psd_desc *d, const char *key, pc_blend_mode def)
{
    size_t n = 0;
    const uint8_t *e = pc_io_desc_enum(d, key, &n);
    pc_blend_mode m;
    return e != NULL && pc_io_blend_from_id(e, n, &m) ? m : def;
}

pc_status pc_io_blend_long_value(pc_blend_mode m, pc_psd_value *out, pc_error *err)
{
    const char *n = "normal";
    bool found = false;
    for (size_t i = 0; i < 17 && !found; i++) {
        if (BLEND_LONG_NAMES[i].mode == m) {
            n = BLEND_LONG_NAMES[i].id;
            found = true;
        }
    }
    for (size_t i = 0; i < 28 && !found; i++) {
        if (BLEND_NAMES[i].mode == m) {
            n = BLEND_NAMES[i].id;
            found = true;
        }
    }
    return pc_psd_value_enum("BlnM", n, out, err);
}

static pc_status blend_value(pc_blend_mode m, pc_psd_value *out, pc_error *err)
{
    const char *n = "Nrml";
    for (size_t i = 0; i < 28; i++) {
        if (BLEND_NAMES[i].mode == m) {
            n = BLEND_NAMES[i].id;
            break;
        }
    }
    return pc_psd_value_enum("BlnM", n, out, err);
}

/* ---- reading ---- */

static float f(const pc_psd_desc *d, const char *key, float def)
{
    double v;
    return pc_io_desc_num(pc_psd_desc_get(d, key), &v) ? pc_f64_to_f32(v) : def;
}

static float pct(const pc_psd_desc *d, const char *key, float def)
{
    return f(d, key, def * 100.0f) / 100.0f;
}

static pc_color color_of(const pc_psd_desc *d, const char *key)
{
    const pc_psd_desc *c = pc_io_desc_get(d, key);
    pc_color col;
    return c != NULL && pc_io_color_from_desc(c, &col) ? col : PC_COLOR_BLACK;
}

static bool not_false(const pc_psd_desc *d, const char *key)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    return !(v != NULL && v->kind == PC_PSD_V_BOOLEAN && !v->b);
}

static bool is_false(const pc_psd_desc *d, const char *key)
{
    return !not_false(d, key);
}

static bool enum_is(const pc_psd_desc *d, const char *key, const char *s)
{
    size_t n = 0;
    const uint8_t *e = pc_io_desc_enum(d, key, &n);
    return id_eq(e, n, s);
}

static pc_status contour_of(const pc_psd_desc *d, const char *key, pc_contour *out, pc_error *err)
{
    const pc_psd_desc *c = pc_io_desc_get(d, key);
    const pc_psd_value *crv;
    pc_contour r;
    bool has = false;
    bool linear;
    pc_status st;
    memset(&r, 0, sizeof r);
    if (c == NULL) {
        *out = r;
        return PC_OK;
    }
    st = pc_io_desc_text(c, "Nm  ", &has, &r.name, err);
    if (st != PC_OK) {
        return st;
    }
    crv = pc_psd_desc_get(c, "Crv ");
    if (crv != NULL && crv->kind == PC_PSD_V_LIST && crv->nlist > 0) {
        r.points.pts = pc_malloc_array(crv->nlist, sizeof *r.points.pts);
        if (r.points.pts == NULL) {
            pc_contour_fini(&r);
            return pc_io_nomem(err, "reading a contour");
        }
        for (size_t i = 0; i < crv->nlist; i++) {
            const pc_psd_value *it = &crv->list[i];
            if (it->kind == PC_PSD_V_DESCRIPTOR) {
                r.points.pts[r.points.len].input = f(it->desc, "Hrzn", 0.0f) / 255.0f;
                r.points.pts[r.points.len].output = f(it->desc, "Vrtc", 0.0f) / 255.0f;
                r.points.len++;
            }
        }
    }
    linear = r.points.len == 2 && r.points.pts[0].input == 0.0f && r.points.pts[0].output == 0.0f &&
             r.points.pts[1].input == 1.0f && r.points.pts[1].output == 1.0f;
    if (r.points.len == 0 || (linear && (r.name.len == 0 || pc_str_eq_c(&r.name, "Linear")))) {
        pc_contour_fini(&r);
        memset(out, 0, sizeof *out);
        return PC_OK;
    }
    r.custom = true;
    *out = r;
    return PC_OK;
}

static pc_fx_common common_of(const pc_psd_desc *d, pc_blend_mode blend, float opacity)
{
    pc_fx_common c;
    c.enabled = not_false(d, "enab");
    c.blend = blend_of(d, "Md  ", blend);
    c.opacity = pct(d, "Opct", opacity);
    return c;
}

static pc_status gradient_from(const pc_psd_desc *d, pc_gradient *out, pc_error *err)
{
    pc_gradient g;
    const pc_psd_desc *grad = pc_io_desc_get(d, "Grad");
    const pc_psd_desc *off = pc_io_desc_get(d, "Ofst");
    pc_status st;
    memset(&g, 0, sizeof g);
    if (grad != NULL) {
        size_t n = 0;
        const uint8_t *m = pc_io_desc_enum(d, "gs99", &n);
        st = pc_io_gradient_stops_with(grad, m, n, &g.stops, &g.nstops, &g.opacity_stops, &g.nopacity_stops,
                                       err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (g.nstops == 0) {
        pc_gradient def;
        free(g.stops);
        st = pc_gradient_default(&def, err);
        if (st != PC_OK) {
            free(g.opacity_stops);
            return st;
        }
        g.stops = def.stops;
        g.nstops = def.nstops;
    }
    g.style = pc_io_gradient_style(d);
    g.angle = f(d, "Angl", 90.0f);
    g.scale = pct(d, "Scl ", 1.0f);
    g.reverse = pc_io_desc_bool(d, "Rvrs");
    g.align = not_false(d, "Algn");
    g.offset[0] = off != NULL ? pct(off, "Hrzn", 0.0f) : 0.0f;
    g.offset[1] = off != NULL ? pct(off, "Vrtc", 0.0f) : 0.0f;
    *out = g;
    return PC_OK;
}

static pc_status parse_shadow(const pc_psd_desc *d, bool inner, pc_shadow *s, pc_error *err)
{
    s->common = common_of(d, PC_BLEND_MULTIPLY, 0.75f);
    s->color = color_of(d, "Clr ");
    s->angle = f(d, "lagl", 120.0f);
    s->use_global_light = not_false(d, "uglg");
    s->distance = f(d, "Dstn", 5.0f);
    s->spread = pct(d, "Ckmt", 0.0f);
    s->size = f(d, "blur", 5.0f);
    s->anti_alias = pc_io_desc_bool(d, "AntA");
    s->noise = pct(d, "Nose", 0.0f);
    s->knocks_out = !inner && not_false(d, "layerConceals");
    return contour_of(d, "TrnS", &s->contour, err);
}

static pc_status parse_glow(const pc_psd_desc *d, bool inner, pc_glow *g, pc_error *err)
{
    pc_status st;
    g->common = common_of(d, PC_BLEND_SCREEN, 0.75f);
    g->technique = enum_is(d, "GlwT", "PrBL") ? PC_GLOW_PRECISE : PC_GLOW_SOFTER;
    g->spread = pct(d, "Ckmt", 0.0f);
    g->size = f(d, "blur", 5.0f);
    g->anti_alias = pc_io_desc_bool(d, "AntA");
    g->range = pct(d, "Inpr", 0.5f);
    g->jitter = pct(d, "ShdN", 0.0f);
    g->noise = pct(d, "Nose", 0.0f);
    g->source = inner && enum_is(d, "glwS", "SrcC") ? PC_GLOW_SOURCE_CENTER : PC_GLOW_SOURCE_EDGE;
    if (pc_psd_desc_get(d, "Grad") != NULL) {
        g->paint.kind = PC_FX_PAINT_GRADIENT;
        st = gradient_from(d, &g->paint.u.gradient, err);
    } else {
        g->paint.kind = PC_FX_PAINT_COLOR;
        g->paint.u.color = color_of(d, "Clr ");
        st = PC_OK;
    }
    return st != PC_OK ? st : contour_of(d, "TrnS", &g->contour, err);
}

static pc_status parse_stroke(const pc_psd_desc *d, pc_stroke_fx *s, pc_error *err)
{
    pc_status st = PC_OK;
    s->common = common_of(d, PC_BLEND_NORMAL, 1.0f);
    s->size = f(d, "Sz  ", 3.0f);
    s->position = enum_is(d, "Styl", "InsF")   ? PC_STROKE_INSIDE
                  : enum_is(d, "Styl", "CtrF") ? PC_STROKE_CENTER
                                               : PC_STROKE_OUTSIDE;
    if (enum_is(d, "PntT", "GrFl")) {
        s->paint.kind = PC_FX_PAINT_GRADIENT;
        st = gradient_from(d, &s->paint.u.gradient, err);
    } else if (enum_is(d, "PntT", "Ptrn")) {
        const pc_psd_desc *p = pc_io_desc_get(d, "Ptrn");
        bool has;
        s->paint.kind = PC_FX_PAINT_PATTERN;
        s->paint.u.pattern.scale = pct(d, "Scl ", 1.0f);
        st = pc_io_desc_text(p, "Nm  ", &has, &s->paint.u.pattern.name, err);
        st = st != PC_OK ? st : pc_io_desc_text(p, "Idnt", &has, &s->paint.u.pattern.id, err);
    } else {
        s->paint.kind = PC_FX_PAINT_COLOR;
        s->paint.u.color = color_of(d, "Clr ");
    }
    return st;
}

static pc_status parse_bevel(const pc_psd_desc *d, pc_bevel *b, pc_error *err)
{
    const pc_psd_desc *hc = pc_io_desc_get(d, "hglC");
    pc_status st;
    b->enabled = not_false(d, "enab");
    b->style = enum_is(d, "bvlS", "OtrB")           ? PC_BEVEL_OUTER
               : enum_is(d, "bvlS", "Embs")         ? PC_BEVEL_EMBOSS
               : enum_is(d, "bvlS", "PlEb")         ? PC_BEVEL_PILLOW_EMBOSS
               : enum_is(d, "bvlS", "strokeEmboss") ? PC_BEVEL_STROKE_EMBOSS
                                                    : PC_BEVEL_INNER;
    b->technique = enum_is(d, "bvlT", "PrBL")   ? PC_BEVEL_CHISEL_HARD
                   : enum_is(d, "bvlT", "Slmt") ? PC_BEVEL_CHISEL_SOFT
                                                : PC_BEVEL_SMOOTH;
    b->depth = pct(d, "srgR", 1.0f);
    b->up = !enum_is(d, "bvlD", "Out ");
    b->size = f(d, "blur", 5.0f);
    b->soften = f(d, "Sftn", 0.0f);
    b->angle = f(d, "lagl", 120.0f);
    b->altitude = f(d, "Lald", 30.0f);
    b->use_global_light = not_false(d, "uglg");
    b->highlight = (pc_fx_common){true, blend_of(d, "hglM", PC_BLEND_SCREEN), pct(d, "hglO", 0.75f)};
    if (hc == NULL || !pc_io_color_from_desc(hc, &b->highlight_color)) {
        b->highlight_color = PC_COLOR_WHITE;
    }
    b->shadow = (pc_fx_common){true, blend_of(d, "sdwM", PC_BLEND_MULTIPLY), pct(d, "sdwO", 0.75f)};
    b->shadow_color = color_of(d, "sdwC");
    st = contour_of(d, "TrnS", &b->gloss_contour, err);
    if (st == PC_OK && pc_io_desc_bool(d, "useShape")) {
        b->has_contour = true;
        b->contour.range = pct(d, "Inpr", 0.5f);
        b->contour.anti_alias = pc_io_desc_bool(d, "AntA");
        st = contour_of(d, "MpgS", &b->contour.contour, err);
    }
    if (st == PC_OK && pc_io_desc_bool(d, "useTexture")) {
        pc_bevel_texture *t = &b->texture;
        float angle;
        b->has_texture = true;
        t->scale = pct(d, "Scl ", 1.0f);
        t->depth = pct(d, "textureDepth", 1.0f);
        t->invert = pc_io_desc_bool(d, "InvT");
        pc_io_pattern_placement(d, &angle, &t->link, t->phase);
        st = pc_io_pattern_ref(d, &t->name, &t->id, err);
    }
    return st;
}

/* parse_one: *e zeroed first; *ok false when the effect is absent (present: false) or unknown. */
static pc_status parse_one(const char *key, const pc_psd_desc *d, bool *ok, pc_effect *e, pc_error *err)
{
    pc_status st = PC_OK;
    memset(e, 0, sizeof *e);
    *ok = false;
    if (is_false(d, "present")) {
        return PC_OK;
    }
    if (strcmp(key, "DrSh") == 0 || strcmp(key, "IrSh") == 0) {
        bool inner = key[0] == 'I';
        e->kind = inner ? PC_FX_INNER_SHADOW : PC_FX_DROP_SHADOW;
        st = parse_shadow(d, inner, &e->u.shadow, err);
    } else if (strcmp(key, "OrGl") == 0 || strcmp(key, "IrGl") == 0) {
        bool inner = key[0] == 'I';
        e->kind = inner ? PC_FX_INNER_GLOW : PC_FX_OUTER_GLOW;
        st = parse_glow(d, inner, &e->u.glow, err);
    } else if (strcmp(key, "FrFX") == 0) {
        e->kind = PC_FX_STROKE;
        st = parse_stroke(d, &e->u.stroke, err);
    } else if (strcmp(key, "SoFi") == 0) {
        e->kind = PC_FX_COLOR_OVERLAY;
        e->u.color_overlay.common = common_of(d, PC_BLEND_NORMAL, 1.0f);
        e->u.color_overlay.color = color_of(d, "Clr ");
    } else if (strcmp(key, "GrFl") == 0) {
        e->kind = PC_FX_GRADIENT_OVERLAY;
        e->u.gradient_overlay.common = common_of(d, PC_BLEND_NORMAL, 1.0f);
        e->u.gradient_overlay.dither = pc_io_desc_bool(d, "Dthr");
        st = gradient_from(d, &e->u.gradient_overlay.gradient, err);
    } else if (strcmp(key, "patternFill") == 0) {
        e->kind = PC_FX_PATTERN_OVERLAY;
        e->u.pattern_overlay.common = common_of(d, PC_BLEND_NORMAL, 1.0f);
        e->u.pattern_overlay.scale = pct(d, "Scl ", 1.0f);
        pc_io_pattern_placement(d, &e->u.pattern_overlay.angle, &e->u.pattern_overlay.link,
                                e->u.pattern_overlay.phase);
        st = pc_io_pattern_ref(d, &e->u.pattern_overlay.name, &e->u.pattern_overlay.id, err);
    } else if (strcmp(key, "ChFX") == 0) {
        pc_satin *s = &e->u.satin;
        e->kind = PC_FX_SATIN;
        s->common = common_of(d, PC_BLEND_MULTIPLY, 0.5f);
        s->color = color_of(d, "Clr ");
        s->angle = f(d, "lagl", 19.0f);
        s->distance = f(d, "Dstn", 11.0f);
        s->size = f(d, "blur", 14.0f);
        s->anti_alias = pc_io_desc_bool(d, "AntA");
        s->invert = pc_io_desc_bool(d, "Invr");
        st = contour_of(d, "MpgS", &s->contour, err);
    } else if (strcmp(key, "ebbl") == 0) {
        e->kind = PC_FX_BEVEL_EMBOSS;
        st = parse_bevel(d, &e->u.bevel, err);
    } else {
        return PC_OK;
    }
    if (st != PC_OK) {
        pc_effect_fini(e);
        return st;
    }
    *ok = true;
    return PC_OK;
}

/* Single-instance key and multi-instance list key for each kind. */
static const char *const KINDS[10][2] = {
    {"DrSh", "dropShadowMulti"},
    {"IrSh", "innerShadowMulti"},
    {"OrGl", ""},
    {"IrGl", ""},
    {"ebbl", ""},
    {"ChFX", ""},
    {"SoFi", "solidFillMulti"},
    {"GrFl", "gradientFillMulti"},
    {"patternFill", ""},
    {"FrFX", "frameFXMulti"},
};

typedef struct fx_vec {
    pc_effect *v;
    size_t n;
    size_t cap;
} fx_vec;

static void fx_vec_fini(fx_vec *v)
{
    for (size_t i = 0; i < v->n; i++) {
        pc_effect_fini(&v->v[i]);
    }
    free(v->v);
    memset(v, 0, sizeof *v);
}

static pc_status fx_vec_push(fx_vec *v, pc_effect *e, pc_error *err)
{
    pc_effect *nv = pc_grow_array(v->v, &v->cap, v->n + 1, sizeof *nv);
    if (nv == NULL) {
        pc_effect_fini(e);
        return pc_io_nomem(err, "reading layer effects");
    }
    v->v = nv;
    v->v[v->n++] = *e;
    return PC_OK;
}

static pc_status parse_into(const char *single, const pc_psd_desc *d, fx_vec *out, pc_error *err)
{
    pc_effect e;
    bool ok = false;
    pc_status st = parse_one(single, d, &ok, &e, err);
    if (st != PC_OK || !ok) {
        return st;
    }
    return fx_vec_push(out, &e, err);
}

pc_status pc_io_parse_lfx2(const uint8_t *data, size_t len, bool *found, bool *master, pc_effect **items,
                           size_t *n, pc_error *err)
{
    pc_psd_desc d;
    bool ok = false;
    fx_vec out = {NULL, 0, 0};
    pc_status st;
    if (data == NULL || len < 4) {
        *found = false;
        return PC_OK;
    }
    st = pc_io_parse_prefix_versioned(data + 4, len - 4, &ok, &d, err);
    if (st != PC_OK || !ok) {
        *found = false;
        return st;
    }
    for (size_t k = 0; k < 10 && st == PC_OK; k++) {
        const char *single = KINDS[k][0];
        const char *multi = KINDS[k][1];
        const pc_psd_value *from_multi = multi[0] != '\0' ? pc_psd_desc_get(&d, multi) : NULL;
        if (from_multi != NULL && from_multi->kind == PC_PSD_V_LIST) {
            for (size_t i = 0; i < from_multi->nlist && st == PC_OK; i++) {
                if (from_multi->list[i].kind == PC_PSD_V_DESCRIPTOR) {
                    st = parse_into(single, from_multi->list[i].desc, &out, err);
                }
            }
        } else {
            const pc_psd_desc *x = pc_io_desc_get(&d, single);
            if (x != NULL) {
                st = parse_into(single, x, &out, err);
            }
        }
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        fx_vec_fini(&out);
        return st;
    }
    *master = not_false(&d, "masterFXSwitch");
    pc_psd_desc_fini(&d);
    *items = out.v;
    *n = out.n;
    *found = true;
    return PC_OK;
}

/* ---- writing ---- */

static pc_status add_value(pc_psd_desc *d, const char *key, pc_psd_value v, pc_error *err)
{
    return pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_unit(pc_psd_desc *d, const char *key, const char unit[4], float v, pc_error *err)
{
    return add_value(d, key, pc_io_unit_float(unit, (double)v), err);
}

static pc_status add_bool(pc_psd_desc *d, const char *key, bool b, pc_error *err)
{
    return add_value(d, key, pc_psd_value_bool(b), err);
}

static pc_status add_enum(pc_psd_desc *d, const char *key, const char *type, const char *value, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_enum(type, value, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_desc(pc_psd_desc *d, const char *key, pc_psd_desc *sub, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_object(sub, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_color(pc_psd_desc *d, const char *key, const pc_color *c, pc_error *err)
{
    pc_psd_desc cd;
    pc_status st = pc_io_color_to_desc(c, &cd, err);
    return st != PC_OK ? st : add_desc(d, key, &cd, err);
}

static pc_status add_blend(pc_psd_desc *d, const char *key, pc_blend_mode m, pc_error *err)
{
    pc_psd_value v;
    pc_status st = blend_value(m, &v, err);
    return st != PC_OK ? st : pc_psd_desc_add(d, key, &v, err);
}

static pc_status add_contour(pc_psd_desc *d, const char *key, const pc_contour *c, pc_error *err)
{
    static const pc_curve_point line[2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    const pc_curve_point *pts = c->custom ? c->points.pts : line;
    size_t n = c->custom ? c->points.len : 2;
    pc_psd_desc s;
    pc_psd_value list;
    pc_psd_value nm;
    pc_status st;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    st = pc_psd_desc_init(&s, "ShpC", err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < n; i++) {
        pc_psd_desc p;
        pc_psd_value item;
        TRY(pc_psd_desc_init(&p, "CrPt", err));
        st = add_value(&p, "Hrzn", pc_psd_value_double((double)(pts[i].input * 255.0f)), err);
        st = st != PC_OK ? st
                         : add_value(&p, "Vrtc", pc_psd_value_double((double)(pts[i].output * 255.0f)), err);
        st = st != PC_OK ? st : pc_psd_value_object(&p, &item, err);
        if (st != PC_OK) {
            pc_psd_desc_fini(&p);
            goto fail;
        }
        TRY(pc_psd_value_list_push(&list, &item, err));
    }
    if (c->custom) {
        TRY(pc_io_text_value(c->name.ptr, c->name.len, &nm, err));
    } else {
        TRY(pc_io_text_value("Linear", 6, &nm, err));
    }
    TRY(pc_psd_desc_add(&s, "Nm  ", &nm, err));
    TRY(pc_psd_desc_add(&s, "Crv ", &list, err));
    return add_desc(d, key, &s, err);
fail:
    pc_psd_value_fini(&list);
    pc_psd_desc_fini(&s);
    return st;
}

static pc_status with_common(pc_psd_desc *d, const pc_fx_common *c, pc_error *err)
{
    pc_status st = add_bool(d, "enab", c->enabled, err);
    st = st != PC_OK ? st : add_bool(d, "present", true, err);
    st = st != PC_OK ? st : add_bool(d, "showInDialog", true, err);
    st = st != PC_OK ? st : add_blend(d, "Md  ", c->blend, err);
    return st != PC_OK ? st : add_unit(d, "Opct", "#Prc", c->opacity * 100.0f, err);
}

static pc_status gradient_keys(pc_psd_desc *d, const pc_gradient *g, pc_error *err)
{
    pc_psd_desc gd;
    pc_psd_desc pnt;
    pc_psd_value style;
    pc_status st =
        pc_io_gradient_desc(g->stops, g->nstops, g->opacity_stops, g->nopacity_stops, NULL, 0, &gd, err);
    st = st != PC_OK ? st : add_desc(d, "Grad", &gd, err);
    st = st != PC_OK ? st : add_unit(d, "Angl", "#Ang", g->angle, err);
    st = st != PC_OK ? st : pc_io_gradient_style_value(g->style, &style, err);
    st = st != PC_OK ? st : pc_psd_desc_add(d, "Type", &style, err);
    st = st != PC_OK ? st : add_bool(d, "Rvrs", g->reverse, err);
    st = st != PC_OK ? st : add_bool(d, "Algn", g->align, err);
    st = st != PC_OK ? st : add_unit(d, "Scl ", "#Prc", g->scale * 100.0f, err);
    st = st != PC_OK ? st : pc_psd_desc_init(&pnt, "Pnt ", err);
    if (st != PC_OK) {
        return st;
    }
    st = add_unit(&pnt, "Hrzn", "#Prc", g->offset[0] * 100.0f, err);
    st = st != PC_OK ? st : add_unit(&pnt, "Vrtc", "#Prc", g->offset[1] * 100.0f, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&pnt);
        return st;
    }
    return add_desc(d, "Ofst", &pnt, err);
}

/* The lfx2 key an effect is stored under. */
static const char *kind_key(const pc_effect *e)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
        return "DrSh";
    case PC_FX_INNER_SHADOW:
        return "IrSh";
    case PC_FX_OUTER_GLOW:
        return "OrGl";
    case PC_FX_INNER_GLOW:
        return "IrGl";
    case PC_FX_STROKE:
        return "FrFX";
    case PC_FX_COLOR_OVERLAY:
        return "SoFi";
    case PC_FX_GRADIENT_OVERLAY:
        return "GrFl";
    case PC_FX_PATTERN_OVERLAY:
        return "patternFill";
    case PC_FX_SATIN:
        return "ChFX";
    case PC_FX_BEVEL_EMBOSS:
    default:
        return "ebbl";
    }
}

static pc_status write_shadow(pc_psd_desc *d, const pc_shadow *s, bool inner, pc_error *err)
{
    pc_status st = with_common(d, &s->common, err);
    st = st != PC_OK ? st : add_color(d, "Clr ", &s->color, err);
    st = st != PC_OK ? st : add_bool(d, "uglg", s->use_global_light, err);
    st = st != PC_OK ? st : add_unit(d, "lagl", "#Ang", s->angle, err);
    st = st != PC_OK ? st : add_unit(d, "Dstn", "#Pxl", s->distance, err);
    st = st != PC_OK ? st : add_unit(d, "Ckmt", "#Pxl", s->spread * 100.0f, err);
    st = st != PC_OK ? st : add_unit(d, "blur", "#Pxl", s->size, err);
    st = st != PC_OK ? st : add_unit(d, "Nose", "#Prc", s->noise * 100.0f, err);
    st = st != PC_OK ? st : add_bool(d, "AntA", s->anti_alias, err);
    st = st != PC_OK ? st : add_contour(d, "TrnS", &s->contour, err);
    if (st == PC_OK && !inner) {
        st = add_bool(d, "layerConceals", s->knocks_out, err);
    }
    return st;
}

static pc_status write_glow(pc_psd_desc *d, const pc_glow *g, bool inner, pc_error *err)
{
    pc_status st = with_common(d, &g->common, err);
    if (st != PC_OK) {
        return st;
    }
    switch (g->paint.kind) {
    case PC_FX_PAINT_GRADIENT:
        st = gradient_keys(d, &g->paint.u.gradient, err);
        break;
    case PC_FX_PAINT_COLOR:
        st = add_color(d, "Clr ", &g->paint.u.color, err);
        break;
    case PC_FX_PAINT_PATTERN:
    default:
        st = add_color(d, "Clr ", &PC_COLOR_WHITE, err);
        break;
    }
    st = st != PC_OK ? st
                     : add_enum(d, "GlwT", "BETE", g->technique == PC_GLOW_PRECISE ? "PrBL" : "SfBL", err);
    st = st != PC_OK ? st : add_unit(d, "Ckmt", "#Pxl", g->spread * 100.0f, err);
    st = st != PC_OK ? st : add_unit(d, "blur", "#Pxl", g->size, err);
    st = st != PC_OK ? st : add_unit(d, "Nose", "#Prc", g->noise * 100.0f, err);
    st = st != PC_OK ? st : add_unit(d, "ShdN", "#Prc", g->jitter * 100.0f, err);
    st = st != PC_OK ? st : add_bool(d, "AntA", g->anti_alias, err);
    st = st != PC_OK ? st : add_contour(d, "TrnS", &g->contour, err);
    st = st != PC_OK ? st : add_unit(d, "Inpr", "#Prc", g->range * 100.0f, err);
    if (st == PC_OK && inner) {
        st = add_enum(d, "glwS", "IGSr", g->source == PC_GLOW_SOURCE_CENTER ? "SrcC" : "SrcE", err);
    }
    return st;
}

static pc_status write_stroke(pc_psd_desc *d, const pc_stroke_fx *s, pc_error *err)
{
    const char *styl = s->position == PC_STROKE_INSIDE   ? "InsF"
                       : s->position == PC_STROKE_CENTER ? "CtrF"
                                                         : "OutF";
    pc_status st = with_common(d, &s->common, err);
    st = st != PC_OK ? st : add_enum(d, "Styl", "FStl", styl, err);
    st = st != PC_OK ? st : add_unit(d, "Sz  ", "#Pxl", s->size, err);
    if (st != PC_OK) {
        return st;
    }
    switch (s->paint.kind) {
    case PC_FX_PAINT_COLOR:
        st = add_enum(d, "PntT", "FrFl", "SClr", err);
        return st != PC_OK ? st : add_color(d, "Clr ", &s->paint.u.color, err);
    case PC_FX_PAINT_GRADIENT:
        st = add_enum(d, "PntT", "FrFl", "GrFl", err);
        return st != PC_OK ? st : gradient_keys(d, &s->paint.u.gradient, err);
    case PC_FX_PAINT_PATTERN:
    default: {
        pc_psd_desc p;
        pc_psd_value v;
        st = add_enum(d, "PntT", "FrFl", "Ptrn", err);
        st = st != PC_OK ? st : pc_psd_desc_init(&p, "Ptrn", err);
        if (st != PC_OK) {
            return st;
        }
        /* The stroke writes the id even when empty (unlike pattern_ref_desc). */
        st = pc_io_text_value(s->paint.u.pattern.name.ptr, s->paint.u.pattern.name.len, &v, err);
        st = st != PC_OK ? st : pc_psd_desc_add(&p, "Nm  ", &v, err);
        st = st != PC_OK ? st
                         : pc_io_text_value(s->paint.u.pattern.id.ptr, s->paint.u.pattern.id.len, &v, err);
        st = st != PC_OK ? st : pc_psd_desc_add(&p, "Idnt", &v, err);
        if (st != PC_OK) {
            pc_psd_desc_fini(&p);
            return st;
        }
        st = add_desc(d, "Ptrn", &p, err);
        return st != PC_OK ? st : add_unit(d, "Scl ", "#Prc", s->paint.u.pattern.scale * 100.0f, err);
    }
    }
}

static pc_status write_bevel(pc_psd_desc *d, const pc_bevel *b, pc_error *err)
{
    static const char *const styles[5] = {"OtrB", "InrB", "Embs", "PlEb", "strokeEmboss"};
    static const char *const techs[3] = {"SfBL", "PrBL", "Slmt"};
    pc_status st = add_bool(d, "enab", b->enabled, err);
    st = st != PC_OK ? st : add_bool(d, "present", true, err);
    st = st != PC_OK ? st : add_bool(d, "showInDialog", true, err);
    st = st != PC_OK ? st : add_blend(d, "hglM", b->highlight.blend, err);
    st = st != PC_OK ? st : add_color(d, "hglC", &b->highlight_color, err);
    st = st != PC_OK ? st : add_unit(d, "hglO", "#Prc", b->highlight.opacity * 100.0f, err);
    st = st != PC_OK ? st : add_blend(d, "sdwM", b->shadow.blend, err);
    st = st != PC_OK ? st : add_color(d, "sdwC", &b->shadow_color, err);
    st = st != PC_OK ? st : add_unit(d, "sdwO", "#Prc", b->shadow.opacity * 100.0f, err);
    st = st != PC_OK ? st
                     : add_enum(d, "bvlT", "bvlT", techs[(size_t)b->technique < 3 ? b->technique : 0], err);
    st = st != PC_OK ? st : add_enum(d, "bvlS", "BESl", styles[(size_t)b->style < 5 ? b->style : 1], err);
    st = st != PC_OK ? st : add_bool(d, "uglg", b->use_global_light, err);
    st = st != PC_OK ? st : add_unit(d, "lagl", "#Ang", b->angle, err);
    st = st != PC_OK ? st : add_unit(d, "Lald", "#Ang", b->altitude, err);
    st = st != PC_OK ? st : add_unit(d, "srgR", "#Prc", b->depth * 100.0f, err);
    st = st != PC_OK ? st : add_unit(d, "blur", "#Pxl", b->size, err);
    st = st != PC_OK ? st : add_enum(d, "bvlD", "BESs", b->up ? "In  " : "Out ", err);
    st = st != PC_OK ? st : add_contour(d, "TrnS", &b->gloss_contour, err);
    st = st != PC_OK ? st : add_unit(d, "Sftn", "#Pxl", b->soften, err);
    if (st != PC_OK) {
        return st;
    }
    if (b->has_contour) {
        st = add_bool(d, "useShape", true, err);
        st = st != PC_OK ? st : add_contour(d, "MpgS", &b->contour.contour, err);
        st = st != PC_OK ? st : add_bool(d, "AntA", b->contour.anti_alias, err);
        st = st != PC_OK ? st : add_unit(d, "Inpr", "#Prc", b->contour.range * 100.0f, err);
    } else {
        st = add_bool(d, "useShape", false, err);
    }
    if (st != PC_OK) {
        return st;
    }
    if (b->has_texture) {
        const pc_bevel_texture *t = &b->texture;
        pc_psd_desc p;
        st = add_bool(d, "useTexture", true, err);
        st = st != PC_OK ? st : add_bool(d, "InvT", t->invert, err);
        st = st != PC_OK ? st : add_unit(d, "Scl ", "#Prc", t->scale * 100.0f, err);
        st = st != PC_OK ? st : add_unit(d, "textureDepth", "#Prc", t->depth * 100.0f, err);
        st = st != PC_OK ? st
                         : pc_io_pattern_ref_desc(t->name.ptr, t->name.len, t->id.ptr, t->id.len, &p, err);
        st = st != PC_OK ? st : add_desc(d, "Ptrn", &p, err);
        return st != PC_OK ? st : pc_io_with_pattern_placement(d, 0.0f, t->link, t->phase, err);
    }
    return add_bool(d, "useTexture", false, err);
}

/* write_one: the effect's descriptor (class = its key). */
static pc_status write_one(const pc_effect *e, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_status st = pc_psd_desc_init(&d, kind_key(e), err);
    if (st != PC_OK) {
        return st;
    }
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        st = write_shadow(&d, &e->u.shadow, e->kind == PC_FX_INNER_SHADOW, err);
        break;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        st = write_glow(&d, &e->u.glow, e->kind == PC_FX_INNER_GLOW, err);
        break;
    case PC_FX_STROKE:
        st = write_stroke(&d, &e->u.stroke, err);
        break;
    case PC_FX_COLOR_OVERLAY:
        st = with_common(&d, &e->u.color_overlay.common, err);
        st = st != PC_OK ? st : add_color(&d, "Clr ", &e->u.color_overlay.color, err);
        break;
    case PC_FX_GRADIENT_OVERLAY:
        st = with_common(&d, &e->u.gradient_overlay.common, err);
        st = st != PC_OK ? st : gradient_keys(&d, &e->u.gradient_overlay.gradient, err);
        st = st != PC_OK ? st : add_bool(&d, "Dthr", e->u.gradient_overlay.dither, err);
        break;
    case PC_FX_PATTERN_OVERLAY: {
        pc_psd_desc p;
        st = with_common(&d, &e->u.pattern_overlay.common, err);
        st = st != PC_OK
                 ? st
                 : pc_io_pattern_ref_desc(e->u.pattern_overlay.name.ptr, e->u.pattern_overlay.name.len,
                                          e->u.pattern_overlay.id.ptr, e->u.pattern_overlay.id.len, &p, err);
        st = st != PC_OK ? st : add_desc(&d, "Ptrn", &p, err);
        st = st != PC_OK ? st : add_unit(&d, "Scl ", "#Prc", e->u.pattern_overlay.scale * 100.0f, err);
        st = st != PC_OK
                 ? st
                 : pc_io_with_pattern_placement(&d, e->u.pattern_overlay.angle, e->u.pattern_overlay.link,
                                                e->u.pattern_overlay.phase, err);
        break;
    }
    case PC_FX_SATIN: {
        const pc_satin *s = &e->u.satin;
        st = with_common(&d, &s->common, err);
        st = st != PC_OK ? st : add_color(&d, "Clr ", &s->color, err);
        st = st != PC_OK ? st : add_bool(&d, "AntA", s->anti_alias, err);
        st = st != PC_OK ? st : add_bool(&d, "Invr", s->invert, err);
        st = st != PC_OK ? st : add_unit(&d, "lagl", "#Ang", s->angle, err);
        st = st != PC_OK ? st : add_unit(&d, "Dstn", "#Pxl", s->distance, err);
        st = st != PC_OK ? st : add_unit(&d, "blur", "#Pxl", s->size, err);
        st = st != PC_OK ? st : add_contour(&d, "MpgS", &s->contour, err);
        break;
    }
    case PC_FX_BEVEL_EMBOSS:
    default:
        st = write_bevel(&d, &e->u.bevel, err);
        break;
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

pc_status pc_io_write_lfx2(bool master, const pc_effect *items, size_t n, pc_buf *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_bytes b = {NULL, 0};
    pc_buf nb = PC_BUF_INIT;
    pc_status st;
    memset(&d, 0, sizeof d);
    TRY(pc_psd_desc_init(&d, "null", err));
    TRY(add_unit(&d, "Scl ", "#Prc", 100.0f, err));
    TRY(add_bool(&d, "masterFXSwitch", master, err));
    for (size_t k = 0; k < 10; k++) {
        const char *single = KINDS[k][0];
        const char *multi = KINDS[k][1];
        size_t count = 0;
        size_t first = 0;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(kind_key(&items[i]), single) == 0) {
                if (count == 0) {
                    first = i;
                }
                count++;
            }
        }
        if (count == 0) {
            continue;
        }
        if (count > 1 && multi[0] != '\0') {
            pc_psd_value list;
            memset(&list, 0, sizeof list);
            list.kind = PC_PSD_V_LIST;
            for (size_t i = 0; i < n && st == PC_OK; i++) {
                pc_psd_desc one;
                pc_psd_value item;
                if (strcmp(kind_key(&items[i]), single) != 0) {
                    continue;
                }
                st = write_one(&items[i], &one, err);
                st = st != PC_OK ? st : pc_psd_value_object(&one, &item, err);
                st = st != PC_OK ? st : pc_psd_value_list_push(&list, &item, err);
            }
            if (st != PC_OK) {
                pc_psd_value_fini(&list);
                goto fail;
            }
            TRY(pc_psd_desc_add(&d, multi, &list, err));
        } else {
            /* One instance, or a kind without a multi key: only the first is kept. */
            pc_psd_desc one;
            TRY(write_one(&items[first], &one, err));
            TRY(add_desc(&d, single, &one, err));
        }
    }
    TRY(pc_buf_append(&nb, (const uint8_t[4]){0, 0, 0, 0}, 4, err));
    TRY(pc_psd_vdesc_to_bytes(&d, &b, err));
    TRY(pc_buf_append(&nb, b.data, b.len, err));
    pc_psd_bytes_fini(&b);
    pc_psd_desc_fini(&d);
    pc_buf_fini(out);
    *out = nb;
    return PC_OK;
fail:
    pc_psd_bytes_fini(&b);
    pc_buf_fini(&nb);
    pc_psd_desc_fini(&d);
    return st;
}

/* ---- legacy lrFX ---- */

typedef struct lrfx_rd {
    const uint8_t *d;
    size_t len;
} lrfx_rd;

static bool rd16(const lrfx_rd *r, size_t at, uint16_t *out)
{
    if (at > r->len || r->len - at < 2) {
        return false;
    }
    *out = (uint16_t)(r->d[at] << 8 | r->d[at + 1]);
    return true;
}

static bool rd32(const lrfx_rd *r, size_t at, uint32_t *out)
{
    if (at > r->len || r->len - at < 4) {
        return false;
    }
    *out =
        (uint32_t)r->d[at] << 24 | (uint32_t)r->d[at + 1] << 16 | (uint32_t)r->d[at + 2] << 8 | r->d[at + 3];
    return true;
}

static bool rd8(const lrfx_rd *r, size_t at, uint8_t *out)
{
    if (at >= r->len) {
        return false;
    }
    *out = r->d[at];
    return true;
}

static float fixed(uint32_t v)
{
    return (float)pc_u32_as_i32(v) / 65536.0f;
}

/* space (2) + 4 x u16 components (RGB uses the first three, high bytes). */
static bool color_at(const lrfx_rd *r, size_t at, pc_color *out)
{
    float c[3];
    for (size_t k = 0; k < 3; k++) {
        uint16_t v;
        if (!rd16(r, at + 2 + k * 2, &v)) {
            return false;
        }
        c[k] = (float)(v >> 8) / 255.0f;
    }
    *out = pc_color_rgb(c[0], c[1], c[2]);
    return true;
}

static pc_blend_mode blend_at(const lrfx_rd *r, size_t at)
{
    pc_blend_mode m;
    if (at + 8 <= r->len && pc_blend_from_psd_key((const char *)r->d + at + 4, &m)) {
        return m;
    }
    return PC_BLEND_NORMAL;
}

static size_t sat_add(size_t a, size_t b)
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

pc_status pc_io_parse_lrfx(const uint8_t *data, size_t len, bool *found, bool *master, pc_effect **items,
                           size_t *n, pc_error *err)
{
    lrfx_rd r = {data, data != NULL ? len : 0};
    fx_vec out = {NULL, 0, 0};
    uint16_t count;
    size_t at = 4;
    pc_status st = PC_OK;
    if (!rd16(&r, 2, &count)) {
        *found = false;
        return PC_OK;
    }
    for (uint16_t i = 0; i < count; i++) {
        uint32_t size;
        size_t b;
        pc_effect e;
        if (at > r.len || r.len - at < 8 || memcmp(r.d + at, "8BIM", 4) != 0 || !rd32(&r, at + 8, &size)) {
            goto bad;
        }
        b = sat_add(at, 12);
        memset(&e, 0, sizeof e);
        if (memcmp(r.d + at + 4, "dsdw", 4) == 0 || memcmp(r.d + at + 4, "isdw", 4) == 0) {
            bool drop = r.d[at + 4] == 'd';
            uint32_t blur;
            uint32_t angle;
            uint32_t dist;
            uint8_t en;
            uint8_t ug;
            uint8_t op;
            pc_shadow *s = &e.u.shadow;
            /* Blur, angle and distance are 16.16 fixed point in practice. */
            if (!rd32(&r, b + 4, &blur) || !rd32(&r, b + 12, &angle) || !rd32(&r, b + 16, &dist) ||
                !color_at(&r, b + 20, &s->color) || !rd8(&r, b + 38, &en) || !rd8(&r, b + 39, &ug) ||
                !rd8(&r, b + 40, &op)) {
                goto bad;
            }
            e.kind = drop ? PC_FX_DROP_SHADOW : PC_FX_INNER_SHADOW;
            s->common = (pc_fx_common){en != 0, blend_at(&r, b + 30), (float)op / 255.0f};
            s->angle = fixed(angle);
            s->use_global_light = ug != 0;
            s->distance = fixed(dist);
            s->size = fixed(blur);
            s->knocks_out = drop;
            st = fx_vec_push(&out, &e, err);
        } else if (memcmp(r.d + at + 4, "oglw", 4) == 0 || memcmp(r.d + at + 4, "iglw", 4) == 0) {
            bool outer = r.d[at + 4] == 'o';
            uint32_t blur;
            uint8_t en;
            uint8_t op;
            pc_glow *g = &e.u.glow;
            if (!rd32(&r, b + 4, &blur) || !color_at(&r, b + 12, &g->paint.u.color) ||
                !rd8(&r, b + 30, &en) || !rd8(&r, b + 31, &op)) {
                goto bad;
            }
            e.kind = outer ? PC_FX_OUTER_GLOW : PC_FX_INNER_GLOW;
            g->common = (pc_fx_common){en != 0, blend_at(&r, b + 22), (float)op / 255.0f};
            g->paint.kind = PC_FX_PAINT_COLOR;
            g->technique = PC_GLOW_SOFTER;
            g->size = fixed(blur);
            g->range = 0.5f;
            g->source = PC_GLOW_SOURCE_EDGE;
            st = fx_vec_push(&out, &e, err);
        } else if (memcmp(r.d + at + 4, "sofi", 4) == 0) {
            uint8_t en;
            uint8_t op;
            pc_color c;
            if (!color_at(&r, b + 12, &c) || !rd8(&r, b + 22, &op) || !rd8(&r, b + 23, &en)) {
                goto bad;
            }
            e.kind = PC_FX_COLOR_OVERLAY;
            e.u.color_overlay.common = (pc_fx_common){en != 0, blend_at(&r, b + 4), (float)op / 255.0f};
            e.u.color_overlay.color = c;
            st = fx_vec_push(&out, &e, err);
        }
        if (st != PC_OK) {
            fx_vec_fini(&out);
            return st;
        }
        at = sat_add(b, size);
    }
    *master = true;
    *items = out.v;
    *n = out.n;
    *found = true;
    return PC_OK;
bad:
    fx_vec_fini(&out);
    *found = false;
    return PC_OK;
}
