/* OpenRaster (.ora) import and export (port of crates/io/src/ora.rs, R10).
 *
 * Layout from the public OpenRaster specification (https://www.openraster.org/, stack.xml
 * 0.0.x): a ZIP whose first, stored entry is `mimetype` = "image/openraster", a stack.xml layer
 * tree, one PNG per layer, and a mergedimage.png / Thumbnails/thumbnail.png rendering.
 *
 * Import keeps raster layers, groups (isolated and pass-through), names, opacity, visibility,
 * svg:* and krita:* composite operations and the edit-locked / alpha-preserve flags; everything
 * else is skipped with a warning. Export writes the same plus a merged image and a thumbnail;
 * other layer kinds are written as their rendered pixels and layer masks are baked in.
 *
 * Untrusted input: entry sizes, pixel budgets (PC_IO_ORA_MAX_SIDE and MAX_PIXEL_BYTES), the XML
 * byte size and the layer/group node count are all capped, group nesting is bounded, and every
 * offset and size is range-checked. Errors are "OpenRaster: <reason>" (PC_IO_ERR_ORA).
 * Thread-safe (no shared state). */
#include "io/ora.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/num.h"
#include "base/str.h"
#include "base/utf8.h"
#include "codecs/codecs.h"
#include "color/blend.h"
#include "format/zip.h"
#include "geom/geom.h"
#include "io/core.h"
#include "io/flat.h"
#include "io/ora_internal.h"
#include "io/svg_internal.h" /* the sx_* XML DOM parser (shared with the SVG importer) */
#include "raster/surface.h"

/* The `mimetype` entry of an OpenRaster archive. */
const char ORA_MIMETYPE[] = "image/openraster";

/* ============================================================================================
 * little-endian reads for zip_mimetype (the ZIP reader validates the rest of the archive)
 * ==========================================================================================*/

static bool ora_rd_u16(const uint8_t *b, size_t len, size_t o, uint16_t *v)
{
    if (o + 2 > len) {
        return false;
    }
    *v = (uint16_t)((uint16_t)b[o] | (uint16_t)((uint16_t)b[o + 1] << 8));
    return true;
}

static bool ora_rd_u32(const uint8_t *b, size_t len, size_t o, uint32_t *v)
{
    if (o + 4 > len) {
        return false;
    }
    *v = (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) |
         ((uint32_t)b[o + 3] << 24);
    return true;
}

const uint8_t *pc_io_zip_mimetype(const uint8_t *bytes, size_t len, size_t *mime_len)
{
    uint32_t sig;
    uint16_t method;
    uint32_t size32;
    uint16_t name_len;
    uint16_t extra_len;
    size_t size;
    size_t start;

    if (mime_len != NULL) {
        *mime_len = 0;
    }
    if (bytes == NULL) {
        return NULL;
    }
    if (!ora_rd_u32(bytes, len, 0, &sig) || sig != 0x04034b50u) {
        return NULL;
    }
    if (!ora_rd_u16(bytes, len, 8, &method) || method != 0) {
        return NULL;
    }
    if (!ora_rd_u32(bytes, len, 18, &size32) || !ora_rd_u16(bytes, len, 26, &name_len) ||
        !ora_rd_u16(bytes, len, 28, &extra_len)) {
        return NULL;
    }
    size = (size_t)size32;
    if (size > 256) {
        return NULL;
    }
    /* The name must be exactly "mimetype": check its length before the compare so an attacker's
     * name_len cannot read past the 8-byte literal (fixed-length memcmp). */
    if (name_len != 8u || (size_t)30 + name_len > len || memcmp(bytes + 30, "mimetype", 8) != 0) {
        return NULL;
    }
    start = (size_t)30 + name_len + extra_len;
    if (start > len || size > len - start) {
        return NULL;
    }
    if (mime_len != NULL) {
        *mime_len = size;
    }
    return bytes + start;
}

bool pc_io_is_ora(const uint8_t *bytes, size_t len)
{
    size_t ml = 0;
    const uint8_t *p = pc_io_zip_mimetype(bytes, len, &ml);
    return p != NULL && ml == ORA_MIMETYPE_LEN && memcmp(p, ORA_MIMETYPE, ORA_MIMETYPE_LEN) == 0;
}

/* ============================================================================================
 * blend-mode id maps (both directions), matching the reference exactly
 * ==========================================================================================*/

/* Blend modes by Krita composite-op id (its .kra attribute and the krita:<id> ORA ops). *approx
 * is set when the mode only approximates Krita's formula. */
