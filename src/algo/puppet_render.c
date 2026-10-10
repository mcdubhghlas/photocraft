/* Edit > Puppet Warp rendering (port of render / puppet_warp in crates/algo/src/puppet.rs): the
 * deformed triangles drawn back to front with textured-triangle rasterisation. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/puppet.h"
#include "algo/warp.h"
#include "base/mem.h"
#include "base/rmath.h"

pc_status pc_algo_puppet_render(const pc_surface *src, pc_rect bounds, const pc_algo_puppet_mesh *mesh,
                                const double (*deformed)[2], const size_t *order, size_t norder,
                                pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_algo_warp_vertex *verts;
    size_t (*tris)[3], i;
    pc_status st;
    verts = pc_malloc_array(mesh->nverts > 0 ? mesh->nverts : 1, sizeof *verts);
    tris = pc_malloc_array(norder > 0 ? norder : 1, sizeof *tris);
    if (verts == NULL || tris == NULL) {
        free(verts);
        free(tris);
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    for (i = 0; i < mesh->nverts; i++) {
        memcpy(verts[i].dst, deformed[i], sizeof verts[i].dst);
        memcpy(verts[i].src, mesh->verts[i], sizeof verts[i].src);
    }
    for (i = 0; i < norder; i++) {
        if (order[i] >= mesh->ntris) {
            free(verts);
            free(tris);
            return pc_fail(err, PC_ERR_INVALID, "puppet warp: triangle %zu out of range", order[i]);
        }
        memcpy(tris[i], mesh->tris[order[i]], sizeof tris[i]);
    }
    st = pc_algo_warp_triangles(src, bounds, verts, mesh->nverts, (const size_t (*)[3])tris, norder, interp,
                                out, err);
    free(verts);
    free(tris);
    return st;
}

pc_status pc_algo_puppet_apply(const pc_surface *src, pc_rect bounds, const pc_algo_puppet_warp *w,
                               pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_algo_puppet_solver *s = NULL;
    double (*v)[2] = NULL, x0 = 1.7976931348623157e308, y0 = 1.7976931348623157e308;
    double x1 = -1.7976931348623157e308, y1 = -1.7976931348623157e308, area;
    size_t *order = NULL, i;
    const pc_algo_puppet_mesh *m;
    pc_status st = pc_algo_puppet_deform(src, bounds, w, PC_ALGO_PUPPET_ITERATIONS, &s, &v, &order, err);
    if (st != PC_OK) {
        return st;
    }
    m = pc_algo_puppet_solver_mesh(s);
    /* No triangles (an expansion that shrinks every pixel away): nothing to warp. Rendering the
     * empty mesh would drop every pixel of the layer. */
    if (m->ntris == 0) {
        st = pc_surface_clone(src, out, err);
        goto done;
    }
    for (i = 0; i < m->nverts; i++) {
        x0 = pc_rs_min(x0, v[i][0]);
        y0 = pc_rs_min(y0, v[i][1]);
        x1 = pc_rs_max(x1, v[i][0]);
        y1 = pc_rs_max(y1, v[i][1]);
    }
    area = (x1 - x0 + 1.0) * (y1 - y0 + 1.0);
    if (isnan(area) || area > PC_ALGO_PUPPET_MAX_WARP_PIXELS) {
        st = pc_fail(err, PC_ERR_LIMIT,
                     "puppet warp: a pin is dragged too far (the result would span %.0f px)", area);
        goto done;
    }
    st = pc_algo_puppet_render(src, bounds, m, (const double (*)[2])v, order, m->ntris, interp, out, err);
done:
    pc_algo_puppet_solver_free(s);
    free(v);
    free(order);
    return st;
}
