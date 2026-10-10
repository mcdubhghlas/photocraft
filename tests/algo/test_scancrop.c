/* scancrop.rs: rotated photos on a flatbed scan. */
#include <math.h>
#include <stdlib.h>

#include "algo/scancrop.h"
#include "analysis_gen.h"
#include "harness.h"

static void test_finds_rotated_photos_in_reading_order(void)
{
    const pc_algo_found_photo truth[3] = {{{150.0, 120.0}, 180.0, 120.0, 7.0, 0.0},
                                          {{450.0, 130.0}, 160.0, 130.0, -12.0, 0.0},
                                          {{300.0, 330.0}, 220.0, 110.0, 3.0, 0.0}};
    size_t w = 600, h = 440, n = 0, i;
    float (*px)[4] = gen_scan(w, h, truth, 3);
    pc_algo_found_photo *f = NULL;
    double c[4][2];
    CHECK(pc_algo_scancrop_find_photos(w, h, (const float (*)[4])px, w * h, &f, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 3);
    for (i = 0; i < n && i < 3; i++) {
        CHECK(fabs(f[i].angle - truth[i].angle) < 1.0);
        CHECK(fabs(f[i].center[0] - truth[i].center[0]) < 3.0 &&
              fabs(f[i].center[1] - truth[i].center[1]) < 3.0);
        CHECK(fabs(f[i].width - truth[i].width) < 6.0 && fabs(f[i].height - truth[i].height) < 6.0);
    }
    /* Corners are consistent with the centre. */
    if (n > 0) {
        pc_algo_found_photo_corners(&f[0], c);
        CHECK(fabs((c[0][0] + c[2][0]) / 2.0 - f[0].center[0]) < 1e-9);
    }
    free(f);
    for (i = 0; i < w * h; i++) {
        px[i][0] = px[i][1] = px[i][2] = px[i][3] = 1.0f;
    }
    CHECK(pc_algo_scancrop_find_photos(w, h, (const float (*)[4])px, w * h, &f, &n, NULL) == PC_OK);
    CHECK(n == 0 && f == NULL);
    free(px);
}

static void test_never_crash(void)
{
    /* Tiny, short, NaN, extreme aspect and noise scans. */
    size_t n = 5, i;
    pc_algo_found_photo *f = NULL;
    float (*px)[4] = malloc(8 * 9000 * sizeof *px);
    for (i = 0; i < 8 * 9000; i++) {
        px[i][0] = (i % 3 == 0) ? NAN : (float)(i * 2654435761u % 1000) / 999.0f;
        px[i][1] = (i % 5 == 0) ? INFINITY : 0.5f;
        px[i][2] = -1.0f;
        px[i][3] = 1.0f;
    }
    CHECK(pc_algo_scancrop_find_photos(7, 100, (const float (*)[4])px, 700, &f, &n, NULL) == PC_OK && n == 0);
    CHECK(pc_algo_scancrop_find_photos(100, 100, (const float (*)[4])px, 99, &f, &n, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_scancrop_find_photos(8, 9000, (const float (*)[4])px, 8 * 9000, &f, &n, NULL) == PC_OK);
    free(f);
    CHECK(pc_algo_scancrop_find_photos(9000, 8, (const float (*)[4])px, 8 * 9000, &f, &n, NULL) == PC_OK);
    free(f);
    CHECK(pc_algo_scancrop_find_photos(SIZE_MAX, 8, (const float (*)[4])px, 8 * 9000, &f, &n, NULL) ==
          PC_ERR_INVALID);
    for (i = 0; i < 8 * 9000; i++) {
        px[i][0] = (float)((i * 7919u) % 101) / 100.0f;
        px[i][1] = (float)((i * 104729u) % 97) / 96.0f;
        px[i][2] = 0.5f;
    }
    CHECK(pc_algo_scancrop_find_photos(240, 300, (const float (*)[4])px, 8 * 9000, &f, &n, NULL) == PC_OK);
    free(f);
    free(px);
}

int main(int argc, char **argv)
{
    RUN(test_finds_rotated_photos_in_reading_order);
    RUN(test_never_crash);
    return pc_t_finish();
}
