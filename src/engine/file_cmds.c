/* File-menu commands beyond New/Close plus the document-level guide items of the View menu
 * (port of crates/engine/src/file_cmds.rs, chunk E3). Everything here routes through src/io
 * (import/export) and src/format (atomic read/write).
 *
 * Commands that touch the file system take explicit paths (the UI asks for them with its own
 * pickers). The byte-level cores (pc_engine_read_file / pc_engine_write_file / pc_engine_import /
 * pc_engine_encode / pc_engine_save_doc / pc_engine_flattened) are exported in engine_internal.h
 * for E6 (editContents / artboard export), E9 (lens / vp / Photomerge load) and E2b (preset
 * import).
 *
 * Never crash: open/place read hostile files through io, which guards; save paths go through the
 * symlink-hardened atomic writer in src/base (shared with src/format). Every run() validates its
 * params and bounds-checks before indexing or dividing (REWRITE_PLAN.md rule 9). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "algo/exif.h"
#include "algo/resample.h"
#include "base/file.h"
#include "base/mem.h"
#include "cms/lutfile.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "doc/vector.h"
#include "prefs.h"
#include "smart_cmds.h"
#include "engine_internal.h"
#include "format/read.h"
#include "io/io.h"
#include "ops/ops.h"

/* A regular file at `path` (follows symlinks, like the reference's Path::is_file). */
static bool fc_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Duplicate a C string into a fresh malloc'd one, or NULL on OOM. */
static char *fc_strdup(const char *s)
{
    return pc_strndup(s, strlen(s));
}

/* Replace `doc`'s whole layer stack with the single `layer` (ownership taken). On OOM `layer` is
 * finalised and the document left with no layers. */
static pc_status fc_set_single_layer(pc_document *doc, pc_layer *layer, pc_error *err)
{
    pc_layer *one = pc_malloc_array(1, sizeof(pc_layer));
    if (one == NULL) {
        pc_layer_fini(layer);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    one[0] = *layer;
    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer_fini(&doc->layers[i]);
    }
    free(doc->layers);
    doc->layers = one;
    doc->nlayers = 1;
    return PC_OK;
}

/* ---- small param helpers ------------------------------------------------------------------ */

static const char *fc_str_param(yyjson_val *p, const char *key, const char *def)
{
    yyjson_val *v = pc_json_get(p, key);
    size_t len = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &len) : NULL;
    return (s != NULL && len > 0) ? s : def;
}

static bool fc_f64_param(yyjson_val *p, const char *key, double *out)
{
    double v;
    if (pc_json_as_f64(pc_json_get(p, key), &v) && isfinite(v)) {
        *out = v;
        return true;
    }
    return false;
}

static bool fc_bool_param(yyjson_val *p, const char *key, bool def)
{
    bool v;
    if (pc_json_as_bool(pc_json_get(p, key), &v)) {
        return v;
    }
    return def;
}

/* Required string param, non-empty. */
static pc_status fc_req_str(yyjson_val *p, const char *key, const char *cmd, const char **out,
                            pc_error *err)
{
    const char *s = fc_str_param(p, key, NULL);
    if (s == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing \"%s\"", key);
    }
    *out = s;
    return PC_OK;
}

/* A preference bool by camelCase path (default on missing/non-bool). */
static bool fc_pref_bool(const pc_engine_session *s, const char *path, bool def)
{
    pc_json_value v = PC_JSON_VALUE_INIT;
    bool out = def;
    if (pc_engine_prefs_get(s, path, &v, NULL) == PC_OK) {
        yyjson_mut_val *r = pc_json_value_root(&v);
        if (r != NULL && yyjson_mut_is_bool(r)) {
            out = yyjson_mut_get_bool(r);
        }
    }
    pc_json_value_fini(&v);
    return out;
}

/* A file-handling Ask preference ("always"/"never"/"ask"): true unless it is exactly "never". */
static bool fc_pref_not_never(const pc_engine_session *s, const char *path)
{
    pc_json_value v = PC_JSON_VALUE_INIT;
    bool not_never = true;
    if (pc_engine_prefs_get(s, path, &v, NULL) == PC_OK) {
        yyjson_mut_val *r = pc_json_value_root(&v);
        const char *str = r != NULL ? yyjson_mut_get_str(r) : NULL;
        if (str != NULL && strcmp(str, "never") == 0) {
            not_never = false;
        }
    }
    pc_json_value_fini(&v);
    return not_never;
}

/* ---- file-name helpers (file_cmds::file_name / stem / extension / join / sanitize) -------- */

static const char *fc_file_name(const char *path)
{
    const char *n = path;
    for (const char *c = path; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\') {
            n = c + 1;
        }
    }
    return n;
}

/* The file name without its extension into out[cap]. */
static void fc_stem(const char *path, char *out, size_t cap)
{
    const char *n = fc_file_name(path);
    const char *dot = NULL;
    for (const char *c = n; *c != '\0'; c++) {
        if (*c == '.') {
            dot = c;
        }
    }
    size_t len = (dot != NULL && dot != n) ? (size_t)(dot - n) : strlen(n);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, n, len);
    out[len] = '\0';
}

/* dir/name into a freshly malloc'd string (caller free()s), or NULL on OOM. */
static char *fc_join(const char *dir, const char *name)
{
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl == 0) {
        char *r = pc_malloc_array(nl + 1, 1);
        if (r != NULL) {
            memcpy(r, name, nl + 1);
        }
        return r;
    }
    bool slash = dir[dl - 1] == '/' || dir[dl - 1] == '\\';
    size_t total = dl + (slash ? 0 : 1) + nl + 1;
    char *r = pc_malloc_array(total, 1);
    if (r == NULL) {
        return NULL;
    }
    memcpy(r, dir, dl);
    size_t off = dl;
    if (!slash) {
        r[off++] = '/';
    }
    memcpy(r + off, name, nl);
    r[off + nl] = '\0';
    return r;
}

/* A file-name-safe version of a layer name, appended to *out (file_cmds::sanitize). */
static pc_status fc_sanitize(pc_buf *out, const char *name, pc_error *err)
{
    /* Build then trim surrounding whitespace and dots; empty -> "layer". */
    pc_buf tmp = PC_BUF_INIT;
    pc_status rc = PC_OK;
    for (const char *c = name; *c != '\0'; c++) {
        unsigned char ch = (unsigned char)*c;
        bool keep = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') || ch == '-' || ch == '_' || ch == ' ' || ch == '.' ||
                    ch >= 0x80; /* keep UTF-8 continuation/lead bytes (alphanumeric Unicode) */
        rc = pc_buf_append_byte(&tmp, keep ? ch : (uint8_t)'_', err);
        if (rc != PC_OK) {
            pc_buf_fini(&tmp);
            return rc;
        }
    }
    const char *s = pc_buf_cstr(&tmp);
    size_t len = tmp.len;
    size_t start = 0, end = len;
    while (start < end) {
        unsigned char c = (unsigned char)s[start];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '.') {
            start++;
        } else {
            break;
        }
    }
    while (end > start) {
        unsigned char c = (unsigned char)s[end - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '.') {
            end--;
        } else {
            break;
        }
    }
    if (end > start) {
        rc = pc_buf_append(out, s + start, end - start, err);
    } else {
        rc = pc_buf_append_str(out, "layer", err);
    }
    pc_buf_fini(&tmp);
    return rc;
}

/* Exports of the file-name helpers for the artboard exports (artboard_cmds.c, ET3). */
void pc_engine_file_stem(const char *path, char *out, size_t cap)
{
    fc_stem(path, out, cap);
}
char *pc_engine_file_join(const char *dir, const char *name)
{
    return fc_join(dir, name);
}
pc_status pc_engine_file_sanitize(pc_buf *out, const char *name, pc_error *err)
{
    return fc_sanitize(out, name, err);
}

/* ---- SaveOpts (file_cmds::SaveOpts); the struct is declared in engine_internal.h ---------- */

pc_engine_save_opts pc_engine_save_opts_default(void)
{
    return (pc_engine_save_opts){false, 0.0, false, true, true, (int)PC_DPX_BITS_AUTO};
}

/* "dpxBits": 8|10|12|16 (absent/null: auto). Anything else is a bad param of `cmd`. */
static pc_status fc_dpx_bits_param(yyjson_val *p, const char *cmd, int *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "dpxBits");
    if (v == NULL || yyjson_is_null(v)) {
        *out = (int)PC_DPX_BITS_AUTO;
        return PC_OK;
    }
    uint64_t u;
    if (pc_json_as_u64(v, &u)) {
        switch (u) {
        case 8:
            *out = (int)PC_DPX_BITS_8;
            return PC_OK;
        case 10:
            *out = (int)PC_DPX_BITS_10;
            return PC_OK;
        case 12:
            *out = (int)PC_DPX_BITS_12;
            return PC_OK;
        case 16:
            *out = (int)PC_DPX_BITS_16;
            return PC_OK;
        default:
            break;
        }
    }
    return pc_engine_err_bad_params(err, cmd, "`dpxBits` must be 8, 10, 12 or 16");
}

/* Build SaveOpts from a command's params and the session's File Handling preferences. */
pc_status pc_engine_save_opts_from_params(const pc_engine_session *s, yyjson_val *p, const char *cmd,
                                          pc_engine_save_opts *out, pc_error *err)
{
    pc_engine_save_opts o = pc_engine_save_opts_default();
    double q;
    if (fc_f64_param(p, "quality", &q)) {
        o.has_quality = true;
        o.quality = q;
    }
    o.tiff_layers = fc_bool_param(p, "tiffLayers", false);
    pc_status rc = fc_dpx_bits_param(p, cmd, &o.dpx_bits, err);
    if (rc != PC_OK) {
        return rc;
    }
    o.image_previews = fc_pref_not_never(s, "fileHandling.imagePreviews");
    o.psd_maximize_compatibility = fc_pref_not_never(s, "fileHandling.maximizePsdCompatibility");
    *out = o;
    return PC_OK;
}

/* ---- exported I/O primitives (engine_internal.h) ------------------------------------------ */

pc_status pc_engine_read_file(const char *path, pc_buf *out, pc_error *err)
{
    pc_status rc = pc_read_file(path, out, err);
    if (rc != PC_OK && err != NULL) {
        /* Prefix the path like the reference's read_file. */
        char saved[PC_ERROR_MSG_MAX];
        memcpy(saved, err->msg, sizeof saved);
        pc_fail_set(err, rc, "%s: %s", path, saved);
    }
    return rc;
}

pc_status pc_engine_write_file(const char *path, const void *data, size_t len, pc_error *err)
{
    return pc_atomic_write(path, data, len, err);
}

/* Import a file's bytes into *out (document only; warnings discarded). An Affinity preview-only
 * import is refused like the reference. Port of file_cmds::import. */
