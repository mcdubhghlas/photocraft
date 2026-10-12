/* Unit tests of chunk I4's maps: the #[test]s of crates/io/src/{vector_map, comps_map,
 * annotations_map, gradient_bake, pattern_map, slices_map}.rs (abr_map/tests.rs, the ABR part of
 * tests/cancel.rs and the never-crash sweeps are in test_maps_abr.c). */
#include "harness.h"

#include "maps.h"
#include "io/maps_desc.h"
#include "paint/render.h"
#include "psd/slices.h"
#include "vector/vector.h"

/* ---------------------------------------------------------------- helpers */

static pc_path sample_path(void)
{
    pc_path p;
    static const double line[2][2] = {{1.0, 2.0}, {3.0, 4.0}};
    memset(&p, 0, sizeof p);
    p.subpaths = xmalloc(2 * sizeof *p.subpaths);
    p.nsubpaths = 2;
    p.subpaths[0].closed = true;
    p.subpaths[0].op = PC_PATH_COMBINE;
    p.subpaths[0].knots = xmalloc(3 * sizeof(pc_knot));
    p.subpaths[0].nknots = 3;
    p.subpaths[0].knots[0] = pc_knot_corner(10.0, 10.0);
    p.subpaths[0].knots[1] =
        pc_knot_smooth((pc_point){50.0, 20.0}, (pc_point){40.0, 10.0}, (pc_point){60.0, 30.0});
    p.subpaths[0].knots[2] = pc_knot_corner(20.0, 40.0);
    must(pc_subpath_polyline(line, 2, &p.subpaths[1], NULL));
    p.subpaths[1].op = PC_PATH_SUBTRACT;
    return p;
}

/* vmsk_with of vector_map.rs's tests: 0.25 squares stepping 0.1 to the right. */
static pc_buf vmsk_with(bool initial_fill, const int16_t *ops, size_t n)
{
    pc_psd_subpath subs[8];
    pc_psd_vector_mask m;
    pc_psd_bytes b;
    pc_buf out = PC_BUF_INIT;
    for (size_t i = 0; i < n; i++) {
        double x = 0.1 * (double)i;
        double pts[4][2] = {{x, 0.25}, {x + 0.25, 0.25}, {x + 0.25, 0.5}, {x, 0.5}};
        subs[i].closed = true;
        subs[i].operation = ops[i];
        subs[i].knots = xmalloc(4 * sizeof *subs[i].knots);
        subs[i].n = 4;
        for (size_t k = 0; k < 4; k++) {
            pc_psd_knot *kn = &subs[i].knots[k];
            kn->linked = false;
            kn->pre[0] = kn->anchor[0] = kn->post[0] = pts[k][0];
            kn->pre[1] = kn->anchor[1] = kn->post[1] = pts[k][1];
        }
    }
    m.version = 3;
    m.flags = 0;
    must(pc_psd_path_from_subpaths(subs, n, initial_fill, &m.path, NULL));
    must(pc_psd_vector_mask_to_bytes(&m, &b, NULL));
    must(pc_buf_append(&out, b.data, b.len, NULL));
    pc_psd_bytes_fini(&b);
    pc_psd_path_fini(&m.path);
    for (size_t i = 0; i < n; i++) {
        free(subs[i].knots);
    }
    return out;
}

static pc_path vmsk_path(const pc_buf *b, uint32_t w, uint32_t h)
{
    pc_path p;
    bool found = false;
    must(pc_io_path_from_vmsk(b->data, b->len, w, h, &found, &p, NULL, NULL));
    if (!found) {
        abort();
    }
    return p;
}

static pc_vector_mask vmask(const uint8_t *d, size_t len, uint32_t w, uint32_t h)
{
    pc_vector_mask m;
    bool found = false;
    must(pc_io_vector_mask_from_block(d, len, w, h, &found, &m, NULL));
    if (!found) {
        abort();
    }
    return m;
}

/* ---------------------------------------------------------------- vector_map */

static void test_path_records_roundtrip(void)
{
    pc_path p = sample_path();
    pc_psd_path rec;
    pc_path back;
    must(pc_io_path_to_records(&p, 64, 64, &rec, NULL));
    must(pc_io_path_from_records(&rec, 64, 64, &back, NULL));
    CHECK(pc_path_eq(&back, &p));
    pc_psd_path_fini(&rec);
    pc_path_fini(&back);
    pc_path_fini(&p);
}

static void test_layer_path_semantics_follow_photoshop(void)
{
    static const int16_t c1[1] = {1};
    static const int16_t lead[4] = {2, 1, 2, 0};
    static const int16_t join[3] = {1, 2, -1};
    static const int16_t lead_join[3] = {2, -1, 1};
    pc_buf b;
    pc_path p;
    pc_index_range *comp = NULL;
    size_t nc = 0;
    b = vmsk_with(true, c1, 1);
    p = vmsk_path(&b, 100, 100);
    CHECK(!p.inverted);
    pc_path_fini(&p);
    pc_buf_fini(&b);
    b = vmsk_with(true, lead, 4);
    p = vmsk_path(&b, 100, 100);
    CHECK(p.inverted);
    CHECK(p.subpaths[0].op == PC_PATH_COMBINE && p.subpaths[1].op == PC_PATH_SUBTRACT &&
          p.subpaths[2].op == PC_PATH_COMBINE && p.subpaths[3].op == PC_PATH_EXCLUDE);
    pc_path_fini(&p);
    pc_buf_fini(&b);
    b = vmsk_with(false, join, 3);
    p = vmsk_path(&b, 100, 100);
    CHECK(p.subpaths[2].op == PC_PATH_JOIN);
    must(pc_path_components(&p, &comp, &nc, NULL));
    CHECK_EQ_UINT(nc, 2);
    CHECK(comp[0].start == 0 && comp[0].end == 1 && comp[1].start == 1 && comp[1].end == 3);
    free(comp);
    pc_path_fini(&p);
    pc_buf_fini(&b);
    b = vmsk_with(false, lead_join, 3);
    p = vmsk_path(&b, 100, 100);
    CHECK(p.inverted);
    CHECK(p.subpaths[0].op == PC_PATH_COMBINE && p.subpaths[1].op == PC_PATH_JOIN &&
          p.subpaths[2].op == PC_PATH_SUBTRACT);
    pc_path_fini(&p);
    pc_buf_fini(&b);
    /* No subpaths: a vector mask with initial fill reveals all, without hides all. */
    {
        pc_buf rb = vmsk_with(true, NULL, 0);
        pc_buf hb = vmsk_with(false, NULL, 0);
        pc_vector_mask reveal = vmask(rb.data, rb.len, 100, 100);
        pc_vector_mask hide = vmask(hb.data, hb.len, 100, 100);
        float *v = NULL;
        size_t n = 0;
        pc_vector_mask *ms[2] = {&reveal, &hide};
        must(pc_vec_mask_values(&reveal, pc_rect_make(0, 0, 2, 2), &v, &n, NULL));
        CHECK(n == 4 && v[0] == 1.0f && v[3] == 1.0f);
        free(v);
        must(pc_vec_mask_values(&hide, pc_rect_make(0, 0, 2, 2), &v, &n, NULL));
        CHECK(n == 4 && v[0] == 0.0f && v[3] == 0.0f);
        free(v);
        p = vmsk_path(&rb, 100, 100);
        CHECK(p.inverted);
        pc_path_fini(&p);
        for (size_t i = 0; i < 2; i++) {
            pc_buf out = PC_BUF_INIT;
            pc_vector_mask back;
            must(pc_io_vector_mask_bytes(ms[i], 100, 100, &out, NULL));
            back = vmask(out.data, out.len, 100, 100);
            CHECK(pc_vector_mask_eq(&back, ms[i]));
            pc_vector_mask_fini(&back);
            pc_buf_fini(&out);
        }
        pc_vector_mask_fini(&reveal);
        pc_vector_mask_fini(&hide);
        pc_buf_fini(&rb);
        pc_buf_fini(&hb);
    }
    {
        pc_path lp = sample_path();
        pc_buf out = PC_BUF_INIT;
        lp.subpaths[0].op = PC_PATH_SUBTRACT;
        must(pc_io_vmsk_bytes(&lp, 0, 64, 64, &out, NULL));
        p = vmsk_path(&out, 64, 64);
        CHECK(!p.inverted); /* a stored leading subtract is drawn as combine by the model */
        pc_path_fini(&p);
        pc_path_fini(&lp);
        pc_buf_fini(&out);
    }
}

