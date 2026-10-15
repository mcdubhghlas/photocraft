/* plugin.* commands: sandboxed WebAssembly filter plug-ins (src/plugins). Port of
 * crates/engine/src/plugin_cmds.rs (E12).
 *
 * Plug-ins run on the active pixel layer (channel-targeted and smart-object runs are DEFERRED:
 * they need channel_cmds::channel_surface_for_filter (E7) and smart_cmds::add_smart_filter (E6),
 * which those chunks did not export; a smart object is reported unsupported, as filters_cmds does).
 * The plug-in smart-filter re-render is wired into pc_engine_hook_filter_subtype (moved here from
 * hooks.c). The Plug-ins-preference-folder auto-load (sync_prefs) is not wired: there is no C
 * prefs-applied hook to attach to; plugin.reload loads a folder explicitly (REWRITE_PLAN.md §7). */
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "plugins/plugins.h"
#include "smart_cmds.h" /* E-tail: pc_engine_add_smart_filter (record a plug-in smart filter) */

#define RUN_ID "plugin.run"

/* The folder last loaded (plugin.reload), for plugin.list; process-wide. */
static pthread_mutex_t g_folder_lock = PTHREAD_MUTEX_INITIALIZER;
static pc_json_value g_folder_report = PC_JSON_VALUE_INIT; /* null = none loaded */

static pc_status always_ok(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* describe(p): JSON description of an installed plug-in. */
static yyjson_mut_val *describe(yyjson_mut_doc *d, const pc_plugin *p)
{
    const pc_plugin_manifest *m = pc_plugin_get_manifest(p);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "id", pc_str_c(&m->id));
    yyjson_mut_obj_add_strcpy(d, o, "name", pc_str_c(&m->name));
    yyjson_mut_obj_add_strcpy(d, o, "version", pc_str_c(&m->version));
    yyjson_mut_obj_add_str(d, o, "kind", "filter");
    yyjson_mut_obj_add_strcpy(d, o, "author", pc_str_c(&m->author));
    yyjson_mut_obj_add_strcpy(d, o, "description", pc_str_c(&m->description));
    pc_buf notation = PC_BUF_INIT;
    if (pc_plugin_manifest_params_notation(m, &notation, NULL) == PC_OK) {
        yyjson_mut_obj_add_strncpy(d, o, "params", (const char *)notation.data, notation.len);
    } else {
        yyjson_mut_obj_add_str(d, o, "params", "{}");
    }
    pc_buf_fini(&notation);
    /* paramsSchema: the "params" object of the serde manifest. */
    yyjson_mut_val *full = pc_plugin_manifest_to_json(m, d);
    yyjson_mut_val *schema = full != NULL ? yyjson_mut_obj_get(full, "params") : NULL;
    if (schema != NULL) {
        yyjson_mut_obj_add_val(d, o, "paramsSchema", yyjson_mut_val_mut_copy(d, schema));
    } else {
        yyjson_mut_obj_add_null(d, o, "paramsSchema");
    }
    yyjson_mut_obj_add_uint(d, o, "overlap", m->overlap);
    yyjson_mut_obj_add_str(d, o, "area", m->area == PC_PLUGIN_AREA_CANVAS ? "canvas" : "content");
    yyjson_mut_obj_add_uint(d, o, "size", pc_plugin_size(p));
    const char *src = pc_plugin_source(p);
    if (src != NULL) {
        yyjson_mut_obj_add_strcpy(d, o, "source", src);
    } else {
        yyjson_mut_obj_add_null(d, o, "source");
    }
    return o;
}

/* plugin_params(p): a new immutable doc holding "params" if given, else the top-level keys minus
 * id/layer/target/channel and __-prefixed ones. Caller frees *doc with yyjson_doc_free. */
