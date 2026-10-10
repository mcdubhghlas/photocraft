/* The brush model: defaults, copies, equality and the preset-picking rules (brush.h). */
#include "paint/brush.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"

/* ---- defaults ---- */

pc_paint_dynamic pc_paint_dynamic_default(void)
{
    return (pc_paint_dynamic){0.0f, PC_PAINT_CONTROL_OFF, 25, 0.0f};
}

pc_paint_dynamic pc_paint_dynamic_controlled(pc_paint_control c)
{
    return (pc_paint_dynamic){0.0f, c, 25, 0.0f};
}

pc_paint_dynamic pc_paint_dynamic_jitter(float jitter)
{
    return (pc_paint_dynamic){jitter, PC_PAINT_CONTROL_OFF, 25, 0.0f};
}

bool pc_paint_dynamic_is_active(const pc_paint_dynamic *d)
{
    return d->jitter > 0.0f || d->control != PC_PAINT_CONTROL_OFF;
}

void pc_paint_tip_default(pc_paint_tip *t)
{
    memset(t, 0, sizeof *t);
    t->kind = PC_PAINT_TIP_ROUND;
}

void pc_paint_shape_dynamics_default(pc_paint_shape_dynamics *s)
{
    memset(s, 0, sizeof *s);
    s->size = pc_paint_dynamic_default();
    s->angle = pc_paint_dynamic_default();
    s->roundness = pc_paint_dynamic_default();
}

void pc_paint_scattering_default(pc_paint_scattering *s)
{
    memset(s, 0, sizeof *s);
    s->scatter = pc_paint_dynamic_default();
    s->count = 1;
    s->count_jitter = pc_paint_dynamic_default();
}

void pc_paint_pattern_default(pc_paint_pattern *p)
{
    memset(p, 0, sizeof *p);
    p->kind = PC_PAINT_PATTERN_PROCEDURAL;
    p->style = PC_PAINT_STYLE_PAPER;
    p->size = 128;
    p->seed = 1;
}

void pc_paint_texture_default(pc_paint_texture *t)
{
    memset(t, 0, sizeof *t);
    pc_paint_pattern_default(&t->pattern);
    t->scale = 1.0f;
    t->mode = PC_PAINT_MASK_MULTIPLY;
    t->depth = 1.0f;
    t->depth_jitter = pc_paint_dynamic_default();
}

void pc_paint_dual_brush_default(pc_paint_dual_brush *d)
{
    memset(d, 0, sizeof *d);
    d->mode = PC_PAINT_MASK_MULTIPLY;
    pc_paint_tip_default(&d->tip);
    d->size = 20.0f;
    d->hardness = 1.0f;
    d->roundness = 1.0f;
    d->spacing = 0.25f;
    d->count = 1;
}

void pc_paint_color_dynamics_default(pc_paint_color_dynamics *c)
{
    memset(c, 0, sizeof *c);
    c->per_tip = true;
    c->fg_bg = pc_paint_dynamic_default();
}

void pc_paint_transfer_default(pc_paint_transfer *t)
{
    memset(t, 0, sizeof *t);
    t->opacity = pc_paint_dynamic_default();
    t->flow = pc_paint_dynamic_default();
    t->wetness = pc_paint_dynamic_default();
    t->mix = pc_paint_dynamic_default();
}

void pc_paint_section_locks_default(pc_paint_section_locks *l)
{
    memset(l, 0, sizeof *l);
}

void pc_paint_pose_default(pc_paint_pose *p)
{
    memset(p, 0, sizeof *p);
    p->pressure = 1.0f;
}

void pc_paint_smoothing_default(pc_paint_smoothing *s)
{
    memset(s, 0, sizeof *s);
    s->catch_up = true;
    s->catch_up_on_end = true;
    s->adjust_for_zoom = true;
}

void pc_paint_mixer_default(pc_paint_mixer_settings *m)
{
    m->wet = 0.5f;
    m->load = 0.5f;
    m->mix = 0.5f;
    m->flow = 1.0f;
    m->sample_all_layers = false;
}

