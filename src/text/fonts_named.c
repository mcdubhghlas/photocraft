/* Exact named faces (port of the face-name half of crates/text/src/fonts.rs): FaceInfo style and
 * PostScript names, Menu style -> face, private per-face alias families for shaping, TTC face
 * isolation, localized family names. See fonts.h. */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/fonts_internal.h"
#include "text/unicode.h"

/* Collections larger than this are not copied to isolate a face (untrusted font data). */
#define MAX_ISOLATED ((size_t)256 << 20)

const char *pc_text_fallback_style(float weight, bool italic, char buf[32])
{
    int32_t w = pc_f32_to_i32(roundf(weight));
    const char *name = w <= 150   ? "Thin"
                       : w <= 250 ? "ExtraLight"
                       : w <= 350 ? "Light"
                       : w <= 450 ? "Regular"
                       : w <= 550 ? "Medium"
                       : w <= 650 ? "SemiBold"
                       : w <= 750 ? "Bold"
                       : w <= 850 ? "ExtraBold"
                                  : "Black";
    if (!italic) {
        (void)snprintf(buf, 32, "%s", name);
    } else if (strcmp(name, "Regular") == 0) {
        (void)snprintf(buf, 32, "Italic");
    } else {
        (void)snprintf(buf, 32, "%s Italic", name);
    }
    return buf;
}

/* A name of the face (english_or_first), NULL when empty or absent. */
static char *face_name(const pc_sfnt *f, uint16_t id)
{
    pc_buf b = PC_BUF_INIT;
    char *r = NULL;
    if (pc_sfnt_name(f, id, PC_SFNT_NAME_SKRIFA, &b, NULL) && b.len > 0) {
        r = pc_strndup((const char *)b.data, b.len);
    }
    pc_buf_fini(&b);
    return r;
}

static void names_from(pc_fontdb_face *face, const uint8_t *data, size_t len)
{
    pc_sfnt f;
    if (!pc_sfnt_open(data, len, face->index, &f)) {
        return;
    }
    face->style = face_name(&f, PC_SFNT_NAME_TYPOGRAPHIC_SUBFAMILY);
    if (face->style == NULL) {
        face->style = face_name(&f, PC_SFNT_NAME_SUBFAMILY);
    }
    face->ps = face_name(&f, PC_SFNT_NAME_POSTSCRIPT);
}

void pc_fontdb_face_names(pc_fontdb *db, pc_fontdb_face *face)
{
    pc_fontdb_source *s;
    if (face->names_done || face->src >= db->nsrc) {
        return;
    }
    face->names_done = true;
    s = &db->src[face->src];
    if (s->bytes != NULL) {
        names_from(face, pc_blob_data(s->bytes), pc_blob_len(s->bytes));
    } else if (s->path != NULL && !s->failed) {
        /* Map the file instead of reading it: only the name table's pages are touched, and the
         * bytes aren't kept (unlike a loaded face). */
        int fd = open(s->path, O_RDONLY);
        struct stat sb;
        if (fd >= 0 && fstat(fd, &sb) == 0 && sb.st_size > 0 && (uint64_t)sb.st_size <= SIZE_MAX) {
            size_t len = (size_t)sb.st_size;
            void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
            if (m != MAP_FAILED) {
                names_from(face, m, len);
                (void)munmap(m, len);
            }
        }
        if (fd >= 0) {
            (void)close(fd);
        }
    }
}

