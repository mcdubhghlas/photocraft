#include "pool.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "mem.h"

enum { MAX_WORKERS = 256 };

struct pc_pool {
    pthread_mutex_t mu;        /* guards everything below */
    pthread_cond_t work_cv;    /* a job was posted, or stop */
    pthread_cond_t done_cv;    /* the last chunk of the job finished */
    pthread_mutex_t submit_mu; /* serialises pc_pool_for callers */
    pthread_t *threads;
    size_t nthreads;
    bool stop;

    /* the current job; valid while active */
    bool active;
    pc_pool_fn fn;
    void *ctx;
    size_t n;
    size_t grain;
    size_t nchunks;
    size_t next;     /* next chunk to hand out */
    size_t finished; /* chunks completed */
    size_t err_index;
    pc_status err_status;
};

/* Set while this thread is running a chunk: nested pc_pool_for calls run serially. */
static _Thread_local bool in_chunk;

size_t pc_pool_chunks(size_t n, size_t grain)
{
    if (grain == 0) {
        grain = 1;
    }
    return n / grain + (n % grain != 0 ? 1u : 0u);
}

static pc_status run_one(pc_pool_fn fn, void *ctx, size_t n, size_t grain, size_t i)
{
    size_t begin = i * grain; /* < n, so no overflow */
    size_t end = n - begin < grain ? n : begin + grain;
    bool was = in_chunk;
    pc_status s;
    in_chunk = true;
    s = fn(ctx, i, begin, end);
    in_chunk = was;
    return s;
}

/* Take chunks of the active job until none are left. Called without the lock held. */
static void run_chunks(pc_pool *p)
{
    for (;;) {
        size_t i;
        pc_status s;
        pc_pool_fn fn;
        void *ctx;
        size_t n;
        size_t grain;

        (void)pthread_mutex_lock(&p->mu);
        if (!p->active || p->next >= p->nchunks) {
            (void)pthread_mutex_unlock(&p->mu);
            return;
        }
        i = p->next++;
        fn = p->fn;
        ctx = p->ctx;
        n = p->n;
        grain = p->grain;
        (void)pthread_mutex_unlock(&p->mu);

        s = run_one(fn, ctx, n, grain, i);

        (void)pthread_mutex_lock(&p->mu);
        if (s != PC_OK && i < p->err_index) {
            p->err_index = i;
            p->err_status = s;
        }
        p->finished++;
        if (p->finished == p->nchunks) {
            (void)pthread_cond_signal(&p->done_cv);
        }
        (void)pthread_mutex_unlock(&p->mu);
    }
}

static void *worker_main(void *arg)
{
    pc_pool *p = arg;
    (void)pthread_mutex_lock(&p->mu);
    for (;;) {
        while (!p->stop && !(p->active && p->next < p->nchunks)) {
            (void)pthread_cond_wait(&p->work_cv, &p->mu);
        }
        if (p->stop) {
            break;
        }
        (void)pthread_mutex_unlock(&p->mu);
        run_chunks(p);
        (void)pthread_mutex_lock(&p->mu);
    }
    (void)pthread_mutex_unlock(&p->mu);
    return NULL;
}

static void stop_and_join(pc_pool *p)
{
    size_t i;
    (void)pthread_mutex_lock(&p->mu);
    p->stop = true;
    (void)pthread_cond_broadcast(&p->work_cv);
    (void)pthread_mutex_unlock(&p->mu);
    for (i = 0; i < p->nthreads; i++) {
        (void)pthread_join(p->threads[i], NULL);
    }
    p->nthreads = 0;
}

