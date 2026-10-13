/* Ports the upstream unit tests added to crates/psd/src/{file,tagged,compression,pixels,
 * descriptor,resources,path}.rs and tests/tree.rs during the Phase 5R resync (global block
 * padding, ZIP trailer checks and the decode budget, white-matte composites, string ids,
 * preallocation from untrusted counts, Photoshop 2026 version info, capped layer trees). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/bytes.h"
#include "harness.h"
#include "psd/compression.h"
#include "psd/descriptor.h"
#include "psd/file.h"
#include "psd/internal.h"
#include "psd/layer.h"
#include "psd/misc.h"
#include "psd/pixels.h"
#include "psd/resources.h"
#include "psd/tagged.h"
#include "testgen.h"

static pc_psd_bytes to_bytes(const pc_psd_file *f)
{
    pc_psd_bytes b = {NULL, 0};
    CHECK_EQ_INT(pc_psd_file_to_bytes(f, &b, NULL), PC_OK);
    return b;
}

static size_t find(const pc_psd_bytes *b, const char *needle)
{
    size_t n = strlen(needle);
    size_t i;
    for (i = 0; i + n <= b->len; i++) {
        if (memcmp(b->data + i, needle, n) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void replace_globals(pc_psd_file *f)
{
    pc_psd_blocks_free(f->global_blocks, f->nglobal);
    f->global_blocks = NULL;
    f->nglobal = 0;
}

/* file.rs global_blocks_default_to_four_byte_padding */
static void test_global_blocks_default_to_four_byte_padding(void)
{
    static const uint8_t eight[4] = {8, 8, 8, 8};
    int v;
    size_t len;
    for (v = 1; v <= 2; v++) {
        for (len = 1; len < 8; len++) {
            pc_psd_file f = tg_small((pc_psd_version)v, PC_PSD_RAW);
            pc_psd_file back;
            uint8_t sevens[8];
            pc_psd_bytes b;
            size_t start;
            size_t next;
            size_t padded = (len + 3) / 4 * 4;
            size_t k;
            memset(sevens, 7, sizeof sevens);
            replace_globals(&f);
            tg_global(&f, "TEST", sevens, len);
            tg_global(&f, "NEXT", eight, 4);
            b = to_bytes(&f);
            start = find(&b, "8BIMTEST") + 12;
            next = find(&b, "8BIMNEXT");
            CHECK(next != SIZE_MAX && start != SIZE_MAX);
            CHECK_EQ_UINT(next - start, padded);
            for (k = 0; k < padded && start + k < b.len; k++) {
                CHECK_EQ_UINT(b.data[start + k], k < len ? 7u : 0u);
            }
            CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &back, NULL), PC_OK);
            CHECK(pc_psd_file_eq(&back, &f));
            pc_psd_file_fini(&back);
            pc_psd_bytes_fini(&b);
            pc_psd_file_fini(&f);
        }
    }
}

