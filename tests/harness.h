/* PhotoCraft's test harness: one header, no dependencies.
 *
 * Each tests/<module>/test_<name>.c is one executable (one translation unit) that includes this
 * header and ends with
 *
 *     int main(int argc, char **argv)
 *     {
 *         RUN(test_something);
 *         RUN(test_other);
 *         return pc_t_finish();
 *     }
 *
 * A failed CHECK reports file:line and the values, and the test keeps going. Exit status is 0
 * only if every check passed. `./test_x <substring>` runs only tests whose name contains it.
 * Pick the CHECK_EQ_* variant matching the operands' signedness (the build uses -Wconversion). */
#ifndef PC_TESTS_HARNESS_H
#define PC_TESTS_HARNESS_H

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int pc_t_failed_checks;
static int pc_t_checks;
static int pc_t_tests;
static int pc_t_failed_tests;
static const char *pc_t_name = "";

static inline void pc_t_report(const char *file, int line, const char *what)
{
    pc_t_failed_checks++;
    (void)fprintf(stderr, "%s:%d: FAIL in %s: %s\n", file, line, pc_t_name, what);
}

#define PC_T_FAIL(...)                                                                            \
    do {                                                                                          \
        char pc_t_msg_[512];                                                                      \
        if (snprintf(pc_t_msg_, sizeof pc_t_msg_, __VA_ARGS__) < 0) { /* truncation is fine */    \
            pc_t_msg_[0] = '\0';                                                                 \
        }                                                                                         \
        pc_t_report(__FILE__, __LINE__, pc_t_msg_);                                               \
    } while (0)

#define CHECK(cond)                                                                               \
    do {                                                                                          \
        pc_t_checks++;                                                                            \
        if (!(cond)) {                                                                            \
            PC_T_FAIL("%s", #cond);                                                               \
        }                                                                                         \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                        \
    do {                                                                                          \
        intmax_t pc_t_a_ = (a), pc_t_b_ = (b);                                                    \
        pc_t_checks++;                                                                            \
        if (pc_t_a_ != pc_t_b_) {                                                                 \
            PC_T_FAIL("%s == %s (%" PRIdMAX " vs %" PRIdMAX ")", #a, #b, pc_t_a_, pc_t_b_);       \
        }                                                                                         \
    } while (0)

#define CHECK_EQ_UINT(a, b)                                                                       \
    do {                                                                                          \
        uintmax_t pc_t_a_ = (a), pc_t_b_ = (b);                                                   \
        pc_t_checks++;                                                                            \
        if (pc_t_a_ != pc_t_b_) {                                                                 \
            PC_T_FAIL("%s == %s (%" PRIuMAX " vs %" PRIuMAX ")", #a, #b, pc_t_a_, pc_t_b_);       \
        }                                                                                         \
    } while (0)

/* |a - b| <= eps, and NaN only equals NaN. */
#define CHECK_NEAR(a, b, eps)                                                                     \
    do {                                                                                          \
        double pc_t_a_ = (a), pc_t_b_ = (b);                                                      \
        int pc_t_ok_ = (isnan(pc_t_a_) && isnan(pc_t_b_)) || fabs(pc_t_a_ - pc_t_b_) <= (eps);    \
        pc_t_checks++;                                                                            \
        if (!pc_t_ok_) {                                                                          \
            PC_T_FAIL("%s ~= %s (%.17g vs %.17g, eps %g)", #a, #b, pc_t_a_, pc_t_b_, (double)(eps)); \
        }                                                                                         \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                                        \
    do {                                                                                          \
        const char *pc_t_a_ = (a), *pc_t_b_ = (b);                                                \
        pc_t_checks++;                                                                            \
        if (pc_t_a_ == NULL || pc_t_b_ == NULL || strcmp(pc_t_a_, pc_t_b_) != 0) {                \
            PC_T_FAIL("%s == %s (\"%s\" vs \"%s\")", #a, #b, pc_t_a_ ? pc_t_a_ : "(null)",       \
                      pc_t_b_ ? pc_t_b_ : "(null)");                                              \
        }                                                                                         \
    } while (0)

#define CHECK_EQ_MEM(a, b, n)                                                                     \
    do {                                                                                          \
        pc_t_checks++;                                                                            \
        if (memcmp((a), (b), (n)) != 0) {                                                         \
            PC_T_FAIL("%s == %s (%zu bytes differ)", #a, #b, (size_t)(n));                        \
        }                                                                                         \
    } while (0)

/* Substring check, for error messages. */
#define CHECK_CONTAINS(hay, needle)                                                               \
    do {                                                                                          \
        const char *pc_t_h_ = (hay), *pc_t_n_ = (needle);                                         \
        pc_t_checks++;                                                                            \
        if (pc_t_h_ == NULL || strstr(pc_t_h_, pc_t_n_) == NULL) {                                \
            PC_T_FAIL("\"%s\" contains \"%s\"", pc_t_h_ ? pc_t_h_ : "(null)", pc_t_n_);           \
        }                                                                                         \
    } while (0)

static inline void pc_t_run(const char *name, void (*fn)(void), int argc, char **argv)
{
    int before = pc_t_failed_checks;
    if (argc > 1 && strstr(name, argv[1]) == NULL) {
        return;
    }
    pc_t_name = name;
    pc_t_tests++;
    fn();
    if (pc_t_failed_checks != before) {
        pc_t_failed_tests++;
        (void)fprintf(stderr, "  FAILED  %s\n", name);
    }
}

#define RUN(fn) pc_t_run(#fn, fn, argc, argv)

static inline int pc_t_finish(void)
{
    (void)printf("%d tests, %d checks, %d failed tests\n", pc_t_tests, pc_t_checks, pc_t_failed_tests);
    return pc_t_failed_tests == 0 ? 0 : 1;
}

#endif
