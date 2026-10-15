/* Window > Tool Presets (port of crates/engine/src/presets/tools.rs commands, E2b).
 *
 * A tool preset is a tool name plus opaque options JSON; the engine understands "brush" (applied
 * like tools.setBrush) and "foreground" (Include Color). Never crashes. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "brush_cmds.h"
#include "engine_internal.h"
#include "presets.h"

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

/* Normalise a tool name: lowercase, drop spaces/_/-, trim a trailing "tool". */
static void norm(const char *t, char out[64])
{
    size_t w = 0;
    for (size_t i = 0; t[i] != '\0' && w < 63; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == ' ' || c == '_' || c == '-') {
            continue;
        }
        out[w++] = (char)tolower(c);
    }
    out[w] = '\0';
    if (w >= 4 && strcmp(out + w - 4, "tool") == 0) {
        out[w - 4] = '\0';
    }
}

static bool is_brushy(const char *n)
{
    static const char *const b[] = {"brush",  "pencil",   "eraser",     "clonestamp", "healing",
                                    "spothealing", "historybrush", "blur",   "sharpen",    "smudge",
                                    "dodge",  "burn",     "sponge",     "mixerbrush", "colorreplacement"};
    for (size_t i = 0; i < sizeof b / sizeof b[0]; i++) {
        if (strcmp(n, b[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Trim + validate a preset name into out[cap]; false (err set) on empty/too long. */
static bool checked_name(const char *s, size_t len, const char *cmd, char *out, size_t cap, pc_error *err)
{
    size_t a = 0, b = len;
    while (a < b && isspace((unsigned char)s[a])) {
        a++;
    }
    while (b > a && isspace((unsigned char)s[b - 1])) {
        b--;
    }
    if (b == a) {
        (void)pc_engine_err_bad_params(err, cmd, "the preset name is empty");
        return false;
    }
    /* count codepoints */
    size_t chars = 0;
    for (size_t i = a; i < b;) {
        unsigned char c = (unsigned char)s[i];
        i += c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
        chars++;
    }
    if (chars > PC_PRESET_MAX_NAME) {
        (void)pc_engine_err_bad_params(err, cmd, "preset names are at most %d characters", PC_PRESET_MAX_NAME);
        return false;
    }
    size_t n = b - a;
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, s + a, n);
    out[n] = '\0';
    return true;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    char filt[64];
    bool have_filt = false;
    size_t tl = 0;
    const char *t = pc_json_as_str(pc_json_get(p, "tool"), &tl);
    if (t != NULL) {
        char tmp[256];
        size_t n = tl < sizeof tmp - 1 ? tl : sizeof tmp - 1;
        memcpy(tmp, t, n);
        tmp[n] = '\0';
        norm(tmp, filt);
        have_filt = true;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->presets.ntool_presets; i++) {
        const pc_tool_preset *tp = &s->presets.tool_presets[i];
        if (have_filt) {
            char nb[64];
            norm(tp->tool, nb);
            if (strcmp(nb, filt) != 0) {
                continue;
            }
        }
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "name", tp->name);
        yyjson_mut_obj_add_strcpy(d, o, "tool", tp->tool);
        yyjson_mut_arr_append(arr, o);
    }
    yyjson_mut_obj_add_val(d, root, "presets", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* A read-only synthetic one-group view over the tool presets, for unique_name scanning. */
static void view_groups(pc_engine_session *s, pc_preset_group *grp, pc_preset_groups *g, char *nmbuf)
{
    nmbuf[0] = '\0';
    grp->name = nmbuf;
    grp->items = s->presets.tool_presets;
    grp->n = s->presets.ntool_presets;
    grp->cap = s->presets.cap_tool_presets;
    g->v = grp;
    g->n = 1;
    g->cap = 1;
}

static pc_status run_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "tool.presets.new";
    size_t tl = 0;
    const char *tool = pc_json_as_str(pc_json_get(p, "tool"), &tl);
    if (tool == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `tool`");
    }
    char toolbuf[256];
    {
        size_t n = tl < sizeof toolbuf - 1 ? tl : sizeof toolbuf - 1;
        memcpy(toolbuf, tool, n);
        toolbuf[n] = '\0';
    }
    /* Build the options object in a fresh mutable doc. */
    yyjson_mut_doc *od = yyjson_mut_doc_new(NULL);
    if (od == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "options");
    }
    yyjson_mut_val *opt;
    yyjson_val *oin = pc_json_get(p, "options");
    if (oin == NULL || yyjson_is_null(oin)) {
        opt = yyjson_mut_obj(od);
    } else if (yyjson_is_obj(oin)) {
        opt = yyjson_val_mut_copy(od, oin);
    } else {
        yyjson_mut_doc_free(od);
        return pc_engine_err_bad_params(err, cmd, "`options` must be an object");
    }
    yyjson_mut_doc_set_root(od, opt);
    char nb[64];
    norm(toolbuf, nb);
    if (yyjson_mut_obj_get(opt, "brush") == NULL && is_brushy(nb)) {
        yyjson_mut_val *bj = pc_engine_brush_json(od, &s->tools.brush);
        if (bj != NULL) {
            yyjson_mut_obj_add_val(od, opt, "brush", bj);
        }
    }
    bool inc;
    if (pc_json_as_bool(pc_json_get(p, "includeColor"), &inc) && inc) {
        yyjson_mut_val *fg = yyjson_mut_arr(od);
        for (int i = 0; i < 4; i++) {
            yyjson_mut_arr_add_real(od, fg, (double)s->tools.foreground[i]);
        }
        yyjson_mut_obj_put(opt, yyjson_mut_strcpy(od, "foreground"), fg);
    }
    if (s->presets.ntool_presets >= PC_TOOL_PRESETS_MAX) {
        yyjson_mut_doc_free(od);
        return pc_engine_err_bad_params(err, cmd,
                                        "at most %d tool presets can be saved; delete some first",
                                        PC_TOOL_PRESETS_MAX);
    }
    char base[512];
    yyjson_val *nv = pc_json_get(p, "name");
    if (nv == NULL || yyjson_is_null(nv)) {
        (void)snprintf(base, sizeof base, "%s", toolbuf);
    } else {
        size_t nl = 0;
        const char *ns = pc_json_as_str(nv, &nl);
        if (ns == NULL) {
            yyjson_mut_doc_free(od);
            return pc_engine_err_bad_params(err, cmd, "`name` must be a string");
        }
        /* empty/whitespace name -> the tool name */
        size_t a = 0, b = nl;
        while (a < b && isspace((unsigned char)ns[a])) {
            a++;
        }
        if (a == b) {
            (void)snprintf(base, sizeof base, "%s", toolbuf);
        } else if (!checked_name(ns, nl, cmd, base, sizeof base, err)) {
            yyjson_mut_doc_free(od);
            return PC_ERR_INVALID;
        }
    }
    pc_preset_group grp;
    pc_preset_groups view;
    char nmbuf[1];
    view_groups(s, &grp, &view, nmbuf);
    char *name = pc_preset_unique_name(&view, base, pc_tool_preset_ops());
    if (name == NULL) {
        yyjson_mut_doc_free(od);
        return pc_fail(err, PC_ERR_NOMEM, "name");
    }
    pc_tool_preset tp;
    memset(&tp, 0, sizeof tp);
    tp.name = name;
    tp.tool = dup_str(toolbuf);
    pc_status rc = tp.tool == NULL ? pc_fail(err, PC_ERR_NOMEM, "tool")
                                   : pc_json_value_from_mut(opt, &tp.options, err);
    yyjson_mut_doc_free(od);
    if (rc != PC_OK) {
        pc_tool_preset_fini(&tp);
        return PC_ERR_NOMEM;
    }
    /* grow the tool_presets array */
    if (s->presets.ntool_presets == s->presets.cap_tool_presets) {
        size_t nc = s->presets.cap_tool_presets == 0 ? 8 : s->presets.cap_tool_presets * 2;
        pc_tool_preset *v = realloc(s->presets.tool_presets, nc * sizeof *v);
        if (v == NULL) {
            pc_tool_preset_fini(&tp);
            return pc_fail(err, PC_ERR_NOMEM, "tool presets");
        }
        s->presets.tool_presets = v;
        s->presets.cap_tool_presets = nc;
    }
    s->presets.tool_presets[s->presets.ntool_presets++] = tp;
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", tp.name);
    yyjson_mut_obj_add_strcpy(d, root, "tool", tp.tool);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_select(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "tool.presets.select";
    size_t nl = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "preset"), &nl);
    if (name == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `preset`");
    }
    char nbuf[512];
    size_t n = nl < sizeof nbuf - 1 ? nl : sizeof nbuf - 1;
    memcpy(nbuf, name, n);
    nbuf[n] = '\0';
    const pc_tool_preset *tp = NULL;
    for (size_t i = 0; i < s->presets.ntool_presets; i++) {
        if (strcmp(s->presets.tool_presets[i].name, nbuf) == 0) {
            tp = &s->presets.tool_presets[i];
            break;
        }
    }
    if (tp == NULL) {
        return pc_engine_err_bad_params(err, cmd, "no tool preset \"%s\" (see tool.presets.list)", nbuf);
    }
    /* Read the stored options through an immutable copy: the analyzer models immutable yyjson
     * traversal cleanly (unlike walking an externally-derived mutable array). */
    yyjson_doc *imm = tp->options.doc != NULL ? yyjson_mut_doc_imut_copy(tp->options.doc, NULL) : NULL;
    yyjson_val *opt = imm != NULL ? yyjson_doc_get_root(imm) : NULL;
    yyjson_val *brush = opt != NULL && yyjson_is_obj(opt) ? yyjson_obj_get(opt, "brush") : NULL;
    if (brush != NULL && yyjson_is_obj(brush)) {
        yyjson_mut_doc *bd = yyjson_mut_doc_new(NULL);
        if (bd == NULL) {
            yyjson_doc_free(imm);
            return pc_fail(err, PC_ERR_NOMEM, "brush");
        }
        yyjson_mut_val *br = yyjson_mut_obj(bd);
        yyjson_mut_obj_add_val(bd, br, "brush", yyjson_val_mut_copy(bd, brush));
        yyjson_mut_doc_set_root(bd, br);
        yyjson_doc *bimm = yyjson_mut_doc_imut_copy(bd, NULL);
        yyjson_mut_doc_free(bd);
        if (bimm == NULL) {
            yyjson_doc_free(imm);
            return pc_fail(err, PC_ERR_NOMEM, "brush");
        }
        pc_json_value scratch = PC_JSON_VALUE_INIT;
        pc_status rc = pc_engine_set_brush(s, yyjson_doc_get_root(bimm), &scratch, err);
        pc_json_value_fini(&scratch);
        yyjson_doc_free(bimm);
        if (rc != PC_OK) {
            yyjson_doc_free(imm);
            return rc;
        }
    }
    yyjson_val *fg = opt != NULL && yyjson_is_obj(opt) ? yyjson_obj_get(opt, "foreground") : NULL;
    if (fg != NULL && yyjson_is_arr(fg) && yyjson_arr_size(fg) >= 3) {
        float c[4] = {0, 0, 0, 1.0f};
        size_t i, m;
        yyjson_val *el;
        yyjson_arr_foreach(fg, i, m, el) {
            if (i >= 4) {
                break;
            }
            if (yyjson_is_num(el)) {
                c[i] = (float)yyjson_get_real(el);
            }
        }
        s->tools.foreground[0] = c[0];
        s->tools.foreground[1] = c[1];
        s->tools.foreground[2] = c[2];
        s->tools.foreground[3] = c[3];
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        yyjson_doc_free(imm);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", tp->name);
    yyjson_mut_obj_add_strcpy(d, root, "tool", tp->tool);
    yyjson_mut_val *optcopy = opt != NULL ? yyjson_val_mut_copy(d, opt) : yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "options", optcopy);
    yyjson_mut_doc_set_root(d, root);
    yyjson_doc_free(imm);
    return PC_OK;
}

static pc_status run_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "tool.presets.edit";
    size_t al = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &al);
    if (action == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `action`");
    }
    bool is_rename = al == 6 && memcmp(action, "rename", 6) == 0;
    bool is_delete = al == 6 && memcmp(action, "delete", 6) == 0;
    if (!is_rename && !is_delete) {
        return pc_engine_err_bad_params(err, cmd, "tool presets support rename and delete");
    }
    if (is_rename) {
        size_t nl = 0;
        const char *ns = pc_json_as_str(pc_json_get(p, "name"), &nl);
        char tmp[512];
        if (ns == NULL || !checked_name(ns, nl, cmd, tmp, sizeof tmp, err)) {
            return err != NULL && err->code != PC_OK ? err->code
                                                      : pc_engine_err_bad_params(err, cmd, "missing `name`");
        }
    }
    /* wrap the flat preset list into one synthetic group for the shared group-edit logic. */
    char *gname = dup_str("");
    if (gname == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "edit");
    }
    pc_preset_group grp = {gname, s->presets.tool_presets, s->presets.ntool_presets, s->presets.cap_tool_presets};
    pc_preset_groups g = {&grp, 1, 1};
    pc_status rc = pc_preset_edit_groups(&g, is_rename ? "rename" : "delete", p, cmd,
                                         pc_tool_preset_ops(), out, err);
    s->presets.tool_presets = grp.items;
    s->presets.ntool_presets = grp.n;
    s->presets.cap_tool_presets = grp.cap;
    free(grp.name);
    if (rc == PC_OK) {
        pc_engine_presets_changed(s);
    }
    return rc;
}

