/* Bayer demosaicing (port of crates/raw/src/demosaic.rs): bilinear, Malvar-He-Cutler (ICASSP
 * 2004) and AHD (Hirakawa & Parks, IEEE TIP 14(3), 2005). Input is a padded plane of normalised
 * CFA samples (mirror-reflected borders that keep the CFA parity); phase gives the colours of the
 * 2x2 cell at the image origin. Output is interleaved RGB, computed in parallel row bands. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "raw/internal.h"

/* Mirror-reflects i into 0..n (n >= 1); clamps when n is tiny. */
static size_t reflect(ptrdiff_t i, size_t n)
{
    ptrdiff_t m = (ptrdiff_t)n;
    if (m <= 1) {
        return 0;
    }
    if (i < 0) {
        i = -i;
    }
    if (i >= m) {
        i = 2 * (m - 1) - i;
    }
    return (size_t)(i < 0 ? 0 : i > m - 1 ? m - 1 : i);
}

pc_status rx_padded_new(size_t w, size_t h, rx_padded *out, pc_error *err)
{
    size_t stride = w + 2 * RX_PAD;
    out->data = pc_calloc_array(stride * (h + 2 * RX_PAD), sizeof *out->data);
    if (out->data == NULL) {
        return rx_nomem(err);
    }
    out->w = w;
    out->h = h;
    out->stride = stride;
    return PC_OK;
}

void rx_padded_fini(rx_padded *p)
{
    free(p->data);
    p->data = NULL;
}

void rx_padded_fill_borders(rx_padded *p)
{
    size_t w = p->w;
    size_t h = p->h;
    size_t s = p->stride;
    size_t y;
    size_t py;
    if (w == 0 || h == 0) {
        return;
    }
    for (y = RX_PAD; y < RX_PAD + h; y++) {
        float *row = p->data + y * s;
        size_t px;
        for (px = 0; px < s; px++) {
            if (px < RX_PAD || px >= RX_PAD + w) {
                row[px] = row[RX_PAD + reflect((ptrdiff_t)px - RX_PAD, w)];
            }
        }
    }
    for (py = 0; py < h + 2 * RX_PAD; py++) {
        if (py < RX_PAD || py >= RX_PAD + h) {
            size_t src = RX_PAD + reflect((ptrdiff_t)py - RX_PAD, h);
            memmove(p->data + py * s, p->data + src * s, s * sizeof *p->data);
        }
    }
}

static uint8_t pcolor(const uint8_t phase[4], ptrdiff_t x, ptrdiff_t y)
{
    return phase[((size_t)y & 1) * 2 + ((size_t)x & 1)];
}

