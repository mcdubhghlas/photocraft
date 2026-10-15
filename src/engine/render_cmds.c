/* Filter > Render > Flame, Picture Frame and Tree: procedural renderers backed by
 * photocraft-algo's render2 (port of crates/engine/src/render_cmds.rs, E9).
 *
 * They draw onto the active pixel layer (or a new layer with newLayer:true, or when the active
 * layer has no pixels), inside the selection, in any colour mode and depth, as one undo step.
 * Results report what was drawn (layer, bounds, primitives) so agents can verify without
 * screenshots. The spec structs deserialise through the strict algo JSON readers, so this module
 * replicates the reference's dialog-unit marshalling (colour parsing, whole-float -> integer for
 * u32 fields, and 1-based names for the enumerated tree/frame designs) before handing them over. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/render2.h"
#include "engine_internal.h"
#include "vector/vector.h"

/* ---- colour + param marshalling ---------------------------------------------------------- */

/* [r,g,b(,a)] (0..1) or "#rrggbb" -> straight RGBA (render_cmds::parse_colour), from the original
 * immutable params. */
static bool parse_colour(yyjson_val *v, float out[4])
{
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        const double def[4] = {0.0, 0.0, 0.0, 1.0};
        for (size_t j = 0; j < 4; j++) {
            yyjson_val *e = yyjson_arr_get(v, j);
            double dd;
            out[j] = (e != NULL && pc_json_as_f64(e, &dd)) ? (float)dd : (float)def[j];
        }
        return true;
    }
    if (yyjson_is_str(v) && yyjson_get_len(v) == 7) {
        const char *h = yyjson_get_str(v);
        if (h != NULL && h[0] == '#') {
            for (size_t j = 0; j < 3; j++) {
                char b[3] = {h[1 + j * 2], h[2 + j * 2], 0};
                char *end = NULL;
                long val = strtol(b, &end, 16);
                out[j] = (end == b + 2) ? (float)val / 255.0f : 0.0f;
            }
            out[3] = 1.0f;
            return true;
        }
    }
    return false;
}

/* Replace `key` in the mut copy with its parsed [r,g,b,a] array, or drop it when the original
 * params' value is unparseable (normalized()). Parsing reads the immutable source `p`. */
static void normalize_colour(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, yyjson_val *p)
{
    yyjson_val *src = pc_json_get(p, key);
    if (src == NULL) {
        return;
    }
    yyjson_mut_obj_remove_key(obj, key);
    float c[4];
    if (parse_colour(src, c)) {
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        for (size_t j = 0; j < 4; j++) {
            yyjson_mut_arr_add_real(d, arr, c[j]);
        }
        yyjson_mut_obj_add_val(d, obj, key, arr);
    }
}

/* A finite non-integer number rounds to the nearest integer (whole_numbers()). */
static void whole_number(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    if (v == NULL || !yyjson_mut_is_real(v)) {
        return;
    }
    double f = yyjson_mut_get_num(v);
    if (!isfinite(f)) {
        return;
    }
    yyjson_mut_obj_remove_key(obj, key);
    yyjson_mut_obj_add_int(d, obj, key, (int64_t)llround(f));
}

/* lowercase and strip spaces/dashes/underscores, like render_cmds::index_param. */
static void squash(const char *s, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; s[i] != '\0' && o + 1 < cap; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == ' ' || ch == '-' || ch == '_') {
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            ch = (unsigned char)(ch - 'A' + 'a');
        }
        out[o++] = (char)ch;
    }
    out[o] = '\0';
}

/* A string value becomes its 1-based position in `names` (or 1 if unknown); numbers are left
 * for whole_number to normalise (index_param()). */
static void index_param(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key,
                        const char *const *names, size_t nnames)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, key);
    if (v == NULL || !yyjson_mut_is_str(v)) {
        return;
    }
    const char *s = yyjson_mut_get_str(v);
    char want[64];
    squash(s != NULL ? s : "", want, sizeof want);
    int64_t idx = 1;
    for (size_t i = 0; i < nnames; i++) {
        char have[64];
        squash(names[i], have, sizeof have);
        if (strcmp(have, want) == 0) {
            idx = (int64_t)i + 1;
            break;
        }
    }
    yyjson_mut_obj_remove_key(obj, key);
    yyjson_mut_obj_add_int(d, obj, key, idx);
}

