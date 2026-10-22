/* See control_server.h. Behavioural port of apps/photocraft/src/control_server.rs. */
#include "control_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "automation/budgets.h"
#include "automation/security.h"
#include "base/buf.h"
#include "base/log.h"

/* ---- request mailbox node ---- */

typedef struct req_node {
    struct req_node *next;
    char *line;
    size_t len;
    char id_text[512]; /* serialized id value for the timeout reply ("null" when absent) */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
    bool abandoned;
    int refcount;
    pc_buf reply; /* reply bytes incl. trailing newline, filled by the pump */
} req_node;

typedef struct conn_slot {
    pthread_t thread;
    int fd;    /* -1 once the connection thread has finished with it */
    bool used;
} conn_slot;

struct pc_app_control_server {
    int lfd;
    uint16_t port;
    char token[65];
    pc_app_control_dispatch dispatch;
    void *dispatch_ctx;
    pc_auto_limiter limiter;
    atomic_bool running;
    pthread_t accept_thread;
    bool accept_started;

    pthread_mutex_t qlock;
    req_node *head;
    req_node *tail;

    void (*wake)(void *ud);
    void *wake_ud;

    pthread_mutex_t clock;
    conn_slot *conns;
    size_t n_conns;
    size_t cap_conns;
};

static void node_unref(req_node *n)
{
    pthread_mutex_lock(&n->lock);
    int rc = --n->refcount;
    pthread_mutex_unlock(&n->lock);
    if (rc == 0) {
        pc_buf_fini(&n->reply);
        free(n->line);
        pthread_cond_destroy(&n->cond);
        pthread_mutex_destroy(&n->lock);
        free(n);
    }
}

/* Build {"id":<id copy>, "ok":false, "error":msg} as an owned doc (id may be NULL -> null). */
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

