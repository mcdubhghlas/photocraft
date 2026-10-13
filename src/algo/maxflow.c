/* Hybrid max-flow / min-cut (port of crates/algo/src/segment/maxflow.rs and
 * maxflow/push_relabel.rs).
 *
 * Boykov-Kolmogorov from Y. Boykov and V. Kolmogorov, "An Experimental Comparison of Min-Cut/
 * Max-Flow Algorithms for Energy Minimization in Vision", PAMI 26(9), 2004: two search trees grow
 * towards each other; each contact gives an augmenting path; saturated tree edges create orphans
 * that are re-adopted (or freed) before growing again. Large finite graphs with terminal links
 * both ways use FIFO push/relabel instead (Goldberg & Tarjan, JACM 35(4), 1988, with global
 * relabelling). Same queue discipline and float arithmetic as the reference, so cuts are
 * identical. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "raster/interrupt.h"

#define NONE UINT32_MAX
#define TERMINAL (UINT32_MAX - 1)
#define ORPHAN (UINT32_MAX - 2)
#define INF_DIST UINT32_MAX
/* Residual capacities at or below this count as saturated. */
#define EPS 1e-5f
/* How many steps the max-flow loops take between asks whether to stop. */
#define CANCEL_EVERY 1024u
/* Graphs from this many nodes may use push/relabel. */
#define PR_MIN_NODES 4096
/* Arc indices are u32 with the top three values reserved. */
#define MAX_ARCS ((size_t)UINT32_MAX - 3)

struct pc_algo_maxflow {
    size_t n;
    uint32_t *first;
    float *tr_cap; /* > 0 from the source, < 0 to the sink */
    uint32_t *parent;
    bool *is_sink;
    uint32_t *ts;
    uint32_t *dist;
    bool *active;
    /* arcs in pairs: a and its reverse a ^ 1 */
    uint32_t *head;
    uint32_t *next;
    float *r_cap;
    size_t narcs, cap;
    double flow;
};

/* A double-ended queue of node indices (VecDeque<u32>). */
typedef struct deque {
    uint32_t *d;
    size_t cap, head, len;
} deque;

static bool dq_reserve(deque *q)
{
    size_t ncap, i;
    uint32_t *nd;
    if (q->len < q->cap) {
        return true;
    }
    ncap = q->cap ? q->cap * 2 : 64;
    nd = pc_malloc_array(ncap, sizeof(uint32_t));
    if (nd == NULL) {
        return false;
    }
    /* q->cap is 0 only while the deque is empty (q->len == 0 too), so this loop never runs
     * then; the guard makes that explicit for the static analyzer (no % by zero). */
    if (q->cap != 0) {
        for (i = 0; i < q->len; i++) {
            nd[i] = q->d[(q->head + i) % q->cap];
        }
    }
    free(q->d);
    q->d = nd;
    q->cap = ncap;
    q->head = 0;
    return true;
}

static bool dq_push_back(deque *q, uint32_t v)
{
    if (!dq_reserve(q)) {
        return false;
    }
    q->d[(q->head + q->len) % q->cap] = v;
    q->len++;
    return true;
}

static bool dq_push_front(deque *q, uint32_t v)
{
    if (!dq_reserve(q)) {
        return false;
    }
    q->head = (q->head + q->cap - 1) % q->cap;
    q->d[q->head] = v;
    q->len++;
    return true;
}