void pc_paint_brush_default(pc_paint_brush *b)
{
    memset(b, 0, sizeof *b);
    b->size = 20.0f;
    b->hardness = 1.0f;
    b->spacing = 0.1f;
    b->spacing_enabled = true;
    b->opacity = 1.0f;
    b->flow = 1.0f;
    b->pressure_size = true;
    b->color[3] = 1.0f;
    for (int i = 0; i < 4; i++) {
        b->background[i] = 1.0f;
    }
    b->mode = PC_BLEND_NORMAL;
    pc_paint_tip_default(&b->tip);
    b->roundness = 1.0f;
    pc_paint_shape_dynamics_default(&b->shape_dynamics);
    pc_paint_scattering_default(&b->scattering);
    pc_paint_texture_default(&b->texture);
    pc_paint_dual_brush_default(&b->dual_brush);
    pc_paint_color_dynamics_default(&b->color_dynamics);
    pc_paint_transfer_default(&b->transfer);
    pc_paint_pose_default(&b->pose);
    b->build_up_rate = 20.0f;
    pc_paint_smoothing_default(&b->smoothing);
    pc_paint_section_locks_default(&b->locks);
    pc_paint_mixer_default(&b->mixer);
}

/* ---- copies ---- */

void pc_paint_tip_copy(pc_paint_tip *dst, const pc_paint_tip *src)
{
    dst->kind = src->kind;
    pc_paint_gray_tile_copy(&dst->tile, &src->tile);
}

void pc_paint_tip_fini(pc_paint_tip *t)
{
    pc_paint_gray_tile_fini(&t->tile);
}

void pc_paint_pattern_copy(pc_paint_pattern *dst, const pc_paint_pattern *src)
{
    *dst = *src;
    pc_paint_gray_tile_copy(&dst->tile, &src->tile);
}

void pc_paint_pattern_fini(pc_paint_pattern *p)
{
    pc_paint_gray_tile_fini(&p->tile);
}

void pc_paint_texture_copy(pc_paint_texture *dst, const pc_paint_texture *src)
{
    *dst = *src;
    pc_paint_pattern_copy(&dst->pattern, &src->pattern);
}

void pc_paint_texture_fini(pc_paint_texture *t)
{
    pc_paint_pattern_fini(&t->pattern);
}

void pc_paint_dual_brush_copy(pc_paint_dual_brush *dst, const pc_paint_dual_brush *src)
{
    *dst = *src;
    pc_paint_tip_copy(&dst->tip, &src->tip);
}

void pc_paint_dual_brush_fini(pc_paint_dual_brush *d)
{
    pc_paint_tip_fini(&d->tip);
}

void pc_paint_brush_copy(pc_paint_brush *dst, const pc_paint_brush *src)
{
    *dst = *src;
    pc_paint_tip_copy(&dst->tip, &src->tip);
    pc_paint_texture_copy(&dst->texture, &src->texture);
    pc_paint_dual_brush_copy(&dst->dual_brush, &src->dual_brush);
}

void pc_paint_brush_fini(pc_paint_brush *b)
{
    if (b == NULL) {
        return;
    }
    pc_paint_tip_fini(&b->tip);
    pc_paint_texture_fini(&b->texture);
    pc_paint_dual_brush_fini(&b->dual_brush);
}

/* Each assign copies into a temporary first, so src may alias dst. */
void pc_paint_tip_assign(pc_paint_tip *dst, const pc_paint_tip *src)
{
    pc_paint_tip t;
    pc_paint_tip_copy(&t, src);
    pc_paint_tip_fini(dst);
    *dst = t;
}

void pc_paint_pattern_assign(pc_paint_pattern *dst, const pc_paint_pattern *src)
{
    pc_paint_pattern t;
    pc_paint_pattern_copy(&t, src);
    pc_paint_pattern_fini(dst);
    *dst = t;
}

void pc_paint_texture_assign(pc_paint_texture *dst, const pc_paint_texture *src)
{
    pc_paint_texture t;
    pc_paint_texture_copy(&t, src);
    pc_paint_texture_fini(dst);
    *dst = t;
}

