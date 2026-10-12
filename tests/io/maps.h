/* Helpers shared by test_maps.c and test_maps_parity.c (chunk I4): serde JSON in and out of the
 * doc model, base64, and the documents both the unit tests and rustref io-maps-vectors build.
 * Setup failures abort (test code only). */
#ifndef PC_TESTS_IO_MAPS_H
#define PC_TESTS_IO_MAPS_H

#include "common.h"

#include "doc/json.h"
#include "io/abr_map.h"
#include "io/annotations_map.h"
#include "io/comps_map.h"
#include "io/gradient_bake.h"
#include "io/pattern_map.h"
#include "io/slices_map.h"
#include "io/text_import.h"
#include "io/text_styles_map.h"
#include "io/vector_map.h"
#include "psd/misc.h"
#include "text/psd.h"

/* ---- JSON (serde_json::to_string / from_str) ---- */

#define JW(b, T, v)                                                                                          \
    do {                                                                                                     \
        yyjson_mut_doc *jw_d_ = yyjson_mut_doc_new(NULL);                                                    \
        yyjson_mut_val *jw_r_ = jw_d_ != NULL ? T##_to_json(jw_d_, (v), 0) : NULL;                           \
        if (jw_r_ == NULL) {                                                                                 \
            abort();                                                                                         \
        }                                                                                                    \
        must(pc_json_write(jw_r_, 0, (b), NULL));                                                            \
        yyjson_mut_doc_free(jw_d_);                                                                          \
    } while (0)

/* JSON of an optional value: "null" when absent. */
#define JW_OPT(b, T, has, v)                                                                                 \
    do {                                                                                                     \
        if (has) {                                                                                           \
            JW(b, T, v);                                                                                     \
        } else {                                                                                             \
            sb((b), "null");                                                                                 \
        }                                                                                                    \
    } while (0)

/* JSON array of n values. */
#define JW_ARR(b, T, v, n)                                                                                   \
    do {                                                                                                     \
        yyjson_mut_doc *jw_d_ = yyjson_mut_doc_new(NULL);                                                    \
        yyjson_mut_val *jw_a_ = jw_d_ != NULL ? yyjson_mut_arr(jw_d_) : NULL;                                \
        if (jw_a_ == NULL) {                                                                                 \
            abort();                                                                                         \
        }                                                                                                    \
        for (size_t jw_i_ = 0; jw_i_ < (n); jw_i_++) {                                                       \
            yyjson_mut_val *jw_e_ = T##_to_json(jw_d_, &(v)[jw_i_], 0);                                      \
            if (jw_e_ == NULL || !yyjson_mut_arr_append(jw_a_, jw_e_)) {                                     \
                abort();                                                                                     \
            }                                                                                                \
        }                                                                                                    \
        must(pc_json_write(jw_a_, 0, (b), NULL));                                                            \
        yyjson_mut_doc_free(jw_d_);                                                                          \
    } while (0)

static inline yyjson_doc *jparse(const char *s)
{
    yyjson_doc *d = NULL;
    must(pc_json_parse(s, strlen(s), &d, NULL));
    return d;
}

