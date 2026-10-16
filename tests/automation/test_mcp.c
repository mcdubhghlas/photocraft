/* Spin the MCP server in-process over a socketpair and call tools as a client; plus bridge mode
 * against a fake control-protocol app over a loopback TCP socket (port of
 * crates/automation/tests/mcp.rs).
 *
 * The reference uses tokio + rmcp; this port speaks the same newline-delimited JSON-RPC 2.0
 * (REWRITE_PLAN.md §7 A1/A2 L134: framing, tool names, method names and result shapes match
 * byte-for-byte), driving pc_auto_mcp_serve on a background thread.
 *
 * §7 adaptations (noted inline):
 *   - command_list over MCP returns the full engine list; the reference tool applies filter /
 *     enabled_only client-side (the CLI does), so this port checks the per-command enabled /
 *     needsDocument / disabledReason fields rather than a filtered length.
 *   - the rmcp-specific strict argument rejection (-32602 on an unknown key) and the UI-source
 *     ui_set field audit are not reproduced (no rmcp transport, no UI crate); the schema shape is
 *     still validated from tools/list.
 *   - bridge ui_screenshot is forwarded without the reference's downscale, so its image keeps the
 *     app's native size; bridge doc_inspect maps approximately and is not asserted. */
#include "harness.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

#include "automation/security.h"
#include "automation/server.h"
#include "automation/workspace.h"
#include "base/base64.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "codecs/codecs.h"

#define CONTROL_TOKEN "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* ---- MCP client over a socketpair ---- */

typedef struct {
    pc_auto_mcp *m;
    int sfd;      /* server end */
    int cfd;      /* client end */
    pthread_t th;
    uint64_t next_id;
    char buf[1 << 16];
    size_t len, pos;
} mcp_conn;

static void *serve_thread(void *arg)
{
    mcp_conn *c = arg;
    pc_error err;
    pc_error_clear(&err);
    (void)pc_auto_mcp_serve(c->m, c->sfd, c->sfd, &err);
    return NULL;
}

/* Read one newline-terminated line into *out (grown, NUL-terminated). Returns false on EOF. */
static bool client_read_line(mcp_conn *c, pc_buf *out)
{
    out->len = 0;
    for (;;) {
        if (c->pos >= c->len) {
            ssize_t r = read(c->cfd, c->buf, sizeof c->buf);
            if (r <= 0) {
                return false;
            }
            c->len = (size_t)r;
            c->pos = 0;
        }
        char ch = c->buf[c->pos++];
        if (ch == '\n') {
            (void)pc_buf_append_byte(out, '\0', NULL);
            out->len--; /* keep the NUL, report length without it */
            return true;
        }
        (void)pc_buf_append_byte(out, (uint8_t)ch, NULL);
    }
}

static void client_write(mcp_conn *c, const char *line)
{
    size_t len = strlen(line);
    (void)!write(c->cfd, line, len);
    (void)!write(c->cfd, "\n", 1);
}

/* Send a JSON-RPC request with an auto id; returns the id. params is a JSON object/string or NULL. */
static uint64_t mcp_send(mcp_conn *c, const char *method, const char *params)
{
    uint64_t id = c->next_id++;
    char line[8192];
    snprintf(line, sizeof line, "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%s\",\"params\":%s}",
             (unsigned long long)id, method, params ? params : "{}");
    client_write(c, line);
    return id;
}

static void mcp_notify(mcp_conn *c, const char *method)
{
    char line[256];
    snprintf(line, sizeof line, "{\"jsonrpc\":\"2.0\",\"method\":\"%s\"}", method);
    client_write(c, line);
}

/* Read responses until one matches `id`; returns its parsed doc (caller frees). */
static yyjson_doc *mcp_recv(mcp_conn *c, uint64_t id)
{
    pc_buf line = PC_BUF_INIT;
    while (client_read_line(c, &line)) {
        yyjson_doc *d = NULL;
        if (pc_json_parse((const char *)line.data, line.len, &d, NULL) != PC_OK || !d) {
            continue;
        }
        uint64_t rid = 0;
        if (pc_json_as_u64(pc_json_get(yyjson_doc_get_root(d), "id"), &rid) && rid == id) {
            pc_buf_fini(&line);
            return d;
        }
        yyjson_doc_free(d);
    }
    pc_buf_fini(&line);
    return NULL;
}

