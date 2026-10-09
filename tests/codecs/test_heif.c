/* HEIF / HEIC: ports of crates/codecs/tests/heif.rs and the unit tests in
 * crates/codecs/src/codecs/heif.rs. The corpus tests decode against Apple's own decoder: the
 * fixtures are in corpus/heif/ (`make corpus`); most come from heic-rs (macOS `sips` encodes of
 * synthetic PNGs, each .ref.png being Apple's decode of the .heic beside it), the 10-bit one from
 * pillow-heif with its source PNG. Corpus tests skip when corpus/ is absent (the reference fails).
 *
 * Decoding needs libheif with an HEVC decoder plugin. Without it (PC_NO_HEIF, or a libheif that
 * has no HEVC decoder, where HEIF is not readable) the reference's no-`heif`-feature test runs. */
#include "common.h"
#include "base/file.h"
#include "codecs/orientation.h"

#define F PC_CF_HEIF

/* HEIF is detected and refused with a clear Unsupported error when it can't be decoded. */
static void test_without_the_feature_heif_is_a_clear_unsupported_error(void)
{
    static const uint8_t header[24] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'h', 'e', 'i', 'c',
                                       0, 0, 0, 0,    'm', 'i', 'f', '1', 'h', 'e', 'i', 'c'};
    pc_cformat det;
    pc_cimage img;
    pc_error e;
    CHECK(pc_cformat_detect(header, sizeof header, &det) && det == F); /* still recognised */
    CHECK_EQ_INT(pc_cdecode(header, sizeof header, &img, &e), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "Heif: ");
#ifdef PC_NO_HEIF
    CHECK_CONTAINS(e.msg, "isn't included in this build");
#else
    /* libheif without an HEVC decoder: "this build's libheif has no HEVC decoder". */
    CHECK_CONTAINS(e.msg, "this build");
#endif
    CHECK(!pc_cformat_caps(F).read);
}

#ifndef PC_NO_HEIF

static pc_buf files[4];
static bool have_corpus;

/* A file of corpus/heif/ (empty when absent). */
static pc_buf corpus(const char *rel)
{
    char path[512];
    pc_buf b = PC_BUF_INIT;
    (void)snprintf(path, sizeof path, "corpus/heif/%s", rel);
    if (pc_file_read(path, (size_t)64 << 20, &b, NULL) != PC_OK) {
        pc_buf_fini(&b);
    }
    return b;
}

static bool load_files(void)
{
    static const char *const names[4] = {"heic-rs/checker-64.heic", "heic-rs/rgb-strips-96.heic",
                                         "heic-rs/checker-1024.heic", "heic-rs/with-exif.heic"};
    size_t i;
    have_corpus = true;
    for (i = 0; i < 4; i++) {
        files[i] = corpus(names[i]);
        have_corpus = have_corpus && files[i].len > 0;
    }
    if (!have_corpus) {
        (void)printf("(corpus/heif absent: run `make corpus`; corpus tests skipped) ");
    }
    return have_corpus;
}

#define CHECKER_64 (&files[0])
#define STRIPS_96 (&files[1])
#define CHECKER_GRID (&files[2]) /* iPhone-style grid of 512x512 tiles (2x2) */
#define WITH_EXIF (&files[3])

/* The test needs the corpus and an HEVC decoder. */
static bool ready(void) { return have_corpus && pc_cformat_caps(F).read; }

static pc_cimage reference(const char *rel)
{
    pc_buf png = corpus(rel);
    pc_cimage img;
    pc_cimage rgb;
    pc_error e;
    pc_cimage_init(&rgb);
    if (pc_cdecode_as(PC_CF_PNG, png.data, png.len, &img, &e) != PC_OK) {
        PC_T_FAIL("%s: %s", rel, e.msg);
    } else {
        if (pc_cimage_convert(&img, PC_CL_RGB, PC_CS_U8, &rgb, &e) != PC_OK) {
            PC_T_FAIL("convert: %s", e.msg);
        }
        pc_cimage_fini(&img);
    }
    pc_buf_fini(&png);
    return rgb;
}

