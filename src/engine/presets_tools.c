/* Tool presets data model (port of crates/engine/src/presets/tools.rs data, E2b).
 *
 * A tool preset is a tool name plus opaque JSON options the shell fills in; the engine understands
 * only the "brush" and "foreground" keys. The commands live in tool_preset_cmds.c. Never crashes. */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "presets.h"

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

void pc_tool_preset_fini(pc_tool_preset *t)
{
    if (t == NULL) {
        return;
    }
    free(t->name);
    free(t->tool);
    pc_json_value_fini(&t->options);
    t->name = NULL;
    t->tool = NULL;
}

pc_status pc_tool_preset_clone(const pc_tool_preset *src, pc_tool_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->name = dup_str(src->name);
    out->tool = dup_str(src->tool);
    if (out->name == NULL || out->tool == NULL) {
        pc_tool_preset_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "tool preset");
    }
    if (pc_json_value_clone(&src->options, &out->options, err) != PC_OK) {
        pc_tool_preset_fini(out);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

/* Build a {"brush":{"size":size,"hardness":hardness}} options value (serde widens f32 -> f64). */
static pc_status brush_options(float size, float hardness, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "options");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *brush = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, brush, "size", (double)size);
    yyjson_mut_obj_add_real(d, brush, "hardness", (double)hardness);
    yyjson_mut_obj_add_val(d, root, "brush", brush);
    yyjson_mut_doc_set_root(d, root);
    pc_status rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(d);
    return rc;
}

/* {"toolOptions":{"gradient_style":"radial"}} for the Radial Gradient preset. */
static pc_status radial_options(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "options");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *to = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, to, "gradient_style", "radial");
    yyjson_mut_obj_add_val(d, root, "toolOptions", to);
    yyjson_mut_doc_set_root(d, root);
    pc_status rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(d);
    return rc;
}

static const char *tp_get_name(const void *item)
{
    return ((const pc_tool_preset *)item)->name;
}
static bool tp_set_name(void *item, const char *name)
{
    char *nm = dup_str(name);
    if (nm == NULL) {
        return false;
    }
    pc_tool_preset *t = item;
    free(t->name);
    t->name = nm;
    return true;
}
static void tp_fini(void *item)
{
    pc_tool_preset_fini(item);
}
static pc_status tp_clone(const void *src, void *dst, pc_error *err)
{
    return pc_tool_preset_clone(src, dst, err);
}

const pc_preset_item_ops *pc_tool_preset_ops(void)
{
    static const pc_preset_item_ops ops = {sizeof(pc_tool_preset), tp_get_name, tp_set_name, tp_fini, tp_clone};
    return &ops;
}

pc_status pc_tool_presets_builtin(pc_tool_preset **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    static const struct {
        const char *name;
        const char *tool;
        float size, hardness;
        bool radial;
    } defs[] = {
        {"Soft Round 100 px", "brush", 100.0f, 0.0f, false},
        {"Hard Round 12 px", "brush", 12.0f, 1.0f, false},
        {"Soft Eraser 60 px", "eraser", 60.0f, 0.0f, false},
        {"Radial Gradient", "gradient", 0.0f, 0.0f, true},
        {"Clone Soft 40 px", "cloneStamp", 40.0f, 0.2f, false},
    };
    size_t cnt = sizeof defs / sizeof defs[0];
    pc_tool_preset *v = calloc(cnt, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "tool presets");
    }
    for (size_t i = 0; i < cnt; i++) {
        v[i].name = dup_str(defs[i].name);
        v[i].tool = dup_str(defs[i].tool);
        if (v[i].name == NULL || v[i].tool == NULL) {
            goto oom;
        }
        pc_status rc = defs[i].radial ? radial_options(&v[i].options, err)
                                      : brush_options(defs[i].size, defs[i].hardness, &v[i].options, err);
        if (rc != PC_OK) {
            goto oom;
        }
    }
    *out = v;
    *n = cnt;
    return PC_OK;
oom:
    for (size_t i = 0; i < cnt; i++) {
        pc_tool_preset_fini(&v[i]);
    }
    free(v);
    return PC_ERR_NOMEM;
}
