/* Security: tokens, the bounded poll() line reader, auth replies, and the connection limiter
 * (port of security.rs tests). */
#include "harness.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "automation/security.h"
#include "base/buf.h"
#include "base/json.h"

/* Write `data` to a temp file and return a read fd positioned at the start. */
static int fd_with(const char *data, size_t len)
{
    char tmpl[64];
    const char *dir = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/pc-sec-XXXXXX", dir ? dir : "/tmp");
    int fd = mkstemp(tmpl);
    (void)unlink(tmpl);
    if (fd < 0) {
        return -1;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
    }
    (void)lseek(fd, 0, SEEK_SET);
    return fd;
}

static void test_tokens(void)
{
    char a[65], b[65];
    CHECK(pc_auto_generate_token(a, NULL) == PC_OK);
    CHECK(pc_auto_generate_token(b, NULL) == PC_OK);
    CHECK(pc_auto_validate_token(a, NULL) == PC_OK);
    CHECK(strcmp(a, b) != 0);
    CHECK(pc_auto_token_matches(a, a));
    CHECK(!pc_auto_token_matches(a, b));
    CHECK(!pc_auto_token_matches(a, "short"));
    CHECK(pc_auto_validate_token("xyz", NULL) != PC_OK);
}

static void test_bounded_reader(void)
{
    /* An over-long line is reported TooLong; the next read is EOF. */
    size_t big = PC_AUTO_MAX_REQUEST_BYTES + 1;
    char *data = malloc(big + 1);
    memset(data, 'x', big);
    data[big] = '\n';
    int fd = fd_with(data, big + 1);
    pc_auto_reader r;
    pc_auto_reader_init(&r, fd, false);
    pc_buf line = PC_BUF_INIT;
    CHECK(pc_auto_read_bounded_line(&r, &line, NULL) == PC_AUTO_LINE_TOO_LONG);
    CHECK_EQ_UINT(line.len, (unsigned)(PC_AUTO_MAX_REQUEST_BYTES + 1));
    (void)pc_auto_discard_rest_of_line(&r, NULL);
    CHECK(pc_auto_read_bounded_line(&r, &line, NULL) == PC_AUTO_LINE_EOF);
    pc_buf_fini(&line);
    (void)close(fd);
    free(data);

    /* Invalid UTF-8 within the limit is reported and the line consumed; the next line parses. */
    int fd2 = fd_with("\xff\xfe\nnext\n", 8);
    pc_auto_reader r2;
    pc_auto_reader_init(&r2, fd2, false);
    pc_buf l2 = PC_BUF_INIT;
    CHECK(pc_auto_read_bounded_line(&r2, &l2, NULL) == PC_AUTO_LINE_INVALID_UTF8);
    CHECK(pc_auto_read_bounded_line(&r2, &l2, NULL) == PC_AUTO_LINE_OK);
    CHECK_EQ_STR(pc_buf_cstr(&l2), "next\n");
    pc_buf_fini(&l2);
    (void)close(fd2);
}

static const char *reply_str(yyjson_mut_doc *d, const char *key)
{
    return yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d), key));
}

static void test_auth_replies(void)
{
    char token[65];
    CHECK(pc_auto_generate_token(token, NULL) == PC_OK);
    /* Before auth, a non-auth method is refused. */
    const char *nope = "{\"id\":1,\"method\":\"methods\",\"params\":{}}";
    yyjson_mut_doc *d = NULL;
    bool authed = true;
    CHECK(pc_auto_authentication_reply(nope, strlen(nope), token, &d, &authed, NULL) == PC_OK);
    CHECK(!authed);
    CHECK_EQ_STR(reply_str(d, "error"), "authentication required");
    yyjson_mut_doc_free(d);
    /* With the token it authenticates. */
    char line[160];
    snprintf(line, sizeof line, "{\"id\":2,\"method\":\"auth\",\"params\":{\"token\":\"%s\"}}", token);
    d = NULL;
    authed = false;
    CHECK(pc_auto_authentication_reply(line, strlen(line), token, &d, &authed, NULL) == PC_OK);
    CHECK(authed);
    yyjson_mut_doc_free(d);
    /* Repeat auth on an authenticated connection says so; a non-auth frame yields NULL. */
    d = NULL;
    CHECK(pc_auto_repeat_auth_reply(line, strlen(line), &d, NULL) == PC_OK);
    CHECK(d != NULL);
    CHECK_EQ_STR(reply_str(d, "error"), "already authenticated");
    yyjson_mut_doc_free(d);
    d = (yyjson_mut_doc *)1;
    CHECK(pc_auto_repeat_auth_reply(nope, strlen(nope), &d, NULL) == PC_OK);
    CHECK(d == NULL);
}

static void test_limiter(void)
{
    pc_auto_limiter l;
    pc_auto_limiter_init(&l, 1);
    CHECK(pc_auto_limiter_acquire(&l));
    CHECK(!pc_auto_limiter_acquire(&l));
    pc_auto_limiter_release(&l);
    CHECK(pc_auto_limiter_acquire(&l));
    pc_auto_limiter_release(&l);
}

int main(int argc, char **argv)
{
    RUN(test_tokens);
    RUN(test_bounded_reader);
    RUN(test_auth_replies);
    RUN(test_limiter);
    return pc_t_finish();
}
