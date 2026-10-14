/* Import / export dispatch (port of crates/io/src/lib.rs), plus the detection helpers lib.rs
 * takes from affinity.rs, svg.rs and psd_import.rs (see io.h's chunk plan). */
#include "io/io.h"

#include <stdlib.h>
#include <string.h>

#include "affinity/affinity.h"
#include "base/blob.h"
#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "format/format.h"
#include "io/kra.h"
#include "io/ora.h"
#include "io/pdn.h"
#include "io/psd_thumbnail.h"
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

/* codecs::detect(bytes).is_some(). */
static bool detected(const uint8_t *bytes, size_t len)
{
    pc_cformat f;
    return pc_cformat_detect(bytes, len, &f);
}

/* Extension names can be wrong (an image downloaded as WebP and renamed .svg). Valid plain SVG is
 * recognised by its XML content; an otherwise unknown .svg is passed to the SVG parser for a
 * useful malformed-file error (a recognised raster signature still wins), and .svgz needs the
 * gzip magic. */
static bool is_svg_input(const char *name, const uint8_t *bytes, size_t len)
{
    return pc_io_is_svg(bytes, len) || (has_extension(name, "svg") && !detected(bytes, len)) ||
           (has_extension(name, "svgz") && len >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b);
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
    if (pc_io_is_ora(bytes, len) || has_extension(name, "ora")) {
        return pc_io_ora_import(name, bytes, len, ctl, out, err);
    }
    /* Krita files are ZIP containers carrying a merged PNG. A recognised raster signature still
     * wins over a misleading .kra suffix (as with SVG imports). */
    if (pc_io_is_kra(bytes, len) || (has_extension(name, "kra") && !detected(bytes, len))) {
        return pc_io_kra_import(name, bytes, len, ctl, out, err);
    }
    if (has_extension(name, "pdn") || (len >= 4 && memcmp(bytes, "PDN3", 4) == 0)) {
        return pc_io_pdn_import(name, bytes, len, ctl, out, err);
    }
    if (pc_io_is_raw(bytes, len)) {
        return pc_io_import_raw(name, bytes, len, out, err);
    }
    if (is_svg_input(name, bytes, len)) {
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
    o.maximize_compatibility = opts->psd_maximize_compatibility;
    memset(&r, 0, sizeof r);
    pc_io_warnings_init(&r.warnings);
    st = pc_io_document_to_psd_with(doc, &o, &file, &r.warnings, err);
    if (st != PC_OK) {
        pc_io_warnings_fini(&r.warnings);
        return st;
    }
    st = pc_io_strip_foreign_order_blocks(&file, &r.warnings, err);
    /* Image preview (resource 1036), beside the other resources Photoshop writes ahead of the
     * version info. A preview that can't be rendered is a warning, not a failure. */
    if (st == PC_OK && opts->image_previews) {
        pc_psd_resource thumb;
        pc_error te;
        if (pc_io_psd_thumbnail_resource(doc, &thumb, &te) == PC_OK) {
            pc_psd_resource *v = realloc(file.resources, (file.nresources + 1) * sizeof *v);
            if (v == NULL) {
                pc_psd_resource_fini(&thumb);
                st = pc_io_nomem(err, "writing the image preview");
            } else {
                size_t at = file.nresources;
                file.resources = v;
                for (size_t i = 0; i < file.nresources; i++) {
                    if (v[i].id == PC_PSD_RES_VERSION_INFO) {
                        at = i;
                        break;
                    }
                }
                memmove(&v[at + 1], &v[at], (file.nresources - at) * sizeof *v);
                v[at] = thumb;
                file.nresources++;
            }
        } else {
            pc_error we;
            st = pc_io_warn(&r.warnings, &we, "image preview not saved: %s", te.msg);
        }
    }
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
    if (strcmp(ext, "pdn") == 0) {
        free(ext);
        return pc_io_unsupported(
            err, "PDN is import-only; save as .pcraft to preserve all layers and blend modes");
    }
    for (size_t i = 0; i < PC_IO_AFFINITY_EXTENSION_COUNT; i++) {
        if (strcmp(ext, PC_IO_AFFINITY_EXTENSIONS[i]) == 0) {
            free(ext);
            return pc_io_unsupported(
                err, "Affinity export is not implemented; save a new PSD, PNG or .pcraft copy");
        }
    }
    if (strcmp(ext, "ora") == 0) {
        free(ext);
        return pc_io_ora_export(doc, out, err);
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

/* An RGB merged image with alpha at its native sample precision, the white matte removed (the
 * PSD module's RGBA8 preview already removes it, at 8 bits). Port of the RGB branch of the
 * reference's merged_composite. */
static pc_status rgb_alpha_composite(const pc_psd_file *file, float (**px)[4], size_t *n, pc_error *err)
{
    const pc_psd_header *h = &file->header;
    pc_psd_bytes raw = {NULL, 0};
    pc_psd_bytes planes8[4];
    const uint8_t *pl[4];
    size_t plane;
    size_t count;
    size_t step = h->depth == 32 ? 4 : h->depth == 16 ? 2 : 1;
    float (*v)[4] = NULL;
    pc_status st = pc_psd_header_validate(h, err);
    size_t i;
    int c;
    memset(planes8, 0, sizeof planes8);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_file_decode_merged(file, &raw, err);
    if (st != PC_OK) {
        return st;
    }
    plane = h->channels > 0 ? raw.len / h->channels : 0;
    if (plane == 0) {
        pc_psd_bytes_fini(&raw);
        return pc_fail(err, PC_ERR_FORMAT, "invalid data: empty RGB composite");
    }
    if (raw.len / plane < 4) {
        pc_psd_bytes_fini(&raw);
        return pc_fail(err, PC_ERR_FORMAT, "invalid data: missing RGB composite channels");
    }
    count = plane / step;
    for (c = 0; c < 4 && st == PC_OK; c++) {
        pl[c] = raw.data + (size_t)c * plane;
        if (step == 1) {
            st = pc_psd_plane_to_u8(pl[c], plane, h->depth, h->width, h->height, &planes8[c], err);
            if (st == PC_OK) {
                pl[c] = planes8[c].data;
                if (c == 0 || planes8[c].len < count) {
                    count = planes8[c].len;
                }
            }
        }
    }
    if (st == PC_OK) {
        v = pc_malloc_array(count > 0 ? count : 1, sizeof *v);
        if (v == NULL) {
            st = pc_io_nomem(err, "un-matting the merged image");
        }
    }
    for (i = 0; st == PC_OK && i < count; i++) {
        for (c = 0; c < 4; c++) {
            float f;
            if (step == 4) {
                f = pc_f32_from_bits(pc_load_be32(pl[c] + 4 * i));
            } else if (step == 2) {
                f = (float)pc_load_be16(pl[c] + 2 * i) / 65535.0f;
            } else {
                f = (float)pl[c][i] / 255.0f;
            }
            v[i][c] = f;
        }
        for (c = 0; c < 3; c++) {
            v[i][c] = pc_io_unmatte(v[i][c], v[i][3], 1.0f);
            if (h->depth != 32) {
                v[i][c] = pc_clampf(v[i][c], 0.0f, 1.0f);
            }
        }
    }
    for (c = 0; c < 4; c++) {
        pc_psd_bytes_fini(&planes8[c]);
    }
    pc_psd_bytes_fini(&raw);
    if (st != PC_OK) {
        free(v);
        return st;
    }
    *px = v;
    *n = count;
    return PC_OK;
}

pc_status pc_io_merged_composite(const pc_psd_file *file, float (**px)[4], size_t *n, pc_error *err)
{
    pc_psd_image8 img;
    uint16_t mode = file->header.color_mode;
    pc_psd_file flat;
    pc_document doc;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_compose_buffer buf = {{0, 0, 0, 0}, NULL, 0};
    pc_status st;
    if (mode == PC_PSD_MODE_RGB && pc_psd_file_merged_has_alpha(file)) {
        return rgb_alpha_composite(file, px, n, err);
    }
    /* CMYK and Lab go through the colour-managed model conversion (the PSD module's RGBA preview
     * is a naive, profile-free conversion). The RGBA8 preview also clips signed/HDR values, so
     * 32-bit merged images are decoded at their original depth through the model path below. */
    if (mode != PC_PSD_MODE_LAB && mode != PC_PSD_MODE_CMYK && file->header.depth != 32 &&
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
    /* Generic path (Lab, CMYK and others) via the document's model conversion. Discard the
     * layers but keep the merged-alpha marker: otherwise an extra spot/alpha channel in an
     * opaque composite is mistaken for transparency (matches the reference's merged_composite). */
    bool merged_alpha = pc_psd_file_merged_has_alpha(file);
    st = pc_psd_file_clone(file, &flat, err);
    if (st != PC_OK) {
        return st;
    }
    if (flat.has_layer_info) {
        pc_psd_layer_info_fini(&flat.layer_info);
    }
    memset(&flat.layer_info, 0, sizeof flat.layer_info);
    flat.has_layer_info = true;
    flat.layer_info.merged_alpha = merged_alpha;
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
