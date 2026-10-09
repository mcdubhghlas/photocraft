/* Layer records, layer masks, blending ranges and per-layer channel data (port of
 * crates/psd/src/layer.rs). Channel image data is kept in its stored encoding and decoded on
 * demand. Every struct owns its arrays (*_fini frees, *_clone deep-copies). Thread-safe. */
#ifndef PC_PSD_LAYER_H
#define PC_PSD_LAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/tagged.h"
#include "psd/types.h"

/* Channel ids. */
#define PC_PSD_CH_TRANSPARENCY (-1)
#define PC_PSD_CH_USER_MASK (-2)
#define PC_PSD_CH_REAL_USER_MASK (-3)

/* Layer flag bits. */
enum {
    PC_PSD_LF_TRANSPARENCY_PROTECTED = 1 << 0,
    PC_PSD_LF_HIDDEN = 1 << 1,
    PC_PSD_LF_OBSOLETE = 1 << 2,
    PC_PSD_LF_BIT4_USEFUL = 1 << 3,
    PC_PSD_LF_PIXEL_DATA_IRRELEVANT = 1 << 4
};
/* Sets pixel-data-irrelevant (also sets bit 3), as LayerFlags::set_pixel_data_irrelevant. */
uint8_t pc_psd_flags_set_pixel_data_irrelevant(uint8_t flags, bool v);

/* Mask flag bits. */
enum {
    PC_PSD_MF_RELATIVE = 1 << 0,
    PC_PSD_MF_DISABLED = 1 << 1,
    PC_PSD_MF_INVERT = 1 << 2,
    PC_PSD_MF_PARAMETERS = 1 << 4
};

/* Optional mask parameters (present when mask flag bit 4 is set). */
typedef struct pc_psd_mask_params {
    uint8_t flags; /* bit 0 user density, 1 user feather, 2 vector density, 3 vector feather */
    bool has_user_density;
    uint8_t user_density;
    bool has_user_feather;
    double user_feather;
    bool has_vector_density;
    uint8_t vector_density;
    bool has_vector_feather;
    double vector_feather;
} pc_psd_mask_params;

typedef struct pc_psd_real_mask {
    uint8_t flags;
    uint8_t background; /* 0 or 255 */
    pc_psd_rect rect;
} pc_psd_real_mask;

typedef struct pc_psd_layer_mask {
    pc_psd_rect rect;
    uint8_t default_color; /* 0 or 255 */
    uint8_t flags;         /* PC_PSD_MF_* */
    bool has_params;
    pc_psd_mask_params params;
    bool has_real;
    pc_psd_real_mask real;
    pc_psd_bytes trailing; /* remaining bytes (the 2 pads of the 20-byte form, ...) */
    /* With both present, the real-mask fields come before the parameters (Photoshop's order;
     * the published spec lists the parameters first). */
    bool real_first;
} pc_psd_layer_mask;

/* The simple 20-byte form (flags without the parameters bit). */
pc_status pc_psd_layer_mask_make(pc_psd_rect rect, uint8_t default_color, uint8_t flags, pc_psd_layer_mask *out,
                                 pc_error *err);
pc_status pc_psd_layer_mask_parse(const uint8_t *d, size_t len, pc_psd_layer_mask *out, pc_error *err);
pc_status pc_psd_layer_mask_to_bytes(const pc_psd_layer_mask *m, pc_psd_bytes *out, pc_error *err);
void pc_psd_layer_mask_fini(pc_psd_layer_mask *m);
bool pc_psd_layer_mask_eq(const pc_psd_layer_mask *a, const pc_psd_layer_mask *b);
/* Byte length of the parameter values announced by the parameter flags byte. */
size_t pc_psd_mask_parameter_bytes(uint8_t pf);
/* Whether a whole mask record with parameters stores the real-mask fields first (see
 * LayerMask::real_before_parameters for the heuristic). */
bool pc_psd_mask_real_before_parameters(const uint8_t *d, size_t len);

typedef enum pc_psd_mask_kind { PC_PSD_MASK_NONE, PC_PSD_MASK_PARSED, PC_PSD_MASK_RAW } pc_psd_mask_kind;

typedef struct pc_psd_mask_data {
    pc_psd_mask_kind kind;
    pc_psd_layer_mask mask; /* PARSED */
    pc_psd_bytes raw;       /* RAW: unparseable mask data, preserved */
} pc_psd_mask_data;

/* Blending ranges ("Blend If") are kept raw: 8-byte entries, composite gray first. */
pc_status pc_psd_blending_ranges_full(size_t channels, pc_psd_bytes *out, pc_error *err);

/* Encoded image data for one layer channel. */
typedef struct pc_psd_channel {
    int16_t id;
    bool has_compression; /* false: the stored length was 0 (no compression field at all) */
    uint16_t compression;
    pc_psd_bytes data; /* bytes after the compression field, verbatim */
} pc_psd_channel;

pc_status pc_psd_channel_encode(int16_t id, uint16_t compression, const uint8_t *decoded, size_t len, size_t width,
                                size_t height, uint16_t depth, pc_psd_version v, pc_psd_channel *out, pc_error *err);