static bool dq_pop_front(deque *q, uint32_t *v)
{
    if (q->len == 0) {
        return false;
    }
    *v = q->d[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return true;
}

pc_status pc_algo_maxflow_new(size_t nodes, size_t edges, pc_algo_maxflow **out, pc_error *err)
{
    pc_algo_maxflow *g;
    size_t i, arcs;
    if (nodes >= (size_t)ORPHAN || !pc_size_mul(edges, 2, &arcs) || arcs > MAX_ARCS) {
        return pc_fail(err, PC_ERR_LIMIT, "graph too large");
    }
    g = calloc(1, sizeof *g);
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph");
    }
    g->n = nodes;
    g->first = pc_malloc_array(nodes ? nodes : 1, sizeof(uint32_t));
    g->tr_cap = pc_calloc_array(nodes ? nodes : 1, sizeof(float));
    g->parent = pc_malloc_array(nodes ? nodes : 1, sizeof(uint32_t));
    g->is_sink = pc_calloc_array(nodes ? nodes : 1, sizeof(bool));
    g->ts = pc_calloc_array(nodes ? nodes : 1, sizeof(uint32_t));
    g->dist = pc_calloc_array(nodes ? nodes : 1, sizeof(uint32_t));
    g->active = pc_calloc_array(nodes ? nodes : 1, sizeof(bool));
    g->cap = arcs ? arcs : 2;
    g->head = pc_malloc_array(g->cap, sizeof(uint32_t));
    g->next = pc_malloc_array(g->cap, sizeof(uint32_t));
    g->r_cap = pc_malloc_array(g->cap, sizeof(float));
    if (g->first == NULL || g->tr_cap == NULL || g->parent == NULL || g->is_sink == NULL || g->ts == NULL ||
        g->dist == NULL || g->active == NULL || g->head == NULL || g->next == NULL || g->r_cap == NULL) {
        pc_algo_maxflow_free(g);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph");
    }
    for (i = 0; i < nodes; i++) {
        g->first[i] = NONE;
        g->parent[i] = NONE;
    }
    *out = g;
    return PC_OK;
}

void pc_algo_maxflow_free(pc_algo_maxflow *g)
{
    if (g == NULL) {
        return;
    }
    free(g->first);
    free(g->tr_cap);
    free(g->parent);
    free(g->is_sink);
    free(g->ts);
    free(g->dist);
    free(g->active);
    free(g->head);
    free(g->next);
    free(g->r_cap);
    free(g);
}

pc_status pc_algo_maxflow_clone(const pc_algo_maxflow *g, pc_algo_maxflow **out, pc_error *err)
{
    pc_algo_maxflow *c;
    size_t nn = g->n ? g->n : 1;
    pc_status st = pc_algo_maxflow_new(g->n, g->narcs / 2, &c, err);
    if (st != PC_OK) {
        return st;
    }
    memcpy(c->first, g->first, nn * sizeof(uint32_t));
    memcpy(c->tr_cap, g->tr_cap, nn * sizeof(float));
    memcpy(c->parent, g->parent, nn * sizeof(uint32_t));
    memcpy(c->is_sink, g->is_sink, nn * sizeof(bool));
    memcpy(c->ts, g->ts, nn * sizeof(uint32_t));
    memcpy(c->dist, g->dist, nn * sizeof(uint32_t));
    memcpy(c->active, g->active, nn * sizeof(bool));
    if (g->narcs > 0) {
        memcpy(c->head, g->head, g->narcs * sizeof(uint32_t));
        memcpy(c->next, g->next, g->narcs * sizeof(uint32_t));
        memcpy(c->r_cap, g->r_cap, g->narcs * sizeof(float));
    }
    c->narcs = g->narcs;
    c->flow = g->flow;
    *out = c;
    return PC_OK;
}

size_t pc_algo_maxflow_node_count(const pc_algo_maxflow *g)
{
    return g->n;
}

void pc_algo_maxflow_add_tweights(pc_algo_maxflow *g, size_t i, float cap_source, float cap_sink)
{
    float delta, s = cap_source, t = cap_sink;
    if (i >= g->n) {
        return;
    }
    /* Only the difference is kept; the common part is flow that must pass anyway. */
    delta = g->tr_cap[i];
    if (delta > 0.0f) {
        s += delta;
    } else {
        t -= delta;
    }
    g->flow += (double)pc_rs_minf(s, t);
    g->tr_cap[i] = s - t;
}