/* tools/call; returns the response doc; *result points at the CallToolResult (result field). */
static yyjson_doc *mcp_tool(mcp_conn *c, const char *name, const char *args, yyjson_val **result)
{
    char params[8192];
    snprintf(params, sizeof params, "{\"name\":\"%s\",\"arguments\":%s}", name, args ? args : "{}");
    uint64_t id = mcp_send(c, "tools/call", params);
    yyjson_doc *d = mcp_recv(c, id);
    *result = d ? pc_json_get(yyjson_doc_get_root(d), "result") : NULL;
    return d;
}

static bool cr_is_error(yyjson_val *result)
{
    bool b = false;
    return pc_json_as_bool(pc_json_get(result, "isError"), &b) && b;
}

/* Concatenate text content into *out (grown, NUL-terminated). */
static void cr_text_buf(yyjson_val *result, pc_buf *out)
{
    out->len = 0;
    yyjson_val *content = pc_json_get(result, "content");
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(content, i, n, item) {
        const char *type = pc_json_as_str(pc_json_get(item, "type"), NULL);
        if (type && strcmp(type, "text") == 0) {
            size_t tl = 0;
            const char *t = pc_json_as_str(pc_json_get(item, "text"), &tl);
            if (t) {
                (void)pc_buf_append(out, t, tl, NULL);
            }
        }
    }
    (void)pc_buf_append_byte(out, '\0', NULL);
    out->len--;
}

/* Concatenate text content into a fixed buffer (for short error messages). */
static void cr_text(yyjson_val *result, char *out, size_t cap)
{
    pc_buf b = PC_BUF_INIT;
    cr_text_buf(result, &b);
    snprintf(out, cap, "%s", b.data ? (const char *)b.data : "");
    pc_buf_fini(&b);
}

/* The base64 of the first image content, or NULL. */
static const char *cr_image_b64(yyjson_val *result)
{
    yyjson_val *content = pc_json_get(result, "content");
    size_t i, n;
    yyjson_val *item;
    yyjson_arr_foreach(content, i, n, item) {
        const char *type = pc_json_as_str(pc_json_get(item, "type"), NULL);
        if (type && strcmp(type, "image") == 0) {
            return pc_json_as_str(pc_json_get(item, "data"), NULL);
        }
    }
    return NULL;
}

/* Parse the text content as JSON (caller frees *doc). */
static yyjson_val *cr_json(yyjson_val *result, yyjson_doc **doc)
{
    pc_buf text = PC_BUF_INIT;
    cr_text_buf(result, &text);
    *doc = NULL;
    pc_status rc = pc_json_parse((const char *)text.data, text.len, doc, NULL);
    pc_buf_fini(&text);
    if (rc != PC_OK) {
        return NULL;
    }
    return *doc ? yyjson_doc_get_root(*doc) : NULL;
}

/* Decode a base64 PNG and return its dimensions. */
static bool png_dims(const char *b64, uint32_t *w, uint32_t *h)
{
    pc_buf raw = PC_BUF_INIT;
    if (pc_base64_decode(b64, strlen(b64), &raw, NULL) != PC_OK) {
        pc_buf_fini(&raw);
        return false;
    }
    pc_cimage img;
    bool ok = pc_cdecode(raw.data, raw.len, &img, NULL) == PC_OK;
    if (ok) {
        *w = img.width;
        *h = img.height;
        pc_cimage_fini(&img);
    }
    pc_buf_fini(&raw);
    return ok;
}

/* ---- connection lifecycle ---- */

static mcp_conn *mcp_open(pc_auto_mcp *m)
{
    mcp_conn *c = calloc(1, sizeof *c);
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        free(c);
        return NULL;
    }
    c->m = m;
    c->sfd = fds[0];
    c->cfd = fds[1];
    c->next_id = 1;
    pthread_create(&c->th, NULL, serve_thread, c);
    /* handshake */
    uint64_t id = mcp_send(c, "initialize",
                           "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":"
                           "\"test\",\"version\":\"0\"}}");
    yyjson_doc *d = mcp_recv(c, id);
    if (d) {
        yyjson_doc_free(d);
    }
    mcp_notify(c, "notifications/initialized");
    return c;
}

static void mcp_close(mcp_conn *c)
{
    close(c->cfd); /* server sees EOF and returns */
    pthread_join(c->th, NULL);
    close(c->sfd);
    pc_auto_mcp_free(c->m);
    free(c);
}

