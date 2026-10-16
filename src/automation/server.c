/* Port of crates/automation/src/server.rs and the server submodules. MCP JSON-RPC over stdio, poll()-based. */
#include "automation/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "automation/automation.h"
#include "automation/budgets.h"
#include "automation/security.h"
#include "base/base64.h"
#include "base/json.h"

#define MCP_PROTOCOL "2025-06-18"
#define INSTRUCTIONS                                                                                           \
    "PhotoCraft image editor. Start with doc_new/doc_open, discover ids and parameter docs with "             \
    "command_list, edit with command_run or command_batch (one history step per command). Inspect with "      \
    "doc_inspect and render_preview; doc_save writes native .pcraft or exports by extension. File paths are " \
    "relative to the read/write roots granted at launch. ui_inspect/ui_screenshot and other ui_* tools "       \
    "require bridge mode. Read photocraft://document and photocraft://commands for live JSON state."

struct pc_auto_mcp {
    pc_headless *headless; /* non-NULL for a headless backend */
    pc_auto_bridge *bridge; /* non-NULL for a bridge backend */
};

/* ---- construction ---- */

pc_status pc_auto_mcp_headless(pc_auto_mcp **out, pc_error *err)
{
    pc_auto_mcp *m = calloc(1, sizeof *m);
    if (!m) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (pc_headless_new(&m->headless, err) != PC_OK) {
        free(m);
        return err ? err->code : PC_ERR_NOMEM;
    }
    *out = m;
    return PC_OK;
}

pc_status pc_auto_mcp_headless_with_workspace(pc_auto_workspace *ws, pc_auto_mcp **out, pc_error *err)
{
    pc_auto_mcp *m = calloc(1, sizeof *m);
    if (!m) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (pc_headless_with_workspace(ws, &m->headless, err) != PC_OK) {
        free(m);
        return err ? err->code : PC_ERR_NOMEM;
    }
    *out = m;
    return PC_OK;
}

pc_status pc_auto_mcp_bridge(const char *addr, const char *token, pc_auto_mcp **out, pc_error *err)
{
    pc_auto_mcp *m = calloc(1, sizeof *m);
    if (!m) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (pc_auto_bridge_new(addr, token, &m->bridge, err) != PC_OK) {
        free(m);
        return err ? err->code : PC_ERR_INVALID;
    }
    *out = m;
    return PC_OK;
}

void pc_auto_mcp_free(pc_auto_mcp *m)
{
    if (!m) {
        return;
    }
    pc_headless_free(m->headless);
    pc_auto_bridge_free(m->bridge);
    free(m);
}

/* ---- tool table ---- */

typedef struct tool_def {
    const char *name;
    const char *description;
    bool read;     /* readOnlyHint / idempotentHint */
    bool additive; /* doc_new/doc_open/doc_place: not destructive */
    const char *schema; /* inputSchema JSON (object, with additionalProperties:false) */
} tool_def;

