/* PSD import through whole files: every test of crates/io/tests/psd_structure.rs (24: testgen /
 * builder files -> document -> PSD structural equality, import details, flattened and
 * Multichannel modes, nesting caps, the skipped merged composite, locks, labels, adjustment /
 * fill / text / smart / shape detection, fill caches, guides, resolution units, unmatting,
 * vector-rendered masks, blend keys, bare rectangles), plus never-crash checks of
 * pc_io_psd_to_document_with: cancellation at every check point, deep and hostile files. */
#include "harness.h"
#include "common.h"
#include "docs.h"
#include "../psd/testgen.h"

#include "base/num.h"
#include "io/blocks.h"
#include "psd/builder.h"
#include "psd/misc.h"
#include "psd/pixels.h"

/* ---- helpers ---- */

static pc_psd_bytes file_bytes(const pc_psd_file *f)
{
    pc_psd_bytes b = {NULL, 0};
    must(pc_psd_file_to_bytes(f, &b, NULL));
    return b;
}

static pc_io_import_result import_bytes(const char *name, const uint8_t *p, size_t n)
{
    pc_io_import_result r;
    must(pc_io_import(name, p, n, &r, NULL));
    return r;
}

static pc_psd_file reparse(const uint8_t *p, size_t n)
{
    pc_psd_file f;
    must(pc_psd_file_parse(p, n, &f, NULL));
    return f;
}

/* psd_to_document of a model (its written bytes re-parsed, as the reference does with files). */
static pc_document to_doc(const pc_psd_file *f, pc_io_warnings *w)
{
    pc_document d;
    pc_io_warnings_init(w);
    must(pc_io_psd_to_document(f, &d, w, NULL));
    return d;
}

static pc_psd_file via_doc(const pc_psd_file *f)
{
    pc_psd_bytes b = file_bytes(f);
    pc_io_import_result r = import_bytes("t.psd", b.data, b.len);
    pc_io_export_result out;
    pc_psd_file g;
    must(pc_io_export(&r.document, "t.psd", NULL, &out, NULL));
    g = reparse(out.bytes, out.len);
    pc_io_export_result_fini(&out);
    pc_io_import_result_fini(&r);
    pc_psd_bytes_fini(&b);
    return g;
}

static char *rec_name(const pc_psd_layer *l)
{
    char *n = NULL;
    must(pc_psd_layer_name(l, &n, NULL));
    return n;
}

static void tree_shape(const pc_psd_file *f, const pc_psd_node *nodes, size_t n, pc_buf *out)
{
    size_t nl = 0;
    const pc_psd_layer *ls = pc_psd_file_layers(f, &nl);
    for (size_t i = 0; i < n; i++) {
        char *name = rec_name(&ls[nodes[i].index]);
        if (i > 0) {
            sb(out, ",");
        }
        sb(out, "%s%s", name,
           pc_psd_layer_section_type(&ls[nodes[i].index]) == PC_PSD_SECTION_CLOSED ? "(c)" : "");
        free(name);
        if (nodes[i].is_group) {
            sb(out, "[");
            tree_shape(f, nodes[i].children, nodes[i].n, out);
            sb(out, "]");
        }
    }
}

static void leaves(const pc_psd_node *nodes, size_t n, size_t *out, size_t *k)
{
    for (size_t i = 0; i < n; i++) {
        out[(*k)++] = nodes[i].index;
        leaves(nodes[i].children, nodes[i].n, out, k);
    }
}

/* A channel's sample at document (x, y): 0 outside its rect (the mask default for -2). */
static void sample(const pc_psd_file *f, size_t li, int16_t id, int32_t x, int32_t y,
                   const pc_psd_bytes *plane, uint8_t out[4], size_t *bps)
{
    size_t nl = 0;
    const pc_psd_layer *rec = &pc_psd_file_layers(f, &nl)[li];
    pc_psd_rect r = pc_psd_layer_channel_rect(rec, id);
    *bps = f->header.depth / 8u;
    memset(out, 0, 4);
    if (x < r.left || x >= r.right || y < r.top || y >= r.bottom || pc_psd_layer_channel(rec, id) == NULL) {
        const pc_psd_layer_mask *m = pc_psd_layer_get_mask(rec);
        uint8_t def = id == -2 && m != NULL ? m->default_color : 0;
        if (def != 0 && *bps == 1) {
            out[0] = def;
        }
        return;
    }
    {
        size_t w = (size_t)(r.right - r.left);
        size_t i = ((size_t)(y - r.top) * w + (size_t)(x - r.left)) * *bps;
        if (i + *bps <= plane->len) {
            memcpy(out, plane->data + i, *bps);
        }
    }
}

static pc_psd_rect rect_union(pc_psd_rect a, pc_psd_rect b)
{
    pc_psd_rect r = {a.top < b.top ? a.top : b.top, a.left < b.left ? a.left : b.left,
                     a.bottom > b.bottom ? a.bottom : b.bottom, a.right > b.right ? a.right : b.right};
    return r;
}

