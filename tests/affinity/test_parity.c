/* Parity with the Rust reference on the public Affinity corpus: tests/golden/affinity-vectors.txt
 * was written by `rustref affinity-vectors corpus/affinity`. Every document's imported model is
 * hashed field by field, with its preview and seeded truncation / mutation sweeps. Skips when
 * corpus/affinity is absent (`make corpus`). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "affinity/affinity.h"
#include "base/buf.h"
#include "base/file.h"
#include "harness.h"

#define H0 0xcbf29ce484222325ull

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

static void u(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}

static void f(uint64_t *h, double v)
{
    uint64_t x;
    memcpy(&x, &v, 8);
    u(h, x);
}

static void s(uint64_t *h, const char *v)
{
    fnv(h, v, strlen(v));
    fnv(h, "", 1);
}

static void b(uint64_t *h, bool v)
{
    uint8_t x = v ? 1 : 0;
    fnv(h, &x, 1);
}

static void byte(uint64_t *h, unsigned v)
{
    uint8_t x = (uint8_t)v;
    fnv(h, &x, 1);
}

static void color(uint64_t *h, const pc_af_color *c)
{
    int k;
    byte(h, c->kind);
    switch (c->kind) {
    case PC_AF_RGB:
    case PC_AF_LAB:
        for (k = 0; k < 3; k++) {
            f(h, c->v[k]);
        }
        break;
    case PC_AF_CMYK:
        for (k = 0; k < 4; k++) {
            f(h, c->v[k]);
        }
        break;
    default:
        f(h, c->v[0]);
    }
    f(h, c->alpha);
}

static void affine(uint64_t *h, const pc_af_affine *m)
{
    int k;
    for (k = 0; k < 6; k++) {
        f(h, m->m[k]);
    }
}

static void paint(uint64_t *h, const pc_af_paint *p)
{
    size_t i;
    if (p->kind == PC_AF_PAINT_NONE) {
        byte(h, 0);
    } else if (p->kind == PC_AF_SOLID) {
        byte(h, 1);
        color(h, &p->color);
    } else {
        byte(h, 2);
        byte(h, p->gkind);
        u(h, p->nstops);
        for (i = 0; i < p->nstops; i++) {
            f(h, p->stops[i].offset);
            color(h, &p->stops[i].color);
            f(h, p->stops[i].bias);
        }
        affine(h, &p->transform);
    }
}

static void path(uint64_t *h, const pc_af_path *p)
{
    size_t i;
    u(h, p->nsubpaths);
    for (i = 0; i < p->nsubpaths; i++) {
        const pc_af_subpath *sp = &p->subpaths[i];
        size_t k;
        f(h, sp->start.x);
        f(h, sp->start.y);
        u(h, sp->nsegments);
        for (k = 0; k < sp->nsegments; k++) {
            int j;
            for (j = 0; j < 3; j++) {
                f(h, sp->segments[k][j].x);
                f(h, sp->segments[k][j].y);
            }
        }
        b(h, sp->closed);
    }
}

static void stroke(uint64_t *h, const pc_af_stroke *k)
{
    size_t i;
    paint(h, &k->paint);
    f(h, k->width);
    byte(h, k->cap);
    byte(h, k->join);
    f(h, k->miter_limit);
    byte(h, k->align);
    if (k->has_dash) {
        byte(h, 1);
        u(h, k->ndash);
        for (i = 0; i < k->ndash; i++) {
            f(h, k->dash[i]);
        }
        f(h, k->dash_phase);
    } else {
        byte(h, 0);
    }
    b(h, k->behind);
}

static void image(uint64_t *h, const pc_af_image *i)
{
    u(h, i->width);
    u(h, i->height);
    byte(h, i->format == PC_AF_RGBA8 ? 0 : 1);
    u(h, i->npixels);
    fnv(h, i->pixels, i->npixels);
    affine(h, &i->transform);
}

static void node(uint64_t *h, const pc_af_node *n)
{
    size_t i;
    u(h, n->cls);
    s(h, n->name);
    b(h, n->visible);
    b(h, n->locked);
    f(h, n->opacity);
    byte(h, n->blend);
    byte(h, n->kind);
    switch (n->kind) {
    case PC_AF_ARTBOARD:
        f(h, n->rect.x0);
        f(h, n->rect.y0);
        f(h, n->rect.x1);
        f(h, n->rect.y1);
        u(h, n->nbackground);
        for (i = 0; i < n->nbackground; i++) {
            paint(h, &n->background[i]);
        }
        break;
    case PC_AF_SHAPE:
        path(h, &n->path);
        u(h, n->nfills);
        for (i = 0; i < n->nfills; i++) {
            paint(h, &n->fills[i]);
        }
        u(h, n->nstrokes);
        for (i = 0; i < n->nstrokes; i++) {
            stroke(h, &n->strokes[i]);
        }
        b(h, n->even_odd);
        break;
    case PC_AF_TEXT: {
        const pc_af_text *t = &n->text;
        u(h, t->nruns);
        for (i = 0; i < t->nruns; i++) {
            const pc_af_text_run *r = &t->runs[i];
            s(h, r->text);
            s(h, r->postscript);
            s(h, r->family);
            u(h, (uint64_t)r->weight);
            b(h, r->italic);
            f(h, r->size);
            f(h, r->tracking);
            if (r->has_leading) {
                byte(h, 1);
                f(h, r->leading);
            } else {
                byte(h, 0);
            }
            paint(h, &r->fill);
        }
        byte(h, t->align);
        f(h, t->anchor.x);
        f(h, t->anchor.y);
        if (t->has_frame) {
            byte(h, 1);
            f(h, t->frame.x0);
            f(h, t->frame.y0);
            f(h, t->frame.x1);
            f(h, t->frame.y1);
        } else {
            byte(h, 0);
        }
        affine(h, &t->transform);
        break;
    }
    case PC_AF_IMAGE:
        image(h, &n->image);
        break;
    default:
        break;
    }
    if (n->has_mask) {
        byte(h, 1);
        path(h, &n->mask);
    } else {
        byte(h, 0);
    }
    if (n->has_pixel_mask) {
        byte(h, 1);
        image(h, &n->pixel_mask);
    } else {
        byte(h, 0);
    }
    u(h, n->nchildren);
    for (i = 0; i < n->nchildren; i++) {
        node(h, &n->children[i]);
    }
}

static uint64_t doc_hash(const pc_af_document *d)
{
    uint64_t h = H0;
    size_t i;
    f(&h, d->dpi);
    u(&h, d->nspreads);
    for (i = 0; i < d->nspreads; i++) {
        const pc_af_spread *sp = &d->spreads[i];
        size_t k;
        f(&h, sp->bounds.x0);
        f(&h, sp->bounds.y0);
        f(&h, sp->bounds.x1);
        f(&h, sp->bounds.y1);
        u(&h, sp->npages);
        for (k = 0; k < sp->npages; k++) {
            f(&h, sp->pages[k].x0);
            f(&h, sp->pages[k].y0);
            f(&h, sp->pages[k].x1);
            f(&h, sp->pages[k].y1);
        }
        b(&h, sp->transparent);
        u(&h, sp->nnodes);
        for (k = 0; k < sp->nnodes; k++) {
            node(&h, &sp->nodes[k]);
        }
    }
    s(&h, d->saved_by != NULL ? d->saved_by : "-");
    for (i = 0; i < d->nwarnings; i++) {
        s(&h, d->warnings[i]);
    }
    return h;
}

static size_t count(const pc_af_node *n, size_t k)
{
    size_t i;
    size_t c = 0;
    for (i = 0; i < k; i++) {
        c += 1 + count(n[i].children, n[i].nchildren);
    }
    return c;
}

/* How early a damaged compressed entry is noticed differs between the reference's decompressors
 * (miniz_oxide, ruzstd) and zlib / libzstd, so its three messages are one outcome here (§7). */