/* file.rs global_layers_preserve_order_padding_and_first_candidate */
static void test_global_layers_preserve_order_padding_and_first_candidate(void)
{
    static const uint8_t one[1] = {1};
    static const uint8_t two[2] = {2, 3};
    static const uint8_t pad[2] = {7, 8};
    static const uint8_t ff[1] = {255};
    int v;
    int depth;
    for (v = 1; v <= 2; v++) {
        for (depth = 16; depth <= 32; depth += 16) {
            const char *key = depth == 16 ? "Lr16" : "Lr32";
            pc_psd_file f = tg_layered((pc_psd_version)v, PC_PSD_MODE_RGB, (uint16_t)depth, PC_PSD_RAW);
            pc_psd_file parsed;
            pc_psd_bytes b;
            pc_psd_bytes again = {NULL, 0};
            pc_psd_bytes dup = {NULL, 0};
            pc_psd_block *moved;
            pc_error why;
            replace_globals(&f);
            tg_global(&f, "BEFR", one, 1);
            tg_global(&f, "AFTR", two, 2);
            f.placement.where = PC_PSD_LI_GLOBAL_BLOCK;
            f.placement.index = 1;
            memcpy(f.placement.signature, "8B64", 4);
            memcpy(f.placement.key, key, 4);
            pc_psd_opt_bytes_fini(&f.placement.padding);
            f.placement.padding.has = true;
            (void)pc_psd_bytes_set(&f.placement.padding.v, pad, 2, NULL);
            pc_psd_bytes_fini(&f.layer_mask_trailing);
            (void)pc_psd_bytes_set(&f.layer_mask_trailing, "\x09\x09\x09\x09", 4, NULL);
            b = to_bytes(&f);
            CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &parsed, NULL), PC_OK);
            CHECK(pc_psd_file_eq(&parsed, &f));
            CHECK_EQ_INT(pc_psd_file_to_bytes(&parsed, &again, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&again, &b));
            CHECK(pc_psd_file_unreadable_layer_block(&parsed, NULL) == NULL);
            pc_psd_file_fini(&parsed);
            pc_psd_bytes_fini(&again);
            pc_psd_bytes_fini(&b);
            /* A second, duplicate layer block stays a plain global block. */
            CHECK_EQ_INT(pc_psd_layer_info_to_bytes(&f.layer_info, f.header.version, &dup, NULL), PC_OK);
            tg_global(&f, key, dup.data, dup.len);
            pc_psd_bytes_fini(&dup);
            b = to_bytes(&f);
            CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &parsed, NULL), PC_OK);
            CHECK(pc_psd_file_eq(&parsed, &f));
            pc_psd_file_fini(&parsed);
            pc_psd_bytes_fini(&b);
            /* An unreadable first candidate: the file reads as flattened, verbatim. */
            tg_global(&f, key, ff, 1);
            moved = &f.global_blocks[f.nglobal - 1];
            {
                pc_psd_block tmp = *moved;
                memmove(&f.global_blocks[1], &f.global_blocks[0], (f.nglobal - 1) * sizeof tmp);
                f.global_blocks[0] = tmp;
            }
            /* The layer info is now after index 0: keep it in place (BEFR, it, AFTR, dup). */
            f.placement.index = 2;
            b = to_bytes(&f);
            CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &parsed, NULL), PC_OK);
            CHECK(!parsed.has_layer_info);
            CHECK_EQ_UINT(parsed.nglobal, 5);
            CHECK(pc_psd_file_unreadable_layer_block(&parsed, &why) == &parsed.global_blocks[0]);
            CHECK_EQ_INT(pc_psd_file_to_bytes(&parsed, &again, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&again, &b));
            pc_psd_bytes_fini(&again);
            pc_psd_file_fini(&parsed);
            pc_psd_bytes_fini(&b);
            pc_psd_file_fini(&f);
        }
    }
}

/* file.rs over_budget_zip_merged_image_opens_without_inflating (#2926) */
static void test_over_budget_zip_merged_image_opens_without_inflating(void)
{
    pc_psd_file f = tg_small(PC_PSD_V2, PC_PSD_ZIP);
    pc_psd_file p;
    pc_psd_bytes b = to_bytes(&f);
    pc_psd_bytes d = {NULL, 0};
    size_t nl = 0;
    pc_store_be32(b.data + 14, 300000);
    pc_store_be32(b.data + 18, 300000);
    CHECK_EQ_INT(pc_psd_file_parse(b.data, b.len, &p, NULL), PC_OK);
    (void)pc_psd_file_layers(&p, &nl);
    CHECK(nl > 0);
    CHECK_EQ_INT(pc_psd_file_decode_merged(&p, &d, NULL), PC_ERR_LIMIT);
    pc_psd_file_fini(&p);
    pc_psd_bytes_fini(&b);
    pc_psd_file_fini(&f);
}

