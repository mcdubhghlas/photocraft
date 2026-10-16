/* Port of crates/automation/src/headless.rs and the Headless methods of rpc.rs. */
#include "automation/headless.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "automation/automation.h"
#include "automation/files.h"
#include "automation/security.h"
#include "base/base64.h"
#include "base/file.h"
#include "engine/type_extra_cmds.h" /* pc_engine_missing_font_warnings_for_layer, pc_strlist */
/* pc_doc_arc_get comes transitively from engine/engine.h (which includes ops/ops.h). */

const char *const pc_headless_methods[] = {
    "engine.execute", "jobs.list",         "jobs.cancel",  "engine.commands", "session.list",
    "doc.open",       "doc.place",         "doc.new",      "doc.save",        "doc.exportLayers",
    "doc.exportDataSets", "doc.importDataSets", "doc.inspect", "doc.render",    "doc.select",
    "doc.close",      "batch",             "methods",      NULL};

/* ---- construction ---- */

static pc_status headless_alloc(pc_auto_fs_mode fs, pc_headless **out, pc_error *err)
{
    pc_headless *h = calloc(1, sizeof *h);
    if (!h) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (pc_engine_session_new(&h->session, err) != PC_OK) {
        free(h);
        return err ? err->code : PC_ERR_NOMEM;
    }
    h->fs = fs;
    *out = h;
    return PC_OK;
}

pc_status pc_headless_new(pc_headless **out, pc_error *err)
{
    return headless_alloc(PC_AUTO_FS_DENIED, out, err);
}

pc_status pc_headless_trusted_local(pc_headless **out, pc_error *err)
{
    return headless_alloc(PC_AUTO_FS_TRUSTED_LOCAL, out, err);
}

pc_status pc_headless_with_workspace(pc_auto_workspace *ws, pc_headless **out, pc_error *err)
{
    pc_headless *h = NULL;
    if (headless_alloc(PC_AUTO_FS_WORKSPACE, &h, err) != PC_OK || !h) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    h->workspace = *ws;              /* move */
    memset(ws, 0, sizeof *ws);
    ws->read.fd = -1;
    ws->write.fd = -1;
    *out = h;
    return PC_OK;
}

void pc_headless_free(pc_headless *h)
{
    if (!h) {
        return;
    }
    for (size_t i = 0; i < h->nwriters; i++) {
        pc_pcraft_writer_free(h->writers[i].writer);
    }
    free(h->writers);
    if (h->fs == PC_AUTO_FS_WORKSPACE) {
        pc_auto_workspace_fini(&h->workspace);
    }
    pc_engine_session_free(h->session);
    free(h);
}

void pc_headless_sync_jobs(pc_headless *h)
{
    pc_engine_job_event *ev = NULL;
    size_t n = 0;
    if (pc_engine_session_poll_jobs(h->session, &ev, &n, NULL) == PC_OK) {
        pc_engine_job_events_free(ev, n);
    }
}

/* ---- writers map ---- */

static pc_pcraft_writer *writer_for(pc_headless *h, uint64_t doc_id, pc_error *err)
{
    for (size_t i = 0; i < h->nwriters; i++) {
        if (h->writers[i].doc_id == doc_id) {
            return h->writers[i].writer;
        }
    }
    if (h->nwriters == h->cap_writers) {
        size_t cap = h->cap_writers ? h->cap_writers * 2 : 4;
        pc_auto_writer_entry *grown = realloc(h->writers, cap * sizeof *grown);
        if (!grown) {
            (void)pc_fail(err, PC_ERR_NOMEM, "out of memory");
            return NULL;
        }
        h->writers = grown;
        h->cap_writers = cap;
    }
    pc_pcraft_writer *w = NULL;
    if (pc_pcraft_writer_new(&w, err) != PC_OK) {
        return NULL;
    }
    h->writers[h->nwriters].doc_id = doc_id;
    h->writers[h->nwriters].writer = w;
    h->nwriters++;
    return w;
}

static void writer_remove(pc_headless *h, uint64_t doc_id)
{
    for (size_t i = 0; i < h->nwriters; i++) {
        if (h->writers[i].doc_id == doc_id) {
            pc_pcraft_writer_free(h->writers[i].writer);
            h->writers[i] = h->writers[--h->nwriters];
            return;
        }
    }
}

/* ---- helpers ---- */