pc_status pc_pool_new(size_t workers, pc_pool **out, pc_error *err)
{
    pc_pool *p;
    int rc;

    if (workers > MAX_WORKERS) {
        workers = MAX_WORKERS;
    }
    p = calloc(1, sizeof *p);
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory creating a thread pool");
    }
    if ((rc = pthread_mutex_init(&p->mu, NULL)) != 0) {
        free(p);
        return pc_fail_errno(err, PC_ERR_IO, rc, "cannot create a thread pool");
    }
    if ((rc = pthread_mutex_init(&p->submit_mu, NULL)) != 0) {
        (void)pthread_mutex_destroy(&p->mu);
        free(p);
        return pc_fail_errno(err, PC_ERR_IO, rc, "cannot create a thread pool");
    }
    if ((rc = pthread_cond_init(&p->work_cv, NULL)) != 0) {
        (void)pthread_mutex_destroy(&p->submit_mu);
        (void)pthread_mutex_destroy(&p->mu);
        free(p);
        return pc_fail_errno(err, PC_ERR_IO, rc, "cannot create a thread pool");
    }
    if ((rc = pthread_cond_init(&p->done_cv, NULL)) != 0) {
        (void)pthread_cond_destroy(&p->work_cv);
        (void)pthread_mutex_destroy(&p->submit_mu);
        (void)pthread_mutex_destroy(&p->mu);
        free(p);
        return pc_fail_errno(err, PC_ERR_IO, rc, "cannot create a thread pool");
    }
    if (workers > 0) {
        p->threads = pc_calloc_array(workers, sizeof *p->threads);
        if (p->threads == NULL) {
            pc_pool_free(p);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory creating a thread pool");
        }
    }
    for (p->nthreads = 0; p->nthreads < workers; p->nthreads++) {
        rc = pthread_create(&p->threads[p->nthreads], NULL, worker_main, p);
        if (rc != 0) {
            pc_pool_free(p);
            return pc_fail_errno(err, PC_ERR_IO, rc, "cannot start thread-pool workers");
        }
    }
    *out = p;
    return PC_OK;
}

void pc_pool_free(pc_pool *pool)
{
    if (pool == NULL) {
        return;
    }
    stop_and_join(pool);
    (void)pthread_cond_destroy(&pool->done_cv);
    (void)pthread_cond_destroy(&pool->work_cv);
    (void)pthread_mutex_destroy(&pool->submit_mu);
    (void)pthread_mutex_destroy(&pool->mu);
    free(pool->threads);
    free(pool);
}

size_t pc_pool_threads(const pc_pool *pool)
{
    return pool != NULL ? pool->nthreads + 1 : 1;
}

pc_status pc_pool_for(pc_pool *pool, size_t n, size_t grain, pc_pool_fn fn, void *ctx)
{
    size_t nchunks;
    pc_status result;

    if (grain == 0) {
        grain = 1;
    }
    nchunks = pc_pool_chunks(n, grain);
    if (nchunks == 0) {
        return PC_OK;
    }
    if (pool == NULL || pool->nthreads == 0 || nchunks == 1 || in_chunk) {
        size_t i;
        result = PC_OK;
        for (i = 0; i < nchunks; i++) {
            pc_status s = run_one(fn, ctx, n, grain, i);
            if (s != PC_OK && result == PC_OK) {
                result = s;
            }
        }
        return result;
    }

    (void)pthread_mutex_lock(&pool->submit_mu);
    (void)pthread_mutex_lock(&pool->mu);
    pool->fn = fn;
    pool->ctx = ctx;
    pool->n = n;
    pool->grain = grain;
    pool->nchunks = nchunks;
    pool->next = 0;
    pool->finished = 0;
    pool->err_index = nchunks;
    pool->err_status = PC_OK;
    pool->active = true;
    (void)pthread_cond_broadcast(&pool->work_cv);
    (void)pthread_mutex_unlock(&pool->mu);

    run_chunks(pool);

    (void)pthread_mutex_lock(&pool->mu);
    while (pool->finished < pool->nchunks) {
        (void)pthread_cond_wait(&pool->done_cv, &pool->mu);
    }
    pool->active = false;
    result = pool->err_status;
    (void)pthread_mutex_unlock(&pool->mu);
    (void)pthread_mutex_unlock(&pool->submit_mu);
    return result;
}

static pc_pool *global_pool;
static pthread_once_t global_once = PTHREAD_ONCE_INIT;

static size_t env_threads(void)
{
    const char *v = getenv("PHOTOCRAFT_THREADS");
    char *end = NULL;
    unsigned long n;
    if (v == NULL || *v == '\0') {
        return 0;
    }
    errno = 0;
    n = strtoul(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || n == 0 || n > MAX_WORKERS + 1) {
        return 0;
    }
    return (size_t)n;
}

static void global_init(void)
{
    size_t threads = env_threads();
    if (threads == 0) {
#ifdef _SC_NPROCESSORS_ONLN
        long cpus = sysconf(_SC_NPROCESSORS_ONLN); /* widely available, not in POSIX proper */
        threads = cpus > 0 ? (size_t)cpus : 1;
#else
        threads = 1;
#endif
    }
    if (pc_pool_new(threads - 1, &global_pool, NULL) != PC_OK) {
        global_pool = NULL;
    }
}

pc_pool *pc_pool_global(void)
{
    if (pthread_once(&global_once, global_init) != 0) {
        return NULL;
    }
    return global_pool;
}
