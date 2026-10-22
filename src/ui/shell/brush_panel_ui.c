#include "brush_picker_ui.h"
#include "brush_panel_ui.h"
#include "shell_internal.h"
#include "filter_ui.h"
#include "ui/brush_settings.h"
#include "ui/brush_tip.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "paint/render.h"
#include "paint/json.h"
#include "paint/procedural.h"
#include "engine/prefs.h"
#include <blake3.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const section_names[] = {
    "Brush Tip Shape", "Shape Dynamics", "Scattering", "Texture",  "Dual Brush", "Color Dynamics", "Transfer",
    "Brush Pose",      "Noise",          "Wet Edges",  "Build-up", "Smoothing",  "Protect Texture"};
static const char *const section_keys[] = {
    "",     "shapeDynamics", "scattering", "texture", "dualBrush", "colorDynamics", "transfer",
    "pose", "noise",         "wetEdges",   "buildUp", "smoothing", "protectTexture"};
static const char *const controls[] = {"off",         "fade",     "penPressure",      "penTilt",
                                       "stylusWheel", "rotation", "initialDirection", "direction"};
static const char *const masks[] = {"multiply",  "subtract",   "darken",  "overlay",      "colorDodge",
                                    "colorBurn", "linearBurn", "hardMix", "linearHeight", "height"};

void pc_ui_brush_panel_fini(pc_ui_brush_panel *p)
{
    if (p->has_preview)
        pc_paint_brush_fini(&p->preview_brush);
    *p = (pc_ui_brush_panel){0};
}
static void map(pc_ui_shell_app *app, const char *path)
{
    char id[512];
    int length = snprintf(id, sizeof id, "brush.%s", path);
    if (length < 0 || (size_t)length >= sizeof id)
        return;
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(pc_ui_shell_app_nk(app)));
}
static void patch(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *path,
                  yyjson_mut_val *value)
{
    yyjson_mut_val *fields = NULL;
    pc_error err;
    if (pc_ui_brush_setting_patch(d, path, value, &fields, &err) != PC_OK) {
        pc_ui_shell_report(app, "Brush Settings", err.msg, true);
        return;
    }
    yyjson_mut_val *params = yyjson_mut_obj(d);
    char key[280];
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT))
        panel->gesture++;
    (void)snprintf(key, sizeof key, "brush-panel:%llu:%s", (unsigned long long)panel->gesture, path);
    if (!params || !yyjson_mut_obj_add_val(d, params, "brush", fields) ||
        !yyjson_mut_obj_add_strcpy(d, params, "coalesce", key)) {
        pc_ui_shell_report(app, "Brush Settings", "Not enough memory to change brush", true);
        return;
    }
    pc_ui_shell_queue(app, "tools.setBrush", params);
}
static void boolean(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                    const char *path, bool value)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_bool checked = value;
    map(app, path);
    if (nk_checkbox_label(ctx, pc_i18n_t(label), &checked))
        patch(app, panel, d, path, yyjson_mut_bool(d, checked));
}
static void numeric(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                    const char *path, const char *key, yyjson_mut_val *value)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float min = 0, max = 1, scale = 100;
    if (!strcmp(key, "size")) {
        min = 1;
        max = 5000;
        scale = 1;
    } else if (!strcmp(key, "angle")) {
        min = -180;
        max = 180;
        scale = 1;
    } else if (!strcmp(key, "rotation")) {
        max = 360;
        scale = 1;
    } else if (!strcmp(key, "tiltX") || !strcmp(key, "tiltY")) {
        min = -90;
        max = 90;
        scale = 1;
    } else if (!strcmp(key, "spacing") || !strcmp(key, "scale")) {
        min = .01f;
        max = 10;
    } else if (!strcmp(key, "count")) {
        min = 1;
        max = 16;
        scale = 1;
    } else if (!strcmp(key, "fadeSteps")) {
        min = 1;
        max = 10000;
        scale = 1;
    } else if (!strcmp(key, "buildUpRate")) {
        min = 1;
        max = 200;
        scale = 1;
    } else if (!strcmp(key, "tiltScale"))
        max = 2;
    else if (!strcmp(path, "scattering.scatter.jitter") || !strcmp(path, "dualBrush.scatter"))
        max = 10;
    else if (!strcmp(key, "brightness") || !strcmp(key, "contrast") || !strcmp(key, "purity"))
        min = -1;
    else if (!strcmp(path, "flow") || !strcmp(key, "roundness"))
        min = .01f;
    double number = yyjson_mut_get_num(value);
    float shown = (float)number * scale, before = shown;
    map(app, path);
    nk_property_float(ctx, pc_i18n_t(label), min * scale, &shown, max * scale, 1, .25f);
    if (shown != before) {
        yyjson_mut_val *next = yyjson_mut_is_int(value)
                                   ? yyjson_mut_uint(d, (uint64_t)fmaxf(0, roundf(shown / scale)))
                                   : yyjson_mut_real(d, (double)(shown / scale));
        patch(app, panel, d, path, next);
    }
}
static void enumeration(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                        const char *path, const char *key, const char *current)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    char display[160];
    pc_ui_shell_parameter_label(current, display);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    map(app, path);
    if (nk_combo_begin_label(ctx, pc_i18n_t(display), nk_vec2(240, 280))) {
        const char *const *names = !strcmp(key, "control") ? controls : masks;
        size_t count = !strcmp(key, "control") ? 8 : 10;
        bool blend = !strcmp(path, "mode");
        if (blend)
            count = PC_BLEND_COUNT - 1;
        nk_layout_row_dynamic(ctx, 22, 1);
        for (size_t i = 0; i < count; i++) {
            const char *name = blend ? pc_blend_name((pc_blend_mode)(i + 1)) : names[i];
            pc_ui_shell_parameter_label(name, display);
            if (nk_combo_item_label(ctx, pc_i18n_t(display), NK_TEXT_LEFT))
                patch(app, panel, d, path, yyjson_mut_strcpy(d, name));
        }
        nk_combo_end(ctx);
    }
}
static void fields(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, yyjson_mut_val *object,
                   const char *prefix, size_t depth)
{
    if (depth > 5 || !yyjson_mut_is_obj(object))
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_obj_iter it;
    yyjson_mut_obj_iter_init(object, &it);
    yyjson_mut_val *entry;
    while ((entry = yyjson_mut_obj_iter_next(&it))) {
        const char *key = yyjson_mut_get_str(entry);
        yyjson_mut_val *value = yyjson_mut_obj_iter_get_val(entry);
        if (!strcmp(key, "locks") || !strcmp(key, "enabled"))
            continue;
        char path[256], label[160];
        int size = snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "." : "", key);
        if (size < 0 || (size_t)size >= sizeof path)
            continue;
        pc_ui_shell_parameter_label(key, label);
        if (yyjson_mut_is_obj(value)) {
            if (!*prefix)
                continue;
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
            fields(app, panel, d, value, path, depth + 1);
        } else if (yyjson_mut_is_bool(value)) {
            nk_layout_row_dynamic(ctx, 24, 1);
            boolean(app, panel, d, label, path, yyjson_mut_get_bool(value));
        } else if (yyjson_mut_is_num(value)) {
            const char *control = yyjson_mut_get_str(yyjson_mut_obj_get(object, "control"));
            bool disabled = !strcmp(key, "minimum") && control && !strcmp(control, "off");
            nk_layout_row_dynamic(ctx, 26, 1);
            if (disabled)
                nk_widget_disable_begin(ctx);
            numeric(app, panel, d, label, path, key, value);
            if (disabled)
                nk_widget_disable_end(ctx);
        } else if (yyjson_mut_is_str(value)) {
            nk_layout_row_dynamic(ctx, 24, 2);
            enumeration(app, panel, d, label, path, key, yyjson_mut_get_str(value));
        }
    }
}
/* ---- Brush Tip Shape / Texture / Dual Brush bespoke panes (port of brush_sections.rs) ---- */