size_t rx_demosaic_kernel(const uint8_t phase[4], pc_raw_demosaic method, size_t stride, size_t pos, size_t cc,
                          ptrdiff_t offs[11], float weights[11])
{
    ptrdiff_t px = (ptrdiff_t)(pos & 1);
    ptrdiff_t py = (ptrdiff_t)(pos >> 1);
    uint8_t own = pcolor(phase, px, py);
    uint8_t c = (uint8_t)cc;
    ptrdiff_t s = (ptrdiff_t)stride;
    ptrdiff_t dx[11];
    ptrdiff_t dy[11];
    float wt[11];
    size_t n = 0;
    size_t i;
    float norm;
#define TAP(a, b, ww)                                                                              \
    do {                                                                                           \
        dx[n] = (a);                                                                               \
        dy[n] = (b);                                                                               \
        wt[n] = (ww);                                                                              \
        n++;                                                                                       \
    } while (0)
    if (c == own) {
        TAP(0, 0, 1.0f);
    } else if (method == PC_RAW_BILINEAR) {
        ptrdiff_t a;
        ptrdiff_t b;
        float w;
        for (b = -1; b <= 1; b++) {
            for (a = -1; a <= 1; a++) {
                if (pcolor(phase, px + a, py + b) == c) {
                    TAP(a, b, 0.0f);
                }
            }
        }
        w = 1.0f / (float)(n > 0 ? n : 1);
        for (i = 0; i < n; i++) {
            wt[i] = w;
        }
    } else if (c == 1) {
        /* Malvar-He-Cutler (weights / 8): G at R or B. */
        TAP(0, 0, 4.0f);
        TAP(-1, 0, 2.0f);
        TAP(1, 0, 2.0f);
        TAP(0, -1, 2.0f);
        TAP(0, 1, 2.0f);
        TAP(-2, 0, -1.0f);
        TAP(2, 0, -1.0f);
        TAP(0, -2, -1.0f);
        TAP(0, 2, -1.0f);
    } else if (own == 1) {
        /* R or B at G: from the row neighbours or the column neighbours. */
        ptrdiff_t ax = pcolor(phase, px + 1, py) == c ? 1 : 0;
        ptrdiff_t ay = ax == 1 ? 0 : 1;
        TAP(0, 0, 5.0f);
        TAP(-ax, -ay, 4.0f);
        TAP(ax, ay, 4.0f);
        TAP(-1, -1, -1.0f);
        TAP(1, -1, -1.0f);
        TAP(-1, 1, -1.0f);
        TAP(1, 1, -1.0f);
        TAP(-2 * ax, -2 * ay, -1.0f);
        TAP(2 * ax, 2 * ay, -1.0f);
        TAP(-2 * ay, -2 * ax, 0.5f);
        TAP(2 * ay, 2 * ax, 0.5f);
    } else {
        /* R at B or B at R. */
        TAP(0, 0, 6.0f);
        TAP(-1, -1, 2.0f);
        TAP(1, -1, 2.0f);
        TAP(-1, 1, 2.0f);
        TAP(1, 1, 2.0f);
        TAP(-2, 0, -1.5f);
        TAP(2, 0, -1.5f);
        TAP(0, -2, -1.5f);
        TAP(0, 2, -1.5f);
    }
#undef TAP
    norm = method == PC_RAW_BILINEAR || c == own ? 1.0f : 1.0f / 8.0f;
    for (i = 0; i < n; i++) {
        offs[i] = dy[i] * s + dx[i];
        weights[i] = wt[i] * norm;
    }
    return n;
}

typedef struct lin_ctx {
    const rx_padded *p;
    uint8_t phase[4];
    pc_raw_demosaic method;
    ptrdiff_t offs[4][3][11];
    float wts[4][3][11];
    size_t ntaps[4][3];
    float *out;
} lin_ctx;

static pc_status linear_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    lin_ctx *c = ctx;
    const float *d = c->p->data;
    size_t w = c->p->w;
    size_t s = c->p->stride;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t base = (y + RX_PAD) * s + RX_PAD;
        size_t x;
        for (x = 0; x < w; x++) {
            size_t pos = (y & 1) * 2 + (x & 1);
            size_t i = base + x;
            float *o = c->out + (y * w + x) * 3;
            size_t cc;
            if (c->method == PC_RAW_BILINEAR) {
                /* Direct Bayer bilinear (the kernels' result without the tap loops). */
                size_t own = c->phase[pos] < 2 ? c->phase[pos] : 2;
                float n = d[i - s];
                float so = d[i + s];
                float e = d[i + 1];
                float wv = d[i - 1];
                if (own == 1) {
                    uint8_t hp = c->phase[(y & 1) * 2 + ((x + 1) & 1)];
                    size_t hc = hp < 2 ? hp : 2;
                    o[1] = d[i];
                    o[hc] = (e + wv) * 0.5f;
                    o[2 - hc] = (n + so) * 0.5f;
                } else {
                    o[own] = d[i];
                    o[1] = (n + so + e + wv) * 0.25f;
                    o[2 - own] = (d[i - s - 1] + d[i - s + 1] + d[i + s - 1] + d[i + s + 1]) * 0.25f;
                }
                continue;
            }
            for (cc = 0; cc < 3; cc++) {
                float v = 0.0f;
                size_t k;
                for (k = 0; k < c->ntaps[pos][cc]; k++) {
                    v += c->wts[pos][cc][k] * d[(size_t)((ptrdiff_t)i + c->offs[pos][cc][k])];
                }
                o[cc] = pc_clampf(v, 0.0f, 1.0f);
            }
        }
    }
    return PC_OK;
}