static void test_vector_mask_roundtrip_with_flags(void)
{
    pc_path p = sample_path();
    pc_vector_mask m;
    pc_vector_mask back;
    pc_buf b = PC_BUF_INIT;
    bool matches = false;
    pc_vector_mask_init(&m, &p);
    m.enabled = false;
    m.linked = false;
    m.path.inverted = true;
    must(pc_io_vector_mask_bytes(&m, 64, 64, &b, NULL));
    back = vmask(b.data, b.len, 64, 64);
    CHECK(pc_vector_mask_eq(&back, &m));
    must(pc_io_vector_mask_block_matches(b.data, b.len, &m, 64, 64, &matches, NULL));
    CHECK(matches);
    pc_vector_mask_fini(&back);
    pc_vector_mask_fini(&m);
    pc_buf_fini(&b);
}

static void test_vstk_roundtrip(void)
{
    pc_shape_stroke s;
    pc_buf b = PC_BUF_INIT;
    pc_io_vstk v;
    bool found = false;
    float rgb[3];
    static const float dashes[2] = {2.0f, 1.0f};
    pc_shape_stroke_default(&s);
    s.width = 4.5f;
    s.paint = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
    s.opacity = 0.5f;
    s.align = PC_STROKE_ALIGN_OUTSIDE;
    s.cap = PC_LINE_CAP_ROUND;
    s.join = PC_LINE_JOIN_BEVEL;
    s.miter_limit = 10.0f;
    s.dashes = xmalloc(sizeof dashes);
    memcpy(s.dashes, dashes, sizeof dashes);
    s.ndashes = 2;
    s.dash_offset = 0.5f;
    must(pc_io_vstk_bytes(&s, false, 72.0f, &b, NULL));
    must(pc_io_parse_vstk(b.data, b.len, 72.0f, &found, &v, NULL));
    CHECK(found && !v.fill_enabled && v.has_stroke);
    CHECK(v.stroke.width == s.width);
    CHECK(v.stroke.cap == s.cap && v.stroke.join == s.join && v.stroke.align == s.align);
    CHECK(v.stroke.ndashes == 2 && v.stroke.dashes[0] == 2.0f && v.stroke.dashes[1] == 1.0f);
    CHECK(v.stroke.paint.kind == PC_FILL_SOLID);
    pc_color_to_rgb(&v.stroke.paint.u.solid, rgb);
    CHECK(fabsf(rgb[0] - 1.0f) < 1e-3f);
    pc_io_vstk_fini(&v);
    must(pc_io_vstk_bytes(NULL, true, 72.0f, &b, NULL));
    must(pc_io_parse_vstk(b.data, b.len, 72.0f, &found, &v, NULL));
    CHECK(found && !v.has_stroke && v.fill_enabled);
    pc_shape_stroke_fini(&s);
    pc_buf_fini(&b);
}

static void test_vogk_roundtrip(void)
{
    pc_live_shape shapes[4];
    memset(shapes, 0, sizeof shapes);
    shapes[0].kind = PC_LIVE_RECT;
    shapes[0].rect[0] = 1.0;
    shapes[0].rect[1] = 2.0;
    shapes[0].rect[2] = 30.0;
    shapes[0].rect[3] = 40.0;
    shapes[1] = shapes[0];
    shapes[1].radii[0] = 1.0;
    shapes[1].radii[1] = 2.0;
    shapes[1].radii[2] = 3.0;
    shapes[1].radii[3] = 4.0;
    shapes[2].kind = PC_LIVE_ELLIPSE;
    shapes[2].rect[0] = 5.0;
    shapes[2].rect[1] = 6.0;
    shapes[2].rect[2] = 7.0;
    shapes[2].rect[3] = 8.0;
    shapes[3].kind = PC_LIVE_LINE;
    shapes[3].from[1] = 10.0;
    shapes[3].to[0] = 20.0;
    shapes[3].weight = 3.0;
    for (size_t i = 0; i < 4; i++) {
        pc_buf b = PC_BUF_INIT;
        bool has = false;
        bool found = false;
        pc_live_shape back;
        must(pc_io_vogk_bytes(&shapes[i], 72.0f, &has, &b, NULL));
        must(pc_io_live_from_vogk(b.data, b.len, &found, &back, NULL));
        CHECK(has && found && pc_live_shape_eq(&back, &shapes[i]));
        pc_buf_fini(&b);
    }
    {
        pc_live_shape poly;
        pc_buf b = PC_BUF_INIT;
        bool has = true;
        memset(&poly, 0, sizeof poly);
        poly.kind = PC_LIVE_POLYGON;
        poly.sides = 5;
        poly.star_ratio = 1.0;
        must(pc_io_vogk_bytes(&poly, 72.0f, &has, &b, NULL));
        CHECK(!has);
    }
}

typedef struct raw_ctx {
    const pc_io_raw_blocks *raw;
} raw_ctx;

static const uint8_t *raw_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    const pc_io_raw_block *b = pc_io_raw_blocks_find(((raw_ctx *)ctx)->raw, key);
    if (b == NULL) {
        return NULL;
    }
    *len = b->len;
    return b->data;
}

static bool raw_eq(const pc_io_raw_blocks *a, const pc_io_raw_blocks *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (size_t i = 0; i < a->n; i++) {
        if (memcmp(a->v[i].key, b->v[i].key, 4) != 0 || a->v[i].len != b->v[i].len ||
            memcmp(a->v[i].data, b->v[i].data, a->v[i].len) != 0) {
            return false;
        }
    }
    return true;
}

