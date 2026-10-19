#include "canvas_ink.h"
#include "crop_ui.h"
#include "snap_ui.h"
#include "canvas_view.h"
#include "jobs_ui.h"
#include "context_ui.h"
#include "ui/crop_overlay.h"
#include "ui/crop_size.h"
#include "ui/sizing.h"
#include "engine/prefs.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "ui/menus.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static const struct crop_preset {
    const char *name, *label, *width, *height, *resolution;
} presets[] = {{"preset:4x5in", "4 x 5 in 300 ppi", "4 in", "5 in", "300"},
               {"preset:8.5x11in", "8.5 x 11 in 300 ppi", "8.5 in", "11 in", "300"},
               {"preset:1024x768px", "1024 x 768 px 92 ppi", "1024 px", "768 px", "92"},
               {"preset:1280x800px", "1280 x 800 px 113 ppi", "1280 px", "800 px", "113"},
               {"preset:1366x768px", "1366 x 768 px 135 ppi", "1366 px", "768 px", "135"},
               {"preset:4x6in", "4 x 6 in 300 ppi", "4 in", "6 in", "300"},
               {"preset:5x7in", "5 x 7 in 300 ppi", "5 in", "7 in", "300"},
               {"preset:8x10in", "8 x 10 in 300 ppi", "8 in", "10 in", "300"}};
