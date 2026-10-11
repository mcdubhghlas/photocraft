/* CSS font matching and synthesis (port of fontique 0.11 matching.rs and FontInfo::synthesis)
 * and the PostScript-name heuristic of crates/text/src/fonts.rs. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/fonts_internal.h"
#include "text/unicode.h"

#define OBLIQUE_THRESHOLD 14.0f

typedef struct style {
    pc_font_slope slope;
    bool has_angle;
    float angle;
} style;

typedef struct cand {
    size_t index;
    int32_t width;
    style st;
    float weight;
    bool has_slnt;
} cand;

static bool style_eq(style a, style b)
{
    if (a.slope != b.slope) {
        return false;
    }
    if (a.slope != PC_SLOPE_OBLIQUE) {
        return true;
    }
    return a.has_angle == b.has_angle && (!a.has_angle || a.angle == b.angle);
}

static float oblique_angle(style s)
{
    return s.has_angle ? s.angle : OBLIQUE_THRESHOLD;
}

/* Iterator::min_by / max_by over oblique candidates passing `pred` (first minimum, last
 * maximum; partial_cmp with NaN treated as Less as in the reference). */
typedef bool (*angle_pred)(float a, float ref);

static bool p_ge(float a, float r)
{
    return a >= r;
}
static bool p_pos_below(float a, float r)
{
    return a > 0.0f && a < r;
}
static bool p_neg(float a, float r)
{
    (void)r;
    return a < 0.0f;
}
static bool p_le(float a, float r)
{
    return a <= r;
}
static bool p_nonpos(float a, float r)
{
    (void)r;
    return a <= 0.0f;
}
static bool p_nonneg(float a, float r)
{
    (void)r;
    return a >= 0.0f;
}
static bool p_neg_above(float a, float r)
{
    return a < 0.0f && a > r;
}

static bool pick(const cand *set, size_t n, angle_pred pred, float ref, bool want_max, style *out)
{
    size_t i;
    bool have = false;
    float best = 0.0f;
    style bs = {PC_SLOPE_NORMAL, false, 0.0f};
    for (i = 0; i < n; i++) {
        float a;
        if (set[i].st.slope != PC_SLOPE_OBLIQUE) {
            continue;
        }
        a = oblique_angle(set[i].st);
        if (!pred(a, ref)) {
            continue;
        }
        if (!have) {
            have = true;
            best = a;
            bs = set[i].st;
        } else if (want_max ? !(best > a) : best > a) {
            /* cmp::max_by takes the new element unless the best compares Greater; min_by keeps
             * the best unless it compares Greater (partial_cmp of NaN counts as Less). */
            best = a;
            bs = set[i].st;
        }
    }
    if (have) {
        *out = bs;
    }
    return have;
}

static bool any_style(const cand *set, size_t n, pc_font_slope s)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (set[i].st.slope == s) {
            return true;
        }
    }
    return false;
}

static bool any_slnt(const cand *set, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (set[i].has_slnt) {
            return true;
        }
    }
    return false;
}

/* The no-match fallbacks shared by every oblique branch. */
static style oblique_fallback(const cand *set, size_t n, bool synthesize, angle_pred last_pred, bool last_max,
                              style use)
{
    style s;
    if (synthesize) {
        if (any_slnt(set, n)) {
            return use; /* use the slnt axis; the requested style stays */
        }
        if (any_style(set, n, PC_SLOPE_NORMAL)) {
            return (style){PC_SLOPE_NORMAL, false, 0.0f};
        }
        return set[0].st;
    }
    if (any_style(set, n, PC_SLOPE_ITALIC)) {
        return (style){PC_SLOPE_ITALIC, false, 0.0f};
    }
    if (pick(set, n, last_pred, 0.0f, last_max, &s)) {
        return s;
    }
    return set[0].st;
}

