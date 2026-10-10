/* Procedural renderers: Tree and Picture Frame (port of crates/algo/src/render2.rs). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx_defs.h"
#include "algo/render2.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- Tree ---- */

typedef enum leaf_kind {
    LEAF_NONE,
    LEAF_ROUND,
    LEAF_OVAL,
    LEAF_NEEDLE,
    LEAF_HEART,
    LEAF_BLOSSOM,
    LEAF_LONG,
    LEAF_FROND
} leaf_kind;
typedef enum habit { BRANCHING, CONIFER, PALM, BAMBOO } habit;

typedef struct tree_kind {
    const char *name;
    habit habit;
    uint32_t levels, children;
    float spread, ratio, trunk, droop, wiggle, thick;
    leaf_kind leaf;
    float leaf_col[3], bark[3];
    bool has_accent;
    float accent[3];
} tree_kind;

#define BROWN {0.36f, 0.25f, 0.16f}
#define GREY_BARK {0.45f, 0.42f, 0.38f}
#define GREEN {0.24f, 0.52f, 0.16f}
#define DARK_GREEN {0.12f, 0.36f, 0.14f}
#define NOACC false, {0, 0, 0}

static const tree_kind TREES[PC_ALGO_TREE_TYPES] = {
    {"Oak", BRANCHING, 5, 3, 38.0f, 0.72f, 0.35f, 0.05f, 0.35f, 0.035f, LEAF_ROUND, GREEN, BROWN, NOACC},
    {"Maple",
     BRANCHING,
     5,
     3,
     34.0f,
     0.74f,
     0.32f,
     0.0f,
     0.25f,
     0.03f,
     LEAF_HEART,
     {0.3f, 0.55f, 0.15f},
     BROWN,
     NOACC},
    {"Pine", CONIFER, 3, 2, 70.0f, 0.5f, 0.15f, 0.25f, 0.1f, 0.025f, LEAF_NEEDLE, DARK_GREEN, BROWN, NOACC},
    {"Palm",
     PALM,
     1,
     9,
     0.0f,
     0.0f,
     0.85f,
     0.0f,
     0.15f,
     0.022f,
     LEAF_FROND,
     {0.25f, 0.5f, 0.18f},
     {0.55f, 0.45f, 0.32f},
     NOACC},
    {"Birch",
     BRANCHING,
     5,
     2,
     28.0f,
     0.75f,
     0.4f,
     0.1f,
     0.3f,
     0.02f,
     LEAF_OVAL,
     {0.45f, 0.65f, 0.2f},
     {0.88f, 0.86f, 0.82f},
     NOACC},
    {"Weeping Willow",
     BRANCHING,
     5,
     3,
     40.0f,
     0.78f,
     0.3f,
     0.9f,
     0.2f,
     0.035f,
     LEAF_LONG,
     {0.45f, 0.62f, 0.22f},
     GREY_BARK,
     NOACC},
    {"Cherry Blossom",
     BRANCHING,
     5,
     3,
     42.0f,
     0.7f,
     0.3f,
     0.0f,
     0.45f,
     0.03f,
     LEAF_BLOSSOM,
     {0.98f, 0.75f, 0.82f},
     {0.3f, 0.2f, 0.17f},
     NOACC},
    {"Bamboo",
     BAMBOO,
     1,
     5,
     6.0f,
     0.0f,
     1.0f,
     0.0f,
     0.05f,
     0.012f,
     LEAF_LONG,
     {0.35f, 0.6f, 0.2f},
     {0.55f, 0.65f, 0.3f},
     NOACC},
    {"Cypress", BRANCHING, 5, 3, 10.0f, 0.8f, 0.1f, -0.1f, 0.1f, 0.02f, LEAF_NEEDLE, DARK_GREEN, BROWN,
     NOACC},
    {"Baobab",
     BRANCHING,
     3,
     4,
     50.0f,
     0.55f,
     0.65f,
     0.0f,
     0.3f,
     0.09f,
     LEAF_ROUND,
     {0.35f, 0.55f, 0.2f},
     {0.55f, 0.47f, 0.4f},
     NOACC},
    {"Ginkgo",
     BRANCHING,
     4,
     3,
     30.0f,
     0.72f,
     0.35f,
     0.0f,
     0.2f,
     0.025f,
     LEAF_HEART,
     {0.75f, 0.75f, 0.2f},
     GREY_BARK,
     NOACC},
    {"Spruce",
     CONIFER,
     3,
     2,
     80.0f,
     0.45f,
     0.1f,
     0.45f,
     0.08f,
     0.025f,
     LEAF_NEEDLE,
     {0.1f, 0.3f, 0.2f},
     BROWN,
     NOACC},
    {"Bare Winter Tree",
     BRANCHING,
     6,
     2,
     34.0f,
     0.74f,
     0.3f,
     0.05f,
     0.35f,
     0.03f,
     LEAF_NONE,
     GREEN,
     {0.3f, 0.25f, 0.22f},
     NOACC},
    {"Apple",
     BRANCHING,
     5,
     3,
     46.0f,
     0.68f,
     0.3f,
     0.1f,
     0.4f,
     0.03f,
     LEAF_ROUND,
     GREEN,
     BROWN,
     true,
     {0.85f, 0.1f, 0.1f}},
    {"Acacia",
     BRANCHING,
     4,
     3,
     55.0f,
     0.75f,
     0.45f,
     -0.25f,
     0.25f,
     0.03f,
     LEAF_ROUND,
     {0.35f, 0.5f, 0.18f},
     BROWN,
     NOACC},
    {"Poplar",
     BRANCHING,
     5,
     3,
     12.0f,
     0.8f,
     0.15f,
     -0.15f,
     0.1f,
     0.025f,
     LEAF_OVAL,
     {0.3f, 0.55f, 0.15f},
     GREY_BARK,
     NOACC},
    {"Elm", BRANCHING, 5, 2, 24.0f, 0.8f, 0.3f, 0.15f, 0.25f, 0.032f, LEAF_OVAL, GREEN, BROWN, NOACC},
    {"Magnolia",
     BRANCHING,
     4,
     3,
     40.0f,
     0.7f,
     0.3f,
     0.0f,
     0.3f,
     0.03f,
     LEAF_BLOSSOM,
     {0.98f, 0.9f, 0.92f},
     {0.35f, 0.28f, 0.25f},
     true,
     {0.85f, 0.5f, 0.65f}},
    {"Banana",
     PALM,
     1,
     7,
     0.0f,
     0.0f,
     0.6f,
     0.0f,
     0.05f,
     0.04f,
     LEAF_FROND,
     {0.45f, 0.7f, 0.2f},
     {0.5f, 0.6f, 0.3f},
     NOACC},
    {"Bonsai",
     BRANCHING,
     4,
     3,
     60.0f,
     0.6f,
     0.45f,
     0.2f,
     0.7f,
     0.05f,
     LEAF_ROUND,
     DARK_GREEN,
     {0.4f, 0.3f, 0.22f},
     NOACC},
    {"Jacaranda",
     BRANCHING,
     5,
     3,
     44.0f,
     0.72f,
     0.3f,
     0.0f,
     0.35f,
     0.03f,
     LEAF_BLOSSOM,
     {0.6f, 0.45f, 0.85f},
     BROWN,
     NOACC},
    {"Autumn Maple",
     BRANCHING,
     5,
     3,
     34.0f,
     0.74f,
     0.32f,
     0.0f,
     0.25f,
     0.03f,
     LEAF_HEART,
     {0.85f, 0.3f, 0.1f},
     BROWN,
     NOACC},
    {"Golden Autumn",
     BRANCHING,
     5,
     3,
     36.0f,
     0.72f,
     0.33f,
     0.05f,
     0.3f,
     0.03f,
     LEAF_ROUND,
     {0.9f, 0.7f, 0.15f},
     BROWN,
     NOACC},
    {"Olive",
     BRANCHING,
     4,
     3,
     48.0f,
     0.68f,
     0.35f,
     0.1f,
     0.6f,
     0.04f,
     LEAF_LONG,
     {0.5f, 0.58f, 0.42f},
     GREY_BARK,
     true,
     {0.25f, 0.28f, 0.12f}},
    {"Fir",
     CONIFER,
     3,
     2,
     75.0f,
     0.5f,
     0.12f,
     0.15f,
     0.05f,
     0.025f,
     LEAF_NEEDLE,
     {0.12f, 0.38f, 0.22f},
     BROWN,
     NOACC},
    {"Cedar",
     CONIFER,
     3,
     3,
     88.0f,
     0.6f,
     0.2f,
     0.0f,
     0.15f,
     0.03f,
     LEAF_NEEDLE,
     {0.2f, 0.4f, 0.25f},
     BROWN,
     NOACC},
    {"Lemon",
     BRANCHING,
     4,
     3,
     45.0f,
     0.68f,
     0.3f,
     0.05f,
     0.35f,
     0.03f,
     LEAF_OVAL,
     DARK_GREEN,
     BROWN,
     true,
     {0.98f, 0.88f, 0.15f}},
    {"Wisteria",
     BRANCHING,
     5,
     3,
     45.0f,
     0.72f,
     0.3f,
     0.7f,
     0.3f,
     0.03f,
     LEAF_BLOSSOM,
     {0.7f, 0.55f, 0.9f},
     {0.35f, 0.3f, 0.27f},
     NOACC},
    {"Young Sapling",
     BRANCHING,
     3,
     2,
     30.0f,
     0.7f,
     0.45f,
     0.0f,
     0.2f,
     0.015f,
     LEAF_OVAL,
     {0.4f, 0.7f, 0.2f},
     {0.45f, 0.35f, 0.22f},
     NOACC},
    {"Dead Tree",
     BRANCHING,
     5,
     2,
     45.0f,
     0.68f,
     0.4f,
     0.1f,
     0.8f,
     0.035f,
     LEAF_NONE,
     GREEN,
     {0.32f, 0.29f, 0.26f},
     NOACC},
    {"Coconut Palm",
     PALM,
     1,
     11,
     0.0f,
     0.0f,
     0.88f,
     0.0f,
     0.35f,
     0.018f,
     LEAF_FROND,
     {0.3f, 0.55f, 0.15f},
     {0.6f, 0.5f, 0.36f},
     true,
     {0.4f, 0.3f, 0.15f}},
    {"Shrub", BRANCHING, 4, 4, 55.0f, 0.7f, 0.05f, 0.0f, 0.35f, 0.02f, LEAF_ROUND, GREEN, BROWN, NOACC},
    {"Plum Blossom",
     BRANCHING,
     5,
     2,
     48.0f,
     0.7f,
     0.3f,
     0.1f,
     0.6f,
     0.03f,
     LEAF_BLOSSOM,
     {0.95f, 0.6f, 0.7f},
     {0.25f, 0.18f, 0.15f},
     NOACC},
    {"Cloud Pine",
     BRANCHING,
     4,
     3,
     70.0f,
     0.65f,
     0.35f,
     0.2f,
     0.5f,
     0.035f,
     LEAF_NEEDLE,
     {0.15f, 0.4f, 0.2f},
     {0.35f, 0.27f, 0.2f},
     NOACC},
};

