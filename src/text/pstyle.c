/* Style resolution for the paragraph builder (parley 0.11's ranged style builder and parlance's
 * Language::parse); see playout.h. */
#include "text/playout.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

/* ------------------------------------------------------------------------------------------ */
/* Languages (parlance 0.1 Language::parse) */

static bool is_sep(char c)
{
    return c == '-' || c == '_';
}

static bool next_part(const char *s, size_t len, size_t *pos, size_t *a, size_t *b)
{
    while (*pos < len && is_sep(s[*pos])) {
        (*pos)++;
    }
    if (*pos >= len) {
        return false;
    }
    *a = *pos;
    while (*pos < len && !is_sep(s[*pos])) {
        (*pos)++;
    }
    *b = *pos;
    return true;
}

static bool all_alpha(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return false;
        }
    }
    return true;
}

static bool all_digit(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool all_alnum(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!all_alpha(s + i, 1) && !all_digit(s + i, 1)) {
            return false;
        }
    }
    return true;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
}

static char upper(char c)
{
    return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}

static bool is_variant(const char *b, size_t n)
{
    if (n == 4) {
        return b[0] >= '0' && b[0] <= '9' && all_alnum(b, n);
    }
    return n >= 5 && n <= 8 && all_alnum(b, n);
}

static bool put_region(pl_lang *o, const char *b, size_t n)
{
    bool alpha2 = n == 2 && all_alpha(b, 2), digit3 = n == 3 && all_digit(b, 3);
    size_t i;
    if (!alpha2 && !digit3) {
        return false;
    }
    o->bytes[o->len++] = '-';
    for (i = 0; i < n; i++) {
        o->bytes[o->len++] = alpha2 ? upper(b[i]) : b[i];
    }
    return true;
}

/* validate_remainder of parlance. */
static bool valid_rest(const char *s, size_t len, size_t pos)
{
    enum { BASE, EXT, PRIV } state = BASE;
    bool needs = false;
    size_t a, b;
    while (next_part(s, len, &pos, &a, &b)) {
        const char *p = s + a;
        size_t n = b - a;
        bool single = n == 1 && all_alnum(p, 1);
        if (state == BASE) {
            if (is_variant(p, n)) {
                continue;
            }
            if (single) {
                state = lower(p[0]) == 'x' ? PRIV : EXT;
                needs = true;
                continue;
            }
            return false;
        } else if (state == EXT) {
            if (single) {
                if (needs) {
                    return false;
                }
                state = lower(p[0]) == 'x' ? PRIV : EXT;
                needs = true;
                continue;
            }
            if (n >= 2 && n <= 8 && all_alnum(p, n)) {
                needs = false;
                continue;
            }
            return false;
        } else {
            if (n >= 1 && n <= 8 && all_alnum(p, n)) {
                needs = false;
                continue;
            }
            return false;
        }
    }
    return !needs;
}

bool pl_lang_parse(const char *s, size_t len, pl_lang *out)
{
    pl_lang o;
    size_t pos = 0, a, b, i, rest;
    memset(&o, 0, sizeof o);
    if (!next_part(s, len, &pos, &a, &b) || b - a < 2 || b - a > 3 || !all_alpha(s + a, b - a)) {
        return false;
    }
    for (i = a; i < b; i++) {
        o.bytes[o.len++] = lower(s[i]);
    }
    o.language_len = o.len;
    rest = len;
    if (next_part(s, len, &pos, &a, &b)) {
        const char *p = s + a;
        size_t n = b - a;
        rest = a;
        if (n == 4) {
            if (!all_alpha(p, 4)) {
                if (!is_variant(p, 4)) {
                    return false;
                }
            } else {
                o.bytes[o.len++] = '-';
                o.bytes[o.len++] = upper(p[0]);
                for (i = 1; i < 4; i++) {
                    o.bytes[o.len++] = lower(p[i]);
                }
                rest = pos;
                if (next_part(s, len, &pos, &a, &b)) {
                    if (b - a == 2 || b - a == 3) {
                        if (!put_region(&o, s + a, b - a)) {
                            return false;
                        }
                        rest = pos;
                    } else {
                        rest = a;
                    }
                }
            }
        } else if (n == 2 || n == 3) {
            if (!put_region(&o, p, n)) {
                return false;
            }
            rest = pos;
        }
    }
    if (!valid_rest(s, len, rest)) {
        return false;
    }
    *out = o;
    return true;
}

bool pl_lang_eq(bool ha, const pl_lang *a, bool hb, const pl_lang *b)
{
    if (ha != hb) {
        return false;
    }
    return !ha || (a->len == b->len && a->language_len == b->language_len &&
                   memcmp(a->bytes, b->bytes, a->len) == 0);
}

void pl_apply_prop(pl_style *s, const pl_prop *p)
{
    switch (p->kind) {
    case PL_PROP_FAMILIES:
        s->families = p->id;
        break;
    case PL_PROP_SIZE:
        s->size = p->f;
        break;
    case PL_PROP_WEIGHT:
        s->weight = p->f;
        break;
    case PL_PROP_ITALIC:
        s->italic = p->b;
        break;
    case PL_PROP_FEATURES:
        s->features = p->id;
        break;
    case PL_PROP_VARIATIONS:
        s->variations = p->id;
        break;
    case PL_PROP_LETTER_SPACING:
        s->letter_spacing = p->f;
        break;
    case PL_PROP_UNDERLINE:
        s->underline = p->b;
        break;
    case PL_PROP_STRIKETHROUGH:
        s->strikethrough = p->b;
        break;
    case PL_PROP_BRUSH:
        s->brush_run = p->brush_run;
        s->brush_class = p->brush_class;
        break;
    case PL_PROP_LOCALE:
        s->has_locale = p->b;
        s->locale = p->lang;
        break;
    }
}