static void assert_structurally_equal(const pc_psd_file *a, const pc_psd_file *b)
{
    pc_psd_node *ta = NULL;
    pc_psd_node *tb = NULL;
    size_t nta = 0;
    size_t ntb = 0;
    pc_buf sa = PC_BUF_INIT;
    pc_buf sb2 = PC_BUF_INIT;
    size_t na = 0;
    size_t nb = 0;
    const pc_psd_layer *la = pc_psd_file_layers(a, &na);
    const pc_psd_layer *lb = pc_psd_file_layers(b, &nb);
    size_t *ia = xmalloc((na + 1) * sizeof *ia);
    size_t *ib = xmalloc((nb + 1) * sizeof *ib);
    size_t ka = 0;
    size_t kb = 0;
    int16_t cc = (int16_t)tg_mode_channels(a->header.color_mode);
    CHECK_EQ_UINT(a->header.width, b->header.width);
    CHECK_EQ_UINT(a->header.height, b->header.height);
    CHECK_EQ_UINT(a->header.depth, b->header.depth);
    CHECK_EQ_UINT(a->header.color_mode, b->header.color_mode);
    must(pc_psd_layer_tree(a, &ta, &nta, NULL));
    must(pc_psd_layer_tree(b, &tb, &ntb, NULL));
    tree_shape(a, ta, nta, &sa);
    tree_shape(b, tb, ntb, &sb2);
    CHECK_EQ_STR(pc_buf_cstr(&sa), pc_buf_cstr(&sb2));
    leaves(ta, nta, ia, &ka);
    leaves(tb, ntb, ib, &kb);
    for (size_t k = 0; k < ka && k < kb; k++) {
        const pc_psd_layer *ra = &la[ia[k]];
        const pc_psd_layer *rb = &lb[ib[k]];
        char *n = rec_name(ra);
        char *m = rec_name(rb);
        uint32_t st = pc_psd_layer_section_type(ra);
        bool folder = st == PC_PSD_SECTION_OPEN || st == PC_PSD_SECTION_CLOSED;
        const pc_psd_block *fa = pc_psd_layer_block(ra, "lfx2");
        const pc_psd_block *fb2 = pc_psd_layer_block(rb, "lfx2");
        CHECK_EQ_STR(n, m);
        if (folder) {
            pc_psd_section_divider da;
            pc_psd_section_divider db;
            bool ha = pc_psd_layer_section_divider(ra, &da) && da.has_blend;
            bool hb = pc_psd_layer_section_divider(rb, &db) && db.has_blend;
            CHECK(ha == hb && (!ha || memcmp(da.blend, db.blend, 4) == 0));
        } else {
            const uint8_t *blend = pc_psd_blend_index(ra->blend) < 0 ? (const uint8_t *)"norm" : ra->blend;
            if (memcmp(blend, rb->blend, 4) != 0) {
                PC_T_FAIL("%s: blend", n);
            }
        }
        CHECK_EQ_UINT(ra->opacity, rb->opacity);
        CHECK_EQ_UINT(pc_psd_layer_fill_opacity(ra), pc_psd_layer_fill_opacity(rb));
        CHECK(pc_psd_layer_is_visible(ra) == pc_psd_layer_is_visible(rb));
        CHECK_EQ_UINT(ra->clipping, rb->clipping);
        CHECK((fa == NULL) == (fb2 == NULL) && (fa == NULL || pc_psd_bytes_eq(&fa->data, &fb2->data)));
        if (!folder) {
            int16_t ids[8];
            size_t nid = 0;
            const pc_psd_layer_mask *mask = pc_psd_layer_get_mask(ra);
            for (int16_t id = -1; id < cc; id++) {
                ids[nid++] = id;
            }
            if (mask != NULL &&
                (pc_psd_layer_channel(ra, -2) != NULL || pc_psd_layer_channel(ra, -3) != NULL)) {
                ids[nid++] = -2;
            }
            for (size_t c = 0; c < nid; c++) {
                int16_t id = ids[c];
                /* The exporter writes the real mask (-3) as the user mask (-2). */
                int16_t src =
                    id == -2 && pc_psd_layer_channel(ra, -3) != NULL && mask != NULL && mask->has_real
                        ? (int16_t)-3
                        : id;
                pc_psd_rect u =
                    rect_union(pc_psd_layer_channel_rect(ra, src), pc_psd_layer_channel_rect(rb, id));
                pc_psd_bytes pa = {NULL, 0};
                pc_psd_bytes pb = {NULL, 0};
                bool same = true;
                if (pc_psd_layer_channel(ra, src) != NULL) {
                    (void)pc_psd_layer_decode_channel(ra, src, a->header.depth, a->header.version, &pa, NULL);
                }
                if (pc_psd_layer_channel(rb, id) != NULL) {
                    (void)pc_psd_layer_decode_channel(rb, id, b->header.depth, b->header.version, &pb, NULL);
                }
                for (int32_t y = u.top; y < u.bottom && same; y++) {
                    for (int32_t x = u.left; x < u.right && same; x++) {
                        uint8_t va[4];
                        uint8_t vb[4];
                        size_t bps;
                        sample(a, ia[k], src, x, y, &pa, va, &bps);
                        sample(b, ib[k], id, x, y, &pb, vb, &bps);
                        if (memcmp(va, vb, bps) != 0) {
                            PC_T_FAIL("%s: channel %d at (%d,%d)", n, (int)id, (int)x, (int)y);
                            same = false;
                        }
                    }
                }
                pc_psd_bytes_fini(&pa);
                pc_psd_bytes_fini(&pb);
            }
        }
        free(n);
        free(m);
    }
    CHECK_EQ_UINT(ka, kb);
    pc_psd_nodes_free(ta, nta);
    pc_psd_nodes_free(tb, ntb);
    pc_buf_fini(&sa);
    pc_buf_fini(&sb2);
    free(ia);
    free(ib);
}

static void tg_roundtrip(pc_psd_version v, uint16_t mode, uint16_t depth, uint16_t c)
{
    pc_psd_file f = tg_layered(v, mode, depth, c);
    pc_psd_bytes b = file_bytes(&f);
    pc_psd_file f2 = reparse(b.data, b.len);
    pc_psd_file g = via_doc(&f2);
    assert_structurally_equal(&f2, &g);
    pc_psd_file_fini(&f);
    pc_psd_file_fini(&f2);
    pc_psd_file_fini(&g);
    pc_psd_bytes_fini(&b);
}

/* ---- psd_structure.rs ---- */

static void test_testgen_layered(void)
{
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_RGB, 16, PC_PSD_ZIP);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_RGB, 32, PC_PSD_ZIP_PREDICT);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_GRAYSCALE, 8, PC_PSD_ZIP_PREDICT);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_GRAYSCALE, 16, PC_PSD_RLE);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_GRAYSCALE, 32, PC_PSD_RAW);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_CMYK, 8, PC_PSD_RLE);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_CMYK, 16, PC_PSD_ZIP);
    tg_roundtrip(PC_PSD_V1, PC_PSD_MODE_LAB, 8, PC_PSD_RLE);
    tg_roundtrip(PC_PSD_V2, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    tg_roundtrip(PC_PSD_V2, PC_PSD_MODE_CMYK, 16, PC_PSD_ZIP_PREDICT);
}

static void test_testgen_small_all_compressions(void)
{
    for (int v = 0; v < 2; v++) {
        for (uint16_t c = 0; c < 4; c++) {
            pc_psd_file f = tg_small(v == 0 ? PC_PSD_V1 : PC_PSD_V2, c);
            pc_psd_bytes b = file_bytes(&f);
            pc_psd_file f2 = reparse(b.data, b.len);
            pc_psd_file g = via_doc(&f2);
            assert_structurally_equal(&f2, &g);
            pc_psd_file_fini(&f);
            pc_psd_file_fini(&f2);
            pc_psd_file_fini(&g);
            pc_psd_bytes_fini(&b);
        }
    }
}

