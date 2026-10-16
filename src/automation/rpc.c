/* Port of crates/automation/src/rpc.rs. */
#include "automation/rpc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "automation/automation.h"
#include "automation/budgets.h"
#include "automation/security.h"
#include "base/json.h"

/* Build {"id":<id copy>, "ok":false, "error":msg} as an owned doc. id may be NULL -> null. */
static yyjson_mut_doc *error_reply(yyjson_val *id, const char *msg)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, o, "id", id ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    yyjson_mut_obj_add_bool(doc, o, "ok", false);
    yyjson_mut_obj_add_strcpy(doc, o, "error", msg);
    yyjson_mut_doc_set_root(doc, o);
    return doc;
}

/* Answer one request line; returns an owned reply doc (caller frees). Never NULL except on OOM. */
static yyjson_mut_doc *respond(pc_headless *h, const char *line, size_t len)
{
    yyjson_doc *req = NULL;
    pc_error perr;
    pc_error_clear(&perr);
    if (pc_json_parse(line, len, &req, &perr) != PC_OK || !req) {
        char msg[512];
        snprintf(msg, sizeof msg, "bad JSON: %s", perr.msg[0] ? perr.msg : "invalid");
        return error_reply(NULL, msg);
    }
    yyjson_val *root = yyjson_doc_get_root(req);
    yyjson_val *id = pc_json_get(root, "id");
    const char *method = pc_json_as_str(pc_json_get(root, "method"), NULL);
    if (!method) {
        yyjson_mut_doc *r = error_reply(id, "missing `method`");
        yyjson_doc_free(req);
        return r;
    }
    yyjson_val *params = pc_json_get(root, "params");
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_error err;
    pc_error_clear(&err);
    pc_status rc = pc_headless_handle(h, method, params, &result, &err);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        pc_json_value_fini(&result);
        yyjson_doc_free(req);
        return NULL;
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, o, "id", id ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc));
    if (rc == PC_OK) {
        yyjson_mut_obj_add_bool(doc, o, "ok", true);
        yyjson_mut_val *rroot = pc_json_value_root(&result);
        yyjson_mut_val *copy = rroot ? yyjson_mut_val_mut_copy(doc, rroot) : yyjson_mut_null(doc);
        yyjson_mut_obj_add_val(doc, o, "result", copy ? copy : yyjson_mut_null(doc));
    } else {
        yyjson_mut_obj_add_bool(doc, o, "ok", false);
        yyjson_mut_obj_add_strcpy(doc, o, "error", err.msg);
    }
    yyjson_mut_doc_set_root(doc, o);
    pc_json_value_fini(&result);
    yyjson_doc_free(req);
    return doc;
}

static bool line_blank(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' && c != '\v') {
            return false;
        }
    }
    return true;
}

/* Write an owned reply doc (plus newline) to out_fd, then free it. */
static pc_status flush_reply(int out_fd, bool use_timeout, yyjson_mut_doc *reply, pc_error *err)
{
    if (!reply) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = pc_auto_write_reply(&buf, yyjson_mut_doc_get_root(reply), err);
    if (rc == PC_OK) {
        rc = pc_auto_write_all(out_fd, use_timeout, buf.data, buf.len, err);
    }
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(reply);
    return rc;
}

pc_status pc_auto_serve_lines(pc_headless *h, int in_fd, int out_fd, pc_error *err)
{
    pc_auto_reader r;
    pc_auto_reader_init(&r, in_fd, false);
    pc_buf line = PC_BUF_INIT;
    for (;;) {
        pc_auto_line kind = pc_auto_read_bounded_line(&r, &line, err);
        yyjson_mut_doc *reply = NULL;
        bool unread_rest = false;
        switch (kind) {
        case PC_AUTO_LINE_EOF:
            pc_buf_fini(&line);
            return PC_OK;
        case PC_AUTO_LINE_TOO_LONG: {
            char msg[96];
            snprintf(msg, sizeof msg, "request exceeds %zu bytes", PC_AUTO_MAX_REQUEST_BYTES);
            reply = error_reply(NULL, msg);
            unread_rest = !(line.len > 0 && line.data[line.len - 1] == '\n');
            break;
        }
        case PC_AUTO_LINE_INVALID_UTF8:
            reply = error_reply(NULL, "request is not valid UTF-8");
            break;
        case PC_AUTO_LINE_IOERR:
            pc_buf_fini(&line);
            return err ? err->code : PC_ERR_IO;
        case PC_AUTO_LINE_OK:
            if (line_blank((const char *)line.data, line.len)) {
                continue;
            }
            reply = respond(h, (const char *)line.data, line.len);
            break;
        }
        pc_status rc = flush_reply(out_fd, false, reply, err);
        if (rc != PC_OK) {
            pc_buf_fini(&line);
            return rc;
        }
        if (unread_rest && pc_auto_discard_rest_of_line(&r, err) != PC_OK) {
            pc_buf_fini(&line);
            return err ? err->code : PC_ERR_IO;
        }
    }
}