bool pl_style_eq(const pl_style *a, const pl_style *b)
{
    return a->families == b->families && a->features == b->features && a->variations == b->variations &&
           a->size == b->size && a->weight == b->weight && a->italic == b->italic &&
           pl_lang_eq(a->has_locale, &a->locale, b->has_locale, &b->locale) && a->brush_run == b->brush_run &&
           a->brush_class == b->brush_class && a->underline == b->underline &&
           a->strikethrough == b->strikethrough && a->letter_spacing == b->letter_spacing;
}

/* ------------------------------------------------------------------------------------------ */
/* Style runs (RangedStyleBuilder::finish) */

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

/* Per segment between property boundaries: the root style with every property that covers it
 * applied in push order; equal neighbours merged (the reference splits and merges spans the same
 * way, so the runs and the style table come out identical). */
pc_status pl_resolve_styles(const pl_style *root, pl_prop *props, size_t nprops, size_t len, pl_style **table,
                            size_t *ntable, pl_style_run **runs, size_t *nruns, pc_error *err)
{
    size_t *bounds = NULL, nb = 0, i, k, *order = NULL, *active = NULL, nactive = 0, next = 0;
    pl_style *t = NULL;
    pl_style_run *r = NULL;
    size_t nt = 0, cap_t = 0, nr = 0, cap_r = 0;
    pc_status st = PC_OK;
    if (len == 0) {
        t = malloc(sizeof *t);
        r = malloc(sizeof *r);
        if (t == NULL || r == NULL) {
            free(t);
            free(r);
            return NOMEM(err);
        }
        t[0] = *root;
        r[0] = (pl_style_run){0, 0, 0};
        *table = t;
        *ntable = 1;
        *runs = r;
        *nruns = 1;
        return PC_OK;
    }
    bounds = pc_malloc_array(2 * nprops + 2, sizeof *bounds);
    order = pc_malloc_array(nprops + 1, sizeof *order);
    active = pc_malloc_array(nprops + 1, sizeof *active);
    if (bounds == NULL || order == NULL || active == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    bounds[nb++] = 0;
    bounds[nb++] = len;
    for (i = 0; i < nprops; i++) {
        pl_prop *p = &props[i];
        p->start = p->start < len ? p->start : len;
        p->end = p->end < len ? p->end : len;
        if (p->start < p->end) {
            bounds[nb++] = p->start;
            bounds[nb++] = p->end;
        }
    }
    qsort(bounds, nb, sizeof *bounds, cmp_size);
    for (i = 0, k = 0; i < nb; i++) {
        if (k == 0 || bounds[k - 1] != bounds[i]) {
            bounds[k++] = bounds[i];
        }
    }
    nb = k;
    /* Properties ordered by start (then push order). */
    for (i = 0, k = 0; i < nprops; i++) {
        if (props[i].start < props[i].end) {
            order[k++] = i;
        }
    }
    {
        size_t n = k, a, j;
        for (a = 1; a < n; a++) {
            size_t x = order[a];
            for (j = a; j > 0 && props[order[j - 1]].start > props[x].start; j--) {
                order[j] = order[j - 1];
            }
            order[j] = x;
        }
        for (i = 0; i + 1 < nb; i++) {
            size_t s = bounds[i], e = bounds[i + 1], w = 0;
            pl_style cur = *root;
            while (next < n && props[order[next]].start <= s) {
                active[nactive++] = order[next++];
            }
            for (j = 0; j < nactive; j++) {
                if (props[active[j]].end > s) {
                    active[w++] = active[j];
                }
            }
            nactive = w;
            /* Apply in push order (indices ascending). */
            for (a = 1; a < nactive; a++) {
                size_t x = active[a];
                for (j = a; j > 0 && active[j - 1] > x; j--) {
                    active[j] = active[j - 1];
                }
                active[j] = x;
            }
            for (j = 0; j < nactive; j++) {
                pl_apply_prop(&cur, &props[active[j]]);
            }
            if (nr > 0 && pl_style_eq(&t[nt - 1], &cur)) {
                r[nr - 1].end = e;
                continue;
            }
            if (nt >= cap_t) {
                void *g = pc_grow_array(t, &cap_t, nt + 1, sizeof *t);
                if (g == NULL) {
                    st = NOMEM(err);
                    goto cleanup;
                }
                t = g;
            }
            if (nr >= cap_r) {
                void *g = pc_grow_array(r, &cap_r, nr + 1, sizeof *r);
                if (g == NULL) {
                    st = NOMEM(err);
                    goto cleanup;
                }
                r = g;
            }
            t[nt] = cur;
            /* `style_index as u16` (wraps past 65535 styles, as in the reference). */
            r[nr++] = (pl_style_run){s, e, (uint16_t)nt};
            nt++;
        }
    }
cleanup:
    free(bounds);
    free(order);
    free(active);
    if (st != PC_OK) {
        free(t);
        free(r);
        return st;
    }
    *table = t;
    *ntable = nt;
    *runs = r;
    *nruns = nr;
    return PC_OK;
}