static float clamp_f(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static void patch_real(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *path,
                       double value)
{
    patch(app, panel, d, path, yyjson_mut_real(d, value));
}
static void patch_uint(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *path,
                       uint64_t value)
{
    patch(app, panel, d, path, yyjson_mut_uint(d, value));
}

/* Render a 34x34 thumbnail of preset `index`'s tip (one dab), cached by preset revision. */
static bool tip_thumbnail(pc_ui_shell_app *app, const pc_engine_tool_state *tools, size_t index,
                          struct nk_image *out)
{
    const pc_paint_brush *src = &tools->presets[index].brush;
    blake3_hasher hasher;
    uint8_t key[32];
    blake3_hasher_init(&hasher);
    const char category[] = "brush-tip-grid-v1";
    blake3_hasher_update(&hasher, category, sizeof category);
    blake3_hasher_update(&hasher, &tools->presets_rev, sizeof tools->presets_rev);
    blake3_hasher_update(&hasher, &index, sizeof index);
    blake3_hasher_finalize(&hasher, key, sizeof key);
    if (pc_ui_shell_bitmap(app, key, 34, 34, NULL, out, NULL) == PC_OK)
        return true;
    pc_paint_brush brush;
    pc_paint_brush_copy(&brush, src);
    brush.size = 30;
    brush.spacing_enabled = true;
    brush.spacing = fmaxf(.08f, brush.spacing);
    brush.build_up = brush.erase = false;
    brush.seed = 0;
    brush.color[0] = brush.color[1] = brush.color[2] = 0;
    brush.color[3] = 1;
    const float white[4] = {1, 1, 1, 1}, zero[1] = {0}, one[1] = {1};
    pc_surface surface, clip;
    pc_surface_init_default(&surface, PC_PIXEL_FORMAT_RGBA8, white, 4);
    pc_surface_init_default(&clip, PC_PIXEL_FORMAT_GRAY8, zero, 1);
    pc_rect rect = pc_rect_make(0, 0, 34, 34), damage;
    pc_status status = pc_surface_fill_rect(&clip, rect, one, 1, NULL);
    pc_paint_point point = pc_paint_point_make(17, 17, 1);
    point.time = 0;
    if (status == PC_OK)
        status = pc_paint_render_stroke(&surface, &brush, &point, 1, &clip, false, 1, &damage, NULL);
    uint8_t pixels[34 * 34 * 4];
    if (status == PC_OK)
        status = pc_surface_read_rgba8_into(&surface, rect, (uint8_t (*)[4])pixels, 34 * 34, NULL);
    if (status == PC_OK)
        status = pc_ui_shell_bitmap(app, key, 34, 34, pixels, out, NULL);
    pc_surface_fini(&surface);
    pc_surface_fini(&clip);
    pc_paint_brush_fini(&brush);
    return status == PC_OK;
}

/* A scrollable grid of preset tips; returns the clicked preset index, or -1. `highlight` marks the
 * current preset (or -1). */
static long tip_grid(pc_ui_shell_app *app, const char *id, long highlight)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    long clicked = -1;
    nk_layout_row_dynamic(ctx, 114, 1);
    if (nk_group_begin(ctx, id, NK_WINDOW_BORDER)) {
        nk_layout_row_static(ctx, 50, 44, 5);
        for (size_t i = 0; i < tools->npresets; i++) {
            struct nk_rect r = nk_widget_bounds(ctx), region = ctx->current->layout->clip;
            if (r.y + r.h <= region.y || r.y >= region.y + region.h) {
                nk_spacing(ctx, 1);
                continue;
            }
            char mapping[64];
            (void)snprintf(mapping, sizeof mapping, "%s.%zu", id, i);
            pc_ui_shell_widget_mapping(app, mapping, r);
            bool selected = highlight == (long)i;
            if (selected)
                nk_style_push_style_item(ctx, &ctx->style.button.normal,
                                         nk_style_item_color(pc_ui_nk_color(t->accent_soft)));
            bool hit = nk_button_label(ctx, "") != 0;
            if (selected)
                nk_style_pop_style_item(ctx);
            if (hit)
                clicked = (long)i;
            struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
            struct nk_image img;
            if (tip_thumbnail(app, tools, i, &img)) {
                float scale = pc_ui_brush_thumb_scale(tools->presets[i].brush.size);
                float side = 34 * scale;
                struct nk_rect ir =
                    nk_rect(r.x + (r.w - side) / 2, r.y + 2 + (34 - side) / 2, side, side);
                nk_draw_image(canvas, ir, &img, nk_rgba(255, 255, 255, 255));
            }
            char num[16];
            (void)snprintf(num, sizeof num, "%ld", (long)lroundf(tools->presets[i].brush.size));
            nk_draw_text(canvas, nk_rect(r.x, r.y + 36, r.w, 12), num, (int)strlen(num), ctx->style.font,
                         nk_rgba(0, 0, 0, 0), pc_ui_nk_color(t->text_dim));
        }
        nk_group_end(ctx);
    }
    return clicked;
}

