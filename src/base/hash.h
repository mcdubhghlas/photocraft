/* BLAKE3 (libblake3) and hex helpers. Replaces the `blake3` crate (REWRITE_PLAN.md D15).
 * For incremental hashing use libblake3's blake3_hasher directly (#include <blake3.h>).
 * Thread-safe. */
#ifndef PC_BASE_HASH_H
#define PC_BASE_HASH_H

#include <stddef.h>
#include <stdint.h>

enum { PC_BLAKE3_LEN = 32, PC_BLAKE3_HEX_LEN = 64 };

/* out receives the 32-byte BLAKE3 hash of data (blake3::hash). */
void pc_blake3(const void *data, size_t len, uint8_t out[PC_BLAKE3_LEN]);

/* out receives the 64 lower-case hex digits plus NUL (blake3::hash(..).to_hex()). */
void pc_blake3_hex(const void *data, size_t len, char out[PC_BLAKE3_HEX_LEN + 1]);

/* Write 2 * len lower-case hex digits plus NUL into out (which holds 2 * len + 1 bytes). */
void pc_hex_encode(const uint8_t *bytes, size_t len, char *out);

#endif
