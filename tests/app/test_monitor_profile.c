/* monitor_profile: RGB-only ICC validation, reading an ICC file, the colord/XDG search path,
 * and unambiguous clone-profile selection. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/monitor_profile.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "harness.h"

typedef struct {
    const char *keys[8];
    const char *vals[8];
    size_t n;
} fake_env;

static const char *env_lookup(void *ctx, const char *name)
{
    fake_env *e = ctx;
    for (size_t i = 0; i < e->n; i++) {
        if (strcmp(e->keys[i], name) == 0) {
            return e->vals[i][0] ? e->vals[i] : NULL;
        }
    }
    return NULL;
}

static bool write_temp(const uint8_t *data, size_t len, char *path_out, size_t cap)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) {
        tmp = "/tmp";
    }
    int n = snprintf(path_out, cap, "%s/pc_monitor_icc_XXXXXX", tmp);
    if (n <= 0 || (size_t)n >= cap) {
        return false;
    }
    int fd = mkstemp(path_out);
    if (fd < 0) {
        return false;
    }
    ssize_t w = write(fd, data, len);
    (void)close(fd);
    return w == (ssize_t)len;
}

static void test_icc_validate(void)
{
    pc_error err;
    pc_buf srgb = PC_BUF_INIT;
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_SRGB), &srgb, &err), PC_OK);
    size_t valid = 0;
    CHECK_EQ_INT(pc_monitor_icc_validate(srgb.data, srgb.len, &valid, &err), PC_OK);
    CHECK(valid >= 132 && valid <= srgb.len);

    /* Corrupting the declared size (header bytes 0..3) past the buffer is rejected, not trusted. */
    uint8_t *bad = malloc(srgb.len);
    CHECK(bad != NULL);
    if (bad) {
        memcpy(bad, srgb.data, srgb.len);
        bad[0] = 0xff;
        bad[1] = 0xff;
        bad[2] = 0xff;
        bad[3] = 0xff;
        CHECK_EQ_INT(pc_monitor_icc_validate(bad, srgb.len, &valid, &err), PC_ERR_FORMAT);
        free(bad);
    }
    pc_buf_fini(&srgb);

    /* A CMYK profile is a valid ICC file but not an RGB display profile. */
    pc_buf cmyk = PC_BUF_INIT;
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), &cmyk, &err), PC_OK);
    CHECK_EQ_INT(pc_monitor_icc_validate(cmyk.data, cmyk.len, &valid, &err), PC_ERR_UNSUPPORTED);
    pc_buf_fini(&cmyk);

    /* Missing signature and oversize length are rejected without reading past the buffer. */
    uint8_t zeros[132];
    memset(zeros, 0, sizeof zeros);
    CHECK_EQ_INT(pc_monitor_icc_validate(zeros, sizeof zeros, &valid, &err), PC_ERR_FORMAT);
    static uint8_t one[1];
    CHECK_EQ_INT(pc_monitor_icc_validate(one, PC_MONITOR_MAX_ICC_BYTES + 1u, &valid, &err),
                 PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_monitor_icc_validate(one, 10, &valid, &err), PC_ERR_FORMAT);
}

static void test_read_icc_file(void)
{
    pc_error err;
    pc_buf srgb = PC_BUF_INIT;
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_SRGB), &srgb, &err), PC_OK);

    char path[256];
    if (write_temp(srgb.data, srgb.len, path, sizeof path)) {
        pc_buf got = PC_BUF_INIT;
        CHECK_EQ_INT(pc_monitor_read_icc(path, &got, &err), PC_OK);
        CHECK(got.len >= 132 && got.len <= srgb.len);
        pc_buf_fini(&got);
        (void)unlink(path);
    }

    /* A CMYK file on disk is rejected (not an RGB profile). */
    pc_buf cmyk = PC_BUF_INIT;
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), &cmyk, &err), PC_OK);
    char cpath[256];
    if (write_temp(cmyk.data, cmyk.len, cpath, sizeof cpath)) {
        pc_buf got = PC_BUF_INIT;
        CHECK_EQ_INT(pc_monitor_read_icc(cpath, &got, &err), PC_ERR_UNSUPPORTED);
        pc_buf_fini(&got);
        (void)unlink(cpath);
    }
    pc_buf_fini(&cmyk);
    pc_buf_fini(&srgb);

    /* A missing file is an error, never a crash. */
    pc_buf none = PC_BUF_INIT;
    CHECK(pc_monitor_read_icc("/nonexistent/photocraft/profile.icc", &none, &err) != PC_OK);
    pc_buf_fini(&none);
}

static bool has_dir(char dirs[][PC_APP_PATH_MAX], size_t n, const char *want)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(dirs[i], want) == 0) {
            return true;
        }
    }
    return false;
}

static void test_search_dirs(void)
{
    char dirs[16][PC_APP_PATH_MAX];

    fake_env a = {{"XDG_DATA_HOME", "XDG_DATA_DIRS"}, {"/x/data", "/a:/b"}, 2};
    size_t n = pc_monitor_icc_search_dirs(env_lookup, &a, dirs, 16);
    CHECK(has_dir(dirs, n, "/x/data/icc"));
    CHECK(has_dir(dirs, n, "/a/color/icc"));
    CHECK(has_dir(dirs, n, "/b/color/icc"));
    CHECK(has_dir(dirs, n, "/usr/share/color/icc"));
    /* XDG_DATA_HOME wins, so the HOME fallback path is not added. */
    CHECK(!has_dir(dirs, n, "/home/me/.local/share/icc"));

    fake_env b = {{"HOME"}, {"/home/me"}, 1};
    n = pc_monitor_icc_search_dirs(env_lookup, &b, dirs, 16);
    CHECK(has_dir(dirs, n, "/home/me/.local/share/icc"));
    CHECK(has_dir(dirs, n, "/var/lib/colord/icc"));

    /* No env at all still lists the system stores and never crashes. */
    n = pc_monitor_icc_search_dirs(NULL, NULL, dirs, 16);
    CHECK(has_dir(dirs, n, "/usr/share/color/icc"));
    CHECK_EQ_UINT(pc_monitor_icc_search_dirs(env_lookup, &b, dirs, 0), 0u);
}

