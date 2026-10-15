/* Shape-layer commands (shape.*, layer.combineShapes.*, select.convertToShape). Included by
 * vector_specs.c; all helpers above are in scope. */

/* ---- insert a freshly built shape layer --------------------------------------------------- */

typedef struct {
    const pc_shape_layer *sh;
    const char *kind;
    bool has_name;
    const char *name;
    pc_layer_id out_id;
} insert_ctx;

static pc_status edit_insert_shape(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    insert_ctx *ctx = vctx;
    pc_str name = {0};
    pc_status rc = ctx->has_name ? pc_str_set_c(&name, ctx->name, err)
                                 : pc_document_next_layer_name(doc, default_name(ctx->kind), &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_content content = {0};
    content.kind = PC_LAYER_SHAPE;
    rc = pc_shape_layer_clone(ctx->sh, &content.u.shape, err);
    if (rc != PC_OK) {
        pc_str_fini(&name);
        return rc;
    }
    pc_engine_refresh_shape(doc, &content.u.shape);
    pc_layer layer;
    rc = pc_layer_init(&layer, pc_str_c(&name), &content, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    rc = pc_document_insert_above(doc, *ha ? a : NULL, &layer, &ctx->out_id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    *a = ctx->out_id;
    *ha = true;
    return PC_OK;
}

/* Insert *sh (borrowed; cloned by the edit) as a new shape layer and report shape.info. */
static pc_status insert_new_shape(pc_engine_session *s, const char *kind, const pc_shape_layer *sh, yyjson_val *p,
                                  pc_json_value *out, pc_error *err)
{
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    insert_ctx ctx = {sh, kind, name != NULL, name, 0};
    if (pc_engine_session_edit(s, "New Shape Layer", edit_insert_shape, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return shape_info(s, ctx.out_id, out, err);
}

/* ---- shape.create ------------------------------------------------------------------------- */

typedef struct {
    const pc_path *src;
    pc_path_op op;
    bool has_stroke;
    const pc_shape_stroke *stroke;
} append_ctx;

static pc_status shape_append(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    (void)l;
    append_ctx *ctx = ud;
    size_t add = ctx->src->nsubpaths;
    if (add > 0) {
        pc_subpath *ns = realloc(sh->path.subpaths, (sh->path.nsubpaths + add) * sizeof *ns);
        if (ns == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "add shape");
        }
        sh->path.subpaths = ns;
        for (size_t i = 0; i < add; i++) {
            pc_subpath sp;
            if (pc_subpath_clone(&ctx->src->subpaths[i], &sp, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            sp.op = ctx->op;
            sh->path.subpaths[sh->path.nsubpaths++] = sp;
        }
    }
    sh->has_live = false;
    pc_blob_release(sh->psd_raw);
    sh->psd_raw = NULL;
    return pc_vec_validate_shape(&sh->path, sh->has_stroke ? &sh->stroke : NULL, err);
}

static pc_status run_shape_create(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "shape.create";
    char emsg[256];
    size_t klen = 0;
    const char *kind = pc_json_as_str(pc_json_get(p, "kind"), &klen);
    char kbuf[32] = "rect";
    if (kind != NULL) {
        if (klen >= sizeof kbuf) {
            return pc_engine_err_bad_params(err, CMD, "unknown kind");
        }
        memcpy(kbuf, kind, klen);
        kbuf[klen] = '\0';
    }

    pc_path path = {0};
    bool has_live = false;
    pc_live_shape live = {0};
    if (!strcmp(kbuf, "path")) {
        yyjson_val *pv = pc_json_get(p, "path");
        if (pv == NULL) {
            return pc_engine_err_bad_params(err, CMD, "kind \"path\" needs `path`");
        }
        if (!parse_path(pv, &path, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
    } else {
        if (!live_from_params(CMD, kbuf, p, NULL, &live, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        if (pc_vec_live_path(&live, &path, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        has_live = true;
    }

    const float *fg = s->tools.foreground;
    pc_fill fill = {0};
    bool has_fill = true;
    yyjson_val *fv = pc_json_get(p, "fill");
    if (fv != NULL) {
        int r = parse_fill(fv, &fill, emsg, sizeof emsg);
        if (r < 0) {
            pc_path_fini(&path);
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        has_fill = r == 1;
    } else {
        fill = pc_fill_solid(pc_color_rgba(fg[0], fg[1], fg[2], fg[3]));
    }

    pc_shape_stroke stroke = {0};
    bool has_stroke = false;
    yyjson_val *sv = pc_json_get(p, "stroke");
    if (sv != NULL) {
        int r = parse_stroke(sv, NULL, fg, &stroke, emsg, sizeof emsg);
        if (r < 0) {
            pc_path_fini(&path);
            if (has_fill) {
                pc_fill_fini(&fill);
            }
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        has_stroke = r == 1;
    }

    if (pc_vec_validate_shape(&path, has_stroke ? &stroke : NULL, err) != PC_OK) {
        pc_path_fini(&path);
        if (has_fill) {
            pc_fill_fini(&fill);
        }
        if (has_stroke) {
            pc_shape_stroke_fini(&stroke);
        }
        /* validate_shape reports via err; re-wrap as bad params to match the reference. */
        char m[256];
        snprintf(m, sizeof m, "%s", err != NULL ? err->msg : "invalid shape");
        return pc_engine_err_bad_params(err, CMD, "%s", m);
    }

    uint64_t target;
    yyjson_val *addto = pc_json_get(p, "addTo");
    if (addto != NULL && pc_json_as_u64(addto, &target)) {
        pc_path_op op = PC_PATH_COMBINE;
        size_t olen = 0;
        const char *os = pc_json_as_str(pc_json_get(p, "op"), &olen);
        if (os != NULL && !op_from(os, olen, &op)) {
            pc_path_fini(&path);
            if (has_fill) {
                pc_fill_fini(&fill);
            }
            if (has_stroke) {
                pc_shape_stroke_fini(&stroke);
            }
            return pc_engine_err_bad_params(err, CMD, "unknown `op`");
        }
        append_ctx ac = {&path, op, has_stroke, has_stroke ? &stroke : NULL};
        pc_status rc = with_shape(s, (pc_layer_id)target, "Add Shape", shape_append, &ac, err);
        pc_path_fini(&path);
        if (has_fill) {
            pc_fill_fini(&fill);
        }
        if (has_stroke) {
            pc_shape_stroke_fini(&stroke);
        }
        if (rc != PC_OK) {
            return rc;
        }
        return shape_info(s, (pc_layer_id)target, out, err);
    }

    pc_shape_layer sh = {0};
    sh.path = path;
    sh.has_fill = has_fill;
    if (has_fill) {
        sh.fill = fill;
    }
    sh.has_stroke = has_stroke;
    if (has_stroke) {
        sh.stroke = stroke;
    }
    sh.has_live = has_live;
    if (has_live) {
        sh.live = live;
    }
    pc_status rc = insert_new_shape(s, kbuf, &sh, p, out, err);
    pc_shape_layer_fini(&sh);
    return rc;
}

/* ---- shape.edit --------------------------------------------------------------------------- */

typedef struct {
    yyjson_val *p;
    const float *fg;
    const char *op_override; /* combineShapes single-layer forces the op */
} edit_ctx;

static pc_status shape_edit_fn(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    static const char *const CMD = "shape.edit";
    edit_ctx *ctx = ud;
    yyjson_val *p = ctx->p;
    char emsg[256];

    yyjson_val *pathv = pc_json_get(p, "path");
    if (pathv != NULL) {
        pc_path np;
        if (!parse_path(pathv, &np, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        pc_path_fini(&sh->path);
        sh->path = np;
        sh->has_live = false;
        pc_blob_release(sh->psd_raw);
        sh->psd_raw = NULL;
    }

    static const char *const GEOM[] = {"kind", "rect", "radii", "radius", "sides", "starRatio", "from", "to", "weight"};
    bool geom = false;
    for (size_t i = 0; i < sizeof GEOM / sizeof GEOM[0]; i++) {
        if (pc_json_get(p, GEOM[i]) != NULL) {
            geom = true;
            break;
        }
    }
    if (geom) {
        size_t klen = 0;
        const char *kind = pc_json_as_str(pc_json_get(p, "kind"), &klen);
        char kbuf[32];
        if (kind != NULL) {
            if (klen >= sizeof kbuf) {
                return pc_engine_err_bad_params(err, CMD, "unknown kind");
            }
            memcpy(kbuf, kind, klen);
            kbuf[klen] = '\0';
        } else if (sh->has_live) {
            snprintf(kbuf, sizeof kbuf, "%s", live_kind(&sh->live));
        } else {
            return pc_engine_err_bad_params(err, CMD, "this shape has no live-shape parameters; pass `kind` or `path`");
        }
        /* Explicit radii on a plain rectangle keep it a (now rounded) rectangle. */
        double rd[4];
        if (!strcmp(kbuf, "rect") && read_radii(p, rd) && (rd[0] > 0.0 || rd[1] > 0.0 || rd[2] > 0.0 || rd[3] > 0.0)) {
            snprintf(kbuf, sizeof kbuf, "roundedRect");
        }
        pc_live_shape nl;
        if (!live_from_params(CMD, kbuf, p, sh->has_live ? &sh->live : NULL, &nl, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        if (!sh->has_live || !pc_live_shape_eq(&sh->live, &nl)) {
            pc_path npth;
            if (pc_vec_live_path(&nl, &npth, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            pc_path_fini(&sh->path);
            sh->path = npth;
            sh->live = nl;
            sh->has_live = true;
            pc_blob_release(sh->psd_raw);
            sh->psd_raw = NULL;
        }
    }

    double m[6];
    if (nums_n(p, "transform", 6, m)) {
        pc_affine a = {{m[0], m[1], m[2], m[3], m[4], m[5]}};
        pc_engine_transform_shape(sh, &a);
    }
    double mv[2];
    if (nums_n(p, "move", 2, mv)) {
        pc_affine a = pc_affine_translate(mv[0], mv[1]);
        pc_engine_transform_shape(sh, &a);
    }

    const char *os = ctx->op_override;
    size_t olen = os != NULL ? strlen(os) : 0;
    if (os == NULL) {
        os = pc_json_as_str(pc_json_get(p, "op"), &olen);
    }
    if (os != NULL) {
        pc_path_op op;
        if (!op_from(os, olen, &op)) {
            return pc_engine_err_bad_params(err, CMD, "unknown op `%.*s`", (int)olen, os);
        }
        uint64_t idx;
        if (pc_json_as_u64(pc_json_get(p, "subpath"), &idx)) {
            if (idx >= sh->path.nsubpaths) {
                return pc_engine_err_bad_params(err, CMD, "no such subpath");
            }
            sh->path.subpaths[idx].op = op;
        } else {
            for (size_t i = 1; i < sh->path.nsubpaths; i++) {
                sh->path.subpaths[i].op = op;
            }
        }
        sh->has_live = false;
    }

    size_t frlen = 0;
    const char *fr = pc_json_as_str(pc_json_get(p, "fillRule"), &frlen);
    if (fr != NULL) {
        sh->path.fill_rule = ieq(fr, "evenodd", frlen) ? PC_FILL_RULE_EVENODD : PC_FILL_RULE_NONZERO;
    }

    yyjson_val *fv = pc_json_get(p, "fill");
    if (fv != NULL) {
        pc_fill nf;
        int r = parse_fill(fv, &nf, emsg, sizeof emsg);
        if (r < 0) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        if (sh->has_fill) {
            pc_fill_fini(&sh->fill);
        }
        if (r == 1) {
            sh->fill = nf;
            sh->has_fill = true;
        } else {
            memset(&sh->fill, 0, sizeof sh->fill);
            sh->has_fill = false;
        }
    }

    yyjson_val *sv = pc_json_get(p, "stroke");
    if (sv != NULL) {
        pc_shape_stroke ns;
        int r = parse_stroke(sv, sh->has_stroke ? &sh->stroke : NULL, ctx->fg, &ns, emsg, sizeof emsg);
        if (r < 0) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        if (sh->has_stroke) {
            pc_shape_stroke_fini(&sh->stroke);
        }
        if (r == 1) {
            sh->stroke = ns;
            sh->has_stroke = true;
        } else {
            memset(&sh->stroke, 0, sizeof sh->stroke);
            sh->has_stroke = false;
        }
    }

    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (name != NULL) {
        if (pc_str_set(&l->name, name, nlen, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }

    if (pc_vec_validate_shape(&sh->path, sh->has_stroke ? &sh->stroke : NULL, err) != PC_OK) {
        char mm[256];
        snprintf(mm, sizeof mm, "%s", err != NULL ? err->msg : "invalid shape");
        return pc_engine_err_bad_params(err, CMD, "%s", mm);
    }
    return PC_OK;
}

static pc_status shape_edit_impl(pc_engine_session *s, yyjson_val *p, const char *op_override, pc_json_value *out,
                                 pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    edit_ctx ctx = {p, s->tools.foreground, op_override};
    if (with_shape(s, id, "Edit Shape", shape_edit_fn, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return shape_info(s, id, out, err);
}

static pc_status run_shape_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return shape_edit_impl(s, p, NULL, out, err);
}

static pc_status run_shape_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return shape_info(s, id, out, err);
}

/* ---- shape.rasterize ---------------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
} rasterize_ctx;

static pc_status edit_rasterize_shape(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    rasterize_ctx *ctx = vctx;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (l->content.kind != PC_LAYER_SHAPE) {
        return pc_engine_err_other(err, "layer %llu is not a shape layer", (unsigned long long)ctx->id);
    }
    pc_shape_layer *sh = &l->content.u.shape;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    if (!sh->has_cache) {
        pc_engine_refresh_shape(doc, sh);
    }
    pc_surface surface;
    if (sh->has_cache) {
        surface = sh->cache;
        pc_surface_init(&sh->cache, fmt); /* leave a valid empty cache for pc_shape_layer_fini */
        sh->has_cache = false;
    } else {
        pc_surface_init(&surface, fmt);
    }
    if (!pc_pixel_format_eq(pc_surface_format(&surface), fmt)) {
        pc_surface conv;
        if (pc_surface_convert(&surface, fmt, &conv, err) != PC_OK) {
            pc_surface_fini(&surface);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&surface);
        surface = conv;
    }
    pc_layer_content_fini(&l->content);
    l->content.kind = PC_LAYER_RASTER;
    l->content.u.raster = surface;
    /* Drop the shape's PSD blocks. */
    size_t w = 0;
    for (size_t i = 0; i < l->npsd_blocks; i++) {
        bool drop = false;
        for (size_t k = 0; k < 8; k++) {
            if (memcmp(l->psd_blocks[i].key, SHAPE_BLOCKS[k], 4) == 0) {
                drop = true;
                break;
            }
        }
        if (drop) {
            pc_blob_release(l->psd_blocks[i].data);
        } else {
            l->psd_blocks[w++] = l->psd_blocks[i];
        }
    }
    l->npsd_blocks = w;
    return PC_OK;
}

static pc_status run_shape_rasterize(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    rasterize_ctx ctx = {id};
    if (pc_engine_session_edit(s, "Rasterize Shape", edit_rasterize_shape, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
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

/* ---- combine shapes / merge components ---------------------------------------------------- */

/* *multi = true with *out / *nout the shape layers to merge (two or more selected, no narrow key);
 * *multi = false means "act on a single layer" (caller falls back to shape.edit / bake). */
static pc_status combine_targets(pc_engine_session *s, yyjson_val *p, const char *const *narrow, size_t nn,
                                 bool *multi, pc_layer_id **out, size_t *nout, pc_error *err)
{
    *multi = false;
    *out = NULL;
    *nout = 0;
    for (size_t i = 0; i < nn; i++) {
        if (pc_json_get(p, narrow[i]) != NULL) {
            return PC_OK;
        }
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_selected(s, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *top = NULL;
    size_t ntop = 0;
    pc_status rc = pc_engine_top_level(doc, sel, nsel, &top, &ntop, err);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id *ids = ntop > 0 ? malloc(ntop * sizeof *ids) : NULL;
    size_t nids = 0;
    for (size_t i = 0; i < ntop; i++) {
        const pc_layer *l = pc_document_layer(doc, top[i]);
        if (l != NULL && l->content.kind == PC_LAYER_SHAPE) {
            ids[nids++] = top[i];
        }
    }
    free(top);
    if (nids > 1) {
        *multi = true;
        *out = ids;
        *nout = nids;
    } else {
        free(ids);
    }
    return PC_OK;
}

static pc_status bake_components(pc_shape_layer *sh, double tol, pc_error *err)
{
    const double K = 4.0;
    pc_path up;
    if (pc_path_transform(&sh->path, pc_affine_scale(K), &up, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_vec_rasterizer r;
    if (pc_vec_fill_rasterizer(&up, PC_VEC_DEFAULT_TOLERANCE, &r, err) != PC_OK) {
        pc_path_fini(&up);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_path_fini(&up);
    pc_rect b;
    if (!pc_vec_rasterizer_pixel_bounds(&r, &b)) {
        pc_vec_rasterizer_fini(&r);
        pc_path_fini(&sh->path);
        memset(&sh->path, 0, sizeof sh->path);
        sh->has_live = false;
        pc_blob_release(sh->psd_raw);
        sh->psd_raw = NULL;
        return PC_OK;
    }
    b = pc_rect_inflate(b, 2);
    if ((uint64_t)pc_rect_width(b) * pc_rect_height(b) > 400000000ULL) {
        pc_vec_rasterizer_fini(&r);
        return pc_engine_err_other(err, "shape too large to merge");
    }
    float *cov = NULL;
    size_t clen = 0;
    if (pc_vec_rasterizer_render(&r, b, &cov, &clen, err) != PC_OK) {
        pc_vec_rasterizer_fini(&r);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_vec_rasterizer_fini(&r);
    pc_path traced;
    pc_status rc = pc_vec_trace_mask(cov, clen, b, 0.0f, tol * K, &traced, err);
    free(cov);
    if (rc != PC_OK) {
        return rc;
    }
    pc_path scaled;
    rc = pc_path_transform(&traced, pc_affine_scale(1.0 / K), &scaled, err);
    pc_path_fini(&traced);
    if (rc != PC_OK) {
        return rc;
    }
    pc_path_fini(&sh->path);
    sh->path = scaled;
    sh->has_live = false;
    pc_blob_release(sh->psd_raw);
    sh->psd_raw = NULL;
    return PC_OK;
}

typedef struct {
    double tol;
} bake_ctx;

static pc_status bake_fn(pc_shape_layer *sh, pc_layer *l, void *ud, pc_error *err)
{
    (void)l;
    return bake_components(sh, ((bake_ctx *)ud)->tol, err);
}

typedef struct {
    const pc_layer_id *ids;
    size_t nids;
    bool has_op;
    pc_path_op op;
    bool has_bake;
    double bake_tol;
    const char *label;
} merge_ctx;

static pc_status edit_merge_shapes(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    merge_ctx *ctx = vctx;
    pc_layer_id keep = ctx->ids[ctx->nids - 1];
    for (size_t i = 0; i < ctx->nids; i++) {
        pc_locks locks = pc_document_effective_locks(doc, ctx->ids[i]);
        if (locks.all || locks.pixels) {
            return pc_engine_err_other(err, "a selected shape layer is locked");
        }
    }
    pc_path merged = {0};
    bool have = false;
    for (size_t i = 0; i < ctx->nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ctx->ids[i]);
        if (l == NULL) {
            pc_path_fini(&merged);
            return pc_engine_err_no_layer(err, ctx->ids[i]);
        }
        if (l->content.kind != PC_LAYER_SHAPE) {
            pc_path_fini(&merged);
            return pc_engine_err_other(err, "layer %llu is not a shape layer", (unsigned long long)ctx->ids[i]);
        }
        const pc_shape_layer *sh = &l->content.u.shape;
        if (!have) {
            if (pc_path_clone(&sh->path, &merged, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            have = true;
            continue;
        }
        size_t add = sh->path.nsubpaths;
        if (add > 0) {
            pc_subpath *ns = realloc(merged.subpaths, (merged.nsubpaths + add) * sizeof *ns);
            if (ns == NULL) {
                pc_path_fini(&merged);
                return pc_fail(err, PC_ERR_NOMEM, "combine shapes");
            }
            merged.subpaths = ns;
            for (size_t j = 0; j < add; j++) {
                pc_subpath sp;
                if (pc_subpath_clone(&sh->path.subpaths[j], &sp, err) != PC_OK) {
                    pc_path_fini(&merged);
                    return err != NULL ? err->code : PC_ERR_NOMEM;
                }
                sp.op = ctx->has_op ? ctx->op : (j == 0 ? PC_PATH_COMBINE : sp.op);
                merged.subpaths[merged.nsubpaths++] = sp;
            }
        }
    }
    if (!have) {
        pc_path_fini(&merged);
        return pc_engine_err_other(err, "Combine Shapes needs two or more shape layers");
    }
    pc_layer *kl = pc_document_layer_mut(doc, keep);
    if (kl == NULL || kl->content.kind != PC_LAYER_SHAPE) {
        pc_path_fini(&merged);
        return pc_engine_err_no_layer(err, keep);
    }
    pc_shape_layer *ksh = &kl->content.u.shape;
    pc_shape_layer m2;
    if (pc_shape_layer_clone(ksh, &m2, err) != PC_OK) {
        pc_path_fini(&merged);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_path_fini(&m2.path);
    m2.path = merged;
    m2.has_live = false;
    pc_blob_release(m2.psd_raw);
    m2.psd_raw = NULL;
    if (m2.has_cache) {
        pc_surface_fini(&m2.cache);
        memset(&m2.cache, 0, sizeof m2.cache);
        m2.has_cache = false;
    }
    if (ctx->has_bake) {
        if (bake_components(&m2, ctx->bake_tol, err) != PC_OK) {
            pc_shape_layer_fini(&m2);
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
    }
    if (pc_vec_validate_shape(&m2.path, m2.has_stroke ? &m2.stroke : NULL, err) != PC_OK) {
        char mm[256];
        snprintf(mm, sizeof mm, "%s", err != NULL ? err->msg : "invalid shape");
        pc_shape_layer_fini(&m2);
        return pc_engine_err_bad_params(err, ctx->label, "%s", mm);
    }
    pc_engine_refresh_shape(doc, &m2);
    pc_shape_layer_fini(ksh);
    *ksh = m2;
    for (size_t i = 0; i < ctx->nids; i++) {
        if (ctx->ids[i] != keep) {
            pc_layer gone;
            if (pc_document_remove(doc, ctx->ids[i], &gone, err) == PC_OK) {
                pc_layer_fini(&gone);
            }
        }
    }
    *a = keep;
    *ha = true;
    return PC_OK;
}

static pc_status merge_shape_layers(pc_engine_session *s, const pc_layer_id *ids, size_t nids, bool has_op,
                                    pc_path_op op, const char *label, bool has_bake, double bake_tol,
                                    pc_json_value *out, pc_error *err)
{
    if (nids < 2) {
        return pc_engine_err_other(err, "Combine Shapes needs two or more shape layers");
    }
    pc_layer_id keep = ids[nids - 1];
    merge_ctx ctx = {ids, nids, has_op, op, has_bake, bake_tol, label};
    if (pc_engine_session_edit(s, label, edit_merge_shapes, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    (void)pc_engine_set_selection(s, &keep, 1, true, keep, true, keep, NULL);
    return shape_info(s, keep, out, err);
}

static pc_status combine_shapes(pc_engine_session *s, yyjson_val *p, const char *op_str, const char *label,
                                pc_json_value *out, pc_error *err)
{
    static const char *const narrow[] = {"layer", "subpath"};
    bool multi;
    pc_layer_id *ids;
    size_t nids;
    if (combine_targets(s, p, narrow, 2, &multi, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (multi) {
        pc_path_op op;
        if (!op_from(op_str, strlen(op_str), &op)) {
            free(ids);
            return pc_engine_err_other(err, "unknown op `%s`", op_str);
        }
        pc_status rc = merge_shape_layers(s, ids, nids, true, op, label, false, 0.0, out, err);
        free(ids);
        return rc;
    }
    return shape_edit_impl(s, p, op_str, out, err);
}

static pc_status run_combine_unite(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return combine_shapes(s, p, "combine", "Unite Shapes", out, err);
}
static pc_status run_combine_subtract(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return combine_shapes(s, p, "subtract", "Subtract Front Shape", out, err);
}
static pc_status run_combine_intersect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return combine_shapes(s, p, "intersect", "Intersect Shape Areas", out, err);
}
static pc_status run_combine_exclude(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return combine_shapes(s, p, "exclude", "Exclude Overlapping Shapes", out, err);
}

static pc_status run_merge_components(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double tol = 0.1;
    (void)get_f64(p, "tolerance", &tol);
    tol = tol < 0.05 ? 0.05 : tol > 10.0 ? 10.0 : tol;
    static const char *const narrow[] = {"layer"};
    bool multi;
    pc_layer_id *ids;
    size_t nids;
    if (combine_targets(s, p, narrow, 1, &multi, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (multi) {
        pc_status rc =
            merge_shape_layers(s, ids, nids, false, PC_PATH_COMBINE, "Merge Shape Components", true, tol, out, err);
        free(ids);
        return rc;
    }
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bake_ctx bc = {tol};
    if (with_shape(s, id, "Merge Shape Components", bake_fn, &bc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return shape_info(s, id, out, err);
}

/* ---- select.convertToShape ---------------------------------------------------------------- */

static pc_status run_selection_to_shape(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double tol;
    if (!get_f64(p, "tolerance", &tol) && !get_f64(p, "simplifyTolerance", &tol)) {
        tol = 2.0;
    }
    tol = tol < 0.5 ? 0.5 : tol > 10.0 ? 10.0 : tol;
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_rect area = pc_document_bounds(doc);
    float *mask = NULL;
    size_t mlen = 0;
    if (pc_algo_selection_mask_from_surface(doc->has_selection ? &doc->selection : NULL, area, &mask, &mlen, err) !=
        PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_path path;
    pc_status rc = pc_vec_trace_mask(mask, mlen, area, 0.0f, tol, &path, err);
    free(mask);
    if (rc != PC_OK) {
        return rc;
    }

    char emsg[256];
    const float *fg = s->tools.foreground;
    pc_fill fill = {0};
    bool has_fill = true;
    yyjson_val *fv = pc_json_get(p, "fill");
    if (fv != NULL) {
        int r = parse_fill(fv, &fill, emsg, sizeof emsg);
        if (r < 0) {
            pc_path_fini(&path);
            return pc_engine_err_bad_params(err, "shape.create", "%s", emsg);
        }
        has_fill = r == 1;
    } else {
        fill = pc_fill_solid(pc_color_rgba(fg[0], fg[1], fg[2], fg[3]));
    }
    pc_shape_stroke stroke = {0};
    bool has_stroke = false;
    yyjson_val *sv = pc_json_get(p, "stroke");
    if (sv != NULL) {
        int r = parse_stroke(sv, NULL, fg, &stroke, emsg, sizeof emsg);
        if (r < 0) {
            pc_path_fini(&path);
            if (has_fill) {
                pc_fill_fini(&fill);
            }
            return pc_engine_err_bad_params(err, "shape.create", "%s", emsg);
        }
        has_stroke = r == 1;
    }
    if (pc_vec_validate_shape(&path, has_stroke ? &stroke : NULL, err) != PC_OK) {
        char mm[256];
        snprintf(mm, sizeof mm, "%s", err != NULL ? err->msg : "invalid shape");
        pc_path_fini(&path);
        if (has_fill) {
            pc_fill_fini(&fill);
        }
        if (has_stroke) {
            pc_shape_stroke_fini(&stroke);
        }
        return pc_engine_err_bad_params(err, "shape.create", "%s", mm);
    }
    pc_shape_layer sh = {0};
    sh.path = path;
    sh.has_fill = has_fill;
    if (has_fill) {
        sh.fill = fill;
    }
    sh.has_stroke = has_stroke;
    if (has_stroke) {
        sh.stroke = stroke;
    }
    sh.has_live = false;
    pc_status r2 = insert_new_shape(s, "path", &sh, p, out, err);
    pc_shape_layer_fini(&sh);
    return r2;
}
