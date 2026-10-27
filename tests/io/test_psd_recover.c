/* Graceful recovery of a PSD whose trailing merged image is damaged (resync wave K1; mirrors the
 * reference crates/psd from_bytes_and_merged_recovering + crates/io can_recover_layers, #6152).
 *
 * A layered colour-mode PSD whose merged composite is truncated still opens: the intact layer
 * stack is read and a warning records the lost merged image. A flattened file (whose merged image
 * *is* the document) and a decode-bomb merged image (over the limit) must still be rejected. The
 * strict pc_psd_file_parse never recovers; only pc_psd_file_parse_recovering does. */
#include "harness.h"
#include "common.h"
#include "../psd/testgen.h"

#include <zlib.h>

#include "io/psd_import.h"
#include "psd/file.h"

static pc_psd_bytes file_bytes(const pc_psd_file *f)
{
    pc_psd_bytes b = {NULL, 0};
    must(pc_psd_file_to_bytes(f, &b, NULL));
    return b;
}

/* A tiny layered RGB PSD (4x3, RGBA, RAW merged = 2 + 4*3*4 = 50 bytes) with the last `cut` bytes
 * of its trailing merged image removed. `cut` leaves the compression field and part of the data. */
static pc_psd_bytes truncated_layered(size_t cut)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes full = file_bytes(&f);
    pc_psd_bytes out = {NULL, 0};
    pc_psd_file_fini(&f);
    CHECK(full.len > cut + 32);
    must(pc_psd_bytes_set(&out, full.data, full.len - cut, NULL));
    pc_psd_bytes_fini(&full);
    return out;
}

/* A truncated merged image on a layered file opens the layers with a warning, not an error. */
static void test_truncated_merged_opens_layers(void)
{
    pc_psd_bytes b = truncated_layered(24);
    pc_psd_file strict;
    pc_psd_file rec;
    pc_error e;
    pc_io_import_result r;

    /* The strict parser (psd-info) still rejects the file byte-for-byte. */
    CHECK(pc_psd_file_parse(b.data, b.len, &strict, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "unexpected end of data");

    /* The recovering parser accepts it, flags the damage and keeps the parse error. */
    must(pc_psd_file_parse_recovering(b.data, b.len, pc_io_psd_can_recover_layers, &rec, &e));
    CHECK(rec.image_damaged);
    CHECK_CONTAINS(rec.image_error.msg, "unexpected end of data");
    CHECK(rec.has_layer_info);
    pc_psd_file_fini(&rec);

    /* The importer opens the layers and records the loss (wording matches the reference). */
    must(pc_io_import("truncated.psd", b.data, b.len, &r, &e));
    CHECK(r.document.nlayers > 0);
    CHECK(pc_io_warnings_any(&r.warnings, "merged image could not be decoded; opened the available layers instead"));
    pc_io_import_result_fini(&r);
    pc_psd_bytes_fini(&b);
}

/* A flattened file (no layers) cannot discard its merged image: truncation stays fatal. */
static void test_truncated_flattened_errors(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 8, 8);
    pc_psd_bytes full = file_bytes(&f);
    pc_psd_bytes b = {NULL, 0};
    pc_psd_file rec;
    pc_io_import_result r;
    pc_error e;
    pc_psd_file_fini(&f);
    CHECK(full.len > 40);
    must(pc_psd_bytes_set(&b, full.data, full.len - 40, NULL));
    pc_psd_bytes_fini(&full);

    CHECK(!pc_io_psd_can_recover_layers(&(pc_psd_file){0})); /* defensive: zeroed file never recovers */
    CHECK(pc_psd_file_parse_recovering(b.data, b.len, pc_io_psd_can_recover_layers, &rec, &e) != PC_OK);
    CHECK(pc_io_import("flat.psd", b.data, b.len, &r, &e) != PC_OK);
    pc_psd_bytes_fini(&b);
}

/* A merged image that decodes past the size limit (a decode bomb) is never recovered, even on a
 * layered file whose layers could otherwise be opened. */
static void test_over_limit_merged_errors(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    uLongf clen;
    unsigned char *comp;
    unsigned char *zeros;
    const size_t raw = (size_t)2 << 20; /* 2 MiB inflates well past the ~48 B + 1 MiB allowance */
    pc_psd_bytes b;
    pc_psd_file rec;
    pc_io_import_result r;
    pc_error e;
    pc_status st;

    zeros = calloc(1, raw);
    CHECK(zeros != NULL);
    clen = compressBound((uLong)raw);
    comp = malloc(clen);
    CHECK(comp != NULL);
    CHECK(compress2(comp, &clen, zeros, (uLong)raw, 6) == Z_OK);
    free(zeros);

    /* Replace the merged image with a ZIP stream that inflates to 2 MiB. */
    f.image_compression = PC_PSD_ZIP;
    must(pc_psd_bytes_set(&f.image_data, comp, (size_t)clen, NULL));
    free(comp);
    b = file_bytes(&f);
    CHECK(pc_io_psd_can_recover_layers(&f)); /* it is a recoverable layout... */
    pc_psd_file_fini(&f);

    /* ...yet a limit error (PC_ERR_LIMIT) stays fatal rather than being swallowed. */
    st = pc_psd_file_parse_recovering(b.data, b.len, pc_io_psd_can_recover_layers, &rec, &e);
    CHECK(st == PC_ERR_LIMIT);
    CHECK(pc_io_import("bomb.psd", b.data, b.len, &r, &e) != PC_OK);
    pc_psd_bytes_fini(&b);
}

/* Empty and absurd inputs through the recovering entry never crash (Rule 9). */
static void test_recovering_never_crashes(void)
{
    static const uint8_t junk[] = {0x38, 0x42, 0x50, 0x53, 1, 2, 3};
    pc_psd_file out;
    pc_error e;
    CHECK(pc_psd_file_parse_recovering(NULL, 0, pc_io_psd_can_recover_layers, &out, &e) != PC_OK);
    CHECK(pc_psd_file_parse_recovering((const uint8_t *)"", 0, pc_io_psd_can_recover_layers, &out, &e) != PC_OK);
    CHECK(pc_psd_file_parse_recovering(junk, sizeof junk, pc_io_psd_can_recover_layers, &out, &e) != PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_truncated_merged_opens_layers);
    RUN(test_truncated_flattened_errors);
    RUN(test_over_limit_merged_errors);
    RUN(test_recovering_never_crashes);
    return pc_t_finish();
}
