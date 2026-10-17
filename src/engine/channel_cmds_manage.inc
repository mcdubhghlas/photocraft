/* Channel management commands (included by channel_cmds.c): new, spot, duplicate, delete, rename,
 * move, target, visibility, options, merge spot, split, merge, slots. */

static bool color_value(yyjson_val *p, const char *key, pc_color *out)
{
    if (pc_json_get(p, key) == NULL) {
        return false;
    }
    float def[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    float c[4];
    pc_engine_param_color(p, key, def, c);
    *out = pc_color_rgb(c[0], c[1], c[2]);
    return true;
}

/* "masked"/"maskedAreas" or "selected"/"selectedAreas". */
static bool indicates_param(yyjson_val *v, pc_color_indicates *out)
{
    if (v == NULL) {
        return false;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL) {
        return false;
    }
    if (str_is(s, len, "masked") || str_is(s, len, "maskedAreas")) {
        *out = PC_INDICATES_MASKED_AREAS;
        return true;
    }
    if (str_is(s, len, "selected") || str_is(s, len, "selectedAreas")) {
        *out = PC_INDICATES_SELECTED_AREAS;
        return true;
    }
    return false;
}

static pc_status append_channel(pc_document *doc, pc_alpha_channel *ch, pc_error *err)
{
    if (doc->nchannels >= SIZE_MAX - 1) {
        return pc_fail(err, PC_ERR_LIMIT, "too many channels");
    }
    size_t count = doc->nchannels + 1;
    pc_alpha_channel *g = realloc(doc->channels, count * sizeof *doc->channels);
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "channels");
    }
    doc->channels = g;
    doc->channels[doc->nchannels] = *ch;
    doc->nchannels++;
    return PC_OK;
}

/* ---- New Channel / Spot ------------------------------------------------------------------ */

struct newch_ctx {
    char fill[16];
    char *name;
    bool has_indicates;
    pc_color_indicates indicates;
    bool has_color;
    pc_color color;
    bool has_opacity;
    float opacity;
    bool spot;
    float solidity;
    bool from_selection;
    size_t out_index;
};

static pc_status newch_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct newch_ctx *c = vctx;
    pc_rect area = pc_document_bounds(doc);
    pc_pixel_format fmt = channel_format(doc);
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    pc_surface surf;
    pc_status rc;
    bool want_sel = c->spot ? (c->from_selection && doc->has_selection)
                            : (strcmp(c->fill, "selection") == 0);
    if (!c->spot && strcmp(c->fill, "white") == 0) {
        float *v = malloc(npx ? npx * sizeof(float) : 1);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "channel");
        }
        for (size_t i = 0; i < npx; i++) {
            v[i] = 1.0f;
        }
        rc = plane_surface(v, npx, area, fmt, &surf, err);
        free(v);
    } else if (want_sel) {
        float *m = NULL;
        size_t mn = 0;
        rc = pc_algo_selection_mask_from_surface(doc->has_selection ? &doc->selection : NULL, area, &m,
                                                 &mn, err);
        if (rc == PC_OK) {
            if (m == NULL) {
                m = calloc(npx ? npx : 1, sizeof(float));
                mn = npx;
            }
            if (m == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "channel");
            }
            rc = plane_surface(m, mn, area, fmt, &surf, err);
            free(m);
        }
    } else {
        pc_surface_init(&surf, fmt);
        rc = PC_OK;
    }
    if (rc != PC_OK) {
        return rc;
    }
    char nm[128];
    if (c->name != NULL) {
        (void)snprintf(nm, sizeof nm, "%s", c->name);
    } else {
        next_alpha_name(doc, c->spot ? "Spot Color" : "Alpha", nm, sizeof nm);
    }
    pc_alpha_channel ch;
    rc = pc_alpha_channel_init(&ch, nm, &surf, err);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    if (c->spot) {
        ch.has_spot = true;
        ch.spot_color = c->color;
        ch.spot_solidity = c->solidity;
    } else {
        if (c->has_indicates) {
            ch.indicates = c->indicates;
        }
        if (c->has_color) {
            ch.color = c->color;
        }
        if (c->has_opacity) {
            ch.opacity = c->opacity;
        }
    }
    rc = append_channel(doc, &ch, err);
    if (rc != PC_OK) {
        pc_alpha_channel_fini(&ch);
        return rc;
    }
    c->out_index = doc->nchannels - 1;
    return PC_OK;
}

