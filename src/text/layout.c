/* The layouter (port of crates/text/src/layout.rs Layouter::layout and its helpers). */
#include "text/layout_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/mem.h"
#include "base/rmath.h"
#include "base/utf8.h"
#include "text/cjk.h"
#include "text/craft_fonts.h"
#include "text/fmetrics.h"
#include "text/optical.h"
#include "text/playout.h"
#include "text/served.h"
#include "text/unicode.h"

/* Photoshop synthesizes small caps for faces without `smcp` at this scale. */
#define SYNTHETIC_SMALL_CAPS_SCALE 0.7f

pc_status pc_text_layouter_new(pc_text_layouter **out, pc_error *err)
{
    pc_text_layouter *lo = calloc(1, sizeof *lo);
    pc_status st;
    if (lo == NULL) {
        return NOMEM(err);
    }
    st = pl_builder_new(&lo->b, err);
    if (st == PC_OK) {
        st = pc_optical_cache_new(&lo->optical, err);
    }
    if (st != PC_OK) {
        pc_text_layouter_free(lo);
        return st;
    }
    *out = lo;
    return PC_OK;
}

void pc_text_layouter_free(pc_text_layouter *lo)
{
    if (lo == NULL) {
        return;
    }
    pl_builder_free(lo->b);
    pc_optical_cache_free(lo->optical);
    free(lo);
}

/* ------------------------------------------------------------------------------------------ */
/* Small pure helpers */

/* ------------------------------------------------------------------------------------------ */
/* Style properties */

static uint32_t tag4(const char *s)
{
    return PC_SFNT_TAG(s[0], s[1], s[2], s[3]);
}