pc_status pc_algo_maxflow_add_edge(pc_algo_maxflow *g, size_t i, size_t j, float cap, float rev_cap,
                                   pc_error *err)
{
    uint32_t a;
    if (i == j || i >= g->n || j >= g->n) {
        return pc_fail(err, PC_ERR_INVALID, "bad edge %zu -> %zu", i, j);
    }
    if (g->narcs + 2 > g->cap) {
        size_t ncap = g->cap, ncap2, ncap3;
        void *h, *nx, *rc;
        if (g->narcs + 2 > MAX_ARCS) {
            return pc_fail(err, PC_ERR_LIMIT, "graph too large");
        }
        ncap2 = ncap;
        ncap3 = ncap;
        h = pc_grow_array(g->head, &ncap, g->narcs + 2, sizeof(uint32_t));
        if (h == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph");
        }
        g->head = h;
        nx = pc_grow_array(g->next, &ncap2, g->narcs + 2, sizeof(uint32_t));
        if (nx == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph");
        }
        g->next = nx;
        rc = pc_grow_array(g->r_cap, &ncap3, g->narcs + 2, sizeof(float));
        if (rc == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph");
        }
        g->r_cap = rc;
        /* The three arrays share one capacity: the smallest that all reached. */
        g->cap = ncap < ncap2 ? (ncap < ncap3 ? ncap : ncap3) : (ncap2 < ncap3 ? ncap2 : ncap3);
    }
    a = (uint32_t)g->narcs;
    g->head[a] = (uint32_t)j;
    g->next[a] = g->first[i];
    g->r_cap[a] = cap;
    g->first[i] = a;
    g->head[a + 1] = (uint32_t)i;
    g->next[a + 1] = g->first[j];
    g->r_cap[a + 1] = rev_cap;
    g->first[j] = a + 1;
    g->narcs += 2;
    return PC_OK;
}

bool pc_algo_maxflow_in_source(const pc_algo_maxflow *g, size_t i)
{
    return i < g->n && g->parent[i] != NONE && !g->is_sink[i];
}

double pc_algo_maxflow_flow(const pc_algo_maxflow *g)
{
    return g->flow;
}

/* Follows tree arcs up from x to its terminal; NULL-arc safety for corrupted trees. */
static bool arc_ok(const pc_algo_maxflow *g, uint32_t a)
{
    return (size_t)a < g->narcs;
}

static pc_status augment(pc_algo_maxflow *g, size_t m, deque *orphans, pc_error *err)
{
    size_t u = g->head[m ^ 1], v = g->head[m], x, steps;
    float b = g->r_cap[m];
    /* Bottleneck. */
    for (x = u, steps = 0; steps <= g->n; steps++) {
        uint32_t a = g->parent[x];
        if (a == TERMINAL || !arc_ok(g, a)) {
            break;
        }
        b = pc_rs_minf(b, g->r_cap[(size_t)a ^ 1]);
        x = g->head[a];
    }
    b = pc_rs_minf(b, g->tr_cap[x]);
    for (x = v, steps = 0; steps <= g->n; steps++) {
        uint32_t a = g->parent[x];
        if (a == TERMINAL || !arc_ok(g, a)) {
            break;
        }
        b = pc_rs_minf(b, g->r_cap[a]);
        x = g->head[a];
    }
    b = pc_rs_minf(b, -g->tr_cap[x]);
    /* Push. */
    g->r_cap[m ^ 1] += b;
    g->r_cap[m] -= b;
    for (x = u, steps = 0; steps <= g->n; steps++) {
        uint32_t a = g->parent[x];
        size_t up;
        if (a == TERMINAL || !arc_ok(g, a)) {
            break;
        }
        g->r_cap[a] += b;
        g->r_cap[(size_t)a ^ 1] -= b;
        up = g->head[a];
        if (g->r_cap[(size_t)a ^ 1] <= EPS) {
            g->parent[x] = ORPHAN;
            if (!dq_push_back(orphans, (uint32_t)x)) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
            }
        }
        x = up;
    }
    g->tr_cap[x] -= b;
    if (g->tr_cap[x] <= EPS) {
        g->parent[x] = ORPHAN;
        if (!dq_push_back(orphans, (uint32_t)x)) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
        }
    }
    for (x = v, steps = 0; steps <= g->n; steps++) {
        uint32_t a = g->parent[x];
        size_t up;
        if (a == TERMINAL || !arc_ok(g, a)) {
            break;
        }
        g->r_cap[(size_t)a ^ 1] += b;
        g->r_cap[a] -= b;
        up = g->head[a];
        if (g->r_cap[a] <= EPS) {
            g->parent[x] = ORPHAN;
            if (!dq_push_back(orphans, (uint32_t)x)) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
            }
        }
        x = up;
    }
    g->tr_cap[x] += b;
    if (g->tr_cap[x] >= -EPS) {
        g->parent[x] = ORPHAN;
        if (!dq_push_back(orphans, (uint32_t)x)) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
        }
    }
    g->flow += (double)b;
    return PC_OK;
}

