#include "ui/print_layout.h"

#include <math.h>
#include <strings.h>
#include <string.h>

const char *const PC_UI_PRINT_PAPER_NAMES[PC_UI_PRINT_NPAPERS] = {"letter", "legal", "tabloid", "a3",
                                                                 "a4",     "a5",    "4x6",     "5x7"};
const double PC_UI_PRINT_PAPER_W[PC_UI_PRINT_NPAPERS] = {612.0,  612.0,  792.0,  841.89,
                                                        595.28, 419.53, 288.0,  360.0};
const double PC_UI_PRINT_PAPER_H[PC_UI_PRINT_NPAPERS] = {792.0,   1008.0,  1224.0, 1190.55,
                                                        841.89,  595.28,  432.0,  504.0};

double pc_ui_print_unit_pt(const char *units)
{
    if (units && !strcmp(units, "mm")) {
        return 72.0 / 25.4;
    }
    if (units && !strcmp(units, "in")) {
        return 72.0;
    }
    return 72.0 / 2.54;
}

bool pc_ui_print_paper_size(const char *name, double *w, double *h)
{
    double pw = 612.0, ph = 792.0;
    bool found = false;
    if (name) {
        for (int i = 0; i < PC_UI_PRINT_NPAPERS; i++) {
            if (!strcasecmp(name, PC_UI_PRINT_PAPER_NAMES[i])) {
                pw = PC_UI_PRINT_PAPER_W[i];
                ph = PC_UI_PRINT_PAPER_H[i];
                found = true;
                break;
            }
        }
    }
    if (w) {
        *w = pw;
    }
    if (h) {
        *h = ph;
    }
    return found;
}

void pc_ui_print_printer_label(const char *name, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    size_t n = 0;
    if (name) {
        for (; name[n] && n + 1 < cap; n++) {
            out[n] = (name[n] == '_') ? ' ' : name[n];
        }
    }
    out[n] = '\0';
}

bool pc_ui_print_compute_layout(double paper_w, double paper_h, bool landscape, uint32_t doc_w,
                                uint32_t doc_h, double dpi, bool marks, bool scale_to_fit,
                                double scale_pct, bool center, double top_in, double left_in,
                                pc_ui_print_layout *out)
{
    if (!out) {
        return false;
    }
    if (!isfinite(paper_w) || !isfinite(paper_h) || paper_w <= 0 || paper_h <= 0) {
        paper_w = 612.0;
        paper_h = 792.0;
    }
    bool valid = paper_w > 36.0 && paper_h > 36.0 && doc_w > 0 && doc_h > 0;
    double pw, ph;
    if (landscape) {
        pw = paper_h > paper_w ? paper_h : paper_w;
        ph = paper_w < paper_h ? paper_w : paper_h;
    } else {
        pw = paper_w;
        ph = paper_h;
    }
    if (!isfinite(dpi) || dpi < 1.0) {
        dpi = 1.0;
    }
    double iw = (double)(doc_w ? doc_w : 1) * 72.0 / dpi;
    double ih = (double)(doc_h ? doc_h : 1) * 72.0 / dpi;
    double margin = marks ? 36.0 : 18.0;
    double scale;
    if (scale_to_fit) {
        double sx = (pw - 2.0 * margin) / iw;
        double sy = (ph - 2.0 * margin) / ih;
        scale = sx < sy ? sx : sy;
    } else {
        double pct = (isfinite(scale_pct) && scale_pct > 0.0) ? scale_pct : 100.0;
        scale = pct / 100.0;
    }
    if (!isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
        valid = false;
    }
    double w = iw * scale, h = ih * scale;
    double x, y;
    if (center) {
        x = (pw - w) / 2.0;
        y = (ph - h) / 2.0;
    } else {
        double top = (isfinite(top_in) ? top_in : 0.0) * 72.0;
        double left = (isfinite(left_in) ? left_in : 0.0) * 72.0;
        x = left;
        y = ph - top - h;
    }
    out->pw = pw;
    out->ph = ph;
    out->x = x;
    out->y = y;
    out->w = w;
    out->h = h;
    out->scale = scale;
    return valid;
}
