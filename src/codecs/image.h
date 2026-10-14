/* The codecs' own flat raster image (port of crates/codecs/src/image.rs).
 *
 * Samples are interleaved (RGBARGBA...), row-major, top to bottom, in native byte order.
 * Integer samples are unsigned and normalised to [0, max]; float samples are nominally [0, 1]
 * but may exceed it (HDR). Alpha, when present, is the last channel and straight (not
 * premultiplied). CMYK holds ink amounts (0 = no ink, not Adobe-inverted).
 *
 * Ownership: every struct owns its arrays; *_fini frees them (NULL-safe) and leaves an empty
 * value, *_clone deep-copies. Thread-safe (no shared state). */
#ifndef PC_CODECS_IMAGE_H
#define PC_CODECS_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

typedef enum pc_clayout { PC_CL_GRAY, PC_CL_GRAYA, PC_CL_RGB, PC_CL_RGBA, PC_CL_CMYK, PC_CL_CMYKA } pc_clayout;
#define PC_CL_COUNT 6

size_t pc_clayout_channels(pc_clayout l);       /* including alpha */
size_t pc_clayout_color_channels(pc_clayout l); /* without alpha */
bool pc_clayout_has_alpha(pc_clayout l);
bool pc_clayout_is_gray(pc_clayout l);
bool pc_clayout_is_rgb(pc_clayout l);
bool pc_clayout_is_cmyk(pc_clayout l);
pc_clayout pc_clayout_with_alpha(pc_clayout l);
pc_clayout pc_clayout_without_alpha(pc_clayout l);
bool pc_clayout_same_model(pc_clayout a, pc_clayout b); /* ignoring alpha */
const char *pc_clayout_name(pc_clayout l);               /* "Gray", "GrayA", "Rgb", ... */

/* F16 holds IEEE half floats as their 16-bit patterns. Ordered as the reference's SampleType. */
typedef enum pc_csample { PC_CS_U8, PC_CS_U16, PC_CS_F16, PC_CS_F32 } pc_csample;
#define PC_CS_COUNT 4

size_t pc_csample_bytes(pc_csample s);
bool pc_csample_is_float(pc_csample s);
const char *pc_csample_name(pc_csample s); /* "U8", "U16", "F16", "F32" */

/* Optional owned bytes (Option<Vec<u8>> / Option<String>; text is UTF-8, may hold NUL). */
typedef struct pc_meta_blob {
    bool has;
    uint8_t *data;
    size_t len;
} pc_meta_blob;

void pc_meta_blob_fini(pc_meta_blob *b);
pc_status pc_meta_blob_set(pc_meta_blob *b, const void *data, size_t len, pc_error *err); /* has = true; replaces */
pc_status pc_meta_blob_clone(const pc_meta_blob *b, pc_meta_blob *out, pc_error *err);
bool pc_meta_blob_eq(const pc_meta_blob *a, const pc_meta_blob *b);

/* One key/value text entry (UTF-8, each NUL-terminated after its length). */
typedef struct pc_ctext {
    char *key;
    size_t key_len;
    char *value;
    size_t value_len;
} pc_ctext;

/* Basic, format-independent metadata. */
typedef struct pc_cmeta {
    pc_meta_blob exif;  /* TIFF-structured EXIF (II*\0 / MM\0*), without JPEG's Exif\0\0 prefix */
    pc_meta_blob xmp;   /* XMP packet, UTF-8 */
    bool has_dpi;
    float dpi_x;
    float dpi_y;
    pc_ctext *text; /* free-form key/value text (PNG tEXt/iTXt, TIFF ASCII tags) */
    size_t ntext;
    pc_meta_blob photoshop_resources; /* TIFF tag 34377, opaque */
    pc_meta_blob photoshop_layers;    /* TIFF tag 37724 (ImageSourceData), opaque */
} pc_cmeta;

void pc_cmeta_fini(pc_cmeta *m);
pc_status pc_cmeta_clone(const pc_cmeta *m, pc_cmeta *out, pc_error *err);
bool pc_cmeta_eq(const pc_cmeta *a, const pc_cmeta *b);
bool pc_cmeta_is_empty(const pc_cmeta *m);
/* Appends a copy of key/value. */
pc_status pc_cmeta_add_text(pc_cmeta *m, const char *key, size_t key_len, const char *value, size_t value_len,
                            pc_error *err);

/* What a decoder noticed that the pixels don't show. */
typedef enum pc_cwarning_kind {
    PC_CW_MORE_FRAMES,    /* only the first frame of an animation was decoded */
    PC_CW_MORE_PAGES,     /* only the first page of a multi-page file was decoded */
    PC_CW_MORE_PARTS,     /* only one part of a multi-part (EXR) file was decoded */
    PC_CW_TRUNCATED,      /* the image data ends early; the decoder filled in the rest */
    PC_CW_DEEP_FLATTENED, /* deep data was composited into the flat image */
    PC_CW_MORE_ELEMENTS,  /* only the first image element of a DPX file was decoded */
    PC_CW_DPX_ENCODING    /* a DPX element's transfer / colorimetric codes (imported as stored) */
} pc_cwarning_kind;

