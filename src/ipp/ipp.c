/* A small IPP (RFC 8010/8011) client for the local CUPS server (port of crates/ipp/src/lib.rs).
 * Talks to CUPS the way `lp` does but in-process, over the CUPS domain socket or TCP. Three
 * operations: list the printers, read the default, submit a PDF. Every read is bounded and every
 * malformed reply is an error (REWRITE_PLAN rule 9). */
#include "ipp/internal.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "base/mem.h"

/* Timeouts (seconds): connecting, each request (a Print-Job carries the whole PDF), and the whole
 * reply however it trickles in (the per-read timeout only covers one read). */
enum { CONNECT_SECS = 3, REQUEST_SECS = 60, REPLY_DEADLINE_SECS = 120 };

static size_t find_crlf_crlf(const uint8_t *raw, size_t len);

/* ---- reply attributes ---------------------------------------------------------------------- */

static void value_fini(pc_ipp_value *v)
{
    free(v->data);
    v->data = NULL;
    v->len = 0;
}

static void attr_fini(pc_ipp_attr *a)
{
    for (size_t i = 0; i < a->nvalues; i++) {
        value_fini(&a->values[i]);
    }
    free(a->values);
    free(a->name);
    memset(a, 0, sizeof *a);
}

void pc_ipp_reply_fini(pc_ipp_reply *r)
{
    if (r == NULL) {
        return;
    }
    for (size_t i = 0; i < r->nattrs; i++) {
        attr_fini(&r->attrs[i]);
    }
    free(r->attrs);
    memset(r, 0, sizeof *r);
}

/* Append a value (len bytes, copied) to attribute a. */
static pc_status attr_push_value(pc_ipp_attr *a, const uint8_t *data, size_t len, pc_error *err)
{
    pc_ipp_value *grown = pc_grow_array(a->values, &a->cap_values, a->nvalues + 1, sizeof *a->values);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing an IPP reply");
    }
    a->values = grown;
    uint8_t *copy = NULL;
    if (len > 0) {
        copy = pc_malloc(len);
        if (copy == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing an IPP reply");
        }
        memcpy(copy, data, len);
    }
    a->values[a->nvalues].data = copy;
    a->values[a->nvalues].len = len;
    a->nvalues++;
    return PC_OK;
}

static pc_status reply_push_attr(pc_ipp_reply *r, uint8_t group, uint8_t tag, const uint8_t *name,
                                 size_t name_len, const uint8_t *value, size_t value_len, pc_error *err)
{
    pc_ipp_attr *grown = pc_grow_array(r->attrs, &r->cap_attrs, r->nattrs + 1, sizeof *r->attrs);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing an IPP reply");
    }
    r->attrs = grown;
    pc_ipp_attr *a = &r->attrs[r->nattrs];
    memset(a, 0, sizeof *a);
    a->group = group;
    a->tag = tag;
    a->name = pc_strndup((const char *)name, name_len);
    if (a->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory parsing an IPP reply");
    }
    r->nattrs++; /* owned now, so attr_fini runs on cleanup even if the value push fails */
    return attr_push_value(a, value, value_len, err);
}

/* Parse the header then groups of attributes until the end tag (RFC 8010 §3). */
static pc_status reply_parse_at(const uint8_t *b, size_t len, pc_ipp_reply *out, size_t *out_end,
                                pc_error *err)
{
    memset(out, 0, sizeof *out);
    if (len < 9) {
        return pc_fail(err, PC_ERR_FORMAT, "reply shorter than an IPP header");
    }
    out->status = (uint16_t)((b[2] << 8) | b[3]);
    out->request_id = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    size_t pos = 8;
    uint8_t group = 0;
    for (;;) {
        if (pos >= len) {
            pc_ipp_reply_fini(out);
            return pc_fail(err, PC_ERR_FORMAT, "reply ends before the end-of-attributes tag");
        }
        uint8_t tag = b[pos++];
        if (tag == PC_IPP_TAG_END) {
            if (out_end != NULL) {
                *out_end = pos;
            }
            return PC_OK;
        }
        if (tag < 0x10) {
            group = tag;
            continue;
        }
        if (pos + 2 > len) {
            pc_ipp_reply_fini(out);
            return pc_fail(err, PC_ERR_FORMAT, "cut attribute");
        }
        size_t name_len = (size_t)((b[pos] << 8) | b[pos + 1]);
        pos += 2;
        if (pos + name_len > len) {
            pc_ipp_reply_fini(out);
            return pc_fail(err, PC_ERR_FORMAT, "cut attribute name");
        }
        const uint8_t *name = b + pos;
        pos += name_len;
        if (pos + 2 > len) {
            pc_ipp_reply_fini(out);
            return pc_fail(err, PC_ERR_FORMAT, "cut attribute");
        }
        size_t value_len = (size_t)((b[pos] << 8) | b[pos + 1]);
        pos += 2;
        if (pos + value_len > len) {
            pc_ipp_reply_fini(out);
            return pc_fail(err, PC_ERR_FORMAT, "cut attribute value");
        }
        const uint8_t *value = b + pos;
        pos += value_len;
        if (name_len == 0) {
            /* An additional value of the previous attribute. */
            if (out->nattrs == 0 || out->attrs[out->nattrs - 1].group != group) {
                pc_ipp_reply_fini(out);
                return pc_fail(err, PC_ERR_FORMAT, "an additional value with no attribute before it");
            }
            if (attr_push_value(&out->attrs[out->nattrs - 1], value, value_len, err) != PC_OK) {
                pc_ipp_reply_fini(out);
                return PC_ERR_NOMEM;
            }
        } else {
            if (reply_push_attr(out, group, tag, name, name_len, value, value_len, err) != PC_OK) {
                pc_ipp_reply_fini(out);
                return PC_ERR_NOMEM;
            }
        }
        if (out->nattrs > PC_IPP_MAX_ATTRIBUTES) {
            pc_ipp_reply_fini(out);
            return pc_fail(err, PC_ERR_FORMAT, "too many attributes");
        }
    }
}

