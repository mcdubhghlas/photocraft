/* CSS for SVG (standing in for simplecss 0.2): style sheets of rule sets with type, universal,
 * class, id, attribute ([a], [a=v], [a~=v], [a|=v]) and :first-child selectors joined by
 * descendant, child and adjacent-sibling combinators; at-rules are skipped; rules are sorted
 * (stably) by specificity. Declarations carry !important. Matching runs on the XML DOM. */
#include <stdlib.h>
#include <string.h>

#include "io/svg_internal.h"

typedef enum { OP_EXISTS, OP_MATCHES, OP_CONTAINS, OP_STARTS_WITH } attr_op;
typedef enum { C_NONE, C_DESCENDANT, C_CHILD, C_ADJACENT } combinator;

typedef struct sub {
    bool pseudo;     /* else attribute */
    int pseudo_kind; /* 0 first-child, 1 never matches (link, visited, hover, active, focus, lang) */
    const char *name;
    attr_op op;
    const char *value;
} sub;

typedef struct comp {
    combinator comb;
    const char *type; /* NULL = universal */
    sub *subs;
    size_t nsubs, capsubs;
} comp;

struct sc_rule {
    comp *comps;
    size_t ncomps;
    sc_decl *decls;
    size_t ndecls;
    uint8_t spec[3];
    size_t order;
};

typedef struct cs {
    const char *s;
    size_t len;
    size_t pos;
} cs;

static bool c_end(const cs *c)
{
    return c->pos >= c->len;
}

static char c_cur(const cs *c)
{
    return c->pos < c->len ? c->s[c->pos] : '\0';
}

static bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* Skips spaces and comments; false for an unclosed comment. */
static bool skip_sc(cs *c)
{
    for (;;) {
        while (!c_end(c) && is_space(c_cur(c))) {
            c->pos++;
        }
        if (c->len - c->pos >= 2 && c->s[c->pos] == '/' && c->s[c->pos + 1] == '*') {
            const char *e = NULL;
            size_t i;
            for (i = c->pos + 2; i + 1 < c->len; i++) {
                if (c->s[i] == '*' && c->s[i + 1] == '/') {
                    e = c->s + i;
                    break;
                }
            }
            if (e == NULL) {
                c->pos = c->len;
                return false;
            }
            c->pos = (size_t)(e - c->s) + 2;
            continue;
        }
        return true;
    }
}

static bool ident_start(unsigned char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch >= 0x80;
}

static bool ident_char(unsigned char ch)
{
    return ident_start(ch) || (ch >= '0' && ch <= '9') || ch == '-';
}

static const char *ident(sv_arena *a, cs *c)
{
    size_t st = c->pos;
    if (c_cur(c) == '-') {
        c->pos++;
    }
    if (c_end(c) || !ident_start((unsigned char)c_cur(c))) {
        c->pos = st;
        return NULL;
    }
    while (!c_end(c) && ident_char((unsigned char)c_cur(c))) {
        c->pos++;
    }
    return sv_strndup(a, c->s + st, c->pos - st);
}

static bool add_comp(sv_arena *a, comp **v, size_t *n, size_t *cap, combinator comb, const char *type)
{
    void *p = *v;
    if (!sv_grow(a, &p, cap, *n + 1, sizeof(comp))) {
        return false;
    }
    *v = p;
    memset(&(*v)[*n], 0, sizeof(comp));
    (*v)[*n].comb = comb;
    (*v)[*n].type = type;
    (*n)++;
    return true;
}

static bool add_sub(sv_arena *a, comp **v, size_t *n, size_t *cap, combinator *comb, sub s)
{
    comp *last;
    void *p;
    if (*comb != C_NONE || *n == 0) {
        if (!add_comp(a, v, n, cap, *comb, NULL)) {
            return false;
        }
        *comb = C_NONE;
    }
    last = &(*v)[*n - 1];
    p = last->subs;
    if (!sv_grow(a, &p, &last->capsubs, last->nsubs + 1, sizeof(sub))) {
        return false;
    }
    last->subs = p;
    last->subs[last->nsubs++] = s;
    return true;
}

