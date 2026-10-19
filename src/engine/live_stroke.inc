/* Included after the brush setup helpers so previews and commits resolve identical brushes. */
struct pc_engine_live_stroke {
    pc_doc_arc *doc, *base_doc;
    size_t color;
    uint64_t seed;
    yyjson_doc *params;
    size_t count;
    pc_paint_point_vec points;
    pc_paint_brush resolved;
    pc_doc_id base;
    uint64_t revision;
    bool pencil;
    pc_paint_renderer *renderer, *mirrors[3];
    bool distinct[3];
    struct pc_engine_symmetry *symmetry;
    pc_paint_union_live union_live;
    size_t merged;
    pc_surface pre, selection;
    bool has_selection, lock, has_layer;
    pc_layer_id layer;
    pc_rect tail;
    pc_cmyk_space *cmyk;
};
void pc_engine_live_stroke_free(pc_engine_live_stroke *l)
{
    if (!l) {
        return;
    }
    pc_paint_brush_fini(&l->resolved);
    pc_paint_point_vec_fini(&l->points);
    pc_doc_arc_release(l->doc);
    pc_doc_arc_release(l->base_doc);
    yyjson_doc_free(l->params);
    pc_paint_renderer_free(l->renderer);
    for (size_t i = 0; i < 3; i++) {
        pc_paint_renderer_free(l->mirrors[i]);
    }
    pc_engine_symmetry_free(l->symmetry);
    pc_paint_union_live_fini(&l->union_live);
    pc_surface_fini(&l->pre);
    pc_surface_fini(&l->selection);
    pc_cmyk_space_release(l->cmyk);
    free(l);
}
pc_doc_arc *pc_engine_live_stroke_document(const pc_engine_live_stroke *l)
{
    return l ? l->doc : NULL;
}
uint64_t pc_engine_live_stroke_seed(const pc_engine_live_stroke *l)
{
    return l ? l->seed : 0;
}
pc_rect pc_engine_live_stroke_bounds(const pc_engine_live_stroke *l)
{
    if (!l) {
        return PC_RECT_EMPTY;
    }
    pc_rect r = pc_rect_union(pc_paint_renderer_bounds(l->renderer), l->tail);
    for (size_t i = 0; i < 3; i++) {
        if (l->distinct[i]) {
            r = pc_rect_union(r, pc_paint_renderer_bounds(l->mirrors[i]));
        }
    }
    return r;
}
bool pc_engine_live_stroke_wants_time(const pc_engine_live_stroke *l)
{
    return l && pc_paint_renderer_wants_time(l->renderer);
}
bool pc_engine_live_stroke_smoothing(const pc_engine_live_stroke *l, double brush[2], double pointer[2])
{
    return l && brush && pointer && pc_paint_renderer_smoothing_string(l->renderer, brush, pointer);
}
static pc_status live_restore(pc_engine_live_stroke *l, pc_surface *target, pc_rect area, pc_error *err)
{
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    float *data = NULL;
    size_t len = 0;
    pc_status status = pc_surface_read_region(&l->pre, area, &data, &len, err);
    if (status == PC_OK) {
        status = pc_surface_write_region(target, area, data, len, err);
    }
    free(data);
    return status;
}
static pc_status live_push_inner(pc_engine_live_stroke *l, const pc_paint_point *points, size_t count,
                                 pc_rect *damage, pc_error *err)
{
    pc_status status = pc_paint_renderer_push(l->renderer, points, count, err);
    pc_paint_point_vec reflected[3] = {{0}, {0}, {0}};
    size_t n = 0;
    if (status == PC_OK && l->symmetry) {
        status = pc_engine_symmetry_reflected_passes(l->symmetry, points, count, reflected, &n, err);
    }
    for (size_t i = 0; i < n && status == PC_OK; i++) {
        l->distinct[i] =
            l->distinct[i] || pc_engine_symmetry_has_distinct_mirror(points, reflected[i].v, count);
        status = pc_paint_renderer_push(l->mirrors[i], reflected[i].v, reflected[i].len, err);
    }
    for (size_t i = 0; i < 3; i++) {
        pc_paint_point_vec_fini(&reflected[i]);
    }
    pc_document *doc = NULL;
    pc_surface *target = NULL;
    bool lock = false;
    if (status == PC_OK) {
        status = pc_doc_arc_make_mut(&l->doc, &doc, err);
    }
    if (status == PC_OK) {
        status = pc_engine_target_surface(doc, l->has_layer, l->layer, yyjson_doc_get_root(l->params),
                                          &target, &lock, err);
    }
    if (status != PC_OK) {
        return status;
    }
    const pc_surface *selection = l->has_selection ? &l->selection : NULL;
    pc_paint_renderer *passes[4] = {l->renderer, NULL, NULL, NULL};
    size_t npasses = 1;
    for (size_t i = 0; i < 3; i++) {
        if (l->distinct[i]) {
            passes[npasses++] = l->mirrors[i];
        }
    }
    pc_rect changed = PC_RECT_EMPTY;
    if (npasses > 1) {
        if (!l->merged) {
            status = live_restore(l, target, l->tail, err);
            changed = l->tail;
        }
        pc_rect update = PC_RECT_EMPTY;
        if (status == PC_OK) {
            status =
                pc_paint_renderer_composite_union_live(passes, npasses, &l->union_live, l->merged != npasses,
                                                       &l->pre, target, selection, l->lock, &update, err);
        }
        l->merged = npasses;
        l->tail = pc_rect_union(l->tail, pc_paint_union_live_bounds(&l->union_live));
        changed = pc_rect_union(changed, update);
    } else {
        changed = l->tail;
        status = live_restore(l, target, l->tail, err);
        if (!pc_rect_is_empty(l->tail)) {
            pc_paint_renderer_mark_dirty(l->renderer, l->tail);
        }
        l->tail = PC_RECT_EMPTY;
        pc_rect update = PC_RECT_EMPTY;
        if (status == PC_OK) {
            status = pc_paint_renderer_composite(l->renderer, &l->pre, target, selection, l->lock, false,
                                                 &update, err);
        }
        changed = pc_rect_union(changed, update);
        pc_paint_renderer *tail = NULL;
        if (status == PC_OK) {
            status = pc_paint_renderer_tail_preview(l->renderer, &tail, err);
        }
        if (status == PC_OK && tail) {
            status =
                pc_paint_renderer_composite(tail, &l->pre, target, selection, l->lock, false, &l->tail, err);
        }
        pc_paint_renderer_free(tail);
        changed = pc_rect_union(changed, l->tail);
    }
    if (status == PC_OK && l->color != SIZE_MAX && l->has_layer) {
        status = pc_engine_hook_restrict_to_color(pc_doc_arc_get(l->base_doc), doc, l->layer, l->color, err);
    }
    if (status == PC_OK && damage) {
        *damage = changed;
    }
    return status;
}
pc_status pc_engine_live_stroke_push(pc_engine_live_stroke *l, const pc_paint_point *points, size_t count,
                                     pc_rect *damage, pc_error *err)
{
    if (!l || (!points && count) || count > 100000 || (l && l->count > 100000 - count)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid live stroke samples");
    }
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(points[i].pressure) || !isfinite(points[i].tilt_x) || !isfinite(points[i].tilt_y) ||
            !isfinite(points[i].rotation)) {
            return pc_fail(err, PC_ERR_INVALID, "invalid live pen sample");
        }
    }
    pc_status status = pc_engine_check_coords(points, count, "paint.stroke", err);
    if (status != PC_OK) {
        return status;
    }
    pc_cmyk_space *previous = l->cmyk ? pc_cmyk_space_enter(l->cmyk) : NULL;
    status = live_push_inner(l, points, count, damage, err);
    if (l->cmyk) {
        pc_cmyk_space_leave(previous);
    }
    if (status == PC_OK) {
        for (size_t i = 0; i < count && status == PC_OK; i++) {
            status = pc_paint_point_vec_push(&l->points, &points[i], err);
        }
        l->count += count;
    }
    return status;
}
pc_status pc_engine_live_stroke_begin(pc_engine_session *session, const char *command, yyjson_val *params,
                                      pc_engine_live_stroke **out, pc_error *err)
{
    if (!session || !command || !out ||
        (strcmp(command, "paint.stroke") && strcmp(command, "paint.pencil"))) {
        return pc_fail(err, PC_ERR_INVALID, "live strokes require Brush or Pencil");
    }
    yyjson_mut_doc *canonical = yyjson_mut_doc_new(NULL);
    if (!canonical) {
        return pc_fail(err, PC_ERR_NOMEM, "live stroke parameters");
    }
    yyjson_mut_val *fields =
        params ? pc_json_normalized_copy_val(canonical, params) : yyjson_mut_obj(canonical);
    fields = pc_engine_hook_inject_target(session, command, canonical, fields);
    yyjson_mut_doc_set_root(canonical, fields);
    yyjson_doc *canonical_read = fields ? yyjson_mut_doc_imut_copy(canonical, NULL) : NULL;
    yyjson_mut_doc_free(canonical);
    if (!canonical_read) {
        return pc_fail(err, PC_ERR_NOMEM, "live stroke parameters");
    }
    params = yyjson_doc_get_root(canonical_read);
    const char *hidden = pc_engine_hidden_target_refusal(session, command, params);
    if (hidden) {
        yyjson_doc_free(canonical_read);
        return pc_fail(err, PC_ERR_INVALID, "%s", hidden);
    }
    pc_status status = pc_engine_has_paintable(session, err);
    if (status != PC_OK) {
        yyjson_doc_free(canonical_read);
        return status;
    }
    pc_paint_point *points = NULL;
    size_t count = 0;
    pc_paint_brush brush = {0};
    status = pc_engine_parse_points(params, command, &points, &count, err);
    if (status == PC_OK) {
        if (!strcmp(command, "paint.pencil")) {
            status = pencil_brush(session, params, &brush, err);
        } else {
            status = pc_engine_resolve_brush(session, params, command, &brush, err);
            if (status == PC_OK) {
                with_blend_mode(&brush, params);
            }
        }
    }
    pc_engine_live_stroke *l = status == PC_OK ? calloc(1, sizeof *l) : NULL;
    if (status == PC_OK && !l) {
        status = pc_fail(err, PC_ERR_NOMEM, "live stroke");
    }
    if (status == PC_OK) {
        l->seed = brush.seed;
        pc_paint_brush_copy(&l->resolved, &brush);
        pc_buf json = PC_BUF_INIT;
        status = pc_json_write_val(params, 0, &json, err);
        if (status == PC_OK) {
            status = pc_json_parse(pc_buf_cstr(&json), json.len, &l->params, err);
        }
        pc_buf_fini(&json);
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (status == PC_OK) {
        status = pc_doc_arc_from_clone(pc_doc_arc_get(state->doc), &l->doc, err);
    }
    if (status == PC_OK) {
        l->base_doc = pc_doc_arc_retain(state->doc);
        l->color = pc_engine_hook_color_restriction(session, command, params);
        l->base = pc_doc_arc_get(state->doc)->id;
        l->revision = state->revision;
        l->pencil = !strcmp(command, "paint.pencil");
    }
    float zoom = 1;
    pc_document *doc = NULL;
    pc_surface *target = NULL;
    if (status == PC_OK) {
        status = stroke_target(session, params, &brush, &l->has_layer, &l->layer, &zoom, command, err);
    }
    if (status == PC_OK) {
        status = pc_doc_arc_make_mut(&l->doc, &doc, err);
    }
    if (status == PC_OK && doc->has_selection) {
        l->has_selection = true;
        status = pc_surface_clone(&doc->selection, &l->selection, err);
    }
    if (status == PC_OK) {
        status = pc_engine_target_surface(doc, l->has_layer, l->layer, params, &target, &l->lock, err);
    }
    if (status == PC_OK) {
        float fg[4];
        memcpy(fg, brush.color, sizeof fg);
        erase_locked(&brush, l->lock, session->tools.background);
        if (!strcmp(command, "paint.pencil") && flag(params, "autoErase", false)) {
            apply_auto_erase(&brush, target, count ? &points[0] : NULL, fg, session->tools.background);
        }
        status = pc_surface_clone(target, &l->pre, err);
    }
    if (status == PC_OK) {
        status = pc_paint_renderer_new(&brush, &target->format, zoom, &l->renderer, err);
    }
    if (status == PC_OK && state->symmetry) {
        l->symmetry = pc_engine_symmetry_clone(state->symmetry);
        if (!l->symmetry) {
            status = pc_fail(err, PC_ERR_NOMEM, "live stroke symmetry");
        }
        size_t n = pc_engine_symmetry_mirror_count(state->symmetry);
        for (size_t i = 0; i < n && i < 3 && status == PC_OK; i++) {
            status = pc_paint_renderer_new(&brush, &target->format, zoom, &l->mirrors[i], err);
        }
    }
    if (status == PC_OK) {
        l->cmyk = pc_engine_doc_cmyk_space(session);
        status = pc_engine_live_stroke_push(l, points, count, NULL, err);
    }
    pc_paint_brush_fini(&brush);
    free(points);
    yyjson_doc_free(canonical_read);
    if (status != PC_OK) {
        pc_engine_live_stroke_free(l);
        return status;
    }
    *out = l;
    return PC_OK;
}

void pc_engine_session_clear_live_stroke(pc_engine_session *session)
{
    if (!session) {
        return;
    }
    pc_engine_live_stroke_free(session->live_commit);
    session->live_commit = NULL;
}
void pc_engine_session_offer_live_stroke(pc_engine_session *session, pc_engine_live_stroke **live)
{
    if (!session || !live) {
        return;
    }
    pc_engine_session_clear_live_stroke(session);
    session->live_commit = *live;
    *live = NULL;
}
static bool live_settings_equal(yyjson_val *a, yyjson_val *b)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        return false;
    }
    yyjson_mut_val *x = pc_json_normalized_copy_val(d, a), *y = pc_json_normalized_copy_val(d, b);
    bool equal = false;
    if (x && y && yyjson_mut_is_obj(x) && yyjson_mut_is_obj(y)) {
        (void)yyjson_mut_obj_remove_key(x, "points");
        (void)yyjson_mut_obj_remove_key(x, "seed");
        (void)yyjson_mut_obj_remove_key(y, "points");
        (void)yyjson_mut_obj_remove_key(y, "seed");
        equal = pc_json_mut_equal(x, y);
    }
    yyjson_mut_doc_free(d);
    return equal;
}
static bool live_matches(const pc_engine_live_stroke *l, const pc_engine_session *session,
                         const char *command, yyjson_val *params, const pc_paint_point *points, size_t count,
                         const pc_paint_brush *brush)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    uint64_t seed = 0;
    if (!l || !state || l->pencil != (!strcmp(command, "paint.pencil")) ||
        l->base != pc_doc_arc_get(state->doc)->id ||
        l->color != pc_engine_hook_color_restriction(session, command, params) ||
        !pc_paint_brush_eq(&l->resolved, brush) || !pc_engine_symmetry_equal(l->symmetry, state->symmetry) ||
        l->revision != state->revision || (l->has_layer && !state->has_active_layer) ||
        (l->has_layer && l->layer != state->active_layer) || l->points.len != count ||
        !pc_json_as_u64(pc_json_get(params, "seed"), &seed) || seed != l->seed) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!pc_paint_point_eq(&l->points.v[i], &points[i])) {
            return false;
        }
    }
    return live_settings_equal(yyjson_doc_get_root(l->params), params);
}

static pc_status live_surface(const pc_engine_live_stroke *l, pc_surface *out, pc_rect *bounds, pc_error *err)
{
    const pc_surface *surface = NULL;
    pc_status status = pc_engine_read_target_surface(pc_doc_arc_get(l->doc), l->has_layer, l->layer,
                                                     yyjson_doc_get_root(l->params), &surface, err);
    if (status == PC_OK) {
        status = pc_surface_clone(surface, out, err);
    }
    if (status == PC_OK) {
        *bounds = pc_engine_live_stroke_bounds(l);
    }
    return status;
}

uint64_t pc_engine_session_live_stroke_reuses(const pc_engine_session *session)
{
    return session ? session->live_commit_reuses : 0;
}
