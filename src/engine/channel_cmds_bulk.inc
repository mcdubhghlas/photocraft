/* Merge Spot Channel, Split/Merge Channels, and the ⌘2..⌘9 channel slots (included by
 * channel_cmds.c, via channel_cmds_manage.inc). */

#if defined(__APPLE__)
#define SLOT_KEY "\xe2\x8c\x98" /* U+2318 ⌘ */
#else
#define SLOT_KEY "Ctrl+"
#endif

/* ---- Merge Spot Channel ------------------------------------------------------------------ */

struct mergespot_ctx {
    size_t index;
    float ink[3];
    float solidity;
};

static pc_status mergespot_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                pc_error *err)
{
    struct mergespot_ctx *c = vctx;
    pc_rect area = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t n = pc_pixel_format_channels(fmt);
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    pc_compose_buffer comp;
    pc_status rc = pc_compose_render(doc, area, &comp, err);
    if (rc != PC_OK) {
        return rc;
    }
    float *k = NULL;
    size_t kn = 0;
    rc = read_plane(&doc->channels[c->index].surface, area, &k, &kn, err);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&comp);
        return rc;
    }
    float *data = malloc(npx ? npx * n * sizeof(float) : 1);
    if (data == NULL) {
        free(k);
        pc_compose_buffer_fini(&comp);
        return pc_fail(err, PC_ERR_NOMEM, "merge spot");
    }
    for (size_t i = 0; i < npx; i++) {
        const float *cp = (i < comp.len) ? comp.px[i] : (const float[]){0, 0, 0, 1};
        float v = (k != NULL && i < kn) ? k[i] : 0.0f;
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        float rgb[3];
        for (int j = 0; j < 3; j++) {
            float over = c->solidity * c->ink[j] + (1.0f - c->solidity) * cp[j] * c->ink[j];
            rgb[j] = cp[j] + (over - cp[j]) * v;
        }
        float rgba[4] = {rgb[0], rgb[1], rgb[2], 1.0f};
        (void)pc_from_rgba(fmt, rgba, &data[i * n]);
    }
    free(k);
    pc_compose_buffer_fini(&comp);
    pc_layer bg;
    rc = pc_layer_raster(&bg, "Background", fmt, err);
    if (rc != PC_OK) {
        free(data);
        return rc;
    }
    bg.locks.transparency = true;
    bg.locks.position = true;
    pc_surface *sf = pc_layer_surface_mut(&bg);
    if (sf != NULL) {
        rc = pc_surface_write_region(sf, area, data, npx * n, err);
        if (rc == PC_OK) {
            pc_surface_prune(sf);
        }
    }
    free(data);
    if (rc != PC_OK) {
        pc_layer_fini(&bg);
        return rc;
    }
    pc_layer_id id = bg.id;
    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer_fini(&doc->layers[i]);
    }
    free(doc->layers);
    doc->layers = malloc(sizeof(pc_layer));
    if (doc->layers == NULL) {
        pc_layer_fini(&bg);
        doc->nlayers = 0;
        return pc_fail(err, PC_ERR_NOMEM, "merge spot");
    }
    doc->layers[0] = bg;
    doc->nlayers = 1;
    pc_alpha_channel_fini(&doc->channels[c->index]);
    memmove(&doc->channels[c->index], &doc->channels[c->index + 1],
            (doc->nchannels - c->index - 1) * sizeof(pc_alpha_channel));
    doc->nchannels--;
    *active = id;
    *ha = true;
    return PC_OK;
}

static pc_status run_merge_spot(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "channel.mergeSpot";
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
        bool found = false;
        for (size_t j = 0; j < doc->nchannels; j++) {
            if (doc->channels[j].has_spot) {
                i = j;
                found = true;
                break;
            }
        }
        if (!found) {
            return pc_engine_err_bad_params(err, cmd, "no spot channel");
        }
    }
    if (i >= doc->nchannels || !doc->channels[i].has_spot) {
        return pc_engine_err_bad_params(err, cmd, "not a spot channel");
    }
    struct mergespot_ctx c = {i, {0, 0, 0}, doc->channels[i].spot_solidity};
    pc_color_to_rgb(&doc->channels[i].spot_color, c.ink);
    pc_status rc = pc_engine_session_edit(s, "Merge Spot Channel", mergespot_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        pc_engine_channel_view_free(st->channel_view);
        st->channel_view = NULL;
        fix_view_now(st);
    }
    return pc_engine_result_null(out, err);
}

/* ---- Split Channels ---------------------------------------------------------------------- */

