/* Ports crates/codecs/tests/{resolution,source_settings,pnm_peak_memory}.rs and the new unit tests
 * of crates/codecs/src/{exif_color,source_settings,web}.rs, codecs/png.rs (parallel encoder) and
 * codecs/tiff_ifd.rs (overlapping directory tables). The proptest properties become seeded loops. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/compress.h"
#include "codecs/orientation.h"
#include "codecs/resolution.h"
#include "codecs/source_settings.h"
#include "codecs/web.h"
#include "common.h"

enum { TX = 282, TY = 283, TUNIT = 296, SHORT = 3, LONG = 4, RATIONAL = 5, ASCII = 2 };

typedef struct entry {
    uint16_t tag;
    uint16_t ty;
    uint32_t count;
    uint8_t value[16];
    size_t len;
} entry;

static void put16(bool big, uint8_t *p, uint16_t v)
{
    p[big ? 0 : 1] = (uint8_t)(v >> 8);
    p[big ? 1 : 0] = (uint8_t)v;
}

static void put32(bool big, uint8_t *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        p[big ? i : 3 - i] = (uint8_t)(v >> (24 - 8 * i));
    }
}

static entry rat(bool big, uint16_t tag, uint32_t n, uint32_t d)
{
    entry e = {tag, RATIONAL, 1, {0}, 8};
    put32(big, e.value, n);
    put32(big, e.value + 4, d);
    return e;
}

static entry ex(bool big, uint32_t n, uint32_t d)
{
    return rat(big, TX, n, d);
}
static entry ey(bool big, uint32_t n, uint32_t d)
{
    return rat(big, TY, n, d);
}

static entry eshort(bool big, uint16_t tag, uint16_t v)
{
    entry e = {tag, SHORT, 1, {0}, 2};
    put16(big, e.value, v);
    return e;
}

static entry elong(bool big, uint16_t tag, uint32_t v)
{
    entry e = {tag, LONG, 1, {0}, 4};
    put32(big, e.value, v);
    return e;
}

static entry eunit(bool big, uint16_t u)
{
    return eshort(big, TUNIT, u);
}

static entry emake(void)
{
    entry e = {0x010F, ASCII, 6, "NIKON", 6};
    return e;
}

/* A TIFF-structured EXIF block: header, IFD0 at 8, then the out-of-line values. */
static pc_buf build(bool big, const entry *es, size_t n)
{
    pc_buf v = PC_BUF_INIT;
    pc_buf data = PC_BUF_INIT;
    uint8_t x[12];
    size_t data_at = 8 + 2 + n * 12 + 4;
    size_t i;
    (void)pc_buf_append(&v, big ? "MM\0*" : "II*\0", 4, NULL);
    put32(big, x, 8);
    (void)pc_buf_append(&v, x, 4, NULL);
    put16(big, x, (uint16_t)n);
    (void)pc_buf_append(&v, x, 2, NULL);
    for (i = 0; i < n; i++) {
        memset(x, 0, sizeof x);
        put16(big, x, es[i].tag);
        put16(big, x + 2, es[i].ty);
        put32(big, x + 4, es[i].count);
        if (es[i].len <= 4) {
            memcpy(x + 8, es[i].value, es[i].len);
        } else {
            put32(big, x + 8, (uint32_t)data_at);
            (void)pc_buf_append(&data, es[i].value, es[i].len, NULL);
            data_at += es[i].len;
        }
        (void)pc_buf_append(&v, x, 12, NULL);
    }
    put32(big, x, 0);
    (void)pc_buf_append(&v, x, 4, NULL);
    (void)pc_buf_append(&v, data.data, data.len, NULL);
    pc_buf_fini(&data);
    return v;
}

static pc_buf camera_exif(bool big)
{
    entry e[4];
    e[0] = emake();
    e[1] = ex(big, 300, 1);
    e[2] = ey(big, 300, 1);
    e[3] = eunit(big, 2);
    return build(big, e, 4);
}

static void segment(pc_buf *o, uint8_t marker, const void *p1, size_t n1, const void *p2, size_t n2)
{
    uint8_t h[4] = {0xFF, marker, (uint8_t)((n1 + n2 + 2) >> 8), (uint8_t)(n1 + n2 + 2)};
    (void)pc_buf_append(o, h, 4, NULL);
    (void)pc_buf_append(o, p1, n1, NULL);
    (void)pc_buf_append(o, p2, n2, NULL);
}

static void jfif(pc_buf *o, uint8_t unit, uint16_t x, uint16_t y)
{
    uint8_t p[14] = {
        'J',        'F', 'I', 'F', 0, 1, 2, unit, (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)(y >> 8),
        (uint8_t)y, 0,   0};
    segment(o, 0xE0, p, sizeof p, NULL, 0);
}

static void exif_app1(pc_buf *o, const uint8_t *exif, size_t n)
{
    segment(o, 0xE1, "Exif\0\0", 6, exif, n);
}

static void xmp_app1(pc_buf *o, const char *xmp)
{
    segment(o, 0xE1, "http://ns.adobe.com/xap/1.0/", 29, xmp, strlen(xmp));
}

static pc_buf irb(double ppi)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t fx[4];
    uint32_t fixed = (uint32_t)lround(ppi * 65536.0);
    int i;
    (void)pc_buf_append(&b,
                        "8BIM\x04\x04\x03"
                        "abc\0\0\0\x03\x01\x02\x03\0",
                        18, NULL);
    (void)pc_buf_append(&b, "8BIM\x03\xed\0\0\0\0\0\x10", 12, NULL);
    put32(true, fx, fixed);
    for (i = 0; i < 2; i++) {
        (void)pc_buf_append(&b, fx, 4, NULL);
        (void)pc_buf_append(&b, "\0\1\0\2", 4, NULL);
    }
    return b;
}

static void app13(pc_buf *o, const uint8_t *r, size_t n)
{
    segment(o, 0xED, "Photoshop 3.0", 14, r, n);
}

/* A 16x8 baseline JPEG whose APPn segments are exactly `segs`. */
static pc_buf jpeg_with(const pc_buf *segs)
{
    uint8_t px[128];
    pc_cimage img;
    pc_buf base = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    size_t i = 2;
    memset(px, 128, sizeof px);
    (void)pc_cimage_from_copy(16, 8, PC_CL_GRAY, PC_CS_U8, px, sizeof px, &img, NULL);
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_JPEG, NULL, &base, NULL), PC_OK);
    pc_cimage_fini(&img);
    while (i + 4 <= base.len && base.data[i + 1] >= 0xE0 && base.data[i + 1] <= 0xEF) {
        i += 2 + ((size_t)base.data[i + 2] << 8 | base.data[i + 3]);
    }
    (void)pc_buf_append(&out, base.data, 2, NULL);
    if (segs != NULL) {
        (void)pc_buf_append(&out, segs->data, segs->len, NULL);
    }
    (void)pc_buf_append(&out, base.data + i, base.len - i, NULL);
    pc_buf_fini(&base);
    return out;
}

/* Decoded dpi of segments-in-a-JPEG; consumes segs. */
static bool dpi_of_segs(pc_buf *segs, float *x, float *y)
{
    pc_buf j = jpeg_with(segs);
    pc_cimage img;
    bool has = false;
    if (pc_cdecode(j.data, j.len, &img, NULL) == PC_OK) {
        has = img.meta.has_dpi;
        *x = img.meta.dpi_x;
        *y = img.meta.dpi_y;
        pc_cimage_fini(&img);
    } else {
        PC_T_FAIL("decode");
    }
    pc_buf_fini(&j);
    pc_buf_fini(segs);
    return has;
}