static pc_status adopt(pc_algo_maxflow *g, size_t i, uint32_t time, deque *queue, deque *orphans,
                       pc_error *err)
{
    bool sink = g->is_sink[i];
    uint32_t best = NONE, d_min = INF_DIST, a;
    for (a = g->first[i]; a != NONE; a = g->next[a]) {
        float cap = sink ? g->r_cap[a] : g->r_cap[(size_t)a ^ 1];
        size_t j = g->head[a];
        if (cap > EPS && g->is_sink[j] == sink && g->parent[j] != NONE) {
            /* Does j's path lead to a terminal? */
            uint32_t d = 0;
            size_t k = j, steps = 0;
            for (;;) {
                uint32_t pa;
                if (g->ts[k] == time) {
                    d = d > UINT32_MAX - g->dist[k] ? UINT32_MAX : d + g->dist[k];
                    break;
                }
                pa = g->parent[k];
                d += 1;
                if (pa == TERMINAL) {
                    g->ts[k] = time;
                    g->dist[k] = 1;
                    break;
                }
                if (pa == ORPHAN || !arc_ok(g, pa) || ++steps > g->n) {
                    d = INF_DIST;
                    break;
                }
                k = g->head[pa];
            }
            if (d < INF_DIST) {
                uint32_t dd = d;
                if (d < d_min) {
                    best = a;
                    d_min = d;
                }
                /* Cache distances along the path. */
                k = j;
                steps = 0;
                while (g->ts[k] != time && steps++ <= g->n) {
                    uint32_t pa = g->parent[k];
                    g->ts[k] = time;
                    g->dist[k] = dd;
                    dd -= 1;
                    if (!arc_ok(g, pa)) {
                        break;
                    }
                    k = g->head[pa];
                }
            }
        }
    }
    if (best != NONE) {
        g->parent[i] = best;
        g->ts[i] = time;
        g->dist[i] = d_min + 1;
        return PC_OK;
    }
    /* No parent: i becomes free. */
    g->parent[i] = NONE;
    for (a = g->first[i]; a != NONE; a = g->next[a]) {
        size_t j = g->head[a];
        uint32_t pj = g->parent[j];
        if (g->is_sink[j] == sink && pj != NONE) {
            float cap = sink ? g->r_cap[a] : g->r_cap[(size_t)a ^ 1];
            if (cap > EPS && !g->active[j]) {
                g->active[j] = true;
                if (!dq_push_back(queue, (uint32_t)j)) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
                }
            }
            if (pj != TERMINAL && pj != ORPHAN && arc_ok(g, pj) && g->head[pj] == i) {
                g->parent[j] = ORPHAN;
                if (!dq_push_back(orphans, (uint32_t)j)) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
                }
            }
        }
    }
    return PC_OK;
}