pc_status pc_fontdb_named_face(pc_fontdb *db, const char *family, const char *style, pc_face_info **out,
                               bool *found, pc_error *err)
{
    pc_face_info *faces = NULL;
    size_t n = 0, i;
    pc_status st;
    *found = false;
    if (db == NULL || family == NULL || style == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    st = pc_fontdb_faces(db, family, strlen(family), &faces, &n, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        if (pc_text_eq_ignore_ascii_case(faces[i].style, strlen(faces[i].style), style, strlen(style))) {
            pc_face_info *one = calloc(1, sizeof *one);
            if (one == NULL) {
                pc_face_infos_free(faces, n);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            *one = faces[i];
            memset(&faces[i], 0, sizeof faces[i]);
            *out = one;
            *found = true;
            break;
        }
    }
    pc_face_infos_free(faces, n);
    return PC_OK;
}

pc_status pc_text_isolated_face(pc_blob *blob, uint32_t index, pc_blob **out, bool *ok, pc_error *err)
{
    const uint8_t *d = pc_blob_data(blob);
    size_t len = pc_blob_len(blob), off;
    uint8_t *copy;
    pc_sfnt f;
    pc_status st;
    *ok = false;
    if (len >= 4 && memcmp(d, "ttcf", 4) == 0) {
        if (index >= pc_sfnt_face_count(d, len) || !pc_sfnt_open(d, len, index, &f) || len > MAX_ISOLATED) {
            return PC_OK;
        }
        off = 12 + (size_t)index * 4;
        if (off + 4 > len) {
            return PC_OK;
        }
        copy = malloc(len);
        if (copy == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(copy, d, len);
        pc_store_be32(copy + 8, 1);
        memcpy(copy + 12, d + off, 4);
        st = pc_blob_new(copy, len, out, err);
        free(copy);
        if (st == PC_OK) {
            *ok = true;
        }
        return st;
    }
    if (index != 0 || !pc_sfnt_open(d, len, 0, &f)) {
        return PC_OK;
    }
    *out = pc_blob_retain(blob);
    *ok = true;
    return PC_OK;
}

/* The alias registered for (family spelling, face index), or NULL. */
static const char *find_alias(const pc_fontdb *db, const char *family, size_t len, size_t index)
{
    size_t i;
    for (i = 0; i < db->naliases; i++) {
        if (db->aliases[i].index == index && strlen(db->aliases[i].family) == len &&
            memcmp(db->aliases[i].family, family, len) == 0) {
            return db->aliases[i].alias;
        }
    }
    return NULL;
}

/* Registers face `index` of `family` under a new alias family; *alias receives its name
 * (borrowed from the alias table). NULL alias (PC_OK) when the face can't be isolated. */
static pc_status register_alias(pc_fontdb *db, const char *family, size_t len, size_t index,
                                const char **alias, pc_error *err)
{
    pc_fontdb_family *fam = pc_fontdb_find_family(db, family, len);
    const pc_fontdb_face *face;
    pc_font *font;
    pc_blob *iso = NULL;
    pc_buf name = PC_BUF_INIT;
    pc_fontdb_alias *g;
    char *fcopy, *acopy;
    size_t src;
    bool ok = false;
    pc_status st;
    *alias = NULL;
    if (fam == NULL || index >= fam->nfaces) {
        return PC_OK;
    }
    face = &fam->faces[index];
    font = pc_fontdb_load_face(db, face, NULL);
    if (font == NULL) {
        return PC_OK;
    }
    st = pc_text_isolated_face(pc_font_blob(font), face->index, &iso, &ok, err);
    if (st != PC_OK || !ok) {
        return st;
    }
    st = pc_buf_appendf(&name, err, ".PhotoCraft-face-%llu-%zu", (unsigned long long)fam->id, index);
    if (st == PC_OK) {
        st = pc_fontdb_add_source(db, NULL, iso, &src, err);
    }
    /* fam may move from here on (new families). */
    if (st == PC_OK) {
        st = pc_fontdb_scan_source(db, src, pc_blob_data(iso), pc_blob_len(iso), pc_buf_cstr(&name), NULL,
                                   NULL, err);
    }
    pc_blob_release(iso);
    if (st != PC_OK) {
        pc_buf_fini(&name);
        return st;
    }
    g = pc_grow_array(db->aliases, &db->alias_cap, db->naliases + 1, sizeof *g);
    fcopy = pc_strndup(family, len);
    acopy = pc_strndup(pc_buf_cstr(&name), name.len);
    pc_buf_fini(&name);
    if (g == NULL || fcopy == NULL || acopy == NULL) {
        if (g != NULL) {
            db->aliases = g;
        }
        free(fcopy);
        free(acopy);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    db->aliases = g;
    g[db->naliases].family = fcopy;
    g[db->naliases].index = index;
    g[db->naliases].alias = acopy;
    db->naliases++;
    *alias = acopy;
    return PC_OK;
}

pc_status pc_fontdb_select_named_face(pc_fontdb *db, pc_char_style *style, pc_error *err)
{
    pc_fontdb_family *fam;
    const char *family, *alias;
    size_t len, i, index = SIZE_MAX;
    uint16_t weight;
    bool italic;
    pc_status st;
    if (db == NULL || style == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    family = pc_str_c(&style->font_family);
    len = style->font_family.len;
    fam = pc_fontdb_find_family(db, family, len);
    if (fam == NULL) {
        return PC_OK;
    }
    for (i = 0; i < fam->nfaces; i++) {
        pc_fontdb_face_names(db, &fam->faces[i]);
    }
    if (style->has_postscript_name) {
        for (i = 0; i < fam->nfaces && index == SIZE_MAX; i++) {
            const char *ps = fam->faces[i].ps;
            if (ps != NULL && strlen(ps) == style->postscript_name.len &&
                memcmp(ps, pc_str_c(&style->postscript_name), style->postscript_name.len) == 0) {
                index = i;
            }
        }
    }
    for (i = 0; i < fam->nfaces && index == SIZE_MAX && style->font_style.len > 0; i++) {
        const pc_fontdb_face *f = &fam->faces[i];
        char sb[32];
        const char *name =
            f->style != NULL ? f->style
                             : pc_text_fallback_style(f->attrs.weight, f->attrs.slope != PC_SLOPE_NORMAL, sb);
        if (pc_text_eq_ignore_ascii_case(name, strlen(name), pc_str_c(&style->font_style),
                                         style->font_style.len) &&
            pc_f32_to_u16(roundf(f->attrs.weight)) == style->weight &&
            (f->attrs.slope != PC_SLOPE_NORMAL) == style->italic) {
            index = i;
        }
    }
    if (index == SIZE_MAX) {
        return PC_OK;
    }
    weight = pc_f32_to_u16(roundf(fam->faces[index].attrs.weight));
    italic = fam->faces[index].attrs.slope != PC_SLOPE_NORMAL;
    alias = find_alias(db, family, len, index);
    if (alias == NULL) {
        st = register_alias(db, family, len, index, &alias, err);
        if (st != PC_OK || alias == NULL) {
            return st;
        }
    }
    st = pc_str_set_c(&style->font_family, alias, err);
    if (st != PC_OK) {
        return st;
    }
    style->weight = weight;
    style->italic = italic;
    return PC_OK;
}

pc_status pc_fontdb_charmap_for_style(pc_fontdb *db, const pc_char_style *style, uint32_t **out, size_t *n,
                                      pc_error *err)
{
    pc_char_style s;
    pc_status st;
    if (db == NULL || style == NULL || out == NULL || n == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    st = pc_char_style_clone(style, &s, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_fontdb_select_named_face(db, &s, err);
    if (st == PC_OK) {
        st = pc_fontdb_charmap(db, pc_str_c(&s.font_family), s.font_family.len, s.weight, s.italic, out, n,
                               err);
    }
    pc_char_style_fini(&s);
    return st;
}

/* ---- localized family names ---- */

const char *pc_text_localized_name(const char *english, const char *const *names, const char *const *langs,
                                   size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        const char *l = langs[i];
        size_t pl = 0;
        bool blank = true;
        if (l == NULL || l[0] == '\0') {
            continue;
        }
        while (l[pl] != '\0' && l[pl] != '-' && l[pl] != '_') {
            pl++;
        }
        if (pc_text_eq_ignore_ascii_case(l, pl, "en", 2)) {
            continue;
        }
        {
            /* str::trim().is_empty(): only Unicode White_Space. */
            size_t at = 0, nl = strlen(names[i]);
            while (at < nl && blank) {
                uint32_t cp;
                blank = pc_utf8_next((const uint8_t *)names[i], nl, &at, &cp) && pc_unicode_is_space(cp);
            }
        }
        if (blank || strcmp(names[i], english) == 0) {
            continue;
        }
        return names[i];
    }
    return NULL;
}

typedef struct lname_acc {
    pc_strlist names;
    const char **langs;
    size_t cap;
    bool nomem;
} lname_acc;

static bool collect_lname(void *ctx, const char *s, size_t len, pc_sfnt_lang lang)
{
    lname_acc *a = ctx;
    static const char *const TAGS[4] = {NULL, "", "en", "x"};
    if (a->nomem) {
        return true;
    }
    if (a->names.n == a->cap) {
        size_t cap = a->cap;
        const char **g = pc_grow_array(a->langs, &cap, a->names.n + 1, sizeof *g);
        if (g == NULL) {
            a->nomem = true;
            return true;
        }
        a->langs = g;
        a->cap = cap;
    }
    if (pc_strlist_push(&a->names, s, len, NULL) != PC_OK) {
        a->nomem = true;
        return true;
    }
    a->langs[a->names.n - 1] = TAGS[lang];
    return false;
}

pc_status pc_fontdb_localized_family_names(pc_fontdb *db, pc_strlist *out, pc_error *err)
{
    pc_strlist fams = {NULL, 0}, res = {NULL, 0};
    size_t i;
    pc_status st;
    if (db == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "fonts: invalid arguments");
    }
    st = pc_fontdb_families(db, &fams, err);
    for (i = 0; st == PC_OK && i < fams.n; i++) {
        pc_fontdb_family *fam = pc_fontdb_find_family(db, fams.v[i], strlen(fams.v[i]));
        pc_font *font;
        pc_sfnt sf;
        lname_acc a;
        const char *local;
        if (fam == NULL || fam->nfaces == 0 ||
            (font = pc_fontdb_load_face(db, &fam->faces[0], NULL)) == NULL) {
            continue;
        }
        pc_font_sfnt(font, &sf);
        memset(&a, 0, sizeof a);
        (void)pc_sfnt_each_name_lang(&sf, PC_SFNT_NAME_TYPOGRAPHIC_FAMILY, collect_lname, &a);
        if (!a.nomem && a.names.n == 0) {
            (void)pc_sfnt_each_name_lang(&sf, PC_SFNT_NAME_FAMILY, collect_lname, &a);
        }
        if (a.nomem) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        } else {
            local = pc_text_localized_name(fams.v[i], (const char *const *)a.names.v, a.langs, a.names.n);
            if (local != NULL) {
                st = pc_strlist_push(&res, fams.v[i], strlen(fams.v[i]), err);
                if (st == PC_OK) {
                    st = pc_strlist_push(&res, local, strlen(local), err);
                }
            }
        }
        pc_strlist_fini(&a.names);
        free(a.langs);
    }
    pc_strlist_fini(&fams);
    if (st != PC_OK) {
        pc_strlist_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}
