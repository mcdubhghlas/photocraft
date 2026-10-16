/* Port of crates/automation/src/security.rs. poll()-based line reader (REWRITE_PLAN.md D14). */
#define _XOPEN_SOURCE 700 /* for strdup-free paths; nothing XSI here, kept at POSIX */
#include "automation/security.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "automation/automation.h"
#include "base/file.h"
#include "base/json.h"

/* ---- minimal UTF-8 validator (serde's from_utf8 rejection) ---- */
static bool valid_utf8(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        size_t extra;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (size_t k = 1; k <= extra; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        /* reject overlong, surrogates, and > U+10FFFF */
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

/* ---- reader ---- */

void pc_auto_reader_init(pc_auto_reader *r, int fd, bool use_timeout)
{
    r->fd = fd;
    r->use_timeout = use_timeout;
    r->pos = r->end = 0;
    r->saw_eof = false;
}

/* 1 = data available in buf, 0 = EOF, -1 = error (err filled). */
static int reader_fill(pc_auto_reader *r, pc_error *err)
{
    if (r->pos < r->end) {
        return 1;
    }
    if (r->saw_eof) {
        return 0;
    }
    r->pos = r->end = 0;
    for (;;) {
        if (r->use_timeout) {
            struct pollfd p = {r->fd, POLLIN, 0};
            int pr = poll(&p, 1, PC_AUTO_IO_TIMEOUT_SECS * 1000);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                (void)pc_fail_errno(err, PC_ERR_IO, errno, "poll");
                return -1;
            }
            if (pr == 0) {
                (void)pc_auto_io(err, "control connection timed out");
                return -1;
            }
        }
        ssize_t n = read(r->fd, r->buf, sizeof r->buf);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)pc_fail_errno(err, PC_ERR_IO, errno, "read");
            return -1;
        }
        if (n == 0) {
            r->saw_eof = true;
            return 0;
        }
        r->end = (size_t)n;
        return 1;
    }
}

pc_auto_line pc_auto_read_bounded_line(pc_auto_reader *r, pc_buf *line, pc_error *err)
{
    pc_buf_truncate(line, 0);
    const size_t cap = PC_AUTO_MAX_REQUEST_BYTES + 1;
    bool got_newline = false;
    for (;;) {
        int f = reader_fill(r, err);
        if (f < 0) {
            return PC_AUTO_LINE_IOERR;
        }
        if (f == 0) {
            break; /* EOF */
        }
        size_t avail = r->end - r->pos;
        size_t room = cap > line->len ? cap - line->len : 0;
        if (room == 0) {
            break;
        }
        size_t take = avail < room ? avail : room;
        const uint8_t *base = r->buf + r->pos;
        const uint8_t *nl = memchr(base, '\n', take);
        size_t chunk = nl ? (size_t)(nl - base) + 1 : take;
        if (pc_buf_append(line, base, chunk, err) != PC_OK) {
            return PC_AUTO_LINE_IOERR;
        }
        r->pos += chunk;
        if (nl) {
            got_newline = true;
            break;
        }
        if (line->len >= cap) {
            break;
        }
    }
    (void)got_newline;
    size_t nbytes = line->len;
    if (nbytes == 0) {
        return PC_AUTO_LINE_EOF;
    }
    if (nbytes > PC_AUTO_MAX_REQUEST_BYTES) {
        return PC_AUTO_LINE_TOO_LONG;
    }
    if (!valid_utf8(line->data, nbytes)) {
        return PC_AUTO_LINE_INVALID_UTF8;
    }
    return PC_AUTO_LINE_OK;
}

pc_status pc_auto_discard_rest_of_line(pc_auto_reader *r, pc_error *err)
{
    for (;;) {
        int f = reader_fill(r, err);
        if (f < 0) {
            return PC_ERR_IO;
        }
        if (f == 0) {
            return PC_OK;
        }
        size_t avail = r->end - r->pos;
        const uint8_t *base = r->buf + r->pos;
        const uint8_t *nl = memchr(base, '\n', avail);
        if (nl) {
            r->pos += (size_t)(nl - base) + 1;
            return PC_OK;
        }
        r->pos = r->end;
    }
}

pc_status pc_auto_write_all(int fd, bool use_timeout, const void *data, size_t len, pc_error *err)
{
    const uint8_t *p = data;
    size_t off = 0;
    while (off < len) {
        if (use_timeout) {
            struct pollfd pf = {fd, POLLOUT, 0};
            int pr = poll(&pf, 1, PC_AUTO_IO_TIMEOUT_SECS * 1000);
            if (pr < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return pc_fail_errno(err, PC_ERR_IO, errno, "poll");
            }
            if (pr == 0) {
                return pc_auto_io(err, "control write timed out");
            }
        }
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return pc_fail_errno(err, PC_ERR_IO, errno, "write");
        }
        off += (size_t)n;
    }
    return PC_OK;
}

