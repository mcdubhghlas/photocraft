/* Version and build provenance (port of crates/engine/src/build_info.rs).
 *
 * The version is the workspace version. The commit and build date are optional and come from
 * -D macros set by CI / packaging scripts at compile time:
 *   -DPHOTOCRAFT_BUILD_SHA=\"...\"   the git commit (shown shortened to 9 chars)
 *   -DPHOTOCRAFT_BUILD_DATE=\"...\"  the build date (conventionally YYYY-MM-DD)
 * Without them a plain build reports itself as a dev build. Nothing shells out to git. */
#include <stdio.h>
#include <string.h>

#include "engine.h"

#ifndef PC_ENGINE_VERSION
#define PC_ENGINE_VERSION "0.8.2" /* [workspace.package] version in the root Cargo.toml */
#endif

const char *const pc_engine_version = PC_ENGINE_VERSION;

#if defined(PHOTOCRAFT_BUILD_SHA) || defined(PHOTOCRAFT_BUILD_DATE)
/* Trim leading/trailing ASCII space/tab/newline, like Rust str::trim for these, and treat an
 * empty result as absent (returns NULL). Writes the trimmed copy into a static buffer. */
static const char *trimmed_env(const char *raw, char *buf, size_t cap)
{
    if (raw == NULL) {
        return NULL;
    }
    const char *start = raw;
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }
    size_t len = strlen(start);
    while (len > 0) {
        char c = start[len - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            len--;
        } else {
            break;
        }
    }
    if (len == 0 || len >= cap) {
        if (len == 0) {
            return NULL;
        }
        len = cap - 1;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}
#endif

const char *pc_engine_git_sha(void)
{
#ifdef PHOTOCRAFT_BUILD_SHA
    static char buf[128];
    return trimmed_env(PHOTOCRAFT_BUILD_SHA, buf, sizeof buf);
#else
    return NULL;
#endif
}

const char *pc_engine_build_date(void)
{
#ifdef PHOTOCRAFT_BUILD_DATE
    static char buf[64];
    return trimmed_env(PHOTOCRAFT_BUILD_DATE, buf, sizeof buf);
#else
    return NULL;
#endif
}

const char *pc_engine_short_sha(void)
{
    const char *sha = pc_engine_git_sha();
    if (sha == NULL) {
        return NULL;
    }
    static char buf[10];
    size_t n = strlen(sha);
    if (n > 9) {
        n = 9;
    }
    memcpy(buf, sha, n);
    buf[n] = '\0';
    return buf;
}

size_t pc_engine_long_version(char *out, size_t cap)
{
    const char *sha = pc_engine_short_sha();
    const char *date = pc_engine_build_date();
    int want;
    if (sha != NULL && date != NULL) {
        want = snprintf(out, cap, "%s (%s, %s)", pc_engine_version, sha, date);
    } else if (sha != NULL) {
        want = snprintf(out, cap, "%s (%s)", pc_engine_version, sha);
    } else if (date != NULL) {
        want = snprintf(out, cap, "%s (%s)", pc_engine_version, date);
    } else {
        want = snprintf(out, cap, "%s (dev build)", pc_engine_version);
    }
    return want < 0 ? 0 : (size_t)want;
}