static const tool_def TOOLS[] = {
    {"session_list", "List open documents (index, name, size, dirty) and the active index.", true, false,
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"doc_open", "Open an image or document file and make it active. Returns index, size and import warnings.",
     false, true,
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}"},
    {"doc_place",
     "Place a file into the document as a Smart Object layer (Place Embedded), reading `path` beneath the read root.",
     false, true,
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"index\":{\"type\":\"integer\"},\"scale\":{\"type\":\"number\"},\"fit\":{\"type\":\"boolean\"},\"center\":{\"type\":\"array\"}},\"required\":[\"path\"],\"additionalProperties\":false}"},
    {"doc_export_layers",
     "Export each exported layer of the document as its own file beneath the write root.", false, false,
     "{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\"},\"index\":{\"type\":\"integer\"},\"format\":{\"type\":\"string\"},\"prefix\":{\"type\":\"string\"},\"visible_only\":{\"type\":\"boolean\"},\"quality\":{\"type\":\"number\"}},\"required\":[\"dir\"],\"additionalProperties\":false}"},
    {"doc_export_data_sets", "Export every data set of the document as its own flattened file.", false, false,
     "{\"type\":\"object\",\"properties\":{\"dir\":{\"type\":\"string\"},\"index\":{\"type\":\"integer\"},\"format\":{\"type\":\"string\"},\"data_sets\":{\"type\":\"array\"},\"naming\":{\"type\":\"string\"}},\"required\":[\"dir\"],\"additionalProperties\":false}"},
    {"doc_import_data_sets", "Import a data set CSV from the read root into the document.", false, false,
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"index\":{\"type\":\"integer\"},\"delimiter\":{\"type\":\"string\"}},\"required\":[\"path\"],\"additionalProperties\":false}"},
    {"doc_new", "Create a new document (defaults: 1920x1080 RGB 8-bit white).", false, true,
     "{\"type\":\"object\",\"properties\":{\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},\"mode\":{\"type\":\"string\"},\"depth\":{\"type\":\"integer\"},\"background\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"additionalProperties\":false}"},
    {"doc_save", "Save the document. .pcraft is the native format; other extensions export.", false, false,
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"format\":{\"type\":\"string\"},\"quality\":{\"type\":\"integer\"},\"tiffLayers\":{\"type\":\"boolean\"},\"tgaBits\":{\"type\":\"integer\"},\"dpxBits\":{\"type\":\"integer\"},\"index\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"doc_export", "Export the document to another format (same as doc_save with an explicit path).", false,
     false,
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"format\":{\"type\":\"string\"},\"quality\":{\"type\":\"integer\"},\"tiffLayers\":{\"type\":\"boolean\"},\"tgaBits\":{\"type\":\"integer\"},\"dpxBits\":{\"type\":\"integer\"},\"index\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"doc_inspect", "Document state as JSON: layer tree (top to bottom), history, selection, active layer.",
     true, false,
     "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"doc_render_preview", "Render the flattened document and return it as a PNG image.", true, false,
     "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"max_side\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"render_preview", "Render a bounded flattened PNG preview.", true, false,
     "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"},\"max_side\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"doc_select", "Make the document at `index` active.", false, false,
     "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"required\":[\"index\"],\"additionalProperties\":false}"},
    {"doc_close", "Close a document (default: the active one) without saving.", false, false,
     "{\"type\":\"object\",\"properties\":{\"index\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"command_list", "List engine commands: id, label, menu path, shortcut, parameter doc, enabled now.",
     true, false,
     "{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"},\"enabled_only\":{\"type\":\"boolean\"}},\"additionalProperties\":false}"},
    {"command_run", "Run an engine command by id with JSON params. Returns the command's JSON result.",
     false, false,
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"},\"params\":{\"type\":\"object\"},\"wait\":{\"type\":\"boolean\"}},\"required\":[\"id\"],\"additionalProperties\":false}"},
    {"jobs_list", "List background jobs with progress, then the last few that ended.", true, false,
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"jobs_cancel", "Cancel a background job by id (or every running job when `job` is omitted).", false,
     false,
     "{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"command_batch", "Run several engine commands in one call, in order.", false, false,
     "{\"type\":\"object\",\"properties\":{\"steps\":{\"type\":\"array\"},\"stop_on_error\":{\"type\":\"boolean\"}},\"required\":[\"steps\"],\"additionalProperties\":false}"},
    {"ui_inspect", "Bridge mode: full UI state of the live app.", true, false,
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}"},
    {"ui_screenshot", "Bridge mode: screenshot of the live app window as PNG.", true, false,
     "{\"type\":\"object\",\"properties\":{\"max_side\":{\"type\":\"integer\"}},\"additionalProperties\":false}"},
    {"ui_pointer", "Bridge mode: send pointer events to the active tool.", false, false,
     "{\"type\":\"object\",\"properties\":{\"events\":{\"type\":\"array\"},\"modifiers\":{\"type\":\"object\"},\"button\":{\"type\":\"string\"}},\"required\":[\"events\"],\"additionalProperties\":false}"},
    {"ui_menu_invoke", "Bridge mode: activate a menu item by id.", false, false,
     "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"],\"additionalProperties\":false}"},
    {"ui_set", "Bridge mode: change UI state (tool, panels, dock, zoom, theme, …).", false, false,
     "{\"type\":\"object\",\"properties\":{\"fields\":{\"type\":\"object\"}},\"required\":[\"fields\"],\"additionalProperties\":false}"},
    {"control_call", "Bridge mode: call any control-protocol method with raw params.", false, false,
     "{\"type\":\"object\",\"properties\":{\"method\":{\"type\":\"string\"},\"params\":{\"type\":\"object\"}},\"required\":[\"method\"],\"additionalProperties\":false}"},
};
#define NTOOLS (sizeof TOOLS / sizeof TOOLS[0])

