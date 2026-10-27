/* Select Subject, classical and heuristic (port of crates/algo/src/segment/subject.rs).
 *
 * SLIC superpixels; per-superpixel saliency from boundary connectivity and background-weighted
 * contrast (Zhu, Liang, Wei & Sun, CVPR 2014) combined by a least-squares optimisation; blended
 * with frequency-tuned saliency (Achanta et al., CVPR 2009); Otsu's threshold seeds a GrabCut. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define SIGMA_CLR 10.0f
#define SIGMA_SPA 0.25f

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

/* Adjacency lists in the reference's insertion order (the Gauss-Seidel sums depend on it). */
typedef struct adjl {
    size_t *v;
    size_t n, cap;
} adjl;

static bool adj_push(adjl *a, size_t q)
{
    if (a->n == a->cap) {
        void *g = pc_grow_array(a->v, &a->cap, a->n + 1, sizeof(size_t));
        if (g == NULL) {
            return false;
        }
        a->v = g;
    }
    a->v[a->n++] = q;
    return true;
}

static bool adj_link(adjl *adj, size_t a, size_t b)
{
    size_t i;
    if (a == b) {
        return true;
    }
    for (i = 0; i < adj[a].n; i++) {
        if (adj[a].v[i] == b) {
            return true;
        }
    }
    return adj_push(&adj[a], b) && adj_push(&adj[b], a);
}

/* BinaryHeap<Item(f32, usize)> with Item ordered by reversed total_cmp of the distance only, so
 * the pop order of ties follows Rust's sift algorithm exactly. */
typedef struct item {
    float d;
    size_t u;
} item;

typedef struct heap {
    item *d;
    size_t n, cap;
} heap;

static int32_t tkey(float f)
{
    int32_t x;
    memcpy(&x, &f, 4);
    return x ^ (int32_t)(((uint32_t)(x >> 31)) >> 1);
}

/* Rust's `a <= b` for Item: b.0.total_cmp(a.0) is not Greater. */
static bool h_le(item a, item b)
{
    return tkey(b.d) <= tkey(a.d);
}

static void h_sift_up(heap *h, size_t start, size_t pos)
{
    item e = h->d[pos];
    while (pos > start) {
        size_t parent = (pos - 1) / 2;
        if (h_le(e, h->d[parent])) {
            break;
        }
        h->d[pos] = h->d[parent];
        pos = parent;
    }
    h->d[pos] = e;
}

static bool h_push(heap *h, item x)
{
    if (h->n == h->cap) {
        void *g = pc_grow_array(h->d, &h->cap, h->n + 1, sizeof *h->d);
        if (g == NULL) {
            return false;
        }
        h->d = g;
    }
    h->d[h->n++] = x;
    h_sift_up(h, 0, h->n - 1);
    return true;
}

static item h_pop(heap *h)
{
    item it = h->d[--h->n];
    if (h->n > 0) {
        size_t end = h->n, pos = 0, child = 1;
        item e = h->d[0];
        h->d[0] = it;
        it = e;
        e = h->d[0];
        while (end >= 2 && child <= end - 2) {
            child += h_le(h->d[child], h->d[child + 1]) ? 1 : 0;
            h->d[pos] = h->d[child];
            pos = child;
            child = 2 * pos + 1;
        }
        if (child == end - 1) {
            h->d[pos] = h->d[child];
            pos = child;
        }
        h->d[pos] = e;
        h_sift_up(h, 0, pos);
    }
    return it;
}