typedef struct pc_cwarning {
    pc_cwarning_kind kind;
    bool has_total;              /* MORE_FRAMES / MORE_PAGES / MORE_PARTS: total is known */
    uint32_t total;              /* frames, pages, parts, (MORE_ELEMENTS) image elements, or
                                  * (DPX_ENCODING) transfer | colorimetric << 8 (see pc_cwarning_dpx) */
    int format;                  /* TRUNCATED / DEEP_FLATTENED: a pc_cformat */
    uint32_t max_samples;        /* DEEP_FLATTENED */
} pc_cwarning;

/* A DPX_ENCODING warning from its transfer / colorimetric codes (255 = undefined). */
static inline pc_cwarning pc_cwarning_dpx(uint8_t transfer, uint8_t colorimetric)
{
    pc_cwarning w;
    w.kind = PC_CW_DPX_ENCODING;
    w.has_total = false;
    w.total = (uint32_t)transfer | ((uint32_t)colorimetric << 8);
    w.format = 0;
    w.max_samples = 0;
    return w;
}

bool pc_cwarning_eq(const pc_cwarning *a, const pc_cwarning *b);
/* The reference's Display text, e.g. "only the first of 3 pages was imported". */
void pc_cwarning_text(const pc_cwarning *w, char *out, size_t cap);

/* A single flat raster image. */
typedef struct pc_cimage {
    uint32_t width;
    uint32_t height;
    pc_clayout layout;
    pc_csample sample;
    uint8_t *data; /* interleaved native-endian bytes, len = w * h * channels * bytes */
    size_t len;
    pc_meta_blob icc; /* embedded ICC profile (describes the layout's colour model) */
    pc_cmeta meta;
    pc_cwarning *warnings; /* empty for complete single images and images built in memory */
    size_t nwarnings;
} pc_cimage;

void pc_cimage_init(pc_cimage *img); /* 0x0 Gray U8, empty */
void pc_cimage_fini(pc_cimage *img);
/* A zero-filled image; PC_ERR_INVALID "invalid image: image size overflows usize". */
pc_status pc_cimage_new(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, pc_cimage *out, pc_error *err);
/* Wraps raw bytes, taking ownership of *data (also on failure, when it is freed). The length
 * must match: "invalid image: buffer has N bytes, expected M for WxH Layout Sample". */
pc_status pc_cimage_from_raw(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, uint8_t *data, size_t len,
                             pc_cimage *out, pc_error *err);
/* Copies data. */
pc_status pc_cimage_from_copy(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, const void *data, size_t len,
                              pc_cimage *out, pc_error *err);
/* From normalised f32 samples ([0,1] for integer targets; clamped and rounded). */
pc_status pc_cimage_from_normalized(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, const float *values,
                                    size_t n, pc_cimage *out, pc_error *err);
pc_status pc_cimage_clone(const pc_cimage *img, pc_cimage *out, pc_error *err);
bool pc_cimage_eq(const pc_cimage *a, const pc_cimage *b);
pc_status pc_cimage_add_warning(pc_cimage *img, pc_cwarning w, pc_error *err);

size_t pc_cimage_pixel_count(const pc_cimage *img);
size_t pc_cimage_sample_count(const pc_cimage *img);
/* Sample `index` (in samples, not bytes) as a normalised f32 (no bounds check beyond len). */
float pc_cimage_sample_normalized(const pc_cimage *img, size_t index);
/* Normalised f32 of channel c at (x, y). */
float pc_cimage_get(const pc_cimage *img, uint32_t x, uint32_t y, size_t c);
/* All samples as normalised f32 (new array of sample_count floats; NULL with n 0 if empty). */
pc_status pc_cimage_to_normalized(const pc_cimage *img, float **out, size_t *n, pc_error *err);
/* Alpha present and some pixel not fully opaque. */
bool pc_cimage_has_translucency(const pc_cimage *img);
/* A float image with a sample outside [0, 1] (or NaN). */
bool pc_cimage_has_out_of_range(const pc_cimage *img);

/* Converts layout and/or sample type into a new image. The ICC profile is kept only when the
 * colour model is unchanged; metadata and warnings are kept. Naive (not colour-managed)
 * formulas: Rec. 709 luma for gray, CMY(K) complements for CMYK. */
pc_status pc_cimage_convert(const pc_cimage *img, pc_clayout l, pc_csample s, pc_cimage *out, pc_error *err);
/* pc_cimage_convert, but *out points at img itself when nothing changes, else at *tmp. *tmp is
 * always initialised, so the caller finishes it either way. */
pc_status pc_cimage_converted(const pc_cimage *img, pc_clayout l, pc_csample s, pc_cimage *tmp,
                              const pc_cimage **out, pc_error *err);
