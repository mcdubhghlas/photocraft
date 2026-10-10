/* Font database (port of crates/text/src/fonts.rs over fontique's collection; see fonts.h). */
#include "text/fonts.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
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
#include "text/unicode.h"

/* Largest font file read from disk; collections of CJK fonts are tens of MB. */
#define MAX_FONT_FILE ((size_t)1 << 30)

typedef struct ps_entry {
    char *ps;
    size_t len;
    pc_resolved_font r;
} ps_entry;

struct pc_fontdb {
    pc_fontdb_source *src;
    size_t nsrc, src_cap;
    pc_fontdb_family *fam;
    size_t nfam, fam_cap;
    bool system_loaded;
    ps_entry *ps;
    size_t nps, ps_cap;
    const char **fallbacks; /* borrowed candidate spellings (static or craft list) */
    size_t nfallbacks;
};

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

static pc_status strlist_push(pc_strlist *l, const char *s, size_t len, pc_error *err)
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

/* fontique register_font_impl over one source's bytes: every face with a family name and a
 * usable cmap joins its family. Collects the families touched in `touched` (may be NULL). */
static pc_status scan_source(pc_fontdb *db, size_t src, const uint8_t *data, size_t len, size_t **touched,
                             size_t *ntouched, pc_error *err)
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

static pc_status add_source(pc_fontdb *db, const char *path, pc_blob *bytes, size_t *idx, pc_error *err)
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

static void refresh_generics(pc_fontdb *db);

static pc_status register_blob(pc_fontdb *db, pc_blob *blob, pc_strlist *names, pc_error *err)
{
    size_t src, *touched = NULL, nt = 0, i, k;
    pc_status st = add_source(db, NULL, blob, &src, err);
    if (st != PC_OK) {
        return st;
    }
    st = scan_source(db, src, pc_blob_data(blob), pc_blob_len(blob), &touched, &nt, err);
    if (st == PC_OK && names != NULL) {
        pc_strlist l = {NULL, 0};
        for (i = 0; i < nt && st == PC_OK; i++) {
            st = strlist_push(&l, db->fam[touched[i]].name, db->fam[touched[i]].len, err);
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
        for (i = 0; i < db->nps; i++) {
            free(db->ps[i].ps);
            pc_resolved_font_fini(&db->ps[i].r);
        }
        db->nps = 0;
        refresh_generics(db);
    }
    return st;
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
    cap = 7 + 4 + ncraft + 4 * 8;
    v = pc_malloc_array(cap, sizeof *v);
    if (v == NULL) {
        free(craft);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < 7; i++) {
        v[m++] = FALLBACK_CANDIDATES[i];
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

static void refresh_generics(pc_fontdb *db)
{
    pc_cjk_script order[4];
    const char **c = NULL;
    size_t n = 0, i, m = 0;
    pc_cjk_ui_script_order(order);
    if (pc_text_fallback_candidates(order, &c, &n, NULL) != PC_OK) {
        return; /* out of memory: keep the previous list */
    }
    for (i = 0; i < n; i++) {
        if (pc_fontdb_has_family(db, c[i], strlen(c[i]))) {
            c[m++] = c[i];
        }
    }
    free(db->fallbacks);
    db->fallbacks = c;
    db->nfallbacks = m;
}

size_t pc_fontdb_fallback_count(const pc_fontdb *db)
{
    return db->nfallbacks + 1;
}

const char *pc_fontdb_fallback(const pc_fontdb *db, size_t i)
{
    if (i == 0) {
        return PC_TEXT_DEFAULT_FAMILY;
    }
    return i - 1 < db->nfallbacks ? db->fallbacks[i - 1] : NULL;
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
    refresh_generics(db);
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
        }
        free(db->fam[i].faces);
        free(db->fam[i].name);
        free(db->fam[i].key);
    }
    for (i = 0; i < db->nps; i++) {
        free(db->ps[i].ps);
        pc_resolved_font_fini(&db->ps[i].r);
    }
    free(db->ps);
    free(db->src);
    free(db->fam);
    free(db->fallbacks);
    free(db);
}

static pc_status push_dir(pc_strlist *l, const char *a, const char *b, pc_error *err)
{
    pc_buf p = PC_BUF_INIT;
    pc_status st = pc_buf_appendf(&p, err, "%s%s", a, b);
    if (st == PC_OK) {
        st = strlist_push(l, pc_buf_cstr(&p), p.len, err);
    }
    pc_buf_fini(&p);
    return st;
}

pc_status pc_text_system_font_dirs(pc_strlist *out, pc_error *err)
{
    const char *home = getenv("HOME");
    pc_strlist l = {NULL, 0};
    pc_status st;
#ifdef __APPLE__
    st = push_dir(&l, "/System/Library/Fonts", "", err);
    if (st == PC_OK) {
        st = push_dir(&l, "/Library/Fonts", "", err);
    }
    if (st == PC_OK && home != NULL) {
        st = push_dir(&l, home, "/Library/Fonts", err);
    }
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    st = push_dir(&l, "/usr/share/fonts", "", err);
    if (st == PC_OK) {
        st = push_dir(&l, "/usr/local/share/fonts", "", err);
    }
    /* Inside a Flatpak sandbox the host's system and per-user fonts are mounted here. */
    if (st == PC_OK) {
        st = push_dir(&l, "/run/host/fonts", "", err);
    }
    if (st == PC_OK) {
        st = push_dir(&l, "/run/host/user-fonts", "", err);
    }
    if (st == PC_OK && xdg != NULL) {
        st = push_dir(&l, xdg, "/fonts", err);
    }
    if (st == PC_OK && home != NULL) {
        st = push_dir(&l, home, "/.local/share/fonts", err);
    }
    if (st == PC_OK && home != NULL) {
        st = push_dir(&l, home, "/.fonts", err);
    }
#endif
    if (st != PC_OK) {
        pc_strlist_fini(&l);
        return st;
    }
    *out = l;
    return PC_OK;
}

/* Path::extension of the last component, compared case-insensitively with font extensions. */
static bool font_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) {
        return false;
    }
    return pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "ttf", 3) ||
           pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "otf", 3) ||
           pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "ttc", 3) ||
           pc_text_eq_ignore_ascii_case(dot + 1, strlen(dot + 1), "otc", 3);
}

