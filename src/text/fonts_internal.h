/* Private structures shared by fonts.c and fontmatch.c. */
#ifndef PC_TEXT_FONTS_INTERNAL_H
#define PC_TEXT_FONTS_INTERNAL_H

#include "text/fonts.h"

/* Where a face's bytes come from. Path sources are read when a face is first loaded. */
typedef struct pc_fontdb_source {
    char *path;      /* NULL for registered data */
    pc_blob *bytes;  /* NULL until loaded (path sources) */
    bool failed;     /* reading the file failed: its faces are unloadable */
    pc_font **fonts; /* per face index, opened on demand */
    uint32_t nfaces;
} pc_fontdb_source;

enum { PC_AXIS_WGHT = 1, PC_AXIS_WDTH = 2, PC_AXIS_SLNT = 4, PC_AXIS_ITAL = 8, PC_AXIS_OPSZ = 16 };

typedef struct pc_fontdb_face {
    size_t src;
    uint32_t index;
    pc_font_attrs attrs;
    pc_font_axis *axes;
    size_t naxes;
    unsigned axis_bits;
} pc_fontdb_face;

typedef struct pc_fontdb_family {
    char *name; /* first registered spelling */
    size_t len;
    char *key; /* fontique NameKey: per-char Unicode lowercase */
    size_t klen;
    pc_fontdb_face *faces;
    size_t nfaces, cap;
} pc_fontdb_family;

/* Lookup by NameKey (NULL when unknown). */
pc_fontdb_family *pc_fontdb_find_family(const pc_fontdb *db, const char *name, size_t len);

/* fontique's match_font over a family's faces: index into faces, or -1 when empty. */
long pc_fontdb_match_index(const pc_fontdb_family *fam, const pc_font_query *q, bool synthesize_style);

/* FontInfo::synthesis of face f for query q: fills m's embolden, skew_deg and vars only. */
void pc_fontdb_face_synthesis(const pc_fontdb_face *f, const pc_font_query *q, pc_font_match *m);

/* fontique's NameKey of a name (appended to out). */
pc_status pc_fontdb_name_key(const char *s, size_t len, pc_buf *out, pc_error *err);

#endif
