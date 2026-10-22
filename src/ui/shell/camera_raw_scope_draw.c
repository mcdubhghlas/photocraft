/* Camera Raw scopes + preview interactions: Nuklear drawing and input routing. See the header.
 * Logic is in src/ui/camera_raw_scope.c (headless-tested); here we only map it to draw commands and
 * read the engine preview, CPU compositor, colour transforms and the vectorscope algorithm. */
#include "camera_raw_scope_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rgb_histogram_draw.h"
#include "shell_internal.h"
#include "ui/camera_raw_scope.h"
#include "ui/i18n.h"
#include "ui/rgb_histogram.h"
#include "ui/widgets.h"

#include "algo/histogram.h"
#include "algo/vectorscope.h"
#include "base/hash.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "cms/transform.h"
#include "color/color.h"
#include "compose/compose.h"
#include "raster/surface.h"

/* ---- bounded singleton caches (one open dialog at a time) ---- */
static struct {
    bool valid;
    uint64_t doc, revision, generation;
    bool before;
    pc_compose_buffer px;
    uint32_t w, h;
    pc_algo_rgb_histogram hist;
} g_proxy;

static struct {
    bool valid;
    uint64_t revision;
    bool before, selected;
    pc_algo_vectorscope scope;
} g_vscope;

static struct {
    bool valid;
    uint64_t doc;
    pc_color_mode mode;
    pc_transform *lab, *scope;
} g_xf;

/* Transient per-frame gesture state (single dialog). */
static struct {
    bool has_sample;
    float sample[2];
    bool dragging_sampler;
    size_t sampler_i;
    bool tone_dragging;
    pc_ui_cr_tone_zone tone_zone;
    float tone_start_x, tone_start_val;
} g_rt;

void pc_ui_shell_cr_scope_fini(void)
{
    pc_compose_buffer_fini(&g_proxy.px);
    pc_transform_release(g_xf.lab);
    pc_transform_release(g_xf.scope);
    memset(&g_proxy, 0, sizeof g_proxy);
    memset(&g_vscope, 0, sizeof g_vscope);
    memset(&g_xf, 0, sizeof g_xf);
}

