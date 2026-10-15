/* Point-level path edits (port of crates/engine/src/path_edit_cmds.rs, E11): Direct Selection,
 * Convert Point and Add/Delete Anchor on the work path, a saved path, a shape layer's path or a
 * vector mask. Included by vector_specs.c (reuses its static helpers: val_point, nums_n,
 * path_to_json, with_shape, layer_id_of). Registered via pc_engine_path_edit_specs. */

#define PE_MAX 1000000.0

static bool pe_finite2(const double d[2])
{
    return isfinite(d[0]) && isfinite(d[1]) && fabs(d[0]) <= PE_MAX && fabs(d[1]) <= PE_MAX;
}

typedef struct {
    int op; /* 0 moveAnchors, 1 moveHandle, 2 bend, 3 convert, 4 add, 5 delete */
    pc_vec_knot_ref *anchors;
    size_t nanchors;
    double d[2];
    pc_vec_knot_ref knot;
    pc_vec_handle handle;
    pc_point to;
    bool independent;
    double t;
    bool has_out;
    pc_point out;
    pc_vec_knot_ref added;
    bool has_added;
    bool removed;
    const char *cmd;
    /* target */
    const char *name;
    size_t namelen;
    bool is_work;
    bool has_layer;
    pc_layer_id layer;
} pe_ctx;

static pc_status pe_apply(pc_path *path, pe_ctx *c, pc_error *err)
{
    switch (c->op) {
    case 0:
        return pc_vec_move_anchors(path, c->anchors, c->nanchors, c->d, err);
    case 1:
        return pc_vec_move_handle(path, c->knot, c->handle, c->to, c->independent, err);
    case 2:
        return pc_vec_bend_segment(path, c->knot, c->t, c->d, err);
    case 3:
        return pc_vec_convert_point(path, c->knot, c->has_out ? &c->out : NULL, err);
    case 4: {
        pc_status rc = pc_vec_add_anchor(path, c->knot, c->t, &c->added, err);
        if (rc == PC_OK) {
            c->has_added = true;
        }
        return rc;
    }
    case 5:
        return pc_vec_delete_anchor(path, c->knot, &c->removed, err);
    default:
        return PC_ERR_INVALID;
    }
}

static pc_status pe_shape_fn(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    (void)l;
    pe_ctx *c = ud;
    pc_status rc = pe_apply(&sh->path, c, err);
    if (rc != PC_OK) {
        return rc;
    }
    sh->has_live = false;
    pc_blob_release(sh->psd_raw);
    sh->psd_raw = NULL;
    return PC_OK;
}

static pc_status pe_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    pe_ctx *c = vctx;
    pc_path *path = NULL;
    if (c->has_layer) {
        pc_layer *l = pc_document_layer_mut(doc, c->layer);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->layer);
        }
        if (!l->has_vector_mask) {
            return pc_engine_err_other(err, "active layer has no shape path or vector mask");
        }
        path = &l->vector_mask.path;
    } else if (c->is_work) {
        if (!doc->has_work_path) {
            return pc_engine_err_other(err, "no work path");
        }
        path = &doc->work_path;
    } else {
        for (size_t i = 0; i < doc->npaths; i++) {
            if (pc_str_eq_mem(&doc->paths[i].name, c->name, c->namelen)) {
                path = &doc->paths[i].path;
                break;
            }
        }
        if (path == NULL) {
            return pc_engine_err_other(err, "no path named \"%.*s\"", (int)c->namelen,
                                       c->name != NULL ? c->name : "");
        }
    }
    return pe_apply(path, c, err);
}

/* The resulting path to serialise (read-only, from the committed document). */
static const pc_path *pe_read_path(const pc_document *doc, const pe_ctx *c)
{
    if (c->has_layer) {
        const pc_layer *l = pc_document_layer(doc, c->layer);
        if (l == NULL) {
            return NULL;
        }
        if (l->content.kind == PC_LAYER_SHAPE) {
            return &l->content.u.shape.path;
        }
        return l->has_vector_mask ? &l->vector_mask.path : NULL;
    }
    if (c->is_work) {
        return doc->has_work_path ? &doc->work_path : NULL;
    }
    for (size_t i = 0; i < doc->npaths; i++) {
        if (pc_str_eq_mem(&doc->paths[i].name, c->name, c->namelen)) {
            return &doc->paths[i].path;
        }
    }
    return NULL;
}

