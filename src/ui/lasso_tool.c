#include "lasso_tool.h"
#include "ui/tool_cursor.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define LIMIT 100000u
void pc_ui_lasso_fini(pc_ui_lasso *l)
{
    if (!l)
        return;
    free(l->path.v);
    free(l->live.v);
    free(l->trail.v);
    free(l->anchors);
    pc_engine_magnetic_source_free(l->source);
    *l = (pc_ui_lasso){0};
}
bool pc_ui_lasso_tool(pc_ui_tool t)
{
    return t == PC_TOOL_LASSO || t == PC_TOOL_POLYGON_LASSO || t == PC_TOOL_MAGNETIC_LASSO;
}
void pc_ui_lasso_sync(pc_engine_session *session, pc_ui_state *ui)
{
    pc_ui_lasso *l = &ui->lasso;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (l->active &&
        (!state || l->tool != (int)ui->tool || l->document != pc_doc_arc_get(state->doc)->id ||
         l->revision != state->revision || l->layer != (state->has_active_layer ? state->active_layer : 0)))
        pc_ui_lasso_fini(l);
}
static pc_status append(pc_ui_lasso_points *p, const double point[2], pc_error *err)
{
    if (p->n && p->v[p->n - 1][0] == point[0] && p->v[p->n - 1][1] == point[1])
        return PC_OK;
    if (p->n >= LIMIT)
        return pc_fail(err, PC_ERR_LIMIT, "lasso point limit");
    if (p->n == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 32;
        if (capacity > LIMIT)
            capacity = LIMIT;
        void *next = realloc(p->v, capacity * sizeof *p->v);
        if (!next)
            return pc_fail(err, PC_ERR_NOMEM, "lasso points");
        p->v = next;
        p->capacity = capacity;
    }
    memcpy(p->v[p->n++], point, 2 * sizeof *point);
    return PC_OK;
}
static pc_status anchor(pc_ui_lasso *l, pc_error *err)
{
    if (!l->path.n || (l->nanchors && l->anchors[l->nanchors - 1] == l->path.n - 1))
        return PC_OK;
    if (l->nanchors == l->anchor_capacity) {
        size_t capacity = l->anchor_capacity ? l->anchor_capacity * 2 : 32;
        if (capacity > LIMIT)
            capacity = LIMIT;
        void *next = realloc(l->anchors, capacity * sizeof *l->anchors);
        if (!next)
            return pc_fail(err, PC_ERR_NOMEM, "lasso anchors");
        l->anchors = next;
        l->anchor_capacity = capacity;
    }
    l->anchors[l->nanchors++] = l->path.n - 1;
    return PC_OK;
}
static double distance(const double a[2], const double b[2])
{
    return hypot(a[0] - b[0], a[1] - b[1]);
}
static pc_status result(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    pc_status status =
        root ? pc_json_value_from_mut(root, out, err) : pc_fail(err, PC_ERR_NOMEM, "lasso response");
    yyjson_mut_doc_free(doc);
    return status;
}
static pc_algo_magnetic_settings settings(const pc_ui_state *ui, pc_ui_tool_point point)
{
    double width = ui->tool_options.magnetic_width;
    if (ui->tool_options.magnetic_pressure && point.pressure >= 0 && point.pressure < 1)
        width = 1 + (width - 1) * (1 - point.pressure);
    return pc_algo_magnetic_settings_new(width, ui->tool_options.magnetic_contrast / 100);
}
static pc_status fasten(pc_ui_lasso *l, pc_error *err)
{
    for (size_t i = 0; i < l->live.n; i++) {
        pc_status status = append(&l->path, l->live.v[i], err);
        if (status != PC_OK)
            return status;
    }
    l->live.n = l->trail.n = 0;
    pc_status status = anchor(l, err);
    if (status == PC_OK && l->path.n)
        status = append(&l->trail, l->path.v[l->path.n - 1], err);
    return status;
}
static pc_status follow(pc_ui_lasso *l, const double p[2], pc_algo_magnetic_settings set, double every,
                        bool auto_fasten, pc_error *err)
{
    if (!l->path.n)
        return PC_OK;
    pc_status status = append(&l->trail, p, err);
    if (status != PC_OK)
        return status;
    double target[2];
    status = pc_engine_magnetic_source_snap(l->source, p, set, target, err);
    if (status != PC_OK)
        return status;
    double (*traced)[2] = NULL;
    size_t n = 0;
    status =
        pc_engine_magnetic_source_trace(l->source, l->path.v[l->path.n - 1], target,
                                        (const double (*)[2])l->trail.v, l->trail.n, set, &traced, &n, err);
    if (status != PC_OK)
        return status;
    if (n > LIMIT) {
        free(traced);
        return pc_fail(err, PC_ERR_LIMIT, "magnetic trace point limit");
    }
    free(l->live.v);
    l->live = (pc_ui_lasso_points){.v = traced, .n = n, .capacity = n};
    /* Fasten exact arc-length intervals behind the live pointer. */
    if (auto_fasten && n > 1) {
        double walked = 0;
        size_t fastened = 0, consumed = 0, next_vertex = 1;
        double from[2] = {traced[0][0], traced[0][1]};
        for (size_t i = 1; i < n && fastened < 64; i++) {
            double segment = distance(from, traced[i]);
            while (segment > 0 && walked + segment >= every && fastened < 64) {
                double part = (every - walked) / segment;
                double q[2] = {from[0] + part * (traced[i][0] - from[0]),
                               from[1] + part * (traced[i][1] - from[1])};
                while (next_vertex < i) {
                    status = append(&l->path, traced[next_vertex++], err);
                    if (status != PC_OK)
                        return status;
                }
                status = append(&l->path, q, err);
                if (status == PC_OK)
                    status = anchor(l, err);
                if (status != PC_OK)
                    return status;
                from[0] = q[0];
                from[1] = q[1];
                segment = distance(from, traced[i]);
                walked = 0;
                fastened++;
                consumed = i;
            }
            if (walked + segment < every || fastened < 64) {
                walked += segment;
                from[0] = traced[i][0];
                from[1] = traced[i][1];
            }
        }
        if (fastened) {
            size_t remaining = n - consumed;
            memmove(traced + 1, traced + consumed, remaining * sizeof *traced);
            memcpy(traced[0], l->path.v[l->path.n - 1], sizeof *traced);
            l->live.n = remaining + 1;
            l->trail.n = 0;
            (void)append(&l->trail, traced[0], NULL);
            (void)append(&l->trail, p, NULL);
        }
    }
    return PC_OK;
}
pc_status pc_ui_lasso_finish(pc_engine_session *session, pc_ui_state *ui, bool straight, pc_json_value *out,
                             pc_error *err)
{
    pc_ui_lasso_sync(session, ui);
    pc_ui_lasso *l = &ui->lasso;
    if (!l->active)
        return PC_ERR_NOT_FOUND;
    pc_status status = PC_OK;
    if (l->tool == PC_TOOL_MAGNETIC_LASSO && !l->source) {
        status = pc_engine_magnetic_source_new(session, &l->source, err);
        if (status != PC_OK)
            return status;
    }
    if (l->tool == PC_TOOL_MAGNETIC_LASSO && !straight) {
        status = fasten(l, err);
        if (status == PC_OK && l->path.n > 1) {
            double (*closing)[2] = NULL;
            size_t n = 0;
            status = pc_engine_magnetic_source_trace(l->source, l->path.v[l->path.n - 1], l->path.v[0], NULL,
                                                     0, settings(ui, (pc_ui_tool_point){.pressure = 1}),
                                                     &closing, &n, err);
            for (size_t i = 1; status == PC_OK && i + 1 < n; i++)
                status = append(&l->path, closing[i], err);
            free(closing);
        }
    } else if (l->tool == PC_TOOL_MAGNETIC_LASSO)
        status = fasten(l, err);
    if (status != PC_OK)
        return status;
    if (l->path.n < 3) {
        pc_ui_lasso_fini(l);
        return result(out, err);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL, *points = doc ? yyjson_mut_arr(doc) : NULL;
    bool ok = root && points && yyjson_mut_obj_add_strcpy(doc, root, "mode", l->mode) &&
              yyjson_mut_obj_add_bool(doc, root, "antiAlias", ui->tool_options.anti_alias) &&
              yyjson_mut_obj_add_real(doc, root, "feather", ui->tool_options.feather);
    for (size_t i = 0; ok && i < l->path.n; i++) {
        yyjson_mut_val *point = yyjson_mut_arr(doc);
        ok = point && yyjson_mut_arr_add_real(doc, point, l->path.v[i][0]) &&
             yyjson_mut_arr_add_real(doc, point, l->path.v[i][1]) && yyjson_mut_arr_add_val(points, point);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, root, "points", points);
    if (l->tool == PC_TOOL_MAGNETIC_LASSO)
        ok = ok && yyjson_mut_obj_add_bool(doc, root, "trace", false);
    pc_buf text = PC_BUF_INIT;
    status = ok ? pc_json_write(root, 0, &text, err) : pc_fail(err, PC_ERR_NOMEM, "lasso parameters");
    if (status == PC_OK)
        status = pc_engine_session_execute_json(
            session, l->tool == PC_TOOL_MAGNETIC_LASSO ? "select.magneticLasso" : "select.lasso",
            pc_buf_cstr(&text), out, err);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(doc);
    if (status == PC_OK)
        pc_ui_lasso_fini(l);
    return status;
}
pc_status pc_ui_lasso_retract(pc_engine_session *session, pc_ui_state *ui, pc_json_value *out, pc_error *err)
{
    pc_ui_lasso_sync(session, ui);
    pc_ui_lasso *l = &ui->lasso;
    if (!l->active)
        return PC_ERR_NOT_FOUND;
    if (l->nanchors <= 1)
        pc_ui_lasso_fini(l);
    else {
        l->nanchors--;
        l->path.n = l->anchors[l->nanchors - 1] + 1;
        l->live.n = l->trail.n = 0;
    }
    return result(out, err);
}
pc_status pc_ui_lasso_pointer(pc_engine_session *session, pc_ui_state *ui, pc_ui_pointer_kind kind,
                              pc_ui_tool_point point, unsigned mods, double zoom, pc_json_value *out,
                              pc_error *err)
{
    pc_ui_lasso_sync(session, ui);
    if (!pc_ui_lasso_tool(ui->tool) || ui->transform.active)
        return PC_ERR_NOT_FOUND;
    pc_ui_lasso *l = &ui->lasso;
    if (kind == PC_UI_POINTER_CANCEL) {
        pc_ui_lasso_fini(l);
        return result(out, err);
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || ui->n_dialogs)
        return pc_fail(err, PC_ERR_INVALID, "lasso is unavailable");
    double p[2] = {point.x, point.y};
    if (!isfinite(p[0]) || !isfinite(p[1]) || fabs(p[0]) > 1000000 || fabs(p[1]) > 1000000)
        return pc_fail(err, PC_ERR_INVALID, "invalid lasso point");
    const pc_document *document = pc_doc_arc_get(state->doc);
    bool magnetic = ui->tool == PC_TOOL_MAGNETIC_LASSO, alt = (mods & PC_UI_MOD_ALT) != 0;
    if (magnetic && l->active && !l->source) {
        pc_status status = pc_engine_magnetic_source_new(session, &l->source, err);
        if (status != PC_OK)
            return status;
    }
    if (!l->active) {
        if (kind != PC_UI_POINTER_DOWN)
            return result(out, err);
        l->tool = (int)ui->tool;
        l->document = document->id;
        l->revision = state->revision;
        l->layer = state->has_active_layer ? state->active_layer : 0;
        l->initial_alt = document->has_selection && alt;
        l->polygonal = alt && !l->initial_alt;
        const char *mode = pc_ui_selection_mode(
            ui->tool, ui->selection_mode, document->has_selection ? mods : mods & ~(unsigned)PC_UI_MOD_ALT,
            document->has_selection);
        (void)snprintf(l->mode, sizeof l->mode, "%s", mode);
        if (magnetic) {
            pc_status status = pc_engine_magnetic_source_new(session, &l->source, err);
            if (status == PC_OK)
                status = pc_engine_magnetic_source_snap(l->source, p, settings(ui, point), p, err);
            if (status != PC_OK) {
                pc_ui_lasso_fini(l);
                return status;
            }
        }
        l->active = true;
    }
    if (!alt)
        l->alt_released = true;
    bool polygonal = alt && (!l->initial_alt || l->alt_released);
    if (ui->tool == PC_TOOL_LASSO && l->polygonal && !polygonal && !l->down)
        return pc_ui_lasso_finish(session, ui, true, out, err);
    l->polygonal = polygonal;
    if (ui->tool == PC_TOOL_POLYGON_LASSO && (mods & PC_UI_MOD_SHIFT) && !alt && l->path.n) {
        const double *last = l->path.v[l->path.n - 1];
        double length = distance(last, p);
        double angle = round(atan2(p[1] - last[1], p[0] - last[0]) / .7853981633974483) * .7853981633974483;
        p[0] = last[0] + length * cos(angle);
        p[1] = last[1] + length * sin(angle);
    }
    memcpy(l->cursor, p, sizeof p);
    pc_status status = PC_OK;
    if (kind == PC_UI_POINTER_DOWN) {
        if (l->path.n >= 3 && distance(l->path.v[0], p) < 8 / fmax(.01, zoom))
            return pc_ui_lasso_finish(session, ui, alt, out, err);
        if (magnetic && l->path.n && !alt) {
            status = follow(l, p, settings(ui, point), 1, false, err);
            if (status == PC_OK)
                status = fasten(l, err);
        } else {
            if (!magnetic && (mods & PC_UI_MOD_SHIFT) && !alt && l->path.n) {
                double *last = l->path.v[l->path.n - 1];
                double len = distance(last, p);
                double angle =
                    round(atan2(p[1] - last[1], p[0] - last[0]) / (.7853981633974483)) * .7853981633974483;
                p[0] = last[0] + len * cos(angle);
                p[1] = last[1] + len * sin(angle);
            }
            status = append(&l->path, p, err);
            if (status == PC_OK)
                status = anchor(l, err);
            if (magnetic) {
                l->live.n = l->trail.n = 0;
                (void)append(&l->trail, p, NULL);
            }
        }
        l->down = true;
        l->freehand = alt;
    } else if (kind == PC_UI_POINTER_MOVE) {
        if (magnetic && !(alt && l->down)) {
            double frequency = fmin(100, fmax(0, ui->tool_options.magnetic_frequency));
            status =
                follow(l, p, settings(ui, point), (8 + (100 - frequency) * .9) / fmax(.01, zoom), !alt, err);
        } else if (l->down && (ui->tool == PC_TOOL_LASSO || alt))
            status = append(&l->path, p, err);
    } else if (kind == PC_UI_POINTER_UP) {
        if (l->down && (ui->tool == PC_TOOL_LASSO || alt))
            status = append(&l->path, p, err);
        l->down = false;
        if (status == PC_OK && (magnetic || alt))
            status = anchor(l, err);
        if (status == PC_OK && ui->tool == PC_TOOL_LASSO && !l->polygonal)
            return pc_ui_lasso_finish(session, ui, true, out, err);
    }
    return status == PC_OK ? result(out, err) : status;
}
pc_status pc_ui_lasso_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_key key, unsigned mods,
                          double zoom, pc_json_value *out, pc_error *err)
{
    (void)zoom;
    pc_ui_lasso_sync(session, ui);
    if (!pc_ui_lasso_tool(ui->tool) || ui->transform.active || ui->n_dialogs)
        return PC_ERR_NOT_FOUND;
    pc_ui_lasso *l = &ui->lasso;
    if (ui->tool == PC_TOOL_MAGNETIC_LASSO && (key == PC_KEY_OPENBRACKET || key == PC_KEY_CLOSEBRACKET)) {
        ui->tool_options.magnetic_width =
            fminf(256, fmaxf(1, ui->tool_options.magnetic_width + (key == PC_KEY_OPENBRACKET ? -1 : 1)));
        return result(out, err);
    }
    if (!l->active)
        return PC_ERR_NOT_FOUND;
    if (key == PC_KEY_ESCAPE) {
        pc_ui_lasso_fini(l);
        return result(out, err);
    }
    if (key == PC_KEY_ENTER)
        return pc_ui_lasso_finish(session, ui, (mods & PC_UI_MOD_ALT) != 0, out, err);
    if (key == PC_KEY_BACKSPACE || key == PC_KEY_DELETE)
        return pc_ui_lasso_retract(session, ui, out, err);
    return PC_ERR_NOT_FOUND;
}
bool pc_ui_lasso_replaces_selection(const pc_ui_state *ui)
{
    return ui->lasso.active && !strcmp(ui->lasso.mode, "replace");
}
pc_status pc_ui_lasso_to_json(const pc_ui_lasso *l, yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc), *path = yyjson_mut_arr(doc), *live = yyjson_mut_arr(doc),
                   *anchors = yyjson_mut_arr(doc);
    bool ok = o && path && live && anchors;
    for (unsigned list = 0; ok && list < 2; list++) {
        const pc_ui_lasso_points *p = list ? &l->live : &l->path;
        for (size_t i = 0; ok && i < p->n; i++) {
            yyjson_mut_val *v = yyjson_mut_arr(doc);
            ok = v && yyjson_mut_arr_add_real(doc, v, p->v[i][0]) &&
                 yyjson_mut_arr_add_real(doc, v, p->v[i][1]) && yyjson_mut_arr_add_val(list ? live : path, v);
        }
    }
    for (size_t i = 0; ok && i < l->nanchors; i++)
        ok = yyjson_mut_arr_add_uint(doc, anchors, l->anchors[i]);
    ok = ok && yyjson_mut_obj_add_sint(doc, o, "tool", l->tool) &&
         yyjson_mut_obj_add_uint(doc, o, "revision", l->revision) &&
         yyjson_mut_obj_add_uint(doc, o, "layer", l->layer) &&
         yyjson_mut_obj_add_bool(doc, o, "polygonal", l->polygonal) &&
         yyjson_mut_obj_add_bool(doc, o, "initialAlt", l->initial_alt) &&
         yyjson_mut_obj_add_bool(doc, o, "altReleased", l->alt_released);
    ok = ok && yyjson_mut_obj_add_val(doc, o, "path", path) && yyjson_mut_obj_add_val(doc, o, "live", live) &&
         yyjson_mut_obj_add_val(doc, o, "anchors", anchors) &&
         yyjson_mut_obj_add_strcpy(doc, o, "mode", l->mode) &&
         yyjson_mut_obj_add_bool(doc, o, "active", l->active) &&
         yyjson_mut_obj_add_uint(doc, o, "document", l->document);
    if (!ok)
        return pc_fail(err, PC_ERR_NOMEM, "lasso state");
    *out = o;
    return PC_OK;
}

