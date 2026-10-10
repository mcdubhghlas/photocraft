/* Anti-aliased polygon rasteriser with exact area coverage and boolean combination (port of
 * crates/vector/src/raster.rs).
 *
 * Each pixel row is cut into horizontal sub-bands at every edge endpoint and every edge
 * crossing inside the row, so within a sub-band all active edges span its full height and keep
 * their left-to-right order. Walking the edges in that order with one winding counter per
 * component tells, between two consecutive edges, whether the region is inside the combined
 * shape. The boundary edges of the inside intervals are accumulated as signed trapezoid areas
 * into a per-row cell buffer whose prefix sum is the exact covered area of every pixel. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "vector/internal.h"

/* Same split depth as the reference: bounds the crossing recursion in band(). */
#define BAND_MAX_DEPTH 24u
/* Not in the reference: a per-row budget for crossing splits, charged with the number of
 * active edges each split re-scans (every split filters and re-sorts the row's edges). Rows with
 * hundreds of mutually crossing edges (e.g. 1e6 px wide dashed strokes), or coordinates so large
 * that rounding noise beats the 1e-9 crossing test (splitting down to BAND_MAX_DEPTH, 2^24
 * pieces), take minutes per render in the reference. Real geometry stays far below the budget
 * (the parity corpus peaks at 0.55M, the reference's 6016^2 perf scene at 0.14M), so its
 * coverage is bit-identical; past it, sub-bands are swept unsplit (still clamped to 0..1). */
#define BAND_SPLIT_BUDGET ((size_t)1 << 21)
/* Not in the reference either: every sub-band scans all of the row's active edges, so a row with
 * n active edges and their endpoints inside it costs O(n^2) (the reference has no bound: a
 * hostile path reached 1.7e9 scans in one row). Past this many scanned edges per row the rest of
 * the row is swept as a single sub-band. Real geometry peaks at 1.1M (parity corpus). */
#define ROW_SCAN_BUDGET ((size_t)1 << 24)
#define STRIP 32u

void pc_vec_rasterizer_init(pc_vec_rasterizer *r, bool inverted)
{
    memset(r, 0, sizeof *r);
    r->inverted = inverted;
    r->union_only = true;
}

void pc_vec_rasterizer_fini(pc_vec_rasterizer *r)
{
    if (r == NULL) {
        return;
    }
    free(r->edges);
    free(r->comps);
    pc_vec_rasterizer_init(r, false);
}