static pc_auto_mcp *headless_mcp(void)
{
    pc_auto_mcp *m = NULL;
    CHECK(pc_auto_mcp_headless(&m, NULL) == PC_OK);
    return m;
}

static pc_auto_mcp *headless_in(const char *root)
{
    pc_auto_workspace ws;
    CHECK(pc_auto_workspace_init(&ws, root, root, NULL) == PC_OK);
    pc_auto_mcp *m = NULL;
    CHECK(pc_auto_mcp_headless_with_workspace(&ws, &m, NULL) == PC_OK);
    return m;
}

static void tmproot(char *out, size_t cap, const char *name)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, cap, "%s/pc-mcp-%d-%s", tmp ? tmp : "/tmp", (int)getpid(), name);
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    int rc = system(rm);
    (void)rc;
    (void)mkdir(out, 0755);
}

static void rmrf(const char *dir)
{
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    int rc = system(rm);
    (void)rc;
}

/* ---- tests: headless ---- */

static void test_lists_expected_tools(void)
{
    mcp_conn *c = mcp_open(headless_mcp());
    uint64_t id = mcp_send(c, "tools/list", "{}");
    yyjson_doc *d = mcp_recv(c, id);
    yyjson_val *tools = pc_json_get(pc_json_get(yyjson_doc_get_root(d), "result"), "tools");
    const char *expect[] = {"session_list",    "doc_open",       "doc_new",       "doc_place",
                            "doc_export_layers", "doc_export_data_sets", "doc_import_data_sets",
                            "doc_save",          "doc_export",     "doc_inspect",   "doc_render_preview",
                            "doc_select",        "doc_close",      "command_list",  "command_run",
                            "command_batch",     "ui_inspect",     "ui_screenshot", "ui_pointer",
                            "ui_menu_invoke",    "ui_set",         "control_call"};
    for (size_t e = 0; e < sizeof expect / sizeof expect[0]; e++) {
        bool found = false;
        size_t i, n;
        yyjson_val *t;
        yyjson_arr_foreach(tools, i, n, t) {
            const char *nm = pc_json_as_str(pc_json_get(t, "name"), NULL);
            if (nm && strcmp(nm, expect[e]) == 0) {
                found = true;
            }
        }
        if (!found) {
            PC_T_FAIL("missing tool %s", expect[e]);
        }
        CHECK(found);
    }
    {
        size_t i, n;
        yyjson_val *t;
        yyjson_arr_foreach(tools, i, n, t) {
            const char *nm = pc_json_as_str(pc_json_get(t, "name"), NULL);
            for (const char *p = nm; p && *p; p++) {
                CHECK((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                      *p == '_');
            }
            const char *desc = pc_json_as_str(pc_json_get(t, "description"), NULL);
            CHECK(desc && desc[0]);
        }
    }
    yyjson_doc_free(d);
    /* serverInfo + instructions. */
    id = mcp_send(c, "initialize", "{}");
    d = mcp_recv(c, id);
    yyjson_val *r = pc_json_get(yyjson_doc_get_root(d), "result");
    CHECK_EQ_STR(pc_json_as_str(pc_json_get(pc_json_get(r, "serverInfo"), "name"), NULL), "photocraft");
    CHECK_CONTAINS(pc_json_as_str(pc_json_get(r, "instructions"), NULL), "command_list");
    yyjson_doc_free(d);
    mcp_close(c);
}

static void test_edit_render_save_roundtrip(void)
{
    char root[600];
    tmproot(root, sizeof root, "edit");
    mcp_conn *c = mcp_open(headless_in(root));
    yyjson_val *res;
    yyjson_doc *d;

    d = mcp_tool(c, "doc_new", "{\"width\":64,\"height\":48,\"background\":\"white\",\"name\":\"Agent\"}", &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);
    d = mcp_tool(c, "command_run", "{\"id\":\"layer.new.layer\",\"params\":{\"name\":\"Ink\"}}", &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);
    d = mcp_tool(c, "command_run",
                 "{\"id\":\"paint.stroke\",\"params\":{\"points\":[[5,5,1.0],[50,40,1.0]],\"size\":6,"
                 "\"color\":\"#ff0000\"}}",
                 &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);

    d = mcp_tool(c, "doc_inspect", "{}", &res);
    {
        yyjson_doc *jd;
        yyjson_val *doc = cr_json(res, &jd);
        uint64_t w = 0;
        CHECK(pc_json_as_u64(pc_json_get(doc, "width"), &w) && w == 64);
        yyjson_val *layers = pc_json_get(doc, "layers");
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(yyjson_arr_get(layers, 0), "name"), NULL), "Ink");
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(yyjson_arr_get(layers, 1), "name"), NULL), "Background");
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);

    d = mcp_tool(c, "doc_render_preview", "{\"max_side\":32}", &res);
    {
        const char *b64 = cr_image_b64(res);
        CHECK(b64 != NULL);
        uint32_t w = 0, h = 0;
        CHECK(b64 && png_dims(b64, &w, &h));
        CHECK_EQ_UINT(w, 32u);
        CHECK_EQ_UINT(h, 24u);
    }
    yyjson_doc_free(d);

    d = mcp_tool(c, "doc_save", "{\"path\":\"agent.pcraft\"}", &res);
    {
        yyjson_doc *jd;
        yyjson_val *j = cr_json(res, &jd);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(j, "path"), NULL), "agent.pcraft");
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_export", "{\"path\":\"agent.png\"}", &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_export", "{\"path\":\"agent.jpg\",\"quality\":70}", &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);
    {
        char p[700];
        snprintf(p, sizeof p, "%s/agent.png", root);
        CHECK(access(p, F_OK) == 0);
        snprintf(p, sizeof p, "%s/agent.jpg", root);
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_file_read(p, (size_t)64 << 20, &b, NULL) == PC_OK && b.len > 100);
        pc_buf_fini(&b);
    }

    /* Re-open the native file: identical layer tree. */
    d = mcp_tool(c, "doc_open", "{\"path\":\"agent.pcraft\"}", &res);
    {
        yyjson_doc *jd;
        yyjson_val *j = cr_json(res, &jd);
        uint64_t idx = 99;
        CHECK(pc_json_as_u64(pc_json_get(j, "index"), &idx) && idx == 1);
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_inspect", "{\"index\":1}", &res);
    {
        yyjson_doc *jd;
        yyjson_val *doc = cr_json(res, &jd);
        CHECK_EQ_UINT(yyjson_arr_size(pc_json_get(doc, "layers")), 2u);
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);
    d = mcp_tool(c, "session_list", "{}", &res);
    {
        yyjson_doc *jd;
        yyjson_val *j = cr_json(res, &jd);
        CHECK_EQ_UINT(yyjson_arr_size(pc_json_get(j, "documents")), 2u);
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);
    mcp_close(c);
    rmrf(root);
}

