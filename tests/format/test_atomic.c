/* Crash-safe replacement (crates/format/src/atomic/tests.rs). The behaviour tests run against
 * both pc_atomic_write (= base's pc_file_write_atomic, what every save uses) and
 * pc_atomic_write_with over the real file system; the failure-injection tests go through a
 * faulty pc_atomic_fs. */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "format/bridge.h"
#include "harness.h"

static const char ORIGINAL[] = "the user's precious original document";

typedef pc_status (*writer_fn)(const char *path, const void *data, size_t len, pc_error *err);

static pc_status via_seam(const char *path, const void *data, size_t len, pc_error *err)
{
    return pc_atomic_write_with(&pc_atomic_real_fs, pc_rename_retry_platform(), path, data, len, err);
}

static const writer_fn WRITERS[2] = {pc_atomic_write, via_seam};

typedef struct tdir {
    char path[512];
} tdir;

static void tdir_new(tdir *d, const char *tag)
{
    if (t_mkdtemp(tag, d->path, sizeof d->path) != 0) {
        PC_T_FAIL("mkdtemp");
        d->path[0] = '\0';
    }
}

static void at(const tdir *d, const char *rel, char *out, size_t cap)
{
    if (snprintf(out, cap, "%s/%s", d->path, rel) >= (int)cap) {
        out[0] = '\0';
    }
}

static bool file_is(const char *path, const char *want)
{
    pc_buf b = PC_BUF_INIT;
    bool ok = t_read_file(path, &b) == 0 && b.len == strlen(want) && memcmp(b.data, want, b.len) == 0;
    pc_buf_fini(&b);
    return ok;
}

/* Only the target remains: no temporary file was left behind. */
static void check_no_temp(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    CHECK(d != NULL);
    while (d != NULL && (e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n >= 4 && strcmp(e->d_name + n - 4, ".tmp") == 0) {
            PC_T_FAIL("temporary file left behind: %s", e->d_name);
        }
    }
    if (d != NULL) {
        (void)closedir(d);
    }
}

/* ---- the faulty file system ---- */

typedef struct faulty {
    long fail_write_after;   /* >= 0: write this many bytes, then fail like a full disk */
    int fail_rename;         /* errno every rename fails with (0 = none) */
    unsigned locked_renames; /* fail the first N renames with EACCES (a transient lock) */
    unsigned renames;
} faulty;

static int f_write_new(void *ctx, const char *tmp, const void *data, size_t len)
{
    faulty *f = ctx;
    if (f->fail_write_after >= 0) {
        size_t n = (size_t)f->fail_write_after < len ? (size_t)f->fail_write_after : len;
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd < 0) {
            return errno;
        }
        (void)pc_fd_write(fd, data, n, NULL);
        (void)close(fd);
        return ENOSPC; /* injected: no space left on device */
    }
    return pc_atomic_real_fs.write_new(NULL, tmp, data, len);
}

static int f_rename(void *ctx, const char *from, const char *to)
{
    faulty *f = ctx;
    f->renames++;
    if (f->fail_rename != 0) {
        return f->fail_rename;
    }
    if (f->locked_renames > 0) {
        f->locked_renames--;
        return EACCES; /* injected: sharing violation */
    }
    return pc_atomic_real_fs.rename(NULL, from, to);
}

static int f_remove(void *ctx, const char *path)
{
    (void)ctx;
    return pc_atomic_real_fs.remove(NULL, path);
}

static int f_sync_dir(void *ctx, const char *dir)
{
    (void)ctx;
    return pc_atomic_real_fs.sync_dir(NULL, dir);
}

static int f_copy_permissions(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    return pc_atomic_real_fs.copy_permissions(NULL, from, to);
}

static pc_atomic_fs faulty_fs(faulty *f)
{
    pc_atomic_fs fs = {f, f_write_new, f_rename, f_remove, f_sync_dir, f_copy_permissions};
    return fs;
}

static const pc_rename_retry NO_RETRY = {0, 0};