static void front_image(pc_ui_shell_app *app);
static pc_status preset(pc_ui_shell_app *app, const char *name, pc_error *err)
{
    pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    if (!strcmp(name, "front")) {
        front_image(app);
        return PC_OK;
    }
    for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++)
        if (!strcmp(name, presets[i].name)) {
            pc_ui_tool_options next = {0};
            pc_status status = pc_ui_tool_options_clone(o, &next, err);
            if (status == PC_OK)
                status = pc_str_set_c(&next.crop_ratio, "whr", err);
            if (status == PC_OK)
                status = pc_str_set_c(&next.crop_width, presets[i].width, err);
            if (status == PC_OK)
                status = pc_str_set_c(&next.crop_height, presets[i].height, err);
            if (status == PC_OK)
                status = pc_str_set_c(&next.crop_resolution, presets[i].resolution, err);
            if (status == PC_OK)
                status = pc_str_set_c(&next.crop_resolution_unit, "px/in", err);
            if (status == PC_OK) {
                pc_ui_tool_options_fini(o);
                *o = next;
                next = (pc_ui_tool_options){0};
            }
            pc_ui_tool_options_fini(&next);
            return status;
        }
    const char *names[] = {"", "whr", "original", "1:1", "4:5", "5:7", "2:3", "16:9", "custom"};
    for (size_t i = 0; i < 9; i++)
        if (!strcmp(name, names[i]))
            return pc_str_set_c(&o->crop_ratio, name, err);
    return pc_fail(err, PC_ERR_INVALID, "unknown crop preset");
}
static double ratio(pc_ui_shell_app *app);
static pc_ui_crop_frame frame(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_frame result = {.present = ui->has_crop, .angle = ui->crop_angle};
    memcpy(result.rect, ui->crop_rect, sizeof result.rect);
    return result;
}
static void put_frame(pc_ui_shell_app *app, const pc_ui_crop_frame *f)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->has_crop = f->present;
    ui->crop_angle = f->angle;
    memcpy(ui->crop_rect, f->rect, sizeof f->rect);
}
static bool scope(pc_ui_shell_app *app)
{
    const pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    return r->scoped && state && pc_doc_arc_get(state->doc)->id == r->document &&
           state->revision == r->revision;
}
static void flags(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    ui->crop_editing = r->editing;
    ui->crop_can_undo = r->nundo != 0;
    ui->crop_can_redo = r->nredo != 0;
}
static pc_ui_crop_step snapshot(pc_ui_shell_app *app)
{
    const pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    return (pc_ui_crop_step){.frame = frame(app),
                             .view = view ? *view : (pc_ui_view){0},
                             .editing = r->editing,
                             .default_frame = r->default_frame};
}
static void restore(pc_ui_shell_app *app, const pc_ui_crop_step *step)
{
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    put_frame(app, &step->frame);
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (view)
        *view = step->view;
    r->editing = step->editing;
    r->default_frame = step->default_frame;
    flags(app);
}
static void push(pc_ui_crop_step steps[128], size_t *count, const pc_ui_crop_step *step)
{
    if (*count == 128) {
        memmove(steps, steps + 1, 127 * sizeof *steps);
        *count = 127;
    }
    steps[(*count)++] = *step;
}
static void settle(pc_ui_shell_app *app, const pc_ui_crop_step *before)
{
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    pc_ui_crop_frame after = frame(app);
    if (after.present != before->frame.present || after.angle != before->frame.angle ||
        memcmp(after.rect, before->frame.rect, sizeof after.rect)) {
        push(r->undo, &r->nundo, before);
        r->nredo = 0;
        r->editing = true;
        r->default_frame = false;
    } else {
        r->editing = before->editing;
        r->default_frame = before->default_frame;
    }
    flags(app);
    pc_ui_shell_app_changed(app);
}
static void upright(pc_ui_shell_app *app)
{
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (view)
        view->rotation = 0;
}
static void reset(pc_ui_shell_app *app)
{
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    memset(r, 0, sizeof *r);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    ui->has_crop = false;
    ui->crop_angle = 0;
    flags(app);
    upright(app);
}
static void sync_angle(pc_ui_shell_app *app, bool pivot)
{
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (!view)
        return;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_frame f = frame(app);
    float want = ui->tool_options.crop_shield.classic_mode ? 0 : (float)-f.angle;
    if (view->rotation == want)
        return;
    pc_ui_canvas_xform xf = {0};
    if (pivot && pc_ui_shell_app_canvas_mapping(app, &xf)) {
        xf.center[0] = view->center[0];
        xf.center[1] = view->center[1];
        xf.rotation = view->rotation;
        double cx = (f.rect[0] + f.rect[2]) / 2, cy = (f.rect[1] + f.rect[3]) / 2;
        float screen[2], after[2];
        if (pc_ui_canvas_to_screen(&xf, (float)cx, (float)cy, screen) == PC_OK) {
            xf.rotation = want;
            if (pc_ui_canvas_to_doc(&xf, screen[0], screen[1], after) == PC_OK) {
                view->center[0] += (float)cx - after[0];
                view->center[1] += (float)cy - after[1];
            }
        }
    }
    view->rotation = want;
}
void pc_ui_shell_crop_tick(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    if (ui->tool != PC_TOOL_CROP || ui->transform.active)
        return;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state) {
        reset(app);
        return;
    }
    bool fresh_scope = !scope(app);
    if (fresh_scope) {
        bool stale = r->scoped && r->editing;
        reset(app);
        if (stale)
            pc_ui_shell_report(app, "Crop", "The document changed; the pending crop was cancelled.", true);
        const pc_document *doc = pc_doc_arc_get(state->doc);
        pc_rect rect =
            doc->has_selection ? pc_surface_content_bounds(&doc->selection) : pc_document_bounds(doc);
        if (pc_rect_is_empty(rect))
            rect = pc_document_bounds(doc);
        pc_ui_crop_frame f = {.present = true, .rect = {rect.x0, rect.y0, rect.x1, rect.y1}};
        put_frame(app, &f);
        r->scoped = true;
        r->document = doc->id;
        r->revision = state->revision;
        r->default_frame = true;
    }
    double desired = ratio(app);
    if (!r->drag.active && !r->straightening && isfinite(desired) && desired >= 0 && desired <= 1000000 &&
        desired != r->last_ratio) {
        r->last_ratio = desired;
        if (desired > 0) {
            pc_ui_crop_step before = snapshot(app);
            pc_ui_crop_frame f = before.frame;
            double w = f.rect[2] - f.rect[0], h = f.rect[3] - f.rect[1], cx = (f.rect[0] + f.rect[2]) / 2,
                   cy = (f.rect[1] + f.rect[3]) / 2;
            if (w / h > desired)
                w = h * desired;
            else
                h = w / desired;
            if (w < 1) {
                w = 1;
                h = 1 / desired;
            }
            if (h < 1) {
                h = 1;
                w = desired;
            }
            f.rect[0] = cx - w / 2;
            f.rect[1] = cy - h / 2;
            f.rect[2] = cx + w / 2;
            f.rect[3] = cy + h / 2;
            if (pc_ui_crop_valid(&f)) {
                put_frame(app, &f);
                if (!fresh_scope)
                    settle(app, &before);
            }
        }
    }
    sync_angle(app, false);
    flags(app);
}
static double ratio(pc_ui_shell_app *app)
{
    const pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    const char *name = pc_str_c(&o->crop_ratio);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    if (!strcmp(name, "original") && doc)
        return (double)doc->size.width / fmax(1, doc->size.height);
    if (!strcmp(name, "custom") && o->crop_custom_ratio_width > 0 && o->crop_custom_ratio_height > 0)
        return o->crop_custom_ratio_width / o->crop_custom_ratio_height;
    if (!strcmp(name, "whr")) {
        double ppi = doc ? doc->resolution_dpi : 72, w, h;
        if (o->crop_resolution.len)
            (void)pc_ui_crop_resolution(pc_str_c(&o->crop_resolution), pc_str_c(&o->crop_resolution_unit),
                                        &ppi);
        if (pc_ui_crop_pixels(pc_str_c(&o->crop_width), ppi, &w) &&
            pc_ui_crop_pixels(pc_str_c(&o->crop_height), ppi, &h))
            return w / h;
        return 0;
    }
    const char *names[] = {"1:1", "4:5", "5:7", "2:3", "16:9"};
    const double values[] = {1, .8, 5. / 7, 2. / 3, 16. / 9};
    for (size_t i = 0; i < 5; i++)
        if (!strcmp(name, names[i]))
            return values[i];
    return 0;
}
static pc_status reply(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL, *rect = d ? yyjson_mut_arr(d) : NULL;
    bool ok = p && rect;
    for (size_t i = 0; ok && i < 4; i++)
        ok = yyjson_mut_arr_add_real(d, rect, ui->crop_rect[i]);
    ok = ok && yyjson_mut_obj_add_val(d, p, "cropRect", ui->has_crop ? rect : yyjson_mut_null(d)) &&
         yyjson_mut_obj_add_real(d, p, "cropAngle", ui->crop_angle) &&
         yyjson_mut_obj_add_bool(d, p, "cropEditing", ui->crop_editing);
    pc_status status = ok ? pc_json_value_from_mut(p, out, err) : pc_fail(err, PC_ERR_NOMEM, "crop reply");
    yyjson_mut_doc_free(d);
    return status;
}
static pc_status commit(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    if (!scope(app))
        return pc_fail(err, PC_ERR_INVALID, "the crop document changed");
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    pc_ui_crop_frame f = frame(app);
    if (!pc_ui_crop_valid(&f))
        return pc_fail(err, PC_ERR_INVALID, "invalid crop frame");
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_ui_tool_options *options = &ui->tool_options;
    bool output_size =
        !strcmp(pc_str_c(&options->crop_ratio), "whr") &&
        (options->crop_resolution.len || (options->crop_width.len && options->crop_height.len));
    if (r->default_frame && !doc->has_selection && !r->editing && !output_size && f.angle == 0 &&
        f.rect[0] == 0 && f.rect[1] == 0 && f.rect[2] == doc->size.width && f.rect[3] == doc->size.height) {
        reset(app);
        return reply(app, out, err);
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    bool ok = p && yyjson_mut_obj_add_sint(d, p, "x", (int64_t)round(f.rect[0])) &&
              yyjson_mut_obj_add_sint(d, p, "y", (int64_t)round(f.rect[1])) &&
              yyjson_mut_obj_add_uint(d, p, "width", (uint64_t)fmax(1, round(f.rect[2] - f.rect[0]))) &&
              yyjson_mut_obj_add_uint(d, p, "height", (uint64_t)fmax(1, round(f.rect[3] - f.rect[1]))) &&
              yyjson_mut_obj_add_real(d, p, "angle", f.angle) &&
              yyjson_mut_obj_add_bool(d, p, "deleteCroppedPixels", ui->tool_options.crop_delete);
    if (ok && !strcmp(pc_str_c(&ui->tool_options.crop_ratio), "whr")) {
        const pc_ui_tool_options *o = &ui->tool_options;
        double ppi = doc->resolution_dpi;
        if (o->crop_resolution.len) {
            if (!pc_ui_crop_resolution(pc_str_c(&o->crop_resolution), pc_str_c(&o->crop_resolution_unit),
                                       &ppi)) {
                yyjson_mut_doc_free(d);
                return pc_fail(err, PC_ERR_INVALID, "invalid crop resolution");
            }
            ok = yyjson_mut_obj_add_real(d, p, "resolution", ppi);
        }
        if (o->crop_width.len && o->crop_height.len) {
            double width, height;
            if (!pc_ui_crop_pixels(pc_str_c(&o->crop_width), ppi, &width) ||
                !pc_ui_crop_pixels(pc_str_c(&o->crop_height), ppi, &height) || width > 300000 ||
                height > 300000) {
                yyjson_mut_doc_free(d);
                return pc_fail(err, PC_ERR_INVALID, "invalid crop output size");
            }
            ok = ok && yyjson_mut_obj_add_uint(d, p, "targetWidth", (uint64_t)fmax(1, round(width))) &&
                 yyjson_mut_obj_add_uint(d, p, "targetHeight", (uint64_t)fmax(1, round(height)));
            pc_json_value interpolation = PC_JSON_VALUE_INIT;
            (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "general.imageInterpolation",
                                      &interpolation, NULL);
            ok = ok && yyjson_mut_obj_add_str(
                           d, p, "resample",
                           pc_ui_resample_preference(yyjson_mut_get_str(pc_json_value_root(&interpolation))));
            pc_json_value_fini(&interpolation);
        }
    }
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_status status = ok ? pc_json_write(p, 0, &text, err) : pc_fail(err, PC_ERR_NOMEM, "crop parameters");
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
    if (status == PC_OK)
        status = pc_engine_session_execute(pc_ui_shell_app_session(app), "image.crop",
                                           yyjson_doc_get_root(parsed), out, err);
    if (status == PC_OK) {
        reset(app);
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        if (view)
            view->fit_pending = true;
    }
    pc_buf_fini(&text);
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(d);
    return status;
}
pc_status pc_ui_shell_crop_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                   pc_json_value *out, pc_error *err)
{
    (void)params;
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->tool != PC_TOOL_CROP || ui->transform.active)
        return PC_ERR_NOT_FOUND;
    pc_ui_shell_crop_tick(app);
    if (!strcmp(id, "ui.crop.preset")) {
        size_t length = 0;
        const char *name = pc_json_as_str(pc_json_get(params, "name"), &length);
        if (!name || length > 64 || strlen(name) != length)
            return pc_fail(err, PC_ERR_INVALID, "invalid crop preset");
        pc_status status = preset(app, name, err);
        if (status != PC_OK)
            return status;
        pc_ui_shell_crop_tick(app);
        return reply(app, out, err);
    }
    if (!strcmp(id, "ui.crop.commit"))
        return commit(app, out, err);
    if (!strcmp(id, "ui.crop.cancel")) {
        reset(app);
        return reply(app, out, err);
    }
    if (!strcmp(id, "ui.crop.straighten")) {
        r->straighten = !r->straighten;
        return reply(app, out, err);
    }
    if (!r->scoped)
        return PC_ERR_NOT_FOUND;
    if (!strcmp(id, "edit.undo") || !strcmp(id, "edit.redo")) {
        if (!r->editing && !r->nundo && !r->nredo)
            return PC_ERR_NOT_FOUND;
        bool redo = !strcmp(id, "edit.redo");
        size_t *from_count = redo ? &r->nredo : &r->nundo, *to_count = redo ? &r->nundo : &r->nredo;
        pc_ui_crop_step *from = redo ? r->redo : r->undo, *to = redo ? r->undo : r->redo;
        if (*from_count && !r->drag.active && !r->straightening) {
            pc_ui_crop_step before = snapshot(app);
            push(to, to_count, &before);
            restore(app, &from[--*from_count]);
            flags(app);
        }
        return reply(app, out, err);
    }
    if (r->editing) {
        pc_ui_tool tool;
        bool selecting = pc_ui_tool_from_select_id(id, &tool) && tool != PC_TOOL_CROP;
        const pc_engine_command *command = pc_engine_command_find(id);
        bool editing = command && command->journal && strncmp(id, "tools.", 6) && strncmp(id, "prefs.", 6) &&
                       strncmp(id, "view.", 5) && strncmp(id, "jobs.", 5);
        if (!ui->n_dialogs && (selecting || editing || !strcmp(id, "edit.freeTransform") ||
                               !strncmp(id, "window.document.", 16))) {
            pc_json_value ignored = PC_JSON_VALUE_INIT;
            pc_status status = commit(app, &ignored, err);
            pc_json_value_fini(&ignored);
            if (status != PC_OK)
                return status;
        }
    }
    return PC_ERR_NOT_FOUND;
}
pc_status pc_ui_shell_crop_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                   unsigned mods, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    if (ui->tool != PC_TOOL_CROP || ui->transform.active)
        return PC_ERR_NOT_FOUND;
    pc_ui_shell_crop_tick(app);
    if (!scope(app) || ui->n_dialogs || pc_ui_shell_jobs_locked(app))
        return pc_fail(err, PC_ERR_INVALID, "crop is unavailable");
    if (kind == PC_UI_POINTER_CANCEL) {
        if (r->drag.active || r->straightening)
            restore(app, &r->press);
        r->drag.active = r->straightening = false;
        return reply(app, out, err);
    }
    if (!isfinite(point.x) || !isfinite(point.y) || fabsf(point.x) > 1000000 || fabsf(point.y) > 1000000) {
        if (kind == PC_UI_POINTER_UP) {
            if (r->drag.active || r->straightening)
                restore(app, &r->press);
            r->drag.active = r->straightening = false;
        }
        return pc_fail(err, PC_ERR_INVALID, "invalid crop pointer");
    }
    double p[2] = {point.x, point.y};
    pc_ui_canvas_xform xf = {0};
    bool mapping = pc_ui_shell_app_canvas_mapping(app, &xf);
    bool classic = ui->tool_options.crop_shield.classic_mode;
    if (r->drag.active && !classic && r->has_press && mapping) {
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        if (view) {
            xf.center[0] = view->center[0];
            xf.center[1] = view->center[1];
            xf.rotation = view->rotation;
            float screen[2], before[2];
            if (pc_ui_canvas_to_screen(&xf, point.x, point.y, screen) == PC_OK &&
                pc_ui_canvas_to_doc(&r->press_xf, screen[0], screen[1], before) == PC_OK) {
                p[0] = before[0];
                p[1] = before[1];
            }
        }
    }
    pc_ui_crop_frame f = frame(app);
    if (kind == PC_UI_POINTER_DOWN) {
        r->press = snapshot(app);
        r->editing = true;
        r->press_xf = xf;
        r->has_press = mapping;
        r->straightening = r->straighten || (mods & PC_UI_MOD_COMMAND) != 0;
        if (r->straightening) {
            memcpy(r->line[0], p, sizeof p);
            memcpy(r->line[1], p, sizeof p);
            r->editing = true;
        } else if (!pc_ui_crop_begin(&r->drag, &f, p, mapping ? 8 / fmax(.01, xf.zoom) : 8, r->default_frame,
                                     classic))
            return pc_fail(err, PC_ERR_INVALID, "invalid crop frame");
    } else if (r->straightening) {
        memcpy(r->line[1], p, sizeof p);
        if (kind == PC_UI_POINTER_UP) {
            double dx = p[0] - r->line[0][0], dy = p[1] - r->line[0][1];
            if (hypot(dx, dy) >= (mapping ? 2 / fmax(.01, xf.zoom) : 2)) {
                f.angle = remainder(atan2(dy, dx) * 180 / 3.14159265358979323846, 90);
                const pc_document *doc =
                    pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc);
                double w = f.rect[2] - f.rect[0], h = f.rect[3] - f.rect[1],
                       a = f.angle * 3.14159265358979323846 / 180, co = fabs(cos(a)), si = fabs(sin(a)),
                       k = fmin(doc->size.width / (w * co + h * si), doc->size.height / (w * si + h * co));
                double cx = doc->size.width / 2., cy = doc->size.height / 2.;
                f.rect[0] = cx - w * k / 2;
                f.rect[1] = cy - h * k / 2;
                f.rect[2] = cx + w * k / 2;
                f.rect[3] = cy + h * k / 2;
                if (pc_ui_crop_valid(&f))
                    put_frame(app, &f);
                settle(app, &r->press);
                sync_angle(app, true);
            } else
                restore(app, &r->press);
            r->straightening = r->straighten = false;
        }
    } else if (r->drag.active) {
        bool alt = (mods & PC_UI_MOD_ALT) != 0;
        bool handle = r->drag.transform.hit.part == PC_TRANSFORM_CORNER ||
                      r->drag.transform.hit.part == PC_TRANSFORM_EDGE;
        if (!classic && ui->tool_options.crop_shield.auto_center_preview && handle && !alt) {
            p[0] = r->drag.transform.start[0] + 2 * (p[0] - r->drag.transform.start[0]);
            p[1] = r->drag.transform.start[1] + 2 * (p[1] - r->drag.transform.start[1]);
        }
        if (handle)
            pc_ui_shell_snap_crop(app, &f, &r->drag, p, ratio(app), (mods & PC_UI_MOD_SHIFT) != 0, alt,
                                  r->space, mods, mapping ? xf.zoom : 1);
        if (pc_ui_crop_motion(&f, &r->drag, p, ratio(app), (mods & PC_UI_MOD_SHIFT) != 0, alt, r->space)) {
            put_frame(app, &f);
            r->editing = true;
            pc_ui_view *view = pc_ui_canvas_active_view(app);
            if (!classic && view &&
                (r->drag.image || (handle && ui->tool_options.crop_shield.auto_center_preview))) {
                view->center[0] =
                    r->press.view.center[0] +
                    (float)((f.rect[0] + f.rect[2] - r->press.frame.rect[0] - r->press.frame.rect[2]) / 2);
                view->center[1] =
                    r->press.view.center[1] +
                    (float)((f.rect[1] + f.rect[3] - r->press.frame.rect[1] - r->press.frame.rect[3]) / 2);
            }
            sync_angle(app, true);
        }
        if (kind == PC_UI_POINTER_UP) {
            r->drag.active = false;
            settle(app, &r->press);
        }
    }
    flags(app);
    return reply(app, out, err);
}
static void front_image(pc_ui_shell_app *app)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state)
        return;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    char text[65];
    (void)pc_str_set_c(&o->crop_ratio, "whr", NULL);
    (void)snprintf(text, sizeof text, "%u px", doc->size.width);
    (void)pc_str_set_c(&o->crop_width, text, NULL);
    (void)snprintf(text, sizeof text, "%u px", doc->size.height);
    (void)pc_str_set_c(&o->crop_height, text, NULL);
    (void)snprintf(text, sizeof text, "%.8g", (double)doc->resolution_dpi);
    (void)pc_str_set_c(&o->crop_resolution, text, NULL);
    (void)pc_str_set_c(&o->crop_resolution_unit, "px/in", NULL);
}
bool pc_ui_shell_crop_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    if (ui->tool != PC_TOOL_CROP || ui->transform.active)
        return false;
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
        event->key.key == SDLK_SPACE) {
        r->space = event->type == SDL_EVENT_KEY_DOWN;
        return false;
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        if (scope(app) && (r->drag.active || r->straightening))
            restore(app, &r->press);
        r->drag.active = r->straightening = r->space = false;
        return false;
    }
    if (event->type != SDL_EVENT_KEY_DOWN || ui->n_dialogs || ui->context_menu.open ||
        pc_ui_menu_nav_is_open(pc_ui_shell_app_nav(app)) || pc_ui_shell_keyboard_captured(app))
        return false;
    pc_ui_shell_crop_tick(app);
    if (!scope(app))
        return false;
    if (event->key.key == SDLK_ESCAPE || event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) {
        if (!event->key.repeat) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status status = pc_ui_shell_crop_command(
                app, event->key.key == SDLK_ESCAPE ? "ui.crop.cancel" : "ui.crop.commit", NULL, &out, &err);
            if (status != PC_OK)
                pc_ui_shell_report(app, "Crop", err.msg, true);
            pc_json_value_fini(&out);
        }
        return true;
    }
    if (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT))
        return false;
    if (event->key.key == SDLK_O && !event->key.repeat) {
        if (event->key.mod & SDL_KMOD_SHIFT)
            ui->tool_options.crop_overlay_orientation =
                (uint8_t)((ui->tool_options.crop_overlay_orientation + 1) %
                          pc_ui_crop_overlay_orientations(ui->tool_options.crop_overlay));
        else
            ui->tool_options.crop_overlay =
                (pc_ui_crop_overlay)(((unsigned)ui->tool_options.crop_overlay + 1) % 6);
        return true;
    }
    if (event->key.key == SDLK_P && !event->key.repeat) {
        ui->tool_options.crop_shield.classic_mode = !ui->tool_options.crop_shield.classic_mode;
        sync_angle(app, true);
        return true;
    }
    if (event->key.key == SDLK_I && !event->key.repeat) {
        front_image(app);
        return true;
    }
    pc_ui_crop_step before = snapshot(app);
    pc_ui_crop_frame f = before.frame;
    if (event->key.key == SDLK_X && !event->key.repeat) {
        if (pc_ui_crop_swap(&f)) {
            put_frame(app, &f);
            pc_str swap = ui->tool_options.crop_width;
            ui->tool_options.crop_width = ui->tool_options.crop_height;
            ui->tool_options.crop_height = swap;
            float ratio_width = ui->tool_options.crop_custom_ratio_width;
            ui->tool_options.crop_custom_ratio_width = ui->tool_options.crop_custom_ratio_height;
            ui->tool_options.crop_custom_ratio_height = ratio_width;
            const char *ratio_name = pc_str_c(&ui->tool_options.crop_ratio);
            if (strcmp(ratio_name, "") && strcmp(ratio_name, "whr") && strcmp(ratio_name, "custom")) {
                ui->tool_options.crop_custom_ratio_width = (float)(f.rect[2] - f.rect[0]);
                ui->tool_options.crop_custom_ratio_height = (float)(f.rect[3] - f.rect[1]);
                (void)pc_str_set_c(&ui->tool_options.crop_ratio, "custom", NULL);
            }
            settle(app, &before);
        }
        return true;
    }
    double dx = event->key.key == SDLK_LEFT    ? -1
                : event->key.key == SDLK_RIGHT ? 1
                                               : 0,
           dy = event->key.key == SDLK_UP     ? -1
                : event->key.key == SDLK_DOWN ? 1
                                              : 0;
    if (dx == 0 && dy == 0)
        return false;
    if (r->drag.active || r->straightening)
        return true;
    double step = (event->key.mod & SDL_KMOD_SHIFT) ? 10 : 1;
    if (!ui->tool_options.crop_shield.classic_mode)
        step = -step;
    for (size_t i = 0; i < 4; i++)
        f.rect[i] += (i % 2 ? dy : dx) * step;
    if (pc_ui_crop_valid(&f)) {
        put_frame(app, &f);
        if (!ui->tool_options.crop_shield.classic_mode) {
            pc_ui_view *view = pc_ui_canvas_active_view(app);
            if (view) {
                view->center[0] += (float)(dx * step);
                view->center[1] += (float)(dy * step);
            }
        }
        settle(app, &before);
    }
    return true;
}
bool pc_ui_shell_crop_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                             float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    if (ui->tool != PC_TOOL_CROP || ui->transform.active || ui->n_dialogs || pc_ui_shell_jobs_locked(app))
        return false;
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!motion && event->type != SDL_EVENT_MOUSE_BUTTON_DOWN && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!motion && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (motion ? event->motion.x : event->button.x) * factor,
          y = (motion ? event->motion.y : event->button.y) * factor;
    if (!r->drag.active && !r->straightening &&
        (!down || r->space || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float point[2];
    if (pc_ui_canvas_to_doc(xf, x, y, point) != PC_OK)
        return true;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status =
        down && event->button.clicks >= 2
            ? commit(app, &out, &err)
            : pc_ui_shell_app_pointer(app,
                                      down     ? PC_UI_POINTER_DOWN
                                      : motion ? PC_UI_POINTER_MOVE
                                               : PC_UI_POINTER_UP,
                                      (pc_ui_tool_point){.x = point[0], .y = point[1], .pressure = 1},
                                      pc_ui_shell_input_modifiers(app), &out, &err);
    if (status != PC_OK)
        pc_ui_shell_report(app, "Crop", err.msg, true);
    pc_json_value_fini(&out);
    return true;
}
static void field(pc_ui_shell_app *app, const char *id, pc_str *value, char buffer[65], bool *active)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!*active)
        (void)snprintf(buffer, 65, "%s", pc_str_c(value));
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    nk_flags flags = nk_edit_string_zero_terminated(
        ctx, (nk_flags)NK_EDIT_FIELD | (nk_flags)NK_EDIT_SIG_ENTER, buffer, 65, nk_filter_default);
    *active = (flags & NK_EDIT_ACTIVE) != 0;
    if ((flags & NK_EDIT_COMMITED) || (!*active && strcmp(buffer, pc_str_c(value))))
        (void)pc_str_set_c(value, buffer, NULL);
}
void pc_ui_shell_crop_options(pc_ui_shell_app *app)
{
    pc_ui_shell_crop_tick(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    pc_ui_tool_options *o = &ui->tool_options;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 12);
    nk_layout_row_push(ctx, 40);
    nk_label(ctx, pc_i18n_t("Crop"), NK_TEXT_LEFT);
    const char *keys[] = {"", "whr", "original", "1:1", "4:5", "5:7", "2:3", "16:9", "custom"};
    const char *labels[] = {
        "Ratio", "W x H x Resolution", "Original Ratio", "1 : 1", "4 : 5", "5 : 7", "2 : 3", "16 : 9",
        "Custom"};
    int selected = 0;
    for (size_t i = 0; i < 9; i++)
        if (pc_str_eq_c(&o->crop_ratio, keys[i]))
            selected = (int)i;
    nk_layout_row_push(ctx, 165);
    pc_ui_shell_widget_mapping(app, "crop.preset", nk_widget_bounds(ctx));
    const char *shown = pc_i18n_t(labels[selected]);
    if (nk_combo_begin_label(ctx, shown, nk_vec2(280, 430))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < 18; i++) {
            const char *key = i < 9 ? keys[i] : i < 17 ? presets[i - 9].name : "front";
            const char *label = i < 9 ? labels[i] : i < 17 ? presets[i - 9].label : "Front Image";
            char mapping[100];
            (void)snprintf(mapping, sizeof mapping, "crop.preset.%s", key);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            if (nk_combo_item_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT)) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
                if (p && yyjson_mut_obj_add_str(d, p, "name", key))
                    pc_ui_shell_queue(app, "ui.crop.preset", p);
                yyjson_mut_doc_free(d);
            }
        }
        nk_combo_end(ctx);
    }
    bool whr = pc_str_eq_c(&o->crop_ratio, "whr");
    if (whr) {
        nk_layout_row_push(ctx, 95);
        field(app, "crop.width", &o->crop_width, r->width, &r->text_active[0]);
        nk_layout_row_push(ctx, 18);
        nk_label(ctx, "x", NK_TEXT_CENTERED);
        nk_layout_row_push(ctx, 95);
        field(app, "crop.height", &o->crop_height, r->height, &r->text_active[1]);
    } else {
        double current = ratio(app), w = current > 0 ? current : 0, h = current > 0 ? 1 : 0;
        if (pc_str_eq_c(&o->crop_ratio, "custom")) {
            w = o->crop_custom_ratio_width;
            h = o->crop_custom_ratio_height;
        }
        double before_w = w, before_h = h;
        nk_layout_row_push(ctx, 95);
        pc_ui_shell_widget_mapping(app, "crop.ratio.width", nk_widget_bounds(ctx));
        nk_property_double(ctx, "W", 0, &w, 1000000, .1, .1f);
        nk_layout_row_push(ctx, 18);
        nk_label(ctx, ":", NK_TEXT_CENTERED);
        nk_layout_row_push(ctx, 95);
        pc_ui_shell_widget_mapping(app, "crop.ratio.height", nk_widget_bounds(ctx));
        nk_property_double(ctx, "H", 0, &h, 1000000, .1, .1f);
        if (w != before_w || h != before_h) {
            o->crop_custom_ratio_width = (float)w;
            o->crop_custom_ratio_height = (float)h;
            (void)pc_str_set_c(&o->crop_ratio, "custom", NULL);
        }
    }
    if (!whr)
        nk_widget_disable_begin(ctx);
    nk_layout_row_push(ctx, 80);
    field(app, "crop.resolution", &o->crop_resolution, r->resolution, &r->text_active[2]);
    nk_layout_row_push(ctx, 80);
    const char *units[] = {"px/in", "px/cm"};
    int unit = pc_str_eq_c(&o->crop_resolution_unit, "px/cm") ? 1 : 0;
    int new_unit = nk_combo(ctx, units, 2, unit, 24, nk_vec2(120, 90));
    if (new_unit >= 0 && new_unit < 2 && new_unit != unit)
        (void)pc_str_set_c(&o->crop_resolution_unit, units[new_unit], NULL);
    if (!whr)
        nk_widget_disable_end(ctx);
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, pc_i18n_t("Front Image")))
        front_image(app);
    nk_layout_row_push(ctx, 55);
    if (nk_button_label(ctx, pc_i18n_t("Clear"))) {
        (void)pc_str_set_c(&o->crop_width, "", NULL);
        (void)pc_str_set_c(&o->crop_height, "", NULL);
        (void)pc_str_set_c(&o->crop_resolution, "", NULL);
        if (!whr)
            (void)pc_str_set_c(&o->crop_ratio, "", NULL);
        o->crop_custom_ratio_width = o->crop_custom_ratio_height = 0;
    }
    nk_layout_row_push(ctx, 55);
    pc_ui_shell_widget_mapping(app, "ui.crop.commit", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Apply")))
        pc_ui_shell_queue(app, "ui.crop.commit", NULL);
    nk_layout_row_push(ctx, 60);
    pc_ui_shell_widget_mapping(app, "ui.crop.cancel", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Cancel")))
        pc_ui_shell_queue(app, "ui.crop.cancel", NULL);
    nk_layout_row_end(ctx);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 8);
    nk_layout_row_push(ctx, 190);
    nk_bool remove = o->crop_delete;
    if (nk_checkbox_label(ctx, pc_i18n_t("Delete Cropped Pixels"), &remove))
        o->crop_delete = remove != 0;
    nk_layout_row_push(ctx, 125);
    nk_bool classic = o->crop_shield.classic_mode;
    if (nk_checkbox_label(ctx, pc_i18n_t("Classic Mode"), &classic)) {
        o->crop_shield.classic_mode = classic != 0;
        sync_angle(app, true);
    }
    nk_layout_row_push(ctx, 90);
    pc_ui_shell_widget_mapping(app, "ui.crop.straighten", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Straighten")))
        r->straighten = !r->straighten;
    nk_layout_row_push(ctx, 155);
    const char *overlays[] = {"Rule of Thirds", "Grid",         "Diagonal",
                              "Triangle",       "Golden Ratio", "Golden Spiral"};
    int overlay = nk_combo(ctx, overlays, 6, (int)o->crop_overlay, 24, nk_vec2(220, 220));
    if (overlay >= 0 && overlay < 6)
        o->crop_overlay = (pc_ui_crop_overlay)overlay;
    nk_layout_row_push(ctx, 180);
    const char *shows[] = {"Auto Show Overlay", "Always Show Overlay", "Never Show Overlay"};
    int show = nk_combo(ctx, shows, 3, (int)o->crop_overlay_show, 24, nk_vec2(230, 130));
    if (show >= 0 && show < 3)
        o->crop_overlay_show = (pc_ui_overlay_show)show;
    nk_layout_row_push(ctx, 80);
    nk_bool shield = o->crop_shield.enabled;
    if (nk_checkbox_label(ctx, pc_i18n_t("Shield"), &shield))
        o->crop_shield.enabled = shield != 0;
    nk_layout_row_push(ctx, 130);
    double opacity = o->crop_shield.opacity;
    nk_property_double(ctx, pc_i18n_t("Opacity"), 0, &opacity, 100, 1, 1);
    o->crop_shield.opacity = (float)opacity;
    nk_layout_row_push(ctx, 85);
    if (nk_combo_begin_label(ctx, pc_i18n_t("Settings"), nk_vec2(280, 340))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_bool center = o->crop_shield.auto_center_preview, area = o->crop_shield.show_cropped_area,
                adjust = o->crop_shield.auto_adjust;
        if (nk_checkbox_label(ctx, pc_i18n_t("Auto Center Preview"), &center))
            o->crop_shield.auto_center_preview = center != 0;
        if (nk_checkbox_label(ctx, pc_i18n_t("Show Cropped Area"), &area))
            o->crop_shield.show_cropped_area = area != 0;
        if (nk_checkbox_label(ctx, pc_i18n_t("Auto Adjust Opacity"), &adjust))
            o->crop_shield.auto_adjust = adjust != 0;
        nk_bool custom = o->crop_shield.custom;
        if (nk_checkbox_label(ctx, pc_i18n_t("Custom Color"), &custom))
            o->crop_shield.custom = custom != 0;
        for (size_t i = 0; i < 3; i++) {
            const char *names[] = {"R", "G", "B"};
            int component = o->crop_shield.custom_color[i];
            nk_property_int(ctx, names[i], 0, &component, 255, 1, 1);
            o->crop_shield.custom_color[i] = (uint8_t)component;
        }
        nk_combo_end(ctx);
    }
    nk_layout_row_end(ctx);
}
/* Disjoint clipping against the four half-planes fills the outside of the rotated frame once,
 * so a translucent shield has uniform opacity even in the corners. */
static size_t clip(const float input[16][2], size_t count, float output[16][2], const float a[2],
                   const float b[2], float sign, bool inside)
{
    size_t n = 0;
    if (!count)
        return 0;
    for (size_t i = 0; i < count; i++) {
        const float *p = input[i], *q = input[(i + 1) % count];
        float dp = ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])) * sign,
              dq = ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0])) * sign;
        bool keep_p = inside ? dp >= 0 : dp <= 0, keep_q = inside ? dq >= 0 : dq <= 0;
        if (keep_p && n < 16) {
            output[n][0] = p[0];
            output[n++][1] = p[1];
        }
        if (keep_p != keep_q && n < 16) {
            float t = dp / (dp - dq);
            output[n][0] = p[0] + (q[0] - p[0]) * t;
            output[n++][1] = p[1] + (q[1] - p[1]) * t;
        }
    }
    return n;
}
static void mapped(const float q[4][2], float u, float v, float out[2])
{
    out[0] = q[0][0] + (q[1][0] - q[0][0]) * u + (q[3][0] - q[0][0]) * v;
    out[1] = q[0][1] + (q[1][1] - q[0][1]) * u + (q[3][1] - q[0][1]) * v;
}
void pc_ui_shell_crop_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_crop_runtime *r = pc_ui_shell_crop_runtime(app);
    if (ui->tool != PC_TOOL_CROP || ui->transform.active || !scope(app))
        return;
    pc_ui_crop_frame f = frame(app);
    if (!pc_ui_crop_valid(&f))
        return;
    pc_ui_transform_frame transform;
    pc_ui_crop_quad(&f, &transform);
    float points[4][2];
    for (size_t i = 0; i < 4; i++)
        if (pc_ui_canvas_to_screen(xf, (float)transform.quad[i][0], (float)transform.quad[i][1], points[i]) !=
            PC_OK)
            return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    struct nk_rect previous = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    pc_ui_color shield;
    if (pc_ui_crop_shield_fill(&ui->tool_options.crop_shield, pc_ui_shell_canvas_color(app),
                               r->drag.active || r->straightening, &shield)) {
        float polygon[16][2] = {{xf->x, xf->y},
                                {xf->x + xf->width, xf->y},
                                {xf->x + xf->width, xf->y + xf->height},
                                {xf->x, xf->y + xf->height}};
        size_t count = 4;
        float sign = ((points[1][0] - points[0][0]) * (points[2][1] - points[1][1]) -
                      (points[1][1] - points[0][1]) * (points[2][0] - points[1][0])) < 0
                         ? -1
                         : 1;
        for (size_t i = 0; i < 4 && count; i++) {
            float outside[16][2], inside[16][2];
            size_t n = clip((const float (*)[2])polygon, count, outside, points[i], points[(i + 1) % 4], sign,
                            false);
            if (n >= 3)
                nk_fill_polygon(canvas, &outside[0][0], (int)n, pc_ui_nk_color(shield));
            count =
                clip((const float (*)[2])polygon, count, inside, points[i], points[(i + 1) % 4], sign, true);
            memcpy(polygon, inside, count * sizeof *polygon);
        }
    }
    struct nk_color accent = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent_border);
    bool overlay =
        ui->tool_options.crop_overlay_show == PC_UI_OVERLAY_ALWAYS ||
        (ui->tool_options.crop_overlay_show == PC_UI_OVERLAY_AUTO && (r->drag.active || r->straightening));
    if (overlay) {
        float lines[PC_UI_CROP_GUIDE_MAX][4];
        size_t count =
            pc_ui_crop_guides(ui->tool_options.crop_overlay, ui->tool_options.crop_overlay_orientation,
                              hypotf(points[1][0] - points[0][0], points[1][1] - points[0][1]),
                              hypotf(points[3][0] - points[0][0], points[3][1] - points[0][1]), lines);
        for (size_t i = 0; i < count; i++) {
            float a[2], b[2];
            mapped((const float (*)[2])points, lines[i][0], lines[i][1], a);
            mapped((const float (*)[2])points, lines[i][2], lines[i][3], b);
            pc_ui_canvas_ink_line(canvas, xf, a[0], a[1], b[0], b[1], 1, accent);
        }
    }
    for (size_t i = 0; i < 4; i++) {
        size_t next = (i + 1) % 4;
        pc_ui_canvas_ink_line(canvas, xf, points[i][0], points[i][1], points[next][0], points[next][1], 1,
                              accent);
        float handles[2][2] = {{points[i][0], points[i][1]},
                               {(points[i][0] + points[next][0]) / 2, (points[i][1] + points[next][1]) / 2}};
        for (size_t k = 0; k < 2; k++) {
            if (!pc_ui_canvas_ink_visible(xf, handles[k][0], handles[k][1], 8))
                continue;
            struct nk_rect rect = nk_rect(handles[k][0] - 3, handles[k][1] - 3, 6, 6);
            nk_fill_rect(canvas, rect, 0, accent);
            char id[64];
            (void)snprintf(id, sizeof id, "crop.%s.%zu", k ? "edge" : "corner", i);
            pc_ui_shell_widget_mapping(app, id, nk_rect(rect.x - 5, rect.y - 5, 16, 16));
        }
    }
    if (r->straightening) {
        float a[2], b[2];
        if (pc_ui_canvas_to_screen(xf, (float)r->line[0][0], (float)r->line[0][1], a) == PC_OK &&
            pc_ui_canvas_to_screen(xf, (float)r->line[1][0], (float)r->line[1][1], b) == PC_OK)
            pc_ui_canvas_ink_line(canvas, xf, a[0], a[1], b[0], b[1], 2, accent);
    }
    if (r->drag.active || r->straightening) {
        char text[100];
        (void)snprintf(text, sizeof text, "%.0f × %.0f px   %.1f°", f.rect[2] - f.rect[0],
                       f.rect[3] - f.rect[1], ui->tool_options.crop_shield.classic_mode ? f.angle : -f.angle);
        struct nk_rect label = nk_rect(ctx->input.mouse.pos.x + 18, ctx->input.mouse.pos.y + 18, 210, 24);
        nk_fill_rect(canvas, label, 3, pc_ui_nk_color(pc_ui_shell_app_tokens(app)->card));
        nk_draw_text(canvas, label, text, (int)strlen(text), ctx->style.font,
                     pc_ui_nk_color(pc_ui_shell_app_tokens(app)->card),
                     pc_ui_nk_color(pc_ui_shell_app_tokens(app)->text));
    }
    nk_push_scissor(canvas, previous);
}
