/* The command registry, param helpers, enabled predicates and the base command set
 * (port of crates/engine/src/commands.rs). Ids follow Photoshop's menu structure and must match
 * the reference byte-for-byte. dispatch and the job commands live in jobs.c.
 *
 * Registering a command module: a later engine chunk adds one line to register_modules() below,
 * naming its pc_engine_<area>_specs() (see the API notes at the top of engine.h). The registry is
 * built once and looked up by exact id; the first spec for a duplicated id wins. */
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "color/color.h"
#include "compose/compose.h"
#include "engine_internal.h"
#include "layer_cmds.h" /* E5: layer_multi/group_view/layer_nav/layer_label/mask_view specs */
#include "layer_base_cmds.h" /* E5-base: layer.new.layer/new.group/duplicate/delete/select/… + clip/arrange */
#include "channel_cmds.h" /* E7: channels, save/load selection, quick mask */
#include "selection_cmds.h" /* E7: select tools, magnetic lasso, sky/isolate, smart select */
#include "layer_style.h" /* E6: layer.layerStyle.* */
#include "smart_cmds.h"  /* E6: layer.smartObjects.* / layer.smartFilter.* */
#include "artboard_cmds.h" /* E6: view.clearSelectedArtboardGuides */
#include "fill_cmds.h"   /* E4: edit.fill + select.rect/float/stamp/fill-key module registries */
#include "edit_cmds.h"   /* E4: clipboard (cut/copy/paste), clear, layer via copy/cut, merge visible */
#include "transform_cmds.h" /* E4/ET1: edit.transform Free Transform + frame-based presets */
#include "extra_cmds.h"  /* E12: Stroke, Reselect, Equalize, rasterize, ungroup, layer-mask toggles, clouds */
#include "align_distribute_cmds.h" /* GAP: layer align + distribute */
#include "view_proof_guide_cmds.h"  /* GAP: view.proofSetup.* variants + guide commands */

/* ---- owned JSON results ------------------------------------------------------------------- */

pc_status pc_engine_result_doc(pc_json_value *out, yyjson_mut_doc **doc, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory building a result");
    }
    memset(out, 0, sizeof *out);
    out->doc = d;
    *doc = d;
    return PC_OK;
}

pc_status pc_engine_result_null(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, yyjson_mut_null(d));
    return PC_OK;
}

static pc_status result_bool(pc_json_value *out, bool v, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, yyjson_mut_bool(d, v));
    return PC_OK;
}

/* ---- param helpers ------------------------------------------------------------------------ */

bool pc_engine_param_int(yyjson_val *p, const char *key, int64_t *out)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return false;
    }
    int64_t i;
    if (pc_json_as_i64(v, &i)) {
        *out = i;
        return true;
    }
    double f;
    if (pc_json_as_f64(v, &f) && isfinite(f)) {
        /* Rust f64::round: half away from zero. */
        *out = (int64_t)llround(f);
        return true;
    }
    return false;
}

pc_status pc_engine_param_int_i32(const char *cmd, yyjson_val *p, const char *key, bool *present,
                                  int32_t *out, pc_error *err)
{
    int64_t v;
    if (!pc_engine_param_int(p, key, &v)) {
        *present = false;
        return PC_OK;
    }
    if (v < INT32_MIN || v > INT32_MAX) {
        return pc_engine_err_bad_params(err, cmd, "`%s` = %lld is outside the 32-bit coordinate range",
                                        key, (long long)v);
    }
    *present = true;
    *out = (int32_t)v;
    return PC_OK;
}

pc_status pc_engine_param_u32_id(const char *cmd, const char *key, yyjson_val *v, uint32_t *out, pc_error *err)
{
    uint64_t u;
    if (v != NULL && pc_json_as_u64(v, &u) && u <= UINT32_MAX) {
        *out = (uint32_t)u;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "`%s` must be a non-negative whole number id", key);
}

