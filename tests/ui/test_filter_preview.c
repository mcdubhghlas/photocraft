#include "harness.h"
#include "ui/filter_dialog.h"
#include "ui/filter_preview.h"
#include "engine/preview.h"
#include "engine/channel_display.h"
#include <time.h>
#include <string.h>
static pc_status ex(pc_engine_session *s, const char *id, const char *text)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = pc_engine_session_execute_json(s, id, text, &out, &error);
    if (status != PC_OK)
        fprintf(stderr, "%s %s: %s\n", id, text, error.msg);
    pc_json_value_fini(&out);
    return status;
}
static pc_engine_session *fresh(const char *mode, unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char json[160];
    (void)snprintf(json, sizeof json, "{\"width\":32,\"height\":24,\"mode\":\"%s\",\"depth\":%u}", mode,
                   depth);
    CHECK(ex(s, "file.new", json) == PC_OK);
    return s;
}
static void set_number(yyjson_mut_doc *doc, const char *key, double number)
{
    CHECK(yyjson_mut_obj_put(yyjson_mut_doc_get_root(doc), yyjson_mut_strcpy(doc, key),
                             yyjson_mut_real(doc, number)));
}
static pc_status wait_ready(pc_ui_filter_preview *p, pc_engine_session *s, yyjson_mut_doc *d)
{
    pc_status status = PC_OK;
    for (size_t i = 0; i < 5000; i++) {
        status = pc_ui_filter_preview_update(p, s, yyjson_mut_doc_get_root(d), NULL);
        if (status != PC_OK || p->has_ready) {
            if (status != PC_OK)
                fprintf(stderr, "preview: %s\n", p->error.msg);
            return status;
        }
        struct timespec tick = {0, 1000000};
        (void)nanosleep(&tick, NULL);
    }
    CHECK(false);
    return PC_ERR_LIMIT;
}
static void test_preview_matches_commit_at_all_depths_and_modes(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    const unsigned depths[] = {8, 16, 32};
    for (size_t m = 0; m < 4; m++)
        for (size_t z = 0; z < 3; z++) {
            pc_engine_session *s = fresh(modes[m], depths[z]);
            yyjson_mut_doc *fields = NULL;
            CHECK(pc_ui_filter_dialog_fields(s, "image.adjustments.exposure", &fields, NULL) == PC_OK);
            if (!fields) {
                pc_engine_session_free(s);
                continue;
            }
            set_number(fields, "exposure", -1);
            const pc_engine_doc_state *before = pc_engine_session_active(s);
            uint64_t revision = before->revision;
            pc_doc_arc *original = pc_doc_arc_retain(before->doc);
            pc_ui_filter_preview p = {0};
            CHECK(wait_ready(&p, s, fields) == PC_OK);
            CHECK(p.result && p.generation == 1);
            CHECK(pc_engine_session_active(s)->revision == revision);
            CHECK(pc_document_eq(pc_doc_arc_get(original), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
            pc_buf params = PC_BUF_INIT;
            CHECK(pc_ui_filter_dialog_params(yyjson_mut_doc_get_root(fields), &params, NULL) == PC_OK);
            CHECK(ex(s, "image.adjustments.exposure", pc_buf_cstr(&params)) == PC_OK);
            if (p.result)
                CHECK(pc_document_eq(pc_doc_arc_get(p.result),
                                     pc_doc_arc_get(pc_engine_session_active(s)->doc)));
            CHECK(pc_ui_filter_preview_update(&p, s, yyjson_mut_doc_get_root(fields), NULL) != PC_OK);
            CHECK(!p.result);
            pc_buf_fini(&params);
            pc_doc_arc_release(original);
            pc_ui_filter_preview_fini(&p);
            yyjson_mut_doc_free(fields);
            pc_engine_session_free(s);
        }
}
static void test_channel_target_clone_and_scope_guard(void)
{
    pc_engine_session *s = fresh("rgb", 16);
    CHECK(ex(s, "channel.new", "{\"fill\":\"white\"}") == PC_OK);
    yyjson_mut_doc *fields = NULL;
    CHECK(pc_ui_filter_dialog_fields(s, "image.adjustments.exposure", &fields, NULL) == PC_OK);
    set_number(fields, "exposure", -1);
    pc_ui_filter_preview p = {0};
    CHECK(wait_ready(&p, s, fields) == PC_OK);
    CHECK(p.result != NULL);
    pc_engine_session *copy = NULL;
    CHECK(pc_engine_preview_clone(s, &copy, NULL) == PC_OK);
    bool quick = false;
    size_t alpha = 999;
    CHECK(pc_engine_targeted_channel(copy, &quick, &alpha));
    CHECK(!quick && alpha == 0);
    CHECK(ex(copy, "image.adjustments.exposure", "{\"exposure\":-1}") == PC_OK);
    if (p.result)
        CHECK(pc_document_eq(pc_doc_arc_get(p.result), pc_doc_arc_get(pc_engine_session_active(copy)->doc)));
    CHECK(ex(s, "channel.target", "{\"channel\":\"composite\"}") == PC_OK);
    CHECK(pc_ui_filter_dialog_guard(s, yyjson_mut_doc_get_root(fields), NULL) != PC_OK);
    pc_engine_session_free(copy);
    pc_ui_filter_preview_fini(&p);
    yyjson_mut_doc_free(fields);
    pc_engine_session_free(s);
}
static void test_latest_settings_cancel_and_failure(void)
{
    pc_engine_session *s = fresh("rgb", 8);
    yyjson_mut_doc *fields = NULL;
    CHECK(pc_ui_filter_dialog_fields(s, "filter.blur.gaussianBlur", &fields, NULL) == PC_OK);
    if (!fields) {
        pc_engine_session_free(s);
        return;
    }
    pc_ui_filter_preview p = {0};
    CHECK(pc_ui_filter_preview_update(&p, s, yyjson_mut_doc_get_root(fields), NULL) == PC_OK);
    set_number(fields, "radius", 2);
    CHECK(wait_ready(&p, s, fields) == PC_OK);
    CHECK(p.result != NULL);
    CHECK(pc_ui_filter_preview_update(&p, s, NULL, NULL) == PC_OK);
    CHECK(!p.result && !p.has_wanted);
    yyjson_mut_doc_free(fields);
    fields = NULL;
    CHECK(pc_ui_filter_dialog_fields(s, "image.adjustments.exposure", &fields, NULL) == PC_OK);
    CHECK(yyjson_mut_obj_put(yyjson_mut_doc_get_root(fields), yyjson_mut_strcpy(fields, "gamma"),
                             yyjson_mut_strcpy(fields, "gamma must be a number")));
    CHECK(wait_ready(&p, s, fields) != PC_OK);
    CHECK(p.has_ready && !p.result);
    set_number(fields, "gamma", 1);
    CHECK(wait_ready(&p, s, fields) == PC_OK);
    CHECK(p.result != NULL);
    pc_ui_filter_preview_fini(&p);
    yyjson_mut_doc_free(fields);
    pc_engine_session_free(s);
}
static void test_preview_clone_preserves_custom_pattern_library(void)
{
    pc_engine_session *s = fresh("rgb", 8);
    CHECK(ex(s, "edit.definePattern", "{\"name\":\"PreviewOnlyPattern\"}") == PC_OK);
    pc_engine_session *copy = NULL;
    CHECK(pc_engine_preview_clone(s, &copy, NULL) == PC_OK);
    CHECK(ex(copy, "layer.layerStyle.patternOverlay", "{\"pattern\":\"PreviewOnlyPattern\"}") == PC_OK);
    CHECK(pc_doc_arc_get(pc_engine_session_active(copy)->doc)->npatterns == 1);
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc)->npatterns == 0);
    pc_engine_session_free(copy);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_preview_clone_preserves_custom_pattern_library);
    RUN(test_preview_matches_commit_at_all_depths_and_modes);
    RUN(test_channel_target_clone_and_scope_guard);
    RUN(test_latest_settings_cancel_and_failure);
    return pc_t_finish();
}