static style choose_style(const cand *set, size_t n, style want, bool synthesize)
{
    style s, use = want;
    size_t i;
    for (i = 0; i < n; i++) {
        if (style_eq(set[i].st, want)) {
            return want;
        }
    }
    if (want.slope == PC_SLOPE_ITALIC) {
        if (pick(set, n, p_ge, OBLIQUE_THRESHOLD, false, &s) ||
            pick(set, n, p_pos_below, OBLIQUE_THRESHOLD, true, &s) || pick(set, n, p_neg, 0.0f, true, &s)) {
            return s;
        }
        return set[0].st;
    }
    if (want.slope == PC_SLOPE_OBLIQUE) {
        float angle = oblique_angle(want);
        if (angle >= OBLIQUE_THRESHOLD) {
            if (pick(set, n, p_ge, angle, false, &s) || pick(set, n, p_pos_below, angle, true, &s)) {
                return s;
            }
            return oblique_fallback(set, n, synthesize, p_nonpos, true, use);
        }
        if (angle >= 0.0f) {
            if (pick(set, n, p_pos_below, angle, true, &s) || pick(set, n, p_ge, angle, false, &s)) {
                return s;
            }
            return oblique_fallback(set, n, synthesize, p_nonpos, true, use);
        }
        if (angle > -OBLIQUE_THRESHOLD) {
            if (pick(set, n, p_neg_above, angle, false, &s) || pick(set, n, p_le, angle, true, &s)) {
                return s;
            }
            return oblique_fallback(set, n, synthesize, p_nonneg, false, use);
        }
        if (pick(set, n, p_le, angle, true, &s) || pick(set, n, p_neg_above, angle, false, &s)) {
            return s;
        }
        return oblique_fallback(set, n, synthesize, p_nonneg, false, use);
    }
    /* Normal: oblique >= 0 ascending, then italic, then negative oblique descending. */
    if (pick(set, n, p_nonneg, 0.0f, false, &s)) {
        return s;
    }
    for (i = 0; i < n; i++) {
        if (set[i].st.slope == PC_SLOPE_ITALIC) {
            return set[i].st;
        }
    }
    if (pick(set, n, p_neg, 0.0f, true, &s)) {
        return s;
    }
    return set[0].st;
}

/* Weight selection over the remaining candidates: index into the family's faces. */
static long choose_weight(const cand *set, size_t n, float weight)
{
    size_t i;
    long best = -1;
    float bw = 0.0f;
    for (i = 0; i < n; i++) {
        if (set[i].weight == weight) {
            return (long)set[i].index;
        }
    }
#define SCAN(COND, MAX)                                                                                      \
    do {                                                                                                     \
        best = -1;                                                                                           \
        for (i = 0; i < n; i++) {                                                                            \
            float w = set[i].weight;                                                                         \
            if (!(COND)) {                                                                                   \
                continue;                                                                                    \
            }                                                                                                \
            if (best < 0 || ((MAX) ? !(bw > w) : bw > w)) {                                                  \
                best = (long)set[i].index;                                                                   \
                bw = w;                                                                                      \
            }                                                                                                \
        }                                                                                                    \
        if (best >= 0) {                                                                                     \
            return best;                                                                                     \
        }                                                                                                    \
    } while (0)
    if (weight >= 400.0f && weight <= 500.0f) {
        SCAN(w >= weight && w <= 500.0f, false);
        SCAN(w < weight, true);
        SCAN(w > 500.0f, false);
    } else if (weight < 400.0f) {
        SCAN(w <= weight, true);
        SCAN(w > weight, false);
    } else {
        SCAN(w >= weight, false);
        SCAN(w < weight, true);
    }
#undef SCAN
    return -1;
}

long pc_fontdb_match_index(const pc_fontdb_family *fam, const pc_font_query *q, bool synthesize_style)
{
    cand *set;
    size_t n = fam->nfaces, m = 0, i;
    int32_t width, use_width;
    bool exact = false, have = false;
    style want, use;
    long res;
    if (n == 0) {
        return -1;
    }
    if (n == 1) {
        return 0;
    }
    set = pc_malloc_array(n, sizeof *set);
    if (set == NULL) {
        return 0; /* out of memory: the first face is a valid (if imperfect) answer */
    }
    for (i = 0; i < n; i++) {
        const pc_fontdb_face *f = &fam->faces[i];
        set[i].index = i;
        set[i].width = pc_f32_to_i32(f->attrs.width * 100.0f);
        set[i].st = (style){f->attrs.slope, f->attrs.has_angle, f->attrs.angle};
        set[i].weight = f->attrs.weight;
        set[i].has_slnt = (f->axis_bits & PC_AXIS_SLNT) != 0;
    }
    width = pc_f32_to_i32(q->width * 100.0f);
    use_width = set[0].width;
    for (i = 0; i < n; i++) {
        exact = exact || set[i].width == width;
    }
    if (exact) {
        use_width = width;
    } else {
        int32_t below = INT32_MIN, above = INT32_MAX;
        bool has_below = false, has_above = false;
        for (i = 0; i < n; i++) {
            if (set[i].width < width && (!has_below || set[i].width >= below)) {
                below = set[i].width;
                has_below = true;
            }
            if (set[i].width > width && (!has_above || set[i].width < above)) {
                above = set[i].width;
                has_above = true;
            }
        }
        if (width <= 100) {
            use_width = has_below ? below : has_above ? above : use_width;
        } else {
            use_width = has_above ? above : has_below ? below : use_width;
        }
    }
    for (i = 0; i < n; i++) {
        if (set[i].width == use_width) {
            set[m++] = set[i];
        }
    }
    want = (style){q->slope, q->has_angle, q->angle};
    use = choose_style(set, m, want, synthesize_style);
    n = m;
    m = 0;
    for (i = 0; i < n; i++) {
        if (style_eq(set[i].st, use)) {
            set[m++] = set[i];
            have = true;
        }
    }
    res = have ? choose_weight(set, m, q->weight) : -1;
    free(set);
    return res;
}

