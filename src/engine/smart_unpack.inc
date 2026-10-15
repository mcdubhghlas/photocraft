/* Layer > Smart Objects > Convert to Layers: unpacks a smart object's contents back into layers
 * at its placement, as Photoshop does (port of crates/engine/src/smart_cmds/unpack.rs). Several
 * layers land in a group named after the smart object, which carries its opacity, blend mode,
 * masks and style; a single layer replaces the smart object directly when those fold into it
 * without changing the look. Smart filters are discarded (the result reports how many).
 *
 * Included by smart_cmds_run.c; reuses its statics (detach_psd) and smart_contents.inc's
 * cn_source_fini. Never crashes on hostile contents (bad source, deep nesting, huge placements).
 * The command id is "layer.smartObjects.convertToLayers". */

#define UP_CMD "layer.smartObjects.convertToLayers"

/* The smart object of layer `id` in `doc` (const), or an error. */
static pc_status up_smart(const pc_document *doc, pc_layer_id id, const pc_smart_object **out,
                          pc_error *err)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_other(err, "the layer is %s %s layer, not a smart object",
                                   pc_layer_content_article(&l->content),
                                   pc_layer_content_kind_name(&l->content));
    }
    *out = &l->content.u.smart;
    return PC_OK;
}

/* embed_nested: a nested PSD placed layer finds its file in the contents' own global blocks, which
 * stay behind; embed that source so the nested smart object keeps rendering in the document. */
static void up_embed_nested(const pc_metadata *meta, pc_layer *l)
{
    if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.source.linked) {
        const char *path = pc_str_c(&l->content.u.smart.source.path);
        bool found = false;
        pc_io_linked_file lf;
        memset(&lf, 0, sizeof lf);
        if (pc_io_find_linked_file(meta, path, strlen(path), &found, &lf, NULL) == PC_OK && found) {
            pc_smart_source embedded;
            memset(&embedded, 0, sizeof embedded);
            embedded.linked = false;
            bool ok = pc_str_clone(&lf.file_name, &embedded.file_name, NULL) == PC_OK &&
                      pc_blob_new(lf.bytes, lf.len, &embedded.bytes, NULL) == PC_OK;
            if (ok) {
                cn_source_fini(&l->content.u.smart.source);
                l->content.u.smart.source = embedded;
                detach_psd(l);
            } else {
                cn_source_fini(&embedded);
            }
        }
        pc_io_linked_file_fini(&lf);
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            up_embed_nested(meta, &g->children[i]);
        }
    }
}

/* The smart object's contents as top-level layers in `doc`'s colour model, profile and depth,
 * still in source pixel coordinates. *out is an owned array of *n layers on PC_OK (caller frees
 * each with pc_layer_fini, then free()s the array). A Background becomes a normal layer. */