/* A copy of b with the first irot box set to angle x 90 degrees anticlockwise (every fixture has
 * one, at 0). */
static pc_buf with_irot(const pc_buf *b, uint8_t angle)
{
    pc_buf out = PC_BUF_INIT;
    size_t i;
    (void)pc_buf_append(&out, b->data, b->len, NULL);
    for (i = 0; i + 4 < out.len; i++) {
        if (memcmp(out.data + i, "irot", 4) == 0) {
            out.data[i + 4] = angle;
            return out;
        }
    }
    PC_T_FAIL("no irot box");
    return out;
}

/* A copy of b with the big-endian EXIF Orientation entry (SHORT, count 1) set to o. */
static pc_buf with_exif_orientation(const pc_buf *b, uint8_t o)
{
    static const uint8_t entry[8] = {0x01, 0x12, 0, 3, 0, 0, 0, 1};
    pc_buf out = PC_BUF_INIT;
    size_t i;
    (void)pc_buf_append(&out, b->data, b->len, NULL);
    for (i = 0; i + 9 < out.len; i++) {
        if (memcmp(out.data + i, entry, 8) == 0) {
            out.data[i + 9] = o;
            return out;
        }
    }
    PC_T_FAIL("no EXIF Orientation entry");
    return out;
}

static pc_cdecode_opts keep(void)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    o.keep_orientation = true;
    return o;
}

static void test_heif_is_read_only_and_listed(void)
{
    static const char *const exts[4] = {"heic", "HEIC", "photo.heif", "IMG_0001.HIF"};
    pc_ccaps c = pc_cformat_caps(F);
    bool listed = false;
    size_t i;
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_cfidelity w[8];
    size_t nw;
    pc_error e;
    if (!ready()) {
        return;
    }
    CHECK(c.read && !c.write);
    for (i = 0; i < PC_ASYMMETRIC_EXCEPTIONS_LEN; i++) {
        listed = listed || PC_ASYMMETRIC_EXCEPTIONS[i].format == F;
    }
    CHECK(listed);
    for (i = 0; i < 4; i++) {
        pc_cformat f;
        if (!pc_cformat_from_extension(exts[i], &f) || f != F) {
            PC_T_FAIL("%s", exts[i]);
        }
    }
    img = dec(CHECKER_64->data, CHECKER_64->len);
    CHECK_EQ_INT(pc_cencode(&img, F, &o, &out, &e), PC_ERR_UNSUPPORTED);
    nw = pc_cfidelity_warnings(&img, F, w, 8);
    CHECK_EQ_UINT(nw, 1);
    CHECK(nw >= 1 && w[0].kind == PC_FW_WRITE_UNSUPPORTED && w[0].format == F);
    pc_cimage_fini(&img);
    pc_buf_fini(&out);
}

static void test_detect_heif_brands(void)
{
    size_t i;
    if (!have_corpus) {
        return;
    }
    for (i = 0; i < 4; i++) {
        pc_cformat det;
        CHECK(pc_cformat_detect(files[i].data, files[i].len, &det) && det == F);
    }
}

/* Max abs difference (normalised) and PSNR of two RGB U8 images, skipping columns 31 and 63. */
static float strips_max_diff(const pc_cimage *a, const pc_cimage *b, double *psnr_out)
{
    size_t i;
    size_t n = 0;
    double mse = 0.0;
    unsigned m = 0;
    for (i = 0; i < a->len && i < b->len; i++) {
        size_t x = (i / 3) % a->width;
        unsigned d;
        if (x == 31 || x == 63) {
            continue;
        }
        d = (unsigned)abs((int)a->data[i] - (int)b->data[i]);
        m = d > m ? d : m;
        mse += ((double)d / 255.0) * ((double)d / 255.0);
        n++;
    }
    mse /= (double)(n > 0 ? n : 1);
    *psnr_out = mse == 0.0 ? INFINITY : 10.0 * log10(1.0 / mse);
    return (float)m / 255.0f;
}

