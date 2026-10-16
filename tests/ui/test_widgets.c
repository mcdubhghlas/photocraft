/* Theme-token -> Nuklear style mapping (port of the styling half of widgets.rs). No GL needed. */
#include "harness.h"
#include "ui/theme.h"
#include "ui/widgets.h"

static bool nk_eq(struct nk_color a, pc_ui_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void test_style_follows_tokens(void)
{
    for (int i = 0; i < PC_UI_THEME_COUNT; i++) {
        pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[i]);
        struct nk_context ctx;
        CHECK(nk_init_default(&ctx, NULL));
        pc_ui_apply_nk_style(&ctx, &t);
        /* The table entries flow into the matching style fields. */
        CHECK(nk_eq(ctx.style.text.color, t.text));
        CHECK(nk_eq(ctx.style.window.background, t.chrome));
        CHECK(nk_eq(ctx.style.button.hover.data.color, t.hover));
        CHECK(nk_eq(ctx.style.button.active.data.color, t.accent));
        /* Radii come from the tokens. */
        CHECK(ctx.style.button.rounding == t.radius_sm);
        CHECK(ctx.style.window.rounding == t.radius);
        nk_free(&ctx);
    }
}

static void test_color_helper(void)
{
    struct nk_color c = pc_ui_nk_color((pc_ui_color){10, 20, 30, 40});
    CHECK(c.r == 10 && c.g == 20 && c.b == 30 && c.a == 40);
}

int main(int argc, char **argv)
{
    RUN(test_style_follows_tokens);
    RUN(test_color_helper);
    return pc_t_finish();
}