pc_status pc_psd_channel_decode(const pc_psd_channel *c, size_t width, size_t height, uint16_t depth, pc_psd_version v,
                                pc_psd_bytes *out, pc_error *err);

/* A layer record with its channel data. */
typedef struct pc_psd_layer {
    pc_psd_rect rect;
    pc_psd_channel *channels;
    size_t nchannels;
    uint8_t blend[4]; /* key, e.g. "norm" */
    uint8_t opacity;
    uint8_t clipping; /* 0 base, 1 clipped to the layer below */
    uint8_t flags;    /* PC_PSD_LF_* */
    uint8_t filler;
    pc_psd_mask_data mask;
    pc_psd_bytes blending_ranges;
    pc_psd_bytes name; /* legacy Pascal name bytes */
    pc_psd_block *blocks;
    size_t nblocks;
    pc_psd_bytes extra_trailing; /* bytes at the end of the extra data that form no block */
} pc_psd_layer;

/* Default record: normal, opacity 255, nothing else. Never fails. */
void pc_psd_layer_init(pc_psd_layer *l);
void pc_psd_layer_fini(pc_psd_layer *l);
pc_status pc_psd_layer_clone(const pc_psd_layer *l, pc_psd_layer *out, pc_error *err);
bool pc_psd_layer_eq(const pc_psd_layer *a, const pc_psd_layer *b);
/* Appends a copy of a block / takes ownership of a channel (zeroed), also on failure. */
pc_status pc_psd_layer_push_block(pc_psd_layer *l, pc_psd_block *b, pc_error *err);
pc_status pc_psd_layer_push_channel(pc_psd_layer *l, pc_psd_channel *c, pc_error *err);

/* Name, preferring the luni block over the legacy name (owned UTF-8). */
pc_status pc_psd_layer_name(const pc_psd_layer *l, char **out, pc_error *err);
const pc_psd_block *pc_psd_layer_block(const pc_psd_layer *l, const char key[4]);
pc_psd_block *pc_psd_layer_block_mut(pc_psd_layer *l, const char key[4]);
/* Section divider from lsct (falling back to lsdk) when present and valid. */
bool pc_psd_layer_section_divider(const pc_psd_layer *l, pc_psd_section_divider *out);
uint32_t pc_psd_layer_section_type(const pc_psd_layer *l); /* OTHER without a divider */
bool pc_psd_layer_is_visible(const pc_psd_layer *l);
bool pc_psd_layer_id(const pc_psd_layer *l, uint32_t *out);
uint8_t pc_psd_layer_fill_opacity(const pc_psd_layer *l); /* 255 when absent */
const pc_psd_layer_mask *pc_psd_layer_get_mask(const pc_psd_layer *l);
const pc_psd_channel *pc_psd_layer_channel(const pc_psd_layer *l, int16_t id);
/* The rect a channel's pixels cover: mask rect for -2, real mask rect for -3, else the layer's. */
pc_psd_rect pc_psd_layer_channel_rect(const pc_psd_layer *l, int16_t id);
pc_status pc_psd_layer_decode_channel(const pc_psd_layer *l, int16_t id, uint16_t depth, pc_psd_version v,
                                      pc_psd_bytes *out, pc_error *err);

/* Layer info: records plus their channel data. */
typedef struct pc_psd_layer_info {
    bool merged_alpha; /* the layer count was stored negative */
    pc_psd_layer *layers; /* bottom-most first (file order) */
    size_t n;
    pc_psd_opt_bytes padding; /* bytes after the channel data; none = zero-pad to even */
} pc_psd_layer_info;

void pc_psd_layer_info_fini(pc_psd_layer_info *li);
pc_status pc_psd_layer_info_clone(const pc_psd_layer_info *li, pc_psd_layer_info *out, pc_error *err);
bool pc_psd_layer_info_eq(const pc_psd_layer_info *a, const pc_psd_layer_info *b);
/* Takes ownership of *l (re-initialised), also on failure. */
pc_status pc_psd_layer_info_push(pc_psd_layer_info *li, pc_psd_layer *l, pc_error *err);
/* Parses a body (the bytes covered by its length field). */
pc_status pc_psd_layer_info_parse(const uint8_t *data, size_t len, pc_psd_version v, pc_psd_layer_info *out,
                                  pc_error *err);
/* Serializes the body (no length field). PC_ERR_LIMIT "more than 32767 layers". */
pc_status pc_psd_layer_info_to_bytes(const pc_psd_layer_info *li, pc_psd_version v, pc_psd_bytes *out, pc_error *err);
/* Body length without its trailing padding. */
pc_status pc_psd_layer_info_unpadded_len(const pc_psd_layer_info *li, pc_psd_version v, uint64_t *out, pc_error *err);
/* Pads the body to a multiple of `align` (Photoshop writes 4). */
pc_status pc_psd_layer_info_pad_to(pc_psd_layer_info *li, pc_psd_version v, uint64_t align, pc_error *err);

struct psd_w;
void psd_write_layer_info(struct psd_w *w, const pc_psd_layer_info *li, pc_psd_version v);

#endif
