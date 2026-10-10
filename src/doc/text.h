/* Type layers and their typed style model: character runs, paragraph runs, point/box shape and
 * warp (port of crates/doc/src/text.rs and lib.rs TextLayer). Layout is src/text.
 *
 * Sizes, leading, indents and spacing are points (px = pt * dpi / 72); tracking is 1/1000 em;
 * box bounds are text-space pixels. Runs cover the text by UTF-8 byte length, in order; run
 * lists that don't add up are normalised by pc_text_layer_char_runs. Conventions: doc/adjust.h. */
#ifndef PC_DOC_TEXT_H
#define PC_DOC_TEXT_H

#include "color/color.h"
#include "doc/adjust.h"
#include "geom/geom.h"
#include "raster/surface.h"

typedef enum pc_kerning { PC_KERNING_METRICS, PC_KERNING_OPTICAL, PC_KERNING_OFF } pc_kerning;
typedef enum pc_caps { PC_CAPS_NORMAL, PC_CAPS_SMALL_CAPS, PC_CAPS_ALL_CAPS } pc_caps;

typedef struct pc_font_feature {
    pc_str tag;
    uint16_t value;
} pc_font_feature;

typedef struct pc_font_variation {
    pc_str axis;
    float value;
} pc_font_variation;

/* Character style (Character panel). Field order = the reference's serde order. */
typedef struct pc_char_style {
    pc_str font_family;
    pc_str font_style;
    bool has_postscript_name;
    pc_str postscript_name;
    uint16_t weight;
    bool italic;
    float size_pt;
    pc_color color;
    float tracking;
    bool has_leading;
    float leading_pt;
    float baseline_shift_pt;
    float horizontal_scale;
    float vertical_scale;
    bool underline;
    bool strikethrough;
    bool faux_bold;
    bool faux_italic;
    pc_kerning kerning;
    float kern;
    pc_caps caps;
    bool ligatures;
    bool discretionary_ligatures;
    pc_font_feature *features;
    size_t nfeatures;
    pc_font_variation *variations;
    size_t nvariations;
    bool has_language;
    pc_str language;
    bool has_style_sheet; /* applied character style id */
    uint32_t style_sheet;
} pc_char_style;

/* Weight 400, 12 pt, black, scales 1, ligatures on; everything else empty/off. No allocation. */
void pc_char_style_default(pc_char_style *out);
void pc_char_style_fini(pc_char_style *s);
pc_status pc_char_style_clone(const pc_char_style *s, pc_char_style *out, pc_error *err);
bool pc_char_style_eq(const pc_char_style *a, const pc_char_style *b);

typedef struct pc_text_run {
    size_t len; /* UTF-8 bytes */
    pc_char_style style;
} pc_text_run;

typedef enum pc_text_align {
    PC_TEXT_ALIGN_LEFT,
    PC_TEXT_ALIGN_CENTER,
    PC_TEXT_ALIGN_RIGHT,
    PC_TEXT_ALIGN_JUSTIFY_LEFT,
    PC_TEXT_ALIGN_JUSTIFY_CENTER,
    PC_TEXT_ALIGN_JUSTIFY_RIGHT,
    PC_TEXT_ALIGN_JUSTIFY_ALL
} pc_text_align;

bool pc_text_align_is_justified(pc_text_align a);

typedef enum pc_text_direction { PC_TEXT_DIR_AUTO, PC_TEXT_DIR_LTR, PC_TEXT_DIR_RTL } pc_text_direction;

/* Paragraph style (Paragraph panel). Plain data (no allocation). */
typedef struct pc_paragraph_style {
    pc_text_align align;
    float first_line_indent_pt;
    float start_indent_pt;
    float end_indent_pt;
    float space_before_pt;
    float space_after_pt;
    float auto_leading; /* default 1.2 */
    pc_text_direction direction;
    bool hyphenate;
    bool has_style_sheet; /* applied paragraph style id; None = Basic Paragraph */
    uint32_t style_sheet;
} pc_paragraph_style;

pc_paragraph_style pc_paragraph_style_default(void);
bool pc_paragraph_style_eq(const pc_paragraph_style *a, const pc_paragraph_style *b);

typedef struct pc_paragraph_run {
    size_t len;
    pc_paragraph_style style;
} pc_paragraph_run;

/* Point text, or paragraph (box) text in text-space pixels. */
typedef struct pc_text_shape {
    bool is_box;
    float x, y, width, height;
} pc_text_shape;

typedef enum pc_orientation { PC_ORIENT_HORIZONTAL, PC_ORIENT_VERTICAL } pc_orientation;

typedef enum pc_antialias {
    PC_AA_NONE,
    PC_AA_SHARP,
    PC_AA_CRISP,
    PC_AA_STRONG,
    PC_AA_SMOOTH, /* default */
    PC_AA_WINDOWS,
    PC_AA_WINDOWS_LCD
} pc_antialias;

/* Warp Text settings (PSD warp descriptor). */
typedef struct pc_text_warp {
    pc_str style; /* "warpNone", "warpArc", ... */
    float value;
    float horizontal_distortion;
    float vertical_distortion;
    bool horizontal;
} pc_text_warp;

void pc_text_warp_fini(pc_text_warp *w);
pc_status pc_text_warp_clone(const pc_text_warp *w, pc_text_warp *out, pc_error *err);
bool pc_text_warp_eq(const pc_text_warp *a, const pc_text_warp *b);

typedef struct pc_text_layer {
    pc_str text;
    pc_str font_family; /* summary of the first run */
    float size_pt;
    pc_color color;
    pc_affine transform;
    bool has_cache;
    pc_surface cache;
    pc_blob *psd_raw; /* TySh block data; NULL = None */
    pc_text_run *runs; /* empty = one run from the summary fields */
    size_t nruns;
    pc_paragraph_run *paragraphs; /* empty = default paragraph style */
    size_t nparagraphs;
    pc_text_shape shape;
    pc_orientation orientation;
    pc_antialias antialias;
    bool has_warp;
    pc_text_warp warp;
} pc_text_layer;

/* Empty text, 12 pt black, identity transform, point, horizontal, smooth. No allocation. */
void pc_text_layer_default(pc_text_layer *out);
void pc_text_layer_fini(pc_text_layer *t);
pc_status pc_text_layer_clone(const pc_text_layer *t, pc_text_layer *out, pc_error *err);
bool pc_text_layer_eq(const pc_text_layer *a, const pc_text_layer *b);

/* Runs covering exactly the text's byte length, each ending on a char boundary (stretching or
 * truncating, dropping empty runs; one run for empty text). Arrays are newly allocated (free
 * with pc_text_runs_free / free()). */
pc_status pc_text_layer_char_runs(const pc_text_layer *t, pc_text_run **out, size_t *n, pc_error *err);
pc_status pc_text_layer_paragraph_runs(const pc_text_layer *t, pc_paragraph_run **out, size_t *n, pc_error *err);
void pc_text_runs_free(pc_text_run *runs, size_t n);
/* Copies the first run's family, size and colour into the summary fields. */
pc_status pc_text_layer_sync_summary(pc_text_layer *t, pc_error *err);

#endif
