/* Text layout, part 1 (port of usvg 0.45.1 text/layout.rs and text/mod.rs): the fonts a text
 * uses (FontResolver's default selectors over the fontdb port), shaping with fallback fonts
 * (shape_text) and process_chunk, which shapes a chunk per span font, merges the results
 * cluster by cluster as the reference does and forms the glyph clusters. Lookups the reference
 * does linearly are indexed; the merge charges the per-import work budget (svg_text.h). */
#include <stdlib.h>
#include <string.h>

#include "io/svg_text.h"

/* ---------------------------------------------------------------- fonts cache */

void svt_fonts_fini(svt_fonts *f)
{
    size_t i;
    for (i = 0; i < f->n; i++) {
        if (f->vals[i] != NULL) {
            svt_rfont_fini(f->vals[i]);
            free(f->vals[i]);
        }
    }
    free(f->keys);
    free(f->vals);
    memset(f, 0, sizeof *f);
}

const svt_rfont *svt_fonts_get(const svt_fonts *f, const svf_font *key)
{
    size_t i;
    for (i = 0; i < f->n; i++) {
        if (f->keys[i] != NULL && svf_font_eq(f->keys[i], key)) {
            return f->vals[i];
        }
    }
    return NULL;
}

bool svt_fonts_has(const svt_fonts *f, const svf_font *key)
{
    size_t i;
    for (i = 0; i < f->n; i++) {
        if (f->keys[i] != NULL && svf_font_eq(f->keys[i], key)) {
            return true;
        }
    }
    return false;
}

/* Adds an entry (key NULL: a fallback font kept alive for its glyphs). */
static bool fonts_add(svt_fonts *f, const svf_font *key, svt_rfont *val)
{
    if (f->n == f->cap) {
        size_t nc = f->cap < 8 ? 8 : f->cap * 2;
        const svf_font **k = realloc(f->keys, nc * sizeof *k);
        svt_rfont **v;
        if (k == NULL) {
            return false;
        }
        f->keys = k;
        v = realloc(f->vals, nc * sizeof *v);
        if (v == NULL) {
            return false;
        }
        f->vals = v;
        f->cap = nc;
    }
    f->keys[f->n] = key;
    f->vals[f->n] = val;
    f->n++;
    return true;
}

static svt_rfont *load(const svf_db *db, size_t id)
{
    svt_rfont *r = malloc(sizeof *r);
    if (r == NULL) {
        return NULL;
    }
    if (!svt_rfont_load(db, id, r)) {
        free(r);
        return NULL;
    }
    return r;
}

