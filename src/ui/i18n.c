/* UI localisation (port of crates/ui-egui/src/i18n/). */
#include "ui/i18n.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- plural rules (i18n/mod.rs) ---- */
static size_t plural_one_other(uint64_t n) { return n != 1 ? 1 : 0; }
static size_t plural_none(uint64_t n) { (void)n; return 0; }
static size_t plural_russian(uint64_t n)
{
    uint64_t u = n % 10, t = n % 100;
    if (u == 1 && !(t >= 11 && t <= 19)) { return 0; }
    if (u >= 2 && u <= 4 && !(t >= 11 && t <= 19)) { return 1; }
    return 2;
}
static size_t plural_cs(uint64_t n) { return n == 1 ? 0 : (n >= 2 && n <= 4 ? 1 : 2); }
static size_t plural_fr(uint64_t n) { return n > 1 ? 1 : 0; }
static size_t plural_pt(uint64_t n) { return n > 1 ? 1 : 0; }
static size_t plural_polish(uint64_t n)
{
    uint64_t last = n % 10, last_two = n % 100;
    if (n == 1) { return 0; }
    if (last >= 2 && last <= 4 && !(last_two >= 12 && last_two <= 14)) { return 1; }
    return 2;
}

/* ---- catalog (i18n/catalog.rs) ---- */
typedef struct {
    char *key;
    char *val;
    size_t order;
} pc_i18n_entry;

typedef struct pc_i18n_catalog {
    pc_i18n_entry *plain;
    size_t n_plain;
    pc_i18n_entry *ctx;
    size_t n_ctx;
    pc_i18n_entry *ids;
    size_t n_ids;
    pc_i18n_entry *plurals; /* key "one|other", val the whole translation */
    size_t n_plurals;
} pc_i18n_catalog;

struct pc_i18n_lang {
    const char *code;
    const char *name;
    const unsigned char *source; /* catalog bytes (NULL for English) */
    size_t source_len;
    size_t (*plural)(uint64_t);
    bool complete_menus;
    pc_i18n_catalog *cat; /* lazily parsed, process-lifetime cache */
    bool parsed;
};

/* Embedded catalogs (Makefile GEN rules; '-' in a code becomes '_'). */
#define PC_I18N_CAT(id) extern const unsigned char pc_i18n_cat_##id[]; extern const size_t pc_i18n_cat_##id##_len
PC_I18N_CAT(ja); PC_I18N_CAT(zh_hans); PC_I18N_CAT(zh_hant); PC_I18N_CAT(es); PC_I18N_CAT(ru);
PC_I18N_CAT(uk); PC_I18N_CAT(sr); PC_I18N_CAT(cs); PC_I18N_CAT(fr); PC_I18N_CAT(id);
PC_I18N_CAT(ko); PC_I18N_CAT(pl); PC_I18N_CAT(de); PC_I18N_CAT(pt_br); PC_I18N_CAT(el);
PC_I18N_CAT(nl); PC_I18N_CAT(it); PC_I18N_CAT(hu); PC_I18N_CAT(tr); PC_I18N_CAT(vi);
#undef PC_I18N_CAT