/* Dispatch one parsed request line and build the reply doc (owned). NULL only on OOM. */
static yyjson_mut_doc *respond(pc_app_control_server *s, const char *line, size_t len)
{
    yyjson_doc *req = NULL;
    pc_error perr;
    pc_error_clear(&perr);
    if (pc_json_parse(line, len, &req, &perr) != PC_OK || !req) {
        char msg[512];
        (void)snprintf(msg, sizeof msg, "bad JSON: %s", perr.msg[0] ? perr.msg : "invalid");
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
    pc_status rc = s->dispatch(s->dispatch_ctx, method, params, &result, &err);
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

/* Write an owned reply doc (plus newline) to fd, then free it. */
static pc_status flush_reply(int fd, yyjson_mut_doc *reply, pc_error *err)
{
    if (!reply) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_buf buf = PC_BUF_INIT;
    pc_status rc = pc_auto_write_reply(&buf, yyjson_mut_doc_get_root(reply), err);
    if (rc == PC_OK) {
        rc = pc_auto_write_all(fd, true, buf.data, buf.len, err);
    }
    pc_buf_fini(&buf);
    yyjson_mut_doc_free(reply);
    return rc;
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

/* Serialize the request's id into id_text[cap] ("null" when absent, too long, or on error). */
static void capture_id(const char *line, size_t len, char *id_text, size_t cap)
{
    (void)snprintf(id_text, cap, "null");
    yyjson_doc *doc = NULL;
    pc_error err;
    if (pc_json_parse(line, len, &doc, &err) != PC_OK || !doc) {
        return;
    }
    yyjson_val *id = pc_json_get(yyjson_doc_get_root(doc), "id");
    if (id) {
        size_t n = 0;
        char *text = yyjson_val_write(id, 0, &n);
        if (text) {
            if (n < cap) {
                memcpy(id_text, text, n);
                id_text[n] = '\0';
            }
            free(text);
        }
    }
    yyjson_doc_free(doc);
}

/* Hand a valid request to the main thread and wait (bounded) for its reply; write it to fd. Returns
 * PC_OK if the connection may continue. */
static pc_status dispatch_via_mailbox(pc_app_control_server *s, int fd, const char *line, size_t len)
{
    req_node *n = calloc(1, sizeof *n);
    if (!n) {
        pc_error err;
        return flush_reply(fd, error_reply(NULL, "out of memory"), &err);
    }
    n->line = malloc(len + 1);
    if (!n->line) {
        free(n);
        pc_error err;
        return flush_reply(fd, error_reply(NULL, "out of memory"), &err);
    }
    memcpy(n->line, line, len);
    n->line[len] = '\0';
    n->len = len;
    n->refcount = 2;
    pthread_mutex_init(&n->lock, NULL);
    pthread_cond_init(&n->cond, NULL);
    capture_id(line, len, n->id_text, sizeof n->id_text);

    pthread_mutex_lock(&s->qlock);
    n->next = NULL;
    if (s->tail) {
        s->tail->next = n;
    } else {
        s->head = n;
    }
    s->tail = n;
    pthread_mutex_unlock(&s->qlock);
    if (s->wake) {
        s->wake(s->wake_ud);
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += PC_APP_CONTROL_REPLY_DEADLINE_SECS;
    pthread_mutex_lock(&n->lock);
    int wrc = 0;
    /* If the server is already stopping, do not block: stop() wakes queued nodes, and this check
     * covers a node enqueued after that wake. */
    while (!n->done && wrc != ETIMEDOUT && atomic_load(&s->running)) {
        wrc = pthread_cond_timedwait(&n->cond, &n->lock, &ts);
    }
    bool done = n->done;
    if (!done) {
        n->abandoned = true;
    }
    pthread_mutex_unlock(&n->lock);

    pc_error err;
    pc_error_clear(&err);
    pc_status rc;
    if (done) {
        rc = pc_auto_write_all(fd, true, n->reply.data, n->reply.len, &err);
    } else {
        char msg[640];
        int m = snprintf(msg, sizeof msg,
                         "{\"id\":%s,\"ok\":false,\"error\":\"the UI did not reply within %ds; the "
                         "operation may still be applying\"}\n",
                         n->id_text, PC_APP_CONTROL_REPLY_DEADLINE_SECS);
        rc = (m > 0 && (size_t)m < sizeof msg)
                 ? pc_auto_write_all(fd, true, msg, (size_t)m, &err)
                 : PC_ERR_INVALID;
    }
    node_unref(n);
    return rc;
}

static void serve_connection(pc_app_control_server *s, int fd)
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
            (void)snprintf(msg, sizeof msg, "request exceeds %zu bytes", PC_AUTO_MAX_REQUEST_BYTES);
            (void)flush_reply(fd, error_reply(NULL, msg), &err);
            break;
        }
        if (kind == PC_AUTO_LINE_INVALID_UTF8) {
            if (flush_reply(fd, error_reply(NULL, "request is not valid UTF-8"), &err) != PC_OK) {
                break;
            }
            continue;
        }
        if (line_blank((const char *)line.data, line.len)) {
            continue;
        }
        if (!authenticated) {
            yyjson_mut_doc *reply = NULL;
            bool ok = false;
            (void)pc_auto_authentication_reply((const char *)line.data, line.len, s->token, &reply, &ok,
                                               &err);
            authenticated = ok;
            if (flush_reply(fd, reply, &err) != PC_OK || !authenticated) {
                break;
            }
            continue;
        }
        yyjson_mut_doc *repeat = NULL;
        (void)pc_auto_repeat_auth_reply((const char *)line.data, line.len, &repeat, &err);
        if (repeat) {
            if (flush_reply(fd, repeat, &err) != PC_OK) {
                break;
            }
            continue;
        }
        if (dispatch_via_mailbox(s, fd, (const char *)line.data, line.len) != PC_OK) {
            break;
        }
    }
    pc_buf_fini(&line);
}

typedef struct conn_arg {
    pc_app_control_server *server;
    int fd;
    size_t slot;
} conn_arg;

static void *conn_thread(void *arg)
{
    conn_arg *c = arg;
    pc_app_control_server *s = c->server;
    int fd = c->fd;
    size_t slot = c->slot;
    free(c);
    serve_connection(s, fd);
    pthread_mutex_lock(&s->clock);
    if (slot < s->n_conns) {
        s->conns[slot].fd = -1; /* fd about to close; stop() must not shutdown a reused number */
    }
    pthread_mutex_unlock(&s->clock);
    (void)close(fd);
    pc_auto_limiter_release(&s->limiter);
    return NULL;
}

static void *accept_thread(void *arg)
{
    pc_app_control_server *s = arg;
    while (atomic_load(&s->running)) {
        int fd = accept(s->lfd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break; /* listener closed by stop(), or a real error */
        }
        if (!atomic_load(&s->running)) {
            (void)close(fd);
            break;
        }
        if (!pc_auto_limiter_acquire(&s->limiter)) {
            pc_error err;
            (void)flush_reply(fd, error_reply(NULL, "connection limit reached"), &err);
            (void)close(fd);
            continue;
        }
        conn_arg *c = calloc(1, sizeof *c);
        if (!c) {
            pc_auto_limiter_release(&s->limiter);
            (void)close(fd);
            continue;
        }
        c->server = s;
        c->fd = fd;
        pthread_mutex_lock(&s->clock);
        if (s->n_conns == s->cap_conns) {
            size_t cap = s->cap_conns ? s->cap_conns * 2 : 16;
            conn_slot *grown = realloc(s->conns, cap * sizeof *grown);
            if (grown) {
                s->conns = grown;
                s->cap_conns = cap;
            }
        }
        size_t slot = s->n_conns;
        bool tracked = slot < s->cap_conns;
        if (tracked) {
            s->conns[slot].fd = fd;
            s->conns[slot].used = true;
            c->slot = slot;
            s->n_conns++;
        } else {
            c->slot = (size_t)-1;
        }
        pthread_mutex_unlock(&s->clock);
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, c) != 0) {
            if (tracked) {
                pthread_mutex_lock(&s->clock);
                s->conns[slot].used = false;
                s->conns[slot].fd = -1;
                pthread_mutex_unlock(&s->clock);
            }
            pc_auto_limiter_release(&s->limiter);
            (void)close(fd);
            free(c);
            continue;
        }
        if (tracked) {
            pthread_mutex_lock(&s->clock);
            s->conns[slot].thread = t;
            pthread_mutex_unlock(&s->clock);
        } else {
            (void)pthread_detach(t);
        }
    }
    return NULL;
}

