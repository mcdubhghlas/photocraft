#include <math.h>

#include "base/bytes.h"
#include "base/num.h"
#include "harness.h"

static void test_loads_and_stores(void)
{
    static const uint8_t d[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t out[8];
    CHECK_EQ_UINT(pc_load_be16(d), 0x0102);
    CHECK_EQ_UINT(pc_load_le16(d), 0x0201);
    CHECK_EQ_UINT(pc_load_be32(d), 0x01020304u);
    CHECK_EQ_UINT(pc_load_le32(d), 0x04030201u);
    CHECK_EQ_UINT(pc_load_be64(d), 0x0102030405060708u);
    CHECK_EQ_UINT(pc_load_le64(d), 0x0807060504030201u);
    pc_store_be64(out, 0x0102030405060708u);
    CHECK_EQ_MEM(out, d, 8);
    pc_store_le64(out, 0x0807060504030201u);
    CHECK_EQ_MEM(out, d, 8);
    pc_store_be16(out, 0xABCD);
    CHECK(out[0] == 0xAB && out[1] == 0xCD);
    pc_store_le32(out, 0xDEADBEEFu);
    CHECK(out[0] == 0xEF && out[3] == 0xDE);
}

static void test_reader_reads_in_order(void)
{
    static const uint8_t d[] = {0xFF, 0x80, 0x00, 0x3F, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFE, 0x12};
    pc_rd r;
    pc_rd_init(&r, d, sizeof d);
    CHECK_EQ_INT(pc_rd_be16s(&r), -128); /* 0xFF80 */
    CHECK_EQ_UINT(pc_f32_to_bits(pc_rd_bef32(&r)), 0x003F8000u); /* a subnormal: bits survive */
    CHECK(pc_rd_ok(&r));
    CHECK_EQ_INT(pc_rd_be32s(&r), pc_i32_from_bits(0x00FFFFFFu));
    CHECK_EQ_UINT(pc_rd_remaining(&r), 2);
    CHECK_EQ_UINT(pc_rd_le16(&r), 0x12FE);
    CHECK_EQ_UINT(pc_rd_remaining(&r), 0);
    CHECK(pc_rd_ok(&r));
}

static void test_reader_float_bits(void)
{
    static const uint8_t one[] = {0x3F, 0x80, 0x00, 0x00, 0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2D, 0x18};
    pc_rd r;
    pc_rd_init(&r, one, sizeof one);
    CHECK_NEAR(pc_rd_bef32(&r), 1.0, 0.0);
    CHECK_NEAR(pc_rd_bef64(&r), 3.141592653589793, 0.0);
    CHECK(pc_rd_ok(&r));
    CHECK(isnan(pc_f32_from_bits(0x7FC00000u)));
    CHECK_EQ_UINT(pc_f32_to_bits(-0.0f), 0x80000000u);
    CHECK_EQ_UINT(pc_f64_to_bits(1.0), 0x3FF0000000000000u);
}

static void test_reader_errors_are_sticky(void)
{
    static const uint8_t d[] = {1, 2, 3};
    pc_rd r;
    pc_rd_init(&r, d, sizeof d);
    CHECK_EQ_UINT(pc_rd_be32(&r), 0);
    CHECK(!pc_rd_ok(&r));
    CHECK_EQ_UINT(pc_rd_u8(&r), 0); /* still failing even though a byte exists */
    CHECK_EQ_UINT(pc_rd_remaining(&r), 0);
    CHECK(pc_rd_bytes(&r, 0) == NULL);

    pc_rd_init(&r, d, sizeof d);
    CHECK(!pc_rd_skip(&r, 4));
    pc_rd_init(&r, d, sizeof d);
    CHECK(!pc_rd_skip(&r, SIZE_MAX));
    pc_rd_init(&r, d, sizeof d);
    CHECK(pc_rd_seek(&r, 3));
    CHECK(pc_rd_ok(&r));
    CHECK(!pc_rd_seek(&r, 4));
    CHECK(!pc_rd_ok(&r));

    pc_rd_init(&r, NULL, 100); /* NULL data is an empty reader */
    CHECK_EQ_UINT(pc_rd_remaining(&r), 0);
    CHECK_EQ_UINT(pc_rd_u8(&r), 0);
    CHECK(!pc_rd_ok(&r));
}

static void test_sub_reader(void)
{
    static const uint8_t d[] = {0, 0, 0, 2, 0xAA, 0xBB, 0xCC};
    pc_rd r;
    pc_rd sub;
    pc_rd_init(&r, d, sizeof d);
    sub = pc_rd_sub(&r, (size_t)pc_rd_be32(&r));
    CHECK_EQ_UINT(pc_rd_u8(&sub), 0xAA);
    CHECK_EQ_UINT(pc_rd_u8(&sub), 0xBB);
    CHECK_EQ_UINT(pc_rd_u8(&sub), 0); /* can't read past its window */
    CHECK(!pc_rd_ok(&sub));
    CHECK(pc_rd_ok(&r));
    CHECK_EQ_UINT(pc_rd_u8(&r), 0xCC);

    pc_rd_init(&r, d, sizeof d);
    sub = pc_rd_sub(&r, 100);
    CHECK(!pc_rd_ok(&sub));
    CHECK(!pc_rd_ok(&r));
}

static void test_signed_from_bits(void)
{
    CHECK_EQ_INT(pc_i16_from_bits(0x8000), INT16_MIN);
    CHECK_EQ_INT(pc_i16_from_bits(0x7FFF), INT16_MAX);
    CHECK_EQ_INT(pc_i16_from_bits(0xFFFF), -1);
    CHECK_EQ_INT(pc_i64_from_bits(0x8000000000000000u), INT64_MIN);
    CHECK_EQ_INT(pc_i64_from_bits(UINT64_MAX), -1);
}

static void test_writers(void)
{
    pc_buf b = PC_BUF_INIT;
    static const uint8_t want[] = {0x12, 0x34, 0x34, 0x12, 0, 0, 0, 1, 1, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0};
    CHECK_EQ_INT(pc_buf_put_be16(&b, 0x1234, NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_put_le16(&b, 0x1234, NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_put_be32(&b, 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_put_le32(&b, 1, NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_put_be64(&b, 2, NULL), PC_OK);
    CHECK_EQ_INT(pc_buf_put_le64(&b, 2, NULL), PC_OK);
    CHECK_EQ_UINT(b.len, sizeof want);
    CHECK_EQ_MEM(b.data, want, sizeof want);
    pc_buf_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_loads_and_stores);
    RUN(test_reader_reads_in_order);
    RUN(test_reader_float_bits);
    RUN(test_reader_errors_are_sticky);
    RUN(test_sub_reader);
    RUN(test_signed_from_bits);
    RUN(test_writers);
    return pc_t_finish();
}