const char *const pc_algo_tree_type_names[PC_ALGO_TREE_TYPES] = {
    "Oak",
    "Maple",
    "Pine",
    "Palm",
    "Birch",
    "Weeping Willow",
    "Cherry Blossom",
    "Bamboo",
    "Cypress",
    "Baobab",
    "Ginkgo",
    "Spruce",
    "Bare Winter Tree",
    "Apple",
    "Acacia",
    "Poplar",
    "Elm",
    "Magnolia",
    "Banana",
    "Bonsai",
    "Jacaranda",
    "Autumn Maple",
    "Golden Autumn",
    "Olive",
    "Fir",
    "Cedar",
    "Lemon",
    "Wisteria",
    "Young Sapling",
    "Dead Tree",
    "Coconut Palm",
    "Shrub",
    "Plum Blossom",
    "Cloud Pine",
};

pc_algo_tree_spec pc_algo_tree_spec_default(void)
{
    pc_algo_tree_spec s;
    memset(&s, 0, sizeof s);
    s.base_tree_type = 1;
    s.light_direction = 3;
    s.leaves_amount = 50.0f;
    s.leaves_size = 100.0f;
    s.branches_height = 100.0f;
    s.branches_thickness = 100.0f;
    s.default_leaves = true;
    s.leaves_color[0] = 0.24f, s.leaves_color[1] = 0.52f, s.leaves_color[2] = 0.16f, s.leaves_color[3] = 1.0f;
    s.branches_color[0] = 0.36f, s.branches_color[1] = 0.25f, s.branches_color[2] = 0.16f,
    s.branches_color[3] = 1.0f;
    s.x = 0.5f;
    s.y = 0.95f;
    s.size = 0.8f;
    return s;
}

typedef struct keyed {
    float key;
    pc_algo_prim prim;
} keyed;

typedef struct tree_ctx {
    const tree_kind *kind;
    const pc_algo_tree_spec *spec;
    pc_algo_rng rng;
    pc_algo_prims branches;
    keyed *leaves;
    size_t nleaves, capleaves;
    float bark[3], leaf[3], light[2], centre[2], leaf_px;
    pc_error *err;
} tree_ctx;

static void shade(const float c[3], float k, float out[4])
{
    pc_algo_render2_shade(c, k, out);
}

static pc_status push_leaf(tree_ctx *t, float key, pc_algo_prim p)
{
    if (t->nleaves >= PC_ALGO_MAX_PRIMS) {
        return pc_fail(t->err, PC_ERR_LIMIT, "more than %zu render primitives", PC_ALGO_MAX_PRIMS);
    }
    if (t->nleaves == t->capleaves) {
        keyed *nv = pc_grow_array(t->leaves, &t->capleaves, t->nleaves + 1, sizeof *nv);
        if (nv == NULL) {
            return pc_fail(t->err, PC_ERR_NOMEM, "out of memory for a tree");
        }
        t->leaves = nv;
    }
    t->leaves[t->nleaves].key = key;
    t->leaves[t->nleaves].prim = p;
    t->nleaves++;
    return PC_OK;
}

/* Brightness of a leaf at p (lit side of the crown brighter). */
static float light_at(tree_ctx *t, const float p[2])
{
    float dx, dy, l;
    if (t->spec->flat_shading) {
        return 1.0f;
    }
    dx = p[0] - t->centre[0];
    dy = p[1] - t->centre[1];
    l = pc_rs_maxf_k(sqrtf(dx * dx + dy * dy), 1.0f);
    return 0.78f + 0.32f * ((dx * t->light[0] + dy * t->light[1]) / l) + 0.12f * pc_algo_rng_sym(&t->rng);
}