pc_status pc_engine_import(const char *name, const uint8_t *bytes, size_t len, pc_document *out,
                           pc_error *err)
{
    pc_io_import_result r;
    pc_status rc = pc_io_import(name, bytes, len, &r, err);
    if (rc != PC_OK) {
        if (err != NULL) {
            char saved[PC_ERROR_MSG_MAX];
            memcpy(saved, err->msg, sizeof saved);
            pc_fail_set(err, rc, "%s: %s", name, saved);
        }
        return rc;
    }
    if (r.preview_only) {
        pc_io_import_result_fini(&r);
        return pc_engine_err_other(
            err,
            "%s: only this Affinity file's embedded preview could be read; open it with File "
            "> Open to see the warning, or export PSD or PNG from Affinity before using it here",
            name);
    }
    *out = r.document;           /* move */
    pc_io_warnings_fini(&r.warnings);
    return PC_OK;
}

/* Apply the settings `doc`'s file was encoded with to encode opts (io::apply_file_encoding). */
static void fc_apply_file_encoding(const pc_document *doc, pc_cencode_opts *e)
{
    const pc_file_encoding *f = &doc->file_encoding;
    if (f->has_jpeg_quality) {
        uint8_t q = f->jpeg_quality;
        if (q < 1) {
            q = 1;
        }
        e->jpeg_quality = q;
    }
    if (f->has_webp_lossless) {
        e->webp_lossless = f->webp_lossless;
    }
    if (f->has_webp_quality) {
        e->webp_quality = f->webp_quality > 100 ? 100 : f->webp_quality;
    }
}

/* Encode `doc` for `path`'s extension into *out (owned bytes) with its warnings. Port of
 * file_cmds::encode. */
pc_status pc_engine_encode(const pc_document *doc, const char *path, const pc_engine_save_opts *save,
                           pc_buf *out, pc_io_warnings *warnings, pc_error *err)
{
    pc_engine_save_opts dflt = pc_engine_save_opts_default();
    if (save == NULL) {
        save = &dflt;
    }
    pc_io_export_opts opts = pc_io_export_opts_default();
    opts.tiff_layers = save->tiff_layers;
    opts.image_previews = save->image_previews;
    opts.psd_maximize_compatibility = save->psd_maximize_compatibility;
    fc_apply_file_encoding(doc, &opts.encode);
    opts.encode.dpx_bits = (pc_dpx_bits)save->dpx_bits;
    if (save->has_quality) {
        double qd = save->quality;
        if (qd < 0.0) {
            qd = 0.0;
        }
        if (qd > 12.0) {
            qd = 12.0;
        }
        uint8_t q = (uint8_t)(qd / 12.0 * 99.0 + 1.0 + 0.5);
        opts.encode.jpeg_quality = q;
        opts.encode.webp_quality = q;
        opts.encode.webp_lossless = false;
    }
    pc_io_export_result r;
    pc_status rc = pc_io_export(doc, path, &opts, &r, err);
    if (rc != PC_OK) {
        if (err != NULL) {
            char saved[PC_ERROR_MSG_MAX];
            memcpy(saved, err->msg, sizeof saved);
            pc_fail_set(err, rc, "%s: %s", path, saved);
        }
        return rc;
    }
    pc_buf_init(out);
    if (r.len > 0) {
        rc = pc_buf_append(out, r.bytes, r.len, err);
        if (rc != PC_OK) {
            pc_buf_fini(out);
            pc_io_export_result_fini(&r);
            return rc;
        }
    }
    if (warnings != NULL) {
        pc_io_warnings_init(warnings);
        (void)pc_io_warnings_append(warnings, &r.warnings, NULL);
    }
    pc_io_export_result_fini(&r);
    return PC_OK;
}

/* Encode and atomically write `doc` to `path`; *warnings (optional) gets export notes. */
pc_status pc_engine_save_doc(const pc_document *doc, const char *path, const pc_engine_save_opts *save,
                             pc_io_warnings *warnings, pc_error *err)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_encode(doc, path, save, &bytes, warnings, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_write_file(path, bytes.data, bytes.len, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK && warnings != NULL) {
        pc_io_warnings_fini(warnings);
    }
    return rc;
}

/* The flattened image of `doc` as a surface in `fmt`, placed at the origin (file_cmds::flattened).
 * On OOM returns a valid empty surface and fills err. */
pc_status pc_engine_flattened(const pc_document *doc, pc_pixel_format fmt, pc_surface *out,
                              pc_error *err)
{
    pc_surface_init(out, fmt);
    return pc_compose_flatten_to_surface(doc, fmt, NULL, out, err);
}

/* ---- JSON warnings array ------------------------------------------------------------------ */

static yyjson_mut_val *fc_warnings_json(yyjson_mut_doc *jd, const pc_io_warnings *w)
{
    yyjson_mut_val *arr = yyjson_mut_arr(jd);
    if (arr == NULL || w == NULL) {
        return arr;
    }
    for (size_t i = 0; i < w->n; i++) {
        yyjson_mut_arr_add_str(jd, arr, w->v[i] != NULL ? w->v[i] : "");
    }
    return arr;
}

/* ---- predicates --------------------------------------------------------------------------- */

static pc_status fc_has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_doc(s, reason);
}

/* can_revert: a document with a saved path that is a file on disk. */
static pc_status fc_can_revert(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "no document open");
        }
        return PC_ERR_INVALID;
    }
    if (d->path == NULL) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "the document has never been saved");
        }
        return PC_ERR_INVALID;
    }
    if (!fc_is_file(d->path)) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "%s is not a file on disk", d->path);
        }
        return PC_ERR_INVALID;
    }
    return PC_OK;
}

/* has_adjustments: a visible adjustment layer at the top level (colorLookupTables). */
static bool fc_doc_has_visible_adjustment(const pc_document *doc)
{
    for (size_t i = 0; i < doc->nlayers; i++) {
        if (doc->layers[i].visible && doc->layers[i].content.kind == PC_LAYER_ADJUSTMENT) {
            return true;
        }
    }
    return false;
}

static pc_status fc_has_adjustments(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "no document open");
        }
        return PC_ERR_INVALID;
    }
    if (!fc_doc_has_visible_adjustment(pc_doc_arc_get(d->doc))) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID,
                         "the document has no visible adjustment layers to export");
        }
        return PC_ERR_INVALID;
    }
    return PC_OK;
}

/* has_shape: the active layer is a shape layer (newGuidesFromShape). */
static pc_status fc_has_shape(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "the active layer is not a shape layer");
        }
        return PC_ERR_INVALID;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l == NULL || l->content.kind != PC_LAYER_SHAPE) {
        if (reason != NULL) {
            pc_fail_set(reason, PC_ERR_INVALID, "the active layer is not a shape layer");
        }
        return PC_ERR_INVALID;
    }
    return PC_OK;
}

/* ---- close / revert / save a copy / open as ----------------------------------------------- */

