#include "retouch_ui.h"
#include "shell_internal.h"
#include "canvas_ink.h"
#include "ui/canvas.h"
#include "ui/clone_marker.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <string.h>

bool pc_ui_retouch_tool(pc_ui_tool tool)
{
    switch (tool) {
    case PC_TOOL_CLONE_STAMP:
    case PC_TOOL_PATTERN_STAMP:
    case PC_TOOL_HEALING:
    case PC_TOOL_SPOT_HEALING:
    case PC_TOOL_PATCH:
    case PC_TOOL_CONTENT_AWARE_MOVE:
    case PC_TOOL_RED_EYE:
    case PC_TOOL_DODGE:
    case PC_TOOL_BURN:
    case PC_TOOL_SPONGE:
    case PC_TOOL_BLUR:
    case PC_TOOL_SHARPEN:
    case PC_TOOL_SMUDGE:
    case PC_TOOL_COLOR_REPLACEMENT:
        return true;
    default:
        return false;
    }
}
/* Tools that paint with a brush (size/hardness live on the current brush, not tool_options). */
static bool has_brush(pc_ui_tool tool)
{
    return tool != PC_TOOL_PATCH && tool != PC_TOOL_CONTENT_AWARE_MOVE && tool != PC_TOOL_RED_EYE;
}
static void brush_field(pc_ui_shell_app *app, const char *label, const char *key, float value, float min,
                        float max, float scale)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    float next = value * scale;
    nk_property_float(c, pc_i18n_t(label), min, &next, max, 1, .25f);
    if (next != value * scale) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d), *brush = yyjson_mut_obj(d);
            yyjson_mut_obj_add_real(d, brush, key, next / scale);
            yyjson_mut_obj_add_val(d, p, "brush", brush);
            yyjson_mut_obj_add_str(d, p, "coalesce", "optionsBar");
            pc_ui_shell_queue(app, "tools.setBrush", p);
            yyjson_mut_doc_free(d);
        }
    }
}
/* A dropdown whose selected key is written into a pc_str tool_options field. */
static void str_combo(pc_ui_shell_app *app, const char *mapping, pc_str *field, const char *const *keys,
                      const char *const *labels, int n, float width)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    const char *translated[16];
    int selected = 0;
    if (n > 16) {
        n = 16;
    }
    for (int i = 0; i < n; i++) {
        translated[i] = pc_i18n_t(labels[i]);
        if (pc_str_eq_c(field, keys[i])) {
            selected = i;
        }
    }
    nk_layout_row_push(c, width);
    if (mapping) {
        pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(c));
    }
    int next = nk_combo(c, translated, n, selected, 24, nk_vec2(width + 70, (float)n * 28 + 12));
    if (next >= 0 && next < n && next != selected) {
        (void)pc_str_set_c(field, keys[next], NULL);
    }
}
static void check(pc_ui_shell_app *app, const char *label, bool *value, float width)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    nk_layout_row_push(c, width);
    nk_bool state = *value;
    if (nk_checkbox_label(c, pc_i18n_t(label), &state)) {
        *value = state != 0;
    }
}
static void fval(pc_ui_shell_app *app, const char *label, float *value, float min, float max, float width)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    nk_layout_row_push(c, width);
    nk_property_float(c, pc_i18n_t(label), min, value, max, 1, .25f);
}
static void label_only(pc_ui_shell_app *app, const char *text, float width)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    nk_layout_row_push(c, width);
    nk_label(c, pc_i18n_t(text), NK_TEXT_LEFT);
}
static bool icon_toggle(pc_ui_shell_app *app, const char *icon, const char *fallback, const char *tip,
                        bool selected)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_layout_row_push(c, 30);
    struct nk_rect r = nk_widget_bounds(c);
    struct nk_style_item before = c->style.button.normal;
    if (selected) {
        c->style.button.normal = nk_style_item_color(pc_ui_nk_color(t->accent_soft));
    }
    struct nk_image image;
    bool clicked;
    if (pc_ui_shell_icon_image(app, icon, 18, &image)) {
        clicked = nk_button_label(c, "") != 0;
        struct nk_command_buffer *canvas = nk_window_get_canvas(c);
        struct nk_rect ir = nk_rect(r.x + (r.w - 18) * .5f, r.y + (r.h - 18) * .5f, 18, 18);
        nk_draw_image(canvas, ir, &image, pc_ui_nk_color(t->icon));
    } else {
        clicked = nk_button_label(c, fallback) != 0;
    }
    c->style.button.normal = before;
    if (tip && nk_input_is_mouse_hovering_rect(&c->input, r)) {
        nk_tooltip(c, pc_i18n_t(tip));
    }
    return clicked;
}
/* The size/hardness (and, for Clone Stamp, opacity/flow) brush chip + brush picker buttons. */
static void brush_chip(pc_ui_shell_app *app, pc_ui_tool tool)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
    nk_layout_row_push(c, 150);
    brush_field(app, "Size", "size", brush->size, 1, 5000, 1);
    nk_layout_row_push(c, 150);
    brush_field(app, "Hardness", "hardness", brush->hardness, 0, 100, 100);
    if (tool == PC_TOOL_CLONE_STAMP) {
        nk_layout_row_push(c, 150);
        brush_field(app, "Opacity", "opacity", brush->opacity, 0, 100, 100);
        nk_layout_row_push(c, 150);
        brush_field(app, "Flow", "flow", brush->flow, 0, 100, 100);
    }
    nk_layout_row_push(c, 80);
    struct nk_rect picker = nk_widget_bounds(c);
    pc_ui_shell_widget_mapping(app, "brush.picker", picker);
    if (nk_button_label(c, pc_i18n_t("Brushes"))) {
        ui->brush_picker.open = !ui->brush_picker.open;
        ui->brush_picker.point[0] = picker.x;
        ui->brush_picker.point[1] = picker.y + picker.h;
    }
    nk_layout_row_push(c, 36);
    pc_ui_shell_widget_mapping(app, "brush.settings", nk_widget_bounds(c));
    if (nk_button_label(c, "F5")) {
        pc_ui_shell_queue(app, "window.panel.brushSettings", NULL);
    }
}
void pc_ui_shell_retouch_options(pc_ui_shell_app *app)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_tool tool = ui->tool;
    pc_ui_tool_options *o = &ui->tool_options;
    nk_layout_row_begin(c, NK_STATIC, 24, 24);
    nk_layout_row_push(c, 150);
    nk_label(c, pc_i18n_t(pc_ui_tool_label(tool)), NK_TEXT_LEFT);
    if (has_brush(tool)) {
        brush_chip(app, tool);
    }
    switch (tool) {
    case PC_TOOL_CLONE_STAMP:
    case PC_TOOL_HEALING: {
        check(app, "Aligned", &o->clone_aligned, 90);
        label_only(app, "Sample:", 60);
        const char *keys[] = {"current", "currentAndBelow", "all"};
        const char *labels[] = {"Current Layer", "Current & Below", "All Layers"};
        str_combo(app, "clone.sample", &o->clone_sample, keys, labels, 3, 150);
        break;
    }
    case PC_TOOL_PATTERN_STAMP: {
        check(app, "Aligned", &o->pattern_stamp_aligned, 90);
        check(app, "Impressionist", &o->pattern_stamp_impressionist, 120);
        label_only(app, "Pattern", 65);
        nk_layout_row_push(c, 90);
        pc_ui_shell_widget_mapping(app, "window.panel.patterns", nk_widget_bounds(c));
        if (nk_button_label(c, pc_i18n_t("Patterns…"))) {
            pc_ui_shell_queue(app, "window.panel.patterns", NULL);
        }
        fval(app, "Scale", &o->pattern_stamp_scale, 1, 1000, 120);
        fval(app, "Angle", &o->pattern_stamp_angle, -180, 180, 120);
        break;
    }
    case PC_TOOL_SPOT_HEALING: {
        label_only(app, "Type:", 50);
        const char *keys[] = {"contentAware", "createTexture", "proximityMatch"};
        const char *labels[] = {"Content-Aware", "Create Texture", "Proximity Match"};
        str_combo(app, "spot.type", &o->spot_type, keys, labels, 3, 150);
        check(app, "Sample All Layers", &o->sample_all_layers, 150);
        break;
    }
    case PC_TOOL_PATCH: {
        label_only(app, "Patch:", 55);
        const char *keys[] = {"normal", "contentAware"};
        const char *labels[] = {"Normal", "Content-Aware"};
        pc_str kind = {0};
        (void)pc_str_set_c(&kind, o->patch_content_aware ? "contentAware" : "normal", NULL);
        str_combo(app, "patch.kind", &kind, keys, labels, 2, 130);
        o->patch_content_aware = pc_str_eq_c(&kind, "contentAware");
        pc_str_fini(&kind);
        if (o->patch_content_aware) {
            fval(app, "Structure", &o->patch_structure, 1, 7, 120);
            fval(app, "Color", &o->patch_color, 0, 10, 120);
            check(app, "Sample All Layers", &o->sample_all_layers, 150);
        } else {
            const char *mkeys[] = {"source", "destination"};
            const char *mlabels[] = {"Source", "Destination"};
            str_combo(app, "patch.mode", &o->patch_mode, mkeys, mlabels, 2, 130);
        }
        break;
    }
    case PC_TOOL_CONTENT_AWARE_MOVE: {
        label_only(app, "Mode:", 50);
        const char *keys[] = {"move", "extend"};
        const char *labels[] = {"Move", "Extend"};
        str_combo(app, "cam.mode", &o->cam_mode, keys, labels, 2, 100);
        fval(app, "Structure", &o->cam_structure, 1, 7, 120);
        fval(app, "Color", &o->cam_color, 0, 10, 120);
        check(app, "Sample All Layers", &o->sample_all_layers, 150);
        check(app, "Transform On Drop", &o->cam_transform_on_drop, 160);
        break;
    }
    case PC_TOOL_RED_EYE: {
        fval(app, "Pupil Size", &o->red_eye_pupil_size, 1, 100, 150);
        fval(app, "Darken Amount", &o->red_eye_darken, 0, 100, 160);
        break;
    }
    case PC_TOOL_DODGE:
    case PC_TOOL_BURN: {
        label_only(app, "Range:", 55);
        const char *keys[] = {"shadows", "midtones", "highlights"};
        const char *labels[] = {"Shadows", "Midtones", "Highlights"};
        str_combo(app, "tone.range", &o->tone_range, keys, labels, 3, 120);
        fval(app, "Exposure", &o->exposure, 1, 100, 140);
        check(app, "Protect Tones", &o->protect_tones, 130);
        break;
    }
    case PC_TOOL_SPONGE: {
        label_only(app, "Mode:", 50);
        const char *keys[] = {"desaturate", "saturate"};
        const char *labels[] = {"Desaturate", "Saturate"};
        str_combo(app, "sponge.mode", &o->sponge_mode, keys, labels, 2, 120);
        check(app, "Vibrance", &o->vibrance, 100);
        break;
    }
    case PC_TOOL_BLUR:
    case PC_TOOL_SHARPEN:
    case PC_TOOL_SMUDGE: {
        fval(app, "Strength", &o->strength, 1, 100, 150);
        check(app, "Sample All Layers", &o->sample_all_layers, 150);
        if (tool == PC_TOOL_SHARPEN) {
            check(app, "Protect Detail", &o->protect_detail, 130);
        }
        if (tool == PC_TOOL_SMUDGE) {
            check(app, "Finger Painting", &o->finger_painting, 140);
        }
        break;
    }
    case PC_TOOL_COLOR_REPLACEMENT: {
        label_only(app, "Mode:", 50);
        const char *keys[] = {"hue", "saturation", "color", "luminosity"};
        const char *labels[] = {"Hue", "Saturation", "Color", "Luminosity"};
        str_combo(app, "colorReplacement.mode", &o->cr_mode, keys, labels, 4, 110);
        if (icon_toggle(app, "pipette", "C", "Sampling: Continuous", pc_str_eq_c(&o->cr_sampling, "continuous"))) {
            (void)pc_str_set_c(&o->cr_sampling, "continuous", NULL);
        }
        if (icon_toggle(app, "circle-dot", "O", "Sampling: Once", pc_str_eq_c(&o->cr_sampling, "once"))) {
            (void)pc_str_set_c(&o->cr_sampling, "once", NULL);
        }
        if (icon_toggle(app, "square", "B", "Sampling: Background Swatch",
                        pc_str_eq_c(&o->cr_sampling, "backgroundSwatch"))) {
            (void)pc_str_set_c(&o->cr_sampling, "backgroundSwatch", NULL);
        }
        label_only(app, "Limits:", 55);
        const char *lkeys[] = {"discontiguous", "contiguous", "findEdges"};
        const char *llabels[] = {"Discontiguous", "Contiguous", "Find Edges"};
        str_combo(app, "colorReplacement.limits", &o->cr_limits, lkeys, llabels, 3, 120);
        fval(app, "Tolerance", &o->cr_tolerance, 0, 100, 150);
        check(app, "Anti-alias", &o->cr_anti_alias, 110);
        break;
    }
    default:
        break;
    }
    nk_layout_row_end(c);
}