/* compression.rs checked_zip_validates_trailer_size_and_padding_budget */
static void test_checked_zip_validates_trailer_size_and_padding_budget(void)
{
    static const uint8_t px[3] = {1, 2, 3};
    static const uint8_t padded4[4] = {1, 2, 3, 0};
    pc_psd_layout l = {1, 3, 1, 8, PC_PSD_V1};
    uint16_t comps[2] = {PC_PSD_ZIP, PC_PSD_ZIP_PREDICT};
    pc_psd_bytes out = {NULL, 0};
    pc_psd_bytes z = {NULL, 0};
    uint8_t *bomb_src;
    size_t bomb_len = ((size_t)1 << 20) + 4;
    int c;
    size_t cut;
    for (c = 0; c < 2; c++) {
        pc_psd_bytes data = {NULL, 0};
        CHECK_EQ_INT(pc_psd_encode_planes(comps[c], px, 3, &l, &data, NULL), PC_OK);
        for (cut = 0; cut < data.len; cut++) {
            CHECK(pc_psd_decode_planes(comps[c], data.data, cut, &l, &out, NULL) != PC_OK);
            CHECK(pc_psd_validate_planes(comps[c], data.data, cut, &l, NULL) != PC_OK);
        }
        data.data[data.len - 1] ^= 1;
        CHECK(pc_psd_decode_planes(comps[c], data.data, data.len, &l, &out, NULL) != PC_OK);
        CHECK(pc_psd_validate_planes(comps[c], data.data, data.len, &l, NULL) != PC_OK);
        pc_psd_bytes_fini(&data);
    }
    CHECK_EQ_INT(pc_psd_zip_compress(padded4, 4, &z, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_decode_planes(PC_PSD_ZIP, z.data, z.len, &l, &out, NULL), PC_OK);
    CHECK(out.len == 3 && memcmp(out.data, px, 3) == 0);
    pc_psd_bytes_fini(&out);
    pc_psd_bytes_fini(&z);
    bomb_src = calloc(bomb_len, 1);
    if (bomb_src != NULL) {
        pc_error e;
        CHECK_EQ_INT(pc_psd_zip_compress(bomb_src, bomb_len, &z, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_decode_planes(PC_PSD_ZIP, z.data, z.len, &l, &out, &e), PC_ERR_LIMIT);
        CHECK_CONTAINS(e.msg, "padding allowance");
        CHECK_EQ_INT(pc_psd_validate_planes(PC_PSD_ZIP, z.data, z.len, &l, NULL), PC_ERR_LIMIT);
        pc_psd_bytes_fini(&z);
        free(bomb_src);
    }
}

/* compression.rs zip_over_budget_is_not_decompressed_at_parse (#2926) */
static void test_zip_over_budget_is_not_decompressed_at_parse(void)
{
    static const uint8_t garbage[7] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03};
    pc_psd_layout big = {4, 300000, 300000, 8, PC_PSD_V2};
    pc_psd_layout small = {1, 4, 4, 8, PC_PSD_V1};
    pc_psd_bytes out = {NULL, 0};
    uint64_t total = 0;
    CHECK_EQ_INT(pc_psd_layout_total_bytes(&big, &total, NULL), PC_OK);
    CHECK(total > PC_PSD_MAX_DECODED_BYTES);
    CHECK_EQ_INT(pc_psd_validate_planes(PC_PSD_ZIP, garbage, 7, &big, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_validate_planes(PC_PSD_ZIP_PREDICT, garbage, 7, &big, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_decode_planes(PC_PSD_ZIP, garbage, 7, &big, &out, NULL), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_psd_decode_planes(PC_PSD_ZIP_PREDICT, garbage, 7, &big, &out, NULL), PC_ERR_LIMIT);
    /* Within the budget the stream is still checked. */
    CHECK(pc_psd_validate_planes(PC_PSD_ZIP, garbage, 7, &small, NULL) != PC_OK);
}

/* pixels.rs float_composite_removes_white_matte_without_changing_native_data */
static void test_float_composite_removes_white_matte(void)
{
    static const float native[4] = {0.75f, 0.875f, 1.0f, 0.25f};
    uint8_t raw[16];
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 32, PC_PSD_RAW, 1, 1);
    pc_psd_image8 img;
    pc_psd_bytes d = {NULL, 0};
    size_t i;
    static const uint8_t want[4] = {0, 128, 255, 64};
    for (i = 0; i < 4; i++) {
        uint32_t bits;
        memcpy(&bits, &native[i], 4);
        pc_store_be32(raw + 4 * i, bits);
    }
    f.header.channels = 4;
    pc_psd_bytes_fini(&f.image_data);
    CHECK_EQ_INT(pc_psd_image_encode(PC_PSD_RAW, raw, 16, &f.header, &f.image_data, NULL), PC_OK);
    f.image_compression = PC_PSD_RAW;
    CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
    CHECK(img.data.len == 4 && memcmp(img.data.data, want, 4) == 0);
    pc_psd_image8_fini(&img);
    CHECK_EQ_INT(pc_psd_file_decode_merged(&f, &d, NULL), PC_OK);
    CHECK(d.len == 16 && memcmp(d.data, raw, 16) == 0);
    pc_psd_bytes_fini(&d);
    /* 16-bit and 8-bit composites remove the matte too (the builder stores against white). */
    pc_psd_file_fini(&f);
}

/* pixels.rs oversized_layer_preview_rejects_missing_channels_before_allocating */
static void test_oversized_layer_preview_rejects_before_allocating(void)
{
    pc_psd_file f = tg_small(PC_PSD_V2, PC_PSD_RAW);
    pc_psd_layer l;
    pc_psd_image8 img;
    size_t nl = 0;
    pc_psd_layer_init(&l);
    l.rect.top = 0;
    l.rect.left = 0;
    l.rect.bottom = 300000;
    l.rect.right = 300000;
    CHECK_EQ_INT(pc_psd_layer_info_push(&f.layer_info, &l, NULL), PC_OK);
    (void)pc_psd_file_layers(&f, &nl);
    CHECK_EQ_INT(pc_psd_layer_rgba8(&f, nl - 1, &img, NULL), PC_ERR_LIMIT);
    pc_psd_file_fini(&f);
}

/* pixels.rs plane_conversion_reports_absurd_sizes */
static void test_plane_conversion_reports_absurd_sizes(void)
{
    static const uint8_t z[1] = {0};
    pc_psd_bytes out = {NULL, 0};
    CHECK_EQ_INT(pc_psd_unpack_bits(z, 1, SIZE_MAX / 2, 4, &out, NULL), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_psd_plane_to_u8(z, 1, 1, SIZE_MAX / 2, 4, &out, NULL), PC_ERR_LIMIT);
    CHECK_EQ_INT(pc_psd_unpack_bits(z, 1, (size_t)1 << 40, (size_t)1 << 20, &out, NULL), PC_ERR_LIMIT);
}

/* pixels.rs composite_rgba8_checks_the_output_budget_first */
static void test_composite_rgba8_checks_the_output_budget_first(void)
{
    pc_psd_file f = tg_small(PC_PSD_V2, PC_PSD_ZIP);
    pc_psd_image8 img;
    pc_error e;
    f.header.color_mode = PC_PSD_MODE_BITMAP;
    f.header.depth = 1;
    f.header.channels = 1;
    f.header.width = 60000;
    f.header.height = 60000;
    CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, &e), PC_ERR_LIMIT);
    CHECK_EQ_STR(e.msg, "limit exceeded: RGBA output exceeds MAX_DECODED_BYTES");
    pc_psd_file_fini(&f);
}

