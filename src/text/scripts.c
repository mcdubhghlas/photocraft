/* Script fallback candidates (port of crates/text/src/scripts.rs). See scripts.h. */
#include "text/scripts.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "text/sfnt.h"
#include "text/unicode.h"

static const char *const THAI[] = {"notosansthai",   "notosansthailooped",
                                   "notosansthaiui", "notoserifthai",
                                   "leelawui",       "leelawad",
                                   "tahoma",         "thonburi",
                                   "sukhumvitset",   "ayuthaya",
                                   "loma",           "garuda",
                                   "waree",          "kinnari",
                                   "norasi",         NULL};
static const char *const LAO[] = {"notosanslao", "notosanslaoui", "notoseriflao", "leelawui", "laosangammn",
                                  "laomn",       "phetsarathot",  "saysetthaot",  NULL};
static const char *const KHMER[] = {
    "notosanskhmer", "notosanskhmerui", "notoserifkhmer", "leelawui",   "daunpenh",
    "khmersangammn", "khmermn",         "khmeros",        "khmerossys", NULL};
static const char *const MYANMAR[] = {"notosansmyanmar", "notosansmyanmarui", "notoserifmyanmar", "mmrtext",
                                      "myanmarsangammn", "myanmarmn",         "padauk",           NULL};
static const char *const TIBETAN[] = {"notoseriftibetan", "himalaya",          "kailasa",
                                      "jomolhari",        "tibetanmachineuni", NULL};
static const char *const ARABIC[] = {"notosansarabic",
                                     "notosansarabicui",
                                     "notonaskharabic",
                                     "notonaskharabicui",
                                     "notokufiarabic",
                                     "segoeui",
                                     "tahoma",
                                     "geezapro",
                                     "sfarabic",
                                     "dejavusans",
                                     "kacstone",
                                     "kacstbook",
                                     "amiri",
                                     "arial",
                                     NULL};
static const char *const HEBREW[] = {"notosanshebrew", "notoserifhebrew", "segoeui",  "arialhb", "sfhebrew",
                                     "dejavusans",     "arial",           "freesans", "culmus",  NULL};
static const char *const ARMENIAN[] = {"notosansarmenian", "notoserifarmenian", "segoeui",  "sylfaen",
                                       "mshtakan",         "dejavusans",        "freesans", NULL};
static const char *const GEORGIAN[] = {"notosansgeorgian", "notoserifgeorgian", "segoeui", "sylfaen",
                                       "dejavusans",       "freesans",          NULL};
static const char *const ETHIOPIC[] = {
    "notosansethiopic", "notoserifethiopic", "ebrima", "kefa", "abyssinicasil", NULL};
static const char *const DEVANAGARI[] = {"notosansdevanagari",
                                         "notosansdevanagariui",
                                         "notoserifdevanagari",
                                         "nirmala",
                                         "mangal",
                                         "kohinoor",
                                         "kohinoordevanagari",
                                         "itfdevanagari",
                                         "devanagarimt",
                                         "devanagarisangammn",
                                         "lohitdevanagari",
                                         "lohit-devanagari",
                                         "gargi",
                                         "freesans",
                                         NULL};
static const char *const BENGALI[] = {"notosansbengali", "notosansbengaliui", "notoserifbengali", "nirmala",
                                      "vrinda",          "kohinoorbangla",    "banglasangammn",   "banglamn",
                                      "lohitbengali",    "lohit-bengali",     "lohit-assamese",   NULL};
static const char *const GURMUKHI[] = {
    "notosansgurmukhi", "notosansgurmukhiui", "notoserifgurmukhi", "nirmala",       "raavi",
    "gurmukhisangammn", "gurmukhimn",         "lohit-gurmukhi",    "lohit-punjabi", NULL};
static const char *const GUJARATI[] = {
    "notosansgujarati", "notosansgujaratiui", "notoserifgujarati", "nirmala",        "shruti",
    "kohinoorgujarati", "gujaratisangammn",   "gujaratimt",        "lohit-gujarati", NULL};
static const char *const ORIYA[] = {
    "notosansoriya", "notosansoriyaui", "notoseriforiya", "nirmala",     "kalinga",
    "oriyasangammn", "oriyamn",         "lohit-odia",     "lohit-oriya", NULL};
static const char *const TAMIL[] = {"notosanstamil", "notosanstamilui", "notoseriftamil", "nirmala", "latha",
                                    "tamilsangammn", "tamilmn",         "lohit-tamil",    NULL};
static const char *const TELUGU[] = {
    "notosanstelugu", "notosansteluguui", "notoseriftelugu", "nirmala",      "gautami",
    "kohinoortelugu", "telugusangammn",   "telugumn",        "lohit-telugu", NULL};