static struct pc_i18n_lang LANGUAGES[] = {
    {"en", "English", NULL, 0, plural_one_other, false, NULL, false},
    {"ja", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", pc_i18n_cat_ja, 0, plural_none, true, NULL, false},
    {"zh-hans", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87", pc_i18n_cat_zh_hans, 0, plural_none, true, NULL, false},
    {"zh-hant", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87", pc_i18n_cat_zh_hant, 0, plural_none, true, NULL, false},
    {"es", "Espa\xc3\xb1ol", pc_i18n_cat_es, 0, plural_one_other, true, NULL, false},
    {"ru", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", pc_i18n_cat_ru, 0, plural_russian, true, NULL, false},
    {"uk", "\xd0\xa3\xd0\xba\xd1\x80\xd0\xb0\xd1\x97\xd0\xbd\xd1\x81\xd1\x8c\xd0\xba\xd0\xb0", pc_i18n_cat_uk, 0, plural_russian, true, NULL, false},
    {"sr", "Srpski", pc_i18n_cat_sr, 0, plural_russian, true, NULL, false},
    {"cs", "\xc4\x8c" "e\xc5\xa1tina", pc_i18n_cat_cs, 0, plural_cs, true, NULL, false},
    {"fr", "Fran\xc3\xa7" "ais", pc_i18n_cat_fr, 0, plural_fr, true, NULL, false},
    {"id", "Bahasa Indonesia", pc_i18n_cat_id, 0, plural_none, true, NULL, false},
    {"ko", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4", pc_i18n_cat_ko, 0, plural_none, true, NULL, false},
    {"pl", "Polski", pc_i18n_cat_pl, 0, plural_polish, true, NULL, false},
    {"de", "Deutsch", pc_i18n_cat_de, 0, plural_one_other, true, NULL, false},
    {"pt-br", "Portugu\xc3\xaas (Brasil)", pc_i18n_cat_pt_br, 0, plural_pt, true, NULL, false},
    {"el", "\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac", pc_i18n_cat_el, 0, plural_one_other, true, NULL, false},
    {"nl", "Nederlands", pc_i18n_cat_nl, 0, plural_one_other, true, NULL, false},
    {"it", "Italiano", pc_i18n_cat_it, 0, plural_one_other, true, NULL, false},
    {"hu", "Magyar", pc_i18n_cat_hu, 0, plural_none, true, NULL, false},
    {"tr", "T\xc3\xbcrk\xc3\xa7" "e", pc_i18n_cat_tr, 0, plural_none, true, NULL, false},
    {"vi", "Ti\xe1\xba\xbfng Vi\xe1\xbb\x87t", pc_i18n_cat_vi, 0, plural_none, true, NULL, false},
};
#define LANG_COUNT (sizeof LANGUAGES / sizeof LANGUAGES[0])

static pthread_mutex_t g_cat_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_len_once = PTHREAD_ONCE_INIT;

static void init_source_lengths(void)
{
    static const size_t *const LENS[] = {
        NULL, &pc_i18n_cat_ja_len, &pc_i18n_cat_zh_hans_len, &pc_i18n_cat_zh_hant_len, &pc_i18n_cat_es_len,
        &pc_i18n_cat_ru_len, &pc_i18n_cat_uk_len, &pc_i18n_cat_sr_len, &pc_i18n_cat_cs_len, &pc_i18n_cat_fr_len,
        &pc_i18n_cat_id_len, &pc_i18n_cat_ko_len, &pc_i18n_cat_pl_len, &pc_i18n_cat_de_len, &pc_i18n_cat_pt_br_len,
        &pc_i18n_cat_el_len, &pc_i18n_cat_nl_len, &pc_i18n_cat_it_len, &pc_i18n_cat_hu_len, &pc_i18n_cat_tr_len,
        &pc_i18n_cat_vi_len,
    };
    for (size_t i = 0; i < LANG_COUNT; i++) {
        LANGUAGES[i].source_len = LENS[i] ? *LENS[i] : 0;
    }
}

const pc_i18n_lang *pc_i18n_en(void) { return &LANGUAGES[0]; }
size_t pc_i18n_count(void) { return LANG_COUNT; }
const pc_i18n_lang *pc_i18n_at(size_t index) { return index < LANG_COUNT ? &LANGUAGES[index] : NULL; }
const char *pc_i18n_lang_code(const pc_i18n_lang *lang) { return lang->code; }
const char *pc_i18n_lang_name(const pc_i18n_lang *lang) { return lang->name; }
bool pc_i18n_lang_complete_menus(const pc_i18n_lang *lang) { return lang->complete_menus; }
size_t pc_i18n_lang_plural(const pc_i18n_lang *lang, uint64_t n) { return lang->plural(n); }
const unsigned char *pc_i18n_lang_source(const pc_i18n_lang *lang, size_t *len)
{
    pthread_once(&g_len_once, init_source_lengths);
    if (len) { *len = lang->source_len; }
    return lang->source;
}

/* ---- direction ---- */
static const char *const RTL[] = {"ar", "ckb", "dv", "fa", "he", "ps", "sd", "ug", "ur", "yi"};

static size_t primary_len(const char *code)
{
    size_t i = 0;
    while (code[i] && code[i] != '-' && code[i] != '_') { i++; }
    return i;
}

static bool ieq_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') { ca = (unsigned char)(ca - 'A' + 'a'); }
        if (cb >= 'A' && cb <= 'Z') { cb = (unsigned char)(cb - 'A' + 'a'); }
        if (ca != cb) { return false; }
    }
    return true;
}

pc_i18n_dir pc_i18n_dir_for_code(const char *code)
{
    if (!code) { return PC_I18N_LTR; }
    size_t pl = primary_len(code);
    for (size_t i = 0; i < sizeof RTL / sizeof RTL[0]; i++) {
        if (strlen(RTL[i]) == pl && ieq_n(code, RTL[i], pl)) { return PC_I18N_RTL; }
    }
    return PC_I18N_LTR;
}

pc_i18n_dir pc_i18n_lang_dir(const pc_i18n_lang *lang) { return pc_i18n_dir_for_code(lang->code); }

static bool ieq(const char *a, const char *b)
{
    return strlen(a) == strlen(b) && ieq_n(a, b, strlen(a));
}

const pc_i18n_lang *pc_i18n_from_code(const char *code)
{
    if (!code) { return NULL; }
    for (size_t i = 0; i < LANG_COUNT; i++) {
        if (ieq(LANGUAGES[i].code, code)) { return &LANGUAGES[i]; }
    }
    return NULL;
}

/* ---- candidates / tags ---- */
size_t pc_i18n_candidates(const char *tag, char out[][32], size_t cap)
{
    if (!tag || cap == 0) { return 0; }
    /* base: up to '.'/'@', '_'->'-', lowercased. */
    char base[128];
    size_t bn = 0;
    for (size_t i = 0; tag[i] && tag[i] != '.' && tag[i] != '@' && bn + 1 < sizeof base; i++) {
        unsigned char c = (unsigned char)tag[i];
        if (c == '_') { c = '-'; }
        if (c >= 'A' && c <= 'Z') { c = (unsigned char)(c - 'A' + 'a'); }
        base[bn++] = (char)c;
    }
    base[bn] = '\0';
    /* split into non-empty parts. */
    char parts[8][32];
    size_t np = 0;
    size_t start = 0;
    for (size_t i = 0; i <= bn; i++) {
        if (base[i] == '-' || base[i] == '\0') {
            size_t plen = i - start;
            if (plen > 0 && np < 8 && plen < 32) {
                memcpy(parts[np], base + start, plen);
                parts[np][plen] = '\0';
                np++;
            }
            start = i + 1;
        }
    }
    if (np == 0) { return 0; }
    /* most specific first: join parts[0..n] for n = np..1. */
    char cands[16][32];
    size_t nc = 0;
    for (size_t n = np; n >= 1 && nc < 16; n--) {
        char joined[32];
        size_t jn = 0;
        for (size_t k = 0; k < n; k++) {
            if (k > 0 && jn + 1 < sizeof joined) { joined[jn++] = '-'; }
            size_t kl = strlen(parts[k]);
            if (jn + kl < sizeof joined) { memcpy(joined + jn, parts[k], kl); jn += kl; }
        }
        joined[jn] = '\0';
        memcpy(cands[nc], joined, jn + 1);
        nc++;
    }
    /* zh without a script: insert region-derived script before the bare primary. */
    bool has_script = false;
    for (size_t k = 0; k < np; k++) {
        if (strcmp(parts[k], "hans") == 0 || strcmp(parts[k], "hant") == 0) { has_script = true; }
    }
    if (strcmp(parts[0], "zh") == 0 && !has_script && nc < 16 && nc >= 1) {
        bool trad = false;
        for (size_t k = 0; k < np; k++) {
            if (strcmp(parts[k], "tw") == 0 || strcmp(parts[k], "hk") == 0 || strcmp(parts[k], "mo") == 0) { trad = true; }
        }
        const char *script = trad ? "zh-hant" : "zh-hans";
        memmove(cands[nc - 1 + 1], cands[nc - 1], sizeof cands[0]); /* shift last down */
        strcpy(cands[nc - 1], script);
        nc++;
    }
    /* pt without pt-br already present: insert before the bare primary. */
    if (strcmp(parts[0], "pt") == 0 && nc < 16) {
        bool have = false;
        for (size_t k = 0; k < nc; k++) {
            if (strcmp(cands[k], "pt-br") == 0) { have = true; }
        }
        if (!have && nc >= 1) {
            memmove(cands[nc], cands[nc - 1], sizeof cands[0]);
            strcpy(cands[nc - 1], "pt-br");
            nc++;
        }
    }
    size_t written = nc < cap ? nc : cap;
    for (size_t i = 0; i < written; i++) {
        size_t cl = strlen(cands[i]);
        memcpy(out[i], cands[i], cl + 1);
    }
    return written;
}

const pc_i18n_lang *pc_i18n_from_tag(const char *tag)
{
    char cands[16][32];
    size_t nc = pc_i18n_candidates(tag, cands, 16);
    if (nc == 0) { return NULL; }
    if (strcmp(cands[0], "c") == 0 || strcmp(cands[0], "posix") == 0) { return pc_i18n_en(); }
    for (size_t i = 0; i < nc; i++) {
        const pc_i18n_lang *l = pc_i18n_from_code(cands[i]);
        if (l) { return l; }
    }
    return NULL;
}

/* ---- system locale ---- */
static _Thread_local char *g_override[64];
static _Thread_local size_t g_override_n;
static _Thread_local bool g_override_set;

void pc_i18n_set_system_tags_for_test(const char *const *tags, size_t n)
{
    for (size_t i = 0; i < g_override_n; i++) { free(g_override[i]); g_override[i] = NULL; }
    g_override_n = 0;
    g_override_set = false;
    if (!tags) { return; }
    g_override_set = true;
    for (size_t i = 0; i < n && g_override_n < 64; i++) {
        size_t len = strlen(tags[i]);
        char *copy = malloc(len + 1);
        if (!copy) { break; }
        memcpy(copy, tags[i], len + 1);
        g_override[g_override_n++] = copy;
    }
}

static const pc_i18n_lang *resolve_tags(const char *const *tags, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const pc_i18n_lang *l = pc_i18n_from_tag(tags[i]);
        if (l) { return l; }
    }
    return pc_i18n_en();
}

const pc_i18n_lang *pc_i18n_system_lang(void)
{
    if (g_override_set) {
        return resolve_tags((const char *const *)g_override, g_override_n);
    }
    const char *forced = getenv("PHOTOCRAFT_LOCALE");
    if (forced && forced[0]) {
        const char *one[1] = {forced};
        return resolve_tags(one, 1);
    }
    /* POSIX locale environment, most specific first. */
    const char *env[4];
    size_t n = 0;
    const char *vars[] = {"LC_ALL", "LC_MESSAGES", "LANG", "LANGUAGE"};
    for (size_t i = 0; i < 4; i++) {
        const char *v = getenv(vars[i]);
        if (v && v[0] && n < 4) { env[n++] = v; }
    }
    return resolve_tags(env, n);
}

const pc_i18n_lang *pc_i18n_from_pref(const char *pref)
{
    if (!pref) { return pc_i18n_system_lang(); }
    if (ieq(pref, "auto")) { return pc_i18n_system_lang(); }
    const pc_i18n_lang *l = pc_i18n_from_code(pref);
    if (l) { return l; }
    l = pc_i18n_from_tag(pref);
    return l ? l : pc_i18n_system_lang();
}

/* ---- catalog parse ---- */
/* unescape \n \t \\; keep \x otherwise (catalog.rs unescape). Writes into dst (<= strlen(src)+1). */
static void unescape(const char *src, size_t len, char *dst)
{
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] != '\\') { dst[j++] = src[i]; continue; }
        if (i + 1 >= len) { dst[j++] = '\\'; break; }
        char o = src[i + 1];
        i++;
        switch (o) {
        case 'n': dst[j++] = '\n'; break;
        case 't': dst[j++] = '\t'; break;
        case '\\': dst[j++] = '\\'; break;
        default: dst[j++] = '\\'; dst[j++] = o; break;
        }
    }
    dst[j] = '\0';
}