static const tool_def *find_tool(const char *name)
{
    for (size_t i = 0; i < NTOOLS; i++) {
        if (strcmp(TOOLS[i].name, name) == 0) {
            return &TOOLS[i];
        }
    }
    return NULL;
}

/* ---- CallToolResult builders (into `doc`) ---- */

/* {content:[{type:"text",text:<compact json of value, budget-checked>}], isError:false|true}. */
static yyjson_mut_val *result_text(yyjson_mut_doc *doc, const char *text, bool is_error)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_val *c = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, c, "type", "text");
    yyjson_mut_obj_add_strcpy(doc, c, "text", text);
    yyjson_mut_arr_append(arr, c);
    yyjson_mut_obj_add_val(doc, o, "content", arr);
    yyjson_mut_obj_add_bool(doc, o, "isError", is_error);
    return o;
}

static yyjson_mut_val *result_error(yyjson_mut_doc *doc, const char *msg)
{
    return result_text(doc, msg, true);
}

/* Success: serialise `value` compact (budget-checked) and embed as text content. */
static yyjson_mut_val *result_value(yyjson_mut_doc *doc, const pc_json_value *value)
{
    pc_buf buf = PC_BUF_INIT;
    pc_error err;
    pc_error_clear(&err);
    yyjson_mut_val *root = pc_json_value_root(value);
    yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *copy = tmp && root ? yyjson_mut_val_mut_copy(tmp, root) : NULL;
    if (tmp) {
        yyjson_mut_doc_set_root(tmp, copy ? copy : yyjson_mut_null(tmp));
    }
    pc_status rc = tmp ? pc_auto_json_bytes(yyjson_mut_doc_get_root(tmp), &buf, &err) : PC_ERR_NOMEM;
    yyjson_mut_doc_free(tmp);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        char m[512];
        snprintf(m, sizeof m, "%s; operation may have completed", err.msg[0] ? err.msg : "response too large");
        return result_error(doc, m);
    }
    yyjson_mut_val *o = result_text(doc, pc_buf_cstr(&buf), false);
    pc_buf_fini(&buf);
    return o;
}

/* Image result: content=[{type:image,data,mimeType},{type:text,text:note}]. */
static yyjson_mut_val *result_image(yyjson_mut_doc *doc, const uint8_t *png, size_t len, const char *note)
{
    pc_error err;
    pc_error_clear(&err);
    if (pc_auto_check_png(len, &err) != PC_OK) {
        return result_error(doc, err.msg);
    }
    pc_buf b64 = PC_BUF_INIT;
    if (pc_base64_encode(png, len, &b64, &err) != PC_OK) {
        pc_buf_fini(&b64);
        return result_error(doc, err.msg);
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_val *img = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, img, "type", "image");
    yyjson_mut_obj_add_strcpy(doc, img, "data", pc_buf_cstr(&b64));
    yyjson_mut_obj_add_strcpy(doc, img, "mimeType", "image/png");
    yyjson_mut_arr_append(arr, img);
    yyjson_mut_val *txt = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, txt, "type", "text");
    yyjson_mut_obj_add_strcpy(doc, txt, "text", note);
    yyjson_mut_arr_append(arr, txt);
    yyjson_mut_obj_add_val(doc, o, "content", arr);
    yyjson_mut_obj_add_bool(doc, o, "isError", false);
    pc_buf_fini(&b64);
    return o;
}

/* ---- backend helpers ---- */

