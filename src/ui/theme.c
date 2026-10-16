/* Themes and tokens (port of crates/ui-egui/src/theme.rs). */
#include "ui/theme.h"

#include <string.h>

/* egui Color32 constructors, unmultiplied. */
static pc_ui_color rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (pc_ui_color){r, g, b, 255};
}
static pc_ui_color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (pc_ui_color){r, g, b, a};
}
static pc_ui_color gray(uint8_t v)
{
    return (pc_ui_color){v, v, v, 255};
}
static pc_ui_color black_alpha(uint8_t a)
{
    return (pc_ui_color){0, 0, 0, a};
}
#define WHITE ((pc_ui_color){255, 255, 255, 255})
#define BLACK ((pc_ui_color){0, 0, 0, 255})
#define TRANSPARENT ((pc_ui_color){0, 0, 0, 0})

const pc_ui_theme_kind PC_UI_THEME_ALL[PC_UI_THEME_COUNT] = {
    PC_UI_THEME_PRO,           PC_UI_THEME_PRO_MEDIUM,     PC_UI_THEME_PRO_LIGHT_GRAY,
    PC_UI_THEME_PRO_LIGHT,     PC_UI_THEME_STUDIO,         PC_UI_THEME_STUDIO_LIGHT,
    PC_UI_THEME_CLASSIC,       PC_UI_THEME_SOLARIZED_DARK, PC_UI_THEME_ADWAITA,
    PC_UI_THEME_ADWAITA_DARK,  PC_UI_THEME_BREEZE_LIGHT,   PC_UI_THEME_BREEZE_DARK,
    PC_UI_THEME_SOFT_DARK,     PC_UI_THEME_SOFT_LIGHT,
};

static pc_ui_tokens pro_base(void)
{
    pc_ui_tokens t = {0};
    t.kind = PC_UI_THEME_PRO;
    t.chrome = rgb(50, 50, 50);
    t.canvas = rgb(40, 40, 40);
    t.canvas_dot = rgb(40, 40, 40);
    t.dock = rgb(30, 30, 30);
    t.card = rgb(50, 50, 50);
    t.card_border = rgb(30, 30, 30);
    t.field = rgb(36, 36, 36);
    t.field_border = rgb(74, 74, 74);
    t.hover = rgb(66, 66, 66);
    t.pressed = rgb(78, 78, 78);
    t.text = rgb(222, 222, 222);
    t.text_dim = rgb(178, 178, 178);
    t.text_faint = rgb(128, 128, 128);
    t.icon = rgb(200, 200, 200);
    t.accent = rgb(55, 142, 240);
    t.accent_soft = rgb(78, 78, 78);
    t.accent_border = rgb(55, 142, 240);
    t.accent_text = WHITE;
    t.separator = rgb(30, 30, 30);
    t.shadow = black_alpha(150);
    t.primary_bg = rgb(55, 142, 240);
    t.primary_text = WHITE;
    t.danger = rgb(236, 91, 98);
    t.warning = rgb(232, 176, 70);
    t.radius_sm = 3.0f;
    t.radius = 4.0f;
    t.radius_lg = 6.0f;
    t.bevel = false;
    t.pro = true;
    t.round_chips = false;
    t.tab_strip = rgb(38, 38, 38);
    t.row_selected = rgb(82, 82, 82);
    t.histogram_bg = rgb(40, 40, 40);
    t.histogram_level = 225;
    t.caption_close = rgb(196, 43, 28);
    t.caption_close_text = WHITE;
    t.scrim = black_alpha(110);
    return t;
}

