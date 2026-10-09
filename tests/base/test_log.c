#include <string.h>

#include "base/log.h"
#include "harness.h"

static char last[2048];
static int count;
static pc_log_level last_level;

static void capture(pc_log_level level, const char *msg)
{
    (void)snprintf(last, sizeof last, "%s", msg);
    last_level = level;
    count++;
}

static void test_sink_and_levels(void)
{
    pc_log_set_sink(capture);
    pc_log_set_level(PC_LOG_INFO);
    count = 0;
    pc_log(PC_LOG_WARN, "layer %d of %s", 3, "doc");
    CHECK_EQ_INT(count, 1);
    CHECK_EQ_STR(last, "layer 3 of doc");
    CHECK_EQ_INT(last_level, PC_LOG_WARN);
    pc_log(PC_LOG_DEBUG, "dropped");
    CHECK_EQ_INT(count, 1);
    pc_log_set_level(PC_LOG_ERROR);
    pc_log(PC_LOG_WARN, "dropped too");
    CHECK_EQ_INT(count, 1);
    pc_log(PC_LOG_ERROR, "kept");
    CHECK_EQ_INT(count, 2);
    pc_log_set_sink(NULL);
}

static void test_long_messages_truncate(void)
{
    char big[3000];
    memset(big, 'q', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    pc_log_set_sink(capture);
    pc_log_set_level(PC_LOG_DEBUG);
    pc_log(PC_LOG_INFO, "%s", big);
    CHECK_EQ_UINT(strlen(last), 1023);
    pc_log_set_sink(NULL);
}

int main(int argc, char **argv)
{
    RUN(test_sink_and_levels);
    RUN(test_long_messages_truncate);
    return pc_t_finish();
}
