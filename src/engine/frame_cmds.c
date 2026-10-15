/* Layer > New > Frame from Layers, and the Frame tool (layer.new.frame): a group clipped to a
 * rectangle or ellipse. Built from existing primitives (group + clip mask). Port of
 * crates/engine/src/frame_cmds.rs (E12). */
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "doc/doc.h"
#include "engine_internal.h"
#include "layer_cmds.h"

#define MAX_FRAME_SIDE 300000

static pc_status frame_has_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    if (d->nselected == 0 && !d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "select a layer");
    }
    return PC_OK;
}

static pc_status frame_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

/* A reveal mask on inside `rect`, off everywhere else (GRAY8). */
static pc_status clip_mask(pc_rect rect, pc_layer_mask *out, pc_error *err)
{
    pc_layer_mask_hide_all(out);
    uint32_t w = pc_rect_width(rect), h = pc_rect_height(rect);
    if (w > 0 && h > 0) {
        size_t n = (size_t)w * (size_t)h;
        float *buf = pc_malloc_array(n, sizeof *buf);
        if (buf == NULL) {
            pc_layer_mask_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "frame");
        }
        for (size_t i = 0; i < n; i++) {
            buf[i] = 1.0f;
        }
        pc_status rc = pc_surface_write_region(&out->surface, rect, buf, n, err);
        free(buf);
        if (rc != PC_OK) {
            pc_layer_mask_fini(out);
            return rc;
        }
        pc_surface_prune(&out->surface);
    }
    return PC_OK;
}

/* A reveal mask on inside the ellipse inscribed in `rect`, off elsewhere, 4x4-supersampled, only
 * within `area`. */
static pc_status ellipse_mask(pc_rect rect, pc_rect area, pc_layer_mask *out, pc_error *err)
{
    pc_layer_mask_hide_all(out);
    double cx = ((double)rect.x0 + (double)rect.x1) / 2.0;
    double cy = ((double)rect.y0 + (double)rect.y1) / 2.0;
    uint32_t rw = pc_rect_width(rect), rh = pc_rect_height(rect);
    double rx = (double)(rw > 1 ? rw : 1) / 2.0;
    double ry = (double)(rh > 1 ? rh : 1) / 2.0;
    pc_rect cut = pc_rect_intersect(rect, area);
    for (int32_t y = cut.y0; y < cut.y1; y++) {
        for (int32_t x = cut.x0; x < cut.x1; x++) {
            int n = 0;
            for (int i = 0; i < 16; i++) {
                double fx = (double)x + ((double)(i % 4) + 0.5) / 4.0;
                double fy = (double)y + ((double)(i / 4) + 0.5) / 4.0;
                double dx = (fx - cx) / rx, dy = (fy - cy) / ry;
                if (dx * dx + dy * dy <= 1.0) {
                    n++;
                }
            }
            if (n > 0) {
                float v = (float)n / 16.0f;
                if (pc_surface_write_pixel(&out->surface, x, y, &v, 1, err) != PC_OK) {
                    pc_layer_mask_fini(out);
                    return err != NULL ? err->code : PC_ERR_NOMEM;
                }
            }
        }
    }
    pc_surface_prune(&out->surface);
    return PC_OK;
}

/* ---- layer.new.frameFromLayers ----------------------------------------------------------- */

typedef struct {
    pc_layer_id gid;
    pc_rect rect;
} frame_group_ctx;