/* ---- helpers ---- */
static uint32_t cs_space_of(pc_color_mode mode)
{
    switch (mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        return PC_CS_GRAY;
    case PC_COLOR_MODE_CMYK:
        return PC_CS_CMYK;
    case PC_COLOR_MODE_LAB:
        return PC_CS_LAB;
    default:
        return PC_CS_RGB;
    }
}
static bool doc_source_profile(const pc_document *doc, pc_profile *out)
{
    uint32_t space = cs_space_of(doc->mode);
    if (doc->icc_profile != NULL) {
        const uint8_t *data = pc_blob_data(doc->icc_profile);
        size_t len = pc_blob_len(doc->icc_profile);
        pc_profile emb;
        if (data && len && pc_profile_parse(data, len, &emb, NULL) == PC_OK) {
            if (emb.space == space) {
                *out = emb;
                return true;
            }
            pc_profile_fini(&emb);
        }
    }
    const pc_profile *builtin = pc_builtin_default_for(space);
    if (!builtin)
        builtin = pc_builtin_profile(PC_BUILTIN_SRGB);
    return builtin && pc_profile_clone(builtin, out, NULL) == PC_OK;
}
static void ensure_transforms(pc_ui_shell_app *app)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state)
        return;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (g_xf.valid && g_xf.doc == doc->id && g_xf.mode == doc->mode)
        return;
    pc_transform_release(g_xf.lab);
    pc_transform_release(g_xf.scope);
    g_xf.lab = g_xf.scope = NULL;
    g_xf.valid = false;
    pc_profile src;
    if (!doc_source_profile(doc, &src))
        return;
    const pc_profile *lab = pc_builtin_profile(PC_BUILTIN_LAB_D50), *srgb = pc_builtin_profile(PC_BUILTIN_SRGB);
    pc_transform_options opts = pc_transform_options_default();
    bool ok = lab && srgb && pc_transform_cached(&src, lab, opts, &g_xf.lab, NULL) == PC_OK &&
              pc_transform_cached(&src, srgb, opts, &g_xf.scope, NULL) == PC_OK;
    pc_profile_fini(&src);
    g_xf.valid = ok;
    g_xf.doc = doc->id;
    g_xf.mode = doc->mode;
    if (!ok) {
        pc_transform_release(g_xf.lab);
        pc_transform_release(g_xf.scope);
        g_xf.lab = g_xf.scope = NULL;
    }
}
static const pc_document *proxy_doc(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, bool before)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state)
        return NULL;
    if (!before && r->preview.result)
        return pc_doc_arc_get(r->preview.result);
    return pc_doc_arc_get(state->doc);
}
static void ensure_proxy(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, bool before)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state)
        return;
    const pc_document *doc = proxy_doc(app, r, before);
    if (!doc)
        return;
    uint64_t id = pc_doc_arc_get(state->doc)->id, revision = state->revision,
             generation = r->preview.generation;
    if (g_proxy.valid && g_proxy.doc == id && g_proxy.revision == revision &&
        g_proxy.generation == generation && g_proxy.before == before)
        return;
    pc_compose_buffer_fini(&g_proxy.px);
    memset(&g_proxy, 0, sizeof g_proxy);
    pc_compose_buffer buf = {0};
    if (pc_compose_thumbnail_buffer(doc, 512, &buf, NULL) != PC_OK)
        return;
    int64_t w = (int64_t)buf.rect.x1 - buf.rect.x0, h = (int64_t)buf.rect.y1 - buf.rect.y0;
    if (w <= 0 || h <= 0 || w > 512 || h > 512 || (size_t)(w * h) != buf.len) {
        pc_compose_buffer_fini(&buf);
        return;
    }
    g_proxy.px = buf;
    g_proxy.w = (uint32_t)w;
    g_proxy.h = (uint32_t)h;
    g_proxy.valid = true;
    g_proxy.doc = id;
    g_proxy.revision = revision;
    g_proxy.generation = generation;
    g_proxy.before = before;
    pc_algo_rgb_histogram_from_rgba((const float (*)[4])g_proxy.px.px, g_proxy.px.len, &g_proxy.hist);
}
static bool proxy_pixel(float u, float v, float out[4])
{
    if (!g_proxy.valid || g_proxy.w == 0 || g_proxy.h == 0 || !isfinite(u) || !isfinite(v) || u < 0 ||
        u > 1 || v < 0 || v > 1)
        return false;
    size_t x = (size_t)(u * (float)g_proxy.w), y = (size_t)(v * (float)g_proxy.h);
    if (x >= g_proxy.w)
        x = g_proxy.w - 1;
    if (y >= g_proxy.h)
        y = g_proxy.h - 1;
    size_t i = y * g_proxy.w + x;
    if (i >= g_proxy.px.len)
        return false;
    const float *p = g_proxy.px.px[i];
    if (!(p[3] > 0.0f) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
        return false;
    for (size_t c = 0; c < 4; c++)
        out[c] = p[c];
    return true;
}
static void ensure_vscope(pc_ui_shell_app *app, bool before, bool selected)
{
    if (!g_proxy.valid || !g_xf.valid)
        return;
    if (g_vscope.valid && g_vscope.revision == g_proxy.revision && g_vscope.before == before &&
        g_vscope.selected == selected)
        return;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    size_t total = g_proxy.px.len;
    /* A fixed 2-D grid bounds analysis work independently of image size. */
    double ratio = (double)total / (double)PC_UI_CR_SCOPE_SAMPLES;
    size_t step = ratio > 1.0 ? (size_t)ceil(sqrt(ratio)) : 1;
    if (step == 0)
        step = 1;
    float (*samples)[4] = malloc((size_t)(((g_proxy.h + step - 1) / step) * ((g_proxy.w + step - 1) / step)) *
                                 sizeof *samples);
    size_t n = 0;
    if (samples) {
        for (size_t y = 0; y < g_proxy.h; y += step)
            for (size_t x = 0; x < g_proxy.w; x += step) {
                size_t i = y * g_proxy.w + x;
                const float *p = g_proxy.px.px[i];
                if (!(p[3] > 0.0f) || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
                    continue;
                if (selected && doc && doc->has_selection) {
                    int32_t dx = (int32_t)((float)x / (float)g_proxy.w * (float)doc->size.width),
                            dy = (int32_t)((float)y / (float)g_proxy.h * (float)doc->size.height);
                    if (!(pc_surface_sample_channel(&doc->selection, dx, dy, 0) > 0.0f))
                        continue;
                }
                float rgb[3];
                pc_transform_eval(g_xf.scope, p, rgb);
                samples[n][0] = rgb[0];
                samples[n][1] = rgb[1];
                samples[n][2] = rgb[2];
                samples[n][3] = p[3];
                n++;
            }
    }
    pc_algo_vectorscope_from_rgba(n ? (const float (*)[4])samples : NULL, n, &g_vscope.scope);
    free(samples);
    g_vscope.valid = true;
    g_vscope.revision = g_proxy.revision;
    g_vscope.before = before;
    g_vscope.selected = selected;
}

bool pc_ui_shell_cr_show_before(pc_ui_dialog *d)
{
    return yyjson_mut_get_bool(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d->fields), "__cameraBefore"));
}
static void set_before(pc_ui_dialog *d, bool before)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    yyjson_mut_val *k = yyjson_mut_strcpy(d->fields, "__cameraBefore"),
                   *v = yyjson_mut_bool(d->fields, before);
    if (k && v)
        (void)yyjson_mut_obj_put(root, k, v);
}

