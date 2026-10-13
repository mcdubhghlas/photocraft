/* OpenEXR headers, read the way the reference's exr crate reads them unvalidated (enough for
 * the limits check, deep detection and the deep chunk walk), and deep-data decoding (port of
 * crates/codecs/src/codecs/deep_exr.rs; REWRITE_PLAN.md D27). Internal to the codecs module. */
#ifndef PC_CODECS_DEEP_EXR_H
#define PC_CODECS_DEEP_EXR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "codecs/codecs.h"

typedef struct exr_chan {
    char *name;
    int type; /* 0 u32, 1 half, 2 float */
    int32_t xs;
    int32_t ys;
} exr_chan;

/* A string attribute kept verbatim (Latin-1 bytes, as the reference's exr crate reads Text). */
typedef struct exr_sattr {
    char *name;
    size_t name_len;
    uint8_t *val;
    size_t len;
} exr_sattr;

typedef struct exr_hdr {
    exr_chan *ch; /* file order */
    size_t nch;
    uint8_t *name; /* the `name` attribute (NULL when absent) */
    size_t name_len;
    uint8_t *view; /* the `view` attribute (NULL when absent) */
    size_t view_len;
    exr_sattr *attrs; /* string attributes named cryptomatte/... (the last duplicate wins) */
    size_t nattrs;
    int compression; /* 0 none, 1 rle, 2 zips, 3 zip, 4 piz, ... */
    int32_t dw[4];   /* data window: xmin, ymin, xmax, ymax */
    uint64_t w;      /* data window size */
    uint64_t h;
    bool tiled;
    uint32_t tw;
    uint32_t th;
    int level_mode; /* 0 singular, 1 mipmap, 2 ripmap */
    bool deep;
    uint64_t chunk_count;
} exr_hdr;

typedef struct exr_meta {
    exr_hdr *parts;
    size_t n;
    bool multipart;
    bool has_deep;     /* the version's deep-data flag */
    size_t header_end; /* where the offset tables start */
} exr_meta;

pc_status cx_exr_meta_parse(const uint8_t *b, size_t n, exr_meta *out, pc_error *err);
/* cx_exr_meta_parse with every part's declared size checked against the limits (4 bytes per
 * channel), and all of them together against max_alloc (exr_parts.c). */
pc_status cx_exr_meta_checked(const uint8_t *b, size_t n, const pc_climits *l, exr_meta *out, pc_error *err);
/* pc_exr_part_color_rank of a parsed header. */
uint8_t cx_exr_hdr_rank(const exr_hdr *h);
/* A NUL-terminated copy of Latin-1 bytes as UTF-8 (how the reference's exr crate shows Text). */
char *cx_latin1_to_utf8(const uint8_t *s, size_t n);
/* Latin-1 text (NUL-terminated) equal to UTF-8 text (NUL-terminated) once converted. */
bool cx_latin1_eq_utf8(const char *latin1, const char *utf8);
void cx_exr_meta_fini(exr_meta *m);
/* The deep samples of the first deep part. */
pc_status cx_exr_decode_deep(const exr_meta *m, const uint8_t *b, size_t n, const pc_climits *l, pc_cdeep *out,
                             pc_error *err);
/* Composites deep samples into a flat image with a DeepFlattened warning. */
pc_status cx_exr_flatten(const pc_cdeep *d, pc_cimage *out, pc_error *err);

#endif
