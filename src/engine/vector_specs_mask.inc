/* Layer vector-mask commands (layer.vectorMask.*, layer.rasterize.vectorMask). Included by
 * vector_specs.c. */

static pc_status vmask_info(const pc_engine_session *s, pc_layer_id id, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    if (l->has_vector_mask) {
        const pc_vector_mask *v = &l->vector_mask;
        yyjson_mut_obj_add_val(rd, root, "path", path_to_json(rd, &v->path));
        yyjson_mut_obj_add_bool(rd, root, "enabled", v->enabled);
        yyjson_mut_obj_add_bool(rd, root, "linked", v->linked);
        yyjson_mut_obj_add_real(rd, root, "density", (double)v->density * 100.0);
        yyjson_mut_obj_add_real(rd, root, "feather", (double)v->feather);
    } else {
        yyjson_mut_obj_add_null(rd, root, "vectorMask");
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_vmask_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return vmask_info(s, id, out, err);
}

/* ---- add -------------------------------------------------------------------------------- */

enum hide_mode { HIDE_FROM_P, HIDE_FALSE, HIDE_TRUE };

typedef struct {
    pc_layer_id id;
    pc_path *path; /* moved into the new mask */
} vmask_add_ctx;

static pc_status edit_vmask_add(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    vmask_add_ctx *ctx = vctx;
    bool locked = pc_document_effective_locks(doc, ctx->id).all;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (l->content.kind == PC_LAYER_SHAPE) {
        return pc_engine_err_other(err, "a shape layer's path is already its vector mask");
    }
    if (locked) {
        return pc_engine_err_other(err, "layer \"%s\" is locked", pc_str_c(&l->name));
    }
    pc_path np;
    if (pc_path_clone(ctx->path, &np, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (l->has_vector_mask) {
        pc_vector_mask_fini(&l->vector_mask);
    }
    pc_vector_mask_init(&l->vector_mask, &np);
    l->has_vector_mask = true;
    return PC_OK;
}

static pc_status vmask_add(pc_engine_session *s, yyjson_val *p, bool from_path, enum hide_mode hide, pc_json_value *out,
                           pc_error *err)
{
    static const char *const CMD = "layer.vectorMask.add";
    char emsg[256];
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_path path = {0};
    yyjson_val *pathv = pc_json_get(p, "path");
    if (pathv != NULL) {
        if (!parse_path(pathv, &path, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
    } else if (from_path || pc_json_get(p, "name") != NULL) {
        size_t nlen = 0;
        const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
        if (is_work(name, nlen)) {
            const pc_document *doc = NULL;
            if (need_doc(s, &doc, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_INVALID;
            }
            if (!doc->has_work_path) {
                return pc_engine_err_other(err, "no work path");
            }
            if (pc_path_clone(&doc->work_path, &path, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        } else {
            if (resolve_path(s, name, nlen, &path, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_INVALID;
            }
        }
    }
    bool invert = hide == HIDE_TRUE || (hide == HIDE_FROM_P && pbool(p, "hide", false));
    if (invert) {
        path.inverted = !path.inverted;
    }
    vmask_add_ctx ctx = {id, &path};
    pc_status rc = pc_engine_session_edit(s, "Add Vector Mask", edit_vmask_add, &ctx, err);
    pc_path_fini(&path);
    if (rc != PC_OK) {
        return rc;
    }
    return vmask_info(s, id, out, err);
}

static pc_status run_vmask_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_add(s, p, false, HIDE_FROM_P, out, err);
}
static pc_status run_vmask_from_path(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_add(s, p, true, HIDE_FROM_P, out, err);
}
static pc_status run_vmask_reveal_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_add(s, p, false, HIDE_FALSE, out, err);
}
static pc_status run_vmask_hide_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_add(s, p, false, HIDE_TRUE, out, err);
}
static pc_status run_vmask_current_path(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_add(s, p, true, HIDE_FROM_P, out, err);
}

/* ---- edit ------------------------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    yyjson_val *p;
    bool has_path;
    pc_path *path;
    bool has_feather;
    float feather;
} vmask_edit_ctx;

static pc_status edit_vmask_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    vmask_edit_ctx *ctx = vctx;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (!l->has_vector_mask) {
        return pc_engine_err_other(err, "layer has no vector mask");
    }
    pc_vector_mask *vm = &l->vector_mask;
    if (ctx->has_path) {
        pc_path np;
        if (pc_path_clone(ctx->path, &np, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_path_fini(&vm->path);
        vm->path = np;
    }
    bool b;
    if (pc_json_as_bool(pc_json_get(ctx->p, "enabled"), &b)) {
        vm->enabled = b;
    }
    if (pc_json_as_bool(pc_json_get(ctx->p, "linked"), &b)) {
        vm->linked = b;
    }
    double d;
    if (get_f64(ctx->p, "density", &d)) {
        vm->density = (float)pc_clampd(d / 100.0, 0.0, 1.0);
    }
    if (ctx->has_feather) {
        vm->feather = ctx->feather;
    }
    if (pc_json_as_bool(pc_json_get(ctx->p, "invert"), &b) && b) {
        vm->path.inverted = !vm->path.inverted;
    }
    return PC_OK;
}

static pc_status vmask_edit_run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const CMD = "layer.vectorMask.edit";
    char emsg[256];
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_path path = {0};
    bool has_path = false;
    yyjson_val *pathv = pc_json_get(p, "path");
    if (pathv != NULL) {
        if (!parse_path(pathv, &path, emsg, sizeof emsg)) {
            return pc_engine_err_bad_params(err, CMD, "%s", emsg);
        }
        has_path = true;
    }
    bool has_feather = false;
    float feather = 0.0f;
    yyjson_val *fv = pc_json_get(p, "feather");
    if (fv != NULL) {
        double f;
        if (!pc_json_as_f64(fv, &f) || !isfinite(f) || f < 0.0 || f > 1000.0) {
            if (has_path) {
                pc_path_fini(&path);
            }
            return pc_engine_err_bad_params(err, CMD, "`feather` must be a finite number within 0..1000");
        }
        feather = (float)f;
        has_feather = true;
    }
    vmask_edit_ctx ctx = {id, p, has_path, &path, has_feather, feather};
    pc_status rc = pc_engine_session_edit(s, "Edit Vector Mask", edit_vmask_edit, &ctx, err);
    if (has_path) {
        pc_path_fini(&path);
    }
    if (rc != PC_OK) {
        return rc;
    }
    return vmask_info(s, id, out, err);
}

static pc_status run_vmask_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_edit_run(s, p, out, err);
}

/* ---- toggle enabled / linked ------------------------------------------------------------ */

typedef struct {
    pc_layer_id id;
    bool is_enabled;
    bool value;
} vmask_toggle_ctx;

static pc_status edit_vmask_toggle(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    vmask_toggle_ctx *ctx = vctx;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (!l->has_vector_mask) {
        return pc_engine_err_other(err, "layer has no vector mask");
    }
    if (ctx->is_enabled) {
        l->vector_mask.enabled = ctx->value;
    } else {
        l->vector_mask.linked = ctx->value;
    }
    return PC_OK;
}

static pc_status vmask_toggle(pc_engine_session *s, yyjson_val *p, bool is_enabled, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_document *doc = NULL;
    if (need_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (!l->has_vector_mask) {
        return pc_engine_err_other(err, "layer has no vector mask");
    }
    bool cur = is_enabled ? l->vector_mask.enabled : l->vector_mask.linked;
    bool v;
    if (!pc_json_as_bool(pc_json_get(p, is_enabled ? "enabled" : "linked"), &v)) {
        v = !cur;
    }
    vmask_toggle_ctx ctx = {id, is_enabled, v};
    pc_status rc =
        pc_engine_session_edit(s, is_enabled ? "Edit Vector Mask" : "Edit Vector Mask", edit_vmask_toggle, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return vmask_info(s, id, out, err);
}

static pc_status run_vmask_toggle_enabled(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_toggle(s, p, true, out, err);
}
static pc_status run_vmask_toggle_linked(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return vmask_toggle(s, p, false, out, err);
}

/* ---- delete ----------------------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
} vmask_del_ctx;

static pc_status edit_vmask_delete(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    vmask_del_ctx *ctx = vctx;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (!l->has_vector_mask) {
        return pc_engine_err_other(err, "layer has no vector mask");
    }
    pc_vector_mask_fini(&l->vector_mask);
    memset(&l->vector_mask, 0, sizeof l->vector_mask);
    l->has_vector_mask = false;
    return PC_OK;
}

static pc_status run_vmask_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    vmask_del_ctx ctx = {id};
    if (pc_engine_session_edit(s, "Delete Vector Mask", edit_vmask_delete, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

/* ---- rasterize vector mask -------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
} vmask_ras_ctx;

static pc_status edit_vmask_rasterize(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    vmask_ras_ctx *ctx = vctx;
    pc_rect area = pc_document_bounds(doc);
    pc_sample_type depth = doc->depth;
    pc_layer *l = pc_document_layer_mut(doc, ctx->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, ctx->id);
    }
    if (!l->has_vector_mask) {
        return pc_engine_err_other(err, "layer has no vector mask");
    }
    bool vmask_soft = l->vector_mask.enabled && pc_compose_feather_sigma(l->vector_mask.feather) > 0.0f;
    bool pmask_soft = l->has_mask && l->mask.enabled &&
                      (pc_compose_feather_sigma(l->mask.feather) > 0.0f || l->mask.density < 1.0f);
    bool soft = l->vector_mask.enabled && (vmask_soft || pmask_soft);

    bool have_shown = false;
    pc_surface shown;
    if (soft) {
        if (pc_compose_combined_mask(l, area, &have_shown, &shown, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }

    /* Take the vector mask. */
    pc_vector_mask vm = l->vector_mask;
    memset(&l->vector_mask, 0, sizeof l->vector_mask);
    l->has_vector_mask = false;

    /* Take or build the pixel mask. */
    pc_layer_mask mask;
    if (l->has_mask) {
        mask = l->mask;
        memset(&l->mask, 0, sizeof l->mask);
        l->has_mask = false;
    } else {
        pc_layer_mask_reveal_all(&mask);
        pc_surface_fini(&mask.surface);
        pc_pixel_format gf = {PC_COLOR_MODE_GRAYSCALE, depth, false};
        float one = 1.0f;
        pc_surface_init_default(&mask.surface, gf, &one, 1);
    }

    pc_status rc = PC_OK;
    if (have_shown) {
        float dpx[PC_RASTER_MAX_CHANNELS];
        pc_surface_default_pixel(&mask.surface, dpx);
        float def = 1.0f - mask.density * (1.0f - dpx[0]);
        pc_surface surface;
        pc_surface_init_default(&surface, pc_surface_format(&mask.surface), &def, 1);
        float *region = NULL;
        size_t rlen = 0;
        rc = pc_surface_read_region(&shown, area, &region, &rlen, err);
        if (rc == PC_OK) {
            rc = pc_surface_write_region(&surface, area, region, rlen, err);
            free(region);
        }
        pc_surface_prune(&surface);
        pc_surface_fini(&mask.surface);
        mask.surface = surface;
        mask.feather = 0.0f;
        mask.density = 1.0f;
        pc_surface_fini(&shown);
    } else {
        if (have_shown) {
            pc_surface_fini(&shown);
        }
        float *vals = NULL;
        size_t vlen = 0;
        rc = pc_vec_mask_values(&vm, area, &vals, &vlen, err);
        if (rc == PC_OK) {
            float *old = NULL;
            size_t olen = 0;
            rc = pc_surface_read_region(&mask.surface, area, &old, &olen, err);
            if (rc == PC_OK) {
                for (size_t i = 0; i < olen && i < vlen; i++) {
                    old[i] *= vals[i];
                }
                rc = pc_surface_write_region(&mask.surface, area, old, olen, err);
                free(old);
            }
            free(vals);
        }
        pc_surface_prune(&mask.surface);
    }
    pc_vector_mask_fini(&vm);
    if (rc != PC_OK) {
        pc_layer_mask_fini(&mask);
        return rc;
    }
    l->mask = mask;
    l->has_mask = true;
    return PC_OK;
}

static pc_status run_vmask_rasterize(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    if (layer_id_of(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    vmask_ras_ctx ctx = {id};
    if (pc_engine_session_edit(s, "Rasterize Vector Mask", edit_vmask_rasterize, &ctx, err) != PC_OK) {
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
