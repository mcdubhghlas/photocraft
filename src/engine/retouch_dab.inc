/* Sequential-dab retouching tools: Dodge, Burn, Sponge, Blur, Sharpen, Smudge (port of the
 * DabTool part of crates/engine/src/retouch_cmds.rs). Included into retouch_cmds.c; uses its
 * helpers (num_p/flag_p/string_p/parse_brush/run_stroke/composite_region). Each dab edits a
 * working copy in order (pc_paint_apply_dab_stroke); Blur/Sharpen/Smudge can sample all layers. */

enum { KIND_DODGE = 0, KIND_BURN, KIND_SPONGE, KIND_BLUR, KIND_SHARPEN, KIND_SMUDGE };

/* Per-dab strength such that one pass over a point totals roughly `e` whatever the spacing. */
static float per_dab(float e, float spacing)
{
    return 1.0f - powf(1.0f - clampf(e, 0.0f, 1.0f), clampf(spacing, 0.01f, 1.0f));
}

/* "Sample All Layers" for the sequential-dab tools (layer pixels only). */
static bool sample_all_layers(yyjson_val *p)
{
    return flag_p(p, "sampleAllLayers", false) &&
           pc_engine_target_of(p, NULL) == PC_ENGINE_PIXTGT_PIXELS;
}

/* ---- tone (Dodge / Burn): once per pixel, from the original, built up by coverage ---------- */

struct tone_ctx {
    pc_pixel_format fmt;
    size_t n;
    size_t alpha; /* SIZE_MAX for none */
    float exposure;
    pc_algo_tone_range range;
    bool burn;
    bool protect;
    float spacing; /* unused by tone (coverage builds directly) */
    /* lazily allocated dense state over the working region */
    pc_rect rect;
    bool ready;
    float *cov;  /* w*h, -1 = untouched */
    float *orig; /* w*h*n */
    bool oom;
};

static void tone_ctx_fini(struct tone_ctx *t)
{
    free(t->cov);
    free(t->orig);
    t->cov = NULL;
    t->orig = NULL;
    t->ready = false;
}

static pc_status tone_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    (void)err;
    struct tone_ctx *t = ctx;
    if (t->oom) {
        return PC_OK;
    }
    if (!t->ready || !pc_rect_eq(t->rect, work->rect)) {
        tone_ctx_fini(t);
        t->rect = work->rect;
        size_t w = (size_t)pc_rect_width(work->rect), h = (size_t)pc_rect_height(work->rect);
        size_t npx = w * h;
        t->cov = malloc((npx > 0 ? npx : 1) * sizeof(float));
        t->orig = calloc((npx * t->n > 0 ? npx * t->n : 1), sizeof(float));
        if (t->cov == NULL || t->orig == NULL) {
            tone_ctx_fini(t);
            t->oom = true;
            return PC_OK; /* best-effort (never crash); leaves pixels unchanged */
        }
        for (size_t i = 0; i < npx; i++) {
            t->cov[i] = -1.0f;
        }
        t->ready = true;
    }
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    size_t w = (size_t)pc_rect_width(work->rect);
    float enc[PC_RASTER_MAX_CHANNELS];
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float k = clampf(pc_paint_footprint_at(fp, x, y), 0.0f, 1.0f);
            if (k <= 0.0f) {
                continue;
            }
            size_t i = (size_t)(y - work->rect.y0) * w + (size_t)(x - work->rect.x0);
            float *px = pc_paint_region_px(work, x, y);
            if (px == NULL) {
                continue;
            }
            if (t->cov[i] < 0.0f) {
                memcpy(&t->orig[i * t->n], px, t->n * sizeof(float));
                t->cov[i] = 0.0f;
            }
            float knew = t->cov[i] + k * (1.0f - t->cov[i]);
            if (knew <= t->cov[i]) {
                continue;
            }
            t->cov[i] = knew;
            float o[4];
            pc_to_rgba(t->fmt, &t->orig[i * t->n], t->n, o);
            float tc[3];
            pc_algo_retouch_dodge_burn((float[3]){o[0], o[1], o[2]}, t->exposure, t->range, t->burn,
                                       t->protect, tc);
            float m[4] = {o[0] + (tc[0] - o[0]) * knew, o[1] + (tc[1] - o[1]) * knew,
                          o[2] + (tc[2] - o[2]) * knew, o[3]};
            pc_from_rgba(t->fmt, m, enc);
            for (size_t c = 0; c < t->n; c++) {
                if (c != t->alpha) {
                    px[c] = enc[c];
                }
            }
        }
    }
    return PC_OK;
}