/* ---- AHD ---- */

#define LABF_STEPS (1u << 14)
#define LABF_RANGE 2.0f
#define AHD_BAND 32u
#define M 3u

float rx_labf_exact(float t) { return t > 0.008856f ? pc_cbrtf(t) : 7.787f * t + 16.0f / 116.0f; }

static float labf(const float *table, float t)
{
    float x;
    size_t i;
    float a;
    if (!(t >= 0.0f && t < LABF_RANGE)) {
        return rx_labf_exact(pc_rs_maxf_k(t, 0.0f));
    }
    x = t * ((float)LABF_STEPS / LABF_RANGE);
    i = pc_f32_to_size(x);
    if (i > LABF_STEPS) {
        i = LABF_STEPS;
    }
    a = table[i];
    return a + (table[i + 1] - a) * (x - (float)i);
}

void rx_labf_table(float table[RX_LABF_ENTRIES])
{
    size_t i;
    for (i = 0; i <= LABF_STEPS + 1; i++) {
        table[i] = rx_labf_exact((float)i * LABF_RANGE / (float)LABF_STEPS);
    }
}

float rx_labf(const float *table, float t) { return labf(table, t); }

typedef struct ahd_ctx {
    const rx_padded *p;
    uint8_t phase[4];
    float m[3][3];
    const float *labf;
    float *out;
    size_t h;
} ahd_ctx;

static uint8_t acolor(const uint8_t phase[4], size_t px, size_t py) { return phase[(py & 1) * 2 + (px & 1)]; }

typedef struct dist {
    float l;
    float c;
} dist;

static dist distance(const float *a, const float *c)
{
    dist d;
    d.l = fabsf(a[0] - c[0]);
    d.c = (a[1] - c[1]) * (a[1] - c[1]) + (a[2] - c[2]) * (a[2] - c[2]);
    return d;
}

