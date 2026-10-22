#include "ui/channel_mixer.h"

#include <math.h>

double pc_ui_channel_mixer_total(double red, double green, double blue)
{
    if (!isfinite(red))
        red = 0;
    if (!isfinite(green))
        green = 0;
    if (!isfinite(blue))
        blue = 0;
    return red + green + blue;
}

bool pc_ui_channel_mixer_overload(double total)
{
    return isfinite(total) && total > 100.0;
}
