/* Unit tests of crates/io/src: pixels.rs (5), channel_map.rs (2), linked.rs (3), flat.rs's
 * cmyk_band_tests (2) and raw.rs (1), plus edge cases of the C API. */
#include "harness.h"

#include "common.h"

/* ---- pixels.rs ---- */

static void invert_sample(uint8_t *b, pc_sample_type s)
{
    if (s == PC_SAMPLE_TYPE_U8) {
        b[0] = (uint8_t)(255 - b[0]);
    } else if (s == PC_SAMPLE_TYPE_U16) {
        uint16_t v;
        memcpy(&v, b, 2);
        v = (uint16_t)(65535 - v);
        memcpy(b, &v, 2);
    } else {
        float f;
        memcpy(&f, b, 4);
        f = 1.0f - f;
        memcpy(b, &f, 4);
    }
}

/* The reference test's sequential versions. */
static void seq_interleave(const pc_io_plane *planes, const uint8_t (*fill)[4], size_t n, pc_sample_type s,
                           const bool *invert, uint8_t *out)
{
    size_t bps = pc_sample_type_bytes(s);
    for (size_t c = 0; c < 4; c++) {
        for (size_t i = 0; i < n; i++) {
            uint8_t *dst = out + (i * 4 + c) * bps;
            if (planes[c].present && planes[c].len >= (i + 1) * bps) {
                const uint8_t *src = planes[c].data + i * bps;
                for (size_t k = 0; k < bps; k++) {
                    dst[k] = src[bps - 1 - k];
                }
                if (invert[c]) {
                    invert_sample(dst, s);
                }
            } else {
                memcpy(dst, fill[c], bps);
            }
        }
    }
}

static void seq_deinterleave(const uint8_t *bytes, size_t len, pc_sample_type s, const bool *invert,
                             uint8_t *out)
{
    size_t bps = pc_sample_type_bytes(s);
    size_t n = len / (4 * bps);
    for (size_t i = 0; i < n; i++) {
        for (size_t c = 0; c < 4; c++) {
            uint8_t tmp[4];
            memcpy(tmp, bytes + (i * 4 + c) * bps, bps);
            if (invert[c]) {
                invert_sample(tmp, s);
            }
            for (size_t k = 0; k < bps; k++) {
                out[c * n * bps + i * bps + k] = tmp[bps - 1 - k];
            }
        }
    }
}