static const char *const KANNADA[] = {
    "notosanskannada", "notosanskannadaui", "notoserifkannada", "nirmala", "tunga",
    "kannadasangammn", "kannadamn",         "lohit-kannada",    NULL};
static const char *const MALAYALAM[] = {
    "notosansmalayalam", "notosansmalayalamui", "notoserifmalayalam", "nirmala", "kartika",
    "malayalamsangammn", "malayalammn",         "lohit-malayalam",    NULL};
static const char *const SINHALA[] = {"notosanssinhala",
                                      "notosanssinhalaui",
                                      "notoserifsinhala",
                                      "nirmala",
                                      "iskpota",
                                      "sinhalasangammn",
                                      "sinhalamn",
                                      "lklug",
                                      NULL};

static const char *const *const NAMES[PC_SCRIPT_COUNT] = {
    THAI,       LAO,     KHMER,    MYANMAR,  TIBETAN, ARABIC, HEBREW, ARMENIAN, GEORGIAN,  ETHIOPIC,
    DEVANAGARI, BENGALI, GURMUKHI, GUJARATI, ORIYA,   TAMIL,  TELUGU, KANNADA,  MALAYALAM, SINHALA,
};

const char *const *pc_text_script_file_names(pc_text_script s)
{
    return (unsigned)s < PC_SCRIPT_COUNT ? NAMES[s] : NULL;
}

bool pc_text_script_classify(uint32_t c, pc_text_script *out)
{
    static const struct {
        uint32_t a, b;
        pc_text_script s;
    } R[] = {
        {0x0E00, 0x0E7F, PC_SCRIPT_THAI},       {0x0E80, 0x0EFF, PC_SCRIPT_LAO},
        {0x1780, 0x17FF, PC_SCRIPT_KHMER},      {0x19E0, 0x19FF, PC_SCRIPT_KHMER},
        {0x1000, 0x109F, PC_SCRIPT_MYANMAR},    {0xA9E0, 0xA9FF, PC_SCRIPT_MYANMAR},
        {0xAA60, 0xAA7F, PC_SCRIPT_MYANMAR},    {0x0F00, 0x0FFF, PC_SCRIPT_TIBETAN},
        {0x0600, 0x06FF, PC_SCRIPT_ARABIC},     {0x0750, 0x077F, PC_SCRIPT_ARABIC},
        {0x0870, 0x08FF, PC_SCRIPT_ARABIC},     {0xFB50, 0xFDFF, PC_SCRIPT_ARABIC},
        {0xFE70, 0xFEFF, PC_SCRIPT_ARABIC},     {0x0590, 0x05FF, PC_SCRIPT_HEBREW},
        {0xFB1D, 0xFB4F, PC_SCRIPT_HEBREW},     {0x0530, 0x058F, PC_SCRIPT_ARMENIAN},
        {0xFB13, 0xFB17, PC_SCRIPT_ARMENIAN},   {0x10A0, 0x10FF, PC_SCRIPT_GEORGIAN},
        {0x1C90, 0x1CBF, PC_SCRIPT_GEORGIAN},   {0x2D00, 0x2D2F, PC_SCRIPT_GEORGIAN},
        {0x1200, 0x139F, PC_SCRIPT_ETHIOPIC},   {0x2D80, 0x2DDF, PC_SCRIPT_ETHIOPIC},
        {0xAB00, 0xAB2F, PC_SCRIPT_ETHIOPIC},   {0x0900, 0x097F, PC_SCRIPT_DEVANAGARI},
        {0xA8E0, 0xA8FF, PC_SCRIPT_DEVANAGARI}, {0x0980, 0x09FF, PC_SCRIPT_BENGALI},
        {0x0A00, 0x0A7F, PC_SCRIPT_GURMUKHI},   {0x0A80, 0x0AFF, PC_SCRIPT_GUJARATI},
        {0x0B00, 0x0B7F, PC_SCRIPT_ORIYA},      {0x0B80, 0x0BFF, PC_SCRIPT_TAMIL},
        {0x0C00, 0x0C7F, PC_SCRIPT_TELUGU},     {0x0C80, 0x0CFF, PC_SCRIPT_KANNADA},
        {0x0D00, 0x0D7F, PC_SCRIPT_MALAYALAM},  {0x0D80, 0x0DFF, PC_SCRIPT_SINHALA},
    };
    size_t i;
    for (i = 0; i < sizeof R / sizeof R[0]; i++) {
        if (c >= R[i].a && c <= R[i].b) {
            *out = R[i].s;
            return true;
        }
    }
    return false;
}

/* Path::components ordering (RootDir < CurDir < ParentDir < Normal; Normal byte-wise; repeated
 * slashes and inner "." ignored). */