static pc_status edit_frame_group(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                  pc_error *err)
{
    (void)has_active;
    (void)active;
    frame_group_ctx *c = ctx;
    pc_layer *g = pc_document_layer_mut(doc, c->gid);
    if (g == NULL) {
        return pc_engine_err_no_layer(err, c->gid);
    }
    pc_layer_mask m;
    pc_status rc = clip_mask(c->rect, &m, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (g->has_mask) {
        pc_layer_mask_fini(&g->mask);
    }
    g->mask = m;
    g->has_mask = true;
    return PC_OK;
}

/* Clip the group `gid` to its contents' bounds (frame_group). */
static pc_status frame_group(pc_engine_session *s, pc_layer_id gid, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *g = pc_document_layer(doc, gid);
    if (g == NULL) {
        return pc_engine_err_no_layer(err, gid);
    }
    pc_rect rect;
    if (!pc_engine_layer_bounds(g, &rect) || pc_rect_is_empty(rect)) {
        rect = pc_document_bounds(doc);
    }
    frame_group_ctx c = {.gid = gid, .rect = rect};
    pc_status rc = pc_engine_session_edit(s, "Frame from Layers", edit_frame_group, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", gid);
    yyjson_mut_val *fr = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, fr, rect.x0);
    yyjson_mut_arr_add_int(rd, fr, rect.y0);
    yyjson_mut_arr_add_uint(rd, fr, pc_rect_width(rect));
    yyjson_mut_arr_add_uint(rd, fr, pc_rect_height(rect));
    yyjson_mut_obj_add_val(rd, root, "frame", fr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static void journal_truncate(pc_engine_session *s, size_t n)
{
    for (size_t i = n; i < s->njournal; i++) {
        free(s->journal[i].id);
        pc_json_value_fini(&s->journal[i].params);
    }
    if (n < s->njournal) {
        s->njournal = n;
    }
}

static pc_status run_frame_from_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                       pc_error *err)
{
    /* group params: pass only "name" through */
    const char *group_params = "{}";
    char buf[512];
    size_t nlen = 0;
    const char *nm = NULL;
    yyjson_val *nv = p != NULL ? pc_json_get(p, "name") : NULL;
    if (nv != NULL && (nm = pc_json_as_str(nv, &nlen)) != NULL) {
        /* build {"name":"..."} with yyjson to escape safely */
        yyjson_mut_doc *gd = yyjson_mut_doc_new(NULL);
        if (gd != NULL) {
            yyjson_mut_val *go = yyjson_mut_obj(gd);
            yyjson_mut_obj_add_strncpy(gd, go, "name", nm, nlen);
            yyjson_mut_doc_set_root(gd, go);
            char *txt = yyjson_mut_write(gd, 0, NULL);
            if (txt != NULL && strlen(txt) < sizeof buf) {
                memcpy(buf, txt, strlen(txt) + 1);
                group_params = buf;
            }
            free(txt);
            yyjson_mut_doc_free(gd);
        }
    }
    size_t journal = s->njournal;
    pc_json_value grouped = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, "layer.new.groupFromLayers", group_params,
                                                  &grouped, err);
    if (rc != PC_OK) {
        return rc;
    }
    uint64_t gid_v = 0;
    yyjson_mut_val *lv = yyjson_mut_obj_get(pc_json_value_root(&grouped), "layer");
    bool ok = lv != NULL && yyjson_mut_is_int(lv);
    if (ok) {
        gid_v = yyjson_mut_get_uint(lv);
    }
    pc_json_value_fini(&grouped);
    if (!ok) {
        return pc_engine_err_other(err, "grouping did not return a layer");
    }
    pc_layer_id gid = gid_v;
    rc = frame_group(s, gid, out, err);
    if (rc == PC_OK) {
        /* Fold the grouping step into the frame step. */
        pc_engine_doc_state *st = pc_engine_session_active_mut(s);
        if (st != NULL) {
            (void)pc_history_purge_last(st->history);
        }
        return PC_OK;
    }
    /* Undo the grouping, truncate the journal, clear redo. */
    (void)pc_engine_session_undo(s);
    journal_truncate(s, journal);
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        pc_history_clear_redo(st->history);
    }
    return rc;
}

/* ---- layer.new.frame --------------------------------------------------------------------- */

typedef struct {
    pc_rect rect;
    bool ellipse;
    bool has_content;
    pc_layer_id content;
    bool has_name;
    char name[256];
    pc_layer_id gid; /* out */
} new_frame_ctx;

static pc_status group_push_child(pc_layer *g, pc_layer *child, pc_error *err)
{
    if (g->content.kind != PC_LAYER_GROUP) {
        return pc_fail(err, PC_ERR_INVALID, "not a group");
    }
    size_t n = g->content.u.group.nchildren;
    size_t need = n + 1;
    if (need == 0 || need > SIZE_MAX / sizeof *g->content.u.group.children) {
        return pc_fail(err, PC_ERR_LIMIT, "frame");
    }
    pc_layer *grown = realloc(g->content.u.group.children, need * sizeof *g->content.u.group.children);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "frame");
    }
    g->content.u.group.children = grown;
    g->content.u.group.children[n] = *child;
    g->content.u.group.nchildren = n + 1;
    return PC_OK;
}

