/* The window/taskbar icon, decoded from the embedded app-icon PNG (assets/app-icon/.../
 * ai.storyteller.photocraft.png, matching PC_APP_ID).
 *
 * Behavioural port of apps/photocraft/src/app_icon.rs. Kept GUI-free (no SDL) so it lives in the app
 * library: it only decodes to RGBA8; the desktop entry turns the pixels into an SDL surface and calls
 * SDL_SetWindowIcon. macOS/Windows-specific icon resources (the .ico, the MSI shortcut icon) are not
 * ported (REWRITE_PLAN.md §7, D2). */
#ifndef PC_APP_ICON_H
#define PC_APP_ICON_H

#include <stdint.h>

#include "base/status.h"

/* Decode the window icon to unmultiplied RGBA8. On success *out is a malloc'd w*h*4 buffer (caller
 * frees) and the dimensions go to *w and *h. On failure *out is untouched and err is filled. */
pc_status pc_app_window_icon_rgba(uint8_t **out, uint32_t *w, uint32_t *h, pc_error *err);

#endif /* PC_APP_ICON_H */
