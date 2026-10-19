/* Refuse painting on, or moving, a hidden layer (port of crates/engine/src/hidden_target.rs, #571).
 * The painting/retouch tools, Paint Bucket, Gradient and the Move tool stop with
 * "the target layer is hidden" instead of changing pixels nobody can see. A layer inside a hidden
 * group is hidden too. A channel/Quick Mask target has no layer, so it is never refused (that
 * check arrives with channel_cmds, E7). */
#include <string.h>

#include "engine_internal.h"
#include "channel_cmds.h"

static const char *const HIDDEN = "Could not complete your request because the target layer is hidden.";

/* Commands that paint on, or move, the target layer. */
static const char *const GATED[] = {
    "paint.stroke",        "paint.pencil",     "paint.mixerBrush",       "paint.colorReplacement",
    "paint.bucket",        "paint.gradient",   "paint.magicEraser",      "paint.backgroundEraser",
    "paint.cloneStamp",    "paint.healingBrush", "paint.spotHealing",    "paint.patch",
    "paint.dodge",         "paint.burn",       "paint.sponge",           "paint.blur",
    "paint.sharpen",       "paint.smudge",     "paint.historyBrush",     "layer.translate"};

/* Is the layer shown: it and every group around it visible. */
static bool shown(const pc_document *doc, pc_layer_id id)
{
    pc_layer_path path = {0};
    if (pc_document_path_of(doc, id, &path, NULL) != PC_OK) {
        return false;
    }
    bool ok = true;
    for (size_t n = 1; n <= path.len; n++) {
        const pc_layer *l = pc_document_layer_at(doc, path.idx, n);
        if (l == NULL || !l->visible) {
            ok = false;
            break;
        }
    }
    pc_layer_path_fini(&path);
    return ok;
}

const char *pc_engine_hidden_target_refusal(const pc_engine_session *s, const char *id, yyjson_val *params)
{
    if(pc_engine_is_channel_target(params)) { return NULL; }
    bool gated = false;
    for (size_t i = 0; i < sizeof GATED / sizeof GATED[0]; i++) {
        if (strcmp(id, GATED[i]) == 0) {
            gated = true;
            break;
        }
    }
    if (!gated) {
        return NULL;
    }
    /* A channel/Quick Mask target is never refused -- that check is added with channel_cmds (E7). */
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return NULL;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id layer;
    bool have_layer = false;
    yyjson_val *lp = pc_json_get(params, "layer");
    uint64_t lv;
    if (lp != NULL && pc_json_as_u64(lp, &lv)) {
        layer = lv;
        have_layer = true;
    } else if (st->has_active_layer) {
        layer = st->active_layer;
        have_layer = true;
    }
    if (!have_layer) {
        return NULL;
    }
    if (pc_document_layer(doc, layer) != NULL && !shown(doc, layer)) {
        return HIDDEN;
    }
    return NULL;
}
