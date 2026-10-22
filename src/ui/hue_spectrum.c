#include "ui/hue_spectrum.h"

#include <math.h>

void pc_ui_hue_color(double deg, float rgb[3])
{
    if (!rgb)
        return;
    if (!isfinite(deg))
        deg = 0;
    double h = fmod(deg, 360.0);
    if (h < 0)
        h += 360.0;
    double sector = h / 60.0;
    int i = (int)floor(sector);
    double f = sector - (double)i;
    double q = 1.0 - f, t = f; /* s = v = 1 */
    double r, g, b;
    switch (i % 6) {
    case 0:
        r = 1;
        g = t;
        b = 0;
        break;
    case 1:
        r = q;
        g = 1;
        b = 0;
        break;
    case 2:
        r = 0;
        g = 1;
        b = t;
        break;
    case 3:
        r = 0;
        g = q;
        b = 1;
        break;
    case 4:
        r = t;
        g = 0;
        b = 1;
        break;
    default:
        r = 1;
        g = 0;
        b = q;
        break;
    }
    rgb[0] = (float)r;
    rgb[1] = (float)g;
    rgb[2] = (float)b;
}

double pc_ui_hue_colorize_saturation(double current)
{
    if (!isfinite(current) || current < 25.0)
        return 25.0;
    return current;
}

void pc_ui_hue_spectrum_fill(uint8_t *rgba, size_t n)
{
    if (!rgba || n == 0)
        return;
    for (size_t i = 0; i < n; i++) {
        float rgb[3];
        pc_ui_hue_color((double)i / (double)n * 360.0, rgb);
        for (size_t c = 0; c < 3; c++) {
            float v = rgb[c] * 255.0f + 0.5f;
            rgba[i * 4 + c] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        rgba[i * 4 + 3] = 255;
    }
}