static void test_single_picture_matches_apple_decode(void)
{
    static const char *const refs[2] = {"heic-rs/checker-64.ref.png", "heic-rs/rgb-strips-96.ref.png"};
    size_t i;
    double p_strips = 0.0;
    if (!ready()) {
        return;
    }
    for (i = 0; i < 2; i++) {
        pc_cimage img = dec(files[i].data, files[i].len);
        pc_cimage want = reference(refs[i]);
        CHECK(img.layout == PC_CL_RGB && img.sample == PC_CS_U8);
        CHECK(img.width == want.width && img.height == want.height);
        if (img.layout == PC_CL_RGB && img.width == want.width && img.height == want.height) {
            /* Deviation: libheif upsamples 4:2:0 chroma nearest-neighbour, Apple (and heic-rs)
             * interpolate it, so the one column before each strip edge (x 31, 63) differs by up
             * to 186; the strips are compared without those columns. */
            float d = i == 1 ? strips_max_diff(&img, &want, &p_strips) : max_abs_diff(&img, &want);
            double p = i == 1 ? p_strips : psnr(&img, &want);
            pc_t_checks += 2;
            if (d > 3.0f / 255.0f) {
                PC_T_FAIL("%s: max diff %g", refs[i], (double)d * 255.0);
            }
            if (!(p > 45.0)) {
                PC_T_FAIL("%s: psnr %g", refs[i], p);
            }
        }
        pc_cimage_fini(&img);
        pc_cimage_fini(&want);
    }
}

static void test_grid_tiled_photo_matches_apple_decode(void)
{
    pc_cimage img;
    pc_cimage want;
    if (!ready()) {
        return;
    }
    /* iPhone photos are grids of 512x512 HEVC tiles: here 2x2. */
    img = dec(CHECKER_GRID->data, CHECKER_GRID->len);
    want = reference("heic-rs/checker-1024.ref.png");
    CHECK_EQ_UINT(img.width, 1024);
    CHECK_EQ_UINT(img.height, 1024);
    CHECK(img.layout == want.layout && img.width == want.width && img.height == want.height &&
          max_abs_diff(&img, &want) <= 3.0f / 255.0f);
    pc_cimage_fini(&img);
    pc_cimage_fini(&want);
}

static void test_rgb_strips_keep_their_colours(void)
{
    static const uint32_t xs[3] = {16, 48, 80};
    pc_cimage img;
    size_t s;
    size_t ch;
    if (!ready()) {
        return;
    }
    /* Three vertical strips, pure red, green and blue: catches swapped channels or matrices. */
    img = dec(STRIPS_96->data, STRIPS_96->len);
    CHECK(img.width == 96 && img.height >= 17 && img.layout == PC_CL_RGB);
    if (img.width == 96 && img.height >= 17 && img.layout == PC_CL_RGB) {
        for (s = 0; s < 3; s++) {
            for (ch = 0; ch < 3; ch++) {
                float v = pc_cimage_get(&img, xs[s], 16, ch);
                pc_t_checks++;
                if (!(ch == s ? v > 0.9f : v < 0.1f)) {
                    PC_T_FAIL("strip at x=%u channel %zu = %g", (unsigned)xs[s], ch, (double)v);
                }
            }
        }
    }
    pc_cimage_fini(&img);
}

static void test_container_rotation_is_applied_once(void)
{
    pc_buf rotated;
    pc_cimage img;
    pc_cdecode_opts k = keep();
    if (!ready()) {
        return;
    }
    /* irot 1 = 90 degrees anticlockwise: the red strip (left) ends at the bottom, blue (right) on top. */
    rotated = with_irot(STRIPS_96, 1);
    img = dec(rotated.data, rotated.len);
    CHECK_EQ_UINT(img.width, 32);
    CHECK_EQ_UINT(img.height, 96);
    if (img.width == 32 && img.height == 96 && img.layout == PC_CL_RGB) {
        CHECK(pc_cimage_get(&img, 16, 8, 2) > 0.9f && pc_cimage_get(&img, 16, 8, 0) < 0.1f);   /* blue on top */
        CHECK(pc_cimage_get(&img, 16, 88, 0) > 0.9f && pc_cimage_get(&img, 16, 88, 2) < 0.1f); /* red at the bottom */
    }
    pc_cimage_fini(&img);
    /* keep_orientation hands back the pixels as coded. */
    img = dec_with(rotated.data, rotated.len, &k);
    CHECK_EQ_UINT(img.width, 96);
    CHECK_EQ_UINT(img.height, 32);
    pc_cimage_fini(&img);
    pc_buf_fini(&rotated);
}

