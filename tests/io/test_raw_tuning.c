/* Raw re-develop tone params + public tone-curve extractor (gap B10): the new pc_io_raw_tuning
 * fields and pc_io_raw_tone_curve. Behavioural develop needs a real raw (corpus); here we cover the
 * extractor's graceful behaviour on input that is not a decodable raw, and that the tuning struct
 * carries the five tone sliders. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "harness.h"
#include "io/raw_import.h"

static void test_tone_curve_on_non_raw_is_empty(void)
{
    const uint8_t junk[] = "not a camera raw file at all";
    float out[64][2];
    size_t n = 123;
    /* A file that does not decode yields no curve, not an error (reference's empty Vec). */
    CHECK(pc_io_raw_tone_curve(junk, sizeof junk, out, 64, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
    /* With no output buffer, *n reports how many knots there are (still 0). */
    n = 123;
    CHECK(pc_io_raw_tone_curve(junk, sizeof junk, NULL, 0, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
    /* Empty input is handled too. */
    CHECK(pc_io_raw_tone_curve(junk, 0, out, 64, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 0);
    /* A NULL count is allowed. */
    CHECK(pc_io_raw_tone_curve(junk, sizeof junk, out, 64, NULL, NULL) == PC_OK);
}

static void test_tuning_has_tone_fields(void)
{
    /* The tuning struct now carries the raw-stage tone sliders. */
    pc_io_raw_tuning t = {0};
    t.temperature = 10.0f;
    t.tint = -5.0f;
    t.exposure = 0.5f;
    t.shadows = 20.0f;
    t.highlights = -30.0f;
    t.clarity = 15.0f;
    t.texture = 25.0f;
    t.dehaze = 40.0f;
    CHECK(t.shadows == 20.0f);
    CHECK(t.highlights == -30.0f);
    CHECK(t.clarity == 15.0f);
    CHECK(t.texture == 25.0f);
    CHECK(t.dehaze == 40.0f);

    /* Developing a non-raw with tuning fails gracefully (no crash), not a success. */
    pc_io_import_result r;
    bool applied = false;
    pc_error e = {0};
    const uint8_t junk[] = "still not a raw";
    pc_status st = pc_io_import_raw_tuned("x.cr2", junk, sizeof junk, &t, &r, &applied, &e);
    CHECK(st != PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_tone_curve_on_non_raw_is_empty);
    RUN(test_tuning_has_tone_fields);
    return pc_t_finish();
}
