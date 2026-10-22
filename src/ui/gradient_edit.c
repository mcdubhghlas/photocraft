#include "ui/gradient_edit.h"
#include "ui/color_picker.h"
#include <string.h>

static float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}
void pc_ui_gradient_init(pc_ui_gradient *g)
{
    if (!g) {
        return;
    }
    memset(g, 0, sizeof *g);
    g->colors[0] = (pc_ui_grad_cstop){.loc = 0, .rgb = {0, 0, 0}};
    g->colors[1] = (pc_ui_grad_cstop){.loc = 1, .rgb = {1, 1, 1}};
    g->ncolors = 2;
    g->opacity[0] = (pc_ui_grad_ostop){.loc = 0, .opacity = 1};
    g->opacity[1] = (pc_ui_grad_ostop){.loc = 1, .opacity = 1};
    g->nopacity = 2;
}
static void sort_colors(pc_ui_gradient *g)
{
    for (size_t i = 1; i < g->ncolors; i++) {
        pc_ui_grad_cstop key = g->colors[i];
        size_t j = i;
        while (j > 0 && g->colors[j - 1].loc > key.loc) {
            g->colors[j] = g->colors[j - 1];
            j--;
        }
        g->colors[j] = key;
    }
}
static void sort_opacity(pc_ui_gradient *g)
{
    for (size_t i = 1; i < g->nopacity; i++) {
        pc_ui_grad_ostop key = g->opacity[i];
        size_t j = i;
        while (j > 0 && g->opacity[j - 1].loc > key.loc) {
            g->opacity[j] = g->opacity[j - 1];
            j--;
        }
        g->opacity[j] = key;
    }
}
bool pc_ui_gradient_parse(pc_ui_gradient *g, yyjson_mut_val *current, const float fg[3], const float bg[3])
{
    if (!g) {
        return false;
    }
    pc_ui_gradient_init(g);
    if (!current || !yyjson_mut_is_obj(current)) {
        return false;
    }
    float foreground[3] = {0, 0, 0}, background[3] = {1, 1, 1};
    if (fg) {
        memcpy(foreground, fg, sizeof foreground);
    }
    if (bg) {
        memcpy(background, bg, sizeof background);
    }
    yyjson_mut_val *stops = yyjson_mut_obj_get(current, "stops");
    if (yyjson_mut_is_arr(stops)) {
        size_t n = 0;
        size_t i, max;
        yyjson_mut_val *item;
        yyjson_mut_arr_foreach(stops, i, max, item)
        {
            if (n >= PC_UI_GRAD_MAX || !yyjson_mut_is_arr(item) || yyjson_mut_arr_size(item) < 2) {
                continue;
            }
            float loc = clamp01((float)yyjson_mut_get_num(yyjson_mut_arr_get(item, 0)));
            const char *col = yyjson_mut_get_str(yyjson_mut_arr_get(item, 1));
            float rgb[3] = {0, 0, 0};
            if (col && !strcmp(col, "foreground")) {
                memcpy(rgb, foreground, sizeof rgb);
            } else if (col && !strcmp(col, "background")) {
                memcpy(rgb, background, sizeof rgb);
            } else if (col) {
                (void)pc_ui_picker_parse_hex(col, strlen(col), rgb);
            }
            g->colors[n].loc = loc;
            memcpy(g->colors[n].rgb, rgb, sizeof rgb);
            n++;
        }
        if (n >= 2) {
            g->ncolors = n;
            sort_colors(g);
        }
    }
    yyjson_mut_val *tr = yyjson_mut_obj_get(current, "transparency");
    if (yyjson_mut_is_arr(tr) && yyjson_mut_arr_size(tr) >= 2) {
        size_t n = 0;
        size_t i, max;
        yyjson_mut_val *item;
        yyjson_mut_arr_foreach(tr, i, max, item)
        {
            if (n >= PC_UI_GRAD_MAX || !yyjson_mut_is_arr(item) || yyjson_mut_arr_size(item) < 2) {
                continue;
            }
            g->opacity[n].loc = clamp01((float)yyjson_mut_get_num(yyjson_mut_arr_get(item, 0)));
            g->opacity[n].opacity = clamp01((float)(yyjson_mut_get_num(yyjson_mut_arr_get(item, 1)) / 100.0));
            n++;
        }
        if (n >= 2) {
            g->nopacity = n;
            sort_opacity(g);
        }
    }
    return true;
}
static float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}
void pc_ui_gradient_sample(const pc_ui_gradient *g, float t, float out_rgba[4])
{
    if (!g || !out_rgba) {
        return;
    }
    t = clamp01(t);
    /* colour */
    out_rgba[0] = out_rgba[1] = out_rgba[2] = 0;
    if (g->ncolors > 0) {
        size_t hi = 0;
        while (hi < g->ncolors && g->colors[hi].loc < t) {
            hi++;
        }
        if (hi == 0) {
            memcpy(out_rgba, g->colors[0].rgb, sizeof(float[3]));
        } else if (hi >= g->ncolors) {
            memcpy(out_rgba, g->colors[g->ncolors - 1].rgb, sizeof(float[3]));
        } else {
            const pc_ui_grad_cstop *a = &g->colors[hi - 1], *b = &g->colors[hi];
            float span = b->loc - a->loc;
            float u = span > 1e-6f ? (t - a->loc) / span : 0;
            for (int c = 0; c < 3; c++) {
                out_rgba[c] = lerp(a->rgb[c], b->rgb[c], u);
            }
        }
    }
    /* opacity */
    out_rgba[3] = 1;
    if (g->nopacity > 0) {
        size_t hi = 0;
        while (hi < g->nopacity && g->opacity[hi].loc < t) {
            hi++;
        }
        if (hi == 0) {
            out_rgba[3] = g->opacity[0].opacity;
        } else if (hi >= g->nopacity) {
            out_rgba[3] = g->opacity[g->nopacity - 1].opacity;
        } else {
            const pc_ui_grad_ostop *a = &g->opacity[hi - 1], *b = &g->opacity[hi];
            float span = b->loc - a->loc;
            float u = span > 1e-6f ? (t - a->loc) / span : 0;
            out_rgba[3] = lerp(a->opacity, b->opacity, u);
        }
    }
}
int pc_ui_gradient_nearest_color(const pc_ui_gradient *g, float loc, float tol)
{
    if (!g) {
        return -1;
    }
    int best = -1;
    float bd = tol;
    for (size_t i = 0; i < g->ncolors; i++) {
        float d = g->colors[i].loc - loc;
        d = d < 0 ? -d : d;
        if (d <= bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}
int pc_ui_gradient_nearest_opacity(const pc_ui_gradient *g, float loc, float tol)
{
    if (!g) {
        return -1;
    }
    int best = -1;
    float bd = tol;
    for (size_t i = 0; i < g->nopacity; i++) {
        float d = g->opacity[i].loc - loc;
        d = d < 0 ? -d : d;
        if (d <= bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}