static void test_shape_blocks_keep_unchanged_and_regenerate_changed(void)
{
    pc_shape_layer sh;
    pc_shape_layer edited;
    pc_shape_layer back;
    pc_io_raw_blocks raw = {NULL, 0, 0};
    pc_io_raw_blocks before = {NULL, 0, 0};
    raw_ctx ctx = {&raw};
    memset(&sh, 0, sizeof sh);
    sh.path = sample_path();
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(0.0f, 1.0f, 0.0f));
    sh.has_stroke = true;
    pc_shape_stroke_default(&sh.stroke);
    sh.has_live = true;
    sh.live.kind = PC_LIVE_ELLIPSE;
    sh.live.rect[2] = 10.0;
    sh.live.rect[3] = 10.0;
    must(pc_io_shape_blocks(&sh, &raw, 64, 64, 72.0f, NULL));
    CHECK_EQ_UINT(raw.n, 3);
    CHECK(memcmp(raw.v[0].key, "vmsk", 4) == 0 && memcmp(raw.v[1].key, "vstk", 4) == 0 &&
          memcmp(raw.v[2].key, "vogk", 4) == 0);
    for (size_t i = 0; i < raw.n; i++) {
        must(pc_io_raw_blocks_push(&before, raw.v[i].key, raw.v[i].data, raw.v[i].len, NULL));
    }
    must(pc_io_shape_blocks(&sh, &raw, 64, 64, 72.0f, NULL));
    CHECK(raw_eq(&raw, &before));
    memset(&back, 0, sizeof back);
    must(pc_io_shape_from_blocks(&back, raw_lookup, &ctx, 64, 64, 72.0f, NULL));
    CHECK(pc_path_eq(&back.path, &sh.path));
    CHECK(back.has_stroke && pc_shape_stroke_eq(&back.stroke, &sh.stroke));
    CHECK(back.has_live && pc_live_shape_eq(&back.live, &sh.live));
    pc_shape_layer_fini(&back);
    /* Edit: path changes -> vmsk regenerated, live dropped for a polygon. */
    must(pc_shape_layer_clone(&sh, &edited, NULL));
    pc_subpath_fini(&edited.path.subpaths[1]);
    edited.path.nsubpaths = 1;
    memset(&edited.live, 0, sizeof edited.live);
    edited.live.kind = PC_LIVE_POLYGON;
    edited.live.sides = 3;
    edited.live.star_ratio = 1.0;
    must(pc_io_shape_blocks(&edited, &raw, 64, 64, 72.0f, NULL));
    CHECK(pc_io_raw_blocks_find(&raw, (const uint8_t *)"vogk") == NULL);
    {
        pc_buf b = PC_BUF_INIT;
        pc_path p;
        must(pc_buf_append(&b, raw.v[0].data, raw.v[0].len, NULL));
        p = vmsk_path(&b, 64, 64);
        CHECK(pc_path_eq(&p, &edited.path));
        pc_path_fini(&p);
        pc_buf_fini(&b);
    }
    pc_shape_layer_fini(&edited);
    pc_shape_layer_fini(&sh);
    pc_io_raw_blocks_fini(&raw);
    pc_io_raw_blocks_fini(&before);
}

/* ---------------------------------------------------------------- comps_map */

static pc_artboard art(pc_rect r, pc_artboard_bg_kind kind)
{
    pc_artboard a;
    pc_artboard_init(&a, r);
    a.background.kind = kind;
    return a;
}

static void test_artboard_roundtrip(void)
{
    pc_artboard a = art(pc_rect_make(10, 20, 110, 220), PC_ARTBOARD_CUSTOM);
    pc_artboard back;
    pc_buf b = PC_BUF_INIT;
    bool found = false;
    static const int32_t guides[2] = {0, 2};
    static const uint8_t junk[3] = {1, 2, 3};
    static const pc_artboard_bg_kind kinds[3] = {PC_ARTBOARD_WHITE, PC_ARTBOARD_BLACK,
                                                 PC_ARTBOARD_TRANSPARENT};
    a.background.custom = pc_color_rgb(0.2f, 0.4f, 0.6f);
    must(pc_str_set_c(&a.preset, "Web", NULL));
    must(pc_io_write_artboard(&a, guides, 2, &b, NULL));
    CHECK_EQ_UINT(b.len % 2, 0);
    must(pc_io_parse_artboard(b.data, b.len, &found, &back, NULL));
    CHECK(found && pc_rect_eq(back.rect, a.rect) && pc_str_eq_c(&back.preset, "Web"));
    CHECK(back.background.kind == PC_ARTBOARD_CUSTOM && fabsf(back.background.custom.c[1] - 0.4f) < 1e-3f);
    pc_artboard_fini(&back);
    pc_artboard_fini(&a);
    for (size_t i = 0; i < 3; i++) {
        pc_artboard x = art(pc_rect_make(0, 0, 4, 4), kinds[i]);
        must(pc_io_write_artboard(&x, NULL, 0, &b, NULL));
        must(pc_io_parse_artboard(b.data, b.len, &found, &back, NULL));
        CHECK(found && pc_artboard_eq(&back, &x));
        pc_artboard_fini(&back);
        pc_artboard_fini(&x);
    }
    must(pc_io_parse_artboard(junk, 3, &found, &back, NULL));
    CHECK(!found);
    pc_buf_fini(&b);
}

static pc_layer_comp one_comp(uint32_t id, pc_layer_id layer, int vis, bool has_pos, int32_t x, int32_t y)
{
    pc_layer_comp c;
    memset(&c, 0, sizeof c);
    c.id = id;
    must(pc_str_set_c(&c.name, "One", NULL));
    c.apply_visibility = true;
    c.apply_position = true;
    c.states = xmalloc(sizeof *c.states);
    memset(c.states, 0, sizeof *c.states);
    c.nstates = 1;
    c.states[0].layer = layer;
    c.states[0].has_visible = vis >= 0;
    c.states[0].visible = vis > 0;
    c.states[0].has_position = has_pos;
    c.states[0].position[0] = x;
    c.states[0].position[1] = y;
    return c;
}