/* Apply preset `index`'s tip fields (tip, size, hardness, angle, roundness, spacing) to the primary
 * tip (dual=false) or the Dual Brush tip (dual=true). The dynamics sections are untouched. For the
 * primary grid the preset is also marked current (brush.presets.setCurrent). */
static void apply_tip(pc_ui_shell_app *app, const pc_engine_tool_state *tools, size_t index, bool dual)
{
    const pc_paint_brush *src = &tools->presets[index].brush;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    yyjson_mut_val *brush = d ? yyjson_mut_obj(d) : NULL;
    yyjson_mut_val *tip = d ? pc_paint_json_write(PC_PAINT_JSON_TIP, d, &src->tip, 0) : NULL;
    yyjson_mut_val *target = dual ? yyjson_mut_obj(d) : brush;
    bool ok = params && brush && tip && target;
    if (ok)
        ok = yyjson_mut_obj_add_val(d, target, "tip", tip) &&
             yyjson_mut_obj_add_real(d, target, "size", (double)src->size) &&
             yyjson_mut_obj_add_real(d, target, "hardness", (double)src->hardness) &&
             yyjson_mut_obj_add_real(d, target, "angle", (double)src->angle) &&
             yyjson_mut_obj_add_real(d, target, "roundness", (double)src->roundness) &&
             yyjson_mut_obj_add_real(d, target, "spacing", (double)src->spacing);
    if (ok && dual)
        ok = yyjson_mut_obj_add_val(d, brush, "dualBrush", target);
    if (ok)
        ok = yyjson_mut_obj_add_val(d, params, "brush", brush);
    if (!ok) {
        pc_ui_shell_report(app, "Brush Settings", "Not enough memory to change brush", true);
        yyjson_mut_doc_free(d);
        return;
    }
    pc_ui_shell_queue(app, "tools.setBrush", params);
    yyjson_mut_doc_free(d);
    if (!dual) {
        const char *name = pc_str_c(&tools->presets[index].name);
        yyjson_mut_doc *nd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *np = nd ? yyjson_mut_obj(nd) : NULL;
        if (np && yyjson_mut_obj_add_strcpy(nd, np, "name", name))
            pc_ui_shell_queue(app, "brush.presets.setCurrent", np);
        yyjson_mut_doc_free(nd);
    }
}

