/* The one-call API and FormatError helpers (crates/format/src/lib.rs), and the load driver
 * (store::load / store::read_manifest). */
#include "format/bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "base/bytes.h"

void pc_format_fail_set(pc_error *err, pc_status code, const char *prefix, const char *fmt, ...)
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

static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Insert `prefix` before err's message (cut to fit). */
static void prefix_msg(pc_error *err, const char *prefix)
{
    char old[PC_ERROR_MSG_MAX];
    if (err == NULL) {
        return;
    }
    memcpy(old, err->msg, sizeof old);
    old[sizeof old - 1] = '\0';
    if (snprintf(err->msg, sizeof err->msg, "%s%s", prefix, old) < 0) {
        err->msg[0] = '\0';
    }
}

pc_status pc_format_io(pc_status s, pc_error *err)
{
    if (s != PC_OK && s != PC_ERR_NOMEM && err != NULL && !starts_with(err->msg, "I/O: ")) {
        prefix_msg(err, "I/O: ");
    }
    return s;
}

pc_status pc_format_json(pc_status s, pc_error *err)
{
    if (s == PC_OK || s == PC_ERR_NOMEM) {
        return s;
    }
    if (err != NULL) {
        prefix_msg(err, "manifest JSON: ");
        err->code = PC_ERR_FORMAT;
        err->sys_errno = 0;
    }
    return PC_ERR_FORMAT;
}

pc_format_err_kind pc_format_error_kind(pc_status s, const pc_error *err)
{
    if (s == PC_OK) {
        return PC_FORMAT_ERR_NONE;
    }
    if (s == PC_ERR_NOMEM) {
        return PC_FORMAT_ERR_OTHER;
    }
    if (err != NULL) {
        const char *m = err->msg;
        if (starts_with(m, "corrupt .pcraft bundle: ")) {
            return PC_FORMAT_ERR_CORRUPT;
        }
        if (starts_with(m, "unsupported .pcraft feature: ")) {
            return PC_FORMAT_ERR_UNSUPPORTED;
        }
        if (starts_with(m, "bundle written by a newer version")) {
            return PC_FORMAT_ERR_TOO_NEW;
        }
        if (starts_with(m, "limit exceeded: ")) {
            return PC_FORMAT_ERR_LIMIT;
        }
        if (starts_with(m, "I/O: ")) {
            return PC_FORMAT_ERR_IO;
        }
        if (starts_with(m, "manifest JSON: ")) {
            return PC_FORMAT_ERR_JSON;
        }
        if (s == PC_ERR_INVALID && strstr(m, "` is NaN or infinite, which a .pcraft manifest") != NULL) {
            return PC_FORMAT_ERR_NON_FINITE;
        }
    }
    switch (s) {
    case PC_ERR_IO:
    case PC_ERR_NOT_FOUND:
        return PC_FORMAT_ERR_IO;
    case PC_ERR_FORMAT:
        return PC_FORMAT_ERR_CORRUPT;
    case PC_ERR_UNSUPPORTED:
        return PC_FORMAT_ERR_UNSUPPORTED;
    case PC_ERR_LIMIT:
        return PC_FORMAT_ERR_LIMIT;
    default:
        return PC_FORMAT_ERR_OTHER;
    }
}

pc_format_load_opts pc_format_load_opts_default(void)
{
    pc_format_load_opts o;
    o.max_manifest_bytes = (size_t)256 << 20;
    o.max_blob_bytes = (size_t)1 << 30;
    o.max_total_bytes = (uint64_t)16 << 30;
    o.preserve_ids = true;
    return o;
}

bool pc_format_is_pcraft(const uint8_t *bytes, size_t len)
{
    static const char NAME[] = PC_FORMAT_MANIFEST;
    pc_zip_reader r;
    bool found;
    if (len < 4 || memcmp(bytes, "PK\x03\x04", 4) != 0) {
        return false;
    }
    /* Bundles written here place the manifest first; re-zipped bundles may reorder entries. */
    if (len >= 30) {
        size_t name_len = pc_load_le16(bytes + 26);
        if (name_len == sizeof NAME - 1 && len - 30 >= name_len && memcmp(bytes + 30, NAME, name_len) == 0) {
            return true;
        }
    }
    if (pc_zip_reader_open(&r, bytes, len, NULL) != PC_OK) {
        return false;
    }
    found = pc_zip_find(&r, NAME, sizeof NAME - 1) != NULL;
    pc_zip_reader_close(&r);
    return found;
}

pc_status pc_format_save_to_bytes(const pc_document *doc, const pc_format_save_opts *opts, pc_buf *out,
                                  pc_error *err)
{
    pc_pcraft_writer *w = NULL;
    pc_status s = pc_pcraft_writer_new(&w, err);
    if (s == PC_OK) {
        s = pc_pcraft_writer_save_zip(w, doc, opts, out, NULL, err);
    }
    pc_pcraft_writer_free(w);
    return s;
}

