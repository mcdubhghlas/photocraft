#include "web_export_ui.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "shell_internal.h"
#include "ui/i18n.h"
#include "ui/sizing.h"
#include "ui/web_export.h"
#include "base/blob.h"
#include "compose/compose.h"
#include "engine/engine.h"
#include "engine/web_cmds.h"

#define WEB_COMMAND "file.export.saveForWebLegacy"

static void put(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key, yyjson_mut_val *value)
{
    if (!value) {
        return;
    }
    (void)yyjson_mut_obj_remove_str(object, key);
    (void)yyjson_mut_obj_add_val(doc, object, key, value);
}

static const char *field_str(yyjson_mut_val *root, const char *key, const char *def)
{
    const char *v = yyjson_mut_get_str(yyjson_mut_obj_get(root, key));
    return v ? v : def;
}

static double field_num(yyjson_mut_val *root, const char *key, double def)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
    if (v && yyjson_mut_is_num(v)) {
        double n = yyjson_mut_get_num(v);
        if (isfinite(n)) {
            return n;
        }
    }
    return def;
}

static bool field_bool(yyjson_mut_val *root, const char *key, bool def)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
    return v ? yyjson_mut_get_bool(v) : def;
}

pc_status pc_ui_shell_web_export_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    pc_engine_session *s = pc_ui_shell_app_session(app);
    if (!pc_engine_session_active(s)) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "web dialog");
    }
    bool ok = yyjson_mut_obj_add_strcpy(d, root, "__command", WEB_COMMAND) &&
              yyjson_mut_obj_add_strcpy(d, root, "__label", "Save for Web (Legacy)") &&
              yyjson_mut_obj_add_bool(d, root, "__web", true) &&
              yyjson_mut_obj_add_bool(d, root, "__export", true) &&
              yyjson_mut_obj_add_strcpy(d, root, "__view", "2up") &&
              yyjson_mut_obj_add_strcpy(d, root, "format", "jpeg") &&
              yyjson_mut_obj_add_int(d, root, "quality", 60) &&
              yyjson_mut_obj_add_strcpy(d, root, "palette", "selective") &&
              yyjson_mut_obj_add_int(d, root, "colors", 128) &&
              yyjson_mut_obj_add_strcpy(d, root, "dither", "diffusion") &&
              yyjson_mut_obj_add_int(d, root, "ditherAmount", 88) &&
              yyjson_mut_obj_add_bool(d, root, "transparency", true) &&
              yyjson_mut_obj_add_strcpy(d, root, "matte", "#ffffff") &&
              yyjson_mut_obj_add_bool(d, root, "interlaced", false) &&
              yyjson_mut_obj_add_bool(d, root, "progressive", false) &&
              yyjson_mut_obj_add_bool(d, root, "optimized", true) &&
              yyjson_mut_obj_add_bool(d, root, "embedIcc", false) &&
              yyjson_mut_obj_add_bool(d, root, "convertToSrgb", true) &&
              yyjson_mut_obj_add_strcpy(d, root, "metadata", "copyright") &&
              yyjson_mut_obj_add_real(d, root, "percent", 100.0) &&
              yyjson_mut_obj_add_int(d, root, "webSnap", 0);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "web dialog");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}

/* ---- settings widgets ---- */

static void combo_field(struct nk_context *ctx, pc_ui_dialog *dialog, const char *key, const char *label,
                        const char *const *ids, const char *const *labels, int n, const char *def)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *value = field_str(root, key, def);
    int selected = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(value, ids[i])) {
            selected = i;
        }
    }
    const char *shown[16];
    int count = n > 16 ? 16 : n;
    for (int i = 0; i < count; i++) {
        shown[i] = pc_i18n_t(labels[i]);
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    int next = nk_combo(ctx, shown, count, selected, 24, nk_vec2(170, 260));
    if (next != selected && next >= 0 && next < count) {
        put(dialog->fields, root, key, yyjson_mut_strcpy(dialog->fields, ids[next]));
    }
}