#define JR(T, text, out)                                                                                     \
    do {                                                                                                     \
        yyjson_doc *jr_d_ = jparse(text);                                                                    \
        must(T##_from_json(yyjson_doc_get_root(jr_d_), (out), NULL));                                        \
        yyjson_doc_free(jr_d_);                                                                              \
    } while (0)

/* Reads an optional value: false for JSON null. */
#define JR_OPT(T, text, out, has)                                                                            \
    do {                                                                                                     \
        yyjson_doc *jr_d_ = jparse(text);                                                                    \
        yyjson_val *jr_r_ = yyjson_doc_get_root(jr_d_);                                                      \
        *(has) = !yyjson_is_null(jr_r_);                                                                     \
        if (*(has)) {                                                                                        \
            must(T##_from_json(jr_r_, (out), NULL));                                                         \
        }                                                                                                    \
        yyjson_doc_free(jr_d_);                                                                              \
    } while (0)

static inline void sb64(pc_buf *b, const void *data, size_t len)
{
    pc_buf t = PC_BUF_INIT;
    must(pc_base64_encode(data, len, &t, NULL));
    must(pc_buf_append(b, t.data != NULL ? t.data : (const uint8_t *)"", t.len, NULL));
    pc_buf_fini(&t);
}

static inline void sh16(pc_buf *b, const void *data, size_t len)
{
    char h[17];
    h16(data, len, h);
    sb(b, "%s", h);
}

/* ---- documents ---- */

/* A 32 x 32 document with a white background and a raster layer "A" filled over (4,4)-(8,8);
 * its shmd holds a cust item and the cmls item when cmls is given. */
static inline pc_document m_comp_doc(const uint8_t *cmls, size_t len, pc_layer_id *lid)
{
    pc_document d;
    pc_layer l;
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    must(pc_document_with_background(&d, "t", pc_size_make(32, 32), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                     PC_COLOR_WHITE, NULL));
    must(pc_layer_raster(&l, "A", PC_PIXEL_FORMAT_RGBA8, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(4, 4, 8, 8), red, 4, NULL));
    if (cmls != NULL) {
        pc_psd_meta_item items[2];
        pc_psd_bytes shmd;
        static const uint8_t cust[2] = {1, 2};
        must(pc_psd_meta_item_make("cust", cust, 2, &items[0], NULL));
        must(pc_psd_meta_item_make("cmls", cmls, len, &items[1], NULL));
        must(pc_psd_write_shmd(items, 2, &shmd, NULL));
        pc_psd_bytes_fini(&items[0].data);
        pc_psd_bytes_fini(&items[1].data);
        l.psd_blocks = xmalloc(sizeof *l.psd_blocks);
        memcpy(l.psd_blocks[0].key, "shmd", 4);
        must(pc_blob_new(shmd.data, shmd.len, &l.psd_blocks[0].data, NULL));
        l.npsd_blocks = 1;
        pc_psd_bytes_fini(&shmd);
    }
    *lid = l.id;
    t_push(&d, &l);
    return d;
}

static inline void m_push_resource(pc_document *d, uint16_t id, const void *data, size_t len)
{
    pc_doc_psd_resource *v = realloc(d->metadata.psd_resources, (d->metadata.npsd_resources + 1) * sizeof *v);
    if (v == NULL) {
        abort();
    }
    d->metadata.psd_resources = v;
    memset(&v[d->metadata.npsd_resources], 0, sizeof *v);
    v[d->metadata.npsd_resources].id = id;
    must(pc_blob_new(data, len, &v[d->metadata.npsd_resources].data, NULL));
    d->metadata.npsd_resources++;
}

static inline void m_push_global(pc_document *d, const char key[4], const void *data, size_t len)
{
    pc_doc_psd_global_block *v =
        realloc(d->metadata.psd_global_blocks, (d->metadata.npsd_global_blocks + 1) * sizeof *v);
    if (v == NULL) {
        abort();
    }
    d->metadata.psd_global_blocks = v;
    memcpy(v[d->metadata.npsd_global_blocks].signature, "8BIM", 4);
    memcpy(v[d->metadata.npsd_global_blocks].key, key, 4);
    must(pc_blob_new(data, len, &v[d->metadata.npsd_global_blocks].data, NULL));
    d->metadata.npsd_global_blocks++;
}

static inline void m_global_blocks_free(pc_doc_psd_global_block *b, size_t n)
{
    for (size_t i = 0; b != NULL && i < n; i++) {
        pc_blob_release(b[i].data);
    }
    free(b);
}

/* pattern(mode, s, alpha) of pattern_map.rs's tests: 5 x 3, values ((x*3 + y*7 + c*5) % 11) / 10. */
static inline pc_pattern m_pattern(pc_color_mode mode, pc_sample_type s, bool alpha)
{
    pc_pixel_format fmt = {mode, s, alpha};
    size_t n = pc_pixel_format_channels(fmt);
    pc_surface surf;
    pc_pattern p;
    pc_surface_init(&surf, fmt);
    for (int32_t y = 0; y < 3; y++) {
        for (int32_t x = 0; x < 5; x++) {
            float px[5];
            for (size_t c = 0; c < n; c++) {
                px[c] = (float)((x * 3 + y * 7 + (int32_t)c * 5) % 11) / 10.0f;
            }
            must(pc_surface_write_pixel(&surf, x, y, px, n, NULL));
        }
    }
    must(pc_pattern_new("P", 1, &surf, 5, 3, &p, NULL));
    return p;
}

/* "{:?}|{:?}|WxH|mode/sample/alpha:hash" of a pattern (rustref pat_s). */
static inline void m_surf_s(pc_buf *b, const pc_surface *s, pc_rect r)
{
    pc_pixel_format f = pc_surface_format(s);
    uint8_t *px = NULL;
    size_t n = 0;
    if (!pc_rect_is_empty(r)) {
        must(pc_surface_to_interleaved(s, r, &px, &n, NULL));
    }
    sb(b, "%s/%s/%d:", pc_color_mode_name(f.mode), pc_sample_type_name(f.sample), f.alpha ? 1 : 0);
    sh16(b, px != NULL ? px : (const uint8_t *)"", n);
    free(px);
}

static inline void m_pat_s(pc_buf *b, const pc_pattern *p)
{
    sq(b, pc_str_c(&p->id), p->id.len);
    sb(b, "|");
    sq(b, pc_str_c(&p->name), p->name.len);
    sb(b, "|%ux%u|", p->width, p->height);
    m_surf_s(b, &p->surface, pc_pattern_rect(p));
}

/* The character and paragraph styles of text_styles.rs's tests (plus a second character style). */
static inline pc_text_styles m_styles(void)
{
    pc_text_styles s;
    pc_char_style c;
    pc_char_style base;
    pc_paragraph_style p = pc_paragraph_style_default();
    pc_paragraph_style pbase = pc_paragraph_style_default();
    must(pc_text_styles_default(&s, NULL));
    s.character = xmalloc(2 * sizeof *s.character);
    s.paragraph = xmalloc(sizeof *s.paragraph);
    memset(s.character, 0, 2 * sizeof *s.character);
    memset(s.paragraph, 0, sizeof *s.paragraph);
    pc_char_style_default(&base);
    pc_char_style_default(&c);
    c.size_pt = 30.0f;
    c.underline = true;
    s.character[0].id = 3;
    must(pc_str_set_c(&s.character[0].name, "Loud", NULL));
    must(pc_char_style_diff_attrs(&c, &base, &s.character[0].attrs, NULL));
    pc_char_style_default(&c);
    c.size_pt = 8.0f;
    c.italic = true;
    s.character[1].id = 4;
    must(pc_str_set_c(&s.character[1].name, "Quiet \xC3\xA9", NULL));
    must(pc_char_style_diff_attrs(&c, &base, &s.character[1].attrs, NULL));
    s.ncharacter = 2;
    p.align = PC_TEXT_ALIGN_CENTER;
    s.paragraph[0].id = 1;
    must(pc_str_set_c(&s.paragraph[0].name, "Middle", NULL));
    must(pc_paragraph_style_diff_attrs(&p, &pbase, &s.paragraph[0].para_attrs, NULL));
    s.nparagraph = 1;
    return s;
}

/* "calm LOUD\nquiet": three runs (plain, Loud, Quiet in Middle) and two paragraphs. */
static inline pc_text_layer m_text_layer(const pc_text_styles *st, bool link)
{
    pc_text_layer t;
    pc_char_style base;
    static const uint32_t three = 3;
    static const uint32_t four = 4;
    static const uint32_t one = 1;
    pc_text_layer_default(&t);
    pc_char_style_default(&base);
    must(pc_str_set_c(&base.font_family, "Inter", NULL));
    must(pc_str_set_c(&t.text, "calm LOUD\nquiet", NULL));
    t.runs = xmalloc(3 * sizeof *t.runs);
    t.nruns = 3;
    t.runs[0].len = 5;
    t.runs[1].len = 5;
    t.runs[2].len = 5;
    must(pc_char_style_clone(&base, &t.runs[0].style, NULL));
    if (link) {
        must(pc_text_styles_resolve_char(st, NULL, &three, "Inter", &t.runs[1].style, NULL));
        must(pc_text_styles_resolve_char(st, &one, &four, "Inter", &t.runs[2].style, NULL));
    } else {
        must(pc_char_style_clone(&base, &t.runs[1].style, NULL));
        must(pc_char_style_clone(&base, &t.runs[2].style, NULL));
    }
    pc_char_style_fini(&base);
    t.paragraphs = xmalloc(2 * sizeof *t.paragraphs);
    t.nparagraphs = 2;
    t.paragraphs[0].len = 10;
    t.paragraphs[1].len = 5;
    t.paragraphs[1].style = pc_paragraph_style_default();
    if (link) {
        must(pc_text_styles_resolve_para(st, &one, &t.paragraphs[0].style, NULL));
    } else {
        t.paragraphs[0].style = pc_paragraph_style_default();
    }
    t.transform = pc_affine_translate(4.0, 40.0);
    must(pc_text_layer_sync_summary(&t, NULL));
    return t;
}

static inline pc_layer m_text(const char *name, pc_text_layer *t)
{
    pc_layer_content c;
    pc_layer l;
    c.kind = PC_LAYER_TEXT;
    c.u.text = *t;
    must(pc_layer_init(&l, name, &c, NULL));
    return l;
}

/* "a,b,c/p,q": each run's and paragraph's style sheet ("-" for none). */
static inline void m_run_refs(pc_buf *b, const pc_text_layer *t)
{
    for (size_t i = 0; i < t->nruns; i++) {
        const pc_char_style *s = &t->runs[i].style;
        sb(b, i > 0 ? "," : "");
        if (s->has_style_sheet) {
            sb(b, "%u", s->style_sheet);
        } else {
            sb(b, "-");
        }
    }
    sb(b, "/");
    for (size_t i = 0; i < t->nparagraphs; i++) {
        const pc_paragraph_style *s = &t->paragraphs[i].style;
        sb(b, i > 0 ? "," : "");
        if (s->has_style_sheet) {
            sb(b, "%u", s->style_sheet);
        } else {
            sb(b, "-");
        }
    }
}

#endif
