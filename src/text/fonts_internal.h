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
    /* FaceInfo names, read on first use (fonts_faces.c): typographic or legacy subfamily (NULL
     * when the font names none) and PostScript name (NULL when none). */
    bool names_done;
    char *style;
    char *ps;
} pc_fontdb_face;

typedef struct pc_fontdb_family {
    uint64_t id; /* process-wide, like fontique's FamilyId (for face aliases) */
    char *name;  /* first registered spelling */
    size_t len;
    char *key; /* fontique NameKey: per-char Unicode lowercase */
    size_t klen;
    pc_fontdb_face *faces;
    size_t nfaces, cap;
} pc_fontdb_family;

typedef struct pc_fontdb_ps_entry {
    char *ps;
    size_t len;
    pc_resolved_font r;
} pc_fontdb_ps_entry;

/* A runtime-only family registered for one exact face (select_named_face): the style's family
 * spelling and face index -> the alias family. */
typedef struct pc_fontdb_alias {
    char *family;
    size_t index;
    char *alias;
} pc_fontdb_alias;

struct pc_fontdb {
    pc_fontdb_source *src;
    size_t nsrc, src_cap;
    pc_fontdb_family *fam;
    size_t nfam, fam_cap;
    bool system_loaded;
    pc_fontdb_ps_entry *ps;
    size_t nps, ps_cap;
    const char **fallbacks; /* borrowed candidate spellings (static, craft list or owned_fallbacks) */
    size_t nfallbacks;
    char **owned_fallbacks; /* served script fallbacks (copies) */
    size_t nowned_fallbacks;
    bool missing_glyph_protection;
    pc_fontdb_alias *aliases;
    size_t naliases, alias_cap;
};

/* Lookup by NameKey (NULL when unknown). */
pc_fontdb_family *pc_fontdb_find_family(const pc_fontdb *db, const char *name, size_t len);

/* fontique's match_font over a family's faces: index into faces, or -1 when empty. */
long pc_fontdb_match_index(const pc_fontdb_family *fam, const pc_font_query *q, bool synthesize_style);

/* FontInfo::synthesis of face f for query q: fills m's embolden, skew_deg and vars only. */
void pc_fontdb_face_synthesis(const pc_fontdb_face *f, const pc_font_query *q, pc_font_match *m);

/* A new source (path or shared bytes); *idx receives its index. */
pc_status pc_fontdb_add_source(pc_fontdb *db, const char *path, pc_blob *bytes, size_t *idx, pc_error *err);
/* fontique register_font_impl over one source: every face with a family name and a usable cmap
 * joins its family, or `family_override` (may be NULL). Collects the families touched in
 * `touched` (may be NULL). */
pc_status pc_fontdb_scan_source(pc_fontdb *db, size_t src, const uint8_t *data, size_t len,
                                const char *family_override, size_t **touched, size_t *ntouched,
                                pc_error *err);
/* The loaded font of a face (borrowed; NULL when its bytes can't be read or opened). */
pc_font *pc_fontdb_load_face(pc_fontdb *db, const pc_fontdb_face *face, pc_error *err);
/* Empties the PostScript-name cache. */
void pc_fontdb_clear_ps_cache(pc_fontdb *db);
/* Rebuilds the fallback list (after fonts were added). */
void pc_fontdb_refresh_generics(pc_fontdb *db);
/* Reads the FaceInfo names of a face once (no-op afterwards). Out of memory leaves them unread. */
void pc_fontdb_face_names(pc_fontdb *db, pc_fontdb_face *face);

/* fontique's NameKey of a name (appended to out). */
pc_status pc_fontdb_name_key(const char *s, size_t len, pc_buf *out, pc_error *err);

#endif
