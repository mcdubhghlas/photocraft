/* Edit > Perspective Warp UI model. See persp_warp.h. Clean-room port of perspective_ui.rs. */
#include "ui/persp_warp.h"

#include <math.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"

static bool finite4(const double v[4])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) && isfinite(v[3]);
}

pc_status pc_ui_persp_begin(pc_ui_persp_session *s, uint64_t key, uint64_t layer, const char *command,
                            const double bounds[4], pc_error *err)
{
    if (s == NULL || bounds == NULL || !finite4(bounds))
        return pc_fail(err, PC_ERR_INVALID, "perspective warp needs finite bounds");
    pc_ui_persp_fini(s);
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
    s->mode = PC_UI_PERSP_LAYOUT;
    s->drag = PC_UI_PERSP_DRAG_NONE;
    return pc_ui_persp_update_grid(s, err);
}

void pc_ui_persp_fini(pc_ui_persp_session *s)
{
    if (s == NULL)
        return;
    free(s->planes);
    free(s->group);
    s->planes = NULL;
    s->group = NULL;
    s->nplanes = s->cap_planes = 0;
    s->ngroup = s->cap_group = 0;
    s->active = false;
}

static pc_status push_plane(pc_ui_persp_session *s, const pc_algo_persp_plane *p, pc_error *err)
{
    if (s->nplanes >= PC_UI_PERSP_MAX_PLANES)
        return pc_fail(err, PC_ERR_LIMIT, "too many perspective planes");
    void *grown = pc_grow_array(s->planes, &s->cap_planes, s->nplanes + 1, sizeof *s->planes);
    if (grown == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    s->planes = grown;
    s->planes[s->nplanes++] = *p;
    return PC_OK;
}

pc_status pc_ui_persp_update_grid(pc_ui_persp_session *s, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    double x0 = s->bounds[0], y0 = s->bounds[1];
    double w = s->bounds[2] - s->bounds[0], h = s->bounds[3] - s->bounds[1];
    pc_algo_persp_map *map = NULL;
    if (s->mode == PC_UI_PERSP_WARP && s->nplanes > 0)
        (void)pc_algo_persp_map_new(s->planes, s->nplanes, &map, NULL); /* NULL => identity below */
    for (size_t j = 0; j < PC_UI_PERSP_GRID_N; j++) {
        for (size_t i = 0; i < PC_UI_PERSP_GRID_N; i++) {
            double gx = x0 + (double)i / (double)PC_UI_PERSP_GRID * w;
            double gy = y0 + (double)j / (double)PC_UI_PERSP_GRID * h;
            double ox = gx, oy = gy;
            if (map != NULL)
                pc_algo_persp_map_apply(map, gx, gy, &ox, &oy);
            size_t k = j * PC_UI_PERSP_GRID_N + i;
            s->grid[k][0] = ox;
            s->grid[k][1] = oy;
        }
    }
    pc_algo_persp_map_free(map);
    return PC_OK;
}

pc_status pc_ui_persp_set_mode(pc_ui_persp_session *s, pc_ui_persp_mode mode, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    s->mode = mode;
    if (mode == PC_UI_PERSP_WARP && s->nplanes > 0)
        (void)pc_algo_persp_unify(s->planes, s->nplanes, NULL);
    return pc_ui_persp_update_grid(s, err);
}

/* Nearest corner (current mode) within tol; false if none. */
static bool corner_at(pc_ui_persp_session *s, double x, double y, double tol, size_t *pp, size_t *pc)
{
    bool found = false;
    double best = tol;
    for (size_t p = 0; p < s->nplanes; p++) {
        double (*c)[2] = s->mode == PC_UI_PERSP_WARP ? s->planes[p].dst : s->planes[p].src;
        for (size_t k = 0; k < 4; k++) {
            double d = hypot(c[k][0] - x, c[k][1] - y);
            if (d <= best) {
                best = d;
                *pp = p;
                *pc = k;
                found = true;
            }
        }
    }
    return found;
}

static bool in_group(const pc_ui_persp_session *s, size_t p, size_t c)
{
    for (size_t i = 0; i < s->ngroup; i++)
        if (s->group[i].plane == p && s->group[i].corner == c)
            return true;
    return false;
}

/* First SRC corner within tol not in the drag group; returns it in out, else leaves (x,y). */
static void snap_point(const pc_ui_persp_session *s, double x, double y, double tol, bool use_group,
                       double out[2])
{
    for (size_t p = 0; p < s->nplanes; p++) {
        for (size_t k = 0; k < 4; k++) {
            if (use_group && in_group(s, p, k))
                continue;
            double sx = s->planes[p].src[k][0], sy = s->planes[p].src[k][1];
            if (hypot(sx - x, sy - y) <= tol) {
                out[0] = sx;
                out[1] = sy;
                return;
            }
        }
    }
    out[0] = x;
    out[1] = y;
}

static void set_corner(pc_ui_persp_session *s, const double p[2])
{
    for (size_t i = 0; i < s->ngroup; i++) {
        size_t pl = s->group[i].plane, c = s->group[i].corner;
        if (pl >= s->nplanes || c >= 4)
            continue;
        if (s->mode == PC_UI_PERSP_LAYOUT) {
            s->planes[pl].src[c][0] = p[0];
            s->planes[pl].src[c][1] = p[1];
        }
        s->planes[pl].dst[c][0] = p[0];
        s->planes[pl].dst[c][1] = p[1];
    }
}

/* Collect the linked group of (pi,ci) into s->group. */
static pc_status build_group(pc_ui_persp_session *s, size_t pi, size_t ci, pc_error *err)
{
    s->ngroup = 0;
    size_t *group_of = pc_malloc_array(s->nplanes * 4, sizeof *group_of);
    if (group_of == NULL && s->nplanes > 0)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    size_t n_groups = 0;
    pc_algo_persp_linked_corners(s->planes, s->nplanes, group_of, &n_groups);
    size_t g = group_of[pi * 4 + ci];
    for (size_t p = 0; p < s->nplanes; p++) {
        for (size_t k = 0; k < 4; k++) {
            if (group_of[p * 4 + k] != g)
                continue;
            void *grown = pc_grow_array(s->group, &s->cap_group, s->ngroup + 1, sizeof *s->group);
            if (grown == NULL) {
                free(group_of);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            s->group = grown;
            s->group[s->ngroup].plane = p;
            s->group[s->ngroup].corner = k;
            s->ngroup++;
        }
    }
    free(group_of);
    return PC_OK;
}

pc_status pc_ui_persp_pointer(pc_ui_persp_session *s, pc_ui_pointer_kind kind, double x, double y,
                              double tol, pc_error *err)
{
    if (s == NULL || !s->active)
        return pc_fail(err, PC_ERR_INVALID, "no perspective session");
    if (!isfinite(x) || !isfinite(y) || !(tol >= 0.0))
        return pc_fail(err, PC_ERR_INVALID, "bad perspective pointer");

    if (kind == PC_UI_POINTER_CANCEL) {
        s->drag = PC_UI_PERSP_DRAG_NONE;
        s->ngroup = 0;
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_DOWN) {
        size_t pi = 0, ci = 0;
        if (corner_at(s, x, y, tol, &pi, &ci)) {
            pc_status rc = build_group(s, pi, ci, err);
            if (rc != PC_OK)
                return rc;
            double (*c)[2] = s->mode == PC_UI_PERSP_WARP ? s->planes[pi].dst : s->planes[pi].src;
            s->corner_offset[0] = c[ci][0] - x;
            s->corner_offset[1] = c[ci][1] - y;
            s->drag = PC_UI_PERSP_DRAG_CORNER;
        } else if (s->mode == PC_UI_PERSP_LAYOUT) {
            double start[2];
            snap_point(s, x, y, tol, false, start);
            s->new_start[0] = start[0];
            s->new_start[1] = start[1];
            s->new_cur[0] = x;
            s->new_cur[1] = y;
            s->drag = PC_UI_PERSP_DRAG_NEWPLANE;
        }
        return PC_OK;
    }
    if (kind == PC_UI_POINTER_MOVE) {
        if (s->drag == PC_UI_PERSP_DRAG_NEWPLANE) {
            s->new_cur[0] = x;
            s->new_cur[1] = y;
        } else if (s->drag == PC_UI_PERSP_DRAG_CORNER) {
            double q[2] = {x + s->corner_offset[0], y + s->corner_offset[1]};
            if (s->mode == PC_UI_PERSP_LAYOUT) {
                double sn[2];
                snap_point(s, q[0], q[1], tol, true, sn);
                q[0] = sn[0];
                q[1] = sn[1];
            }
            set_corner(s, q);
            if (s->mode == PC_UI_PERSP_WARP)
                return pc_ui_persp_update_grid(s, err);
        }
        return PC_OK;
    }
    /* PC_UI_POINTER_UP */
    if (s->drag == PC_UI_PERSP_DRAG_NEWPLANE) {
        double end[2];
        snap_point(s, x, y, tol, false, end);
        double x0 = fmin(s->new_start[0], end[0]), x1 = fmax(s->new_start[0], end[0]);
        double y0 = fmin(s->new_start[1], end[1]), y1 = fmax(s->new_start[1], end[1]);
        if (x1 - x0 > 4.0 && y1 - y0 > 4.0) {
            double q[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
            for (size_t k = 0; k < 4; k++) {
                double sn[2];
                snap_point(s, q[k][0], q[k][1], tol, false, sn);
                q[k][0] = sn[0];
                q[k][1] = sn[1];
            }
            pc_algo_persp_plane plane = pc_algo_persp_plane_identity((const double(*)[2])q);
            pc_status rc = push_plane(s, &plane, err);
            if (rc != PC_OK) {
                s->drag = PC_UI_PERSP_DRAG_NONE;
                return rc;
            }
        }
    } else if (s->drag == PC_UI_PERSP_DRAG_CORNER) {
        double q[2] = {x + s->corner_offset[0], y + s->corner_offset[1]};
        if (s->mode == PC_UI_PERSP_LAYOUT) {
            double sn[2];
            snap_point(s, q[0], q[1], tol, true, sn);
            q[0] = sn[0];
            q[1] = sn[1];
        }
        set_corner(s, q);
    }
    s->drag = PC_UI_PERSP_DRAG_NONE;
    s->ngroup = 0;
    return pc_ui_persp_update_grid(s, err);
}

void pc_ui_persp_clear(pc_ui_persp_session *s)
{
    if (s == NULL)
        return;
    s->nplanes = 0;
    s->drag = PC_UI_PERSP_DRAG_NONE;
    s->ngroup = 0;
    (void)pc_ui_persp_update_grid(s, NULL);
}

pc_status pc_ui_persp_straighten(pc_ui_persp_session *s, pc_algo_persp_straighten mode, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    pc_status rc = pc_algo_persp_straighten_planes(s->planes, s->nplanes, mode, err);
    if (rc != PC_OK)
        return rc;
    return pc_ui_persp_update_grid(s, err);
}

static bool parse_quad(yyjson_val *arr, double q[4][2])
{
    if (arr == NULL || !yyjson_is_arr(arr) || yyjson_arr_size(arr) != 4)
        return false;
    for (size_t k = 0; k < 4; k++) {
        yyjson_val *pair = yyjson_arr_get(arr, k);
        if (pair == NULL || !yyjson_is_arr(pair) || yyjson_arr_size(pair) != 2)
            return false;
        double vx, vy;
        if (!pc_json_as_f64(yyjson_arr_get(pair, 0), &vx) || !pc_json_as_f64(yyjson_arr_get(pair, 1), &vy))
            return false;
        if (!isfinite(vx) || !isfinite(vy))
            return false;
        q[k][0] = vx;
        q[k][1] = vy;
    }
    return true;
}

pc_status pc_ui_persp_set_planes_json(pc_ui_persp_session *s, yyjson_val *arr, pc_error *err)
{
    if (s == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null session");
    if (arr == NULL || !yyjson_is_arr(arr))
        return pc_fail(err, PC_ERR_INVALID, "planes must be an array");
    size_t n = yyjson_arr_size(arr);
    if (n > PC_UI_PERSP_MAX_PLANES)
        return pc_fail(err, PC_ERR_LIMIT, "too many perspective planes");
    pc_algo_persp_plane *planes = n > 0 ? pc_calloc_array(n, sizeof *planes) : NULL;
    if (n > 0 && planes == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    for (size_t i = 0; i < n; i++) {
        yyjson_val *obj = yyjson_arr_get(arr, i);
        yyjson_val *src = pc_json_get(obj, "src");
        yyjson_val *dst = pc_json_get(obj, "dst");
        if (!parse_quad(src, planes[i].src) || !parse_quad(dst, planes[i].dst)) {
            free(planes);
            return pc_fail(err, PC_ERR_INVALID, "each plane needs src and dst quads of 4 points");
        }
    }
    free(s->planes);
    s->planes = planes;
    s->nplanes = n;
    s->cap_planes = n;
    s->drag = PC_UI_PERSP_DRAG_NONE; /* #952: a held drag indexed the old plane list */
    s->ngroup = 0;
    return pc_ui_persp_update_grid(s, err);
}

bool pc_ui_persp_is_noop(const pc_ui_persp_session *s)
{
    if (s == NULL || s->nplanes == 0)
        return true;
    return pc_algo_persp_is_identity(s->planes, s->nplanes);
}

static yyjson_mut_val *planes_json(const pc_ui_persp_session *s, yyjson_mut_doc *doc)
{
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    if (arr == NULL)
        return NULL;
    for (size_t i = 0; i < s->nplanes; i++) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_val *src = yyjson_mut_arr(doc);
        yyjson_mut_val *dst = yyjson_mut_arr(doc);
        if (obj == NULL || src == NULL || dst == NULL)
            return NULL;
        for (size_t k = 0; k < 4; k++) {
            yyjson_mut_val *sp = yyjson_mut_arr(doc), *dp = yyjson_mut_arr(doc);
            if (sp == NULL || dp == NULL)
                return NULL;
            yyjson_mut_arr_add_real(doc, sp, s->planes[i].src[k][0]);
            yyjson_mut_arr_add_real(doc, sp, s->planes[i].src[k][1]);
            yyjson_mut_arr_add_real(doc, dp, s->planes[i].dst[k][0]);
            yyjson_mut_arr_add_real(doc, dp, s->planes[i].dst[k][1]);
            yyjson_mut_arr_add_val(src, sp);
            yyjson_mut_arr_add_val(dst, dp);
        }
        yyjson_mut_obj_add_val(doc, obj, "src", src);
        yyjson_mut_obj_add_val(doc, obj, "dst", dst);
        yyjson_mut_arr_add_val(arr, obj);
    }
    return arr;
}

pc_status pc_ui_persp_describe(const pc_ui_persp_session *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                               pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_val *planes = planes_json(s, doc);
    if (obj == NULL || planes == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    yyjson_mut_obj_add_str(doc, obj, "mode", s->mode == PC_UI_PERSP_WARP ? "warp" : "layout");
    yyjson_mut_obj_add_val(doc, obj, "planes", planes);
    *out = obj;
    return PC_OK;
}

pc_status pc_ui_persp_commit_params(const pc_ui_persp_session *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                    bool *run, pc_error *err)
{
    if (s == NULL || doc == NULL || out == NULL || run == NULL)
        return pc_fail(err, PC_ERR_INVALID, "null arg");
    *out = NULL;
    *run = false;
    if (pc_ui_persp_is_noop(s))
        return PC_OK;
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_val *planes = planes_json(s, doc);
    if (obj == NULL || planes == NULL)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_obj_add_val(doc, obj, "planes", planes);
    yyjson_mut_obj_add_uint(doc, obj, "layer", s->layer);
    *out = obj;
    *run = true;
    return PC_OK;
}