static void test_cmls_setting_roundtrip_and_shmd_merge(void)
{
    pc_layer_id lid;
    pc_document d = m_comp_doc(NULL, 0, &lid);
    pc_buf res = PC_BUF_INIT;
    pc_buf cm = PC_BUF_INIT;
    bool has = false;
    pc_io_raw_blocks raw = {NULL, 0, 0};
    pc_psd_meta_item item;
    pc_psd_bytes shmd;
    pc_psd_meta_item *items = NULL;
    size_t n = 0;
    uint8_t *data;
    pc_io_comps c;
    bool same = false;
    static const uint8_t cust[2] = {1, 2};
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    pc_layer *layer;
    d.layer_comps = xmalloc(sizeof *d.layer_comps);
    d.layer_comps[0] = one_comp(7, lid, 0, true, 10, 1);
    must(pc_str_set_c(&d.layer_comps[0].comment, "c", NULL));
    d.nlayer_comps = 1;
    d.has_last_applied_comp = true;
    d.last_applied_comp = 7;
    must(pc_io_write_comps_resource(&d, &has, &res, NULL));
    CHECK(has);
    must(pc_io_write_cmls(d.layer_comps, 1, NULL, pc_document_layer(&d, lid), 3, &has, &cm, NULL));
    CHECK(has);
    /* Attach as Photoshop would: shmd with cmls plus another item. */
    must(pc_psd_meta_item_make("cust", cust, 2, &item, NULL));
    must(pc_psd_write_shmd(&item, 1, &shmd, NULL));
    pc_psd_bytes_fini(&item.data);
    must(pc_io_raw_blocks_push_take(&raw, (const uint8_t *)"shmd", &shmd.data, shmd.len, NULL));
    data = pc_buf_take(&cm, &n);
    must(pc_io_set_cmls(&raw, &data, n, NULL));
    must(pc_psd_parse_shmd(raw.v[0].data, raw.v[0].len, &items, &n, NULL));
    CHECK_EQ_UINT(n, 2);
    pc_psd_meta_items_free(items, n);
    layer = pc_document_layer_mut(&d, lid);
    layer->psd_blocks = xmalloc(sizeof *layer->psd_blocks);
    memcpy(layer->psd_blocks[0].key, "shmd", 4);
    must(pc_blob_new(raw.v[0].data, raw.v[0].len, &layer->psd_blocks[0].data, NULL));
    layer->npsd_blocks = 1;
    m_push_resource(&d, PC_IO_LAYER_COMPS, res.data, res.len);
    must(pc_io_decode_comps(res.data, res.len, &d, &c, NULL, NULL));
    CHECK(c.n == 1 && pc_layer_comp_eq(&c.comps[0], &d.layer_comps[0]));
    CHECK(c.has_last_applied && c.last_applied == 7 && !c.has_last_state);
    pc_io_comps_fini(&c);
    must(pc_io_comps_unchanged(&d, &same, NULL));
    CHECK(same);
    /* Moving the layer changes the decoded (relative) position. */
    must(pc_surface_fill_rect(pc_layer_surface_mut(pc_document_layer_mut(&d, lid)), pc_rect_make(0, 0, 2, 2),
                              black, 4, NULL));
    must(pc_io_comps_unchanged(&d, &same, NULL));
    CHECK(!same);
    /* Removing cmls keeps the other item; removing everything drops the block. */
    must(pc_io_set_cmls(&raw, NULL, 0, NULL));
    must(pc_psd_parse_shmd(raw.v[0].data, raw.v[0].len, &items, &n, NULL));
    CHECK_EQ_UINT(n, 1);
    pc_psd_meta_items_free(items, n);
    pc_io_raw_blocks_fini(&raw);
    {
        static const uint8_t zero2[2] = {0, 0};
        must(pc_psd_meta_item_make("cmls", zero2, 2, &item, NULL));
        must(pc_psd_write_shmd(&item, 1, &shmd, NULL));
        pc_psd_bytes_fini(&item.data);
        must(pc_io_raw_blocks_push_take(&raw, (const uint8_t *)"shmd", &shmd.data, shmd.len, NULL));
        must(pc_io_set_cmls(&raw, NULL, 0, NULL));
        CHECK_EQ_UINT(raw.n, 0);
    }
    pc_io_raw_blocks_fini(&raw);
    pc_buf_fini(&res);
    pc_document_fini(&d);
}

/* A layer at (4, 4) whose cmls has one setting for comp 1: visible, offset by (h, v). */
static pc_document doc_with_offset(pc_psd_value h, pc_psd_value v, pc_buf *res, pc_layer_id *lid)
{
    pc_status st = PC_OK;
    pc_psd_desc s;
    pc_psd_desc o;
    pc_psd_desc d;
    pc_psd_desc comp;
    pc_psd_value ids = pc_io4_list();
    pc_psd_value list = pc_io4_list();
    pc_psd_value comps = pc_io4_list();
    pc_psd_value item;
    pc_psd_bytes b;
    pc_document doc;
    pc_io4_desc(&s, "null", &st, NULL);
    pc_io4_put(&s, "enab", pc_psd_value_bool(true), &st, NULL);
    pc_io4_list_push(&ids, pc_psd_value_int(1), &st, NULL);
    pc_io4_put(&s, "compList", ids, &st, NULL);
    pc_io4_desc(&o, "null", &st, NULL);
    pc_io4_put(&o, "Hrzn", h, &st, NULL);
    pc_io4_put(&o, "Vrtc", v, &st, NULL);
    pc_io4_put_obj(&s, "Ofst", &o, &st, NULL);
    must(st);
    must(pc_psd_value_object(&s, &item, NULL));
    pc_io4_list_push(&list, item, &st, NULL);
    pc_io4_desc(&d, "null", &st, NULL);
    pc_io4_put(&d, "layerSettings", list, &st, NULL);
    must(st);
    must(pc_psd_vdesc_to_bytes(&d, &b, NULL));
    pc_psd_desc_fini(&d);
    doc = m_comp_doc(b.data, b.len, lid);
    pc_psd_bytes_fini(&b);
    pc_io4_desc(&comp, "Comp", &st, NULL);
    pc_io4_put_text(&comp, "Nm  ", "", 0, true, &st, NULL);
    pc_io4_put(&comp, "compID", pc_psd_value_int(1), &st, NULL);
    pc_io4_put(&comp, "capturedInfo", pc_psd_value_int(3), &st, NULL);
    must(st);
    must(pc_psd_value_object(&comp, &item, NULL));
    pc_io4_list_push(&comps, item, &st, NULL);
    pc_io4_desc(&d, "CompList", &st, NULL);
    pc_io4_put(&d, "list", comps, &st, NULL);
    must(st);
    must(pc_psd_vdesc_to_bytes(&d, &b, NULL));
    pc_psd_desc_fini(&d);
    must(pc_buf_append(res, b.data, b.len, NULL));
    pc_psd_bytes_fini(&b);
    return doc;
}

static void test_unrepresentable_comp_offsets_are_dropped_not_wrapped(void)
{
    static const int32_t cases[2][2] = {{INT32_MAX, 0}, {0, INT32_MAX}};
    for (size_t k = 0; k < 3; k++) {
        pc_buf res = PC_BUF_INIT;
        pc_layer_id lid;
        pc_psd_value h = k < 2 ? pc_psd_value_int(cases[k][0]) : pc_psd_value_int(6);
        pc_psd_value v = k < 2 ? pc_psd_value_int(cases[k][1]) : pc_psd_value_int(-3);
        pc_document d = doc_with_offset(h, v, &res, &lid);
        pc_io_comps c;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        const pc_comp_layer_state *st;
        must(pc_io_decode_comps(res.data, res.len, &d, &c, &w, NULL));
        st = pc_layer_comp_state(&c.comps[0], lid);
        CHECK(st != NULL && st->has_visible && st->visible);
        if (k < 2) {
            CHECK(!st->has_position);
            CHECK_EQ_UINT(w.n, 1);
            CHECK(pc_io_warnings_any(&w, "layer comp position"));
        } else {
            CHECK(st->has_position && st->position[0] == 10 && st->position[1] == 1);
            CHECK_EQ_UINT(w.n, 0);
        }
        pc_io_warnings_fini(&w);
        pc_io_comps_fini(&c);
        pc_document_fini(&d);
        pc_buf_fini(&res);
    }
    {
        /* Offsets wider than i32 (64-bit integers) are dropped too, not truncated. */
        pc_buf res = PC_BUF_INIT;
        pc_layer_id lid;
        pc_psd_value h;
        pc_document d;
        pc_io_comps c;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        memset(&h, 0, sizeof h);
        h.kind = PC_PSD_V_LARGE_INTEGER;
        h.l = (int64_t)UINT32_MAX + 2;
        d = doc_with_offset(h, pc_psd_value_int(0), &res, &lid);
        must(pc_io_decode_comps(res.data, res.len, &d, &c, &w, NULL));
        CHECK(!pc_layer_comp_state(&c.comps[0], lid)->has_position);
        CHECK_EQ_UINT(w.n, 1);
        pc_io_warnings_fini(&w);
        pc_io_comps_fini(&c);
        pc_document_fini(&d);
        pc_buf_fini(&res);
    }
}

