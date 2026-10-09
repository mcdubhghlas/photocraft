/* Colour conversions between document colour models (port of crates/color/src/convert.rs).
 *
 * CMYK <-> RGB goes through cms with the built-in coated CMYK profile and sRGB (relative
 * colorimetric + black point compensation, Photoshop's default). A document's embedded CMYK
 * profile replaces the built-in one while its pc_cmyk_space is active on the calling thread
 * (pc_cmyk_space_enter/leave: the compositor, GPU upload and composite exports enter it).
 * Lab uses the exact D50 formulas; gray is sGray (gray v = sRGB (v, v, v)).
 *
 * Thread safety: everything is thread-safe. The active space is per thread; parallel workers
 * must enter the scope themselves. */
#ifndef PC_COLOR_CONVERT_H
#define PC_COLOR_CONVERT_H

#include <stddef.h>
#include <stdint.h>

/* A document's own CMYK profile linked to sRGB both ways. Reference counted. */
typedef struct pc_cmyk_space pc_cmyk_space;

/* The space for a document's embedded profile bytes, as a new reference (release it), or NULL
 * when there are none, they don't parse as a CMYK profile, they are the built-in coated CMYK
 * (the default path), or memory ran out. Cached by content, so calling this per render is
 * cheap and equal bytes give the same object. */
pc_cmyk_space *pc_cmyk_space_for_profile(const uint8_t *icc, size_t len);
pc_cmyk_space *pc_cmyk_space_retain(pc_cmyk_space *s); /* NULL-safe; returns s */
void pc_cmyk_space_release(pc_cmyk_space *s);         /* NULL-safe */
/* Content hash of the profile (cache keys of converted pixels). */
uint64_t pc_cmyk_space_id(const pc_cmyk_space *s);

/* Makes `space` (NULL: the built-in coated CMYK) this thread's CMYK space and returns the
 * previous one as a token that must be passed to pc_cmyk_space_leave, in LIFO order. The scope
 * holds its own reference to `space`. Typical use:
 *     pc_cmyk_space *prev = pc_cmyk_space_enter(space);
 *     ... convert ...
 *     pc_cmyk_space_leave(prev); */
pc_cmyk_space *pc_cmyk_space_enter(pc_cmyk_space *space);
void pc_cmyk_space_leave(pc_cmyk_space *prev);
/* The space active on this thread, borrowed (valid until the scope is left), or NULL. */
pc_cmyk_space *pc_cmyk_space_active(void);

/* Colour-managed CMYK -> sRGB and sRGB -> CMYK through the active space or the built-in coated
 * CMYK (relative colorimetric + BPC). rgb_to_cmyk clamps its input to [0, 1] first. */
void pc_cmyk_to_rgb(const float cmyk[4], float out[3]);
void pc_rgb_to_cmyk(const float rgb[3], float out[4]);

/* Naive formulas (no profile): (1 - c)(1 - k), and full-GCR RGB -> CMYK. */
void pc_cmyk_to_rgb_naive(const float cmyk[4], float out[3]);
void pc_rgb_to_cmyk_naive(const float rgb[3], float out[4]);

/* sRGB transfer function. */
float pc_srgb_to_linear(float v);
float pc_linear_to_srgb(float v);

/* D50 white (ICC PCS) and the Bradford-adapted linear sRGB <-> D50 XYZ matrices (row-major). */
extern const float PC_COLOR_D50[3];
extern const float PC_SRGB_TO_XYZ_D50[3][3];
extern const float PC_XYZ_D50_TO_SRGB[3][3];
void pc_color_mat3_mul(const float m[3][3], const float v[3], float out[3]); /* out may alias v */

/* CIE L*a*b* (D50; L 0..100) <-> encoded sRGB. lab_to_srgb clamps linear values to [0, 1]. */
void pc_lab_to_srgb(const float lab[3], float out[3]);
void pc_srgb_to_lab(const float rgb[3], float out[3]);

/* Rec.601 luma (Photoshop's Grayscale conversion default). */
float pc_rgb_to_gray(const float rgb[3]);

#endif
