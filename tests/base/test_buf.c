#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "harness.h"

static void test_empty_buffer(void)
{
    pc_buf b = PC_BUF_INIT;
    size_t len = 9;
    CHECK_EQ_STR(pc_buf_cstr(&b), "");
    CHECK(pc_buf_take(&b, &len) == NULL);
    CHECK_EQ_UINT(len, 0);
    pc_buf_fini(&b);
    pc_buf_fini(NULL);
}

static void test_append_keeps_terminator(void)
{
    pc_buf b = PC_BUF_INIT;
    int i;
    CHECK_EQ_INT(pc_buf_append_str(&b, "abc", NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_append_byte(&b, 'd', NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_append(&b, "\0e", 2, NULL), PC_OK);
    CHECK_EQ_UINT(b.len, 6);
    CHECK_EQ_MEM(b.data, "abcd\0e", 7); /* includes the terminator */
    for (i = 0; i < 10000; i++) {
        CHECK_EQ_INT(pc_buf_append_byte(&b, 'x', NULL), PC_OK);
    }
    CHECK_EQ_UINT(b.len, 10006);
    CHECK_EQ_UINT(b.data[b.len], 0);
    CHECK(b.cap > b.len);
    pc_buf_fini(&b);
    CHECK(b.data == NULL && b.len == 0 && b.cap == 0);
}

static void test_appendf(void)
{
    pc_buf b = PC_BUF_INIT;
    char big[600];
    CHECK_EQ_INT(pc_buf_appendf(&b, NULL, "%d-%s", 42, "x"), PC_OK);
    CHECK_EQ_INT(pc_buf_appendf(&b, NULL, "/%05.1f", 3.14159), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&b), "42-x/003.1");
    memset(big, 'y', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    CHECK_EQ_INT(pc_buf_appendf(&b, NULL, "%s", big), PC_OK);
    CHECK_EQ_UINT(b.len, 10 + 599);
    CHECK_EQ_UINT(b.data[b.len], 0);
    pc_buf_fini(&b);
}

static void test_truncate_and_take(void)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t *p;
    size_t len = 0;
    CHECK_EQ_INT(pc_buf_append_str(&b, "hello", NULL), PC_OK);
    pc_buf_truncate(&b, 10); /* no-op */
    CHECK_EQ_STR(pc_buf_cstr(&b), "hello");
    pc_buf_truncate(&b, 2);
    CHECK_EQ_STR(pc_buf_cstr(&b), "he");
    p = pc_buf_take(&b, &len);
    CHECK_EQ_UINT(len, 2);
    CHECK_EQ_STR((char *)p, "he");
    CHECK(b.data == NULL && b.len == 0);
    free(p);
}

static void test_reserve_overflow_leaves_buffer(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    CHECK_EQ_INT(pc_buf_append_str(&b, "keep", NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_reserve(&b, SIZE_MAX, &e), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_buf_reserve(&b, SIZE_MAX - 10, &e), PC_ERR_NOMEM);
    CHECK_EQ_INT(pc_buf_append(&b, "x", SIZE_MAX - 2, &e), PC_ERR_LIMIT);
    CHECK_EQ_STR(pc_buf_cstr(&b), "keep");
    CHECK_EQ_UINT(b.len, 4);
    pc_buf_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_empty_buffer);
    RUN(test_append_keeps_terminator);
    RUN(test_appendf);
    RUN(test_truncate_and_take);
    RUN(test_reserve_overflow_leaves_buffer);
    return pc_t_finish();
}
