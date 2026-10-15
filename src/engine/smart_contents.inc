/* Layer > Smart Objects contents commands (port of crates/engine/src/smart_cmds.rs, ET4): Export
 * Contents, Replace Contents, Relink to File, Update (All) Modified Content, Convert to
 * Embedded / Linked. Included by smart_cmds_run.c (reuses its statics: detach_psd, smart_mut,
 * active_smart, pc_engine_layer_param).
 *
 * These swap the shared contents of every instance of a smart object (set_shared_source), keeping
 * each instance's transform and filters, and re-render. Source bytes / embedded-linked lookup go
 * through pc_engine_smart_source_bytes; file I/O through pc_engine_read_file / write_file / import.
 *
 * DEFERRED (reported to the coordinator, §7): Edit Contents / Save Contents need the Session
 * Edit-Contents document links (Session::smart_links) + multi-document lifecycle (add_document /
 * commit_child / on_close auto-commit); Update Changed Linked Content needs crate::link_watch (not
 * ported); Convert to Layers needs the smart-contents unpack (unpack.rs: transform_layer + the
 * layer-depth converter). Those remain unregistered (owners noted).
 *
 * §7: set_shared_source does not invalidate open Edit-Contents editor links (there are none until
 * editContents lands); refresh_or_fail treats a Stack-Mode smart object as unavailable (its render
 * keeps the cache, so a relink/convert of a stack-mode SO errors), a rare edge the deferred stack
 * combine will resolve. */

#include "base/buf.h"
/* pc_engine_read_file / write_file / import are declared in engine_internal.h (already included). */

/* The file name part of a path (after the last / or \\). */
static const char *cn_base_name(const char *path, size_t len, size_t *out_len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/' || path[i] == '\\') {
            start = i + 1;
        }
    }
    *out_len = len - start;
    return path + start;
}

/* The active layer's smart object (const) for layer `id`. */
static pc_status cn_smart_const(const pc_document *doc, pc_layer_id id, const pc_smart_object **out,
                                pc_error *err)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SMART) {
        return pc_engine_err_other(err, "the layer is not a smart object");
    }
    *out = &l->content.u.smart;
    return PC_OK;
}

static pc_status cn_has_linked(const pc_engine_session *s, pc_error *reason)
{
    const pc_smart_object *sm = NULL;
    pc_status rc = active_smart(s, &sm, reason);
    if (rc != PC_OK) {
        return rc;
    }
    return sm->source.linked ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "the smart object is embedded");
}

/* The required `path` string param. */
static pc_status cn_path_param(const char *cmd, yyjson_val *p, const char **out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "path");
    size_t l = 0;
    const char *str = (v != NULL) ? pc_json_as_str(v, &l) : NULL;
    if (str == NULL) {
        return pc_engine_err_bad_params(err, cmd, "pass `path` (a file path)");
    }
    *out = str;
    return PC_OK;
}

static void cn_source_fini(pc_smart_source *s)
{
    pc_str_fini(&s->file_name);
    pc_str_fini(&s->path);
    pc_blob_release(s->bytes);
    memset(s, 0, sizeof *s);
}

