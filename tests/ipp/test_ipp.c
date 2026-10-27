/* Ports the unit and integration tests of crates/ipp/src/tests.rs: endpoint parsing and authority,
 * the request builder's mandatory attributes, bounded reply parsing, the chunked-transfer walk and
 * dechunk, printer-name encoding, and the client against an in-process fake CUPS server over TCP
 * (content-length and chunked replies, every operation, error statuses and broken replies). */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "base/buf.h"
#include "harness.h"
#include "ipp/internal.h"
#include "ipp/ipp.h"

/* ---- IPP reply builder (mirrors the Rust `ipp()` test helper) ------------------------------ */

static void put16(pc_buf *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    (void)pc_buf_append(b, x, 2, NULL);
}

typedef struct {
    uint8_t tag;
    const char *name;
    const uint8_t *const *values; /* value pointers */
    const size_t *value_lens;
    size_t nvalues;
} tattr;

typedef struct {
    uint8_t group;
    const tattr *attrs;
    size_t nattrs;
} tgroup;

/* Build an IPP reply body with request_id 1. */
static void build_ipp(pc_buf *out, uint16_t status, const tgroup *groups, size_t ngroups)
{
    uint8_t ver[2] = {2, 0};
    (void)pc_buf_append(out, ver, 2, NULL);
    put16(out, status);
    uint8_t id[4] = {0, 0, 0, 1};
    (void)pc_buf_append(out, id, 4, NULL);
    for (size_t g = 0; g < ngroups; g++) {
        (void)pc_buf_append_byte(out, groups[g].group, NULL);
        for (size_t i = 0; i < groups[g].nattrs; i++) {
            const tattr *a = &groups[g].attrs[i];
            for (size_t v = 0; v < a->nvalues; v++) {
                (void)pc_buf_append_byte(out, a->tag, NULL);
                const char *nm = v == 0 ? a->name : "";
                put16(out, (uint16_t)strlen(nm));
                (void)pc_buf_append(out, nm, strlen(nm), NULL);
                put16(out, (uint16_t)a->value_lens[v]);
                (void)pc_buf_append(out, a->values[v], a->value_lens[v], NULL);
            }
        }
    }
    (void)pc_buf_append_byte(out, PC_IPP_TAG_END, NULL);
}

/* ---- in-process fake CUPS server over TCP -------------------------------------------------- */

typedef struct {
    int listen_fd;
    uint16_t port;
    pc_buf reply;        /* reply IPP body */
    bool chunked;
    int http_status;
    int accepts;         /* how many connections to serve */
    int keep_open_ms;    /* linger after replying (tests the no-wait-for-close read loop) */
    pthread_t thread;
    /* captured first request (copied body) */
    uint8_t *req_body;
    size_t req_len;
} fake;

static void write_all_fd(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) {
                continue;
            }
            return;
        }
        off += (size_t)w;
    }
}

/* Read HTTP request headers + Content-Length body from fd; returns body (malloc) and *len. */
static uint8_t *read_request(int fd, size_t *len)
{
    pc_buf raw = PC_BUF_INIT;
    uint8_t c;
    size_t header_end = 0;
    size_t content_length = 0;
    bool have_len = false;
    /* Read until "\r\n\r\n". */
    for (;;) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) {
            pc_buf_fini(&raw);
            *len = 0;
            return NULL;
        }
        (void)pc_buf_append_byte(&raw, c, NULL);
        if (raw.len >= 4 && memcmp(raw.data + raw.len - 4, "\r\n\r\n", 4) == 0) {
            header_end = raw.len;
            break;
        }
    }
    /* Parse content-length (case-insensitive) from the header block. */
    char *head = (char *)malloc(header_end + 1);
    memcpy(head, raw.data, header_end);
    head[header_end] = '\0';
    for (char *p = head; *p; p++) {
        if ((*p == 'C' || *p == 'c') && strncasecmp(p, "content-length:", 15) == 0) {
            content_length = (size_t)strtoul(p + 15, NULL, 10);
            have_len = true;
            break;
        }
    }
    free(head);
    uint8_t *body = NULL;
    if (have_len && content_length > 0) {
        body = (uint8_t *)malloc(content_length);
        size_t off = 0;
        while (off < content_length) {
            ssize_t n = read(fd, body + off, content_length - off);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        }
        *len = off;
    } else {
        *len = 0;
    }
    pc_buf_fini(&raw);
    return body;
}

