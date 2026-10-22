#include "ui/level_auto.h"

#include <math.h>

void pc_ui_auto_levels(const uint32_t hist[256], double *lo, double *hi)
{
    size_t low = 0, high = 255;
    if (hist) {
        uint64_t total = 0;
        for (size_t i = 0; i < 256; i++)
            total += hist[i];
        uint64_t clip = total / 1000;
        uint64_t acc = 0;
        while (low < 253 && acc + hist[low] <= clip)
            acc += hist[low++];
        acc = 0;
        while (high > low + 2 && acc + hist[high] <= clip)
            acc += hist[high--];
    }
    if (lo)
        *lo = (double)low;
    if (hi)
        *hi = (double)high;
}

double pc_ui_levels_gamma_pos(double black, double white, double gamma)
{
    if (!isfinite(black) || !isfinite(white) || !isfinite(gamma))
        return 0;
    return black + (white - black) * pow(0.5, gamma);
}

double pc_ui_levels_gamma_from_pos(double black, double white, double x)
{
    double span = white - black;
    if (!isfinite(span) || span < 1)
        span = 1;
    double t = (x - black) / span;
    if (!isfinite(t))
        t = 0.5;
    if (t < 0.01)
        t = 0.01;
    else if (t > 0.99)
        t = 0.99;
    double g = log(t) / log(0.5);
    if (!isfinite(g) || g < 0.01)
        g = 0.01;
    else if (g > 9.99)
        g = 9.99;
    return g;
}

size_t pc_ui_slider_nearest(const double *xs, size_t n, double x)
{
    if (!xs || n == 0)
        return SIZE_MAX;
    size_t best = 0;
    double best_d = fabs(xs[0] - x);
    for (size_t i = 1; i < n; i++) {
        double d = fabs(xs[i] - x);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}
