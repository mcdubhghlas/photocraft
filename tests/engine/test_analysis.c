/* Image > Analysis: measurement scale, data points, Ruler, Count, Record Measurements, the
 * Measurement Log, plus an adversarial-params sweep over every registered analysis command. */
#include <string.h>

#include <yyjson.h>

#include "doc/doc.h"
#include "engine/engine.h"
#include "harness.h"

static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)active;
    (void)ctx;
    pc_surface *surf = pc_layer_surface_mut(&doc->layers[0]);
    size_t ch = pc_surface_channels(surf);
    float white[5] = {1, 1, 1, 1, 1};
    float gray[5] = {0.3f, 0.3f, 0.3f, 1, 1};
    (void)pc_surface_fill_rect(surf, pc_document_bounds(doc), white, ch, err);
    (void)pc_surface_fill_rect(surf, pc_rect_make(4, 4, 24, 24), gray, ch, err);
    return PC_OK;
}

static pc_engine_session *mk_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "file.new", "{\"width\":64,\"height\":48,\"depth\":8,\"mode\":\"rgb\"}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    return s;
}

static double num_at(pc_json_value *r, const char *key)
{
    return yyjson_mut_get_num(yyjson_mut_obj_get(pc_json_value_root(r), key));
}

static pc_status run(pc_engine_session *s, const char *id, const char *p, pc_json_value *out)
{
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, p, out, &err);
    if (rc != PC_OK) {
        PC_T_FAIL("%s failed: %s", id, err.msg);
    }
    return rc;
}

