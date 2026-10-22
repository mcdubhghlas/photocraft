/* The Color Settings open policy applied on import (pc_engine_color_open_policy) and the Embedded
 * Profile Mismatch report it produces for the UI / color.profileMismatch. Covers: untagged files,
 * a profile mismatch under each policy (preserve / convert / off), and that file.openAs surfaces the
 * report in its `color` field. Never crashes on an odd document. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/blob.h"
#include "base/buf.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    return s;
}

/* An RGB U8 document, optionally tagged with the bytes of a built-in profile. */
static void mk_doc(pc_document *doc, pc_builtin tag, bool tagged)
{
    CHECK(pc_document_with_background(doc, "t", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(0.5f, 0.5f, 0.5f, 1.0f),
                                      NULL) == PC_OK);
    if (tagged) {
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_profile_to_bytes(pc_builtin_profile(tag), &b, NULL) == PC_OK);
        pc_blob *blob = NULL;
        CHECK(pc_blob_new(b.data, b.len, &blob, NULL) == PC_OK);
        pc_buf_fini(&b);
        doc->icc_profile = blob;
    }
}

static const char *action_of(pc_json_value *o)
{
    yyjson_mut_val *r = pc_json_value_root(o);
    return r != NULL ? yyjson_mut_get_str(yyjson_mut_obj_get(r, "action")) : NULL;
}

static bool bool_of(pc_json_value *o, const char *k, bool dflt)
{
    yyjson_mut_val *r = pc_json_value_root(o);
    yyjson_mut_val *v = r != NULL ? yyjson_mut_obj_get(r, k) : NULL;
    return v != NULL ? yyjson_mut_get_bool(v) : dflt;
}

/* Untagged RGB whose working space is the default (sRGB): assumed to be the working space, not
 * tagged, and reported "untagged" + missing. */
static void test_untagged(void)
{
    pc_engine_session *s = mk();
    pc_document doc;
    mk_doc(&doc, PC_BUILTIN_SRGB, false);
    pc_json_value rep = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_color_open_policy(s, &doc, false, &rep, NULL) == PC_OK);
    const char *a = action_of(&rep);
    CHECK(a != NULL && strcmp(a, "untagged") == 0);
    CHECK(bool_of(&rep, "missing", false));
    CHECK(doc.icc_profile == NULL); /* default working: not tagged */
    pc_json_value_fini(&rep);
    pc_document_fini(&doc);
    pc_engine_session_free(s);
}

/* A document tagged Adobe RGB while the working space is sRGB: a mismatch. The default policy
 * Preserve keeps the embedded profile and asks. */
static void test_mismatch_preserve(void)
{
    pc_engine_session *s = mk();
    pc_document doc;
    mk_doc(&doc, PC_BUILTIN_ADOBE_RGB_COMPAT, true);
    pc_json_value rep = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_color_open_policy(s, &doc, false, &rep, NULL) == PC_OK);
    const char *a = action_of(&rep);
    CHECK(a != NULL && strcmp(a, "kept") == 0);
    CHECK(bool_of(&rep, "mismatch", false));
    CHECK(bool_of(&rep, "ask", false)); /* ask_on_mismatch defaults true */
    CHECK(doc.icc_profile != NULL);     /* preserved */
    pc_json_value_fini(&rep);
    pc_document_fini(&doc);
    pc_engine_session_free(s);
}

/* Convert policy converts the pixels to the working space. */
static void test_mismatch_convert(void)
{
    pc_engine_session *s = mk();
    s->color.settings.policy_rgb = PC_COLOR_POLICY_CONVERT;
    pc_document doc;
    mk_doc(&doc, PC_BUILTIN_ADOBE_RGB_COMPAT, true);
    pc_json_value rep = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_color_open_policy(s, &doc, false, &rep, NULL) == PC_OK);
    const char *a = action_of(&rep);
    CHECK(a != NULL && strcmp(a, "converted") == 0);
    pc_json_value_fini(&rep);
    pc_document_fini(&doc);
    pc_engine_session_free(s);
}

/* Off policy discards the embedded profile. */
static void test_mismatch_off(void)
{
    pc_engine_session *s = mk();
    s->color.settings.policy_rgb = PC_COLOR_POLICY_OFF;
    pc_document doc;
    mk_doc(&doc, PC_BUILTIN_ADOBE_RGB_COMPAT, true);
    pc_json_value rep = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_color_open_policy(s, &doc, false, &rep, NULL) == PC_OK);
    const char *a = action_of(&rep);
    CHECK(a != NULL && strcmp(a, "discarded") == 0);
    CHECK(doc.icc_profile == NULL);
    pc_json_value_fini(&rep);
    pc_document_fini(&doc);
    pc_engine_session_free(s);
}

/* No embedded profile, working == default: a matching embedded sRGB tag is kept with mismatch=false
 * (so the dialog never fires). */
static void test_embedded_matches_working(void)
{
    pc_engine_session *s = mk();
    pc_document doc;
    mk_doc(&doc, PC_BUILTIN_SRGB, true);
    pc_json_value rep = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_color_open_policy(s, &doc, false, &rep, NULL) == PC_OK);
    const char *a = action_of(&rep);
    CHECK(a != NULL && strcmp(a, "kept") == 0);
    CHECK(!bool_of(&rep, "mismatch", true));
    pc_json_value_fini(&rep);
    pc_document_fini(&doc);
    pc_engine_session_free(s);
}

/* file.openAs surfaces the report in its `color` field. */
static void test_open_as_reports_color(void)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0') {
        base = "/dev/shm/pc-build";
    }
    char path[400];
    (void)snprintf(path, sizeof path, "%s/pc-openpolicy-%d.png", base, (int)getpid());

    pc_engine_session *w = mk();
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(w, "file.new", "{\"width\":8,\"height\":8}", &o, NULL) ==
          PC_OK);
    pc_json_value_fini(&o);
    const pc_engine_doc_state *d = pc_engine_session_active(w);
    CHECK(d != NULL && pc_engine_save_doc(pc_doc_arc_get(d->doc), path, NULL, NULL, NULL) == PC_OK);
    pc_engine_session_free(w);

    pc_engine_session *s = mk();
    char p[600];
    (void)snprintf(p, sizeof p, "{\"path\":\"%s\"}", path);
    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.openAs", p, &r, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&r);
    yyjson_mut_val *color = root != NULL ? yyjson_mut_obj_get(root, "color") : NULL;
    CHECK(color != NULL && yyjson_mut_is_obj(color));
    CHECK(yyjson_mut_get_str(yyjson_mut_obj_get(color, "action")) != NULL);
    pc_json_value_fini(&r);
    pc_engine_session_free(s);
    remove(path);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_untagged);
    RUN(test_mismatch_preserve);
    RUN(test_mismatch_convert);
    RUN(test_mismatch_off);
    RUN(test_embedded_matches_working);
    RUN(test_open_as_reports_color);
    return pc_t_finish();
}
