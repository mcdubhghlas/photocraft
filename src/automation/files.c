/* Port of crates/automation/src/files.rs. */
#include "automation/files.h"

#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "automation/automation.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "compose/compose.h"
#include "format/format.h"
#include "io/io.h"

void pc_auto_opened_fini(pc_auto_opened *o)
{
    if (!o) {
        return;
    }
    pc_document_fini(&o->document);
    pc_io_warnings_fini(&o->warnings);
    o->source_read_only = false;
}

/* Lowercased extension of a file name/path (after the final '.'), without a leading dot, or NULL.
 * Only a dot in the last path component counts (as Path::extension). Writes into out[cap]. */
static const char *ext_of(const char *name, char *out, size_t cap)
{
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    const char *bslash = strrchr(base, '\\');
    if (bslash) {
        base = bslash + 1;
    }
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == '\0') {
        return NULL;
    }
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[n++] = c;
    }
    out[n] = '\0';
    return out;
}

/* basename after the final '/' or '\\', or the whole string. */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            b = p + 1;
        }
    }
    return b;
}

static bool ext_is(const char *name, const char *want)
{
    char buf[32];
    const char *e = ext_of(name, buf, sizeof buf);
    return e && strcasecmp(e, want) == 0;
}

pc_status pc_auto_open_bytes(const char *name, const uint8_t *bytes, size_t len, pc_auto_opened *out,
                             pc_error *err)
{
    pc_io_warnings_init(&out->warnings);
    out->source_read_only = false;
    if (ext_is(name, PC_FORMAT_EXTENSION)) {
        return pc_format_load_from_bytes(bytes, len, &out->document, err);
    }
    pc_io_import_result r;
    pc_status rc = pc_io_import(name, bytes, len, &r, err);
    if (rc != PC_OK) {
        return rc;
    }
    out->document = r.document;             /* move */
    out->warnings = r.warnings;             /* move */
    out->source_read_only = r.source_read_only;
    /* r.document/warnings moved out; do not fini them. */
    return PC_OK;
}

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

pc_status pc_auto_open_path(const char *path, pc_auto_opened *out, pc_error *err)
{
    pc_io_warnings_init(&out->warnings);
    out->source_read_only = false;
    if (is_dir(path)) {
        return pc_format_load_path(path, &out->document, err);
    }
    pc_buf bytes = PC_BUF_INIT;
    if (pc_file_read(path, (size_t)1 << 34, &bytes, err) != PC_OK) {
        pc_error sub = err ? *err : (pc_error){0};
        pc_buf_fini(&bytes);
        return pc_auto_io(err, "%s: %s", path, sub.msg);
    }
    pc_status rc = pc_auto_open_bytes(base_name(path), bytes.data, bytes.len, out, err);
    pc_buf_fini(&bytes);
    return rc;
}

