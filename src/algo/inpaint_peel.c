/* The completion's coarsest-level initialisation: the onion peel (inpaint::onion_peel); see
 * inpaint_int.h. The search is exhaustive but gives most sources up early (the target's known
 * pixels are gathered once, the search starts from where filled neighbours took their sources,
 * and a source's error stops being summed once it can only lose), finding exactly the source a
 * plain scan in order finds. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/inpaint_int.h"
#include "base/mem.h"
#include "base/rmath.h"

#define NONE UINT32_MAX
/* Sources summed at once (the reference's LANES). */
#define LANES 4

float pc_ip_least_sum(float v, float n, bool strict)
{
    float e;
    if (!isfinite(v) || !isfinite(n) || n <= 0.0f) {
        return INFINITY;
    }
    e = pc_rs_maxf_k(v * n, 0.0f);
#define HOLDS(x) (strict ? (x) / n > v : (x) / n >= v)
    while (e > 0.0f && HOLDS(nextafterf(e, -INFINITY))) {
        e = nextafterf(e, -INFINITY);
    }
    while (!HOLDS(e) && e < INFINITY) {
        e = nextafterf(e, INFINITY);
    }
#undef HOLDS
    return e;
}

/* The best source so far: error per sample, place in the sources, and the sums of squared errors
 * from which sum / n is above v (worse) and at least v (equal). */
typedef struct best {
    float v;
    size_t j;
    float worse, equal;
} best;

typedef struct peel_job {
    size_t w, h, ch;
    int32_t r;
    const float *img;
    const bool *known;
    const uint32_t *took, *index;
    const pc_ip_pt *sources;
    size_t nsources;
    const size_t *front;
    float *vals;    /* front * ch */
    uint32_t *picks; /* front */
    const pc_interrupt *ctl;
} peel_job;

/* sum (t - s)^2 over the target's known pixels against the source centred at pixel `base`, in
 * order; false once it reaches limit. */
static bool known_sse(const peel_job *j, const int64_t *offs, const float *tv, size_t no, int64_t base,
                      float limit, float *out)
{
    size_t ch = j->ch, i, c;
    float e = 0.0f;
    for (i = 0; i < no; i++) {
        const float *s = &j->img[(size_t)(base + offs[i]) * ch], *t = &tv[i * ch];
        for (c = 0; c < ch; c++) {
            float d = t[c] - s[c];
            e += d * d;
        }
        if (e >= limit) {
            return false;
        }
    }
    *out = e;
    return true;
}

/* known_sse of LANES sources at once, each against its own limit. */
static void known_sse_lanes(const peel_job *j, const int64_t *offs, const float *tv, size_t no,
                            const int64_t *bases, const float *limits, bool *ok, float *e)
{
    size_t ch = j->ch, i, c, l;
    for (l = 0; l < LANES; l++) {
        e[l] = 0.0f;
    }
    for (i = 0; i < no; i++) {
        const float *t = &tv[i * ch];
        bool all;
        for (l = 0; l < LANES; l++) {
            const float *s = &j->img[(size_t)(bases[l] + offs[i]) * ch];
            for (c = 0; c < ch; c++) {
                float d = t[c] - s[c];
                e[l] += d * d;
            }
        }
        all = true;
        for (l = 0; l < LANES; l++) {
            all = all && e[l] >= limits[l];
        }
        if (all) {
            break;
        }
    }
    for (l = 0; l < LANES; l++) {
        ok[l] = e[l] < limits[l];
    }
}

static int64_t base_of(const peel_job *j, size_t k)
{
    return k < j->nsources ? (int64_t)j->sources[k].y * (int64_t)j->w + (int64_t)j->sources[k].x : 0;
}

/* The sum at which source k can't win: a tie loses to an earlier source. */
static float limit_of(size_t k, const best *b)
{
    return k > b->j ? b->equal : b->worse;
}