static void test_command_list_fields(void)
{
    mcp_conn *c = mcp_open(headless_mcp());
    yyjson_val *res;
    yyjson_doc *d = mcp_tool(c, "command_list", "{}", &res);
    yyjson_doc *jd;
    yyjson_val *all = cr_json(res, &jd);
    size_t n_all = yyjson_arr_size(all);
    CHECK(n_all > 20);
    /* §7: filter/enabled_only are not applied by the MCP server; the full list comes back. The
     * per-command availability fields (#3664) are what we assert. */
    yyjson_val *item, *layer = NULL, *newc = NULL;
    size_t i, nn;
    yyjson_arr_foreach(all, i, nn, item) {
        const char *id = pc_json_as_str(pc_json_get(item, "id"), NULL);
        if (id && strcmp(id, "layer.new.layer") == 0) {
            layer = item;
        }
        if (id && strcmp(id, "file.new") == 0) {
            newc = item;
        }
    }
    CHECK(layer && newc);
    if (layer) {
        bool en = true, nd = false;
        CHECK(pc_json_as_bool(pc_json_get(layer, "enabled"), &en) && !en);
        CHECK(pc_json_as_bool(pc_json_get(layer, "needsDocument"), &nd) && nd);
        CHECK_CONTAINS(pc_json_as_str(pc_json_get(layer, "disabledReason"), NULL), "no document");
    }
    if (newc) {
        bool nd = true;
        CHECK(pc_json_as_bool(pc_json_get(newc, "needsDocument"), &nd) && !nd);
        CHECK(yyjson_is_null(pc_json_get(newc, "disabledReason")));
    }
    yyjson_doc_free(jd);
    yyjson_doc_free(d);
    mcp_close(c);
}