static style face_style(const pc_fontdb_face *f)
{
    return (style){f->attrs.slope, f->attrs.has_angle, f->attrs.angle};
}

static void add_var(pc_font_match *m, const char *tag, float v)
{
    if (m->nvars < 3) {
        m->var_tags[m->nvars] = PC_SFNT_TAG(tag[0], tag[1], tag[2], tag[3]);
        m->var_values[m->nvars] = v;
        m->nvars++;
    }
}

/* FontInfo::synthesis for the matched face and the requested attributes. */
static void synthesis(const pc_fontdb_face *f, const pc_font_query *q, pc_font_match *m)
{
    style fs = face_style(f);
    m->nvars = 0;
    m->embolden = false;
    m->skew_deg = 0.0f;
    if ((f->axis_bits & PC_AXIS_WDTH) != 0 && f->attrs.width != q->width) {
        add_var(m, "wdth", q->width * 100.0f);
    }
    if (f->attrs.weight != q->weight) {
        if ((f->axis_bits & PC_AXIS_WGHT) != 0) {
            add_var(m, "wght", q->weight);
        } else if (q->weight > f->attrs.weight) {
            m->embolden = true;
        }
    }
    if (!style_eq(fs, (style){q->slope, q->has_angle, q->angle}) && fs.slope == PC_SLOPE_NORMAL) {
        if (q->slope == PC_SLOPE_ITALIC) {
            if ((f->axis_bits & PC_AXIS_ITAL) != 0) {
                add_var(m, "ital", 1.0f);
            } else if ((f->axis_bits & PC_AXIS_SLNT) != 0) {
                add_var(m, "slnt", 14.0f);
            } else {
                m->skew_deg = 14.0f;
            }
        } else if (q->slope == PC_SLOPE_OBLIQUE) {
            float deg = q->has_angle ? q->angle : 14.0f;
            if ((f->axis_bits & PC_AXIS_SLNT) != 0) {
                add_var(m, "slnt", deg);
            } else if ((f->axis_bits & PC_AXIS_ITAL) != 0 && deg > 0.0f) {
                add_var(m, "ital", 1.0f);
            } else {
                /* `degrees as i8`, saturating; zero means no skew. */
                double d = (double)deg;
                int v = isnan(d) ? 0 : d >= 127.0 ? 127 : d <= -128.0 ? -128 : (int)d;
                m->skew_deg = (float)v;
            }
        }
    }
}

void pc_fontdb_face_synthesis(const pc_fontdb_face *f, const pc_font_query *q, pc_font_match *m)
{
    synthesis(f, q, m);
}

pc_status pc_fontdb_match(pc_fontdb *db, const char *family, size_t len, const pc_font_query *q,
                          bool synthesize_style, pc_font_match *out, pc_error *err)
{
    const pc_fontdb_family *fam;
    long ix;
    pc_font *font;
    pc_font_match m;
    if (db == NULL || q == NULL || out == NULL || (family == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    fam = pc_fontdb_find_family(db, family != NULL ? family : "", len);
    if (fam == NULL || fam->nfaces == 0) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no font family \"%.*s\"", (int)(len > 100 ? 100 : len),
                       family != NULL ? family : "");
    }
    ix = pc_fontdb_match_index(fam, q, synthesize_style);
    if (ix < 0) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no matching face");
    }
    font = pc_fontdb_face_font(db, fam->name, fam->len, (size_t)ix, err);
    if (font == NULL) {
        return pc_fail(err, PC_ERR_FORMAT, "font family \"%s\": face %ld can't be loaded", fam->name, ix);
    }
    memset(&m, 0, sizeof m);
    m.font = pc_font_retain(font);
    m.family = fam->name;
    m.weight = fam->faces[ix].attrs.weight;
    m.slope = fam->faces[ix].attrs.slope;
    synthesis(&fam->faces[ix], q, &m);
    *out = m;
    return PC_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* PostScript names */

