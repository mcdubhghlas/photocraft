/* A bounds-checked reader for the TIFF directory structure: classic TIFF (42) and BigTIFF (43)
 * in either byte order (port of crates/codecs/src/codecs/tiff_ifd.rs). Every offset and count is
 * untrusted: reads are bounds-checked with checked arithmetic, directory tables must lie wholly
 * inside the file, the IFD chain and SubIFD trees are walked with a seen-set, a depth limit and a
 * cap on the number of directories. Internal to the codecs module. */
#ifndef PC_CODECS_TIFF_IFD_H
#define PC_CODECS_TIFF_IFD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codecs/codecs.h"

#define TIFF_MAX_IFDS 10000u

enum {
    TAG_NEW_SUBFILE_TYPE = 254,
    TAG_SUBFILE_TYPE = 255,
    TAG_WIDTH = 256,
    TAG_HEIGHT = 257,
    TAG_BITS = 258,
    TAG_COMPRESSION = 259,
    TAG_PHOTOMETRIC = 262,
    TAG_STRIP_OFFSETS = 273,
    TAG_ORIENTATION = 274,
    TAG_SAMPLES = 277,
    TAG_ROWS_PER_STRIP = 278,
    TAG_STRIP_COUNTS = 279,
    TAG_PLANAR = 284,
    TAG_PREDICTOR = 317,
    TAG_COLOR_MAP = 320,
    TAG_TILE_WIDTH = 322,
    TAG_TILE_LENGTH = 323,
    TAG_TILE_OFFSETS = 324,
    TAG_TILE_COUNTS = 325,
    TAG_SUB_IFDS = 330,
    TAG_EXTRA_SAMPLES = 338,
    TAG_SAMPLE_FORMAT = 339
};

typedef struct tiff_entry {
    uint16_t tag;
    uint16_t ty;
    uint64_t count;
    uint64_t value_at; /* inside the entry when it fits, else the stored offset */
} tiff_entry;

typedef struct tiff_dir {
    uint64_t at;
    tiff_entry *entries; /* file order; entries of unknown types are skipped */
    size_t n;
    uint64_t next;
    uint64_t next_at; /* position of the next-IFD pointer */
} tiff_dir;

typedef struct tiff_file {
    const uint8_t *b;
    size_t n;
    bool le;
    bool big;
    uint64_t first_ifd;
} tiff_file;

/* false when the bytes are not a (Big)TIFF header. */
bool tiff_file_parse(const uint8_t *b, size_t n, tiff_file *f);
uint64_t tiff_header_len(const tiff_file *f);
bool tiff_u16_at(const tiff_file *f, uint64_t at, uint16_t *v);
bool tiff_u32_at(const tiff_file *f, uint64_t at, uint32_t *v);
bool tiff_u64_at(const tiff_file *f, uint64_t at, uint64_t *v);
/* The directory at `at` (whole table and next pointer inside the file, after the header).
 * false when unreadable or out of memory. dir_fini frees. */
bool tiff_dir_read(const tiff_file *f, uint64_t at, tiff_dir *out);
void tiff_dir_fini(tiff_dir *d);
const tiff_entry *tiff_dir_get(const tiff_dir *d, uint16_t tag);
size_t tiff_type_size(uint16_t ty); /* 0 for unknown types */
/* The value bytes of an entry when they are all inside the file (NULL otherwise). */
const uint8_t *tiff_data(const tiff_file *f, const tiff_entry *e, size_t *len);
/* Value i of an unsigned integer entry (BYTE, SHORT, LONG, IFD, LONG8, IFD8). */
bool tiff_uint(const tiff_file *f, const tiff_entry *e, uint64_t i, uint64_t *v);
bool tiff_tag_uint(const tiff_file *f, const tiff_dir *d, uint16_t tag, uint64_t *v);
/* Up to max values (false if the tag is absent or any value unreadable). *out allocated. */
bool tiff_tag_uints(const tiff_file *f, const tiff_dir *d, uint16_t tag, uint64_t max, uint64_t **out, size_t *n);
bool tiff_tag_f64(const tiff_file *f, const tiff_dir *d, uint16_t tag, double *v);
/* Bytes of a one-byte-per-value tag (BYTE, ASCII, SBYTE, UNDEFINED). */
const uint8_t *tiff_tag_bytes(const tiff_file *f, const tiff_dir *d, uint16_t tag, size_t *len);
uint16_t tiff_orientation_of(const tiff_file *f, const tiff_dir *d);
/* Lists the directories (error only when there is no readable first directory). */
pc_status tiff_info_of(const tiff_file *f, pc_tiff_info *out, pc_error *err);

#endif
