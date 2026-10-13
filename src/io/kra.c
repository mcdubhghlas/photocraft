/* Krita (.kra) import (port of crates/io/src/kra.rs, R10): the archive's mergedimage.png opens as
 * one editable raster layer, read-only source. Krita's native layer tree is NOT imported (a
 * warning says so), so never write this document back to the .kra source. The ZIP reader checks
 * entry bounds, sizes and CRC before the PNG decoder runs; the merged image's dimensions and
 * pixel budget are capped like OpenRaster's. */
#include "io/kra.h"

#include <stdio.h>
#include <string.h>

#include "codecs/codecs.h"
#include "format/zip.h"
#include "io/flat.h"
#include "io/ora.h"

#define KRA_MIME "application/x-krita"
#define KRA_MIME_LEN (sizeof(KRA_MIME) - 1)
/* The decompressed mergedimage.png entry is capped at 512 MiB, its decoded pixels at 1 GiB. */
#define KRA_MAX_ENTRY ((size_t)512 << 20)
#define KRA_MAX_PIXELS_BYTES ((uint64_t)1 << 30)

/* The bytes Rust's <[u8]>::trim_ascii strips (u8::is_ascii_whitespace; note: no vertical tab). */
static bool kra_ascii_ws(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

/* mimetype bytes, ASCII white space trimmed from both ends, equal to application/x-krita. */
static bool kra_mime_matches(const uint8_t *p, size_t n)
{
    size_t start = 0;
    size_t end = n;
    while (start < end && kra_ascii_ws(p[start])) {
        start++;
    }
    while (end > start && kra_ascii_ws(p[end - 1])) {
        end--;
    }
    return end - start == KRA_MIME_LEN && memcmp(p + start, KRA_MIME, KRA_MIME_LEN) == 0;
}

/* Wrap a sub-error (ZIP or codec) as IoError::Kra: PC_ERR_FORMAT "Krita: <prefix><sub message>".
 * Out of memory is left as it is (never prefixed). */
static pc_status kra_wrap(pc_status st, pc_error *err, const char *prefix)
{
    char sub[PC_ERROR_MSG_MAX];
    if (st == PC_ERR_NOMEM) {
        return st;
    }
    sub[0] = '\0';
    if (err != NULL) {
        snprintf(sub, sizeof sub, "%s", err->msg);
    }
    return pc_io_kra_error(err, "%s%s", prefix, sub);
}

bool pc_io_is_kra(const uint8_t *bytes, size_t len)
{
    size_t mime_len = 0;
    const uint8_t *mime = pc_io_zip_mimetype(bytes, len, &mime_len);
    return mime != NULL && kra_mime_matches(mime, mime_len);
}

pc_status pc_io_kra_import(const char *name, const uint8_t *bytes, size_t len, const pc_interrupt *ctl,
                           pc_io_import_result *out, pc_error *err)
{
    pc_zip_reader zip;
    pc_buf mime = PC_BUF_INIT;
    pc_buf merged = PC_BUF_INIT;
    pc_cimage img;
    pc_io_import_result r;
    pc_cdecode_opts opts;
    pc_cformat fmt;
    bool zip_open = false;
    bool img_ok = false;
    pc_status st;

    st = pc_interrupt_check(ctl, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_zip_reader_open(&zip, bytes, len, err);
    if (st != PC_OK) {
        return kra_wrap(st, err, "");
    }
    zip_open = true;

    st = pc_zip_read_by_name(&zip, "mimetype", 256, &mime, err);
    if (st != PC_OK) {
        st = kra_wrap(st, err, "");
        goto cleanup;
    }
    if (!kra_mime_matches(mime.data, mime.len)) {
        st = pc_io_kra_error(err, "the archive's mimetype is not application/x-krita");
        goto cleanup;
    }
    pc_buf_fini(&mime);
    pc_interrupt_progress(ctl, 0.10f);

    st = pc_zip_read_by_name(&zip, "mergedimage.png", KRA_MAX_ENTRY, &merged, err);
    if (st != PC_OK) {
        st = kra_wrap(st, err, "mergedimage.png: ");
        goto cleanup;
    }
    if (!pc_cformat_detect(merged.data, merged.len, &fmt) || fmt != PC_CF_PNG) {
        st = pc_io_kra_error(err, "mergedimage.png is not a PNG image");
        goto cleanup;
    }
    st = pc_interrupt_check(ctl, err);
    if (st != PC_OK) {
        goto cleanup;
    }

    opts = pc_cdecode_opts_default();
    opts.limits.max_width = PC_IO_ORA_MAX_SIDE;
    opts.limits.max_height = PC_IO_ORA_MAX_SIDE;
    opts.limits.max_alloc = KRA_MAX_PIXELS_BYTES;
    st = pc_cdecode_with(merged.data, merged.len, &opts, &img, err);
    if (st != PC_OK) {
        st = kra_wrap(st, err, "mergedimage.png: ");
        goto cleanup;
    }
    img_ok = true;
    pc_buf_fini(&merged);
    st = pc_interrupt_check(ctl, err);
    if (st != PC_OK) {
        goto cleanup;
    }

    st = pc_io_image_to_document(name, &img, &r, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    pc_cimage_fini(&img);
    img_ok = false;

    /* The flattened preview is editable, but it is not Krita's layered document; say so and
     * forbid a flat save from overwriting the .kra source. */
    r.source_read_only = true;
    st = pc_io_warn(&r.warnings, err,
                    "Krita layers, masks and editable document structure were not imported; "
                    "only mergedimage.png is available");
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        goto cleanup;
    }
    *out = r;
    st = PC_OK;

cleanup:
    if (img_ok) {
        pc_cimage_fini(&img);
    }
    pc_buf_fini(&merged);
    pc_buf_fini(&mime);
    if (zip_open) {
        pc_zip_reader_close(&zip);
    }
    return st;
}