static void test_exif_and_xmp_survive_and_exif_orientation_is_not_applied_twice(void)
{
    pc_cimage img;
    pc_cimage raw;
    pc_buf tagged;
    pc_cdecode_opts k = keep();
    if (!ready()) {
        return;
    }
    img = dec(WITH_EXIF->data, WITH_EXIF->len);
    CHECK_EQ_UINT(img.width, 2048);
    CHECK_EQ_UINT(img.height, 1536);
    /* TIFF-structured EXIF, without the HEIF offset header */
    CHECK(img.meta.exif.has && img.meta.exif.len >= 4 && memcmp(img.meta.exif.data, "MM\0*", 4) == 0);
    CHECK(img.meta.xmp.has && find_bytes(img.meta.xmp.data, img.meta.xmp.len, "x:xmpmeta", 9));
    pc_cimage_fini(&img);

    /* HEIF's EXIF Orientation repeats what irot says; the container is the authority. */
    tagged = with_exif_orientation(WITH_EXIF, 6);
    img = dec(tagged.data, tagged.len);
    CHECK_EQ_UINT(img.width, 2048); /* not turned by the EXIF tag */
    CHECK_EQ_UINT(img.height, 1536);
    CHECK(img.meta.exif.has);
    /* rewritten so exports don't turn it either */
    CHECK_EQ_UINT(pc_exif_orientation(img.meta.exif.data, img.meta.exif.len), 1);
    raw = dec_with(tagged.data, tagged.len, &k);
    CHECK(raw.meta.exif.has);
    /* kept verbatim when asked */
    CHECK_EQ_UINT(pc_exif_orientation(raw.meta.exif.data, raw.meta.exif.len), 6);
    pc_cimage_fini(&img);
    pc_cimage_fini(&raw);
    pc_buf_fini(&tagged);
}

static pc_status decode_limited(const pc_buf *b, pc_climits l)
{
    pc_cdecode_opts o = pc_cdecode_opts_default();
    pc_cimage img;
    pc_error e;
    pc_status st;
    o.limits = l;
    st = pc_cdecode_with(b->data, b->len, &o, &img, &e);
    if (st == PC_OK) {
        pc_cimage_fini(&img);
    }
    return st;
}

static void test_limits_are_checked_before_decoding(void)
{
    pc_climits small = pc_climits_default();
    pc_climits few = pc_climits_default();
    pc_climits bytes = pc_climits_default();
    if (!ready()) {
        return;
    }
    small.max_width = 512;
    CHECK_EQ_INT(decode_limited(CHECKER_GRID, small), PC_ERR_LIMIT);
    few.max_pixels = 1000;
    CHECK_EQ_INT(decode_limited(CHECKER_64, few), PC_ERR_LIMIT);
    bytes.max_alloc = 64 * 64 * 3 - 1;
    CHECK_EQ_INT(decode_limited(CHECKER_64, bytes), PC_ERR_LIMIT);
}

static const uint8_t SEQUENCE_ONLY[24] = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'm', 's', 'f', '1',
                                          0, 0, 0, 0,    'm', 's', 'f', '1', 'h', 'e', 'v', 'c'};

static void test_unsupported_and_broken_files_are_clear_errors(void)
{
    pc_cimage img;
    pc_error e;
    if (!ready()) {
        return;
    }
    /* An ftyp and nothing else: a HEIF image sequence would have a moov here instead of a meta. */
    CHECK_EQ_INT(pc_cdecode(SEQUENCE_ONLY, sizeof SEQUENCE_ONLY, &img, &e), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "Heif: ");
    CHECK_CONTAINS(e.msg, "sequence");
    CHECK(pc_cdecode_as(F, (const uint8_t *)"", 0, &img, &e) != PC_OK);
    /* half a file */
    CHECK(pc_cdecode_as(F, CHECKER_GRID->data, CHECKER_GRID->len / 2, &img, &e) != PC_OK);
}

