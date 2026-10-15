/* Layer > Smart Objects contents commands (ET4, port of a subset of
 * crates/engine/src/smart_cmds.rs tests): Export / Replace / Relink / Update(All) Modified /
 * Convert to Embedded / Linked, plus the mandatory never-crash sweep over hostile params. Edit /
 * Save Contents (Session smart_links + multi-document) and Convert to Layers are deferred. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static char g_path[512];

static void tmp_path(void)
{
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    snprintf(g_path, sizeof g_path, "%s/pc_et4_smart_%ld.pcraft", dir, (long)getpid());
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(24, 24), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.5f, 0.3f, 0.7f, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    pc_json_value_fini(&out);
    return rc;
}

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
}

/* A smart object as the active layer (convert the background). */
static pc_engine_session *mk_smart(void)
{
    pc_engine_session *s = mk();
    ok(s, "layer.smartObjects.convertToSmartObject", "{}");
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.exportContents"));
    return s;
}

static void test_registration(void)
{
    pc_engine_session *s = mk_smart();
    /* On a smart object the has_smart contents commands and the has_doc Update All are enabled;
     * this proves they are registered. */
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.replaceContents"));
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.exportContents"));
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.relinkToFile"));
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.convertToLinked"));
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.updateAllModifiedContent"));
    pc_engine_session_free(s);
}

static void test_export_and_round_trip(void)
{
    tmp_path();
    pc_engine_session *s = mk_smart();
    /* Export the embedded contents to a file. */
    char json[640];
    snprintf(json, sizeof json, "{\"path\":\"%s\"}", g_path);
    ok(s, "layer.smartObjects.exportContents", json);
    /* The file exists and is non-empty. */
    FILE *f = fopen(g_path, "rb");
    CHECK(f != NULL);
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        CHECK(ftell(f) > 0);
        fclose(f);
    }
    /* Replace the contents from that file (decodes, swaps the source, re-renders). */
    ok(s, "layer.smartObjects.replaceContents", json);
    /* Relink to it (now a linked source). */
    ok(s, "layer.smartObjects.relinkToFile", json);
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.updateModifiedContent"));
    /* Update the linked content, then the whole document's linked content. */
    ok(s, "layer.smartObjects.updateModifiedContent", "{}");
    ok(s, "layer.smartObjects.updateAllModifiedContent", "{}");
    /* Back to embedded, then to linked again. */
    ok(s, "layer.smartObjects.convertToEmbedded", "{}");
    ok(s, "layer.smartObjects.convertToLinked", json);
    pc_engine_session_free(s);
    unlink(g_path);
}

static void test_convert_embedded_needs_linked(void)
{
    pc_engine_session *s = mk_smart();
    /* A freshly converted smart object is embedded: Convert to Embedded / Update Modified are off. */
    CHECK(!pc_engine_session_is_enabled(s, "layer.smartObjects.convertToEmbedded"));
    CHECK(!pc_engine_session_is_enabled(s, "layer.smartObjects.updateModifiedContent"));
    pc_engine_session_free(s);
}

static void test_adversarial_params(void)
{
    static const char *IDS[] = {
        "layer.smartObjects.replaceContents",     "layer.smartObjects.exportContents",
        "layer.smartObjects.relinkToFile",        "layer.smartObjects.updateModifiedContent",
        "layer.smartObjects.updateAllModifiedContent", "layer.smartObjects.convertToEmbedded",
        "layer.smartObjects.convertToLinked",
    };
    static const char *HOSTILE[] = {
        "{}",
        "null",
        "[]",
        "{\"path\":\"\"}",
        "{\"path\":123}",
        "{\"path\":\"/nonexistent/dir/does/not/exist/x.pcraft\"}",
        "{\"path\":\"/dev/null\"}",
        "{\"layer\":999999,\"path\":\"/tmp/x.pcraft\"}",
        "{\"layer\":\"nope\"}",
        "{\"path\":[\"x\"]}",
    };
    for (int smart = 0; smart < 2; smart++) {
        for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
            for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
                pc_engine_session *s = smart ? mk_smart() : mk();
                (void)exec(s, IDS[i], HOSTILE[j]);
                pc_engine_session_free(s);
            }
        }
    }
    CHECK(true);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registration);
    RUN(test_export_and_round_trip);
    RUN(test_convert_embedded_needs_linked);
    RUN(test_adversarial_params);
    return pc_t_finish();
}
