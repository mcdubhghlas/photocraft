/* Decode warnings: port of crates/codecs/tests/decode_warnings.rs. A file that holds more than was
 * decoded (#523: animations, multi-page TIFF) or less than it should (#518: truncated JPEG) says
 * so; complete single images don't. */
#include <stdio.h>

#include <jpeglib.h>
#include <zlib.h>

#include "common.h"
#include "codecs/orientation.h"
#include "codecs/web.h"
#include "tiffgen.h"

#define W 24u
#define H 16u

static const uint8_t COLOURS[3][3] = {{200, 30, 10}, {10, 200, 30}, {30, 10, 200}};

static pc_cimage solid(const uint8_t rgb[3])
{
    uint8_t px[W * H * 3];
    pc_cimage img;
    size_t i;
    for (i = 0; i < W * H; i++) {
        memcpy(px + 3 * i, rgb, 3);
    }
    if (pc_cimage_from_copy(W, H, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, NULL) != PC_OK) {
        PC_T_FAIL("solid image");
        pc_cimage_init(&img);
    }
    return img;
}

/* The warnings of img equal want[0..n] exactly (Rust's `assert_eq!(img.warnings, [...])`). */
static void check_warnings_at(const pc_cimage *img, const pc_cwarning *want, size_t n, const char *what, int line)
{
    bool ok = img->nwarnings == n;
    size_t i;
    pc_t_checks++;
    for (i = 0; ok && i < n; i++) {
        ok = pc_cwarning_eq(&img->warnings[i], &want[i]);
    }
    if (!ok) {
        char got[160] = "";
        if (img->nwarnings > 0) {
            pc_cwarning_text(&img->warnings[0], got, sizeof got);
        }
        PC_T_FAIL("line %d %s: %zu warnings (want %zu), first: %s", line, what, img->nwarnings, n, got);
    }
}
#define CHECK_WARNINGS(img, want, n, what) check_warnings_at((img), (want), (n), (what), __LINE__)

static const pc_cwarning TRUNCATED = {PC_CW_TRUNCATED, false, 0, PC_CF_JPEG, 0};

static pc_cwarning more_frames(uint32_t n)
{
    pc_cwarning w = {PC_CW_MORE_FRAMES, true, n, 0, 0};
    return w;
}

static pc_cwarning more_pages(bool has, uint32_t n)
{
    pc_cwarning w = {PC_CW_MORE_PAGES, has, n, 0, 0};
    return w;
}

/* Decodes b[0..n] and checks its warnings; an error is a failure. */
static void check_dec_at(const uint8_t *b, size_t n, const pc_cwarning *want, size_t nw, const char *what, int line)
{
    pc_cimage img = dec(b, n);
    check_warnings_at(&img, want, nw, what, line);
    pc_cimage_fini(&img);
}
#define CHECK_DEC(b, n, want, nw, what) check_dec_at((b), (n), (want), (nw), (what), __LINE__)

/* ---------------------------------------------------------------------------
 * #518: truncated JPEG
 * --------------------------------------------------------------------------- */

/* Rust used the jpeg-encoder crate; libjpeg-turbo writes the fixture here. */
static pc_buf jpeg(pc_clayout layout, bool progressive)
{
    pc_cimage img = synth(64, 48, layout, PC_CS_U8, 3, 0.3f);
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr jerr;
    unsigned char *mem = NULL;
    unsigned long mem_len = 0;
    int nc = (int)pc_clayout_channels(layout);
    pc_buf out = PC_BUF_INIT;
    c.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &mem_len);
    c.image_width = 64;
    c.image_height = 48;
    c.input_components = nc;
    c.in_color_space = layout == PC_CL_GRAY ? JCS_GRAYSCALE : layout == PC_CL_CMYK ? JCS_CMYK : JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 90, TRUE);
    if (progressive) {
        jpeg_simple_progression(&c);
    }
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = img.data + (size_t)c.next_scanline * 64 * (size_t)nc;
        (void)jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    (void)pc_buf_append(&out, mem, (size_t)mem_len, NULL);
    free(mem);
    pc_cimage_fini(&img);
    return out;
}

/* Offset of the first byte of entropy-coded data (just past the first SOS header). */
static size_t scan_start(const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i + 3 < n; i++) {
        if (b[i] == 0xFF && b[i + 1] == 0xDA) {
            return i + 2 + (((size_t)b[i + 2] << 8) | b[i + 3]);
        }
    }
    PC_T_FAIL("no SOS marker");
    return 0;
}

