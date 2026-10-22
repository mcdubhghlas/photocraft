#include "theme_cards.h"
#include <string.h>

/* Order matches prefs_ui.rs LIGHT_THEME_OPTIONS / DARK_THEME_OPTIONS. The ids are the ones the
 * engine stores in interface.lightTheme / interface.darkTheme / interface.favoriteThemes. */
static const pc_ui_theme_option LIGHT_OPTIONS[] = {
    {"studioLight", PC_UI_THEME_STUDIO_LIGHT}, {"proLightGray", PC_UI_THEME_PRO_LIGHT_GRAY},
    {"proLight", PC_UI_THEME_PRO_LIGHT},       {"classic", PC_UI_THEME_CLASSIC},
    {"adwaita", PC_UI_THEME_ADWAITA},          {"breezeLight", PC_UI_THEME_BREEZE_LIGHT},
    {"softLight", PC_UI_THEME_SOFT_LIGHT},
};
static const pc_ui_theme_option DARK_OPTIONS[] = {
    {"proMedium", PC_UI_THEME_PRO_MEDIUM},     {"pro", PC_UI_THEME_PRO},
    {"studio", PC_UI_THEME_STUDIO},            {"solarizedDark", PC_UI_THEME_SOLARIZED_DARK},
    {"adwaitaDark", PC_UI_THEME_ADWAITA_DARK}, {"breezeDark", PC_UI_THEME_BREEZE_DARK},
    {"softDark", PC_UI_THEME_SOFT_DARK},
};

size_t pc_ui_theme_options(bool dark, const pc_ui_theme_option **out)
{
    if (out) {
        *out = dark ? DARK_OPTIONS : LIGHT_OPTIONS;
    }
    return dark ? sizeof DARK_OPTIONS / sizeof *DARK_OPTIONS : sizeof LIGHT_OPTIONS / sizeof *LIGHT_OPTIONS;
}

bool pc_ui_theme_light_active(const char *mode, bool system_is_light)
{
    if (!mode) {
        return false;
    }
    if (!strcmp(mode, "light")) {
        return true;
    }
    if (!strcmp(mode, "dark")) {
        return false;
    }
    return system_is_light; /* "auto" (and any unknown value): follow the system appearance */
}

bool pc_ui_theme_favorite_contains(const char *const *favorites, size_t n, const char *id)
{
    if (!favorites || !id) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (favorites[i] && !strcmp(favorites[i], id)) {
            return true;
        }
    }
    return false;
}
