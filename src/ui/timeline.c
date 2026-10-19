#include "ui/timeline.h"
#include <string.h>
#include <math.h>
pc_status pc_ui_timeline_configure(pc_ui_timeline *state, yyjson_val *params, pc_error *err)
{
    if (!state || !yyjson_is_obj(params))
        return pc_fail(err, PC_ERR_INVALID, "timeline must be an object");
    pc_ui_timeline next = *state;
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(params, i, n, key, value)
    {
        const char *name = yyjson_get_str(key);
        if (!yyjson_is_bool(value) || (strcmp(name, "open") && strcmp(name, "playing")))
            return pc_fail(err, PC_ERR_INVALID, "timeline accepts open and playing booleans");
        if (!strcmp(name, "open"))
            next.open = yyjson_get_bool(value);
        else
            next.playing = yyjson_get_bool(value);
    }
    if (!next.open && !pc_json_get(params, "playing"))
        next.playing = false;
    if (next.playing && !next.open)
        return pc_fail(err, PC_ERR_INVALID, "open the Timeline before starting playback");
    if (next.playing != state->playing) {
        next.document = 0;
        next.last_time = 0;
    }
    *state = next;
    return PC_OK;
}
yyjson_mut_val *pc_ui_timeline_json(const pc_ui_timeline *state, yyjson_mut_doc *doc)
{
    yyjson_mut_val *out = doc ? yyjson_mut_obj(doc) : NULL;
    return state && out && yyjson_mut_obj_add_bool(doc, out, "open", state->open) &&
                   yyjson_mut_obj_add_bool(doc, out, "playing", state->playing)
               ? out
               : NULL;
}
bool pc_ui_timeline_step(pc_ui_timeline *state, pc_doc_id doc, const pc_timeline *t, double now,
                         size_t *frame)
{
    if (!state || !frame || !state->open || !state->playing || !isfinite(now) || now < 0)
        return false;
    if (!t || !t->duration) {
        state->playing = false;
        return false;
    }
    if (!state->document) {
        state->document = doc;
        state->last_time = now;
        return false;
    }
    if (state->document != doc) {
        state->playing = false;
        return false;
    }
    if (!isfinite(state->last_time) || now < state->last_time) {
        state->last_time = now;
        return false;
    }
    double fps = isfinite(t->fps) ? fmax(1, t->fps) : 1;
    if (now - state->last_time < 1000 / fps)
        return false;
    state->last_time = now;
    *frame = t->current >= t->duration - 1 ? 0 : t->current + 1;
    return *frame != t->current;
}