static bool ora_krita_blend(const char *id, pc_blend_mode *mode, bool *approx)
{
    static const struct {
        const char *id;
        pc_blend_mode mode;
        bool approx;
    } map[] = {
        {"normal", PC_BLEND_NORMAL, false},
        {"multiply", PC_BLEND_MULTIPLY, false},
        {"screen", PC_BLEND_SCREEN, false},
        {"overlay", PC_BLEND_OVERLAY, false},
        {"darken", PC_BLEND_DARKEN, false},
        {"lighten", PC_BLEND_LIGHTEN, false},
        {"dodge", PC_BLEND_COLOR_DODGE, false},
        {"burn", PC_BLEND_COLOR_BURN, false},
        {"hard_light", PC_BLEND_HARD_LIGHT, false},
        {"soft_light", PC_BLEND_SOFT_LIGHT, false},
        {"soft_light_svg", PC_BLEND_SOFT_LIGHT, true}, /* the W3C curve, close to Photoshop's */
        {"diff", PC_BLEND_DIFFERENCE, false},
        {"exclusion", PC_BLEND_EXCLUSION, false},
        {"linear_burn", PC_BLEND_LINEAR_BURN, false},
        {"linear_dodge", PC_BLEND_LINEAR_DODGE, false},
        {"add", PC_BLEND_LINEAR_DODGE, false},
        {"subtract", PC_BLEND_SUBTRACT, false},
        {"divide", PC_BLEND_DIVIDE, false},
        {"vivid_light", PC_BLEND_VIVID_LIGHT, false},
        {"linear light", PC_BLEND_LINEAR_LIGHT, false},
        {"pin_light", PC_BLEND_PIN_LIGHT, false},
        {"hard_mix_photoshop", PC_BLEND_HARD_MIX, false},
        {"hard mix", PC_BLEND_HARD_MIX, true}, /* Krita thresholds differently from Photoshop */
        {"darker color", PC_BLEND_DARKER_COLOR, false},
        {"lighter color", PC_BLEND_LIGHTER_COLOR, false},
        {"hue", PC_BLEND_HUE, false},
        {"saturation", PC_BLEND_SATURATION, false},
        {"color", PC_BLEND_COLOR, false},
        {"luminize", PC_BLEND_LUMINOSITY, false},
        {"dissolve", PC_BLEND_DISSOLVE, false},
        {"reflect", PC_BLEND_REFLECT, false},
        {"glow", PC_BLEND_GLOW, false},
        {"negation", PC_BLEND_NEGATION, false},
        {"xor", PC_BLEND_XOR, true},
    };
    size_t i;
    for (i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcmp(id, map[i].id) == 0) {
            *mode = map[i].mode;
            *approx = map[i].approx;
            return true;
        }
    }
    return false;
}

/* The SVG compositing names of the OpenRaster specification. */
const ora_svg_op ORA_SVG_OPS[] = {
    {"svg:src-over", PC_BLEND_NORMAL},
    {"svg:multiply", PC_BLEND_MULTIPLY},
    {"svg:screen", PC_BLEND_SCREEN},
    {"svg:overlay", PC_BLEND_OVERLAY},
    {"svg:darken", PC_BLEND_DARKEN},
    {"svg:lighten", PC_BLEND_LIGHTEN},
    {"svg:color-dodge", PC_BLEND_COLOR_DODGE},
    {"svg:color-burn", PC_BLEND_COLOR_BURN},
    {"svg:hard-light", PC_BLEND_HARD_LIGHT},
    {"svg:soft-light", PC_BLEND_SOFT_LIGHT},
    {"svg:difference", PC_BLEND_DIFFERENCE},
    {"svg:color", PC_BLEND_COLOR},
    {"svg:luminosity", PC_BLEND_LUMINOSITY},
    {"svg:hue", PC_BLEND_HUE},
    {"svg:saturation", PC_BLEND_SATURATION},
};
const size_t ORA_SVG_OP_COUNT = sizeof ORA_SVG_OPS / sizeof ORA_SVG_OPS[0];

/* ============================================================================================
 * grouped warnings (port of the Rust Warnings struct): every distinct general message once,
 * per-layer ones grouped by message with the first five layer names listed. Shared with export.
 * ==========================================================================================*/

void ora_warnings_init(ora_warnings *w) { memset(w, 0, sizeof *w); }

void ora_warnings_fini(ora_warnings *w)
{
    size_t i;
    size_t j;
    if (w == NULL) {
        return;
    }
    for (i = 0; i < w->ng; i++) {
        free(w->general[i]);
    }
    free(w->general);
    for (i = 0; i < w->np; i++) {
        for (j = 0; j < w->per[i].n; j++) {
            free(w->per[i].layers[j]);
        }
        free(w->per[i].layers);
        free(w->per[i].msg);
    }
    free(w->per);
    memset(w, 0, sizeof *w);
}

static char *ora_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p != NULL) {
        memcpy(p, s, n);
    }
    return p;
}

/* Appends p (owned) to a char** vector, growing it. On failure frees p. */
static pc_status ora_strvec_push(char ***v, size_t *n, size_t *cap, char *p, pc_error *err)
{
    if (p == NULL) {
        return pc_io_nomem(err, "importing an OpenRaster document");
    }
    if (*n == *cap) {
        size_t c = *cap == 0 ? 4 : *cap * 2;
        char **nv = c > SIZE_MAX / sizeof *nv ? NULL : realloc(*v, c * sizeof *nv);
        if (nv == NULL) {
            free(p);
            return pc_io_nomem(err, "importing an OpenRaster document");
        }
        *v = nv;
        *cap = c;
    }
    (*v)[(*n)++] = p;
    return PC_OK;
}

/* A distinct general message (deduplicated). */
pc_status ora_note(ora_warnings *w, const char *msg, pc_error *err)
{
    size_t i;
    for (i = 0; i < w->ng; i++) {
        if (strcmp(w->general[i], msg) == 0) {
            return PC_OK;
        }
    }
    return ora_strvec_push(&w->general, &w->ng, &w->cg, ora_strdup(msg), err);
}

