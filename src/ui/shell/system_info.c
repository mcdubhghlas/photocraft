#include "system_info.h"
#include "shell_internal.h"
#include "engine/display_color.h"
#include "ui/i18n.h"
#include <string.h>
#include <stdio.h>
static const char *arch(void)
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__riscv) && defined(__riscv_xlen) && __riscv_xlen == 64
    return "riscv64";
#elif defined(__powerpc64__)
    return "powerpc64";
#else
    return "unknown";
#endif
}
static const char *machine_os(void)
{
    const char *platform = SDL_GetPlatform();
    if (!strcmp(platform, "Windows"))
        return "windows";
    if (!strcmp(platform, "Mac OS X"))
        return "macos";
    if (!strcmp(platform, "Linux"))
        return "linux";
    if (!strcmp(platform, "FreeBSD"))
        return "freebsd";
    if (!strcmp(platform, "OpenBSD"))
        return "openbsd";
    return platform;
}
static const char *str(yyjson_mut_val *object, const char *key, const char *fallback)
{
    const char *value = yyjson_mut_get_str(yyjson_mut_obj_get(object, key));
    return value ? value : fallback;
}
static pc_status text(yyjson_mut_val *info, bool localized, pc_buf *out, pc_error *err)
{
    yyjson_mut_val *gpu = yyjson_mut_obj_get(info, "gpu"), *monitor = yyjson_mut_obj_get(info, "monitor");
    const char *labels[] = {"Platform",        "Logical CPU cores",       "System memory", "UI renderer",
                            "Canvas renderer", "Graphics adapter",        "Graphics API",  "Monitor profile",
                            "Monitor source",  "Monitor profile request", "Monitor status"};
    const char *names[11];
    for (size_t i = 0; i < 11; i++)
        names[i] = localized ? pc_i18n_t(labels[i]) : labels[i];
    pc_status status = pc_buf_appendf(
        out, err,
        "PhotoCraft %s\n%s: %s %s\n%s: %llu\n%s: %llu MiB\n\n%s: OpenGL 3.3 / Nuklear\n%s: %s\n%s: %s\n%s: "
        "%s\n\n%s: %s\n%s: %s\n%s: %s\n",
        str(info, "version", ""), names[0], str(info, "os", "unknown"), str(info, "arch", "unknown"),
        names[1], (unsigned long long)yyjson_mut_get_uint(yyjson_mut_obj_get(info, "cpuCores")), names[2],
        (unsigned long long)yyjson_mut_get_uint(yyjson_mut_obj_get(info, "ramMiB")), names[3], names[4],
        str(gpu, "canvas", "cpu"), names[5], str(gpu, "adapter", ""), names[6], str(gpu, "driver", ""),
        names[7], str(monitor, "profile", "sRGB"), names[8], str(monitor, "source", "fallback"), names[9],
        str(monitor, "requested", "auto"));
    const char *reason = yyjson_mut_get_str(yyjson_mut_obj_get(monitor, "reason"));
    if (status == PC_OK && reason)
        status = pc_buf_appendf(out, err, "%s: %s\n", names[10], reason);
    const char *lost = yyjson_mut_get_str(yyjson_mut_obj_get(gpu, "lost"));
    if (status == PC_OK && lost)
        status = pc_buf_appendf(out, err, "\n%s\n", lost);
    return status;
}
pc_status pc_ui_shell_system_info(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    if (!app || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing system information output");
    pc_json_value gpu = PC_JSON_VALUE_INIT, color = PC_JSON_VALUE_INIT;
    pc_status status = pc_ui_shell_app_gpu_info(app, &gpu, err);
    if (status == PC_OK)
        status = pc_engine_color_diagnostics(pc_ui_shell_app_session(app), &color, err);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    char version[192];
    (void)pc_engine_long_version(version, sizeof version);
    yyjson_mut_val *color_root = pc_json_value_root(&color),
                   *monitor = yyjson_mut_obj_get(color_root, "monitorStatus"),
                   *displays = yyjson_mut_obj_get(color_root, "displays");
    int cores = SDL_GetNumLogicalCPUCores(), ram = SDL_GetSystemRAM();
    bool ok = root && yyjson_mut_obj_add_strcpy(d, root, "version", version) &&
              yyjson_mut_obj_add_str(d, root, "os", machine_os()) &&
              yyjson_mut_obj_add_str(d, root, "arch", arch()) &&
              yyjson_mut_obj_add_uint(d, root, "cpuCores", cores > 0 ? (uint64_t)cores : 0) &&
              yyjson_mut_obj_add_uint(d, root, "ramMiB", ram > 0 ? (uint64_t)ram : 0) &&
              yyjson_mut_obj_add_val(d, root, "gpu", yyjson_mut_val_mut_copy(d, pc_json_value_root(&gpu))) &&
              yyjson_mut_obj_add_val(d, root, "monitor",
                                     monitor ? yyjson_mut_val_mut_copy(d, monitor) : yyjson_mut_obj(d)) &&
              yyjson_mut_obj_add_val(d, root, "displays",
                                     displays ? yyjson_mut_val_mut_copy(d, displays) : yyjson_mut_arr(d));
    if (status == PC_OK && !ok)
        status = pc_fail(err, PC_ERR_NOMEM, "system information");
    pc_buf english = PC_BUF_INIT;
    if (status == PC_OK)
        status = text(root, false, &english, err);
    if (status == PC_OK) {
        yyjson_mut_val *lines = yyjson_mut_arr(d);
        bool complete = lines != NULL;
        for (size_t at = 0; complete && at < english.len;) {
            const char *start = pc_buf_cstr(&english) + at, *end = memchr(start, '\n', english.len - at);
            size_t n = end ? (size_t)(end - start) : english.len - at;
            yyjson_mut_val *line = yyjson_mut_strncpy(d, start, n);
            complete = line && yyjson_mut_arr_add_val(lines, line);
            at += n + (end ? 1u : 0u);
        }
        if (!complete || !yyjson_mut_obj_add_val(d, root, "lines", lines))
            status = pc_fail(err, PC_ERR_NOMEM, "system information lines");
    }
    if (status == PC_OK)
        status = pc_json_value_from_mut(root, out, err);
    pc_buf_fini(&english);
    pc_json_value_fini(&gpu);
    pc_json_value_fini(&color);
    yyjson_mut_doc_free(d);
    return status;
}
pc_status pc_ui_shell_system_info_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    pc_json_value info = PC_JSON_VALUE_INIT;
    pc_status status = pc_ui_shell_system_info(app, &info, err);
    pc_buf english = PC_BUF_INIT, display = PC_BUF_INIT;
    if (status == PC_OK)
        status = text(pc_json_value_root(&info), false, &english, err);
    if (status == PC_OK)
        status = text(pc_json_value_root(&info), true, &display, err);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (status == PC_OK &&
        (!root || !yyjson_mut_obj_add_bool(d, root, "systemInfo", true) ||
         !yyjson_mut_obj_add_str(d, root, "__label", "System Info") ||
         !yyjson_mut_obj_add_strncpy(d, root, "__copyText", pc_buf_cstr(&english), english.len) ||
         !yyjson_mut_obj_add_strncpy(d, root, "__displayText", pc_buf_cstr(&display), display.len)))
        status = pc_fail(err, PC_ERR_NOMEM, "system information dialog");
    if (status == PC_OK) {
        status = pc_ui_shell_dialog_store(app, PC_DIALOG_ABOUT, d, root, out, err);
        d = NULL;
        yyjson_mut_val *reply = status == PC_OK ? pc_json_value_root(out) : NULL;
        if (reply)
            (void)yyjson_mut_obj_add_val(out->doc, reply, "info",
                                         yyjson_mut_val_mut_copy(out->doc, pc_json_value_root(&info)));
    }
    yyjson_mut_doc_free(d);
    pc_buf_fini(&english);
    pc_buf_fini(&display);
    pc_json_value_fini(&info);
    return status;
}
void pc_ui_shell_system_info_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *display = str(root, "__displayText", ""), *copy = str(root, "__copyText", "");
    pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, "systemInfo", display);
    nk_layout_row_dynamic(ctx, 430, 1);
    if (buffer) {
        nk_flags flags = nk_edit_string_zero_terminated(
            ctx, (nk_flags)NK_EDIT_BOX | (nk_flags)NK_EDIT_READ_ONLY, (char *)buffer->text.data,
            (int)buffer->text.cap, nk_filter_default);
        buffer->active = (flags & NK_EDIT_ACTIVE) != 0;
        buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
    }
    nk_layout_row_dynamic(ctx, 30, 1);
    pc_ui_shell_widget_mapping(app, "systemInfo.copy", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Copy")) && !SDL_SetClipboardText(copy))
        (void)snprintf(r->error, sizeof r->error, "Clipboard: %.230s", SDL_GetError());
}
