/* Document-path commands (path.*, select.toWorkPath, path.fill, shape-style clipboard). Included
 * by vector_specs.c. */

/* ---- path targeting ----------------------------------------------------------------------- */

static bool is_work(const char *name, size_t len)
{
    if (name == NULL || len == 0) {
        return true;
    }
    if (ieq(name, "work", len)) {
        return true;
    }
    return len == 9 && memcmp(name, "Work Path", 9) == 0;
}

/* The active layer's shape path or linked/unlinked vector-mask path, cloned. False when the
 * active layer has neither. */
static bool layer_path_clone(const pc_document *doc, const pc_engine_doc_state *d, pc_path *out, pc_error *err)
{
    if (!d->has_active_layer) {
        return false;
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return false;
    }
    if (l->content.kind == PC_LAYER_SHAPE) {
        return pc_path_clone(&l->content.u.shape.path, out, err) == PC_OK;
    }
    if (l->has_vector_mask) {
        return pc_path_clone(&l->vector_mask.path, out, err) == PC_OK;
    }
    return false;
}

static pc_status resolve_path(const pc_engine_session *s, const char *name, size_t namelen, pc_path *out,
                              pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (name != NULL && namelen == 5 && memcmp(name, "layer", 5) == 0) {
        if (layer_path_clone(doc, d, out, err)) {
            return PC_OK;
        }
        return pc_engine_err_other(err, "active layer has no shape path or vector mask");
    }
    if (is_work(name, namelen)) {
        if (doc->has_work_path) {
            return pc_path_clone(&doc->work_path, out, err);
        }
        if (layer_path_clone(doc, d, out, err)) {
            return PC_OK;
        }
        return pc_engine_err_other(err, "no work path");
    }
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, name, namelen)) {
            return pc_path_clone(&doc->paths[i].path, out, err);
        }
    }
    return pc_engine_err_other(err, "no path named \"%.*s\"", (int)namelen, name != NULL ? name : "");
}

/* ---- path.list / path.info ---------------------------------------------------------------- */

static size_t path_knots(const pc_path *p)
{
    size_t n = 0;
    for (size_t i = 0; i < p->nsubpaths; i++) {
        n += p->subpaths[i].nknots;
    }
    return n;
}

