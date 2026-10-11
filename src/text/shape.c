/* Building a paragraph layout: parley 0.11's ranged style builder, text analysis, itemisation,
 * fontique font selection and shaping, ported over HarfBuzz / FriBidi / libunibreak (see
 * playout.h). Function names follow the parley functions they port. */
#include "text/playout.h"

#include <graphemebreak.h>
#include <hb-ot.h>
#include <hb.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/utf8.h"
#include "text/fmetrics.h"
#include "text/uprops.h"

#define NOMEM(err) pc_fail(err, PC_ERR_NOMEM, "text layout: out of memory")

/* ------------------------------------------------------------------------------------------ */
/* Small helpers */

static bool nearly_eq(float x, float y)
{
    return fabsf(x - y) < 1.1920929e-7f; /* f32::EPSILON */
}

static bool nearly_zero(float x)
{
    return nearly_eq(x, 0.0f);
}

int pl_whitespace(uint32_t c)
{
    switch (c) {
    case ' ':
        return PL_WS_SPACE;
    case '\t':
        return PL_WS_TAB;
    case '\n':
    case '\r':
    case 0x2028:
    case 0x2029:
        return PL_WS_NEWLINE;
    case 0xA0:
        return PL_WS_NBSP;
    default:
        return PL_WS_NONE;
    }
}

static size_t utf8_len(uint32_t c)
{
    return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
}

/* Decodes one char of known-valid UTF-8 (anything else reads as U+FFFD, one byte). */
static uint32_t next_cp(const char *s, size_t len, size_t *i)
{
    uint32_t cp;
    if (!pc_utf8_next((const uint8_t *)s, len, i, &cp)) {
        (*i)++;
        return 0xFFFD;
    }
    return cp;
}

#define GROW(arr, n, cap, err)                                                                               \
    do {                                                                                                     \
        if ((n) >= (cap)) {                                                                                  \
            void *g_ = pc_grow_array((arr), &(cap), (n) + 1, sizeof *(arr));                                 \
            if (g_ == NULL) {                                                                                \
                return NOMEM(err);                                                                           \
            }                                                                                                \
            (arr) = g_;                                                                                      \
        }                                                                                                    \
    } while (0)

/* ------------------------------------------------------------------------------------------ */
/* Builder */

typedef struct fam_list {
    const pc_fontdb_family **v;
    size_t n;
} fam_list;

typedef struct feat_list {
    pl_feature *v;
    size_t n;
} feat_list;

typedef struct var_list {
    pl_variation *v;
    size_t n;
} var_list;

typedef struct face_entry {
    pc_font *font; /* one reference */
    hb_blob_t *blob;
    hb_face_t *face;
    bool has_cmap;
    pc_sfnt_cmap cmap; /* fontique's CharmapIndex */
} face_entry;

struct pl_builder {
    pc_fontdb *db;
    fam_list *fams;
    size_t nfams, cap_fams;
    feat_list *feats;
    size_t nfeats, cap_feats;
    var_list *vars;
    size_t nvars, cap_vars;
    pl_style root;
    pl_prop *props;
    size_t nprops, cap_props;
    face_entry *faces;
    size_t nfaces, cap_faces;
};

static void clear_lists(pl_builder *b)
{
    size_t i;
    for (i = 0; i < b->nfams; i++) {
        free(b->fams[i].v);
    }
    for (i = 0; i < b->nfeats; i++) {
        free(b->feats[i].v);
    }
    for (i = 0; i < b->nvars; i++) {
        free(b->vars[i].v);
    }
    b->nfams = b->nfeats = b->nvars = b->nprops = 0;
}

pc_status pl_builder_new(pl_builder **out, pc_error *err)
{
    pl_builder *b = calloc(1, sizeof *b);
    if (b == NULL) {
        return NOMEM(err);
    }
    *out = b;
    return PC_OK;
}

void pl_builder_free(pl_builder *b)
{
    size_t i;
    if (b == NULL) {
        return;
    }
    clear_lists(b);
    for (i = 0; i < b->nfaces; i++) {
        hb_face_destroy(b->faces[i].face);
        hb_blob_destroy(b->faces[i].blob);
        pc_font_release(b->faces[i].font);
    }
    free(b->fams);
    free(b->feats);
    free(b->vars);
    free(b->props);
    free(b->faces);
    free(b);
}

void pl_builder_begin(pl_builder *b, pc_fontdb *db)
{
    clear_lists(b);
    b->db = db;
    memset(&b->root, 0, sizeof b->root);
    /* TextStyle::default(): 16 px, weight 400, no lists. */
    b->root.size = 16.0f;
    b->root.weight = 400.0f;
    b->root.features = SIZE_MAX;
    b->root.variations = SIZE_MAX;
    b->root.families = SIZE_MAX;
}

pc_status pl_builder_families(pl_builder *b, const pc_fontdb_family *const *fams, size_t n, size_t *id,
                              pc_error *err)
{
    size_t i;
    const pc_fontdb_family **v;
    for (i = 0; i < b->nfams; i++) {
        if (b->fams[i].n == n && (n == 0 || memcmp(b->fams[i].v, fams, n * sizeof *fams) == 0)) {
            *id = i;
            return PC_OK;
        }
    }
    GROW(b->fams, b->nfams, b->cap_fams, err);
    v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        return NOMEM(err);
    }
    if (n > 0) {
        memcpy(v, fams, n * sizeof *v);
    }
    b->fams[b->nfams] = (fam_list){v, n};
    *id = b->nfams++;
    return PC_OK;
}

/* Stable insertion sort by tag (slice::sort_by_key is stable). */
#define SORT_BY_TAG(v, n, T)                                                                                 \
    do {                                                                                                     \
        size_t i_, j_;                                                                                       \
        for (i_ = 1; i_ < (n); i_++) {                                                                       \
            T x_ = (v)[i_];                                                                                  \
            for (j_ = i_; j_ > 0 && (v)[j_ - 1].tag > x_.tag; j_--) {                                        \
                (v)[j_] = (v)[j_ - 1];                                                                       \
            }                                                                                                \
            (v)[j_] = x_;                                                                                    \
        }                                                                                                    \
    } while (0)

