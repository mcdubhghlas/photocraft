/* crash_guard: a clean run passes through; a fatal signal becomes an error, not a crash. */
#include <signal.h>
#include <stddef.h>

#include "app/crash_guard.h"
#include "harness.h"

static pc_status ok_fn(void *ctx, pc_error *err)
{
    (void)err;
    *(int *)ctx = 7;
    return PC_OK;
}

static pc_status err_fn(void *ctx, pc_error *err)
{
    (void)ctx;
    return pc_fail(err, PC_ERR_FORMAT, "bad file");
}

static pc_status crash_fn(void *ctx, pc_error *err)
{
    (void)ctx;
    (void)err;
    raise(SIGSEGV); /* a synthetic fault the guard must catch */
    return PC_OK;    /* unreachable */
}

static void test_clean_run(void)
{
    int sentinel = 0;
    pc_error err;
    pc_error_clear(&err);
    CHECK_EQ_INT(pc_app_crash_guard("Open", ok_fn, &sentinel, &err), PC_OK);
    CHECK_EQ_INT(sentinel, 7);
}

static void test_error_passthrough(void)
{
    pc_error err;
    pc_error_clear(&err);
    CHECK_EQ_INT(pc_app_crash_guard("Open", err_fn, NULL, &err), PC_ERR_FORMAT);
    CHECK_EQ_STR(err.msg, "bad file");
}

static void test_signal_becomes_error(void)
{
    pc_app_crash_install_hook();
    pc_error err;
    pc_error_clear(&err);
    pc_status rc = pc_app_crash_guard("Open", crash_fn, NULL, &err);
    CHECK(rc != PC_OK);
    CHECK_CONTAINS(err.msg, "Open failed");
    /* The guard is reusable after catching a fault. */
    int sentinel = 0;
    CHECK_EQ_INT(pc_app_crash_guard("Resume", ok_fn, &sentinel, &err), PC_OK);
    CHECK_EQ_INT(sentinel, 7);
}

int main(int argc, char **argv)
{
    RUN(test_clean_run);
    RUN(test_error_passthrough);
    RUN(test_signal_becomes_error);
    return pc_t_finish();
}