static bool dijkstra(size_t src, const adjl *adj, const float (*mean)[3], float *dist, size_t n, heap *hp)
{
    size_t i;
    item s0;
    for (i = 0; i < n; i++) {
        dist[i] = INFINITY;
    }
    dist[src] = 0.0f;
    hp->n = 0;
    s0.d = 0.0f;
    s0.u = src;
    if (!h_push(hp, s0)) {
        return false;
    }
    while (hp->n > 0) {
        item it = h_pop(hp);
        size_t u = it.u, k;
        if (it.d > dist[u]) {
            continue;
        }
        for (k = 0; k < adj[u].n; k++) {
            size_t v = adj[u].v[k];
            float nd = it.d + sqrtf(d2(mean[u], mean[v]));
            if (nd < dist[v]) {
                item e;
                dist[v] = nd;
                e.d = nd;
                e.u = v;
                if (!h_push(hp, e)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static pc_status blur_lab(const float (*src)[3], size_t w, size_t h, float (**out)[3], pc_error *err)
{
    size_t n = w * h, x, y, pass;
    float (*cur)[3] = pc_malloc_array(n ? n : 1, sizeof *cur),
          (*tmp)[3] = pc_malloc_array(n ? n : 1, sizeof *tmp);
    int k;
    if (cur == NULL || tmp == NULL) {
        free(cur);
        free(tmp);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for saliency");
    }
    memcpy(cur, src, n * sizeof *cur);
    for (pass = 0; pass < 2; pass++) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const float *l = cur[y * w + (x > 0 ? x - 1 : 0)], *c = cur[y * w + x];
                const float *r = cur[y * w + (x + 1 < w ? x + 1 : w - 1)];
                for (k = 0; k < 3; k++) {
                    tmp[y * w + x][k] = (l[k] + 2.0f * c[k] + r[k]) / 4.0f;
                }
            }
        }
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                const float *u = tmp[(y > 0 ? y - 1 : 0) * w + x], *c = tmp[y * w + x];
                const float *d = tmp[(y + 1 < h ? y + 1 : h - 1) * w + x];
                for (k = 0; k < 3; k++) {
                    cur[y * w + x][k] = (u[k] + 2.0f * c[k] + d[k]) / 4.0f;
                }
            }
        }
    }
    free(tmp);
    *out = cur;
    return PC_OK;
}

