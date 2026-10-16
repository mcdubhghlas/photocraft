/* Port of apps/photocraft-cli/src/lib.rs + main.rs. */
#include "cli/cli.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "automation/automation.h"
#include "automation/files.h"
#include "automation/headless.h"
#include "automation/rpc.h"
#include "automation/security.h"
#include "automation/server.h"
#include "automation/workspace.h"
#include "base/json.h"
#include "engine/actions_cmds.h"     /* shell_view/shell_save/nested_failure */
#include "engine/type_extra_cmds.h"  /* pc_engine_missing_font_warnings_for_layer, pc_strlist */
/* pc_doc_arc_get comes transitively from engine/engine.h (via automation/headless.h). */

const char PC_CLI_USAGE[] =
    "photocraft-cli: headless PhotoCraft\n"
    "\n"
    "USAGE:\n"
    "  photocraft-cli convert <in> <out> [--format <ext>] [--quality <1-100>] [--tiff-layers]\n"
    "      Convert between formats (.pcraft, .psd, .png, .jpg, .tif, .webp, .exr, ...).\n"
    "  photocraft-cli info <file> [--compact]\n"
    "      Print the document as JSON (size, mode, depth, layer tree).\n"
    "  photocraft-cli run (<file> | --new <json>) --cmd <id> [--params <json>] [--cmd ...]\n"
    "      [--out <file>] [--format <ext>] [--quality <1-100>] [--tiff-layers]\n"
    "      Open a file, run engine commands in order, save the result.\n"
    "  photocraft-cli batch --actions <actions.json> [--action <name>] --in <dir> --out <dir>\n"
    "      [--format <ext>] [--quality <1-100>] [--in-place] [--tiff-layers]\n"
    "      Apply an action list (JSON) to every image in a directory.\n"
    "  photocraft-cli droplet <file.pcdroplet> <file-or-dir>... [--out <dir>]\n"
    "      Run a droplet on images and folders.\n"
    "  photocraft-cli commands [--json] [--filter <text>]\n"
    "      List the engine command registry.\n"
    "  photocraft-cli mcp [--bridge <127.0.0.1:port>] [--control-token <64-hex> | --control-token-file <path>]\n"
    "      [--automation-read-root <dir>] [--automation-write-root <dir>]\n"
    "      Run the MCP server on stdio (headless engine, or bridge to a running app).\n"
    "  photocraft-cli serve [--port <port>] [--control-token <64-hex> | --control-token-file <path>]\n"
    "      [--automation-read-root <dir>] [--automation-write-root <dir>]\n"
    "      Keep one headless session open and answer JSON lines on stdio or 127.0.0.1:<port>.\n";

/* ---- argument parsing ---- */

typedef struct cli_flag {
    const char *key;
    const char *val; /* NULL for bare flags */
} cli_flag;

typedef struct cli_args {
    const char **pos;
    size_t npos;
    cli_flag *flags;
    size_t nflags;
} cli_args;

static const char *arg_get(const cli_args *a, const char *key)
{
    for (size_t i = a->nflags; i-- > 0;) {
        if (strcmp(a->flags[i].key, key) == 0) {
            return a->flags[i].val;
        }
    }
    return NULL;
}

static bool arg_has(const cli_args *a, const char *key)
{
    for (size_t i = 0; i < a->nflags; i++) {
        if (strcmp(a->flags[i].key, key) == 0) {
            return true;
        }
    }
    return false;
}

