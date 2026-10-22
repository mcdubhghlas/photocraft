#include "print_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "shell_internal.h"
#include "ui/i18n.h"
#include "ui/print_layout.h"
#include "engine/engine.h"

#define PRINT_COMMAND "file.print"

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

pc_status pc_ui_shell_print_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    pc_engine_session *s = pc_ui_shell_app_session(app);
    if (!pc_engine_session_active(s)) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "print dialog");
    }
    bool ok = yyjson_mut_obj_add_strcpy(d, root, "__command", PRINT_COMMAND) &&
              yyjson_mut_obj_add_strcpy(d, root, "__label", "PhotoCraft Print Settings") &&
              yyjson_mut_obj_add_bool(d, root, "__print", true) &&
              yyjson_mut_obj_add_strcpy(d, root, "printer", "") &&
              yyjson_mut_obj_add_int(d, root, "copies", 1) &&
              yyjson_mut_obj_add_strcpy(d, root, "paper", "letter") &&
              yyjson_mut_obj_add_strcpy(d, root, "orientation", "portrait") &&
              yyjson_mut_obj_add_strcpy(d, root, "colorHandling", "printerManages") &&
              yyjson_mut_obj_add_strcpy(d, root, "printerProfile", "coated-cmyk") &&
              yyjson_mut_obj_add_strcpy(d, root, "intent", "relative") &&
              yyjson_mut_obj_add_bool(d, root, "bpc", true) &&
              yyjson_mut_obj_add_bool(d, root, "center", true) &&
              yyjson_mut_obj_add_real(d, root, "top", 0.0) &&
              yyjson_mut_obj_add_real(d, root, "left", 0.0) &&
              yyjson_mut_obj_add_real(d, root, "scale", 100.0) &&
              yyjson_mut_obj_add_bool(d, root, "scaleToFit", false) &&
              yyjson_mut_obj_add_bool(d, root, "cornerCropMarks", false) &&
              yyjson_mut_obj_add_bool(d, root, "centerCropMarks", false) &&
              yyjson_mut_obj_add_bool(d, root, "registrationMarks", false) &&
              yyjson_mut_obj_add_bool(d, root, "description", false) &&
              yyjson_mut_obj_add_bool(d, root, "labels", false) &&
              yyjson_mut_obj_add_bool(d, root, "emulsionDown", false) &&
              yyjson_mut_obj_add_strcpy(d, root, "units", "cm") &&
              yyjson_mut_obj_add_strcpy(d, root, "output", "");
    /* Ask the spooler once, now, what printers it knows (stored under __printers). */
    pc_json_value printers = PC_JSON_VALUE_INIT;
    if (ok && pc_engine_session_execute_json(s, "file.printers", "{}", &printers, NULL) == PC_OK) {
        yyjson_mut_val *list = yyjson_mut_obj_get(pc_json_value_root(&printers), "printers");
        if (list) {
            put(d, root, "__printers", yyjson_mut_val_mut_copy(d, list));
        }
    }
    pc_json_value_fini(&printers);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "print dialog");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}

/* ---- widgets ---- */

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
    int next = nk_combo(ctx, shown, count, selected, 24, nk_vec2(220, 280));
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
        put(dialog->fields, root, key, yyjson_mut_real(dialog->fields, next));
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

/* A length field editing a value stored in points (custom paper) or inches (position). `unit_pt`
 * is points per display unit; `in_inches` scales the stored value from inches to points first. */
static void length_field(struct nk_context *ctx, pc_ui_dialog *dialog, const char *key,
                         const char *label, double unit_pt, bool in_inches)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    double pt = field_num(root, key, 0.0) * (in_inches ? 72.0 : 1.0);
    double shown = unit_pt > 0 ? pt / unit_pt : 0.0;
    double next = shown;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_property_double(ctx, pc_i18n_t(label), 0, &next, 10000, 0.1, 0.01f);
    if (fabs(next - shown) > 1e-6 && isfinite(next)) {
        double new_pt = next * unit_pt;
        double stored = in_inches ? new_pt / 72.0 : new_pt;
        put(dialog->fields, root, key, yyjson_mut_real(dialog->fields, stored));
    }
}

/* ---- preview ---- */