#define NEAR_DPI(has, x, y, want) CHECK((has) && fabsf((x) - (want)) < 0.01f && fabsf((y) - (want)) < 0.01f)

static void check_segs_dpi(pc_buf *segs, float want)
{
    float x = 0;
    float y = 0;
    bool has = dpi_of_segs(segs, &x, &y);
    NEAR_DPI(has, x, y, want);
}

static void test_exif_only_camera_jpeg_opens_at_its_exif_resolution(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        pc_buf s = PC_BUF_INIT;
        pc_buf e = camera_exif(big);
        entry a[2] = {ex(big, 300, 1), ey(big, 300, 1)};
        entry c[3] = {ex(big, 3000000, 10000), ey(big, 1200, 4), eunit(big, 2)};
        exif_app1(&s, e.data, e.len);
        check_segs_dpi(&s, 300.0f);
        pc_buf_fini(&e);
        e = build(big, a, 2);
        exif_app1(&s, e.data, e.len);
        check_segs_dpi(&s, 300.0f);
        pc_buf_fini(&e);
        e = build(big, c, 3);
        exif_app1(&s, e.data, e.len);
        check_segs_dpi(&s, 300.0f);
        pc_buf_fini(&e);
    }
}

static void test_exif_resolution_in_centimetres_is_converted(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        entry a[3] = {ex(big, 11811, 100), ey(big, 11811, 100), eunit(big, 3)};
        pc_buf e = build(big, a, 3);
        pc_buf s = PC_BUF_INIT;
        float x = 0;
        float y = 0;
        bool has = pc_exif_resolution(e.data, e.len, &x, &y);
        NEAR_DPI(has, x, y, 299.9994f);
        exif_app1(&s, e.data, e.len);
        check_segs_dpi(&s, 299.9994f);
        pc_buf_fini(&e);
    }
}

static void test_jfif_and_exif_precedence(void)
{
    pc_buf le = camera_exif(false);
    pc_buf be = camera_exif(true);
    pc_buf s = PC_BUF_INIT;
    float x;
    float y;
    jfif(&s, 0, 1, 1);
    exif_app1(&s, le.data, le.len);
    check_segs_dpi(&s, 300.0f);
    jfif(&s, 1, 72, 72);
    exif_app1(&s, le.data, le.len);
    check_segs_dpi(&s, 300.0f);
    jfif(&s, 1, 72, 72);
    exif_app1(&s, be.data, be.len);
    check_segs_dpi(&s, 300.0f);
    jfif(&s, 1, 72, 72);
    check_segs_dpi(&s, 72.0f);
    jfif(&s, 2, 100, 100);
    check_segs_dpi(&s, 254.0f);
    jfif(&s, 0, 1, 1);
    CHECK(!dpi_of_segs(&s, &x, &y));
    pc_buf_fini(&le);
    pc_buf_fini(&be);
}

static void test_photoshop_resolution_info_wins_then_xmp_then_exif(void)
{
    static const char XMP[] =
        "<x:xmpmeta><rdf:Description tiff:XResolution=\"200/1\" tiff:YResolution=\"200/1\" "
        "tiff:ResolutionUnit=\"2\"/></x:xmpmeta>";
    static const char ELEM[] =
        "<tiff:XResolution>100/1</tiff:XResolution><tiff:YResolution>100</tiff:YResolution>"
        "<tiff:ResolutionUnit>3</tiff:ResolutionUnit>";
    pc_buf e = camera_exif(false);
    pc_buf r240 = irb(240.0);
    pc_buf r2995 = irb(299.5);
    pc_buf r150 = irb(150.0);
    pc_buf s = PC_BUF_INIT;
    float x = 0;
    float y = 0;
    bool has;
    jfif(&s, 1, 72, 72);
    exif_app1(&s, e.data, e.len);
    xmp_app1(&s, XMP);
    app13(&s, r240.data, r240.len);
    check_segs_dpi(&s, 240.0f);
    jfif(&s, 1, 72, 72);
    exif_app1(&s, e.data, e.len);
    xmp_app1(&s, XMP);
    check_segs_dpi(&s, 200.0f);
    jfif(&s, 1, 72, 72);
    exif_app1(&s, e.data, e.len);
    app13(&s, r2995.data, r2995.len);
    check_segs_dpi(&s, 299.5f);
    has = pc_photoshop_resolution(r150.data, r150.len, &x, &y);
    NEAR_DPI(has, x, y, 150.0f);
    has = pc_xmp_resolution(ELEM, strlen(ELEM), &x, &y);
    NEAR_DPI(has, x, y, 254.0f);
    pc_buf_fini(&e);
    pc_buf_fini(&r240);
    pc_buf_fini(&r2995);
    pc_buf_fini(&r150);
}

static void test_unusable_exif_falls_back_to_jfif(void)
{
    const bool b = false;
    entry cases[8][2];
    size_t nc[8] = {2, 2, 2, 2, 2, 2, 2, 1};
    entry tworat = {TX, RATIONAL, 2, {0}, 16};
    float x;
    float y;
    size_t i;
    put32(b, tworat.value, 300);
    put32(b, tworat.value + 4, 1);
    put32(b, tworat.value + 8, 300);
    put32(b, tworat.value + 12, 1);
    for (i = 0; i < 8; i++) {
        cases[i][0] = ex(b, 300, 1);
        cases[i][1] = ey(b, 300, 1);
    }
    for (i = 0; i < 3; i++) {
        pc_buf e;
        entry three[3] = {ex(b, 300, 1), ey(b, 300, 1),
                          i == 0   ? eunit(b, 1)
                          : i == 1 ? eunit(b, 9)
                                   : elong(b, TUNIT, 2)};
        pc_buf s = PC_BUF_INIT;
        e = build(b, three, 3);
        CHECK(!pc_exif_resolution(e.data, e.len, &x, &y));
        jfif(&s, 1, 72, 72);
        exif_app1(&s, e.data, e.len);
        check_segs_dpi(&s, 72.0f);
        pc_buf_fini(&e);
    }
    cases[0][0] = ex(b, 300, 0);
    cases[1][0] = ex(b, 0, 1);
    cases[2][0] = eshort(b, TX, 300);
    cases[3][0] = tworat;
    for (i = 0; i < 5; i++) {
        size_t k = i < 4 ? i : 7;
        pc_buf e = build(b, cases[k], nc[k]);
        pc_buf s = PC_BUF_INIT;
        CHECK(!pc_exif_resolution(e.data, e.len, &x, &y));
        jfif(&s, 1, 72, 72);
        exif_app1(&s, e.data, e.len);
        check_segs_dpi(&s, 72.0f);
        pc_buf_fini(&e);
    }
    {
        pc_buf far = camera_exif(b);
        pc_buf s = PC_BUF_INIT;
        put32(b, far.data + 8 + 2 + 12 + 8, 60000);
        CHECK(!pc_exif_resolution(far.data, far.len, &x, &y));
        jfif(&s, 1, 72, 72);
        exif_app1(&s, far.data, far.len);
        check_segs_dpi(&s, 72.0f);
        pc_buf_fini(&far);
    }
}

