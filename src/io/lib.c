/* Import / export dispatch (port of crates/io/src/lib.rs), plus the detection helpers lib.rs
 * takes from affinity.rs, svg.rs and psd_import.rs (see io.h's chunk plan). */
#include "io/io.h"

#include <stdlib.h>
#include <string.h>

#include "affinity/affinity.h"
#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "format/format.h"
#include "psd/pixels.h"

bool pc_io_is_psd(const uint8_t *bytes, size_t len)
{
    return len >= 4 && memcmp(bytes, "8BPS", 4) == 0;
}

/* ---- names and extensions ---- */

static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static bool eq_ignore_ascii_case(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        if (ascii_lower((unsigned char)a[i]) != ascii_lower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

/* The last path component (after the last '/' or '\\'). */
static const char *base_name(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    return b;
}

/* The extension after the last '.' of the last path component; NULL when there is no dot.
 * *base_empty says whether nothing precedes that dot. */
static const char *last_ext(const char *name, bool *base_empty)
{
    const char *b = base_name(name);
    const char *dot = strrchr(b, '.');
    if (dot == NULL) {
        return NULL;
    }
    *base_empty = dot == b;
    return dot + 1;
}

/* has_extension: a non-empty base and ext equal to expected, ASCII case-insensitively. */
static bool has_extension(const char *name, const char *expected)
{
    bool base_empty = true;
    const char *ext = last_ext(name, &base_empty);
    return ext != NULL && !base_empty && eq_ignore_ascii_case(ext, strlen(ext), expected);
}

/* extension(): the text after the last '.', '/' or '\\', lower-cased (malloc'd). */
static char *extension_of(const char *name_or_ext)
{
    const char *b = name_or_ext;
    size_t n;
    char *out;
    for (const char *p = name_or_ext; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == '.') {
            b = p + 1;
        }
    }
    n = strlen(b);
    out = pc_malloc(n + 1);
    if (out != NULL) {
        for (size_t i = 0; i < n; i++) {
            out[i] = (char)ascii_lower((unsigned char)b[i]);
        }
        out[n] = '\0';
    }
    return out;
}

const char *const PC_IO_AFFINITY_EXTENSIONS[PC_IO_AFFINITY_EXTENSION_COUNT] = {"af", "afphoto", "afdesign",
                                                                               "afpub"};

bool pc_io_is_affinity(const uint8_t *bytes, size_t len)
{
    return pc_af_is_affinity(bytes, len);
}

bool pc_io_affinity_has_extension(const char *name)
{
    bool base_empty = true;
    const char *ext = last_ext(name, &base_empty);
    if (ext == NULL) {
        return false;
    }
    for (size_t i = 0; i < PC_IO_AFFINITY_EXTENSION_COUNT; i++) {
        if (eq_ignore_ascii_case(ext, strlen(ext), PC_IO_AFFINITY_EXTENSIONS[i])) {
            return true;
        }
    }
    return false;
}

static bool starts_with(const uint8_t *p, size_t n, const char *s)
{
    size_t k = strlen(s);
    return n >= k && memcmp(p, s, k) == 0;
}

static bool contains(const uint8_t *p, size_t n, const char *s)
{
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(p + i, s, k) == 0) {
            return true;
        }
    }
    return false;
}

bool pc_io_is_svg(const uint8_t *bytes, size_t len)
{
    /* The reference works on String::from_utf8_lossy of the first 4096 bytes. ASCII bytes map to
     * themselves under the lossy conversion and invalid bytes become U+FFFD (not white space),
     * so the checks can run on the bytes directly. */
    size_t n = len < 4096 ? len : 4096;
    size_t i = 0;
    uint32_t cp;
    for (;;) { /* trim_start_matches(BOM) */
        size_t j = i;
        if (pc_utf8_next(bytes, n, &j, &cp) && cp == 0xFEFF) {
            i = j;
        } else {
            break;
        }
    }
    for (;;) { /* trim_start() */
        size_t j = i;
        if (pc_utf8_next(bytes, n, &j, &cp) && pc_unicode_is_space(cp)) {
            i = j;
        } else {
            break;
        }
    }
    bytes += i;
    n -= i;
    if (starts_with(bytes, n, "<svg")) {
        return true;
    }
    return (starts_with(bytes, n, "<?xml") || starts_with(bytes, n, "<!DOCTYPE") ||
            starts_with(bytes, n, "<!--")) &&
           contains(bytes, n, "<svg");
}

