/* Mixer Brush (mixer.h). */
#include "paint/mixer.h"

#include <math.h>
#include <string.h>

#include "base/num.h"
#include "paint/pmath.h"
#include "paint/retouch.h"
#include "raster/pixel.h"

#define MAXC PC_RASTER_MAX_CHANNELS

void pc_paint_mixer_state_default(pc_paint_mixer_state *s)
{
    memset(s, 0, sizeof *s);
}

bool pc_paint_mixer_state_eq(const pc_paint_mixer_state *a, const pc_paint_mixer_state *b)
{
    if (a->has_reservoir != b->has_reservoir || a->has_pickup != b->has_pickup || !(a->amount == b->amount)) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if ((a->has_reservoir && !(a->reservoir[i] == b->reservoir[i])) ||
            (a->has_pickup && !(a->pickup[i] == b->pickup[i]))) {
            return false;
        }
    }
    return true;
}

void pc_paint_mixer_state_load(pc_paint_mixer_state *s, const float color[4])
{
    s->has_reservoir = true;
    memcpy(s->reservoir, color, sizeof s->reservoir);
    s->amount = 1.0f;
}

void pc_paint_mixer_state_clean(pc_paint_mixer_state *s)
{
    s->has_pickup = false;
    memset(s->pickup, 0, sizeof s->pickup);
}

typedef struct mix_ctx {
    pc_pixel_format fmt;
    size_t n, nc, a;
    bool has_a;
    const pc_surface *sample;
    pc_pixel_format sfmt;
    bool has_res;
    float res[MAXC];
    float amount;
    bool has_pickup;
    float pickup[MAXC];
    float wet, mix, flow, load;
    bool lock;
} mix_ctx;

/* A straight RGBA colour as an opaque native pixel. */
static void native(const mix_ctx *c, const float rgba[4], float out[MAXC])
{
    (void)pc_from_rgba(c->fmt, rgba, out);
    if (c->has_a) {
        out[c->a] = 1.0f;
    }
}

static pc_status mixer_dab(void *vctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    mix_ctx *c = vctx;
    /* Transfer > Wetness / Mix Jitter scale the settings per dab. */
    float wet = c->wet * pc_clampf(fp->dab.wet, 0.0f, 1.0f),
          mix = c->mix * pc_clampf(fp->dab.mix, 0.0f, 1.0f);
    float acc[MAXC] = {0}, paint[MAXC] = {0}, wsum = 0.0f, asum = 0.0f, wr, wp, total;
    size_t n = c->n, nc = c->nc;
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    (void)err;
    /* Sample the canvas under the dab. */
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float w = pc_paint_footprint_at(fp, x, y), px[MAXC] = {0}, al;
            if (w <= 0.0f) {
                continue;
            }
            if (c->sample != NULL) {
                if (pc_pixel_format_eq(c->sfmt, c->fmt)) {
                    pc_surface_read_pixel(c->sample, x, y, px);
                } else {
                    float rgba[4];
                    pc_surface_rgba(c->sample, x, y, rgba);
                    (void)pc_from_rgba(c->fmt, rgba, px);
                }
            } else {
                const float *p = pc_paint_region_cpx(work, x, y);
                if (p == NULL) {
                    continue;
                }
                memcpy(px, p, n * sizeof *px);
            }
            al = c->has_a ? px[c->a] : 1.0f;
            for (size_t k = 0; k < nc; k++) {
                acc[k] += px[k] * w * al;
            }
            wsum += w;
            asum += w * al;
        }
    }
    if (asum > 1e-6f && wet > 0.0f) {
        float ca = asum / pc_paint_maxf_k(wsum, 1e-6f);
        for (size_t k = 0; k < n; k++) {
            float cv = k < nc ? acc[k] / asum : 1.0f;
            c->pickup[k] =
                c->has_pickup ? c->pickup[k] + (cv - c->pickup[k]) * PC_PAINT_PICKUP_RATE * wet * ca : cv;
        }
        c->has_pickup = true;
    }
    wr = c->has_res ? c->amount * (1.0f - mix * wet) : 0.0f;
    wp = c->has_pickup ? mix * wet : 0.0f;
    total = wr + wp;
    if (total > 1e-6f) {
        float dep;
        for (size_t k = 0; k < n; k++) {
            float rv = c->has_res ? c->res[k] : 0.0f, pv = c->has_pickup ? c->pickup[k] : 0.0f;
            paint[k] = (rv * wr + pv * wp) / total;
        }
        dep = pc_paint_minf_k(total, 1.0f) * c->flow;
        for (int32_t y = r.y0; y < r.y1; y++) {
            for (int32_t x = r.x0; x < r.x1; x++) {
                float k = pc_paint_footprint_at(fp, x, y) * dep;
                float *px;
                if (k > 0.0f && (px = pc_paint_region_px(work, x, y)) != NULL) {
                    pc_paint_over_native(c->fmt, px, paint, k, c->lock);
                }
            }
        }
    }
    c->amount *= 1.0f - 0.08f * (1.0f - c->load);
    return PC_OK;
}

pc_status pc_paint_apply_mixer_stroke(pc_surface *target, const pc_surface *sample,
                                      const pc_paint_stroke *stroke, const pc_paint_mixer_settings *m,
                                      pc_paint_mixer_state *state, const pc_surface *selection,
                                      bool lock_transparency, pc_rect *damage, pc_error *err)
{
    mix_ctx c;
    pc_paint_stroke st = *stroke; /* shallow: only read, never finalised */
    pc_status s;
    memset(&c, 0, sizeof c);
    c.fmt = pc_surface_format(target);
    c.n = pc_pixel_format_channels(c.fmt);
    c.nc = pc_color_mode_channels(c.fmt.mode);
    c.has_a = pc_paint_alpha_index(c.fmt, &c.a);
    c.sample = sample;
    if (sample != NULL) {
        c.sfmt = pc_surface_format(sample);
    }
    if (c.n > MAXC || c.nc > c.n) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported pixel format for the Mixer Brush");
    }
    c.has_res = state->has_reservoir;
    if (c.has_res) {
        native(&c, state->reservoir, c.res);
    }
    c.amount = c.has_res ? pc_clampf(state->amount, 0.0f, 1.0f) : 0.0f;
    c.has_pickup = state->has_pickup;
    if (c.has_pickup) {
        native(&c, state->pickup, c.pickup);
    }
    c.wet = pc_clampf(m->wet, 0.0f, 1.0f);
    c.mix = pc_clampf(m->mix, 0.0f, 1.0f);
    c.flow = pc_clampf(m->flow, 0.0f, 1.0f);
    c.load = pc_clampf(m->load, 0.0f, 1.0f);
    c.lock = lock_transparency;
    st.brush.opacity = 1.0f;
    s = pc_paint_apply_dab_stroke(target, &st, selection, lock_transparency, 0, mixer_dab, &c, damage, err);
    if (s != PC_OK) {
        return s;
    }
    state->amount = c.amount;
    state->has_pickup = c.has_pickup;
    if (c.has_pickup) {
        pc_to_rgba(c.fmt, c.pickup, c.n, state->pickup);
    } else {
        memset(state->pickup, 0, sizeof state->pickup);
    }
    return PC_OK;
}