/* ---- readout text for one normalised point ---- */
static void readout_for(pc_ui_dialog *d, bool lab, float u, float v, char *out, size_t n)
{
    float rgba[4];
    if (!proxy_pixel(u, v, rgba)) {
        (void)snprintf(out, n, "%s", "\xE2\x80\x94"); /* em dash */
        return;
    }
    double values[3];
    if (lab && g_xf.valid && g_xf.lab) {
        float l[3];
        pc_transform_eval(g_xf.lab, rgba, l);
        pc_ui_cr_readout_lab(l, values);
    } else {
        pc_ui_cr_readout_rgb(rgba, values);
    }
    pc_ui_cr_readout_text(values, lab && g_xf.valid, out, n);
    (void)d;
}

/* ---- display + overlay bitmaps ---- */
static struct nk_image before_bitmap(pc_ui_shell_app *app, bool *ok)
{
    struct nk_image image = {0};
    *ok = false;
    if (!g_proxy.valid)
        return image;
    uint64_t scope[] = {g_proxy.doc, g_proxy.revision, g_proxy.generation, 1 /*before*/, g_proxy.w,
                        g_proxy.h};
    uint8_t key[32];
    pc_blake3(scope, sizeof scope, key);
    if (pc_ui_shell_bitmap(app, key, (uint16_t)g_proxy.w, (uint16_t)g_proxy.h, NULL, &image, NULL) == PC_OK) {
        *ok = true;
        return image;
    }
    size_t count = (size_t)g_proxy.w * g_proxy.h;
    uint8_t *rgba = malloc(count * 4);
    if (!rgba)
        return image;
    for (size_t i = 0; i < count && i < g_proxy.px.len; i++) {
        const float *p = g_proxy.px.px[i];
        float a = isfinite(p[3]) ? p[3] : 0.0f;
        for (size_t c = 0; c < 3; c++) {
            float v = isfinite(p[c]) ? p[c] : 0.0f;
            rgba[i * 4 + c] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
        }
        rgba[i * 4 + 3] = (uint8_t)(fminf(fmaxf(a, 0.0f), 1.0f) * 255.0f + 0.5f);
    }
    *ok = pc_ui_shell_bitmap(app, key, (uint16_t)g_proxy.w, (uint16_t)g_proxy.h, rgba, &image, NULL) == PC_OK;
    free(rgba);
    return image;
}
static struct nk_image clip_bitmap(pc_ui_shell_app *app, const pc_ui_tokens *t, pc_ui_cr_clip_mode mode,
                                   uint8_t warn, bool *ok)
{
    struct nk_image image = {0};
    *ok = false;
    if (!g_proxy.valid || mode == PC_UI_CR_CLIP_NONE)
        return image;
    uint64_t scope[] = {g_proxy.doc, g_proxy.revision, g_proxy.generation, 2 /*overlay*/,
                        (uint64_t)mode << 8 | warn, g_proxy.w, g_proxy.h, t->histogram_level};
    uint8_t key[32];
    pc_blake3(scope, sizeof scope, key);
    if (pc_ui_shell_bitmap(app, key, (uint16_t)g_proxy.w, (uint16_t)g_proxy.h, NULL, &image, NULL) == PC_OK) {
        *ok = true;
        return image;
    }
    size_t count = (size_t)g_proxy.w * g_proxy.h;
    uint8_t *rgba = calloc(count, 4);
    if (!rgba)
        return image;
    for (size_t i = 0; i < count && i < g_proxy.px.len; i++) {
        pc_ui_color c;
        if (pc_ui_cr_clip_color(g_proxy.px.px[i], mode, warn, t, &c)) {
            rgba[i * 4 + 0] = c.r;
            rgba[i * 4 + 1] = c.g;
            rgba[i * 4 + 2] = c.b;
            rgba[i * 4 + 3] = 255;
        }
    }
    *ok = pc_ui_shell_bitmap(app, key, (uint16_t)g_proxy.w, (uint16_t)g_proxy.h, rgba, &image, NULL) == PC_OK;
    free(rgba);
    return image;
}

