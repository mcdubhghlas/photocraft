/* Theme tokens (port of crates/ui-egui/src/theme.rs tests). */
#include "harness.h"
#include "ui/theme.h"

#include <math.h>

static bool col_eq(pc_ui_color a, pc_ui_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

/* theme.rs Tokens::dark(). */
static bool dark(pc_ui_theme_kind k)
{
    return k == PC_UI_THEME_STUDIO || k == PC_UI_THEME_PRO || k == PC_UI_THEME_PRO_MEDIUM ||
           k == PC_UI_THEME_SOLARIZED_DARK || k == PC_UI_THEME_ADWAITA_DARK || k == PC_UI_THEME_BREEZE_DARK ||
           k == PC_UI_THEME_SOFT_DARK;
}

/* WCAG 2 contrast ratio (theme.rs test helper). */
static float lin(uint8_t v)
{
    float c = (float)v / 255.0f;
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}
static float luminance(pc_ui_color c)
{
    return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}
static float contrast(pc_ui_color a, pc_ui_color b)
{
    float la = luminance(a), lb = luminance(b);
    float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

/* theme.rs Tokens::field_selection(). */
static pc_ui_color field_selection(const pc_ui_tokens *t)
{
    if (t->pro) {
        uint8_t v = dark(t->kind) ? 96 : 201;
        return (pc_ui_color){v, v, v, 255};
    }
    return t->accent_soft;
}

static void test_names_parse(void)
{
    pc_ui_theme_kind k;
    CHECK(pc_ui_theme_kind_from_name("Classic", &k) && k == PC_UI_THEME_CLASSIC);
    CHECK(pc_ui_theme_kind_from_name("studio (light)", &k) && k == PC_UI_THEME_STUDIO_LIGHT);
    CHECK(pc_ui_theme_kind_from_name("dark", &k) && k == PC_UI_THEME_PRO);
    CHECK(pc_ui_theme_kind_from_name("studio", &k) && k == PC_UI_THEME_STUDIO);
    CHECK(pc_ui_theme_kind_from_name("Pro (Medium Gray)", &k) && k == PC_UI_THEME_PRO_MEDIUM);
    CHECK(pc_ui_theme_kind_from_name("Solarized Dark", &k) && k == PC_UI_THEME_SOLARIZED_DARK);
    CHECK(pc_ui_theme_kind_from_name("solarized", &k) && k == PC_UI_THEME_SOLARIZED_DARK);
    CHECK(pc_ui_theme_kind_from_name("light gray", &k) && k == PC_UI_THEME_PRO_LIGHT_GRAY);
    CHECK(pc_ui_theme_kind_from_name("lightest", &k) && k == PC_UI_THEME_PRO_LIGHT);
    CHECK(pc_ui_theme_kind_from_name("Soft_Light", &k) && k == PC_UI_THEME_SOFT_LIGHT);
    CHECK(pc_ui_theme_kind_from_name("gnome dark", &k) && k == PC_UI_THEME_ADWAITA_DARK);
    CHECK(pc_ui_theme_kind_from_name("plasma-dark", &k) && k == PC_UI_THEME_BREEZE_DARK);
    CHECK(!pc_ui_theme_kind_from_name("neon", &k));
    CHECK(!pc_ui_theme_kind_from_name(NULL, &k));

    pc_ui_tokens m = pc_ui_tokens_for_kind(PC_UI_THEME_PRO_MEDIUM);
    CHECK(m.pro && dark(m.kind) && m.kind == PC_UI_THEME_PRO_MEDIUM && col_eq(m.card, (pc_ui_color){83, 83, 83, 255}));
    pc_ui_tokens s = pc_ui_tokens_for_kind(PC_UI_THEME_SOLARIZED_DARK);
    CHECK(!s.bevel && s.kind == PC_UI_THEME_SOLARIZED_DARK && col_eq(s.card, (pc_ui_color){7, 54, 66, 255}));
}

static void test_ids_and_serde(void)
{
    for (int i = 0; i < PC_UI_THEME_COUNT; i++) {
        pc_ui_theme_kind k = PC_UI_THEME_ALL[i];
        pc_ui_theme_kind back;
        /* from_name round-trips id and label for every theme. */
        CHECK(pc_ui_theme_kind_from_name(pc_ui_theme_kind_id(k), &back) && back == k);
        CHECK(pc_ui_theme_kind_from_name(pc_ui_theme_kind_label(k), &back) && back == k);
        CHECK(pc_ui_theme_kind_from_serde(pc_ui_theme_kind_name(k), &back) && back == k);
    }
    CHECK_EQ_STR(pc_ui_theme_kind_name(PC_UI_THEME_PRO_MEDIUM), "ProMedium");
    CHECK_EQ_STR(pc_ui_theme_kind_id(PC_UI_THEME_PRO_LIGHT_GRAY), "proLightGray");
    CHECK_EQ_STR(pc_ui_theme_kind_label(PC_UI_THEME_PRO_LIGHT), "Pro (Light)");
    /* next() wraps. */
    CHECK(pc_ui_theme_kind_next(PC_UI_THEME_ALL[PC_UI_THEME_COUNT - 1]) == PC_UI_THEME_ALL[0]);
    CHECK(pc_ui_theme_kind_next(PC_UI_THEME_PRO) == PC_UI_THEME_PRO_MEDIUM);
}

static void test_pro_levels_step(void)
{
    pc_ui_theme_kind levels[4] = {PC_UI_THEME_PRO, PC_UI_THEME_PRO_MEDIUM, PC_UI_THEME_PRO_LIGHT_GRAY,
                                  PC_UI_THEME_PRO_LIGHT};
    uint8_t prev = 0;
    for (int i = 0; i < 4; i++) {
        pc_ui_tokens t = pc_ui_tokens_for_kind(levels[i]);
        CHECK(t.pro && t.kind == levels[i] && pc_ui_theme_kind_is_pro(levels[i]));
        if (i > 0) {
            CHECK(t.card.r > prev); /* darkest to lightest */
        }
        prev = t.card.r;
    }
    CHECK(!pc_ui_theme_kind_is_pro(PC_UI_THEME_STUDIO));
    CHECK(!pc_ui_theme_kind_is_pro(PC_UI_THEME_CLASSIC));
    /* ProLightGray and ProLight are light (not dark), with the matching panel grey. */
    CHECK(!dark(PC_UI_THEME_PRO_LIGHT_GRAY) && !dark(PC_UI_THEME_PRO_LIGHT));
    CHECK(col_eq(pc_ui_tokens_for_kind(PC_UI_THEME_PRO_LIGHT_GRAY).card, (pc_ui_color){184, 184, 184, 255}));
    CHECK(col_eq(pc_ui_tokens_for_kind(PC_UI_THEME_PRO_LIGHT).card, (pc_ui_color){240, 240, 240, 255}));
}

static void test_field_selection(void)
{
    /* #3789: grey in monochrome Pro themes, accent elsewhere. */
    CHECK(col_eq(field_selection(&(pc_ui_tokens){.pro = true, .kind = PC_UI_THEME_PRO}), (pc_ui_color){96, 96, 96, 255}));
    pc_ui_tokens pl = pc_ui_tokens_for_kind(PC_UI_THEME_PRO_LIGHT);
    CHECK(col_eq(field_selection(&pl), (pc_ui_color){201, 201, 201, 255}));
    pc_ui_tokens st = pc_ui_tokens_for_kind(PC_UI_THEME_STUDIO);
    CHECK(col_eq(field_selection(&st), st.accent_soft));
    pc_ui_tokens cl = pc_ui_tokens_for_kind(PC_UI_THEME_CLASSIC);
    CHECK(col_eq(field_selection(&cl), cl.accent_soft));
}

static void test_contrast_holds(void)
{
    /* Studio body text is high contrast; Adwaita/Soft/Breeze AA-check a subset. */
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_STUDIO);
    CHECK(contrast(t.text, t.card) >= 4.5f);
    CHECK(contrast(t.text_dim, t.card) >= 3.0f);

    pc_ui_theme_kind aa[] = {PC_UI_THEME_ADWAITA, PC_UI_THEME_ADWAITA_DARK, PC_UI_THEME_SOFT_DARK,
                             PC_UI_THEME_SOFT_LIGHT, PC_UI_THEME_BREEZE_LIGHT, PC_UI_THEME_BREEZE_DARK,
                             PC_UI_THEME_PRO_LIGHT_GRAY, PC_UI_THEME_PRO_LIGHT};
    for (size_t i = 0; i < sizeof aa / sizeof aa[0]; i++) {
        pc_ui_tokens k = pc_ui_tokens_for_kind(aa[i]);
        pc_ui_color surfaces[] = {k.chrome, k.dock, k.card, k.field};
        for (size_t j = 0; j < 4; j++) {
            CHECK(contrast(k.text, surfaces[j]) >= 4.5f);
            CHECK(contrast(k.text_dim, surfaces[j]) >= 3.0f);
        }
    }
    /* Breeze surface order and blue accent. */
    for (int d = 0; d < 2; d++) {
        pc_ui_tokens b = pc_ui_tokens_for_kind(d ? PC_UI_THEME_BREEZE_DARK : PC_UI_THEME_BREEZE_LIGHT);
        int lc = b.canvas.r + b.canvas.g + b.canvas.b, ld = b.dock.r + b.dock.g + b.dock.b;
        int lcard = b.card.r + b.card.g + b.card.b, lf = b.field.r + b.field.g + b.field.b;
        CHECK(lc < ld && ld < lcard && lcard < lf);
        CHECK(b.accent.b > b.accent.g && b.accent.g > b.accent.r);
    }
}

static void test_histogram(void)
{
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_PRO);
    pc_ui_color red = pc_ui_tokens_histogram_color(&t, 1);
    CHECK(col_eq(red, (pc_ui_color){225, 0, 0, 255}));
    pc_ui_color fill = pc_ui_tokens_histogram_fill(&t, 1);
    CHECK_EQ_UINT(fill.r, 68); /* round(225*0.3)=68 */
    CHECK_EQ_UINT(fill.a, 77); /* round(255*0.3)=77 */
}

int main(int argc, char **argv)
{
    RUN(test_names_parse);
    RUN(test_ids_and_serde);
    RUN(test_pro_levels_step);
    RUN(test_field_selection);
    RUN(test_contrast_holds);
    RUN(test_histogram);
    return pc_t_finish();
}
