/* Photoshop's brush keys (port of crates/engine/src/brush_key_cmds.rs, E10, #626): [ / ] step the
 * brush size, Shift+[ / Shift+] the hardness, and tools.rotateBrush turns the tip. Each one is a
 * tools.setBrush on the session brush, so it can be rebound, recorded and driven by agents. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "base/json.h"

#include "brush_cmds.h"
#include "engine_internal.h"

#define MAX_SIZE 5000.0f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* [ / ]: 25 % smaller or larger, at least a pixel either way (#3229). */
static float size_step(float size, bool up)
{
    float next;
    if (up) {
        float a = roundf(size * 1.25f), b = size + 1.0f;
        next = a > b ? a : b;
    } else {
        float a = roundf(size / 1.25f), b = size - 1.0f;
        next = a < b ? a : b;
    }
    return clampf(next, 1.0f, MAX_SIZE);
}

/* Shift+[ / Shift+]: to the previous or next quarter (0, 25, 50, 75, 100 %). */
static float hardness_step(float hardness, bool up)
{
    float quarter = roundf(hardness * 4.0f);
    float next = up ? quarter + 1.0f : quarter - 1.0f;
    return clampf(next, 0.0f, 4.0f) / 4.0f;
}

/* The brush angle turned by `degrees` (positive anticlockwise), wrapped into -180..=180. */
static float turned(float angle, float degrees)
{
    float a = fmodf(angle + degrees, 360.0f);
    if (a < 0.0f) {
        a += 360.0f;
    }
    return a > 180.0f ? a - 360.0f : a;
}

static pc_status set_brush_field(pc_engine_session *s, const char *field, double value,
                                 pc_json_value *out, pc_error *err)
{
    char json[128];
    (void)snprintf(json, sizeof json, "{\"brush\":{\"%s\":%.9g}}", field, value);
    yyjson_doc *doc = NULL;
    if (pc_json_parse(json, strlen(json), &doc, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_status rc = pc_engine_set_brush(s, yyjson_doc_get_root(doc), out, err);
    yyjson_doc_free(doc);
    return rc;
}

static pc_status run_dec_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_brush_field(s, "size", size_step(s->tools.brush.size, false), out, err);
}
static pc_status run_inc_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_brush_field(s, "size", size_step(s->tools.brush.size, true), out, err);
}
static pc_status run_dec_hard(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_brush_field(s, "hardness", hardness_step(s->tools.brush.hardness, false), out, err);
}
static pc_status run_inc_hard(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_brush_field(s, "hardness", hardness_step(s->tools.brush.hardness, true), out, err);
}

static pc_status run_rotate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    double by;
    yyjson_val *dv = pc_json_get(p, "degrees");
    if (dv == NULL || !pc_json_as_f64(dv, &by) || !isfinite(by) || fabs(by) > 360.0) {
        return pc_engine_err_bad_params(err, "tools.rotateBrush",
                                        "`degrees` must be a number from -360 to 360");
    }
    float angle = turned(s->tools.brush.angle, (float)by);
    return set_brush_field(s, "angle", angle, out, err);
}

static pc_status en_always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"tools.decreaseBrushSize", "Decrease Brush Size", NULL, 0, "[", "{}", en_always, run_dec_size, true},
    {"tools.increaseBrushSize", "Increase Brush Size", NULL, 0, "]", "{}", en_always, run_inc_size, true},
    {"tools.decreaseBrushHardness", "Decrease Brush Hardness", NULL, 0, "Shift+[", "{}", en_always,
     run_dec_hard, true},
    {"tools.increaseBrushHardness", "Increase Brush Hardness", NULL, 0, "Shift+]", "{}", en_always,
     run_inc_hard, true},
    {"tools.rotateBrush", "Rotate Brush Tip", NULL, 0, NULL,
     "{\"degrees\":-360..360 (positive anticlockwise; ← / → while painting send 1, ⇧ 15)}", en_always,
     run_rotate, true},
};

const pc_engine_command *pc_engine_brush_key_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
