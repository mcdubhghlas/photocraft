/* CJK font fallback (port of crates/text/src/cjk.rs; see cjk.h). */
#include "text/cjk.h"

#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/file.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "text/sfnt.h"

pc_cjk_char pc_cjk_classify(uint32_t u)
{
    if ((u >= 0x1100 && u <= 0x11FF) || (u >= 0x3130 && u <= 0x318F) || (u >= 0xA960 && u <= 0xA97F) ||
        (u >= 0xAC00 && u <= 0xD7AF) || (u >= 0xD7B0 && u <= 0xD7FF) || (u >= 0xFFA0 && u <= 0xFFDC)) {
        return PC_CJK_HANGUL;
    }
    if ((u >= 0x3040 && u <= 0x30FF) || (u >= 0x31F0 && u <= 0x31FF) || (u >= 0xFF65 && u <= 0xFF9F) ||
        (u >= 0x1B000 && u <= 0x1B16F)) {
        return PC_CJK_KANA;
    }
    if ((u >= 0x3100 && u <= 0x312F) || (u >= 0x31A0 && u <= 0x31BF)) {
        return PC_CJK_BOPOMOFO;
    }
    if ((u >= 0x2E80 && u <= 0x2FDF) || (u >= 0x3000 && u <= 0x303F) || (u >= 0x3190 && u <= 0x319F) ||
        (u >= 0x31C0 && u <= 0x31EF) || (u >= 0x3200 && u <= 0x33FF) || (u >= 0x3400 && u <= 0x4DBF) ||
        (u >= 0x4E00 && u <= 0x9FFF) || (u >= 0xF900 && u <= 0xFAFF) || (u >= 0xFE30 && u <= 0xFE4F) ||
        (u >= 0xFF00 && u <= 0xFF64) || (u >= 0xFFE0 && u <= 0xFFEF) || (u >= 0x20000 && u <= 0x3FFFF)) {
        return PC_CJK_HAN;
    }
    return PC_CJK_NONE;
}

pc_cjk_script pc_cjk_preferred(pc_cjk_char c, const pc_cjk_script order[4])
{
    switch (c) {
    case PC_CJK_KANA:
        return PC_CJK_JAPANESE;
    case PC_CJK_HANGUL:
        return PC_CJK_KOREAN;
    case PC_CJK_BOPOMOFO:
        return PC_CJK_TRADITIONAL_CHINESE;
    default:
        return order[0];
    }
}