void pc_paint_dual_brush_assign(pc_paint_dual_brush *dst, const pc_paint_dual_brush *src)
{
    pc_paint_dual_brush t;
    pc_paint_dual_brush_copy(&t, src);
    pc_paint_dual_brush_fini(dst);
    *dst = t;
}

void pc_paint_brush_assign(pc_paint_brush *dst, const pc_paint_brush *src)
{
    pc_paint_brush t;
    pc_paint_brush_copy(&t, src);
    pc_paint_brush_fini(dst);
    *dst = t;
}

/* ---- equality ---- */

static bool f4_eq(const float a[4], const float b[4])
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

bool pc_paint_dynamic_eq(const pc_paint_dynamic *a, const pc_paint_dynamic *b)
{
    return a->jitter == b->jitter && a->control == b->control && a->fade_steps == b->fade_steps &&
           a->minimum == b->minimum;
}

bool pc_paint_tip_eq(const pc_paint_tip *a, const pc_paint_tip *b)
{
    return a->kind == b->kind && (a->kind == PC_PAINT_TIP_ROUND || pc_paint_gray_tile_eq(&a->tile, &b->tile));
}

bool pc_paint_shape_dynamics_eq(const pc_paint_shape_dynamics *a, const pc_paint_shape_dynamics *b)
{
    return a->enabled == b->enabled && pc_paint_dynamic_eq(&a->size, &b->size) &&
           a->tilt_scale == b->tilt_scale && pc_paint_dynamic_eq(&a->angle, &b->angle) &&
           pc_paint_dynamic_eq(&a->roundness, &b->roundness) && a->flip_x_jitter == b->flip_x_jitter &&
           a->flip_y_jitter == b->flip_y_jitter && a->brush_projection == b->brush_projection;
}

bool pc_paint_scattering_eq(const pc_paint_scattering *a, const pc_paint_scattering *b)
{
    return a->enabled == b->enabled && pc_paint_dynamic_eq(&a->scatter, &b->scatter) &&
           a->both_axes == b->both_axes && a->count == b->count &&
           pc_paint_dynamic_eq(&a->count_jitter, &b->count_jitter);
}

bool pc_paint_pattern_eq(const pc_paint_pattern *a, const pc_paint_pattern *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    if (a->kind == PC_PAINT_PATTERN_TILE) {
        return pc_paint_gray_tile_eq(&a->tile, &b->tile);
    }
    return a->style == b->style && a->size == b->size && a->seed == b->seed;
}

bool pc_paint_texture_eq(const pc_paint_texture *a, const pc_paint_texture *b)
{
    return a->enabled == b->enabled && pc_paint_pattern_eq(&a->pattern, &b->pattern) &&
           a->invert == b->invert && a->scale == b->scale && a->brightness == b->brightness &&
           a->contrast == b->contrast && a->each_tip == b->each_tip && a->mode == b->mode &&
           a->depth == b->depth && pc_paint_dynamic_eq(&a->depth_jitter, &b->depth_jitter);
}

bool pc_paint_dual_brush_eq(const pc_paint_dual_brush *a, const pc_paint_dual_brush *b)
{
    return a->enabled == b->enabled && a->mode == b->mode && pc_paint_tip_eq(&a->tip, &b->tip) &&
           a->size == b->size && a->hardness == b->hardness && a->roundness == b->roundness &&
           a->angle == b->angle && a->spacing == b->spacing && a->scatter == b->scatter &&
           a->both_axes == b->both_axes && a->count == b->count && a->flip == b->flip;
}

bool pc_paint_color_dynamics_eq(const pc_paint_color_dynamics *a, const pc_paint_color_dynamics *b)
{
    return a->enabled == b->enabled && a->per_tip == b->per_tip &&
           pc_paint_dynamic_eq(&a->fg_bg, &b->fg_bg) && a->hue_jitter == b->hue_jitter &&
           a->saturation_jitter == b->saturation_jitter && a->brightness_jitter == b->brightness_jitter &&
           a->purity == b->purity;
}