/* "#rgb", "#rrggbb", "#rrggbbaa" -> straight RGBA (0..1); false when malformed. */
bool pc_engine_parse_hex(const char *s, size_t len, float out[4])
{
    if (len == 0 || s[0] != '#') {
        return false;
    }
    const char *h = s + 1;
    size_t n = len - 1;
    unsigned v[4] = {0, 0, 0, 255};
    if (n == 3 || n == 4) {
        for (size_t i = 0; i < n; i++) {
            int c = h[i];
            int d;
            if (c >= '0' && c <= '9') {
                d = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                d = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                d = c - 'A' + 10;
            } else {
                return false;
            }
            v[i] = (unsigned)(d * 16 + d);
        }
    } else if (n == 6 || n == 8) {
        for (size_t i = 0; i < n; i += 2) {
            int hi = h[i], lo = h[i + 1];
            int dh, dl;
            if (hi >= '0' && hi <= '9') {
                dh = hi - '0';
            } else if (hi >= 'a' && hi <= 'f') {
                dh = hi - 'a' + 10;
            } else if (hi >= 'A' && hi <= 'F') {
                dh = hi - 'A' + 10;
            } else {
                return false;
            }
            if (lo >= '0' && lo <= '9') {
                dl = lo - '0';
            } else if (lo >= 'a' && lo <= 'f') {
                dl = lo - 'a' + 10;
            } else if (lo >= 'A' && lo <= 'F') {
                dl = lo - 'A' + 10;
            } else {
                return false;
            }
            v[i / 2] = (unsigned)(dh * 16 + dl);
        }
    } else {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (float)v[i] / 255.0f;
    }
    return true;
}

void pc_engine_param_color(yyjson_val *p, const char *key, const float def[4], float out[4])
{
    out[0] = def[0];
    out[1] = def[1];
    out[2] = def[2];
    out[3] = def[3];
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return;
    }
    size_t slen = 0;
    const char *s = pc_json_as_str(v, &slen);
    if (s != NULL) {
        (void)pc_engine_parse_hex(s, slen, out);
        return;
    }
    if (yyjson_is_arr(v)) {
        size_t idx = 0, max = yyjson_arr_size(v);
        yyjson_val *e;
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        while ((e = yyjson_arr_iter_next(&it)) != NULL && idx < 4) {
            double f;
            if (pc_json_as_f64(e, &f)) {
                out[idx] = (float)f;
            }
            idx++;
        }
        (void)max;
    }
}

yyjson_mut_val *pc_engine_inject_kind(const char *id, yyjson_mut_doc *doc, yyjson_val *params)
{
    const char *kind = NULL;
    const char *pfx1 = "layer.newAdjustmentLayer.";
    const char *pfx2 = "image.adjustments.";
    size_t l1 = strlen(pfx1), l2 = strlen(pfx2);
    if (strncmp(id, pfx1, l1) == 0) {
        kind = id + l1;
    } else if (strncmp(id, pfx2, l2) == 0) {
        kind = id + l2;
    }
    yyjson_mut_val *root;
    if (params != NULL) {
        root = pc_json_normalized_copy_val(doc, params);
    } else {
        root = yyjson_mut_null(doc);
    }
    if (kind == NULL) {
        return root;
    }
    if (root == NULL || !yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(doc);
    }
    yyjson_mut_obj_add_str(doc, root, "__kind", kind);
    return root;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

pc_status pc_engine_always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

pc_status pc_engine_has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) != NULL) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no document open");
}

const pc_layer *pc_engine_active_layer_of(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        (void)pc_fail(reason, PC_ERR_INVALID, "no document open");
        return NULL;
    }
    if (!d->has_active_layer) {
        (void)pc_fail(reason, PC_ERR_INVALID, "no active layer");
        return NULL;
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer);
    if (l == NULL) {
        (void)pc_fail(reason, PC_ERR_INVALID, "no active layer");
        return NULL;
    }
    return l;
}

pc_status pc_engine_has_layer(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_active_layer_of(s, reason) != NULL ? PC_OK : PC_ERR_INVALID;
}

static pc_status can_undo(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_history_can_undo(d->history)) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "nothing to undo");
}

static pc_status can_redo(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_history_can_redo(d->history)) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "nothing to redo");
}

static pc_status has_selection(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_doc_arc_get(d->doc)->has_selection) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "no selection");
}

/* ---- base command runs -------------------------------------------------------------------- */