static char *dup_unescaped(const char *src, size_t len)
{
    char *d = malloc(len + 1);
    if (d) { unescape(src, len, d); }
    return d;
}

static int entry_cmp(const void *pa, const void *pb)
{
    const pc_i18n_entry *a = pa, *b = pb;
    int c = strcmp(a->key, b->key);
    if (c != 0) { return c; }
    /* equal keys: higher order first, so the winner (last insert) leads each group. */
    if (a->order > b->order) { return -1; }
    if (a->order < b->order) { return 1; }
    return 0;
}

/* Sort by key, keep the last duplicate (highest order), compact in place; returns new count. */
static size_t dedup_sort(pc_i18n_entry *arr, size_t n)
{
    if (n == 0) { return 0; }
    qsort(arr, n, sizeof arr[0], entry_cmp);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (w == 0 || strcmp(arr[i].key, arr[w - 1].key) != 0) {
            arr[w++] = arr[i];
        } else {
            free(arr[i].key);
            free(arr[i].val);
        }
    }
    return w;
}

enum cat_kind { K_PLAIN, K_CTX, K_ID, K_PLURAL };

static enum cat_kind classify(const char *ctx, size_t ctxlen)
{
    if (ctxlen == 0) { return K_PLAIN; }
    if (ctxlen == 3 && memcmp(ctx, "@id", 3) == 0) { return K_ID; }
    if (ctxlen == 7 && memcmp(ctx, "@plural", 7) == 0) { return K_PLURAL; }
    return K_CTX;
}

