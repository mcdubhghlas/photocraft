/* Layer > Video Layers (and Rasterize > Video). A video layer holds a stack of frames; the
 * displayed raster content is the frame at the timeline playhead, kept in sync by sync() after any
 * edit or navigation, and written back by store() before re-syncing. Port of
 * crates/engine/src/video_cmds.rs (E12).
 *
 * DEFERRED (owner: io/codecs): the file-footage loaders (newVideoLayerFromFile, replaceFootage,
 * reloadFrame, file.import.videoFramesToLayers) need pc_io import_gif_frames +
 * tga_alpha_channel_to_transparency, which the io chunk did not export; file.export.renderVideo
 * needs the same gif/quantize export pipeline. They are not registered here (REWRITE_PLAN.md §7). */
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "doc/doc.h"
#include "doc/video.h"
#include "engine_internal.h"

/* ---- store / sync (exported for timeline_cmds) ------------------------------------------- */

static size_t timeline_current(const pc_document *doc)
{
    return doc->has_timeline ? doc->timeline.current : 0;
}

static void sync_layers(pc_layer *layers, size_t n, size_t cur)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        if (l->has_video && l->content.kind == PC_LAYER_RASTER) {
            const pc_video_data *v = &l->video;
            const pc_surface *vis = v->frames;
            size_t nvis = v->nframes;
            if (!v->show_altered && v->has_original_frames) {
                vis = v->original_frames;
                nvis = v->noriginal_frames;
            }
            if (nvis > 0) {
                size_t at = cur < nvis ? cur : nvis - 1;
                pc_surface copy;
                if (pc_surface_clone(&vis[at], &copy, NULL) == PC_OK) {
                    pc_surface_fini(&l->content.u.raster);
                    l->content.u.raster = copy;
                }
            }
        }
        if (l->content.kind == PC_LAYER_GROUP) {
            sync_layers(l->content.u.group.children, l->content.u.group.nchildren, cur);
        }
    }
}

void pc_engine_video_sync(pc_document *doc)
{
    sync_layers(doc->layers, doc->nlayers, timeline_current(doc));
}

static void store_layers(pc_layer *layers, size_t n, size_t cur)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        if (l->has_video && l->content.kind == PC_LAYER_RASTER) {
            pc_video_data *v = &l->video;
            if (v->nframes > 0 && (v->show_altered || !v->has_original_frames)) {
                size_t at = cur < v->nframes ? cur : v->nframes - 1;
                pc_surface copy;
                if (pc_surface_clone(&l->content.u.raster, &copy, NULL) == PC_OK) {
                    pc_surface_fini(&v->frames[at]);
                    v->frames[at] = copy;
                }
            }
        }
        if (l->content.kind == PC_LAYER_GROUP) {
            store_layers(l->content.u.group.children, l->content.u.group.nchildren, cur);
        }
    }
}

void pc_engine_video_store(pc_document *doc)
{
    store_layers(doc->layers, doc->nlayers, timeline_current(doc));
}

/* ---- enabled predicates ------------------------------------------------------------------ */

static pc_status video_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    return PC_OK;
}

static pc_status active_video(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    if (d->has_active_layer) {
        const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
        if (l != NULL && l->has_video) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "select a video layer");
}

/* ---- blank-surface vector helpers -------------------------------------------------------- */

static pc_status surfaces_insert(pc_surface **arr, size_t *n, size_t at, pc_surface s, pc_error *err)
{
    size_t need = *n + 1;
    if (need == 0 || need > SIZE_MAX / sizeof **arr) {
        pc_surface_fini(&s);
        return pc_fail(err, PC_ERR_LIMIT, "video");
    }
    pc_surface *grown = realloc(*arr, need * sizeof **arr);
    if (grown == NULL) {
        pc_surface_fini(&s);
        return pc_fail(err, PC_ERR_NOMEM, "video");
    }
    *arr = grown;
    if (at > *n) {
        at = *n;
    }
    memmove(&grown[at + 1], &grown[at], (*n - at) * sizeof *grown);
    grown[at] = s;
    *n += 1;
    return PC_OK;
}

static void surfaces_remove(pc_surface *arr, size_t *n, size_t at)
{
    if (at >= *n) {
        return;
    }
    pc_surface_fini(&arr[at]);
    memmove(&arr[at], &arr[at + 1], (*n - at - 1) * sizeof *arr);
    *n -= 1;
}

/* ---- grow timeline (grow_duration) -------------------------------------------------------- */

static void grow_duration(pc_timeline *t, size_t frames)
{
    size_t old = t->duration;
    size_t want = frames > 1 ? frames : 1;
    t->duration = t->duration > want ? t->duration : want;
    if (t->work_end >= old) {
        t->work_end = t->duration;
    }
    pc_timeline_clamp(t);
}

/* ---- edit_video wrapper ------------------------------------------------------------------- */

typedef pc_status (*video_mut_fn)(pc_video_data *v, size_t cur, void *ctx, pc_error *err);