static pc_status plugin_params(yyjson_val *p, yyjson_doc **out, pc_error *err)
{
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, RUN_ID);
    }
    yyjson_mut_val *root;
    yyjson_val *given = p != NULL ? pc_json_get(p, "params") : NULL;
    if (given != NULL) {
        root = yyjson_val_mut_copy(md, given);
    } else if (p != NULL && yyjson_is_obj(p)) {
        root = yyjson_mut_obj(md);
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(p, idx, max, k, v) {
            const char *key = yyjson_get_str(k);
            if (key == NULL) {
                continue;
            }
            if (strcmp(key, "id") == 0 || strcmp(key, "layer") == 0 || strcmp(key, "target") == 0 ||
                strcmp(key, "channel") == 0 || (key[0] == '_' && key[1] == '_')) {
                continue;
            }
            yyjson_mut_obj_add(root, yyjson_mut_strcpy(md, key), yyjson_val_mut_copy(md, v));
        }
    } else {
        root = yyjson_mut_obj(md);
    }
    if (root == NULL) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, RUN_ID);
    }
    yyjson_mut_doc_set_root(md, root);
    /* serialise and reparse into an immutable doc (apply/resolve take yyjson_val*) */
    char *txt = yyjson_mut_write(md, 0, NULL);
    yyjson_mut_doc_free(md);
    if (txt == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, RUN_ID);
    }
    yyjson_doc *doc = yyjson_read(txt, strlen(txt), 0);
    free(txt);
    if (doc == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, RUN_ID);
    }
    *out = doc;
    return PC_OK;
}

/* plugin_by_id(cmd, p): the installed plug-in named by p["id"] (a new reference). */
static pc_status plugin_by_id(const char *cmd, yyjson_val *p, pc_plugin **out, pc_error *err)
{
    size_t idlen = 0;
    const char *id = NULL;
    yyjson_val *idv = p != NULL ? pc_json_get(p, "id") : NULL;
    if (idv != NULL) {
        id = pc_json_as_str(idv, &idlen);
    }
    if (id == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `id` (see plugin.list)");
    }
    /* NUL-terminate for the registry lookup */
    char buf[128];
    if (idlen >= sizeof buf) {
        return pc_plugin_fail_not_found(err, id, idlen);
    }
    memcpy(buf, id, idlen);
    buf[idlen] = '\0';
    pc_plugin *pl = pc_plugins_get(buf);
    if (pl == NULL) {
        return pc_plugin_fail_not_found(err, id, idlen);
    }
    *out = pl;
    return PC_OK;
}

/* ---- the smart-filter subtype hook (moved from hooks.c) ----------------------------------- *
 * Re-render of a recorded plug-in run (filters::apply_filter_to_surface). E4 distort, E6 lens and
 * E8 smart-adjustment subtypes add their id cases here when they land (no duplicate symbol). */
pc_status pc_engine_hook_filter_subtype(const char *id, yyjson_val *params, const pc_surface *surf,
                                        pc_rect bounds, const pc_surface *selection, pc_rect canvas,
                                        pc_surface *out, bool *applied, pc_error *err)
{
    (void)bounds;
    *applied = false;
    /* ET5: Lens Correction / Adaptive Wide Angle / Camera Raw Filter re-render on the placed
     * source (lens_cmds.c). Leaves *applied false for ids it does not own. */
    pc_status lrc = pc_engine_lens_apply_to_surface(id, params, surf, canvas, out, applied, err);
    if (lrc != PC_OK || *applied) {
        return lrc;
    }
    if (id == NULL || strcmp(id, RUN_ID) != 0) {
        return PC_OK; /* not a plug-in run (distort/adjust owners add their cases here) */
    }
    size_t plen = 0;
    const char *pid = NULL;
    yyjson_val *pv = params != NULL ? pc_json_get(params, "id") : NULL;
    if (pv != NULL) {
        pid = pc_json_as_str(pv, &plen);
    }
    if (pid == NULL) {
        return PC_OK; /* unknown recorded filter: skip, as for others */
    }
    char buf[128];
    if (plen >= sizeof buf) {
        return PC_OK;
    }
    memcpy(buf, pid, plen);
    buf[plen] = '\0';
    pc_plugin *pl = pc_plugins_get(buf);
    if (pl == NULL) {
        return PC_OK; /* not installed: the smart filter is skipped */
    }
    yyjson_val *inner = params != NULL ? pc_json_get(params, "params") : NULL;
    pc_surface res;
    pc_status rc = pc_plugin_apply(pl, surf, canvas, selection, inner, &res, err);
    pc_plugin_release(pl);
    if (rc != PC_OK) {
        /* a failing plug-in smart filter is skipped (Rust None), not an error */
        return PC_OK;
    }
    *out = res;
    *applied = true;
    return PC_OK;
}