static int component_kind(const char *p, size_t len, bool first)
{
    if (len == 1 && p[0] == '.') {
        return first ? 1 : -1; /* inner "." is skipped */
    }
    if (len == 2 && p[0] == '.' && p[1] == '.') {
        return 2;
    }
    return 3;
}

static bool next_component(const char *s, size_t *at, bool *first, const char **c, size_t *len, int *kind)
{
    for (;;) {
        size_t a = *at, b;
        if (*first && s[a] == '/') {
            while (s[*at] == '/') {
                (*at)++;
            }
            *first = false;
            *c = s + a;
            *len = 1;
            *kind = 0;
            return true;
        }
        while (s[a] == '/') {
            a++;
        }
        if (s[a] == '\0') {
            return false;
        }
        for (b = a; s[b] != '\0' && s[b] != '/'; b++) {
        }
        *at = b;
        *kind = component_kind(s + a, b - a, *first);
        *first = false;
        if (*kind < 0) {
            continue;
        }
        *c = s + a;
        *len = b - a;
        return true;
    }
}

static int path_cmp(const char *x, const char *y)
{
    size_t ax = 0, ay = 0;
    bool fx = true, fy = true;
    for (;;) {
        const char *cx, *cy;
        size_t lx, ly;
        int kx, ky;
        bool hx = next_component(x, &ax, &fx, &cx, &lx, &kx), hy = next_component(y, &ay, &fy, &cy, &ly, &ky);
        int r;
        if (!hx || !hy) {
            return hx == hy ? 0 : hx ? 1 : -1;
        }
        if (kx != ky) {
            return kx < ky ? -1 : 1;
        }
        if (kx == 3) {
            size_t m = lx < ly ? lx : ly;
            r = m > 0 ? memcmp(cx, cy, m) : 0;
            if (r != 0) {
                return r < 0 ? -1 : 1;
            }
            if (lx != ly) {
                return lx < ly ? -1 : 1;
            }
        }
    }
}

/* file_key: the squashed base name, the stem and the style rank (0 regular, 1 variable, 2 other
 * weights, 3 bold/italic/condensed); false for non-font files. */
static bool file_key(const char *path, pc_buf *base, pc_buf *stem, int *rank, pc_error *err)
{
    const char *name = strrchr(path, '/'), *dot, *cut;
    size_t nlen, slen, i;
    char rest[256];
    size_t rl = 0;
    static const char *const WORDS[] = {"bold",  "italic", "oblique", "condensed", "narrow",
                                        "black", "heavy",  "thin",    "light"};
    name = name != NULL ? name + 1 : path;
    nlen = strlen(name);
    dot = strrchr(name, '.');
    if (dot == NULL || dot == name || !pc_text_font_path_ext(name, nlen)) {
        return false;
    }
    slen = (size_t)(dot - name);
    for (i = 0; i < nlen;) {
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)name, nlen, &i, &cp)) {
            return false; /* to_str fails */
        }
    }
    if (pc_buf_append(stem, name, slen, err) != PC_OK) {
        return false;
    }
    for (cut = name; cut < dot && *cut != '-' && *cut != '['; cut++) {
    }
    {
        pc_buf sq = PC_BUF_INIT;
        for (i = 0; name + i < cut; i++) {
            if (name[i] != ' ' && name[i] != '_' && pc_buf_append_byte(&sq, (uint8_t)name[i], err) != PC_OK) {
                pc_buf_fini(&sq);
                return false;
            }
        }
        if (pc_text_lower(sq.len > 0 ? (const char *)sq.data : "", sq.len, base, err) != PC_OK) {
            pc_buf_fini(&sq);
            return false;
        }
        pc_buf_fini(&sq);
    }
    for (i = 0; cut + i < dot && rl < sizeof rest - 1; i++) {
        char ch = cut[i];
        rest[rl++] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
    }
    rest[rl] = '\0';
    if (rl == 0 || strcmp(rest, "-regular") == 0) {
        *rank = 0;
    } else if (rest[0] == '[') {
        *rank = 1;
    } else {
        *rank = 2;
        for (i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++) {
            if (strstr(rest, WORDS[i]) != NULL) {
                *rank = 3;
            }
        }
    }
    return true;
}

typedef struct ranked {
    size_t rank;
    int style;
    const char *path;
    size_t pos; /* input position: the reference's sort is stable */
} ranked;

