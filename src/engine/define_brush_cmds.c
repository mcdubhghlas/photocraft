/* Edit > Define Brush Preset: edit.defineBrushPreset (port of edit_menu_cmds::define_brush_preset).
 *
 * Defines a brush from the current selection, naming it (default "Sampled Brush N"), by delegating
 * to brush.defineFromSelection. Never crashes (REWRITE_PLAN.md rule 9): a missing selection is a
 * graceful error from the delegate. */
#include <stddef.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/str.h"
#include "paint/presets.h"

#include "define_brush_cmds.h"
#include "engine_internal.h"

#define CMD "edit.defineBrushPreset"

static const char *const MENU_EDIT[] = {"Edit"};

static pc_status db_enabled(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_is_enabled(s, "brush.defineFromSelection")) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "select pixels to define a brush from");
}

/* Trimmed copy of `name` into buf (returns false when it is empty after trimming). */
static bool trim_name(const char *name, size_t len, char *buf, size_t cap)
{
    size_t a = 0, b = len;
    while (a < b && (name[a] == ' ' || name[a] == '\t' || name[a] == '\n' || name[a] == '\r')) {
        a++;
    }
    while (b > a && (name[b - 1] == ' ' || name[b - 1] == '\t' || name[b - 1] == '\n' ||
                     name[b - 1] == '\r')) {
        b--;
    }
    if (b == a) {
        return false;
    }
    size_t n = b - a;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(buf, name + a, n);
    buf[n] = '\0';
    return true;
}

static pc_status run_define_brush(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    char name[256];
    size_t nlen = 0;
    const char *given = p != NULL ? pc_json_as_str(pc_json_get(p, "name"), &nlen) : NULL;
    if (given == NULL || !trim_name(given, nlen, name, sizeof name)) {
        /* Default "Sampled Brush N" (N = count of existing "Sampled Brush*" presets + 1). */
        size_t count = 0;
        for (size_t i = 0; i < s->tools.npresets; i++) {
            const char *pn = pc_str_c(&s->tools.presets[i].name);
            if (pn != NULL && strncmp(pn, "Sampled Brush", 13) == 0) {
                count++;
            }
        }
        (void)snprintf(name, sizeof name, "Sampled Brush %zu", count + 1);
    }

    /* Build {"name": name} and delegate to brush.defineFromSelection. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "define brush");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    if (!yyjson_mut_obj_add_strcpy(d, root, "name", name)) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "define brush");
    }
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = pc_json_write(root, 0, &buf, err);
    yyjson_mut_doc_free(d);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return rc;
    }
    rc = pc_engine_session_execute_json(s, "brush.defineFromSelection", (const char *)buf.data, out, err);
    pc_buf_fini(&buf);
    return rc;
}

static const pc_engine_command SPECS[] = {
    {CMD, "Define Brush Preset…", MENU_EDIT, 1, NULL, "{\"name\":text}", db_enabled, run_define_brush,
     true},
};

const pc_engine_command *pc_engine_define_brush_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
