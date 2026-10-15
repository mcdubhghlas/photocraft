/* Save/Load Selection and Quick Mask (included by channel_cmds.c). */

/* Replace doc->selection with combine(old, m, area, mode). *some reports whether anything is
 * selected. */
static pc_status combine_into_doc(pc_document *doc, const float *m, size_t n, pc_rect area,
                                  pc_algo_selection_mode mode, bool *some, pc_error *err)
{
    pc_surface out;
    bool s = false;
    pc_status rc = pc_algo_selection_combine(doc->has_selection ? &doc->selection : NULL, m, n, area,
                                             mode, &out, &s, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (s) {
        doc->selection = out;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&out);
    }
    if (some != NULL) {
        *some = s;
    }
    return PC_OK;
}

/* ---- Save Selection ---------------------------------------------------------------------- */

struct save_ctx {
    const char *cmd;
    float *sel; /* selection plane over area */
    size_t n;
    pc_rect area;
    char op[16];
    yyjson_val *channel; /* raw param, parsed against the destination doc */
    char *name;          /* owned, or NULL */
    size_t out_index;
};

static pc_status save_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                           pc_error *err)
{
    (void)has_active;
    (void)active;
    struct save_ctx *c = vctx;
    pc_pixel_format fmt = channel_format(doc);
    /* existing alpha channel index (or none). */
    bool has_existing = false;
    size_t existing = 0;
    if (c->channel != NULL) {
        size_t sl = 0;
        const char *s = pc_json_as_str(c->channel, &sl);
        bool is_new = yyjson_is_null(c->channel) || (s != NULL && str_is(s, sl, "new"));
        if (!is_new) {
            chan_ref r;
            if (parse_ref(c->channel, doc, &r) && r.kind == REF_ALPHA && r.index < doc->nchannels) {
                has_existing = true;
                existing = r.index;
            } else {
                return pc_engine_err_bad_params(err, c->cmd,
                                                "`channel` must be \"new\" or an existing alpha channel");
            }
        }
    }
    bool make_new = !has_existing || strcmp(c->op, "new") == 0;
    if (make_new) {
        char nm[128];
        if (c->name != NULL) {
            (void)snprintf(nm, sizeof nm, "%s", c->name);
        } else {
            next_alpha_name(doc, "Alpha", nm, sizeof nm);
        }
        pc_surface surf;
        pc_status rc = plane_surface(c->sel, c->n, c->area, fmt, &surf, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_alpha_channel ch;
        rc = pc_alpha_channel_init(&ch, nm, &surf, err);
        if (rc != PC_OK) {
            pc_surface_fini(&surf);
            return rc;
        }
        pc_alpha_channel *g = realloc(doc->channels, (doc->nchannels + 1) * sizeof *doc->channels);
        if (g == NULL) {
            pc_alpha_channel_fini(&ch);
            return pc_fail(err, PC_ERR_NOMEM, "channels");
        }
        doc->channels = g;
        doc->channels[doc->nchannels] = ch;
        doc->nchannels++;
        c->out_index = doc->nchannels - 1;
        return PC_OK;
    }
    /* Combine into the existing channel. */
    float *old = NULL;
    size_t on = 0;
    pc_status rc = read_plane(&doc->channels[existing].surface, c->area, &old, &on, err);
    if (rc != PC_OK) {
        return rc;
    }
    float *vals = malloc(c->n ? c->n * sizeof(float) : 1);
    if (vals == NULL) {
        free(old);
        return pc_fail(err, PC_ERR_NOMEM, "save selection");
    }
    for (size_t i = 0; i < c->n; i++) {
        float a = (i < on) ? old[i] : 0.0f, b = c->sel[i];
        if (strcmp(c->op, "add") == 0) {
            vals[i] = a > b ? a : b;
        } else if (strcmp(c->op, "subtract") == 0) {
            float d = a - b;
            vals[i] = d > 0.0f ? d : 0.0f;
        } else if (strcmp(c->op, "intersect") == 0) {
            vals[i] = a < b ? a : b;
        } else {
            vals[i] = b;
        }
    }
    free(old);
    pc_surface surf;
    rc = plane_surface(vals, c->n, c->area, fmt, &surf, err);
    free(vals);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_fini(&doc->channels[existing].surface);
    doc->channels[existing].surface = surf;
    if (c->name != NULL) {
        (void)pc_str_set_c(&doc->channels[existing].name, c->name, NULL);
    }
    c->out_index = existing;
    return PC_OK;
}

static pc_status run_save_selection(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    const char *cmd = "select.saveSelection";
    size_t oplen = 0;
    const char *op = get_str(p, "operation", &oplen);
    if (op == NULL) {
        op = "new";
        oplen = 3;
    }
    if (!(str_is(op, oplen, "new") || str_is(op, oplen, "replace") || str_is(op, oplen, "add") ||
          str_is(op, oplen, "subtract") || str_is(op, oplen, "intersect"))) {
        return pc_engine_err_bad_params(err, cmd, "unknown operation `%.*s`", (int)oplen, op);
    }
    /* document index */
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndocs);
    size_t di;
    int64_t dv;
    if (pc_engine_param_int(p, "document", &dv)) {
        if (dv < 0 || (uint64_t)dv >= ndocs) {
            return pc_engine_err_other(err, "no open document %lld", (long long)dv);
        }
        di = (size_t)dv;
    } else {
        if (!pc_engine_session_active_index(s, &di)) {
            return pc_engine_err_no_document(err);
        }
    }
    const pc_engine_doc_state *src = pc_engine_session_active(s);
    if (src == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *sd = pc_doc_arc_get(src->doc);
    pc_rect area = pc_document_bounds(sd);
    const pc_document *dd = pc_doc_arc_get(docs[di].doc);
    if (dd->size.width != sd->size.width || dd->size.height != sd->size.height) {
        return pc_engine_err_bad_params(err, cmd, "the destination document must have the same size");
    }
    float *sel = NULL;
    size_t n = 0;
    pc_status rc = pc_algo_selection_mask_from_surface(sd->has_selection ? &sd->selection : NULL, area,
                                                       &sel, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    if (sel == NULL) { /* empty area */
        sel = calloc(npx ? npx : 1, sizeof(float));
        if (sel == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "selection");
        }
        n = npx;
    }
    struct save_ctx c = {cmd, sel, n, area, {0}, pc_json_get(p, "channel"), NULL, 0};
    (void)snprintf(c.op, sizeof c.op, "%.*s", (int)oplen, op);
    size_t nmlen = 0;
    const char *nm = get_str(p, "name", &nmlen);
    if (nm != NULL) {
        /* trim */
        while (nmlen > 0 && (nm[0] == ' ' || nm[0] == '\t')) {
            nm++;
            nmlen--;
        }
        while (nmlen > 0 && (nm[nmlen - 1] == ' ' || nm[nmlen - 1] == '\t')) {
            nmlen--;
        }
        if (nmlen > 0) {
            c.name = malloc(nmlen + 1);
            if (c.name == NULL) {
                free(sel);
                return pc_fail(err, PC_ERR_NOMEM, "name");
            }
            memcpy(c.name, nm, nmlen);
            c.name[nmlen] = '\0';
        }
    }
    size_t prev;
    bool had_prev = pc_engine_session_active_index(s, &prev);
    pc_engine_session_set_active(s, di);
    rc = pc_engine_session_edit(s, "Save Selection", save_edit, &c, err);
    if (had_prev) {
        pc_engine_session_set_active(s, prev);
    }
    free(sel);
    if (rc != PC_OK) {
        free(c.name);
        return rc;
    }
    /* Read the channel name back from the destination document. */
    char name_out[256] = {0};
    const pc_engine_doc_state *dst = &docs[di];
    const pc_document *dsd = pc_doc_arc_get(dst->doc);
    if (c.out_index < dsd->nchannels) {
        (void)snprintf(name_out, sizeof name_out, "%s", pc_str_c(&dsd->channels[c.out_index].name));
    }
    free(c.name);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "channel", c.out_index);
    yyjson_mut_obj_add_strcpy(d, root, "name", name_out);
    yyjson_mut_obj_add_uint(d, root, "document", di);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Load Selection ---------------------------------------------------------------------- */

struct load_ctx {
    float *plane;
    size_t n;
    pc_rect area;
    pc_algo_selection_mode mode;
    bool out_selected;
};

static pc_status load_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                           pc_error *err)
{
    (void)has_active;
    (void)active;
    struct load_ctx *c = vctx;
    bool some = false;
    pc_status rc = combine_into_doc(doc, c->plane, c->n, c->area, c->mode, &some, err);
    if (rc == PC_OK) {
        c->out_selected = some;
    }
    return rc;
}

