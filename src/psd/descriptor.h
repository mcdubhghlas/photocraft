/* Photoshop ActionDescriptors: OSType-tagged key/value trees (port of
 * crates/psd/src/descriptor.rs). They appear inside many tagged blocks (lfx2, SoCo, GdFl, TySh,
 * PlLd, ...), usually wrapped in a versioned descriptor (version 16). Parsing and writing are
 * exact: write(parse(b)) == b for well-formed input. Nesting is limited to
 * PC_PSD_DESC_MAX_DEPTH (PC_ERR_LIMIT "descriptor nesting too deep").
 *
 * Ownership: every struct owns its arrays; *_fini frees them (NULL-safe), *_clone deep-copies.
 * Thread-safe (no shared state). */
#ifndef PC_PSD_DESCRIPTOR_H
#define PC_PSD_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/types.h"

#define PC_PSD_DESC_MAX_DEPTH 64

/* A Photoshop Unicode string: UTF-16 code units, kept exactly (trailing NUL, lone surrogates). */
typedef struct pc_psd_ustr {
    uint16_t *u;
    size_t n;
} pc_psd_ustr;

/* Class / key / type identifier: stored length 0 + 4-byte code, or explicit length + bytes. */
typedef struct pc_psd_id {
    bool is_code;
    uint8_t code[4]; /* when is_code */
    pc_psd_bytes str; /* otherwise */
} pc_psd_id;

typedef struct pc_psd_class {
    pc_psd_ustr name;
    pc_psd_id class_id;
} pc_psd_class;

typedef enum pc_psd_ref_kind {
    PC_PSD_REF_PROPERTY,   /* prop: class, key */
    PC_PSD_REF_CLASS,      /* Clss: class */
    PC_PSD_REF_ENUMERATED, /* Enmr: class, type_id, value */
    PC_PSD_REF_OFFSET,     /* rele: class, offset */
    PC_PSD_REF_IDENTIFIER, /* Idnt: number */
    PC_PSD_REF_INDEX,      /* indx: number */
    PC_PSD_REF_NAME        /* name: class, name */
} pc_psd_ref_kind;

typedef struct pc_psd_ref_item {
    pc_psd_ref_kind kind;
    pc_psd_class cls;
    pc_psd_id key;     /* PROPERTY key; ENUMERATED type */
    pc_psd_id value;   /* ENUMERATED value */
    int32_t offset;    /* OFFSET */
    uint32_t number;   /* IDENTIFIER, INDEX */
    pc_psd_ustr name;  /* NAME */
} pc_psd_ref_item;

typedef enum pc_psd_vkind {
    PC_PSD_V_REFERENCE,     /* obj  */
    PC_PSD_V_DESCRIPTOR,    /* Objc */
    PC_PSD_V_GLOBAL_OBJECT, /* GlbO */
    PC_PSD_V_LIST,          /* VlLs */
    PC_PSD_V_DOUBLE,        /* doub */
    PC_PSD_V_UNIT_FLOAT,    /* UntF */
    PC_PSD_V_UNIT_FLOATS,   /* UnFl */
    PC_PSD_V_TEXT,          /* TEXT */
    PC_PSD_V_ENUMERATED,    /* enum */
    PC_PSD_V_INTEGER,       /* long */
    PC_PSD_V_LARGE_INTEGER, /* comp */
    PC_PSD_V_BOOLEAN,       /* bool */
    PC_PSD_V_CLASS,         /* type */
    PC_PSD_V_GLOBAL_CLASS,  /* GlbC */
    PC_PSD_V_ALIAS,         /* alis */
    PC_PSD_V_PATH,          /* Pth  */
    PC_PSD_V_RAW_DATA,      /* tdta */
    PC_PSD_V_OBJECT_ARRAY   /* ObAr: prefix + body */
} pc_psd_vkind;

typedef struct pc_psd_desc pc_psd_desc;
typedef struct pc_psd_value pc_psd_value;

struct pc_psd_value {
    pc_psd_vkind kind;
    pc_psd_desc *desc;      /* DESCRIPTOR, GLOBAL_OBJECT, OBJECT_ARRAY (body); owned */
    uint32_t prefix;        /* OBJECT_ARRAY */
    pc_psd_value *list;     /* LIST */
    size_t nlist;
    pc_psd_ref_item *refs;  /* REFERENCE */
    size_t nrefs;
    double d;               /* DOUBLE, UNIT_FLOAT */
    uint8_t unit[4];        /* UNIT_FLOAT(S) */
    double *floats;         /* UNIT_FLOATS */
    size_t nfloats;
    pc_psd_ustr text;       /* TEXT */
    pc_psd_id type_id;      /* ENUMERATED */
    pc_psd_id value;        /* ENUMERATED */
    int32_t i;              /* INTEGER */
    int64_t l;              /* LARGE_INTEGER */
    bool b;                 /* BOOLEAN */
    pc_psd_class cls;       /* CLASS, GLOBAL_CLASS */
    pc_psd_bytes raw;       /* ALIAS, PATH, RAW_DATA */
};

typedef struct pc_psd_item {
    pc_psd_id key;
    pc_psd_value value;
} pc_psd_item;