static pc_status add_leaf(tree_ctx *t, const float p[2], float dir)
{
    float s = t->leaf_px * pc_algo_rng_range(&t->rng, 0.7f, 1.2f);
    float ang = dir + pc_algo_rng_sym(&t->rng) * 1.2f;
    float k = light_at(t, p), ca[4], cb[4], order;
    const float *col = t->leaf;
    pc_algo_prim prim;
    pc_status st;
    switch (t->kind->leaf) {
    case LEAF_NONE:
        return PC_OK;
    case LEAF_ROUND:
        shade(col, k * 1.1f, ca), shade(col, k * 0.8f, cb);
        prim = pc_algo_prim_ellipse(p, s, s * 0.85f, ang, ca, cb);
        break;
    case LEAF_OVAL:
        shade(col, k * 1.1f, ca), shade(col, k * 0.8f, cb);
        prim = pc_algo_prim_ellipse(p, s * 1.2f, s * 0.55f, ang, ca, cb);
        break;
    case LEAF_LONG:
        shade(col, k * 1.1f, ca), shade(col, k * 0.8f, cb);
        prim = pc_algo_prim_ellipse(p, s * 1.6f, s * 0.28f, ang, ca, cb);
        break;
    case LEAF_HEART:
        shade(col, k * 1.1f, ca), shade(col, k * 0.75f, cb);
        prim = pc_algo_prim_ellipse(p, s * 0.9f, s * 1.1f, ang, ca, cb);
        break;
    case LEAF_NEEDLE: {
        /* A tuft of needles fanning out from the twig. */
        const int n = 7;
        int j;
        for (j = 0; j < n; j++) {
            float a = ang + ((float)j / (float)(n - 1) - 0.5f) * 2.4f + pc_algo_rng_sym(&t->rng) * 0.15f;
            float l = s * pc_algo_rng_range(&t->rng, 1.1f, 1.7f), q[2], kk;
            q[0] = p[0] + cosf(a) * l;
            q[1] = p[1] + sinf(a) * l;
            kk = k + 0.1f * pc_algo_rng_sym(&t->rng);
            shade(col, kk * 0.85f, ca), shade(col, kk * 1.1f, cb);
            st = push_leaf(t, kk,
                           pc_algo_prim_capsule(p, q, pc_rs_maxf_k(s * 0.1f, 0.45f),
                                                pc_rs_maxf_k(s * 0.05f, 0.3f), ca, cb));
            if (st != PC_OK) {
                return st;
            }
        }
        return PC_OK;
    }
    case LEAF_BLOSSOM:
        shade(col, k * 1.08f, ca), shade(col, k * 0.85f, cb);
        prim = pc_algo_prim_ellipse(p, s * 0.75f, s * 0.75f, 0.0f, ca, cb);
        break;
    default:
        shade(col, k, ca), shade(col, k * 0.8f, cb);
        prim = pc_algo_prim_ellipse(p, s * 1.6f, s * 0.3f, ang, ca, cb);
        break;
    }
    order = k + pc_algo_rng_f(&t->rng) * 0.05f;
    st = push_leaf(t, order, prim);
    if (st == PC_OK && t->kind->has_accent && pc_algo_rng_f(&t->rng) < 0.12f) {
        float q[2], r;
        q[0] = p[0] + pc_algo_rng_sym(&t->rng) * s;
        q[1] = p[1] + pc_algo_rng_sym(&t->rng) * s;
        r = pc_rs_maxf_k(s * 0.45f, 1.0f);
        shade(t->kind->accent, k * 1.15f, ca), shade(t->kind->accent, k * 0.75f, cb);
        st = push_leaf(t, k + 1.0f, pc_algo_prim_ellipse(q, r, r, 0.0f, ca, cb));
    }
    return st;
}

/* A curved branch from p heading ang (radians, 0 = right, y down); returns its end and heading. */
static pc_status limb(tree_ctx *t, const float p[2], float ang, float len, float r0, float r1, float depth01,
                      float end[2], float *end_ang)
{
    size_t segs = pc_f32_to_size(len / 6.0f), k;
    float q[2] = {p[0], p[1]}, a = ang, droop = t->kind->droop * depth01, col0[4];
    pc_status st;
    segs = segs < 2 ? 2 : (segs > 10 ? 10 : segs);
    shade(t->bark, 0.9f + 0.2f * depth01, col0);
    for (k = 0; k < segs; k++) {
        float t0 = (float)k / (float)segs, t1 = (float)(k + 1) / (float)segs, diff, step, nq[2];
        const float down = PC_FX_FRAC_PI_2;
        a += t->kind->wiggle * pc_algo_rng_sym(&t->rng) * 0.25f;
        /* Gravity pulls the heading towards straight down. */
        diff = atan2f(sinf(down - a), cosf(down - a));
        a += diff * droop * 0.12f;
        step = len / (float)segs;
        nq[0] = q[0] + cosf(a) * step;
        nq[1] = q[1] + sinf(a) * step;
        st = pc_algo_prims_push(
            &t->branches, pc_algo_prim_capsule(q, nq, r0 + (r1 - r0) * t0, r0 + (r1 - r0) * t1, col0, col0),
            t->err);
        if (st != PC_OK) {
            return st;
        }
        if (depth01 > 0.99f && t->kind->leaf != LEAF_NONE && k > 0 &&
            pc_algo_rng_f(&t->rng) < pc_clampf(t->spec->leaves_amount, 0.0f, 100.0f) / 100.0f * 0.9f) {
            st = add_leaf(t, nq, a);
            if (st != PC_OK) {
                return st;
            }
        }
        q[0] = nq[0], q[1] = nq[1];
    }
    end[0] = q[0], end[1] = q[1];
    *end_ang = a;
    return PC_OK;
}

static pc_status branch(tree_ctx *t, const float p[2], float ang, float len, float r, uint32_t level)
{
    uint32_t levels = t->kind->levels, n = t->kind->children, c;
    float depth01 = (float)level / (float)(levels > 1 ? levels : 1), r_end = r * 0.62f, end[2], a, spread;
    pc_status st = limb(t, p, ang, len, r, r_end, depth01, end, &a);
    if (st != PC_OK) {
        return st;
    }
    if (level >= levels) {
        float amount = pc_clampf(t->spec->leaves_amount, 0.0f, 100.0f) / 100.0f;
        if (t->kind->leaf != LEAF_NONE) {
            size_t nl = pc_f32_to_size(roundf(amount * 9.0f)), i;
            for (i = 0; i < nl && st == PC_OK; i++) {
                float d = len * 0.5f * pc_algo_rng_f(&t->rng), th = pc_algo_rng_f(&t->rng) * PC_FX_TAU, q[2];
                q[0] = end[0] + cosf(th) * d;
                q[1] = end[1] + sinf(th) * d;
                st = add_leaf(t, q, a);
            }
        }
        return st;
    }
    spread = pc_fx_rad(t->kind->spread);
    for (c = 0; c < n && st == PC_OK; c++) {
        float side = n == 1 ? 0.0f : (float)c / (float)(n - 1) * 2.0f - 1.0f;
        float ca = a + side * spread + pc_algo_rng_sym(&t->rng) * spread * 0.35f;
        float cl = len * t->kind->ratio * pc_algo_rng_range(&t->rng, 0.8f, 1.1f);
        float cr = r_end * pc_algo_rng_range(&t->rng, 0.75f, 0.95f);
        st = branch(t, end, ca, cl, cr, level + 1);
    }
    return st;
}

