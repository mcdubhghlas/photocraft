#define _XOPEN_SOURCE 700 /* realpath (D21) */
/* The fontconfig files fontdb 0.23 reads on Linux and the BSDs (its `fontconfig` feature, through
 * fontconfig-parser 0.5.8): which directories hold fonts and which families the generic names
 * stand for. Only what fontdb uses is interpreted: <dir>, <reset-dirs>, <include> (files and
 * conf.d directories in lexicographic order) and <alias>. As in fontconfig-parser, a part that
 * fails to parse (a <dir> or <include> without text, an unknown prefix, an <alias> without a
 * family, a <test> or <edit> without a value) ends the processing of that file; parts before it
 * stay applied, and including the file still succeeds for its parent. <match>, <selectfont> and
 * <config> values are not validated further (fontconfig-parser parses their expressions and may
 * reject odd ones: REWRITE_PLAN.md section 7).
 *
 * Bounded: at most MAX_FILES configuration files, each at most 4 MiB, includes at most
 * MAX_DEPTH deep, MAX_DIRS directories. Everything is malloc'd; svf_fc_fini frees it. */
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/buf.h"
#include "base/file.h"
#include "io/svg_text.h"

#define MAX_FILES 4096u
#define MAX_DEPTH 32u
#define MAX_DIRS 4096u
#define MAX_CONF_BYTES ((size_t)4 << 20)

typedef struct strv {
    char **v;
    size_t n, cap;
} strv;

typedef struct fcs {
    svf_fc *fc;
    strv seen; /* canonical paths of merged files (FontConfig::config_files) */
    bool nomem;
} fcs;

static char *xstrdup(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d != NULL) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

static bool strv_push(strv *s, char *p)
{
    if (p == NULL) {
        return false;
    }
    if (s->n == s->cap) {
        size_t nc = s->cap < 8 ? 8 : s->cap * 2;
        char **v = realloc(s->v, nc * sizeof *v);
        if (v == NULL) {
            free(p);
            return false;
        }
        s->v = v;
        s->cap = nc;
    }
    s->v[s->n++] = p;
    return true;
}

static void strv_fini(strv *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        free(s->v[i]);
    }
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* a + "/" + b (Path::join; an absolute b replaces a). */
static char *join(const char *a, const char *b)
{
    size_t la;
    size_t lb = strlen(b);
    char *r;
    if (b[0] == '/') {
        return xstrdup(b, lb);
    }
    la = strlen(a);
    r = malloc(la + lb + 2);
    if (r == NULL) {
        return NULL;
    }
    memcpy(r, a, la);
    if (la == 0 || a[la - 1] != '/') {
        r[la++] = '/';
    }
    memcpy(r + la, b, lb + 1);
    return r;
}

static const char *home(void)
{
    const char *h = getenv("HOME");
    return h != NULL ? h : NULL;
}

/* expand_tilde: "~" or "~/..." becomes $HOME (or "/") joined with the rest. */
static char *expand_tilde(const char *p)
{
    if (p[0] == '~' && (p[1] == '\0' || p[1] == '/')) {
        const char *h = home();
        const char *rest = p[1] == '/' ? p + 2 : p + 1;
        while (*rest == '/') {
            rest++;
        }
        return join(h != NULL ? h : "/", rest);
    }
    return xstrdup(p, strlen(p));
}

typedef enum { PFX_DEFAULT, PFX_CWD, PFX_XDG, PFX_RELATIVE } pfx;
typedef enum { BEH_CONFIG, BEH_CWD, BEH_XDG, BEH_RELATIVE } beh;

static bool parse_prefix(const sx_node *n, pfx *out)
{
    const char *v = sx_attr_get(n, NULL, "prefix");
    *out = PFX_DEFAULT;
    if (v == NULL) {
        return true;
    }
    if (strcmp(v, "default") == 0) {
        *out = PFX_DEFAULT;
    } else if (strcmp(v, "cwd") == 0) {
        *out = PFX_CWD;
    } else if (strcmp(v, "xdg") == 0) {
        *out = PFX_XDG;
    } else if (strcmp(v, "relative") == 0) {
        *out = PFX_RELATIVE;
    } else {
        return false;
    }
    return true;
}

/* define_calculate_path!: the path of a <dir>/<include> given the file it appears in. */
static char *calculate_path(const char *text, pfx p, beh def, const char *xdg_env, const char *xdg_fallback,
                            const char *config_path)
{
    char *e = expand_tilde(text);
    char *r;
    beh b;
    if (e == NULL || e[0] == '/') {
        return e;
    }
    b = p == PFX_DEFAULT ? def : p == PFX_CWD ? BEH_CWD : p == PFX_XDG ? BEH_XDG : BEH_RELATIVE;
    switch (b) {
    case BEH_CONFIG:
        r = join("/etc/fonts", e);
        break;
    case BEH_CWD:
        r = join(".", e);
        break;
    case BEH_RELATIVE: {
        const char *slash = strrchr(config_path, '/');
        if (slash != NULL) {
            char *parent =
                xstrdup(config_path, (size_t)(slash - config_path) + (slash == config_path ? 1 : 0));
            r = parent != NULL ? join(parent, e) : NULL;
            free(parent);
        } else {
            r = join(".", e);
        }
        break;
    }
    case BEH_XDG:
    default: {
        const char *x = getenv(xdg_env);
        char *base = expand_tilde(x != NULL ? x : xdg_fallback);
        r = base != NULL ? join(base, e) : NULL;
        free(base);
        break;
    }
    }
    free(e);
    return r;
}

