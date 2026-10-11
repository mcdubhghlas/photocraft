/* Warp Text styles (see warp.h). */
#include "text/warp.h"

#include <string.h>

#include "text/unicode.h"

const char *const PC_TEXT_WARP_STYLES[15][2] = {
    {"warpArc", "arc"},
    {"warpArcLower", "arcLower"},
    {"warpArcUpper", "arcUpper"},
    {"warpArch", "arch"},
    {"warpBulge", "bulge"},
    {"warpShellLower", "shellLower"},
    {"warpShellUpper", "shellUpper"},
    {"warpFlag", "flag"},
    {"warpWave", "wave"},
    {"warpFish", "fish"},
    {"warpRise", "rise"},
    {"warpFisheye", "fisheye"},
    {"warpInflate", "inflate"},
    {"warpSqueeze", "squeeze"},
    {"warpTwist", "twist"},
};

const char *pc_text_warp_psd_style(const char *id, size_t len)
{
    size_t i;
    if (id == NULL) {
        id = "";
        len = 0;
    }
    if (pc_text_eq_ignore_ascii_case(id, len, "none", 4) ||
        pc_text_eq_ignore_ascii_case(id, len, "warpnone", 8)) {
        return "warpNone";
    }
    for (i = 0; i < 15; i++) {
        const char *psd = PC_TEXT_WARP_STYLES[i][0], *sh = PC_TEXT_WARP_STYLES[i][1];
        if (pc_text_eq_ignore_ascii_case(id, len, psd, strlen(psd)) ||
            pc_text_eq_ignore_ascii_case(id, len, sh, strlen(sh))) {
            return psd;
        }
    }
    return NULL;
}

bool pc_text_warp_prepare(const pc_text_warp *w, const float bounds[4], pc_style_warp *out)
{
    pc_warp_style s;
    const char *name;
    double b[4];
    if (!pc_warp_style_parse(pc_str_c(&w->style), w->style.len, &s) || !pc_warp_style_is_preset(s)) {
        return false;
    }
    name = pc_warp_style_psd_name(s);
    if (strlen(name) != w->style.len || memcmp(name, pc_str_c(&w->style), w->style.len) != 0) {
        return false;
    }
    b[0] = bounds[0];
    b[1] = bounds[1];
    b[2] = bounds[2];
    b[3] = bounds[3];
    return pc_style_warp_init(out, s, (double)w->value, (double)w->horizontal_distortion,
                              (double)w->vertical_distortion, !w->horizontal, b);
}

bool pc_text_layout_warp(const pc_text_layout *l, const pc_text_warp *w, pc_style_warp *out)
{
    float b[4];
    if (w == NULL || !pc_text_layout_bounds(l, b)) {
        return false;
    }
    return pc_text_warp_prepare(w, b, out);
}
