/* Whole-file reads (crates/format/src/read.rs tests). */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "format/format.h"
#include "harness.h"

static void test_reads_whole_file_across_many_chunks(void)
{
    char dir[512];
    char p[600];
    size_t n = 100003;
    uint8_t *data = malloc(n);
    static const size_t CHUNKS[] = {1, 7, 4096, 100003, 1 << 20, 0};
    static const uint64_t HINTS[] = {0, 10, 100003, 1 << 20};
    pc_buf got = PC_BUF_INIT;
    for (uint32_t i = 0; i < n; i++) {
        data[i] = (uint8_t)(i * 7 % 251);
    }
    CHECK(t_mkdtemp("read", dir, sizeof dir) == 0);
    (void)snprintf(p, sizeof p, "%s/f.bin", dir);
    CHECK(t_write_file(p, data, n) == 0);
    for (size_t c = 0; c < sizeof CHUNKS / sizeof *CHUNKS; c++) { /* a zero chunk still makes progress */
        CHECK(pc_read_file_with(p, CHUNKS[c], &got, NULL) == PC_OK);
        CHECK(got.len == n && memcmp(got.data, data, n) == 0);
    }
    CHECK(pc_read_file(p, &got, NULL) == PC_OK);
    CHECK(got.len == n && memcmp(got.data, data, n) == 0);
    /* From an open descriptor, whatever the hint says. */
    for (size_t h = 0; h < sizeof HINTS / sizeof *HINTS; h++) {
        int fd = open(p, O_RDONLY);
        CHECK(fd >= 0);
        CHECK(pc_read_all(fd, HINTS[h], &got, NULL) == PC_OK);
        CHECK(got.len == n && memcmp(got.data, data, n) == 0);
        (void)close(fd);
    }
    /* A pipe: no size at all. */
    {
        int fds[2];
        CHECK(pipe(fds) == 0);
        CHECK(pc_fd_write(fds[1], data, 4000, NULL) == PC_OK);
        (void)close(fds[1]);
        CHECK(pc_read_all_with(fds[0], 0, 3, &got, NULL) == PC_OK);
        CHECK(got.len == 4000 && memcmp(got.data, data, 4000) == 0);
        (void)close(fds[0]);
    }
    t_rm_rf(dir);
    free(data);
    pc_buf_fini(&got);
}

static void test_empty_and_missing_files(void)
{
    char dir[512];
    char p[600];
    pc_buf got = PC_BUF_INIT;
    pc_error e;
    CHECK(t_mkdtemp("read-empty", dir, sizeof dir) == 0);
    (void)snprintf(p, sizeof p, "%s/empty", dir);
    CHECK(t_write_file(p, "", 0) == 0);
    CHECK(pc_read_file(p, &got, NULL) == PC_OK);
    CHECK_EQ_UINT(got.len, 0);
    (void)snprintf(p, sizeof p, "%s/photocraft-read-does-not-exist.psb", dir);
    (void)pc_buf_append_str(&got, "kept", NULL);
    CHECK(pc_read_file(p, &got, &e) == PC_ERR_NOT_FOUND);
    CHECK_EQ_INT(e.sys_errno, ENOENT);
    CHECK_EQ_STR(pc_buf_cstr(&got), "kept");
    /* A directory is an error, not a crash or an endless read. */
    CHECK(pc_read_file(dir, &got, &e) == PC_ERR_IO);
    CHECK_EQ_INT(e.sys_errno, EISDIR);
    t_rm_rf(dir);
    pc_buf_fini(&got);
}

static void test_a_file_too_large_for_memory_is_a_clear_error(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    /* More than any address space can hold. */
    CHECK(pc_read_buffer_for(UINT64_MAX, &b, &e) == PC_ERR_NOMEM);
    CHECK_CONTAINS(e.msg, "18446744.07 TB");
    CHECK_EQ_STR(e.msg, "not enough memory to read the whole file (18446744.07 TB)");
    CHECK(pc_read_buffer_for((uint64_t)1 << 62, &b, &e) == PC_ERR_NOMEM);
    CHECK(pc_read_buffer_for(PC_READ_MAX_RESERVE, &b, &e) == PC_ERR_NOMEM);
    CHECK(pc_read_buffer_for(1024, &b, NULL) == PC_OK);
    CHECK_EQ_UINT(b.cap - 1, 1024);
    CHECK_EQ_UINT(b.len, 0);
    pc_buf_fini(&b);
}

static void test_sizes_read_like_the_os_shows_them(void)
{
    char s[48];
    pc_read_size_text(512, s);
    CHECK_EQ_STR(s, "512 bytes");
    pc_read_size_text(4750000000ull, s);
    CHECK_EQ_STR(s, "4.75 GB");
    pc_read_size_text(759000000ull, s);
    CHECK_EQ_STR(s, "759.00 MB");
    pc_read_size_text(999, s);
    CHECK_EQ_STR(s, "999 bytes");
    pc_read_size_text(1000, s);
    CHECK_EQ_STR(s, "1.00 KB");
    pc_read_size_text(UINT64_MAX, s);
    CHECK_EQ_STR(s, "18446744.07 TB");
}

int main(int argc, char **argv)
{
    RUN(test_reads_whole_file_across_many_chunks);
    RUN(test_empty_and_missing_files);
    RUN(test_a_file_too_large_for_memory_is_a_clear_error);
    RUN(test_sizes_read_like_the_os_shows_them);
    return pc_t_finish();
}