static pc_status edit_new_frame(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                pc_error *err)
{
    new_frame_ctx *c = ctx;
    pc_layer_mask mask;
    pc_status rc;
    if (c->ellipse) {
        rc = ellipse_mask(c->rect, pc_document_bounds(doc), &mask, err);
    } else {
        rc = clip_mask(pc_rect_intersect(c->rect, pc_document_bounds(doc)), &mask, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_str name = PC_STR_INIT;
    if (c->has_name) {
        if (pc_str_set_c(&name, c->name, err) != PC_OK) {
            pc_layer_mask_fini(&mask);
            return PC_ERR_NOMEM;
        }
    } else {
        if (pc_document_next_layer_name(doc, "Frame", &name, err) != PC_OK) {
            pc_layer_mask_fini(&mask);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    pc_layer group;
    rc = pc_layer_group(&group, pc_str_c(&name), NULL, 0, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        pc_layer_mask_fini(&mask);
        return rc;
    }
    pc_layer_id gid;
    if (c->has_content) {
        const pc_layer *l = pc_document_layer(doc, c->content);
        if (l == NULL) {
            pc_layer_fini(&group);
            pc_layer_mask_fini(&mask);
            return pc_engine_err_no_layer(err, c->content);
        }
        if (l->content.kind != PC_LAYER_RASTER || pc_document_is_background(doc, c->content)) {
            pc_layer_fini(&group);
            pc_layer_mask_fini(&mask);
            return pc_engine_err_bad_params(err, "layer.new.frame",
                                            "only a pixel layer (not the Background) can go in a frame");
        }
        rc = pc_document_insert_above(doc, &c->content, &group, &gid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&group);
            pc_layer_mask_fini(&mask);
            return rc;
        }
        pc_layer child;
        rc = pc_document_remove(doc, c->content, &child, err);
        if (rc != PC_OK) {
            pc_layer_mask_fini(&mask);
            return rc;
        }
        pc_layer *g = pc_document_layer_mut(doc, gid);
        if (g == NULL) {
            pc_layer_fini(&child);
            pc_layer_mask_fini(&mask);
            return pc_engine_err_no_layer(err, gid);
        }
        rc = group_push_child(g, &child, err);
        if (rc != PC_OK) {
            pc_layer_fini(&child);
            pc_layer_mask_fini(&mask);
            return rc;
        }
        size_t depth = 0;
        if (pc_document_max_group_depth(doc, &depth, err) != PC_OK) {
            pc_layer_mask_fini(&mask);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (depth > PC_MAX_GROUP_DEPTH) {
            pc_layer_mask_fini(&mask);
            return pc_engine_err_other(err, "New Frame would nest layers deeper than %d groups",
                                       PC_MAX_GROUP_DEPTH);
        }
    } else {
        const pc_layer_id *above = *has_active ? active : NULL;
        rc = pc_document_insert_above(doc, above, &group, &gid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&group);
            pc_layer_mask_fini(&mask);
            return rc;
        }
    }
    pc_layer *g = pc_document_layer_mut(doc, gid);
    if (g == NULL) {
        pc_layer_mask_fini(&mask);
        return pc_engine_err_no_layer(err, gid);
    }
    if (g->has_mask) {
        pc_layer_mask_fini(&g->mask);
    }
    g->mask = mask;
    g->has_mask = true;
    *active = gid;
    *has_active = true;
    c->gid = gid;
    return PC_OK;
}

static pc_status run_new_frame(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.new.frame";
    int32_t x = 0, y = 0, w = 0, h = 0;
    bool present = false;
    pc_status prc;
    if ((prc = pc_engine_param_int_i32(CMD, p, "x", &present, &x, err)) != PC_OK) {
        return prc;
    }
    if (!present) {
        return pc_engine_err_bad_params(err, CMD, "missing `x`");
    }
    if ((prc = pc_engine_param_int_i32(CMD, p, "y", &present, &y, err)) != PC_OK) {
        return prc;
    }
    if (!present) {
        return pc_engine_err_bad_params(err, CMD, "missing `y`");
    }
    if ((prc = pc_engine_param_int_i32(CMD, p, "width", &present, &w, err)) != PC_OK) {
        return prc;
    }
    if (!present) {
        return pc_engine_err_bad_params(err, CMD, "missing `width`");
    }
    if ((prc = pc_engine_param_int_i32(CMD, p, "height", &present, &h, err)) != PC_OK) {
        return prc;
    }
    if (!present) {
        return pc_engine_err_bad_params(err, CMD, "missing `height`");
    }
    if (w < 1 || w > MAX_FRAME_SIDE || h < 1 || h > MAX_FRAME_SIDE) {
        return pc_engine_err_bad_params(err, CMD, "`width` and `height` must be 1..=%d", MAX_FRAME_SIDE);
    }
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x1 > INT32_MAX || y1 > INT32_MAX) {
        return pc_engine_err_bad_params(err, CMD, "the frame is outside the 32-bit coordinate range");
    }
    new_frame_ctx c;
    memset(&c, 0, sizeof c);
    c.rect = pc_rect_make(x, y, (int32_t)x1, (int32_t)y1);
    yyjson_val *sh = pc_json_get(p, "shape");
    if (sh != NULL) {
        size_t sl = 0;
        const char *ss = pc_json_as_str(sh, &sl);
        if (ss == NULL) {
            return pc_engine_err_bad_params(err, CMD, "`shape` must be \"rectangle\" or \"ellipse\"");
        }
        if (sl == 7 && memcmp(ss, "ellipse", 7) == 0) {
            c.ellipse = true;
        } else if (sl == 9 && memcmp(ss, "rectangle", 9) == 0) {
            c.ellipse = false;
        } else {
            return pc_engine_err_bad_params(err, CMD, "`shape` must be \"rectangle\" or \"ellipse\"");
        }
    }
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && !yyjson_is_null(lv)) {
        uint64_t id = 0;
        if (!pc_json_as_u64(lv, &id)) {
            return pc_engine_err_bad_params(err, CMD, "`layer` is not a layer id");
        }
        c.has_content = true;
        c.content = id;
    }
    yyjson_val *nv = pc_json_get(p, "name");
    size_t nl = 0;
    const char *nm = nv != NULL ? pc_json_as_str(nv, &nl) : NULL;
    if (nm != NULL && nl < sizeof c.name) {
        memcpy(c.name, nm, nl);
        c.name[nl] = '\0';
        c.has_name = true;
    }
    pc_status rc = pc_engine_session_edit(s, "New Frame", edit_new_frame, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    (void)pc_engine_set_selection(s, &c.gid, 1, true, c.gid, false, 0, NULL);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", c.gid);
    yyjson_mut_val *fr = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_int(rd, fr, x);
    yyjson_mut_arr_add_int(rd, fr, y);
    yyjson_mut_arr_add_int(rd, fr, w);
    yyjson_mut_arr_add_int(rd, fr, h);
    yyjson_mut_obj_add_val(rd, root, "frame", fr);
    yyjson_mut_obj_add_str(rd, root, "shape", c.ellipse ? "ellipse" : "rectangle");
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_LAYER_NEW[] = {"Layer", "New"};

static const pc_engine_command SPECS[] = {
    {"layer.new.frame", "New Frame", NULL, 0, NULL,
     "{\"x\":i32,\"y\":i32,\"width\":u32,\"height\":u32,\"shape\":\"rectangle|ellipse\"=\"rectangle\","
     "\"layer\":id?,\"name\":str?} \xE2\x86\x92 {layer, frame:[x,y,w,h], shape}: Frame tool; a group "
     "clipped to the shape, holding pixel layer `layer` if given",
     frame_has_doc, run_new_frame, true},
    {"layer.new.frameFromLayers", "Frame from Layers", MENU_LAYER_NEW, 2, NULL,
     "{\"name\":str?} \xE2\x86\x92 {layer, frame:[x,y,w,h]}: groups the selected layers and clips "
     "them to a frame rect",
     frame_has_layer, run_frame_from_layers, true},
};

const pc_engine_command *pc_engine_frame_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
