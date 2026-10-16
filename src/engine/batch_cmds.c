/* The file batch / automate pipeline (E-tail BATCH). Ports:
 *   - crates/engine/src/file_cmds.rs batch helpers: list_images / batch_inputs / process_files /
 *     OutputClaims and the commands file.automate.batch, file.scripts.imageProcessor,
 *     file.scripts.loadFilesIntoStack;
 *   - crates/engine/src/automate_cmds.rs: file.automate.contactSheetII, file.scripts.statistics,
 *     file.automate.createDroplet, file.automate.runDroplet;
 *   - crates/engine/src/lens_cmds.rs lens_batch: file.automate.lensCorrection (the batch twin ET5
 *     deferred).
 *
 * Every command opens each input in a throw-away Session (pc_engine_import), runs its work through
 * the engine's own registered commands, and saves through pc_engine_save_doc; a bad file in the
 * batch is collected into "errors" and skipped, never fatal (never-crash, REWRITE_PLAN rule 9).
 *
 * DEFERRED (owner: a later E-tail actions chunk): the called-actions / ActionState propagation.
 * The reference copies the caller's action list (and playback stack) into each scratch Session so a
 * "Play Action" step inside a batched action replays, records the transitively called actions into
 * a droplet ("calledActions"), and reloads them in runDroplet. That needs an exported deep-clone of
 * the Session action list, a called_actions walker and an action-list JSON (de)serialiser, none of
 * which actions_cmds.c exports yet (its pc_action_state is lazily owned and only reachable through
 * static helpers). The core pipeline here runs ordinary command steps; a "Play Action" step in a
 * scratch Session just finds no such action and that file reports an error (graceful, not a crash).
 * Also deferred: the droplet .command shim (OS executable packaging). */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "actions_cmds.h" /* pc_engine_actions_shell_save_command / shell_view / nested_failure */
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "smart_cmds.h"  /* pc_engine_layers_to_smart */
#include "text/fonts.h"  /* PC_TEXT_DEFAULT_FAMILY (contact-sheet caption font) */

#include "engine_internal.h"

/* ---- enabled gate (file_cmds::native: a batch needs no open document) ---------------------- */

static pc_status bf_native(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* ---- small helpers ------------------------------------------------------------------------- */

static const char *bf_file_name(const char *path)
{
    const char *n = path;
    for (const char *c = path; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\') {
            n = c + 1;
        }
    }
    return n;
}

/* ASCII lowercase one char. */
static char bf_lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Required non-empty string param. */
static const char *bf_str(yyjson_val *p, const char *key)
{
    yyjson_val *v = pc_json_get(p, key);
    size_t len = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &len) : NULL;
    return (s != NULL && len > 0) ? s : NULL;
}

/* p[k] as a finite double, else NULL kept. */
static bool bf_f64(yyjson_val *p, const char *key, double *out)
{
    double v;
    if (pc_json_as_f64(pc_json_get(p, key), &v) && isfinite(v)) {
        *out = v;
        return true;
    }
    return false;
}

static bool bf_bool(yyjson_val *p, const char *key, bool def)
{
    bool v;
    if (pc_json_as_bool(pc_json_get(p, key), &v)) {
        return v;
    }
    return def;
}

/* ---- list_images (file_cmds::list_images) -------------------------------------------------- */

/* Extensions the batch commands pick up from a folder (file_cmds::OPENABLE). */
static const char *const OPENABLE[] = {
    "pdn", "ora", "kra", "krz", "psd", "psb", "pcraft", "png", "jpg", "jpeg", "tif",
    "tiff", "webp", "gif", "bmp", "tga", "exr", "hdr", "qoi", "ico", "pnm", "ppm", "pgm",
    "fits", "fit", "fts", "dpx", "heic", "heif", "hif", "dng", "cr2", "nef", "nrw", "arw",
    "pef", "svg", "svgz", "af", "afdesign", "afphoto", "afpub"};

static bool bf_openable_ext(const char *name)
{
    const char *dot = NULL;
    for (const char *c = name; *c != '\0'; c++) {
        if (*c == '.') {
            dot = c;
        }
    }
    if (dot == NULL || dot[1] == '\0') {
        return false;
    }
    const char *ext = dot + 1;
    char low[16];
    size_t i = 0;
    for (; ext[i] != '\0' && i < sizeof low - 1; i++) {
        low[i] = bf_lc(ext[i]);
    }
    if (ext[i] != '\0') {
        return false; /* longer than any OPENABLE entry */
    }
    low[i] = '\0';
    for (size_t k = 0; k < sizeof OPENABLE / sizeof OPENABLE[0]; k++) {
        if (strcmp(low, OPENABLE[k]) == 0) {
            return true;
        }
    }
    return false;
}

static int bf_str_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void bf_free_list(char **list, size_t n)
{
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
}

/* Files of `dir` that look like images we can open, as owned path strings sorted by name. */
static pc_status bf_list_images(const char *dir, char ***out, size_t *out_n, pc_error *err)
{
    DIR *dd = opendir(dir);
    if (dd == NULL) {
        return pc_fail_errno(err, PC_ERR_IO, errno, "%s", dir);
    }
    char **list = NULL;
    size_t n = 0, cap = 0;
    pc_status rc = PC_OK;
    struct dirent *de;
    while ((de = readdir(dd)) != NULL) {
        if (!bf_openable_ext(de->d_name)) {
            continue;
        }
        char *full = pc_engine_file_join(dir, de->d_name);
        if (full == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", dir);
            break;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(full);
            continue;
        }
        char **g = pc_grow_array(list, &cap, n + 1, sizeof *list);
        if (g == NULL) {
            free(full);
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", dir);
            break;
        }
        list = g;
        list[n++] = full;
    }
    closedir(dd);
    if (rc != PC_OK) {
        bf_free_list(list, n);
        return rc;
    }
    if (n > 1) {
        qsort(list, n, sizeof *list, bf_str_cmp);
    }
    *out = list;
    *out_n = n;
    return PC_OK;
}

/* Collect the strings of a JSON array into an owned list (non-strings skipped, as filter_map). */
static pc_status bf_collect_strings(yyjson_val *arr, char ***out, size_t *out_n, pc_error *err)
{
    char **list = NULL;
    size_t n = 0, cap = 0;
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(arr, idx, max, v) {
        size_t len = 0;
        const char *s = pc_json_as_str(v, &len);
        if (s == NULL) {
            continue;
        }
        char **g = pc_grow_array(list, &cap, n + 1, sizeof *list);
        if (g == NULL) {
            bf_free_list(list, n);
            return pc_fail(err, PC_ERR_NOMEM, "inputs");
        }
        list = g;
        list[n] = pc_strndup(s, len);
        if (list[n] == NULL) {
            bf_free_list(list, n);
            return pc_fail(err, PC_ERR_NOMEM, "inputs");
        }
        n++;
    }
    *out = list;
    *out_n = n;
    return PC_OK;
}

/* Inputs of a batch command: "input"/"files" is a folder or an array of files (file_cmds::batch_inputs). */
static pc_status bf_batch_inputs(yyjson_val *p, const char *cmd, char ***out, size_t *out_n,
                                 pc_error *err)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, "input") : NULL;
    if (v == NULL && p != NULL) {
        v = pc_json_get(p, "files");
    }
    if (v != NULL && yyjson_is_arr(v)) {
        return bf_collect_strings(v, out, out_n, err);
    }
    size_t len = 0;
    const char *dir = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (dir != NULL) {
        return bf_list_images(dir, out, out_n, err);
    }
    return pc_engine_err_bad_params(err, cmd, "missing \"input\" (folder or array of files)");
}

/* ---- OutputClaims (file_cmds::OutputClaims) ------------------------------------------------ */

typedef struct bf_claims {
    char **lower; /* lower-cased output paths */
    char **input; /* the input that wrote each */
    size_t n, cap;
} bf_claims;

static char *bf_to_lower(const char *s)
{
    size_t len = strlen(s);
    char *r = pc_malloc_array(len + 1, 1);
    if (r == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        r[i] = bf_lc(s[i]);
    }
    r[len] = '\0';
    return r;
}

/* The earlier input that already wrote `out` in this run (ignoring case), or NULL. */
static const char *bf_claims_check(const bf_claims *c, const char *out)
{
    char *low = bf_to_lower(out);
    if (low == NULL) {
        return NULL;
    }
    const char *first = NULL;
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->lower[i], low) == 0) {
            first = c->input[i];
            break;
        }
    }
    free(low);
    return first;
}

static pc_status bf_claims_record(bf_claims *c, const char *out, const char *input, pc_error *err)
{
    char **gl = pc_grow_array(c->lower, &c->cap, c->n + 1, sizeof *c->lower);
    if (gl == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "batch");
    }
    c->lower = gl;
    /* input array grows in lock-step; cap is shared. */
    char **gi = pc_malloc_array(c->cap, sizeof *c->input);
    if (gi == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "batch");
    }
    for (size_t i = 0; i < c->n; i++) {
        gi[i] = c->input[i];
    }
    free(c->input);
    c->input = gi;
    char *low = bf_to_lower(out);
    char *in = pc_strndup(input, strlen(input));
    if (low == NULL || in == NULL) {
        free(low);
        free(in);
        return pc_fail(err, PC_ERR_NOMEM, "batch");
    }
    c->lower[c->n] = low;
    c->input[c->n] = in;
    c->n++;
    return PC_OK;
}

