/* The arena and a small XML parser for SVG documents (standing in for roxmltree): elements,
 * attributes (normalised, entities expanded), text and CDATA, comments, processing
 * instructions, a DOCTYPE with internal general entities (Illustrator writes its namespaces as
 * entities), and namespace prefixes. Entity expansion is bounded (no "billion laughs"), and so
 * is nesting; the parser itself never recurses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "io/svg_internal.h"

/* ---------------------------------------------------------------- arena */

struct sv_chunk {
    sv_chunk *next;
    size_t used;
    size_t cap;
    max_align_t data[1];
};

#define SV_CHUNK ((size_t)64 * 1024)

void sv_arena_init(sv_arena *a)
{
    a->head = NULL;
    a->total = 0;
    a->nomem = false;
}

void sv_arena_fini(sv_arena *a)
{
    sv_chunk *c = a->head;
    while (c != NULL) {
        sv_chunk *n = c->next;
        free(c);
        c = n;
    }
    sv_arena_init(a);
}

void *sv_alloc(sv_arena *a, size_t n)
{
    size_t align = sizeof(max_align_t);
    size_t need;
    sv_chunk *c = a->head;
    void *p;
    if (n == 0) {
        n = 1;
    }
    if (n > SV_ARENA_MAX) {
        a->nomem = true;
        return NULL;
    }
    need = (n + align - 1) / align * align;
    if (c == NULL || c->cap - c->used < need) {
        size_t cap = need > SV_CHUNK ? need : SV_CHUNK;
        if (a->total + cap > SV_ARENA_MAX) {
            a->nomem = true;
            return NULL;
        }
        c = malloc(offsetof(sv_chunk, data) + cap);
        if (c == NULL) {
            a->nomem = true;
            return NULL;
        }
        c->used = 0;
        c->cap = cap;
        c->next = a->head;
        a->head = c;
        a->total += cap;
    }
    p = (char *)c->data + c->used;
    c->used += need;
    memset(p, 0, n);
    return p;
}

void *sv_alloc_array(sv_arena *a, size_t count, size_t size)
{
    if (size != 0 && count > SV_ARENA_MAX / size) {
        a->nomem = true;
        return NULL;
    }
    return sv_alloc(a, count * size);
}

char *sv_strndup(sv_arena *a, const char *s, size_t n)
{
    char *p = sv_alloc(a, n + 1);
    if (p != NULL && n > 0) {
        memcpy(p, s, n);
    }
    return p;
}

bool sv_grow(sv_arena *a, void **v, size_t *cap, size_t need, size_t elem)
{
    size_t nc;
    void *nv;
    if (need <= *cap) {
        return true;
    }
    nc = *cap < 8 ? 8 : *cap * 2;
    while (nc < need) {
        nc *= 2;
    }
    nv = sv_alloc_array(a, nc, elem);
    if (nv == NULL) {
        return false;
    }
    if (*v != NULL && *cap > 0) {
        memcpy(nv, *v, *cap * elem);
    }
    *v = nv;
    *cap = nc;
    return true;
}

/* ---------------------------------------------------------------- XML */

#define MAX_DEPTH 1024
#define MAX_ENTITY_DEPTH 8
#define MAX_EXPANSION ((size_t)16 << 20)

typedef struct entity {
    const char *name;
    size_t nlen;
    const char *value;
    size_t vlen;
} entity;

/* A namespace binding in scope: prefix ("" = default) -> uri (NULL = unbound). */
typedef struct nsbind {
    const char *prefix;
    size_t plen;
    const char *uri;
    size_t depth;
} nsbind;

typedef struct xp {
    sv_arena *a;
    const char *s;
    size_t len;
    size_t pos;
    entity *ents;
    size_t nents, capents;
    nsbind *ns;
    size_t nns, capns;
    size_t expanded;
    char *msg;
    size_t cap;
    bool nomem;
} xp;

/* Growable byte buffer in the arena. */
typedef struct tbuf {
    char *p;
    size_t n, cap;
} tbuf;

static bool tb_put(xp *x, tbuf *b, const char *s, size_t n)
{
    void *v = b->p;
    if (!sv_grow(x->a, &v, &b->cap, b->n + n + 1, 1)) {
        x->nomem = true;
        return false;
    }
    b->p = v;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
    return true;
}