/* A tag the CSS settings parser accepts inside double quotes. */
static bool tag_parses(const pc_str *t)
{
    size_t i;
    if (t->len != 4) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)t->ptr[i];
        if (c == '"' || c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool tag_is_ascii4(const pc_str *t)
{
    size_t i;
    if (t->len != 4) {
        return false;
    }
    for (i = 0; i < 4; i++) {
        if ((unsigned char)t->ptr[i] >= 0x80) {
            return false;
        }
    }
    return true;
}

size_t pc_text_style_features(const pc_char_style *st, pc_small_caps_mode mode, pc_text_feature *out,
                              size_t cap)
{
    size_t n = 0, i;
#define ADD(t, v)                                                                                            \
    do {                                                                                                     \
        if (n < cap) {                                                                                       \
            out[n] = (pc_text_feature){(t), (uint16_t)(v)};                                                  \
        }                                                                                                    \
        n++;                                                                                                 \
    } while (0)
    if (st->kerning != PC_KERNING_METRICS || st->kern != 0.0f) {
        ADD(tag4("kern"), 0);
    }
    if (!st->ligatures) {
        ADD(tag4("liga"), 0);
        ADD(tag4("clig"), 0);
    }
    if (st->discretionary_ligatures) {
        ADD(tag4("dlig"), 1);
    }
    if (mode == PC_SMALL_CAPS_OPENTYPE) {
        ADD(tag4("smcp"), 1);
    }
    for (i = 0; i < st->nfeatures; i++) {
        const pc_font_feature *f = &st->features[i];
        if (!tag_is_ascii4(&f->tag)) {
            continue;
        }
        if (!tag_parses(&f->tag)) {
            break; /* the CSS list stops parsing at the first bad entry */
        }
        ADD(tag4(f->tag.ptr), f->value);
    }
#undef ADD
    return n;
}

/* Everything one style_props call pushes, resolved for the builder. */
typedef struct style_props {
    size_t families, features, variations;
    float size, weight, letter_spacing;
    bool italic, underline, strikethrough, has_locale;
    pl_lang locale;
} style_props;

static pc_status features_id(pl_builder *b, const pc_char_style *st, pc_small_caps_mode mode, bool vert,
                             size_t *id, pc_error *err)
{
    size_t n = pc_text_style_features(st, mode, NULL, 0), k;
    pl_feature *f = pc_malloc_array(n + 1, sizeof *f);
    pc_text_feature *t = pc_malloc_array(n + 1, sizeof *t);
    pc_status r;
    if (f == NULL || t == NULL) {
        free(f);
        free(t);
        return NOMEM(err);
    }
    n = pc_text_style_features(st, mode, t, n);
    for (k = 0; k < n; k++) {
        f[k] = (pl_feature){t[k].tag, t[k].value};
    }
    /* "vert" follows the style's list; after a bad entry the parser stops before it too. */
    if (vert) {
        bool stopped = false;
        for (k = 0; k < st->nfeatures; k++) {
            if (tag_is_ascii4(&st->features[k].tag) && !tag_parses(&st->features[k].tag)) {
                stopped = true;
            }
        }
        if (!stopped) {
            f[n++] = (pl_feature){tag4("vert"), 1};
        }
    }
    free(t);
    r = pl_builder_features(b, f, n, id, err);
    free(f);
    return r;
}

static pc_status resolve_props(pl_builder *b, pc_fontdb *db, const pc_char_style *st, float k,
                               const char *const *fallback, size_t nfallback, pc_small_caps_mode mode,
                               style_props *out, pc_error *err)
{
    const pc_fontdb_family **fams = NULL;
    size_t nf = 0, i;
    pc_status r;
    pl_variation *vars = NULL;
    size_t nv = 0;
    const char *sans;
    memset(out, 0, sizeof *out);
    fams = pc_malloc_array(nfallback + 3, sizeof *fams);
    vars = pc_malloc_array(st->nvariations + 2, sizeof *vars);
    if (fams == NULL || vars == NULL) {
        free(fams);
        free(vars);
        return NOMEM(err);
    }
    /* quote() drops '\' and '"'; the name then matches as a family (unknown ones are skipped). */
#define ADD_FAMILY(name, len)                                                                                \
    do {                                                                                                     \
        char *q_ = malloc((len) + 1);                                                                        \
        size_t a_, z_ = 0;                                                                                   \
        const pc_fontdb_family *f_;                                                                          \
        if (q_ == NULL) {                                                                                    \
            free(fams);                                                                                      \
            free(vars);                                                                                      \
            return NOMEM(err);                                                                               \
        }                                                                                                    \
        for (a_ = 0; a_ < (len); a_++) {                                                                     \
            if ((name)[a_] != '\\' && (name)[a_] != '"') {                                                   \
                q_[z_++] = (name)[a_];                                                                       \
            }                                                                                                \
        }                                                                                                    \
        f_ = pc_fontdb_find_family(db, q_, z_);                                                              \
        free(q_);                                                                                            \
        if (f_ != NULL) {                                                                                    \
            fams[nf++] = f_;                                                                                 \
        }                                                                                                    \
    } while (0)
    if (st->font_family.len > 0) {
        ADD_FAMILY(st->font_family.ptr, st->font_family.len);
    }
    for (i = 0; i < nfallback; i++) {
        ADD_FAMILY(fallback[i], strlen(fallback[i]));
    }
#undef ADD_FAMILY
    sans = pc_fontdb_generic(db, "sans-serif");
    if (sans != NULL) {
        const pc_fontdb_family *f = pc_fontdb_find_family(db, sans, strlen(sans));
        if (f != NULL) {
            fams[nf++] = f;
        }
    }
    r = pl_builder_families(b, fams, nf, &out->families, err);
    free(fams);
    if (r == PC_OK) {
        r = features_id(b, st, mode, false, &out->features, err);
    }
    if (r == PC_OK) {
        /* variation_list: the explicit axis values, plus `opsz` at the type size when the style
         * doesn't set it (CSS font-optical-sizing: auto). A tag the settings parser rejects ends
         * the list there. */
        bool stopped = false, has_opsz = false;
        for (i = 0; i < st->nvariations; i++) {
            has_opsz = has_opsz || pc_str_eq_c(&st->variations[i].axis, "opsz");
        }
        for (i = 0; i < st->nvariations; i++) {
            const pc_font_variation *v = &st->variations[i];
            if (!tag_is_ascii4(&v->axis) || !isfinite(v->value)) {
                continue;
            }
            if (!tag_parses(&v->axis)) {
                stopped = true;
                break;
            }
            vars[nv++] = (pl_variation){tag4(v->axis.ptr), v->value};
        }
        if (!stopped && isfinite(st->size_pt) && !has_opsz) {
            vars[nv++] = (pl_variation){tag4("opsz"), pc_rs_maxf_k(st->size_pt, 0.01f)};
        }
        r = pl_builder_variations(b, vars, nv, &out->variations, err);
    }
    free(vars);
    if (r != PC_OK) {
        return r;
    }
    out->size = pc_rs_maxf_k(st->size_pt * k, 0.01f);
    out->weight = (float)(st->weight < 1 ? 1 : st->weight > 1000 ? 1000 : st->weight);
    out->italic = st->italic;
    out->letter_spacing = st->tracking / 1000.0f * out->size;
    out->underline = st->underline;
    out->strikethrough = st->strikethrough;
    out->has_locale = st->has_language && pl_lang_parse(st->language.ptr, st->language.len, &out->locale);
    return PC_OK;
}

static pc_status push_props(pl_builder *b, const style_props *p, uint32_t idx, bool dflt, size_t start,
                            size_t end, pc_error *err)
{
    pl_prop v[11];
    size_t i;
    pc_status r = PC_OK;
    memset(v, 0, sizeof v);
    v[0].kind = PL_PROP_FAMILIES;
    v[0].id = p->families;
    v[1].kind = PL_PROP_SIZE;
    v[1].f = p->size;
    v[2].kind = PL_PROP_WEIGHT;
    v[2].f = p->weight;
    v[3].kind = PL_PROP_ITALIC;
    v[3].b = p->italic;
    v[4].kind = PL_PROP_FEATURES;
    v[4].id = p->features;
    v[5].kind = PL_PROP_VARIATIONS;
    v[5].id = p->variations;
    v[6].kind = PL_PROP_LETTER_SPACING;
    v[6].f = p->letter_spacing;
    v[7].kind = PL_PROP_UNDERLINE;
    v[7].b = p->underline;
    v[8].kind = PL_PROP_STRIKETHROUGH;
    v[8].b = p->strikethrough;
    v[9].kind = PL_PROP_BRUSH;
    v[9].brush_run = idx;
    v[10].kind = PL_PROP_LOCALE;
    v[10].b = p->has_locale;
    v[10].lang = p->locale;
    for (i = 0; i < 11 && r == PC_OK; i++) {
        v[i].start = start;
        v[i].end = end;
        r = dflt ? pl_builder_push_default(b, &v[i], err) : pl_builder_push(b, &v[i], err);
    }
    return r;
}

/* Everything building one paragraph's styled text needs (the reference's `build` closure). */
typedef struct build_ctx {
    pc_text_layouter *lo;
    pc_fontdb *db;
    pc_text_layout *out;
    const para_ctx *pc;
    float k;
    size_t si0;
    const char *const *fallback;
    const char *const *serif_fallback;
    size_t nfallback;
    size_t pstart, content_end, prefix_len;
    const char *ptext;
    size_t plen;
    bool vertical;
    const size_t *sc_ranges; /* synthetic small caps: start, end */
    const float *sc_sizes;
    size_t nsc;
    const size_t *tabs; /* offsets in ptext of the spaces standing in for tabs */
    size_t ntabs;
} build_ctx;

/* Shapes the paragraph (each tab widened by its letter spacing: tab_spacing[i], or `wide` for all
 * when tab_spacing is NULL) into *pl and breaks its lines. */
static pc_status build_and_break(const build_ctx *c, bool is_box, float avail, float first_indent, float wide,
                                 const float *tab_spacing, pl_layout *pl, pc_error *err)
{
    size_t i;
    pc_status st = PC_OK;
    pl_builder_begin(c->lo->b, c->db);
    {
        style_props p;
        const pc_char_style *s0 = &c->out->styles[c->si0];
        bool serif = pc_craft_is_serif_family(s0->font_family.ptr, s0->font_family.len);
        st = resolve_props(c->lo->b, c->db, s0, c->k, serif ? c->serif_fallback : c->fallback, c->nfallback,
                           c->pc->small_caps[c->si0], &p, err);
        if (st == PC_OK) {
            st = push_props(c->lo->b, &p, (uint32_t)c->si0, true, 0, 0, err);
        }
    }
    for (i = style_at(c->pc, c->pstart);
         st == PC_OK && i < c->pc->nruns && c->pc->run_starts[i] < c->content_end; i++) {
        const pc_char_style *s = &c->out->styles[i];
        size_t rs = c->pc->run_starts[i], a = rs > c->pstart ? rs : c->pstart;
        size_t z = rs + c->pc->runs[i].len < c->content_end ? rs + c->pc->runs[i].len : c->content_end;
        size_t ra, rz;
        style_props p;
        bool serif;
        if (a >= z) {
            continue;
        }
        ra = a - c->pstart + c->prefix_len;
        rz = z - c->pstart + c->prefix_len;
        serif = pc_craft_is_serif_family(s->font_family.ptr, s->font_family.len);
        st = resolve_props(c->lo->b, c->db, s, c->k, serif ? c->serif_fallback : c->fallback, c->nfallback,
                           c->pc->small_caps[i], &p, err);
        if (st == PC_OK) {
            st = push_props(c->lo->b, &p, (uint32_t)i, false, ra, rz, err);
        }
        if (st == PC_OK && c->vertical) {
            /* Upright pieces get the vertical alternates and their class in the brush. */
            const char *piece = c->ptext + ra;
            size_t plen = rz - ra, at = 0, from = 0;
            bool has_seg = false;
            pc_vclass cls = PC_VCLASS_ROTATE;
            size_t vid = SIZE_MAX;
            while (st == PC_OK && at <= plen) {
                size_t b0 = at;
                uint32_t ch = 0;
                pc_vclass c2 = PC_VCLASS_ROTATE;
                bool end = at == plen;
                if (!end) {
                    (void)pc_utf8_next((const uint8_t *)piece, plen, &at, &ch);
                    c2 = pc_text_vertical_class(ch);
                }
                if (has_seg && (end || c2 != cls)) {
                    if (cls != PC_VCLASS_ROTATE && from < b0) {
                        pl_prop fp, bp;
                        if (vid == SIZE_MAX) {
                            st = features_id(c->lo->b, s, c->pc->small_caps[i], true, &vid, err);
                        }
                        memset(&fp, 0, sizeof fp);
                        memset(&bp, 0, sizeof bp);
                        fp.kind = PL_PROP_FEATURES;
                        fp.id = vid;
                        fp.start = ra + from;
                        fp.end = ra + b0;
                        bp.kind = PL_PROP_BRUSH;
                        bp.brush_run = (uint32_t)i;
                        bp.brush_class = (uint8_t)cls;
                        bp.start = ra + from;
                        bp.end = ra + b0;
                        if (st == PC_OK) {
                            st = pl_builder_push(c->lo->b, &fp, err);
                        }
                        if (st == PC_OK) {
                            st = pl_builder_push(c->lo->b, &bp, err);
                        }
                    }
                    from = b0;
                }
                if (end) {
                    break;
                }
                if (!has_seg) {
                    from = b0;
                    has_seg = true;
                }
                cls = c2;
            }
        }
    }
    for (i = 0; st == PC_OK && i < c->nsc; i++) {
        pl_prop p;
        memset(&p, 0, sizeof p);
        p.kind = PL_PROP_SIZE;
        p.f = c->sc_sizes[i];
        p.start = c->sc_ranges[2 * i];
        p.end = c->sc_ranges[2 * i + 1];
        st = pl_builder_push(c->lo->b, &p, err);
    }
    for (i = 0; st == PC_OK && i < c->ntabs; i++) {
        pl_prop p;
        memset(&p, 0, sizeof p);
        p.kind = PL_PROP_LETTER_SPACING;
        p.f = tab_spacing != NULL ? tab_spacing[i] : wide;
        p.start = c->tabs[i];
        p.end = c->tabs[i] + 1;
        st = pl_builder_push(c->lo->b, &p, err);
    }
    if (st == PC_OK) {
        pl_layout_fini(pl);
        st = pl_builder_build(c->lo->b, c->ptext, c->plen, pl, err);
    }
    if (st == PC_OK) {
        pl->indent_amount = first_indent != 0.0f ? first_indent : 0.0f;
        st = pl_break_all_lines(pl, is_box ? avail : FLT_MAX, err);
    }
    return st;
}

/* Re-breaks box text with the tab widths its lines give until they agree (at most 4 times);
 * keeps the safe layout in *pl when they don't settle. */
static pc_status settle_tabs(const build_ctx *c, float avail, float first_indent, float indent_start,
                             float tab_px, pl_layout *pl, pc_error *err)
{
    pc_text_tab_width *first = NULL, *got = NULL;
    float *want = NULL, *spacing = NULL;
    pl_layout next;
    size_t i, round;
    pc_status st = PC_OK;
    memset(&next, 0, sizeof next);
    first = pc_malloc_array(c->ntabs, sizeof *first);
    got = pc_malloc_array(c->ntabs, sizeof *got);
    want = pc_malloc_array(c->ntabs, sizeof *want);
    spacing = pc_malloc_array(c->ntabs, sizeof *spacing);
    if (first == NULL || got == NULL || want == NULL || spacing == NULL) {
        st = NOMEM(err);
        goto done;
    }
    st = pc_text_tab_widths(pl, c->out, c->tabs, c->ntabs, indent_start, first_indent, tab_px, first, err);
    for (i = 0; st == PC_OK && i < c->ntabs; i++) {
        want[i] = first[i].width;
    }
    for (round = 0; st == PC_OK && round < 4; round++) {
        bool same = true;
        for (i = 0; i < c->ntabs; i++) {
            spacing[i] = want[i] / first[i].scale - (first[i].advance - tab_px);
        }
        st = build_and_break(c, true, avail, first_indent, 0.0f, spacing, &next, err);
        if (st == PC_OK) {
            st = pc_text_tab_widths(&next, c->out, c->tabs, c->ntabs, indent_start, first_indent, tab_px, got,
                                    err);
        }
        if (st != PC_OK) {
            break;
        }
        for (i = 0; i < c->ntabs; i++) {
            same = same && fabsf(got[i].width - want[i]) < 0.01f;
        }
        if (same) {
            pl_layout_fini(pl);
            *pl = next;
            memset(&next, 0, sizeof next);
            break;
        }
        for (i = 0; i < c->ntabs; i++) {
            want[i] = got[i].width;
        }
    }
done:
    pl_layout_fini(&next);
    free(first);
    free(got);
    free(want);
    free(spacing);
    return st;
}

/* Whether a face of the style's family already has the style's PostScript name. */
static bool family_has_ps_face(pc_fontdb *db, const pc_char_style *s)
{
    pc_face_info *faces = NULL;
    size_t n = 0, i;
    bool has = false;
    if (pc_fontdb_faces(db, pc_str_c(&s->font_family), s->font_family.len, &faces, &n, NULL) != PC_OK) {
        return false;
    }
    for (i = 0; i < n && !has; i++) {
        has = faces[i].postscript_name != NULL &&
              strlen(faces[i].postscript_name) == s->postscript_name.len &&
              memcmp(faces[i].postscript_name, pc_str_c(&s->postscript_name), s->postscript_name.len) == 0;
    }
    pc_face_infos_free(faces, n);
    return has;
}

/* ------------------------------------------------------------------------------------------ */
/* The layout */

pc_status pc_text_layouter_layout(pc_text_layouter *lo, pc_fontdb *db, const pc_text_layer *t, float dpi,
                                  pc_text_layout *out, pc_error *err)
{
    const char *text = pc_str_c(&t->text);
    size_t tlen = t->text.len;
    float k = dpi > 0.0f ? dpi / 72.0f : 1.0f;
    pc_text_run *runs = NULL;
    size_t nruns = 0, npar_runs = 0, i;
    pc_paragraph_run *pruns = NULL;
    size_t *run_starts = NULL, *para_starts = NULL, *paras = NULL, nparas = 0;
    pc_small_caps_mode *small_caps = NULL;
    const char **fallback = NULL, **serif_fallback = NULL, **craft = NULL;
    size_t nfallback = 0, ncraft = 0;
    bool vertical = t->orientation == PC_ORIENT_VERTICAL, is_box = t->shape.kind == PC_TEXT_SHAPE_BOX;
    float box[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool has_prev = false, stop = false;
    float prev_baseline = 0.0f, pending_space = 0.0f;
    pc_buf ptext = PC_BUF_INIT;
    pl_layout pl;
    outv ov;
    para_ctx pc;
    pc_status st = PC_OK;
    size_t pi, *tabs = NULL, cap_tabs = 0;
    float tab_px = PC_TEXT_DEFAULT_TAB_STOP_PT * k;
    pc_text_layout_init(out);
    memset(&pl, 0, sizeof pl);
    memset(&ov, 0, sizeof ov);
    ov.l = out;
    out->px_per_pt = k;
    out->vertical = vertical;
    if (tlen > PC_TEXT_MAX_LAYOUT_BYTES) {
        return pc_fail(err, PC_ERR_LIMIT, "text layout: text longer than %zu bytes",
                       PC_TEXT_MAX_LAYOUT_BYTES);
    }
    {
        size_t at = 0;
        uint32_t cp;
        while (at < tlen) {
            if (!pc_utf8_next((const uint8_t *)text, tlen, &at, &cp)) {
                return pc_fail(err, PC_ERR_INVALID, "text layout: text is not valid UTF-8 (byte %zu)", at);
            }
        }
    }
    if (is_box) {
        box[0] = t->shape.x;
        box[1] = t->shape.y;
        box[2] = t->shape.width;
        box[3] = t->shape.height;
    }
    st = pc_text_layer_char_runs(t, &runs, &nruns, err);
    if (st == PC_OK) {
        st = pc_text_layer_paragraph_runs(t, &pruns, &npar_runs, err);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    /* Styles, with PostScript names resolved (an exact face wins; a guess fills a missing
     * family only). */
    out->styles = calloc(nruns > 0 ? nruns : 1, sizeof *out->styles);
    run_starts = pc_malloc_array(nruns + 1, sizeof *run_starts);
    small_caps = pc_malloc_array(nruns + 1, sizeof *small_caps);
    para_starts = pc_malloc_array(npar_runs + 1, sizeof *para_starts);
    if (out->styles == NULL || run_starts == NULL || small_caps == NULL || para_starts == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < nruns; i++) {
        pc_char_style *s = &out->styles[i];
        st = pc_char_style_clone(&runs[i].style, s, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        out->nstyles++;
        if (s->has_postscript_name && !family_has_ps_face(db, s)) {
            pc_resolved_font f;
            memset(&f, 0, sizeof f);
            st = pc_fontdb_resolve_postscript(db, s->postscript_name.ptr, s->postscript_name.len, &f, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            if (f.exact || (!pc_fontdb_has_family(db, s->font_family.ptr, s->font_family.len) &&
                            pc_fontdb_has_family(db, f.family, strlen(f.family)))) {
                st = pc_str_set_c(&s->font_family, f.family, err);
                s->weight = f.weight;
                s->italic = f.italic;
            }
            pc_resolved_font_fini(&f);
            if (st != PC_OK) {
                goto cleanup;
            }
        }
        /* Drawn with a fallback for now; a host that serves this family would fetch it (served.h:
         * not ported, a no-op). */
        if (!pc_fontdb_has_family(db, s->font_family.ptr, s->font_family.len)) {
            pc_text_served_request(pc_str_c(&s->font_family), s->font_family.len);
        }
        st = pc_fontdb_select_named_face(db, s, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        run_starts[i] = i == 0 ? 0 : run_starts[i - 1] + runs[i - 1].len;
        small_caps[i] = pc_text_small_caps_mode(
            s->caps,
            pc_fontdb_selected_face_has_feature(db, s->font_family.ptr, s->font_family.len, s->weight,
                                                s->italic, PC_SFNT_TAG('s', 'm', 'c', 'p')));
    }
    for (i = 0; i < npar_runs; i++) {
        para_starts[i] = i == 0 ? 0 : para_starts[i - 1] + pruns[i - 1].len;
    }
    /* The fallback families (the default family, then the installed candidates); serif runs
     * put the craft Mincho families first. */
    nfallback = pc_fontdb_fallback_count(db);
    fallback = pc_malloc_array(nfallback + 1, sizeof *fallback);
    serif_fallback = pc_malloc_array(nfallback + 1, sizeof *serif_fallback);
    if (fallback == NULL || serif_fallback == NULL) {
        st = NOMEM(err);
        goto cleanup;
    }
    for (i = 0; i < nfallback; i++) {
        fallback[i] = pc_fontdb_fallback(db, i);
        serif_fallback[i] = fallback[i];
    }
    {
        size_t ncf;
        const pc_craft_font *cf = pc_craft_fonts(&ncf);
        st = pc_craft_japanese_families(cf, ncf, &craft, &ncraft, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        pc_craft_mincho_first(serif_fallback, nfallback, craft, ncraft);
    }
    pc.t = t;
    pc.k = k;
    pc.runs = runs;
    pc.nruns = nruns;
    pc.run_starts = run_starts;
    pc.small_caps = small_caps;
    st = pc_text_split_paragraphs(text, tlen, &paras, &nparas, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    for (pi = 0; pi < nparas && !stop; pi++) {
        size_t pstart = paras[2 * pi], pend = paras[2 * pi + 1], content_end = pend, prefix_len, si0;
        const pc_paragraph_style *ps;
        float indent_start, indent_end, line_origin, line_len, avail = 0.0f, first_px;
        size_t *sc_ranges = NULL, nsc = 0, cap_sc = 0; /* synthetic small caps: start, end */
        size_t ntabs = 0;
        float first_indent;
        build_ctx bc;
        float *sc_sizes = NULL;
        size_t li;
        {
            size_t lo2 = 0, hi2 = npar_runs;
            while (lo2 < hi2) {
                size_t mid = lo2 + (hi2 - lo2) / 2;
                if (para_starts[mid] <= pstart) {
                    lo2 = mid + 1;
                } else {
                    hi2 = mid;
                }
            }
            ps = &pruns[lo2 > 0 ? lo2 - 1 : 0].style;
        }
        while (content_end > pstart && (text[content_end - 1] == '\r' || text[content_end - 1] == '\n')) {
            content_end--;
        }
        pc_buf_truncate(&ptext, 0);
        st = pc_buf_append_str(&ptext,
                               ps->direction == PC_TEXT_DIR_LTR   ? "\xE2\x80\x8E"
                               : ps->direction == PC_TEXT_DIR_RTL ? "\xE2\x80\x8F"
                                                                  : "",
                               err);
        prefix_len = ptext.len;
        {
            size_t at = pstart;
            while (st == PC_OK && at < content_end) {
                size_t b0 = at;
                uint32_t ch;
                size_t si;
                const pc_char_style *s;
                (void)pc_utf8_next((const uint8_t *)text, content_end, &at, &ch);
                if (ch == PC_TEXT_FORCED_LINE_BREAK) {
                    st = pc_buf_append_byte(&ptext, '\n', err);
                    continue;
                }
                /* Imported PSD text can contain literal tabs, which fonts may draw as .notdef: a
                 * space keeps the one-byte source offsets and gives a real whitespace advance; it
                 * is widened to its tab stop after line breaking. */
                if (ch == '\t') {
                    if (ntabs == cap_tabs) {
                        size_t *g = pc_grow_array(tabs, &cap_tabs, ntabs + 1, sizeof *g);
                        if (g == NULL) {
                            st = NOMEM(err);
                            break;
                        }
                        tabs = g;
                    }
                    tabs[ntabs++] = ptext.len;
                    st = pc_buf_append_byte(&ptext, ' ', err);
                    continue;
                }
                si = style_at(&pc, b0);
                s = &out->styles[si];
                if (s->caps == PC_CAPS_ALL_CAPS || small_caps[si] == PC_SMALL_CAPS_SYNTHETIC) {
                    uint32_t up[3];
                    size_t nu = pc_text_to_upper(ch, up), ulen = 0, j;
                    for (j = 0; j < nu; j++) {
                        ulen += up[j] < 0x80 ? 1 : up[j] < 0x800 ? 2 : up[j] < 0x10000 ? 3 : 4;
                    }
                    if (ulen == at - b0) {
                        for (j = 0; j < nu && st == PC_OK; j++) {
                            st = pc_utf8_put(&ptext, up[j], err);
                        }
                        if (st == PC_OK && s->caps == PC_CAPS_SMALL_CAPS && pc_text_is_lowercase(ch)) {
                            if (2 * nsc + 2 > cap_sc) {
                                size_t c2 = cap_sc;
                                void *g = pc_grow_array(sc_ranges, &cap_sc, 2 * nsc + 2, sizeof *sc_ranges);
                                void *h = g != NULL
                                              ? pc_grow_array(sc_sizes, &c2, 2 * nsc + 2, sizeof *sc_sizes)
                                              : NULL;
                                if (g != NULL) {
                                    sc_ranges = g;
                                }
                                if (h == NULL) {
                                    st = NOMEM(err);
                                    break;
                                }
                                sc_sizes = h;
                            }
                            sc_ranges[2 * nsc] = ptext.len - ulen;
                            sc_ranges[2 * nsc + 1] = ptext.len;
                            sc_sizes[nsc] = s->size_pt * k * SYNTHETIC_SMALL_CAPS_SCALE;
                            nsc++;
                        }
                        continue;
                    }
                }
                st = pc_buf_append(&ptext, text + b0, at - b0, err);
            }
        }
        if (st != PC_OK) {
            free(sc_ranges);
            free(sc_sizes);
            goto cleanup;
        }
        si0 = style_at(&pc, pstart);
        first_px = out->styles[si0].size_pt * k;
        /* Build the paragraph's styled text. */
        bc.lo = lo;
        bc.db = db;
        bc.out = out;
        bc.pc = &pc;
        bc.k = k;
        bc.si0 = si0;
        bc.fallback = fallback;
        bc.serif_fallback = serif_fallback;
        bc.nfallback = nfallback;
        bc.pstart = pstart;
        bc.content_end = content_end;
        bc.prefix_len = prefix_len;
        bc.ptext = (const char *)ptext.data;
        bc.plen = ptext.len;
        bc.vertical = vertical;
        bc.sc_ranges = sc_ranges;
        bc.sc_sizes = sc_sizes;
        bc.nsc = nsc;
        bc.tabs = tabs;
        bc.ntabs = ntabs;
        indent_start = ps->start_indent_pt * k;
        indent_end = ps->end_indent_pt * k;
        first_indent = ps->first_line_indent_pt * k;
        line_origin = vertical ? box[1] : box[0];
        line_len = vertical ? box[3] : box[2];
        if (is_box) {
            avail = pc_rs_maxf_k(line_len - indent_start - indent_end, 1.0f);
        }
        st = build_and_break(&bc, is_box, avail, first_indent, is_box ? tab_px : 0.0f, NULL, &pl, err);
        /* Paragraph text: a tab is never wider than a space plus one interval, so breaking at that
         * width keeps expanded tabs inside the box but can wrap early. Break again with the widths
         * that layout gives until they agree; if they don't settle, keep the safe layout. */
        if (st == PC_OK && is_box && ntabs > 0) {
            st = settle_tabs(&bc, avail, first_indent, indent_start, tab_px, &pl, err);
        }
        free(sc_ranges);
        free(sc_sizes);
        sc_ranges = NULL;
        sc_sizes = NULL;
        if (st != PC_OK) {
            goto cleanup;
        }
        {
            pl_align a = PL_ALIGN_LEFT;
            if (is_box) {
                a = ps->align == PC_TEXT_ALIGN_LEFT     ? PL_ALIGN_LEFT
                    : ps->align == PC_TEXT_ALIGN_CENTER ? PL_ALIGN_CENTER
                    : ps->align == PC_TEXT_ALIGN_RIGHT  ? PL_ALIGN_RIGHT
                                                        : PL_ALIGN_JUSTIFY;
            }
            pl_align_lines(&pl, a, !is_box);
        }
        if (has_prev) {
            pending_space += ps->space_before_pt * k;
        }
        for (li = 0; li < pl.nlines; li++) {
            line_ctx lc = {lo,
                           &pc,
                           ps,
                           &pl,
                           pi,
                           text,
                           tlen,
                           (const char *)ptext.data,
                           ptext.len,
                           pstart,
                           content_end,
                           prefix_len,
                           first_px,
                           is_box,
                           box,
                           line_origin,
                           avail,
                           indent_start,
                           vertical,
                           &has_prev,
                           &prev_baseline,
                           &pending_space,
                           &stop,
                           &ov,
                           tabs,
                           ntabs,
                           tab_px,
                           first_indent};
            st = pc_text_lay_line(&lc, li, err);
            if (st != PC_OK) {
                goto cleanup;
            }
            if (stop) {
                break;
            }
        }
        pending_space += ps->space_after_pt * k;
    }
cleanup:
    pl_layout_fini(&pl);
    pc_buf_fini(&ptext);
    pc_text_runs_free(runs, nruns);
    free(pruns);
    free(run_starts);
    free(para_starts);
    free(paras);
    free(small_caps);
    free(fallback);
    free(serif_fallback);
    free(craft);
    free(tabs);
    if (st != PC_OK) {
        pc_text_layout_fini(out);
        pc_text_layout_init(out);
    }
    return st;
}
