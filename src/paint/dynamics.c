/* Points to dabs: smoothing, path walking and per-dab dynamics (dynamics.h).
 *
 * Float expressions keep the reference's types and operation order; `a.min(b)` / `a.max(b)`
 * use the pc_rs_* helpers (the _k forms where the reference has a literal operand). */
#include "paint/dynamics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"
#include "paint/render.h"
#include "paint/rng.h"

/* Longest pixel-grid segment (in pixels) before falling back to distance spacing. */
#define MAX_GRID_STEPS 1e7
/* Densest speed spacing, in pixels between dabs. */
#define MIN_SPEED_STEP 0.5
/* Maximum airbrush catch-up dabs for one input segment. */
#define MAX_AIRBRUSH_DABS_PER_SEGMENT 512

/* Rust's f32::to_degrees / to_radians constants. */
#define DEG_PER_RAD_F 57.2957795130823208767981548141051703f
#define RAD_PER_DEG_F (3.14159265358979323846f / 180.0f)

static float to_degrees(float r)
{
    return r * DEG_PER_RAD_F;
}

static float to_radians(float d)
{
    return d * RAD_PER_DEG_F;
}

static float rem_euclid_f(float a, float b)
{
    float r = fmodf(a, b);
    return r < 0.0f ? r + fabsf(b) : r;
}

static double rem_euclid_d(double a, double b)
{
    double r = fmod(a, b);
    return r < 0.0 ? r + fabs(b) : r;
}

static pc_paint_point lerp_pt(const pc_paint_point *a, const pc_paint_point *b, double f)
{
    float ff = pc_f64_to_f32(f);
    pc_paint_point p;
    p.x = a->x + (b->x - a->x) * f;
    p.y = a->y + (b->y - a->y) * f;
    p.pressure = a->pressure + (b->pressure - a->pressure) * ff;
    p.tilt_x = a->tilt_x + (b->tilt_x - a->tilt_x) * ff;
    p.tilt_y = a->tilt_y + (b->tilt_y - a->tilt_y) * ff;
    p.rotation = a->rotation + (b->rotation - a->rotation) * ff;
    p.wheel = a->wheel + (b->wheel - a->wheel) * ff;
    p.time = a->time + (b->time - a->time) * f;
    return p;
}

/* ---- smoothing ---- */

void pc_paint_smoother_init(pc_paint_smoother *s, const pc_paint_smoothing *cfg, float zoom)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->zoom = (double)pc_paint_maxf_k(zoom, 0.01f);
}

static bool smoother_active(const pc_paint_smoother *s)
{
    return s->cfg.amount > 0.0f;
}