/* One parsed line: returns true with field pointers/lengths, or false for skip/malformed.
 * *malformed is set true for a malformed (non-comment, non-blank) line. */
static bool parse_line(const char *line, size_t len, const char **ctx, size_t *ctxlen, const char **src,
                       size_t *srclen, const char **tr, size_t *trlen, bool *malformed)
{
    *malformed = false;
    /* trim for blank detection. */
    size_t s = 0, e = len;
    while (s < e && (line[s] == ' ' || line[s] == '\t' || line[s] == '\r')) { s++; }
    while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '\r')) { e--; }
    if (s == e) { return false; } /* blank */
    if (line[s] == '#') { return false; } /* comment */
    /* split the whole line on TAB into exactly 3 fields (a 4th tab is malformed). */
    size_t t1 = (size_t)-1, t2 = (size_t)-1;
    size_t tabs = 0;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == '\t') {
            tabs++;
            if (tabs == 1) { t1 = i; } else if (tabs == 2) { t2 = i; }
        }
    }
    if (tabs != 2) { *malformed = true; return false; }
    *ctx = line;
    *ctxlen = t1;
    *src = line + t1 + 1;
    *srclen = t2 - t1 - 1;
    *tr = line + t2 + 1;
    *trlen = len - t2 - 1;
    if (*srclen == 0 || *trlen == 0) { *malformed = true; return false; }
    return true;
}

