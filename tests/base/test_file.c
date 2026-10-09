#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "base/file.h"
#include "base/mem.h"
#include "harness.h"

/* ---- helpers ---- */

static char tmp_root[512];

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *ent;
        (void)chmod(path, 0700);
        d = opendir(path);
        if (d != NULL) {
            while ((ent = readdir(d)) != NULL) {
                char child[1024];
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                    continue;
                }
                if (snprintf(child, sizeof child, "%s/%s", path, ent->d_name) < (int)sizeof child) {
                    rm_rf(child);
                }
            }
            (void)closedir(d);
        }
        (void)rmdir(path);
    } else {
        (void)unlink(path);
    }
}

static const char *in_tmp(const char *rel)
{
    static char path[1024];
    (void)snprintf(path, sizeof path, "%s/%s", tmp_root, rel);
    return path;
}

static int count_entries(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (d == NULL) {
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            n++;
        }
    }
    (void)closedir(d);
    return n;
}

static int file_equals(const char *path, const char *want)
{
    pc_buf b = PC_BUF_INIT;
    int eq = pc_file_read(path, 1 << 20, &b, NULL) == PC_OK && b.len == strlen(want) &&
             memcmp(pc_buf_cstr(&b), want, b.len) == 0;
    pc_buf_fini(&b);
    return eq;
}

static volatile sig_atomic_t signal_hits;

static void on_usr1(int sig)
{
    (void)sig;
    signal_hits++;
}

/* Install a SIGUSR1 handler *without* SA_RESTART, so blocked read/write return EINTR. */
static void install_eintr_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGUSR1, &sa, NULL);
}

/* ---- read side: EINTR and short reads ---- */

struct read_job {
    int fd;
    pc_buf out;
    pc_status status;
};

static void *read_all_thread(void *arg)
{
    struct read_job *j = arg;
    j->status = pc_fd_read_all(j->fd, 1 << 20, &j->out, NULL);
    return NULL;
}

static void test_read_all_survives_eintr_and_short_reads(void)
{
    int fds[2];
    pthread_t t;
    struct read_job job;
    static const char msg[] = "the quick brown fox jumps over the lazy dog";
    size_t i;

    install_eintr_handler();
    signal_hits = 0;
    CHECK(pipe(fds) == 0);
    job.fd = fds[0];
    pc_buf_init(&job.out);
    job.status = PC_ERR_INVALID;
    CHECK(pthread_create(&t, NULL, read_all_thread, &job) == 0);
    sleep_ms(50); /* let the reader block in read() */
    CHECK(pthread_kill(t, SIGUSR1) == 0);
    sleep_ms(20);
    for (i = 0; i < sizeof msg - 1; i += 3) { /* dribble the data in 3-byte pieces */
        size_t n = sizeof msg - 1 - i < 3 ? sizeof msg - 1 - i : 3;
        CHECK_EQ_INT(pc_fd_write(fds[1], msg + i, n, NULL), PC_OK);
        if (i == 9) {
            CHECK(pthread_kill(t, SIGUSR1) == 0);
        }
        sleep_ms(1);
    }
    (void)close(fds[1]);
    (void)pthread_join(t, NULL);
    (void)close(fds[0]);
    CHECK(signal_hits >= 1);
    CHECK_EQ_INT(job.status, PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&job.out), msg);
    pc_buf_fini(&job.out);
}

/* ---- write side: partial writes into a full pipe, interrupted by signals ---- */

struct drain_job {
    int fd;
    pthread_t writer;
    size_t total;
    int mismatches;
};

static void *drain_thread(void *arg)
{
    struct drain_job *j = arg;
    unsigned char buf[1000];
    int kicks = 0;
    for (;;) {
        ssize_t n = read(j->fd, buf, sizeof buf);
        ssize_t k;
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        for (k = 0; k < n; k++) {
            if (buf[k] != (unsigned char)((j->total + (size_t)k) % 251)) {
                j->mismatches++;
            }
        }
        j->total += (size_t)n;
        if (kicks < 5 && j->total > (size_t)kicks * 200000) {
            (void)pthread_kill(j->writer, SIGUSR1); /* interrupt the blocked writer */
            kicks++;
        }
        sleep_ms(0);
    }
    return NULL;
}

