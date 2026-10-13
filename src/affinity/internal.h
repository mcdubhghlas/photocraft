/* Internals of src/affinity shared between its files. Not a public API. */
#ifndef PC_AFFINITY_INTERNAL_H
#define PC_AFFINITY_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "affinity/affinity.h"
#include "base/status.h"

/* ---- errors (the reference's Error Display) ---- */

#define ax_malformed(err, s) pc_fail((err), PC_ERR_FORMAT, "damaged Affinity file: %s", (s))
#define ax_unsupported(err, s) pc_fail((err), PC_ERR_UNSUPPORTED, "unsupported Affinity file: %s", (s))
#define ax_limit(err, s) pc_fail((err), PC_ERR_LIMIT, "Affinity file exceeds a safety limit: %s", (s))
#define ax_nomem(err) pc_fail((err), PC_ERR_NOMEM, "out of memory")

/* ---- byte cursor (container.rs Cursor) ---- */

typedef struct ax_cursor {
    const uint8_t *b;
    size_t n;
    size_t pos;
} ax_cursor;

size_t ax_remaining(const ax_cursor *c);
pc_status ax_take(ax_cursor *c, size_t n, const uint8_t **out, pc_error *err);
pc_status ax_u8(ax_cursor *c, uint8_t *v, pc_error *err);
pc_status ax_u16(ax_cursor *c, uint16_t *v, pc_error *err);
pc_status ax_u32(ax_cursor *c, uint32_t *v, pc_error *err);
pc_status ax_u64(ax_cursor *c, uint64_t *v, pc_error *err);

/* Lossy UTF-8 copy (NUL-terminated, malloc'd); NULL on out of memory. */
char *ax_lossy(const uint8_t *b, size_t n, size_t *len);

/* ---- typed object access (access.rs); every accessor treats odd data as absent ---- */

const pc_af_object *ax_object(const pc_af_stream *s, size_t id);
const pc_af_value *ax_field(const pc_af_stream *s, size_t id, const char *tag);
bool ax_class(const pc_af_stream *s, size_t id, pc_af_tag *out);
bool ax_is(const pc_af_stream *s, size_t id, const char *cls);
bool ax_obj(const pc_af_stream *s, size_t id, const char *tag, size_t *out);
/* An array field of objects (NULL when absent or not an array). Iterate its non-null objects
 * with ax_next_obj(arr, &i, &id) starting at i = 0; ax_objs_count counts them. */
const pc_af_value *ax_objs(const pc_af_stream *s, size_t id, const char *tag);
bool ax_next_obj(const pc_af_value *arr, size_t *i, size_t *id);
size_t ax_objs_count(const pc_af_value *arr);
bool ax_f64(const pc_af_stream *s, size_t id, const char *tag, double *out);
bool ax_int(const pc_af_stream *s, size_t id, const char *tag, int64_t *out);
bool ax_bool(const pc_af_stream *s, size_t id, const char *tag, bool *out);
const char *ax_str(const pc_af_stream *s, size_t id, const char *tag);
bool ax_enum(const pc_af_stream *s, size_t id, const char *tag, uint16_t *eid, uint16_t *ver);
/* A fixed vector of n finite floats. */
bool ax_floats(const pc_af_stream *s, size_t id, const char *tag, size_t n, double *out);
const uint8_t *ax_bytes(const pc_af_stream *s, size_t id, const char *tag, size_t *n);
const char *ax_entry(const pc_af_stream *s, size_t id, const char *tag);

/* ---- model reader (model.rs) ---- */

typedef struct ax_warning {
    char *text;
    size_t count;
} ax_warning;

typedef struct ax_reader {
    const pc_af_stream *s;
    pc_af_archive *archive;
    double dpi;
    ax_warning *warnings; /* sorted by text (BTreeMap order) */
    size_t nwarnings;
    size_t nodes;
    uint8_t *active; /* per object: on the current path (cycle guard) */
    bool nomem; /* a warning could not be recorded */
    int color_depth; /* recursion guard for spot colours */
} ax_reader;

