/* EXIF orientation end to end through photocraft-cli (port of apps/photocraft-cli/tests/
 * orientation.rs, #285): a phone-style JPEG (stored landscape, Orientation = 6) converts to an
 * upright portrait, an explicit rotation turns it back, and a .pcraft round trip changes nothing.
 *
 * Driven through pc_cli_run in-process (the C convention), with fixtures built via the codecs API
 * as the reference builds them via photocraft_codecs. The reference also reads EXIF orientation off
 * the raw output file bytes; this port reads it from the decoded image's embedded EXIF instead
 * (pc_exif_orientation takes a TIFF-structured block, not a whole PNG/JPEG file). */
#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "cli/cli.h"
#include "codecs/codecs.h"
#include "codecs/orientation.h"

static int run(char **argv, int argc)
{
    FILE *fo = tmpfile();
    FILE *fe = tmpfile();
    int code = pc_cli_run(argc, argv, fo, fe);
    if (code != 0) {
        rewind(fe);
        char buf[2048];
        size_t n = fread(buf, 1, sizeof buf - 1, fe);
        buf[n] = '\0';
        PC_T_FAIL("cli exit %d: %s", code, buf);
    }
    fclose(fo);
    fclose(fe);
    return code;
}

/* run capturing stdout into `out`. */
static void run_out(char **argv, int argc, char *out, size_t cap)
{
    FILE *fo = tmpfile();
    FILE *fe = tmpfile();
    int code = pc_cli_run(argc, argv, fo, fe);
    (void)code;
    rewind(fo);
    size_t n = fread(out, 1, cap - 1, fo);
    out[n] = '\0';
    fclose(fo);
    fclose(fe);
}

static void tmpdir(char *out, size_t cap)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, cap, "%s/pc-cli-orient-%d", tmp ? tmp : "/tmp", (int)getpid());
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    int rc = system(rm);
    (void)rc;
    (void)mkdir(out, 0755);
}

static void rmrf(const char *dir)
{
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    int rc = system(rm);
    (void)rc;
}

static void upright_color(int x, int y, uint8_t out[3])
{
    if (y < 16) {
        if (x < 8) {
            out[0] = 230; out[1] = 20; out[2] = 20; /* red */
        } else {
            out[0] = 20; out[1] = 230; out[2] = 20; /* green */
        }
    } else {
        out[0] = 20; out[1] = 20; out[2] = 230; /* blue */
    }
}

/* EXIF block: Orientation = o (SHORT) and an inline Software tag, little-endian. */
static size_t exif_block(uint16_t o, uint8_t *v)
{
    size_t i = 0;
    memcpy(v + i, "II*\0\x08\0\0\0\x02\0", 10);
    i += 10;
    uint8_t e1[8] = {0x12, 0x01, 3, 0, 1, 0, 0, 0};
    memcpy(v + i, e1, 8);
    i += 8;
    v[i++] = (uint8_t)(o & 0xff);
    v[i++] = (uint8_t)(o >> 8);
    v[i++] = 0;
    v[i++] = 0;
    uint8_t e2[8] = {0x31, 0x01, 2, 0, 4, 0, 0, 0};
    memcpy(v + i, e2, 8);
    i += 8;
    memcpy(v + i, "PC1\0", 4);
    i += 4;
    memset(v + i, 0, 4);
    i += 4;
    return i;
}