static void number_field(struct nk_context *ctx, pc_ui_dialog *dialog, const char *key, const char *label,
                         double min, double max, double def)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    double value = field_num(root, key, def);
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }
    double next = value;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_double(ctx, pc_i18n_t(label), min, &next, max, 1, 1);
    if (next != value) {
        put(dialog->fields, root, key, yyjson_mut_real(dialog->fields, round(next)));
    }
}

static void check_field(struct nk_context *ctx, pc_ui_dialog *dialog, const char *key, const char *label,
                        bool def)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    nk_bool value = field_bool(root, key, def), next = value;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_checkbox_label(ctx, pc_i18n_t(label), &next);
    if (next != value) {
        put(dialog->fields, root, key, yyjson_mut_bool(dialog->fields, next != 0));
    }
}

/* ---- estimate ---- */

/* Flatten the active document (exact for small images; an area-averaged proxy for large ones) and
 * optimise it with the current settings, returning the estimated file size in bytes and, for an
 * indexed format, the palette size. Scaled by the Image Size percent squared. */
static bool estimate_size(pc_ui_shell_app *app, yyjson_mut_val *root, double *out_bytes,
                          uint64_t *out_colors, char *msg, size_t msgcap)
{
    pc_engine_session *s = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    if (!state) {
        return false;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (!doc || doc->size.width == 0 || doc->size.height == 0) {
        return false;
    }
    /* Build immutable params for pc_web_settings_from_params. */
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_error err = {0};
    pc_web_settings st;
    pc_web_settings_default(&st);
    bool have_st = false;
    if (pc_json_write(root, 0, &text, &err) == PC_OK &&
        pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, &err) == PC_OK) {
        pc_web_settings_fini(&st);
        if (pc_web_settings_from_params(yyjson_doc_get_root(parsed), WEB_COMMAND, &st, &err) == PC_OK) {
            have_st = true;
        }
    }
    pc_buf_fini(&text);
    yyjson_doc_free(parsed);
    if (!have_st) {
        (void)snprintf(msg, msgcap, "%s", err.msg[0] ? err.msg : "invalid settings");
        return false;
    }

    uint64_t full_area = (uint64_t)doc->size.width * doc->size.height;
    pc_compose_buffer buf = {0};
    double ratio = 1.0;
    pc_status rc;
    if (full_area <= 4000000ULL) {
        rc = pc_compose_flatten(doc, &buf, &err);
    } else {
        double k = sqrt(2048.0 * 2048.0 / (double)full_area);
        uint32_t pw = (uint32_t)fmax(1.0, round((double)doc->size.width * k));
        uint32_t ph = (uint32_t)fmax(1.0, round((double)doc->size.height * k));
        rc = pc_compose_render_reduced(doc, pw, ph, &buf, &err);
        uint64_t proxy_area = (uint64_t)pw * ph;
        ratio = proxy_area ? (double)full_area / (double)proxy_area : 1.0;
    }
    if (rc != PC_OK) {
        pc_web_settings_fini(&st);
        (void)snprintf(msg, msgcap, "%s", err.msg);
        return false;
    }

    pc_web_optimized opt = {0};
    const uint8_t *icc = pc_blob_data(doc->icc_profile);
    size_t icc_len = pc_blob_len(doc->icc_profile);
    uint32_t bw = pc_rect_width(buf.rect);
    rc = pc_web_optimize((const float(*)[4])buf.px, bw, buf.rect, &st, icc, icc_len, NULL, 0,
                         doc->resolution_dpi, false, &opt, &err);
    bool ok = rc == PC_OK;
    if (ok) {
        double pct = field_num(root, "percent", 100.0);
        if (!isfinite(pct) || pct < 1.0 || pct > 1000.0) {
            pct = 100.0;
        }
        double scale = (pct / 100.0) * (pct / 100.0);
        if (out_bytes) {
            *out_bytes = (double)opt.bytes.len * ratio * scale;
        }
        if (out_colors) {
            *out_colors = opt.has_colors ? (uint64_t)opt.colors : 0;
        }
    } else {
        (void)snprintf(msg, msgcap, "%s", err.msg);
    }
    pc_web_optimized_fini(&opt);
    pc_compose_buffer_fini(&buf);
    pc_web_settings_fini(&st);
    return ok;
}