/* ---- plugin.run --------------------------------------------------------------------------- */

typedef struct {
    pc_plugin *plugin; /* borrowed (a held reference) */
    yyjson_doc *params; /* immutable plugin params */
    pc_layer_id layer;
} run_ctx;

static pc_status edit_run(pc_document *doc, bool *ha, pc_layer_id *al, void *ctx, pc_error *err)
{
    (void)ha;
    (void)al;
    run_ctx *c = ctx;
    yyjson_val *pp = yyjson_doc_get_root(c->params);
    pc_layer *l = pc_document_layer_mut(doc, c->layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->layer);
    }
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *selection = doc->has_selection ? &doc->selection : NULL;
    if (l->content.kind == PC_LAYER_RASTER) {
        pc_surface res;
        pc_status rc = pc_plugin_apply(c->plugin, &l->content.u.raster, canvas, selection, pp, &res, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&l->content.u.raster);
        l->content.u.raster = res;
        return PC_OK;
    }
    if (l->content.kind == PC_LAYER_SMART) {
        /* Non-destructive: record {"id": plugin, "params": params} as a `plugin.run` smart filter
         * and re-render from the source (plugin_cmds::apply_to_surface re-applies on re-render). */
        const char *pid = pc_plugin_id(c->plugin);
        yyjson_mut_doc *jd = yyjson_mut_doc_new(NULL);
        if (jd == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *o = yyjson_mut_obj(jd);
        yyjson_mut_obj_add_strcpy(jd, o, "id", pid != NULL ? pid : "");
        yyjson_mut_val *pcopy = pc_json_normalized_copy_val(jd, pp);
        yyjson_mut_obj_add_val(jd, o, "params", pcopy != NULL ? pcopy : yyjson_mut_null(jd));
        pc_smart_filter sf;
        memset(&sf, 0, sizeof sf);
        pc_status rc = pc_str_set(&sf.command, RUN_ID, strlen(RUN_ID), err);
        if (rc == PC_OK) {
            rc = pc_json_value_from_mut(o, &sf.params, err);
        }
        yyjson_mut_doc_free(jd);
        if (rc != PC_OK) {
            pc_smart_filter_fini(&sf);
            return rc;
        }
        sf.blend = PC_BLEND_NORMAL;
        sf.opacity = 1.0f;
        sf.visible = true;
        return pc_engine_add_smart_filter(doc, c->layer, &sf, selection, err);
    }
    return pc_engine_err_other(err,
                               "plug-in filters need a pixel layer (active layer is %s %s layer)",
                               pc_layer_content_article(&l->content),
                               pc_layer_content_kind_name(&l->content));
}