static pc_status run_file_close(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t i;
    uint64_t u;
    yyjson_val *v = pc_json_get(p, "document");
    if (v != NULL && pc_json_as_u64(v, &u)) {
        i = (size_t)u;
    } else if (!pc_engine_session_active_index(s, &i)) {
        return pc_engine_err_no_document(err);
    }
    if (!pc_engine_session_close(s, i)) {
        return pc_engine_err_no_document(err);
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_undo(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return result_bool(out, pc_engine_session_undo(s), err);
}

static pc_status run_redo(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return result_bool(out, pc_engine_session_redo(s), err);
}

static pc_status edit_select_all(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)ctx;
    pc_surface m;
    pc_surface_init(&m, PC_PIXEL_FORMAT_GRAY8);
    float one = 1.0f;
    if (pc_surface_fill_rect(&m, pc_document_bounds(doc), &one, 1, err) != PC_OK) {
        pc_surface_fini(&m);
        return PC_ERR_NOMEM;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
    }
    doc->selection = m;
    doc->has_selection = true;
    return PC_OK;
}

static pc_status edit_deselect(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)ctx;
    (void)err;
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

static pc_status edit_inverse(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)ctx;
    pc_rect b = pc_document_bounds(doc);
    pc_surface empty;
    bool have_empty = false;
    const pc_surface *old;
    if (doc->has_selection) {
        old = &doc->selection;
    } else {
        pc_surface_init(&empty, PC_PIXEL_FORMAT_GRAY8);
        have_empty = true;
        old = &empty;
    }
    float *data = NULL;
    size_t len = 0;
    pc_status rc = pc_surface_read_region(old, b, &data, &len, err);
    if (have_empty) {
        pc_surface_fini(&empty);
    }
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < len; i++) {
        data[i] = 1.0f - data[i];
    }
    pc_surface m;
    pc_surface_init(&m, PC_PIXEL_FORMAT_GRAY8);
    rc = pc_surface_write_region(&m, b, data, len, err);
    free(data);
    if (rc != PC_OK) {
        pc_surface_fini(&m);
        return rc;
    }
    pc_surface_prune(&m);
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
    }
    doc->selection = m;
    doc->has_selection = true;
    return PC_OK;
}