static void adopt(size_t k, float e, float nf, best *b)
{
    float v = e / nf;
    if (v < INFINITY && (v < b->v || (v == b->v && k < b->j))) {
        b->v = v, b->j = k;
        b->worse = pc_ip_least_sum(v, nf, true);
        b->equal = pc_ip_least_sum(v, nf, false);
    }
}

static void consider(const peel_job *j, const int64_t *offs, const float *tv, size_t no, float nf, size_t k,
                     best *b)
{
    float e;
    if (k >= j->nsources) {
        return;
    }
    if (known_sse(j, offs, tv, no, base_of(j, k), limit_of(k, b), &e)) {
        adopt(k, e, nf, b);
    }
}

static void peel_one(const peel_job *j, size_t fi, int64_t *offs, float *tv)
{
    static const int32_t NB[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    size_t i = j->front[fi], w = j->w, h = j->h, ch = j->ch, no = 0, q, k;
    int32_t x = (int32_t)(i % w), y = (int32_t)(i / w), r = j->r, dx, dy;
    best b = {INFINITY, SIZE_MAX, INFINITY, INFINITY};
    pc_ip_pt sp = {0, 0};
    size_t si;
    for (dy = -r; dy <= r; dy++) {
        for (dx = -r; dx <= r; dx++) {
            int32_t tx = x + dx, ty = y + dy;
            if (tx >= 0 && ty >= 0 && (size_t)tx < w && (size_t)ty < h && j->known[(size_t)ty * w + (size_t)tx]) {
                offs[no] = (int64_t)dy * (int64_t)w + dx;
                memcpy(&tv[no * ch], &j->img[((size_t)ty * w + (size_t)tx) * ch], ch * sizeof(float));
                no++;
            }
        }
    }
    if (no > 0) {
        float nf = (float)no;
        for (q = 0; q < 8; q++) {
            int32_t qx = x + NB[q][0], qy = y + NB[q][1], cx, cy;
            uint32_t t;
            if (qx < 0 || qy < 0 || (size_t)qx >= w || (size_t)qy >= h) {
                continue;
            }
            t = j->took[(size_t)qy * w + (size_t)qx];
            if (t == NONE || (size_t)t >= j->nsources) {
                continue;
            }
            cx = j->sources[t].x - NB[q][0], cy = j->sources[t].y - NB[q][1];
            if (cx >= 0 && cy >= 0 && (size_t)cx < w && (size_t)cy < h && j->index[(size_t)cy * w + (size_t)cx] != NONE) {
                consider(j, offs, tv, no, nf, j->index[(size_t)cy * w + (size_t)cx], &b);
            }
        }
        if (b.j == SIZE_MAX) {
            for (k = 0; k < j->nsources; k += 16) {
                consider(j, offs, tv, no, nf, k, &b);
            }
        }
        k = 0;
        while (k + LANES <= j->nsources) {
            int64_t bases[LANES];
            float limits[LANES], e[LANES];
            bool ok[LANES];
            size_t l;
            if (k % 4096 == 0 && k > 0 && pc_interrupt_cancelled(j->ctl)) {
                break;
            }
            for (l = 0; l < LANES; l++) {
                bases[l] = base_of(j, k + l);
                limits[l] = limit_of(k + l, &b);
            }
            known_sse_lanes(j, offs, tv, no, bases, limits, ok, e);
            for (l = 0; l < LANES; l++) {
                if (ok[l]) {
                    adopt(k + l, e[l], nf, &b);
                }
            }
            k += LANES;
        }
        for (; k < j->nsources; k++) {
            consider(j, offs, tv, no, nf, k, &b);
        }
    }
    if (b.j < j->nsources) {
        sp = j->sources[b.j];
    }
    si = ((size_t)sp.y * w + (size_t)sp.x) * ch;
    memcpy(&j->vals[fi * ch], &j->img[si], ch * sizeof(float));
    j->picks[fi] = b.j == SIZE_MAX ? NONE : (uint32_t)b.j;
}

static pc_status peel_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const peel_job *j = ctx;
    size_t side = (size_t)(2 * j->r + 1), fi;
    int64_t *offs = pc_malloc_array(side * side, sizeof *offs);
    float *tv = pc_malloc_array(side * side, j->ch * sizeof *tv);
    (void)chunk;
    if (offs == NULL || tv == NULL) {
        free(offs);
        free(tv);
        return PC_ERR_NOMEM;
    }
    for (fi = begin; fi < end; fi++) {
        peel_one(j, fi, offs, tv);
    }
    free(offs);
    free(tv);
    return PC_OK;
}

