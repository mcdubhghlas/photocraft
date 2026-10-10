/* Edit > Puppet Warp: pins on a triangle mesh over the layer's opaque region, deformed
 * as-rigidly-as-possible (port of crates/algo/src/puppet.rs).
 *
 * Mesh: the layer's coverage (alpha above 0.05, grown or shrunk by `expansion`) is rasterised onto
 * a coarse block grid; a square grid at the density's spacing keeps every cell touching covered
 * blocks, split into two triangles (alternating diagonals).
 * Deformation: the per-triangle ARAP energy (Liu et al. 2008) with cotangent weights, minimised by
 * alternating a local step (best rotation per triangle; Distort fits similarities, Normal rotation
 * x sqrt(scale)) and a global step (a banded Cholesky factor of the SPD system, or Jacobi-
 * preconditioned conjugate gradients when the band is too wide or the factor fails). Pins are
 * stiff soft constraints on the barycentric point under them; a pin's rotation fixes R on the
 * triangles around it; mesh components without a pin stay where they are.
 * Rendering: textured triangles (pc_algo_warp_triangles), back to front by nearest-pin depth.
 *
 * Not a FilterParams filter: the engine's edit.puppetWarp commands call it.
 *
 * Failure: NOMEM; PC_ERR_LIMIT for a mesh grid the reference could not allocate (bounds near the
 * i32 range), more than PC_ALGO_PUPPET_MAX_ITERATIONS rounds, or (pc_algo_puppet_warp) a deformed
 * mesh spanning more than PC_ALGO_PUPPET_MAX_WARP_PIXELS (the reference's `None`). Singular or
 * degenerate systems never fail: the Cholesky factor falls back to CG, CG stops on a vanishing
 * curvature, degenerate triangles get zero weight and pins outside the mesh bind to the nearest
 * vertex, all as in the reference. Thread safety: a solver is immutable after creation; solves
 * on one solver may run concurrently. Results do not depend on the thread count. */
#ifndef PC_ALGO_PUPPET_H
#define PC_ALGO_PUPPET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

typedef enum pc_algo_puppet_mode {
    PC_ALGO_PUPPET_RIGID,
    PC_ALGO_PUPPET_NORMAL, /* default */
    PC_ALGO_PUPPET_DISTORT
} pc_algo_puppet_mode;

typedef enum pc_algo_puppet_density {
    PC_ALGO_PUPPET_FEWER,
    PC_ALGO_PUPPET_DENSITY_NORMAL, /* default */
    PC_ALGO_PUPPET_MORE
} pc_algo_puppet_density;

extern const pc_algo_enum_desc pc_algo_puppet_mode_desc, pc_algo_puppet_density_desc;

/* PuppetMode::parse / PuppetDensity::parse (ASCII case-insensitive; density also takes
 * "fewerPoints" / "morePoints"). False when unknown. */
bool pc_algo_puppet_mode_parse(const char *s, size_t len, pc_algo_puppet_mode *out);
bool pc_algo_puppet_density_parse(const char *s, size_t len, pc_algo_puppet_density *out);

/* Number of local/global rounds the command uses. */
#define PC_ALGO_PUPPET_ITERATIONS 40
/* Rounds accepted by pc_algo_puppet_solve (the reference has no bound; it would run for hours). */
#define PC_ALGO_PUPPET_MAX_ITERATIONS 100000
/* The largest area (px) a puppet warp's result may span: 2^30 (#1019). */
#define PC_ALGO_PUPPET_MAX_WARP_PIXELS 1073741824.0

/* An optional fixed rotation (Option<f64>, degrees). */
typedef struct pc_algo_puppet_rot {
    bool set;
    double deg;
} pc_algo_puppet_rot;

/* A pin: where it was placed (src) and where it is now (dst), document pixels. */
typedef struct pc_algo_puppet_pin {
    double src[2];
    double dst[2];
    pc_algo_puppet_rot rotate; /* fixed mesh rotation around the pin; unset = automatic */
    int32_t depth;             /* where the mesh overlaps, triangles nearest a higher pin draw on top */
} pc_algo_puppet_pin;

/* A complete puppet warp (the command's params and a smart filter's stored data). */
typedef struct pc_algo_puppet_warp {
    pc_algo_puppet_pin *pins; /* owned; NULL when npins == 0 */
    size_t npins;
    pc_algo_puppet_mode mode;
    pc_algo_puppet_density density;
    double expansion; /* px; default 2 */
} pc_algo_puppet_warp;

pc_algo_puppet_warp pc_algo_puppet_warp_make(void);    /* the serde defaults, no pins */
void pc_algo_puppet_warp_fini(pc_algo_puppet_warp *w); /* NULL-safe */
bool pc_algo_puppet_warp_is_identity(const pc_algo_puppet_warp *w);

/* serde_json::from_value of Vec<PuppetPin> / PuppetWarp (serde's error texts) and to_value
 * (rotate omitted when unset, depth when 0). *pins is owned by the caller (free()). */
pc_status pc_algo_puppet_pins_from_json(yyjson_val *v, pc_algo_puppet_pin **pins, size_t *n, pc_error *err);
pc_status pc_algo_puppet_warp_from_json(yyjson_val *v, pc_algo_puppet_warp *out, pc_error *err);
yyjson_mut_val *pc_algo_puppet_pin_to_json(yyjson_mut_doc *d, const pc_algo_puppet_pin *p);
yyjson_mut_val *pc_algo_puppet_warp_to_json(yyjson_mut_doc *d, const pc_algo_puppet_warp *w);

