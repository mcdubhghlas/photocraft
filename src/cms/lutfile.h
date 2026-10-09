/* Creative 3D LUT files for Image > Adjustments > Color Lookup: .cube (Adobe/Resolve), .3dl
 * (Lustre/Flame), .look (SpeedGrade XML), and generated CC0 looks (port of
 * crates/cms/src/lutfile.rs).
 *
 * Every table is normalised to size^3 RGB triplets in 0..1, red varying fastest
 * (((b * size + g) * size + r) * 3). Errors are PC_ERR_FORMAT with the reference's messages.
 * A pc_lutfile owns its title and data (pc_lutfile_fini). */
#ifndef PC_CMS_LUTFILE_H
#define PC_CMS_LUTFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* Largest edge length accepted. */
#define PC_LUTFILE_MAX_SIZE 129

typedef struct pc_lutfile {
    char *title; /* owned UTF-8, never NULL */
    size_t size;
    float *data;  /* size^3 * 3 values, red fastest */
    size_t ndata; /* = size^3 * 3 */
    /* Input domain (.cube DOMAIN_MIN/MAX, LUT_3D_INPUT_RANGE); {0,0,0}..{1,1,1} by default. */
    float domain_min[3];
    float domain_max[3];
} pc_lutfile;

void pc_lutfile_fini(pc_lutfile *l);
bool pc_lutfile_domain_is_default(const pc_lutfile *l);

/* The identity table of edge size (2..129). */
pc_status pc_lutfile_identity(size_t size, pc_lutfile *out, pc_error *err);

/* Parse, picking the format from file_name's extension (falling back to sniffing). */
pc_status pc_lutfile_parse(const char *file_name, const uint8_t *bytes, size_t len, pc_lutfile *out, pc_error *err);
pc_status pc_lutfile_parse_cube(const char *text, size_t len, pc_lutfile *out, pc_error *err);
pc_status pc_lutfile_parse_3dl(const char *text, size_t len, pc_lutfile *out, pc_error *err);
pc_status pc_lutfile_parse_look(const char *text, size_t len, pc_lutfile *out, pc_error *err);

/* Append a .cube file (what Photoshop embeds in a Color Lookup layer). */
pc_status pc_lutfile_write_cube(const pc_lutfile *l, pc_buf *out, pc_error *err);

/* Generated looks shipped with PhotoCraft (procedural, CC0). */
#define PC_LUTFILE_BUILTIN_COUNT 8
const char *pc_lutfile_builtin_id(size_t i);    /* "warm", "cool", "tealOrange", ... */
const char *pc_lutfile_builtin_label(size_t i); /* "Warm Filter", ... */
/* A built-in look by id (33^3); PC_ERR_NOT_FOUND for unknown ids. */
pc_status pc_lutfile_builtin(const char *id, pc_lutfile *out, pc_error *err);

#endif