static const pc_layer *find_layer(const pc_layer *v, size_t n, const char *prefix)
{
    for (size_t i = 0; i < n; i++) {
        if (strncmp(pc_str_c(&v[i].name), prefix, strlen(prefix)) == 0) {
            return &v[i];
        }
    }
    return NULL;
}

static void test_testgen_import_details(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    pc_io_warnings w;
    pc_document d = to_doc(&f, &w);
    const pc_layer *outer = find_layer(d.layers, d.nlayers, "Outer");
    const pc_layer *neg = find_layer(d.layers, d.nlayers, "Neg offset");
    pc_walk walk;
    bool fx = false;
    bool path = false;
    bool patt = false;
    /* Unknown blend mode warned about. */
    CHECK(pc_io_warnings_any(&w, "unknown blend mode"));
    /* Groups: Outer (open) containing Inner (closed). */
    CHECK(outer != NULL && outer->content.kind == PC_LAYER_GROUP);
    if (outer != NULL && outer->content.kind == PC_LAYER_GROUP) {
        const pc_layer *inner =
            find_layer(outer->content.u.group.children, outer->content.u.group.nchildren, "Inner");
        CHECK(outer->content.u.group.expanded && outer->blend == PC_BLEND_PASS_THROUGH);
        CHECK(inner != NULL && inner->content.kind == PC_LAYER_GROUP && !inner->content.u.group.expanded);
    }
    /* Negative offsets preserved. */
    CHECK(neg != NULL && neg->has_mask);
    if (neg != NULL) {
        pc_rect b = pc_surface_content_bounds(pc_layer_surface(neg));
        CHECK(b.x0 < 0 && b.y0 < 0);
    }
    CHECK(d.icc_profile != NULL);
    CHECK(fabsf(d.resolution_dpi - 72.0f) < 1e-3f);
    CHECK(d.metadata.has_xmp);
    /* Named saved-path resource (2000) becomes a document path, raw bytes kept for export. */
    for (size_t i = 0; i < d.npaths; i++) {
        path = path || (pc_str_eq_c(&d.paths[i].name, "Path 1") && pc_blob_len(d.paths[i].psd_raw) == 3 &&
                        memcmp(pc_blob_data(d.paths[i].psd_raw), "\1\2\3", 3) == 0);
    }
    CHECK(path);
    for (size_t i = 0; i < d.metadata.npsd_global_blocks; i++) {
        patt = patt || memcmp(d.metadata.psd_global_blocks[i].key, "Patt", 4) == 0;
    }
    CHECK(patt);
    must(pc_document_walk(&d, &walk, NULL));
    for (size_t i = 0; i < walk.len; i++) {
        fx = fx || walk.items[i].layer->effects.psd_raw != NULL;
    }
    CHECK(fx);
    pc_walk_fini(&walk);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_fallback_modes_import_flattened(void)
{
    static const uint16_t modes[3] = {PC_PSD_MODE_INDEXED, PC_PSD_MODE_BITMAP, PC_PSD_MODE_DUOTONE};
    for (size_t i = 0; i < 3; i++) {
        uint16_t depths[3];
        pc_psd_file f;
        pc_io_warnings w;
        pc_document d;
        pc_io_export_result out;
        pc_psd_file back;
        (void)tg_mode_depths(modes[i], depths);
        f = tg_merged_only(PC_PSD_V1, modes[i], depths[0], PC_PSD_RLE, 9, 5);
        d = to_doc(&f, &w);
        CHECK_EQ_UINT(d.nlayers, 1);
        CHECK(w.n > 0);
        CHECK(d.depth == PC_SAMPLE_TYPE_U8);
        /* Re-export produces a valid PSD. */
        must(pc_io_export(&d, "x.psd", NULL, &out, NULL));
        CHECK(pc_psd_file_parse(out.bytes, out.len, &back, NULL) == PC_OK);
        pc_psd_file_fini(&back);
        pc_io_export_result_fini(&out);
        pc_document_fini(&d);
        pc_io_warnings_fini(&w);
        pc_psd_file_fini(&f);
    }
}

static void test_multichannel_imports_ink_channels(void)
{
    uint16_t depths[3];
    size_t nd = tg_mode_depths(PC_PSD_MODE_MULTICHANNEL, depths);
    for (size_t i = 0; i < nd; i++) {
        pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_MULTICHANNEL, depths[i], PC_PSD_RLE, 9, 5);
        pc_io_warnings w;
        pc_document d = to_doc(&f, &w);
        pc_psd_bytes merged = {NULL, 0};
        pc_psd_bytes m2 = {NULL, 0};
        pc_io_export_result out;
        pc_psd_file back;
        float first;
        float px[PC_RASTER_MAX_CHANNELS];
        CHECK(d.mode == PC_COLOR_MODE_MULTICHANNEL && d.nlayers == 0);
        CHECK_EQ_UINT(d.nchannels, f.header.channels);
        for (size_t k = 0; k < d.nchannels; k++) {
            CHECK(d.channels[k].has_spot);
        }
        /* Stored dark = ink: the channel value is 1 - the stored sample. */
        must(pc_psd_file_decode_merged(&f, &merged, NULL));
        first = depths[i] == 8 ? (float)merged.data[0] / 255.0f
                               : (float)(merged.data[0] << 8 | merged.data[1]) / 65535.0f;
        pc_surface_read_pixel(&d.channels[0].surface, 0, 0, px);
        CHECK(fabsf(px[0] - (1.0f - first)) < 1e-3f);
        /* Re-export writes the same planes. */
        must(pc_io_export(&d, "x.psd", NULL, &out, NULL));
        back = reparse(out.bytes, out.len);
        CHECK_EQ_UINT(back.header.color_mode, PC_PSD_MODE_MULTICHANNEL);
        must(pc_psd_file_decode_merged(&back, &m2, NULL));
        CHECK(pc_psd_bytes_eq(&m2, &merged));
        pc_psd_bytes_fini(&merged);
        pc_psd_bytes_fini(&m2);
        pc_psd_file_fini(&back);
        pc_io_export_result_fini(&out);
        pc_document_fini(&d);
        pc_io_warnings_fini(&w);
        pc_psd_file_fini(&f);
    }
}

