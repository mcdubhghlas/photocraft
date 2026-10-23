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

#include "algo/quantize.h"
#include "base/buf.h"
#include "base/json.h"
#include "base/num.h"
#include "base/str.h"
#include "codecs/codecs.h"
#include "codecs/format.h"
#include "codecs/web.h"
#include "doc/doc.h"
#include "doc/video.h"
#include "io/core.h"
#include "io/flat.h"
#include "io/io.h"
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

/* ---- file footage loaders (new_from_file / replaceFootage / reloadFrame / framesToLayers) ---
 *
 * A single image is one frame; a GIF is its animation; a clean-room pipeline, no proprietary codec.
 * (A directory image-sequence, which the reference also accepts, is not supported here; such a path
 * reads as an I/O error. See REWRITE_PLAN.md §7.) */

/* The most common GIF delay as a frame rate, or 0 when there are none (video_cmds::fps_from_delays). */
static float fps_from_delays(const uint32_t *delays, size_t n)
{
    uint32_t keys[256];
    size_t counts[256];
    size_t nk = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t d = delays[i] < 20 ? 100 : delays[i];
        size_t j = 0;
        for (; j < nk; j++) {
            if (keys[j] == d) {
                counts[j]++;
                break;
            }
        }
        if (j == nk && nk < 256) {
            keys[nk] = d;
            counts[nk] = 1;
            nk++;
        }
    }
    uint32_t best = 0;
    size_t bn = 0;
    for (size_t j = 0; j < nk; j++) {
        if (counts[j] > bn) {
            bn = counts[j];
            best = keys[j];
        }
    }
    return (best > 0) ? 1000.0f / (float)best : 0.0f;
}

/* Load the frames of the file at `path` (GIF animation, or a single image) as surfaces in `fmt`.
 * frames/nframes owned on success (free each surface + the array); fps is 0 when unknown. */
static pc_status load_frames(const char *path, pc_pixel_format fmt, pc_surface **frames_out,
                             size_t *nframes_out, float *fps_out, pc_error *err)
{
    *frames_out = NULL;
    *nframes_out = 0;
    *fps_out = 0.0f;
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_read_file(path, &bytes, err);
    if (rc != PC_OK) {
        pc_buf_fini(&bytes);
        return rc;
    }
    pc_surface *frames = NULL;
    size_t n = 0, cap = 0;
    pc_cformat f;
    bool is_gif = pc_cformat_detect(bytes.data, bytes.len, &f) && f == PC_CF_GIF;
    if (is_gif) {
        pc_cdecode_opts opts = pc_cdecode_opts_default();
        pc_canim_frame *gframes = NULL;
        size_t ng = 0;
        rc = pc_cdecode_gif_frames(bytes.data, bytes.len, &opts, &gframes, &ng, err);
        if (rc != PC_OK) {
            pc_buf_fini(&bytes);
            return rc;
        }
        uint32_t *delays = ng > 0 ? malloc(ng * sizeof *delays) : NULL;
        for (size_t i = 0; i < ng; i++) {
            pc_io_import_result res;
            memset(&res, 0, sizeof res);
            if (pc_io_image_to_document(path, &gframes[i].image, &res, err) != PC_OK) {
                rc = err != NULL ? err->code : PC_ERR_FORMAT;
                free(delays);
                goto fail;
            }
            pc_surface surf;
            rc = pc_engine_flattened(&res.document, fmt, &surf, err);
            pc_io_import_result_fini(&res);
            if (rc != PC_OK) {
                free(delays);
                goto fail;
            }
            if (n == cap) {
                size_t nc = cap == 0 ? 8 : cap * 2;
                pc_surface *nv = realloc(frames, nc * sizeof *nv);
                if (nv == NULL) {
                    pc_surface_fini(&surf);
                    free(delays);
                    rc = pc_fail(err, PC_ERR_NOMEM, "video frames");
                    goto fail;
                }
                frames = nv;
                cap = nc;
            }
            frames[n++] = surf;
            if (delays != NULL) {
                delays[i] = gframes[i].delay_ms;
            }
        }
        pc_canim_frames_free(gframes, ng);
        *fps_out = delays != NULL ? fps_from_delays(delays, ng) : 0.0f;
        free(delays);
    } else {
        pc_document doc;
        rc = pc_engine_import(path, bytes.data, bytes.len, &doc, err);
        if (rc != PC_OK) {
            pc_buf_fini(&bytes);
            return rc;
        }
        pc_surface surf;
        rc = pc_engine_flattened(&doc, fmt, &surf, err);
        pc_document_fini(&doc);
        if (rc != PC_OK) {
            pc_buf_fini(&bytes);
            return rc;
        }
        frames = malloc(sizeof *frames);
        if (frames == NULL) {
            pc_surface_fini(&surf);
            pc_buf_fini(&bytes);
            return pc_fail(err, PC_ERR_NOMEM, "video frame");
        }
        frames[0] = surf;
        n = 1;
    }
    pc_buf_fini(&bytes);
    if (n == 0) {
        free(frames);
        return pc_engine_err_other(err, "no frames in `%s`", path);
    }
    *frames_out = frames;
    *nframes_out = n;
    return PC_OK;
fail:
    for (size_t i = 0; i < n; i++) {
        pc_surface_fini(&frames[i]);
    }
    free(frames);
    pc_buf_fini(&bytes);
    return rc;
}