pc_status pc_ipp_reply_parse(const uint8_t *b, size_t len, pc_ipp_reply *out, size_t *out_end,
                             pc_error *err)
{
    return reply_parse_at(b, len, out, out_end, err);
}

pc_status pc_ipp_attr_text(const pc_ipp_attr *a, pc_buf *out, pc_error *err)
{
    if (a->nvalues == 0) {
        return PC_OK;
    }
    return pc_buf_append(out, a->values[0].data, a->values[0].len, err);
}

bool pc_ipp_attr_int(const pc_ipp_attr *a, int32_t *out)
{
    if (a->nvalues == 0 || a->values[0].len != 4) {
        return false;
    }
    const uint8_t *v = a->values[0].data;
    *out = (int32_t)(((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3]);
    return true;
}

const pc_ipp_attr *pc_ipp_reply_find(const pc_ipp_reply *r, const char *name)
{
    for (size_t i = 0; i < r->nattrs; i++) {
        if (strcmp(r->attrs[i].name, name) == 0) {
            return &r->attrs[i];
        }
    }
    return NULL;
}

/* The server's own message for an error status, or a name for the code, appended to out. */
static void status_message(const pc_ipp_reply *r, pc_buf *out)
{
    const pc_ipp_attr *m = pc_ipp_reply_find(r, "status-message");
    if (m != NULL && m->nvalues > 0 && m->values[0].len > 0) {
        (void)pc_buf_append(out, m->values[0].data, m->values[0].len, NULL);
        return;
    }
    const char *s;
    switch (r->status) {
    case 0x0400: s = "bad request"; break;
    case 0x0401: s = "forbidden"; break;
    case 0x0402: s = "not authenticated"; break;
    case 0x0403: s = "not authorized"; break;
    case 0x0406: s = "not found"; break;
    case 0x0409: s = "unsupported document format"; break;
    case 0x0500: s = "internal server error"; break;
    case 0x0506: s = "the printer is not accepting jobs"; break;
    default: s = "error"; break;
    }
    (void)pc_buf_append_str(out, s, NULL);
}

/* ---- request builder ----------------------------------------------------------------------- */

static pc_status append_u16(pc_buf *b, uint16_t v, pc_error *err)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return pc_buf_append(b, x, 2, err);
}