static void test_truncated_metadata_never_crashes_and_falls_back(void)
{
    static const char *const XS[5] = {"tiff:XResolution=", "tiff:XResolution=\"",
                                      "tiff:XResolution=\"\" tiff:YResolution=\"5/0\"", "<tiff:XResolution>",
                                      "</tiff:XResolution>9<"};
    pc_buf e = camera_exif(true);
    pc_buf r = irb(300.0);
    float x;
    float y;
    size_t n;
    for (n = 0; n < e.len; n++) {
        uint8_t *o = NULL;
        size_t ol = 0;
        pc_buf s = PC_BUF_INIT;
        bool has;
        (void)pc_exif_resolution(e.data, n, &x, &y);
        if (pc_exif_with_resolution(e.data, n, 240.0f, 240.0f, &o, &ol, NULL) == PC_OK) {
            free(o);
        }
        jfif(&s, 1, 72, 72);
        exif_app1(&s, e.data, n);
        has = dpi_of_segs(&s, &x, &y);
        CHECK(has && ((x == 72.0f && y == 72.0f) || (x == 300.0f && y == 300.0f)));
    }
    for (n = 0; n < r.len; n++) {
        pc_buf s = PC_BUF_INIT;
        CHECK(!pc_photoshop_resolution(r.data, n, &x, &y));
        jfif(&s, 1, 72, 72);
        app13(&s, r.data, n);
        check_segs_dpi(&s, 72.0f);
    }
    for (n = 0; n < 5; n++) {
        char *o = NULL;
        size_t ol = 0;
        CHECK(!pc_xmp_resolution(XS[n], strlen(XS[n]), &x, &y));
        if (pc_xmp_with_resolution(XS[n], strlen(XS[n]), 1.0f, 1.0f, &o, &ol, NULL) == PC_OK) {
            free(o);
        }
    }
    pc_buf_fini(&e);
    pc_buf_fini(&r);
}

static void test_hostile_bytes_never_crash(void)
{
    rng r = {77};
    int k;
    for (k = 0; k < 2000; k++) {
        size_t n = (size_t)(rng_next(&r) % 256);
        uint8_t *b = rng_bytes(&r, n);
        float ppi = rng_f32(&r) * 2e6f;
        float x;
        float y;
        uint8_t *o = NULL;
        size_t ol = 0;
        char *s = NULL;
        (void)pc_exif_resolution(b, n, &x, &y);
        if (pc_exif_with_resolution(b, n, ppi, ppi, &o, &ol, NULL) == PC_OK) {
            free(o);
        }
        (void)pc_photoshop_resolution(b, n, &x, &y);
        (void)pc_xmp_resolution((const char *)b, n, &x, &y);
        if (pc_xmp_with_resolution((const char *)b, n, ppi, ppi, &s, &ol, NULL) == PC_OK) {
            free(s);
        }
        (void)pc_exif_color_space(b, n, &(uint16_t){0});
        free(b);
    }
    /* mutated_exif_never_panics */
    for (k = 0; k < 2000; k++) {
        pc_buf e = camera_exif(false);
        size_t i = (size_t)(rng_next(&r) % 80);
        float ppi = 1.0f + rng_f32(&r) * 9999.0f;
        float x;
        float y;
        uint8_t *o = NULL;
        size_t ol = 0;
        if (i < e.len) {
            e.data[i] = (uint8_t)rng_next(&r);
        }
        (void)pc_exif_resolution(e.data, e.len, &x, &y);
        if (pc_exif_with_resolution(e.data, e.len, ppi, ppi, &o, &ol, NULL) == PC_OK) {
            const uint8_t *f = o != NULL ? o : e.data;
            size_t fl = o != NULL ? ol : e.len;
            (void)pc_exif_resolution(f, fl, &x, &y);
            (void)pc_exif_orientation(f, fl);
            free(o);
        }
        pc_buf_fini(&e);
    }
    CHECK(true);
}

static void test_rewriting_keeps_every_other_byte(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        entry s5[5] = {emake(), eshort(big, 0x0112, 6), ex(big, 300, 1), ey(big, 300, 1), eunit(big, 3)};
        entry w5[5] = {emake(), eshort(big, 0x0112, 6), ex(big, 240, 1), ey(big, 240, 1), eunit(big, 2)};
        pc_buf src = build(big, s5, 5);
        pc_buf want = build(big, w5, 5);
        pc_buf cam = camera_exif(big);
        entry mk[1] = {emake()};
        pc_buf nores = build(big, mk, 1);
        pc_buf pre = PC_BUF_INIT;
        uint8_t *o = NULL;
        size_t ol = 0;
        float x;
        float y;
        bool has;
        CHECK_EQ_INT(pc_exif_with_resolution(src.data, src.len, 240.0f, 240.0f, &o, &ol, NULL), PC_OK);
        CHECK(o != NULL && ol == src.len);
        has = o != NULL && pc_exif_resolution(o, ol, &x, &y);
        NEAR_DPI(has, x, y, 240.0f);
        CHECK(o != NULL && ol == want.len && memcmp(o, want.data, ol) == 0);
        free(o);
        o = NULL;
        CHECK_EQ_INT(pc_exif_with_resolution(src.data, src.len, 299.5f, 72.25f, &o, &ol, NULL), PC_OK);
        has = o != NULL && pc_exif_resolution(o, ol, &x, &y);
        CHECK(has && fabsf(x - 299.5f) < 0.01f && fabsf(y - 72.25f) < 0.01f);
        free(o);
        o = NULL;
        CHECK_EQ_INT(pc_exif_with_resolution(cam.data, cam.len, 300.0f, 300.0f, &o, &ol, NULL), PC_OK);
        CHECK(o == NULL);
        CHECK_EQ_INT(pc_exif_with_resolution(nores.data, nores.len, 240.0f, 240.0f, &o, &ol, NULL), PC_OK);
        CHECK(o == NULL);
        (void)pc_buf_append(&pre, "Exif\0\0", 6, NULL);
        (void)pc_buf_append(&pre, src.data, src.len, NULL);
        CHECK_EQ_INT(pc_exif_with_resolution(pre.data, pre.len, 240.0f, 240.0f, &o, &ol, NULL), PC_OK);
        CHECK(o != NULL && ol == 6 + want.len && memcmp(o, "Exif\0\0", 6) == 0 &&
              memcmp(o + 6, want.data, want.len) == 0);
        free(o);
        pc_buf_fini(&src);
        pc_buf_fini(&want);
        pc_buf_fini(&cam);
        pc_buf_fini(&nores);
        pc_buf_fini(&pre);
    }
}

static uint16_t rd16(bool big, const uint8_t *p)
{
    uint32_t v = big ? (uint32_t)p[0] << 8 | p[1] : (uint32_t)p[1] << 8 | p[0];
    return (uint16_t)v;
}

static uint32_t rd32(bool big, const uint8_t *p)
{
    return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
               : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static bool contains(const uint8_t *b, size_t n, const char *needle, size_t nl)
{
    size_t i;
    for (i = 0; i + nl <= n; i++) {
        if (memcmp(b + i, needle, nl) == 0) {
            return true;
        }
    }
    return false;
}

static void test_entries_that_cannot_be_rewritten_are_removed(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        entry s5[5] = {emake(), eshort(big, TX, 300), ey(big, 300, 1), elong(big, TUNIT, 2),
                       eshort(big, 0x0112, 6)};
        pc_buf src = build(big, s5, 5);
        uint8_t *o = NULL;
        size_t ol = 0;
        float x;
        float y;
        size_t i;
        CHECK_EQ_INT(pc_exif_with_resolution(src.data, src.len, 240.0f, 240.0f, &o, &ol, NULL), PC_OK);
        CHECK(o != NULL && ol == src.len);
        if (o != NULL && ol == src.len) {
            CHECK_EQ_UINT(rd16(big, o + 8), 3);
            CHECK_EQ_UINT(pc_exif_orientation(o, ol), 6);
            CHECK(!pc_exif_resolution(o, ol, &x, &y));
            CHECK_EQ_UINT(rd16(big, o + 10), 0x010F);
            CHECK_EQ_UINT(rd16(big, o + 22), TY);
            CHECK_EQ_UINT(rd16(big, o + 34), 0x0112);
            CHECK(rd32(big, o + 10 + 36) == 0);
            for (i = 10 + 40; i < 10 + 64; i++) {
                CHECK(o[i] == 0);
            }
            CHECK(contains(o, ol, "NIKON", 6));
        }
        free(o);
        pc_buf_fini(&src);
    }
}