/* ---- tokens ---- */

enum { TOKEN_BYTES = 32, TOKEN_HEX_LEN = 64 };

pc_status pc_auto_generate_token(char out[65], pc_error *err)
{
    uint8_t bytes[TOKEN_BYTES];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return pc_auto_other(err, "cannot generate control token: %s", strerror(errno));
    }
    size_t got = 0;
    while (got < sizeof bytes) {
        ssize_t n = read(fd, bytes + got, sizeof bytes - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            int e = errno;
            (void)close(fd);
            return pc_auto_other(err, "cannot generate control token: %s", strerror(e));
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    (void)close(fd);
    if (got != sizeof bytes) {
        return pc_auto_other(err, "cannot generate control token: short read");
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < TOKEN_BYTES; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[TOKEN_HEX_LEN] = '\0';
    return PC_OK;
}

pc_status pc_auto_validate_token(const char *token, pc_error *err)
{
    size_t n = strlen(token);
    if (n != TOKEN_HEX_LEN) {
        return pc_auto_bad(err, "control token must contain exactly 64 hexadecimal characters");
    }
    for (size_t i = 0; i < n; i++) {
        char c = token[i];
        bool hexdigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hexdigit) {
            return pc_auto_bad(err, "control token must contain exactly 64 hexadecimal characters");
        }
    }
    return PC_OK;
}

bool pc_auto_token_matches(const char *expected, const char *supplied)
{
    if (strlen(expected) != TOKEN_HEX_LEN || strlen(supplied) != TOKEN_HEX_LEN) {
        return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < TOKEN_HEX_LEN; i++) {
        diff |= (unsigned char)(expected[i] ^ supplied[i]);
    }
    return diff == 0;
}

static void ascii_lower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

static pc_status read_token_file(const char *path, char out[65], pc_error *err)
{
    pc_buf buf = PC_BUF_INIT;
    if (pc_file_read(path, 1 << 20, &buf, err) != PC_OK) {
        pc_error sub = *err;
        pc_buf_fini(&buf);
        return pc_auto_io(err, "%s: %s", path, sub.msg);
    }
    /* trim ASCII whitespace */
    size_t start = 0, end = buf.len;
    while (start < end && (buf.data[start] == ' ' || buf.data[start] == '\t' || buf.data[start] == '\n' ||
                           buf.data[start] == '\r')) {
        start++;
    }
    while (end > start && (buf.data[end - 1] == ' ' || buf.data[end - 1] == '\t' || buf.data[end - 1] == '\n' ||
                           buf.data[end - 1] == '\r')) {
        end--;
    }
    size_t tlen = end - start;
    if (tlen != TOKEN_HEX_LEN) {
        pc_buf_fini(&buf);
        return pc_auto_bad(err, "control token must contain exactly 64 hexadecimal characters");
    }
    memcpy(out, buf.data + start, tlen);
    out[tlen] = '\0';
    pc_buf_fini(&buf);
    return pc_auto_validate_token(out, err);
}

static pc_status create_token_file(const char *path, const char *token, pc_error *err)
{
    /* Ensure the parent directory exists. */
    const char *slash = strrchr(path, '/');
    if (slash && slash != path) {
        size_t plen = (size_t)(slash - path);
        char parent[4096];
        if (plen < sizeof parent) {
            memcpy(parent, path, plen);
            parent[plen] = '\0';
            if (pc_mkdir_p(parent, err) != PC_OK) {
                pc_error sub = *err;
                return pc_auto_io(err, "%s: %s", parent, sub.msg);
            }
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return pc_auto_io(err, "%s: %s", path, strerror(errno));
    }
    char line[66];
    (void)snprintf(line, sizeof line, "%s\n", token);
    pc_status rc = pc_auto_write_all(fd, false, line, strlen(line), err);
    (void)close(fd);
    if (rc != PC_OK) {
        return pc_auto_io(err, "%s: write failed", path);
    }
    return PC_OK;
}

static bool path_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

pc_status pc_auto_server_token(const char *supplied, const char *token_file, char out[65], pc_error *err)
{
    if (supplied && token_file) {
        return pc_auto_bad(err, "use either a control token or a control token file, not both");
    }
    if (supplied) {
        if (pc_auto_validate_token(supplied, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        (void)snprintf(out, 65, "%s", supplied);
        ascii_lower(out);
        return PC_OK;
    }
    char token[65];
    if (pc_auto_generate_token(token, err) != PC_OK) {
        return err ? err->code : PC_ERR_INVALID;
    }
    if (!token_file) {
        memcpy(out, token, 65);
        return PC_OK;
    }
    pc_error sub;
    pc_error_clear(&sub);
    if (read_token_file(token_file, out, &sub) == PC_OK) {
        ascii_lower(out);
        return PC_OK;
    }
    if (sub.code == PC_ERR_IO && !path_exists(token_file)) {
        if (create_token_file(token_file, token, err) == PC_OK) {
            memcpy(out, token, 65);
            return PC_OK;
        }
        /* Another process won the create-new race. */
        if (path_exists(token_file) && read_token_file(token_file, out, err) == PC_OK) {
            ascii_lower(out);
            return PC_OK;
        }
        return err && err->code != PC_OK ? err->code : PC_ERR_IO;
    }
    if (err) {
        *err = sub;
    }
    return sub.code;
}

pc_status pc_auto_client_token(const char *supplied, const char *token_file, char out[65], pc_error *err)
{
    if (supplied && token_file) {
        return pc_auto_bad(err, "use either a control token or a control token file, not both");
    }
    if (supplied) {
        if (pc_auto_validate_token(supplied, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        (void)snprintf(out, 65, "%s", supplied);
        ascii_lower(out);
        return PC_OK;
    }
    if (!token_file) {
        return pc_auto_bad(err, "bridge mode needs --control-token, --control-token-file, "
                                "PHOTOCRAFT_CONTROL_TOKEN, or PHOTOCRAFT_CONTROL_TOKEN_FILE");
    }
    if (read_token_file(token_file, out, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    ascii_lower(out);
    return PC_OK;
}

/* ---- auth replies ---- */

/* Build {"id": <id>, "ok": <ok>, ...}. On ok adds result{authenticated:true}; else error string. */
static yyjson_mut_doc *auth_doc(yyjson_val *id, bool ok, const char *error)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return NULL;
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_val *idv = id ? yyjson_val_mut_copy(doc, id) : yyjson_mut_null(doc);
    yyjson_mut_obj_add_val(doc, o, "id", idv ? idv : yyjson_mut_null(doc));
    yyjson_mut_obj_add_bool(doc, o, "ok", ok);
    if (ok) {
        yyjson_mut_val *res = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_bool(doc, res, "authenticated", true);
        yyjson_mut_obj_add_val(doc, o, "result", res);
    } else {
        yyjson_mut_obj_add_strcpy(doc, o, "error", error);
    }
    yyjson_mut_doc_set_root(doc, o);
    return doc;
}

pc_status pc_auto_authentication_reply(const char *line, size_t len, const char *token,
                                       yyjson_mut_doc **out_doc, bool *authed, pc_error *err)
{
    *authed = false;
    yyjson_doc *req = NULL;
    if (pc_json_parse(line, len, &req, NULL) != PC_OK || !req) {
        *out_doc = auth_doc(NULL, false, "authentication required");
        if (!*out_doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        return PC_OK;
    }
    yyjson_val *root = yyjson_doc_get_root(req);
    yyjson_val *id = pc_json_get(root, "id");
    yyjson_val *method = pc_json_get(root, "method");
    yyjson_val *params = pc_json_get(root, "params");
    yyjson_val *tokv = params ? pc_json_get(params, "token") : NULL;
    const char *supplied = tokv ? pc_json_as_str(tokv, NULL) : NULL;
    const char *m = method ? pc_json_as_str(method, NULL) : NULL;
    bool ok = m && strcmp(m, PC_AUTO_AUTH_METHOD) == 0 && supplied && pc_auto_token_matches(token, supplied);
    *out_doc = auth_doc(id, ok, "authentication required");
    *authed = ok;
    yyjson_doc_free(req);
    if (!*out_doc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    return PC_OK;
}

pc_status pc_auto_repeat_auth_reply(const char *line, size_t len, yyjson_mut_doc **out_doc, pc_error *err)
{
    *out_doc = NULL;
    yyjson_doc *req = NULL;
    if (pc_json_parse(line, len, &req, NULL) != PC_OK || !req) {
        return PC_OK; /* not valid JSON -> dispatch as usual (None) */
    }
    yyjson_val *root = yyjson_doc_get_root(req);
    yyjson_val *method = pc_json_get(root, "method");
    const char *m = method ? pc_json_as_str(method, NULL) : NULL;
    if (!m || strcmp(m, PC_AUTO_AUTH_METHOD) != 0) {
        yyjson_doc_free(req);
        return PC_OK;
    }
    yyjson_val *id = pc_json_get(root, "id");
    *out_doc = auth_doc(id, false, "already authenticated");
    yyjson_doc_free(req);
    if (!*out_doc) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    return PC_OK;
}

/* ---- connection limiter ---- */

void pc_auto_limiter_init(pc_auto_limiter *l, size_t max)
{
    atomic_init(&l->active, 0);
    l->max = max;
}

bool pc_auto_limiter_acquire(pc_auto_limiter *l)
{
    size_t cur = atomic_load_explicit(&l->active, memory_order_acquire);
    for (;;) {
        if (cur >= l->max) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&l->active, &cur, cur + 1, memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

void pc_auto_limiter_release(pc_auto_limiter *l)
{
    (void)atomic_fetch_sub_explicit(&l->active, 1, memory_order_acq_rel);
}
