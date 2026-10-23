/* File > Open EXR Parts and the EXR Parts Info query (port of crates/engine/src/exr_cmds.rs).
 *
 * A multi-part or channel-grouped EXR (a Maya/Arnold render: one part per AOV, or prefixed channel
 * sets like `diffuse.R`) is listed by file.exrParts and opened by file.openExrParts, importing the
 * chosen parts/groups as the layers of one document. The EXR decoding lives in src/io/exr_parts.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): a missing/unreadable path or an empty selection is a
 * graceful error. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "io/core.h"
#include "io/exr_parts.h"

#include "engine_internal.h"
#include "exr_parts_cmds.h"

static const char *const MENU_FILE[] = {"File"};

/* Read the EXR file at `path` (param) into *buf. */
static pc_status read_param_file(yyjson_val *p, const char *cmd, pc_buf *buf, char *path_out,
                                 size_t path_cap, pc_error *err)
{
    size_t pl = 0;
    const char *path = p != NULL ? pc_json_as_str(pc_json_get(p, "path"), &pl) : NULL;
    if (path == NULL || pl == 0) {
        return pc_engine_err_bad_params(err, cmd, "need `path` (the EXR file)");
    }
    if (pl >= path_cap) {
        return pc_engine_err_bad_params(err, cmd, "path too long");
    }
    memcpy(path_out, path, pl);
    path_out[pl] = '\0';
    return pc_engine_read_file(path_out, buf, err);
}

static pc_status run_exr_parts(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    char path[4096];
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = read_param_file(p, "file.exrParts", &buf, path, sizeof path, err);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return rc;
    }
    pc_io_exr_entry *entries = NULL;
    size_t n = 0;
    rc = pc_io_exr_entries(buf.data, buf.len, &entries, &n, err);
    pc_buf_fini(&buf);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_io_exr_entries_free(entries, n);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n; i++) {
        pc_io_exr_entry *e = &entries[i];
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        const char *kind = e->kind == PC_IO_EXR_PART      ? "part"
                           : e->kind == PC_IO_EXR_GROUP   ? "group"
                                                          : "cryptomatte";
        yyjson_mut_obj_add_strcpy(rd, o, "kind", kind);
        yyjson_mut_obj_add_strcpy(rd, o, "name", e->name != NULL ? e->name : "");
        if (e->view != NULL) {
            yyjson_mut_obj_add_strcpy(rd, o, "view", e->view);
        } else {
            yyjson_mut_obj_add_null(rd, o, "view");
        }
        yyjson_mut_obj_add_uint(rd, o, "width", e->width);
        yyjson_mut_obj_add_uint(rd, o, "height", e->height);
        yyjson_mut_obj_add_uint(rd, o, "channels", e->channels);
        if (e->has_objects) {
            yyjson_mut_obj_add_uint(rd, o, "objects", e->objects);
        }
        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_doc_set_root(rd, arr);
    pc_io_exr_entries_free(entries, n);
    return PC_OK;
}

/* Collect `<prefix><name>` boolean params set true into a borrowed-pointer array. */
static const char **collect_selected(yyjson_val *p, const char *prefix, size_t *n_out)
{
    *n_out = 0;
    if (p == NULL || !yyjson_is_obj(p)) {
        return NULL;
    }
    size_t plen = strlen(prefix);
    size_t cap = 0, n = 0;
    const char **names = NULL;
    size_t idx, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(p, idx, max, key, val)
    {
        size_t kl = 0;
        const char *k = pc_json_as_str(key, &kl);
        bool b = false;
        if (k == NULL || kl < plen || strncmp(k, prefix, plen) != 0 || !pc_json_as_bool(val, &b) || !b) {
            continue;
        }
        if (n == cap) {
            size_t nc = cap == 0 ? 8 : cap * 2;
            const char **nv = realloc(names, nc * sizeof *nv);
            if (nv == NULL) {
                free(names);
                *n_out = 0;
                return NULL;
            }
            names = nv;
            cap = nc;
        }
        names[n++] = k + plen; /* borrowed; valid while the params doc lives */
    }
    *n_out = n;
    return names;
}

