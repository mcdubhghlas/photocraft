#include "gpu/fx.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <blake3.h>
#include "base/num.h"
#include "base/mem.h"
#include "compose/effects.h"
#include "compose/pattern.h"

#define RAD .017453292519943295f
static const pc_gpu_fx_input shape_input = {PC_GPU_FX_SHAPE, 0};
static const pc_gpu_fx_input no_input = {PC_GPU_FX_NONE, 0};
typedef struct builder {
    pc_gpu_fx_program p;
    pc_status status;
    pc_error *error;
} builder;
static float clamp(float x, float lo, float hi)
{
    return fminf(hi, fmaxf(lo, x));
}
void pc_gpu_fx_program_fini(pc_gpu_fx_program *p)
{
    if (p) {
        for (size_t i = 0; i < p->count; i++) {
            free(p->stages[i].lut);
        }
        *p = (pc_gpu_fx_program){0};
    }
}
static pc_gpu_fx_stage stage(pc_gpu_fx_kernel kernel, pc_gpu_fx_input a, pc_gpu_fx_input b)
{
    return (pc_gpu_fx_stage){.kernel = kernel, .a = a, .b = b, .output = -1};
}
static pc_gpu_fx_input push(builder *b, pc_gpu_fx_stage s)
{
    if (b->status != PC_OK) {
        free(s.lut);
        return no_input;
    }
    if (b->p.count == PC_GPU_FX_MAX_STAGES) {
        free(s.lut);
        b->status = pc_fail(b->error, PC_ERR_LIMIT, "GPU effect stage limit");
        return no_input;
    }
    b->p.stages[b->p.count] = s;
    return (pc_gpu_fx_input){PC_GPU_FX_VALUE, b->p.count++};
}
static pc_gpu_fx_input field(builder *b, pc_compose_fx_field_kind kind, float width)
{
    int32_t reach = pc_f32_to_i32(ceilf(clamp(width, 0, PC_COMPOSE_FX_MAX_REACH))) + 2;
    b->p.fields[kind] = true;
    if (reach > b->p.reach[kind]) {
        b->p.reach[kind] = reach;
    }
    return (pc_gpu_fx_input){PC_GPU_FX_FIELD, (size_t)kind};
}
static pc_gpu_fx_input shift(builder *b, pc_gpu_fx_input src, float dx, float dy, float outside, bool invert)
{
    if (dx == 0 && dy == 0 && !invert) {
        return src;
    }
    pc_gpu_fx_stage s = stage(PC_GPU_FX_SHIFT, src, no_input);
    s.params[0][0] = dx;
    s.params[0][1] = dy;
    s.params[0][2] = outside;
    s.params[0][3] = invert ? 1 : 0;
    s.radius = pc_f32_to_i32(fmaxf(fabsf(dx), fabsf(dy)));
    return push(b, s);
}
static pc_gpu_fx_input dilate(builder *b, pc_gpu_fx_input src, pc_gpu_fx_input dist, float radius)
{
    pc_gpu_fx_stage s = stage(PC_GPU_FX_DILATE, src, dist);
    s.params[0][0] = radius;
    return push(b, s);
}
static pc_gpu_fx_input convolution(builder *b, pc_gpu_fx_input src, float width)
{
    int32_t radius;
    float *weights = NULL;
    size_t n;
    if (b->status != PC_OK) {
        return no_input;
    }
    b->status = pc_compose_fx_tent_kernel(width, &radius, &weights, &n, b->error);
    if (b->status != PC_OK) {
        return no_input;
    }
    if (radius <= 0) {
        free(weights);
        return src;
    }
    pc_gpu_fx_stage horizontal = stage(PC_GPU_FX_BLUR, src, no_input);
    horizontal.params[0][1] = (float)radius;
    horizontal.radius = radius;
    horizontal.lut = weights;
    horizontal.lut_len = n;
    pc_gpu_fx_input h = push(b, horizontal);
    if (b->status != PC_OK) {
        return no_input;
    }
    pc_gpu_fx_stage vertical = stage(PC_GPU_FX_BLUR, h, no_input);
    vertical.params[0][0] = 1;
    vertical.params[0][1] = (float)radius;
    vertical.radius = radius;
    /* Each stage owns its LUT, including when an allocation or later stage fails. */
    vertical.lut = pc_malloc_array(n, sizeof *weights);
    if (!vertical.lut) {
        b->status = pc_fail(b->error, PC_ERR_NOMEM, "GPU effect blur kernel");
        return no_input;
    }
    memcpy(vertical.lut, weights, n * sizeof *weights);
    vertical.lut_len = n;
    return push(b, vertical);
}
static pc_gpu_fx_input blur(builder *b, pc_gpu_fx_input src, float width)
{
    return isfinite(width) ? convolution(b, src, width) : src;
}
static float *contour(builder *b, const pc_contour *c)
{
    float *lut = NULL;
    if (b->status == PC_OK) {
        b->status = pc_compose_fx_contour_lut(c, &lut, b->error);
    }
    return lut;
}
static void finish(builder *b, pc_gpu_fx_input a, pc_gpu_fx_input other, float invert, float *lut,
                   bool times_shape, int output)
{
    pc_gpu_fx_stage s = stage(PC_GPU_FX_FINISH, a, other);
    s.params[0][0] = other.kind != PC_GPU_FX_NONE ? 1 : 0;
    s.params[0][1] = invert;
    s.params[0][2] = lut ? 1 : 0;
    s.params[0][3] = times_shape ? 1 : 0;
    s.lut = lut;
    s.lut_len = lut ? PC_COMPOSE_FX_LUT_SIZE : 0;
    s.shape = times_shape ? shape_input : no_input;
    s.output = output;
    (void)push(b, s);
}
static void offset(float angle, float distance, float *dx, float *dy)
{
    *dx = roundf(-cosf(angle * RAD) * distance);
    *dy = roundf(sinf(angle * RAD) * distance);
}
static float paint_kind(pc_compose_bevel_paint paint)
{
    return paint == PC_COMPOSE_BEVEL_PAINT_OUTER ? 0 : paint == PC_COMPOSE_BEVEL_PAINT_BOTH ? 1 : 2;
}
static void hash_input(blake3_hasher *hash, pc_gpu_fx_input input)
{
    blake3_hasher_update(hash, &input.kind, sizeof input.kind);
    blake3_hasher_update(hash, &input.index, sizeof input.index);
}
void pc_gpu_fx_program_key(const pc_gpu_fx_program *p, uint8_t key[32])
{
    blake3_hasher hash;
    blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, &p->maps, sizeof p->maps);
    blake3_hasher_update(&hash, p->fields, sizeof p->fields);
    blake3_hasher_update(&hash, p->reach, sizeof p->reach);
    for (size_t i = 0; i < p->count; i++) {
        const pc_gpu_fx_stage *s = &p->stages[i];
        blake3_hasher_update(&hash, &s->kernel, sizeof s->kernel);
        hash_input(&hash, s->a);
        hash_input(&hash, s->b);
        hash_input(&hash, s->shape);
        blake3_hasher_update(&hash, s->params, sizeof s->params);
        blake3_hasher_update(&hash, &s->radius, sizeof s->radius);
        blake3_hasher_update(&hash, &s->output, sizeof s->output);
        if (s->lut) {
            blake3_hasher_update(&hash, s->lut, s->lut_len * sizeof *s->lut);
        }
        if (s->pattern) {
            const pc_pattern *pattern = s->pattern;
            if (pattern->id.len) {
                blake3_hasher_update(&hash, pattern->id.ptr, pattern->id.len);
            }
            pc_pixel_format format = pc_surface_format(&pattern->surface);
            blake3_hasher_update(&hash, &format.mode, sizeof format.mode);
            blake3_hasher_update(&hash, &format.sample, sizeof format.sample);
            blake3_hasher_update(&hash, &format.alpha, sizeof format.alpha);
            float pixel[PC_RASTER_MAX_CHANNELS];
            pc_surface_default_pixel(&pattern->surface, pixel);
            blake3_hasher_update(&hash, pixel, pc_surface_channels(&pattern->surface) * sizeof *pixel);
            blake3_hasher_update(&hash, &pattern->width, sizeof pattern->width);
            blake3_hasher_update(&hash, &pattern->height, sizeof pattern->height);
            size_t cursor = 0;
            pc_tile coord;
            const pc_raster_tile *tile;
            while (pc_surface_next_tile(&pattern->surface, &cursor, &coord, &tile)) {
                blake3_hasher_update(&hash, &coord, sizeof coord);
                blake3_hasher_update(&hash, pc_raster_tile_bytes(tile), pc_raster_tile_len(tile));
            }
        }
    }
    blake3_hasher_finalize(&hash, key, 32);
}
pc_status pc_gpu_fx_program_build(const pc_effect *effect, pc_global_light light, bool vector_shape,
                                  const pc_pattern *patterns, size_t count, const double anchor[2],
                                  pc_gpu_fx_program *out, pc_error *err)
{
    if (!effect || !out || !anchor || (count && !patterns)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect program");
    }
    /* Noise activates for every strictly positive blur value. The CPU's sliding sums and
     * the shader's convolution differ at cancellation tails, so noisy maps must use the
     * CPU oracle instead of turning sub-ulp differences into visible speckles. */
    if (((effect->kind == PC_FX_DROP_SHADOW || effect->kind == PC_FX_INNER_SHADOW) &&
         effect->u.shadow.noise > 0) ||
        ((effect->kind == PC_FX_OUTER_GLOW || effect->kind == PC_FX_INNER_GLOW) &&
         effect->u.glow.noise > 0)) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "noisy effects require the CPU map renderer");
    }
    builder b = {.error = err};
    switch (effect->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW: {
        const pc_shadow *s = &effect->u.shadow;
        bool inner = effect->kind == PC_FX_INNER_SHADOW;
        float dx, dy;
        offset(s->use_global_light ? light.angle : s->angle, s->distance, &dx, &dy);
        pc_gpu_fx_input src = shift(&b, shape_input, dx, dy, inner ? 1 : 0, inner);
        float radius, width;
        pc_compose_fx_spread_split(s->size, s->spread, &radius, &width);
        pc_gpu_fx_input map = src;
        if (radius > 0) {
            pc_gpu_fx_input distance =
                field(&b, inner ? PC_COMPOSE_FIELD_CHOKE_INSIDE : PC_COMPOSE_FIELD_STROKE_OUTSIDE, radius);
            distance = shift(&b, distance, dx, dy, inner ? -.5f : 1e10f, false);
            map = dilate(&b, src, distance, radius);
        }
        map = blur(&b, map, width);
        finish(&b, map, no_input, 0, contour(&b, &s->contour), inner, 0);
        if (b.p.count) {
            b.p.stages[b.p.count - 1].params[1][0] = isfinite(s->noise) ? s->noise : 0;
        }
        b.p.maps = 1;
        break;
    }
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW: {
        const pc_glow *g = &effect->u.glow;
        bool inner = effect->kind == PC_FX_INNER_GLOW;
        pc_gpu_fx_input map;
        if (g->technique == PC_GLOW_PRECISE) {
            pc_gpu_fx_stage s = stage(
                PC_GPU_FX_GLOW,
                field(&b, inner ? PC_COMPOSE_FIELD_INSIDE : PC_COMPOSE_FIELD_OUTSIDE, g->size), no_input);
            s.params[0][0] = g->size * g->spread;
            s.params[0][1] = fmaxf(1e-3f, g->size + 1 - s.params[0][0]);
            map = push(&b, s);
        } else {
            pc_gpu_fx_input src = inner ? shift(&b, shape_input, 0, 0, 0, true) : shape_input;
            float radius, width;
            pc_compose_fx_spread_split(g->size, g->spread, &radius, &width);
            map = src;
            if (radius > 0) {
                map =
                    dilate(&b, src,
                           field(&b, inner ? PC_COMPOSE_FIELD_CHOKE_INSIDE : PC_COMPOSE_FIELD_STROKE_OUTSIDE,
                                 radius),
                           radius);
            }
            map = blur(&b, map, width);
        }
        float *lut = NULL;
        if (b.status == PC_OK) {
            b.status = pc_compose_fx_glow_lut(g, &lut, err);
        }
        finish(&b, map, no_input, inner && g->source == PC_GLOW_SOURCE_CENTER ? 2.f : 0.f, lut, inner, 0);
        if (b.p.count) {
            b.p.stages[b.p.count - 1].params[1][0] = isfinite(g->noise) ? g->noise : 0;
        }
        b.p.maps = 1;
        break;
    }
    case PC_FX_SATIN: {
        const pc_satin *s = &effect->u.satin;
        float dx, dy;
        offset(s->angle, pc_compose_fx_satin_distance(s), &dx, &dy);
        pc_gpu_fx_input a = shift(&b, shape_input, dx, dy, 0, false);
        a = convolution(&b, a, s->size);
        pc_gpu_fx_input c = shift(&b, shape_input, -dx, -dy, 0, false);
        c = convolution(&b, c, s->size);
        float *lut = NULL;
        if (b.status == PC_OK) {
            b.status = pc_compose_fx_satin_lut(&s->contour, &lut, err);
        }
        finish(&b, a, c, s->invert ? 1.f : 0.f, lut, true, 0);
        b.p.maps = 1;
        break;
    }
    case PC_FX_STROKE: {
        float in_w, out_w;
        pc_compose_fx_stroke_widths(&effect->u.stroke, &in_w, &out_w);
        const float widths[2] = {out_w, in_w};
        for (size_t i = 0; i < 2; i++) {
            if (widths[i] <= 0) {
                continue;
            }
            pc_compose_fx_field_kind kind = i              ? PC_COMPOSE_FIELD_STROKE_INSIDE
                                            : vector_shape ? PC_COMPOSE_FIELD_STROKE_OUTSIDE_VECTOR
                                                           : PC_COMPOSE_FIELD_STROKE_OUTSIDE;
            pc_gpu_fx_stage s = stage(PC_GPU_FX_STROKE, field(&b, kind, widths[i]), no_input);
            s.params[0][0] = widths[i];
            s.output = (int)i;
            (void)push(&b, s);
        }
        b.p.maps = 2;
        break;
    }
    case PC_FX_BEVEL_EMBOSS: {
        const pc_bevel *bevel = &effect->u.bevel;
        pc_compose_bevel_geom geometry = pc_compose_fx_bevel_geom(bevel);
        float size = fmaxf(1, bevel->size);
        pc_gpu_fx_input height;
        if (bevel->technique == PC_BEVEL_SMOOTH) {
            height = convolution(&b, shape_input, geometry.width);
        } else {
            pc_gpu_fx_stage s = stage(PC_GPU_FX_BEVEL_HEIGHT, field(&b, PC_COMPOSE_FIELD_BEVEL_INSIDE, size),
                                      field(&b, PC_COMPOSE_FIELD_BEVEL_OUTSIDE, size));
            s.params[0][0] = paint_kind(geometry.paint);
            s.params[0][1] = size;
            s.params[0][2] = geometry.chisel_soft > 0 ? 1 : 0;
            height = push(&b, s);
            if (geometry.chisel_soft > 0 && geometry.paint == PC_COMPOSE_BEVEL_PAINT_BOTH) {
                height = convolution(&b, height, geometry.chisel_soft);
            }
        }
        if (bevel->has_contour) {
            pc_gpu_fx_stage s = stage(PC_GPU_FX_FINISH, height, no_input);
            if (b.status == PC_OK) {
                b.status = pc_compose_fx_bevel_contour_lut(&bevel->contour, &s.lut, err);
            }
            s.lut_len = s.lut ? PC_COMPOSE_FX_LUT_SIZE : 0;
            s.params[0][2] = 1;
            height = push(&b, s);
        }
        if (bevel->has_texture) {
            const pc_bevel_texture *t = &bevel->texture;
            const pc_pattern *pattern =
                pc_pattern_find(patterns, count, t->id.ptr, t->id.len, t->name.ptr, t->name.len);
            if (pattern && pc_pattern_is_paintable(pattern)) {
                float unit = fabsf(bevel->depth) > 1e-6f ? fmaxf(1e-3f, fabsf(geometry.depth / bevel->depth))
                                                         : fmaxf(1, geometry.width);
                pc_compose_placement placement =
                    pc_compose_placement_anchored(anchor, t->link, t->phase, t->scale, 0);
                pc_gpu_fx_stage s = stage(PC_GPU_FX_BEVEL_TEXTURE, height, no_input);
                s.params[0][0] = t->depth / unit;
                s.params[0][1] = t->invert ? 1 : 0;
                s.params[2][0] = pc_f64_to_f32(placement.origin[0]);
                s.params[2][1] = pc_f64_to_f32(placement.origin[1]);
                s.params[2][2] = pc_f64_to_f32(placement.cs[0]);
                s.params[2][3] = pc_f64_to_f32(placement.cs[1]);
                s.params[3][0] = pc_f64_to_f32(placement.inv_scale);
                s.params[3][1] = (float)pattern->width;
                s.params[3][2] = (float)pattern->height;
                s.pattern = pattern;
                height = push(&b, s);
            }
        }
        if (bevel->soften >= 1) {
            height = convolution(&b, height, bevel->soften);
        }
        float angle = (bevel->use_global_light ? light.angle : bevel->angle) * RAD;
        float altitude = (bevel->use_global_light ? light.altitude : bevel->altitude) * RAD;
        for (int which = 0; which < 2; which++) {
            pc_gpu_fx_stage s = stage(PC_GPU_FX_BEVEL_SHADE, height, no_input);
            s.params[0][0] = cosf(angle) * cosf(altitude);
            s.params[0][1] = -sinf(angle) * cosf(altitude);
            s.params[0][2] = s.params[0][3] = sinf(altitude);
            s.params[1][0] = geometry.depth;
            s.params[1][1] = geometry.paint == PC_COMPOSE_BEVEL_PAINT_INNER   ? 0
                             : geometry.paint == PC_COMPOSE_BEVEL_PAINT_OUTER ? 1
                             : geometry.pillow                                ? 3
                                                                              : 2;
            s.params[1][2] = (float)which;
            s.lut = contour(&b, &bevel->gloss_contour);
            s.lut_len = s.lut ? PC_COMPOSE_FX_LUT_SIZE : 0;
            s.params[1][3] = s.lut ? 1 : 0;
            s.params[2][0] =
                geometry.paint == PC_COMPOSE_BEVEL_PAINT_OUTER && bevel->technique != PC_BEVEL_SMOOTH ? 1 : 0;
            s.shape = shape_input;
            s.radius = 1;
            s.output = which;
            (void)push(&b, s);
        }
        b.p.maps = 2;
        break;
    }
    case PC_FX_COLOR_OVERLAY:
    case PC_FX_GRADIENT_OVERLAY:
    case PC_FX_PATTERN_OVERLAY:
        break;
    default:
        b.status = pc_fail(err, PC_ERR_INVALID, "unknown GPU effect");
        break;
    }
    if (b.status != PC_OK) {
        pc_gpu_fx_program_fini(&b.p);
        return b.status;
    }
    for (size_t i = 0; i < b.p.count; i++) {
        for (size_t row = 0; row < 4; row++) {
            for (size_t ch = 0; ch < 4; ch++) {
                if (!isfinite(b.p.stages[i].params[row][ch])) {
                    pc_gpu_fx_program_fini(&b.p);
                    return pc_fail(err, PC_ERR_INVALID, "non-finite GPU effect parameter");
                }
            }
        }
    }
    pc_gpu_fx_program_key(&b.p, b.p.key);
    pc_gpu_fx_program_fini(out);
    *out = b.p;
    return PC_OK;
}
