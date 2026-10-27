/* Ports the unit tests of contour_lib_cmds (layer.layerStyle.contours.*) and export_preset_cmds
 * (file.export.presets.*): library ops, the preferences-document round-trip, and param validation. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "engine/engine_internal.h"
#include "engine/prefs.h"
#include "harness.h"

static pc_engine_session *new_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    return s;
}

static pc_status exec(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    return rc;
}

/* Execute and return the result (caller fini). */
static pc_status exec_out(pc_engine_session *s, const char *id, const char *params, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, params, out, NULL);
}

static uint64_t uint_field(pc_json_value *v, const char *key)
{
    return yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(v), key));
}

/* ---- contour library ----------------------------------------------------------------------- */

static void test_contours_library_and_roundtrip(void)
{
    pc_engine_session *s = new_session();

    pc_json_value out = {0};
    CHECK(exec_out(s, "layer.layerStyle.contours.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(uint_field(&out, "count"), 0u);
    pc_json_value_fini(&out);

    CHECK(exec_out(s, "layer.layerStyle.contours.new", "{\"contour\":\"Linear\",\"name\":\"MyC\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "name")), "MyC");
    CHECK_EQ_UINT(uint_field(&out, "count"), 1u);
    pc_json_value_fini(&out);

    /* Same name again -> uniquified. */
    CHECK(exec_out(s, "layer.layerStyle.contours.new", "{\"contour\":\"Linear\",\"name\":\"MyC\"}", &out) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "name")), "MyC 2");
    CHECK_EQ_UINT(uint_field(&out, "count"), 2u);
    pc_json_value_fini(&out);

    /* Save all to base64 .shc. */
    CHECK(exec_out(s, "layer.layerStyle.contours.save", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(uint_field(&out, "count"), 2u);
    CHECK(uint_field(&out, "bytes") > 0u);
    CHECK(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "data")) != NULL);
    pc_json_value_fini(&out);

    /* Save to a file, then load it back (replace). */
    const char *tmp = getenv("TMPDIR");
    char path[512];
    (void)snprintf(path, sizeof path, "%s/pc-contours-test.shc", tmp != NULL ? tmp : "/tmp");
    char savep[600];
    (void)snprintf(savep, sizeof savep, "{\"path\":\"%s\"}", path);
    CHECK(exec(s, "layer.layerStyle.contours.save", savep) == PC_OK);
    char loadp[700];
    (void)snprintf(loadp, sizeof loadp, "{\"path\":\"%s\",\"mode\":\"replace\"}", path);
    CHECK(exec_out(s, "layer.layerStyle.contours.load", loadp, &out) == PC_OK);
    CHECK_EQ_UINT(uint_field(&out, "count"), 2u);
    pc_json_value_fini(&out);
    (void)remove(path);

    /* Preferences round-trip. */
    char *raw = pc_engine_session_prefs_to_json(s);
    CHECK(raw != NULL);
    pc_engine_session *restored = new_session();
    CHECK(pc_engine_session_load_prefs_json(restored, raw, strlen(raw), NULL) == PC_OK);
    free(raw);
    CHECK(exec_out(restored, "layer.layerStyle.contours.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(uint_field(&out, "count"), 2u);
    pc_json_value_fini(&out);
    pc_engine_session_free(restored);

    /* Delete. */
    CHECK(exec_out(s, "layer.layerStyle.contours.delete", "{\"name\":\"MyC\"}", &out) == PC_OK);
    CHECK_EQ_UINT(uint_field(&out, "count"), 1u);
    pc_json_value_fini(&out);

    /* Bad params. */
    CHECK(exec(s, "layer.layerStyle.contours.new", "{}") != PC_OK);
    CHECK(exec(s, "layer.layerStyle.contours.new", "{\"contour\":\"nope\"}") != PC_OK);
    CHECK(exec(s, "layer.layerStyle.contours.delete", "{\"name\":\"gone\"}") != PC_OK);
    CHECK(exec(s, "layer.layerStyle.contours.load", "{\"data\":\"not base64 shc!!\"}") != PC_OK);

    pc_engine_session_free(s);
}

/* ---- export presets ------------------------------------------------------------------------ */

static const char *SETTINGS_PIXELS =
    "{\"format\":\"jpg\",\"quality\":70,\"lossless\":false,\"transparency\":false,\"scale\":50,"
    "\"width\":640,\"height\":480,\"lockAspect\":true,\"sizing\":\"pixels\",\"metadata\":\"exif\","
    "\"profile\":\"srgb\",\"embedProfile\":false,\"tgaBits\":\"24\",\"resample\":\"nearest\"}";
static const char *SETTINGS_SCALE =
    "{\"format\":\"jpg\",\"quality\":70,\"lossless\":false,\"transparency\":false,\"scale\":50,"
    "\"width\":640,\"height\":480,\"lockAspect\":true,\"sizing\":\"scale\",\"metadata\":\"exif\","
    "\"profile\":\"srgb\",\"embedProfile\":false,\"tgaBits\":\"24\",\"resample\":\"nearest\"}";

static void test_export_presets_roundtrip(void)
{
    pc_engine_session *s = new_session();
    char buf[1024];

    (void)snprintf(buf, sizeof buf, "{\"name\":\"  Web JPEG  \",\"settings\":%s}", SETTINGS_PIXELS);
    CHECK(exec(s, "file.export.presets.save", buf) == PC_OK);
    (void)snprintf(buf, sizeof buf, "{\"name\":\"web jpeg\",\"settings\":%s}", SETTINGS_SCALE);
    pc_json_value out = {0};
    CHECK(exec_out(s, "file.export.presets.save", buf, &out) == PC_OK);
    yyjson_mut_val *pr = yyjson_mut_obj_get(pc_json_value_root(&out), "preset");
    yyjson_mut_val *set = yyjson_mut_obj_get(pr, "settings");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(set, "sizing")), "scale");
    pc_json_value_fini(&out);
    /* Same name (case-insensitive) replaced it. */
    CHECK(exec_out(s, "file.export.presets.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&out), "presets")), 1u);
    pc_json_value_fini(&out);

    /* Preferences round-trip. */
    char *raw = pc_engine_session_prefs_to_json(s);
    CHECK(raw != NULL);
    pc_engine_session *r = new_session();
    CHECK(pc_engine_session_load_prefs_json(r, raw, strlen(raw), NULL) == PC_OK);
    free(raw);
    CHECK(exec_out(r, "file.export.presets.list", "{}", &out) == PC_OK);
    yyjson_mut_val *arr = yyjson_mut_obj_get(pc_json_value_root(&out), "presets");
    CHECK_EQ_UINT(yyjson_mut_arr_size(arr), 1u);
    yyjson_mut_val *p0 = yyjson_mut_arr_get(arr, 0);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(p0, "name")), "web jpeg");
    yyjson_mut_val *s0 = yyjson_mut_obj_get(p0, "settings");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(s0, "format")), "jpg");
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(s0, "width")), 640u);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(s0, "embedProfile")));
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(s0, "metadata")), "exif");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(s0, "resample")), "nearest");
    pc_json_value_fini(&out);

    CHECK(exec_out(r, "file.export.presets.get", "{\"name\":\"WEB JPEG\"}", &out) == PC_OK);
    yyjson_mut_val *gs = yyjson_mut_obj_get(yyjson_mut_obj_get(pc_json_value_root(&out), "preset"), "settings");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(gs, "profile")), "srgb");
    pc_json_value_fini(&out);
    CHECK(exec(r, "file.export.presets.delete", "{\"name\":\"web jpeg\"}") == PC_OK);
    CHECK(exec_out(r, "file.export.presets.list", "{}", &out) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&out), "presets")), 0u);
    pc_json_value_fini(&out);
    pc_engine_session_free(r);
    pc_engine_session_free(s);
}