pc_status pc_algo_subject_saliency(const pc_algo_rgb_image *img, float **out_p, pc_error *err)
{
    size_t w = img->w, h = img->h, npx = w * h, n = 0, x, y, p, q, i, it, target;
    float (*lab)[3] = NULL, (*mean)[3] = NULL, (*pos)[2] = NULL, (*blurred)[3] = NULL;
    double (*meand)[3] = NULL, (*posd)[2] = NULL, *cnt = NULL, mean_all_d[3] = {0.0, 0.0, 0.0};
    bool *on_border = NULL;
    adjl *adj = NULL;
    float *wbg = NULL, *geo = NULL, *wctr = NULL, *wfg = NULL, *s = NULL, *out = NULL, mean_all[3];
    float **wij = NULL;
    float mx, ftmax = 0.0f, smin = FLT_MAX, smax = -FLT_MAX, srange, lo = FLT_MAX, hi = -FLT_MAX, rr;
    heap hp = {NULL, 0, 0};
    pc_algo_superpixels sp = {0, 0, NULL, 0, 0.0f};
    pc_status st = PC_OK;
    lab = pc_malloc_array(npx ? npx : 1, sizeof *lab);
    if (lab == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for saliency");
    }
    for (i = 0; i < npx; i++) {
        pc_algo_slic_rgb_to_lab(img->px[i], lab[i]);
    }
    target = npx / 150;
    target = target < 16 ? 16 : (target > 400 ? 400 : target);
    st = pc_algo_slic_lab((const float (*)[3])lab, w, h, target, 10.0f, 8, &sp, err);
    if (st != PC_OK) {
        goto done;
    }
    n = sp.count;
    meand = pc_calloc_array(n ? n : 1, sizeof *meand);
    posd = pc_calloc_array(n ? n : 1, sizeof *posd);
    cnt = pc_calloc_array(n ? n : 1, sizeof(double));
    on_border = pc_calloc_array(n ? n : 1, sizeof(bool));
    mean = pc_malloc_array(n ? n : 1, sizeof *mean);
    pos = pc_malloc_array(n ? n : 1, sizeof *pos);
    adj = pc_calloc_array(n ? n : 1, sizeof *adj);
    wbg = pc_calloc_array(n ? n : 1, sizeof(float));
    geo = pc_malloc_array(n ? n : 1, sizeof(float));
    wctr = pc_calloc_array(n ? n : 1, sizeof(float));
    wfg = pc_calloc_array(n ? n : 1, sizeof(float));
    s = pc_calloc_array(n ? n : 1, sizeof(float));
    wij = pc_calloc_array(n ? n : 1, sizeof(float *));
    out = pc_malloc_array(npx ? npx : 1, sizeof(float));
    if (meand == NULL || posd == NULL || cnt == NULL || on_border == NULL || mean == NULL || pos == NULL ||
        adj == NULL || wbg == NULL || geo == NULL || wctr == NULL || wfg == NULL || s == NULL ||
        wij == NULL || out == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for saliency");
        goto done;
    }
    /* Region statistics. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t l = sp.labels[y * w + x];
            const float *c = lab[y * w + x];
            int k;
            for (k = 0; k < 3; k++) {
                meand[l][k] += (double)c[k];
            }
            posd[l][0] += (double)x / (double)w;
            posd[l][1] += (double)y / (double)h;
            cnt[l] += 1.0;
            if (x == 0 || y == 0 || x + 1 == w || y + 1 == h) {
                on_border[l] = true;
            }
        }
    }
    for (p = 0; p < n; p++) {
        double c = pc_rs_max_k(cnt[p], 1.0);
        int k;
        for (k = 0; k < 3; k++) {
            mean[p][k] = pc_f64_to_f32(meand[p][k] / c);
        }
        pos[p][0] = pc_f64_to_f32(posd[p][0] / c);
        pos[p][1] = pc_f64_to_f32(posd[p][1] / c);
    }
    /* Adjacency. */
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t l = sp.labels[y * w + x];
            if ((x + 1 < w && !adj_link(adj, l, sp.labels[y * w + x + 1])) ||
                (y + 1 < h && !adj_link(adj, l, sp.labels[(y + 1) * w + x]))) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for saliency");
                goto done;
            }
        }
    }
    /* Boundary connectivity from all-pairs geodesic distances (Dijkstra per region). */
    for (p = 0; p < n; p++) {
        float area = 0.0f, len = 0.0f, bnd;
        if (!dijkstra(p, adj, (const float (*)[3])mean, geo, n, &hp)) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for saliency");
            goto done;
        }
        for (q = 0; q < n; q++) {
            float sv = expf(-geo[q] * geo[q] / (2.0f * SIGMA_CLR * SIGMA_CLR));
            area += sv;
            if (on_border[q]) {
                len += sv;
            }
        }
        bnd = len / sqrtf(pc_rs_maxf_k(area, 1e-6f));
        wbg[p] = 1.0f - expf(-bnd * bnd / 2.0f);
    }
    /* Background-weighted contrast. */
    for (p = 0; p < n; p++) {
        float sum = 0.0f;
        for (q = 0; q < n; q++) {
            float dx = pos[p][0] - pos[q][0], dy = pos[p][1] - pos[q][1];
            float wspa = expf(-(dx * dx + dy * dy) / (2.0f * SIGMA_SPA * SIGMA_SPA));
            sum += sqrtf(d2(mean[p], mean[q])) * wspa * wbg[q];
        }
        wctr[p] = sum;
    }
    mx = 0.0f;
    for (p = 0; p < n; p++) {
        mx = pc_rs_maxf(mx, wctr[p]);
    }
    mx = pc_rs_maxf_k(mx, 1e-6f);
    for (p = 0; p < n; p++) {
        wfg[p] = wctr[p] / mx;
    }
    /* Saliency optimisation (Gauss-Seidel). */
    for (p = 0; p < n; p++) {
        size_t k;
        wij[p] = pc_malloc_array(adj[p].n ? adj[p].n : 1, sizeof(float));
        if (wij[p] == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for saliency");
            goto done;
        }
        for (k = 0; k < adj[p].n; k++) {
            float dd = sqrtf(d2(mean[p], mean[adj[p].v[k]]));
            wij[p][k] = expf(-(dd * dd) / (2.0f * SIGMA_CLR * SIGMA_CLR)) + 0.1f;
        }
    }
    memcpy(s, wfg, n * sizeof(float));
    for (it = 0; it < 200; it++) {
        for (p = 0; p < n; p++) {
            float num = wfg[p], den = wbg[p] + wfg[p];
            size_t k;
            for (k = 0; k < adj[p].n; k++) {
                num += wij[p][k] * s[adj[p].v[k]];
                den += wij[p][k];
            }
            s[p] = num / pc_rs_maxf_k(den, 1e-6f);
        }
    }
    /* Frequency-tuned saliency. */
    for (i = 0; i < npx; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            mean_all_d[k] += (double)lab[i][k];
        }
    }
    for (i = 0; i < 3; i++) {
        mean_all[i] = pc_f64_to_f32(mean_all_d[i] / (double)npx);
    }
    st = blur_lab((const float (*)[3])lab, w, h, &blurred, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < npx; i++) {
        float ft = sqrtf(d2(blurred[i], mean_all));
        out[i] = ft; /* ft kept in out until the final blend */
        ftmax = pc_rs_maxf(ftmax, ft);
    }
    ftmax = pc_rs_maxf_k(ftmax, 1e-6f);
    for (p = 0; p < n; p++) {
        smin = pc_rs_minf(smin, s[p]);
        smax = pc_rs_maxf(smax, s[p]);
    }
    srange = pc_rs_maxf_k(smax - smin, 1e-6f);
    for (i = 0; i < npx; i++) {
        out[i] = 0.75f * (s[sp.labels[i]] - smin) / srange + 0.25f * out[i] / ftmax;
        lo = pc_rs_minf(lo, out[i]);
        hi = pc_rs_maxf(hi, out[i]);
    }
    rr = pc_rs_maxf_k(hi - lo, 1e-6f);
    for (i = 0; i < npx; i++) {
        out[i] = (out[i] - lo) / rr;
    }
    *out_p = out;
    out = NULL;