static void test_indexed_colors_match_palette(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_INDEXED, 8, PC_PSD_RAW, 4, 2);
    pc_io_warnings w;
    pc_document d = to_doc(&f, &w);
    pc_psd_image8 rgba;
    must(pc_psd_composite_rgba8(&f, &rgba, NULL));
    for (int32_t y = 0; y < 2; y++) {
        for (int32_t x = 0; x < 4; x++) {
            float p[PC_RASTER_MAX_CHANNELS];
            size_t i = (size_t)((y * 4 + x) * 4);
            pc_surface_read_pixel(pc_layer_surface(&d.layers[0]), x, y, p);
            for (size_t c = 0; c < 3; c++) {
                CHECK_EQ_UINT(pc_f32_to_u8(roundf(p[c] * 255.0f)), rgba.data.data[i + c]);
            }
        }
    }
    pc_psd_image8_fini(&rgba);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_flattened_psd_becomes_background(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 16, PC_PSD_ZIP_PREDICT, 7, 3);
    pc_io_warnings w;
    pc_document d = to_doc(&f, &w);
    pc_psd_bytes merged = {NULL, 0};
    float p[PC_RASTER_MAX_CHANNELS];
    CHECK_EQ_UINT(d.nlayers, 1);
    CHECK(pc_str_eq_c(&d.layers[0].name, "Background"));
    CHECK(d.depth == PC_SAMPLE_TYPE_U16);
    must(pc_psd_file_decode_merged(&f, &merged, NULL));
    pc_surface_read_pixel(pc_layer_surface(&d.layers[0]), 0, 0, p);
    CHECK_EQ_UINT(pc_f32_to_u16(roundf(p[0] * 65535.0f)), (unsigned)(merged.data[0] << 8 | merged.data[1]));
    pc_psd_bytes_fini(&merged);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_builder_psd_imports(void)
{
    static const uint8_t px[16] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 9, 9, 9, 9};
    static const uint8_t px2[4] = {1, 2, 3, 4};
    static const uint8_t md[1] = {100};
    pc_psd_builder *b = pc_psd_builder_new(4, 4);
    pc_psd_layer_spec s = pc_psd_layer_spec_make("a", 0, 0, 2, 2, (pc_psd_pixels){PC_PSD_PX_RGBA8, px, 16});
    pc_psd_mask_spec m = {pc_psd_rect_from_xywh(0, 0, 1, 1), md, 1, 255, true};
    pc_psd_group_spec g = pc_psd_group_spec_make("G");
    pc_psd_layer_spec s2 = pc_psd_layer_spec_make("b", 1, 1, 1, 1, (pc_psd_pixels){PC_PSD_PX_RGBA8, px2, 4});
    pc_psd_bytes bytes = {NULL, 0};
    pc_io_import_result r;
    s.mask = &m;
    s.has_fill_opacity = true;
    s.fill_opacity = 77;
    g.open = false;
    must(pc_psd_builder_push_layer(b, &s, NULL));
    must(pc_psd_builder_begin_group(b, &g, NULL));
    must(pc_psd_builder_push_layer(b, &s2, NULL));
    must(pc_psd_builder_end_group(b, NULL));
    must(pc_psd_builder_to_bytes(b, &bytes, NULL));
    r = import_bytes("b.psd", bytes.data, bytes.len);
    CHECK_EQ_UINT(r.document.nlayers, 2);
    if (r.document.nlayers == 2) {
        const pc_layer *a = &r.document.layers[0];
        float p[PC_RASTER_MAX_CHANNELS];
        CHECK(a->fill_opacity == 77.0f / 255.0f);
        CHECK(a->has_mask && !a->mask.enabled);
        pc_surface_read_pixel(&a->mask.surface, 0, 0, p);
        CHECK(p[0] == 100.0f / 255.0f);
        pc_surface_read_pixel(&a->mask.surface, 3, 3, p);
        CHECK(p[0] == 1.0f);
        pc_surface_read_pixel(pc_layer_surface(a), 1, 0, p);
        CHECK(p[0] == 0.0f && p[1] == 1.0f && p[2] == 0.0f && p[3] == 128.0f / 255.0f);
        CHECK(r.document.layers[1].content.kind == PC_LAYER_GROUP &&
              !r.document.layers[1].content.u.group.expanded &&
              r.document.layers[1].content.u.group.nchildren == 1);
    }
    pc_io_import_result_fini(&r);
    pc_psd_bytes_fini(&bytes);
    pc_psd_builder_free(b);
}

static pc_psd_bytes nested(int levels)
{
    static const uint8_t px[4] = {1, 2, 3, 4};
    pc_psd_builder *b = pc_psd_builder_new(4, 4);
    pc_psd_group_spec g = pc_psd_group_spec_make("g");
    pc_psd_layer_spec s = pc_psd_layer_spec_make("deep", 0, 0, 1, 1, (pc_psd_pixels){PC_PSD_PX_RGBA8, px, 4});
    pc_psd_bytes bytes = {NULL, 0};
    for (int i = 0; i < levels; i++) {
        must(pc_psd_builder_begin_group(b, &g, NULL));
    }
    must(pc_psd_builder_push_layer(b, &s, NULL));
    for (int i = 0; i < levels; i++) {
        must(pc_psd_builder_end_group(b, NULL));
    }
    must(pc_psd_builder_to_bytes(b, &bytes, NULL));
    pc_psd_builder_free(b);
    return bytes;
}