static void test_unrepresentable_comp_positions_are_written_without_offset(void)
{
    pc_layer l;
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    static const int32_t cases[3][2] = {{INT32_MIN, 0}, {0, INT32_MIN}, {10, 1}};
    must(pc_layer_raster(&l, "A", PC_PIXEL_FORMAT_RGBA8, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(4, 4, 8, 8), red, 4, NULL));
    for (size_t k = 0; k < 3; k++) {
        pc_layer_comp c = one_comp(1, l.id, 0, true, cases[k][0], cases[k][1]);
        pc_buf b = PC_BUF_INIT;
        bool has = false;
        pc_psd_desc d;
        const pc_psd_value *ls;
        const pc_psd_desc *s;
        const pc_psd_desc *o;
        must(pc_io_write_cmls(&c, 1, NULL, &l, 1, &has, &b, NULL));
        must(pc_psd_vdesc_parse(b.data, b.len, &d, NULL));
        ls = pc_psd_desc_get(&d, "layerSettings");
        s = ls->list[0].desc;
        CHECK(pc_psd_desc_get(s, "enab") != NULL && !pc_psd_desc_get(s, "enab")->b);
        o = pc_io4_obj(s, "Ofst");
        if (k < 2) {
            CHECK(o == NULL);
        } else {
            CHECK(o != NULL && pc_psd_desc_get(o, "Hrzn")->i == 6 && pc_psd_desc_get(o, "Vrtc")->i == -3);
        }
        pc_psd_desc_fini(&d);
        pc_buf_fini(&b);
        pc_layer_comp_fini(&c);
    }
    pc_layer_fini(&l);
}

/* ---------------------------------------------------------------- annotations_map */

static pc_note note(const char *text, const char *author, double x, double y)
{
    pc_note n;
    pc_note_default(&n);
    must(pc_str_set_c(&n.text, text, NULL));
    must(pc_str_set_c(&n.author, author, NULL));
    n.position[0] = x;
    n.position[1] = y;
    return n;
}