static pc_i18n_catalog *parse_catalog(const unsigned char *bytes, size_t len)
{
    pc_i18n_catalog *c = calloc(1, sizeof *c);
    if (!c) { return NULL; }
    /* Pass 1: count per kind. */
    size_t counts[4] = {0, 0, 0, 0};
    const char *text = (const char *)bytes;
    size_t pos = 0;
    while (pos < len) {
        size_t nl = pos;
        while (nl < len && text[nl] != '\n') { nl++; }
        const char *ctx, *src, *tr;
        size_t cl, sl, tl;
        bool bad;
        if (parse_line(text + pos, nl - pos, &ctx, &cl, &src, &sl, &tr, &tl, &bad)) {
            counts[classify(ctx, cl)]++;
        }
        pos = nl + 1;
    }
    pc_i18n_entry *arrs[4] = {NULL, NULL, NULL, NULL};
    for (int k = 0; k < 4; k++) {
        if (counts[k] > 0) {
            arrs[k] = calloc(counts[k], sizeof(pc_i18n_entry));
            if (!arrs[k]) {
                for (int j = 0; j < 4; j++) { free(arrs[j]); }
                free(c);
                return NULL;
            }
        }
    }
    size_t fill[4] = {0, 0, 0, 0};
    size_t order = 0;
    pos = 0;
    while (pos < len) {
        size_t nl = pos;
        while (nl < len && text[nl] != '\n') { nl++; }
        const char *ctx, *src, *tr;
        size_t cl, sl, tl;
        bool bad;
        if (parse_line(text + pos, nl - pos, &ctx, &cl, &src, &sl, &tr, &tl, &bad)) {
            enum cat_kind k = classify(ctx, cl);
            pc_i18n_entry *e = &arrs[k][fill[k]++];
            e->order = order++;
            e->val = dup_unescaped(tr, tl);
            if (k == K_CTX) {
                /* key = ctx \x01 src (both unescaped). */
                char *cs = dup_unescaped(ctx, cl);
                char *ss = dup_unescaped(src, sl);
                if (cs && ss) {
                    size_t kl = strlen(cs) + 1 + strlen(ss);
                    e->key = malloc(kl + 1);
                    if (e->key) {
                        size_t off = strlen(cs);
                        memcpy(e->key, cs, off);
                        e->key[off] = '\x01';
                        memcpy(e->key + off + 1, ss, strlen(ss) + 1);
                    }
                }
                free(cs);
                free(ss);
            } else {
                e->key = dup_unescaped(src, sl);
            }
        }
        pos = nl + 1;
    }
    c->plain = arrs[K_PLAIN];
    c->n_plain = dedup_sort(arrs[K_PLAIN], fill[K_PLAIN]);
    c->ctx = arrs[K_CTX];
    c->n_ctx = dedup_sort(arrs[K_CTX], fill[K_CTX]);
    c->ids = arrs[K_ID];
    c->n_ids = dedup_sort(arrs[K_ID], fill[K_ID]);
    c->plurals = arrs[K_PLURAL];
    c->n_plurals = dedup_sort(arrs[K_PLURAL], fill[K_PLURAL]);
    return c;
}