static void test_group_nesting_past_the_cap_is_rejected(void)
{
    pc_psd_bytes bytes = nested(101);
    pc_io_import_result r;
    pc_error e;
    pc_psd_file f;
    pc_io_warnings w;
    pc_document d;
    size_t depth = 0;
    CHECK(pc_io_import("deep.psd", bytes.data, bytes.len, &r, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "nested deeper than 100");
    /* The never-fail converter still returns a document, capped, with a warning. */
    f = reparse(bytes.data, bytes.len);
    d = to_doc(&f, &w);
    must(pc_document_max_group_depth(&d, &depth, NULL));
    CHECK_EQ_UINT(depth, 100);
    CHECK(pc_io_warnings_any(&w, "deeper than 100"));
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
    pc_psd_bytes_fini(&bytes);
}

static void test_group_nesting_at_the_cap_imports(void)
{
    pc_psd_bytes bytes = nested(100);
    pc_io_import_result r = import_bytes("hundred.psd", bytes.data, bytes.len);
    size_t depth = 0;
    must(pc_document_max_group_depth(&r.document, &depth, NULL));
    CHECK_EQ_UINT(depth, 100);
    CHECK(!pc_io_warnings_any(&r.warnings, "deeper"));
    pc_io_import_result_fini(&r);
    pc_psd_bytes_fini(&bytes);
}

static void test_layered_import_skips_the_unused_merged_composite(void)
{
    uint8_t px[16];
    uint8_t comp[64];
    pc_psd_builder *b = pc_psd_builder_new(4, 4);
    pc_psd_layer_spec s;
    pc_psd_bytes bytes = {NULL, 0};
    pc_psd_file f;
    pc_io_warnings w;
    pc_document d;
    memset(px, 7, sizeof px);
    memset(comp, 128, sizeof comp);
    s = pc_psd_layer_spec_make("a", 0, 0, 2, 2, (pc_psd_pixels){PC_PSD_PX_RGBA8, px, 16});
    must(pc_psd_builder_push_layer(b, &s, NULL));
    must(pc_psd_builder_composite(b, (pc_psd_pixels){PC_PSD_PX_RGBA8, comp, 64}, NULL));
    must(pc_psd_builder_to_bytes(b, &bytes, NULL));
    f = reparse(bytes.data, bytes.len);
    pc_psd_bytes_fini(&f.image_data);
    f.layer_info.merged_alpha = true;
    d = to_doc(&f, &w);
    CHECK_EQ_UINT(d.nlayers, 1);
    CHECK(!pc_io_warnings_any(&w, "merged image"));
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    /* A real extra channel behind the colour ones is decoded, and the garbage warns. */
    f.layer_info.merged_alpha = false;
    d = to_doc(&f, &w);
    CHECK(pc_io_warnings_any(&w, "merged image"));
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
    pc_psd_bytes_fini(&bytes);
    pc_psd_builder_free(b);
}

static void push_block(pc_psd_layer *l, pc_status st, pc_psd_block *b)
{
    must(st);
    must(pc_psd_layer_push_block(l, b, NULL));
    pc_psd_block_fini(b);
}

static void test_locks_and_labels_from_psd(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_block b;
    pc_io_warnings w;
    pc_document d;
    push_block(&f.layer_info.layers[0], pc_psd_block_make_u32("lspf", 5, &b, NULL), &b);
    push_block(&f.layer_info.layers[0], pc_psd_block_make_sheet_color(3, &b, NULL), &b);
    d = to_doc(&f, &w);
    CHECK(d.layers[0].locks.transparency && d.layers[0].locks.position && !d.layers[0].locks.pixels);
    CHECK(d.layers[0].label == PC_LABEL_YELLOW);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_artboard_lock_round_trips_byte_for_byte(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_block b;
    pc_psd_file g;
    size_t n = 0;
    const pc_psd_layer *ls;
    const pc_psd_block *lspf = NULL;
    push_block(&f.layer_info.layers[0], pc_psd_block_make_u32("lspf", 0x0D, &b, NULL), &b);
    g = via_doc(&f);
    ls = pc_psd_file_layers(&g, &n);
    for (size_t i = 0; i < n && lspf == NULL; i++) {
        lspf = pc_psd_layer_block(&ls[i], "lspf");
    }
    CHECK(lspf != NULL && lspf->data.len == 4 && memcmp(lspf->data.data, "\0\0\0\x0d", 4) == 0);
    pc_psd_file_fini(&g);
    pc_psd_file_fini(&f);
}

static pc_psd_bytes soco_red(void)
{
    pc_psd_desc d;
    pc_psd_desc c;
    pc_psd_bytes b = {NULL, 0};
    must(pc_psd_desc_init(&d, "null", NULL));
    must(pc_psd_desc_init(&c, "RGBC", NULL));
    tg_add(&c, "Rd  ", pc_psd_value_double(255.0));
    tg_add(&c, "Grn ", pc_psd_value_double(0.0));
    tg_add(&c, "Bl  ", pc_psd_value_double(0.0));
    tg_add(&d, "Clr ", tg_obj(&c));
    must(pc_psd_vdesc_to_bytes(&d, &b, NULL));
    pc_psd_desc_fini(&d);
    return b;
}

/* A record named name with one extra block (key, data). */
static void push_record(pc_psd_file *f, const char *name, const char *key, const void *data, size_t len)
{
    pc_psd_layer l;
    pc_psd_block b;
    pc_psd_layer_init(&l);
    must(pc_psd_bytes_set(&l.name, name, strlen(name), NULL));
    push_block(&l, pc_psd_block_make_unicode_name(name, &b, NULL), &b);
    push_block(&l, pc_psd_block_init(&b, key, data, len, NULL), &b);
    must(pc_psd_layer_info_push(&f->layer_info, &l, NULL));
}

static void test_adjustment_and_fill_layers_from_psd(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes soco = soco_red();
    pc_io_warnings w;
    pc_document d;
    size_t n;
    push_record(&f, "inv", "nvrt", NULL, 0);
    push_record(&f, "post", "post", (const uint8_t[4]){0, 4, 0, 0}, 4);
    push_record(&f, "sel", "selc", (const uint8_t[3]){1, 2, 3}, 3);
    push_record(&f, "red", "SoCo", soco.data, soco.len);
    d = to_doc(&f, &w);
    n = d.nlayers;
    CHECK(d.layers[n - 4].content.kind == PC_LAYER_ADJUSTMENT &&
          d.layers[n - 4].content.u.adjustment.kind == PC_ADJ_INVERT);
    CHECK(d.layers[n - 3].content.kind == PC_LAYER_ADJUSTMENT &&
          d.layers[n - 3].content.u.adjustment.kind == PC_ADJ_POSTERIZE &&
          d.layers[n - 3].content.u.adjustment.u.posterize.levels == 4);
    {
        const pc_adjustment *a = &d.layers[n - 2].content.u.adjustment;
        CHECK(d.layers[n - 2].content.kind == PC_LAYER_ADJUSTMENT && a->kind == PC_ADJ_UNSUPPORTED &&
              pc_str_eq_c(&a->u.unsupported.psd_key, "selc") && pc_blob_len(a->u.unsupported.raw) == 3);
    }
    CHECK(d.layers[n - 1].content.kind == PC_LAYER_FILL &&
          d.layers[n - 1].content.u.fill.kind == PC_FILL_SOLID &&
          d.layers[n - 1].content.u.fill.u.solid.c[0] == 1.0f);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_bytes_fini(&soco);
    pc_psd_file_fini(&f);
}

static void test_unmodelled_blocks_are_preserved(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_block b;
    pc_io_warnings w;
    pc_document d;
    bool vmsk = false;
    bool regen = false;
    uint32_t id = 0;
    push_block(&f.layer_info.layers[0], pc_psd_block_init(&b, "vmsk", (const uint8_t[8]){0}, 8, NULL), &b);
    d = to_doc(&f, &w);
    for (size_t i = 0; i < d.layers[0].npsd_blocks; i++) {
        vmsk = vmsk || memcmp(d.layers[0].psd_blocks[i].key, "vmsk", 4) == 0;
        regen = regen || memcmp(d.layers[0].psd_blocks[i].key, "luni", 4) == 0 ||
                memcmp(d.layers[0].psd_blocks[i].key, "lyid", 4) == 0;
    }
    CHECK(vmsk && !regen);
    CHECK(pc_psd_layer_id(&f.layer_info.layers[0], &id) == d.layers[0].has_psd_id &&
          id == d.layers[0].psd_id);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_fill_layer_keeps_photoshop_pixels(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_bytes soco = soco_red();
    pc_psd_layer l;
    pc_psd_block b;
    pc_io_warnings w;
    pc_document d;
    const pc_layer *last;
    pc_compose_buffer flat;
    static const uint8_t vals[4] = {255, 10, 20, 30};
    pc_psd_layer_init(&l);
    l.rect = pc_psd_rect_from_xywh(0, 0, 2, 2);
    must(pc_psd_bytes_set(&l.name, "fill", 4, NULL));
    for (int16_t id = -1; id < 3; id++) {
        pc_psd_channel c;
        uint8_t plane[4];
        memset(&c, 0, sizeof c);
        memset(plane, vals[id + 1], 4);
        c.id = id;
        c.has_compression = true;
        c.compression = PC_PSD_RAW;
        must(pc_psd_bytes_set(&c.data, plane, 4, NULL));
        must(pc_psd_layer_push_channel(&l, &c, NULL));
    }
    push_block(&l, pc_psd_block_make_unicode_name("fill", &b, NULL), &b);
    push_block(&l, pc_psd_block_init(&b, "SoCo", soco.data, soco.len, NULL), &b);
    must(pc_psd_layer_info_push(&f.layer_info, &l, NULL));
    d = to_doc(&f, &w);
    last = &d.layers[d.nlayers - 1];
    CHECK(last->content.kind == PC_LAYER_FILL && last->has_fill_cache);
    if (last->has_fill_cache) {
        float p[PC_RASTER_MAX_CHANNELS];
        pc_surface_read_pixel(&last->fill_cache.surface, 1, 1, p);
        CHECK(p[0] == 10.0f / 255.0f && p[1] == 20.0f / 255.0f && p[2] == 30.0f / 255.0f && p[3] == 1.0f);
    }
    /* The compositor uses the cache while the fill is unchanged. */
    must(pc_compose_flatten(&d, &flat, NULL));
    CHECK(fabsf(pc_compose_buffer_get(&flat, 1, 1)[0] - 10.0f / 255.0f) < 1e-3f);
    pc_compose_buffer_fini(&flat);
    /* Editing the fill invalidates the cache. */
    pc_fill_fini(&d.layers[d.nlayers - 1].content.u.fill);
    d.layers[d.nlayers - 1].content.u.fill = pc_fill_solid(pc_color_rgb(0.0f, 0.0f, 1.0f));
    must(pc_compose_flatten(&d, &flat, NULL));
    CHECK(pc_compose_buffer_get(&flat, 1, 1)[2] > 0.99f && pc_compose_buffer_get(&flat, 1, 1)[0] < 0.01f);
    pc_compose_buffer_fini(&flat);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_bytes_fini(&soco);
    pc_psd_file_fini(&f);
}

static void test_text_and_smart_detected(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_io_warnings w;
    pc_document d;
    size_t n;
    push_record(&f, "t", "TySh", (const uint8_t[8]){0}, 8);
    push_record(&f, "s", "PlLd", (const uint8_t[8]){0}, 8);
    push_record(&f, "sh", "vscg", (const uint8_t[8]){0}, 8);
    d = to_doc(&f, &w);
    n = d.nlayers;
    CHECK_EQ_STR(pc_layer_content_kind_name(&d.layers[n - 3].content), "Type");
    CHECK_EQ_STR(pc_layer_content_kind_name(&d.layers[n - 2].content), "Smart Object");
    CHECK_EQ_STR(pc_layer_content_kind_name(&d.layers[n - 1].content), "Shape");
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_guides_and_alpha_names_roundtrip(void)
{
    pc_document d;
    pc_io_export_result out;
    pc_psd_file f;
    pc_io_warnings w;
    pc_document d2;
    static const char *const names[2] = {"Spot \xc3\xa9", "Alpha 2"};
    must(pc_document_init(&d, "g", pc_size_make(8, 8), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL));
    d.guides.horizontal = xmalloc(2 * sizeof(double));
    d.guides.horizontal[0] = 1.0f;
    d.guides.horizontal[1] = 2.5f;
    d.guides.nhorizontal = 2;
    d.guides.vertical = xmalloc(sizeof(double));
    d.guides.vertical[0] = 7.03125f;
    d.guides.nvertical = 1;
    for (size_t i = 0; i < 2; i++) {
        pc_surface s;
        pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
        t_add_channel(&d, t_channel(names[i], s));
    }
    must(pc_io_export(&d, "g.psd", NULL, &out, NULL));
    f = reparse(out.bytes, out.len);
    /* RGB + merged transparency (the empty document is transparent) + 2 alpha channels. */
    CHECK_EQ_UINT(f.header.channels, 6);
    d2 = to_doc(&f, &w);
    CHECK(pc_guides_eq(&d2.guides, &d.guides));
    CHECK_EQ_UINT(d2.nchannels, 2);
    if (d2.nchannels == 2) {
        CHECK(pc_str_eq_c(&d2.channels[0].name, names[0]) && pc_str_eq_c(&d2.channels[1].name, names[1]));
    }
    pc_document_fini(&d2);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
    pc_io_export_result_fini(&out);
    pc_document_fini(&d);
}

static void test_resolution_in_cm_converted(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_resolution ri = pc_psd_resolution_from_dpi(100.0);
    uint8_t b[16];
    pc_io_warnings w;
    pc_document d;
    ri.h_res_unit = 2;
    pc_psd_resolution_write(&ri, b);
    must(pc_psd_bytes_set(&f.resources[0].data, b, 16, NULL));
    f.resources[0].id = 1005;
    d = to_doc(&f, &w);
    CHECK(fabsf(d.resolution_dpi - 254.0f) < 1e-3f);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_stale_index_resources_dropped(void)
{
    static const uint16_t ids[6] = {1024, 1026, 1036, 1057, 1069, 1072};
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_io_warnings w;
    pc_document d;
    for (size_t i = 0; i < 6; i++) {
        tg_res(&f, ids[i], (const uint8_t[2]){0, 0}, 2);
    }
    d = to_doc(&f, &w);
    for (size_t i = 0; i < d.metadata.npsd_resources; i++) {
        for (size_t k = 0; k < 6; k++) {
            CHECK(d.metadata.psd_resources[i].id != ids[k]);
        }
    }
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_merged_composite_unmattes_photoshop_white(void)
{
    pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RAW, 1, 1);
    float (*m)[4] = NULL;
    size_t n = 0;
    pc_io_warnings w;
    pc_document d;
    float p[PC_RASTER_MAX_CHANNELS];
    f.header.channels = 4;
    f.image_compression = PC_PSD_RAW;
    must(pc_psd_bytes_set(&f.image_data, (const uint8_t[4]){255, 128, 128, 128}, 4, NULL));
    must(pc_io_merged_composite(&f, &m, &n, NULL));
    CHECK(fabsf(m[0][0] - 1.0f) < 0.01f && m[0][1] < 0.01f && m[0][2] < 0.01f);
    free(m);
    d = to_doc(&f, &w);
    pc_surface_read_pixel(pc_layer_surface(&d.layers[0]), 0, 0, p);
    CHECK(p[1] < 0.01f && fabsf(p[3] - 128.0f / 255.0f) < 1e-6f);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
    /* HDR and negative straight samples survive the F32 PSD import boundary. */
    {
        pc_psd_file hdr = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, 32, PC_PSD_RAW, 1, 1);
        const float v[4] = {1.5f, 0.25f, 0.5f, 0.5f};
        uint8_t be[16];
        pc_psd_bytes bytes;
        pc_io_import_result r;
        for (size_t i = 0; i < 4; i++) {
            uint32_t u;
            memcpy(&u, &v[i], 4);
            be[i * 4] = (uint8_t)(u >> 24);
            be[i * 4 + 1] = (uint8_t)(u >> 16);
            be[i * 4 + 2] = (uint8_t)(u >> 8);
            be[i * 4 + 3] = (uint8_t)u;
        }
        hdr.header.channels = 4;
        hdr.image_compression = PC_PSD_RAW;
        must(pc_psd_bytes_set(&hdr.image_data, be, 16, NULL));
        bytes = file_bytes(&hdr);
        r = import_bytes("hdr.psd", bytes.data, bytes.len);
        CHECK(r.document.depth == PC_SAMPLE_TYPE_F32);
        pc_surface_read_pixel(pc_layer_surface(&r.document.layers[0]), 0, 0, p);
        CHECK(p[0] == 2.0f && p[1] == -0.5f && p[2] == 0.0f && p[3] == 0.5f);
        pc_io_import_result_fini(&r);
        pc_psd_bytes_fini(&bytes);
        pc_psd_file_fini(&hdr);
    }
}

static void test_vector_rendered_mask_not_doubled_on_shapes(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_layer l;
    pc_psd_block b;
    pc_psd_channel c;
    pc_io_warnings w;
    pc_document d;
    const pc_layer *last;
    pc_psd_layer_init(&l);
    must(pc_psd_bytes_set(&l.name, "shape", 5, NULL));
    l.mask.kind = PC_PSD_MASK_PARSED;
    must(pc_psd_layer_mask_make((pc_psd_rect){0, 0, 0, 0}, 0, 8, &l.mask.mask, NULL));
    memset(&c, 0, sizeof c);
    c.id = -2;
    c.has_compression = true;
    c.compression = PC_PSD_RAW;
    must(pc_psd_layer_push_channel(&l, &c, NULL));
    push_block(&l, pc_psd_block_make_unicode_name("shape", &b, NULL), &b);
    push_block(&l, pc_psd_block_init(&b, "SoCo", NULL, 0, NULL), &b);
    push_block(&l, pc_psd_block_init(&b, "vmsk", (const uint8_t[8]){0}, 8, NULL), &b);
    must(pc_psd_layer_info_push(&f.layer_info, &l, NULL));
    d = to_doc(&f, &w);
    last = &d.layers[d.nlayers - 1];
    CHECK_EQ_STR(pc_layer_content_kind_name(&last->content), "Shape");
    CHECK(!last->has_mask);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    {
        pc_psd_layer *rec = &f.layer_info.layers[f.layer_info.n - 1];
        pc_io_export_result out;
        pc_io_import_result re;
        float p[PC_RASTER_MAX_CHANNELS];
        float q[PC_RASTER_MAX_CHANNELS];
        rec->mask.mask.has_real = true;
        rec->mask.mask.real.flags = 0;
        rec->mask.mask.real.background = 255;
        rec->mask.mask.real.rect = (pc_psd_rect){0, 0, 1, 1};
        memset(&c, 0, sizeof c);
        c.id = -3;
        c.has_compression = true;
        c.compression = PC_PSD_RAW;
        must(pc_psd_bytes_set(&c.data, (const uint8_t[1]){64}, 1, NULL));
        must(pc_psd_layer_push_channel(rec, &c, NULL));
        d = to_doc(&f, &w);
        last = &d.layers[d.nlayers - 1];
        CHECK_EQ_STR(pc_layer_content_kind_name(&last->content), "Shape");
        CHECK(last->has_mask);
        if (last->has_mask) {
            pc_surface_read_pixel(&last->mask.surface, 0, 0, p);
            CHECK(fabsf(p[0] - 64.0f / 255.0f) < 1e-6f);
            must(pc_io_export(&d, "real-mask.psd", NULL, &out, NULL));
            re = import_bytes("real-mask.psd", out.bytes, out.len);
            CHECK(re.document.layers[re.document.nlayers - 1].has_mask);
            pc_surface_read_pixel(&re.document.layers[re.document.nlayers - 1].mask.surface, 0, 0, q);
            CHECK(q[0] == p[0]);
            pc_io_import_result_fini(&re);
            pc_io_export_result_fini(&out);
        }
        pc_document_fini(&d);
        pc_io_warnings_fini(&w);
    }
    pc_psd_file_fini(&f);
}

static void test_every_blend_mode_maps_both_ways(void)
{
    /* Pass Through and the Photoshop modes (the Paint.NET ones have no PSD key: "norm"). */
    for (int m = 0; m < PC_BLEND_PSD_COUNT; m++) {
        const char *k = pc_blend_psd_key((pc_blend_mode)m);
        pc_blend_mode back;
        CHECK(pc_psd_blend_index((const uint8_t *)k) >= 0);
        CHECK(pc_blend_from_psd_key(k, &back) && back == (pc_blend_mode)m);
    }
}

static void test_layer_bounds_without_channel_data_allocate_nothing(void)
{
    pc_psd_file f = tg_small(PC_PSD_V1, PC_PSD_RAW);
    pc_psd_layer l;
    pc_io_warnings w;
    pc_document d;
    const pc_layer *last;
    pc_psd_layer_init(&l);
    l.rect = pc_psd_rect_from_xywh(0, 0, 300000, 300000);
    must(pc_psd_bytes_set(&l.name, "huge", 4, NULL));
    must(pc_psd_layer_info_push(&f.layer_info, &l, NULL));
    d = to_doc(&f, &w);
    last = &d.layers[d.nlayers - 1];
    CHECK(last->content.kind == PC_LAYER_RASTER && pc_surface_tile_count(&last->content.u.raster) == 0);
    CHECK(pc_io_warnings_any(&w, "\"huge\""));
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

/* ---- never crash ---- */

typedef struct cancel_state {
    int calls;
    int at;
} cancel_state;

static bool cancel_at(void *ctx)
{
    cancel_state *s = ctx;
    return ++s->calls >= s->at;
}

/* Cancelling at every check point fails with "cancelled" and frees everything (ASan). */
static void test_cancellation_at_every_check(void)
{
    pc_psd_file f = tg_layered(PC_PSD_V1, PC_PSD_MODE_RGB, 8, PC_PSD_RLE);
    int total = 0;
    {
        cancel_state s = {0, 1 << 30};
        pc_interrupt ctl = {cancel_at, NULL, &s};
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_document d;
        must(pc_io_psd_to_document_with(&f, &ctl, &d, &w, NULL));
        pc_document_fini(&d);
        pc_io_warnings_fini(&w);
        total = s.calls;
    }
    CHECK(total > 10);
    for (int at = 1; at <= total; at++) {
        cancel_state s = {0, at};
        pc_interrupt ctl = {cancel_at, NULL, &s};
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_document d;
        pc_error e;
        pc_status st = pc_io_psd_to_document_with(&f, &ctl, &d, &w, &e);
        CHECK_EQ_INT(st, PC_ERR_CANCELLED);
        if (st == PC_OK) {
            pc_document_fini(&d);
        } else {
            CHECK_EQ_STR(e.msg, "cancelled");
        }
        pc_io_warnings_fini(&w);
    }
    pc_psd_file_fini(&f);
}

/* Hostile records: every field of a layered file's records mutated, never a crash. */
static void test_hostile_records(void)
{
    pc_psd_file base = tg_layered(PC_PSD_V1, PC_PSD_MODE_CMYK, 16, PC_PSD_RLE);
    uint32_t s = 99;
    for (int k = 0; k < 300; k++) {
        pc_psd_file f;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_document d;
        must(pc_psd_file_clone(&base, &f, NULL));
        for (size_t i = 0; i < f.layer_info.n; i++) {
            pc_psd_layer *l = &f.layer_info.layers[i];
            s = s * 1103515245u + 12345u;
            switch ((s >> 16) % 6) {
            case 0:
                l->rect.right = (int32_t)(s >> 3) % 400000;
                break;
            case 1:
                for (size_t b = 0; b < l->nblocks; b++) {
                    if (l->blocks[b].data.len > 0) {
                        l->blocks[b].data.data[(s >> 7) % l->blocks[b].data.len] ^= (uint8_t)(s >> 24);
                    }
                }
                break;
            case 2:
                if (l->nchannels > 0) {
                    l->channels[(s >> 5) % l->nchannels].id = (int16_t)((s >> 9) % 9) - 4;
                }
                break;
            case 3:
                l->blend[0] = (uint8_t)(s >> 20);
                break;
            case 4:
                l->blending_ranges.len = (s >> 11) % (l->blending_ranges.len + 1);
                break;
            default:
                l->flags = (uint8_t)(s >> 13);
                break;
            }
        }
        must(pc_io_psd_to_document(&f, &d, &w, NULL));
        pc_document_fini(&d);
        pc_io_warnings_fini(&w);
        pc_psd_file_fini(&f);
    }
    pc_psd_file_fini(&base);
}

int main(int argc, char **argv)
{
    RUN(test_testgen_layered);
    RUN(test_testgen_small_all_compressions);
    RUN(test_testgen_import_details);
    RUN(test_fallback_modes_import_flattened);
    RUN(test_multichannel_imports_ink_channels);
    RUN(test_indexed_colors_match_palette);
    RUN(test_flattened_psd_becomes_background);
    RUN(test_builder_psd_imports);
    RUN(test_group_nesting_past_the_cap_is_rejected);
    RUN(test_group_nesting_at_the_cap_imports);
    RUN(test_layered_import_skips_the_unused_merged_composite);
    RUN(test_locks_and_labels_from_psd);
    RUN(test_artboard_lock_round_trips_byte_for_byte);
    RUN(test_adjustment_and_fill_layers_from_psd);
    RUN(test_unmodelled_blocks_are_preserved);
    RUN(test_fill_layer_keeps_photoshop_pixels);
    RUN(test_text_and_smart_detected);
    RUN(test_guides_and_alpha_names_roundtrip);
    RUN(test_resolution_in_cm_converted);
    RUN(test_stale_index_resources_dropped);
    RUN(test_merged_composite_unmattes_photoshop_white);
    RUN(test_vector_rendered_mask_not_doubled_on_shapes);
    RUN(test_every_blend_mode_maps_both_ways);
    RUN(test_layer_bounds_without_channel_data_allocate_nothing);
    RUN(test_cancellation_at_every_check);
    RUN(test_hostile_records);
    return pc_t_finish();
}