/* A triangle mesh in document pixels. */
typedef struct pc_algo_puppet_mesh {
    double (*verts)[2];
    size_t nverts;
    size_t (*tris)[3];
    size_t ntris;
    double spacing; /* grid spacing px (0 for the empty mesh) */
} pc_algo_puppet_mesh;

void pc_algo_puppet_mesh_fini(pc_algo_puppet_mesh *m); /* NULL-safe; leaves the empty mesh */
bool pc_algo_puppet_mesh_eq(const pc_algo_puppet_mesh *a, const pc_algo_puppet_mesh *b);

/* Builds the mesh over the covered part of src inside bounds (empty bounds: the empty mesh). */
pc_status pc_algo_puppet_build_mesh(const pc_surface *src, pc_rect bounds, pc_algo_puppet_density density,
                                    double expansion, pc_algo_puppet_mesh *out, pc_error *err);

/* Prepared ARAP solver for one mesh and a set of pin source positions. */
typedef struct pc_algo_puppet_solver pc_algo_puppet_solver;

/* Takes ownership of *mesh (left empty, also on failure, which frees it). fixed_rotation[k] marks
 * pins whose rotation is fixed (missing entries: false). */
pc_status pc_algo_puppet_solver_new(pc_algo_puppet_mesh *mesh, const double (*pin_src)[2], size_t npins,
                                    const bool *fixed_rotation, size_t nfixed, pc_algo_puppet_solver **out,
                                    pc_error *err);
void pc_algo_puppet_solver_free(pc_algo_puppet_solver *s); /* NULL-safe */
/* Borrowed; valid while the solver lives. */
const pc_algo_puppet_mesh *pc_algo_puppet_solver_mesh(const pc_algo_puppet_solver *s);
/* Whether the system was factored (banded Cholesky) rather than left to conjugate gradients. */
bool pc_algo_puppet_solver_factored(const pc_algo_puppet_solver *s);
/* Number of pins bound to the mesh (0 for an empty mesh, else npins). */
size_t pc_algo_puppet_solver_bound_count(const pc_algo_puppet_solver *s);
/* Position of bound pin k on vertices v (its barycentric point); false when k is out of range. */
bool pc_algo_puppet_solver_pin_point(const pc_algo_puppet_solver *s, size_t k, const double (*v)[2],
                                     double out[2]);

/* Deforms the mesh: pin_dst (one per pin; extra or missing entries are ignored like the
 * reference's zips), optional fixed rotations, `iterations` local/global rounds, warm-started
 * from init when it holds exactly one position per vertex (else NULL / any other length).
 * *v_out (nverts positions) and *energies (iterations values, the rigid ARAP energy after each
 * round) are owned by the caller (free()); either may be NULL when empty. */
pc_status pc_algo_puppet_solve(const pc_algo_puppet_solver *s, const double (*pin_dst)[2], size_t ndst,
                               const pc_algo_puppet_rot *rotations, size_t nrot, pc_algo_puppet_mode mode,
                               size_t iterations, const double (*init)[2], size_t ninit, double (**v_out)[2],
                               double **energies, pc_error *err);
/* Energy of v with the rigid best fit (for monitoring). */
pc_status pc_algo_puppet_energy(const pc_algo_puppet_solver *s, const double (*v)[2],
                                const double (*pin_dst)[2], size_t ndst, const pc_algo_puppet_rot *rotations,
                                size_t nrot, double *out, pc_error *err);
/* Triangle draw order (back to front) for pins with depths: *order holds ntris indices (free()). */
pc_status pc_algo_puppet_draw_order(const pc_algo_puppet_solver *s, const int32_t *depths, size_t ndepths,
                                    const double (*pin_src)[2], size_t npins, size_t **order, pc_error *err);

/* The deformed mesh of a puppet warp: solver, deformed vertices and draw order (all owned by the
 * caller: pc_algo_puppet_solver_free, free, free). */
pc_status pc_algo_puppet_deform(const pc_surface *src, pc_rect bounds, const pc_algo_puppet_warp *w,
                                size_t iterations, pc_algo_puppet_solver **solver, double (**v)[2],
                                size_t **order, pc_error *err);
/* Rasterises the mesh deformation of src (inside bounds): the deformed triangles only, so pixels
 * outside the mesh are dropped. Output has src's format with alpha; `out` initialised on success. */
pc_status pc_algo_puppet_render(const pc_surface *src, pc_rect bounds, const pc_algo_puppet_mesh *mesh,
                                const double (*deformed)[2], const size_t *order, size_t norder,
                                pc_algo_interp interp, pc_surface *out, pc_error *err);
/* Applies a puppet warp to src (content inside bounds). PC_ERR_LIMIT when the deformed mesh
 * would span more than PC_ALGO_PUPPET_MAX_WARP_PIXELS; a mesh without triangles returns a copy. */
pc_status pc_algo_puppet_apply(const pc_surface *src, pc_rect bounds, const pc_algo_puppet_warp *w,
                               pc_algo_interp interp, pc_surface *out, pc_error *err);

#endif