/* ---- preview ---- */

static void draw_checker(struct nk_context *ctx, struct nk_rect r)
{
    struct nk_command_buffer *out = nk_window_get_canvas(ctx);
    nk_fill_rect(out, r, 0, nk_rgb(200, 200, 200));
    float cell = 10.0f;
    int nx = (int)ceilf(r.w / cell), ny = (int)ceilf(r.h / cell);
    if (nx < 0 || ny < 0 || (long)nx * ny > 4096) {
        return;
    }
    for (int yy = 0; yy < ny; yy++) {
        for (int xx = 0; xx < nx; xx++) {
            if (((xx + yy) & 1) == 0) {
                continue;
            }
            float cx = r.x + (float)xx * cell, cy = r.y + (float)yy * cell;
            float cw = fminf(cell, r.x + r.w - cx), ch = fminf(cell, r.y + r.h - cy);
            if (cw > 0 && ch > 0) {
                nk_fill_rect(out, nk_rect(cx, cy, cw, ch), 0, nk_rgb(235, 235, 235));
            }
        }
    }
}

static void preview_pane(pc_ui_shell_app *app, struct nk_context *ctx, float height, const char *title,
                         const char *subtitle)
{
    nk_layout_row_dynamic(ctx, height, 1);
    struct nk_rect bounds;
    if (!nk_widget(&bounds, ctx)) {
        return;
    }
    draw_checker(ctx, bounds);
    struct nk_image image;
    float aspect = 0;
    if (pc_ui_shell_canvas_preview(app, &image, &aspect) && aspect > 0) {
        float w = fminf(bounds.w, bounds.h * aspect), h = w / aspect;
        struct nk_rect rect = nk_rect(bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h);
        nk_draw_image(nk_window_get_canvas(ctx), rect, &image, nk_rgba(255, 255, 255, 255));
    }
    nk_layout_row_dynamic(ctx, 18, 1);
    nk_label(ctx, title, NK_TEXT_LEFT);
    if (subtitle && subtitle[0]) {
        nk_layout_row_dynamic(ctx, 16, 1);
        nk_label(ctx, subtitle, NK_TEXT_LEFT);
    }
}

