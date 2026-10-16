/* Design system: themes and colour/shape tokens (REWRITE_PLAN.md U1, port of
 * crates/ui-egui/src/theme.rs).
 *
 * Widgets read pc_ui_tokens_for_kind() instead of hard-coding colours, so every theme applies
 * everywhere. Colours are RGBA bytes (egui Color32); the Rust reference stores them unmultiplied.
 * Pure data + logic: this header carries no GUI dependency, so the whole library can read the
 * tokens; src/ui/widgets map them onto Nuklear's style. */
#ifndef PC_UI_THEME_H
#define PC_UI_THEME_H

#include <stdbool.h>
#include <stdint.h>

/* An unmultiplied RGBA colour, like egui's Color32. */
typedef struct pc_ui_color {
    uint8_t r, g, b, a;
} pc_ui_color;

/* Interface theme (serde variant names in pc_ui_theme_kind_name; ui.set ids in _id). The order
 * matches the Rust ThemeKind enum so the serde discriminant and PC_UI_THEME_* agree. */
typedef enum pc_ui_theme_kind {
    PC_UI_THEME_PRO = 0,     /* darkest Photoshop brightness */
    PC_UI_THEME_PRO_MEDIUM,  /* Photoshop default (#535353 panels) */
    PC_UI_THEME_PRO_LIGHT_GRAY,
    PC_UI_THEME_PRO_LIGHT,
    PC_UI_THEME_STUDIO,
    PC_UI_THEME_STUDIO_LIGHT,
    PC_UI_THEME_CLASSIC,
    PC_UI_THEME_ADWAITA,
    PC_UI_THEME_ADWAITA_DARK,
    PC_UI_THEME_SOLARIZED_DARK,
    PC_UI_THEME_BREEZE_LIGHT,
    PC_UI_THEME_BREEZE_DARK,
    PC_UI_THEME_SOFT_DARK,
    PC_UI_THEME_SOFT_LIGHT,
    PC_UI_THEME_COUNT
} pc_ui_theme_kind;

/* The default theme (Rust ThemeKind::default() = ProMedium, used by UiState::default). */
#define PC_UI_THEME_DEFAULT PC_UI_THEME_PRO_MEDIUM

/* Colour and shape tokens for one theme. Field order and meaning mirror theme.rs Tokens. */
typedef struct pc_ui_tokens {
    pc_ui_theme_kind kind;
    pc_ui_color chrome;      /* window chrome (title bar, toolbars) */
    pc_ui_color canvas;      /* canvas surround */
    pc_ui_color canvas_dot;  /* dot colour of the canvas grid pattern */
    pc_ui_color dock;        /* dock background (behind cards) */
    pc_ui_color card;        /* cards / panels */
    pc_ui_color card_border;
    pc_ui_color field;       /* inputs, fields, dropdowns */
    pc_ui_color field_border;
    pc_ui_color hover;
    pc_ui_color pressed;
    pc_ui_color text;
    pc_ui_color text_dim;
    pc_ui_color text_faint;
    pc_ui_color icon;
    pc_ui_color accent;
    pc_ui_color accent_soft;
    pc_ui_color accent_border;
    pc_ui_color accent_text;
    pc_ui_color separator;
    pc_ui_color shadow;
    pc_ui_color primary_bg;
    pc_ui_color primary_text;
    pc_ui_color danger;
    pc_ui_color warning;
    float radius_sm;
    float radius;
    float radius_lg;
    bool bevel;        /* Classic theme draws 3D bevels instead of flat fills */
    bool pro;          /* Photoshop-grammar layout (tab strips, flat panels) */
    bool round_chips;  /* large round stacked colour chips (Studio) */
    pc_ui_color tab_strip;
    pc_ui_color row_selected;
    pc_ui_color histogram_bg;
    uint8_t histogram_level;
    pc_ui_color caption_close;
    pc_ui_color caption_close_text;
    pc_ui_color scrim;
} pc_ui_tokens;

/* The 14 theme kinds in registry order (ThemeKind::ALL). */
extern const pc_ui_theme_kind PC_UI_THEME_ALL[PC_UI_THEME_COUNT];

/* Tokens for a theme. Always succeeds (an out-of-range kind yields the default theme). */
pc_ui_tokens pc_ui_tokens_for_kind(pc_ui_theme_kind kind);

/* Human label, e.g. "Pro (Medium Gray)". */
const char *pc_ui_theme_kind_label(pc_ui_theme_kind kind);

/* Serde variant name, e.g. "ProMedium" (UiState.theme JSON value). */
const char *pc_ui_theme_kind_name(pc_ui_theme_kind kind);

/* Canonical id, e.g. "proMedium" (ui.set {theme} / window.theme.<id>). */
const char *pc_ui_theme_kind_id(pc_ui_theme_kind kind);

/* Photoshop-grammar layout (the four Pro brightness levels). */
bool pc_ui_theme_kind_is_pro(pc_ui_theme_kind kind);

/* The next theme in the cycle (wraps). */
pc_ui_theme_kind pc_ui_theme_kind_next(pc_ui_theme_kind kind);

/* Parse a user/automation name (case- and separator-insensitive, plus aliases). Returns true and
 * writes *out on a match; leaves *out untouched otherwise. */
bool pc_ui_theme_kind_from_name(const char *s, pc_ui_theme_kind *out);

/* Parse a serde variant name ("Pro", "ProMedium", ...). Returns true on an exact match. */
bool pc_ui_theme_kind_from_serde(const char *s, pc_ui_theme_kind *out);

/* Thin RGB scope outline / filled-band colours (histogram, vectorscope). mask bit 0=R,1=G,2=B. */
pc_ui_color pc_ui_tokens_histogram_color(const pc_ui_tokens *t, uint8_t mask);
pc_ui_color pc_ui_tokens_histogram_fill(const pc_ui_tokens *t, uint8_t mask);

#endif
