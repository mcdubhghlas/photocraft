/* Decode parity for the formats R2 added: the FITS, PCX and EXR fixtures in tests/fixtures/codecs
 * (made by `rustref codecs-fixtures`) against the reference's decodes recorded by
 * `tools/golden.sh decode tests/fixtures/codecs` (size, layout, sample type, warnings, SHA-256 of
 * the samples, or the error text), and the FITS writer byte for byte against the reference's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "harness.h"

/* SHA-256 (FIPS 180-4) as lower-case hex: the goldens' raw_sha256. */
static void sha256_hex(const uint8_t *d, size_t n, char out[65])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t bits = (uint64_t)n * 8;
    size_t total = (n + 9 + 63) / 64 * 64;
    size_t off;
    int i;
    for (off = 0; off < total; off += 64) {
        uint8_t blk[64];
        uint32_t w[64];
        uint32_t a[8];
        for (i = 0; i < 64; i++) {
            size_t at = off + (size_t)i;
            uint64_t v = at < n            ? d[at]
                         : at == n         ? 0x80
                         : at >= total - 8 ? bits >> (8 * (total - 1 - at))
                                           : 0;
            blk[i] = (uint8_t)v;
        }
        for (i = 0; i < 16; i++) {
            w[i] = ((uint32_t)blk[4 * i] << 24) | ((uint32_t)blk[4 * i + 1] << 16) |
                   ((uint32_t)blk[4 * i + 2] << 8) | blk[4 * i + 3];
        }
        for (i = 16; i < 64; i++) {
            uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^
                          (w[i - 15] >> 3);
            uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^
                          (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        memcpy(a, h, sizeof a);
        for (i = 0; i < 64; i++) {
            uint32_t S1 =
                ((a[4] >> 6) | (a[4] << 26)) ^ ((a[4] >> 11) | (a[4] << 21)) ^ ((a[4] >> 25) | (a[4] << 7));
            uint32_t t1 = a[7] + S1 + ((a[4] & a[5]) ^ (~a[4] & a[6])) + k[i] + w[i];
            uint32_t S0 =
                ((a[0] >> 2) | (a[0] << 30)) ^ ((a[0] >> 13) | (a[0] << 19)) ^ ((a[0] >> 22) | (a[0] << 10));
            uint32_t t2 = S0 + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
            memmove(a + 1, a, 7 * sizeof a[0]);
            a[4] += t1;
            a[0] = t1 + t2;
        }
        for (i = 0; i < 8; i++) {
            h[i] += a[i];
        }
    }
    for (i = 0; i < 8; i++) {
        (void)snprintf(out + 8 * i, 9, "%08x", (unsigned)h[i]);
    }
}

static const char *const LAYOUTS[6] = {"Gray", "GrayA", "Rgb", "Rgba", "Cmyk", "Cmyka"};
static const char *const SAMPLES[4] = {"U8", "U16", "F16", "F32"};

static pc_buf read_file(const char *path)
{
    pc_buf b = PC_BUF_INIT;
    if (pc_file_read(path, (size_t)1 << 22, &b, NULL) != PC_OK) {
        PC_T_FAIL("missing %s", path);
    }
    return b;
}

static void check_fixture(const char *name)
{
    char path[256];
    char want[1024];
    char got[1024];
    pc_buf file;
    pc_buf golden;
    pc_cimage img;
    pc_error e;
    size_t n;
    (void)snprintf(path, sizeof path, "tests/fixtures/codecs/%s", name);
    file = read_file(path);
    (void)snprintf(path, sizeof path, "tests/golden/decode/tests/fixtures/codecs/%s.json", name);
    golden = read_file(path);
    n = golden.len < sizeof want - 1 ? golden.len : sizeof want - 1;
    memcpy(want, golden.data, n);
    want[n] = '\0';
    while (n > 0 && (want[n - 1] == '\n' || want[n - 1] == '\r')) {
        want[--n] = '\0';
    }
    if (pc_cdecode(file.data, file.len, &img, &e) != PC_OK) {
        (void)snprintf(got, sizeof got, "{\"error\":\"%s\"}", e.msg);
    } else {
        char sha[65];
        char warn[512] = "";
        size_t at = 0;
        size_t i;
        for (i = 0; i < img.nwarnings; i++) {
            char t[200];
            pc_cwarning_text(&img.warnings[i], t, sizeof t);
            at += (size_t)snprintf(warn + at, sizeof warn - at, "%s\"%s\"", i > 0 ? "," : "", t);
            if (at >= sizeof warn) {
                break;
            }
        }
        sha256_hex(img.data, img.len, sha);
        if (img.icc.has) {
            (void)snprintf(got, sizeof got,
                           "{\"bytes\":%zu,\"height\":%u,\"icc_len\":%zu,\"layout\":\"%s\",\"sample\":\"%s\","
                           "\"warnings\":[%s],\"width\":%u,\"raw_sha256\":\"%s\"}",
                           img.len, (unsigned)img.height, img.icc.len, LAYOUTS[img.layout],
                           SAMPLES[img.sample], warn, (unsigned)img.width, sha);
        } else {
            (void)snprintf(
                got, sizeof got,
                "{\"bytes\":%zu,\"height\":%u,\"icc_len\":null,\"layout\":\"%s\",\"sample\":\"%s\","
                "\"warnings\":[%s],\"width\":%u,\"raw_sha256\":\"%s\"}",
                img.len, (unsigned)img.height, LAYOUTS[img.layout], SAMPLES[img.sample], warn,
                (unsigned)img.width, sha);
        }
        pc_cimage_fini(&img);
    }
    pc_t_checks++;
    if (strcmp(got, want) != 0) {
        PC_T_FAIL("%s:\n  got  %s\n  want %s", name, got, want);
    }
    pc_buf_fini(&file);
    pc_buf_fini(&golden);
}

static void test_fits_and_pcx_decodes_match_the_reference(void)
{
    static const char *const names[] = {"fits_u16_bzero.fits",
                                        "fits_f32_rgb.fits",
                                        "fits_i32_extension.fits",
                                        "fits_written_u16.fits",
                                        "fits_written_f32_rgb.fits",
                                        "fits_written_u8_rgba.fits",
                                        "pcx_rgb24.pcx",
                                        "pcx_vga.pcx",
                                        "pcx_4bit.pcx",
                                        "pcx_1bit_4planes.pcx"};
    size_t i;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        check_fixture(names[i]);
    }
}

