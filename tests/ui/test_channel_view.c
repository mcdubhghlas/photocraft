#include <stdlib.h>
#include "harness.h"
#include "ui/channel_view.h"
#include "engine/channel_display.h"
static void execute(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, params, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void test_channels_masks_and_caching(void)
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "channels", pc_size_make(4, 4), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.25f, .5f, .75f), NULL) == PC_OK);
    doc.layers[0].has_mask = true;
    pc_layer_mask_reveal_all(&doc.layers[0].mask);
    doc.layers[0].mask.enabled = false;
    doc.channels = calloc(1, sizeof *doc.channels);
    CHECK(doc.channels != NULL);
    pc_surface alpha;
    pc_surface_init(&alpha, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_alpha_channel_init(&doc.channels[0], "Alpha", &alpha, NULL) == PC_OK);
    doc.nchannels = 1;
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(s, &doc, NULL, &index, NULL) == PC_OK);
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    pc_ui_canvas_cache composite = {0};
    pc_ui_channel_cache channels = {0};
    CHECK(pc_ui_canvas_cache_update(&composite, st, NULL) == PC_OK);
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, false, NULL) == PC_OK && !channels.active);
    uint64_t generation = channels.generation;
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, false, NULL) == PC_OK &&
          channels.generation == generation);
    execute(s, "channel.target", "{\"channel\":\"green\"}");
    st = pc_engine_session_active(s);
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, false, NULL) == PC_OK && channels.active);
    for (size_t ch = 0; ch < 3; ch++) {
        CHECK_NEAR(channels.image.px[0][ch], .5, 1.f / 255);
    }
    CHECK(channels.image.px[0][3] == 1);
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, true, NULL) == PC_OK);
    CHECK_NEAR(channels.image.px[0][0], 0, 1.f / 255);
    CHECK_NEAR(channels.image.px[0][1], .5, 1.f / 255);
    execute(s, "channel.target", "{\"channel\":\"composite\"}");
    execute(s, "channel.setVisible", "{\"channel\":0,\"visible\":true}");
    st = pc_engine_session_active(s);
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, false, NULL) == PC_OK && channels.active);
    CHECK_NEAR(channels.image.px[0][0], .5, 1.f / 255);
    CHECK_NEAR(channels.image.px[0][1], 0, 1.f / 255);
    CHECK_NEAR(channels.image.px[0][3], .5, 1.f / 255);
    execute(s, "channel.setVisible", "{\"channel\":0,\"visible\":false}");
    execute(s, "view.layerMask", "{\"mode\":\"gray\"}");
    st = pc_engine_session_active(s);
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, false, NULL) == PC_OK);
    for (size_t ch = 0; ch < 4; ch++) {
        CHECK_NEAR(channels.image.px[0][ch], 1, 0);
    }
    execute(s, "view.layerMask", "{\"mode\":\"overlay\"}");
    st = pc_engine_session_active(s);
    CHECK(pc_ui_channel_cache_update(&channels, st, &composite, false, NULL) == PC_OK);
    CHECK_NEAR(channels.image.px[0][3], 0, 0); /* reveal-all mask has no rubylith */
    pc_ui_channel_cache_fini(&channels);
    pc_ui_canvas_cache_fini(&composite);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_channels_masks_and_caching);
    return pc_t_finish();
}