/* Boykov-Kolmogorov; PC_ERR_CANCELLED when ctl stopped it (the cut is then meaningless). */
static pc_status bk(pc_algo_maxflow *g, const pc_interrupt *ctl, double *flow, pc_error *err)
{
    deque queue = {NULL, 0, 0, 0}, orphans = {NULL, 0, 0, 0};
    uint32_t time = 0, qi, steps = 0;
    size_t i;
    pc_status st = PC_OK;
    for (i = 0; i < g->n; i++) {
        g->active[i] = false;
        g->ts[i] = 0;
        if (fabsf(g->tr_cap[i]) > EPS) {
            g->is_sink[i] = g->tr_cap[i] < 0.0f;
            g->parent[i] = TERMINAL;
            g->dist[i] = 1;
            g->active[i] = true;
            if (!dq_push_back(&queue, (uint32_t)i)) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
                goto done;
            }
        } else {
            g->parent[i] = NONE;
        }
    }
    while (dq_pop_front(&queue, &qi)) {
        size_t ii = qi;
        uint32_t contact = NONE, a, oi;
        bool sink;
        steps++;
        if (steps % CANCEL_EVERY == 0 && pc_interrupt_cancelled(ctl)) {
            st = pc_fail(err, PC_ERR_CANCELLED, "cancelled");
            goto done;
        }
        g->active[ii] = false;
        if (g->parent[ii] == NONE) {
            continue;
        }
        /* Growth stage. */
        sink = g->is_sink[ii];
        for (a = g->first[ii]; a != NONE; a = g->next[a]) {
            size_t j = g->head[a];
            float cap = sink ? g->r_cap[(size_t)a ^ 1] : g->r_cap[a];
            if (cap > EPS) {
                if (g->parent[j] == NONE) {
                    g->is_sink[j] = sink;
                    g->parent[j] = a ^ 1;
                    g->ts[j] = g->ts[ii];
                    g->dist[j] = g->dist[ii] + 1;
                    if (!g->active[j]) {
                        g->active[j] = true;
                        if (!dq_push_back(&queue, (uint32_t)j)) {
                            st = pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
                            goto done;
                        }
                    }
                } else if (g->is_sink[j] != sink) {
                    /* Arc from the S side to the T side. */
                    contact = sink ? a ^ 1 : a;
                    break;
                } else if (g->ts[j] <= g->ts[ii] && g->dist[j] > g->dist[ii]) {
                    /* Heuristic: shorten paths to the terminal. */
                    g->parent[j] = a ^ 1;
                    g->ts[j] = g->ts[ii];
                    g->dist[j] = g->dist[ii] + 1;
                }
            }
        }
        if (contact == NONE) {
            continue;
        }
        /* Keep scanning this node after the augmentation. */
        if (!g->active[ii]) {
            g->active[ii] = true;
            if (!dq_push_front(&queue, (uint32_t)ii)) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
                goto done;
            }
        }
        time += 1;
        st = augment(g, contact, &orphans, err);
        if (st != PC_OK) {
            goto done;
        }
        /* Adoption stage. */
        while (dq_pop_front(&orphans, &oi)) {
            st = adopt(g, oi, time, &queue, &orphans, err);
            if (st != PC_OK) {
                goto done;
            }
        }
    }
    *flow = g->flow;
done:
    free(queue.d);
    free(orphans.d);
    return st;
}

/* ---- FIFO push/relabel (push_relabel.rs) ----
 *
 * Terminal source arcs are saturated into a preflow; `back` keeps their reverse capacity and
 * `excess` is f64 so local f32 capacity updates don't accumulate rounding drift. `dist` holds the
 * labels and `parent` each node's current arc. */

