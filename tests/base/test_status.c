#include <errno.h>
#include <string.h>

#include "base/status.h"
#include "harness.h"

static void test_fail_fills_error(void)
{
    pc_error e;
    pc_status s = pc_fail(&e, PC_ERR_FORMAT, "bad %s at %d", "chunk", 7);
    CHECK_EQ_INT(s, PC_ERR_FORMAT);
    CHECK_EQ_INT(e.code, PC_ERR_FORMAT);
    CHECK_EQ_INT(e.sys_errno, 0);
    CHECK_EQ_STR(e.msg, "bad chunk at 7");
}

static void test_fail_accepts_null(void)
{
    CHECK_EQ_INT(pc_fail(NULL, PC_ERR_LIMIT, "x"), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_fail_errno(NULL, PC_ERR_IO, ENOENT, "x"), PC_ERR_IO);
    pc_error_clear(NULL);
}

static void test_fail_errno_appends_strerror(void)
{
    pc_error e;
    pc_fail_errno(&e, PC_ERR_IO, ENOENT, "cannot open %s", "a.psd");
    CHECK_EQ_INT(e.sys_errno, ENOENT);
    CHECK_CONTAINS(e.msg, "cannot open a.psd: ");
    CHECK_CONTAINS(e.msg, strerror(ENOENT));
}

static void test_long_message_truncates(void)
{
    char big[1000];
    pc_error e;
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    pc_fail(&e, PC_ERR_INVALID, "%s", big);
    CHECK_EQ_UINT(strlen(e.msg), PC_ERROR_MSG_MAX - 1);
    pc_fail_errno(&e, PC_ERR_IO, EACCES, "%s", big);
    CHECK_EQ_UINT(strlen(e.msg), PC_ERROR_MSG_MAX - 1);
}

static void test_clear_and_names(void)
{
    pc_error e;
    pc_fail(&e, PC_ERR_IO, "x");
    pc_error_clear(&e);
    CHECK_EQ_INT(e.code, PC_OK);
    CHECK_EQ_STR(e.msg, "");
    CHECK_EQ_STR(pc_status_name(PC_OK), "ok");
    CHECK_EQ_STR(pc_status_name(PC_ERR_NOT_FOUND), "not_found");
    CHECK_EQ_STR(pc_status_name((pc_status)999), "unknown");
}

int main(int argc, char **argv)
{
    RUN(test_fail_fills_error);
    RUN(test_fail_accepts_null);
    RUN(test_fail_errno_appends_strerror);
    RUN(test_long_message_truncates);
    RUN(test_clear_and_names);
    return pc_t_finish();
}