static void test_export_presets_validation_and_drop(void)
{
    pc_engine_session *s = new_session();
    CHECK(exec(s, "file.export.presets.save", "{}") != PC_OK);
    CHECK(exec(s, "file.export.presets.save", "{\"name\":\"  \",\"settings\":{}}") != PC_OK);
    CHECK(exec(s, "file.export.presets.save", "{\"name\":\"x\",\"settings\":{\"format\":\"bmp\"}}") != PC_OK);
    CHECK(exec(s, "file.export.presets.save",
               "{\"name\":\"x\",\"settings\":{\"sizing\":\"pixels\",\"width\":0,\"height\":10}}") != PC_OK);
    CHECK(exec(s, "file.export.presets.save", "{\"name\":\"x\",\"settings\":{\"scale\":0}}") != PC_OK);
    CHECK(exec(s, "file.export.presets.save", "{\"name\":\"x\",\"settings\":{\"metadata\":\"copyright\"}}") != PC_OK);
    CHECK(exec(s, "file.export.presets.save", "{\"name\":\"x\",\"settings\":{\"profile\":\"fogra39\"}}") != PC_OK);
    pc_error err = {0};
    pc_json_value out = {0};
    pc_status rc = pc_engine_session_execute_json(
        s, "file.export.presets.save", "{\"name\":\"x\",\"settings\":{\"resample\":\"lanczos\"}}", &out, &err);
    CHECK(rc != PC_OK);
    CHECK_CONTAINS(err.msg, "resample must be one of");
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    CHECK(exec(s, "file.export.presets.save", "{\"name\":\"x\",\"settings\":{\"resample\":3}}") != PC_OK);
    CHECK(exec(s, "file.export.presets.delete", "{\"name\":\"missing\"}") != PC_OK);
    CHECK(exec(s, "file.export.presets.get", "{\"name\":\"\"}") != PC_OK);

    /* A damaged saved entry is dropped on load; good ones kept; a preset saved before Resample
     * existed loads with the default. */
    const char *prefs =
        "{\"presets\":{\"export_presets\":["
        "{\"name\":\"ok\",\"settings\":{\"format\":\"png\",\"quality\":85,\"lossless\":false,"
        "\"transparency\":true,\"scale\":100.0,\"width\":0,\"height\":0,\"lockAspect\":true,"
        "\"sizing\":\"scale\",\"metadata\":\"none\",\"profile\":\"document\",\"embedProfile\":true,"
        "\"tgaBits\":\"auto\"}},"
        "{\"name\":\"broken\",\"settings\":{\"format\":\"nope\"}},"
        "\"not an object\"]}}";
    pc_engine_session *r = new_session();
    CHECK(pc_engine_session_load_prefs_json(r, prefs, strlen(prefs), NULL) == PC_OK);
    CHECK(exec_out(r, "file.export.presets.list", "{}", &out) == PC_OK);
    yyjson_mut_val *arr = yyjson_mut_obj_get(pc_json_value_root(&out), "presets");
    CHECK_EQ_UINT(yyjson_mut_arr_size(arr), 1u);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_arr_get(arr, 0), "name")), "ok");
    yyjson_mut_val *s0 = yyjson_mut_obj_get(yyjson_mut_arr_get(arr, 0), "settings");
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(s0, "resample")), "bicubicAutomatic");
    pc_json_value_fini(&out);
    pc_engine_session_free(r);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_contours_library_and_roundtrip);
    RUN(test_export_presets_roundtrip);
    RUN(test_export_presets_validation_and_drop);
    return pc_t_finish();
}
