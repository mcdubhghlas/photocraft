/* Ports crates/codecs/tests/{exr_multipart,exr_cryptomatte}.rs. The synthetic files the Rust tests
 * write with the `exr` crate are committed as the .exr files in tests/fixtures/codecs (byte-identical, made
 * by `tools/rustref/target/release/rustref codecs-fixtures tests/fixtures/codecs`). Header listings work in
 * every build; pixel decoding needs OpenEXRCore (skipped with WITH_EXR=0). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "harness.h"

static pc_buf load(const char *name)
{
    char path[256];
    pc_buf b = PC_BUF_INIT;
    (void)snprintf(path, sizeof path, "tests/fixtures/codecs/%s", name);
    if (pc_file_read(path, (size_t)1 << 20, &b, NULL) != PC_OK) {
        PC_T_FAIL("missing fixture %s", path);
    }
    return b;
}

#ifndef PC_NO_EXR
static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-6f;
}

static bool has_more_parts(const pc_cimage *img, uint32_t total)
{
    size_t i;
    for (i = 0; i < img->nwarnings; i++) {
        if (img->warnings[i].kind == PC_CW_MORE_PARTS && img->warnings[i].has_total &&
            img->warnings[i].total == total) {
            return true;
        }
    }
    return false;
}

#endif

/* ---- exr_multipart.rs ---- */