static pc_status run_select_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (pc_engine_session_edit(s, "Select All", edit_select_all, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_select_deselect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (pc_engine_session_edit(s, "Deselect", edit_deselect, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_select_inverse(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (pc_engine_session_edit(s, "Inverse", edit_inverse, NULL, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_session_inspect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return pc_engine_inspect_session(s, out, err);
}

static pc_status run_document_inspect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d;
    yyjson_val *v = pc_json_get(p, "document");
    if (v != NULL) {
        uint64_t u;
        if (!pc_json_as_u64(v, &u)) {
            return pc_engine_err_bad_params(err, "document.inspect", "`document` must be an index");
        }
        size_t n;
        const pc_engine_doc_state *docs = pc_engine_session_documents(s, &n);
        if ((size_t)u >= n) {
            return pc_engine_err_other(err, "no document at index %llu", (unsigned long long)u);
        }
        d = &docs[(size_t)u];
    } else {
        d = pc_engine_session_active(s);
        if (d == NULL) {
            return pc_engine_err_no_document(err);
        }
    }
    return pc_engine_inspect_document(d, out, err);
}

static pc_status run_document_activate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "document");
    uint64_t u;
    if (v == NULL || !pc_json_as_u64(v, &u)) {
        return pc_engine_err_bad_params(err, "document.activate", "missing `document`");
    }
    if (!pc_engine_session_set_active(s, (size_t)u)) {
        return pc_engine_err_other(err, "no document at index %llu", (unsigned long long)u);
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_document_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t from;
    yyjson_val *vd = pc_json_get(p, "document");
    if (vd != NULL) {
        uint64_t u;
        if (!pc_json_as_u64(vd, &u)) {
            return pc_engine_err_bad_params(err, "document.move", "`document` must be a tab index");
        }
        from = (size_t)u;
    } else if (!pc_engine_session_active_index(s, &from)) {
        return pc_engine_err_no_document(err);
    }
    yyjson_val *vt = pc_json_get(p, "to");
    uint64_t t;
    if (vt == NULL || !pc_json_as_u64(vt, &t)) {
        return pc_engine_err_bad_params(err, "document.move", "missing `to`");
    }
    size_t to;
    if (!pc_engine_session_move_document(s, from, (size_t)t, &to)) {
        return pc_engine_err_no_document(err);
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "document", to);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_command_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    size_t n;
    const pc_engine_command *cmds = pc_engine_commands(&n);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < n; i++) {
        pc_error reason = {0};
        const char *why = NULL;
        if (cmds[i].enabled(s, &reason) != PC_OK) {
            why = reason.msg;
        }
        pc_json_value row;
        if (pc_engine_command_list_entry(s, &cmds[i], why, &row, err) != PC_OK) {
            pc_json_value_fini(out);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(d, pc_json_value_root(&row));
        pc_json_value_fini(&row);
        if (copy == NULL) {
            pc_json_value_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "command list");
        }
        yyjson_mut_arr_append(arr, copy);
    }
    yyjson_mut_doc_set_root(d, arr);
    return PC_OK;
}

static pc_status run_document_pixel(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    int64_t xi = 0, yi = 0;
    (void)pc_engine_param_int(p, "x", &xi);
    (void)pc_engine_param_int(p, "y", &yi);
    if (xi < INT32_MIN || xi > INT32_MAX) {
        return pc_engine_err_bad_params(err, "document.pixel", "x must fit in 32 bits");
    }
    if (yi < INT32_MIN || yi > INT32_MAX) {
        return pc_engine_err_bad_params(err, "document.pixel", "y must fit in 32 bits");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_compose_buffer buf;
    if (pc_compose_render(pc_doc_arc_get(d->doc), pc_rect_from_xywh((int32_t)xi, (int32_t)yi, 1, 1), &buf,
                          err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    float px[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (buf.len > 0) {
        px[0] = buf.px[0][0];
        px[1] = buf.px[0][1];
        px[2] = buf.px[0][2];
        px[3] = buf.px[0][3];
    }
    pc_compose_buffer_fini(&buf);
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(jd);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_real(jd, arr, (double)px[i]);
    }
    yyjson_mut_doc_set_root(jd, arr);
    return PC_OK;
}

/* ---- base command table ------------------------------------------------------------------- */

static const char *const MENU_FILE[] = {"File"};
static const char *const MENU_EDIT[] = {"Edit"};
static const char *const MENU_SELECT[] = {"Select"};

static const pc_engine_command BASE[] = {
    {"file.close", "Close", MENU_FILE, 1, "Cmd+W", "{\"document\":index?}", pc_engine_has_doc,
     run_file_close, true},
    {"edit.undo", "Undo", MENU_EDIT, 1, "Cmd+Z", "{}", can_undo, run_undo, true},
    {"edit.redo", "Redo", MENU_EDIT, 1, "Cmd+Shift+Z", "{}", can_redo, run_redo, true},
    {"select.all", "All", MENU_SELECT, 1, "Cmd+A", "{}", pc_engine_has_doc, run_select_all, true},
    {"select.deselect", "Deselect", MENU_SELECT, 1, "Cmd+D", "{}", has_selection, run_select_deselect, true},
    {"select.inverse", "Inverse", MENU_SELECT, 1, "Cmd+Shift+I", "{}", has_selection, run_select_inverse, true},
    {"session.inspect", "Inspect Session", NULL, 0, NULL, "{}", pc_engine_always, run_session_inspect, false},
    {"document.inspect", "Inspect Document", NULL, 0, NULL, "{\"document\":index?}", pc_engine_has_doc,
     run_document_inspect, false},
    {"document.activate", "Activate Document", NULL, 0, NULL, "{\"document\":index}", pc_engine_has_doc,
     run_document_activate, false},
    {"document.move", "Move Document", NULL, 0, NULL, "{\"document\":index?,\"to\":index}", pc_engine_has_doc,
     run_document_move, false},
    {"command.list", "List Commands", NULL, 0, NULL, "{}", pc_engine_always, run_command_list, false},
    {"document.pixel", "Read Composite Pixel", NULL, 0, NULL, "{\"x\":i32,\"y\":i32}", pc_engine_has_doc,
     run_document_pixel, false},
};

/* ---- the registry ------------------------------------------------------------------------- */

static pc_engine_command *g_specs;
static size_t g_nspecs;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static bool append_specs(pc_engine_command **arr, size_t *n, size_t *cap, const pc_engine_command *src,
                         size_t count)
{
    pc_engine_command *grown = pc_grow_array(*arr, cap, *n + count, sizeof **arr);
    if (grown == NULL) {
        return false;
    }
    *arr = grown;
    memcpy(&(*arr)[*n], src, count * sizeof **arr);
    *n += count;
    return true;
}

/* The module aggregator. Base commands first, then each command module's specs(). A later engine
 * chunk adds one line here (a surgical edit to this shared file), mirroring the Rust
 * `v.extend(crate::<area>::specs())` list in commands.rs::build(). */
static void register_modules(pc_engine_command **arr, size_t *n, size_t *cap)
{
    size_t m;
    const pc_engine_command *jobs = pc_engine_jobs_specs(&m);
    (void)append_specs(arr, n, cap, jobs, m);
    const pc_engine_command *prefs = pc_engine_prefs_specs(&m); /* E2 */
    (void)append_specs(arr, n, cap, prefs, m);
    const pc_engine_command *docpreset = pc_engine_document_preset_specs(&m); /* E3: file.new */
    (void)append_specs(arr, n, cap, docpreset, m);
    const pc_engine_command *wia = pc_engine_wia_specs(&m); /* E3: File > Import > WIA */
    (void)append_specs(arr, n, cap, wia, m);
    const pc_engine_command *multichannel = pc_engine_multichannel_specs(&m); /* E3 */
    (void)append_specs(arr, n, cap, multichannel, m);
    const pc_engine_command *image = pc_engine_image_specs(&m); /* E3: Image menu */
    (void)append_specs(arr, n, cap, image, m);
    const pc_engine_command *mode = pc_engine_mode_specs(&m); /* E3: Image > Mode palette/ink modes */
    (void)append_specs(arr, n, cap, mode, m);
    const pc_engine_command *file = pc_engine_file_specs(&m); /* E3: File menu + guides */
    (void)append_specs(arr, n, cap, file, m);
    const pc_engine_command *print = pc_engine_print_specs(&m); /* E3: Paths to Illustrator */
    (void)append_specs(arr, n, cap, print, m);
    const pc_engine_command *layer_multi = pc_engine_layer_multi_specs(&m); /* E5 */
    (void)append_specs(arr, n, cap, layer_multi, m);
    const pc_engine_command *group_view = pc_engine_group_view_specs(&m); /* E5 */
    (void)append_specs(arr, n, cap, group_view, m);
    const pc_engine_command *layer_nav = pc_engine_layer_nav_specs(&m); /* E5 */
    (void)append_specs(arr, n, cap, layer_nav, m);
    const pc_engine_command *layer_label = pc_engine_layer_label_specs(&m); /* E5 */
    (void)append_specs(arr, n, cap, layer_label, m);
    const pc_engine_command *mask_view = pc_engine_mask_view_specs(&m); /* E5 */
    (void)append_specs(arr, n, cap, mask_view, m);
    const pc_engine_command *channel = pc_engine_channel_specs(&m); /* E7 */
    (void)append_specs(arr, n, cap, channel, m);
    const pc_engine_command *selection = pc_engine_selection_specs(&m); /* E7 */
    (void)append_specs(arr, n, cap, selection, m);
    const pc_engine_command *magnetic = pc_engine_magnetic_specs(&m); /* E7 */
    (void)append_specs(arr, n, cap, magnetic, m);
    const pc_engine_command *select_extra = pc_engine_select_extra_specs(&m); /* E7 */
    (void)append_specs(arr, n, cap, select_extra, m);
    const pc_engine_command *smartselect = pc_engine_smartselect_specs(&m); /* E7 */
    (void)append_specs(arr, n, cap, smartselect, m);
    const pc_engine_command *color = pc_engine_color_specs(&m); /* E8 */
    (void)append_specs(arr, n, cap, color, m);
    const pc_engine_command *trap = pc_engine_trap_specs(&m); /* E9 */
    (void)append_specs(arr, n, cap, trap, m);
    const pc_engine_command *fx_view = pc_engine_fx_view_specs(&m); /* E9 */
    (void)append_specs(arr, n, cap, fx_view, m);
    const pc_engine_command *render = pc_engine_render_specs(&m); /* E9 */
    (void)append_specs(arr, n, cap, render, m);
    const pc_engine_command *filters = pc_engine_filters_specs(&m); /* E9 */
    (void)append_specs(arr, n, cap, filters, m);
    const pc_engine_command *filters_ext = pc_engine_filters_ext_specs(&m); /* E-tail EF2 */
    (void)append_specs(arr, n, cap, filters_ext, m);
    const pc_engine_command *brush = pc_engine_brush_specs(&m); /* E10 */
    (void)append_specs(arr, n, cap, brush, m);
    const pc_engine_command *symmetry = pc_engine_symmetry_specs(&m); /* E10 */
    (void)append_specs(arr, n, cap, symmetry, m);
    const pc_engine_command *brush_key = pc_engine_brush_key_specs(&m); /* E10 */
    (void)append_specs(arr, n, cap, brush_key, m);
    const pc_engine_command *redeye = pc_engine_redeye_specs(&m); /* E10 */
    (void)append_specs(arr, n, cap, redeye, m);
    const pc_engine_command *pick = pc_engine_pick_specs(&m); /* E10 */
    (void)append_specs(arr, n, cap, pick, m);
    const pc_engine_command *vector = pc_engine_vector_specs(&m); /* E11 */
    (void)append_specs(arr, n, cap, vector, m);
    const pc_engine_command *layer_style = pc_engine_layer_style_specs(&m); /* E6 */
    (void)append_specs(arr, n, cap, layer_style, m);
    const pc_engine_command *fill = pc_engine_fill_specs(&m); /* E4: edit.fill */
    (void)append_specs(arr, n, cap, fill, m);
    const pc_engine_command *fill_key = pc_engine_fill_key_specs(&m); /* E4: the fill keys */
    (void)append_specs(arr, n, cap, fill_key, m);
    const pc_engine_command *selrect = pc_engine_select_rect_specs(&m); /* E4: select.rect */
    (void)append_specs(arr, n, cap, selrect, m);
    const pc_engine_command *efloat = pc_engine_float_specs(&m); /* E4: select.float/drop */
    (void)append_specs(arr, n, cap, efloat, m);
    const pc_engine_command *stamp = pc_engine_stamp_specs(&m); /* E4: stamp visible/down */
    (void)append_specs(arr, n, cap, stamp, m);
    const pc_engine_command *new_layer = pc_engine_new_layer_specs(&m); /* E5-base: layer.new.layer */
    (void)append_specs(arr, n, cap, new_layer, m);
    const pc_engine_command *layer_base = pc_engine_layer_base_specs(&m); /* E5-base: new.group/dup/… */
    (void)append_specs(arr, n, cap, layer_base, m);
    const pc_engine_command *clip = pc_engine_clip_specs(&m); /* E5-base: clipping mask + arrange */
    (void)append_specs(arr, n, cap, clip, m);
    const pc_engine_command *type = pc_engine_type_specs(&m); /* E11: type.* */
    (void)append_specs(arr, n, cap, type, m);
    const pc_engine_command *type_extra = pc_engine_type_extra_specs(&m); /* E11: Type menu */
    (void)append_specs(arr, n, cap, type_extra, m);
    const pc_engine_command *type_caret = pc_engine_type_caret_specs(&m); /* E11: caret queries */
    (void)append_specs(arr, n, cap, type_caret, m);
    const pc_engine_command *type_spell = pc_engine_type_spell_specs(&m); /* E11: spelling + glyph */
    (void)append_specs(arr, n, cap, type_spell, m);
    const pc_engine_command *type_styles = pc_engine_type_styles_specs(&m); /* E11: type styles */
    (void)append_specs(arr, n, cap, type_styles, m);
    const pc_engine_command *path_edit = pc_engine_path_edit_specs(&m); /* E11: point path edits */
    (void)append_specs(arr, n, cap, path_edit, m);
    const pc_engine_command *edit = pc_engine_edit_specs(&m); /* E4: clipboard + clear + via/merge */
    (void)append_specs(arr, n, cap, edit, m);
    const pc_engine_command *transform = pc_engine_transform_specs(&m); /* E4/ET1: Free Transform + presets */
    (void)append_specs(arr, n, cap, transform, m);
    const pc_engine_command *adjust = pc_engine_adjust_specs(&m); /* E8: Image > Adjustments + adj layers */
    (void)append_specs(arr, n, cap, adjust, m);
    const pc_engine_command *analysis = pc_engine_analysis_specs(&m); /* E8: Image > Analysis */
    (void)append_specs(arr, n, cap, analysis, m);
    const pc_engine_command *align = pc_engine_align_specs(&m); /* E9: Auto-Align/Auto-Blend Layers */
    (void)append_specs(arr, n, cap, align, m);
    const pc_engine_command *clone_src = pc_engine_clone_source_specs(&m); /* E2b: Clone Source panel */
    (void)append_specs(arr, n, cap, clone_src, m);
    const pc_engine_command *tool_preset = pc_engine_tool_preset_specs(&m); /* E2b: Tool Presets panel */
    (void)append_specs(arr, n, cap, tool_preset, m);
    const pc_engine_command *swatch = pc_engine_swatch_specs(&m); /* E2b: Swatches panel */
    (void)append_specs(arr, n, cap, swatch, m);
    const pc_engine_command *grad_preset = pc_engine_gradient_preset_specs(&m); /* E2b: Gradients panel */
    (void)append_specs(arr, n, cap, grad_preset, m);
    const pc_engine_command *gallery = pc_engine_gallery_specs(&m); /* E9: Filter Gallery */
    (void)append_specs(arr, n, cap, gallery, m);
    const pc_engine_command *smart = pc_engine_smart_specs(&m); /* E6: smart objects + smart filters */
    (void)append_specs(arr, n, cap, smart, m);
    const pc_engine_command *artboard = pc_engine_artboard_specs(&m); /* E6: artboard guides */
    (void)append_specs(arr, n, cap, artboard, m);
    const pc_engine_command *pattern = pc_engine_pattern_specs(&m); /* E10: patterns + Patterns panel */
    (void)append_specs(arr, n, cap, pattern, m);
    const pc_engine_command *paint = pc_engine_paint_specs(&m); /* E10: Paint Bucket + Gradient tool */
    (void)append_specs(arr, n, cap, paint, m);
    const pc_engine_command *eraser = pc_engine_eraser_specs(&m); /* E10: Magic / Background Eraser */
    (void)append_specs(arr, n, cap, eraser, m);
    const pc_engine_command *gradfill = pc_engine_gradient_fill_specs(&m); /* E10: live Gradient Fill + apply */
    (void)append_specs(arr, n, cap, gradfill, m);
    const pc_engine_command *retouch = pc_engine_retouch_specs(&m); /* E10-retouch: Clone/Heal/Dodge/… */
    (void)append_specs(arr, n, cap, retouch, m);
    const pc_engine_command *extra = pc_engine_extra_specs(&m); /* E12: extra_cmds grab-bag */
    (void)append_specs(arr, n, cap, extra, m);
    const pc_engine_command *notes = pc_engine_notes_specs(&m); /* E12: Note tool annotations */
    (void)append_specs(arr, n, cap, notes, m);
    const pc_engine_command *frame = pc_engine_frame_specs(&m); /* E12: Frame tool / Frame from Layers */
    (void)append_specs(arr, n, cap, frame, m);
    const pc_engine_command *video = pc_engine_video_specs(&m); /* E12: Video Layers (frame stack) */
    (void)append_specs(arr, n, cap, video, m);
    const pc_engine_command *timeline = pc_engine_timeline_specs(&m); /* E12: Timeline playhead */
    (void)append_specs(arr, n, cap, timeline, m);
    const pc_engine_command *comps = pc_engine_comps_specs(&m); /* E12: Layer Comps */
    (void)append_specs(arr, n, cap, comps, m);
    const pc_engine_command *slice = pc_engine_slice_specs(&m); /* E12: Web slices */
    (void)append_specs(arr, n, cap, slice, m);
    const pc_engine_command *plugin = pc_engine_plugin_specs(&m); /* E12: WASM plug-ins */
    (void)append_specs(arr, n, cap, plugin, m);
    const pc_engine_command *variables = pc_engine_variables_specs(&m); /* E12: Variables / Data Sets */
    (void)append_specs(arr, n, cap, variables, m);
    const pc_engine_command *actions = pc_engine_actions_specs(&m); /* E12: Actions recorder */
    (void)append_specs(arr, n, cap, actions, m);
    const pc_engine_command *automate = pc_engine_automate_specs(&m); /* E12: Scripts (Browse, Events) */
    (void)append_specs(arr, n, cap, automate, m);
    const pc_engine_command *web = pc_engine_web_specs(&m); /* E12: Save for Web / Export / Image Assets */
    (void)append_specs(arr, n, cap, web, m);
    const pc_engine_command *tool_colors = pc_engine_tool_colors_specs(&m); /* ET2: fg/bg colours */
    (void)append_specs(arr, n, cap, tool_colors, m);
    const pc_engine_command *auto_adjust = pc_engine_auto_adjust_specs(&m); /* ET2: Auto Tone/Contrast/Color */
    (void)append_specs(arr, n, cap, auto_adjust, m);
    const pc_engine_command *layer_menu = pc_engine_layer_menu_specs(&m); /* ET2: Flatten/masks/stack mode */
    (void)append_specs(arr, n, cap, layer_menu, m);
    const pc_engine_command *edit_menu = pc_engine_edit_menu_specs(&m); /* ET2: Edit > Purge */
    (void)append_specs(arr, n, cap, edit_menu, m);
    const pc_engine_command *color_to_alpha = pc_engine_color_to_alpha_specs(&m); /* ET2: Color to Alpha */
    (void)append_specs(arr, n, cap, color_to_alpha, m);
    const pc_engine_command *photo_automate = pc_engine_photo_automate_specs(&m); /* ET3: File > Automate */
    (void)append_specs(arr, n, cap, photo_automate, m);
    const pc_engine_command *sampler = pc_engine_sampler_specs(&m); /* EF: Color Sampler points */
    (void)append_specs(arr, n, cap, sampler, m);
    const pc_engine_command *lens = pc_engine_lens_specs(&m); /* ET5: Lens Correction / Wide Angle / Camera Raw */
    (void)append_specs(arr, n, cap, lens, m);
    const pc_engine_command *batch = pc_engine_batch_specs(&m); /* E-tail BATCH: file batch/automate pipeline */
    (void)append_specs(arr, n, cap, batch, m);
    const pc_engine_command *style_preset = pc_engine_style_preset_specs(&m); /* E2b: Styles panel */
    (void)append_specs(arr, n, cap, style_preset, m);
    const pc_engine_command *shape_preset = pc_engine_shape_preset_specs(&m); /* E2b: Shapes panel */
    (void)append_specs(arr, n, cap, shape_preset, m);
    const pc_engine_command *align_distribute = pc_engine_align_distribute_specs(&m); /* GAP: align/distribute */
    (void)append_specs(arr, n, cap, align_distribute, m);
    const pc_engine_command *view_proof_guide = pc_engine_view_proof_guide_specs(&m); /* GAP: proofSetup + guides */
    (void)append_specs(arr, n, cap, view_proof_guide, m);
    /* E3..E12: append your module's specs() here. */
}

static void build_registry(void)
{
    size_t cap = 0;
    if (!append_specs(&g_specs, &g_nspecs, &cap, BASE, sizeof BASE / sizeof BASE[0])) {
        g_specs = NULL;
        g_nspecs = 0;
        return;
    }
    register_modules(&g_specs, &g_nspecs, &cap);
}

const pc_engine_command *pc_engine_commands(size_t *n)
{
    (void)pthread_once(&g_once, build_registry);
    *n = g_nspecs;
    return g_specs;
}

const pc_engine_command *pc_engine_command_find(const char *id)
{
    size_t n;
    const pc_engine_command *all = pc_engine_commands(&n);
    for (size_t i = 0; i < n; i++) { /* first match wins (reference's linear scan) */
        if (strcmp(all[i].id, id) == 0) {
            return &all[i];
        }
    }
    return NULL;
}

bool pc_engine_session_disabled_reason(const pc_engine_session *s, const char *id, pc_error *reason)
{
    const pc_engine_command *spec = pc_engine_command_find(id);
    if (spec == NULL) {
        (void)pc_fail(reason, PC_ERR_NOT_FOUND, "unknown command `%s`", id);
        return true;
    }
    pc_error r = {0};
    if (spec->enabled(s, &r) != PC_OK) {
        if (reason != NULL) {
            *reason = r;
        }
        return true;
    }
    pc_error jc = {0};
    if (pc_engine_job_conflict(s, id, spec->journal, &jc)) {
        if (reason != NULL) {
            *reason = jc;
        }
        return true;
    }
    return false;
}

bool pc_engine_session_is_enabled(const pc_engine_session *s, const char *id)
{
    return !pc_engine_session_disabled_reason(s, id, NULL);
}

pc_status pc_engine_command_list_entry(const pc_engine_session *s, const pc_engine_command *c,
                                       const char *reason, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "id", c->id);
    yyjson_mut_obj_add_str(d, root, "label", c->label);
    yyjson_mut_val *menu = yyjson_mut_arr(d);
    for (size_t i = 0; i < c->menu_len; i++) {
        yyjson_mut_arr_add_str(d, menu, c->menu[i]);
    }
    yyjson_mut_obj_add_val(d, root, "menu", menu);
    if (c->shortcut != NULL) {
        yyjson_mut_obj_add_str(d, root, "shortcut", c->shortcut);
    } else {
        yyjson_mut_obj_add_null(d, root, "shortcut");
    }
    yyjson_mut_obj_add_str(d, root, "params", c->params);
    yyjson_mut_obj_add_bool(d, root, "enabled", reason == NULL);
    if (reason != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "disabledReason", reason);
    } else {
        yyjson_mut_obj_add_null(d, root, "disabledReason");
    }
    bool needs_document = false;
    if (pc_engine_session_active(s) == NULL && reason != NULL) {
        needs_document = strncmp(reason, "no document", 11) == 0 ||
                         strncmp(reason, "no active document", 18) == 0 ||
                         strncmp(reason, "no open document", 16) == 0;
    }
    yyjson_mut_obj_add_bool(d, root, "needsDocument", needs_document);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}