static pc_status collect(const char *dir, unsigned depth, pc_strlist *out, pc_error *err)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    pc_status st = PC_OK;
    if (d == NULL) {
        return PC_OK;
    }
    while (st == PC_OK && (e = readdir(d)) != NULL) {
        pc_buf p = PC_BUF_INIT;
        struct stat sb;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        st = pc_buf_appendf(&p, err, "%s/%s", dir, e->d_name);
        if (st != PC_OK) {
            pc_buf_fini(&p);
            break;
        }
        if (stat(pc_buf_cstr(&p), &sb) == 0 && S_ISDIR(sb.st_mode)) {
            /* Depth-limited, so symlink loops end. */
            if (depth < 8) {
                st = collect(pc_buf_cstr(&p), depth + 1, out, err);
            }
        } else if (font_ext(e->d_name)) {
            st = strlist_push(out, pc_buf_cstr(&p), p.len, err);
        }
        pc_buf_fini(&p);
    }
    closedir(d);
    return st;
}

pc_status pc_text_collect_font_files(const char *dir, pc_strlist *out, pc_error *err)
{
    if (dir == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    return collect(dir, 0, out, err);
}

pc_status pc_fontdb_add_font_file(pc_fontdb *db, const char *path, pc_error *err)
{
    pc_buf bytes = PC_BUF_INIT;
    size_t src;
    pc_status st;
    pc_error e;
    if (db == NULL || path == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    st = pc_file_read(path, MAX_FONT_FILE, &bytes, &e);
    if (st != PC_OK) {
        pc_buf_fini(&bytes);
        if (st == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
        }
        return PC_OK; /* unreadable files are skipped, as fontique's scan does */
    }
    st = add_source(db, path, NULL, &src, err);
    if (st == PC_OK) {
        st = scan_source(db, src, bytes.data, bytes.len, NULL, NULL, err);
    }
    pc_buf_fini(&bytes); /* reloaded when a face is used */
    return st;
}

pc_status pc_fontdb_load_system_fonts(pc_fontdb *db, pc_error *err)
{
    pc_strlist dirs = {NULL, 0}, files = {NULL, 0};
    size_t i;
    pc_status st;
    if (db == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    if (db->system_loaded) {
        return PC_OK;
    }
    db->system_loaded = true;
    st = pc_text_system_font_dirs(&dirs, err);
    for (i = 0; st == PC_OK && i < dirs.n; i++) {
        st = collect(dirs.v[i], 0, &files, err);
    }
#ifdef __APPLE__
    if (st == PC_OK) {
        /* PingFang (the macOS Chinese UI font) lives outside the font folders. */
        char *pf = pc_cjk_mac_pingfang();
        if (pf != NULL) {
            st = strlist_push(&files, pf, strlen(pf), err);
            free(pf);
        }
    }
#endif
    for (i = 0; st == PC_OK && i < files.n; i++) {
        st = pc_fontdb_add_font_file(db, files.v[i], err);
    }
    pc_strlist_fini(&dirs);
    pc_strlist_fini(&files);
    if (st == PC_OK) {
        for (i = 0; i < db->nps; i++) {
            free(db->ps[i].ps);
            pc_resolved_font_fini(&db->ps[i].r);
        }
        db->nps = 0;
        refresh_generics(db);
    }
    return st;
}

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
            st = strlist_push(&l, v[i].name, strlen(v[i].name), err);
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
        free(f[i].axes);
    }
    free(f);
}

pc_status pc_fontdb_faces(pc_fontdb *db, const char *family, size_t len, pc_face_info **out, size_t *n,
                          pc_error *err)
{
    const pc_fontdb_family *fam;
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
        const pc_fontdb_face *f = &fam->faces[i];
        v[i].family = pc_strndup(fam->name, fam->len);
        v[i].weight = f->attrs.weight;
        v[i].italic = f->attrs.slope != PC_SLOPE_NORMAL;
        v[i].naxes = f->naxes;
        if (f->naxes > 0) {
            v[i].axes = pc_malloc_array(f->naxes, sizeof *v[i].axes);
            if (v[i].axes != NULL) {
                memcpy(v[i].axes, f->axes, f->naxes * sizeof *f->axes);
            }
        }
        if (v[i].family == NULL || (f->naxes > 0 && v[i].axes == NULL)) {
            pc_face_infos_free(v, i + 1);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    *out = v;
    *n = fam->nfaces;
    return PC_OK;
}

/* The loaded font of a face (borrowed; NULL when its bytes can't be read or opened). */
static pc_font *face_font(pc_fontdb *db, const pc_fontdb_face *face, pc_error *err)
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
    return face_font(db, &fam->faces[i], err);
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
    if (best == NULL || (font = face_font(db, best, err)) == NULL) {
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
    if (ix < 0 || (font = face_font(db, &fam->faces[ix], NULL)) == NULL) {
        return false;
    }
    pc_font_sfnt(font, &sf);
    return pc_sfnt_gsub_has_feature(&sf, tag);
}

/* ------------------------------------------------------------------------------------------ */
/* PostScript names */

typedef struct exact_ctx {
    const char *ps;
    size_t len;
} exact_ctx;

static pc_status find_exact(pc_fontdb *db, const char *ps, size_t len, const char *guess,
                            pc_resolved_font *out, bool *found, pc_error *err)
{
    pc_buf first = PC_BUF_INIT;
    pc_strlist fams = {NULL, 0};
    size_t glen = strlen(guess), sp = 0, i, k;
    pc_status st;
    *found = false;
    while (sp < glen && guess[sp] != ' ') {
        sp++;
    }
    st = pc_text_lower(guess, sp, &first, err);
    if (st == PC_OK) {
        st = pc_fontdb_families(db, &fams, err);
    }
    for (i = 0; st == PC_OK && !*found && i < fams.n; i++) {
        pc_buf low = PC_BUF_INIT, cand = PC_BUF_INIT;
        const pc_fontdb_family *fam;
        size_t j;
        /* f.to_lowercase().replace(' ', "").starts_with(first_word) */
        st = pc_text_lower(fams.v[i], strlen(fams.v[i]), &low, err);
        for (j = 0; st == PC_OK && j < low.len; j++) {
            if (low.data[j] != ' ') {
                st = pc_buf_append_byte(&cand, low.data[j], err);
            }
        }
        if (st != PC_OK || !pc_text_starts_with(cand.len > 0 ? (const char *)cand.data : "", cand.len,
                                                first.len > 0 ? (const char *)first.data : "")) {
            pc_buf_fini(&low);
            pc_buf_fini(&cand);
            continue;
        }
        pc_buf_fini(&low);
        pc_buf_fini(&cand);
        fam = pc_fontdb_find_family(db, fams.v[i], strlen(fams.v[i]));
        for (k = 0; fam != NULL && k < fam->nfaces && !*found; k++) {
            pc_font *font = face_font(db, &fam->faces[k], NULL);
            pc_sfnt sf;
            pc_buf name = PC_BUF_INIT;
            if (font == NULL) {
                continue;
            }
            pc_font_sfnt(font, &sf);
            if (pc_sfnt_name(&sf, PC_SFNT_NAME_POSTSCRIPT, PC_SFNT_NAME_SKRIFA, &name, NULL) &&
                name.len == len && (len == 0 || memcmp(name.data, ps, len) == 0)) {
                out->family = pc_strndup(fam->name, fam->len);
                out->weight = pc_f32_to_u16(roundf(fam->faces[k].attrs.weight));
                out->italic = fam->faces[k].attrs.slope != PC_SLOPE_NORMAL;
                out->exact = true;
                if (out->family == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                } else {
                    *found = true;
                }
            }
            pc_buf_fini(&name);
        }
    }
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
    /* Cache a copy. */
    {
        ps_entry *g = pc_grow_array(db->ps, &db->ps_cap, db->nps + 1, sizeof *g);
        char *key = pc_strndup(ps, len), *fam = pc_strndup(r.family, strlen(r.family));
        if (g == NULL || key == NULL || fam == NULL) {
            free(key);
            free(fam);
            if (g != NULL) {
                db->ps = g;
            }
            pc_resolved_font_fini(&r);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        db->ps = g;
        db->ps[db->nps].ps = key;
        db->ps[db->nps].len = len;
        db->ps[db->nps].r = r;
        db->ps[db->nps].r.family = fam;
        db->nps++;
    }
    *out = r;
    return PC_OK;
}