struct pc_psd_desc {
    pc_psd_ustr name;
    pc_psd_id class_id;
    pc_psd_item *items;
    size_t n;
    size_t cap;
};

/* ---- strings and ids ---- */

void pc_psd_ustr_fini(pc_psd_ustr *s);
/* UTF-8 → units, optionally with a trailing NUL (UnicodeString::new / new_nul). */
pc_status pc_psd_ustr_from_utf8(const char *s, bool nul, pc_psd_ustr *out, pc_error *err);
pc_status pc_psd_ustr_from_utf8n(const char *s, size_t len, bool nul, pc_psd_ustr *out, pc_error *err);
/* Lossy UTF-8, trailing NULs stripped; owned, NUL-terminated (free()). */
pc_status pc_psd_ustr_to_utf8(const pc_psd_ustr *s, char **out, pc_error *err);
bool pc_psd_ustr_eq(const pc_psd_ustr *a, const pc_psd_ustr *b);
pc_status pc_psd_ustr_clone(const pc_psd_ustr *s, pc_psd_ustr *out, pc_error *err);

/* 4-byte strings become codes, others explicit strings (Id::new). */
pc_status pc_psd_id_make(const char *s, pc_psd_id *out, pc_error *err);
void pc_psd_id_fini(pc_psd_id *id);
bool pc_psd_id_is(const pc_psd_id *id, const char *s);
const uint8_t *pc_psd_id_bytes(const pc_psd_id *id, size_t *len);
bool pc_psd_id_eq(const pc_psd_id *a, const pc_psd_id *b);

/* ---- descriptors ---- */

/* An empty descriptor of class `class_id`. */
pc_status pc_psd_desc_init(pc_psd_desc *d, const char *class_id, pc_error *err);
void pc_psd_desc_fini(pc_psd_desc *d);
void pc_psd_desc_free(pc_psd_desc *d); /* fini + free of a heap descriptor; NULL-safe */
pc_status pc_psd_desc_clone(const pc_psd_desc *d, pc_psd_desc *out, pc_error *err);
bool pc_psd_desc_eq(const pc_psd_desc *a, const pc_psd_desc *b);
/* Appends key → value, taking ownership of *value (zeroed afterwards), also on failure. */
pc_status pc_psd_desc_add(pc_psd_desc *d, const char *key, pc_psd_value *value, pc_error *err);
/* First item with key `key`, borrowed, or NULL. */
const pc_psd_value *pc_psd_desc_get(const pc_psd_desc *d, const char *key);

/* Parses a descriptor that must use all of data ("trailing bytes after descriptor"). */
pc_status pc_psd_desc_parse(const uint8_t *data, size_t len, pc_psd_desc *out, pc_error *err);
pc_status pc_psd_desc_to_bytes(const pc_psd_desc *d, pc_psd_bytes *out, pc_error *err);

/* Versioned descriptor: u32 version (must be 16) + descriptor. parse needs all of data;
 * parse_prefix allows trailing data and reports the bytes used. */
pc_status pc_psd_vdesc_parse(const uint8_t *data, size_t len, pc_psd_desc *out, pc_error *err);
pc_status pc_psd_vdesc_parse_prefix(const uint8_t *data, size_t len, pc_psd_desc *out, size_t *used, pc_error *err);
pc_status pc_psd_vdesc_to_bytes(const pc_psd_desc *d, pc_psd_bytes *out, pc_error *err);

/* ---- values ---- */

void pc_psd_value_fini(pc_psd_value *v);
pc_status pc_psd_value_clone(const pc_psd_value *v, pc_psd_value *out, pc_error *err);
bool pc_psd_value_eq(const pc_psd_value *a, const pc_psd_value *b);
/* Simple constructors (zero everything else). */
pc_psd_value pc_psd_value_int(int32_t i);
pc_psd_value pc_psd_value_double(double d);
pc_psd_value pc_psd_value_bool(bool b);
pc_status pc_psd_value_text(const char *utf8, bool nul, pc_psd_value *out, pc_error *err);
pc_status pc_psd_value_enum(const char *type_id, const char *value, pc_psd_value *out, pc_error *err);
/* A DESCRIPTOR value taking ownership of *d (zeroed), also on failure. */
pc_status pc_psd_value_object(pc_psd_desc *d, pc_psd_value *out, pc_error *err);
/* Appends to a LIST value, taking ownership of *item (zeroed), also on failure. */
pc_status pc_psd_value_list_push(pc_psd_value *list, pc_psd_value *item, pc_error *err);

/* For other modules: read / write at a cursor (internal.h types). */
struct psd_rd;
struct psd_w;
pc_status psd_read_desc(struct psd_rd *r, pc_psd_desc *out, pc_error *err);
pc_status psd_read_vdesc(struct psd_rd *r, pc_psd_desc *out, pc_error *err);
void psd_write_desc(struct psd_w *w, const pc_psd_desc *d);
void psd_write_vdesc(struct psd_w *w, const pc_psd_desc *d);
pc_status psd_read_ustr(struct psd_rd *r, pc_psd_ustr *out, pc_error *err);
void psd_write_ustr(struct psd_w *w, const pc_psd_ustr *s);

#endif
