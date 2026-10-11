/* Pure helpers of the layouter (layout.rs): vertical classes, paragraph splitting, the
 * Japanese punctuation squeeze, synthetic bold and small caps rules. */
#include "text/layout.h"

#include <math.h>
#include <stdlib.h>

#include "base/mem.h"
#include "base/utf8.h"
#include "text/cjk.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

pc_vclass pc_text_vertical_class(uint32_t u)
{
    bool alt = (u >= 0x2014 && u <= 0x2016) || u == 0x2025 || u == 0x2026 || u == 0x2329 || u == 0x232A ||
               (u >= 0x3008 && u <= 0x3011) || (u >= 0x3014 && u <= 0x301F) || u == 0x3030 || u == 0x30A0 ||
               u == 0x30FC || (u >= 0xFE59 && u <= 0xFE5E) || u == 0xFF08 || u == 0xFF09 || u == 0xFF0D ||
               (u >= 0xFF1A && u <= 0xFF1E) || u == 0xFF3B || u == 0xFF3D || u == 0xFF3F ||
               (u >= 0xFF5B && u <= 0xFF60) || u == 0xFFE3;
    bool upright;
    if (alt) {
        return PC_VCLASS_ALTERNATE_OR_ROTATE;
    }
    if (u >= 0xFF61 && u <= 0xFFDC) {
        return PC_VCLASS_ROTATE;
    }
    upright = pc_cjk_classify(u) != PC_CJK_NONE || u == 0xA7 || u == 0xA9 || u == 0xAE || u == 0xB1 ||
              (u >= 0xBC && u <= 0xBE) || u == 0xD7 || u == 0xF7 || (u >= 0x2E80 && u <= 0x2EFF) ||
              (u >= 0xFE10 && u <= 0xFE1F) || (u >= 0xFE30 && u <= 0xFE4F) || (u >= 0x1F000 && u <= 0x1FAFF);
    return upright ? PC_VCLASS_UPRIGHT : PC_VCLASS_ROTATE;
}

bool pc_text_synthetic_bold(bool matcher_says, uint16_t requested, float face_weight)
{
    return matcher_says && requested >= 600 && face_weight <= 500.0f;
}

pc_small_caps_mode pc_text_small_caps_mode(pc_caps caps, bool face_has_smcp)
{
    if (caps == PC_CAPS_SMALL_CAPS) {
        return face_has_smcp ? PC_SMALL_CAPS_OPENTYPE : PC_SMALL_CAPS_SYNTHETIC;
    }
    return PC_SMALL_CAPS_NONE;
}

pc_status pc_text_split_paragraphs(const char *text, size_t len, size_t **out, size_t *n, pc_error *err)
{
    size_t cap = 0, cnt = 0, start = 0, i = 0;
    size_t *v = NULL;
#define PUSH(a, z)                                                                                           \
    do {                                                                                                     \
        if (2 * cnt + 2 > cap) {                                                                             \
            void *g_ = pc_grow_array(v, &cap, 2 * cnt + 2, sizeof *v);                                       \
            if (g_ == NULL) {                                                                                \
                free(v);                                                                                     \
                return NOMEM(err);                                                                           \
            }                                                                                                \
            v = g_;                                                                                          \
        }                                                                                                    \
        v[2 * cnt] = (a);                                                                                    \
        v[2 * cnt + 1] = (z);                                                                                \
        cnt++;                                                                                               \
    } while (0)
    while (i < len) {
        if (text[i] == '\r') {
            size_t end = i + 1 < len && text[i + 1] == '\n' ? i + 2 : i + 1;
            PUSH(start, end);
            start = end;
            i = end;
        } else if (text[i] == '\n') {
            PUSH(start, i + 1);
            start = i + 1;
            i++;
        } else {
            i++;
        }
    }
    PUSH(start, len);
#undef PUSH
    *out = v;
    *n = cnt;
    return PC_OK;
}

static bool is_closing(uint32_t c)
{
    switch (c) {
    case 0x3001:
    case 0x3002:
    case 0xFF0C:
    case 0xFF0E:
    case 0x300D:
    case 0x300F:
    case 0xFF09:
    case 0x3015:
    case 0x3011:
    case 0x3009:
    case 0x300B:
    case 0x3019:
    case 0x3017:
    case 0xFF3D:
    case 0xFF5D:
        return true;
    default:
        return false;
    }
}

static bool is_opening(uint32_t c)
{
    switch (c) {
    case 0x300C:
    case 0x300E:
    case 0xFF08:
    case 0x3014:
    case 0x3010:
    case 0x3008:
    case 0x300A:
    case 0x3018:
    case 0x3016:
    case 0xFF3B:
    case 0xFF5B:
        return true;
    default:
        return false;
    }
}

/* First and last char of text[a..z) when that is a valid, non-empty char range. */
static bool range_chars(const char *text, size_t len, size_t a, size_t z, uint32_t *first, uint32_t *last)
{
    size_t i = a;
    uint32_t cp;
    if (a > z || z > len || a == z) {
        return false;
    }
    /* str::get: both ends on char boundaries. */
    if ((a < len && ((unsigned char)text[a] & 0xC0) == 0x80) ||
        (z < len && ((unsigned char)text[z] & 0xC0) == 0x80)) {
        return false;
    }
    if (!pc_utf8_next((const uint8_t *)text, z, &i, &cp)) {
        return false;
    }
    *first = cp;
    *last = cp;
    while (i < z) {
        if (!pc_utf8_next((const uint8_t *)text, z, &i, &cp)) {
            return false;
        }
        *last = cp;
    }
    return true;
}

float pc_text_squeeze_punctuation(const char *text, size_t len, pc_cluster_info *clusters, size_t nclusters,
                                  pc_placed_glyph *glyphs, size_t nglyphs)
{
    float *shifts, *starts, total = 0.0f;
    size_t i;
    if (nclusters == 0) {
        return 0.0f;
    }
    shifts = calloc(nclusters, sizeof *shifts);
    starts = pc_malloc_array(nclusters, sizeof *starts);
    if (shifts == NULL || starts == NULL) {
        free(shifts);
        free(starts);
        return 0.0f; /* out of memory: no squeeze */
    }
    for (i = 1; i < nclusters; i++) {
        const pc_cluster_info *prev = &clusters[i - 1], *cur = &clusters[i];
        uint32_t af, a, b, bl;
        float cut = 0.0f;
        if (prev->rtl || cur->rtl) {
            continue;
        }
        if (!range_chars(text, len, prev->start, prev->end, &af, &a) ||
            !range_chars(text, len, cur->start, cur->end, &b, &bl)) {
            continue;
        }
        if (is_closing(a) && (is_opening(b) || is_closing(b))) {
            cut = prev->advance * 0.5f;
        } else if (is_opening(a) && is_opening(b)) {
            cut = cur->advance * 0.5f;
        }
        if (isfinite(cut) && cut > 0.0f) {
            total += cut;
        }
        shifts[i] = total;
    }
    if (total <= 0.0f) {
        free(shifts);
        free(starts);
        return 0.0f;
    }
    for (i = 0; i < nclusters; i++) {
        starts[i] = clusters[i].x;
        clusters[i].x -= shifts[i];
    }
    for (i = 0; i < nglyphs; i++) {
        size_t j = nclusters, at = 0;
        while (j > 0) {
            if (starts[j - 1] <= glyphs[i].x + 1e-3f) {
                at = j - 1;
                break;
            }
            j--;
        }
        glyphs[i].x -= shifts[at];
    }
    free(shifts);
    free(starts);
    return total;
}