/* ---- sponge: an RGB transform through straight RGBA at half flow per dab ------------------- */

struct sponge_ctx {
    pc_pixel_format fmt;
    size_t n;
    size_t alpha;
    float spacing;
    bool saturate;
    bool vibrance;
};

static pc_status sponge_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    (void)err;
    struct sponge_ctx *c = ctx;
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    float enc[PC_RASTER_MAX_CHANNELS];
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float k = per_dab(pc_paint_footprint_at(fp, x, y) * 0.5f, c->spacing);
            if (k <= 0.0f) {
                continue;
            }
            float *px = pc_paint_region_px(work, x, y);
            if (px == NULL) {
                continue;
            }
            float rgba[4];
            pc_to_rgba(c->fmt, px, c->n, rgba);
            float o[3];
            pc_algo_retouch_sponge((float[3]){rgba[0], rgba[1], rgba[2]}, k, c->saturate, c->vibrance, o);
            float m[4] = {o[0], o[1], o[2], rgba[3]};
            pc_from_rgba(c->fmt, m, enc);
            for (size_t cc = 0; cc < c->n; cc++) {
                if (cc != c->alpha) {
                    px[cc] = enc[cc];
                }
            }
        }
    }
    return PC_OK;
}

/* ---- focus (Blur / Sharpen): read a neighbourhood, mix by per-dab strength ----------------- */

struct focus_ctx {
    size_t alpha; /* SIZE_MAX for none */
    float spacing;
    bool sharpen;
    float strength;
    bool protect;
    float sigma;
};

static pc_status focus_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    struct focus_ctx *c = ctx;
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    size_t w = (size_t)pc_rect_width(work->rect), h = (size_t)pc_rect_height(work->rect), n = work->ch;
    size_t win[4] = {(size_t)(r.x0 - work->rect.x0), (size_t)(r.y0 - work->rect.y0),
                     (size_t)(r.x1 - work->rect.x0), (size_t)(r.y1 - work->rect.y0)};
    float *outp = NULL;
    pc_status rc = c->sharpen ? pc_algo_retouch_local_sharpen(work->data, work->len, w, h, n, c->alpha, win,
                                                              1.0f, c->protect, &outp, err)
                              : pc_algo_retouch_local_blur(work->data, work->len, w, h, n, c->alpha, win,
                                                           c->sigma, &outp, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t rw = (size_t)pc_rect_width(r);
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float k = per_dab(pc_paint_footprint_at(fp, x, y) * c->strength, c->spacing);
            if (k <= 0.0f) {
                continue;
            }
            size_t o = ((size_t)(y - r.y0) * rw + (size_t)(x - r.x0)) * n;
            float *px = pc_paint_region_px(work, x, y);
            if (px == NULL) {
                continue;
            }
            for (size_t cc = 0; cc < n; cc++) {
                px[cc] += (outp[o + cc] - px[cc]) * k;
            }
        }
    }
    free(outp);
    return PC_OK;
}

/* ---- Sample All Layers: run the effect on the visible composite, copy covered pixels back ---- */

struct all_layers {
    pc_paint_dab_effect effect;
    void *ctx;
    const pc_document *pre;
    pc_pixel_format fmt;
    bool ready;
    pc_paint_region visible;
};

static pc_status all_layers_effect(void *vctx, pc_paint_region *work, const pc_paint_footprint *fp,
                                  pc_error *err)
{
    struct all_layers *a = vctx;
    if (!a->ready) {
        if (composite_region(a->pre, false, 0, SAMPLE_ALL, work->rect, a->fmt, &a->visible, err) != PC_OK) {
            /* best-effort on OOM: fall back to the plain effect on the working copy */
            return a->effect(a->ctx, work, fp, err);
        }
        a->ready = true;
    }
    if (!pc_rect_eq(a->visible.rect, work->rect) || a->visible.ch != work->ch) {
        return a->effect(a->ctx, work, fp, err);
    }
    pc_status rc = a->effect(a->ctx, &a->visible, fp, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            if (pc_paint_footprint_at(fp, x, y) > 0.0f) {
                float *dst = pc_paint_region_px(work, x, y);
                const float *src = pc_paint_region_cpx(&a->visible, x, y);
                if (dst != NULL && src != NULL) {
                    memcpy(dst, src, work->ch * sizeof(float));
                }
            }
        }
    }
    return PC_OK;
}

