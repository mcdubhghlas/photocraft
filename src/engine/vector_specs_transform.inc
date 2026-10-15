/* path.transform (Free Transform / Distort / Perspective / Warp of a path). Included by
 * vector_specs_path.inc. */

/* n finite numbers (|.| <= 1e6) at key, flattening [[x,y]...]. 0 absent, 1 ok, -1 error. */
static int coords(yyjson_val *p, const char *key, size_t n, double *out, char *emsg, size_t ecap)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return 0;
    }
    size_t flatcount = 0, valid = 0;
    if (yyjson_is_arr(v)) {
        size_t m = yyjson_arr_size(v);
        bool all_arr = m > 0;
        for (size_t i = 0; i < m; i++) {
            if (!yyjson_is_arr(yyjson_arr_get(v, i))) {
                all_arr = false;
                break;
            }
        }
        if (all_arr) {
            for (size_t i = 0; i < m; i++) {
                yyjson_val *c = yyjson_arr_get(v, i);
                size_t cm = yyjson_arr_size(c);
                for (size_t j = 0; j < cm; j++) {
                    flatcount++;
                    double x;
                    if (pc_json_as_f64(yyjson_arr_get(c, j), &x) && isfinite(x) && fabs(x) <= 1000000.0) {
                        if (valid < n) {
                            out[valid] = x;
                        }
                        valid++;
                    }
                }
            }
        } else {
            for (size_t i = 0; i < m; i++) {
                flatcount++;
                double x;
                if (pc_json_as_f64(yyjson_arr_get(v, i), &x) && isfinite(x) && fabs(x) <= 1000000.0) {
                    if (valid < n) {
                        out[valid] = x;
                    }
                    valid++;
                }
            }
        }
    }
    if (flatcount != n || valid != n) {
        snprintf(emsg, ecap, "`%s` must hold %zu finite numbers with magnitude at most 1000000", key, n);
        return -1;
    }
    return 1;
}

/* A numeric param (finite, |.| <= 1e6) with a default when absent. */
static bool pt_num_t(yyjson_val *p, const char *k, double def, double *out, char *emsg, size_t ecap)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL) {
        *out = def;
        return true;
    }
    double x;
    if (!pc_json_as_f64(v, &x) || !isfinite(x) || fabs(x) > 1000000.0) {
        snprintf(emsg, ecap, "`%s` must be a finite number with magnitude at most 1000000", k);
        return false;
    }
    *out = x;
    return true;
}

static pc_status transform_target(const pc_engine_session *s, const char *name, size_t namelen, bool has_layer,
                                  pc_layer_id id, pc_path *out, pc_error *err)
{
    if (!has_layer) {
        return resolve_path(s, name, namelen, out, err);
    }
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind == PC_LAYER_SHAPE) {
        return pc_path_clone(&l->content.u.shape.path, out, err);
    }
    if (l->has_vector_mask) {
        return pc_path_clone(&l->vector_mask.path, out, err);
    }
    return pc_engine_err_other(err, "target layer has no shape path or vector mask");
}