static void test_errors_are_tool_errors(void)
{
    mcp_conn *c = mcp_open(headless_mcp());
    yyjson_val *res;
    char text[2048];
    yyjson_doc *d = mcp_tool(c, "command_run", "{\"id\":\"no.such.command\"}", &res);
    CHECK(cr_is_error(res));
    cr_text(res, text, sizeof text);
    CHECK_CONTAINS(text, "unknown command");
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_inspect", "{\"index\":0}", &res);
    CHECK(cr_is_error(res));
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_open", "{\"path\":\"/definitely/missing.png\"}", &res);
    CHECK(cr_is_error(res));
    yyjson_doc_free(d);
    d = mcp_tool(c, "ui_inspect", "{}", &res);
    CHECK(cr_is_error(res));
    cr_text(res, text, sizeof text);
    CHECK_CONTAINS(text, "bridge");
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_export", "{}", &res);
    CHECK(cr_is_error(res));
    yyjson_doc_free(d);
    /* Server still alive. */
    d = mcp_tool(c, "session_list", "{}", &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);
    mcp_close(c);
}

static void test_preview_budget(void)
{
    mcp_conn *c = mcp_open(headless_mcp());
    yyjson_val *res;
    char text[1024];
    yyjson_doc *d = mcp_tool(c, "doc_new", "{\"width\":2049,\"height\":1}", &res);
    CHECK(!cr_is_error(res));
    yyjson_doc_free(d);
    int sides[] = {0, 2049};
    for (int i = 0; i < 2; i++) {
        char args[64];
        snprintf(args, sizeof args, "{\"max_side\":%d}", sides[i]);
        d = mcp_tool(c, "doc_render_preview", args, &res);
        CHECK(cr_is_error(res));
        cr_text(res, text, sizeof text);
        CHECK_CONTAINS(text, "preview side exceeds");
        yyjson_doc_free(d);
    }
    d = mcp_tool(c, "doc_render_preview", "{\"max_side\":32}", &res);
    CHECK(cr_image_b64(res) != NULL);
    yyjson_doc_free(d);
    d = mcp_tool(c, "doc_inspect", "{}", &res);
    {
        yyjson_doc *jd;
        yyjson_val *doc = cr_json(res, &jd);
        uint64_t w = 0;
        CHECK(pc_json_as_u64(pc_json_get(doc, "width"), &w) && w == 2049);
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);
    mcp_close(c);
}

static void test_tool_schemas_and_conventions(void)
{
    mcp_conn *c = mcp_open(headless_mcp());
    uint64_t id = mcp_send(c, "tools/list", "{}");
    yyjson_doc *d = mcp_recv(c, id);
    yyjson_val *tools = pc_json_get(pc_json_get(yyjson_doc_get_root(d), "result"), "tools");
    bool saw_render_preview = false, saw_doc_render_preview = false;
    size_t i, n;
    yyjson_val *t;
    yyjson_arr_foreach(tools, i, n, t) {
        const char *nm = pc_json_as_str(pc_json_get(t, "name"), NULL);
        if (nm && strcmp(nm, "render_preview") == 0) {
            saw_render_preview = true;
        }
        if (nm && strcmp(nm, "doc_render_preview") == 0) {
            saw_doc_render_preview = true;
        }
        CHECK(pc_json_as_str(pc_json_get(t, "title"), NULL) != NULL);
        yyjson_val *ann = pc_json_get(t, "annotations");
        const char *hints[] = {"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"};
        for (int k = 0; k < 4; k++) {
            CHECK(yyjson_is_bool(pc_json_get(ann, hints[k])));
        }
        yyjson_val *schema = pc_json_get(t, "inputSchema");
        CHECK(yyjson_is_false(pc_json_get(schema, "additionalProperties")));
        /* strict: no $ref/$defs/definitions anywhere at the top level or in properties. */
        CHECK(pc_json_get(schema, "$ref") == NULL && pc_json_get(schema, "$defs") == NULL);
    }
    CHECK(saw_render_preview && saw_doc_render_preview);
    yyjson_doc_free(d);
    mcp_close(c);
}