static const char *path_param(yyjson_val *p, const char *cmd, char *buf, size_t cap, pc_error *err)
{
    size_t pl = 0;
    const char *path = p != NULL ? pc_json_as_str(pc_json_get(p, "path"), &pl) : NULL;
    if (path == NULL || pl == 0 || pl >= cap) {
        (void)pc_engine_err_bad_params(err, cmd, "need `path`");
        return NULL;
    }
    memcpy(buf, path, pl);
    buf[pl] = '\0';
    return buf;
}

struct new_from_file_ctx {
    pc_surface *frames;
    size_t nframes;
    float fps;
    const char *path;
    pc_layer_id out_id;
};

static pc_status edit_new_from_file(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                    pc_error *err)
{
    struct new_from_file_ctx *c = vctx;
    pc_engine_video_store(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    if (!doc->has_timeline) {
        doc->timeline = pc_timeline_new(c->nframes, c->fps);
        doc->has_timeline = true;
    } else {
        grow_duration(&doc->timeline, c->nframes);
    }
    char stem[256];
    pc_engine_file_stem(c->path, stem, sizeof stem);
    pc_str name = PC_STR_INIT;
    if (pc_document_next_layer_name(doc, stem[0] != '\0' ? stem : "Video", &name, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, pc_str_c(&name), fmt, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        return rc;
    }
    /* Clone the frames for original_frames before pc_video_data_init takes the array over. */
    pc_surface *orig = pc_malloc_array(c->nframes, sizeof *orig);
    if (orig == NULL) {
        pc_layer_fini(&l);
        return pc_fail(err, PC_ERR_NOMEM, "video");
    }
    for (size_t i = 0; i < c->nframes; i++) {
        if (pc_surface_clone(&c->frames[i], &orig[i], err) != PC_OK) {
            for (size_t j = 0; j < i; j++) {
                pc_surface_fini(&orig[j]);
            }
            free(orig);
            pc_layer_fini(&l);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    pc_video_data_init(&l.video, c->frames, c->nframes, c->fps); /* takes over frames */
    c->frames = NULL;
    l.has_video = true;
    l.video.has_original_frames = true;
    l.video.original_frames = orig;
    l.video.noriginal_frames = c->nframes;
    l.video.source.is_file = true;
    (void)pc_str_set_c(&l.video.source.path, c->path, NULL);
    const pc_layer_id *above = *has_active ? active : NULL;
    pc_layer_id id;
    rc = pc_document_insert_above(doc, above, &l, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *active = id;
    *has_active = true;
    c->out_id = id;
    pc_engine_video_sync(doc);
    return PC_OK;
}

static pc_status run_new_from_file(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    char path[4096];
    if (path_param(p, "layer.videoLayers.newVideoLayerFromFile", path, sizeof path, err) == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_pixel_format fmt = pc_document_pixel_format(pc_doc_arc_get(d->doc));
    struct new_from_file_ctx c;
    memset(&c, 0, sizeof c);
    c.path = path;
    pc_status rc = load_frames(path, fmt, &c.frames, &c.nframes, &c.fps, err);
    if (rc != PC_OK) {
        return rc;
    }
    double fps_override = 0.0;
    yyjson_val *jf = p != NULL ? pc_json_get(p, "fps") : NULL;
    if (jf != NULL && pc_json_as_f64(jf, &fps_override) && fps_override > 0.0) {
        c.fps = (float)fps_override;
    } else if (c.fps <= 0.0f) {
        c.fps = 30.0f;
    }
    size_t nframes = c.nframes;
    rc = pc_engine_session_edit(s, "New Video Layer from File", edit_new_from_file, &c, err);
    if (c.frames != NULL) {
        for (size_t i = 0; i < c.nframes; i++) {
            pc_surface_fini(&c.frames[i]);
        }
        free(c.frames);
    }
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", c.out_id);
    yyjson_mut_obj_add_uint(rd, root, "frames", nframes);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* replaceFootage / reloadFrame: swap a video layer's frames (and original_frames) for `frames`. */
struct swap_ctx {
    pc_surface *frames;
    size_t nframes;
    bool set_source;
    const char *path;
};

static pc_status mut_swap(pc_video_data *v, size_t cur, void *ctx, pc_error *err)
{
    (void)cur;
    struct swap_ctx *c = ctx;
    pc_surface *fr = pc_malloc_array(c->nframes, sizeof *fr);
    pc_surface *orig = pc_malloc_array(c->nframes, sizeof *orig);
    if ((c->nframes > 0 && (fr == NULL || orig == NULL))) {
        free(fr);
        free(orig);
        return pc_fail(err, PC_ERR_NOMEM, "video footage");
    }
    for (size_t i = 0; i < c->nframes; i++) {
        if (pc_surface_clone(&c->frames[i], &fr[i], err) != PC_OK) {
            for (size_t j = 0; j < i; j++) {
                pc_surface_fini(&fr[j]);
                pc_surface_fini(&orig[j]);
            }
            free(fr);
            free(orig);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (pc_surface_clone(&c->frames[i], &orig[i], err) != PC_OK) {
            for (size_t j = 0; j < i; j++) {
                pc_surface_fini(&fr[j]);
                pc_surface_fini(&orig[j]);
            }
            pc_surface_fini(&fr[i]);
            free(fr);
            free(orig);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    for (size_t i = 0; i < v->nframes; i++) {
        pc_surface_fini(&v->frames[i]);
    }
    free(v->frames);
    v->frames = fr;
    v->nframes = c->nframes;
    if (v->has_original_frames) {
        for (size_t i = 0; i < v->noriginal_frames; i++) {
            pc_surface_fini(&v->original_frames[i]);
        }
        free(v->original_frames);
    }
    v->has_original_frames = true;
    v->original_frames = orig;
    v->noriginal_frames = c->nframes;
    if (c->set_source) {
        pc_video_source_fini(&v->source);
        v->source.is_file = true;
        v->source.path = (pc_str)PC_STR_INIT;
        (void)pc_str_set_c(&v->source.path, c->path, NULL);
    }
    return PC_OK;
}

static pc_status run_replace_footage(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    char path[4096];
    if (path_param(p, "layer.videoLayers.replaceFootage", path, sizeof path, err) == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_pixel_format fmt = pc_document_pixel_format(pc_doc_arc_get(d->doc));
    struct swap_ctx c;
    memset(&c, 0, sizeof c);
    c.set_source = true;
    c.path = path;
    float fps = 0.0f;
    pc_status rc = load_frames(path, fmt, &c.frames, &c.nframes, &fps, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = edit_video(s, "Replace Footage", mut_swap, &c, out, err);
    for (size_t i = 0; i < c.nframes; i++) {
        pc_surface_fini(&c.frames[i]);
    }
    free(c.frames);
    return rc;
}

static pc_status run_reload_frame(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!d->has_active_layer) {
        return pc_engine_err_bad_params(err, "layer.videoLayers.reloadFrame", "not a video layer");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL || !l->has_video) {
        return pc_engine_err_bad_params(err, "layer.videoLayers.reloadFrame", "not a video layer");
    }
    if (!l->video.source.is_file) {
        return pc_engine_err_bad_params(err, "layer.videoLayers.reloadFrame",
                                        "this layer has no file source to reload");
    }
    char path[4096];
    const char *sp = pc_str_c(&l->video.source.path);
    (void)snprintf(path, sizeof path, "%s", sp);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    struct swap_ctx c;
    memset(&c, 0, sizeof c);
    c.set_source = false;
    float fps = 0.0f;
    pc_status rc = load_frames(path, fmt, &c.frames, &c.nframes, &fps, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = edit_video(s, "Reload Frame", mut_swap, &c, out, err);
    for (size_t i = 0; i < c.nframes; i++) {
        pc_surface_fini(&c.frames[i]);
    }
    free(c.frames);
    return rc;
}

struct ftl_ctx {
    pc_surface *frames;
    size_t nframes;
    pc_pixel_format fmt;
};

static pc_status edit_frames_to_layers(pc_document *doc, bool *has_active, pc_layer_id *active,
                                       void *vctx, pc_error *err)
{
    struct ftl_ctx *c = vctx;
    pc_layer_id above = (*has_active) ? *active : 0;
    bool have_above = *has_active;
    for (size_t i = 0; i < c->nframes; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "Frame %zu", i + 1);
        pc_layer l;
        pc_status rc = pc_layer_raster(&l, name, c->fmt, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface *su = pc_layer_surface_mut(&l);
        pc_surface_fini(su);
        if (pc_surface_clone(&c->frames[i], su, err) != PC_OK) {
            pc_layer_fini(&l);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(doc, have_above ? &above : NULL, &l, &nid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&l);
            return rc;
        }
        above = nid;
        have_above = true;
    }
    if (have_above) {
        *active = above;
        *has_active = true;
    }
    return PC_OK;
}

static pc_status run_frames_to_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    char path[4096];
    if (path_param(p, "file.import.videoFramesToLayers", path, sizeof path, err) == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_pixel_format fmt = pc_document_pixel_format(pc_doc_arc_get(d->doc));
    struct ftl_ctx c;
    memset(&c, 0, sizeof c);
    c.fmt = fmt;
    float fps = 0.0f;
    pc_status rc = load_frames(path, fmt, &c.frames, &c.nframes, &fps, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t n = c.nframes;
    rc = pc_engine_session_edit(s, "Video Frames to Layers", edit_frames_to_layers, &c, err);
    for (size_t i = 0; i < c.nframes; i++) {
        pc_surface_fini(&c.frames[i]);
    }
    free(c.frames);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layers", n);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- Render Video (video_cmds::render_video) ---------------------------------------------- */

/* One animated-GIF frame from straight RGBA pixels (Save for Web's GIF rule): pixels under 50 %
 * opacity take the transparent index, the palette is built from the visible pixels. *idx and *pal
 * are owned on success. */
static pc_status gif_frame_build(float (*px)[4], size_t npx, size_t w, uint8_t **idx_out,
                                 uint8_t (**pal_out)[3], size_t *npal_out, int *transparent_out,
                                 pc_error *err)
{
    bool transparent = false;
    for (size_t i = 0; i < npx; i++) {
        if (px[i][3] < 0.5f) {
            transparent = true;
            break;
        }
    }
    float(*visible)[4] = malloc((npx ? npx : 1) * sizeof *visible);
    if (visible == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gif frame");
    }
    size_t nv = 0;
    for (size_t i = 0; i < npx; i++) {
        if (px[i][3] >= 0.5f) {
            visible[nv][0] = px[i][0];
            visible[nv][1] = px[i][1];
            visible[nv][2] = px[i][2];
            visible[nv][3] = 1.0f;
            nv++;
        }
    }
    pc_algo_palette pal;
    pc_status rc = pc_algo_quantize_build_palette((const float(*)[4])(nv > 0 ? visible : px), nv > 0 ? nv : npx,
                                                  PC_ALGO_PALETTE_ADAPTIVE, transparent ? 255 : 256,
                                                  PC_ALGO_FORCED_NONE, &pal, err);
    free(visible);
    if (rc != PC_OK) {
        return rc;
    }
    int tindex = -1;
    if (transparent) {
        if (pal.n > 255) {
            pal.n = 255;
        }
        if (pal.n < 256) {
            pal.c[pal.n][0] = pal.c[0][0];
            pal.c[pal.n][1] = pal.c[0][1];
            pal.c[pal.n][2] = pal.c[0][2];
            pal.n++;
        }
        tindex = (int)pal.n - 1;
    }
    uint8_t *idx = malloc(npx ? npx : 1);
    uint8_t(*palette)[3] = malloc((pal.n ? pal.n : 1) * sizeof *palette);
    if (idx == NULL || palette == NULL) {
        free(idx);
        free(palette);
        return pc_fail(err, PC_ERR_NOMEM, "gif frame");
    }
    uint8_t *flat = (uint8_t *)pal.c;
    rc = pc_algo_quantize(px, npx, w, flat, pal.n, PC_ALGO_DITHER_NONE, 1.0f, tindex >= 0,
                          tindex >= 0 ? (size_t)tindex : 0, idx, err);
    if (rc != PC_OK) {
        free(idx);
        free(palette);
        return rc;
    }
    memcpy(palette, pal.c, pal.n * sizeof *palette);
    *idx_out = idx;
    *pal_out = palette;
    *npal_out = pal.n;
    *transparent_out = tindex;
    return PC_OK;
}

static pc_status run_render_video(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.export.renderVideo";
    size_t dl = 0;
    const char *dir = p != NULL ? pc_json_as_str(pc_json_get(p, "dir"), &dl) : NULL;
    if (dir == NULL || dl == 0) {
        return pc_engine_err_bad_params(err, cmd, "need `dir`");
    }
    char dirbuf[4096];
    if (dl >= sizeof dirbuf) {
        return pc_engine_err_bad_params(err, cmd, "dir too long");
    }
    memcpy(dirbuf, dir, dl);
    dirbuf[dl] = '\0';
    size_t ffl = 0;
    const char *format = p != NULL ? pc_json_as_str(pc_json_get(p, "format"), &ffl) : NULL;
    char fmtbuf[16] = "png";
    if (format != NULL && ffl > 0 && ffl < sizeof fmtbuf) {
        memcpy(fmtbuf, format, ffl);
        fmtbuf[ffl] = '\0';
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    /* Work on a clone so store()/playhead changes don't touch the live document. */
    pc_document base;
    pc_status rc = pc_document_clone(pc_doc_arc_get(d->doc), &base, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_video_store(&base);
    size_t frames = base.has_timeline && base.timeline.duration > 1 ? base.timeline.duration : 1;
    float fps = base.has_timeline && base.timeline.fps > 1.0f ? base.timeline.fps : 30.0f;
    char stem[256];
    pc_engine_file_stem(pc_str_c(&base.name), stem, sizeof stem);
    if (stem[0] == '\0') {
        (void)snprintf(stem, sizeof stem, "Untitled");
    }
    pc_pixel_format fmt = pc_document_pixel_format(&base);
    pc_rect bounds = pc_document_bounds(&base);
    size_t w = (size_t)pc_rect_width(bounds), h = (size_t)pc_rect_height(bounds);

    yyjson_mut_doc *rd = NULL;
    bool gif = (fmtbuf[0] == 'g' || fmtbuf[0] == 'G');

    if (gif) {
        uint16_t delay = (uint16_t)0;
        {
            double dc = 100.0 / (double)fps;
            dc = dc < 1.0 ? 1.0 : (dc > 65535.0 ? 65535.0 : dc);
            delay = (uint16_t)(dc + 0.5);
        }
        pc_gif_frame *gf = pc_malloc_array(frames ? frames : 1, sizeof *gf);
        uint8_t **idxs = pc_malloc_array(frames ? frames : 1, sizeof *idxs);
        uint8_t(**pals)[3] = pc_malloc_array(frames ? frames : 1, sizeof *pals);
        if (gf == NULL || idxs == NULL || pals == NULL) {
            free(gf);
            free(idxs);
            free(pals);
            pc_document_fini(&base);
            return pc_fail(err, PC_ERR_NOMEM, "render video");
        }
        size_t built = 0;
        for (size_t fidx = 0; fidx < frames; fidx++) {
            if (base.has_timeline) {
                base.timeline.current = fidx;
            }
            pc_engine_video_sync(&base);
            pc_surface surf;
            rc = pc_engine_flattened(&base, fmt, &surf, err);
            if (rc != PC_OK) {
                goto gif_fail;
            }
            float(*px)[4] = malloc((w * h == 0 ? 1 : w * h) * sizeof *px);
            if (px == NULL) {
                pc_surface_fini(&surf);
                rc = pc_fail(err, PC_ERR_NOMEM, "render video");
                goto gif_fail;
            }
            rc = pc_surface_read_rgba_into(&surf, bounds, px, w * h, err);
            pc_surface_fini(&surf);
            if (rc != PC_OK) {
                free(px);
                goto gif_fail;
            }
            int tindex = -1;
            rc = gif_frame_build(px, w * h, w, &idxs[built], &pals[built], &gf[built].npal, &tindex, err);
            free(px);
            if (rc != PC_OK) {
                goto gif_fail;
            }
            gf[built].indices = idxs[built];
            gf[built].n = w * h;
            gf[built].palette = (const uint8_t(*)[3])pals[built];
            gf[built].transparent = tindex;
            gf[built].delay_cs = delay;
            built++;
            continue;
        gif_fail:
            for (size_t j = 0; j < built; j++) {
                free(idxs[j]);
                free(pals[j]);
            }
            free(gf);
            free(idxs);
            free(pals);
            pc_document_fini(&base);
            return rc;
        }
        pc_buf gifbuf = PC_BUF_INIT;
        rc = pc_web_encode_gif_animated((uint32_t)w, (uint32_t)h, gf, built, true, &gifbuf, err);
        for (size_t j = 0; j < built; j++) {
            free(idxs[j]);
            free(pals[j]);
        }
        free(gf);
        free(idxs);
        free(pals);
        if (rc != PC_OK) {
            pc_buf_fini(&gifbuf);
            pc_document_fini(&base);
            return rc;
        }
        char name[300];
        (void)snprintf(name, sizeof name, "%s.gif", stem);
        char *path = pc_engine_file_join(dirbuf, name);
        if (path == NULL) {
            pc_buf_fini(&gifbuf);
            pc_document_fini(&base);
            return pc_fail(err, PC_ERR_NOMEM, "render video path");
        }
        rc = pc_engine_write_file(path, gifbuf.data, gifbuf.len, err);
        pc_buf_fini(&gifbuf);
        pc_document_fini(&base);
        if (rc != PC_OK) {
            free(path);
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            free(path);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "frames", frames);
        yyjson_mut_obj_add_strcpy(rd, root, "file", path);
        yyjson_mut_doc_set_root(rd, root);
        free(path);
        return PC_OK;
    }

    /* Image sequence. */
    pc_io_export_opts opts = pc_io_export_opts_default();
    size_t written = 0;
    for (size_t fidx = 0; fidx < frames; fidx++) {
        if (base.has_timeline) {
            base.timeline.current = fidx;
        }
        pc_engine_video_sync(&base);
        char name[320];
        (void)snprintf(name, sizeof name, "%s_%04zu.%s", stem, fidx, fmtbuf);
        char *path = pc_engine_file_join(dirbuf, name);
        if (path == NULL) {
            pc_document_fini(&base);
            return pc_fail(err, PC_ERR_NOMEM, "render video path");
        }
        pc_io_export_result res = {0};
        rc = pc_io_export(&base, fmtbuf, &opts, &res, err);
        if (rc == PC_OK) {
            rc = pc_engine_write_file(path, res.bytes, res.len, err);
        }
        pc_io_export_result_fini(&res);
        free(path);
        if (rc != PC_OK) {
            pc_document_fini(&base);
            return rc;
        }
        written++;
    }
    pc_document_fini(&base);
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "frames", written);
    yyjson_mut_obj_add_strcpy(rd, root, "dir", dirbuf);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_FILE_IMPORT[] = {"File", "Import"};
static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};
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
    {"layer.videoLayers.newVideoLayerFromFile", "New Video Layer from File\xE2\x80\xA6", MENU_VL, 2, NULL,
     "{\"path\":str (an image or GIF),\"fps\":num?} \xE2\x86\x92 {layer,frames}", video_has_doc,
     run_new_from_file, true},
    {"layer.videoLayers.replaceFootage", "Replace Footage\xE2\x80\xA6", MENU_VL, 2, NULL,
     "{\"path\":str} \xE2\x86\x92 {}", active_video, run_replace_footage, true},
    {"layer.videoLayers.reloadFrame", "Reload Frame", MENU_VL, 2, NULL, "{} \xE2\x86\x92 {}", active_video,
     run_reload_frame, true},
    {"file.import.videoFramesToLayers", "Video Frames to Layers\xE2\x80\xA6", MENU_FILE_IMPORT, 2, NULL,
     "{\"path\":str (an image or GIF)} \xE2\x86\x92 {layers}", video_has_doc, run_frames_to_layers, true},
    {"file.export.renderVideo", "Render Video\xE2\x80\xA6", MENU_FILE_EXPORT, 2, NULL,
     "{\"dir\":folder,\"format\":\"png|jpg|gif|...\"=\"png\"} \xE2\x86\x92 {frames,dir|file} (an image "
     "sequence, or one animated GIF)",
     video_has_doc, run_render_video, true},
};

const pc_engine_command *pc_engine_video_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
