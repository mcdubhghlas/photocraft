#ifndef PC_UI_SHELL_JOBS_UI_H
#define PC_UI_SHELL_JOBS_UI_H
#include "app.h"
#include <SDL3/SDL.h>
#include "photocraft_nuklear.h"
typedef struct pc_ui_jobs {
    pc_json_value info;
    bool enabled, locked, modal;
    uint64_t focus;
} pc_ui_jobs;
pc_ui_jobs *pc_ui_shell_jobs_runtime(pc_ui_shell_app *app);
void pc_ui_shell_jobs_fini(pc_ui_jobs *jobs);
void pc_ui_shell_jobs_tick(pc_ui_shell_app *app);
bool pc_ui_shell_jobs_locked(pc_ui_shell_app *app);
bool pc_ui_shell_jobs_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_jobs_status(pc_ui_shell_app *app);
void pc_ui_shell_jobs_draw(pc_ui_shell_app *app, float width, float height);
pc_status pc_ui_shell_jobs_execute(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                   pc_json_value *out, pc_error *err);
#endif
