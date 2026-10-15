/* Pattern Stamp (port of crates/engine/src/retouch_cmds/pattern_stamp.rs). Included into
 * retouch_cmds.c. Paint with the current pattern through a brush stroke: aligned strokes lock the
 * tile origin in document space, unaligned restart at the first point, Impressionist jitters each
 * dab's phase with a seeded RNG (a replay of the same points is identical). */

/* A single stroke's accumulated coverage stays at or under 8192^2 px; larger returns an error. */
#define PAT_MAX_STROKE_PIXELS ((uint64_t)67108864)
#define PAT_STREAM_PHASE_X ((uint64_t)101)
#define PAT_STREAM_PHASE_Y ((uint64_t)102)

static pc_status pattern_stamp_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_paintable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pattern pat = {0};
    bool found = false;
    if (pc_engine_pattern_resolve(s, "", 0, &pat, &found, NULL) != PC_OK || !found) {
        return pc_fail(reason, PC_ERR_INVALID, "no pattern selected");
    }
    pc_pattern_fini(&pat);
    return PC_OK;
}

static uint64_t pat_seed_from_points(yyjson_val *p)
{
    yyjson_val *pts = pc_json_get(p, "points");
    if (pts == NULL) {
        return pc_paint_seed_from_bytes((const uint8_t *)"", 0);
    }
    pc_buf buf = PC_BUF_INIT;
    if (pc_json_write_val(pts, PC_JSON_SORT_KEYS, &buf, NULL) != PC_OK) {
        pc_buf_fini(&buf);
        return pc_paint_seed_from_bytes((const uint8_t *)"", 0);
    }
    uint64_t seed = pc_paint_seed_from_bytes(buf.data, buf.len);
    pc_buf_fini(&buf);
    return seed;
}

static pc_status stamp_scale(yyjson_val *p, const char *cmd, float *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "scale");
    if (v == NULL) {
        *out = 1.0f;
        return PC_OK;
    }
    double s;
    if (!pc_json_as_f64(v, &s)) {
        return pc_engine_err_bad_params(err, cmd, "`scale` must be a number (1..1000 %)");
    }
    if (!isfinite(s) || s < 1.0 || s > 1000.0) {
        return pc_engine_err_bad_params(err, cmd, "scale must be 1..1000 %");
    }
    *out = (float)(s / 100.0);
    return PC_OK;
}

static pc_status stamp_angle(yyjson_val *p, const char *cmd, float *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "angle");
    if (v == NULL) {
        *out = 0.0f;
        return PC_OK;
    }
    double a;
    if (!pc_json_as_f64(v, &a)) {
        return pc_engine_err_bad_params(err, cmd, "`angle` must be a number (degrees)");
    }
    if (!isfinite(a)) {
        return pc_engine_err_bad_params(err, cmd, "`angle` must be finite");
    }
    *out = (float)a;
    return PC_OK;
}

static pc_status stamp_phase(yyjson_val *p, const char *cmd, bool aligned, const pc_paint_point *first,
                            float out[2], pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "phase");
    if (v == NULL) {
        out[0] = aligned ? 0.0f : (float)first->x;
        out[1] = aligned ? 0.0f : (float)first->y;
        return PC_OK;
    }
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) < 2) {
        return pc_engine_err_bad_params(err, cmd, "`phase` must be [px, py]");
    }
    double x, y;
    if (!pc_json_as_f64(yyjson_arr_get(v, 0), &x) || !pc_json_as_f64(yyjson_arr_get(v, 1), &y)) {
        return pc_engine_err_bad_params(err, cmd, "`phase` must be [px, py]");
    }
    if (!isfinite(x) || !isfinite(y)) {
        return pc_engine_err_bad_params(err, cmd, "`phase` coordinates must be finite");
    }
    out[0] = (float)x;
    out[1] = (float)y;
    return PC_OK;
}

