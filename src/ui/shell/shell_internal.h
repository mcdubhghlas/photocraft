/* Shared declarations between the shell's app.c (owns the PhotocraftApp struct and the frame loop)
 * and main.c (SDL entry point + input translation). GUI types (SDL/Nuklear) cross this boundary, so
 * it is only used by the shell's own translation units, never by the control test or app.h clients. */
#ifndef PC_UI_SHELL_INTERNAL_H
#define PC_UI_SHELL_INTERNAL_H

#include <SDL3/SDL.h>

#include "app.h"
#include "ui/shortcuts.h"
#include "ui/tool_gesture.h"
#include "ui/type_tool.h"
#include "photocraft_nuklear.h"

bool pc_ui_shell_preview_doc_point(pc_ui_shell_app *app, const struct nk_image *image, float u, float v,
                                   float out[2]);
bool pc_ui_shell_canvas_preview(pc_ui_shell_app *app, struct nk_image *out, float *aspect);
bool pc_ui_shell_icon_image(pc_ui_shell_app *app, const char *name, float logical_side, struct nk_image *out);
pc_status pc_ui_shell_notices_json(pc_ui_shell_app *app, pc_json_value *out, pc_error *err);
void pc_ui_shell_report(pc_ui_shell_app *app, const char *title, const char *details, bool error);
void pc_ui_shell_cancel_canvas_gesture(pc_ui_shell_app *app);
void pc_ui_shell_queue(pc_ui_shell_app *app, const char *command, const yyjson_mut_val *params);
void pc_ui_shell_widget_mapping(pc_ui_shell_app *app, const char *command, struct nk_rect rect);
void pc_ui_shell_request_exit(pc_ui_shell_app *app);
bool pc_ui_shell_exit_requested(const pc_ui_shell_app *app);
void pc_ui_shell_app_changed(pc_ui_shell_app *app);
void pc_ui_shell_apply_preferences(pc_ui_shell_app *app);
pc_status pc_ui_shell_file_execute(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                   pc_json_value *out, pc_error *err);

pc_status pc_ui_shell_app_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                  unsigned modifiers, pc_json_value *out, pc_error *err);

/* Translate one SDL event into Nuklear input and shell shortcuts (pointer, keys, text). Called by
 * the frame loop between nk_input_begin/end. Never crashes on any event. */
bool pc_ui_shell_canvas_pointer_active(const pc_ui_shell_app *app);
void pc_ui_shell_sync_targets(pc_ui_shell_app *app);
bool pc_ui_shell_layers_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_opacity_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_keyboard_captured(pc_ui_shell_app *app);
pc_ui_key pc_ui_shell_input_key(SDL_Keycode key);
unsigned pc_ui_shell_input_mods(SDL_Keymod mods);
unsigned pc_ui_shell_input_modifiers(const pc_ui_shell_app *app);
void pc_ui_shell_input_track(pc_ui_shell_app *app, const SDL_Event *event);

void pc_ui_shell_input_event(pc_ui_shell_app *a, const SDL_Event *e);

bool pc_ui_shell_pen_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_dock_cancel(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_dock_capture(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_menu_key(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_menu_capture(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_dialog_key(pc_ui_shell_app *app, const SDL_Event *event);
void pc_ui_shell_modal_changed(pc_ui_shell_app *app);
bool pc_ui_shell_dialog_at(const pc_ui_shell_app *app, float x, float y);

pc_ui_type_pointer *pc_ui_shell_type_pointer(pc_ui_shell_app *app);
pc_status pc_ui_shell_type_clipboard(pc_ui_shell_app *app,SDL_Keycode key,pc_json_value *out,pc_error *err);
bool pc_ui_shell_type_input(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_type_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                             float factor);
void pc_ui_shell_type_draw(pc_ui_shell_app *app, struct nk_command_buffer *canvas);
pc_status pc_ui_shell_bitmap(pc_ui_shell_app *app, const uint8_t key[32], uint16_t width, uint16_t height,
                             const uint8_t *rgba, struct nk_image *out, pc_error *err);

bool pc_ui_shell_canvas_event(pc_ui_shell_app *a, const SDL_Event *event);

/* Release the lazily-built shortcut binding table (the interactive loop builds it on first key). */
void pc_ui_shell_input_cleanup(void);

/* Internals the input translation reaches (defined in app.c). */
struct nk_context *pc_ui_shell_app_nk(pc_ui_shell_app *a);
SDL_Window *pc_ui_shell_app_sdl_window(pc_ui_shell_app *a);
float pc_ui_shell_app_ui_scale(const pc_ui_shell_app *a);


void pc_ui_shell_result_warnings(pc_ui_shell_app *app, const char *command, const pc_json_value *result);
bool pc_ui_shell_doc_size_event(pc_ui_shell_app *app, const SDL_Event *event);
#endif /* PC_UI_SHELL_INTERNAL_H */