static pc_status up_contents(pc_engine_session *s, const pc_document *doc, const pc_smart_object *sm,
                             pc_intent intent, bool bpc, pc_layer **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    pc_status rc =
        pc_engine_smart_authorized_source_bytes(s, UP_CMD, &doc->metadata, &sm->source, &found, &name, &bytes, err);
    if (rc != PC_OK) {
        pc_str_fini(&name);
        return rc;
    }
    if (!found) {
        pc_str_fini(&name);
        return pc_engine_err_other(err, "the smart object's contents are unavailable (missing linked file?)");
    }

    pc_document src;
    rc = pc_engine_smart_decode_source(pc_str_c(&name), pc_blob_data(bytes), pc_blob_len(bytes), &src, err);
    pc_str_fini(&name);
    pc_blob_release(bytes);
    if (rc != PC_OK) {
        return rc;
    }

    pc_color_mode mode = pc_document_pixel_format(doc).mode;
    bool mode_differs = pc_document_pixel_format(&src).mode != mode;
    bool profile_differs = !pc_blob_eq(src.icc_profile, doc->icc_profile);
    if (mode_differs || profile_differs) {
        pc_profile dp;
        rc = pc_engine_document_profile(doc, &dp, err);
        if (rc != PC_OK) {
            pc_document_fini(&src);
            return rc;
        }
        rc = pc_engine_convert_document(&src, &dp, intent, bpc, err);
        pc_profile_fini(&dp);
        if (rc != PC_OK) {
            pc_document_fini(&src);
            return rc;
        }
        if (pc_document_pixel_format(&src).mode != mode) {
            pc_document_fini(&src);
            return pc_engine_err_other(
                err, "can't convert the smart object's contents to the document's %s mode",
                pc_color_mode_name(mode));
        }
    }
    if (src.depth != doc->depth) {
        rc = pc_engine_convert_layers_depth(src.layers, src.nlayers, doc->depth, err);
        if (rc != PC_OK) {
            pc_document_fini(&src);
            return rc;
        }
    }

    const pc_layer *bg = pc_document_background(&src);
    bool has_bg = bg != NULL;
    pc_layer_id bg_id = has_bg ? bg->id : (pc_layer_id)0;

    /* Move the layers out of src (std::mem::take), leaving src with no layers to free; its
     * metadata stays for up_embed_nested. */
    pc_layer *layers = src.layers;
    size_t nlayers = src.nlayers;
    src.layers = NULL;
    src.nlayers = 0;
    for (size_t i = 0; i < nlayers; i++) {
        pc_layer *l = &layers[i];
        if (has_bg && l->id == bg_id) {
            if (pc_str_set(&l->name, "Layer 0", 7, NULL) != PC_OK) {
                /* keep the old name on allocation failure; never crash */
            }
            l->locks = (pc_locks){0};
        }
        up_embed_nested(&src.metadata, l);
    }
    pc_document_fini(&src);
    *out = layers;
    *n = nlayers;
    return PC_OK;
}

/* ---- placement -------------------------------------------------------------------------------- */

typedef struct {
    pc_locks locks;
    bool has_mask, mask_linked;
    bool has_vmask, vmask_linked;
} up_saved;

static size_t up_subtree_count(const pc_layer *l)
{
    size_t total = 1;
    if (l->content.kind == PC_LAYER_GROUP) {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            total += up_subtree_count(&g->children[i]);
        }
    }
    return total;
}

/* Pre-order (f(l) then children), matching image_cmds::for_each_layer. */
static void up_release_save(pc_layer *l, up_saved *saved, size_t *idx)
{
    up_saved *e = &saved[(*idx)++];
    e->locks = l->locks;
    e->has_mask = l->has_mask;
    e->mask_linked = l->has_mask ? l->mask.linked : false;
    e->has_vmask = l->has_vector_mask;
    e->vmask_linked = l->has_vector_mask ? l->vector_mask.linked : false;
    l->locks = (pc_locks){0};
    if (l->has_mask) {
        l->mask.linked = true;
    }
    if (l->has_vector_mask) {
        l->vector_mask.linked = true;
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            up_release_save(&g->children[i], saved, idx);
        }
    }
}

static void up_release_restore(pc_layer *l, const up_saved *saved, size_t total, size_t *idx)
{
    if (*idx >= total) {
        return;
    }
    const up_saved *e = &saved[(*idx)++];
    l->locks = e->locks;
    if (l->has_mask && e->has_mask) {
        l->mask.linked = e->mask_linked;
    }
    if (l->has_vector_mask && e->has_vmask) {
        l->vector_mask.linked = e->vmask_linked;
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            up_release_restore(&g->children[i], saved, total, idx);
        }
    }
}

/* Transform `l` by `h` with every lock and mask link in its tree released (so the whole subtree
 * moves as one), then restore them. */
static pc_status up_transform_released(pc_layer *l, const pc_algo_homography *h, const pc_affine *affine,
                                       pc_error *err)
{
    size_t count = up_subtree_count(l);
    up_saved *saved = pc_calloc_array(count > 0 ? count : 1, sizeof *saved);
    if (saved == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "Convert to Layers");
    }
    size_t idx = 0;
    up_release_save(l, saved, &idx);
    pc_status rc = pc_engine_transform_layer(NULL, (pc_locks){0}, l, h, affine, PC_ALGO_INTERP_BICUBIC, err);
    size_t ridx = 0;
    up_release_restore(l, saved, count, &ridx);
    free(saved);
    return rc;
}