done:
    if (adj != NULL) {
        for (p = 0; p < n; p++) {
            free(adj[p].v);
        }
    }
    if (wij != NULL) {
        for (p = 0; p < n; p++) {
            free(wij[p]);
        }
    }
    free(adj);
    free(wij);
    free(lab);
    free(mean);
    free(pos);
    free(blurred);
    free(meand);
    free(posd);
    free(cnt);
    free(on_border);
    free(wbg);
    free(geo);
    free(wctr);
    free(wfg);
    free(s);
    free(out);
    free(hp.d);
    pc_algo_superpixels_fini(&sp);
    return st;
}

float pc_algo_subject_otsu(const float *v, size_t n)
{
    uint64_t hist[256];
    double total = (double)n, sum = -0.0, wb = 0.0, sb = 0.0, best = -1.0;
    size_t i, bt = 0, t;
    memset(hist, 0, sizeof hist);
    for (i = 0; i < n; i++) {
        size_t b = pc_f32_to_size(roundf(pc_clampf(v[i], 0.0f, 1.0f) * 255.0f));
        hist[b < 256 ? b : 255]++;
    }
    for (t = 0; t < 256; t++) {
        sum += (double)t * (double)hist[t];
    }
    for (t = 0; t < 256; t++) {
        double wf, mb, mf, between;
        wb += (double)hist[t];
        if (wb == 0.0) {
            continue;
        }
        wf = total - wb;
        if (wf == 0.0) {
            break;
        }
        sb += (double)t * (double)hist[t];
        mb = sb / wb;
        mf = (sum - sb) / wf;
        between = wb * wf * (mb - mf) * (mb - mf);
        if (between > best) {
            best = between;
            bt = t;
        }
    }
    return ((float)bt + 0.5f) / 255.0f;
}

/* Fills the holes of `mask` (4-connected background not touching the border) by what they look
 * like: a hole up to speck_frac of the mask's area always fills; a larger one up to hole_frac only
 * when its pixels fit the foreground model better than the background's (crate
 * fill_holes_by_model). `out` is a caller buffer of w*h. */
