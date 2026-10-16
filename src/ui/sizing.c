/* Image Size / Canvas Size unit maths (port of crates/ui-egui/src/sizing.rs). */
#include "ui/sizing.h"

#include <stdio.h>
#include <string.h>

const char *const PC_UI_SIZING_UNITS[7][2] = {
    {"px", "Pixels"},  {"percent", "Percent"}, {"in", "Inches"}, {"cm", "Centimeters"},
    {"mm", "Millimeters"}, {"pt", "Points"},   {"pica", "Picas"},
};

const char *const PC_UI_SIZING_ANCHORS[9] = {
    "topLeft", "top", "topRight", "left", "center", "right", "bottomLeft", "bottom", "bottomRight",
};

bool pc_ui_is_sizing(const char *command)
{
    return command && (strcmp(command, "image.imageSize") == 0 || strcmp(command, "image.canvasSize") == 0);
}

static double maxd(double a, double b) { return a > b ? a : b; }

double pc_ui_to_unit(double px, const char *unit, double orig, double res)
{
    if (strcmp(unit, "percent") == 0) { return px / maxd(orig, 1.0) * 100.0; }
    if (strcmp(unit, "in") == 0) { return px / maxd(res, 1.0); }
    if (strcmp(unit, "cm") == 0) { return px / maxd(res, 1.0) * 2.54; }
    if (strcmp(unit, "mm") == 0) { return px / maxd(res, 1.0) * 25.4; }
    if (strcmp(unit, "pt") == 0) { return px / maxd(res, 1.0) * 72.0; }
    if (strcmp(unit, "pica") == 0) { return px / maxd(res, 1.0) * 6.0; }
    return px;
}

double pc_ui_from_unit(double v, const char *unit, double orig, double res)
{
    if (strcmp(unit, "percent") == 0) { return v * orig / 100.0; }
    if (strcmp(unit, "in") == 0) { return v * res; }
    if (strcmp(unit, "cm") == 0) { return v / 2.54 * res; }
    if (strcmp(unit, "mm") == 0) { return v / 25.4 * res; }
    if (strcmp(unit, "pt") == 0) { return v / 72.0 * res; }
    if (strcmp(unit, "pica") == 0) { return v / 6.0 * res; }
    return v;
}

void pc_ui_human_bytes(double b, char out[16])
{
    const double gib = 1024.0 * 1024.0 * 1024.0;
    const double mib = 1024.0 * 1024.0;
    if (b >= gib) {
        (void)snprintf(out, 16, "%.2fG", b / gib);
    } else if (b >= mib) {
        (void)snprintf(out, 16, "%.1fM", b / mib);
    } else {
        (void)snprintf(out, 16, "%.0fK", maxd(b / 1024.0, 1.0));
    }
}
