/* Layer > Matting, Layer > Mask All Objects, and Layer > Smart Objects > Reveal in Finder
 * (port of crates/engine/src/layer_menu_cmds.rs: defringe / remove_matte / color_decontaminate /
 * mask_all_objects / reveal_in_finder and the edit_rgba helper they share).
 *
 *   - layer.matting.defringe: edge pixels (within `width` of transparency) take the colour of the
 *     nearest pixels further inside, grown outward one ring at a time. Alpha is unchanged.
 *   - layer.matting.removeBlackMatte / removeWhiteMatte: un-premultiply colours composited over
 *     black / white (partial alpha only).
 *   - layer.matting.colorDecontaminate: the Select-and-Mask fringe decontamination driven by the
 *     layer's own alpha (pc_algo_matting_decontaminate).
 *   - layer.maskAllObjects: Select-Subject segmentation of the layer (or the composite for a
 *     non-pixel layer) becomes a layer mask.
 *   - layer.smartObjects.revealInFinder: native-only; show a linked smart object's file in the
 *     platform file manager (dryRun answers the program+args instead of spawning).
 *
 * Never crashes on any document / selection / params (REWRITE_PLAN.md rule 9): bounds are checked,
 * allocations capped by pc_malloc_array, and pixel work routes through the shared surface helpers. */
#include <math.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "base/json.h"
#include "base/mem.h"
#include "algo/matting.h"
#include "algo/segment.h"
#include "algo/selection.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "brush_cmds.h" /* pc_engine_paint_surface */
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "matting_gap_cmds.h"

extern char **environ;

/* ---- shared pixel helpers (layer_menu_cmds::read_rgba / write_rgba / edit_rgba) ------------ */

/* Encode `count` straight-RGBA pixels back into `surf` over `r`. */
static pc_status write_rgba(pc_surface *surf, pc_rect r, const float (*px)[4], size_t count, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_surface_channels(surf);
    if (count == 0 || n == 0) {
        return PC_OK;
    }
    float *buf = pc_malloc_array(count, n * sizeof *buf);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "write_rgba");
    }
    for (size_t i = 0; i < count; i++) {
        float enc[PC_RASTER_MAX_CHANNELS];
        (void)pc_from_rgba(fmt, px[i], enc);
        memcpy(buf + i * n, enc, n * sizeof *buf);
    }
    pc_status rc = pc_surface_write_region(surf, r, buf, count * n, err);
    free(buf);
    return rc;
}

/* The callback edit_rgba runs over the read-back pixels. */
typedef void (*rgba_fn)(float (*px)[4], uint32_t w, uint32_t h, size_t count, void *ctx);

/* Port of edit_rgba: read the layer's content bounds inflated by `grow` (clamped to the canvas,
 * also inflated), apply `f`, write back, prune. Nothing happens when the region is empty. */
static pc_status edit_rgba(pc_document *doc, pc_layer_id id, int32_t grow, rgba_fn f, void *ctx,
                           pc_error *err)
{
    pc_rect bounds = pc_document_bounds(doc);
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect r = pc_rect_intersect(pc_rect_inflate(pc_surface_content_bounds(surf), grow),
                                  pc_rect_inflate(bounds, grow));
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    uint32_t w = pc_rect_width(r), h = pc_rect_height(r);
    size_t count = (size_t)w * (size_t)h;
    float(*px)[4] = pc_malloc_array(count, sizeof *px);
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "matting");
    }
    rc = pc_surface_read_rgba_into(surf, r, px, count, err);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    f(px, w, h, count, ctx);
    rc = write_rgba(surf, r, (const float (*)[4])px, count, err);
    free(px);
    if (rc == PC_OK) {
        pc_surface_prune(surf);
    }
    return rc;
}

/* ---- enabled predicates -------------------------------------------------------------------- */

static pc_status has_raster(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer, not a pixel layer",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static pc_status has_linked_smart(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL ? reason->code : PC_ERR_INVALID;
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer is not a smart object");
    }
    if (!l->content.u.smart.source.linked) {
        return pc_fail(reason, PC_ERR_INVALID, "embedded smart objects have no file to reveal");
    }
    return PC_OK;
}