/* The box frame: `rect` else the target path's control bounds, {x0,y0,x1,y1}. */
static pc_status transform_frame(const pc_engine_session *s, yyjson_val *p, const char *name, size_t namelen,
                                 bool has_layer, pc_layer_id id, double out[4], pc_error *err)
{
    char emsg[256];
    int r = coords(p, "rect", 4, out, emsg, sizeof emsg);
    if (r < 0) {
        return pc_engine_err_bad_params(err, "path.transform", "%s", emsg);
    }
    if (r == 0) {
        pc_path path;
        if (transform_target(s, name, namelen, has_layer, id, &path, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        bool ok = pc_path_control_bounds(&path, out);
        pc_path_fini(&path);
        if (!ok) {
            return pc_engine_err_bad_params(err, "path.transform", "path has no points to transform");
        }
    }
    if (out[2] - out[0] <= 0.0 || out[3] - out[1] <= 0.0) {
        return pc_engine_err_bad_params(err, "path.transform",
                                        "`rect` must have a positive width and height (a straight path needs a `rect`)");
    }
    return PC_OK;
}

enum map_kind { MAP_AFFINE, MAP_PROJECTIVE, MAP_WARP };

typedef struct {
    enum map_kind kind;
    pc_affine a;
    pc_algo_homography h;
    pc_warp w; /* valid for MAP_WARP; owned */
} path_map;

typedef struct {
    pc_layer_id id;
    path_map *map;
} tf_shape_ctx;

static pc_status tf_apply_shape(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    (void)l;
    path_map *m = ((tf_shape_ctx *)ud)->map;
    pc_status rc = PC_OK;
    if (m->kind == MAP_AFFINE) {
        pc_engine_transform_shape(sh, &m->a);
    } else if (m->kind == MAP_PROJECTIVE) {
        rc = pc_engine_map_shape(sh, &m->h, NULL, err);
    } else {
        rc = pc_engine_map_shape(sh, NULL, &m->w, err);
    }
    if (rc == PC_OK) {
        pc_blob_release(sh->psd_raw);
        sh->psd_raw = NULL;
    }
    return rc;
}

typedef struct {
    bool is_work;
    const char *name;
    size_t namelen;
    bool has_layer;
    pc_layer_id id;
    path_map *map;
} tf_path_ctx;

static pc_status tf_apply_path(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    tf_path_ctx *ctx = vctx;
    pc_path *path = NULL;
    if (ctx->has_layer) {
        pc_layer *l = pc_document_layer_mut(doc, ctx->id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, ctx->id);
        }
        if (!l->has_vector_mask) {
            return pc_engine_err_other(err, "active layer has no shape path or vector mask");
        }
        path = &l->vector_mask.path;
    } else if (ctx->is_work) {
        if (!doc->has_work_path) {
            return pc_engine_err_other(err, "no work path");
        }
        path = &doc->work_path;
    } else {
        for (size_t i = 0; i < doc->npaths; i++) {
            if (pc_str_eq_mem(&doc->paths[i].name, ctx->name, ctx->namelen)) {
                path = &doc->paths[i].path;
                break;
            }
        }
        if (path == NULL) {
            return pc_engine_err_other(err, "no path named \"%.*s\"", (int)ctx->namelen,
                                       ctx->name != NULL ? ctx->name : "");
        }
    }
    path_map *m = ctx->map;
    pc_path np;
    pc_status rc;
    if (m->kind == MAP_AFFINE) {
        rc = pc_path_transform(path, m->a, &np, err);
    } else if (m->kind == MAP_PROJECTIVE) {
        rc = pc_engine_map_path(path, &m->h, NULL, &np, err);
    } else {
        rc = pc_engine_map_path(path, NULL, &m->w, &np, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_path_fini(path);
    *path = np;
    return PC_OK;
}

static pc_status run_path_transform(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.transform";
    char emsg[256];
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (name == NULL) {
        name = "work";
        nlen = 4;
    }
    bool is_layer = nlen == 5 && memcmp(name, "layer", 5) == 0;
    bool has_layer = false;
    pc_layer_id lid = 0;
    if (is_layer) {
        if (layer_id_of(s, p, &lid, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        has_layer = true;
    }

    path_map map = {MAP_AFFINE, PC_AFFINE_IDENTITY, PC_ALGO_HOMOGRAPHY_IDENTITY, {0}};
    bool map_has_warp = false;

    yyjson_val *matrix = pc_json_get(p, "matrix");
    double q[8];
    int qr = coords(p, "quad", 8, q, emsg, sizeof emsg);
    if (qr < 0) {
        return pc_engine_err_bad_params(err, CMD, "%s", emsg);
    }
    bool has_warp_keys = pc_json_get(p, "warp") != NULL || pc_json_get(p, "style") != NULL ||
                         pc_json_get(p, "mesh") != NULL;

    if (matrix != NULL) {
        if (!yyjson_is_arr(matrix) || yyjson_arr_size(matrix) != 6) {
            return pc_engine_err_bad_params(err, CMD, "`matrix` must contain six numbers");
        }
        double m[6];
        for (size_t i = 0; i < 6; i++) {
            double x;
            if (!pc_json_as_f64(yyjson_arr_get(matrix, i), &x) || !isfinite(x) || fabs(x) > 1000000.0) {
                return pc_engine_err_bad_params(err, CMD,
                                                "matrix entries must be finite numbers with magnitude at most 1000000");
            }
            m[i] = x;
        }
        map.kind = MAP_AFFINE;
        map.a = (pc_affine){{m[0], m[1], m[2], m[3], m[4], m[5]}};
    } else if (qr == 1) {
        double frame[4];
        if (transform_frame(s, p, name, nlen, has_layer, lid, frame, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        double quad[4][2] = {{q[0], q[1]}, {q[2], q[3]}, {q[4], q[5]}, {q[6], q[7]}};
        pc_algo_homography h;
        pc_algo_homography inv;
        bool ok = pc_algo_homography_rect_to_quad(frame, (const double(*)[2])quad, &h);
        if (ok) {
            for (size_t i = 0; i < 9; i++) {
                if (!isfinite(h.m[i])) {
                    ok = false;
                }
            }
        }
        if (ok) {
            ok = pc_algo_homography_inverse(&h, &inv);
        }
        if (!ok) {
            return pc_engine_err_bad_params(err, CMD, "degenerate quad");
        }
        double i8 = h.m[8];
        if (fabs(h.m[6]) < 1e-12 && fabs(h.m[7]) < 1e-12 && fabs(i8) > 1e-12) {
            map.kind = MAP_AFFINE;
            map.a = (pc_affine){{h.m[0] / i8, h.m[3] / i8, h.m[1] / i8, h.m[4] / i8, h.m[2] / i8, h.m[5] / i8}};
        } else {
            map.kind = MAP_PROJECTIVE;
            map.h = h;
        }
    } else if (has_warp_keys) {
        double frame[4];
        if (transform_frame(s, p, name, nlen, has_layer, lid, frame, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        pc_warp w;
        if (pc_engine_warp_from_params(CMD, p, frame, &w, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        double vals[7] = {w.bend, w.h_distort, w.v_distort, w.bounds[0], w.bounds[1], w.bounds[2], w.bounds[3]};
        for (size_t i = 0; i < 7; i++) {
            if (!isfinite(vals[i]) || fabs(vals[i]) > 1000000.0) {
                pc_warp_fini(&w);
                return pc_engine_err_bad_params(err, CMD,
                                                "warp values must be finite numbers with magnitude at most 1000000");
            }
        }
        map.kind = MAP_WARP;
        map.w = w;
        map_has_warp = true;
    } else {
        if (pc_json_get(p, "translateX") == NULL && pc_json_get(p, "translateY") == NULL &&
            pc_json_get(p, "scaleX") == NULL && pc_json_get(p, "scaleY") == NULL && pc_json_get(p, "angle") == NULL) {
            return pc_engine_err_bad_params(
                err, CMD, "pass `matrix`, `quad`, a warp (`style`, `mesh` or `warp`) or translate/scale/angle "
                          "parameters");
        }
        double tx, ty, sx, sy, angle;
        if (!pt_num_t(p, "translateX", 0.0, &tx, emsg, sizeof emsg) ||
            !pt_num_t(p, "translateY", 0.0, &ty, emsg, sizeof emsg) ||
            !pt_num_t(p, "scaleX", 1.0, &sx, emsg, sizeof emsg) ||
            !pt_num_t(p, "scaleY", 1.0, &sy, emsg, sizeof emsg) ||
            !pt_num_t(p, "angle", 0.0, &angle, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        pc_path path;
        if (transform_target(s, name, nlen, has_layer, lid, &path, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        double b[4];
        bool ok = pc_path_control_bounds(&path, b);
        pc_path_fini(&path);
        if (!ok) {
            return pc_engine_err_bad_params(err, CMD, "path has no points to transform");
        }
        double cx = (b[0] + b[2]) * 0.5, cy = (b[1] + b[3]) * 0.5;
        pc_affine t = pc_affine_translate(-cx, -cy);
        pc_affine sc = {{sx, 0.0, 0.0, sy, 0.0, 0.0}};
        t = pc_affine_then(t, sc);
        t = pc_affine_then(t, pc_affine_rotate(angle * 3.14159265358979323846 / 180.0));
        t = pc_affine_then(t, pc_affine_translate(cx + tx, cy + ty));
        map.kind = MAP_AFFINE;
        map.a = t;
    }

    /* Build the result (reports the applied map). */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        if (map_has_warp) {
            pc_warp_fini(&map.w);
        }
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strncpy(rd, root, "name", name, nlen);
    if (map.kind == MAP_AFFINE) {
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        for (size_t i = 0; i < 6; i++) {
            yyjson_mut_arr_add_real(rd, arr, map.a.m[i]);
        }
        yyjson_mut_obj_add_val(rd, root, "matrix", arr);
    } else if (map.kind == MAP_PROJECTIVE) {
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        for (size_t i = 0; i < 9; i++) {
            yyjson_mut_arr_add_real(rd, arr, map.h.m[i]);
        }
        yyjson_mut_obj_add_val(rd, root, "homography", arr);
    } else {
        yyjson_mut_obj_add_bool(rd, root, "warp", true);
    }
    yyjson_mut_doc_set_root(rd, root);

    /* Apply. A shape layer target keeps its shape; everything else edits a path in place. */
    pc_status rc;
    bool shape_target = false;
    if (has_layer) {
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        const pc_layer *l = pc_document_layer(doc, lid);
        shape_target = l != NULL && l->content.kind == PC_LAYER_SHAPE;
    }
    if (shape_target) {
        tf_shape_ctx ctx = {lid, &map};
        rc = with_shape(s, lid, "Transform Path", tf_apply_shape, &ctx, err);
    } else {
        tf_path_ctx ctx = {is_work(name, nlen), name, nlen, has_layer, lid, &map};
        rc = pc_engine_session_edit(s, "Transform Path", tf_apply_path, &ctx, err);
    }
    if (map_has_warp) {
        pc_warp_fini(&map.w);
    }
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    return PC_OK;
}
