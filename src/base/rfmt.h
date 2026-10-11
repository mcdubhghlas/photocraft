/* Rust's float text formatting where it differs from C's printf and from serde's (ryu) text.
 * Pure functions; thread-safe (uses only snprintf/strtod in the C locale). */
#ifndef PC_BASE_RFMT_H
#define PC_BASE_RFMT_H

#include <stddef.h>

/* Large enough for any f64 in plain decimal: 5e-324 is "0." + 323 zeros + "5", f64::MAX has 309
 * integer digits, with room for a sign and a few more characters. */
enum { PC_RUST_F64_TEXT_MAX = 400 };

/* Rust's `{}` (Display) for an f64: the shortest digits that parse back to v, never in exponent
 * form ("0.0000001", "1000000000000000000000"), "NaN", "inf", "-inf"; no ".0" on integers.
 *
 * Rust's flt2dec (Grisu with a Dragon4 fallback) picks the shortest length k, then of the two
 * k-digit neighbours of v the one inside the rounding interval, the closer when both are, and
 * on an exact tie the one ABOVE (ryu, and so serde's text, rounds such ties to even: Rust prints
 * 3578819939694.65625 as ...6563 where ryu gives ...6562). This reproduces that from glibc's
 * exact printf digits and its correctly rounded strtod. */
size_t pc_rust_display_f64(double v, char out[PC_RUST_F64_TEXT_MAX]);

#endif