void ax_warn(ax_reader *r, const char *what);
void ax_warnf(ax_reader *r, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

pc_af_affine ax_affine_from_xfrm(const double m[6]);
pc_af_affine ax_affine_then(pc_af_affine self, pc_af_affine outer);
pc_af_point ax_affine_apply(const pc_af_affine *m, pc_af_point p);
double ax_affine_scale(const pc_af_affine *m);
bool ax_affine_is_finite(const pc_af_affine *m);
extern const pc_af_affine AX_IDENTITY;
/* A node's Xfrm, or the identity. */
pc_af_affine ax_xfrm(const pc_af_stream *s, size_t id);

pc_af_rect ax_rect(const double v[4]);
pc_af_rect ax_union(pc_af_rect a, pc_af_rect b);
const char *ax_describe(pc_af_tag cls);

/* Paths (owned); *dst gets src's subpaths transformed (appended). */
void ax_path_fini(pc_af_path *p);
pc_status ax_path_append_transformed(pc_af_path *dst, const pc_af_path *src, pc_af_affine m, pc_error *err);
pc_status ax_path_push(pc_af_path *p, pc_af_subpath sp, pc_error *err);
void ax_subpath_fini(pc_af_subpath *sp);
void ax_paint_fini(pc_af_paint *p);
void ax_stroke_fini(pc_af_stroke *s);
void ax_image_fini(pc_af_image *i);
void ax_text_fini(pc_af_text *t);
void ax_node_fini(pc_af_node *n);

/* geometry.rs: the node-space outline; false when unreadable (warnings may be recorded). A
 * status other than PC_OK only for out of memory. */
pc_status ax_outline(ax_reader *r, size_t id, pc_af_tag cls, pc_af_affine world, bool *ok, pc_af_path *out,
                     bool *even_odd, pc_error *err);

/* shapes.rs */
/* *even_odd is set for shapes with holes (cogs). */
pc_status ax_shape(ax_reader *r, size_t shape, const double b[4], pc_af_affine world, bool *ok,
                   pc_af_path *out, bool *even_odd, pc_error *err);
pc_status ax_rectangle(double x0, double y0, double x1, double y1, pc_af_subpath *out, pc_error *err);
typedef struct ax_corner_pt {
    pc_af_point p;
    double radius;
} ax_corner_pt;
pc_status ax_fillet(const ax_corner_pt *pts, size_t n, bool closed, pc_af_subpath *out, pc_error *err);

/* paint.rs: descriptor returns ok=false for "None" (unreadable). */
pc_status ax_descriptor(ax_reader *r, size_t id, pc_af_affine local, bool *ok, pc_af_paint *out, pc_error *err);
/* Fills of `list`: 0 or 1 paints (none-paints dropped). */
pc_status ax_fills(ax_reader *r, size_t id, pc_af_affine local, const char *list, pc_af_paint **out, size_t *n,
                   pc_error *err);
pc_status ax_node_paint(ax_reader *r, size_t id, pc_af_affine local, pc_af_paint **fills, size_t *nfills,
                        pc_af_stroke **strokes, size_t *nstrokes, pc_error *err);

/* text.rs: ok=false when the story can't be read. */
pc_status ax_text_read(ax_reader *r, size_t id, pc_af_affine world, bool *ok, pc_af_text *out, pc_error *err);

/* raster.rs: *has is false for "no image". */
pc_status ax_node_image(ax_reader *r, size_t id, pc_af_affine world, bool *has, pc_af_image *out, pc_error *err);
pc_status ax_mask(ax_reader *r, size_t id, pc_af_affine world, bool *has, pc_af_image *out, pc_error *err);
pc_status ax_embedded(ax_reader *r, size_t id, pc_af_affine world, bool *has, pc_af_image *out, pc_error *err);

#endif
