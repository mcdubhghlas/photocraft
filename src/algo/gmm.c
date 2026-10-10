/* Gaussian mixture colour models with full 3x3 covariances (port of
 * crates/algo/src/segment/gmm.rs): k-means++ seeding (Arthur & Vassilvitskii 2007), Lloyd
 * iterations, a few EM steps; GrabCut's hard-assignment refit. Covariances get a small ridge. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define PI_D 3.14159265358979323846

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

void pc_algo_gmm_fini(pc_algo_gmm *g)
{
    if (g == NULL) {
        return;
    }
    free(g->comps);
    g->comps = NULL;
    g->n = 0;
}

pc_status pc_algo_gmm_clone(const pc_algo_gmm *g, pc_algo_gmm *out, pc_error *err)
{
    pc_algo_gmm_component *c = pc_malloc_array(g->n ? g->n : 1, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a colour model");
    }
    if (g->n > 0) {
        memcpy(c, g->comps, g->n * sizeof *c);
    }
    out->comps = c;
    out->n = g->n;
    out->reg = g->reg;
    return PC_OK;
}

/* Component::new: false when the covariance is singular or the weight not positive. */
static bool component_new(double weight, const double mean[3], double cov[3][3], double reg,
                          pc_algo_gmm_component *out)
{
    double c[3][3], det;
    int i, j;
    memcpy(c, cov, sizeof c);
    for (i = 0; i < 3; i++) {
        c[i][i] += reg;
    }
    det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
          c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
          c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0]);
    if (det <= 1e-30 || weight <= 0.0) {
        return false;
    }
    out->inv[0][0] = (c[1][1] * c[2][2] - c[1][2] * c[2][1]) / det;
    out->inv[0][1] = (c[0][2] * c[2][1] - c[0][1] * c[2][2]) / det;
    out->inv[0][2] = (c[0][1] * c[1][2] - c[0][2] * c[1][1]) / det;
    out->inv[1][0] = (c[1][2] * c[2][0] - c[1][0] * c[2][2]) / det;
    out->inv[1][1] = (c[0][0] * c[2][2] - c[0][2] * c[2][0]) / det;
    out->inv[1][2] = (c[0][2] * c[1][0] - c[0][0] * c[1][2]) / det;
    out->inv[2][0] = (c[1][0] * c[2][1] - c[1][1] * c[2][0]) / det;
    out->inv[2][1] = (c[0][1] * c[2][0] - c[0][0] * c[2][1]) / det;
    out->inv[2][2] = (c[0][0] * c[1][1] - c[0][1] * c[1][0]) / det;
    out->log_norm = log(weight) - 0.5 * log(det) - 1.5 * log(2.0 * PI_D);
    out->weight = pc_f64_to_f32(weight);
    for (i = 0; i < 3; i++) {
        out->mean[i] = pc_f64_to_f32(mean[i]);
        for (j = 0; j < 3; j++) {
            out->cov[i][j] = pc_f64_to_f32(c[i][j]);
        }
    }
    return true;
}

double pc_algo_gmm_component_log_weighted(const pc_algo_gmm_component *c, const float z[3])
{
    double d[3], q;
    const double (*m)[3] = c->inv;
    d[0] = (double)(z[0] - c->mean[0]);
    d[1] = (double)(z[1] - c->mean[1]);
    d[2] = (double)(z[2] - c->mean[2]);
    q = d[0] * (m[0][0] * d[0] + m[0][1] * d[1] + m[0][2] * d[2]) +
        d[1] * (m[1][0] * d[0] + m[1][1] * d[1] + m[1][2] * d[2]) +
        d[2] * (m[2][0] * d[0] + m[2][1] * d[1] + m[2][2] * d[2]);
    return c->log_norm - 0.5 * q;
}

typedef struct acc {
    double n;
    double s[3];
    double ss[3][3];
} acc;

static void acc_add(acc *a, const float zf[3], double w)
{
    double z[3] = {(double)zf[0], (double)zf[1], (double)zf[2]};
    int i, j;
    a->n += w;
    for (i = 0; i < 3; i++) {
        a->s[i] += w * z[i];
        for (j = 0; j < 3; j++) {
            a->ss[i][j] += w * z[i] * z[j];
        }
    }
}

static bool acc_component(const acc *a, double total, float reg, pc_algo_gmm_component *out)
{
    double m[3], c[3][3];
    int i, j;
    if (a->n <= 0.0) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        m[i] = a->s[i] / a->n;
    }
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            c[i][j] = a->ss[i][j] / a->n - m[i] * m[j];
        }
    }
    return component_new(a->n / total, m, c, (double)reg, out);
}

/* Components from accumulators; *some false when none is usable. */
static pc_status from_accs(const acc *accs, size_t k, double total, float reg, pc_algo_gmm *out, bool *some,
                           pc_error *err)
{
    pc_algo_gmm_component *c = pc_malloc_array(k ? k : 1, sizeof *c);
    size_t i, n = 0;
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a colour model");
    }
    for (i = 0; i < k; i++) {
        if (acc_component(&accs[i], total, reg, &c[n])) {
            n++;
        }
    }
    if (n == 0) {
        free(c);
        *some = false;
        return PC_OK;
    }
    out->comps = c;
    out->n = n;
    out->reg = reg;
    *some = true;
    return PC_OK;
}

