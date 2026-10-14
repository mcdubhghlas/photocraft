/* Font database (port of crates/text/src/fonts.rs over fontique's collection; see fonts.h). */
#include "text/fonts.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/file.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/bundled.h"
#include "text/craft_fonts.h"
#include "text/fonts_internal.h"
#include "text/served.h"
#include "text/unicode.h"

/* Largest font file read from disk; collections of CJK fonts are tens of MB. */
#define MAX_FONT_FILE ((size_t)1 << 30)

/* Bumped whenever font data is registered in any database (fonts.rs GENERATION). */
static atomic_uint_fast64_t generation;
/* fontique's FamilyId counter: every new family gets the next number. */
static atomic_uint_fast64_t family_ids = 1;

uint64_t pc_text_fonts_generation(void)
{
    return (uint64_t)atomic_load_explicit(&generation, memory_order_relaxed);
}

static const pc_bundled_font BUNDLED[] = {
    {"Inter-Regular.ttf", pc_font_Inter_Regular, 0},
    {"Inter-Medium.ttf", pc_font_Inter_Medium, 0},
    {"Inter-SemiBold.ttf", pc_font_Inter_SemiBold, 0},
    {"JetBrainsMono-Regular.ttf", pc_font_JetBrainsMono_Regular, 0},
};

/* The lengths are link-time constants, not constant expressions: fill the table once. */
static pc_bundled_font bundled_list[4];
static pthread_once_t bundled_once = PTHREAD_ONCE_INIT;

static void fill_bundled(void)
{
    memcpy(bundled_list, BUNDLED, sizeof bundled_list);
    bundled_list[0].len = pc_font_Inter_Regular_len;
    bundled_list[1].len = pc_font_Inter_Medium_len;
    bundled_list[2].len = pc_font_Inter_SemiBold_len;
    bundled_list[3].len = pc_font_JetBrainsMono_Regular_len;
}

const pc_bundled_font *pc_text_bundled_fonts(size_t *n)
{
    (void)pthread_once(&bundled_once, fill_bundled);
    if (n != NULL) {
        *n = 4;
    }
    return bundled_list;
}

