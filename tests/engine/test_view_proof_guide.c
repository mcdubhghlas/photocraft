/* View > Proof Setup variants, 32-bit Preview Options and the guide commands
 * (src/engine/view_proof_guide_cmds.c). Calls the module's specs() directly: builds a session and
 * document, then exercises each command's enabled() and run(), including graceful failure on no
 * document and hostile params (AGENTS.md rule 9). */
#include <string.h>

#include <yyjson.h>

#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/view_proof_guide_cmds.h"
#include "harness.h"

static const pc_engine_command *find(const char *id)
{
    size_t n = 0;
    const pc_engine_command *v = pc_engine_view_proof_guide_specs(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(v[i].id, id) == 0) {
            return &v[i];
        }
    }
    return NULL;
}

/* Parse a JSON params string into a doc; caller frees *pd. Returns the root val. */
static yyjson_val *parse(const char *json, yyjson_doc **pd)
{
    *pd = yyjson_read(json, strlen(json), 0);
    return *pd != NULL ? yyjson_doc_get_root(*pd) : NULL;
}

static pc_status call(const pc_engine_command *c, pc_engine_session *s, const char *params,
                      pc_json_value *out, pc_error *err)
{
    yyjson_doc *pd = NULL;
    yyjson_val *p = parse(params, &pd);
    pc_status rc = c->run(s, p, out, err);
    yyjson_doc_free(pd);
    return rc;
}

static pc_engine_session *mk_session(const char *mode, int depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char params[128];
    (void)snprintf(params, sizeof params, "{\"width\":16,\"height\":12,\"depth\":%d,\"mode\":\"%s\"}",
                   depth, mode);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.new", params, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static const pc_document *active_doc(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

static const char *str_at(pc_json_value *r, const char *key)
{
    return yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(r), key));
}
static double num_at(pc_json_value *r, const char *key)
{
    return yyjson_mut_get_num(yyjson_mut_obj_get(pc_json_value_root(r), key));
}
static bool bool_at(pc_json_value *r, const char *key)
{
    return yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(r), key));
}

static void test_no_document(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const char *ids[] = {"view.proofSetup.workingCyanPlate", "view.thirtyTwoBitPreviewOptions",
                         "view.clearGuides", "view.newGuide", "view.newGuideLayout"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        const pc_engine_command *c = find(ids[i]);
        CHECK(c != NULL);
        if (c == NULL) {
            continue;
        }
        pc_error reason = {0};
        CHECK(c->enabled(s, &reason) != PC_OK); /* disabled with no doc */
        pc_json_value out;
        pc_error err = {0};
        CHECK(call(c, s, "{}", &out, &err) != PC_OK); /* run must not crash, returns Err */
    }
    pc_engine_session_free(s);
}