static void check_xmp_rewrite(const char *in, float x, float y, const char *want)
{
    char *o = NULL;
    size_t ol = 0;
    CHECK_EQ_INT(pc_xmp_with_resolution(in, strlen(in), x, y, &o, &ol, NULL), PC_OK);
    if (want == NULL) {
        CHECK(o == NULL);
    } else {
        CHECK(o != NULL && ol == strlen(want) && memcmp(o, want, ol) == 0);
    }
    free(o);
}

static void test_xmp_resolution_is_rewritten(void)
{
    check_xmp_rewrite(
        "<rdf:Description tiff:XResolution=\"300/1\" tiff:YResolution='300/1' tiff:ResolutionUnit=\"3\" "
        "tiff:Make=\"X\"/>",
        240.0f, 240.0f,
        "<rdf:Description tiff:XResolution=\"240/1\" tiff:YResolution='240/1' tiff:ResolutionUnit=\"2\" "
        "tiff:Make=\"X\"/>");
    check_xmp_rewrite(
        "<tiff:XResolution>300/1</tiff:XResolution><tiff:YResolution>300/1</tiff:YResolution>", 72.5f, 72.5f,
        "<tiff:XResolution>72500/1000</tiff:XResolution><tiff:YResolution>72500/1000</tiff:YResolution>");
    check_xmp_rewrite("tiff:XResolution=\"240\" tiff:YResolution=\"240/1\"", 240.0f, 240.0f, NULL);
    check_xmp_rewrite("<x/>", 240.0f, 240.0f, NULL);
}

/* APPn payload of the first segment with marker m starting with prefix (borrowed). */
static const uint8_t *app_payload(const pc_buf *b, uint8_t m, const char *prefix, size_t pl, size_t *len)
{
    size_t i = 2;
    while (i + 4 <= b->len && b->data[i] == 0xFF && b->data[i + 1] != 0xDA) {
        size_t l = (size_t)b->data[i + 2] << 8 | b->data[i + 3];
        if (b->data[i + 1] == m && l >= 2 + pl && i + 2 + l <= b->len &&
            memcmp(b->data + i + 4, prefix, pl) == 0) {
            *len = l - 2;
            return b->data + i + 4;
        }
        i += 2 + l;
    }
    *len = 0;
    return NULL;
}

static void test_jpeg_export_writes_one_resolution_everywhere(void)
{
    static const char XMP[] = "<rdf:Description tiff:XResolution=\"300/1\" tiff:YResolution=\"300/1\"/>";
    int big;
    for (big = 0; big < 2; big++) {
        pc_buf e = camera_exif(big);
        pc_buf s = PC_BUF_INIT;
        pc_buf src;
        pc_buf out = PC_BUF_INIT;
        pc_cimage img;
        pc_cimage back;
        const uint8_t *p;
        size_t pl;
        float x;
        float y;
        bool has;
        jfif(&s, 1, 72, 72);
        exif_app1(&s, e.data, e.len);
        xmp_app1(&s, XMP);
        src = jpeg_with(&s);
        CHECK_EQ_INT(pc_cdecode(src.data, src.len, &img, NULL), PC_OK);
        NEAR_DPI(img.meta.has_dpi, img.meta.dpi_x, img.meta.dpi_y, 300.0f);
        img.meta.dpi_x = 240.0f;
        img.meta.dpi_y = 240.0f;
        CHECK_EQ_INT(pc_cencode(&img, PC_CF_JPEG, NULL, &out, NULL), PC_OK);
        p = app_payload(&out, 0xE0, "JFIF\0", 5, &pl);
        CHECK(p != NULL && pl >= 12 && p[7] == 1 && p[8] == 0 && p[9] == 240 && p[10] == 0 && p[11] == 240);
        p = app_payload(&out, 0xE1, "Exif\0\0", 6, &pl);
        has = p != NULL && pc_exif_resolution(p + 6, pl - 6, &x, &y);
        NEAR_DPI(has, x, y, 240.0f);
        CHECK(p != NULL && contains(p, pl, "NIKON", 6));
        p = app_payload(&out, 0xE1, "http://ns.adobe.com/xap/1.0/", 29, &pl);
        has = p != NULL && pc_xmp_resolution((const char *)p + 29, pl - 29, &x, &y);
        NEAR_DPI(has, x, y, 240.0f);
        CHECK_EQ_INT(pc_cdecode(out.data, out.len, &back, NULL), PC_OK);
        NEAR_DPI(back.meta.has_dpi, back.meta.dpi_x, back.meta.dpi_y, 240.0f);
        pc_cimage_fini(&back);
        pc_cimage_fini(&img);
        pc_buf_fini(&out);
        pc_buf_fini(&src);
        pc_buf_fini(&s);
        pc_buf_fini(&e);
    }
}

static void test_other_formats_take_the_exif_resolution_when_they_have_none(void)
{
    static const pc_cformat FS[2] = {PC_CF_WEBP, PC_CF_PNG};
    uint8_t px[48];
    pc_cimage img;
    pc_buf e = camera_exif(false);
    int k;
    memset(px, 90, sizeof px);
    (void)pc_cimage_from_copy(4, 4, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, NULL);
    (void)pc_meta_blob_set(&img.meta.exif, e.data, e.len, NULL);
    for (k = 0; k < 2; k++) {
        pc_buf out = PC_BUF_INIT;
        pc_cimage back;
        float x;
        float y;
        bool has;
        img.meta.has_dpi = true;
        img.meta.dpi_x = img.meta.dpi_y = 240.0f;
        CHECK_EQ_INT(pc_cencode(&img, FS[k], NULL, &out, NULL), PC_OK);
        CHECK_EQ_INT(pc_cdecode(out.data, out.len, &back, NULL), PC_OK);
        NEAR_DPI(back.meta.has_dpi, back.meta.dpi_x, back.meta.dpi_y, 240.0f);
        has = back.meta.exif.has && pc_exif_resolution(back.meta.exif.data, back.meta.exif.len, &x, &y);
        NEAR_DPI(has, x, y, 240.0f);
        pc_cimage_fini(&back);
        img.meta.has_dpi = false;
        CHECK_EQ_INT(pc_cencode(&img, FS[k], NULL, &out, NULL), PC_OK);
        CHECK_EQ_INT(pc_cdecode(out.data, out.len, &back, NULL), PC_OK);
        NEAR_DPI(back.meta.has_dpi, back.meta.dpi_x, back.meta.dpi_y, 300.0f);
        pc_cimage_fini(&back);
        pc_buf_fini(&out);
    }
    pc_cimage_fini(&img);
    pc_buf_fini(&e);
}

