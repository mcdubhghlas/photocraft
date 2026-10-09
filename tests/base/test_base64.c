/* Expected results and messages come from base64 0.22.1 STANDARD via tools/rustref. */
#include <string.h>

#include "base/base64.h"
#include "harness.h"

static void check_encode(const char *in, const char *want)
{
    pc_buf b = PC_BUF_INIT;
    CHECK_EQ_INT(pc_base64_encode((const uint8_t *)in, strlen(in), &b, NULL), PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&b), want);
    pc_buf_fini(&b);
}

static void check_decode_ok(const char *in, const char *want)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    CHECK_EQ_INT(pc_base64_decode(in, strlen(in), &b, &e), PC_OK);
    CHECK_EQ_UINT(b.len, strlen(want));
    CHECK_EQ_STR(pc_buf_cstr(&b), want);
    pc_buf_fini(&b);
}

static void check_decode_err(const char *in, const char *msg)
{
    pc_buf b = PC_BUF_INIT;
    pc_error e;
    CHECK_EQ_INT(pc_buf_append_str(&b, "keep", NULL), PC_OK);
    CHECK_EQ_INT(pc_base64_decode(in, strlen(in), &b, &e), PC_ERR_FORMAT);
    CHECK_EQ_STR(e.msg, msg);
    CHECK_EQ_STR(pc_buf_cstr(&b), "keep");
    pc_buf_fini(&b);
}

static void test_encode(void)
{
    check_encode("", "");
    check_encode("f", "Zg==");
    check_encode("fo", "Zm8=");
    check_encode("foo", "Zm9v");
    check_encode("foobar", "Zm9vYmFy");
    {
        static const uint8_t bin[] = {0xFB, 0xFF, 0x00};
        pc_buf b = PC_BUF_INIT;
        CHECK_EQ_INT(pc_base64_encode(bin, sizeof bin, &b, NULL), PC_OK);
        CHECK_EQ_STR(pc_buf_cstr(&b), "+/8A");
        pc_buf_fini(&b);
    }
}

static void test_decode_matches_crate(void)
{
    check_decode_ok("", "");
    check_decode_ok("QQ==", "A");
    check_decode_ok("QUJD", "ABC");
    check_decode_ok("QUJDRA==", "ABCD");
    check_decode_ok("Zm9vYmFy", "foobar");
    check_decode_err("QQ", "Invalid padding");
    check_decode_err("QQ=", "Invalid padding");
    check_decode_err("Q", "Invalid input length: 1");
    check_decode_err("QR==", "Invalid last symbol 82, offset 1.");
    check_decode_err("QU=D", "Invalid symbol 61, offset 2.");
    check_decode_err("Q===", "Invalid symbol 61, offset 1.");
    check_decode_err("QUJD====", "Invalid symbol 61, offset 4.");
    check_decode_err("QUJ!", "Invalid symbol 33, offset 3.");
    check_decode_err("!UJD", "Invalid symbol 33, offset 0.");
    check_decode_err("QUJDR===", "Invalid symbol 61, offset 5.");
    check_decode_err("Zm9vYg", "Invalid padding");
    check_decode_err("Zm9v\nYmFy", "Invalid symbol 10, offset 4.");
}

static void test_roundtrip_all_lengths(void)
{
    uint8_t data[300];
    size_t n;
    int bad = 0;
    for (n = 0; n < sizeof data; n++) {
        data[n] = (uint8_t)(n * 37 + 11);
    }
    for (n = 0; n <= sizeof data; n++) {
        pc_buf enc = PC_BUF_INIT;
        pc_buf dec = PC_BUF_INIT;
        if (pc_base64_encode(data, n, &enc, NULL) != PC_OK ||
            pc_base64_decode(pc_buf_cstr(&enc), enc.len, &dec, NULL) != PC_OK || dec.len != n ||
            (n > 0 && memcmp(dec.data, data, n) != 0)) {
            bad++;
        }
        pc_buf_fini(&enc);
        pc_buf_fini(&dec);
    }
    CHECK_EQ_INT(bad, 0);
}

int main(int argc, char **argv)
{
    RUN(test_encode);
    RUN(test_decode_matches_crate);
    RUN(test_roundtrip_all_lengths);
    return pc_t_finish();
}