static pc_status ahd_band(void *ctx, size_t chunk, size_t begin, size_t end)
{
    ahd_ctx *c = ctx;
    const float *d = c->p->data;
    size_t w = c->p->w;
    size_t s = c->p->stride;
    size_t band = chunk;
    size_t rows = end - begin;
    size_t n = rows + 2 * M;
    float *g[2] = {NULL, NULL};
    float *rgb[2] = {NULL, NULL};
    float *lab[2] = {NULL, NULL};
    uint8_t *hom[2] = {NULL, NULL};
    size_t b;
    size_t x;
    int dir;
    pc_status st = PC_OK;
    const uint8_t *phase = c->phase;
#define PY(bb) (band * AHD_BAND + RX_PAD - M + (bb))
    for (dir = 0; dir < 2; dir++) {
        g[dir] = pc_calloc_array(n * s, sizeof(float));
        rgb[dir] = pc_calloc_array(n * s * 3, sizeof(float));
        lab[dir] = pc_calloc_array(n * s * 3, sizeof(float));
        hom[dir] = pc_calloc_array(n * s, 1);
        if (g[dir] == NULL || rgb[dir] == NULL || lab[dir] == NULL || hom[dir] == NULL) {
            st = PC_ERR_NOMEM;
            goto done;
        }
    }
    /* 1. Green interpolated horizontally (0) and vertically (1), columns 2..s-2. */
    for (b = 0; b < n; b++) {
        size_t y = PY(b);
        for (x = 2; x < s - 2; x++) {
            size_t i = y * s + x;
            float cv = d[i];
            size_t k = b * s + x;
            float l;
            float r;
            float u;
            float dn;
            float gh;
            float gv;
            if (acolor(phase, x, y) == 1) {
                g[0][k] = cv;
                g[1][k] = cv;
                continue;
            }
            l = d[i - 1];
            r = d[i + 1];
            gh = (l + r) * 0.5f + (2.0f * cv - d[i - 2] - d[i + 2]) * 0.25f;
            g[0][k] = pc_clampf(gh, pc_rs_minf(l, r), pc_rs_maxf(l, r));
            u = d[i - s];
            dn = d[i + s];
            gv = (u + dn) * 0.5f + (2.0f * cv - d[i - 2 * s] - d[i + 2 * s]) * 0.25f;
            g[1][k] = pc_clampf(gv, pc_rs_minf(u, dn), pc_rs_maxf(u, dn));
        }
    }
    /* 2. RGB candidates and their Lab, band rows 1..n-1, columns 3..s-3. */
    for (dir = 0; dir < 2; dir++) {
        const float *gd = g[dir];
        float *rgb_d = rgb[dir];
        float *lab_d = lab[dir];
#define DIFF(bb, xx) (d[PY(bb) * s + (xx)] - gd[(bb) * s + (xx)])
        for (b = 1; b + 1 < n; b++) {
            size_t y = PY(b);
            for (x = 3; x < s - 3; x++) {
                uint8_t own = acolor(phase, x, y);
                float gv = gd[b * s + x];
                float px[3];
                float xyz[3];
                float fx;
                float fy;
                float fz;
                size_t i;
                int k;
                px[0] = px[1] = px[2] = gv;
                if (own == 1) {
                    uint8_t hcol = acolor(phase, x + 1, y);
                    size_t hc = hcol < 2 ? hcol : 2;
                    px[hc] = gv + 0.5f * (DIFF(b, x - 1) + DIFF(b, x + 1));
                    px[2 - hc] = gv + 0.5f * (DIFF(b - 1, x) + DIFF(b + 1, x));
                } else {
                    size_t o = own < 2 ? own : 2;
                    px[o] = d[y * s + x];
                    px[2 - o] = gv + 0.25f * (DIFF(b - 1, x - 1) + DIFF(b - 1, x + 1) + DIFF(b + 1, x - 1) +
                                              DIFF(b + 1, x + 1));
                }
                for (k = 0; k < 3; k++) {
                    px[k] = pc_clampf(px[k], 0.0f, 1.0f);
                }
                i = (b * s + x) * 3;
                memcpy(rgb_d + i, px, sizeof px);
                for (k = 0; k < 3; k++) {
                    xyz[k] = c->m[k][0] * px[0] + c->m[k][1] * px[1] + c->m[k][2] * px[2];
                }
                fx = labf(c->labf, xyz[0]);
                fy = labf(c->labf, xyz[1]);
                fz = labf(c->labf, xyz[2]);
                lab_d[i] = 116.0f * fy - 16.0f;
                lab_d[i + 1] = 500.0f * (fx - fy);
                lab_d[i + 2] = 200.0f * (fy - fz);
            }
        }
#undef DIFF
    }
    /* 3. Homogeneity maps, band rows 2..n-2, columns 4..s-4. */
    for (b = 2; b + 2 < n; b++) {
        for (x = 4; x < s - 4; x++) {
            size_t k = b * s + x;
            size_t nb[4] = {k - 1, k + 1, k - s, k + s};
            dist dh[4];
            dist dv[4];
            float eps_l;
            float eps_c;
            uint8_t ch = 0;
            uint8_t cv = 0;
            int j;
            for (j = 0; j < 4; j++) {
                dh[j] = distance(lab[0] + k * 3, lab[0] + nb[j] * 3);
                dv[j] = distance(lab[1] + k * 3, lab[1] + nb[j] * 3);
            }
            /* The horizontal candidate judged along the row, the vertical along the column. */
            eps_l = pc_rs_minf(pc_rs_maxf(dh[0].l, dh[1].l), pc_rs_maxf(dv[2].l, dv[3].l));
            eps_c = pc_rs_minf(pc_rs_maxf(dh[0].c, dh[1].c), pc_rs_maxf(dv[2].c, dv[3].c));
            for (j = 0; j < 4; j++) {
                ch = (uint8_t)(ch + (dh[j].l <= eps_l && dh[j].c <= eps_c));
                cv = (uint8_t)(cv + (dv[j].l <= eps_l && dv[j].c <= eps_c));
            }
            hom[0][k] = ch;
            hom[1][k] = cv;
        }
    }
    /* 4. Per pixel, the candidate with the larger 3x3 homogeneity sum. */
    for (b = 0; b < rows; b++) {
        size_t bb0 = b + M;
        for (x = 0; x < w; x++) {
            size_t px = x + RX_PAD;
            uint32_t sh = 0;
            uint32_t sv = 0;
            size_t bb;
            size_t i = (bb0 * s + px) * 3;
            float *o = c->out + ((begin + b) * w + x) * 3;
            int k;
            for (bb = bb0 - 1; bb <= bb0 + 1; bb++) {
                size_t xx;
                for (xx = px - 1; xx <= px + 1; xx++) {
                    sh += hom[0][bb * s + xx];
                    sv += hom[1][bb * s + xx];
                }
            }
            for (k = 0; k < 3; k++) {
                if (sh > sv) {
                    o[k] = rgb[0][i + (size_t)k];
                } else if (sh < sv) {
                    o[k] = rgb[1][i + (size_t)k];
                } else {
                    o[k] = 0.5f * (rgb[0][i + (size_t)k] + rgb[1][i + (size_t)k]);
                }
            }
        }
    }
#undef PY
done:
    for (dir = 0; dir < 2; dir++) {
        free(g[dir]);
        free(rgb[dir]);
        free(lab[dir]);
        free(hom[dir]);
    }
    return st;
}