/* An attribute selector value: quoted string or ident. */
static const char *attr_value(sv_arena *a, cs *c)
{
    char q = c_cur(c);
    if (q == '\'' || q == '"') {
        size_t st = ++c->pos;
        while (!c_end(c) && c_cur(c) != q) {
            c->pos++;
        }
        if (c_end(c)) {
            return NULL;
        }
        c->pos++;
        return sv_strndup(a, c->s + st, c->pos - 1 - st);
    }
    return ident(a, c);
}

/* Parses one selector at c; *ok false when it is invalid (skipped, as simplecss). Stops at '{',
 * ',' or the end. */
static bool selector(sv_arena *a, cs *c, comp **out, size_t *nout, bool *ok)
{
    comp *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    combinator comb = C_NONE;
    *ok = false;
    while (!c_end(c)) {
        char ch = c_cur(c);
        sub s;
        memset(&s, 0, sizeof s);
        if (ch == '{' || ch == ',') {
            break;
        }
        if (is_space(ch) || ch == '/') {
            size_t st = c->pos;
            if (!skip_sc(c)) {
                return true;
            }
            ch = c_cur(c);
            if (c_end(c) || ch == '{' || ch == ',') {
                break;
            }
            if (ch != '>' && ch != '+' && c->pos != st && n > 0) {
                comb = C_DESCENDANT;
            }
            continue;
        }
        if (ch == '>' || ch == '+') {
            c->pos++;
            comb = ch == '>' ? C_CHILD : C_ADJACENT;
            if (!skip_sc(c)) {
                return true;
            }
            continue;
        }
        if (ch == '*') {
            c->pos++;
            if (!add_comp(a, &v, &n, &cap, comb, NULL)) {
                return false;
            }
            comb = C_NONE;
            continue;
        }
        if (ch == '.' || ch == '#') {
            c->pos++;
            s.value = ident(a, c);
            if (s.value == NULL) {
                return !a->nomem;
            }
            s.name = ch == '.' ? "class" : "id";
            s.op = ch == '.' ? OP_CONTAINS : OP_MATCHES;
            if (!add_sub(a, &v, &n, &cap, &comb, s)) {
                return false;
            }
            continue;
        }
        if (ch == '[') {
            c->pos++;
            (void)skip_sc(c);
            s.name = ident(a, c);
            if (s.name == NULL) {
                return !a->nomem;
            }
            (void)skip_sc(c);
            s.op = OP_EXISTS;
            if (c_cur(c) == '=') {
                s.op = OP_MATCHES;
                c->pos++;
            } else if ((c_cur(c) == '~' || c_cur(c) == '|') && c->pos + 1 < c->len &&
                       c->s[c->pos + 1] == '=') {
                s.op = c_cur(c) == '~' ? OP_CONTAINS : OP_STARTS_WITH;
                c->pos += 2;
            } else if (c_cur(c) != ']') {
                return true;
            }
            if (s.op != OP_EXISTS) {
                (void)skip_sc(c);
                s.value = attr_value(a, c);
                if (s.value == NULL) {
                    return !a->nomem;
                }
                (void)skip_sc(c);
            }
            if (c_cur(c) != ']') {
                return true;
            }
            c->pos++;
            if (!add_sub(a, &v, &n, &cap, &comb, s)) {
                return false;
            }
            continue;
        }
        if (ch == ':') {
            const char *pc;
            c->pos++;
            pc = ident(a, c);
            if (pc == NULL) {
                return !a->nomem;
            }
            s.pseudo = true;
            if (strcmp(pc, "first-child") == 0) {
                s.pseudo_kind = 0;
            } else if (strcmp(pc, "lang") == 0 && c_cur(c) == '(') {
                while (!c_end(c) && c_cur(c) != ')') {
                    c->pos++;
                }
                if (c_end(c)) {
                    return true;
                }
                c->pos++;
                s.pseudo_kind = 1;
            } else if (strcmp(pc, "link") == 0 || strcmp(pc, "visited") == 0 || strcmp(pc, "hover") == 0 ||
                       strcmp(pc, "active") == 0 || strcmp(pc, "focus") == 0) {
                s.pseudo_kind = 1;
            } else {
                return true;
            }
            if (!add_sub(a, &v, &n, &cap, &comb, s)) {
                return false;
            }
            continue;
        }
        {
            const char *t = ident(a, c);
            if (t == NULL) {
                return !a->nomem;
            }
            if (!add_comp(a, &v, &n, &cap, comb, t)) {
                return false;
            }
            comb = C_NONE;
        }
    }
    if (n > 0 && v[0].comb == C_NONE && comb == C_NONE) {
        *ok = true;
        *out = v;
        *nout = n;
    }
    return true;
}