static int cmp_ranked(const void *a, const void *b)
{
    const ranked *x = a, *y = b;
    if (x->rank != y->rank) {
        return x->rank < y->rank ? -1 : 1;
    }
    if (x->style != y->style) {
        return x->style < y->style ? -1 : 1;
    }
    {
        int r = path_cmp(x->path, y->path);
        if (r != 0) {
            return r;
        }
    }
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

pc_status pc_text_script_candidates(pc_text_script s, const char *const *files, size_t n, pc_strlist *out,
                                    pc_error *err)
{
    const char *const *names = pc_text_script_file_names(s);
    ranked *v = NULL;
    size_t m = 0, i, k;
    pc_strlist l = {NULL, 0};
    pc_status st = PC_OK;
    if (names == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "scripts: unknown script");
    }
    if (n > 0 && (v = pc_malloc_array(n, sizeof *v)) == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n && st == PC_OK; i++) {
        pc_buf base = PC_BUF_INIT, stem = PC_BUF_INIT, full = PC_BUF_INIT, low = PC_BUF_INIT;
        int style = 0;
        if (files[i] != NULL && file_key(files[i], &base, &stem, &style, NULL)) {
            /* A name with a hyphen ("lohit-tamil") matches the whole stem. */
            if (pc_text_lower(stem.len > 0 ? (const char *)stem.data : "", stem.len, &low, err) != PC_OK) {
                st = PC_ERR_NOMEM;
            }
            for (k = 0; st == PC_OK && k < low.len; k++) {
                if (low.data[k] != ' ' && low.data[k] != '_') {
                    st = pc_buf_append_byte(&full, low.data[k], err);
                }
            }
            for (k = 0; st == PC_OK && names[k] != NULL; k++) {
                bool is_base = base.len > 0 && strlen(names[k]) == base.len &&
                               memcmp(names[k], base.data, base.len) == 0;
                bool is_full = full.len > 0 && strlen(names[k]) == full.len &&
                               memcmp(names[k], full.data, full.len) == 0;
                if (is_base || is_full) {
                    v[m].rank = k;
                    v[m].style = is_full ? 0 : style;
                    v[m].path = files[i];
                    v[m].pos = m;
                    m++;
                    break;
                }
            }
        }
        pc_buf_fini(&base);
        pc_buf_fini(&stem);
        pc_buf_fini(&full);
        pc_buf_fini(&low);
    }
    if (st == PC_OK && m > 1) {
        qsort(v, m, sizeof *v, cmp_ranked);
    }
    for (i = 0; st == PC_OK && i < m && i < 16; i++) {
        st = pc_strlist_push(&l, v[i].path, strlen(v[i].path), err);
    }
    free(v);
    if (st != PC_OK) {
        pc_strlist_fini(&l);
        return st;
    }
    *out = l;
    return PC_OK;
}

static pthread_once_t files_once = PTHREAD_ONCE_INIT;
static pc_strlist files_list;
static bool files_ok;

static int cmp_path_ptr(const void *a, const void *b)
{
    return path_cmp(*(const char *const *)a, *(const char *const *)b);
}

static void list_files(void)
{
    pc_strlist dirs = {NULL, 0}, files = {NULL, 0};
    size_t i, m = 0;
    pc_status st = pc_text_system_font_dirs(&dirs, NULL);
    for (i = 0; st == PC_OK && i < dirs.n; i++) {
        st = pc_text_collect_font_files(dirs.v[i], &files, NULL);
    }
    pc_strlist_fini(&dirs);
    if (st != PC_OK) {
        pc_strlist_fini(&files);
        return;
    }
    if (files.n > 1) {
        qsort(files.v, files.n, sizeof *files.v, cmp_path_ptr);
    }
    for (i = 0; i < files.n; i++) {
        if (m > 0 && path_cmp(files.v[m - 1], files.v[i]) == 0) {
            free(files.v[i]);
        } else {
            files.v[m++] = files.v[i];
        }
    }
    files.n = m;
    files_list = files;
    files_ok = true;
}

const pc_strlist *pc_text_system_font_files(void)
{
    (void)pthread_once(&files_once, list_files);
    return files_ok ? &files_list : NULL;
}

pc_status pc_text_script_system_candidates(pc_text_script s, pc_strlist *out, pc_error *err)
{
    const pc_strlist *f = pc_text_system_font_files();
    if (f == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory listing font files");
    }
    return pc_text_script_candidates(s, (const char *const *)f->v, f->n, out, err);
}

bool pc_text_covering_face(const uint8_t *bytes, size_t len, uint32_t cp, uint32_t *face)
{
    uint32_t n, i;
    if (bytes == NULL || len < 4) {
        return false;
    }
    n = memcmp(bytes, "ttcf", 4) == 0 ? pc_sfnt_face_count(bytes, len) : 1;
    for (i = 0; i < n; i++) {
        pc_sfnt f;
        pc_sfnt_cmap cm;
        if (!pc_sfnt_open(bytes, len, i, &f)) {
            if (memcmp(bytes, "ttcf", 4) != 0) {
                return false;
            }
            continue;
        }
        pc_sfnt_cmap_skrifa(&f, &cm);
        if (cm.sub != NULL && pc_sfnt_cmap_map(&cm, cp) != 0) {
            *face = i;
            return true;
        }
    }
    return false;
}