/* Run a control method on the active backend; *out gets the owned result or *err the message. */
static pc_status backend_handle(pc_auto_mcp *m, const char *method, yyjson_val *params, bool read,
                                pc_json_value *out, pc_error *err)
{
    if (m->headless) {
        return pc_headless_handle(m->headless, method, params, out, err);
    }
    return pc_auto_bridge_call(m->bridge, method, params, read, out, err);
}

/* Build an immutable params doc {command:id, params:<p or null>, wait:w}; caller frees *holder. */
static yyjson_val *execute_params(const char *id, yyjson_val *p, bool wait, yyjson_doc **holder)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        *holder = NULL;
        return NULL;
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "command", id);
    yyjson_mut_obj_add_val(d, o, "params", p ? yyjson_val_mut_copy(d, p) : yyjson_mut_null(d));
    yyjson_mut_obj_add_bool(d, o, "wait", wait);
    yyjson_mut_doc_set_root(d, o);
    yyjson_doc *immut = yyjson_mut_doc_imut_copy(d, NULL);
    yyjson_mut_doc_free(d);
    *holder = immut;
    return immut ? yyjson_doc_get_root(immut) : NULL;
}

/* ---- tool dispatch ---- */

static yyjson_mut_val *tool_call(pc_auto_mcp *m, const char *name, yyjson_val *args, yyjson_mut_doc *out)
{
    const tool_def *t = find_tool(name);
    if (!t) {
        char msg[128];
        snprintf(msg, sizeof msg, "unknown tool `%s`", name);
        return result_error(out, msg);
    }
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_error_clear(&err);
    pc_status rc;
    bool handled = true;

    if (strcmp(name, "session_list") == 0) {
        rc = backend_handle(m, m->headless ? "session.list" : "ui.inspect", NULL, true, &result, &err);
    } else if (strcmp(name, "doc_open") == 0) {
        rc = backend_handle(m, m->headless ? "doc.open" : "app.open", args, false, &result, &err);
    } else if (strcmp(name, "doc_new") == 0) {
        rc = backend_handle(m, "doc.new", args, false, &result, &err);
        if (!m->headless) {
            yyjson_doc *holder = NULL;
            yyjson_val *p = execute_params("file.new", args, true, &holder);
            pc_json_value_fini(&result);
            rc = pc_auto_bridge_call(m->bridge, "engine.execute", p, false, &result, &err);
            yyjson_doc_free(holder);
        }
    } else if (strcmp(name, "doc_inspect") == 0) {
        if (m->headless) {
            /* Active-none special case. */
            size_t active = 0;
            bool has_index = pc_json_get(args, "index") != NULL;
            if (!has_index && !pc_engine_session_active_index(m->headless->session, &active)) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                yyjson_mut_val *o = yyjson_mut_obj(d);
                yyjson_mut_obj_add_null(d, o, "document");
                pc_json_value sess = PC_JSON_VALUE_INIT;
                (void)pc_headless_session_list(m->headless, &sess, NULL);
                yyjson_mut_val *sr = pc_json_value_root(&sess);
                yyjson_mut_obj_add_val(d, o, "session", sr ? yyjson_mut_val_mut_copy(d, sr) : yyjson_mut_null(d));
                yyjson_mut_doc_set_root(d, o);
                rc = pc_json_value_from_mut(o, &result, &err);
                yyjson_mut_doc_free(d);
                pc_json_value_fini(&sess);
            } else {
                rc = pc_headless_handle(m->headless, "doc.inspect", args, &result, &err);
            }
        } else {
            rc = backend_handle(m, "engine.execute", args, true, &result, &err); /* approximate */
        }
    } else if (strcmp(name, "doc_save") == 0 || strcmp(name, "doc_export") == 0) {
        if (strcmp(name, "doc_export") == 0 && !pc_json_get(args, "path")) {
            rc = pc_auto_bad(&err, "doc_export needs `path`");
        } else {
            rc = backend_handle(m, m->headless ? "doc.save" : "app.save", args, false, &result, &err);
        }
    } else if (strcmp(name, "doc_place") == 0) {
        rc = backend_handle(m, "doc.place", args, false, &result, &err);
    } else if (strcmp(name, "doc_export_layers") == 0) {
        rc = backend_handle(m, "doc.exportLayers", args, false, &result, &err);
    } else if (strcmp(name, "doc_export_data_sets") == 0) {
        rc = backend_handle(m, "doc.exportDataSets", args, false, &result, &err);
    } else if (strcmp(name, "doc_import_data_sets") == 0) {
        rc = backend_handle(m, "doc.importDataSets", args, false, &result, &err);
    } else if (strcmp(name, "command_run") == 0) {
        const char *id = pc_json_as_str(pc_json_get(args, "id"), NULL);
        if (!id) {
            rc = pc_auto_bad(&err, "command_run needs `id`");
        } else {
            bool wait = true, b;
            if (pc_json_as_bool(pc_json_get(args, "wait"), &b)) {
                wait = b;
            }
            if (m->headless) {
                rc = pc_headless_command_start(m->headless, id, pc_json_get(args, "params"), wait, &result, &err);
            } else {
                yyjson_doc *holder = NULL;
                yyjson_val *p = execute_params(id, pc_json_get(args, "params"), wait, &holder);
                rc = pc_auto_bridge_call(m->bridge, "engine.execute", p, false, &result, &err);
                yyjson_doc_free(holder);
            }
        }
    } else if (strcmp(name, "jobs_list") == 0) {
        rc = backend_handle(m, "jobs.list", NULL, true, &result, &err);
    } else if (strcmp(name, "jobs_cancel") == 0) {
        rc = backend_handle(m, "jobs.cancel", args, false, &result, &err);
    } else if (strcmp(name, "command_list") == 0) {
        if (m->headless) {
            rc = pc_headless_command_list(m->headless, &result, &err);
        } else {
            rc = pc_auto_bridge_call(m->bridge, "engine.commands", NULL, true, &result, &err);
        }
        /* enabled_only / filter are applied client-side in the Rust tool; left to the caller here. */
    } else if (strcmp(name, "command_batch") == 0) {
        if (m->headless) {
            rc = pc_headless_batch(m->headless, args, pc_auto_batch_budget_escaped(), &result, &err);
        } else {
            rc = pc_auto_bad(&err, "command_batch over a bridge is not supported in this build");
        }
    } else if (strcmp(name, "doc_render_preview") == 0 || strcmp(name, "render_preview") == 0) {
        if (m->headless) {
            uint64_t ms = 1024;
            (void)pc_json_as_u64(pc_json_get(args, "max_side"), &ms);
            int64_t idx = -1;
            uint64_t iv;
            if (pc_json_as_u64(pc_json_get(args, "index"), &iv) && iv <= (uint64_t)INT64_MAX) {
                idx = (int64_t)iv;
            }
            pc_buf png = PC_BUF_INIT;
            if (pc_headless_render_png(m->headless, idx, (uint32_t)ms, &png, &err) == PC_OK) {
                char note[64];
                snprintf(note, sizeof note, "flattened preview (max side %llu)", (unsigned long long)ms);
                yyjson_mut_val *o = result_image(out, png.data, png.len, note);
                pc_buf_fini(&png);
                return o;
            }
            pc_buf_fini(&png);
            return result_error(out, err.msg);
        }
        rc = pc_auto_bad(&err, "render preview needs headless mode");
    } else if (strcmp(name, "doc_select") == 0) {
        if (m->headless) {
            rc = pc_headless_handle(m->headless, "doc.select", args, &result, &err);
        } else {
            rc = pc_auto_bad(&err, "`doc_select` is only available in headless mode");
        }
    } else if (strcmp(name, "doc_close") == 0) {
        if (m->headless) {
            rc = pc_headless_handle(m->headless, "doc.close", args, &result, &err);
        } else {
            rc = pc_auto_bad(&err, "`doc_close` is only available in headless mode");
        }
    } else if (strncmp(name, "ui_", 3) == 0 || strcmp(name, "control_call") == 0) {
        if (!m->bridge) {
            rc = pc_auto_bad(&err, "`%s` drives the live GUI and needs bridge mode", name);
        } else if (strcmp(name, "ui_inspect") == 0) {
            rc = pc_auto_bridge_call(m->bridge, "ui.inspect", NULL, true, &result, &err);
        } else if (strcmp(name, "ui_menu_invoke") == 0) {
            rc = pc_auto_bridge_call(m->bridge, "ui.menu.invoke", args, false, &result, &err);
        } else if (strcmp(name, "ui_set") == 0) {
            rc = pc_auto_bridge_call(m->bridge, "ui.set", pc_json_get(args, "fields"), false, &result, &err);
        } else if (strcmp(name, "ui_pointer") == 0) {
            rc = pc_auto_bridge_call(m->bridge, "ui.pointer", args, false, &result, &err);
        } else if (strcmp(name, "control_call") == 0) {
            const char *method = pc_json_as_str(pc_json_get(args, "method"), NULL);
            if (!method) {
                rc = pc_auto_bad(&err, "control_call needs `method`");
            } else {
                rc = pc_auto_bridge_call(m->bridge, method, pc_json_get(args, "params"), false, &result, &err);
            }
        } else { /* ui_screenshot */
            rc = pc_auto_bridge_call(m->bridge, "ui.screenshot", NULL, true, &result, &err);
        }
    } else {
        handled = false;
        rc = pc_auto_bad(&err, "tool `%s` not implemented", name);
    }
    (void)handled;
    yyjson_mut_val *o;
    if (rc == PC_OK) {
        o = result_value(out, &result);
    } else {
        o = result_error(out, err.msg);
    }
    pc_json_value_fini(&result);
    return o;
}

