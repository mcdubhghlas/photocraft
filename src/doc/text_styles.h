/* Named type styles: Character Styles and Paragraph Styles (port of
 * crates/doc/src/text_styles.rs).
 *
 * Runs keep fully resolved attributes and reference their styles by id
 * (pc_char_style.style_sheet, pc_paragraph_style.style_sheet). A run's attributes from its
 * styles alone are defaults <- Basic Paragraph <- paragraph style <- character style; what
 * differs is a local override. Attribute sets are JSON objects keyed by the serde field names of
 * pc_char_style / pc_paragraph_style ("size_pt", "align", ...), with serde_json::Value semantics
 * (pc_json_value, root object; a NULL doc is the empty set).
 *
 * Errors: functions returning pc_status fail with PC_ERR_INVALID and the reference's message
 * (serde_json's from_value text, or "unknown style attribute \"x\"") when an attribute set does
 * not apply; PC_ERR_NOMEM otherwise. Conventions: doc/adjust.h. */
#ifndef PC_DOC_TEXT_STYLES_H
#define PC_DOC_TEXT_STYLES_H

#include "base/json.h"
#include "doc/text.h"

#define PC_BASIC_PARAGRAPH "Basic Paragraph"
#define PC_NO_CHARACTER_STYLE "None"

/* A partial attribute set (serde field name -> value). */
typedef pc_json_value pc_style_attrs;

bool pc_style_attrs_is_empty(const pc_style_attrs *a);
size_t pc_style_attrs_len(const pc_style_attrs *a);
/* The value of key, or NULL. Borrowed. */
yyjson_mut_val *pc_style_attrs_get(const pc_style_attrs *a, const char *key);
/* Inserts (or replaces) key with a normalised copy of v (NULL = null), keeping keys sorted. */
pc_status pc_style_attrs_insert(pc_style_attrs *a, const char *key, const yyjson_mut_val *v, pc_error *err);

typedef struct pc_character_style_def {
    uint32_t id; /* >= 1 */
    pc_str name;
    pc_style_attrs attrs;
} pc_character_style_def;

typedef struct pc_paragraph_style_def {
    uint32_t id; /* >= 1; Basic Paragraph is 0 */
    pc_str name;
    pc_style_attrs para_attrs;
    pc_style_attrs char_attrs;
} pc_paragraph_style_def;

typedef struct pc_text_styles {
    pc_character_style_def *character; /* panel order ("None" is implicit) */
    size_t ncharacter;
    pc_paragraph_style_def *paragraph; /* panel order (Basic Paragraph is `basic`) */
    size_t nparagraph;
    pc_paragraph_style_def basic;
    pc_json_value psd_raw; /* Option<Value>: doc NULL = None */
} pc_text_styles;

void pc_character_style_def_fini(pc_character_style_def *d);
pc_status pc_character_style_def_clone(const pc_character_style_def *d, pc_character_style_def *out, pc_error *err);
bool pc_character_style_def_eq(const pc_character_style_def *a, const pc_character_style_def *b);
void pc_paragraph_style_def_fini(pc_paragraph_style_def *d);
pc_status pc_paragraph_style_def_clone(const pc_paragraph_style_def *d, pc_paragraph_style_def *out, pc_error *err);
bool pc_paragraph_style_def_eq(const pc_paragraph_style_def *a, const pc_paragraph_style_def *b);

/* No styles; basic = {0, "Basic Paragraph", {}, {}}. */
pc_status pc_text_styles_default(pc_text_styles *out, pc_error *err);
void pc_text_styles_fini(pc_text_styles *s);
pc_status pc_text_styles_clone(const pc_text_styles *s, pc_text_styles *out, pc_error *err);
bool pc_text_styles_eq(const pc_text_styles *a, const pc_text_styles *b);
bool pc_text_styles_is_empty(const pc_text_styles *s);

/* Lookups (borrowed; NULL when missing). para id 0 = Basic Paragraph. */
const pc_character_style_def *pc_text_styles_char_style(const pc_text_styles *s, uint32_t id);
pc_character_style_def *pc_text_styles_char_style_mut(pc_text_styles *s, uint32_t id);
const pc_paragraph_style_def *pc_text_styles_para_style(const pc_text_styles *s, uint32_t id);
pc_paragraph_style_def *pc_text_styles_para_style_mut(pc_text_styles *s, uint32_t id);
/* Fresh ids (max + 1); false when the stored ids exhaust u32. */
bool pc_text_styles_next_char_id(const pc_text_styles *s, uint32_t *out);
bool pc_text_styles_next_para_id(const pc_text_styles *s, uint32_t *out);
/* "<base> N" with the first unused N. */
pc_status pc_text_styles_unique_name(const pc_text_styles *s, bool paragraph, const char *base, pc_str *out,
                                     pc_error *err);

/* Optional ids are pointers: NULL = None. */
pc_status pc_text_styles_resolve_para(const pc_text_styles *s, const uint32_t *para, pc_paragraph_style *out,
                                      pc_error *err);
pc_status pc_text_styles_resolve_char(const pc_text_styles *s, const uint32_t *para, const uint32_t *chr,
                                      const char *default_family, pc_char_style *out, pc_error *err);
pc_status pc_text_styles_char_overrides(const pc_text_styles *s, const pc_char_style *run, const uint32_t *para,
                                        const char *default_family, pc_style_attrs *out, pc_error *err);
pc_status pc_text_styles_para_overrides(const pc_text_styles *s, const pc_paragraph_style *p, pc_style_attrs *out,
                                        pc_error *err);
pc_status pc_text_styles_restyle_char(const pc_text_styles *s, const pc_text_styles *old, const pc_char_style *run,
                                      const uint32_t *old_para, const uint32_t *new_para, const uint32_t *new_chr,
                                      const char *family, pc_char_style *out, pc_error *err);
pc_status pc_text_styles_restyle_para(const pc_text_styles *s, const pc_text_styles *old, const pc_paragraph_style *p,
                                      const uint32_t *new_para, pc_paragraph_style *out, pc_error *err);

/* base with attrs applied (apply_attrs). PC_ERR_INVALID with the reference's message when an
 * attribute is unknown or has the wrong type. */
pc_status pc_char_style_apply_attrs(const pc_char_style *base, const pc_style_attrs *attrs, pc_char_style *out,
                                    pc_error *err);
pc_status pc_paragraph_style_apply_attrs(const pc_paragraph_style *base, const pc_style_attrs *attrs,
                                         pc_paragraph_style *out, pc_error *err);
/* Attributes of style that differ from base (the style reference ignored). */
pc_status pc_char_style_diff_attrs(const pc_char_style *style, const pc_char_style *base, pc_style_attrs *out,
                                   pc_error *err);
pc_status pc_paragraph_style_diff_attrs(const pc_paragraph_style *style, const pc_paragraph_style *base,
                                        pc_style_attrs *out, pc_error *err);
/* Every attribute (without the style reference). */
pc_status pc_char_style_all_attrs(const pc_char_style *s, pc_style_attrs *out, pc_error *err);
pc_status pc_paragraph_style_all_attrs(const pc_paragraph_style *s, pc_style_attrs *out, pc_error *err);
/* validate::<CharStyle> / validate::<ParagraphStyle>. */
pc_status pc_char_style_validate_attrs(const pc_style_attrs *attrs, pc_error *err);
pc_status pc_paragraph_style_validate_attrs(const pc_style_attrs *attrs, pc_error *err);

#endif