/* ---- preview ---- */
void pc_ui_shell_cr_preview(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d,
                            struct nk_rect box, enum nk_widget_layout_states layout)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_camera_raw_scope *view = &ui->camera_raw_scope;
    pc_ui_camera_raw_preview *nav = &ui->camera_raw_preview;
    pc_ui_cr_scope_sanitize(view);
    bool before = pc_ui_shell_cr_show_before(d);
    ensure_transforms(app);
    ensure_proxy(app, r, before);

    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    if (!doc || doc->size.width == 0 || doc->size.height == 0)
        return;
    float viewport[4] = {box.x, box.y, box.w, box.h};
    float size[2] = {(float)doc->size.width, (float)doc->size.height};
    float rect[4];
    pc_ui_cr_preview_image_rect(nav, viewport, size, rect);

    /* Checkerboard behind the image. */
    nk_fill_rect(canvas, box, 0, pc_ui_nk_color(t->field));
    struct nk_rect saved = canvas->clip;
    nk_push_scissor(canvas, box);
    const float cell = 8.0f;
    for (float y = box.y; y < box.y + box.h; y += cell)
        for (float x = box.x; x < box.x + box.w; x += cell) {
            int odd = (((int)((x - box.x) / cell)) ^ ((int)((y - box.y) / cell))) & 1;
            struct nk_color c = odd ? nk_rgb(90, 90, 90) : nk_rgb(120, 120, 120);
            nk_fill_rect(canvas, nk_rect(x, y, cell, cell), 0, c);
        }
    struct nk_rect image_rect = nk_rect(rect[0], rect[1], rect[2], rect[3]);
    bool shown = false;
    if (before) {
        struct nk_image bmp = before_bitmap(app, &shown);
        if (shown)
            nk_draw_image(canvas, image_rect, &bmp, nk_rgba(255, 255, 255, 255));
    }
    if (!shown) {
        struct nk_image after;
        float aspect;
        if (pc_ui_shell_canvas_preview(app, &after, &aspect))
            nk_draw_image(canvas, image_rect, &after, nk_rgba(255, 255, 255, 255));
    }

    /* Clipping overlay: warnings, or Alt tone diagnostics while dragging a shadow/highlight zone. */
    pc_ui_cr_clip_mode mode = PC_UI_CR_CLIP_NONE;
    uint8_t warn = 0;
    unsigned modifiers = pc_ui_shell_input_modifiers(app);
    if (g_rt.tone_dragging && (modifiers & PC_UI_MOD_ALT)) {
        mode = (g_rt.tone_zone == PC_UI_CR_ZONE_BLACKS || g_rt.tone_zone == PC_UI_CR_ZONE_SHADOWS)
                   ? PC_UI_CR_CLIP_SHADOW_CHANNELS
                   : PC_UI_CR_CLIP_HIGHLIGHT_CHANNELS;
    } else if (view->shadows || view->highlights) {
        mode = PC_UI_CR_CLIP_WARNINGS;
        warn = (uint8_t)((view->shadows ? 1u : 0u) | (view->highlights ? 2u : 0u));
    }
    if (mode != PC_UI_CR_CLIP_NONE) {
        bool ok = false;
        struct nk_image overlay = clip_bitmap(app, t, mode, warn, &ok);
        if (ok)
            nk_draw_image(canvas, image_rect, &overlay, nk_rgba(255, 255, 255, 255));
    }

    /* Pointer sample (for the hover badge / vectorscope target). */
    bool hover = layout == NK_WIDGET_VALID && nk_input_is_mouse_hovering_rect(&ctx->input, box);
    g_rt.has_sample = false;
    if (hover) {
        g_rt.sample[0] = fminf(fmaxf((ctx->input.mouse.pos.x - box.x) / fmaxf(box.w, 1.0f), 0.0f), 1.0f);
        g_rt.sample[1] = fminf(fmaxf((ctx->input.mouse.pos.y - box.y) / fmaxf(box.h, 1.0f), 0.0f), 1.0f);
        g_rt.has_sample = true;
    }

    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT],
                                 *middle = &ctx->input.mouse.buttons[NK_BUTTON_MIDDLE];
    bool panning = (nav->hand && !view->sampler_tool) || (middle->down && hover);

    /* Wheel zoom at the pointer. */
    if (hover && ctx->input.mouse.scroll_delta.y != 0.0f) {
        float at[2] = {ctx->input.mouse.pos.x, ctx->input.mouse.pos.y};
        float factor = ctx->input.mouse.scroll_delta.y > 0 ? 1.1f : 1.0f / 1.1f;
        pc_ui_cr_preview_zoom_at(nav, pc_ui_cr_preview_scale(nav, viewport, size) * factor, at, viewport, size);
    }

    pc_ui_shell_widget_mapping(app, "camera.preview", box);
    if (view->sampler_tool && !panning) {
        float p[2] = {g_rt.sample[0], g_rt.sample[1]};
        if (hover && left->clicked && left->down && !r->graph_blocked) {
            /* nearest existing probe within ~12px */
            int near = -1;
            for (size_t i = 0; i < view->n_samplers; i++) {
                float sx = box.x + view->samplers[i][0] * box.w, sy = box.y + view->samplers[i][1] * box.h;
                float qx = box.x + p[0] * box.w, qy = box.y + p[1] * box.h;
                if ((sx - qx) * (sx - qx) + (sy - qy) * (sy - qy) < 12.0f * 12.0f) {
                    near = (int)i;
                    break;
                }
            }
            if (modifiers & PC_UI_MOD_ALT) {
                if (near >= 0)
                    pc_ui_cr_scope_remove_sampler(view, (size_t)near);
            } else if (near < 0) {
                pc_ui_cr_scope_add_sampler(view, p);
            }
        }
        if (left->down && g_rt.dragging_sampler)
            pc_ui_cr_scope_move_sampler(view, g_rt.sampler_i, p);
        else
            g_rt.dragging_sampler = false;
        if (hover && left->clicked && left->down && !g_rt.dragging_sampler) {
            for (size_t i = 0; i < view->n_samplers; i++) {
                float sx = box.x + view->samplers[i][0] * box.w, sy = box.y + view->samplers[i][1] * box.h;
                float qx = box.x + p[0] * box.w, qy = box.y + p[1] * box.h;
                if ((sx - qx) * (sx - qx) + (sy - qy) * (sy - qy) < 16.0f * 16.0f) {
                    g_rt.dragging_sampler = true;
                    g_rt.sampler_i = i;
                    break;
                }
            }
        }
    } else if (panning && left->down && hover) {
        float delta[2] = {ctx->input.mouse.delta.x, ctx->input.mouse.delta.y};
        pc_ui_cr_preview_pan(nav, delta, viewport, size);
    } else if (!panning && !view->sampler_tool && hover && left->clicked && left->down &&
               !r->graph_blocked) {
        /* Zoom tool: Alt = out, otherwise fit<->100%. */
        float at[2] = {ctx->input.mouse.pos.x, ctx->input.mouse.pos.y};
        if (modifiers & PC_UI_MOD_ALT)
            pc_ui_cr_preview_zoom_at(nav, pc_ui_cr_preview_scale(nav, viewport, size) / 1.25f, at, viewport, size);
        else if (!nav->has_zoom)
            pc_ui_cr_preview_zoom_at(nav, 1.0f, at, viewport, size);
        else
            pc_ui_cr_preview_fit(nav);
    }
    if (!left->down)
        g_rt.dragging_sampler = false;

    /* Colour sampler probes and their readouts. */
    if (view->n_samplers > 0) {
        float panel_w = fminf(box.w, 330.0f);
        float panel_h = (float)view->n_samplers * 16.0f + 12.0f;
        nk_fill_rect(canvas, nk_rect(box.x, box.y, panel_w, panel_h), 0,
                     nk_rgba(t->dock.r, t->dock.g, t->dock.b, 235));
    }
    for (size_t i = 0; i < view->n_samplers; i++) {
        float qx = box.x + view->samplers[i][0] * box.w, qy = box.y + view->samplers[i][1] * box.h;
        struct nk_color ink = pc_ui_nk_color(t->text);
        nk_stroke_circle(canvas, nk_rect(qx - 5, qy - 5, 10, 10), 1, ink);
        nk_stroke_line(canvas, qx - 8, qy, qx + 8, qy, 1, ink);
        nk_stroke_line(canvas, qx, qy - 8, qx, qy + 8, 1, ink);
        char text[96], label[16];
        (void)snprintf(label, sizeof label, "%u", (unsigned)(i + 1));
        char readout[64];
        readout_for(d, view->lab, view->samplers[i][0], view->samplers[i][1], readout, sizeof readout);
        (void)snprintf(text, sizeof text, "%zu  %s", i + 1, readout);
        nk_draw_text(canvas, nk_rect(box.x + 6, box.y + 6 + (float)i * 16.0f, box.w - 12, 14), text,
                     (int)strlen(text), ctx->style.font, pc_ui_nk_color(t->dock), ink);
        nk_draw_text(canvas, nk_rect(qx + 7, qy + 7, 20, 14), label, (int)strlen(label), ctx->style.font,
                     nk_rgba(0, 0, 0, 0), ink);
    }
    nk_push_scissor(canvas, saved);
}

