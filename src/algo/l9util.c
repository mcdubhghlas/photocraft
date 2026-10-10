/* L9 helpers (see l9util.h). */
#include "algo/l9util.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

int pc_algo_l9u_total_cmpf(float a, float b)
{
    int32_t ia, ib;
    uint32_t ua, ub;
    memcpy(&ua, &a, 4);
    memcpy(&ub, &b, 4);
    ua ^= (uint32_t)((uint32_t)(-(int32_t)(ua >> 31)) >> 1);
    ub ^= (uint32_t)((uint32_t)(-(int32_t)(ub >> 31)) >> 1);
    ia = pc_i32_from_bits(ua);
    ib = pc_i32_from_bits(ub);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

int pc_algo_l9u_total_cmp(double a, double b)
{
    uint64_t ua, ub;
    int64_t ia, ib;
    memcpy(&ua, &a, 8);
    memcpy(&ub, &b, 8);
    if (ua >> 63) {
        ua ^= 0x7FFFFFFFFFFFFFFFull;
    }
    if (ub >> 63) {
        ub ^= 0x7FFFFFFFFFFFFFFFull;
    }
    /* Two's complement reinterpretation without implementation-defined conversion. */
    ia = (ua >> 63) ? -(int64_t)(~ua) - 1 : (int64_t)ua;
    ib = (ub >> 63) ? -(int64_t)(~ub) - 1 : (int64_t)ub;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

static int cmp_f(const void *a, const void *b)
{
    return pc_algo_l9u_total_cmpf(*(const float *)a, *(const float *)b);
}
static int cmp_d(const void *a, const void *b)
{
    return pc_algo_l9u_total_cmp(*(const double *)a, *(const double *)b);
}

/* Equal keys under total_cmp have identical bits, so an unstable sort gives the same result. */
void pc_algo_l9u_sortf(float *v, size_t n)
{
    if (n > 1) {
        qsort(v, n, sizeof *v, cmp_f);
    }
}
void pc_algo_l9u_sort(double *v, size_t n)
{
    if (n > 1) {
        qsort(v, n, sizeof *v, cmp_d);
    }
}

static pc_status alloc_n(size_t n, size_t elem, bool zero, void **out, pc_error *err)
{
    size_t bytes;
    void *p;
    if (!pc_size_mul(n, elem, &bytes) || bytes > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "buffer of %zu values exceeds the allocation limit", n);
    }
    p = zero ? pc_calloc_array(n == 0 ? 1 : n, elem) : pc_malloc(bytes == 0 ? 1 : bytes);
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = p;
    return PC_OK;
}

pc_status pc_algo_l9u_alloc_f(size_t n, bool zero, float **out, pc_error *err)
{
    void *p = NULL;
    pc_status st = alloc_n(n, sizeof(float), zero, &p, err);
    if (st == PC_OK) {
        *out = p;
    }
    return st;
}

pc_status pc_algo_l9u_alloc_d(size_t n, bool zero, double **out, pc_error *err)
{
    void *p = NULL;
    pc_status st = alloc_n(n, sizeof(double), zero, &p, err);
    if (st == PC_OK) {
        *out = p;
    }
    return st;
}