/* simplecss Stream::consume_ident without the string: false on an invalid ident (the position
 * then stays after a leading '-', as there). */
static bool term_ident(cs *c)
{
    size_t st = c->pos;
    if (c_cur(c) == '-') {
        c->pos++;
    }
    if (!c_end(c)) {
        if (!ident_start((unsigned char)c_cur(c))) {
            return false;
        }
        c->pos++;
        while (!c_end(c) && ident_char((unsigned char)c_cur(c))) {
            c->pos++;
        }
    }
    return c->pos != st;
}

/* simplecss consume_term (lib.rs). */
static bool consume_term(cs *c)
{
    char ch;
    if (c_end(c)) {
        return false;
    }
    ch = c_cur(c);
    if (ch == '#') {
        size_t p0;
        c->pos++;
        p0 = c->pos;
        if (!term_ident(c)) {
            (void)p0;
            while (!c_end(c) &&
                   ((c_cur(c) >= '0' && c_cur(c) <= '9') || (c_cur(c) >= 'a' && c_cur(c) <= 'f') ||
                    (c_cur(c) >= 'A' && c_cur(c) <= 'F'))) {
                c->pos++;
            }
        }
        return true;
    }
    if (ch == '+' || ch == '-' || (ch >= '0' && ch <= '9') || ch == '.') {
        c->pos++;
        while (!c_end(c) && c_cur(c) >= '0' && c_cur(c) <= '9') {
            c->pos++;
        }
        if (c_cur(c) == '.') {
            c->pos++;
            while (!c_end(c) && c_cur(c) >= '0' && c_cur(c) <= '9') {
                c->pos++;
            }
        }
        if (c_cur(c) == '%') {
            c->pos++;
        } else {
            (void)term_ident(c);
        }
        return true;
    }
    if (ch == '\'' || ch == '"') {
        char prev = ch;
        c->pos++;
        while (!c_end(c)) {
            char cur = c_cur(c);
            if (cur == ch && prev != '\\') {
                break;
            }
            prev = cur;
            c->pos++;
        }
        if (c_end(c)) {
            return false;
        }
        c->pos++;
        return true;
    }
    if (ch == ',') {
        c->pos++;
        return true;
    }
    if (!term_ident(c)) {
        return false;
    }
    if (c_cur(c) == '(') {
        while (!c_end(c) && c_cur(c) != ')') {
            c->pos++;
        }
        if (c_end(c)) {
            return false;
        }
        c->pos++;
    }
    return true;
}

/* One declaration; *ok false when malformed. */
static bool declaration(sv_arena *a, cs *c, sc_decl *out, bool *ok)
{
    const char *nm;
    size_t st;
    size_t e;
    *ok = false;
    if (!skip_sc(c)) {
        return true;
    }
    if (c_cur(c) == '*') {
        c->pos++;
    }
    nm = ident(a, c);
    if (nm == NULL) {
        return !a->nomem;
    }
    if (!skip_sc(c) || c_cur(c) != ':') {
        return true;
    }
    c->pos++;
    if (!skip_sc(c)) {
        return true;
    }
    /* simplecss: the value is a run of terms (consume_term) separated by spaces/comments */
    st = c->pos;
    e = c->pos;
    while (consume_term(c)) {
        e = c->pos;
        (void)skip_sc(c);
    }
    while (e > st && is_space(c->s[e - 1])) {
        e--;
    }
    out->important = false;
    if (c_cur(c) == '!') {
        c->pos++;
        (void)skip_sc(c);
        if (c->len - c->pos >= 9 && memcmp(c->s + c->pos, "important", 9) == 0) {
            c->pos += 9;
            out->important = true;
        }
    }
    (void)skip_sc(c);
    while (c_cur(c) == ';') {
        c->pos++;
        (void)skip_sc(c);
    }
    if (e == st) {
        return true;
    }
    out->name = nm;
    out->value = sv_strndup(a, c->s + st, e - st);
    if (out->value == NULL) {
        return false;
    }
    *ok = true;
    return true;
}

