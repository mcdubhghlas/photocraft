/* Text analysis for the paragraph builder (parley 0.11 analysis::analyze_text): break
 * opportunities from libunibreak (UAX #14), character properties from parley_data's table, bidi
 * levels from FriBidi with parley's fix-ups; see playout.h. */
#include "text/playout.h"

#include <fribidi.h>
#include <graphemebreak.h>
#include <limits.h>
#include <linebreak.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/utf8.h"
#include "text/uprops.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

/* Decodes one char of known-valid UTF-8 (anything else reads as U+FFFD, one byte). */
static uint32_t next_cp(const char *s, size_t len, size_t *i)
{
    uint32_t cp;
    if (!pc_utf8_next((const uint8_t *)s, len, i, &cp)) {
        (*i)++;
        return 0xFFFD;
    }
    return cp;
}

/* ------------------------------------------------------------------------------------------ */
/* Text analysis (parley analysis::analyze_text) */

static pthread_once_t ub_once = PTHREAD_ONCE_INIT;

static void ub_once_init(void)
{
    init_linebreak();
    init_graphemebreak();
}

void pl_unibreak_init(void)
{
    (void)pthread_once(&ub_once, ub_once_init);
}

static bool needs_bidi(unsigned bc)
{
    switch (bc) {
    case PC_BC_LRE:
    case PC_BC_LRO:
    case PC_BC_RLE:
    case PC_BC_RLO:
    case PC_BC_FSI:
    case PC_BC_LRI:
    case PC_BC_RLI:
    case PC_BC_R:
    case PC_BC_AL:
    case PC_BC_AN:
        return true;
    default:
        return false;
    }
}

static bool removed_by_x9(unsigned bc)
{
    return bc == PC_BC_LRE || bc == PC_BC_RLE || bc == PC_BC_LRO || bc == PC_BC_RLO || bc == PC_BC_PDF ||
           bc == PC_BC_BN;
}

static bool isolate_initiator(unsigned bc)
{
    return bc == PC_BC_LRI || bc == PC_BC_RLI || bc == PC_BC_FSI;
}

static FriBidiCharType fribidi_type(unsigned bc)
{
    static const FriBidiCharType map[] = {
        FRIBIDI_TYPE_LTR, FRIBIDI_TYPE_RTL, FRIBIDI_TYPE_EN,  FRIBIDI_TYPE_ES, FRIBIDI_TYPE_ET,
        FRIBIDI_TYPE_AN,  FRIBIDI_TYPE_CS,  FRIBIDI_TYPE_BS,  FRIBIDI_TYPE_SS, FRIBIDI_TYPE_WS,
        FRIBIDI_TYPE_ON,  FRIBIDI_TYPE_LRE, FRIBIDI_TYPE_LRO, FRIBIDI_TYPE_AL, FRIBIDI_TYPE_RLE,
        FRIBIDI_TYPE_RLO, FRIBIDI_TYPE_PDF, FRIBIDI_TYPE_NSM, FRIBIDI_TYPE_BN, FRIBIDI_TYPE_FSI,
        FRIBIDI_TYPE_LRI, FRIBIDI_TYPE_RLI, FRIBIDI_TYPE_PDI};
    return bc < sizeof map / sizeof map[0] ? map[bc] : FRIBIDI_TYPE_ON;
}

/* BidiResolver::default_level: the first strong type outside isolates. */
static uint8_t default_level(const pl_info *info, size_t n)
{
    size_t i, isolates = 0;
    for (i = 0; i < n; i++) {
        unsigned t = pc_uprops_bidi(info[i].props);
        if (isolate_initiator(t)) {
            isolates++;
        } else if (t == PC_BC_PDI && isolates > 0) {
            isolates--;
        } else if ((t == PC_BC_L || t == PC_BC_R || t == PC_BC_AL) && isolates == 0) {
            return t == PC_BC_L ? 0 : 1;
        }
    }
    return 0;
}

/* Levels from FriBidi, then parley's own fix-ups after resolution: segment and paragraph
 * separators (and the whitespace before them) take the paragraph level, characters removed by
 * rule X9 that of the character before them. Trailing whitespace is handed to FriBidi as a
 * neutral so it keeps its resolved level: parley does not apply rule L1 at the end of the
 * paragraph. */