static pc_status fill_holes_by_model(const bool *mask, const pc_algo_rgb_image *img,
                                     const pc_algo_gmm *fg, const pc_algo_gmm *bg, float speck_frac,
                                     float hole_frac, bool *out, pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, i, x, y, area = 0, max_speck, max_hole, nhs;
    uint32_t *hl = NULL;
    size_t *hs = NULL;
    bool *inv = NULL, *touches = NULL;
    double *cf = NULL, *cb = NULL;
    pc_status st;
    memcpy(out, mask, n * sizeof(bool));
    for (i = 0; i < n; i++) {
        area += mask[i];
    }
    max_speck = (size_t)((float)area * speck_frac);
    max_hole = (size_t)((float)area * hole_frac);
    inv = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (inv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory filling holes");
    }
    for (i = 0; i < n; i++) {
        inv[i] = !mask[i];
    }
    st = pc_algo_segment_components4(inv, w, h, &hl, &hs, &nhs, err);
    free(inv);
    if (st != PC_OK) {
        return st;
    }
    touches = pc_calloc_array(nhs ? nhs : 1, sizeof(bool));
    cf = pc_calloc_array(nhs ? nhs : 1, sizeof(double));
    cb = pc_calloc_array(nhs ? nhs : 1, sizeof(double));
    if (touches == NULL || cf == NULL || cb == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory filling holes");
        goto done;
    }
    for (x = 0; x < w; x++) {
        touches[hl[x]] = true;
        touches[hl[(h - 1) * w + x]] = true;
    }
    for (y = 0; y < h; y++) {
        touches[hl[y * w]] = true;
        touches[hl[y * w + w - 1]] = true;
    }
    for (i = 0; i < n; i++) {
        size_t l = hl[i];
        if (l == 0 || touches[l] || hs[l] > max_hole || hs[l] <= max_speck) {
            continue;
        }
        cf[l] += (double)pc_algo_gmm_neg_log(fg, img->px[i]);
        cb[l] += (double)pc_algo_gmm_neg_log(bg, img->px[i]);
    }
    for (i = 0; i < n; i++) {
        size_t l = hl[i];
        if (l == 0 || touches[l] || hs[l] > max_hole) {
            continue;
        }
        if (hs[l] <= max_speck || cf[l] <= cb[l]) {
            out[i] = true;
        }
    }
done:
    free(touches), free(cf), free(cb), free(hl), free(hs);
    return st;
}

static int cost_cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* A second GrabCut whose background model only knows the backdrop (crate backdrop_pass). On
 * success *produced is true and low_out/fg_out/bg_out are written; otherwise *produced is false
 * and the caller keeps the first pass's result. */
static pc_status backdrop_pass(const pc_algo_rgb_image *img, const bool *blob, bool *low_out,
                               pc_algo_gmm *fg_out, pc_algo_gmm *bg_out, bool *produced, pc_error *err)
{
    size_t w = img->w, h = img->h, n = w * h, i, x, y;
    size_t x0 = w, y0 = h, x1 = 0, y1 = 0, margin, bx0, by0, bx1, by1, frame, dim, ns = 0;
    float (*samples)[3] = NULL, (*ss)[3] = NULL;
    float *fcosts = NULL, limit;
    size_t nss = 0;
    uint8_t *trimap = NULL;
    bool *cut = NULL, some = false, found = false, blob_present = false;
    pc_algo_gmm backdrop = {NULL, 0, 0.0f}, fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f};
    pc_status st = PC_OK;
    *produced = false;
    for (i = 0; i < n; i++) {
        if (blob[i]) {
            x = i % w, y = i / w;
            x0 = x < x0 ? x : x0, y0 = y < y0 ? y : y0;
            x1 = x > x1 ? x : x1, y1 = y > y1 ? y : y1;
            blob_present = true;
        }
    }
    if (!blob_present || x1 < x0 || y1 < y0) {
        return PC_OK;
    }
    dim = (x1 - x0 + 1) > (y1 - y0 + 1) ? (x1 - x0 + 1) : (y1 - y0 + 1);
    margin = (size_t)ceilf((float)dim * 0.15f);
    if (margin < 4) {
        margin = 4;
    }
    bx0 = x0 > margin ? x0 - margin : 0;
    by0 = y0 > margin ? y0 - margin : 0;
    bx1 = (x1 + margin) < (w - 1) ? (x1 + margin) : (w - 1);
    by1 = (y1 + margin) < (h - 1) ? (y1 + margin) : (h - 1);
    frame = (size_t)roundf((float)(w < h ? w : h) * 0.06f);
    if (frame < 2) {
        frame = 2;
    }
    samples = pc_malloc_array(n ? n : 1, sizeof *samples);
    if (samples == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Select Subject");
    }