static void consume_until_block_end(cs *c)
{
    int braces = 0;
    while (!c_end(c)) {
        char ch = c_cur(c);
        if (ch == '{') {
            braces++;
        } else if (ch == '}') {
            if (braces == 0) {
                break;
            }
            braces--;
        }
        c->pos++;
    }
    if (c_cur(c) == '}') {
        c->pos++;
    }
}

bool sc_parse_decls(sv_arena *a, const char *text, sc_decl **out, size_t *n)
{
    cs c = {text, strlen(text), 0};
    sc_decl *v = NULL;
    size_t cap = 0;
    *n = 0;
    *out = NULL;
    for (;;) {
        sc_decl d;
        bool ok;
        void *p = v;
        (void)skip_sc(&c);
        if (c_end(&c)) {
            break;
        }
        if (!declaration(a, &c, &d, &ok)) {
            return false;
        }
        if (!ok) {
            break;
        }
        if (!sv_grow(a, &p, &cap, *n + 1, sizeof(sc_decl))) {
            return false;
        }
        v = p;
        v[(*n)++] = d;
    }
    *out = v;
    return true;
}

static int spec_cmp(const sc_rule *x, const sc_rule *y)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (x->spec[i] != y->spec[i]) {
            return x->spec[i] < y->spec[i] ? -1 : 1;
        }
    }
    return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

static int rule_cmp(const void *a, const void *b)
{
    return spec_cmp(*(sc_rule *const *)a, *(sc_rule *const *)b);
}

static void specificity(sc_rule *r)
{
    size_t i;
    size_t j;
    unsigned s[3] = {0, 0, 0};
    for (i = 0; i < r->ncomps; i++) {
        if (r->comps[i].type != NULL) {
            s[2]++;
        }
        for (j = 0; j < r->comps[i].nsubs; j++) {
            const sub *x = &r->comps[i].subs[j];
            if (!x->pseudo && strcmp(x->name, "id") == 0) {
                s[0]++;
            } else {
                s[1]++;
            }
        }
    }
    for (i = 0; i < 3; i++) {
        r->spec[i] = (uint8_t)(s[i] > 255 ? 255 : s[i]);
    }
}

bool sc_parse_more(sv_arena *a, sc_sheet *sheet, const char *text)
{
    cs c = {text, strlen(text), 0};
    size_t i;
    if (!skip_sc(&c)) {
        return true;
    }
    while (!c_end(&c)) {
        size_t first = sheet->n;
        sc_decl *decls = NULL;
        size_t ndecls = 0;
        size_t capd = 0;
        if (!skip_sc(&c) || c_end(&c)) {
            break;
        }
        if (c_cur(&c) == '@') {
            c.pos++;
            while (!c_end(&c) && c_cur(&c) != ';' && c_cur(&c) != '{') {
                c.pos++;
            }
            if (c_cur(&c) == ';') {
                c.pos++;
            } else if (c_cur(&c) == '{') {
                c.pos++;
                consume_until_block_end(&c);
            }
            continue;
        }
        for (;;) {
            comp *comps = NULL;
            size_t ncomps = 0;
            bool ok;
            if (c_cur(&c) == ',') {
                c.pos++;
            }
            (void)skip_sc(&c);
            if (!selector(a, &c, &comps, &ncomps, &ok)) {
                return false;
            }
            if (ok) {
                sc_rule *r = sv_alloc(a, sizeof *r);
                void *p = sheet->rules;
                if (r == NULL || !sv_grow(a, &p, &sheet->cap, sheet->n + 1, sizeof(sc_rule *))) {
                    return false;
                }
                sheet->rules = p;
                r->comps = comps;
                r->ncomps = ncomps;
                specificity(r);
                sheet->rules[sheet->n++] = r;
            }
            (void)skip_sc(&c);
            if (c_end(&c) || c_cur(&c) == '{') {
                break;
            }
            if (c_cur(&c) != ',') {
                while (!c_end(&c) && c_cur(&c) != '{') {
                    c.pos++;
                }
                break;
            }
        }
        if (c_cur(&c) == '{') {
            c.pos++;
        }
        for (;;) {
            sc_decl d;
            bool ok;
            void *p = decls;
            (void)skip_sc(&c);
            if (c_end(&c) || c_cur(&c) == '}') {
                break;
            }
            if (!declaration(a, &c, &d, &ok)) {
                return false;
            }
            if (!ok) {
                consume_until_block_end(&c);
                break;
            }
            if (!sv_grow(a, &p, &capd, ndecls + 1, sizeof(sc_decl))) {
                return false;
            }
            decls = p;
            decls[ndecls++] = d;
        }
        if (c_cur(&c) == '}') {
            c.pos++;
        }
        for (i = first; i < sheet->n; i++) {
            sheet->rules[i]->decls = decls;
            sheet->rules[i]->ndecls = ndecls;
        }
    }
    /* Remove empty rules, then sort by specificity (stable: the order breaks ties). */
    {
        size_t k = 0;
        for (i = 0; i < sheet->n; i++) {
            if (sheet->rules[i]->ndecls > 0) {
                sheet->rules[k++] = sheet->rules[i];
            }
        }
        sheet->n = k;
        for (i = 0; i < sheet->n; i++) {
            sheet->rules[i]->order = i;
        }
        if (sheet->n > 1) {
            qsort(sheet->rules, sheet->n, sizeof(sc_rule *), rule_cmp);
        }
    }
    return true;
}

