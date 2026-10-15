/* File > New: creation, every parameter, background choices and adversarial-param rejection
 * (ported from the file.new assertions of crates/engine/src/document_preset_cmds.rs and
 * commands.rs). */
#include <math.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static const pc_document *new_doc(pc_engine_session *s, const char *json)
{
    pc_json_value out;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, "file.new", json, &out, &err);
    CHECK(rc == PC_OK);
    if (rc != PC_OK) {
        return NULL;
    }
    pc_json_value_fini(&out);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

static void test_registered_in_file_menu(void)
{
    const pc_engine_command *c = pc_engine_command_find("file.new");
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK_EQ_STR(c->label, "New\xe2\x80\xa6");
        CHECK_EQ_STR(c->shortcut, "Cmd+N");
        CHECK(c->menu_len == 1 && strcmp(c->menu[0], "File") == 0);
        CHECK(c->journal);
    }
}

static void test_defaults(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const pc_document *doc = new_doc(s, "{}");
    CHECK(doc != NULL);
    if (doc != NULL) {
        CHECK(doc->size.width == 1920 && doc->size.height == 1080);
        CHECK(doc->mode == PC_COLOR_MODE_RGB && doc->depth == PC_SAMPLE_TYPE_U8);
        CHECK(doc->resolution_dpi == 72.0f);
        CHECK_EQ_STR(pc_str_c(&doc->name), "Untitled");
    }
    pc_engine_session_free(s);
}

static void test_every_field(void)
{
    static const char *const modes[] = {"rgb", "gray", "cmyk", "lab"};
    static const pc_color_mode want[] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                         PC_COLOR_MODE_LAB};
    static const int depths[] = {8, 16, 32};
    static const pc_sample_type wantd[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t m = 0; m < 4; m++) {
        for (size_t d = 0; d < 3; d++) {
            pc_engine_session *s = NULL;
            CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
            char buf[256];
            snprintf(buf, sizeof buf,
                     "{\"width\":16,\"height\":24,\"resolution\":254.5,\"mode\":\"%s\",\"depth\":%d,"
                     "\"name\":\"Doc\"}",
                     modes[m], depths[d]);
            const pc_document *doc = new_doc(s, buf);
            CHECK(doc != NULL);
            if (doc != NULL) {
                CHECK(doc->size.width == 16 && doc->size.height == 24);
                CHECK(doc->resolution_dpi == 254.5f);
                CHECK(doc->mode == want[m] && doc->depth == wantd[d]);
                CHECK_EQ_STR(pc_str_c(&doc->name), "Doc");
            }
            pc_engine_session_free(s);
        }
    }
}

/* A size sent as a float (a UI field) is still that size, rounded (#254). */
static void test_float_sizes_rounded(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const pc_document *doc = new_doc(s, "{\"width\":512.6,\"height\":10.2}");
    CHECK(doc != NULL && doc->size.width == 513 && doc->size.height == 10);
    pc_engine_session_free(s);
}

static void pixel0(pc_engine_session *s, double out[4])
{
    pc_json_value v;
    CHECK(pc_engine_session_execute_json(s, "document.pixel", "{\"x\":0,\"y\":0}", &v, NULL) == PC_OK);
    yyjson_mut_val *arr = pc_json_value_root(&v);
    for (size_t i = 0; i < 4; i++) {
        out[i] = yyjson_mut_get_real(yyjson_mut_arr_get(arr, i));
    }
    pc_json_value_fini(&v);
}

