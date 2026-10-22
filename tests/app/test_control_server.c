/* control_server: a background TCP connection authenticates, then requests are dispatched on the
 * main thread via the pump; bad JSON and repeat auth are answered without reaching the dispatcher. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "app/control_server.h"
#include "base/json.h"
#include "harness.h"

#define TOKEN "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static atomic_int g_dispatch_calls;

static pc_status dispatcher(void *ctx, const char *method, yyjson_val *params, pc_json_value *out,
                            pc_error *err)
{
    (void)ctx;
    (void)params;
    atomic_fetch_add(&g_dispatch_calls, 1);
    if (strcmp(method, "test.ok") != 0) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "unknown method %s", method);
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        return pc_fail(err, PC_ERR_NOMEM, "oom");
    }
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "method", method);
    yyjson_mut_doc_set_root(d, o);
    pc_status rc = pc_json_value_from_mut(o, out, err);
    yyjson_mut_doc_free(d);
    return rc;
}

static bool send_line(int fd, const char *line)
{
    size_t len = strlen(line);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, line + off, len - off);
        if (w < 0) {
            return false;
        }
        off += (size_t)w;
    }
    return true;
}

static bool read_line(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) {
            return false;
        }
        buf[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    buf[n] = '\0';
    return n > 0;
}

typedef struct {
    uint16_t port;
    atomic_bool done;
    bool auth_ok;
    bool req_ok;
    double req_id;
    bool bad_ok_false;
    bool bad_id_null;
} client_state;

static bool reply_ok(const char *line)
{
    yyjson_doc *d = NULL;
    pc_error err;
    bool ok = false;
    if (pc_json_parse(line, strlen(line), &d, &err) == PC_OK && d) {
        (void)pc_json_as_bool(pc_json_get(yyjson_doc_get_root(d), "ok"), &ok);
    }
    yyjson_doc_free(d);
    return ok;
}

static void *client_thread(void *arg)
{
    client_state *cs = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(cs->port);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
            char line[1024];
            /* auth */
            if (send_line(fd, "{\"id\":1,\"method\":\"auth\",\"params\":{\"token\":\"" TOKEN "\"}}\n") &&
                read_line(fd, line, sizeof line)) {
                cs->auth_ok = reply_ok(line);
            }
            /* a dispatched request */
            if (send_line(fd, "{\"id\":42,\"method\":\"test.ok\"}\n") && read_line(fd, line, sizeof line)) {
                yyjson_doc *d = NULL;
                pc_error err;
                if (pc_json_parse(line, strlen(line), &d, &err) == PC_OK && d) {
                    bool ok = false;
                    (void)pc_json_as_bool(pc_json_get(yyjson_doc_get_root(d), "ok"), &ok);
                    (void)pc_json_as_f64(pc_json_get(yyjson_doc_get_root(d), "id"), &cs->req_id);
                    cs->req_ok = ok;
                }
                yyjson_doc_free(d);
            }
            /* bad JSON: answered with id null, ok false, connection keeps serving */
            if (send_line(fd, "{not json}\n") && read_line(fd, line, sizeof line)) {
                yyjson_doc *d = NULL;
                pc_error err;
                if (pc_json_parse(line, strlen(line), &d, &err) == PC_OK && d) {
                    bool ok = true;
                    (void)pc_json_as_bool(pc_json_get(yyjson_doc_get_root(d), "ok"), &ok);
                    cs->bad_ok_false = !ok;
                    cs->bad_id_null = yyjson_is_null(pc_json_get(yyjson_doc_get_root(d), "id"));
                }
                yyjson_doc_free(d);
            }
        }
        (void)close(fd);
    }
    atomic_store(&cs->done, true);
    return NULL;
}

static void test_dispatch_and_framing(void)
{
    atomic_store(&g_dispatch_calls, 0);
    pc_app_control_server *s = NULL;
    uint16_t port = 0;
    pc_error err;
    pc_error_clear(&err);
    pc_status rc = pc_app_control_server_start(0, TOKEN, dispatcher, NULL, &s, &port, &err);
    CHECK_EQ_INT(rc, PC_OK);
    if (rc != PC_OK) {
        return;
    }
    CHECK(port != 0);

    client_state cs;
    memset(&cs, 0, sizeof cs);
    cs.port = port;
    atomic_init(&cs.done, false);
    pthread_t t;
    CHECK_EQ_INT(pthread_create(&t, NULL, client_thread, &cs), 0);

    /* Act as the main loop: pump until the client is done (bounded). */
    for (int i = 0; i < 5000 && !atomic_load(&cs.done); i++) {
        pc_app_control_server_pump(s);
        struct timespec ts = {0, 2 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    pc_app_control_server_pump(s);
    (void)pthread_join(t, NULL);

    CHECK(cs.auth_ok);
    CHECK(cs.req_ok);
    CHECK_NEAR(cs.req_id, 42.0, 0.0001);
    CHECK(cs.bad_ok_false);
    CHECK(cs.bad_id_null);
    /* Only the real request reached the dispatcher (auth and bad JSON did not). */
    CHECK_EQ_INT(atomic_load(&g_dispatch_calls), 1);

    pc_app_control_server_stop(s);
}

static void test_bad_token_rejected(void)
{
    pc_app_control_server *s = NULL;
    pc_error err;
    /* A short/invalid token is refused at start. */
    CHECK(pc_app_control_server_start(0, "not-hex", dispatcher, NULL, &s, NULL, &err) != PC_OK);
    CHECK(s == NULL);
}

int main(int argc, char **argv)
{
    RUN(test_dispatch_and_framing);
    RUN(test_bad_token_rejected);
    return pc_t_finish();
}