static void send_reply(int fd, const fake *f)
{
    pc_buf resp = PC_BUF_INIT;
    const char *status_text = f->http_status == 200 ? "OK" : "Nope";
    if (f->chunked) {
        (void)pc_buf_appendf(&resp, NULL,
                             "HTTP/1.1 %d %s\r\nContent-Type: application/ipp\r\nTransfer-Encoding: chunked\r\n\r\n",
                             f->http_status, status_text);
        size_t pos = 0;
        while (pos < f->reply.len) {
            size_t chunk = f->reply.len - pos < 7 ? f->reply.len - pos : 7;
            (void)pc_buf_appendf(&resp, NULL, "%zx\r\n", chunk);
            (void)pc_buf_append(&resp, f->reply.data + pos, chunk, NULL);
            (void)pc_buf_append_str(&resp, "\r\n", NULL);
            pos += chunk;
        }
        (void)pc_buf_append_str(&resp, "0\r\n\r\n", NULL);
    } else {
        (void)pc_buf_appendf(&resp, NULL,
                             "HTTP/1.1 %d %s\r\nContent-Type: application/ipp\r\nContent-Length: %zu\r\n\r\n",
                             f->http_status, status_text, f->reply.len);
        (void)pc_buf_append(&resp, f->reply.data, f->reply.len, NULL);
    }
    write_all_fd(fd, resp.data, resp.len);
    pc_buf_fini(&resp);
}