/* Interleaved RGBA, 8 or 16 bits per sample (new arrays). */
pc_status pc_cimage_to_rgba8(const pc_cimage *img, uint8_t **out, size_t *n, pc_error *err);
pc_status pc_cimage_to_rgba16(const pc_cimage *img, uint16_t **out, size_t *n, pc_error *err);
pc_status pc_cimage_to_rgba_f32(const pc_cimage *img, float **out, size_t *n, pc_error *err);

/* ---- one channel of deep OpenEXR data ---- */

typedef struct pc_cdeep_channel {
    char *name;         /* as in the file (R, G, B, A, Z, ...) */
    pc_csample sample;  /* as stored: F16, or F32 (also for 32-bit unsigned) */
    float *samples;     /* every sample of the channel in pixel order */
    size_t nsamples;
} pc_cdeep_channel;

/* Deep image data: a variable-length list of samples per pixel. Pixel i holds samples
 * counts[i] .. counts[i+1] of every channel; counts has width * height + 1 entries. */
typedef struct pc_cdeep {
    uint32_t width;
    uint32_t height;
    pc_cdeep_channel *channels; /* file order */
    size_t nchannels;
    uint64_t *counts;
    size_t ncounts;
} pc_cdeep;

void pc_cdeep_fini(pc_cdeep *d);
const pc_cdeep_channel *pc_cdeep_channel_named(const pc_cdeep *d, const char *name);
void pc_cdeep_sample_range(const pc_cdeep *d, size_t pixel, uint64_t *start, uint64_t *end);
uint64_t pc_cdeep_total_samples(const pc_cdeep *d);

/* ---- multi-part OpenEXR and Cryptomatte ---- */

/* One channel of an EXR part: the name as in the file (an AOV layer keeps its prefix, e.g.
 * "diffuse.R") and the sample type the part stores it with (32-bit unsigned is listed as F32). */
typedef struct pc_exr_channel_info {
    char *name;
    pc_csample sample;
} pc_exr_channel_info;

/* One part of a (multi-part) OpenEXR file: header facts only. */
typedef struct pc_exr_part_info {
    size_t index; /* in file order */
    char *name;   /* the part `name` attribute, NULL when absent */
    char *view;   /* the `view` attribute (stereo), NULL when absent */
    uint32_t width;
    uint32_t height;
    bool deep; /* deepscanline / deeptile */
    bool tiled;
    pc_exr_channel_info *channels; /* file order */
    size_t nchannels;
} pc_exr_part_info;

void pc_exr_part_info_fini(pc_exr_part_info *p);
void pc_exr_parts_free(pc_exr_part_info *parts, size_t n); /* fini each, then free the array */
/* How much the part looks like a colour image: 4 RGBA, 3 RGB, 2 YA, 1 any single channel, 0 else. */
uint8_t pc_exr_part_color_rank(const pc_exr_part_info *p);

/* (ID value, name) of a Cryptomatte manifest. */
typedef struct pc_crypto_entry {
    float id;
    char *name;
} pc_crypto_entry;

/* One Cryptomatte layer (specification 1.2) of an EXR file. */
typedef struct pc_crypto_layer {
    size_t part;               /* the part the channels and attributes live in */
    char *name;                /* the `cryptomatte/<key>/name` attribute, e.g. "CryptoAsset" */
    char *key;                 /* the 7-hex-digit metadata key */
    char *conversion;          /* NULL when not recorded */
    pc_crypto_entry *manifest; /* sorted by name (the embedded JSON manifest) */
    size_t nmanifest;
    char *sidecar_manifest; /* the `manif_file` attribute, NULL when absent */
    char **channels;        /* stream channel bases in file order, e.g. "crypto_asset00" */
    size_t nchannels;
} pc_crypto_layer;

void pc_crypto_layer_fini(pc_crypto_layer *l);
void pc_crypto_layers_free(pc_crypto_layer *layers, size_t n);
/* The manifest name of an ID (compared by value), or NULL. Borrowed. */
const char *pc_crypto_layer_name_of(const pc_crypto_layer *l, float id);

/* One (ID, coverage) pair. */
typedef struct pc_crypto_pair {
    float id;
    float coverage;
} pc_crypto_pair;

/* Decoded Cryptomatte samples: per pixel its pairs with coverage above zero, sorted by
 * descending coverage. Pixel i holds pairs[start[i] .. start[i + 1]]; start has w * h + 1 entries. */
typedef struct pc_crypto_buffer {
    uint32_t width;
    uint32_t height;
    pc_crypto_pair *pairs;
    size_t npairs;
    size_t *start;
} pc_crypto_buffer;

void pc_crypto_buffer_fini(pc_crypto_buffer *b);
/* The pairs of pixel (x, y) (borrowed; *n is 0 for an uncovered or out-of-range pixel). */
const pc_crypto_pair *pc_crypto_buffer_at(const pc_crypto_buffer *b, uint32_t x, uint32_t y, size_t *n);

#endif