pc_status pc_vec_rasterizer_clone(const pc_vec_rasterizer *r, pc_vec_rasterizer *out, pc_error *err)
{
    pc_vec_rasterizer c = *r;
    c.edges = NULL;
    c.comps = NULL;
    c.edges_cap = r->nedges;
    c.comps_cap = r->ncomps;
    if (r->nedges > 0) {
        c.edges = pc_malloc_array(r->nedges, sizeof *c.edges);
        if (c.edges == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        memcpy(c.edges, r->edges, r->nedges * sizeof *c.edges);
    }
    if (r->ncomps > 0) {
        c.comps = pc_malloc_array(r->ncomps, sizeof *c.comps);
        if (c.comps == NULL) {
            free(c.edges);
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        memcpy(c.comps, r->comps, r->ncomps * sizeof *c.comps);
    }
    *out = c;
    return PC_OK;
}

static int edge_cmp(const void *a, const void *b)
{
    return pc_vec_total_cmp(((const pc_vec_raster_edge *)a)->y0, ((const pc_vec_raster_edge *)b)->y0);
}

pc_status pc_vec_rasterizer_add_component(pc_vec_rasterizer *r, const pc_vec_poly *polys, size_t n,
                                          pc_path_op op, pc_fill_rule rule, pc_error *err)
{
    size_t need = r->nedges;
    size_t old_edges = r->nedges;
    size_t comp_index = r->ncomps;
    uint32_t comp;
    size_t i;
    bool has_bounds = r->has_bounds;
    double bb[4];
    void *scratch;
    if (comp_index >= UINT32_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "vector: too many path components");
    }
    comp = (uint32_t)comp_index;
    for (i = 0; i < n; i++) {
        if (polys[i].len >= 2) {
            if (!pc_size_add(need, polys[i].len, &need) || need > PC_VEC_MAX_POINTS) {
                return pc_fail(err, PC_ERR_LIMIT, "vector: more than %zu edges", (size_t)PC_VEC_MAX_POINTS);
            }
        }
    }
    /* Reserve everything first, so a failure leaves r untouched. */
    if (r->ncomps == r->comps_cap) {
        size_t cap = r->comps_cap;
        void *nc = pc_grow_array(r->comps, &cap, r->ncomps + 1, sizeof *r->comps);
        if (nc == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        r->comps = nc;
        r->comps_cap = cap;
    }
    if (need > r->edges_cap) {
        size_t cap = r->edges_cap;
        void *ne = pc_grow_array(r->edges, &cap, need, sizeof *r->edges);
        if (ne == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        r->edges = ne;
        r->edges_cap = cap;
    }
    scratch = need > 0 ? pc_malloc_array(need, sizeof *r->edges) : NULL;
    if (need > 0 && scratch == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    memcpy(bb, r->bounds, sizeof bb);
    for (i = 0; i < n; i++) {
        const pc_vec_poly *poly = &polys[i];
        size_t m = poly->len;
        size_t k;
        if (m < 2) {
            continue;
        }
        for (k = 0; k < m; k++) {
            const double *a = poly->pts[k];
            const double *b = poly->pts[(k + 1) % m];
            pc_vec_raster_edge *e;
            if (!(isfinite(a[0]) && isfinite(a[1]) && isfinite(b[0]) && isfinite(b[1]))) {
                continue;
            }
            if (!has_bounds) {
                bb[0] = pc_rs_min(a[0], b[0]);
                bb[1] = pc_rs_min(a[1], b[1]);
                bb[2] = pc_rs_max(a[0], b[0]);
                bb[3] = pc_rs_max(a[1], b[1]);
                has_bounds = true;
            } else {
                bb[0] = pc_rs_min(bb[0], pc_rs_min(a[0], b[0]));
                bb[1] = pc_rs_min(bb[1], pc_rs_min(a[1], b[1]));
                bb[2] = pc_rs_max(bb[2], pc_rs_max(a[0], b[0]));
                bb[3] = pc_rs_max(bb[3], pc_rs_max(a[1], b[1]));
            }
            if (a[1] == b[1]) {
                continue;
            }
            e = &r->edges[r->nedges++];
            if (a[1] < b[1]) {
                e->x0 = a[0];
                e->y0 = a[1];
                e->y1 = b[1];
                e->dir = 1;
                e->dxdy = (b[0] - a[0]) / (b[1] - a[1]);
            } else {
                e->x0 = b[0];
                e->y0 = b[1];
                e->y1 = a[1];
                e->dir = -1;
                e->dxdy = (a[0] - b[0]) / (a[1] - b[1]);
            }
            e->comp = comp;
        }
    }
    (void)old_edges;
    r->comps[r->ncomps].op = op;
    r->comps[r->ncomps].rule = rule;
    r->ncomps++;
    if (op != PC_PATH_COMBINE && comp != 0) {
        r->union_only = false;
    }
    r->has_bounds = has_bounds;
    memcpy(r->bounds, bb, sizeof bb);
    pc_vec_stable_sort(r->edges, r->nedges, sizeof *r->edges, scratch, edge_cmp);
    free(scratch);
    return PC_OK;
}

bool pc_vec_rasterizer_pixel_bounds(const pc_vec_rasterizer *r, pc_rect *out)
{
    pc_rect b;
    if (r->inverted || !r->has_bounds) {
        return false;
    }
    b.x0 = pc_f64_to_i32(floor(r->bounds[0]));
    b.y0 = pc_f64_to_i32(floor(r->bounds[1]));
    b.x1 = pc_i32_sat_add(pc_f64_to_i32(ceil(r->bounds[2])), 1);
    b.y1 = pc_i32_sat_add(pc_f64_to_i32(ceil(r->bounds[3])), 1);
    if (pc_rect_is_empty(b)) {
        return false;
    }
    *out = b;
    return true;
}

bool pc_vec_rasterizer_is_inverted(const pc_vec_rasterizer *r)
{
    return r->inverted;
}

/* ---- rendering ---- */

typedef struct band_edge {
    double xa, xb, xm;
    uint32_t idx;
} band_edge;

typedef struct row_state {
    size_t *active;
    size_t nactive;
    size_t active_cap;
    double *cuts;
    double *cuts_scratch;
    size_t ncuts;
    size_t cuts_cap;
    band_edge *band;
    band_edge *band_scratch;
    size_t nband;
    size_t band_cap;
    int32_t *wind;
    float *acc; /* width + 1 cells: the extra one absorbs spill-over */
    size_t width;
    size_t splits; /* split budget left in this row (active edges re-scanned) */
    size_t scans;  /* active edges scanned by band() in this row */
} row_state;

static void row_state_fini(row_state *st)
{
    free(st->active);
    free(st->cuts);
    free(st->cuts_scratch);
    free(st->band);
    free(st->band_scratch);
    free(st->wind);
    free(st->acc);
}

static pc_status row_state_init(row_state *st, size_t comps, size_t width, pc_error *err)
{
    memset(st, 0, sizeof *st);
    st->width = width;
    /* The edge lists grow with the active set (row_reserve), not with the total edge count:
     * every parallel strip has its own state. */
    st->wind = pc_calloc_array(comps > 0 ? comps : 1, sizeof *st->wind);
    st->acc = pc_calloc_array(width + 1, sizeof *st->acc);
    if (st->wind == NULL || st->acc == NULL) {
        row_state_fini(st);
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    return PC_OK;
}

/* Grows a pair of same-sized arrays (data + sort scratch) to hold `need` elements. */
static bool grow_pair(void **a, void **b, size_t *cap, size_t need, size_t elem)
{
    size_t ca = *cap;
    size_t cb = *cap;
    void *na;
    void *nb;
    if (need <= *cap) {
        return true;
    }
    na = pc_grow_array(*a, &ca, need, elem);
    if (na == NULL) {
        return false;
    }
    *a = na;
    if (b != NULL) {
        nb = pc_grow_array(*b, &cb, ca, elem);
        if (nb == NULL) {
            return false; /* *a grew, *cap stays: still consistent */
        }
        *b = nb;
    }
    *cap = ca;
    return true;
}

static pc_status active_push(row_state *st, size_t idx, pc_error *err)
{
    if (!grow_pair((void **)&st->active, NULL, &st->active_cap, st->nactive + 1, sizeof *st->active)) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    st->active[st->nactive++] = idx;
    return PC_OK;
}

/* Room for this row's cuts (2 + 2 per active edge) and band entries (1 per active edge). */
static pc_status row_reserve(row_state *st, pc_error *err)
{
    if (!grow_pair((void **)&st->cuts, (void **)&st->cuts_scratch, &st->cuts_cap, 2 + 2 * st->nactive,
                   sizeof *st->cuts) ||
        !grow_pair((void **)&st->band, (void **)&st->band_scratch, &st->band_cap, st->nactive,
                   sizeof *st->band)) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    return PC_OK;
}

static bool comp_inside(const pc_vec_rasterizer *r, size_t comp, int32_t w)
{
    if (r->comps[comp].rule == PC_FILL_RULE_EVENODD) {
        return (w & 1) != 0;
    }
    return w != 0;
}

/* Whether the combined shape is inside given per-component winding numbers. */
static bool inside_at(const pc_vec_rasterizer *r, const int32_t *wind, int32_t union_count)
{
    bool acc = false;
    size_t i;
    if (r->union_only) {
        return (union_count > 0) != r->inverted;
    }
    for (i = 0; i < r->ncomps; i++) {
        bool v = comp_inside(r, i, wind[i]);
        /* Photoshop applies the first component as "combine". */
        pc_path_op op = i == 0 ? PC_PATH_COMBINE : r->comps[i].op;
        switch (op) {
        case PC_PATH_SUBTRACT:
            acc = acc && !v;
            break;
        case PC_PATH_INTERSECT:
            acc = acc && v;
            break;
        case PC_PATH_EXCLUDE:
            acc = acc != v;
            break;
        case PC_PATH_COMBINE:
        case PC_PATH_JOIN: /* a joined component (only reachable when built by hand) combines */
        default:
            acc = acc || v;
            break;
        }
    }
    return acc != r->inverted;
}

/* Coverage h over the whole row (a boundary left of the rectangle). */
static void add_full(float *acc, double h)
{
    acc[0] += pc_f64_to_f32(h);
}

/* Adds w x the area to the right of the segment (xa, ya) -> (xb, yb) (ya < yb, both inside one
 * pixel row) to the cell accumulator whose cell 0 is column x0. */
static void draw(float *acc, size_t ncells, int32_t x0, double xa, double ya, double xb, double yb, double w)
{
    double width = (double)(ncells - 1);
    double left = (double)x0;
    double ua = xa - left;
    double ub = xb - left;
    double dy = yb - ya;
    double dx;
    double end;
    double u;
    if (dy <= 0.0) {
        return;
    }
    if (ua > ub) {
        double t = ua;
        ua = ub;
        ub = t;
    }
    dx = ub - ua;
    /* Part left of the rectangle: full coverage of every column. */
    if (ua < 0.0) {
        double cut = pc_rs_min_k(ub, 0.0);
        double h = dx > 1e-12 ? dy * (cut - ua) / dx : dy;
        acc[0] += pc_f64_to_f32(w * h);
        if (ub <= 0.0) {
            return;
        }
        ua = 0.0;
    }
    if (ua >= width) {
        return;
    }
    end = pc_rs_min(ub, width);
    if (dx <= 1e-12) {
        double c = floor(ua);
        double f = ua - c;
        size_t ci = pc_f64_to_size(c);
        if (ci + 1 < ncells) {
            acc[ci] += pc_f64_to_f32(w * dy * (1.0 - f));
            acc[ci + 1] += pc_f64_to_f32(w * dy * f);
        }
        return;
    }
    u = ua;
    while (u < end) {
        double c = floor(u);
        double v = pc_rs_min(c + 1.0, end);
        double h = dy * (v - u) / dx;
        double m = 0.5 * (u + v) - c;
        size_t ci = pc_f64_to_size(c);
        /* ci + 1 <= width always holds (u < end <= width); the check only guards the buffer. */
        if (ci + 1 < ncells) {
            acc[ci] += pc_f64_to_f32(w * h * (1.0 - m));
            acc[ci + 1] += pc_f64_to_f32(w * h * m);
        }
        if (v <= u) {
            break;
        }
        u = v;
    }
}

static int band_cmp(const void *a, const void *b)
{
    return pc_vec_total_cmp(((const band_edge *)a)->xm, ((const band_edge *)b)->xm);
}

/* Accumulates the sub-band [a, b) of the current row. Recursion depth <= BAND_MAX_DEPTH. */
static void band(const pc_vec_rasterizer *r, double a, double b, int32_t x0, row_state *st, uint32_t depth)
{
    double mid;
    size_t i;
    size_t n;
    int32_t union_count = 0;
    bool inside;
    st->scans += st->nactive;
    if (b - a <= 1e-12) {
        return;
    }
    st->nband = 0;
    mid = 0.5 * (a + b);
    for (i = 0; i < st->nactive; i++) {
        const pc_vec_raster_edge *e = &r->edges[st->active[i]];
        if (e->y0 <= mid && e->y1 >= mid) {
            band_edge *be = &st->band[st->nband++];
            be->xa = e->x0 + (a - e->y0) * e->dxdy;
            be->xb = e->x0 + (b - e->y0) * e->dxdy;
            be->xm = 0.5 * (be->xa + be->xb);
            be->idx = (uint32_t)st->active[i];
        }
    }
    if (st->nband == 0) {
        if (r->inverted) {
            add_full(st->acc, b - a);
        }
        return;
    }
    pc_vec_stable_sort(st->band, st->nband, sizeof *st->band, st->band_scratch, band_cmp);
    /* An inversion of the order at the band's top or bottom means two edges cross inside it:
     * split there so the order is constant in every piece. */
    if (depth < BAND_MAX_DEPTH && st->splits >= st->nactive) {
        size_t k;
        for (k = 1; k < st->nband; k++) {
            band_edge p = st->band[k - 1];
            band_edge q = st->band[k];
            double top = p.xa - q.xa;
            double bot = p.xb - q.xb;
            if (top > 1e-9 || bot > 1e-9) {
                /* Crossing where (p - q)(y) = 0, linear in y over the band. */
                double t = fabs(top - bot) > 1e-15 ? top / (top - bot) : 0.5;
                double y = a + (b - a) * pc_clampd(t, 0.0, 1.0);
                if (y <= a + 1e-9 || y >= b - 1e-9) {
                    y = mid;
                }
                st->splits -= st->nactive;
                band(r, a, y, x0, st, depth + 1);
                band(r, y, b, x0, st, depth + 1);
                return;
            }
        }
    }
    memset(st->wind, 0, (r->ncomps > 0 ? r->ncomps : 1) * sizeof *st->wind);
    inside = r->inverted;
    if (inside) {
        add_full(st->acc, b - a);
    }
    n = st->nband;
    for (i = 0; i < n; i++) {
        band_edge be = st->band[i];
        const pc_vec_raster_edge *e = &r->edges[be.idx];
        size_t c = e->comp;
        bool now;
        if (r->union_only) {
            bool was = comp_inside(r, c, st->wind[c]);
            bool is;
            st->wind[c] = pc_i32_wrapping_add(st->wind[c], e->dir);
            is = comp_inside(r, c, st->wind[c]);
            union_count = pc_i32_wrapping_add(union_count, (int32_t)is - (int32_t)was);
        } else {
            st->wind[c] = pc_i32_wrapping_add(st->wind[c], e->dir);
        }
        now = inside_at(r, st->wind, union_count);
        if (now != inside) {
            draw(st->acc, st->width + 1, x0, be.xa, a, be.xb, b, now ? 1.0 : -1.0);
            inside = now;
        }
    }
}

/* Index of the first edge with y0 >= top (edges are sorted by total order of y0). */
static size_t partition_point(const pc_vec_rasterizer *r, double top)
{
    size_t lo = 0;
    size_t hi = r->nedges;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (r->edges[m].y0 < top) {
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    return lo;
}

static int cut_cmp(const void *a, const void *b)
{
    return pc_vec_total_cmp(*(const double *)a, *(const double *)b);
}

static pc_status render_rows(const pc_vec_rasterizer *r, pc_rect rect, float *out, row_state *st,
                             pc_error *err)
{
    size_t w = pc_rect_width(rect);
    double top = (double)rect.y0;
    size_t next = partition_point(r, top);
    size_t i;
    size_t row = 0;
    int64_t y;
    st->nactive = 0;
    for (i = 0; i < next; i++) {
        if (r->edges[i].y1 > top && active_push(st, i, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    for (y = rect.y0; y < rect.y1; y++, row++) {
        double ya = (double)y;
        double yb = (double)y + 1.0;
        float *row_out = out + row * w;
        size_t k;
        size_t keep = 0;
        float sum = 0.0f;
        double *cuts;
        size_t ncuts;
        for (k = 0; k < st->nactive; k++) {
            if (r->edges[st->active[k]].y1 > ya) {
                st->active[keep++] = st->active[k];
            }
        }
        st->nactive = keep;
        while (next < r->nedges && r->edges[next].y0 < yb) {
            if (r->edges[next].y1 > ya && active_push(st, next, err) != PC_OK) {
                return PC_ERR_NOMEM;
            }
            next++;
        }
        memset(st->acc, 0, (w + 1) * sizeof *st->acc);
        if (st->nactive == 0) {
            float v = r->inverted ? 1.0f : 0.0f;
            for (k = 0; k < w; k++) {
                row_out[k] = v;
            }
            continue;
        }
        if (row_reserve(st, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        /* Sub-band boundaries: the row edges plus every edge endpoint inside the row. */
        st->ncuts = 0;
        st->cuts[st->ncuts++] = ya;
        st->cuts[st->ncuts++] = yb;
        for (k = 0; k < st->nactive; k++) {
            const pc_vec_raster_edge *e = &r->edges[st->active[k]];
            if (e->y0 > ya && e->y0 < yb) {
                st->cuts[st->ncuts++] = e->y0;
            }
            if (e->y1 > ya && e->y1 < yb) {
                st->cuts[st->ncuts++] = e->y1;
            }
        }
        cuts = st->cuts;
        ncuts = st->ncuts;
        pc_vec_stable_sort(cuts, ncuts, sizeof *cuts, st->cuts_scratch, cut_cmp);
        /* dedup: drop values equal (==) to the previous kept one. */
        {
            size_t kept = 0;
            for (k = 0; k < ncuts; k++) {
                if (kept == 0 || cuts[k] != cuts[kept - 1]) {
                    cuts[kept++] = cuts[k];
                }
            }
            ncuts = kept;
        }
        st->splits = BAND_SPLIT_BUDGET;
        st->scans = 0;
        for (k = 0; k + 1 < ncuts; k++) {
            if (st->scans > ROW_SCAN_BUDGET) {
                band(r, cuts[k], cuts[ncuts - 1], rect.x0, st, 0);
                break;
            }
            band(r, cuts[k], cuts[k + 1], rect.x0, st, 0);
        }
        for (k = 0; k < w; k++) {
            sum += st->acc[k];
            row_out[k] = pc_clampf(sum, 0.0f, 1.0f);
        }
    }
    return PC_OK;
}

pc_status pc_vec_rasterizer_render_into(const pc_vec_rasterizer *r, pc_rect rect, float *out, size_t len,
                                        pc_error *err)
{
    size_t w = pc_rect_width(rect);
    size_t h = pc_rect_height(rect);
    size_t need;
    row_state st;
    pc_status s;
    if (!pc_size_mul(w, h, &need) || need > len) {
        return pc_fail(err, PC_ERR_INVALID, "vector: output buffer too small");
    }
    if (need == 0) {
        return PC_OK;
    }
    s = row_state_init(&st, r->ncomps, w, err);
    if (s != PC_OK) {
        return s;
    }
    s = render_rows(r, rect, out, &st, err);
    row_state_fini(&st);
    return s;
}

typedef struct strip_ctx {
    const pc_vec_rasterizer *r;
    pc_rect rect;
    float *out;
    size_t w;
} strip_ctx;

static pc_status strip_fn(void *vctx, size_t chunk, size_t begin, size_t end)
{
    strip_ctx *c = vctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        size_t h = pc_rect_height(c->rect);
        size_t row0 = i * STRIP;
        size_t rows = h - row0 < STRIP ? h - row0 : STRIP;
        int32_t y0 = (int32_t)((int64_t)c->rect.y0 + (int64_t)row0);
        pc_rect sr = pc_rect_make(c->rect.x0, y0, c->rect.x1, (int32_t)((int64_t)y0 + (int64_t)rows));
        pc_status st = pc_vec_rasterizer_render_into(c->r, sr, c->out + row0 * c->w, rows * c->w, NULL);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_vec_rasterizer_render(const pc_vec_rasterizer *r, pc_rect rect, float **out, size_t *len,
                                   pc_error *err)
{
    size_t w = pc_rect_width(rect);
    size_t h = pc_rect_height(rect);
    size_t n;
    float *buf;
    pc_status st;
    if (!pc_size_mul(w, h, &n) || n > PC_VEC_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "vector: render area of %zux%zu pixels is too large", w, h);
    }
    if (n == 0) {
        *out = NULL;
        *len = 0;
        return PC_OK;
    }
    buf = pc_calloc_array(n, sizeof *buf);
    if (buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    if (h > STRIP && n > 64u * 1024u) {
        /* Strips of 32 rows in parallel: every row is independent, so the result does not
         * depend on the scheduling. */
        strip_ctx c;
        c.r = r;
        c.rect = rect;
        c.out = buf;
        c.w = w;
        st = pc_pool_for(pc_pool_global(), (h + STRIP - 1) / STRIP, 1, strip_fn, &c);
        if (st != PC_OK) {
            st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
    } else {
        st = pc_vec_rasterizer_render_into(r, rect, buf, n, err);
    }
    if (st != PC_OK) {
        free(buf);
        return st;
    }
    *out = buf;
    *len = n;
    return PC_OK;
}
