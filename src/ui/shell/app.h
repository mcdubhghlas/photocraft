/* PhotocraftApp: the application shell that ties the engine, the UI state and the SDL3 + OpenGL
 * 3.3 + Nuklear + FreeType window together (REWRITE_PLAN.md U2/U3, D4; reimplementation of the
 * egui `PhotocraftApp` root of crates/ui-egui/src/lib.rs against the Nuklear stack proven by the
 * B3 spike).
 *
 * The app drives the engine ONLY through the public pc_engine_session_* API and reads/writes the
 * U1 UiState; it never reaches into ops/doc/compose internals (src/ui's layer deps are engine +
 * the lower UI + the layers tools/check-layers.sh allows). The GUI stack (SDL window, GL context,
 * Nuklear, the FreeType atlas) is optional: pc_ui_shell_app_new builds a headless app (session +
 * UiState) that the control channel can drive with no display, and pc_ui_shell_app_open_window
 * adds the window for the interactive frame loop and GL screenshots.
 *
 * EXTENSION REGION (U3..U10): docking/tabs/work-area chrome, the canvas view, panels, tool UIs and
 * dialogs attach to this struct and build their part of the frame. The extension points are marked
 * below; this chunk provides the app root, the frame loop, the menu bar and the control handler. */
#ifndef PC_UI_SHELL_APP_H
#define PC_UI_SHELL_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "engine/engine.h"
#include "ui/menu_nav.h"
#include "ui/state.h"
#include "ui/theme.h"

/* Opaque so the GUI headers (SDL/GL/Nuklear/FreeType) stay inside app.c and the control test can
 * link the shell objects without pulling in a GL context. */
typedef struct pc_ui_shell_app pc_ui_shell_app;

/* A headless app: a fresh engine session, the default UiState and theme tokens, no window. Never
 * touches SDL. Free with pc_ui_shell_app_free. */
pc_status pc_ui_shell_app_new(pc_ui_shell_app **out, pc_error *err);
void pc_ui_shell_app_free(pc_ui_shell_app *a); /* NULL-safe */

/* Open the SDL3 window, GL 3.3 core context, Nuklear and the FreeType atlas (reusing the spike's
 * gl33/font/render helpers). `font_path` is the TTF to rasterise; `forced_scale` overrides the
 * display scale (0 = follow the display); `hidden` opens a hidden window (snapshots / offscreen).
 * Requires SDL_Init(SDL_INIT_VIDEO) to have succeeded (the caller does it, so SDL_VIDEO_DRIVER can
 * be set first). On failure *a keeps working headlessly. */
pc_status pc_ui_shell_app_open_window(pc_ui_shell_app *a, const char *font_path, float forced_scale,
                                      bool hidden, pc_error *err);

/* Run the interactive frame loop until the window is closed. Requires an open window. Returns 0 on
 * a clean exit, non-zero on a fatal render error (reported to stderr). */
int pc_ui_shell_app_run(pc_ui_shell_app *a);

/* Render one frame of the current UI into the bound framebuffer at `fb_w`x`fb_h` pixels. Requires
 * an open window (a current GL context). Builds the menu bar and the work area. */
pc_status pc_ui_shell_app_render_frame(pc_ui_shell_app *a, int fb_w, int fb_h, pc_error *err);

/* Capture the window as RGBA8 (top-to-bottom rows, w*h*4 bytes, caller frees with free()). With a
 * GL context the current UI frame is rendered to an offscreen framebuffer and read back; without a
 * window a CPU image is produced (the theme canvas colour, with the active document's thumbnail
 * composited in when one is open), so the control channel yields a valid PNG with no display. */
pc_status pc_ui_shell_app_capture(pc_ui_shell_app *a, uint8_t **out_rgba, int *out_w, int *out_h,
                                  pc_error *err);

/* ---- accessors the control handler uses (no GUI types cross this boundary) ---- */

pc_engine_session *pc_ui_shell_app_session(pc_ui_shell_app *a);
pc_ui_state *pc_ui_shell_app_state(pc_ui_shell_app *a);
const pc_ui_tokens *pc_ui_shell_app_tokens(const pc_ui_shell_app *a);
uint64_t pc_ui_shell_app_frame(const pc_ui_shell_app *a);
void pc_ui_shell_app_window_size(const pc_ui_shell_app *a, float *w, float *h, float *pixels_per_point);
void pc_ui_shell_app_set_window_size(pc_ui_shell_app *a, float w, float h);

/* Apply the active theme to the live Nuklear style (after UiState.theme changed). Safe headlessly. */
void pc_ui_shell_app_apply_theme(pc_ui_shell_app *a);

/* Invoke a menu item / command id by its routing rule: UI commands go to view/shell state, engine
 * commands to pc_engine_session_execute. `params` may be NULL. Writes *out (owned) only on success.
 * This is the shell's menus::invoke; the control channel and the menu bar both use it. */
pc_status pc_ui_shell_app_invoke(pc_ui_shell_app *a, const char *id, yyjson_val *params,
                                 pc_json_value *out, pc_error *err);

/* Synchronise per-document cameras after open/close/reorder. Preserves them by document id. */
pc_status pc_ui_shell_app_sync_views(pc_ui_shell_app *a, pc_error *err);

/* The menu-bar keyboard navigation state (used by the frame loop and testable on its own). */
pc_ui_menu_nav *pc_ui_shell_app_nav(pc_ui_shell_app *a);

#endif /* PC_UI_SHELL_APP_H */
