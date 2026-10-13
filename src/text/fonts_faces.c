/* Font database queries (port of crates/text/src/fonts.rs and glyphs.rs over fontique's
 * collection): family lists, faces and their names, exact named faces and their aliases,
 * localized family names, character maps, PostScript-name resolution. See fonts.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/file.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/fonts_internal.h"
#include "text/unicode.h"

/* Largest font file read from disk; collections of CJK fonts are tens of MB. */
#define MAX_FONT_FILE ((size_t)1 << 30)

/* ------------------------------------------------------------------------------------------ */
/* Queries */

typedef struct name_ix {
    const char *name;
    pc_buf lower;
    size_t ix;
} name_ix;

static int cmp_name_ix(const void *a, const void *b)
{
    const name_ix *x = a, *y = b;
    size_t n = x->lower.len < y->lower.len ? x->lower.len : y->lower.len;
    int r = n > 0 ? memcmp(x->lower.data, y->lower.data, n) : 0;
    if (r == 0) {
        r = x->lower.len < y->lower.len ? -1 : x->lower.len > y->lower.len;
    }
    if (r == 0) {
        r = x->ix < y->ix ? -1 : x->ix > y->ix; /* stable */
    }
    return r;
}

pc_status pc_fontdb_families(pc_fontdb *db, pc_strlist *out, pc_error *err)
{
    name_ix *v;
    size_t i, m = 0;
    pc_strlist l = {NULL, 0};
    pc_status st = PC_OK;
    if (db == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    v = pc_calloc_array(db->nfam > 0 ? db->nfam : 1, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < db->nfam && st == PC_OK; i++) {
        if (pc_text_is_hidden_family(db->fam[i].name, db->fam[i].len)) {
            continue;
        }
        v[m].name = db->fam[i].name;
        v[m].ix = m;
        st = pc_text_lower(db->fam[i].name, db->fam[i].len, &v[m].lower, err);
        m++;
    }
    if (st == PC_OK) {
        qsort(v, m, sizeof *v, cmp_name_ix);
        for (i = 0; i < m && st == PC_OK; i++) {
            /* dedup: consecutive equal names */
            if (i > 0 && strcmp(v[i].name, v[i - 1].name) == 0) {
                continue;
            }
            st = pc_strlist_push(&l, v[i].name, strlen(v[i].name), err);
        }
    }
    for (i = 0; i < m; i++) {
        pc_buf_fini(&v[i].lower);
    }
    free(v);
    if (st != PC_OK) {
        pc_strlist_fini(&l);
        return st;
    }
    *out = l;
    return PC_OK;
}

void pc_face_infos_free(pc_face_info *f, size_t n)
{
    size_t i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(f[i].family);
        free(f[i].style);
        free(f[i].postscript_name);
        free(f[i].axes);
    }
    free(f);
}