pc_status pc_algo_gmm_from_labels(const float (*samples)[3], size_t n, const size_t *labels, size_t k,
                                  float reg, pc_algo_gmm *out, bool *some, pc_error *err)
{
    acc *accs = pc_calloc_array(k ? k : 1, sizeof *accs);
    size_t i;
    pc_status st;
    if (accs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a colour model");
    }
    for (i = 0; i < n; i++) {
        if (labels[i] >= k) {
            free(accs);
            return pc_fail(err, PC_ERR_INVALID, "component label %zu out of range", labels[i]);
        }
        acc_add(&accs[labels[i]], samples[i], 1.0);
    }
    st = from_accs(accs, k, (double)n, reg, out, some, err);
    free(accs);
    return st;
}

/* One EM step (soft responsibilities). */
static pc_status em_step(const pc_algo_gmm *g, const float (*samples)[3], size_t n, pc_algo_gmm *out,
                         bool *some, pc_error *err)
{
    size_t k = g->n, i, c;
    acc *accs = pc_calloc_array(k ? k : 1, sizeof *accs);
    double *lw = pc_calloc_array(k ? k : 1, sizeof(double));
    pc_status st;
    if (accs == NULL || lw == NULL) {
        free(accs);
        free(lw);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a colour model");
    }
    for (i = 0; i < n; i++) {
        double mx = -DBL_MAX, sum = 0.0;
        for (c = 0; c < k; c++) {
            lw[c] = pc_algo_gmm_component_log_weighted(&g->comps[c], samples[i]);
            mx = pc_rs_max(mx, lw[c]);
        }
        for (c = 0; c < k; c++) {
            lw[c] = exp(lw[c] - mx);
            sum += lw[c];
        }
        for (c = 0; c < k; c++) {
            double r = lw[c] / sum;
            if (r > 1e-6) {
                acc_add(&accs[c], samples[i], r);
            }
        }
    }
    st = from_accs(accs, k, (double)n, g->reg, out, some, err);
    free(accs);
    free(lw);
    return st;
}

size_t pc_algo_gmm_component_of(const pc_algo_gmm *g, const float z[3])
{
    size_t best = 0, i;
    double bv = -DBL_MAX;
    for (i = 0; i < g->n; i++) {
        double v = pc_algo_gmm_component_log_weighted(&g->comps[i], z);
        if (v > bv) {
            bv = v;
            best = i;
        }
    }
    return best;
}

double pc_algo_gmm_log_prob(const pc_algo_gmm *g, const float z[3])
{
    double mx = -DBL_MAX, vals[16] = {0.0}, s = -0.0;
    size_t n = g->n < 16 ? g->n : 16, i;
    for (i = 0; i < n; i++) {
        vals[i] = pc_algo_gmm_component_log_weighted(&g->comps[i], z);
        mx = pc_rs_max(mx, vals[i]);
    }
    for (i = 0; i < n; i++) {
        s += exp(vals[i] - mx);
    }
    return mx + log(s);
}

float pc_algo_gmm_neg_log(const pc_algo_gmm *g, const float z[3])
{
    return pc_f64_to_f32(pc_clampd(-pc_algo_gmm_log_prob(g, z), -50.0, 200.0));
}

pc_status pc_algo_gmm_refit(const pc_algo_gmm *g, const float (*samples)[3], size_t n, pc_algo_gmm *out,
                            bool *some, pc_error *err)
{
    size_t *labels = pc_malloc_array(n ? n : 1, sizeof(size_t)), i;
    pc_status st;
    if (labels == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a colour model");
    }
    for (i = 0; i < n; i++) {
        labels[i] = pc_algo_gmm_component_of(g, samples[i]);
    }
    st = pc_algo_gmm_from_labels(samples, n, labels, g->n, g->reg, out, some, err);
    free(labels);
    return st;
}