static void test_clone_selection(void)
{
    const uint64_t h1[] = {0x1111};
    const uint64_t h1b[] = {0x1111};
    const uint64_t h2[] = {0x2222};
    const uint64_t conflict[] = {0x1111, 0x2222};
    uint64_t out = 0;

    /* Two cloned outputs sharing one profile: unambiguous. */
    const uint64_t *cand_same[] = {h1, h1b};
    const size_t count_same[] = {1, 1};
    CHECK(pc_monitor_clone_unambiguous(cand_same, count_same, 2, &out));
    CHECK_EQ_UINT(out, 0x1111u);

    /* Outputs assigned different profiles: ambiguous. */
    const uint64_t *cand_diff[] = {h1, h2};
    const size_t count_diff[] = {1, 1};
    CHECK(!pc_monitor_clone_unambiguous(cand_diff, count_diff, 2, &out));

    /* One output with two candidate profiles: ambiguous. */
    const uint64_t *cand_conf[] = {conflict};
    const size_t count_conf[] = {2};
    CHECK(!pc_monitor_clone_unambiguous(cand_conf, count_conf, 1, &out));

    /* A missing profile (count 0) is ambiguous. */
    const uint64_t *cand_missing[] = {h1, h2};
    const size_t count_missing[] = {1, 0};
    CHECK(!pc_monitor_clone_unambiguous(cand_missing, count_missing, 2, &out));

    /* One output, one profile: unambiguous. Zero outputs: nothing. */
    const uint64_t *cand_one[] = {h1};
    const size_t count_one[] = {1};
    CHECK(pc_monitor_clone_unambiguous(cand_one, count_one, 1, &out));
    CHECK(!pc_monitor_clone_unambiguous(cand_one, count_one, 0, &out));
}

static void test_live_colord(void)
{
    pc_error err;
    pc_buf out = PC_BUF_INIT;
    /* A missing output buffer is rejected, never dereferenced. */
    CHECK_EQ_INT(pc_monitor_colord_profile("DP-1", NULL, &err), PC_ERR_INVALID);
    /* A NULL/empty output name matches nothing: PC_OK with no profile, in every build. */
    CHECK_EQ_INT(pc_monitor_colord_profile(NULL, &out, &err), PC_OK);
    CHECK_EQ_UINT(out.len, 0u);
#ifdef PC_NO_COLORD
    CHECK_EQ_INT(pc_monitor_colord_profile("DP-1", &out, &err), PC_ERR_UNSUPPORTED);
    CHECK_EQ_UINT(out.len, 0u);
#else
    /* A made-up output: colord either has no such device (PC_OK, empty) or is unreachable
     * (PC_ERR_UNSUPPORTED). Never a crash, never stale bytes. */
    pc_status st = pc_monitor_colord_profile("pc-nonexistent-output-zzz", &out, &err);
    CHECK(st == PC_OK || st == PC_ERR_UNSUPPORTED);
    if (st == PC_OK) {
        CHECK_EQ_UINT(out.len, 0u);
    }
#endif
    pc_buf_fini(&out);
}

static void test_live_randr_atom(void)
{
    pc_error err;
    pc_buf out = PC_BUF_INIT;
    CHECK_EQ_INT(pc_monitor_randr_atom_profile(0, NULL, &err), PC_ERR_INVALID);
#ifdef PC_NO_X11SCREEN
    CHECK_EQ_INT(pc_monitor_randr_atom_profile(0, &out, &err), PC_ERR_UNSUPPORTED);
#else
    /* With a display this reads the root _ICC_PROFILE atom; without one it degrades cleanly. Any
     * profile that comes back is the validated, bounded RGB ICC. */
    pc_status st = pc_monitor_randr_atom_profile(0, &out, &err);
    CHECK(st == PC_OK || st == PC_ERR_UNSUPPORTED);
    if (st == PC_OK && out.len) {
        CHECK(out.len >= 132);
    }
#endif
    pc_buf_fini(&out);
}

static void test_profile_for_output(void)
{
    pc_error err;
    pc_buf out = PC_BUF_INIT;
    CHECK_EQ_INT(pc_monitor_profile_for_output("DP-1", 0, NULL, &err), PC_ERR_INVALID);
    /* The combined resolver always degrades to PC_OK (a profile or none), never crashes. */
    pc_status st = pc_monitor_profile_for_output("pc-nonexistent-output-zzz", 0, &out, &err);
    CHECK_EQ_INT(st, PC_OK);
    if (out.len) {
        CHECK(out.len >= 132);
    }
    pc_buf_fini(&out);
    /* A NULL output name skips colord but still resolves (atom fallback or none). */
    st = pc_monitor_profile_for_output(NULL, 0, &out, &err);
    CHECK_EQ_INT(st, PC_OK);
    pc_buf_fini(&out);
}

int main(int argc, char **argv)
{
    RUN(test_icc_validate);
    RUN(test_read_icc_file);
    RUN(test_search_dirs);
    RUN(test_clone_selection);
    RUN(test_live_colord);
    RUN(test_live_randr_atom);
    RUN(test_profile_for_output);
    return pc_t_finish();
}
