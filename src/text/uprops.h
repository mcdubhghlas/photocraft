/* Per-code-point Unicode properties as parley 0.11's text analysis sees them (parley_data 0.11.1's
 * composite table: script, general category, grapheme cluster break, bidi class and four flags),
 * so the layouter classifies characters exactly as the reference did. Values use ICU4C's
 * numbering (UScriptCode, UCharCategory, UGraphemeClusterBreak, UCharDirection).
 *
 * Private to src/text. Pure functions; thread-safe. */
#ifndef PC_TEXT_UPROPS_H
#define PC_TEXT_UPROPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The packed properties of cp (0 for values above U+10FFFF). */
uint32_t pc_uprops_get(uint32_t cp);
/* ISO 15924 tag of an ICU script code ("Zzzz" for unknown codes); static string. */
const char *pc_uprops_script_tag(uint16_t script);

static inline uint16_t pc_uprops_script(uint32_t p)
{
    return (uint16_t)(p & 0xFFu);
}
static inline unsigned pc_uprops_gc(uint32_t p)
{
    return (p >> 8) & 31u;
}
static inline unsigned pc_uprops_gcb(uint32_t p)
{
    return (p >> 13) & 31u;
}
static inline unsigned pc_uprops_bidi(uint32_t p)
{
    return (p >> 18) & 31u;
}
static inline bool pc_uprops_is_emoji_or_pictograph(uint32_t p)
{
    return ((p >> 23) & 1u) != 0;
}
static inline bool pc_uprops_is_variation_selector(uint32_t p)
{
    return ((p >> 24) & 1u) != 0;
}
static inline bool pc_uprops_is_region_indicator(uint32_t p)
{
    return ((p >> 25) & 1u) != 0;
}
static inline bool pc_uprops_is_mandatory_linebreak(uint32_t p)
{
    return ((p >> 26) & 1u) != 0;
}

/* Scripts (UScriptCode). */
enum { PC_SC_COMMON = 0, PC_SC_INHERITED = 1, PC_SC_LATIN = 25, PC_SC_UNKNOWN = 103 };

/* General categories (UCharCategory). */
enum {
    PC_GC_CN = 0,
    PC_GC_LU,
    PC_GC_LL,
    PC_GC_LT,
    PC_GC_LM,
    PC_GC_LO,
    PC_GC_MN,
    PC_GC_ME,
    PC_GC_MC,
    PC_GC_ND,
    PC_GC_NL,
    PC_GC_NO,
    PC_GC_ZS,
    PC_GC_ZL,
    PC_GC_ZP,
    PC_GC_CC,
    PC_GC_CF,
    PC_GC_CO,
    PC_GC_CS,
    PC_GC_PD,
    PC_GC_PS,
    PC_GC_PE,
    PC_GC_PC,
    PC_GC_PO,
    PC_GC_SM,
    PC_GC_SC,
    PC_GC_SK,
    PC_GC_SO,
    PC_GC_PI,
    PC_GC_PF
};

/* Grapheme cluster break values used by the analysis. */
enum { PC_GCB_EXTEND = 3, PC_GCB_SPACING_MARK = 10 };

/* Bidi classes (UCharDirection). */
enum {
    PC_BC_L = 0,
    PC_BC_R,
    PC_BC_EN,
    PC_BC_ES,
    PC_BC_ET,
    PC_BC_AN,
    PC_BC_CS,
    PC_BC_B,
    PC_BC_S,
    PC_BC_WS,
    PC_BC_ON,
    PC_BC_LRE,
    PC_BC_LRO,
    PC_BC_AL,
    PC_BC_RLE,
    PC_BC_RLO,
    PC_BC_PDF,
    PC_BC_NSM,
    PC_BC_BN,
    PC_BC_FSI,
    PC_BC_LRI,
    PC_BC_RLI,
    PC_BC_PDI
};

/* parley's contributes_to_shaping(general_category, script). */
static inline bool pc_uprops_contributes_to_shaping(uint32_t p)
{
    unsigned gc = pc_uprops_gc(p);
    if (gc == PC_GC_CC || gc == PC_GC_ZL || gc == PC_GC_ZP) {
        return false;
    }
    return !(gc == PC_GC_CF && pc_uprops_script(p) != PC_SC_INHERITED);
}

#endif