static void test_every_truncation_errors_or_decodes_never_panics(void)
{
    size_t f;
    if (!ready()) {
        return;
    }
    for (f = 0; f < 3; f++) {
        size_t cut;
        for (cut = 0; cut < files[f].len; cut++) {
            pc_cimage img;
            if (pc_cdecode_as(F, files[f].data, cut, &img, NULL) == PC_OK) {
                pc_cimage_fini(&img);
            }
        }
    }
    CHECK(true); /* reaching here is the test */
}

static void test_ten_bit_with_alpha_decodes_to_16_bit_rgba(void)
{
    pc_buf heif;
    pc_buf png;
    pc_cimage img;
    pc_cimage src;
    pc_error e;
    if (!ready()) {
        return;
    }
    /* pillow-heif's fixture: a 16-bit RGBA gradient (the .src.png) encoded as 10-bit HEIF by
     * libheif/x265. */
    heif = corpus("pillow-heif/heif/RGBA_10__29x100.heif");
    png = corpus("pillow-heif/non_heif/RGBA_16__29x100.png");
    if (heif.len == 0 || png.len == 0) {
        PC_T_FAIL("pillow-heif fixtures missing");
        pc_buf_fini(&heif);
        pc_buf_fini(&png);
        return;
    }
    img = dec(heif.data, heif.len);
    if (pc_cdecode_as(PC_CF_PNG, png.data, png.len, &src, &e) != PC_OK) {
        PC_T_FAIL("png: %s", e.msg);
        pc_cimage_init(&src);
    }
    CHECK(img.width == 29 && img.height == 100 && img.layout == PC_CL_RGBA && img.sample == PC_CS_U16);
    if (img.width == 29 && img.height == 100 && img.layout == PC_CL_RGBA && img.sample == PC_CS_U16 &&
        src.width == 29 && src.height == 100 && pc_clayout_has_alpha(src.layout)) {
        size_t n = pc_cimage_sample_count(&img);
        size_t odd = 0;
        size_t i;
        uint32_t x;
        uint32_t y;
        size_t bad = 0;
        pc_cimage a;
        pc_cimage b;
        /* Depth is kept, not 8 bits stretched: most samples are not multiples of 257. */
        for (i = 0; i < n; i++) {
            odd += ne16(img.data, i) % 257 != 0;
        }
        CHECK(odd > n / 2);
        /* The alpha plane is coded on its own and comes back almost exactly. */
        for (y = 0; y < 100; y++) {
            for (x = 0; x < 29; x++) {
                if (!(fabsf(pc_cimage_get(&img, x, y, 3) - pc_cimage_get(&src, x, y, 3)) < 0.002f) && bad++ == 0) {
                    PC_T_FAIL("alpha at %u,%u", (unsigned)x, (unsigned)y);
                }
            }
        }
        CHECK_EQ_UINT(bad, 0);
        /* Colour went through 4:2:0 chroma on a 29-pixel-wide saturated gradient, which costs
         * about 26 dB at any depth; a decoder bug (range, matrix, bit shift) lands far below. */
        if (pc_cimage_convert(&img, PC_CL_RGB, PC_CS_U16, &a, &e) == PC_OK) {
            if (pc_cimage_convert(&src, PC_CL_RGB, PC_CS_U16, &b, &e) == PC_OK) {
                double p = psnr(&a, &b);
                pc_t_checks++;
                if (!(p > 24.0)) {
                    PC_T_FAIL("psnr %g", p);
                }
                pc_cimage_fini(&b);
            }
            pc_cimage_fini(&a);
        }
    }
    pc_cimage_fini(&img);
    pc_cimage_fini(&src);
    pc_buf_fini(&heif);
    pc_buf_fini(&png);
}

