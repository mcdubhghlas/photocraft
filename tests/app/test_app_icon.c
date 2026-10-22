/* app_icon: the embedded window icon decodes to a non-blank RGBA image. */
#include <stdbool.h>
#include <stdlib.h>

#include "app/app_icon.h"
#include "harness.h"

static void test_window_icon_decodes(void)
{
    uint8_t *rgba = NULL;
    uint32_t w = 0, h = 0;
    pc_error err;
    pc_error_clear(&err);
    pc_status rc = pc_app_window_icon_rgba(&rgba, &w, &h, &err);
    CHECK_EQ_INT(rc, PC_OK);
    if (rc != PC_OK) {
        return;
    }
    CHECK(w >= 64 && w == h);
    bool any_opaque = false;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        if (rgba[i * 4 + 3] > 0) {
            any_opaque = true;
            break;
        }
    }
    CHECK(any_opaque);
    free(rgba);
}

int main(int argc, char **argv)
{
    RUN(test_window_icon_decodes);
    return pc_t_finish();
}
