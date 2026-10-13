/* Camera raw decoding and development (port of crates/raw: clean-room, standalone).
 *
 * pc_raw_identify / pc_raw_is_raw recognise raw files from their bytes. pc_raw_decode reads the
 * undeveloped sensor data and metadata (pc_raw_sensor). pc_raw_develop_sensor turns it into
 * 16-bit RGB in ProPhoto RGB (ROMM primaries, D50 white, gamma 1.8): linearization, black and
 * white levels, white balance, highlight clipping, demosaicing, camera -> XYZ -> ProPhoto,
 * exposure, the camera profile's tone curve and orientation. pc_raw_embedded_preview finds the
 * camera's full-size JPEG preview (pc_raw_embedded_previews lists every one). pc_raw_fit_look fits a
 * per-file colour matrix and tone curve to the camera's own (decoded) JPEG, for files without
 * colour calibration; pass it in pc_raw_develop_options.camera_look. pc_raw_develop_options.tone
 * applies Camera Raw's Shadows / Highlights / Clarity / Texture / Dehaze in linear light.
 *
 * Decoded: DNG (uncompressed, deflate, lossless JPEG, lossy JPEG and JPEG XL, strips and tiles,
 * CFA and LinearRaw incl. monochrome), CR2, uncompressed or lossless-JPEG TIFF/EP raws (NEF, ARW,
 * PEF...), Nikon compressed NEF, Sony compressed ARW (cRAW), Panasonic RW2 (RawFormat 5),
 * uncompressed Olympus ORF and uncompressed Fujifilm RAF (Bayer and X-Trans). JPEG XL needs
 * libjxl (WITH_JXL); without it those files are PC_ERR_UNSUPPORTED. Everything else is
 * PC_ERR_UNSUPPORTED.
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
    size_t samples; /* 1 for CFA data, 3 for LinearRaw (1 for monochrome LinearRaw) */
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
    /* Camera Raw's adaptive black point for profiles that copy its default rendering: this factor
     * times the 0.1th percentile of the image's linear luminance is subtracted before the tone
     * curve. 0: none. */
    float black_point;
    pc_raw_strings warnings;
} pc_raw_sensor;

void pc_raw_sensor_init(pc_raw_sensor *s);
void pc_raw_sensor_fini(pc_raw_sensor *s);
/* Monochrome data (one-sample LinearRaw: monochrome cameras, scanner DNGs): develops to neutral
 * grey with no white balance or colour calibration. */
bool pc_raw_sensor_is_monochrome(const pc_raw_sensor *s);

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

/* Camera Raw's Basic-panel sliders that act on raw data before the tone curve, -100..100. */
typedef struct pc_raw_tone {
    float shadows;
    float highlights;
    float clarity;
    float texture;
    float dehaze;
} pc_raw_tone;

/* Every slider at 0. */
bool pc_raw_tone_is_neutral(const pc_raw_tone *t);

/* A monotone tone curve on linear values 0..1, piecewise linear between knots in the square-root
 * domain; always through (0, 0) and (1, 1). */
#define PC_RAW_TONE_MAX_KNOTS 256
typedef struct pc_raw_look_curve {
    size_t n;
    double knots[PC_RAW_TONE_MAX_KNOTS][2]; /* (sqrt x, sqrt y), x strictly and y weakly increasing */
} pc_raw_look_curve;

/* A curve through linear-light knots (x, y): 2..PC_RAW_TONE_MAX_KNOTS finite points in 0..1, x
 * strictly increasing, y not decreasing, from (0, 0) to (1, 1); false otherwise. */
bool pc_raw_look_curve_new(const double (*knots)[2], size_t n, pc_raw_look_curve *out);
/* f(x) for linear x (clamped to 0..1, NaN -> 0). */
double pc_raw_look_curve_eval(const pc_raw_look_curve *c, double x);
/* The knots in linear light (out has c->n entries). */
void pc_raw_look_curve_knots(const pc_raw_look_curve *c, double (*out)[2]);