pc_status pl_builder_features(pl_builder *b, pl_feature *f, size_t n, size_t *id, pc_error *err)
{
    size_t i;
    pl_feature *v;
    if (n == 0) {
        *id = SIZE_MAX;
        return PC_OK;
    }
    SORT_BY_TAG(f, n, pl_feature);
    for (i = 0; i < b->nfeats; i++) {
        if (b->feats[i].n == n && memcmp(b->feats[i].v, f, n * sizeof *f) == 0) {
            *id = i;
            return PC_OK;
        }
    }
    GROW(b->feats, b->nfeats, b->cap_feats, err);
    v = pc_malloc_array(n, sizeof *v);
    if (v == NULL) {
        return NOMEM(err);
    }
    memcpy(v, f, n * sizeof *v);
    b->feats[b->nfeats] = (feat_list){v, n};
    *id = b->nfeats++;
    return PC_OK;
}

pc_status pl_builder_variations(pl_builder *b, pl_variation *f, size_t n, size_t *id, pc_error *err)
{
    size_t i, k;
    pl_variation *v;
    if (n == 0) {
        *id = SIZE_MAX;
        return PC_OK;
    }
    SORT_BY_TAG(f, n, pl_variation);
    for (i = 0; i < b->nvars; i++) {
        bool same = b->vars[i].n == n;
        for (k = 0; same && k < n; k++) {
            /* FontVariation's derived PartialEq compares the f32 values. */
            same = b->vars[i].v[k].tag == f[k].tag && b->vars[i].v[k].value == f[k].value;
        }
        if (same) {
            *id = i;
            return PC_OK;
        }
    }
    GROW(b->vars, b->nvars, b->cap_vars, err);
    v = pc_malloc_array(n, sizeof *v);
    if (v == NULL) {
        return NOMEM(err);
    }
    memcpy(v, f, n * sizeof *v);
    b->vars[b->nvars] = (var_list){v, n};
    *id = b->nvars++;
    return PC_OK;
}

pc_status pl_builder_push_default(pl_builder *b, const pl_prop *p, pc_error *err)
{
    (void)err;
    pl_apply_prop(&b->root, p);
    return PC_OK;
}

pc_status pl_builder_push(pl_builder *b, const pl_prop *p, pc_error *err)
{
    GROW(b->props, b->nprops, b->cap_props, err);
    b->props[b->nprops++] = *p;
    return PC_OK;
}

/* ---- HarfBuzz faces and charmaps, cached per font ---- */

static pc_status face_of(pl_builder *b, pc_font *font, face_entry **out, pc_error *err)
{
    size_t i, len;
    const uint8_t *data;
    face_entry *e;
    pc_sfnt s;
    for (i = 0; i < b->nfaces; i++) {
        if (b->faces[i].font == font) {
            *out = &b->faces[i];
            return PC_OK;
        }
    }
    data = pc_font_data(font, &len);
    if (len > (size_t)UINT_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "text layout: font too large");
    }
    GROW(b->faces, b->nfaces, b->cap_faces, err);
    e = &b->faces[b->nfaces];
    memset(e, 0, sizeof *e);
    e->blob = hb_blob_create((const char *)data, (unsigned)len, HB_MEMORY_MODE_READONLY, NULL, NULL);
    e->face = hb_face_create(e->blob, pc_font_index(font));
    e->font = pc_font_retain(font);
    pc_font_sfnt(font, &s);
    e->has_cmap = pc_sfnt_cmap_fontique(&s, &e->cmap);
    b->nfaces++;
    *out = e;
    return PC_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Character clusters and coverage (parley analysis::cluster) */

#define MAX_CLUSTER_SIZE 32

typedef struct pl_char {
    uint32_t ch;
    bool contributes, is_control;
    uint16_t style;
} pl_char;

enum { FORM_NONE, FORM_VALID, FORM_INVALID };

typedef struct form {
    pl_char chars[2];
    uint8_t len, map_len;
    bool has_contributing;
    int state;
} form;

typedef struct char_cluster {
    pl_char *chars;
    size_t n, cap;
    bool is_emoji;
    uint8_t map_len;
    uint32_t start, end;
    bool force_normalize;
    form comp, decomp;
    float best_ratio;
} char_cluster;

static void form_setup(form *f)
{
    size_t i, k = 0;
    f->has_contributing = false;
    for (i = 0; i < f->len; i++) {
        k += f->chars[i].is_control ? 0 : 1;
        f->has_contributing = f->has_contributing || f->chars[i].contributes;
    }
    f->map_len = (uint8_t)(k > 0 ? k : 1);
}

/* Mapper::map: the share of characters the font covers. */
static float mapper(const pl_char *chars, size_t n, uint8_t map_len, bool has_contributing,
                    const face_entry *fe)
{
    size_t i, mapped = 0;
    if (map_len == 0) {
        return 1.0f;
    }
    if (n > MAX_CLUSTER_SIZE) {
        n = MAX_CLUSTER_SIZE; /* zipped with a 32-entry glyph buffer */
    }
    for (i = 0; i < n; i++) {
        bool has = pc_sfnt_cmap_map_any(&fe->cmap, chars[i].ch) != 0;
        if (!chars[i].contributes) {
            if (!has_contributing) {
                mapped++;
            }
        } else if (has) {
            mapped++;
        }
    }
    return (float)mapped / (float)map_len;
}

static bool composed(char_cluster *c)
{
    if (c->comp.state == FORM_INVALID) {
        return false;
    }
    if (c->comp.state == FORM_NONE) {
        hb_codepoint_t ab;
        c->comp.state = FORM_INVALID;
        if (c->n != 2) {
            return false;
        }
        if (hb_unicode_compose(hb_unicode_funcs_get_default(), c->chars[0].ch, c->chars[1].ch, &ab)) {
            pl_char copy = c->chars[0];
            copy.ch = ab;
            copy.contributes = pc_uprops_contributes_to_shaping(pc_uprops_get(ab));
            c->comp.chars[0] = copy;
            c->comp.len = 1;
        }
        c->comp.state = FORM_VALID;
        form_setup(&c->comp);
    }
    return true;
}

