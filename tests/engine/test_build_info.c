/* build_info parity (crates/engine/src/build_info.rs tests). The reference checks an internal
 * format_long() over fixed args; here we check the public long_version against the compiled
 * provenance (a plain build has none -> "<version> (dev build)"). */
#include <string.h>

#include "engine/engine.h"
#include "harness.h"

static void test_version_is_non_empty(void)
{
    CHECK(pc_engine_version != NULL && pc_engine_version[0] != '\0');
    char buf[128];
    size_t n = pc_engine_long_version(buf, sizeof buf);
    CHECK(n > 0);
    CHECK(strncmp(buf, pc_engine_version, strlen(pc_engine_version)) == 0);
    const char *sha = pc_engine_short_sha();
    if (sha != NULL) {
        CHECK(strlen(sha) <= 9);
    }
}

static void test_long_version_format(void)
{
    char buf[128];
    (void)pc_engine_long_version(buf, sizeof buf);
    /* Without build provenance a plain build is a dev build. */
    if (pc_engine_git_sha() == NULL && pc_engine_build_date() == NULL) {
        char want[160];
        (void)snprintf(want, sizeof want, "%s (dev build)", pc_engine_version);
        CHECK_EQ_STR(buf, want);
    } else {
        CHECK(strstr(buf, "(") != NULL && strstr(buf, ")") != NULL);
    }
}

int main(int argc, char **argv)
{
    RUN(test_version_is_non_empty);
    RUN(test_long_version_format);
    return pc_t_finish();
}