static pc_status doc_index(pc_headless *h, int64_t index, size_t *out, pc_error *err)
{
    size_t n = 0;
    (void)pc_engine_session_documents(h->session, &n);
    if (index >= 0) {
        if ((size_t)index < n) {
            *out = (size_t)index;
            return PC_OK;
        }
        return pc_auto_bad(err, "no document at index %" PRId64, index);
    }
    size_t active = 0;
    if (pc_engine_session_active_index(h->session, &active)) {
        *out = active;
        return PC_OK;
    }
    return pc_auto_bad(err, "no document open");
}

/* Wrap a built mut root into an owned normalized pc_json_value, freeing the scratch doc. */
static pc_status wrap(yyjson_mut_doc *doc, yyjson_mut_val *root, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc_set_root(doc, root);
    pc_status rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(doc);
    return rc;
}

/* Append the "font '<name>' is not installed; ..." warnings of a document to *list (deduped). */
static pc_status collect_missing_fonts(const pc_document *doc, pc_strlist *list, pc_error *err)
{
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    for (size_t i = 0; i < w.len && rc == PC_OK; i++) {
        const pc_layer *l = w.items[i].layer;
        if (l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        rc = pc_engine_missing_font_warnings_for_layer(&l->content.u.text, list, err);
    }
    pc_walk_fini(&w);
    return rc;
}

/* Add a JSON string array `key` to obj from pc_strlist (deduped by value). */
static void add_strlist_dedup(yyjson_mut_doc *doc, yyjson_mut_val *arr, const pc_strlist *list)
{
    for (size_t i = 0; i < list->n; i++) {
        bool seen = false;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(list->v[i], list->v[j]) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            yyjson_mut_arr_add_strcpy(doc, arr, list->v[i]);
        }
    }
}

/* ---- open ---- */

