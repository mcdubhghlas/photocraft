/* Save for Web writers: port of the unit tests in crates/codecs/src/web.rs (tests,
 * subsampling_tests, anim_tests). GIFs and JPEGs are read back through our own pc_cdecode. */
#include "common.h"
#include "codecs/web.h"

/* Decodes and converts to RGBA U8 (new array of 4 * w * h bytes; NULL on failure). */
static uint8_t *decode_rgba(const pc_buf *b, uint32_t *w, uint32_t *h)
{
    pc_cimage img = dec(b->data, b->len);
    pc_cimage rgba;
    pc_error e;
    uint8_t *px = NULL;
    *w = img.width;
    *h = img.height;
    if (pc_cimage_convert(&img, PC_CL_RGBA, PC_CS_U8, &rgba, &e) != PC_OK) {
        PC_T_FAIL("convert: %s", e.msg);
    } else {
        px = rgba.data;
        rgba.data = NULL;
        rgba.len = 0;
        pc_cimage_fini(&rgba);
    }
    pc_cimage_fini(&img);
    return px;
}

static uint8_t *pattern(size_t w, size_t h, size_t n)
{
    uint8_t *v = malloc(w * h + 1);
    size_t i;
    for (i = 0; i < w * h; i++) {
        v[i] = (uint8_t)((((i % w) * 7 + (i / w) * 13 + (i * i) % 5) % n));
    }
    return v;
}

static pc_buf gif(uint32_t w, uint32_t h, const uint8_t *idx, size_t n, const uint8_t (*pal)[3], size_t npal,
                  int transparent, bool interlaced)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    if (pc_web_encode_gif_indexed(w, h, idx, n, pal, npal, transparent, interlaced, &b, &e) != PC_OK) {
        PC_T_FAIL("encode_gif_indexed %ux%u: %s", (unsigned)w, (unsigned)h, e.msg);
    }
    return b;
}

static void test_gif_roundtrips_through_the_decoder(void)
{
    static const struct {
        uint32_t w, h;
        bool interlaced;
    } cases[] = {{1, 1, false}, {37, 23, false}, {37, 23, true}, {300, 200, false}};
    uint8_t pal[200][3];
    size_t i;
    size_t c;
    for (i = 0; i < 200; i++) {
        pal[i][0] = (uint8_t)i;
        pal[i][1] = (uint8_t)(255 - i);
        pal[i][2] = (uint8_t)(i * 3 % 256);
    }
    for (c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint32_t w = cases[c].w;
        uint32_t h = cases[c].h;
        uint8_t *idx = pattern(w, h, 200);
        pc_buf bytes = gif(w, h, idx, (size_t)w * h, (const uint8_t (*)[3])pal, 200, -1, cases[c].interlaced);
        uint32_t dw;
        uint32_t dh;
        uint8_t *px = decode_rgba(&bytes, &dw, &dh);
        CHECK_EQ_UINT(dw, w);
        CHECK_EQ_UINT(dh, h);
        if (px != NULL && dw == w && dh == h) {
            size_t bad = 0;
            for (i = 0; i < (size_t)w * h; i++) {
                if (memcmp(px + i * 4, pal[idx[i]], 3) != 0 && bad++ == 0) {
                    PC_T_FAIL("pixel %zu of %ux%u interlaced=%d", i, (unsigned)w, (unsigned)h, cases[c].interlaced);
                }
            }
            CHECK_EQ_UINT(bad, 0);
        }
        free(px);
        free(idx);
        pc_buf_fini(&bytes);
    }
}

static void test_gif_small_palettes_and_transparency(void)
{
    static const uint8_t pal[3][3] = {{255, 255, 255}, {0, 0, 0}, {255, 0, 0}};
    static const uint8_t bad_idx[4] = {0, 1, 2, 3};
    uint8_t idx[64 * 64];
    uint8_t bw[1000];
    size_t i;
    uint32_t w;
    uint32_t h;
    pc_buf bytes;
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    uint8_t *px;
    for (i = 0; i < sizeof idx; i++) {
        idx[i] = (uint8_t)(i % 3);
    }
    bytes = gif(64, 64, idx, sizeof idx, pal, 3, 2, false);
    px = decode_rgba(&bytes, &w, &h);
    CHECK(px != NULL);
    if (px != NULL) {
        CHECK_EQ_UINT(px[3], 255);
        CHECK_EQ_UINT(px[2 * 4 + 3], 0); /* index 2 is transparent */
    }
    free(px);
    pc_buf_fini(&bytes);
    /* Two-colour image: minimum code size 2. */
    for (i = 0; i < sizeof bw; i++) {
        bw[i] = (uint8_t)(i % 7 == 0);
    }
    bytes = gif(100, 10, bw, sizeof bw, pal, 2, -1, false);
    px = decode_rgba(&bytes, &w, &h);
    CHECK(px != NULL);
    if (px != NULL) {
        CHECK_EQ_UINT(px[0], 0); /* index 1 = black at x 0 */
    }
    free(px);
    pc_buf_fini(&bytes);
    CHECK(pc_web_encode_gif_indexed(2, 2, bad_idx, 4, pal, 3, -1, false, &out, &e) != PC_OK);
    pc_buf_fini(&out);
}