/* A per-layer message, grouped: the same message lists every layer it applies to (once). */
pc_status ora_layer_warn(ora_warnings *w, const char *layer, const char *msg, pc_error *err)
{
    size_t i;
    size_t j;
    ora_warn_group *g = NULL;
    for (i = 0; i < w->np; i++) {
        if (strcmp(w->per[i].msg, msg) == 0) {
            g = &w->per[i];
            break;
        }
    }
    if (g == NULL) {
        if (w->np == w->cp) {
            size_t c = w->cp == 0 ? 4 : w->cp * 2;
            ora_warn_group *np = c > SIZE_MAX / sizeof *np ? NULL : realloc(w->per, c * sizeof *np);
            if (np == NULL) {
                return pc_io_nomem(err, "importing an OpenRaster document");
            }
            w->per = np;
            w->cp = c;
        }
        g = &w->per[w->np];
        memset(g, 0, sizeof *g);
        g->msg = ora_strdup(msg);
        if (g->msg == NULL) {
            return pc_io_nomem(err, "importing an OpenRaster document");
        }
        w->np++;
    }
    for (j = 0; j < g->n; j++) {
        if (strcmp(g->layers[j], layer) == 0) {
            return PC_OK;
        }
    }
    return ora_strvec_push(&g->layers, &g->n, &g->cap, ora_strdup(layer), err);
}

/* Flattens the warnings into the import result's list: general ones first, then one line per
 * grouped per-layer message with its first five quoted layer names. */