static pc_status run_reset(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_tool_preset *v = NULL;
    size_t n = 0;
    if (pc_tool_presets_builtin(&v, &n, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    for (size_t i = 0; i < s->presets.ntool_presets; i++) {
        pc_tool_preset_fini(&s->presets.tool_presets[i]);
    }
    free(s->presets.tool_presets);
    s->presets.tool_presets = v;
    s->presets.ntool_presets = n;
    s->presets.cap_tool_presets = n;
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "presets", n);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"tool.presets.list", "Tool Presets", NULL, 0, NULL,
     "{\"tool\":name? (Current Tool Only)} -> {presets:[{name,tool}]}", pc_engine_always, run_list, false},
    {"tool.presets.new", "New Tool Preset...", NULL, 0, NULL,
     "{\"name\":str?=tool (at most 255 characters; a taken name gets \" 2\", \" 3\"...),\"tool\":name "
     "(\"crop\" for Crop presets),\"options\":{...}? (opaque; \"brush\" defaults to the current brush for "
     "painting tools),\"includeColor\":bool=false}",
     pc_engine_always, run_new, true},
    {"tool.presets.select", "Select Tool Preset", NULL, 0, NULL,
     "{\"preset\":name} -> {name,tool,options} (applies \"brush\" and \"foreground\"; the shell switches "
     "tool and options)",
     pc_engine_always, run_select, true},
    {"tool.presets.edit", "Edit Tool Presets", NULL, 0, NULL,
     "{\"action\":\"rename|delete\",\"preset\":name|[names],\"name\":str (rename)}", pc_engine_always,
     run_edit, true},
    {"tool.presets.reset", "Reset Tool Presets", NULL, 0, NULL, "{}", pc_engine_always, run_reset, true},
};

const pc_engine_command *pc_engine_tool_preset_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