pc_ui_tokens pc_ui_tokens_for_kind(pc_ui_theme_kind kind)
{
    pc_ui_tokens t;
    switch (kind) {
    case PC_UI_THEME_PRO_MEDIUM:
        t = pro_base();
        t.kind = kind;
        t.chrome = rgb(83, 83, 83);
        t.dock = rgb(66, 66, 66);
        t.card = rgb(83, 83, 83);
        t.card_border = rgb(66, 66, 66);
        t.field = rgb(69, 69, 69);
        t.field_border = rgb(104, 104, 104);
        t.hover = rgb(98, 98, 98);
        t.pressed = rgb(112, 112, 112);
        t.text = rgb(238, 238, 238);
        t.text_dim = rgb(212, 212, 212);
        t.text_faint = rgb(160, 160, 160);
        t.icon = rgb(226, 226, 226);
        t.accent_soft = rgb(110, 110, 110);
        t.separator = rgb(62, 62, 62);
        t.tab_strip = rgb(74, 74, 74);
        t.row_selected = rgb(107, 107, 107);
        return t;
    case PC_UI_THEME_PRO_LIGHT_GRAY:
        t = pro_base();
        t.kind = kind;
        t.chrome = rgb(184, 184, 184);
        t.canvas = rgb(160, 160, 160);
        t.canvas_dot = rgb(160, 160, 160);
        t.dock = rgb(168, 168, 168);
        t.card = rgb(184, 184, 184);
        t.card_border = rgb(160, 160, 160);
        t.field = rgb(214, 214, 214);
        t.field_border = rgb(140, 140, 140);
        t.hover = rgb(198, 198, 198);
        t.pressed = rgb(170, 170, 170);
        t.text = rgb(20, 20, 20);
        t.text_dim = rgb(52, 52, 52);
        t.text_faint = rgb(96, 96, 96);
        t.icon = rgb(38, 38, 38);
        t.accent_soft = rgb(208, 208, 208);
        t.accent_text = rgb(0, 66, 150);
        t.separator = rgb(158, 158, 158);
        t.shadow = black_alpha(70);
        t.tab_strip = rgb(170, 170, 170);
        t.row_selected = rgb(212, 212, 212);
        return t;
    case PC_UI_THEME_PRO_LIGHT:
        t = pro_base();
        t.kind = kind;
        t.chrome = rgb(240, 240, 240);
        t.canvas = rgb(214, 214, 214);
        t.canvas_dot = rgb(214, 214, 214);
        t.dock = rgb(226, 226, 226);
        t.card = rgb(240, 240, 240);
        t.card_border = rgb(214, 214, 214);
        t.field = WHITE;
        t.field_border = rgb(186, 186, 186);
        t.hover = rgb(226, 226, 226);
        t.pressed = rgb(206, 206, 206);
        t.text = rgb(20, 20, 20);
        t.text_dim = rgb(64, 64, 64);
        t.text_faint = rgb(120, 120, 120);
        t.icon = rgb(46, 46, 46);
        t.accent_soft = rgb(214, 214, 214);
        t.accent_text = rgb(16, 98, 196);
        t.separator = rgb(214, 214, 214);
        t.shadow = black_alpha(50);
        t.tab_strip = rgb(226, 226, 226);
        t.row_selected = rgb(204, 204, 204);
        return t;
    case PC_UI_THEME_STUDIO:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(20, 20, 21);
        t.canvas = rgb(14, 14, 15);
        t.canvas_dot = rgb(46, 46, 50);
        t.dock = rgb(17, 17, 18);
        t.card = rgb(26, 26, 28);
        t.card_border = rgb(40, 40, 44);
        t.field = rgb(35, 35, 38);
        t.field_border = rgb(52, 52, 57);
        t.hover = rgb(44, 44, 48);
        t.pressed = rgb(56, 56, 62);
        t.text = rgb(236, 236, 240);
        t.text_dim = rgb(150, 150, 158);
        t.text_faint = rgb(96, 96, 104);
        t.icon = rgb(196, 196, 204);
        t.accent = rgb(139, 124, 246);
        t.accent_soft = rgba(139, 124, 246, 46);
        t.accent_border = rgba(160, 148, 255, 110);
        t.accent_text = rgb(214, 208, 255);
        t.separator = rgb(38, 38, 42);
        t.shadow = black_alpha(140);
        t.primary_bg = rgb(246, 246, 248);
        t.primary_text = rgb(12, 12, 14);
        t.danger = rgb(240, 96, 96);
        t.warning = rgb(240, 190, 90);
        t.radius_sm = 6.0f;
        t.radius = 8.0f;
        t.radius_lg = 12.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(14, 14, 15);
        t.histogram_level = 225;
        t.caption_close = rgb(196, 43, 28);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_STUDIO_LIGHT:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(246, 246, 248);
        t.canvas = rgb(226, 226, 230);
        t.canvas_dot = rgb(200, 200, 206);
        t.dock = rgb(240, 240, 243);
        t.card = rgb(252, 252, 253);
        t.card_border = rgb(222, 222, 228);
        t.field = rgb(242, 242, 245);
        t.field_border = rgb(214, 214, 220);
        t.hover = rgb(232, 232, 237);
        t.pressed = rgb(220, 220, 226);
        t.text = rgb(24, 24, 28);
        t.text_dim = rgb(96, 96, 106);
        t.text_faint = rgb(150, 150, 160);
        t.icon = rgb(60, 60, 68);
        t.accent = rgb(108, 92, 231);
        t.accent_soft = rgba(108, 92, 231, 36);
        t.accent_border = rgba(108, 92, 231, 120);
        t.accent_text = rgb(80, 64, 200);
        t.separator = rgb(226, 226, 232);
        t.shadow = black_alpha(50);
        t.primary_bg = rgb(20, 20, 24);
        t.primary_text = rgb(250, 250, 252);
        t.danger = rgb(210, 60, 60);
        t.warning = rgb(190, 130, 20);
        t.radius_sm = 6.0f;
        t.radius = 8.0f;
        t.radius_lg = 12.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = gray(40);
        t.histogram_level = 240;
        t.caption_close = rgb(196, 43, 28);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_CLASSIC:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(212, 208, 200);
        t.canvas = rgb(128, 128, 128);
        t.canvas_dot = rgb(128, 128, 128);
        t.dock = rgb(212, 208, 200);
        t.card = rgb(212, 208, 200);
        t.card_border = rgb(128, 128, 128);
        t.field = WHITE;
        t.field_border = rgb(128, 128, 128);
        t.hover = rgb(226, 222, 214);
        t.pressed = rgb(190, 186, 178);
        t.text = BLACK;
        t.text_dim = rgb(64, 64, 64);
        t.text_faint = rgb(128, 128, 128);
        t.icon = BLACK;
        t.accent = rgb(10, 36, 106);
        t.accent_soft = rgb(10, 36, 106);
        t.accent_border = rgb(10, 36, 106);
        t.accent_text = WHITE;
        t.separator = rgb(128, 128, 128);
        t.shadow = black_alpha(0);
        t.primary_bg = rgb(212, 208, 200);
        t.primary_text = BLACK;
        t.danger = rgb(160, 0, 0);
        t.warning = rgb(128, 96, 0);
        t.radius_sm = 0.0f;
        t.radius = 0.0f;
        t.radius_lg = 0.0f;
        t.bevel = true;
        t.pro = false;
        t.round_chips = false;
        t.tab_strip = rgb(212, 208, 200);
        t.row_selected = rgb(10, 36, 106);
        t.histogram_bg = gray(40);
        t.histogram_level = 240;
        t.caption_close = rgb(196, 43, 28);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_ADWAITA:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = WHITE;
        t.canvas = rgb(222, 222, 223);
        t.canvas_dot = rgb(222, 222, 223);
        t.dock = rgb(235, 235, 237);
        t.card = WHITE;
        t.card_border = rgb(218, 218, 221);
        t.field = rgb(235, 235, 237);
        t.field_border = rgb(218, 218, 221);
        t.hover = rgb(226, 226, 229);
        t.pressed = rgb(212, 212, 216);
        t.text = rgb(50, 50, 54);
        t.text_dim = rgb(100, 100, 104);
        t.text_faint = rgb(155, 155, 160);
        t.icon = rgb(56, 56, 60);
        t.accent = rgb(53, 132, 228);
        t.accent_soft = rgba(53, 132, 228, 40);
        t.accent_border = rgba(53, 132, 228, 130);
        t.accent_text = rgb(28, 113, 216);
        t.separator = rgb(222, 222, 225);
        t.shadow = black_alpha(45);
        t.primary_bg = rgb(53, 132, 228);
        t.primary_text = WHITE;
        t.danger = rgb(192, 28, 40);
        t.warning = rgb(156, 110, 3);
        t.radius_sm = 6.0f;
        t.radius = 9.0f;
        t.radius_lg = 12.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = gray(40);
        t.histogram_level = 240;
        t.caption_close = rgb(212, 212, 216);
        t.caption_close_text = rgb(50, 50, 54);
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_ADWAITA_DARK:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(46, 46, 50);
        t.canvas = rgb(29, 29, 32);
        t.canvas_dot = rgb(29, 29, 32);
        t.dock = rgb(34, 34, 38);
        t.card = rgb(54, 54, 58);
        t.card_border = rgb(64, 64, 68);
        t.field = rgb(64, 64, 68);
        t.field_border = rgb(76, 76, 80);
        t.hover = rgb(72, 72, 76);
        t.pressed = rgb(86, 86, 90);
        t.text = WHITE;
        t.text_dim = rgb(192, 192, 194);
        t.text_faint = rgb(145, 145, 148);
        t.icon = rgb(235, 235, 237);
        t.accent = rgb(53, 132, 228);
        t.accent_soft = rgba(53, 132, 228, 60);
        t.accent_border = rgba(120, 174, 237, 140);
        t.accent_text = rgb(120, 174, 237);
        t.separator = rgb(24, 24, 27);
        t.shadow = black_alpha(140);
        t.primary_bg = rgb(53, 132, 228);
        t.primary_text = WHITE;
        t.danger = rgb(255, 123, 99);
        t.warning = rgb(248, 228, 92);
        t.radius_sm = 6.0f;
        t.radius = 9.0f;
        t.radius_lg = 12.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(29, 29, 32);
        t.histogram_level = 225;
        t.caption_close = rgb(72, 72, 76);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_BREEZE_LIGHT:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(219, 222, 226);
        t.canvas = rgb(219, 222, 226);
        t.canvas_dot = rgb(219, 222, 226);
        t.dock = rgb(224, 227, 231);
        t.card = rgb(236, 238, 241);
        t.card_border = rgb(203, 208, 213);
        t.field = rgb(249, 250, 252);
        t.field_border = rgb(184, 191, 198);
        t.hover = rgb(226, 230, 235);
        t.pressed = rgb(208, 214, 221);
        t.text = rgb(33, 37, 42);
        t.text_dim = rgb(54, 63, 72);
        t.text_faint = rgb(81, 90, 100);
        t.icon = rgb(52, 58, 64);
        t.accent = rgb(58, 172, 232);
        t.accent_soft = rgba(58, 172, 232, 48);
        t.accent_border = rgba(58, 172, 232, 170);
        t.accent_text = rgb(30, 94, 136);
        t.separator = rgb(214, 218, 222);
        t.shadow = black_alpha(45);
        t.primary_bg = rgb(35, 122, 180);
        t.primary_text = WHITE;
        t.danger = rgb(170, 50, 64);
        t.warning = rgb(153, 72, 0);
        t.radius_sm = 5.0f;
        t.radius = 5.0f;
        t.radius_lg = 6.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(33, 37, 42);
        t.histogram_level = 240;
        t.caption_close = rgb(213, 51, 60);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_BREEZE_DARK:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(38, 42, 48);
        t.canvas = rgb(17, 20, 24);
        t.canvas_dot = rgb(17, 20, 24);
        t.dock = rgb(26, 29, 34);
        t.card = rgb(29, 33, 38);
        t.card_border = rgb(52, 56, 61);
        t.field = rgb(38, 42, 48);
        t.field_border = rgb(68, 73, 79);
        t.hover = rgb(50, 54, 59);
        t.pressed = rgb(58, 63, 69);
        t.text = rgb(250, 251, 253);
        t.text_dim = rgb(200, 206, 213);
        t.text_faint = rgb(164, 170, 177);
        t.icon = rgb(236, 238, 240);
        t.accent = rgb(58, 172, 232);
        t.accent_soft = rgba(58, 172, 232, 40);
        t.accent_border = rgba(58, 172, 232, 160);
        t.accent_text = rgb(80, 184, 238);
        t.separator = rgb(46, 50, 54);
        t.shadow = black_alpha(150);
        t.primary_bg = rgb(30, 118, 176);
        t.primary_text = WHITE;
        t.danger = rgb(227, 111, 123);
        t.warning = rgb(244, 120, 6);
        t.radius_sm = 5.0f;
        t.radius = 5.0f;
        t.radius_lg = 6.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(17, 20, 24);
        t.histogram_level = 225;
        t.caption_close = rgb(213, 51, 60);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_SOFT_DARK:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(58, 58, 68);
        t.canvas = rgb(40, 40, 48);
        t.canvas_dot = rgb(40, 40, 48);
        t.dock = rgb(47, 47, 56);
        t.card = rgb(54, 54, 64);
        t.card_border = rgb(70, 70, 82);
        t.field = rgb(64, 64, 75);
        t.field_border = rgb(84, 84, 98);
        t.hover = rgb(68, 68, 80);
        t.pressed = rgb(74, 74, 88);
        t.text = rgb(238, 238, 246);
        t.text_dim = rgb(212, 212, 224);
        t.text_faint = rgb(190, 190, 206);
        t.icon = rgb(214, 214, 226);
        t.accent = rgb(138, 154, 232);
        t.accent_soft = rgba(138, 154, 232, 48);
        t.accent_border = rgba(138, 154, 232, 150);
        t.accent_text = rgb(190, 200, 252);
        t.separator = rgb(36, 36, 44);
        t.shadow = black_alpha(110);
        t.primary_bg = rgb(86, 104, 196);
        t.primary_text = WHITE;
        t.danger = rgb(250, 160, 172);
        t.warning = rgb(240, 200, 130);
        t.radius_sm = 7.0f;
        t.radius = 10.0f;
        t.radius_lg = 14.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(40, 40, 48);
        t.histogram_level = 220;
        t.caption_close = rgb(200, 72, 86);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(100);
        return t;
    case PC_UI_THEME_SOFT_LIGHT:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(232, 230, 236);
        t.canvas = rgb(218, 216, 224);
        t.canvas_dot = rgb(218, 216, 224);
        t.dock = rgb(226, 224, 231);
        t.card = rgb(240, 238, 243);
        t.card_border = rgb(208, 205, 215);
        t.field = rgb(250, 249, 252);
        t.field_border = rgb(190, 187, 201);
        t.hover = rgb(228, 226, 234);
        t.pressed = rgb(212, 209, 221);
        t.text = rgb(44, 42, 56);
        t.text_dim = rgb(62, 60, 76);
        t.text_faint = rgb(80, 78, 96);
        t.icon = rgb(64, 62, 80);
        t.accent = rgb(110, 128, 216);
        t.accent_soft = rgba(110, 128, 216, 44);
        t.accent_border = rgba(110, 128, 216, 170);
        t.accent_text = rgb(46, 58, 146);
        t.separator = rgb(210, 207, 218);
        t.shadow = black_alpha(40);
        t.primary_bg = rgb(80, 98, 190);
        t.primary_text = WHITE;
        t.danger = rgb(166, 44, 62);
        t.warning = rgb(140, 72, 0);
        t.radius_sm = 7.0f;
        t.radius = 10.0f;
        t.radius_lg = 14.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(44, 42, 56);
        t.histogram_level = 240;
        t.caption_close = rgb(200, 60, 72);
        t.caption_close_text = WHITE;
        t.scrim = black_alpha(90);
        return t;
    case PC_UI_THEME_SOLARIZED_DARK:
        t = (pc_ui_tokens){0};
        t.kind = kind;
        t.chrome = rgb(7, 54, 66);
        t.canvas = rgb(0, 43, 54);
        t.canvas_dot = rgb(10, 61, 74);
        t.dock = rgb(0, 37, 47);
        t.card = rgb(7, 54, 66);
        t.card_border = rgb(0, 37, 47);
        t.field = rgb(0, 37, 47);
        t.field_border = rgb(21, 80, 95);
        t.hover = rgb(14, 74, 90);
        t.pressed = rgb(18, 89, 107);
        t.text = rgb(131, 148, 150);
        t.text_dim = rgb(101, 123, 131);
        t.text_faint = rgb(88, 110, 117);
        t.icon = rgb(147, 161, 161);
        t.accent = rgb(38, 139, 210);
        t.accent_soft = rgba(38, 139, 210, 46);
        t.accent_border = rgba(38, 139, 210, 120);
        t.accent_text = rgb(253, 246, 227);
        t.separator = rgb(0, 37, 47);
        t.shadow = black_alpha(150);
        t.primary_bg = rgb(38, 139, 210);
        t.primary_text = rgb(253, 246, 227);
        t.danger = rgb(220, 50, 47);
        t.warning = rgb(181, 137, 0);
        t.radius_sm = 6.0f;
        t.radius = 8.0f;
        t.radius_lg = 12.0f;
        t.bevel = false;
        t.pro = false;
        t.round_chips = true;
        t.tab_strip = TRANSPARENT;
        t.row_selected = TRANSPARENT;
        t.histogram_bg = rgb(0, 43, 54);
        t.histogram_level = 225;
        t.caption_close = rgb(220, 50, 47);
        t.caption_close_text = rgb(253, 246, 227);
        t.scrim = black_alpha(110);
        return t;
    case PC_UI_THEME_PRO:
    default:
        return pro_base();
    }
}