/* ---- tests ---- */

static void test_replaces_and_creates(void)
{
    for (int w = 0; w < 2; w++) {
        tdir d;
        char p[600];
        tdir_new(&d, "ok");
        at(&d, "doc.psd", p, sizeof p);
        CHECK(WRITERS[w](p, "first", 5, NULL) == PC_OK);
        CHECK(file_is(p, "first"));
        CHECK(WRITERS[w](p, "second, longer", 14, NULL) == PC_OK);
        CHECK(file_is(p, "second, longer"));
        CHECK(WRITERS[w](p, "", 0, NULL) == PC_OK);
        CHECK(file_is(p, ""));
        check_no_temp(d.path);
        t_rm_rf(d.path);
    }
}

static void test_creates_missing_parent_folders(void)
{
    for (int w = 0; w < 2; w++) {
        tdir d;
        char p[600];
        tdir_new(&d, "parent");
        at(&d, "a/b/out.png", p, sizeof p);
        CHECK(WRITERS[w](p, "x", 1, NULL) == PC_OK);
        CHECK(file_is(p, "x"));
        t_rm_rf(d.path);
    }
}

static void test_write_failure_halfway_keeps_original(void)
{
    tdir d;
    char p[600];
    uint8_t data[4096];
    faulty f = {5, 0, 0, 0};
    pc_atomic_fs fs = faulty_fs(&f);
    pc_error e;
    memset(data, 7, sizeof data);
    tdir_new(&d, "write");
    at(&d, "doc.psd", p, sizeof p);
    CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
    CHECK(pc_atomic_write_with(&fs, NO_RETRY, p, data, sizeof data, &e) == PC_ERR_IO);
    CHECK_EQ_INT(e.sys_errno, ENOSPC);
    CHECK_CONTAINS(e.msg, "the disk is full");
    CHECK_CONTAINS(e.msg, "not changed");
    CHECK(file_is(p, ORIGINAL));
    CHECK_EQ_UINT(f.renames, 0); /* never renames after a failed write */
    check_no_temp(d.path);
    t_rm_rf(d.path);
}

static void test_rename_failure_keeps_original(void)
{
    tdir d;
    char p[600];
    faulty f = {-1, EIO, 0, 0};
    pc_atomic_fs fs = faulty_fs(&f);
    pc_error e;
    tdir_new(&d, "rename");
    at(&d, "doc.psd", p, sizeof p);
    CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
    CHECK(pc_atomic_write_with(&fs, NO_RETRY, p, "new", 3, &e) == PC_ERR_IO);
    CHECK_CONTAINS(e.msg, "not changed");
    CHECK(file_is(p, ORIGINAL));
    check_no_temp(d.path);
    t_rm_rf(d.path);
}

