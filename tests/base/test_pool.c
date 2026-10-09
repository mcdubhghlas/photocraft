#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/pool.h"
#include "harness.h"

enum { N = 1000003, GRAIN = 4096 };

static float *values;

struct sum_job {
    const float *v;
    float *partial; /* one slot per chunk */
};

static pc_status sum_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    struct sum_job *j = ctx;
    float s = 0.0f;
    size_t i;
    for (i = begin; i < end; i++) {
        s += j->v[i];
    }
    j->partial[chunk] = s;
    return PC_OK;
}

/* Sum with partials combined in chunk order: must be bit-identical for any thread count. */
static float pool_sum(pc_pool *pool)
{
    size_t chunks = pc_pool_chunks(N, GRAIN);
    struct sum_job j;
    float total = 0.0f;
    size_t i;
    j.v = values;
    j.partial = pc_calloc_array(chunks, sizeof *j.partial);
    if (j.partial == NULL || pc_pool_for(pool, N, GRAIN, sum_chunk, &j) != PC_OK) {
        free(j.partial);
        return -1.0f;
    }
    for (i = 0; i < chunks; i++) {
        total += j.partial[i];
    }
    free(j.partial);
    return total;
}

static void test_chunk_math(void)
{
    CHECK_EQ_UINT(pc_pool_chunks(0, 10), 0);
    CHECK_EQ_UINT(pc_pool_chunks(10, 10), 1);
    CHECK_EQ_UINT(pc_pool_chunks(11, 10), 2);
    CHECK_EQ_UINT(pc_pool_chunks(5, 0), 5); /* grain 0 means 1 */
    CHECK_EQ_UINT(pc_pool_chunks(SIZE_MAX, 1), SIZE_MAX);
    CHECK_EQ_UINT(pc_pool_threads(NULL), 1);
}

static void test_deterministic_across_thread_counts(void)
{
    static const size_t workers[] = {0, 1, 3, 7};
    float serial = pool_sum(NULL);
    size_t w;
    for (w = 0; w < sizeof workers / sizeof workers[0]; w++) {
        pc_pool *p = NULL;
        int rep;
        CHECK_EQ_INT(pc_pool_new(workers[w], &p, NULL), PC_OK);
        CHECK_EQ_UINT(pc_pool_threads(p), workers[w] + 1);
        for (rep = 0; rep < 5; rep++) {
            float s = pool_sum(p);
            CHECK(memcmp(&s, &serial, sizeof s) == 0);
        }
        pc_pool_free(p);
    }
}

static pc_status mark_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    unsigned char *seen = ctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        seen[i]++;
    }
    return PC_OK;
}

static void test_every_index_once(void)
{
    pc_pool *p = NULL;
    unsigned char *seen = pc_calloc_array(N, 1);
    size_t i;
    size_t bad = 0;
    CHECK(seen != NULL);
    CHECK_EQ_INT(pc_pool_new(5, &p, NULL), PC_OK);
    CHECK_EQ_INT(pc_pool_for(p, N, 997, mark_chunk, seen), PC_OK);
    for (i = 0; seen != NULL && i < N; i++) {
        bad += seen[i] != 1;
    }
    CHECK_EQ_UINT(bad, 0);
    pc_pool_free(p);
    free(seen);
}

static pc_status fail_some(void *ctx, size_t chunk, size_t begin, size_t end)
{
    (void)ctx;
    (void)begin;
    (void)end;
    if (chunk == 5) {
        return PC_ERR_FORMAT;
    }
    if (chunk == 2) {
        return PC_ERR_LIMIT;
    }
    return PC_OK;
}

static void test_lowest_index_error_wins(void)
{
    pc_pool *p = NULL;
    int rep;
    CHECK_EQ_INT(pc_pool_new(7, &p, NULL), PC_OK);
    for (rep = 0; rep < 50; rep++) {
        CHECK_EQ_INT(pc_pool_for(p, 100, 10, fail_some, NULL), PC_ERR_LIMIT);
    }
    CHECK_EQ_INT(pc_pool_for(NULL, 100, 10, fail_some, NULL), PC_ERR_LIMIT);
    pc_pool_free(p);
}