static bool decomposed(char_cluster *c)
{
    if (c->decomp.state == FORM_INVALID) {
        return false;
    }
    if (c->decomp.state == FORM_NONE) {
        hb_codepoint_t a, b;
        pl_char copy;
        c->decomp.state = FORM_INVALID;
        if (c->n != 1) {
            return false;
        }
        if (!hb_unicode_decompose(hb_unicode_funcs_get_default(), c->chars[0].ch, &a, &b) || b == 0) {
            return false; /* no decomposition, or a singleton */
        }
        copy = c->chars[0];
        copy.ch = a;
        copy.contributes = pc_uprops_contributes_to_shaping(pc_uprops_get(a));
        c->decomp.chars[0] = copy;
        copy.ch = b;
        copy.contributes = pc_uprops_contributes_to_shaping(pc_uprops_get(b));
        c->decomp.chars[1] = copy;
        c->decomp.len = 2;
        c->decomp.state = FORM_VALID;
        form_setup(&c->decomp);
    }
    return true;
}

enum { MAP_DISCARD, MAP_KEEP, MAP_COMPLETE };

/* CharCluster::map. */
static int cluster_map(char_cluster *c, const face_entry *fe)
{
    float prev = c->best_ratio, ratio;
    if (c->n == 0) {
        return MAP_COMPLETE;
    }
    if (c->force_normalize && composed(c)) {
        ratio = mapper(c->comp.chars, c->comp.len, c->comp.map_len, c->comp.has_contributing, fe);
        if (ratio > c->best_ratio) {
            c->best_ratio = ratio;
            if (ratio >= 1.0f) {
                return MAP_COMPLETE;
            }
        }
    }
    ratio = mapper(c->chars, c->n, c->map_len > 0 ? c->map_len : 1, c->map_len > 0, fe);
    if (ratio > c->best_ratio) {
        c->best_ratio = ratio;
        if (ratio >= 1.0f) {
            return MAP_COMPLETE;
        }
    }
    if (decomposed(c)) {
        ratio = mapper(c->decomp.chars, c->decomp.len, c->decomp.map_len, c->decomp.has_contributing, fe);
        if (ratio > c->best_ratio) {
            c->best_ratio = ratio;
            if (ratio >= 1.0f) {
                return MAP_COMPLETE;
            }
        }
        if (!c->force_normalize && composed(c)) {
            ratio = mapper(c->comp.chars, c->comp.len, c->comp.map_len, c->comp.has_contributing, fe);
            if (ratio > c->best_ratio) {
                c->best_ratio = ratio;
                if (ratio >= 1.0f) {
                    return MAP_COMPLETE;
                }
            }
        }
    }
    return c->best_ratio > prev ? MAP_KEEP : MAP_DISCARD;
}