/* ---- the shared dab-command runner --------------------------------------------------------- */

struct dab_work {
    int kind;
    const pc_paint_stroke *stroke;
    pc_algo_tone_range range;
    float exposure;
    bool protect_tones;
    bool saturate;
    bool vibrance;
    bool protect_detail;
    float strength;
    float sigma;
    bool finger;
    float max_size;
    float foreground[4];
    int32_t halo;
    bool all;
};

static pc_status dab_work_fn(void *tool, const pc_document *pre, pc_surface *surf, const pc_surface *sel,
                            bool lock, pc_rect *dmg, pc_error *err)
{
    struct dab_work *d = tool;
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_pixel_format_channels(fmt);
    size_t alpha = SIZE_MAX;
    (void)pc_paint_alpha_index(fmt, &alpha);
    float spacing = d->stroke->brush.spacing;

    struct tone_ctx tc = {0};
    struct sponge_ctx sc = {0};
    struct focus_ctx fc = {0};
    pc_paint_smudge sm = {0};
    pc_paint_dab_effect effect = NULL;
    void *ectx = NULL;
    pc_status rc = PC_OK;
    switch (d->kind) {
    case KIND_DODGE:
    case KIND_BURN:
        tc.fmt = fmt;
        tc.n = n;
        tc.alpha = alpha;
        tc.exposure = d->exposure;
        tc.range = d->range;
        tc.burn = d->kind == KIND_BURN;
        tc.protect = d->protect_tones;
        effect = tone_effect;
        ectx = &tc;
        break;
    case KIND_SPONGE:
        sc.fmt = fmt;
        sc.n = n;
        sc.alpha = alpha;
        sc.spacing = spacing;
        sc.saturate = d->saturate;
        sc.vibrance = d->vibrance;
        effect = sponge_effect;
        ectx = &sc;
        break;
    case KIND_BLUR:
    case KIND_SHARPEN:
        fc.alpha = alpha;
        fc.spacing = spacing;
        fc.sharpen = d->kind == KIND_SHARPEN;
        fc.strength = d->strength;
        fc.protect = d->protect_detail;
        fc.sigma = d->sigma;
        effect = focus_effect;
        ectx = &fc;
        break;
    case KIND_SMUDGE: {
        float *finger = NULL;
        float finger_buf[PC_RASTER_MAX_CHANNELS];
        size_t finger_len = 0;
        if (d->finger) {
            finger_len = pc_from_rgba(fmt, d->foreground, finger_buf);
            finger = finger_buf;
        }
        rc = pc_paint_smudge_init(&sm, d->strength, finger, finger_len, d->max_size, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_paint_smudge_with_alpha(&sm, alpha != SIZE_MAX, alpha == SIZE_MAX ? 0 : alpha);
        effect = pc_paint_smudge_effect;
        ectx = &sm;
        break;
    }
    default:
        return pc_engine_err_other(err, "not a sequential-dab tool");
    }

    struct all_layers al = {0};
    pc_paint_dab_effect run_effect = effect;
    void *run_ctx = ectx;
    if (d->all) {
        al.effect = effect;
        al.ctx = ectx;
        al.pre = pre;
        al.fmt = fmt;
        run_effect = all_layers_effect;
        run_ctx = &al;
    }
    rc = pc_paint_apply_dab_stroke(surf, d->stroke, sel, lock, d->halo, run_effect, run_ctx, dmg, err);

    if (d->kind == KIND_DODGE || d->kind == KIND_BURN) {
        tone_ctx_fini(&tc);
    } else if (d->kind == KIND_SMUDGE) {
        pc_paint_smudge_fini(&sm);
    }
    if (d->all) {
        pc_paint_region_fini(&al.visible);
    }
    return rc;
}

static pc_status tone_range_of(yyjson_val *p, const char *cmd, pc_algo_tone_range *out, pc_error *err)
{
    size_t len = 0;
    const char *s = string_p(p, "range", "midtones", &len);
    if (!pc_algo_tone_range_from_name(s, len, out)) {
        return pc_engine_err_bad_params(err, cmd, "unknown range `%.*s` (shadows|midtones|highlights)",
                                        (int)len, s);
    }
    return PC_OK;
}

static pc_status dab_cmd(pc_engine_session *s, yyjson_val *p, const char *cmd, int kind, pc_json_value *out,
                        pc_error *err)
{
    pc_paint_stroke stroke = {0};
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = parse_brush(s, p, cmd, &stroke, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct dab_work d = {0};
    d.kind = kind;
    d.stroke = &stroke;
    const char *label = NULL;
    switch (kind) {
    case KIND_DODGE:
    case KIND_BURN:
        rc = tone_range_of(p, cmd, &d.range, err);
        d.exposure = clampf(num_p(p, "exposure", 50.0f), 1.0f, 100.0f) / 100.0f;
        d.protect_tones = flag_p(p, "protectTones", true);
        label = kind == KIND_BURN ? "Burn Tool" : "Dodge Tool";
        break;
    case KIND_SPONGE: {
        size_t len = 0;
        const char *m = string_p(p, "mode", "desaturate", &len);
        if (len == 10 && memcmp(m, "desaturate", 10) == 0) {
            d.saturate = false;
        } else if (len == 8 && memcmp(m, "saturate", 8) == 0) {
            d.saturate = true;
        } else {
            rc = pc_engine_err_bad_params(err, cmd, "unknown mode `%.*s` (desaturate|saturate)", (int)len, m);
        }
        d.vibrance = flag_p(p, "vibrance", true);
        label = "Sponge Tool";
        break;
    }
    case KIND_BLUR:
    case KIND_SHARPEN:
        d.strength = clampf(num_p(p, "strength", 50.0f), 1.0f, 100.0f) / 100.0f;
        d.protect_detail = flag_p(p, "protectDetail", true);
        d.sigma = clampf(stroke.brush.size * 0.03f, 1.0f, 4.0f);
        label = kind == KIND_SHARPEN ? "Sharpen Tool" : "Blur Tool";
        break;
    case KIND_SMUDGE:
        d.strength = clampf(num_p(p, "strength", 50.0f), 1.0f, 100.0f) / 100.0f;
        d.finger = flag_p(p, "fingerPainting", false);
        d.max_size = stroke.brush.size;
        memcpy(d.foreground, s->tools.foreground, sizeof d.foreground);
        label = "Smudge Tool";
        break;
    default:
        rc = pc_engine_err_other(err, "not a sequential-dab tool");
        break;
    }
    if (rc != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    bool focus_or_smudge = kind == KIND_BLUR || kind == KIND_SHARPEN || kind == KIND_SMUDGE;
    d.all = focus_or_smudge && sample_all_layers(p);
    d.halo = (kind == KIND_BLUR || kind == KIND_SHARPEN) ? (int32_t)ceilf(d.sigma * 3.0f) + 1 : 0;

    struct retouch_edit ed = {has_layer, layer, p, &d, dab_work_fn, PC_RECT_EMPTY};
    pc_rect dmg = PC_RECT_EMPTY;
    rc = run_stroke(s, label, &ed, &dmg, err);
    pc_paint_stroke_fini(&stroke);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_only(out, dmg, err);
}

static pc_status run_dodge(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return dab_cmd(s, p, "paint.dodge", KIND_DODGE, out, err);
}
static pc_status run_burn(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return dab_cmd(s, p, "paint.burn", KIND_BURN, out, err);
}
static pc_status run_sponge(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return dab_cmd(s, p, "paint.sponge", KIND_SPONGE, out, err);
}
static pc_status run_blur(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return dab_cmd(s, p, "paint.blur", KIND_BLUR, out, err);
}
static pc_status run_sharpen(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return dab_cmd(s, p, "paint.sharpen", KIND_SHARPEN, out, err);
}
static pc_status run_smudge(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return dab_cmd(s, p, "paint.smudge", KIND_SMUDGE, out, err);
}