static void test_truncated_jpeg_decodes_with_a_warning(void)
{
    static const pc_clayout LAYOUTS[3] = {PC_CL_GRAY, PC_CL_RGB, PC_CL_CMYK};
    size_t li;
    int progressive;
    for (li = 0; li < 3; li++) {
        for (progressive = 0; progressive < 2; progressive++) {
            pc_buf full = jpeg(LAYOUTS[li], progressive != 0);
            char what[64];
            pc_cimage half;
            size_t start;
            (void)snprintf(what, sizeof what, "%s progressive %d", pc_clayout_name(LAYOUTS[li]), progressive);
            CHECK_DEC(full.data, full.len, NULL, 0, what);
            half = dec(full.data, full.len / 2);
            CHECK_WARNINGS(&half, &TRUNCATED, 1, what);
            CHECK_EQ_UINT(half.width, 64);
            CHECK_EQ_UINT(half.height, 48);
            pc_cimage_fini(&half);
            /* One byte short: the end-of-image marker is incomplete. */
            CHECK_DEC(full.data, full.len - 1, &TRUNCATED, 1, what);
            /* A few bytes of scan data still decode, with the warning. */
            start = scan_start(full.data, full.len);
            CHECK_DEC(full.data, start + 8, &TRUNCATED, 1, what);
            pc_buf_fini(&full);
        }
    }
}

static void test_truncated_jpeg_message_names_the_problem(void)
{
    char s[256];
    pc_cwarning_text(&TRUNCATED, s, sizeof s);
    CHECK(strncmp(s, "JPEG data ends early", 20) == 0);
    CHECK_CONTAINS(s, "truncated");
}

