/* Patch-based completion: grids, sources and the coarse-to-fine driver (port of
 * crates/algo/src/inpaint.rs); see inpaint.h. The solver is in inpaint_solver.c, the onion peel in
 * inpaint_peel.c, proximity match and texture synthesis in inpaint_synth.c. */
#include "algo/inpaint.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/inpaint_int.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Largest level (in cells) whose source patches' moments the solver keeps. */
#define SOURCE_MOMENTS_MAX ((size_t)1 << 20)
/* Columns per block of the sources' index. */
#define SOURCE_BLOCK ((size_t)64)
/* Row band of valid_sources' column pass. */
#define VS_BAND ((size_t)64)
#define MAX_LEVELS 64

/* ---- colour adaptation -------------------------------------------------------------------------- */

bool pc_algo_adapt_is_none(pc_algo_adapt a)
{
    return a.gain_lo == 1.0f && a.gain_hi == 1.0f && a.bias == 0.0f;
}

pc_status pc_algo_adapt_check(pc_algo_adapt a, pc_error *err)
{
    if (isnan(a.gain_lo) || isnan(a.gain_hi) || isnan(a.bias) || a.gain_lo > a.gain_hi || a.bias < 0.0f) {
        return pc_fail(err, PC_ERR_INVALID, "colour adaptation limits are invalid");
    }
    return PC_OK;
}

void pc_algo_adapt_fit(pc_algo_adapt a, float n, float st, float stt, float ss, float sss, float *gain,
                       float *bias)
{
    float mt = st / n, ms = ss / n;
    float vt = pc_rs_maxf_k(stt / n - mt * mt, 0.0f);
    float vs = pc_rs_maxf_k(sss / n - ms * ms, 0.0f);
    float g = vs > 1e-10f ? sqrtf(vt / vs) : 1.0f;
    float lo = a.gain_lo, hi = a.gain_hi;
    /* Nearly flat patches of about the same colour (their means within the bias of each other):
     * the ratio of their deviations is noise, so match the mean first, keeping the gain where the
     * bias can still bring the source's mean to the target's. A limit gain would otherwise move a
     * bright mean further than the bias could bring back, per channel: a tinted blotch in a smooth
     * gradient. FLAT is about 5 levels of 255. Textured patches are fitted as before. */
    if (pc_rs_maxf(vt, vs) < 0.02f * 0.02f && fabsf(mt - ms) <= a.bias && ms > 1e-6f) {
        float glo = (mt - a.bias) / ms, ghi = (mt + a.bias) / ms;
        if (pc_rs_maxf(glo, lo) <= pc_rs_minf(ghi, hi)) {
            lo = pc_rs_maxf(glo, lo);
            hi = pc_rs_minf(ghi, hi);
        }
    }
    g = pc_clampf(g, lo, hi);
    *gain = g;
    *bias = pc_clampf(mt - g * ms, -a.bias, a.bias);
}

pc_algo_complete_params pc_algo_complete_params_default(void)
{
    pc_algo_complete_params p = {3, 10, 3, 2, 192, {1.0f, 1.0f, 0.0f}, 1};
    return p;
}

/* ---- grids -------------------------------------------------------------------------------------- */

uint32_t *pc_ip_integral(size_t w, size_t h, const bool *m)
{
    size_t x, y;
    uint32_t *s = pc_calloc_array((w + 1) * (h + 1), sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        uint32_t row = 0;
        for (x = 0; x < w; x++) {
            row += m[y * w + x] ? 1u : 0u;
            s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + row;
        }
    }
    return s;
}

uint32_t pc_ip_window_count(const uint32_t *s, size_t gw, size_t gh, int64_t x0, int64_t y0, int64_t x1,
                            int64_t y1)
{
    size_t ux0 = x0 > 0 ? (size_t)x0 : 0, uy0 = y0 > 0 ? (size_t)y0 : 0;
    size_t ux1 = x1 > 0 ? (size_t)x1 : 0, uy1 = y1 > 0 ? (size_t)y1 : 0;
    size_t w1 = gw + 1;
    ux1 = ux1 < gw ? ux1 : gw;
    uy1 = uy1 < gh ? uy1 : gh;
    if (ux1 <= ux0 || uy1 <= uy0) {
        return 0;
    }
    return s[uy1 * w1 + ux1] + s[uy0 * w1 + ux0] - s[uy0 * w1 + ux1] - s[uy1 * w1 + ux0];
}