/* The on-canvas Clone Stamp / Healing Brush source crosshair (gap 16). Two-tone cross at the
 * Alt-click source, clipped to the canvas by the ink helpers; drawn from canvas_view.c. The source
 * point and the decision to show it live in ui/clone_marker.c (headless, tested). */
void pc_ui_shell_clone_source_draw(pc_ui_shell_app *app, const pc_ui_tool_gesture *gesture,
                                   const pc_ui_canvas_xform *xf)
{
    if (app == NULL || gesture == NULL || xf == NULL) {
        return;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (state == NULL) {
        return;
    }
    double source[2];
    if (!pc_ui_clone_source_marker(gesture, pc_ui_shell_app_state(app)->tool,
                                   pc_doc_arc_get(state->doc)->id, source)) {
        return;
    }
    float p[2];
    if (pc_ui_canvas_to_screen(xf, (float)source[0], (float)source[1], p) != PC_OK ||
        !pc_ui_canvas_ink_visible(xf, p[0], p[1], 8)) {
        return;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    const float arm = 7.f;
    for (unsigned pass = 0; pass < 2; pass++) {
        float width = pass ? 1.f : 3.f;
        struct nk_color ink = pass ? nk_rgb(255, 255, 255) : nk_rgba(0, 0, 0, 160);
        pc_ui_canvas_ink_line(canvas, xf, p[0] - arm, p[1], p[0] + arm, p[1], width, ink);
        pc_ui_canvas_ink_line(canvas, xf, p[0], p[1] - arm, p[0], p[1] + arm, width, ink);
    }
}
