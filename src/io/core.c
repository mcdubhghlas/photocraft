/* Shared io types: warnings, errors, results, options, containers (see core.h). */
#include "io/core.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "psd/types.h"

/* ---- warnings ---- */

void pc_io_warnings_init(pc_io_warnings *w)
{
    w->v = NULL;
    w->n = 0;
    w->cap = 0;
}

void pc_io_warnings_fini(pc_io_warnings *w)
{
    if (w == NULL) {
        return;
    }
    for (size_t i = 0; i < w->n; i++) {
        free(w->v[i]);
    }
    free(w->v);
    pc_io_warnings_init(w);
}

/* Inserts the owned string s at `at`; frees it on failure. */
static pc_status insert_owned(pc_io_warnings *w, size_t at, char *s, pc_error *err)
{
    char **v;
    if (w->n == SIZE_MAX) {
        free(s);
        return pc_io_nomem(err, "adding a warning");
    }
    v = pc_grow_array(w->v, &w->cap, w->n + 1, sizeof *v);
    if (v == NULL) {
        free(s);
        return pc_io_nomem(err, "adding a warning");
    }
    w->v = v;
    if (at > w->n) {
        at = w->n;
    }
    if (at < w->n) {
        memmove(&v[at + 1], &v[at], (w->n - at) * sizeof *v);
    }
    v[at] = s;
    w->n++;
    return PC_OK;
}

/* The formatted text in a new exact-size buffer, or NULL. */
static char *vformat(const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char *s;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        return NULL;
    }
    s = pc_malloc((size_t)n + 1);
    if (s == NULL) {
        return NULL;
    }
    if (vsnprintf(s, (size_t)n + 1, fmt, ap) < 0) {
        s[0] = '\0';
    }
    return s;
}

pc_status pc_io_warn(pc_io_warnings *w, pc_error *err, const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = vformat(fmt, ap);
    va_end(ap);
    if (s == NULL) {
        return pc_io_nomem(err, "formatting a warning");
    }
    return insert_owned(w, w->n, s, err);
}

pc_status pc_io_warn_at(pc_io_warnings *w, size_t at, pc_error *err, const char *fmt, ...)
{
    va_list ap;
    char *s;
    va_start(ap, fmt);
    s = vformat(fmt, ap);
    va_end(ap);
    if (s == NULL) {
        return pc_io_nomem(err, "formatting a warning");
    }
    return insert_owned(w, at, s, err);
}

pc_status pc_io_warnings_extend(pc_io_warnings *w, const char *prefix, char *const *v, size_t n,
                                pc_error *err)
{
    for (size_t i = 0; i < n; i++) {
        pc_status s = pc_io_warn(w, err, "%s%s", prefix, v[i] != NULL ? v[i] : "");
        if (s != PC_OK) {
            return s;
        }
    }
    return PC_OK;
}

pc_status pc_io_warnings_append(pc_io_warnings *w, pc_io_warnings *from, pc_error *err)
{
    pc_status st = PC_OK;
    size_t i = 0;
    for (; i < from->n && st == PC_OK; i++) {
        st = insert_owned(w, w->n, from->v[i], err); /* frees the string on failure */
        from->v[i] = NULL;
    }
    for (; i < from->n; i++) {
        free(from->v[i]);
    }
    free(from->v);
    pc_io_warnings_init(from);
    return st;
}

void pc_io_warnings_remove(pc_io_warnings *w, size_t i)
{
    if (i >= w->n) {
        return;
    }
    free(w->v[i]);
    memmove(&w->v[i], &w->v[i + 1], (w->n - i - 1) * sizeof *w->v);
    w->n--;
}