bool pc_ip_hole_bbox(size_t w, size_t h, const bool *hole, size_t b[4])
{
    size_t x, y;
    bool any = false;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (!hole[y * w + x]) {
                continue;
            }
            if (!any) {
                b[0] = x, b[1] = y, b[2] = x + 1, b[3] = y + 1;
                any = true;
            } else {
                b[0] = x < b[0] ? x : b[0];
                b[1] = y < b[1] ? y : b[1];
                b[2] = x + 1 > b[2] ? x + 1 : b[2];
                b[3] = y + 1 > b[3] ? y + 1 : b[3];
            }
        }
    }
    return any;
}

bool pc_ip_hole_bounds(size_t w, size_t h, const uint8_t *px, pc_ip_bounds *out)
{
    size_t x, y;
    bool any = false;
    pc_ip_bounds b = {0, 0, 0, 0};
    for (y = 0; y < h; y++) {
        const uint8_t *row = &px[y * w];
        size_t x0 = SIZE_MAX, x1 = 0;
        for (x = 0; x < w; x++) {
            if (row[x] == PC_ALGO_PX_HOLE) {
                x0 = x0 == SIZE_MAX ? x : x0;
                x1 = x + 1;
            }
        }
        if (x0 == SIZE_MAX) {
            continue;
        }
        if (!any) {
            b = (pc_ip_bounds){x0, y, x1, y + 1};
            any = true;
        } else {
            b.x0 = x0 < b.x0 ? x0 : b.x0;
            b.x1 = x1 > b.x1 ? x1 : b.x1;
            b.y1 = y + 1;
        }
    }
    if (any) {
        *out = b;
    }
    return any;
}

/* ---- valid sources ------------------------------------------------------------------------------ */

typedef struct vs_job {
    size_t w, h, r, side;
    const uint8_t *px;
    bool *run, *valid;
    const pc_interrupt *ctl;
} vs_job;

/* Per row: is the run of `side` pixels centred on each one all copyable (a sliding count)? */
static pc_status vs_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const vs_job *j = ctx;
    size_t y, x, w = j->w, r = j->r, side = j->side;
    (void)chunk;
    for (y = begin; y < end; y++) {
        const uint8_t *row = &j->px[y * w];
        bool *out = &j->run[y * w];
        size_t count = 0;
        if (y % VS_BAND == 0 && pc_interrupt_cancelled(j->ctl)) {
            continue;
        }
        for (x = 0; x < side; x++) {
            count += row[x] == PC_ALGO_PX_SOURCE ? 1u : 0u;
        }
        out[r] = count == side;
        for (x = r + 1; x < w - r; x++) {
            count = count + (row[x + r] == PC_ALGO_PX_SOURCE ? 1u : 0u) -
                    (row[x - r - 1] == PC_ALGO_PX_SOURCE ? 1u : 0u);
            out[x] = count == side;
        }
    }
    return PC_OK;
}

/* Per column, in bands of rows: how many runs end each row in a row; a centre is valid where
 * `side` of them stack up to r rows below it. */
static pc_status vs_bands(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const vs_job *j = ctx;
    size_t band, w = j->w, h = j->h, r = j->r, side = j->side;
    size_t *stack = pc_malloc_array(w, sizeof *stack);
    (void)chunk;
    if (stack == NULL) {
        return PC_ERR_NOMEM;
    }
    for (band = begin; band < end; band++) {
        size_t y0 = band * VS_BAND, rows = y0 + VS_BAND < h ? VS_BAND : h - y0, yy, x;
        size_t top = y0 > r ? y0 - r : 0, bottom = y0 + rows + r < h ? y0 + rows + r : h;
        if (pc_interrupt_cancelled(j->ctl)) {
            continue;
        }
        memset(stack, 0, w * sizeof *stack);
        for (yy = top; yy < bottom; yy++) {
            size_t y;
            for (x = 0; x < w; x++) {
                stack[x] = j->run[yy * w + x] ? stack[x] + 1 : 0;
            }
            if (yy < y0 + r || yy < 2 * r) {
                continue;
            }
            y = yy - r;
            for (x = r; x < w - r; x++) {
                j->valid[y * w + x] = stack[x] >= side;
            }
        }
    }
    free(stack);
    return PC_OK;
}