void pc_strlist_fini(pc_strlist *l)
{
    size_t i;
    if (l == NULL) {
        return;
    }
    for (i = 0; i < l->n; i++) {
        free(l->v[i]);
    }
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

pc_status pc_strlist_push(pc_strlist *l, const char *s, size_t len, pc_error *err)
{
    char **g;
    char *d = pc_strndup(s, len);
    size_t cap = l->n;
    if (d == NULL || (g = pc_grow_array(l->v, &cap, l->n + 1, sizeof *g)) == NULL) {
        free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    l->v = g;
    l->v[l->n++] = d;
    return PC_OK;
}

void pc_resolved_font_fini(pc_resolved_font *r)
{
    if (r != NULL) {
        free(r->family);
        r->family = NULL;
    }
}

uint32_t pc_text_face_count(const uint8_t *data, size_t len)
{
    return pc_sfnt_face_count(data, len);
}

bool pc_text_is_hidden_family(const char *name, size_t len)
{
    return len > 0 && name[0] == '.';
}

/* ------------------------------------------------------------------------------------------ */
/* Families */

pc_status pc_fontdb_name_key(const char *s, size_t len, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        pc_status st;
        if (!pc_utf8_next((const uint8_t *)s, len, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        if (cp == 0x130) { /* char::to_lowercase: "i̇" */
            st = pc_utf8_put(out, 'i', err);
            if (st == PC_OK) {
                st = pc_utf8_put(out, 0x307, err);
            }
        } else {
            st = pc_utf8_put(out, pc_text_to_lower1(cp), err);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_fontdb_family *pc_fontdb_find_family(const pc_fontdb *db, const char *name, size_t len)
{
    pc_buf k = PC_BUF_INIT;
    size_t i;
    pc_fontdb_family *res = NULL;
    if (pc_fontdb_name_key(name, len, &k, NULL) != PC_OK) {
        pc_buf_fini(&k);
        return NULL;
    }
    for (i = 0; i < db->nfam && res == NULL; i++) {
        if (db->fam[i].klen == k.len && (k.len == 0 || memcmp(db->fam[i].key, k.data, k.len) == 0)) {
            res = &db->fam[i];
        }
    }
    pc_buf_fini(&k);
    return res;
}

bool pc_fontdb_has_family(const pc_fontdb *db, const char *name, size_t len)
{
    return db != NULL && name != NULL && pc_fontdb_find_family(db, name, len) != NULL;
}

static pc_status get_or_insert(pc_fontdb *db, const char *name, size_t len, size_t *idx, pc_error *err)
{
    pc_fontdb_family *f = pc_fontdb_find_family(db, name, len), *g;
    pc_buf k = PC_BUF_INIT;
    if (f != NULL) {
        *idx = (size_t)(f - db->fam);
        return PC_OK;
    }
    if (pc_fontdb_name_key(name, len, &k, err) != PC_OK) {
        pc_buf_fini(&k);
        return PC_ERR_NOMEM;
    }
    g = pc_grow_array(db->fam, &db->fam_cap, db->nfam + 1, sizeof *g);
    if (g == NULL) {
        pc_buf_fini(&k);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    db->fam = g;
    f = &db->fam[db->nfam];
    memset(f, 0, sizeof *f);
    f->id = (uint64_t)atomic_fetch_add_explicit(&family_ids, 1, memory_order_relaxed);
    f->name = pc_strndup(name, len);
    f->len = len;
    f->klen = k.len;
    f->key = (char *)pc_buf_take(&k, NULL);
    if (f->key == NULL) {
        f->key = pc_strndup("", 0);
    }
    if (f->name == NULL || f->key == NULL) {
        free(f->name);
        free(f->key);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *idx = db->nfam++;
    return PC_OK;
}

static unsigned axis_bits(const pc_font_axis *a, size_t n)
{
    unsigned bits = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (memcmp(a[i].tag, "wght", 4) == 0) {
            bits |= PC_AXIS_WGHT;
        } else if (memcmp(a[i].tag, "wdth", 4) == 0) {
            bits |= PC_AXIS_WDTH;
        } else if (memcmp(a[i].tag, "slnt", 4) == 0) {
            bits |= PC_AXIS_SLNT;
        } else if (memcmp(a[i].tag, "ital", 4) == 0) {
            bits |= PC_AXIS_ITAL;
        } else if (memcmp(a[i].tag, "opsz", 4) == 0) {
            bits |= PC_AXIS_OPSZ;
        }
    }
    return bits;
}

pc_status pc_fontdb_scan_source(pc_fontdb *db, size_t src, const uint8_t *data, size_t len,
                                const char *family_override, size_t **touched, size_t *ntouched,
                                pc_error *err)
{
    uint32_t nfaces = pc_sfnt_face_count(data, len), i;
    pc_buf name = PC_BUF_INIT;
    pc_status st = PC_OK;
    size_t tcap = touched != NULL ? *ntouched : 0;
    db->src[src].nfaces = nfaces;
    for (i = 0; i < nfaces && st == PC_OK; i++) {
        pc_sfnt f;
        pc_sfnt_cmap cm;
        pc_fontdb_face face;
        pc_fontdb_family *fam;
        size_t fi, naxes;
        if (!pc_sfnt_open(data, len, i, &f) || pc_sfnt_has_table(&f, PC_SFNT_TAG('h', 'v', 'g', 'l')) ||
            !pc_sfnt_has_name(&f)) {
            continue;
        }
        pc_buf_truncate(&name, 0);
        if (!pc_sfnt_name(&f, PC_SFNT_NAME_TYPOGRAPHIC_FAMILY, PC_SFNT_NAME_FONTIQUE, &name, NULL) &&
            !pc_sfnt_name(&f, PC_SFNT_NAME_FAMILY, PC_SFNT_NAME_FONTIQUE, &name, NULL)) {
            continue;
        }
        if (name.len == 0 || !pc_sfnt_cmap_fontique(&f, &cm)) {
            continue;
        }
        if (family_override != NULL) {
            pc_buf_truncate(&name, 0);
            if (pc_buf_append_str(&name, family_override, err) != PC_OK) {
                st = PC_ERR_NOMEM;
                break;
            }
        }
        memset(&face, 0, sizeof face);
        face.src = src;
        face.index = i;
        pc_sfnt_attrs(&f, &face.attrs);
        naxes = pc_sfnt_axes(&f, NULL, 0);
        if (naxes > 0) {
            face.axes = pc_malloc_array(naxes, sizeof *face.axes);
            if (face.axes == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            face.naxes = pc_sfnt_axes(&f, face.axes, naxes);
            face.axis_bits = axis_bits(face.axes, face.naxes);
        }
        st = get_or_insert(db, (const char *)name.data, name.len, &fi, err);
        if (st != PC_OK) {
            free(face.axes);
            break;
        }
        fam = &db->fam[fi];
        {
            pc_fontdb_face *g = pc_grow_array(fam->faces, &fam->cap, fam->nfaces + 1, sizeof *g);
            if (g == NULL) {
                free(face.axes);
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            fam->faces = g;
        }
        fam->faces[fam->nfaces++] = face;
        if (touched != NULL) {
            size_t k;
            bool seen = false;
            for (k = 0; k < *ntouched && !seen; k++) {
                seen = (*touched)[k] == fi;
            }
            if (!seen) {
                size_t *g = pc_grow_array(*touched, &tcap, *ntouched + 1, sizeof *g);
                if (g == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    break;
                }
                *touched = g;
                (*touched)[(*ntouched)++] = fi;
            }
        }
    }
    pc_buf_fini(&name);
    return st;
}

pc_status pc_fontdb_add_source(pc_fontdb *db, const char *path, pc_blob *bytes, size_t *idx, pc_error *err)
{
    pc_fontdb_source *g = pc_grow_array(db->src, &db->src_cap, db->nsrc + 1, sizeof *g);
    char *p = NULL;
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    db->src = g;
    if (path != NULL && (p = pc_strndup(path, strlen(path))) == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memset(&db->src[db->nsrc], 0, sizeof db->src[db->nsrc]);
    db->src[db->nsrc].path = p;
    db->src[db->nsrc].bytes = pc_blob_retain(bytes);
    *idx = db->nsrc++;
    return PC_OK;
}

/* Sort key order of register_font_data's names: (hidden, lowercase). */
static int names_cmp(const char *a, const char *b)
{
    bool ha = a[0] == '.', hb = b[0] == '.';
    if (ha != hb) {
        return ha ? 1 : -1;
    }
    return pc_text_cmp_lower(a, strlen(a), b, strlen(b));
}

static pc_status register_blob(pc_fontdb *db, pc_blob *blob, pc_strlist *names, pc_error *err)
{
    size_t src, *touched = NULL, nt = 0, i, k;
    pc_status st = pc_fontdb_add_source(db, NULL, blob, &src, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_fontdb_scan_source(db, src, pc_blob_data(blob), pc_blob_len(blob), NULL, &touched, &nt, err);
    if (st == PC_OK && names != NULL) {
        pc_strlist l = {NULL, 0};
        for (i = 0; i < nt && st == PC_OK; i++) {
            st = pc_strlist_push(&l, db->fam[touched[i]].name, db->fam[touched[i]].len, err);
        }
        /* Insertion sort: lists are tiny and the order must be stable. */
        for (i = 1; i < l.n && st == PC_OK; i++) {
            char *x = l.v[i];
            for (k = i; k > 0 && names_cmp(l.v[k - 1], x) > 0; k--) {
                l.v[k] = l.v[k - 1];
            }
            l.v[k] = x;
        }
        if (st == PC_OK) {
            *names = l;
        } else {
            pc_strlist_fini(&l);
        }
    }
    free(touched);
    if (st == PC_OK) {
        pc_fontdb_clear_ps_cache(db);
        pc_fontdb_refresh_generics(db);
        (void)atomic_fetch_add_explicit(&generation, 1, memory_order_relaxed);
    }
    return st;
}

void pc_fontdb_clear_ps_cache(pc_fontdb *db)
{
    size_t i;
    for (i = 0; i < db->nps; i++) {
        free(db->ps[i].ps);
        pc_resolved_font_fini(&db->ps[i].r);
    }
    db->nps = 0;
}

pc_status pc_fontdb_register_font_data(pc_fontdb *db, const uint8_t *data, size_t len, pc_strlist *names,
                                       pc_error *err)
{
    pc_blob *b;
    pc_status st;
    if (db == NULL || (data == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    if (names != NULL) {
        names->v = NULL;
        names->n = 0;
    }
    st = pc_blob_new(data, len, &b, err);
    if (st != PC_OK) {
        return st;
    }
    st = register_blob(db, b, names, err);
    pc_blob_release(b);
    return st;
}

/* ------------------------------------------------------------------------------------------ */
/* Fallbacks and generics */

static const char *const FALLBACK_CANDIDATES[] = {"Noto Sans",       "Segoe UI",     "DejaVu Sans",
                                                  "Geeza Pro",       "Arial Hebrew", "Noto Sans Arabic",
                                                  "Noto Sans Hebrew"};
const char *const PC_TEXT_THAI_FAMILIES[] = {
    "Leelawadee UI",         "Leelawadee",        "Tahoma", "Thonburi", "Sukhumvit Set", "Noto Sans Thai",
    "Noto Sans Thai Looped", "Noto Sans Thai UI", NULL};
const char *const PC_TEXT_KHMER_FAMILIES[] = {"Khmer Sangam MN", "Khmer MN", "Khmer UI", "Noto Sans Khmer",
                                              NULL};
const char *const PC_TEXT_LAO_FAMILIES[] = {"Lao Sangam MN",        "Lao MN", "Lao UI", "Noto Sans Lao",
                                            "Noto Sans Lao Looped", NULL};
const char *const PC_TEXT_MYANMAR_FAMILIES[] = {"Myanmar Sangam MN", "Myanmar MN", "Myanmar Text",
                                                "Noto Sans Myanmar", NULL};
static const char *const *const SEA_FAMILIES[] = {PC_TEXT_THAI_FAMILIES, PC_TEXT_KHMER_FAMILIES,
                                                  PC_TEXT_LAO_FAMILIES, PC_TEXT_MYANMAR_FAMILIES};
/* Indic-script families for document text (Nirmala UI covers all nine scripts and is listed by
   each; it is probed once). Matches crate::text::fonts INDIC_FAMILIES, in fallback order. */
static const char *const DEVANAGARI_FAMILIES[] = {"Nirmala UI",             "Kohinoor Devanagari",
                                                  "Devanagari Sangam MN",   "Noto Sans Devanagari",
                                                  "Noto Sans Devanagari UI", NULL};
static const char *const BENGALI_FAMILIES[] = {"Nirmala UI",           "Kohinoor Bangla",
                                               "Bangla Sangam MN",     "Noto Sans Bengali",
                                               "Noto Sans Bengali UI", NULL};
static const char *const GURMUKHI_FAMILIES[] = {"Nirmala UI", "Gurmukhi Sangam MN", "Noto Sans Gurmukhi",
                                                "Noto Sans Gurmukhi UI", NULL};
static const char *const GUJARATI_FAMILIES[] = {"Nirmala UI",            "Kohinoor Gujarati",
                                                "Gujarati Sangam MN",    "Noto Sans Gujarati",
                                                "Noto Sans Gujarati UI", NULL};
static const char *const TAMIL_FAMILIES[] = {"Nirmala UI", "Tamil Sangam MN", "Noto Sans Tamil",
                                             "Noto Sans Tamil UI", NULL};
static const char *const TELUGU_FAMILIES[] = {"Nirmala UI",          "Kohinoor Telugu",
                                              "Telugu Sangam MN",    "Noto Sans Telugu",
                                              "Noto Sans Telugu UI", NULL};
static const char *const KANNADA_FAMILIES[] = {"Nirmala UI", "Kannada Sangam MN", "Noto Sans Kannada",
                                               "Noto Sans Kannada UI", NULL};
static const char *const MALAYALAM_FAMILIES[] = {"Nirmala UI", "Malayalam Sangam MN", "Noto Sans Malayalam",
                                                 "Noto Sans Malayalam UI", NULL};
static const char *const SINHALA_FAMILIES[] = {"Nirmala UI", "Sinhala Sangam MN", "Noto Sans Sinhala",
                                               "Noto Sans Sinhala UI", NULL};
static const char *const *const INDIC_FAMILIES[] = {
    DEVANAGARI_FAMILIES, BENGALI_FAMILIES, GURMUKHI_FAMILIES, GUJARATI_FAMILIES, TAMIL_FAMILIES,
    TELUGU_FAMILIES,     KANNADA_FAMILIES, MALAYALAM_FAMILIES, SINHALA_FAMILIES};
static const char *const FALLBACK_LAST[] = {"Arial Unicode MS", "Apple Color Emoji", "Segoe UI Emoji",
                                            "Noto Color Emoji"};

pc_status pc_text_fallback_candidates(const pc_cjk_script order[4], const char ***out, size_t *n,
                                      pc_error *err)
{
    const char **craft = NULL, **v;
    size_t ncraft = 0, cap, m = 0, i, s;
    size_t ncf;
    const pc_craft_font *cf = pc_craft_fonts(&ncf);
    pc_status st = pc_craft_japanese_families(cf, ncf, &craft, &ncraft, err);
    if (st != PC_OK) {
        return st;
    }
    cap = 7 + 4 + ncraft + 4 * 8 + 8 + 4 + 5 + 4 + 40;
    v = pc_malloc_array(cap, sizeof *v);
    if (v == NULL) {
        free(craft);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < 7; i++) {
        v[m++] = FALLBACK_CANDIDATES[i];
    }
    /* The Southeast Asian scripts (Thai, Khmer, Lao, Myanmar) before the CJK families. */
    for (s = 0; s < 4; s++) {
        const char *const *f;
        for (f = SEA_FAMILIES[s]; *f != NULL && m < cap; f++) {
            v[m++] = *f;
        }
    }
    /* The nine Indic scripts, before the CJK families; Nirmala UI is listed by each, so a name
       already present is not pushed again (!v.contains(f)). */
    for (s = 0; s < sizeof INDIC_FAMILIES / sizeof INDIC_FAMILIES[0]; s++) {
        const char *const *f;
        for (f = INDIC_FAMILIES[s]; *f != NULL && m < cap; f++) {
            size_t j;
            bool seen = false;
            for (j = 0; j < m; j++) {
                if (strcmp(v[j], *f) == 0) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                v[m++] = *f;
            }
        }
    }
    for (s = 0; s < 4; s++) {
        const char *const *f;
        if (order[s] == PC_CJK_JAPANESE) {
            for (i = 0; i < ncraft; i++) {
                v[m++] = craft[i];
            }
        }
        for (f = pc_cjk_families(order[s]); *f != NULL && m < cap; f++) {
            v[m++] = *f;
        }
    }
    for (i = 0; i < 4 && m < cap; i++) {
        v[m++] = FALLBACK_LAST[i];
    }
    free(craft);
    *out = v;
    *n = m;
    return PC_OK;
}

void pc_fontdb_refresh_generics(pc_fontdb *db)
{
    pc_cjk_script order[4];
    const char **c = NULL, **g;
    size_t n = 0, i, m = 0, k;
    pc_strlist served = {NULL, 0}, owned = {NULL, 0};
    pc_cjk_ui_script_order(order);
    if (pc_text_fallback_candidates(order, &c, &n, NULL) != PC_OK) {
        return; /* out of memory: keep the previous list */
    }
    for (i = 0; i < n; i++) {
        if (pc_fontdb_has_family(db, c[i], strlen(c[i]))) {
            c[m++] = c[i];
        }
    }
    /* Served fonts that are a script's fallback (the manifest's `scripts`), once they arrived. */
    if (pc_text_served_script_fallback_families(&served, NULL) == PC_OK && served.n > 0) {
        g = realloc(c, (m + served.n) * sizeof *c);
        if (g != NULL) {
            c = g;
            for (i = 0; i < served.n; i++) {
                bool have = false;
                for (k = 0; k < m && !have; k++) {
                    have = strcmp(c[k], served.v[i]) == 0;
                }
                if (!have && pc_fontdb_has_family(db, served.v[i], strlen(served.v[i])) &&
                    pc_strlist_push(&owned, served.v[i], strlen(served.v[i]), NULL) == PC_OK) {
                    c[m++] = owned.v[owned.n - 1];
                }
            }
        }
    }
    pc_strlist_fini(&served);
    free(db->fallbacks);
    for (i = 0; i < db->nowned_fallbacks; i++) {
        free(db->owned_fallbacks[i]);
    }
    free(db->owned_fallbacks);
    db->fallbacks = c;
    db->nfallbacks = m;
    db->owned_fallbacks = owned.v;
    db->nowned_fallbacks = owned.n;
}

void pc_fontdb_set_missing_glyph_protection(pc_fontdb *db, bool on)
{
    if (db != NULL) {
        db->missing_glyph_protection = on;
    }
}

bool pc_fontdb_missing_glyph_protection(const pc_fontdb *db)
{
    return db != NULL && db->missing_glyph_protection;
}

size_t pc_fontdb_fallback_count(const pc_fontdb *db)
{
    /* With missing glyph protection off (Preferences > Type), only the default family shapes. */
    return db->missing_glyph_protection ? db->nfallbacks + 1 : 1;
}

const char *pc_fontdb_fallback(const pc_fontdb *db, size_t i)
{
    if (i == 0) {
        return PC_TEXT_DEFAULT_FAMILY;
    }
    return db->missing_glyph_protection && i - 1 < db->nfallbacks ? db->fallbacks[i - 1] : NULL;
}

const char *pc_fontdb_generic(const pc_fontdb *db, const char *g)
{
    const pc_fontdb_family *f = NULL;
    if (g == NULL) {
        return NULL;
    }
    if (strcmp(g, "sans-serif") == 0 || strcmp(g, "serif") == 0 || strcmp(g, "system-ui") == 0 ||
        strcmp(g, "ui-sans-serif") == 0) {
        f = pc_fontdb_find_family(db, PC_TEXT_DEFAULT_FAMILY, strlen(PC_TEXT_DEFAULT_FAMILY));
    } else if (strcmp(g, "monospace") == 0 || strcmp(g, "ui-monospace") == 0) {
        f = pc_fontdb_find_family(db, PC_TEXT_MONO_FAMILY, strlen(PC_TEXT_MONO_FAMILY));
    }
    return f != NULL ? f->name : NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Construction and system fonts */

pc_status pc_fontdb_new(pc_fontdb **out, pc_error *err)
{
    pc_fontdb *db = calloc(1, sizeof *db);
    const pc_bundled_font *b;
    const pc_craft_font *cf;
    size_t n, i;
    pc_status st = PC_OK;
    if (db == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    db->missing_glyph_protection = true;
    b = pc_text_bundled_fonts(&n);
    for (i = 0; i < n && st == PC_OK; i++) {
        st = pc_fontdb_register_font_data(db, b[i].data, b[i].len, NULL, err);
    }
    /* The optional craft fonts (Japanese only), before any system font; shared, not copied. */
    cf = pc_craft_fonts(&n);
    for (i = 0; i < n && st == PC_OK; i++) {
        if (pc_craft_font_is_japanese(&cf[i])) {
            st = register_blob(db, cf[i].bytes, NULL, err);
        }
    }
    if (st != PC_OK) {
        pc_fontdb_free(db);
        return st;
    }
    pc_fontdb_refresh_generics(db);
    *out = db;
    return PC_OK;
}

pc_status pc_fontdb_with_system_fonts(pc_fontdb **out, pc_error *err)
{
    pc_fontdb *db;
    pc_status st = pc_fontdb_new(&db, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_fontdb_load_system_fonts(db, err);
    if (st != PC_OK) {
        pc_fontdb_free(db);
        return st;
    }
    *out = db;
    return PC_OK;
}

void pc_fontdb_free(pc_fontdb *db)
{
    size_t i, k;
    if (db == NULL) {
        return;
    }
    for (i = 0; i < db->nsrc; i++) {
        pc_fontdb_source *s = &db->src[i];
        if (s->fonts != NULL) {
            for (k = 0; k < s->nfaces; k++) {
                pc_font_release(s->fonts[k]);
            }
        }
        free(s->fonts);
        free(s->path);
        pc_blob_release(s->bytes);
    }
    for (i = 0; i < db->nfam; i++) {
        for (k = 0; k < db->fam[i].nfaces; k++) {
            free(db->fam[i].faces[k].axes);
            free(db->fam[i].faces[k].style);
            free(db->fam[i].faces[k].ps);
        }
        free(db->fam[i].faces);
        free(db->fam[i].name);
        free(db->fam[i].key);
    }
    for (i = 0; i < db->nps; i++) {
        free(db->ps[i].ps);
        pc_resolved_font_fini(&db->ps[i].r);
    }
    for (i = 0; i < db->naliases; i++) {
        free(db->aliases[i].family);
        free(db->aliases[i].alias);
    }
    for (i = 0; i < db->nowned_fallbacks; i++) {
        free(db->owned_fallbacks[i]);
    }
    free(db->owned_fallbacks);
    free(db->aliases);
    free(db->ps);
    free(db->src);
    free(db->fam);
    free(db->fallbacks);
    free(db);
}
