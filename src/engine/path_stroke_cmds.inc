/* Stroke Path: exact geometry, optional pressure taper, one undo/journal entry. */
#include "paint/render.h"
#include <float.h>
typedef struct pc_path_stroke_line {
    pc_paint_point *points;
    size_t count;
} pc_path_stroke_line;
static void ps_lines_fini(pc_path_stroke_line *lines, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(lines[i].points);
    }
    free(lines);
}
static pc_status ps_points(const pc_vec_polyline *line, bool simulate, size_t budget,
                           pc_path_stroke_line *out, pc_error *err)
{
    size_t n = line->len;
    if (!n) {
        *out = (pc_path_stroke_line){0};
        return PC_OK;
    }
    if (!budget) {
        return pc_fail(err, PC_ERR_LIMIT, "Stroke Path exceeds its point budget");
    }
    size_t segments = n - 1 + (line->closed ? 1u : 0u);
    double total = 0;
    for (size_t i = 0; i < segments; i++) {
        size_t next = (i + 1) % n;
        total += hypot(line->pts[next][0] - line->pts[i][0], line->pts[next][1] - line->pts[i][1]);
    }
    bool taper = simulate && isfinite(total) && total > 0;
    double step = fmax(1, total / 200000.);
    size_t count = n + (line->closed ? 1u : 0u);
    if (taper) {
        count = 1;
        for (size_t i = 0; i < segments; i++) {
            size_t next = (i + 1) % n;
            double length = hypot(line->pts[next][0] - line->pts[i][0], line->pts[next][1] - line->pts[i][1]);
            double samples = fmin(200000, fmax(1, ceil(length / step)));
            if (!isfinite(samples) || samples > (double)(budget - count)) {
                return pc_fail(err, PC_ERR_LIMIT, "Stroke Path exceeds its point budget");
            }
            count += (size_t)samples;
        }
    }
    if (count > budget) {
        return pc_fail(err, PC_ERR_LIMIT, "Stroke Path exceeds one million points");
    }
    pc_paint_point *points = pc_malloc_array(count, sizeof *points);
    if (!points) {
        return pc_fail(err, PC_ERR_NOMEM, "Stroke Path points");
    }
    if (!taper) {
        for (size_t i = 0; i < count; i++) {
            size_t index = i % n;
            points[i] = pc_paint_point_make(line->pts[index][0], line->pts[index][1], 1);
        }
    } else {
        double walked = 0;
        size_t written = 0;
        for (size_t i = 0; i < segments; i++) {
            size_t next = (i + 1) % n;
            const double *a = line->pts[i], *b = line->pts[next];
            double length = hypot(b[0] - a[0], b[1] - a[1]);
            size_t samples = (size_t)fmin(200000, fmax(1, ceil(length / step)));
            for (size_t j = 0; j < samples; j++) {
                double t = (double)j / (double)samples;
                float pressure =
                    (float)fmin(1, fmax(0, sin((walked + length * t) / total * 3.14159265358979323846)));
                points[written++] =
                    pc_paint_point_make(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, pressure);
            }
            walked += length;
        }
        size_t last = line->closed ? 0 : n - 1;
        points[written++] = pc_paint_point_make(line->pts[last][0], line->pts[last][1], 0);
        if (written != count) {
            free(points);
            return pc_fail(err, PC_ERR_INVALID, "invalid Stroke Path sample count");
        }
    }
    *out = (pc_path_stroke_line){points, count};
    return PC_OK;
}
typedef struct pc_path_stroke_context {
    pc_layer_id layer;
    const pc_path_stroke_line *lines;
    size_t count;
    pc_paint_brush brush;
    float background[4];
    pc_rect damage;
    yyjson_val *params;
    const char *command;
    bool has_damage;
} pc_path_stroke_context;
static pc_status ps_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                         pc_error *err)
{
    (void)has_active;
    (void)active;
    pc_path_stroke_context *c = context;
    pc_locks locks = pc_document_effective_locks(doc, c->layer);
    pc_layer *layer = pc_document_layer_mut(doc, c->layer);
    if (!layer) {
        return pc_engine_err_no_layer(err, c->layer);
    }
    if (locks.pixels || locks.all) {
        return pc_engine_err_other(err, "the layer \"%s\" is locked", pc_str_c(&layer->name));
    }
    if (layer->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_other(err, "Stroke Path needs a pixel layer");
    }
    pc_paint_brush brush = {0};
    pc_paint_brush_copy(&brush, &c->brush);
    if (brush.erase && locks.transparency) {
        brush.erase = false;
        memcpy(brush.color, c->background, sizeof brush.color);
        brush.color_cmyk_set = false;
    }
    const pc_surface *selection = doc->has_selection ? &doc->selection : NULL;
    pc_status status = PC_OK;
    for (size_t i = 0; status == PC_OK && i < c->count; i++) {
        pc_paint_stroke stroke = {.brush = brush, .points = c->lines[i].points, .count = c->lines[i].count};
        pc_rect damage = PC_RECT_EMPTY;
        status = pc_paint_apply_stroke(&layer->content.u.raster, &stroke, selection, locks.transparency,
                                       &damage, err);
        if (status == PC_OK && !pc_rect_is_empty(damage)) {
            c->damage = pc_rect_is_empty(c->damage) ? damage : pc_rect_union(c->damage, damage);
        }
    }
    pc_paint_brush_fini(&brush);
    return status;
}
static pc_status ps_commands(pc_engine_session *session, void *context, pc_error *err)
{
    pc_path_stroke_context *c = context;
    pc_status status = PC_OK;
    c->has_damage = true;
    for (size_t i = 0; status == PC_OK && i < c->count; i++) {
        const pc_path_stroke_line *line = &c->lines[i];
        if (!line->count) {
            continue;
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = doc ? yyjson_val_mut_copy(doc, c->params) : NULL,
                       *points = doc ? yyjson_mut_arr(doc) : NULL;
        bool ok = p && points;
        const char *remove[] = {"name", "tool", "simulatePressure", "coalesce", "points"};
        for (size_t j = 0; p && j < 5; j++) {
            (void)yyjson_mut_obj_remove_str(p, remove[j]);
        }
        for (size_t j = 0; ok && j < line->count; j++) {
            yyjson_mut_val *point = yyjson_mut_arr(doc);
            const pc_paint_point *v = &line->points[j];
            ok = point && yyjson_mut_arr_add_real(doc, point, v->x) &&
                 yyjson_mut_arr_add_real(doc, point, v->y) &&
                 yyjson_mut_arr_add_real(doc, point, v->pressure) && yyjson_mut_arr_add_val(points, point);
        }
        ok = ok && yyjson_mut_obj_add_val(doc, p, "points", points);
        pc_buf json = PC_BUF_INIT;
        pc_json_value out = PC_JSON_VALUE_INIT;
        status =
            ok ? pc_json_write(p, 0, &json, err) : pc_fail(err, PC_ERR_NOMEM, "Stroke Path tool parameters");
        if (status == PC_OK) {
            status = pc_engine_session_execute_json(session, c->command, pc_buf_cstr(&json), &out, err);
        }
        const pc_engine_doc_state *state = pc_engine_session_active(session);
        if (status == PC_OK) {
            if (!state || !state->has_last_damage) {
                c->has_damage = false;
            } else if (c->has_damage && !pc_rect_is_empty(state->last_damage)) {
                c->damage = pc_rect_is_empty(c->damage) ? state->last_damage
                                                        : pc_rect_union(c->damage, state->last_damage);
            }
        }
        pc_buf_fini(&json);
        pc_json_value_fini(&out);
        yyjson_mut_doc_free(doc);
    }
    return status;
}
static pc_status run_path_stroke(pc_engine_session *session, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const struct {
        const char *tool, *command;
    } tools[] = {{"backgroundEraser", "paint.backgroundEraser"},
                 {"cloneStamp", "paint.cloneStamp"},
                 {"patternStamp", "paint.patternStamp"},
                 {"healingBrush", "paint.healingBrush"},
                 {"spotHealingBrush", "paint.spotHealing"},
                 {"historyBrush", "paint.historyBrush"},
                 {"blur", "paint.blur"},
                 {"sharpen", "paint.sharpen"},
                 {"smudge", "paint.smudge"},
                 {"dodge", "paint.dodge"},
                 {"burn", "paint.burn"},
                 {"sponge", "paint.sponge"},
                 {"colorReplacement", "paint.colorReplacement"},
                 {"mixerBrush", "paint.mixerBrush"}};
    bool simulate = false;
    yyjson_val *value = pc_json_get(p, "simulatePressure");
    if (value && !yyjson_is_null(value) && !pc_json_as_bool(value, &simulate)) {
        return pc_engine_err_bad_params(err, "path.stroke", "simulatePressure must be boolean");
    }
    const char *tool = pc_json_as_str(pc_json_get(p, "tool"), NULL);
    if (!tool) {
        if (pc_json_get(p, "tool") && !yyjson_is_null(pc_json_get(p, "tool"))) {
            return pc_engine_err_bad_params(err, "path.stroke", "tool must be a string");
        }
        tool = "brush";
    }
    const char *command = NULL;
    for (size_t i = 0; i < sizeof tools / sizeof *tools; i++) {
        if (!strcmp(tool, tools[i].tool)) {
            command = tools[i].command;
            break;
        }
    }
    if (!command && strcmp(tool, "brush") && strcmp(tool, "pencil") && strcmp(tool, "eraser")) {
        return pc_engine_err_bad_params(err, "path.stroke", "unknown tool");
    }
    size_t name_len = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &name_len);
    pc_path path = {0};
    pc_status status = resolve_path(session, name, name_len, &path, err);
    if (status == PC_OK) {
        status = pc_vec_validate_shape(&path, NULL, err);
    }
    pc_vec_polyline *polylines = NULL;
    size_t count = 0;
    if (status == PC_OK) {
        status = pc_vec_flatten_path(&path, .1, &polylines, &count, err);
    }
    pc_path_fini(&path);
    if (status != PC_OK) {
        return status;
    }
    pc_path_stroke_line *lines = pc_calloc_array(count ? count : 1, sizeof *lines);
    if (!lines) {
        pc_vec_polylines_free(polylines, count);
        return pc_fail(err, PC_ERR_NOMEM, "Stroke Path lines");
    }
    size_t used = 0;
    for (size_t i = 0; status == PC_OK && i < count; i++) {
        status = ps_points(&polylines[i], simulate, 1000000 - used, &lines[i], err);
        if (status == PC_OK) {
            used += lines[i].count;
        }
    }
    pc_vec_polylines_free(polylines, count);
    pc_path_stroke_context ctx = {
        .lines = lines, .count = count, .command = command, .params = p, .has_damage = true};
    if (status == PC_OK && !command) {
        status = layer_id_of(session, p, &ctx.layer, err);
        pc_paint_brush_copy(&ctx.brush, &session->tools.brush);
        memcpy(ctx.brush.color, session->tools.foreground, sizeof ctx.brush.color);
        memcpy(ctx.background, session->tools.background, sizeof ctx.background);
        memcpy(ctx.brush.background, ctx.background, sizeof ctx.brush.background);
        ctx.brush.erase = !strcmp(tool, "eraser");
        ctx.brush.smoothing.amount = 0;
        const char *keys[] = {"size", "hardness", "opacity"};
        float *fields[] = {&ctx.brush.size, &ctx.brush.hardness, &ctx.brush.opacity};
        for (size_t i = 0; status == PC_OK && i < 3; i++) {
            value = pc_json_get(p, keys[i]);
            double number = 0;
            if (!value || yyjson_is_null(value)) {
                continue;
            }
            if (!pc_json_as_f64(value, &number) || !isfinite(number) || fabs(number) > FLT_MAX ||
                (i == 1 && (number < 0 || number > 1)) || (i == 2 && (number < 0 || number > 100))) {
                status = pc_engine_err_bad_params(err, "path.stroke", "invalid brush scalar");
            } else {
                *fields[i] = (float)(i == 2 ? number / 100 : number);
            }
        }
        value = pc_json_get(p, "color");
        pc_color color;
        if (status == PC_OK && value && !yyjson_is_null(value)) {
            if (!vcolor(value, &color)) {
                status = pc_engine_err_bad_params(err, "path.stroke", "invalid color");
            } else {
                pc_color_to_rgb(&color, ctx.brush.color);
                ctx.brush.color[3] = color.alpha;
            }
        }
        float inks[4];
        ctx.brush.color_cmyk_set = pc_engine_tool_state_picked_cmyk(&session->tools, ctx.brush.color, inks);
        if (ctx.brush.color_cmyk_set) {
            memcpy(ctx.brush.color_cmyk_rgb, ctx.brush.color, 3 * sizeof(float));
            memcpy(ctx.brush.color_cmyk_inks, inks, sizeof inks);
        }
        if (!strcmp(tool, "pencil")) {
            ctx.brush.hardness = 1;
        }
        for (size_t i = 0; status == PC_OK && i < 4; i++) {
            if (!isfinite(ctx.brush.color[i])) {
                status = pc_engine_err_bad_params(err, "path.stroke", "color must be finite");
            }
        }
        if (status == PC_OK) {
            status = pc_engine_validate_brush(&ctx.brush, "path.stroke", err);
        }
        pc_cmyk_space *space = status == PC_OK ? pc_engine_doc_cmyk_space(session) : NULL,
                      *prior = space ? pc_cmyk_space_enter(space) : NULL;
        if (status == PC_OK) {
            status = pc_engine_session_edit(session, "Stroke Path", ps_edit, &ctx, err);
        }
        if (space) {
            pc_cmyk_space_leave(prior);
            pc_cmyk_space_release(space);
        }
    } else if (status == PC_OK) {
        size_t journal = session->njournal;
        status = pc_engine_compound(session, "Stroke Path", ps_commands, &ctx, err);
        for (size_t i = journal; i < session->njournal; i++) {
            free(session->journal[i].id);
            pc_json_value_fini(&session->journal[i].params);
        }
        session->njournal = journal;
    }
    pc_paint_brush_fini(&ctx.brush);
    ps_lines_fini(lines, count);
    if (status != PC_OK) {
        return status;
    }
    pc_engine_doc_state *state = pc_engine_session_active_mut(session);
    if (state) {
        state->has_last_damage = ctx.has_damage;
        state->last_damage = ctx.damage;
    }
    pc_rect damage = ctx.has_damage ? ctx.damage
                     : state        ? pc_document_bounds(pc_doc_arc_get(state->doc))
                                    : PC_RECT_EMPTY;
    yyjson_mut_doc *doc = NULL;
    status = pc_engine_result_doc(out, &doc, err);
    if (status != PC_OK) {
        return status;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc), *array = yyjson_mut_arr(doc);
    bool ok = root && array && yyjson_mut_arr_add_int(doc, array, damage.x0) &&
              yyjson_mut_arr_add_int(doc, array, damage.y0) &&
              yyjson_mut_arr_add_uint(doc, array, pc_rect_width(damage)) &&
              yyjson_mut_arr_add_uint(doc, array, pc_rect_height(damage)) &&
              yyjson_mut_obj_add_val(doc, root, "damage", array);
    if (!ok) {
        return pc_fail(err, PC_ERR_NOMEM, "Stroke Path result");
    }
    yyjson_mut_doc_set_root(doc, root);
    return PC_OK;
}
