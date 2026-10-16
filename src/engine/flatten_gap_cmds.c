/* File > Scripts > Flatten All Layer Effects / Flatten All Masks (port of flatten_all_effects and
 * flatten_all_masks in crates/engine/src/file_cmds.rs).
 *
 *   - flattenAllLayerEffects: every layer with live effects is baked to a raster carrying those
 *     effects (pixel and vector masks stay live on the baked raster, so they still apply once).
 *   - flattenAllMasks: every raster layer with an enabled pixel or vector mask is baked with the
 *     mask folded into its alpha, and the masks are removed. Non-raster masked layers are skipped.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): no parameters to validate; every layer lookup and render
 * is bounds-checked, and an allocation failure returns a graceful error. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/fill.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "flatten_gap_cmds.h"

/* A compose buffer as a surface of `fmt` (file_cmds::buffer_surface). */
static pc_status buffer_to_surface(const pc_compose_buffer *buf, pc_pixel_format fmt, pc_surface *out,
                                   pc_error *err)
{
    pc_surface_init(out, fmt);
    if (buf->len == 0 || pc_rect_is_empty(buf->rect)) {
        pc_surface_prune(out);
        return PC_OK;
    }
    size_t n = pc_surface_channels(out);
    if (n == 0) {
        pc_surface_prune(out);
        return PC_OK;
    }
    float *data = pc_malloc_array(buf->len, n * sizeof *data);
    if (data == NULL) {
        pc_surface_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "flatten");
    }
    for (size_t i = 0; i < buf->len; i++) {
        (void)pc_from_rgba(fmt, buf->px[i], &data[i * n]);
    }
    pc_status rc = pc_surface_write_region(out, buf->rect, data, buf->len * n, err);
    free(data);
    if (rc != PC_OK) {
        pc_surface_fini(out);
        return rc;
    }
    pc_surface_prune(out);
    return PC_OK;
}

/* The layer rendered alone, keeping what the flags say, as a surface in `fmt`. */
static pc_status bake(const pc_layer *l, pc_rect canvas, pc_pixel_format fmt, bool keep_effects,
                      bool apply_mask, pc_surface *out, pc_error *err)
{
    pc_layer tmp;
    if (pc_layer_clone(l, &tmp, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    tmp.opacity = 1.0f;
    tmp.blend = PC_BLEND_NORMAL;
    tmp.visible = true;
    tmp.clipped = false;
    if (!keep_effects) {
        pc_effects_fini(&tmp.effects);
        memset(&tmp.effects, 0, sizeof tmp.effects);
        tmp.fill_opacity = 1.0f;
    }
    if (!apply_mask) {
        if (tmp.has_mask) {
            pc_layer_mask_fini(&tmp.mask);
            tmp.has_mask = false;
        }
        if (tmp.has_vector_mask) {
            pc_vector_mask_fini(&tmp.vector_mask);
            tmp.has_vector_mask = false;
        }
    }
    const pc_surface *surf = pc_layer_surface(l);
    pc_rect area = canvas;
    if (surf != NULL) {
        area = pc_rect_union(pc_surface_content_bounds(surf), canvas);
    }
    if (keep_effects) {
        area = pc_rect_inflate(area, 256);
    }
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render_layer(&tmp, area, &buf, err);
    pc_layer_fini(&tmp);
    if (rc != PC_OK) {
        return rc;
    }
    rc = buffer_to_surface(&buf, fmt, out, err);
    pc_compose_buffer_fini(&buf);
    return rc;
}

static bool has_live_effects(const pc_layer *l)
{
    if (!l->effects.enabled) {
        return false;
    }
    for (size_t i = 0; i < l->effects.nitems; i++) {
        if (pc_effect_enabled(&l->effects.items[i])) {
            return true;
        }
    }
    return false;
}

/* Drop every psd_block whose 4-byte key is in `keys` (each a 4-char code). Releases their data. */
static void retain_blocks(pc_layer *l, const char (*keys)[4], size_t nkeys)
{
    size_t k = 0;
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        bool drop = false;
        for (size_t j = 0; j < nkeys; j++) {
            if (memcmp(l->psd_blocks[i].key, keys[j], 4) == 0) {
                drop = true;
                break;
            }
        }
        if (drop) {
            pc_blob_release(l->psd_blocks[i].data);
        } else {
            l->psd_blocks[k++] = l->psd_blocks[i];
        }
    }
    l->npsd_blocks = k;
}

/* Collect every layer id in walk order (file_cmds::walk_ids). *out owned (free). */
static pc_status walk_ids(const pc_document *doc, pc_layer_id **out, size_t *nout, pc_error *err)
{
    *out = NULL;
    *nout = 0;
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = w.len > 0 ? malloc(w.len * sizeof *ids) : NULL;
    if (w.len > 0 && ids == NULL) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "flatten");
    }
    for (size_t i = 0; i < w.len; i++) {
        ids[i] = w.items[i].layer->id;
    }
    *nout = w.len;
    *out = ids;
    pc_walk_fini(&w);
    return PC_OK;
}

/* ---- flatten all layer effects ------------------------------------------------------------- */

struct flat_result {
    size_t flattened;
    size_t skipped;
};

