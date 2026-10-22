#include "harness.h"
#include "ui/channel_mixer.h"
#include <math.h>

static void test_total_and_overload(void)
{
    CHECK_NEAR(pc_ui_channel_mixer_total(100, 0, 0), 100, 0.001);
    CHECK_NEAR(pc_ui_channel_mixer_total(40, 40, 20), 100, 0.001);
    CHECK_NEAR(pc_ui_channel_mixer_total(60, 60, 40), 160, 0.001);
    CHECK_NEAR(pc_ui_channel_mixer_total(-50, 30, 10), -10, 0.001);
    /* NaN components are treated as 0. */
    CHECK_NEAR(pc_ui_channel_mixer_total(NAN, 50, 50), 100, 0.001);

    CHECK(!pc_ui_channel_mixer_overload(100));
    CHECK(pc_ui_channel_mixer_overload(100.5));
    CHECK(pc_ui_channel_mixer_overload(160));
    CHECK(!pc_ui_channel_mixer_overload(-10));
    CHECK(!pc_ui_channel_mixer_overload(NAN));
}

int main(int argc, char **argv)
{
    RUN(test_total_and_overload);
    return pc_t_finish();
}