bool pc_paint_transfer_eq(const pc_paint_transfer *a, const pc_paint_transfer *b)
{
    return a->enabled == b->enabled && pc_paint_dynamic_eq(&a->opacity, &b->opacity) &&
           pc_paint_dynamic_eq(&a->flow, &b->flow) && pc_paint_dynamic_eq(&a->wetness, &b->wetness) &&
           pc_paint_dynamic_eq(&a->mix, &b->mix);
}

bool pc_paint_section_locks_eq(const pc_paint_section_locks *a, const pc_paint_section_locks *b)
{
    return a->shape_dynamics == b->shape_dynamics && a->scattering == b->scattering &&
           a->texture == b->texture && a->dual_brush == b->dual_brush &&
           a->color_dynamics == b->color_dynamics && a->transfer == b->transfer && a->pose == b->pose &&
           a->noise == b->noise && a->wet_edges == b->wet_edges && a->build_up == b->build_up &&
           a->smoothing == b->smoothing && a->protect_texture == b->protect_texture;
}

bool pc_paint_pose_eq(const pc_paint_pose *a, const pc_paint_pose *b)
{
    return a->enabled == b->enabled && a->tilt_x == b->tilt_x && a->tilt_y == b->tilt_y &&
           a->rotation == b->rotation && a->pressure == b->pressure && a->override_tilt == b->override_tilt &&
           a->override_rotation == b->override_rotation && a->override_pressure == b->override_pressure;
}

bool pc_paint_smoothing_eq(const pc_paint_smoothing *a, const pc_paint_smoothing *b)
{
    return a->amount == b->amount && a->pulled_string == b->pulled_string && a->catch_up == b->catch_up &&
           a->catch_up_on_end == b->catch_up_on_end && a->adjust_for_zoom == b->adjust_for_zoom;
}

bool pc_paint_mixer_eq(const pc_paint_mixer_settings *a, const pc_paint_mixer_settings *b)
{
    return a->wet == b->wet && a->load == b->load && a->mix == b->mix && a->flow == b->flow &&
           a->sample_all_layers == b->sample_all_layers;
}

bool pc_paint_brush_eq(const pc_paint_brush *a, const pc_paint_brush *b)
{
    return a->size == b->size && a->hardness == b->hardness && a->spacing == b->spacing &&
           a->spacing_enabled == b->spacing_enabled && a->opacity == b->opacity && a->flow == b->flow &&
           a->pressure_size == b->pressure_size && a->pressure_opacity == b->pressure_opacity &&
           f4_eq(a->color, b->color) && f4_eq(a->background, b->background) && a->erase == b->erase &&
           a->mode == b->mode && pc_paint_tip_eq(&a->tip, &b->tip) && a->angle == b->angle &&
           a->roundness == b->roundness && a->flip_x == b->flip_x && a->flip_y == b->flip_y &&
           a->aliased == b->aliased && pc_paint_shape_dynamics_eq(&a->shape_dynamics, &b->shape_dynamics) &&
           pc_paint_scattering_eq(&a->scattering, &b->scattering) &&
           pc_paint_texture_eq(&a->texture, &b->texture) &&
           pc_paint_dual_brush_eq(&a->dual_brush, &b->dual_brush) &&
           pc_paint_color_dynamics_eq(&a->color_dynamics, &b->color_dynamics) &&
           pc_paint_transfer_eq(&a->transfer, &b->transfer) && pc_paint_pose_eq(&a->pose, &b->pose) &&
           a->noise == b->noise && a->wet_edges == b->wet_edges && a->build_up == b->build_up &&
           a->build_up_rate == b->build_up_rate && pc_paint_smoothing_eq(&a->smoothing, &b->smoothing) &&
           a->protect_texture == b->protect_texture && pc_paint_section_locks_eq(&a->locks, &b->locks) &&
           pc_paint_mixer_eq(&a->mixer, &b->mixer) && a->seed == b->seed;
}

/* ---- operations ---- */

