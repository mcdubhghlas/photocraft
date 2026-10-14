/* The built-in brush presets (presets.h). Each preset is the shared base brush (pressure size off,
 * 25 % spacing) with the fields the reference sets; sections the reference builds from their
 * defaults are still default in the base, so setting fields in place is equivalent. */
#include "paint/presets.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "paint/procedural.h"

const char *const PC_PAINT_GROUPS[PC_PAINT_GROUP_COUNT] = {"General", "Dry Media", "Wet Media",
                                                           "Special Effects"};

void pc_paint_preset_captures_default(pc_paint_preset_captures *c)
{
    c->size = true;
    c->tool_settings = true;
    c->color = false;
}

bool pc_paint_preset_captures_is_default(const pc_paint_preset_captures *c)
{
    return c->size && c->tool_settings && !c->color;
}

bool pc_paint_preset_captures_eq(const pc_paint_preset_captures *a, const pc_paint_preset_captures *b)
{
    return a->size == b->size && a->tool_settings == b->tool_settings && a->color == b->color;
}

void pc_paint_preset_init(pc_paint_preset *p)
{
    memset(p, 0, sizeof *p);
    pc_paint_brush_default(&p->brush);
    pc_paint_preset_captures_default(&p->captures);
}

void pc_paint_preset_fini(pc_paint_preset *p)
{
    if (p == NULL) {
        return;
    }
    pc_str_fini(&p->name);
    pc_str_fini(&p->group);
    pc_paint_str_list_fini(&p->folder);
    pc_paint_brush_fini(&p->brush);
}

void pc_paint_str_list_fini(pc_paint_str_list *l)
{
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->len; i++) {
        pc_str_fini(&l->v[i]);
    }
    free(l->v);
    l->v = NULL;
    l->len = 0;
}

pc_status pc_paint_str_list_clone(const pc_paint_str_list *src, pc_paint_str_list *out, pc_error *err)
{
    pc_paint_str_list l = {NULL, 0};
    if (src->len > 0) {
        l.v = pc_calloc_array(src->len, sizeof *l.v);
        if (l.v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a preset");
        }
    }
    for (; l.len < src->len; l.len++) {
        if (pc_str_clone(&src->v[l.len], &l.v[l.len], err) != PC_OK) {
            pc_paint_str_list_fini(&l);
            return PC_ERR_NOMEM;
        }
    }
    *out = l;
    return PC_OK;
}

