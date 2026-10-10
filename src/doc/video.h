/* Window > Timeline and video-layer frame stacks (port of crates/doc/src/video.rs).
 * Conventions: doc/adjust.h. */
#ifndef PC_DOC_VIDEO_H
#define PC_DOC_VIDEO_H

#include "doc/adjust.h"
#include "raster/surface.h"

/* Blank (is_file false) or loaded from `path`. */
typedef struct pc_video_source {
    bool is_file;
    pc_str path;
} pc_video_source;

void pc_video_source_fini(pc_video_source *s);
pc_status pc_video_source_clone(const pc_video_source *s, pc_video_source *out, pc_error *err);
bool pc_video_source_eq(const pc_video_source *a, const pc_video_source *b);

typedef struct pc_video_data {
    pc_surface *frames;
    size_t nframes;
    pc_video_source source;
    float fps;
    bool show_altered;
} pc_video_data;

/* Takes over the frames array (moved; never fails); source Blank, show_altered on. */
void pc_video_data_init(pc_video_data *out, pc_surface *frames, size_t nframes, float fps);
void pc_video_data_fini(pc_video_data *v);
pc_status pc_video_data_clone(const pc_video_data *v, pc_video_data *out, pc_error *err);
/* By shape, not pixels: frame count, source, fps, show_altered (the reference's PartialEq). */
bool pc_video_data_eq(const pc_video_data *a, const pc_video_data *b);

/* The document playhead. */
typedef struct pc_timeline {
    float fps;
    size_t duration; /* frames, >= 1 after clamp */
    size_t current;
    size_t work_start; /* inclusive */
    size_t work_end;   /* exclusive */
} pc_timeline;

pc_timeline pc_timeline_new(size_t duration, float fps);
bool pc_timeline_eq(const pc_timeline *a, const pc_timeline *b);
void pc_timeline_clamp(pc_timeline *t);
void pc_timeline_step(pc_timeline *t, int64_t delta); /* saturating, never leaves the timeline */
float pc_timeline_time(const pc_timeline *t);

#endif
