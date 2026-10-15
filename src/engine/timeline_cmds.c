/* Window > Timeline: create and drive the document timeline (playhead over N frames at a frame
 * rate). Navigation doesn't add history steps. Port of crates/engine/src/timeline_cmds.rs (E12). */
#include <math.h>
#include <string.h>

#include "doc/doc.h"
#include "doc/video.h"
#include "engine_internal.h"

#define MAX_DURATION ((size_t)1000000)
#define MAX_FPS 1000.0f

static pc_status tl_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    return PC_OK;
}

static pc_status tl_has_timeline(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    if (!pc_doc_arc_get(d->doc)->has_timeline) {
        return pc_fail(reason, PC_ERR_INVALID, "no timeline (create one first)");
    }
    return PC_OK;
}

/* {timeline:{...}} or {timeline:null} into *out. */
static pc_status info(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    if (doc->has_timeline) {
        const pc_timeline *t = &doc->timeline;
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_real(rd, o, "fps", (double)t->fps);
        yyjson_mut_obj_add_uint(rd, o, "duration", t->duration);
        yyjson_mut_obj_add_uint(rd, o, "current", t->current);
        yyjson_mut_obj_add_uint(rd, o, "workStart", t->work_start);
        yyjson_mut_obj_add_uint(rd, o, "workEnd", t->work_end);
        yyjson_mut_obj_add_real(rd, o, "time", (double)pc_timeline_time(t));
        yyjson_mut_obj_add_val(rd, root, "timeline", o);
    } else {
        yyjson_mut_obj_add_null(rd, root, "timeline");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Mutate the document timeline without a history step. mut is applied to the (mutable) document. */
typedef void (*tl_mut_fn)(pc_document *doc, void *ctx);

static pc_status with_timeline(pc_engine_session *s, tl_mut_fn mut, void *ctx, pc_json_value *out,
                               pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_document *doc = NULL;
    if (pc_doc_arc_make_mut(&st->doc, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_engine_video_store(doc);
    mut(doc, ctx);
    if (doc->has_timeline) {
        pc_timeline_clamp(&doc->timeline);
    }
    pc_engine_video_sync(doc);
    st->revision += 1;
    return info(s, out, err);
}

/* ---- param parsing ------------------------------------------------------------------------ */

/* duration param: present? *have, value in *out (whole number 1..=MAX_DURATION); error otherwise. */
static pc_status duration_param(const char *cmd, yyjson_val *p, bool *have, size_t *out, pc_error *err)
{
    *have = false;
    yyjson_val *v = pc_json_get(p, "duration");
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    uint64_t d;
    if (pc_json_as_u64(v, &d) && d >= 1 && d <= (uint64_t)MAX_DURATION) {
        *have = true;
        *out = (size_t)d;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd,
                                    "`duration` must be a whole number of frames from 1 to %zu",
                                    MAX_DURATION);
}

static pc_status fps_param(const char *cmd, yyjson_val *p, bool *have, float *out, pc_error *err)
{
    *have = false;
    yyjson_val *v = pc_json_get(p, "fps");
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    double f;
    if (pc_json_as_f64(v, &f) && isfinite(f) && f > 0.0 && f <= (double)MAX_FPS) {
        *have = true;
        *out = (float)f;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "`fps` must be a number above 0 and at most %g",
                                    (double)MAX_FPS);
}

/* ---- commands ----------------------------------------------------------------------------- */

typedef struct {
    size_t duration;
    float fps;
} create_ctx;

static void mut_create(pc_document *doc, void *ctx)
{
    create_ctx *c = ctx;
    doc->timeline = pc_timeline_new(c->duration, c->fps);
    doc->has_timeline = true;
}

static pc_status run_create(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    create_ctx c = {.duration = 30, .fps = 30.0f};
    bool have;
    size_t d;
    float f;
    pc_status prc = duration_param("timeline.create", p, &have, &d, err);
    if (prc != PC_OK) {
        return prc;
    }
    if (have) {
        c.duration = d;
    }
    prc = fps_param("timeline.create", p, &have, &f, err);
    if (prc != PC_OK) {
        return prc;
    }
    if (have) {
        c.fps = f;
    }
    return with_timeline(s, mut_create, &c, out, err);
}

static void mut_delete(pc_document *doc, void *ctx)
{
    (void)ctx;
    doc->has_timeline = false;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return with_timeline(s, mut_delete, NULL, out, err);
}

static void mut_set_frame(pc_document *doc, void *ctx)
{
    if (doc->has_timeline) {
        doc->timeline.current = *(const size_t *)ctx;
    }
}

static pc_status run_set_frame(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    uint64_t f;
    yyjson_val *fv = pc_json_get(p, "frame");
    if (fv == NULL || !pc_json_as_u64(fv, &f)) {
        return pc_engine_err_bad_params(err, "timeline.setFrame", "need `frame`");
    }
    size_t frame = f > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)f;
    return with_timeline(s, mut_set_frame, &frame, out, err);
}

static void mut_step(pc_document *doc, void *ctx)
{
    if (doc->has_timeline) {
        pc_timeline_step(&doc->timeline, *(const int64_t *)ctx);
    }
}

static pc_status run_next(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    int64_t d = 1;
    return with_timeline(s, mut_step, &d, out, err);
}

static pc_status run_prev(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    int64_t d = -1;
    return with_timeline(s, mut_step, &d, out, err);
}

typedef struct {
    bool have_fps;
    float fps;
    bool have_dur;
    size_t dur;
    bool have_ws;
    size_t ws;
    bool have_we;
    size_t we;
} props_ctx;

static void mut_set_props(pc_document *doc, void *ctx)
{
    props_ctx *c = ctx;
    if (!doc->has_timeline) {
        return;
    }
    pc_timeline *t = &doc->timeline;
    if (c->have_fps) {
        t->fps = c->fps;
    }
    if (c->have_dur) {
        t->duration = c->dur;
    }
    if (c->have_ws) {
        t->work_start = c->ws;
    }
    if (c->have_we) {
        t->work_end = c->we;
    }
}

static pc_status run_set_props(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    props_ctx c;
    memset(&c, 0, sizeof c);
    pc_status prc = fps_param("timeline.setProps", p, &c.have_fps, &c.fps, err);
    if (prc != PC_OK) {
        return prc;
    }
    prc = duration_param("timeline.setProps", p, &c.have_dur, &c.dur, err);
    if (prc != PC_OK) {
        return prc;
    }
    uint64_t v;
    yyjson_val *ws = pc_json_get(p, "workStart");
    if (ws != NULL && pc_json_as_u64(ws, &v)) {
        c.have_ws = true;
        c.ws = v > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)v;
    }
    yyjson_val *we = pc_json_get(p, "workEnd");
    if (we != NULL && pc_json_as_u64(we, &v)) {
        c.have_we = true;
        c.we = v > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)v;
    }
    return with_timeline(s, mut_set_props, &c, out, err);
}

static pc_status run_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return info(s, out, err);
}