static bool up_whole(double v)
{
    return fabs(v - round(v)) < 1e-9;
}

/* Pre-check every nested linked source's authorize gate before transforming anything. */
static pc_status up_authorize_subtree(pc_engine_session *s, const pc_document *doc, const pc_layer *l,
                                      pc_error *err)
{
    if (l->content.kind == PC_LAYER_SMART) {
        pc_status rc = pc_engine_smart_authorize_source(s, UP_CMD, doc, &l->content.u.smart.source, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (l->content.kind == PC_LAYER_GROUP) {
        const pc_group *g = &l->content.u.group;
        for (size_t i = 0; i < g->nchildren; i++) {
            pc_status rc = up_authorize_subtree(s, doc, &g->children[i], err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    return PC_OK;
}

/* Move the unpacked layers from source pixels to where the smart object placed them: an exact
 * whole-pixel shift, else its transform (projective for Distort/Perspective). */
static pc_status up_place(pc_engine_session *s, const pc_document *doc, const pc_smart_object *sm,
                          pc_layer *layers, size_t n, pc_error *err)
{
    if (sm->has_warp && !pc_warp_is_identity(&sm->warp)) {
        return pc_engine_err_other(
            err, "a warped smart object can't be converted to layers; rasterize it instead "
                 "(Layer \xE2\x80\xBA Smart Objects \xE2\x80\xBA Rasterize)");
    }
    const double *m = sm->transform.m; /* [a, b, c, d, e, f] */
    if (!sm->has_perspective && fabs(m[0] - 1.0) < 1e-9 && fabs(m[1]) < 1e-9 && fabs(m[2]) < 1e-9 &&
        fabs(m[3] - 1.0) < 1e-9 && up_whole(m[4]) && up_whole(m[5])) {
        int32_t dx = pc_f64_to_i32(round(m[4]));
        int32_t dy = pc_f64_to_i32(round(m[5]));
        for (size_t i = 0; i < n; i++) {
            pc_engine_smart_shift_layer(&layers[i], dx, dy);
        }
        return PC_OK;
    }

    /* The transform path re-renders nested smart objects from their sources: check every read
     * first, so a refused source leaves the document unchanged. */
    if (s != NULL && s->authorize != NULL) {
        for (size_t i = 0; i < n; i++) {
            pc_status rc = up_authorize_subtree(s, doc, &layers[i], err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }

    pc_algo_homography h;
    pc_engine_smart_placement(sm, &h);
    const pc_affine *affine = sm->has_perspective ? NULL : &sm->transform;
    for (size_t i = 0; i < n; i++) {
        pc_status rc = up_transform_released(&layers[i], &h, affine, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_engine_hook_refresh_text(doc, &layers[i]);
    }
    return PC_OK;
}

/* ---- fold the smart layer's properties into the result --------------------------------------- */

/* Whether the smart layer's own properties fold into its one unpacked layer `c` without changing
 * the look. */
static bool up_foldable(const pc_layer *so, const pc_layer *c)
{
    pc_blend_if def_if = so->blend_if;
    (void)def_if;
    return !so->has_mask && !so->has_vector_mask &&
           (!so->effects.enabled || so->effects.nitems == 0) && so->fill_opacity >= 1.0f &&
           pc_blend_if_is_default(&so->blend_if) && so->excluded_channels == 0 && !c->clipped &&
           (c->blend == PC_BLEND_NORMAL || c->blend == PC_BLEND_PASS_THROUGH) &&
           pc_blend_if_is_default(&c->blend_if) && c->excluded_channels == 0;
}

/* What replaces smart layer `so`: its one unpacked layer with `so`'s properties folded in, or a
 * group named after it that carries them (isolated like the smart object, never pass-through).
 * Consumes `layers` (the array is freed; its contents are moved into *out or freed). *is_group is
 * set. */
static pc_status up_unpacked(const pc_layer *so, pc_layer *layers, size_t n, pc_layer *out,
                             bool *is_group, pc_error *err)
{
    pc_blend_mode blend = so->blend == PC_BLEND_PASS_THROUGH ? PC_BLEND_NORMAL : so->blend;

    if (n == 1 && up_foldable(so, &layers[0])) {
        pc_layer l = layers[0]; /* move */
        free(layers);
        pc_str nm;
        if (pc_str_clone(&so->name, &nm, err) != PC_OK) {
            pc_layer_fini(&l);
            return PC_ERR_NOMEM;
        }
        pc_str_fini(&l.name);
        l.name = nm;
        l.visible = l.visible && so->visible;
        l.opacity *= so->opacity;
        l.blend = blend;
        l.clipped = so->clipped;
        l.locks = so->locks;
        l.label = so->label;
        l.has_link_group = so->has_link_group;
        l.link_group = so->link_group;
        *out = l;
        *is_group = false;
        return PC_OK;
    }

    /* A group carrying the smart layer's properties: clone the smart layer and swap its content
     * for a group of the unpacked layers. */
    pc_layer g;
    if (pc_layer_clone(so, &g, err) != PC_OK) {
        for (size_t i = 0; i < n; i++) {
            pc_layer_fini(&layers[i]);
        }
        free(layers);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    g.id = pc_layer_id_fresh();
    g.has_psd_id = false;
    g.blend = blend;
    pc_layer_content_fini(&g.content); /* drop the cloned smart content */
    g.content.kind = PC_LAYER_GROUP;
    memset(&g.content.u.group, 0, sizeof g.content.u.group);
    g.content.u.group.children = layers; /* takes ownership of the array + layers */
    g.content.u.group.nchildren = n;
    g.content.u.group.expanded = true;
    g.content.u.group.has_artboard = false;
    detach_psd(&g);
    *out = g;
    *is_group = true;
    return PC_OK;
}

/* ---- the command ------------------------------------------------------------------------------ */

typedef struct {
    pc_layer_id id;      /* the smart layer to replace */
    pc_layer out;        /* the replacement (moved into the document on success) */
    bool built;          /* out holds a layer that must be freed if the edit never runs */
} up_ctx;

static pc_status up_replace_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    up_ctx *c = vctx;
    pc_layer *slot = pc_document_layer_mut(doc, c->id);
    if (slot == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_layer_id new_id = c->out.id;
    pc_layer_fini(slot);
    *slot = c->out;
    c->built = false; /* ownership moved into the document */
    *active = new_id;
    *has_active = true;
    return PC_OK;
}

static pc_status up_convert_to_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_intent intent = pc_color_settings_intent(&s->color.settings);
    bool bpc = s->color.settings.bpc;

    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *so = pc_document_layer(doc, id);
    if (so == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    const pc_smart_object *sm = NULL;
    rc = up_smart(doc, id, &sm, err);
    if (rc != PC_OK) {
        return rc;
    }

    pc_layer *layers = NULL;
    size_t nlayers = 0;
    rc = up_contents(s, doc, sm, intent, bpc, &layers, &nlayers, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = up_place(s, doc, sm, layers, nlayers, err);
    if (rc != PC_OK) {
        for (size_t i = 0; i < nlayers; i++) {
            pc_layer_fini(&layers[i]);
        }
        free(layers);
        return rc;
    }

    uint64_t discarded = (uint64_t)sm->nsmart_filters;

    up_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.id = id;
    bool is_group = false;
    rc = up_unpacked(so, layers, nlayers, &ctx.out, &is_group, err);
    if (rc != PC_OK) {
        return rc; /* up_unpacked consumed `layers` on every path */
    }
    ctx.built = true;
    pc_layer_id new_id = ctx.out.id;

    rc = pc_engine_session_edit(s, "Convert to Layers", up_replace_edit, &ctx, err);
    if (ctx.built) {
        pc_layer_fini(&ctx.out); /* the edit never took ownership */
    }
    if (rc != PC_OK) {
        return rc;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", (uint64_t)new_id);
    yyjson_mut_obj_add_bool(rd, root, "group", is_group);
    yyjson_mut_obj_add_uint(rd, root, "discardedSmartFilters", discarded);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}