static void test_png_phys_outranks_its_exif(void)
{
    uint8_t px[48];
    pc_cimage img;
    pc_cimage back;
    pc_buf e = camera_exif(false);
    pc_buf png = PC_BUF_INIT;
    pc_buf with = PC_BUF_INIT;
    uint8_t body[13] = {'p', 'H', 'Y', 's', 0, 0, 0x17, 0x12, 0, 0, 0x17, 0x12, 1};
    uint8_t h[4] = {0, 0, 0, 9};
    uint8_t crc[4];
    size_t ihdr_end = 8 + 8 + 13 + 4;
    memset(px, 90, sizeof px);
    (void)pc_cimage_from_copy(4, 4, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, NULL);
    (void)pc_meta_blob_set(&img.meta.exif, e.data, e.len, NULL);
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_PNG, NULL, &png, NULL), PC_OK);
    CHECK_EQ_INT(pc_cdecode(png.data, png.len, &back, NULL), PC_OK);
    NEAR_DPI(back.meta.has_dpi, back.meta.dpi_x, back.meta.dpi_y, 300.0f);
    pc_cimage_fini(&back);
    put32(true, crc, pc_crc32(0, body, 13));
    (void)pc_buf_append(&with, png.data, ihdr_end, NULL);
    (void)pc_buf_append(&with, h, 4, NULL);
    (void)pc_buf_append(&with, body, 13, NULL);
    (void)pc_buf_append(&with, crc, 4, NULL);
    (void)pc_buf_append(&with, png.data + ihdr_end, png.len - ihdr_end, NULL);
    CHECK_EQ_INT(pc_cdecode(with.data, with.len, &back, NULL), PC_OK);
    NEAR_DPI(back.meta.has_dpi, back.meta.dpi_x, back.meta.dpi_y, 150.0114f);
    pc_cimage_fini(&back);
    pc_cimage_fini(&img);
    pc_buf_fini(&png);
    pc_buf_fini(&with);
    pc_buf_fini(&e);
}

/* camera_exif plus IFD1 naming a fake JPEG thumbnail after the IFD. */
static pc_buf exif_with_thumbnail(bool big)
{
    static const char thumb[] = "\xFF\xD8stale thumbnail\xFF\xD9";
    pc_buf v = camera_exif(big);
    uint32_t ifd1 = (uint32_t)v.len;
    uint32_t thumb_at = ifd1 + 2 + 2 * 12 + 4;
    uint8_t x[12];
    int k;
    put32(big, v.data + 8 + 2 + 4 * 12, ifd1);
    put16(big, x, 2);
    (void)pc_buf_append(&v, x, 2, NULL);
    for (k = 0; k < 2; k++) {
        put16(big, x, k == 0 ? 0x0201 : 0x0202);
        put16(big, x + 2, LONG);
        put32(big, x + 4, 1);
        put32(big, x + 8, k == 0 ? thumb_at : (uint32_t)(sizeof thumb - 1));
        (void)pc_buf_append(&v, x, 12, NULL);
    }
    put32(big, x, 0);
    (void)pc_buf_append(&v, x, 4, NULL);
    (void)pc_buf_append(&v, thumb, sizeof thumb - 1, NULL);
    return v;
}

static uint32_t next_ifd(bool big, const uint8_t *exif)
{
    return rd32(big, exif + 8 + 2 + 4 * 12);
}

static void test_exports_drop_the_source_exif_thumbnail(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        pc_buf exif = exif_with_thumbnail(big);
        const float p300[2] = {300.0f, 300.0f};
        const float p240[2] = {240.0f, 240.0f};
        const float *ppis[3] = {NULL, p300, p240};
        uint8_t *o = NULL;
        size_t ol = 0;
        int k;
        CHECK(next_ifd(big, exif.data) != 0);
        for (k = 0; k < 3; k++) {
            float x;
            float y;
            bool has;
            CHECK_EQ_INT(pc_export_exif(exif.data, exif.len, ppis[k], &o, &ol, NULL), PC_OK);
            CHECK(o != NULL && ol == exif.len);
            if (o != NULL) {
                CHECK_EQ_UINT(next_ifd(big, o), 0);
                CHECK(contains(o, ol, "NIKON", 6));
                has = pc_exif_resolution(o, ol, &x, &y);
                NEAR_DPI(has, x, y, ppis[k] == NULL ? 300.0f : ppis[k][0]);
            }
            if (k == 0 && o != NULL) {
                size_t i;
                for (i = 0; i < ol; i++) {
                    if (o[i] != exif.data[i]) {
                        CHECK(i >= 8 + 2 + 4 * 12 && i < 8 + 2 + 4 * 12 + 4);
                    }
                }
            }
            free(o);
            o = NULL;
        }
        {
            pc_buf pre = PC_BUF_INIT;
            pc_buf cam = camera_exif(big);
            (void)pc_buf_append(&pre, "Exif\0\0", 6, NULL);
            (void)pc_buf_append(&pre, exif.data, exif.len, NULL);
            CHECK_EQ_INT(pc_export_exif(pre.data, pre.len, NULL, &o, &ol, NULL), PC_OK);
            CHECK(o != NULL && next_ifd(big, o + 6) == 0);
            free(o);
            o = NULL;
            CHECK_EQ_INT(pc_export_exif(cam.data, cam.len, NULL, &o, &ol, NULL), PC_OK);
            CHECK(o == NULL);
            pc_buf_fini(&pre);
            pc_buf_fini(&cam);
        }
        /* Through every encoder that writes EXIF. */
        {
            uint8_t px[48];
            pc_cimage img;
            pc_buf jpeg = PC_BUF_INIT;
            const uint8_t *p;
            size_t pl;
            static const pc_cformat FS[2] = {PC_CF_PNG, PC_CF_WEBP};
            memset(px, 90, sizeof px);
            (void)pc_cimage_from_copy(4, 4, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, NULL);
            (void)pc_meta_blob_set(&img.meta.exif, exif.data, exif.len, NULL);
            CHECK_EQ_INT(pc_cencode(&img, PC_CF_JPEG, NULL, &jpeg, NULL), PC_OK);
            p = app_payload(&jpeg, 0xE1, "Exif\0\0", 6, &pl);
            CHECK(p != NULL && next_ifd(big, p + 6) == 0);
            for (k = 0; k < 2; k++) {
                pc_buf out = PC_BUF_INIT;
                pc_cimage back;
                CHECK_EQ_INT(pc_cencode(&img, FS[k], NULL, &out, NULL), PC_OK);
                CHECK_EQ_INT(pc_cdecode(out.data, out.len, &back, NULL), PC_OK);
                CHECK(back.meta.exif.has);
                if (back.meta.exif.has) {
                    const uint8_t *body = back.meta.exif.data;
                    if (back.meta.exif.len >= 6 && memcmp(body, "Exif\0\0", 6) == 0) {
                        body += 6;
                    }
                    CHECK_EQ_UINT(next_ifd(big, body), 0);
                }
                pc_cimage_fini(&back);
                pc_buf_fini(&out);
            }
            pc_cimage_fini(&img);
            pc_buf_fini(&jpeg);
        }
        /* thumbnail_removal_never_panics_on_truncated_exif */
        {
            size_t n;
            for (n = 0; n < exif.len; n++) {
                if (pc_export_exif(exif.data, n, NULL, &o, &ol, NULL) == PC_OK) {
                    free(o);
                }
                if (pc_export_exif(exif.data, n, p300, &o, &ol, NULL) == PC_OK) {
                    free(o);
                }
            }
        }
        pc_buf_fini(&exif);
    }
}

/* ---- exif_color.rs ---- */