static void draw_marks(struct nk_command_buffer *out, struct nk_rect img, bool corner, bool reg)
{
    struct nk_color black = nk_rgb(0, 0, 0);
    if (corner) {
        struct nk_vec2 c[4] = {{img.x, img.y},
                               {img.x + img.w, img.y},
                               {img.x, img.y + img.h},
                               {img.x + img.w, img.y + img.h}};
        float dx[4] = {-1, 1, -1, 1};
        float dy[4] = {-1, 1, 1, 1};
        for (int i = 0; i < 4; i++) {
            float sdx = (i == 0 || i == 2) ? -1.0f : 1.0f;
            float sdy = (i < 2) ? -1.0f : 1.0f;
            nk_stroke_line(out, c[i].x + sdx * 3, c[i].y, c[i].x + sdx * 12, c[i].y, 1, black);
            nk_stroke_line(out, c[i].x, c[i].y + sdy * 3, c[i].x, c[i].y + sdy * 12, 1, black);
            (void)dx;
            (void)dy;
        }
    }
    if (reg) {
        struct nk_vec2 c[4] = {{img.x + img.w / 2, img.y - 9},
                               {img.x + img.w / 2, img.y + img.h + 9},
                               {img.x - 9, img.y + img.h / 2},
                               {img.x + img.w + 9, img.y + img.h / 2}};
        for (int i = 0; i < 4; i++) {
            nk_stroke_circle(out, nk_rect(c[i].x - 3.5f, c[i].y - 3.5f, 7, 7), 1, black);
        }
    }
}

static void preview(pc_ui_shell_app *app, struct nk_context *ctx, pc_ui_dialog *dialog,
                    const pc_document *doc)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    nk_layout_row_dynamic(ctx, 360, 1);
    struct nk_rect area;
    if (!nk_widget(&area, ctx)) {
        return;
    }
    struct nk_command_buffer *out = nk_window_get_canvas(ctx);
    nk_fill_rect(out, area, 0, nk_rgb(60, 60, 60));

    double paper_w = 612.0, paper_h = 792.0;
    yyjson_mut_val *pv = yyjson_mut_obj_get(root, "paper");
    if (yyjson_mut_is_arr(pv) && yyjson_mut_arr_size(pv) == 2) {
        paper_w = yyjson_mut_get_num(yyjson_mut_arr_get(pv, 0));
        paper_h = yyjson_mut_get_num(yyjson_mut_arr_get(pv, 1));
    } else {
        pc_ui_print_paper_size(field_str(root, "paper", "letter"), &paper_w, &paper_h);
    }
    bool landscape = !strcmp(field_str(root, "orientation", "portrait"), "landscape");
    bool marks = field_bool(root, "cornerCropMarks", false) || field_bool(root, "centerCropMarks", false) ||
                 field_bool(root, "registrationMarks", false);
    pc_ui_print_layout lay;
    bool valid = pc_ui_print_compute_layout(
        paper_w, paper_h, landscape, doc->size.width, doc->size.height, (double)doc->resolution_dpi,
        marks, field_bool(root, "scaleToFit", false), field_num(root, "scale", 100.0),
        field_bool(root, "center", true), field_num(root, "top", 0.0), field_num(root, "left", 0.0),
        &lay);

    if (lay.pw > 0 && lay.ph > 0) {
        float k = fminf((area.w - 20.0f) / (float)lay.pw, (area.h - 20.0f) / (float)lay.ph);
        if (k > 0) {
            struct nk_rect page = nk_rect(area.x + (area.w - (float)lay.pw * k) / 2,
                                          area.y + (area.h - (float)lay.ph * k) / 2,
                                          (float)lay.pw * k, (float)lay.ph * k);
            nk_fill_rect(out, page, 0, nk_rgb(255, 255, 255));
            double ix = field_bool(root, "emulsionDown", false) ? lay.pw - lay.x - lay.w : lay.x;
            struct nk_rect img = nk_rect(page.x + (float)ix * k,
                                         page.y + page.h - (float)(lay.y + lay.h) * k,
                                         (float)lay.w * k, (float)lay.h * k);
            struct nk_image image;
            float aspect = 0;
            if (img.w > 0 && img.h > 0 && pc_ui_shell_canvas_preview(app, &image, &aspect)) {
                nk_draw_image(out, img, &image, nk_rgba(255, 255, 255, 255));
            }
            draw_marks(out, img, field_bool(root, "cornerCropMarks", false),
                       field_bool(root, "registrationMarks", false));
        }
    }
    char info[128];
    bool fits = lay.x >= 0 && lay.y >= 0 && lay.x + lay.w <= lay.pw && lay.y + lay.h <= lay.ph;
    (void)snprintf(info, sizeof info, "Scale %.1f%%  -  %.2f x %.2f in%s", lay.scale * 100.0,
                   lay.w / 72.0, lay.h / 72.0, (valid && fits) ? "" : "  -  larger than the paper");
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, info, NK_TEXT_LEFT);
}