/* ---- histogram with clipping warnings + tone drag ---- */
static void warning_triangle(struct nk_command_buffer *canvas, const pc_ui_tokens *t, struct nk_rect box,
                             uint8_t mask, bool on)
{
    struct nk_rect r = nk_rect(box.x + 3, box.y + 3, box.w - 6, box.h - 6);
    pc_ui_color fill = mask == 0 ? t->histogram_bg : pc_ui_tokens_histogram_color(t, mask);
    nk_fill_triangle(canvas, r.x + r.w / 2, r.y, r.x, r.y + r.h, r.x + r.w, r.y + r.h,
                     pc_ui_nk_color(fill));
    struct nk_color stroke = pc_ui_nk_color(on ? t->text : t->text_faint);
    nk_stroke_triangle(canvas, r.x + r.w / 2, r.y, r.x, r.y + r.h, r.x + r.w, r.y + r.h, 1, stroke);
}
void pc_ui_shell_cr_histogram(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d,
                              struct nk_rect graph)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_camera_raw_scope *view = &ui->camera_raw_scope;
    bool before = pc_ui_shell_cr_show_before(d);
    ensure_proxy(app, r, before);

    const float corner = 18.0f;
    struct nk_rect plot = nk_rect(graph.x + 4, graph.y + 4, graph.w - 8, graph.h - 8);
    nk_fill_rect(canvas, graph, 0, pc_ui_nk_color(t->histogram_bg));

    /* Tone-zone drag + hover highlight band. */
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    bool hover = nk_input_is_mouse_hovering_rect(&ctx->input, graph);
    float mx = ctx->input.mouse.pos.x;
    pc_ui_cr_tone_zone zone = g_rt.tone_dragging ? g_rt.tone_zone
                              : hover && ctx->input.mouse.pos.y > graph.y + corner
                                  ? pc_ui_cr_tone_zone_at(fminf(fmaxf((mx - plot.x) / fmaxf(plot.w, 1.0f), 0.0f), 1.0f))
                                  : (pc_ui_cr_tone_zone)-1;
    if ((int)zone >= 0) {
        float rr[2];
        pc_ui_cr_tone_zone_range(zone, rr);
        struct nk_rect band = nk_rect(plot.x + rr[0] * plot.w, plot.y, (rr[1] - rr[0]) * plot.w, plot.h);
        nk_fill_rect(canvas, band, 0, nk_rgba(t->text.r, t->text.g, t->text.b, 14));
        const char *key = pc_ui_cr_tone_zone_key(zone);
        double value = yyjson_mut_get_num(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d->fields), key));
        if (hover && left->clicked && left->down && !r->graph_blocked &&
            ctx->input.mouse.pos.y > graph.y + corner) {
            g_rt.tone_dragging = true;
            g_rt.tone_zone = zone;
            g_rt.tone_start_x = mx;
            g_rt.tone_start_val = (float)value;
        }
    }
    if (g_rt.tone_dragging && left->down) {
        float delta = (mx - g_rt.tone_start_x) / fmaxf(plot.w, 1.0f) * pc_ui_cr_tone_zone_limit(g_rt.tone_zone) * 2.0f;
        float next = pc_ui_cr_tone_zone_clamp(g_rt.tone_zone, g_rt.tone_start_val + delta);
        yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields),
                       *k = yyjson_mut_strcpy(d->fields, pc_ui_cr_tone_zone_key(g_rt.tone_zone)),
                       *v = yyjson_mut_real(d->fields, next);
        if (k && v)
            (void)yyjson_mut_obj_put(root, k, v);
    }
    if (!left->down)
        g_rt.tone_dragging = false;

    /* Ribbon from the proxy histogram (before/after). */
    if (g_proxy.valid)
        pc_ui_shell_rgb_histogram_paint(ctx, plot, &g_proxy.hist, t);

    /* Hover RGB badge. */
    if (g_rt.has_sample) {
        float rgba[4];
        if (proxy_pixel(g_rt.sample[0], g_rt.sample[1], rgba)) {
            double vals[3];
            pc_ui_cr_readout_rgb(rgba, vals);
            float mark = pc_ui_rgb_histogram_sample_x(plot.x, plot.x + plot.w, rgba);
            if (mark >= 0)
                nk_stroke_line(canvas, mark, plot.y, mark, plot.y + plot.h, 1, pc_ui_nk_color(t->text_dim));
            char text[48];
            (void)snprintf(text, sizeof text, "R %.0f   G %.0f   B %.0f", vals[0], vals[1], vals[2]);
            struct nk_rect badge = nk_rect(graph.x + graph.w / 2 - 70, graph.y + 3, 140, 18);
            nk_fill_rect(canvas, badge, 2, nk_rgba(t->card.r, t->card.g, t->card.b, 170));
            nk_draw_text(canvas, badge, text, (int)strlen(text), ctx->style.font, nk_rgba(0, 0, 0, 0),
                         pc_ui_nk_color(t->text));
        }
    }

    /* Clipping-warning triangles (clickable). */
    uint8_t sh = g_proxy.valid ? pc_ui_cr_shadow_mask(&g_proxy.hist) : 0,
            hi = g_proxy.valid ? pc_ui_cr_highlight_mask(&g_proxy.hist) : 0;
    struct nk_rect sh_box = nk_rect(graph.x, graph.y, corner, corner),
                   hi_box = nk_rect(graph.x + graph.w - corner, graph.y, corner, corner);
    warning_triangle(canvas, t, sh_box, sh, view->shadows);
    warning_triangle(canvas, t, hi_box, hi, view->highlights);
    pc_ui_shell_widget_mapping(app, "camera.clip.shadow", sh_box);
    pc_ui_shell_widget_mapping(app, "camera.clip.highlight", hi_box);
    if (left->clicked && left->down && !r->graph_blocked) {
        if (nk_input_is_mouse_hovering_rect(&ctx->input, sh_box))
            view->shadows = !view->shadows;
        else if (nk_input_is_mouse_hovering_rect(&ctx->input, hi_box))
            view->highlights = !view->highlights;
    }
    nk_stroke_rect(canvas, graph, 0, 1, pc_ui_nk_color(t->field_border));
}