/* pixels.rs rgb_interleave_checks_planes_and_preserves_alpha, through the public composite:
 * an RGB merged image whose planes are short is an error, not a read past the end. */
static void test_rgb_composite_checks_planes(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 2, 1);
    pc_psd_image8 img;
    CHECK_EQ_INT(pc_psd_composite_rgba8(&f, &img, NULL), PC_OK);
    CHECK_EQ_UINT(img.data.len, 8);
    if (img.data.len == 8) {
        CHECK(img.data.data[3] == 255 && img.data.data[7] == 255);
    }
    pc_psd_image8_fini(&img);
    /* Four channels declared on three planes of data: the alpha plane is missing. */
    f.header.channels = 4;
    CHECK(pc_psd_composite_rgba8(&f, &img, NULL) != PC_OK);
    pc_psd_file_fini(&f);
}

/* descriptor.rs declared_counts_do_not_drive_preallocation */
static void test_declared_counts_do_not_drive_preallocation(void)
{
    const uint32_t n = 1000000;
    pc_buf b = PC_BUF_INIT;
    pc_psd_desc d;
    size_t i;
    uint8_t hdr[4];
    /* A descriptor: version 16, empty name, class "null", one item "list" -> VlLs of n bools. */
    static const uint8_t head[] = {0, 0, 0, 16, 0, 0, 0, 0, 0,   0,   0,   0,   'n', 'u', 'l', 'l',
                                   0, 0, 0, 1,  0, 0, 0, 0, 'L', 'i', 's', 't', 'V', 'l', 'L', 's'};
    (void)pc_buf_append(&b, head, sizeof head, NULL);
    pc_store_be32(hdr, n);
    (void)pc_buf_append(&b, hdr, 4, NULL);
    for (i = 0; i < n; i++) {
        (void)pc_buf_append(&b, "bool\x01", 5, NULL);
    }
    CHECK_EQ_INT(pc_psd_vdesc_parse(b.data, b.len, &d, NULL), PC_OK);
    {
        const pc_psd_value *v = pc_psd_desc_get(&d, "List");
        CHECK(v != NULL && v->kind == PC_PSD_V_LIST && v->nlist == n);
    }
    pc_psd_desc_fini(&d);
    /* A huge declared count with too little data is still refused before allocating. */
    pc_store_be32(b.data + sizeof head, 0xffffffffu);
    CHECK(pc_psd_vdesc_parse(b.data, b.len, &d, NULL) != PC_OK);
    pc_buf_fini(&b);
}