/* The background choice and a captured colour are independent of the toolbox colour. */
static void test_background_choices(void)
{
    struct {
        const char *bg;
        float expect[4];
    } cases[] = {
        {"white", {1.0f, 1.0f, 1.0f, 1.0f}},
        {"black", {0.0f, 0.0f, 0.0f, 1.0f}},
        {"transparent", {0.0f, 0.0f, 0.0f, 0.0f}},
        {"#ff0000", {1.0f, 0.0f, 0.0f, 1.0f}},
    };
    for (size_t i = 0; i < 4; i++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        char buf[128];
        snprintf(buf, sizeof buf, "{\"width\":4,\"height\":4,\"depth\":32,\"background\":\"%s\"}", cases[i].bg);
        CHECK(new_doc(s, buf) != NULL);
        double px[4];
        pixel0(s, px);
        for (int k = 0; k < 4; k++) {
            CHECK(fabs(px[k] - (double)cases[i].expect[k]) < 0.001);
        }
        pc_engine_session_free(s);
    }
    /* A captured backgroundColor does not follow a later toolbox change. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    s->tools.background[0] = 1.0f;
    s->tools.background[1] = 0.0f;
    s->tools.background[2] = 1.0f;
    s->tools.background[3] = 1.0f;
    CHECK(new_doc(s, "{\"width\":4,\"height\":4,\"depth\":32,\"background\":\"backgroundColor\","
                     "\"backgroundColor\":[0.125,0.375,0.625]}") != NULL);
    double px[4];
    pixel0(s, px);
    CHECK(fabs(px[0] - 0.125) < 0.001 && fabs(px[1] - 0.375) < 0.001 && fabs(px[2] - 0.625) < 0.001);
    pc_engine_session_free(s);
}

/* Default backgroundColor falls back to the toolbox colour. */
static void test_background_color_defaults_to_toolbox(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    s->tools.background[0] = 0.25f;
    s->tools.background[1] = 0.5f;
    s->tools.background[2] = 0.75f;
    s->tools.background[3] = 1.0f;
    CHECK(new_doc(s, "{\"width\":4,\"height\":4,\"depth\":32,\"background\":\"backgroundColor\"}") != NULL);
    double px[4];
    pixel0(s, px);
    CHECK(fabs(px[0] - 0.25) < 0.001 && fabs(px[1] - 0.5) < 0.001 && fabs(px[2] - 0.75) < 0.001);
    pc_engine_session_free(s);
}

/* Adversarial params are rejected, and no document is created (never-crash). */
static void test_bad_params_rejected(void)
{
    static const char *const bad[] = {
        "{\"width\":0}",
        "{\"height\":-1}",
        "{\"width\":300001}",
        "{\"width\":\"wrong\"}",
        "{\"resolution\":0}",
        "{\"resolution\":30001}",
        "{\"resolution\":null}",
        "{\"mode\":\"indexed\"}",
        "{\"depth\":64}",
        "{\"depth\":\"16\"}",
        "{\"background\":\"invalid\"}",
        "{\"background\":\"backgroundColor\",\"backgroundColor\":[1,0]}",
        "{\"background\":\"backgroundColor\",\"backgroundColor\":[2,0,0]}",
        "{\"background\":\"backgroundColor\",\"backgroundColor\":null}",
        "{\"background\":\"backgroundColor\",\"backgroundColor\":\"red\"}",
        "{\"name\":12}",
        "{\"unknownKey\":1}",
        "[3]",
        "42",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        pc_json_value out;
        pc_error err = {0};
        CHECK(pc_engine_session_execute_json(s, "file.new", bad[i], &out, &err) != PC_OK);
        size_t n = 0;
        (void)pc_engine_session_documents(s, &n);
        CHECK(n == 0);
        pc_engine_session_free(s);
    }
}

/* Null params create the default document (a null object keeps the documented defaults). */
static void test_null_params_ok(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute(s, "file.new", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t n = 0;
    (void)pc_engine_session_documents(s, &n);
    CHECK(n == 1);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_registered_in_file_menu);
    RUN(test_defaults);
    RUN(test_every_field);
    RUN(test_float_sizes_rounded);
    RUN(test_background_choices);
    RUN(test_background_color_defaults_to_toolbox);
    RUN(test_bad_params_rejected);
    RUN(test_null_params_ok);
    return pc_t_finish();
}