static void bf_claims_fini(bf_claims *c)
{
    for (size_t i = 0; i < c->n; i++) {
        free(c->lower[i]);
        free(c->input[i]);
    }
    free(c->lower);
    free(c->input);
    c->lower = NULL;
    c->input = NULL;
    c->n = c->cap = 0;
}

/* ---- process_files (file_cmds::process_files) ---------------------------------------------- */

typedef pc_status (*bf_each_fn)(pc_engine_session *scratch, void *ctx, pc_error *err);

static void bf_push_error(yyjson_mut_doc *rd, yyjson_mut_val *errors, const char *file,
                          const char *msg)
{
    yyjson_mut_val *e = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, e, "file", file);
    yyjson_mut_obj_add_strcpy(rd, e, "error", msg != NULL ? msg : "error");
    yyjson_mut_arr_append(errors, e);
}

/* The output extension for `path` under `format` ("same" keeps the input's). */
static void bf_out_ext(const char *path, const char *format, char *out, size_t cap)
{
    const char *src;
    if (strcmp(format, "same") == 0) {
        const char *dot = NULL;
        for (const char *c = path; *c != '\0'; c++) {
            if (*c == '.') {
                dot = c;
            }
        }
        src = (dot != NULL) ? dot + 1 : path; /* rsplit('.').next(): whole path when no dot */
    } else {
        src = format;
        while (*src == '.') {
            src++;
        }
    }
    size_t i = 0;
    for (; src[i] != '\0' && i < cap - 1; i++) {
        out[i] = bf_lc(src[i]);
    }
    out[i] = '\0';
    if (out[0] == '\0') {
        (void)snprintf(out, cap, "png");
    }
}

/* Opens each input in a scratch Session, runs `each` and saves it to `output` as `format`. Fills
 * *out with {files, errors}. Returns an error only on a failure building the result. */