static pc_status pe_run(pc_engine_session *s, yyjson_val *p, const char *label, pe_ctx *c, pc_json_value *out,
                        pc_error *err)
{
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (name == NULL) {
        name = "work";
        nlen = 4;
    }
    c->name = name;
    c->namelen = nlen;
    c->is_work = nlen == 4 && memcmp(name, "work", 4) == 0;
    c->has_layer = false;
    bool want_layer = nlen == 5 && memcmp(name, "layer", 5) == 0;
    bool is_shape = false;
    if (want_layer) {
        pc_layer_id id;
        if (layer_id_of(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        const pc_engine_doc_state *st = pc_engine_session_active(s);
        const pc_layer *l = st != NULL ? pc_document_layer(pc_doc_arc_get(st->doc), id) : NULL;
        is_shape = l != NULL && l->content.kind == PC_LAYER_SHAPE;
        c->has_layer = !is_shape; /* shape path edited through with_shape */
        c->layer = id;
    }
    pc_status rc;
    if (is_shape) {
        pc_layer_id id = c->layer;
        c->has_layer = false;
        rc = with_shape(s, id, label, pe_shape_fn, c, err);
        c->layer = id;
        c->has_layer = false;
    } else {
        rc = pc_engine_session_edit(s, label, pe_edit, c, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    /* for the result read-back, a shape path lives on the layer */
    if (is_shape) {
        c->has_layer = true;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_path *path = pe_read_path(doc, c);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strncpy(rd, root, "name", name, nlen);
    if (path != NULL) {
        yyjson_mut_obj_add_val(rd, root, "path", path_to_json(rd, path));
    } else {
        yyjson_mut_obj_add_null(rd, root, "path");
    }
    if (c->op == 4 && c->has_added) {
        yyjson_mut_val *an = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, an, "subpath", c->added.sub);
        yyjson_mut_obj_add_uint(rd, an, "knot", c->added.knot);
        yyjson_mut_obj_add_val(rd, root, "anchor", an);
    }
    if (c->op == 5) {
        yyjson_mut_obj_add_bool(rd, root, "subpathRemoved", c->removed);
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static bool pe_index(yyjson_val *p, const char *key, size_t *out)
{
    uint64_t v;
    if (!pc_json_as_u64(pc_json_get(p, key), &v)) {
        return false;
    }
    *out = (size_t)v;
    return true;
}

static pc_status pe_knot(yyjson_val *p, const char *cmd, pc_vec_knot_ref *out, pc_error *err)
{
    if (!pe_index(p, "subpath", &out->sub)) {
        return pc_engine_err_bad_params(err, cmd, "missing `subpath` index");
    }
    if (!pe_index(p, "knot", &out->knot)) {
        return pc_engine_err_bad_params(err, cmd, "missing `knot` index");
    }
    return PC_OK;
}

static pc_status run_move_anchors(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.moveAnchors";
    yyjson_val *arr = pc_json_get(p, "anchors");
    if (arr == NULL || !yyjson_is_arr(arr) || yyjson_arr_size(arr) == 0) {
        return pc_engine_err_bad_params(err, CMD, "`anchors` must list [subpath, knot] pairs");
    }
    size_t n = yyjson_arr_size(arr);
    pc_vec_knot_ref *anchors = malloc(n * sizeof *anchors);
    if (anchors == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    size_t idx, mx;
    yyjson_val *v;
    size_t w = 0;
    yyjson_arr_foreach(arr, idx, mx, v) {
        uint64_t sb, kn;
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) < 2 || !pc_json_as_u64(yyjson_arr_get(v, 0), &sb) ||
            !pc_json_as_u64(yyjson_arr_get(v, 1), &kn)) {
            free(anchors);
            return pc_engine_err_bad_params(err, CMD, "each anchor is [subpath, knot]");
        }
        anchors[w].sub = (size_t)sb;
        anchors[w].knot = (size_t)kn;
        w++;
    }
    pe_ctx c = {0};
    c.op = 0;
    c.anchors = anchors;
    c.nanchors = n;
    if (!nums_n(p, "move", 2, c.d) || !pe_finite2(c.d)) {
        free(anchors);
        return pc_engine_err_bad_params(err, CMD, "`move` must be [dx, dy] with finite numbers of magnitude at most "
                                                  "1000000");
    }
    pc_status rc = pe_run(s, p, "Drag Anchor Points", &c, out, err);
    free(anchors);
    return rc;
}

static pc_status run_move_handle(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.moveHandle";
    pe_ctx c = {0};
    c.op = 1;
    if (pe_knot(p, CMD, &c.knot, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t hl = 0;
    const char *h = pc_json_as_str(pc_json_get(p, "handle"), &hl);
    if (h == NULL || !pc_vec_handle_parse(h, hl, &c.handle)) {
        return pc_engine_err_bad_params(err, CMD, "`handle` must be \"in\" or \"out\"");
    }
    yyjson_val *to = pc_json_get(p, "to");
    if (to == NULL || !val_point(to, &c.to) || !isfinite(c.to.x) || !isfinite(c.to.y) || fabs(c.to.x) > PE_MAX ||
        fabs(c.to.y) > PE_MAX) {
        return pc_engine_err_bad_params(err, CMD, "`to` must be [x, y] with finite numbers of magnitude at most "
                                                  "1000000");
    }
    (void)pc_json_as_bool(pc_json_get(p, "independent"), &c.independent);
    return pe_run(s, p, "Drag Direction Point", &c, out, err);
}

static pc_status run_bend_segment(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.bendSegment";
    pe_ctx c = {0};
    c.op = 2;
    if (pe_knot(p, CMD, &c.knot, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_val *tv = pc_json_get(p, "t");
    if (tv != NULL) {
        if (!pc_json_as_f64(tv, &c.t) || !(c.t >= 0.0 && c.t <= 1.0)) {
            return pc_engine_err_bad_params(err, CMD, "`t` must be between 0 and 1");
        }
    } else {
        c.t = 0.5;
    }
    if (!nums_n(p, "move", 2, c.d) || !pe_finite2(c.d)) {
        return pc_engine_err_bad_params(err, CMD, "`move` must be [dx, dy] with finite numbers of magnitude at most "
                                                  "1000000");
    }
    return pe_run(s, p, "Drag Segment", &c, out, err);
}

static pc_status run_convert_point(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.convertPoint";
    pe_ctx c = {0};
    c.op = 3;
    if (pe_knot(p, CMD, &c.knot, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_val *o = pc_json_get(p, "out");
    if (o != NULL && !yyjson_is_null(o)) {
        if (!val_point(o, &c.out) || !isfinite(c.out.x) || !isfinite(c.out.y) || fabs(c.out.x) > PE_MAX ||
            fabs(c.out.y) > PE_MAX) {
            return pc_engine_err_bad_params(err, CMD, "`out` must be [x, y] with finite numbers of magnitude at most "
                                                      "1000000");
        }
        c.has_out = true;
    }
    return pe_run(s, p, "Convert Point", &c, out, err);
}

static pc_status run_add_anchor(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.addAnchor";
    pe_ctx c = {0};
    c.op = 4;
    if (pe_knot(p, CMD, &c.knot, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_val *tv = pc_json_get(p, "t");
    if (tv != NULL) {
        if (!pc_json_as_f64(tv, &c.t) || !(c.t > 0.0 && c.t < 1.0)) {
            return pc_engine_err_bad_params(err, CMD, "`t` must be strictly between 0 and 1");
        }
    } else {
        c.t = 0.5;
    }
    return pe_run(s, p, "Add Anchor Point", &c, out, err);
}

static pc_status run_delete_anchor(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "path.deleteAnchor";
    pe_ctx c = {0};
    c.op = 5;
    if (pe_knot(p, CMD, &c.knot, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pe_run(s, p, "Delete Anchor Point", &c, out, err);
}

#define PE_TARGET                                                                                                      \
    "\"name\":str|\"work\"|\"layer\"=\"work\",\"layer\":id? (with \"layer\": a shape layer's path or a vector mask)"

static const pc_engine_command PE_SPECS[] = {
    {"path.moveAnchors", "Drag Anchor Points", NULL, 0, NULL,
     "{" PE_TARGET ",\"anchors\":[[subpath,knot],\xe2\x80\xa6],\"move\":[dx,dy] (anchors move with their handles; "
     "Direct Selection drag)} \xe2\x86\x92 {name,path}",
     pc_engine_has_doc, run_move_anchors, true},
    {"path.moveHandle", "Drag Direction Point", NULL, 0, NULL,
     "{" PE_TARGET ",\"subpath\":i,\"knot\":j,\"handle\":\"in|out\",\"to\":[x,y],\"independent\":bool=false (a "
     "smooth point turns its other handle to stay collinear; independent = \xe2\x8c\xa5 / Convert Point, makes a "
     "corner)} \xe2\x86\x92 {name,path}",
     pc_engine_has_doc, run_move_handle, true},
    {"path.bendSegment", "Drag Segment", NULL, 0, NULL,
     "{" PE_TARGET ",\"subpath\":i,\"knot\":j (the segment leaving it),\"t\":0..1=0.5 (where it was "
     "grabbed),\"move\":[dx,dy] (a straight segment moves with its anchors; a curve reshapes through the dragged "
     "point)} \xe2\x86\x92 {name,path}",
     pc_engine_has_doc, run_bend_segment, true},
    {"path.convertPoint", "Convert Point", NULL, 0, NULL,
     "{" PE_TARGET ",\"subpath\":i,\"knot\":j,\"out\":[x,y]? (none: corner with retracted handles; given: smooth "
     "with this out handle and its mirror)} \xe2\x86\x92 {name,path}",
     pc_engine_has_doc, run_convert_point, true},
    {"path.addAnchor", "Add Anchor Point", NULL, 0, NULL,
     "{" PE_TARGET ",\"subpath\":i,\"knot\":j (the segment leaving it),\"t\":0<t<1=0.5 (where on the segment) (a "
     "curve is split there without changing its shape and gets a smooth anchor; a straight segment gets a corner) "
     "(the result also has anchor:{subpath,knot})} \xe2\x86\x92 {name,path}",
     pc_engine_has_doc, run_add_anchor, true},
    {"path.deleteAnchor", "Delete Anchor Point", NULL, 0, NULL,
     "{" PE_TARGET ",\"subpath\":i,\"knot\":j (its neighbours are joined, keeping their handles; a subpath's last "
     "anchor removes the subpath; the path's only anchor is refused) (the result also has subpathRemoved:bool)} "
     "\xe2\x86\x92 {name,path}",
     pc_engine_has_doc, run_delete_anchor, true},
};

const pc_engine_command *pc_engine_path_edit_specs(size_t *n)
{
    *n = sizeof PE_SPECS / sizeof PE_SPECS[0];
    return PE_SPECS;
}
