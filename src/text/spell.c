/* Spelling checker (see spell.h). */
#include "text/spell.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/compress.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "text/unicode.h"

extern const unsigned char pc_dict_scowl_gz[];
extern const size_t pc_dict_scowl_gz_len;

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "spell: out of memory")

/* ---- code point strings ---- */

typedef struct u32s {
    uint32_t *v;
    size_t n, cap;
} u32s;

static void u32s_fini(u32s *s)
{
    free(s->v);
    memset(s, 0, sizeof *s);
}

static bool u32s_push(u32s *s, uint32_t c)
{
    if (s->n >= s->cap) {
        void *g = pc_grow_array(s->v, &s->cap, s->n + 1, sizeof *s->v);
        if (g == NULL) {
            return false;
        }
        s->v = g;
    }
    s->v[s->n++] = c;
    return true;
}

static bool decode(const char *s, size_t len, u32s *out)
{
    size_t i = 0;
    out->n = 0;
    while (i < len) {
        uint32_t cp;
        if (!pc_utf8_next((const uint8_t *)s, len, &i, &cp)) {
            cp = 0xFFFD;
            i++;
        }
        if (!u32s_push(out, cp)) {
            return false;
        }
    }
    return true;
}

static bool encode(const uint32_t *v, size_t n, pc_buf *out)
{
    size_t i;
    pc_buf_truncate(out, 0);
    for (i = 0; i < n; i++) {
        if (pc_utf8_put(out, v[i], NULL) != PC_OK) {
            return false;
        }
    }
    return pc_buf_reserve(out, 1, NULL) == PC_OK; /* a terminator even when empty */
}

static bool any_upper(const uint32_t *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (pc_text_is_uppercase(v[i])) {
            return true;
        }
    }
    return false;
}

/* str::to_uppercase. */
static bool to_upper(const uint32_t *v, size_t n, u32s *out)
{
    size_t i, j;
    out->n = 0;
    for (i = 0; i < n; i++) {
        uint32_t u[3];
        size_t k = pc_text_to_upper(v[i], u);
        for (j = 0; j < k; j++) {
            if (!u32s_push(out, u[j])) {
                return false;
            }
        }
    }
    return true;
}

/* capitalize: the first char upper-cased, the rest as is. */
static bool capitalize(const uint32_t *v, size_t n, u32s *out)
{
    size_t i, j;
    out->n = 0;
    for (i = 0; i < n; i++) {
        if (i == 0) {
            uint32_t u[3];
            size_t k = pc_text_to_upper(v[0], u);
            for (j = 0; j < k; j++) {
                if (!u32s_push(out, u[j])) {
                    return false;
                }
            }
        } else if (!u32s_push(out, v[i])) {
            return false;
        }
    }
    return true;
}

static bool lower_of(const char *s, size_t len, pc_buf *out)
{
    pc_buf_truncate(out, 0);
    return pc_text_lower(s, len, out, NULL) == PC_OK && pc_buf_reserve(out, 1, NULL) == PC_OK;
}

/* ---- the dictionary ---- */

typedef struct group {
    size_t lower, lower_len; /* in the pool */
    size_t first, last;      /* forms: word indices linked through next */
    size_t chars, nchars;    /* lowercase chars in the char pool */
    uint8_t lvl;
} group;

struct pc_spell_dict {
    char *pool;
    size_t pool_len, pool_cap;
    size_t *word_off, *word_len, *word_next;
    uint8_t *word_level;
    size_t nwords, cap_words;
    size_t *word_ht; /* index + 1, 0 = empty */
    size_t word_htcap;
    group *groups;
    size_t ngroups, cap_groups;
    size_t *group_ht;
    size_t group_htcap;
    uint32_t *chars;
    size_t nchars_pool, cap_chars;
    size_t **by_len; /* group indices per char length, sorted by lowercase bytes */
    size_t *nby_len;
    size_t nlen;
    bool is_static;
};