static pc_status fc_result_obj(pc_json_value *out, yyjson_mut_doc **jd, yyjson_mut_val **root,
                               pc_error *err)
{
    if (pc_engine_result_doc(out, jd, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    *root = yyjson_mut_obj(*jd);
    yyjson_mut_doc_set_root(*jd, *root);
    return PC_OK;
}

static pc_status run_close_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    size_t n = 0;
    (void)pc_engine_session_documents(s, &n);
    size_t closed = n;
    while (true) {
        size_t cur = 0;
        (void)pc_engine_session_documents(s, &cur);
        if (cur == 0) {
            break;
        }
        if (!pc_engine_session_close(s, cur - 1)) {
            break;
        }
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    pc_status rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_uint(jd, root, "closed", closed);
    return PC_OK;
}

static pc_status run_close_others(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                  pc_error *err)
{
    size_t n = 0;
    (void)pc_engine_session_documents(s, &n);
    size_t keep;
    yyjson_val *dv = pc_json_get(p, "document");
    if (dv != NULL) {
        uint64_t u;
        if (!pc_json_as_u64(dv, &u)) {
            return pc_engine_err_bad_params(err, "file.closeOthers", "`document` must be an index");
        }
        keep = (size_t)u;
    } else if (!pc_engine_session_active_index(s, &keep)) {
        return pc_engine_err_no_document(err);
    }
    if (keep >= n) {
        return pc_engine_err_bad_params(err, "file.closeOthers", "no document %zu", keep);
    }
    size_t closed = n - 1;
    for (size_t i = n; i-- > 0;) {
        if (i != keep) {
            (void)pc_engine_session_close(s, i);
        }
    }
    (void)pc_engine_session_set_active(s, 0);
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    pc_status rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_uint(jd, root, "closed", closed);
    return PC_OK;
}

/* Revert edit ctx: the fresh document (moved in on success). */
typedef struct {
    pc_document fresh; /* moved into the doc on success */
    bool consumed;
} fc_revert_ctx;

static pc_status fc_revert_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                pc_error *err)
{
    (void)err;
    fc_revert_ctx *c = vctx;
    /* Keep the identity and the tab name; replace everything else with the fresh import. */
    pc_doc_id id = doc->id;
    pc_str name = doc->name;        /* keep (move out before fini) */
    doc->name = (pc_str){0};
    pc_document_fini(doc);
    *doc = c->fresh;                /* move */
    c->consumed = true;
    doc->id = id;
    pc_str_fini(&doc->name);
    doc->name = name;               /* restore kept name */
    pc_layer_id top;
    if (pc_document_top_layer(doc, &top)) {
        *has_active = true;
        *active = top;
    } else {
        *has_active = false;
    }
    return PC_OK;
}

static pc_status run_revert(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || d->path == NULL) {
        return pc_engine_err_other(err, "the document has never been saved");
    }
    char *path = fc_strdup(d->path);
    if (path == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_read_file(path, &bytes, err);
    if (rc != PC_OK) {
        free(path);
        return rc;
    }
    fc_revert_ctx ctx = {0};
    rc = pc_engine_import(fc_file_name(path), bytes.data, bytes.len, &ctx.fresh, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        free(path);
        return rc;
    }
    rc = pc_engine_session_edit(s, "Revert", fc_revert_edit, &ctx, err);
    if (rc != PC_OK) {
        if (!ctx.consumed) {
            pc_document_fini(&ctx.fresh);
        }
        free(path);
        return rc;
    }
    /* The document is clean afterwards. */
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    size_t nlayers = 0;
    if (st != NULL) {
        st->saved_revision = st->revision;
        nlayers = pc_doc_arc_get(st->doc)->nlayers;
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        free(path);
        return rc;
    }
    yyjson_mut_obj_add_strcpy(jd, root, "path", path);
    yyjson_mut_obj_add_uint(jd, root, "layers", nlayers);
    free(path);
    return PC_OK;
}

static pc_status run_save_a_copy(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                 pc_error *err)
{
    const char *path = NULL;
    pc_status rc = fc_req_str(p, "path", "file.saveACopy", &path, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_engine_save_opts save;
    rc = pc_engine_save_opts_from_params(s, p, "file.saveACopy", &save, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_document doc;
    rc = pc_document_clone(pc_doc_arc_get(d->doc), &doc, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* "Layers" unchecked: write the flattened image as a single Background layer. */
    yyjson_val *lv = pc_json_get(p, "layers");
    bool flatten = false;
    bool lb;
    if (lv != NULL && pc_json_as_bool(lv, &lb) && !lb) {
        flatten = true;
    }
    if (flatten) {
        pc_pixel_format fmt = pc_document_pixel_format(&doc);
        pc_surface surf;
        rc = pc_engine_flattened(&doc, fmt, &surf, err);
        if (rc != PC_OK) {
            pc_surface_fini(&surf);
            pc_document_fini(&doc);
            return rc;
        }
        pc_layer bg;
        pc_layer_content content = {0};
        content.kind = PC_LAYER_RASTER;
        content.u.raster = surf;
        rc = pc_layer_init(&bg, "Background", &content, err);
        if (rc != PC_OK) {
            pc_layer_content_fini(&content);
            pc_document_fini(&doc);
            return rc;
        }
        rc = fc_set_single_layer(&doc, &bg, err);
        if (rc != PC_OK) {
            pc_document_fini(&doc);
            return rc;
        }
    }
    pc_io_warnings warnings;
    rc = pc_engine_save_doc(&doc, path, &save, &warnings, err);
    pc_document_fini(&doc);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        pc_io_warnings_fini(&warnings);
        return rc;
    }
    yyjson_mut_obj_add_strcpy(jd, root, "path", path);
    yyjson_mut_obj_add_val(jd, root, "warnings", fc_warnings_json(jd, &warnings));
    pc_io_warnings_fini(&warnings);
    return PC_OK;
}

/* Open As: read `path`, decode it as `as` (when given) by swapping the extension, add it as a new
 * active document. Color-management open policy (preserve/convert on profile mismatch) is deferred
 * to an E8 follow-up; the report is {"action":"opened"}. */
static pc_status run_open_as(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *path = NULL;
    pc_status rc = fc_req_str(p, "path", "file.openAs", &path, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *as_ext = fc_str_param(p, "as", NULL);
    if (as_ext == NULL) {
        as_ext = fc_str_param(p, "format", NULL);
    }
    pc_buf bytes = PC_BUF_INIT;
    rc = pc_engine_read_file(path, &bytes, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Decode name: "<stem>.<as>" for Open As, else the file name. */
    char decode[1024];
    if (as_ext != NULL) {
        char stem[512];
        fc_stem(path, stem, sizeof stem);
        const char *e = as_ext;
        while (*e == '.') {
            e++;
        }
        (void)snprintf(decode, sizeof decode, "%s.%s", stem, e);
    } else {
        (void)snprintf(decode, sizeof decode, "%s", fc_file_name(path));
    }
    pc_io_import_result r;
    rc = pc_io_import(decode, bytes.data, bytes.len, &r, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        if (err != NULL) {
            char saved[PC_ERROR_MSG_MAX];
            memcpy(saved, err->msg, sizeof saved);
            pc_fail_set(err, rc, "%s: %s", decode, saved);
        }
        return rc;
    }
    /* Keep the document's name as the real file name. */
    rc = pc_str_set_c(&r.document.name, fc_file_name(path), err);
    if (rc != PC_OK) {
        pc_io_import_result_fini(&r);
        return rc;
    }
    pc_document doc = r.document; /* move */
    bool read_only = r.source_read_only;
    pc_io_warnings warnings = r.warnings; /* move */
    size_t index = 0;
    rc = pc_engine_session_add_document(s, &doc, read_only ? NULL : path, &index, err);
    if (rc != PC_OK) {
        pc_document_fini(&doc);
        pc_io_warnings_fini(&warnings);
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->source_read_only = read_only;
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        pc_io_warnings_fini(&warnings);
        return rc;
    }
    yyjson_mut_obj_add_uint(jd, root, "document", index);
    yyjson_mut_val *color = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_str(jd, color, "action", "opened");
    yyjson_mut_obj_add_val(jd, root, "color", color);
    yyjson_mut_obj_add_val(jd, root, "warnings", fc_warnings_json(jd, &warnings));
    pc_io_warnings_fini(&warnings);
    return PC_OK;
}

/* ---- place -------------------------------------------------------------------------------- */

typedef struct {
    pc_surface px;       /* the placed, scaled, translated pixels (moved into the layer) */
    bool consumed;
    bool smart;
    bool vector;
    bool linked;
    const char *link_path;
    const char *layer_name;
    const char *src_file_name;
    const uint8_t *embed_bytes;
    size_t embed_len;
    double scale, dx, dy;
    pc_layer_id result_id;
} fc_place_ctx;

static pc_status fc_place_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                               pc_error *err)
{
    fc_place_ctx *c = vctx;
    pc_layer layer;
    pc_status rc;
    if (c->smart) {
        pc_smart_source source = {0};
        if (c->linked) {
            source.linked = true;
            rc = pc_str_set_c(&source.path, c->link_path, err);
            if (rc != PC_OK) {
                pc_str_fini(&source.path);
                return rc;
            }
        } else {
            source.linked = false;
            rc = pc_str_set_c(&source.file_name, c->src_file_name, err);
            if (rc != PC_OK) {
                pc_str_fini(&source.file_name);
                return rc;
            }
            if (c->embed_len > 0) {
                rc = pc_blob_new(c->embed_bytes, c->embed_len, &source.bytes, err);
                if (rc != PC_OK) {
                    pc_str_fini(&source.file_name);
                    pc_blob_release(source.bytes);
                    return rc;
                }
            }
        }
        pc_affine tf = {{c->scale, 0.0, 0.0, c->scale, c->dx, c->dy}};
        pc_surface cache = c->px; /* move */
        c->consumed = true;
        pc_smart_object so;
        pc_smart_object_init(&so, &source, tf, &cache);
        /* A vector (SVG) source renders sharp at its placement scale. */
        if (c->vector) {
            bool avail = false;
            pc_surface sharp;
            if (pc_engine_smart_render(doc, &so, &avail, &sharp, NULL) == PC_OK && avail) {
                if (so.has_cache) {
                    pc_surface_fini(&so.cache);
                }
                so.cache = sharp;
                so.has_cache = true;
            } else if (avail) {
                pc_surface_fini(&sharp);
            }
        }
        pc_layer_content content = {0};
        content.kind = PC_LAYER_SMART;
        content.u.smart = so;
        rc = pc_layer_init(&layer, c->layer_name, &content, err);
        if (rc != PC_OK) {
            pc_layer_content_fini(&content);
            return rc;
        }
    } else {
        pc_layer_content content = {0};
        content.kind = PC_LAYER_RASTER;
        content.u.raster = c->px; /* move */
        c->consumed = true;
        rc = pc_layer_init(&layer, c->layer_name, &content, err);
        if (rc != PC_OK) {
            pc_layer_content_fini(&content);
            return rc;
        }
    }
    const pc_layer_id *above = *has_active ? active : NULL;
    pc_layer_id id;
    rc = pc_document_insert_above(doc, above, &layer, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    c->result_id = id;
    *has_active = true;
    *active = id;
    return PC_OK;
}

/* place_bytes: place a file's bytes as a smart object (or plain pixels), centred and fit-scaled. */
static pc_status fc_place_bytes(pc_engine_session *s, const char *name, const uint8_t *bytes,
                                size_t len, bool linked, const char *link_path, yyjson_val *p,
                                pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *active_doc = pc_doc_arc_get(d->doc);
    double cw = (double)active_doc->size.width, ch = (double)active_doc->size.height;
    pc_pixel_format fmt = pc_document_pixel_format(active_doc);
    bool vector = pc_io_is_svg(bytes, len);

    pc_document src = {0};
    pc_status rc = pc_engine_import(name, bytes, len, &src, err);
    if (rc != PC_OK) {
        return rc;
    }
    double w = (double)src.size.width, h = (double)src.size.height;
    bool fit;
    yyjson_val *fv = pc_json_get(p, "fit");
    bool fb;
    if (fv != NULL && pc_json_as_bool(fv, &fb)) {
        fit = fb;
    } else {
        fit = fc_pref_bool(s, "general.resizeImageDuringPlace", true);
    }
    double scale;
    double k;
    if (fc_f64_param(p, "scale", &k)) {
        scale = k / 100.0;
        if (scale < 1e-4) {
            scale = 1e-4;
        }
    } else if (fit && (w > cw || h > ch) && w > 0.0 && h > 0.0) {
        double a = cw / w, b = ch / h;
        scale = a < b ? a : b;
    } else {
        scale = 1.0;
    }
    double centerx = cw / 2.0, centery = ch / 2.0;
    yyjson_val *cv = pc_json_get(p, "center");
    if (cv != NULL && yyjson_is_arr(cv) && yyjson_arr_size(cv) == 2) {
        double a, b;
        if (pc_json_as_f64(yyjson_arr_get(cv, 0), &a) && isfinite(a)) {
            centerx = a;
        }
        if (pc_json_as_f64(yyjson_arr_get(cv, 1), &b) && isfinite(b)) {
            centery = b;
        }
    }
    double dx = round(centerx - w * scale / 2.0);
    double dy = round(centery - h * scale / 2.0);

    /* The flattened source in the document's format, then scaled and translated. */
    pc_surface flat;
    rc = pc_engine_flattened(&src, fmt, &flat, err);
    pc_document_fini(&src);
    if (rc != PC_OK) {
        pc_surface_fini(&flat);
        return rc;
    }
    pc_surface px;
    if (fabs(scale - 1.0) > 1e-9) {
        rc = pc_algo_resize_surface(&flat, scale, scale, PC_ALGO_RESAMPLE_BICUBIC, &px, err);
        pc_surface_fini(&flat);
        if (rc != PC_OK) {
            pc_surface_fini(&px);
            return rc;
        }
    } else {
        px = flat;
    }
    pc_surface placed;
    rc = pc_algo_translate_surface(&px, (int32_t)dx, (int32_t)dy, &placed, err);
    pc_surface_fini(&px);
    if (rc != PC_OK) {
        pc_surface_fini(&placed);
        return rc;
    }

    bool smart;
    if (linked) {
        smart = true;
    } else {
        yyjson_val *sv = pc_json_get(p, "smartObject");
        bool sb;
        if (sv != NULL && pc_json_as_bool(sv, &sb)) {
            smart = sb;
        } else {
            smart = fc_pref_bool(s, "general.alwaysCreateSmartObjectsWhenPlacing", true);
        }
    }

    char layer_name[512];
    fc_stem(name, layer_name, sizeof layer_name);
    fc_place_ctx ctx = {0};
    ctx.px = placed;
    ctx.smart = smart;
    ctx.vector = vector;
    ctx.linked = linked;
    ctx.link_path = link_path;
    ctx.layer_name = layer_name;
    ctx.src_file_name = fc_file_name(name);
    ctx.embed_bytes = bytes;
    ctx.embed_len = len;
    ctx.scale = scale;
    ctx.dx = dx;
    ctx.dy = dy;
    const char *label = linked ? "Place Linked" : "Place Embedded";
    rc = pc_engine_session_edit(s, label, fc_place_edit, &ctx, err);
    if (rc != PC_OK) {
        if (!ctx.consumed) {
            pc_surface_fini(&ctx.px);
        }
        return rc;
    }

    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_uint(jd, root, "layer", ctx.result_id);
    yyjson_mut_obj_add_real(jd, root, "scale", scale * 100.0);
    yyjson_mut_val *b = yyjson_mut_arr(jd);
    yyjson_mut_arr_add_real(jd, b, dx);
    yyjson_mut_arr_add_real(jd, b, dy);
    yyjson_mut_arr_add_real(jd, b, dx + w * scale);
    yyjson_mut_arr_add_real(jd, b, dy + h * scale);
    yyjson_mut_obj_add_val(jd, root, "bounds", b);
    yyjson_mut_obj_add_bool(jd, root, "smartObject", smart);
    return PC_OK;
}

static pc_status run_place(pc_engine_session *s, yyjson_val *p, bool linked, pc_json_value *out,
                           pc_error *err)
{
    const char *cmd = linked ? "file.placeLinked" : "file.placeEmbedded";
    const char *path = NULL;
    pc_status rc = fc_req_str(p, "path", cmd, &path, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_buf bytes = PC_BUF_INIT;
    rc = pc_engine_read_file(path, &bytes, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = fc_place_bytes(s, path, bytes.data, bytes.len, linked, linked ? path : NULL, p, out, err);
    pc_buf_fini(&bytes);
    return rc;
}

static pc_status run_place_embedded(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    return run_place(s, p, false, out, err);
}

static pc_status run_place_linked(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                  pc_error *err)
{
    return run_place(s, p, true, out, err);
}

/* ---- Automate: Fit Image / Conditional Mode Change ---------------------------------------- */

static pc_status run_fit_image(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    double w = (double)doc->size.width, h = (double)doc->size.height;
    double bw = w, bh = h;
    double v;
    if (fc_f64_param(p, "width", &v) && v > 0.0) {
        bw = v;
    }
    if (fc_f64_param(p, "height", &v) && v > 0.0) {
        bh = v;
    }
    if (bw < 1.0) {
        bw = 1.0;
    }
    if (bh < 1.0) {
        bh = 1.0;
    }
    double a = bw / w, b = bh / h;
    double k = a < b ? a : b;
    bool dont_enlarge = fc_bool_param(p, "dontEnlarge", false);
    if ((k > 1.0 && dont_enlarge) || (round(w * k) == w && round(h * k) == h)) {
        yyjson_mut_doc *jd = NULL;
        yyjson_mut_val *root = NULL;
        pc_status rc = fc_result_obj(out, &jd, &root, err);
        if (rc != PC_OK) {
            return rc;
        }
        yyjson_mut_obj_add_real(jd, root, "width", w);
        yyjson_mut_obj_add_real(jd, root, "height", h);
        yyjson_mut_obj_add_bool(jd, root, "changed", false);
        return PC_OK;
    }
    double nw = round(w * k), nh = round(h * k);
    if (nw < 1.0) {
        nw = 1.0;
    }
    if (nh < 1.0) {
        nh = 1.0;
    }
    const char *resample = fc_str_param(p, "resample", "bicubic");
    char params[512];
    (void)snprintf(params, sizeof params, "{\"width\":%.0f,\"height\":%.0f,\"resample\":\"%s\"}", nw,
                   nh, resample);
    pc_json_value sub = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, "image.imageSize", params, &sub, err);
    pc_json_value_fini(&sub);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_real(jd, root, "width", nw);
    yyjson_mut_obj_add_real(jd, root, "height", nh);
    yyjson_mut_obj_add_bool(jd, root, "changed", true);
    return PC_OK;
}

static const char *fc_mode_name(pc_color_mode m)
{
    switch (m) {
    case PC_COLOR_MODE_BITMAP:
        return "bitmap";
    case PC_COLOR_MODE_GRAYSCALE:
        return "grayscale";
    case PC_COLOR_MODE_DUOTONE:
        return "duotone";
    case PC_COLOR_MODE_INDEXED:
        return "indexed";
    case PC_COLOR_MODE_RGB:
        return "rgb";
    case PC_COLOR_MODE_CMYK:
        return "cmyk";
    case PC_COLOR_MODE_LAB:
        return "lab";
    case PC_COLOR_MODE_MULTICHANNEL:
        return "multichannel";
    default:
        return "rgb";
    }
}

/* Lower-case s into buf[cap] with "gray" normalised to "grayscale". */
static void fc_mode_norm(const char *s, char *buf, size_t cap)
{
    if (s == NULL) {
        buf[0] = '\0';
        return;
    }
    size_t i = 0;
    for (const char *c = s; *c != '\0' && i + 1 < cap; c++) {
        int ch = (unsigned char)*c;
        if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
        }
        buf[i++] = (char)ch;
    }
    buf[i] = '\0';
    if (strcmp(buf, "gray") == 0) {
        (void)snprintf(buf, cap, "grayscale");
    }
}

static pc_status run_conditional_mode_change(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                             pc_error *err)
{
    const char *cmd = "file.automate.conditionalModeChange";
    const char *to_raw = NULL;
    pc_status rc = fc_req_str(p, "to", cmd, &to_raw, err);
    if (rc != PC_OK) {
        return rc;
    }
    char tob[32];
    fc_mode_norm(to_raw, tob, sizeof tob);
    const char *to;
    if (strcmp(tob, "grayscale") == 0) {
        to = "grayscale";
    } else if (strcmp(tob, "rgb") == 0) {
        to = "rgb";
    } else if (strcmp(tob, "cmyk") == 0) {
        to = "cmyk";
    } else if (strcmp(tob, "lab") == 0) {
        to = "lab";
    } else {
        return pc_engine_err_bad_params(
            err, cmd, "unsupported target mode \"%s\" (rgb, grayscale, cmyk, lab)", to_raw);
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const char *cur = fc_mode_name(pc_doc_arc_get(d->doc)->mode);
    /* "from" filter: array, a single non-"any" string, or "any". */
    bool match = false;
    yyjson_val *fromv = pc_json_get(p, "from");
    if (fromv != NULL && yyjson_is_arr(fromv)) {
        yyjson_arr_iter it = yyjson_arr_iter_with(fromv);
        yyjson_val *e;
        while ((e = yyjson_arr_iter_next(&it)) != NULL) {
            size_t el = 0;
            const char *es = pc_json_as_str(e, &el);
            if (es == NULL) {
                continue;
            }
            char nb[32];
            fc_mode_norm(es, nb, sizeof nb);
            if (strcmp(nb, "any") == 0 || strcmp(nb, cur) == 0) {
                match = true;
                break;
            }
        }
    } else if (fromv != NULL && yyjson_is_str(fromv)) {
        char nb[32];
        fc_mode_norm(yyjson_get_str(fromv), nb, sizeof nb);
        match = strcmp(nb, "any") == 0 || strcmp(nb, cur) == 0;
    } else {
        match = true; /* default "any" */
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    if (strcmp(cur, to) == 0 || !match) {
        rc = fc_result_obj(out, &jd, &root, err);
        if (rc != PC_OK) {
            return rc;
        }
        yyjson_mut_obj_add_bool(jd, root, "changed", false);
        yyjson_mut_obj_add_strcpy(jd, root, "mode", cur);
        return PC_OK;
    }
    char id[64];
    (void)snprintf(id, sizeof id, "image.mode.%s", to);
    pc_json_value sub = PC_JSON_VALUE_INIT;
    rc = pc_engine_session_execute_json(s, id, "{}", &sub, err);
    pc_json_value_fini(&sub);
    if (rc != PC_OK) {
        return rc;
    }
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_bool(jd, root, "changed", true);
    yyjson_mut_obj_add_strcpy(jd, root, "mode", to);
    return PC_OK;
}

/* ---- Export: Layers to Files -------------------------------------------------------------- */

static pc_status run_layers_to_files(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    const char *cmd = "file.export.layersToFiles";
    const char *dir = fc_str_param(p, "dir", NULL);
    if (dir == NULL) {
        dir = fc_str_param(p, "output", NULL);
    }
    if (dir == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing \"dir\"");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_engine_save_opts save;
    pc_status rc = pc_engine_save_opts_from_params(s, p, cmd, &save, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *format_raw = fc_str_param(p, "format", "png");
    char format[32];
    {
        const char *f = format_raw;
        while (*f == '.') {
            f++;
        }
        fc_mode_norm(f, format, sizeof format); /* reuse for lower-casing (no "gray" here) */
        if (strcmp(format, "grayscale") == 0) {
            (void)snprintf(format, sizeof format, "gray"); /* not an image ext; harmless */
        }
    }
    bool visible_only = fc_bool_param(p, "visibleOnly", true);

    const pc_document *doc = pc_doc_arc_get(d->doc);
    /* prefix: given, else stem of the document name. */
    char prefix[512];
    const char *pfx = fc_str_param(p, "prefix", NULL);
    if (pfx != NULL) {
        (void)snprintf(prefix, sizeof prefix, "%s", pfx);
    } else {
        fc_stem(pc_str_c(&doc->name), prefix, sizeof prefix);
    }

    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *files = yyjson_mut_arr(jd);

    /* Top-level layers, top to bottom like Photoshop's numbering. */
    size_t n = doc->nlayers;
    size_t idx = 0;
    for (size_t ri = 0; ri < n; ri++) {
        const pc_layer *l = &doc->layers[n - 1 - ri];
        if (!(l->visible || !visible_only)) {
            continue;
        }
        if (l->content.kind == PC_LAYER_ADJUSTMENT) {
            continue;
        }
        /* Build a one-layer document (the layer forced visible, unclipped). */
        pc_document one;
        rc = pc_document_clone(doc, &one, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer only;
        rc = pc_layer_clone(l, &only, err);
        if (rc != PC_OK) {
            pc_document_fini(&one);
            return rc;
        }
        only.visible = true;
        only.clipped = false;
        rc = fc_set_single_layer(&one, &only, err);
        if (rc != PC_OK) {
            pc_document_fini(&one);
            return rc;
        }
        /* name = "<prefix>_NNNN_<layer>.<format>". */
        pc_buf nameb = PC_BUF_INIT;
        rc = fc_sanitize(&nameb, prefix, err);
        if (rc == PC_OK) {
            rc = pc_buf_appendf(&nameb, err, "_%04zu_", idx);
        }
        if (rc == PC_OK) {
            rc = fc_sanitize(&nameb, pc_str_c(&l->name), err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_appendf(&nameb, err, ".%s", format);
        }
        if (rc != PC_OK) {
            pc_buf_fini(&nameb);
            pc_document_fini(&one);
            return rc;
        }
        char *fname = fc_strdup(pc_buf_cstr(&nameb));
        pc_buf_fini(&nameb);
        if (fname == NULL) {
            pc_document_fini(&one);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        pc_buf bytes = PC_BUF_INIT;
        rc = pc_engine_encode(&one, fname, &save, &bytes, NULL, err);
        pc_document_fini(&one);
        if (rc != PC_OK) {
            free(fname);
            pc_buf_fini(&bytes);
            return rc;
        }
        char *full = fc_join(dir, fname);
        free(fname);
        if (full == NULL) {
            pc_buf_fini(&bytes);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        rc = pc_engine_write_file(full, bytes.data, bytes.len, err);
        pc_buf_fini(&bytes);
        if (rc != PC_OK) {
            free(full);
            return rc;
        }
        yyjson_mut_arr_add_strcpy(jd, files, full);
        free(full);
        idx++;
    }
    yyjson_mut_obj_add_val(jd, root, "files", files);
    return PC_OK;
}

/* ---- Export: Color Lookup Tables (.cube) -------------------------------------------------- */

/* Bake the visible top-level adjustment stack against an identity RGB lattice into a .cube text
 * appended to *cube. */
static pc_status fc_bake_cube(const pc_document *doc, size_t size, const char *title, pc_buf *cube,
                              pc_error *err)
{
    size_t nn = size;
    uint32_t w = (uint32_t)(nn * nn), h = (uint32_t)nn;
    pc_document lattice;
    pc_status rc =
        pc_document_init(&lattice, "lut", pc_size_make(w, h), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format fmt = pc_document_pixel_format(&lattice);
    size_t nch = pc_pixel_format_channels(fmt);
    double step = 1.0 / (double)(nn - 1);
    size_t npx = (size_t)w * h;
    float *data = pc_malloc_array(npx * nch, sizeof(float));
    if (data == NULL) {
        pc_document_fini(&lattice);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory building the colour lattice");
    }
    size_t pi = 0;
    for (size_t g = 0; g < nn; g++) {
        for (size_t b = 0; b < nn; b++) {
            for (size_t r = 0; r < nn; r++) {
                float rgba[4] = {(float)((double)r * step), (float)((double)g * step),
                                 (float)((double)b * step), 1.0f};
                float enc[PC_RASTER_MAX_CHANNELS];
                size_t m = pc_from_rgba(fmt, rgba, enc);
                for (size_t k = 0; k < nch; k++) {
                    data[pi * nch + k] = k < m ? enc[k] : 0.0f;
                }
                pi++;
            }
        }
    }
    pc_surface surf;
    pc_surface_init(&surf, fmt);
    rc = pc_surface_write_region(&surf, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), data, npx * nch,
                                 err);
    free(data);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        pc_document_fini(&lattice);
        return rc;
    }
    pc_layer lat;
    pc_layer_content lc = {0};
    lc.kind = PC_LAYER_RASTER;
    lc.u.raster = surf;
    rc = pc_layer_init(&lat, "Lattice", &lc, err);
    if (rc != PC_OK) {
        pc_layer_content_fini(&lc);
        pc_document_fini(&lattice);
        return rc;
    }
    pc_layer_id latid;
    rc = pc_document_insert_above(&lattice, NULL, &lat, &latid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&lat);
        pc_document_fini(&lattice);
        return rc;
    }
    /* Append visible top-level adjustment layers (masks/clipping stripped). */
    for (size_t i = 0; i < doc->nlayers; i++) {
        const pc_layer *l = &doc->layers[i];
        if (!(l->visible && l->content.kind == PC_LAYER_ADJUSTMENT)) {
            continue;
        }
        pc_layer a;
        rc = pc_layer_clone(l, &a, err);
        if (rc != PC_OK) {
            pc_document_fini(&lattice);
            return rc;
        }
        if (a.has_mask) {
            pc_layer_mask_fini(&a.mask);
            a.has_mask = false;
        }
        if (a.has_vector_mask) {
            pc_vector_mask_fini(&a.vector_mask);
            a.has_vector_mask = false;
        }
        a.clipped = false;
        pc_layer_id aid;
        rc = pc_document_insert_above(&lattice, NULL, &a, &aid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&a);
            pc_document_fini(&lattice);
            return rc;
        }
    }
    pc_compose_buffer buf;
    rc = pc_compose_flatten(&lattice, &buf, err);
    pc_document_fini(&lattice);
    if (rc != PC_OK) {
        return rc;
    }
    /* Header. */
    pc_buf ttl = PC_BUF_INIT;
    for (const char *c = title; *c != '\0'; c++) {
        (void)pc_buf_append_byte(&ttl, *c == '"' ? (uint8_t)'\'' : (uint8_t)*c, NULL);
    }
    rc = pc_buf_appendf(cube, err,
                        "TITLE \"%s\"\n# Created by Photocraft\nLUT_3D_SIZE %zu\nDOMAIN_MIN 0.0 "
                        "0.0 0.0\nDOMAIN_MAX 1.0 1.0 1.0\n",
                        pc_buf_cstr(&ttl), nn);
    pc_buf_fini(&ttl);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return rc;
    }
    /* .cube order: red fastest, then green, then blue. The lattice stores r at x=b*n+r, y=g. */
    for (size_t b = 0; b < nn; b++) {
        for (size_t g = 0; g < nn; g++) {
            for (size_t r = 0; r < nn; r++) {
                size_t x = b * nn + r, y = g;
                size_t idx2 = y * (size_t)w + x;
                float px0 = 0, px1 = 0, px2 = 0;
                if (idx2 < buf.len) {
                    px0 = buf.px[idx2][0];
                    px1 = buf.px[idx2][1];
                    px2 = buf.px[idx2][2];
                }
                double c0 = px0 < 0 ? 0 : px0 > 1 ? 1 : px0;
                double c1 = px1 < 0 ? 0 : px1 > 1 ? 1 : px1;
                double c2 = px2 < 0 ? 0 : px2 > 1 ? 1 : px2;
                rc = pc_buf_appendf(cube, err, "%.6f %.6f %.6f\n", c0, c1, c2);
                if (rc != PC_OK) {
                    pc_compose_buffer_fini(&buf);
                    return rc;
                }
            }
        }
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

static pc_status run_color_lookup_tables(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    const char *cmd = "file.export.colorLookupTables";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    uint64_t size = 33;
    yyjson_val *sv = pc_json_get(p, "size");
    if (sv != NULL && !yyjson_is_null(sv)) {
        if (!pc_json_as_u64(sv, &size)) {
            return pc_engine_err_bad_params(err, cmd, "size must be an integer");
        }
    }
    if (size < 2 || size > PC_LUTFILE_MAX_SIZE) {
        return pc_engine_err_bad_params(err, cmd, "size must be 2..=%d", PC_LUTFILE_MAX_SIZE);
    }
    if (!fc_doc_has_visible_adjustment(doc)) {
        return pc_engine_err_bad_params(err, cmd, "no visible adjustment layers to export");
    }
    /* Count the layers exported (visible top-level adjustments). */
    size_t count = 0;
    for (size_t i = 0; i < doc->nlayers; i++) {
        if (doc->layers[i].visible && doc->layers[i].content.kind == PC_LAYER_ADJUSTMENT) {
            count++;
        }
    }
    char title[512];
    const char *t = fc_str_param(p, "title", NULL);
    if (t != NULL) {
        (void)snprintf(title, sizeof title, "%s", t);
    } else {
        fc_stem(pc_str_c(&doc->name), title, sizeof title);
    }
    pc_buf cube = PC_BUF_INIT;
    pc_status rc = fc_bake_cube(doc, (size_t)size, title, &cube, err);
    if (rc != PC_OK) {
        pc_buf_fini(&cube);
        return rc;
    }
    const char *path = fc_str_param(p, "path", NULL);
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    if (path != NULL) {
        rc = pc_engine_write_file(path, cube.data, cube.len, err);
        pc_buf_fini(&cube);
        if (rc != PC_OK) {
            return rc;
        }
        rc = fc_result_obj(out, &jd, &root, err);
        if (rc != PC_OK) {
            return rc;
        }
        yyjson_mut_obj_add_strcpy(jd, root, "path", path);
        yyjson_mut_obj_add_uint(jd, root, "size", size);
        yyjson_mut_obj_add_uint(jd, root, "layerCount", count);
        return PC_OK;
    }
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        pc_buf_fini(&cube);
        return rc;
    }
    yyjson_mut_obj_add_strcpy(jd, root, "cube", pc_buf_cstr(&cube));
    pc_buf_fini(&cube);
    yyjson_mut_obj_add_uint(jd, root, "size", size);
    yyjson_mut_obj_add_uint(jd, root, "layerCount", count);
    return PC_OK;
}

/* ---- guides ------------------------------------------------------------------------------- */

static void fc_push_unique(double **v, size_t *n, size_t *cap, double x)
{
    for (size_t i = 0; i < *n; i++) {
        if (fabs((*v)[i] - x) < 0.01) {
            return;
        }
    }
    if (*n == *cap) {
        double *nv = pc_grow_array(*v, cap, *n + 1, sizeof(double));
        if (nv == NULL) {
            return; /* best-effort */
        }
        *v = nv;
    }
    (*v)[(*n)++] = x;
}

typedef struct {
    double x0, y0, x1, y1;
} fc_bounds;

static pc_status fc_guides_from_shape_edit(pc_document *doc, bool *has_active, pc_layer_id *active,
                                           void *vctx, pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    fc_bounds *b = vctx;
    size_t cap_v = doc->guides.nvertical, cap_h = doc->guides.nhorizontal;
    double xs[3] = {b->x0, (b->x0 + b->x1) / 2.0, b->x1};
    double ys[3] = {b->y0, (b->y0 + b->y1) / 2.0, b->y1};
    for (int i = 0; i < 3; i++) {
        fc_push_unique(&doc->guides.vertical, &doc->guides.nvertical, &cap_v, xs[i]);
    }
    for (int i = 0; i < 3; i++) {
        fc_push_unique(&doc->guides.horizontal, &doc->guides.nhorizontal, &cap_h, ys[i]);
    }
    return PC_OK;
}

static pc_status run_guides_from_shape(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                       pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    /* Target the given layer, or the active one. */
    pc_layer_id id;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL) {
        uint64_t u;
        if (!pc_json_as_u64(lv, &u)) {
            return pc_engine_err_bad_params(err, "view.newGuidesFromShape", "`layer` must be an id");
        }
        id = (pc_layer_id)u;
    } else if (d->has_active_layer) {
        id = d->active_layer;
    } else {
        return pc_engine_err_other(err, "the layer is not a shape layer");
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_SHAPE) {
        return pc_engine_err_other(err, "the layer is not a shape layer");
    }
    double bounds[4];
    if (!pc_path_control_bounds(&l->content.u.shape.path, bounds)) {
        return pc_engine_err_other(err, "the shape has no path");
    }
    fc_bounds b = {bounds[0], bounds[1], bounds[2], bounds[3]};
    pc_status rc = pc_engine_session_edit(s, "New Guides From Shape", fc_guides_from_shape_edit, &b, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(jd);
    yyjson_mut_arr_add_real(jd, arr, b.x0);
    yyjson_mut_arr_add_real(jd, arr, b.y0);
    yyjson_mut_arr_add_real(jd, arr, b.x1);
    yyjson_mut_arr_add_real(jd, arr, b.y1);
    yyjson_mut_obj_add_val(jd, root, "bounds", arr);
    return PC_OK;
}

/* Whether x falls inside any artboard rect (clear_canvas_guides keeps those). */
static bool fc_in_artboard_x(const pc_document *doc, double x)
{
    for (size_t i = 0; i < doc->nlayers; i++) {
        const pc_layer *l = &doc->layers[i];
        if (l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
            pc_rect r = l->content.u.group.artboard.rect;
            if (x > (double)r.x0 && x < (double)r.x1) {
                return true;
            }
        }
    }
    return false;
}

static bool fc_in_artboard_y(const pc_document *doc, double y)
{
    for (size_t i = 0; i < doc->nlayers; i++) {
        const pc_layer *l = &doc->layers[i];
        if (l->content.kind == PC_LAYER_GROUP && l->content.u.group.has_artboard) {
            pc_rect r = l->content.u.group.artboard.rect;
            if (y > (double)r.y0 && y < (double)r.y1) {
                return true;
            }
        }
    }
    return false;
}

static pc_status fc_clear_guides_edit(pc_document *doc, bool *has_active, pc_layer_id *active,
                                      void *vctx, pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)vctx;
    (void)err;
    size_t w = 0;
    for (size_t i = 0; i < doc->guides.nvertical; i++) {
        if (fc_in_artboard_x(doc, doc->guides.vertical[i])) {
            doc->guides.vertical[w++] = doc->guides.vertical[i];
        }
    }
    doc->guides.nvertical = w;
    w = 0;
    for (size_t i = 0; i < doc->guides.nhorizontal; i++) {
        if (fc_in_artboard_y(doc, doc->guides.horizontal[i])) {
            doc->guides.horizontal[w++] = doc->guides.horizontal[i];
        }
    }
    doc->guides.nhorizontal = w;
    pc_guides_prune_colors(&doc->guides);
    return PC_OK;
}

static pc_status run_clear_canvas_guides(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    size_t cleared = 0;
    for (size_t i = 0; i < doc->guides.nvertical; i++) {
        if (!fc_in_artboard_x(doc, doc->guides.vertical[i])) {
            cleared++;
        }
    }
    for (size_t i = 0; i < doc->guides.nhorizontal; i++) {
        if (!fc_in_artboard_y(doc, doc->guides.horizontal[i])) {
            cleared++;
        }
    }
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *root = NULL;
    pc_status rc;
    if (cleared == 0) {
        rc = fc_result_obj(out, &jd, &root, err);
        if (rc != PC_OK) {
            return rc;
        }
        yyjson_mut_obj_add_uint(jd, root, "cleared", 0);
        return PC_OK;
    }
    rc = pc_engine_session_edit(s, "Clear Canvas Guides", fc_clear_guides_edit, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = fc_result_obj(out, &jd, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_uint(jd, root, "cleared", cleared);
    return PC_OK;
}

/* ---- File Info (XMP) ---------------------------------------------------------------------- *
 * Port of file_cmds::read_file_info / camera_data / write_file_info. All scanning is index-bounded
 * (hostile XMP must never crash); the edited fields are rewritten into their own rdf:Description. */

/* The File Info fields we edit: (JSON key, XMP property, container kind). */
typedef struct {
    const char *key;
    const char *prop;
    const char *kind; /* "Alt" | "Seq" | "Bag" | "" */
} fc_info_field;

static const fc_info_field FC_INFO_FIELDS[] = {
    {"title", "dc:title", "Alt"},        {"author", "dc:creator", "Seq"},
    {"description", "dc:description", "Alt"}, {"keywords", "dc:subject", "Bag"},
    {"copyright", "dc:rights", "Alt"},   {"authorTitle", "photoshop:AuthorsPosition", ""},
};
#define FC_NINFO (sizeof FC_INFO_FIELDS / sizeof FC_INFO_FIELDS[0])

static pc_status fc_xml_escape(pc_buf *b, const char *s, size_t len, pc_error *err)
{
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        default: break;
        }
        pc_status rc = rep != NULL ? pc_buf_append_str(b, rep, err) : pc_buf_append_byte(b, (uint8_t)c, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* XML-unescape s[0..len) into a freshly malloc'd NUL-terminated string (NULL on OOM). */
static char *fc_xml_unescape(const char *s, size_t len)
{
    pc_buf b = PC_BUF_INIT;
    size_t i = 0;
    while (i < len) {
        if (s[i] == '&') {
            if (len - i >= 5 && memcmp(s + i, "&amp;", 5) == 0) { (void)pc_buf_append_byte(&b, '&', NULL); i += 5; continue; }
            if (len - i >= 4 && memcmp(s + i, "&lt;", 4) == 0) { (void)pc_buf_append_byte(&b, '<', NULL); i += 4; continue; }
            if (len - i >= 4 && memcmp(s + i, "&gt;", 4) == 0) { (void)pc_buf_append_byte(&b, '>', NULL); i += 4; continue; }
            if (len - i >= 6 && memcmp(s + i, "&quot;", 6) == 0) { (void)pc_buf_append_byte(&b, '"', NULL); i += 6; continue; }
            if (len - i >= 6 && memcmp(s + i, "&apos;", 6) == 0) { (void)pc_buf_append_byte(&b, '\'', NULL); i += 6; continue; }
        }
        (void)pc_buf_append_byte(&b, (uint8_t)s[i], NULL);
        i++;
    }
    char *out = fc_strdup(pc_buf_cstr(&b));
    pc_buf_fini(&b);
    return out;
}

/* Trim ASCII whitespace of s[0..len); returns the trimmed [*a, *a+*n). */
static void fc_trim(const char *s, size_t len, const char **a, size_t *n)
{
    size_t i = 0, j = len;
    while (i < j && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' || s[j - 1] == '\n' || s[j - 1] == '\r')) j--;
    *a = s + i;
    *n = j - i;
}

/* First element <tag ...>...</tag>: start and end span it, inner/inner_len its contents.
 * false when not found. Index-bounded (never reads past len). */
static bool fc_find_element(const char *x, size_t xl, const char *tag, size_t *start, size_t *end,
                            const char **inner, size_t *inner_len)
{
    size_t tl = strlen(tag);
    size_t from = 0;
    while (from < xl) {
        /* find "<tag" */
        const char *p = NULL;
        for (size_t i = from; i + 1 + tl <= xl; i++) {
            if (x[i] == '<' && memcmp(x + i + 1, tag, tl) == 0) {
                p = x + i;
                break;
            }
        }
        if (p == NULL) {
            return false;
        }
        size_t i = (size_t)(p - x);
        size_t after = i + 1 + tl;
        char c = after < xl ? x[after] : '\0';
        if (c == '>' || c == ' ' || c == '/' || c == '\n' || c == '\r' || c == '\t') {
            /* find '>' */
            size_t gt = after;
            while (gt < xl && x[gt] != '>') gt++;
            if (gt >= xl) {
                return false;
            }
            if (gt > 0 && x[gt - 1] == '/') {
                *start = i;
                *end = gt + 1;
                *inner = x + gt + 1;
                *inner_len = 0;
                return true;
            }
            /* find "</tag>" from gt */
            char close[64];
            (void)snprintf(close, sizeof close, "</%s>", tag);
            size_t cl = strlen(close);
            size_t cpos = SIZE_MAX;
            for (size_t j = gt; j + cl <= xl; j++) {
                if (memcmp(x + j, close, cl) == 0) {
                    cpos = j;
                    break;
                }
            }
            if (cpos == SIZE_MAX) {
                return false;
            }
            *start = i;
            *end = cpos + cl;
            *inner = x + gt + 1;
            *inner_len = cpos - (gt + 1);
            return true;
        }
        from = after;
    }
    return false;
}

/* Attribute prop="value" on an element: start and end span the attribute, val its value. */
static bool fc_find_attr(const char *x, size_t xl, const char *prop, size_t *start, size_t *end,
                         const char **val, size_t *val_len)
{
    char key[128];
    (void)snprintf(key, sizeof key, " %s=\"", prop);
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl <= xl; i++) {
        if (memcmp(x + i, key, kl) == 0) {
            size_t v0 = i + kl;
            size_t v1 = v0;
            while (v1 < xl && x[v1] != '"') v1++;
            if (v1 >= xl) {
                return false;
            }
            *start = i;
            *end = v1 + 1;
            *val = x + v0;
            *val_len = v1 - v0;
            return true;
        }
    }
    return false;
}

/* Append the rdf:li values of a container (or the element text) to a JSON array `arr`. */
static void fc_li_values(yyjson_mut_doc *d, yyjson_mut_val *arr, const char *inner, size_t len)
{
    const char *rest = inner;
    size_t rl = len;
    bool any = false;
    while (rl > 0) {
        size_t s0, e0, il;
        const char *iv;
        if (!fc_find_element(rest, rl, "rdf:li", &s0, &e0, &iv, &il)) {
            break;
        }
        const char *t;
        size_t tn;
        fc_trim(iv, il, &t, &tn);
        char *u = fc_xml_unescape(t, tn);
        if (u != NULL) {
            yyjson_mut_arr_add_strcpy(d, arr, u);
            free(u);
            any = true;
        }
        rest += e0;
        rl -= e0;
    }
    if (!any) {
        /* No rdf:li and no child tags: the element text itself. */
        bool has_tag = false;
        for (size_t i = 0; i < len; i++) {
            if (inner[i] == '<') { has_tag = true; break; }
        }
        const char *t;
        size_t tn;
        fc_trim(inner, len, &t, &tn);
        if (!has_tag && tn > 0) {
            char *u = fc_xml_unescape(t, tn);
            if (u != NULL) {
                yyjson_mut_arr_add_strcpy(d, arr, u);
                free(u);
            }
        }
    }
}

/* read_file_info: parse the File Info fields from an XMP packet into a new result object. */
static yyjson_mut_val *fc_read_file_info(yyjson_mut_doc *d, const char *xmp, size_t xl)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    for (size_t fi = 0; fi < FC_NINFO; fi++) {
        const fc_info_field *f = &FC_INFO_FIELDS[fi];
        yyjson_mut_val *vals = yyjson_mut_arr(d);
        size_t s0, e0, il;
        const char *iv;
        if (fc_find_element(xmp, xl, f->prop, &s0, &e0, &iv, &il)) {
            fc_li_values(d, vals, iv, il);
        } else {
            size_t as, ae, avl;
            const char *av;
            if (fc_find_attr(xmp, xl, f->prop, &as, &ae, &av, &avl)) {
                char *u = fc_xml_unescape(av, avl);
                if (u != NULL) {
                    yyjson_mut_arr_add_strcpy(d, vals, u);
                    free(u);
                }
            }
        }
        if (strcmp(f->kind, "Bag") == 0) {
            yyjson_mut_obj_add_val(d, o, f->key, vals);
        } else if (strcmp(f->kind, "Seq") == 0) {
            /* join with "; " */
            pc_buf j = PC_BUF_INIT;
            size_t idx, mx;
            yyjson_mut_val *vv;
            bool first = true;
            yyjson_mut_arr_foreach(vals, idx, mx, vv) {
                if (!first) (void)pc_buf_append_str(&j, "; ", NULL);
                (void)pc_buf_append_str(&j, yyjson_mut_get_str(vv), NULL);
                first = false;
            }
            yyjson_mut_obj_add_strcpy(d, o, f->key, pc_buf_cstr(&j));
            pc_buf_fini(&j);
        } else {
            yyjson_mut_val *first = yyjson_mut_arr_get(vals, 0);
            yyjson_mut_obj_add_strcpy(d, o, f->key, first != NULL ? yyjson_mut_get_str(first) : "");
        }
    }
    /* copyrightStatus from xmpRights:Marked. */
    const char *marked = NULL;
    size_t ml = 0;
    size_t s0, e0, il;
    const char *iv;
    if (fc_find_element(xmp, xl, "xmpRights:Marked", &s0, &e0, &iv, &il)) {
        const char *t;
        size_t tn;
        fc_trim(iv, il, &t, &tn);
        marked = t;
        ml = tn;
    } else {
        size_t as, ae, avl;
        const char *av;
        if (fc_find_attr(xmp, xl, "xmpRights:Marked", &as, &ae, &av, &avl)) {
            marked = av;
            ml = avl;
        }
    }
    const char *status = "unknown";
    if (marked != NULL && ml == 4 && memcmp(marked, "True", 4) == 0) {
        status = "copyrighted";
    } else if (marked != NULL && ml == 5 && memcmp(marked, "False", 5) == 0) {
        status = "publicDomain";
    }
    yyjson_mut_obj_add_strcpy(d, o, "copyrightStatus", status);
    /* copyrightUrl from xmpRights:WebStatement. */
    char *url = NULL;
    if (fc_find_element(xmp, xl, "xmpRights:WebStatement", &s0, &e0, &iv, &il)) {
        const char *t;
        size_t tn;
        fc_trim(iv, il, &t, &tn);
        url = fc_xml_unescape(t, tn);
    } else {
        size_t as, ae, avl;
        const char *av;
        if (fc_find_attr(xmp, xl, "xmpRights:WebStatement", &as, &ae, &av, &avl)) {
            url = fc_xml_unescape(av, avl);
        }
    }
    yyjson_mut_obj_add_strcpy(d, o, "copyrightUrl", url != NULL ? url : "");
    free(url);
    return o;
}

/* camera_data: the read-only Camera Data from the EXIF block (cap each field at 200 chars). */
static yyjson_mut_val *fc_camera_data(yyjson_mut_doc *d, const uint8_t *exif, size_t exl)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    pc_algo_camera_info e;
    memset(&e, 0, sizeof e);
    if (exif != NULL && exl > 0) {
        (void)pc_algo_exif_read(exif, exl, &e, NULL);
    }
    char buf[256];
    if (e.make.s != NULL) {
        (void)snprintf(buf, sizeof buf, "%.200s", e.make.s);
        if (buf[0]) yyjson_mut_obj_add_strcpy(d, o, "make", buf);
    }
    if (e.model.s != NULL) {
        (void)snprintf(buf, sizeof buf, "%.200s", e.model.s);
        if (buf[0]) yyjson_mut_obj_add_strcpy(d, o, "model", buf);
    }
    if (e.date_taken.s != NULL) {
        (void)snprintf(buf, sizeof buf, "%.200s", e.date_taken.s);
        if (buf[0]) yyjson_mut_obj_add_strcpy(d, o, "dateTaken", buf);
    }
    if (e.lens.s != NULL) {
        (void)snprintf(buf, sizeof buf, "%.200s", e.lens.s);
        if (buf[0]) yyjson_mut_obj_add_strcpy(d, o, "lens", buf);
    }
    if (e.f_number.set && e.f_number.v > 0.0) {
        (void)snprintf(buf, sizeof buf, "f/%g", e.f_number.v);
        yyjson_mut_obj_add_strcpy(d, o, "fNumber", buf);
    }
    if (e.iso.set && e.iso.v > 0.0) {
        (void)snprintf(buf, sizeof buf, "ISO %.0f", e.iso.v);
        yyjson_mut_obj_add_strcpy(d, o, "iso", buf);
    }
    if (e.focal_length.set && e.focal_length.v > 0.0) {
        (void)snprintf(buf, sizeof buf, "%g mm", e.focal_length.v);
        yyjson_mut_obj_add_strcpy(d, o, "focalLength", buf);
    }
    if (e.focal_length_35mm.set && e.focal_length_35mm.v > 0.0) {
        (void)snprintf(buf, sizeof buf, "%g mm", e.focal_length_35mm.v);
        yyjson_mut_obj_add_strcpy(d, o, "focalLength35mm", buf);
    }
    if (e.exposure_time.set && e.exposure_time.v > 0.0) {
        double t = e.exposure_time.v;
        double r = 1.0 / t;
        if (t < 1.0 && fabs(r - round(r)) <= r * 0.01) {
            (void)snprintf(buf, sizeof buf, "1/%.0f s", round(r));
        } else {
            (void)snprintf(buf, sizeof buf, "%g s", t);
        }
        yyjson_mut_obj_add_strcpy(d, o, "exposure", buf);
    }
    pc_algo_camera_info_fini(&e);
    return o;
}

/* Remove every occurrence of element `prop` and attribute `prop` from the buffer `x` in place. */
static void fc_strip_prop(pc_buf *x, const char *prop)
{
    for (;;) {
        size_t s0, e0, il;
        const char *iv;
        if (!fc_find_element(pc_buf_cstr(x), x->len, prop, &s0, &e0, &iv, &il)) {
            break;
        }
        memmove(x->data + s0, x->data + e0, x->len - e0);
        x->len -= (e0 - s0);
        x->data[x->len] = '\0';
    }
    for (;;) {
        size_t s0, e0, avl;
        const char *av;
        if (!fc_find_attr(pc_buf_cstr(x), x->len, prop, &s0, &e0, &av, &avl)) {
            break;
        }
        memmove(x->data + s0, x->data + e0, x->len - e0);
        x->len -= (e0 - s0);
        x->data[x->len] = '\0';
    }
}

/* A string/array field of `info` as a JSON value, or NULL. */
static yyjson_val *fc_info_get(yyjson_val *info, const char *key)
{
    return info != NULL ? pc_json_get(info, key) : NULL;
}

/* write_file_info: rewrite only changed File Info fields of an XMP packet into *out (owned string).
 * Port of file_cmds::write_file_info. `cur` is the current read_file_info as a yyjson_val. */
static char *fc_write_file_info(const char *xmp, size_t xl, yyjson_val *info, yyjson_val *cur)
{
    static const char *const DEFAULT_PACKET =
        "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        " </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>";
    /* changed(key): info has it and it differs from cur. */
    pc_buf x = PC_BUF_INIT;
    bool has_rdf = false;
    for (size_t i = 0; xmp != NULL && i + 10 <= xl; i++) {
        if (memcmp(xmp + i, "</rdf:RDF>", 10) == 0) { has_rdf = true; break; }
    }
    if (xmp != NULL && has_rdf) {
        (void)pc_buf_append(&x, xmp, xl, NULL);
    } else {
        (void)pc_buf_append_str(&x, DEFAULT_PACKET, NULL);
    }

    const char *extra_keys[] = {"copyrightStatus", "copyrightUrl"};
    const char *extra_props[] = {"xmpRights:Marked", "xmpRights:WebStatement"};
    /* Remove each changed property. */
    for (size_t fi = 0; fi < FC_NINFO + 2; fi++) {
        const char *key = fi < FC_NINFO ? FC_INFO_FIELDS[fi].key : extra_keys[fi - FC_NINFO];
        const char *prop = fi < FC_NINFO ? FC_INFO_FIELDS[fi].prop : extra_props[fi - FC_NINFO];
        yyjson_val *nv = fc_info_get(info, key);
        if (nv == NULL) {
            continue;
        }
        yyjson_val *ov = fc_info_get(cur, key);
        /* changed when info has the key and it differs from cur. */
        pc_json_value a = PC_JSON_VALUE_INIT, b = PC_JSON_VALUE_INIT;
        (void)pc_json_value_from_val(nv, &a, NULL);
        (void)pc_json_value_from_val(ov, &b, NULL);
        bool changed = !pc_json_value_eq(&a, &b);
        pc_json_value_fini(&a);
        pc_json_value_fini(&b);
        if (changed) {
            fc_strip_prop(&x, prop);
        }
    }
    /* Build the body of changed fields. */
    pc_buf body = PC_BUF_INIT;
    for (size_t fi = 0; fi < FC_NINFO; fi++) {
        const fc_info_field *f = &FC_INFO_FIELDS[fi];
        yyjson_val *nv = fc_info_get(info, f->key);
        if (nv == NULL) {
            continue;
        }
        yyjson_val *ov = fc_info_get(cur, f->key);
        pc_json_value a = PC_JSON_VALUE_INIT, b = PC_JSON_VALUE_INIT;
        (void)pc_json_value_from_val(nv, &a, NULL);
        (void)pc_json_value_from_val(ov, &b, NULL);
        bool changed = !pc_json_value_eq(&a, &b);
        pc_json_value_fini(&a);
        pc_json_value_fini(&b);
        if (!changed) {
            continue;
        }
        bool bag_or_seq = strcmp(f->kind, "Bag") == 0 || strcmp(f->kind, "Seq") == 0;
        /* Collect the values. */
        char *items[64];
        size_t nitems = 0;
        if (yyjson_is_arr(nv)) {
            size_t idx, mx;
            yyjson_val *el;
            yyjson_arr_foreach(nv, idx, mx, el) {
                size_t sl = 0;
                const char *sv = pc_json_as_str(el, &sl);
                if (sv != NULL && sl > 0 && nitems < 64) {
                    items[nitems++] = pc_strndup(sv, sl);
                }
            }
        } else if (yyjson_is_str(nv)) {
            size_t sl = 0;
            const char *sv = pc_json_as_str(nv, &sl);
            if (sv != NULL && sl > 0) {
                if (bag_or_seq) {
                    /* split on ';' and ','. */
                    size_t start = 0;
                    for (size_t i = 0; i <= sl; i++) {
                        if (i == sl || sv[i] == ';' || sv[i] == ',') {
                            const char *t;
                            size_t tn;
                            fc_trim(sv + start, i - start, &t, &tn);
                            if (tn > 0 && nitems < 64) {
                                items[nitems++] = pc_strndup(t, tn);
                            }
                            start = i + 1;
                        }
                    }
                } else {
                    items[nitems++] = pc_strndup(sv, sl);
                }
            }
        }
        if (nitems == 0) {
            continue;
        }
        if (f->kind[0] == '\0') {
            (void)pc_buf_append_str(&body, "   <", NULL);
            (void)pc_buf_append_str(&body, f->prop, NULL);
            (void)pc_buf_append_byte(&body, '>', NULL);
            (void)fc_xml_escape(&body, items[0], strlen(items[0]), NULL);
            (void)pc_buf_appendf(&body, NULL, "</%s>\n", f->prop);
        } else {
            (void)pc_buf_appendf(&body, NULL, "   <%s><rdf:%s>", f->prop, f->kind);
            for (size_t i = 0; i < nitems; i++) {
                if (strcmp(f->kind, "Alt") == 0) {
                    (void)pc_buf_append_str(&body, "<rdf:li xml:lang=\"x-default\">", NULL);
                } else {
                    (void)pc_buf_append_str(&body, "<rdf:li>", NULL);
                }
                (void)fc_xml_escape(&body, items[i], strlen(items[i]), NULL);
                (void)pc_buf_append_str(&body, "</rdf:li>", NULL);
            }
            (void)pc_buf_appendf(&body, NULL, "</rdf:%s></%s>\n", f->kind, f->prop);
        }
        for (size_t i = 0; i < nitems; i++) {
            free(items[i]);
        }
    }
    /* copyrightStatus / copyrightUrl (only when changed). */
    {
        yyjson_val *nv = fc_info_get(info, "copyrightStatus");
        if (nv != NULL) {
            yyjson_val *ov = fc_info_get(cur, "copyrightStatus");
            pc_json_value a = PC_JSON_VALUE_INIT, b = PC_JSON_VALUE_INIT;
            (void)pc_json_value_from_val(nv, &a, NULL);
            (void)pc_json_value_from_val(ov, &b, NULL);
            bool changed = !pc_json_value_eq(&a, &b);
            pc_json_value_fini(&a);
            pc_json_value_fini(&b);
            size_t sl = 0;
            const char *sv = pc_json_as_str(nv, &sl);
            if (changed && sv != NULL) {
                if (sl == 11 && memcmp(sv, "copyrighted", 11) == 0) {
                    (void)pc_buf_append_str(&body, "   <xmpRights:Marked>True</xmpRights:Marked>\n", NULL);
                } else if (sl == 12 && memcmp(sv, "publicDomain", 12) == 0) {
                    (void)pc_buf_append_str(&body, "   <xmpRights:Marked>False</xmpRights:Marked>\n", NULL);
                }
            }
        }
        yyjson_val *uv = fc_info_get(info, "copyrightUrl");
        if (uv != NULL) {
            yyjson_val *ov = fc_info_get(cur, "copyrightUrl");
            pc_json_value a = PC_JSON_VALUE_INIT, b = PC_JSON_VALUE_INIT;
            (void)pc_json_value_from_val(uv, &a, NULL);
            (void)pc_json_value_from_val(ov, &b, NULL);
            bool changed = !pc_json_value_eq(&a, &b);
            pc_json_value_fini(&a);
            pc_json_value_fini(&b);
            size_t sl = 0;
            const char *sv = pc_json_as_str(uv, &sl);
            if (changed && sv != NULL && sl > 0) {
                (void)pc_buf_append_str(&body, "   <xmpRights:WebStatement>", NULL);
                (void)fc_xml_escape(&body, sv, sl, NULL);
                (void)pc_buf_append_str(&body, "</xmpRights:WebStatement>\n", NULL);
            }
        }
    }
    if (body.len > 0) {
        pc_buf desc = PC_BUF_INIT;
        (void)pc_buf_append_str(&desc,
                                "  <rdf:Description rdf:about=\"\" "
                                "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
                                "xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\" "
                                "xmlns:xmpRights=\"http://ns.adobe.com/xap/1.0/rights/\">\n",
                                NULL);
        (void)pc_buf_append(&desc, body.data, body.len, NULL);
        (void)pc_buf_append_str(&desc, "  </rdf:Description>\n ", NULL);
        /* Insert before the last </rdf:RDF>. */
        size_t at = x.len;
        for (size_t i = 0; i + 10 <= x.len; i++) {
            if (memcmp(x.data + i, "</rdf:RDF>", 10) == 0) {
                at = i;
            }
        }
        pc_buf merged = PC_BUF_INIT;
        (void)pc_buf_append(&merged, x.data, at, NULL);
        (void)pc_buf_append(&merged, desc.data, desc.len, NULL);
        (void)pc_buf_append(&merged, x.data + at, x.len - at, NULL);
        pc_buf_fini(&x);
        x = merged;
        pc_buf_fini(&desc);
    }
    pc_buf_fini(&body);
    char *result = fc_strdup(pc_buf_cstr(&x));
    pc_buf_fini(&x);
    return result;
}

/* file.fileInfo: with File Info keys present, edit the XMP as one history step; always return the
 * current fields plus read-only camera data. */
typedef struct {
    char *xmp; /* new XMP string (moved into the doc) */
} fc_fileinfo_ctx;

static pc_status fc_fileinfo_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                  pc_error *err)
{
    (void)ha;
    (void)active;
    fc_fileinfo_ctx *c = vctx;
    pc_str ns = {0};
    pc_status rc = pc_str_set_c(&ns, c->xmp, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str_fini(&doc->metadata.xmp);
    doc->metadata.xmp = ns;
    doc->metadata.has_xmp = true;
    return PC_OK;
}

static pc_status run_file_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *const KEYS[] = {"title",     "author",          "authorTitle", "description",
                                       "keywords",  "copyright",       "copyrightStatus",
                                       "copyrightUrl"};
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    bool edits = false;
    if (p != NULL && yyjson_is_obj(p)) {
        for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
            if (pc_json_get(p, KEYS[i]) != NULL) {
                edits = true;
                break;
            }
        }
    }
    if (edits) {
        const pc_document *doc = pc_doc_arc_get(d->doc);
        const char *xmp = doc->metadata.has_xmp ? pc_str_c(&doc->metadata.xmp) : "";
        size_t xl = doc->metadata.has_xmp ? doc->metadata.xmp.len : 0;
        /* current fields for the changed-comparison. */
        yyjson_mut_doc *cd = yyjson_mut_doc_new(NULL);
        if (cd == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *cur_mut = fc_read_file_info(cd, xmp, xl);
        yyjson_mut_doc_set_root(cd, cur_mut);
        /* Convert cur to an immutable doc for comparison; and p is already yyjson_val. */
        pc_buf curtext = PC_BUF_INIT;
        (void)pc_json_write(cur_mut, 0, &curtext, NULL);
        yyjson_doc *curdoc = NULL;
        (void)pc_json_parse(pc_buf_cstr(&curtext), curtext.len, &curdoc, NULL);
        pc_buf_fini(&curtext);
        yyjson_val *cur = curdoc != NULL ? yyjson_doc_get_root(curdoc) : NULL;
        char *nx = fc_write_file_info(xmp, xl, p, cur);
        yyjson_doc_free(curdoc);
        yyjson_mut_doc_free(cd);
        if (nx == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        fc_fileinfo_ctx ctx = {nx};
        pc_status rc = pc_engine_session_edit(s, "File Info", fc_fileinfo_edit, &ctx, err);
        free(nx);
        if (rc != PC_OK) {
            return rc;
        }
        d = pc_engine_session_active(s);
    }
    /* Build the result: the current fields + camera data. */
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const char *xmp = doc->metadata.has_xmp ? pc_str_c(&doc->metadata.xmp) : "";
    size_t xl = doc->metadata.has_xmp ? doc->metadata.xmp.len : 0;
    yyjson_mut_val *info = fc_read_file_info(jd, xmp, xl);
    yyjson_mut_obj_add_val(jd, info,
                           "camera", fc_camera_data(jd, pc_blob_data(doc->metadata.exif),
                                                    pc_blob_len(doc->metadata.exif)));
    yyjson_mut_doc_set_root(jd, info);
    return PC_OK;
}

/* ---- registry ----------------------------------------------------------------------------- */

static const char *const MENU_FILE[] = {"File"};
static const char *const MENU_FILE_AUTOMATE[] = {"File", "Automate"};
static const char *const MENU_FILE_EXPORT[] = {"File", "Export"};
static const char *const MENU_VIEW[] = {"View"};

const pc_engine_command *pc_engine_file_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"file.closeAll", "Close All", MENU_FILE, 1, "Cmd+Alt+W", "{}", fc_has_doc, run_close_all,
         true},
        {"file.closeOthers", "Close Others", MENU_FILE, 1, "Cmd+Alt+P",
         "{\"document\":index? (the one to keep; default active)}", fc_has_doc, run_close_others,
         true},
        {"file.revert", "Revert", MENU_FILE, 1, "F12",
         "{} (reloads the saved file as one undoable step)", fc_can_revert, run_revert, true},
        {"file.saveACopy", "Save a Copy\xe2\x80\xa6", MENU_FILE, 1, "Cmd+Alt+S",
         "{\"path\":str (format from the extension),\"quality\":0..12? (JPEG),\"layers\":bool=true,"
         "\"tiffLayers\":bool=false,\"dpxBits\":8|10|12|16?}",
         fc_has_doc, run_save_a_copy, true},
        {"file.openAs", "Open As\xe2\x80\xa6", MENU_FILE, 1, "Cmd+Alt+Shift+O",
         "{\"path\":str,\"as\":\"psd|png|jpg|tiff|…\"? (decode as this format)}", pc_engine_always,
         run_open_as, true},
        {"file.placeEmbedded", "Place Embedded\xe2\x80\xa6", MENU_FILE, 1, NULL,
         "{\"path\":str,\"scale\":%? (default: fit when larger than the canvas),\"fit\":bool?,"
         "\"smartObject\":bool?,\"center\":[x,y]?}",
         fc_has_doc, run_place_embedded, true},
        {"file.placeLinked", "Place Linked\xe2\x80\xa6", MENU_FILE, 1, NULL,
         "{\"path\":str,\"scale\":%?,\"fit\":bool?,\"center\":[x,y]?}", fc_has_doc, run_place_linked,
         true},
        {"file.fileInfo", "File Info\xe2\x80\xa6", MENU_FILE, 1, "Cmd+Alt+Shift+I",
         "{\"title\":str?,\"author\":str?,\"authorTitle\":str?,\"description\":str?,"
         "\"keywords\":[str]|\"a; b\"?,\"copyright\":str?,"
         "\"copyrightStatus\":\"unknown|copyrighted|publicDomain\"?,\"copyrightUrl\":str?} (no keys: "
         "read; the result also has read-only \"camera\":{make,model,dateTaken,exposure,fNumber,iso,"
         "focalLength,focalLength35mm,lens})",
         fc_has_doc, run_file_info, true},
        {"file.automate.fitImage", "Fit Image\xe2\x80\xa6", MENU_FILE_AUTOMATE, 2, NULL,
         "{\"width\":px,\"height\":px,\"dontEnlarge\":bool=false,\"resample\":str=\"bicubic\"}",
         fc_has_doc, run_fit_image, true},
        {"file.automate.conditionalModeChange", "Conditional Mode Change\xe2\x80\xa6",
         MENU_FILE_AUTOMATE, 2, NULL,
         "{\"from\":[\"rgb\",\"grayscale\",\"cmyk\",\"lab\",…]|\"any\"=\"any\","
         "\"to\":\"rgb|grayscale|cmyk|lab\"}",
         fc_has_doc, run_conditional_mode_change, true},
        {"file.export.layersToFiles", "Layers to Files\xe2\x80\xa6", MENU_FILE_EXPORT, 2, NULL,
         "{\"dir\":folder,\"format\":\"png|jpg|psd|tiff|…\"=\"png\",\"prefix\":str=document name,"
         "\"visibleOnly\":bool=true,\"quality\":0..12?,\"tiffLayers\":bool=false,\"dpxBits\":8|10|12|16?}"
         " -> {files}",
         fc_has_doc, run_layers_to_files, true},
        {"file.export.colorLookupTables", "Color Lookup Tables\xe2\x80\xa6", MENU_FILE_EXPORT, 2,
         NULL,
         "{\"path\":str? (.cube; omit to return the text),\"size\":2..129=33,\"title\":str?}",
         fc_has_adjustments, run_color_lookup_tables, true},
        {"view.newGuidesFromShape", "New Guides From Shape", MENU_VIEW, 1, NULL, "{\"layer\":id?}",
         fc_has_shape, run_guides_from_shape, true},
        {"view.clearCanvasGuides", "Clear Canvas Guides", MENU_VIEW, 1, NULL, "{}", fc_has_doc,
         run_clear_canvas_guides, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