pc_status pc_ui_lasso_from_json(yyjson_val *v, pc_ui_lasso *out, pc_error *err)
{
    pc_ui_lasso next = {0};
    if (!v || yyjson_is_null(v)) {
        pc_ui_lasso_fini(out);
        return PC_OK;
    }
    if (!yyjson_is_obj(v))
        return pc_fail(err, PC_ERR_INVALID, "invalid saved lasso");
    int64_t tool = 0;
    if (!pc_json_as_i64(pc_json_get(v, "tool"), &tool) || tool < 0 || tool >= PC_TOOL_N)
        return pc_fail(err, PC_ERR_INVALID, "invalid saved lasso tool");
    next.tool = (int)tool;
    bool ok = pc_json_as_bool(pc_json_get(v, "active"), &next.active) &&
              pc_json_as_u64(pc_json_get(v, "document"), &next.document) &&
              pc_json_as_u64(pc_json_get(v, "revision"), &next.revision) &&
              pc_json_as_u64(pc_json_get(v, "layer"), &next.layer);
    yyjson_val *polygonal = pc_json_get(v, "polygonal"), *initial = pc_json_get(v, "initialAlt"),
               *released = pc_json_get(v, "altReleased");
    ok = ok && pc_json_as_bool(polygonal, &next.polygonal) && pc_json_as_bool(initial, &next.initial_alt) &&
         pc_json_as_bool(released, &next.alt_released);
    size_t length = 0;
    const char *mode = pc_json_as_str(pc_json_get(v, "mode"), &length);
    if (!mode || length >= sizeof next.mode || strlen(mode) != length)
        ok = false;
    else
        memcpy(next.mode, mode, length + 1);
    if (next.active && (!next.document || !pc_ui_lasso_tool((pc_ui_tool)next.tool) ||
                        (strcmp(next.mode, "replace") && strcmp(next.mode, "add") &&
                         strcmp(next.mode, "subtract") && strcmp(next.mode, "intersect"))))
        ok = false;
    const char *names[] = {"path", "live"};
    for (size_t list = 0; ok && list < 2; list++) {
        pc_ui_lasso_points *points = list ? &next.live : &next.path;
        yyjson_val *array = pc_json_get(v, names[list]);
        if (!yyjson_is_arr(array) || yyjson_arr_size(array) > LIMIT) {
            ok = false;
            break;
        }
        size_t i, count;
        yyjson_val *entry;
        yyjson_arr_foreach(array, i, count, entry)
        {
            double p[2];
            if (!yyjson_is_arr(entry) || yyjson_arr_size(entry) != 2 ||
                !pc_json_as_f64(yyjson_arr_get(entry, 0), &p[0]) ||
                !pc_json_as_f64(yyjson_arr_get(entry, 1), &p[1]) || !isfinite(p[0]) || !isfinite(p[1]) ||
                fabs(p[0]) > 1000000 || fabs(p[1]) > 1000000) {
                ok = false;
                break;
            }
            pc_status status = append(points, p, err);
            if (status != PC_OK) {
                pc_ui_lasso_fini(&next);
                return status;
            }
        }
    }
    yyjson_val *anchors = pc_json_get(v, "anchors");
    if (!yyjson_is_arr(anchors) || yyjson_arr_size(anchors) > next.path.n)
        ok = false;
    if (ok) {
        next.nanchors = next.anchor_capacity = yyjson_arr_size(anchors);
        next.anchors = next.nanchors ? pc_malloc_array(next.nanchors, sizeof *next.anchors) : NULL;
        if (next.nanchors && !next.anchors) {
            pc_ui_lasso_fini(&next);
            return pc_fail(err, PC_ERR_NOMEM, "saved lasso anchors");
        }
        for (size_t i = 0; i < next.nanchors; i++) {
            uint64_t n;
            if (!pc_json_as_u64(yyjson_arr_get(anchors, i), &n) || n >= next.path.n ||
                (i && n <= next.anchors[i - 1])) {
                ok = false;
                break;
            }
            next.anchors[i] = (size_t)n;
        }
    }
    if (next.active && (!next.path.n || !next.nanchors || next.anchors[0] != 0))
        ok = false;
    if (!ok) {
        pc_ui_lasso_fini(&next);
        return pc_fail(err, PC_ERR_INVALID, "invalid saved lasso");
    }
    if (next.path.n)
        memcpy(next.cursor, next.path.v[next.path.n - 1], sizeof next.cursor);
    pc_ui_lasso_fini(out);
    *out = next;
    return PC_OK;
}
