/* See ui_state.h. */
#include "ui_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <yyjson.h>

#include "app_dirs.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "base/log.h"
#include "base/mem.h"

#define UI_STATE_FILE "ui_state.json"

bool pc_app_ui_state_path(const char *config_dir, char *out, size_t cap)
{
    if (!config_dir || !out || cap == 0) {
        return false;
    }
    int n = snprintf(out, cap, "%s/%s", config_dir, UI_STATE_FILE);
    return n > 0 && (size_t)n < cap;
}

pc_status pc_app_ui_state_save(const pc_ui_state *s, const char *config_dir, pc_error *err)
{
    char path[PC_APP_PATH_MAX];
    if (!pc_app_ui_state_path(config_dir, path, sizeof path)) {
        return pc_fail(err, PC_ERR_INVALID, "no settings directory for UI state");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = NULL;
    pc_status rc = pc_ui_state_to_json(s, doc, &root, err);
    if (rc != PC_OK) {
        yyjson_mut_doc_free(doc);
        return rc;
    }
    yyjson_mut_doc_set_root(doc, root);
    size_t len = 0;
    char *text = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!text) {
        return pc_fail(err, PC_ERR_NOMEM, "UI state encoding failed");
    }
    rc = pc_file_write_atomic(path, text, len, err);
    free(text);
    return rc;
}

void pc_app_ui_state_load(pc_ui_state *s, const char *config_dir)
{
    char path[PC_APP_PATH_MAX];
    if (!s || !pc_app_ui_state_path(config_dir, path, sizeof path)) {
        return;
    }
    pc_buf buf = PC_BUF_INIT;
    pc_error err;
    if (pc_file_read(path, PC_MAX_ALLOC, &buf, &err) != PC_OK) {
        pc_buf_fini(&buf);
        return; /* no file (or unreadable): keep defaults, leave the file alone */
    }
    yyjson_doc *doc = NULL;
    bool bad = false;
    if (pc_json_parse((const char *)buf.data, buf.len, &doc, &err) != PC_OK || !doc) {
        bad = true;
    } else if (pc_ui_state_from_json(s, yyjson_doc_get_root(doc), &err) != PC_OK) {
        bad = true;
    }
    yyjson_doc_free(doc);
    pc_buf_fini(&buf);
    if (bad) {
        pc_log(PC_LOG_WARN, "ignoring saved UI state %s: %s", path, err.msg);
        (void)unlink(path); /* so a corrupt file cannot keep breaking launches */
    }
}
