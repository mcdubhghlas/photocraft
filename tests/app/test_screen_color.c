/* screen_color: pixel budget, monitor geometry match, 16->8 channel packing, RGB-only ICC. */
#include <stdint.h>

#include "app/screen_color.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "harness.h"

static void test_pixel_budget(void)
{
    size_t n = 0;
    pc_error err;
    CHECK_EQ_INT(pc_screen_pixel_budget(100, 100, &n, &err), PC_OK);
    CHECK_EQ_UINT(n, 10000u);
    /* Exactly the 64 MP budget is allowed; one past it is not. */
    CHECK_EQ_INT(pc_screen_pixel_budget(8192, 8192, &n, &err), PC_OK);
    CHECK_EQ_UINT(n, PC_SCREEN_MAX_PIXELS);
    CHECK_EQ_INT(pc_screen_pixel_budget(8193, 8192, &n, &err), PC_ERR_LIMIT);
    /* Zero and overflow are rejected without wrapping. */
    CHECK_EQ_INT(pc_screen_pixel_budget(0, 10, &n, &err), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_screen_pixel_budget(10, 0, &n, &err), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_screen_pixel_budget(SIZE_MAX, 2, &n, &err), PC_ERR_LIMIT);
}

static void test_find_monitor(void)
{
    pc_screen_placement p[] = {
        {2, -3840, 0, 3840, 2160},
        {0, 0, 0, 1920, 1080},
    };
    size_t idx = 99;
    CHECK(pc_screen_find_monitor(p, 2, -3840, 0, 3840, 2160, &idx));
    CHECK_EQ_UINT(idx, 2u);
    CHECK(pc_screen_find_monitor(p, 2, 0, 0, 1920, 1080, &idx));
    CHECK_EQ_UINT(idx, 0u);
    CHECK(!pc_screen_find_monitor(p, 2, 100, 0, 1920, 1080, &idx));
    CHECK(!pc_screen_find_monitor(p, 0, 0, 0, 1920, 1080, &idx));
}

static void test_channel_packing(void)
{
    CHECK_EQ_UINT(pc_screen_chan8(0), 0u);
    CHECK_EQ_UINT(pc_screen_chan8(65535), 255u);
    CHECK_EQ_UINT(pc_screen_chan8(257), 1u);
    CHECK_EQ_UINT(pc_screen_chan8(32767), 127u);

    const uint16_t rgb16[] = {0, 65535, 0, 65535, 0, 32767};
    uint8_t out[8];
    pc_error err;
    CHECK_EQ_INT(pc_screen_pack_rgba(rgb16, 2, out, &err), PC_OK);
    const uint8_t want[] = {0, 255, 0, 255, 255, 0, 127, 255};
    CHECK_EQ_MEM(out, want, sizeof want);
    CHECK_EQ_INT(pc_screen_pack_rgba(NULL, 2, out, &err), PC_ERR_INVALID);
    CHECK_EQ_INT(pc_screen_pack_rgba(rgb16, PC_SCREEN_MAX_PIXELS + 1, out, &err), PC_ERR_LIMIT);
}

static void test_icc_validate(void)
{
    pc_error err;
    pc_buf srgb = PC_BUF_INIT;
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_SRGB), &srgb, &err), PC_OK);
    CHECK_EQ_INT(pc_screen_icc_validate(srgb.data, srgb.len, &err), PC_OK);
    pc_buf_fini(&srgb);

    pc_buf cmyk = PC_BUF_INIT;
    CHECK_EQ_INT(pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), &cmyk, &err), PC_OK);
    CHECK_EQ_INT(pc_screen_icc_validate(cmyk.data, cmyk.len, &err), PC_ERR_UNSUPPORTED);
    pc_buf_fini(&cmyk);

    CHECK_EQ_INT(pc_screen_icc_validate(NULL, 0, &err), PC_ERR_FORMAT);
    static uint8_t huge[1];
    CHECK_EQ_INT(pc_screen_icc_validate(huge, (1024u * 1024u) + 1u, &err), PC_ERR_LIMIT);
}

static void test_pick_availability(void)
{
    pc_error err;
    /* Wayland is deferred (D19) in every build. */
    CHECK_EQ_INT(pc_screen_pick(true, &err), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(err.msg, "Wayland");
    CHECK_CONTAINS(err.msg, "D19");
#ifdef PC_NO_X11SCREEN
    /* Built without libxcb: the X11 path reports it is unavailable, naming the library. */
    CHECK_EQ_INT(pc_screen_pick(false, &err), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(err.msg, "libxcb");
#else
    /* Built with libxcb: available when a TrueColor display is present, else a clean error. */
    pc_status st = pc_screen_pick(false, &err);
    CHECK(st == PC_OK || st == PC_ERR_UNSUPPORTED);
#endif
}

static void test_pick_at(void)
{
    pc_error err;
    uint8_t rgba[4] = {1, 2, 3, 4};
    /* Wayland deferred (D19) in every build. */
    CHECK_EQ_INT(pc_screen_pick_at(true, 0, 0, NULL, 0, rgba, NULL, NULL, &err), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(err.msg, "D19");
    /* A missing output buffer is rejected, never dereferenced. */
    CHECK_EQ_INT(pc_screen_pick_at(false, 0, 0, NULL, 0, NULL, NULL, NULL, &err), PC_ERR_INVALID);
#ifdef PC_NO_X11SCREEN
    CHECK_EQ_INT(pc_screen_pick_at(false, 0, 0, NULL, 0, rgba, NULL, NULL, &err), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(err.msg, "libxcb");
#else
    /* Real path: exercised opportunistically when a display is present, graceful otherwise. */
    pc_buf prof = PC_BUF_INIT;
    size_t mon = 12345;
    pc_status st = pc_screen_pick_at(false, 0, 0, NULL, 0, rgba, &mon, &prof, &err);
    CHECK(st == PC_OK || st == PC_ERR_UNSUPPORTED || st == PC_ERR_INVALID || st == PC_ERR_IO);
    if (st == PC_OK) {
        CHECK_EQ_UINT(rgba[3], 255u); /* always opaque */
    }
    pc_buf_fini(&prof);
    /* A wildly off-screen coordinate is an error (or no display), never a crash. */
    st = pc_screen_pick_at(false, 1 << 30, 1 << 30, NULL, 0, rgba, NULL, NULL, &err);
    CHECK(st != PC_OK);
#endif
}

int main(int argc, char **argv)
{
    RUN(test_pixel_budget);
    RUN(test_find_monitor);
    RUN(test_channel_packing);
    RUN(test_icc_validate);
    RUN(test_pick_availability);
    RUN(test_pick_at);
    return pc_t_finish();
}