static size_t nearest(const float (*centers)[3], size_t nc, const float z[3])
{
    size_t best = 0, i;
    float bd = FLT_MAX;
    for (i = 0; i < nc; i++) {
        float d = d2(centers[i], z);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

pc_status pc_algo_gmm_kmeans(const float (*samples)[3], size_t n, size_t k, size_t iters,
                             float (**centers_out)[3], size_t *ncenters, pc_error *err)
{
    pc_algo_segment_rng rng;
    float (*centers)[3], (*nw)[3] = NULL, *dist;
    double (*sums)[3] = NULL;
    size_t *counts = NULL, nc = 0, i, it, ccap;
    pc_status st = PC_OK;
    if (n == 0) {
        return pc_fail(err, PC_ERR_INVALID, "k-means needs samples");
    }
    ccap = 0;
    centers = pc_grow_array(NULL, &ccap, k < 64 ? (k ? k : 1) : 64, sizeof *centers);
    dist = pc_malloc_array(n, sizeof(float));
    if (centers == NULL || dist == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for k-means");
        goto fail;
    }
    rng = pc_algo_segment_rng_new(0x5EEDu + (uint64_t)n);
    memcpy(centers[0], samples[pc_algo_segment_rng_next(&rng) % (uint64_t)n], sizeof centers[0]);
    nc = 1;
    for (i = 0; i < n; i++) {
        dist[i] = d2(samples[i], centers[0]);
    }
    while (nc < k) {
        double total = -0.0, t;
        size_t pick = n - 1;
        for (i = 0; i < n; i++) {
            total += (double)dist[i];
        }
        if (total <= 1e-12) {
            break;
        }
        t = (double)pc_algo_segment_rng_f32(&rng) * total;
        for (i = 0; i < n; i++) {
            t -= (double)dist[i];
            if (t <= 0.0) {
                pick = i;
                break;
            }
        }
        if (nc == ccap) {
            void *gc = pc_grow_array(centers, &ccap, nc + 1, sizeof *centers);
            if (gc == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for k-means");
                goto fail;
            }
            centers = gc;
        }
        memcpy(centers[nc], samples[pick], sizeof centers[0]);
        for (i = 0; i < n; i++) {
            dist[i] = pc_rs_minf(dist[i], d2(samples[i], centers[nc]));
        }
        nc++;
    }
    sums = pc_malloc_array(nc, sizeof *sums);
    counts = pc_malloc_array(nc, sizeof(size_t));
    nw = pc_malloc_array(nc, sizeof *nw);
    if (sums == NULL || counts == NULL || nw == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for k-means");
        goto fail;
    }
    for (it = 0; it < iters; it++) {
        size_t c, nn = 0;
        bool done;
        memset(sums, 0, nc * sizeof *sums);
        memset(counts, 0, nc * sizeof(size_t));
        for (i = 0; i < n; i++) {
            size_t ci = nearest((const float (*)[3])centers, nc, samples[i]);
            sums[ci][0] += (double)samples[i][0];
            sums[ci][1] += (double)samples[i][1];
            sums[ci][2] += (double)samples[i][2];
            counts[ci]++;
        }
        for (c = 0; c < nc; c++) {
            if (counts[c] > 0) {
                nw[nn][0] = pc_f64_to_f32(sums[c][0] / (double)counts[c]);
                nw[nn][1] = pc_f64_to_f32(sums[c][1] / (double)counts[c]);
                nw[nn][2] = pc_f64_to_f32(sums[c][2] / (double)counts[c]);
                nn++;
            }
        }
        done = nn == nc;
        for (c = 0; c < nn && done; c++) {
            if (nw[c][0] != centers[c][0] || nw[c][1] != centers[c][1] || nw[c][2] != centers[c][2]) {
                done = false;
            }
        }
        memcpy(centers, nw, nn * sizeof *nw);
        nc = nn;
        if (done) {
            break;
        }
    }
    free(sums);
    free(counts);
    free(nw);
    free(dist);
    *centers_out = centers;
    *ncenters = nc;
    return PC_OK;
fail:
    free(centers);
    free(dist);
    free(sums);
    free(counts);
    free(nw);
    return st;
}

pc_status pc_algo_gmm_fit_em(const float (*samples)[3], size_t n, size_t k, float reg, size_t em_iters,
                             pc_algo_gmm *out, bool *some, pc_error *err)
{
    float (*centers)[3] = NULL;
    size_t nc = 0, *labels, i;
    pc_algo_gmm g;
    bool ok = false;
    pc_status st;
    if (n == 0) {
        *some = false;
        return PC_OK;
    }
    k = k < 1 ? 1 : (k > n ? n : k);
    st = pc_algo_gmm_kmeans(samples, n, k, 10, &centers, &nc, err);
    if (st != PC_OK) {
        return st;
    }
    labels = pc_malloc_array(n, sizeof(size_t));
    if (labels == NULL) {
        free(centers);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a colour model");
    }
    for (i = 0; i < n; i++) {
        labels[i] = nearest((const float (*)[3])centers, nc, samples[i]);
    }
    free(centers);
    st = pc_algo_gmm_from_labels(samples, n, labels, nc, reg, &g, &ok, err);
    free(labels);
    if (st != PC_OK) {
        return st;
    }
    if (!ok) {
        *some = false;
        return PC_OK;
    }
    for (i = 0; i < em_iters; i++) {
        pc_algo_gmm next;
        bool nok = false;
        st = em_step(&g, samples, n, &next, &nok, err);
        if (st != PC_OK) {
            pc_algo_gmm_fini(&g);
            return st;
        }
        if (nok) {
            pc_algo_gmm_fini(&g);
            g = next;
        }
    }
    *out = g;
    *some = true;
    return PC_OK;
}

pc_status pc_algo_gmm_fit(const float (*samples)[3], size_t n, size_t k, float reg, pc_algo_gmm *out,
                          bool *some, pc_error *err)
{
    return pc_algo_gmm_fit_em(samples, n, k, reg, 3, out, some, err);
}
