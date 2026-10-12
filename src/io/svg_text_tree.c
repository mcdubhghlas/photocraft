/* Text preprocessing while the svgtree is built (port of usvg 0.45.1 parser/svgtree/text.rs) and
 * the font value parsers of svgtypes 0.15 (font.rs): the `font` shorthand and font-family lists.
 *
 * <text> keeps its tspan/tref/textPath children (an <a> counts as a tspan, a tref becomes a tspan
 * holding the referenced element's character data, a textPath must be a direct child) and its
 * character data as text nodes, with tabs and newlines made spaces, runs of spaces collapsed
 * (xml:space="default") and spaces trimmed across node boundaries the way Chrome does.
 *
 * Bounded: nesting is the XML parser's (at most 1024 deep), so the recursion is too; <tref> text
 * shares one budget per document (st_build_tref_budget) so a reference repeated many times can't
 * multiply a large text. */
#include <stdlib.h>
#include <string.h>

#include "io/svg_text.h"

typedef enum { SP_DEFAULT, SP_PRESERVE } xmlspace;

static xmlspace get_xmlspace(const st_node *n, xmlspace def)
{
    const char *v = st_attr(n, A_SPACE);
    if (v == NULL) {
        return def;
    }
    return strcmp(v, "preserve") == 0 ? SP_PRESERVE : SP_DEFAULT;
}