static void test_exr_decodes_match_the_reference(void)
{
    static const char *const names[] = {"aov.exr",         "stereo.exr",    "single_y.exr",
                                        "half_rgb.exr",    "plain_rgb.exr", "crypto_asset.exr",
                                        "crypto_short.exr"};
    size_t i;
    if (!pc_cformat_caps(PC_CF_EXR).read) {
        return;
    }
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        check_fixture(names[i]);
    }
}

/* The images and metadata of rustref's fits_written(kind). */
static void test_fits_writer_matches_the_reference_bytes(void)
{
    static const char *const names[3] = {"fits_written_u16.fits", "fits_written_f32_rgb.fits",
                                         "fits_written_u8_rgba.fits"};
    static const uint16_t u16px[6] = {0, 1000, 20000, 40000, 65535, 7};
    static const float f32px[12] = {0.0f, 0.5f,   1.0f, 0.25f, 0.75f, 0.125f,
                                    1.5f, -0.25f, 0.3f, 0.6f,  0.9f,  0.05f};
    static const uint8_t u8px[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    static const char *const keys[5] = {"OBSERVER", "Software", "HISTORY", "EXPTIME", "Comment"};
    static const char *const vals[5] = {
        "O'Brien", "photocraft 1.0", "stacked", "300.5",
        "a long comment that does not fit the eighty columns of one card, so it wraps"};
    int kind;
    for (kind = 0; kind < 3; kind++) {
        char path[256];
        pc_cimage img;
        pc_buf out = PC_BUF_INIT;
        pc_buf want;
        int k;
        if (kind == 0) {
            CHECK_EQ_INT(pc_cimage_from_copy(3, 2, PC_CL_GRAY, PC_CS_U16, u16px, sizeof u16px, &img, NULL),
                         PC_OK);
        } else if (kind == 1) {
            CHECK_EQ_INT(pc_cimage_from_copy(2, 2, PC_CL_RGB, PC_CS_F32, f32px, sizeof f32px, &img, NULL),
                         PC_OK);
        } else {
            CHECK_EQ_INT(pc_cimage_from_copy(2, 1, PC_CL_RGBA, PC_CS_U8, u8px, sizeof u8px, &img, NULL),
                         PC_OK);
        }
        for (k = 0; k < 5; k++) {
            (void)pc_cmeta_add_text(&img.meta, keys[k], strlen(keys[k]), vals[k], strlen(vals[k]), NULL);
        }
        img.meta.has_dpi = true;
        img.meta.dpi_x = img.meta.dpi_y = 300.0f;
        CHECK_EQ_INT(pc_cencode(&img, PC_CF_FITS, NULL, &out, NULL), PC_OK);
        (void)snprintf(path, sizeof path, "tests/fixtures/codecs/%s", names[kind]);
        want = read_file(path);
        if (out.len != want.len || memcmp(out.data, want.data, out.len) != 0) {
            size_t i = 0;
            while (i < out.len && i < want.len && out.data[i] == want.data[i]) {
                i++;
            }
            PC_T_FAIL("%s: %zu bytes, want %zu; first difference at %zu", names[kind], out.len, want.len, i);
        }
        pc_t_checks++;
        pc_buf_fini(&want);
        pc_buf_fini(&out);
        pc_cimage_fini(&img);
    }
}

int main(int argc, char **argv)
{
    RUN(test_fits_and_pcx_decodes_match_the_reference);
    RUN(test_exr_decodes_match_the_reference);
    RUN(test_fits_writer_matches_the_reference_bytes);
    return pc_t_finish();
}