static pc_buf color_exif(bool big, const uint32_t (*es)[4], size_t n)
{
    pc_buf b = PC_BUF_INIT;
    uint8_t x[16];
    size_t i;
    (void)pc_buf_append(&b, big ? "MM\0\x2a" : "II\x2a\0", 4, NULL);
    put32(big, x, 8);
    (void)pc_buf_append(&b, x, 4, NULL);
    put16(big, x, 1);
    put16(big, x + 2, 0x8769);
    put16(big, x + 4, LONG);
    put32(big, x + 6, 1);
    put32(big, x + 10, 26);
    (void)pc_buf_append(&b, x, 2, NULL);
    (void)pc_buf_append(&b, x + 2, 12, NULL);
    put32(big, x, 0);
    (void)pc_buf_append(&b, x, 4, NULL);
    put16(big, x, (uint16_t)n);
    (void)pc_buf_append(&b, x, 2, NULL);
    for (i = 0; i < n; i++) {
        memset(x, 0, sizeof x);
        put16(big, x, (uint16_t)es[i][0]);
        put16(big, x + 2, (uint16_t)es[i][1]);
        put32(big, x + 4, es[i][2]);
        if (es[i][1] == SHORT) {
            put16(big, x + 8, (uint16_t)es[i][3]);
        } else {
            put32(big, x + 8, es[i][3]);
        }
        (void)pc_buf_append(&b, x, 12, NULL);
    }
    put32(big, x, 0);
    (void)pc_buf_append(&b, x, 4, NULL);
    return b;
}

static int cs_of(const pc_buf *b)
{
    uint16_t v = 0;
    return pc_exif_color_space(b->data, b->len, &v) ? (int)v : -1;
}

static void test_exif_color_space(void)
{
    int big;
    for (big = 0; big < 2; big++) {
        const uint32_t srgb_e[2][4] = {{0x9000, 7, 4, 0x30323330u}, {0xA001, SHORT, 1, 1}};
        const uint32_t unc[1][4] = {{0xA001, SHORT, 1, 0xFFFF}};
        const uint32_t lng[1][4] = {{0xA001, LONG, 1, 1}};
        pc_buf s = color_exif(big, srgb_e, 2);
        pc_buf p = PC_BUF_INIT;
        pc_buf u = color_exif(big, unc, 1);
        pc_buf l = color_exif(big, lng, 1);
        CHECK_EQ_INT(cs_of(&s), PC_EXIF_COLOR_SPACE_SRGB);
        (void)pc_buf_append(&p, "Exif\0\0", 6, NULL);
        (void)pc_buf_append(&p, s.data, s.len, NULL);
        CHECK_EQ_INT(cs_of(&p), 1);
        CHECK_EQ_INT(cs_of(&u), 0xFFFF);
        CHECK_EQ_INT(cs_of(&l), 1);
        pc_buf_fini(&s);
        pc_buf_fini(&p);
        pc_buf_fini(&u);
        pc_buf_fini(&l);
    }
    {
        const uint32_t none[1][4] = {{0x9000, 7, 4, 0}};
        const uint32_t cnt2[1][4] = {{0xA001, SHORT, 2, 1}};
        const uint32_t ascii[1][4] = {{0xA001, 2, 1, 1}};
        const uint32_t big_long[1][4] = {{0xA001, LONG, 1, 0x10000}};
        const uint32_t good_e[1][4] = {{0xA001, SHORT, 1, 1}};
        static const uint32_t ptrs[3] = {0xFFFFFFF0u, 4, 0xFFFFFFFFu};
        pc_buf b = color_exif(false, none, 1);
        pc_buf good = color_exif(false, good_e, 1);
        size_t n;
        int k;
        CHECK_EQ_INT(cs_of(&b), -1);
        pc_buf_fini(&b);
        b = color_exif(false, cnt2, 1);
        CHECK_EQ_INT(cs_of(&b), -1);
        pc_buf_fini(&b);
        b = color_exif(false, ascii, 1);
        CHECK_EQ_INT(cs_of(&b), -1);
        pc_buf_fini(&b);
        b = color_exif(false, big_long, 1);
        CHECK_EQ_INT(cs_of(&b), -1);
        pc_buf_fini(&b);
        for (n = 0; n < good.len - 4; n++) {
            uint16_t v;
            CHECK(!pc_exif_color_space(good.data, n, &v));
        }
        for (k = 0; k < 3; k++) {
            pc_buf c = PC_BUF_INIT;
            (void)pc_buf_append(&c, good.data, good.len, NULL);
            put32(false, c.data + 18, ptrs[k]);
            CHECK_EQ_INT(cs_of(&c), -1);
            pc_buf_fini(&c);
        }
        b = (pc_buf)PC_BUF_INIT;
        (void)pc_buf_append(&b, good.data, good.len, NULL);
        put16(false, b.data + 12, SHORT);
        CHECK_EQ_INT(cs_of(&b), -1);
        put16(false, b.data + 12, LONG);
        put16(false, b.data + 26, 0xFFFF);
        CHECK_EQ_INT(cs_of(&b), -1);
        pc_buf_fini(&b);
        CHECK(!pc_exif_color_space((const uint8_t *)"not exif", 8, &(uint16_t){0}));
        pc_buf_fini(&good);
    }
}

/* ---- source_settings ---- */

static pc_cimage photo(uint32_t w, uint32_t h, pc_clayout l)
{
    return synth(w, h, l, PC_CS_U8, 5, 0.03f);
}

static void test_jpeg_quality_is_estimated_within_two(void)
{
    static const uint8_t QS[15] = {1, 5, 10, 25, 40, 50, 60, 70, 75, 80, 85, 90, 95, 98, 100};
    static const pc_clayout LS[2] = {PC_CL_RGB, PC_CL_GRAY};
    int l;
    int k;
    for (l = 0; l < 2; l++) {
        pc_cimage img = photo(40, 24, LS[l]);
        for (k = 0; k < 15; k++) {
            pc_cencode_opts o = pc_cencode_opts_default();
            pc_buf b = PC_BUF_INIT;
            uint8_t est = 0;
            o.jpeg_quality = QS[k];
            CHECK_EQ_INT(pc_cencode(&img, PC_CF_JPEG, &o, &b, NULL), PC_OK);
            CHECK(pc_jpeg_quality(b.data, b.len, &est));
            if (abs((int)est - (int)QS[k]) > 2) {
                PC_T_FAIL("layout %d: saved at %u, estimated %u", l, (unsigned)QS[k], (unsigned)est);
            }
            pc_buf_fini(&b);
        }
        pc_cimage_fini(&img);
    }
}

static void test_jpeg_quality_of_hand_built_tables(void)
{
    static const uint8_t luma[64] = {
        16, 11,  12, 14, 12, 10, 16,  14,  13,  14, 18, 17,  16,  19,  24,  40,  26, 24,  22,  22, 24, 49,
        35, 37,  29, 40, 58, 51, 61,  60,  57,  51, 56, 55,  64,  72,  92,  78,  64, 68,  87,  69, 55, 56,
        80, 109, 81, 87, 95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99};
    pc_buf b = PC_BUF_INIT;
    uint8_t q = 0;
    (void)pc_buf_append(&b, "\xFF\xD8\xFF\xDB\x00\x43\x00", 7, NULL);
    (void)pc_buf_append(&b, luma, 64, NULL);
    (void)pc_buf_append(&b, "\xFF\xDA\0\2", 4, NULL);
    CHECK(pc_jpeg_quality(b.data, b.len, &q) && q == 50);
    pc_buf_fini(&b);
}