/* ---- unit tests of crates/codecs/src/codecs/heif.rs (with the feature) ---- */

static void test_with_the_feature_heif_is_readable_and_errors_map(void)
{
    pc_cimage img;
    pc_error e;
    pc_status st;
    if (!pc_cformat_caps(F).read) {
        return;
    }
    CHECK(pc_cformat_caps(F).read);
    CHECK_EQ_INT(pc_cdecode(SEQUENCE_ONLY, sizeof SEQUENCE_ONLY, &img, &e), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "sequence");
    st = pc_cdecode_as(F, (const uint8_t *)"", 0, &img, &e);
    CHECK(st == PC_ERR_FORMAT || st == PC_ERR_UNSUPPORTED);
}

/* Found by the decode_heif fuzz target: a malformed box made heic-rs 0.1.1 slice out of range. It
 * must be an error, not a crash. */
static const uint8_t HEIC_RS_BOX_PANIC[72] = {
    0x00, 0x00, 0x00, 0x24, 0x66, 0x74, 0x79, 0x70, 0x68, 0x65, 0x69, 0x63, 0x00, 0x00, 0x00, 0x00, 0x6d, 0x69,
    0x66, 0x31, 0x4d, 0x69, 0x50, 0x72, 0x6d, 0x69, 0x61, 0x66, 0x4d, 0x69, 0x48, 0x42, 0x72, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x6d, 0x65, 0x74, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x75, 0x75,
    0x69, 0x64, 0x00, 0x00, 0x00, 0x00, 0x03, 0x08, 0x08, 0x08, 0x00, 0x03, 0x01, 0x03, 0x70, 0x00, 0xa8, 0x00,
};

static void test_a_heic_rs_panic_is_a_malformed_file_error(void)
{
    pc_cformat det;
    pc_cdecode_opts opts[2];
    size_t i;
    if (!pc_cformat_caps(F).read) {
        return;
    }
    opts[0] = pc_cdecode_opts_default();
    opts[1] = keep();
    CHECK(pc_cformat_detect(HEIC_RS_BOX_PANIC, sizeof HEIC_RS_BOX_PANIC, &det) && det == F);
    for (i = 0; i < 2; i++) {
        pc_cimage img;
        pc_error e;
        CHECK_EQ_INT(pc_cdecode_with(HEIC_RS_BOX_PANIC, sizeof HEIC_RS_BOX_PANIC, &opts[i], &img, &e), PC_ERR_FORMAT);
        CHECK_CONTAINS(e.msg, "Heif: malformed data: ");
    }
}

#endif

int main(int argc, char **argv)
{
#ifndef PC_NO_HEIF
    size_t i;
    (void)load_files();
    if (!pc_cformat_caps(F).read) {
        (void)printf("(libheif has no HEVC decoder: HEIF decode tests skipped) ");
    }
    RUN(test_heif_is_read_only_and_listed);
    RUN(test_detect_heif_brands);
    RUN(test_single_picture_matches_apple_decode);
    RUN(test_grid_tiled_photo_matches_apple_decode);
    RUN(test_rgb_strips_keep_their_colours);
    RUN(test_container_rotation_is_applied_once);
    RUN(test_exif_and_xmp_survive_and_exif_orientation_is_not_applied_twice);
    RUN(test_limits_are_checked_before_decoding);
    RUN(test_unsupported_and_broken_files_are_clear_errors);
    RUN(test_every_truncation_errors_or_decodes_never_panics);
    RUN(test_ten_bit_with_alpha_decodes_to_16_bit_rgba);
    RUN(test_with_the_feature_heif_is_readable_and_errors_map);
    RUN(test_a_heic_rs_panic_is_a_malformed_file_error);
    if (!pc_cformat_caps(F).read) {
        RUN(test_without_the_feature_heif_is_a_clear_unsupported_error);
    }
    for (i = 0; i < 4; i++) {
        pc_buf_fini(&files[i]);
    }
#else
    RUN(test_without_the_feature_heif_is_a_clear_unsupported_error);
#endif
    return pc_t_finish();
}