/* Photoshop's angle/roundness widget: drag the arrow to rotate, the dots to squash the tip. */
static void ellipse_widget(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, float angle,
                           float roundness)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_layout_row_dynamic(ctx, 88, 1);
    struct nk_rect region;
    if (nk_widget(&region, ctx) == NK_WIDGET_INVALID)
        return;
    pc_ui_shell_widget_mapping(app, "brush.tipEllipse", region);
    float cx = region.x + 44, cy = region.y + region.h / 2;
    if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT) &&
        nk_input_is_mouse_hovering_rect(&ctx->input, region)) {
        float rx = ctx->input.mouse.pos.x - cx, ry = ctx->input.mouse.pos.y - cy;
        panel->ellipse_grab = (int)pc_ui_ellipse_grab_at(rx, ry, angle, roundness);
        panel->ellipse_dragging = true;
    }
    if (panel->ellipse_dragging) {
        if (nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
            float rx = ctx->input.mouse.pos.x - cx, ry = ctx->input.mouse.pos.y - cy;
            float na = angle, nr = roundness;
            pc_ui_ellipse_drag((pc_ui_ellipse_grab)panel->ellipse_grab, rx, ry, &na, &nr);
            if (na != angle)
                patch_real(app, panel, d, "angle", (double)na);
            if (nr != roundness)
                patch_real(app, panel, d, "roundness", (double)nr);
        } else
            panel->ellipse_dragging = false;
    }
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    float outer_r = 40;
    nk_fill_circle(c, nk_rect(cx - outer_r, cy - outer_r, 2 * outer_r, 2 * outer_r),
                   pc_ui_nk_color(t->field));
    nk_stroke_circle(c, nk_rect(cx - outer_r, cy - outer_r, 2 * outer_r, 2 * outer_r), 1,
                     pc_ui_nk_color(t->field_border));
    nk_stroke_line(c, cx - outer_r, cy, cx + outer_r, cy, 1, pc_ui_nk_color(t->separator));
    nk_stroke_line(c, cx, cy - outer_r, cx, cy + outer_r, 1, pc_ui_nk_color(t->separator));
    float rad = angle * (float)(3.14159265358979323846 / 180.0);
    float s = sinf(rad), co = cosf(rad);
    float mjx = co, mjy = -s, mnx = -s, mny = -co;
    float rr = clamp_f(roundness, .01f, 1);
    float pts[48 * 2];
    for (int i = 0; i < 48; i++) {
        float a = (float)i / 48.0f * 6.28318530717958648f;
        float ca = cosf(a), sa = sinf(a);
        pts[i * 2] = cx + mjx * ca * PC_UI_ELLIPSE_R + mnx * sa * PC_UI_ELLIPSE_R * rr;
        pts[i * 2 + 1] = cy + mjy * ca * PC_UI_ELLIPSE_R + mny * sa * PC_UI_ELLIPSE_R * rr;
    }
    struct nk_color text = pc_ui_nk_color(t->text), accent = pc_ui_nk_color(t->accent);
    struct nk_color fill = text;
    fill.a = 46;
    nk_fill_polygon(c, pts, 48, fill);
    nk_stroke_polygon(c, pts, 48, 1.5f, text);
    nk_stroke_line(c, cx, cy, cx + mjx * (PC_UI_ELLIPSE_R + 5), cy + mjy * (PC_UI_ELLIPSE_R + 5), 1.5f,
                   accent);
    float handles[2][2];
    pc_ui_ellipse_handles(angle, roundness, handles);
    for (int i = 0; i < 2; i++) {
        struct nk_rect hr = nk_rect(cx + handles[i][0] - 4, cy + handles[i][1] - 4, 8, 8);
        nk_fill_circle(c, hr, nk_rgb(255, 255, 255));
        nk_stroke_circle(c, hr, 1, accent);
    }
}

/* Size: a px value field over a logarithmic slider (small sizes get most of the travel). */
static void size_row(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                     const char *path, float size, float max)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_dynamic(ctx, 26, 1);
    float px = size, before = px;
    map(app, path);
    nk_property_float(ctx, pc_i18n_t(label), 1, &px, max, 1, 1);
    if (px != before)
        patch_real(app, panel, d, path, (double)roundf(clamp_f(px, 1, max)));
    nk_layout_row_dynamic(ctx, 12, 1);
    float lv = pc_ui_brush_size_slider(size, max), lbefore = lv;
    (void)nk_slider_float(ctx, 0, &lv, logf(max), .01f);
    if (lv != lbefore)
        patch_real(app, panel, d, path, (double)pc_ui_brush_slider_size(lv, max));
}

/* A percent field (0..1 fraction shown 0..maxpct); patches `path` with the fraction. */
static void pct_row(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, const char *label,
                    const char *path, float frac, float minpct, float maxpct)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_dynamic(ctx, 26, 1);
    float p = roundf(frac * 100), before = p;
    map(app, path);
    nk_property_float(ctx, pc_i18n_t(label), minpct, &p, maxpct, 1, 1);
    if (p != before)
        patch_real(app, panel, d, path, (double)clamp_f(p / 100, minpct / 100, maxpct / 100));
}