size_t pc_app_control_server_pump(pc_app_control_server *s)
{
    if (!s) {
        return 0;
    }
    pthread_mutex_lock(&s->qlock);
    req_node *n = s->head;
    s->head = NULL;
    s->tail = NULL;
    pthread_mutex_unlock(&s->qlock);

    size_t count = 0;
    while (n) {
        req_node *next = n->next;
        pthread_mutex_lock(&n->lock);
        bool abandoned = n->abandoned;
        pthread_mutex_unlock(&n->lock);
        if (!abandoned) {
            yyjson_mut_doc *reply = respond(s, n->line, n->len);
            pc_error err;
            pc_error_clear(&err);
            if (reply) {
                (void)pc_auto_write_reply(&n->reply, yyjson_mut_doc_get_root(reply), &err);
                yyjson_mut_doc_free(reply);
            }
        }
        pthread_mutex_lock(&n->lock);
        n->done = true;
        pthread_cond_signal(&n->cond);
        pthread_mutex_unlock(&n->lock);
        node_unref(n);
        count++;
        n = next;
    }
    return count;
}

void pc_app_control_server_on_wake(pc_app_control_server *s, void (*wake)(void *ud), void *ud)
{
    if (!s) {
        return;
    }
    s->wake = wake;
    s->wake_ud = ud;
}

uint16_t pc_app_control_server_port(const pc_app_control_server *s)
{
    return s ? s->port : 0;
}

