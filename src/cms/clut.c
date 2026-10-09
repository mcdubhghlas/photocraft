#include "clut.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

pc_status pc_cms_clut_init(pc_cms_clut *out, const size_t *grid, size_t inputs, size_t outputs, float *data,
                           size_t len, pc_error *err)
{
    pc_cms_clut c;
    size_t s = outputs;
    size_t i;
    if (inputs < 1 || inputs > PC_CLUT_MAX_CHANNELS || outputs < 1 || outputs > PC_CLUT_MAX_CHANNELS) {
        free(data);
        return pc_fail(err, PC_ERR_INVALID, "bad CLUT dimensions");
    }
    memset(&c, 0, sizeof c);
    c.inputs = inputs;
    c.outputs = outputs;
    for (i = inputs; i-- > 0;) {
        size_t g = grid[i] < 1 ? 1 : grid[i];
        c.grid[i] = grid[i];
        c.strides[i] = s;
        if (!pc_size_mul(s, g, &s)) {
            free(data);
            return pc_fail(err, PC_ERR_LIMIT, "CLUT too large");
        }
    }
    if (len != s || data == NULL) {
        free(data);
        return pc_fail(err, PC_ERR_INVALID, "CLUT data length");
    }
    c.data = data;
    c.len = len;
    *out = c;
    return PC_OK;
}

pc_status pc_cms_clut_sample(const size_t *grid, size_t inputs, size_t outputs, pc_cms_clut_fn f, void *ctx,
                             pc_cms_clut *out, pc_error *err)
{
    size_t total = 1;
    size_t len;
    size_t node;
    size_t d;
    size_t idx[PC_CLUT_MAX_CHANNELS];
    float inp[PC_CLUT_MAX_CHANNELS];
    float *data;
    if (inputs < 1 || inputs > PC_CLUT_MAX_CHANNELS || outputs < 1 || outputs > PC_CLUT_MAX_CHANNELS) {
        return pc_fail(err, PC_ERR_INVALID, "bad CLUT dimensions");
    }
    for (d = 0; d < inputs; d++) {
        if (grid[d] < 1 || !pc_size_mul(total, grid[d], &total)) {
            return pc_fail(err, PC_ERR_INVALID, "bad CLUT grid");
        }
    }
    if (!pc_size_mul(total, outputs, &len)) {
        return pc_fail(err, PC_ERR_LIMIT, "CLUT too large");
    }
    data = pc_calloc_array(len, sizeof *data);
    if (data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory sampling a CLUT");
    }
    for (node = 0; node < total; node++) {
        size_t rem = node;
        for (d = inputs; d-- > 0;) {
            idx[d] = rem % grid[d];
            rem /= grid[d];
        }
        for (d = 0; d < inputs; d++) {
            inp[d] = grid[d] > 1 ? (float)idx[d] / (float)(grid[d] - 1) : 0.0f;
        }
        f(ctx, inp, data + node * outputs);
    }
    return pc_cms_clut_init(out, grid, inputs, outputs, data, len, err);
}

void pc_cms_clut_fini(pc_cms_clut *c)
{
    if (c != NULL) {
        free(c->data);
        c->data = NULL;
        c->len = 0;
    }
}

pc_status pc_cms_clut_clone(const pc_cms_clut *c, pc_cms_clut *out, pc_error *err)
{
    pc_cms_clut o = *c;
    o.data = pc_malloc_array(c->len, sizeof *o.data);
    if (o.data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a CLUT");
    }
    if (c->len > 0) {
        memcpy(o.data, c->data, c->len * sizeof *o.data);
    }
    *out = o;
    return PC_OK;
}

size_t pc_cms_clut_nodes(const pc_cms_clut *c)
{
    return c->outputs > 0 ? c->len / c->outputs : 0;
}

/* Grid cell along dimension d: data offset, fraction, stride (0 stride for degenerate dims). */
static void locate(const pc_cms_clut *c, size_t d, float v, size_t *base, float *r, size_t *stride)
{
    size_t n = c->grid[d];
    float p;
    size_t i;
    if (n < 2) {
        *base = 0;
        *r = 0.0f;
        *stride = 0;
        return;
    }
    p = pc_clampf(v, 0.0f, 1.0f) * (float)(n - 1);
    i = pc_f32_to_size(p);
    if (i > n - 2) {
        i = n - 2;
    }
    *base = i * c->strides[d];
    *r = p - (float)i;
    *stride = c->strides[d];
}

static size_t min_outputs(const pc_cms_clut *c)
{
    return c->outputs < PC_CLUT_MAX_CHANNELS ? c->outputs : PC_CLUT_MAX_CHANNELS;
}

