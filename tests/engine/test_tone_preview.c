#include "harness.h"
#include "engine/tone_preview.h"
#include <math.h>
static void execute(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, params, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void test_curve_sampler_uses_level_units(void)
{
    const double identity[2][2] = {{0, 0}, {255, 255}}, lift[3][2] = {{0, 0}, {128, 192}, {255, 255}};
    float values[256];
    CHECK(pc_engine_tone_curve_sample(identity, 2, values, NULL) == PC_OK);
    for (size_t i = 0; i < 256; i++)
        CHECK(fabsf(values[i] - (float)i / 255) < 1e-5f);
    CHECK(pc_engine_tone_curve_sample(lift, 3, values, NULL) == PC_OK);
    CHECK(fabsf(values[128] - 192.f / 255) < 1e-4f);
    CHECK(values[64] > .3f && values[64] < .6f);
    const double bad[2][2] = {{0, 0}, {NAN, 255}};
    CHECK(pc_engine_tone_curve_sample(bad, 2, values, NULL) != PC_OK);
}
static void test_histograms_mode_channels_and_target_surfaces(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    const size_t channels[] = {4, 1, 5, 3};
    for (size_t m = 0; m < 4; m++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        char json[160];
        (void)snprintf(json, sizeof json,
                       "{\"width\":32,\"height\":24,\"mode\":\"%s\",\"depth\":16,\"background\":\"white\"}",
                       modes[m]);
        execute(s, "file.new", json);
        pc_engine_tone_histogram h = {0};
        CHECK(pc_engine_tone_histogram_compute(s, false, &h, NULL) == PC_OK);
        CHECK(h.channels == channels[m]);
        for (size_t c = 0; c < h.channels; c++) {
            uint32_t sum = 0;
            for (size_t i = 0; i < 256; i++)
                sum += h.bins[c][i];
            CHECK(sum == 32 * 24);
        }
        execute(s, "channel.new", "{\"fill\":\"white\"}");
        CHECK(pc_engine_tone_histogram_compute(s, false, &h, NULL) == PC_OK);
        CHECK(h.grayscale && h.channels == 1 && h.bins[0][255] == 32 * 24);
        execute(s, "image.adjustments.invert", "{}");
        CHECK(pc_engine_tone_histogram_compute(s, false, &h, NULL) == PC_OK);
        CHECK(h.bins[0][0] == 32 * 24);
        CHECK(pc_engine_session_undo(s));
        CHECK(pc_engine_tone_histogram_compute(s, false, &h, NULL) == PC_OK);
        CHECK(h.bins[0][255] == 32 * 24);
        pc_engine_session_free(s);
    }
}
int main(int argc, char **argv)
{
    RUN(test_curve_sampler_uses_level_units);
    RUN(test_histograms_mode_channels_and_target_surfaces);
    return pc_t_finish();
}
