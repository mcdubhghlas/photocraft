/* exif.rs: round trip, garbage, truncation and hostile IFD structures. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/exif.h"
#include "base/bytes.h"
#include "harness.h"

static pc_algo_exif_str S(char *s)
{
    pc_algo_exif_str r;
    r.s = s;
    r.len = strlen(s);
    return r;
}

static pc_algo_exif_num N(double v)
{
    pc_algo_exif_num r = {true, v};
    return r;
}

static pc_algo_camera_info sample_info(void)
{
    pc_algo_camera_info info;
    memset(&info, 0, sizeof info);
    info.make = S("Generic");
    info.model = S("Camera 1");
    info.lens = S("24-70mm");
    info.exposure_time = N(0.004);
    info.f_number = N(8.0);
    info.iso = N(200.0);
    info.focal_length = N(35.0);
    info.focal_length_35mm = N(52.0);
    info.date_taken = S("2024:02:17 12:40:19");
    return info;
}

static void read_ok(const uint8_t *b, size_t n, pc_algo_camera_info *out)
{
    CHECK(pc_algo_exif_read(b, n, out, NULL) == PC_OK);
}

static void test_round_trips_and_survives_garbage(void)
{
    pc_algo_camera_info info = sample_info(), r, empty;
    pc_buf b = PC_BUF_INIT, pre = PC_BUF_INIT;
    double f = 0.0;
    size_t cuts[4], i;
    memset(&empty, 0, sizeof empty);
    CHECK(pc_algo_exif_build(&info, &b, NULL) == PC_OK);
    read_ok(b.data, b.len, &r);
    CHECK(pc_algo_camera_info_equal(&r, &info));
    CHECK(pc_algo_exif_exposure_factor(&r, &f) && fabs(f - 0.004 * 2.0 / 64.0) < 1e-9);
    pc_algo_camera_info_fini(&r);
    CHECK(pc_buf_append(&pre, "Exif\0\0", 6, NULL) == PC_OK);
    CHECK(pc_buf_append(&pre, b.data, b.len, NULL) == PC_OK);
    read_ok(pre.data, pre.len, &r);
    CHECK(pc_algo_camera_info_equal(&r, &info));
    pc_algo_camera_info_fini(&r);
    read_ok((const uint8_t *)"nonsense", 8, &r);
    CHECK(pc_algo_camera_info_equal(&r, &empty));
    cuts[0] = 5, cuts[1] = 12, cuts[2] = 30, cuts[3] = b.len - 3;
    for (i = 0; i < 4; i++) {
        read_ok(b.data, cuts[i], &r);
        pc_algo_camera_info_fini(&r);
    }
    pc_buf_fini(&b);
    pc_buf_fini(&pre);
}

static void test_every_truncation_and_byte_flip(void)
{
    pc_algo_camera_info info = sample_info(), r;
    pc_buf b = PC_BUF_INIT;
    uint8_t *m;
    size_t i;
    int v;
    CHECK(pc_algo_exif_build(&info, &b, NULL) == PC_OK);
    m = malloc(b.len);
    for (i = 0; i <= b.len; i++) {
        /* Exact-size copies so ASan catches any over-read. */
        uint8_t *cut = malloc(i > 0 ? i : 1);
        memcpy(cut, b.data, i);
        read_ok(cut, i, &r);
        pc_algo_camera_info_fini(&r);
        free(cut);
    }
    for (i = 0; i < b.len; i++) {
        for (v = 0; v < 256; v += 51) {
            memcpy(m, b.data, b.len);
            m[i] = (uint8_t)v;
            read_ok(m, b.len, &r);
            pc_algo_camera_info_fini(&r);
        }
    }
    free(m);
    pc_buf_fini(&b);
}

static void test_hostile_structures(void)
{
    /* Big-endian, IFD pointing at itself, the Exif pointer pointing back at IFD0, huge counts and
     * offsets: no crash, no endless loop, fields left unset where unreadable. */
    uint8_t mm[64];
    pc_algo_camera_info r;
    memset(mm, 0, sizeof mm);
    memcpy(mm, "MM\0*", 4);
    pc_store_be32(mm + 4, 8);
    pc_store_be16(mm + 8, 3);
    /* Make: ASCII, count 0xFFFFFFFF at offset 0xFFFFFFF0 */
    pc_store_be16(mm + 10, 0x010F), pc_store_be16(mm + 12, 2), pc_store_be32(mm + 14, 0xFFFFFFFFu);
    pc_store_be32(mm + 18, 0xFFFFFFF0u);
    /* ExifIFD pointer back to IFD0 (cycle) */
    pc_store_be16(mm + 22, 0x8769), pc_store_be16(mm + 24, 4), pc_store_be32(mm + 26, 1),
        pc_store_be32(mm + 30, 8);
    /* ISO: SHORT inline = 400 */
    pc_store_be16(mm + 34, 0x8827), pc_store_be16(mm + 36, 3), pc_store_be32(mm + 38, 1),
        pc_store_be16(mm + 42, 400);
    read_ok(mm, sizeof mm, &r);
    CHECK(r.make.s == NULL);
    CHECK(r.iso.set && r.iso.v == 400.0);
    pc_algo_camera_info_fini(&r);
    /* A huge entry count that runs off the end. */
    pc_store_be16(mm + 8, 0xFFFF);
    read_ok(mm, sizeof mm, &r);
    pc_algo_camera_info_fini(&r);
    /* IFD offset past the end. */
    pc_store_be32(mm + 4, 0xFFFFFFFFu);
    read_ok(mm, sizeof mm, &r);
    CHECK(!r.iso.set);
    pc_algo_camera_info_fini(&r);
    read_ok(mm, 0, &r);
    pc_algo_camera_info_fini(&r);
}

