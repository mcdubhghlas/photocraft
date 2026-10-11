/* PSD type layers: the TySh block (transform + text descriptor with EngineData + warp) <-> the
 * typed pc_text_layer model, the document's Txt2 block, and named style sheets (port of
 * crates/text/src/psd.rs and psd_styles.rs).
 *
 * Units: EngineData sizes are in text-space units, which the TySh transform maps to document
 * pixels; Photoshop shows them as points at the document resolution, so
 * size_pt = FontSize * 72 / dpi (likewise leading, baseline shift, indents and spacing). Box
 * bounds stay in text-space pixels. A dpi <= 0 (or NaN) means 72.
 *
 * Writing starts from the layer's original TySh (pc_text_layer.psd_raw, when present) and patches
 * only the text, runs, paragraphs, fonts, shape and transform, so Photoshop-only settings survive
 * an edit. Output bytes are identical to the reference's.
 *
 * Errors: the reference returns Option for data it can't read. Here that "None" is
 * PC_ERR_FORMAT (with a short message saying what was missing); PC_ERR_NOMEM / PC_ERR_LIMIT are
 * resource failures. Callers that only need the Option treat every status other than OK and
 * NOMEM as None. Out-params are written only on success. All input bytes are untrusted.
 * Thread-safe (no shared state); the layer arguments are only read unless documented. */
#ifndef PC_TEXT_PSD_H
#define PC_TEXT_PSD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc/text.h"
#include "doc/text_styles.h"
#include "psd/descriptor.h"
#include "text/engine_data.h"

/* Parsed TySh contents. */
typedef struct pc_tysh {
    pc_affine transform;
    pc_psd_desc text; /* the text descriptor (class TxLr) */
    bool has_warp;
    pc_psd_desc warp;
    int32_t bounds[4]; /* the four trailing i32 values (left, top, right, bottom) */
} pc_tysh;

void pc_tysh_fini(pc_tysh *t); /* NULL-safe */
/* parse_tysh. PC_ERR_FORMAT "invalid TySh block" when the transform or text descriptor can't be
 * read; a missing warp or bounds is tolerated like the reference. */
pc_status pc_tysh_parse(const uint8_t *data, size_t len, pc_tysh *out, pc_error *err);
/* write_tysh (a missing warp is written as the default "warpNone" descriptor). */
pc_status pc_tysh_write(const pc_tysh *t, pc_psd_bytes *out, pc_error *err);

/* The EngineData of a text descriptor (its `EngineData` raw-data item, parsed). PC_ERR_FORMAT
 * when absent or unparsable. */
pc_status pc_tysh_engine_data(const pc_psd_desc *text, pc_ed_value *out, pc_error *err);
/* Whether the text engine of a TySh block has nothing to draw: EngineDict/Editor/Text is present
 * and only white space (false for unreadable data). */
pc_status pc_tysh_engine_text_is_blank(const uint8_t *tysh, size_t len, bool *out, pc_error *err);

/* The document's Txt2 block (a bare `/key value` sequence) as a dictionary. PC_ERR_FORMAT when
 * it doesn't parse. */
pc_status pc_txt2_parse(const uint8_t *data, size_t len, pc_ed_value *out, pc_error *err);
/* Applies the automatic kerning modes Photoshop keeps only in Txt2 (Optical, "0") to `layer`,
 * read from its TySh `tysh`, when the layer's TextIndex object in Txt2 still holds the layer's
 * text. Missing or stale data leaves the layer unchanged (PC_OK). On failure (NOMEM) the layer is
 * unchanged. */
pc_status pc_txt2_apply(pc_text_layer *layer, const uint8_t *tysh, size_t len, const pc_ed_value *txt2,
                        pc_error *err);

/* A text layer (model, text and transform; no cache, no psd_raw) from TySh data.
 * PC_ERR_FORMAT when the block can't be read (pc_tysh_parse). */
pc_status pc_text_layer_from_tysh(const uint8_t *data, size_t len, float dpi, pc_text_layer *out,
                                  pc_error *err);

/* Updates (or creates) an EngineData tree for `layer`. `tmpl` (may be NULL = None) is taken over
 * (left as an empty dictionary) whether or not the call succeeds; a non-dictionary template is
 * replaced by a fresh tree. */