/* trim_text: \r \n \t become spaces; outside xml:space="preserve" runs of spaces collapse. */
static char *trim_text(sv_arena *a, const char *t, size_t n, xmlspace sp)
{
    char *s = sv_alloc(a, n + 1);
    size_t i;
    size_t k = 0;
    char prev = '0';
    if (s == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        char c = t[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        if (sp == SP_DEFAULT && c == ' ' && c == prev) {
            continue;
        }
        prev = c;
        s[k++] = c;
    }
    s[k] = '\0';
    return s;
}

/* The character data below x (descendants().filter(is_text)), NULL when empty or past budget. */
static char *element_text(st_builder *b, const sx_node *x)
{
    sv_arena *a = st_build_arena(b);
    size_t *budget = st_build_tref_budget(b);
    size_t total = 0;
    const sx_node *c = x->first;
    char *s;
    size_t k = 0;
    /* first pass: length */
    while (c != NULL) {
        if (c->is_text) {
            size_t l = strlen(c->text);
            if (l > *budget - total) {
                return NULL;
            }
            total += l;
        }
        if (c->first != NULL) {
            c = c->first;
            continue;
        }
        while (c != NULL && c != x && c->next == NULL) {
            c = c->parent;
        }
        if (c == NULL || c == x) {
            break;
        }
        c = c->next;
    }
    if (total == 0) {
        return NULL;
    }
    s = sv_alloc(a, total + 1);
    if (s == NULL) {
        return NULL;
    }
    *budget -= total;
    c = x->first;
    while (c != NULL) {
        if (c->is_text) {
            size_t l = strlen(c->text);
            memcpy(s + k, c->text, l);
            k += l;
        }
        if (c->first != NULL) {
            c = c->first;
            continue;
        }
        while (c != NULL && c != x && c->next == NULL) {
            c = c->parent;
        }
        if (c == NULL || c == x) {
            break;
        }
        c = c->next;
    }
    s[k] = '\0';
    return s;
}

static char *resolve_tref_text(st_builder *b, const char *href)
{
    const char *id;
    size_t len;
    const sx_node *node;
    sv_eid tag;
    if (!sv_parse_iri(href, &id, &len)) {
        return NULL;
    }
    node = st_build_xml_by_id(b, id, len);
    if (node == NULL || !st_build_tag(node, &tag)) {
        return NULL;
    }
    return element_text(b, node);
}

static bool parse_impl(st_builder *b, const sx_node *x, st_node *n, xmlspace space)
{
    sv_arena *a = st_build_arena(b);
    const sx_node *c;
    for (c = x->first; c != NULL; c = c->next) {
        sv_eid tag;
        bool is_tref = false;
        st_node *node;
        xmlspace sp2;
        if (c->is_text) {
            char *t = trim_text(a, c->text, strlen(c->text), space);
            if (t == NULL || st_build_text(b, n, t) == NULL) {
                return false;
            }
            continue;
        }
        if (!st_build_tag(c, &tag)) {
            continue;
        }
        if (tag == E_A) {
            tag = E_TSPAN;
        }
        if (tag != E_TSPAN && tag != E_TREF && tag != E_TEXT_PATH) {
            continue;
        }
        if (tag == E_TEXT_PATH && strcmp(x->name, "text") != 0) {
            continue;
        }
        if (tag == E_TREF) {
            tag = E_TSPAN;
            is_tref = true;
        }
        node = st_build_element(b, c, n, tag);
        if (node == NULL) {
            return false;
        }
        sp2 = get_xmlspace(node, space);
        if (is_tref) {
            const char *href = sx_attr_get(c, SV_NS_XLINK, "href");
            if (href == NULL) {
                href = sx_attr_get(c, NULL, "href");
            }
            if (href != NULL) {
                char *text = resolve_tref_text(b, href);
                if (text != NULL) {
                    char *t = trim_text(a, text, strlen(text), sp2);
                    if (t == NULL || st_build_text(b, node, t) == NULL) {
                        return false;
                    }
                } else if (a->nomem) {
                    return false;
                }
            }
        } else if (!parse_impl(b, c, node, sp2)) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- trim_text_nodes */

typedef struct tnode {
    st_node *n;
    size_t depth;
} tnode;

typedef struct tlist {
    tnode *v;
    size_t n, cap;
} tlist;

/* collect_text_nodes (pre-order, element depth). Iterative: nesting can be 1024 deep. */
static bool collect(sv_arena *a, st_node *root, tlist *out)
{
    st_node *c = root->first;
    size_t depth = 0;
    while (c != NULL) {
        if (c->kind == ST_TEXT) {
            void *v = out->v;
            if (!sv_grow(a, &v, &out->cap, out->n + 1, sizeof(tnode))) {
                return false;
            }
            out->v = v;
            out->v[out->n].n = c;
            out->v[out->n].depth = depth;
            out->n++;
        } else if (c->kind == ST_ELEMENT && c->first != NULL) {
            c = c->first;
            depth++;
            continue;
        }
        while (c != NULL && c != root && c->next == NULL) {
            c = c->parent;
            depth--;
        }
        if (c == NULL || c == root) {
            break;
        }
        c = c->next;
    }
    return true;
}

static char *mtext(st_node *n)
{
    return (char *)(uintptr_t)n->text;
}

static size_t tlen(const st_node *n)
{
    return strlen(n->text);
}

static void remove_first(st_node *n)
{
    if (n->text[0] != '\0') {
        n->text++;
    }
}

/* String::pop: removes the last character. */
static void remove_last(st_node *n)
{
    char *t = mtext(n);
    size_t l = strlen(t);
    if (l == 0) {
        return;
    }
    l--;
    while (l > 0 && ((unsigned char)t[l] & 0xC0) == 0x80) {
        l--;
    }
    t[l] = '\0';
}

static bool trim_is_empty(const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r') {
            unsigned char u = (unsigned char)*s;
            /* str::trim also strips Unicode white space; the common ones in UTF-8: */
            if (u == 0xC2 && (unsigned char)s[1] == 0x85) {
                s++;
                continue;
            }
            if (u == 0xC2 && (unsigned char)s[1] == 0xA0) {
                s++;
                continue;
            }
            if (u == 0xE3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80) {
                s += 2;
                continue;
            }
            if (u == 0xE2 && (unsigned char)s[1] == 0x80 &&
                (((unsigned char)s[2] >= 0x80 && (unsigned char)s[2] <= 0x8A) ||
                 (unsigned char)s[2] == 0xA8 || (unsigned char)s[2] == 0xA9 || (unsigned char)s[2] == 0xAF)) {
                s += 2;
                continue;
            }
            if (u == 0xE2 && (unsigned char)s[1] == 0x81 && (unsigned char)s[2] == 0x9F) {
                s += 2;
                continue;
            }
            if (u == 0xE1 && (unsigned char)s[1] == 0x9A && (unsigned char)s[2] == 0x80) {
                s += 2;
                continue;
            }
            if (*s == '\v' || *s == '\f') {
                continue;
            }
            return false;
        }
    }
    return true;
}

static void trim_text_nodes(sv_arena *a, st_node *text, xmlspace space, bool *ok)
{
    tlist l = {NULL, 0, 0};
    size_t i;
    size_t len;
    st_node *last_non_empty = NULL;
    if (!collect(a, text, &l)) {
        *ok = false;
        return;
    }
    if (l.n == 1) {
        st_node *n = l.v[0].n;
        size_t tl = tlen(n);
        if (space == SP_DEFAULT && tl > 0) {
            if (tl == 1) {
                if (n->text[0] == ' ') {
                    mtext(n)[0] = '\0';
                }
            } else {
                bool c1 = n->text[0] == ' ';
                bool c2 = n->text[tl - 1] == ' ';
                if (c1) {
                    remove_first(n);
                }
                if (c2) {
                    remove_last(n);
                }
            }
        }
        return;
    }
    if (l.n < 2) {
        return;
    }
    len = l.n - 1;
    for (i = 0; i < len; i++) {
        st_node *n1 = l.v[i].n;
        st_node *n2 = l.v[i + 1].n;
        size_t d1 = l.v[i].depth;
        size_t d2 = l.v[i + 1].depth;
        xmlspace s1;
        xmlspace s2;
        int c1, c2, c3, c4; /* -1: none */
        size_t l1, l2;
        bool is_first = i == 0;
        bool is_last = i == len - 1;
        if (n1->text[0] == '\0' && last_non_empty != NULL) {
            n1 = last_non_empty;
        }
        s1 = get_xmlspace(n1->parent, space);
        s2 = get_xmlspace(n2->parent, space);
        l1 = tlen(n1);
        l2 = tlen(n2);
        c1 = l1 > 0 ? (unsigned char)n1->text[0] : -1;
        c2 = l1 > 0 ? (unsigned char)n1->text[l1 - 1] : -1;
        c3 = l2 > 0 ? (unsigned char)n2->text[0] : -1;
        c4 = l2 > 0 ? (unsigned char)n2->text[l2 - 1] : -1;
        if (d1 < d2) {
            if (c3 == ' ' && s2 == SP_DEFAULT) {
                remove_first(n2);
            }
        } else if (c2 == ' ' && c2 == c3) {
            if (s1 == SP_DEFAULT && s2 == SP_DEFAULT) {
                remove_last(n1);
            } else if (s1 == SP_PRESERVE && s2 == SP_DEFAULT) {
                remove_first(n2);
            }
        }
        if (is_first && c1 == ' ' && s1 == SP_DEFAULT && n1->text[0] != '\0') {
            remove_first(n1);
        } else if (is_last && c4 == ' ' && n2->text[0] != '\0' && s2 == SP_DEFAULT) {
            remove_last(n2);
        }
        if (is_last && c2 == ' ' && n1->text[0] != '\0' && n2->text[0] == '\0') {
            size_t k = tlen(n1);
            if (k > 0 && n1->text[k - 1] == ' ') {
                remove_last(n1);
            }
        }
        if (!trim_is_empty(n1->text)) {
            last_non_empty = n1;
        }
    }
}

bool st_text_parse(st_builder *b, const sx_node *x, st_node *n)
{
    xmlspace space = SP_DEFAULT;
    bool ok = true;
    if (st_has(n, A_SPACE)) {
        space = get_xmlspace(n, SP_DEFAULT);
    } else {
        const st_node *p;
        for (p = n->parent; p != NULL; p = p->parent) {
            if (st_has(p, A_SPACE)) {
                space = get_xmlspace(p, SP_DEFAULT);
                break;
            }
        }
    }
    if (!parse_impl(b, x, n, space)) {
        return false;
    }
    trim_text_nodes(st_build_arena(b), n, space, &ok);
    return ok;
}

/* ---------------------------------------------------------------- svgtypes font.rs */

typedef struct fs {
    const char *s;
    size_t len, pos;
} fs;

static bool fs_end(const fs *f)
{
    return f->pos >= f->len;
}

static void fs_skip_spaces(fs *f)
{
    while (!fs_end(f) &&
           (f->s[f->pos] == ' ' || f->s[f->pos] == '\t' || f->s[f->pos] == '\n' || f->s[f->pos] == '\r')) {
        f->pos++;
    }
}

/* The code point at pos and its UTF-8 length (input is valid UTF-8: the XML parser checks it). */
static uint32_t fs_char(const fs *f, size_t *n)
{
    const unsigned char *p = (const unsigned char *)f->s + f->pos;
    size_t left = f->len - f->pos;
    if (p[0] < 0x80 || left < 2) {
        *n = 1;
        return p[0];
    }
    if (p[0] < 0xE0 || left < 3) {
        *n = 2;
        return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    }
    if (p[0] < 0xF0 || left < 4) {
        *n = 3;
        return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    }
    *n = 4;
    return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 |
           (p[3] & 0x3F);
}

static bool name_start(uint32_t c)
{
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c > 237;
}

static bool name_char(uint32_t c)
{
    return name_start(c) || (c >= '0' && c <= '9') || c == '-';
}

/* Stream::parse_ident. */
static bool fs_ident(fs *f, size_t *start)
{
    size_t n;
    *start = f->pos;
    if (!fs_end(f) && f->s[f->pos] == '-') {
        f->pos++;
    }
    if (!fs_end(f)) {
        uint32_t c = fs_char(f, &n);
        if (!name_start(c)) {
            return false;
        }
        f->pos += n;
        while (!fs_end(f)) {
            c = fs_char(f, &n);
            if (!name_char(c)) {
                break;
            }
            f->pos += n;
        }
    }
    return f->pos != *start;
}

static size_t fs_ascii_ident(fs *f)
{
    size_t s = f->pos;
    while (!fs_end(f)) {
        char c = f->s[f->pos];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' ||
            c == '_') {
            f->pos++;
        } else {
            break;
        }
    }
    return s;
}