size_t pc_io_psd_group_depth(const pc_psd_file *file)
{
    size_t n = 0;
    const pc_psd_layer *layers = pc_psd_file_layers(file, &n);
    size_t open = 0;
    size_t depth = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t t = pc_psd_layer_section_type(&layers[i]);
        if (t == PC_PSD_SECTION_DIVIDER) {
            open++;
            depth = depth > open ? depth : open;
        } else if (t == PC_PSD_SECTION_OPEN || t == PC_PSD_SECTION_CLOSED) {
            open = open > 0 ? open - 1 : 0;
        }
    }
    return depth;
}

/* ---- import ---- */

static pc_status check(const pc_interrupt *ctl, pc_error *err)
{
    return ctl != NULL && pc_interrupt_cancelled(ctl) ? pc_io_cancelled(err) : PC_OK;
}

static pc_status import_psd(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                            pc_io_import_result *out, pc_error *err)
{
    pc_psd_file file;
    pc_io_import_result r;
    pc_status st = pc_psd_file_parse(bytes, len, &file, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "PSD: ");
    }
    /* Nesting past the document model's cap could never be saved (.pcraft refuses it): reject
     * the file with the actionable limit. */
    if (pc_io_psd_group_depth(&file) > PC_MAX_GROUP_DEPTH) {
        pc_psd_file_fini(&file);
        return pc_io_unsupported(err, "layer groups nested deeper than %d", PC_MAX_GROUP_DEPTH);
    }
    st = check(ctl, err);
    if (st == PC_OK) {
        if (ctl != NULL) {
            pc_interrupt_progress(ctl, 0.05f);
        }
        memset(&r, 0, sizeof r);
        pc_io_warnings_init(&r.warnings);
        st = pc_io_psd_to_document_with(&file, ctl, &r.document, &r.warnings, err);
        if (st != PC_OK) {
            pc_io_warnings_fini(&r.warnings);
        }
    }
    pc_psd_file_fini(&file);
    if (st != PC_OK) {
        return st;
    }
    st = pc_str_set_c(&r.document.name, name, err);
    if (st == PC_OK) {
        st = pc_io_text_prepare(&r.document, err);
    }
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

static pc_status import_stages(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                               pc_io_import_result *out, pc_error *err)
{
    /* A declared native extension must reach its loader so malformed bundles keep their format
     * errors. */
    if (has_extension(name, PC_FORMAT_EXTENSION) || pc_format_is_pcraft(bytes, len)) {
        pc_io_import_result r;
        pc_status st;
        memset(&r, 0, sizeof r);
        pc_io_warnings_init(&r.warnings);
        st = pc_format_load_from_bytes(bytes, len, &r.document, err);
        if (st != PC_OK) {
            return pc_io_err_prefix(st, err, "pcraft: ");
        }
        *out = r;
        return PC_OK;
    }
    if (pc_io_is_psd(bytes, len)) {
        return import_psd(name, bytes, len, ctl, out, err);
    }
    if (pc_io_is_affinity(bytes, len) || pc_io_affinity_has_extension(name)) {
        return pc_io_affinity_import(name, bytes, len, out, err);
    }
    if (pc_io_is_raw(bytes, len)) {
        return pc_io_import_raw(name, bytes, len, out, err);
    }
    if (has_extension(name, "svg") || has_extension(name, "svgz") || pc_io_is_svg(bytes, len)) {
        return pc_io_svg_import(name, bytes, len, out, err);
    }
    return pc_io_import_flat(name, bytes, len, out, err);
}

