#include <stdlib.h>

#include "base/mem.h"
#include "harness.h"

static void test_malloc_cap_and_zero(void)
{
    void *p = pc_malloc(0);
    CHECK(p != NULL);
    free(p);
    CHECK(pc_malloc(PC_MAX_ALLOC + 1) == NULL);
    CHECK(pc_malloc(SIZE_MAX) == NULL);
}

static void test_array_overflow(void)
{
    unsigned char *z;
    size_t i;
    int all_zero = 1;
    CHECK(pc_malloc_array(SIZE_MAX / 2 + 1, 2) == NULL);
    CHECK(pc_calloc_array(SIZE_MAX / 8, 16) == NULL);
    CHECK(pc_calloc_array(PC_MAX_ALLOC, 2) == NULL);
    z = pc_calloc_array(100, 3);
    CHECK(z != NULL);
    if (z != NULL) {
        for (i = 0; i < 300; i++) {
            all_zero &= z[i] == 0;
        }
    }
    CHECK(all_zero);
    free(z);
    z = pc_calloc_array(0, 8);
    CHECK(z != NULL);
    free(z);
}

static void test_grow_array(void)
{
    int *a = NULL;
    int *n;
    size_t cap = 0;
    size_t i;

    for (i = 0; i < 1000; i++) {
        n = pc_grow_array(a, &cap, i + 1, sizeof *a);
        CHECK(n != NULL);
        if (n == NULL) {
            break;
        }
        a = n;
        a[i] = (int)i;
    }
    CHECK(cap >= 1000);
    CHECK(cap < 4000); /* geometric, not wildly over-allocated */
    CHECK_EQ_INT(a[999], 999);

    /* Impossible growth fails, leaving the array and capacity intact. */
    {
        size_t old_cap = cap;
        n = pc_grow_array(a, &cap, SIZE_MAX / 2, sizeof *a);
        CHECK(n == NULL);
        CHECK_EQ_UINT(cap, old_cap);
        CHECK_EQ_INT(a[500], 500);
    }
    /* No growth needed: same pointer back. */
    CHECK(pc_grow_array(a, &cap, 10, sizeof *a) == a);
    CHECK(pc_grow_array(a, &cap, 10, 0) == NULL);
    free(a);
}

static void test_strndup(void)
{
    char *s = pc_strndup("hello world", 5);
    CHECK_EQ_STR(s, "hello");
    free(s);
    s = pc_strndup("", 0);
    CHECK_EQ_STR(s, "");
    free(s);
    CHECK(pc_strndup("x", SIZE_MAX) == NULL);
}

int main(int argc, char **argv)
{
    RUN(test_malloc_cap_and_zero);
    RUN(test_array_overflow);
    RUN(test_grow_array);
    RUN(test_strndup);
    return pc_t_finish();
}