static uint64_t hash(const char *s, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static bool pool_add(pc_spell_dict *d, const char *s, size_t n, size_t *off)
{
    if (d->pool == NULL || d->pool_len + n + 1 > d->pool_cap) {
        void *g = pc_grow_array(d->pool, &d->pool_cap, d->pool_len + n + 1, 1);
        if (g == NULL) {
            return false;
        }
        d->pool = g;
    }
    memcpy(d->pool + d->pool_len, s, n);
    d->pool[d->pool_len + n] = '\0';
    *off = d->pool_len;
    d->pool_len += n + 1;
    return true;
}

static size_t word_find(const pc_spell_dict *d, const char *s, size_t n)
{
    size_t i;
    if (d->word_htcap == 0) {
        return SIZE_MAX;
    }
    for (i = (size_t)(hash(s, n) & (d->word_htcap - 1));; i = (i + 1) & (d->word_htcap - 1)) {
        size_t w = d->word_ht[i];
        if (w == 0) {
            return SIZE_MAX;
        }
        if (d->word_len[w - 1] == n && memcmp(d->pool + d->word_off[w - 1], s, n) == 0) {
            return w - 1;
        }
    }
}

static size_t group_find(const pc_spell_dict *d, const char *s, size_t n)
{
    size_t i;
    if (d->group_htcap == 0) {
        return SIZE_MAX;
    }
    for (i = (size_t)(hash(s, n) & (d->group_htcap - 1));; i = (i + 1) & (d->group_htcap - 1)) {
        size_t g = d->group_ht[i];
        if (g == 0) {
            return SIZE_MAX;
        }
        if (d->groups[g - 1].lower_len == n && memcmp(d->pool + d->groups[g - 1].lower, s, n) == 0) {
            return g - 1;
        }
    }
}

/* Rehashes a table of `count` entries to keep it at most half full. */
static bool ht_grow(size_t **ht, size_t *cap, size_t count, const pc_spell_dict *d, bool words)
{
    size_t ncap = *cap == 0 ? 1024 : *cap, i;
    size_t *t;
    if ((count + 1) * 2 <= *cap) {
        return true;
    }
    while ((count + 1) * 2 > ncap) {
        ncap *= 2;
    }
    t = calloc(ncap, sizeof *t);
    if (t == NULL) {
        return false;
    }
    for (i = 0; i < count; i++) {
        const char *s = d->pool + (words ? d->word_off[i] : d->groups[i].lower);
        size_t n = words ? d->word_len[i] : d->groups[i].lower_len, j;
        for (j = (size_t)(hash(s, n) & (ncap - 1)); t[j] != 0; j = (j + 1) & (ncap - 1)) {
        }
        t[j] = i + 1;
    }
    free(*ht);
    *ht = t;
    *cap = ncap;
    return true;
}

static void ht_insert(size_t *ht, size_t cap, const char *s, size_t n, size_t index)
{
    size_t j;
    for (j = (size_t)(hash(s, n) & (cap - 1)); ht[j] != 0; j = (j + 1) & (cap - 1)) {
    }
    ht[j] = index + 1;
}

void pc_spell_free(pc_spell_dict *d)
{
    size_t i;
    if (d == NULL || d->is_static) {
        return;
    }
    free(d->pool);
    free(d->word_off);
    free(d->word_len);
    free(d->word_next);
    free(d->word_level);
    free(d->word_ht);
    free(d->groups);
    free(d->group_ht);
    free(d->chars);
    for (i = 0; i < d->nlen; i++) {
        free(d->by_len[i]);
    }
    free(d->by_len);
    free(d->nby_len);
    free(d);
}

size_t pc_spell_len(const pc_spell_dict *d)
{
    return d != NULL ? d->nwords : 0;
}

static bool add_word(pc_spell_dict *d, const char *w, size_t n, uint8_t level, pc_buf *low, u32s *tmp)
{
    size_t off, g;
#define GROWA(arr)                                                                                           \
    do {                                                                                                     \
        size_t c_ = d->cap_words;                                                                            \
        void *p_ = pc_grow_array((arr), &c_, d->nwords + 1, sizeof *(arr));                                  \
        if (p_ == NULL) {                                                                                    \
            return false;                                                                                    \
        }                                                                                                    \
        (arr) = p_;                                                                                          \
        cap = c_;                                                                                            \
    } while (0)
    if (d->nwords >= d->cap_words) {
        size_t cap;
        GROWA(d->word_off);
        GROWA(d->word_len);
        GROWA(d->word_next);
        GROWA(d->word_level);
        d->cap_words = cap;
    }
#undef GROWA
    if (!ht_grow(&d->word_ht, &d->word_htcap, d->nwords, d, true) || !pool_add(d, w, n, &off)) {
        return false;
    }
    d->word_off[d->nwords] = off;
    d->word_len[d->nwords] = n;
    d->word_next[d->nwords] = SIZE_MAX;
    d->word_level[d->nwords] = level;
    ht_insert(d->word_ht, d->word_htcap, w, n, d->nwords);
    if (!lower_of(w, n, low)) {
        return false;
    }
    g = group_find(d, (const char *)low->data, low->len);
    if (g == SIZE_MAX) {
        group gr;
        size_t i;
        if (!ht_grow(&d->group_ht, &d->group_htcap, d->ngroups, d, false)) {
            return false;
        }
        if (d->ngroups >= d->cap_groups) {
            void *p = pc_grow_array(d->groups, &d->cap_groups, d->ngroups + 1, sizeof *d->groups);
            if (p == NULL) {
                return false;
            }
            d->groups = p;
        }
        memset(&gr, 0, sizeof gr);
        if (!pool_add(d, (const char *)low->data, low->len, &gr.lower) ||
            !decode((const char *)low->data, low->len, tmp)) {
            return false;
        }
        gr.lower_len = low->len;
        gr.first = gr.last = d->nwords;
        gr.chars = d->nchars_pool;
        gr.nchars = tmp->n;
        gr.lvl = level;
        for (i = 0; i < tmp->n; i++) {
            if (d->nchars_pool >= d->cap_chars) {
                void *p = pc_grow_array(d->chars, &d->cap_chars, d->nchars_pool + 1, sizeof *d->chars);
                if (p == NULL) {
                    return false;
                }
                d->chars = p;
            }
            d->chars[d->nchars_pool++] = tmp->v[i];
        }
        d->groups[d->ngroups] = gr;
        ht_insert(d->group_ht, d->group_htcap, d->pool + gr.lower, gr.lower_len, d->ngroups);
        d->ngroups++;
    } else {
        group *gr = &d->groups[g];
        d->word_next[gr->last] = d->nwords;
        gr->last = d->nwords;
        if (level < gr->lvl) {
            gr->lvl = level;
        }
    }
    d->nwords++;
    return true;
}

/* u8::from_str: an optional '+', then decimal digits, at most 255. */
static bool parse_u8(const char *s, size_t n, uint8_t *out)
{
    unsigned v = 0;
    size_t i = 0;
    if (n > 0 && s[0] == '+') {
        i = 1;
    }
    if (i >= n) {
        return false;
    }
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
        v = v * 10 + (unsigned)(s[i] - '0');
        if (v > 255) {
            return false;
        }
    }
    *out = (uint8_t)v;
    return true;
}