static const pc_i18n_catalog *catalog_of(const pc_i18n_lang *lang)
{
    pthread_once(&g_len_once, init_source_lengths);
    /* lang points into the static LANGUAGES array; recover the writable element by index. */
    struct pc_i18n_lang *m = &LANGUAGES[(size_t)(lang - LANGUAGES)];
    pthread_mutex_lock(&g_cat_lock);
    if (!m->parsed) {
        m->cat = m->source ? parse_catalog(m->source, m->source_len) : calloc(1, sizeof(pc_i18n_catalog));
        m->parsed = true;
    }
    const pc_i18n_catalog *c = m->cat;
    pthread_mutex_unlock(&g_cat_lock);
    return c;
}

static const char *lookup(const pc_i18n_entry *arr, size_t n, const char *key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(key, arr[mid].key);
        if (c == 0) { return arr[mid].val; }
        if (c < 0) { hi = mid; } else { lo = mid + 1; }
    }
    return NULL;
}

const char *pc_i18n_tr(const pc_i18n_lang *lang, const char *s)
{
    const pc_i18n_catalog *c = catalog_of(lang);
    if (c) {
        const char *v = lookup(c->plain, c->n_plain, s);
        if (v) { return v; }
    }
    return s;
}

bool pc_i18n_has(const pc_i18n_lang *lang, const char *s)
{
    const pc_i18n_catalog *c = catalog_of(lang);
    return c && lookup(c->plain, c->n_plain, s) != NULL;
}

