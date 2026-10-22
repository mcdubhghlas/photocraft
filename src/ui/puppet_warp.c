/* Edit > Puppet Warp UI model. See puppet_warp.h. Clean-room port of puppet_ui.rs. */
#include "ui/puppet_warp.h"

#include <math.h>
#include <string.h>

#include "base/mem.h"

enum { PUPPET_SOLVE_EXACT = 40, PUPPET_SOLVE_WARM = 6 };

/* ---- pin-list snapshots (session-local undo/redo) ---- */

static void snapshot_free(pc_ui_puppet_snapshot *s)
{
    if (s == NULL)
        return;
    free(s->pins);
    s->pins = NULL;
    s->n = 0;
}

static pc_status snapshot_copy(pc_ui_puppet_snapshot *out, const pc_algo_puppet_pin *pins, size_t n,
                              pc_error *err)
{
    out->pins = NULL;
    out->n = 0;
    if (n == 0)
        return PC_OK;
    out->pins = pc_malloc_array(n, sizeof *out->pins);
    if (out->pins == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    memcpy(out->pins, pins, n * sizeof *out->pins);
    out->n = n;
    return PC_OK;
}

static bool pins_eq(const pc_algo_puppet_pin *a, size_t an, const pc_algo_puppet_pin *b, size_t bn)
{
    if (an != bn)
        return false;
    for (size_t i = 0; i < an; i++) {
        if (a[i].src[0] != b[i].src[0] || a[i].src[1] != b[i].src[1] || a[i].dst[0] != b[i].dst[0] ||
            a[i].dst[1] != b[i].dst[1] || a[i].depth != b[i].depth ||
            a[i].rotate.set != b[i].rotate.set || (a[i].rotate.set && a[i].rotate.deg != b[i].rotate.deg))
            return false;
    }
    return true;
}

static pc_status push_snapshot(pc_ui_puppet_snapshot **stack, size_t *n, size_t *cap,
                               pc_ui_puppet_snapshot snap, pc_error *err)
{
    void *grown = pc_grow_array(*stack, cap, *n + 1, sizeof **stack);
    if (grown == NULL) {
        snapshot_free(&snap);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *stack = grown;
    (*stack)[(*n)++] = snap;
    return PC_OK;
}

static void clear_stack(pc_ui_puppet_snapshot *stack, size_t n)
{
    for (size_t i = 0; i < n; i++)
        snapshot_free(&stack[i]);
}

/* Record `before` as an undo step if it differs from the current pins; otherwise free it. Always
 * consumes `before`. A new edit clears the redo stack. */
static pc_status record(pc_ui_puppet_session *s, pc_ui_puppet_snapshot before, pc_error *err)
{
    if (pins_eq(before.pins, before.n, s->warp.pins, s->warp.npins)) {
        snapshot_free(&before);
        return PC_OK;
    }
    clear_stack(s->redo, s->nredo);
    s->nredo = 0;
    return push_snapshot(&s->undo, &s->nundo, &s->cap_undo, before, err);
}

/* ---- mesh / solver ---- */

static pc_rect rect_of(const pc_ui_puppet_session *s)
{
    pc_rect r = {(int32_t)lround(s->bounds[0]), (int32_t)lround(s->bounds[1]),
                 (int32_t)lround(s->bounds[2]), (int32_t)lround(s->bounds[3])};
    return r;
}

static void free_solve(pc_ui_puppet_session *s)
{
    free(s->deformed);
    s->deformed = NULL;
    s->ndeformed = 0;
    free(s->order);
    s->order = NULL;
    s->norder = 0;
}

/* Re-solve the current solver with the current pin dst/rotations/mode. */
static pc_status solve_current(pc_ui_puppet_session *s, bool exact, pc_error *err)
{
    if (s->solver == NULL) {
        free_solve(s);
        return PC_OK;
    }
    const pc_algo_puppet_mesh *m = pc_algo_puppet_solver_mesh(s->solver);
    size_t nverts = m->nverts, np = s->warp.npins;
    double(*dst)[2] = NULL;
    pc_algo_puppet_rot *rots = NULL;
    if (np > 0) {
        dst = pc_malloc_array(np, sizeof *dst);
        rots = pc_malloc_array(np, sizeof *rots);
        if (dst == NULL || rots == NULL) {
            free(dst);
            free(rots);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t k = 0; k < np; k++) {
            dst[k][0] = s->warp.pins[k].dst[0];
            dst[k][1] = s->warp.pins[k].dst[1];
            rots[k] = s->warp.pins[k].rotate;
        }
    }
    const double(*init)[2] = (s->ndeformed == nverts) ? (const double(*)[2])s->deformed : NULL;
    size_t ninit = init ? nverts : 0;
    double(*v)[2] = NULL;
    double *energies = NULL;
    pc_status rc = pc_algo_puppet_solve(s->solver, (const double(*)[2])dst, np, rots, np, s->warp.mode,
                                        exact ? PUPPET_SOLVE_EXACT : PUPPET_SOLVE_WARM, init, ninit, &v,
                                        &energies, err);
    free(energies);
    if (rc != PC_OK) {
        free(dst);
        free(rots);
        return rc;
    }
    free(s->deformed);
    s->deformed = v;
    s->ndeformed = nverts;

    int32_t *depths = NULL;
    double(*src)[2] = NULL;
    if (np > 0) {
        depths = pc_malloc_array(np, sizeof *depths);
        src = pc_malloc_array(np, sizeof *src);
        if (depths == NULL || src == NULL) {
            free(dst);
            free(rots);
            free(depths);
            free(src);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t k = 0; k < np; k++) {
            depths[k] = s->warp.pins[k].depth;
            src[k][0] = s->warp.pins[k].src[0];
            src[k][1] = s->warp.pins[k].src[1];
        }
    }
    size_t *order = NULL;
    rc = pc_algo_puppet_draw_order(s->solver, depths, np, (const double(*)[2])src, np, &order, err);
    free(dst);
    free(rots);
    free(depths);
    free(src);
    if (rc != PC_OK)
        return rc;
    free(s->order);
    s->order = order;
    s->norder = m->ntris;
    return PC_OK;
}

/* Rebuild the mesh + solver from the surface and bind the current pin set, then solve exactly.
 * On an empty mesh (no opaque pixels at this density/expansion) sets *empty and leaves no solver. */
static pc_status build_solver(pc_ui_puppet_session *s, bool *empty, pc_error *err)
{
    if (empty != NULL)
        *empty = false;
    pc_algo_puppet_mesh mesh;
    memset(&mesh, 0, sizeof mesh);
    pc_status rc = pc_algo_puppet_build_mesh(&s->surface, rect_of(s), s->warp.density, s->warp.expansion,
                                             &mesh, err);
    if (rc != PC_OK)
        return rc;
    if (mesh.ntris == 0) {
        pc_algo_puppet_mesh_fini(&mesh);
        pc_algo_puppet_solver_free(s->solver);
        s->solver = NULL;
        free_solve(s);
        if (empty != NULL)
            *empty = true;
        return PC_OK;
    }
    size_t np = s->warp.npins;
    double(*src)[2] = NULL;
    bool *fixed = NULL;
    if (np > 0) {
        src = pc_malloc_array(np, sizeof *src);
        fixed = pc_malloc_array(np, sizeof *fixed);
        if (src == NULL || fixed == NULL) {
            free(src);
            free(fixed);
            pc_algo_puppet_mesh_fini(&mesh);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t k = 0; k < np; k++) {
            src[k][0] = s->warp.pins[k].src[0];
            src[k][1] = s->warp.pins[k].src[1];
            fixed[k] = s->warp.pins[k].rotate.set;
        }
    }
    pc_algo_puppet_solver *solver = NULL;
    rc = pc_algo_puppet_solver_new(&mesh, (const double(*)[2])src, np, fixed, np, &solver, err);
    free(src);
    free(fixed);
    if (rc != PC_OK)
        return rc; /* solver_new freed the mesh */
    pc_algo_puppet_solver_free(s->solver);
    s->solver = solver;
    free_solve(s);
    return solve_current(s, true, err);
}

/* Rebuild after a pin-set change (mesh unchanged, but a new solver binds the new pins). */
static pc_status rebind(pc_ui_puppet_session *s, pc_error *err)
{
    return build_solver(s, NULL, err);
}

static pc_status remesh_or_restore(pc_ui_puppet_session *s, pc_algo_puppet_density prev_density,
                                   double prev_expansion, pc_error *err)
{
    bool empty = false;
    pc_status rc = build_solver(s, &empty, err);
    if (rc != PC_OK)
        return rc;
    if (empty) {
        s->warp.density = prev_density;
        s->warp.expansion = prev_expansion;
        (void)build_solver(s, NULL, NULL);
        return pc_fail(err, PC_ERR_INVALID,
                       "Could not use Puppet Warp: no opaque pixels at this density and expansion");
    }
    return PC_OK;
}

/* ---- hit testing ---- */

static bool pin_at(const pc_ui_puppet_session *s, double x, double y, double tol, size_t *out)
{
    bool found = false;
    double best = tol;
    for (size_t k = 0; k < s->warp.npins; k++) {
        double d = hypot(s->warp.pins[k].dst[0] - x, s->warp.pins[k].dst[1] - y);
        if (d <= best) {
            best = d;
            *out = k;
            found = true;
        }
    }
    return found;
}

/* Where a new pin placed at (x,y) attaches: the rest (undeformed) point under the deformed mesh. */
static bool rest_of(const pc_ui_puppet_session *s, double x, double y, double out[2])
{
    if (s->solver == NULL || s->deformed == NULL)
        return false;
    const pc_algo_puppet_mesh *m = pc_algo_puppet_solver_mesh(s->solver);
    for (size_t idx = s->norder; idx-- > 0;) {
        size_t t = s->order[idx];
        if (t >= m->ntris)
            continue;
        size_t i0 = m->tris[t][0], i1 = m->tris[t][1], i2 = m->tris[t][2];
        if (i0 >= s->ndeformed || i1 >= s->ndeformed || i2 >= s->ndeformed)
            continue;
        if (i0 >= m->nverts || i1 >= m->nverts || i2 >= m->nverts)
            continue;
        double ax = s->deformed[i0][0], ay = s->deformed[i0][1];
        double v0x = s->deformed[i1][0] - ax, v0y = s->deformed[i1][1] - ay;
        double v1x = s->deformed[i2][0] - ax, v1y = s->deformed[i2][1] - ay;
        double v2x = x - ax, v2y = y - ay;
        double den = v0x * v1y - v1x * v0y;
        if (fabs(den) < 1e-12)
            continue;
        double l1 = (v2x * v1y - v1x * v2y) / den;
        double l2 = (v0x * v2y - v2x * v0y) / den;
        double l0 = 1.0 - l1 - l2;
        if (l0 >= 0.0 && l1 >= 0.0 && l2 >= 0.0) {
            out[0] = l0 * m->verts[i0][0] + l1 * m->verts[i1][0] + l2 * m->verts[i2][0];
            out[1] = l0 * m->verts[i0][1] + l1 * m->verts[i1][1] + l2 * m->verts[i2][1];
            return true;
        }
    }
    return false;
}

static pc_status push_pin(pc_ui_puppet_session *s, const pc_algo_puppet_pin *p, pc_error *err)
{
    size_t cap = s->warp.npins; /* algo pin arrays are exact; grow with a local capacity */
    void *grown = pc_grow_array(s->warp.pins, &cap, s->warp.npins + 1, sizeof *s->warp.pins);
    if (grown == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    s->warp.pins = grown;
    s->warp.pins[s->warp.npins++] = *p;
    return PC_OK;
}

static void remove_pin(pc_ui_puppet_session *s, size_t i)
{
    if (i >= s->warp.npins)
        return;
    memmove(&s->warp.pins[i], &s->warp.pins[i + 1], (s->warp.npins - i - 1) * sizeof *s->warp.pins);
    s->warp.npins--;
}

/* ---- lifecycle ---- */

pc_status pc_ui_puppet_begin(pc_ui_puppet_session *s, uint64_t key, uint64_t layer, const char *command,
                             const double bounds[4], const pc_surface *surface, pc_error *err)
{
    if (s == NULL || bounds == NULL || surface == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    for (size_t i = 0; i < 4; i++)
        if (!isfinite(bounds[i]))
            return pc_fail(err, PC_ERR_INVALID, "puppet warp needs finite bounds");
    pc_ui_puppet_fini(s);
    memset(s, 0, sizeof *s);
    s->active = true;
    s->key = key;
    s->layer = layer;
    if (command != NULL) {
        size_t n = strlen(command);
        if (n >= sizeof s->command)
            n = sizeof s->command - 1;
        memcpy(s->command, command, n);
        s->command[n] = '\0';
    }
    memcpy(s->bounds, bounds, sizeof s->bounds);
    pc_status rc = pc_surface_clone(surface, &s->surface, err);
    if (rc != PC_OK) {
        s->active = false;
        return rc;
    }
    s->warp = pc_algo_puppet_warp_make();
    s->show_mesh = true;
    bool empty = false;
    rc = build_solver(s, &empty, err);
    if (rc != PC_OK) {
        pc_ui_puppet_fini(s);
        return rc;
    }
    if (empty) {
        pc_ui_puppet_fini(s);
        return pc_fail(err, PC_ERR_INVALID, "Puppet Warp needs a layer with opaque pixels");
    }
    return PC_OK;
}

void pc_ui_puppet_fini(pc_ui_puppet_session *s)
{
    if (s == NULL)
        return;
    pc_algo_puppet_solver_free(s->solver);
    free_solve(s);
    pc_algo_puppet_warp_fini(&s->warp);
    pc_surface_fini(&s->surface);
    snapshot_free(&s->drag_from);
    clear_stack(s->undo, s->nundo);
    clear_stack(s->redo, s->nredo);
    free(s->undo);
    free(s->redo);
    memset(s, 0, sizeof *s);
}

/* ---- pointer ---- */

pc_status pc_ui_puppet_pointer(pc_ui_puppet_session *s, pc_ui_pointer_kind kind, double x, double y,
                               double tol, bool alt, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no puppet session");
    if (!isfinite(x) || !isfinite(y) || !(tol >= 0.0))
        return pc_fail(err, PC_ERR_INVALID, "bad puppet pointer");

    if (kind == PC_UI_POINTER_CANCEL) {
        s->has_drag = false;
        snapshot_free(&s->drag_from);
        s->has_drag_from = false;
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_DOWN) {
        pc_ui_puppet_snapshot before;
        pc_status rc = snapshot_copy(&before, s->warp.pins, s->warp.npins, err);
        if (rc != PC_OK)
            return rc;
        size_t i = 0;
        if (pin_at(s, x, y, tol, &i)) {
            if (alt) {
                remove_pin(s, i);
                s->has_selected = false;
                rc = rebind(s, err);
                if (rc != PC_OK) {
                    snapshot_free(&before);
                    return rc;
                }
                return record(s, before, err);
            }
            s->has_selected = true;
            s->selected = i;
            s->has_drag = true;
            s->drag = i;
            snapshot_free(&s->drag_from);
            s->drag_from = before;
            s->has_drag_from = true;
            return PC_OK;
        }
        double src[2];
        if (!alt && rest_of(s, x, y, src)) {
            pc_algo_puppet_pin p;
            memset(&p, 0, sizeof p);
            p.src[0] = src[0];
            p.src[1] = src[1];
            p.dst[0] = x;
            p.dst[1] = y;
            rc = push_pin(s, &p, err);
            if (rc != PC_OK) {
                snapshot_free(&before);
                return rc;
            }
            s->has_selected = true;
            s->selected = s->warp.npins - 1;
            s->has_drag = true;
            s->drag = s->warp.npins - 1;
            rc = rebind(s, err);
            if (rc != PC_OK) {
                snapshot_free(&before);
                return rc;
            }
            return record(s, before, err);
        }
        snapshot_free(&before); /* click off the mesh adds nothing */
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_MOVE) {
        if (s->has_drag && s->drag < s->warp.npins) {
            if (s->warp.pins[s->drag].dst[0] != x || s->warp.pins[s->drag].dst[1] != y) {
                s->warp.pins[s->drag].dst[0] = x;
                s->warp.pins[s->drag].dst[1] = y;
                return solve_current(s, false, err);
            }
        }
        return PC_OK;
    }
    /* PC_UI_POINTER_UP */
    if (s->has_drag && s->drag < s->warp.npins) {
        s->warp.pins[s->drag].dst[0] = x;
        s->warp.pins[s->drag].dst[1] = y;
        pc_status rc = solve_current(s, true, err);
        if (rc != PC_OK) {
            s->has_drag = false;
            return rc;
        }
    }
    s->has_drag = false;
    if (s->has_drag_from) {
        pc_ui_puppet_snapshot from = s->drag_from;
        s->drag_from.pins = NULL;
        s->drag_from.n = 0;
        s->has_drag_from = false;
        return record(s, from, err);
    }
    return PC_OK;
}

/* ---- options ---- */

pc_status pc_ui_puppet_set_mode(pc_ui_puppet_session *s, pc_algo_puppet_mode mode, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    s->warp.mode = mode;
    return solve_current(s, true, err);
}

pc_status pc_ui_puppet_set_density(pc_ui_puppet_session *s, pc_algo_puppet_density density, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    pc_algo_puppet_density prev = s->warp.density;
    if (density == prev)
        return PC_OK;
    s->warp.density = density;
    return remesh_or_restore(s, prev, s->warp.expansion, err);
}

pc_status pc_ui_puppet_set_expansion(pc_ui_puppet_session *s, double expansion, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (!isfinite(expansion))
        expansion = 0.0;
    if (expansion > 200.0)
        expansion = 200.0;
    if (expansion < -200.0)
        expansion = -200.0;
    double prev = s->warp.expansion;
    if (expansion == prev)
        return PC_OK;
    s->warp.expansion = expansion;
    return remesh_or_restore(s, s->warp.density, prev, err);
}

void pc_ui_puppet_set_show_mesh(pc_ui_puppet_session *s, bool on)
{
    if (s != NULL)
        s->show_mesh = on;
}

pc_status pc_ui_puppet_nudge_depth(pc_ui_puppet_session *s, int delta, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (!s->has_selected || s->selected >= s->warp.npins)
        return PC_OK;
    pc_ui_puppet_snapshot before;
    pc_status rc = snapshot_copy(&before, s->warp.pins, s->warp.npins, err);
    if (rc != PC_OK)
        return rc;
    int64_t d = (int64_t)s->warp.pins[s->selected].depth + delta;
    if (d > INT32_MAX)
        d = INT32_MAX;
    if (d < INT32_MIN)
        d = INT32_MIN;
    s->warp.pins[s->selected].depth = (int32_t)d;
    rc = solve_current(s, true, err);
    if (rc != PC_OK) {
        snapshot_free(&before);
        return rc;
    }
    return record(s, before, err);
}

pc_status pc_ui_puppet_set_rotation(pc_ui_puppet_session *s, bool fixed, double deg, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (!s->has_selected || s->selected >= s->warp.npins)
        return PC_OK;
    if (fixed && !isfinite(deg))
        return pc_fail(err, PC_ERR_INVALID, "rotation must be finite");
    pc_ui_puppet_snapshot before;
    pc_status rc = snapshot_copy(&before, s->warp.pins, s->warp.npins, err);
    if (rc != PC_OK)
        return rc;
    bool was_fixed = s->warp.pins[s->selected].rotate.set;
    s->warp.pins[s->selected].rotate.set = fixed;
    s->warp.pins[s->selected].rotate.deg = fixed ? deg : 0.0;
    /* Toggling fixed changes the solver's fixed-rotation set, so rebind; a value change re-solves. */
    rc = (fixed != was_fixed) ? rebind(s, err) : solve_current(s, true, err);
    if (rc != PC_OK) {
        snapshot_free(&before);
        return rc;
    }
    return record(s, before, err);
}

pc_status pc_ui_puppet_remove_all(pc_ui_puppet_session *s, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (s->warp.npins == 0)
        return PC_OK;
    pc_ui_puppet_snapshot before;
    pc_status rc = snapshot_copy(&before, s->warp.pins, s->warp.npins, err);
    if (rc != PC_OK)
        return rc;
    free(s->warp.pins);
    s->warp.pins = NULL;
    s->warp.npins = 0;
    s->has_selected = false;
    s->has_drag = false;
    rc = rebind(s, err);
    if (rc != PC_OK) {
        snapshot_free(&before);
        return rc;
    }
    return record(s, before, err);
}

/* ---- undo / redo ---- */

static bool restore(pc_ui_puppet_session *s, const pc_ui_puppet_snapshot *snap)
{
    pc_algo_puppet_pin *copy = NULL;
    if (snap->n > 0) {
        copy = pc_malloc_array(snap->n, sizeof *copy);
        if (copy == NULL)
            return false;
        memcpy(copy, snap->pins, snap->n * sizeof *copy);
    }
    free(s->warp.pins);
    s->warp.pins = copy;
    s->warp.npins = snap->n;
    if (s->has_selected && s->selected >= s->warp.npins)
        s->has_selected = false;
    s->has_drag = false;
    snapshot_free(&s->drag_from);
    s->has_drag_from = false;
    (void)rebind(s, NULL);
    return true;
}

static bool pop_restore(pc_ui_puppet_session *s, pc_ui_puppet_snapshot **from, size_t *nfrom,
                        pc_ui_puppet_snapshot **to, size_t *nto, size_t *cap_to)
{
    if (*nfrom == 0)
        return false;
    pc_ui_puppet_snapshot cur;
    if (snapshot_copy(&cur, s->warp.pins, s->warp.npins, NULL) != PC_OK)
        return false;
    pc_ui_puppet_snapshot snap = (*from)[--(*nfrom)];
    if (push_snapshot(to, nto, cap_to, cur, NULL) != PC_OK) {
        /* keep the popped step so nothing is lost */
        (*from)[(*nfrom)++] = snap;
        return false;
    }
    bool ok = restore(s, &snap);
    snapshot_free(&snap);
    return ok;
}

bool pc_ui_puppet_undo(pc_ui_puppet_session *s)
{
    if (s == NULL)
        return false;
    return pop_restore(s, &s->undo, &s->nundo, &s->redo, &s->nredo, &s->cap_redo);
}

bool pc_ui_puppet_redo(pc_ui_puppet_session *s)
{
    if (s == NULL)
        return false;
    return pop_restore(s, &s->redo, &s->nredo, &s->undo, &s->nundo, &s->cap_undo);
}

/* ---- describe / commit ---- */

bool pc_ui_puppet_is_noop(const pc_ui_puppet_session *s)
{
    return s == NULL || pc_algo_puppet_warp_is_identity(&s->warp);
}

pc_status pc_ui_puppet_describe(const pc_ui_puppet_session *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    yyjson_mut_val *obj = pc_algo_puppet_warp_to_json(doc, &s->warp);
    if (obj == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    size_t nverts = 0, ntris = 0;
    if (s->solver != NULL) {
        const pc_algo_puppet_mesh *m = pc_algo_puppet_solver_mesh(s->solver);
        nverts = m->nverts;
        ntris = m->ntris;
    }
    yyjson_mut_obj_add_uint(doc, obj, "vertices", nverts);
    yyjson_mut_obj_add_uint(doc, obj, "triangles", ntris);
    if (s->has_selected)
        yyjson_mut_obj_add_uint(doc, obj, "selected", s->selected);
    else
        yyjson_mut_obj_add_null(doc, obj, "selected");
    *out = obj;
    return PC_OK;
}

pc_status pc_ui_puppet_commit_params(const pc_ui_puppet_session *s, yyjson_mut_doc *doc,
                                     yyjson_mut_val **out, bool *run, pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL || run == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    *out = NULL;
    *run = false;
    if (pc_ui_puppet_is_noop(s))
        return PC_OK;
    yyjson_mut_val *obj = pc_algo_puppet_warp_to_json(doc, &s->warp);
    if (obj == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    *out = obj;
    *run = true;
    return PC_OK;
}