static pc_status result_channel_index(pc_json_value *out, size_t index, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "channel", index);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static char *dup_trimmed_name(yyjson_val *p, const char *key)
{
    size_t len = 0;
    const char *s = get_str(p, key, &len);
    if (s == NULL) {
        return NULL;
    }
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) {
        s++;
        len--;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        len--;
    }
    if (len == 0) {
        return NULL;
    }
    char *r = malloc(len + 1);
    if (r == NULL) {
        return NULL;
    }
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

static pc_status run_new_channel(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct newch_ctx c = {{0}, NULL, false, PC_INDICATES_MASKED_AREAS, false, {0}, false, 0.0f, false,
                          0.0f, true, 0};
    size_t flen = 0;
    const char *fill = get_str(p, "fill", &flen);
    (void)snprintf(c.fill, sizeof c.fill, "%.*s", fill ? (int)flen : 5, fill ? fill : "black");
    /* name may have interior spaces, so take it verbatim (not trimmed) when non-null. */
    size_t nlen = 0;
    const char *nm = get_str(p, "name", &nlen);
    if (nm != NULL) {
        c.name = malloc(nlen + 1);
        if (c.name == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "name");
        }
        memcpy(c.name, nm, nlen);
        c.name[nlen] = '\0';
    }
    pc_color_indicates ind;
    if (indicates_param(pc_json_get(p, "indicates"), &ind)) {
        c.has_indicates = true;
        c.indicates = ind;
    }
    pc_color col;
    if (color_value(p, "color", &col)) {
        c.has_color = true;
        c.color = col;
    }
    double op;
    yyjson_val *ov = pc_json_get(p, "opacity");
    if (ov != NULL && pc_json_as_f64(ov, &op)) {
        float v = (float)op / 100.0f;
        c.has_opacity = true;
        c.opacity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    pc_status rc = pc_engine_session_edit(s, "New Channel", newch_edit, &c, err);
    free(c.name);
    if (rc != PC_OK) {
        return rc;
    }
    (void)set_target(s, CT_ALPHA, c.out_index, err);
    return result_channel_index(out, c.out_index, err);
}

static pc_status run_new_spot(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct newch_ctx c = {{0}, NULL, false, PC_INDICATES_MASKED_AREAS, false, {0}, false, 0.0f, true,
                          0.0f, true, 0};
    pc_color col;
    c.color = color_value(p, "color", &col) ? col : pc_color_rgb(1.0f, 0.0f, 0.0f);
    double sol;
    yyjson_val *sv = pc_json_get(p, "solidity");
    if (sv != NULL && pc_json_as_f64(sv, &sol)) {
        float v = (float)sol;
        v = v < 0.0f ? 0.0f : (v > 100.0f ? 100.0f : v);
        c.solidity = v / 100.0f;
    }
    c.from_selection = get_bool(p, "fromSelection", true);
    size_t nlen = 0;
    const char *nm = get_str(p, "name", &nlen);
    if (nm != NULL) {
        c.name = malloc(nlen + 1);
        if (c.name == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "name");
        }
        memcpy(c.name, nm, nlen);
        c.name[nlen] = '\0';
    }
    pc_status rc = pc_engine_session_edit(s, "New Spot Channel", newch_edit, &c, err);
    free(c.name);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        struct pc_engine_channel_view *v = cv_ensure(st);
        if (v != NULL) {
            (void)cv_set_alpha_visible(v, c.out_index, true);
        }
        touch(st);
    }
    return result_channel_index(out, c.out_index, err);
}

/* ---- gray document ----------------------------------------------------------------------- */

static pc_status gray_document(const char *name, const pc_document *like, const float *plane, size_t n,
                               pc_document *out, pc_error *err)
{
    pc_status rc = pc_document_init(out, name, like->size, PC_COLOR_MODE_GRAYSCALE, like->depth, err);
    if (rc != PC_OK) {
        return rc;
    }
    out->resolution_dpi = like->resolution_dpi;
    pc_pixel_format fmt = pc_document_pixel_format(out);
    pc_layer bg;
    rc = pc_layer_raster(&bg, "Background", fmt, err);
    if (rc != PC_OK) {
        pc_document_fini(out);
        return rc;
    }
    bg.locks.transparency = true;
    bg.locks.position = true;
    pc_surface *surf = pc_layer_surface_mut(&bg);
    if (surf != NULL) {
        size_t ch = pc_pixel_format_channels(fmt);
        float *data = malloc(n ? n * ch * sizeof(float) : 1);
        if (data == NULL) {
            pc_layer_fini(&bg);
            pc_document_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "gray doc");
        }
        for (size_t i = 0; i < n; i++) {
            float v = plane[i];
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            data[i * ch] = v;
            if (ch > 1) {
                data[i * ch + 1] = 1.0f;
            }
        }
        rc = pc_surface_write_region(surf, pc_document_bounds(out), data, n * ch, err);
        free(data);
        if (rc != PC_OK) {
            pc_layer_fini(&bg);
            pc_document_fini(out);
            return rc;
        }
        pc_surface_prune(surf);
    }
    pc_layer *lg = realloc(out->layers, (out->nlayers + 1) * sizeof(pc_layer));
    if (lg == NULL) {
        pc_layer_fini(&bg);
        pc_document_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "gray doc");
    }
    out->layers = lg;
    out->layers[out->nlayers] = bg;
    out->nlayers++;
    return PC_OK;
}