/* A mutable copy of the params object (minus layer/newLayer/path), ready to transform. */
static yyjson_mut_val *params_copy(yyjson_mut_doc *d, yyjson_val *p)
{
    yyjson_mut_val *obj;
    if (p != NULL && yyjson_is_obj(p)) {
        obj = pc_json_normalized_copy_val(d, p);
        if (obj == NULL || !yyjson_mut_is_obj(obj)) {
            obj = yyjson_mut_obj(d);
        }
    } else {
        obj = yyjson_mut_obj(d);
    }
    if (obj != NULL) {
        yyjson_mut_obj_remove_key(obj, "layer");
        yyjson_mut_obj_remove_key(obj, "newLayer");
        yyjson_mut_obj_remove_key(obj, "path");
    }
    return obj;
}

/* ---- drawing onto a layer ---------------------------------------------------------------- */

struct draw_ctx {
    const pc_algo_prim *prims;
    size_t nprims;
    bool want_new;
    bool has_target;
    pc_layer_id target;
    const char *label; /* new-layer name = label without a trailing ellipsis */
    /* outputs */
    pc_layer_id layer;
    bool created;
    bool has_used;
    pc_rect used;
};

/* paint_surface for the Pixels target (Null params): lock check, then the raster surface. */
static pc_status pixel_surface(pc_document *doc, pc_layer_id id, pc_surface **out, pc_error *err)
{
    pc_locks locks = pc_document_effective_locks(doc, id);
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (locks.pixels || locks.all) {
        return pc_engine_err_other(
            err, "Could not complete your request because the layer \"%s\" is locked", pc_str_c(&l->name));
    }
    pc_surface *s = pc_layer_surface_mut(l);
    if (s == NULL) {
        return pc_engine_err_other(err, "not a pixel layer");
    }
    *out = s;
    return PC_OK;
}

