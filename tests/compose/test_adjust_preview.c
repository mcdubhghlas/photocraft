/* pc_compose_adjust_apply_rgba8: apply an adjustment to a small caller RGBA8 buffer for UI
 * previews (colour-tracked sliders, adjusted spectrum bars). Behavioural and graceful-failure. */
#include <stdint.h>
#include <string.h>

#include "compose/adjust.h"
#include "doc/adjust.h"
#include "harness.h"

static pc_adjustment adj_of(pc_adjustment_kind k)
{
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = k;
    return a;
}

static void test_invert_inverts_rgb_keeps_alpha(void)
{
    /* Two pixels: a colour and black, with distinct alphas. */
    uint8_t px[8] = {10, 20, 200, 128, 0, 0, 0, 255};
    pc_adjustment inv = adj_of(PC_ADJ_INVERT);
    CHECK(pc_compose_adjust_apply_rgba8(&inv, px, 2, 1, NULL) == PC_OK);
    CHECK_EQ_UINT(px[0], 245); /* 255 - 10 */
    CHECK_EQ_UINT(px[1], 235);
    CHECK_EQ_UINT(px[2], 55);
    CHECK_EQ_UINT(px[3], 128); /* alpha untouched */
    CHECK_EQ_UINT(px[4], 255);
    CHECK_EQ_UINT(px[5], 255);
    CHECK_EQ_UINT(px[6], 255);
    CHECK_EQ_UINT(px[7], 255);
}

static void test_unsupported_is_identity(void)
{
    uint8_t px[4] = {12, 34, 56, 78};
    uint8_t before[4];
    pc_adjustment u = adj_of(PC_ADJ_UNSUPPORTED);
    memcpy(before, px, sizeof px);
    CHECK(pc_compose_adjust_apply_rgba8(&u, px, 1, 1, NULL) == PC_OK);
    CHECK(memcmp(px, before, sizeof px) == 0);
}

static void test_graceful_failures(void)
{
    uint8_t px[4] = {1, 2, 3, 4};
    pc_adjustment inv = adj_of(PC_ADJ_INVERT);
    pc_error e = {0};
    /* Zero size is a no-op, not an error. */
    CHECK(pc_compose_adjust_apply_rgba8(&inv, px, 0, 4, NULL) == PC_OK);
    CHECK(pc_compose_adjust_apply_rgba8(&inv, px, 4, 0, NULL) == PC_OK);
    CHECK_EQ_UINT(px[0], 1); /* unchanged */
    /* NULL adjustment and NULL buffer with a non-empty size are rejected. */
    CHECK(pc_compose_adjust_apply_rgba8(NULL, px, 1, 1, &e) == PC_ERR_INVALID);
    CHECK(pc_compose_adjust_apply_rgba8(&inv, NULL, 1, 1, &e) == PC_ERR_INVALID);
    /* A size past the bound is refused before allocating. */
    CHECK(pc_compose_adjust_apply_rgba8(&inv, px, 65536, 65536, &e) == PC_ERR_LIMIT);
}

int main(int argc, char **argv)
{
    RUN(test_invert_inverts_rgb_keeps_alpha);
    RUN(test_unsupported_is_identity);
    RUN(test_graceful_failures);
    return pc_t_finish();
}
