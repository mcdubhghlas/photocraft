#include "credits_ui.h"
#include "shell_internal.h"
#include "ui/i18n.h"
#include "ui/links.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put(pc_ui_dialog *d, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(d->fields, key);
    if (k && value)
        (void)yyjson_mut_obj_put(yyjson_mut_doc_get_root(d->fields), k, value);
}
static void grouped(uint64_t value, char out[40])
{
    char raw[32];
    int n = snprintf(raw, sizeof raw, "%" PRIu64, value);
    size_t length = n > 0 && (size_t)n < sizeof raw ? (size_t)n : 0, at = 0;
    for (size_t i = 0; i < length; i++) {
        if (i && (length - i) % 3 == 0)
            out[at++] = ',';
        out[at++] = raw[i];
    }
    out[at] = 0;
}
static void delta(const pc_ui_contributor *c, char out[48])
{
    bool negative = c->added < c->deleted;
    char digits[40];
    grouped(negative ? c->deleted - c->added : c->added - c->deleted, digits);
    (void)snprintf(out, 48, "%s%s", negative ? "−" : "+", digits);
}
static void contributor_rows(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    size_t name_mode = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "names")),
           sort = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "sort"));
    if (!yyjson_mut_obj_get(root, "sort"))
        sort = PC_CREDIT_FIRST;
    if (name_mode > 2)
        name_mode = 0;
    if (sort >= PC_CREDIT_SORT_COUNT)
        sort = PC_CREDIT_FIRST;
    yyjson_mut_val *direction = yyjson_mut_obj_get(root, "ascending");
    bool ascending =
        direction ? yyjson_mut_get_bool(direction) : sort == PC_CREDIT_NAME || sort >= PC_CREDIT_FIRST;
    const char *modes[] = {"Username", "Display name", "Real name"};
    nk_layout_row_dynamic(ctx, 26, 3);
    for (size_t i = 0; i < 3; i++) {
        char id[80];
        (void)snprintf(id, sizeof id, "about.names.%zu", i);
        pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
        if (nk_option_label(ctx, pc_i18n_t(modes[i]), name_mode == i)) {
            name_mode = i;
            put(d, "names", yyjson_mut_uint(d->fields, i));
        }
    }
    const char *labels[] = {"Name (A–Z)",
                            "Merged PRs",
                            "Commits",
                            "Lines added",
                            "Lines deleted",
                            "Line delta",
                            "Binary assets added",
                            "Binary assets removed",
                            "First commit",
                            "Last commit"};
    const char *translated[PC_CREDIT_SORT_COUNT];
    for (size_t i = 0; i < PC_CREDIT_SORT_COUNT; i++)
        translated[i] = pc_i18n_t(labels[i]);
    nk_layout_row_dynamic(ctx, 26, 2);
    pc_ui_shell_widget_mapping(app, "about.sort", nk_widget_bounds(ctx));
    int next = nk_combo(ctx, translated, PC_CREDIT_SORT_COUNT, (int)sort, 24, nk_vec2(260, 280));
    if (next >= 0 && next < PC_CREDIT_SORT_COUNT && (size_t)next != sort) {
        sort = (size_t)next;
        ascending = sort == PC_CREDIT_NAME || sort >= PC_CREDIT_FIRST;
        put(d, "sort", yyjson_mut_uint(d->fields, sort));
        put(d, "ascending", yyjson_mut_bool(d->fields, ascending));
    }
    pc_ui_shell_widget_mapping(app, "about.reverse", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t(ascending ? "Ascending" : "Descending"))) {
        ascending = !ascending;
        put(d, "ascending", yyjson_mut_bool(d->fields, ascending));
    }
    size_t *indices = NULL;
    if (pc_ui_credits_order(&r->credits, (pc_ui_credit_name_mode)name_mode, (pc_ui_credit_sort)sort,
                            ascending, &indices, NULL) != PC_OK)
        return;
    nk_layout_row_dynamic(ctx, 380, 1);
    if (nk_group_begin(ctx, "contributors-table", NK_WINDOW_BORDER)) {
        const char *headers[] = {"Name", "PRs",  "Commits", "+LOC",  "−LOC",
                                 "ΔLOC", "+Bin", "−Bin",    "First", "Last"};
        const float widths[] = {180, 50, 65, 80, 80, 90, 50, 50, 100, 100};
        nk_layout_row_begin(ctx, NK_STATIC, 24, 10);
        for (size_t i = 0; i < 10; i++) {
            nk_layout_row_push(ctx, widths[i]);
            nk_label(ctx, pc_i18n_t(headers[i]), i ? NK_TEXT_RIGHT : NK_TEXT_LEFT);
        }
        nk_layout_row_end(ctx);
        for (size_t i = 0; i < r->credits.count; i++) {
            const pc_ui_contributor *row = &r->credits.contributors[indices[i]];
            const char *chosen = pc_ui_credit_chosen_name(row, (pc_ui_credit_name_mode)name_mode);
            char fallback[256];
            (void)snprintf(fallback, sizeof fallback, "@%s", row->login);
            nk_layout_row_begin(ctx, NK_STATIC, 24, 10);
            nk_layout_row_push(ctx, widths[0]);
            if (nk_button_label(ctx, chosen ? chosen : fallback)) {
                bool safe = *row->login && strlen(row->login) <= 100;
                for (size_t n = 0; safe && row->login[n]; n++) {
                    char c = row->login[n];
                    safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-';
                }
                if (safe) {
                    char url[160];
                    (void)snprintf(url, sizeof url, "https://github.com/%s", row->login);
                    (void)SDL_OpenURL(url);
                }
            }
            uint64_t counts[] = {row->prs, row->commits,      row->added,         row->deleted,
                                 0,        row->binary_added, row->binary_deleted};
            for (size_t column = 1; column < 10; column++) {
                char value[48];
                if (column == 5)
                    delta(row, value);
                else if (column >= 8) {
                    const char *date = column == 8 ? row->first : row->last;
                    (void)snprintf(value, sizeof value, "%.10s", *date ? date : "—");
                } else
                    grouped(counts[column - 1], value);
                nk_layout_row_push(ctx, widths[column]);
                nk_label(ctx, value, NK_TEXT_RIGHT);
            }
            nk_layout_row_end(ctx);
        }
        nk_group_end(ctx);
    }
    free(indices);
}
static void models(pc_ui_shell_app *app, pc_ui_dialog_runtime *r)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const char *headers[] = {"Company", "Model", "Version", "Commits", "+LOC", "−LOC"};
    nk_layout_row_dynamic(ctx, 26, 6);
    for (size_t i = 0; i < 6; i++)
        nk_label(ctx, pc_i18n_t(headers[i]), NK_TEXT_LEFT);
    for (size_t i = 0; i < r->credits.nmodels; i++) {
        const pc_ui_credit_model *m = &r->credits.models[i];
        nk_layout_row_dynamic(ctx, 26, 6);
        nk_label(ctx, m->company, NK_TEXT_LEFT);
        nk_label(ctx, m->model, NK_TEXT_LEFT);
        nk_label(ctx, m->version, NK_TEXT_LEFT);
        const uint64_t numbers[] = {m->commits, m->added, m->deleted};
        for (size_t n = 0; n < 3; n++) {
            char value[40];
            grouped(numbers[n], value);
            nk_label(ctx, value, NK_TEXT_RIGHT);
        }
    }
}
void pc_ui_shell_credits_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!r->has_credits) {
        pc_error error = {0};
        if (pc_ui_credits_embedded(&r->credits, &error) == PC_OK)
            r->has_credits = true;
        else {
            nk_layout_row_dynamic(ctx, 40, 1);
            nk_label_wrap(ctx, error.msg);
            return;
        }
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    size_t selected = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "tab"));
    if (selected > 2)
        selected = 0;
    nk_layout_row_dynamic(ctx, 30, 3);
    const char *tabs[] = {"About PhotoCraft", "Contributors", "Models"};
    for (size_t i = 0; i < 3; i++) {
        char id[80];
        (void)snprintf(id, sizeof id, "about.tab.%zu", i);
        pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t(tabs[i]))) {
            selected = i;
            put(d, "tab", yyjson_mut_uint(d->fields, i));
        }
    }
    if (selected == 1)
        contributor_rows(app, r, d);
    else if (selected == 2)
        models(app, r);
    else {
        nk_layout_row_dynamic(ctx, 50, 1);
        nk_label(ctx, pc_i18n_t("PhotoCraft"), NK_TEXT_CENTERED);
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label(ctx, pc_i18n_t("Native image editing"), NK_TEXT_CENTERED);
        nk_label(ctx, pc_i18n_t("MIT / Apache-2.0"), NK_TEXT_CENTERED);
        char total[40], label[128];
        grouped(r->credits.total_commits, total);
        (void)snprintf(label, sizeof label, "%s %s", total, pc_i18n_t("Commits"));
        nk_label(ctx, label, NK_TEXT_CENTERED);
        nk_layout_row_dynamic(ctx, 32, 1);
        if (nk_button_label(ctx, pc_i18n_t("Project website")))
            (void)SDL_OpenURL(PC_UI_LINK_APP_PAGE);
        if (nk_button_label(ctx, pc_i18n_t("Source code")))
            (void)SDL_OpenURL(PC_UI_LINK_GITHUB);
    }
}