/* FontResolver::default_font_selector + load_font. */
bool svt_select_font(sv_arena *a, svt_fonts *f, const svf_font *font)
{
    svf_family *fams;
    size_t id;
    svt_rfont *r = NULL;
    fams = sv_alloc_array(a, font->nfams + 1, sizeof *fams);
    if (fams == NULL) {
        return false;
    }
    memcpy(fams, font->fams, font->nfams * sizeof *fams);
    fams[font->nfams].kind = SVF_SERIF;
    fams[font->nfams].name = NULL;
    id = svf_db_query(f->db, fams, font->nfams + 1, font->weight, font->stretch, font->style);
    if (id != SIZE_MAX) {
        r = load(f->db, id);
    }
    if (!fonts_add(f, font, r)) {
        if (r != NULL) {
            svt_rfont_fini(r);
            free(r);
        }
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- shaping with fallback */

bool svt_charge(svt_lctx *l, size_t units)
{
    if (units > SVT_MAX_WORK - l->bud->work) {
        l->bud->work = SVT_MAX_WORK;
        l->limit = true;
        return false;
    }
    l->bud->work += units;
    return true;
}

uint32_t svt_char_at(const char *s, size_t n, size_t i)
{
    const unsigned char *p = (const unsigned char *)s + i;
    if (i >= n) {
        return 0;
    }
    if (p[0] < 0x80 || n - i < 2) {
        return p[0];
    }
    if (p[0] < 0xE0 || n - i < 3) {
        return (uint32_t)(p[0] & 0x1F) << 6 | (p[1] & 0x3F);
    }
    if (p[0] < 0xF0 || n - i < 4) {
        return (uint32_t)(p[0] & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F);
    }
    return (uint32_t)(p[0] & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 |
           (p[3] & 0x3F);
}

/* default_fallback_selector. */
static size_t select_fallback(const svf_db *db, uint32_t c, const size_t *used, size_t nused)
{
    const svf_face *base = svf_db_face(db, used[0]);
    size_t n = svf_db_len(db);
    size_t i;
    if (base == NULL) {
        return SIZE_MAX;
    }
    for (i = 0; i < n; i++) {
        const svf_face *f = svf_db_face(db, i);
        size_t k;
        bool ex = false;
        for (k = 0; k < nused; k++) {
            ex = ex || used[k] == i;
        }
        if (ex) {
            continue;
        }
        if (base->style != f->style && base->weight != f->weight && base->stretch != f->stretch) {
            continue;
        }
        if (!svf_has_char(db, i, c)) {
            continue;
        }
        return i;
    }
    return SIZE_MAX;
}

/* shape_text: glyphs of the whole chunk text with one font, missing glyphs taken from fallbacks. */
static bool shape_text(svt_lctx *l, const char *text, size_t len, const svt_rfont *font, bool small_caps,
                       bool kerning, svt_glyph **out, size_t *nout)
{
    sv_arena *a = l->c->a;
    svt_glyph *g = NULL;
    size_t ng = 0;
    size_t *used;
    size_t nused = 1;
    size_t cap = 8;
    if (!svt_charge(l, len + 1)) {
        return false;
    }
    if (!svt_shape(a, text, len, font, small_caps, kerning, &g, &ng)) {
        if (a->nomem) {
            l->fail = true;
            return false;
        }
        ng = 0;
    }
    used = malloc(cap * sizeof *used);
    if (used == NULL) {
        l->fail = true;
        return false;
    }
    used[0] = font->id;
    for (;;) {
        size_t i;
        size_t miss = SIZE_MAX;
        size_t fid;
        svt_rfont *fb;
        svt_glyph *fg = NULL;
        size_t nfg = 0;
        bool all = true;
        for (i = 0; i < ng; i++) {
            if (g[i].id == 0) {
                miss = i;
                break;
            }
        }
        if (miss == SIZE_MAX) {
            break;
        }
        fid = select_fallback(l->fonts->db, svt_char_at(text, len, g[miss].byte_idx), used, nused);
        if (fid == SIZE_MAX) {
            break;
        }
        fb = load(l->fonts->db, fid);
        if (fb == NULL) {
            break;
        }
        if (!fonts_add(l->fonts, NULL, fb)) {
            svt_rfont_fini(fb);
            free(fb);
            l->fail = true;
            break;
        }
        if (!svt_charge(l, len + 1)) {
            break;
        }
        if (!svt_shape(a, text, len, fb, small_caps, kerning, &fg, &nfg)) {
            if (a->nomem) {
                l->fail = true;
                break;
            }
            nfg = 0;
        }
        for (i = 0; i < nfg; i++) {
            all = all && fg[i].id != 0;
        }
        if (all) {
            g = fg;
            ng = nfg;
            break;
        }
        if (ng != nfg) {
            break;
        }
        for (i = 0; i < ng; i++) {
            if (g[i].id == 0 && fg[i].id != 0) {
                g[i] = fg[i];
            }
        }
        if (nused == cap) {
            size_t *u = realloc(used, cap * 2 * sizeof *u);
            if (u == NULL) {
                l->fail = true;
                break;
            }
            used = u;
            cap *= 2;
        }
        used[nused++] = fid;
    }
    free(used);
    if (l->fail || l->limit) {
        return false;
    }
    *out = g;
    *nout = ng;
    return true;
}

/* ---------------------------------------------------------------- clusters */

sv_ts svt_cluster_ts(const svt_cluster *c)
{
    return ts_post_concat(c->path_transform, c->transform);
}

/* chunk_span_at: spans are contiguous and in order, so a binary search. */
const svt_span *svt_span_at(const svt_chunk *ch, size_t b, size_t *idx)
{
    size_t lo = 0;
    size_t hi = ch->nspans;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ch->spans[mid].start <= b) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return NULL;
    }
    lo--;
    if (b >= ch->spans[lo].start && b < ch->spans[lo].end) {
        if (idx != NULL) {
            *idx = lo;
        }
        return &ch->spans[lo];
    }
    return NULL;
}

bool svt_span_contains(const svt_span *s, size_t b)
{
    return b >= s->start && b < s->end;
}

/* The shaping result of one (font, small caps, kerning) combination, reused across spans. */
typedef struct shaped {
    const svt_rfont *font;
    bool small_caps, kerning;
    svt_glyph *g;
    size_t n;
    size_t *order; /* glyph indices sorted by byte index (stable) */
} shaped;

static int cmp_idx_byte(const void *x, const void *y, const svt_glyph *g)
{
    size_t a = *(const size_t *)x;
    size_t b = *(const size_t *)y;
    if (g[a].byte_idx != g[b].byte_idx) {
        return g[a].byte_idx < g[b].byte_idx ? -1 : 1;
    }
    return a < b ? -1 : a > b ? 1 : 0;
}

/* Merge sort of glyph indices by byte (no qsort context in C17). */
static void sort_idx(size_t *v, size_t *tmp, size_t n, const svt_glyph *g)
{
    size_t w;
    for (w = 1; w < n; w *= 2) {
        size_t i;
        for (i = 0; i < n; i += 2 * w) {
            size_t m = i + w < n ? i + w : n;
            size_t e = i + 2 * w < n ? i + 2 * w : n;
            size_t p = i, q = m, k = i;
            while (p < m && q < e) {
                tmp[k++] = cmp_idx_byte(&v[q], &v[p], g) < 0 ? v[q++] : v[p++];
            }
            while (p < m) {
                tmp[k++] = v[p++];
            }
            while (q < e) {
                tmp[k++] = v[q++];
            }
        }
        memcpy(v, tmp, n * sizeof *v);
    }
}

/* Glyph vector of the chunk with an index of the first glyph per byte. */
typedef struct gvec {
    svt_glyph *v;
    size_t n, cap;
    size_t *first; /* per byte of the text: index of the first glyph with that byte, or SIZE_MAX */
    bool dirty;
} gvec;

static void gvec_index(gvec *gv, size_t len)
{
    size_t i;
    for (i = 0; i <= len; i++) {
        gv->first[i] = SIZE_MAX;
    }
    for (i = gv->n; i-- > 0;) {
        if (gv->v[i].byte_idx <= len) {
            gv->first[gv->v[i].byte_idx] = i;
        }
    }
    gv->dirty = false;
}

/* process_chunk: shaping, the per-span cluster merge, then clusters. */
bool svt_process_chunk(svt_lctx *l, const svt_chunk *ch, svt_cluster **out, size_t *nout)
{
    sv_arena *a = l->c->a;
    shaped *cache = NULL;
    size_t ncache = 0;
    gvec gv = {NULL, 0, 0, NULL, false};
    bool have = false;
    size_t s;
    svt_cluster *cl = NULL;
    size_t ncl = 0;
    size_t i;
    *out = NULL;
    *nout = 0;
    cache = sv_alloc_array(a, ch->nspans + 1, sizeof *cache);
    gv.first = sv_alloc_array(a, ch->len + 1, sizeof *gv.first);
    if (cache == NULL || gv.first == NULL) {
        l->fail = true;
        return false;
    }
    for (s = 0; s < ch->nspans; s++) {
        const svt_span *sp = &ch->spans[s];
        const svt_rfont *font = svt_fonts_get(l->fonts, sp->font);
        shaped *sh = NULL;
        size_t k;
        if (font == NULL) {
            continue;
        }
        for (k = 0; k < ncache; k++) {
            if (cache[k].font == font && cache[k].small_caps == sp->small_caps &&
                cache[k].kerning == sp->kerning) {
                sh = &cache[k];
                break;
            }
        }
        if (sh == NULL) {
            sh = &cache[ncache];
            memset(sh, 0, sizeof *sh);
            sh->font = font;
            sh->small_caps = sp->small_caps;
            sh->kerning = sp->kerning;
            if (!shape_text(l, ch->text, ch->len, font, sp->small_caps, sp->kerning, &sh->g, &sh->n)) {
                return false;
            }
            ncache++;
        }
        if (!have) {
            /* the first run: taken as is */
            void *v = NULL;
            gv.cap = 0;
            if (sh->n > 0 && !sv_grow(a, &v, &gv.cap, sh->n, sizeof(svt_glyph))) {
                l->fail = true;
                return false;
            }
            gv.v = v;
            if (sh->n > 0) {
                memcpy(gv.v, sh->g, sh->n * sizeof(svt_glyph));
            }
            gv.n = sh->n;
            gv.dirty = true;
            if (gv.n > 0) {
                have = true;
            }
            continue;
        }
        if (sh->order == NULL && sh->n > 0) {
            size_t *tmp;
            sh->order = sv_alloc_array(a, sh->n, sizeof *sh->order);
            tmp = sv_alloc_array(a, sh->n, sizeof *tmp);
            if (sh->order == NULL || tmp == NULL) {
                l->fail = true;
                return false;
            }
            for (k = 0; k < sh->n; k++) {
                sh->order[k] = k;
            }
            sort_idx(sh->order, tmp, sh->n, sh->g);
        }
        {
            /* the glyphs of the span in shaping order: those with byte_idx in [start, end) */
            size_t lo = 0;
            size_t hi = sh->n;
            size_t *pos;
            size_t np;
            size_t consumed_to = 0;
            size_t *tmp;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (sh->g[sh->order[mid]].byte_idx < sp->start) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            hi = lo;
            while (hi < sh->n && sh->g[sh->order[hi]].byte_idx < sp->end) {
                hi++;
            }
            np = hi - lo;
            if (np == 0) {
                continue;
            }
            if (!svt_charge(l, np)) {
                return false;
            }
            pos = sv_alloc_array(a, np, sizeof *pos);
            tmp = sv_alloc_array(a, np, sizeof *tmp);
            if (pos == NULL || tmp == NULL) {
                l->fail = true;
                return false;
            }
            for (k = 0; k < np; k++) {
                pos[k] = sh->order[lo + k];
            }
            /* back to shaping order */
            {
                size_t w;
                for (w = 1; w < np; w *= 2) {
                    size_t i0;
                    for (i0 = 0; i0 < np; i0 += 2 * w) {
                        size_t m = i0 + w < np ? i0 + w : np;
                        size_t e = i0 + 2 * w < np ? i0 + 2 * w : np;
                        size_t p = i0, q = m, kk = i0;
                        while (p < m && q < e) {
                            tmp[kk++] = pos[q] < pos[p] ? pos[q++] : pos[p++];
                        }
                        while (p < m) {
                            tmp[kk++] = pos[p++];
                        }
                        while (q < e) {
                            tmp[kk++] = pos[q++];
                        }
                    }
                    memcpy(pos, tmp, np * sizeof *pos);
                }
            }
            for (k = 0; k < np; k++) {
                size_t gi = pos[k];
                const svt_glyph *ng = &sh->g[gi];
                size_t idx;
                size_t prev_len;
                if (gi < consumed_to) {
                    continue;
                }
                if (gv.dirty) {
                    if (!svt_charge(l, gv.n + ch->len)) {
                        return false;
                    }
                    gvec_index(&gv, ch->len);
                }
                idx = ng->byte_idx <= ch->len ? gv.first[ng->byte_idx] : SIZE_MAX;
                if (idx == SIZE_MAX) {
                    continue;
                }
                prev_len = gv.v[idx].cluster_len;
                if (prev_len < ng->cluster_len) {
                    size_t r;
                    for (r = 1; r < ng->cluster_len; r++) {
                        if (idx + 1 < gv.n) {
                            memmove(&gv.v[idx + 1], &gv.v[idx + 2], (gv.n - idx - 2) * sizeof(svt_glyph));
                            gv.n--;
                            gv.dirty = true;
                        } else {
                            /* Vec::remove out of bounds panics in the reference; stop here */
                            break;
                        }
                    }
                } else if (prev_len > ng->cluster_len) {
                    size_t j;
                    for (j = 1; j < prev_len; j++) {
                        if (gi + j < sh->n) {
                            void *v = gv.v;
                            if (idx + j > gv.n) {
                                break;
                            }
                            if (!sv_grow(a, &v, &gv.cap, gv.n + 1, sizeof(svt_glyph))) {
                                l->fail = true;
                                return false;
                            }
                            gv.v = v;
                            memmove(&gv.v[idx + j + 1], &gv.v[idx + j], (gv.n - idx - j) * sizeof(svt_glyph));
                            gv.v[idx + j] = sh->g[gi + j];
                            gv.n++;
                            gv.dirty = true;
                            consumed_to = gi + j + 1;
                        }
                    }
                }
                gv.v[idx] = *ng;
                gv.dirty = true;
            }
        }
    }
    if (gv.n == 0) {
        return true;
    }
    if (gv.n > SVT_MAX_GLYPHS - l->bud->glyphs) {
        l->limit = true;
        return false;
    }
    l->bud->glyphs += gv.n;
    /* GlyphClusters + form_glyph_clusters */
    cl = sv_alloc_array(a, gv.n, sizeof *cl);
    if (cl == NULL) {
        l->fail = true;
        return false;
    }
    for (i = 0; i < gv.n;) {
        size_t start = i;
        size_t b = gv.v[i].byte_idx;
        const svt_span *sp;
        while (i < gv.n && gv.v[i].byte_idx == b) {
            i++;
        }
        sp = svt_span_at(ch, b, NULL);
        if (sp != NULL) {
            svt_cluster *c = &cl[ncl++];
            float width = 0.0f;
            float x = 0.0f;
            size_t k;
            const svt_rfont *f0 = gv.v[start].font;
            memset(c, 0, sizeof *c);
            c->glyphs = sv_alloc_array(a, i - start, sizeof *c->glyphs);
            if (c->glyphs == NULL) {
                l->fail = true;
                return false;
            }
            for (k = start; k < i; k++) {
                const svt_glyph *g = &gv.v[k];
                float sx = svt_scale(g->font, sp->font_size);
                svt_pglyph *pg = &c->glyphs[c->nglyphs++];
                float gw;
                pg->glyph_ts = ts_translate(x + (float)g->dx, -(float)g->dy);
                pg->cluster_ts = ts_identity();
                pg->span_ts = ts_identity();
                pg->upem = g->font->upem;
                pg->font_size = sp->font_size;
                pg->id = g->id;
                pg->font = g->font;
                x += (float)g->width;
                gw = (float)g->width * sx;
                if (gw > width) {
                    width = gw;
                }
            }
            c->byte_idx = b;
            c->cp = svt_char_at(ch->text, ch->len, b);
            c->width = width;
            c->advance = width;
            c->ascent = (float)f0->ascent * svt_scale(f0, sp->font_size);
            c->descent = (float)f0->descent * svt_scale(f0, sp->font_size);
            c->transform = ts_identity();
            c->path_transform = ts_identity();
            c->visible = true;
        }
    }
    *out = cl;
    *nout = ncl;
    return true;
}