/* ---- vectorscope ---- */
void pc_ui_shell_cr_vectorscope(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d,
                                struct nk_rect box)
{
    (void)r;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_camera_raw_scope *view = &ui->camera_raw_scope;
    bool before = pc_ui_shell_cr_show_before(d);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    bool has_selection = state && pc_doc_arc_get(state->doc)->has_selection;
    ensure_vscope(app, before, view->selected_region && has_selection);

    nk_fill_rect(canvas, box, 0, pc_ui_nk_color(t->histogram_bg));
    float rect[4] = {box.x, box.y, box.w, box.h};
    float centre[2] = {box.x + box.w / 2, box.y + box.h / 2};
    float radius = pc_ui_cr_scope_radius(rect);
    struct nk_color faint = nk_rgba(t->text_faint.r, t->text_faint.g, t->text_faint.b, 115);
    for (size_t ring = 1; ring <= 4; ring++)
        nk_stroke_circle(canvas, nk_rect(centre[0] - radius * (float)ring / 4, centre[1] - radius * (float)ring / 4,
                                         radius * (float)ring / 2, radius * (float)ring / 2),
                         1, faint);
    /* Cloud. */
    if (g_vscope.valid) {
        uint32_t peak = 1;
        for (size_t i = 0; i < PC_ALGO_VECTORSCOPE_HUES * PC_ALGO_VECTORSCOPE_SATURATIONS; i++)
            if (g_vscope.scope.counts[i] > peak)
                peak = g_vscope.scope.counts[i];
        float dot = fminf(fmaxf(radius / (float)(PC_ALGO_VECTORSCOPE_SATURATIONS - 1), 0.7f), 3.0f);
        for (size_t i = 0; i < PC_ALGO_VECTORSCOPE_HUES * PC_ALGO_VECTORSCOPE_SATURATIONS; i++) {
            uint32_t count = g_vscope.scope.counts[i];
            if (!count)
                continue;
            float h = (float)(i / PC_ALGO_VECTORSCOPE_SATURATIONS) / (float)PC_ALGO_VECTORSCOPE_HUES;
            float s = (float)(i % PC_ALGO_VECTORSCOPE_SATURATIONS) / (float)(PC_ALGO_VECTORSCOPE_SATURATIONS - 1);
            float p[2];
            pc_ui_cr_scope_point(rect, view->red_right, h, s, p);
            pc_ui_color col = pc_ui_cr_scope_hue(t, h, s);
            float weight = 0.2f + 0.8f * sqrtf((float)count / (float)peak);
            col.a = (uint8_t)(255.0f * fminf(fmaxf(weight, 0.0f), 1.0f));
            nk_fill_rect(canvas, nk_rect(p[0] - dot, p[1] - dot, dot * 2, dot * 2), 0, pc_ui_nk_color(col));
        }
    }
    /* Spokes + primary/secondary labels. */
    const char *labels[6] = {"R", "Y", "G", "C", "B", "M"};
    for (size_t i = 0; i < 6; i++) {
        float h = (float)i / 6.0f, p[2], lp[2];
        pc_ui_cr_scope_point(rect, view->red_right, h, 1.0f, p);
        pc_ui_cr_scope_point(rect, view->red_right, h, 1.11f, lp);
        nk_stroke_line(canvas, centre[0], centre[1], p[0], p[1], 1, faint);
        pc_ui_color col = pc_ui_cr_scope_hue(t, h, 1.0f);
        nk_fill_circle(canvas, nk_rect(p[0] - 2, p[1] - 2, 4, 4), pc_ui_nk_color(col));
        nk_draw_text(canvas, nk_rect(lp[0] - 6, lp[1] - 7, 14, 14), labels[i], 1, ctx->style.font,
                     nk_rgba(0, 0, 0, 0), pc_ui_nk_color(col));
    }
    if (!view->hide_skin_line) {
        float p[2];
        pc_ui_cr_scope_point(rect, view->red_right, 23.0f / 360.0f, 1.0f, p);
        nk_stroke_line(canvas, centre[0], centre[1], p[0], p[1], 1, pc_ui_nk_color(t->text_dim));
    }
    /* Hover sample target. */
    if (g_rt.has_sample && g_xf.valid && g_xf.scope) {
        float rgba[4];
        if (proxy_pixel(g_rt.sample[0], g_rt.sample[1], rgba)) {
            float rgb[3];
            pc_transform_eval(g_xf.scope, rgba, rgb);
            if (isfinite(rgb[0]) && isfinite(rgb[1]) && isfinite(rgb[2])) {
                float clamped[3] = {fminf(fmaxf(rgb[0], 0.0f), 1.0f), fminf(fmaxf(rgb[1], 0.0f), 1.0f),
                                    fminf(fmaxf(rgb[2], 0.0f), 1.0f)};
                float hue, sat;
                pc_algo_vectorscope_hue_saturation(clamped, &hue, &sat);
                float at[2];
                pc_ui_cr_scope_point(rect, view->red_right, hue, sat, at);
                struct nk_color mark = pc_ui_nk_color(pc_ui_tokens_histogram_color(t, 7));
                nk_stroke_circle(canvas, nk_rect(at[0] - 5, at[1] - 5, 10, 10), 1, mark);
                nk_stroke_line(canvas, at[0] - 9, at[1], at[0] + 9, at[1], 1, mark);
                nk_stroke_line(canvas, at[0], at[1] - 9, at[0], at[1] + 9, 1, mark);
            }
        }
    }
    nk_stroke_rect(canvas, box, 0, 1, pc_ui_nk_color(t->field_border));
}