static pc_status run_split(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t idx;
    if (!pc_engine_session_active_index(s, &idx)) {
        return pc_engine_err_no_document(err);
    }
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndocs);
    pc_doc_arc *arc = pc_doc_arc_retain(docs[idx].doc);
    const pc_document *doc = pc_doc_arc_get(arc);
    pc_rect area = pc_document_bounds(doc);
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    float *px = NULL;
    size_t pxn = 0, nch = 0;
    pc_status rc = native_pixels(doc, false, 0, &px, &pxn, &nch, err);
    if (rc != PC_OK) {
        pc_doc_arc_release(arc);
        return rc;
    }
    pc_color_mode mode = edit_mode(doc);
    size_t ncolors = 0;
    const char *const *names = color_names(mode, &ncolors);
    const char *docname = pc_str_c(&doc->name);

    size_t total = ncolors + doc->nchannels;
    pc_document *made = calloc(total ? total : 1, sizeof(pc_document));
    if (made == NULL) {
        free(px);
        pc_doc_arc_release(arc);
        return pc_fail(err, PC_ERR_NOMEM, "split");
    }
    size_t built = 0;
    float *plane = malloc(npx ? npx * sizeof(float) : 1);
    if (plane == NULL) {
        free(made);
        free(px);
        pc_doc_arc_release(arc);
        return pc_fail(err, PC_ERR_NOMEM, "split");
    }
    for (size_t c = 0; c < ncolors; c++) {
        for (size_t i = 0; i < npx; i++) {
            plane[i] = px[i * nch + c];
        }
        char nm[300];
        char first = names[c][0];
        if (first >= 'a' && first <= 'z') {
            first = (char)(first - 32);
        }
        (void)snprintf(nm, sizeof nm, "%s_%c", docname, first);
        rc = gray_document(nm, doc, plane, npx, &made[built], err);
        if (rc != PC_OK) {
            goto fail;
        }
        built++;
    }
    for (size_t j = 0; j < doc->nchannels; j++) {
        float *ap = NULL;
        size_t an = 0;
        rc = read_plane(&doc->channels[j].surface, area, &ap, &an, err);
        if (rc != PC_OK) {
            goto fail;
        }
        char nm[400];
        (void)snprintf(nm, sizeof nm, "%s_%s", docname, pc_str_c(&doc->channels[j].name));
        rc = gray_document(nm, doc, ap ? ap : plane, ap ? an : npx, &made[built], err);
        free(ap);
        if (rc != PC_OK) {
            goto fail;
        }
        built++;
    }
    free(plane);
    free(px);
    pc_doc_arc_release(arc);

    if (get_bool(p, "closeOriginal", true)) {
        pc_engine_session_close(s, idx);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        for (size_t i = 0; i < built; i++) {
            pc_document_fini(&made[i]);
        }
        free(made);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < built; i++) {
        size_t di;
        if (pc_engine_session_add_document(s, &made[i], NULL, &di, err) != PC_OK) {
            pc_document_fini(&made[i]);
            for (size_t k = i + 1; k < built; k++) {
                pc_document_fini(&made[k]);
            }
            free(made);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        yyjson_mut_arr_add_uint(rd, arr, di);
    }
    free(made);
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, root, "documents", arr);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;

fail:
    free(plane);
    free(px);
    for (size_t i = 0; i < built; i++) {
        pc_document_fini(&made[i]);
    }
    free(made);
    pc_doc_arc_release(arc);
    return rc;
}

/* ---- Merge Channels ---------------------------------------------------------------------- */