static void test_gif_long_runs_hit_the_dictionary_limit(void)
{
    /* Noise forces many codes and dictionary resets; flat areas make long strings. */
    size_t n = 512 * 512;
    uint8_t *idx = malloc(n);
    uint8_t pal[256][3];
    uint32_t seed = 12345u;
    uint32_t w;
    uint32_t h;
    size_t i;
    pc_buf bytes;
    uint8_t *px;
    for (i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        idx[i] = (i / 512) % 64 < 32 ? (uint8_t)(seed >> 16) : 7;
    }
    for (i = 0; i < 256; i++) {
        pal[i][0] = (uint8_t)i;
        pal[i][1] = (uint8_t)i;
        pal[i][2] = (uint8_t)(255 - i);
    }
    bytes = gif(512, 512, idx, n, (const uint8_t (*)[3])pal, 256, -1, false);
    px = decode_rgba(&bytes, &w, &h);
    CHECK(px != NULL && w == 512 && h == 512);
    if (px != NULL && w == 512 && h == 512) {
        size_t bad = 0;
        for (i = 0; i < n; i += 97) {
            if (px[i * 4] != idx[i] && bad++ == 0) {
                PC_T_FAIL("pixel %zu: %u vs %u", i, px[i * 4], idx[i]);
            }
        }
        CHECK_EQ_UINT(bad, 0);
    }
    free(px);
    free(idx);
    pc_buf_fini(&bytes);
}

static void test_wbmp_roundtrips(void)
{
    static const uint8_t head[4] = {0, 0, 13, 5};
    static const uint8_t multibyte[2] = {0x82, 0x2c};
    static const uint8_t not_type0[5] = {1, 0, 1, 1, 0};
    bool px[13 * 5];
    bool all_white[300];
    size_t i;
    pc_buf bytes = PC_BUF_INIT;
    pc_buf big = PC_BUF_INIT;
    pc_error e;
    uint32_t w = 0;
    uint32_t h = 0;
    bool *back = NULL;
    for (i = 0; i < 13 * 5; i++) {
        px[i] = i % 3 == 0;
    }
    for (i = 0; i < 300; i++) {
        all_white[i] = true;
    }
    CHECK(pc_web_encode_wbmp(13, 5, px, 13 * 5, &bytes, &e) == PC_OK);
    CHECK(bytes.len >= 4 && memcmp(bytes.data, head, 4) == 0);
    CHECK(pc_web_decode_wbmp(bytes.data, bytes.len, &w, &h, &back, &e) == PC_OK);
    CHECK_EQ_UINT(w, 13);
    CHECK_EQ_UINT(h, 5);
    CHECK(back != NULL && memcmp(back, px, sizeof px) == 0);
    free(back);
    back = NULL;
    CHECK(pc_web_encode_wbmp(300, 1, all_white, 300, &big, &e) == PC_OK);
    CHECK(big.len >= 10 && memcmp(big.data + 2, multibyte, 2) == 0); /* 300 as a multi-byte integer */
    CHECK(pc_web_decode_wbmp(big.data, big.len < 10 ? big.len : 10, &w, &h, &back, &e) != PC_OK);
    CHECK(pc_web_decode_wbmp(not_type0, sizeof not_type0, &w, &h, &back, &e) != PC_OK);
    CHECK(back == NULL);
    pc_buf_fini(&bytes);
    pc_buf_fini(&big);
}

static void test_progressive_jpeg_decodes(void)
{
    static const char xmp[] = "<x:xmpmeta/>";
    static const uint8_t sof2[2] = {0xFF, 0xC2};
    size_t n = 64 * 48 * 3;
    uint8_t *rgb = malloc(n);
    pc_buf base = PC_BUF_INIT;
    pc_buf prog = PC_BUF_INIT;
    pc_error e;
    pc_cimage img;
    size_t i;
    for (i = 0; i < n; i++) {
        rgb[i] = (uint8_t)(i % 251);
    }
    CHECK(pc_web_encode_jpeg_rgb8(64, 48, rgb, n, 80, false, false, NULL, 0, true, 72.0f, xmp, strlen(xmp), &base,
                                  &e) == PC_OK);
    CHECK(pc_web_encode_jpeg_rgb8(64, 48, rgb, n, 80, true, true, NULL, 0, false, 0.0f, NULL, 0, &prog, &e) ==
          PC_OK);
    CHECK(base.len != prog.len || memcmp(base.data, prog.data, base.len) != 0);
    img = dec(base.data, base.len);
    CHECK(img.meta.xmp.has && img.meta.xmp.len == strlen(xmp) && memcmp(img.meta.xmp.data, xmp, strlen(xmp)) == 0);
    pc_cimage_fini(&img);
    /* SOF2 marks a progressive frame. */
    CHECK(find_bytes(prog.data, prog.len, sof2, 2));
    img = dec(prog.data, prog.len);
    CHECK_EQ_UINT(img.width, 64);
    CHECK_EQ_UINT(img.height, 48);
    pc_cimage_fini(&img);
    pc_buf_fini(&base);
    pc_buf_fini(&prog);
    free(rgb);
}