static void test_write_survives_partial_writes_and_eintr(void)
{
    enum { N = 4 * 1024 * 1024 };
    unsigned char *data = pc_malloc(N);
    int fds[2];
    pthread_t t;
    struct drain_job job;
    size_t i;

    CHECK(data != NULL);
    if (data == NULL) {
        return;
    }
    for (i = 0; i < N; i++) {
        data[i] = (unsigned char)(i % 251);
    }
    install_eintr_handler();
    signal_hits = 0;
    CHECK(pipe(fds) == 0);
    memset(&job, 0, sizeof job);
    job.fd = fds[0];
    job.writer = pthread_self();
    CHECK(pthread_create(&t, NULL, drain_thread, &job) == 0);
    CHECK_EQ_INT(pc_fd_write(fds[1], data, N, NULL), PC_OK);
    (void)close(fds[1]);
    (void)pthread_join(t, NULL);
    (void)close(fds[0]);
    CHECK_EQ_UINT(job.total, N);
    CHECK_EQ_INT(job.mismatches, 0);
    CHECK(signal_hits >= 1);
    free(data);
}

static void test_write_to_closed_fd_fails(void)
{
    pc_error e;
    CHECK_EQ_INT(pc_fd_write(-1, "x", 1, &e), PC_ERR_IO);
    CHECK_EQ_INT(e.sys_errno, EBADF);
}

/* ---- bounded reads ---- */

static void test_read_all_limit(void)
{
    int fds[2];
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    char data[100];

    memset(data, 'z', sizeof data);
    CHECK_EQ_INT(pc_buf_append_str(&out, "keep", NULL), PC_OK);

    CHECK(pipe(fds) == 0);
    CHECK_EQ_INT(pc_fd_write(fds[1], data, sizeof data, NULL), PC_OK);
    (void)close(fds[1]);
    CHECK_EQ_INT(pc_fd_read_all(fds[0], 99, &out, &e), PC_ERR_LIMIT);
    CHECK_EQ_STR(pc_buf_cstr(&out), "keep"); /* untouched on failure */
    (void)close(fds[0]);

    CHECK(pipe(fds) == 0);
    CHECK_EQ_INT(pc_fd_write(fds[1], data, sizeof data, NULL), PC_OK);
    (void)close(fds[1]);
    CHECK_EQ_INT(pc_fd_read_all(fds[0], 100, &out, &e), PC_OK);
    CHECK_EQ_UINT(out.len, 100);
    (void)close(fds[0]);

    /* SIZE_MAX as the limit must not overflow the "one byte past max" probe. */
    CHECK(pipe(fds) == 0);
    CHECK_EQ_INT(pc_fd_write(fds[1], "ab", 2, NULL), PC_OK);
    (void)close(fds[1]);
    CHECK_EQ_INT(pc_fd_read_all(fds[0], SIZE_MAX, &out, &e), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "ab");
    (void)close(fds[0]);

    /* Empty input with a zero limit is fine. */
    CHECK(pipe(fds) == 0);
    (void)close(fds[1]);
    CHECK_EQ_INT(pc_fd_read_all(fds[0], 0, &out, &e), PC_OK);
    CHECK_EQ_UINT(out.len, 0);
    (void)close(fds[0]);
    pc_buf_fini(&out);
}

static void test_file_read_errors(void)
{
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    CHECK_EQ_INT(pc_file_read(in_tmp("missing"), 100, &out, &e), PC_ERR_NOT_FOUND);
    CHECK_EQ_INT(e.sys_errno, ENOENT);
    CHECK_CONTAINS(e.msg, "missing");
    CHECK_EQ_INT(pc_file_read(tmp_root, 100, &out, &e), PC_ERR_IO);
    CHECK_EQ_INT(e.sys_errno, EISDIR);
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("ten"), "0123456789", 10, NULL), PC_OK);
    CHECK_EQ_INT(pc_file_read(in_tmp("ten"), 9, &out, &e), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_file_read(in_tmp("ten"), 10, &out, &e), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&out), "0123456789");
    pc_buf_fini(&out);
}

/* ---- mkdir -p ---- */

static void test_mkdir_p(void)
{
    pc_error e;
    struct stat st;
    CHECK_EQ_INT(pc_mkdir_p(in_tmp("m/n/o"), &e), PC_OK);
    CHECK(stat(in_tmp("m/n/o"), &st) == 0 && S_ISDIR(st.st_mode));
    CHECK_EQ_INT(pc_mkdir_p(in_tmp("m/n/o"), &e), PC_OK); /* existing is fine */
    CHECK_EQ_INT(pc_mkdir_p(in_tmp("m//n/"), &e), PC_OK);
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("m/file"), "x", 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_mkdir_p(in_tmp("m/file"), &e), PC_ERR_IO);
    CHECK_EQ_INT(e.sys_errno, ENOTDIR);
    CHECK_EQ_INT(pc_mkdir_p(in_tmp("m/file/sub"), &e), PC_ERR_IO);
}