static bool is(const fs *f, size_t s, const char *w)
{
    size_t l = strlen(w);
    return f->pos - s == l && memcmp(f->s + s, w, l) == 0;
}

static bool any_of(const fs *f, size_t s, const char *const *list, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (is(f, s, list[i])) {
            return true;
        }
    }
    return false;
}

bool svf_parse_families(sv_arena *a, const char *s, svf_family **out, size_t *nout)
{
    fs f = {s, strlen(s), 0};
    svf_family *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    while (!fs_end(&f)) {
        svf_family fam;
        char q;
        fs_skip_spaces(&f);
        if (fs_end(&f)) {
            return false;
        }
        q = f.s[f.pos];
        if (q == '\'' || q == '"') {
            size_t st;
            char prev = q;
            f.pos++;
            st = f.pos;
            while (!fs_end(&f)) {
                char cur = f.s[f.pos];
                if (cur == q && prev != '\\') {
                    break;
                }
                prev = cur;
                f.pos++;
            }
            if (fs_end(&f)) {
                return false;
            }
            fam.kind = SVF_NAMED;
            fam.name = sv_strndup(a, f.s + st, f.pos - st);
            f.pos++;
        } else {
            /* idents joined by single spaces */
            char *buf = sv_alloc(a, f.len - f.pos + 1);
            size_t k = 0;
            if (buf == NULL) {
                return false;
            }
            while (!fs_end(&f) && f.s[f.pos] != ',') {
                size_t st;
                if (!fs_ident(&f, &st)) {
                    return false;
                }
                if (k > 0) {
                    buf[k++] = ' ';
                }
                memcpy(buf + k, f.s + st, f.pos - st);
                k += f.pos - st;
                fs_skip_spaces(&f);
            }
            buf[k] = '\0';
            fam.name = buf;
            fam.kind = strcmp(buf, "serif") == 0        ? SVF_SERIF
                       : strcmp(buf, "sans-serif") == 0 ? SVF_SANS
                       : strcmp(buf, "cursive") == 0    ? SVF_CURSIVE
                       : strcmp(buf, "fantasy") == 0    ? SVF_FANTASY
                       : strcmp(buf, "monospace") == 0  ? SVF_MONO
                                                        : SVF_NAMED;
        }
        if (fam.name == NULL) {
            return false;
        }
        if (fam.kind != SVF_NAMED || fam.name[0] != '\0') {
            void *p = v;
            if (!sv_grow(a, &p, &cap, n + 1, sizeof(svf_family))) {
                return false;
            }
            v = p;
            v[n++] = fam;
        }
        if (!fs_end(&f)) {
            if (f.s[f.pos] == ',') {
                f.pos++;
            } else {
                break;
            }
        }
    }
    fs_skip_spaces(&f);
    if (!fs_end(&f)) {
        return false;
    }
    *out = v;
    *nout = n;
    return true;
}