static const char *node_text(const sx_node *n)
{
    return sx_text(n);
}

static bool is_el(const sx_node *n, const char *name)
{
    return n != NULL && !n->is_text && strcmp(n->name, name) == 0;
}

static bool merge(fcs *s, const char *path, unsigned depth);

/* FontConfig::include: a file, or every file (and symlink) of a directory in sorted order. */
static bool include(fcs *s, const char *path, unsigned depth)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        return merge(s, path, depth);
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        strv names = {NULL, 0, 0};
        struct dirent *e;
        size_t i;
        if (d == NULL) {
            return false;
        }
        while ((e = readdir(d)) != NULL && names.n < MAX_FILES) {
            struct stat ls;
            char *full;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            full = join(path, e->d_name);
            if (full == NULL) {
                s->nomem = true;
                break;
            }
            if (lstat(full, &ls) != 0 || !(S_ISREG(ls.st_mode) || S_ISLNK(ls.st_mode))) {
                free(full);
                continue;
            }
            if (!strv_push(&names, full)) {
                s->nomem = true;
                break;
            }
        }
        (void)closedir(d);
        /* PathBuf ordering within one directory is the order of the names' bytes. */
        for (i = 1; i < names.n; i++) {
            size_t j = i;
            while (j > 0 && strcmp(names.v[j - 1], names.v[j]) > 0) {
                char *t = names.v[j - 1];
                names.v[j - 1] = names.v[j];
                names.v[j] = t;
                j--;
            }
        }
        for (i = 0; i < names.n && !s->nomem; i++) {
            (void)merge(s, names.v[i], depth);
        }
        strv_fini(&names);
    }
    return true;
}

