/* Theme-card model for Preferences › Interface (B7): the light/dark theme option tables, which
 * appearance slot is active, and favourite-set membership. Pure logic, no GUI dependency, so the
 * selection rules are unit-tested without a window (port of prefs_ui.rs LIGHT/DARK_THEME_OPTIONS,
 * favorite_theme_groups and the appearance_rows active-slot computation). */
#ifndef PC_UI_THEME_CARDS_H
#define PC_UI_THEME_CARDS_H

#include "theme.h"
#include <stdbool.h>
#include <stddef.h>

/* One theme a card offers: its preference id ("studioLight") and the kind it renders. */
typedef struct pc_ui_theme_option {
    const char *id;
    pc_ui_theme_kind kind;
} pc_ui_theme_option;

/* The dark (dark=true) or light (dark=false) theme options in card order. Writes the borrowed,
 * static table into *out and returns its length. */
size_t pc_ui_theme_options(bool dark, const pc_ui_theme_option **out);

/* Whether the light card is the active appearance for the given appearance mode ("auto", "light"
 * or "dark"; anything else is treated as "auto") and the resolved system appearance. Mirrors the
 * shell's pc_ui_shell_apply_preferences slot choice. */
bool pc_ui_theme_light_active(const char *mode, bool system_is_light);

/* Whether `id` is one of the `n` favourite ids (a borrowed array of borrowed strings). NULL-safe. */
bool pc_ui_theme_favorite_contains(const char *const *favorites, size_t n, const char *id);

#endif /* PC_UI_THEME_CARDS_H */
