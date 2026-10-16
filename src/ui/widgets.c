/* Theme-token -> Nuklear style mapping (partial port of crates/ui-egui/src/widgets.rs). */
#include "ui/widgets.h"

struct nk_color pc_ui_nk_color(pc_ui_color c)
{
    return nk_rgba(c.r, c.g, c.b, c.a);
}

void pc_ui_apply_nk_style(struct nk_context *ctx, const pc_ui_tokens *t)
{
    struct nk_color table[NK_COLOR_COUNT];
    table[NK_COLOR_TEXT] = pc_ui_nk_color(t->text);
    table[NK_COLOR_WINDOW] = pc_ui_nk_color(t->chrome);
    table[NK_COLOR_HEADER] = pc_ui_nk_color(t->card);
    table[NK_COLOR_BORDER] = pc_ui_nk_color(t->card_border);
    table[NK_COLOR_BUTTON] = pc_ui_nk_color(t->field);
    table[NK_COLOR_BUTTON_HOVER] = pc_ui_nk_color(t->hover);
    table[NK_COLOR_BUTTON_ACTIVE] = pc_ui_nk_color(t->accent);
    table[NK_COLOR_TOGGLE] = pc_ui_nk_color(t->field);
    table[NK_COLOR_TOGGLE_HOVER] = pc_ui_nk_color(t->hover);
    table[NK_COLOR_TOGGLE_CURSOR] = pc_ui_nk_color(t->accent);
    table[NK_COLOR_SELECT] = pc_ui_nk_color(t->card);
    table[NK_COLOR_SELECT_ACTIVE] = pc_ui_nk_color(t->accent);
    table[NK_COLOR_SLIDER] = pc_ui_nk_color(t->field);
    table[NK_COLOR_SLIDER_CURSOR] = pc_ui_nk_color(t->icon);
    table[NK_COLOR_SLIDER_CURSOR_HOVER] = pc_ui_nk_color(t->text);
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = pc_ui_nk_color(t->text);
    table[NK_COLOR_PROPERTY] = pc_ui_nk_color(t->field);
    table[NK_COLOR_EDIT] = pc_ui_nk_color(t->field);
    table[NK_COLOR_EDIT_CURSOR] = pc_ui_nk_color(t->text);
    table[NK_COLOR_COMBO] = pc_ui_nk_color(t->field);
    table[NK_COLOR_CHART] = pc_ui_nk_color(t->field);
    table[NK_COLOR_CHART_COLOR] = pc_ui_nk_color(t->accent);
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = pc_ui_nk_color(t->text);
    table[NK_COLOR_SCROLLBAR] = pc_ui_nk_color(t->dock);
    table[NK_COLOR_SCROLLBAR_CURSOR] = pc_ui_nk_color(t->field_border);
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = pc_ui_nk_color(t->hover);
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = pc_ui_nk_color(t->pressed);
    table[NK_COLOR_TAB_HEADER] = pc_ui_nk_color(t->tab_strip.a ? t->tab_strip : t->card);
    table[NK_COLOR_KNOB] = pc_ui_nk_color(t->field);
    table[NK_COLOR_KNOB_CURSOR] = pc_ui_nk_color(t->icon);
    table[NK_COLOR_KNOB_CURSOR_HOVER] = pc_ui_nk_color(t->text);
    table[NK_COLOR_KNOB_CURSOR_ACTIVE] = pc_ui_nk_color(t->text);
    nk_style_from_table(ctx, table);

    /* Corner radii from the tokens (egui's CornerRadius). */
    struct nk_style *s = &ctx->style;
    s->button.rounding = t->radius_sm;
    s->contextual_button.rounding = t->radius_sm;
    s->menu_button.rounding = t->radius_sm;
    s->selectable.rounding = t->radius_sm;
    s->slider.rounding = t->radius_sm;
    s->property.rounding = t->radius_sm;
    s->edit.rounding = t->radius_sm;
    s->combo.rounding = t->radius_sm;
    s->window.rounding = t->radius;
    s->window.group_border = 1.0f;
    s->tab.rounding = t->radius_sm;
}