static void test_banded_shuffles_match_a_sequential_reference(void)
{
    static const size_t ns[4] = {1, 7, 64, 1000};
    static const pc_sample_type ss[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const bool invert[4] = {false, true, false, true};
    for (size_t a = 0; a < 4; a++) {
        for (size_t b = 0; b < 3; b++) {
            size_t n = ns[a];
            pc_sample_type s = ss[b];
            size_t bps = pc_sample_type_bytes(s);
            uint8_t *planes[4];
            pc_io_plane refs[4];
            uint8_t fill[4][4];
            uint8_t *got = NULL;
            size_t got_len = 0;
            uint8_t *want = xmalloc(n * 4 * bps);
            uint8_t *back = NULL;
            size_t plane = 0;
            uint8_t *want_back = xmalloc(n * 4 * bps);
            for (size_t c = 0; c < 4; c++) {
                planes[c] = xmalloc(n * bps);
                for (size_t i = 0; i < n * bps; i++) {
                    planes[c][i] = (uint8_t)(i * 13 + c * 5);
                }
                refs[c] = (pc_io_plane){true, planes[c], n * bps};
                memset(fill[c], (int)(c * 40), 4);
            }
            refs[2].present = false;
            CHECK(pc_io_interleave(refs, 4, (const uint8_t (*)[4])fill, n, s, invert, &got, &got_len, NULL) ==
                  PC_OK);
            seq_interleave(refs, (const uint8_t (*)[4])fill, n, s, invert, want);
            CHECK_EQ_UINT(got_len, n * 4 * bps);
            CHECK_EQ_MEM(got, want, n * 4 * bps);
            CHECK(pc_io_deinterleave(got, got_len, 4, s, invert, &back, &plane, NULL) == PC_OK);
            seq_deinterleave(got, got_len, s, invert, want_back);
            CHECK_EQ_UINT(plane, n * bps);
            CHECK_EQ_MEM(back, want_back, n * 4 * bps);
            free(got);
            free(want);
            free(back);
            free(want_back);
            for (size_t c = 0; c < 4; c++) {
                free(planes[c]);
            }
        }
    }
}

static void test_interleave_roundtrip_all_depths(void)
{
    static const pc_sample_type ss[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const bool inv[3] = {false, true, false};
    for (size_t b = 0; b < 3; b++) {
        pc_sample_type s = ss[b];
        size_t bps = pc_sample_type_bytes(s);
        uint8_t planes[3][16];
        pc_io_plane refs[3];
        uint8_t fill[3][4] = {{0}};
        uint8_t *il = NULL;
        size_t il_len = 0;
        uint8_t *back = NULL;
        size_t plane = 0;
        for (size_t c = 0; c < 3; c++) {
            for (size_t i = 0; i < 4 * bps; i++) {
                planes[c][i] = (uint8_t)(i * 7 + c * 3);
            }
            refs[c] = (pc_io_plane){true, planes[c], 4 * bps};
        }
        CHECK(pc_io_interleave(refs, 3, (const uint8_t (*)[4])fill, 4, s, inv, &il, &il_len, NULL) == PC_OK);
        CHECK(pc_io_deinterleave(il, il_len, 3, s, inv, &back, &plane, NULL) == PC_OK);
        CHECK_EQ_UINT(plane, 4 * bps);
        CHECK_EQ_MEM(back, planes[0], plane);
        CHECK_EQ_MEM(back + 2 * plane, planes[2], plane);
        if (s != PC_SAMPLE_TYPE_F32) {
            /* 1 - (1 - v) may not be exact for floats. */
            CHECK_EQ_MEM(back + plane, planes[1], plane);
        }
        free(il);
        free(back);
    }
}

static void test_missing_planes_use_fill(void)
{
    static const uint8_t p1[2] = {5, 6};
    pc_io_plane refs[2] = {{false, NULL, 0}, {true, p1, 2}};
    uint8_t fill[2][4] = {{9}, {0}};
    static const bool inv[2] = {false, false};
    uint8_t *il = NULL;
    size_t len = 0;
    CHECK(pc_io_interleave(refs, 2, (const uint8_t (*)[4])fill, 2, PC_SAMPLE_TYPE_U8, inv, &il, &len, NULL) ==
          PC_OK);
    CHECK_EQ_UINT(len, 4);
    CHECK(il[0] == 9 && il[1] == 5 && il[2] == 9 && il[3] == 6);
    free(il);
}

static uint16_t px16(const uint8_t *b, size_t i, size_t c)
{
    uint16_t v;
    memcpy(&v, b + (i * 3 + c) * 2, 2);
    return v;
}

static void test_lab16_chroma_scale_round_trips(void)
{
    size_t len = (size_t)65536 * 6;
    uint8_t *all = xmalloc(len);
    uint8_t *orig = xmalloc(len);
    uint8_t s[3] = {1, 2, 3};
    for (uint32_t v = 0; v <= 65535; v++) {
        uint16_t u = (uint16_t)v;
        for (size_t c = 0; c < 3; c++) {
            memcpy(all + ((size_t)v * 3 + c) * 2, &u, 2);
        }
    }
    memcpy(orig, all, len);
    pc_io_lab16_chroma(all, len, 3, true);
    /* Neutral (32768 in PSD) is 128 / 255 of the document scale; 65280 (a* = 127) is the top. */
    CHECK_EQ_UINT(px16(all, 32768, 0), 32768);
    CHECK_EQ_UINT(px16(all, 32768, 1), (uint16_t)round(32768.0 * 65535.0 / 65280.0));
    CHECK_EQ_UINT(px16(all, 65280, 2), 65535);
    pc_io_lab16_chroma(all, len, 3, false);
    for (size_t i = 0; i <= 65280; i++) {
        if (px16(all, i, 1) != px16(orig, i, 1)) {
            CHECK_EQ_UINT(px16(all, i, 1), px16(orig, i, 1));
            break;
        }
    }
    /* Short or odd buffers are left alone. */
    pc_io_lab16_chroma(s, 3, 3, true);
    pc_io_lab16_chroma(s, 3, 1, true);
    CHECK(s[0] == 1 && s[1] == 2 && s[2] == 3);
    free(all);
    free(orig);
}

static void test_be_codec(void)
{
    uint8_t v[4];
    CHECK_EQ_UINT(pc_io_encode_be(1.0f, PC_SAMPLE_TYPE_U16, v), 2);
    CHECK(v[0] == 0xff && v[1] == 0xff);
    CHECK(pc_io_decode_be(v, 2, 0, PC_SAMPLE_TYPE_U16) == 1.0f);
    CHECK_EQ_UINT(pc_io_encode_be(2.5f, PC_SAMPLE_TYPE_F32, v), 4);
    CHECK(pc_io_decode_be(v, 4, 0, PC_SAMPLE_TYPE_F32) == 2.5f);
    /* Out of range reads 0 instead of overrunning. */
    CHECK(pc_io_decode_be(v, 4, 1, PC_SAMPLE_TYPE_F32) == 0.0f);
    CHECK(pc_io_decode_be(v, 4, SIZE_MAX, PC_SAMPLE_TYPE_U16) == 0.0f);
    CHECK_EQ_UINT(pc_io_psd_depth(PC_SAMPLE_TYPE_F32), 32);
    CHECK(pc_io_sample_for_depth(1) == PC_SAMPLE_TYPE_U8 && pc_io_sample_for_depth(16) == PC_SAMPLE_TYPE_U16);
    CHECK_EQ_UINT(pc_io_max_sample(PC_SAMPLE_TYPE_U8, v), 1);
    CHECK_EQ_UINT(v[0], 255);
}

/* ---- channel_map.rs ---- */

static bool pc_color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

static pc_alpha_channel gray_channel(const char *name)
{
    pc_surface s;
    pc_alpha_channel c;
    pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
    must(pc_alpha_channel_init(&c, name, &s, NULL));
    return c;
}

static void test_display_info_roundtrip(void)
{
    pc_alpha_channel a = gray_channel("A");
    pc_alpha_channel s = gray_channel("S");
    pc_alpha_channel back[2];
    pc_alpha_channel c;
    const pc_alpha_channel *refs[2] = {&a, &s};
    pc_buf b = PC_BUF_INIT;
    a.color = pc_color_rgb(0, 1, 0);
    a.opacity = 0.3f;
    a.indicates = PC_INDICATES_SELECTED_AREAS;
    s.has_spot = true;
    s.spot_color = pc_color_rgb(1, 0, 1);
    s.spot_solidity = 0.8f;
    must(pc_io_display_info(refs, 2, &b, NULL));
    CHECK_EQ_UINT(b.len, 4 + 28);
    back[0] = gray_channel("A");
    back[1] = gray_channel("S");
    pc_io_apply_display_info(b.data, b.len, true, back, 2);
    CHECK(pc_color_eq(&back[0].color, &a.color));
    CHECK_NEAR(back[0].opacity, 0.3, 1e-6);
    CHECK(back[0].indicates == PC_INDICATES_SELECTED_AREAS);
    CHECK(back[1].has_spot && pc_color_eq(&back[1].spot_color, &s.spot_color) &&
          back[1].spot_solidity == 0.8f);
    /* Truncated data is ignored rather than misread. */
    c = gray_channel("x");
    pc_io_apply_display_info(b.data, 10, true, &c, 1);
    CHECK(c.opacity == 0.5f);
    pc_alpha_channel_fini(&a);
    pc_alpha_channel_fini(&s);
    pc_alpha_channel_fini(&back[0]);
    pc_alpha_channel_fini(&back[1]);
    pc_alpha_channel_fini(&c);
    pc_buf_fini(&b);
}

static void test_decodes_other_spaces(void)
{
    static const uint16_t g[4] = {10000, 0, 0, 0};
    float rgb[3];
    pc_color c = pc_io_decode_color(8, g);
    pc_color_to_rgb(&c, rgb);
    CHECK(rgb[0] == 0.0f && rgb[1] == 0.0f && rgb[2] == 0.0f);
    c = pc_io_decode_color(7, g);
    pc_color_to_rgb(&c, rgb);
    CHECK(rgb[0] > 0.95f && rgb[1] > 0.95f && rgb[2] > 0.95f);
}

/* ---- linked.rs ---- */

static pc_io_linked_file lf(const char *uuid, const char *name, const void *bytes, size_t n)
{
    pc_io_linked_file f;
    memset(&f, 0, sizeof f);
    must(pc_str_set_c(&f.uuid, uuid, NULL));
    must(pc_str_set_c(&f.file_name, name, NULL));
    f.bytes = xmalloc(n);
    memcpy(f.bytes, bytes, n);
    f.len = n;
    return f;
}

static bool lf_eq(const pc_io_linked_file *a, const pc_io_linked_file *b)
{
    return pc_str_eq(&a->uuid, &b->uuid) && pc_str_eq(&a->file_name, &b->file_name) && a->len == b->len &&
           (a->len == 0 || memcmp(a->bytes, b->bytes, a->len) == 0);
}

static void test_round_trips_and_finds_by_uuid(void)
{
    uint8_t nine[10];
    pc_io_linked_file a = lf("abc-1", "a.png", "\x01\x02\x03", 3);
    pc_io_linked_file b;
    pc_buf data = PC_BUF_INIT;
    pc_io_linked_files parsed;
    pc_metadata meta;
    pc_doc_psd_global_block g;
    pc_io_linked_file found;
    bool ok = false;
    memset(nine, 9, sizeof nine);
    b = lf("def-2", "b.psd", nine, 10);
    must(pc_io_encode_linked_file(&a, &data, NULL));
    must(pc_io_encode_linked_file(&b, &data, NULL));
    must(pc_io_parse_linked_files(data.data, data.len, &parsed, NULL));
    CHECK_EQ_UINT(parsed.n, 2);
    CHECK(parsed.n == 2 && lf_eq(&parsed.v[0], &a) && lf_eq(&parsed.v[1], &b));
    pc_io_linked_files_fini(&parsed);
    memset(&meta, 0, sizeof meta);
    memcpy(g.signature, "8BIM", 4);
    memcpy(g.key, "lnk2", 4);
    must(pc_blob_new(data.data, data.len, &g.data, NULL));
    meta.psd_global_blocks = &g;
    meta.npsd_global_blocks = 1;
    must(pc_io_find_linked_file(&meta, "def-2", 5, &ok, &found, NULL));
    CHECK(ok && lf_eq(&found, &b));
    if (ok) {
        pc_io_linked_file_fini(&found);
    }
    must(pc_io_find_linked_file(&meta, "zzz", 3, &ok, &found, NULL));
    CHECK(!ok);
    must(pc_io_find_linked_file(&meta, "", 0, &ok, &found, NULL));
    CHECK(!ok);
    pc_blob_release(g.data);
    pc_io_linked_file_fini(&a);
    pc_io_linked_file_fini(&b);
    pc_buf_fini(&data);
}

static bool not_a(void *ctx, const char *u, size_t n)
{
    (void)ctx;
    return !(n == 1 && u[0] == 'a');
}

static bool all(void *ctx, const char *u, size_t n)
{
    (void)ctx;
    (void)u;
    (void)n;
    return true;
}

static void test_blocks_are_pruned_and_extended(void)
{
    static const uint8_t seven[3] = {7, 7, 7};
    pc_io_linked_file a = lf("a", "a.png", "\x89PNG\x01", 5);
    pc_io_linked_file b = lf("b", "b.psb", "8BPS\0\x02rest", 10);
    pc_io_linked_file c = lf("c", "c", seven, 3);
    pc_buf data = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    pc_io_strs u;
    pc_io_linked_files parsed;
    bool changed = true;
    uint8_t t[4];
    pc_psd_block blk;
    pc_buf bad = PC_BUF_INIT;
    must(pc_io_encode_linked_file(&a, &data, NULL));
    must(pc_io_encode_linked_file(&b, &data, NULL));
    must(pc_io_linked_block_uuids(data.data, data.len, &u, NULL));
    CHECK(u.n == 2 && pc_str_eq_c(&u.v[0], "a") && pc_str_eq_c(&u.v[1], "b"));
    pc_io_strs_fini(&u);
    must(pc_io_rebuild_linked_block(data.data, data.len, all, NULL, NULL, 0, &changed, &out, NULL));
    CHECK(!changed);
    must(pc_io_rebuild_linked_block(data.data, data.len, not_a, NULL, &c, 1, &changed, &out, NULL));
    CHECK(changed);
    must(pc_io_parse_linked_files(out.data, out.len, &parsed, NULL));
    CHECK(parsed.n == 2 && lf_eq(&parsed.v[0], &b) && lf_eq(&parsed.v[1], &c));
    pc_io_linked_files_fini(&parsed);
    must(pc_psd_block_init(&blk, "lnk2", out.data, out.len, NULL));
    CHECK(pc_psd_block_check_structure(&blk, NULL) == PC_OK);
    pc_psd_block_fini(&blk);
    pc_io_linked_file_type(b.bytes, b.len, t);
    CHECK_EQ_MEM(t, "8BPB", 4);
    pc_io_linked_file_type(a.bytes, a.len, t);
    CHECK_EQ_MEM(t, "PNGf", 4);
    /* A malformed tail is kept as is. */
    must(pc_io_encode_linked_file(&a, &bad, NULL));
    must(pc_buf_append(&bad, "\0\0\0\0\0\0\x01\0\x09", 9, NULL));
    must(pc_io_rebuild_linked_block(bad.data, bad.len, all, NULL, &c, 1, &changed, &out, NULL));
    CHECK(changed && out.len > bad.len && memcmp(out.data, bad.data, bad.len) == 0);
    pc_io_linked_file_fini(&a);
    pc_io_linked_file_fini(&b);
    pc_io_linked_file_fini(&c);
    pc_buf_fini(&data);
    pc_buf_fini(&out);
    pc_buf_fini(&bad);
}

static void test_malformed_data_is_ignored(void)
{
    static const uint8_t short_item[10] = {0, 0, 0, 0, 0, 0, 0, 200, 1, 2};
    uint8_t five[4] = {5, 5, 5, 5};
    pc_io_linked_files parsed;
    pc_io_linked_file f = lf("x", "f", five, 4);
    pc_buf d = PC_BUF_INIT;
    must(pc_io_parse_linked_files(short_item, sizeof short_item, &parsed, NULL));
    CHECK_EQ_UINT(parsed.n, 0);
    must(pc_io_parse_linked_files(NULL, 0, &parsed, NULL));
    CHECK_EQ_UINT(parsed.n, 0);
    must(pc_io_encode_linked_file(&f, &d, NULL));
    must(pc_io_parse_linked_files(d.data, d.len - 6, &parsed, NULL));
    CHECK_EQ_UINT(parsed.n, 0);
    /* A length near u64::MAX, an absurd name length. */
    {
        uint8_t huge[24] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8, 'l',  'i', 'F', 'D',
                            0,    0,    0,    7,    0,    0xff, 0xff, 0xff, 0xff, 0,   0,   0};
        must(pc_io_parse_linked_files(huge, sizeof huge, &parsed, NULL));
        CHECK_EQ_UINT(parsed.n, 0);
        must(pc_io_parse_linked_files(huge + 8, sizeof huge - 8, &parsed, NULL));
        CHECK_EQ_UINT(parsed.n, 0);
    }
    pc_io_linked_file_fini(&f);
    pc_buf_fini(&d);
}

/* ---- flat.rs cmyk_band_tests ---- */

static void test_empty_cmyk_images_convert_without_panicking(void)
{
    static const uint32_t sizes[3][2] = {{0, 0}, {0, 7}, {7, 0}};
    for (size_t i = 0; i < 3; i++) {
        pc_cimage img;
        pc_cimage out;
        /* Zero width divided the band size by zero; zero height inverted the clamp range. */
        if (pc_cimage_from_copy(sizes[i][0], sizes[i][1], PC_CL_CMYK, PC_CS_U8, "", 0, &img, NULL) != PC_OK) {
            continue;
        }
        if (pc_io_cmyk_image_to_srgb(&img, &out, NULL) == PC_OK) {
            CHECK(out.width == sizes[i][0] && out.height == sizes[i][1]);
            pc_cimage_fini(&out);
        }
        pc_cimage_fini(&img);
    }
}

static void test_banded_conversion_covers_every_row(void)
{
    /* One colour everywhere converts to one colour everywhere, first row to last. */
    uint32_t w = 5;
    uint32_t h = 9;
    uint8_t *px = xmalloc((size_t)w * h * 5);
    pc_cimage img;
    pc_cimage out;
    bool same = true;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        memcpy(px + i * 5, "\x28\x5a\x0a\x14\xff", 5);
    }
    must(pc_cimage_from_raw(w, h, PC_CL_CMYKA, PC_CS_U8, px, (size_t)w * h * 5, &img, NULL));
    must(pc_io_cmyk_image_to_srgb(&img, &out, NULL));
    CHECK(out.width == w && out.height == h);
    CHECK_EQ_UINT(out.len, (size_t)w * h * 4);
    for (size_t i = 1; i < (size_t)w * h; i++) {
        same = same && memcmp(out.data, out.data + i * 4, 4) == 0;
    }
    CHECK(same);
    CHECK_EQ_UINT(out.data[3], 255);
    CHECK(out.icc.has);
    pc_cimage_fini(&img);
    pc_cimage_fini(&out);
}