/* ---- JSON-RPC envelope ---- */

static pc_status write_message(int fd, yyjson_mut_val *root, pc_error *err)
{
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = pc_auto_json_bytes(root, &buf, err);
    if (rc == PC_OK) {
        rc = pc_buf_append_byte(&buf, '\n', err);
    }
    if (rc == PC_OK) {
        rc = pc_auto_write_all(fd, false, buf.data, buf.len, err);
    }
    pc_buf_fini(&buf);
    return rc;
}

/* Build and send {jsonrpc, id, result:<result or null>}. Consumes nothing; `result` copied. */
static pc_status send_result(int fd, yyjson_val *id, yyjson_mut_val *result, yyjson_mut_doc *doc,
                             pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, o, "id", id ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    yyjson_mut_obj_add_val(doc, o, "result", result ? result : yyjson_mut_null(doc));
    yyjson_mut_doc_set_root(doc, o);
    return write_message(fd, o, err);
}

static pc_status send_rpc_error(int fd, yyjson_val *id, int code, const char *message, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val(doc, o, "id", id ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    yyjson_mut_val *e = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, e, "code", code);
    yyjson_mut_obj_add_strcpy(doc, e, "message", message);
    yyjson_mut_obj_add_val(doc, o, "error", e);
    yyjson_mut_doc_set_root(doc, o);
    pc_status rc = write_message(fd, o, err);
    yyjson_mut_doc_free(doc);
    return rc;
}

