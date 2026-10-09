/* Camera raw decoding and development (port of crates/raw: clean-room, standalone).
 *
 * pc_raw_identify / pc_raw_is_raw recognise raw files from their bytes. pc_raw_decode reads the
 * undeveloped sensor data and metadata (pc_raw_sensor). pc_raw_develop_sensor turns it into
 * 16-bit RGB in ProPhoto RGB (ROMM primaries, D50 white, gamma 1.8): linearization, black and
 * white levels, white balance, highlight clipping, demosaicing, camera -> XYZ -> ProPhoto,
 * exposure, the camera profile's tone curve and orientation. pc_raw_embedded_preview finds the
 * camera's full-size JPEG preview.
 *
 * Decoded: DNG (uncompressed and lossless JPEG, strips and tiles, CFA and LinearRaw), CR2,
 * uncompressed or lossless-JPEG TIFF/EP raws (NEF, ARW, PEF...), Nikon compressed NEF, Sony
 * compressed ARW (cRAW), Panasonic RW2 (RawFormat 5) and uncompressed Olympus ORF. Everything
 * else is PC_ERR_UNSUPPORTED.
 *
 * Errors (the reference's RawError, same Display text):
 *   PC_ERR_NOT_FOUND    "not a camera raw file"
 *   PC_ERR_UNSUPPORTED  "unsupported camera raw: ..."
 *   PC_ERR_FORMAT       "malformed camera raw: ..."
 *   PC_ERR_LIMIT        "camera raw exceeds limits: ..."
 *   PC_ERR_NOMEM        out of memory (the reference aborts)
 * Sizes are checked against pc_raw_limits before allocating. Nothing here does I/O.
 *
 * Thread safety: every function is reentrant; decoding and development use pc_pool_global(). */
#ifndef PC_RAW_RAW_H
#define PC_RAW_RAW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

/* Name of the output colour space of pc_raw_develop. */
#define PC_RAW_OUTPUT_SPACE "ProPhoto RGB (ROMM primaries, D50, gamma 1.8)"

typedef enum pc_raw_format {
    PC_RAW_DNG,
    PC_RAW_CR2,
    PC_RAW_CR3,
    PC_RAW_NEF,
    PC_RAW_ARW,
    PC_RAW_PEF,
    PC_RAW_ORF,
    PC_RAW_RW2,
    PC_RAW_RAF,
    PC_RAW_TIFF_EP
} pc_raw_format;

/* "DNG", "CR2", ..., "TIFF/EP raw". */
const char *pc_raw_format_name(pc_raw_format f);

/* File extensions of the recognised formats (lower case, without the dot). */
#define PC_RAW_NEXTENSIONS 12
extern const char *const PC_RAW_EXTENSIONS[PC_RAW_NEXTENSIONS];

/* Decompression-bomb guards (same shape and defaults as the codecs' limits). */
typedef struct pc_raw_limits {
    uint32_t max_width;
    uint32_t max_height;
    uint64_t max_pixels; /* width * height */
    uint64_t max_alloc;  /* bytes of one buffer */
} pc_raw_limits;

pc_raw_limits pc_raw_limits_default(void);

/* Checks a width x height buffer of bytes_per_pixel bytes: zero sizes are PC_ERR_FORMAT, the rest
 * PC_ERR_LIMIT. */
pc_status pc_raw_limits_check(const pc_raw_limits *l, uint64_t width, uint64_t height, uint64_t bytes_per_pixel,
                              pc_error *err);

/* A rectangle in sensor-data pixels. */
typedef struct pc_raw_rect {
    size_t x;
    size_t y;
    size_t width;
    size_t height;
} pc_raw_rect;

pc_raw_rect pc_raw_rect_make(size_t x, size_t y, size_t width, size_t height);
pc_raw_rect pc_raw_rect_intersect(pc_raw_rect a, pc_raw_rect b);
bool pc_raw_rect_is_empty(pc_raw_rect r);
bool pc_raw_rect_eq(pc_raw_rect a, pc_raw_rect b);

/* A colour filter array: colour of each cell (0 red, 1 green, 2 blue), row major, at most 16x16. */
#define PC_RAW_CFA_MAX 16
typedef struct pc_raw_cfa {
    size_t width;
    size_t height;
    uint8_t colors[PC_RAW_CFA_MAX * PC_RAW_CFA_MAX];
    /* Data coordinates of the pattern's top-left cell. */
    size_t origin_x;
    size_t origin_y;
} pc_raw_cfa;