/* compiler-rt's __powisf2 (f32::powi with a run-time exponent). */
static float powi_rt(float a, int b)
{
    int recip = b < 0;
    float r = 1.0f;
    for (;;) {
        if (b & 1) {
            r *= a;
        }
        b /= 2;
        if (b == 0) {
            break;
        }
        a *= a;
    }
    return recip ? 1.0f / r : r;
}

/* Stable merge sort of the leaves by key (partial_cmp; NaN compares Equal). */
static pc_status sort_leaves(keyed *v, size_t n, pc_error *err)
{
    keyed *tmp;
    size_t width, i;
    if (n < 2) {
        return PC_OK;
    }
    tmp = pc_malloc_array(n, sizeof *tmp);
    if (tmp == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory sorting tree leaves");
    }
    for (width = 1; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            size_t lo = i, mid = i + width < n ? i + width : n, hi = i + 2 * width < n ? i + 2 * width : n;
            size_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                /* Take the right element only when it is strictly less. */
                tmp[k++] = v[b].key < v[a].key ? v[b++] : v[a++];
            }
            while (a < mid) {
                tmp[k++] = v[a++];
            }
            while (b < hi) {
                tmp[k++] = v[b++];
            }
        }
        memcpy(v, tmp, n * sizeof *v);
    }
    free(tmp);
    return PC_OK;
}

static pc_status tree_body(tree_ctx *t, const pc_algo_tree_spec *spec, pc_rect canvas, float h,
                           const float base[2], float bh, float r0)
{
    const tree_kind *kind = t->kind;
    const float up = -PC_FX_FRAC_PI_2;
    float top[2], a, ca[4], cb[4];
    pc_status st;
    uint32_t c;
    size_t k;
    (void)canvas;
    switch (kind->habit) {
    case BRANCHING: {
        float trunk = pc_rs_minf(h * kind->trunk * bh, h * 0.92f), geo = -0.0f, first;
        uint32_t n = kind->children > 2 ? kind->children : 2, lv;
        st = limb(t, base, up, pc_rs_maxf_k(trunk, 1.0f), r0, r0 * 0.75f, 0.0f, top, &a);
        if (st != PC_OK) {
            return st;
        }
        /* Crown length so the tree reaches about h. */
        for (lv = 0; lv < kind->levels; lv++) {
            geo += powi_rt(kind->ratio, (int)lv);
        }
        first = (pc_rs_maxf(h - trunk, h * 0.08f) / pc_rs_maxf_k(geo, 0.5f)) * 1.05f;
        for (c = 0; c < n && st == PC_OK; c++) {
            float side = (float)c / (float)(n - 1) * 2.0f - 1.0f, sp = pc_fx_rad(kind->spread) * 0.8f;
            float cang = a + side * sp + pc_algo_rng_sym(&t->rng) * 0.15f;
            float fl = first * pc_algo_rng_range(&t->rng, 0.85f, 1.05f);
            st = branch(t, top, cang, fl, r0 * 0.7f, 1);
        }
        return st;
    }
    case CONIFER: {
        float start, spread;
        size_t whorls;
        st = limb(t, base, up, h, r0, r0 * 0.1f, 0.0f, top, &a);
        if (st != PC_OK) {
            return st;
        }
        start = pc_rs_minf(h * kind->trunk * bh, h * 0.8f);
        whorls = pc_f32_to_size(14.0f * pc_clampf(sqrtf(h / 300.0f), 0.6f, 2.5f));
        spread = pc_fx_rad(kind->spread);
        for (k = 0; k < whorls; k++) {
            float f = (float)k / (float)whorls, y = base[1] - start - (h - start) * f;
            float x = base[0] + (top[0] - base[0]) * ((base[1] - y) / h);
            float span = (h - start) * 0.42f * powf(1.0f - f, 0.9f) + h * 0.03f, pt[2] = {x, y};
            int sd;
            for (sd = 0; sd < 2; sd++) {
                float side = sd == 0 ? -1.0f : 1.0f,
                      ang = up + side * spread + pc_algo_rng_sym(&t->rng) * 0.1f;
                st = branch(t, pt, ang, span, pc_rs_maxf_k(r0 * 0.35f * (1.0f - f), 0.6f), kind->levels);
                if (st == PC_OK && kind->levels >= 2) {
                    float s2 = sinf(ang + side * 0.5f), c2 = cosf(ang + side * 0.5f);
                    float mid[2] = {x + c2 * span * 0.5f, y + s2 * span * 0.5f};
                    st = branch(t, mid, ang + side * 0.6f, span * 0.4f, pc_rs_maxf_k(r0 * 0.15f, 0.5f),
                                kind->levels);
                }
                if (st != PC_OK) {
                    return st;
                }
            }
        }
        return PC_OK;
    }
    case PALM: {
        float trunk = pc_rs_maxf_k(h * kind->trunk * pc_rs_minf_k(bh, 1.15f), 1.0f);
        float lean = pc_algo_rng_sym(&t->rng) * kind->wiggle, p[2] = {base[0], base[1]}, flen;
        const int segs = 12;
        int kk;
        uint32_t fronds = kind->children, fk;
        size_t leaflets;
        for (kk = 0; kk < segs; kk++) {
            float f = (float)kk / (float)segs, an = up + lean * f, q[2], r, ring[4];
            q[0] = p[0] + cosf(an) * trunk / (float)segs;
            q[1] = p[1] + sinf(an) * trunk / (float)segs;
            r = r0 * (1.0f - 0.35f * f);
            shade(t->bark, kk % 2 == 0 ? 1.0f : 0.85f, ring);
            st = pc_algo_prims_push(&t->branches, pc_algo_prim_capsule(p, q, r, r * 0.97f, ring, ring),
                                    t->err);
            if (st != PC_OK) {
                return st;
            }
            p[0] = q[0], p[1] = q[1];
        }
        flen = h * 0.38f * sqrtf(pc_clampf(spec->leaves_size, 10.0f, 200.0f) / 100.0f);
        leaflets = pc_f32_to_size(roundf(pc_clampf(spec->leaves_amount, 0.0f, 100.0f) / 100.0f * 3.0f));
        for (fk = 0; fk < fronds; fk++) {
            float an =
                up + ((float)fk / (float)fronds - 0.5f) * PC_FX_PI * 1.7f + pc_algo_rng_sym(&t->rng) * 0.2f;
            float q[2] = {p[0], p[1]}, ang = an;
            const int fsegs = 10;
            int s;
            for (s = 0; s < fsegs; s++) {
                float d = PC_FX_FRAC_PI_2 - ang, nq[2], k2;
                size_t j;
                ang += 0.06f * atan2f(sinf(d), cosf(d));
                nq[0] = q[0] + cosf(ang) * flen / (float)fsegs;
                nq[1] = q[1] + sinf(ang) * flen / (float)fsegs;
                k2 = light_at(t, nq);
                shade(t->leaf, k2 * 0.8f, ca);
                st = push_leaf(t, k2 - 0.1f,
                               pc_algo_prim_capsule(q, nq, pc_rs_maxf_k(r0 * 0.15f, 0.6f),
                                                    pc_rs_maxf_k(r0 * 0.1f, 0.4f), ca, ca));
                for (j = 0; j < leaflets && st == PC_OK; j++) {
                    float along = (float)j / (float)leaflets, at[2];
                    int sd;
                    at[0] = q[0] + (nq[0] - q[0]) * along;
                    at[1] = q[1] + (nq[1] - q[1]) * along;
                    for (sd = 0; sd < 2 && st == PC_OK; sd++) {
                        float side = sd == 0 ? -1.0f : 1.0f, la = ang + side * 1.1f;
                        float ll = flen * 0.22f * (1.0f - (float)s / (float)fsegs * 0.7f), tip[2];
                        tip[0] = at[0] + cosf(la) * ll;
                        tip[1] = at[1] + sinf(la) * ll + ll * 0.25f;
                        shade(t->leaf, k2, ca), shade(t->leaf, k2 * 0.9f, cb);
                        st = push_leaf(
                            t, k2,
                            pc_algo_prim_capsule(at, tip, pc_rs_maxf_k(ll * 0.08f, 0.5f), 0.3f, ca, cb));
                    }
                }
                if (st != PC_OK) {
                    return st;
                }
                q[0] = nq[0], q[1] = nq[1];
            }
        }
        if (kind->has_accent) {
            for (kk = 0; kk < 4; kk++) {
                float cc[2];
                cc[0] = p[0] + pc_algo_rng_sym(&t->rng) * r0 * 2.0f;
                cc[1] = p[1] + pc_algo_rng_f(&t->rng) * r0 * 2.0f;
                shade(kind->accent, 1.0f, ca);
                st = push_leaf(t, 2.0f, pc_algo_prim_disc(cc, r0 * 1.1f, ca));
                if (st != PC_OK) {
                    return st;
                }
            }
        }
        return PC_OK;
    }
    default: {
        uint32_t stalks = kind->children, sk;
        for (sk = 0; sk < stalks; sk++) {
            float x = base[0] + ((float)sk - (float)(stalks - 1) / 2.0f) * r0 * 5.0f +
                      pc_algo_rng_sym(&t->rng) * r0;
            float hh = h * pc_algo_rng_range(&t->rng, 0.7f, 1.0f);
            float an = up + pc_algo_rng_sym(&t->rng) * pc_fx_rad(kind->spread) * 0.5f, p[2] = {x, base[1]};
            const int joints = 8;
            int j;
            for (j = 0; j < joints; j++) {
                float q[2], l0[2], l1[2], c7[4];
                q[0] = p[0] + cosf(an) * hh / (float)joints;
                q[1] = p[1] + sinf(an) * hh / (float)joints;
                shade(t->bark, 1.0f, ca), shade(t->bark, 0.85f, cb);
                st = pc_algo_prims_push(&t->branches, pc_algo_prim_capsule(p, q, r0, r0, ca, cb), t->err);
                l0[0] = q[0] - r0 * 1.1f, l0[1] = q[1];
                l1[0] = q[0] + r0 * 1.1f, l1[1] = q[1];
                shade(t->bark, 0.7f, c7);
                if (st == PC_OK) {
                    st = pc_algo_prims_push(
                        &t->branches, pc_algo_prim_capsule(l0, l1, r0 * 0.25f, r0 * 0.25f, c7, c7), t->err);
                }
                if (st == PC_OK && j >= joints / 3) {
                    size_t nl = pc_f32_to_size(
                               roundf(pc_clampf(spec->leaves_amount, 0.0f, 100.0f) / 100.0f * 4.0f)),
                           i;
                    for (i = 0; i < nl && st == PC_OK; i++) {
                        float side = pc_algo_rng_f(&t->rng) < 0.5f ? -1.0f : 1.0f;
                        float la = an + side * pc_algo_rng_range(&t->rng, 0.6f, 1.4f), ll = t->leaf_px * 2.2f,
                              lp[2];
                        lp[0] = q[0] + cosf(la) * ll;
                        lp[1] = q[1] + sinf(la) * ll;
                        st = add_leaf(t, lp, la);
                    }
                }
                if (st != PC_OK) {
                    return st;
                }
                p[0] = q[0], p[1] = q[1];
            }
        }
        return PC_OK;
    }
    }
}