/* Removes every trailing repetition of `suffix` (str::trim_end_matches). */
static size_t trim_end_matches(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    while (n > 0 && len >= n && memcmp(s + len - n, suffix, n) == 0) {
        len -= n;
    }
    return len;
}

pc_status pc_text_guess_from_postscript(const char *ps, size_t len, pc_resolved_font *out, pc_error *err)
{
    const char *dash;
    size_t famlen, stlen, plen, i = 0, nch = 0, k, start, tl;
    const char *sty;
    const char *suffix = "";
    uint32_t *chars = NULL;
    pc_buf fam = PC_BUF_INIT, low = PC_BUF_INIT;
    uint16_t weight;
    pc_status st = PC_OK;
    if (out == NULL || (ps == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    if (ps == NULL) {
        ps = "";
    }
    dash = memchr(ps, '-', len);
    plen = dash != NULL ? (size_t)(dash - ps) : len;
    sty = dash != NULL ? dash + 1 : ps + len;
    stlen = dash != NULL ? len - plen - 1 : 0;
    famlen = trim_end_matches(ps, plen, "MT");
    famlen = trim_end_matches(ps, famlen, "PS");
    famlen = trim_end_matches(ps, famlen, "Std");
    famlen = trim_end_matches(ps, famlen, "Pro");
    if (pc_text_ends_with(ps, plen, "Pro")) {
        suffix = " Pro";
    } else if (pc_text_ends_with(ps, plen, "Std")) {
        suffix = " Std";
    }
    /* Split camel case: "TimesNewRoman" -> "Times New Roman". */
    chars = pc_malloc_array(famlen > 0 ? famlen : 1, sizeof *chars);
    if (chars == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    while (i < famlen) {
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)ps, famlen, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        chars[nch++] = cp;
    }
    for (k = 0; k < nch && st == PC_OK; k++) {
        uint32_t c = chars[k];
        if (k > 0 && pc_text_is_uppercase(c) &&
            (pc_text_is_lowercase(chars[k - 1]) ||
             (k + 1 < nch && pc_text_is_lowercase(chars[k + 1]) && pc_text_is_uppercase(chars[k - 1])))) {
            st = pc_buf_append_byte(&fam, ' ', err);
        }
        if (st == PC_OK) {
            st = pc_utf8_put(&fam, c, err);
        }
    }
    free(chars);
    if (st == PC_OK) {
        st = pc_buf_append_str(&fam, suffix, err);
    }
    if (st == PC_OK) {
        st = pc_text_lower(sty, stlen, &low, err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&fam);
        pc_buf_fini(&low);
        return st;
    }
    {
        const char *s = pc_buf_cstr(&low);
        size_t n = low.len;
#define HAS(w) pc_text_contains(s, n, (w))
        if (HAS("thin") || HAS("hairline")) {
            weight = 100;
        } else if (HAS("extralight") || HAS("ultralight")) {
            weight = 200;
        } else if (HAS("light")) {
            weight = 300;
        } else if (HAS("medium")) {
            weight = 500;
        } else if (HAS("semibold") || HAS("demibold") || HAS("demi")) {
            weight = 600;
        } else if (HAS("extrabold") || HAS("ultrabold") || HAS("heavy")) {
            weight = 800;
        } else if (HAS("black")) {
            weight = 900;
        } else if (HAS("bold")) {
            weight = 700;
        } else {
            weight = 400;
        }
        out->italic = HAS("italic") || pc_text_ends_with(s, n, "it") || HAS("oblique");
#undef HAS
    }
    pc_buf_fini(&low);
    tl = pc_utf8_trim(pc_buf_cstr(&fam), fam.len, &start);
    out->family = pc_strndup(pc_buf_cstr(&fam) + start, tl);
    pc_buf_fini(&fam);
    if (out->family == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    out->weight = weight;
    out->exact = false;
    return PC_OK;
}
