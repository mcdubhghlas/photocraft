/* Unit tests for the Save for Web (Legacy) portable helpers (src/ui/web_export.c): format table,
 * 4-Up variants, download-time estimate, image-size scaling and the preview playback clock. The
 * advance_preview cases are a direct port of crates/ui-egui/src/file_ui.rs's tests. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "ui/web_export.h"

static void test_format_table(void)
{
    CHECK_EQ_INT(pc_ui_web_format_index("gif"), 0);
    CHECK_EQ_INT(pc_ui_web_format_index("png8"), 1);
    CHECK_EQ_INT(pc_ui_web_format_index("png24"), 2);
    CHECK_EQ_INT(pc_ui_web_format_index("jpeg"), 3);
    CHECK_EQ_INT(pc_ui_web_format_index("wbmp"), 4);
    CHECK_EQ_INT(pc_ui_web_format_index("bogus"), -1);
    CHECK_EQ_INT(pc_ui_web_format_index(NULL), -1);
    CHECK(!strcmp(PC_UI_WEB_FORMAT_LABELS[2], "PNG-24"));
}

static void variant(const char *fmt, double q, double c, double k, const char *want_fmt,
                    double want_q, double want_c)
{
    char f[8];
    double vq = q, vc = c;
    pc_ui_web_variant(fmt, q, c, k, f, sizeof f, &vq, &vc);
    CHECK(!strcmp(f, want_fmt));
    CHECK(fabs(vq - want_q) < 1e-9);
    CHECK(fabs(vc - want_c) < 1e-9);
}

static void test_variants(void)
{
    /* JPEG scales quality. */
    variant("jpeg", 60, 128, 2, "jpeg", 30, 128);
    variant("jpeg", 60, 128, 4, "jpeg", 15, 128);
    /* GIF / PNG-8 scale the colour count (floored at 2). */
    variant("gif", 128, 128, 2, "gif", 128, 64);
    variant("png8", 128, 128, 4, "png8", 128, 32);
    variant("gif", 60, 3, 4, "gif", 60, 2);
    /* PNG-24 (and WBMP) fall back to indexed formats. */
    variant("png24", 60, 128, 2, "png8", 60, 128);
    variant("png24", 60, 128, 4, "gif", 60, 64);
    variant("wbmp", 60, 128, 2, "png8", 60, 128);
}

static void test_download_seconds(void)
{
    CHECK(pc_ui_web_download_seconds(56600) == 8.0);
    CHECK(pc_ui_web_download_seconds(0) == 0.0);
    CHECK(pc_ui_web_download_seconds(-5) == 0.0);
    CHECK(pc_ui_web_download_seconds(NAN) == 0.0);
    CHECK(pc_ui_web_download_seconds(7075) == 1.0); /* 7075*8/56600 == 1.0 exactly */
    CHECK(pc_ui_web_download_seconds(7076) == 2.0); /* just over rounds up */
}

static void test_scaled_size(void)
{
    uint32_t w = 0, h = 0;
    pc_ui_web_scaled_size(100, 80, 50, &w, &h);
    CHECK_EQ_UINT(w, 50);
    CHECK_EQ_UINT(h, 40);
    pc_ui_web_scaled_size(100, 80, 100, &w, &h);
    CHECK_EQ_UINT(w, 100);
    CHECK_EQ_UINT(h, 80);
    pc_ui_web_scaled_size(100, 80, 1000, &w, &h);
    CHECK_EQ_UINT(w, 1000);
    CHECK_EQ_UINT(h, 800);
    /* Out-of-range / non-finite percent is treated as 100. */
    pc_ui_web_scaled_size(100, 80, -5, &w, &h);
    CHECK_EQ_UINT(w, 100);
    pc_ui_web_scaled_size(10, 10, 1, &w, &h);
    CHECK_EQ_UINT(w, 1); /* never below 1px */
}

static void advance(size_t frame, size_t duration, double fps, double last, double now,
                    size_t want_frame, double want_last)
{
    size_t of = 999;
    double ol = -1;
    pc_ui_web_advance_preview(frame, duration, fps, last, now, &of, &ol);
    CHECK_EQ_UINT(of, want_frame);
    CHECK(fabs(ol - want_last) < 1e-9);
}

static void test_advance_preview(void)
{
    advance(0, 3, 2.0, 0.0, 0.49, 0, 0.0);
    advance(0, 3, 2.0, 0.0, 0.75, 1, 0.5);
    advance(1, 3, 2.0, 0.5, 1.0, 2, 1.0);
    advance(2, 3, 2.0, 0.0, 1.0, 1, 1.0);
    advance(0, 3, 2.0, 0.0, 6.2, 0, 6.0);
    advance(1, 3, 2.0, 20.0, 20.25, 1, 20.0);
    advance(1, 3, 2.0, 20.0, 20.5, 2, 20.5);
    advance(0, 3, 0.5, 0.0, 2.0, 1, 2.0);
    advance(SIZE_MAX, 0, NAN, 0.0, 1.0, 0, 1.0);
    advance(SIZE_MAX - 2, SIZE_MAX, 2.0, 0.0, 1.0, 0, 1.0);
    advance(2, 3, 2.0, 10.0, 9.0, 2, 9.0); /* time going backwards rewinds the clock only */
    /* Non-finite now keeps the current frame and clock. */
    advance(1, 3, 2.0, 5.0, INFINITY, 1, 5.0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_format_table);
    RUN(test_variants);
    RUN(test_download_seconds);
    RUN(test_scaled_size);
    RUN(test_advance_preview);
    return pc_t_finish();
}