static pc_status run_run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_plugin *plugin = NULL;
    pc_status rc = plugin_by_id(RUN_ID, p, &plugin, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_doc *params = NULL;
    rc = plugin_params(p, &params, err);
    if (rc != PC_OK) {
        pc_plugin_release(plugin);
        return rc;
    }
    /* Bad parameters fail before anything is touched. */
    const pc_plugin_manifest *m = pc_plugin_get_manifest(plugin);
    pc_json_value resolved = PC_JSON_VALUE_INIT;
    rc = pc_plugin_manifest_resolve_params(m, yyjson_doc_get_root(params), &resolved, err);
    pc_json_value_fini(&resolved);
    if (rc != PC_OK) {
        yyjson_doc_free(params);
        pc_plugin_release(plugin);
        return rc;
    }
    pc_layer_id layer;
    rc = pc_engine_layer_param(s, p, &layer, err);
    if (rc != PC_OK) {
        yyjson_doc_free(params);
        pc_plugin_release(plugin);
        return rc;
    }
    char label[128];
    snprintf(label, sizeof label, "%s", pc_str_c(&m->name));
    const char *id = pc_plugin_id(plugin);
    char idbuf[128];
    snprintf(idbuf, sizeof idbuf, "%s", id != NULL ? id : "");
    run_ctx c = {.plugin = plugin, .params = params, .layer = layer};
    rc = pc_engine_session_edit(s, label, edit_run, &c, err);
    yyjson_doc_free(params);
    pc_plugin_release(plugin);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", layer);
    yyjson_mut_obj_add_strcpy(rd, root, "plugin", idbuf);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- plugin.install ----------------------------------------------------------------------- */

#define MAX_DATA_CHARS (((size_t)32 << 20) / 3 * 4 + 4)

static int b64_val(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

static bool is_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* base64 decode (standard + url-safe), ignoring whitespace and '='. NULL on invalid. */
static pc_status b64_decode(const char *s, size_t len, pc_buf *out, pc_error *err)
{
    uint8_t *clean = pc_malloc_array(len > 0 ? len : 1, 1);
    if (clean == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "plugin.install");
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (is_ws(c) || c == '=') {
            continue;
        }
        clean[n++] = c;
    }
    if (n % 4 == 1) {
        free(clean);
        return pc_fail(err, PC_ERR_INVALID, "`data` is not valid base64");
    }
    pc_buf_init(out);
    for (size_t i = 0; i < n; i += 4) {
        size_t chunk = n - i < 4 ? n - i : 4;
        uint32_t acc = 0;
        for (size_t j = 0; j < chunk; j++) {
            int v = b64_val(clean[i + j]);
            if (v < 0) {
                free(clean);
                pc_buf_fini(out);
                return pc_fail(err, PC_ERR_INVALID, "`data` is not valid base64");
            }
            acc |= (uint32_t)v << (18 - 6 * j);
        }
        uint8_t bytes[3] = {(uint8_t)(acc >> 16), (uint8_t)(acc >> 8), (uint8_t)acc};
        if (pc_buf_append(out, bytes, chunk - 1, err) != PC_OK) {
            free(clean);
            pc_buf_fini(out);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    free(clean);
    return PC_OK;
}

static pc_status run_install(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    const char *CMD = "plugin.install";
    pc_plugin *plugin = NULL;
    yyjson_val *data = p != NULL ? pc_json_get(p, "data") : NULL;
    yyjson_val *path = p != NULL ? pc_json_get(p, "path") : NULL;
    if (data != NULL) {
        size_t dlen = 0;
        const char *ds = pc_json_as_str(data, &dlen);
        if (ds == NULL) {
            return pc_engine_err_bad_params(err, CMD, "`data` must be a base64 string");
        }
        if (dlen > MAX_DATA_CHARS) {
            return pc_engine_err_bad_params(err, CMD, "`data` is larger than the module limit");
        }
        pc_buf bytes;
        pc_status rc = b64_decode(ds, dlen, &bytes, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_plugin_load(bytes.data, bytes.len, NULL, &plugin, err);
        pc_buf_fini(&bytes);
        if (rc != PC_OK) {
            return rc;
        }
    } else if (path != NULL) {
        size_t plen = 0;
        const char *ps = pc_json_as_str(path, &plen);
        if (ps == NULL || plen == 0) {
            return pc_engine_err_bad_params(err, CMD, "`path` must be a file path");
        }
        char buf[4096];
        if (plen >= sizeof buf) {
            return pc_engine_err_bad_params(err, CMD, "`path` is too long");
        }
        memcpy(buf, ps, plen);
        buf[plen] = '\0';
        pc_status rc = pc_plugins_load_file(buf, NULL, &plugin, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        return pc_engine_err_bad_params(err, CMD,
                                        "give `path` (a .wasm file) or `data` (the module as base64)");
    }
    bool replace = true;
    yyjson_val *rv = p != NULL ? pc_json_get(p, "replace") : NULL;
    if (rv != NULL) {
        (void)pc_json_as_bool(rv, &replace);
    }
    if (!replace) {
        pc_plugin *existing = pc_plugins_get(pc_plugin_id(plugin));
        if (existing != NULL) {
            pc_plugin_release(existing);
            char msg[160];
            snprintf(msg, sizeof msg, "a plug-in with id \"%s\" is already installed",
                     pc_plugin_id(plugin));
            pc_plugin_release(plugin);
            return pc_engine_err_bad_params(err, CMD, "%s", msg);
        }
    }
    pc_status rc = pc_plugins_install(plugin, err);
    if (rc != PC_OK) {
        pc_plugin_release(plugin);
        return rc;
    }
    /* describe the installed plug-in (fetch a fresh reference to be safe) */
    pc_plugin *installed = pc_plugins_get(pc_plugin_id(plugin));
    pc_plugin_release(plugin);
    if (installed == NULL) {
        return pc_engine_err_other(err, "plug-in install failed");
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        pc_plugin_release(installed);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(rd, describe(rd, installed));
    pc_plugin_release(installed);
    return PC_OK;
}

static pc_status run_remove(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    size_t idlen = 0;
    const char *id = NULL;
    yyjson_val *idv = p != NULL ? pc_json_get(p, "id") : NULL;
    if (idv != NULL) {
        id = pc_json_as_str(idv, &idlen);
    }
    if (id == NULL) {
        return pc_engine_err_bad_params(err, "plugin.remove", "missing `id`");
    }
    char buf[128];
    if (idlen >= sizeof buf) {
        return pc_plugin_fail_not_found(err, id, idlen);
    }
    memcpy(buf, id, idlen);
    buf[idlen] = '\0';
    if (!pc_plugins_remove(buf)) {
        return pc_plugin_fail_not_found(err, id, idlen);
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "removed", buf);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* load a folder report into *out (owned) and into the process-wide FOLDER record. */
static pc_status load_folder_report(const char *folder, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "folder", folder);
    pc_plugins_folder_report rep;
    pc_error ferr = {0};
    if (pc_plugins_load_folder(folder, &rep, &ferr) != PC_OK) {
        yyjson_mut_obj_add_strcpy(rd, root, "error", ferr.msg);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    yyjson_mut_val *loaded = yyjson_mut_arr(rd);
    for (size_t i = 0; i < rep.n_loaded; i++) {
        yyjson_mut_arr_add_strcpy(rd, loaded, pc_str_c(&rep.loaded[i]));
    }
    yyjson_mut_obj_add_val(rd, root, "loaded", loaded);
    yyjson_mut_val *failed = yyjson_mut_arr(rd);
    for (size_t i = 0; i < rep.n_failed; i++) {
        yyjson_mut_val *fo = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, fo, "file", rep.failed[i].file != NULL ? rep.failed[i].file : "");
        yyjson_mut_obj_add_strcpy(rd, fo, "error",
                                  rep.failed[i].error != NULL ? rep.failed[i].error : "");
        yyjson_mut_arr_append(failed, fo);
    }
    yyjson_mut_obj_add_val(rd, root, "failed", failed);
    pc_plugins_folder_report_fini(&rep);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_reload(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "plugin.reload";
    char folder[4096];
    folder[0] = '\0';
    yyjson_val *pv = p != NULL ? pc_json_get(p, "path") : NULL;
    if (pv != NULL) {
        size_t fl = 0;
        const char *fs = pc_json_as_str(pv, &fl);
        /* trim */
        if (fs != NULL) {
            while (fl > 0 && is_ws((uint8_t)fs[0])) {
                fs++;
                fl--;
            }
            while (fl > 0 && is_ws((uint8_t)fs[fl - 1])) {
                fl--;
            }
        }
        if (fs == NULL || fl == 0) {
            return pc_engine_err_bad_params(err, CMD, "`path` must be a folder");
        }
        if (fl >= sizeof folder) {
            return pc_engine_err_bad_params(err, CMD, "`path` is too long");
        }
        memcpy(folder, fs, fl);
        folder[fl] = '\0';
    } else {
        /* default: the Plug-ins preference folder (prefs plugIns.additionalPluginsFolder) */
        (void)s; /* prefs routing of the folder is deferred with sync_prefs */
    }
    if (folder[0] == '\0') {
        return pc_engine_err_bad_params(
            err, CMD,
            "no folder: pass `path` or set Preferences \xE2\x80\xBA Plug-ins \xE2\x80\xBA Additional "
            "Plug-ins Folder");
    }
    pc_status rc = load_folder_report(folder, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *errv = yyjson_mut_obj_get(pc_json_value_root(out), "error");
    if (errv != NULL) {
        char msg[256];
        snprintf(msg, sizeof msg, "%s", yyjson_mut_get_str(errv) != NULL ? yyjson_mut_get_str(errv) : "");
        pc_json_value_fini(out);
        return pc_engine_err_other(err, "%s", msg);
    }
    /* record as the last folder */
    pthread_mutex_lock(&g_folder_lock);
    pc_json_value_fini(&g_folder_report);
    (void)pc_json_value_clone(out, &g_folder_report, NULL);
    pthread_mutex_unlock(&g_folder_lock);
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    pc_plugin **list = NULL;
    size_t n = 0;
    if (pc_plugins_list(&list, &n, NULL) == PC_OK) {
        for (size_t i = 0; i < n; i++) {
            yyjson_mut_arr_append(arr, describe(rd, list[i]));
        }
        pc_plugins_list_free(list, n);
    }
    yyjson_mut_obj_add_val(rd, root, "plugins", arr);
    pthread_mutex_lock(&g_folder_lock);
    yyjson_mut_val *fr = pc_json_value_root(&g_folder_report);
    if (fr != NULL) {
        yyjson_mut_obj_add_val(rd, root, "folder", yyjson_mut_val_mut_copy(rd, fr));
    } else {
        yyjson_mut_obj_add_null(rd, root, "folder");
    }
    pthread_mutex_unlock(&g_folder_lock);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static const char *const MENU_FILTER_PLUGINS[] = {"Filter", "Plug-ins"};

static const pc_engine_command SPECS[] = {
    {"plugin.list", "List Plug-ins", NULL, 0, NULL, "{}", always_ok, run_list, false},
    {"plugin.install", "Install Plug-in\xE2\x80\xA6", MENU_FILTER_PLUGINS, 2, NULL,
     "{\"path\":text,\"data\":json,\"replace\":bool=true} (path: a .wasm file, native only; data: the "
     "module as base64)",
     always_ok, run_install, false},
    {"plugin.remove", "Remove Plug-in", NULL, 0, NULL, "{\"id\":text}", always_ok, run_remove, false},
    {"plugin.reload", "Reload Plug-ins", NULL, 0, NULL,
     "{\"path\":text} (a folder of .wasm plug-ins; default: the Plug-ins preference folder)",
     always_ok, run_reload, false},
    {RUN_ID, "Run Plug-in", NULL, 0, NULL,
     "{\"id\":text,\"params\":json} (plug-in parameters under \"params\" or at the top level; see "
     "plugin.list)",
     pc_engine_filterable, run_run, true},
};

const pc_engine_command *pc_engine_plugin_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
