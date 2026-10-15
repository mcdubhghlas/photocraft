/* Photoshop's fill keys (port of crates/engine/src/fill_key_cmds.rs, E4, #249): Alt+Backspace fills
 * with the foreground colour, Cmd+Backspace with the background, Shift added preserves transparency,
 * and Cmd+Alt+Backspace fills from the History Brush's source state. Each one is edit.fill with the
 * colour and Preserve Transparency fixed. On a shape layer the colour keys recolour the fill; on a
 * type layer they recolour the text (#4580); the layer stays its kind.
 *
 * Deviation (§7): the "channels panel targets the layer mask" guard of target_live_layer is omitted
 * (the C ChannelView exposes no such accessor yet, E7); the "target" param mask/quickMask/channel
 * guard is honoured. The type-layer recolour depends on type.setStyle, which is not registered yet
 * (E11 type commands); when it is absent the key reports it gracefully rather than recolouring. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"

#include "engine_internal.h"
#include "fill_cmds.h"

#define PARAMS "{\"layer\":id?,\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}?}"

/* The shape or type layer the colour fill keys recolour instead of filling pixels; PC_LAYER_RASTER
 * sentinel in *kind when there is none (target_live_layer). *present set when a layer was found. */
static bool target_live_layer(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out,
                              pc_layer_kind *kind)
{
    yyjson_val *t = pc_json_get(p, "target");
    if (t != NULL) {
        size_t len = 0;
        const char *ts = pc_json_as_str(t, &len);
        if (ts != NULL && ((len == 4 && memcmp(ts, "mask", 4) == 0) ||
                           (len == 9 && memcmp(ts, "quickMask", 9) == 0))) {
            return false;
        }
        if (yyjson_is_obj(t) && pc_json_get(t, "channel") != NULL) {
            return false;
        }
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return false;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id id;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL) {
        uint64_t u;
        if (!pc_json_as_u64(lv, &u)) {
            return false;
        }
        id = u;
    } else if (st->has_active_layer) {
        id = st->active_layer;
    } else {
        return false;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return false;
    }
    if (l->content.kind == PC_LAYER_SHAPE || l->content.kind == PC_LAYER_TEXT) {
        *out = id;
        *kind = l->content.kind;
        return true;
    }
    return false;
}

static pc_status fill_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_command *edit_fill = pc_engine_command_find("edit.fill");
    if (edit_fill == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "Fill is unavailable");
    }
    return edit_fill->enabled(s, reason);
}

static pc_status en_color(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id id;
    pc_layer_kind kind;
    if (target_live_layer(s, NULL, &id, &kind)) {
        return PC_OK;
    }
    return fill_enabled(s, reason);
}

/* A copy of p (an object; Null -> empty) as a mutable object, or NULL on OOM / wrong type. */
static yyjson_mut_doc *copy_params(yyjson_val *p, bool *bad)
{
    *bad = false;
    if (p != NULL && !yyjson_is_obj(p) && !yyjson_is_null(p)) {
        *bad = true;
        return NULL;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return NULL;
    }
    yyjson_mut_val *root;
    if (p != NULL && yyjson_is_obj(p)) {
        root = yyjson_val_mut_copy(d, p);
    } else {
        root = yyjson_mut_obj(d);
    }
    if (root == NULL) {
        yyjson_mut_doc_free(d);
        return NULL;
    }
    yyjson_mut_doc_set_root(d, root);
    return d;
}

/* Run edit.fill with the mutable params `d` (consumed: freed here); writes *out. */
static pc_status run_edit_fill(pc_engine_session *s, yyjson_mut_doc *d, pc_json_value *out, pc_error *err)
{
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    if (imm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "fill key");
    }
    pc_status rc = pc_engine_fill(s, yyjson_doc_get_root(imm), out, err);
    yyjson_doc_free(imm);
    return rc;
}

static void color_hex(const float c[4], char out[10])
{
    int r = (int)lroundf((c[0] < 0 ? 0 : c[0] > 1 ? 1 : c[0]) * 255.0f);
    int g = (int)lroundf((c[1] < 0 ? 0 : c[1] > 1 ? 1 : c[1]) * 255.0f);
    int b = (int)lroundf((c[2] < 0 ? 0 : c[2] > 1 ? 1 : c[2]) * 255.0f);
    if (c[3] < 1.0f) {
        int a = (int)lroundf((c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3]) * 255.0f);
        snprintf(out, 10, "#%02x%02x%02x%02x", r, g, b, a);
    } else {
        snprintf(out, 10, "#%02x%02x%02x", r, g, b);
    }
}