/* Render the .pcraft previews: thumbnail 256 and composite 1024. */
static pc_status render_previews(const pc_document *doc, pc_rgba8_image *thumb, pc_rgba8_image *comp,
                                 pc_error *err)
{
    if (pc_compose_thumbnail(doc, 256, thumb, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    if (pc_compose_thumbnail(doc, 1024, comp, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    return PC_OK;
}

/* Clone `doc` and set its name to the output file's base name, for the manifest the reopened bundle
 * reports (port of named_for_output). */
static pc_status named_for_output(const pc_document *doc, const char *path, pc_document *out, pc_error *err)
{
    if (pc_document_clone(doc, out, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    const char *name = base_name(path);
    if (name[0] != '\0') {
        if (pc_str_set_c(&out->name, name, err) != PC_OK) {
            pc_document_fini(out);
            return err ? err->code : PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

static pc_status save_pcraft_path(const pc_document *doc, const char *path, pc_pcraft_writer *writer,
                                  pc_error *err)
{
    pc_document named;
    if (named_for_output(doc, path, &named, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    pc_rgba8_image thumb = {0, 0, NULL}, comp = {0, 0, NULL};
    pc_status rc = render_previews(&named, &thumb, &comp, err);
    if (rc != PC_OK) {
        pc_document_fini(&named);
        return rc;
    }
    pc_format_save_opts opts = {&thumb, &comp};
    pc_pcraft_writer *w = writer;
    pc_pcraft_writer *local = NULL;
    if (!w) {
        if (pc_pcraft_writer_new(&local, err) != PC_OK) {
            free(thumb.pixels);
            free(comp.pixels);
            pc_document_fini(&named);
            return err ? err->code : PC_ERR_NOMEM;
        }
        w = local;
    }
    pc_format_save_stats stats;
    rc = pc_pcraft_writer_save_path(w, &named, path, &opts, &stats, err);
    free(thumb.pixels);
    free(comp.pixels);
    pc_document_fini(&named);
    pc_pcraft_writer_free(local);
    return rc;
}

pc_status pc_auto_save(const pc_document *doc, const char *path, const char *format_override,
                       const pc_io_export_opts *opts, pc_pcraft_writer *writer, pc_io_warnings *warnings,
                       pc_error *err)
{
    char ebuf[32];
    const char *ext = NULL;
    char ov[32];
    if (format_override) {
        const char *f = format_override;
        while (*f == '.') {
            f++;
        }
        size_t n = 0;
        for (; *f && n + 1 < sizeof ov; f++) {
            char c = *f;
            ov[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        ov[n] = '\0';
        ext = ov;
    } else {
        ext = ext_of(path, ebuf, sizeof ebuf);
    }
    if (!ext) {
        return pc_auto_bad(err, "cannot tell the format of `%s`; pass a format", path);
    }
    if (strcmp(ext, PC_FORMAT_EXTENSION) == 0) {
        return save_pcraft_path(doc, path, writer, err);
    }
    pc_io_export_result r;
    if (pc_io_export(doc, ext, opts, &r, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    /* Crash-safe: a failed write never destroys the previous file. */
    pc_status rc = pc_file_write_atomic(path, r.bytes, r.len, err);
    if (rc == PC_OK && warnings) {
        rc = pc_io_warnings_append(warnings, &r.warnings, err);
    }
    pc_io_export_result_fini(&r);
    return rc;
}

pc_status pc_auto_save_bytes(const pc_document *doc, const char *name, const char *format_override,
                             const pc_io_export_opts *opts, pc_buf *bytes, pc_io_warnings *warnings,
                             pc_error *err)
{
    char ebuf[32];
    const char *ext = NULL;
    char ov[32];
    if (format_override) {
        const char *f = format_override;
        while (*f == '.') {
            f++;
        }
        size_t n = 0;
        for (; *f && n + 1 < sizeof ov; f++) {
            char c = *f;
            ov[n++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        ov[n] = '\0';
        ext = ov;
    } else {
        ext = ext_of(name, ebuf, sizeof ebuf);
    }
    if (!ext) {
        return pc_auto_bad(err, "cannot tell the format of `%s`; pass a format", name);
    }
    if (strcmp(ext, PC_FORMAT_EXTENSION) == 0) {
        pc_document named;
        if (named_for_output(doc, name, &named, err) != PC_OK) {
            return err ? err->code : PC_ERR_NOMEM;
        }
        pc_rgba8_image thumb = {0, 0, NULL}, comp = {0, 0, NULL};
        pc_status rc = render_previews(&named, &thumb, &comp, err);
        if (rc != PC_OK) {
            pc_document_fini(&named);
            return rc;
        }
        pc_format_save_opts sopts = {&thumb, &comp};
        rc = pc_format_save_to_bytes(&named, &sopts, bytes, err);
        free(thumb.pixels);
        free(comp.pixels);
        pc_document_fini(&named);
        return rc;
    }
    pc_io_export_result r;
    if (pc_io_export(doc, ext, opts, &r, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    pc_status rc = pc_buf_append(bytes, r.bytes, r.len, err);
    if (rc == PC_OK && warnings) {
        rc = pc_io_warnings_append(warnings, &r.warnings, err);
    }
    pc_io_export_result_fini(&r);
    return rc;
}

pc_status pc_auto_render_png(const pc_document *doc, uint32_t max_side, pc_buf *png, pc_error *err)
{
    uint32_t w = doc->size.width, h = doc->size.height;
    uint32_t side = max_side == 0 ? (w > h ? w : h) : max_side;
    if (side < 1) {
        side = 1;
    }
    pc_rgba8_image img = {0, 0, NULL};
    if (pc_compose_thumbnail(doc, side, &img, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    size_t len = (size_t)img.width * img.height * 4;
    pc_cimage cimg;
    pc_status rc = pc_cimage_from_copy(img.width, img.height, PC_CL_RGBA, PC_CS_U8, img.pixels, len, &cimg, err);
    free(img.pixels);
    if (rc != PC_OK) {
        return rc;
    }
    pc_cencode_opts opts = pc_cencode_opts_default();
    rc = pc_cencode(&cimg, PC_CF_PNG, &opts, png, err);
    pc_cimage_fini(&cimg);
    return rc;
}