static void test_scale(void)
{
    pc_engine_session *s = mk_session();
    pc_json_value out;
    /* Query (no params) returns the default. */
    if (run(s, "image.analysis.setMeasurementScale", "{}", &out) == PC_OK) {
        CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "default")));
        pc_json_value_fini(&out);
    }
    /* Custom scale. */
    if (run(s, "image.analysis.setMeasurementScale",
            "{\"preset\":\"custom\",\"pixelLength\":100,\"logicalLength\":25,\"units\":\"mm\"}",
            &out) == PC_OK) {
        CHECK(num_at(&out, "factor") == 0.25);
        pc_json_value_fini(&out);
    }
    /* Bad: non-positive length. */
    pc_json_value o2;
    CHECK(pc_engine_session_execute_json(s, "image.analysis.setMeasurementScale",
                                         "{\"pixelLength\":0}", &o2, NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_data_points(void)
{
    pc_engine_session *s = mk_session();
    pc_json_value out;
    if (run(s, "image.analysis.selectDataPoints", "{\"selection\":{\"area\":false}}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    /* A bad key is an error, state unchanged. */
    CHECK(pc_engine_session_execute_json(s, "image.analysis.selectDataPoints",
                                         "{\"selection\":[\"bogus\"]}", &out, NULL) != PC_OK);
    if (run(s, "image.analysis.selectDataPoints", "{\"reset\":true}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void test_ruler(void)
{
    pc_engine_session *s = mk_session();
    pc_json_value out;
    if (run(s, "image.analysis.rulerTool", "{\"start\":[0,0],\"end\":[30,40]}", &out) == PC_OK) {
        yyjson_mut_val *info = yyjson_mut_obj_get(pc_json_value_root(&out), "info");
        CHECK(info != NULL);
        /* 3-4-5 triangle: length 50. */
        CHECK(yyjson_mut_get_real(yyjson_mut_obj_get(info, "l1")) == 50.0);
        pc_json_value_fini(&out);
    }
    /* Query keeps it. */
    if (run(s, "image.analysis.rulerTool", "{}", &out) == PC_OK) {
        CHECK(yyjson_mut_obj_get(pc_json_value_root(&out), "ruler") != NULL);
        pc_json_value_fini(&out);
    }
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc)->measurement.has_ruler);
    /* Clear. */
    if (run(s, "image.analysis.rulerTool", "{\"clear\":true}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    CHECK(!pc_doc_arc_get(pc_engine_session_active(s)->doc)->measurement.has_ruler);
    pc_engine_session_free(s);
}

static void test_count(void)
{
    pc_engine_session *s = mk_session();
    pc_json_value out;
    if (run(s, "count.add", "{\"x\":10,\"y\":10}", &out) == PC_OK) {
        CHECK(num_at(&out, "total") == 1);
        pc_json_value_fini(&out);
    }
    if (run(s, "count.add", "{\"x\":20,\"y\":20}", &out) == PC_OK) {
        CHECK(num_at(&out, "total") == 2);
        pc_json_value_fini(&out);
    }
    if (run(s, "count.newGroup", "{\"name\":\"Second\"}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (run(s, "count.setGroup", "{\"group\":0,\"markerSize\":5,\"visible\":false}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (run(s, "count.remove", "{\"index\":0,\"group\":0}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (run(s, "count.move", "{\"index\":0,\"group\":0,\"to\":[5,5]}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (run(s, "count.clear", "{\"group\":\"all\"}", &out) == PC_OK) {
        CHECK(num_at(&out, "total") == 0);
        pc_json_value_fini(&out);
    }
    if (run(s, "count.deleteGroup", "{\"group\":1}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (run(s, "image.analysis.countTool", "{}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void test_record_and_log(void)
{
    pc_engine_session *s = mk_session();
    pc_json_value out;
    /* Count source. */
    CHECK(run(s, "count.add", "{\"x\":10,\"y\":10}", &out) == PC_OK);
    pc_json_value_fini(&out);
    if (run(s, "image.analysis.recordMeasurements", "{\"source\":\"count\"}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    /* Ruler source. */
    CHECK(run(s, "image.analysis.rulerTool", "{\"start\":[0,0],\"end\":[30,40]}", &out) == PC_OK);
    pc_json_value_fini(&out);
    if (run(s, "image.analysis.recordMeasurements", "{\"source\":\"ruler\"}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    /* Selection source (a filled rect -> one feature). */
    CHECK(run(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":20,\"height\":20}", &out) == PC_OK);
    pc_json_value_fini(&out);
    if (run(s, "image.analysis.recordMeasurements", "{\"source\":\"selection\"}", &out) == PC_OK) {
        yyjson_mut_val *rows = yyjson_mut_obj_get(pc_json_value_root(&out), "rows");
        CHECK(rows != NULL && yyjson_mut_arr_size(rows) >= 1);
        pc_json_value_fini(&out);
    }
    /* Log has rows now. */
    if (run(s, "measurementLog.list", "{}", &out) == PC_OK) {
        yyjson_mut_val *rows = yyjson_mut_obj_get(pc_json_value_root(&out), "rows");
        CHECK(rows != NULL && yyjson_mut_arr_size(rows) >= 3);
        pc_json_value_fini(&out);
    }
    /* Export CSV (no path -> csv text). */
    if (run(s, "measurementLog.export", "{}", &out) == PC_OK) {
        const char *csv = yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "csv"));
        CHECK(csv != NULL && strstr(csv, "Label") != NULL);
        pc_json_value_fini(&out);
    }
    /* Delete all. */
    if (run(s, "measurementLog.delete", "{\"all\":true}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    /* Info. */
    if (run(s, "image.analysis.info", "{}", &out) == PC_OK) {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void test_deferred_report_errors(void)
{
    pc_engine_session *s = mk_session();
    pc_json_value out;
    /* Place Scale Marker and Straighten (needs a ruler) are deferred: graceful errors. */
    CHECK(pc_engine_session_execute_json(s, "image.analysis.placeScaleMarker", "{}", &out, NULL) !=
          PC_OK);
    pc_json_value r;
    CHECK(run(s, "image.analysis.rulerTool", "{\"start\":[0,0],\"end\":[30,10]}", &r) == PC_OK);
    pc_json_value_fini(&r);
    CHECK(pc_engine_session_execute_json(s, "image.analysis.straightenLayer", "{}", &out, NULL) !=
          PC_OK);
    pc_engine_session_free(s);
}

static bool is_analysis_id(const char *id)
{
    return strncmp(id, "image.analysis.", 15) == 0 || strncmp(id, "count.", 6) == 0 ||
           strncmp(id, "measurementLog.", 15) == 0;
}

static void test_adversarial_params(void)
{
    static const char *const PARAMS[] = {
        "{}",
        "null",
        "[]",
        "42",
        "{\"preset\":\"bogus\",\"pixelLength\":-1,\"units\":\"   \"}",
        "{\"start\":[1e400,0],\"end\":\"no\",\"protractor\":5}",
        "{\"x\":\"a\",\"y\":null,\"group\":-9,\"index\":1e30}",
        "{\"group\":99999,\"markerSize\":-5,\"labelSize\":1e9}",
        "{\"selection\":5,\"ruler\":{\"nope\":true}}",
        "{\"source\":\"bogus\"}",
        "{\"rows\":[\"x\",1,2],\"all\":\"maybe\"}",
        "{\"to\":[1]}",
    };
    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    size_t fuzzed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_analysis_id(all[i].id)) {
            continue;
        }
        for (size_t j = 0; j < sizeof PARAMS / sizeof PARAMS[0]; j++) {
            pc_engine_session *s = mk_session();
            pc_json_value out;
            pc_error err = {0};
            if (pc_engine_session_execute_json(s, all[i].id, PARAMS[j], &out, &err) == PC_OK) {
                pc_json_value_fini(&out);
            }
            fuzzed++;
            pc_engine_session_free(s);
        }
    }
    CHECK(fuzzed > 0);
}

int main(int argc, char **argv)
{
    RUN(test_scale);
    RUN(test_data_points);
    RUN(test_ruler);
    RUN(test_count);
    RUN(test_record_and_log);
    RUN(test_deferred_report_errors);
    RUN(test_adversarial_params);
    return pc_t_finish();
}