/* A 2x2 pattern from a string such as "RGGB", anchored at (0, 0); false for anything else. */
bool pc_raw_cfa_bayer(const char *pattern, pc_raw_cfa *out);
/* Colour at data coordinates (1 for an empty pattern). */
uint8_t pc_raw_cfa_color(const pc_raw_cfa *c, size_t x, size_t y);
/* The 2x2 colours seen from data coordinates (x0, y0), row major. */
void pc_raw_cfa_phase(const pc_raw_cfa *c, size_t x0, size_t y0, uint8_t out[4]);
/* A 2x2 period holding one red, one blue and two diagonal greens. */
bool pc_raw_cfa_is_bayer(const pc_raw_cfa *c);

/* Black levels (DNG BlackLevel / BlackLevelRepeatDim / BlackLevelDeltaH/V). */
typedef struct pc_raw_black {
    size_t rows;
    size_t cols;
    float *values; /* rows * cols * samples values, sample fastest */
    size_t nvalues;
    float *delta_h; /* per column of the active area (may be NULL) */
    size_t ndelta_h;
    float *delta_v; /* per row of the active area (may be NULL) */
    size_t ndelta_v;
} pc_raw_black;

/* Black level at active-area coordinates for sample s of spp. */
float pc_raw_black_at(const pc_raw_black *b, size_t ax, size_t ay, size_t s, size_t spp);
bool pc_raw_black_is_uniform(const pc_raw_black *b);

/* A 3x3 matrix, row major. */
typedef struct pc_raw_mat3 {
    double m[3][3];
} pc_raw_mat3;

/* One DNG calibration (ColorMatrixN with its companions). */
typedef struct pc_raw_calibration {
    double temperature;           /* illuminant temperature in kelvin */
    pc_raw_mat3 color_matrix;     /* XYZ -> reference camera */
    bool has_forward_matrix;
    pc_raw_mat3 forward_matrix;   /* white-balanced camera -> XYZ D50 */
    pc_raw_mat3 camera_calibration; /* reference camera -> individual camera */
} pc_raw_calibration;

/* Colour description of a raw file. */
typedef struct pc_raw_color_info {
    pc_raw_calibration calibrations[2]; /* sorted by temperature */
    size_t ncalibrations;
    bool has_analog_balance;
    double analog_balance[3];
    bool has_as_shot_neutral;
    double as_shot_neutral[3]; /* camera coordinates of a neutral */
    bool has_as_shot_white_xy;
    double as_shot_white_xy[2];
} pc_raw_color_info;

/* The matrix from white-balanced camera values to XYZ D50 (balanced white -> Y = 1); false
 * without calibrations or for an unusable neutral. */
bool pc_raw_balanced_to_xyz_d50(const pc_raw_color_info *c, const double neutral[3], pc_raw_mat3 *out);

/* One DNG GainMap opcode (lens shading), in active-area coordinates. */
typedef struct pc_raw_gain_map {
    size_t top, left, bottom, right;
    size_t plane, planes;
    size_t row_pitch, col_pitch;
    size_t points_v, points_h;
    double spacing_v, spacing_h, origin_v, origin_h;
    size_t map_planes;
    float *gains; /* points_v * points_h * map_planes */
    size_t ngains;
} pc_raw_gain_map;

/* A list of owned UTF-8 strings (warnings). */
typedef struct pc_raw_strings {
    char **v;
    size_t n;
} pc_raw_strings;

void pc_raw_strings_fini(pc_raw_strings *s);
/* true when some string contains needle. */
bool pc_raw_strings_any(const pc_raw_strings *s, const char *needle);

/* Undeveloped sensor data with everything needed to develop it. Free with pc_raw_sensor_fini. */
typedef struct pc_raw_sensor {
    pc_raw_format format;
    char *make;  /* NULL when absent */
    char *model; /* NULL when absent */
    size_t width;
    size_t height;
    size_t samples; /* 1 for CFA data, 3 for LinearRaw */
    uint16_t *data; /* width * height * samples, row major, interleaved */
    size_t ndata;
    bool has_cfa;
    pc_raw_cfa cfa;
    uint16_t *linearization; /* NULL when absent */
    size_t nlinearization;
    pc_raw_black black;
    float white[3];
    pc_raw_rect active; /* image-data area; black deltas are indexed from it */
    pc_raw_rect crop;   /* the area to output */
    pc_raw_color_info color;
    bool has_camera_wb;
    double camera_wb[3]; /* white-balance multipliers R G B from vendor metadata */
    uint16_t orientation; /* TIFF orientation 1-8 */
    double baseline_exposure; /* EV */
    pc_raw_gain_map *gain_maps;
    size_t ngain_maps;
    float (*tone_curve)[2]; /* increasing [input, output] pairs in linear ProPhoto; NULL: none */
    size_t ntone_curve;
    pc_raw_strings warnings;
} pc_raw_sensor;

