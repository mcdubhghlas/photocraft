/* Importing Photoshop brush files: brush.presets.importAbr (port of the importAbr half of
 * crates/engine/src/preset_import_cmds.rs). The .abr is parsed and mapped onto brush presets by the
 * io crate (pc_io_read_abr, which calls src/psd/abr.c); this command adds them to the library as one
 * group named after the file, with the file's own folders nested inside it.
 *
 * Runs synchronously (the reference runs it as a cancellable background job; §7). Never crashes on
 * hostile input (rule 9): a missing/oversized/invalid file or base64 is a graceful error. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "io/abr_map.h"
#include "paint/presets.h"
#include "paint/tile.h"

#include "brush_cmds.h" /* pc_engine_brush_presets_changed */
#include "engine_internal.h"
#include "preset_import_cmds.h"
#include "preset_store.h" /* pc_engine_session_load_brush_tips */

#define ABR_MAX_FILE_BYTES ((uint64_t)512 << 20)

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* A name not used by any preset name in `taken` (case-insensitive): name, name 2, name 3… The
 * result is owned (free()). */
static char *unique_name(const char *name, char **taken, size_t ntaken)
{
    bool free_base = true;
    for (size_t i = 0; i < ntaken; i++) {
        int eq = 1;
        const char *a = name, *b = taken[i];
        while (*a != '\0' && *b != '\0') {
            unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
            if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
            if (ca != cb) {
                eq = 0;
                break;
            }
            a++;
            b++;
        }
        if (eq && *a == '\0' && *b == '\0') {
            free_base = false;
            break;
        }
    }
    if (free_base) {
        size_t n = strlen(name);
        char *out = malloc(n + 1);
        if (out != NULL) {
            memcpy(out, name, n + 1);
        }
        return out;
    }
    for (unsigned i = 2; i < 100000; i++) {
        char buf[300];
        int wrote = snprintf(buf, sizeof buf, "%s %u", name, i);
        if (wrote < 0 || (size_t)wrote >= sizeof buf) {
            break;
        }
        bool clash = false;
        for (size_t k = 0; k < ntaken; k++) {
            /* case-insensitive compare */
            const char *a = buf, *b = taken[k];
            int eq = 1;
            while (*a != '\0' && *b != '\0') {
                unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
                if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca - 'A' + 'a');
                if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb - 'A' + 'a');
                if (ca != cb) {
                    eq = 0;
                    break;
                }
                a++;
                b++;
            }
            if (eq && *a == '\0' && *b == '\0') {
                clash = true;
                break;
            }
        }
        if (!clash) {
            size_t n = strlen(buf);
            char *out = malloc(n + 1);
            if (out != NULL) {
                memcpy(out, buf, n + 1);
            }
            return out;
        }
    }
    size_t n = strlen(name);
    char *out = malloc(n + 1);
    if (out != NULL) {
        memcpy(out, name, n + 1);
    }
    return out;
}