bool pc_io_warnings_any(const pc_io_warnings *w, const char *needle)
{
    for (size_t i = 0; i < w->n; i++) {
        if (strstr(w->v[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

pc_status pc_io_unequal_resolution_warning(double x, double y, pc_io_warnings *w, pc_error *err)
{
    /* Same {:.2} rounding as printf (both round the exact binary value half to even). */
    if (!(fabs(x - y) >= 0.01)) {
        return PC_OK;
    }
    return pc_io_warn(w, err,
                      "the vertical resolution (%.2f ppi) differs from the horizontal (%.2f ppi); "
                      "the document keeps %.2f ppi for both",
                      y, x, x);
}

/* ---- errors ---- */

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

pc_io_err_kind pc_io_error_kind(pc_status s, const pc_error *err)
{
    const char *m;
    if (s == PC_ERR_CANCELLED) {
        return PC_IO_ERR_CANCELLED;
    }
    if (s == PC_OK || s == PC_ERR_NOMEM || err == NULL) {
        return PC_IO_ERR_OTHER;
    }
    m = err->msg;
    if (starts_with(m, "PSD: ")) {
        return PC_IO_ERR_PSD;
    }
    if (starts_with(m, "codec: ")) {
        return PC_IO_ERR_CODEC;
    }
    if (starts_with(m, "unknown file format: ")) {
        return PC_IO_ERR_UNKNOWN_FORMAT;
    }
    if (starts_with(m, "unsupported: ")) {
        return PC_IO_ERR_UNSUPPORTED;
    }
    if (starts_with(m, "pcraft: ")) {
        return PC_IO_ERR_PCRAFT;
    }
    if (starts_with(m, "SVG: ")) {
        return PC_IO_ERR_SVG;
    }
    if (starts_with(m, "PDN: ")) {
        return PC_IO_ERR_PDN;
    }
    if (starts_with(m, "OpenRaster: ")) {
        return PC_IO_ERR_ORA;
    }
    if (starts_with(m, "Krita: ")) {
        return PC_IO_ERR_KRA;
    }
    if (starts_with(m, "not a camera raw file") || starts_with(m, "unsupported camera raw: ") ||
        starts_with(m, "malformed camera raw: ") || starts_with(m, "camera raw exceeds limits: ")) {
        return PC_IO_ERR_RAW;
    }
    return PC_IO_ERR_OTHER;
}

pc_status pc_io_err_prefix(pc_status s, pc_error *err, const char *prefix)
{
    char body[PC_ERROR_MSG_MAX];
    if (s == PC_OK || s == PC_ERR_NOMEM || err == NULL) {
        return s;
    }
    memcpy(body, err->msg, sizeof body);
    body[sizeof body - 1] = '\0';
    err->code = s;
    if (snprintf(err->msg, sizeof err->msg, "%s%s", prefix, body) < 0) {
        err->msg[0] = '\0';
    }
    return s;
}

void pc_io_fail_set(pc_error *err, pc_status code, const char *prefix, const char *fmt, ...)
{
    char body[PC_ERROR_MSG_MAX];
    va_list ap;
    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    if (vsnprintf(body, sizeof body, fmt, ap) < 0) {
        body[0] = '\0';
    }
    va_end(ap);
    pc_fail_set(err, code, "%s%s", prefix, body);
}

/* ---- results and options ---- */

void pc_io_import_result_fini(pc_io_import_result *r)
{
    if (r == NULL) {
        return;
    }
    pc_document_fini(&r->document);
    pc_io_warnings_fini(&r->warnings);
}

void pc_io_export_result_fini(pc_io_export_result *r)
{
    if (r == NULL) {
        return;
    }
    free(r->bytes);
    r->bytes = NULL;
    r->len = 0;
    pc_io_warnings_fini(&r->warnings);
}

pc_io_export_opts pc_io_export_opts_default(void)
{
    pc_io_export_opts o;
    o.encode = pc_cencode_opts_default();
    o.force_psb = false;
    o.tiff_layers = false;
    o.xmp = PC_IO_XMP_ALL;
    o.image_previews = true;
    o.psd_maximize_compatibility = true;
    return o;
}

pc_io_psd_export_opts pc_io_psd_export_opts_default(void)
{
    pc_io_psd_export_opts o;
    o.force_psb = false;
    o.merged_matte = true;
    o.merged_compression = PC_PSD_RLE;
    o.maximize_compatibility = true;
    return o;
}

/* ---- containers ---- */

void pc_io_raw_blocks_fini(pc_io_raw_blocks *b)
{
    if (b == NULL) {
        return;
    }
    for (size_t i = 0; i < b->n; i++) {
        free(b->v[i].data);
    }
    free(b->v);
    b->v = NULL;
    b->n = 0;
    b->cap = 0;
}

pc_status pc_io_raw_blocks_push_take(pc_io_raw_blocks *b, const uint8_t key[4], uint8_t **data, size_t len,
                                     pc_error *err)
{
    pc_io_raw_block *v = b->n < SIZE_MAX ? pc_grow_array(b->v, &b->cap, b->n + 1, sizeof *v) : NULL;
    if (v == NULL) {
        free(*data);
        *data = NULL;
        return pc_io_nomem(err, "adding a block");
    }
    b->v = v;
    memcpy(v[b->n].key, key, 4);
    v[b->n].data = *data;
    v[b->n].len = len;
    *data = NULL;
    b->n++;
    return PC_OK;
}

pc_status pc_io_raw_blocks_push(pc_io_raw_blocks *b, const uint8_t key[4], const void *data, size_t len,
                                pc_error *err)
{
    uint8_t *copy = pc_malloc(len);
    if (copy == NULL) {
        return pc_io_nomem(err, "copying a block");
    }
    if (len > 0) {
        memcpy(copy, data, len);
    }
    return pc_io_raw_blocks_push_take(b, key, &copy, len, err);
}

const pc_io_raw_block *pc_io_raw_blocks_find(const pc_io_raw_blocks *b, const uint8_t key[4])
{
    for (size_t i = 0; i < b->n; i++) {
        if (memcmp(b->v[i].key, key, 4) == 0) {
            return &b->v[i];
        }
    }
    return NULL;
}

size_t pc_io_raw_blocks_remove_key(pc_io_raw_blocks *b, const uint8_t key[4])
{
    size_t kept = 0;
    size_t removed = 0;
    for (size_t i = 0; i < b->n; i++) {
        if (memcmp(b->v[i].key, key, 4) == 0) {
            free(b->v[i].data);
            removed++;
        } else {
            b->v[kept++] = b->v[i];
        }
    }
    b->n = kept;
    return removed;
}

void pc_io_strs_fini(pc_io_strs *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->n; i++) {
        pc_str_fini(&s->v[i]);
    }
    free(s->v);
    s->v = NULL;
    s->n = 0;
    s->cap = 0;
}

pc_status pc_io_strs_push(pc_io_strs *s, const char *p, size_t len, pc_error *err)
{
    pc_str str = PC_STR_INIT;
    pc_str *v;
    pc_status st = pc_str_set(&str, p, len, err);
    if (st != PC_OK) {
        return st;
    }
    v = s->n < SIZE_MAX ? pc_grow_array(s->v, &s->cap, s->n + 1, sizeof *v) : NULL;
    if (v == NULL) {
        pc_str_fini(&str);
        return pc_io_nomem(err, "adding a string");
    }
    s->v = v;
    v[s->n++] = str;
    return PC_OK;
}

/* ---- document helpers ---- */

pc_status pc_io_push_layer(pc_document *d, pc_layer *layer, pc_error *err)
{
    return pc_document_insert_above(d, NULL, layer, NULL, err);
}

pc_status pc_io_raster_layer(const char *name, pc_surface *s, pc_layer *out, pc_error *err)
{
    pc_layer_content c;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_RASTER;
    c.u.raster = *s;
    st = pc_layer_init(out, name, &c, err);
    if (st == PC_OK) {
        memset(s, 0, sizeof *s);
        pc_surface_init(s, pc_surface_format(&out->content.u.raster));
    }
    return st;
}

pc_status pc_io_set_blob(pc_blob **slot, const void *data, size_t len, pc_error *err)
{
    pc_blob *b = NULL;
    pc_status st = pc_blob_new(data, len, &b, err);
    if (st != PC_OK) {
        return st;
    }
    pc_blob_release(*slot);
    *slot = b;
    return PC_OK;
}
