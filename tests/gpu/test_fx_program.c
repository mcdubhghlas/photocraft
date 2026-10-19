#include <math.h>
#include <string.h>
#include "harness.h"
#include "gpu/fx.h"

static void test_geometry_keys_and_windows(void)
{
    pc_effect effect;
    pc_effect_default_drop_shadow(&effect);
    effect.u.shadow.spread = .5f;
    const double anchor[2] = {0};
    pc_gpu_fx_program a = {0}, b = {0};
    CHECK(pc_gpu_fx_program_build(&effect, pc_global_light_default(), false, NULL, 0, anchor, &a, NULL) ==
          PC_OK);
    effect.u.shadow.color = pc_color_rgb(1, 0, 0);
    effect.u.shadow.common = pc_fx_common_new(PC_BLEND_SCREEN, .3f);
    CHECK(pc_gpu_fx_program_build(&effect, pc_global_light_default(), false, NULL, 0, anchor, &b, NULL) ==
          PC_OK);
    CHECK_EQ_MEM(a.key, b.key, 32);
    effect.u.shadow.size += 1;
    CHECK(pc_gpu_fx_program_build(&effect, pc_global_light_default(), false, NULL, 0, anchor, &b, NULL) ==
          PC_OK);
    CHECK(memcmp(a.key, b.key, 32) != 0);
    pc_rect windows[PC_GPU_FX_MAX_STAGES];
    pc_rect damage = pc_rect_make(0, 0, 10, 10), region = pc_rect_make(-100, -100, 1000, 1000);
    CHECK(pc_gpu_fx_windows(&a, damage, region, windows, NULL) == PC_OK);
    CHECK(a.count > 1);
    pc_rect final = windows[a.count - 1];
    CHECK(pc_rect_eq(pc_rect_intersect(final, pc_rect_inflate(damage, 7)), pc_rect_inflate(damage, 7)));
    CHECK(pc_rect_width(windows[0]) >= pc_rect_width(final));
    int temps[PC_GPU_FX_MAX_STAGES];
    size_t count = 0;
    CHECK(pc_gpu_fx_temporaries(&a, temps, &count, NULL) == PC_OK);
    CHECK(count <= 3);
    CHECK(temps[a.count - 1] == -1);
    uint8_t previous[32];
    memcpy(previous, b.key, 32);
    effect.u.shadow.noise = .15f;
    CHECK(pc_gpu_fx_program_build(&effect, pc_global_light_default(), false, NULL, 0, anchor, &b, NULL) ==
          PC_ERR_UNSUPPORTED);
    CHECK_EQ_MEM(previous, b.key, 32);
    effect.u.shadow.noise = 0;
    effect.u.shadow.distance = NAN;
    CHECK(pc_gpu_fx_program_build(&effect, pc_global_light_default(), false, NULL, 0, anchor, &b, NULL) ==
          PC_ERR_INVALID);
    CHECK_EQ_MEM(previous, b.key, 32);
    pc_gpu_fx_program_fini(&a);
    pc_gpu_fx_program_fini(&b);
    pc_effect_fini(&effect);
}
static void test_all_map_programs(void)
{
    const double anchor[2] = {12, -8};
    for (pc_effect_kind kind = PC_FX_DROP_SHADOW; kind < PC_FX_KIND_COUNT; kind++) {
        pc_effect effect = {.kind = kind};
        if (kind == PC_FX_DROP_SHADOW || kind == PC_FX_INNER_SHADOW) {
            pc_effect_default_drop_shadow(&effect);
            effect.kind = kind;
            effect.u.shadow.distance = 3;
            effect.u.shadow.size = 8;
            effect.u.shadow.spread = .4f;
        } else if (kind == PC_FX_INNER_GLOW || kind == PC_FX_OUTER_GLOW) {
            effect.u.glow.size = 8;
            effect.u.glow.spread = .3f;
            effect.u.glow.range = .5f;
            effect.u.glow.technique = kind == PC_FX_INNER_GLOW ? PC_GLOW_PRECISE : PC_GLOW_SOFTER;
        } else if (kind == PC_FX_STROKE) {
            effect.u.stroke.size = 4;
            effect.u.stroke.position = PC_STROKE_CENTER;
        } else if (kind == PC_FX_SATIN) {
            effect.u.satin.size = 6;
            effect.u.satin.distance = 4;
            effect.u.satin.angle = 35;
        } else if (kind == PC_FX_BEVEL_EMBOSS) {
            effect.u.bevel.size = 5;
            effect.u.bevel.depth = 1;
            effect.u.bevel.up = true;
            effect.u.bevel.style = PC_BEVEL_EMBOSS;
            effect.u.bevel.technique = PC_BEVEL_CHISEL_SOFT;
            effect.u.bevel.soften = 2;
            effect.u.bevel.use_global_light = true;
        }
        pc_gpu_fx_program program = {0};
        CHECK(pc_gpu_fx_program_build(&effect, pc_global_light_default(), false, NULL, 0, anchor, &program,
                                      NULL) == PC_OK);
        bool overlay = kind >= PC_FX_COLOR_OVERLAY && kind <= PC_FX_PATTERN_OVERLAY;
        CHECK(overlay ? program.count == 0 : program.count > 0);
        int temps[PC_GPU_FX_MAX_STAGES];
        size_t count;
        CHECK(pc_gpu_fx_temporaries(&program, temps, &count, NULL) == PC_OK);
        CHECK(count <= 4);
        pc_rect windows[PC_GPU_FX_MAX_STAGES];
        CHECK(pc_gpu_fx_windows(&program, pc_rect_make(2, 3, 4, 5), pc_rect_make(-30, -30, 50, 50), windows,
                                NULL) == PC_OK);
        for (size_t i = 0; i < program.count; i++) {
            CHECK(windows[i].x0 >= -30 && windows[i].y0 >= -30 && windows[i].x1 <= 50 && windows[i].y1 <= 50);
        }
        if (program.count) {
            pc_rect saved[PC_GPU_FX_MAX_STAGES];
            memcpy(saved, windows, sizeof saved);
            program.stages[0].a = (pc_gpu_fx_input){PC_GPU_FX_VALUE, program.count};
            CHECK(pc_gpu_fx_windows(&program, pc_rect_make(0, 0, 8, 8), pc_rect_make(0, 0, 8, 8), windows,
                                    NULL) == PC_ERR_INVALID);
            CHECK_EQ_MEM(saved, windows, sizeof saved);
        }
        pc_gpu_fx_program_fini(&program);
        pc_effect_fini(&effect);
    }
}
int main(int argc, char **argv)
{
    RUN(test_geometry_keys_and_windows);
    RUN(test_all_map_programs);
    return pc_t_finish();
}