static pc_status run_path_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *paths = yyjson_mut_arr(rd);
    for (size_t i = 0; i < doc->npaths; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&doc->paths[i].name));
        yyjson_mut_obj_add_uint(rd, o, "knots", path_knots(&doc->paths[i].path));
        yyjson_mut_obj_add_uint(rd, o, "subpaths", doc->paths[i].path.nsubpaths);
        yyjson_mut_arr_add_val(paths, o);
    }
    yyjson_mut_obj_add_val(rd, root, "paths", paths);
    if (doc->has_work_path) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, o, "knots", path_knots(&doc->work_path));
        yyjson_mut_obj_add_uint(rd, o, "subpaths", doc->work_path.nsubpaths);
        yyjson_mut_obj_add_val(rd, root, "workPath", o);
    } else {
        yyjson_mut_obj_add_null(rd, root, "workPath");
    }
    if (doc->has_clipping_path) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, o, "name", pc_str_c(&doc->clipping_path.name));
        yyjson_mut_obj_add_real(rd, o, "flatness", (double)doc->clipping_path.flatness);
        yyjson_mut_obj_add_val(rd, root, "clippingPath", o);
    } else {
        yyjson_mut_obj_add_null(rd, root, "clippingPath");
    }
    yyjson_mut_val *layer = NULL;
    if (d->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        if (l != NULL) {
            char nb[320];
            if (l->content.kind == PC_LAYER_SHAPE) {
                layer = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_str(rd, layer, "kind", "shape");
                yyjson_mut_obj_add_uint(rd, layer, "layer", l->id);
                snprintf(nb, sizeof nb, "%s Shape Path", pc_str_c(&l->name));
                yyjson_mut_obj_add_strcpy(rd, layer, "name", nb);
                yyjson_mut_obj_add_uint(rd, layer, "knots", path_knots(&l->content.u.shape.path));
            } else if (l->has_vector_mask) {
                layer = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_str(rd, layer, "kind", "vectorMask");
                yyjson_mut_obj_add_uint(rd, layer, "layer", l->id);
                snprintf(nb, sizeof nb, "%s Vector Mask", pc_str_c(&l->name));
                yyjson_mut_obj_add_strcpy(rd, layer, "name", nb);
                yyjson_mut_obj_add_uint(rd, layer, "knots", path_knots(&l->vector_mask.path));
            }
        }
    }
    if (layer != NULL) {
        yyjson_mut_obj_add_val(rd, root, "layerPath", layer);
    } else {
        yyjson_mut_obj_add_null(rd, root, "layerPath");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_path_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    pc_path path;
    if (resolve_path(s, name, nlen, &path, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_path_fini(&path);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "name", name != NULL ? name : "work");
    yyjson_mut_obj_add_val(rd, root, "path", path_to_json(rd, &path));
    yyjson_mut_doc_set_root(rd, root);
    pc_path_fini(&path);
    return PC_OK;
}

/* ---- path.set ----------------------------------------------------------------------------- */

typedef struct {
    const pc_path *path;
    bool is_work;
    const char *name;
    size_t namelen;
    bool has_op;
    pc_path_op op;
} pathset_ctx;

static pc_status append_into(pc_path *dst, const pc_path *src, pc_path_op op, pc_error *err)
{
    size_t add = src->nsubpaths;
    if (add == 0) {
        return PC_OK;
    }
    pc_subpath *ns = realloc(dst->subpaths, (dst->nsubpaths + add) * sizeof *ns);
    if (ns == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "set path");
    }
    dst->subpaths = ns;
    for (size_t i = 0; i < add; i++) {
        pc_subpath sp;
        if (pc_subpath_clone(&src->subpaths[i], &sp, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        sp.op = op;
        dst->subpaths[dst->nsubpaths++] = sp;
    }
    return PC_OK;
}

static pc_status edit_path_set(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    pathset_ctx *ctx = vctx;
    if (ctx->is_work) {
        if (ctx->has_op && doc->has_work_path) {
            return append_into(&doc->work_path, ctx->path, ctx->op, err);
        }
        pc_path np;
        if (pc_path_clone(ctx->path, &np, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (doc->has_work_path) {
            pc_path_fini(&doc->work_path);
        }
        doc->work_path = np;
        doc->has_work_path = true;
        return PC_OK;
    }
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, ctx->name, ctx->namelen)) {
            if (ctx->has_op) {
                return append_into(&doc->paths[i].path, ctx->path, ctx->op, err);
            }
            pc_path np;
            if (pc_path_clone(ctx->path, &np, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            pc_path_fini(&doc->paths[i].path);
            doc->paths[i].path = np;
            return PC_OK;
        }
    }
    pc_named_path *ns = realloc(doc->paths, (doc->npaths + 1) * sizeof *ns);
    if (ns == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "set path");
    }
    doc->paths = ns;
    pc_named_path entry = {0};
    if (pc_str_set(&entry.name, ctx->name != NULL ? ctx->name : "", ctx->namelen, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (pc_path_clone(ctx->path, &entry.path, err) != PC_OK) {
        pc_str_fini(&entry.name);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    entry.psd_raw = NULL;
    doc->paths[doc->npaths++] = entry;
    return PC_OK;
}

static pc_status run_path_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.set";
    char emsg[256];
    yyjson_val *pathv = pc_json_get(p, "path");
    if (pathv == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing `path`");
    }
    pc_path path;
    if (!parse_path(pathv, &path, emsg, sizeof emsg)) {
        return pc_engine_err_bad_params(err, CMD, "%s", emsg);
    }
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    bool work = is_work(name, nlen);
    pc_path_op op = PC_PATH_COMBINE;
    bool has_op = false;
    size_t olen = 0;
    const char *os = pc_json_as_str(pc_json_get(p, "op"), &olen);
    if (os != NULL) {
        if (!op_from(os, olen, &op)) {
            pc_path_fini(&path);
            return pc_engine_err_bad_params(err, CMD, "unknown op `%.*s`", (int)olen, os);
        }
        has_op = true;
    }
    pathset_ctx ctx = {&path, work, name, nlen, has_op, op};
    pc_status rc = pc_engine_session_edit(s, work ? "Work Path" : "Save Path", edit_path_set, &ctx, err);
    pc_path_fini(&path);
    if (rc != PC_OK) {
        return rc;
    }
    pc_path result;
    if (resolve_path(s, name, nlen, &result, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_path_fini(&result);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "name", name != NULL ? name : "work");
    yyjson_mut_obj_add_val(rd, root, "path", path_to_json(rd, &result));
    yyjson_mut_doc_set_root(rd, root);
    pc_path_fini(&result);
    return PC_OK;
}

/* ---- path.delete -------------------------------------------------------------------------- */

typedef struct {
    bool is_work;
    const char *name;
    size_t namelen;
} pathdel_ctx;

static pc_status edit_path_delete(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    pathdel_ctx *ctx = vctx;
    if (ctx->is_work) {
        if (!doc->has_work_path) {
            return pc_engine_err_other(err, "no work path");
        }
        pc_path_fini(&doc->work_path);
        memset(&doc->work_path, 0, sizeof doc->work_path);
        doc->has_work_path = false;
        return PC_OK;
    }
    size_t found = SIZE_MAX;
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, ctx->name, ctx->namelen)) {
            found = i;
            break;
        }
    }
    if (found == SIZE_MAX) {
        return pc_engine_err_other(err, "no path named \"%.*s\"", (int)ctx->namelen, ctx->name != NULL ? ctx->name : "");
    }
    if (doc->has_clipping_path && pc_str_eq(&doc->clipping_path.name, &doc->paths[found].name)) {
        pc_clipping_path_fini(&doc->clipping_path);
        memset(&doc->clipping_path, 0, sizeof doc->clipping_path);
        doc->has_clipping_path = false;
    }
    pc_named_path_fini(&doc->paths[found]);
    for (size_t i = found + 1; i < doc->npaths; i++) {
        doc->paths[i - 1] = doc->paths[i];
    }
    doc->npaths--;
    return PC_OK;
}

static pc_status run_path_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    pathdel_ctx ctx = {is_work(name, nlen), name, nlen};
    if (pc_engine_session_edit(s, "Delete Path", edit_path_delete, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- clipping path ------------------------------------------------------------------------ */

typedef struct {
    const char *name;
    size_t namelen;
    float flatness;
} clip_ctx;

static pc_status edit_clip_set(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    clip_ctx *ctx = vctx;
    bool found = false;
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, ctx->name, ctx->namelen)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return pc_engine_err_other(err, "no saved path named \"%.*s\"", (int)ctx->namelen, ctx->name);
    }
    if (doc->has_clipping_path) {
        pc_clipping_path_fini(&doc->clipping_path);
    }
    memset(&doc->clipping_path, 0, sizeof doc->clipping_path);
    if (pc_str_set(&doc->clipping_path.name, ctx->name, ctx->namelen, err) != PC_OK) {
        doc->has_clipping_path = false;
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    doc->clipping_path.flatness = ctx->flatness;
    doc->has_clipping_path = true;
    return PC_OK;
}

static pc_status run_clipping_path_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.clippingPath.set";
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (name == NULL || nlen == 0) {
        return pc_engine_err_bad_params(err, CMD, "missing saved path `name`");
    }
    float flatness = 0.0f;
    yyjson_val *fv = pc_json_get(p, "flatness");
    if (fv != NULL) {
        double f;
        if (!pc_json_as_f64(fv, &f) || !isfinite(f) || f < 0.0 || f > 100.0) {
            return pc_engine_err_bad_params(err, CMD, "`flatness` must be between 0 and 100");
        }
        flatness = (float)f;
    }
    clip_ctx ctx = {name, nlen, flatness};
    if (pc_engine_session_edit(s, "Clipping Path", edit_clip_set, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strncpy(rd, root, "name", name, nlen);
    yyjson_mut_obj_add_real(rd, root, "flatness", (double)flatness);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status edit_clip_clear(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)vctx;
    if (!doc->has_clipping_path) {
        return pc_engine_err_other(err, "no clipping path");
    }
    pc_clipping_path_fini(&doc->clipping_path);
    memset(&doc->clipping_path, 0, sizeof doc->clipping_path);
    doc->has_clipping_path = false;
    return PC_OK;
}

static pc_status run_clipping_path_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (pc_engine_session_edit(s, "Clear Clipping Path", edit_clip_clear, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- path.rename -------------------------------------------------------------------------- */

typedef struct {
    bool from_work;
    const char *name;
    size_t namelen;
    const char *to;
    size_t tolen;
} rename_ctx;

static pc_status edit_path_rename(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    rename_ctx *ctx = vctx;
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, ctx->to, ctx->tolen)) {
            return pc_engine_err_other(err, "a path named \"%.*s\" already exists", (int)ctx->tolen, ctx->to);
        }
    }
    if (ctx->from_work) {
        if (!doc->has_work_path) {
            return pc_engine_err_other(err, "no work path");
        }
        pc_named_path *ns = realloc(doc->paths, (doc->npaths + 1) * sizeof *ns);
        if (ns == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "rename path");
        }
        doc->paths = ns;
        pc_named_path entry = {0};
        if (pc_str_set(&entry.name, ctx->to, ctx->tolen, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        entry.path = doc->work_path;
        entry.psd_raw = NULL;
        memset(&doc->work_path, 0, sizeof doc->work_path);
        doc->has_work_path = false;
        doc->paths[doc->npaths++] = entry;
        return PC_OK;
    }
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, ctx->name, ctx->namelen)) {
            bool was_clip = doc->has_clipping_path && pc_str_eq(&doc->clipping_path.name, &doc->paths[i].name);
            if (pc_str_set(&doc->paths[i].name, ctx->to, ctx->tolen, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            if (was_clip) {
                if (pc_str_set(&doc->clipping_path.name, ctx->to, ctx->tolen, err) != PC_OK) {
                    return err != NULL ? err->code : PC_ERR_NOMEM;
                }
            }
            return PC_OK;
        }
    }
    return pc_engine_err_other(err, "no path named \"%.*s\"", (int)ctx->namelen, ctx->name != NULL ? ctx->name : "");
}

static pc_status run_path_rename(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.rename";
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    size_t tolen = 0;
    const char *to = pc_json_as_str(pc_json_get(p, "to"), &tolen);
    if (to == NULL || tolen == 0) {
        return pc_engine_err_bad_params(err, CMD, "missing `to`");
    }
    if (is_work(to, tolen)) {
        return pc_engine_err_bad_params(err, CMD, "`to` cannot name the work path");
    }
    bool from_work = is_work(name, nlen);
    rename_ctx ctx = {from_work, name, nlen, to, tolen};
    if (pc_engine_session_edit(s, from_work ? "Save Path" : "Rename Path", edit_path_rename, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strncpy(rd, root, "name", to, tolen);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- path.toSelection / select.toWorkPath ------------------------------------------------- */

typedef struct {
    const float *mask;
    size_t mlen;
    pc_rect area;
    pc_algo_selection_mode mode;
    bool some;
} tosel_ctx;

static pc_status edit_to_selection(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    tosel_ctx *ctx = vctx;
    pc_surface outsel;
    bool some = false;
    if (pc_algo_selection_combine(doc->has_selection ? &doc->selection : NULL, ctx->mask, ctx->mlen, ctx->area,
                                  ctx->mode, &outsel, &some, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
    }
    if (some) {
        doc->selection = outsel;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&outsel);
        memset(&doc->selection, 0, sizeof doc->selection);
        doc->has_selection = false;
    }
    ctx->some = some;
    return PC_OK;
}

static pc_status run_path_to_selection(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    pc_path path;
    if (resolve_path(s, name, nlen, &path, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    double feather = 0.0;
    (void)get_f64(p, "feather", &feather);
    bool aa = pbool(p, "antiAlias", true);
    size_t mlen_m = 0;
    const char *modestr = pc_json_as_str(pc_json_get(p, "mode"), &mlen_m);
    char mbuf[16] = "replace";
    if (modestr != NULL && mlen_m < sizeof mbuf) {
        memcpy(mbuf, modestr, mlen_m);
        mbuf[mlen_m] = '\0';
    }
    pc_algo_selection_mode mode = pc_algo_selection_mode_parse(mbuf);

    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        pc_path_fini(&path);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_rect area = pc_document_bounds(doc);
    pc_vec_rasterizer r;
    if (pc_vec_fill_rasterizer(&path, PC_VEC_DEFAULT_TOLERANCE, &r, err) != PC_OK) {
        pc_path_fini(&path);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_path_fini(&path);
    float *mask = NULL;
    size_t mlen = 0;
    if (pc_vec_rasterizer_render(&r, area, &mask, &mlen, err) != PC_OK) {
        pc_vec_rasterizer_fini(&r);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_vec_rasterizer_fini(&r);
    if (!aa) {
        for (size_t i = 0; i < mlen; i++) {
            mask[i] = mask[i] >= 0.5f ? 1.0f : 0.0f;
        }
    }
    if (feather > 0.0) {
        float *fm = NULL;
        if (pc_algo_selection_feather(mask, mlen, pc_rect_width(area), pc_rect_height(area), (float)feather, &fm,
                                      err) != PC_OK) {
            free(mask);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        free(mask);
        mask = fm;
    }
    tosel_ctx ctx = {mask, mlen, area, mode, false};
    pc_status rc = pc_engine_session_edit(s, "Make Selection", edit_to_selection, &ctx, err);
    free(mask);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "selected", ctx.some);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

typedef struct {
    pc_path *path; /* moved into the work path on success */
} workpath_ctx;

static pc_status edit_set_work_path(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    workpath_ctx *ctx = vctx;
    pc_path np;
    if (pc_path_clone(ctx->path, &np, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (doc->has_work_path) {
        pc_path_fini(&doc->work_path);
    }
    doc->work_path = np;
    doc->has_work_path = true;
    return PC_OK;
}

static pc_status run_select_to_work_path(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double tol = 2.0;
    (void)get_f64(p, "tolerance", &tol);
    tol = tol < 0.5 ? 0.5 : tol > 10.0 ? 10.0 : tol;
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!doc->has_selection) {
        return pc_engine_err_other(err, "no selection");
    }
    pc_rect area = pc_document_bounds(doc);
    float *mask = NULL;
    size_t mlen = 0;
    if (pc_algo_selection_mask_from_surface(&doc->selection, area, &mask, &mlen, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_path path;
    pc_status rc = pc_vec_trace_mask(mask, mlen, area, 0.0f, tol, &path, err);
    free(mask);
    if (rc != PC_OK) {
        return rc;
    }
    size_t knots = path_knots(&path);
    size_t subpaths = path.nsubpaths;
    workpath_ctx ctx = {&path};
    rc = pc_engine_session_edit(s, "Make Work Path", edit_set_work_path, &ctx, err);
    pc_path_fini(&path);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "subpaths", subpaths);
    yyjson_mut_obj_add_uint(rd, root, "knots", knots);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- path.fill ---------------------------------------------------------------------------- */

static void paint_coverage(pc_surface *surf, pc_rect area, const float *cov, size_t covlen, const float src[4],
                           float opacity, pc_blend_mode mode, bool lock_alpha)
{
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t ch = pc_pixel_format_channels(fmt);
    float *vals = NULL;
    size_t len = 0;
    if (pc_surface_read_region(surf, area, &vals, &len, NULL) != PC_OK) {
        return;
    }
    size_t npx = ch > 0 ? len / ch : 0;
    for (size_t i = 0; i < npx && i < covlen; i++) {
        float c = cov[i];
        if (c <= 0.0f) {
            continue;
        }
        float *px = vals + i * ch;
        float dst[4];
        pc_to_rgba(fmt, px, ch, dst);
        float s4[4] = {src[0], src[1], src[2], src[3] * c};
        float o4[4];
        pc_psblend_composite(mode, dst, s4, opacity, o4);
        if (lock_alpha) {
            o4[3] = dst[3];
        }
        pc_from_rgba(fmt, o4, px);
    }
    (void)pc_surface_write_region(surf, area, vals, len, NULL);
    free(vals);
    pc_surface_prune(surf);
}

typedef struct {
    pc_layer_id id;
    const pc_path *path;
    float src[4];
    float opacity;
    pc_blend_mode mode;
    float feather;
    bool aa;
} fill_ctx;

static pc_status edit_path_fill(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    fill_ctx *ctx = vctx;
    pc_rect area = pc_document_bounds(doc);
    pc_vec_rasterizer r;
    if (pc_vec_fill_rasterizer(ctx->path, PC_VEC_DEFAULT_TOLERANCE, &r, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_rect pb;
    if (pc_vec_rasterizer_pixel_bounds(&r, &pb)) {
        int32_t pad = (int32_t)ceilf(ctx->feather);
        if (pad > INT32_MAX / 3) {
            pad = INT32_MAX / 3;
        }
        pad = pad * 3 + 1;
        area = pc_rect_intersect(pc_rect_inflate(pb, pad), area);
    }
    if (pc_rect_is_empty(area)) {
        pc_vec_rasterizer_fini(&r);
        return PC_OK;
    }
    float *cov = NULL;
    size_t clen = 0;
    if (pc_vec_rasterizer_render(&r, area, &cov, &clen, err) != PC_OK) {
        pc_vec_rasterizer_fini(&r);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_vec_rasterizer_fini(&r);
    if (!ctx->aa) {
        for (size_t i = 0; i < clen; i++) {
            cov[i] = cov[i] >= 0.5f ? 1.0f : 0.0f;
        }
    }
    if (ctx->feather > 0.0f) {
        float *fm = NULL;
        if (pc_algo_selection_feather(cov, clen, pc_rect_width(area), pc_rect_height(area), ctx->feather, &fm,
                                      err) != PC_OK) {
            free(cov);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        free(cov);
        cov = fm;
    }
    if (doc->has_selection) {
        float *sm = NULL;
        size_t slen = 0;
        if (pc_algo_selection_mask_from_surface(&doc->selection, area, &sm, &slen, err) == PC_OK) {
            for (size_t i = 0; i < clen && i < slen; i++) {
                cov[i] *= sm[i];
            }
            free(sm);
        }
    }
    pc_locks locks = pc_document_effective_locks(doc, ctx->id);
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        free(cov);
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (locks.pixels || locks.all) {
        free(cov);
        return pc_engine_err_other(err, "Could not complete your request because the layer \"%s\" is locked",
                                   pc_str_c(&l->name));
    }
    bool lock = locks.transparency;
    pc_surface *surf = pc_layer_surface_mut(l);
    if (surf == NULL) {
        free(cov);
        return pc_engine_err_other(err, "Fill Path needs a pixel layer");
    }
    paint_coverage(surf, area, cov, clen, ctx->src, ctx->opacity, ctx->mode, lock);
    free(cov);
    return PC_OK;
}

static pc_status run_path_fill(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    pc_path path;
    if (resolve_path(s, name, nlen, &path, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const float *fg = s->tools.foreground;
    float src[4] = {fg[0], fg[1], fg[2], fg[3]};
    pc_color c;
    if (vcolor(pc_json_get(p, "color"), &c)) {
        float rgb[3];
        pc_color_to_rgb(&c, rgb);
        src[0] = rgb[0];
        src[1] = rgb[1];
        src[2] = rgb[2];
        src[3] = c.alpha;
    }
    double opacity = 100.0;
    (void)get_f64(p, "opacity", &opacity);
    float op = (float)pc_clampd(opacity / 100.0, 0.0, 1.0);
    pc_blend_mode mode = PC_BLEND_NORMAL;
    size_t mlen = 0;
    const char *ms = pc_json_as_str(pc_json_get(p, "mode"), &mlen);
    if (ms != NULL) {
        (void)pc_engine_blend_from_str(ms, mlen, &mode);
    }
    double feather = 0.0;
    (void)get_f64(p, "feather", &feather);
    bool aa = pbool(p, "antiAlias", true);
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        pc_path_fini(&path);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    fill_ctx ctx = {id, &path, {src[0], src[1], src[2], src[3]}, op, mode, (float)feather, aa};
    pc_status rc = pc_engine_session_edit(s, "Fill Path", edit_path_fill, &ctx, err);
    pc_path_fini(&path);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- shape-style clipboard ---------------------------------------------------------------- */

static pc_status run_copy_fill(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_shape_layer *sh = active_shape(s, err);
    if (sh == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!sh->has_fill) {
        return pc_engine_err_other(err, "shape has no fill");
    }
    pc_fill nf;
    if (pc_fill_clone(&sh->fill, &nf, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (s->has_path_fill_clip) {
        pc_fill_fini(&s->path_fill_clip);
    }
    s->path_fill_clip = nf;
    s->has_path_fill_clip = true;
    return pc_engine_result_null(out, err);
}

static pc_status run_copy_stroke(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_shape_layer *sh = active_shape(s, err);
    if (sh == NULL) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!sh->has_stroke) {
        return pc_engine_err_other(err, "shape has no stroke");
    }
    pc_shape_stroke ns;
    if (pc_shape_stroke_clone(&sh->stroke, &ns, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (s->has_path_stroke_clip) {
        pc_shape_stroke_fini(&s->path_stroke_clip);
    }
    s->path_stroke_clip = ns;
    s->has_path_stroke_clip = true;
    return pc_engine_result_null(out, err);
}

static pc_status paste_fill_fn(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    (void)l;
    pc_fill *src = ud;
    pc_fill nf;
    if (pc_fill_clone(src, &nf, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (sh->has_fill) {
        pc_fill_fini(&sh->fill);
    }
    sh->fill = nf;
    sh->has_fill = true;
    return PC_OK;
}

static pc_status paste_stroke_fn(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    (void)l;
    pc_shape_stroke *src = ud;
    pc_shape_stroke ns;
    if (pc_shape_stroke_clone(src, &ns, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (sh->has_stroke) {
        pc_shape_stroke_fini(&sh->stroke);
    }
    sh->stroke = ns;
    sh->has_stroke = true;
    return PC_OK;
}

static pc_status run_paste_fill(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (!s->has_path_fill_clip) {
        return pc_engine_err_other(err, "no shape fill copied");
    }
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (with_shape(s, id, "Paste Fill", paste_fill_fn, &s->path_fill_clip, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return shape_info(s, id, out, err);
}

static pc_status run_paste_stroke(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (!s->has_path_stroke_clip) {
        return pc_engine_err_other(err, "no shape stroke copied");
    }
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (with_shape(s, id, "Paste Complete Stroke", paste_stroke_fn, &s->path_stroke_clip, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return shape_info(s, id, out, err);
}

/* ---- path.transform ----------------------------------------------------------------------- */

#include "vector_specs_transform.inc"