/* BFS over residual arcs into `queue` from the nodes already in it (labels dist + 1). */
static bool relabel_bfs(pc_algo_maxflow *g, deque *q)
{
    uint32_t qi;
    while (dq_pop_front(q, &qi)) {
        size_t i = qi;
        uint32_t a;
        for (a = g->first[i]; a != NONE; a = g->next[a]) {
            size_t j = g->head[a];
            if (g->r_cap[(size_t)a ^ 1] > EPS && g->dist[j] == UINT32_MAX) {
                g->dist[j] = g->dist[i] == UINT32_MAX ? UINT32_MAX : g->dist[i] + 1;
                if (!dq_push_back(q, (uint32_t)j)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Exact distances to the sink, then (for sink-unreachable nodes) back to the source. */
static bool global_relabel(pc_algo_maxflow *g, uint32_t source_height, const float *back, deque *q)
{
    size_t i;
    q->len = 0;
    q->head = 0;
    for (i = 0; i < g->n; i++) {
        g->dist[i] = UINT32_MAX;
        g->parent[i] = g->first[i];
    }
    for (i = 0; i < g->n; i++) {
        if (g->tr_cap[i] < -EPS) {
            g->dist[i] = 1;
            if (!dq_push_back(q, (uint32_t)i)) {
                return false;
            }
        }
    }
    if (!relabel_bfs(g, q)) {
        return false;
    }
    /* Give sink-unreachable vertices exact distances back to the source; resetting all of them
     * to the same height can undo discharge progress. */
    for (i = 0; i < g->n; i++) {
        if (back[i] > EPS && g->dist[i] == UINT32_MAX) {
            g->dist[i] = source_height + 1;
            if (!dq_push_back(q, (uint32_t)i)) {
                return false;
            }
        }
    }
    return relabel_bfs(g, q);
}

/* One node's discharge: pushes to the sink, back to the source and along admissible arcs, and
 * relabels until its excess is gone or it has no residual arc left. */
static bool discharge(pc_algo_maxflow *g, size_t i, uint32_t source_height, float *back, double *excess,
                      deque *queue, size_t *work)
{
    while (excess[i] > (double)EPS) {
        uint32_t a, height;
        if (g->dist[i] == 1 && g->tr_cap[i] < -EPS) {
            float delta = pc_rs_minf(pc_f64_to_f32(excess[i]), -g->tr_cap[i]);
            g->tr_cap[i] += delta;
            excess[i] -= (double)delta;
            g->flow += (double)delta;
            if (excess[i] <= (double)EPS) {
                break;
            }
        }
        if (g->dist[i] == source_height + 1 && back[i] > EPS) {
            float delta = pc_rs_minf(pc_f64_to_f32(excess[i]), back[i]);
            back[i] -= delta;
            g->tr_cap[i] += delta;
            excess[i] -= (double)delta;
            if (excess[i] <= (double)EPS) {
                break;
            }
        }
        a = g->parent[i];
        while (a != NONE) {
            size_t j = g->head[a];
            uint32_t dj = g->dist[j];
            *work += 1;
            if (g->r_cap[a] > EPS && g->dist[i] == (dj == UINT32_MAX ? UINT32_MAX : dj + 1)) {
                float delta = pc_rs_minf(pc_f64_to_f32(excess[i]), g->r_cap[a]);
                g->r_cap[a] -= delta;
                g->r_cap[(size_t)a ^ 1] += delta;
                excess[i] -= (double)delta;
                excess[j] += (double)delta;
                if (!g->active[j] && excess[j] > (double)EPS) {
                    g->active[j] = true;
                    if (!dq_push_back(queue, (uint32_t)j)) {
                        return false;
                    }
                }
                if (excess[i] <= (double)EPS) {
                    break;
                }
            }
            a = g->next[a];
            g->parent[i] = a;
        }
        if (excess[i] <= (double)EPS) {
            break;
        }
        height = g->tr_cap[i] < -EPS ? 0 : UINT32_MAX;
        if (back[i] > EPS && source_height < height) {
            height = source_height;
        }
        for (a = g->first[i]; a != NONE; a = g->next[a]) {
            *work += 1;
            if (g->r_cap[a] > EPS && g->dist[g->head[a]] < height) {
                height = g->dist[g->head[a]];
            }
        }
        if (height == UINT32_MAX) {
            break;
        }
        g->dist[i] = height + 1;
        g->parent[i] = g->first[i];
    }
    return true;
}

static pc_status push_relabel(pc_algo_maxflow *g, const pc_interrupt *ctl, double *flow, pc_error *err)
{
    size_t n = g->n, i, work = 0, frequency;
    uint32_t source_height, steps = 0, qi;
    float *back;
    double *excess;
    deque queue = {NULL, 0, 0, 0}, rq = {NULL, 0, 0, 0};
    pc_status st = PC_OK;
    if (n >= UINT32_MAX / 2) {
        return bk(g, ctl, flow, err);
    }
    source_height = (uint32_t)n + 2;
    back = pc_calloc_array(n ? n : 1, sizeof(float));
    excess = pc_calloc_array(n ? n : 1, sizeof(double));
    if (back == NULL || excess == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
        goto done;
    }
    for (i = 0; i < n; i++) {
        g->active[i] = false;
        if (g->tr_cap[i] > EPS) {
            back[i] = g->tr_cap[i];
            excess[i] = (double)g->tr_cap[i];
            g->tr_cap[i] = 0.0f;
        }
    }
    if (!global_relabel(g, source_height, back, &rq)) {
        goto nomem;
    }
    for (i = 0; i < n; i++) {
        if (excess[i] > (double)EPS) {
            g->active[i] = true;
            if (!dq_push_back(&queue, (uint32_t)i)) {
                goto nomem;
            }
        }
    }
    /* r_cap.len() * 2 + n, saturating */
    frequency = g->narcs > (SIZE_MAX - n) / 2 ? SIZE_MAX : g->narcs * 2 + n;
    while (dq_pop_front(&queue, &qi)) {
        steps++;
        if (steps % CANCEL_EVERY == 0 && pc_interrupt_cancelled(ctl)) {
            st = pc_fail(err, PC_ERR_CANCELLED, "cancelled");
            goto done;
        }
        g->active[qi] = false;
        if (!discharge(g, qi, source_height, back, excess, &queue, &work)) {
            goto nomem;
        }
        if (work >= frequency) {
            if (!global_relabel(g, source_height, back, &rq)) {
                goto nomem;
            }
            work = 0;
        }
    }
    /* Report the smallest source-side cut, independently of discharge order. */
    for (i = 0; i < n; i++) {
        g->parent[i] = NONE;
        g->is_sink[i] = true;
    }
    for (i = 0; i < n; i++) {
        if (g->tr_cap[i] > EPS) {
            g->parent[i] = TERMINAL;
            g->is_sink[i] = false;
            if (!dq_push_back(&queue, (uint32_t)i)) {
                goto nomem;
            }
        }
    }
    while (dq_pop_front(&queue, &qi)) {
        uint32_t a;
        for (a = g->first[qi]; a != NONE; a = g->next[a]) {
            size_t j = g->head[a];
            if (g->r_cap[a] > EPS && g->parent[j] == NONE) {
                g->parent[j] = TERMINAL;
                g->is_sink[j] = false;
                if (!dq_push_back(&queue, (uint32_t)j)) {
                    goto nomem;
                }
            }
        }
    }
    *flow = g->flow;
    goto done;
nomem:
    st = pc_fail(err, PC_ERR_NOMEM, "out of memory in max-flow");
done:
    free(back);
    free(excess);
    free(queue.d);
    free(rq.d);
    return st;
}

pc_status pc_algo_maxflow_run_with(pc_algo_maxflow *g, const pc_interrupt *ctl, double *flow, pc_error *err)
{
    if (g->n >= PR_MIN_NODES) {
        bool source = false, sink = false;
        size_t i;
        for (i = 0; i < g->n; i++) {
            float c = g->tr_cap[i];
            if (!isfinite(c)) {
                return bk(g, ctl, flow, err);
            }
            source |= c > EPS;
            sink |= c < -EPS;
        }
        if (source && sink) {
            bool ok = true;
            for (i = 0; i < g->narcs && ok; i++) {
                ok = isfinite(g->r_cap[i]) && g->r_cap[i] >= 0.0f;
            }
            if (ok) {
                return push_relabel(g, ctl, flow, err);
            }
        }
    }
    return bk(g, ctl, flow, err);
}

pc_status pc_algo_maxflow_run(pc_algo_maxflow *g, double *flow, pc_error *err)
{
    return pc_algo_maxflow_run_with(g, NULL, flow, err);
}

pc_status pc_algo_maxflow_run_bk(pc_algo_maxflow *g, double *flow, pc_error *err)
{
    return bk(g, NULL, flow, err);
}

pc_status pc_algo_maxflow_run_push_relabel(pc_algo_maxflow *g, double *flow, pc_error *err)
{
    return push_relabel(g, NULL, flow, err);
}

float pc_algo_maxflow_residual_out_of_source_side(const pc_algo_maxflow *g)
{
    float s = 0.0f;
    size_t i;
    for (i = 0; i < g->n; i++) {
        uint32_t a;
        if (!pc_algo_maxflow_in_source(g, i)) {
            continue;
        }
        for (a = g->first[i]; a != NONE; a = g->next[a]) {
            if (!pc_algo_maxflow_in_source(g, g->head[a])) {
                s += g->r_cap[a];
            }
        }
    }
    return s;
}