static pc_status edit_draw(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                           pc_error *err)
{
    struct draw_ctx *c = vctx;
    bool have_id = false;
    pc_layer_id id = 0;
    if (c->has_target) {
        id = c->target;
        have_id = true;
    } else if (*has_active) {
        id = *active;
        have_id = true;
    }
    bool raster = have_id && pc_document_layer(doc, id) != NULL &&
                  pc_document_layer(doc, id)->content.kind == PC_LAYER_RASTER;
    if (c->want_new || !raster) {
        /* A new raster layer named like the command (minus the ellipsis). */
        char name[64];
        size_t n = strlen(c->label);
        /* strip a trailing "\xE2\x80\xA6" (U+2026). */
        if (n >= 3 && (unsigned char)c->label[n - 3] == 0xE2 && (unsigned char)c->label[n - 2] == 0x80 &&
            (unsigned char)c->label[n - 1] == 0xA6) {
            n -= 3;
        }
        if (n >= sizeof name) {
            n = sizeof name - 1;
        }
        memcpy(name, c->label, n);
        name[n] = '\0';
        pc_layer l;
        if (pc_layer_raster(&l, name, pc_document_pixel_format(doc), err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_layer_id nid;
        if (pc_document_insert_above(doc, *has_active ? active : NULL, &l, &nid, err) != PC_OK) {
            pc_layer_fini(&l);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        *active = nid;
        *has_active = true;
        id = nid;
        c->created = true;
    } else {
        c->created = false;
    }

    pc_rect clip = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    if (sel != NULL) {
        clip = pc_rect_intersect(clip, pc_surface_content_bounds(sel));
    }
    pc_surface *surf = NULL;
    pc_status rc = pixel_surface(doc, id, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect used;
    rc = pc_algo_render_composite(surf, c->prims, c->nprims, clip, sel, &used, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    c->layer = id;
    c->used = used;
    c->has_used = !pc_rect_is_empty(used);
    return PC_OK;
}

/* Run the edit then build the {layer,newLayer,bounds,primitives,...extra} result. */
static pc_status draw(pc_engine_session *s, const char *label, yyjson_val *p,
                      const pc_algo_prims *prims, pc_json_value *out, pc_error *err,
                      void (*add_extra)(yyjson_mut_doc *, yyjson_mut_val *, void *), void *extra_ud)
{
    struct draw_ctx c = {0};
    c.prims = prims->v;
    c.nprims = prims->n;
    yyjson_val *lv = pc_json_get(p, "newLayer");
    bool wn = false;
    if (lv != NULL) {
        (void)pc_json_as_bool(lv, &wn);
    }
    c.want_new = wn;
    yyjson_val *tv = pc_json_get(p, "layer");
    if (tv != NULL && yyjson_is_uint(tv)) {
        c.target = (pc_layer_id)yyjson_get_uint(tv);
        c.has_target = true;
    }
    c.label = label;
    pc_status rc = pc_engine_session_edit(s, label, edit_draw, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", c.layer);
    yyjson_mut_obj_add_bool(rd, root, "newLayer", c.created);
    if (c.has_used) {
        yyjson_mut_val *bb = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_int(rd, bb, c.used.x0);
        yyjson_mut_arr_add_int(rd, bb, c.used.y0);
        yyjson_mut_arr_add_int(rd, bb, c.used.x1);
        yyjson_mut_arr_add_int(rd, bb, c.used.y1);
        yyjson_mut_obj_add_val(rd, root, "bounds", bb);
    } else {
        yyjson_mut_obj_add_null(rd, root, "bounds");
    }
    yyjson_mut_obj_add_uint(rd, root, "primitives", c.nprims);
    if (add_extra != NULL) {
        add_extra(rd, root, extra_ud);
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- paths (Flame follows a path) -------------------------------------------------------- */

static bool str_ieq(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == *b;
}

/* The path Flame follows, or NULL (document has no usable path). *out/npaths own the polylines
 * (free each .v then the array). */
static pc_algo_polyline *flame_paths(const pc_engine_session *s, yyjson_val *p, size_t *npaths)
{
    *npaths = 0;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return NULL;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_path *path = NULL;
    yyjson_val *pv = pc_json_get(p, "path");
    size_t nlen = 0;
    const char *name = (pv != NULL && yyjson_is_str(pv)) ? yyjson_get_str(pv) : NULL;
    if (name != NULL) {
        nlen = yyjson_get_len(pv);
    }
    if (name != NULL && nlen > 0 && !str_ieq(name, "work")) {
        for (size_t i = 0; i < doc->npaths; i++) {
            if (strcmp(pc_str_c(&doc->paths[i].name), name) == 0) {
                path = &doc->paths[i].path;
                break;
            }
        }
    } else {
        if (doc->has_work_path) {
            path = &doc->work_path;
        } else if (doc->npaths > 0) {
            path = &doc->paths[0].path;
        }
    }
    if (path == NULL) {
        return NULL;
    }
    pc_vec_polyline *pls = NULL;
    size_t npl = 0;
    if (pc_vec_flatten_path(path, 0.5, &pls, &npl, NULL) != PC_OK) {
        return NULL;
    }
    pc_algo_polyline *out = calloc(npl ? npl : 1, sizeof *out);
    if (out == NULL) {
        pc_vec_polylines_free(pls, npl);
        return NULL;
    }
    size_t count = 0;
    for (size_t i = 0; i < npl; i++) {
        size_t m = pls[i].len;
        if (m == 0) {
            continue;
        }
        size_t cap = m + (pls[i].closed ? 1 : 0);
        float(*v)[2] = calloc(cap, sizeof *v);
        if (v == NULL) {
            continue;
        }
        for (size_t k = 0; k < m; k++) {
            v[k][0] = (float)pls[i].pts[k][0];
            v[k][1] = (float)pls[i].pts[k][1];
        }
        if (pls[i].closed) {
            v[m][0] = v[0][0];
            v[m][1] = v[0][1];
        }
        out[count].v = v;
        out[count].n = cap;
        count++;
    }
    pc_vec_polylines_free(pls, npl);
    if (count == 0) {
        free(out);
        return NULL;
    }
    *npaths = count;
    return out;
}

static void free_paths(pc_algo_polyline *paths, size_t n)
{
    if (paths == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(paths[i].v);
    }
    free(paths);
}

static pc_rect canvas_of(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_rect_make(0, 0, 1, 1);
    }
    return pc_document_bounds(pc_doc_arc_get(d->doc));
}

/* ---- commands ---------------------------------------------------------------------------- */

struct flame_extra {
    bool used_path;
};
static void flame_add_extra(yyjson_mut_doc *d, yyjson_mut_val *root, void *ud)
{
    struct flame_extra *e = ud;
    yyjson_mut_obj_add_bool(d, root, "usedPath", e->used_path);
}

static pc_status run_flame(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "flame params");
    }
    yyjson_mut_val *m = params_copy(md, p);
    if (m == NULL) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, "flame params");
    }
    normalize_colour(md, m, "color", p);
    bool had_color = yyjson_mut_obj_get(m, "color") != NULL;
    yyjson_mut_val *uc = yyjson_mut_obj_get(m, "useCustomColor");
    bool keep = uc != NULL ? yyjson_mut_get_bool(uc) : had_color;
    if (!keep) {
        yyjson_mut_obj_remove_key(m, "color");
    }
    yyjson_mut_obj_remove_key(m, "useCustomColor");
    /* quality: a number clamps to 0..4; a name maps to its index (default medium = 2). */
    yyjson_mut_val *q = yyjson_mut_obj_get(m, "quality");
    if (q != NULL) {
        int64_t qi = 2;
        if (yyjson_mut_is_num(q)) {
            double qf = yyjson_mut_get_num(q);
            if (qf < 0.0) {
                qf = 0.0;
            }
            if (qf > 4.0) {
                qf = 4.0;
            }
            qi = (int64_t)qf; /* `as u32` truncates toward zero */
            yyjson_mut_obj_remove_key(m, "quality");
            yyjson_mut_obj_add_int(md, m, "quality", qi);
        } else if (yyjson_mut_is_str(q)) {
            static const char *const QN[] = {"draft", "low", "medium", "high", "fine"};
            const char *s2 = yyjson_mut_get_str(q);
            qi = 2;
            for (int i = 0; i < 5; i++) {
                if (s2 != NULL && strcmp(QN[i], s2) == 0) {
                    qi = i;
                    break;
                }
            }
            yyjson_mut_obj_remove_key(m, "quality");
            yyjson_mut_obj_add_int(md, m, "quality", qi);
        }
    }
    whole_number(md, m, "seed");
    yyjson_mut_doc_set_root(md, m);

    yyjson_doc *imm = yyjson_mut_doc_imut_copy(md, NULL);
    yyjson_mut_doc_free(md);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "flame params");
    }
    pc_algo_flame_spec spec;
    {
        pc_error sub;
        pc_error_clear(&sub);
        if (pc_algo_flame_spec_from_json(yyjson_doc_get_root(imm), &spec, &sub) != PC_OK) {
            yyjson_doc_free(imm);
            return pc_engine_err_other(err, "invalid parameters: %s", sub.msg);
        }
    }
    yyjson_doc_free(imm);

    size_t npaths = 0;
    pc_algo_polyline *paths = flame_paths(s, p, &npaths);
    bool used_path = paths != NULL;
    float fallback[33][2];
    pc_algo_polyline one;
    float candle[1][2];
    if (paths == NULL) {
        pc_rect c = canvas_of(s);
        if (spec.flame_type == PC_ALGO_FLAME_CANDLE_LIGHT) {
            candle[0][0] = (float)c.x0 + (float)pc_rect_width(c) * 0.5f;
            candle[0][1] = (float)c.y0 + (float)pc_rect_height(c) * 0.75f;
            one.v = candle;
            one.n = 1;
        } else {
            pc_algo_default_flame_path(c, fallback);
            one.v = fallback;
            one.n = 33;
        }
        paths = &one;
        npaths = 1;
    }
    pc_algo_prims prims;
    pc_status rc = pc_algo_flame(&spec, paths, npaths, &prims, err);
    if (used_path) {
        free_paths(paths, npaths);
    }
    if (rc != PC_OK) {
        return rc;
    }
    struct flame_extra ex = {used_path};
    rc = draw(s, "Flame\xE2\x80\xA6", p, &prims, out, err, flame_add_extra, &ex);
    pc_algo_prims_fini(&prims);
    return rc;
}

struct name_extra {
    const char *key;
    const char *name;
};
static void name_add_extra(yyjson_mut_doc *d, yyjson_mut_val *root, void *ud)
{
    struct name_extra *e = ud;
    yyjson_mut_obj_add_str(d, root, e->key, e->name);
}

static pc_status run_tree(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "tree params");
    }
    yyjson_mut_val *m = params_copy(md, p);
    if (m == NULL) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, "tree params");
    }
    normalize_colour(md, m, "leavesColor", p);
    normalize_colour(md, m, "branchesColor", p);
    index_param(md, m, "baseTreeType", pc_algo_tree_type_names, PC_ALGO_TREE_TYPES);
    if (yyjson_mut_obj_get(m, "leavesColor") != NULL && yyjson_mut_obj_get(m, "defaultLeaves") == NULL) {
        yyjson_mut_obj_add_bool(md, m, "defaultLeaves", false);
    }
    if (yyjson_mut_obj_get(m, "branchesColor") != NULL &&
        yyjson_mut_obj_get(m, "customBranchColor") == NULL) {
        yyjson_mut_obj_add_bool(md, m, "customBranchColor", true);
    }
    whole_number(md, m, "baseTreeType");
    whole_number(md, m, "lightDirection");
    whole_number(md, m, "seed");
    yyjson_mut_doc_set_root(md, m);

    yyjson_doc *imm = yyjson_mut_doc_imut_copy(md, NULL);
    yyjson_mut_doc_free(md);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "tree params");
    }
    pc_algo_tree_spec spec;
    {
        pc_error sub;
        pc_error_clear(&sub);
        if (pc_algo_tree_spec_from_json(yyjson_doc_get_root(imm), &spec, &sub) != PC_OK) {
            yyjson_doc_free(imm);
            return pc_engine_err_other(err, "invalid parameters: %s", sub.msg);
        }
    }
    yyjson_doc_free(imm);

    pc_algo_prims prims;
    if (pc_algo_tree(&spec, canvas_of(s), &prims, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t bt = spec.base_tree_type;
    if (bt < 1) {
        bt = 1;
    }
    if (bt > 34) {
        bt = 34;
    }
    struct name_extra ex = {"treeType", pc_algo_tree_type_names[bt - 1]};
    pc_status rc = draw(s, "Tree\xE2\x80\xA6", p, &prims, out, err, name_add_extra, &ex);
    pc_algo_prims_fini(&prims);
    return rc;
}

static pc_status run_frame(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "frame params");
    }
    yyjson_mut_val *m = params_copy(md, p);
    if (m == NULL) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, "frame params");
    }
    normalize_colour(md, m, "vineColor", p);
    normalize_colour(md, m, "flowerColor", p);
    normalize_colour(md, m, "leafColor", p);
    index_param(md, m, "frame", pc_algo_frame_styles, PC_ALGO_FRAME_STYLES);
    whole_number(md, m, "frame");
    whole_number(md, m, "lines");
    whole_number(md, m, "seed");
    yyjson_mut_doc_set_root(md, m);

    yyjson_doc *imm = yyjson_mut_doc_imut_copy(md, NULL);
    yyjson_mut_doc_free(md);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "frame params");
    }
    pc_algo_frame_spec spec;
    {
        pc_error sub;
        pc_error_clear(&sub);
        if (pc_algo_frame_spec_from_json(yyjson_doc_get_root(imm), &spec, &sub) != PC_OK) {
            yyjson_doc_free(imm);
            return pc_engine_err_other(err, "invalid parameters: %s", sub.msg);
        }
    }
    yyjson_doc_free(imm);

    pc_algo_prims prims;
    if (pc_algo_picture_frame(&spec, canvas_of(s), &prims, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    uint32_t fr = spec.frame;
    if (fr < 1) {
        fr = 1;
    }
    if (fr > PC_ALGO_FRAME_STYLES) {
        fr = PC_ALGO_FRAME_STYLES;
    }
    struct name_extra ex = {"frame", pc_algo_frame_styles[fr - 1]};
    pc_status rc = draw(s, "Picture Frame\xE2\x80\xA6", p, &prims, out, err, name_add_extra, &ex);
    pc_algo_prims_fini(&prims);
    return rc;
}

static const char *const MENU_RENDER[] = {"Filter", "Render"};

static const pc_engine_command SPECS[] = {
    {"filter.render.flame", "Flame\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"flameType\":\"oneFlameAlongPath|multipleFlamesAlongPath|multipleFlamesPathDirections|"
     "multipleFlamesVariousLength|candleLight|multipleFlamesOneDirection\",\"length\":1..1000=150,"
     "\"randomizeLength\":bool,\"width\":1..1000=40,\"angle\":-180..180=0,\"interval\":1..1000=60,"
     "\"adjustIntervalForLoops\":bool=true,\"useCustomColor\":bool,\"color\":color,"
     "\"turbulent\":0..100=25,\"jag\":0..100=25,\"opacity\":0..100=75,\"flameLines\":1..100=20,"
     "\"flameBottomAlignment\":0..100=20,\"flameStyle\":\"normal|violent|flat\",\"flameShape\":"
     "\"parallel|toCenter|spread|oval|pointed\",\"randomizeShapes\":bool,\"seed\":u32=0,\"quality\":"
     "\"draft|low|medium|high|fine\",\"path\":text,\"newLayer\":bool} \xE2\x86\x92 "
     "{layer,newLayer,bounds,primitives,usedPath}",
     pc_engine_has_doc, run_flame, true},
    {"filter.render.pictureFrame", "Picture Frame\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"frame\":\"vineWithFlowers|vineWithLeaves|ivy|roses|daisies|berries|bamboo|waves|zigzag|dots|"
     "rope|scallops|doubleLine|snowflakes|stars|hearts\",\"margin\":0..30=4,\"size\":1..100=50,"
     "\"arrangement\":1..100=50,\"lines\":1..5=1,\"thickness\":1..100=30,\"fade\":0..100=0,"
     "\"vineColor\":color,\"flowerColor\":color,\"leafColor\":color,\"seed\":u32=0,\"newLayer\":bool} "
     "\xE2\x86\x92 {layer,newLayer,bounds,primitives,frame}",
     pc_engine_has_doc, run_frame, true},
    {"filter.render.tree", "Tree\xE2\x80\xA6", MENU_RENDER, 2, NULL,
     "{\"baseTreeType\":1..34=1,\"lightDirection\":1..5=3,\"leavesAmount\":0..100=50,"
     "\"leavesSize\":0..200=100,\"branchesHeight\":50..300=100,\"branchesThickness\":50..200=100,"
     "\"defaultLeaves\":bool=true,\"leavesColor\":color,\"customBranchColor\":bool,\"branchesColor\":"
     "color,\"flatShading\":bool,\"seed\":u32=0,\"x\":0..1=0.5,\"y\":0..1=0.95,\"size\":0.05..2=0.8,"
     "\"newLayer\":bool} \xE2\x86\x92 {layer,newLayer,bounds,primitives,treeType}",
     pc_engine_has_doc, run_tree, true},
};

const pc_engine_command *pc_engine_render_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