static const pc_spell_dict *sort_ctx;

static int cmp_group(const void *a, const void *b)
{
    const group *x = &sort_ctx->groups[*(const size_t *)a], *y = &sort_ctx->groups[*(const size_t *)b];
    size_t n = x->lower_len < y->lower_len ? x->lower_len : y->lower_len;
    int c = memcmp(sort_ctx->pool + x->lower, sort_ctx->pool + y->lower, n);
    if (c != 0) {
        return c;
    }
    return x->lower_len < y->lower_len ? -1 : x->lower_len > y->lower_len;
}

static pthread_mutex_t sort_lock = PTHREAD_MUTEX_INITIALIZER;

static bool build_by_len(pc_spell_dict *d)
{
    size_t i, maxn = 0, *fill;
    for (i = 0; i < d->ngroups; i++) {
        maxn = d->groups[i].nchars > maxn ? d->groups[i].nchars : maxn;
    }
    d->nlen = d->ngroups > 0 ? maxn + 1 : 0;
    d->by_len = calloc(d->nlen > 0 ? d->nlen : 1, sizeof *d->by_len);
    d->nby_len = calloc(d->nlen > 0 ? d->nlen : 1, sizeof *d->nby_len);
    fill = calloc(d->nlen > 0 ? d->nlen : 1, sizeof *fill);
    if (d->by_len == NULL || d->nby_len == NULL || fill == NULL) {
        free(fill);
        return false;
    }
    for (i = 0; i < d->ngroups; i++) {
        d->nby_len[d->groups[i].nchars]++;
    }
    for (i = 0; i < d->nlen; i++) {
        d->by_len[i] = pc_malloc_array(d->nby_len[i] > 0 ? d->nby_len[i] : 1, sizeof **d->by_len);
        if (d->by_len[i] == NULL) {
            free(fill);
            return false;
        }
    }
    for (i = 0; i < d->ngroups; i++) {
        size_t n = d->groups[i].nchars;
        d->by_len[n][fill[n]++] = i;
    }
    free(fill);
    (void)pthread_mutex_lock(&sort_lock);
    sort_ctx = d;
    for (i = 0; i < d->nlen; i++) {
        if (d->nby_len[i] > 1) {
            qsort(d->by_len[i], d->nby_len[i], sizeof **d->by_len, cmp_group);
        }
    }
    sort_ctx = NULL;
    (void)pthread_mutex_unlock(&sort_lock);
    return true;
}

