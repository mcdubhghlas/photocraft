/* Port of crates/doc/src/video.rs: the document timeline and video-layer frame stacks. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"
#include "doc/doc.h"
#include "doc/tree.h"

void pc_video_source_fini(pc_video_source *s)
{
    if (s == NULL) {
        return;
    }
    pc_str_fini(&s->path);
    memset(s, 0, sizeof *s);
}

pc_status pc_video_source_clone(const pc_video_source *s, pc_video_source *out, pc_error *err)
{
    pc_video_source c = *s;
    pc_status st = pc_str_clone(&s->path, &c.path, err);
    if (st != PC_OK) {
        return st;
    }
    *out = c;
    return PC_OK;
}

bool pc_video_source_eq(const pc_video_source *a, const pc_video_source *b)
{
    return a->is_file == b->is_file && (!a->is_file || pc_str_eq(&a->path, &b->path));
}

void pc_video_data_init(pc_video_data *out, pc_surface *frames, size_t nframes, float fps)
{
    memset(out, 0, sizeof *out);
    if (frames != NULL && nframes > 0) {
        out->frames = frames;
        out->nframes = nframes;
    } else {
        free(frames); /* taken over: an empty stack holds NULL */
    }
    out->fps = fps;
    out->show_altered = true;
}

void pc_video_data_fini(pc_video_data *v)
{
    if (v == NULL) {
        return;
    }
    PC_DOC_FINI_VEC(v->frames, v->nframes, pc_surface_fini);
    PC_DOC_FINI_VEC(v->original_frames, v->noriginal_frames, pc_surface_fini);
    pc_video_source_fini(&v->source);
    memset(v, 0, sizeof *v);
}

pc_status pc_video_data_clone(const pc_video_data *v, pc_video_data *out, pc_error *err)
{
    pc_video_data c = *v;
    pc_status s;
    memset(&c.source, 0, sizeof c.source);
    PC_DOC_CLONE_VEC(s, v->frames, v->nframes, c.frames, pc_surface_clone, pc_surface_fini, err);
    if (s != PC_OK) {
        return s;
    }
    PC_DOC_CLONE_VEC(s, v->original_frames, v->noriginal_frames, c.original_frames, pc_surface_clone,
                     pc_surface_fini, err);
    if (s != PC_OK) {
        PC_DOC_FINI_VEC(c.frames, c.nframes, pc_surface_fini);
        return s;
    }
    s = pc_video_source_clone(&v->source, &c.source, err);
    if (s != PC_OK) {
        PC_DOC_FINI_VEC(c.frames, c.nframes, pc_surface_fini);
        PC_DOC_FINI_VEC(c.original_frames, c.noriginal_frames, pc_surface_fini);
        return s;
    }
    *out = c;
    return PC_OK;
}

bool pc_video_data_eq(const pc_video_data *a, const pc_video_data *b)
{
    /* By shape, as the reference: surfaces aren't compared. */
    if (a->has_original_frames != b->has_original_frames ||
        (a->has_original_frames && a->noriginal_frames != b->noriginal_frames)) {
        return false;
    }
    return a->nframes == b->nframes && pc_video_source_eq(&a->source, &b->source) && a->fps == b->fps &&
           a->show_altered == b->show_altered;
}

/* A usable frame rate: positive and finite, else the 30 fps default. */
static float valid_fps(float fps)
{
    return (isfinite(fps) && fps > 0.0f) ? fps : 30.0f;
}

pc_timeline pc_timeline_new(size_t duration, float fps)
{
    pc_timeline t;
    size_t d = duration > 0 ? duration : 1;
    t.fps = valid_fps(fps);
    t.duration = d;
    t.current = 0;
    t.work_start = 0;
    t.work_end = d;
    return t;
}

bool pc_timeline_eq(const pc_timeline *a, const pc_timeline *b)
{
    return a->fps == b->fps && a->duration == b->duration && a->current == b->current &&
           a->work_start == b->work_start && a->work_end == b->work_end;
}

void pc_timeline_clamp(pc_timeline *t)
{
    size_t lo;
    if (t->duration == 0) {
        t->duration = 1;
    }
    t->fps = valid_fps(t->fps);
    if (t->current > t->duration - 1) {
        t->current = t->duration - 1;
    }
    if (t->work_start > t->duration - 1) {
        t->work_start = t->duration - 1;
    }
    /* clamp(work_start + 1, duration): work_start < duration, so lo <= duration. */
    lo = t->work_start + 1;
    if (t->work_end < lo) {
        t->work_end = lo;
    } else if (t->work_end > t->duration) {
        t->work_end = t->duration;
    }
}

void pc_timeline_step(pc_timeline *t, int64_t delta)
{
    /* delta.unsigned_abs() without overflowing on INT64_MIN. */
    uint64_t mag = delta < 0 ? (uint64_t)(-(delta + 1)) + 1u : (uint64_t)delta;
    size_t n = mag > SIZE_MAX ? SIZE_MAX : (size_t)mag;
    size_t last = t->duration > 0 ? t->duration - 1 : 0;
    size_t moved;
    if (delta < 0) {
        moved = t->current > n ? t->current - n : 0;
    } else {
        moved = t->current > SIZE_MAX - n ? SIZE_MAX : t->current + n;
    }
    t->current = moved < last ? moved : last;
}

float pc_timeline_time(const pc_timeline *t)
{
    return (float)t->current / pc_rs_maxf_k(t->fps, 1e-3f);
}