static void tip_shape_pane(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    const pc_paint_brush *b = &tools->brush;
    long highlight = -1;
    if (tools->current_preset)
        for (size_t i = 0; i < tools->npresets; i++)
            if (!strcmp(pc_str_c(&tools->presets[i].name), tools->current_preset)) {
                highlight = (long)i;
                break;
            }
    long clicked = tip_grid(app, "brush-tip-grid", highlight);
    if (clicked >= 0) {
        apply_tip(app, tools, (size_t)clicked, false);
        return;
    }
    size_row(app, panel, d, "Size", "size", b->size, PC_PAINT_MAX_BRUSH_SIZE);
    nk_layout_row_dynamic(ctx, 24, 2);
    boolean(app, panel, d, "Flip X", "flipX", b->flip_x);
    boolean(app, panel, d, "Flip Y", "flipY", b->flip_y);
    ellipse_widget(app, panel, d, b->angle, b->roundness);
    nk_layout_row_dynamic(ctx, 26, 1);
    float ang = b->angle, ab = ang;
    map(app, "angle");
    nk_property_float(ctx, pc_i18n_t("Angle"), -180, &ang, 180, 1, .5f);
    if (ang != ab)
        patch_real(app, panel, d, "angle", (double)roundf(clamp_f(ang, -180, 180)));
    pct_row(app, panel, d, "Roundness", "roundness", b->roundness, 1, 100);
    bool round_tip = b->tip.kind == PC_PAINT_TIP_ROUND;
    if (!round_tip)
        nk_widget_disable_begin(ctx);
    pct_row(app, panel, d, "Hardness", "hardness", round_tip ? b->hardness : b->hardness, 0, 100);
    if (!round_tip)
        nk_widget_disable_end(ctx);
    nk_layout_row_dynamic(ctx, 24, 1);
    boolean(app, panel, d, "Spacing", "spacingEnabled", b->spacing_enabled);
    bool sp_on = b->spacing_enabled;
    if (!sp_on)
        nk_widget_disable_begin(ctx);
    pct_row(app, panel, d, "Spacing %", "spacing", b->spacing, 1, 1000);
    if (!sp_on)
        nk_widget_disable_end(ctx);
}

static void texture_swatch(pc_ui_shell_app *app, const pc_paint_texture *tx)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_rect r;
    if (nk_widget(&r, ctx) == NK_WIDGET_INVALID)
        return;
    struct nk_rect box = nk_rect(r.x, r.y + 2, 56, 56);
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    blake3_hasher hasher;
    uint8_t key[32];
    blake3_hasher_init(&hasher);
    const char category[] = "brush-texture-swatch-v1";
    blake3_hasher_update(&hasher, category, sizeof category);
    blake3_hasher_update(&hasher, &tx->pattern.kind, sizeof tx->pattern.kind);
    blake3_hasher_update(&hasher, &tx->pattern.style, sizeof tx->pattern.style);
    blake3_hasher_update(&hasher, &tx->pattern.size, sizeof tx->pattern.size);
    blake3_hasher_update(&hasher, &tx->pattern.seed, sizeof tx->pattern.seed);
    blake3_hasher_update(&hasher, &tx->invert, sizeof tx->invert);
    blake3_hasher_update(&hasher, &tx->brightness, sizeof tx->brightness);
    blake3_hasher_update(&hasher, &tx->contrast, sizeof tx->contrast);
    blake3_hasher_finalize(&hasher, key, sizeof key);
    struct nk_image img;
    if (pc_ui_shell_bitmap(app, key, 56, 56, NULL, &img, NULL) != PC_OK) {
        uint8_t pixels[56 * 56 * 4];
        pc_paint_pattern_image *pi = NULL;
        bool have = tx->pattern.kind == PC_PAINT_PATTERN_PROCEDURAL &&
                    pc_paint_pattern_generate(tx->pattern.style, tx->pattern.size ? tx->pattern.size : 128,
                                              tx->pattern.seed, &pi, NULL) == PC_OK;
        for (int yy = 0; yy < 56; yy++)
            for (int xx = 0; xx < 56; xx++) {
                float v = have ? pc_paint_pattern_sample_wrap(pi, (float)xx, (float)yy) : 0.5f;
                v = pc_paint_texture_level(tx, v);
                uint8_t g = (uint8_t)(clamp_f(v, 0, 1) * 255 + .5f);
                size_t idx = ((size_t)yy * 56 + (size_t)xx) * 4;
                pixels[idx] = pixels[idx + 1] = pixels[idx + 2] = g;
                pixels[idx + 3] = 255;
            }
        pc_paint_pattern_image_release(pi);
        (void)pc_ui_shell_bitmap(app, key, 56, 56, pixels, &img, NULL);
    }
    nk_draw_image(c, box, &img, nk_rgba(255, 255, 255, 255));
    nk_stroke_rect(c, box, 0, 1, pc_ui_nk_color(t->field_border));
}