static void test_cross_device_rename_errors_clearly_and_never_writes_in_place(void)
{
    tdir d;
    char p[600];
    faulty f = {-1, EXDEV, 0, 0};
    pc_atomic_fs fs = faulty_fs(&f);
    pc_error e;
    tdir_new(&d, "exdev");
    at(&d, "doc.psd", p, sizeof p);
    CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
    CHECK(pc_atomic_write_with(&fs, NO_RETRY, p, "new", 3, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "different volume");
    CHECK_EQ_INT(e.sys_errno, EXDEV);
    CHECK(file_is(p, ORIGINAL));
    check_no_temp(d.path);
    t_rm_rf(d.path);
}

static void test_locked_target_rename_is_retried(void)
{
    tdir d;
    char p[600];
    faulty f = {-1, 0, 2, 0};
    pc_atomic_fs fs = faulty_fs(&f);
    pc_rename_retry retry = {3, 1};
    tdir_new(&d, "locked");
    at(&d, "doc.psd", p, sizeof p);
    CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
    CHECK(pc_atomic_write_with(&fs, retry, p, "new", 3, NULL) == PC_OK);
    CHECK(file_is(p, "new"));
    CHECK_EQ_UINT(f.renames, 3);
    check_no_temp(d.path);
    t_rm_rf(d.path);
}

static void test_lock_that_outlasts_the_retries_keeps_original(void)
{
    tdir d;
    char p[600];
    faulty f = {-1, 0, 10, 0};
    pc_atomic_fs fs = faulty_fs(&f);
    pc_rename_retry retry = {2, 1};
    pc_error e;
    tdir_new(&d, "locked2");
    at(&d, "doc.psd", p, sizeof p);
    CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
    CHECK(pc_atomic_write_with(&fs, retry, p, "new", 3, &e) == PC_ERR_IO);
    CHECK_EQ_INT(e.sys_errno, EACCES);
    CHECK_CONTAINS(e.msg, "locked");
    CHECK_EQ_UINT(f.renames, 3);
    CHECK(file_is(p, ORIGINAL));
    check_no_temp(d.path);
    t_rm_rf(d.path);
}

static void test_read_only_folder_errors_clearly_and_never_writes_in_place(void)
{
    for (int w = 0; w < 2; w++) {
        tdir d;
        char p[600];
        char probe[600];
        int fd;
        pc_error e;
        tdir_new(&d, "ro");
        at(&d, "doc.psd", p, sizeof p);
        at(&d, "probe", probe, sizeof probe);
        CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
        /* The file itself stays writable: an in-place fallback would succeed and must not happen. */
        CHECK(chmod(d.path, 0555) == 0);
        fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { /* running as root: permissions aren't enforced */
            (void)close(fd);
            t_rm_rf(d.path);
            return;
        }
        CHECK(WRITERS[w](p, "new", 3, &e) == PC_ERR_IO);
        CHECK_EQ_INT(e.sys_errno, EACCES);
        CHECK_CONTAINS(e.msg, "not writable");
        CHECK(file_is(p, ORIGINAL));
        CHECK(chmod(d.path, 0755) == 0);
        check_no_temp(d.path);
        t_rm_rf(d.path);
    }
}

static void test_keeps_the_original_file_mode(void)
{
    for (int w = 0; w < 2; w++) {
        tdir d;
        char p[600];
        struct stat st;
        tdir_new(&d, "mode");
        at(&d, "doc.psd", p, sizeof p);
        CHECK(t_write_file(p, ORIGINAL, strlen(ORIGINAL)) == 0);
        CHECK(chmod(p, 0600) == 0);
        CHECK(WRITERS[w](p, "new", 3, NULL) == PC_OK);
        CHECK(stat(p, &st) == 0);
        CHECK_EQ_UINT(st.st_mode & 0777, 0600);
        t_rm_rf(d.path);
    }
}

static void test_writes_through_a_symlink(void)
{
    for (int w = 0; w < 2; w++) {
        tdir d;
        char real[600];
        char link[600];
        struct stat st;
        tdir_new(&d, "link");
        at(&d, "real.psd", real, sizeof real);
        at(&d, "link.psd", link, sizeof link);
        CHECK(t_write_file(real, ORIGINAL, strlen(ORIGINAL)) == 0);
        CHECK(symlink(real, link) == 0);
        CHECK(WRITERS[w](link, "new", 3, NULL) == PC_OK);
        CHECK(lstat(link, &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(file_is(real, "new"));
        t_rm_rf(d.path);
    }
}

static void test_directory_target_or_bare_root_is_an_error_not_a_panic(void)
{
    for (int w = 0; w < 2; w++) {
        tdir d;
        tdir_new(&d, "dir");
        CHECK(WRITERS[w](d.path, "x", 1, NULL) != PC_OK);
        CHECK(WRITERS[w]("/", "x", 1, NULL) != PC_OK);
        CHECK(WRITERS[w]("", "x", 1, NULL) != PC_OK);
        check_no_temp(d.path);
        t_rm_rf(d.path);
    }
}

typedef struct counting {
    int results[4];
    int calls;
} counting;

static int counted_rename(void *ctx)
{
    counting *c = ctx;
    return c->results[c->calls++ < 3 ? c->calls - 1 : 3];
}

static void test_retry_rename_policy(void)
{
    pc_rename_retry none = pc_rename_retry_platform();
    pc_rename_retry three = {3, 0};
    counting c = {{EACCES, EPERM, 0, 0}, 0};
    CHECK_EQ_UINT(none.attempts, 0); /* POSIX: a permission error is real, never retried */
    CHECK_EQ_INT(pc_atomic_retry_rename(none, counted_rename, &c), EACCES);
    CHECK_EQ_INT(c.calls, 1);
    c.calls = 0;
    CHECK_EQ_INT(pc_atomic_retry_rename(three, counted_rename, &c), 0);
    CHECK_EQ_INT(c.calls, 3);
    {
        counting other = {{EIO, 0, 0, 0}, 0};
        CHECK_EQ_INT(pc_atomic_retry_rename(three, counted_rename, &other), EIO); /* only EACCES/EPERM */
        CHECK_EQ_INT(other.calls, 1);
    }
}

static void test_temp_names_are_unique_short_and_hidden(void)
{
    pc_buf a = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    char long_name[300];
    memset(long_name, 'x', 299);
    long_name[299] = '\0';
    CHECK(pc_atomic_temp_name("doc.psd", &a, NULL) == PC_OK);
    CHECK(pc_atomic_temp_name("doc.psd", &b, NULL) == PC_OK);
    CHECK(strcmp(pc_buf_cstr(&a), pc_buf_cstr(&b)) != 0);
    CHECK(pc_buf_cstr(&a)[0] == '.');
    CHECK(strncmp(pc_buf_cstr(&a), ".doc.psd.", 9) == 0);
    CHECK(a.len > 11 && strcmp(pc_buf_cstr(&a) + a.len - 11, ".pcsave.tmp") == 0);
    CHECK(pc_atomic_temp_name(long_name, &a, NULL) == PC_OK);
    CHECK(a.len < 64 + 40); /* a long name plus the suffix stays under NAME_MAX */
    /* 64 characters, never a split UTF-8 sequence. */
    {
        char multi[256] = "";
        for (int i = 0; i < 40; i++) {
            strcat(multi, "\xc3\xbc\xe2\x98\x83");
        }
        CHECK(pc_atomic_temp_name(multi, &a, NULL) == PC_OK);
        CHECK(memcmp(pc_buf_cstr(&a) + 1, multi, 32 * 5) == 0);
        CHECK(pc_buf_cstr(&a)[1 + 32 * 5] == '.');
    }
    pc_buf_fini(&a);
    pc_buf_fini(&b);
}

static void test_format_write_atomic_reports_io(void)
{
    pc_error e;
    CHECK(pc_format_write_atomic("", "x", 1, &e) != PC_OK);
    CHECK(strncmp(e.msg, "I/O: ", 5) == 0);
    CHECK(pc_format_error_kind(e.code, &e) == PC_FORMAT_ERR_IO);
}

int main(int argc, char **argv)
{
    RUN(test_replaces_and_creates);
    RUN(test_creates_missing_parent_folders);
    RUN(test_write_failure_halfway_keeps_original);
    RUN(test_rename_failure_keeps_original);
    RUN(test_cross_device_rename_errors_clearly_and_never_writes_in_place);
    RUN(test_locked_target_rename_is_retried);
    RUN(test_lock_that_outlasts_the_retries_keeps_original);
    RUN(test_read_only_folder_errors_clearly_and_never_writes_in_place);
    RUN(test_keeps_the_original_file_mode);
    RUN(test_writes_through_a_symlink);
    RUN(test_directory_target_or_bare_root_is_an_error_not_a_panic);
    RUN(test_retry_rename_policy);
    RUN(test_temp_names_are_unique_short_and_hidden);
    RUN(test_format_write_atomic_reports_io);
    return pc_t_finish();
}