/* The reference's jpeg-encoder writes broken scans for baseline 4:2:0 with optimised Huffman
 * tables; pc_web_encode_jpeg_rgb8 must avoid that combination. */
static void test_every_option_combination_decodes_to_the_source(void)
{
    static const uint8_t qs[3] = {30, 60, 95};
    static const bool combos[4][2] = {{false, false}, {false, true}, {true, false}, {true, true}};
    uint32_t w = 64;
    uint32_t h = 48;
    size_t n = (size_t)w * h * 3;
    uint8_t *rgb = malloc(n);
    size_t i;
    size_t q;
    size_t c;
    for (i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = (uint8_t)((i % w) * 4);
        rgb[i * 3 + 1] = 76;
        rgb[i * 3 + 2] = (uint8_t)((i / w) * 5);
    }
    for (q = 0; q < 3; q++) {
        for (c = 0; c < 4; c++) {
            pc_buf bytes = PC_BUF_INIT;
            pc_error e;
            pc_cimage img;
            pc_cimage ours;
            unsigned err = 0;
            if (pc_web_encode_jpeg_rgb8(w, h, rgb, n, qs[q], combos[c][0], combos[c][1], NULL, 0, false, 0.0f, NULL,
                                        0, &bytes, &e) != PC_OK) {
                PC_T_FAIL("encode q%u: %s", qs[q], e.msg);
                continue;
            }
            img = dec(bytes.data, bytes.len);
            if (pc_cimage_convert(&img, PC_CL_RGB, PC_CS_U8, &ours, &e) == PC_OK) {
                for (i = 0; i < n && i < ours.len; i++) {
                    unsigned d = (unsigned)abs((int)ours.data[i] - (int)rgb[i]);
                    err = d > err ? d : err;
                }
                CHECK_EQ_UINT(ours.len, n);
                pc_cimage_fini(&ours);
            } else {
                PC_T_FAIL("convert: %s", e.msg);
            }
            pc_t_checks++;
            if (err >= 24) {
                PC_T_FAIL("q%u progressive=%d optimized=%d: max error %u", qs[q], combos[c][0], combos[c][1], err);
            }
            pc_cimage_fini(&img);
            pc_buf_fini(&bytes);
        }
    }
    free(rgb);
}

static void test_animated_gif_has_header_and_frames(void)
{
    static const uint8_t pal[3][3] = {{0, 0, 0}, {255, 0, 0}, {0, 255, 0}};
    uint8_t idx[3][4];
    pc_gif_frame frames[3];
    pc_buf g = PC_BUF_INIT;
    pc_error e;
    pc_cimage img;
    size_t i;
    for (i = 0; i < 3; i++) {
        memset(idx[i], (int)i, 4);
        frames[i].indices = idx[i];
        frames[i].n = 4;
        frames[i].palette = pal;
        frames[i].npal = 3;
        frames[i].transparent = -1;
        frames[i].delay_cs = 4;
    }
    CHECK(pc_web_encode_gif_animated(2, 2, frames, 3, true, &g, &e) == PC_OK);
    CHECK(g.len >= 6 && memcmp(g.data, "GIF89a", 6) == 0);
    CHECK(g.len > 0 && g.data[g.len - 1] == 0x3B);
    /* Three image descriptors (0x2C) for three frames. */
    CHECK_EQ_UINT(count_bytes(g.data, g.len, "\x2C", 1), 3);
    /* NETSCAPE loop extension present. */
    CHECK(find_bytes(g.data, g.len, "NETSCAPE2.0", 11));
    /* First frame still decodes. */
    img = dec(g.data, g.len);
    CHECK_EQ_UINT(img.width, 2);
    CHECK_EQ_UINT(img.height, 2);
    pc_cimage_fini(&img);
    pc_buf_fini(&g);
}

static void test_rejects_bad_frames(void)
{
    static const uint8_t pal[1][3] = {{0, 0, 0}};
    static const uint8_t idx[3] = {0, 0, 0};
    pc_gif_frame bad = {idx, 3, pal, 1, -1, 1};
    pc_buf out = PC_BUF_INIT;
    pc_error e;
    CHECK(pc_web_encode_gif_animated(2, 2, NULL, 0, false, &out, &e) != PC_OK);
    CHECK(pc_web_encode_gif_animated(2, 2, &bad, 1, false, &out, &e) != PC_OK);
    CHECK_EQ_UINT(out.len, 0);
    pc_buf_fini(&out);
}

int main(int argc, char **argv)
{
    RUN(test_gif_roundtrips_through_the_decoder);
    RUN(test_gif_small_palettes_and_transparency);
    RUN(test_gif_long_runs_hit_the_dictionary_limit);
    RUN(test_wbmp_roundtrips);
    RUN(test_progressive_jpeg_decodes);
    RUN(test_every_option_combination_decodes_to_the_source);
    RUN(test_animated_gif_has_header_and_frames);
    RUN(test_rejects_bad_frames);
    return pc_t_finish();
}