pc_status rx_demosaic(const rx_padded *p, const uint8_t phase[4], pc_raw_demosaic method, const pc_raw_mat3 *to_xyz,
                      float **out, pc_error *err)
{
    size_t w = p->w;
    size_t h = p->h;
    size_t s = p->stride;
    float *o = pc_calloc_array(w * h * 3 > 0 ? w * h * 3 : 1, sizeof *o);
    pc_status st;
    if (o == NULL) {
        return rx_nomem(err);
    }
    if (w == 0 || h == 0 || p->data == NULL) {
        *out = o;
        return PC_OK;
    }
    (void)s;
    if (method == PC_RAW_AHD) {
        static const float WHITE[3] = {0.9642f, 1.0f, 0.8249f};
        ahd_ctx c;
        float *table = pc_malloc_array(LABF_STEPS + 2, sizeof *table);
        size_t i;
        int r;
        int q;
        if (table == NULL) {
            free(o);
            return rx_nomem(err);
        }
        for (i = 0; i <= LABF_STEPS + 1; i++) {
            table[i] = rx_labf_exact((float)i * LABF_RANGE / (float)LABF_STEPS);
        }
        c.p = p;
        memcpy(c.phase, phase, 4);
        for (r = 0; r < 3; r++) {
            for (q = 0; q < 3; q++) {
                c.m[r][q] = pc_f64_to_f32(to_xyz->m[r][q]) / WHITE[r];
            }
        }
        c.labf = table;
        c.out = o;
        c.h = h;
        st = pc_pool_for(pc_pool_global(), h, AHD_BAND, ahd_band, &c);
        free(table);
        if (st != PC_OK) {
            free(o);
            return rx_nomem(err);
        }
    } else {
        lin_ctx *c = malloc(sizeof *c);
        size_t pos;
        size_t cc;
        if (c == NULL) {
            free(o);
            return rx_nomem(err);
        }
        c->p = p;
        memcpy(c->phase, phase, 4);
        c->method = method;
        c->out = o;
        for (pos = 0; pos < 4; pos++) {
            for (cc = 0; cc < 3; cc++) {
                c->ntaps[pos][cc] =
                    rx_demosaic_kernel(phase, method, s, pos, cc, c->offs[pos][cc], c->wts[pos][cc]);
            }
        }
        (void)pc_pool_for(pc_pool_global(), h, rx_band_rows(w), linear_rows, c);
        free(c);
    }
    *out = o;
    return PC_OK;
}