static void test_jpeg_without_any_scan_data_is_an_error(void)
{
    pc_buf full = jpeg(PC_CL_RGB, false);
    size_t start = scan_start(full.data, full.len);
    size_t cuts[3];
    size_t i;
    cuts[0] = start - 3;
    cuts[1] = start - 1;
    cuts[2] = start;
    for (i = 0; i < 3; i++) {
        pc_cimage img;
        pc_error e;
        memset(&e, 0, sizeof e);
        CHECK(pc_cdecode(full.data, cuts[i], &img, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "before any image data");
    }
    pc_buf_fini(&full);
}

static void test_data_after_the_end_of_image_is_not_truncation(void)
{
    static const uint8_t TRAIL[] = "\0\0trailing bytes \xFF\xD8\xFF";
    pc_buf b = jpeg(PC_CL_RGB, false);
    (void)pc_buf_append(&b, TRAIL, sizeof TRAIL - 1, NULL);
    CHECK_DEC(b.data, b.len, NULL, 0, "trailing data");
    pc_buf_fini(&b);
}

static void test_an_end_marker_inside_metadata_does_not_hide_truncation(void)
{
    /* An EXIF block holding FF D9 (as an embedded thumbnail does) must be skipped as a segment. */
    static const uint8_t THUMB[5] = {0xFF, 0xD8, 0x00, 0xFF, 0xD9};
    pc_cimage img = synth(64, 48, PC_CL_RGB, PC_CS_U8, 3, 0.3f);
    pc_buf exif = sample_exif();
    pc_buf full;
    (void)pc_buf_append(&exif, THUMB, sizeof THUMB, NULL);
    set_blob(&img.meta.exif, exif.data, exif.len);
    full = enc(&img, PC_CF_JPEG, NULL);
    CHECK(find_bytes(full.data, full.len, THUMB, sizeof THUMB));
    CHECK_DEC(full.data, full.len, NULL, 0, "full");
    CHECK_DEC(full.data, full.len * 2 / 3, &TRUNCATED, 1, "two thirds");
    pc_buf_fini(&full);
    pc_buf_fini(&exif);
    pc_cimage_fini(&img);
}

static void test_restart_markers_and_fill_bytes_are_scan_data(void)
{
    /* Restart markers (FF D0-D7) and fill bytes (FF FF) inside a scan don't end it. */
    static const uint8_t SCAN[11] = {0x12, 0xFF, 0x00, 0x34, 0xFF, 0xD0, 0x56, 0xFF, 0xFF, 0xD7, 0x78};
    static const uint8_t END[3] = {0xFF, 0xFF, 0xD9};
    pc_buf full = jpeg(PC_CL_RGB, false);
    size_t start = scan_start(full.data, full.len);
    pc_buf b = PC_BUF_INIT;
    (void)pc_buf_append(&b, full.data, start, NULL);
    (void)pc_buf_append(&b, SCAN, sizeof SCAN, NULL);
    CHECK_DEC(b.data, b.len, &TRUNCATED, 1, "open scan");
    (void)pc_buf_append(&b, END, sizeof END, NULL);
    CHECK_DEC(b.data, b.len, NULL, 0, "closed scan");
    pc_buf_fini(&b);
    pc_buf_fini(&full);
}

/* ---------------------------------------------------------------------------
 * #523: only the first frame or page is decoded
 * --------------------------------------------------------------------------- */

/* Rust used the `image` crate's GifEncoder; web.h's animated GIF writer builds it here (no loop
 * extension, each frame a one-entry colour table). */
static pc_buf gif(size_t frames)
{
    static uint8_t idx[W * H];
    pc_gif_frame f[3];
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    size_t i;
    memset(idx, 0, sizeof idx);
    for (i = 0; i < frames && i < 3; i++) {
        f[i].indices = idx;
        f[i].n = sizeof idx;
        f[i].palette = &COLOURS[i];
        f[i].npal = 1;
        f[i].transparent = -1;
        f[i].delay_cs = 0;
    }
    if (pc_web_encode_gif_animated(W, H, f, frames, false, &out, &e) != PC_OK) {
        PC_T_FAIL("gif: %s", e.msg);
    }
    return out;
}

static bool same_rgba8(const pc_cimage *a, const pc_cimage *b)
{
    uint8_t *pa = NULL;
    uint8_t *pb = NULL;
    size_t na = 0;
    size_t nb = 0;
    bool same;
    (void)pc_cimage_to_rgba8(a, &pa, &na, NULL);
    (void)pc_cimage_to_rgba8(b, &pb, &nb, NULL);
    same = na == nb && (na == 0 || memcmp(pa, pb, na) == 0);
    free(pa);
    free(pb);
    return same;
}

static void test_animated_gif_warns_and_keeps_the_first_frame(void)
{
    pc_buf b1 = gif(1);
    pc_cimage one = dec(b1.data, b1.len);
    size_t n;
    pc_cimage s;
    pc_buf ours;
    CHECK_WARNINGS(&one, NULL, 0, "one frame");
    for (n = 2; n <= 3; n++) {
        pc_buf b = gif(n);
        pc_cimage img = dec(b.data, b.len);
        pc_cwarning w = more_frames((uint32_t)n);
        CHECK_WARNINGS(&img, &w, 1, "frames");
        CHECK(same_rgba8(&img, &one));
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    /* A single-frame GIF written by our own encoder. */
    s = solid(COLOURS[0]);
    ours = enc(&s, PC_CF_GIF, NULL);
    CHECK_DEC(ours.data, ours.len, NULL, 0, "our GIF");
    pc_buf_fini(&ours);
    pc_cimage_fini(&s);
    pc_cimage_fini(&one);
    pc_buf_fini(&b1);
}

static void test_gif_without_a_trailer_warns_without_a_total(void)
{
    pc_buf b = gif(3);
    pc_cimage img;
    pc_cwarning w = {PC_CW_MORE_FRAMES, false, 0, 0, 0};
    char s[256] = "";
    CHECK(b.len > 0 && b.data[b.len - 1] == 0x3B);
    img = dec(b.data, b.len - 1);
    CHECK_WARNINGS(&img, &w, 1, "no trailer");
    if (img.nwarnings > 0) {
        pc_cwarning_text(&img.warnings[0], s, sizeof s);
    }
    CHECK(strcmp(s, "only the first frame of the animation was imported") == 0);
    pc_cimage_fini(&img);
    pc_buf_fini(&b);
}

static void be32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    (void)pc_buf_append(b, x, 4, NULL);
}

static void png_chunk(pc_buf *out, const char *id, const uint8_t *data, size_t n)
{
    uLong crc = crc32(0, (const Bytef *)id, 4);
    if (n > 0) {
        crc = crc32(crc, data, (uInt)n);
    }
    be32(out, (uint32_t)n);
    (void)pc_buf_append(out, id, 4, NULL);
    if (n > 0) {
        (void)pc_buf_append(out, data, n, NULL);
    }
    be32(out, (uint32_t)crc);
}

static void fctl(pc_buf *out, uint32_t *seq)
{
    pc_buf d = PC_BUF_INIT;
    static const uint8_t TAIL[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* x, y, delay, dispose, blend */
    be32(&d, (*seq)++);
    be32(&d, W);
    be32(&d, H);
    (void)pc_buf_append(&d, TAIL, sizeof TAIL, NULL);
    png_chunk(out, "fcTL", d.data, d.len);
    pc_buf_fini(&d);
}

/* An APNG of `frames` solid frames at 8 or 16 bits; with separate_default the IDAT image is an
 * extra default image outside the animation. Rust used the png crate's APNG writer; the chunks
 * (acTL, fcTL, IDAT, fdAT) are written by hand here. */
static pc_buf apng(uint32_t frames, bool sixteen, bool separate_default)
{
    static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    uint32_t images = frames + (separate_default ? 1u : 0u);
    size_t bps = sixteen ? 2 : 1;
    size_t row = 1 + W * 3 * bps;
    uint8_t *raw = malloc(row * H);
    uint8_t *z = malloc(compressBound((uLong)(row * H)));
    pc_buf out = PC_BUF_INIT;
    pc_buf hdr = PC_BUF_INIT;
    uint32_t seq = 0;
    uint32_t k;
    uint8_t tail[5] = {sixteen ? 16 : 8, 2, 0, 0, 0};
    (void)pc_buf_append(&out, SIG, 8, NULL);
    be32(&hdr, W);
    be32(&hdr, H);
    (void)pc_buf_append(&hdr, tail, 5, NULL);
    png_chunk(&out, "IHDR", hdr.data, hdr.len);
    pc_buf_fini(&hdr);
    if (frames > 0) {
        pc_buf a = PC_BUF_INIT;
        be32(&a, frames);
        be32(&a, 0);
        png_chunk(&out, "acTL", a.data, a.len);
        pc_buf_fini(&a);
    }
    for (k = 0; k < (images > 0 ? images : 1); k++) {
        const uint8_t *c = COLOURS[k % 3];
        uLongf zn = compressBound((uLong)(row * H));
        size_t y;
        size_t x;
        for (y = 0; y < H; y++) {
            uint8_t *r = raw + y * row;
            r[0] = 0;
            for (x = 0; x < W * 3; x++) {
                if (sixteen) {
                    r[1 + 2 * x] = c[x % 3];
                    r[2 + 2 * x] = c[x % 3];
                } else {
                    r[1 + x] = c[x % 3];
                }
            }
        }
        (void)compress(z, &zn, raw, (uLong)(row * H));
        if (k == 0) {
            if (frames > 0 && !separate_default) {
                fctl(&out, &seq);
            }
            png_chunk(&out, "IDAT", z, (size_t)zn);
        } else {
            pc_buf d = PC_BUF_INIT;
            fctl(&out, &seq);
            be32(&d, seq++);
            (void)pc_buf_append(&d, z, (size_t)zn, NULL);
            png_chunk(&out, "fdAT", d.data, d.len);
            pc_buf_fini(&d);
        }
    }
    png_chunk(&out, "IEND", NULL, 0);
    free(raw);
    free(z);
    return out;
}

static void test_apng_warns_and_keeps_the_first_frame(void)
{
    int sixteen;
    for (sixteen = 0; sixteen < 2; sixteen++) {
        pc_buf bs = apng(0, sixteen != 0, false);
        pc_buf b1 = apng(1, sixteen != 0, false);
        pc_buf b3 = apng(3, sixteen != 0, false);
        pc_buf bd = apng(2, sixteen != 0, true);
        pc_cimage still = dec(bs.data, bs.len);
        pc_cimage img;
        pc_cwarning w3 = more_frames(3);
        CHECK_WARNINGS(&still, NULL, 0, "still");
        CHECK_DEC(b1.data, b1.len, NULL, 0, "a one-frame APNG is a still");
        img = dec(b3.data, b3.len);
        CHECK_WARNINGS(&img, &w3, 1, "3 frames");
        CHECK(img.sample == still.sample && img.len == still.len && img.len > 0 &&
              memcmp(img.data, still.data, img.len) == 0);
        pc_cimage_fini(&img);
        /* Default image + 2 animation frames: three images, the default one kept. */
        img = dec(bd.data, bd.len);
        CHECK_WARNINGS(&img, &w3, 1, "separate default");
        CHECK(img.len == still.len && img.len > 0 && memcmp(img.data, still.data, img.len) == 0);
        pc_cimage_fini(&img);
        pc_cimage_fini(&still);
        pc_buf_fini(&bs);
        pc_buf_fini(&b1);
        pc_buf_fini(&b3);
        pc_buf_fini(&bd);
    }
}

static void le32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    (void)pc_buf_append(b, x, 4, NULL);
}

static void riff_chunk(pc_buf *out, const char *id, const uint8_t *data, size_t n)
{
    static const uint8_t ZERO = 0;
    (void)pc_buf_append(out, id, 4, NULL);
    le32(out, (uint32_t)n);
    (void)pc_buf_append(out, data, n, NULL);
    if (n % 2 == 1) {
        (void)pc_buf_append(out, &ZERO, 1, NULL);
    }
}

static void u24(pc_buf *b, uint32_t v)
{
    uint8_t x[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
    (void)pc_buf_append(b, x, 3, NULL);
}

/* An animated WebP: each frame is a full-canvas lossless still wrapped in an ANMF chunk. */
static pc_buf animated_webp(const pc_cimage *frames, size_t n)
{
    static const uint8_t VP8X_FLAGS[4] = {0x02, 0, 0, 0}; /* animation flag */
    static const uint8_t ANIM[6] = {0, 0, 0, 0, 0, 0};
    static const uint8_t XY[6] = {0, 0, 0, 0, 0, 0};
    pc_buf body = PC_BUF_INIT;
    pc_buf vp8x = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    size_t i;
    (void)pc_buf_append(&body, "WEBP", 4, NULL);
    (void)pc_buf_append(&vp8x, VP8X_FLAGS, 4, NULL);
    u24(&vp8x, W - 1);
    u24(&vp8x, H - 1);
    riff_chunk(&body, "VP8X", vp8x.data, vp8x.len);
    riff_chunk(&body, "ANIM", ANIM, sizeof ANIM);
    for (i = 0; i < n; i++) {
        pc_buf still = enc(&frames[i], PC_CF_WEBP, NULL);
        pc_buf anmf = PC_BUF_INIT;
        size_t at;
        size_t len;
        uint8_t zero = 0;
        for (at = 0; at + 8 <= still.len && memcmp(still.data + at, "VP8L", 4) != 0; at++) {
        }
        if (at + 8 > still.len) {
            PC_T_FAIL("no VP8L chunk");
            pc_buf_fini(&still);
            continue;
        }
        len = (size_t)still.data[at + 4] | ((size_t)still.data[at + 5] << 8) | ((size_t)still.data[at + 6] << 16) |
              ((size_t)still.data[at + 7] << 24);
        (void)pc_buf_append(&anmf, XY, sizeof XY, NULL); /* frame x, y */
        u24(&anmf, W - 1);
        u24(&anmf, H - 1);
        u24(&anmf, 100); /* duration */
        (void)pc_buf_append(&anmf, &zero, 1, NULL);
        riff_chunk(&anmf, "VP8L", still.data + at + 8, len);
        riff_chunk(&body, "ANMF", anmf.data, anmf.len);
        pc_buf_fini(&anmf);
        pc_buf_fini(&still);
    }
    (void)pc_buf_append(&out, "RIFF", 4, NULL);
    le32(&out, (uint32_t)body.len);
    (void)pc_buf_append(&out, body.data, body.len, NULL);
    pc_buf_fini(&body);
    pc_buf_fini(&vp8x);
    return out;
}

static void test_animated_webp_warns_and_keeps_the_first_frame(void)
{
    pc_cimage frames[3];
    pc_buf sb;
    pc_cimage still;
    pc_buf b1;
    size_t n;
    size_t i;
    for (i = 0; i < 3; i++) {
        frames[i] = solid(COLOURS[i]);
    }
    sb = enc(&frames[0], PC_CF_WEBP, NULL);
    still = dec(sb.data, sb.len);
    CHECK_WARNINGS(&still, NULL, 0, "still");
    b1 = animated_webp(frames, 1);
    CHECK_DEC(b1.data, b1.len, NULL, 0, "a one-frame animation");
    for (n = 2; n <= 3; n++) {
        pc_buf b = animated_webp(frames, n);
        pc_cimage img = dec(b.data, b.len);
        pc_cwarning w = more_frames((uint32_t)n);
        uint8_t *pa = NULL;
        uint8_t *pb = NULL;
        size_t na = 0;
        size_t nb = 0;
        bool close;
        CHECK_WARNINGS(&img, &w, 1, "frames");
        /* The animation decoder composites the frame onto its canvas: within one level. */
        (void)pc_cimage_to_rgba8(&img, &pa, &na, NULL);
        (void)pc_cimage_to_rgba8(&still, &pb, &nb, NULL);
        close = na == nb && na > 0;
        for (i = 0; close && i < na; i++) {
            close = abs((int)pa[i] - (int)pb[i]) <= 1;
        }
        CHECK(close);
        free(pa);
        free(pb);
        pc_cimage_fini(&img);
        pc_buf_fini(&b);
    }
    pc_buf_fini(&b1);
    pc_cimage_fini(&still);
    pc_buf_fini(&sb);
    for (i = 0; i < 3; i++) {
        pc_cimage_fini(&frames[i]);
    }
}

/* A TIFF with one IFD per entry: (NewSubfileType, sample type). Each page is a flat grey at its
 * own level, written at the sample depth. Rust used the tiff crate's encoder; tiffgen.h builds
 * it here (little-endian, one strip per page). */
static tg_built tiff(const uint32_t *kinds, const pc_csample *samples, size_t n)
{
    tg_dir dirs[3];
    size_t chain[3] = {0, 1, 2};
    size_t k;
    size_t i;
    tg_built b;
    for (k = 0; k < n && k < 3; k++) {
        uint8_t v = (uint8_t)(40 * k + 10);
        size_t np = (size_t)W * H;
        uint8_t *data;
        size_t len;
        uint16_t bits;
        if (samples[k] == PC_CS_U8) {
            data = malloc(np);
            memset(data, v, np);
            len = np;
            bits = 8;
        } else if (samples[k] == PC_CS_U16) {
            uint16_t *s = malloc(np * 2);
            for (i = 0; i < np; i++) {
                s[i] = (uint16_t)(v * 257);
            }
            data = tg_u16s(s, np, true);
            free(s);
            len = 2 * np;
            bits = 16;
        } else {
            float *s = malloc(np * 4);
            for (i = 0; i < np; i++) {
                s[i] = (float)v / 255.0f;
            }
            data = tg_f32s(s, np, true);
            free(s);
            len = 4 * np;
            bits = 32;
        }
        dirs[k] = tg_strips(W, H, bits, 1, 1, H, data, len);
        if (bits == 32) {
            tg_tag1(&dirs[k], 339, TG_SHORT, 3);
        }
        if (kinds[k] != 0) {
            tg_tag1(&dirs[k], 254, TG_LONG, kinds[k]);
        }
        free(data);
    }
    b = tg_build(true, false, dirs, n, chain, n);
    for (k = 0; k < n; k++) {
        tg_dir_free(&dirs[k]);
    }
    return b;
}

static void test_multi_page_tiff_warns_and_keeps_the_first_page(void)
{
    static const pc_csample SAMPLES[3] = {PC_CS_U8, PC_CS_U16, PC_CS_F32};
    static const uint32_t K0[3] = {0, 0, 0};
    static const uint32_t K02[2] = {0, 2};
    size_t si;
    for (si = 0; si < 3; si++) {
        pc_csample s[3] = {SAMPLES[si], SAMPLES[si], SAMPLES[si]};
        const char *what = pc_csample_name(SAMPLES[si]);
        tg_built b1 = tiff(K0, s, 1);
        tg_built b2 = tiff(K02, s, 2);
        tg_built b3 = tiff(K0, s, 3);
        pc_cimage one = dec(b1.bytes, b1.len);
        pc_cimage img;
        pc_cwarning w2 = more_pages(true, 2);
        pc_cwarning w3 = more_pages(true, 3);
        char text[128] = "";
        CHECK_WARNINGS(&one, NULL, 0, what);
        img = dec(b2.bytes, b2.len);
        CHECK_WARNINGS(&img, &w2, 1, what);
        CHECK(img.len == one.len && img.len > 0 && memcmp(img.data, one.data, img.len) == 0);
        pc_cimage_fini(&img);
        img = dec(b3.bytes, b3.len);
        CHECK_WARNINGS(&img, &w3, 1, what);
        if (img.nwarnings > 0) {
            pc_cwarning_text(&img.warnings[0], text, sizeof text);
        }
        CHECK(strcmp(text, "only the first of 3 pages was imported") == 0);
        pc_cimage_fini(&img);
        pc_cimage_fini(&one);
        tg_built_free(&b1);
        tg_built_free(&b2);
        tg_built_free(&b3);
    }
}

static void test_tiff_thumbnails_and_masks_are_not_pages(void)
{
    static const pc_csample U8[3] = {PC_CS_U8, PC_CS_U8, PC_CS_U8};
    static const uint32_t K01[2] = {0, 1};
    static const uint32_t K04[2] = {0, 4};
    static const uint32_t K010[3] = {0, 1, 0};
    pc_cwarning w2 = more_pages(true, 2);
    /* A reduced-resolution copy (bit 0) or a transparency mask (bit 2) is part of the page. */
    tg_built b = tiff(K01, U8, 2);
    CHECK_DEC(b.bytes, b.len, NULL, 0, "reduced");
    tg_built_free(&b);
    b = tiff(K04, U8, 2);
    CHECK_DEC(b.bytes, b.len, NULL, 0, "mask");
    tg_built_free(&b);
    b = tiff(K010, U8, 3);
    CHECK_DEC(b.bytes, b.len, &w2, 1, "page, reduced, page");
    tg_built_free(&b);
}

static void test_single_images_carry_no_warnings(void)
{
    int f;
    for (f = 0; f < PC_CF_COUNT; f++) {
        pc_cimage img;
        pc_cencode_opts o = pc_cencode_opts_default();
        pc_buf b = PC_BUF_INIT;
        if (!rw((pc_cformat)f)) {
            continue;
        }
        img = synth(9, 7, PC_CL_RGBA, PC_CS_U8, 5, 0.2f);
        if (pc_cencode(&img, (pc_cformat)f, &o, &b, NULL) == PC_OK) {
            CHECK_DEC(b.data, b.len, NULL, 0, pc_cformat_debug((pc_cformat)f));
        }
        pc_buf_fini(&b);
        pc_cimage_fini(&img);
    }
}

static void test_warnings_survive_orientation(void)
{
    pc_cimage img = synth(8, 4, PC_CL_RGB, PC_CS_U8, 1, 0.0f);
    pc_cwarning w = more_pages(false, 0);
    pc_cimage conv;
    uint16_t o;
    (void)pc_cimage_add_warning(&img, w, NULL);
    for (o = 1; o <= 8; o++) {
        pc_cimage c;
        pc_cimage out;
        CHECK(pc_cimage_clone(&img, &c, NULL) == PC_OK);
        if (pc_cimage_oriented(&c, o, &out, NULL) == PC_OK) {
            CHECK_WARNINGS(&out, &w, 1, "oriented");
            pc_cimage_fini(&out);
        } else {
            PC_T_FAIL("orientation %u", (unsigned)o);
        }
        pc_cimage_fini(&c);
    }
    if (pc_cimage_convert(&img, PC_CL_RGBA, PC_CS_U16, &conv, NULL) == PC_OK) {
        CHECK_WARNINGS(&conv, &w, 1, "converted");
        pc_cimage_fini(&conv);
    } else {
        PC_T_FAIL("convert");
    }
    pc_cimage_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_truncated_jpeg_decodes_with_a_warning);
    RUN(test_truncated_jpeg_message_names_the_problem);
    RUN(test_jpeg_without_any_scan_data_is_an_error);
    RUN(test_data_after_the_end_of_image_is_not_truncation);
    RUN(test_an_end_marker_inside_metadata_does_not_hide_truncation);
    RUN(test_restart_markers_and_fill_bytes_are_scan_data);
    RUN(test_animated_gif_warns_and_keeps_the_first_frame);
    RUN(test_gif_without_a_trailer_warns_without_a_total);
    RUN(test_apng_warns_and_keeps_the_first_frame);
    RUN(test_animated_webp_warns_and_keeps_the_first_frame);
    RUN(test_multi_page_tiff_warns_and_keeps_the_first_page);
    RUN(test_tiff_thumbnails_and_masks_are_not_pages);
    RUN(test_single_images_carry_no_warnings);
    RUN(test_warnings_survive_orientation);
    return pc_t_finish();
}