/* Derive a chan_ref from the Channels-panel target (for channel == "targeted"). */
static bool targeted_ref(const pc_engine_doc_state *st, chan_ref *out)
{
    const struct pc_engine_channel_view *v = st->channel_view;
    int kind = v != NULL ? v->target_kind : CT_COMPOSITE;
    switch (kind) {
    case CT_COLOR:
        out->kind = REF_COLOR;
        out->index = v->target_index;
        return true;
    case CT_ALPHA:
        out->kind = REF_ALPHA;
        out->index = v->target_index;
        return true;
    case CT_LAYER_MASK:
        out->kind = REF_LAYER_MASK;
        return true;
    case CT_VECTOR_MASK:
        out->kind = REF_VECTOR_MASK;
        return true;
    default:
        out->kind = REF_COMPOSITE;
        return true;
    }
}

static pc_status run_load_selection(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    const char *cmd = "select.loadSelection";
    size_t oplen = 0;
    const char *op = get_str(p, "operation", &oplen);
    pc_algo_selection_mode mode;
    if (op == NULL || str_is(op, oplen, "new") || str_is(op, oplen, "replace")) {
        mode = PC_ALGO_SELECTION_REPLACE;
    } else if (str_is(op, oplen, "add")) {
        mode = PC_ALGO_SELECTION_ADD;
    } else if (str_is(op, oplen, "subtract")) {
        mode = PC_ALGO_SELECTION_SUBTRACT;
    } else if (str_is(op, oplen, "intersect")) {
        mode = PC_ALGO_SELECTION_INTERSECT;
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown operation `%.*s`", (int)oplen, op);
    }
    yyjson_val *cv = pc_json_get(p, "channel");
    if (cv == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `channel`");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect area = pc_document_bounds(doc);

    float *plane = NULL;
    size_t n = 0;
    chan_ref label_ref = {REF_COMPOSITE, 0};
    size_t cvlen = 0;
    const char *cvs = pc_json_as_str(cv, &cvlen);
    bool targeted = cvs != NULL && str_is(cvs, cvlen, "targeted");
    if (targeted) {
        if (!targeted_ref(d, &label_ref)) {
            return pc_engine_err_bad_params(err, cmd, "no targeted channel");
        }
        pc_status rc = gray_plane(doc, false, 0, d->has_active_layer, d->active_layer, label_ref, cmd,
                                  &plane, &n, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (get_bool(p, "invert", false)) {
            for (size_t i = 0; i < n; i++) {
                plane[i] = 1.0f - plane[i];
            }
        }
    } else {
        plane_list pl = {0};
        pc_status rc = source_planes(s, p, cmd, true, &pl, err);
        if (rc != PC_OK) {
            return rc;
        }
        plane = pl.planes[0];
        n = pl.n;
        free(pl.planes); /* keep plane[0] */
        (void)parse_ref(cv, doc, &label_ref);
    }
    const char *label = "Load Selection";
    if (label_ref.kind == REF_TRANSPARENCY) {
        label = "Load Transparency";
    } else if (label_ref.kind == REF_LAYER_MASK) {
        label = "Load Layer Mask";
    }
    struct load_ctx c = {plane, n, area, mode, false};
    pc_status rc = pc_engine_session_edit(s, label, load_edit, &c, err);
    free(plane);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "selected", c.out_selected);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_load_luminosity(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    (void)p;
    /* ⌘⌥2: load the composite's luminosity. */
    yyjson_doc *jd = yyjson_read("{\"channel\":\"composite\"}", 24, 0);
    if (jd == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "params");
    }
    pc_status rc = run_load_selection(s, yyjson_doc_get_root(jd), out, err);
    yyjson_doc_free(jd);
    return rc;
}

/* ---- Quick Mask -------------------------------------------------------------------------- */

/* Every pixel of vals reads as target on the 8-bit grid. */
static bool uniform_plane(const float *vals, size_t n, float target)
{
    for (size_t i = 0; i < n; i++) {
        if (fabsf(vals[i] - target) > 0.5f / 255.0f) {
            return false;
        }
    }
    return true;
}

/* The colourless, nothing-selected Quick Mask for `indicates`. out initialised. */
static pc_status unselected_surface(pc_rect area, pc_pixel_format fmt, pc_color_indicates indicates,
                                    pc_surface *out, pc_error *err)
{
    pc_surface s;
    pc_surface_init(&s, fmt);
    if (indicates != PC_INDICATES_SELECTED_AREAS) {
        float one = 1.0f;
        pc_status rc = pc_surface_fill_rect(&s, area, &one, 1, err);
        if (rc != PC_OK) {
            pc_surface_fini(&s);
            return rc;
        }
    }
    *out = s;
    return PC_OK;
}

/* Build the Quick Mask channel for the current selection. out (AlphaChannel) initialised. */
static pc_status quick_mask_from(const pc_document *doc, pc_color color, float opacity,
                                 pc_color_indicates indicates, pc_alpha_channel *out, pc_error *err)
{
    pc_rect area = pc_document_bounds(doc);
    pc_pixel_format fmt = channel_format(doc);
    pc_surface surf;
    if (doc->has_selection) {
        float *m = NULL;
        size_t n = 0;
        pc_status rc = pc_algo_selection_mask_from_surface(&doc->selection, area, &m, &n, err);
        if (rc != PC_OK) {
            return rc;
        }
        size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
        if (m == NULL) {
            m = calloc(npx ? npx : 1, sizeof(float));
            if (m == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "quick mask");
            }
            n = npx;
        }
        rc = plane_surface(m, n, area, fmt, &surf, err);
        free(m);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        pc_status rc = unselected_surface(area, fmt, indicates, &surf, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_status rc = pc_alpha_channel_init(out, "Quick Mask", &surf, err);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    out->color = color;
    out->opacity = opacity;
    out->indicates = indicates;
    return PC_OK;
}

/* The selection a Quick Mask channel turns back into: *some false = no selection. */
static pc_status selection_from_quick(const pc_document *doc, const pc_alpha_channel *ch,
                                      pc_surface *out, bool *some, pc_error *err)
{
    pc_rect area = pc_document_bounds(doc);
    float *vals = NULL;
    size_t n = 0;
    pc_status rc = read_plane(&ch->surface, area, &vals, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (vals == NULL) {
        vals = calloc(1, sizeof(float));
        if (vals == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "quick mask");
        }
        n = 0;
    }
    if (doc->quick_mask_unselected && ch->indicates != PC_INDICATES_SELECTED_AREAS &&
        uniform_plane(vals, n, 1.0f)) {
        free(vals);
        *some = false;
        return PC_OK;
    }
    rc = pc_algo_selection_combine(NULL, vals, n, area, PC_ALGO_SELECTION_REPLACE, out, some, err);
    free(vals);
    return rc;
}

struct qm_ctx {
    bool enter;
    pc_color color;
    float opacity;
    pc_color_indicates indicates;
};

static pc_status qm_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                         pc_error *err)
{
    (void)has_active;
    (void)active;
    struct qm_ctx *c = vctx;
    if (c->enter) {
        pc_alpha_channel q;
        pc_status rc = quick_mask_from(doc, c->color, c->opacity, c->indicates, &q, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (doc->has_quick_mask) {
            pc_alpha_channel_fini(&doc->quick_mask);
        }
        doc->quick_mask = q;
        doc->has_quick_mask = true;
        doc->quick_mask_unselected = !doc->has_selection;
        if (doc->has_selection) {
            pc_surface_fini(&doc->selection);
            doc->has_selection = false;
        }
        return PC_OK;
    }
    if (doc->has_quick_mask) {
        pc_alpha_channel q = doc->quick_mask;
        doc->has_quick_mask = false;
        pc_surface sel;
        bool some = false;
        pc_status rc = selection_from_quick(doc, &q, &sel, &some, err);
        pc_alpha_channel_fini(&q);
        if (rc != PC_OK) {
            return rc;
        }
        if (doc->has_selection) {
            pc_surface_fini(&doc->selection);
            doc->has_selection = false;
        }
        if (some) {
            doc->selection = sel;
            doc->has_selection = true;
        } else {
            pc_surface_fini(&sel);
        }
    }
    return PC_OK;
}

static pc_status run_quick_mask(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    bool on = pc_doc_arc_get(d->doc)->has_quick_mask;
    bool want = on;
    bool wb;
    yyjson_val *ov = pc_json_get(p, "on");
    if (ov != NULL && pc_json_as_bool(ov, &wb)) {
        want = wb;
    } else {
        want = !on;
    }
    if (want == on) {
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_bool(rd, root, "quickMask", on);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    struct qm_ctx c = {want, s->quick_mask_color, s->quick_mask_opacity, s->quick_mask_indicates};
    pc_status rc = pc_engine_session_edit(s, want ? "Edit in Quick Mask" : "Exit Quick Mask", qm_edit,
                                          &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (want) {
        pc_engine_doc_state *st = pc_engine_session_active_mut(s);
        if (st != NULL) {
            struct pc_engine_channel_view *v = cv_ensure(st);
            if (v != NULL) {
                v->target_kind = CT_COMPOSITE;
                v->quick_mask_hidden = false;
            }
        }
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, root, "quickMask", want);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}