static const char *const KIND_LABEL[PC_UI_THEME_COUNT] = {
    "Pro (Dark)",      "Pro (Medium Gray)", "Pro (Light Gray)", "Pro (Light)",
    "Studio (Dark)",   "Studio (Light)",    "Classic",          "Adwaita (Light)",
    "Adwaita (Dark)",  "Solarized Dark",    "Breeze Light",     "Breeze Dark",
    "Soft Dark",       "Soft Light",
};
static const char *const KIND_NAME[PC_UI_THEME_COUNT] = {
    "Pro",      "ProMedium",    "ProLightGray", "ProLight", "Studio",     "StudioLight", "Classic",
    "Adwaita",  "AdwaitaDark",  "SolarizedDark", "BreezeLight", "BreezeDark", "SoftDark", "SoftLight",
};
static const char *const KIND_ID[PC_UI_THEME_COUNT] = {
    "pro",      "proMedium",    "proLightGray", "proLight", "studio",     "studioLight", "classic",
    "adwaita",  "adwaitaDark",  "solarizedDark", "breezeLight", "breezeDark", "softDark", "softLight",
};

static bool valid(pc_ui_theme_kind k)
{
    return (unsigned)k < (unsigned)PC_UI_THEME_COUNT;
}

const char *pc_ui_theme_kind_label(pc_ui_theme_kind kind)
{
    return valid(kind) ? KIND_LABEL[kind] : KIND_LABEL[PC_UI_THEME_DEFAULT];
}
const char *pc_ui_theme_kind_name(pc_ui_theme_kind kind)
{
    return valid(kind) ? KIND_NAME[kind] : KIND_NAME[PC_UI_THEME_DEFAULT];
}
const char *pc_ui_theme_kind_id(pc_ui_theme_kind kind)
{
    return valid(kind) ? KIND_ID[kind] : KIND_ID[PC_UI_THEME_DEFAULT];
}

