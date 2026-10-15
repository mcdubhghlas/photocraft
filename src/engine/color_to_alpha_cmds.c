/* Filter > Other > Color to Alpha (filter.other.colorToAlpha; port of
 * crates/engine/src/color_to_alpha_cmds.rs): turns a chosen colour into transparency, unmixing it
 * from partly covered pixels so antialiased edges keep their shape over any new background.
 *
 * Photoshop has no such filter (GIMP and Krita do), so the id is our own; the menu item sits in
 * Filter > Other after Offset. It runs through the shared filter runner (pc_engine_run_filter)
 * like every other filter (selection, Last Filter, one history step), which turns a Background
 * layer into a normal layer first (filters_cmds.c handles the makes_transparency case). A layer
 * whose transparency is locked is refused rather than silently getting its old alpha back.
 *
 * Never crashes on any params (REWRITE_PLAN.md rule 9): validate() rejects anything the lenient
 * algo mapping (params_for in filters_cmds.c) would otherwise quietly replace. */
#include <math.h>
#include <stddef.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"

#include "channel_cmds.h"
#include "engine_internal.h"

#define ID "filter.other.colorToAlpha"

/* Validate a threshold key: absent is fine; otherwise a finite number in 0..1. */
static pc_status threshold(yyjson_val *p, const char *key, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return PC_OK;
    }
    double x;
    if (!pc_json_as_f64(v, &x)) {
        return pc_engine_err_bad_params(err, ID, "`%s` must be a number in 0..1", key);
    }
    if (!isfinite(x) || x < 0.0 || x > 1.0) {
        return pc_engine_err_bad_params(err, ID, "`%s` must be in 0..1", key);
    }
    return PC_OK;
}

/* Rejects params the lenient algo mapping would otherwise quietly replace. */
static pc_status validate(yyjson_val *p, pc_error *err)
{
    if (p == NULL || !yyjson_is_obj(p)) {
        return pc_engine_err_bad_params(err, ID, "params must be a JSON object");
    }
    yyjson_val *color = pc_json_get(p, "color");
    if (color != NULL) {
        size_t slen = 0;
        const char *s = pc_json_as_str(color, &slen);
        if (s != NULL) {
            float tmp[4];
            if (!pc_engine_parse_hex(s, slen, tmp)) {
                return pc_engine_err_bad_params(err, ID, "`color` must be \"#rrggbb\", not \"%s\"", s);
            }
        } else if (yyjson_is_arr(color)) {
            size_t len = yyjson_arr_size(color);
            if (len < 3 || len > 4) {
                return pc_engine_err_bad_params(err, ID,
                                                "`color` as an array must hold 3 or 4 numbers in 0..1");
            }
            for (size_t i = 0; i < len; i++) {
                double x;
                if (!pc_json_as_f64(yyjson_arr_get(color, i), &x) || !isfinite(x) || x < 0.0 || x > 1.0) {
                    return pc_engine_err_bad_params(
                        err, ID, "`color` as an array must hold 3 or 4 numbers in 0..1");
                }
            }
        } else {
            return pc_engine_err_bad_params(err, ID, "`color` must be \"#rrggbb\" or [r, g, b] in 0..1");
        }
    }
    pc_status rc = threshold(p, "transparencyThreshold", err);
    if (rc != PC_OK) {
        return rc;
    }
    return threshold(p, "opacityThreshold", err);
}

/* A pixel layer (or smart object) in an RGB or Grayscale document whose pixels and transparency
 * may change. The Background counts: it becomes a normal layer when run. */
static pc_status enabled(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_edits_channel(s)) {
        return pc_fail(reason, PC_ERR_INVALID,
                       "Color to Alpha works on layer pixels, not on a channel or Quick Mask");
    }
    pc_status rc = pc_engine_filterable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->mode != PC_COLOR_MODE_RGB && doc->mode != PC_COLOR_MODE_GRAYSCALE) {
        return pc_fail(reason, PC_ERR_INVALID,
                       "Color to Alpha needs an RGB or Grayscale document (this one is %s)",
                       pc_color_mode_name(doc->mode));
    }
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return reason != NULL && reason->code != PC_OK ? reason->code : PC_ERR_INVALID;
    }
    pc_locks locks = pc_document_effective_locks(doc, l->id);
    if (locks.pixels || locks.all) {
        return pc_fail(reason, PC_ERR_INVALID, "the layer \"%s\" is locked", pc_str_c(&l->name));
    }
    if (locks.transparency && !pc_document_is_background(doc, l->id)) {
        return pc_fail(reason, PC_ERR_INVALID, "the transparency of layer \"%s\" is locked",
                       pc_str_c(&l->name));
    }
    return PC_OK;
}

static pc_status run(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_status rc = validate(p, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_run_filter(s, ID, p, out, err);
}

static const char *const MENU_OTHER[] = {"Filter", "Other"};

static const pc_engine_command SPECS[] = {
    {ID, "Color to Alpha\xE2\x80\xA6", MENU_OTHER, 2, NULL,
     "{\"color\":color=#ffffff,\"transparencyThreshold\":0..1=0,\"opacityThreshold\":0..1=1}", enabled,
     run, true},
};

const pc_engine_command *pc_engine_color_to_alpha_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