pc_status pc_algo_tree(const pc_algo_tree_spec *spec, pc_rect canvas, pc_algo_prims *out, pc_error *err)
{
    uint32_t bt =
        spec->base_tree_type < 1
            ? 1
            : (spec->base_tree_type > PC_ALGO_TREE_TYPES ? PC_ALGO_TREE_TYPES : spec->base_tree_type);
    const tree_kind *kind = &TREES[bt - 1];
    float ch = (float)pc_rect_height(canvas), h = ch * pc_clampf(spec->size, 0.05f, 2.0f);
    float base[2] = {(float)canvas.x0 + (float)pc_rect_width(canvas) * spec->x,
                     (float)canvas.y0 + ch * spec->y};
    uint32_t ld = spec->light_direction < 1 ? 1 : (spec->light_direction > 5 ? 5 : spec->light_direction);
    float la = pc_fx_rad(ld == 1 ? 180.0f : ld == 2 ? 225.0f : ld == 3 ? 270.0f : ld == 4 ? 315.0f : 0.0f);
    float thick = pc_clampf(spec->branches_thickness, 50.0f, 200.0f) / 100.0f;
    float bh = pc_clampf(spec->branches_height, 50.0f, 300.0f) / 100.0f, r0;
    tree_ctx t;
    pc_status st;
    size_t i;
    memset(&t, 0, sizeof t);
    t.kind = kind;
    t.spec = spec;
    t.err = err;
    t.rng = pc_algo_rng_new(spec->seed, 0x7EEu + (uint64_t)spec->base_tree_type);
    memcpy(t.leaf, spec->default_leaves ? kind->leaf_col : spec->leaves_color, sizeof t.leaf);
    memcpy(t.bark, spec->custom_branch_color ? spec->branches_color : kind->bark, sizeof t.bark);
    t.light[0] = cosf(la);
    t.light[1] = sinf(la);
    t.centre[0] = base[0];
    t.centre[1] = base[1] - h * 0.65f;
    t.leaf_px = pc_rs_maxf_k(h * 0.012f * pc_clampf(spec->leaves_size, 0.0f, 200.0f) / 100.0f, 0.6f);
    r0 = pc_rs_maxf_k(h * kind->thick * thick, 0.8f);
    st = tree_body(&t, spec, canvas, h, base, bh, r0);
    /* Back leaves first: darker ones underneath, lit ones on top. */
    if (st == PC_OK) {
        st = sort_leaves(t.leaves, t.nleaves, err);
    }
    for (i = 0; i < t.nleaves && st == PC_OK; i++) {
        st = pc_algo_prims_push(&t.branches, t.leaves[i].prim, err);
    }
    free(t.leaves);
    if (st != PC_OK) {
        pc_algo_prims_fini(&t.branches);
        return st;
    }
    *out = t.branches;
    return PC_OK;
}

/* ---- Picture Frame ---- */

const char *const pc_algo_frame_styles[PC_ALGO_FRAME_STYLES] = {
    "vineWithFlowers", "vineWithLeaves", "ivy",  "roses",    "daisies",    "berries",    "bamboo", "waves",
    "zigzag",          "dots",           "rope", "scallops", "doubleLine", "snowflakes", "stars",  "hearts",
};

pc_algo_frame_spec pc_algo_frame_spec_default(void)
{
    pc_algo_frame_spec s;
    const float vine[4] = {0.25f, 0.4f, 0.15f, 1.0f}, fl[4] = {0.9f, 0.3f, 0.45f, 1.0f},
                lf[4] = {0.3f, 0.6f, 0.2f, 1.0f};
    memset(&s, 0, sizeof s);
    s.frame = 1;
    s.margin = 4.0f;
    s.size = 50.0f;
    s.arrangement = 50.0f;
    s.lines = 1;
    s.thickness = 30.0f;
    memcpy(s.vine_color, vine, sizeof vine);
    memcpy(s.flower_color, fl, sizeof fl);
    memcpy(s.leaf_color, lf, sizeof lf);
    return s;
}