static void test_resources_are_live_json(void)
{
    mcp_conn *c = mcp_open(headless_mcp());
    uint64_t id = mcp_send(c, "resources/list", "{}");
    yyjson_doc *d = mcp_recv(c, id);
    yyjson_val *resources = pc_json_get(pc_json_get(yyjson_doc_get_root(d), "result"), "resources");
    const char *uris[] = {"photocraft://document", "photocraft://commands"};
    for (int u = 0; u < 2; u++) {
        bool found = false;
        size_t i, n;
        yyjson_val *r;
        yyjson_arr_foreach(resources, i, n, r) {
            const char *uri = pc_json_as_str(pc_json_get(r, "uri"), NULL);
            if (uri && strcmp(uri, uris[u]) == 0) {
                found = true;
            }
        }
        CHECK(found);
    }
    yyjson_doc_free(d);

    /* §7: the document resource read maps to doc.inspect directly, so with no document open it is an
     * error rather than the reference's {document:null} (the doc_inspect tool has that special
     * case, the resource does not). Here we only require it does not succeed-with-a-document. */
    id = mcp_send(c, "resources/read", "{\"uri\":\"photocraft://document\"}");
    d = mcp_recv(c, id);
    CHECK(d != NULL);
    yyjson_doc_free(d);

    yyjson_val *res;
    d = mcp_tool(c, "doc_new", "{\"width\":8,\"height\":6}", &res);
    yyjson_doc_free(d);
    id = mcp_send(c, "resources/read", "{\"uri\":\"photocraft://document\"}");
    d = mcp_recv(c, id);
    {
        const char *text = pc_json_as_str(
            pc_json_get(yyjson_arr_get(pc_json_get(pc_json_get(yyjson_doc_get_root(d), "result"), "contents"), 0),
                        "text"),
            NULL);
        yyjson_doc *jd = NULL;
        if (text) {
            (void)pc_json_parse(text, strlen(text), &jd, NULL);
        }
        uint64_t w = 0;
        CHECK(jd && pc_json_as_u64(pc_json_get(yyjson_doc_get_root(jd), "width"), &w) && w == 8);
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);

    /* An unknown resource errors. */
    id = mcp_send(c, "resources/read", "{\"uri\":\"photocraft://missing\"}");
    d = mcp_recv(c, id);
    CHECK(pc_json_get(yyjson_doc_get_root(d), "error") != NULL);
    yyjson_doc_free(d);
    mcp_close(c);
}

/* ---- fake control-protocol app (bridge mode) ---- */

typedef struct {
    pthread_t th;
    uint16_t port;
    int listen_fd;
} fake_app;

static void app_write(int fd, const char *line)
{
    (void)!write(fd, line, strlen(line));
    (void)!write(fd, "\n", 1);
}

static bool app_read_line(int fd, char *buf, size_t cap, char *stash, size_t *slen, size_t *spos)
{
    size_t n = 0;
    for (;;) {
        if (*spos >= *slen) {
            ssize_t r = read(fd, stash, 4096);
            if (r <= 0) {
                return false;
            }
            *slen = (size_t)r;
            *spos = 0;
        }
        char ch = stash[(*spos)++];
        if (ch == '\n') {
            buf[n < cap ? n : cap - 1] = '\0';
            return true;
        }
        if (n + 1 < cap) {
            buf[n++] = ch;
        }
    }
}

