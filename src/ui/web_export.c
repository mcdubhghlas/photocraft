#include "ui/web_export.h"

#include <math.h>
#include <string.h>

const char *const PC_UI_WEB_FORMAT_IDS[PC_UI_WEB_NFORMATS] = {"gif", "png8", "png24", "jpeg", "wbmp"};
const char *const PC_UI_WEB_FORMAT_LABELS[PC_UI_WEB_NFORMATS] = {"GIF", "PNG-8", "PNG-24", "JPEG",
                                                                "WBMP"};

int pc_ui_web_format_index(const char *id)
{
    if (!id) {
        return -1;
    }
    for (int i = 0; i < PC_UI_WEB_NFORMATS; i++) {
        if (!strcmp(id, PC_UI_WEB_FORMAT_IDS[i])) {
            return i;
        }
    }
    return -1;
}

void pc_ui_web_variant(const char *fmt, double quality, double colors, double k, char *out_fmt,
                       size_t cap, double *out_quality, double *out_colors)
{
    double q = quality, c = colors;
    const char *f = fmt ? fmt : "jpeg";
    if (!isfinite(k) || k <= 0) {
        k = 1;
    }
    if (!strcmp(f, "jpeg")) {
        q = fmax(1.0, round((isfinite(quality) ? quality : 60.0) / k));
    } else if (!strcmp(f, "gif") || !strcmp(f, "png8")) {
        c = fmax(2.0, round((isfinite(colors) ? colors : 128.0) / k));
    } else {
        f = (k == 2.0) ? "png8" : "gif";
        c = floor(256.0 / k);
        if (!isfinite(c) || c < 2.0) {
            c = 2.0;
        }
    }
    if (out_fmt && cap > 0) {
        size_t n = strlen(f);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out_fmt, f, n);
        out_fmt[n] = '\0';
    }
    if (out_quality) {
        *out_quality = q;
    }
    if (out_colors) {
        *out_colors = c;
    }
}

double pc_ui_web_download_seconds(double bytes)
{
    if (!isfinite(bytes) || bytes <= 0) {
        return 0;
    }
    return ceil(bytes * 8.0 / 56600.0);
}

void pc_ui_web_scaled_size(uint32_t width, uint32_t height, double percent, uint32_t *out_w,
                          uint32_t *out_h)
{
    if (!isfinite(percent) || percent < 1.0 || percent > 1000.0) {
        percent = 100.0;
    }
    double pct = percent / 100.0;
    double w = round((double)width * pct);
    double h = round((double)height * pct);
    if (!isfinite(w) || w < 1.0) {
        w = 1.0;
    }
    if (!isfinite(h) || h < 1.0) {
        h = 1.0;
    }
    if (w > (double)UINT32_MAX) {
        w = (double)UINT32_MAX;
    }
    if (h > (double)UINT32_MAX) {
        h = (double)UINT32_MAX;
    }
    if (out_w) {
        *out_w = (uint32_t)w;
    }
    if (out_h) {
        *out_h = (uint32_t)h;
    }
}

void pc_ui_web_advance_preview(size_t frame, size_t duration, double fps, double last, double now,
                              size_t *out_frame, double *out_last)
{
    if (duration < 1) {
        duration = 1;
    }
    if (frame > duration - 1) {
        frame = duration - 1;
    }
    size_t next = frame;
    double next_last = last;
    if (!isfinite(now)) {
        /* keep (frame, last) */
    } else if (!isfinite(last) || now < last) {
        next_last = now;
    } else {
        double f = (isfinite(fps) && fps > 0.0) ? fps : 30.0;
        double elapsed = (now - last) * f;
        if (!isfinite(elapsed)) {
            next_last = now;
        } else {
            double steps = floor(elapsed);
            if (steps < 1.0) {
                /* keep (frame, last) */
                next_last = last;
            } else {
                double durf = (double)duration;
                double dm = fmod(steps, durf);
                if (!isfinite(dm) || dm < 0.0) {
                    dm = 0.0;
                }
                size_t delta = (dm < 18446744073709551615.0) ? (size_t)dm : 0;
                delta %= duration;
                size_t remaining = duration - frame;
                next = (delta >= remaining) ? (delta - remaining) : (frame + delta);
                next_last = now - (elapsed - steps) / f;
            }
        }
    }
    if (out_frame) {
        *out_frame = next;
    }
    if (out_last) {
        *out_last = next_last;
    }
}