/* The phone JPEG: 32x16 stored landscape with an Orientation=6 EXIF segment. */
static void write_phone_jpeg(const char *path)
{
    int w = 16, h = 32; /* upright dims */
    size_t n = (size_t)w * (size_t)h * 3;
    uint8_t *px = calloc(1, n);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sx = y, sy = w - 1 - x; /* stored coords in a 32-wide image */
            uint8_t c[3];
            upright_color(x, y, c);
            memcpy(px + ((size_t)sy * (size_t)h + (size_t)sx) * 3, c, 3);
        }
    }
    pc_cimage stored;
    CHECK(pc_cimage_from_raw((uint32_t)h, (uint32_t)w, PC_CL_RGB, PC_CS_U8, px, n, &stored, NULL) == PC_OK);
    pc_cencode_opts opts = pc_cencode_opts_default();
    opts.jpeg_quality = 100;
    opts.jpeg_chroma_subsampling = false;
    pc_buf jpeg = PC_BUF_INIT;
    CHECK(pc_cencode(&stored, PC_CF_JPEG, &opts, &jpeg, NULL) == PC_OK);
    pc_cimage_fini(&stored);

    uint8_t exifbytes[64];
    size_t exn = exif_block(6, exifbytes);
    /* seg = "Exif\0\0" + exif. */
    pc_buf seg = PC_BUF_INIT;
    (void)pc_buf_append(&seg, "Exif\0\0", 6, NULL);
    (void)pc_buf_append(&seg, exifbytes, exn, NULL);
    pc_buf out = PC_BUF_INIT;
    (void)pc_buf_append(&out, jpeg.data, 2, NULL); /* SOI */
    uint8_t app1[4];
    app1[0] = 0xFF;
    app1[1] = 0xE1;
    uint16_t len = (uint16_t)(seg.len + 2);
    app1[2] = (uint8_t)(len >> 8);
    app1[3] = (uint8_t)(len & 0xff);
    (void)pc_buf_append(&out, app1, 4, NULL);
    (void)pc_buf_append(&out, seg.data, seg.len, NULL);
    (void)pc_buf_append(&out, jpeg.data + 2, jpeg.len - 2, NULL);
    FILE *f = fopen(path, "wb");
    if (f) {
        (void)fwrite(out.data, 1, out.len, f);
        fclose(f);
    }
    pc_buf_fini(&seg);
    pc_buf_fini(&out);
    pc_buf_fini(&jpeg);
}

/* Decode a file keeping stored orientation, into *img. */
static bool decode_stored(const char *path, pc_cimage *img)
{
    pc_buf b = PC_BUF_INIT;
    if (pc_file_read(path, (size_t)64 << 20, &b, NULL) != PC_OK) {
        pc_buf_fini(&b);
        return false;
    }
    pc_cdecode_opts o = pc_cdecode_opts_default();
    o.keep_orientation = true;
    bool ok = pc_cdecode_with(b.data, b.len, &o, img, NULL) == PC_OK;
    pc_buf_fini(&b);
    return ok;
}

static void assert_portrait_pixels(pc_cimage *img)
{
    pc_cimage rgb;
    CHECK(pc_cimage_convert(img, PC_CL_RGB, PC_CS_U8, &rgb, NULL) == PC_OK);
    int pts[4][2] = {{3, 4}, {12, 4}, {3, 28}, {12, 28}};
    for (int k = 0; k < 4; k++) {
        int x = pts[k][0], y = pts[k][1];
        size_t i = ((size_t)y * 16 + (size_t)x) * 3;
        uint8_t want[3];
        upright_color(x, y, want);
        for (int c = 0; c < 3; c++) {
            int d = (int)rgb.data[i + (size_t)c] - (int)want[c];
            CHECK(d <= 12 && d >= -12);
        }
    }
    pc_cimage_fini(&rgb);
}

static void dims(const char *path, uint32_t *w, uint32_t *h, bool keep)
{
    pc_cimage img;
    *w = *h = 0;
    if (keep ? decode_stored(path, &img) : false) {
        *w = img.width;
        *h = img.height;
        pc_cimage_fini(&img);
        return;
    }
    pc_buf b = PC_BUF_INIT;
    if (pc_file_read(path, (size_t)64 << 20, &b, NULL) == PC_OK) {
        if (pc_cdecode(b.data, b.len, &img, NULL) == PC_OK) {
            *w = img.width;
            *h = img.height;
            pc_cimage_fini(&img);
        }
    }
    pc_buf_fini(&b);
}