static void set4(pc_cjk_script out[4], pc_cjk_script a, pc_cjk_script b, pc_cjk_script c, pc_cjk_script d)
{
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

static bool part_is(const char *p, size_t n, const char *w)
{
    return strlen(w) == n && memcmp(p, w, n) == 0;
}

void pc_cjk_script_order(const char *locale, pc_cjk_script out[4])
{
    char tag[64];
    size_t n = 0, i, lang_len;
    const char *s = locale != NULL ? locale : "";
    bool hant = false, hans = false;
    /* tag = everything before '.' or '@', ASCII-lowercased, '_' -> '-'. Long tags are cut; only
     * the language and short subtags matter. */
    for (i = 0; s[i] != '\0' && s[i] != '.' && s[i] != '@' && n + 1 < sizeof tag; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        tag[n++] = c == '_' ? '-' : c;
    }
    tag[n] = '\0';
    for (lang_len = 0; lang_len < n && tag[lang_len] != '-'; lang_len++) {
    }
    if (part_is(tag, lang_len, "ja")) {
        set4(out, PC_CJK_JAPANESE, PC_CJK_SIMPLIFIED_CHINESE, PC_CJK_TRADITIONAL_CHINESE, PC_CJK_KOREAN);
        return;
    }
    if (part_is(tag, lang_len, "ko")) {
        set4(out, PC_CJK_KOREAN, PC_CJK_TRADITIONAL_CHINESE, PC_CJK_SIMPLIFIED_CHINESE, PC_CJK_JAPANESE);
        return;
    }
    if (part_is(tag, lang_len, "zh") || part_is(tag, lang_len, "yue")) {
        size_t at = lang_len;
        bool yue = part_is(tag, lang_len, "yue");
        while (at < n) {
            size_t st = at + 1, e = st;
            while (e < n && tag[e] != '-') {
                e++;
            }
            if (part_is(tag + st, e - st, "hant") || part_is(tag + st, e - st, "tw") ||
                part_is(tag + st, e - st, "hk") || part_is(tag + st, e - st, "mo")) {
                hant = true;
            }
            if (part_is(tag + st, e - st, "hans") || part_is(tag + st, e - st, "cn") ||
                part_is(tag + st, e - st, "sg")) {
                hans = true;
            }
            at = e;
        }
        if ((hant && !hans) || (yue && !hans)) {
            set4(out, PC_CJK_TRADITIONAL_CHINESE, PC_CJK_SIMPLIFIED_CHINESE, PC_CJK_JAPANESE, PC_CJK_KOREAN);
        } else {
            set4(out, PC_CJK_SIMPLIFIED_CHINESE, PC_CJK_TRADITIONAL_CHINESE, PC_CJK_JAPANESE, PC_CJK_KOREAN);
        }
        return;
    }
    set4(out, PC_CJK_SIMPLIFIED_CHINESE, PC_CJK_TRADITIONAL_CHINESE, PC_CJK_JAPANESE, PC_CJK_KOREAN);
}

/* ------------------------------------------------------------------------------------------ */
/* UI locale */

static pthread_once_t locale_once = PTHREAD_ONCE_INIT;
static char *locale_value;

/* The trimmed value of an environment variable, unless empty / C / POSIX / C.* (malloc'd). */
static char *env_locale(const char *k)
{
    const char *v = getenv(k);
    size_t start, len;
    if (v == NULL) {
        return NULL;
    }
    len = pc_utf8_trim(v, strlen(v), &start);
    if (len == 0 || (len == 1 && v[start] == 'C') || (len == 5 && memcmp(v + start, "POSIX", 5) == 0) ||
        (len >= 2 && v[start] == 'C' && v[start + 1] == '.')) {
        return NULL;
    }
    return pc_strndup(v + start, len);
}

#ifdef __APPLE__
static char *os_locale(void)
{
    const char *home = getenv("HOME");
    pc_buf path = PC_BUF_INIT, bytes = PC_BUF_INIT, lang = PC_BUF_INIT;
    char *res = NULL;
    if (home == NULL ||
        pc_buf_appendf(&path, NULL, "%s/Library/Preferences/.GlobalPreferences.plist", home) != PC_OK) {
        pc_buf_fini(&path);
        return NULL;
    }
    if (pc_file_read(pc_buf_cstr(&path), (size_t)4 << 20, &bytes, NULL) == PC_OK &&
        pc_cjk_bplist_first_language(bytes.data, bytes.len, &lang)) {
        res = pc_strndup(pc_buf_cstr(&lang), lang.len);
    }
    pc_buf_fini(&path);
    pc_buf_fini(&bytes);
    pc_buf_fini(&lang);
    return res;
}
#else
static char *os_locale(void)
{
    return NULL;
}
#endif

static void detect_locale(void)
{
    static const char *const KEYS[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    size_t i;
    char *v = env_locale("PHOTOCRAFT_LOCALE");
    if (v == NULL) {
        v = os_locale();
    }
    for (i = 0; v == NULL && i < sizeof KEYS / sizeof KEYS[0]; i++) {
        v = env_locale(KEYS[i]);
        if (v != NULL) {
            char *colon = strchr(v, ':');
            if (colon != NULL) {
                *colon = '\0';
            }
        }
    }
    locale_value = v;
}

const char *pc_cjk_ui_locale(void)
{
    (void)pthread_once(&locale_once, detect_locale);
    return locale_value;
}

void pc_cjk_ui_script_order(pc_cjk_script out[4])
{
    pc_cjk_script_order(pc_cjk_ui_locale(), out);
}

/* ------------------------------------------------------------------------------------------ */
/* Binary property list (bplist00): just enough to read AppleLanguages[0]. */

typedef struct bpl {
    const uint8_t *b;
    size_t len, nobj, table, osz, rsz;
} bpl;

static bool be_n(const bpl *p, size_t at, size_t n, uint64_t *v)
{
    uint64_t a = 0;
    size_t i;
    if (at > p->len || p->len - at < n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        a = (a << 8) | p->b[at + i];
    }
    *v = a;
    return true;
}

static bool to_size(uint64_t v, size_t *out)
{
    if (v > SIZE_MAX) {
        return false;
    }
    *out = (size_t)v;
    return true;
}

static bool obj_offset(const bpl *p, size_t obj, size_t *out)
{
    size_t at;
    uint64_t v;
    if (obj >= p->nobj || !pc_size_mul(obj, p->osz, &at) || !pc_size_add(p->table, at, &at) ||
        !be_n(p, at, p->osz, &v)) {
        return false;
    }
    return to_size(v, out);
}

/* (marker high nibble, count, start of payload) */
static bool header(const bpl *p, size_t obj, unsigned *kind, size_t *count, size_t *start)
{
    size_t at, n;
    uint8_t m, im;
    uint64_t v;
    if (!obj_offset(p, obj, &at) || at >= p->len) {
        return false;
    }
    m = p->b[at];
    *kind = m >> 4;
    if ((m & 0x0F) != 0x0F || !(*kind == 0x5 || *kind == 0x6 || *kind == 0xA || *kind == 0xD)) {
        *count = m & 0x0F;
        *start = at + 1;
        return true;
    }
    if (at + 1 >= p->len) {
        return false;
    }
    im = p->b[at + 1];
    if ((im >> 4) != 0x1) {
        return false;
    }
    /* A count of 1 << (im & 15) bytes, folded into 64 bits as the reference does (bytes above
     * the low eight are shifted out). */
    n = (size_t)1 << (im & 0x0F);
    if (at + 2 > p->len || p->len - (at + 2) < n) {
        return false;
    }
    {
        uint64_t a = 0;
        size_t i;
        for (i = 0; i < n; i++) {
            a = (a << 8) | p->b[at + 2 + i];
        }
        v = a;
    }
    if (!to_size(v, count)) {
        return false;
    }
    *start = at + 2 + n;
    return true;
}

static bool string_obj(const bpl *p, size_t obj, pc_buf *out)
{
    unsigned kind;
    size_t n, start, bytes;
    if (!header(p, obj, &kind, &n, &start)) {
        return false;
    }
    if (kind == 0x5) {
        if (start > p->len || p->len - start < n) {
            return false;
        }
        return pc_utf8_lossy(p->b + start, n, out, NULL) == PC_OK;
    }
    if (kind == 0x6) {
        if (!pc_size_mul(n, 2, &bytes) || start > p->len || p->len - start < bytes) {
            return false;
        }
        return pc_utf16be_lossy(p->b + start, n, out, NULL) == PC_OK;
    }
    return false;
}

static bool obj_ref(const bpl *p, size_t at, size_t *out)
{
    uint64_t v;
    return be_n(p, at, p->rsz, &v) && to_size(v, out);
}

bool pc_cjk_bplist_first_language(const uint8_t *b, size_t len, pc_buf *out)
{
    bpl p;
    uint64_t v;
    size_t top, i, n, start;
    unsigned kind;
    pc_buf key = PC_BUF_INIT;
    if (b == NULL || len < 40 || memcmp(b, "bplist00", 8) != 0) {
        return false;
    }
    p.b = b;
    p.len = len;
    p.osz = b[len - 32 + 6];
    p.rsz = b[len - 32 + 7];
    if (!be_n(&p, len - 32 + 8, 8, &v) || !to_size(v, &p.nobj) || !be_n(&p, len - 32 + 16, 8, &v) ||
        !to_size(v, &top) || !be_n(&p, len - 32 + 24, 8, &v) || !to_size(v, &p.table)) {
        return false;
    }
    if (p.osz < 1 || p.osz > 8 || p.rsz < 1 || p.rsz > 8 || p.nobj > len) {
        return false;
    }
    if (!header(&p, top, &kind, &n, &start) || kind != 0xD) {
        return false;
    }
    for (i = 0; i < n; i++) {
        size_t at, k, val, a_n, a_start, first;
        unsigned akind;
        bool is_key;
        if (!pc_size_mul(i, p.rsz, &at) || !pc_size_add(start, at, &at) || !obj_ref(&p, at, &k)) {
            pc_buf_fini(&key);
            return false;
        }
        pc_buf_truncate(&key, 0);
        is_key = string_obj(&p, k, &key) && key.len == 14 && memcmp(key.data, "AppleLanguages", 14) == 0;
        if (!is_key) {
            continue;
        }
        pc_buf_fini(&key);
        if (!pc_size_add(n, i, &at) || !pc_size_mul(at, p.rsz, &at) || !pc_size_add(start, at, &at) ||
            !obj_ref(&p, at, &val) || !header(&p, val, &akind, &a_n, &a_start) || akind != 0xA || a_n == 0 ||
            !obj_ref(&p, a_start, &first)) {
            return false;
        }
        {
            size_t keep = out->len;
            if (!string_obj(&p, first, out) || out->len == keep) {
                pc_buf_truncate(out, keep);
                return false;
            }
        }
        return true;
    }
    pc_buf_fini(&key);
    return false;
}

/* ------------------------------------------------------------------------------------------ */
/* Families and files */

static const char *const JA[] = {"Hiragino Sans",    "Hiragino Kaku Gothic ProN",
                                 "Yu Gothic",        "Meiryo",
                                 "Noto Sans CJK JP", "Noto Sans JP",
                                 "IPAexGothic",      NULL};
static const char *const SC[] = {"PingFang SC",       "Hiragino Sans GB", "Microsoft YaHei",
                                 "Noto Sans CJK SC",  "Noto Sans SC",     "WenQuanYi Micro Hei",
                                 "WenQuanYi Zen Hei", "Heiti SC",         NULL};
static const char *const TC[] = {
    "PingFang TC", "Microsoft JhengHei", "Noto Sans CJK TC", "Noto Sans TC", "Hiragino Sans CNS", "Heiti TC",
    NULL};
static const char *const KO[] = {"Apple SD Gothic Neo",
                                 "Malgun Gothic",
                                 "Noto Sans CJK KR",
                                 "Noto Sans KR",
                                 "NanumGothic",
                                 "UnDotum",
                                 NULL};

const char *const *pc_cjk_families(pc_cjk_script s)
{
    switch (s) {
    case PC_CJK_JAPANESE:
        return JA;
    case PC_CJK_SIMPLIFIED_CHINESE:
        return SC;
    case PC_CJK_TRADITIONAL_CHINESE:
        return TC;
    default:
        return KO;
    }
}

void pc_cjk_font_files_free(pc_cjk_font_file *f, size_t n)
{
    size_t i;
    if (f == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(f[i].path);
    }
    free(f);
}

typedef struct filelist {
    pc_cjk_font_file *v;
    size_t n, cap;
    bool nomem;
} filelist;

static void add_file(filelist *l, const char *path, const char *family)
{
    char *p;
    if (l->nomem || path == NULL) {
        return;
    }
    if (l->n == l->cap) {
        pc_cjk_font_file *g = pc_grow_array(l->v, &l->cap, l->n + 1, sizeof *g);
        if (g == NULL) {
            l->nomem = true;
            return;
        }
        l->v = g;
    }
    p = pc_strndup(path, strlen(path));
    if (p == NULL) {
        l->nomem = true;
        return;
    }
    l->v[l->n].path = p;
    l->v[l->n].family = family;
    l->n++;
}

#if defined(__linux__)
/* Flatpak mounts the host's /usr/share/fonts at /run/host/fonts: each such file is followed by
 * its mirror, keeping the face and preference order. */
static void add_with_host(filelist *l, const char *path, const char *family)
{
    static const char PREFIX[] = "/usr/share/fonts";
    size_t n = sizeof PREFIX - 1;
    add_file(l, path, family);
    if (strncmp(path, PREFIX, n) == 0 && (path[n] == '/' || path[n] == '\0')) {
        pc_buf b = PC_BUF_INIT;
        const char *rest = path + n;
        while (*rest == '/') {
            rest++;
        }
        if (pc_buf_appendf(&b, NULL, "/run/host/fonts%s%s", *rest != '\0' ? "/" : "", rest) == PC_OK) {
            add_file(l, pc_buf_cstr(&b), family);
        } else {
            l->nomem = true;
        }
        pc_buf_fini(&b);
    }
}
#else
static void add_with_host(filelist *l, const char *path, const char *family)
{
    add_file(l, path, family);
}
#endif

static pc_status finish(filelist *l, pc_cjk_font_file **out, size_t *n, pc_error *err)
{
    if (l->nomem) {
        pc_cjk_font_files_free(l->v, l->n);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = l->v;
    *n = l->n;
    return PC_OK;
}

pc_status pc_cjk_font_files(pc_cjk_script s, pc_cjk_font_file **out, size_t *n, pc_error *err)
{
    filelist l = {NULL, 0, 0, false};
#ifdef __APPLE__
    char *pf = pc_cjk_mac_pingfang();
    switch (s) {
    case PC_CJK_JAPANESE:
        add_file(&l, "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc", "");
        add_file(&l, "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc", "");
        break;
    case PC_CJK_SIMPLIFIED_CHINESE:
        add_file(&l, pf, "PingFang SC");
        add_file(&l, "/System/Library/Fonts/Hiragino Sans GB.ttc", "");
        add_file(&l, "/System/Library/Fonts/STHeiti Light.ttc", "Heiti SC");
        break;
    case PC_CJK_TRADITIONAL_CHINESE:
        add_file(&l, pf, "PingFang TC");
        add_file(&l, "/System/Library/Fonts/STHeiti Light.ttc", "Heiti TC");
        break;
    default:
        add_file(&l, "/System/Library/Fonts/AppleSDGothicNeo.ttc", "Apple SD Gothic Neo");
        break;
    }
    free(pf);
#else
    static const char *const NOTO[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        /* FreeBSD ports (x11-fonts/noto-sans-cjk and friends) install under /usr/local. */
        "/usr/local/share/fonts/noto/NotoSansCJK-Regular.ttc",
        "/usr/local/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/local/share/fonts/noto/NotoSansCJK-VF.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-sans-cjk-fonts/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-sans-cjk-vf-fonts/NotoSansCJK-VF.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-VF.ttc",
    };
    static const char *const JA_FILES[] = {"/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf",
                                           "/usr/share/fonts/truetype/fonts-japanese-gothic.ttf",
                                           "/usr/share/fonts/truetype/takao-gothic/TakaoPGothic.ttf", NULL};
    static const char *const ZH_FILES[] = {"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
                                           "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
                                           "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                                           "/usr/share/fonts/wenquanyi/wqy-zenhei/wqy-zenhei.ttc", NULL};
    static const char *const KO_FILES[] = {
        "/usr/share/fonts/truetype/nanum/NanumGothic.ttf", "/usr/share/fonts/nanum/NanumGothic.ttf",
        "/usr/share/fonts/truetype/unfonts-core/UnDotum.ttf", "/usr/share/fonts/un-core/UnDotum.ttf", NULL};
    const char *noto = s == PC_CJK_JAPANESE              ? "Noto Sans CJK JP"
                       : s == PC_CJK_SIMPLIFIED_CHINESE  ? "Noto Sans CJK SC"
                       : s == PC_CJK_TRADITIONAL_CHINESE ? "Noto Sans CJK TC"
                                                         : "Noto Sans CJK KR";
    const char *const *extra = s == PC_CJK_JAPANESE ? JA_FILES : s == PC_CJK_KOREAN ? KO_FILES : ZH_FILES;
    size_t i;
    for (i = 0; i < sizeof NOTO / sizeof NOTO[0]; i++) {
        add_with_host(&l, NOTO[i], noto);
    }
    for (i = 0; extra[i] != NULL; i++) {
        add_with_host(&l, extra[i], "");
    }
#endif
    return finish(&l, out, n, err);
}

pc_status pc_cjk_last_resort_files(pc_cjk_font_file **out, size_t *n, pc_error *err)
{
    filelist l = {NULL, 0, 0, false};
#ifdef __APPLE__
    add_file(&l, "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "");
    add_file(&l, "/Library/Fonts/Arial Unicode.ttf", "");
#else
    add_with_host(&l, "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", "");
    add_with_host(&l, "/usr/share/fonts/google-droid-sans-fonts/DroidSansFallbackFull.ttf", "");
#endif
    return finish(&l, out, n, err);
}

static bool is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

char *pc_cjk_mac_pingfang(void)
{
    static const char *const ROOTS[] = {"/System/Library/AssetsV2",
                                        "/System/Library/AssetsV2/PreinstalledAssetsV2/InstallWithOs"};
    size_t r;
    if (is_file("/System/Library/Fonts/PingFang.ttc")) {
        return pc_strndup("/System/Library/Fonts/PingFang.ttc", 34);
    }
    for (r = 0; r < 2; r++) {
        DIR *d = opendir(ROOTS[r]);
        struct dirent *e;
        size_t seen = 0;
        if (d == NULL) {
            continue;
        }
        while (seen < 512 && (e = readdir(d)) != NULL) {
            pc_buf sub = PC_BUF_INIT;
            DIR *a;
            struct dirent *ae;
            size_t aseen = 0;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            seen++;
            if (strncmp(e->d_name, "com_apple_MobileAsset_Font", 26) != 0 ||
                pc_buf_appendf(&sub, NULL, "%s/%s", ROOTS[r], e->d_name) != PC_OK) {
                pc_buf_fini(&sub);
                continue;
            }
            a = opendir(pc_buf_cstr(&sub));
            while (a != NULL && aseen < 4096 && (ae = readdir(a)) != NULL) {
                pc_buf p = PC_BUF_INIT;
                if (strcmp(ae->d_name, ".") == 0 || strcmp(ae->d_name, "..") == 0) {
                    continue;
                }
                aseen++;
                if (pc_buf_appendf(&p, NULL, "%s/%s/AssetData/PingFang.ttc", pc_buf_cstr(&sub), ae->d_name) ==
                        PC_OK &&
                    is_file(pc_buf_cstr(&p))) {
                    char *res = pc_strndup(pc_buf_cstr(&p), p.len);
                    pc_buf_fini(&p);
                    closedir(a);
                    pc_buf_fini(&sub);
                    closedir(d);
                    return res;
                }
                pc_buf_fini(&p);
            }
            if (a != NULL) {
                closedir(a);
            }
            pc_buf_fini(&sub);
        }
        closedir(d);
    }
    return NULL;
}

typedef struct match_ctx {
    const char *family;
    size_t len;
} match_ctx;

static bool name_matches(void *ctx, const char *s, size_t len)
{
    const match_ctx *m = ctx;
    return len == m->len && memcmp(s, m->family, len) == 0;
}

uint32_t pc_cjk_face_index_for_family(const uint8_t *bytes, size_t len, const char *family)
{
    uint32_t n, i;
    match_ctx m;
    if (family == NULL || family[0] == '\0' || bytes == NULL || len < 12 || memcmp(bytes, "ttcf", 4) != 0) {
        return 0;
    }
    m.family = family;
    m.len = strlen(family);
    n = pc_sfnt_face_count(bytes, len);
    for (i = 0; i < n; i++) {
        pc_sfnt f;
        if (!pc_sfnt_open(bytes, len, i, &f)) {
            continue;
        }
        if (pc_sfnt_each_name(&f, PC_SFNT_NAME_TYPOGRAPHIC_FAMILY, name_matches, &m) ||
            pc_sfnt_each_name(&f, PC_SFNT_NAME_FAMILY, name_matches, &m)) {
            return i;
        }
    }
    return 0;
}