static void test_values_and_text_rules(void)
{
    /* Signed rational, zero denominators, focal_35 of 0 dropped, whitespace / NUL trimmed text,
     * invalid UTF-8 replaced. */
    uint8_t b[128];
    pc_algo_camera_info r;
    double f;
    memset(b, 0, sizeof b);
    memcpy(b, "II*\0", 4);
    pc_store_le32(b + 4, 8);
    pc_store_le16(b + 8, 4);
    pc_store_le16(b + 10, 0x829A), pc_store_le16(b + 12, 10), pc_store_le32(b + 14, 1),
        pc_store_le32(b + 18, 80);
    pc_store_le16(b + 22, 0x829D), pc_store_le16(b + 24, 5), pc_store_le32(b + 26, 1),
        pc_store_le32(b + 30, 88);
    pc_store_le16(b + 34, 0xA405), pc_store_le16(b + 36, 3), pc_store_le32(b + 38, 1),
        pc_store_le16(b + 42, 0);
    pc_store_le16(b + 46, 0x0110), pc_store_le16(b + 48, 2), pc_store_le32(b + 50, 10),
        pc_store_le32(b + 54, 96);
    pc_store_le32(b + 80, (uint32_t)-3), pc_store_le32(b + 84, 2); /* -1.5 */
    pc_store_le32(b + 88, 5), pc_store_le32(b + 92, 0);            /* x / 0 */
    memcpy(b + 96,
           "  A\xff"
           "B \0\0\0\0",
           10);
    read_ok(b, sizeof b, &r);
    CHECK(r.exposure_time.set && r.exposure_time.v == -1.5);
    CHECK(!r.f_number.set);
    CHECK(!r.focal_length_35mm.set);
    CHECK(r.model.s != NULL && r.model.len == 5 &&
          memcmp(r.model.s,
                 "A\xef\xbf\xbd"
                 "B",
                 5) == 0);
    CHECK(pc_algo_exif_exposure_factor(&r, &f) && f == -1.5); /* N = 1, ISO 100 */
    pc_algo_camera_info_fini(&r);
}

static void put_entry(uint8_t *b, uint16_t tag, uint16_t typ, uint32_t count, uint32_t value)
{
    pc_store_le16(b, tag);
    pc_store_le16(b + 2, typ);
    pc_store_le32(b + 4, count);
    pc_store_le32(b + 8, value);
}

static void read_any(const uint8_t *b, size_t n)
{
    pc_algo_camera_info r;
    uint8_t *copy = malloc(n > 0 ? n : 1); /* exact size, so sanitizers see any over-read */
    memcpy(copy, b, n);
    read_ok(copy, n, &r);
    pc_algo_camera_info_fini(&r);
    free(copy);
}

/* hostile_payloads_never_panic: huge counts, offsets near u32::MAX, self-referencing IFDs and
 * random bytes read as unset fields. */
static void test_hostile_payloads_never_panic(void)
{
    uint8_t b[8 + 2 + 7 * 12], be[8];
    pc_algo_camera_info r, empty, good;
    pc_buf g = PC_BUF_INIT;
    uint32_t x = 0x2545F491u;
    size_t k, cut;
    memset(&empty, 0, sizeof empty);
    memcpy(b, "II*\0", 4);
    pc_store_le32(b + 4, 8);
    pc_store_le16(b + 8, 7 + 3); /* claims more entries than exist */
    put_entry(b + 10, 0x010F, 2, UINT32_MAX, 8);
    put_entry(b + 22, 0x0110, 2, 16, UINT32_MAX - 1);
    put_entry(b + 34, 0x829A, 5, UINT32_MAX, 20);
    put_entry(b + 46, 0x829D, 5, 1, UINT32_MAX - 3);
    put_entry(b + 58, 0x9003, 2, 0x40000000u, 8);
    put_entry(b + 70, 0x8769, 4, 1, 8);
    put_entry(b + 82, 0x8769, 4, 1, UINT32_MAX);
    read_ok(b, sizeof b, &r);
    CHECK(pc_algo_camera_info_equal(&r, &empty));
    pc_algo_camera_info_fini(&r);
    memcpy(be, "MM\0*", 4);
    pc_store_be32(be + 4, UINT32_MAX);
    read_ok(be, sizeof be, &r);
    CHECK(pc_algo_camera_info_equal(&r, &empty));
    pc_algo_camera_info_fini(&r);
    for (k = 0; k < 200; k++) {
        uint8_t n[8 + 96];
        size_t i;
        memcpy(n, "II*\0\x08\0\0\0", 8);
        for (i = 0; i < 96; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            n[8 + i] = (uint8_t)x;
        }
        read_any(n, sizeof n);
    }
    memset(&good, 0, sizeof good);
    good.make = S("Make");
    good.date_taken = S("2024:02:17 12:40:19");
    good.exposure_time = N(0.004);
    CHECK(pc_algo_exif_build(&good, &g, NULL) == PC_OK);
    read_ok(g.data, g.len, &r);
    CHECK(pc_algo_camera_info_equal(&r, &good));
    pc_algo_camera_info_fini(&r);
    for (cut = 0; cut < g.len; cut++) {
        read_any(g.data, cut);
    }
    pc_buf_fini(&g);
}

int main(int argc, char **argv)
{
    RUN(test_round_trips_and_survives_garbage);
    RUN(test_every_truncation_and_byte_flip);
    RUN(test_hostile_structures);
    RUN(test_values_and_text_rules);
    RUN(test_hostile_payloads_never_panic);
    return pc_t_finish();
}