static void texture_pane(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d,
                         yyjson_mut_val *root)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    const pc_paint_texture *tx = &tools->brush.texture;
    nk_layout_row_static(ctx, 60, 56, 1);
    texture_swatch(app, tx);
    /* Style dropdown. */
    const char *style_names[] = {"noise", "canvas", "paper", "dots"};
    const char *style_labels[] = {"Noise", "Canvas", "Paper", "Dots"};
    bool procedural = tx->pattern.kind == PC_PAINT_PATTERN_PROCEDURAL;
    const char *current =
        procedural && (size_t)tx->pattern.style < 4 ? style_labels[(size_t)tx->pattern.style] : "Custom";
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_label(ctx, pc_i18n_t("Pattern"), NK_TEXT_LEFT);
    map(app, "texture.pattern");
    if (nk_combo_begin_label(ctx, pc_i18n_t(current), nk_vec2(150, 160))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        for (size_t i = 0; i < 4; i++)
            if (nk_combo_item_label(ctx, pc_i18n_t(style_labels[i]), NK_TEXT_LEFT)) {
                uint32_t sz = procedural && tx->pattern.size ? tx->pattern.size : 128;
                uint32_t seed = procedural ? tx->pattern.seed : 1;
                yyjson_mut_val *proc = yyjson_mut_obj(d), *wrap = yyjson_mut_obj(d);
                bool ok = proc && wrap && yyjson_mut_obj_add_strcpy(d, proc, "style", style_names[i]) &&
                          yyjson_mut_obj_add_uint(d, proc, "size", sz) &&
                          yyjson_mut_obj_add_uint(d, proc, "seed", seed) &&
                          yyjson_mut_obj_add_val(d, wrap, "procedural", proc);
                if (ok)
                    patch(app, panel, d, "texture.pattern", wrap);
                else
                    pc_ui_shell_report(app, "Brush Settings", "Not enough memory", true);
            }
        nk_combo_end(ctx);
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    boolean(app, panel, d, "Invert", "texture.invert", tx->invert);
    if (procedural) {
        nk_layout_row_dynamic(ctx, 26, 1);
        float ps = (float)tx->pattern.size, pb = ps;
        map(app, "texture.pattern.procedural.size");
        nk_property_float(ctx, pc_i18n_t("Pattern Size"), 16, &ps, 1024, 1, 1);
        if (ps != pb)
            patch_uint(app, panel, d, "texture.pattern.procedural.size", (uint64_t)clamp_f(ps, 16, 1024));
    }
    pct_row(app, panel, d, "Scale %", "texture.scale", tx->scale, 1, 1000);
    /* Brightness and Contrast use Photoshop's slider ranges. */
    nk_layout_row_dynamic(ctx, 26, 1);
    float br = pc_ui_texture_brightness_to_ps(tx->brightness), brb = br;
    map(app, "texture.brightness");
    nk_property_float(ctx, pc_i18n_t("Brightness"), -150, &br, 150, 1, 1);
    if (br != brb)
        patch_real(app, panel, d, "texture.brightness", (double)pc_ui_texture_brightness_from_ps(br));
    nk_layout_row_dynamic(ctx, 26, 1);
    float ct = pc_ui_texture_contrast_to_ps(tx->contrast), ctb = ct;
    map(app, "texture.contrast");
    nk_property_float(ctx, pc_i18n_t("Contrast"), -50, &ct, 100, 1, 1);
    if (ct != ctb)
        patch_real(app, panel, d, "texture.contrast", (double)pc_ui_texture_contrast_from_ps(ct));
    nk_layout_row_dynamic(ctx, 24, 1);
    boolean(app, panel, d, "Texture Each Tip", "texture.eachTip", tx->each_tip);
    nk_layout_row_dynamic(ctx, 24, 2);
    enumeration(app, panel, d, "Mode", "texture.mode", "mode", masks[(size_t)tx->mode]);
    pct_row(app, panel, d, "Depth", "texture.depth", tx->depth, 0, 100);
    if (tx->each_tip) {
        yyjson_mut_val *texv = root ? yyjson_mut_obj_get(root, "texture") : NULL;
        yyjson_mut_val *dj = texv ? yyjson_mut_obj_get(texv, "depthJitter") : NULL;
        if (yyjson_mut_is_obj(dj))
            fields(app, panel, d, dj, "texture.depthJitter", 0);
    }
}

static void dual_brush_pane(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    const pc_paint_dual_brush *db = &tools->brush.dual_brush;
    nk_layout_row_dynamic(ctx, 24, 2);
    enumeration(app, panel, d, "Mode", "dualBrush.mode", "mode", masks[(size_t)db->mode]);
    nk_layout_row_dynamic(ctx, 24, 1);
    boolean(app, panel, d, "Flip", "dualBrush.flip", db->flip);
    long clicked = tip_grid(app, "brush-dual-grid", -1);
    if (clicked >= 0) {
        apply_tip(app, tools, (size_t)clicked, true);
        return;
    }
    size_row(app, panel, d, "Size", "dualBrush.size", db->size, 2500);
    pct_row(app, panel, d, "Spacing %", "dualBrush.spacing", db->spacing, 1, 1000);
    pct_row(app, panel, d, "Scatter %", "dualBrush.scatter", db->scatter, 0, 1000);
    nk_layout_row_dynamic(ctx, 24, 1);
    boolean(app, panel, d, "Both Axes", "dualBrush.bothAxes", db->both_axes);
    nk_layout_row_dynamic(ctx, 26, 1);
    float cn = (float)db->count, cb = cn;
    map(app, "dualBrush.count");
    nk_property_float(ctx, pc_i18n_t("Count"), 1, &cn, 16, 1, 1);
    if (cn != cb)
        patch_uint(app, panel, d, "dualBrush.count", (uint64_t)clamp_f(cn, 1, 16));
}