static pc_status run_merge(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "channel.merge";
    size_t mlen = 0;
    const char *ms = get_str(p, "mode", &mlen);
    pc_color_mode mode;
    if (ms == NULL || str_is(ms, mlen, "rgb")) {
        mode = PC_COLOR_MODE_RGB;
    } else if (str_is(ms, mlen, "cmyk")) {
        mode = PC_COLOR_MODE_CMYK;
    } else if (str_is(ms, mlen, "lab")) {
        mode = PC_COLOR_MODE_LAB;
    } else {
        return pc_engine_err_bad_params(err, cmd, "unknown mode `%.*s`", (int)mlen, ms);
    }
    size_t cc = pc_color_mode_channels(mode);
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &ndocs);

    size_t *srcs = NULL;
    size_t nsrcs = 0;
    yyjson_val *dv = pc_json_get(p, "documents");
    if (dv != NULL && yyjson_is_arr(dv)) {
        size_t len = yyjson_arr_size(dv);
        srcs = malloc(len ? len * sizeof(size_t) : 1);
        if (srcs == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "merge");
        }
        size_t i, max;
        yyjson_val *v;
        yyjson_arr_foreach(dv, i, max, v) {
            uint64_t u;
            if (!pc_json_as_u64(v, &u) || u >= ndocs) {
                free(srcs);
                return pc_engine_err_bad_params(err, cmd, "bad document index");
            }
            srcs[nsrcs++] = (size_t)u;
        }
    } else {
        size_t *m = NULL;
        size_t k = mergeable(s, &m);
        size_t take = k < cc ? k : cc;
        srcs = malloc(take ? take * sizeof(size_t) : 1);
        if (srcs == NULL) {
            free(m);
            return pc_fail(err, PC_ERR_NOMEM, "merge");
        }
        for (size_t i = 0; i < take; i++) {
            srcs[nsrcs++] = m[i];
        }
        free(m);
    }
    if (nsrcs < cc) {
        size_t got = nsrcs;
        free(srcs);
        return pc_engine_err_bad_params(err, cmd, "%s needs %zu grayscale documents, got %zu",
                                        pc_color_mode_name(mode), cc, got);
    }
    /* No repeated index. */
    for (size_t i = 0; i < nsrcs; i++) {
        for (size_t j = i + 1; j < nsrcs; j++) {
            if (srcs[i] == srcs[j]) {
                size_t dup = srcs[i];
                free(srcs);
                return pc_engine_err_bad_params(err, cmd, "document %zu is listed more than once", dup);
            }
        }
    }
    const pc_document *first = pc_doc_arc_get(docs[srcs[0]].doc);
    pc_size size = first->size;
    float dpi = first->resolution_dpi;
    pc_sample_type depth = first->depth;
    /* Planes from each source (grayscale composite). */
    size_t npx = (size_t)size.width * (size_t)size.height;
    float **planes = calloc(nsrcs, sizeof(float *));
    if (planes == NULL) {
        free(srcs);
        return pc_fail(err, PC_ERR_NOMEM, "merge");
    }
    pc_status rc = PC_OK;
    size_t got_planes = 0;
    for (size_t i = 0; i < nsrcs; i++) {
        const pc_document *sd = pc_doc_arc_get(docs[srcs[i]].doc);
        if (sd->size.width != size.width || sd->size.height != size.height ||
            sd->mode != PC_COLOR_MODE_GRAYSCALE) {
            rc = pc_engine_err_bad_params(err, cmd,
                                          "\"%s\" is not a grayscale document of the same size",
                                          pc_str_c(&sd->name));
            goto done;
        }
        chan_ref r = {REF_COMPOSITE, 0};
        size_t pn = 0;
        rc = gray_plane(sd, false, 0, false, 0, r, cmd, &planes[i], &pn, err);
        if (rc != PC_OK) {
            goto done;
        }
        got_planes++;
    }
    /* Build the output document. */
    pc_document outdoc;
    rc = pc_document_init(&outdoc, ms != NULL ? "Untitled" : "Untitled", size, mode, depth, err);
    {
        size_t nnlen = 0;
        const char *nn = get_str(p, "name", &nnlen);
        if (nn != NULL) {
            (void)pc_str_set(&outdoc.name, nn, nnlen, NULL);
        }
    }
    if (rc != PC_OK) {
        goto done;
    }
    outdoc.resolution_dpi = dpi;
    pc_pixel_format fmt = pc_document_pixel_format(&outdoc);
    size_t n = pc_pixel_format_channels(fmt);
    float *data = malloc(npx ? npx * n * sizeof(float) : 1);
    if (data == NULL) {
        pc_document_fini(&outdoc);
        rc = pc_fail(err, PC_ERR_NOMEM, "merge");
        goto done;
    }
    for (size_t i = 0; i < npx; i++) {
        for (size_t c = 0; c < cc; c++) {
            float v = planes[c][i];
            data[i * n + c] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        if (fmt.alpha) {
            data[i * n + cc] = 1.0f;
        }
    }
    pc_layer bg;
    rc = pc_layer_raster(&bg, "Background", fmt, err);
    if (rc != PC_OK) {
        free(data);
        pc_document_fini(&outdoc);
        goto done;
    }
    bg.locks.transparency = true;
    bg.locks.position = true;
    {
        pc_surface *sf = pc_layer_surface_mut(&bg);
        if (sf != NULL) {
            rc = pc_surface_write_region(sf, pc_document_bounds(&outdoc), data, npx * n, err);
            if (rc == PC_OK) {
                pc_surface_prune(sf);
            }
        }
    }
    free(data);
    if (rc != PC_OK) {
        pc_layer_fini(&bg);
        pc_document_fini(&outdoc);
        goto done;
    }
    {
        pc_layer *lg = realloc(outdoc.layers, (outdoc.nlayers + 1) * sizeof(pc_layer));
        if (lg == NULL) {
            pc_layer_fini(&bg);
            pc_document_fini(&outdoc);
            rc = pc_fail(err, PC_ERR_NOMEM, "merge");
            goto done;
        }
        outdoc.layers = lg;
        outdoc.layers[outdoc.nlayers] = bg;
        outdoc.nlayers++;
    }
    /* Extra planes become alpha channels. */
    {
        pc_rect area = pc_document_bounds(&outdoc);
        pc_pixel_format cf = channel_format(&outdoc);
        for (size_t k = cc; k < nsrcs; k++) {
            pc_surface surf;
            rc = plane_surface(planes[k], npx, area, cf, &surf, err);
            if (rc != PC_OK) {
                pc_document_fini(&outdoc);
                goto done;
            }
            char nm[32];
            (void)snprintf(nm, sizeof nm, "Alpha %zu", k - cc + 1);
            pc_alpha_channel ch;
            rc = pc_alpha_channel_init(&ch, nm, &surf, err);
            if (rc != PC_OK) {
                pc_surface_fini(&surf);
                pc_document_fini(&outdoc);
                goto done;
            }
            rc = append_channel(&outdoc, &ch, err);
            if (rc != PC_OK) {
                pc_alpha_channel_fini(&ch);
                pc_document_fini(&outdoc);
                goto done;
            }
        }
    }
    /* Close the sources (highest index first), then add the result. */
    for (size_t i = 0; i < got_planes; i++) {
        free(planes[i]);
    }
    free(planes);
    planes = NULL;
    {
        size_t sorted[32];
        size_t ns = nsrcs <= 32 ? nsrcs : 32;
        for (size_t i = 0; i < ns; i++) {
            sorted[i] = srcs[i];
        }
        for (size_t i = 0; i < ns; i++) {
            for (size_t j = i + 1; j < ns; j++) {
                if (sorted[j] > sorted[i]) {
                    size_t t = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = t;
                }
            }
        }
        for (size_t i = 0; i < ns; i++) {
            pc_engine_session_close(s, sorted[i]);
        }
    }
    free(srcs);
    srcs = NULL;
    size_t di;
    rc = pc_engine_session_add_document(s, &outdoc, NULL, &di, err);
    if (rc != PC_OK) {
        pc_document_fini(&outdoc);
        return rc;
    }
    {
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "document", di);
        yyjson_mut_doc_set_root(rd, root);
    }
    return PC_OK;