static void test_info_lists_every_part(void)
{
    pc_buf b = load("aov.exr");
    pc_climits l = pc_climits_default();
    pc_exr_part_info *p = NULL;
    size_t n = 0;
    CHECK_EQ_INT(pc_cexr_info(b.data, b.len, &l, &p, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 3);
    if (n == 3) {
        static const char *const names[3] = {"Z", "beauty", "diffuse"};
        static const char *const chans[4] = {"A", "B", "G", "R"};
        size_t i;
        for (i = 0; i < 3; i++) {
            CHECK(p[i].name != NULL && strcmp(p[i].name, names[i]) == 0);
        }
        CHECK(p[0].width == 4 && p[0].height == 4 && !p[0].deep && !p[0].tiled);
        CHECK(p[0].nchannels == 1 && strcmp(p[0].channels[0].name, "Z") == 0 &&
              p[0].channels[0].sample == PC_CS_F32);
        CHECK_EQ_UINT(p[1].nchannels, 4);
        for (i = 0; i < 4 && i < p[1].nchannels; i++) {
            CHECK_EQ_STR(p[1].channels[i].name, chans[i]);
            CHECK(p[1].channels[i].sample == PC_CS_F32);
        }
        CHECK_EQ_UINT(pc_exr_part_color_rank(&p[0]), 1);
        CHECK_EQ_UINT(pc_exr_part_color_rank(&p[1]), 4);
        CHECK_EQ_UINT(pc_exr_part_color_rank(&p[2]), 3);
    }
    pc_exr_parts_free(p, n);
    pc_buf_fini(&b);
}

static void test_parts_are_capped_together(void)
{
    /* Each part fits alone (largest 256 bytes) but the three together (512) do not. */
    pc_buf b = load("aov.exr");
    pc_climits l = pc_climits_default();
    pc_exr_part_info *p = NULL;
    size_t n = 0;
    pc_error e;
    l.max_alloc = 300;
    CHECK(pc_cexr_info(b.data, b.len, &l, &p, &n, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "across 3 parts");
    pc_buf_fini(&b);
}

static void test_views_are_listed(void)
{
    pc_buf b = load("stereo.exr");
    pc_climits l = pc_climits_default();
    pc_exr_part_info *p = NULL;
    size_t n = 0;
    CHECK_EQ_INT(pc_cexr_info(b.data, b.len, &l, &p, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 2);
    if (n == 2) {
        CHECK(p[0].view != NULL && strcmp(p[0].view, "left") == 0);
        CHECK(p[1].view != NULL && strcmp(p[1].view, "right") == 0);
    }
    pc_exr_parts_free(p, n);
    pc_buf_fini(&b);
}

#ifndef PC_NO_EXR
static void test_auto_pick_skips_the_depth_part(void)
{
    pc_buf b = load("aov.exr");
    pc_cimage img;
    int i;
    CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
    CHECK(img.layout == PC_CL_RGBA);
    CHECK(has_more_parts(&img, 3));
    for (i = 0; i < 16; i++) {
        uint32_t x = (uint32_t)(i % 4);
        uint32_t y = (uint32_t)(i / 4);
        CHECK(near(pc_cimage_get(&img, x, y, 0), (float)i / 15.0f));
        CHECK(near(pc_cimage_get(&img, x, y, 1), 1.0f - (float)i / 15.0f));
        CHECK(near(pc_cimage_get(&img, x, y, 2), 0.25f));
        CHECK(near(pc_cimage_get(&img, x, y, 3), 1.0f));
    }
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
}

static void test_parts_decode_by_index(void)
{
    pc_buf b = load("aov.exr");
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_cimage z;
    pc_cimage d;
    pc_error e;
    int i;
    CHECK_EQ_INT(pc_cdecode_exr_part(b.data, b.len, 0, &o, &z, NULL), PC_OK);
    CHECK(z.layout == PC_CL_GRAY);
    for (i = 0; i < 16; i++) {
        CHECK(near(pc_cimage_get(&z, (uint32_t)(i % 4), (uint32_t)(i / 4), 0), (float)i));
    }
    pc_cimage_fini(&z);
    CHECK_EQ_INT(pc_cdecode_exr_part(b.data, b.len, 2, &o, &d, NULL), PC_OK);
    CHECK(d.layout == PC_CL_RGB);
    CHECK(near(pc_cimage_get(&d, 1, 2, 2), 0.9f));
    CHECK(!has_more_parts(&d, 3));
    pc_cimage_fini(&d);
    /* part_index_out_of_range_errors */
    CHECK(pc_cdecode_exr_part(b.data, b.len, 3, &o, &d, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "out of range");
    CHECK_CONTAINS(e.msg, "3 parts");
    pc_buf_fini(&b);
}

static void test_left_view_wins_the_auto_pick(void)
{
    pc_buf b = load("stereo.exr");
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_cimage img;
    pc_cimage right;
    CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
    CHECK(near(pc_cimage_get(&img, 0, 0, 0), 0.0f) && near(pc_cimage_get(&img, 3 % 2, 1, 0), 0.1f));
    CHECK(near(pc_cimage_get(&img, 1, 1, 1), 0.1f));
    pc_cimage_fini(&img);
    CHECK_EQ_INT(pc_cdecode_exr_part(b.data, b.len, 1, &o, &right, NULL), PC_OK);
    CHECK(near(pc_cimage_get(&right, 1, 1, 1), 0.9f));
    pc_cimage_fini(&right);
    pc_buf_fini(&b);
}

static void test_single_part_files_do_not_warn(void)
{
    pc_buf b = load("single_y.exr");
    pc_cimage img;
    CHECK_EQ_INT(pc_cdecode(b.data, b.len, &img, NULL), PC_OK);
    CHECK(img.layout == PC_CL_GRAY);
    CHECK_EQ_UINT(img.nwarnings, 0);
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
}

static void test_half_float_parts_stay_half(void)
{
    pc_buf b = load("half_rgb.exr");
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_cimage img;
    CHECK_EQ_INT(pc_cdecode_exr_part(b.data, b.len, 0, &o, &img, NULL), PC_OK);
    CHECK(img.sample == PC_CS_F16);
    CHECK(near(pc_cimage_get(&img, 1, 1, 0), 0.5f));
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
}

static void test_named_channels(void)
{
    pc_buf b = load("aov.exr");
    pc_cdecode_opts o = pc_cdecode_opts_default();
    static const char *const rgb[3] = {"R", "G", "B"};
    static const char *const z[1] = {"Z"};
    static const char *const none[1] = {"nope"};
    pc_cimage img;
    CHECK_EQ_INT(pc_cdecode_exr_channels(b.data, b.len, 1, rgb, 3, &o, &img, NULL), PC_OK);
    CHECK(img.layout == PC_CL_RGB && near(pc_cimage_get(&img, 0, 0, 2), 0.25f));
    pc_cimage_fini(&img);
    CHECK_EQ_INT(pc_cdecode_exr_channels(b.data, b.len, 0, z, 1, &o, &img, NULL), PC_OK);
    CHECK(img.layout == PC_CL_GRAY && near(pc_cimage_get(&img, 3, 3, 0), 15.0f));
    pc_cimage_fini(&img);
    CHECK(pc_cdecode_exr_channels(b.data, b.len, 0, none, 1, &o, &img, NULL) != PC_OK);
    pc_buf_fini(&b);
}
#endif

static void test_truncated_files_error_never_crash(void)
{
    pc_buf b = load("aov.exr");
    pc_climits l = pc_climits_default();
    pc_cdecode_opts o = pc_cdecode_opts_default();
    size_t end;
    for (end = 8; end < b.len; end += 5) {
        pc_exr_part_info *p = NULL;
        size_t n = 0;
        pc_cimage img;
        if (pc_cexr_info(b.data, end, &l, &p, &n, NULL) == PC_OK) {
            pc_exr_parts_free(p, n);
        }
        if (pc_cdecode_as_with(PC_CF_EXR, b.data, end, &o, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
        if (pc_cdecode_exr_part(b.data, end, 1, &o, &img, NULL) == PC_OK) {
            pc_cimage_fini(&img);
        }
    }
    CHECK(b.len > 0);
    pc_buf_fini(&b);
}

/* ---- exr_cryptomatte.rs ---- */

static float idf(const char *s)
{
    return pc_cryptomatte_id(s, strlen(s));
}

static uint32_t bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void test_mm3_hash_matches_the_official_vectors(void)
{
    static const char *const names[6] = {"hello",
                                         "cube",
                                         "sphere",
                                         "plane",
                                         "\xd1\x80\xd0\xb0\xd0\xb2\xd0\xbd\xd0\xb8\xd0\xbd\xd0\xb0",
                                         ("m\xc3\xa4"
                                          "dchen")};
    static const double want[6] = {6.0705627102400005616e-17,  -4.08461912519e+15,
                                   2.79018604383e+15,          3.66557617593e-11,
                                   -1.3192631212399999468e-25, 6.2361298211599995797e+25};
    int i;
    for (i = 0; i < 6; i++) {
        CHECK_EQ_UINT(bits(idf(names[i])), bits((float)want[i]));
    }
}

static void test_every_id_is_a_finite_normal_float(void)
{
    static const char *const names[5] = {"", "a", "hello", "CryptoAsset",
                                         ("very long object name with \xc3\xbcn\xc3\xaf"
                                          "c\xc3\xb6"
                                          "d\xc3\xa9")};
    int i;
    for (i = 0; i < 5; i++) {
        float id = idf(names[i]);
        CHECK(isfinite(id) && fabsf(id) >= 1.17549435e-38f);
    }
}

static void test_key_and_preview_color(void)
{
    char key[8];
    char full[16];
    float c[3];
    float id = idf("cube");
    uint32_t u = bits(id);
    float g;
    float b;
    pc_cryptomatte_key("CryptoAsset", 11, key);
    (void)snprintf(full, sizeof full, "%08x", (unsigned)bits(idf("CryptoAsset")));
    CHECK_EQ_UINT(strlen(key), 7);
    CHECK(strncmp(key, full, 7) == 0);
    pc_cryptomatte_preview_color(id, c);
    u <<= 8;
    memcpy(&g, &u, 4);
    u = bits(id) << 16;
    memcpy(&b, &u, 4);
    CHECK(c[0] == 0.0f && bits(c[1]) == bits(g) && bits(c[2]) == bits(b));
}

static void test_layers_list_name_streams_and_manifest(void)
{
    pc_buf b = load("crypto_asset.exr");
    pc_climits l = pc_climits_default();
    pc_crypto_layer *ls = NULL;
    size_t n = 0;
    char key[8];
    pc_cryptomatte_key("CryptoAsset", 11, key);
    CHECK_EQ_INT(pc_ccryptomatte_layers(b.data, b.len, &l, &ls, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 1);
    if (n == 1) {
        const pc_crypto_layer *x = &ls[0];
        float cube = idf("cube");
        float sphere = idf("sphere");
        size_t i;
        bool has_cube = false;
        bool has_sphere = false;
        CHECK_EQ_UINT(x->part, 0);
        CHECK_EQ_STR(x->name, "CryptoAsset");
        CHECK_EQ_STR(x->key, key);
        CHECK(x->conversion != NULL && strcmp(x->conversion, "uint32_to_float32") == 0);
        CHECK(x->nchannels == 2 && strcmp(x->channels[0], "crypto_asset00") == 0 &&
              strcmp(x->channels[1], "crypto_asset01") == 0);
        CHECK_EQ_UINT(x->nmanifest, 2);
        for (i = 0; i < x->nmanifest; i++) {
            has_cube = has_cube || (x->manifest[i].id == cube && strcmp(x->manifest[i].name, "cube") == 0);
            has_sphere =
                has_sphere || (x->manifest[i].id == sphere && strcmp(x->manifest[i].name, "sphere") == 0);
        }
        CHECK(has_cube && has_sphere);
        CHECK(pc_crypto_layer_name_of(x, cube) != NULL &&
              strcmp(pc_crypto_layer_name_of(x, cube), "cube") == 0);
    }
    pc_crypto_layers_free(ls, n);
    pc_buf_fini(&b);
}

static void test_plain_exr_has_no_cryptomatte_layers(void)
{
    pc_buf b = load("plain_rgb.exr");
    pc_climits l = pc_climits_default();
    pc_crypto_layer *ls = NULL;
    size_t n = 7;
    CHECK_EQ_INT(pc_ccryptomatte_layers(b.data, b.len, &l, &ls, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 0);
    pc_crypto_layers_free(ls, n);
    pc_buf_fini(&b);
}

static void corrupt_manifest(pc_buf *b)
{
    static const char needle[] = "{\"cube\":";
    size_t i;
    for (i = 0; i + 8 <= b->len; i++) {
        if (memcmp(b->data + i, needle, 8) == 0) {
            memset(b->data + i, '#', 8);
            return;
        }
    }
    CHECK(false);
}

static void test_a_broken_manifest_degrades_to_empty(void)
{
    pc_buf b = load("crypto_asset.exr");
    pc_climits l = pc_climits_default();
    pc_crypto_layer *ls = NULL;
    size_t n = 0;
    corrupt_manifest(&b);
    CHECK_EQ_INT(pc_ccryptomatte_layers(b.data, b.len, &l, &ls, &n, NULL), PC_OK);
    CHECK(n == 1 && ls[0].nmanifest == 0);
    pc_crypto_layers_free(ls, n);
#ifndef PC_NO_EXR
    {
        pc_crypto_buffer buf;
        size_t np = 0;
        memset(&buf, 0, sizeof buf);
        CHECK_EQ_INT(pc_cdecode_cryptomatte(b.data, b.len, "CryptoAsset", &l, &buf, NULL), PC_OK);
        (void)pc_crypto_buffer_at(&buf, 1, 0, &np);
        CHECK_EQ_UINT(np, 1);
        pc_crypto_buffer_fini(&buf);
    }
#endif
    pc_buf_fini(&b);
}

#ifndef PC_NO_EXR
static void test_decode_gives_the_pairs_per_pixel_sorted_by_coverage(void)
{
    pc_buf b = load("crypto_asset.exr");
    pc_climits l = pc_climits_default();
    pc_crypto_buffer buf;
    float cube = idf("cube");
    float sphere = idf("sphere");
    uint32_t x;
    uint32_t y;
    memset(&buf, 0, sizeof buf);
    CHECK_EQ_INT(pc_cdecode_cryptomatte(b.data, b.len, "CryptoAsset", &l, &buf, NULL), PC_OK);
    CHECK(buf.width == 4 && buf.height == 4);
    for (y = 0; y < 4; y++) {
        size_t np = 0;
        const pc_crypto_pair *p = pc_crypto_buffer_at(&buf, 0, y, &np);
        CHECK(np == 2 && p[0].id == cube && p[0].coverage == 0.6f && p[1].id == sphere &&
              p[1].coverage == 0.25f);
        for (x = 1; x < 4; x++) {
            p = pc_crypto_buffer_at(&buf, x, y, &np);
            CHECK(np == 1 && p[0].id == cube && p[0].coverage == 0.6f);
        }
    }
    pc_crypto_buffer_fini(&buf);
    pc_buf_fini(&b);
}

static void test_short_suffixes_and_unknown_layers(void)
{
    pc_buf b = load("crypto_short.exr");
    pc_buf a = load("crypto_asset.exr");
    pc_climits l = pc_climits_default();
    pc_crypto_buffer buf;
    size_t np = 0;
    const pc_crypto_pair *p;
    pc_error e;
    memset(&buf, 0, sizeof buf);
    CHECK_EQ_INT(pc_cdecode_cryptomatte(b.data, b.len, "CryptoAsset", &l, &buf, NULL), PC_OK);
    p = pc_crypto_buffer_at(&buf, 3, 3, &np);
    CHECK(np == 1 && p[0].id == idf("cube") && p[0].coverage == 1.0f);
    pc_crypto_buffer_fini(&buf);
    CHECK(pc_cdecode_cryptomatte(a.data, a.len, "CryptoMaterial", &l, &buf, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "no Cryptomatte layer");
    /* Layer names match ASCII case-insensitively. */
    CHECK_EQ_INT(pc_cdecode_cryptomatte(a.data, a.len, "cryptoasset", &l, &buf, NULL), PC_OK);
    pc_crypto_buffer_fini(&buf);
    pc_buf_fini(&a);
    pc_buf_fini(&b);
}
#endif

static void test_cryptomatte_hostile_input_never_crashes(void)
{
    pc_buf b = load("crypto_asset.exr");
    pc_climits l = pc_climits_default();
    pc_climits tiny = pc_climits_default();
    pc_crypto_layer *ls = NULL;
    size_t n = 0;
    pc_error e;
    size_t end;
    for (end = 8; end < b.len; end += 7) {
        pc_crypto_buffer buf;
        memset(&buf, 0, sizeof buf);
        if (pc_ccryptomatte_layers(b.data, end, &l, &ls, &n, NULL) == PC_OK) {
            pc_crypto_layers_free(ls, n);
        }
        if (pc_cdecode_cryptomatte(b.data, end, "CryptoAsset", &l, &buf, NULL) == PC_OK) {
            pc_crypto_buffer_fini(&buf);
        }
        if (pc_cdecode_cryptomatte(b.data, end, "nothing", &l, &buf, NULL) == PC_OK) {
            pc_crypto_buffer_fini(&buf);
        }
    }
    for (end = 0; end < b.len; end++) {
        pc_crypto_buffer buf;
        memset(&buf, 0, sizeof buf);
        b.data[end] ^= 0x5a;
        if (pc_ccryptomatte_layers(b.data, b.len, &l, &ls, &n, NULL) == PC_OK) {
            pc_crypto_layers_free(ls, n);
        }
        if (pc_cdecode_cryptomatte(b.data, b.len, "CryptoAsset", &l, &buf, NULL) == PC_OK) {
            pc_crypto_buffer_fini(&buf);
        }
        b.data[end] ^= 0x5a;
    }
    /* Tiny limits refuse before allocating. */
    tiny.max_alloc = 8;
    CHECK(pc_ccryptomatte_layers(b.data, b.len, &tiny, &ls, &n, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "exceed");
    pc_buf_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_info_lists_every_part);
    RUN(test_parts_are_capped_together);
    RUN(test_views_are_listed);
#ifndef PC_NO_EXR
    RUN(test_auto_pick_skips_the_depth_part);
    RUN(test_parts_decode_by_index);
    RUN(test_left_view_wins_the_auto_pick);
    RUN(test_single_part_files_do_not_warn);
    RUN(test_half_float_parts_stay_half);
    RUN(test_named_channels);
    RUN(test_decode_gives_the_pairs_per_pixel_sorted_by_coverage);
    RUN(test_short_suffixes_and_unknown_layers);
#endif
    RUN(test_truncated_files_error_never_crash);
    RUN(test_mm3_hash_matches_the_official_vectors);
    RUN(test_every_id_is_a_finite_normal_float);
    RUN(test_key_and_preview_color);
    RUN(test_layers_list_name_streams_and_manifest);
    RUN(test_plain_exr_has_no_cryptomatte_layers);
    RUN(test_a_broken_manifest_degrades_to_empty);
    RUN(test_cryptomatte_hostile_input_never_crashes);
    return pc_t_finish();
}