/* tools/list result. */
static yyjson_mut_val *build_tools_list(yyjson_mut_doc *doc)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (size_t i = 0; i < NTOOLS; i++) {
        const tool_def *t = &TOOLS[i];
        yyjson_mut_val *tv = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, tv, "name", t->name);
        /* Title: capitalised, underscores to spaces. */
        char title[64];
        size_t n = 0;
        for (const char *p = t->name; *p && n + 1 < sizeof title; p++) {
            char c = *p == '_' ? ' ' : *p;
            if (n == 0 && c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            title[n++] = c;
        }
        title[n] = '\0';
        yyjson_mut_obj_add_strcpy(doc, tv, "title", title);
        yyjson_mut_obj_add_strcpy(doc, tv, "description", t->description);
        yyjson_doc *sd = NULL;
        if (pc_json_parse(t->schema, strlen(t->schema), &sd, NULL) == PC_OK && sd) {
            yyjson_mut_obj_add_val(doc, tv, "inputSchema",
                                   yyjson_val_mut_copy(doc, yyjson_doc_get_root(sd)));
            yyjson_doc_free(sd);
        }
        yyjson_mut_val *ann = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, ann, "title", title);
        yyjson_mut_obj_add_bool(doc, ann, "readOnlyHint", t->read);
        yyjson_mut_obj_add_bool(doc, ann, "destructiveHint", !t->read && !t->additive);
        yyjson_mut_obj_add_bool(doc, ann, "idempotentHint", t->read);
        yyjson_mut_obj_add_bool(doc, ann, "openWorldHint", false);
        yyjson_mut_obj_add_val(doc, tv, "annotations", ann);
        yyjson_mut_arr_append(arr, tv);
    }
    yyjson_mut_obj_add_val(doc, o, "tools", arr);
    return o;
}