bool pc_ui_theme_kind_is_pro(pc_ui_theme_kind kind)
{
    return kind == PC_UI_THEME_PRO || kind == PC_UI_THEME_PRO_MEDIUM || kind == PC_UI_THEME_PRO_LIGHT_GRAY ||
           kind == PC_UI_THEME_PRO_LIGHT;
}

pc_ui_theme_kind pc_ui_theme_kind_next(pc_ui_theme_kind kind)
{
    for (int i = 0; i < PC_UI_THEME_COUNT; i++) {
        if (PC_UI_THEME_ALL[i] == kind) {
            return PC_UI_THEME_ALL[(i + 1) % PC_UI_THEME_COUNT];
        }
    }
    return PC_UI_THEME_ALL[0];
}

/* Lowercase and drop space/_/-/()/ for name matching (ThemeKind::from_name's normalisation). */
static void normalize(const char *s, char *out, size_t cap)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '_' || c == '-' || c == '(' || c == ')') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        out[j++] = (char)c;
    }
    out[j] = '\0';
}

bool pc_ui_theme_kind_from_name(const char *s, pc_ui_theme_kind *out)
{
    if (!s) {
        return false;
    }
    char n[64];
    normalize(s, n, sizeof n);
    static const struct {
        const char *alias;
        pc_ui_theme_kind kind;
    } ALIASES[] = {
        {"pro", PC_UI_THEME_PRO}, {"prodark", PC_UI_THEME_PRO}, {"photoshop", PC_UI_THEME_PRO}, {"dark", PC_UI_THEME_PRO},
        {"promedium", PC_UI_THEME_PRO_MEDIUM}, {"promediumgray", PC_UI_THEME_PRO_MEDIUM}, {"medium", PC_UI_THEME_PRO_MEDIUM},
        {"mediumgray", PC_UI_THEME_PRO_MEDIUM},
        {"prolightgray", PC_UI_THEME_PRO_LIGHT_GRAY}, {"lightgray", PC_UI_THEME_PRO_LIGHT_GRAY},
        {"mediumlight", PC_UI_THEME_PRO_LIGHT_GRAY},
        {"prolight", PC_UI_THEME_PRO_LIGHT}, {"prolightest", PC_UI_THEME_PRO_LIGHT}, {"lightest", PC_UI_THEME_PRO_LIGHT},
        {"studio", PC_UI_THEME_STUDIO}, {"studiodark", PC_UI_THEME_STUDIO},
        {"studiolight", PC_UI_THEME_STUDIO_LIGHT}, {"light", PC_UI_THEME_STUDIO_LIGHT},
        {"classic", PC_UI_THEME_CLASSIC}, {"win2000", PC_UI_THEME_CLASSIC}, {"retro", PC_UI_THEME_CLASSIC},
        {"adwaita", PC_UI_THEME_ADWAITA}, {"adwaitalight", PC_UI_THEME_ADWAITA}, {"gnome", PC_UI_THEME_ADWAITA},
        {"gnomelight", PC_UI_THEME_ADWAITA},
        {"adwaitadark", PC_UI_THEME_ADWAITA_DARK}, {"gnomedark", PC_UI_THEME_ADWAITA_DARK},
        {"solarizeddark", PC_UI_THEME_SOLARIZED_DARK}, {"solarized", PC_UI_THEME_SOLARIZED_DARK},
        {"breezelight", PC_UI_THEME_BREEZE_LIGHT}, {"breeze", PC_UI_THEME_BREEZE_LIGHT}, {"kde", PC_UI_THEME_BREEZE_LIGHT},
        {"kdelight", PC_UI_THEME_BREEZE_LIGHT}, {"plasma", PC_UI_THEME_BREEZE_LIGHT}, {"plasmalight", PC_UI_THEME_BREEZE_LIGHT},
        {"breezedark", PC_UI_THEME_BREEZE_DARK}, {"kdedark", PC_UI_THEME_BREEZE_DARK}, {"plasmadark", PC_UI_THEME_BREEZE_DARK},
        {"softdark", PC_UI_THEME_SOFT_DARK},
        {"softlight", PC_UI_THEME_SOFT_LIGHT},
    };
    for (size_t i = 0; i < sizeof ALIASES / sizeof ALIASES[0]; i++) {
        if (strcmp(n, ALIASES[i].alias) == 0) {
            *out = ALIASES[i].kind;
            return true;
        }
    }
    return false;
}

