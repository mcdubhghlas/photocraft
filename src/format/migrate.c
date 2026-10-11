/* Manifest migrations (crates/format/src/migrate.rs) and the manifest parse
 * (store::read_manifest after the bytes are read and their depth checked).
 *
 * Each step upgrades the raw JSON from version n to n + 1, so old bundles keep loading as the
 * schema evolves. There are no steps while version 1 is the only version; the parse runs the
 * version check on the immutable tree and only copies it into a mutable one when a step has to
 * run. */
#include <string.h>

#include "base/json.h"
#include "format/bridge.h"
#include "format/manifest.h"

/* STEPS[i] upgrades version i + 1 to i + 2. */
static const pc_format_migrate_step *const STEPS = NULL;
static const size_t NSTEPS = 0;

/* Value::as_u64 */
static bool mut_as_u64(yyjson_mut_val *v, uint64_t *out)
{
    if (yyjson_mut_is_uint(v)) {
        *out = yyjson_mut_get_uint(v);
        return true;
    }
    if (yyjson_mut_is_sint(v) && yyjson_mut_get_sint(v) >= 0) {
        *out = (uint64_t)yyjson_mut_get_sint(v);
        return true;
    }
    return false;
}

/* Value::get on an object (the last of duplicate keys, as serde_json keeps it). */
static yyjson_mut_val *mut_get(yyjson_mut_val *obj, const char *key)
{
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    yyjson_mut_val *found = NULL;
    if (!yyjson_mut_is_obj(obj)) {
        return NULL;
    }
    (void)yyjson_mut_obj_iter_init(obj, &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        if (yyjson_mut_equals_str(k, key)) {
            found = yyjson_mut_obj_iter_get_val(k);
        }
    }
    return found;
}

static pc_status check_version(bool present, uint64_t v, uint32_t target, uint32_t *found, pc_error *err)
{
    if (!present) {
        return pc_format_corrupt(err, "manifest has no format_version");
    }
    if (v > UINT32_MAX) {
        return pc_format_corrupt(err, "format_version out of range");
    }
    if (v == 0) {
        return pc_format_corrupt(err, "format_version 0 is invalid");
    }
    if (v > target) {
        return pc_format_too_new(err, v, target);
    }
    *found = (uint32_t)v;
    return PC_OK;
}

pc_status pc_format_manifest_version(yyjson_val *root, uint32_t target, uint32_t *found, pc_error *err)
{
    yyjson_val *v = yyjson_is_obj(root) ? pc_json_get(root, "format_version") : NULL;
    uint64_t u = 0;
    bool present = v != NULL && pc_json_as_u64(v, &u);
    return check_version(present, u, target, found, err);
}

pc_status pc_format_manifest_version_mut(yyjson_mut_val *root, uint32_t target, uint32_t *found,
                                         pc_error *err)
{
    yyjson_mut_val *v = mut_get(root, "format_version");
    uint64_t u = 0;
    bool present = v != NULL && mut_as_u64(v, &u);
    return check_version(present, u, target, found, err);
}

pc_status pc_format_migrate_with(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                 const pc_format_migrate_step *steps, size_t nsteps, uint32_t target,
                                 uint32_t *found, pc_error *err)
{
    uint32_t start = 0;
    pc_status st = pc_format_manifest_version_mut(root, target, &start, err);
    if (st != PC_OK) {
        return st;
    }
    for (uint32_t ver = start; ver < target; ver++) {
        yyjson_mut_val *key;
        yyjson_mut_val *val;
        if ((size_t)ver - 1 >= nsteps || steps == NULL) {
            return pc_format_unsupported(err, "no migration from format %lu", (unsigned long)ver);
        }
        st = steps[ver - 1](doc, root, err);
        if (st != PC_OK) {
            return st;
        }
        key = yyjson_mut_str(doc, "format_version");
        val = yyjson_mut_uint(doc, (uint64_t)ver + 1);
        if (key == NULL || val == NULL || !yyjson_mut_obj_put(root, key, val)) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    *found = start;
    return PC_OK;
}

/* Runs the build's migrations on a parsed manifest that is older than this build: copies it
 * into a mutable tree, migrates, and replaces *doc with the result. */
static pc_status migrate_old(yyjson_doc **doc, pc_error *err)
{
    yyjson_mut_doc *m = yyjson_doc_mut_copy(*doc, NULL);
    yyjson_doc *back;
    uint32_t found = 0;
    pc_status st;
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    st = pc_format_migrate_with(m, yyjson_mut_doc_get_root(m), STEPS, NSTEPS, PC_FORMAT_VERSION, &found, err);
    if (st == PC_OK) {
        back = yyjson_mut_doc_imut_copy(m, NULL);
        if (back == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            yyjson_doc_free(*doc);
            *doc = back;
        }
    }
    yyjson_mut_doc_free(m);
    return st;
}

pc_status pc_format_manifest_parse(const uint8_t *json, size_t len, pc_format_manifest **out, pc_error *err)
{
    yyjson_doc *doc = NULL;
    uint32_t found = 0;
    pc_status st;
    /* serde_json's 128-level limit is replaced by the store's linear depth check. The store has
     * run it already; running it here too keeps direct callers' input bounded, since decoding
     * recurses with the JSON nesting (yyjson's parser itself does not recurse). */
    st = pc_format_check_manifest_depth(json, len, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_json_parse_depth((const char *)json, len, 0, &doc, err);
    if (st != PC_OK) {
        return pc_format_json(st, err);
    }
    st = pc_format_manifest_version(yyjson_doc_get_root(doc), PC_FORMAT_VERSION, &found, err);
    if (st == PC_OK && found < PC_FORMAT_VERSION) {
        st = migrate_old(&doc, err);
    }
    if (st == PC_OK) {
        st = pc_format_manifest_decode(yyjson_doc_get_root(doc), out, err);
        if (st == PC_ERR_INVALID) {
            st = pc_format_json(st, err); /* serde's from_value errors are FormatError::Json */
        }
    }
    yyjson_doc_free(doc);
    return st;
}