static pc_status reject_huge_stroke(const char *cmd, const pc_paint_stroke *stroke, pc_error *err)
{
    double pad = fma((double)(stroke->brush.size > 1.0f ? stroke->brush.size : 1.0f), 0.5, 4.0);
    double min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (size_t i = 0; i < stroke->count; i++) {
        double x = stroke->points[i].x, y = stroke->points[i].y;
        min_x = fmin(min_x, x);
        min_y = fmin(min_y, y);
        max_x = fmax(max_x, x);
        max_y = fmax(max_y, y);
    }
    if (!isfinite(min_x) || !isfinite(min_y) || !isfinite(max_x) || !isfinite(max_y)) {
        return pc_engine_err_bad_params(err, cmd, "point coordinates must be finite");
    }
    double w = fmax(ceil(max_x - min_x + pad * 2.0), 1.0);
    double h = fmax(ceil(max_y - min_y + pad * 2.0), 1.0);
    if (!isfinite(w) || !isfinite(h) || w > 1.0e9 || h > 1.0e9) {
        return pc_engine_err_bad_params(err, cmd, "stroke too large (at most %llu px)",
                                        (unsigned long long)PAT_MAX_STROKE_PIXELS);
    }
    uint64_t area = (uint64_t)w * (uint64_t)h;
    if (area > PAT_MAX_STROKE_PIXELS) {
        return pc_engine_err_bad_params(err, cmd, "stroke too large (%.0fx%.0f px, at most %llu)", w, h,
                                        (unsigned long long)PAT_MAX_STROKE_PIXELS);
    }
    return PC_OK;
}

static pc_status reject_bounds(const char *cmd, pc_rect bounds, pc_error *err)
{
    if (pc_rect_is_empty(bounds)) {
        return PC_OK;
    }
    uint64_t area = (uint64_t)pc_rect_width(bounds) * (uint64_t)pc_rect_height(bounds);
    if (area > PAT_MAX_STROKE_PIXELS) {
        return pc_engine_err_bad_params(err, cmd, "stroke too large (%ux%u px, at most %llu)",
                                        pc_rect_width(bounds), pc_rect_height(bounds),
                                        (unsigned long long)PAT_MAX_STROKE_PIXELS);
    }
    return PC_OK;
}

static pc_compose_placement pat_placement(const float phase[2], float scale, float angle)
{
    return pc_compose_placement_new(PC_RECT_EMPTY, false, phase, scale, angle);
}