pc_status pc_spell_from_list(const char *list, size_t len, pc_spell_dict **out, pc_error *err)
{
    pc_spell_dict *d = calloc(1, sizeof *d);
    pc_buf low = PC_BUF_INIT;
    u32s tmp = {NULL, 0, 0};
    size_t at = 0;
    uint8_t level = 50;
    bool ok = d != NULL;
    while (ok && at < len) {
        const char *nl = memchr(list + at, '\n', len - at);
        size_t end = nl != NULL ? (size_t)(nl - list) : len, a = at, z;
        size_t start;
        at = nl != NULL ? end + 1 : len;
        z = end;
        if (z > a && list[z - 1] == '\r') {
            z--;
        }
        /* str::trim (Unicode white space). */
        {
            size_t tl = pc_utf8_trim(list + a, z - a, &start);
            a += start;
            z = a + tl;
        }
        if (a < z && list[a] == '#') {
            uint8_t lv;
            if (parse_u8(list + a + 1, z - a - 1, &lv)) {
                level = lv;
            }
            continue;
        }
        if (a == z || word_find(d, list + a, z - a) != SIZE_MAX) {
            continue;
        }
        ok = add_word(d, list + a, z - a, level, &low, &tmp);
    }
    ok = ok && build_by_len(d);
    pc_buf_fini(&low);
    u32s_fini(&tmp);
    if (!ok) {
        pc_spell_free(d);
        return NOMEM(err);
    }
    *out = d;
    return PC_OK;
}

static pthread_once_t english_once = PTHREAD_ONCE_INIT;
static pc_spell_dict *english;
static pc_spell_dict english_empty = {.is_static = true};

static void english_init(void)
{
    pc_buf text = PC_BUF_INIT;
    pc_spell_dict *d = NULL;
    if (pc_inflate(PC_Z_GZIP, pc_dict_scowl_gz, pc_dict_scowl_gz_len, (size_t)64 << 20, &text, NULL, NULL) ==
            PC_OK &&
        pc_spell_from_list((const char *)text.data, text.len, &d, NULL) == PC_OK) {
        d->is_static = true;
        english = d;
    }
    pc_buf_fini(&text);
}

