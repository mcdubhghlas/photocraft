#include "shell_internal.h"
#include "engine/file_ui.h"
#include "engine/prefs.h"
#include "ui/recent.h"
#include <string.h>
pc_status pc_ui_shell_file_execute(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                   pc_json_value *out, pc_error *err)
{
    pc_engine_session *session = pc_ui_shell_app_session(app);
    pc_status result;
    if (!strcmp(command, "file.save") || !strcmp(command, "file.saveAs")) {
        result = pc_engine_file_save_ui(session, params, !strcmp(command, "file.saveAs"), out, err);
    } else {
        result = pc_engine_session_execute(session, !strcmp(command, "file.open") ? "file.openAs" : command,
                                           params, out, err);
    }
    if (result == PC_OK && (!strcmp(command, "file.open") || !strcmp(command, "file.openAs") ||
                            !strcmp(command, "file.save") || !strcmp(command, "file.saveAs"))) {
        const char *path = pc_json_as_str(pc_json_get(params, "path"), NULL);
        const pc_engine_doc_state *active = pc_engine_session_active(session);
        if (!path && active) {
            path = active->path;
        }
        if (path && *path) {
            size_t limit = 20;
            pc_json_value pref = PC_JSON_VALUE_INIT;
            if (pc_engine_prefs_get(session, "fileHandling.recentFileCount", &pref, NULL) == PC_OK) {
                double count = yyjson_mut_get_num(pc_json_value_root(&pref));
                if (count >= 0 && count <= 100) {
                    limit = (size_t)count;
                }
            }
            pc_json_value_fini(&pref);
            (void)pc_ui_recent_push(pc_ui_shell_app_state(app), path, limit, NULL);
        }
    }
    if (result == PC_OK)
        pc_ui_shell_result_warnings(app, command, out);
    return result;
}