pc_status pc_text_build_engine_data(const pc_text_layer *layer, pc_ed_value *tmpl, float dpi,
                                    pc_ed_value *out, pc_error *err);
/* The TySh block for `layer`, patching its psd_raw when present. `ink` (NULL = None) is the
 * layer's rendered bounds in text space [l, t, r, b] (for point text's bounds/boundingBox). */
pc_status pc_text_build_tysh(const pc_text_layer *layer, float dpi, const float *ink, pc_psd_bytes *out,
                             pc_error *err);

/* UTF-16 run lengths over `text` -> UTF-8 byte lengths (out has n entries; negative/NaN lengths
 * count as 0, lengths past the end stop at the end). */
void pc_text_utf16_to_byte_lengths(const char *text, size_t len, const double *lens, size_t n, size_t *out);
/* The PostScript name the PSD FontSet stores for a style ("Inter-SemiBoldItalic"). */
pc_status pc_text_postscript_for(const pc_char_style *s, pc_str *out, pc_error *err);

/* EngineData style keys for the Type > OpenType toggles and the feature tag each turns on. */
#define PC_TEXT_OPENTYPE_KEY_COUNT 8
extern const char *const PC_TEXT_OPENTYPE_KEYS[PC_TEXT_OPENTYPE_KEY_COUNT][2];

/* ---- named style sheets (psd_styles.rs) ----
 *
 * How Photoshop links a run to a named sheet isn't in Adobe's published PSD spec. Reading takes a
 * `Parent` index, else a `Name`, on the run's sheet; writing sets `Parent` (plus the run's fully
 * resolved attributes). Unverified against Photoshop, like the reference. */

typedef struct pc_opt_index {
    bool has;
    size_t v;
} pc_opt_index;

typedef struct pc_psd_char_sheet {
    size_t index; /* position in StyleSheetSet */
    pc_str name;
    pc_char_style style; /* resolved */
} pc_psd_char_sheet;

typedef struct pc_psd_para_sheet {
    size_t index; /* position in ParagraphSheetSet */
    pc_str name;
    pc_paragraph_style style;
    bool has_char; /* character style of its DefaultStyleSheet when that isn't the normal sheet */
    pc_char_style chr;
} pc_psd_para_sheet;

typedef struct pc_psd_style_sheets {
    pc_char_style normal_char; /* the default ("Normal") character sheet, resolved */
    pc_paragraph_style normal_para;
    pc_psd_char_sheet *char_sheets; /* named sheets other than the normal one */
    size_t nchar_sheets;
    pc_psd_para_sheet *para_sheets;
    size_t npara_sheets;
    pc_opt_index *run_char; /* per StyleRun entry: the character sheet it refers to */
    size_t nrun_char;
    size_t *run_char_lens; /* per StyleRun entry: UTF-8 length in the layer text */
    size_t nrun_char_lens;
    pc_opt_index *run_para; /* per ParagraphRun entry: the paragraph sheet it refers to */
    size_t nrun_para;
} pc_psd_style_sheets;

void pc_psd_style_sheets_fini(pc_psd_style_sheets *s); /* NULL-safe */
/* read_style_sheets. PC_ERR_FORMAT when the block has no readable engine data / resources. */
pc_status pc_text_read_style_sheets(const uint8_t *tysh, size_t len, float dpi, pc_psd_style_sheets *out,
                                    pc_error *err);
/* Whether a TySh block carries named sheets beyond the normal ones (false for unreadable data). */
pc_status pc_text_has_named_sheets(const uint8_t *tysh, size_t len, bool *out, pc_error *err);
/* Rewrites the style sheets of a TySh block from the document's styles and the layer's run
 * references (the normal sheets are kept). PC_ERR_FORMAT when the block or its engine data
 * can't be read or the descriptor has no EngineData item. */
pc_status pc_text_write_style_sheets(const uint8_t *tysh, size_t len, const pc_text_layer *layer,
                                     const pc_text_styles *styles, float dpi, pc_psd_bytes *out,
                                     pc_error *err);

#endif
