#include "ui/move_mods.h"
#include "engine/floating.h"
#include "canvas_ink.h"
#include "snap_ui.h"
#include "crop_ui.h"
#include "canvas_view.h"
#include "engine/prefs.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static bool point_tool(pc_ui_tool tool)
{
    switch (tool) {
    case PC_TOOL_RECT_MARQUEE:
    case PC_TOOL_ELLIPSE_MARQUEE:
    case PC_TOOL_SINGLE_ROW_MARQUEE:
    case PC_TOOL_SINGLE_COLUMN_MARQUEE:
    case PC_TOOL_OBJECT_SELECTION:
    case PC_TOOL_PEN:
    case PC_TOOL_CURVATURE_PEN:
    case PC_TOOL_RECTANGLE:
    case PC_TOOL_ELLIPSE_SHAPE:
    case PC_TOOL_TRIANGLE:
    case PC_TOOL_POLYGON:
    case PC_TOOL_LINE:
    case PC_TOOL_CROP:
    case PC_TOOL_TYPE:
    case PC_TOOL_VERTICAL_TYPE:
    case PC_TOOL_HORIZONTAL_TYPE_MASK:
    case PC_TOOL_VERTICAL_TYPE_MASK:
        return true;
    default:
        return false;
    }
}
static void begin(pc_ui_shell_app *app, const double p[2], unsigned handoff)
{
    pc_ui_snap *s = pc_ui_shell_snap_runtime(app);
    pc_ui_snap_fini(s);
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || ui->n_dialogs)
        return;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer_id *exclude = NULL;
    size_t nexclude = 0;
    double rect[4] = {0};
    bool moving = false, smart = false, selection = false, preserve_press = false;
    if (ui->transform.active) {
        if (ui->transform.mode == PC_TRANSFORM_DISTORT)
            return;
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        double zoom = view ? view->zoom / pc_ui_shell_app_ui_scale(app) : 1;
        pc_ui_transform_hit hit = pc_ui_transform_hit_test(&ui->transform.frame, p, 12 / fmax(.01, zoom));
        if (hit.part == PC_TRANSFORM_INSIDE) {
            rect[0] = rect[1] = INFINITY;
            rect[2] = rect[3] = -INFINITY;
            for (size_t i = 0; i < 4; i++) {
                rect[0] = fmin(rect[0], ui->transform.frame.quad[i][0]);
                rect[1] = fmin(rect[1], ui->transform.frame.quad[i][1]);
                rect[2] = fmax(rect[2], ui->transform.frame.quad[i][0]);
                rect[3] = fmax(rect[3], ui->transform.frame.quad[i][1]);
            }
            moving = true;
        } else if (hit.part != PC_TRANSFORM_CORNER && hit.part != PC_TRANSFORM_EDGE)
            return;
        exclude = &ui->transform.layer;
        nexclude = 1;
    } else if (ui->tool == PC_TOOL_MOVE || handoff) {
        if ((handoff == 1 || handoff == 2 ||
             (!handoff && pc_engine_floating_can_move(pc_ui_shell_app_session(app)))) &&
            doc->has_selection && !pc_rect_is_empty(pc_surface_content_bounds(&doc->selection))) {
            pc_rect r = pc_surface_content_bounds(&doc->selection);
            int32_t offset[2] = {0, 0};
            (void)pc_engine_floating_offset(pc_ui_shell_app_session(app), offset);
            rect[0] = (double)r.x0 + offset[0];
            rect[1] = (double)r.y0 + offset[1];
            rect[2] = (double)r.x1 + offset[0];
            rect[3] = (double)r.y1 + offset[1];
            moving = selection = true;
        } else {
            for (size_t i = 0; i < state->nselected; i++) {
                double r[4];
                if (pc_snap_layer_rect(doc, state->selected_layers[i], r)) {
                    for (size_t j = 0; j < 4; j++)
                        rect[j] = !moving ? r[j] : j < 2 ? fmin(rect[j], r[j]) : fmax(rect[j], r[j]);
                    moving = true;
                }
            }
        }
        if (!moving)
            return;
        exclude = handoff == 1 ? NULL : state->selected_layers;
        nexclude = handoff == 1 ? 0 : state->nselected;
        smart = ui->view.extras && ui->view.show[4];
    } else if (ui->tool == PC_TOOL_CROP) {
        pc_ui_shell_crop_tick(app);
        pc_ui_crop_runtime *crop = pc_ui_shell_crop_runtime(app);
        pc_ui_crop_frame f = {.present = ui->has_crop, .angle = ui->crop_angle};
        memcpy(f.rect, ui->crop_rect, sizeof f.rect);
        pc_ui_transform_frame q;
        pc_ui_crop_quad(&f, &q);
        pc_ui_view *view = pc_ui_canvas_active_view(app);
        double zoom = view ? view->zoom / pc_ui_shell_app_ui_scale(app) : 1;
        pc_ui_transform_hit hit = pc_ui_transform_hit_test(&q, p, 8 / fmax(.01, zoom));
        if (hit.part == PC_TRANSFORM_PIVOT)
            hit.part = PC_TRANSFORM_INSIDE;
        if (hit.part == PC_TRANSFORM_INSIDE && crop->default_frame) {
            /* The untouched box draws a new frame. */
        } else if (hit.part == PC_TRANSFORM_INSIDE && f.angle == 0 &&
                   ui->tool_options.crop_shield.classic_mode) {
            memcpy(rect, f.rect, sizeof rect);
            moving = true;
        } else if (hit.part != PC_TRANSFORM_CORNER && hit.part != PC_TRANSFORM_EDGE)
            return;
        else
            preserve_press = true;
    } else if (!point_tool(ui->tool))
        return;
    (void)pc_ui_snap_begin(s, pc_ui_shell_app_session(app), ui, p, moving ? rect : NULL, exclude, nexclude,
                           smart, selection, false, NULL);
    s->preserve_press = preserve_press;
}
static bool pixel_round(pc_ui_shell_app *app)
{
    pc_ui_tool tool = pc_ui_shell_app_state(app)->tool;
    if (!(tool == PC_TOOL_PEN || tool == PC_TOOL_CURVATURE_PEN || tool == PC_TOOL_RECTANGLE ||
          tool == PC_TOOL_ELLIPSE_SHAPE || tool == PC_TOOL_TRIANGLE || tool == PC_TOOL_POLYGON ||
          tool == PC_TOOL_LINE))
        return false;
    pc_json_value value = PC_JSON_VALUE_INIT;
    bool enabled =
        pc_engine_prefs_get(pc_ui_shell_app_session(app), "tools.snapVectorToolsAndTransformsToPixelGrid",
                            &value, NULL) == PC_OK &&
        yyjson_mut_get_bool(pc_json_value_root(&value));
    pc_json_value_fini(&value);
    return enabled;
}
void pc_ui_shell_snap_filter(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point *point,
                             unsigned mods)
{
    pc_ui_snap *s = pc_ui_shell_snap_runtime(app);
    double p[2] = {point->x, point->y}, out[2];
    if (!isfinite(p[0]) || !isfinite(p[1]) || fabs(p[0]) > 1000000 || fabs(p[1]) > 1000000 ||
        kind == PC_UI_POINTER_CANCEL) {
        pc_ui_snap_fini(s);
        return;
    }
    if (kind == PC_UI_POINTER_DOWN)
        begin(app, p,
              pc_ui_selection_move_kind(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app), p[0], p[1],
                                        mods));
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state ||
        (s->active && (pc_doc_arc_get(state->doc)->id != s->document || state->revision != s->revision)))
        pc_ui_snap_fini(s);
    bool skip = kind == PC_UI_POINTER_DOWN && (s->moving || s->preserve_press);
    /* Crop handles snap their actual geometry in crop_ui after calculating their preview. */
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->tool == PC_TOOL_CROP && !s->moving &&
        (s->preserve_press || !pc_ui_shell_crop_runtime(app)->default_frame))
        skip = true;
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    double zoom = view ? view->zoom / pc_ui_shell_app_ui_scale(app) : 1;
#if defined(__APPLE__)
    bool disabled = (mods & PC_UI_MOD_CTRL) && !(mods & PC_UI_MOD_COMMAND);