static pc_status run_open_exr_parts(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.openExrParts";
    char path[4096];
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = read_param_file(p, cmd, &buf, path, sizeof path, err);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return rc;
    }
    size_t vl = 0;
    const char *view = p != NULL ? pc_json_as_str(pc_json_get(p, "view"), &vl) : NULL;
    char viewbuf[8] = "both";
    if (view != NULL) {
        if (!((vl == 4 && memcmp(view, "both", 4) == 0) || (vl == 4 && memcmp(view, "left", 4) == 0) ||
              (vl == 5 && memcmp(view, "right", 5) == 0))) {
            pc_buf_fini(&buf);
            return pc_engine_err_bad_params(err, cmd, "view is \"both\", \"left\" or \"right\"");
        }
        memcpy(viewbuf, view, vl);
        viewbuf[vl] = '\0';
    }
    bool precomp = false;
    yyjson_val *jpre = p != NULL ? pc_json_get(p, "precomp") : NULL;
    if (jpre != NULL) {
        (void)pc_json_as_bool(jpre, &precomp);
    }
    size_t nparts = 0, ngroups = 0;
    const char **parts = collect_selected(p, "part/", &nparts);
    const char **groups = collect_selected(p, "group/", &ngroups);

    char stem[256];
    pc_engine_file_stem(path, stem, sizeof stem);
    if (stem[0] == '\0') {
        (void)snprintf(stem, sizeof stem, "EXR");
    }

    pc_io_exr_selection sel;
    sel.parts = parts;
    sel.nparts = nparts;
    sel.groups = groups;
    sel.ngroups = ngroups;
    sel.view = viewbuf;
    sel.precomp = precomp;

    pc_io_import_result r;
    memset(&r, 0, sizeof r);
    rc = pc_io_exr_import(stem, buf.data, buf.len, &sel, &r, err);
    pc_buf_fini(&buf);
    free(parts);
    free(groups);
    if (rc != PC_OK) {
        return rc;
    }

    pc_json_value color_report = PC_JSON_VALUE_INIT;
    rc = pc_engine_color_open_policy(s, &r.document, false, &color_report, err);
    if (rc != PC_OK) {
        pc_io_import_result_fini(&r);
        pc_json_value_fini(&color_report);
        return rc;
    }
    size_t index = 0;
    rc = pc_engine_session_add_document(s, &r.document, path, &index, err);
    if (rc != PC_OK) {
        pc_io_import_result_fini(&r);
        pc_json_value_fini(&color_report);
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->source_read_only = r.source_read_only;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_io_import_result_fini(&r);
        pc_json_value_fini(&color_report);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "document", index);
    yyjson_mut_val *rep = pc_json_value_root(&color_report);
    yyjson_mut_obj_add_val(rd, root, "color", rep != NULL ? yyjson_mut_val_mut_copy(rd, rep)
                                                          : yyjson_mut_obj(rd));
    pc_json_value_fini(&color_report);
    yyjson_mut_val *warr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < r.warnings.n; i++) {
        yyjson_mut_arr_add_strcpy(rd, warr, r.warnings.v[i] != NULL ? r.warnings.v[i] : "");
    }
    yyjson_mut_obj_add_val(rd, root, "warnings", warr);
    yyjson_mut_doc_set_root(rd, root);
    pc_io_warnings_fini(&r.warnings); /* the document was moved out by add_document */
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"file.exrParts", "EXR Parts Info", NULL, 0, NULL, "{\"path\":str (the EXR file)}",
     pc_engine_always, run_exr_parts, false},
    {"file.openExrParts", "Open EXR Parts\xE2\x80\xA6", MENU_FILE, 1, NULL,
     "{\"path\":str (the EXR file),\"view\":\"both|left|right\"=\"both\",\"part/<name>\":bool (import "
     "this part as a layer),\"group/<name>\":bool (import this channel group as a layer),\"precomp\":"
     "bool=false (add the additive beauty precomp from the AOVs)} -> {document,color,warnings}",
     pc_engine_always, run_open_exr_parts, true},
};

const pc_engine_command *pc_engine_exr_parts_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