pc_status ora_warnings_finish(const ora_warnings *w, pc_io_warnings *out, pc_error *err)
{
    size_t i;
    size_t j;
    pc_status st;
    for (i = 0; i < w->ng; i++) {
        st = pc_io_warn(out, err, "%s", w->general[i]);
        if (st != PC_OK) {
            return st;
        }
    }
    for (i = 0; i < w->np; i++) {
        const ora_warn_group *g = &w->per[i];
        pc_buf line = PC_BUF_INIT;
        size_t shown = g->n < 5 ? g->n : 5;
        st = pc_buf_appendf(&line, err, "%s (", g->msg);
        for (j = 0; j < shown && st == PC_OK; j++) {
            st = pc_buf_appendf(&line, err, "%s\"%s\"", j == 0 ? "" : ", ", g->layers[j]);
        }
        if (st == PC_OK && g->n > 5) {
            st = pc_buf_appendf(&line, err, " and %zu more", g->n - 5);
        }
        if (st == PC_OK) {
            st = pc_buf_append_byte(&line, (uint8_t)')', err);
        }
        if (st == PC_OK) {
            st = pc_io_warn(out, err, "%s", pc_buf_cstr(&line));
        }
        pc_buf_fini(&line);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* ============================================================================================
 * XML attribute helpers (match roxmltree + Rust's parsing exactly)
 * ==========================================================================================*/

static const char *ora_attr(const sx_node *n, const char *name)
{
    return sx_attr_get(n, NULL, name);
}

/* A NUL-terminated copy of str::trim(s) (Unicode white space); NULL only on allocation failure. */
static char *ora_trim_dup(const char *s)
{
    size_t start = 0;
    size_t trimmed = pc_utf8_trim(s, strlen(s), &start);
    char *p = malloc(trimmed + 1);
    if (p != NULL) {
        memcpy(p, s + start, trimmed);
        p[trimmed] = '\0';
    }
    return p;
}

/* str::trim(s) equals lit? */
static bool ora_trim_eq(const char *s, const char *lit)
{
    size_t start = 0;
    size_t trimmed = pc_utf8_trim(s, strlen(s), &start);
    return trimmed == strlen(lit) && memcmp(s + start, lit, trimmed) == 0;
}

/* str::trim(s) is empty? */
static bool ora_trim_empty(const char *s)
{
    size_t start = 0;
    return pc_utf8_trim(s, strlen(s), &start) == 0;
}

/* A number attribute: absent gives `def`, present must parse as a finite f64 (else an error). */
static pc_status ora_num(const sx_node *node, const char *name, double def, double *out, pc_error *err)
{
    const char *raw = ora_attr(node, name);
    char *t;
    char *end;
    double v;
    size_t i;
    if (raw == NULL) {
        *out = def;
        return PC_OK;
    }
    t = ora_trim_dup(raw);
    if (t == NULL) {
        return pc_io_nomem(err, "parsing an OpenRaster number");
    }
    /* Rust's f64::from_str rejects hex floats that strtod would accept. */
    i = (t[0] == '+' || t[0] == '-') ? 1 : 0;
    if (t[i] == '0' && (t[i + 1] == 'x' || t[i + 1] == 'X')) {
        free(t);
        return pc_io_ora_error(err, "invalid %s attribute `%s`", name, raw);
    }
    v = strtod(t, &end);
    if (end == t || *end != '\0' || !isfinite(v)) {
        free(t);
        return pc_io_ora_error(err, "invalid %s attribute `%s`", name, raw);
    }
    free(t);
    *out = v;
    return PC_OK;
}

/* A rounded integer offset, bounded to +-4 canvases. */
static pc_status ora_int(const sx_node *node, const char *name, int32_t *out, pc_error *err)
{
    double v;
    pc_status st = ora_num(node, name, 0.0, &v, err);
    if (st != PC_OK) {
        return st;
    }
    v = round(v);
    if (fabs(v) > (double)PC_IO_ORA_MAX_SIDE * 4.0) {
        return pc_io_ora_error(err, "%s offset %.0f is out of range", name, v);
    }
    *out = pc_f64_to_i32(v);
    return PC_OK;
}

/* A boolean flag attribute (present and "true" or "1"). */
static bool ora_flag(const sx_node *node, const char *name)
{
    const char *v = ora_attr(node, name);
    return v != NULL && (ora_trim_eq(v, "true") || ora_trim_eq(v, "1"));
}

/* A composite-op attribute (already trimmed) as a blend mode; unknown names open as Normal. */
static pc_status ora_composite_op(const char *op, const char *layer, ora_warnings *w, pc_blend_mode *out,
                                  pc_error *err)
{
    size_t i;
    pc_blend_mode mode;
    bool approx;
    for (i = 0; i < ORA_SVG_OP_COUNT; i++) {
        if (strcmp(ORA_SVG_OPS[i].name, op) == 0) {
            if (strcmp(op, "svg:soft-light") == 0) {
                pc_status st = ora_note(
                    w, "svg:soft-light (the W3C curve) opens as Photoshop's Soft Light, which differs slightly",
                    err);
                if (st != PC_OK) {
                    return st;
                }
            }
            *out = ORA_SVG_OPS[i].mode;
            return PC_OK;
        }
    }
    if (strcmp(op, "svg:plus") == 0) {
        *out = PC_BLEND_LINEAR_DODGE;
        return PC_OK;
    }
    if (strcmp(op, "svg:exclusion") == 0) { /* not in the spec, but written by several apps */
        *out = PC_BLEND_EXCLUSION;
        return PC_OK;
    }
    if (strncmp(op, "krita:", 6) == 0 && ora_krita_blend(op + 6, &mode, &approx)) {
        if (approx) {
            char buf[256];
            pc_status st;
            (void)snprintf(buf, sizeof buf, "Krita's %s blend mode is approximated by %s", op,
                           pc_blend_label(mode));
            st = ora_note(w, buf, err);
            if (st != PC_OK) {
                return st;
            }
        }
        *out = mode;
        return PC_OK;
    }
    {
        char buf[256];
        (void)snprintf(buf, sizeof buf, "composite operation %s is not supported; it opens as Normal", op);
        pc_status st = ora_layer_warn(w, layer, buf, err);
        if (st != PC_OK) {
            return st;
        }
    }
    *out = PC_BLEND_NORMAL;
    return PC_OK;
}

/* ============================================================================================
 * import: the node tree
 * ==========================================================================================*/

typedef struct ora_node ora_node;
struct ora_node {
    bool is_group;
    char *name; /* owned */
    bool visible;
    float opacity;
    pc_blend_mode blend;
    bool lock_all;
    bool lock_transparency;
    /* raster */
    int image; /* index into the reader's images, or -1 */
    int32_t x, y;
    /* group */
    ora_node *children;
    size_t nchildren;
    size_t capchildren;
};

static void ora_node_free(ora_node *n)
{
    size_t i;
    if (n == NULL) {
        return;
    }
    free(n->name);
    for (i = 0; i < n->nchildren; i++) {
        ora_node_free(&n->children[i]);
    }
    free(n->children);
    memset(n, 0, sizeof *n);
}

static void ora_nodes_free(ora_node *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        ora_node_free(&v[i]);
    }
    free(v);
}

typedef struct {
    const pc_zip_reader *zip;
    const pc_interrupt *ctl;
    size_t nodes;
    size_t budget;
    ora_warnings warn;
    pc_cimage *images;
    size_t nimages;
    size_t capimages;
} ora_reader;

/* Decodes the layer image `src`, registers it and returns its index (or -1 when missing). */
static pc_status ora_decode_png(ora_reader *r, const char *src, const char *layer, int *out_index,
                                pc_error *err)
{
    const pc_zip_entry *e;
    pc_buf data = PC_BUF_INIT;
    pc_error inner;
    pc_cdecode_opts opts;
    pc_cimage image;
    pc_status st;
    size_t px;
    size_t rgba16;
    pc_cformat detected;

    *out_index = -1;
    e = pc_zip_find(r->zip, src, strlen(src));
    if (e == NULL) {
        return ora_layer_warn(&r->warn, layer,
                              "the layer's image is missing from the archive; the layer opens empty", err);
    }
    pc_error_clear(&inner);
    st = pc_zip_read(r->zip, e, ORA_MAX_ENTRY, &data, &inner);
    if (st != PC_OK) {
        pc_buf_fini(&data);
        if (st == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", inner.msg);
        }
        return pc_io_ora_error(err, "%s: %s", src, inner.msg);
    }
    opts = pc_cdecode_opts_default();
    opts.limits.max_width = PC_IO_ORA_MAX_SIDE;
    opts.limits.max_height = PC_IO_ORA_MAX_SIDE;
    opts.limits.max_alloc = (uint64_t)r->budget;
    pc_error_clear(&inner);
    st = pc_cdecode_with(data.data != NULL ? data.data : (const uint8_t *)"", data.len, &opts, &image,
                         &inner);
    if (st != PC_OK) {
        pc_status rc;
        if (st == PC_ERR_NOMEM) {
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", inner.msg);
        } else {
            rc = pc_io_ora_error(err, "%s: %s", src, inner.msg);
        }
        pc_buf_fini(&data);
        return rc;
    }
    /* Charge the decoded layer against the 1 GiB budget (as 16-bit RGBA). */
    if (!pc_size_mul((size_t)image.width, (size_t)image.height, &px) || !pc_size_mul(px, 8, &rgba16)) {
        pc_cimage_fini(&image);
        pc_buf_fini(&data);
        return pc_io_ora_error(err, "layer image size overflow");
    }
    if (rgba16 > r->budget) {
        pc_cimage_fini(&image);
        pc_buf_fini(&data);
        return pc_io_ora_error(err, "decoded layers exceed the 1 GiB limit");
    }
    r->budget -= rgba16;
    if (!pc_cformat_detect(data.data, data.len, &detected) || detected != PC_CF_PNG) {
        st = ora_note(&r->warn,
                      "some layer images are not PNG, as OpenRaster expects; they were read anyway", err);
        if (st != PC_OK) {
            pc_cimage_fini(&image);
            pc_buf_fini(&data);
            return st;
        }
    }
    pc_buf_fini(&data);
    /* Append the decoded image. */
    if (r->nimages == r->capimages) {
        size_t c = r->capimages == 0 ? 8 : r->capimages * 2;
        pc_cimage *nv = c > SIZE_MAX / sizeof *nv ? NULL : realloc(r->images, c * sizeof *nv);
        if (nv == NULL) {
            pc_cimage_fini(&image);
            return pc_io_nomem(err, "importing an OpenRaster document");
        }
        r->images = nv;
        r->capimages = c;
    }
    r->images[r->nimages] = image;
    *out_index = (int)r->nimages;
    r->nimages++;
    return PC_OK;
}

/* Reads shared node properties into *n (name, visibility, opacity, blend, locks). */
static pc_status ora_props(const sx_node *node, ora_node *n, size_t index, ora_warnings *w, pc_error *err)
{
    const char *name = ora_attr(node, "name");
    const char *vis = ora_attr(node, "visibility");
    const char *co = ora_attr(node, "composite-op");
    char *nm;
    char *co_trim = NULL;
    double opacity;
    pc_status st;

    if (name != NULL && !ora_trim_empty(name)) {
        nm = ora_strdup(name);
    } else {
        char buf[64];
        (void)snprintf(buf, sizeof buf, "Layer %zu", index);
        nm = ora_strdup(buf);
    }
    if (nm == NULL) {
        return pc_io_nomem(err, "importing an OpenRaster document");
    }
    st = ora_num(node, "opacity", 1.0, &opacity, err);
    if (st != PC_OK) {
        free(nm);
        return st;
    }
    if (co != NULL) {
        co_trim = ora_trim_dup(co);
        if (co_trim == NULL) {
            free(nm);
            return pc_io_nomem(err, "importing an OpenRaster document");
        }
    }
    st = ora_composite_op(co_trim != NULL ? co_trim : "svg:src-over", nm, w, &n->blend, err);
    free(co_trim);
    if (st != PC_OK) {
        free(nm);
        return st;
    }
    n->name = nm; /* ownership moves to the node (freed by ora_node_free) */
    n->visible = vis == NULL || !ora_trim_eq(vis, "hidden");
    n->opacity = (float)pc_clampd(opacity, 0.0, 1.0);
    n->lock_all = ora_flag(node, "edit-locked");
    n->lock_transparency = ora_flag(node, "alpha-preserve");
    return PC_OK;
}

/* Appends *node (moved; the source is emptied on success, left untouched on failure). */
static pc_status ora_nodes_push(ora_node **v, size_t *n, size_t *cap, ora_node *node, pc_error *err)
{
    if (*n == *cap) {
        size_t c = *cap == 0 ? 4 : *cap * 2;
        ora_node *nv = c > SIZE_MAX / sizeof *nv ? NULL : realloc(*v, c * sizeof *nv);
        if (nv == NULL) {
            return pc_io_nomem(err, "importing an OpenRaster document");
        }
        *v = nv;
        *cap = c;
    }
    (*v)[(*n)++] = *node;
    memset(node, 0, sizeof *node);
    return PC_OK;
}

/* Walks a <stack> element, decoding layer PNGs and building the node tree. */
static pc_status ora_stack(ora_reader *r, const sx_node *stack, size_t depth, ora_node **out,
                           size_t *out_n, pc_error *err)
{
    ora_node *nodes = NULL;
    size_t n = 0;
    size_t cap = 0;
    const sx_node *child;
    pc_status st = PC_OK;

    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_io_unsupported(err, "layer groups nested deeper than %d", PC_MAX_GROUP_DEPTH);
    }
    for (child = sx_first_element(stack); child != NULL; child = sx_next_element(child)) {
        size_t index;
        if (pc_interrupt_cancelled(r->ctl)) {
            st = pc_io_cancelled(err);
            goto fail;
        }
        r->nodes++;
        if (r->nodes > ORA_MAX_NODES) {
            st = pc_io_ora_error(err, "more than %zu layers", ORA_MAX_NODES);
            goto fail;
        }
        index = r->nodes;
        if (strcmp(child->name, "layer") == 0) {
            ora_node node;
            const char *src;
            memset(&node, 0, sizeof node);
            node.image = -1;
            st = ora_props(child, &node, index, &r->warn, err);
            if (st == PC_OK) {
                st = ora_int(child, "x", &node.x, err);
            }
            if (st == PC_OK) {
                st = ora_int(child, "y", &node.y, err);
            }
            if (st != PC_OK) {
                ora_node_free(&node);
                goto fail;
            }
            src = ora_attr(child, "src");
            if (src != NULL) {
                char *src_trim = ora_trim_dup(src);
                int image_index = -1;
                if (src_trim == NULL) {
                    st = pc_io_nomem(err, "importing an OpenRaster document");
                    ora_node_free(&node);
                    goto fail;
                }
                st = ora_decode_png(r, src_trim, node.name, &image_index, err);
                node.image = image_index;
                free(src_trim);
            } else {
                st = ora_layer_warn(&r->warn, node.name, "the layer has no image; it opens empty", err);
            }
            if (st != PC_OK) {
                ora_node_free(&node);
                goto fail;
            }
            st = ora_nodes_push(&nodes, &n, &cap, &node, err);
            if (st != PC_OK) {
                ora_node_free(&node);
                goto fail;
            }
        } else if (strcmp(child->name, "stack") == 0) {
            ora_node node;
            const char *iso = ora_attr(child, "isolation");
            bool isolate = iso != NULL && ora_trim_eq(iso, "isolate");
            memset(&node, 0, sizeof node);
            node.is_group = true;
            node.image = -1;
            st = ora_props(child, &node, index, &r->warn, err);
            if (st != PC_OK) {
                ora_node_free(&node);
                goto fail;
            }
            /* An auto group without its own blend mode is a pass-through group. */
            if (!isolate && node.blend == PC_BLEND_NORMAL) {
                node.blend = PC_BLEND_PASS_THROUGH;
            }
            st = ora_stack(r, child, depth + 1, &node.children, &node.nchildren, err);
            if (st != PC_OK) {
                ora_node_free(&node);
                goto fail;
            }
            node.capchildren = node.nchildren;
            st = ora_nodes_push(&nodes, &n, &cap, &node, err);
            if (st != PC_OK) {
                ora_node_free(&node);
                goto fail;
            }
        } else {
            const char *nm = ora_attr(child, "name");
            char buf[128];
            (void)snprintf(buf, sizeof buf, "OpenRaster <%s> elements are not supported and were skipped",
                           child->name);
            st = ora_layer_warn(&r->warn, nm != NULL ? nm : child->name, buf, err);
            if (st != PC_OK) {
                goto fail;
            }
        }
    }
    *out = nodes;
    *out_n = n;
    return PC_OK;
fail:
    ora_nodes_free(nodes, n);
    return st;
}

/* Deepest sample type across every decoded image (float stays float). */
static pc_sample_type ora_document_depth(const pc_cimage *images, size_t n)
{
    pc_sample_type d = PC_SAMPLE_TYPE_U8;
    size_t i;
    for (i = 0; i < n; i++) {
        pc_csample s = images[i].sample;
        if (d == PC_SAMPLE_TYPE_F32 || s == PC_CS_F32 || s == PC_CS_F16) {
            d = PC_SAMPLE_TYPE_F32;
        } else if (d == PC_SAMPLE_TYPE_U16 || s == PC_CS_U16) {
            if (d != PC_SAMPLE_TYPE_F32) {
                d = PC_SAMPLE_TYPE_U16;
            }
        }
    }
    return d;
}

static int32_t ora_sat_add_i32(int32_t a, int32_t b)
{
    int64_t s = (int64_t)a + (int64_t)b;
    if (s > INT32_MAX) {
        return INT32_MAX;
    }
    if (s < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)s;
}

/* Builds the document layer tree bottom-to-top from the nodes (stack.xml lists the top first). */
static pc_status ora_build(const ora_node *nodes, size_t n, pc_cimage *images, size_t nimages,
                           pc_pixel_format fmt, const pc_interrupt *ctl, pc_layer **out, size_t *out_n,
                           pc_error *err)
{
    pc_layer *arr;
    size_t built = 0;
    size_t i;
    pc_status st = PC_OK;

    arr = n == 0 ? NULL : calloc(n, sizeof *arr);
    if (n != 0 && arr == NULL) {
        return pc_io_nomem(err, "importing an OpenRaster document");
    }
    for (i = 0; i < n; i++) {
        const ora_node *node = &nodes[n - 1 - i];
        pc_layer *dst = &arr[built];
        if (pc_interrupt_cancelled(ctl)) {
            st = pc_io_cancelled(err);
            goto fail;
        }
        if (node->is_group) {
            pc_layer *children = NULL;
            size_t nchild = 0;
            st = ora_build(node->children, node->nchildren, images, nimages, fmt, ctl, &children, &nchild,
                           err);
            if (st != PC_OK) {
                goto fail;
            }
            st = pc_layer_group(dst, node->name, children, nchild, err);
            if (st != PC_OK) {
                size_t k;
                for (k = 0; k < nchild; k++) {
                    pc_layer_fini(&children[k]);
                }
                free(children);
                goto fail;
            }
        } else {
            pc_surface surf;
            pc_surface_init(&surf, fmt);
            if (node->image >= 0 && (size_t)node->image < nimages) {
                const pc_cimage *img = &images[node->image];
                pc_cimage conv;
                pc_rect rect;
                size_t bpp = pc_pixel_format_bytes_per_pixel(fmt);
                size_t want;
                st = pc_cimage_convert(img, PC_CL_RGBA, pc_io_csample(fmt.sample), &conv, err);
                if (st != PC_OK) {
                    pc_surface_fini(&surf);
                    goto fail;
                }
                rect = pc_rect_make(node->x, node->y, ora_sat_add_i32(node->x, (int32_t)conv.width),
                                    ora_sat_add_i32(node->y, (int32_t)conv.height));
                want = (size_t)conv.width * (size_t)conv.height * bpp;
                if (pc_rect_width(rect) != conv.width || pc_rect_height(rect) != conv.height ||
                    conv.len != want) {
                    pc_cimage_fini(&conv);
                    pc_surface_fini(&surf);
                    st = pc_io_ora_error(err, "layer image does not fit its position");
                    goto fail;
                }
                st = pc_surface_write_interleaved(&surf, rect, conv.data, conv.len, err);
                pc_cimage_fini(&conv);
                if (st != PC_OK) {
                    pc_surface_fini(&surf);
                    goto fail;
                }
                pc_surface_prune(&surf);
            }
            st = pc_io_raster_layer(node->name, &surf, dst, err);
            pc_surface_fini(&surf);
            if (st != PC_OK) {
                goto fail;
            }
        }
        dst->visible = node->visible;
        dst->opacity = node->opacity;
        dst->blend = node->blend;
        dst->locks.all = node->lock_all;
        dst->locks.transparency = node->lock_transparency;
        built++;
    }
    *out = arr;
    *out_n = n;
    return PC_OK;
fail:
    for (i = 0; i < built; i++) {
        pc_layer_fini(&arr[i]);
    }
    free(arr);
    return st;
}

pc_status pc_io_ora_import(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                           pc_io_import_result *out, pc_error *err)
{
    pc_zip_reader zip;
    bool zip_open = false;
    pc_buf mime = PC_BUF_INIT;
    pc_buf xmlbuf = PC_BUF_INIT;
    sv_arena arena;
    bool arena_open = false;
    sx_doc doc;
    char xmsg[256];
    ora_reader r;
    ora_node *nodes = NULL;
    size_t nnodes = 0;
    pc_io_import_result res;
    const sx_node *image;
    const sx_node *rootstack;
    const sx_node *c;
    double wv;
    double hv;
    double xres;
    double yres;
    uint32_t w;
    uint32_t h;
    pc_sample_type depth;
    pc_pixel_format fmt;
    pc_layer *layers = NULL;
    size_t nlayers = 0;
    pc_status st;
    size_t i;

    memset(&res, 0, sizeof res);
    pc_io_warnings_init(&res.warnings);
    memset(&r, 0, sizeof r);
    r.ctl = ctl;
    r.budget = ORA_MAX_PIXEL_BYTES;
    ora_warnings_init(&r.warn);

    st = pc_zip_reader_open(&zip, bytes, len, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, st == PC_ERR_NOMEM ? "" : "OpenRaster: ");
    }
    zip_open = true;
    r.zip = &zip;

    st = pc_zip_read_by_name(&zip, "mimetype", 256, &mime, err);
    if (st != PC_OK) {
        st = pc_io_err_prefix(st, err, st == PC_ERR_NOMEM ? "" : "OpenRaster: ");
        goto done;
    }
    {
        /* trim_ascii then compare. */
        const uint8_t *m = mime.data;
        size_t ml = mime.len;
        while (ml > 0 && (m[0] == ' ' || m[0] == '\t' || m[0] == '\n' || m[0] == '\r' || m[0] == '\f')) {
            m++;
            ml--;
        }
        while (ml > 0 && (m[ml - 1] == ' ' || m[ml - 1] == '\t' || m[ml - 1] == '\n' ||
                          m[ml - 1] == '\r' || m[ml - 1] == '\f')) {
            ml--;
        }
        if (ml != ORA_MIMETYPE_LEN || memcmp(m, ORA_MIMETYPE, ORA_MIMETYPE_LEN) != 0) {
            st = pc_io_ora_error(err, "the archive's mimetype is not image/openraster");
            goto done;
        }
    }

    st = pc_zip_read_by_name(&zip, "stack.xml", ORA_MAX_STACK_XML, &xmlbuf, err);
    if (st != PC_OK) {
        st = pc_io_err_prefix(st, err, st == PC_ERR_NOMEM ? "" : "OpenRaster: ");
        goto done;
    }
    /* stack.xml must be UTF-8. */
    i = 0;
    while (i < xmlbuf.len) {
        uint32_t cp;
        if (!pc_utf8_next(xmlbuf.data, xmlbuf.len, &i, &cp)) {
            st = pc_io_ora_error(err, "stack.xml is not UTF-8");
            goto done;
        }
    }
    sv_arena_init(&arena);
    arena_open = true;
    st = sx_parse(&arena, xmlbuf.len > 0 ? (const char *)xmlbuf.data : "", xmlbuf.len, &doc, xmsg,
                  sizeof xmsg);
    if (st != PC_OK || arena.nomem) {
        if (st == PC_ERR_NOMEM || arena.nomem) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory parsing stack.xml");
        } else {
            st = pc_io_ora_error(err, "stack.xml: %s", xmsg);
        }
        goto done;
    }
    image = sx_first_element(doc.root);
    if (image == NULL || strcmp(image->name, "image") != 0) {
        st = pc_io_ora_error(err, "stack.xml has no <image> root");
        goto done;
    }
    st = ora_num(image, "w", 0.0, &wv, err);
    if (st == PC_OK) {
        st = ora_num(image, "h", 0.0, &hv, err);
    }
    if (st != PC_OK) {
        goto done;
    }
    if (!(wv >= 1.0 && wv <= (double)PC_IO_ORA_MAX_SIDE) || wv != floor(wv)) {
        st = pc_io_ora_error(err, "image w %.0f must be a whole number of pixels from 1 to %u", wv,
                             PC_IO_ORA_MAX_SIDE);
        goto done;
    }
    if (!(hv >= 1.0 && hv <= (double)PC_IO_ORA_MAX_SIDE) || hv != floor(hv)) {
        st = pc_io_ora_error(err, "image h %.0f must be a whole number of pixels from 1 to %u", hv,
                             PC_IO_ORA_MAX_SIDE);
        goto done;
    }
    w = (uint32_t)wv;
    h = (uint32_t)hv;
    {
        size_t px;
        size_t bytes_needed;
        if (!pc_size_mul((size_t)w, (size_t)h, &px) || !pc_size_mul(px, 8, &bytes_needed) ||
            bytes_needed > ORA_MAX_PIXEL_BYTES) {
            st = pc_io_ora_error(err, "the canvas exceeds the 1 GiB pixel limit");
            goto done;
        }
    }
    rootstack = NULL;
    for (c = sx_first_element(image); c != NULL; c = sx_next_element(c)) {
        if (strcmp(c->name, "stack") == 0) {
            rootstack = c;
            break;
        }
    }
    if (rootstack == NULL) {
        st = pc_io_ora_error(err, "stack.xml has no root <stack>");
        goto done;
    }
    st = ora_stack(&r, rootstack, 1, &nodes, &nnodes, err);
    if (st != PC_OK) {
        goto done;
    }
    pc_interrupt_progress(ctl, 0.8f);
    depth = ora_document_depth(r.images, r.nimages);
    if (depth == PC_SAMPLE_TYPE_F32) {
        st = ora_note(&r.warn, "float layer images open as a 32-bit float document", err);
        if (st != PC_OK) {
            goto done;
        }
    }
    fmt.mode = PC_COLOR_MODE_RGB;
    fmt.sample = depth;
    fmt.alpha = true;
    st = pc_document_init(&res.document, name, pc_size_make(w, h), PC_COLOR_MODE_RGB, depth, err);
    if (st != PC_OK) {
        goto done;
    }
    st = ora_build(nodes, nnodes, r.images, r.nimages, fmt, ctl, &layers, &nlayers, err);
    if (st != PC_OK) {
        goto done;
    }
    res.document.layers = layers;
    res.document.nlayers = nlayers;
    layers = NULL;

    st = ora_num(image, "xres", 72.0, &xres, err);
    if (st == PC_OK) {
        st = ora_num(image, "yres", xres, &yres, err);
    }
    if (st != PC_OK) {
        goto done;
    }
    if (xres > 0.0 && xres < 1.0e6) {
        res.document.resolution_dpi = (float)xres;
        if (fabs(xres - yres) >= 0.01) {
            char buf[256];
            (void)snprintf(buf, sizeof buf,
                           "the vertical resolution (%.2f ppi) differs from the horizontal (%.2f ppi); "
                           "the document keeps %.2f ppi for both",
                           yres, xres, xres);
            st = ora_note(&r.warn, buf, err);
            if (st != PC_OK) {
                goto done;
            }
        }
    }
    if (res.document.nlayers == 0) {
        st = ora_note(&r.warn, "the OpenRaster file has no layers", err);
        if (st != PC_OK) {
            goto done;
        }
    }
    st = ora_warnings_finish(&r.warn, &res.warnings, err);
    if (st != PC_OK) {
        goto done;
    }
    res.source_read_only = false;
    res.preview_only = false;
    *out = res;
    memset(&res, 0, sizeof res); /* ownership transferred */
done:
    if (layers != NULL) {
        for (i = 0; i < nlayers; i++) {
            pc_layer_fini(&layers[i]);
        }
        free(layers);
    }
    ora_nodes_free(nodes, nnodes);
    for (i = 0; i < r.nimages; i++) {
        pc_cimage_fini(&r.images[i]);
    }
    free(r.images);
    ora_warnings_fini(&r.warn);
    if (arena_open) {
        sv_arena_fini(&arena);
    }
    pc_buf_fini(&xmlbuf);
    pc_buf_fini(&mime);
    if (zip_open) {
        pc_zip_reader_close(&zip);
    }
    pc_io_import_result_fini(&res);
    return st;
}