static float safe_size(float size)
{
    return isfinite(size) ? pc_clampf(size, 0.5f, PC_PAINT_MAX_BRUSH_SIZE) : 0.5f;
}

/* NaN scatters nothing (it never passed the `> 0` check); infinity is the maximum. */
static float safe_scatter(float amount)
{
    return isnan(amount) ? 0.0f : pc_clampf(amount, 0.0f, PC_PAINT_MAX_SCATTER);
}

void pc_paint_brush_bounded(const pc_paint_brush *b, pc_paint_brush *out)
{
    pc_paint_brush_copy(out, b);
    out->size = safe_size(out->size);
    out->dual_brush.size = safe_size(out->dual_brush.size);
    out->scattering.scatter.jitter = safe_scatter(out->scattering.scatter.jitter);
    out->dual_brush.scatter = safe_scatter(out->dual_brush.scatter);
}

void pc_paint_brush_with_protected_texture(pc_paint_brush *b, const pc_paint_brush *current)
{
    if (current->protect_texture && current->texture.enabled && b->texture.enabled) {
        pc_paint_pattern_assign(&b->texture.pattern, &current->texture.pattern);
        b->texture.scale = current->texture.scale;
        b->protect_texture = true;
    }
}

void pc_paint_brush_picked_over(pc_paint_brush *b, const pc_paint_brush *current)
{
    const pc_paint_section_locks *l = &current->locks;
    pc_paint_brush_with_protected_texture(b, current);
    b->smoothing = current->smoothing;
    if (l->shape_dynamics) {
        b->shape_dynamics = current->shape_dynamics;
    }
    if (l->scattering) {
        b->scattering = current->scattering;
    }
    if (l->texture) {
        pc_paint_texture_assign(&b->texture, &current->texture);
    }
    if (l->dual_brush) {
        pc_paint_dual_brush_assign(&b->dual_brush, &current->dual_brush);
    }
    if (l->color_dynamics) {
        b->color_dynamics = current->color_dynamics;
    }
    if (l->transfer) {
        b->transfer = current->transfer;
    }
    if (l->pose) {
        b->pose = current->pose;
    }
    if (l->noise) {
        b->noise = current->noise;
    }
    if (l->wet_edges) {
        b->wet_edges = current->wet_edges;
    }
    if (l->build_up) {
        b->build_up = current->build_up;
        b->build_up_rate = current->build_up_rate;
    }
    if (l->protect_texture) {
        b->protect_texture = current->protect_texture;
    }
    b->locks = current->locks;
}

/* ---- stroke data ---- */

pc_paint_point pc_paint_point_default(void)
{
    return (pc_paint_point){0.0, 0.0, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0};
}

pc_paint_point pc_paint_point_make(double x, double y, float pressure)
{
    pc_paint_point p = pc_paint_point_default();
    p.x = x;
    p.y = y;
    p.pressure = pressure;
    return p;
}

bool pc_paint_point_eq(const pc_paint_point *a, const pc_paint_point *b)
{
    return a->x == b->x && a->y == b->y && a->pressure == b->pressure && a->tilt_x == b->tilt_x &&
           a->tilt_y == b->tilt_y && a->rotation == b->rotation && a->wheel == b->wheel && a->time == b->time;
}

void pc_paint_stroke_fini(pc_paint_stroke *s)
{
    if (s == NULL) {
        return;
    }
    pc_paint_brush_fini(&s->brush);
    free(s->points);
    s->points = NULL;
    s->count = 0;
}

bool pc_paint_stroke_eq(const pc_paint_stroke *a, const pc_paint_stroke *b)
{
    if (!pc_paint_brush_eq(&a->brush, &b->brush) || a->count != b->count) {
        return false;
    }
    for (size_t i = 0; i < a->count; i++) {
        if (!pc_paint_point_eq(&a->points[i], &b->points[i])) {
            return false;
        }
    }
    return true;
}