static const char *entry_error(const char *m)
{
    if (strcmp(m, "damaged Affinity file: corrupt compressed entry") == 0 ||
        strcmp(m, "damaged Affinity file: archive entry size mismatch") == 0 ||
        strcmp(m, "damaged Affinity file: archive entry checksum mismatch") == 0) {
        return "damaged entry";
    }
    return m;
}

static void result(uint64_t *h, const uint8_t *bytes, size_t n)
{
    pc_af_limits l;
    pc_af_document d;
    pc_af_preview p;
    pc_error e;
    l.max_entry = (size_t)64 << 20;
    l.max_total = (size_t)256 << 20;
    if (pc_af_read(bytes, n, l, &d, &e) == PC_OK) {
        u(h, doc_hash(&d));
        pc_af_document_fini(&d);
    } else {
        s(h, entry_error(e.msg));
    }
    if (pc_af_preview_read(bytes, n, &p, &e) == PC_OK) {
        u(h, p.width);
        u(h, p.height);
        u(h, p.len);
    } else {
        s(h, e.msg);
    }
}

/* ---- corpus walk (sorted, like the reference's path sort) ---- */

static char **paths;
static size_t npaths;

static int cmp_str(const void *a, const void *b2)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b2);
}

static bool is_doc(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && (strcmp(dot, ".af") == 0 || strcmp(dot, ".afdesign") == 0 || strcmp(dot, ".afphoto") == 0 ||
                           strcmp(dot, ".afpub") == 0 || strcmp(dot, ".aftemplate") == 0);
}

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **names = NULL;
    size_t n = 0;
    size_t i;
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char *full;
        char **g;
        size_t len;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        len = strlen(dir) + strlen(e->d_name) + 2;
        full = malloc(len);
        g = realloc(names, (n + 1) * sizeof *names);
        if (full == NULL || g == NULL) {
            free(full);
            if (g != NULL) {
                names = g;
            }
            break;
        }
        names = g;
        (void)snprintf(full, len, "%s/%s", dir, e->d_name);
        names[n++] = full;
    }
    (void)closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (i = 0; i < n; i++) {
        struct stat st;
        if (stat(names[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            walk(names[i]);
            free(names[i]);
        } else if (is_doc(names[i])) {
            char **g = realloc(paths, (npaths + 1) * sizeof *paths);
            if (g == NULL) {
                free(names[i]);
                continue;
            }
            paths = g;
            paths[npaths++] = names[i];
        } else {
            free(names[i]);
        }
    }
    free(names);
}

static char **gold;
static size_t ngold;

static void load_golden(void)
{
    FILE *fp = fopen("tests/golden/affinity-vectors.txt", "r");
    char line[1024];
    if (fp == NULL) {
        PC_T_FAIL("cannot open tests/golden/affinity-vectors.txt");
        return;
    }
    while (fgets(line, sizeof line, fp) != NULL) {
        size_t n = strlen(line);
        char **g;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        g = realloc(gold, (ngold + 1) * sizeof *gold);
        if (g == NULL) {
            break;
        }
        gold = g;
        gold[ngold] = malloc(n + 1);
        if (gold[ngold] == NULL) {
            break;
        }
        memcpy(gold[ngold], line, n + 1);
        ngold++;
    }
    (void)fclose(fp);
}

static size_t gi;

static void expect(const char *got)
{
    CHECK(gi < ngold && strcmp(gold[gi], got) == 0);
    if (gi >= ngold) {
        PC_T_FAIL("golden ran out at: %s", got);
        return;
    }
    if (strcmp(gold[gi], got) != 0) {
        PC_T_FAIL("line %zu\n  want %s\n  got  %s", gi + 1, gold[gi], got);
    }
    gi++;
}

static void test_corpus_matches_the_reference(void)
{
    const char *root = "corpus/affinity";
    size_t i;
    struct stat st;
    if (stat("corpus/affinity/SOURCES.md", &st) != 0) {
        (void)fprintf(stderr, "(corpus/affinity absent: run `make corpus`; skipped) ");
        return;
    }
    load_golden();
    walk(root);
    CHECK(npaths > 0);
    for (i = 0; i < npaths; i++) {
        const char *name = paths[i] + strlen(root) + 1;
        uint8_t *bytes = NULL;
        size_t n = 0;
        char line[1024];
        pc_af_document d;
        pc_af_preview p;
        pc_error e;
        uint64_t h;
        uint64_t stt;
        size_t k;
        int it;
        {
            pc_buf fb = PC_BUF_INIT;
            if (pc_file_read(paths[i], SIZE_MAX, &fb, &e) != PC_OK) {
                PC_T_FAIL("cannot read %s: %s", paths[i], e.msg);
                continue;
            }
            n = fb.len;
            bytes = pc_buf_take(&fb, NULL);
        }
        if (pc_af_read(bytes, n, pc_af_limits_default(), &d, &e) == PC_OK) {
            size_t nodes = 0;
            for (k = 0; k < d.nspreads; k++) {
                nodes += count(d.spreads[k].nodes, d.spreads[k].nnodes);
            }
            (void)snprintf(line, sizeof line, "doc %s %016llx spreads %zu nodes %zu warnings %zu", name,
                           (unsigned long long)doc_hash(&d), d.nspreads, nodes, d.nwarnings);
            pc_af_document_fini(&d);
        } else {
            (void)snprintf(line, sizeof line, "doc %s err %s", name, e.msg);
        }
        expect(line);
        if (pc_af_preview_read(bytes, n, &p, &e) == PC_OK) {
            (void)snprintf(line, sizeof line, "preview %s %ux%u %zu", name, (unsigned)p.width, (unsigned)p.height,
                           p.len);
        } else {
            (void)snprintf(line, sizeof line, "preview %s err %s", name, e.msg);
        }
        expect(line);
        h = H0;
        for (k = 0; k < 64; k++) {
            size_t cut = (size_t)((uint64_t)n * k / 64);
            uint8_t *c = malloc(cut > 0 ? cut : 1);
            if (c == NULL) {
                break;
            }
            memcpy(c, bytes, cut);
            result(&h, c, cut);
            free(c);
        }
        (void)snprintf(line, sizeof line, "truncate %s %016llx", name, (unsigned long long)h);
        expect(line);
        h = H0;
        stt = 0x5EEDull ^ (uint64_t)n;
#define NEXT() (stt = stt * 6364136223846793005ull + 1442695040888963407ull, stt >> 33)
        for (it = 0; it < 40; it++) {
            uint8_t *c = malloc(n);
            uint64_t m;
            uint64_t j;
            if (c == NULL) {
                break;
            }
            memcpy(c, bytes, n);
            m = 1 + NEXT() % 6;
            for (j = 0; j < m; j++) {
                size_t at = (size_t)(NEXT() % n);
                c[at] = (uint8_t)NEXT();
            }
            result(&h, c, n);
            free(c);
        }
#undef NEXT
        (void)snprintf(line, sizeof line, "mutate %s %016llx", name, (unsigned long long)h);
        expect(line);
        free(bytes);
    }
    CHECK_EQ_UINT(gi, ngold);
    for (i = 0; i < npaths; i++) {
        free(paths[i]);
    }
    free(paths);
    for (i = 0; i < ngold; i++) {
        free(gold[i]);
    }
    free(gold);
}

int main(int argc, char **argv)
{
    RUN(test_corpus_matches_the_reference);
    return pc_t_finish();
}