void pc_ui_shell_web_export_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    pc_engine_session *s = pc_ui_shell_app_session(app);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    if (!doc) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("No document."), NK_TEXT_LEFT);
        return;
    }

    /* View tabs. */
    static const char *const views[] = {"original", "optimized", "2up", "4up"};
    static const char *const view_labels[] = {"Original", "Optimized", "2-Up", "4-Up"};
    const char *view = field_str(root, "__view", "2up");
    nk_layout_row_dynamic(ctx, 26, 4);
    for (int i = 0; i < 4; i++) {
        if (nk_option_label(ctx, pc_i18n_t(view_labels[i]), strcmp(view, views[i]) == 0)) {
            if (strcmp(view, views[i]) != 0) {
                put(dialog->fields, root, "__view", yyjson_mut_strcpy(dialog->fields, views[i]));
                view = views[i];
            }
        }
    }

    const char *fmt = field_str(root, "format", "jpeg");
    char orig_sub[128];
    (void)snprintf(orig_sub, sizeof orig_sub, "%u x %u px", doc->size.width, doc->size.height);

    /* Estimated-size subtitle for the optimised panes. */
    char opt_title[64] = "Optimized";
    char opt_sub[128] = "";
    const char *est = field_str(root, "__estimateLabel", NULL);
    const char *est_sub = field_str(root, "__estimateSub", NULL);
    if (est) {
        (void)snprintf(opt_title, sizeof opt_title, "%s", est);
    }
    if (est_sub) {
        (void)snprintf(opt_sub, sizeof opt_sub, "%s", est_sub);
    }

    static const float ratio[2] = {0.6f, 0.4f};
    float area = fmaxf(220.0f, nk_window_get_content_region(ctx).h - 130.0f);
    nk_layout_row(ctx, NK_DYNAMIC, area, 2, ratio);

    if (nk_group_begin(ctx, "web-preview", NK_WINDOW_BORDER)) {
        if (!strcmp(view, "original")) {
            preview_pane(app, ctx, area - 54.0f, pc_i18n_t("Original"), orig_sub);
        } else if (!strcmp(view, "optimized")) {
            preview_pane(app, ctx, area - 54.0f, opt_title, opt_sub[0] ? opt_sub : pc_i18n_t(fmt));
        } else if (!strcmp(view, "2up")) {
            float half = (area - 20.0f) / 2.0f;
            preview_pane(app, ctx, half - 36.0f, pc_i18n_t("Original"), orig_sub);
            preview_pane(app, ctx, half - 36.0f, opt_title, opt_sub[0] ? opt_sub : pc_i18n_t(fmt));
        } else {
            float quarter = (area - 20.0f) / 2.0f - 40.0f;
            double q = field_num(root, "quality", 60.0), c = field_num(root, "colors", 128.0);
            preview_pane(app, ctx, quarter, pc_i18n_t("Original"), orig_sub);
            preview_pane(app, ctx, quarter, opt_title, opt_sub[0] ? opt_sub : pc_i18n_t(fmt));
            for (double k = 2.0; k <= 4.0; k += 2.0) {
                char vf[8];
                double vq = q, vc = c;
                pc_ui_web_variant(fmt, q, c, k, vf, sizeof vf, &vq, &vc);
                char label[96];
                if (!strcmp(vf, "jpeg")) {
                    (void)snprintf(label, sizeof label, "JPEG  Quality %.0f", vq);
                } else if (!strcmp(vf, "gif") || !strcmp(vf, "png8")) {
                    (void)snprintf(label, sizeof label, "%s  %.0f colors",
                                   !strcmp(vf, "gif") ? "GIF" : "PNG-8", vc);
                } else {
                    (void)snprintf(label, sizeof label, "%s", vf);
                }
                preview_pane(app, ctx, quarter, label, "");
            }
        }
        nk_group_end(ctx);
    }

    if (nk_group_begin(ctx, "web-settings", NK_WINDOW_BORDER)) {
        static const char *const fmt_labels[PC_UI_WEB_NFORMATS] = {"GIF", "PNG-8", "PNG-24", "JPEG",
                                                                   "WBMP"};
        combo_field(ctx, dialog, "format", "Format", PC_UI_WEB_FORMAT_IDS, fmt_labels,
                    PC_UI_WEB_NFORMATS, "jpeg");
        fmt = field_str(root, "format", "jpeg");
        if (!strcmp(fmt, "gif") || !strcmp(fmt, "png8")) {
            static const char *const pal_ids[] = {"perceptual", "selective", "adaptive", "restrictive",
                                                  "exact"};
            static const char *const pal_labels[] = {"Perceptual", "Selective", "Adaptive",
                                                     "Restrictive (Web)", "Exact"};
            combo_field(ctx, dialog, "palette", "Palette", pal_ids, pal_labels, 5, "selective");
            number_field(ctx, dialog, "colors", "Colors", 2, 256, 128);
            static const char *const dith_ids[] = {"none", "diffusion", "pattern", "noise"};
            static const char *const dith_labels[] = {"No Dither", "Diffusion", "Pattern", "Noise"};
            combo_field(ctx, dialog, "dither", "Dither", dith_ids, dith_labels, 4, "diffusion");
            number_field(ctx, dialog, "ditherAmount", "Dither %", 0, 100, 88);
            check_field(ctx, dialog, "transparency", "Transparency", true);
            check_field(ctx, dialog, "interlaced", "Interlaced", false);
            number_field(ctx, dialog, "webSnap", "Web Snap %", 0, 100, 0);
        } else if (!strcmp(fmt, "jpeg")) {
            number_field(ctx, dialog, "quality", "Quality", 0, 100, 60);
            check_field(ctx, dialog, "progressive", "Progressive", false);
            check_field(ctx, dialog, "optimized", "Optimized", true);
            check_field(ctx, dialog, "embedIcc", "Embed Color Profile", false);
        } else if (!strcmp(fmt, "wbmp")) {
            static const char *const dith_ids[] = {"none", "diffusion", "pattern"};
            static const char *const dith_labels[] = {"No Dither", "Diffusion", "Pattern"};
            combo_field(ctx, dialog, "dither", "Dither", dith_ids, dith_labels, 3, "none");
        } else { /* png24 */
            check_field(ctx, dialog, "transparency", "Transparency", true);
            check_field(ctx, dialog, "interlaced", "Interlaced", false);
        }

        if (strcmp(fmt, "png24") || !field_bool(root, "transparency", true)) {
            const char *matte = field_str(root, "matte", "#ffffff");
            pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, "matte", matte);
            if (buffer) {
                nk_layout_row_dynamic(ctx, 26, 2);
                nk_label(ctx, pc_i18n_t("Matte"), NK_TEXT_LEFT);
                nk_flags f = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data,
                                                            (int)buffer->text.cap, nk_filter_default);
                buffer->active = (f & NK_EDIT_ACTIVE) != 0;
                buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
                if (strcmp(pc_buf_cstr(&buffer->text), matte)) {
                    put(dialog->fields, root, "matte",
                        yyjson_mut_strcpy(dialog->fields, pc_buf_cstr(&buffer->text)));
                }
            }
        }
        check_field(ctx, dialog, "convertToSrgb", "Convert to sRGB", true);
        static const char *const meta_ids[] = {"none", "copyright", "copyrightAndContact", "all"};
        static const char *const meta_labels[] = {"None", "Copyright", "Copyright and Contact Info",
                                                 "All"};
        combo_field(ctx, dialog, "metadata", "Metadata", meta_ids, meta_labels, 4, "copyright");

        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("Image Size"), NK_TEXT_LEFT);
        number_field(ctx, dialog, "percent", "Percent %", 1, 1000, 100);
        uint32_t ow = doc->size.width, oh = doc->size.height;
        pc_ui_web_scaled_size(doc->size.width, doc->size.height, field_num(root, "percent", 100.0), &ow,
                              &oh);
        char dims[64];
        (void)snprintf(dims, sizeof dims, "W: %u px   H: %u px", ow, oh);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, dims, NK_TEXT_LEFT);

        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, pc_i18n_t("Estimate file size"))) {
            double bytes = 0;
            uint64_t colors = 0;
            char msg[256] = "";
            if (estimate_size(app, root, &bytes, &colors, msg, sizeof msg)) {
                char human[16], secs_sub[96];
                pc_ui_human_bytes(bytes, human);
                double secs = pc_ui_web_download_seconds(bytes);
                if (colors) {
                    (void)snprintf(secs_sub, sizeof secs_sub, "%.0f sec @ 56.6 Kbps  -  %llu colors",
                                   secs, (unsigned long long)colors);
                } else {
                    (void)snprintf(secs_sub, sizeof secs_sub, "%.0f sec @ 56.6 Kbps", secs);
                }
                char title[48];
                char up[8];
                size_t ei = 0;
                for (; fmt[ei] && ei < sizeof up - 1; ei++) {
                    up[ei] = (char)toupper((unsigned char)fmt[ei]);
                }
                up[ei] = '\0';
                (void)snprintf(title, sizeof title, "%s  %s", up, human);
                put(dialog->fields, root, "__estimateLabel", yyjson_mut_strcpy(dialog->fields, title));
                put(dialog->fields, root, "__estimateSub", yyjson_mut_strcpy(dialog->fields, secs_sub));
                r->error[0] = 0;
            } else if (msg[0]) {
                (void)snprintf(r->error, sizeof r->error, "%s", msg);
            }
        }
        nk_layout_row_dynamic(ctx, 28, 1);
        const char *path = field_str(root, "path", NULL);
        if (nk_button_label(ctx, r->file_reply ? pc_i18n_t("Waiting for file chooser...")
                                               : pc_i18n_t("Save..."))) {
            put(dialog->fields, root, "__choose", yyjson_mut_bool(dialog->fields, true));
        }
        if (path && path[0]) {
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label(ctx, path, NK_TEXT_LEFT);
        }
        nk_group_end(ctx);
    }
}
