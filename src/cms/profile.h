/* ICC profiles: the parsed model, the v2/v4 reader, and a profile's connection to the PCS as
 * pipeline stages (port of crates/cms/src/profile.rs).
 *
 * A pc_profile owns everything it points to; release with pc_profile_fini. Parsed profiles keep
 * their original bytes for byte-exact embedding. Profiles are immutable once built, so a
 * const pc_profile may be shared between threads. */
#ifndef PC_CMS_PROFILE_H
#define PC_CMS_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "cms.h"
#include "curve.h"
#include "math.h"
#include "pipeline.h"

/* Encoding family of a LUT tag, which fixes how its PCS side is encoded. */
typedef enum pc_lut_kind {
    PC_LUT8,  /* mft1: v4-style Lab encoding */
    PC_LUT16, /* mft2: legacy v2 Lab encoding */
    PC_LUT_AB /* mAB / mBA (v4): v4 Lab encoding */
} pc_lut_kind;

/* A parsed AToB/BToA tag; device and PCS values are normalised to [0, 1]. */
typedef struct pc_lut {
    pc_lut_kind kind;
    size_t inputs;
    size_t outputs;
    pc_cms_stages stages;
} pc_lut;

/* Largest colour difference (dE*ab) for which two profiles count as the same colour space. */
#define PC_SAME_COLORS_MAX_DELTA_E 0.5

typedef struct pc_profile {
    uint8_t version_major; /* e.g. 4 */
    uint8_t version_minor; /* minor.bugfix byte, e.g. 0x30 for 4.3 */
    uint32_t class_sig;    /* PC_CLASS_* */
    uint32_t space;        /* PC_CS_* */
    pc_pcs pcs;
    pc_intent rendering_intent;
    char *description; /* owned UTF-8 (never NULL); may contain U+0000, hence the length */
    size_t description_len;
    char *copyright; /* owned UTF-8 (never NULL) */
    size_t copyright_len;
    double white_point[3]; /* wtpt as stored (D50 when absent) */
    bool has_black_point;
    double black_point[3];
    bool has_chad;
    pc_mat3 chad;
    bool has_matrix;
    pc_mat3 matrix; /* columns rXYZ, gXYZ, bXYZ (D50-adapted) */
    bool has_trc;
    pc_cms_curve trc[3];
    bool has_gray_trc;
    pc_cms_curve gray_trc;
    pc_lut *a2b[3]; /* owned or NULL: perceptual, colorimetric, saturation */
    pc_lut *b2a[3];
    uint8_t *bytes; /* owned ICC bytes this profile was parsed from or encoded to, or NULL */
    size_t nbytes;
    uint64_t hash; /* content hash of bytes (valid when bytes != NULL) */
} pc_profile;

/* An empty profile (every pointer NULL, strings ""), safe to fini. */
pc_status pc_profile_init_empty(pc_profile *p, pc_error *err);
void pc_profile_fini(pc_profile *p);
pc_status pc_profile_clone(const pc_profile *p, pc_profile *out, pc_error *err);

/* Parse ICC bytes (copied). */
pc_status pc_profile_parse(const uint8_t *data, size_t len, pc_profile *out, pc_error *err);

/* Replace the cached bytes with a fresh ICC v4.3 encoding of the model (built-in profiles). */
pc_status pc_profile_encode_bytes(pc_profile *p, pc_error *err);
/* Append the profile's ICC bytes to *out: the cached bytes, or a fresh v4.3 encoding. */
pc_status pc_profile_to_bytes(const pc_profile *p, pc_buf *out, pc_error *err);
/* 64-bit hash of the ICC bytes (cache keys; equal bytes give equal hashes). */
pc_status pc_profile_content_hash(const pc_profile *p, uint64_t *out, pc_error *err);

size_t pc_profile_channels(const pc_profile *p);
/* Media white for ICC-absolute colorimetry: wtpt, except v2 display profiles (D50). */
void pc_profile_media_white(const pc_profile *p, double out[3]);
/* An RGB view of a gray TRC profile (each channel carries the gray curve and a third of the
 * white). *ok is false (out untouched) when p has no gray curve or an Lab PCS. */
pc_status pc_profile_gray_as_rgb(const pc_profile *p, pc_profile *out, bool *ok, pc_error *err);
/* Matrix/TRC RGB or gray TRC without LUTs. */
bool pc_profile_is_matrix_shaper(const pc_profile *p);
/* Whether p has a dedicated table for intent (matrix profiles support every intent). */
bool pc_profile_supports_intent(const pc_profile *p, pc_intent intent, bool input);

/* Stages mapping normalised device values to the float PCS (*pcs says which). *out must be
 * empty; it is filled only on success. */
pc_status pc_profile_device_to_pcs(const pc_profile *p, pc_intent intent, pc_cms_stages *out, pc_pcs *pcs,
                                   pc_error *err);
/* Stages mapping the float PCS to normalised device values. */
pc_status pc_profile_pcs_to_device(const pc_profile *p, pc_intent intent, pc_cms_stages *out, pc_pcs *pcs,
                                   pc_error *err);

/* Same colour space whatever the encoding: same channels and every sample of a device grid
 * within PC_SAME_COLORS_MAX_DELTA_E through both (relative colorimetric). Defined in
 * transform.c. False on any error. */
bool pc_profile_same_colors(const pc_profile *a, const pc_profile *b);

#endif
