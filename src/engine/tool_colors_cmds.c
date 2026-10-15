/* Foreground / background colour commands (port of the tools.setColors / tools.swapColors /
 * tools.defaultColors specs in crates/engine/src/commands.rs). These act on the app-wide
 * ToolState colours, not on any document, so they are always enabled. setColors can also record
 * the CMYK inks a colour was picked as, so a CMYK document fills with exactly those inks.
 *
 * Never crashes on any params (REWRITE_PLAN.md rule 9): a malformed CMYK array is rejected with a
 * BadParams error before anything changes. */
#include <math.h>
#include <stddef.h>

#include "base/json.h"

#include "engine_internal.h"

#define SET_ID "tools.setColors"

static pc_status en_always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* Read a [c, m, y, k] inks array under `key`: *present=false when the param is absent or null;
 * otherwise exactly four finite numbers, each clamped to 0..1. A malformed value is an error (so
 * the lenient path never quietly replaces it). */
static pc_status cmyk_param(yyjson_val *p, const char *key, bool *present, float out[4], pc_error *err)
{
    *present = false;
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 4) {
        return pc_engine_err_bad_params(err, SET_ID, "`%s` must be [c, m, y, k] numbers 0..1", key);
    }
    for (size_t i = 0; i < 4; i++) {
        double d;
        if (!pc_json_as_f64(yyjson_arr_get(v, i), &d) || !isfinite(d)) {
            return pc_engine_err_bad_params(err, SET_ID, "`%s` must be [c, m, y, k] numbers 0..1", key);
        }
        out[i] = (float)(d < 0.0 ? 0.0 : (d > 1.0 ? 1.0 : d));
    }
    *present = true;
    return PC_OK;
}

static pc_status run_set_colors(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    float fg_cmyk[4], bg_cmyk[4];
    bool has_fg_cmyk = false, has_bg_cmyk = false;
    if (cmyk_param(p, "foregroundCmyk", &has_fg_cmyk, fg_cmyk, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (cmyk_param(p, "backgroundCmyk", &has_bg_cmyk, bg_cmyk, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_engine_param_color(p, "foreground", s->tools.foreground, s->tools.foreground);
    pc_engine_param_color(p, "background", s->tools.background, s->tools.background);
    if (has_fg_cmyk) {
        float rgb[3] = {s->tools.foreground[0], s->tools.foreground[1], s->tools.foreground[2]};
        pc_engine_tool_state_remember_cmyk(&s->tools, rgb, fg_cmyk);
    }
    if (has_bg_cmyk) {
        float rgb[3] = {s->tools.background[0], s->tools.background[1], s->tools.background[2]};
        pc_engine_tool_state_remember_cmyk(&s->tools, rgb, bg_cmyk);
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_swap_colors(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    float tmp[4];
    for (int i = 0; i < 4; i++) {
        tmp[i] = s->tools.foreground[i];
        s->tools.foreground[i] = s->tools.background[i];
        s->tools.background[i] = tmp[i];
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_default_colors(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (s->tools.mask_targeted) {
        for (int i = 0; i < 4; i++) {
            s->tools.foreground[i] = PC_ENGINE_MASK_COLORS[0][i];
            s->tools.background[i] = PC_ENGINE_MASK_COLORS[1][i];
        }
    } else {
        s->tools.foreground[0] = 0.0f;
        s->tools.foreground[1] = 0.0f;
        s->tools.foreground[2] = 0.0f;
        s->tools.foreground[3] = 1.0f;
        s->tools.background[0] = 1.0f;
        s->tools.background[1] = 1.0f;
        s->tools.background[2] = 1.0f;
        s->tools.background[3] = 1.0f;
    }
    return pc_engine_result_null(out, err);
}

static const pc_engine_command SPECS[] = {
    {SET_ID, "Set Colors", NULL, 0, NULL,
     "{\"foreground\":\"#rrggbb\"|[r,g,b,a?]?,\"background\":same?,\"foregroundCmyk\":[c,m,y,k]? (inks "
     "0..1 the foreground was picked as: a CMYK document is filled with exactly these),"
     "\"backgroundCmyk\":same?}",
     en_always, run_set_colors, true},
    {"tools.swapColors", "Switch Foreground and Background Colors", NULL, 0, "X", "{}", en_always,
     run_swap_colors, true},
    {"tools.defaultColors", "Default Foreground and Background Colors", NULL, 0, "D", "{}", en_always,
     run_default_colors, true},
};

const pc_engine_command *pc_engine_tool_colors_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
