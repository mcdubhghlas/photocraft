/* Edit > Fill (port of a subset of crates/engine/src/fill_cmds.rs tests, E4). */
#include <stdlib.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(uint32_t w, uint32_t h, const char *bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char json[128];
    snprintf(json, sizeof json, "{\"width\":%u,\"height\":%u,\"background\":\"%s\"}", w, h, bg);
    CHECK(exec(s, "file.new", json) == PC_OK);
    return s;
}

static pc_status run(pc_engine_session *s, const char *json)
{
    return exec(s, "edit.fill", json);
}

static void px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, st->active_layer);
    const pc_surface *surf = pc_layer_surface(l);
    pc_surface_rgba(surf, x, y, out);
}

static bool close4(const float a[4], const float b[4])
{
    for (int i = 0; i < 4; i++) {
        if (fabsf(a[i] - b[i]) >= 1.5f / 255.0f) {
            return false;
        }
    }
    return true;
}

static void test_every_constant_contents(void)
{
    pc_engine_session *s = mk(40, 30, "transparent");
    /* tools.setColors is not registered yet (E10/E12); set the tool colours directly. */
    float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    memcpy(s->tools.foreground, red, sizeof red);
    memcpy(s->tools.background, blue, sizeof blue);
    struct {
        const char *c;
        float want[4];
    } cases[] = {
        {"foreground", {1, 0, 0, 1}},
        {"background", {0, 0, 1, 1}},
        {"black", {0, 0, 0, 1}},
        {"gray", {128.0f / 255, 128.0f / 255, 128.0f / 255, 1}},
        {"white", {1, 1, 1, 1}},
    };
    float got[4];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char json[64];
        snprintf(json, sizeof json, "{\"contents\":\"%s\"}", cases[i].c);
        CHECK(run(s, json) == PC_OK);
        px(s, 5, 5, got);
        CHECK(close4(got, cases[i].want));
    }
    CHECK(run(s, "{\"contents\":\"color\",\"color\":\"#00ff00\"}") == PC_OK);
    px(s, 5, 5, got);
    float green[4] = {0, 1, 0, 1};
    CHECK(close4(got, green));
    /* The old form: a colour, or the foreground. */
    CHECK(run(s, "{\"color\":\"#ffff00\"}") == PC_OK);
    px(s, 5, 5, got);
    float yellow[4] = {1, 1, 0, 1};
    CHECK(close4(got, yellow));
    pc_engine_session_free(s);
}

static void test_opacity_preserve_and_dissolve(void)
{
    pc_engine_session *s = mk(40, 30, "transparent");
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":20,\"height\":30}") == PC_OK);
    CHECK(run(s, "{\"contents\":\"white\"}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    /* Preserve Transparency: the empty right half stays empty. */
    CHECK(run(s, "{\"contents\":\"black\",\"opacity\":50,\"preserveTransparency\":true}") == PC_OK);
    float got[4];
    px(s, 5, 5, got);
    float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    CHECK(close4(got, grey));
    px(s, 30, 5, got);
    CHECK(got[3] == 0.0f);
    /* Without it the transparent half is filled. */
    CHECK(run(s, "{\"contents\":\"white\",\"opacity\":100}") == PC_OK);
    px(s, 30, 5, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f);
    /* Dissolve paints whole pixels only (some black, some untouched; none fractional on a row). */
    CHECK(run(s, "{\"contents\":\"black\",\"mode\":\"dissolve\",\"opacity\":50}") == PC_OK);
    int blacks = 0;
    for (int32_t x = 0; x < 40; x++) {
        for (int32_t y = 0; y < 30; y++) {
            px(s, x, y, got);
            if (got[0] < 0.01f) {
                blacks++;
            }
        }
    }
    CHECK(blacks > 300 && blacks < 900);
    pc_engine_session_free(s);
}

static void test_history_and_content_aware(void)
{
    pc_engine_session *s = mk(40, 30, "transparent");
    CHECK(run(s, "{\"contents\":\"white\"}") == PC_OK);
    CHECK(run(s, "{\"contents\":\"black\"}") == PC_OK);
    float got[4];
    /* History: back to state 1 (after the white fill). */
    CHECK(run(s, "{\"contents\":\"history\",\"state\":1}") == PC_OK);
    px(s, 5, 5, got);
    CHECK(got[0] == 1.0f && got[3] == 1.0f);
    /* The default source is the oldest state: the empty layer. */
    CHECK(run(s, "{\"contents\":\"history\"}") == PC_OK);
    px(s, 5, 5, got);
    CHECK(got[3] == 0.0f);
    CHECK(run(s, "{\"contents\":\"history\",\"state\":999}") != PC_OK);
    /* Content-Aware fills the hole from what is around it. */
    CHECK(run(s, "{\"contents\":\"color\",\"color\":\"#336699\"}") == PC_OK);
    CHECK(exec(s, "select.rect", "{\"x\":15,\"y\":10,\"width\":6,\"height\":6}") == PC_OK);
    CHECK(run(s, "{\"contents\":\"white\"}") == PC_OK);
    CHECK(run(s, "{\"contents\":\"contentAware\",\"colorAdaptation\":false}") == PC_OK);
    px(s, 17, 12, got);
    float base[4] = {0x33 / 255.0f, 0x66 / 255.0f, 0x99 / 255.0f, 1.0f};
    CHECK(close4(got, base));
    pc_engine_session_free(s);
}

static void test_bad_params(void)
{
    pc_engine_session *s = mk(40, 30, "transparent");
    const char *bad[] = {
        "{\"contents\":\"plaid\"}",
        "{\"contents\":3}",
        "{\"mode\":\"sideways\"}",
        "{\"mode\":\"passThrough\"}",
        "{\"opacity\":\"lots\"}",
        "{\"preserveTransparency\":\"yes\"}",
        "{\"contents\":\"contentAware\"}",
        "{\"contents\":\"history\",\"state\":-1}",
        "{\"contents\":\"contentAware\",\"colorAdaptation\":1}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(run(s, bad[i]) != PC_OK);
    }
    /* Everything selected leaves nothing to sample from. */
    CHECK(exec(s, "select.all", "{}") == PC_OK);
    CHECK(run(s, "{\"contents\":\"contentAware\"}") != PC_OK);
    /* Out-of-range opacity is clamped (not an error). */
    CHECK(run(s, "{\"contents\":\"white\",\"opacity\":1e300}") == PC_OK);
    /* Pattern is deferred: an unsupported error, never a crash. */
    CHECK(run(s, "{\"contents\":\"pattern\"}") != PC_OK);
    pc_engine_session_free(s);
}

/* Adversarial params against the registered command: never a crash, always a status. */
static void test_adversarial(void)
{
    pc_engine_session *s = mk(8, 8, "white");
    const char *cases[] = {
        "[]", "\"x\"", "3", "null", "{\"layer\":99999}", "{\"target\":{\"channel\":42}}",
        "{\"opacity\":[1,2]}", "{\"state\":{}}", "{\"color\":42}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        (void)exec(s, "edit.fill", cases[i]);
    }
    /* No document: an error, not a crash. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(exec(empty, "edit.fill", "{}") != PC_OK);
    pc_engine_session_free(empty);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_every_constant_contents);
    RUN(test_opacity_preserve_and_dissolve);
    RUN(test_history_and_content_aware);
    RUN(test_bad_params);
    RUN(test_adversarial);
    return pc_t_finish();
}