/* Handle one parsed JSON-RPC request; sends a response unless it is a notification. */
static pc_status dispatch(pc_auto_mcp *m, int out_fd, yyjson_val *root, pc_error *err)
{
    yyjson_val *id = pc_json_get(root, "id");
    const char *method = pc_json_as_str(pc_json_get(root, "method"), NULL);
    yyjson_val *params = pc_json_get(root, "params");
    if (!method) {
        return id ? send_rpc_error(out_fd, id, -32600, "missing method", err) : PC_OK;
    }
    bool is_notification = id == NULL;

    if (strcmp(method, "initialize") == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *r = yyjson_mut_obj(doc);
        const char *pv = pc_json_as_str(pc_json_get(params, "protocolVersion"), NULL);
        yyjson_mut_obj_add_strcpy(doc, r, "protocolVersion", pv ? pv : MCP_PROTOCOL);
        yyjson_mut_val *caps = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, caps, "tools", yyjson_mut_obj(doc));
        yyjson_mut_obj_add_val(doc, caps, "resources", yyjson_mut_obj(doc));
        yyjson_mut_obj_add_val(doc, r, "capabilities", caps);
        yyjson_mut_val *info = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, info, "name", "photocraft");
        yyjson_mut_obj_add_strcpy(doc, info, "version", pc_engine_version);
        yyjson_mut_obj_add_val(doc, r, "serverInfo", info);
        yyjson_mut_obj_add_strcpy(doc, r, "instructions", INSTRUCTIONS);
        pc_status rc = send_result(out_fd, id, r, doc, err);
        yyjson_mut_doc_free(doc);
        return rc;
    }
    if (strncmp(method, "notifications/", 14) == 0) {
        return PC_OK; /* notifications get no response */
    }
    if (strcmp(method, "ping") == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        pc_status rc = send_result(out_fd, id, yyjson_mut_obj(doc), doc, err);
        yyjson_mut_doc_free(doc);
        return rc;
    }
    if (strcmp(method, "tools/list") == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        pc_status rc = send_result(out_fd, id, build_tools_list(doc), doc, err);
        yyjson_mut_doc_free(doc);
        return rc;
    }
    if (strcmp(method, "tools/call") == 0) {
        const char *name = pc_json_as_str(pc_json_get(params, "name"), NULL);
        if (!name) {
            return is_notification ? PC_OK : send_rpc_error(out_fd, id, -32602, "missing tool name", err);
        }
        yyjson_val *args = pc_json_get(params, "arguments");
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *result = tool_call(m, name, args, doc);
        pc_status rc = send_result(out_fd, id, result, doc, err);
        yyjson_mut_doc_free(doc);
        return rc;
    }
    if (strcmp(method, "resources/list") == 0) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = yyjson_mut_obj(doc);
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        const char *uris[2] = {"photocraft://document", "photocraft://commands"};
        const char *names[2] = {"document", "commands"};
        for (int i = 0; i < 2; i++) {
            yyjson_mut_val *res = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, res, "uri", uris[i]);
            yyjson_mut_obj_add_strcpy(doc, res, "name", names[i]);
            yyjson_mut_obj_add_strcpy(doc, res, "mimeType", "application/json");
            yyjson_mut_arr_append(arr, res);
        }
        yyjson_mut_obj_add_val(doc, r, "resources", arr);
        pc_status rc = send_result(out_fd, id, r, doc, err);
        yyjson_mut_doc_free(doc);
        return rc;
    }
    if (strcmp(method, "resources/read") == 0) {
        const char *uri = pc_json_as_str(pc_json_get(params, "uri"), NULL);
        pc_json_value val = PC_JSON_VALUE_INIT;
        pc_error e;
        pc_error_clear(&e);
        pc_status vr;
        if (uri && strcmp(uri, "photocraft://document") == 0) {
            vr = backend_handle(m, m->headless ? "doc.inspect" : "ui.inspect", NULL, true, &val, &e);
        } else if (uri && strcmp(uri, "photocraft://commands") == 0) {
            vr = m->headless ? pc_headless_command_list(m->headless, &val, &e)
                             : pc_auto_bridge_call(m->bridge, "engine.commands", NULL, true, &val, &e);
        } else {
            return send_rpc_error(out_fd, id, -32002, "unknown resource", err);
        }
        if (vr != PC_OK) {
            return send_rpc_error(out_fd, id, -32603, e.msg, err);
        }
        pc_buf text = PC_BUF_INIT;
        (void)pc_json_write(pc_json_value_root(&val), 0, &text, NULL);
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *r = yyjson_mut_obj(doc);
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        yyjson_mut_val *c = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, c, "uri", uri);
        yyjson_mut_obj_add_strcpy(doc, c, "mimeType", "application/json");
        yyjson_mut_obj_add_strcpy(doc, c, "text", pc_buf_cstr(&text));
        yyjson_mut_arr_append(arr, c);
        yyjson_mut_obj_add_val(doc, r, "contents", arr);
        pc_status rc = send_result(out_fd, id, r, doc, err);
        yyjson_mut_doc_free(doc);
        pc_buf_fini(&text);
        pc_json_value_fini(&val);
        return rc;
    }
    if (is_notification) {
        return PC_OK;
    }
    char msg[128];
    snprintf(msg, sizeof msg, "method not found: %s", method);
    return send_rpc_error(out_fd, id, -32601, msg, err);
}