/* The rendered straight-RGBA buffer `buf` over `rect` as a native-channel region. */
static pc_status rgba_to_region(pc_pixel_format fmt, pc_rect rect, const pc_compose_buffer *buf,
                               pc_paint_region *out, pc_error *err)
{
    size_t n = pc_pixel_format_channels(fmt);
    pc_status rc = pc_paint_region_new(rect, n, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t count = (n > 0) ? out->len / n : 0;
    if (count > buf->len) {
        count = buf->len;
    }
    for (size_t i = 0; i < count; i++) {
        pc_from_rgba(fmt, buf->px[i], enc);
        memcpy(&out->data[i * n], enc, n * sizeof(float));
    }
    return PC_OK;
}

static pc_status pat_paint_aligned(pc_surface *surf, const pc_paint_stroke *stroke, const pc_surface *sel,
                                  bool lock, const pc_compose_pattern_tile *tile, const float phase[2],
                                  float scale, float angle, pc_blend_mode mode, const char *cmd,
                                  pc_rect *dmg, pc_error *err)
{
    pc_rect bounds = PC_RECT_EMPTY;
    float *cov = NULL;
    size_t ncov = 0;
    pc_status rc = pc_paint_stroke_coverage(stroke, &bounds, &cov, &ncov, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = reject_bounds(cmd, bounds, err);
    if (rc != PC_OK || pc_rect_is_empty(bounds)) {
        free(cov);
        *dmg = PC_RECT_EMPTY;
        return rc;
    }
    pc_compose_placement place = pat_placement(phase, scale, angle);
    pc_compose_buffer buf = {0};
    rc = pc_compose_pattern_render(tile, &place, bounds, &buf, err);
    pc_paint_region paint = {0};
    if (rc == PC_OK) {
        rc = rgba_to_region(pc_surface_format(surf), bounds, &buf, &paint, err);
    }
    if (rc == PC_OK) {
        rc = pc_paint_apply_coverage(surf, bounds, cov, ncov, stroke->brush.opacity, sel, lock, &paint, mode,
                                     dmg, err);
    }
    pc_compose_buffer_fini(&buf);
    pc_paint_region_fini(&paint);
    free(cov);
    return rc;
}

struct stamp_dab_ctx {
    pc_pixel_format fmt;
    const pc_compose_pattern_tile *tile;
    float phase[2];
    float scale;
    float angle;
    uint64_t seed;
    float tw, th;
    pc_blend_mode mode;
};

static pc_status stamp_dab_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp,
                                 pc_error *err)
{
    struct stamp_dab_ctx *c = ctx;
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    uint64_t i = (uint64_t)fp->index;
    float jx = pc_paint_rand_signed(c->seed, i, PAT_STREAM_PHASE_X) * c->tw;
    float jy = pc_paint_rand_signed(c->seed, i, PAT_STREAM_PHASE_Y) * c->th;
    float ph[2] = {c->phase[0] + jx, c->phase[1] + jy};
    pc_compose_placement place = pat_placement(ph, c->scale, c->angle);
    pc_compose_buffer buf = {0};
    pc_status rc = pc_compose_pattern_render(c->tile, &place, r, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t rw = (size_t)pc_rect_width(r), n = work->ch;
    float enc[PC_RASTER_MAX_CHANNELS];
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float k = pc_paint_footprint_at(fp, x, y);
            if (k <= 0.0f) {
                continue;
            }
            size_t bi = (size_t)(y - r.y0) * rw + (size_t)(x - r.x0);
            if (bi >= buf.len) {
                continue;
            }
            float *dst = pc_paint_region_px(work, x, y);
            if (dst == NULL) {
                continue;
            }
            pc_from_rgba(c->fmt, buf.px[bi], enc);
            if (c->mode == PC_BLEND_NORMAL) {
                pc_paint_over_native(c->fmt, dst, enc, k, false);
            } else {
                float d[4];
                pc_to_rgba(c->fmt, dst, n, d);
                float o[4];
                pc_blend_composite(c->mode, d, buf.px[bi], k, o);
                pc_from_rgba(c->fmt, o, enc);
                memcpy(dst, enc, n * sizeof(float));
            }
        }
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

struct pat_work {
    const pc_paint_stroke *stroke;
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    const pc_pattern *pat;
    pc_compose_pattern_tile *tile;
    float phase[2];
    float scale;
    float angle;
    bool impressionist;
    pc_blend_mode mode;
    uint64_t seed;
    const char *cmd;
    pc_rect damage;
};

static pc_status pat_edit_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct pat_work *c = vctx;
    pc_status rc = pc_engine_pattern_ensure_in_doc(doc, c->pat, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *surf = NULL;
    bool lock = false;
    rc = pc_engine_target_surface(doc, c->has_layer, c->layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_rect dmg = PC_RECT_EMPTY;
    if (c->impressionist) {
        struct stamp_dab_ctx sc = {pc_surface_format(surf),
                                   c->tile,
                                   {c->phase[0], c->phase[1]},
                                   c->scale,
                                   c->angle,
                                   c->seed,
                                   (float)c->pat->width * c->scale,
                                   (float)c->pat->height * c->scale,
                                   c->mode};
        rc = pc_paint_apply_dab_stroke(surf, c->stroke, sel, lock, 0, stamp_dab_effect, &sc, &dmg, err);
    } else {
        rc = pat_paint_aligned(surf, c->stroke, sel, lock, c->tile, c->phase, c->scale, c->angle, c->mode,
                              c->cmd, &dmg, err);
    }
    if (rc == PC_OK) {
        c->damage = dmg;
    }
    return rc;
}

static pc_status pattern_stamp_run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "paint.patternStamp";
    pc_paint_stroke stroke = {0};
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = parse_brush(s, p, CMD, &stroke, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (stroke.count == 0 || stroke.points == NULL) {
        pc_paint_stroke_fini(&stroke);
        return pc_engine_err_bad_params(err, CMD, "`points` is empty");
    }
    pc_blend_mode mode;
    pc_pattern pat = {0};
    bool aligned = flag_p(p, "aligned", true);
    bool impressionist = flag_p(p, "impressionist", false);
    float scale = 1.0f, angle = 0.0f, phase[2] = {0, 0};
    pc_compose_pattern_tile *tile = NULL;
    if ((rc = blend_param(p, CMD, &mode, err)) != PC_OK ||
        (rc = pc_engine_pattern_resolve_param(s, CMD, p, &pat, err)) != PC_OK) {
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    if (pc_pattern_is_empty(&pat)) {
        pc_pattern_fini(&pat);
        pc_paint_stroke_fini(&stroke);
        return pc_engine_err_bad_params(err, CMD, "the pattern is empty");
    }
    if ((rc = stamp_scale(p, CMD, &scale, err)) != PC_OK ||
        (rc = stamp_angle(p, CMD, &angle, err)) != PC_OK ||
        (rc = stamp_phase(p, CMD, aligned, &stroke.points[0], phase, err)) != PC_OK) {
        pc_pattern_fini(&pat);
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    rc = pc_compose_pattern_tile_new(&pat, &tile, err);
    if (rc != PC_OK || tile == NULL) {
        pc_pattern_fini(&pat);
        pc_paint_stroke_fini(&stroke);
        if (rc == PC_OK) {
            return pc_engine_err_bad_params(err, CMD, "the pattern is empty or too large to paint");
        }
        return rc;
    }
    rc = reject_huge_stroke(CMD, &stroke, err);
    if (rc != PC_OK) {
        pc_compose_pattern_tile_release(tile);
        pc_pattern_fini(&pat);
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    uint64_t seed;
    yyjson_val *sv = pc_json_get(p, "seed");
    seed = (sv != NULL && pc_json_as_u64(sv, &seed)) ? seed : pat_seed_from_points(p);
    stroke.brush.seed = seed;

    struct pat_work pw = {&stroke, has_layer, layer, p,     &pat,   tile,     {phase[0], phase[1]},
                          scale,    angle,     impressionist, mode,  seed, CMD, PC_RECT_EMPTY};
    rc = pc_engine_session_edit(s, "Pattern Stamp", pat_edit_cb, &pw, err);
    pc_compose_pattern_tile_release(tile);
    if (rc != PC_OK) {
        pc_pattern_fini(&pat);
        pc_paint_stroke_fini(&stroke);
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = pw.damage;
        st->has_last_damage = true;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_pattern_fini(&pat);
        pc_paint_stroke_fini(&stroke);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *da = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, da, pw.damage.x0);
    yyjson_mut_arr_add_int(d, da, pw.damage.y0);
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_width(pw.damage));
    yyjson_mut_arr_add_int(d, da, (int64_t)pc_rect_height(pw.damage));
    yyjson_mut_obj_add_val(d, root, "damage", da);
    yyjson_mut_val *pa = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, pa, phase[0]);
    yyjson_mut_arr_add_real(d, pa, phase[1]);
    yyjson_mut_obj_add_val(d, root, "phase", pa);
    yyjson_mut_obj_add_bool(d, root, "aligned", aligned);
    yyjson_mut_obj_add_strcpy(d, root, "pattern", pc_str_c(&pat.id));
    yyjson_mut_doc_set_root(d, root);
    pc_pattern_fini(&pat);
    pc_paint_stroke_fini(&stroke);
    return PC_OK;
}