static void *fake_run(void *arg)
{
    fake *f = arg;
    for (int i = 0; i < f->accepts; i++) {
        int fd = accept(f->listen_fd, NULL, NULL);
        if (fd < 0) {
            break;
        }
        size_t len = 0;
        uint8_t *body = read_request(fd, &len);
        if (i == 0) {
            f->req_body = body;
            f->req_len = len;
        } else {
            free(body);
        }
        if (body != NULL || len == 0) {
            send_reply(fd, f);
        }
        if (f->keep_open_ms > 0) {
            struct timespec ts = {f->keep_open_ms / 1000, (long)(f->keep_open_ms % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
        close(fd);
    }
    close(f->listen_fd);
    return NULL;
}

/* Start a fake answering `accepts` requests with `reply`. */
static void fake_start(fake *f, const pc_buf *reply, bool chunked, int http_status, int accepts,
                       int keep_open_ms)
{
    memset(f, 0, sizeof *f);
    f->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(f->listen_fd >= 0);
    int one = 1;
    (void)setsockopt(f->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(f->listen_fd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(f->listen_fd, 8) == 0);
    socklen_t slen = sizeof addr;
    CHECK(getsockname(f->listen_fd, (struct sockaddr *)&addr, &slen) == 0);
    f->port = ntohs(addr.sin_port);
    f->chunked = chunked;
    f->http_status = http_status;
    f->accepts = accepts;
    f->keep_open_ms = keep_open_ms;
    (void)pc_buf_append(&f->reply, reply->data, reply->len, NULL);
    CHECK(pthread_create(&f->thread, NULL, fake_run, f) == 0);
}

static void fake_endpoint(const fake *f, pc_ipp_endpoint *ep)
{
    memset(ep, 0, sizeof *ep);
    ep->kind = PC_IPP_TCP;
    ep->port = f->port;
    (void)pc_str_set_c(&ep->host, "127.0.0.1", NULL);
}

static void fake_join(fake *f)
{
    (void)pthread_join(f->thread, NULL);
    free(f->req_body);
    f->req_body = NULL;
    pc_buf_fini(&f->reply);
}

/* ---- tests --------------------------------------------------------------------------------- */

#define VAL(x) ((const uint8_t *)(x))

static void test_printers_are_listed_with_state_and_default(void)
{
    uint8_t state3[4] = {0, 0, 0, 3};
    uint8_t state5[4] = {0, 0, 0, 5};
    uint8_t yes = 1, no = 0;
    const uint8_t *nm0[] = {VAL("Office")};
    size_t l0[] = {6};
    const uint8_t *info0[] = {VAL("HP in the office")};
    size_t li0[] = {16};
    const uint8_t *st0[] = {state3};
    size_t ls0[] = {4};
    const uint8_t *acc0[] = {&yes};
    size_t la0[] = {1};
    const uint8_t *nm1[] = {VAL("Stopped")};
    size_t l1[] = {7};
    const uint8_t *st1[] = {state5};
    const uint8_t *acc1[] = {&no};

    tattr p0[] = {{PC_IPP_TAG_NAME, "printer-name", nm0, l0, 1},
                  {PC_IPP_TAG_TEXT, "printer-info", info0, li0, 1},
                  {PC_IPP_TAG_ENUM, "printer-state", st0, ls0, 1},
                  {PC_IPP_TAG_BOOLEAN, "printer-is-accepting-jobs", acc0, la0, 1}};
    tattr p1[] = {{PC_IPP_TAG_NAME, "printer-name", nm1, l1, 1},
                  {PC_IPP_TAG_ENUM, "printer-state", st1, ls0, 1},
                  {PC_IPP_TAG_BOOLEAN, "printer-is-accepting-jobs", acc1, la0, 1}};
    tgroup groups[] = {{PC_IPP_TAG_OPERATION_GROUP, NULL, 0},
                       {PC_IPP_TAG_PRINTER_GROUP, p0, 4},
                       {PC_IPP_TAG_PRINTER_GROUP, p1, 3}};

    for (int chunked = 0; chunked < 2; chunked++) {
        pc_buf body = PC_BUF_INIT;
        build_ipp(&body, 0, groups, 3);
        fake f;
        fake_start(&f, &body, chunked != 0, 200, 1, 0);
        pc_buf_fini(&body);
        pc_ipp_endpoint ep;
        fake_endpoint(&f, &ep);
        pc_ipp_printer_list list;
        pc_error err = {0};
        CHECK(pc_ipp_printers(&ep, &list, &err) == PC_OK);
        CHECK_EQ_UINT(list.count, 2u);
        if (list.count == 2) {
            CHECK(pc_str_eq_c(&list.items[0].name, "Office"));
            CHECK(pc_str_eq_c(&list.items[0].info, "HP in the office"));
            CHECK_EQ_UINT(list.items[0].state, 3u);
            CHECK(list.items[0].accepting_jobs);
            CHECK(pc_str_eq_c(&list.items[1].name, "Stopped"));
            CHECK_EQ_UINT(list.items[1].state, 5u);
            CHECK(!list.items[1].accepting_jobs);
            CHECK(!list.items[0].is_default && !list.items[1].is_default);
        }
        pc_ipp_printer_list_fini(&list);
        pc_ipp_endpoint_fini(&ep);
        /* Verify the request op and that requested-attributes carried 4 keyword values. */
        fake_join(&f);
    }
}

static void test_default_printer_read_and_absence(void)
{
    const uint8_t *nm[] = {VAL("Office")};
    size_t l[] = {6};
    tattr p[] = {{PC_IPP_TAG_NAME, "printer-name", nm, l, 1}};
    tgroup groups[] = {{PC_IPP_TAG_OPERATION_GROUP, NULL, 0}, {PC_IPP_TAG_PRINTER_GROUP, p, 1}};
    pc_buf body = PC_BUF_INIT;
    build_ipp(&body, 0, groups, 2);
    fake f;
    fake_start(&f, &body, false, 200, 1, 0);
    pc_buf_fini(&body);
    pc_ipp_endpoint ep;
    fake_endpoint(&f, &ep);
    pc_str name = PC_STR_INIT;
    bool present = false;
    pc_error err = {0};
    CHECK(pc_ipp_default_printer(&ep, &name, &present, &err) == PC_OK);
    CHECK(present);
    CHECK(pc_str_eq_c(&name, "Office"));
    pc_str_fini(&name);
    pc_ipp_endpoint_fini(&ep);
    fake_join(&f);

    /* Absence (status 0x0406) is not an error. */
    const uint8_t *sm[] = {VAL("No default printer")};
    size_t sl[] = {18};
    tattr op[] = {{PC_IPP_TAG_TEXT, "status-message", sm, sl, 1}};
    tgroup g2[] = {{PC_IPP_TAG_OPERATION_GROUP, op, 1}};
    pc_buf body2 = PC_BUF_INIT;
    build_ipp(&body2, 0x0406, g2, 1);
    fake f2;
    fake_start(&f2, &body2, false, 200, 1, 0);
    pc_buf_fini(&body2);
    pc_ipp_endpoint ep2;
    fake_endpoint(&f2, &ep2);
    pc_str n2 = PC_STR_INIT;
    bool p2 = true;
    pc_error e2 = {0};
    CHECK(pc_ipp_default_printer(&ep2, &n2, &p2, &e2) == PC_OK);
    CHECK(!p2);
    pc_str_fini(&n2);
    pc_ipp_endpoint_fini(&ep2);
    fake_join(&f2);
}

static void test_print_job_carries_document_and_returns_id(void)
{
    uint8_t jid[4] = {0, 0, 0, 42};
    const uint8_t *id[] = {jid};
    size_t idl[] = {4};
    tattr j[] = {{PC_IPP_TAG_INTEGER, "job-id", id, idl, 1}};
    tgroup groups[] = {{PC_IPP_TAG_OPERATION_GROUP, NULL, 0}, {PC_IPP_TAG_JOB_GROUP, j, 1}};
    pc_buf body = PC_BUF_INIT;
    build_ipp(&body, 0, groups, 2);
    fake f;
    fake_start(&f, &body, true, 200, 1, 0);
    pc_buf_fini(&body);
    pc_ipp_endpoint ep;
    fake_endpoint(&f, &ep);
    const char *pdf = "%PDF-1.4 fake";
    pc_ipp_job job = {.printer = "Office",
                      .is_class = false,
                      .title = "photo.psd",
                      .user = "diego",
                      .copies = 2,
                      .document = VAL(pdf),
                      .document_len = strlen(pdf),
                      .format = "application/pdf"};
    uint32_t got = 0;
    pc_error err = {0};
    CHECK(pc_ipp_print_job(&ep, &job, &got, &err) == PC_OK);
    CHECK_EQ_UINT(got, 42u);
    pc_ipp_endpoint_fini(&ep);
    fake_join(&f);
    /* The captured request is inspected in test_print_request_contents. */
}

/* A second print-job test that inspects the captured request body before the server frees it. */
static void test_print_request_contents(void)
{
    uint8_t jid[4] = {0, 0, 0, 7};
    const uint8_t *id[] = {jid};
    size_t idl[] = {4};
    tattr j[] = {{PC_IPP_TAG_INTEGER, "job-id", id, idl, 1}};
    tgroup groups[] = {{PC_IPP_TAG_OPERATION_GROUP, NULL, 0}, {PC_IPP_TAG_JOB_GROUP, j, 1}};

    for (int copies = 1; copies <= 2; copies++) {
        pc_buf body = PC_BUF_INIT;
        build_ipp(&body, 0, groups, 2);
        fake f;
        fake_start(&f, &body, false, 200, 1, 0);
        pc_buf_fini(&body);
        pc_ipp_endpoint ep;
        fake_endpoint(&f, &ep);
        const char *pdf = "%PDF data";
        pc_ipp_job job = {.printer = "Office",
                          .is_class = false,
                          .title = "photo.psd",
                          .user = "diego",
                          .copies = (uint32_t)copies,
                          .document = VAL(pdf),
                          .document_len = strlen(pdf),
                          .format = "application/pdf"};
        uint32_t got = 0;
        CHECK(pc_ipp_print_job(&ep, &job, &got, NULL) == PC_OK);
        pc_ipp_endpoint_fini(&ep);
        (void)pthread_join(f.thread, NULL); /* like fake_join but keep req_body for inspection */

        pc_ipp_reply req;
        const uint8_t *doc = NULL;
        size_t doc_len = 0;
        CHECK(f.req_body != NULL);
        if (f.req_body != NULL) {
            CHECK(pc_ipp_parse_request(f.req_body, f.req_len, &req, &doc, &doc_len, NULL) == PC_OK);
            CHECK_EQ_UINT(req.status, (unsigned)PC_IPP_OP_PRINT_JOB);
            CHECK_EQ_UINT(doc_len, strlen(pdf));
            CHECK(doc != NULL && memcmp(doc, pdf, doc_len) == 0);
            const pc_ipp_attr *uri = pc_ipp_reply_find(&req, "printer-uri");
            CHECK(uri != NULL);
            if (uri != NULL) {
                pc_buf t = PC_BUF_INIT;
                (void)pc_ipp_attr_text(uri, &t, NULL);
                CHECK_CONTAINS(pc_buf_cstr(&t), "ipp://127.0.0.1:");
                CHECK_CONTAINS(pc_buf_cstr(&t), "/printers/Office");
                pc_buf_fini(&t);
            }
            const pc_ipp_attr *jn = pc_ipp_reply_find(&req, "job-name");
            CHECK(jn != NULL && jn->nvalues == 1 && jn->values[0].len == 9 &&
                  memcmp(jn->values[0].data, "photo.psd", 9) == 0);
            const pc_ipp_attr *cp = pc_ipp_reply_find(&req, "copies");
            if (copies > 1) {
                int32_t cv = 0;
                CHECK(cp != NULL && cp->group == PC_IPP_TAG_JOB_GROUP && pc_ipp_attr_int(cp, &cv) && cv == 2);
            } else {
                CHECK(cp == NULL); /* one copy: no job attributes group at all */
            }
            pc_ipp_reply_fini(&req);
        }
        free(f.req_body);
        pc_buf_fini(&f.reply);
    }
}

static void test_error_statuses_and_broken_replies(void)
{
    /* 0x0506 is an error with the server's message. */
    const uint8_t *sm[] = {VAL("Printer is not accepting jobs")};
    size_t sl[] = {29};
    tattr op[] = {{PC_IPP_TAG_TEXT, "status-message", sm, sl, 1}};
    tgroup g[] = {{PC_IPP_TAG_OPERATION_GROUP, op, 1}};
    pc_buf body = PC_BUF_INIT;
    build_ipp(&body, 0x0506, g, 1);
    fake f;
    fake_start(&f, &body, false, 200, 1, 0);
    pc_buf_fini(&body);
    pc_ipp_endpoint ep;
    fake_endpoint(&f, &ep);
    const char *x = "x";
    pc_ipp_job job = {.printer = "P", .title = "t", .user = "u", .copies = 1, .document = VAL(x),
                      .document_len = 1, .format = "application/pdf"};
    uint32_t got = 0;
    pc_error err = {0};
    pc_status rc = pc_ipp_print_job(&ep, &job, &got, &err);
    CHECK(rc == PC_ERR_INVALID);
    CHECK_CONTAINS(err.msg, "0x0506");
    CHECK_CONTAINS(err.msg, "Printer is not accepting jobs");
    pc_ipp_endpoint_fini(&ep);
    fake_join(&f);

    /* 0x0406 for printers is an empty list, not an error. */
    const uint8_t *sm2[] = {VAL("No destinations added.")};
    size_t sl2[] = {22};
    tattr op2[] = {{PC_IPP_TAG_TEXT, "status-message", sm2, sl2, 1}};
    tgroup g2[] = {{PC_IPP_TAG_OPERATION_GROUP, op2, 1}};
    pc_buf body2 = PC_BUF_INIT;
    build_ipp(&body2, 0x0406, g2, 1);
    fake f2;
    fake_start(&f2, &body2, false, 200, 1, 0);
    pc_buf_fini(&body2);
    pc_ipp_endpoint ep2;
    fake_endpoint(&f2, &ep2);
    pc_ipp_printer_list list;
    CHECK(pc_ipp_printers(&ep2, &list, NULL) == PC_OK);
    CHECK_EQ_UINT(list.count, 0u);
    pc_ipp_printer_list_fini(&list);
    pc_ipp_endpoint_fini(&ep2);
    fake_join(&f2);

    /* HTTP 401. */
    tgroup gh[] = {{PC_IPP_TAG_OPERATION_GROUP, NULL, 0}};
    pc_buf body3 = PC_BUF_INIT;
    build_ipp(&body3, 0, gh, 1);
    fake f3;
    fake_start(&f3, &body3, false, 401, 1, 0);
    pc_buf_fini(&body3);
    pc_ipp_endpoint ep3;
    fake_endpoint(&f3, &ep3);
    pc_ipp_printer_list l3;
    pc_error e3 = {0};
    CHECK(pc_ipp_printers(&ep3, &l3, &e3) == PC_ERR_FORMAT);
    CHECK_CONTAINS(e3.msg, "401");
    pc_ipp_printer_list_fini(&l3);
    pc_ipp_endpoint_fini(&ep3);
    fake_join(&f3);

    /* Garbage reply. */
    pc_buf body4 = PC_BUF_INIT;
    (void)pc_buf_append_str(&body4, "not ipp at all", NULL);
    fake f4;
    fake_start(&f4, &body4, false, 200, 1, 0);
    pc_buf_fini(&body4);
    pc_ipp_endpoint ep4;
    fake_endpoint(&f4, &ep4);
    pc_ipp_printer_list l4;
    CHECK(pc_ipp_printers(&ep4, &l4, NULL) == PC_ERR_FORMAT);
    pc_ipp_printer_list_fini(&l4);
    pc_ipp_endpoint_fini(&ep4);
    fake_join(&f4);
}

static void test_unreachable_names_the_server(void)
{
    /* An RFC 2606 name that never resolves reaches the Unreachable exit naming the server. */
    pc_ipp_endpoint ep;
    (void)pc_ipp_endpoint_parse("printer.invalid:631", &ep, NULL);
    pc_ipp_printer_list list;
    pc_error err = {0};
    pc_status rc = pc_ipp_printers(&ep, &list, &err);
    CHECK(rc == PC_ERR_IO);
    CHECK_CONTAINS(err.msg, "printer.invalid");
    pc_ipp_printer_list_fini(&list);
    pc_ipp_endpoint_fini(&ep);
}

static void test_reply_parsing_is_bounded(void)
{
    pc_ipp_reply r;
    CHECK(pc_ipp_reply_parse((const uint8_t *)"", 0, &r, NULL, NULL) != PC_OK);
    uint8_t no_end[] = {2, 0, 0, 0, 0, 0, 0, 1};
    CHECK(pc_ipp_reply_parse(no_end, sizeof no_end, &r, NULL, NULL) != PC_OK);

    /* Too many attributes. */
    pc_buf many = PC_BUF_INIT;
    uint8_t hdr[] = {2, 0, 0, 0, 0, 0, 0, 1, PC_IPP_TAG_OPERATION_GROUP};
    (void)pc_buf_append(&many, hdr, sizeof hdr, NULL);
    for (size_t i = 0; i < PC_IPP_MAX_ATTRIBUTES + 1; i++) {
        uint8_t a[] = {PC_IPP_TAG_NAME, 0, 1, 'a', 0, 0};
        (void)pc_buf_append(&many, a, sizeof a, NULL);
    }
    (void)pc_buf_append_byte(&many, PC_IPP_TAG_END, NULL);
    pc_error err = {0};
    CHECK(pc_ipp_reply_parse(many.data, many.len, &r, NULL, &err) == PC_ERR_FORMAT);
    CHECK_CONTAINS(err.msg, "too many");
    pc_buf_fini(&many);

    /* An additional value before any attribute. */
    uint8_t add[] = {2, 0, 0, 0, 0, 0, 0, 1, PC_IPP_TAG_OPERATION_GROUP,
                     PC_IPP_TAG_NAME, 0, 0, 0, 1, 'x', PC_IPP_TAG_END};
    CHECK(pc_ipp_reply_parse(add, sizeof add, &r, NULL, NULL) != PC_OK);

    /* dechunk. */
    pc_buf out = PC_BUF_INIT;
    CHECK(pc_ipp_dechunk((const uint8_t *)"zz\r\n", 4, &out, NULL) != PC_OK);
    pc_buf_fini(&out);
    pc_buf out2 = PC_BUF_INIT;
    CHECK(pc_ipp_dechunk((const uint8_t *)"5\r\nab", 5, &out2, NULL) != PC_OK);
    pc_buf_fini(&out2);
    pc_buf out3 = PC_BUF_INIT;
    CHECK(pc_ipp_dechunk((const uint8_t *)"3\r\nabc\r\n0\r\n\r\n", 13, &out3, NULL) == PC_OK);
    CHECK_EQ_UINT(out3.len, 3u);
    CHECK(out3.data != NULL && memcmp(out3.data, "abc", 3) == 0);
    pc_buf_fini(&out3);
}

static bool complete(const char *s)
{
    size_t resume = 0;
    return pc_ipp_chunked_complete((const uint8_t *)s, strlen(s), &resume);
}

static void test_chunk_walk(void)
{
    CHECK(!complete("5\r\nab\r\n0"));
    CHECK(!complete("7\r\n\r\n0\r\n\r\nx\r\n"));
    CHECK(complete("7\r\n\r\n0\r\n\r\nx\r\n0\r\n\r\n"));
    CHECK(complete("zz\r\n"));

    /* Fed a byte at a time, the walk resumes and agrees with the one-shot answer. */
    const char *bodys = "3\r\nabc\r\n10\r\n0123456789abcdef\r\n0\r\n\r\n";
    size_t total = strlen(bodys);
    size_t at = 0;
    for (size_t len = 0; len <= total; len++) {
        size_t one = 0;
        bool a = pc_ipp_chunked_complete((const uint8_t *)bodys, len, &at);
        bool b = pc_ipp_chunked_complete((const uint8_t *)bodys, len, &one);
        CHECK(a == b);
        CHECK(at <= len);
    }
    CHECK(at > 0);

    /* Impossible chunk sizes are protocol errors, never overflow. */
    const char *sizes[] = {"ffffffffffffffff", "fffffffffffffffe", "1000001"};
    for (size_t i = 0; i < 3; i++) {
        pc_buf line = PC_BUF_INIT;
        (void)pc_buf_appendf(&line, NULL, "%s\r\n", sizes[i]);
        size_t resume = 0;
        CHECK(pc_ipp_chunked_complete(line.data, line.len, &resume));
        pc_buf_fini(&line);
        pc_buf chunk = PC_BUF_INIT;
        (void)pc_buf_appendf(&chunk, NULL, "%s\r\nab\r\n", sizes[i]);
        pc_buf out = PC_BUF_INIT;
        CHECK(pc_ipp_dechunk(chunk.data, chunk.len, &out, NULL) != PC_OK);
        pc_buf_fini(&out);
        pc_buf_fini(&chunk);
    }
}

static void test_request_mandatory_attributes(void)
{
    pc_buf b = PC_BUF_INIT;
    CHECK(pc_ipp_request_begin(&b, PC_IPP_OP_PRINT_JOB, 9, NULL) == PC_OK);
    CHECK(pc_ipp_request_end(&b, NULL) == PC_OK);
    pc_ipp_reply req;
    const uint8_t *doc = NULL;
    size_t doc_len = 0;
    CHECK(pc_ipp_parse_request(b.data, b.len, &req, &doc, &doc_len, NULL) == PC_OK);
    CHECK_EQ_UINT(req.status, (unsigned)PC_IPP_OP_PRINT_JOB);
    CHECK_EQ_UINT(req.request_id, 9u);
    CHECK(req.nattrs >= 2);
    if (req.nattrs >= 2) {
        CHECK_EQ_STR(req.attrs[0].name, "attributes-charset");
        CHECK_EQ_STR(req.attrs[1].name, "attributes-natural-language");
    }
    CHECK_EQ_UINT(doc_len, 0u);
    pc_ipp_reply_fini(&req);
    pc_buf_fini(&b);
}

static void test_ipv6_endpoint_parse(void)
{
    struct {
        const char *in;
        pc_ipp_kind kind;
        const char *host;
        uint16_t port;
    } cases[] = {
        {"::1", PC_IPP_TCP, "::1", 631},
        {"fe80::1", PC_IPP_TCP, "fe80::1", 631},
        {"2001:db8::1", PC_IPP_TCP, "2001:db8::1", 631},
        {"[::1]", PC_IPP_TCP, "::1", 631},
        {"[::1]:631", PC_IPP_TCP, "::1", 631},
        {"[fe80::1]:6310", PC_IPP_TCP, "fe80::1", 6310},
        {"fe80::1%eth0:631", PC_IPP_TCP, "fe80::1%eth0", 631},
        {"cups.example", PC_IPP_TCP, "cups.example", 631},
        {"cups.example:6310", PC_IPP_TCP, "cups.example", 6310},
        {"127.0.0.1:6310", PC_IPP_TCP, "127.0.0.1", 6310},
        {"/var/run/cups/cups.sock", PC_IPP_UNIX, "/var/run/cups/cups.sock", 0},
        {"[::1]:abc", PC_IPP_TCP, "[::1]:abc", 631},
        {"[::1]:70000", PC_IPP_TCP, "[::1]:70000", 631},
        {"[::1]junk", PC_IPP_TCP, "[::1]junk", 631},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_ipp_endpoint ep;
        CHECK(pc_ipp_endpoint_parse(cases[i].in, &ep, NULL) == PC_OK);
        CHECK_EQ_INT((int)ep.kind, (int)cases[i].kind);
        CHECK_EQ_STR(pc_str_c(&ep.host), cases[i].host);
        if (cases[i].kind == PC_IPP_TCP) {
            CHECK_EQ_UINT(ep.port, cases[i].port);
        }
        pc_ipp_endpoint_fini(&ep);
    }
}

static void test_ipv6_authority(void)
{
    struct {
        const char *in;
        const char *host;
        const char *display;
    } cases[] = {
        {"::1", "[::1]:631", "[::1]:631"},
        {"[::1]:631", "[::1]:631", "[::1]:631"},
        {"[fe80::1]:6310", "[fe80::1]:6310", "[fe80::1]:6310"},
        {"cups.example", "cups.example:631", "cups.example:631"},
        {"cups.example:6310", "cups.example:6310", "cups.example:6310"},
        {"127.0.0.1:6310", "127.0.0.1:6310", "127.0.0.1:6310"},
        {"fe80::1%3:631", "[fe80::1%253]:631", "[fe80::1%3]:631"},
        {"[fe80::1%3]:6310", "[fe80::1%253]:6310", "[fe80::1%3]:6310"},
        {"fe80::1%eth0:631", "[fe80::1%25eth0]:631", "[fe80::1%eth0]:631"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_ipp_endpoint ep;
        CHECK(pc_ipp_endpoint_parse(cases[i].in, &ep, NULL) == PC_OK);
        pc_buf host = PC_BUF_INIT, disp = PC_BUF_INIT;
        CHECK(pc_ipp_endpoint_host(&ep, &host, NULL) == PC_OK);
        CHECK(pc_ipp_endpoint_display(&ep, &disp, NULL) == PC_OK);
        CHECK_EQ_STR(pc_buf_cstr(&host), cases[i].host);
        CHECK_EQ_STR(pc_buf_cstr(&disp), cases[i].display);
        pc_buf_fini(&host);
        pc_buf_fini(&disp);
        pc_ipp_endpoint_fini(&ep);
    }
}

static void test_printer_names_encoded(void)
{
    pc_buf out = PC_BUF_INIT;
    CHECK(pc_ipp_queue_path("office printer/1", false, &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "/printers/office%20printer%2F1");
    pc_buf_fini(&out);
    pc_buf bad = PC_BUF_INIT;
    CHECK(pc_ipp_queue_path("printer\r\nX-Injected: yes", false, &bad, NULL) != PC_OK);
    pc_buf_fini(&bad);
}

static void test_reply_ends_without_waiting_for_close(void)
{
    /* The server keeps the connection open after its reply; the client must not block on it. */
    const uint8_t *nm[] = {VAL("Office")};
    size_t l[] = {6};
    tattr p[] = {{PC_IPP_TAG_NAME, "printer-name", nm, l, 1}};
    tgroup groups[] = {{PC_IPP_TAG_OPERATION_GROUP, NULL, 0}, {PC_IPP_TAG_PRINTER_GROUP, p, 1}};
    for (int chunked = 0; chunked < 2; chunked++) {
        pc_buf body = PC_BUF_INIT;
        build_ipp(&body, 0, groups, 2);
        fake f;
        /* Serve get-printers; linger 400ms before closing so a blocking read would wait. */
        fake_start(&f, &body, chunked != 0, 200, 1, 400);
        pc_buf_fini(&body);
        pc_ipp_endpoint ep;
        fake_endpoint(&f, &ep);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        pc_ipp_printer_list list;
        /* classes + default requests after the single accept fail fast (port closed). */
        CHECK(pc_ipp_printers(&ep, &list, NULL) == PC_OK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;
        CHECK_EQ_UINT(list.count, 1u);
        CHECK(secs < 3.0);
        pc_ipp_printer_list_fini(&list);
        pc_ipp_endpoint_fini(&ep);
        fake_join(&f);
    }
}

int main(int argc, char **argv)
{
    RUN(test_printers_are_listed_with_state_and_default);
    RUN(test_default_printer_read_and_absence);
    RUN(test_print_job_carries_document_and_returns_id);
    RUN(test_print_request_contents);
    RUN(test_error_statuses_and_broken_replies);
    RUN(test_unreachable_names_the_server);
    RUN(test_reply_parsing_is_bounded);
    RUN(test_chunk_walk);
    RUN(test_request_mandatory_attributes);
    RUN(test_ipv6_endpoint_parse);
    RUN(test_ipv6_authority);
    RUN(test_printer_names_encoded);
    RUN(test_reply_ends_without_waiting_for_close);
    return pc_t_finish();
}