static void test_orientation_6_jpeg(void)
{
    char d[600];
    tmpdir(d, sizeof d);
    char jpg[700], png[700], turned[700];
    snprintf(jpg, sizeof jpg, "%s/IMG_0006.JPG", d);
    snprintf(png, sizeof png, "%s/upright.png", d);
    snprintf(turned, sizeof turned, "%s/turned.png", d);
    write_phone_jpeg(jpg);

    /* convert -> PNG: stored pixels are already upright, any EXIF says 1. */
    run((char *[]){"convert", jpg, png}, 3);
    {
        pc_cimage raw;
        CHECK(decode_stored(png, &raw));
        CHECK_EQ_UINT(raw.width, 16u);
        CHECK_EQ_UINT(raw.height, 32u);
        assert_portrait_pixels(&raw);
        if (raw.meta.exif.has) {
            CHECK_EQ_UINT(pc_exif_orientation(raw.meta.exif.data, raw.meta.exif.len), 1u);
        }
        pc_cimage_fini(&raw);
        uint32_t w, h;
        dims(png, &w, &h, false);
        CHECK_EQ_UINT(w, 16u); /* no second turn on reopen */
        CHECK_EQ_UINT(h, 32u);
    }

    /* An explicit Image Rotation 90 CW makes it landscape again. */
    run((char *[]){"run", jpg, "--cmd", "image.imageRotation.90cw", "--out", turned}, 6);
    {
        pc_cimage t;
        CHECK(decode_stored(turned, &t));
        CHECK_EQ_UINT(t.width, 32u);
        CHECK_EQ_UINT(t.height, 16u);
        pc_cimage rgb;
        CHECK(pc_cimage_convert(&t, PC_CL_RGB, PC_CS_U8, &rgb, NULL) == PC_OK);
        /* The portrait's top-left (red) is now top-right; its bottom (blue) on the left. */
        struct {
            int x, y, ux, uy;
        } checks[3] = {{28, 3, 3, 4}, {28, 12, 12, 4}, {4, 8, 8, 28}};
        for (int k = 0; k < 3; k++) {
            size_t i = ((size_t)checks[k].y * 32 + (size_t)checks[k].x) * 3;
            uint8_t want[3];
            upright_color(checks[k].ux, checks[k].uy, want);
            for (int c = 0; c < 3; c++) {
                int diff = (int)rgb.data[i + (size_t)c] - (int)want[c];
                CHECK(diff <= 12 && diff >= -12);
            }
        }
        pc_cimage_fini(&rgb);
        pc_cimage_fini(&t);
    }

    /* .pcraft save, reopen and save again: identical document and pixels. */
    char a[700], b[700], pa[700], pb[700];
    snprintf(a, sizeof a, "%s/a.pcraft", d);
    snprintf(b, sizeof b, "%s/b.pcraft", d);
    snprintf(pa, sizeof pa, "%s/a.png", d);
    snprintf(pb, sizeof pb, "%s/b.png", d);
    run((char *[]){"convert", jpg, a}, 3);
    run((char *[]){"convert", a, b}, 3);
    {
        char ja[1 << 16], jb[1 << 16];
        run_out((char *[]){"info", a}, 2, ja, sizeof ja);
        run_out((char *[]){"info", b}, 2, jb, sizeof jb);
        yyjson_doc *da = NULL, *db = NULL;
        CHECK(pc_json_parse(ja, strlen(ja), &da, NULL) == PC_OK);
        CHECK(pc_json_parse(jb, strlen(jb), &db, NULL) == PC_OK);
        yyjson_val *ra = yyjson_doc_get_root(da), *rb = yyjson_doc_get_root(db);
        uint64_t w = 0, hh = 0;
        CHECK(pc_json_as_u64(pc_json_get(ra, "width"), &w) && w == 16);
        CHECK(pc_json_as_u64(pc_json_get(ra, "height"), &hh) && hh == 32);
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(ra, "name"), NULL), "a.pcraft");
        CHECK_EQ_STR(pc_json_as_str(pc_json_get(rb, "name"), NULL), "b.pcraft");
        yyjson_doc_free(da);
        yyjson_doc_free(db);
    }
    run((char *[]){"convert", a, pa}, 3);
    run((char *[]){"convert", b, pb}, 3);
    {
        pc_cimage xa, xb;
        CHECK(decode_stored(pa, &xa));
        CHECK(decode_stored(pb, &xb));
        CHECK_EQ_UINT(xa.width, 16u);
        CHECK_EQ_UINT(xa.height, 32u);
        uint8_t *ra = NULL, *rb = NULL;
        size_t na = 0, nb = 0;
        CHECK(pc_cimage_to_rgba8(&xa, &ra, &na, NULL) == PC_OK);
        CHECK(pc_cimage_to_rgba8(&xb, &rb, &nb, NULL) == PC_OK);
        CHECK(na == nb && ra && rb && memcmp(ra, rb, na) == 0);
        /* The .pcraft round trip matches the direct conversion. */
        pc_cimage up;
        CHECK(decode_stored(png, &up));
        uint8_t *ru = NULL;
        size_t nu = 0;
        CHECK(pc_cimage_to_rgba8(&up, &ru, &nu, NULL) == PC_OK);
        CHECK(nu == na && ru && memcmp(ru, ra, na) == 0);
        free(ra);
        free(rb);
        free(ru);
        pc_cimage_fini(&xa);
        pc_cimage_fini(&xb);
        pc_cimage_fini(&up);
    }
    rmrf(d);
}

int main(int argc, char **argv)
{
    RUN(test_orientation_6_jpeg);
    return pc_t_finish();
}