pc_status pc_headless_open(pc_headless *h, const char *path, pc_json_value *out, pc_error *err)
{
    pc_auto_opened o;
    pc_status rc;
    switch (h->fs) {
    case PC_AUTO_FS_DENIED:
        return pc_auto_bad(err, "automation filesystem access is not granted: read authority is absent");
    case PC_AUTO_FS_TRUSTED_LOCAL:
        rc = pc_auto_open_path(path, &o, err);
        break;
    case PC_AUTO_FS_WORKSPACE: {
        pc_buf bytes = PC_BUF_INIT;
        if (pc_auto_workspace_read(&h->workspace, path, &bytes, err) != PC_OK) {
            return err ? err->code : PC_ERR_IO;
        }
        /* base name for format sniffing */
        const char *name = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/') {
                name = p + 1;
            }
        }
        rc = pc_auto_open_bytes(name, bytes.data, bytes.len, &o, err);
        pc_buf_fini(&bytes);
        break;
    }
    default:
        return pc_auto_bad(err, "automation filesystem access is not granted");
    }
    if (rc != PC_OK) {
        return rc;
    }
    size_t index = 0;
    bool source_ro = o.source_read_only;
    rc = pc_engine_session_add_document(h->session, &o.document, source_ro ? NULL : path, &index, err);
    if (rc != PC_OK) {
        pc_auto_opened_fini(&o);
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(h->session);
    if (st) {
        st->source_read_only = source_ro;
    }
    /* Build the result. */
    const pc_engine_doc_state *d = &pc_engine_session_documents(h->session, &(size_t){0})[index];
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t nlayers = 0;
    (void)pc_document_layer_count(doc, &nlayers, NULL);
    yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
    if (!rdoc) {
        pc_io_warnings_fini(&o.warnings);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(rdoc);
    yyjson_mut_obj_add_uint(rdoc, root, "index", index);
    yyjson_mut_obj_add_strcpy(rdoc, root, "name", pc_str_c(&doc->name));
    yyjson_mut_obj_add_uint(rdoc, root, "width", doc->size.width);
    yyjson_mut_obj_add_uint(rdoc, root, "height", doc->size.height);
    yyjson_mut_obj_add_uint(rdoc, root, "layers", nlayers);
    yyjson_mut_val *warr = yyjson_mut_arr(rdoc);
    for (size_t i = 0; i < o.warnings.n; i++) {
        yyjson_mut_arr_add_strcpy(rdoc, warr, o.warnings.v[i]);
    }
    pc_strlist fonts = {NULL, 0};
    (void)collect_missing_fonts(doc, &fonts, NULL);
    add_strlist_dedup(rdoc, warr, &fonts);
    pc_strlist_fini(&fonts);
    yyjson_mut_obj_add_val(rdoc, root, "warnings", warr);
    pc_io_warnings_fini(&o.warnings);
    return wrap(rdoc, root, out, err);
}

/* ---- save ---- */

static bool format_holds(const char *ext)
{
    /* Layered formats that write the whole document back (losses_at detail is omitted, §7). */
    return strcmp(ext, "pcraft") == 0 || strcmp(ext, "psd") == 0 || strcmp(ext, "psb") == 0 ||
           strcmp(ext, "ora") == 0;
}

static void lower_ext(const char *in, char *out, size_t cap)
{
    const char *f = in;
    while (*f == '.') {
        f++;
    }
    size_t n = 0;
    for (; *f && n + 1 < cap; f++) {
        char c = *f;
        out[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
}

/* Lowercased extension from a path's last component, or "" when none. */
static void path_ext(const char *path, char *out, size_t cap)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == '\0') {
        out[0] = '\0';
        return;
    }
    lower_ext(dot + 1, out, cap);
}

pc_status pc_headless_save(pc_headless *h, int64_t index, const char *path, const char *format,
                           const pc_io_export_opts *opts, pc_json_value *out, pc_error *err)
{
    size_t i;
    if (doc_index(h, index, &i, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = &pc_engine_session_documents(h->session, &(size_t){0})[i];
    const char *stored_path = d->path;
    const pc_document *doc = pc_doc_arc_get(d->doc);
    uint64_t doc_id = doc->id;

    /* Resolve the target path. */
    const char *target = NULL;
    if (path) {
        target = path;
    } else if (stored_path) {
        char own[32], written[32];
        path_ext(stored_path, own, sizeof own);
        if (format) {
            lower_ext(format, written, sizeof written);
        } else {
            snprintf(written, sizeof written, "%s", own);
        }
        if (!format_holds(own) || strcmp(written, own) != 0) {
            return pc_auto_bad(err,
                               "pass `path`: without one, only a PSD, PSB, .pcraft or OpenRaster file that "
                               "holds the whole document is written back, in its own format, so `%s` was "
                               "left unchanged",
                               stored_path);
        }
        target = stored_path;
    } else {
        return pc_auto_bad(err, "document has no path; pass `path`");
    }

    pc_io_warnings warnings = PC_IO_WARNINGS_INIT;
    pc_status rc;
    switch (h->fs) {
    case PC_AUTO_FS_DENIED:
        return pc_auto_bad(err, "automation filesystem access is not granted: write authority is absent");
    case PC_AUTO_FS_TRUSTED_LOCAL: {
        pc_pcraft_writer *w = writer_for(h, doc_id, err);
        if (!w) {
            return err ? err->code : PC_ERR_NOMEM;
        }
        rc = pc_auto_save(doc, target, format, opts, w, &warnings, err);
        break;
    }
    case PC_AUTO_FS_WORKSPACE: {
        pc_buf bytes = PC_BUF_INIT;
        rc = pc_auto_save_bytes(doc, target, format, opts, &bytes, &warnings, err);
        if (rc == PC_OK) {
            rc = pc_auto_workspace_write(&h->workspace, target, bytes.data, bytes.len, err);
        }
        pc_buf_fini(&bytes);
        break;
    }
    default:
        return pc_auto_bad(err, "automation filesystem access is not granted");
    }
    if (rc != PC_OK) {
        pc_io_warnings_fini(&warnings);
        return rc;
    }

    /* Mark saved when a layered format held the whole document. */
    char ext[32];
    if (format) {
        lower_ext(format, ext, sizeof ext);
    } else {
        path_ext(target, ext, sizeof ext);
    }
    if (format_holds(ext)) {
        size_t prev_active = 0;
        bool had_active = pc_engine_session_active_index(h->session, &prev_active);
        pc_engine_session_set_active(h->session, i);
        pc_engine_doc_state *st = pc_engine_session_active_mut(h->session);
        if (st) {
            char *dup = strdup(target);
            if (dup) {
                /* On a pathless (write-back) save `target` aliases st->path, so repoint `target`
                 * at the new copy before freeing the old one -- the result JSON below still reads
                 * it (and a flat caller path is likewise left valid). */
                char *old = st->path;
                st->path = dup;
                st->saved_revision = st->revision;
                target = dup;
                free(old);
            }
        }
        if (had_active) {
            pc_engine_session_set_active(h->session, prev_active);
        }
    }

    yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
    if (!rdoc) {
        pc_io_warnings_fini(&warnings);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(rdoc);
    yyjson_mut_obj_add_strcpy(rdoc, root, "path", target);
    yyjson_mut_val *warr = yyjson_mut_arr(rdoc);
    for (size_t k = 0; k < warnings.n; k++) {
        yyjson_mut_arr_add_strcpy(rdoc, warr, warnings.v[k]);
    }
    pc_strlist fonts = {NULL, 0};
    (void)collect_missing_fonts(doc, &fonts, NULL);
    add_strlist_dedup(rdoc, warr, &fonts);
    pc_strlist_fini(&fonts);
    yyjson_mut_obj_add_val(rdoc, root, "warnings", warr);
    pc_io_warnings_fini(&warnings);
    return wrap(rdoc, root, out, err);
}

/* ---- inspect / render / session / select / close ---- */

pc_status pc_headless_inspect(pc_headless *h, int64_t index, pc_json_value *out, pc_error *err)
{
    size_t i;
    if (doc_index(h, index, &i, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = &pc_engine_session_documents(h->session, &(size_t){0})[i];
    return pc_engine_inspect_document(d, out, err);
}

pc_status pc_headless_render_png(pc_headless *h, int64_t index, uint32_t max_side, pc_buf *png, pc_error *err)
{
    size_t i;
    if (doc_index(h, index, &i, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = &pc_engine_session_documents(h->session, &(size_t){0})[i];
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (h->fs != PC_AUTO_FS_TRUSTED_LOCAL) {
        if (pc_auto_check_preview(doc->size.width, doc->size.height, max_side, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
    }
    if (pc_auto_render_png(doc, max_side, png, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    if (h->fs != PC_AUTO_FS_TRUSTED_LOCAL) {
        if (pc_auto_check_png(png->len, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
    }
    return PC_OK;
}

pc_status pc_headless_session_list(pc_headless *h, pc_json_value *out, pc_error *err)
{
    return pc_engine_inspect_session(h->session, out, err);
}

pc_status pc_headless_select(pc_headless *h, size_t index, pc_json_value *out, pc_error *err)
{
    if (!pc_engine_session_set_active(h->session, index)) {
        return pc_auto_bad(err, "no document at index %zu", index);
    }
    return pc_headless_session_list(h, out, err);
}

pc_status pc_headless_close(pc_headless *h, int64_t index, pc_json_value *out, pc_error *err)
{
    size_t i;
    if (doc_index(h, index, &i, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = &pc_engine_session_documents(h->session, &(size_t){0})[i];
    uint64_t doc_id = pc_doc_arc_get(d->doc)->id;
    if (pc_engine_session_close(h->session, i)) {
        writer_remove(h, doc_id);
    }
    return pc_headless_session_list(h, out, err);
}

pc_status pc_headless_command_list(pc_headless *h, pc_json_value *out, pc_error *err)
{
    size_t n = 0;
    const pc_engine_command *cmds = pc_engine_commands(&n);
    yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
    if (!rdoc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *arr = yyjson_mut_arr(rdoc);
    for (size_t i = 0; i < n; i++) {
        pc_error re;
        pc_error_clear(&re);
        bool disabled = pc_engine_session_disabled_reason(h->session, cmds[i].id, &re);
        const char *reason = disabled ? re.msg : NULL;
        pc_json_value entry = PC_JSON_VALUE_INIT;
        if (pc_engine_command_list_entry(h->session, &cmds[i], reason, &entry, err) != PC_OK) {
            yyjson_mut_doc_free(rdoc);
            return err ? err->code : PC_ERR_NOMEM;
        }
        yyjson_mut_val *r = pc_json_value_root(&entry);
        yyjson_mut_val *copy = r ? yyjson_mut_val_mut_copy(rdoc, r) : yyjson_mut_null(rdoc);
        yyjson_mut_arr_append(arr, copy ? copy : yyjson_mut_null(rdoc));
        pc_json_value_fini(&entry);
    }
    return wrap(rdoc, arr, out, err);
}

pc_status pc_headless_command_start(pc_headless *h, const char *id, yyjson_val *params, bool wait,
                                    pc_json_value *out, pc_error *err)
{
    if (h->fs != PC_AUTO_FS_TRUSTED_LOCAL) {
        if (pc_auto_authorize_engine_command(id, params, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
    }
    pc_headless_sync_jobs(h);
    if (wait) {
        return pc_engine_session_execute(h->session, id, params, out, err);
    }
    bool is_job = false;
    uint64_t job_id = 0;
    pc_json_value inline_out = PC_JSON_VALUE_INIT;
    if (pc_engine_session_start(h->session, id, params, &is_job, &job_id, &inline_out, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    if (!is_job) {
        *out = inline_out;
        return PC_OK;
    }
    pc_json_value_fini(&inline_out);
    yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
    if (!rdoc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(rdoc);
    yyjson_mut_obj_add_uint(rdoc, root, "job", job_id);
    yyjson_mut_obj_add_bool(rdoc, root, "pending", true);
    return wrap(rdoc, root, out, err);
}

pc_status pc_headless_write_render(pc_headless *h, const char *path, const void *bytes, size_t len,
                                   pc_error *err)
{
    switch (h->fs) {
    case PC_AUTO_FS_DENIED:
        return pc_auto_bad(err, "automation filesystem access is not granted: write authority is absent");
    case PC_AUTO_FS_TRUSTED_LOCAL:
        return pc_file_write_atomic(path, bytes, len, err);
    case PC_AUTO_FS_WORKSPACE:
        return pc_auto_workspace_write(&h->workspace, path, bytes, len, err);
    default:
        return pc_auto_bad(err, "automation filesystem access is not granted");
    }
}

/* ---- control-protocol dispatch (port of rpc.rs Headless::handle / batch) ---- */

static const char *str_of(yyjson_val *p, const char *k)
{
    return pc_json_as_str(pc_json_get(p, k), NULL);
}

static pc_status index_of(yyjson_val *p, int64_t *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "index");
    if (!v || yyjson_is_null(v)) {
        *out = -1;
        return PC_OK;
    }
    uint64_t u;
    if (pc_json_as_u64(v, &u) && u <= (uint64_t)INT64_MAX) {
        *out = (int64_t)u;
        return PC_OK;
    }
    return pc_auto_bad(err, "`index` must be a non-negative integer");
}

/* A capability-scoped document method (doc.place / doc.exportLayers / ...) that in trusted-local
 * mode routes to the ambient engine command. The workspace variants that read/write beneath the
 * roots are deferred to a later wave (§6/§7); they report that rather than crashing. */
static pc_status capability_route(pc_headless *h, const char *ambient_cmd, const char *path_key,
                                  const char *path_value, yyjson_val *extra, pc_json_value *out,
                                  pc_error *err)
{
    if (h->fs != PC_AUTO_FS_TRUSTED_LOCAL) {
        return pc_auto_bad(err,
                           "capability-scoped `%s` is not available in this build; run the CLI, which "
                           "reads and writes host paths directly, or use doc.open/doc.save/doc.render",
                           ambient_cmd);
    }
    /* Build {<path_key>: path_value, ...extra} and run the ambient command. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *o = extra ? yyjson_val_mut_copy(doc, extra) : yyjson_mut_obj(doc);
    if (!o || !yyjson_mut_is_obj(o)) {
        o = yyjson_mut_obj(doc);
    }
    yyjson_mut_obj_add_strcpy(doc, o, path_key, path_value);
    yyjson_mut_doc_set_root(doc, o);
    pc_buf text = PC_BUF_INIT;
    pc_status rc = pc_json_write(o, 0, &text, err);
    if (rc == PC_OK) {
        rc = pc_engine_session_execute_json(h->session, ambient_cmd, pc_buf_cstr(&text), out, err);
    }
    pc_buf_fini(&text);
    yyjson_mut_doc_free(doc);
    return rc;
}

pc_status pc_headless_batch(pc_headless *h, yyjson_val *params, pc_auto_batch_budget budget,
                            pc_json_value *out, pc_error *err)
{
    yyjson_val *steps = pc_json_get(params, "steps");
    if (!steps || !yyjson_is_arr(steps)) {
        return pc_auto_bad(err, "batch needs `steps`");
    }
    size_t nsteps = yyjson_arr_size(steps);
    if (nsteps > PC_AUTO_MAX_BATCH_STEPS) {
        return pc_auto_bad(err, "batch contains %zu steps; maximum is %d", nsteps, PC_AUTO_MAX_BATCH_STEPS);
    }
    bool stop = true;
    bool b;
    if (pc_json_as_bool(pc_json_get(params, "stopOnError"), &b)) {
        stop = b;
    }
    yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
    if (!rdoc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *results = yyjson_mut_arr(rdoc);
    size_t failed = 0, completed = 0;
    size_t idx = 0;
    yyjson_val *step;
    yyjson_arr_iter it;
    yyjson_arr_iter_init(steps, &it);
    while ((step = yyjson_arr_iter_next(&it))) {
        yyjson_val *sparams = pc_json_get(step, "params");
        const char *cmd = str_of(step, "command");
        const char *method = str_of(step, "method");
        pc_json_value sr = PC_JSON_VALUE_INIT;
        pc_error se;
        pc_error_clear(&se);
        pc_status src;
        if (cmd) {
            bool wait = true;
            bool sb;
            if (pc_json_as_bool(pc_json_get(step, "wait"), &sb)) {
                wait = sb;
            }
            src = pc_headless_command_start(h, cmd, sparams, wait, &sr, &se);
        } else if (method) {
            if (strcmp(method, "batch") == 0) {
                src = pc_auto_bad(&se, "nested batch");
            } else {
                src = pc_headless_handle(h, method, sparams, &sr, &se);
            }
        } else {
            src = pc_auto_bad(&se, "step %zu needs `command` or `method`", idx);
        }
        bool was_error = src != PC_OK;
        /* Build {"ok":true,"result":v} or {"ok":false,"error":msg}. */
        yyjson_mut_val *entry = yyjson_mut_obj(rdoc);
        yyjson_mut_obj_add_bool(rdoc, entry, "ok", !was_error);
        if (was_error) {
            yyjson_mut_obj_add_strcpy(rdoc, entry, "error", se.msg);
        } else {
            yyjson_mut_val *rc = pc_json_value_root(&sr);
            yyjson_mut_val *copy = rc ? yyjson_mut_val_mut_copy(rdoc, rc) : yyjson_mut_null(rdoc);
            yyjson_mut_obj_add_val(rdoc, entry, "result", copy ? copy : yyjson_mut_null(rdoc));
        }
        pc_json_value_fini(&sr);
        if (pc_auto_batch_budget_charge(&budget, entry, &se) != PC_OK) {
            failed++;
            yyjson_mut_val *over = yyjson_mut_obj(rdoc);
            char m[512];
            snprintf(m, sizeof m, "batch response budget exceeded; current step may have completed: %s",
                     se.msg);
            yyjson_mut_obj_add_bool(rdoc, over, "ok", false);
            yyjson_mut_obj_add_strcpy(rdoc, over, "error", m);
            yyjson_mut_arr_append(results, over);
            break;
        }
        yyjson_mut_arr_append(results, entry);
        if (was_error) {
            failed++;
            if (stop) {
                break;
            }
        }
        idx++;
    }
    size_t total = yyjson_mut_arr_size(results);
    completed = total - failed;
    yyjson_mut_val *root = yyjson_mut_obj(rdoc);
    yyjson_mut_obj_add_uint(rdoc, root, "completed", completed);
    yyjson_mut_obj_add_uint(rdoc, root, "failed", failed);
    yyjson_mut_obj_add_val(rdoc, root, "results", results);
    return wrap(rdoc, root, out, err);
}

pc_status pc_headless_handle(pc_headless *h, const char *method, yyjson_val *params, pc_json_value *out,
                             pc_error *err)
{
    pc_headless_sync_jobs(h);
    if (strcmp(method, "engine.execute") == 0) {
        const char *id = str_of(params, "command");
        if (!id) {
            return pc_auto_bad(err, "engine.execute needs `command`");
        }
        bool wait = true, b;
        if (pc_json_as_bool(pc_json_get(params, "wait"), &b)) {
            wait = b;
        }
        return pc_headless_command_start(h, id, pc_json_get(params, "params"), wait, out, err);
    }
    if (strcmp(method, "jobs.list") == 0) {
        return pc_headless_command_start(h, "jobs.list", NULL, true, out, err);
    }
    if (strcmp(method, "jobs.cancel") == 0) {
        return pc_headless_command_start(h, "jobs.cancel", params, true, out, err);
    }
    if (strcmp(method, "engine.commands") == 0) {
        pc_json_value all = PC_JSON_VALUE_INIT;
        if (pc_headless_command_list(h, &all, err) != PC_OK) {
            return err ? err->code : PC_ERR_NOMEM;
        }
        const char *filter = str_of(params, "filter");
        if (!filter) {
            *out = all;
            return PC_OK;
        }
        /* Filter the array by id/label substring (case-insensitive). */
        char needle[256];
        size_t ni = 0;
        for (const char *p = filter; *p && ni + 1 < sizeof needle; p++) {
            char c = *p;
            needle[ni++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        needle[ni] = '\0';
        yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *arr = rdoc ? yyjson_mut_arr(rdoc) : NULL;
        yyjson_mut_val *src = pc_json_value_root(&all);
        if (rdoc && arr && src && yyjson_mut_is_arr(src)) {
            yyjson_mut_val *c;
            yyjson_mut_arr_iter ai;
            yyjson_mut_arr_iter_init(src, &ai);
            while ((c = yyjson_mut_arr_iter_next(&ai))) {
                const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(c, "id"));
                const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(c, "label"));
                char hay[512];
                snprintf(hay, sizeof hay, "%s %s", id ? id : "", label ? label : "");
                for (char *q = hay; *q; q++) {
                    if (*q >= 'A' && *q <= 'Z') {
                        *q = (char)(*q - 'A' + 'a');
                    }
                }
                if (strstr(hay, needle)) {
                    yyjson_mut_arr_append(arr, yyjson_mut_val_mut_copy(rdoc, c));
                }
            }
        }
        pc_json_value_fini(&all);
        if (!rdoc || !arr) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        return wrap(rdoc, arr, out, err);
    }
    if (strcmp(method, "session.list") == 0) {
        return pc_headless_session_list(h, out, err);
    }
    if (strcmp(method, "doc.open") == 0) {
        const char *path = str_of(params, "path");
        if (!path) {
            return pc_auto_bad(err, "doc.open needs `path`");
        }
        return pc_headless_open(h, path, out, err);
    }
    if (strcmp(method, "doc.place") == 0) {
        const char *path = str_of(params, "path");
        if (!path) {
            return pc_auto_bad(err, "doc.place needs `path`");
        }
        return capability_route(h, "file.placeEmbedded", "path", path, params, out, err);
    }
    if (strcmp(method, "doc.new") == 0) {
        return pc_headless_command_start(h, "file.new", params, true, out, err);
    }
    if (strcmp(method, "doc.save") == 0) {
        pc_io_export_opts opts = pc_io_export_opts_default();
        bool b;
        if (pc_json_as_bool(pc_json_get(params, "tiffLayers"), &b)) {
            opts.tiff_layers = b;
        }
        yyjson_val *tga = pc_json_get(params, "tgaBits");
        if (tga) {
            uint64_t t;
            if (!pc_json_as_u64(tga, &t) || (t != 24 && t != 32)) {
                return pc_auto_bad(err, "`tgaBits` must be 24 or 32");
            }
        }
        yyjson_val *dpx = pc_json_get(params, "dpxBits");
        if (dpx) {
            uint64_t t;
            if (!pc_json_as_u64(dpx, &t)) {
                return pc_auto_bad(err, "`dpxBits` must be 8, 10, 12 or 16");
            }
            switch (t) {
            case 8: opts.encode.dpx_bits = PC_DPX_BITS_8; break;
            case 10: opts.encode.dpx_bits = PC_DPX_BITS_10; break;
            case 12: opts.encode.dpx_bits = PC_DPX_BITS_12; break;
            case 16: opts.encode.dpx_bits = PC_DPX_BITS_16; break;
            default: return pc_auto_bad(err, "`dpxBits` must be 8, 10, 12 or 16");
            }
        }
        uint64_t q;
        if (pc_json_as_u64(pc_json_get(params, "quality"), &q)) {
            uint8_t qq = (uint8_t)(q < 1 ? 1 : (q > 100 ? 100 : q));
            opts.encode.jpeg_quality = qq;
            opts.encode.webp_quality = qq;
            opts.encode.webp_lossless = false;
        }
        int64_t idx;
        if (index_of(params, &idx, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
        return pc_headless_save(h, idx, str_of(params, "path"), str_of(params, "format"), &opts, out, err);
    }
    if (strcmp(method, "doc.exportLayers") == 0 || strcmp(method, "doc.exportDataSets") == 0) {
        const char *dir = str_of(params, "dir");
        if (!dir) {
            return pc_auto_bad(err, "%s needs `dir`", method);
        }
        const char *cmd = strcmp(method, "doc.exportLayers") == 0 ? "file.export.layersToFiles"
                                                                  : "file.export.dataSetsAsFiles";
        return capability_route(h, cmd, "dir", dir, params, out, err);
    }
    if (strcmp(method, "doc.importDataSets") == 0) {
        const char *path = str_of(params, "path");
        if (!path) {
            return pc_auto_bad(err, "doc.importDataSets needs `path`");
        }
        return capability_route(h, "file.import.variableDataSets", "path", path, params, out, err);
    }
    if (strcmp(method, "doc.inspect") == 0) {
        int64_t idx;
        if (index_of(params, &idx, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
        return pc_headless_inspect(h, idx, out, err);
    }
    if (strcmp(method, "doc.render") == 0) {
        uint32_t max = 1024;
        yyjson_val *ms = pc_json_get(params, "maxSide");
        if (ms) {
            uint64_t v;
            if (!pc_json_as_u64(ms, &v) || v > UINT32_MAX) {
                return pc_auto_bad(err, "maxSide must be an unsigned 32-bit integer");
            }
            max = (uint32_t)v;
        }
        int64_t idx;
        if (index_of(params, &idx, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
        pc_buf png = PC_BUF_INIT;
        if (pc_headless_render_png(h, idx, max, &png, err) != PC_OK) {
            pc_buf_fini(&png);
            return err ? err->code : PC_ERR_INVALID;
        }
        const char *path = str_of(params, "path");
        yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
        if (!rdoc) {
            pc_buf_fini(&png);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *root = yyjson_mut_obj(rdoc);
        if (path) {
            pc_status wr = pc_headless_write_render(h, path, png.data, png.len, err);
            if (wr != PC_OK) {
                yyjson_mut_doc_free(rdoc);
                pc_buf_fini(&png);
                return wr;
            }
            yyjson_mut_obj_add_strcpy(rdoc, root, "path", path);
            yyjson_mut_obj_add_uint(rdoc, root, "bytes", png.len);
        } else {
            pc_buf b64 = PC_BUF_INIT;
            if (pc_base64_encode(png.data, png.len, &b64, err) != PC_OK) {
                yyjson_mut_doc_free(rdoc);
                pc_buf_fini(&png);
                pc_buf_fini(&b64);
                return err ? err->code : PC_ERR_NOMEM;
            }
            yyjson_mut_obj_add_strcpy(rdoc, root, "mime", "image/png");
            yyjson_mut_obj_add_strcpy(rdoc, root, "base64", pc_buf_cstr(&b64));
            pc_buf_fini(&b64);
        }
        pc_buf_fini(&png);
        return wrap(rdoc, root, out, err);
    }
    if (strcmp(method, "doc.select") == 0) {
        int64_t idx;
        if (index_of(params, &idx, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
        if (idx < 0) {
            return pc_auto_bad(err, "doc.select needs `index`");
        }
        return pc_headless_select(h, (size_t)idx, out, err);
    }
    if (strcmp(method, "doc.close") == 0) {
        int64_t idx;
        if (index_of(params, &idx, err) != PC_OK) {
            return err ? err->code : PC_ERR_INVALID;
        }
        return pc_headless_close(h, idx, out, err);
    }
    if (strcmp(method, "batch") == 0) {
        return pc_headless_batch(h, params, pc_auto_batch_budget_default(), out, err);
    }
    if (strcmp(method, "methods") == 0) {
        yyjson_mut_doc *rdoc = yyjson_mut_doc_new(NULL);
        if (!rdoc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *arr = yyjson_mut_arr(rdoc);
        for (size_t i = 0; pc_headless_methods[i]; i++) {
            yyjson_mut_arr_add_strcpy(rdoc, arr, pc_headless_methods[i]);
        }
        return wrap(rdoc, arr, out, err);
    }
    return pc_auto_bad(err, "unknown method `%s` (try `methods`)", method);
}