static pc_status flatten_effects_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                      pc_error *err)
{
    (void)ha;
    (void)active;
    struct flat_result *res = vctx;
    pc_rect canvas = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_layer_id *ids = NULL;
    size_t n = 0;
    pc_status rc = walk_ids(doc, &ids, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    static const char drop[][4] = {{'T', 'y', 'S', 'h'}, {'S', 'o', 'L', 'd'}, {'P', 'l', 'L', 'd'},
                                   {'S', 'o', 'L', 'E'}, {'v', 'm', 's', 'k'}, {'v', 's', 'm', 's'},
                                   {'v', 'o', 'g', 'k'}, {'v', 's', 'c', 'g'}, {'v', 's', 't', 'k'}};
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l == NULL || !has_live_effects(l) || l->content.kind == PC_LAYER_GROUP ||
            l->content.kind == PC_LAYER_ADJUSTMENT) {
            continue;
        }
        pc_surface baked;
        rc = bake(l, canvas, fmt, true, false, &baked, err);
        if (rc != PC_OK) {
            free(ids);
            return rc;
        }
        pc_layer_id cur = ids[i];
        pc_layer *m = pc_document_layer_mut(doc, cur);
        if (m == NULL) {
            pc_surface_fini(&baked);
            free(ids);
            return pc_engine_err_no_layer(err, cur);
        }
        pc_layer_content content;
        memset(&content, 0, sizeof content);
        content.kind = PC_LAYER_RASTER;
        content.u.raster = baked;
        pc_layer_content_fini(&m->content);
        m->content = content;
        pc_effects_fini(&m->effects);
        memset(&m->effects, 0, sizeof m->effects);
        m->fill_opacity = 1.0f;
        if (m->has_fill_cache) {
            pc_fill_fini(&m->fill_cache.fill);
            pc_surface_fini(&m->fill_cache.surface);
            m->has_fill_cache = false;
        }
        retain_blocks(m, drop, sizeof drop / sizeof drop[0]);
        res->flattened++;
    }
    free(ids);
    return PC_OK;
}

static pc_status flat_result_json(struct flat_result *res, bool with_skipped, pc_json_value *out,
                                  pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "flattened", res->flattened);
    if (with_skipped) {
        yyjson_mut_obj_add_uint(rd, root, "skipped", res->skipped);
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_flatten_effects(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    (void)p;
    struct flat_result res = {0, 0};
    if (pc_engine_session_edit(s, "Flatten All Layer Effects", flatten_effects_edit, &res, err) !=
        PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return flat_result_json(&res, false, out, err);
}

/* ---- flatten all masks --------------------------------------------------------------------- */

static pc_status flatten_masks_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                    pc_error *err)
{
    (void)ha;
    (void)active;
    struct flat_result *res = vctx;
    pc_rect canvas = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_layer_id *ids = NULL;
    size_t n = 0;
    pc_status rc = walk_ids(doc, &ids, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    static const char drop[][4] = {{'v', 'm', 's', 'k'}, {'v', 's', 'm', 's'}};
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l == NULL) {
            continue;
        }
        bool masked = (l->has_mask && l->mask.enabled) || l->has_vector_mask;
        if (!masked) {
            continue;
        }
        if (l->content.kind != PC_LAYER_RASTER) {
            res->skipped++;
            continue;
        }
        pc_surface baked;
        rc = bake(l, canvas, fmt, false, true, &baked, err);
        if (rc != PC_OK) {
            free(ids);
            return rc;
        }
        pc_layer_id cur = ids[i];
        pc_layer *m = pc_document_layer_mut(doc, cur);
        if (m == NULL) {
            pc_surface_fini(&baked);
            free(ids);
            return pc_engine_err_no_layer(err, cur);
        }
        pc_layer_content content;
        memset(&content, 0, sizeof content);
        content.kind = PC_LAYER_RASTER;
        content.u.raster = baked;
        pc_layer_content_fini(&m->content);
        m->content = content;
        if (m->has_mask) {
            pc_layer_mask_fini(&m->mask);
            m->has_mask = false;
        }
        if (m->has_vector_mask) {
            pc_vector_mask_fini(&m->vector_mask);
            m->has_vector_mask = false;
        }
        retain_blocks(m, drop, sizeof drop / sizeof drop[0]);
        res->flattened++;
    }
    free(ids);
    return PC_OK;
}

static pc_status run_flatten_masks(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    struct flat_result res = {0, 0};
    if (pc_engine_session_edit(s, "Flatten All Masks", flatten_masks_edit, &res, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return flat_result_json(&res, true, out, err);
}

/* ---- registry ------------------------------------------------------------------------------ */

static const char *const MENU_SCRIPTS[] = {"File", "Scripts"};

static const pc_engine_command SPECS[] = {
    {"file.scripts.flattenAllLayerEffects", "Flatten All Layer Effects", MENU_SCRIPTS, 2, NULL, "{}",
     pc_engine_has_doc, run_flatten_effects, true},
    {"file.scripts.flattenAllMasks", "Flatten All Masks", MENU_SCRIPTS, 2, NULL, "{}", pc_engine_has_doc,
     run_flatten_masks, true},
};

const pc_engine_command *pc_engine_flatten_gap_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