pc_status pc_format_read_manifest_source(const pc_format_source *src, const pc_format_load_opts *opts,
                                         pc_format_manifest **out, pc_error *err)
{
    pc_buf raw = PC_BUF_INIT;
    pc_status s = pc_format_read_manifest_raw(src, opts->max_manifest_bytes, &raw, err);
    if (s == PC_OK) {
        s = pc_format_manifest_parse(raw.data, raw.len, out, err);
    }
    pc_buf_fini(&raw);
    return s;
}

pc_status pc_format_load_source(const pc_format_source *src, const pc_format_load_opts *opts,
                                pc_document *out, pc_error *err)
{
    pc_format_load_opts o = *opts;
    for (;;) {
        pc_format_manifest *m = NULL;
        pc_format_fetch *fetch = NULL;
        pc_document doc;
        uint64_t max_id = 0;
        pc_status s = pc_format_read_manifest_source(src, &o, &m, err);
        if (s == PC_OK) {
            s = pc_format_fetch_new(src, &o, &fetch, err);
        }
        if (s == PC_OK) {
            s = pc_format_manifest_to_document(m, fetch, o.preserve_ids, &doc, &max_id, err);
        }
        pc_format_fetch_free(fetch);
        pc_format_manifest_free(m);
        if (s != PC_OK) {
            return s;
        }
        if (o.preserve_ids && !pc_format_reserve_ids_through(max_id)) {
            /* Ids far beyond our counter: remap instead of risking collisions. */
            pc_document_fini(&doc);
            o.preserve_ids = false;
            continue;
        }
        *out = doc;
        return PC_OK;
    }
}

pc_status pc_format_load_from_bytes_with(const uint8_t *bytes, size_t len, const pc_format_load_opts *opts,
                                         pc_document *out, pc_error *err)
{
    pc_format_source *src = NULL;
    pc_status s = pc_format_source_zip(bytes, len, &src, err);
    if (s == PC_OK) {
        s = pc_format_load_source(src, opts, out, err);
    }
    pc_format_source_free(src);
    return s;
}

pc_status pc_format_load_from_bytes(const uint8_t *bytes, size_t len, pc_document *out, pc_error *err)
{
    pc_format_load_opts o = pc_format_load_opts_default();
    return pc_format_load_from_bytes_with(bytes, len, &o, out, err);
}

pc_status pc_format_load_path_with(const char *path, const pc_format_load_opts *opts, pc_document *out,
                                   pc_error *err)
{
    struct stat st;
    pc_buf bytes = PC_BUF_INIT;
    pc_status s;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        pc_format_source *src = NULL;
        s = pc_format_source_dir(path, &src, err);
        if (s == PC_OK) {
            s = pc_format_load_source(src, opts, out, err);
        }
        pc_format_source_free(src);
        return s;
    }
    s = pc_read_file(path, &bytes, err);
    if (s != PC_OK) {
        pc_buf_fini(&bytes);
        return pc_format_io(s, err);
    }
    s = pc_format_load_from_bytes_with(bytes.data, bytes.len, opts, out, err);
    pc_buf_fini(&bytes);
    return s;
}

pc_status pc_format_load_path(const char *path, pc_document *out, pc_error *err)
{
    pc_format_load_opts o = pc_format_load_opts_default();
    return pc_format_load_path_with(path, &o, out, err);
}

pc_status pc_format_read_manifest(const uint8_t *bytes, size_t len, pc_format_manifest **out, pc_error *err)
{
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_format_source *src = NULL;
    pc_status s = pc_format_source_zip(bytes, len, &src, err);
    if (s == PC_OK) {
        s = pc_format_read_manifest_source(src, &o, out, err);
    }
    pc_format_source_free(src);
    return s;
}

pc_status pc_format_read_thumbnail(const uint8_t *bytes, size_t len, bool *present, pc_buf *out,
                                   pc_error *err)
{
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_format_source *src = NULL;
    pc_format_manifest *m = NULL;
    pc_status s = pc_format_source_zip(bytes, len, &src, err);
    if (s == PC_OK) {
        s = pc_format_read_manifest_source(src, &o, &m, err);
    }
    if (s == PC_OK) {
        const char *thumb = pc_format_manifest_thumbnail(m);
        if (thumb == NULL) {
            *present = false;
        } else {
            s = pc_format_source_get(src, thumb, (size_t)64 << 20, out, err);
            if (s == PC_OK) {
                *present = true;
            }
        }
    }
    pc_format_manifest_free(m);
    pc_format_source_free(src);
    return s;
}