static void test_hostile_jpeg_tables_and_webp(void)
{
    pc_cimage img = photo(16, 16, PC_CL_RGB);
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf good = PC_BUF_INIT;
    pc_buf png = PC_BUF_INIT;
    uint8_t q;
    pc_webp_encoding w;
    size_t n;
    int k;
    o.jpeg_quality = 70;
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_JPEG, &o, &good, NULL), PC_OK);
    for (n = 0; n < good.len && n < 800; n++) {
        (void)pc_jpeg_quality(good.data, n, &q);
    }
    {
        uint8_t c[200];
        memset(c, 9, sizeof c);
        memcpy(c, "\xFF\xD8\xFF\xDB\x00\x0c\x00", 7);
        (void)pc_jpeg_quality(c, 16, &q);
        memcpy(c, "\xFF\xD8\xFF\xDB\x00\x43\x70", 7);
        (void)pc_jpeg_quality(c, 71, &q);
        memcpy(c, "\xFF\xD8\xFF\xDB\x00\x43\x0F", 7);
        (void)pc_jpeg_quality(c, 71, &q);
        memset(c, 0xFF, sizeof c);
        memcpy(c, "\xFF\xD8\xFF\xDB\x00\x44\x10", 7);
        (void)pc_jpeg_quality(c, 72, &q);
        memcpy(c, "\xFF\xD8\xFF\xDB\x00\x83\x10", 7);
        (void)pc_jpeg_quality(c, 135, &q);
        memset(c, 0, sizeof c);
        memcpy(c, "\xFF\xD8\xFF\xDB\x00\x43\x00", 7);
        (void)pc_jpeg_quality(c, 71, &q);
        memset(c, 0xFF, 64);
        (void)pc_jpeg_quality(c, 64, &q);
        (void)pc_jpeg_quality((const uint8_t *)"\xFF\xD8\xFF\xDB\xFF\xFF\x00\x01\x02\x03", 10, &q);
        (void)pc_jpeg_quality((const uint8_t *)"\xFF", 1, &q);
        (void)pc_jpeg_quality((const uint8_t *)"", 0, &q);
    }
    CHECK(!pc_jpeg_quality((const uint8_t *)"\xFF\xD8\xFF\xDB\x00\x00", 6, &q));
    CHECK(!pc_jpeg_quality((const uint8_t *)"\xFF\xD8\xFF\xDB\x00\x01", 6, &q));
    CHECK(!pc_jpeg_quality((const uint8_t *)"not a jpeg at all", 17, &q));
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_PNG, NULL, &png, NULL), PC_OK);
    CHECK(!pc_jpeg_quality(png.data, png.len, &q));
    /* hostile_webp_never_panics */
    for (k = 0; k < 2; k++) {
        pc_cencode_opts wo = pc_cencode_opts_default();
        pc_buf wb = PC_BUF_INIT;
        pc_cimage im2 = photo(24, 24, PC_CL_RGB);
        size_t i;
        wo.webp_lossless = k == 0;
        CHECK_EQ_INT(pc_cencode(&im2, PC_CF_WEBP, &wo, &wb, NULL), PC_OK);
        for (n = 0; n < wb.len && n < 400; n++) {
            (void)pc_webp_encoding_of(wb.data, n, &w);
        }
        for (i = 12; i < wb.len && i < 64; i++) {
            wb.data[i] ^= 0xA5;
            (void)pc_webp_encoding_of(wb.data, wb.len, &w);
        }
        pc_buf_fini(&wb);
        pc_cimage_fini(&im2);
    }
    (void)pc_webp_encoding_of((const uint8_t *)"RIFF\0\0\0\0WEBPVP8 \xff\xff\xff\xff", 20, &w);
    (void)pc_webp_encoding_of((const uint8_t *)"RIFF\0\0\0\0WEBPANMF\x03\0\0\0", 20, &w);
    CHECK(!pc_webp_encoding_of((const uint8_t *)"RIFF\0\0\0\0WAVE", 12, &w));
    pc_buf_fini(&good);
    pc_buf_fini(&png);
    pc_cimage_fini(&img);
}

static void test_webp_lossless_and_lossy_are_told_apart(void)
{
    static const uint8_t QS[7] = {10, 30, 50, 75, 80, 90, 100};
    pc_cimage img = photo(33, 21, PC_CL_RGB);
    pc_cimage rgba = photo(20, 20, PC_CL_RGBA);
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf b = PC_BUF_INIT;
    pc_webp_encoding w;
    int k;
    CHECK_EQ_INT(pc_cencode(&img, PC_CF_WEBP, &o, &b, NULL), PC_OK);
    CHECK(pc_webp_encoding_of(b.data, b.len, &w) && w.kind == PC_WEBP_LOSSLESS);
    for (k = 0; k < 7; k++) {
        o.webp_lossless = false;
        o.webp_quality = QS[k];
        CHECK_EQ_INT(pc_cencode(&img, PC_CF_WEBP, &o, &b, NULL), PC_OK);
        CHECK(pc_webp_encoding_of(b.data, b.len, &w) && w.kind == PC_WEBP_LOSSY && w.has_quality);
        if (w.has_quality && abs((int)w.quality - (int)QS[k]) > 2) {
            PC_T_FAIL("lossy WebP saved at %u, estimated %u", (unsigned)QS[k], (unsigned)w.quality);
        }
    }
    o.webp_quality = 60;
    CHECK_EQ_INT(pc_cencode(&rgba, PC_CF_WEBP, &o, &b, NULL), PC_OK);
    CHECK(pc_webp_encoding_of(b.data, b.len, &w) && w.kind == PC_WEBP_LOSSY && w.has_quality &&
          w.quality >= 58 && w.quality <= 62);
    pc_buf_fini(&b);
    pc_cimage_fini(&img);
    pc_cimage_fini(&rgba);
}

/* ---- web.rs ---- */