const pc_spell_dict *pc_spell_english(void)
{
    (void)pthread_once(&english_once, english_init);
    return english != NULL ? english : &english_empty;
}

/* ---- checking ---- */

static bool is_apos_norm(uint32_t c)
{
    return c == 0x2019 || c == 0x2018 || c == 0x02BC;
}

static bool user_has(const pc_spell_user *u, const char *s, size_t n)
{
    size_t i;
    if (u == NULL) {
        return false;
    }
    for (i = 0; i < u->n; i++) {
        if (u->words[i] != NULL && strlen(u->words[i]) == n && memcmp(u->words[i], s, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool check(const pc_spell_dict *d, const char *word, size_t len, const pc_spell_user *user, bool *ok)
{
    u32s w = {NULL, 0, 0}, cap = {NULL, 0, 0}, fc = {NULL, 0, 0};
    pc_buf ws = PC_BUF_INIT, low = PC_BUF_INIT, cs = PC_BUF_INIT;
    size_t i, a = 0, z, g;
    bool numeric = false, alpha = false, res = false, all_caps = true, first_cap;
    *ok = false;
    if (!decode(word, len, &w)) {
        goto done;
    }
    for (i = 0; i < w.n; i++) {
        if (is_apos_norm(w.v[i])) {
            w.v[i] = '\'';
        }
    }
    z = w.n;
    if (z >= 2 && w.v[z - 2] == '\'' && w.v[z - 1] == 's' && z > 2) {
        z -= 2;
    }
    while (a < z && w.v[a] == '\'') {
        a++;
    }
    while (z > a && w.v[z - 1] == '\'') {
        z--;
    }
    for (i = a; i < z; i++) {
        numeric = numeric || pc_text_is_numeric(w.v[i]);
        alpha = alpha || pc_text_is_alphabetic(w.v[i]);
    }
    if (a == z || numeric || !alpha) {
        res = true;
        *ok = true;
        goto done;
    }
    if (!encode(w.v + a, z - a, &ws) || !lower_of((const char *)ws.data, ws.len, &low)) {
        goto done;
    }
    *ok = true;
    if (user_has(user, (const char *)low.data, low.len) ||
        word_find(d, (const char *)ws.data, ws.len) != SIZE_MAX) {
        res = true;
        goto done;
    }
    g = group_find(d, (const char *)low.data, low.len);
    if (g == SIZE_MAX) {
        goto done;
    }
    for (i = a; i < z; i++) {
        if (pc_text_is_alphabetic(w.v[i]) && !pc_text_is_uppercase(w.v[i])) {
            all_caps = false;
        }
    }
    first_cap = pc_text_is_uppercase(w.v[a]) && !any_upper(w.v + a + 1, z - a - 1);
    for (i = d->groups[g].first; i != SIZE_MAX && !res; i = d->word_next[i]) {
        const char *f = d->pool + d->word_off[i];
        bool f_lower;
        if (!decode(f, d->word_len[i], &fc)) {
            *ok = false;
            goto done;
        }
        f_lower = !any_upper(fc.v, fc.n);
        if (all_caps || (f_lower && first_cap)) {
            res = true;
        } else if (first_cap) {
            if (!capitalize(fc.v, fc.n, &cap) || !encode(cap.v, cap.n, &cs)) {
                *ok = false;
                goto done;
            }
            res = cs.len == ws.len && memcmp(cs.data, ws.data, ws.len) == 0;
        }
    }
done:
    u32s_fini(&w);
    u32s_fini(&cap);
    u32s_fini(&fc);
    pc_buf_fini(&ws);
    pc_buf_fini(&low);
    pc_buf_fini(&cs);
    return res;
}

bool pc_spell_check(const pc_spell_dict *d, const char *word, size_t len, const pc_spell_user *user)
{
    bool ok;
    bool r;
    if (d == NULL || (word == NULL && len > 0)) {
        return true;
    }
    r = check(d, word != NULL ? word : "", len, user, &ok);
    return ok ? r : true;
}

/* ---- suggestions ---- */

void pc_spell_list_fini(pc_spell_list *l)
{
    size_t i;
    if (l == NULL) {
        return;
    }
    for (i = 0; i < l->n; i++) {
        free(l->v[i]);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* Damerau-Levenshtein (optimal string alignment) distance, or SIZE_MAX when above max. */
static size_t distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m, size_t max, size_t *rows)
{
    size_t *prev2 = rows, *prev = rows + (m + 1), *cur = rows + 2 * (m + 1), *t, i, j;
    if ((n > m ? n - m : m - n) > max) {
        return SIZE_MAX;
    }
    for (j = 0; j <= m; j++) {
        prev2[j] = 0;
        prev[j] = j;
        cur[j] = 0;
    }
    for (i = 1; i <= n; i++) {
        size_t row_min;
        cur[0] = i;
        row_min = cur[0];
        for (j = 1; j <= m; j++) {
            size_t cost = a[i - 1] != b[j - 1] ? 1 : 0;
            size_t v = prev[j] + 1;
            if (cur[j - 1] + 1 < v) {
                v = cur[j - 1] + 1;
            }
            if (prev[j - 1] + cost < v) {
                v = prev[j - 1] + cost;
            }
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && prev2[j - 2] + 1 < v) {
                v = prev2[j - 2] + 1;
            }
            cur[j] = v;
            if (v < row_min) {
                row_min = v;
            }
        }
        if (row_min > max) {
            return SIZE_MAX;
        }
        t = prev2;
        prev2 = prev;
        prev = cur;
        cur = t;
    }
    return prev[m] <= max ? prev[m] : SIZE_MAX;
}

size_t pc_spell_distance(const uint32_t *a, size_t n, const uint32_t *b, size_t m, size_t max)
{
    size_t *rows, r;
    if ((a == NULL && n > 0) || (b == NULL && m > 0)) {
        return SIZE_MAX;
    }
    rows = pc_malloc_array(3 * (m + 1), sizeof *rows);
    if (rows == NULL) {
        return SIZE_MAX;
    }
    r = distance(a, n, b, m, max, rows);
    free(rows);
    return r;
}

typedef struct found {
    size_t d;
    bool not_anagram;
    uint8_t lvl;
    size_t group;
} found;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int cmp_found(const void *a, const void *b)
{
    const found *x = a, *y = b;
    if (x->d != y->d) {
        return x->d < y->d ? -1 : 1;
    }
    if (x->not_anagram != y->not_anagram) {
        return x->not_anagram ? 1 : -1;
    }
    if (x->lvl != y->lvl) {
        return x->lvl < y->lvl ? -1 : 1;
    }
    return cmp_group(&x->group, &y->group);
}

static bool list_has(const pc_spell_list *l, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (strlen(l->v[i]) == n && memcmp(l->v[i], s, n) == 0) {
            return true;
        }
    }
    return false;
}

pc_status pc_spell_suggest(const pc_spell_dict *d, const char *word, size_t len, size_t max,
                           pc_spell_list *out, pc_error *err)
{
    u32s w = {NULL, 0, 0}, lc = {NULL, 0, 0}, sorted = {NULL, 0, 0}, tmp = {NULL, 0, 0}, form = {NULL, 0, 0},
         res = {NULL, 0, 0};
    pc_buf ws = PC_BUF_INIT, low = PC_BUF_INIT, sb = PC_BUF_INIT;
    found *f = NULL;
    size_t nf = 0, capf = 0, i, j, n, max_d, len0, len1, alpha = 0, *rows = NULL;
    size_t *forms = NULL, nforms, capforms = 0;
    bool all_caps = true, first_cap, ok = true;
    pc_spell_list l = {NULL, 0};
    size_t capl = 0;
    if (d == NULL || out == NULL || (word == NULL && len > 0)) {
        return pc_fail(err, PC_ERR_INVALID, "spell: invalid arguments");
    }
    if (word == NULL) {
        word = "";
    }
    ok = decode(word, len, &w);
    for (i = 0; ok && i < w.n; i++) {
        if (is_apos_norm(w.v[i])) {
            w.v[i] = '\'';
        }
    }
    ok = ok && encode(w.v, w.n, &ws) && lower_of((const char *)ws.data, ws.len, &low) &&
         decode((const char *)low.data, low.len, &lc) && decode((const char *)low.data, low.len, &sorted);
    if (!ok) {
        goto nomem;
    }
    n = lc.n;
    max_d = n <= 4 ? 1 : 2;
    if (sorted.n > 1) {
        qsort(sorted.v, sorted.n, sizeof *sorted.v, cmp_u32);
    }
    len0 = n > max_d ? n - max_d : 0;
    len1 = n + max_d;
    rows = pc_malloc_array(3 * (len1 + 2), sizeof *rows);
    if (rows == NULL) {
        goto nomem;
    }
    for (i = len0; i <= len1 && i < d->nlen; i++) {
        for (j = 0; j < d->nby_len[i]; j++) {
            const group *g = &d->groups[d->by_len[i][j]];
            size_t dist = distance(lc.v, lc.n, d->chars + g->chars, g->nchars, max_d, rows);
            bool anagram;
            if (dist == SIZE_MAX) {
                continue;
            }
            tmp.n = 0;
            for (size_t q = 0; q < g->nchars; q++) {
                if (!u32s_push(&tmp, d->chars[g->chars + q])) {
                    goto nomem;
                }
            }
            if (tmp.n > 1) {
                qsort(tmp.v, tmp.n, sizeof *tmp.v, cmp_u32);
            }
            anagram =
                tmp.n == sorted.n && (tmp.n == 0 || memcmp(tmp.v, sorted.v, tmp.n * sizeof *tmp.v) == 0);
            if (nf >= capf) {
                void *p = pc_grow_array(f, &capf, nf + 1, sizeof *f);
                if (p == NULL) {
                    goto nomem;
                }
                f = p;
            }
            f[nf++] = (found){dist, !anagram, g->lvl, d->by_len[i][j]};
        }
    }
    (void)pthread_mutex_lock(&sort_lock);
    sort_ctx = d;
    if (nf > 1) {
        qsort(f, nf, sizeof *f, cmp_found);
    }
    sort_ctx = NULL;
    (void)pthread_mutex_unlock(&sort_lock);
    for (i = 0; i < w.n; i++) {
        if (pc_text_is_alphabetic(w.v[i])) {
            alpha++;
            if (!pc_text_is_uppercase(w.v[i])) {
                all_caps = false;
            }
        }
    }
    all_caps = all_caps && alpha > 1;
    first_cap = w.n > 0 && pc_text_is_uppercase(w.v[0]);
    for (i = 0; i < nf; i++) {
        const group *g = &d->groups[f[i].group];
        size_t k, a;
        nforms = 0;
        for (k = g->first; k != SIZE_MAX; k = d->word_next[k]) {
            if (nforms >= capforms) {
                void *p = pc_grow_array(forms, &capforms, nforms + 1, sizeof *forms);
                if (p == NULL) {
                    goto nomem;
                }
                forms = p;
            }
            forms[nforms++] = k;
        }
        /* Stable: lowercase forms first. */
        for (a = 0; a < 2; a++) {
            for (k = 0; k < nforms; k++) {
                const char *fs = d->pool + d->word_off[forms[k]];
                if (!decode(fs, d->word_len[forms[k]], &form)) {
                    goto nomem;
                }
                if (any_upper(form.v, form.n) != (a == 1)) {
                    continue;
                }
                if (all_caps) {
                    ok = to_upper(form.v, form.n, &res);
                } else if (first_cap) {
                    ok = capitalize(form.v, form.n, &res);
                } else {
                    res.n = 0;
                    for (j = 0; ok && j < form.n; j++) {
                        ok = u32s_push(&res, form.v[j]);
                    }
                }
                if (!ok || !encode(res.v, res.n, &sb)) {
                    goto nomem;
                }
                if (!list_has(&l, (const char *)sb.data, sb.len) &&
                    !(sb.len == ws.len && memcmp(sb.data, ws.data, ws.len) == 0)) {
                    char *s = pc_strndup((const char *)sb.data, sb.len);
                    if (s == NULL) {
                        goto nomem;
                    }
                    if (l.n >= capl) {
                        void *p = pc_grow_array(l.v, &capl, l.n + 1, sizeof *l.v);
                        if (p == NULL) {
                            free(s);
                            goto nomem;
                        }
                        l.v = p;
                    }
                    l.v[l.n++] = s;
                }
            }
        }
        if (l.n >= max) {
            break;
        }
    }
    while (l.n > max) {
        free(l.v[--l.n]);
    }
    *out = l;
    l.v = NULL;
    l.n = 0;
    goto cleanup;
nomem:
    pc_spell_list_fini(&l);
    ok = false;
cleanup:
    u32s_fini(&w);
    u32s_fini(&lc);
    u32s_fini(&sorted);
    u32s_fini(&tmp);
    u32s_fini(&form);
    u32s_fini(&res);
    pc_buf_fini(&ws);
    pc_buf_fini(&low);
    pc_buf_fini(&sb);
    free(f);
    free(rows);
    free(forms);
    return ok ? PC_OK : NOMEM(err);
}

/* ---- tokenising ---- */

pc_status pc_spell_words(const char *text, size_t len, pc_spell_span **out, size_t *n, pc_error *err)
{
    pc_spell_span *v = NULL;
    size_t nv = 0, cap = 0, i = 0, start = 0;
    bool has_start = false;
    if (text == NULL && len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "spell: invalid arguments");
    }
    while (i < len) {
        size_t b = i, nx;
        uint32_t c, next = 0;
        bool inner, has_next;
        if (!pc_utf8_next((const uint8_t *)text, len, &i, &c)) {
            c = 0xFFFD;
            i++;
        }
        nx = i;
        has_next = nx < len;
        if (has_next && !pc_utf8_next((const uint8_t *)text, len, &nx, &next)) {
            next = 0xFFFD;
        }
        inner =
            (c == '\'' || c == 0x2019 || c == 0x02BC) && has_start && has_next && pc_text_is_alphabetic(next);
        if (pc_unicode_is_alphanumeric(c) || inner) {
            if (!has_start) {
                start = b;
                has_start = true;
            }
        } else if (has_start) {
            if (nv >= cap) {
                void *p = pc_grow_array(v, &cap, nv + 1, sizeof *v);
                if (p == NULL) {
                    free(v);
                    return NOMEM(err);
                }
                v = p;
            }
            v[nv++] = (pc_spell_span){start, b};
            has_start = false;
        }
    }
    if (has_start) {
        if (nv >= cap) {
            void *p = pc_grow_array(v, &cap, nv + 1, sizeof *v);
            if (p == NULL) {
                free(v);
                return NOMEM(err);
            }
            v = p;
        }
        v[nv++] = (pc_spell_span){start, len};
    }
    *out = v;
    *n = nv;
    return PC_OK;
}

pc_status pc_spell_misspellings(const pc_spell_dict *d, const char *text, size_t len,
                                const pc_spell_user *user, pc_spell_span **out, size_t *n, pc_error *err)
{
    pc_spell_span *w;
    size_t nw, i, k = 0;
    pc_status st = pc_spell_words(text, len, &w, &nw, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < nw; i++) {
        if (!pc_spell_check(d, text + w[i].start, w[i].end - w[i].start, user)) {
            w[k++] = w[i];
        }
    }
    *out = w;
    *n = k;
    return PC_OK;
}