bool sv_parse_font_shorthand(sv_arena *a, const char *s, sv_font_shorthand *out)
{
    static const char *const WEIGHTS[12] = {"bold", "bolder", "lighter", "100", "200", "300",
                                            "400",  "500",    "600",     "700", "800", "900"};
    static const char *const STRETCHES[8] = {"ultra-condensed", "extra-condensed", "condensed",
                                             "semi-condensed",  "semi-expanded",   "expanded",
                                             "extra-expanded",  "ultra-expanded"};
    static const char *const SIZES[9] = {"xx-small", "x-small",  "small",  "medium", "large",
                                         "x-large",  "xx-large", "larger", "smaller"};
    fs f = {s, strlen(s), 0};
    size_t prev;
    size_t i;
    sv_font_shorthand r;
    memset(&r, 0, sizeof r);
    fs_skip_spaces(&f);
    prev = f.pos;
    for (i = 0; i < 4; i++) {
        size_t st = fs_ascii_ident(&f);
        const char **slot = NULL;
        if (is(&f, st, "normal")) {
            /* ambiguous: ignored */
        } else if (is(&f, st, "small-caps")) {
            slot = &r.variant;
        } else if (is(&f, st, "italic") || is(&f, st, "oblique")) {
            slot = &r.style;
        } else if (any_of(&f, st, WEIGHTS, 12)) {
            slot = &r.weight;
        } else if (any_of(&f, st, STRETCHES, 8)) {
            slot = &r.stretch;
        } else {
            f.pos = prev;
            break;
        }
        if (slot != NULL) {
            *slot = sv_strndup(a, f.s + st, f.pos - st);
            if (*slot == NULL) {
                return false;
            }
        }
        fs_skip_spaces(&f);
        prev = f.pos;
    }
    prev = f.pos;
    if (fs_end(&f)) {
        return false;
    }
    if (f.s[f.pos] >= '0' && f.s[f.pos] <= '9') {
        sv_stream st;
        sv_length l;
        st.s = f.s;
        st.len = f.len;
        st.pos = f.pos;
        if (!ss_parse_length(&st, &l)) {
            return false;
        }
        f.pos = st.pos;
    } else {
        size_t st = fs_ascii_ident(&f);
        if (!any_of(&f, st, SIZES, 9)) {
            return false;
        }
    }
    r.size = sv_strndup(a, f.s + prev, f.pos - prev);
    fs_skip_spaces(&f);
    if (fs_end(&f)) {
        return false;
    }
    if (f.s[f.pos] == '/') {
        sv_stream st;
        sv_length l;
        f.pos++;
        fs_skip_spaces(&f);
        st.s = f.s;
        st.len = f.len;
        st.pos = f.pos;
        if (!ss_parse_length(&st, &l)) {
            return false;
        }
        f.pos = st.pos;
        fs_skip_spaces(&f);
    }
    if (fs_end(&f)) {
        return false;
    }
    r.family = sv_strndup(a, f.s + f.pos, f.len - f.pos);
    if (r.size == NULL || r.family == NULL) {
        return false;
    }
    *out = r;
    return true;
}