struct nested {
    pc_pool *pool;
    unsigned char *seen;
};

static pc_status outer_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    struct nested *n = ctx;
    (void)chunk;
    /* Nested call on the same pool runs serially on this thread (no deadlock). */
    return pc_pool_for(n->pool, end - begin, 7, mark_chunk, n->seen + begin);
}

static void test_nested_calls(void)
{
    struct nested n;
    size_t i;
    size_t bad = 0;
    n.pool = NULL;
    n.seen = pc_calloc_array(10000, 1);
    CHECK_EQ_INT(pc_pool_new(3, &n.pool, NULL), PC_OK);
    CHECK_EQ_INT(pc_pool_for(n.pool, 10000, 100, outer_chunk, &n), PC_OK);
    for (i = 0; n.seen != NULL && i < 10000; i++) {
        bad += n.seen[i] != 1;
    }
    CHECK_EQ_UINT(bad, 0);
    pc_pool_free(n.pool);
    free(n.seen);
}

static int calls;

static pc_status count_call(void *ctx, size_t chunk, size_t begin, size_t end)
{
    (void)ctx;
    (void)chunk;
    (void)begin;
    (void)end;
    calls++;
    return PC_OK;
}

static void test_empty_and_serial(void)
{
    calls = 0;
    CHECK_EQ_INT(pc_pool_for(NULL, 0, 10, count_call, NULL), PC_OK);
    CHECK_EQ_INT(calls, 0);
    CHECK_EQ_INT(pc_pool_for(NULL, 25, 10, count_call, NULL), PC_OK);
    CHECK_EQ_INT(calls, 3);
    pc_pool_free(NULL);
}

struct caller {
    pc_pool *pool;
    float expect;
    int mismatches;
};

static void *caller_thread(void *arg)
{
    struct caller *c = arg;
    int i;
    for (i = 0; i < 20; i++) {
        float s = pool_sum(c->pool);
        if (memcmp(&s, &c->expect, sizeof s) != 0) {
            c->mismatches++;
        }
    }
    return NULL;
}

static void test_concurrent_callers(void)
{
    pc_pool *p = NULL;
    pthread_t t[3];
    struct caller c[3];
    float expect = pool_sum(NULL);
    int i;
    CHECK_EQ_INT(pc_pool_new(4, &p, NULL), PC_OK);
    for (i = 0; i < 3; i++) {
        c[i].pool = p;
        c[i].expect = expect;
        c[i].mismatches = 0;
        CHECK(pthread_create(&t[i], NULL, caller_thread, &c[i]) == 0);
    }
    for (i = 0; i < 3; i++) {
        (void)pthread_join(t[i], NULL);
        CHECK_EQ_INT(c[i].mismatches, 0);
    }
    pc_pool_free(p);
}

static void test_global_pool(void)
{
    pc_pool *g = pc_pool_global();
    CHECK(g != NULL);
    CHECK(g == pc_pool_global());
    CHECK(pc_pool_threads(g) >= 1);
    {
        float a = pool_sum(g);
        float b = pool_sum(NULL);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
    }
}

int main(int argc, char **argv)
{
    size_t i;
    int rc;
    values = pc_malloc_array(N, sizeof *values);
    if (values == NULL) {
        return 1;
    }
    /* Values whose float sum depends on association order. */
    for (i = 0; i < N; i++) {
        values[i] = (float)((i * 2654435761u) % 1000003u) * 1e-3f + (i % 7 == 0 ? 1e4f : 0.0f);
    }
    RUN(test_chunk_math);
    RUN(test_deterministic_across_thread_counts);
    RUN(test_every_index_once);
    RUN(test_lowest_index_error_wins);
    RUN(test_nested_calls);
    RUN(test_empty_and_serial);
    RUN(test_concurrent_callers);
    RUN(test_global_pool);
    rc = pc_t_finish();
    free(values);
    return rc;
}