static bool in_list(const char *s, const char *const *list)
{
    for (size_t i = 0; list[i]; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Parse the subcommand's args. Returns 0 ok, 1 asked for help, 2 usage error (message in *errmsg). */
static int parse_args(int argc, char **argv, const char *const *values, const char *const *bare,
                      const char *name, cli_args *out, char *errmsg, size_t errcap)
{
    out->pos = calloc((size_t)argc + 1, sizeof(char *));
    out->flags = calloc((size_t)argc + 1, sizeof(cli_flag));
    out->npos = out->nflags = 0;
    if (!out->pos || !out->flags) {
        snprintf(errmsg, errcap, "out of memory");
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (strcmp(s, "--help") == 0 || strcmp(s, "-h") == 0) {
            return 1;
        }
        const char *eq = strchr(s, '=');
        if (eq && s[0] == '-' && s[1] == '-') {
            char key[128];
            size_t kl = (size_t)(eq - s);
            if (kl >= sizeof key) {
                kl = sizeof key - 1;
            }
            memcpy(key, s, kl);
            key[kl] = '\0';
            if (in_list(key, bare)) {
                snprintf(errmsg, errcap, "%s takes no value", key);
                return 2;
            }
            if (!in_list(key, values)) {
                snprintf(errmsg, errcap, "unknown flag %s for %s", key, name);
                return 2;
            }
            /* store key (persist by pointing into argv is unsafe for the '=' form) */
            char *kd = strdup(key);
            out->flags[out->nflags].key = kd;
            out->flags[out->nflags].val = eq + 1;
            out->nflags++;
        } else if (in_list(s, values)) {
            if (i + 1 >= argc) {
                snprintf(errmsg, errcap, "%s needs a value", s);
                return 2;
            }
            out->flags[out->nflags].key = s;
            out->flags[out->nflags].val = argv[++i];
            out->nflags++;
        } else if (in_list(s, bare)) {
            out->flags[out->nflags].key = s;
            out->flags[out->nflags].val = NULL;
            out->nflags++;
        } else if (s[0] == '-' && s[1] == '-') {
            snprintf(errmsg, errcap, "unknown flag %s for %s", s, name);
            return 2;
        } else {
            out->pos[out->npos++] = s;
        }
    }
    return 0;
}

static void args_free(cli_args *a)
{
    /* keys stored via strdup only for the '=' form; those are the ones whose key is not a static
     * flag literal. We cannot easily tell, so we leak nothing important: free pos/flags arrays. */
    free(a->pos);
    free(a->flags);
}

/* ---- export options from flags ---- */

static int export_opts(const cli_args *a, pc_io_export_opts *o, FILE *err)
{
    *o = pc_io_export_opts_default();
    o->tiff_layers = arg_has(a, "--tiff-layers");
    const char *q = arg_get(a, "--quality");
    if (q) {
        char *end;
        long v = strtol(q, &end, 10);
        if (*end != '\0' || v < 1 || v > 100) {
            fprintf(err, "error: bad --quality `%s`: expected a whole number from 1 to 100\n", q);
            return 1;
        }
        o->encode.jpeg_quality = (uint8_t)v;
        o->encode.webp_quality = (uint8_t)v;
        o->encode.webp_lossless = false;
    }
    const char *dpx = arg_get(a, "--dpx-bits");
    if (dpx) {
        char *end;
        long v = strtol(dpx, &end, 10);
        if (*end != '\0') {
            v = -1;
        }
        switch (v) {
        case 8: o->encode.dpx_bits = PC_DPX_BITS_8; break;
        case 10: o->encode.dpx_bits = PC_DPX_BITS_10; break;
        case 12: o->encode.dpx_bits = PC_DPX_BITS_12; break;
        case 16: o->encode.dpx_bits = PC_DPX_BITS_16; break;
        default:
            fprintf(err, "error: bad --dpx-bits `%s`: expected 8, 10, 12 or 16\n", dpx);
            return 1;
        }
    }
    /* --tga-bits is validated (24 or 32) but not applied: the TGA encoder derives the bit depth
     * from the image (REWRITE_PLAN.md §7 A2 L138; there is no force option). */
    const char *tga = arg_get(a, "--tga-bits");
    if (tga) {
        char *end;
        long v = strtol(tga, &end, 10);
        if (*end != '\0' || (v != 24 && v != 32)) {
            fprintf(err, "error: bad --tga-bits `%s`: expected 24 or 32\n", tga);
            return 1;
        }
    }
    return 0;
}

/* ---- warnings ---- */

static void warn_strlist(FILE *err, const pc_strlist *l)
{
    for (size_t i = 0; i < l->n; i++) {
        fprintf(err, "warning: %s\n", l->v[i]);
    }
}

/* Append a JSON string array to *l (owned copies). */
static void arr_to_strlist(yyjson_mut_val *arr, pc_strlist *l)
{
    if (!arr || !yyjson_mut_is_arr(arr)) {
        return;
    }
    yyjson_mut_val *v;
    yyjson_mut_arr_iter it;
    yyjson_mut_arr_iter_init(arr, &it);
    while ((v = yyjson_mut_arr_iter_next(&it))) {
        const char *s = yyjson_mut_get_str(v);
        if (s) {
            (void)pc_strlist_push(l, s, strlen(s), NULL);
        }
    }
}

/* ---- JSON printing ---- */

static int print_json(FILE *out, yyjson_mut_val *root, bool compact, FILE *err)
{
    pc_buf buf = PC_BUF_INIT;
    pc_error e;
    pc_error_clear(&e);
    if (pc_json_write(root, compact ? 0u : PC_JSON_PRETTY, &buf, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        pc_buf_fini(&buf);
        return 1;
    }
    fwrite(buf.data, 1, buf.len, out);
    fputc('\n', out);
    pc_buf_fini(&buf);
    return 0;
}

/* ---- subcommands ---- */

static int cmd_convert(const cli_args *a, FILE *out, FILE *err)
{
    (void)out;
    if (a->npos != 2) {
        fprintf(err, "error: convert needs <in> <out>\n");
        return 1;
    }
    pc_io_export_opts opts;
    if (export_opts(a, &opts, err)) {
        return 1;
    }
    pc_auto_opened o;
    pc_error e;
    pc_error_clear(&e);
    if (pc_auto_open_path(a->pos[0], &o, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    for (size_t i = 0; i < o.warnings.n; i++) {
        fprintf(err, "warning: %s\n", o.warnings.v[i]);
    }
    /* missing-font warnings */
    pc_strlist fonts = {NULL, 0};
    pc_walk w;
    if (pc_document_walk(&o.document, &w, NULL) == PC_OK) {
        for (size_t i = 0; i < w.len; i++) {
            const pc_layer *l = w.items[i].layer;
            if (l->content.kind == PC_LAYER_TEXT) {
                (void)pc_engine_missing_font_warnings_for_layer(&l->content.u.text, &fonts, NULL);
            }
        }
        pc_walk_fini(&w);
    }
    warn_strlist(err, &fonts);
    pc_strlist_fini(&fonts);
    pc_io_warnings warnings = PC_IO_WARNINGS_INIT;
    pc_status rc = pc_auto_save(&o.document, a->pos[1], arg_get(a, "--format"), &opts, NULL, &warnings, &e);
    for (size_t i = 0; i < warnings.n; i++) {
        fprintf(err, "warning: %s\n", warnings.v[i]);
    }
    pc_io_warnings_fini(&warnings);
    pc_auto_opened_fini(&o);
    if (rc != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    return 0;
}

static int cmd_info(const cli_args *a, FILE *out, FILE *err)
{
    if (a->npos != 1) {
        fprintf(err, "error: info needs <file>\n");
        return 1;
    }
    pc_headless *h;
    pc_error e;
    pc_error_clear(&e);
    if (pc_headless_trusted_local(&h, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    pc_json_value opened = PC_JSON_VALUE_INIT, doc = PC_JSON_VALUE_INIT;
    int ret = 1;
    if (pc_headless_open(h, a->pos[0], &opened, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        goto done;
    }
    if (pc_headless_inspect(h, -1, &doc, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        goto done;
    }
    yyjson_mut_val *root = pc_json_value_root(&doc);
    if (root && yyjson_mut_is_obj(root)) {
        yyjson_mut_obj_remove_key(root, "history");
        yyjson_mut_obj_remove_key(root, "canUndo");
        yyjson_mut_obj_remove_key(root, "canRedo");
        yyjson_mut_obj_remove_key(root, "revision");
        yyjson_mut_doc *rd = doc.doc;
        /* warnings from the open result */
        yyjson_mut_val *warr = yyjson_mut_arr(rd);
        yyjson_mut_val *oroot = pc_json_value_root(&opened);
        yyjson_mut_val *ws = oroot ? yyjson_mut_obj_get(oroot, "warnings") : NULL;
        if (ws && yyjson_mut_is_arr(ws)) {
            yyjson_mut_val *v;
            yyjson_mut_arr_iter it;
            yyjson_mut_arr_iter_init(ws, &it);
            while ((v = yyjson_mut_arr_iter_next(&it))) {
                const char *s = yyjson_mut_get_str(v);
                if (s) {
                    yyjson_mut_arr_add_strcpy(rd, warr, s);
                }
            }
        }
        yyjson_mut_obj_remove_key(root, "warnings");
        yyjson_mut_obj_add_val(rd, root, "warnings", warr);
        yyjson_mut_obj_remove_key(root, "file");
        yyjson_mut_obj_add_strcpy(rd, root, "file", a->pos[0]);
    }
    ret = print_json(out, root, arg_has(a, "--compact"), err);
done:
    pc_json_value_fini(&opened);
    pc_json_value_fini(&doc);
    pc_headless_free(h);
    return ret;
}

static int cmd_commands(const cli_args *a, FILE *out, FILE *err)
{
    pc_headless *h;
    pc_error e;
    pc_error_clear(&e);
    if (pc_headless_new(&h, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    pc_json_value list = PC_JSON_VALUE_INIT;
    int ret = 1;
    if (pc_headless_command_list(h, &list, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        goto done;
    }
    const char *filter = arg_get(a, "--filter");
    char needle[256] = {0};
    if (filter) {
        size_t n = 0;
        for (const char *p = filter; *p && n + 1 < sizeof needle; p++) {
            char c = *p;
            needle[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        needle[n] = '\0';
    }
    yyjson_mut_val *arr = pc_json_value_root(&list);
    bool json = arg_has(a, "--json");
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *jarr = NULL;
    if (json) {
        jd = yyjson_mut_doc_new(NULL);
        jarr = jd ? yyjson_mut_arr(jd) : NULL;
    }
    if (arr && yyjson_mut_is_arr(arr)) {
        yyjson_mut_val *c;
        yyjson_mut_arr_iter it;
        yyjson_mut_arr_iter_init(arr, &it);
        while ((c = yyjson_mut_arr_iter_next(&it))) {
            const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(c, "id"));
            const char *label = yyjson_mut_get_str(yyjson_mut_obj_get(c, "label"));
            if (filter) {
                char hay[512];
                snprintf(hay, sizeof hay, "%s %s", id ? id : "", label ? label : "");
                for (char *q = hay; *q; q++) {
                    if (*q >= 'A' && *q <= 'Z') {
                        *q = (char)(*q - 'A' + 'a');
                    }
                }
                if (!strstr(hay, needle)) {
                    continue;
                }
            }
            if (json && jd && jarr) {
                yyjson_mut_arr_append(jarr, yyjson_mut_val_mut_copy(jd, c));
            } else {
                yyjson_mut_val *menu = yyjson_mut_obj_get(c, "menu");
                char menubuf[256] = {0};
                if (menu && yyjson_mut_is_arr(menu)) {
                    size_t mo = 0;
                    yyjson_mut_val *mv;
                    yyjson_mut_arr_iter mit;
                    yyjson_mut_arr_iter_init(menu, &mit);
                    bool first = true;
                    while ((mv = yyjson_mut_arr_iter_next(&mit))) {
                        const char *ms = yyjson_mut_get_str(mv);
                        if (!ms) {
                            continue;
                        }
                        int wn = snprintf(menubuf + mo, sizeof menubuf - mo, "%s%s", first ? "" : " > ", ms);
                        if (wn > 0) {
                            mo += (size_t)wn;
                        }
                        first = false;
                    }
                }
                fprintf(out, "%-44s %-32s %s\n", id ? id : "", label ? label : "", menubuf);
            }
        }
    }
    if (json && jd && jarr) {
        ret = print_json(out, jarr, false, err);
    } else {
        ret = 0;
    }
    yyjson_mut_doc_free(jd);
done:
    pc_json_value_fini(&list);
    pc_headless_free(h);
    return ret;
}

/* Collect the active document's newly-seen missing-font warnings (deduped against `printed`). */
static void report_new_fonts(pc_headless *h, pc_strlist *printed, FILE *err, pc_strlist *new_out)
{
    const pc_engine_doc_state *d = pc_engine_session_active(h->session);
    if (!d) {
        return;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_strlist all = {NULL, 0};
    pc_walk w;
    if (pc_document_walk(doc, &w, NULL) == PC_OK) {
        for (size_t i = 0; i < w.len; i++) {
            const pc_layer *l = w.items[i].layer;
            if (l->content.kind == PC_LAYER_TEXT) {
                (void)pc_engine_missing_font_warnings_for_layer(&l->content.u.text, &all, NULL);
            }
        }
        pc_walk_fini(&w);
    }
    for (size_t i = 0; i < all.n; i++) {
        bool seen = false;
        for (size_t j = 0; j < printed->n; j++) {
            if (strcmp(all.v[i], printed->v[j]) == 0) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            (void)pc_strlist_push(printed, all.v[i], strlen(all.v[i]), NULL);
            if (new_out) {
                (void)pc_strlist_push(new_out, all.v[i], strlen(all.v[i]), NULL);
            }
            fprintf(err, "warning: %s\n", all.v[i]);
        }
    }
    pc_strlist_fini(&all);
}

static int cmd_run(const cli_args *a, FILE *out, FILE *err)
{
    pc_io_export_opts opts;
    if (export_opts(a, &opts, err)) {
        return 1;
    }
    pc_headless *h;
    pc_error e;
    pc_error_clear(&e);
    if (pc_headless_trusted_local(&h, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    pc_strlist printed = {NULL, 0};
    int ret = 1;
    const char *newjson = arg_get(a, "--new");
    if (a->npos == 1 && !newjson) {
        pc_json_value opened = PC_JSON_VALUE_INIT;
        if (pc_headless_open(h, a->pos[0], &opened, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            goto done;
        }
        yyjson_mut_val *oroot = pc_json_value_root(&opened);
        yyjson_mut_val *ws = oroot ? yyjson_mut_obj_get(oroot, "warnings") : NULL;
        pc_strlist wl = {NULL, 0};
        arr_to_strlist(ws, &wl);
        for (size_t i = 0; i < wl.n; i++) {
            fprintf(err, "warning: %s\n", wl.v[i]);
            (void)pc_strlist_push(&printed, wl.v[i], strlen(wl.v[i]), NULL);
        }
        pc_strlist_fini(&wl);
        pc_json_value_fini(&opened);
    } else if (a->npos == 0 && newjson) {
        yyjson_doc *p = NULL;
        if (pc_json_parse(newjson, strlen(newjson), &p, &e) != PC_OK) {
            fprintf(err, "error: bad --new JSON: %s\n", e.msg);
            goto done;
        }
        pc_json_value r = PC_JSON_VALUE_INIT;
        pc_status rc = pc_headless_command_start(h, "file.new", yyjson_doc_get_root(p), true, &r, &e);
        yyjson_doc_free(p);
        pc_json_value_fini(&r);
        if (rc != PC_OK) {
            fprintf(err, "error: --new: %s\n", e.msg);
            goto done;
        }
    } else {
        fprintf(err, "error: run needs exactly one of <file> or --new <json>\n");
        goto done;
    }
    /* Collect --cmd/--params pairs in order. */
    bool any_cmd = false;
    for (size_t i = 0; i < a->nflags; i++) {
        if (strcmp(a->flags[i].key, "--cmd") != 0) {
            continue;
        }
        any_cmd = true;
        const char *id = a->flags[i].val;
        yyjson_doc *pdoc = NULL;
        yyjson_val *params = NULL;
        /* a following --params (immediately after this --cmd) is its params */
        if (i + 1 < a->nflags && strcmp(a->flags[i + 1].key, "--params") == 0) {
            if (pc_json_parse(a->flags[i + 1].val, strlen(a->flags[i + 1].val), &pdoc, &e) != PC_OK) {
                fprintf(err, "error: bad --params JSON for `%s`: %s\n", id, e.msg);
                goto done;
            }
            params = yyjson_doc_get_root(pdoc);
        }
        pc_json_value r = PC_JSON_VALUE_INIT;
        pc_status rc = pc_headless_command_start(h, id, params, true, &r, &e);
        if (rc != PC_OK) {
            fprintf(err, "error: `%s`: %s\n", id, e.msg);
            yyjson_doc_free(pdoc);
            pc_json_value_fini(&r);
            goto done;
        }
        char *nested = pc_engine_actions_nested_failure(id, pc_json_value_root(&r));
        if (nested) {
            fprintf(err, "error: `%s`: %s\n", id, nested);
            free(nested);
            yyjson_doc_free(pdoc);
            pc_json_value_fini(&r);
            goto done;
        }
        pc_strlist newf = {NULL, 0};
        report_new_fonts(h, &printed, err, &newf);
        /* print {"command": id, "result": r, ["warnings": newf]} */
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, o, "command", id);
        yyjson_mut_val *rc_root = pc_json_value_root(&r);
        yyjson_mut_obj_add_val(rd, o, "result",
                               rc_root ? yyjson_mut_val_mut_copy(rd, rc_root) : yyjson_mut_null(rd));
        if (newf.n > 0) {
            yyjson_mut_val *wa = yyjson_mut_arr(rd);
            for (size_t k = 0; k < newf.n; k++) {
                yyjson_mut_arr_add_strcpy(rd, wa, newf.v[k]);
            }
            yyjson_mut_obj_add_val(rd, o, "warnings", wa);
        }
        (void)print_json(out, o, true, err);
        yyjson_mut_doc_free(rd);
        pc_strlist_fini(&newf);
        pc_json_value_fini(&r);
        yyjson_doc_free(pdoc);
    }
    const char *outp = arg_get(a, "--out");
    if (!any_cmd && !outp) {
        fprintf(err, "error: run needs at least one --cmd (or --out)\n");
        goto done;
    }
    report_new_fonts(h, &printed, err, NULL);
    if (outp) {
        pc_json_value r = PC_JSON_VALUE_INIT;
        if (pc_headless_save(h, -1, outp, arg_get(a, "--format"), &opts, &r, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            pc_json_value_fini(&r);
            goto done;
        }
        pc_json_value_fini(&r);
    }
    ret = 0;
done:
    pc_strlist_fini(&printed);
    pc_headless_free(h);
    return ret;
}

/* ---- batch ---- */

static bool known_input_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    char ext[16];
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < sizeof ext; p++) {
        char c = *p;
        ext[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    ext[n] = '\0';
    static const char *const native[] = {"pcraft", "psd",      "psb",      "pdn", "ora",
                                         "kra",    "krz",      "af",       "afdesign", "afphoto", "afpub", NULL};
    return in_list(ext, native);
}

static int cmd_batch(const cli_args *a, FILE *out, FILE *err);
static int cmd_droplet(const cli_args *a, FILE *out, FILE *err);

static int cmd_serve(const cli_args *a, FILE *out, FILE *err)
{
    (void)out;
    pc_auto_workspace ws;
    pc_error e;
    pc_error_clear(&e);
    if (pc_auto_workspace_init(&ws, arg_get(a, "--automation-read-root"), arg_get(a, "--automation-write-root"),
                               &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    pc_headless *h;
    if (pc_headless_with_workspace(&ws, &h, &e) != PC_OK) {
        pc_auto_workspace_fini(&ws);
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    int ret = 0;
    const char *port = arg_get(a, "--port");
    if (port) {
        char token[65];
        const char *supplied = arg_get(a, "--control-token");
        const char *tfile = arg_get(a, "--control-token-file");
        if (!supplied) {
            supplied = getenv("PHOTOCRAFT_CONTROL_TOKEN");
        }
        if (!tfile) {
            tfile = getenv("PHOTOCRAFT_CONTROL_TOKEN_FILE");
        }
        if (pc_auto_server_token(supplied, tfile, token, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            ret = 1;
            goto done;
        }
        if (tfile) {
            fprintf(err, "photocraft-cli control token file: %s\n", tfile);
        } else if (!supplied) {
            fprintf(err, "photocraft-cli control token: %s\n", token);
        }
        char addr[64];
        snprintf(addr, sizeof addr, "127.0.0.1:%s", port);
        if (pc_auto_serve_tcp(addr, h, token, NULL, NULL, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            ret = 1;
        }
    } else {
        if (pc_auto_serve_lines(h, 0, 1, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            ret = 1;
        }
    }
done:
    pc_headless_free(h);
    return ret;
}

static int cmd_mcp(const cli_args *a, FILE *out, FILE *err)
{
    (void)out;
    pc_error e;
    pc_error_clear(&e);
    pc_auto_mcp *m = NULL;
    const char *bridge = arg_get(a, "--bridge");
    if (bridge) {
        char token[65];
        const char *supplied = arg_get(a, "--control-token");
        const char *tfile = arg_get(a, "--control-token-file");
        if (!supplied) {
            supplied = getenv("PHOTOCRAFT_CONTROL_TOKEN");
        }
        if (!tfile) {
            tfile = getenv("PHOTOCRAFT_CONTROL_TOKEN_FILE");
        }
        if (pc_auto_client_token(supplied, tfile, token, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            return 1;
        }
        if (pc_auto_mcp_bridge(bridge, token, &m, &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            return 1;
        }
    } else {
        pc_auto_workspace ws;
        if (pc_auto_workspace_init(&ws, arg_get(a, "--automation-read-root"),
                                   arg_get(a, "--automation-write-root"), &e) != PC_OK) {
            fprintf(err, "error: %s\n", e.msg);
            return 1;
        }
        if (pc_auto_mcp_headless_with_workspace(&ws, &m, &e) != PC_OK) {
            pc_auto_workspace_fini(&ws);
            fprintf(err, "error: %s\n", e.msg);
            return 1;
        }
    }
    int ret = 0;
    if (pc_auto_mcp_serve(m, 0, 1, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        ret = 1;
    }
    pc_auto_mcp_free(m);
    return ret;
}

/* ---- dispatch ---- */

int pc_cli_run(int argc, char **argv, FILE *out, FILE *err)
{
    if (argc < 1) {
        fputs(PC_CLI_USAGE, err);
        return 2;
    }
    const char *cmd = argv[0];
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        fputs(PC_CLI_USAGE, out);
        return 0;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        char ver[128];
        (void)pc_engine_long_version(ver, sizeof ver);
        fprintf(out, "photocraft-cli %s\n", ver);
        return 0;
    }
    struct {
        const char *name;
        const char *const *values;
        const char *const *bare;
        int (*run)(const cli_args *, FILE *, FILE *);
    } subs[] = {
        {"convert", (const char *const[]){"--format", "--quality", "--tga-bits", "--dpx-bits", NULL},
         (const char *const[]){"--tiff-layers", NULL}, cmd_convert},
        {"info", (const char *const[]){NULL}, (const char *const[]){"--compact", NULL}, cmd_info},
        {"run",
         (const char *const[]){"--new", "--cmd", "--params", "--out", "--format", "--quality", "--tga-bits",
                               "--dpx-bits", NULL},
         (const char *const[]){"--tiff-layers", NULL}, cmd_run},
        {"batch",
         (const char *const[]){"--actions", "--action", "--in", "--out", "--format", "--quality", "--tga-bits",
                               "--dpx-bits", NULL},
         (const char *const[]){"--in-place", "--tiff-layers", NULL}, cmd_batch},
        {"droplet", (const char *const[]){"--out", NULL}, (const char *const[]){NULL}, cmd_droplet},
        {"commands", (const char *const[]){"--filter", NULL}, (const char *const[]){"--json", NULL},
         cmd_commands},
        {"mcp",
         (const char *const[]){"--bridge", "--control-token", "--control-token-file", "--automation-read-root",
                               "--automation-write-root", NULL},
         (const char *const[]){NULL}, cmd_mcp},
        {"serve",
         (const char *const[]){"--port", "--control-token", "--control-token-file", "--automation-read-root",
                               "--automation-write-root", NULL},
         (const char *const[]){NULL}, cmd_serve},
    };
    for (size_t i = 0; i < sizeof subs / sizeof subs[0]; i++) {
        if (strcmp(cmd, subs[i].name) != 0) {
            continue;
        }
        cli_args a;
        char emsg[256];
        int pr = parse_args(argc - 1, argv + 1, subs[i].values, subs[i].bare, subs[i].name, &a, emsg,
                            sizeof emsg);
        if (pr == 1) {
            args_free(&a);
            fputs(PC_CLI_USAGE, out);
            return 0;
        }
        if (pr == 2) {
            args_free(&a);
            fprintf(err, "error: %s\n\n", emsg);
            fputs(PC_CLI_USAGE, err);
            return 2;
        }
        int rc = subs[i].run(&a, out, err);
        args_free(&a);
        return rc;
    }
    fprintf(err, "error: unknown command `%s`\n\n", cmd);
    fputs(PC_CLI_USAGE, err);
    return 2;
}

/* ---- batch / droplet (appended) ---- */

#include <dirent.h>
#include <sys/stat.h>

#include "base/file.h"
#include "codecs/format.h"

static bool is_input(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (known_input_ext(path)) {
        return true;
    }
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return false;
    }
    char ext[16];
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < sizeof ext; p++) {
        char c = *p;
        ext[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    ext[n] = '\0';
    pc_cformat f;
    return pc_cformat_from_extension(ext, &f) && pc_cformat_caps(f).read;
}

/* One step's command id and params (borrowed). step may be [id,params], {command|id,params}, or id. */
static const char *step_id_params(yyjson_val *step, yyjson_val **params)
{
    *params = NULL;
    if (yyjson_is_str(step)) {
        return yyjson_get_str(step);
    }
    if (yyjson_is_arr(step)) {
        yyjson_val *id = yyjson_arr_get(step, 0);
        *params = yyjson_arr_get(step, 1);
        return id ? yyjson_get_str(id) : NULL;
    }
    if (yyjson_is_obj(step)) {
        yyjson_val *id = pc_json_get(step, "command");
        if (!id) {
            id = pc_json_get(step, "id");
        }
        *params = pc_json_get(step, "params");
        return id ? yyjson_get_str(id) : NULL;
    }
    return NULL;
}

static bool same_dir(const char *a, const char *b)
{
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int cmd_batch(const cli_args *a, FILE *out, FILE *err)
{
    const char *actions_path = arg_get(a, "--actions");
    const char *in_dir = arg_get(a, "--in");
    const char *out_dir = arg_get(a, "--out");
    if (!actions_path) {
        fprintf(err, "error: batch needs --actions <file>\n");
        return 1;
    }
    if (!in_dir) {
        fprintf(err, "error: batch needs --in <dir>\n");
        return 1;
    }
    if (!out_dir) {
        fprintf(err, "error: batch needs --out <dir>\n");
        return 1;
    }
    pc_io_export_opts opts;
    if (export_opts(a, &opts, err)) {
        return 1;
    }
    pc_error e;
    pc_error_clear(&e);
    pc_buf text = PC_BUF_INIT;
    if (pc_file_read(actions_path, 64u << 20, &text, &e) != PC_OK) {
        fprintf(err, "error: %s: %s\n", actions_path, e.msg);
        return 1;
    }
    if (text.len >= 4 && text.data[0] == 0 && text.data[1] == 0 && text.data[2] == 0 && text.data[3] == 16) {
        pc_buf_fini(&text);
        fprintf(err, "error: Photoshop .atn Actions files are not yet supported by the C CLI batch; "
                     "export the action as JSON steps\n");
        return 1;
    }
    yyjson_doc *doc = NULL;
    if (pc_json_parse((const char *)text.data, text.len, &doc, &e) != PC_OK || !doc) {
        pc_buf_fini(&text);
        fprintf(err, "error: actions JSON: %s\n", e.msg);
        return 1;
    }
    pc_buf_fini(&text);
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *list = pc_json_get(root, "actions");
    if (!list) {
        list = root;
    }
    /* Is it an Actions set (array of {name, steps})? */
    yyjson_val *steps = NULL;
    const char *action = arg_get(a, "--action");
    bool is_set = false;
    if (yyjson_is_arr(list) && yyjson_arr_size(list) > 0) {
        is_set = true;
        yyjson_val *x;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(list, &it);
        while ((x = yyjson_arr_iter_next(&it))) {
            if (!yyjson_is_obj(x) || !yyjson_is_str(pc_json_get(x, "name"))) {
                is_set = false;
                break;
            }
        }
    }
    if (is_set) {
        yyjson_val *chosen = NULL;
        yyjson_val *x;
        yyjson_arr_iter it;
        yyjson_arr_iter_init(list, &it);
        while ((x = yyjson_arr_iter_next(&it))) {
            const char *nm = yyjson_get_str(pc_json_get(x, "name"));
            if (action ? (nm && strcmp(nm, action) == 0) : (chosen == NULL)) {
                chosen = x;
                if (action) {
                    break;
                }
            }
        }
        if (!chosen) {
            yyjson_doc_free(doc);
            fprintf(err, "error: no action named `%s` in the set\n", action ? action : "");
            return 1;
        }
        if (!action && yyjson_arr_size(list) > 1) {
            yyjson_doc_free(doc);
            fprintf(err, "error: the Actions set holds %zu actions: pick one with --action <name>\n",
                    yyjson_arr_size(list));
            return 1;
        }
        steps = pc_json_get(chosen, "steps");
    } else {
        steps = pc_json_get(root, "steps");
        if (!steps) {
            /* A .pcdroplet store: {photocraftDroplet, action: {steps: [...]}}. */
            steps = pc_json_get(pc_json_get(root, "action"), "steps");
        }
        if (!steps) {
            steps = list;
        }
    }
    if (!steps || !yyjson_is_arr(steps)) {
        yyjson_doc_free(doc);
        fprintf(err, "error: actions file holds no steps\n");
        return 1;
    }
    if (!arg_has(a, "--in-place") && same_dir(in_dir, out_dir)) {
        yyjson_doc_free(doc);
        fprintf(err, "error: --out %s is the --in folder, so the results would replace the originals: "
                     "choose another --out folder, or pass --in-place to overwrite them\n",
                out_dir);
        return 1;
    }
    if (pc_mkdir_p(out_dir, &e) != PC_OK) {
        yyjson_doc_free(doc);
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    /* Collect inputs, sorted. */
    DIR *dir = opendir(in_dir);
    if (!dir) {
        yyjson_doc_free(doc);
        fprintf(err, "error: %s: cannot read directory\n", in_dir);
        return 1;
    }
    char **inputs = NULL;
    size_t ninputs = 0, capinputs = 0;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", in_dir, de->d_name);
        if (!is_input(full)) {
            continue;
        }
        if (ninputs == capinputs) {
            size_t nc = capinputs ? capinputs * 2 : 16;
            char **g = realloc(inputs, nc * sizeof *g);
            if (!g) {
                break;
            }
            inputs = g;
            capinputs = nc;
        }
        inputs[ninputs++] = strdup(full);
    }
    closedir(dir);
    /* sort */
    for (size_t i = 1; i < ninputs; i++) {
        char *key = inputs[i];
        size_t j = i;
        while (j > 0 && strcmp(inputs[j - 1], key) > 0) {
            inputs[j] = inputs[j - 1];
            j--;
        }
        inputs[j] = key;
    }
    const char *format = arg_get(a, "--format");
    if (format && format[0] == '.') {
        format++;
    }
    char **claimed = calloc(ninputs ? ninputs : 1, sizeof(char *));
    size_t nclaimed = 0;
    size_t ok = 0, failed = 0;
    for (size_t i = 0; i < ninputs; i++) {
        const char *input = inputs[i];
        const char *bn = strrchr(input, '/');
        bn = bn ? bn + 1 : input;
        char stem[256];
        snprintf(stem, sizeof stem, "%s", bn);
        char *dot = strrchr(stem, '.');
        char iext[16] = "png";
        if (dot) {
            snprintf(iext, sizeof iext, "%s", dot + 1);
            *dot = '\0';
        }
        const char *ext = format ? format : iext;
        char target[4300];
        snprintf(target, sizeof target, "%s/%s.%s", out_dir, stem, ext);
        /* claim check */
        bool clash = false;
        for (size_t c = 0; c < nclaimed; c++) {
            if (strcmp(claimed[c], target) == 0) {
                clash = true;
                break;
            }
        }
        pc_error fe;
        pc_error_clear(&fe);
        bool ok_file = false;
        if (clash) {
            fprintf(err, "FAIL  %s: output `%s` was already written in this run\n", input, target);
        } else {
            pc_headless *h;
            if (pc_headless_trusted_local(&h, &fe) != PC_OK) {
                fprintf(err, "FAIL  %s: %s\n", input, fe.msg);
            } else {
                pc_json_value r = PC_JSON_VALUE_INIT;
                bool step_failed = false;
                if (pc_headless_open(h, input, &r, &fe) != PC_OK) {
                    fprintf(err, "FAIL  %s: %s\n", input, fe.msg);
                    step_failed = true;
                }
                pc_json_value_fini(&r);
                yyjson_val *step;
                yyjson_arr_iter sit;
                yyjson_arr_iter_init(steps, &sit);
                while (!step_failed && (step = yyjson_arr_iter_next(&sit))) {
                    yyjson_val *sp;
                    const char *id = step_id_params(step, &sp);
                    if (!id) {
                        continue;
                    }
                    if (pc_engine_actions_shell_view_command(id) || pc_engine_actions_shell_save_command(id)) {
                        continue;
                    }
                    pc_json_value sr = PC_JSON_VALUE_INIT;
                    if (pc_headless_command_start(h, id, sp, true, &sr, &fe) != PC_OK) {
                        fprintf(err, "FAIL  %s: `%s`: %s\n", input, id, fe.msg);
                        step_failed = true;
                    } else {
                        char *nested = pc_engine_actions_nested_failure(id, pc_json_value_root(&sr));
                        if (nested) {
                            fprintf(err, "FAIL  %s: `%s`: %s\n", input, id, nested);
                            free(nested);
                            step_failed = true;
                        }
                    }
                    pc_json_value_fini(&sr);
                }
                if (!step_failed) {
                    pc_json_value sv = PC_JSON_VALUE_INIT;
                    if (pc_headless_save(h, -1, target, ext, &opts, &sv, &fe) != PC_OK) {
                        fprintf(err, "FAIL  %s: %s\n", input, fe.msg);
                    } else {
                        ok_file = true;
                    }
                    pc_json_value_fini(&sv);
                }
                pc_headless_free(h);
            }
        }
        if (ok_file) {
            ok++;
            claimed[nclaimed++] = strdup(target);
            fprintf(out, "ok    %s -> %s\n", input, target);
        } else if (!clash) {
            failed++;
        } else {
            failed++;
        }
        free(inputs[i]);
    }
    free(inputs);
    for (size_t c = 0; c < nclaimed; c++) {
        free(claimed[c]);
    }
    free(claimed);
    yyjson_doc_free(doc);
    fprintf(out, "%zu succeeded, %zu failed\n", ok, failed);
    return failed > 0 ? 1 : 0;
}

static int cmd_droplet(const cli_args *a, FILE *out, FILE *err)
{
    if (a->npos < 1) {
        fprintf(err, "error: droplet needs <file.pcdroplet> and inputs\n");
        return 1;
    }
    if (a->npos < 2) {
        fprintf(err, "error: droplet needs at least one input file or folder\n");
        return 1;
    }
    pc_error e;
    pc_error_clear(&e);
    pc_headless *h;
    if (pc_headless_trusted_local(&h, &e) != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        return 1;
    }
    /* Build {droplet, input:[...], output?}. */
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *o = yyjson_mut_obj(pd);
    yyjson_mut_obj_add_strcpy(pd, o, "droplet", a->pos[0]);
    yyjson_mut_val *in = yyjson_mut_arr(pd);
    for (size_t i = 1; i < a->npos; i++) {
        yyjson_mut_arr_add_strcpy(pd, in, a->pos[i]);
    }
    yyjson_mut_obj_add_val(pd, o, "input", in);
    const char *outp = arg_get(a, "--out");
    if (outp) {
        yyjson_mut_obj_add_strcpy(pd, o, "output", outp);
    }
    yyjson_mut_doc_set_root(pd, o);
    pc_buf text = PC_BUF_INIT;
    (void)pc_json_write(o, 0, &text, NULL);
    yyjson_mut_doc_free(pd);
    pc_json_value r = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(h->session, "file.automate.runDroplet", pc_buf_cstr(&text),
                                                  &r, &e);
    pc_buf_fini(&text);
    int ret = 0;
    if (rc != PC_OK) {
        fprintf(err, "error: %s\n", e.msg);
        ret = 1;
    } else {
        yyjson_mut_val *rr = pc_json_value_root(&r);
        yyjson_mut_val *files = rr ? yyjson_mut_obj_get(rr, "files") : NULL;
        if (files && yyjson_mut_is_arr(files)) {
            yyjson_mut_val *f;
            yyjson_mut_arr_iter it;
            yyjson_mut_arr_iter_init(files, &it);
            while ((f = yyjson_mut_arr_iter_next(&it))) {
                const char *s = yyjson_mut_get_str(f);
                if (s) {
                    fprintf(out, "ok    %s\n", s);
                }
            }
        }
        yyjson_mut_val *errs = rr ? yyjson_mut_obj_get(rr, "errors") : NULL;
        size_t nerr = 0;
        if (errs && yyjson_mut_is_arr(errs)) {
            yyjson_mut_val *ev;
            yyjson_mut_arr_iter it;
            yyjson_mut_arr_iter_init(errs, &it);
            while ((ev = yyjson_mut_arr_iter_next(&it))) {
                const char *file = yyjson_mut_get_str(yyjson_mut_obj_get(ev, "file"));
                const char *msg = yyjson_mut_get_str(yyjson_mut_obj_get(ev, "error"));
                fprintf(err, "FAIL  %s: %s\n", file ? file : "", msg ? msg : "");
                nerr++;
            }
        }
        if (nerr > 0) {
            ret = 1;
        }
    }
    pc_json_value_fini(&r);
    pc_headless_free(h);
    return ret;
}
