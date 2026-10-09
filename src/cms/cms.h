/* PhotoCraft colour management: ICC v2/v4 profiles, transforms through the PCS, built-in
 * profiles (port of crates/cms). This header holds the shared vocabulary; see profile.h,
 * transform.h, builtin.h, gamut.h, lut3d.h, lutfile.h, synth.h.
 *
 * Implemented from the public ICC.1:2010 (v4.3) and ICC.1:2001-04 (v2) specifications, CIE 15
 * and Adobe's published black point compensation paper. Depends only on src/base.
 *
 * Errors follow the reference's CmsError: truncated or non-ICC data and invalid profiles are
 * PC_ERR_FORMAT ("ICC profile is truncated", "not an ICC profile (missing 'acsp' signature)",
 * "invalid ICC profile: ..."); unsupported features are PC_ERR_UNSUPPORTED ("unsupported ICC
 * feature: ..."). */
#ifndef PC_CMS_CMS_H
#define PC_CMS_CMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ICC rendering intent (values are the ICC codes). */
typedef enum pc_intent {
    PC_INTENT_PERCEPTUAL = 0,
    PC_INTENT_RELATIVE = 1, /* relative colorimetric (the default) */
    PC_INTENT_SATURATION = 2,
    PC_INTENT_ABSOLUTE = 3
} pc_intent;

bool pc_intent_from_u32(uint32_t v, pc_intent *out);
/* "perceptual", "relative"/"relativeColorimetric"/"colorimetric", "saturation",
 * "absolute"/"absoluteColorimetric" or "0".."3": ASCII case-insensitive, non-alphanumerics
 * ignored. */
bool pc_intent_parse(const char *s, size_t len, pc_intent *out);
const char *pc_intent_id(pc_intent i); /* "perceptual", "relative", "saturation", "absolute" */

/* A four-character ICC signature as a big-endian u32. */
#define PC_SIG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

/* Data colour spaces are stored as their header signature (lossless; `nCLR` for n colorants). */
#define PC_CS_XYZ PC_SIG('X', 'Y', 'Z', ' ')
#define PC_CS_LAB PC_SIG('L', 'a', 'b', ' ')
#define PC_CS_LUV PC_SIG('L', 'u', 'v', ' ')
#define PC_CS_YCBCR PC_SIG('Y', 'C', 'b', 'r')
#define PC_CS_YXY PC_SIG('Y', 'x', 'y', ' ')
#define PC_CS_RGB PC_SIG('R', 'G', 'B', ' ')
#define PC_CS_GRAY PC_SIG('G', 'R', 'A', 'Y')
#define PC_CS_HSV PC_SIG('H', 'S', 'V', ' ')
#define PC_CS_HLS PC_SIG('H', 'L', 'S', ' ')
#define PC_CS_CMYK PC_SIG('C', 'M', 'Y', 'K')
#define PC_CS_CMY PC_SIG('C', 'M', 'Y', ' ')

/* Channels of a colour space: Gray 1, CMYK 4, nCLR n (2..15), unknown 0, other known 3. */
size_t pc_cs_channels(uint32_t space);
/* The reference's Debug name ("Rgb", "Cmyk", "Color(5)", "Unknown(1234)") for messages. */
void pc_cs_debug_name(uint32_t space, char out[32]);

/* Profile/device classes, also stored as signatures. */
#define PC_CLASS_INPUT PC_SIG('s', 'c', 'n', 'r')
#define PC_CLASS_DISPLAY PC_SIG('m', 'n', 't', 'r')
#define PC_CLASS_OUTPUT PC_SIG('p', 'r', 't', 'r')
#define PC_CLASS_DEVICE_LINK PC_SIG('l', 'i', 'n', 'k')
#define PC_CLASS_COLOR_SPACE PC_SIG('s', 'p', 'a', 'c')
#define PC_CLASS_ABSTRACT PC_SIG('a', 'b', 's', 't')
#define PC_CLASS_NAMED_COLOR PC_SIG('n', 'm', 'c', 'l')
void pc_class_debug_name(uint32_t cls, char out[32]); /* "Display", "DeviceLink", "Unknown(n)" */

/* Which profile connection space values are in (floats: XYZ with Y = 1 for white, or Lab
 * with L 0..100). */
typedef enum pc_pcs { PC_PCS_XYZ, PC_PCS_LAB } pc_pcs;

#endif