void pc_raw_sensor_init(pc_raw_sensor *s);
void pc_raw_sensor_fini(pc_raw_sensor *s);

/* Demosaicing algorithm. */
typedef enum pc_raw_demosaic { PC_RAW_BILINEAR, PC_RAW_MHC, PC_RAW_AHD } pc_raw_demosaic;

/* "bilinear", "mhc", "ahd". */
const char *pc_raw_demosaic_id(pc_raw_demosaic d);
/* Accepts the ids and the aliases fast/malvar/best, ASCII case-insensitively. */
bool pc_raw_demosaic_from_id(const char *s, pc_raw_demosaic *out);

typedef enum pc_raw_wb_kind {
    PC_RAW_WB_AS_SHOT,   /* the camera's as-shot balance when recorded, else automatic */
    PC_RAW_WB_AUTO,      /* grey-world estimate */
    PC_RAW_WB_MULTIPLIERS /* explicit R, G, B multipliers */
} pc_raw_wb_kind;

typedef struct pc_raw_white_balance {
    pc_raw_wb_kind kind;
    double multipliers[3];
} pc_raw_white_balance;

typedef struct pc_raw_develop_options {
    pc_raw_demosaic demosaic;           /* default AHD */
    pc_raw_white_balance white_balance; /* default as shot */
    double exposure;                    /* EV added to the baseline exposure */
    bool orient;                        /* apply the orientation (default true) */
    pc_raw_limits limits;
} pc_raw_develop_options;

pc_raw_develop_options pc_raw_develop_options_default(void);

/* What was developed and how. */
typedef struct pc_raw_info {
    pc_raw_format format;
    char *make;
    char *model;
    size_t sensor_width;
    size_t sensor_height;
    bool has_cfa;
    char cfa[5]; /* e.g. "RGGB" at the crop origin */
    double wb_multipliers[3]; /* normalised to a minimum of 1 */
    uint16_t orientation;
    double baseline_exposure;
} pc_raw_info;

/* A developed image: 16-bit RGB in ProPhoto RGB. Free with pc_raw_developed_fini. */
typedef struct pc_raw_developed {
    uint32_t width;
    uint32_t height;
    uint16_t *rgb; /* width * height * 3, interleaved */
    size_t nrgb;
    pc_raw_info info;
    pc_raw_strings warnings;
} pc_raw_developed;

void pc_raw_developed_fini(pc_raw_developed *d);

/* An embedded DCT JPEG preview; jpeg points into the input bytes. */
typedef struct pc_raw_preview {
    const uint8_t *jpeg;
    size_t len;
    uint32_t width;
    uint32_t height;
} pc_raw_preview;

/* Recognises a raw file from its bytes (header and IFDs only). */
bool pc_raw_identify(const uint8_t *b, size_t n, pc_raw_format *out);
bool pc_raw_is_raw(const uint8_t *b, size_t n);

/* Reads the undeveloped sensor data; *out is written only on success. */
pc_status pc_raw_decode(const uint8_t *b, size_t n, const pc_raw_limits *limits, pc_raw_sensor *out,
                        pc_error *err);

/* Develops sensor data. */
pc_status pc_raw_develop_sensor(const pc_raw_sensor *s, const pc_raw_develop_options *o, pc_raw_developed *out,
                                pc_error *err);

/* Decodes and develops. */
pc_status pc_raw_develop(const uint8_t *b, size_t n, const pc_raw_develop_options *o, pc_raw_developed *out,
                         pc_error *err);

/* The largest embedded JPEG preview (TIFF-based raws and RAF). */
bool pc_raw_embedded_preview(const uint8_t *b, size_t n, pc_raw_preview *out);

/* Decodes a lossless JPEG stream (for fuzzing and tools): at most max_samples samples are
 * allocated; *samples is freed by the caller. */
pc_status pc_raw_decode_lossless_jpeg(const uint8_t *b, size_t n, size_t max_samples, size_t *width,
                                      size_t *height, size_t *components, uint16_t **samples, size_t *nsamples,
                                      pc_error *err);

/* A listing of every IFD and entry (debugging aid); caller frees. NULL only on out of memory. */
char *pc_raw_dump_structure(const uint8_t *b, size_t n);

#endif