static pc_status resolve_bidi(pl_info *info, size_t n, uint8_t *base_out, pc_error *err)
{
    FriBidiCharType *types = NULL;
    FriBidiBracketType *brackets = NULL;
    FriBidiChar *chars = NULL;
    FriBidiLevel *levels = NULL;
    FriBidiParType dir;
    uint8_t base = default_level(info, n);
    size_t i;
    pc_status st = PC_OK;
    if (n > (size_t)INT_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "text layout: paragraph too long");
    }
    types = pc_malloc_array(n, sizeof *types);
    brackets = pc_malloc_array(n, sizeof *brackets);
    chars = pc_malloc_array(n, sizeof *chars);
    levels = pc_malloc_array(n, sizeof *levels);
    if (types == NULL || brackets == NULL || chars == NULL || levels == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        types[i] = fribidi_type(pc_uprops_bidi(info[i].props));
        chars[i] = info[i].cp;
    }
    for (i = n; i > 0; i--) {
        unsigned t = pc_uprops_bidi(info[i - 1].props);
        if (t == PC_BC_WS) {
            types[i - 1] = FRIBIDI_TYPE_ON;
        } else if (!removed_by_x9(t)) {
            break;
        }
    }
    fribidi_get_bracket_types(chars, (FriBidiStrIndex)n, types, brackets);
    dir = base != 0 ? FRIBIDI_PAR_RTL : FRIBIDI_PAR_LTR;
    if (fribidi_get_par_embedding_levels_ex(types, brackets, (FriBidiStrIndex)n, &dir, levels) == 0) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        unsigned t = pc_uprops_bidi(info[i].props);
        uint8_t lv = levels[i] < 0 ? 0 : (uint8_t)levels[i];
        if (t == PC_BC_S || t == PC_BC_B) {
            size_t j;
            lv = base;
            for (j = i; j > 0; j--) {
                unsigned u = pc_uprops_bidi(info[j - 1].props);
                if (removed_by_x9(u)) {
                    continue;
                }
                if (u == PC_BC_WS || isolate_initiator(u) || u == PC_BC_PDI) {
                    info[j - 1].level = base;
                } else {
                    break;
                }
            }
        } else if (removed_by_x9(t)) {
            lv = i == 0 ? base : info[i - 1].level;
        }
        info[i].level = lv;
    }
    *base_out = base;
cleanup:
    free(types);
    free(brackets);
    free(chars);
    free(levels);
    return st;
}

/* Character infos for text (a single space for empty text): line boundaries from UAX #14
 * (libunibreak; the reference's ICU segmenter also used dictionaries for some scripts), the
 * composite properties and the bidi levels. Word boundaries are not computed: the layouter only
 * uses line and mandatory boundaries. */
pc_status pl_analyze_text(const char *text, size_t len, pl_info **out, size_t *nout, uint8_t *base,
                          pc_error *err)
{
    char *brks = NULL;
    pl_info *info = NULL;
    size_t n = 0, i = 0;
    bool mandatory = false, bidi = false;
    if (len == 0) {
        text = " ";
        len = 1;
    }
    pl_unibreak_init();
    brks = malloc(len);
    info = pc_malloc_array(len, sizeof *info);
    if (brks == NULL || info == NULL) {
        free(brks);
        free(info);
        return NOMEM(err);
    }
    set_linebreaks_utf8((const utf8_t *)text, len, NULL, brks);
    while (i < len) {
        size_t at = i;
        uint32_t cp = next_cp(text, len, &i);
        uint32_t p = pc_uprops_get(cp);
        uint8_t boundary = PL_BOUNDARY_NONE;
        if (at > 0 && (brks[at - 1] == LINEBREAK_MUSTBREAK || brks[at - 1] == LINEBREAK_ALLOWBREAK)) {
            boundary = PL_BOUNDARY_LINE;
        }
        if (mandatory) {
            boundary = PL_BOUNDARY_MANDATORY;
        }
        mandatory = pc_uprops_is_mandatory_linebreak(p);
        bidi = bidi || needs_bidi(pc_uprops_bidi(p));
        info[n++] = (pl_info){cp, p, boundary, 0, 0};
    }
    free(brks);
    *base = 0;
    if (bidi) {
        pc_status st = resolve_bidi(info, n, base, err);
        if (st != PC_OK) {
            free(info);
            return st;
        }
    }
    *out = info;
    *nout = n;
    return PC_OK;
}

bool pl_force_normalize(uint32_t cp, uint32_t p)
{
    unsigned gcb = pc_uprops_gcb(p);
    if (gcb == PC_GCB_EXTEND && cp != 0x200C && !pc_uprops_is_variation_selector(p)) {
        return true;
    }
    return gcb == PC_GCB_SPACING_MARK;
}