/* ---- matting.defringe ---------------------------------------------------------------------- */

struct defringe_ctx {
    uint32_t width;
};

/* Port of layer_menu_cmds::defringe: grow the known-inside colours outward across the fringe band
 * one ring at a time. Alpha untouched. */
static void defringe_fn(float (*px)[4], uint32_t w, uint32_t h, size_t count, void *vctx)
{
    struct defringe_ctx *c = vctx;
    if (count == 0) {
        return;
    }
    bool *outside = pc_malloc_array(count, sizeof *outside);
    if (outside == NULL) {
        return;
    }
    bool any_out = false;
    for (size_t i = 0; i < count; i++) {
        outside[i] = px[i][3] <= 0.01f;
        any_out = any_out || outside[i];
    }
    if (!any_out) {
        free(outside);
        return;
    }
    float *dist = NULL;
    if (pc_algo_selection_edt(outside, w, h, &dist, NULL) != PC_OK || dist == NULL) {
        free(outside);
        return;
    }
    bool *known = pc_malloc_array(count, sizeof *known);
    size_t *todo = pc_malloc_array(count, sizeof *todo);
    size_t *next = pc_malloc_array(count, sizeof *next);
    if (known == NULL || todo == NULL || next == NULL) {
        free(outside);
        free(dist);
        free(known);
        free(todo);
        free(next);
        return;
    }
    size_t ntodo = 0;
    for (size_t i = 0; i < count; i++) {
        known[i] = !outside[i] && dist[i] > (float)c->width;
        if (!outside[i] && !known[i]) {
            todo[ntodo++] = i;
        }
    }
    static const int dxs[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dys[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    /* Accumulated colours for this ring, applied after the ring is scanned (so neighbours use the
     * previous ring's colours, matching the Rust `updates` batch). */
    float(*acc)[3] = pc_malloc_array(count, sizeof *acc);
    bool *filled = pc_malloc_array(count, sizeof *filled);
    if (acc == NULL || filled == NULL) {
        free(outside);
        free(dist);
        free(known);
        free(todo);
        free(next);
        free(acc);
        free(filled);
        return;
    }
    while (ntodo > 0) {
        size_t nnext = 0;
        size_t nupd = 0;
        for (size_t t = 0; t < ntodo; t++) {
            size_t i = todo[t];
            int32_t x = (int32_t)(i % w), y = (int32_t)(i / w);
            float a[3] = {0.0f, 0.0f, 0.0f};
            float n = 0.0f;
            for (int k = 0; k < 8; k++) {
                int32_t nx = x + dxs[k], ny = y + dys[k];
                if (nx < 0 || ny < 0 || nx >= (int32_t)w || ny >= (int32_t)h) {
                    continue;
                }
                size_t j = (size_t)ny * w + (size_t)nx;
                if (known[j]) {
                    a[0] += px[j][0];
                    a[1] += px[j][1];
                    a[2] += px[j][2];
                    n += 1.0f;
                }
            }
            if (n > 0.0f) {
                acc[i][0] = a[0] / n;
                acc[i][1] = a[1] / n;
                acc[i][2] = a[2] / n;
                filled[i] = true;
                nupd++;
            } else {
                next[nnext++] = i;
                filled[i] = false;
            }
        }
        if (nupd == 0) {
            break; /* isolated fringe with no core colour nearby */
        }
        for (size_t t = 0; t < ntodo; t++) {
            size_t i = todo[t];
            if (filled[i]) {
                px[i][0] = acc[i][0];
                px[i][1] = acc[i][1];
                px[i][2] = acc[i][2];
                known[i] = true;
            }
        }
        memcpy(todo, next, nnext * sizeof *todo);
        ntodo = nnext;
    }
    free(outside);
    free(dist);
    free(known);
    free(todo);
    free(next);
    free(acc);
    free(filled);
}

struct one_layer_ctx {
    pc_layer_id id;
    void *inner;
    int32_t grow;
    rgba_fn fn;
};

static pc_status edit_one(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct one_layer_ctx *c = vctx;
    return edit_rgba(doc, c->id, c->grow, c->fn, c->inner, err);
}

static pc_status run_defringe(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double width = 1.0;
    yyjson_val *v = p != NULL ? pc_json_get(p, "width") : NULL;
    double d;
    if (v != NULL && pc_json_as_f64(v, &d)) {
        width = d;
    }
    if (!(width >= 1.0)) { /* also catches NaN */
        width = 1.0;
    }
    if (width > 200.0) {
        width = 200.0;
    }
    struct defringe_ctx dc = {(uint32_t)(width + 0.5)};
    struct one_layer_ctx c = {0, &dc, 1, defringe_fn};
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Defringe", edit_one, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- matting.removeBlackMatte / removeWhiteMatte ------------------------------------------- */

static void clampf_inplace(float *v)
{
    if (*v < 0.0f) {
        *v = 0.0f;
    } else if (*v > 1.0f) {
        *v = 1.0f;
    }
}

static void remove_matte_fn(float (*px)[4], uint32_t w, uint32_t h, size_t count, void *vctx)
{
    (void)w;
    (void)h;
    bool white = *(const bool *)vctx;
    for (size_t i = 0; i < count; i++) {
        float a = px[i][3];
        if (a > 0.0f && a < 1.0f) {
            for (int c = 0; c < 3; c++) {
                float v = white ? (px[i][c] - (1.0f - a)) / a : px[i][c] / a;
                clampf_inplace(&v);
                px[i][c] = v;
            }
        }
    }
}

static pc_status run_remove_matte(pc_engine_session *s, bool white, pc_json_value *out, pc_error *err)
{
    static bool w_true = true, w_false = false;
    struct one_layer_ctx c = {0, white ? &w_true : &w_false, 0, remove_matte_fn};
    if (pc_engine_layer_param(s, NULL, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, white ? "Remove White Matte" : "Remove Black Matte", edit_one, &c,
                               err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_remove_black(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_remove_matte(s, false, out, err);
}

static pc_status run_remove_white(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return run_remove_matte(s, true, out, err);
}

/* ---- matting.colorDecontaminate ------------------------------------------------------------ */

struct decon_ctx {
    pc_layer_id id;
    float radius;
    float amount;
};

static pc_status decon_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct decon_ctx *c = vctx;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect r = pc_surface_content_bounds(surf);
    if (pc_rect_is_empty(r) || !pc_surface_format(surf).alpha) {
        return PC_OK;
    }
    uint32_t w = pc_rect_width(r), h = pc_rect_height(r);
    size_t count = (size_t)w * (size_t)h;
    float(*px)[4] = pc_malloc_array(count, sizeof *px);
    uint8_t *alpha = pc_malloc_array(count, sizeof *alpha);
    if (px == NULL || alpha == NULL) {
        free(px);
        free(alpha);
        return pc_fail(err, PC_ERR_NOMEM, "color decontaminate");
    }
    rc = pc_surface_read_rgba_into(surf, r, px, count, err);
    if (rc != PC_OK) {
        free(px);
        free(alpha);
        return rc;
    }
    for (size_t i = 0; i < count; i++) {
        float v = px[i][3];
        clampf_inplace(&v);
        alpha[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
    free(px);
    pc_algo_region region = {r, alpha, count};
    pc_surface decon;
    rc = pc_algo_matting_decontaminate(surf, &region, c->radius, c->amount, &decon, err);
    free(alpha);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(surf);
    *surf = decon;
    return PC_OK;
}

static double clamp_num(yyjson_val *p, const char *key, double def, double lo, double hi)
{
    double v = def;
    yyjson_val *x = p != NULL ? pc_json_get(p, key) : NULL;
    double d;
    if (x != NULL && pc_json_as_f64(x, &d)) {
        v = d;
    }
    if (!(v >= lo)) { /* also NaN */
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return v;
}

static pc_status run_decontaminate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct decon_ctx c;
    c.amount = (float)clamp_num(p, "amount", 100.0, 0.0, 100.0);
    c.radius = (float)clamp_num(p, "radius", 4.0, 1.0, 100.0);
    if (pc_engine_layer_param(s, NULL, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Color Decontaminate", decon_edit, &c, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- maskAllObjects ------------------------------------------------------------------------ */

/* Composite sampler (ctx = the document): straight RGBA of the whole composition over r. */
static pc_status composite_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    const pc_document *doc = ctx;
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render(doc, r, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < n && i < buf.len; i++) {
        out[i][0] = buf.px[i][0];
        out[i][1] = buf.px[i][1];
        out[i][2] = buf.px[i][2];
        out[i][3] = buf.px[i][3];
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

struct mask_obj_ctx {
    pc_layer_id id;
    pc_algo_region region; /* moved into the edit, finalised by the caller */
};

static pc_status mask_obj_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct mask_obj_ctx *c = vctx;
    pc_rect bbox = c->region.bbox;
    uint32_t w = pc_rect_width(bbox), h = pc_rect_height(bbox);
    size_t count = (size_t)w * (size_t)h;
    pc_layer_mask mask;
    pc_layer_mask_hide_all(&mask);
    if (count > 0 && c->region.mask != NULL) {
        float *v = pc_malloc_array(count, sizeof *v);
        if (v == NULL) {
            pc_layer_mask_fini(&mask);
            return pc_fail(err, PC_ERR_NOMEM, "mask all objects");
        }
        for (size_t i = 0; i < count && i < c->region.len; i++) {
            v[i] = (float)c->region.mask[i] / 255.0f;
        }
        pc_status rc = pc_surface_write_region(&mask.surface, bbox, v, count, err);
        free(v);
        if (rc != PC_OK) {
            pc_layer_mask_fini(&mask);
            return rc;
        }
        pc_surface_prune(&mask.surface);
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        pc_layer_mask_fini(&mask);
        return pc_engine_err_no_layer(err, c->id);
    }
    if (l->has_mask) {
        pc_layer_mask_fini(&l->mask);
    }
    l->mask = mask;
    l->has_mask = true;
    return PC_OK;
}

static pc_status run_mask_all_objects(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    struct mask_obj_ctx c;
    memset(&c, 0, sizeof c);
    if (pc_engine_layer_param(s, p, &c.id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, c.id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c.id);
    }
    pc_rect canvas = pc_document_bounds(doc);
    pc_algo_sampler sampler;
    const pc_surface *surf = pc_layer_surface(l);
    if (surf != NULL && l->content.kind == PC_LAYER_RASTER) {
        sampler = pc_algo_surface_sampler(surf);
    } else {
        sampler.rgba = composite_rgba;
        sampler.ctx = doc;
    }
    pc_status rc = pc_algo_subject_select(&sampler, canvas, &c.region, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_algo_region_is_none(&c.region)) {
        pc_algo_region_fini(&c.region);
        return pc_engine_err_other(err, "no objects were found");
    }
    pc_rect bbox = c.region.bbox;
    rc = pc_engine_session_edit(s, "Mask All Objects", mask_obj_edit, &c, err);
    pc_algo_region_fini(&c.region);
    if (rc != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, arr, bbox.x0);
    yyjson_mut_arr_add_int(rd, arr, bbox.y0);
    yyjson_mut_arr_add_int(rd, arr, (int64_t)pc_rect_width(bbox));
    yyjson_mut_arr_add_int(rd, arr, (int64_t)pc_rect_height(bbox));
    yyjson_mut_obj_add_val(rd, root, "bounds", arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- smartObjects.revealInFinder ----------------------------------------------------------- */

/* The file-manager invocation that selects `path` on this platform. On Linux we open the parent
 * directory with xdg-open (no reliable cross-manager "select" flag). *out_args is a NULL-terminated
 * owned argv (free each string then the array); *program is a static string. */
static pc_status reveal_command(const char *path, const char **program, char ***out_args, pc_error *err)
{
    /* parent directory of path, or "." */
    const char *slash = strrchr(path, '/');
    char *parent;
    if (slash == NULL) {
        parent = pc_strndup(".", 1);
    } else if (slash == path) {
        parent = pc_strndup("/", 1);
    } else {
        parent = pc_strndup(path, (size_t)(slash - path));
    }
    if (parent == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "reveal");
    }
    char **args = calloc(2, sizeof *args);
    if (args == NULL) {
        free(parent);
        return pc_fail(err, PC_ERR_NOMEM, "reveal");
    }
    args[0] = parent;
    args[1] = NULL;
    *program = "xdg-open";
    *out_args = args;
    return PC_OK;
}

static pc_status run_reveal(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!d->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_engine_err_other(err, "no active layer");
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_other(err, "the active layer is not a smart object");
    }
    if (!l->content.u.smart.source.linked) {
        return pc_engine_err_other(err, "embedded smart objects have no file to reveal");
    }
    const char *path = pc_str_c(&l->content.u.smart.source.path);
    bool dry_run = false;
    yyjson_val *v = p != NULL ? pc_json_get(p, "dryRun") : NULL;
    bool b;
    if (v != NULL && pc_json_as_bool(v, &b)) {
        dry_run = b;
    }
    const char *program = NULL;
    char **args = NULL;
    pc_status rc = reveal_command(path, &program, &args, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        for (size_t i = 0; args[i] != NULL; i++) {
            free(args[i]);
        }
        free(args);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    if (dry_run) {
        yyjson_mut_obj_add_strcpy(rd, root, "program", program);
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        for (size_t i = 0; args[i] != NULL; i++) {
            yyjson_mut_arr_add_strcpy(rd, arr, args[i]);
        }
        yyjson_mut_obj_add_val(rd, root, "args", arr);
    } else {
        /* Build argv = {program, args..., NULL} and spawn, detached. */
        size_t na = 0;
        while (args[na] != NULL) {
            na++;
        }
        char *prog0 = pc_strndup(program, strlen(program));
        char **argv = calloc(na + 2, sizeof *argv);
        if (argv == NULL || prog0 == NULL) {
            free(prog0);
            free(argv);
            for (size_t i = 0; args[i] != NULL; i++) {
                free(args[i]);
            }
            free(args);
            return pc_fail(err, PC_ERR_NOMEM, "reveal");
        }
        argv[0] = prog0;
        for (size_t i = 0; i < na; i++) {
            argv[i + 1] = args[i];
        }
        argv[na + 1] = NULL;
        pid_t pid;
        int sr = posix_spawnp(&pid, program, NULL, NULL, argv, environ);
        free(argv);
        free(prog0);
        if (sr != 0) {
            for (size_t i = 0; args[i] != NULL; i++) {
                free(args[i]);
            }
            free(args);
            return pc_engine_err_other(err, "%s: could not launch the file manager", program);
        }
        yyjson_mut_obj_add_strcpy(rd, root, "path", path);
    }
    for (size_t i = 0; args[i] != NULL; i++) {
        free(args[i]);
    }
    free(args);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- registry ------------------------------------------------------------------------------ */

static const char *const MENU_MATTING[] = {"Layer", "Matting"};
static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_SMART[] = {"Layer", "Smart Objects"};

static const pc_engine_command SPECS[] = {
    {"layer.maskAllObjects", "Mask All Objects", MENU_LAYER, 1, NULL, "{\"layer\":id?}",
     pc_engine_has_layer, run_mask_all_objects, true},
    {"layer.matting.defringe", "Defringe…", MENU_MATTING, 2, NULL, "{\"width\":1..200=1}", has_raster,
     run_defringe, true},
    {"layer.matting.removeBlackMatte", "Remove Black Matte", MENU_MATTING, 2, NULL, "{}", has_raster,
     run_remove_black, true},
    {"layer.matting.removeWhiteMatte", "Remove White Matte", MENU_MATTING, 2, NULL, "{}", has_raster,
     run_remove_white, true},
    {"layer.matting.colorDecontaminate", "Color Decontaminate…", MENU_MATTING, 2, NULL,
     "{\"amount\":0..100=100,\"radius\":1..100=4}", has_raster, run_decontaminate, true},
    {"layer.smartObjects.revealInFinder", "Reveal in Finder", MENU_SMART, 2, NULL,
     "{\"dryRun\":bool=false}", has_linked_smart, run_reveal, true},
};

const pc_engine_command *pc_engine_matting_gap_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