#else
    bool disabled = (mods & (PC_UI_MOD_CTRL | PC_UI_MOD_COMMAND)) != 0;
#endif
    if (!skip && pc_ui_snap_point(s, p, 8 / fmax(.01, zoom), kind != PC_UI_POINTER_DOWN && disabled, out,
                                  NULL) == PC_OK) {
        if (pixel_round(app)) {
            out[0] = round(out[0]);
            out[1] = round(out[1]);
        }
        point->x = (float)out[0];
        point->y = (float)out[1];
    }
}
void pc_ui_shell_snap_retarget_move(pc_ui_shell_app *app, pc_ui_tool_point point)
{
    if (pc_ui_shell_app_state(app)->tool == PC_TOOL_MOVE) {
        double p[2] = {point.x, point.y};
        begin(app, p, 0);
    }
}
void pc_ui_shell_snap_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    const pc_ui_snap *s = pc_ui_shell_snap_runtime(app);
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->view.extras || !ui->view.show[4])
        return;
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    struct nk_rect saved = canvas->clip;
    nk_push_scissor(canvas, nk_rect(xf->x, xf->y, xf->width, xf->height));
    for (size_t i = 0; i < s->nlines; i++) {
        const pc_snap_line *line = &s->lines[i];
        if (!pc_snap_kind_is_smart(line->kind))
            continue;
        float a[2], b[2];
        if (pc_ui_canvas_to_screen(xf, (float)(line->vertical ? line->pos : line->from),
                                   (float)(line->vertical ? line->from : line->pos), a) == PC_OK &&
            pc_ui_canvas_to_screen(xf, (float)(line->vertical ? line->pos : line->to),
                                   (float)(line->vertical ? line->to : line->pos), b) == PC_OK)
            pc_ui_canvas_ink_line(canvas, xf, a[0], a[1], b[0], b[1], 1, nk_rgb(230, 38, 205));
    }
    nk_push_scissor(canvas, saved);
}
double pc_ui_shell_snap_guide(pc_ui_shell_app *app, bool vertical, double position)
{
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->extras.snap)
        return position;
    pc_ui_snap targets = {0};
    double p[2] = {0};
    if (pc_ui_snap_begin(&targets, pc_ui_shell_app_session(app), ui, p, NULL, NULL, 0, false, false, true,
                         NULL) != PC_OK)
        return position;
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    double zoom = view ? view->zoom / pc_ui_shell_app_ui_scale(app) : 1;
    pc_snap_target target;
    if (pc_snap_targets_nearest(&targets.targets, vertical, position, 8 / fmax(.01, zoom), &target))
        position = target.pos;
    pc_ui_snap_fini(&targets);
    return position;
}