void pc_ui_shell_print_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
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

    static const float ratio[2] = {0.46f, 0.54f};
    float area = fmaxf(360.0f, nk_window_get_content_region(ctx).h - 90.0f);
    nk_layout_row(ctx, NK_DYNAMIC, area, 2, ratio);

    if (nk_group_begin(ctx, "print-preview", NK_WINDOW_BORDER)) {
        preview(app, ctx, dialog, doc);
        nk_group_end(ctx);
    }

    if (nk_group_begin(ctx, "print-settings", NK_WINDOW_BORDER)) {
        /* Printer. */
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("Printer Setup"), NK_TEXT_LEFT);
        {
            yyjson_mut_val *list = yyjson_mut_obj_get(root, "__printers");
            const char *cur = field_str(root, "printer", "");
            /* Build id/label arrays: default (""), then each printer. */
            static char ids[33][128];
            static char labels[33][160];
            int n = 0;
            (void)snprintf(ids[0], sizeof ids[0], "%s", "");
            const char *default_name = NULL;
            size_t count = yyjson_mut_is_arr(list) ? yyjson_mut_arr_size(list) : 0;
            for (size_t i = 0; i < count && n < 31; i++) {
                yyjson_mut_val *pe = yyjson_mut_arr_get(list, i);
                const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(pe, "name"));
                if (!name) {
                    continue;
                }
                if (yyjson_mut_get_bool(yyjson_mut_obj_get(pe, "default"))) {
                    default_name = name;
                }
                n++;
                (void)snprintf(ids[n], sizeof ids[n], "%s", name);
                pc_ui_print_printer_label(name, labels[n], sizeof labels[n]);
            }
            if (default_name) {
                char pretty[128];
                pc_ui_print_printer_label(default_name, pretty, sizeof pretty);
                (void)snprintf(labels[0], sizeof labels[0], "%s (%s)", pc_i18n_t("Default printer"),
                               pretty);
            } else {
                (void)snprintf(labels[0], sizeof labels[0], "%s", pc_i18n_t("Default printer"));
            }
            int total = n + 1;
            /* A saved printer the spooler no longer lists stays selectable. */
            bool present = cur[0] == '\0';
            for (int i = 1; i < total; i++) {
                if (!strcmp(ids[i], cur)) {
                    present = true;
                }
            }
            if (!present && cur[0] && total < 33) {
                (void)snprintf(ids[total], sizeof ids[total], "%s", cur);
                pc_ui_print_printer_label(cur, labels[total], sizeof labels[total]);
                total++;
            }
            int selected = 0;
            for (int i = 0; i < total; i++) {
                if (!strcmp(ids[i], cur)) {
                    selected = i;
                }
            }
            const char *shown[33];
            for (int i = 0; i < total; i++) {
                shown[i] = labels[i];
            }
            nk_layout_row_dynamic(ctx, 26, 1);
            int next = nk_combo(ctx, shown, total, selected, 24, nk_vec2(300, 300));
            if (next != selected && next >= 0 && next < total) {
                put(dialog->fields, root, "printer", yyjson_mut_strcpy(dialog->fields, ids[next]));
            }
        }
        number_field(ctx, dialog, "copies", "Copies", 1, 999, 1);

        /* Paper + orientation. */
        {
            static const char *const paper_ids[PC_UI_PRINT_NPAPERS + 1] = {
                "letter", "legal", "tabloid", "a3", "a4", "a5", "4x6", "5x7", "custom"};
            static const char *const paper_labels[PC_UI_PRINT_NPAPERS + 1] = {
                "Letter", "Legal", "Tabloid", "A3", "A4", "A5", "4x6", "5x7", "Custom"};
            bool custom = yyjson_mut_is_arr(yyjson_mut_obj_get(root, "paper"));
            const char *value = custom ? "custom" : field_str(root, "paper", "letter");
            int selected = 0;
            for (int i = 0; i <= PC_UI_PRINT_NPAPERS; i++) {
                if (!strcmp(value, paper_ids[i])) {
                    selected = i;
                }
            }
            const char *shown[PC_UI_PRINT_NPAPERS + 1];
            for (int i = 0; i <= PC_UI_PRINT_NPAPERS; i++) {
                shown[i] = pc_i18n_t(paper_labels[i]);
            }
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_label(ctx, pc_i18n_t("Paper"), NK_TEXT_LEFT);
            int next = nk_combo(ctx, shown, PC_UI_PRINT_NPAPERS + 1, selected, 24, nk_vec2(160, 280));
            if (next != selected && next >= 0 && next <= PC_UI_PRINT_NPAPERS) {
                if (!strcmp(paper_ids[next], "custom")) {
                    double w = 612.0, h = 792.0;
                    pc_ui_print_paper_size(custom ? "letter" : value, &w, &h);
                    yyjson_mut_val *a = yyjson_mut_arr(dialog->fields);
                    if (a && yyjson_mut_arr_add_real(dialog->fields, a, w) &&
                        yyjson_mut_arr_add_real(dialog->fields, a, h)) {
                        put(dialog->fields, root, "paper", a);
                    }
                } else {
                    put(dialog->fields, root, "paper",
                        yyjson_mut_strcpy(dialog->fields, paper_ids[next]));
                }
            }
            static const char *const orient_ids[] = {"portrait", "landscape"};
            static const char *const orient_labels[] = {"Portrait", "Landscape"};
            combo_field(ctx, dialog, "orientation", "Orientation", orient_ids, orient_labels, 2,
                        "portrait");
        }

        const char *units = field_str(root, "units", "cm");
        double unit_pt = pc_ui_print_unit_pt(units);
        if (yyjson_mut_is_arr(yyjson_mut_obj_get(root, "paper"))) {
            yyjson_mut_val *pv = yyjson_mut_obj_get(root, "paper");
            double w = yyjson_mut_get_num(yyjson_mut_arr_get(pv, 0));
            double h = yyjson_mut_get_num(yyjson_mut_arr_get(pv, 1));
            double sw = unit_pt > 0 ? w / unit_pt : 0, sh = unit_pt > 0 ? h / unit_pt : 0;
            double nw = sw, nh = sh;
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_property_double(ctx, pc_i18n_t("Paper Width"), 1, &nw, 10000, 0.1, 0.01f);
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_property_double(ctx, pc_i18n_t("Paper Height"), 1, &nh, 10000, 0.1, 0.01f);
            if ((fabs(nw - sw) > 1e-6 || fabs(nh - sh) > 1e-6) && isfinite(nw) && isfinite(nh)) {
                yyjson_mut_val *a = yyjson_mut_arr(dialog->fields);
                if (a && yyjson_mut_arr_add_real(dialog->fields, a, nw * unit_pt) &&
                    yyjson_mut_arr_add_real(dialog->fields, a, nh * unit_pt)) {
                    put(dialog->fields, root, "paper", a);
                }
            }
        }

        /* Colour management. */
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("Color Management"), NK_TEXT_LEFT);
        {
            static const char *const ch_ids[] = {"printerManages", "photocraftManages",
                                                "noColorManagement"};
            static const char *const ch_labels[] = {"Printer Manages Colors", "PhotoCraft Manages Colors",
                                                    "No Color Management"};
            combo_field(ctx, dialog, "colorHandling", "Color Handling", ch_ids, ch_labels, 3,
                        "printerManages");
        }
        if (!strcmp(field_str(root, "colorHandling", ""), "photocraftManages")) {
            static const char *const prof_ids[] = {"coated-cmyk", "srgb", "adobe-rgb-compat",
                                                  "display-p3", "gray-gamma-2.2"};
            static const char *const prof_labels[] = {"Coated CMYK", "sRGB IEC61966-2.1",
                                                     "Adobe RGB (1998) compatible", "Display P3",
                                                     "Gray Gamma 2.2"};
            combo_field(ctx, dialog, "printerProfile", "Printer Profile", prof_ids, prof_labels, 5,
                        "coated-cmyk");
            static const char *const int_ids[] = {"perceptual", "relative", "saturation", "absolute"};
            static const char *const int_labels[] = {"Perceptual", "Relative Colorimetric", "Saturation",
                                                    "Absolute Colorimetric"};
            combo_field(ctx, dialog, "intent", "Rendering Intent", int_ids, int_labels, 4, "relative");
            check_field(ctx, dialog, "bpc", "Black Point Compensation", true);
        }

        /* Position and size. */
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("Position and Size"), NK_TEXT_LEFT);
        {
            static const char *const unit_ids[] = {"cm", "mm", "in"};
            static const char *const unit_labels[] = {"Centimeters", "Millimeters", "Inches"};
            combo_field(ctx, dialog, "units", "Units", unit_ids, unit_labels, 3, "cm");
        }
        units = field_str(root, "units", "cm");
        unit_pt = pc_ui_print_unit_pt(units);
        check_field(ctx, dialog, "center", "Center", true);
        if (!field_bool(root, "center", true)) {
            length_field(ctx, dialog, "top", "Top", unit_pt, true);
            length_field(ctx, dialog, "left", "Left", unit_pt, true);
        }
        check_field(ctx, dialog, "scaleToFit", "Scale to Fit Media", false);
        if (!field_bool(root, "scaleToFit", false)) {
            number_field(ctx, dialog, "scale", "Scale %", 1, 1000, 100);
        }
        if (doc->size.width > 0) {
            pc_ui_print_layout lay;
            double paper_w = 612.0, paper_h = 792.0;
            yyjson_mut_val *pv = yyjson_mut_obj_get(root, "paper");
            if (yyjson_mut_is_arr(pv) && yyjson_mut_arr_size(pv) == 2) {
                paper_w = yyjson_mut_get_num(yyjson_mut_arr_get(pv, 0));
                paper_h = yyjson_mut_get_num(yyjson_mut_arr_get(pv, 1));
            } else {
                pc_ui_print_paper_size(field_str(root, "paper", "letter"), &paper_w, &paper_h);
            }
            (void)pc_ui_print_compute_layout(
                paper_w, paper_h, !strcmp(field_str(root, "orientation", ""), "landscape"),
                doc->size.width, doc->size.height, (double)doc->resolution_dpi, false,
                field_bool(root, "scaleToFit", false), field_num(root, "scale", 100.0),
                field_bool(root, "center", true), 0, 0, &lay);
            if (lay.w > 0) {
                char ppi[64];
                double res = (double)doc->size.width / (lay.w / 72.0);
                (void)snprintf(ppi, sizeof ppi, "%s %.0f PPI", pc_i18n_t("Print Resolution:"), res);
                nk_layout_row_dynamic(ctx, 18, 1);
                nk_label(ctx, ppi, NK_TEXT_LEFT);
            }
        }

        /* Printing marks. */
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("Printing Marks"), NK_TEXT_LEFT);
        check_field(ctx, dialog, "cornerCropMarks", "Corner Crop Marks", false);
        check_field(ctx, dialog, "centerCropMarks", "Center Crop Marks", false);
        check_field(ctx, dialog, "registrationMarks", "Registration Marks", false);
        check_field(ctx, dialog, "description", "Description", false);
        check_field(ctx, dialog, "labels", "Labels", false);
        check_field(ctx, dialog, "emulsionDown", "Emulsion Down", false);

        /* Save as PDF. */
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("Save as PDF"), NK_TEXT_LEFT);
        {
            const char *output = field_str(root, "output", "");
            pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, "output", output);
            if (buffer) {
                nk_layout_row_dynamic(ctx, 26, 1);
                nk_flags f = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data,
                                                            (int)buffer->text.cap, nk_filter_default);
                buffer->active = (f & NK_EDIT_ACTIVE) != 0;
                buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
                if (strcmp(pc_buf_cstr(&buffer->text), output)) {
                    if (buffer->text.len) {
                        put(dialog->fields, root, "output",
                            yyjson_mut_strcpy(dialog->fields, pc_buf_cstr(&buffer->text)));
                    } else {
                        put(dialog->fields, root, "output", yyjson_mut_strcpy(dialog->fields, ""));
                    }
                }
            }
        }

        /* Print One Copy: reuse the dialog's confirm path with the one-copy command. */
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, pc_i18n_t("Print One Copy"))) {
            put(dialog->fields, root, "__command",
                yyjson_mut_strcpy(dialog->fields, "file.printOneCopy"));
            r->confirm_requested = true;
        }
        nk_group_end(ctx);
    }
}
