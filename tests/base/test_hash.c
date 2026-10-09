#include <string.h>

#include "base/hash.h"
#include "harness.h"

static void test_blake3_known_vectors(void)
{
    char hex[PC_BLAKE3_HEX_LEN + 1];
    uint8_t raw[PC_BLAKE3_LEN];
    pc_blake3_hex("", 0, hex);
    CHECK_EQ_STR(hex, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
    pc_blake3_hex("abc", 3, hex);
    CHECK_EQ_STR(hex, "6437b3ac38465133ffb63b75273a8db548c558465d79db03fd359c6cd5bd9d85");
    pc_blake3("abc", 3, raw);
    CHECK_EQ_UINT(raw[0], 0x64);
    CHECK_EQ_UINT(raw[31], 0x85);
}

static void test_hex(void)
{
    static const uint8_t b[] = {0x00, 0x0F, 0xA0, 0xFF};
    char out[9];
    pc_hex_encode(b, sizeof b, out);
    CHECK_EQ_STR(out, "000fa0ff");
    pc_hex_encode(b, 0, out);
    CHECK_EQ_STR(out, "");
}

int main(int argc, char **argv)
{
    RUN(test_blake3_known_vectors);
    RUN(test_hex);
    return pc_t_finish();
}