#define IN_FRAME(xx, yy) ((xx) < frame || (yy) < frame || (xx) + frame >= w || (yy) + frame >= h)
#define NEAR_BLOB(xx, yy) ((xx) >= bx0 && (xx) <= bx1 && (yy) >= by0 && (yy) <= by1)
    for (i = 0; i < n; i++) {
        x = i % w, y = i / w;
        if (IN_FRAME(x, y) && !NEAR_BLOB(x, y)) {
            samples[ns][0] = img->px[i][0], samples[ns][1] = img->px[i][1], samples[ns][2] = img->px[i][2];
            ns++;
        }
    }
    st = pc_algo_segment_subsample((const float (*)[3])samples, ns, 40000, &ss, &nss, err);
    if (st != PC_OK) {
        goto done;
    }
    st = pc_algo_gmm_fit((const float (*)[3])ss, nss, PC_ALGO_GRABCUT_K, PC_ALGO_GMM_DEFAULT_REG, &backdrop,
                         &some, err);
    if (st != PC_OK) {
        goto done;
    }
    if (!some || ns == 0) {
        goto done;
    }
    fcosts = pc_malloc_array(ns, sizeof *fcosts);
    trimap = pc_malloc_array(n ? n : 1, 1);
    cut = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (fcosts == NULL || trimap == NULL || cut == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Select Subject");
        goto done;
    }
    for (i = 0; i < ns; i++) {
        fcosts[i] = pc_algo_gmm_neg_log(&backdrop, samples[i]);
    }
    qsort(fcosts, ns, sizeof *fcosts, cost_cmp);
    limit = fcosts[ns * 99 / 100] + 4.0f;
    for (i = 0; i < n; i++) {
        x = i % w, y = i / w;
        if (IN_FRAME(x, y) && !NEAR_BLOB(x, y)) {
            trimap[i] = PC_ALGO_GRABCUT_BG;
        } else if (blob[i] || pc_algo_gmm_neg_log(&backdrop, img->px[i]) > limit) {
            trimap[i] = PC_ALGO_GRABCUT_PR_FG;
        } else {
            trimap[i] = PC_ALGO_GRABCUT_PR_BG;
        }
    }
    st = pc_algo_grabcut(img, trimap, 4, &fg, &bg, &some, err);
    if (st != PC_OK || !some) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        cut[i] = trimap[i] == PC_ALGO_GRABCUT_PR_FG || trimap[i] == PC_ALGO_GRABCUT_FG;
    }
    st = pc_algo_segment_keep_seeded(cut, blob, w, h, low_out, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < n && !found; i++) {
        found = low_out[i];
    }
    if (found) {
        *fg_out = fg, *bg_out = bg;
        fg.comps = NULL, bg.comps = NULL; /* ownership moved to the caller */
        *produced = true;
    }
#undef IN_FRAME
#undef NEAR_BLOB
done:
    pc_algo_gmm_fini(&backdrop);
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    free(samples), free(ss), free(fcosts), free(trimap), free(cut);
    return st;
}