/* The frame rectangle walked by arc length (clockwise from the top-left). */
typedef struct border {
    float x0, y0, w, h;
} border;

static float border_len(const border *b)
{
    return 2.0f * (b->w + b->h);
}

/* Point, unit tangent and inward normal at arc length s. */
static void border_at(const border *b, float s, float p[2], float t[2], float nn[2])
{
    float w = b->w, h = b->h;
    s = pc_algo_rem_euclidf(s, border_len(b));
    if (s < w) {
        p[0] = b->x0 + s, p[1] = b->y0, t[0] = 1.0f, t[1] = 0.0f, nn[0] = 0.0f, nn[1] = 1.0f;
    } else if (s < w + h) {
        p[0] = b->x0 + w, p[1] = b->y0 + s - w, t[0] = 0.0f, t[1] = 1.0f, nn[0] = -1.0f, nn[1] = 0.0f;
    } else if (s < 2.0f * w + h) {
        p[0] = b->x0 + w - (s - w - h), p[1] = b->y0 + h, t[0] = -1.0f, t[1] = 0.0f, nn[0] = 0.0f,
        nn[1] = -1.0f;
    } else {
        p[0] = b->x0, p[1] = b->y0 + h - (s - 2.0f * w - h), t[0] = 0.0f, t[1] = -1.0f, nn[0] = 1.0f,
        nn[1] = 0.0f;
    }
}

static pc_status flower(pc_algo_prims *out, const float c[2], float r, int petals, const float petal[4],
                        const float centre[4], float rot, pc_error *err)
{
    float pd[4], cd[4];
    int k;
    pc_status st = PC_OK;
    pc_algo_render2_shade(petal, 0.8f, pd);
    pc_algo_render2_shade(centre, 0.75f, cd);
    for (k = 0; k < petals && st == PC_OK; k++) {
        float a = rot + (float)k / (float)petals * PC_FX_TAU, p[2];
        p[0] = c[0] + cosf(a) * r * 0.55f;
        p[1] = c[1] + sinf(a) * r * 0.55f;
        st = pc_algo_prims_push(out, pc_algo_prim_ellipse(p, r * 0.5f, r * 0.26f, a, petal, pd), err);
    }
    if (st == PC_OK) {
        st = pc_algo_prims_push(out, pc_algo_prim_ellipse(c, r * 0.28f, r * 0.28f, 0.0f, centre, cd), err);
    }
    return st;
}

static pc_status leaf(pc_algo_prims *out, const float p[2], float ang, float len, const float col[4],
                      pc_error *err)
{
    float c[2] = {p[0] + cosf(ang) * len * 0.5f, p[1] + sinf(ang) * len * 0.5f}, a[4], b[4];
    pc_algo_render2_shade(col, 1.12f, a);
    pc_algo_render2_shade(col, 0.8f, b);
    return pc_algo_prims_push(out, pc_algo_prim_ellipse(c, len * 0.5f, len * 0.22f, ang, a, b), err);
}

static pc_status star(pc_algo_prims *out, const float c[2], float r, int points, const float col[4],
                      float rot, pc_error *err)
{
    int k;
    pc_status st = PC_OK;
    for (k = 0; k < points && st == PC_OK; k++) {
        float a = rot + (float)k / (float)points * PC_FX_TAU, e[2] = {c[0] + cosf(a) * r, c[1] + sinf(a) * r};
        st = pc_algo_prims_push(out, pc_algo_prim_capsule(c, e, r * 0.28f, 0.4f, col, col), err);
    }
    return st;
}

static float wave_off(float s, float amp, float wl)
{
    return amp * sinf(s / wl * PC_FX_TAU);
}

/* Counts derived from the canvas past this are refused (the reference would run out of memory). */
#define MAX_STEPS ((size_t)1 << 22)

static pc_status steps_of(float v, size_t *out, pc_error *err)
{
    size_t n = pc_f32_to_size(v);
    if (n > MAX_STEPS) {
        return pc_fail(err, PC_ERR_LIMIT, "picture frame of %zu ornaments is too large", n);
    }
    *out = n;
    return PC_OK;
}

#define TRYP(x)                                                                                              \
    do {                                                                                                     \
        st = (x);                                                                                            \
        if (st != PC_OK) {                                                                                   \
            goto fail;                                                                                       \
        }                                                                                                    \
    } while (0)