static const pc_engine_command SPECS[] = {
    {"timeline.create", "Create Video Timeline", NULL, 0, NULL,
     "{duration?:30, fps?:30} \xE2\x86\x92 {timeline}", tl_has_doc, run_create, false},
    {"timeline.delete", "Delete Timeline", NULL, 0, NULL, "{} \xE2\x86\x92 {timeline:null}",
     tl_has_timeline, run_delete, false},
    {"timeline.setFrame", "Go to Frame", NULL, 0, NULL, "{frame} \xE2\x86\x92 {timeline}",
     tl_has_timeline, run_set_frame, false},
    {"timeline.nextFrame", "Next Frame", NULL, 0, NULL, "{} \xE2\x86\x92 {timeline}", tl_has_timeline,
     run_next, false},
    {"timeline.previousFrame", "Previous Frame", NULL, 0, NULL, "{} \xE2\x86\x92 {timeline}",
     tl_has_timeline, run_prev, false},
    {"timeline.setProps", "Timeline Settings", NULL, 0, NULL,
     "{fps?, duration?, workStart?, workEnd?} \xE2\x86\x92 {timeline}", tl_has_timeline, run_set_props,
     false},
    {"timeline.info", "Timeline Info", NULL, 0, NULL, "{} \xE2\x86\x92 {timeline}", tl_has_doc, run_info,
     false},
};

const pc_engine_command *pc_engine_timeline_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