pc_status pc_algo_subject_select(const pc_algo_sampler *s, pc_rect canvas, pc_algo_region *out, pc_error *err)
{
    size_t step, w, h, n, x, y;
    pc_algo_rgb_image img = {0, 0, NULL};
    float *sal = NULL, t;
    uint8_t *trimap = NULL;
    bool *low = NULL, *clean = NULL, *bp_low = NULL, *filled = NULL, any_fg = false, some = false, any = false;
    bool produced = false;
    pc_algo_gmm fg = {NULL, 0, 0.0f}, bg = {NULL, 0, 0.0f}, fg2 = {NULL, 0, 0.0f}, bg2 = {NULL, 0, 0.0f};
    pc_status st;
    out->bbox = PC_RECT_EMPTY;
    out->mask = NULL;
    out->len = 0;
    if (pc_rect_width(canvas) < 4 || pc_rect_height(canvas) < 4) {
        return PC_OK;
    }
    step = pc_algo_segment_scale_for(canvas, PC_ALGO_SUBJECT_WORK_PX);
    st = pc_algo_sampler_rgb_scaled(s, canvas, step, &img, err);
    if (st != PC_OK) {
        return st;
    }
    w = img.w;
    h = img.h;
    n = w * h;
    st = pc_algo_subject_saliency(&img, &sal, err);
    if (st != PC_OK) {
        goto done;
    }
    t = pc_clampf(pc_algo_subject_otsu(sal, n), 0.15f, 0.85f);
    trimap = pc_malloc_array(n ? n : 1, 1);
    low = pc_malloc_array(n ? n : 1, sizeof(bool));
    clean = pc_malloc_array(n ? n : 1, sizeof(bool));
    bp_low = pc_malloc_array(n ? n : 1, sizeof(bool));
    filled = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (trimap == NULL || low == NULL || clean == NULL || bp_low == NULL || filled == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Select Subject");
        goto done;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            size_t i = y * w + x;
            bool border = x == 0 || y == 0 || x + 1 == w || y + 1 == h;
            if (sal[i] >= t) {
                any_fg = true;
                trimap[i] = PC_ALGO_GRABCUT_PR_FG;
            } else if (border || sal[i] < 0.4f * t) {
                trimap[i] = PC_ALGO_GRABCUT_BG;
            } else {
                trimap[i] = PC_ALGO_GRABCUT_PR_BG;
            }
        }
    }
    if (!any_fg) {
        goto done;
    }
    st = pc_algo_grabcut(&img, trimap, 6, &fg, &bg, &some, err);
    if (st != PC_OK || !some) {
        goto done;
    }
    for (x = 0; x < n; x++) {
        low[x] = trimap[x] == PC_ALGO_GRABCUT_PR_FG || trimap[x] == PC_ALGO_GRABCUT_FG;
    }
    st = pc_algo_segment_clean_mask(low, w, h, 0.2f, 0.0f, clean, err);
    if (st != PC_OK) {
        goto done;
    }
    for (x = 0; x < n && !any; x++) {
        any = clean[x];
    }
    if (!any) {
        goto done;
    }
    /* A second GrabCut whose background model only knows the backdrop; keeps subject parts whose
     * colour is close to the backdrop that the first pass explained away (#2788). */
    st = backdrop_pass(&img, clean, bp_low, &fg2, &bg2, &produced, err);
    if (st != PC_OK) {
        goto done;
    }
    if (produced) {
        memcpy(clean, bp_low, n * sizeof(bool));
        pc_algo_gmm_fini(&fg);
        pc_algo_gmm_fini(&bg);
        fg = fg2, bg = bg2;
        fg2.comps = NULL, bg2.comps = NULL;
    }
    /* Fill holes that fit the foreground better than the backdrop; keep enclosed backdrop open. */
    st = fill_holes_by_model(clean, &img, &fg, &bg, 0.002f, 0.05f, filled, err);
    if (st != PC_OK) {
        goto done;
    }
    st = pc_algo_segment_finish_region(s, canvas, step, filled, w, h, &fg, &bg, out, err);
done:
    pc_algo_gmm_fini(&fg);
    pc_algo_gmm_fini(&bg);
    pc_algo_gmm_fini(&fg2);
    pc_algo_gmm_fini(&bg2);
    pc_algo_rgb_image_fini(&img);
    free(sal);
    free(trimap);
    free(low);
    free(clean);
    free(bp_low);
    free(filled);
    return st;
}