static pc_status cn_source_clone(const pc_smart_source *s, pc_smart_source *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->linked = s->linked;
    if (pc_str_clone(&s->file_name, &out->file_name, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (pc_str_clone(&s->path, &out->path, err) != PC_OK) {
        pc_str_fini(&out->file_name);
        return PC_ERR_NOMEM;
    }
    out->bytes = pc_blob_retain(s->bytes);
    return PC_OK;
}

/* Re-render smart object `id`, failing if its contents can't be read (refresh_or_fail). */
static pc_status cn_refresh_or_fail(pc_document *doc, pc_layer_id id, pc_error *err)
{
    bool updated = false;
    pc_status rc = pc_engine_smart_refresh(doc, id, &updated, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!updated) {
        return pc_engine_err_other(
            err, "the smart object's contents are unavailable (missing linked file?), so it can't be re-rendered");
    }
    return PC_OK;
}

/* The layer ids of every instance sharing `cid` (instances). *out owned (free). */
static pc_status cn_instances(const pc_document *doc, pc_smart_contents_id cid, pc_layer_id **out, size_t *n,
                              pc_error *err)
{
    pc_walk w = {0};
    pc_status rc = pc_document_walk(doc, &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id *ids = NULL;
    size_t cnt = 0, cap = 0;
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.contents_id == cid) {
            if (cnt == cap) {
                size_t nc = cap > 0 ? cap * 2 : 8;
                pc_layer_id *np = realloc(ids, nc * sizeof *np);
                if (np == NULL) {
                    free(ids);
                    pc_walk_fini(&w);
                    return pc_fail(err, PC_ERR_NOMEM, "instances");
                }
                ids = np;
                cap = nc;
            }
            ids[cnt++] = l->id;
        }
    }
    pc_walk_fini(&w);
    *out = ids;
    *n = cnt;
    return PC_OK;
}

/* set_shared_source transaction: swap the source of every instance, re-render, one undo step. */
struct cn_set_ctx {
    pc_layer_id id;
    const pc_smart_source *src;
    bool keep_psd;
};

static pc_status cn_set_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *err)
{
    (void)ha;
    (void)a;
    struct cn_set_ctx *c = v;
    pc_smart_object *sm0 = smart_mut(doc, c->id, err);
    if (sm0 == NULL) {
        return PC_ERR_INVALID;
    }
    pc_smart_contents_id cid = sm0->contents_id;
    pc_layer_id *ids = NULL;
    size_t n = 0;
    pc_status rc = cn_instances(doc, cid, &ids, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < n; i++) {
        pc_smart_object *sm = smart_mut(doc, ids[i], err);
        if (sm == NULL) {
            rc = PC_ERR_INVALID;
            break;
        }
        pc_smart_source clone;
        if ((rc = cn_source_clone(c->src, &clone, err)) != PC_OK) {
            break;
        }
        cn_source_fini(&sm->source);
        sm->source = clone;
        if (!c->keep_psd) {
            pc_layer *l = pc_document_layer_mut(doc, ids[i]);
            if (l != NULL) {
                detach_psd(l);
            }
        }
        if ((rc = cn_refresh_or_fail(doc, ids[i], err)) != PC_OK) {
            break;
        }
    }
    free(ids);
    return rc;
}

/* Runs the transaction and frees `new_src` (moved in). */
static pc_status cn_set_shared_source(pc_engine_session *s, pc_layer_id id, const char *label, bool keep_psd,
                                      pc_smart_source *new_src, pc_error *err)
{
    struct cn_set_ctx c = {id, new_src, keep_psd};
    pc_status rc = pc_engine_session_edit(s, label, cn_set_cb, &c, err);
    cn_source_fini(new_src);
    return rc;
}