bool pc_ui_theme_kind_from_serde(const char *s, pc_ui_theme_kind *out)
{
    if (!s) {
        return false;
    }
    for (int i = 0; i < PC_UI_THEME_COUNT; i++) {
        if (strcmp(s, KIND_NAME[i]) == 0) {
            *out = (pc_ui_theme_kind)i;
            return true;
        }
    }
    return false;
}

pc_ui_color pc_ui_tokens_histogram_color(const pc_ui_tokens *t, uint8_t mask)
{
    uint8_t v = t->histogram_level;
    return (pc_ui_color){(mask & 1) ? v : 0, (mask & 2) ? v : 0, (mask & 4) ? v : 0, 255};
}

pc_ui_color pc_ui_tokens_histogram_fill(const pc_ui_tokens *t, uint8_t mask)
{
    /* egui gamma_multiply(0.3): every channel (incl. alpha) * 0.3, rounded (x + 0.5 as u8). */
    pc_ui_color c = pc_ui_tokens_histogram_color(t, mask);
    c.r = (uint8_t)((float)c.r * 0.3f + 0.5f);
    c.g = (uint8_t)((float)c.g * 0.3f + 0.5f);
    c.b = (uint8_t)((float)c.b * 0.3f + 0.5f);
    c.a = (uint8_t)((float)c.a * 0.3f + 0.5f);
    return c;
}