/* The file stem (basename without extension) of `path` into *out (owned, may be empty). */
static char *path_stem(const char *path)
{
    const char *base = path;
    for (const char *c = path; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    const char *dot = NULL;
    for (const char *c = base; *c != '\0'; c++) {
        if (*c == '.') {
            dot = c;
        }
    }
    size_t len = dot != NULL && dot != base ? (size_t)(dot - base) : strlen(base);
    char *out = malloc(len + 1);
    if (out != NULL) {
        memcpy(out, base, len);
        out[len] = '\0';
    }
    return out;
}

static pc_status run_import_abr(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.importAbr";
    pc_buf bytes = {0};
    char *stem = NULL;
    pc_status rc;

    yyjson_val *dv = pc_json_get(p, "data");
    if (dv != NULL && !yyjson_is_null(dv)) {
        size_t slen = 0;
        const char *sdata = pc_json_as_str(dv, &slen);
        if (sdata == NULL) {
            return pc_engine_err_bad_params(err, cmd, "`data` must be a base64 string");
        }
        if ((uint64_t)slen > ABR_MAX_FILE_BYTES / 3 * 4 + 4) {
            return pc_engine_err_bad_params(err, cmd, "`data` is too large");
        }
        if (pc_paint_b64_decode(sdata, slen, &bytes, err) != PC_OK) {
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "`data` is not valid base64");
        }
        stem = malloc(1);
        if (stem != NULL) {
            stem[0] = '\0';
        }
    } else {
        yyjson_val *pv = pc_json_get(p, "path");
        size_t plen = 0;
        const char *path = pv != NULL ? pc_json_as_str(pv, &plen) : NULL;
        if (path == NULL || plen == 0) {
            return pc_engine_err_bad_params(err, cmd, "give `path` (.abr file) or `data` (base64)");
        }
        /* pc_json strings are NUL-terminated. */
        rc = pc_engine_read_file(path, &bytes, err);
        if (rc != PC_OK) {
            pc_buf_fini(&bytes);
            return rc;
        }
        if ((uint64_t)bytes.len > ABR_MAX_FILE_BYTES) {
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "%s: file too large", path);
        }
        stem = path_stem(path);
    }
    if (stem == NULL) {
        pc_buf_fini(&bytes);
        return pc_fail(err, PC_ERR_NOMEM, "import");
    }

    /* The group name: `group`, else the file stem, else "Imported Brushes". */
    char *group = NULL;
    yyjson_val *gv = pc_json_get(p, "group");
    if (gv != NULL && !yyjson_is_null(gv)) {
        size_t glen = 0;
        const char *gs = pc_json_as_str(gv, &glen);
        /* trim */
        if (gs != NULL) {
            while (glen > 0 && (gs[0] == ' ' || gs[0] == '\t' || gs[0] == '\n' || gs[0] == '\r')) {
                gs++;
                glen--;
            }
            while (glen > 0 && (gs[glen - 1] == ' ' || gs[glen - 1] == '\t' || gs[glen - 1] == '\n' ||
                                gs[glen - 1] == '\r')) {
                glen--;
            }
        }
        if (gs == NULL || glen == 0) {
            free(stem);
            pc_buf_fini(&bytes);
            return pc_engine_err_bad_params(err, cmd, "`group` must be a non-empty string");
        }
        group = malloc(glen + 1);
        if (group != NULL) {
            memcpy(group, gs, glen);
            group[glen] = '\0';
        }
    } else if (stem[0] != '\0') {
        group = malloc(strlen(stem) + 1);
        if (group != NULL) {
            memcpy(group, stem, strlen(stem) + 1);
        }
    } else {
        const char *def = "Imported Brushes";
        group = malloc(strlen(def) + 1);
        if (group != NULL) {
            memcpy(group, def, strlen(def) + 1);
        }
    }
    free(stem);
    if (group == NULL) {
        pc_buf_fini(&bytes);
        return pc_fail(err, PC_ERR_NOMEM, "group");
    }

    pc_io_abr_import imp = {0};
    rc = pc_io_read_abr(bytes.data, bytes.len, group, &imp, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        free(group);
        return rc;
    }

    /* replace (default true): drop existing non-builtin presets of this group. */
    bool replace = true;
    yyjson_val *rv = pc_json_get(p, "replace");
    bool rvb;
    if (rv != NULL && pc_json_as_bool(rv, &rvb)) {
        replace = rvb;
    }
    if (replace) {
        size_t w = 0;
        for (size_t i = 0; i < s->tools.npresets; i++) {
            if (s->tools.presets[i].builtin || !pc_str_eq_c(&s->tools.presets[i].group, group)) {
                if (w != i) {
                    s->tools.presets[w] = s->tools.presets[i];
                }
                w++;
            } else {
                pc_paint_preset_fini(&s->tools.presets[i]);
            }
        }
        s->tools.npresets = w;
    }

    /* Grow the library for the new presets. */
    size_t add = imp.presets.len;
    size_t cap = s->tools.npresets;
    if (add > 0) {
        pc_paint_preset *grown =
            pc_grow_array(s->tools.presets, &cap, s->tools.npresets + add, sizeof *s->tools.presets);
        if (grown == NULL) {
            pc_io_abr_import_fini(&imp);
            free(group);
            return pc_fail(err, PC_ERR_NOMEM, "presets");
        }
        s->tools.presets = grown;
    }

    /* Collect the taken names (owned C strings). */
    char **taken = NULL;
    size_t ntaken = 0, takencap = 0;
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (ntaken == takencap) {
            size_t nc = takencap ? takencap * 2 : 16;
            char **g = realloc(taken, nc * sizeof(char *));
            if (g == NULL) {
                break;
            }
            taken = g;
            takencap = nc;
        }
        const pc_str *nm = &s->tools.presets[i].name;
        char *d = malloc(nm->len + 1);
        if (d == NULL) {
            break;
        }
        memcpy(d, pc_str_c(nm), nm->len + 1);
        taken[ntaken++] = d;
    }

    /* Names of the imported presets (for the result). */
    char **names = NULL;
    size_t nnames = 0;
    if (add > 0) {
        names = calloc(add, sizeof(char *));
    }
    for (size_t i = 0; i < add; i++) {
        pc_paint_preset preset = imp.presets.v[i]; /* move */
        char *uname = unique_name(pc_str_c(&preset.name), taken, ntaken);
        if (uname != NULL) {
            (void)pc_str_set_c(&preset.name, uname, NULL);
            /* record the chosen name for the result */
            if (names != NULL) {
                char *copy = malloc(strlen(uname) + 1);
                if (copy != NULL) {
                    memcpy(copy, uname, strlen(uname) + 1);
                    names[nnames++] = copy;
                }
            }
            /* make the next unique() see this name; taken owns uname. */
            if (ntaken == takencap) {
                size_t nc = takencap ? takencap * 2 : 16;
                char **g = realloc(taken, nc * sizeof(char *));
                if (g != NULL) {
                    taken = g;
                    takencap = nc;
                }
            }
            if (ntaken < takencap) {
                taken[ntaken++] = uname;
            } else {
                free(uname);
            }
        }
        s->tools.presets[s->tools.npresets++] = preset;
    }
    imp.presets.len = 0; /* moved into the library */
    free(imp.presets.v);
    imp.presets.v = NULL;

    pc_engine_brush_presets_changed(s);

    /* select: make the first imported preset's brush current. */
    bool select = false;
    yyjson_val *sv = pc_json_get(p, "select");
    bool svb;
    if (sv != NULL && pc_json_as_bool(sv, &svb)) {
        select = svb;
    }
    if (select && nnames > 0 && names != NULL) {
        const pc_paint_preset *pr =
            pc_paint_presets_find(s->tools.presets, s->tools.npresets, names[0], strlen(names[0]));
        if (pr != NULL) {
            pc_paint_brush b;
            pc_paint_brush_copy(&b, &pr->brush);
            pc_paint_brush_picked_over(&b, &s->tools.brush);
            if (pc_engine_session_load_brush_tips(s, &b, err) == PC_OK) {
                pc_paint_brush_fini(&s->tools.brush);
                s->tools.brush = b;
            } else {
                pc_paint_brush_fini(&b);
            }
        }
    }

    /* folders: distinct folder prefixes the file brought (non-builtin presets in `group`). */
    size_t folders = 0;
    char **seen = NULL;
    size_t nseen = 0, seencap = 0;
    for (size_t i = 0; i < s->tools.npresets; i++) {
        const pc_paint_preset *pr = &s->tools.presets[i];
        if (pr->builtin || !pc_str_eq_c(&pr->group, group)) {
            continue;
        }
        for (size_t d = 1; d <= pr->folder.len; d++) {
            /* join folder[0..d] with '/' as a set key. */
            pc_buf key = {0};
            bool ok = true;
            for (size_t k = 0; k < d; k++) {
                if (k > 0) {
                    ok = ok && pc_buf_append(&key, "/", 1, NULL) == PC_OK;
                }
                ok = ok && pc_buf_append(&key, pc_str_c(&pr->folder.v[k]), pr->folder.v[k].len, NULL) == PC_OK;
            }
            if (!ok) {
                pc_buf_fini(&key);
                continue;
            }
            const char *ks = key.data != NULL ? (const char *)key.data : "";
            bool dup = false;
            for (size_t j = 0; j < nseen; j++) {
                if (strlen(seen[j]) == key.len && memcmp(seen[j], ks, key.len) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                if (nseen == seencap) {
                    size_t nc = seencap ? seencap * 2 : 8;
                    char **g = realloc(seen, nc * sizeof(char *));
                    if (g != NULL) {
                        seen = g;
                        seencap = nc;
                    }
                }
                if (nseen < seencap) {
                    char *copy = malloc(key.len + 1);
                    if (copy != NULL) {
                        memcpy(copy, ks, key.len);
                        copy[key.len] = '\0';
                        seen[nseen++] = copy;
                        folders++;
                    }
                }
            }
            pc_buf_fini(&key);
        }
    }
    for (size_t j = 0; j < nseen; j++) {
        free(seen[j]);
    }
    free(seen);

    /* Build the result. */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    {
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, root, "group", group);
        yyjson_mut_val *imported = yyjson_mut_arr(rd);
        for (size_t i = 0; i < nnames; i++) {
            yyjson_mut_arr_add_strcpy(rd, imported, names[i]);
        }
        yyjson_mut_obj_add_val(rd, root, "imported", imported);
        yyjson_mut_obj_add_uint(rd, root, "count", nnames);
        yyjson_mut_obj_add_uint(rd, root, "folders", folders);
        yyjson_mut_obj_add_uint(rd, root, "version", imp.version);
        yyjson_mut_val *warns = yyjson_mut_arr(rd);
        for (size_t i = 0; i < imp.warnings.n; i++) {
            if (imp.warnings.v[i] != NULL) {
                yyjson_mut_arr_add_strcpy(rd, warns, imp.warnings.v[i]);
            }
        }
        yyjson_mut_obj_add_val(rd, root, "warnings", warns);
        yyjson_mut_doc_set_root(rd, root);
    }
    rc = PC_OK;
cleanup:
    for (size_t i = 0; i < ntaken; i++) {
        free(taken[i]);
    }
    free(taken);
    for (size_t i = 0; i < nnames; i++) {
        free(names[i]);
    }
    free(names);
    free(group);
    pc_io_abr_import_fini(&imp);
    return rc;
}

static const pc_engine_command SPECS[] = {
    {"brush.presets.importAbr", "Import Brushes…", NULL, 0, NULL,
     "{\"path\":\".abr file\"?,\"data\":base64 bytes?,\"group\":string?=file name,\"replace\":bool=true "
     "(replace a group of the same name),\"select\":bool=false (make the first imported preset current)} "
     "-> {group, imported, count, folders, version, warnings}",
     always, run_import_abr, true},
};

const pc_engine_command *pc_engine_preset_import_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