pc_status pc_io_import_with(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                            pc_io_import_result *out, pc_error *err)
{
    pc_io_import_result r;
    pc_status st = check(ctl, err);
    if (st != PC_OK) {
        return st;
    }
    if (bytes == NULL) {
        bytes = (const uint8_t *)"";
        len = 0;
    }
    st = import_stages(name != NULL ? name : "", bytes, len, ctl, &r, err);
    if (st != PC_OK) {
        return st;
    }
    st = check(ctl, err);
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    if (ctl != NULL) {
        pc_interrupt_progress(ctl, 1.0f);
    }
    *out = r;
    return PC_OK;
}

pc_status pc_io_import(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                       pc_error *err)
{
    return pc_io_import_with(name, bytes, len, NULL, out, err);
}

/* ---- export ---- */

static pc_status export_pcraft(const pc_document *doc, pc_io_export_result *out, pc_error *err)
{
    pc_rgba8_image thumb = {0, 0, NULL};
    pc_rgba8_image comp = {0, 0, NULL};
    pc_format_save_opts o;
    pc_buf b = PC_BUF_INIT;
    pc_status st = pc_compose_thumbnail(doc, 256, &thumb, err);
    if (st == PC_OK) {
        st = pc_compose_thumbnail(doc, 1024, &comp, err);
    }
    if (st == PC_OK) {
        o.thumbnail = &thumb;
        o.composite = &comp;
        st = pc_format_save_to_bytes(doc, &o, &b, err);
        st = pc_io_err_prefix(st, err, "pcraft: ");
    }
    pc_rgba8_image_fini(&thumb);
    pc_rgba8_image_fini(&comp);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    memset(out, 0, sizeof *out);
    out->bytes = pc_buf_take(&b, &out->len);
    return PC_OK;
}

static pc_status export_psd(const pc_document *doc, bool psb, const pc_io_export_opts *opts,
                            pc_io_export_result *out, pc_error *err)
{
    pc_io_psd_export_opts o = pc_io_psd_export_opts_default();
    pc_io_export_result r;
    pc_psd_file file;
    pc_psd_bytes bytes = {NULL, 0};
    pc_status st;
    o.force_psb = opts->force_psb || psb;
    memset(&r, 0, sizeof r);
    pc_io_warnings_init(&r.warnings);
    st = pc_io_document_to_psd_with(doc, &o, &file, &r.warnings, err);
    if (st != PC_OK) {
        pc_io_warnings_fini(&r.warnings);
        return st;
    }
    st = pc_io_strip_foreign_order_blocks(&file, &r.warnings, err);
    /* Never write a header the reader would refuse (e.g. a zero-sized canvas). */
    if (st == PC_OK) {
        st = pc_io_err_prefix(pc_psd_header_validate(&file.header, err), err, "PSD: ");
    }
    if (st == PC_OK) {
        st = pc_io_err_prefix(pc_psd_file_to_bytes(&file, &bytes, err), err, "PSD: ");
    }
    pc_psd_file_fini(&file);
    if (st != PC_OK) {
        pc_io_export_result_fini(&r);
        return st;
    }
    r.bytes = bytes.data;
    r.len = bytes.len;
    *out = r;
    return PC_OK;
}

pc_status pc_io_export(const pc_document *doc, const char *name_or_ext, const pc_io_export_opts *opts,
                       pc_io_export_result *out, pc_error *err)
{
    pc_io_export_opts defaults = pc_io_export_opts_default();
    char *ext;
    pc_cformat format;
    pc_status st;
    if (opts == NULL) {
        opts = &defaults;
    }
    if (name_or_ext == NULL) {
        name_or_ext = "";
    }
    ext = extension_of(name_or_ext);
    if (ext == NULL) {
        return pc_io_nomem(err, "reading an extension");
    }
    for (size_t i = 0; i < PC_IO_AFFINITY_EXTENSION_COUNT; i++) {
        if (strcmp(ext, PC_IO_AFFINITY_EXTENSIONS[i]) == 0) {
            free(ext);
            return pc_io_unsupported(
                err, "Affinity export is not implemented; save a new PSD, PNG or .pcraft copy");
        }
    }
    if (strcmp(ext, PC_FORMAT_EXTENSION) == 0) {
        free(ext);
        return export_pcraft(doc, out, err);
    }
    if (strcmp(ext, "psd") == 0 || strcmp(ext, "psb") == 0) {
        bool psb = strcmp(ext, "psb") == 0;
        free(ext);
        return export_psd(doc, psb, opts, out, err);
    }
    if (ext[0] == '\0' || !pc_cformat_from_extension(ext, &format)) {
        free(ext);
        pc_io_fail_set(err, PC_ERR_NOT_FOUND, "unknown file format: ", "%s", name_or_ext);
        return PC_ERR_NOT_FOUND;
    }
    free(ext);
    if (format == PC_CF_TIFF && opts->tiff_layers && pc_io_would_write_layers(doc)) {
        return pc_io_export_layered(doc, opts, out, err);
    }
    st = pc_io_export_flat(doc, format, opts, out, err);
    return st;
}