static void test_web_units(void)
{
    uint8_t rgb[8 * 8 * 3];
    static const char XMP[] = "<rdf:Description tiff:XResolution=\"300/1\" tiff:YResolution=\"300/1\"/>";
    pc_buf out = PC_BUF_INIT;
    pc_cimage img;
    float x;
    float y;
    bool has;
    memset(rgb, 100, sizeof rgb);
    CHECK_EQ_INT(pc_web_encode_jpeg_rgb8(8, 8, rgb, sizeof rgb, 80, false, false, NULL, 0, true, 72.0f, XMP,
                                         sizeof XMP - 1, &out, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_cdecode(out.data, out.len, &img, NULL), PC_OK);
    NEAR_DPI(img.meta.has_dpi, img.meta.dpi_x, img.meta.dpi_y, 72.0f);
    has = img.meta.xmp.has && pc_xmp_resolution((const char *)img.meta.xmp.data, img.meta.xmp.len, &x, &y);
    NEAR_DPI(has, x, y, 72.0f);
    pc_cimage_fini(&img);
    /* transparent_frames_are_cleared_before_the_next */
    {
        static const uint8_t idx[4] = {0, 1, 1, 1};
        static const uint8_t pal[2][3] = {{0, 0, 0}, {255, 0, 0}};
        pc_gif_frame f = {idx, 4, pal, 2, -1, 4};
        size_t i;
        uint8_t *px = NULL;
        size_t n = 0;
        CHECK_EQ_INT(pc_web_encode_gif_animated(2, 2, &f, 1, false, &out, NULL), PC_OK);
        for (i = 0; i + 4 <= out.len; i++) {
            if (out.data[i] == 0x21 && out.data[i + 1] == 0xF9 && out.data[i + 2] == 4) {
                CHECK_EQ_UINT(out.data[i + 3], 0x04);
                break;
            }
        }
        f.transparent = 0;
        CHECK_EQ_INT(pc_web_encode_gif_animated(2, 2, &f, 1, false, &out, NULL), PC_OK);
        for (i = 0; i + 4 <= out.len; i++) {
            if (out.data[i] == 0x21 && out.data[i + 1] == 0xF9 && out.data[i + 2] == 4) {
                CHECK_EQ_UINT(out.data[i + 3], 0x09);
                break;
            }
        }
        CHECK_EQ_INT(pc_cdecode(out.data, out.len, &img, NULL), PC_OK);
        CHECK_EQ_INT(pc_cimage_to_rgba8(&img, &px, &n, NULL), PC_OK);
        CHECK(n == 16 && px[3] == 0 && px[4] == 255 && px[5] == 0 && px[6] == 0 && px[7] == 255);
        free(px);
        pc_cimage_fini(&img);
    }
    pc_buf_fini(&out);
}

/* ---- png.rs: the parallel encoder gives the same bytes on any thread count. The hash printed
 * here is compared between PHOTOCRAFT_THREADS=1 and 8 runs by the R2 gate. ---- */

static void test_parallel_png_encoder(void)
{
    const size_t w = 1201;
    const size_t h = 1013;
    size_t n = w * h * 4;
    uint8_t *px = malloc(n);
    static const pc_png_compression LV[3] = {PC_PNG_FAST, PC_PNG_DEFAULT, PC_PNG_BEST};
    uint64_t hsum = 0xcbf29ce484222325ull;
    size_t i;
    int k;
    if (px == NULL) {
        CHECK(false);
        return;
    }
    for (i = 0; i < n; i++) {
        uint32_t v = (uint32_t)(i / 5) * 2654435761u;
        unsigned r = (unsigned)(i % 11);
        v = r == 0 ? v : (v << r) | (v >> (32 - r));
        px[i] = (uint8_t)((uint8_t)v / 5 + (uint8_t)(i % 89));
    }
    for (k = 0; k < 3; k++) {
        pc_cimage img;
        pc_cimage back;
        pc_cencode_opts o = pc_cencode_opts_default();
        pc_buf png = PC_BUF_INIT;
        o.png_compression = LV[k];
        (void)pc_cimage_from_copy((uint32_t)w, (uint32_t)h, PC_CL_RGBA, PC_CS_U8, px, n, &img, NULL);
        CHECK_EQ_INT(pc_cencode(&img, PC_CF_PNG, &o, &png, NULL), PC_OK);
        for (i = 0; i < png.len; i++) {
            hsum ^= png.data[i];
            hsum *= 0x100000001b3ull;
        }
        CHECK_EQ_INT(pc_cdecode(png.data, png.len, &back, NULL), PC_OK);
        CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
        pc_cimage_fini(&back);
        pc_cimage_fini(&img);
        pc_buf_fini(&png);
    }
    (void)printf("(png bytes %016llx) ", (unsigned long long)hsum);
    free(px);
}

/* ---- tiff_ifd.rs: overlapping directory tables exhaust the entry budget ---- */

static void test_overlapping_directory_tables_exhaust_the_entry_budget(void)
{
    const size_t first = 8;
    const size_t table = 2 + 65535 * 12 + 4;
    size_t len = first + table + 4 * 10000;
    uint8_t *b = calloc(len, 1);
    pc_tiff_info info;
    size_t i;
    if (b == NULL) {
        CHECK(false);
        return;
    }
    memcpy(b, "II*\0\x08\0\0\0", 8);
    for (i = 0; i < 10000; i++) {
        size_t at = first + 4 * i;
        size_t next_cell = at + 2 + 65535 * 12;
        b[at] = 0xFF;
        b[at + 1] = 0xFF;
        if (next_cell + 4 <= len) {
            put32(false, b + next_cell, (uint32_t)(first + 4 * (i + 1)));
        }
    }
    CHECK_EQ_INT(pc_ctiff_info(b, len, &info, NULL), PC_OK);
    CHECK(!info.complete);
    CHECK(info.npages < 200);
    pc_tiff_info_fini(&info);
    free(b);
}

/* ---- pnm_peak_memory.rs (#2938): decoding a 16 MiB 8-bit PGM raises peak RSS by under twice the
 * output (Linux only; skipped where /proc/self/clear_refs is not writable). ---- */

static long long peak_rss(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long long kib = -1;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            kib = atoll(line + 6);
            break;
        }
    }
    (void)fclose(f);
    return kib < 0 ? -1 : kib * 1024;
}

/* Sanitizers keep shadow memory per allocated byte: the peak RSS then says nothing. */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define PEAK_RSS_MEANINGFUL 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define PEAK_RSS_MEANINGFUL 0
#endif
#endif
#ifndef PEAK_RSS_MEANINGFUL
#define PEAK_RSS_MEANINGFUL 1
#endif

static void test_pnm_decode_peak_memory(void)
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    /* Sanitizer shadow memory grows RSS with every allocation; the bound only holds without. */
    return;
#endif
    const size_t side = 4096;
    size_t out = side * side;
    pc_buf file = PC_BUF_INIT;
    char head[64];
    FILE *cr;
    long long before;
    long long after;
    pc_cimage img;
    size_t i;
    int hl = snprintf(head, sizeof head, "P5\n%zu %zu\n255\n", side, side);
    if (!PEAK_RSS_MEANINGFUL) {
        return;
    }
    (void)pc_buf_append(&file, head, (size_t)hl, NULL);
    if (pc_buf_reserve(&file, out, NULL) != PC_OK) {
        CHECK(false);
        return;
    }
    for (i = 0; i < out; i++) {
        file.data[file.len + i] = (uint8_t)(i % 251);
    }
    file.len += out;
    cr = fopen("/proc/self/clear_refs", "w");
    if (cr == NULL || fputs("5", cr) < 0 || fclose(cr) != 0) {
        pc_buf_fini(&file);
        return;
    }
    before = peak_rss();
    if (before < 0) {
        pc_buf_fini(&file);
        return;
    }
    CHECK_EQ_INT(pc_cdecode(file.data, file.len, &img, NULL), PC_OK);
    after = peak_rss();
    CHECK_EQ_UINT(img.len, out);
    CHECK(img.len == out && img.data[300] == 300 % 251);
    CHECK(after - before < (long long)(2 * out));
    pc_cimage_fini(&img);
    pc_buf_fini(&file);
}

int main(int argc, char **argv)
{
    RUN(test_exif_only_camera_jpeg_opens_at_its_exif_resolution);
    RUN(test_exif_resolution_in_centimetres_is_converted);
    RUN(test_jfif_and_exif_precedence);
    RUN(test_photoshop_resolution_info_wins_then_xmp_then_exif);
    RUN(test_unusable_exif_falls_back_to_jfif);
    RUN(test_truncated_metadata_never_crashes_and_falls_back);
    RUN(test_hostile_bytes_never_crash);
    RUN(test_rewriting_keeps_every_other_byte);
    RUN(test_entries_that_cannot_be_rewritten_are_removed);
    RUN(test_xmp_resolution_is_rewritten);
    RUN(test_jpeg_export_writes_one_resolution_everywhere);
    RUN(test_other_formats_take_the_exif_resolution_when_they_have_none);
    RUN(test_png_phys_outranks_its_exif);
    RUN(test_exports_drop_the_source_exif_thumbnail);
    RUN(test_exif_color_space);
    RUN(test_jpeg_quality_is_estimated_within_two);
    RUN(test_jpeg_quality_of_hand_built_tables);
    RUN(test_hostile_jpeg_tables_and_webp);
    RUN(test_webp_lossless_and_lossy_are_told_apart);
    RUN(test_web_units);
    RUN(test_overlapping_directory_tables_exhaust_the_entry_budget);
    RUN(test_pnm_decode_peak_memory);
    RUN(test_parallel_png_encoder);
    return pc_t_finish();
}
