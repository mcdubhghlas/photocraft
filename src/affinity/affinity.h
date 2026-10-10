/* Reader for native Affinity documents (port of crates/affinity): the archive container, the
 * tagged object stream in doc.dat, and the subset of the document model PhotoCraft imports.
 * `.af` and `.afdesign` / `.afphoto` / `.afpub` from Affinity 1 to 3 (container versions 8-12).
 * Nothing here writes Affinity files.
 *
 * Errors (the reference's Error, same Display text):
 *   PC_ERR_FORMAT       "damaged Affinity file: <reason>"
 *   PC_ERR_UNSUPPORTED  "unsupported Affinity file: <reason>"
 *   PC_ERR_LIMIT        "Affinity file exceeds a safety limit: <reason>"
 *   PC_ERR_NOMEM        out of memory (the reference aborts)
 * Out-params are written only on success. Reentrant; no I/O. */
#ifndef PC_AFFINITY_AFFINITY_H
#define PC_AFFINITY_AFFINITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* "\x00\xffKA": documents and libraries share it. */
extern const uint8_t PC_AF_MAGIC[4];

bool pc_af_is_affinity(const uint8_t *b, size_t n);

/* Decompression budget for one import, checked before allocating. */
typedef struct pc_af_limits {
    size_t max_entry; /* largest single entry (doc.dat or one tile); default 256 MiB */
    size_t max_total; /* total bytes extracted over the import; default 1 GiB */
} pc_af_limits;

pc_af_limits pc_af_limits_default(void);

/* ---- container ---- */

typedef struct pc_af_header {
    uint16_t version;
    uint16_t flags;
    uint64_t fat_offset;
    uint64_t thumbnail_offset;
    uint32_t saved; /* Unix seconds */
} pc_af_header;

/* Checks the signature, version and document kind. */
pc_status pc_af_header_read(const uint8_t *b, size_t n, pc_af_header *out, pc_error *err);

typedef struct pc_af_entry {
    uint32_t id;
    char *name;
    uint64_t offset;
    uint64_t size; /* declared decompressed size */
    uint64_t compressed;
    uint32_t crc;
    uint8_t compression;
} pc_af_entry;

/* An open archive borrowing the file bytes. Free with pc_af_archive_fini. */
typedef struct pc_af_archive {
    const uint8_t *bytes;
    size_t len;
    pc_af_header header;
    pc_af_entry *entries; /* head revision of every named, non-deleted entry, sorted by name */
    size_t nentries;
    size_t revisions; /* tables (saves) in the chain */
    size_t extracted; /* bytes extracted so far, against max_total */
    pc_af_limits limits;
} pc_af_archive;

pc_status pc_af_archive_open(const uint8_t *b, size_t n, pc_af_limits limits, pc_af_archive *out, pc_error *err);
void pc_af_archive_fini(pc_af_archive *a);
const pc_af_entry *pc_af_archive_entry(const pc_af_archive *a, const char *name);
/* Extracts an entry: decompress, undo the byte predictor, verify the CRC. *out is malloc'd. */
pc_status pc_af_archive_read(pc_af_archive *a, const char *name, uint8_t **out, size_t *n, pc_error *err);

/* ---- object stream ---- */

/* A four-character tag in readable order (big-endian of "Desc"). */
typedef uint32_t pc_af_tag;
#define PC_AF_TAG(s) ((pc_af_tag)((uint32_t)(uint8_t)(s)[0] << 24 | (uint32_t)(uint8_t)(s)[1] << 16 | \
                                  (uint32_t)(uint8_t)(s)[2] << 8 | (uint32_t)(uint8_t)(s)[3]))

typedef enum pc_af_vkind {
    PC_AF_INT,
    PC_AF_UINT,
    PC_AF_FLOAT,
    PC_AF_BOOL,
    PC_AF_ENUM,
    PC_AF_STR,
    PC_AF_INTS,
    PC_AF_FLOATS,
    PC_AF_BYTES,
    PC_AF_BLOB,
    PC_AF_ENTRY,
    PC_AF_FLAGS,
    PC_AF_OBJ,
    PC_AF_ARRAY
} pc_af_vkind;

typedef struct pc_af_value pc_af_value;

struct pc_af_value {
    pc_af_vkind kind;
    union {
        int64_t i;
        uint64_t u; /* UINT, FLAGS */
        double f;
        bool b;
        struct {
            uint16_t id;
            uint16_t version;
        } e;
        struct {
            char *s; /* NUL-terminated lossy UTF-8 (STR, ENTRY) */
            size_t n;
        } str;
        struct {
            int64_t *v;
            size_t n;
        } ints;
        struct {
            double *v;
            size_t n;
        } floats;
        struct {
            uint8_t *v; /* BYTES, BLOB */
            size_t n;
        } bytes;
        struct {
            bool some;
            size_t id;
        } obj;
        struct {
            pc_af_value *v;
            size_t n;
        } arr;
    } as;
};