bool pc_paint_str_list_eq(const pc_paint_str_list *a, const pc_paint_str_list *b)
{
    if (a->len != b->len) {
        return false;
    }
    for (size_t i = 0; i < a->len; i++) {
        if (!pc_str_eq(&a->v[i], &b->v[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_paint_preset_clone(const pc_paint_preset *src, pc_paint_preset *out, pc_error *err)
{
    pc_paint_preset p;
    memset(&p, 0, sizeof p);
    if (pc_str_clone(&src->name, &p.name, err) != PC_OK ||
        pc_str_clone(&src->group, &p.group, err) != PC_OK ||
        pc_paint_str_list_clone(&src->folder, &p.folder, err) != PC_OK) {
        pc_str_fini(&p.name);
        pc_str_fini(&p.group);
        return PC_ERR_NOMEM;
    }
    pc_paint_brush_copy(&p.brush, &src->brush);
    p.builtin = src->builtin;
    p.captures = src->captures;
    *out = p;
    return PC_OK;
}

bool pc_paint_preset_eq(const pc_paint_preset *a, const pc_paint_preset *b)
{
    return pc_str_eq(&a->name, &b->name) && pc_paint_brush_eq(&a->brush, &b->brush) &&
           a->builtin == b->builtin && pc_str_eq(&a->group, &b->group) &&
           pc_paint_str_list_eq(&a->folder, &b->folder) &&
           pc_paint_preset_captures_eq(&a->captures, &b->captures);
}

void pc_paint_preset_list_fini(pc_paint_preset_list *l)
{
    if (l == NULL) {
        return;
    }
    for (size_t i = 0; i < l->len; i++) {
        pc_paint_preset_fini(&l->v[i]);
    }
    free(l->v);
    l->v = NULL;
    l->len = 0;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

const pc_paint_preset *pc_paint_presets_find(const pc_paint_preset *v, size_t n, const char *name, size_t len)
{
    for (size_t i = 0; i < n; i++) {
        const pc_str *s = &v[i].name;
        size_t j = 0;
        if (s->len != len) {
            continue;
        }
        while (j < len && lower((unsigned char)s->ptr[j]) == lower((unsigned char)name[j])) {
            j++;
        }
        if (j == len) {
            return &v[i];
        }
    }
    return NULL;
}

/* ---- building ---- */

#define BUILTIN_COUNT 36

typedef struct builder {
    pc_paint_preset_list *l;
    const pc_paint_brush *base;
    const char *group;
    pc_status st;
    pc_error *err;
} builder;

static pc_paint_dynamic pressure(void)
{
    return pc_paint_dynamic_controlled(PC_PAINT_CONTROL_PEN_PRESSURE);
}

static pc_paint_dynamic pressure_min(float minimum)
{
    pc_paint_dynamic d = pressure();
    d.minimum = minimum;
    return d;
}

static pc_paint_dynamic jit(float j, float minimum)
{
    pc_paint_dynamic d = pc_paint_dynamic_jitter(j);
    d.minimum = minimum;
    return d;
}

static void size_dyn(pc_paint_brush *b, pc_paint_dynamic size)
{
    b->shape_dynamics.enabled = true;
    b->shape_dynamics.size = size;
}

static void angle_dir(pc_paint_brush *b)
{
    b->shape_dynamics.enabled = true;
    b->shape_dynamics.angle = pc_paint_dynamic_controlled(PC_PAINT_CONTROL_DIRECTION);
}

static void transfer(pc_paint_brush *b, pc_paint_dynamic opacity, pc_paint_dynamic flow)
{
    b->transfer.enabled = true;
    b->transfer.opacity = opacity;
    b->transfer.flow = flow;
}

static void textured(pc_paint_brush *b, pc_paint_pattern_style style, uint32_t size, uint32_t seed,
                     float depth)
{
    b->texture.enabled = true;
    b->texture.pattern.kind = PC_PAINT_PATTERN_PROCEDURAL;
    b->texture.pattern.style = style;
    b->texture.pattern.size = size;
    b->texture.pattern.seed = seed;
    b->texture.depth = depth;
}

static void paper(pc_paint_brush *b, uint32_t seed, float depth)
{
    textured(b, PC_PAINT_STYLE_PAPER, 128, seed, depth);
}

/* A fresh copy of the base brush with size and hardness set. */
static void start(const builder *bd, pc_paint_brush *b, float size, float hardness)
{
    pc_paint_brush_copy(b, bd->base);
    b->size = size;
    b->hardness = hardness;
}

/* Sets a sampled tip from a generator result; records the first failure. */
static void sampled(builder *bd, pc_paint_tip *tip, pc_status st, pc_paint_gray_tile *t)
{
    if (st != PC_OK) {
        if (bd->st == PC_OK) {
            bd->st = st;
        }
        return;
    }
    pc_paint_tip_fini(tip);
    tip->kind = PC_PAINT_TIP_SAMPLED;
    tip->tile = *t;
}

/* Appends a preset taking b; b is finalised on failure. */
static void add(builder *bd, const char *name, pc_paint_brush *b)
{
    pc_paint_preset *p;
    if (bd->st != PC_OK || bd->l->len == BUILTIN_COUNT) {
        pc_paint_brush_fini(b);
        return;
    }
    p = &bd->l->v[bd->l->len];
    memset(p, 0, sizeof *p);
    if (pc_str_set_c(&p->name, name, bd->err) != PC_OK ||
        pc_str_set_c(&p->group, bd->group, bd->err) != PC_OK) {
        pc_str_fini(&p->name);
        pc_paint_brush_fini(b);
        bd->st = PC_ERR_NOMEM;
        return;
    }
    p->brush = *b;
    p->builtin = true;
    pc_paint_preset_captures_default(&p->captures);
    bd->l->len++;
}

static void general(builder *bd)
{
    pc_paint_brush b;
    bd->group = PC_PAINT_GROUPS[0];
    start(bd, &b, 30.0f, 1.0f);
    add(bd, "Hard Round", &b);
    start(bd, &b, 45.0f, 0.0f);
    add(bd, "Soft Round", &b);
    start(bd, &b, 5.0f, 1.0f);
    b.spacing = 0.1f;
    add(bd, "Hard Round Fine", &b);
    start(bd, &b, 300.0f, 0.0f);
    b.spacing = 0.15f;
    add(bd, "Soft Round Large", &b);
    start(bd, &b, 30.0f, 1.0f);
    size_dyn(&b, pressure());
    add(bd, "Hard Round Pressure Size", &b);
    start(bd, &b, 45.0f, 0.0f);
    size_dyn(&b, pressure());
    add(bd, "Soft Round Pressure Size", &b);
    start(bd, &b, 30.0f, 1.0f);
    transfer(&b, pressure(), pc_paint_dynamic_default());
    add(bd, "Hard Round Pressure Opacity", &b);
    start(bd, &b, 45.0f, 0.0f);
    transfer(&b, pressure(), pc_paint_dynamic_default());
    add(bd, "Soft Round Pressure Opacity", &b);
    start(bd, &b, 30.0f, 0.9f);
    transfer(&b, pressure(), pressure_min(0.2f));
    add(bd, "Hard Round Pressure Opacity and Flow", &b);
    start(bd, &b, 80.0f, 0.0f);
    b.flow = 0.1f;
    b.spacing = 0.1f;
    b.build_up = true;
    b.build_up_rate = 25.0f;
    add(bd, "Airbrush Soft", &b);
    start(bd, &b, 120.0f, 0.0f);
    b.flow = 0.15f;
    b.spacing = 0.1f;
    b.build_up = true;
    b.build_up_rate = 30.0f;
    transfer(&b, pc_paint_dynamic_default(), pressure());
    add(bd, "Airbrush Pressure Flow", &b);
    start(bd, &b, 28.0f, 1.0f);
    b.roundness = 0.2f;
    b.angle = 45.0f;
    b.spacing = 0.05f;
    add(bd, "Calligraphy Flat", &b);
}

static void dry_media(builder *bd)
{
    pc_paint_brush b;
    pc_paint_gray_tile t;
    bd->group = PC_PAINT_GROUPS[1];
    start(bd, &b, 3.0f, 1.0f);
    b.aliased = true;
    b.spacing = 0.1f;
    add(bd, "Hard Pencil", &b);

    start(bd, &b, 6.0f, 0.6f);
    b.spacing = 0.08f;
    size_dyn(&b, pressure_min(0.4f));
    transfer(&b, pressure_min(0.15f), pc_paint_dynamic_default());
    paper(&b, 21, 0.8f);
    b.texture.scale = 0.5f;
    add(bd, "Soft Pencil", &b);

    start(bd, &b, 40.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_chalk_tip(64, 7, &t, bd->err), &t);
    b.spacing = 0.2f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.size = jit(0.15f, 0.0f);
    paper(&b, 3, 0.6f);
    add(bd, "Chalk", &b);

    start(bd, &b, 45.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_charcoal_tip(64, 4, &t, bd->err), &t);
    b.spacing = 0.12f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = pressure_min(0.5f);
    b.shape_dynamics.angle = pc_paint_dynamic_controlled(PC_PAINT_CONTROL_DIRECTION);
    transfer(&b, pressure_min(0.3f), pc_paint_dynamic_default());
    paper(&b, 5, 0.7f);
    b.texture.mode = PC_PAINT_MASK_SUBTRACT;
    /* Subtract takes paint away where the pattern is light; inverting keeps the preset as
     * designed (the dark parts take it away). */
    b.texture.invert = true;
    add(bd, "Charcoal", &b);

    start(bd, &b, 50.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_chalk_tip(64, 31, &t, bd->err), &t);
    b.spacing = 0.15f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.roundness = jit(0.3f, 0.5f);
    b.scattering.enabled = true;
    b.scattering.scatter = pc_paint_dynamic_jitter(0.25f);
    textured(&b, PC_PAINT_STYLE_CANVAS, 64, 2, 0.55f);
    b.texture.mode = PC_PAINT_MASK_SUBTRACT;
    /* Subtract takes paint away where the pattern is light; inverting keeps the preset as
     * designed (the dark parts take it away). */
    b.texture.invert = true;
    add(bd, "Pastel", &b);

    start(bd, &b, 22.0f, 0.85f);
    b.roundness = 0.7f;
    b.spacing = 0.08f;
    angle_dir(&b);
    paper(&b, 17, 0.9f);
    b.texture.each_tip = true;
    b.texture.mode = PC_PAINT_MASK_HARD_MIX;
    b.texture.depth_jitter = pc_paint_dynamic_jitter(0.3f);
    add(bd, "Wax Crayon", &b);

    start(bd, &b, 50.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_bristle_tip(64, 5, &t, bd->err), &t);
    b.spacing = 0.04f;
    angle_dir(&b);
    b.transfer.enabled = true;
    b.transfer.flow = (pc_paint_dynamic){0.0f, PC_PAINT_CONTROL_FADE, 400, 0.1f};
    add(bd, "Dry Bristle", &b);

    start(bd, &b, 60.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_rake_tip(64, 3, 7, &t, bd->err), &t);
    b.spacing = 0.03f;
    angle_dir(&b);
    transfer(&b, pressure_min(0.25f), pc_paint_dynamic_default());
    add(bd, "Rake", &b);
}

static void wet_media(builder *bd)
{
    pc_paint_brush b;
    pc_paint_gray_tile t;
    pc_paint_dynamic d;
    bd->group = PC_PAINT_GROUPS[2];
    start(bd, &b, 60.0f, 0.3f);
    b.wet_edges = true;
    paper(&b, 9, 0.35f);
    b.texture.scale = 1.5f;
    b.texture.pattern.size = 256;
    add(bd, "Watercolor Wet Edges", &b);

    start(bd, &b, 140.0f, 0.0f);
    b.flow = 0.35f;
    b.spacing = 0.12f;
    b.wet_edges = true;
    size_dyn(&b, pressure_min(0.35f));
    d = pressure();
    d.jitter = 0.2f;
    transfer(&b, pc_paint_dynamic_default(), d);
    paper(&b, 13, 0.25f);
    b.texture.scale = 2.0f;
    b.texture.pattern.size = 256;
    add(bd, "Watercolor Wash", &b);

    start(bd, &b, 8.0f, 1.0f);
    b.spacing = 0.05f;
    size_dyn(&b, pressure_min(0.15f));
    b.smoothing.amount = 0.35f;
    add(bd, "Ink Pen", &b);

    start(bd, &b, 70.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_bristle_tip(64, 41, &t, bd->err), &t);
    b.spacing = 0.03f;
    angle_dir(&b);
    b.shape_dynamics.size = pressure_min(0.6f);
    b.color_dynamics.enabled = true;
    b.color_dynamics.per_tip = false;
    b.color_dynamics.fg_bg = pc_paint_dynamic_jitter(0.15f);
    b.color_dynamics.brightness_jitter = 0.05f;
    textured(&b, PC_PAINT_STYLE_CANVAS, 64, 4, 0.3f);
    add(bd, "Oil Bristle", &b);

    start(bd, &b, 40.0f, 0.9f);
    b.roundness = 0.35f;
    b.spacing = 0.05f;
    angle_dir(&b);
    transfer(&b, pc_paint_dynamic_default(), (pc_paint_dynamic){0.0f, PC_PAINT_CONTROL_FADE, 600, 0.35f});
    add(bd, "Gouache Flat", &b);

    start(bd, &b, 70.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_sponge_tip(96, 8, &t, bd->err), &t);
    b.spacing = 0.3f;
    b.flow = 0.6f;
    b.wet_edges = true;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.size = pc_paint_dynamic_jitter(0.2f);
    add(bd, "Ink Wash Sponge", &b);
}

static void special_effects(builder *bd)
{
    pc_paint_brush b;
    pc_paint_gray_tile t;
    bd->group = PC_PAINT_GROUPS[3];
    start(bd, &b, 60.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_spatter_tip(64, 11, 14, &t, bd->err), &t);
    b.spacing = 0.6f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = jit(0.6f, 0.2f);
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.flip_x_jitter = true;
    b.shape_dynamics.flip_y_jitter = true;
    b.scattering =
        (pc_paint_scattering){true, pc_paint_dynamic_jitter(1.5f), true, 2, pc_paint_dynamic_jitter(0.5f)};
    add(bd, "Spatter", &b);

    start(bd, &b, 40.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_spatter_tip(64, 77, 40, &t, bd->err), &t);
    b.spacing = 0.5f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.flip_x_jitter = true;
    b.scattering.enabled = true;
    b.scattering.scatter = pc_paint_dynamic_jitter(0.6f);
    b.scattering.both_axes = true;
    transfer(&b, pressure(), pc_paint_dynamic_default());
    add(bd, "Stipple", &b);

    start(bd, &b, 24.0f, 1.0f);
    b.roundness = 0.5f;
    b.spacing = 1.2f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.size = pc_paint_dynamic_jitter(0.5f);
    b.scattering.enabled = true;
    b.scattering.scatter = pc_paint_dynamic_jitter(3.0f);
    b.scattering.both_axes = true;
    b.scattering.count = 3;
    b.color_dynamics.enabled = true;
    b.color_dynamics.per_tip = true;
    b.color_dynamics.hue_jitter = 1.0f;
    b.color_dynamics.saturation_jitter = 0.2f;
    b.color_dynamics.purity = 0.5f;
    add(bd, "Confetti", &b);

    start(bd, &b, 50.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_leaf_tip(64, &t, bd->err), &t);
    b.spacing = 0.35f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = jit(0.5f, 0.3f);
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.roundness = jit(0.4f, 0.4f);
    b.shape_dynamics.flip_y_jitter = true;
    b.scattering =
        (pc_paint_scattering){true, pc_paint_dynamic_jitter(1.2f), true, 2, pc_paint_dynamic_jitter(0.5f)};
    b.color_dynamics.enabled = true;
    b.color_dynamics.per_tip = true;
    b.color_dynamics.fg_bg = pc_paint_dynamic_jitter(0.6f);
    b.color_dynamics.hue_jitter = 0.04f;
    b.color_dynamics.brightness_jitter = 0.12f;
    add(bd, "Foliage", &b);

    start(bd, &b, 70.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_grass_tip(96, 6, 11, &t, bd->err), &t);
    b.spacing = 0.25f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = pressure();
    b.shape_dynamics.size.jitter = 0.4f;
    b.shape_dynamics.size.minimum = 0.4f;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(0.06f);
    b.shape_dynamics.flip_x_jitter = true;
    b.scattering.enabled = true;
    b.scattering.scatter = pc_paint_dynamic_jitter(0.3f);
    b.color_dynamics.enabled = true;
    b.color_dynamics.per_tip = true;
    b.color_dynamics.fg_bg = pc_paint_dynamic_jitter(0.7f);
    b.color_dynamics.hue_jitter = 0.03f;
    add(bd, "Grass", &b);

    start(bd, &b, 40.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_star_tip(64, &t, bd->err), &t);
    b.spacing = 1.5f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = jit(0.8f, 0.15f);
    b.scattering.enabled = true;
    b.scattering.scatter = pc_paint_dynamic_jitter(2.5f);
    b.scattering.both_axes = true;
    transfer(&b, pc_paint_dynamic_jitter(0.5f), pc_paint_dynamic_default());
    add(bd, "Sparkle", &b);

    start(bd, &b, 60.0f, bd->base->hardness);
    sampled(bd, &b.tip, pc_paint_sponge_tip(64, 19, &t, bd->err), &t);
    b.spacing = 0.45f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    transfer(&b, pc_paint_dynamic_jitter(0.3f), pc_paint_dynamic_default());
    add(bd, "Sponge Texture", &b);

    start(bd, &b, 50.0f, 0.8f);
    textured(&b, PC_PAINT_STYLE_CANVAS, 64, 1, 0.7f);
    b.texture.mode = PC_PAINT_MASK_SUBTRACT;
    /* Subtract takes paint away where the pattern is light; inverting keeps the preset as
     * designed (the dark parts take it away). */
    b.texture.invert = true;
    add(bd, "Canvas Texture", &b);

    start(bd, &b, 80.0f, 0.5f);
    textured(&b, PC_PAINT_STYLE_DOTS, 64, 1, 1.0f);
    b.texture.mode = PC_PAINT_MASK_HARD_MIX;
    add(bd, "Halftone Dots", &b);

    start(bd, &b, 50.0f, 0.6f);
    b.noise = true;
    b.dual_brush.enabled = true;
    sampled(bd, &b.dual_brush.tip, pc_paint_spatter_tip(48, 23, 20, &t, bd->err), &t);
    b.dual_brush.size = 30.0f;
    b.dual_brush.spacing = 0.3f;
    b.dual_brush.scatter = 1.0f;
    b.dual_brush.both_axes = true;
    b.dual_brush.count = 2;
    b.dual_brush.flip = true;
    add(bd, "Dual Grain", &b);
}

pc_status pc_paint_presets_builtin(pc_paint_preset_list *out, pc_error *err)
{
    pc_paint_brush base;
    pc_paint_preset_list l = {NULL, 0};
    builder bd;
    l.v = pc_calloc_array(BUILTIN_COUNT, sizeof *l.v);
    if (l.v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for brush presets");
    }
    pc_paint_brush_default(&base);
    base.pressure_size = false;
    base.spacing = 0.25f;
    bd = (builder){&l, &base, NULL, PC_OK, err};
    general(&bd);
    dry_media(&bd);
    wet_media(&bd);
    special_effects(&bd);
    if (bd.st != PC_OK) {
        pc_paint_preset_list_fini(&l);
        return bd.st;
    }
    *out = l;
    return PC_OK;
}