pc_status pc_paint_smoother_push(pc_paint_smoother *s, pc_paint_point p, pc_paint_point_vec *out,
                                 pc_error *err)
{
    bool had_prev = s->has_last;
    pc_paint_point prev = s->last_input, cur;
    double amount, a, dt, step;
    size_t iters = 1;
    s->last_input = p;
    s->has_last = true;
    if (!smoother_active(s)) {
        return pc_paint_point_vec_push(out, &p, err);
    }
    if (!s->has_pos) {
        s->pos = p;
        s->has_pos = true;
        return pc_paint_point_vec_push(out, &p, err);
    }
    amount = (double)pc_clampf(s->cfg.amount, 0.0f, 1.0f);
    if (s->cfg.pulled_string) {
        double len = amount * PC_PAINT_MAX_STRING_PX, dx = p.x - s->pos.x, dy = p.y - s->pos.y, d;
        if (s->cfg.adjust_for_zoom) {
            len /= s->zoom;
        }
        d = sqrt(dx * dx + dy * dy);
        if (d > len) {
            double k = (d - len) / d;
            pc_paint_point np = p;
            np.x = s->pos.x + dx * k;
            np.y = s->pos.y + dy * k;
            s->pos = np;
            return pc_paint_point_vec_push(out, &np, err);
        }
        return PC_OK;
    }
    /* Exponential: every SMOOTHING_STEP_MS of stroke time moves `a` of the way towards the
     * pointer, so the feel doesn't depend on how often the device reports. Input without
     * timestamps takes one step per point. */
    a = 1.0 - pc_paint_min_k(amount, 0.95);
    dt = had_prev ? p.time - prev.time : 0.0;
    if (had_prev && prev.x == p.x && prev.y == p.y && !s->cfg.catch_up) {
        /* Without Stroke Catch-Up the brush stops while the pointer pauses (Photoshop). */
        return PC_OK;
    }
    if (dt > 0.0 && isfinite(dt)) {
        double steps = dt / PC_PAINT_SMOOTHING_STEP_MS, it = pc_clampd(ceil(steps), 1.0, 64.0);
        /* Each of `iters` moves covers `steps / iters` steps' worth of pull. */
        iters = pc_f64_to_size(it);
        step = 1.0 - pow(1.0 - a, pc_paint_min_k(steps, 64.0) / it);
    } else {
        step = a;
    }
    cur = s->pos;
    for (size_t i = 0; i < iters; i++) {
        pc_paint_point n = p;
        n.x = cur.x + (p.x - cur.x) * step;
        n.y = cur.y + (p.y - cur.y) * step;
        cur = n;
        if (pc_paint_point_vec_push(out, &cur, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    s->pos = cur;
    return PC_OK;
}

bool pc_paint_smoother_lagging(const pc_paint_smoother *s)
{
    if (!(smoother_active(s) && s->cfg.catch_up && !s->cfg.pulled_string) || !s->has_pos || !s->has_last) {
        return false;
    }
    return fabs(s->pos.x - s->last_input.x) + fabs(s->pos.y - s->last_input.y) > 0.01;
}

bool pc_paint_smoother_string(const pc_paint_smoother *s, double brush[2], double pointer[2])
{
    if (!s->has_pos || !s->has_last || !smoother_active(s) ||
        (s->pos.x == s->last_input.x && s->pos.y == s->last_input.y)) {
        return false;
    }
    brush[0] = s->pos.x;
    brush[1] = s->pos.y;
    pointer[0] = s->last_input.x;
    pointer[1] = s->last_input.y;
    return true;
}

pc_status pc_paint_smoother_finish(pc_paint_smoother *s, pc_paint_point_vec *out, pc_error *err)
{
    if (!smoother_active(s) || !s->cfg.catch_up_on_end) {
        return PC_OK;
    }
    if (s->has_pos && s->has_last && (s->pos.x != s->last_input.x || s->pos.y != s->last_input.y)) {
        s->pos = s->last_input;
        return pc_paint_point_vec_push(out, &s->last_input, err);
    }
    return PC_OK;
}

pc_status pc_paint_smooth_points(const pc_paint_smoothing *cfg, float zoom, const pc_paint_point *pts,
                                 size_t n, pc_paint_point_vec *out, pc_error *err)
{
    pc_paint_smoother s;
    pc_paint_smoother_init(&s, cfg, zoom);
    for (size_t i = 0; i < n; i++) {
        pc_status st = pc_paint_smoother_push(&s, pts[i], out, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return pc_paint_smoother_finish(&s, out, err);
}

/* ---- path walking ---- */

void pc_paint_walker_init(pc_paint_walker *w, bool has_interval, double build_up_interval_ms)
{
    memset(w, 0, sizeof *w);
    w->has_interval = has_interval;
    w->interval = build_up_interval_ms;
}

void pc_paint_walker_speed_spacing(pc_paint_walker *w, double interval_ms)
{
    w->has_speed = true;
    w->speed = pc_paint_max_k(interval_ms, 0.1);
}

void pc_paint_walker_pixel_grid(pc_paint_walker *w, double offset)
{
    w->has_grid = true;
    w->grid = offset;
}

static pc_status walker_emit(pc_paint_walker *w, const pc_paint_point *point, pc_paint_step_fn emit,
                             void *ctx, pc_error *err)
{
    pc_paint_step s;
    if (w->step >= PC_PAINT_MAX_STEPS) {
        return pc_fail(err, PC_ERR_LIMIT, "stroke has more than %llu dab steps",
                       (unsigned long long)PC_PAINT_MAX_STEPS);
    }
    s.point = *point;
    s.direction = w->dir;
    s.initial_direction = w->has_initial_dir ? w->initial_dir : w->dir;
    s.step = w->step;
    w->step++;
    return emit(ctx, &s, err);
}

#define TRY(expr)                                                                                            \
    do {                                                                                                     \
        pc_status st_ = (expr);                                                                              \
        if (st_ != PC_OK) {                                                                                  \
            return st_;                                                                                      \
        }                                                                                                    \
    } while (0)

static double grid_cell(double v, double offset)
{
    return offset > 0.0 ? floor(v) : round(v);
}

/* Emits the pixel steps from a to p (see pc_paint_walker_pixel_grid). *done = false, emitting
 * nothing, when the span is not finite or implausibly long: distance spacing takes over. */
static pc_status grid_steps(pc_paint_walker *w, const pc_paint_point *a, const pc_paint_point *p,
                            pc_paint_step_fn emit, void *ctx, bool *done, pc_error *err)
{
    double off = w->grid, x0 = grid_cell(a->x, off), y0 = grid_cell(a->y, off);
    double dx = grid_cell(p->x, off) - x0, dy = grid_cell(p->y, off) - y0;
    double n = pc_rs_max(fabs(dx), fabs(dy));
    uint64_t steps;
    *done = false;
    if (!isfinite(n) || n > MAX_GRID_STEPS) {
        return PC_OK;
    }
    *done = true;
    steps = (uint64_t)n;
    for (uint64_t i = 1; i <= steps; i++) {
        double fi = (double)i;
        pc_paint_point q = lerp_pt(a, p, fi / n);
        q.x = x0 + round(dx * fi / n) + off;
        q.y = y0 + round(dy * fi / n) + off;
        TRY(walker_emit(w, &q, emit, ctx, err));
    }
    return PC_OK;
}

pc_status pc_paint_walker_push(pc_paint_walker *w, pc_paint_point p, pc_paint_step_len_fn len_fn,
                               void *len_ctx, pc_paint_step_fn emit, void *emit_ctx, pc_error *err)
{
    pc_paint_point a;
    double dx, dy, len;
    if (!w->has_last) {
        w->has_last = true;
        w->last = p;
        w->has_pending_first = true;
        w->pending_first = p;
        w->next_at = len_fn(len_ctx, &p);
        return PC_OK;
    }
    a = w->last;
    dx = p.x - a.x;
    dy = p.y - a.y;
    len = sqrt(dx * dx + dy * dy);
    if (len > 1e-9) {
        w->dir = pc_f64_to_f32(atan2(-dy, dx));
        if (!w->has_initial_dir) {
            w->has_initial_dir = true;
            w->initial_dir = w->dir;
        }
    }
    if (w->has_pending_first) {
        pc_paint_point f = w->pending_first;
        w->has_pending_first = false;
        TRY(walker_emit(w, &f, emit, emit_ctx, err));
    }
    if (len > 1e-9) {
        /* Speed spacing needs the segment's duration; without timestamps (a straight shift-click
         * line, or input that carries no times) the segment is spaced by distance instead. */
        double dt = p.time - a.time;
        if (!(w->has_speed && dt > 0.0 && isfinite(dt))) {
            bool gridded = false;
            if (w->has_grid) {
                TRY(grid_steps(w, &a, &p, emit, emit_ctx, &gridded, err));
            }
            if (!gridded) {
                while (w->next_at <= len + 1e-9) {
                    pc_paint_point q = lerp_pt(&a, &p, pc_paint_min_k(w->next_at / len, 1.0));
                    TRY(walker_emit(w, &q, emit, emit_ctx, err));
                    w->next_at += pc_paint_max_k(len_fn(len_ctx, &q), 0.25);
                }
                w->next_at -= len;
            }
        } else {
            double iv = w->speed;
            size_t max_n = pc_f64_to_size(pc_paint_max_k(ceil(len / MIN_SPEED_STEP), 1.0)), n = 0;
            w->speed_acc += dt;
            while (w->speed_acc >= iv && n < max_n) {
                double f;
                pc_paint_point q;
                w->speed_acc -= iv;
                f = pc_clampd((dt - w->speed_acc) / dt, 0.0, 1.0);
                q = lerp_pt(&a, &p, f);
                TRY(walker_emit(w, &q, emit, emit_ctx, err));
                n++;
            }
            if (n == max_n) {
                w->speed_acc = rem_euclid_d(w->speed_acc, iv);
            }
        }
    }
    if (w->has_interval) {
        double iv = w->interval, dt = p.time - a.time;
        if (dt > 0.0 && isfinite(dt)) {
            size_t n = 0;
            if (dt > iv * (double)MAX_AIRBRUSH_DABS_PER_SEGMENT) {
                /* A stalled or hostile timestamp must not replay an arbitrarily long backlog. */
                w->time_acc = 0.0;
                TRY(walker_emit(w, &p, emit, emit_ctx, err));
                w->last = p;
                return PC_OK;
            }
            w->time_acc += dt;
            while (w->time_acc >= iv && n < MAX_AIRBRUSH_DABS_PER_SEGMENT) {
                double f;
                pc_paint_point q;
                w->time_acc -= iv;
                f = pc_clampd((dt - w->time_acc) / dt, 0.0, 1.0);
                q = lerp_pt(&a, &p, f);
                TRY(walker_emit(w, &q, emit, emit_ctx, err));
                n++;
            }
        }
    }
    w->last = p;
    return PC_OK;
}

pc_status pc_paint_walker_finish(pc_paint_walker *w, pc_paint_step_fn emit, void *emit_ctx, pc_error *err)
{
    if (w->has_pending_first) {
        pc_paint_point f = w->pending_first;
        w->has_pending_first = false;
        return walker_emit(w, &f, emit, emit_ctx, err);
    }
    return PC_OK;
}

/* ---- dynamics ---- */

float pc_paint_control_value(pc_paint_control c, uint32_t fade_steps, const pc_paint_step *s)
{
    const pc_paint_point *p = &s->point;
    switch (c) {
    case PC_PAINT_CONTROL_FADE:
        return 1.0f - pc_paint_minf_k((float)s->step / (float)(fade_steps > 1 ? fade_steps : 1), 1.0f);
    case PC_PAINT_CONTROL_PEN_PRESSURE:
        return pc_clampf(p->pressure, 0.0f, 1.0f);
    case PC_PAINT_CONTROL_PEN_TILT:
        /* Upright pen = 1, flat pen = 0 (a mouse reports no tilt, so paints at full value). */
        return 1.0f - pc_clampf(hypotf(p->tilt_x, p->tilt_y) / 90.0f, 0.0f, 1.0f);
    case PC_PAINT_CONTROL_STYLUS_WHEEL:
        return pc_clampf(p->wheel, 0.0f, 1.0f);
    case PC_PAINT_CONTROL_ROTATION:
        return rem_euclid_f(p->rotation, 360.0f) / 360.0f;
    default: /* Off, InitialDirection, Direction */
        return 1.0f;
    }
}

float pc_paint_control_angle(pc_paint_control c, uint32_t fade_steps, const pc_paint_step *s)
{
    const pc_paint_point *p = &s->point;
    switch (c) {
    case PC_PAINT_CONTROL_FADE:
        return 360.0f * pc_paint_minf_k((float)s->step / (float)(fade_steps > 1 ? fade_steps : 1), 1.0f);
    case PC_PAINT_CONTROL_PEN_PRESSURE:
        return 360.0f * pc_clampf(p->pressure, 0.0f, 1.0f);
    case PC_PAINT_CONTROL_PEN_TILT:
        if (p->tilt_x == 0.0f && p->tilt_y == 0.0f) {
            return 0.0f;
        }
        return to_degrees(atan2f(-p->tilt_y, p->tilt_x));
    case PC_PAINT_CONTROL_STYLUS_WHEEL:
        return 360.0f * pc_clampf(p->wheel, 0.0f, 1.0f);
    case PC_PAINT_CONTROL_ROTATION:
        return p->rotation;
    case PC_PAINT_CONTROL_INITIAL_DIRECTION:
        return to_degrees(s->initial_direction);
    case PC_PAINT_CONTROL_DIRECTION:
        return to_degrees(s->direction);
    default: /* Off */
        return 0.0f;
    }
}

/* A 0..1 multiplier from a dynamic: control lerps from `minimum` to 1, jitter reduces randomly,
 * never below `minimum`. */
static float scalar(const pc_paint_dynamic *d, const pc_paint_step *s, float r)
{
    float min, cv, v;
    if (!pc_paint_dynamic_is_active(d)) {
        return 1.0f;
    }
    min = pc_clampf(d->minimum, 0.0f, 1.0f);
    cv = pc_paint_control_value(d->control, d->fade_steps, s);
    v = (min + (1.0f - min) * cv) * (1.0f - pc_clampf(d->jitter, 0.0f, 1.0f) * r);
    return pc_rs_maxf(v, min);
}

static void rgb_to_hsv(const float c[3], float out[3])
{
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float d = mx - mn, h;
    if (d <= 0.0f) {
        h = 0.0f;
    } else if (mx == c[0]) {
        h = rem_euclid_f((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    out[0] = h / 6.0f;
    out[1] = mx > 0.0f ? d / mx : 0.0f;
    out[2] = mx;
}

static void hsv_to_rgb(float h, float s, float v, float out[3])
{
    float h6 = rem_euclid_f(h, 1.0f) * 6.0f;
    float i = floorf(h6), f = h6 - i;
    float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (pc_f32_to_i32(i)) {
    case 0:
        out[0] = v, out[1] = t, out[2] = p;
        break;
    case 1:
        out[0] = q, out[1] = v, out[2] = p;
        break;
    case 2:
        out[0] = p, out[1] = v, out[2] = t;
        break;
    case 3:
        out[0] = p, out[1] = q, out[2] = v;
        break;
    case 4:
        out[0] = t, out[1] = p, out[2] = v;
        break;
    default:
        out[0] = v, out[1] = p, out[2] = q;
        break;
    }
}

void pc_paint_dynamic_color(const pc_paint_brush *b, const pc_paint_step *s, uint64_t seed, uint64_t index,
                            float out[4])
{
    const pc_paint_color_dynamics *cd = &b->color_dynamics;
    const float *fg = b->color, *bg = b->background;
    float base = cd->fg_bg.control != PC_PAINT_CONTROL_OFF
                     ? 1.0f - pc_paint_control_value(cd->fg_bg.control, cd->fg_bg.fade_steps, s)
                     : 0.0f;
    float t = pc_clampf(base + (1.0f - base) * pc_clampf(cd->fg_bg.jitter, 0.0f, 1.0f) *
                                   pc_paint_rand01(seed, index, PC_PAINT_STREAM_FG_BG),
                        0.0f, 1.0f);
    float rgb[3], hsv[3], o[3], pu;
    for (int i = 0; i < 3; i++) {
        rgb[i] = fg[i] + (bg[i] - fg[i]) * t;
    }
    rgb_to_hsv(rgb, hsv);
    hsv[0] +=
        pc_paint_rand_signed(seed, index, PC_PAINT_STREAM_HUE) * pc_clampf(cd->hue_jitter, 0.0f, 1.0f) * 0.5f;
    hsv[1] = pc_clampf(hsv[1] + pc_paint_rand_signed(seed, index, PC_PAINT_STREAM_SAT) *
                                    pc_clampf(cd->saturation_jitter, 0.0f, 1.0f),
                       0.0f, 1.0f);
    pu = pc_clampf(cd->purity, -1.0f, 1.0f);
    hsv[1] = pu >= 0.0f ? hsv[1] + (1.0f - hsv[1]) * pu : hsv[1] * (1.0f + pu);
    hsv[2] = pc_clampf(hsv[2] + pc_paint_rand_signed(seed, index, PC_PAINT_STREAM_BRIGHT) *
                                    pc_clampf(cd->brightness_jitter, 0.0f, 1.0f),
                       0.0f, 1.0f);
    hsv_to_rgb(hsv[0], hsv[1], hsv[2], o);
    out[0] = o[0];
    out[1] = o[1];
    out[2] = o[2];
    out[3] = fg[3] + (bg[3] - fg[3]) * t;
}

float pc_paint_base_diameter(const pc_paint_brush *b, const pc_paint_step *s)
{
    const pc_paint_shape_dynamics *sd = &b->shape_dynamics;
    float f = 1.0f;
    if (sd->enabled && sd->size.control != PC_PAINT_CONTROL_OFF) {
        float min = pc_clampf(sd->size.minimum, 0.0f, 1.0f);
        f = min + (1.0f - min) * pc_paint_control_value(sd->size.control, sd->size.fade_steps, s);
    } else if (b->pressure_size) {
        f = pc_clampf(s->point.pressure, 0.0f, 1.0f);
    }
    return b->size * f;
}

pc_paint_point pc_paint_apply_pose(const pc_paint_brush *b, pc_paint_point p)
{
    const pc_paint_pose *pose = &b->pose;
    if (pose->enabled) {
        if (pose->override_tilt) {
            p.tilt_x = pose->tilt_x;
            p.tilt_y = pose->tilt_y;
        }
        if (pose->override_rotation) {
            p.rotation = pose->rotation;
        }
        if (pose->override_pressure) {
            p.pressure = pose->pressure;
        }
    }
    return p;
}

void pc_paint_dab_builder_init(pc_paint_dab_builder *b, const pc_paint_brush *brush)
{
    memset(b, 0, sizeof *b);
    b->seed = brush->seed;
}

double pc_paint_dab_builder_step_len(const pc_paint_brush *brush, const pc_paint_point *p)
{
    pc_paint_step s;
    float d;
    memset(&s, 0, sizeof s);
    s.point = *p;
    d = pc_paint_maxf_k(pc_paint_base_diameter(brush, &s), 1.0f);
    return pc_paint_max_k((double)(d * pc_paint_maxf_k(brush->spacing, 0.01f)), 0.5);
}

pc_status pc_paint_dab_builder_build(pc_paint_dab_builder *bld, const pc_paint_brush *b,
                                     const pc_paint_step *s, pc_paint_dab_vec *out, pc_error *err)
{
    uint64_t seed = bld->seed;
    const pc_paint_shape_dynamics *sd = &b->shape_dynamics;
    const pc_paint_scattering *sc = &b->scattering;
    const pc_paint_transfer *tr = &b->transfer;
    const pc_paint_texture *tx = &b->texture;
    uint32_t count = 1;
    float base_d;
    if (sc->enabled) {
        float n = (float)(sc->count < 1 ? 1 : (sc->count > 16 ? 16 : sc->count));
        float k = scalar(&sc->count_jitter, s, pc_paint_rand01(seed, s->step, PC_PAINT_STREAM_COUNT));
        uint32_t c = pc_f32_to_u32(roundf(n * k));
        count = c < 1 ? 1 : (c > 16 ? 16 : c);
    }
    base_d = pc_paint_base_diameter(b, s);
    for (uint32_t ci = 0; ci < count; ci++) {
        uint64_t i = bld->next_index++;
        float diameter = base_d, angle = b->angle, roundness = pc_clampf(b->roundness, 0.01f, 1.0f);
        bool fx = b->flip_x, fy = b->flip_y;
        float proj_angle = 0.0f, proj_scale = 1.0f, radius, opacity = 1.0f, flow = b->flow, wet = 1.0f,
              mix = 1.0f, depth;
        pc_point c;
        pc_paint_dab d;
        /* Shape. */
        if (sd->enabled) {
            if (sd->size.jitter > 0.0f) {
                float min = pc_clampf(sd->size.minimum, 0.0f, 1.0f);
                float f = pc_rs_maxf(1.0f - pc_clampf(sd->size.jitter, 0.0f, 1.0f) *
                                                pc_paint_rand01(seed, i, PC_PAINT_STREAM_SIZE),
                                     min);
                diameter *= f;
            }
            angle += pc_paint_control_angle(sd->angle.control, sd->angle.fade_steps, s) +
                     pc_paint_rand_signed(seed, i, PC_PAINT_STREAM_ANGLE) *
                         pc_clampf(sd->angle.jitter, 0.0f, 1.0f) * 180.0f;
            roundness = pc_clampf(
                roundness * scalar(&sd->roundness, s, pc_paint_rand01(seed, i, PC_PAINT_STREAM_ROUNDNESS)),
                0.01f, 1.0f);
            if (sd->flip_x_jitter && pc_paint_rand01(seed, i, PC_PAINT_STREAM_FLIP_X) < 0.5f) {
                fx = !fx;
            }
            if (sd->flip_y_jitter && pc_paint_rand01(seed, i, PC_PAINT_STREAM_FLIP_Y) < 0.5f) {
                fy = !fy;
            }
        }
        /* Tilt Scale and Brush Projection (Shape Dynamics). */
        if (sd->enabled) {
            float tilt = pc_clampf(hypotf(s->point.tilt_x, s->point.tilt_y), 0.0f, 90.0f);
            if (sd->size.control == PC_PAINT_CONTROL_PEN_TILT && sd->tilt_scale > 0.0f) {
                float k = 1.0f - pc_clampf(sd->tilt_scale, 0.0f, 2.0f) * 0.5f * tilt / 90.0f;
                roundness = pc_clampf(roundness * k, 0.01f, 1.0f);
            }
            if (sd->brush_projection) {
                angle += s->point.rotation;
                if (tilt > 0.0f) {
                    proj_angle = atan2f(-s->point.tilt_y, s->point.tilt_x);
                    proj_scale = pc_paint_maxf_k(cosf(to_radians(tilt)), 0.05f);
                }
            }
        }
        radius = pc_paint_maxf_k(diameter / 2.0f, 0.5f);
        /* Scatter. */
        c = pc_point_make(s->point.x, s->point.y);
        if (sc->enabled && sc->scatter.jitter > 0.0f) {
            float cv = pc_paint_control_value(sc->scatter.control, sc->scatter.fade_steps, s);
            double amt = (double)(sc->scatter.jitter * cv * radius);
            double ox = (double)pc_paint_rand_signed(seed, i, PC_PAINT_STREAM_SCATTER_X) * amt;
            double oy = (double)pc_paint_rand_signed(seed, i, PC_PAINT_STREAM_SCATTER_Y) * amt;
            if (sc->both_axes) {
                c.x += ox;
                c.y += oy;
            } else {
                /* Perpendicular to the direction of travel (y-down image space). */
                double dx = (double)cosf(s->direction), dy = -(double)sinf(s->direction);
                c.x += -dy * ox;
                c.y += dx * ox;
            }
        }
        /* Transfer. */
        if (tr->enabled) {
            opacity = scalar(&tr->opacity, s, pc_paint_rand01(seed, i, PC_PAINT_STREAM_OPACITY));
            flow *= scalar(&tr->flow, s, pc_paint_rand01(seed, i, PC_PAINT_STREAM_FLOW));
            wet = scalar(&tr->wetness, s, pc_paint_rand01(seed, i, PC_PAINT_STREAM_WET));
            mix = scalar(&tr->mix, s, pc_paint_rand01(seed, i, PC_PAINT_STREAM_MIX));
        }
        if (b->pressure_opacity && !(tr->enabled && tr->opacity.control != PC_PAINT_CONTROL_OFF)) {
            opacity *= pc_clampf(s->point.pressure, 0.0f, 1.0f);
        }
        memset(&d, 0, sizeof d);
        /* Colour. */
        if (b->color_dynamics.enabled) {
            if (b->color_dynamics.per_tip) {
                pc_paint_dynamic_color(b, s, seed, i, d.color);
            } else {
                if (!bld->has_stroke_color) {
                    pc_paint_dynamic_color(b, s, seed, 0, bld->stroke_color);
                    bld->has_stroke_color = true;
                }
                memcpy(d.color, bld->stroke_color, sizeof d.color);
            }
        } else {
            memcpy(d.color, b->color, sizeof d.color);
        }
        /* Texture depth (per tip). */
        if (tx->enabled && tx->each_tip) {
            depth = pc_clampf(tx->depth, 0.0f, 1.0f) *
                    scalar(&tx->depth_jitter, s, pc_paint_rand01(seed, i, PC_PAINT_STREAM_DEPTH));
        } else {
            depth = pc_clampf(tx->depth, 0.0f, 1.0f);
        }
        d.center = c;
        d.radius = radius;
        d.alpha = pc_clampf(flow, 0.0f, 1.0f);
        d.angle = to_radians(angle);
        d.roundness = roundness;
        d.flip_x = fx;
        d.flip_y = fy;
        d.opacity = pc_clampf(opacity, 0.0f, 1.0f);
        d.depth = depth;
        d.index = i;
        d.proj_angle = proj_angle;
        d.proj_scale = proj_scale;
        d.wet = wet;
        d.mix = mix;
        TRY(pc_paint_dab_vec_push(out, &d, err));
    }
    return PC_OK;
}

double pc_paint_dual_builder_step_len(const pc_paint_brush *brush, const pc_paint_point *p)
{
    const pc_paint_dual_brush *d = &brush->dual_brush;
    (void)p;
    return pc_paint_max_k((double)(pc_paint_maxf_k(d->size, 1.0f) * pc_paint_maxf_k(d->spacing, 0.01f)), 0.5);
}

pc_status pc_paint_dual_builder_build(pc_paint_dual_builder *bld, const pc_paint_brush *brush,
                                      const pc_paint_step *s, pc_paint_dab_vec *out, pc_error *err)
{
    const pc_paint_dual_brush *d = &brush->dual_brush;
    uint64_t seed = brush->seed ^ 0xD0A1u;
    uint32_t count = d->count < 1 ? 1 : (d->count > 16 ? 16 : d->count);
    for (uint32_t ci = 0; ci < count; ci++) {
        uint64_t i = bld->next_index++;
        float radius = pc_paint_maxf_k(d->size / 2.0f, 0.5f);
        double amt = (double)(d->scatter * radius);
        double ox = (double)pc_paint_rand_signed(seed, i, PC_PAINT_STREAM_DUAL_X) * amt;
        double oy = (double)pc_paint_rand_signed(seed, i, PC_PAINT_STREAM_DUAL_Y) * amt;
        pc_point c = pc_point_make(s->point.x, s->point.y);
        pc_paint_dab dab;
        if (d->both_axes) {
            c.x += ox;
            c.y += oy;
        } else {
            double dx = (double)cosf(s->direction), dy = -(double)sinf(s->direction);
            c.x += -dy * ox;
            c.y += dx * ox;
        }
        dab = pc_paint_dab_round(c, radius, 1.0f);
        dab.angle = to_radians(d->angle);
        dab.roundness = pc_clampf(d->roundness, 0.01f, 1.0f);
        dab.flip_x = d->flip && pc_paint_rand01(seed, i, PC_PAINT_STREAM_DUAL_FLIP) < 0.5f;
        dab.index = i;
        TRY(pc_paint_dab_vec_push(out, &dab, err));
    }
    return PC_OK;
}

/* ---- generator ---- */

void pc_paint_dab_generator_init(pc_paint_dab_generator *g, const pc_paint_brush *brush, float zoom)
{
    bool has_interval;
    double interval = 0.0;
    memset(g, 0, sizeof *g);
    pc_paint_brush_bounded(brush, &g->brush);
    has_interval = g->brush.build_up;
    if (has_interval) {
        interval = 1000.0 / (double)pc_clampf(g->brush.build_up_rate, 0.1f, 1000.0f);
    }
    pc_paint_smoother_init(&g->smoother, &g->brush.smoothing, zoom);
    pc_paint_walker_init(&g->walker, has_interval, interval);
    if (!g->brush.spacing_enabled) {
        pc_paint_walker_speed_spacing(&g->walker, PC_PAINT_SPEED_SPACING_MS);
    }
    /* A small aliased tip whose dabs are at most a pixel apart steps pixel by pixel. */
    if (g->brush.aliased && g->brush.size * pc_paint_maxf_k(g->brush.spacing, 0.01f) <= 1.0f) {
        float cx, cy;
        pc_paint_grid_center(0.0, 0.0, g->brush.size, &cx, &cy);
        pc_paint_walker_pixel_grid(&g->walker, (double)cx);
    }
    g->dual_on = g->brush.dual_brush.enabled;
    pc_paint_walker_init(&g->dual_walker, false, 0.0);
    pc_paint_dab_builder_init(&g->builder, &g->brush);
}

void pc_paint_dab_generator_fini(pc_paint_dab_generator *g)
{
    if (g == NULL) {
        return;
    }
    pc_paint_brush_fini(&g->brush);
    pc_paint_point_vec_fini(&g->scratch);
}

void pc_paint_dab_generator_copy(pc_paint_dab_generator *dst, const pc_paint_dab_generator *src)
{
    *dst = *src;
    pc_paint_brush_copy(&dst->brush, &src->brush);
    memset(&dst->scratch, 0, sizeof dst->scratch); /* scratch only, never shared */
}

typedef struct walkctx {
    pc_paint_dab_generator *g;
    pc_paint_dab_vec *out;
} walkctx;

static double primary_len(void *ctx, const pc_paint_point *p)
{
    return pc_paint_dab_builder_step_len(&((walkctx *)ctx)->g->brush, p);
}

static double dual_len(void *ctx, const pc_paint_point *p)
{
    return pc_paint_dual_builder_step_len(&((walkctx *)ctx)->g->brush, p);
}

static pc_status primary_step(void *ctx, const pc_paint_step *s, pc_error *err)
{
    walkctx *w = ctx;
    return pc_paint_dab_builder_build(&w->g->builder, &w->g->brush, s, w->out, err);
}

static pc_status dual_step(void *ctx, const pc_paint_step *s, pc_error *err)
{
    walkctx *w = ctx;
    return pc_paint_dual_builder_build(&w->g->dual, &w->g->brush, s, w->out, err);
}

static pc_status walk(pc_paint_dab_generator *g, const pc_paint_point *pts, size_t n, pc_paint_dab_vec *dabs,
                      pc_paint_dab_vec *dual, pc_error *err)
{
    walkctx pw = {g, dabs}, dw = {g, dual};
    for (size_t i = 0; i < n; i++) {
        pc_paint_point p = pc_paint_apply_pose(&g->brush, pts[i]);
        TRY(pc_paint_walker_push(&g->walker, p, primary_len, &pw, primary_step, &pw, err));
        if (g->dual_on) {
            TRY(pc_paint_walker_push(&g->dual_walker, p, dual_len, &dw, dual_step, &dw, err));
        }
    }
    return PC_OK;
}

bool pc_paint_dab_generator_wants_time(const pc_paint_dab_generator *g)
{
    return g->walker.has_interval || pc_paint_smoother_lagging(&g->smoother);
}

bool pc_paint_dab_generator_smoothing_string(const pc_paint_dab_generator *g, double brush[2],
                                             double pointer[2])
{
    return pc_paint_smoother_string(&g->smoother, brush, pointer);
}

pc_status pc_paint_dab_generator_push(pc_paint_dab_generator *g, const pc_paint_point *pts, size_t n,
                                      pc_paint_dab_vec *dabs, pc_paint_dab_vec *dual, pc_error *err)
{
    g->scratch.len = 0;
    for (size_t i = 0; i < n; i++) {
        TRY(pc_paint_smoother_push(&g->smoother, pts[i], &g->scratch, err));
    }
    return walk(g, g->scratch.v, g->scratch.len, dabs, dual, err);
}

pc_status pc_paint_dab_generator_finish(pc_paint_dab_generator *g, pc_paint_dab_vec *dabs,
                                        pc_paint_dab_vec *dual, pc_error *err)
{
    walkctx pw = {g, dabs}, dw = {g, dual};
    g->scratch.len = 0;
    TRY(pc_paint_smoother_finish(&g->smoother, &g->scratch, err));
    TRY(walk(g, g->scratch.v, g->scratch.len, dabs, dual, err));
    TRY(pc_paint_walker_finish(&g->walker, primary_step, &pw, err));
    if (g->dual_on) {
        TRY(pc_paint_walker_finish(&g->dual_walker, dual_step, &dw, err));
    }
    return PC_OK;
}

pc_status pc_paint_dabs(const pc_paint_brush *brush, const pc_paint_point *pts, size_t n,
                        pc_paint_dab_vec *out, pc_error *err)
{
    pc_paint_dab_generator g;
    pc_paint_dab_vec dual = {0};
    pc_status st;
    pc_paint_dab_generator_init(&g, brush, 1.0f);
    st = pc_paint_dab_generator_push(&g, pts, n, out, &dual, err);
    if (st == PC_OK) {
        st = pc_paint_dab_generator_finish(&g, out, &dual, err);
    }
    pc_paint_dab_vec_fini(&dual);
    pc_paint_dab_generator_fini(&g);
    return st;
}