typedef enum pc_af_okind { PC_AF_ROOT, PC_AF_SHARED, PC_AF_INLINE, PC_AF_POSITIONAL } pc_af_okind;

typedef struct pc_af_field {
    pc_af_tag tag; /* 0 for positional fields */
    pc_af_value v;
} pc_af_field;

typedef struct pc_af_object {
    pc_af_tag cls;
    pc_af_okind kind;
    pc_af_tag *chain; /* class chain, most derived first */
    size_t nchain;
    pc_af_field *fields; /* stream order */
    size_t nfields;
} pc_af_object;

typedef struct pc_af_stream {
    size_t root;
    pc_af_object *objects;
    size_t nobjects;
} pc_af_stream;

#define PC_AF_MAX_DEPTH 384
#define PC_AF_MAX_VALUES ((size_t)1 << 24)

/* Parses a stream ("\x00\xffKS" header). Bytes after the root's terminator are ignored. */
pc_status pc_af_stream_parse(const uint8_t *b, size_t n, pc_af_stream *out, pc_error *err);
void pc_af_stream_fini(pc_af_stream *s);
/* The first field with this tag, or NULL. */
const pc_af_value *pc_af_object_get(const pc_af_object *o, pc_af_tag tag);

/* ---- document model ---- */

/* x' = a*x + c*y + e, y' = b*x + d*y + f; m = (a, b, c, d, e, f). */
typedef struct pc_af_affine {
    double m[6];
} pc_af_affine;

typedef struct pc_af_point {
    double x;
    double y;
} pc_af_point;

typedef struct pc_af_rect {
    double x0, y0, x1, y1;
} pc_af_rect;

typedef struct pc_af_subpath {
    pc_af_point start;
    pc_af_point (*segments)[3]; /* (control 1, control 2, end) per segment */
    size_t nsegments;
    bool closed;
} pc_af_subpath;

typedef struct pc_af_path {
    pc_af_subpath *subpaths;
    size_t nsubpaths;
} pc_af_path;

typedef enum pc_af_blend {
    PC_AF_NORMAL,
    PC_AF_PASS_THROUGH,
    PC_AF_DARKEN,
    PC_AF_DARKER_COLOR,
    PC_AF_MULTIPLY,
    PC_AF_COLOR_BURN,
    PC_AF_LIGHTEN,
    PC_AF_LIGHTER_COLOR,
    PC_AF_SCREEN,
    PC_AF_COLOR_DODGE,
    PC_AF_ADD,
    PC_AF_OVERLAY,
    PC_AF_SOFT_LIGHT,
    PC_AF_HARD_LIGHT,
    PC_AF_VIVID_LIGHT,
    PC_AF_PIN_LIGHT,
    PC_AF_LINEAR_LIGHT,
    PC_AF_HARD_MIX,
    PC_AF_DIFFERENCE,
    PC_AF_EXCLUSION,
    PC_AF_SUBTRACT,
    PC_AF_HUE,
    PC_AF_SATURATION,
    PC_AF_LUMINOSITY,
    PC_AF_COLOR,
    PC_AF_AVERAGE,
    PC_AF_NEGATION,
    PC_AF_REFLECT,
    PC_AF_GLOW,
    PC_AF_ERASE
} pc_af_blend;

/* Colours as stored: RGB and grey in the document's RGB space, CMYK through its CMYK profile,
 * Lab D50; alpha straight. v holds (r, g, b), (c, m, y, k), (v) or (L, a, b). */
typedef enum pc_af_ckind { PC_AF_RGB, PC_AF_CMYK, PC_AF_GRAY, PC_AF_LAB } pc_af_ckind;

typedef struct pc_af_color {
    pc_af_ckind kind;
    double v[4];
    double alpha;
} pc_af_color;

typedef enum pc_af_gkind { PC_AF_LINEAR, PC_AF_RADIAL, PC_AF_CONICAL } pc_af_gkind;

typedef struct pc_af_stop {
    double offset;
    pc_af_color color;
    double bias; /* midpoint bias to the next stop (0.5 even) */
} pc_af_stop;

/* Where between this stop (0) and the next (1) the blend reaches half way. */
double pc_af_stop_half_point(const pc_af_stop *s);

typedef enum pc_af_pkind { PC_AF_PAINT_NONE, PC_AF_SOLID, PC_AF_GRADIENT } pc_af_pkind;