/* ---- the merged composite ---- */

pc_status pc_io_merged_composite(const pc_psd_file *file, float (**px)[4], size_t *n, pc_error *err)
{
    pc_psd_image8 img;
    uint16_t mode = file->header.color_mode;
    pc_psd_file flat;
    pc_document doc;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_compose_buffer buf = {{0, 0, 0, 0}, NULL, 0};
    pc_status st;
    /* CMYK and Lab go through the colour-managed model conversion (the PSD module's RGBA preview
     * is a naive, profile-free conversion). */
    if (mode != PC_PSD_MODE_LAB && mode != PC_PSD_MODE_CMYK &&
        pc_psd_composite_rgba8(file, &img, NULL) == PC_OK) {
        size_t count = (size_t)img.width * img.height;
        bool unmatte = pc_psd_file_merged_has_alpha(file);
        float (*v)[4] = pc_malloc_array(count > 0 ? count : 1, sizeof *v);
        if (v == NULL || img.data.len / 4 < count) {
            free(v);
            pc_psd_image8_fini(&img);
            return v == NULL ? pc_io_nomem(err, "un-matting the merged image")
                             : pc_fail(err, PC_ERR_FORMAT, "invalid data: merged image size mismatch");
        }
        for (size_t i = 0; i < count; i++) {
            for (size_t c = 0; c < 4; c++) {
                v[i][c] = (float)img.data.data[i * 4 + c] / 255.0f;
            }
            if (unmatte) {
                for (size_t c = 0; c < 3; c++) {
                    v[i][c] = pc_clampf(pc_io_unmatte(v[i][c], v[i][3], 1.0f), 0.0f, 1.0f);
                }
            }
        }
        pc_psd_image8_fini(&img);
        *px = v;
        *n = count;
        return PC_OK;
    }
    /* Generic path (Lab, CMYK and others) via the document's model conversion. */
    st = pc_psd_file_clone(file, &flat, err);
    if (st != PC_OK) {
        return st;
    }
    if (flat.has_layer_info) {
        pc_psd_layer_info_fini(&flat.layer_info);
        memset(&flat.layer_info, 0, sizeof flat.layer_info);
        flat.has_layer_info = false;
    }
    st = pc_io_psd_to_document(&flat, &doc, &w, err);
    pc_psd_file_fini(&flat);
    pc_io_warnings_fini(&w);
    if (st != PC_OK) {
        return st;
    }
    if (doc.nlayers == 0 && doc.mode == PC_COLOR_MODE_MULTICHANNEL) {
        /* Multichannel documents keep their channels apart (no layer): composite them. */
        st = pc_compose_flatten(&doc, &buf, err);
    } else if (doc.nlayers == 0 || pc_layer_surface(&doc.layers[0]) == NULL) {
        st = pc_io_unsupported(err, "no merged image");
    } else {
        st = pc_compose_surface_to_buffer(pc_layer_surface(&doc.layers[0]), pc_document_bounds(&doc), &buf,
                                          err);
    }
    pc_document_fini(&doc);
    if (st != PC_OK) {
        return st;
    }
    *px = buf.px;
    *n = buf.len;
    return PC_OK;
}