static pc_status bf_process_files(char **inputs, size_t ninputs, const char *output,
                                  const char *format, const pc_engine_save_opts *save,
                                  const char *suffix, bf_each_fn each, void *ctx,
                                  pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *files = yyjson_mut_arr(rd);
    yyjson_mut_val *errors = yyjson_mut_arr(rd);
    bf_claims claims = {0};
    for (size_t i = 0; i < ninputs; i++) {
        const char *path = inputs[i];
        char ext[32];
        bf_out_ext(path, format, ext, sizeof ext);
        char stem[256];
        pc_engine_file_stem(path, stem, sizeof stem);
        char name[512];
        (void)snprintf(name, sizeof name, "%s%s.%s", stem, suffix, ext);
        char *outp = pc_engine_file_join(output, name);
        if (outp == NULL) {
            pc_json_value_fini(out);
            bf_claims_fini(&claims);
            return pc_fail(err, PC_ERR_NOMEM, "batch");
        }
        const char *first = bf_claims_check(&claims, outp);
        if (first != NULL) {
            char msg[512];
            (void)snprintf(msg, sizeof msg,
                           "not written: %s already holds the result of %s from this run (same "
                           "output name)",
                           outp, first);
            bf_push_error(rd, errors, path, msg);
            free(outp);
            continue;
        }
        pc_error ee = {0};
        pc_buf bytes = PC_BUF_INIT;
        if (pc_engine_read_file(path, &bytes, &ee) != PC_OK) {
            bf_push_error(rd, errors, path, ee.msg);
            pc_buf_fini(&bytes);
            free(outp);
            continue;
        }
        pc_engine_session *scratch = NULL;
        if (pc_engine_session_new(&scratch, &ee) != PC_OK) {
            bf_push_error(rd, errors, path, ee.msg);
            pc_buf_fini(&bytes);
            free(outp);
            continue;
        }
        pc_document doc;
        pc_status rc = pc_engine_import(bf_file_name(path), bytes.data, bytes.len, &doc, &ee);
        pc_buf_fini(&bytes);
        if (rc != PC_OK) {
            bf_push_error(rd, errors, path, ee.msg);
            pc_engine_session_free(scratch);
            free(outp);
            continue;
        }
        size_t didx = 0;
        if (pc_engine_session_add_document(scratch, &doc, path, &didx, &ee) != PC_OK) {
            bf_push_error(rd, errors, path, ee.msg);
            pc_document_fini(&doc);
            pc_engine_session_free(scratch);
            free(outp);
            continue;
        }
        if (each(scratch, ctx, &ee) != PC_OK) {
            bf_push_error(rd, errors, path, ee.msg);
            pc_engine_session_free(scratch);
            free(outp);
            continue;
        }
        const pc_engine_doc_state *d = pc_engine_session_active(scratch);
        if (d == NULL) {
            bf_push_error(rd, errors, path, "no document");
            pc_engine_session_free(scratch);
            free(outp);
            continue;
        }
        if (pc_engine_save_doc(pc_doc_arc_get(d->doc), outp, save, NULL, &ee) != PC_OK) {
            bf_push_error(rd, errors, path, ee.msg);
            pc_engine_session_free(scratch);
            free(outp);
            continue;
        }
        pc_engine_session_free(scratch);
        if (bf_claims_record(&claims, outp, path, err) != PC_OK) {
            free(outp);
            pc_json_value_fini(out);
            bf_claims_fini(&claims);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        yyjson_mut_arr_add_strcpy(rd, files, outp);
        free(outp);
    }
    bf_claims_fini(&claims);
    yyjson_mut_obj_add_val(rd, root, "files", files);
    yyjson_mut_obj_add_val(rd, root, "errors", errors);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* Execute a sub-command, passing params built in `pd` (its root is the params object; freed here).
 * On PC_OK and result_out != NULL the owned result value is moved into *result_out. */
static pc_status bf_exec(pc_engine_session *s, const char *id, yyjson_mut_doc *pd,
                         pc_json_value *result_out, pc_error *err)
{
    yyjson_doc *imut = NULL;
    bool has_root = pd != NULL && yyjson_mut_doc_get_root(pd) != NULL;
    if (has_root) {
        imut = yyjson_mut_doc_imut_copy(pd, NULL);
    }
    yyjson_mut_doc_free(pd);
    if (has_root && imut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", id);
    }
    pc_json_value v = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute(s, id, imut != NULL ? yyjson_doc_get_root(imut) : NULL,
                                             &v, err);
    yyjson_doc_free(imut);
    if (rc == PC_OK && result_out != NULL) {
        *result_out = v;
    } else {
        pc_json_value_fini(&v);
    }
    return rc;
}

/* ---- file.automate.batch ------------------------------------------------------------------- */

/* Collect an action's steps ([id,params] / {command|id,params} / "id") into `arr` as [[id,p]...],
 * dropping the steps a destination-folder batch overrides (Save/Save As, view, Quick Export). */
static pc_status bf_collect_steps(yyjson_mut_doc *md, yyjson_mut_val *arr, yyjson_val *steps_val,
                                  const char *cmd, pc_error *err)
{
    if (steps_val == NULL || !yyjson_is_arr(steps_val)) {
        return pc_engine_err_bad_params(err, cmd, "\"steps\" must be an array");
    }
    size_t idx, max;
    yyjson_val *st;
    yyjson_arr_foreach(steps_val, idx, max, st) {
        const char *id = NULL;
        size_t idlen = 0;
        yyjson_val *params = NULL;
        if (yyjson_is_arr(st) && yyjson_arr_size(st) >= 1) {
            id = pc_json_as_str(yyjson_arr_get(st, 0), &idlen);
            if (id == NULL) {
                return pc_engine_err_bad_params(err, cmd, "step id must be a string");
            }
            params = yyjson_arr_size(st) >= 2 ? yyjson_arr_get(st, 1) : NULL;
        } else if (yyjson_is_obj(st)) {
            yyjson_val *idv = pc_json_get(st, "command");
            if (idv == NULL) {
                idv = pc_json_get(st, "id");
            }
            id = idv != NULL ? pc_json_as_str(idv, &idlen) : NULL;
            if (id == NULL) {
                return pc_engine_err_bad_params(err, cmd, "step needs \"command\"");
            }
            params = pc_json_get(st, "params");
        } else if (yyjson_is_str(st)) {
            id = pc_json_as_str(st, &idlen);
        } else {
            return pc_engine_err_bad_params(err, cmd,
                                            "each step is [id, params] or {\"command\", \"params\"}");
        }
        /* The destination folder replaces recorded saves; view steps don't change the output. */
        char idbuf[128];
        size_t cp = idlen < sizeof idbuf - 1 ? idlen : sizeof idbuf - 1;
        memcpy(idbuf, id, cp);
        idbuf[cp] = '\0';
        if (pc_engine_actions_shell_save_command(idbuf) ||
            pc_engine_actions_shell_view_command(idbuf) ||
            strcmp(idbuf, "file.export.quickExport") == 0) {
            continue;
        }
        yyjson_mut_val *pair = yyjson_mut_arr(md);
        yyjson_mut_arr_append(pair, yyjson_mut_strncpy(md, id, idlen));
        yyjson_mut_arr_append(pair,
                              params != NULL ? yyjson_val_mut_copy(md, params) : yyjson_mut_obj(md));
        yyjson_mut_arr_append(arr, pair);
    }
    return PC_OK;
}

/* Context for the batch / lens / image-processor per-file callbacks. */
typedef struct bf_steps_ctx {
    yyjson_mut_val *steps; /* [[id, params]...] in `md` */
    yyjson_mut_doc *md;
} bf_steps_ctx;

static pc_status bf_run_steps_each(pc_engine_session *s, void *vctx, pc_error *err)
{
    bf_steps_ctx *ctx = vctx;
    size_t idx, max;
    yyjson_mut_val *pair;
    yyjson_mut_arr_foreach(ctx->steps, idx, max, pair) {
        const char *id = yyjson_mut_get_str(yyjson_mut_arr_get(pair, 0));
        yyjson_mut_val *pm = yyjson_mut_arr_get(pair, 1);
        yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
        yyjson_doc *imut = NULL;
        if (tmp != NULL && pm != NULL) {
            yyjson_mut_doc_set_root(tmp, yyjson_mut_val_mut_copy(tmp, pm));
            imut = yyjson_mut_doc_imut_copy(tmp, NULL);
        }
        yyjson_mut_doc_free(tmp);
        yyjson_val *prm = imut != NULL ? yyjson_doc_get_root(imut) : NULL;
        pc_json_value value = PC_JSON_VALUE_INIT;
        pc_status rc = pc_engine_session_execute(s, id != NULL ? id : "", prm, &value, err);
        yyjson_doc_free(imut);
        if (rc != PC_OK) {
            pc_json_value_fini(&value);
            return rc;
        }
        char *nested = pc_engine_actions_nested_failure(id, pc_json_value_root(&value));
        pc_json_value_fini(&value);
        if (nested != NULL) {
            pc_status fr = pc_engine_err_other(err, "%s", nested);
            free(nested);
            return fr;
        }
    }
    return PC_OK;
}

static pc_status run_batch(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "file.automate.batch";
    yyjson_val *steps_val = p != NULL ? pc_json_get(p, "steps") : NULL;
    if (steps_val == NULL && p != NULL) {
        steps_val = pc_json_get(p, "action");
    }
    if (steps_val == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"steps\"");
    }
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    yyjson_mut_val *steps = yyjson_mut_arr(md);
    pc_status rc = bf_collect_steps(md, steps, steps_val, CMD, err);
    if (rc != PC_OK) {
        yyjson_mut_doc_free(md);
        return rc;
    }
    /* Every step id must be a known command. */
    size_t idx, max;
    yyjson_mut_val *pair;
    yyjson_mut_arr_foreach(steps, idx, max, pair) {
        const char *id = yyjson_mut_get_str(yyjson_mut_arr_get(pair, 0));
        if (id == NULL || pc_engine_command_find(id) == NULL) {
            /* `id` lives in md's string pool; copy it out before freeing md. */
            char idcopy[128];
            size_t ilen = id != NULL ? strlen(id) : 0;
            size_t ic = ilen < sizeof idcopy - 1 ? ilen : sizeof idcopy - 1;
            if (id != NULL) {
                memcpy(idcopy, id, ic);
            }
            idcopy[ic] = '\0';
            yyjson_mut_doc_free(md);
            return pc_engine_err_bad_params(err, CMD, "unknown command `%s` in the action", idcopy);
        }
    }
    const char *output = bf_str(p, "output");
    if (output == NULL) {
        yyjson_mut_doc_free(md);
        return pc_engine_err_bad_params(err, CMD, "missing \"output\"");
    }
    char *output_dup = pc_strndup(output, strlen(output));
    const char *format = bf_str(p, "format");
    char *format_dup = pc_strndup(format != NULL ? format : "same",
                                  format != NULL ? strlen(format) : 4);
    pc_engine_save_opts save = pc_engine_save_opts_default();
    rc = pc_engine_save_opts_from_params(s, p, CMD, &save, err);
    char **inputs = NULL;
    size_t ninputs = 0;
    if (rc == PC_OK) {
        rc = bf_batch_inputs(p, CMD, &inputs, &ninputs, err);
    }
    if (rc == PC_OK && (output_dup == NULL || format_dup == NULL)) {
        rc = pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    if (rc == PC_OK) {
        bf_steps_ctx ctx = {steps, md};
        rc = bf_process_files(inputs, ninputs, output_dup, format_dup, &save, "",
                              bf_run_steps_each, &ctx, out, err);
    }
    bf_free_list(inputs, ninputs);
    free(output_dup);
    free(format_dup);
    yyjson_mut_doc_free(md);
    return rc;
}

/* ---- file.scripts.imageProcessor ----------------------------------------------------------- */

typedef struct bf_ip_ctx {
    bool to_srgb;
    bool has_fit;
    double fit_w, fit_h;
} bf_ip_ctx;

static pc_status bf_image_processor_each(pc_engine_session *s, void *vctx, pc_error *err)
{
    bf_ip_ctx *ctx = vctx;
    if (ctx->to_srgb) {
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        if (d != NULL && pc_doc_arc_get(d->doc)->mode != PC_COLOR_MODE_RGB) {
            pc_status rc = bf_exec(s, "image.mode.rgb", yyjson_mut_doc_new(NULL), NULL, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
    }
    if (ctx->has_fit) {
        yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
        if (pd == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "file.automate.fitImage");
        }
        yyjson_mut_val *o = yyjson_mut_obj(pd);
        yyjson_mut_obj_add_real(pd, o, "width", ctx->fit_w);
        yyjson_mut_obj_add_real(pd, o, "height", ctx->fit_h);
        yyjson_mut_obj_add_bool(pd, o, "dontEnlarge", true);
        yyjson_mut_doc_set_root(pd, o);
        pc_status rc = bf_exec(s, "file.automate.fitImage", pd, NULL, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

static pc_status run_image_processor(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                     pc_error *err)
{
    const char *CMD = "file.scripts.imageProcessor";
    const char *output = bf_str(p, "output");
    if (output == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"output\"");
    }
    const char *format = bf_str(p, "format");
    char *output_dup = pc_strndup(output, strlen(output));
    char *format_dup = pc_strndup(format != NULL ? format : "jpg",
                                  format != NULL ? strlen(format) : 3);
    pc_engine_save_opts save = pc_engine_save_opts_default();
    pc_status rc = pc_engine_save_opts_from_params(s, p, CMD, &save, err);
    if (!save.has_quality) {
        save.has_quality = true;
        save.quality = 8.0; /* SaveOpts::or_quality(8.0) */
    }
    char **inputs = NULL;
    size_t ninputs = 0;
    if (rc == PC_OK) {
        rc = bf_batch_inputs(p, CMD, &inputs, &ninputs, err);
    }
    if (rc == PC_OK && (output_dup == NULL || format_dup == NULL)) {
        rc = pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    if (rc == PC_OK) {
        double w, h;
        bool has_w = bf_f64(p, "width", &w) && w > 0.0;
        bool has_h = bf_f64(p, "height", &h) && h > 0.0;
        bf_ip_ctx ctx = {bf_bool(p, "convertToSrgb", false), has_w || has_h,
                         has_w ? w : 1e9, has_h ? h : 1e9};
        rc = bf_process_files(inputs, ninputs, output_dup, format_dup, &save, "",
                              bf_image_processor_each, &ctx, out, err);
    }
    bf_free_list(inputs, ninputs);
    free(output_dup);
    free(format_dup);
    return rc;
}

/* ---- file.scripts.loadFilesIntoStack ------------------------------------------------------- */

/* Context for the align+smart "Convert to Smart Object" edit step. */
typedef struct bf_to_smart_ctx {
    char name[256];
} bf_to_smart_ctx;

static pc_status bf_to_smart_edit(pc_document *doc, bool *has_active, pc_layer_id *active,
                                  void *vctx, pc_error *err)
{
    bf_to_smart_ctx *ctx = vctx;
    pc_layer *layers = doc->layers;
    size_t n = doc->nlayers;
    doc->layers = NULL;
    doc->nlayers = 0;
    pc_layer smart;
    pc_status rc = pc_engine_layers_to_smart(doc, ctx->name, layers, n, &smart, err);
    if (rc != PC_OK) {
        return rc; /* layers_to_smart freed `layers` */
    }
    pc_layer *one = pc_malloc_array(1, sizeof(pc_layer));
    if (one == NULL) {
        pc_layer_fini(&smart);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    one[0] = smart;
    doc->layers = one;
    doc->nlayers = 1;
    *has_active = true;
    *active = smart.id;
    return PC_OK;
}

static pc_status run_load_files_into_stack(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                           pc_error *err)
{
    const char *CMD = "file.scripts.loadFilesIntoStack";
    yyjson_val *pv = p != NULL ? pc_json_get(p, "paths") : NULL;
    if (pv == NULL && p != NULL) {
        pv = pc_json_get(p, "input");
    }
    char **paths = NULL;
    size_t npaths = 0;
    pc_status rc;
    if (pv != NULL && yyjson_is_arr(pv)) {
        rc = bf_collect_strings(pv, &paths, &npaths, err);
    } else {
        size_t len = 0;
        const char *dir = pv != NULL ? pc_json_as_str(pv, &len) : NULL;
        if (dir == NULL) {
            return pc_engine_err_bad_params(err, CMD, "missing \"paths\"");
        }
        rc = bf_list_images(dir, &paths, &npaths, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    if (npaths == 0) {
        bf_free_list(paths, npaths);
        return pc_engine_err_bad_params(err, CMD, "no files to load");
    }
    /* Load each file as a flattened document. */
    pc_document *docs = pc_malloc_array(npaths, sizeof(pc_document));
    char **names = pc_malloc_array(npaths, sizeof(char *));
    if (docs == NULL || names == NULL) {
        free(docs);
        free(names);
        bf_free_list(paths, npaths);
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    size_t loaded = 0;
    for (; loaded < npaths; loaded++) {
        pc_buf bytes = PC_BUF_INIT;
        rc = pc_engine_read_file(paths[loaded], &bytes, err);
        if (rc == PC_OK) {
            rc = pc_engine_import(bf_file_name(paths[loaded]), bytes.data, bytes.len, &docs[loaded],
                                  err);
        }
        pc_buf_fini(&bytes);
        if (rc != PC_OK) {
            break;
        }
        names[loaded] = pc_strndup(bf_file_name(paths[loaded]), strlen(bf_file_name(paths[loaded])));
        if (names[loaded] == NULL) {
            pc_document_fini(&docs[loaded]);
            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
            break;
        }
    }
    if (rc != PC_OK) {
        for (size_t i = 0; i < loaded; i++) {
            pc_document_fini(&docs[i]);
            free(names[i]);
        }
        free(docs);
        free(names);
        bf_free_list(paths, npaths);
        return rc;
    }
    /* The stack document: first file's mode/depth/profile, widest/tallest size. */
    uint32_t w = docs[0].size.width, h = docs[0].size.height;
    for (size_t i = 1; i < npaths; i++) {
        if (docs[i].size.width > w) {
            w = docs[i].size.width;
        }
        if (docs[i].size.height > h) {
            h = docs[i].size.height;
        }
    }
    if (w == 0) {
        w = 1;
    }
    if (h == 0) {
        h = 1;
    }
    char stem[256];
    pc_engine_file_stem(paths[0], stem, sizeof stem);
    pc_document stack;
    rc = pc_document_init(&stack, stem, pc_size_make(w, h), docs[0].mode, docs[0].depth, err);
    if (rc != PC_OK) {
        goto cleanup_docs;
    }
    stack.resolution_dpi = docs[0].resolution_dpi;
    if (docs[0].icc_profile != NULL) {
        stack.icc_profile = pc_blob_retain(docs[0].icc_profile);
    }
    pc_pixel_format fmt = pc_document_pixel_format(&stack);
    /* First file at the bottom, like Photoshop's script. */
    for (size_t i = 0; i < npaths; i++) {
        pc_surface flat;
        rc = pc_engine_flattened(&docs[i], fmt, &flat, err);
        if (rc != PC_OK) {
            pc_surface_fini(&flat);
            pc_document_fini(&stack);
            goto cleanup_docs;
        }
        pc_layer l;
        rc = pc_layer_raster(&l, names[i], fmt, err);
        if (rc != PC_OK) {
            pc_surface_fini(&flat);
            pc_document_fini(&stack);
            goto cleanup_docs;
        }
        pc_surface *ls = pc_layer_surface_mut(&l);
        if (ls != NULL) {
            pc_surface_fini(ls);
            *ls = flat;
        } else {
            pc_surface_fini(&flat);
        }
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(&stack, NULL, &l, &nid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&l);
            pc_document_fini(&stack);
            goto cleanup_docs;
        }
    }
    size_t n_loaded = stack.nlayers; /* reported layer count (before any smart conversion) */
    bool smart = bf_bool(p, "createSmartObject", false);
    bool align = bf_bool(p, "align", false) && n_loaded > 1;
    if (smart && !align) {
        pc_layer *layers = stack.layers;
        size_t n = stack.nlayers;
        stack.layers = NULL;
        stack.nlayers = 0;
        pc_layer sm;
        rc = pc_engine_layers_to_smart(&stack, stem, layers, n, &sm, err);
        if (rc != PC_OK) {
            pc_document_fini(&stack);
            goto cleanup_docs;
        }
        pc_layer *one = pc_malloc_array(1, sizeof(pc_layer));
        if (one == NULL) {
            pc_layer_fini(&sm);
            pc_document_fini(&stack);
            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
            goto cleanup_docs;
        }
        one[0] = sm;
        stack.layers = one;
        stack.nlayers = 1;
    }
    size_t didx = 0;
    rc = pc_engine_session_add_document(s, &stack, NULL, &didx, err);
    if (rc != PC_OK) {
        pc_document_fini(&stack);
        goto cleanup_docs;
    }
    if (align) {
        rc = bf_exec(s, "select.allLayers", yyjson_mut_doc_new(NULL), NULL, err);
        if (rc == PC_OK) {
            yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
            if (pd != NULL) {
                yyjson_mut_val *o = yyjson_mut_obj(pd);
                yyjson_mut_obj_add_str(pd, o, "projection", "auto");
                yyjson_mut_doc_set_root(pd, o);
                rc = bf_exec(s, "edit.autoAlignLayers", pd, NULL, err);
            } else {
                rc = pc_fail(err, PC_ERR_NOMEM, CMD);
            }
        }
        if (rc == PC_OK && smart) {
            bf_to_smart_ctx ctx;
            const pc_engine_doc_state *d = pc_engine_session_active(s);
            const char *dn = d != NULL ? pc_doc_arc_get(d->doc)->name.ptr : NULL;
            size_t dl = d != NULL ? pc_doc_arc_get(d->doc)->name.len : 0;
            size_t cp = dl < sizeof ctx.name - 1 ? dl : sizeof ctx.name - 1;
            if (dn != NULL) {
                memcpy(ctx.name, dn, cp);
            }
            ctx.name[dn != NULL ? cp : 0] = '\0';
            rc = pc_engine_session_edit(s, "Convert to Smart Object", bf_to_smart_edit, &ctx, err);
            if (rc == PC_OK) {
                pc_engine_doc_state *dm = pc_engine_session_active_mut(s);
                if (dm != NULL && dm->has_active_layer) {
                    free(dm->selected_layers);
                    dm->selected_layers = pc_malloc_array(1, sizeof(pc_layer_id));
                    if (dm->selected_layers != NULL) {
                        dm->selected_layers[0] = dm->active_layer;
                        dm->nselected = 1;
                    } else {
                        dm->nselected = 0;
                    }
                }
            }
        }
        if (rc != PC_OK) {
            goto cleanup_docs;
        }
    }
    /* result {document, layers} */
    {
        yyjson_mut_doc *rd = NULL;
        if (pc_engine_result_doc(out, &rd, err) == PC_OK) {
            yyjson_mut_val *root = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_uint(rd, root, "document", didx);
            yyjson_mut_obj_add_uint(rd, root, "layers", n_loaded);
            yyjson_mut_doc_set_root(rd, root);
        } else {
            rc = PC_ERR_NOMEM;
        }
    }
cleanup_docs:
    for (size_t i = 0; i < npaths; i++) {
        pc_document_fini(&docs[i]);
        free(names[i]);
    }
    free(docs);
    free(names);
    bf_free_list(paths, npaths);
    return rc;
}

/* ---- file.scripts.statistics --------------------------------------------------------------- */

static pc_status run_statistics(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                pc_error *err)
{
    const char *CMD = "file.scripts.statistics";
    const char *mode = bf_str(p, "mode");
    if (mode == NULL) {
        mode = bf_str(p, "stackMode");
    }
    if (mode == NULL) {
        mode = "median";
    }
    pc_stack_mode sm;
    if (!pc_stack_mode_from_id(mode, strlen(mode), &sm)) {
        return pc_engine_err_bad_params(err, CMD, "unknown stack mode `%s`", mode);
    }
    yyjson_val *input = p != NULL ? pc_json_get(p, "input") : NULL;
    if (input == NULL && p != NULL) {
        input = pc_json_get(p, "paths");
    }
    if (input == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"input\" (files or a folder)");
    }
    bool align = bf_bool(p, "align", false);
    /* Load the files into one stack-mode smart object. */
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    if (pd == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    yyjson_mut_val *o = yyjson_mut_obj(pd);
    yyjson_mut_obj_add(o, yyjson_mut_strcpy(pd, "paths"), yyjson_val_mut_copy(pd, input));
    yyjson_mut_obj_add_bool(pd, o, "align", align);
    yyjson_mut_obj_add_bool(pd, o, "createSmartObject", true);
    yyjson_mut_doc_set_root(pd, o);
    pc_json_value loaded = PC_JSON_VALUE_INIT;
    pc_status rc = bf_exec(s, "file.scripts.loadFilesIntoStack", pd, &loaded, err);
    if (rc != PC_OK) {
        return rc;
    }
    uint64_t n = 0;
    yyjson_mut_val *lr = pc_json_value_root(&loaded);
    if (lr != NULL) {
        n = yyjson_mut_get_uint(yyjson_mut_obj_get(lr, "layers"));
    }
    pc_json_value_fini(&loaded);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->nlayers == 0) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id first = doc->layers[0].id;
    rc = pc_engine_session_select_layer(s, first, err);
    if (rc != PC_OK) {
        return rc;
    }
    char id[80];
    (void)snprintf(id, sizeof id, "layer.smartObjects.stackMode.%s", pc_stack_mode_id(sm));
    yyjson_mut_doc *sp = yyjson_mut_doc_new(NULL);
    if (sp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    yyjson_mut_val *so = yyjson_mut_obj(sp);
    yyjson_mut_obj_add_uint(sp, so, "layer", first);
    yyjson_mut_doc_set_root(sp, so);
    rc = bf_exec(s, id, sp, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t aidx = 0;
    (void)pc_engine_session_active_index(s, &aidx);
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "document", aidx);
    yyjson_mut_obj_add_uint(rd, root, "layer", first);
    yyjson_mut_obj_add_uint(rd, root, "images", n);
    yyjson_mut_obj_add_str(rd, root, "mode", pc_stack_mode_id(sm));
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- file.automate.contactSheetII ---------------------------------------------------------- */

static double bf_units_px(double v, const char *unit, double res)
{
    if (strcmp(unit, "cm") == 0) {
        return v * res / 2.54;
    }
    if (strcmp(unit, "mm") == 0) {
        return v * res / 25.4;
    }
    if (strcmp(unit, "px") == 0 || strcmp(unit, "pixels") == 0) {
        return v;
    }
    return v * res; /* inches (default) */
}

/* p[k] as a finite positive double, else `d`. */
static double bf_pos(yyjson_val *p, const char *k, double d)
{
    double v;
    if (bf_f64(p, k, &v) && v > 0.0) {
        return v;
    }
    return d;
}

static double bf_clamp(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Context for pushing one thumbnail layer onto the sheet. */
typedef struct bf_thumb_ctx {
    pc_surface surf; /* moved into the layer */
    const char *label;
} bf_thumb_ctx;

static pc_status bf_push_thumb(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                               pc_error *err)
{
    bf_thumb_ctx *ctx = vctx;
    pc_layer l;
    pc_status rc = pc_layer_raster(&l, ctx->label, pc_surface_format(&ctx->surf), err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *ls = pc_layer_surface_mut(&l);
    if (ls != NULL) {
        pc_surface_fini(ls);
        *ls = ctx->surf;         /* move */
        pc_surface_init(&ctx->surf, pc_surface_format(ls)); /* leave ctx owning an empty surface */
    }
    pc_layer_id nid = 0;
    rc = pc_document_insert_above(doc, NULL, &l, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    *has_active = true;
    *active = nid;
    return PC_OK;
}

/* Build the thumbnail surface for `path` fitted into cell_w x box_h at (x0,y0). On success *out is
 * a surface holding the thumbnail region; the caller moves it into a layer. */
static pc_status bf_make_thumb(const char *path, double cell_w, double box_h, double x0, double y0,
                               bool rotate, pc_pixel_format fmt, pc_surface *out, pc_error *err)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_read_file(path, &bytes, err);
    if (rc != PC_OK) {
        pc_buf_fini(&bytes);
        return rc;
    }
    pc_engine_session *t = NULL;
    rc = pc_engine_session_new(&t, err);
    if (rc != PC_OK) {
        pc_buf_fini(&bytes);
        return rc;
    }
    pc_document src;
    rc = pc_engine_import(bf_file_name(path), bytes.data, bytes.len, &src, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        pc_engine_session_free(t);
        return rc;
    }
    size_t sidx = 0;
    rc = pc_engine_session_add_document(t, &src, NULL, &sidx, err);
    if (rc != PC_OK) {
        pc_document_fini(&src);
        pc_engine_session_free(t);
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(t);
    double w = 1.0, h = 1.0;
    if (d != NULL) {
        w = (double)pc_doc_arc_get(d->doc)->size.width;
        h = (double)pc_doc_arc_get(d->doc)->size.height;
    }
    if (w < 1.0) {
        w = 1.0;
    }
    if (h < 1.0) {
        h = 1.0;
    }
    /* fit(w,h) = min(cell_w/w, box_h/h). */
    double fit_wh = (cell_w / w < box_h / h) ? cell_w / w : box_h / h;
    double fit_hw = (cell_w / h < box_h / w) ? cell_w / h : box_h / w;
    if (rotate && fit_hw > fit_wh * 1.001) {
        rc = bf_exec(t, "image.imageRotation.90cw", yyjson_mut_doc_new(NULL), NULL, err);
        if (rc != PC_OK) {
            pc_engine_session_free(t);
            return rc;
        }
        d = pc_engine_session_active(t);
        if (d != NULL) {
            w = (double)pc_doc_arc_get(d->doc)->size.width;
            h = (double)pc_doc_arc_get(d->doc)->size.height;
        }
        if (w < 1.0) {
            w = 1.0;
        }
        if (h < 1.0) {
            h = 1.0;
        }
    }
    double k = (cell_w / w < box_h / h) ? cell_w / w : box_h / h;
    double twf = round(w * k);
    double thf = round(h * k);
    if (twf < 1.0) {
        twf = 1.0;
    }
    if (thf < 1.0) {
        thf = 1.0;
    }
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    if (pd == NULL) {
        pc_engine_session_free(t);
        return pc_fail(err, PC_ERR_NOMEM, "file.automate.contactSheetII");
    }
    yyjson_mut_val *o = yyjson_mut_obj(pd);
    yyjson_mut_obj_add_real(pd, o, "width", twf);
    yyjson_mut_obj_add_real(pd, o, "height", thf);
    yyjson_mut_obj_add_str(pd, o, "resample", "bicubic");
    yyjson_mut_doc_set_root(pd, o);
    rc = bf_exec(t, "image.imageSize", pd, NULL, err);
    if (rc != PC_OK) {
        pc_engine_session_free(t);
        return rc;
    }
    d = pc_engine_session_active(t);
    if (d == NULL) {
        pc_engine_session_free(t);
        return pc_engine_err_no_document(err);
    }
    pc_compose_buffer buf = {0};
    rc = pc_compose_flatten(pc_doc_arc_get(d->doc), &buf, err);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&buf);
        pc_engine_session_free(t);
        return rc;
    }
    int32_t tw = (int32_t)twf, th = (int32_t)thf;
    int32_t ox = (int32_t)round(x0 + (cell_w - twf) / 2.0);
    int32_t oy = (int32_t)round(y0 + (box_h - thf) / 2.0);
    size_t nch = pc_pixel_format_channels(fmt);
    size_t count = (size_t)tw * (size_t)th;
    float *data = (nch > 0 && count > 0) ? pc_malloc_array(count * nch, sizeof(float)) : NULL;
    if (data == NULL) {
        pc_compose_buffer_fini(&buf);
        pc_engine_session_free(t);
        return pc_fail(err, PC_ERR_NOMEM, "file.automate.contactSheetII");
    }
    size_t usable = buf.len < count ? buf.len : count;
    for (size_t i = 0; i < usable; i++) {
        (void)pc_from_rgba(fmt, buf.px[i], &data[i * nch]);
    }
    pc_compose_buffer_fini(&buf);
    pc_engine_session_free(t);
    pc_surface surf;
    pc_surface_init(&surf, fmt);
    rc = pc_surface_write_region(&surf, pc_rect_make(ox, oy, ox + tw, oy + th), data, count * nch,
                                 err);
    free(data);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    *out = surf;
    return PC_OK;
}

static pc_status run_contact_sheet(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                   pc_error *err)
{
    const char *CMD = "file.automate.contactSheetII";
    char **files = NULL;
    size_t nfiles = 0;
    yyjson_val *inv = p != NULL ? pc_json_get(p, "input") : NULL;
    if (inv == NULL && p != NULL) {
        inv = pc_json_get(p, "files");
    }
    pc_status rc;
    if (inv != NULL && yyjson_is_arr(inv)) {
        rc = bf_collect_strings(inv, &files, &nfiles, err);
    } else {
        size_t len = 0;
        const char *dir = inv != NULL ? pc_json_as_str(inv, &len) : NULL;
        if (dir == NULL) {
            return pc_engine_err_bad_params(err, CMD,
                                            "missing \"input\" (a folder or an array of files)");
        }
        rc = bf_list_images(dir, &files, &nfiles, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    if (nfiles == 0) {
        bf_free_list(files, nfiles);
        return pc_engine_err_bad_params(err, CMD, "no images to place");
    }
    const char *unit = bf_str(p, "units");
    if (unit == NULL) {
        unit = "inches";
    }
    double res = bf_clamp(bf_pos(p, "resolution", 300.0), 1.0, 10000.0);
    uint32_t pw = (uint32_t)bf_clamp(round(bf_units_px(bf_pos(p, "width", 8.0), unit, res)), 16.0,
                                     30000.0);
    uint32_t ph = (uint32_t)bf_clamp(round(bf_units_px(bf_pos(p, "height", 10.0), unit, res)), 16.0,
                                     30000.0);
    uint32_t cols = (uint32_t)bf_clamp(round(bf_pos(p, "columns", 5.0)), 1.0, 100.0);
    uint32_t rows = (uint32_t)bf_clamp(round(bf_pos(p, "rows", 6.0)), 1.0, 100.0);
    bool autosp = bf_bool(p, "autoSpacing", true);
    double gap = (double)(pw < ph ? pw : ph) * 0.014;
    if (gap < 2.0) {
        gap = 2.0;
    }
    double hs = gap, vs = gap, tmp;
    if (!autosp) {
        hs = bf_f64(p, "horizontal", &tmp) ? bf_units_px(tmp, unit, res) : gap;
        vs = bf_f64(p, "vertical", &tmp) ? bf_units_px(tmp, unit, res) : gap;
    }
    bool across = bf_bool(p, "placeAcrossFirst", true);
    bool rotate = bf_bool(p, "rotateForBestFit", false);
    bool caption = bf_bool(p, "caption", true);
    const char *font = bf_str(p, "font");
    if (font == NULL) {
        font = PC_TEXT_DEFAULT_FAMILY;
    }
    double font_pt = bf_pos(p, "fontSize", 12.0);
    bool flatten = bf_bool(p, "flatten", false);
    const char *mode = bf_str(p, "mode");
    if (mode == NULL) {
        mode = "rgb";
    }
    int64_t depth = 8;
    (void)pc_json_as_i64(pc_json_get(p, "depth"), &depth);
    double caption_px = caption ? font_pt * res / 72.0 * 1.5 : 0.0;
    double cell_w = ((double)pw - (double)(cols + 1) * hs) / (double)cols;
    if (cell_w < 1.0) {
        cell_w = 1.0;
    }
    double cell_h = ((double)ph - (double)(rows + 1) * vs) / (double)rows;
    if (cell_h < 1.0) {
        cell_h = 1.0;
    }
    double box_h = cell_h - caption_px;
    if (box_h < 1.0) {
        box_h = 1.0;
    }
    size_t per_page = (size_t)cols * (size_t)rows;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        bf_free_list(files, nfiles);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *docs = yyjson_mut_arr(rd);
    yyjson_mut_val *errors = yyjson_mut_arr(rd);
    size_t placed = 0;
    size_t npages = (nfiles + per_page - 1) / per_page;
    for (size_t page = 0; page < npages; page++) {
        char name[48];
        if (nfiles > per_page) {
            (void)snprintf(name, sizeof name, "ContactSheet-%03zu", page + 1);
        } else {
            (void)snprintf(name, sizeof name, "ContactSheet-001");
        }
        yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
        if (pd == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
            break;
        }
        yyjson_mut_val *o = yyjson_mut_obj(pd);
        yyjson_mut_obj_add_uint(pd, o, "width", pw);
        yyjson_mut_obj_add_uint(pd, o, "height", ph);
        yyjson_mut_obj_add_str(pd, o, "mode", mode);
        yyjson_mut_obj_add_sint(pd, o, "depth", depth);
        yyjson_mut_obj_add_str(pd, o, "background", "white");
        yyjson_mut_obj_add_str(pd, o, "name", name);
        yyjson_mut_doc_set_root(pd, o);
        rc = bf_exec(s, "file.new", pd, NULL, err);
        if (rc != PC_OK) {
            break;
        }
        size_t idx = 0;
        if (!pc_engine_session_active_index(s, &idx)) {
            rc = pc_engine_err_no_document(err);
            break;
        }
        pc_engine_doc_state *dm = pc_engine_session_active_mut(s);
        pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
        if (dm != NULL) {
            pc_document *mut_doc = NULL;
            if (pc_doc_arc_make_mut(&dm->doc, &mut_doc, err) == PC_OK && mut_doc != NULL) {
                mut_doc->resolution_dpi = (float)res;
                fmt = pc_document_pixel_format(mut_doc);
            }
        }
        size_t base = page * per_page;
        size_t chunk = nfiles - base < per_page ? nfiles - base : per_page;
        for (size_t k = 0; k < chunk; k++) {
            const char *path = files[base + k];
            uint32_t c = across ? (uint32_t)(k % cols) : (uint32_t)(k / rows);
            uint32_t r = across ? (uint32_t)(k / cols) : (uint32_t)(k % rows);
            double x0 = hs + (double)c * (cell_w + hs);
            double y0 = vs + (double)r * (cell_h + vs);
            pc_surface surf;
            pc_error te = {0};
            if (bf_make_thumb(path, cell_w, box_h, x0, y0, rotate, fmt, &surf, &te) != PC_OK) {
                bf_push_error(rd, errors, path, te.msg);
                continue;
            }
            char label[256];
            pc_engine_file_stem(path, label, sizeof label);
            bf_thumb_ctx tc = {surf, label};
            rc = pc_engine_session_edit(s, "Contact Sheet", bf_push_thumb, &tc, err);
            pc_surface_fini(&tc.surf);
            if (rc != PC_OK) {
                break;
            }
            placed++;
            if (caption) {
                double baseline = y0 + box_h + caption_px * 0.72;
                yyjson_mut_doc *cp = yyjson_mut_doc_new(NULL);
                if (cp == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, CMD);
                    break;
                }
                yyjson_mut_val *co = yyjson_mut_obj(cp);
                yyjson_mut_obj_add_real(cp, co, "x", x0 + cell_w / 2.0);
                yyjson_mut_obj_add_real(cp, co, "y", baseline);
                yyjson_mut_obj_add_strcpy(cp, co, "text", label);
                yyjson_mut_obj_add_str(cp, co, "font", font);
                yyjson_mut_obj_add_real(cp, co, "size", font_pt);
                yyjson_mut_obj_add_str(cp, co, "align", "center");
                yyjson_mut_val *col = yyjson_mut_arr(cp);
                yyjson_mut_arr_add_real(cp, col, 0.0);
                yyjson_mut_arr_add_real(cp, col, 0.0);
                yyjson_mut_arr_add_real(cp, col, 0.0);
                yyjson_mut_obj_add_val(cp, co, "color", col);
                yyjson_mut_obj_add_strcpy(cp, co, "name", label);
                yyjson_mut_doc_set_root(cp, co);
                rc = bf_exec(s, "type.create", cp, NULL, err);
                if (rc != PC_OK) {
                    break;
                }
            }
        }
        if (rc != PC_OK) {
            break;
        }
        if (flatten) {
            rc = bf_exec(s, "layer.flattenImage", yyjson_mut_doc_new(NULL), NULL, err);
            if (rc != PC_OK) {
                break;
            }
        }
        yyjson_mut_arr_add_uint(rd, docs, idx);
    }
    bf_free_list(files, nfiles);
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, root, "documents", docs);
    yyjson_mut_obj_add_uint(rd, root, "pages", yyjson_mut_arr_size(docs));
    yyjson_mut_obj_add_uint(rd, root, "images", placed);
    yyjson_mut_obj_add_val(rd, root, "errors", errors);
    yyjson_mut_obj_add_uint(rd, root, "width", pw);
    yyjson_mut_obj_add_uint(rd, root, "height", ph);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- file.automate.createDroplet ----------------------------------------------------------- */

static pc_status run_create_droplet(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    const char *CMD = "file.automate.createDroplet";
    (void)s;
    const char *path = bf_str(p, "path");
    if (path == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"path\" (where to save the droplet)");
    }
    yyjson_val *steps_val = p != NULL ? pc_json_get(p, "steps") : NULL;
    if (steps_val == NULL && p != NULL) {
        steps_val = pc_json_get(p, "action");
    }
    if (steps_val == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"steps\" (the action to run)");
    }
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    yyjson_mut_val *steps = yyjson_mut_arr(md);
    pc_status rc = bf_collect_steps(md, steps, steps_val, CMD, err);
    if (rc != PC_OK) {
        yyjson_mut_doc_free(md);
        return rc;
    }
    size_t idx, max;
    yyjson_mut_val *pair;
    yyjson_mut_arr_foreach(steps, idx, max, pair) {
        const char *id = yyjson_mut_get_str(yyjson_mut_arr_get(pair, 0));
        if (id == NULL || pc_engine_command_find(id) == NULL) {
            /* `id` lives in md's string pool; copy it out before freeing md. */
            char idcopy[128];
            size_t ilen = id != NULL ? strlen(id) : 0;
            size_t ic = ilen < sizeof idcopy - 1 ? ilen : sizeof idcopy - 1;
            if (id != NULL) {
                memcpy(idcopy, id, ic);
            }
            idcopy[ic] = '\0';
            yyjson_mut_doc_free(md);
            return pc_engine_err_bad_params(err, CMD, "unknown command `%s` in the script", idcopy);
        }
    }
    /* Give the droplet its own path (".pcdroplet" unless already .pcdroplet/.json). */
    size_t plen = strlen(path);
    bool has_ext = (plen >= 10 && strcmp(path + plen - 10, ".pcdroplet") == 0) ||
                   (plen >= 5 && strcmp(path + plen - 5, ".json") == 0);
    char *dpath;
    if (has_ext) {
        dpath = pc_strndup(path, plen);
    } else {
        dpath = pc_malloc_array(plen + 11, 1);
        if (dpath != NULL) {
            memcpy(dpath, path, plen);
            memcpy(dpath + plen, ".pcdroplet", 11);
        }
    }
    if (dpath == NULL) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    char nbuf[256];
    const char *nm = bf_str(p, "name");
    if (nm == NULL) {
        pc_engine_file_stem(dpath, nbuf, sizeof nbuf);
        nm = nbuf;
    }
    /* Build the droplet document. */
    yyjson_mut_doc *dd = yyjson_mut_doc_new(NULL);
    if (dd == NULL) {
        free(dpath);
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    yyjson_mut_val *root = yyjson_mut_obj(dd);
    yyjson_mut_obj_add_uint(dd, root, "photocraftDroplet", 1);
    yyjson_mut_obj_add_strcpy(dd, root, "name", nm);
    yyjson_mut_val *action = yyjson_mut_obj(dd);
    yyjson_mut_obj_add_strcpy(dd, action, "name", nm);
    yyjson_mut_obj_add_val(dd, action, "steps", yyjson_mut_val_mut_copy(dd, steps));
    yyjson_mut_obj_add_val(dd, root, "action", action);
    yyjson_mut_val *options = yyjson_mut_obj(dd);
    static const char *const OPT_KEYS[] = {"output", "format", "quality", "suffix", "overrideOpen"};
    for (size_t i = 0; i < sizeof OPT_KEYS / sizeof OPT_KEYS[0]; i++) {
        yyjson_val *v = p != NULL ? pc_json_get(p, OPT_KEYS[i]) : NULL;
        if (v != NULL) {
            yyjson_mut_obj_add_val(dd, options, OPT_KEYS[i], yyjson_val_mut_copy(dd, v));
        }
    }
    yyjson_mut_obj_add_val(dd, root, "options", options);
    yyjson_mut_doc_set_root(dd, root);
    size_t json_len = 0;
    char *text = yyjson_mut_write(dd, YYJSON_WRITE_PRETTY, &json_len);
    yyjson_mut_doc_free(dd);
    if (text == NULL) {
        free(dpath);
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    rc = pc_engine_write_file(dpath, text, json_len, err);
    free(text);
    size_t nsteps = yyjson_mut_arr_size(steps);
    yyjson_mut_doc_free(md);
    if (rc != PC_OK) {
        free(dpath);
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(dpath);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *rroot = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, rroot, "path", dpath);
    yyjson_mut_obj_add_null(rd, rroot, "shim"); /* .command shim deferred (OS packaging) */
    yyjson_mut_obj_add_uint(rd, rroot, "steps", nsteps);
    yyjson_mut_doc_set_root(rd, rroot);
    free(dpath);
    return PC_OK;
}

/* ---- file.automate.runDroplet -------------------------------------------------------------- */

static pc_status run_run_droplet(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                 pc_error *err)
{
    const char *CMD = "file.automate.runDroplet";
    const char *path = bf_str(p, "droplet");
    if (path == NULL) {
        path = bf_str(p, "path");
    }
    if (path == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"droplet\"");
    }
    char *path_dup = pc_strndup(path, strlen(path));
    if (path_dup == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_engine_read_file(path_dup, &bytes, err);
    if (rc != PC_OK) {
        pc_buf_fini(&bytes);
        free(path_dup);
        return rc;
    }
    yyjson_doc *dv = NULL;
    rc = pc_json_parse((const char *)bytes.data, bytes.len, &dv, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        free(path_dup);
        return rc;
    }
    yyjson_val *droot = yyjson_doc_get_root(dv);
    if (pc_json_get(droot, "photocraftDroplet") == NULL) {
        yyjson_doc_free(dv);
        pc_status fr = pc_engine_err_bad_params(err, CMD, "%s is not a PhotoCraft droplet", path_dup);
        free(path_dup);
        return fr;
    }
    yyjson_val *action = pc_json_get(droot, "action");
    yyjson_val *steps = action != NULL ? pc_json_get(action, "steps") : NULL;
    if (steps == NULL) {
        yyjson_doc_free(dv);
        free(path_dup);
        return pc_engine_err_bad_params(err, CMD, "droplet has no action");
    }
    yyjson_val *opts = pc_json_get(droot, "options");
    /* Resolve inputs: each file as is, each folder expanded. */
    yyjson_val *raw = p != NULL ? pc_json_get(p, "input") : NULL;
    if (raw == NULL && p != NULL) {
        raw = pc_json_get(p, "files");
    }
    char **rawlist = NULL;
    size_t nraw = 0;
    if (raw != NULL && yyjson_is_arr(raw)) {
        rc = bf_collect_strings(raw, &rawlist, &nraw, err);
    } else {
        size_t len = 0;
        const char *one = raw != NULL ? pc_json_as_str(raw, &len) : NULL;
        if (one == NULL) {
            yyjson_doc_free(dv);
            free(path_dup);
            return pc_engine_err_bad_params(err, CMD, "missing \"input\" (files or folders)");
        }
        rawlist = pc_malloc_array(1, sizeof(char *));
        if (rawlist != NULL) {
            rawlist[0] = pc_strndup(one, len);
            nraw = rawlist[0] != NULL ? 1 : 0;
        }
        rc = (rawlist != NULL && nraw == 1) ? PC_OK : pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    if (rc != PC_OK) {
        bf_free_list(rawlist, nraw);
        yyjson_doc_free(dv);
        free(path_dup);
        return rc;
    }
    /* Expand folders. */
    char **inputs = NULL;
    size_t ninputs = 0, cap = 0;
    for (size_t i = 0; i < nraw && rc == PC_OK; i++) {
        struct stat st;
        if (stat(rawlist[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            char **imgs = NULL;
            size_t nimgs = 0;
            rc = bf_list_images(rawlist[i], &imgs, &nimgs, err);
            for (size_t j = 0; j < nimgs && rc == PC_OK; j++) {
                char **g = pc_grow_array(inputs, &cap, ninputs + 1, sizeof *inputs);
                if (g == NULL) {
                    rc = pc_fail(err, PC_ERR_NOMEM, CMD);
                    break;
                }
                inputs = g;
                inputs[ninputs++] = imgs[j];
                imgs[j] = NULL;
            }
            bf_free_list(imgs, nimgs);
        } else {
            char **g = pc_grow_array(inputs, &cap, ninputs + 1, sizeof *inputs);
            if (g == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, CMD);
                break;
            }
            inputs = g;
            inputs[ninputs] = pc_strndup(rawlist[i], strlen(rawlist[i]));
            if (inputs[ninputs] == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, CMD);
                break;
            }
            ninputs++;
        }
    }
    bf_free_list(rawlist, nraw);
    if (rc == PC_OK && ninputs == 0) {
        rc = pc_engine_err_bad_params(err, CMD, "no input images");
    }
    /* Output: param, else droplet option, else <first input folder>/droplet-output. */
    char *output = NULL;
    const char *outp = bf_str(p, "output");
    if (outp == NULL && opts != NULL) {
        size_t ol = 0;
        const char *v = pc_json_as_str(pc_json_get(opts, "output"), &ol);
        if (v != NULL && ol > 0) {
            output = pc_strndup(v, ol);
        }
    } else if (outp != NULL) {
        output = pc_strndup(outp, strlen(outp));
    }
    if (rc == PC_OK && output == NULL && inputs != NULL && ninputs > 0) {
        /* parent folder of the first input */
        const char *first = inputs[0];
        const char *slash = NULL;
        for (const char *c = first; *c != '\0'; c++) {
            if (*c == '/' || *c == '\\') {
                slash = c;
            }
        }
        char parent[4096];
        if (slash != NULL) {
            size_t pl = (size_t)(slash - first);
            if (pl >= sizeof parent) {
                pl = sizeof parent - 1;
            }
            memcpy(parent, first, pl);
            parent[pl] = '\0';
        } else {
            parent[0] = '\0';
        }
        output = pc_engine_file_join(parent, "droplet-output");
    }
    if (rc == PC_OK && output == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    /* Build the batch params and dispatch. */
    pc_json_value batch_res = PC_JSON_VALUE_INIT;
    if (rc == PC_OK) {
        yyjson_mut_doc *bp = yyjson_mut_doc_new(NULL);
        if (bp == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, CMD);
        } else {
            yyjson_mut_val *bo = yyjson_mut_obj(bp);
            yyjson_mut_obj_add_val(bp, bo, "steps", yyjson_val_mut_copy(bp, steps));
            yyjson_mut_val *inarr = yyjson_mut_arr(bp);
            for (size_t i = 0; i < ninputs; i++) {
                yyjson_mut_arr_add_strcpy(bp, inarr, inputs[i]);
            }
            yyjson_mut_obj_add_val(bp, bo, "input", inarr);
            yyjson_mut_obj_add_strcpy(bp, bo, "output", output);
            const char *fmt = opts != NULL ? bf_str(opts, "format") : NULL;
            yyjson_mut_obj_add_strcpy(bp, bo, "format", fmt != NULL ? fmt : "same");
            yyjson_val *q = opts != NULL ? pc_json_get(opts, "quality") : NULL;
            if (q != NULL) {
                yyjson_mut_obj_add_val(bp, bo, "quality", yyjson_val_mut_copy(bp, q));
            }
            yyjson_mut_doc_set_root(bp, bo);
            rc = bf_exec(s, "file.automate.batch", bp, &batch_res, err);
        }
    }
    bf_free_list(inputs, ninputs);
    yyjson_doc_free(dv);
    if (rc != PC_OK) {
        free(output);
        free(path_dup);
        pc_json_value_fini(&batch_res);
        return rc;
    }
    /* result {droplet, output, files, errors} */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(output);
        free(path_dup);
        pc_json_value_fini(&batch_res);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "droplet", path_dup);
    yyjson_mut_obj_add_strcpy(rd, root, "output", output);
    yyjson_mut_val *br = pc_json_value_root(&batch_res);
    yyjson_mut_val *bf = br != NULL ? yyjson_mut_obj_get(br, "files") : NULL;
    yyjson_mut_val *be = br != NULL ? yyjson_mut_obj_get(br, "errors") : NULL;
    yyjson_mut_obj_add_val(rd, root, "files",
                           bf != NULL ? yyjson_mut_val_mut_copy(rd, bf) : yyjson_mut_arr(rd));
    yyjson_mut_obj_add_val(rd, root, "errors",
                           be != NULL ? yyjson_mut_val_mut_copy(rd, be) : yyjson_mut_arr(rd));
    yyjson_mut_doc_set_root(rd, root);
    pc_json_value_fini(&batch_res);
    free(output);
    free(path_dup);
    return PC_OK;
}

/* ---- file.automate.lensCorrection (lens_cmds::lens_batch) ---------------------------------- */

typedef struct bf_lens_ctx {
    yyjson_val *params; /* the filter params (input doc, read-only) */
} bf_lens_ctx;

static pc_status bf_lens_each(pc_engine_session *s, void *vctx, pc_error *err)
{
    bf_lens_ctx *ctx = vctx;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_doc_arc_get(d->doc)->nlayers > 1) {
        pc_status rc = bf_exec(s, "layer.flattenImage", yyjson_mut_doc_new(NULL), NULL, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    if (pd == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "filter.lensCorrection");
    }
    yyjson_mut_val *root = ctx->params != NULL ? yyjson_val_mut_copy(pd, ctx->params)
                                               : yyjson_mut_obj(pd);
    if (!yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(pd);
    }
    /* Drop the batch-only keys; default the EXIF-driven profile. */
    yyjson_mut_obj_remove_key(root, "input");
    yyjson_mut_obj_remove_key(root, "files");
    yyjson_mut_obj_remove_key(root, "output");
    yyjson_mut_obj_remove_key(root, "format");
    yyjson_mut_obj_remove_key(root, "quality");
    if (yyjson_mut_obj_get(root, "profile") == NULL) {
        yyjson_mut_obj_add_str(pd, root, "profile", "auto");
    }
    yyjson_mut_doc_set_root(pd, root);
    return bf_exec(s, "filter.lensCorrection", pd, NULL, err);
}

static pc_status run_lens_batch(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                pc_error *err)
{
    const char *CMD = "file.automate.lensCorrection";
    const char *output = bf_str(p, "output");
    if (output == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing \"output\"");
    }
    const char *format = bf_str(p, "format");
    char *output_dup = pc_strndup(output, strlen(output));
    char *format_dup = pc_strndup(format != NULL ? format : "same",
                                  format != NULL ? strlen(format) : 4);
    pc_engine_save_opts save = pc_engine_save_opts_default();
    pc_status rc = pc_engine_save_opts_from_params(s, p, CMD, &save, err);
    char **inputs = NULL;
    size_t ninputs = 0;
    if (rc == PC_OK) {
        rc = bf_batch_inputs(p, CMD, &inputs, &ninputs, err);
    }
    if (rc == PC_OK && (output_dup == NULL || format_dup == NULL)) {
        rc = pc_fail(err, PC_ERR_NOMEM, CMD);
    }
    if (rc == PC_OK) {
        bf_lens_ctx ctx = {p};
        rc = bf_process_files(inputs, ninputs, output_dup, format_dup, &save, "", bf_lens_each,
                              &ctx, out, err);
    }
    bf_free_list(inputs, ninputs);
    free(output_dup);
    free(format_dup);
    return rc;
}

/* ---- specs --------------------------------------------------------------------------------- */

static const char *const MENU_FILE_AUTOMATE[] = {"File", "Automate"};
static const char *const MENU_FILE_SCRIPTS[] = {"File", "Scripts"};

static const pc_engine_command SPECS[] = {
    {"file.automate.batch", "Batch\xe2\x80\xa6", MENU_FILE_AUTOMATE, 2, NULL,
     "{\"steps\":[[commandId,params]|{\"command\":id,\"params\":{}}\xe2\x80\xa6] (a recorded action),"
     "\"input\":folder|[paths],\"output\":folder,\"format\":\"same|png|jpg|psd|tiff|\xe2\x80\xa6\"=\"same\","
     "\"quality\":0..12?,\"tiffLayers\":bool=false,\"tgaBits\":24|32?,\"dpxBits\":8|10|12|16?,"
     "\"calledActions\":[{\"name\",\"steps\"}\xe2\x80\xa6]?} \xe2\x86\x92 {files, errors} (an input whose output "
     "name was already written in the run goes to errors)",
     bf_native, run_batch, true},
    {"file.scripts.imageProcessor", "Image Processor\xe2\x80\xa6", MENU_FILE_SCRIPTS, 2, NULL,
     "{\"input\":folder|[paths],\"output\":folder,\"format\":\"jpg|png|psd|tiff|\xe2\x80\xa6\"=\"jpg\","
     "\"quality\":0..12=8,\"tiffLayers\":bool=false,\"tgaBits\":24|32?,\"dpxBits\":8|10|12|16?,"
     "\"width\":px?,\"height\":px? (fit, never enlarge),\"convertToSrgb\":bool=false} \xe2\x86\x92 {files, "
     "errors} (an input whose output name was already written in the run goes to errors)",
     bf_native, run_image_processor, true},
    {"file.scripts.loadFilesIntoStack", "Load Files into Stack\xe2\x80\xa6", MENU_FILE_SCRIPTS, 2, NULL,
     "{\"paths\":[str]|folder,\"align\":bool=false (Auto-Align Layers on them first),"
     "\"createSmartObject\":bool=false} \xe2\x86\x92 new document with one layer per file (inside one smart "
     "object with createSmartObject)",
     bf_native, run_load_files_into_stack, true},
    {"file.scripts.statistics", "Statistics\xe2\x80\xa6", MENU_FILE_SCRIPTS, 2, NULL,
     "{\"mode\":\"mean|median|maximum|minimum|range|summation|variance|standardDeviation|skewness|"
     "kurtosis|entropy\"=\"median\",\"input\":folder|[paths],\"align\":bool=false (Auto-Align first)} "
     "\xe2\x86\x92 new document with one stack-mode smart object",
     bf_native, run_statistics, true},
    {"file.automate.contactSheetII", "Contact Sheet II\xe2\x80\xa6", MENU_FILE_AUTOMATE, 2, NULL,
     "{\"input\":folder|[paths],\"units\":\"inches|cm|mm|pixels\"=\"inches\",\"width\":8,\"height\":10,"
     "\"resolution\":ppi=300,\"mode\":\"rgb|gray|cmyk|lab\"=\"rgb\",\"depth\":8|16=8,\"columns\":5,"
     "\"rows\":6,\"placeAcrossFirst\":bool=true,\"autoSpacing\":bool=true,\"horizontal\":units?,"
     "\"vertical\":units?,\"rotateForBestFit\":bool=false,\"caption\":bool=true (file name as caption),"
     "\"font\":family?,\"fontSize\":pt=12,\"flatten\":bool=false} \xe2\x86\x92 {documents, pages, images}",
     bf_native, run_contact_sheet, true},
    {"file.automate.createDroplet", "Create Droplet\xe2\x80\xa6", MENU_FILE_AUTOMATE, 2, NULL,
     "{\"path\":str (.pcdroplet),\"steps\":[[id,params]\xe2\x80\xa6] (the action),\"name\":str?,"
     "\"output\":folder?,\"format\":\"same|png|jpg|\xe2\x80\xa6\"?,\"quality\":0..12?} \xe2\x86\x92 {path, shim}",
     bf_native, run_create_droplet, true},
    {"file.automate.runDroplet", "Run Droplet", NULL, 0, NULL,
     "{\"droplet\":path,\"input\":[files or folders],\"output\":folder? (default: droplet's, else "
     "<input folder>/droplet-output)} \xe2\x86\x92 {files, errors}",
     bf_native, run_run_droplet, true},
    {"file.automate.lensCorrection", "Lens Correction\xe2\x80\xa6", MENU_FILE_AUTOMATE, 2, NULL,
     "{\"input\":folder|[files],\"output\":folder,\"format\":\"same|jpg|png|tif|psd\",\"quality\":0..12?, "
     "\xe2\x80\xa6" "Filter \xe2\x80\xba Lens Correction params (\"profile\" defaults to \"auto\")}",
     bf_native, run_lens_batch, true},
};

const pc_engine_command *pc_engine_batch_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