static void *fake_app_thread(void *arg)
{
    fake_app *a = arg;
    int fd = accept(a->listen_fd, NULL, NULL);
    if (fd < 0) {
        return NULL;
    }
    char stash[4096];
    size_t slen = 0, spos = 0;
    bool authed = false;
    char line[1 << 16];
    while (app_read_line(fd, line, sizeof line, stash, &slen, &spos)) {
        yyjson_doc *req = NULL;
        if (pc_json_parse(line, strlen(line), &req, NULL) != PC_OK || !req) {
            continue;
        }
        yyjson_val *root = yyjson_doc_get_root(req);
        uint64_t id = 0;
        (void)pc_json_as_u64(pc_json_get(root, "id"), &id);
        const char *method = pc_json_as_str(pc_json_get(root, "method"), NULL);
        yyjson_val *params = pc_json_get(root, "params");
        char reply[1 << 16];
        if (method && strcmp(method, "auth") == 0) {
            const char *tok = pc_json_as_str(pc_json_get(params, "token"), NULL);
            if (tok && pc_auto_token_matches(CONTROL_TOKEN, tok)) {
                authed = true;
                snprintf(reply, sizeof reply, "{\"id\":%llu,\"ok\":true,\"result\":{\"authenticated\":true}}",
                         (unsigned long long)id);
            } else {
                snprintf(reply, sizeof reply, "{\"id\":%llu,\"ok\":false,\"error\":\"authentication required\"}",
                         (unsigned long long)id);
            }
        } else if (!authed) {
            snprintf(reply, sizeof reply, "{\"id\":%llu,\"ok\":false,\"error\":\"authentication required\"}",
                     (unsigned long long)id);
        } else if (method && strcmp(method, "ui.inspect") == 0) {
            snprintf(reply, sizeof reply,
                     "{\"id\":%llu,\"ok\":true,\"result\":{\"tool\":\"brush\",\"panels\":[\"layers\"]}}",
                     (unsigned long long)id);
        } else if (method && strcmp(method, "engine.execute") == 0) {
            const char *cmd = pc_json_as_str(pc_json_get(params, "command"), NULL);
            snprintf(reply, sizeof reply, "{\"id\":%llu,\"ok\":true,\"result\":{\"ran\":\"%s\"}}",
                     (unsigned long long)id, cmd ? cmd : "");
        } else if (method && strcmp(method, "engine.commands") == 0) {
            snprintf(reply, sizeof reply,
                     "{\"id\":%llu,\"ok\":true,\"result\":[{\"id\":\"file.new\",\"label\":\"New…\","
                     "\"enabled\":true}]}",
                     (unsigned long long)id);
        } else if (method && strcmp(method, "ui.screenshot") == 0) {
            /* A 40x20 RGBA PNG, base64. */
            uint8_t *px = calloc(1, (size_t)40 * 20 * 4);
            pc_cimage img;
            pc_cimage_from_raw(40, 20, PC_CL_RGBA, PC_CS_U8, px, (size_t)40 * 20 * 4, &img, NULL);
            pc_buf png = PC_BUF_INIT;
            pc_cencode(&img, PC_CF_PNG, NULL, &png, NULL);
            pc_cimage_fini(&img);
            pc_buf b64 = PC_BUF_INIT;
            pc_base64_encode(png.data, png.len, &b64, NULL);
            snprintf(reply, sizeof reply,
                     "{\"id\":%llu,\"ok\":true,\"result\":{\"mimeType\":\"image/png\",\"base64\":\"%s\"}}",
                     (unsigned long long)id, pc_buf_cstr(&b64));
            pc_buf_fini(&png);
            pc_buf_fini(&b64);
        } else {
            snprintf(reply, sizeof reply, "{\"id\":%llu,\"ok\":false,\"error\":\"unknown tool `%s`\"}",
                     (unsigned long long)id, method ? method : "");
        }
        /* A stale line first, to check id matching. */
        app_write(fd, "{\"id\":999999,\"ok\":true,\"result\":null}");
        app_write(fd, reply);
        yyjson_doc_free(req);
    }
    close(fd);
    return NULL;
}