static bool sub_matches(const sub *s, const sx_node *el)
{
    const char *v;
    size_t vl;
    size_t sl;
    if (s->pseudo) {
        return s->pseudo_kind == 0 && sx_prev_element(el) == NULL;
    }
    v = sx_attr_get(el, NULL, s->name);
    if (v == NULL) {
        return false;
    }
    switch (s->op) {
    case OP_EXISTS:
        return true;
    case OP_MATCHES:
        return strcmp(v, s->value) == 0;
    case OP_CONTAINS: {
        const char *p = v;
        sl = strlen(s->value);
        for (;;) {
            const char *sp = strchr(p, ' ');
            size_t n = sp != NULL ? (size_t)(sp - p) : strlen(p);
            if (n == sl && memcmp(p, s->value, n) == 0) {
                return true;
            }
            if (sp == NULL) {
                return false;
            }
            p = sp + 1;
        }
    }
    case OP_STARTS_WITH:
    default:
        vl = strlen(v);
        sl = strlen(s->value);
        if (strcmp(v, s->value) == 0) {
            return true;
        }
        return vl > sl && memcmp(v, s->value, sl) == 0 && v[sl] == '-';
    }
}

static bool comp_matches(const comp *c, const sx_node *el)
{
    size_t i;
    if (c->type != NULL && strcmp(el->name, c->type) != 0) {
        return false;
    }
    for (i = 0; i < c->nsubs; i++) {
        if (!sub_matches(&c->subs[i], el)) {
            return false;
        }
    }
    return true;
}

static const sx_node *parent_element(const sx_node *n)
{
    const sx_node *p = n->parent;
    return p != NULL && p->parent != NULL ? p : NULL;
}

/* Backtracking over the components; descendant combinators may retry every ancestor, so the
 * work per element is capped (a hostile sheet could otherwise take exponential time). */
#define MATCH_STEPS 100000

static bool matches_at(const sc_rule *r, size_t idx, const sx_node *el, size_t *steps)
{
    const comp *c = &r->comps[idx];
    if (*steps == 0) {
        return false;
    }
    (*steps)--;
    if (!comp_matches(c, el)) {
        return false;
    }
    switch (c->comb) {
    case C_DESCENDANT: {
        const sx_node *p = parent_element(el);
        while (p != NULL && *steps > 0) {
            if (matches_at(r, idx - 1, p, steps)) {
                return true;
            }
            p = parent_element(p);
        }
        return false;
    }
    case C_CHILD: {
        const sx_node *p = parent_element(el);
        return p != NULL && matches_at(r, idx - 1, p, steps);
    }
    case C_ADJACENT: {
        const sx_node *p = sx_prev_element(el);
        return p != NULL && matches_at(r, idx - 1, p, steps);
    }
    case C_NONE:
    default:
        return true;
    }
}

bool sc_rule_matches(const sc_rule *r, const sx_node *el)
{
    size_t steps = MATCH_STEPS;
    /* The recursion depth is the component count (bounded here). */
    if (r->ncomps == 0 || r->ncomps > 64) {
        return false;
    }
    return matches_at(r, r->ncomps - 1, el, &steps);
}

const sc_decl *sc_rule_decls(const sc_rule *r, size_t *n)
{
    *n = r->ndecls;
    return r->decls;
}
