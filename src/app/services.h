/* Small platform OS-services glue for the desktop app (Linux/POSIX).
 *
 * Behavioural port of the Linux-relevant parts of apps/photocraft/src/services.rs. The rest of
 * that reference module is egui/rfd/recovery glue that belongs to the UI shell, and several of its
 * OS services are already provided by SDL3 and the shell, so they are NOT re-implemented here
 * (REWRITE_PLAN P2, decisions D2/D19):
 *   - Opening a URL / revealing a file ("open-in-finder") uses SDL_OpenURL in the shell
 *     (src/ui/shell/credits_ui.c), which runs xdg-open on Linux; no duplicate is needed.
 *   - The Wayland clipboard-image fallback and arboard glue are SDL3 clipboard territory.
 *   - Apple-event service registration is macOS-only and deferred (D2).
 *
 * What remains here is pure POSIX and testable without a display: shell-word quoting, the
 * XWayland relaunch command string, and the "Beep When Done" terminal bell. Nothing crashes. */
#ifndef PC_APP_SERVICES_H
#define PC_APP_SERVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "app_dirs.h"
#include "base/status.h"

/* `s` as one POSIX shell word, written NUL-terminated into out[cap]: bare when every byte is safe
 * there (ASCII alphanumeric or one of /._-+:,@), otherwise single-quoted with embedded quotes
 * escaped as '\''. PC_ERR_LIMIT when the result would not fit (out is left empty). */
pc_status pc_services_shell_word(const char *s, char *out, size_t cap, pc_error *err);

/* The shell command that relaunches this install under XWayland, written into out[cap]; sets
 * *available. A Flatpak (non-empty flatpak_id) is given the X11 socket only; an AppImage
 * (non-empty appimage path) relaunches by its own path; anything else is "photocraft" on PATH.
 * Outside Flatpak, *available is false (out empty) when there is no X server to run on
 * (x_display == false). PC_ERR_LIMIT when the command would not fit.
 *
 * Deviation (noted in REWRITE_PLAN P2 §7): this workaround exists because winit 0.30 delivered no
 * Wayland file drops. SDL3 delivers Wayland file drops, so the relaunch is expected to be
 * unnecessary; the pure builder is ported for parity and so the decision can be tested. */
pc_status pc_services_xwayland_command(const char *flatpak_id, const char *appimage, bool x_display,
                                       char *out, size_t cap, bool *available, pc_error *err);

/* Preferences > General > Beep When Done: write the terminal bell (BEL, 0x07) to `f` and flush.
 * pc_services_beep() does the same to stdout (audible when launched from a terminal, the cue
 * macOS terminals also use). No beep API among this app's dependencies on Linux. */
void pc_services_beep_to(FILE *f);
void pc_services_beep(void);

#endif /* PC_APP_SERVICES_H */