static pc_status cn_result_layer(pc_json_value *out, pc_layer_id id, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", (uint64_t)id);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Export Contents -------------------------------------------------------------------- */

static pc_status cn_export_contents(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.smartObjects.exportContents";
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *path = NULL;
    if ((rc = cn_path_param(CMD, p, &path, err)) != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_smart_object *sm = NULL;
    if ((rc = cn_smart_const(doc, id, &sm, err)) != PC_OK) {
        return rc;
    }
    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    if ((rc = pc_engine_smart_source_bytes(&doc->metadata, &sm->source, &found, &name, &bytes, err)) != PC_OK) {
        return rc;
    }
    if (!found) {
        return pc_engine_err_other(err, "the smart object's contents are unavailable");
    }
    rc = pc_engine_write_file(path, pc_blob_data(bytes), pc_blob_len(bytes), err);
    if (rc != PC_OK) {
        pc_str_fini(&name);
        pc_blob_release(bytes);
        return rc;
    }
    yyjson_mut_doc *dj = NULL;
    if (pc_engine_result_doc(out, &dj, err) != PC_OK) {
        pc_str_fini(&name);
        pc_blob_release(bytes);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(dj);
    yyjson_mut_obj_add_strcpy(dj, root, "path", path);
    yyjson_mut_obj_add_strcpy(dj, root, "fileName", pc_str_c(&name));
    yyjson_mut_obj_add_uint(dj, root, "bytes", (uint64_t)pc_blob_len(bytes));
    yyjson_mut_doc_set_root(dj, root);
    pc_str_fini(&name);
    pc_blob_release(bytes);
    return PC_OK;
}

/* ---- Replace Contents ------------------------------------------------------------------- */

static pc_status cn_replace_contents(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.smartObjects.replaceContents";
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *path = NULL;
    if ((rc = cn_path_param(CMD, p, &path, err)) != PC_OK) {
        return rc;
    }
    pc_buf buf = PC_BUF_INIT;
    if ((rc = pc_engine_read_file(path, &buf, err)) != PC_OK) {
        return rc;
    }
    size_t blen = 0;
    const char *bn = cn_base_name(path, strlen(path), &blen);
    /* Validate the contents before touching the document. */
    pc_document tmp = {0};
    rc = pc_engine_import(bn, buf.data, buf.len, &tmp, err);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return rc;
    }
    pc_document_fini(&tmp);
    pc_smart_source new_src;
    memset(&new_src, 0, sizeof new_src);
    new_src.linked = false;
    if ((rc = pc_str_set(&new_src.file_name, bn, blen, err)) != PC_OK ||
        (rc = pc_blob_new(buf.data, buf.len, &new_src.bytes, err)) != PC_OK) {
        cn_source_fini(&new_src);
        pc_buf_fini(&buf);
        return rc;
    }
    pc_buf_fini(&buf);
    rc = cn_set_shared_source(s, id, "Replace Contents", false, &new_src, err);
    if (rc != PC_OK) {
        return rc;
    }
    return cn_result_layer(out, id, err);
}

/* ---- Relink to File --------------------------------------------------------------------- */

static pc_status cn_relink(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.smartObjects.relinkToFile";
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *path = NULL;
    if ((rc = cn_path_param(CMD, p, &path, err)) != PC_OK) {
        return rc;
    }
    pc_smart_source new_src;
    memset(&new_src, 0, sizeof new_src);
    new_src.linked = true;
    if ((rc = pc_str_set(&new_src.path, path, strlen(path), err)) != PC_OK) {
        return rc;
    }
    rc = cn_set_shared_source(s, id, "Relink to File", false, &new_src, err);
    if (rc != PC_OK) {
        return rc;
    }
    return cn_result_layer(out, id, err);
}

/* ---- Convert to Embedded ---------------------------------------------------------------- */

static pc_status cn_convert_embedded(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
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
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_smart_object *sm = NULL;
    if ((rc = cn_smart_const(doc, id, &sm, err)) != PC_OK) {
        return rc;
    }
    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    if ((rc = pc_engine_smart_source_bytes(&doc->metadata, &sm->source, &found, &name, &bytes, err)) != PC_OK) {
        return rc;
    }
    if (!found) {
        return pc_engine_err_other(err, "the linked file can't be read");
    }
    pc_smart_source new_src;
    memset(&new_src, 0, sizeof new_src);
    new_src.linked = false;
    new_src.file_name = name; /* moved */
    new_src.bytes = bytes;    /* moved (already retained) */
    rc = cn_set_shared_source(s, id, "Convert to Embedded", true, &new_src, err);
    if (rc != PC_OK) {
        return rc;
    }
    return cn_result_layer(out, id, err);
}

/* ---- Convert to Linked ------------------------------------------------------------------ */

static pc_status cn_convert_linked(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "layer.smartObjects.convertToLinked";
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *path = NULL;
    if ((rc = cn_path_param(CMD, p, &path, err)) != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_smart_object *sm = NULL;
    if ((rc = cn_smart_const(doc, id, &sm, err)) != PC_OK) {
        return rc;
    }
    bool found = false;
    pc_str name = PC_STR_INIT;
    pc_blob *bytes = NULL;
    if ((rc = pc_engine_smart_source_bytes(&doc->metadata, &sm->source, &found, &name, &bytes, err)) != PC_OK) {
        return rc;
    }
    if (!found) {
        pc_str_fini(&name);
        pc_blob_release(bytes);
        return pc_engine_err_other(err, "the smart object's contents are unavailable");
    }
    rc = pc_engine_write_file(path, pc_blob_data(bytes), pc_blob_len(bytes), err);
    pc_str_fini(&name);
    pc_blob_release(bytes);
    if (rc != PC_OK) {
        return rc;
    }
    pc_smart_source new_src;
    memset(&new_src, 0, sizeof new_src);
    new_src.linked = true;
    if ((rc = pc_str_set(&new_src.path, path, strlen(path), err)) != PC_OK) {
        return rc;
    }
    rc = cn_set_shared_source(s, id, "Convert to Linked", false, &new_src, err);
    if (rc != PC_OK) {
        return rc;
    }
    return cn_result_layer(out, id, err);
}

/* ---- Update Modified Content ------------------------------------------------------------ */

struct cn_update_ctx {
    pc_layer_id id;
};

static pc_status cn_update_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *err)
{
    (void)ha;
    (void)a;
    struct cn_update_ctx *c = v;
    return cn_refresh_or_fail(doc, c->id, err);
}

static pc_status cn_update_modified(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct cn_update_ctx c = {id};
    rc = pc_engine_session_edit(s, "Update Modified Content", cn_update_cb, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return cn_result_layer(out, id, err);
}

/* ---- Update All Modified Content -------------------------------------------------------- */

struct cn_update_all_ctx {
    const pc_layer_id *ids;
    size_t n;
    pc_layer_id *updated;
    size_t nupdated;
};

static pc_status cn_update_all_cb(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *err)
{
    (void)ha;
    (void)a;
    struct cn_update_all_ctx *c = v;
    for (size_t i = 0; i < c->n; i++) {
        bool updated = false;
        pc_status rc = pc_engine_smart_refresh(doc, c->ids[i], &updated, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (updated) {
            c->updated[c->nupdated++] = c->ids[i];
        }
    }
    return PC_OK;
}

static pc_status cn_update_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    /* Every linked smart object in the document. */
    pc_walk w = {0};
    pc_status rc = pc_document_walk(pc_doc_arc_get(d->doc), &w, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id *ids = NULL;
    size_t n = 0, cap = 0;
    for (size_t i = 0; i < w.len; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->content.kind == PC_LAYER_SMART && l->content.u.smart.source.linked) {
            if (n == cap) {
                size_t nc = cap > 0 ? cap * 2 : 8;
                pc_layer_id *np = realloc(ids, nc * sizeof *np);
                if (np == NULL) {
                    free(ids);
                    pc_walk_fini(&w);
                    return pc_fail(err, PC_ERR_NOMEM, "linked");
                }
                ids = np;
                cap = nc;
            }
            ids[n++] = l->id;
        }
    }
    pc_walk_fini(&w);
    struct cn_update_all_ctx c = {ids, n, NULL, 0};
    if (n > 0) {
        c.updated = malloc(n * sizeof *c.updated);
        if (c.updated == NULL) {
            free(ids);
            return pc_fail(err, PC_ERR_NOMEM, "updated");
        }
    }
    rc = pc_engine_session_edit(s, "Update All Modified Content", cn_update_all_cb, &c, err);
    free(ids);
    if (rc != PC_OK) {
        free(c.updated);
        return rc;
    }
    yyjson_mut_doc *dj = NULL;
    if (pc_engine_result_doc(out, &dj, err) != PC_OK) {
        free(c.updated);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(dj);
    yyjson_mut_val *arr = yyjson_mut_arr(dj);
    for (size_t i = 0; i < c.nupdated; i++) {
        yyjson_mut_arr_add_uint(dj, arr, (uint64_t)c.updated[i]);
    }
    yyjson_mut_obj_add_val(dj, root, "updated", arr);
    yyjson_mut_doc_set_root(dj, root);
    free(c.updated);
    return PC_OK;
}