pc_status pc_ipp_request_attr(pc_buf *b, uint8_t tag, const char *name, const void *value,
                              size_t value_len, pc_error *err)
{
    /* IPP lengths are 16-bit; longer names and values are cut, both the length and the bytes. */
    size_t name_len = name != NULL ? strlen(name) : 0;
    if (name_len > 0xffff) {
        name_len = 0xffff;
    }
    if (value_len > 0xffff) {
        value_len = 0xffff;
    }
    if (pc_buf_append_byte(b, tag, err) != PC_OK || append_u16(b, (uint16_t)name_len, err) != PC_OK ||
        pc_buf_append(b, name, name_len, err) != PC_OK || append_u16(b, (uint16_t)value_len, err) != PC_OK ||
        pc_buf_append(b, value, value_len, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

pc_status pc_ipp_request_begin(pc_buf *b, uint16_t op, uint32_t id, pc_error *err)
{
    uint8_t head[8] = {2,
                       0,
                       (uint8_t)(op >> 8),
                       (uint8_t)op,
                       (uint8_t)(id >> 24),
                       (uint8_t)(id >> 16),
                       (uint8_t)(id >> 8),
                       (uint8_t)id};
    if (pc_buf_append(b, head, sizeof head, err) != PC_OK ||
        pc_buf_append_byte(b, PC_IPP_TAG_OPERATION_GROUP, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (pc_ipp_request_attr(b, PC_IPP_TAG_CHARSET, "attributes-charset", "utf-8", 5, err) != PC_OK ||
        pc_ipp_request_attr(b, PC_IPP_TAG_LANGUAGE, "attributes-natural-language", "en", 2, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

pc_status pc_ipp_request_integer(pc_buf *b, const char *name, int32_t v, pc_error *err)
{
    uint8_t x[4] = {(uint8_t)((uint32_t)v >> 24), (uint8_t)((uint32_t)v >> 16), (uint8_t)((uint32_t)v >> 8),
                    (uint8_t)v};
    return pc_ipp_request_attr(b, PC_IPP_TAG_INTEGER, name, x, 4, err);
}

pc_status pc_ipp_request_group(pc_buf *b, uint8_t tag, pc_error *err)
{
    return pc_buf_append_byte(b, tag, err);
}

pc_status pc_ipp_request_end(pc_buf *b, pc_error *err)
{
    return pc_buf_append_byte(b, PC_IPP_TAG_END, err);
}

/* Append an attribute with several keyword values (the extra ones carry an empty name). */
static pc_status request_attr_many(pc_buf *b, uint8_t tag, const char *name, const char *const *values,
                                   size_t n, pc_error *err)
{
    for (size_t i = 0; i < n; i++) {
        const char *nm = i == 0 ? name : "";
        if (pc_ipp_request_attr(b, tag, nm, values[i], strlen(values[i]), err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

/* ---- chunked transfer ---------------------------------------------------------------------- */

/* Index of the first "\r\n" at or after `from` in b[..len], or SIZE_MAX. */
static size_t find_crlf(const uint8_t *b, size_t len, size_t from)
{
    for (size_t i = from; i + 1 < len; i++) {
        if (b[i] == '\r' && b[i + 1] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Parse a hex chunk size (the part before any ';'); true on success. */
static bool parse_chunk_size(const uint8_t *line, size_t len, size_t *out)
{
    /* Stop at ';' and trim surrounding whitespace. */
    size_t end = 0;
    while (end < len && line[end] != ';') {
        end++;
    }
    size_t start = 0;
    while (start < end && (line[start] == ' ' || line[start] == '\t')) {
        start++;
    }
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        end--;
    }
    if (start == end) {
        return false;
    }
    size_t v = 0;
    for (size_t i = start; i < end; i++) {
        uint8_t c = line[i];
        unsigned d;
        if (c >= '0' && c <= '9') {
            d = (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
        if (v > (SIZE_MAX - d) / 16u) {
            v = SIZE_MAX; /* saturate: an impossible size is reported as complete by the caller */
        } else {
            v = v * 16u + d;
        }
    }
    *out = v;
    return true;
}

bool pc_ipp_chunked_complete(const uint8_t *body, size_t len, size_t *resume)
{
    size_t pos = *resume <= len ? *resume : len;
    for (;;) {
        *resume = pos;
        size_t nl = find_crlf(body, len, pos);
        if (nl == SIZE_MAX) {
            return false;
        }
        size_t size;
        if (!parse_chunk_size(body + pos, nl - pos, &size)) {
            return true; /* a malformed size: complete, so dechunk reports it */
        }
        pos = nl + 2;
        if (size == 0) {
            return true;
        }
        /* A size past the reply cap can never be satisfied: complete, so dechunk rejects it. */
        if (size > PC_IPP_MAX_REPLY || size > SIZE_MAX - 2) {
            return true;
        }
        if (size + 2 > len - pos) {
            return false;
        }
        pos += size + 2;
    }
}

pc_status pc_ipp_dechunk(const uint8_t *b, size_t len, pc_buf *out, pc_error *err)
{
    size_t pos = 0;
    for (;;) {
        size_t nl = find_crlf(b, len, pos);
        if (nl == SIZE_MAX) {
            return pc_fail(err, PC_ERR_FORMAT, "cut chunk size");
        }
        size_t size;
        if (!parse_chunk_size(b + pos, nl - pos, &size)) {
            return pc_fail(err, PC_ERR_FORMAT, "bad chunk size");
        }
        pos = nl + 2;
        if (size == 0) {
            return PC_OK;
        }
        if (size > SIZE_MAX - pos || pos + size > len) {
            return pc_fail(err, PC_ERR_FORMAT, "cut chunk");
        }
        if (pc_buf_append(out, b + pos, size, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        if (out->len > PC_IPP_MAX_REPLY) {
            return pc_fail(err, PC_ERR_FORMAT, "the reply is larger than 16 MiB");
        }
        pos += size;
        if (pos + 2 > len) {
            return pc_fail(err, PC_ERR_FORMAT, "cut chunk end");
        }
        pos += 2; /* trailing CRLF */
    }
}

/* ---- parse_request (fake server helper) ---------------------------------------------------- */

pc_status pc_ipp_parse_request(const uint8_t *b, size_t len, pc_ipp_reply *out_reply,
                               const uint8_t **out_doc, size_t *out_doc_len, pc_error *err)
{
    size_t end = 0;
    pc_status rc = reply_parse_at(b, len, out_reply, &end, err);
    if (rc != PC_OK) {
        return rc;
    }
    *out_doc = b + end;
    *out_doc_len = len - end;
    return PC_OK;
}

/* ---- queue path ---------------------------------------------------------------------------- */

pc_status pc_ipp_queue_path(const char *name, bool is_class, pc_buf *out, pc_error *err)
{
    if (name[0] == '\0') {
        return pc_fail(err, PC_ERR_FORMAT, "printer name contains an invalid control character");
    }
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return pc_fail(err, PC_ERR_FORMAT, "printer name contains an invalid control character");
        }
    }
    if (pc_buf_appendf(out, err, "/%s/", is_class ? "classes" : "printers") != PC_OK) {
        return PC_ERR_NOMEM;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p != '\0'; p++) {
        unsigned char c = *p;
        bool unreserved = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          c == '-' || c == '.' || c == '_' || c == '~';
        pc_status rc = unreserved ? pc_buf_append_byte(out, c, err) : pc_buf_appendf(out, err, "%%%02X", c);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* ---- sockets ------------------------------------------------------------------------------- */

/* Connect to the endpoint, returning a socket fd with request read/write timeouts set, or -1 with
 * *err an Unreachable (PC_ERR_IO). */
static int ipp_connect(const pc_ipp_endpoint *ep, pc_error *err)
{
    struct timeval req = {REQUEST_SECS, 0};
    if (ep->kind == PC_IPP_UNIX) {
        const char *path = pc_str_c(&ep->host);
        struct stat st;
        if (stat(path, &st) != 0) {
            (void)pc_fail(err, PC_ERR_IO, "%s does not exist", path);
            return -1;
        }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        if (strlen(path) >= sizeof addr.sun_path) {
            (void)pc_fail(err, PC_ERR_IO, "%s: socket path too long", path);
            return -1;
        }
        memcpy(addr.sun_path, path, strlen(path));
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            (void)pc_fail_errno(err, PC_ERR_IO, errno, "%s", path);
            return -1;
        }
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
            (void)pc_fail_errno(err, PC_ERR_IO, errno, "%s", path);
            close(fd);
            return -1;
        }
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &req, sizeof req);
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &req, sizeof req);
        return fd;
    }

    /* TCP: resolve, then connect to the first address with a CONNECT-second timeout. */
    const char *host = pc_str_c(&ep->host);
    char port_str[16];
    (void)snprintf(port_str, sizeof port_str, "%u", (unsigned)ep->port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || res == NULL) {
        (void)pc_fail(err, PC_ERR_IO, "%s:%u: %s", host, (unsigned)ep->port, gai_strerror(gai));
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return -1;
    }
    int last_errno = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            struct timeval to = {CONNECT_SECS, 0};
            rc = select(fd + 1, NULL, &wset, NULL, &to);
            if (rc > 0) {
                int so_err = 0;
                socklen_t slen = sizeof so_err;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err == 0) {
                    rc = 0;
                } else {
                    errno = so_err != 0 ? so_err : ETIMEDOUT;
                    rc = -1;
                }
            } else {
                errno = rc == 0 ? ETIMEDOUT : errno;
                rc = -1;
            }
        }
        if (rc == 0) {
            (void)fcntl(fd, F_SETFL, flags);
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &req, sizeof req);
            (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &req, sizeof req);
            freeaddrinfo(res);
            return fd;
        }
        last_errno = errno;
        close(fd);
    }
    freeaddrinfo(res);
    (void)pc_fail_errno(err, PC_ERR_IO, last_errno, "%s:%u", host, (unsigned)ep->port);
    return -1;
}

static pc_status write_all(int fd, const uint8_t *data, size_t len, pc_error *err)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return pc_fail_errno(err, PC_ERR_FORMAT, errno, "sending");
        }
        off += (size_t)n;
    }
    return PC_OK;
}

static double monotonic_secs(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Lowercased value of header `name` from the header block raw[..head_end], or NULL. Caller frees. */
static char *header_value(const char *head, const char *name)
{
    size_t nlen = strlen(name);
    const char *line = head;
    /* Skip the status line. */
    const char *nl = strstr(line, "\r\n");
    if (nl == NULL) {
        return NULL;
    }
    line = nl + 2;
    while (*line != '\0') {
        nl = strstr(line, "\r\n");
        size_t line_len = nl != NULL ? (size_t)(nl - line) : strlen(line);
        const char *colon = memchr(line, ':', line_len);
        if (colon != NULL) {
            size_t key_len = (size_t)(colon - line);
            if (key_len == nlen) {
                bool match = true;
                for (size_t i = 0; i < nlen; i++) {
                    char c = line[i];
                    if (c >= 'A' && c <= 'Z') {
                        c = (char)(c - 'A' + 'a');
                    }
                    if (c != name[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    const char *v = colon + 1;
                    size_t vlen = line_len - key_len - 1;
                    while (vlen > 0 && (*v == ' ' || *v == '\t')) {
                        v++;
                        vlen--;
                    }
                    while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t' || v[vlen - 1] == '\r')) {
                        vlen--;
                    }
                    char *out = pc_malloc(vlen + 1);
                    if (out == NULL) {
                        return NULL;
                    }
                    for (size_t i = 0; i < vlen; i++) {
                        char c = v[i];
                        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                    }
                    out[vlen] = '\0';
                    return out;
                }
            }
        }
        if (nl == NULL) {
            break;
        }
        line = nl + 2;
    }
    return NULL;
}

/* Split an HTTP/1.1 response into status and the body (header block parsed separately). Returns
 * the HTTP status, or -1 on a malformed header, and sets *body_at to the body offset. */
static int http_status(const uint8_t *raw, size_t len, size_t *body_at)
{
    size_t end = find_crlf_crlf(raw, len);
    if (end == SIZE_MAX) {
        return -1;
    }
    *body_at = end + 4;
    /* "HTTP/1.1 NNN ...": the status is the second token. */
    size_t i = 0;
    while (i < len && raw[i] != ' ' && raw[i] != '\r') {
        i++;
    }
    while (i < len && raw[i] == ' ') {
        i++;
    }
    int code = 0;
    bool any = false;
    while (i < len && raw[i] >= '0' && raw[i] <= '9') {
        code = code * 10 + (raw[i] - '0');
        any = true;
        i++;
    }
    return any ? code : -1;
}

/* Index of "\r\n\r\n" in raw[..len], or SIZE_MAX. Defined above http_status via forward need. */
static size_t find_crlf_crlf(const uint8_t *raw, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Read one HTTP response: the headers, then as much body as Content-Length says, or up to the
 * last chunk, or to end of stream. */
static pc_status read_response(int fd, pc_buf *raw, pc_error *err)
{
    uint8_t buf[16 * 1024];
    size_t body_at = SIZE_MAX;
    bool have_expected = false;
    size_t expected = 0;
    bool chunked = false;
    size_t chunk_at = 0;
    double started = monotonic_secs();
    for (;;) {
        if (monotonic_secs() - started > (double)REPLY_DEADLINE_SECS) {
            return pc_fail(err, PC_ERR_FORMAT, "the print server took too long to reply");
        }
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return pc_fail_errno(err, PC_ERR_FORMAT, errno, "reading");
        }
        if (n == 0) {
            return PC_OK;
        }
        if (pc_buf_append(raw, buf, (size_t)n, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        if (raw->len > PC_IPP_MAX_REPLY) {
            return pc_fail(err, PC_ERR_FORMAT, "the reply is larger than 16 MiB");
        }
        if (body_at == SIZE_MAX) {
            size_t end = find_crlf_crlf(raw->data, raw->len);
            if (end != SIZE_MAX) {
                body_at = end + 4;
                /* Terminate the header block so string helpers stay in bounds. */
                char *head = pc_strndup((const char *)raw->data, end);
                if (head == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory");
                }
                char *cl = header_value(head, "content-length");
                char *te = header_value(head, "transfer-encoding");
                if (cl != NULL) {
                    char *endp = NULL;
                    unsigned long v = strtoul(cl, &endp, 10);
                    if (endp != cl) {
                        have_expected = true;
                        expected = (size_t)v;
                    }
                }
                if (te != NULL && strstr(te, "chunked") != NULL) {
                    chunked = true;
                }
                free(cl);
                free(te);
                free(head);
            }
        }
        if (body_at == SIZE_MAX) {
            continue;
        }
        const uint8_t *body = raw->data + body_at;
        size_t body_len = raw->len - body_at;
        bool complete;
        if (chunked) {
            complete = pc_ipp_chunked_complete(body, body_len, &chunk_at);
        } else if (have_expected) {
            complete = body_len >= expected;
        } else {
            complete = false;
        }
        if (complete) {
            if (have_expected && !chunked) {
                pc_buf_truncate(raw, body_at + expected);
            }
            return PC_OK;
        }
    }
}

/* ---- post ---------------------------------------------------------------------------------- */

/* POST body to path and parse the reply. On an IPP error status, *out_ipp_status (if non-NULL)
 * gets the status and the function returns PC_ERR_INVALID. On an HTTP/transport error it returns
 * PC_ERR_FORMAT/PC_ERR_IO. On OK *reply is filled (caller pc_ipp_reply_fini). */
static pc_status ipp_post(const pc_ipp_endpoint *ep, const char *path, const uint8_t *body, size_t len,
                          pc_ipp_reply *reply, uint16_t *out_ipp_status, pc_error *err)
{
    if (out_ipp_status != NULL) {
        *out_ipp_status = 0;
    }
    int fd = ipp_connect(ep, err);
    if (fd < 0) {
        return err != NULL ? err->code : PC_ERR_IO;
    }
    pc_buf host = PC_BUF_INIT;
    pc_buf head = PC_BUF_INIT;
    pc_buf raw = PC_BUF_INIT;
    pc_buf dec = PC_BUF_INIT;
    pc_status rc = pc_ipp_endpoint_host(ep, &host, err);
    if (rc == PC_OK) {
        rc = pc_buf_appendf(&head, err,
                            "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/ipp\r\n"
                            "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                            path, pc_buf_cstr(&host), len);
    }
    if (rc == PC_OK) {
        rc = write_all(fd, head.data, head.len, err);
    }
    if (rc == PC_OK) {
        rc = write_all(fd, body, len, err);
    }
    if (rc == PC_OK) {
        rc = read_response(fd, &raw, err);
    }
    close(fd);
    if (rc != PC_OK) {
        goto done;
    }

    size_t body_at = 0;
    int status = http_status(raw.data, raw.len, &body_at);
    if (status < 0) {
        rc = pc_fail(err, PC_ERR_FORMAT, "no HTTP header");
        goto done;
    }
    if (status != 200) {
        if (status == 401) {
            rc = pc_fail(err, PC_ERR_FORMAT, "the print server asks for authentication (HTTP 401)");
        } else if (status == 403) {
            rc = pc_fail(err, PC_ERR_FORMAT, "the print server forbids this request (HTTP 403)");
        } else {
            rc = pc_fail(err, PC_ERR_FORMAT, "HTTP %d", status);
        }
        goto done;
    }

    if (raw.data == NULL) { /* unreachable once status >= 0 (needs a header), but keeps the
                             * pointer arithmetic below provably in bounds */
        rc = pc_fail(err, PC_ERR_FORMAT, "empty reply");
        goto done;
    }
    const uint8_t *ipp_body = raw.data + body_at;
    size_t ipp_len = raw.len - body_at;
    char *te = NULL;
    {
        char *head_str = pc_strndup((const char *)raw.data, find_crlf_crlf(raw.data, raw.len));
        if (head_str != NULL) {
            te = header_value(head_str, "transfer-encoding");
            free(head_str);
        }
    }
    bool chunked = te != NULL && strstr(te, "chunked") != NULL;
    free(te);
    if (chunked) {
        rc = pc_ipp_dechunk(ipp_body, ipp_len, &dec, err);
        if (rc != PC_OK) {
            goto done;
        }
        ipp_body = dec.data;
        ipp_len = dec.len;
    }

    rc = pc_ipp_reply_parse(ipp_body, ipp_len, reply, NULL, err);
    if (rc != PC_OK) {
        goto done;
    }
    if (reply->status >= 0x0400) {
        if (out_ipp_status != NULL) {
            *out_ipp_status = reply->status;
        }
        pc_buf msg = PC_BUF_INIT;
        status_message(reply, &msg);
        rc = pc_fail(err, PC_ERR_INVALID, "print server refused (%#06x): %s", reply->status,
                     pc_buf_cstr(&msg));
        pc_buf_fini(&msg);
        pc_ipp_reply_fini(reply);
        goto done;
    }

done:
    pc_buf_fini(&host);
    pc_buf_fini(&head);
    pc_buf_fini(&raw);
    pc_buf_fini(&dec);
    return rc;
}

/* ---- endpoint ------------------------------------------------------------------------------ */

void pc_ipp_endpoint_fini(pc_ipp_endpoint *ep)
{
    if (ep == NULL) {
        return;
    }
    pc_str_fini(&ep->host);
    memset(ep, 0, sizeof *ep);
}

static pc_status endpoint_tcp(pc_ipp_endpoint *out, const char *host, size_t host_len, uint16_t port,
                              pc_error *err)
{
    out->kind = PC_IPP_TCP;
    out->port = port;
    return pc_str_set(&out->host, host, host_len, err);
}

pc_status pc_ipp_endpoint_parse(const char *server, pc_ipp_endpoint *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    size_t len = strlen(server);
    if (len > 0 && server[0] == '/') {
        out->kind = PC_IPP_UNIX;
        return pc_str_set(&out->host, server, len, err);
    }
    /* `[IPv6]:port`: the brackets tell the address from the port and are not part of the host. */
    if (len > 0 && server[0] == '[') {
        const char *close = memchr(server, ']', len);
        if (close != NULL) {
            const char *host = server + 1;
            size_t hlen = (size_t)(close - host);
            const char *tail = close + 1;
            size_t tail_len = len - (size_t)(tail - server);
            if (tail_len == 0) {
                return endpoint_tcp(out, host, hlen, 631, err);
            }
            if (tail[0] == ':') {
                char *endp = NULL;
                long p = strtol(tail + 1, &endp, 10);
                if (endp != tail + 1 && *endp == '\0' && p >= 0 && p <= 65535) {
                    return endpoint_tcp(out, host, hlen, (uint16_t)p, err);
                }
            }
            /* Anything else keeps the whole string as the host (unresolvable, not the default). */
            return endpoint_tcp(out, server, len, 631, err);
        }
    }
    /* A bare IPv6 literal has more than one ':'; splitting the last would cut the address. A zone
     * id (`fe80::1%eth0`) carries no ':', so there a port can still follow. */
    size_t colons = 0;
    bool has_percent = false;
    for (size_t i = 0; i < len; i++) {
        if (server[i] == ':') {
            colons++;
        } else if (server[i] == '%') {
            has_percent = true;
        }
    }
    if (colons > 1 && !has_percent) {
        return endpoint_tcp(out, server, len, 631, err);
    }
    const char *last = NULL;
    for (size_t i = len; i-- > 0;) {
        if (server[i] == ':') {
            last = server + i;
            break;
        }
    }
    if (last != NULL) {
        char *endp = NULL;
        long p = strtol(last + 1, &endp, 10);
        if (endp != last + 1 && *endp == '\0' && p >= 0 && p <= 65535) {
            return endpoint_tcp(out, server, (size_t)(last - server), (uint16_t)p, err);
        }
    }
    return endpoint_tcp(out, server, len, 631, err);
}

bool pc_ipp_endpoint_reachable(const pc_ipp_endpoint *ep)
{
    pc_error e = {0};
    int fd = ipp_connect(ep, &e);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

static bool host_is_ipv6(const pc_str *host)
{
    const char *s = pc_str_c(host);
    return strchr(s, ':') != NULL;
}

pc_status pc_ipp_endpoint_host(const pc_ipp_endpoint *ep, pc_buf *out, pc_error *err)
{
    if (ep->kind == PC_IPP_UNIX) {
        return pc_buf_append_str(out, "localhost", err);
    }
    if (host_is_ipv6(&ep->host)) {
        if (pc_buf_append_byte(out, '[', err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        for (const char *p = pc_str_c(&ep->host); *p != '\0'; p++) {
            pc_status rc = *p == '%' ? pc_buf_append_str(out, "%25", err) : pc_buf_append_byte(out, (uint8_t)*p, err);
            if (rc != PC_OK) {
                return rc;
            }
        }
        return pc_buf_appendf(out, err, "]:%u", (unsigned)ep->port);
    }
    return pc_buf_appendf(out, err, "%s:%u", pc_str_c(&ep->host), (unsigned)ep->port);
}

pc_status pc_ipp_endpoint_display(const pc_ipp_endpoint *ep, pc_buf *out, pc_error *err)
{
    if (ep->kind == PC_IPP_UNIX) {
        return pc_buf_append_str(out, pc_str_c(&ep->host), err);
    }
    if (host_is_ipv6(&ep->host)) {
        return pc_buf_appendf(out, err, "[%s]:%u", pc_str_c(&ep->host), (unsigned)ep->port);
    }
    return pc_buf_appendf(out, err, "%s:%u", pc_str_c(&ep->host), (unsigned)ep->port);
}

bool pc_ipp_endpoint_detect(pc_ipp_endpoint *out)
{
    const char *server = getenv("CUPS_SERVER");
    if (server != NULL && server[0] != '\0') {
        pc_ipp_endpoint ep = {0};
        if (pc_ipp_endpoint_parse(server, &ep, NULL) != PC_OK) {
            return false;
        }
        if (pc_ipp_endpoint_reachable(&ep)) {
            *out = ep;
            return true;
        }
        pc_ipp_endpoint_fini(&ep);
        return false;
    }
    static const char *const sockets[] = {"/run/cups/cups.sock", "/var/run/cups/cups.sock",
                                           "/private/var/run/cupsd"};
    for (size_t i = 0; i < sizeof sockets / sizeof sockets[0]; i++) {
        pc_ipp_endpoint ep = {0};
        ep.kind = PC_IPP_UNIX;
        if (pc_str_set_c(&ep.host, sockets[i], NULL) != PC_OK) {
            continue;
        }
        if (pc_ipp_endpoint_reachable(&ep)) {
            *out = ep;
            return true;
        }
        pc_ipp_endpoint_fini(&ep);
    }
    pc_ipp_endpoint ep = {0};
    ep.kind = PC_IPP_TCP;
    ep.port = 631;
    if (pc_str_set_c(&ep.host, "localhost", NULL) == PC_OK && pc_ipp_endpoint_reachable(&ep)) {
        *out = ep;
        return true;
    }
    pc_ipp_endpoint_fini(&ep);
    return false;
}

/* ---- printers ------------------------------------------------------------------------------ */

void pc_ipp_printer_list_fini(pc_ipp_printer_list *list)
{
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        pc_str_fini(&list->items[i].name);
        pc_str_fini(&list->items[i].info);
    }
    free(list->items);
    memset(list, 0, sizeof *list);
}

static pc_status list_push(pc_ipp_printer_list *list, pc_ipp_printer *p, pc_error *err)
{
    pc_ipp_printer *grown = pc_grow_array(list->items, &list->cap, list->count + 1, sizeof *list->items);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    list->items = grown;
    list->items[list->count++] = *p;
    memset(p, 0, sizeof *p);
    return PC_OK;
}

/* Copy an attribute's first text value into a pc_str. */
static pc_status attr_to_str(const pc_ipp_attr *a, pc_str *out, pc_error *err)
{
    if (a->nvalues == 0) {
        return PC_OK;
    }
    return pc_str_set(out, (const char *)a->values[0].data, a->values[0].len, err);
}

static pc_status queues(const pc_ipp_endpoint *ep, uint16_t op, bool is_class, pc_ipp_printer_list *out,
                        pc_error *err)
{
    pc_buf req = PC_BUF_INIT;
    pc_ipp_reply reply;
    memset(&reply, 0, sizeof reply);
    pc_status rc = pc_ipp_request_begin(&req, op, 1, err);
    if (rc == PC_OK) {
        static const char *const want[] = {"printer-name", "printer-info", "printer-state",
                                           "printer-is-accepting-jobs"};
        rc = request_attr_many(&req, PC_IPP_TAG_KEYWORD, "requested-attributes", want, 4, err);
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_end(&req, err);
    }
    uint16_t ipp_status = 0;
    if (rc == PC_OK) {
        rc = ipp_post(ep, "/", req.data, req.len, &reply, &ipp_status, err);
    }
    pc_buf_fini(&req);
    if (rc != PC_OK) {
        if (ipp_status == 0x0406) { /* no printer set up: an empty list, not an error */
            pc_error_clear(err);
            return PC_OK;
        }
        return rc;
    }

    pc_ipp_printer current;
    memset(&current, 0, sizeof current);
    bool have = false;
    for (size_t i = 0; i < reply.nattrs; i++) {
        const pc_ipp_attr *a = &reply.attrs[i];
        if (a->group != PC_IPP_TAG_PRINTER_GROUP) {
            continue;
        }
        if (strcmp(a->name, "printer-name") == 0) {
            if (have) {
                if ((rc = list_push(out, &current, err)) != PC_OK) {
                    goto fail;
                }
            }
            memset(&current, 0, sizeof current);
            current.state = 3;
            current.accepting_jobs = true;
            current.is_class = is_class;
            have = true;
            if ((rc = attr_to_str(a, &current.name, err)) != PC_OK) {
                goto fail;
            }
        } else if (have && strcmp(a->name, "printer-info") == 0) {
            if ((rc = attr_to_str(a, &current.info, err)) != PC_OK) {
                goto fail;
            }
        } else if (have && strcmp(a->name, "printer-state") == 0) {
            int32_t v;
            if (pc_ipp_attr_int(a, &v)) {
                if (v < 0) {
                    v = 0;
                }
                if (v > 5) {
                    v = 5;
                }
                current.state = (uint32_t)v;
            }
        } else if (have && strcmp(a->name, "printer-is-accepting-jobs") == 0) {
            current.accepting_jobs = !(a->nvalues > 0 && a->values[0].len > 0 && a->values[0].data[0] == 0);
        }
    }
    if (have) {
        rc = list_push(out, &current, err);
    }
fail:
    pc_str_fini(&current.name);
    pc_str_fini(&current.info);
    pc_ipp_reply_fini(&reply);
    return rc;
}

pc_status pc_ipp_default_printer(const pc_ipp_endpoint *ep, pc_str *out, bool *present, pc_error *err)
{
    *present = false;
    pc_buf req = PC_BUF_INIT;
    pc_ipp_reply reply;
    memset(&reply, 0, sizeof reply);
    pc_status rc = pc_ipp_request_begin(&req, PC_IPP_OP_CUPS_GET_DEFAULT, 2, err);
    if (rc == PC_OK) {
        static const char *const want[] = {"printer-name"};
        rc = request_attr_many(&req, PC_IPP_TAG_KEYWORD, "requested-attributes", want, 1, err);
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_end(&req, err);
    }
    uint16_t ipp_status = 0;
    if (rc == PC_OK) {
        rc = ipp_post(ep, "/", req.data, req.len, &reply, &ipp_status, err);
    }
    pc_buf_fini(&req);
    if (rc != PC_OK) {
        if (ipp_status == 0x0406) {
            pc_error_clear(err);
            return PC_OK;
        }
        return rc;
    }
    const pc_ipp_attr *a = pc_ipp_reply_find(&reply, "printer-name");
    if (a != NULL && a->nvalues > 0 && a->values[0].len > 0) {
        rc = pc_str_set(out, (const char *)a->values[0].data, a->values[0].len, err);
        *present = rc == PC_OK;
    }
    pc_ipp_reply_fini(&reply);
    return rc;
}

pc_status pc_ipp_printers(const pc_ipp_endpoint *ep, pc_ipp_printer_list *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    pc_status rc = queues(ep, PC_IPP_OP_CUPS_GET_PRINTERS, false, out, err);
    if (rc != PC_OK) {
        pc_ipp_printer_list_fini(out);
        return rc;
    }
    /* A server without classes answers "not found"; an older one may not know the operation. */
    pc_error ignore = {0};
    (void)queues(ep, PC_IPP_OP_CUPS_GET_CLASSES, true, out, &ignore);
    pc_str def = PC_STR_INIT;
    bool present = false;
    pc_error de = {0};
    if (pc_ipp_default_printer(ep, &def, &present, &de) == PC_OK && present) {
        for (size_t i = 0; i < out->count; i++) {
            out->items[i].is_default = pc_str_eq_c(&out->items[i].name, pc_str_c(&def));
        }
    }
    pc_str_fini(&def);
    return PC_OK;
}

pc_status pc_ipp_print_job(const pc_ipp_endpoint *ep, const pc_ipp_job *job, uint32_t *out_job_id,
                           pc_error *err)
{
    pc_buf req = PC_BUF_INIT;
    pc_buf path = PC_BUF_INIT;
    pc_buf host = PC_BUF_INIT;
    pc_buf uri = PC_BUF_INIT;
    pc_ipp_reply reply;
    memset(&reply, 0, sizeof reply);

    pc_status rc = pc_ipp_queue_path(job->printer, job->is_class, &path, err);
    if (rc == PC_OK) {
        rc = pc_ipp_endpoint_host(ep, &host, err);
    }
    if (rc == PC_OK) {
        rc = pc_buf_appendf(&uri, err, "ipp://%s%s", pc_buf_cstr(&host), pc_buf_cstr(&path));
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_begin(&req, PC_IPP_OP_PRINT_JOB, 3, err);
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_attr(&req, PC_IPP_TAG_URI, "printer-uri", uri.data, uri.len, err);
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_attr(&req, PC_IPP_TAG_NAME, "requesting-user-name", job->user,
                                 strlen(job->user), err);
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_attr(&req, PC_IPP_TAG_NAME, "job-name", job->title, strlen(job->title), err);
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_attr(&req, PC_IPP_TAG_MIME, "document-format", job->format,
                                 strlen(job->format), err);
    }
    if (rc == PC_OK && job->copies > 1) {
        uint32_t c = job->copies > 999 ? 999 : job->copies;
        if ((rc = pc_ipp_request_group(&req, PC_IPP_TAG_JOB_GROUP, err)) == PC_OK) {
            rc = pc_ipp_request_integer(&req, "copies", (int32_t)c, err);
        }
    }
    if (rc == PC_OK) {
        rc = pc_ipp_request_end(&req, err);
    }
    if (rc == PC_OK && job->document_len > 0) {
        rc = pc_buf_append(&req, job->document, job->document_len, err);
    }
    if (rc == PC_OK) {
        rc = ipp_post(ep, pc_buf_cstr(&path), req.data, req.len, &reply, NULL, err);
    }
    pc_buf_fini(&req);
    pc_buf_fini(&path);
    pc_buf_fini(&host);
    pc_buf_fini(&uri);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_ipp_attr *a = pc_ipp_reply_find(&reply, "job-id");
    int32_t id = 0;
    if (a == NULL || !pc_ipp_attr_int(a, &id)) {
        pc_ipp_reply_fini(&reply);
        return pc_fail(err, PC_ERR_FORMAT, "no job id in the reply");
    }
    *out_job_id = id < 0 ? 0u : (uint32_t)id;
    pc_ipp_reply_fini(&reply);
    return PC_OK;
}
