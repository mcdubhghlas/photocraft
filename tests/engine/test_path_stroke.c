#include "engine/engine_internal.h"
#include "harness.h"
static pc_engine_session *make_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document d;
    CHECK(pc_document_with_background(&d, "Stroke", pc_size_make(200, 60), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &d, NULL, NULL, NULL) == PC_OK);
    return s;
}
static pc_status execute(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = pc_engine_session_execute_json(s, id, p, &out, &error);
    if (status != PC_OK)
        fprintf(stderr, "%s: %s\n", id, error.msg);
    pc_json_value_fini(&out);
    return status;
}
static void pixel(pc_engine_session *s, int x, int y, float rgba[4])
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
    pc_surface_rgba(&l->content.u.raster, x, y, rgba);
}
static void test_fill_and_pencil(void)
{
    pc_engine_session *s = make_session();
    CHECK(execute(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(execute(s, "path.set",
                  "{\"path\":{\"subpaths\":[{\"knots\":[[10,10],[60,10],[60,50],[10,50]]}]}}") == PC_OK);
    CHECK(execute(s, "path.fill", "{\"color\":\"#ff0000\",\"opacity\":50}") == PC_OK);
    float p[4];
    pixel(s, 30, 30, p);
    CHECK(p[0] > .99f && p[3] > .49f && p[3] < .51f);
    CHECK(execute(s, "path.stroke",
                  "{\"tool\":\"pencil\",\"size\":4,\"color\":\"#0000ff\",\"opacity\":100}") == PC_OK);
    pixel(s, 10, 30, p);
    CHECK(p[2] > .9f);
    pixel(s, 60, 30, p);
    CHECK(p[2] > .9f);
    pixel(s, 30, 30, p);
    CHECK(p[2] < .1f);
    CHECK(execute(s, "path.stroke", "{\"tool\":\"airbrush\"}") != PC_OK);
    CHECK(execute(s, "path.stroke", "{\"simulatePressure\":\"yes\"}") != PC_OK);
    CHECK(execute(s, "path.stroke", "{\"size\":1e300}") != PC_OK);
    pc_engine_session_free(s);
}
static size_t coverage(bool pressure, int x)
{
    pc_engine_session *s = make_session();
    CHECK(execute(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(execute(s, "path.set",
                  "{\"path\":{\"subpaths\":[{\"closed\":false,\"knots\":[[20,30],[180,30]]}]}}") == PC_OK);
    CHECK(execute(s, "path.stroke",
                  pressure ? "{\"size\":20,\"hardness\":1,\"simulatePressure\":true}"
                           : "{\"size\":20,\"hardness\":1}") == PC_OK);
    size_t count = 0;
    float p[4];
    for (int y = 0; y < 60; y++) {
        pixel(s, x, y, p);
        count += p[3] > .5f;
    }
    pc_engine_session_free(s);
    return count;
}
static void test_pressure(void)
{
    size_t end = coverage(true, 26), middle = coverage(true, 100);
    CHECK(middle >= 18);
    CHECK(end * 3 < middle);
    end = coverage(false, 26);
    middle = coverage(false, 100);
    CHECK(middle >= 18);
    CHECK(end + 2 >= middle);
}
static void test_blur_compound_undo(void)
{
    pc_engine_session *s = make_session();
    CHECK(execute(s, "path.set", "{\"path\":{\"subpaths\":[{\"knots\":[[0,0],[50,0],[50,60],[0,60]]}]}}") ==
          PC_OK);
    CHECK(execute(s, "path.fill", "{\"color\":\"#000000\"}") == PC_OK);
    CHECK(execute(s, "path.set",
                  "{\"path\":{\"subpaths\":[{\"closed\":false,\"knots\":[[50,5],[50,28]]},{\"closed\":false,"
                  "\"knots\":[[50,28],[50,55]]}]}}") == PC_OK);
    size_t past = pc_history_past_len(pc_engine_session_active(s)->history), journal = s->njournal;
    CHECK(execute(s, "path.stroke", "{\"tool\":\"blur\",\"size\":10,\"strength\":100}") == PC_OK);
    float p[4];
    pixel(s, 49, 30, p);
    CHECK(p[0] > .02f);
    pixel(s, 50, 30, p);
    CHECK(p[0] < .98f);
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == past + 1);
    CHECK(s->njournal == journal + 1);
    CHECK_EQ_STR(s->journal[journal].id, "path.stroke");
    CHECK(execute(s, "edit.undo", "{}") == PC_OK);
    pixel(s, 49, 30, p);
    CHECK(p[0] < .01f);
    pixel(s, 50, 30, p);
    CHECK(p[0] > .99f);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_fill_and_pencil);
    RUN(test_pressure);
    RUN(test_blur_compound_undo);
    return pc_t_finish();
}