static void test_annotations_sample_byte_exact(void)
{
    /* corpus/psd/ag-psd/read-write/annotations/src.psd (skipped without the corpus). */
    pc_buf file = PC_BUF_INIT;
    const uint8_t *at = NULL;
    pc_note *n = NULL;
    size_t nn = 0;
    bool found = false;
    uint32_t len;
    float rgb[3];
    if (pc_file_read("corpus/psd/ag-psd/read-write/annotations/src.psd", (size_t)1 << 30, &file, NULL) !=
        PC_OK) {
        (void)printf("  (corpus missing: annotations sample skipped)\n");
        return;
    }
    for (size_t i = 0; i + 12 <= file.len && at == NULL; i++) {
        if (memcmp(file.data + i, "8BIMAnno", 8) == 0) {
            at = file.data + i + 8;
        }
    }
    CHECK(at != NULL);
    if (at == NULL) {
        pc_buf_fini(&file);
        return;
    }
    len = (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
    must(pc_io_parse_anno(at + 4, len, &found, &n, &nn, NULL));
    CHECK(found && nn == 2);
    CHECK(pc_str_eq_c(&n[0].author, "someone"));
    CHECK(pc_str_eq_c(&n[0].text, "Example annotation\nis here\n\n~ annotator\n"));
    CHECK(n[0].position[0] == 42.0 && n[0].position[1] == 28.0);
    CHECK(n[0].popup[0] == 92.0 && n[0].popup[1] == 74.0 && n[0].popup[2] == 332.0 && n[0].popup[3] == 214.0);
    CHECK(pc_str_eq_c(&n[0].modified, "D:20210521231029+01'00'"));
    CHECK(n[0].open);
    CHECK(pc_str_eq_c(&n[1].author, "some author") && pc_str_eq_c(&n[1].text, "open note"));
    pc_color_to_rgb(&n[1].color, rgb);
    CHECK(rgb[0] > 0.99f && rgb[1] < rgb[2] && rgb[2] < 0.7f);
    {
        pc_buf again = PC_BUF_INIT;
        uint32_t n0;
        pc_note *b = NULL;
        size_t nb = 0;
        must(pc_io_write_anno(n, 1, NULL, 0, &again, NULL));
        must(pc_io_parse_anno(again.data, again.len, &found, &b, &nb, NULL));
        CHECK(nb == 1 && pc_note_eq(&b[0], &n[0]));
        n0 = 8 + ((uint32_t)at[12] << 24 | (uint32_t)at[13] << 16 | (uint32_t)at[14] << 8 | at[15]);
        CHECK(again.len - 8 == n0 - 8 && memcmp(again.data + 8, at + 4 + 8, n0 - 8) == 0);
        pc_note_fini(&b[0]);
        free(b);
        pc_buf_fini(&again);
    }
    for (size_t i = 0; i < nn; i++) {
        pc_note_fini(&n[i]);
    }
    free(n);
    pc_buf_fini(&file);
}

static void test_export_blocks_keeps_replaces_and_drops(void)
{
    pc_document d = t_doc("n", 10, 10, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_doc_psd_global_block *out = NULL;
    size_t n = 0;
    pc_doc_psd_global_block *in;
    pc_note *back = NULL;
    size_t nb = 0;
    bool found = false;
    static const uint8_t bad[8] = {0, 2, 0, 1, 0, 0, 0, 9};
    must(pc_io_annotation_export_blocks(&d, NULL, 0, &out, &n, NULL));
    CHECK_EQ_UINT(n, 0);
    free(out);
    d.notes = xmalloc(sizeof *d.notes);
    d.notes[0] = note("h\xC3\xA9llo \xE2\x9C\x93", "me", 3.0, 4.0);
    d.nnotes = 1;
    must(pc_io_annotation_export_blocks(&d, NULL, 0, &out, &n, NULL));
    CHECK_EQ_UINT(n, 1);
    must(pc_io_parse_anno(pc_blob_data(out[0].data), pc_blob_len(out[0].data), &found, &back, &nb, NULL));
    CHECK(found && nb == 1 && pc_str_eq_c(&back[0].text, "h\xC3\xA9llo \xE2\x9C\x93"));
    pc_note_fini(&back[0]);
    free(back);
    /* Unchanged: the very same bytes. */
    d.metadata.psd_global_blocks = out;
    d.metadata.npsd_global_blocks = 1;
    in = xmalloc(sizeof *in);
    in[0] = out[0];
    pc_blob_retain(in[0].data);
    must(pc_io_annotation_export_blocks(&d, in, 1, &out, &n, NULL));
    CHECK(n == 1 && out[0].data == d.metadata.psd_global_blocks[0].data);
    m_global_blocks_free(out, n);
    pc_note_fini(&d.notes[0]);
    d.nnotes = 0;
    in = xmalloc(sizeof *in);
    in[0] = d.metadata.psd_global_blocks[0];
    pc_blob_retain(in[0].data);
    must(pc_io_annotation_export_blocks(&d, in, 1, &out, &n, NULL));
    CHECK_EQ_UINT(n, 0);
    free(out);
    must(pc_io_parse_anno(bad, 8, &found, &back, &nb, NULL));
    CHECK(!found);
    pc_document_fini(&d);
}

/* Issue #944: an Anno block that doesn't parse was taken for "no notes" and dropped on save. */
static void test_export_blocks_keeps_an_unreadable_block(void)
{
    pc_document d = t_doc("n", 10, 10, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    static const uint8_t bad[8] = {0, 2, 0, 1, 0, 0, 0, 9};
    pc_doc_psd_global_block *in;
    pc_doc_psd_global_block *out = NULL;
    size_t n = 0;
    pc_note *notes = NULL;
    size_t nn = 0;
    bool found = false;
    m_push_global(&d, "Anno", bad, 8);
    CHECK(pc_io_anno_unreadable(&d));
    must(pc_io_notes_from_blocks(&d, &notes, &nn, NULL));
    CHECK_EQ_UINT(nn, 0);
    free(notes);
    in = xmalloc(sizeof *in);
    in[0] = d.metadata.psd_global_blocks[0];
    pc_blob_retain(in[0].data);
    must(pc_io_annotation_export_blocks(&d, in, 1, &out, &n, NULL));
    CHECK(n == 1 && out[0].data == d.metadata.psd_global_blocks[0].data);
    m_global_blocks_free(out, n);
    /* A note added by the user still writes a fresh block in its place. */
    d.notes = xmalloc(sizeof *d.notes);
    d.notes[0] = note("new", "", 0.0, 0.0);
    d.nnotes = 1;
    in = xmalloc(sizeof *in);
    in[0] = d.metadata.psd_global_blocks[0];
    pc_blob_retain(in[0].data);
    must(pc_io_annotation_export_blocks(&d, in, 1, &out, &n, NULL));
    must(pc_io_parse_anno(pc_blob_data(out[0].data), pc_blob_len(out[0].data), &found, &notes, &nn, NULL));
    CHECK(found && nn == 1 && pc_str_eq_c(&notes[0].text, "new"));
    pc_note_fini(&notes[0]);
    free(notes);
    /* A readable block is not reported. */
    m_global_blocks_free(d.metadata.psd_global_blocks, d.metadata.npsd_global_blocks);
    d.metadata.psd_global_blocks = out;
    d.metadata.npsd_global_blocks = n;
    CHECK(!pc_io_anno_unreadable(&d));
    pc_document_fini(&d);
}

static void test_scale_resource_roundtrip_and_keep_rules(void)
{
    pc_measurement_scale s;
    pc_measurement_scale back;
    pc_buf b = PC_BUF_INIT;
    bool found = false;
    bool has = true;
    pc_document d = t_doc("n", 10, 10, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    static const uint8_t bad[5] = {0, 0, 0, 16, 1};
    memset(&s, 0, sizeof s);
    s.pixel_length = 150.0;
    s.logical_length = 2.0;
    must(pc_str_set_c(&s.units, "mm", NULL));
    must(pc_io_write_scale_resource(&s, &b, NULL));
    must(pc_io_scale_from_resource(b.data, b.len, &found, &back, NULL));
    CHECK(found && pc_measurement_scale_eq(&back, &s));
    pc_measurement_scale_fini(&back);
    must(pc_io_fresh_scale_resource(&d, &has, &b, NULL));
    CHECK(!has);
    /* An unparseable resource stays while the scale is untouched. */
    m_push_resource(&d, PC_IO_MEASUREMENT_SCALE, bad, 5);
    must(pc_io_fresh_scale_resource(&d, &has, &b, NULL));
    CHECK(pc_io_keep_resource(&d, PC_IO_MEASUREMENT_SCALE) && !has);
    pc_measurement_scale_fini(&d.measurement.scale);
    must(pc_measurement_scale_clone(&s, &d.measurement.scale, NULL));
    CHECK(!pc_io_keep_resource(&d, PC_IO_MEASUREMENT_SCALE));
    must(pc_io_fresh_scale_resource(&d, &has, &b, NULL));
    must(pc_io_scale_from_resource(b.data, b.len, &found, &back, NULL));
    CHECK(has && found && pc_measurement_scale_eq(&back, &s));
    pc_measurement_scale_fini(&back);
    CHECK(pc_io_keep_resource(&d, 1080));
    d.measurement.count_groups = xmalloc(sizeof *d.measurement.count_groups);
    must(pc_count_group_default(&d.measurement.count_groups[0], NULL));
    d.measurement.count_groups[0].points = xmalloc(sizeof *d.measurement.count_groups[0].points);
    d.measurement.count_groups[0].points[0][0] = 1.0;
    d.measurement.count_groups[0].points[0][1] = 1.0;
    d.measurement.count_groups[0].npoints = 1;
    d.measurement.ncount_groups = 1;
    CHECK(!pc_io_keep_resource(&d, 1080));
    pc_measurement_scale_fini(&s);
    pc_buf_fini(&b);
    pc_document_fini(&d);
}

/* ---------------------------------------------------------------- gradient_bake */

static pc_io_color_stop stop(float at, pc_color c)
{
    pc_io_color_stop s;
    s.at = at;
    s.color = c;
    return s;
}

/* Linear interpolation of baked stops at t (the compositors' reading). */
static void at_t(const pc_io_color_stop *s, size_t n, float t, float out[3])
{
    for (size_t i = 0; i + 1 < n; i++) {
        if (t >= s[i].at && t <= s[i + 1].at) {
            float u = (t - s[i].at) / (s[i + 1].at - s[i].at);
            float x[3];
            float y[3];
            pc_color_to_rgb(&s[i].color, x);
            pc_color_to_rgb(&s[i + 1].color, y);
            for (size_t c = 0; c < 3; c++) {
                out[c] = x[c] + (y[c] - x[c]) * u;
            }
            return;
        }
    }
    abort();
}

static void test_gradient_bake(void)
{
    pc_io_color_stop bw[2] = {stop(0.0f, PC_COLOR_BLACK), stop(1.0f, PC_COLOR_WHITE)};
    float half = 0.5f;
    float quarter = 0.25f;
    pc_io_color_stop *out = NULL;
    size_t n = 0;
    float p[3];
    /* classic_without_smoothness_is_untouched */
    must(pc_io_gradient_bake(bw, 2, &half, 1, 0.0f, PC_IO_BAKE_CLASSIC, &out, &n, NULL));
    CHECK(n == 2 && memcmp(out, bw, sizeof bw) == 0);
    free(out);
    /* smoothness_eases_between_two_stops */
    must(pc_io_gradient_bake(bw, 2, &half, 1, 1.0f, PC_IO_BAKE_CLASSIC, &out, &n, NULL));
    {
        static const float ts[5] = {0.1f, 0.25f, 0.5f, 0.75f, 0.9f};
        for (size_t i = 0; i < 5; i++) {
            float t = ts[i];
            float ss = t * t * (3.0f - 2.0f * t);
            at_t(out, n, t, p);
            CHECK(fabsf(p[0] - (t + 0.5f * (ss - t))) < 2e-3f);
        }
    }
    free(out);
    /* perceptual_matches_photoshop_sample */
    {
        pc_io_color_stop ob[2] = {stop(0.0f, pc_color_rgb(0.882f, 0.602f, 0.201f)),
                                  stop(1.0f, pc_color_rgb(0.164f, 0.568f, 0.851f))};
        static const float want1[3] = {0.592f, 0.612f, 0.596f};
        static const float want2[3] = {0.839f, 0.608f, 0.290f};
        must(pc_io_gradient_bake(ob, 2, &half, 1, 1.0f, PC_IO_BAKE_PERCEPTUAL, &out, &n, NULL));
        at_t(out, n, 0.491f, p);
        for (size_t c = 0; c < 3; c++) {
            CHECK(fabsf(p[c] - want1[c]) < 0.01f);
        }
        at_t(out, n, 0.113f, p);
        for (size_t c = 0; c < 3; c++) {
            CHECK(fabsf(p[c] - want2[c]) < 0.012f);
        }
        free(out);
    }
    /* classic_lab_stops_interpolate_in_lab */
    {
        pc_color a = {PC_COLOR_MODE_LAB,
                      {19.07f / 100.0f, (52.29f + 128.0f) / 255.0f, (-85.08f + 128.0f) / 255.0f, 0},
                      1};
        pc_color b = {PC_COLOR_MODE_LAB,
                      {54.29f / 100.0f, (80.81f + 128.0f) / 255.0f, (69.91f + 128.0f) / 255.0f, 0},
                      1};
        pc_color want = {PC_COLOR_MODE_LAB,
                         {36.68f / 100.0f, (66.55f + 128.0f) / 255.0f, (-7.585f + 128.0f) / 255.0f, 0},
                         1};
        pc_io_color_stop lab[2] = {stop(0.0f, a), stop(1.0f, b)};
        const pc_io_color_stop *mid = NULL;
        must(pc_io_gradient_bake(lab, 2, &half, 1, 0.0f, PC_IO_BAKE_CLASSIC, &out, &n, NULL));
        CHECK(n > 2);
        for (size_t i = 0; i < n; i++) {
            CHECK(out[i].color.mode == PC_COLOR_MODE_LAB);
            if (fabsf(out[i].at - 0.5f) < 1e-6f) {
                mid = &out[i];
            }
        }
        CHECK(mid != NULL);
        for (size_t c = 0; mid != NULL && c < 3; c++) {
            CHECK(fabsf(mid->color.c[c] - want.c[c]) < 1e-4f);
        }
        free(out);
    }
    /* midpoint_shifts_the_half_way_colour */
    must(pc_io_gradient_bake(bw, 2, &quarter, 1, 0.0f, PC_IO_BAKE_CLASSIC, &out, &n, NULL));
    at_t(out, n, 0.25f, p);
    CHECK(fabsf(p[0] - 0.5f) < 0.01f);
    free(out);
    CHECK(pc_io_bake_method_from_code((const uint8_t *)"Perc", 4) == PC_IO_BAKE_PERCEPTUAL);
    CHECK(pc_io_bake_method_from_code((const uint8_t *)"Lnr ", 4) == PC_IO_BAKE_LINEAR);
    CHECK(pc_io_bake_method_from_code(NULL, 0) == PC_IO_BAKE_CLASSIC);
}

/* ---------------------------------------------------------------- pattern_map */

static void test_patterns_round_trip_through_psd_at_every_depth_and_model(void)
{
    static const pc_color_mode modes[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB};
    static const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t m = 0; m < 4; m++) {
        for (size_t s = 0; s < 3; s++) {
            for (int a = 0; a < 2; a++) {
                pc_pattern p = m_pattern(modes[m], samples[s], a != 0);
                pc_psd_pattern psd;
                pc_pattern back;
                bool found = false;
                float *x = NULL;
                float *y = NULL;
                size_t nx = 0;
                size_t ny = 0;
                must(pc_io_pattern_to_psd(&p, &psd, NULL));
                must(pc_io_pattern_from_psd(&psd, &found, &back, NULL));
                CHECK(found && pc_str_eq(&back.id, &p.id) && pc_str_eq(&back.name, &p.name) &&
                      back.width == p.width &&
                      pc_pixel_format_eq(pc_surface_format(&back.surface), pc_surface_format(&p.surface)));
                must(pc_surface_read_region(&back.surface, pc_pattern_rect(&p), &x, &nx, NULL));
                must(pc_surface_read_region(&p.surface, pc_pattern_rect(&p), &y, &ny, NULL));
                CHECK(nx == ny);
                for (size_t i = 0; i < nx && i < ny; i++) {
                    CHECK(fabsf(x[i] - y[i]) < 1e-6f);
                }
                if (!(modes[m] == PC_COLOR_MODE_CMYK && samples[s] == PC_SAMPLE_TYPE_F32)) {
                    CHECK(pc_pattern_eq(&back, &p));
                }
                free(x);
                free(y);
                pc_pattern_fini(&back);
                pc_psd_pattern_fini(&psd);
                pc_pattern_fini(&p);
            }
        }
    }
}

static void test_export_reuses_raw_blocks_until_patterns_change(void)
{
    pc_document d = t_doc("x", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_pattern p = m_pattern(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    pc_psd_pattern psd;
    pc_psd_bytes raw;
    pc_doc_psd_global_block *out = NULL;
    size_t n = 0;
    static const uint8_t one = 1;
    must(pc_io_pattern_to_psd(&p, &psd, NULL));
    must(pc_psd_write_pattern_block(&psd, 1, &raw, NULL));
    m_push_global(&d, "Txt2", &one, 1);
    m_push_global(&d, "Patt", raw.data, raw.len);
    must(pc_io_patterns_from_global_blocks(&d, &d.patterns, &d.npatterns, NULL));
    CHECK_EQ_UINT(d.npatterns, 1);
    must(pc_io_export_pattern_blocks(&d, &out, &n, NULL));
    CHECK(n == 2 && out[0].data == d.metadata.psd_global_blocks[0].data &&
          out[1].data == d.metadata.psd_global_blocks[1].data);
    m_global_blocks_free(out, n);
    d.patterns = realloc(d.patterns, 2 * sizeof *d.patterns);
    d.patterns[1] = m_pattern(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true);
    d.npatterns = 2;
    must(pc_io_export_pattern_blocks(&d, &out, &n, NULL));
    CHECK(n == 2 && memcmp(out[1].key, "Patt", 4) == 0);
    {
        pc_document d2;
        pc_pattern *back = NULL;
        size_t nb = 0;
        pc_buf pat = PC_BUF_INIT;
        must(pc_document_clone(&d, &d2, NULL));
        m_global_blocks_free(d2.metadata.psd_global_blocks, d2.metadata.npsd_global_blocks);
        d2.metadata.psd_global_blocks = out;
        d2.metadata.npsd_global_blocks = n;
        must(pc_io_patterns_from_global_blocks(&d2, &back, &nb, NULL));
        CHECK(nb == 2 && pc_pattern_eq(&back[0], &d.patterns[0]) && pc_pattern_eq(&back[1], &d.patterns[1]));
        pc_io_patterns_free(back, nb);
        must(pc_io_write_pat(d.patterns, 2, &pat, NULL));
        must(pc_io_read_pat(pat.data, pat.len, &back, &nb, NULL));
        CHECK(nb == 2 && pc_pattern_eq(&back[0], &d.patterns[0]) && pc_pattern_eq(&back[1], &d.patterns[1]));
        pc_io_patterns_free(back, nb);
        pc_buf_fini(&pat);
        pc_document_fini(&d2);
    }
    pc_psd_bytes_fini(&raw);
    pc_psd_pattern_fini(&psd);
    pc_pattern_fini(&p);
    pc_document_fini(&d);
}

/* ---------------------------------------------------------------- slices_map */

static void test_slices_survive_a_psd_round_trip(void)
{
    pc_document d = t_doc("site.psd", 120, 90, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer l;
    pc_layer_id lid;
    uint32_t five = 5;
    pc_io_layer_id_map ids;
    pc_io_slices_action act;
    pc_buf out = PC_BUF_INIT;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_document back;
    bool same = false;
    must(pc_layer_raster(&l, "Logo", pc_document_pixel_format(&d), NULL));
    l.has_psd_id = true;
    l.psd_id = 5;
    lid = l.id;
    t_push(&d, &l);
    d.slices.list = xmalloc(2 * sizeof *d.slices.list);
    d.slices.len = 2;
    pc_slice_default(&d.slices.list[0]);
    d.slices.list[0].id = 1;
    must(pc_str_set_c(&d.slices.list[0].name, "hero", NULL));
    d.slices.list[0].rect = pc_rect_make(10, 10, 60, 40);
    must(pc_str_set_c(&d.slices.list[0].url, "https://example.org", NULL));
    must(pc_str_set_c(&d.slices.list[0].alt, "Hero", NULL));
    pc_slice_default(&d.slices.list[1]);
    d.slices.list[1].id = 2;
    d.slices.list[1].origin = PC_SLICE_LAYER;
    d.slices.list[1].has_layer = true;
    d.slices.list[1].layer = lid;
    d.slices.list[1].rect = pc_rect_make(70, 50, 100, 80);
    d.slices.list[1].kind = PC_SLICE_NO_IMAGE;
    for (size_t i = 0; i < 4; i++) {
        d.slices.list[1].outsets[i] = 1;
    }
    ids.ids = &lid;
    ids.psd = &five;
    ids.n = 1;
    must(pc_io_slices_export_resource(&d, &ids, &act, &out, &w, NULL));
    CHECK(act == PC_IO_SLICES_WRITE && w.n == 0);
    must(pc_document_clone(&d, &back, NULL));
    pc_slices_fini(&back.slices);
    memset(&back.slices, 0, sizeof back.slices);
    m_push_resource(&back, PC_IO_SLICES, out.data, out.len);
    must(pc_io_slices_import(&back, NULL));
    CHECK(back.slices.len == 2 && pc_slice_eq(&back.slices.list[0], &d.slices.list[0]) &&
          pc_slice_eq(&back.slices.list[1], &d.slices.list[1]));
    must(pc_io_slices_unchanged(&back, &same, NULL));
    CHECK(same);
    must(pc_io_slices_export_resource(&back, &ids, &act, &out, &w, NULL));
    CHECK(act == PC_IO_SLICES_KEEP && w.n == 0);
    pc_buf_fini(&out);
    pc_document_fini(&back);
    pc_document_fini(&d);
}

static void test_auto_slice_ids_do_not_collide_with_maximal_stored_id(void)
{
    pc_document d = t_doc("site.psd", 120, 90, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_io_slices_action act;
    pc_buf out = PC_BUF_INIT;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_psd_slices r;
    bool has_max = false;
    bool has_one = false;
    d.slices.list = xmalloc(sizeof *d.slices.list);
    d.slices.len = 1;
    pc_slice_default(&d.slices.list[0]);
    d.slices.list[0].id = UINT32_MAX;
    d.slices.list[0].rect = pc_rect_make(10, 10, 20, 20);
    must(pc_io_slices_export_resource(&d, NULL, &act, &out, &w, NULL));
    CHECK(act == PC_IO_SLICES_WRITE && w.n == 0);
    must(pc_psd_slices_parse(out.data, out.len, &r, NULL));
    for (size_t i = 0; i < r.n; i++) {
        has_max = has_max || r.slices[i].id == UINT32_MAX;
        has_one = has_one || r.slices[i].id == 1;
        for (size_t k = 0; k < i; k++) {
            CHECK(r.slices[k].id != r.slices[i].id);
        }
    }
    CHECK(has_max && has_one);
    pc_psd_slices_fini(&r);
    pc_buf_fini(&out);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_path_records_roundtrip);
    RUN(test_layer_path_semantics_follow_photoshop);
    RUN(test_vector_mask_roundtrip_with_flags);
    RUN(test_vstk_roundtrip);
    RUN(test_vogk_roundtrip);
    RUN(test_shape_blocks_keep_unchanged_and_regenerate_changed);
    RUN(test_artboard_roundtrip);
    RUN(test_cmls_setting_roundtrip_and_shmd_merge);
    RUN(test_unrepresentable_comp_offsets_are_dropped_not_wrapped);
    RUN(test_unrepresentable_comp_positions_are_written_without_offset);
    RUN(test_annotations_sample_byte_exact);
    RUN(test_export_blocks_keeps_replaces_and_drops);
    RUN(test_export_blocks_keeps_an_unreadable_block);
    RUN(test_scale_resource_roundtrip_and_keep_rules);
    RUN(test_gradient_bake);
    RUN(test_patterns_round_trip_through_psd_at_every_depth_and_model);
    RUN(test_export_reuses_raw_blocks_until_patterns_change);
    RUN(test_slices_survive_a_psd_round_trip);
    RUN(test_auto_slice_ids_do_not_collide_with_maximal_stored_id);
    return pc_t_finish();
}