pc_status pc_algo_picture_frame(const pc_algo_frame_spec *spec, pc_rect canvas, pc_algo_prims *out,
                                pc_error *err)
{
    pc_algo_prims o = {NULL, 0, 0};
    uint32_t cw = pc_rect_width(canvas), chh = pc_rect_height(canvas);
    float shortside = pc_rs_maxf_k((float)(cw < chh ? cw : chh), 8.0f);
    float margin = shortside * pc_clampf(spec->margin, 0.0f, 30.0f) / 100.0f;
    float unit = shortside * (0.008f + pc_clampf(spec->size, 1.0f, 100.0f) / 100.0f * 0.05f);
    float thick =
        pc_rs_maxf_k(unit * 0.04f + shortside * 0.00012f * pc_clampf(spec->thickness, 1.0f, 100.0f), 0.6f);
    float spacing = unit * (4.0f - 3.2f * pc_clampf(spec->arrangement, 1.0f, 100.0f) / 100.0f);
    float inset = margin + unit, total, step;
    border b;
    size_t count, k, lines;
    pc_algo_rng rng = pc_algo_rng_new(spec->seed, 0xF8A3Eu + (uint64_t)spec->frame);
    const float *vine = spec->vine_color, *fl = spec->flower_color, *lf = spec->leaf_color;
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f}, yellow[4] = {0.98f, 0.82f, 0.2f, 1.0f};
    uint32_t fi =
        spec->frame < 1 ? 1 : (spec->frame > PC_ALGO_FRAME_STYLES ? PC_ALGO_FRAME_STYLES : spec->frame);
    const char *style = pc_algo_frame_styles[fi - 1];
    pc_status st = PC_OK;
    float cc[4], cd[4];
    b.x0 = (float)canvas.x0 + inset;
    b.y0 = (float)canvas.y0 + inset;
    b.w = pc_rs_maxf_k((float)cw - 2.0f * inset, 1.0f);
    b.h = pc_rs_maxf_k((float)chh - 2.0f * inset, 1.0f);
    total = border_len(&b);
    TRYP(steps_of(pc_rs_maxf_k(roundf(total / spacing), 4.0f), &count, err));
    step = total / (float)count;
    lines = spec->lines < 1 ? 1 : (spec->lines > 5 ? 5 : spec->lines);
    if (strcmp(style, "vineWithFlowers") == 0 || strcmp(style, "vineWithLeaves") == 0 ||
        strcmp(style, "ivy") == 0 || strcmp(style, "roses") == 0 || strcmp(style, "daisies") == 0 ||
        strcmp(style, "berries") == 0) {
        float amp = unit * 0.5f, wl = step * 2.0f, prev[2] = {0, 0};
        size_t n;
        bool has_prev = false;
        /* A wavy vine along the border. */
        TRYP(steps_of(pc_rs_maxf_k(ceilf(total / 3.0f), 8.0f), &n, err));
        for (k = 0; k <= n; k++) {
            float s = (float)k / (float)n * total, p[2], t[2], nn[2], ov, q[2];
            border_at(&b, s, p, t, nn);
            ov = amp * sinf(s / wl * PC_FX_TAU);
            q[0] = p[0] + nn[0] * ov, q[1] = p[1] + nn[1] * ov;
            if (has_prev) {
                TRYP(pc_algo_prims_push(&o, pc_algo_prim_capsule(prev, q, thick, thick, vine, vine), err));
            }
            prev[0] = q[0], prev[1] = q[1];
            has_prev = true;
        }
        for (k = 0; k < count; k++) {
            float s = (float)k * step + step * 0.5f, p[2], t[2], nn[2], ov, q[2], side, ta, la, ll;
            border_at(&b, s, p, t, nn);
            ov = wave_off(s, amp, wl);
            q[0] = p[0] + nn[0] * ov, q[1] = p[1] + nn[1] * ov;
            side = k % 2 == 0 ? 1.0f : -1.0f;
            ta = atan2f(t[1], t[0]);
            la = ta + side * 0.9f + pc_algo_rng_sym(&rng) * 0.3f;
            ll = unit * pc_algo_rng_range(&rng, 0.9f, 1.2f);
            if (strcmp(style, "vineWithFlowers") == 0) {
                TRYP(leaf(&o, q, la, ll, lf, err));
                if (k % 2 == 0) {
                    float c[2] = {q[0] + nn[0] * unit * 0.7f * side, q[1] + nn[1] * unit * 0.7f * side};
                    TRYP(flower(&o, c, unit * 0.65f, 5, fl, yellow, pc_algo_rng_f(&rng) * 6.3f, err));
                }
            } else if (strcmp(style, "vineWithLeaves") == 0) {
                TRYP(leaf(&o, q, la, ll, lf, err));
                TRYP(leaf(&o, q, ta - side * 0.9f, ll * 0.8f, lf, err));
            } else if (strcmp(style, "ivy") == 0) {
                int j;
                for (j = 0; j < 3; j++) {
                    float a = la + ((float)j - 1.0f) * 0.7f;
                    pc_algo_render2_shade(lf, 0.75f + 0.15f * (float)j, cc);
                    TRYP(leaf(&o, q, a, ll * 0.75f, cc, err));
                }
            } else if (strcmp(style, "roses") == 0) {
                TRYP(leaf(&o, q, la, ll, lf, err));
                if (k % 3 == 0) {
                    const float fs[4] = {1.0f, 0.75f, 0.5f, 0.28f};
                    float r = unit * 0.7f;
                    int j;
                    for (j = 0; j < 4; j++) {
                        pc_algo_render2_shade(fl, 0.75f + 0.1f * (float)j, cc);
                        pc_algo_render2_shade(fl, 0.55f, cd);
                        TRYP(pc_algo_prims_push(&o,
                                                pc_algo_prim_ellipse(q, r * fs[j], r * fs[j] * 0.9f,
                                                                     pc_algo_rng_f(&rng) * 3.0f, cc, cd),
                                                err));
                    }
                }
            } else if (strcmp(style, "daisies") == 0) {
                TRYP(leaf(&o, q, la, ll * 0.8f, lf, err));
                if (k % 2 == 1) {
                    TRYP(flower(&o, q, unit * 0.6f, 12, white, yellow, pc_algo_rng_f(&rng), err));
                }
            } else {
                int j;
                TRYP(leaf(&o, q, la, ll, lf, err));
                for (j = 0; j < 3; j++) {
                    float c[2];
                    c[0] = q[0] + pc_algo_rng_sym(&rng) * unit * 0.4f;
                    c[1] = q[1] + pc_algo_rng_sym(&rng) * unit * 0.4f;
                    pc_algo_render2_shade(fl, 1.2f, cc);
                    pc_algo_render2_shade(fl, 0.6f, cd);
                    TRYP(pc_algo_prims_push(
                        &o, pc_algo_prim_ellipse(c, unit * 0.2f, unit * 0.2f, 0.0f, cc, cd), err));
                }
            }
        }
    } else if (strcmp(style, "bamboo") == 0) {
        int edge;
        for (edge = 0; edge < 4; edge++) {
            float s0 = edge == 0 ? 0.0f : edge == 1 ? b.w : edge == 2 ? b.w + b.h : 2.0f * b.w + b.h;
            float l = edge == 0 || edge == 2 ? b.w : b.h, a[2], z[2], t[2], nn[2];
            size_t joints, j, nlv;
            border_at(&b, s0 + 0.01f, a, t, nn);
            border_at(&b, s0 + l - 0.01f, z, t, nn);
            TRYP(steps_of(pc_rs_maxf_k(roundf(l / (spacing * 2.0f)), 1.0f), &joints, err));
            for (j = 0; j < joints; j++) {
                float f0 = (float)j / (float)joints, f1 = (float)(j + 1) / (float)joints;
                float p0[2] = {a[0] + (z[0] - a[0]) * f0, a[1] + (z[1] - a[1]) * f0};
                float p1[2] = {a[0] + (z[0] - a[0]) * f1, a[1] + (z[1] - a[1]) * f1};
                pc_algo_render2_shade(vine, 0.8f, cc);
                pc_algo_render2_shade(vine, 0.7f, cd);
                TRYP(pc_algo_prims_push(
                    &o, pc_algo_prim_capsule(p0, p1, unit * 0.35f, unit * 0.35f, vine, cc), err));
                TRYP(pc_algo_prims_push(&o, pc_algo_prim_disc(p1, unit * 0.4f, cd), err));
            }
            /* A few leaves per edge. */
            nlv = joints / 2 > 1 ? joints / 2 : 1;
            for (j = 0; j < nlv; j++) {
                float f = pc_algo_rng_f(&rng), p[2] = {a[0] + (z[0] - a[0]) * f, a[1] + (z[1] - a[1]) * f};
                float ang = pc_algo_rng_f(&rng) * 6.3f;
                TRYP(leaf(&o, p, ang, unit * 1.4f, lf, err));
            }
        }
    } else if (strcmp(style, "waves") == 0 || strcmp(style, "zigzag") == 0 ||
               strcmp(style, "doubleLine") == 0 || strcmp(style, "rope") == 0) {
        size_t li;
        bool rope = strcmp(style, "rope") == 0, dbl = strcmp(style, "doubleLine") == 0;
        for (li = 0; li < lines; li++) {
            float off = ((float)li - (float)(lines - 1) / 2.0f) * thick * 4.0f, prev[2] = {0, 0};
            bool has_prev = false;
            size_t n;
            TRYP(steps_of(pc_rs_maxf_k(ceilf(total / 2.0f), 8.0f), &n, err));
            for (k = 0; k <= n; k++) {
                float s = (float)k / (float)n * total, p[2], t[2], nn[2], ov, q[2];
                border_at(&b, s, p, t, nn);
                if (strcmp(style, "waves") == 0) {
                    ov = off + wave_off(s + (float)li * step * 0.25f, unit * 0.4f, step);
                } else if (strcmp(style, "zigzag") == 0) {
                    float ph = pc_fx_fract(s / step);
                    ov = off + unit * 0.4f * (4.0f * fabsf(ph - 0.5f) - 1.0f);
                } else {
                    ov = off + 0.0f;
                }
                q[0] = p[0] + nn[0] * ov, q[1] = p[1] + nn[1] * ov;
                if (rope) {
                    if (k % 2 == 0) {
                        border_at(&b, s, p, t, nn);
                        pc_algo_render2_shade(vine, 1.1f, cc);
                        pc_algo_render2_shade(vine, 0.65f, cd);
                        TRYP(pc_algo_prims_push(&o,
                                                pc_algo_prim_ellipse(q, unit * 0.45f, unit * 0.2f,
                                                                     atan2f(t[1], t[0]) + 0.7f, cc, cd),
                                                err));
                    }
                } else if (has_prev) {
                    TRYP(
                        pc_algo_prims_push(&o, pc_algo_prim_capsule(prev, q, thick, thick, vine, vine), err));
                }
                prev[0] = q[0], prev[1] = q[1];
                has_prev = true;
            }
            if (dbl) {
                /* A second rule inside each line and corner squares. */
                border in;
                int e;
                in.x0 = b.x0 + unit * 0.6f + off;
                in.y0 = b.y0 + unit * 0.6f + off;
                in.w = b.w - 2.0f * (unit * 0.6f + off);
                in.h = b.h - 2.0f * (unit * 0.6f + off);
                for (e = 0; e < 4; e++) {
                    const float sa[4] = {0.0f, in.w, in.w + in.h, 2.0f * in.w + in.h};
                    const float sb[4] = {in.w, in.w + in.h, 2.0f * in.w + in.h, border_len(&in) - 0.001f};
                    float p[2], q[2], t[2], nn[2];
                    border_at(&in, sa[e], p, t, nn);
                    border_at(&in, sb[e], q, t, nn);
                    TRYP(pc_algo_prims_push(
                        &o, pc_algo_prim_capsule(p, q, thick * 0.6f, thick * 0.6f, vine, vine), err));
                }
            }
        }
        if (dbl) {
            int e;
            for (e = 0; e < 4; e++) {
                const float sa[4] = {0.0f, b.w, b.w + b.h, 2.0f * b.w + b.h};
                float p[2], t[2], nn[2];
                border_at(&b, sa[e], p, t, nn);
                TRYP(pc_algo_prims_push(
                    &o, pc_algo_prim_ellipse(p, unit * 0.45f, unit * 0.45f, PC_FX_FRAC_PI_4, fl, fl), err));
            }
        }
    } else {
        /* dots, scallops, snowflakes, stars, hearts */
        for (k = 0; k < count; k++) {
            float s = (float)k * step, p[2], t[2], nn[2], ta;
            border_at(&b, s, p, t, nn);
            ta = atan2f(t[1], t[0]);
            if (strcmp(style, "dots") == 0) {
                float r = unit * (k % 2 == 0 ? 0.4f : 0.22f);
                pc_algo_render2_shade(fl, 1.15f, cc);
                pc_algo_render2_shade(fl, 0.75f, cd);
                TRYP(pc_algo_prims_push(&o, pc_algo_prim_ellipse(p, r, r, 0.0f, cc, cd), err));
            } else if (strcmp(style, "scallops") == 0) {
                float c0[2], t0[2], n0[2], mid[2], r = step * 0.5f;
                const int n = 8;
                int j;
                border_at(&b, s - step * 0.5f, c0, t0, n0);
                mid[0] = (c0[0] + p[0]) / 2.0f;
                mid[1] = (c0[1] + p[1]) / 2.0f;
                for (j = 0; j < n; j++) {
                    float a0 = PC_FX_PI * (float)j / (float)n, a1 = PC_FX_PI * (float)(j + 1) / (float)n,
                          q0[2], q1[2];
                    q0[0] = mid[0] - t[0] * r * cosf(a0) - nn[0] * r * sinf(a0);
                    q0[1] = mid[1] - t[1] * r * cosf(a0) - nn[1] * r * sinf(a0);
                    q1[0] = mid[0] - t[0] * r * cosf(a1) - nn[0] * r * sinf(a1);
                    q1[1] = mid[1] - t[1] * r * cosf(a1) - nn[1] * r * sinf(a1);
                    TRYP(pc_algo_prims_push(&o, pc_algo_prim_capsule(q0, q1, thick, thick, vine, vine), err));
                }
            } else if (strcmp(style, "snowflakes") == 0) {
                const float c[4] = {0.85f, 0.92f, 1.0f, 1.0f};
                float r = unit * 0.6f;
                int j;
                for (j = 0; j < 6; j++) {
                    float a = ta + (float)j * (PC_FX_PI / 3.0f), e[2], m[2];
                    int sd;
                    e[0] = p[0] + cosf(a) * r, e[1] = p[1] + sinf(a) * r;
                    TRYP(pc_algo_prims_push(&o,
                                            pc_algo_prim_capsule(p, e, pc_rs_maxf_k(thick * 0.6f, 0.6f),
                                                                 pc_rs_maxf_k(thick * 0.4f, 0.5f), c, c),
                                            err));
                    m[0] = p[0] + cosf(a) * r * 0.6f, m[1] = p[1] + sinf(a) * r * 0.6f;
                    for (sd = 0; sd < 2; sd++) {
                        float sdv = sd == 0 ? -0.7f : 0.7f, b2[2];
                        b2[0] = m[0] + cosf(a + sdv) * r * 0.3f;
                        b2[1] = m[1] + sinf(a + sdv) * r * 0.3f;
                        TRYP(pc_algo_prims_push(
                            &o, pc_algo_prim_capsule(m, b2, pc_rs_maxf_k(thick * 0.4f, 0.5f), 0.4f, c, c),
                            err));
                    }
                }
            } else if (strcmp(style, "stars") == 0) {
                TRYP(star(&o, p, unit * 0.55f, 5, k % 2 == 0 ? yellow : fl, ta - PC_FX_FRAC_PI_2, err));
            } else {
                /* Heart: two lobes and a point towards the inside. */
                float r = unit * 0.3f, l[2], rr[2], tip[2];
                l[0] = p[0] - t[0] * r * 0.8f - nn[0] * r * 0.3f;
                l[1] = p[1] - t[1] * r * 0.8f - nn[1] * r * 0.3f;
                rr[0] = p[0] + t[0] * r * 0.8f - nn[0] * r * 0.3f;
                rr[1] = p[1] + t[1] * r * 0.8f - nn[1] * r * 0.3f;
                tip[0] = p[0] + nn[0] * r * 1.6f;
                tip[1] = p[1] + nn[1] * r * 1.6f;
                TRYP(pc_algo_prims_push(&o, pc_algo_prim_capsule(l, tip, r, 0.6f, fl, fl), err));
                TRYP(pc_algo_prims_push(&o, pc_algo_prim_capsule(rr, tip, r, 0.6f, fl, fl), err));
            }
        }
    }
    /* Fade: lower the opacity of ornaments that reach further inside. */
    {
        float fade = pc_clampf(spec->fade, 0.0f, 100.0f) / 100.0f;
        if (fade > 0.0f) {
            float cx = (float)canvas.x0 + (float)cw / 2.0f, cy = (float)canvas.y0 + (float)chh / 2.0f;
            for (k = 0; k < o.n; k++) {
                pc_algo_prim *pr = &o.v[k];
                const float *c = pr->shape == PC_ALGO_PRIM_CAPSULE ? pr->a : pr->c;
                float edge = pc_clampf(
                    pc_rs_maxf(fabsf(c[0] - cx) / (b.w / 2.0f), fabsf(c[1] - cy) / (b.h / 2.0f)), 0.0f, 1.0f);
                float kk = pc_clampf(1.0f - fade * (1.0f - edge) * 8.0f, 0.0f, 1.0f);
                pr->c0[3] *= kk;
                pr->c1[3] *= kk;
            }
        }
    }
    *out = o;
    return PC_OK;
fail:
    pc_algo_prims_fini(&o);
    return st;
}
