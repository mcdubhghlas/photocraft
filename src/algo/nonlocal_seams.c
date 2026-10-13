/* The non-local completion's seam hiding (nonlocal::Solver::blend_seams and fuse_seams); see
 * nonlocal_int.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/nonlocal_int.h"
#include "algo/poisson.h"
#include "algo/remove.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Radius of the median taken along the edge. */
#define EDGE_MEDIAN ((size_t)8)
/* Largest shift the edge passes on, beyond twice its local texture. */
#define MAX_SHIFT 0.05f
#define SEAM_CONTRAST 3.0f
#define OMEGA 1.8f
/* Only cells within this many of a seam are solved for. */
#define FUSE_BAND ((size_t)16)

static const int32_t N4[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

typedef struct blend_box {
    size_t x0, y0, bw, bh;
} blend_box;

static size_t local_of(const nl_solver *s, const blend_box *b, size_t i)
{
    return (i / s->w - b->y0) * b->bw + (i % s->w - b->x0);
}

static bool in_hole(const nl_solver *s, const blend_box *b, size_t lq)
{
    return s->hole[(b->y0 + lq / b->bw) * s->w + b->x0 + lq % b->bw];
}

/* One colour channel's shift field over the box (v: bw * bh). */
static pc_status blend_channel(const nl_solver *s, const nl_copied *copied, size_t ncopied, const blend_box *b,
                               size_t c, float unscale, float *v, uint8_t *n, float *raw, float *near,
                               size_t *queue, bool *seen, bool *unknown, pc_error *err)
{
    size_t w = s->w, h = s->h, cha = s->cha, ch = s->ch, area = b->bw * b->bh, k, q, lq, head = 0, tail = 0;
    memset(v, 0, area * sizeof *v);
    memset(n, 0, area);
    for (k = 0; k < ncopied; k++) {
        int32_t px = (int32_t)(copied[k].p % w), py = (int32_t)(copied[k].p / w);
        int32_t sx = (int32_t)(copied[k].s % w), sy = (int32_t)(copied[k].s / w);
        for (q = 0; q < 4; q++) {
            int32_t qx = px + N4[q][0], qy = py + N4[q][1], tx = sx + N4[q][0], ty = sy + N4[q][1];
            size_t qi, ti;
            if (qx < 0 || qy < 0 || (size_t)qx >= w || (size_t)qy >= h || tx < 0 || ty < 0 || (size_t)tx >= w ||
                (size_t)ty >= h) {
                continue;
            }
            qi = (size_t)qy * w + (size_t)qx, ti = (size_t)ty * w + (size_t)tx;
            if (s->hole[qi]) {
                continue;
            }
            lq = local_of(s, b, qi);
            v[lq] += s->img[qi * cha + c] - s->img[ti * cha + c];
            n[lq] = n[lq] < 255 ? (uint8_t)(n[lq] + 1) : 255;
        }
    }
    for (lq = 0; lq < area; lq++) {
        if (n[lq] > 0) {
            v[lq] /= (float)n[lq];
        }
    }
    /* Each edge cell takes the median difference of the edge cells around it, capped. */
    memcpy(raw, v, area * sizeof *raw);
    for (lq = 0; lq < area; lq++) {
        size_t qx = lq % b->bw, qy = lq / b->bw, x, y, nn = 0, gq;
        size_t ylo = qy > EDGE_MEDIAN ? qy - EDGE_MEDIAN : 0, yhi = qy + EDGE_MEDIAN + 1 < b->bh ? qy + EDGE_MEDIAN + 1 : b->bh;
        size_t xlo = qx > EDGE_MEDIAN ? qx - EDGE_MEDIAN : 0, xhi = qx + EDGE_MEDIAN + 1 < b->bw ? qx + EDGE_MEDIAN + 1 : b->bw;
        float m, texture, cap;
        pc_status st;
        if (n[lq] == 0) {
            continue;
        }
        for (y = ylo; y < yhi; y++) {
            for (x = xlo; x < xhi; x++) {
                if (n[y * b->bw + x] > 0) {
                    near[nn++] = raw[y * b->bw + x];
                }
            }
        }
        if ((st = pc_ip_nth_total(near, nn, nn / 2, &m, err)) != PC_OK) {
            return st;
        }
        gq = (b->y0 + qy) * w + b->x0 + qx;
        texture = (s->img[gq * cha + ch] + s->img[gq * cha + ch + 1]) * unscale;
        cap = MAX_SHIFT + 2.0f * texture;
        v[lq] = pc_clampf(m, -cap, cap);
    }
    /* Known cells off the ring take the value of the nearest ring cell (breadth first). */
    for (lq = 0; lq < area; lq++) {
        seen[lq] = n[lq] > 0;
        if (seen[lq]) {
            queue[tail++] = lq;
        }
    }
    while (head < tail) {
        size_t cur = queue[head++], x = cur % b->bw, y = cur / b->bw, nb[4], nnb = 0;
        if (x > 0) {
            nb[nnb++] = cur - 1;
        }
        if (x + 1 < b->bw) {
            nb[nnb++] = cur + 1;
        }
        if (y > 0) {
            nb[nnb++] = cur - b->bw;
        }
        if (y + 1 < b->bh) {
            nb[nnb++] = cur + b->bw;
        }
        for (k = 0; k < nnb; k++) {
            if (!seen[nb[k]] && !in_hole(s, b, nb[k])) {
                seen[nb[k]] = true;
                v[nb[k]] = v[cur];
                queue[tail++] = nb[k];
            }
        }
    }
    for (lq = 0; lq < area; lq++) {
        unknown[lq] = in_hole(s, b, lq);
    }
    return pc_algo_poisson_solve_membrane(b->bw, b->bh, unknown, v, NULL, err);
}

pc_status pc_nl_blend_seams(nl_solver *s, const nl_copied *copied, size_t ncopied, float texture_scale, pc_error *err)
{
    size_t w = s->w, h = s->h, x0 = w, y0 = h, x1 = 0, y1 = 0, k, c, area, ch = s->ch;
    float unscale = texture_scale > 0.0f ? 0.5f / texture_scale : 0.0f;
    float *shifts = NULL, *raw = NULL, *near = NULL;
    uint8_t *n = NULL;
    size_t *queue = NULL;
    bool *seen = NULL, *unknown = NULL;
    blend_box b;
    pc_status st = PC_OK;
    if (ncopied == 0) {
        return PC_OK;
    }
    for (k = 0; k < ncopied; k++) {
        size_t px = copied[k].p % w, py = copied[k].p / w;
        x0 = px < x0 ? px : x0, y0 = py < y0 ? py : y0;
        x1 = px + 1 > x1 ? px + 1 : x1, y1 = py + 1 > y1 ? py + 1 : y1;
    }
    x0 = x0 > 0 ? x0 - 1 : 0, y0 = y0 > 0 ? y0 - 1 : 0;
    x1 = x1 + 1 < w ? x1 + 1 : w, y1 = y1 + 1 < h ? y1 + 1 : h;
    b = (blend_box){x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    area = b.bw * b.bh;
    shifts = pc_malloc_array(area, ch * sizeof *shifts);
    raw = pc_malloc_array(area, sizeof *raw);
    near = pc_malloc_array((2 * EDGE_MEDIAN + 1) * (2 * EDGE_MEDIAN + 1), sizeof *near);
    n = pc_malloc_array(area, 1);
    queue = pc_malloc_array(area, sizeof *queue);
    seen = pc_malloc_array(area, sizeof *seen);
    unknown = pc_malloc_array(area, sizeof *unknown);
    if (shifts == NULL || raw == NULL || near == NULL || n == NULL || queue == NULL || seen == NULL ||
        unknown == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (c = 0; c < ch; c++) {
        if ((st = blend_channel(s, copied, ncopied, &b, c, unscale, &shifts[c * area], n, raw, near, queue, seen,
                                unknown, err)) != PC_OK) {
            goto cleanup;
        }
    }
    for (k = 0; k < ncopied; k++) {
        size_t lp = local_of(s, &b, copied[k].p);
        for (c = 0; c < ch; c++) {
            s->img[copied[k].p * s->cha + c] += shifts[c * area + lp];
        }
    }
cleanup:
    free(shifts), free(raw), free(near), free(n), free(queue), free(seen), free(unknown);
    return st;
}

typedef struct relax_job {
    const nl_solver *s;
    const size_t *cells;
    const float *delta, *div;
    float *next;
} relax_job;

static pc_status relax_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const relax_job *j = ctx;
    size_t w = j->s->w, h = j->s->h, ch = j->s->ch, k, q, c;
    (void)chunk;
    for (k = begin; k < end; k++) {
        size_t p = j->cells[k];
        int32_t x = (int32_t)(p % w), y = (int32_t)(p / w);
        float deg = 0.0f, *out = &j->next[k * ch];
        memset(out, 0, ch * sizeof *out);
        for (q = 0; q < 4; q++) {
            int32_t nx = x + N4[q][0], ny = y + N4[q][1];
            size_t qi;
            if (nx < 0 || ny < 0 || (size_t)nx >= w || (size_t)ny >= h) {
                continue;
            }
            qi = (size_t)ny * w + (size_t)nx;
            deg += 1.0f;
            for (c = 0; c < ch; c++) {
                out[c] += j->delta[qi * ch + c];
            }
        }
        for (c = 0; c < ch; c++) {
            float cur = j->delta[p * ch + c];
            out[c] = cur + OMEGA * ((out[c] - j->div[p * ch + c]) / pc_rs_maxf_k(deg, 1.0f) - cur);
        }
    }
    return PC_OK;
}

/* cell(x, y) as an index, false outside the grid. */
static bool cell_at(size_t w, size_t h, int64_t x, int64_t y, size_t *out)
{
    if (x < 0 || y < 0 || (size_t)x >= w || (size_t)y >= h) {
        return false;
    }
    *out = (size_t)y * w + (size_t)x;
    return true;
}

pc_status pc_nl_fuse_seams(nl_solver *s, const nl_copied *copied, size_t ncopied, float texture_scale, size_t iters,
                           pc_error *err)
{
    static const int32_t FW[2][2] = {{1, 0}, {0, 1}};
    size_t w = s->w, h = s->h, cha = s->cha, ch = s->ch, n = w * h, k, c, q, it, half, nh[2] = {0, 0};
    float unscale = texture_scale > 0.0f ? 0.5f / texture_scale : 0.0f, e[PC_ALGO_HEAL_MAX_CH];
    size_t *src = pc_malloc_array(n, sizeof *src), *halves[2] = {NULL, NULL};
    float *srcc = pc_malloc_array(n, sizeof *srcc), *div = pc_calloc_array(n * ch, sizeof *div);
    float *delta = NULL, *next = NULL;
    bool *seams = NULL, *band = NULL, any = false;
    pc_status st = PC_OK;
    if (src == NULL || srcc == NULL || div == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (k = 0; k < n; k++) {
        src[k] = SIZE_MAX, srcc[k] = INFINITY;
    }
    for (k = 0; k < ncopied; k++) {
        src[copied[k].p] = copied[k].s, srcc[copied[k].p] = copied[k].c;
    }
    for (k = 0; k < ncopied; k++) {
        size_t p = copied[k].p, sp = copied[k].s;
        int64_t px = (int64_t)(p % w), py = (int64_t)(p / w);
        float texture = (s->img[p * cha + ch] + s->img[p * cha + ch + 1]) * unscale, cp = copied[k].c;
        for (q = 0; q < 2; q++) {
            int64_t dx = FW[q][0], dy = FW[q][1];
            size_t qi, sq, fp, fq, a, bb;
            bool has_p, has_q;
            float cq, sum = -0.0f;
            if (!cell_at(w, h, px + dx, py + dy, &qi)) {
                continue;
            }
            sq = src[qi], cq = srcc[qi];
            if (sq == SIZE_MAX) {
                continue;
            }
            /* The step each side's source predicts (p's source one cell on, q's one cell back). */
            has_p = cell_at(w, h, (int64_t)(sp % w) + dx, (int64_t)(sp / w) + dy, &fp);
            if (has_p && fp == sq) {
                continue;
            }
            has_q = cell_at(w, h, (int64_t)(sq % w) - dx, (int64_t)(sq / w) - dy, &fq);
            if (has_p && has_q) {
                if (cp <= cq) {
                    a = sp, bb = fp;
                } else {
                    a = fq, bb = sq;
                }
            } else if (has_p) {
                a = sp, bb = fp;
            } else if (has_q) {
                a = fq, bb = sq;
            } else {
                continue;
            }
            for (c = 0; c < ch; c++) {
                e[c] = (s->img[bb * cha + c] - s->img[a * cha + c]) - (s->img[qi * cha + c] - s->img[p * cha + c]);
                sum += fabsf(e[c]);
            }
            if (sum / (float)ch <= SEAM_CONTRAST * texture) {
                continue;
            }
            for (c = 0; c < ch; c++) {
                div[p * ch + c] += e[c];
                div[qi * ch + c] -= e[c];
            }
            any = true;
        }
    }
    if (!any) {
        goto cleanup;
    }
    seams = pc_malloc_array(n, sizeof *seams);
    if (seams == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (k = 0; k < n; k++) {
        seams[k] = false;
        for (c = 0; c < ch; c++) {
            seams[k] = seams[k] || div[k * ch + c] != 0.0f;
        }
    }
    if ((st = pc_algo_remove_dilate(w, h, seams, n, FUSE_BAND, &band, err)) != PC_OK) {
        goto cleanup;
    }
    for (half = 0; half < 2; half++) {
        halves[half] = pc_malloc_array(ncopied > 0 ? ncopied : 1, sizeof *halves[half]);
        if (halves[half] == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
        for (k = 0; k < ncopied; k++) {
            size_t p = copied[k].p;
            if (band[p] && (p % w + p / w) % 2 == half) {
                halves[half][nh[half]++] = p;
            }
        }
    }
    delta = pc_calloc_array(n * ch, sizeof *delta);
    next = pc_malloc_array(ncopied > 0 ? ncopied * ch : 1, sizeof *next);
    if (delta == NULL || next == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (it = 0; it < iters; it++) {
        if (pc_interrupt_cancelled(s->ctl)) {
            goto cleanup;
        }
        for (half = 0; half < 2; half++) {
            relax_job j = {s, halves[half], delta, div, next};
            (void)pc_algo_par_for(nh[half], 256, relax_run, &j);
            for (k = 0; k < nh[half]; k++) {
                memcpy(&delta[halves[half][k] * ch], &next[k * ch], ch * sizeof *delta);
            }
        }
    }
    for (k = 0; k < ncopied; k++) {
        for (c = 0; c < ch; c++) {
            s->img[copied[k].p * cha + c] += delta[copied[k].p * ch + c];
        }
    }
cleanup:
    free(src), free(srcc), free(div), free(delta), free(next), free(seams), free(band);
    free(halves[0]), free(halves[1]);
    return st;
}