static void crop_handle(const pc_ui_crop_frame *f, pc_ui_transform_hit hit, double out[2])
{
    pc_ui_transform_frame q;
    pc_ui_crop_quad(f, &q);
    size_t i = hit.index % 4;
    for (size_t axis = 0; axis < 2; axis++)
        out[axis] = hit.part == PC_TRANSFORM_CORNER ? q.quad[i][axis]
                                                    : (q.quad[i][axis] + q.quad[(i + 1) % 4][axis]) / 2;
}
void pc_ui_shell_snap_crop(pc_ui_shell_app *app, const pc_ui_crop_frame *frame, const pc_ui_crop_drag *drag,
                           double p[2], double ratio, bool shift, bool alt, bool space, unsigned mods,
                           double zoom)
{
    pc_ui_snap *s = pc_ui_shell_snap_runtime(app);
#if defined(__APPLE__)
    bool disabled = (mods & PC_UI_MOD_CTRL) && !(mods & PC_UI_MOD_COMMAND);
#else
    bool disabled = (mods & (PC_UI_MOD_CTRL | PC_UI_MOD_COMMAND)) != 0;
#endif
    free(s->lines);
    s->lines = NULL;
    s->nlines = 0;
    pc_ui_transform_hit hit = drag->transform.hit;
    if (!s->active || disabled || (hit.part != PC_TRANSFORM_EDGE && hit.part != PC_TRANSFORM_CORNER))
        return;
    pc_ui_crop_frame candidate = *frame;
    pc_ui_crop_drag copy = *drag;
    if (!pc_ui_crop_motion(&candidate, &copy, p, ratio, shift, alt, space))
        return;
    double handle[2], snapped[2], jac[2][2];
    crop_handle(&candidate, hit, handle);
    if (pc_snap_targets_snap_point(&s->targets, handle, 8 / fmax(.01, zoom), snapped, &s->lines, &s->nlines,
                                   NULL) != PC_OK ||
        !s->nlines)
        return;
    for (size_t axis = 0; axis < 2; axis++) {
        double probe[2] = {p[0], p[1]}, h[2];
        probe[axis] += .01;
        pc_ui_crop_frame trial = *frame;
        copy = *drag;
        if (!pc_ui_crop_motion(&trial, &copy, probe, ratio, shift, alt, space))
            return;
        crop_handle(&trial, hit, h);
        for (size_t row = 0; row < 2; row++)
            jac[row][axis] = (h[row] - handle[row]) / .01;
    }
    size_t reachable = 0;
    for (size_t i = 0; i < s->nlines; i++) {
        size_t axis = s->lines[i].vertical ? 0 : 1;
        if (jac[axis][0] * jac[axis][0] + jac[axis][1] * jac[axis][1] > 1e-8)
            s->lines[reachable++] = s->lines[i];
    }
    s->nlines = reachable;
    if (!reachable)
        return;
    double error[2] = {snapped[0] - handle[0], snapped[1] - handle[1]}, correction[2] = {0};
    double det = jac[0][0] * jac[1][1] - jac[0][1] * jac[1][0];
    if (fabs(det) > 1e-6 && s->nlines == 2) {
        correction[0] = (error[0] * jac[1][1] - jac[0][1] * error[1]) / det;
        correction[1] = (jac[0][0] * error[1] - error[0] * jac[1][0]) / det;
    } else {
        /* A constrained corner has one degree of freedom. Honor one reachable alignment. */
        size_t axis = s->lines[0].vertical ? 0 : 1;
        double norm = jac[axis][0] * jac[axis][0] + jac[axis][1] * jac[axis][1];
        if (norm < 1e-8)
            return;
        correction[0] = error[axis] * jac[axis][0] / norm;
        correction[1] = error[axis] * jac[axis][1] / norm;
    }
    double adjusted[2] = {p[0] + correction[0], p[1] + correction[1]};
    candidate = *frame;
    copy = *drag;
    if (!pc_ui_crop_motion(&candidate, &copy, adjusted, ratio, shift, alt, space))
        return;
    crop_handle(&candidate, hit, handle);
    size_t count = 0;
    for (size_t i = 0; i < s->nlines; i++)
        if (fabs(handle[s->lines[i].vertical ? 0 : 1] - s->lines[i].pos) < .001)
            s->lines[count++] = s->lines[i];
    s->nlines = count;
    if (count) {
        p[0] = adjusted[0];
        p[1] = adjusted[1];
    }
}
