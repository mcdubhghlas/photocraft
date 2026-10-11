/* Shaping and layout of a text layer into positioned glyphs (port of crates/text/src/layout.rs):
 * text-space pixels, y down, first baseline of point text at y = 0.
 *
 * Each paragraph is shaped and line-broken by the parley port (playout.h: HarfBuzz shaping,
 * FriBidi bidi, libunibreak UAX #14 line breaking); paragraphs are stacked with Photoshop's
 * rules: baseline-to-baseline distance = the largest leading on the line (auto leading =
 * paragraph factor x size), space before/after, indents, point-text alignment around the
 * anchor, manual and optical kerning, horizontal scale, synthetic small caps.
 *
 * Vertical type (tategaki) is laid out in the same line space and turned 90 degrees clockwise
 * into text space: CJK stays upright (VORG / vhea+vmtx / ideographic em box, `vert`
 * alternates), other text runs rotated; lines, clusters, carets and hit tests stay in line space
 * (pc_text_layout_to_text / _to_line convert).
 *
 * Threads: a pc_text_layouter is single-owner (it caches HarfBuzz faces and optical kerning);
 * use one per thread or lock around it (pc_text_engine does). The free functions are pure. */
#ifndef PC_TEXT_LAYOUT_H
#define PC_TEXT_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "doc/text.h"
#include "text/fonts.h"
#include "text/layout_data.h"

/* Longest layer text laid out (bytes); longer text is PC_ERR_LIMIT. */
#define PC_TEXT_MAX_LAYOUT_BYTES ((size_t)64 << 20)

/* A line break inside a paragraph (Shift+Return), stored as U+0003 in PSD type. */
#define PC_TEXT_FORCED_LINE_BREAK 0x3u

typedef struct pc_text_layouter pc_text_layouter;

pc_status pc_text_layouter_new(pc_text_layouter **out, pc_error *err);
void pc_text_layouter_free(pc_text_layouter *lo); /* NULL-safe */

/* Lays out `t` with fonts from `db` at `dpi` (px per pt = dpi / 72, 1 for dpi <= 0 or NaN).
 * `out` is initialised by this call; release it with pc_text_layout_fini. Fails with
 * PC_ERR_INVALID for text that is not valid UTF-8, PC_ERR_LIMIT past PC_TEXT_MAX_LAYOUT_BYTES,
 * PC_ERR_NOMEM; any style or shape values are accepted (NaN, negative, absurd). */
pc_status pc_text_layouter_layout(pc_text_layouter *lo, pc_fontdb *db, const pc_text_layer *t, float dpi,
                                  pc_text_layout *out, pc_error *err);

/* ---- carets, hit tests, navigation (layout_nav.c) ---- */

/* Byte offset of the caret nearest to a text-space / line-space point. */
size_t pc_text_layout_hit_test(const pc_text_layout *l, float x, float y);
size_t pc_text_layout_hit_test_line(const pc_text_layout *l, float x, float y);
/* Caret geometry for a byte offset in line space: x, top, bottom. */
void pc_text_layout_caret(const pc_text_layout *l, size_t offset, float *x, float *top, float *bottom);
/* The caret as a text-space segment {x0, y0, x1, y1}. */
void pc_text_layout_caret_segment(const pc_text_layout *l, size_t offset, float out[4]);

/* Byte offset of character `index` (the text length past the end). */
size_t pc_text_byte_index(const char *text, size_t len, size_t index);
/* Character index of byte `byte`, floored to a char boundary. */
size_t pc_text_char_index(const char *text, size_t len, size_t byte);
/* Line containing a byte offset (the last line when past every line; 0 without lines). */
size_t pc_text_line_index(const pc_text_layout *l, size_t byte);
/* Nearest caret to a text-space point: character index and its line. */
void pc_text_hit_char(const pc_text_layout *l, const char *text, size_t len, float x, float y, size_t *index,
                      size_t *line);
/* Text-space point inside the line boxes expanded by `slop` px (non-finite slop = 0). */
bool pc_text_point_inside(const pc_text_layout *l, float x, float y, float slop);
/* Character index of the word boundary before (forward false) or after idx; a word is a run of
 * alphanumeric characters. */
size_t pc_text_word_boundary(const char *text, size_t len, size_t idx, bool forward);
/* Caret on the neighbouring line (dir < 0 previous) keeping line-space x; past the first/last
 * line the caret goes to the start/end of the text. Character indices. */
size_t pc_text_line_step(const pc_text_layout *l, const char *text, size_t len, size_t idx, float x, int dir);
/* Line start (end false) or end for the line containing idx, as a character index. */
size_t pc_text_line_edge(const pc_text_layout *l, const char *text, size_t len, size_t idx, bool end);

/* ---- helpers exposed for tests and later chunks ---- */

/* How a character is set in vertical type (simplified UAX #50). */
typedef enum pc_vclass {
    PC_VCLASS_ROTATE = 0,             /* rotated 90 degrees clockwise (Latin, digits, symbols) */
    PC_VCLASS_UPRIGHT = 1,            /* upright, with a vertical alternate when the font has one */
    PC_VCLASS_ALTERNATE_OR_ROTATE = 2 /* upright through its vertical alternate, else rotated */
} pc_vclass;

pc_vclass pc_text_vertical_class(uint32_t c);

/* Byte ranges of paragraphs (each with its \r, \n or \r\n); text ending with a break yields a
 * final empty paragraph. *out (free()) holds 2 * *n offsets: start, end. */
pc_status pc_text_split_paragraphs(const char *text, size_t len, size_t **out, size_t *n, pc_error *err);

/* Japanese punctuation squeeze for one line in line space (layout.rs squeeze_punctuation):
 * returns the length removed; clusters and glyphs from the squeezed points move back. Ranges
 * outside `text` are ignored. */
float pc_text_squeeze_punctuation(const char *text, size_t len, pc_cluster_info *clusters, size_t nclusters,
                                  pc_placed_glyph *glyphs, size_t nglyphs);

/* Synthetic emboldening only for a bold request (>= 600) on a face that isn't bold (<= 500). */
bool pc_text_synthetic_bold(bool matcher_says, uint16_t requested, float face_weight);

/* Small caps rendering path of a character style. */
typedef enum pc_small_caps_mode {
    PC_SMALL_CAPS_NONE,
    PC_SMALL_CAPS_OPENTYPE,
    PC_SMALL_CAPS_SYNTHETIC
} pc_small_caps_mode;

pc_small_caps_mode pc_text_small_caps_mode(pc_caps caps, bool face_has_smcp);

/* The OpenType feature settings a style requests (layout.rs feature_list, as parsed by the
 * shaper): up to cap (tag, value) pairs written, the count returned. */
typedef struct pc_text_feature {
    uint32_t tag;
    uint16_t value;
} pc_text_feature;

size_t pc_text_style_features(const pc_char_style *st, pc_small_caps_mode mode, pc_text_feature *out,
                              size_t cap);

#endif