pc_status pc_auto_mcp_serve(pc_auto_mcp *m, int in_fd, int out_fd, pc_error *err)
{
    pc_auto_reader r;
    pc_auto_reader_init(&r, in_fd, false);
    pc_buf line = PC_BUF_INIT;
    for (;;) {
        pc_auto_line kind = pc_auto_read_bounded_line(&r, &line, err);
        if (kind == PC_AUTO_LINE_EOF) {
            pc_buf_fini(&line);
            return PC_OK;
        }
        if (kind == PC_AUTO_LINE_TOO_LONG) {
            pc_buf_fini(&line);
            return pc_auto_bad(err, "request exceeds %zu bytes", PC_AUTO_MAX_REQUEST_BYTES);
        }
        if (kind == PC_AUTO_LINE_INVALID_UTF8) {
            pc_buf_fini(&line);
            return pc_auto_bad(err, "request is not valid UTF-8");
        }
        if (kind == PC_AUTO_LINE_IOERR) {
            pc_buf_fini(&line);
            return err ? err->code : PC_ERR_IO;
        }
        /* blank line: skip */
        bool blank = true;
        for (size_t i = 0; i < line.len; i++) {
            unsigned char c = line.data[i];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                blank = false;
                break;
            }
        }
        if (blank) {
            continue;
        }
        yyjson_doc *req = NULL;
        pc_error pe;
        pc_error_clear(&pe);
        if (pc_json_parse((const char *)line.data, line.len, &req, &pe) != PC_OK || !req) {
            char msg[512];
            snprintf(msg, sizeof msg, "parse error: %s", pe.msg[0] ? pe.msg : "invalid");
            if (send_rpc_error(out_fd, NULL, -32700, msg, err) != PC_OK) {
                pc_buf_fini(&line);
                return err ? err->code : PC_ERR_IO;
            }
            continue;
        }
        pc_status rc = dispatch(m, out_fd, yyjson_doc_get_root(req), err);
        yyjson_doc_free(req);
        if (rc != PC_OK) {
            pc_buf_fini(&line);
            return rc;
        }
    }
}