static void sections(pc_ui_shell_app *app, pc_ui_brush_panel *panel, yyjson_mut_doc *d, yyjson_mut_val *root)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->brush_section >= 13)
        ui->brush_section = 0;
    nk_layout_row_begin(ctx, NK_STATIC, 420, 2);
    nk_layout_row_push(ctx, 255);
    if (nk_group_begin(ctx, "brush-sections", 0)) {
        yyjson_mut_val *locks = yyjson_mut_obj_get(root, "locks");
        for (size_t i = 0; i < 13; i++) {
            nk_layout_row_begin(ctx, NK_STATIC, 26, 3);
            nk_layout_row_push(ctx, 20);
            yyjson_mut_val *section = yyjson_mut_obj_get(root, section_keys[i]);
            bool object = yyjson_mut_is_obj(section);
            if (i) {
                char path[96];
                (void)snprintf(path, sizeof path, "%s%s", section_keys[i], object ? ".enabled" : "");
                boolean(app, panel, d, "", path,
                        yyjson_mut_get_bool(object ? yyjson_mut_obj_get(section, "enabled") : section));
            } else
                nk_spacing(ctx, 1);
            nk_layout_row_push(ctx, 140);
            char select[80];
            (void)snprintf(select, sizeof select, "section.%s", section_keys[i]);
            map(app, select);
            if (nk_select_label(ctx, pc_i18n_t(section_names[i]), NK_TEXT_LEFT, ui->brush_section == i)) {
                ui->brush_section = i;
                if (object) {
                    char path[96];
                    (void)snprintf(path, sizeof path, "%s.enabled", section_keys[i]);
                    patch(app, panel, d, path, yyjson_mut_bool(d, true));
                }
            }
            nk_layout_row_push(ctx, 55);
            if (i) {
                char path[96];
                (void)snprintf(path, sizeof path, "locks.%s", section_keys[i]);
                boolean(app, panel, d, "Lock", path,
                        yyjson_mut_get_bool(yyjson_mut_obj_get(locks, section_keys[i])));
            } else
                nk_spacing(ctx, 1);
            nk_layout_row_end(ctx);
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, fmaxf(160, nk_window_get_content_region(ctx).w - 275));
    if (nk_group_begin(ctx, "brush-properties", 0)) {
        size_t selected = ui->brush_section;
        nk_layout_row_dynamic(ctx, 25, 1);
        nk_label(ctx, pc_i18n_t(section_names[selected]), NK_TEXT_LEFT);
        if (selected == 0)
            tip_shape_pane(app, panel, d);
        else if (selected == 3)
            texture_pane(app, panel, d, root);
        else if (selected == 4)
            dual_brush_pane(app, panel, d);
        else {
            yyjson_mut_val *section = yyjson_mut_obj_get(root, section_keys[selected]);
            if (yyjson_mut_is_obj(section))
                fields(app, panel, d, section, section_keys[selected], 0);
            if (selected == 6)
                fields(app, panel, d, yyjson_mut_obj_get(root, "mixer"), "mixer", 0);
            if (selected == 8 || selected == 9 || selected == 12) {
                nk_layout_row_dynamic(ctx, 60, 1);
                const char *note =
                    selected == 8 ? "Adds random variation to the soft edges of the brush tip."
                    : selected == 9
                        ? "Builds up paint along the edges of the stroke."
                        : "Keeps the current pattern and scale when selecting another textured preset.";
                nk_label_wrap(ctx, pc_i18n_t(note));
            }
            if (selected == 10)
                numeric(app, panel, d, "Rate", "buildUpRate", "buildUpRate",
                        yyjson_mut_obj_get(root, "buildUpRate"));
        }
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
}
static void preset_action(pc_ui_shell_app *app, const char *command, const char *name)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (p && yyjson_mut_obj_add_strcpy(d, p, !strcmp(command, "tools.setBrush") ? "preset" : "name", name))
        pc_ui_shell_queue(app, command, p);
    else
        pc_ui_shell_report(app, "Brush Presets", "Not enough memory", true);
    yyjson_mut_doc_free(d);
}
static void presets(pc_ui_shell_app *app, pc_ui_brush_panel *panel)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_dynamic(ctx, 25, 2);
    nk_label(ctx, pc_i18n_t("Search"), NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, panel->search, sizeof panel->search,
                                   nk_filter_default);
    pc_json_value data = PC_JSON_VALUE_INIT;
    pc_engine_session *session = pc_ui_shell_app_session(app);
    (void)pc_engine_session_execute(session, "brush.presets.list", NULL, &data, NULL);
    yyjson_mut_val *items = yyjson_mut_obj_get(pc_json_value_root(&data), "presets");
    nk_layout_row_dynamic(ctx, 335, 1);
    if (nk_group_begin(ctx, "brush-preset-list", 0)) {
        if (pc_ui_shell_app_state(app)->brushes_grid)
            nk_layout_row_dynamic(ctx, 86, 3);
        for (size_t i = 0; i < yyjson_mut_arr_size(items); i++) {
            yyjson_mut_val *item = yyjson_mut_arr_get(items, i);
            const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(item, "name"));
            if (!name || !pc_ui_brush_preset_matches(name, panel->search))
                continue;
            if (pc_ui_shell_app_state(app)->brushes_grid) {
                if (i == 0)
                    nk_layout_row_dynamic(ctx, 86, 3);
                pc_ui_brush_picker view;
                pc_ui_brush_picker_default(&view);
                view.scale = .8f;
                const pc_engine_tool_state *tools = pc_engine_session_tools(session);
                if (i < tools->npresets)
                    pc_ui_shell_brush_preset_card(app, i, tools, &view);
                continue;
            }
            nk_layout_row_dynamic(ctx, 28, 1);
            char id[256];
            (void)snprintf(id, sizeof id, "preset.%s", name);
            map(app, id);
            bool selected = pc_engine_session_tools(session)->current_preset &&
                            !strcmp(pc_engine_session_tools(session)->current_preset, name);
            if (nk_select_label(ctx, name, NK_TEXT_LEFT, selected)) {
                preset_action(app, "tools.setBrush", name);
                (void)snprintf(panel->name, sizeof panel->name, "%s", name);
            }
        }
        nk_group_end(ctx);
    }
    pc_json_value_fini(&data);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Preset Name"), NK_TEXT_LEFT);
    map(app, "preset.name");
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, panel->name, sizeof panel->name, nk_filter_default);
    const char *labels[] = {"New", "Update", "Delete"};
    const char *commands[] = {"brush.presets.save", "brush.presets.update", "brush.presets.delete"};
    nk_layout_row_dynamic(ctx, 28, 3);
    if (!*panel->name)
        nk_widget_disable_begin(ctx);
    for (size_t i = 0; i < 3; i++) {
        map(app, commands[i]);
        if (nk_button_label(ctx, pc_i18n_t(labels[i])))
            preset_action(app, commands[i], panel->name);
    }
    if (!*panel->name)
        nk_widget_disable_end(ctx);
}
static void preview(pc_ui_shell_app *app, pc_ui_brush_panel *panel)
{
    const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
    if (!panel->has_preview || !pc_paint_brush_eq(brush, &panel->preview_brush)) {
        pc_paint_brush small;
        pc_paint_brush_copy(&small, brush);
        float factor = fminf(1, 36 / fmaxf(1, small.size));
        small.size = fmaxf(3, small.size * factor);
        small.dual_brush.size = fminf(36, small.dual_brush.size * factor);
        small.spacing = fmaxf(.08f, small.spacing);
        small.build_up = false;
        small.erase = false;
        const float white[4] = {1, 1, 1, 1};
        pc_surface surface;
        pc_surface_init_default(&surface, PC_PIXEL_FORMAT_RGBA8, white, 4);
        pc_surface clip;
        const float zero[1] = {0}, one[1] = {1};
        pc_surface_init_default(&clip, PC_PIXEL_FORMAT_GRAY8, zero, 1);
        pc_rect rect = pc_rect_make(0, 0, 256, 64), damage;
        pc_status status = pc_surface_fill_rect(&clip, rect, one, 1, NULL);
        pc_paint_point points[32];
        for (size_t i = 0; i < 32; i++) {
            double t = (double)i / 31;
            points[i] = pc_paint_point_make(12 + t * 232, 32 - sin(t * 6.283185307179586) * 10,
                                            (float)(.2 + .8 * sin(t * 3.141592653589793)));
            points[i].time = t * 1000;
        }
        if (status == PC_OK)
            status = pc_paint_render_stroke(&surface, &small, points, 32, &clip, false, 1, &damage, NULL);
        if (status == PC_OK)
            status =
                pc_surface_read_rgba8_into(&surface, rect, (uint8_t (*)[4])panel->pixels, 256 * 64, NULL);
        pc_surface_fini(&surface);
        pc_surface_fini(&clip);
        pc_paint_brush_fini(&small);
        if (status != PC_OK)
            return;
        if (panel->has_preview)
            pc_paint_brush_fini(&panel->preview_brush);
        pc_paint_brush_copy(&panel->preview_brush, brush);
        panel->has_preview = true;
        blake3_hasher hash;
        blake3_hasher_init(&hash);
        blake3_hasher_update(&hash, panel->pixels, sizeof panel->pixels);
        blake3_hasher_finalize(&hash, panel->key, sizeof panel->key);
    }
    struct nk_image image;
    if (pc_ui_shell_bitmap(app, panel->key, 256, 64, panel->pixels, &image, NULL) == PC_OK) {
        struct nk_context *ctx = pc_ui_shell_app_nk(app);
        nk_layout_row_dynamic(ctx, 64, 1);
        nk_image(ctx, image);
    }
}
void pc_ui_brush_panel_draw(pc_ui_shell_app *app, pc_ui_brush_panel *panel)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!ui->panels.brush_settings) {
        panel->shown = false;
        return;
    }
    bool first = !panel->shown;
    panel->shown = true;
    if (nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_LEFT))
        panel->gesture++;
    if (nk_begin(ctx, "Brush Settings", nk_rect(150, 100, 760, 610),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        if (first)
            nk_widget_disable_begin(ctx);
        nk_layout_row_dynamic(ctx, 28, 2);
        map(app, "tab.settings");
        if (nk_select_label(ctx, pc_i18n_t("Brush Settings"), NK_TEXT_CENTERED, ui->brush_tab == 0))
            ui->brush_tab = 0;
        map(app, "tab.presets");
        if (nk_select_label(ctx, pc_i18n_t("Brushes"), NK_TEXT_CENTERED, ui->brush_tab != 0))
            ui->brush_tab = 1;
        if (!ui->brush_tab) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *root =
                pc_ui_brush_settings_fields(d, &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush);
            if (root)
                sections(app, panel, d, root);
            yyjson_mut_doc_free(d);
        } else
            presets(app, panel);
        preview(app, panel);
        if (first)
            nk_widget_disable_end(ctx);
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, "Brush Settings")) {
        ui->panels.brush_settings = false;
        panel->shown = false;
        nk_window_show(ctx, "Brush Settings", NK_SHOWN);
    }
}