/* descriptor.rs photoshop_string_ids_are_written_as_strings */
static void test_photoshop_string_ids_are_written_as_strings(void)
{
    static const char *const ids[3] = {"warp", "list", "time"};
    size_t k;
    for (k = 0; k < 3; k++) {
        const char *name = ids[k];
        pc_psd_id id;
        pc_psd_desc d;
        pc_psd_desc back;
        pc_psd_value v = pc_psd_value_int(1);
        pc_psd_bytes b = {NULL, 0};
        pc_psd_bytes again = {NULL, 0};
        uint8_t str_id[8] = {0, 0, 0, 4, 0, 0, 0, 0};
        uint8_t code_id[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(str_id + 4, name, 4);
        memcpy(code_id + 4, name, 4);
        CHECK_EQ_INT(pc_psd_id_make(name, &id, NULL), PC_OK);
        CHECK(!id.is_code);
        pc_psd_id_fini(&id);
        CHECK_EQ_INT(pc_psd_desc_init(&d, name, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_desc_add(&d, name, &v, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_desc_to_bytes(&d, &b, NULL), PC_OK);
        /* Name (empty unicode string), then the class id; then the item count and the key. */
        CHECK(b.len >= 24 && memcmp(b.data + 4, str_id, 8) == 0);
        CHECK(b.len >= 24 && memcmp(b.data + 16, str_id, 8) == 0);
        CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &back, NULL), PC_OK);
        CHECK(pc_psd_desc_eq(&back, &d));
        CHECK(pc_psd_id_is(&back.class_id, name) && pc_psd_desc_get(&back, name) != NULL);
        pc_psd_desc_fini(&back);
        /* A file that wrote the code form keeps it, and lookups still find the key. */
        memcpy(b.data + 16, code_id, 8);
        CHECK_EQ_INT(pc_psd_desc_parse(b.data, b.len, &back, NULL), PC_OK);
        {
            const pc_psd_value *got = pc_psd_desc_get(&back, name);
            CHECK(got != NULL && got->kind == PC_PSD_V_INTEGER && got->i == 1);
        }
        CHECK_EQ_INT(pc_psd_desc_to_bytes(&back, &again, NULL), PC_OK);
        CHECK(pc_psd_bytes_eq(&again, &b));
        pc_psd_desc_fini(&back);
        pc_psd_desc_fini(&d);
        pc_psd_bytes_fini(&b);
        pc_psd_bytes_fini(&again);
    }
    {
        pc_psd_id id;
        CHECK_EQ_INT(pc_psd_id_make("view", &id, NULL), PC_OK);
        CHECK(id.is_code);
        pc_psd_id_fini(&id);
        CHECK_EQ_INT(pc_psd_id_make("Txt ", &id, NULL), PC_OK);
        CHECK(id.is_code);
        pc_psd_id_fini(&id);
    }
}

/* resources.rs version_info_matches_photoshop_bytes */
static void test_version_info_matches_photoshop_bytes(void)
{
    static const char *const names[2] = {"Adobe Photoshop", "Adobe Photoshop 2026"};
    pc_buf want = PC_BUF_INIT;
    pc_psd_resource r;
    size_t k;
    size_t i;
    (void)pc_buf_append(&want, "\0\0\0\1\1", 5, NULL);
    for (k = 0; k < 2; k++) {
        uint8_t n4[4];
        pc_store_be32(n4, (uint32_t)strlen(names[k]));
        (void)pc_buf_append(&want, n4, 4, NULL);
        for (i = 0; names[k][i] != '\0'; i++) {
            uint8_t u[2] = {0, (uint8_t)names[k][i]};
            (void)pc_buf_append(&want, u, 2, NULL);
        }
    }
    (void)pc_buf_append(&want, "\0\0\0\1", 4, NULL);
    CHECK_EQ_INT(pc_psd_version_info_resource(true, &r, NULL), PC_OK);
    CHECK_EQ_UINT(r.id, 1057);
    CHECK(r.data.len == want.len && memcmp(r.data.data, want.data, want.len) == 0);
    pc_psd_bytes_fini(&r.data);
    pc_psd_bytes_fini(&r.name);
    pc_buf_fini(&want);
}

/* path.rs subpaths_does_not_preallocate_from_untrusted_counts: 100,000 closed-subpath length
 * records claiming 65535 knots each parse into 100,000 empty subpaths. */
static void test_subpaths_from_untrusted_counts(void)
{
    const size_t n = 100000;
    uint8_t *bytes = calloc(n, 26);
    pc_psd_path p;
    pc_psd_subpath *subs = NULL;
    size_t ns = 0;
    size_t i;
    if (bytes == NULL) {
        CHECK(bytes != NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        bytes[i * 26 + 1] = 0; /* selector 0: closed subpath length record */
        bytes[i * 26 + 2] = 0xff;
        bytes[i * 26 + 3] = 0xff;
    }
    CHECK_EQ_INT(pc_psd_path_parse(bytes, n * 26, &p, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_path_subpaths(&p, &subs, &ns, NULL), PC_OK);
    CHECK_EQ_UINT(ns, n);
    for (i = 0; i < ns; i++) {
        if (subs[i].n != 0) {
            CHECK_EQ_UINT(subs[i].n, 0);
            break;
        }
    }
    pc_psd_subpaths_free(subs, ns);
    pc_psd_path_fini(&p);
    free(bytes);
}

/* tests/tree.rs layer_tree_max_depth_drops_deeper_children (#2940) */
static pc_psd_layer trec(const char *name, int kind)
{
    pc_psd_layer l;
    pc_psd_block b;
    pc_psd_layer_init(&l);
    (void)pc_psd_bytes_set(&l.name, name, strlen(name), NULL);
    if (kind >= 0) {
        (void)pc_psd_block_make_section((uint32_t)kind, (const uint8_t *)"pass", NULL, &b, NULL);
        (void)pc_psd_layer_push_block(&l, &b, NULL);
    }
    return l;
}

static void tree_names(const pc_psd_file *f, const pc_psd_node *n, size_t count, pc_buf *o)
{
    size_t i;
    for (i = 0; i < count; i++) {
        char *nm = NULL;
        if (i > 0) {
            (void)pc_buf_append_byte(o, ',', NULL);
        }
        (void)pc_psd_layer_name(&f->layer_info.layers[n[i].index], &nm, NULL);
        (void)pc_buf_append_str(o, nm != NULL ? nm : "", NULL);
        free(nm);
        if (n[i].is_group) {
            (void)pc_buf_append_byte(o, '[', NULL);
            tree_names(f, n[i].children, n[i].n, o);
            (void)pc_buf_append_byte(o, ']', NULL);
        }
    }
}

static void check_capped(const pc_psd_file *f, size_t max, const char *want)
{
    pc_psd_node *t = NULL;
    size_t nt = 0;
    pc_buf o = PC_BUF_INIT;
    CHECK_EQ_INT(pc_psd_layer_tree_max_depth(f, max, &t, &nt, NULL), PC_OK);
    tree_names(f, t, nt, &o);
    CHECK_EQ_STR(pc_buf_cstr(&o), want);
    pc_buf_fini(&o);
    pc_psd_nodes_free(t, nt);
}

static void test_layer_tree_max_depth_drops_deeper_children(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 1, 1);
    pc_psd_layer recs[8];
    pc_psd_node *t = NULL;
    size_t nt = 0;
    pc_buf o = PC_BUF_INIT;
    size_t i;
    recs[0] = trec("</Layer group>", PC_PSD_SECTION_DIVIDER);
    recs[1] = trec("</Layer group>", PC_PSD_SECTION_DIVIDER);
    recs[2] = trec("</Layer group>", PC_PSD_SECTION_DIVIDER);
    recs[3] = trec("x", -1);
    recs[4] = trec("g2", PC_PSD_SECTION_OPEN);
    recs[5] = trec("g1", PC_PSD_SECTION_OPEN);
    recs[6] = trec("g0", PC_PSD_SECTION_OPEN);
    recs[7] = trec("top", -1);
    f.has_layer_info = true;
    for (i = 0; i < 8; i++) {
        (void)pc_psd_layer_info_push(&f.layer_info, &recs[i], NULL);
    }
    CHECK_EQ_INT(pc_psd_layer_tree(&f, &t, &nt, NULL), PC_OK);
    tree_names(&f, t, nt, &o);
    CHECK_EQ_STR(pc_buf_cstr(&o), "g0[g1[g2[x]]],top");
    pc_buf_fini(&o);
    pc_psd_nodes_free(t, nt);
    check_capped(&f, SIZE_MAX, "g0[g1[g2[x]]],top");
    check_capped(&f, 2, "g0[g1[g2[]]],top");
    check_capped(&f, 1, "g0[g1[]],top");
    check_capped(&f, 0, "g0[],top");
    pc_psd_file_fini(&f);
}

/* tagged.rs: opaque trailing bytes can follow canonical zero padding without another signature. */
static void test_trailing_bytes_after_canonical_padding(void)
{
    static const uint8_t data[3] = {1, 2, 3};
    static const uint8_t junk[5] = {0, 0xaa, 0xbb, 0xcc, 0xdd};
    pc_psd_block b;
    psd_w w;
    pc_psd_bytes out = {NULL, 0};
    psd_rd r;
    pc_psd_block *back = NULL;
    size_t nb = 0;
    pc_psd_bytes trailing = {NULL, 0};
    CHECK_EQ_INT(pc_psd_block_init(&b, "zOdd", data, 3, NULL), PC_OK);
    psd_w_init(&w, NULL);
    psd_write_block(&w, &b, PC_PSD_V1, 4);
    psd_put(&w, junk + 1, 4);
    CHECK_EQ_INT(psd_w_finish(&w, &out), PC_OK);
    r = psd_rd_make(out.data, out.len);
    CHECK_EQ_INT(psd_read_blocks(&r, PC_PSD_V1, 4, &back, &nb, &trailing, NULL), PC_OK);
    CHECK_EQ_UINT(nb, 1);
    if (nb == 1) {
        CHECK(!back[0].padding.has);
    }
    CHECK(trailing.len == 4 && memcmp(trailing.data, junk + 1, 4) == 0);
    pc_psd_blocks_free(back, nb);
    pc_psd_bytes_fini(&trailing);
    pc_psd_bytes_fini(&out);
    pc_psd_block_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_global_blocks_default_to_four_byte_padding);
    RUN(test_global_layers_preserve_order_padding_and_first_candidate);
    RUN(test_over_budget_zip_merged_image_opens_without_inflating);
    RUN(test_checked_zip_validates_trailer_size_and_padding_budget);
    RUN(test_zip_over_budget_is_not_decompressed_at_parse);
    RUN(test_float_composite_removes_white_matte);
    RUN(test_oversized_layer_preview_rejects_before_allocating);
    RUN(test_plane_conversion_reports_absurd_sizes);
    RUN(test_composite_rgba8_checks_the_output_budget_first);
    RUN(test_rgb_composite_checks_planes);
    RUN(test_declared_counts_do_not_drive_preallocation);
    RUN(test_photoshop_string_ids_are_written_as_strings);
    RUN(test_version_info_matches_photoshop_bytes);
    RUN(test_subpaths_from_untrusted_counts);
    RUN(test_layer_tree_max_depth_drops_deeper_children);
    RUN(test_trailing_bytes_after_canonical_padding);
    return pc_t_finish();
}