/* ---- raw.rs ---- */

static void cam(const char *make, const char *model, const char *want)
{
    char out[128];
    (void)pc_io_camera_name(make, model, out, sizeof out);
    CHECK_EQ_STR(out, want);
}

static void test_camera_names_drop_a_repeated_make(void)
{
    cam("Canon", "Canon EOS 80D", "Canon EOS 80D");
    cam("NIKON CORPORATION", "NIKON D3200", "NIKON D3200");
    cam("SONY", "ILCE-7M3", "SONY ILCE-7M3");
    cam(NULL, "X", "X");
    cam("Panasonic", "DC-G9", "Panasonic DC-G9");
    {
        char tiny[4];
        CHECK_EQ_UINT(pc_io_camera_name("Canon", "Canon EOS", tiny, sizeof tiny), 3);
        CHECK_EQ_STR(tiny, "Can");
    }
}

/* ---- the C API's own edges ---- */

static void test_warnings_and_errors(void)
{
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_io_warnings x = PC_IO_WARNINGS_INIT;
    pc_error e;
    char *v[2] = {"a", "b"};
    char big[2000];
    memset(big, 'z', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    must(pc_io_warn(&w, NULL, "one %d", 1));
    must(pc_io_warn_at(&w, 0, NULL, "zero"));
    must(pc_io_warn_at(&w, 99, NULL, "last"));
    must(pc_io_warnings_extend(&w, "p: ", v, 2, NULL));
    must(pc_io_warn(&w, NULL, "%s", big));
    CHECK_EQ_UINT(w.n, 6);
    CHECK_EQ_STR(w.v[0], "zero");
    CHECK_EQ_STR(w.v[2], "last");
    CHECK_EQ_STR(w.v[4], "p: b");
    CHECK_EQ_UINT(strlen(w.v[5]), sizeof big - 1); /* warnings are not capped */
    must(pc_io_warn(&x, NULL, "moved"));
    must(pc_io_warnings_append(&w, &x, NULL));
    CHECK(x.n == 0 && w.n == 7 && pc_io_warnings_any(&w, "moved"));
    pc_io_warnings_remove(&w, 0);
    pc_io_warnings_remove(&w, 99);
    CHECK_EQ_STR(w.v[0], "one 1");
    pc_io_warnings_fini(&w);
    pc_io_warnings_fini(&w);
    must(pc_io_unequal_resolution_warning(300, 300.009, &w, NULL));
    CHECK_EQ_UINT(w.n, 0);
    must(pc_io_unequal_resolution_warning(300, 150.006, &w, NULL));
    CHECK(w.n == 1 && strstr(w.v[0], "(150.01 ppi)") != NULL && strstr(w.v[0], "(300.00 ppi)") != NULL);
    pc_io_warnings_fini(&w);
    /* Error kinds and prefixes. */
    CHECK(pc_io_unsupported(&e, "x %d", 2) == PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported: x 2");
    CHECK(pc_io_error_kind(PC_ERR_UNSUPPORTED, &e) == PC_IO_ERR_UNSUPPORTED);
    CHECK(pc_io_cancelled(&e) == PC_ERR_CANCELLED &&
          pc_io_error_kind(PC_ERR_CANCELLED, &e) == PC_IO_ERR_CANCELLED);
    pc_fail_set(&e, PC_ERR_FORMAT, "%s", big);
    CHECK(pc_io_err_prefix(PC_ERR_FORMAT, &e, "codec: ") == PC_ERR_FORMAT);
    CHECK(strncmp(e.msg, "codec: zzz", 10) == 0 && strlen(e.msg) == PC_ERROR_MSG_MAX - 1);
    pc_fail_set(&e, PC_ERR_NOMEM, "out of memory");
    CHECK(pc_io_err_prefix(PC_ERR_NOMEM, &e, "codec: ") == PC_ERR_NOMEM);
    CHECK_EQ_STR(e.msg, "out of memory");
    CHECK(pc_io_err_prefix(PC_ERR_FORMAT, NULL, "x") == PC_ERR_FORMAT);
    pc_fail_set(&e, PC_ERR_FORMAT, "malformed camera raw: x");
    CHECK(pc_io_error_kind(PC_ERR_FORMAT, &e) == PC_IO_ERR_RAW);
    CHECK(pc_io_error_kind(PC_ERR_NOMEM, &e) == PC_IO_ERR_OTHER);
    /* Raw blocks. */
    {
        pc_io_raw_blocks r = {NULL, 0, 0};
        must(pc_io_raw_blocks_push(&r, (const uint8_t *)"abcd", "xy", 2, NULL));
        must(pc_io_raw_blocks_push(&r, (const uint8_t *)"efgh", NULL, 0, NULL));
        must(pc_io_raw_blocks_push(&r, (const uint8_t *)"abcd", "z", 1, NULL));
        CHECK(pc_io_raw_blocks_find(&r, (const uint8_t *)"efgh") != NULL);
        CHECK_EQ_UINT(pc_io_raw_blocks_remove_key(&r, (const uint8_t *)"abcd"), 2);
        CHECK(r.n == 1 && pc_io_raw_blocks_find(&r, (const uint8_t *)"abcd") == NULL);
        pc_io_raw_blocks_fini(&r);
        pc_io_raw_blocks_fini(NULL);
    }
}

static void test_try_buffer_limits(void)
{
    uint8_t *p = NULL;
    size_t n = 0;
    pc_error e;
    CHECK(pc_io_try_buffer(SIZE_MAX, 2, &p, &n, &e) == PC_ERR_UNSUPPORTED);
    CHECK_EQ_STR(e.msg, "unsupported: image too large");
    CHECK(pc_io_try_buffer(SIZE_MAX / 2, 1, &p, &n, &e) == PC_ERR_NOMEM);
    CHECK_CONTAINS(e.msg, "not enough memory for a ");
    CHECK(pc_io_try_buffer(3, 4, &p, &n, &e) == PC_OK && n == 12);
    free(p);
}

int main(int argc, char **argv)
{
    RUN(test_banded_shuffles_match_a_sequential_reference);
    RUN(test_interleave_roundtrip_all_depths);
    RUN(test_missing_planes_use_fill);
    RUN(test_lab16_chroma_scale_round_trips);
    RUN(test_be_codec);
    RUN(test_display_info_roundtrip);
    RUN(test_decodes_other_spaces);
    RUN(test_round_trips_and_finds_by_uuid);
    RUN(test_blocks_are_pruned_and_extended);
    RUN(test_malformed_data_is_ignored);
    RUN(test_empty_cmyk_images_convert_without_panicking);
    RUN(test_banded_conversion_covers_every_row);
    RUN(test_camera_names_drop_a_repeated_make);
    RUN(test_warnings_and_errors);
    RUN(test_try_buffer_limits);
    return pc_t_finish();
}