pc_paint_dab pc_paint_dab_round(pc_point center, float radius, float alpha)
{
    pc_paint_dab d;
    memset(&d, 0, sizeof d);
    d.center = center;
    d.radius = radius;
    d.alpha = alpha;
    d.roundness = 1.0f;
    d.opacity = 1.0f;
    d.color[3] = 1.0f;
    d.depth = 1.0f;
    d.proj_scale = 1.0f;
    d.wet = 1.0f;
    d.mix = 1.0f;
    return d;
}

bool pc_paint_dab_eq(const pc_paint_dab *a, const pc_paint_dab *b)
{
    return a->center.x == b->center.x && a->center.y == b->center.y && a->radius == b->radius &&
           a->alpha == b->alpha && a->angle == b->angle && a->roundness == b->roundness &&
           a->flip_x == b->flip_x && a->flip_y == b->flip_y && a->opacity == b->opacity &&
           f4_eq(a->color, b->color) && a->depth == b->depth && a->index == b->index &&
           a->proj_angle == b->proj_angle && a->proj_scale == b->proj_scale && a->wet == b->wet &&
           a->mix == b->mix;
}

pc_status pc_paint_dab_vec_push(pc_paint_dab_vec *a, const pc_paint_dab *d, pc_error *err)
{
    if (a->len == a->cap) {
        pc_paint_dab *nv = pc_grow_array(a->v, &a->cap, a->len + 1, sizeof *nv);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for dabs");
        }
        a->v = nv;
    }
    a->v[a->len++] = *d;
    return PC_OK;
}

void pc_paint_dab_vec_fini(pc_paint_dab_vec *a)
{
    free(a->v);
    a->v = NULL;
    a->len = 0;
    a->cap = 0;
}

pc_status pc_paint_point_vec_push(pc_paint_point_vec *a, const pc_paint_point *p, pc_error *err)
{
    if (a->len == a->cap) {
        pc_paint_point *nv = pc_grow_array(a->v, &a->cap, a->len + 1, sizeof *nv);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for stroke points");
        }
        a->v = nv;
    }
    a->v[a->len++] = *p;
    return PC_OK;
}

void pc_paint_point_vec_fini(pc_paint_point_vec *a)
{
    free(a->v);
    a->v = NULL;
    a->len = 0;
    a->cap = 0;
}

float pc_paint_dab_coverage(float d, float radius, float hardness)
{
    float edge, h, inner, falloff;
    if (d >= radius + 0.5f) {
        return 0.0f;
    }
    /* The reference's `clamp(0, 1).min(1.0)`: NaN gives 1. Tested on the input because clang 23
     * folds a literal min over a clamp into a NaN-propagating minss. */
    edge = radius + 0.5f - d;
    edge = isnan(edge) ? 1.0f : pc_clampf(edge, 0.0f, 1.0f);
    h = pc_clampf(hardness, 0.0f, 1.0f);
    inner = radius * h;
    if (d <= inner || radius - inner < 1e-3f) {
        falloff = 1.0f;
    } else {
        float t = pc_clampf((d - inner) / (radius - inner), 0.0f, 1.0f);
        falloff = 1.0f - t * t * (3.0f - 2.0f * t); /* smoothstep falloff */
    }
    return edge * falloff;
}

pc_status pc_paint_smooth(const pc_paint_point *pts, size_t n, float amount, pc_paint_point_vec *out,
                          pc_error *err)
{
    double a = (double)(1.0f - pc_clampf(amount, 0.0f, 0.95f));
    size_t start = out->len;
    pc_paint_point cur = {0};
    for (size_t i = 0; i < n; i++) {
        pc_paint_point p = pts[i];
        if (i > 0) {
            pc_paint_point m = p;
            m.x = cur.x + (p.x - cur.x) * a;
            m.y = cur.y + (p.y - cur.y) * a;
            p = m;
        }
        if (pc_paint_point_vec_push(out, &p, err) != PC_OK) {
            out->len = start;
            return PC_ERR_NOMEM;
        }
        cur = p;
    }
    if (n > 0) {
        out->v[out->len - 1] = pts[n - 1];
    }
    return PC_OK;
}