typedef struct {
    const char *label;
    video_mut_fn mut;
    void *ctx;
    size_t frames_out;
} video_edit_ctx;

static pc_status edit_video_fn(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                               pc_error *err)
{
    video_edit_ctx *ec = vctx;
    pc_engine_video_store(doc);
    size_t cur = timeline_current(doc);
    if (!*has_active) {
        return pc_engine_err_no_document(err);
    }
    pc_layer *l = pc_document_layer_mut(doc, *active);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, *active);
    }
    if (!l->has_video) {
        return pc_engine_err_bad_params(err, ec->label, "the active layer is not a video layer");
    }
    pc_status rc = ec->mut(&l->video, cur, ec->ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t nfr = l->video.nframes;
    if (doc->has_timeline) {
        grow_duration(&doc->timeline, nfr);
    }
    pc_engine_video_sync(doc);
    ec->frames_out = nfr;
    return PC_OK;
}

static pc_status edit_video(pc_engine_session *s, const char *label, video_mut_fn mut, void *ctx,
                            pc_json_value *out, pc_error *err)
{
    video_edit_ctx ec = {.label = label, .mut = mut, .ctx = ctx, .frames_out = 0};
    pc_status rc = pc_engine_session_edit(s, label, edit_video_fn, &ec, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "frames", ec.frames_out);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- per-command mutators ---------------------------------------------------------------- */

static pc_status mut_insert_blank(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    pc_pixel_format fmt = *(const pc_pixel_format *)ctx;
    size_t at = cur + 1 < v->nframes ? cur + 1 : v->nframes;
    pc_surface blank;
    pc_surface_init(&blank, fmt);
    pc_status rc = surfaces_insert(&v->frames, &v->nframes, at, blank, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (v->has_original_frames) {
        pc_surface ob;
        pc_surface_init(&ob, fmt);
        size_t oat = at < v->noriginal_frames ? at : v->noriginal_frames;
        rc = surfaces_insert(&v->original_frames, &v->noriginal_frames, oat, ob, err);
    }
    return rc;
}

static pc_status mut_duplicate(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    (void)ctx;
    if (cur >= v->nframes) {
        return PC_OK;
    }
    pc_surface dup;
    if (pc_surface_clone(&v->frames[cur], &dup, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (v->has_original_frames) {
        pc_surface orig;
        if (cur < v->noriginal_frames) {
            if (pc_surface_clone(&v->original_frames[cur], &orig, err) != PC_OK) {
                pc_surface_fini(&dup);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        } else {
            pc_surface_init(&orig, pc_surface_format(&dup));
        }
        size_t oat = cur + 1 < v->noriginal_frames ? cur + 1 : v->noriginal_frames;
        if (surfaces_insert(&v->original_frames, &v->noriginal_frames, oat, orig, err) != PC_OK) {
            pc_surface_fini(&dup);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    return surfaces_insert(&v->frames, &v->nframes, cur + 1, dup, err);
}

static pc_status mut_delete(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    (void)ctx;
    (void)err;
    if (v->nframes > 1 && cur < v->nframes) {
        surfaces_remove(v->frames, &v->nframes, cur);
        if (v->has_original_frames && cur < v->noriginal_frames) {
            surfaces_remove(v->original_frames, &v->noriginal_frames, cur);
        }
    }
    return PC_OK;
}

static pc_status mut_restore(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    pc_pixel_format fmt = *(const pc_pixel_format *)ctx;
    if (cur >= v->nframes) {
        return PC_OK;
    }
    pc_surface rep;
    if (v->has_original_frames && cur < v->noriginal_frames) {
        if (pc_surface_clone(&v->original_frames[cur], &rep, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    } else {
        pc_surface_init(&rep, fmt);
    }
    pc_surface_fini(&v->frames[cur]);
    v->frames[cur] = rep;
    return PC_OK;
}

static pc_status mut_restore_all(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    (void)cur;
    pc_pixel_format fmt = *(const pc_pixel_format *)ctx;
    for (size_t i = 0; i < v->nframes; i++) {
        pc_surface rep;
        if (v->has_original_frames && i < v->noriginal_frames) {
            if (pc_surface_clone(&v->original_frames[i], &rep, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        } else {
            pc_surface_init(&rep, fmt);
        }
        pc_surface_fini(&v->frames[i]);
        v->frames[i] = rep;
    }
    return PC_OK;
}

static pc_status mut_show_altered(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    (void)cur;
    (void)ctx;
    (void)err;
    v->show_altered = !v->show_altered;
    return PC_OK;
}

static pc_status mut_interpret(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    (void)cur;
    (void)err;
    const double *fps = ctx;
    if (fps != NULL) {
        v->fps = (float)*fps;
    }
    return PC_OK;
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_pixel_format active_format(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return pc_document_pixel_format(pc_doc_arc_get(d->doc));
}

static pc_status run_insert_blank(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_pixel_format fmt = active_format(s);
    return edit_video(s, "Insert Blank Frame", mut_insert_blank, &fmt, out, err);
}
static pc_status run_duplicate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return edit_video(s, "Duplicate Frame", mut_duplicate, NULL, out, err);
}
static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return edit_video(s, "Delete Frame", mut_delete, NULL, out, err);
}
static pc_status run_restore(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_pixel_format fmt = active_format(s);
    return edit_video(s, "Restore Frame", mut_restore, &fmt, out, err);
}
static pc_status run_restore_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_pixel_format fmt = active_format(s);
    return edit_video(s, "Restore All Frames", mut_restore_all, &fmt, out, err);
}
static pc_status run_show_altered(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return edit_video(s, "Show Altered Video", mut_show_altered, NULL, out, err);
}
static pc_status run_interpret(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double fps;
    yyjson_val *fv = pc_json_get(p, "fps");
    bool have = fv != NULL && pc_json_as_f64(fv, &fps);
    return edit_video(s, "Interpret Footage", mut_interpret, have ? &fps : NULL, out, err);
}

/* New Blank Video Layer. */
static pc_status edit_new_blank(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                pc_error *err)
{
    pc_layer_id *out_id = ctx;
    pc_engine_video_store(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    if (!doc->has_timeline) {
        doc->timeline = pc_timeline_new(1, 30.0f);
        doc->has_timeline = true;
    }
    float fps = doc->timeline.fps;
    size_t n = doc->timeline.duration > 1 ? doc->timeline.duration : 1;
    pc_surface *frames = pc_malloc_array(n, sizeof *frames);
    if (frames == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "video");
    }
    for (size_t i = 0; i < n; i++) {
        pc_surface_init(&frames[i], fmt);
    }
    pc_str name = PC_STR_INIT;
    if (pc_document_next_layer_name(doc, "Video Layer", &name, err) != PC_OK) {
        for (size_t i = 0; i < n; i++) {
            pc_surface_fini(&frames[i]);
        }
        free(frames);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, pc_str_c(&name), fmt, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        for (size_t i = 0; i < n; i++) {
            pc_surface_fini(&frames[i]);
        }
        free(frames);
        return rc;
    }
    pc_video_data_init(&l.video, frames, n, fps); /* takes over frames */
    l.has_video = true;
    const pc_layer_id *above = *has_active ? active : NULL;
    pc_layer_id id;
    rc = pc_document_insert_above(doc, above, &l, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *active = id;
    *has_active = true;
    *out_id = id;
    pc_engine_video_sync(doc);
    return PC_OK;
}

static pc_status run_new_blank(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_layer_id id = 0;
    pc_status rc = pc_engine_session_edit(s, "New Blank Video Layer", edit_new_blank, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Rasterize: drop the frame stack, keep the current frame as a normal pixel layer. */
static pc_status edit_rasterize(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                pc_error *err)
{
    (void)ctx;
    if (!*has_active) {
        return pc_engine_err_no_document(err);
    }
    pc_layer *l = pc_document_layer_mut(doc, *active);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, *active);
    }
    if (l->has_video) {
        pc_video_data_fini(&l->video);
        l->has_video = false;
    }
    return PC_OK;
}

static pc_status run_rasterize(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_status rc = pc_engine_session_edit(s, "Rasterize Video", edit_rasterize, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "rasterized", true);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_VL[] = {"Layer", "Video Layers"};
static const char *const MENU_RAST[] = {"Layer", "Rasterize"};

static const pc_engine_command SPECS[] = {
    {"layer.videoLayers.newBlankVideoLayer", "New Blank Video Layer", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}",
     video_has_doc, run_new_blank, true},
    {"layer.videoLayers.insertBlankFrame", "Insert Blank Frame", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}",
     active_video, run_insert_blank, true},
    {"layer.videoLayers.duplicateFrame", "Duplicate Frame", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}",
     active_video, run_duplicate, true},
    {"layer.videoLayers.deleteFrame", "Delete Frame", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}", active_video,
     run_delete, true},
    {"layer.videoLayers.restoreFrame", "Restore Frame", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}",
     active_video, run_restore, true},
    {"layer.videoLayers.restoreAllFrames", "Restore All Frames", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}",
     active_video, run_restore_all, true},
    {"layer.videoLayers.showAlteredVideo", "Show Altered Video", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}",
     active_video, run_show_altered, true},
    {"layer.videoLayers.rasterize", "Rasterize", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}", active_video,
     run_rasterize, true},
    {"layer.rasterize.video", "Video", MENU_RAST, 2, NULL, "{} \xE2\x86\x92 {}", active_video,
     run_rasterize, true},
    {"layer.videoLayers.interpretFootage", "Interpret Footage\xE2\x80\xA6", MENU_VL, 2, NULL,
     "{} \xE2\x86\x92 {}", active_video, run_interpret, true},
};

const pc_engine_command *pc_engine_video_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