/* ---- toolbar ---- */
void pc_ui_shell_cr_toolbar(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    (void)r;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_camera_raw_scope *view = &ui->camera_raw_scope;
    pc_ui_camera_raw_preview *nav = &ui->camera_raw_preview;
    nk_layout_row_dynamic(ctx, 26, 5);
    pc_ui_shell_widget_mapping(app, "camera.tool.zoom", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Zoom")))
        nav->hand = false;
    pc_ui_shell_widget_mapping(app, "camera.tool.hand", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Hand")))
        nav->hand = true;
    pc_ui_shell_widget_mapping(app, "camera.tool.fit", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Fit")))
        pc_ui_cr_preview_fit(nav);
    pc_ui_shell_widget_mapping(app, "camera.tool.hundred", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("100%"))) {
        nav->has_zoom = true;
        nav->zoom = 1.0f;
    }
    nk_bool sampler = view->sampler_tool;
    pc_ui_shell_widget_mapping(app, "camera.tool.sampler", nk_widget_bounds(ctx));
    if (nk_checkbox_label(ctx, pc_i18n_t("Sampler"), &sampler))
        view->sampler_tool = sampler != 0;
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_bool before = pc_ui_shell_cr_show_before(d);
    pc_ui_shell_widget_mapping(app, "camera.before", nk_widget_bounds(ctx));
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Before"), &before))
        set_before(d, before != 0);
    pc_ui_shell_widget_mapping(app, "camera.clear.samplers", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Clear Samplers")))
        pc_ui_cr_scope_clear_samplers(view);
}

/* ---- scope display options ---- */
void pc_ui_shell_cr_scope_options(pc_ui_shell_app *app, pc_ui_dialog *d)
{
    (void)d;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_camera_raw_scope *view = &ui->camera_raw_scope;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    bool has_selection = state && pc_doc_arc_get(state->doc)->has_selection;
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_bool lab = view->lab, vs = view->vectorscope;
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Lab Color Readouts"), &lab))
        view->lab = lab != 0;
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Vectorscope"), &vs))
        view->vectorscope = vs != 0;
    if (view->vectorscope) {
        nk_layout_row_dynamic(ctx, 24, 2);
        if (has_selection) {
            nk_bool sel = view->selected_region;
            if (nk_checkbox_label(ctx, pc_i18n_t("Show Selected Region"), &sel))
                view->selected_region = sel != 0;
        }
        nk_bool rr = view->red_right;
        if (nk_checkbox_label(ctx, pc_i18n_t("Show Red at 3 o'clock"), &rr))
            view->red_right = rr != 0;
        nk_bool skin = !view->hide_skin_line;
        if (nk_checkbox_label(ctx, pc_i18n_t("Show Skin Tone Indicator"), &skin))
            view->hide_skin_line = skin == 0;
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_bool floating = view->floating;
    if (nk_checkbox_label(ctx, pc_i18n_t("Floating Scopes Panel"), &floating))
        view->floating = floating != 0;
}