static void set_alias(svf_fc *fc, const char *alias, const char *name)
{
    char low[32];
    size_t i;
    char **slot = NULL;
    size_t n = strlen(alias);
    if (n >= sizeof low) {
        return;
    }
    for (i = 0; i <= n; i++) {
        char ch = alias[i];
        low[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
    }
    if (strcmp(low, "serif") == 0) {
        slot = &fc->serif;
    } else if (strcmp(low, "sans-serif") == 0 || strcmp(low, "sans serif") == 0) {
        slot = &fc->sans;
    } else if (strcmp(low, "monospace") == 0) {
        slot = &fc->mono;
    } else if (strcmp(low, "cursive") == 0) {
        slot = &fc->cursive;
    } else if (strcmp(low, "fantasy") == 0) {
        slot = &fc->fantasy;
    }
    if (slot != NULL) {
        char *d = xstrdup(name, strlen(name));
        if (d != NULL) {
            free(*slot);
            *slot = d;
        }
    }
}

/* One <alias>: its family and the first of prefer, accept, default. False on a parse error. */
static bool alias_part(svf_fc *fc, const sx_node *el)
{
    const sx_node *c;
    const char *alias = NULL;
    const char *prefer = NULL;
    const char *accept = NULL;
    const char *dflt = NULL;
    for (c = el->first; c != NULL; c = c->next) {
        const char **slot = NULL;
        const sx_node *f;
        if (c->is_text) {
            continue;
        }
        if (strcmp(c->name, "family") == 0) {
            const char *t = node_text(c);
            if (t == NULL) {
                return false;
            }
            alias = t;
            continue;
        }
        if (strcmp(c->name, "prefer") == 0) {
            slot = &prefer;
        } else if (strcmp(c->name, "accept") == 0) {
            slot = &accept;
        } else if (strcmp(c->name, "default") == 0) {
            slot = &dflt;
        } else {
            continue;
        }
        for (f = c->first; f != NULL; f = f->next) {
            if (is_el(f, "family") && node_text(f) != NULL && *slot == NULL) {
                *slot = node_text(f);
            }
        }
    }
    {
        const char *name = prefer != NULL ? prefer : accept != NULL ? accept : dflt;
        if (name != NULL) {
            set_alias(fc, alias != NULL ? alias : "", name);
        }
    }
    return true;
}

/* <match>: every <test>/<edit> needs an element value. */
static bool match_part(const sx_node *el)
{
    const sx_node *c;
    for (c = el->first; c != NULL; c = c->next) {
        if ((is_el(c, "test") || is_el(c, "edit")) && sx_first_element(c) == NULL) {
            return false;
        }
    }
    return true;
}

/* parse_config_part for one child of <fontconfig>; false ends the file. */
static bool part(fcs *s, const sx_node *el, const char *config_path, unsigned depth)
{
    svf_fc *fc = s->fc;
    const char *name = el->name;
    pfx p;
    if (strcmp(name, "description") == 0) {
        return node_text(el) != NULL;
    }
    if (strcmp(name, "alias") == 0) {
        return alias_part(fc, el);
    }
    if (strcmp(name, "dir") == 0) {
        const char *t;
        char *path;
        if (!parse_prefix(el, &p) || (t = node_text(el)) == NULL) {
            return false;
        }
        path = calculate_path(t, p, BEH_CWD, "XDG_DATA_HOME", "~/.local/share", config_path);
        if (path == NULL) {
            s->nomem = true;
            return false;
        }
        if (fc->ndirs >= MAX_DIRS) {
            free(path);
            return true;
        }
        {
            strv d = {fc->dirs, fc->ndirs, fc->ndirs};
            if (!strv_push(&d, path)) {
                s->nomem = true;
                return false;
            }
            fc->dirs = d.v;
            fc->ndirs = d.n;
        }
        return true;
    }
    if (strcmp(name, "reset-dirs") == 0) {
        size_t i;
        for (i = 0; i < fc->ndirs; i++) {
            free(fc->dirs[i]);
        }
        fc->ndirs = 0;
        return true;
    }
    if (strcmp(name, "remap-dir") == 0 || strcmp(name, "cachedir") == 0) {
        return parse_prefix(el, &p) && node_text(el) != NULL;
    }
    if (strcmp(name, "include") == 0) {
        const char *t;
        char *path;
        if (!parse_prefix(el, &p) || (t = node_text(el)) == NULL) {
            return false;
        }
        path = calculate_path(t, p, BEH_CONFIG, "XDG_CONFIG_HOME", "~/.config", config_path);
        if (path == NULL) {
            s->nomem = true;
            return false;
        }
        if (depth < MAX_DEPTH) {
            (void)include(s, path, depth + 1);
        }
        free(path);
        return true;
    }
    if (strcmp(name, "match") == 0) {
        return match_part(el);
    }
    return true;
}

/* FontConfig::merge_config. False when the file is missing, unreadable, not a fontconfig file or
 * has a part that fails to parse (fontdb then also reads the global configuration). */
static bool merge(fcs *s, const char *path, unsigned depth)
{
    char canon[PATH_MAX];
    pc_buf b;
    sv_arena a;
    sx_doc doc;
    char msg[64];
    const sx_node *root;
    const sx_node *c;
    size_t i;
    bool ok = true;
    if (s->nomem || realpath(path, canon) == NULL) {
        return false;
    }
    for (i = 0; i < s->seen.n; i++) {
        if (strcmp(s->seen.v[i], canon) == 0) {
            return true;
        }
    }
    if (s->seen.n >= MAX_FILES || !strv_push(&s->seen, xstrdup(canon, strlen(canon)))) {
        return false;
    }
    pc_buf_init(&b);
    if (pc_file_read(path, MAX_CONF_BYTES, &b, NULL) != PC_OK) {
        return false;
    }
    sv_arena_init(&a);
    if (sx_parse(&a, b.len > 0 ? (const char *)b.data : "", b.len, &doc, msg, sizeof msg) != PC_OK) {
        sv_arena_fini(&a);
        pc_buf_fini(&b);
        return false;
    }
    root = NULL;
    for (c = doc.root != NULL ? doc.root->first : NULL; c != NULL; c = c->next) {
        if (!c->is_text) {
            root = c;
            break;
        }
    }
    if (root == NULL || strcmp(root->name, "fontconfig") != 0) {
        sv_arena_fini(&a);
        pc_buf_fini(&b);
        return false;
    }
    for (c = root->first; c != NULL && ok && !s->nomem; c = c->next) {
        if (!c->is_text) {
            ok = part(s, c, path, depth);
        }
    }
    sv_arena_fini(&a);
    pc_buf_fini(&b);
    return ok;
}

bool svf_fc_load(svf_fc *out)
{
    fcs s;
    const char *file = getenv("FONTCONFIG_FILE");
    memset(out, 0, sizeof *out);
    memset(&s, 0, sizeof s);
    s.fc = out;
    if (file != NULL) {
        (void)merge(&s, file, 0);
    } else {
        const char *xch = getenv("XDG_CONFIG_HOME");
        char *xdg = NULL;
        bool read_global = true;
        if (xch != NULL) {
            xdg = xstrdup(xch, strlen(xch));
        } else if (home() != NULL) {
            xdg = join(home(), ".config");
        }
        if (xdg != NULL) {
            char *p = join(xdg, "fontconfig/fonts.conf");
            read_global = p == NULL || !merge(&s, p, 0);
            free(p);
            free(xdg);
        }
        if (read_global) {
            (void)merge(&s, "/etc/fonts/local.conf", 0);
        }
        (void)merge(&s, "/etc/fonts/fonts.conf", 0);
    }
    strv_fini(&s.seen);
    return out->ndirs > 0;
}

void svf_fc_fini(svf_fc *fc)
{
    size_t i;
    for (i = 0; i < fc->ndirs; i++) {
        free(fc->dirs[i]);
    }
    free(fc->dirs);
    free(fc->serif);
    free(fc->sans);
    free(fc->cursive);
    free(fc->fantasy);
    free(fc->mono);
    memset(fc, 0, sizeof *fc);
}