const char *pc_i18n_tr_ctx(const pc_i18n_lang *lang, const char *context, const char *s)
{
    const pc_i18n_catalog *c = catalog_of(lang);
    if (c && c->n_ctx) {
        size_t kl = strlen(context) + 1 + strlen(s);
        char stackbuf[256];
        char *key = kl + 1 <= sizeof stackbuf ? stackbuf : malloc(kl + 1);
        if (key) {
            size_t off = strlen(context);
            memcpy(key, context, off);
            key[off] = '\x01';
            memcpy(key + off + 1, s, strlen(s) + 1);
            const char *v = lookup(c->ctx, c->n_ctx, key);
            if (key != stackbuf) { free(key); }
            if (v) { return v; }
        }
    }
    return pc_i18n_tr(lang, s);
}

const char *pc_i18n_tr_id(const pc_i18n_lang *lang, const char *id, const char *label)
{
    const pc_i18n_catalog *c = catalog_of(lang);
    if (c) {
        const char *v = lookup(c->ids, c->n_ids, id);
        if (v) { return v; }
    }
    return pc_i18n_tr(lang, label);
}

/* ---- current language (thread-local) ---- */
static _Thread_local const pc_i18n_lang *g_current;

void pc_i18n_set_current(const pc_i18n_lang *lang) { g_current = lang; }
const pc_i18n_lang *pc_i18n_current(void) { return g_current ? g_current : pc_i18n_en(); }
const char *pc_i18n_t(const char *s) { return pc_i18n_tr(pc_i18n_current(), s); }

/* ---- fmt / trn ---- */
/* Replace every "{key}" in `in` with `val`, appending to out. */
static pc_status replace_all(const char *in, const char *key, const char *val, pc_buf *out, pc_error *err)
{
    size_t klen = strlen(key);
    char *token = malloc(klen + 3);
    if (!token) { return pc_fail(err, PC_ERR_NOMEM, "out of memory"); }
    token[0] = '{';
    memcpy(token + 1, key, klen);
    token[klen + 1] = '}';
    token[klen + 2] = '\0';
    size_t tlen = klen + 2;
    const char *p = in;
    pc_status st = PC_OK;
    for (;;) {
        const char *hit = strstr(p, token);
        if (!hit) {
            st = pc_buf_append_str(out, p, err);
            break;
        }
        st = pc_buf_append(out, p, (size_t)(hit - p), err);
        if (st != PC_OK) { break; }
        st = pc_buf_append_str(out, val, err);
        if (st != PC_OK) { break; }
        p = hit + tlen;
    }
    free(token);
    return st;
}

pc_status pc_i18n_fmt(const char *template, const char *const *keys, const char *const *vals, size_t n,
                      pc_buf *out, pc_error *err)
{
    pc_buf_truncate(out, 0);
    if (n == 0) {
        return pc_buf_append_str(out, template, err);
    }
    /* sequentially replace, like serde's String::replace chain. */
    pc_buf a = PC_BUF_INIT, b = PC_BUF_INIT;
    pc_status st = pc_buf_append_str(&a, template, err);
    pc_buf *cur = &a, *nxt = &b;
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        pc_buf_truncate(nxt, 0);
        st = replace_all(pc_buf_cstr(cur), keys[i], vals[i], nxt, err);
        pc_buf *tmp = cur;
        cur = nxt;
        nxt = tmp;
    }
    if (st == PC_OK) {
        st = pc_buf_append_str(out, pc_buf_cstr(cur), err);
    }
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    return st;
}