typedef struct pc_af_paint {
    pc_af_pkind kind;
    pc_af_color color; /* SOLID */
    pc_af_gkind gkind; /* GRADIENT */
    pc_af_stop *stops;
    size_t nstops;
    pc_af_affine transform; /* unit space to document pixels */
} pc_af_paint;

typedef enum pc_af_cap { PC_AF_BUTT, PC_AF_ROUND_CAP, PC_AF_SQUARE } pc_af_cap;
typedef enum pc_af_join { PC_AF_MITER, PC_AF_ROUND_JOIN, PC_AF_BEVEL } pc_af_join;
typedef enum pc_af_salign { PC_AF_CENTER, PC_AF_INSIDE, PC_AF_OUTSIDE } pc_af_salign;

typedef struct pc_af_stroke {
    pc_af_paint paint;
    double width;
    pc_af_cap cap;
    pc_af_join join;
    double miter_limit;
    pc_af_salign align;
    bool has_dash;
    double *dash; /* dash and gap lengths in document pixels */
    size_t ndash;
    double dash_phase;
    bool behind; /* painted below the fill */
} pc_af_stroke;

typedef struct pc_af_text_run {
    char *text;
    char *postscript;
    char *family;
    int64_t weight;
    bool italic;
    double size;
    double tracking;
    bool has_leading;
    double leading;
    pc_af_paint fill;
} pc_af_text_run;

typedef enum pc_af_align { PC_AF_LEFT, PC_AF_ACENTER, PC_AF_RIGHT, PC_AF_JUSTIFY } pc_af_align;

typedef struct pc_af_text {
    pc_af_text_run *runs;
    size_t nruns;
    pc_af_align align;
    pc_af_point anchor;
    bool has_frame;
    pc_af_rect frame;
    pc_af_affine transform;
} pc_af_text;

typedef enum pc_af_pixfmt { PC_AF_RGBA8, PC_AF_ENCODED } pc_af_pixfmt;

typedef struct pc_af_image {
    uint32_t width;
    uint32_t height;
    pc_af_pixfmt format;
    uint8_t *pixels;
    size_t npixels;
    pc_af_affine transform; /* pixel space to document pixels */
} pc_af_image;

typedef enum pc_af_nkind {
    PC_AF_LAYER,
    PC_AF_GROUP,
    PC_AF_ARTBOARD,
    PC_AF_SHAPE,
    PC_AF_TEXT,
    PC_AF_IMAGE,
    PC_AF_UNSUPPORTED
} pc_af_nkind;

typedef struct pc_af_node pc_af_node;

struct pc_af_node {
    pc_af_tag cls;
    char *name;
    bool visible;
    bool locked;
    double opacity;
    pc_af_blend blend;
    pc_af_nkind kind;
    /* ARTBOARD */
    pc_af_rect rect;
    pc_af_paint *background;
    size_t nbackground;
    /* SHAPE */
    pc_af_path path;
    pc_af_paint *fills;
    size_t nfills;
    pc_af_stroke *strokes;
    size_t nstrokes;
    bool even_odd;
    /* TEXT */
    pc_af_text text;
    /* IMAGE */
    pc_af_image image;
    bool has_mask;
    pc_af_path mask;
    bool has_pixel_mask;
    pc_af_image pixel_mask;
    pc_af_node *children; /* bottom to top */
    size_t nchildren;
};

typedef struct pc_af_spread {
    pc_af_rect bounds;
    pc_af_rect *pages;
    size_t npages;
    bool transparent;
    pc_af_node *nodes;
    size_t nnodes;
} pc_af_spread;

typedef struct pc_af_document {
    double dpi;
    pc_af_spread *spreads;
    size_t nspreads;
    char *saved_by; /* NULL when absent */
    char **warnings;
    size_t nwarnings;
} pc_af_document;

/* Reads a document: archive, doc.dat and the model. Free with pc_af_document_fini. */
pc_status pc_af_read(const uint8_t *b, size_t n, pc_af_limits limits, pc_af_document *out, pc_error *err);
void pc_af_document_fini(pc_af_document *d);

/* ---- preview ---- */

#define PC_AF_MAX_PREVIEW_BYTES ((size_t)16 << 20)
#define PC_AF_MAX_PREVIEW_DIMENSION 4096u

typedef struct pc_af_preview {
    const uint8_t *png; /* borrowed from the file bytes */
    size_t len;
    uint32_t width;
    uint32_t height;
} pc_af_preview;

/* The indexed Thmb record's PNG, every chunk CRC checked. */
pc_status pc_af_preview_read(const uint8_t *b, size_t n, pc_af_preview *out, pc_error *err);

#endif