/* fill_cluster_in_place over the infos of one grapheme. */
static pc_status fill_cluster(char_cluster *c, const pl_info *infos, size_t n, uint32_t start, pc_error *err)
{
    size_t i;
    uint32_t pos = start;
    c->n = 0;
    c->is_emoji = false;
    c->map_len = 0;
    c->force_normalize = false;
    memset(&c->comp, 0, sizeof c->comp);
    memset(&c->decomp, 0, sizeof c->decomp);
    c->best_ratio = 0.0f;
    for (i = 0; i < n; i++) {
        const pl_info *in = &infos[i];
        bool contributes;
        c->force_normalize = c->force_normalize || pl_force_normalize(in->cp, in->props);
        c->is_emoji = c->is_emoji || pc_uprops_is_emoji_or_pictograph(in->props);
        pos += (uint32_t)utf8_len(in->cp);
        contributes = pc_uprops_contributes_to_shaping(in->props) &&
                      !(c->is_emoji && pc_uprops_is_variation_selector(in->props));
        if (contributes) {
            c->map_len = (uint8_t)(c->map_len + 1u); /* u8 arithmetic, as the reference */
        }
        GROW(c->chars, c->n, c->cap, err);
        c->chars[c->n++] = (pl_char){in->cp, contributes, pc_uprops_gc(in->props) == PC_GC_CC, in->style};
    }
    c->start = start;
    c->end = pos;
    return PC_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Font selection (parley FontSelector over fontique's Query) */

typedef struct fam_fonts {
    bool best_ok, def_ok, def_differs;
    pl_font_choice best, def;
} fam_fonts;

typedef struct selector {
    pl_builder *b;
    size_t families; /* cached for this list and these attributes */
    float weight;
    bool italic;
    bool valid;
    fam_fonts *fonts;
    size_t nfonts, cap;
} selector;

static bool load_choice(pl_builder *b, const pc_fontdb_family *fam, long ix, const pc_font_query *q,
                        pl_font_choice *out)
{
    pc_font *font;
    if (ix < 0 || (size_t)ix >= fam->nfaces) {
        return false;
    }
    font = pc_fontdb_face_font(b->db, fam->name, fam->len, (size_t)ix, NULL);
    if (font == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->family = fam;
    out->face = (size_t)ix;
    out->font = font;
    pc_fontdb_face_synthesis(&fam->faces[ix], q, &out->synth);
    out->attr_weight = q->weight;
    out->attr_italic = q->slope == PC_SLOPE_ITALIC;
    return true;
}

static pc_status selector_prepare(selector *s, const pl_style *st, pc_error *err)
{
    const fam_list *fl;
    pc_font_query q = {1.0f, st->italic ? PC_SLOPE_ITALIC : PC_SLOPE_NORMAL, false, 0.0f, st->weight};
    pc_font_query dq = {1.0f, PC_SLOPE_NORMAL, false, 0.0f, 400.0f};
    size_t i;
    if (s->valid && s->families == st->families && s->weight == st->weight && s->italic == st->italic) {
        return PC_OK;
    }
    s->nfonts = 0;
    s->valid = true;
    s->families = st->families;
    s->weight = st->weight;
    s->italic = st->italic;
    if (st->families >= s->b->nfams) {
        return PC_OK;
    }
    fl = &s->b->fams[st->families];
    for (i = 0; i < fl->n; i++) {
        const pc_fontdb_family *fam = fl->v[i];
        fam_fonts f;
        long best = pc_fontdb_match_index(fam, &q, true);
        long def = pc_fontdb_match_index(fam, &dq, false);
        memset(&f, 0, sizeof f);
        if (def < 0) {
            def = 0;
        }
        f.best_ok = load_choice(s->b, fam, best, &q, &f.best);
        f.def_differs = !(f.best_ok && best == def);
        if (f.def_differs) {
            f.def_ok = load_choice(s->b, fam, def, &q, &f.def);
        }
        GROW(s->fonts, s->nfonts, s->cap, err);
        s->fonts[s->nfonts++] = f;
    }
    return PC_OK;
}

static bool synth_eq(const pc_font_match *a, const pc_font_match *b)
{
    size_t i;
    if (a->nvars != b->nvars || a->embolden != b->embolden || a->skew_deg != b->skew_deg) {
        return false;
    }
    for (i = 0; i < a->nvars; i++) {
        if (a->var_tags[i] != b->var_tags[i] || a->var_values[i] != b->var_values[i]) {
            return false;
        }
    }
    return true;
}

static bool choice_eq(const pl_font_choice *a, const pl_font_choice *b)
{
    return a->family == b->family && a->face == b->face && synth_eq(&a->synth, &b->synth);
}

/* Offers one font to the cluster; *stop when the query ends. */
static pc_status offer(pl_builder *b, char_cluster *c, const pl_font_choice *f, pl_font_choice *sel,
                       bool *has, bool *stop, pc_error *err)
{
    face_entry *fe;
    int status;
    pc_status st = face_of(b, f->font, &fe, err);
    *stop = false;
    if (st != PC_OK || !fe->has_cmap) {
        return st;
    }
    status = cluster_map(c, fe);
    if (status == MAP_COMPLETE || status == MAP_KEEP || !*has) {
        *sel = *f;
        *has = true;
    }
    *stop = status == MAP_COMPLETE;
    return PC_OK;
}

/* FontSelector::select_font. The emoji generic family is empty in this collection (as in the
 * reference, which registers none), so emoji clusters query the same families. */
static pc_status select_font(selector *s, const pl_style *styles, size_t nstyles, char_cluster *c,
                             pl_font_choice *out, bool *found, pc_error *err)
{
    const pl_style *st;
    size_t i;
    pc_status r;
    bool stop = false;
    *found = false;
    if (c->n == 0 || c->chars[0].style >= nstyles) {
        return PC_OK;
    }
    st = &styles[c->chars[0].style];
    r = selector_prepare(s, st, err);
    if (r != PC_OK) {
        return r;
    }
    for (i = 0; i < s->nfonts && !stop; i++) {
        fam_fonts *f = &s->fonts[i];
        if (f->best_ok && (r = offer(s->b, c, &f->best, out, found, &stop, err)) != PC_OK) {
            return r;
        }
        if (!stop && f->def_differs && f->def_ok &&
            (r = offer(s->b, c, &f->def, out, found, &stop, err)) != PC_OK) {
            return r;
        }
    }
    return PC_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Shaping (parley shape::shape_text / shape_item and LayoutData::push_run) */

typedef struct item {
    uint16_t style_index;
    float size;
    uint16_t script;
    uint8_t level;
    bool has_locale;
    pl_lang locale;
    size_t variations, features;
    float letter_spacing;
} item;

static bool real_script(uint16_t s)
{
    return s != PC_SC_COMMON && s != PC_SC_UNKNOWN && s != PC_SC_INHERITED;
}

typedef struct shape_ctx {
    pl_builder *b;
    pl_layout *out;
    const char *text;
    const pl_info *infos;
    size_t ninfos;
    selector sel;
    char_cluster cluster;
    char *gbrks;
    size_t cap_gbrks;
} shape_ctx;

static pc_status push_cluster(pl_layout *l, const pl_info *ci, uint32_t ch, size_t at, uint32_t glyph_offset,
                              float advance, uint32_t total, int type, bool has_inline, uint32_t inline_id,
                              pc_error *err)
{
    enum { REG, LIG_START, LIG_COMP, NEWLINE };
    pl_cluster c;
    memset(&c, 0, sizeof c);
    c.boundary = ci->boundary;
    c.source_char = ch;
    c.flags = type == LIG_START  ? PL_CLUSTER_LIGATURE_START
              : type == LIG_COMP ? PL_CLUSTER_LIGATURE_COMPONENT
                                 : 0;
    c.style_index = ci->style;
    c.text_len = (uint8_t)utf8_len(ch);
    c.text_offset = (uint16_t)at; /* `as u16`, as the reference */
    if (type == LIG_COMP) {
        c.glyph_len = 0;
        c.glyph_offset = 0;
        c.advance = advance;
    } else if (type == NEWLINE) {
        c.glyph_len = 0;
        c.glyph_offset = 0;
        c.advance = 0.0f;
    } else if (has_inline) {
        c.glyph_len = 0xFF;
        c.glyph_offset = inline_id;
        c.advance = advance;
    } else {
        c.glyph_len = (uint8_t)(total - glyph_offset);
        c.glyph_offset = glyph_offset;
        c.advance = advance;
    }
    GROW(l->clusters, l->nclusters, l->cap_clusters, err);
    l->clusters[l->nclusters++] = c;
    return PC_OK;
}

static pc_status push_glyph(pl_layout *l, const pl_glyph *g, pc_error *err)
{
    GROW(l->glyphs, l->nglyphs, l->cap_glyphs, err);
    l->glyphs[l->nglyphs++] = *g;
    return PC_OK;
}

/* The segment's chars in shaping order: (byte offset in the segment, char). */
typedef struct seg_char {
    size_t at;
    uint32_t ch;
} seg_char;

/* process_clusters: glyphs in visual order to logical clusters. */
static pc_status process_clusters(pl_layout *l, bool rtl, float scale, const hb_glyph_info_t *gi,
                                  const hb_glyph_position_t *gp, unsigned ng, const pl_info *ci, size_t nci,
                                  const seg_char *chars, size_t nchars, float *run_advance, pc_error *err)
{
    enum { REG, LIG_START, LIG_COMP, NEWLINE };
    size_t it = 0;
    seg_char start_char;
    uint32_t total = 0, offset = 0, cluster_id, last_cluster_id;
    const pl_info *cinfo;
    float adv_run = 0.0f, cluster_advance = 0.0f;
    bool has_pending = false;
    pl_glyph pending;
    unsigned k;
    pc_status st;
#define NEXT_CHAR(dst)                                                                                       \
    do {                                                                                                     \
        if (it >= nchars) {                                                                                  \
            goto done; /* the reference would panic: stop with what we have */                               \
        }                                                                                                    \
        (dst) = chars[rtl ? nchars - 1 - it : it];                                                           \
        it++;                                                                                                \
    } while (0)
    memset(&pending, 0, sizeof pending);
    NEXT_CHAR(start_char);
    cluster_id = gi[0].cluster;
    if (cluster_id >= nci) {
        goto done;
    }
    cinfo = &ci[cluster_id];
    last_cluster_id = rtl ? (uint32_t)nci : 0;
    for (k = 0; k < ng; k++) {
        pl_glyph g;
        if (cluster_id != gi[k].cluster) {
            uint32_t num = rtl ? last_cluster_id - cluster_id : gi[k].cluster - cluster_id;
            bool newline;
            int type;
            bool has_inline = false;
            uint32_t inline_id = 0;
            adv_run += cluster_advance;
            cluster_advance /= (float)num;
            newline = pl_whitespace(start_char.ch) == PL_WS_NEWLINE;
            type = num > 1 ? LIG_START : newline ? NEWLINE : REG;
            if (type == REG) {
                if (has_pending) {
                    has_inline = true;
                    inline_id = pending.id;
                }
            } else if (has_pending) {
                if ((st = push_glyph(l, &pending, err)) != PC_OK) {
                    return st;
                }
                total++;
            }
            st = push_cluster(l, cinfo, start_char.ch, start_char.at, offset, cluster_advance, total, type,
                              has_inline, inline_id, err);
            if (st != PC_OK) {
                return st;
            }
            offset = total;
            if (num > 1) {
                uint32_t i;
                for (i = 1; i < num; i++) {
                    size_t ix;
                    NEXT_CHAR(start_char);
                    if (pl_whitespace(start_char.ch) == PL_WS_SPACE) {
                        break;
                    }
                    ix = rtl ? (size_t)cluster_id + num - i : (size_t)cluster_id + i;
                    if (ix >= nci) {
                        goto done;
                    }
                    st = push_cluster(l, &ci[ix], start_char.ch, start_char.at, offset, cluster_advance,
                                      total, LIG_COMP, false, 0, err);
                    if (st != PC_OK) {
                        return st;
                    }
                }
            }
            NEXT_CHAR(start_char);
            cluster_advance = 0.0f;
            last_cluster_id = cluster_id;
            cluster_id = gi[k].cluster;
            if (cluster_id >= nci) {
                goto done;
            }
            cinfo = &ci[cluster_id];
            has_pending = false;
        }
        g.id = gi[k].codepoint;
        g.style_index = cinfo->style;
        g.x = (float)gp[k].x_offset * scale;
        g.y = -((float)gp[k].y_offset * scale);
        g.advance = (float)gp[k].x_advance * scale;
        cluster_advance += g.advance;
        if (has_pending) {
            if ((st = push_glyph(l, &pending, err)) != PC_OK) {
                return st;
            }
            total++;
            has_pending = false;
        }
        if (total == offset && g.x == 0.0f && g.y == 0.0f) {
            pending = g;
            has_pending = true;
        } else {
            if ((st = push_glyph(l, &g, err)) != PC_OK) {
                return st;
            }
            total++;
        }
    }
    {
        uint32_t next = rtl ? 0 : (uint32_t)nci;
        uint32_t num = rtl ? last_cluster_id - cluster_id : next - cluster_id;
        if (num > 1) {
            float lig = cluster_advance / (float)num;
            uint32_t i;
            if (has_pending) {
                if ((st = push_glyph(l, &pending, err)) != PC_OK) {
                    return st;
                }
                total++;
            }
            st = push_cluster(l, cinfo, start_char.ch, start_char.at, offset, lig, total, LIG_START, false, 0,
                              err);
            if (st != PC_OK) {
                return st;
            }
            offset = total;
            for (i = 1; it < nchars; i++) {
                seg_char c2;
                size_t ix;
                NEXT_CHAR(c2);
                if (pl_whitespace(c2.ch) == PL_WS_SPACE) {
                    break;
                }
                ix = rtl ? (size_t)cluster_id + num - i : (size_t)cluster_id + i;
                if (ix >= nci) {
                    goto done;
                }
                st = push_cluster(l, &ci[ix], c2.ch, c2.at, offset, lig, total, LIG_COMP, false, 0, err);
                if (st != PC_OK) {
                    return st;
                }
            }
        } else {
            bool newline = pl_whitespace(start_char.ch) == PL_WS_NEWLINE;
            bool has_inline = false;
            uint32_t inline_id = 0;
            if (!newline) {
                if (total == offset && has_pending) {
                    has_inline = true;
                    inline_id = pending.id;
                }
            } else if (has_pending) {
                if ((st = push_glyph(l, &pending, err)) != PC_OK) {
                    return st;
                }
                total++;
            }
            st = push_cluster(l, cinfo, start_char.ch, start_char.at, offset, cluster_advance, total,
                              newline ? NEWLINE : REG, has_inline, inline_id, err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
done:
#undef NEXT_CHAR
    *run_advance = adv_run;
    return PC_OK;
}

/* LayoutData::push_run. */
static pc_status push_run(shape_ctx *sc, const pl_font_choice *font, const item *it, const int16_t *coords,
                          size_t ncoords, const hb_glyph_info_t *gi, const hb_glyph_position_t *gp,
                          unsigned ng, size_t seg_start, size_t seg_end, const pl_info *seg_infos,
                          size_t nseg, pc_error *err)
{
    pl_layout *l = sc->out;
    pl_run run;
    pl_metrics m;
    size_t i, coords_start = l->ncoords, cluster_start;
    float upem, scale, size = it->size;
    seg_char *chars = NULL;
    size_t nchars = 0;
    bool rtl = (it->level & 1) != 0;
    pc_status st;
    for (i = 0; i < ncoords; i++) {
        GROW(l->coords, l->ncoords, l->cap_coords, err);
        l->coords[l->ncoords++] = coords[i];
    }
    memset(&run, 0, sizeof run);
    for (i = 0; i < l->nfonts && l->fonts[i] != font->font; i++) {
    }
    if (i == l->nfonts) {
        GROW(l->fonts, l->nfonts, l->cap_fonts, err);
        l->fonts[l->nfonts++] = font->font;
    }
    run.font_index = i;
    pl_font_metrics(font->font, &size, coords, ncoords, &m);
    upem = (float)m.units_per_em;
    run.font_size = size;
    run.attr_weight = font->attr_weight;
    run.attr_italic = font->attr_italic;
    run.synth = font->synth;
    run.coords_start = coords_start;
    run.coords_end = l->ncoords;
    run.text_start = seg_start;
    run.text_end = seg_end;
    run.bidi_level = it->level;
    run.glyph_start = l->nglyphs;
    run.letter_spacing = it->letter_spacing;
    run.metrics.ascent = m.ascent;
    run.metrics.descent = -m.descent;
    run.metrics.leading = m.leading;
    if (m.has_underline) {
        run.metrics.underline_offset = m.underline_offset;
        run.metrics.underline_size = m.underline_thickness;
    } else {
        run.metrics.underline_offset = upem / 18.0f;
        run.metrics.underline_size = upem / 18.0f;
    }
    if (m.has_strikeout) {
        run.metrics.strikethrough_offset = m.strikeout_offset;
        run.metrics.strikethrough_size = m.strikeout_thickness;
    } else {
        run.metrics.strikethrough_offset = m.ascent / 2.0f;
        run.metrics.strikethrough_size = upem / 18.0f;
    }
    run.metrics.line_height = (m.ascent - m.descent + m.leading) * 1.0f;
    if (ng == 0) {
        return PC_OK;
    }
    chars = pc_malloc_array(nseg > 0 ? nseg : 1, sizeof *chars);
    if (chars == NULL) {
        return NOMEM(err);
    }
    {
        size_t at = 0, len = seg_end - seg_start;
        while (at < len && nchars < nseg) {
            size_t b0 = at;
            uint32_t cp = next_cp(sc->text + seg_start, len, &at);
            chars[nchars++] = (seg_char){b0, cp};
        }
    }
    scale = size / upem;
    cluster_start = l->nclusters;
    st = process_clusters(l, rtl, scale, gi, gp, ng, seg_infos, nseg, chars, nchars, &run.advance, err);
    free(chars);
    if (st != PC_OK) {
        return st;
    }
    if (rtl) {
        size_t a = cluster_start, z = l->nclusters;
        while (a + 1 < z) {
            pl_cluster t = l->clusters[a];
            l->clusters[a] = l->clusters[z - 1];
            l->clusters[z - 1] = t;
            a++;
            z--;
        }
    }
    run.cluster_start = cluster_start;
    run.cluster_end = l->nclusters;
    if (run.cluster_end > run.cluster_start) {
        GROW(l->runs, l->nruns, l->cap_runs, err);
        l->runs[l->nruns++] = run;
        GROW(l->items, l->nitems, l->cap_items, err);
        l->items[l->nitems++] = l->nruns - 1;
    }
    return PC_OK;
}

/* Shapes one font segment with HarfBuzz. */
static pc_status shape_segment(shape_ctx *sc, const pl_font_choice *font, const item *it,
                               const char *fb_script, size_t seg_start, size_t seg_end, size_t char_start,
                               size_t nseg, pc_error *err)
{
    pl_builder *b = sc->b;
    face_entry *fe;
    hb_font_t *hbf;
    hb_buffer_t *buf;
    hb_variation_t vars[3 + 64];
    hb_feature_t *feats = NULL;
    size_t nvars = 0, nfeats = 0, i;
    const int *nc;
    unsigned ncoords = 0, ng = 0;
    int16_t coords[64];
    const hb_glyph_info_t *gi;
    const hb_glyph_position_t *gp;
    pc_status st = face_of(b, font->font, &fe, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < font->synth.nvars; i++) {
        vars[nvars++] = (hb_variation_t){font->synth.var_tags[i], font->synth.var_values[i]};
    }
    if (it->variations < b->nvars) {
        const var_list *vl = &b->vars[it->variations];
        for (i = 0; i < vl->n && nvars < sizeof vars / sizeof vars[0]; i++) {
            vars[nvars++] = (hb_variation_t){vl->v[i].tag, vl->v[i].value};
        }
    }
    if (it->features < b->nfeats) {
        const feat_list *fl = &b->feats[it->features];
        feats = pc_malloc_array(fl->n, sizeof *feats);
        if (feats == NULL) {
            return NOMEM(err);
        }
        for (i = 0; i < fl->n; i++) {
            feats[i] =
                (hb_feature_t){fl->v[i].tag, fl->v[i].value, HB_FEATURE_GLOBAL_START, HB_FEATURE_GLOBAL_END};
        }
        nfeats = fl->n;
    }
    hbf = hb_font_create(fe->face);
    if (nvars > 0) {
        hb_font_set_variations(hbf, vars, (unsigned)nvars);
    }
    hb_font_set_ptem(hbf, it->size);
    nc = hb_font_get_var_coords_normalized(hbf, &ncoords);
    {
        bool zero = true;
        if (ncoords > 64) {
            ncoords = 64;
        }
        for (i = 0; i < ncoords; i++) {
            coords[i] = (int16_t)(nc[i] < -16384 ? -16384 : nc[i] > 16384 ? 16384 : nc[i]);
            zero = zero && coords[i] == 0;
        }
        if (zero) {
            ncoords = 0; /* harfrust's ShaperInstance::check_default */
        }
    }
    buf = hb_buffer_create();
    if (!hb_buffer_allocation_successful(buf) || !hb_buffer_pre_allocate(buf, (unsigned)nseg)) {
        hb_buffer_destroy(buf);
        hb_font_destroy(hbf);
        free(feats);
        return NOMEM(err);
    }
    {
        size_t at = seg_start;
        for (i = 0; i < nseg && at < seg_end; i++) {
            uint32_t cp = next_cp(sc->text, seg_end, &at);
            hb_buffer_add(buf, cp, (unsigned)i);
        }
    }
    hb_buffer_set_content_type(buf, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(buf, (it->level & 1) != 0 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_set_script(
        buf, hb_script_from_iso15924_tag(HB_TAG(fb_script[0], fb_script[1], fb_script[2], fb_script[3])));
    if (it->has_locale) {
        hb_buffer_set_language(buf, hb_language_from_string(it->locale.bytes, (int)it->locale.language_len));
    }
    hb_shape(hbf, buf, feats, (unsigned)nfeats);
    free(feats);
    if (!hb_buffer_allocation_successful(buf)) {
        hb_buffer_destroy(buf);
        hb_font_destroy(hbf);
        return NOMEM(err);
    }
    gi = hb_buffer_get_glyph_infos(buf, &ng);
    gp = hb_buffer_get_glyph_positions(buf, NULL);
    st = push_run(sc, font, it, coords, ncoords, gi, gp, ng, seg_start, seg_end, &sc->infos[char_start], nseg,
                  err);
    hb_buffer_destroy(buf);
    hb_font_destroy(hbf);
    return st;
}

/* shape_item: grapheme clusters, font runs, shaping. */
static pc_status shape_item(shape_ctx *sc, const item *it, size_t tstart, size_t tend, size_t cstart,
                            size_t cend, pc_error *err)
{
    size_t len = tend - tstart, ci = cstart, last = 0, cur, gi;
    const char *fb_script = pc_uprops_script_tag(it->script);
    char *brks;
    pl_font_choice font, next;
    bool have, have_next;
    pc_status st;
    (void)cend;
    if (len == 0) {
        return PC_OK;
    }
    pl_unibreak_init();
    if (len > sc->cap_gbrks) {
        char *g = realloc(sc->gbrks, len);
        if (g == NULL) {
            return NOMEM(err);
        }
        sc->gbrks = g;
        sc->cap_gbrks = len;
    }
    brks = sc->gbrks;
    set_graphemebreaks_utf8((const utf8_t *)sc->text + tstart, len, NULL, brks);
    /* Grapheme ends: positions p with a break before p, plus the end. */
#define NEXT_BOUNDARY(from, out)                                                                             \
    do {                                                                                                     \
        size_t p_ = (from) + 1;                                                                              \
        while (p_ < len && brks[p_ - 1] != GRAPHEMEBREAK_BREAK) {                                            \
            p_++;                                                                                            \
        }                                                                                                    \
        (out) = p_ < len ? p_ : len;                                                                         \
    } while (0)
    NEXT_BOUNDARY(0, cur);
#define FILL(a, b)                                                                                           \
    do {                                                                                                     \
        size_t i_ = tstart + (a), n_ = 0;                                                                    \
        while (i_ < tstart + (b)) {                                                                          \
            (void)next_cp(sc->text, tstart + (b), &i_);                                                      \
            n_++;                                                                                            \
        }                                                                                                    \
        if (ci + n_ > sc->ninfos) {                                                                          \
            n_ = sc->ninfos - ci;                                                                            \
        }                                                                                                    \
        st = fill_cluster(&sc->cluster, &sc->infos[ci], n_, (uint32_t)(tstart + (a)), err);                  \
        if (st != PC_OK) {                                                                                   \
            return st;                                                                                       \
        }                                                                                                    \
        ci += n_;                                                                                            \
    } while (0)
    FILL(last, cur);
    st = select_font(&sc->sel, sc->out->styles, sc->out->nstyles, &sc->cluster, &font, &have, err);
    if (st != PC_OK) {
        return st;
    }
    while (have) {
        size_t seg_start = sc->cluster.start - tstart, seg_end = sc->cluster.end - tstart, char_start,
               nseg = 0;
        bool changed = false;
        while (cur < len) {
            last = cur;
            NEXT_BOUNDARY(cur, gi);
            cur = gi;
            FILL(last, cur);
            st = select_font(&sc->sel, sc->out->styles, sc->out->nstyles, &sc->cluster, &next, &have_next,
                             err);
            if (st != PC_OK) {
                return st;
            }
            if (!have_next) {
                continue;
            }
            if (!choice_eq(&next, &font)) {
                changed = true;
                break;
            }
            seg_end = sc->cluster.end - tstart;
        }
        /* Characters of the segment and the index of its first one. */
        {
            size_t i = tstart, k = 0;
            while (i < tstart + seg_start) {
                (void)next_cp(sc->text, tstart + seg_start, &i);
                k++;
            }
            char_start = cstart + k;
            i = tstart + seg_start;
            while (i < tstart + seg_end) {
                (void)next_cp(sc->text, tstart + seg_end, &i);
                nseg++;
            }
            if (char_start > sc->ninfos) {
                char_start = sc->ninfos;
            }
            if (char_start + nseg > sc->ninfos) {
                nseg = sc->ninfos - char_start;
            }
        }
        st = shape_segment(sc, &font, it, fb_script, tstart + seg_start, tstart + seg_end, char_start, nseg,
                           err);
        if (st != PC_OK) {
            return st;
        }
        have = changed;
        font = next;
    }
#undef FILL
#undef NEXT_BOUNDARY
    return PC_OK;
}

static pc_status shape_text(shape_ctx *sc, const char *text, size_t len, pc_error *err)
{
    const pl_style *styles = sc->out->styles;
    size_t nstyles = sc->out->nstyles, i, at = 0, tstart = 0, tend = 0, cstart = 0, cend = 0;
    const pl_style *style;
    item it;
    pc_status st;
    if (len == 0) {
        text = " ";
        len = 1;
    }
    sc->text = text;
    if (nstyles == 0 || sc->ninfos == 0) {
        return PC_OK;
    }
    memset(&it, 0, sizeof it);
    it.style_index = sc->infos[0].style;
    style = &styles[it.style_index < nstyles ? it.style_index : 0];
    it.size = style->size;
    it.level = sc->infos[0].level;
    it.script = PC_SC_LATIN;
    for (i = 0; i < sc->ninfos; i++) {
        if (real_script(pc_uprops_script(sc->infos[i].props))) {
            it.script = pc_uprops_script(sc->infos[i].props);
            break;
        }
    }
    it.has_locale = style->has_locale;
    it.locale = style->locale;
    it.variations = style->variations;
    it.features = style->features;
    it.letter_spacing = style->letter_spacing;
    for (i = 0; i < sc->ninfos && at < len; i++) {
        const pl_info *in = &sc->infos[i];
        size_t b0 = at;
        bool brk = false;
        uint16_t script = pc_uprops_script(in->props);
        (void)next_cp(text, len, &at);
        if (!real_script(script)) {
            script = it.script;
        }
        if (it.style_index != in->style) {
            it.style_index = in->style;
            style = &styles[in->style < nstyles ? in->style : 0];
            if (!nearly_eq(style->size, it.size) ||
                !pl_lang_eq(style->has_locale, &style->locale, it.has_locale, &it.locale) ||
                style->variations != it.variations || style->features != it.features ||
                !nearly_eq(style->letter_spacing, it.letter_spacing)) {
                brk = true;
            }
        }
        if (in->level != it.level || script != it.script) {
            brk = true;
        }
        if (brk && tend > tstart) {
            st = shape_item(sc, &it, tstart, tend, cstart, cend, err);
            if (st != PC_OK) {
                return st;
            }
            it.size = style->size;
            it.level = in->level;
            it.script = script;
            it.has_locale = style->has_locale;
            it.locale = style->locale;
            it.variations = style->variations;
            it.features = style->features;
            it.letter_spacing = style->letter_spacing;
            tstart = tend;
            cstart = cend;
        }
        tend += at - b0;
        cend++;
    }
    if (tend > tstart) {
        return shape_item(sc, &it, tstart, tend, cstart, cend, err);
    }
    return PC_OK;
}

/* LayoutData::finish: letter spacing. */
static void finish_spacing(pl_layout *l)
{
    size_t r, c;
    for (r = 0; r < l->nruns; r++) {
        const pl_run *run = &l->runs[r];
        float letter = run->letter_spacing;
        if (nearly_zero(letter)) {
            continue;
        }
        for (c = run->cluster_start; c < run->cluster_end && c < l->nclusters; c++) {
            pl_cluster *cl = &l->clusters[c];
            float spacing = letter;
            if (!nearly_zero(spacing)) {
                cl->advance += spacing;
                if (cl->glyph_len != 0xFF && cl->glyph_len > 0) {
                    size_t end = run->glyph_start + cl->glyph_offset + cl->glyph_len;
                    if (end <= l->nglyphs) {
                        l->glyphs[end - 1].advance += spacing;
                    }
                }
            }
        }
    }
}

void pl_layout_fini(pl_layout *l)
{
    if (l == NULL) {
        return;
    }
    free(l->styles);
    free(l->fonts);
    free(l->coords);
    free(l->runs);
    free(l->items);
    free(l->clusters);
    free(l->glyphs);
    free(l->lines);
    free(l->line_items);
    memset(l, 0, sizeof *l);
}

pc_status pl_builder_build(pl_builder *b, const char *text, size_t len, pl_layout *out, pc_error *err)
{
    pl_style_run *runs = NULL;
    size_t nruns = 0, i, at = 0, r = 0;
    pl_info *infos = NULL;
    size_t ninfos = 0;
    uint8_t base = 0;
    shape_ctx sc;
    pc_status st;
    memset(out, 0, sizeof *out);
    memset(&sc, 0, sizeof sc);
    st = pl_resolve_styles(&b->root, b->props, b->nprops, len, &out->styles, &out->nstyles, &runs, &nruns,
                           err);
    if (st != PC_OK) {
        return st;
    }
    st = pl_analyze_text(text, len, &infos, &ninfos, &base, err);
    if (st != PC_OK) {
        free(runs);
        return st;
    }
    out->base_level = base;
    out->text_len = len;
    /* Style index per char from the style runs. */
    for (i = 0; i < ninfos && len > 0 && at < len; i++) {
        while (r < nruns && runs[r].end <= at) {
            r++;
        }
        if (r < nruns) {
            infos[i].style = runs[r].index;
        }
        (void)next_cp(text, len, &at);
    }
    free(runs);
    sc.b = b;
    sc.out = out;
    sc.infos = infos;
    sc.ninfos = ninfos;
    sc.sel.b = b;
    st = shape_text(&sc, text, len, err);
    free(infos);
    free(sc.sel.fonts);
    free(sc.cluster.chars);
    free(sc.gbrks);
    if (st != PC_OK) {
        return st;
    }
    finish_spacing(out);
    return PC_OK;
}