pc_status pc_app_control_server_start(uint16_t port, const char *token,
                                      pc_app_control_dispatch dispatch, void *dispatch_ctx,
                                      pc_app_control_server **out, uint16_t *bound_port, pc_error *err)
{
    if (!token || !dispatch || !out) {
        return pc_fail(err, PC_ERR_INVALID, "control server: bad arguments");
    }
    if (pc_auto_validate_token(token, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    pc_app_control_server *s = calloc(1, sizeof *s);
    if (!s) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s->dispatch = dispatch;
    s->dispatch_ctx = dispatch_ctx;
    (void)snprintf(s->token, sizeof s->token, "%s", token);
    pc_auto_limiter_init(&s->limiter, PC_AUTO_MAX_CONNECTIONS);
    atomic_init(&s->running, true);
    pthread_mutex_init(&s->qlock, NULL);
    pthread_mutex_init(&s->clock, NULL);
    s->lfd = -1;

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        pc_status rc = pc_fail_errno(err, PC_ERR_IO, errno, "control server socket");
        pc_app_control_server_stop(s);
        return rc;
    }
    int one = 1;
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        (void)close(lfd);
        pc_status rc = pc_fail_errno(err, PC_ERR_IO, e, "control server bind 127.0.0.1:%u", port);
        pc_app_control_server_stop(s);
        return rc;
    }
    if (listen(lfd, 16) != 0) {
        int e = errno;
        (void)close(lfd);
        pc_status rc = pc_fail_errno(err, PC_ERR_IO, e, "control server listen");
        pc_app_control_server_stop(s);
        return rc;
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof bound;
    if (getsockname(lfd, (struct sockaddr *)&bound, &blen) == 0) {
        s->port = ntohs(bound.sin_port);
    } else {
        s->port = port;
    }
    s->lfd = lfd;
    if (pthread_create(&s->accept_thread, NULL, accept_thread, s) != 0) {
        pc_status rc = pc_fail(err, PC_ERR_IO, "control server: cannot start accept thread");
        pc_app_control_server_stop(s);
        return rc;
    }
    s->accept_started = true;
    if (bound_port) {
        *bound_port = s->port;
    }
    *out = s;
    return PC_OK;
}

void pc_app_control_server_stop(pc_app_control_server *s)
{
    if (!s) {
        return;
    }
    atomic_store(&s->running, false);
    if (s->lfd >= 0) {
        (void)shutdown(s->lfd, SHUT_RDWR);
        (void)close(s->lfd);
        s->lfd = -1;
    }
    if (s->accept_started) {
        (void)pthread_join(s->accept_thread, NULL);
    }
    /* Wake any connection threads waiting on a queued request (the main loop will not pump again). */
    pthread_mutex_lock(&s->qlock);
    for (req_node *node = s->head; node; node = node->next) {
        pthread_mutex_lock(&node->lock);
        node->done = true;
        pthread_cond_broadcast(&node->cond);
        pthread_mutex_unlock(&node->lock);
    }
    pthread_mutex_unlock(&s->qlock);
    /* Force any lingering connections to return, then join their threads. */
    pthread_mutex_lock(&s->clock);
    size_t n = s->n_conns;
    pthread_mutex_unlock(&s->clock);
    for (size_t i = 0; i < n; i++) {
        pthread_mutex_lock(&s->clock);
        int fd = s->conns[i].fd;
        pthread_mutex_unlock(&s->clock);
        if (fd >= 0) {
            (void)shutdown(fd, SHUT_RDWR);
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (s->conns[i].used) {
            (void)pthread_join(s->conns[i].thread, NULL);
        }
    }
    /* Drain any still-queued nodes (their server threads are gone): release our references. */
    req_node *node = s->head;
    while (node) {
        req_node *next = node->next;
        node_unref(node);
        node = next;
    }
    free(s->conns);
    pthread_mutex_destroy(&s->qlock);
    pthread_mutex_destroy(&s->clock);
    free(s);
}