/* A colour rendering fitted to the camera's JPEG (pc_raw_fit_look). */
typedef struct pc_raw_camera_look {
    pc_raw_mat3 to_xyz; /* white-balanced camera RGB -> XYZ (D50), white -> D50 with Y = 1 */
    bool has_tone;
    pc_raw_look_curve tone;  /* applied per channel to linear ProPhoto RGB after exposure */
    double delta_e;          /* mean CIE76 dE against the camera JPEG on the fit grid */
    double delta_e_fallback; /* the same with the neutral fallback */
} pc_raw_camera_look;

typedef struct pc_raw_develop_options {
    pc_raw_demosaic demosaic;           /* default AHD */
    pc_raw_white_balance white_balance; /* default as shot */
    double exposure;                    /* EV added to the baseline exposure */
    bool orient;                        /* apply the orientation (default true) */
    pc_raw_tone tone;                   /* default all 0 */
    pc_raw_limits limits;
    /* A look fitted to the camera's JPEG (borrowed, may be NULL): replaces the file's colour
     * calibration (or the neutral fallback) and adds its tone curve. */
    const pc_raw_camera_look *camera_look;
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
    /* CFA pattern at the crop origin: "RGGB" for Bayer; other patterns list their whole repeat
     * row by row, rows separated by '/' (X-Trans: "GGRGGB/GGBGGR/..."). */
    char cfa[PC_RAW_CFA_MAX * (PC_RAW_CFA_MAX + 1)];
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

/* Every embedded DCT-JPEG preview, each stream once, in file order: writes at most cap into out
 * and returns how many there are (at most PC_RAW_MAX_PREVIEWS). */
#define PC_RAW_MAX_PREVIEWS (3 * 64 + 1)
size_t pc_raw_embedded_previews(const uint8_t *b, size_t n, pc_raw_preview *out, size_t cap);

typedef enum pc_raw_preview_color_space {
    PC_RAW_PREVIEW_SRGB,
    PC_RAW_PREVIEW_ADOBE_RGB
} pc_raw_preview_color_space;

/* The colour space the raw's EXIF gives for the camera's JPEG rendering: ColorSpace 1 = sRGB, or
 * Uncalibrated with InteropIndex "R03" = Adobe RGB. false when the file doesn't say, and for a DNG
 * (its EXIF describes the capture, its previews are the converter's). */
bool pc_raw_embedded_preview_color_space(const uint8_t *b, size_t n, pc_raw_preview_color_space *out);

/* Long edge of the look-fit grid, in cells. */
#define PC_RAW_PROXY_EDGE 256

/* The camera's rendering of the picture: upright, interleaved 8-bit sRGB. */
typedef struct pc_raw_reference {
    uint32_t width;
    uint32_t height;
    const uint8_t *rgb; /* width * height * 3 */
    size_t len;
} pc_raw_reference;

/* Fits a camera look so that developing s with o (white balance, baseline exposure) matches the
 * camera's reference rendering. When no look fits, returns PC_ERR_INVALID with the reason
 * ("colour not fitted to the camera JPEG: ..."); PC_ERR_NOMEM on out of memory. */
pc_status pc_raw_fit_look(const pc_raw_sensor *s, const pc_raw_develop_options *o, const pc_raw_reference *r,
                          pc_raw_camera_look *out, pc_error *err);

/* The raw-stage sliders on an image pc_raw_develop already made (gamma-1.8 ProPhoto RGBA, 0..1, w*h
 * pixels) with the profile tone curve: the curve is undone, the change applied in linear light and
 * the curve redone. PC_ERR_NOMEM on out of memory (px unchanged). */
pc_status pc_raw_retone(float (*px)[4], size_t w, size_t h, const float (*curve)[2], size_t ncurve,
                        const pc_raw_tone *tone, pc_error *err);

/* Decodes a lossless JPEG stream (for fuzzing and tools): at most max_samples samples are
 * allocated; *samples is freed by the caller. */
pc_status pc_raw_decode_lossless_jpeg(const uint8_t *b, size_t n, size_t max_samples, size_t *width,
                                      size_t *height, size_t *components, uint16_t **samples, size_t *nsamples,
                                      pc_error *err);

/* A listing of every IFD and entry (debugging aid); caller frees. NULL only on out of memory. */
char *pc_raw_dump_structure(const uint8_t *b, size_t n);

#endif
