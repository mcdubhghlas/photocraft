/* Internals of src/raw shared between its files. Not a public API. */
#ifndef PC_RAW_INTERNAL_H
#define PC_RAW_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "raw/raw.h"

/* ---- errors (RawError's Display text) ---- */

#define rx_notraw(err) pc_fail((err), PC_ERR_NOT_FOUND, "not a camera raw file")
#define rx_unsupported(err, ...) pc_fail((err), PC_ERR_UNSUPPORTED, "unsupported camera raw: " __VA_ARGS__)
#define rx_malformed(err, ...) pc_fail((err), PC_ERR_FORMAT, "malformed camera raw: " __VA_ARGS__)
#define rx_limit(err, ...) pc_fail((err), PC_ERR_LIMIT, "camera raw exceeds limits: " __VA_ARGS__)
#define rx_nomem(err) pc_fail((err), PC_ERR_NOMEM, "out of memory")

/* Appends a formatted warning. */
pc_status rx_warn(pc_raw_strings *w, pc_error *err, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
/* Removes every string containing needle. */
void rx_strings_remove_containing(pc_raw_strings *w, const char *needle);
pc_status rx_strings_clone(const pc_raw_strings *src, pc_raw_strings *dst, pc_error *err);

/* ---- TIFF structure reader (tiff.rs) ---- */

enum {
    RX_NEW_SUBFILE_TYPE = 254,
    RX_IMAGE_WIDTH = 256,
    RX_IMAGE_LENGTH = 257,
    RX_BITS_PER_SAMPLE = 258,
    RX_COMPRESSION = 259,
    RX_PHOTOMETRIC = 262,
    RX_MAKE = 271,
    RX_MODEL = 272,
    RX_STRIP_OFFSETS = 273,
    RX_ORIENTATION = 274,
    RX_SAMPLES_PER_PIXEL = 277,
    RX_ROWS_PER_STRIP = 278,
    RX_STRIP_BYTE_COUNTS = 279,
    RX_PLANAR_CONFIGURATION = 284,
    RX_TILE_WIDTH = 322,
    RX_TILE_LENGTH = 323,
    RX_TILE_OFFSETS = 324,
    RX_TILE_BYTE_COUNTS = 325,
    RX_SUB_IFDS = 330,
    RX_SAMPLE_FORMAT = 339,
    RX_JPEG_INTERCHANGE_FORMAT = 513,
    RX_JPEG_INTERCHANGE_FORMAT_LENGTH = 514,
    RX_CFA_REPEAT_PATTERN_DIM = 33421,
    RX_CFA_PATTERN = 33422,
    RX_EXIF_IFD = 34665,
    RX_EXIF_CFA_PATTERN = 41730,
    RX_MAKER_NOTE = 37500,
    RX_DNG_VERSION = 50706,
    RX_UNIQUE_CAMERA_MODEL = 50708,
    RX_CFA_PLANE_COLOR = 50710,
    RX_CFA_LAYOUT = 50711,
    RX_LINEARIZATION_TABLE = 50712,
    RX_BLACK_LEVEL_REPEAT_DIM = 50713,
    RX_BLACK_LEVEL = 50714,
    RX_BLACK_LEVEL_DELTA_H = 50715,
    RX_BLACK_LEVEL_DELTA_V = 50716,
    RX_WHITE_LEVEL = 50717,
    RX_DEFAULT_SCALE = 50718,
    RX_DEFAULT_CROP_ORIGIN = 50719,
    RX_DEFAULT_CROP_SIZE = 50720,
    RX_COLOR_MATRIX_1 = 50721,
    RX_COLOR_MATRIX_2 = 50722,
    RX_CAMERA_CALIBRATION_1 = 50723,
    RX_CAMERA_CALIBRATION_2 = 50724,
    RX_ANALOG_BALANCE = 50727,
    RX_AS_SHOT_NEUTRAL = 50728,
    RX_AS_SHOT_WHITE_XY = 50729,
    RX_BASELINE_EXPOSURE = 50730,
    RX_CALIBRATION_ILLUMINANT_1 = 50778,
    RX_CALIBRATION_ILLUMINANT_2 = 50779,
    RX_ACTIVE_AREA = 50829,
    RX_FORWARD_MATRIX_1 = 50964,
    RX_FORWARD_MATRIX_2 = 50965,
    RX_OPCODE_LIST_1 = 51008,
    RX_OPCODE_LIST_2 = 51009,
    RX_OPCODE_LIST_3 = 51022,
    RX_BASELINE_EXPOSURE_OFFSET = 51109,
    RX_CR2_SLICE = 50752
};

/* Most IFDs a walk visits; most entries read from one IFD; most values a list accessor reads. */
#define RX_MAX_IFDS 64
#define RX_MAX_ENTRIES 4096
#define RX_MAX_VALUES ((size_t)1 << 20)

typedef struct rx_entry {
    uint16_t tag;
    uint16_t typ;
    uint32_t count;
    size_t at; /* absolute offset of the value bytes (may be out of range) */
} rx_entry;

/* An IFD: its entries are decoded from the bytes on demand (n readable entries). */
typedef struct rx_ifd {
    size_t offset;
    size_t base;
    size_t n;
} rx_ifd;

typedef struct rx_tiff {
    const uint8_t *data;
    size_t len;
    bool le;
    size_t first_ifd;
} rx_tiff;

size_t rx_type_size(uint16_t typ);
bool rx_tiff_new(const uint8_t *data, size_t len, rx_tiff *out);
const uint8_t *rx_bytes(const rx_tiff *t, size_t at, size_t len);
bool rx_u16_at(const rx_tiff *t, size_t at, uint16_t *v);
bool rx_u32_at(const rx_tiff *t, size_t at, uint32_t *v);
bool rx_ifd_at(const rx_tiff *t, size_t offset, size_t base, rx_ifd *out);
/* Entry i (< ifd->n). */
rx_entry rx_ifd_entry(const rx_tiff *t, const rx_ifd *ifd, size_t i);
bool rx_ifd_get(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, rx_entry *out);
bool rx_ifd_has(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag);
size_t rx_next_ifd(const rx_tiff *t, const rx_ifd *ifd);
/* Every reachable IFD (chain, SubIFDs, EXIF), depth first, deduplicated; at most RX_MAX_IFDS. */
size_t rx_all_ifds(const rx_tiff *t, rx_ifd out[RX_MAX_IFDS]);

bool rx_uint(const rx_tiff *t, const rx_entry *e, size_t i, uint32_t *v);
bool rx_float(const rx_tiff *t, const rx_entry *e, size_t i, double *v);
/* Number of values the list accessors return: min(count, RX_MAX_VALUES), or 0 when any of them is
 * unreadable (the reference's "empty if any is unreadable"). */
size_t rx_uints_n(const rx_tiff *t, const rx_entry *e);
size_t rx_floats_n(const rx_tiff *t, const rx_entry *e);
/* Copies up to cap values; returns the list length (as above). */
size_t rx_uints(const rx_tiff *t, const rx_entry *e, uint32_t *out, size_t cap);
size_t rx_floats(const rx_tiff *t, const rx_entry *e, double *out, size_t cap);
/* The raw value bytes. */
const uint8_t *rx_raw(const rx_tiff *t, const rx_entry *e, size_t *len);
/* An ASCII value up to the first NUL, lossy UTF-8, trimmed; NULL when absent or empty. Sets
 * *nomem on allocation failure. */
char *rx_ascii(const rx_tiff *t, const rx_entry *e, bool *nomem);

bool rx_tag_uint(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, uint32_t *v);
/* List lengths of a tag (0 when absent); the entry is written when present. */
size_t rx_tag_uints(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, uint32_t *out, size_t cap);
size_t rx_tag_floats(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, double *out, size_t cap);
char *rx_tag_ascii(const rx_tiff *t, const rx_ifd *ifd, uint16_t tag, bool *nomem);
/* The first Make / Model among ifds. */
char *rx_find_ascii(const rx_tiff *t, const rx_ifd *ifds, size_t n, uint16_t tag, bool *nomem);
/* Orientation of an IFD: 1-8, else 1. */
uint16_t rx_orientation(const rx_tiff *t, const rx_ifd *ifd);

char *rx_dump(const uint8_t *data, size_t len);

/* ---- sensor planes (sensor.rs) ---- */

typedef struct rx_plane {
    size_t width;
    size_t height;
    size_t samples;
    uint32_t bits;
    uint16_t *data;
} rx_plane;

typedef struct rx_segment {
    size_t x, y, w, h, offset, len;
} rx_segment;

typedef enum rx_jpeg_layout { RX_FLAT, RX_QUADS } rx_jpeg_layout;

pc_status rx_read_plane(const rx_tiff *t, const rx_ifd *ifd, const pc_raw_limits *limits, rx_jpeg_layout layout,
                        rx_plane *out, pc_error *err);
/* *out is malloc'd. */
pc_status rx_segments(const rx_tiff *t, const rx_ifd *ifd, size_t width, size_t height, rx_segment **out,
                      size_t *n, pc_error *err);
pc_status rx_unpack(const uint8_t *src, size_t srclen, size_t w, size_t rows, size_t samples, uint32_t bits, bool le,
                    uint16_t *out, pc_error *err);

pc_status rx_black_uniform(pc_raw_black *b, float v, pc_error *err);
pc_status rx_black_2x2(pc_raw_black *b, const float v[4], pc_error *err);
void rx_black_fini(pc_raw_black *b);

/* ---- lossless JPEG (ljpeg.rs) ---- */

typedef struct rx_frame {
    uint8_t precision;
    size_t width;
    size_t height;
    size_t components;
} rx_frame;

pc_status rx_ljpeg_frame(const uint8_t *data, size_t len, rx_frame *out, pc_error *err);
/* *out is malloc'd (frame samples). */
pc_status rx_ljpeg_decode(const uint8_t *data, size_t len, size_t max_samples, rx_frame *frame, uint16_t **out,
                          size_t *nout, pc_error *err);

/* ---- colour (color.rs) ---- */

extern const pc_raw_mat3 RX_IDENTITY;
extern const double RX_D50[3];
extern const double RX_D50_XY[2];
extern const double RX_D65_XY[2];

pc_raw_mat3 rx_mul(const pc_raw_mat3 *a, const pc_raw_mat3 *b);
void rx_apply(const pc_raw_mat3 *m, const double v[3], double out[3]);
pc_raw_mat3 rx_diag(const double v[3]);
pc_raw_mat3 rx_scale(const pc_raw_mat3 *m, double k);
bool rx_invert(const pc_raw_mat3 *m, pc_raw_mat3 *out);
bool rx_from_slice(const double *v, size_t n, pc_raw_mat3 *out);
void rx_xy_to_xyz(const double xy[2], double out[3]);
double rx_cct(const double xy[2]);
bool rx_illuminant_temperature(uint32_t code, double *t);
pc_raw_mat3 rx_bradford(const double src[3], const double dst[3]);
pc_raw_mat3 rx_xyz_d50_to_prophoto(void);
pc_raw_mat3 rx_srgb_to_xyz_d50(void);
/* RGB primaries and white chromaticities -> RGB to XYZ (for unit tests). */
pc_raw_mat3 rx_rgb_to_xyz(const double prim[3][2], const double white[2]);
void rx_neutral_to_xy(const pc_raw_color_info *c, const double neutral[3], double out[2]);
bool rx_xy_to_neutral(const pc_raw_color_info *c, const double xy[2], double out[3]);

/* ---- per-format decoders ---- */

pc_status rx_dng_decode(const rx_tiff *t, const pc_raw_limits *l, pc_raw_sensor *out, pc_error *err);
pc_status rx_cr2_decode(const rx_tiff *t, const pc_raw_limits *l, pc_raw_sensor *out, pc_error *err);
pc_status rx_rw2_decode(const rx_tiff *t, const pc_raw_limits *l, pc_raw_sensor *out, pc_error *err);
pc_status rx_orf_decode(const rx_tiff *t, const pc_raw_limits *l, pc_raw_sensor *out, pc_error *err);
pc_status rx_tiffep_decode(const rx_tiff *t, pc_raw_format f, const pc_raw_limits *l, pc_raw_sensor *out,
                           pc_error *err);

bool rx_is_sony_non_cfa(const rx_tiff *t, const rx_ifd *ifds, size_t n);
bool rx_tiffep_cfa(const rx_tiff *t, const rx_ifd *raw, const rx_ifd *ifds, size_t n, pc_raw_cfa *out);
bool rx_rggb_by_position(const pc_raw_cfa *cfa, const double *v, size_t n, float out[4]);
size_t rx_padded_width(const uint16_t *data, size_t len, size_t w, size_t h, uint32_t bits);

float rx_clip_level(const uint16_t *data, size_t n, uint32_t precision);
bool rx_measured_red_row(const uint16_t *data, size_t n, size_t width, pc_raw_rect area, float black, float white,
                         size_t *out);
bool rx_masked_black(const uint16_t *data, size_t n, size_t width, pc_raw_rect area, float out[4]);

bool rx_orf_preview_range(const rx_tiff *t, size_t *off, size_t *len);

/* Sony cRAW (sony.rs). */
#define RX_SONY_RAW_FILE_TYPE 0x7000
bool rx_sony_tone_lut(const uint32_t *knots, size_t n, uint16_t lut[2048]);
void rx_sony_decode_block(const uint8_t b[16], uint16_t out[16]);
bool rx_sony_is_craw(const rx_tiff *t, const rx_ifd *ifd);
pc_status rx_sony_read_craw(const rx_tiff *t, const rx_ifd *ifd, const pc_raw_limits *l, rx_plane *out,
                            pc_error *err);

/* Nikon compressed NEF (nefc.rs). */
#define RX_NIKON_COMPRESSION 34713u
#define RX_NIKON_LINEARIZATION_TABLE 0x0096
typedef struct rx_nef_table {
    uint16_t *curve; /* NULL: lossless */
    size_t ncurve;
    int32_t seeds[2][2];
} rx_nef_table;
pc_status rx_nef_parse_table(const uint8_t *t, size_t n, bool le, uint32_t bits, rx_nef_table *out, pc_error *err);
void rx_nef_table_fini(rx_nef_table *t);
pc_status rx_nef_decode(const uint8_t *src, size_t n, size_t w, size_t h, uint32_t bits, const rx_nef_table *t,
                        uint16_t **out, pc_error *err);
/* note: the maker note's TIFF view and IFD, or NULL. */
pc_status rx_nef_read_compressed(const rx_tiff *t, const rx_ifd *raw, const rx_tiff *note_t, const rx_ifd *note,
                                 const pc_raw_limits *l, rx_plane *out, pc_error *err);

/* Panasonic RW2 helpers (rw2.rs). */
size_t rx_rw2_physical(size_t i);
void rx_rw2_unpack_block(const uint8_t b[16], uint32_t bits, uint16_t *out, size_t n);

/* DNG opcodes (opcodes.rs). */
void rx_gain_map_fini(pc_raw_gain_map *m);
/* Parses an opcode list into gain maps and the names of the skipped opcodes. */
pc_status rx_opcodes_parse(const uint8_t *list, size_t n, pc_raw_gain_map **maps, size_t *nmaps,
                           pc_raw_strings *skipped, pc_error *err);
/* Gains along the map's horizontal points for image-relative row rv and map plane mp (out has
 * points_h entries). */
void rx_gain_map_row(const pc_raw_gain_map *m, double rv, size_t mp, float *out);
float rx_gain_map_at(const pc_raw_gain_map *m, const float *row, double rh);
bool rx_gain_map_covers(const pc_raw_gain_map *m, size_t ax, size_t ay);

/* Measured camera profiles (cameras.rs). */
pc_status rx_cameras_apply(pc_raw_sensor *s, pc_error *err);
/* The Nikon D4 profile's ForwardMatrix and tone curve (for unit tests). */
const pc_raw_mat3 *rx_d4_forward(void);
const float (*rx_d4_tone(size_t *n))[2];

/* ---- demosaic (demosaic.rs) ---- */

#define RX_PAD 6
typedef struct rx_padded {
    float *data;
    size_t w;
    size_t h;
    size_t stride;
} rx_padded;

pc_status rx_padded_new(size_t w, size_t h, rx_padded *out, pc_error *err);
void rx_padded_fini(rx_padded *p);
void rx_padded_fill_borders(rx_padded *p);
/* Interleaved RGB (w * h * 3), malloc'd. */
pc_status rx_demosaic(const rx_padded *p, const uint8_t phase[4], pc_raw_demosaic method, const pc_raw_mat3 *to_xyz,
                      float **out, pc_error *err);
/* Kernel taps of the generic path (exposed for the bilinear-vs-kernels test): taps for 2x2
 * position pos and colour c; returns the count (at most 11). */
size_t rx_demosaic_kernel(const uint8_t phase[4], pc_raw_demosaic method, size_t stride, size_t pos, size_t c,
                          ptrdiff_t offs[11], float weights[11]);
float rx_labf_exact(float t);
/* The AHD path's tabulated CIE f(t) (for unit tests): fills RX_LABF_ENTRIES values, and evaluates
 * the table at t. */
#define RX_LABF_ENTRIES ((1u << 14) + 2)
void rx_labf_table(float table[RX_LABF_ENTRIES]);
float rx_labf(const float *table, float t);

/* ---- develop (develop.rs) ---- */

/* The gamma 1.8 table after a tone curve (GAMMA_STEPS + 2 entries). */
#define RX_GAMMA_STEPS 4096
void rx_gamma_table(const float (*curve)[2], size_t n, float table[RX_GAMMA_STEPS + 2]);
uint16_t rx_gamma_encode(const float table[RX_GAMMA_STEPS + 2], float v);
/* Orients interleaved RGB; *out malloc'd (or src returned via *out = NULL for orientation 1). */
pc_status rx_orient(const uint16_t *src, size_t w, size_t h, uint16_t o, uint16_t **out, size_t *ow, size_t *oh,
                    pc_error *err);

/* ---- parallel helpers (par.rs) ---- */

/* Rows per parallel band for an image width samples wide (about 64 K samples a band). */
size_t rx_band_rows(size_t width);

#endif