pc_status pc_ip_valid_sources(size_t w, size_t h, size_t r, const uint8_t *px, const pc_interrupt *ctl,
                              bool **out, pc_error *err)
{
    size_t side = 2 * r + 1, bands;
    bool par = w * h >= PC_IP_PAR_MIN;
    vs_job j;
    pc_status st;
    bool *valid = pc_calloc_array(w * h, sizeof *valid), *run;
    if (valid == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (w < side || h < side) {
        *out = valid;
        return PC_OK;
    }
    run = pc_calloc_array(w * h, sizeof *run);
    if (run == NULL) {
        free(valid);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    j = (vs_job){w, h, r, side, px, run, valid, ctl};
    st = par ? pc_algo_par_for(h, 1, vs_rows, &j) : vs_rows(&j, 0, 0, h);
    if (st == PC_OK && !pc_interrupt_cancelled(ctl)) {
        bands = (h + VS_BAND - 1) / VS_BAND;
        st = par ? pc_algo_par_for(bands, 1, vs_bands, &j) : vs_bands(&j, 0, 0, bands);
    }
    free(run);
    if (st != PC_OK) {
        free(valid);
        return pc_fail(err, st, "out of memory");
    }
    if (pc_interrupt_cancelled(ctl)) {
        free(valid);
        return pc_interrupt_check(ctl, err);
    }
    *out = valid;
    return PC_OK;
}

/* ---- the sources' index ------------------------------------------------------------------------- */

pc_status pc_ip_sources_new(const bool *valid, size_t w, size_t h, pc_ip_sources *out, pc_error *err)
{
    size_t blocks = (w + SOURCE_BLOCK - 1) / SOURCE_BLOCK, y, b, x, total = 0;
    pc_ip_sources s = {NULL, NULL, blocks, h};
    s.row_start = pc_malloc_array(h + 1, sizeof *s.row_start);
    s.block_start = pc_calloc_array(h * blocks + 1, sizeof *s.block_start);
    if (s.row_start == NULL || s.block_start == NULL) {
        pc_ip_sources_free(&s);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s.row_start[0] = 0;
    for (y = 0; y < h; y++) {
        uint32_t n = 0;
        for (b = 0; b < blocks; b++) {
            size_t x1 = (b + 1) * SOURCE_BLOCK < w ? (b + 1) * SOURCE_BLOCK : w;
            s.block_start[y * blocks + b] = n;
            for (x = b * SOURCE_BLOCK; x < x1; x++) {
                n += valid[y * w + x] ? 1u : 0u;
            }
        }
        total += n;
        s.row_start[y + 1] = total;
    }
    *out = s;
    return PC_OK;
}

void pc_ip_sources_free(pc_ip_sources *s)
{
    free(s->row_start);
    free(s->block_start);
    s->row_start = NULL;
    s->block_start = NULL;
}

size_t pc_ip_sources_len(const pc_ip_sources *s)
{
    return s->row_start != NULL ? s->row_start[s->h] : 0;
}

pc_ip_pt pc_ip_sources_nth(const pc_ip_sources *s, size_t k, const bool *valid, size_t w)
{
    /* partition_point(|s| s <= k) - 1 over the nondecreasing row starts. */
    size_t lo = 0, hi = s->h + 1, y, left, b, x0, x;
    const uint32_t *rb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->row_start[mid] <= k) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    y = lo > 0 ? lo - 1 : 0;
    if (y >= s->h) {
        return (pc_ip_pt){0, 0};
    }
    left = k - s->row_start[y];
    rb = &s->block_start[y * s->blocks];
    lo = 0, hi = s->blocks;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((size_t)rb[mid] <= left) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    b = lo > 0 ? lo - 1 : 0;
    left -= s->blocks > 0 ? rb[b] : 0;
    x0 = b * SOURCE_BLOCK;
    for (x = x0; x < w; x++) {
        if (valid[y * w + x]) {
            if (left == 0) {
                return (pc_ip_pt){(int32_t)x, (int32_t)y};
            }
            left--;
        }
    }
    return (pc_ip_pt){0, 0};
}

pc_status pc_ip_sources_all(const pc_ip_sources *s, const bool *valid, size_t w, pc_ip_pt **out,
                            pc_error *err)
{
    size_t n = pc_ip_sources_len(s), i, k = 0;
    pc_ip_pt *v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < w * s->h && k < n; i++) {
        if (valid[i]) {
            v[k++] = (pc_ip_pt){(int32_t)(i % w), (int32_t)(i / w)};
        }
    }
    *out = v;
    return PC_OK;
}

bool *pc_ip_targets_around(size_t w, size_t r, const uint8_t *px, pc_ip_bounds hb, pc_ip_bounds tb)
{
    size_t bw = pc_ip_bw(hb), bh = pc_ip_bh(hb), x, y, tw = pc_ip_bw(tb);
    int64_t ri = (int64_t)r, ox = (int64_t)hb.x0, oy = (int64_t)hb.y0;
    uint32_t *s = pc_calloc_array((bw + 1) * (bh + 1), sizeof *s);
    bool *t = pc_calloc_array(pc_ip_barea(tb) > 0 ? pc_ip_barea(tb) : 1, sizeof *t);
    if (s == NULL || t == NULL) {
        free(s);
        free(t);
        return NULL;
    }
    for (y = 0; y < bh; y++) {
        uint32_t row = 0;
        for (x = 0; x < bw; x++) {
            row += px[(hb.y0 + y) * w + hb.x0 + x] == PC_ALGO_PX_HOLE ? 1u : 0u;
            s[(y + 1) * (bw + 1) + x + 1] = s[y * (bw + 1) + x + 1] + row;
        }
    }
    for (y = tb.y0; y < tb.y1; y++) {
        for (x = tb.x0; x < tb.x1; x++) {
            int64_t xi = (int64_t)x, yi = (int64_t)y;
            t[(y - tb.y0) * tw + (x - tb.x0)] =
                pc_ip_window_count(s, bw, bh, xi - ri - ox, yi - ri - oy, xi + ri + 1 - ox, yi + ri + 1 - oy) > 0;
        }
    }
    free(s);
    return t;
}

/* ---- order statistics ---------------------------------------------------------------------------- */

/* f32::total_cmp as an unsigned key. */
static uint32_t total_key(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

pc_status pc_ip_nth_total(const float *v, size_t n, size_t k, float *out, pc_error *err)
{
    /* Radix select over the total-order keys: the value is the same as select_nth_unstable_by. */
    uint32_t prefix = 0, mask = 0, key;
    size_t i, shift, cnt[256];
    if (n == 0 || k >= n) {
        return pc_fail(err, PC_ERR_INVALID, "no such order statistic");
    }
    for (shift = 32; shift > 0; shift -= 8) {
        size_t b, s = shift - 8, acc = 0;
        memset(cnt, 0, sizeof cnt);
        for (i = 0; i < n; i++) {
            key = total_key(v[i]);
            if ((key & mask) == prefix) {
                cnt[(key >> s) & 0xFFu]++;
            }
        }
        for (b = 0; b < 256; b++) {
            if (k < acc + cnt[b]) {
                break;
            }
            acc += cnt[b];
        }
        k -= acc;
        prefix |= (uint32_t)b << s;
        mask |= 0xFFu << s;
    }
    key = prefix & 0x80000000u ? prefix & 0x7FFFFFFFu : ~prefix;
    memcpy(out, &key, 4);
    return PC_OK;
}

pc_status pc_ip_sigma2(const float *costs, size_t n, float *out, pc_error *err)
{
    size_t k;
    float v;
    pc_status st;
    if (n == 0) {
        *out = 1.0f;
        return PC_OK;
    }
    k = n * 3 / 4 < n - 1 ? n * 3 / 4 : n - 1;
    if ((st = pc_ip_nth_total(costs, n, k, &v, err)) != PC_OK) {
        return st;
    }
    *out = pc_rs_maxf_k(v, 1e-6f);
    return PC_OK;
}

/* ---- the driver --------------------------------------------------------------------------------- */

typedef struct level {
    size_t w, h;
    float *img;  /* owned except at level 0 */
    uint8_t *px; /* owned except at level 0 */
} level;

typedef struct ds_job {
    const level *l;
    level *o;
    size_t ch;
} ds_job;

/* Half-size level rows: a cell is a hole if any of its pixels is; else it averages its non-void
 * pixels, and may be copied from only if all of them may. */
static pc_status ds_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const ds_job *j = ctx;
    const level *l = j->l;
    size_t y, x, c, ch = j->ch, w = j->o->w;
    static const size_t DX[4] = {0, 1, 0, 1}, DY[4] = {0, 0, 1, 1};
    (void)chunk;
    for (y = begin; y < end; y++) {
        for (x = 0; x < w; x++) {
            float n = 0.0f, *o = &j->o->img[(y * w + x) * ch];
            bool hole = false, all_source = true, any = false;
            int q;
            for (q = 0; q < 4; q++) {
                size_t sx = x * 2 + DX[q], sy = y * 2 + DY[q], si;
                if (sx >= l->w || sy >= l->h) {
                    continue;
                }
                si = sy * l->w + sx;
                switch (l->px[si]) {
                case PC_ALGO_PX_HOLE:
                    hole = true;
                    break;
                case PC_ALGO_PX_VOID:
                    all_source = false;
                    break;
                default:
                    any = true;
                    all_source = all_source && l->px[si] == PC_ALGO_PX_SOURCE;
                    for (c = 0; c < ch; c++) {
                        o[c] += l->img[si * ch + c];
                    }
                    n += 1.0f;
                    break;
                }
            }
            if (n > 0.0f) {
                for (c = 0; c < ch; c++) {
                    o[c] /= n;
                }
            }
            j->o->px[y * w + x] = hole ? PC_ALGO_PX_HOLE
                                       : (!any ? PC_ALGO_PX_VOID : (all_source ? PC_ALGO_PX_SOURCE : PC_ALGO_PX_KNOWN));
        }
    }
    return PC_OK;
}

static pc_status downsample(const level *l, size_t ch, level *out, pc_error *err)
{
    size_t w = (l->w + 1) / 2, h = (l->h + 1) / 2;
    ds_job j;
    level o = {w, h, pc_calloc_array(w * h * ch, sizeof(float)), pc_calloc_array(w * h, 1)};
    if (o.img == NULL || o.px == NULL) {
        free(o.img);
        free(o.px);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    j = (ds_job){l, &o, ch};
    if (w * h >= PC_IP_PAR_MIN) {
        (void)pc_algo_par_for(h, 1, ds_rows, &j);
    } else {
        (void)ds_rows(&j, 0, 0, h);
    }
    *out = o;
    return PC_OK;
}

/* A finished coarser level: size, pixels, match field over its target box (INT32_MIN where none;
 * NULL when it had nothing to copy). */
typedef struct coarser {
    size_t w, h;
    float *img; /* owned unless it is level 0's */
    pc_ip_pt *field;
    pc_ip_bounds tbox;
} coarser;

static pc_ip_pt field_at(const coarser *c, size_t x, size_t y)
{
    pc_ip_bounds t = c->tbox;
    if (c->field == NULL || x < t.x0 || x >= t.x1 || y < t.y0 || y >= t.y1) {
        return (pc_ip_pt){INT32_MIN, INT32_MIN};
    }
    return c->field[(y - t.y0) * pc_ip_bw(t) + (x - t.x0)];
}

static pc_ip_bounds grow(pc_ip_bounds b, size_t r, size_t w, size_t h)
{
    pc_ip_bounds o;
    o.x0 = b.x0 > r ? b.x0 - r : 0;
    o.y0 = b.y0 > r ? b.y0 - r : 0;
    o.x1 = b.x1 + r < w ? b.x1 + r : w;
    o.y1 = b.y1 + r < h ? b.y1 + r : h;
    return o;
}

typedef struct complete_state {
    level levels[MAX_LEVELS];
    pc_ip_bounds boxes[MAX_LEVELS];
    size_t exts[MAX_LEVELS];
    size_t nlev;
    coarser prev;
    bool have_prev;
    float *level0; /* the caller's buffer */
} complete_state;

static void free_coarser(complete_state *s, coarser *c)
{
    if (c->img != s->level0) {
        free(c->img);
    }
    free(c->field);
    c->img = NULL;
    c->field = NULL;
}

/* Fill the hole pixels of level li from the membrane of its known pixels (nothing to copy from). */
static pc_status membrane_init(size_t lw, size_t lh, size_t ch, float *limg, const uint8_t *lpx, pc_error *err)
{
    size_t n = lw * lh, i;
    bool *unknown = pc_malloc_array(n, sizeof *unknown);
    float *m = NULL;
    pc_status st;
    if (unknown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        unknown[i] = lpx[i] == PC_ALGO_PX_HOLE || lpx[i] == PC_ALGO_PX_VOID;
    }
    st = pc_algo_poisson_membrane_fill(lw, lh, ch, limg, n * ch, unknown, n, NULL, &m, err);
    if (st == PC_OK) {
        for (i = 0; i < n; i++) {
            if (lpx[i] == PC_ALGO_PX_HOLE) {
                memcpy(&limg[i * ch], &m[i * ch], ch * sizeof(float));
            }
        }
    }
    free(m);
    free(unknown);
    return st;
}

/* Solve level li (taking ownership of its buffers into s->prev). *none is set when level 0 has
 * nothing to copy from. */
static pc_status complete_level(complete_state *s, size_t li, size_t ch, const pc_algo_complete_params *p,
                                size_t total_px, size_t *done_px, const pc_interrupt *ctl, bool *none,
                                pc_error *err)
{
    level L = s->levels[li];
    size_t lw = L.w, lh = L.h, r = p->patch_radius > 1 ? p->patch_radius : 1, psz = 2 * r + 1, x, y;
    pc_ip_bounds hb = s->boxes[li], tb = grow(hb, r, lw, lh);
    size_t tw = pc_ip_bw(tb), tarea = pc_ip_barea(tb), nsrc, n_targets = 0, k, em, pm_iters, it, pass;
    pc_ip_sources sources = {NULL, NULL, 0, 0};
    pc_ip_solver sv;
    coarser c = s->prev;
    bool have_c = s->have_prev, upsampled = false, refine_only, keep, owned = L.img != s->level0;
    pc_ip_pt *nnf = NULL, *all = NULL;
    float *cost = NULL;
    pc_ip_rng rng;
    int32_t full_search;
    pc_status st;
    memset(&sv, 0, sizeof sv);
    s->have_prev = false;
    s->levels[li].img = NULL;
    s->levels[li].px = NULL;
    if ((st = pc_ip_valid_sources(lw, lh, r, L.px, ctl, &sv.valid, err)) != PC_OK) {
        goto cleanup;
    }
    if ((st = pc_ip_sources_new(sv.valid, lw, lh, &sources, err)) != PC_OK) {
        goto cleanup;
    }
    nsrc = pc_ip_sources_len(&sources);
    if (li == 0 && nsrc == 0) {
        *none = true;
        goto cleanup;
    }
    sv.targets = pc_ip_targets_around(lw, r, L.px, hb, tb);
    if (sv.targets == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    if (have_c) {
        for (y = hb.y0; y < hb.y1; y++) {
            for (x = hb.x0; x < hb.x1; x++) {
                if (L.px[y * lw + x] == PC_ALGO_PX_HOLE) {
                    size_t sx = x / 2 < c.w - 1 ? x / 2 : c.w - 1, sy = y / 2 < c.h - 1 ? y / 2 : c.h - 1;
                    memcpy(&L.img[(y * lw + x) * ch], &c.img[(sy * c.w + sx) * ch], ch * sizeof(float));
                }
            }
        }
    } else if (nsrc > 0) {
        if ((st = pc_ip_sources_all(&sources, sv.valid, lw, &all, err)) != PC_OK) {
            goto cleanup;
        }
        st = pc_ip_onion_peel(lw, lh, ch, (int32_t)r, L.img, L.px, hb, all, nsrc, ctl, err);
        free(all);
        all = NULL;
        if (st != PC_OK) {
            goto cleanup;
        }
    } else if ((st = membrane_init(lw, lh, ch, L.img, L.px, err)) != PC_OK) {
        goto cleanup;
    }
    if (nsrc == 0) {
        if (have_c) {
            free_coarser(s, &c);
            have_c = false;
        }
        s->prev = (coarser){lw, lh, L.img, NULL, tb};
        s->have_prev = true;
        L.img = NULL;
        goto cleanup;
    }
    refine_only = li + 1 < s->nlev && s->exts[li] > p->refine_extent;
    for (k = 0; k < tarea; k++) {
        n_targets += sv.targets[k] ? 1u : 0u;
    }
    {
        size_t mult = refine_only ? 4 : 64;
        size_t lim = n_targets > SIZE_MAX / mult ? SIZE_MAX : n_targets * mult;
        keep = lw * lh <= SOURCE_MOMENTS_MAX && nsrc <= lim;
    }
    sv.w = lw, sv.h = lh, sv.ch = ch, sv.r = (int32_t)r, sv.img = L.img, sv.px = L.px;
    sv.hole_box = hb, sv.tbox = tb, sv.adapt = p->adapt, sv.ctl = ctl;
    if ((st = pc_ip_solver_prepare(&sv, keep, err)) != PC_OK) {
        goto cleanup;
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    rng = pc_ip_rng_new(p->seed ^ ((uint64_t)li << 20));
    nnf = pc_calloc_array(tarea > 0 ? tarea : 1, sizeof *nnf);
    cost = pc_malloc_array(tarea > 0 ? tarea : 1, sizeof *cost);
    if (nnf == NULL || cost == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (y = tb.y0; y < tb.y1; y++) {
        if (y % 32 == 0 && (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        for (x = tb.x0; x < tb.x1; x++) {
            bool got = false;
            k = (y - tb.y0) * tw + (x - tb.x0);
            if (!sv.targets[k]) {
                continue;
            }
            if (have_c && c.field != NULL) {
                size_t cx = x / 2 < c.w - 1 ? x / 2 : c.w - 1, cy = y / 2 < c.h - 1 ? y / 2 : c.h - 1;
                pc_ip_pt f = field_at(&c, cx, cy);
                if (f.x != INT32_MIN) {
                    /* The same displacement, twice as long. */
                    pc_ip_pt cand = {(int32_t)x + 2 * (f.x - (int32_t)cx), (int32_t)y + 2 * (f.y - (int32_t)cy)};
                    if (pc_ip_solver_is_valid(&sv, cand.x, cand.y)) {
                        nnf[k] = cand;
                        got = true;
                    }
                }
            }
            upsampled = upsampled || got;
            if (!got) {
                nnf[k] = pc_ip_sources_nth(&sources, (size_t)(pc_ip_rng_next(&rng) % (uint64_t)nsrc), sv.valid, lw);
            }
        }
    }
    if (have_c) {
        free_coarser(s, &c);
        have_c = false;
    }
    for (k = 0; k < tarea; k++) {
        cost[k] = INFINITY;
    }
    if (upsampled) {
        if ((st = pc_ip_costs(&sv, nnf, cost, err)) != PC_OK || (st = pc_ip_vote(&sv, nnf, cost, err)) != PC_OK) {
            goto cleanup;
        }
    }
    if (li + 1 == s->nlev) {
        em = p->em_iters;
    } else if (refine_only) {
        em = 1;
    } else {
        size_t sh = s->nlev - 1 - li;
        em = sh >= 64 ? 0 : p->em_iters >> sh;
        em = em > p->min_em_iters ? em : p->min_em_iters;
    }
    pm_iters = refine_only ? 1 : p->pm_iters;
    full_search = (int32_t)(lw > lh ? lw : lh);
    for (it = 0; it < em; it++) {
        int32_t radius;
        if ((st = pc_ip_costs(&sv, nnf, cost, err)) != PC_OK) {
            goto cleanup;
        }
        if (refine_only) {
            radius = 2;
        } else if (li + 1 == s->nlev || it == 0) {
            radius = full_search;
        } else {
            radius = 4 * (int32_t)psz < full_search ? 4 * (int32_t)psz : full_search;
        }
        for (pass = 0; pass < pm_iters; pass++) {
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                goto cleanup;
            }
            pc_ip_patchmatch(&sv, nnf, cost, pass + it * pm_iters, p->seed + (uint64_t)(li * 1000 + it), radius);
        }
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK || (st = pc_ip_vote(&sv, nnf, cost, err)) != PC_OK ||
            (st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
    }
    if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
        goto cleanup;
    }
    /* Keep the match field (unmatched cells marked) for the next level (none after the last). */
    if (li > 0) {
        for (k = 0; k < tarea; k++) {
            if (!sv.targets[k]) {
                nnf[k] = (pc_ip_pt){INT32_MIN, INT32_MIN};
            }
        }
    } else {
        free(nnf);
        nnf = NULL;
    }
    *done_px += lw * lh;
    pc_interrupt_progress(ctl, (float)*done_px / (float)(total_px > 1 ? total_px : 1));
    s->prev = (coarser){lw, lh, L.img, nnf, tb};
    s->have_prev = true;
    nnf = NULL;
    L.img = NULL;
cleanup:
    if (have_c) {
        free_coarser(s, &c);
    }
    pc_ip_sources_free(&sources);
    pc_ip_solver_fini(&sv);
    free(nnf);
    free(cost);
    if (owned) {
        free(L.img);
        free(L.px);
    }
    return st;
}

static pc_status check_params(const pc_algo_complete_params *p, pc_error *err)
{
    if (p == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing parameters");
    }
    if (p->patch_radius > PC_ALGO_INPAINT_MAX_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "patch radius %zu too large", p->patch_radius);
    }
    if (p->em_iters > PC_ALGO_INPAINT_MAX_ITERS || p->min_em_iters > PC_ALGO_INPAINT_MAX_ITERS ||
        p->pm_iters > PC_ALGO_INPAINT_MAX_ITERS) {
        return pc_fail(err, PC_ERR_LIMIT, "too many iterations");
    }
    return pc_algo_adapt_check(p->adapt, err);
}

pc_status pc_algo_inpaint_complete_masked_inplace(size_t w, size_t h, size_t ch, float *img, size_t len,
                                                  const uint8_t *px, size_t px_len,
                                                  const pc_algo_complete_params *p, const pc_interrupt *ctl,
                                                  bool *filled, pc_error *err)
{
    complete_state s;
    size_t n, r, psz, ext, i, total_px = 0, done_px = 0, li;
    pc_ip_bounds hb0;
    bool none = false;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if ((st = check_params(p, err)) != PC_OK) {
        return st;
    }
    if (px_len != n || (n > 0 && (img == NULL || px == NULL))) {
        return pc_fail(err, PC_ERR_INVALID, "role mask holds %zu entries, expected %zu", px_len, n);
    }
    for (i = 0; i < n; i++) {
        if (px[i] > PC_ALGO_PX_HOLE) {
            return pc_fail(err, PC_ERR_INVALID, "unknown pixel role %u", (unsigned)px[i]);
        }
    }
    if (n == 0 || !pc_ip_hole_bounds(w, h, px, &hb0)) {
        *filled = true;
        return PC_OK;
    }
    r = p->patch_radius > 1 ? p->patch_radius : 1;
    psz = 2 * r + 1;
    memset(&s, 0, sizeof s);
    s.level0 = img;
    s.levels[0] = (level){w, h, img, (uint8_t *)(uintptr_t)px};
    s.boxes[0] = hb0;
    ext = pc_ip_bw(hb0) > pc_ip_bh(hb0) ? pc_ip_bw(hb0) : pc_ip_bh(hb0);
    s.exts[0] = ext;
    s.nlev = 1;
    /* Pyramid: shrink until the hole is about four patches across. */
    for (;;) {
        level *l = &s.levels[s.nlev - 1];
        pc_ip_bounds b = s.boxes[s.nlev - 1];
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        if (ext <= 4 * psz || l->w / 2 < 3 * psz || l->h / 2 < 3 * psz || s.nlev == MAX_LEVELS) {
            break;
        }
        if ((st = downsample(l, ch, &s.levels[s.nlev], err)) != PC_OK) {
            goto cleanup;
        }
        s.boxes[s.nlev] = (pc_ip_bounds){b.x0 / 2, b.y0 / 2, (b.x1 + 1) / 2, (b.y1 + 1) / 2};
        ext = (ext + 1) / 2;
        s.exts[s.nlev] = ext;
        s.nlev++;
    }
    for (li = 0; li < s.nlev; li++) {
        total_px += s.levels[li].w * s.levels[li].h;
    }
    for (li = s.nlev; li-- > 0;) {
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        if ((st = complete_level(&s, li, ch, p, total_px, &done_px, ctl, &none, err)) != PC_OK || none) {
            goto cleanup;
        }
    }
cleanup:
    for (li = 1; li < s.nlev; li++) {
        free(s.levels[li].img);
        free(s.levels[li].px);
    }
    if (s.have_prev) {
        free_coarser(&s, &s.prev);
    }
    if (st == PC_OK) {
        *filled = !none;
    }
    return st;
}

pc_status pc_algo_inpaint_complete_masked(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                          const uint8_t *px, size_t px_len, const pc_algo_complete_params *p,
                                          const pc_interrupt *ctl, float **out, pc_error *err)
{
    size_t n;
    float *o;
    bool filled = false;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 0 && img == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    o = pc_malloc_array(n * ch > 0 ? n * ch : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (n > 0) {
        memcpy(o, img, n * ch * sizeof *o);
    }
    st = pc_algo_inpaint_complete_masked_inplace(w, h, ch, o, len, px, px_len, p, ctl, &filled, err);
    if (st != PC_OK || !filled) {
        free(o);
        o = NULL;
    }
    if (st == PC_OK) {
        *out = o;
    }
    return st;
}

pc_status pc_algo_inpaint_complete(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const bool *hole, size_t hole_len, const pc_algo_complete_params *p,
                                   const pc_interrupt *ctl, float **out, pc_error *err)
{
    size_t n, i;
    uint8_t *px;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (hole_len != n || (n > 0 && hole == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "hole mask holds %zu entries, expected %zu", hole_len, n);
    }
    px = pc_malloc_array(n > 0 ? n : 1, 1);
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        px[i] = hole[i] ? PC_ALGO_PX_HOLE : PC_ALGO_PX_SOURCE;
    }
    st = pc_algo_inpaint_complete_masked(w, h, ch, img, len, px, n, p, ctl, out, err);
    free(px);
    return st;
}