void pc_cms_clut_tetra(const pc_cms_clut *c, size_t base, const size_t s[3], const float r[3], float *out)
{
    const float *d = c->data;
    float rx = r[0];
    float ry = r[1];
    float rz = r[2];
    size_t sx = s[0];
    size_t sy = s[1];
    size_t sz = s[2];
    size_t p1;
    size_t p2;
    size_t p3;
    float w1;
    float w2;
    float w3;
    size_t k;
    size_t o = min_outputs(c);
    /* Pick the tetrahedron containing (rx, ry, rz) and the path through the cube corners. */
    if (rx >= ry) {
        if (ry >= rz) {
            p1 = sx; p2 = sx + sy; p3 = sx + sy + sz; w1 = rx; w2 = ry; w3 = rz;
        } else if (rx >= rz) {
            p1 = sx; p2 = sx + sz; p3 = sx + sy + sz; w1 = rx; w2 = rz; w3 = ry;
        } else {
            p1 = sz; p2 = sx + sz; p3 = sx + sy + sz; w1 = rz; w2 = rx; w3 = ry;
        }
    } else if (rx >= rz) {
        p1 = sy; p2 = sx + sy; p3 = sx + sy + sz; w1 = ry; w2 = rx; w3 = rz;
    } else if (ry >= rz) {
        p1 = sy; p2 = sy + sz; p3 = sx + sy + sz; w1 = ry; w2 = rz; w3 = rx;
    } else {
        p1 = sz; p2 = sy + sz; p3 = sx + sy + sz; w1 = rz; w2 = ry; w3 = rx;
    }
    for (k = 0; k < o; k++) {
        float v0 = d[base + k];
        float v1 = d[base + p1 + k];
        float v2 = d[base + p2 + k];
        float v3 = d[base + p3 + k];
        out[k] = v0 + (v1 - v0) * w1 + (v2 - v1) * w2 + (v3 - v2) * w3;
    }
}

static void eval1(const pc_cms_clut *c, float v, float *out)
{
    size_t b;
    size_t s;
    float r;
    size_t k;
    locate(c, 0, v, &b, &r, &s);
    for (k = 0; k < c->outputs; k++) {
        float a = c->data[b + k];
        float cc = s > 0 ? c->data[b + s + k] : a;
        out[k] = a + (cc - a) * r;
    }
}

void pc_cms_clut_eval(const pc_cms_clut *c, const float *in, float *out)
{
    size_t b[4];
    size_t s[4];
    float r[4];
    size_t k;
    switch (c->inputs) {
    case 1: eval1(c, in[0], out); return;
    case 3:
        for (k = 0; k < 3; k++) {
            locate(c, k, in[k], &b[k], &r[k], &s[k]);
        }
        pc_cms_clut_tetra(c, b[0] + b[1] + b[2], s, r, out);
        return;
    case 4: {
        float lo[PC_CLUT_MAX_CHANNELS];
        size_t base;
        size_t o = min_outputs(c);
        for (k = 0; k < 4; k++) {
            locate(c, k, in[k], &b[k], &r[k], &s[k]);
        }
        base = b[0] + b[1] + b[2] + b[3];
        memset(lo, 0, sizeof lo);
        pc_cms_clut_tetra(c, base, s + 1, r + 1, lo);
        if (r[0] > 0.0f && s[0] > 0) {
            float hi[PC_CLUT_MAX_CHANNELS];
            memset(hi, 0, sizeof hi);
            pc_cms_clut_tetra(c, base + s[0], s + 1, r + 1, hi);
            for (k = 0; k < o; k++) {
                out[k] = lo[k] + (hi[k] - lo[k]) * r[0];
            }
        } else {
            memcpy(out, lo, o * sizeof *out);
        }
        return;
    }
    default: pc_cms_clut_eval_multilinear(c, in, out); return;
    }
}

void pc_cms_clut_eval_multilinear(const pc_cms_clut *c, const float *in, float *out)
{
    size_t n = c->inputs;
    size_t base = 0;
    float r[PC_CLUT_MAX_CHANNELS];
    size_t s[PC_CLUT_MAX_CHANNELS];
    size_t d;
    size_t k;
    size_t corner;
    for (d = 0; d < n; d++) {
        size_t b;
        locate(c, d, in[d], &b, &r[d], &s[d]);
        base += b;
    }
    for (k = 0; k < c->outputs; k++) {
        out[k] = 0.0f;
    }
    for (corner = 0; corner < ((size_t)1 << n); corner++) {
        float w = 1.0f;
        size_t off = base;
        for (d = 0; d < n; d++) {
            if ((corner & ((size_t)1 << d)) != 0) {
                w *= r[d];
                off += s[d];
            } else {
                w *= 1.0f - r[d];
            }
        }
        if (w == 0.0f) {
            continue;
        }
        for (k = 0; k < c->outputs; k++) {
            out[k] += w * c->data[off + k];
        }
    }
}