/* ---- atomic replacement ---- */

static void test_atomic_creates_parents(void)
{
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("a/b/c.txt"), "hello", 5, NULL), PC_OK);
    CHECK(file_equals(in_tmp("a/b/c.txt"), "hello"));
    CHECK_EQ_INT(count_entries(in_tmp("a/b")), 1); /* no temporary file left */
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("a/b/empty"), "", 0, NULL), PC_OK);
    CHECK(file_equals(in_tmp("a/b/empty"), ""));
}

static void test_atomic_replace_keeps_mode(void)
{
    struct stat st;
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("private"), "old", 3, NULL), PC_OK);
    CHECK(chmod(in_tmp("private"), 0600) == 0);
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("private"), "new contents", 12, NULL), PC_OK);
    CHECK(file_equals(in_tmp("private"), "new contents"));
    CHECK(stat(in_tmp("private"), &st) == 0);
    CHECK_EQ_UINT(st.st_mode & 07777, 0600);
}

static void test_atomic_writes_through_symlink(void)
{
    char target[1024];
    struct stat st;
    (void)snprintf(target, sizeof target, "%s", in_tmp("real.txt"));
    CHECK_EQ_INT(pc_file_write_atomic(target, "v1", 2, NULL), PC_OK);
    CHECK(symlink(target, in_tmp("link")) == 0);
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("link"), "v2", 2, NULL), PC_OK);
    CHECK(lstat(in_tmp("link"), &st) == 0 && S_ISLNK(st.st_mode));
    CHECK(file_equals(target, "v2"));
}

static void test_atomic_rejects_directory_paths(void)
{
    pc_error e;
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("dir/"), "x", 1, &e), PC_ERR_INVALID);
    CHECK_CONTAINS(e.msg, "not a file path");
    CHECK_EQ_INT(pc_mkdir_p(in_tmp("isdir"), NULL), PC_OK);
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("isdir"), "x", 1, &e), PC_ERR_IO);
    CHECK_CONTAINS(e.msg, "the original file was not changed");
    CHECK_EQ_INT(count_entries(tmp_root) >= 0, 1);
    CHECK_EQ_INT(count_entries(in_tmp("isdir")), 0);
}

static void test_atomic_unwritable_folder_keeps_original(void)
{
    pc_error e;
    int before;
    if (geteuid() == 0) {
        (void)printf("(skipping unwritable-folder test as root) ");
        return;
    }
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("ro/f"), "old", 3, NULL), PC_OK);
    before = count_entries(in_tmp("ro"));
    CHECK(chmod(in_tmp("ro"), 0500) == 0);
    CHECK_EQ_INT(pc_file_write_atomic(in_tmp("ro/f"), "new", 3, &e), PC_ERR_IO);
    CHECK_CONTAINS(e.msg, "is not writable");
    CHECK_CONTAINS(e.msg, "the original file was not changed");
    CHECK(e.sys_errno == EACCES || e.sys_errno == EPERM);
    CHECK(file_equals(in_tmp("ro/f"), "old"));
    CHECK_EQ_INT(count_entries(in_tmp("ro")), before);
    (void)chmod(in_tmp("ro"), 0700);
}

int main(int argc, char **argv)
{
    const char *base = getenv("TMPDIR");
    int rc;
    (void)snprintf(tmp_root, sizeof tmp_root, "%s/pctest-XXXXXX", base != NULL && *base ? base : "/tmp");
    if (mkdtemp(tmp_root) == NULL) {
        (void)fprintf(stderr, "cannot create a temporary directory\n");
        return 1;
    }
    RUN(test_read_all_survives_eintr_and_short_reads);
    RUN(test_write_survives_partial_writes_and_eintr);
    RUN(test_write_to_closed_fd_fails);
    RUN(test_read_all_limit);
    RUN(test_file_read_errors);
    RUN(test_mkdir_p);
    RUN(test_atomic_creates_parents);
    RUN(test_atomic_replace_keeps_mode);
    RUN(test_atomic_writes_through_symlink);
    RUN(test_atomic_rejects_directory_paths);
    RUN(test_atomic_unwritable_folder_keeps_original);
    rc = pc_t_finish();
    rm_rf(tmp_root);
    return rc;
}