/* ---- Duplicate Channel ------------------------------------------------------------------- */

struct dupch_ctx {
    pc_alpha_channel ch; /* moved into the destination */
    size_t out_index;
};

static pc_status dup_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct dupch_ctx *c = vctx;
    pc_pixel_format fmt = channel_format(doc);
    if (!pc_pixel_format_eq(pc_surface_format(&c->ch.surface), fmt)) {
        pc_surface conv;
        pc_status rc = pc_surface_convert(&c->ch.surface, fmt, &conv, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&c->ch.surface);
        c->ch.surface = conv;
    }
    pc_status rc = append_channel(doc, &c->ch, err);
    if (rc != PC_OK) {
        return rc;
    }
    c->out_index = doc->nchannels - 1;
    memset(&c->ch, 0, sizeof c->ch); /* moved into the document */
    return PC_OK;
}

static pc_status run_duplicate_channel(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                       pc_error *err)
{
    const char *cmd = "channel.duplicate";
    const pc_engine_doc_state *src = pc_engine_session_active(s);
    if (src == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *sd = pc_doc_arc_get(src->doc);
    pc_rect area = pc_document_bounds(sd);
    chan_ref r;
    if (!parse_ref(pc_json_get(p, "channel"), sd, &r)) {
        return pc_engine_err_bad_params(err, cmd, "missing or unknown `channel`");
    }
    pc_alpha_channel ch;
    memset(&ch, 0, sizeof ch);
    char base[256] = {0};
    if (r.kind == REF_ALPHA) {
        if (r.index >= sd->nchannels) {
            return pc_engine_err_bad_params(err, cmd, "no alpha channel %zu", r.index);
        }
        pc_status rc = pc_alpha_channel_clone(&sd->channels[r.index], &ch, err);
        if (rc != PC_OK) {
            return rc;
        }
        (void)snprintf(base, sizeof base, "%s", pc_str_c(&ch.name));
    } else if (r.kind == REF_QUICK_MASK) {
        if (!sd->has_quick_mask) {
            return pc_engine_err_bad_params(err, cmd, "not in Quick Mask mode");
        }
        pc_status rc = pc_alpha_channel_clone(&sd->quick_mask, &ch, err);
        if (rc != PC_OK) {
            return rc;
        }
        (void)snprintf(base, sizeof base, "%s", pc_str_c(&ch.name));
    } else {
        float *plane = NULL;
        size_t pn = 0;
        pc_status rc = gray_plane(sd, false, 0, src->has_active_layer, src->active_layer, r, cmd,
                                  &plane, &pn, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface surf;
        rc = plane_surface(plane, pn, area, channel_format(sd), &surf, err);
        free(plane);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_alpha_channel_init(&ch, "", &surf, err);
        if (rc != PC_OK) {
            pc_surface_fini(&surf);
            return rc;
        }
        size_t ncolors = 0;
        const char *const *names = color_names(edit_mode(sd), &ncolors);
        if (r.kind == REF_COLOR && r.index < ncolors) {
            (void)snprintf(base, sizeof base, "%s", names[r.index]);
        } else {
            (void)snprintf(base, sizeof base, "%s", composite_name(edit_mode(sd)));
        }
    }
    /* name */
    char *nm = dup_trimmed_name(p, "name");
    char fullname[300];
    if (nm != NULL) {
        (void)snprintf(fullname, sizeof fullname, "%s", nm);
        free(nm);
    } else {
        (void)snprintf(fullname, sizeof fullname, "%s copy", base);
    }
    if (pc_str_set_c(&ch.name, fullname, err) != PC_OK) {
        pc_alpha_channel_fini(&ch);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (get_bool(p, "invert", false)) {
        float *v = NULL;
        size_t vn = 0;
        pc_status rc = read_plane(&ch.surface, area, &v, &vn, err);
        if (rc != PC_OK) {
            pc_alpha_channel_fini(&ch);
            return rc;
        }
        if (v != NULL) {
            for (size_t i = 0; i < vn; i++) {
                v[i] = 1.0f - v[i];
            }
            pc_surface surf;
            rc = plane_surface(v, vn, area, pc_surface_format(&ch.surface), &surf, err);
            free(v);
            if (rc != PC_OK) {
                pc_alpha_channel_fini(&ch);
                return rc;
            }
            pc_surface_fini(&ch.surface);
            ch.surface = surf;
        }
    }
    /* destination: "new" document or an existing one. */
    size_t cvlen = 0;
    const char *docp = get_str(p, "document", &cvlen);
    if (docp != NULL && str_is(docp, cvlen, "new")) {
        float *plane = NULL;
        size_t pn = 0;
        pc_status rc = read_plane(&ch.surface, area, &plane, &pn, err);
        if (rc != PC_OK) {
            pc_alpha_channel_fini(&ch);
            return rc;
        }
        char dn[300];
        (void)snprintf(dn, sizeof dn, "%s", pc_str_c(&ch.name));
        pc_document nd;
        rc = gray_document(dn, sd, plane ? plane : (float[]){0}, pn, &nd, err);
        free(plane);
        pc_alpha_channel_fini(&ch);
        if (rc != PC_OK) {
            return rc;
        }
        size_t idx;
        rc = pc_engine_session_add_document(s, &nd, NULL, &idx, err);
        if (rc != PC_OK) {
            pc_document_fini(&nd);
            return rc;
        }
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "document", idx);
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    /* existing document index */
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndocs);
    size_t di;
    int64_t dv;
    if (pc_engine_param_int(p, "document", &dv)) {
        if (dv < 0 || (uint64_t)dv >= ndocs) {
            pc_alpha_channel_fini(&ch);
            return pc_engine_err_other(err, "no open document %lld", (long long)dv);
        }
        di = (size_t)dv;
    } else {
        if (!pc_engine_session_active_index(s, &di)) {
            pc_alpha_channel_fini(&ch);
            return pc_engine_err_no_document(err);
        }
    }
    const pc_document *dd = pc_doc_arc_get(docs[di].doc);
    if (dd->size.width != sd->size.width || dd->size.height != sd->size.height) {
        pc_alpha_channel_fini(&ch);
        return pc_engine_err_bad_params(err, cmd, "the destination document must have the same size");
    }
    struct dupch_ctx c = {ch, 0};
    size_t prev;
    bool had_prev = pc_engine_session_active_index(s, &prev);
    pc_engine_session_set_active(s, di);
    pc_status rc = pc_engine_session_edit(s, "Duplicate Channel", dup_edit, &c, err);
    if (had_prev) {
        pc_engine_session_set_active(s, prev);
    }
    if (rc != PC_OK) {
        pc_alpha_channel_fini(&c.ch);
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "channel", c.out_index);
    yyjson_mut_obj_add_uint(d, root, "document", di);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Delete / Rename / Move -------------------------------------------------------------- */

/* Alpha channel index from p[key] (number or name), checked against the document. */
static pc_status alpha_index(const pc_document *doc, yyjson_val *p, const char *key, const char *cmd,
                             size_t *out, pc_error *err)
{
    chan_ref r;
    yyjson_val *v = pc_json_get(p, key);
    if (v != NULL && parse_ref(v, doc, &r) && r.kind == REF_ALPHA) {
        if (r.index < doc->nchannels) {
            *out = r.index;
            return PC_OK;
        }
        return pc_engine_err_bad_params(err, cmd, "no alpha channel %zu (document has %zu)", r.index,
                                        doc->nchannels);
    }
    return pc_engine_err_bad_params(err, cmd, "`%s` must name an alpha channel (index or name)", key);
}

struct removech_ctx {
    size_t index;
};

static pc_status remove_channel_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx,
                                     pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    struct removech_ctx *c = vctx;
    if (c->index >= doc->nchannels) {
        return PC_OK;
    }
    pc_alpha_channel_fini(&doc->channels[c->index]);
    memmove(&doc->channels[c->index], &doc->channels[c->index + 1],
            (doc->nchannels - c->index - 1) * sizeof(pc_alpha_channel));
    doc->nchannels--;
    return PC_OK;
}

/* Snapshot of a channel view (so delete can restore it past the edit's fix_view). */
static struct pc_engine_channel_view *cv_clone(const struct pc_engine_channel_view *src)
{
    if (src == NULL) {
        return NULL;
    }
    struct pc_engine_channel_view *v = calloc(1, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    v->target_kind = src->target_kind;
    v->target_index = src->target_index;
    v->quick_mask_hidden = src->quick_mask_hidden;
    if (src->ncolor_hidden > 0) {
        v->color_hidden = malloc(src->ncolor_hidden * sizeof(bool));
        if (v->color_hidden == NULL) {
            free(v);
            return NULL;
        }
        memcpy(v->color_hidden, src->color_hidden, src->ncolor_hidden * sizeof(bool));
        v->ncolor_hidden = src->ncolor_hidden;
    }
    if (src->nalpha_visible > 0) {
        v->alpha_visible = malloc(src->nalpha_visible * sizeof(bool));
        if (v->alpha_visible == NULL) {
            free(v->color_hidden);
            free(v);
            return NULL;
        }
        memcpy(v->alpha_visible, src->alpha_visible, src->nalpha_visible * sizeof(bool));
        v->nalpha_visible = src->nalpha_visible;
    }
    return v;
}

static pc_status run_delete_channel(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    const char *cmd = "channel.delete";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t i;
    if (pc_json_get(p, "channel") != NULL) {
        pc_status rc = alpha_index(doc, p, "channel", cmd, &i, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else if (d->channel_view != NULL && d->channel_view->target_kind == CT_ALPHA) {
        i = d->channel_view->target_index;
    } else {
        return pc_engine_err_bad_params(err, cmd, "missing `channel`");
    }
    /* Keep the original view across the edit (whose fix_view would remap against the shorter list). */
    struct pc_engine_channel_view *saved = cv_clone(d->channel_view);
    bool had_view = d->channel_view != NULL;
    if (had_view && saved == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "channel view");
    }
    struct removech_ctx c = {i};
    pc_status rc = pc_engine_session_edit(s, "Delete Channel", remove_channel_edit, &c, err);
    if (rc != PC_OK) {
        pc_engine_channel_view_free(saved);
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        pc_engine_channel_view_free(st->channel_view);
        st->channel_view = saved;
        struct pc_engine_channel_view *v = st->channel_view;
        if (v != NULL) {
            if (i < v->nalpha_visible) {
                memmove(&v->alpha_visible[i], &v->alpha_visible[i + 1],
                        (v->nalpha_visible - i - 1) * sizeof(bool));
                v->nalpha_visible--;
            }
            if (v->target_kind == CT_ALPHA) {
                if (v->target_index == i) {
                    cv_clear_colors(v);
                    v->target_kind = CT_COMPOSITE;
                } else if (v->target_index > i) {
                    v->target_index--;
                }
            }
        }
        fix_view_now(st);
    } else {
        pc_engine_channel_view_free(saved);
    }
    return pc_engine_result_null(out, err);
}

struct renamech_ctx {
    size_t index;
    const char *name;
};

static pc_status rename_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct renamech_ctx *c = vctx;
    if (c->index < doc->nchannels) {
        return pc_str_set_c(&doc->channels[c->index].name, c->name, err);
    }
    return PC_OK;
}

static pc_status run_rename_channel(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    const char *cmd = "channel.rename";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    size_t i;
    pc_status rc = alpha_index(pc_doc_arc_get(d->doc), p, "channel", cmd, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    char *nm = dup_trimmed_name(p, "name");
    if (nm == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `name`");
    }
    struct renamech_ctx c = {i, nm};
    rc = pc_engine_session_edit(s, "Rename Channel", rename_edit, &c, err);
    free(nm);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

struct movech_ctx {
    size_t from, to;
};

static pc_status move_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    struct movech_ctx *c = vctx;
    if (c->from >= doc->nchannels || c->to >= doc->nchannels) {
        return PC_OK;
    }
    pc_alpha_channel ch = doc->channels[c->from];
    if (c->from < c->to) {
        memmove(&doc->channels[c->from], &doc->channels[c->from + 1],
                (c->to - c->from) * sizeof(pc_alpha_channel));
    } else {
        memmove(&doc->channels[c->to + 1], &doc->channels[c->to],
                (c->from - c->to) * sizeof(pc_alpha_channel));
    }
    doc->channels[c->to] = ch;
    return PC_OK;
}

static pc_status run_move_channel(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "channel.move";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t n = doc->nchannels;
    size_t i;
    pc_status rc = alpha_index(doc, p, "channel", cmd, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    uint64_t tov;
    yyjson_val *tv = pc_json_get(p, "to");
    if (tv == NULL || !pc_json_as_u64(tv, &tov)) {
        return pc_engine_err_bad_params(err, cmd, "missing `to`");
    }
    size_t to = (size_t)tov;
    if (n > 0 && to > n - 1) {
        to = n - 1;
    }
    if (to == i) {
        return result_channel_index(out, i, err);
    }
    struct movech_ctx c = {i, to};
    rc = pc_engine_session_edit(s, "Channel Order", move_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        struct pc_engine_channel_view *v = cv_ensure(st);
        if (v != NULL) {
            if (!bool_resize(&v->alpha_visible, &v->nalpha_visible, n)) {
                return pc_fail(err, PC_ERR_NOMEM, "channel view");
            }
            bool vis = v->alpha_visible[i];
            memmove(&v->alpha_visible[i], &v->alpha_visible[i + 1],
                    (n - i - 1) * sizeof(bool));
            memmove(&v->alpha_visible[to + 1], &v->alpha_visible[to],
                    (n - to - 1) * sizeof(bool));
            v->alpha_visible[to] = vis;
            if (v->target_kind == CT_ALPHA) {
                size_t t = v->target_index;
                if (t == i) {
                    t = to;
                } else if (i < t && t <= to) {
                    t--;
                } else if (to <= t && t < i) {
                    t++;
                }
                v->target_index = t;
            }
        }
        touch(st);
    }
    return result_channel_index(out, to, err);
}

/* ---- Target / visibility ----------------------------------------------------------------- */

/* Parse a target into (kind,index). */
static pc_status parse_target(pc_engine_session *s, yyjson_val *p, const char *cmd, int *kind,
                              size_t *index, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    chan_ref r = {REF_COMPOSITE, 0};
    yyjson_val *cv = pc_json_get(p, "channel");
    if (cv != NULL && !parse_ref(cv, doc, &r)) {
        return pc_engine_err_bad_params(err, cmd,
                                        "`channel` must be \"composite\", \"mask\", \"vectorMask\", a "
                                        "colour channel name or an alpha channel");
    }
    switch (r.kind) {
    case REF_COMPOSITE:
        *kind = CT_COMPOSITE;
        return PC_OK;
    case REF_COLOR:
        if (r.index < pc_engine_color_count(doc) && pc_engine_color_count(doc) > 1) {
            *kind = CT_COLOR;
            *index = r.index;
            return PC_OK;
        }
        break;
    case REF_ALPHA:
        if (r.index < doc->nchannels) {
            *kind = CT_ALPHA;
            *index = r.index;
            return PC_OK;
        }
        break;
    case REF_LAYER_MASK: {
        pc_error why = {0};
        const pc_layer *l = pc_engine_active_layer_of(s, &why);
        if (l == NULL) {
            return pc_engine_err_bad_params(err, cmd, "%s", why.msg);
        }
        if (!l->has_mask) {
            return pc_engine_err_bad_params(err, cmd, "the active layer has no layer mask");
        }
        *kind = CT_LAYER_MASK;
        return PC_OK;
    }
    case REF_VECTOR_MASK: {
        pc_error why = {0};
        const pc_layer *l = pc_engine_active_layer_of(s, &why);
        if (l == NULL) {
            return pc_engine_err_bad_params(err, cmd, "%s", why.msg);
        }
        if (!l->has_vector_mask) {
            return pc_engine_err_bad_params(err, cmd, "the active layer has no vector mask");
        }
        *kind = CT_VECTOR_MASK;
        return PC_OK;
    }
    default:
        break;
    }
    return pc_engine_err_bad_params(err, cmd,
                                    "`channel` must be \"composite\", \"mask\", \"vectorMask\", a "
                                    "colour channel name or an alpha channel");
}

static pc_status run_target_cmd(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    int kind = CT_COMPOSITE;
    size_t index = 0;
    pc_status rc = parse_target(s, p, "channel.target", &kind, &index, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = set_target(s, kind, index, err);
    if (rc != PC_OK) {
        return rc;
    }
    return channels_result(s, out, err);
}

static pc_status run_target_layer_mask(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                       pc_error *err)
{
    (void)p;
    pc_status rc = set_target(s, CT_LAYER_MASK, 0, err);
    if (rc != PC_OK) {
        return rc;
    }
    return channels_result(s, out, err);
}

static pc_status run_channel_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return channels_result(s, out, err);
}

static pc_status run_set_visible(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "channel.setVisible";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    chan_ref r;
    if (!parse_ref(pc_json_get(p, "channel"), doc, &r)) {
        return pc_engine_err_bad_params(err, cmd, "missing or unknown `channel`");
    }
    size_t colors = pc_engine_color_count(doc);
    size_t n = doc->nchannels;
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    struct pc_engine_channel_view *v = cv_ensure(st);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "channel view");
    }
    bool has_vis = false, vis = false;
    yyjson_val *vv = pc_json_get(p, "visible");
    if (vv != NULL && pc_json_as_bool(vv, &vis)) {
        has_vis = true;
    }
    switch (r.kind) {
    case REF_COMPOSITE: {
        bool all = cv_visible_colors(v, colors) == colors;
        bool on = has_vis ? vis : !all;
        if (!bool_resize(&v->color_hidden, &v->ncolor_hidden, colors)) {
            return pc_fail(err, PC_ERR_NOMEM, "channel view");
        }
        for (size_t k = 0; k < colors; k++) {
            v->color_hidden[k] = !on;
        }
        break;
    }
    case REF_COLOR:
        if (r.index >= colors) {
            return pc_engine_err_bad_params(err, cmd, "that channel has no visibility");
        }
        {
            bool on = has_vis ? vis : !cv_color_visible(v, r.index);
            if (!cv_set_color_visible(v, r.index, on)) {
                return pc_fail(err, PC_ERR_NOMEM, "channel view");
            }
        }
        break;
    case REF_ALPHA:
        if (r.index >= n) {
            return pc_engine_err_bad_params(err, cmd, "that channel has no visibility");
        }
        {
            bool on = has_vis ? vis : !cv_alpha_shown(v, r.index);
            if (!cv_set_alpha_visible(v, r.index, on)) {
                return pc_fail(err, PC_ERR_NOMEM, "channel view");
            }
        }
        break;
    case REF_QUICK_MASK: {
        bool on = has_vis ? vis : v->quick_mask_hidden; /* toggle of !hidden */
        v->quick_mask_hidden = !(has_vis ? vis : !v->quick_mask_hidden);
        (void)on;
        break;
    }
    default:
        return pc_engine_err_bad_params(err, cmd, "that channel has no visibility");
    }
    fix_view_now(st);
    return channels_result(s, out, err);
}

/* ---- Channel Options --------------------------------------------------------------------- */

struct options_ctx {
    size_t index;
    bool has_name;
    char *name;
    bool has_color;
    pc_color color;
    bool has_opacity;
    float opacity;
    bool has_solidity;
    float solidity;
    int kind; /* 0 none, 1 spot, 2 masked, 3 selected */
};

static void options_apply(pc_alpha_channel *ch, const struct options_ctx *c)
{
    if (c->has_name) {
        (void)pc_str_set_c(&ch->name, c->name, NULL);
    }
    if (c->kind == 1) { /* spot */
        pc_color col = ch->has_spot ? ch->spot_color : ch->color;
        float sol = ch->has_spot ? ch->spot_solidity : 0.0f;
        ch->has_spot = true;
        ch->spot_color = c->has_color ? c->color : col;
        ch->spot_solidity = c->has_solidity ? c->solidity : sol;
        return;
    }
    if (c->kind == 2 || c->kind == 3) {
        ch->indicates = c->kind == 3 ? PC_INDICATES_SELECTED_AREAS : PC_INDICATES_MASKED_AREAS;
        ch->has_spot = false;
    }
    if (ch->has_spot) {
        if (c->has_color) {
            ch->spot_color = c->color;
        }
        if (c->has_solidity) {
            ch->spot_solidity = c->solidity;
        } else if (c->has_opacity) {
            ch->spot_solidity = c->opacity;
        }
    } else {
        if (c->has_color) {
            ch->color = c->color;
        }
        if (c->has_opacity) {
            ch->opacity = c->opacity;
        }
    }
}

static pc_status options_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    struct options_ctx *c = vctx;
    if (c->index < doc->nchannels) {
        options_apply(&doc->channels[c->index], c);
    }
    return PC_OK;
}

/* Apply new Quick Mask Options to the live Quick Mask channel. */
struct qmopts_ctx {
    pc_color color;
    float opacity;
    pc_color_indicates indicates;
};

static pc_status qmopts_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct qmopts_ctx *o = vctx;
    if (!doc->has_quick_mask) {
        return PC_OK;
    }
    pc_rect area = pc_document_bounds(doc);
    pc_pixel_format fmt = channel_format(doc);
    pc_alpha_channel *q = &doc->quick_mask;
    /* An untouched no-selection mask stays colourless when the colour switches masked/selected. */
    if (doc->quick_mask_unselected && q->indicates != o->indicates) {
        float empty = q->indicates == PC_INDICATES_SELECTED_AREAS ? 0.0f : 1.0f;
        float *vals = NULL;
        size_t n = 0;
        pc_status rc = read_plane(&q->surface, area, &vals, &n, err);
        if (rc != PC_OK) {
            return rc;
        }
        bool uni = vals != NULL && uniform_plane(vals, n, empty);
        free(vals);
        if (uni) {
            pc_surface fresh;
            rc = unselected_surface(area, fmt, o->indicates, &fresh, err);
            if (rc != PC_OK) {
                return rc;
            }
            pc_surface_fini(&q->surface);
            q->surface = fresh;
        }
    }
    q->color = o->color;
    q->opacity = o->opacity;
    q->indicates = o->indicates;
    return PC_OK;
}

static pc_status run_options(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "channel.options";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    chan_ref r;
    if (!parse_ref(pc_json_get(p, "channel"), doc, &r)) {
        return pc_engine_err_bad_params(err, cmd, "missing or unknown `channel`");
    }
    struct options_ctx c = {0};
    char *nm = dup_trimmed_name(p, "name");
    if (nm != NULL) {
        c.has_name = true;
        c.name = nm;
    }
    pc_color col;
    if (color_value(p, "color", &col)) {
        c.has_color = true;
        c.color = col;
    }
    double op;
    yyjson_val *ov = pc_json_get(p, "opacity");
    if (ov != NULL && pc_json_as_f64(ov, &op)) {
        float v = (float)op / 100.0f;
        c.has_opacity = true;
        c.opacity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    double sol;
    yyjson_val *sv = pc_json_get(p, "solidity");
    if (sv != NULL && pc_json_as_f64(sv, &sol)) {
        float v = (float)sol / 100.0f;
        c.has_solidity = true;
        c.solidity = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    size_t klen = 0;
    const char *kind = get_str(p, "indicates", &klen);
    if (kind != NULL) {
        if (str_is(kind, klen, "spot")) {
            c.kind = 1;
        } else {
            pc_color_indicates ind;
            if (!indicates_param(pc_json_get(p, "indicates"), &ind)) {
                free(c.name);
                return pc_engine_err_bad_params(err, cmd, "unknown `indicates` %.*s", (int)klen, kind);
            }
            c.kind = ind == PC_INDICATES_SELECTED_AREAS ? 3 : 2;
        }
    }

    if (r.kind == REF_ALPHA && r.index < doc->nchannels) {
        c.index = r.index;
        pc_status rc = pc_engine_session_edit(s, "Channel Options", options_edit, &c, err);
        free(c.name);
        if (rc != PC_OK) {
            return rc;
        }
        return channels_result(s, out, err);
    }
    if (r.kind == REF_QUICK_MASK) {
        /* Quick Mask Options persist for the session; the live Quick Mask follows them. */
        pc_alpha_channel probe;
        memset(&probe, 0, sizeof probe);
        probe.color = s->quick_mask_color;
        probe.opacity = s->quick_mask_opacity;
        probe.indicates = s->quick_mask_indicates;
        options_apply(&probe, &c);
        if (probe.has_spot) {
            pc_str_fini(&probe.name);
            free(c.name);
            return pc_engine_err_bad_params(err, cmd, "Quick Mask can't be a spot channel");
        }
        s->quick_mask_color = probe.color;
        s->quick_mask_opacity = probe.opacity;
        s->quick_mask_indicates = probe.indicates;
        pc_str_fini(&probe.name);
        free(c.name);
        if (pc_doc_arc_get(d->doc)->has_quick_mask) {
            struct qmopts_ctx o = {s->quick_mask_color, s->quick_mask_opacity, s->quick_mask_indicates};
            pc_status rc = pc_engine_session_edit(s, "Quick Mask Options", qmopts_edit, &o, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        return channels_result(s, out, err);
    }
    free(c.name);
    return pc_engine_err_bad_params(err, cmd, "`channel` must be an alpha/spot channel or \"quickMask\"");
}

#include "channel_cmds_bulk.inc"