static bool fail(xp *x, const char *what)
{
    size_t line = 1;
    size_t col = 1;
    size_t i;
    for (i = 0; i < x->pos && i < x->len; i++) {
        if (x->s[i] == '\n') {
            line++;
            col = 1;
        } else if (((unsigned char)x->s[i] & 0xC0) != 0x80) {
            col++;
        }
    }
    if (x->msg != NULL && x->msg[0] == '\0') {
        if (strncmp(what, "the ", 4) == 0) {
            (void)snprintf(x->msg, x->cap, "%s", what); /* roxmltree gives these no position */
        } else {
            (void)snprintf(x->msg, x->cap, "%s at %zu:%zu", what, line, col);
        }
    }
    return false;
}

static bool at_end(const xp *x)
{
    return x->pos >= x->len;
}

static char cur(const xp *x)
{
    return x->pos < x->len ? x->s[x->pos] : '\0';
}

static bool starts(const xp *x, const char *t)
{
    size_t n = strlen(t);
    return x->len - x->pos >= n && memcmp(x->s + x->pos, t, n) == 0;
}

static bool is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_ws(xp *x)
{
    while (!at_end(x) && is_ws(cur(x))) {
        x->pos++;
    }
}

static bool is_name_start(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
}

static bool is_name_char(unsigned char c)
{
    return is_name_start(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

static bool name(xp *x, const char **out, size_t *n)
{
    size_t st = x->pos;
    if (at_end(x) || !is_name_start((unsigned char)cur(x))) {
        return fail(x, "invalid name token");
    }
    while (!at_end(x) && is_name_char((unsigned char)cur(x))) {
        x->pos++;
    }
    *out = x->s + st;
    *n = x->pos - st;
    return true;
}

static const entity *find_entity(const xp *x, const char *nm, size_t n)
{
    size_t i;
    for (i = 0; i < x->nents; i++) {
        if (x->ents[i].nlen == n && memcmp(x->ents[i].name, nm, n) == 0) {
            return &x->ents[i];
        }
    }
    return NULL;
}

static bool put_cp(xp *x, tbuf *b, unsigned long cp)
{
    char u[4];
    size_t n;
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return fail(x, "invalid character reference");
    }
    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    return tb_put(x, b, u, n);
}

static bool expand(xp *x, tbuf *b, const char *s, size_t n, bool attr, size_t depth);

/* A reference at s[*i] ('&' ... ';') appended to b. */
static bool reference(xp *x, tbuf *b, const char *s, size_t n, size_t *i, bool attr, size_t depth)
{
    size_t st = *i + 1;
    size_t e = st;
    while (e < n && s[e] != ';' && e - st < 64) {
        e++;
    }
    if (e >= n || s[e] != ';' || e == st) {
        return fail(x, "invalid reference");
    }
    *i = e + 1;
    if (s[st] == '#') {
        unsigned long cp = 0;
        size_t k = st + 1;
        bool hex = k < e && s[k] == 'x';
        if (hex) {
            k++;
        }
        if (k == e) {
            return fail(x, "invalid character reference");
        }
        for (; k < e; k++) {
            char c = s[k];
            int d = c >= '0' && c <= '9'          ? c - '0'
                    : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : -1;
            if (d < 0 || cp > 0x10FFFF) {
                return fail(x, "invalid character reference");
            }
            cp = cp * (hex ? 16u : 10u) + (unsigned long)d;
        }
        return put_cp(x, b, cp);
    }
    {
        size_t nl = e - st;
        const char *nm = s + st;
        const entity *ent;
        if (nl == 2 && memcmp(nm, "lt", 2) == 0) {
            return tb_put(x, b, "<", 1);
        }
        if (nl == 2 && memcmp(nm, "gt", 2) == 0) {
            return tb_put(x, b, ">", 1);
        }
        if (nl == 3 && memcmp(nm, "amp", 3) == 0) {
            return tb_put(x, b, "&", 1);
        }
        if (nl == 4 && memcmp(nm, "apos", 4) == 0) {
            return tb_put(x, b, "'", 1);
        }
        if (nl == 4 && memcmp(nm, "quot", 4) == 0) {
            return tb_put(x, b, "\"", 1);
        }
        ent = find_entity(x, nm, nl);
        if (ent == NULL) {
            return fail(x, "unknown entity reference");
        }
        if (depth >= MAX_ENTITY_DEPTH) {
            return fail(x, "entity reference loop detected");
        }
        x->expanded += ent->vlen;
        if (x->expanded > MAX_EXPANSION) {
            return fail(x, "entities expand past the size limit");
        }
        return expand(x, b, ent->value, ent->vlen, attr, depth + 1);
    }
}

/* Appends s with references expanded; attribute values get whitespace normalised. */
static bool expand(xp *x, tbuf *b, const char *s, size_t n, bool attr, size_t depth)
{
    size_t i = 0;
    while (i < n) {
        size_t st = i;
        while (i < n && s[i] != '&' && !(attr && (s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) &&
               !(!attr && s[i] == '\r')) {
            i++;
        }
        if (i > st && !tb_put(x, b, s + st, i - st)) {
            return false;
        }
        if (i >= n) {
            break;
        }
        if (s[i] == '&') {
            if (!reference(x, b, s, n, &i, attr, depth)) {
                return false;
            }
        } else if (s[i] == '\r') {
            /* \r\n and lone \r become one \n (one space in attributes). */
            if (!tb_put(x, b, attr ? " " : "\n", 1)) {
                return false;
            }
            i++;
            if (i < n && s[i] == '\n') {
                i++;
            }
        } else {
            if (!tb_put(x, b, " ", 1)) {
                return false;
            }
            i++;
        }
    }
    return true;
}

static bool skip_until(xp *x, const char *end, const char *what)
{
    size_t n = strlen(end);
    while (x->len - x->pos >= n) {
        if (memcmp(x->s + x->pos, end, n) == 0) {
            x->pos += n;
            return true;
        }
        x->pos++;
    }
    x->pos = x->len;
    return fail(x, what);
}

static bool quoted(xp *x, const char **v, size_t *n)
{
    char q = cur(x);
    size_t st;
    if (q != '"' && q != '\'') {
        return fail(x, "expected a quote");
    }
    x->pos++;
    st = x->pos;
    while (!at_end(x) && cur(x) != q) {
        x->pos++;
    }
    if (at_end(x)) {
        return fail(x, "unexpected end of stream");
    }
    *v = x->s + st;
    *n = x->pos - st;
    x->pos++;
    return true;
}

/* <!DOCTYPE name ... [ internal subset ]>: general entities are kept, the rest skipped. */
static bool doctype(xp *x)
{
    x->pos += 9;
    while (!at_end(x) && cur(x) != '[' && cur(x) != '>') {
        if (cur(x) == '"' || cur(x) == '\'') {
            const char *v;
            size_t n;
            if (!quoted(x, &v, &n)) {
                return false;
            }
        } else {
            x->pos++;
        }
    }
    if (cur(x) == '[') {
        x->pos++;
        for (;;) {
            skip_ws(x);
            if (at_end(x)) {
                return fail(x, "unexpected end of stream");
            }
            if (cur(x) == ']') {
                x->pos++;
                break;
            }
            if (starts(x, "<!--")) {
                if (!skip_until(x, "-->", "unclosed comment")) {
                    return false;
                }
            } else if (starts(x, "<!ENTITY")) {
                const char *nm = NULL;
                size_t nl = 0;
                bool param = false;
                x->pos += 8;
                skip_ws(x);
                if (cur(x) == '%') {
                    param = true;
                    x->pos++;
                    skip_ws(x);
                }
                if (!name(x, &nm, &nl)) {
                    return false;
                }
                skip_ws(x);
                if (cur(x) == '"' || cur(x) == '\'') {
                    const char *v = NULL;
                    size_t vn = 0;
                    if (!quoted(x, &v, &vn)) {
                        return false;
                    }
                    if (!param && find_entity(x, nm, nl) == NULL) {
                        void *e = x->ents;
                        if (!sv_grow(x->a, &e, &x->capents, x->nents + 1, sizeof(entity))) {
                            x->nomem = true;
                            return false;
                        }
                        x->ents = e;
                        x->ents[x->nents].name = nm;
                        x->ents[x->nents].nlen = nl;
                        x->ents[x->nents].value = v;
                        x->ents[x->nents].vlen = vn;
                        x->nents++;
                    }
                }
                if (!skip_until(x, ">", "unclosed entity declaration")) {
                    return false;
                }
            } else if (starts(x, "<?")) {
                if (!skip_until(x, "?>", "unclosed processing instruction")) {
                    return false;
                }
            } else if (cur(x) == '<' || cur(x) == '%') {
                /* Other markup declarations and parameter entity references. */
                while (!at_end(x) && cur(x) != '>' && cur(x) != ';') {
                    if (cur(x) == '"' || cur(x) == '\'') {
                        const char *v;
                        size_t n;
                        if (!quoted(x, &v, &n)) {
                            return false;
                        }
                    } else {
                        x->pos++;
                    }
                }
                if (at_end(x)) {
                    return fail(x, "unexpected end of stream");
                }
                x->pos++;
            } else {
                return fail(x, "invalid DTD");
            }
        }
        skip_ws(x);
    }
    if (cur(x) != '>') {
        return fail(x, "invalid DTD");
    }
    x->pos++;
    return true;
}

static sx_node *new_node(xp *x, sx_node *parent)
{
    sx_node *n = sv_alloc(x->a, sizeof *n);
    if (n == NULL) {
        x->nomem = true;
        return NULL;
    }
    n->parent = parent;
    if (parent->last != NULL) {
        parent->last->next = n;
        n->prev = parent->last;
    } else {
        parent->first = n;
    }
    parent->last = n;
    return n;
}

static bool add_text(xp *x, sx_node *parent, const char *s, size_t n, bool raw)
{
    tbuf b = {NULL, 0, 0};
    sx_node *t = parent->last;
    if (n == 0) {
        return true;
    }
    if (t != NULL && t->is_text) {
        /* Merge with the previous text (text + CDATA + text is one node). */
        if (!tb_put(x, &b, t->text, strlen(t->text))) {
            return false;
        }
    } else {
        t = NULL;
    }
    if (raw ? !tb_put(x, &b, s, n) : !expand(x, &b, s, n, false, 0)) {
        return false;
    }
    if (t == NULL) {
        t = new_node(x, parent);
        if (t == NULL) {
            return false;
        }
        t->is_text = true;
    }
    t->text = b.p != NULL ? b.p : "";
    return true;
}

static const char *lookup_ns(const xp *x, const char *prefix, size_t plen, bool *bound)
{
    size_t i = x->nns;
    if (plen == 3 && memcmp(prefix, "xml", 3) == 0) {
        *bound = true;
        return SV_NS_XML;
    }
    while (i > 0) {
        i--;
        if (x->ns[i].plen == plen && (plen == 0 || memcmp(x->ns[i].prefix, prefix, plen) == 0)) {
            *bound = x->ns[i].uri != NULL || plen == 0;
            return x->ns[i].uri;
        }
    }
    *bound = plen == 0;
    return NULL;
}

typedef struct rawattr {
    const char *name;
    size_t nlen;
    const char *value;
    size_t vlen;
} rawattr;

/* A start tag at '<'; *elem gets the element, *empty for "/>". */
static bool start_tag(xp *x, sx_node *parent, size_t depth, sx_node **elem, bool *empty)
{
    const char *eof =
        depth <= 1 ? "the document does not have a root node" : "the root node was opened but never closed";
    const char *nm;
    size_t nl;
    rawattr *raw = NULL;
    size_t nraw = 0;
    size_t capraw = 0;
    sx_node *el;
    size_t i;
    const char *colon;
    bool bound;
    x->pos++;
    if (!name(x, &nm, &nl)) {
        return false;
    }
    for (;;) {
        bool ws = is_ws(cur(x));
        skip_ws(x);
        if (at_end(x)) {
            return fail(x, eof);
        }
        if (cur(x) == '>' || starts(x, "/>")) {
            break;
        }
        if (!ws) {
            return fail(x, "expected a space between attributes");
        }
        {
            rawattr r;
            void *v = raw;
            if (!name(x, &r.name, &r.nlen)) {
                return false;
            }
            skip_ws(x);
            if (cur(x) != '=') {
                return fail(x, "expected '='");
            }
            x->pos++;
            skip_ws(x);
            if (!quoted(x, &r.value, &r.vlen)) {
                return false;
            }
            if (memchr(r.value, '<', r.vlen) != NULL) {
                return fail(x, "'<' is not allowed in an attribute value");
            }
            for (i = 0; i < nraw; i++) {
                if (raw[i].nlen == r.nlen && memcmp(raw[i].name, r.name, r.nlen) == 0) {
                    return fail(x, "duplicated attribute");
                }
            }
            if (!sv_grow(x->a, &v, &capraw, nraw + 1, sizeof(rawattr))) {
                x->nomem = true;
                return false;
            }
            raw = v;
            raw[nraw++] = r;
        }
    }
    *empty = cur(x) == '/';
    x->pos += *empty ? 2 : 1;
    /* Namespace declarations first (they apply to the element's own name and attributes). */
    for (i = 0; i < nraw; i++) {
        bool is_default = raw[i].nlen == 5 && memcmp(raw[i].name, "xmlns", 5) == 0;
        bool is_prefixed = raw[i].nlen > 6 && memcmp(raw[i].name, "xmlns:", 6) == 0;
        if (is_default || is_prefixed) {
            tbuf b = {NULL, 0, 0};
            void *v = x->ns;
            if (!expand(x, &b, raw[i].value, raw[i].vlen, true, 0)) {
                return false;
            }
            if (!sv_grow(x->a, &v, &x->capns, x->nns + 1, sizeof(nsbind))) {
                x->nomem = true;
                return false;
            }
            x->ns = v;
            x->ns[x->nns].prefix = is_default ? "" : raw[i].name + 6;
            x->ns[x->nns].plen = is_default ? 0 : raw[i].nlen - 6;
            x->ns[x->nns].uri = b.n > 0 ? b.p : NULL;
            x->ns[x->nns].depth = depth;
            x->nns++;
        }
    }
    el = new_node(x, parent);
    if (el == NULL) {
        return false;
    }
    colon = memchr(nm, ':', nl);
    if (colon != NULL) {
        el->ns = lookup_ns(x, nm, (size_t)(colon - nm), &bound);
        if (!bound || el->ns == NULL) {
            return fail(x, "an unknown namespace prefix");
        }
        el->name = sv_strndup(x->a, colon + 1, nl - (size_t)(colon - nm) - 1);
    } else {
        el->ns = lookup_ns(x, "", 0, &bound);
        el->name = sv_strndup(x->a, nm, nl);
    }
    el->attrs = nraw > 0 ? sv_alloc_array(x->a, nraw, sizeof(sx_attr)) : NULL;
    if (el->name == NULL || (nraw > 0 && el->attrs == NULL)) {
        x->nomem = true;
        return false;
    }
    for (i = 0; i < nraw; i++) {
        const char *an = raw[i].name;
        size_t al = raw[i].nlen;
        tbuf b = {NULL, 0, 0};
        sx_attr *at;
        if ((al == 5 && memcmp(an, "xmlns", 5) == 0) || (al > 6 && memcmp(an, "xmlns:", 6) == 0)) {
            continue;
        }
        at = &el->attrs[el->nattrs];
        colon = memchr(an, ':', al);
        if (colon != NULL) {
            at->ns = lookup_ns(x, an, (size_t)(colon - an), &bound);
            if (!bound || at->ns == NULL) {
                return fail(x, "an unknown namespace prefix");
            }
            at->name = sv_strndup(x->a, colon + 1, al - (size_t)(colon - an) - 1);
        } else {
            at->ns = NULL; /* unprefixed attributes have no namespace */
            at->name = sv_strndup(x->a, an, al);
        }
        if (!expand(x, &b, raw[i].value, raw[i].vlen, true, 0)) {
            return false;
        }
        at->value = b.p != NULL ? b.p : "";
        if (at->name == NULL) {
            x->nomem = true;
            return false;
        }
        el->nattrs++;
    }
    *elem = el;
    return true;
}

static void pop_ns(xp *x, size_t depth)
{
    while (x->nns > 0 && x->ns[x->nns - 1].depth >= depth) {
        x->nns--;
    }
}

static bool parse_doc(xp *x, sx_node *docnode)
{
    sx_node *cur_el = docnode;
    size_t depth = 0;
    bool root_seen = false;
    size_t doc_start;
    if (starts(x, "\xEF\xBB\xBF")) {
        x->pos += 3;
    }
    doc_start = x->pos;
    while (!at_end(x)) {
        if (cur(x) == '<') {
            if (starts(x, "<!--")) {
                if (!skip_until(x, "-->", "unclosed comment")) {
                    return false;
                }
            } else if (starts(x, "<?")) {
                /* An XML declaration is only allowed at the very start (after a BOM). */
                if (starts(x, "<?xml") && x->pos + 5 < x->len &&
                    (is_ws(x->s[x->pos + 5]) || x->s[x->pos + 5] == '?') && x->pos != doc_start) {
                    return fail(x, "unexpected XML declaration");
                }
                if (!skip_until(x, "?>", "unclosed processing instruction")) {
                    return false;
                }
            } else if (starts(x, "<![CDATA[")) {
                size_t st;
                if (depth == 0) {
                    return fail(x, "unexpected CDATA");
                }
                x->pos += 9;
                st = x->pos;
                if (!skip_until(x, "]]>", "unclosed CDATA")) {
                    return false;
                }
                if (!add_text(x, cur_el, x->s + st, x->pos - 3 - st, true)) {
                    return false;
                }
            } else if (starts(x, "<!DOCTYPE")) {
                if (depth > 0 || root_seen) {
                    return fail(x, "unexpected DTD");
                }
                if (!doctype(x)) {
                    return false;
                }
            } else if (starts(x, "</")) {
                const char *nm;
                size_t nl;
                const char *colon;
                const char *local;
                size_t ll;
                if (depth == 0) {
                    return fail(x, "unexpected close tag");
                }
                x->pos += 2;
                if (!name(x, &nm, &nl)) {
                    return false;
                }
                skip_ws(x);
                if (cur(x) != '>') {
                    return fail(x, "expected '>'");
                }
                x->pos++;
                colon = memchr(nm, ':', nl);
                local = colon != NULL ? colon + 1 : nm;
                ll = colon != NULL ? nl - (size_t)(colon - nm) - 1 : nl;
                if (strlen(cur_el->name) != ll || memcmp(cur_el->name, local, ll) != 0) {
                    return fail(x, "unexpected close tag");
                }
                pop_ns(x, depth);
                depth--;
                cur_el = cur_el->parent;
            } else {
                sx_node *el = NULL;
                bool empty = false;
                if (depth == 0 && root_seen) {
                    return fail(x, "unknown token");
                }
                if (depth + 1 > MAX_DEPTH) {
                    return fail(x, "nodes limit reached");
                }
                if (!start_tag(x, cur_el, depth + 1, &el, &empty)) {
                    return false;
                }
                if (depth == 0) {
                    root_seen = true;
                }
                if (empty) {
                    pop_ns(x, depth + 1);
                } else {
                    cur_el = el;
                    depth++;
                }
            }
        } else {
            size_t st = x->pos;
            while (!at_end(x) && cur(x) != '<') {
                x->pos++;
            }
            if (depth == 0) {
                size_t i;
                for (i = st; i < x->pos; i++) {
                    if (!is_ws(x->s[i])) {
                        x->pos = i;
                        return fail(x, "unknown token");
                    }
                }
            } else if (!add_text(x, cur_el, x->s + st, x->pos - st, false)) {
                return false;
            }
        }
    }
    if (depth > 0) {
        return fail(x, "the root node was opened but never closed");
    }
    if (!root_seen) {
        return fail(x, "the document does not have a root node");
    }
    return true;
}

pc_status sx_parse(sv_arena *a, const char *text, size_t len, sx_doc *out, char *msg, size_t cap)
{
    xp x;
    sx_node *doc;
    memset(&x, 0, sizeof x);
    x.a = a;
    x.s = text;
    x.len = len;
    x.msg = msg;
    x.cap = cap;
    if (msg != NULL && cap > 0) {
        msg[0] = '\0';
    }
    doc = sv_alloc(a, sizeof *doc);
    if (doc == NULL) {
        return PC_ERR_NOMEM;
    }
    doc->name = "";
    if (!parse_doc(&x, doc)) {
        if (x.nomem || a->nomem) {
            return PC_ERR_NOMEM;
        }
        return PC_ERR_FORMAT;
    }
    if (x.nomem || a->nomem) {
        return PC_ERR_NOMEM;
    }
    out->root = doc;
    return PC_OK;
}

const char *sx_attr_get(const sx_node *n, const char *ns, const char *nm)
{
    size_t i;
    for (i = 0; n != NULL && i < n->nattrs; i++) {
        const sx_attr *at = &n->attrs[i];
        if (strcmp(at->name, nm) != 0) {
            continue;
        }
        if ((ns == NULL && at->ns == NULL) || (ns != NULL && at->ns != NULL && strcmp(ns, at->ns) == 0)) {
            return at->value;
        }
    }
    return NULL;
}

const sx_node *sx_first_element(const sx_node *n)
{
    const sx_node *c = n != NULL ? n->first : NULL;
    while (c != NULL && c->is_text) {
        c = c->next;
    }
    return c;
}

const sx_node *sx_next_element(const sx_node *n)
{
    const sx_node *c = n->next;
    while (c != NULL && c->is_text) {
        c = c->next;
    }
    return c;
}

const sx_node *sx_prev_element(const sx_node *n)
{
    const sx_node *c = n->prev;
    while (c != NULL && c->is_text) {
        c = c->prev;
    }
    return c;
}

const char *sx_text(const sx_node *n)
{
    if (n == NULL) {
        return NULL;
    }
    if (n->is_text) {
        return n->text;
    }
    return n->first != NULL && n->first->is_text ? n->first->text : NULL;
}
