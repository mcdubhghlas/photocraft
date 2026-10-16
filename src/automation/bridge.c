/* Port of crates/automation/src/bridge.rs (async -> blocking + poll). */
#include "automation/bridge.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "automation/automation.h"
#include "automation/security.h"
#include "base/buf.h"

#define IDLE_RECONNECT_SECS 25 /* IO_TIMEOUT (30s) minus a 5s margin */

struct pc_auto_bridge {
    char *addr;
    char token[65];
    int fd;           /* cached connection, or -1 */
    time_t last_used; /* for idle reconnect */
    uint64_t next_id;
    uint16_t port;
};

static bool host_is_loopback(const char *host, size_t len)
{
    return (len == 9 && strncmp(host, "127.0.0.1", 9) == 0) ||
           (len == 9 && strncmp(host, "localhost", 9) == 0) || (len == 5 && strncmp(host, "[::1]", 5) == 0) ||
           (len == 3 && strncmp(host, "::1", 3) == 0);
}

pc_status pc_auto_bridge_new(const char *addr, const char *token, pc_auto_bridge **out, pc_error *err)
{
    const char *colon = strrchr(addr, ':');
    size_t hlen = colon ? (size_t)(colon - addr) : strlen(addr);
    if (!host_is_loopback(addr, hlen)) {
        return pc_auto_bad(err, "bridge address must be loopback, got `%s`", addr);
    }
    if (pc_auto_validate_token(token, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    pc_auto_bridge *b = calloc(1, sizeof *b);
    if (!b) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    b->addr = strdup(addr);
    if (!b->addr) {
        free(b);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    snprintf(b->token, sizeof b->token, "%s", token);
    for (char *p = b->token; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = (char)(*p - 'A' + 'a');
        }
    }
    b->fd = -1;
    b->next_id = 1;
    b->port = colon ? (uint16_t)atoi(colon + 1) : 0;
    *out = b;
    return PC_OK;
}

void pc_auto_bridge_free(pc_auto_bridge *b)
{
    if (!b) {
        return;
    }
    if (b->fd >= 0) {
        (void)close(b->fd);
    }
    free(b->addr);
    free(b);
}

const char *pc_auto_bridge_addr(const pc_auto_bridge *b)
{
    return b->addr;
}

static void drop_conn(pc_auto_bridge *b)
{
    if (b->fd >= 0) {
        (void)close(b->fd);
        b->fd = -1;
    }
}

/* Send one framed request and read the reply matching `id`. Returns PC_OK and fills *result (owned)
 * on an app-level ok; an app error fills *err ("app: ...") and returns PC_ERR_INVALID; a transport
 * failure fills *err ("bridge: ...") and returns PC_ERR_IO. */
static pc_status exchange(pc_auto_bridge *b, uint64_t id, const char *method, yyjson_val *params,
                          pc_json_value *result, pc_error *err)
{
    /* Build the request line. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, o, "id", id);
    yyjson_mut_obj_add_strcpy(doc, o, "method", method);
    yyjson_mut_val *pv = params ? yyjson_val_mut_copy(doc, params) : yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, o, "params", pv ? pv : yyjson_mut_obj(doc));
    yyjson_mut_doc_set_root(doc, o);
    pc_buf line = PC_BUF_INIT;
    pc_status rc = pc_json_write(o, 0, &line, err);
    yyjson_mut_doc_free(doc);
    if (rc != PC_OK) {
        pc_buf_fini(&line);
        return rc;
    }
    if (line.len + 1 > PC_AUTO_MAX_REQUEST_BYTES) {
        pc_buf_fini(&line);
        return pc_auto_bridge(err, "request is %zu bytes; maximum is %zu", line.len + 1,
                              PC_AUTO_MAX_REQUEST_BYTES);
    }
    (void)pc_buf_append_byte(&line, '\n', NULL);
    if (pc_auto_write_all(b->fd, true, line.data, line.len, err) != PC_OK) {
        pc_buf_fini(&line);
        return pc_auto_bridge(err, "write failed");
    }
    pc_buf_fini(&line);
    /* Read reply lines until one matches `id`. */
    pc_auto_reader r;
    pc_auto_reader_init(&r, b->fd, true);
    pc_buf rl = PC_BUF_INIT;
    for (;;) {
        pc_auto_line kind = pc_auto_read_bounded_line(&r, &rl, err);
        if (kind == PC_AUTO_LINE_EOF) {
            pc_buf_fini(&rl);
            return pc_auto_bridge(err, "connection closed by the app");
        }
        if (kind == PC_AUTO_LINE_IOERR) {
            pc_buf_fini(&rl);
            return pc_auto_bridge(err, "read failed");
        }
        if (kind != PC_AUTO_LINE_OK) {
            continue;
        }
        yyjson_doc *rep = NULL;
        if (pc_json_parse((const char *)rl.data, rl.len, &rep, NULL) != PC_OK || !rep) {
            continue;
        }
        yyjson_val *root = yyjson_doc_get_root(rep);
        uint64_t rid = 0;
        if (!pc_json_as_u64(pc_json_get(root, "id"), &rid) || rid != id) {
            yyjson_doc_free(rep);
            continue; /* stale reply */
        }
        bool ok = false;
        (void)pc_json_as_bool(pc_json_get(root, "ok"), &ok);
        if (ok) {
            rc = pc_json_value_from_val(pc_json_get(root, "result"), result, err);
        } else {
            const char *msg = pc_json_as_str(pc_json_get(root, "error"), NULL);
            rc = pc_auto_app(err, "%s", msg ? msg : "unknown error");
        }
        yyjson_doc_free(rep);
        pc_buf_fini(&rl);
        return rc;
    }
}

/* Ensure a connected, authenticated socket, reconnecting after a long idle. */
static pc_status ensure_conn(pc_auto_bridge *b, pc_error *err)
{
    time_t now = time(NULL);
    if (b->fd >= 0 && now - b->last_used >= IDLE_RECONNECT_SECS) {
        drop_conn(b);
    }
    if (b->fd >= 0) {
        return PC_OK;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return pc_auto_bridge(err, "cannot create socket: %s", strerror(errno));
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(b->port);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        (void)close(fd);
        return pc_auto_bridge(err,
                              "cannot connect to %s (%s); start the app with `photocraft --control "
                              "<port>` and matching control credentials",
                              b->addr, strerror(e));
    }
    b->fd = fd;
    /* Authenticate. */
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    if (!pd) {
        drop_conn(b);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *po = yyjson_mut_obj(pd);
    yyjson_mut_obj_add_strcpy(pd, po, "token", b->token);
    yyjson_mut_doc_set_root(pd, po);
    yyjson_doc *immut = yyjson_mut_doc_imut_copy(pd, NULL);
    yyjson_mut_doc_free(pd);
    if (!immut) {
        drop_conn(b);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_json_value res = PC_JSON_VALUE_INIT;
    pc_status rc = exchange(b, b->next_id++, PC_AUTO_AUTH_METHOD, yyjson_doc_get_root(immut), &res, err);
    yyjson_doc_free(immut);
    pc_json_value_fini(&res);
    if (rc != PC_OK) {
        drop_conn(b);
        return rc;
    }
    b->last_used = time(NULL);
    return PC_OK;
}

static pc_status attempt(pc_auto_bridge *b, const char *method, yyjson_val *params, pc_json_value *out,
                         pc_error *err)
{
    if (ensure_conn(b, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    pc_status rc = exchange(b, b->next_id++, method, params, out, err);
    if (rc == PC_ERR_IO) {
        drop_conn(b); /* transport failure: reconnect next time */
    } else {
        b->last_used = time(NULL);
    }
    return rc;
}

pc_status pc_auto_bridge_call(pc_auto_bridge *b, const char *method, yyjson_val *params, bool read,
                              pc_json_value *out, pc_error *err)
{
    pc_status rc = attempt(b, method, params, out, err);
    if (rc == PC_ERR_IO && read) {
        /* A read applied nothing: one fresh attempt. */
        rc = attempt(b, method, params, out, err);
    }
    return rc;
}