pc_status pc_fontdb_faces(pc_fontdb *db, const char *family, size_t len, pc_face_info **out, size_t *n,
                          pc_error *err)
{
    pc_fontdb_family *fam;
    pc_face_info *v;
    size_t i;
    if (db == NULL || out == NULL || n == NULL || (family == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    fam = pc_fontdb_find_family(db, family != NULL ? family : "", len);
    if (fam == NULL || fam->nfaces == 0) {
        *out = NULL;
        *n = 0;
        return PC_OK;
    }
    v = pc_calloc_array(fam->nfaces, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < fam->nfaces; i++) {
        pc_fontdb_face *f = &fam->faces[i];
        char sb[32];
        pc_fontdb_face_names(db, f);
        v[i].family = pc_strndup(fam->name, fam->len);
        v[i].style = f->style != NULL
                         ? pc_strndup(f->style, strlen(f->style))
                         : pc_strndup(sb, strlen(pc_text_fallback_style(
                                              f->attrs.weight, f->attrs.slope != PC_SLOPE_NORMAL, sb)));
        v[i].postscript_name = f->ps != NULL ? pc_strndup(f->ps, strlen(f->ps)) : NULL;
        v[i].weight = f->attrs.weight;
        v[i].italic = f->attrs.slope != PC_SLOPE_NORMAL;
        v[i].naxes = f->naxes;
        if (f->naxes > 0) {
            v[i].axes = pc_malloc_array(f->naxes, sizeof *v[i].axes);
            if (v[i].axes != NULL) {
                memcpy(v[i].axes, f->axes, f->naxes * sizeof *f->axes);
            }
        }
        if (v[i].family == NULL || v[i].style == NULL || (f->ps != NULL && v[i].postscript_name == NULL) ||
            (f->naxes > 0 && v[i].axes == NULL)) {
            pc_face_infos_free(v, i + 1);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    *out = v;
    *n = fam->nfaces;
    return PC_OK;
}

pc_font *pc_fontdb_load_face(pc_fontdb *db, const pc_fontdb_face *face, pc_error *err)
{
    pc_fontdb_source *s = &db->src[face->src];
    if (s->failed || face->index >= s->nfaces) {
        return NULL;
    }
    if (s->bytes == NULL) {
        pc_buf b = PC_BUF_INIT;
        pc_status st = pc_file_read(s->path, MAX_FONT_FILE, &b, err);
        if (st == PC_OK) {
            st = pc_blob_new(b.data, b.len, &s->bytes, err);
        }
        pc_buf_fini(&b);
        if (st != PC_OK) {
            s->failed = st != PC_ERR_NOMEM;
            return NULL;
        }
    }
    if (s->fonts == NULL) {
        s->fonts = pc_calloc_array(s->nfaces, sizeof *s->fonts);
        if (s->fonts == NULL) {
            (void)pc_fail(err, PC_ERR_NOMEM, "out of memory");
            return NULL;
        }
    }
    if (s->fonts[face->index] == NULL &&
        pc_font_new(s->bytes, face->index, &s->fonts[face->index], err) != PC_OK) {
        return NULL;
    }
    return s->fonts[face->index];
}

pc_font *pc_fontdb_face_font(pc_fontdb *db, const char *family, size_t len, size_t i, pc_error *err)
{
    const pc_fontdb_family *fam;
    if (db == NULL || family == NULL) {
        return NULL;
    }
    fam = pc_fontdb_find_family(db, family, len);
    if (fam == NULL || i >= fam->nfaces) {
        return NULL;
    }
    return pc_fontdb_load_face(db, &fam->faces[i], err);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct cm_acc {
    uint32_t *v;
    size_t n, cap;
    bool nomem;
} cm_acc;

static bool cm_push(void *ctx, uint32_t cp, uint32_t gid)
{
    cm_acc *a = ctx;
    (void)gid;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return true; /* char::from_u32 fails */
    }
    if (cp != ' ' && (pc_text_is_control(cp) || pc_unicode_is_space(cp) || (cp >= 0xFE00 && cp <= 0xFE0F))) {
        return true;
    }
    if (a->n == a->cap) {
        uint32_t *g = pc_grow_array(a->v, &a->cap, a->n + 1, sizeof *g);
        if (g == NULL) {
            a->nomem = true;
            return false;
        }
        a->v = g;
    }
    a->v[a->n++] = cp;
    return true;
}

pc_status pc_fontdb_charmap(pc_fontdb *db, const char *family, size_t len, uint16_t weight, bool italic,
                            uint32_t **out, size_t *n, pc_error *err)
{
    const pc_fontdb_family *fam;
    const pc_fontdb_face *best = NULL;
    uint32_t best_key = 0;
    size_t i, m = 0;
    pc_font *font;
    pc_sfnt sf;
    pc_sfnt_cmap cm;
    cm_acc acc = {NULL, 0, 0, false};
    if (db == NULL || out == NULL || n == NULL || (family == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    *out = NULL;
    *n = 0;
    fam = pc_fontdb_find_family(db, family != NULL ? family : "", len);
    if (fam == NULL) {
        return PC_OK;
    }
    for (i = 0; i < fam->nfaces; i++) {
        const pc_fontdb_face *f = &fam->faces[i];
        bool it = f->attrs.slope != PC_SLOPE_NORMAL;
        /* (it != italic) * 10000 + |weight difference| as u32, first minimum. */
        uint32_t key = (it != italic ? 10000u : 0u) + pc_f32_to_u32(fabsf(f->attrs.weight - (float)weight));
        if (best == NULL || key < best_key) {
            best = f;
            best_key = key;
        }
    }
    if (best == NULL || (font = pc_fontdb_load_face(db, best, err)) == NULL) {
        return PC_OK;
    }
    pc_font_sfnt(font, &sf);
    pc_sfnt_cmap_skrifa(&sf, &cm);
    pc_sfnt_cmap_each(&cm, cm_push, &acc);
    if (acc.nomem) {
        free(acc.v);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    qsort(acc.v, acc.n, sizeof *acc.v, cmp_u32);
    for (i = 0; i < acc.n; i++) {
        if (m == 0 || acc.v[m - 1] != acc.v[i]) {
            acc.v[m++] = acc.v[i];
        }
    }
    *out = acc.v;
    *n = m;
    return PC_OK;
}

bool pc_fontdb_selected_face_has_feature(pc_fontdb *db, const char *family, size_t len, uint16_t weight,
                                         bool italic, uint32_t tag)
{
    const pc_fontdb_family *fam;
    pc_font_query q;
    long ix;
    pc_font *font;
    pc_sfnt sf;
    if (db == NULL) {
        return false;
    }
    if (family == NULL || len == 0 || !pc_fontdb_has_family(db, family, len)) {
        family = PC_TEXT_DEFAULT_FAMILY;
        len = strlen(family);
    }
    fam = pc_fontdb_find_family(db, family, len);
    if (fam == NULL) {
        return false;
    }
    q.width = 1.0f;
    q.slope = italic ? PC_SLOPE_ITALIC : PC_SLOPE_NORMAL;
    q.has_angle = false;
    q.angle = 0.0f;
    q.weight = (float)(weight < 1 ? 1 : weight > 1000 ? 1000 : weight);
    ix = pc_fontdb_match_index(fam, &q, true);
    if (ix < 0 || (font = pc_fontdb_load_face(db, &fam->faces[ix], NULL)) == NULL) {
        return false;
    }
    pc_font_sfnt(font, &sf);
    return pc_sfnt_gsub_has_feature(&sf, tag);
}

/* ------------------------------------------------------------------------------------------ */
/* PostScript names */

/* ps_cache.insert: replaces the entry for `ps` or appends one (copies r). */
static pc_status cache_put(pc_fontdb *db, const char *ps, size_t len, const pc_resolved_font *r,
                           pc_error *err)
{
    size_t i;
    char *fam = pc_strndup(r->family, strlen(r->family)), *key;
    pc_fontdb_ps_entry *g;
    if (fam == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < db->nps; i++) {
        if (db->ps[i].len == len && memcmp(db->ps[i].ps, ps, len) == 0) {
            free(db->ps[i].r.family);
            db->ps[i].r = *r;
            db->ps[i].r.family = fam;
            return PC_OK;
        }
    }
    g = pc_grow_array(db->ps, &db->ps_cap, db->nps + 1, sizeof *g);
    key = pc_strndup(ps, len);
    if (g == NULL || key == NULL) {
        free(key);
        free(fam);
        if (g != NULL) {
            db->ps = g;
        }
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    db->ps = g;
    db->ps[db->nps].ps = key;
    db->ps[db->nps].len = len;
    db->ps[db->nps].r = *r;
    db->ps[db->nps].r.family = fam;
    db->nps++;
    return PC_OK;
}

/* Whether a family's lowercase name without spaces starts with `first` (lowercase). */
static pc_status starts_with_word(const char *fam, const pc_buf *first, bool *out, pc_error *err)
{
    pc_buf low = PC_BUF_INIT, cand = PC_BUF_INIT;
    size_t j;
    pc_status st = pc_text_lower(fam, strlen(fam), &low, err);
    for (j = 0; st == PC_OK && j < low.len; j++) {
        if (low.data[j] != ' ') {
            st = pc_buf_append_byte(&cand, low.data[j], err);
        }
    }
    *out = st == PC_OK && pc_text_starts_with(cand.len > 0 ? (const char *)cand.data : "", cand.len,
                                              first->len > 0 ? (const char *)first->data : "");
    pc_buf_fini(&low);
    pc_buf_fini(&cand);
    return st;
}

/* find_exact: every face of every family (those whose name starts with the guess's first word
 * first) by PostScript name; each face read on the way is cached. */
static pc_status find_exact(pc_fontdb *db, const char *ps, size_t len, const char *guess,
                            pc_resolved_font *out, bool *found, pc_error *err)
{
    pc_buf first = PC_BUF_INIT;
    pc_strlist fams = {NULL, 0};
    size_t glen = strlen(guess), sp = 0, i, k, pass;
    bool *match = NULL;
    pc_status st;
    *found = false;
    while (sp < glen && guess[sp] != ' ') {
        sp++;
    }
    st = pc_text_lower(guess, sp, &first, err);
    if (st == PC_OK) {
        st = pc_fontdb_families(db, &fams, err);
    }
    if (st == PC_OK && fams.n > 0 && (match = calloc(fams.n, sizeof *match)) == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; st == PC_OK && i < fams.n; i++) {
        st = starts_with_word(fams.v[i], &first, &match[i], err);
    }
    /* A stable sort by "doesn't start with the word": the matching families, then the rest. */
    for (pass = 0; pass < 2 && st == PC_OK && !*found; pass++) {
        for (i = 0; st == PC_OK && !*found && i < fams.n; i++) {
            pc_fontdb_family *fam;
            if (match[i] != (pass == 0)) {
                continue;
            }
            fam = pc_fontdb_find_family(db, fams.v[i], strlen(fams.v[i]));
            for (k = 0; fam != NULL && st == PC_OK && k < fam->nfaces && !*found; k++) {
                pc_fontdb_face *face = &fam->faces[k];
                pc_resolved_font r;
                pc_fontdb_face_names(db, face);
                if (face->ps == NULL) {
                    continue;
                }
                r.family = fam->name;
                r.weight = pc_f32_to_u16(roundf(face->attrs.weight));
                r.italic = face->attrs.slope != PC_SLOPE_NORMAL;
                r.exact = true;
                st = cache_put(db, face->ps, strlen(face->ps), &r, err);
                if (st == PC_OK && strlen(face->ps) == len && memcmp(face->ps, ps, len) == 0) {
                    out->family = pc_strndup(fam->name, fam->len);
                    out->weight = r.weight;
                    out->italic = r.italic;
                    out->exact = true;
                    if (out->family == NULL) {
                        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    } else {
                        *found = true;
                    }
                }
            }
        }
    }
    free(match);
    pc_buf_fini(&first);
    pc_strlist_fini(&fams);
    return st;
}

static bool eq_no_spaces_ignore_case(const char *a, const char *b)
{
    for (;;) {
        while (*a == ' ') {
            a++;
        }
        while (*b == ' ') {
            b++;
        }
        if (*a == '\0' || *b == '\0') {
            return *a == *b;
        }
        if (!pc_text_eq_ignore_ascii_case(a, 1, b, 1)) {
            return false;
        }
        a++;
        b++;
    }
}

pc_status pc_fontdb_resolve_postscript(pc_fontdb *db, const char *ps, size_t len, pc_resolved_font *out,
                                       pc_error *err)
{
    pc_resolved_font guess, r;
    bool found = false;
    size_t i;
    pc_status st;
    if (db == NULL || out == NULL || (ps == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    if (ps == NULL) {
        ps = "";
    }
    for (i = 0; i < db->nps; i++) {
        if (db->ps[i].len == len && memcmp(db->ps[i].ps, ps, len) == 0) {
            out->family = pc_strndup(db->ps[i].r.family, strlen(db->ps[i].r.family));
            if (out->family == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            out->weight = db->ps[i].r.weight;
            out->italic = db->ps[i].r.italic;
            out->exact = db->ps[i].r.exact;
            return PC_OK;
        }
    }
    st = pc_text_guess_from_postscript(ps, len, &guess, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&r, 0, sizeof r);
    st = find_exact(db, ps, len, guess.family, &r, &found, err);
    if (st != PC_OK) {
        pc_resolved_font_fini(&guess);
        return st;
    }
    if (found) {
        pc_resolved_font_fini(&guess);
    } else {
        r = guess;
        if (!pc_fontdb_has_family(db, r.family, strlen(r.family))) {
            pc_strlist fams = {NULL, 0};
            st = pc_fontdb_families(db, &fams, err);
            for (i = 0; st == PC_OK && i < fams.n; i++) {
                if (eq_no_spaces_ignore_case(fams.v[i], r.family)) {
                    free(r.family);
                    r.family = fams.v[i];
                    fams.v[i] = pc_strndup("", 0);
                    break;
                }
            }
            pc_strlist_fini(&fams);
            if (st != PC_OK) {
                pc_resolved_font_fini(&r);
                return st;
            }
        }
    }
    st = cache_put(db, ps, len, &r, err);
    if (st != PC_OK) {
        pc_resolved_font_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}
