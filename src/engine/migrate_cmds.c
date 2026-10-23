/* Edit > Presets > Migrate Presets: edit.presets.migratePresets (port of
 * crates/engine/src/migrate_cmds.rs).
 *
 * Merges a presets file (another install's preferences, or an exported preset library) into the
 * current library, appending groups the library doesn't already have (by name). Headless and
 * scriptable. Accepts a full preferences file (with a `presets` section) or a bare presets object.
 *
 * Never crashes (REWRITE_PLAN.md rule 9): a missing/unreadable path or non-JSON file is a graceful
 * error; a malformed section is skipped. */
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"

#include "engine_internal.h"
#include "migrate_cmds.h"
#include "presets.h"

#define CMD "edit.presets.migratePresets"

/* Largest presets/preferences file we read (16 MiB), so a hostile path can't exhaust memory. */
#define MIGRATE_MAX_BYTES ((size_t)16 << 20)

static const char *const MENU_EDIT_PRESETS[] = {"Edit", "Presets"};

static pc_status run_migrate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    size_t plen = 0;
    const char *path = p != NULL ? pc_json_as_str(pc_json_get(p, "path"), &plen) : NULL;
    if (path == NULL || plen == 0) {
        return pc_engine_err_bad_params(err, CMD, "need `path` to a presets or preferences file");
    }
    /* pc_json_as_str returns a borrowed, NUL-terminated string from the parsed params. */
    pc_buf buf = {0};
    pc_status rc = pc_file_read(path, MIGRATE_MAX_BYTES, &buf, err);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return pc_engine_err_other(err, "read `%s`", path);
    }
    yyjson_doc *doc = NULL;
    rc = pc_json_parse((const char *)buf.data, buf.len, &doc, err);
    pc_buf_fini(&buf);
    if (rc != PC_OK) {
        return pc_engine_err_other(err, "`%s` is not JSON", path);
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    /* Accept a preferences file (has a `presets` section) or a bare presets object. */
    yyjson_val *v = pc_json_get(root, "presets");
    if (v == NULL) {
        v = root;
    }
    uint64_t counts[5] = {0, 0, 0, 0, 0};
    size_t total = pc_engine_presets_migrate(&s->presets, v, counts);
    yyjson_doc_free(doc);
    if (total > 0) {
        pc_engine_presets_changed(s);
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, obj, "migrated", total);
    yyjson_mut_val *added = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, added, "gradients", counts[0]);
    yyjson_mut_obj_add_uint(rd, added, "styles", counts[1]);
    yyjson_mut_obj_add_uint(rd, added, "shapes", counts[2]);
    yyjson_mut_obj_add_uint(rd, added, "patternGroups", counts[3]);
    yyjson_mut_obj_add_uint(rd, added, "toolPresets", counts[4]);
    yyjson_mut_obj_add_val(rd, obj, "added", added);
    yyjson_mut_doc_set_root(rd, obj);
    return PC_OK;
}

#define MIGRATE_PARAMS                                                                               \
    "{path} -> {migrated, added:{gradients,styles,shapes,patternGroups,toolPresets}}: merge a "       \
    "presets/preferences file into the library (appends groups by name)"

static const pc_engine_command SPECS[] = {
    {CMD, "Migrate Presets", MENU_EDIT_PRESETS, 2, NULL, MIGRATE_PARAMS, pc_engine_always, run_migrate,
     false},
};

const pc_engine_command *pc_engine_migrate_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