/* ---- TCP ---- */

typedef struct tcp_shared {
    pc_headless *h;
    pthread_mutex_t lock;
    char token[65];
} tcp_shared;

typedef struct tcp_conn {
    tcp_shared *shared;
    int fd;
    pc_auto_limiter *limiter;
} tcp_conn;

static void serve_tcp_connection(tcp_shared *s, int fd)
{
    pc_auto_reader r;
    pc_auto_reader_init(&r, fd, true);
    pc_buf line = PC_BUF_INIT;
    bool authenticated = false;
    for (;;) {
        pc_error err;
        pc_error_clear(&err);
        pc_auto_line kind = pc_auto_read_bounded_line(&r, &line, &err);
        if (kind == PC_AUTO_LINE_EOF || kind == PC_AUTO_LINE_IOERR) {
            break;
        }
        if (kind == PC_AUTO_LINE_TOO_LONG) {
            char msg[96];
            snprintf(msg, sizeof msg, "request exceeds %zu bytes", PC_AUTO_MAX_REQUEST_BYTES);
            (void)flush_reply(fd, true, error_reply(NULL, msg), &err);
            break;
        }
        if (kind == PC_AUTO_LINE_INVALID_UTF8) {
            (void)flush_reply(fd, true, error_reply(NULL, "request is not valid UTF-8"), &err);
            continue;
        }
        if (line_blank((const char *)line.data, line.len)) {
            continue;
        }
        yyjson_mut_doc *reply = NULL;
        if (authenticated) {
            pc_error ie;
            pc_error_clear(&ie);
            yyjson_mut_doc *repeat = NULL;
            (void)pc_auto_repeat_auth_reply((const char *)line.data, line.len, &repeat, &ie);
            if (repeat) {
                reply = repeat;
            } else {
                pthread_mutex_lock(&s->lock);
                reply = respond(s->h, (const char *)line.data, line.len);
                pthread_mutex_unlock(&s->lock);
            }
        } else {
            bool ok = false;
            pc_error ae;
            pc_error_clear(&ae);
            (void)pc_auto_authentication_reply((const char *)line.data, line.len, s->token, &reply, &ok, &ae);
            authenticated = ok;
        }
        if (flush_reply(fd, true, reply, &err) != PC_OK) {
            break;
        }
        if (!authenticated) {
            break;
        }
    }
    pc_buf_fini(&line);
}

static void *conn_thread(void *arg)
{
    tcp_conn *c = arg;
    serve_tcp_connection(c->shared, c->fd);
    (void)close(c->fd);
    pc_auto_limiter_release(c->limiter);
    free(c);
    return NULL;
}

pc_status pc_auto_serve_tcp(const char *addr, pc_headless *h, const char *token,
                            void (*ready)(int port, void *ud), void *ud, pc_error *err)
{
    /* addr is "127.0.0.1:<port>". Parse the port. */
    const char *colon = strrchr(addr, ':');
    if (!colon) {
        return pc_auto_bad(err, "%s is not a loopback address", addr);
    }
    int port = atoi(colon + 1);
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        return pc_auto_io(err, "bind %s: %s", addr, strerror(errno));
    }
    int one = 1;
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        (void)close(lfd);
        return pc_auto_io(err, "bind %s: %s", addr, strerror(e));
    }
    if (listen(lfd, 16) != 0) {
        int e = errno;
        (void)close(lfd);
        return pc_auto_io(err, "listen %s: %s", addr, strerror(e));
    }
    socklen_t slen = sizeof sa;
    if (getsockname(lfd, (struct sockaddr *)&sa, &slen) == 0 && ready) {
        ready(ntohs(sa.sin_port), ud);
    }
    tcp_shared shared;
    shared.h = h;
    pthread_mutex_init(&shared.lock, NULL);
    snprintf(shared.token, sizeof shared.token, "%s", token);
    pc_auto_limiter limiter;
    pc_auto_limiter_init(&limiter, PC_AUTO_MAX_CONNECTIONS);
    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (!pc_auto_limiter_acquire(&limiter)) {
            const char *m = "{\"id\":null,\"ok\":false,\"error\":\"connection limit reached\"}\n";
            ssize_t wn = write(cfd, m, strlen(m));
            (void)wn;
            (void)close(cfd);
            continue;
        }
        tcp_conn *c = calloc(1, sizeof *c);
        if (!c) {
            pc_auto_limiter_release(&limiter);
            (void)close(cfd);
            continue;
        }
        c->shared = &shared;
        c->fd = cfd;
        c->limiter = &limiter;
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, c) != 0) {
            pc_auto_limiter_release(&limiter);
            (void)close(cfd);
            free(c);
            continue;
        }
        pthread_detach(t);
    }
    (void)close(lfd);
    pthread_mutex_destroy(&shared.lock);
    return PC_OK;
}