static fake_app *fake_app_start(void)
{
    fake_app *a = calloc(1, sizeof *a);
    a->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(a->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(a->listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(a->listen_fd, 1) != 0) {
        close(a->listen_fd);
        free(a);
        return NULL;
    }
    socklen_t sl = sizeof sa;
    getsockname(a->listen_fd, (struct sockaddr *)&sa, &sl);
    a->port = ntohs(sa.sin_port);
    pthread_create(&a->th, NULL, fake_app_thread, a);
    return a;
}

static void fake_app_stop(fake_app *a)
{
    pthread_join(a->th, NULL);
    close(a->listen_fd);
    free(a);
}

static void test_bridge_rejects_non_loopback_and_short_token(void)
{
    pc_auto_mcp *m = NULL;
    CHECK(pc_auto_mcp_bridge("10.0.0.5:7878", CONTROL_TOKEN, &m, NULL) != PC_OK);
    CHECK(pc_auto_mcp_bridge("127.0.0.1:7878", CONTROL_TOKEN, &m, NULL) == PC_OK);
    pc_auto_mcp_free(m);
    m = NULL;
    CHECK(pc_auto_mcp_bridge("localhost:1", CONTROL_TOKEN, &m, NULL) == PC_OK);
    pc_auto_mcp_free(m);
    m = NULL;
    CHECK(pc_auto_mcp_bridge("localhost:1", "short", &m, NULL) != PC_OK);
}

static void test_bridge_reports_unreachable_app(void)
{
    pc_auto_mcp *m = NULL;
    CHECK(pc_auto_mcp_bridge("127.0.0.1:1", CONTROL_TOKEN, &m, NULL) == PC_OK);
    mcp_conn *c = mcp_open(m);
    yyjson_val *res;
    char text[1024];
    yyjson_doc *d = mcp_tool(c, "ui_inspect", "{}", &res);
    CHECK(cr_is_error(res));
    cr_text(res, text, sizeof text);
    CHECK_CONTAINS(text, "--control");
    yyjson_doc_free(d);
    mcp_close(c);
}

static void test_bridge_rejects_wrong_token(void)
{
    fake_app *a = fake_app_start();
    CHECK(a != NULL);
    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", a->port);
    const char *wrong = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    pc_auto_mcp *m = NULL;
    CHECK(pc_auto_mcp_bridge(addr, wrong, &m, NULL) == PC_OK);
    mcp_conn *c = mcp_open(m);
    yyjson_val *res;
    char text[1024];
    yyjson_doc *d = mcp_tool(c, "ui_inspect", "{}", &res);
    CHECK(cr_is_error(res));
    cr_text(res, text, sizeof text);
    CHECK_CONTAINS(text, "authentication required");
    yyjson_doc_free(d);
    mcp_close(c);
    fake_app_stop(a);
}

static void test_bridge_forwards_to_control_protocol(void)
{
    fake_app *a = fake_app_start();
    CHECK(a != NULL);
    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", a->port);
    pc_auto_mcp *m = NULL;
    CHECK(pc_auto_mcp_bridge(addr, CONTROL_TOKEN, &m, NULL) == PC_OK);
    mcp_conn *c = mcp_open(m);
    yyjson_val *res;
    yyjson_doc *d, *jd;

    d = mcp_tool(c, "ui_inspect", "{}", &res);
    {
        yyjson_val *j = cr_json(res, &jd);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(j, "tool"), NULL), "brush");
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);

    d = mcp_tool(c, "command_run", "{\"id\":\"layer.new.layer\",\"params\":{\"name\":\"X\"}}", &res);
    {
        yyjson_val *j = cr_json(res, &jd);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(j, "ran"), NULL), "layer.new.layer");
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);

    d = mcp_tool(c, "command_list", "{}", &res);
    {
        yyjson_val *j = cr_json(res, &jd);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(yyjson_arr_get(j, 0), "id"), NULL), "file.new");
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);

    d = mcp_tool(c, "ui_screenshot", "{\"max_side\":20}", &res);
    {
        /* §7: the bridge forwards the control reply as JSON text ({mimeType, base64}) without the
         * reference's image-content wrapping or downscale, so the PNG keeps the app's native size. */
        yyjson_val *j = cr_json(res, &jd);
        const char *b64 = pc_json_as_str(pc_json_get(j, "base64"), NULL);
        uint32_t w = 0, h = 0;
        CHECK(b64 && png_dims(b64, &w, &h));
        CHECK_EQ_UINT(w, 40u);
        CHECK_EQ_UINT(h, 20u);
        yyjson_doc_free(jd);
    }
    yyjson_doc_free(d);

    char text[1024];
    d = mcp_tool(c, "control_call", "{\"method\":\"bogus.method\"}", &res);
    CHECK(cr_is_error(res));
    cr_text(res, text, sizeof text);
    CHECK_CONTAINS(text, "unknown tool");
    yyjson_doc_free(d);

    d = mcp_tool(c, "doc_select", "{\"index\":0}", &res);
    CHECK(cr_is_error(res));
    yyjson_doc_free(d);

    mcp_close(c);
    fake_app_stop(a);
}

int main(int argc, char **argv)
{
    RUN(test_lists_expected_tools);
    RUN(test_edit_render_save_roundtrip);
    RUN(test_command_list_fields);
    RUN(test_errors_are_tool_errors);
    RUN(test_preview_budget);
    RUN(test_tool_schemas_and_conventions);
    RUN(test_resources_are_live_json);
    RUN(test_bridge_rejects_non_loopback_and_short_token);
    RUN(test_bridge_reports_unreachable_app);
    RUN(test_bridge_rejects_wrong_token);
    RUN(test_bridge_forwards_to_control_protocol);
    return pc_t_finish();
}
