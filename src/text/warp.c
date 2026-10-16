/* Warp Text styles (see warp.h). */
#include "text/warp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
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

const char *pc_text_warp_short_style(const char *psd, size_t len)
{
    size_t i;
    if (psd == NULL) {
        return NULL;
    }
    if (len == 8 && memcmp(psd, "warpNone", 8) == 0) {
        return "none";
    }
    for (i = 0; i < 15; i++) {
        const char *p = PC_TEXT_WARP_STYLES[i][0];
        if (strlen(p) == len && memcmp(p, psd, len) == 0) {
            return PC_TEXT_WARP_STYLES[i][1];
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

/* ------------------------------------------------------------------------------------------ */
/* Type on a path (warp.rs PathFrame + Warp::Path). */

/* Most cubic segments of a type path that are followed (a hostile file cannot make this large). */
#define PC_MAX_PATH_SEGMENTS 4096

void pc_text_deform_fini(pc_text_deform *w)
{
    if (w != NULL && w->kind == PC_DEFORM_PATH) {
        free(w->path.pts);
        free(w->path.cum);
        w->path.pts = NULL;
        w->path.cum = NULL;
        w->path.n = 0;
    }
}

/* Builds the flattened frame from a text-space subpath. False (nothing allocated) for a path
 * with a non-finite point, fewer than two points, or no length. Mirrors PathFrame::new. */
static bool path_frame_new(const pc_subpath *path, float start, bool flip, pc_path_frame *out)
{
    pc_point *pts = NULL;
    double *cum = NULL;
    size_t cap = 0, n = 0, nseg = pc_subpath_segment_count(path), si, w, i;
    double acc = 0.0;
    if (nseg > PC_MAX_PATH_SEGMENTS) {
        nseg = PC_MAX_PATH_SEGMENTS;
    }
    for (si = 0; si < nseg; si++) {
        pc_point p[4];
        double poly;
        size_t steps;
        pc_subpath_segment(path, si, p);
        for (i = 0; i < 4; i++) {
            if (!isfinite(p[i].x) || !isfinite(p[i].y)) {
                free(pts);
                return false;
            }
        }
        poly = hypot(p[1].x - p[0].x, p[1].y - p[0].y) + hypot(p[2].x - p[1].x, p[2].y - p[1].y) +
               hypot(p[3].x - p[2].x, p[3].y - p[2].y);
        steps = pc_f64_to_size(ceil(poly / 2.0));
        if (steps < 4) {
            steps = 4;
        } else if (steps > 128) {
            steps = 128;
        }
        if (n == 0) {
            pc_point *g = pc_grow_array(pts, &cap, n + 1, sizeof *g);
            if (g == NULL) {
                free(pts);
                return false;
            }
            pts = g;
            pts[n++] = p[0];
        }
        for (i = 1; i <= steps; i++) {
            double t = (double)i / (double)steps, u = 1.0 - t;
            double a = u * u * u, b = 3.0 * u * u * t, c = 3.0 * u * t * t, d = t * t * t;
            pc_point q;
            pc_point *g = pc_grow_array(pts, &cap, n + 1, sizeof *g);
            if (g == NULL) {
                free(pts);
                return false;
            }
            pts = g;
            q.x = a * p[0].x + b * p[1].x + c * p[2].x + d * p[3].x;
            q.y = a * p[0].y + b * p[1].y + c * p[2].y + d * p[3].y;
            pts[n++] = q;
        }
    }
    /* Drop repeated points so every piece has a direction (Vec::dedup_by keeps the first). */
    w = 0;
    for (i = 0; i < n; i++) {
        if (w == 0 || fabs(pts[i].x - pts[w - 1].x) >= 1e-9 || fabs(pts[i].y - pts[w - 1].y) >= 1e-9) {
            pts[w++] = pts[i];
        }
    }
    n = w;
    if (flip) {
        for (i = 0; i < n / 2; i++) {
            pc_point tmp = pts[i];
            pts[i] = pts[n - 1 - i];
            pts[n - 1 - i] = tmp;
        }
    }
    cum = pc_malloc_array(n > 0 ? n : 1, sizeof *cum);
    if (cum == NULL) {
        free(pts);
        return false;
    }
    for (i = 0; i < n; i++) {
        if (i > 0) {
            acc += hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        }
        cum[i] = acc;
    }
    if (!(n >= 2 && isfinite(acc) && acc > 1e-6 && isfinite((double)start))) {
        free(pts);
        free(cum);
        return false;
    }
    out->pts = pts;
    out->cum = cum;
    out->n = n;
    out->start = (double)start;
    return true;
}

bool pc_text_warp_on_path(const pc_text_shape *shape, pc_text_deform *out)
{
    pc_path_frame f;
    if (shape->kind != PC_TEXT_SHAPE_PATH) {
        return false;
    }
    if (!path_frame_new(&shape->path, shape->start, shape->flip, &f)) {
        return false;
    }
    out->kind = PC_DEFORM_PATH;
    out->path = f;
    memset(&out->style, 0, sizeof out->style);
    return true;
}

bool pc_text_layout_deform(const pc_text_layout *l, const pc_text_layer *layer, pc_text_deform *out)
{
    pc_style_warp sw;
    if (pc_text_warp_on_path(&layer->shape, out)) {
        return true;
    }
    if (layer->has_warp && pc_text_layout_warp(l, &layer->warp, &sw)) {
        out->kind = PC_DEFORM_STYLE;
        out->style = sw;
        memset(&out->path, 0, sizeof out->path);
        return true;
    }
    return false;
}

double pc_text_deform_max_segment(const pc_text_deform *w)
{
    return w->kind == PC_DEFORM_PATH ? 2.0 : pc_style_warp_max_segment(&w->style);
}

static double path_frame_length(const pc_path_frame *p)
{
    return p->n > 0 ? p->cum[p->n - 1] : 0.0;
}

/* Point and unit tangent at arc length s; past either end the end tangent continues straight. */
static void path_frame_at(const pc_path_frame *p, double s, pc_point *pt, pc_point *tan)
{
    size_t last = p->n - 1, i, lo = 0, hi = p->n; /* partition_point(|c| c < s) */
    double len, along;
    pc_point a, b, t;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->cum[mid] < s) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    i = lo;
    if (i < 1) {
        i = 1;
    } else if (i > last) {
        i = last;
    }
    a = p->pts[i - 1];
    b = p->pts[i];
    len = hypot(b.x - a.x, b.y - a.y);
    if (len < 1e-12) {
        len = 1e-12;
    }
    t.x = (b.x - a.x) / len;
    t.y = (b.y - a.y) / len;
    along = s - p->cum[i - 1];
    pt->x = a.x + t.x * along;
    pt->y = a.y + t.y * along;
    *tan = t;
}

pc_point pc_text_deform_apply(const pc_text_deform *w, double x, double y)
{
    pc_point p, t;
    if (w->kind != PC_DEFORM_PATH) {
        return pc_style_warp_apply(&w->style, x, y);
    }
    path_frame_at(&w->path, w->path.start + x, &p, &t);
    return pc_point_make(p.x - t.y * y, p.y + t.x * y);
}

pc_placement_kind pc_text_deform_place_glyph(const pc_text_deform *w, double cx, double frame[6])
{
    double s, len;
    pc_point p, t;
    if (w->kind != PC_DEFORM_PATH) {
        return PC_PLACEMENT_BEND;
    }
    s = w->path.start + cx;
    len = path_frame_length(&w->path);
    if (!(s >= 0.0 && s <= len)) {
        return PC_PLACEMENT_HIDDEN;
    }
    path_frame_at(&w->path, s, &p, &t);
    frame[0] = t.x;
    frame[1] = t.y;
    frame[2] = -t.y;
    frame[3] = t.x;
    frame[4] = p.x - t.x * cx;
    frame[5] = p.y - t.y * cx;
    return PC_PLACEMENT_FRAME;
}