done:
    if (planes != NULL) {
        for (size_t i = 0; i < got_planes; i++) {
            free(planes[i]);
        }
        free(planes);
    }
    free(srcs);
    return rc;
}

/* ---- Channel slots (⌘2..⌘9) -------------------------------------------------------------- */

static pc_status target_slot(pc_engine_session *s, size_t slot, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t colors = pc_engine_color_count(doc);
    size_t shown = colors > 1 ? colors : 0;
    int kind;
    size_t index = 0;
    if (slot == 2) {
        kind = CT_COMPOSITE;
    } else if (slot >= 3 && slot - 3 < shown) {
        kind = CT_COLOR;
        index = slot - 3;
    } else if (slot >= 3 && slot - 3 - shown < doc->nchannels) {
        kind = CT_ALPHA;
        index = slot - 3 - shown;
    } else {
        return pc_engine_err_other(err, "no channel for %s%zu", SLOT_KEY, slot);
    }
    pc_status rc = set_target(s, kind, index, err);
    if (rc != PC_OK) {
        return rc;
    }
    return channels_result(s, out, err);
}

static pc_status slot_ok_n(const pc_engine_session *s, size_t n, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t colors = pc_engine_color_count(doc);
    size_t shown = colors > 1 ? colors : 0;
    if (n - 3 < shown + doc->nchannels) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no channel for %s%zu", SLOT_KEY, n);
}

#define SLOT_FN(N)                                                                                   \
    static pc_status run_slot##N(pc_engine_session *s, yyjson_val *p, pc_json_value *out,             \
                                 pc_error *err)                                                       \
    {                                                                                                \
        (void)p;                                                                                     \
        return target_slot(s, N, out, err);                                                          \
    }                                                                                                \
    static pc_status slot_ok##N(const pc_engine_session *s, pc_error *reason)                         \
    {                                                                                                \
        return slot_ok_n(s, N, reason);                                                              \
    }

SLOT_FN(3)
SLOT_FN(4)
SLOT_FN(5)
SLOT_FN(6)
SLOT_FN(7)
SLOT_FN(8)
SLOT_FN(9)

static pc_status run_target_composite(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    (void)p;
    return target_slot(s, 2, out, err);
}