static void test_proof_variants(void)
{
    struct {
        const char *id;
        const char *proof;
    } cases[] = {
        {"view.proofSetup.workingCyanPlate", "workingCyanPlate"},
        {"view.proofSetup.workingMagentaPlate", "workingMagentaPlate"},
        {"view.proofSetup.workingYellowPlate", "workingYellowPlate"},
        {"view.proofSetup.workingBlackPlate", "workingBlackPlate"},
        {"view.proofSetup.workingCmyPlate", "workingCmyPlate"},
        {"view.proofSetup.legacyMacintoshRgb", "legacyMacintoshRgb"},
        {"view.proofSetup.colorBlindnessProtanopia", "colorBlindnessProtanopia"},
        {"view.proofSetup.colorBlindnessDeuteranopia", "colorBlindnessDeuteranopia"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_engine_session *s = mk_session("rgb", 8);
        const pc_engine_command *c = find(cases[i].id);
        CHECK(c != NULL);
        if (c == NULL) {
            pc_engine_session_free(s);
            continue;
        }
        CHECK(c->enabled(s, NULL) == PC_OK);
        CHECK(!c->journal); /* proof-setup variants do not journal */
        pc_json_value out;
        pc_error err = {0};
        if (call(c, s, "{}", &out, &err) == PC_OK) {
            CHECK_EQ_STR(str_at(&out, "proof"), cases[i].proof);
            CHECK(bool_at(&out, "proofColors"));
            CHECK(str_at(&out, "profile") != NULL);
            pc_json_value_fini(&out);
        } else {
            PC_T_FAIL("%s failed: %s", cases[i].id, err.msg);
        }
        pc_engine_session_free(s);
    }
}

static void test_preview_32(void)
{
    const pc_engine_command *c = find("view.thirtyTwoBitPreviewOptions");
    CHECK(c != NULL);
    /* 8-bit: disabled, and run returns Err. */
    pc_engine_session *s8 = mk_session("rgb", 8);
    CHECK(c->enabled(s8, NULL) != PC_OK);
    pc_json_value o;
    pc_error e = {0};
    CHECK(call(c, s8, "{\"exposure\":1}", &o, &e) != PC_OK);
    pc_engine_session_free(s8);

    /* 32-bit: enabled; exposure/gamma clamped; bad method rejected; state merges. */
    pc_engine_session *s = mk_session("rgb", 32);
    CHECK(c->enabled(s, NULL) == PC_OK);
    pc_json_value out;
    pc_error err = {0};
    if (call(c, s, "{\"exposure\":1.0,\"gamma\":1.0}", &out, &err) == PC_OK) {
        CHECK_NEAR(num_at(&out, "exposure"), 1.0, 1e-9);
        CHECK_EQ_STR(str_at(&out, "method"), "exposureGamma");
        pc_json_value_fini(&out);
    } else {
        PC_T_FAIL("preview failed: %s", err.msg);
    }
    /* Clamp out-of-range values. */
    if (call(c, s, "{\"exposure\":99,\"gamma\":0}", &out, NULL) == PC_OK) {
        CHECK_NEAR(num_at(&out, "exposure"), 20.0, 1e-9);
        CHECK_NEAR(num_at(&out, "gamma"), 0.1, 1e-6);
        pc_json_value_fini(&out);
    }
    /* Only method given -> previous exposure/gamma preserved (merge). */
    if (call(c, s, "{\"method\":\"highlightCompression\"}", &out, NULL) == PC_OK) {
        CHECK_EQ_STR(str_at(&out, "method"), "highlightCompression");
        pc_json_value_fini(&out);
    }
    pc_error be = {0};
    CHECK(call(c, s, "{\"method\":\"nope\"}", &out, &be) != PC_OK);
    pc_engine_session_free(s);
}

static void test_new_and_clear_guides(void)
{
    pc_engine_session *s = mk_session("rgb", 8);
    const pc_engine_command *ng = find("view.newGuide");
    const pc_engine_command *cg = find("view.clearGuides");
    CHECK(ng != NULL && cg != NULL);
    CHECK(ng->journal && cg->journal);
    pc_json_value out;
    pc_error err = {0};

    /* Vertical guide at 8 px. */
    if (call(ng, s, "{\"orientation\":\"vertical\",\"position\":8}", &out, &err) == PC_OK) {
        pc_json_value_fini(&out);
    } else {
        PC_T_FAIL("new guide failed: %s", err.msg);
    }
    const pc_document *d = active_doc(s);
    CHECK_EQ_UINT(d->guides.nvertical, 1u);
    CHECK_NEAR(d->guides.vertical[0], 8.0, 1e-9);

    /* Horizontal guide with a unit and a colour. */
    if (call(ng, s, "{\"orientation\":\"horizontal\",\"position\":\"50%\",\"color\":\"magenta\"}",
             &out, NULL) == PC_OK) {
        pc_json_value_fini(&out);
    }
    d = active_doc(s);
    CHECK_EQ_UINT(d->guides.nhorizontal, 1u);
    CHECK_NEAR(d->guides.horizontal[0], 6.0, 1e-6); /* 50% of height 12 */

    /* Bad: unparseable position, missing position, bad colour. */
    pc_error e2 = {0};
    CHECK(call(ng, s, "{\"orientation\":\"vertical\",\"position\":\"abc\"}", &out, &e2) != PC_OK);
    CHECK(call(ng, s, "{\"orientation\":\"vertical\"}", &out, NULL) != PC_OK);
    CHECK(call(ng, s, "{\"position\":1,\"color\":\"notacolour\"}", &out, NULL) != PC_OK);

    /* Clear removes every guide. */
    if (call(cg, s, "{}", &out, NULL) == PC_OK) {
        pc_json_value_fini(&out);
    }
    d = active_doc(s);
    CHECK_EQ_UINT(d->guides.nvertical, 0u);
    CHECK_EQ_UINT(d->guides.nhorizontal, 0u);

    pc_engine_session_free(s);
}

static void test_new_guide_layout(void)
{
    pc_engine_session *s = mk_session("rgb", 8);
    const pc_engine_command *c = find("view.newGuideLayout");
    CHECK(c != NULL);
    CHECK(c->journal);
    pc_json_value out;
    pc_error err = {0};

    /* 2 columns -> both edges of each column: 4 vertical lines. */
    if (call(c, s, "{\"columns\":2}", &out, &err) == PC_OK) {
        CHECK(num_at(&out, "vertical") >= 2.0);
        pc_json_value_fini(&out);
    } else {
        PC_T_FAIL("guide layout failed: %s", err.msg);
    }
    const pc_document *d = active_doc(s);
    CHECK(d->guides.nvertical >= 2);

    /* Empty request -> Err. */
    CHECK(call(c, s, "{}", &out, NULL) != PC_OK);
    /* Absurd count -> Err, no crash. */
    CHECK(call(c, s, "{\"columns\":9999}", &out, NULL) != PC_OK);
    /* Bad margin array length -> Err. */
    CHECK(call(c, s, "{\"margin\":[1,2,3]}", &out, NULL) != PC_OK);
    /* Margins larger than the canvas -> Err. */
    CHECK(call(c, s, "{\"margin\":100}", &out, NULL) != PC_OK);

    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_no_document);
    RUN(test_proof_variants);
    RUN(test_preview_32);
    RUN(test_new_and_clear_guides);
    RUN(test_new_guide_layout);
    return pc_t_finish();
}