static pc_status recolor_live(pc_engine_session *s, pc_layer_id id, pc_layer_kind kind,
                              const float c[4], pc_json_value *out, pc_error *err)
{
    if (kind == PC_LAYER_TEXT) {
        /* type.setStyle doesn't check locks itself; refuse like the pixel fill does. */
        const pc_engine_doc_state *st = pc_engine_session_active(s);
        if (st == NULL) {
            return pc_engine_err_no_document(err);
        }
        const pc_document *doc = pc_doc_arc_get(st->doc);
        pc_locks locks = pc_document_effective_locks(doc, id);
        if (locks.pixels || locks.all) {
            const pc_layer *l = pc_document_layer(doc, id);
            return pc_engine_err_other(err,
                                       "Could not complete your request because the layer \"%s\" is locked",
                                       l != NULL ? pc_str_c(&l->name) : "layer");
        }
        const pc_engine_command *set_style = pc_engine_command_find("type.setStyle");
        if (set_style == NULL) {
            return pc_engine_err_other(err, "type.setStyle is unavailable");
        }
        char json[96];
        snprintf(json, sizeof json, "{\"layer\":%llu,\"color\":[%.9g,%.9g,%.9g,1.0]}",
                 (unsigned long long)id, (double)c[0], (double)c[1], (double)c[2]);
        yyjson_doc *jd = NULL;
        if (pc_json_parse(json, strlen(json), &jd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        pc_status rc = set_style->run(s, yyjson_doc_get_root(jd), out, err);
        yyjson_doc_free(jd);
        return rc;
    }
    const pc_engine_command *shape_edit = pc_engine_command_find("shape.edit");
    if (shape_edit == NULL) {
        return pc_engine_err_other(err, "shape.edit is unavailable");
    }
    char hex[10];
    color_hex(c, hex);
    char json[64];
    snprintf(json, sizeof json, "{\"layer\":%llu,\"fill\":\"%s\"}", (unsigned long long)id, hex);
    yyjson_doc *jd = NULL;
    if (pc_json_parse(json, strlen(json), &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_status rc = shape_edit->run(s, yyjson_doc_get_root(jd), out, err);
    yyjson_doc_free(jd);
    return rc;
}

static pc_status fill_color_key(pc_engine_session *s, yyjson_val *p, const char *cmd, bool background,
                                bool preserve, pc_json_value *out, pc_error *err)
{
    bool bad = false;
    yyjson_mut_doc *d = copy_params(p, &bad);
    if (bad) {
        return pc_engine_err_bad_params(err, cmd, "expected an object");
    }
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "fill key");
    }
    const float *c = background ? pc_engine_session_tools(s)->background : pc_engine_session_tools(s)->foreground;

    pc_layer_id id;
    pc_layer_kind kind;
    if (target_live_layer(s, p, &id, &kind)) {
        yyjson_mut_doc_free(d);
        float col[4] = {c[0], c[1], c[2], 1.0f};
        return recolor_live(s, id, kind, col, out, err);
    }

    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    const char *drop[] = {"color", "contents", "pattern", "preserveTransparency", "opacity"};
    for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++) {
        yyjson_mut_obj_remove_key(root, drop[i]);
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, arr, (double)c[0]);
    yyjson_mut_arr_add_real(d, arr, (double)c[1]);
    yyjson_mut_arr_add_real(d, arr, (double)c[2]);
    yyjson_mut_arr_add_real(d, arr, 1.0);
    yyjson_mut_obj_add_val(d, root, "color", arr);
    yyjson_mut_obj_add_bool(d, root, "preserveTransparency", preserve);
    return run_edit_fill(s, d, out, err);
}

static pc_status fill_history(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool bad = false;
    yyjson_mut_doc *d = copy_params(p, &bad);
    if (bad) {
        return pc_engine_err_bad_params(err, "edit.fillHistory", "expected an object");
    }
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "fill key");
    }
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d);
    const char *drop[] = {"color", "contents", "pattern"};
    for (size_t i = 0; i < sizeof drop / sizeof drop[0]; i++) {
        yyjson_mut_obj_remove_key(root, drop[i]);
    }
    yyjson_mut_obj_add_strcpy(d, root, "contents", "history");
    return run_edit_fill(s, d, out, err);
}

static pc_status run_fg(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return fill_color_key(s, p, "edit.fillForeground", false, false, out, err);
}
static pc_status run_bg(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return fill_color_key(s, p, "edit.fillBackground", true, false, out, err);
}
static pc_status run_fg_pr(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return fill_color_key(s, p, "edit.fillForegroundPreserve", false, true, out, err);
}
static pc_status run_bg_pr(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return fill_color_key(s, p, "edit.fillBackgroundPreserve", true, true, out, err);
}

static const pc_engine_command SPECS[] = {
    {"edit.fillForeground", "Fill with Foreground Color", NULL, 0, "Alt+Backspace", PARAMS, en_color, run_fg,
     true},
    {"edit.fillBackground", "Fill with Background Color", NULL, 0, "Cmd+Backspace", PARAMS, en_color, run_bg,
     true},
    {"edit.fillForegroundPreserve", "Fill with Foreground Color, Preserve Transparency", NULL, 0,
     "Alt+Shift+Backspace", PARAMS, en_color, run_fg_pr, true},
    {"edit.fillBackgroundPreserve", "Fill with Background Color, Preserve Transparency", NULL, 0,
     "Cmd+Shift+Backspace", PARAMS, en_color, run_bg_pr, true},
    {"edit.fillHistory", "Fill from History", NULL, 0, "Cmd+Alt+Backspace",
     "{\"state\":n? (default: the History Brush's source, the oldest state still held),\"layer\":id?,"
     "\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}?,\"preserveTransparency\":bool?}",
     fill_enabled, fill_history, true},
};

const pc_engine_command *pc_engine_fill_key_specs(size_t *n)
{
    /* Fill in the shared PARAMS for the colour keys (static table needs a const expression). */
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
