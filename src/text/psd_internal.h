/* Helpers shared by psd_read.c, psd_write.c and psd_styles.c (the reference's pub(crate) items in
 * crates/text/src/psd.rs). Not a public header. */
#ifndef PC_TEXT_PSD_INTERNAL_H
#define PC_TEXT_PSD_INTERNAL_H

#include "text/psd.h"

/* A list of owned strings (font names, sheet names). */
typedef struct psd_names {
    pc_str *v;
    size_t n;
    size_t cap;
} psd_names;

void psd_names_fini(psd_names *l);
pc_status psd_names_push(psd_names *l, const char *s, size_t len, pc_error *err);
/* Index of the first name equal to s, or n. */
size_t psd_names_find(const psd_names *l, const char *s, size_t len);
/* `Name` (string, or "") of every element of an array value; empty for non-arrays. */
pc_status psd_names_of(const pc_ed_value *set, psd_names *out, pc_error *err);
/* font_index: position of s in l, appending it when missing. */
pc_status psd_names_index(psd_names *l, const pc_str *s, size_t *out, pc_error *err);

/* 72 / dpi (dpi <= 0 or NaN -> 1), in f32 like the reference. */
float psd_k_of(float dpi);
/* arr_f: the numbers of an array value (others skipped). *out is malloc'd (NULL when empty). */
pc_status psd_arr_f(const pc_ed_value *v, double **out, size_t *n, pc_error *err);
/* utf16_to_byte_lengths over arr_f(lens_value); *out malloc'd with *n entries. */
pc_status psd_byte_lengths(const pc_str *text, const pc_ed_value *lens_value, size_t **out, size_t *n,
                           pc_error *err);

/* char_style / para_style: a style from an EngineData style dictionary `d` over `base`. */
pc_status psd_char_style(const pc_ed_value *base, const pc_ed_value *d, const psd_names *fonts, float k,
                         pc_char_style *out, pc_error *err);
pc_paragraph_style psd_para_style(const pc_ed_value *base, const pc_ed_value *d, float k);

/* style_sheet_data / paragraph_properties / font_entry as EngineData dictionaries. */
pc_status psd_style_sheet_data(const pc_char_style *s, size_t font, float k, pc_ed_value *out, pc_error *err);
pc_status psd_paragraph_properties(const pc_paragraph_style *p, float k, pc_ed_value *out, pc_error *err);
pc_status psd_font_entry(const pc_str *name, pc_ed_value *out, pc_error *err);
/* An array of font_entry for every name. */
pc_status psd_font_set(const psd_names *fonts, pc_ed_value *out, pc_error *err);

/* Sets dict[key] = Int(i) / Bool(b) / Real(r) / String(s). */
pc_status psd_set_int(pc_ed_value *d, const char *key, int64_t i, pc_error *err);
pc_status psd_set_bool(pc_ed_value *d, const char *key, bool b, pc_error *err);
pc_status psd_set_real(pc_ed_value *d, const char *key, double r, pc_error *err);
pc_status psd_set_str(pc_ed_value *d, const char *key, const char *s, size_t len, pc_error *err);

/* Copy of s[0..len) with every byte `from` replaced by `to` (str::replace of ASCII chars). */
pc_status psd_replace_byte(const char *s, size_t len, char from, char to, pc_str *out, pc_error *err);

/* Next char of (normally valid) UTF-8 text at *i: advances *i and returns the scalar value.
 * An invalid byte counts as one U+FFFD char of one byte, so iteration always progresses. */
uint32_t psd_next_char(const uint8_t *s, size_t len, size_t *i);
static inline size_t psd_utf16_len(uint32_t cp)
{
    return cp >= 0x10000 ? 2 : 1;
}

/* A parse failure that is the reference's None (anything but out-of-memory). */
static inline bool psd_is_none(pc_status s)
{
    return s != PC_OK && s != PC_ERR_NOMEM;
}

#endif