pc_status pc_i18n_trn(const pc_i18n_lang *lang, uint64_t n, const char *one, const char *other, pc_buf *out,
                      pc_error *err)
{
    size_t idx = lang->plural(n);
    const char *text = NULL;
    const pc_i18n_catalog *c = catalog_of(lang);
    if (c && c->n_plurals) {
        size_t kl = strlen(one) + 1 + strlen(other);
        char stackbuf[256];
        char *key = kl + 1 <= sizeof stackbuf ? stackbuf : malloc(kl + 1);
        if (key) {
            size_t off = strlen(one);
            memcpy(key, one, off);
            key[off] = '|';
            memcpy(key + off + 1, other, strlen(other) + 1);
            const char *raw = lookup(c->plurals, c->n_plurals, key);
            if (key != stackbuf) { free(key); }
            if (raw) {
                /* split on '|', pick min(idx, forms-1). */
                size_t forms = 1;
                for (const char *q = raw; *q; q++) {
                    if (*q == '|') { forms++; }
                }
                size_t pick = idx < forms ? idx : forms - 1;
                const char *start = raw;
                size_t cur = 0;
                while (cur < pick) {
                    while (*start && *start != '|') { start++; }
                    if (*start == '|') { start++; }
                    cur++;
                }
                const char *end = start;
                while (*end && *end != '|') { end++; }
                /* build directly: fmt({n}) on this slice. */
                char num[32];
                int nn = snprintf(num, sizeof num, "%llu", (unsigned long long)n);
                (void)nn;
                char *slice = malloc((size_t)(end - start) + 1);
                if (!slice) { return pc_fail(err, PC_ERR_NOMEM, "out of memory"); }
                memcpy(slice, start, (size_t)(end - start));
                slice[end - start] = '\0';
                const char *keys[1] = {"n"};
                const char *vals[1] = {num};
                pc_status st = pc_i18n_fmt(slice, keys, vals, 1, out, err);
                free(slice);
                return st;
            }
        }
    }
    text = (n == 1) ? one : other;
    char num[32];
    (void)snprintf(num, sizeof num, "%llu", (unsigned long long)n);
    const char *keys[1] = {"n"};
    const char *vals[1] = {num};
    return pc_i18n_fmt(text, keys, vals, 1, out, err);
}

/* ---- diagnostics (tests) ---- */
size_t pc_i18n_catalog_errors(const char *text, size_t len, char *first_error, size_t first_error_cap)
{
    size_t errors = 0;
    size_t pos = 0, lineno = 0;
    while (pos < len) {
        size_t nl = pos;
        while (nl < len && text[nl] != '\n') { nl++; }
        lineno++;
        const char *ctx, *src, *tr;
        size_t cl, sl, tl;
        bool bad;
        parse_line(text + pos, nl - pos, &ctx, &cl, &src, &sl, &tr, &tl, &bad);
        if (bad) {
            if (errors == 0 && first_error && first_error_cap > 0) {
                (void)snprintf(first_error, first_error_cap,
                               "line %zu: expected `context<TAB>source<TAB>translation`", lineno);
            }
            errors++;
        }
        pos = nl + 1;
    }
    return errors;
}

size_t pc_i18n_placeholders(const char *s, char out[][64], size_t cap)
{
    size_t count = 0;
    const char *p = s;
    while (count < cap) {
        const char *a = strchr(p, '{');
        if (!a) { break; }
        const char *b = strchr(a + 1, '}');
        if (!b) { break; }
        size_t plen = (size_t)(b - a - 1);
        if (plen < 64) {
            memcpy(out[count], a + 1, plen);
            out[count][plen] = '\0';
            count++;
        }
        p = b + 1;
    }
    return count;
}