pc_status pc_ip_onion_peel(size_t w, size_t h, size_t ch, int32_t r, float *img, const uint8_t *px,
                           pc_ip_bounds hb, const pc_ip_pt *sources, size_t nsources, const pc_interrupt *ctl,
                           pc_error *err)
{
    size_t n = w * h, i, nfront, x, y, fi;
    bool *known = pc_malloc_array(n, sizeof *known);
    uint32_t *index = pc_malloc_array(n, sizeof *index), *took = pc_malloc_array(n, sizeof *took);
    size_t *front = pc_malloc_array(pc_ip_barea(hb) > 0 ? pc_ip_barea(hb) : 1, sizeof *front);
    float *vals = pc_malloc_array(pc_ip_barea(hb) > 0 ? pc_ip_barea(hb) : 1, ch * sizeof *vals);
    uint32_t *picks = pc_malloc_array(pc_ip_barea(hb) > 0 ? pc_ip_barea(hb) : 1, sizeof *picks);
    pc_status st = PC_OK;
    peel_job j;
    if (known == NULL || index == NULL || took == NULL || front == NULL || vals == NULL || picks == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        known[i] = px[i] == PC_ALGO_PX_KNOWN || px[i] == PC_ALGO_PX_SOURCE;
        index[i] = NONE;
        took[i] = NONE;
    }
    for (i = 0; i < nsources; i++) {
        index[(size_t)sources[i].y * w + (size_t)sources[i].x] = (uint32_t)i;
    }
    j = (peel_job){w, h, ch, r, img, known, took, index, sources, nsources, front, vals, picks, ctl};
    for (;;) {
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        nfront = 0;
        for (y = hb.y0; y < hb.y1; y++) {
            for (x = hb.x0; x < hb.x1; x++) {
                bool edge = false;
                int32_t dx, dy;
                i = y * w + x;
                if (known[i] || px[i] != PC_ALGO_PX_HOLE) {
                    continue;
                }
                for (dy = -1; dy <= 1 && !edge; dy++) {
                    for (dx = -1; dx <= 1 && !edge; dx++) {
                        int64_t nx = (int64_t)x + dx, ny = (int64_t)y + dy;
                        edge = nx >= 0 && ny >= 0 && (size_t)nx < w && (size_t)ny < h &&
                               known[(size_t)ny * w + (size_t)nx];
                    }
                }
                if (edge) {
                    front[nfront++] = i;
                }
            }
        }
        if (nfront == 0) {
            break;
        }
        if (nfront * nsources >= 4096) {
            st = pc_algo_par_for(nfront, 4, peel_run, &j);
        } else {
            st = peel_run(&j, 0, 0, nfront);
        }
        if (st != PC_OK) {
            st = pc_fail(err, st, "out of memory");
            goto cleanup;
        }
        if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
            goto cleanup;
        }
        for (fi = 0; fi < nfront; fi++) {
            i = front[fi];
            memcpy(&img[i * ch], &vals[fi * ch], ch * sizeof(float));
            known[i] = true;
            took[i] = picks[fi];
        }
    }
cleanup:
    free(known);
    free(index);
    free(took);
    free(front);
    free(vals);
    free(picks);
    return st;
}
