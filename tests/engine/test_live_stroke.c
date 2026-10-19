#include "harness.h"
#include "engine/live_stroke.h"
#include "compose/compose.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
static void command(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &err);
    CHECK(rc == PC_OK);
    if (rc != PC_OK) {
        PC_T_FAIL("%s: %s", id, err.msg);
    }
    pc_json_value_fini(&out);
}
static pc_engine_session *session(unsigned depth, const char *mode)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char params[160];
    (void)snprintf(params, sizeof params, "{\"width\":64,\"height\":48,\"depth\":%u,\"mode\":\"%s\"}", depth,
                   mode);
    command(s, "file.new", params);
    return s;
}
static void same(const pc_document *a, const pc_document *b)
{
    pc_compose_buffer x = {0}, y = {0};
    pc_rect area = pc_rect_from_size(a->size);
    CHECK(pc_compose_render(a, area, &x, NULL) == PC_OK);
    CHECK(pc_compose_render(b, area, &y, NULL) == PC_OK);
    CHECK(pc_compose_buffer_eq(&x, &y));
    pc_compose_buffer_fini(&x);
    pc_compose_buffer_fini(&y);
}
static void test_live_preview_and_reuse(void)
{
    unsigned depths[] = {8, 16, 32};
    const char *modes[] = {"rgb", "gray", "cmyk", "lab"};
    const char *commands[] = {"paint.stroke", "paint.pencil"};
    for (size_t depth = 0; depth < 3; depth++)
        for (size_t mode = 0; mode < 4; mode++)
            for (size_t cmd = 0; cmd < 2; cmd++)
                for (unsigned sym = 0; sym < 2; sym++) {
                    pc_engine_session *s = session(depths[depth], modes[mode]),
                                      *oracle = session(depths[depth], modes[mode]);
                    if (sym) {
                        command(s, "paint.setSymmetry", "{\"mode\":\"vertical\",\"center\":[32,24]}");
                        command(oracle, "paint.setSymmetry", "{\"mode\":\"vertical\",\"center\":[32,24]}");
                    }
                    const char *initial = "{\"points\":[[10,10,0.7]],\"size\":8,\"opacity\":0.6,\"color\":\"#"
                                          "da253f\",\"seed\":123,\"smoothing\":0.5}";
                    yyjson_doc *params = NULL;
                    CHECK(pc_json_parse(initial, strlen(initial), &params, NULL) == PC_OK);
                    pc_engine_live_stroke *live = NULL;
                    pc_error err;
                    CHECK(pc_engine_live_stroke_begin(s, commands[cmd], yyjson_doc_get_root(params), &live,
                                                      &err) == PC_OK);
                    yyjson_doc_free(params);
                    if (!live) {
                        PC_T_FAIL("live begin: %s", err.msg);
                        pc_engine_session_free(s);
                        pc_engine_session_free(oracle);
                        continue;
                    }
                    uint64_t revision = pc_engine_session_active(s)->revision;
                    CHECK(pc_engine_live_stroke_seed(live) == 123);
                    pc_paint_point point = pc_paint_point_make(20, 20, 0.9f);
                    CHECK(pc_engine_live_stroke_push(live, &point, 1, NULL, NULL) == PC_OK);
                    point = pc_paint_point_make(35, 25, 1);
                    CHECK(pc_engine_live_stroke_push(live, &point, 1, NULL, NULL) == PC_OK);
                    CHECK(pc_engine_session_active(s)->revision == revision);
                    pc_doc_arc *preview = pc_doc_arc_retain(pc_engine_live_stroke_document(live));
                    const char *finished =
                        "{\"points\":[[10,10,0.7],[20,20,0.9],[35,25,1]],\"size\":8,\"opacity\":0.6,"
                        "\"color\":\"#da253f\",\"seed\":123,\"smoothing\":0.5}";
                    command(oracle, commands[cmd], finished);
                    same(pc_doc_arc_get(preview), pc_doc_arc_get(pc_engine_session_active(oracle)->doc));
                    pc_engine_session_offer_live_stroke(s, &live);
                    CHECK(live == NULL);
                    command(s, commands[cmd], finished);
                    CHECK(pc_engine_session_live_stroke_reuses(s) == 1);
                    same(pc_doc_arc_get(preview), pc_doc_arc_get(pc_engine_session_active(s)->doc));
                    CHECK(pc_engine_session_active(s)->revision == revision + 1);
                    command(s, "edit.undo", "{}");
                    CHECK(pc_engine_session_live_stroke_reuses(s) == 1);
                    pc_doc_arc_release(preview);
                    pc_engine_session_free(s);
                    pc_engine_session_free(oracle);
                }
}
static void test_live_rejects_bad_samples_and_mismatched_offer(void)
{
    pc_engine_session *s = session(8, "rgb");
    pc_engine_live_stroke *live = NULL;
    const char *initial = "{\"points\":[[10,10]],\"seed\":17}";
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse(initial, strlen(initial), &params, NULL) == PC_OK);
    CHECK(pc_engine_live_stroke_begin(s, "paint.stroke", yyjson_doc_get_root(params), &live, NULL) == PC_OK);
    yyjson_doc_free(params);
    pc_doc_arc *saved = pc_doc_arc_retain(pc_engine_live_stroke_document(live));
    pc_paint_point point = pc_paint_point_make(NAN, 2, 1);
    CHECK(pc_engine_live_stroke_push(live, &point, 1, NULL, NULL) == PC_ERR_INVALID);
    same(pc_doc_arc_get(saved), pc_doc_arc_get(pc_engine_live_stroke_document(live)));
    point = pc_paint_point_make(2, 2, NAN);
    CHECK(pc_engine_live_stroke_push(live, &point, 1, NULL, NULL) == PC_ERR_INVALID);
    pc_engine_session_offer_live_stroke(s, &live);
    command(s, "paint.stroke", "{\"points\":[[10,10]],\"seed\":18}");
    CHECK(pc_engine_session_live_stroke_reuses(s) == 0);
    pc_doc_arc_release(saved);
    pc_engine_session_free(s);
}
static void target(pc_engine_session *s,unsigned kind)
{
    if(kind==0) { command(s,"channel.target","{\"channel\":\"red\"}"); }
    if(kind==1) { command(s,"channel.new","{\"color\":\"white\"}"); }
    if(kind==2) { command(s,"select.editInQuickMaskMode","{}"); }
}
static void test_live_channel_routing(void)
{
    unsigned depths[]={8,16,32};
    for(size_t depth=0;depth<3;depth++) for(unsigned kind=0;kind<3;kind++) {
        pc_engine_session *s=session(depths[depth],"rgb"),*oracle=session(depths[depth],"rgb");
        target(s,kind);target(oracle,kind);
        const char *initial="{\"points\":[[10,10]],\"seed\":42,\"size\":8,\"smoothing\":0,\"color\":\"#000000\"}";
        yyjson_doc *params=NULL;CHECK(pc_json_parse(initial,strlen(initial),&params,NULL)==PC_OK);
        pc_engine_live_stroke *live=NULL;
        CHECK(pc_engine_live_stroke_begin(s,"paint.stroke",yyjson_doc_get_root(params),&live,NULL)==PC_OK);
        yyjson_doc_free(params);
        if(!live) { pc_engine_session_free(s);pc_engine_session_free(oracle);continue; }
        pc_paint_point p=pc_paint_point_make(20,20,1);
        CHECK(pc_engine_live_stroke_push(live,&p,1,NULL,NULL)==PC_OK);
        const char *finished="{\"points\":[[10,10],[20,20]],\"seed\":42,\"size\":8,\"smoothing\":0,\"color\":\"#000000\"}";
        command(oracle,"paint.stroke",finished);
        const pc_document *shown=pc_doc_arc_get(pc_engine_live_stroke_document(live));
        const pc_document *expected=pc_doc_arc_get(pc_engine_session_active(oracle)->doc);
        if(kind==1) { CHECK(pc_surface_eq(&shown->channels[0].surface,&expected->channels[0].surface)); }
        else if(kind==2) { CHECK(pc_surface_eq(&shown->quick_mask.surface,&expected->quick_mask.surface)); }
        else {
            same(shown,expected);
            float c[4];pc_surface_rgba(pc_layer_surface(&shown->layers[0]),15,15,c);
            CHECK(c[0]<0.1f && c[1]==1 && c[2]==1);
        }
        pc_engine_session_offer_live_stroke(s,&live);command(s,"paint.stroke",finished);
        CHECK(pc_engine_session_live_stroke_reuses(s)==1);
        pc_engine_session_free(s);pc_engine_session_free(oracle);
    }
}

int main(int argc, char **argv)
{
    RUN(test_live_channel_routing);
    RUN(test_live_preview_and_reuse);
    RUN(test_live_rejects_bad_samples_and_mismatched_offer);
    return pc_t_finish();
}
