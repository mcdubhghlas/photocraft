/* Chunk I4's maps through whole PSD files (ports of crates/io/tests/annotations.rs,
 * artboard_guide_order.rs, comps_artboards.rs and vector.rs): notes and the measurement scale,
 * artboards and their guide indices, layer comps, saved paths, shape layers and vector masks,
 * byte-exact while unchanged and regenerated after edits. Corpus tests skip without corpus/. */
#include "harness.h"

#include <dirent.h>
#include <strings.h>

#include "base/num.h"
#include "io/maps_desc.h"
#include "maps.h"
#include "psd/file.h"
#include "psd/layer.h"
#include "vector/vector.h"

/* ---------------------------------------------------------------- helpers */

static pc_buf to_psd(const pc_document *d)
{
    pc_io_export_result r;
    pc_buf b = PC_BUF_INIT;
    must(pc_io_export(d, "x.psd", NULL, &r, NULL));
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    pc_io_export_result_fini(&r);
    return b;
}

static pc_document from_psd_w(const pc_buf *b, pc_io_warnings *w)
{
    pc_io_import_result r;
    pc_document d;
    must(pc_io_import("x.psd", b->data, b->len, &r, NULL));
    d = r.document;
    memset(&r.document, 0, sizeof r.document);
    if (w != NULL) {
        must(pc_io_warnings_append(w, &r.warnings, NULL));
    }
    pc_io_warnings_fini(&r.warnings);
    return d;
}

static pc_document from_psd(const pc_buf *b)
{
    return from_psd_w(b, NULL);
}

static pc_document roundtrip(const pc_document *d)
{
    pc_buf b = to_psd(d);
    pc_document out = from_psd(&b);
    pc_buf_fini(&b);
    return out;
}

static pc_psd_file parse(const pc_buf *b)
{
    pc_psd_file f;
    must(pc_psd_file_parse(b->data, b->len, &f, NULL));
    return f;
}

static const pc_psd_block *global_block(const pc_psd_file *f, const char *key)
{
    for (size_t i = 0; i < f->nglobal; i++) {
        if (memcmp(f->global_blocks[i].key, key, 4) == 0) {
            return &f->global_blocks[i];
        }
    }
    return NULL;
}

static const pc_psd_resource *resource(const pc_psd_file *f, uint16_t id, size_t *count)
{
    const pc_psd_resource *r = NULL;
    size_t n = 0;
    for (size_t i = 0; i < f->nresources; i++) {
        if (f->resources[i].id == id) {
            r = r != NULL ? r : &f->resources[i];
            n++;
        }
    }
    if (count != NULL) {
        *count = n;
    }
    return r;
}

static bool layer_named(const pc_psd_layer *l, const char *name)
{
    char *n = NULL;
    bool eq;
    must(pc_psd_layer_name(l, &n, NULL));
    eq = strcmp(n, name) == 0;
    free(n);
    return eq;
}

static const pc_psd_block *layer_block(const pc_psd_file *f, const char *name, const char *key)
{
    for (size_t i = 0; f->has_layer_info && i < f->layer_info.n; i++) {
        if (layer_named(&f->layer_info.layers[i], name)) {
            return pc_psd_layer_block(&f->layer_info.layers[i], key);
        }
    }
    return NULL;
}

static bool bytes_eq(const pc_psd_bytes *a, const pc_psd_bytes *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}

static bool corpus_read(const char *path, pc_buf *out)
{
    if (pc_file_read(path, (size_t)1 << 32, out, NULL) != PC_OK) {
        (void)printf("  (corpus missing: %s skipped)\n", path);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- annotations.rs */

static void test_corpus_notes_import_and_verbatim_export(void)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_psd_file src;
    pc_document doc;
    pc_buf out;
    pc_psd_file f;
    pc_document back;
    if (!corpus_read("corpus/psd/ag-psd/read-write/annotations/src.psd", &bytes)) {
        return;
    }
    src = parse(&bytes);
    doc = from_psd(&bytes);
    CHECK_EQ_UINT(doc.nnotes, 2);
    CHECK(pc_str_eq_c(&doc.notes[1].text, "open note"));
    out = to_psd(&doc);
    f = parse(&out);
    CHECK(global_block(&f, "Anno") != NULL &&
          bytes_eq(&global_block(&f, "Anno")->data, &global_block(&src, "Anno")->data));
    pc_psd_file_fini(&f);
    pc_buf_fini(&out);
    /* Edit a note: the block is regenerated and reads back with the change. */
    must(pc_str_set_c(&doc.notes[0].text, "changed", NULL));
    doc.notes = realloc(doc.notes, 3 * sizeof *doc.notes);
    pc_note_default(&doc.notes[2]);
    must(pc_str_set_c(&doc.notes[2].author, "agent", NULL));
    must(pc_str_set_c(&doc.notes[2].text, "third", NULL));
    doc.notes[2].position[0] = 5.0;
    doc.notes[2].position[1] = 6.0;
    doc.nnotes = 3;
    back = roundtrip(&doc);
    CHECK(back.nnotes == 3 && pc_str_eq_c(&back.notes[0].text, "changed") &&
          back.notes[2].position[0] == 5.0 && back.notes[2].position[1] == 6.0);
    pc_document_fini(&back);
    /* Deleting every note drops the block. */
    for (size_t i = 0; i < doc.nnotes; i++) {
        pc_note_fini(&doc.notes[i]);
    }
    doc.nnotes = 0;
    out = to_psd(&doc);
    f = parse(&out);
    CHECK(global_block(&f, "Anno") == NULL);
    pc_psd_file_fini(&f);
    pc_buf_fini(&out);
    pc_document_fini(&doc);
    pc_psd_file_fini(&src);
    pc_buf_fini(&bytes);
}

static void test_measurement_scale_roundtrips_through_psd(void)
{
    static const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_document doc;
        pc_buf out;
        pc_psd_file f;
        pc_document back;
        size_t count = 0;
        must(pc_document_with_background(&doc, "m", pc_size_make(16, 8), PC_COLOR_MODE_RGB, depths[k],
                                         pc_color_rgb(1.0f, 1.0f, 1.0f), NULL));
        out = to_psd(&doc);
        f = parse(&out);
        CHECK(resource(&f, PC_IO_MEASUREMENT_SCALE, NULL) == NULL);
        pc_psd_file_fini(&f);
        pc_buf_fini(&out);
        doc.measurement.scale.pixel_length = 120.0;
        doc.measurement.scale.logical_length = 3.0;
        must(pc_str_set_c(&doc.measurement.scale.units, "cm", NULL));
        doc.notes = xmalloc(sizeof *doc.notes);
        pc_note_default(&doc.notes[0]);
        must(pc_str_set_c(&doc.notes[0].text, "n", NULL));
        doc.nnotes = 1;
        back = roundtrip(&doc);
        CHECK(pc_measurement_scale_eq(&back.measurement.scale, &doc.measurement.scale));
        CHECK(back.nnotes == 1 && pc_str_eq_c(&back.notes[0].text, "n") && back.notes[0].position[0] == 0.0 &&
              back.notes[0].position[1] == 0.0);
        /* Unchanged -> the preserved resource is written byte-exact (once). */
        out = to_psd(&back);
        f = parse(&out);
        (void)resource(&f, PC_IO_MEASUREMENT_SCALE, &count);
        CHECK_EQ_UINT(count, 1);
        pc_psd_file_fini(&f);
        pc_buf_fini(&out);
        pc_document_fini(&back);
        pc_document_fini(&doc);
    }
}

static void be32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    must(pc_buf_append(b, x, 4, NULL));
}

static void be16(pc_buf *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    must(pc_buf_append(b, x, 2, NULL));
}

/* One text note whose icon is [top, left, 0, 0] and whose popup is all zeros (#939's fixture). */
static pc_buf anno_with_icon(int32_t top, int32_t left)
{
    pc_buf e = PC_BUF_INIT;
    pc_buf out = PC_BUF_INIT;
    static const uint8_t text[4] = {0xFE, 0xFF, 0x00, 'A'};
    static const uint8_t six_zero[6] = {0, 0, 0, 0, 0, 0};
    must(pc_buf_append(&e, "txtA", 4, NULL));
    must(pc_buf_append_byte(&e, 1, NULL));
    must(pc_buf_append_byte(&e, 0x1C, NULL));
    be16(&e, 1);
    be32(&e, (uint32_t)(int64_t)top);
    be32(&e, (uint32_t)(int64_t)left);
    be32(&e, 0);
    be32(&e, 0);
    for (int i = 0; i < 4; i++) {
        be32(&e, 0);
    }
    be16(&e, 0);
    for (int i = 0; i < 4; i++) {
        be16(&e, 0);
    }
    must(pc_buf_append(&e, six_zero, 6, NULL));
    be32(&e, 4 + 12);
    must(pc_buf_append(&e, "txtC", 4, NULL));
    be32(&e, 4);
    must(pc_buf_append(&e, text, 4, NULL));
    be16(&out, 2);
    be16(&out, 1);
    be32(&out, 1);
    be32(&out, (uint32_t)e.len + 4);
    must(pc_buf_append(&out, e.data, e.len, NULL));
    pc_buf_fini(&e);
    return out;
}

static pc_buf psd_with_anno(const pc_buf *anno)
{
    pc_document doc = t_doc("notes", 16, 16, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer l;
    pc_buf b;
    pc_psd_file f;
    pc_psd_block *g;
    pc_psd_bytes out;
    pc_buf r = PC_BUF_INIT;
    size_t pad = (4 - anno->len % 4) % 4;
    must(pc_layer_raster(&l, "Background", pc_document_pixel_format(&doc), NULL));
    t_push(&doc, &l);
    b = to_psd(&doc);
    f = parse(&b);
    f.global_blocks = realloc(f.global_blocks, (f.nglobal + 1) * sizeof *f.global_blocks);
    g = &f.global_blocks[f.nglobal++];
    memset(g, 0, sizeof *g);
    memcpy(g->signature, "8BIM", 4);
    memcpy(g->key, "Anno", 4);
    must(pc_psd_bytes_set(&g->data, anno->data, anno->len, NULL));
    g->padding.has = true;
    g->padding.v.data = calloc(pad + 1, 1);
    g->padding.v.len = pad;
    must(pc_psd_file_to_bytes(&f, &out, NULL));
    must(pc_buf_append(&r, out.data, out.len, NULL));
    pc_psd_bytes_fini(&out);
    pc_psd_file_fini(&f);
    pc_buf_fini(&b);
    pc_document_fini(&doc);
    return r;
}

static void exported_icon(const pc_document *doc, int32_t out[4])
{
    pc_buf b = to_psd(doc);
    pc_psd_file f = parse(&b);
    const pc_psd_block *a = global_block(&f, "Anno");
    for (size_t i = 0; i < 4; i++) {
        const uint8_t *p = a->data.data + 20 + i * 4;
        out[i] = pc_u32_as_i32((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
    }
    pc_psd_file_fini(&f);
    pc_buf_fini(&b);
}

/* Issue #939: a note icon near i32::MAX saves with saturated (representable) geometry. */
static void test_a_note_icon_near_i32_max_saves_with_representable_geometry(void)
{
    static const int32_t cases[3][2] = {{INT32_MAX - 5, 0}, {0, INT32_MAX - 3}, {INT32_MIN, INT32_MIN}};
    pc_document doc;
    int32_t icon[4];
    for (size_t k = 0; k < 3; k++) {
        int32_t top = cases[k][0];
        int32_t left = cases[k][1];
        pc_buf anno = anno_with_icon(top, left);
        pc_buf file = psd_with_anno(&anno);
        pc_document imp = from_psd(&file);
        pc_document back;
        CHECK(imp.nnotes == 1 && imp.notes[0].position[0] == (double)left &&
              imp.notes[0].position[1] == (double)top);
        exported_icon(&imp, icon);
        CHECK(icon[0] == top && icon[1] == left && icon[2] == pc_i32_sat_add(top, 20) &&
              icon[3] == pc_i32_sat_add(left, 16));
        back = roundtrip(&imp);
        CHECK(back.nnotes == 1 && pc_str_eq_c(&back.notes[0].text, "A") &&
              back.notes[0].position[0] == (double)left && back.notes[0].position[1] == (double)top);
        pc_document_fini(&back);
        pc_document_fini(&imp);
        pc_buf_fini(&file);
        pc_buf_fini(&anno);
    }
    doc = t_doc("notes", 16, 16, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    doc.notes = xmalloc(sizeof *doc.notes);
    pc_note_default(&doc.notes[0]);
    must(pc_str_set_c(&doc.notes[0].text, "far", NULL));
    doc.notes[0].position[0] = 1e300;
    doc.notes[0].position[1] = INFINITY;
    doc.nnotes = 1;
    exported_icon(&doc, icon);
    CHECK(icon[0] == INT32_MAX && icon[1] == INT32_MAX && icon[2] == INT32_MAX && icon[3] == INT32_MAX);
    doc.notes[0].position[0] = NAN;
    doc.notes[0].position[1] = -1e300;
    exported_icon(&doc, icon);
    CHECK(icon[0] == INT32_MIN && icon[1] == 0 && icon[2] == INT32_MIN + 20 && icon[3] == 16);
    doc.notes[0].position[0] = 42.0;
    doc.notes[0].position[1] = 28.0;
    exported_icon(&doc, icon);
    CHECK(icon[0] == 28 && icon[1] == 42 && icon[2] == 48 && icon[3] == 58);
    {
        pc_document back = roundtrip(&doc);
        CHECK(back.notes[0].position[0] == 42.0 && back.notes[0].position[1] == 28.0);
        pc_document_fini(&back);
    }
    pc_document_fini(&doc);
}

/* Issue #944: an unreadable Anno block opens with a warning and is written back as is. */
static void test_an_unreadable_anno_block_warns_and_survives_a_save(void)
{
    pc_buf broken = anno_with_icon(28, 42);
    pc_buf file;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_document imp;
    pc_buf out;
    pc_psd_file f;
    broken.data[7] = 2; /* declares two annotations, holds one */
    file = psd_with_anno(&broken);
    imp = from_psd_w(&file, &w);
    CHECK_EQ_UINT(imp.nnotes, 0);
    CHECK(pc_io_warnings_any(&w, "Anno"));
    out = to_psd(&imp);
    f = parse(&out);
    CHECK(global_block(&f, "Anno") != NULL && global_block(&f, "Anno")->data.len == broken.len &&
          memcmp(global_block(&f, "Anno")->data.data, broken.data, broken.len) == 0);
    pc_psd_file_fini(&f);
    pc_buf_fini(&out);
    pc_document_fini(&imp);
    pc_io_warnings_fini(&w);
    pc_buf_fini(&file);
    pc_buf_fini(&broken);
    {
        pc_buf ok = anno_with_icon(28, 42);
        pc_buf okf = psd_with_anno(&ok);
        pc_document d = from_psd_w(&okf, &w);
        CHECK(d.nnotes == 1 && !pc_io_warnings_any(&w, "Anno"));
        pc_document_fini(&d);
        pc_io_warnings_fini(&w);
        pc_buf_fini(&okf);
        pc_buf_fini(&ok);
    }
}

/* ---------------------------------------------------------------- artboard_guide_order.rs */

static void set_guides(pc_guides *g, float h, float v)
{
    g->horizontal = xmalloc(sizeof *g->horizontal);
    g->horizontal[0] = h;
    g->nhorizontal = 1;
    g->vertical = xmalloc(sizeof *g->vertical);
    g->vertical[0] = v;
    g->nvertical = 1;
}

static void test_guides_in_numbers_vertical_first_like_resource_1032(void)
{
    pc_guides g;
    int32_t *v = NULL;
    size_t n = 0;
    memset(&g, 0, sizeof g);
    set_guides(&g, 90.0f, 20.0f);
    must(pc_io_guides_in(&g, pc_rect_make(10, 0, 30, 50), &v, &n, NULL));
    CHECK(n == 1 && v[0] == 0);
    free(v);
    pc_guides_fini(&g);
    set_guides(&g, 50.0f, 50.0f);
    must(pc_io_guides_in(&g, pc_rect_make(0, 0, 100, 100), &v, &n, NULL));
    CHECK(n == 2 && v[0] == 0 && v[1] == 1);
    free(v);
    pc_guides_fini(&g);
}

static pc_layer board(const char *name, pc_layer *kids, size_t n, pc_rect r, pc_artboard_bg_kind bg,
                      pc_color c, const char *preset)
{
    pc_layer g;
    must(pc_layer_group(&g, name, kids, n, NULL));
    g.content.u.group.has_artboard = true;
    pc_artboard_init(&g.content.u.group.artboard, r);
    g.content.u.group.artboard.background.kind = bg;
    g.content.u.group.artboard.background.custom = c;
    must(pc_str_set_c(&g.content.u.group.artboard.preset, preset, NULL));
    return g;
}

static void test_exported_artboard_indices_match_guides_resource(void)
{
    pc_document d = t_doc("s", 100, 100, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer *kids = xmalloc(sizeof *kids);
    pc_layer g;
    pc_buf b;
    pc_psd_file f;
    const pc_psd_resource *res;
    const pc_psd_block *artb;
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    must(pc_layer_raster(&kids[0], "A", PC_PIXEL_FORMAT_RGBA8, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&kids[0]), pc_rect_make(1, 1, 2, 2), red, 4, NULL));
    g = board("Board", kids, 1, pc_rect_make(10, 0, 30, 50), PC_ARTBOARD_WHITE, PC_COLOR_WHITE, "");
    t_push(&d, &g);
    set_guides(&d.guides, 90.0f, 20.0f);
    b = to_psd(&d);
    f = parse(&b);
    res = resource(&f, 1032, NULL);
    CHECK(res != NULL);
    if (res != NULL) {
        const uint8_t *p = res->data.data;
        uint32_t n = (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16 | (uint32_t)p[14] << 8 | p[15];
        CHECK(n == 2 && p[16 + 4] == 0 && p[16 + 5 + 4] == 1);
    }
    artb = layer_block(&f, "Board", "artb");
    CHECK(artb != NULL);
    if (artb != NULL) {
        const uint8_t *h = artb->data.data;
        size_t pos = SIZE_MAX;
        for (size_t i = 0; i + 12 <= artb->data.len && pos == SIZE_MAX; i++) {
            if (memcmp(h + i, "guideIndeces", 12) == 0) {
                pos = i;
            }
        }
        CHECK(pos != SIZE_MAX);
        if (pos != SIZE_MAX) {
            uint32_t count = (uint32_t)h[pos + 16] << 24 | (uint32_t)h[pos + 17] << 16 |
                             (uint32_t)h[pos + 18] << 8 | h[pos + 19];
            uint32_t idx = (uint32_t)h[pos + 24] << 24 | (uint32_t)h[pos + 25] << 16 |
                           (uint32_t)h[pos + 26] << 8 | h[pos + 27];
            CHECK(count == 1 && idx == 0);
        }
    }
    pc_psd_file_fini(&f);
    pc_buf_fini(&b);
    pc_document_fini(&d);
}

/* ---------------------------------------------------------------- comps_artboards.rs */

static const pc_layer *walk_layer(const pc_document *d, pc_layer_id id)
{
    return pc_document_layer(d, id);
}

static void test_corpus_layer_comps_import_and_verbatim_export(void)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_psd_file src;
    pc_document doc;
    pc_buf out;
    pc_psd_file f;
    static const char *const names[5] = {"Layer Comp 1", "Layer Comp 2", "Layer Comp 3", "Layer Comp 4",
                                         "Layer Comp 5"};
    if (!corpus_read("corpus/psd/ag-psd/read-write/layer-comps/src.psd", &bytes)) {
        return;
    }
    src = parse(&bytes);
    doc = from_psd(&bytes);
    CHECK_EQ_UINT(doc.nlayer_comps, 5);
    for (size_t i = 0; i < 5 && i < doc.nlayer_comps; i++) {
        CHECK(pc_str_eq_c(&doc.layer_comps[i].name, names[i]));
    }
    CHECK(pc_str_eq_c(&doc.layer_comps[3].comment, "some comments"));
    CHECK(pc_layer_comp_captured_info(&doc.layer_comps[1]) == 1 &&
          pc_layer_comp_captured_info(&doc.layer_comps[4]) == 7);
    CHECK(doc.has_last_applied_comp && doc.last_applied_comp == doc.layer_comps[3].id);
    {
        bool hidden = false;
        size_t moved = 0;
        const pc_comp_layer_state *m = NULL;
        for (size_t i = 0; i < doc.layer_comps[1].nstates; i++) {
            hidden =
                hidden || (doc.layer_comps[1].states[i].has_visible && !doc.layer_comps[1].states[i].visible);
        }
        CHECK(hidden);
        for (size_t i = 0; i < doc.layer_comps[0].nstates; i++) {
            if (doc.layer_comps[0].states[i].has_position) {
                moved++;
                m = &doc.layer_comps[0].states[i];
            }
        }
        CHECK(moved == 1);
        if (m != NULL) {
            int32_t here[2];
            CHECK(pc_comp_layer_position(walk_layer(&doc, m->layer), here));
            CHECK(m->position[0] == here[0] - 48 && m->position[1] == here[1] - 35);
        }
    }
    /* Unchanged: resource 1065 and every layer's shmd are written back byte for byte. */
    out = to_psd(&doc);
    f = parse(&out);
    CHECK(bytes_eq(&resource(&f, PC_IO_LAYER_COMPS, NULL)->data,
                   &resource(&src, PC_IO_LAYER_COMPS, NULL)->data));
    for (size_t i = 0; i < src.layer_info.n; i++) {
        const pc_psd_block *s = pc_psd_layer_block(&src.layer_info.layers[i], "shmd");
        char *name = NULL;
        const pc_psd_block *o;
        if (s == NULL) {
            continue;
        }
        must(pc_psd_layer_name(&src.layer_info.layers[i], &name, NULL));
        o = layer_block(&f, name, "shmd");
        CHECK(o != NULL && bytes_eq(&o->data, &s->data));
        free(name);
    }
    pc_psd_file_fini(&f);
    pc_buf_fini(&out);
    /* Edited: regenerated, and it decodes back to the edited comps. */
    {
        pc_document edited;
        pc_document back;
        must(pc_document_clone(&doc, &edited, NULL));
        must(pc_str_set_c(&edited.layer_comps[0].name, "Renamed", NULL));
        pc_layer_comp_fini(&edited.layer_comps[2]);
        memmove(&edited.layer_comps[2], &edited.layer_comps[3], 2 * sizeof *edited.layer_comps);
        edited.nlayer_comps = 4;
        out = to_psd(&edited);
        f = parse(&out);
        CHECK(!bytes_eq(&resource(&f, PC_IO_LAYER_COMPS, NULL)->data,
                        &resource(&src, PC_IO_LAYER_COMPS, NULL)->data));
        back = from_psd(&out);
        CHECK(back.nlayer_comps == 4 && pc_str_eq_c(&back.layer_comps[0].name, "Renamed"));
        for (size_t i = 0; i < 4 && i < back.nlayer_comps; i++) {
            const pc_layer_comp *a = &edited.layer_comps[i];
            const pc_layer_comp *b = &back.layer_comps[i];
            CHECK(a->id == b->id && pc_layer_comp_captured_info(a) == pc_layer_comp_captured_info(b) &&
                  pc_str_eq(&a->comment, &b->comment) && a->nstates == b->nstates);
            for (size_t k = 0; k < a->nstates && k < b->nstates; k++) {
                CHECK(a->states[k].has_visible == b->states[k].has_visible &&
                      a->states[k].visible == b->states[k].visible &&
                      a->states[k].has_position == b->states[k].has_position &&
                      (!a->states[k].has_position || (a->states[k].position[0] == b->states[k].position[0] &&
                                                      a->states[k].position[1] == b->states[k].position[1])));
            }
        }
        CHECK(back.has_last_applied_comp == edited.has_last_applied_comp &&
              back.last_applied_comp == edited.last_applied_comp);
        /* Other metadata items in shmd (cust) survive regeneration. */
        for (size_t i = 0; i < f.layer_info.n; i++) {
            const pc_psd_block *s = pc_psd_layer_block(&f.layer_info.layers[i], "shmd");
            pc_psd_meta_item *items = NULL;
            size_t n = 0;
            bool cust = false;
            bool cmls = false;
            if (s == NULL) {
                continue;
            }
            must(pc_psd_parse_shmd(s->data.data, s->data.len, &items, &n, NULL));
            for (size_t k = 0; k < n; k++) {
                cust = cust || memcmp(items[k].key, "cust", 4) == 0;
                cmls = cmls || memcmp(items[k].key, "cmls", 4) == 0;
            }
            CHECK(cust && cmls);
            pc_psd_meta_items_free(items, n);
            break;
        }
        pc_psd_file_fini(&f);
        pc_buf_fini(&out);
        pc_document_fini(&back);
        pc_document_fini(&edited);
    }
    /* All comps deleted: the resource and the cmls items go away. */
    {
        pc_document none;
        must(pc_document_clone(&doc, &none, NULL));
        for (size_t i = 0; i < none.nlayer_comps; i++) {
            pc_layer_comp_fini(&none.layer_comps[i]);
        }
        none.nlayer_comps = 0;
        none.has_last_applied_comp = false;
        if (none.has_last_document_state) {
            pc_layer_comp_fini(&none.last_document_state);
            none.has_last_document_state = false;
        }
        out = to_psd(&none);
        f = parse(&out);
        CHECK(resource(&f, PC_IO_LAYER_COMPS, NULL) == NULL);
        for (size_t i = 0; i < f.layer_info.n; i++) {
            const pc_psd_block *s = pc_psd_layer_block(&f.layer_info.layers[i], "shmd");
            pc_psd_meta_item *items = NULL;
            size_t n = 0;
            if (s == NULL) {
                continue;
            }
            must(pc_psd_parse_shmd(s->data.data, s->data.len, &items, &n, NULL));
            for (size_t k = 0; k < n; k++) {
                CHECK(memcmp(items[k].key, "cmls", 4) != 0);
            }
            pc_psd_meta_items_free(items, n);
        }
        pc_psd_file_fini(&f);
        pc_buf_fini(&out);
        pc_document_fini(&none);
    }
    pc_document_fini(&doc);
    pc_psd_file_fini(&src);
    pc_buf_fini(&bytes);
}

static void test_corpus_artboards_import_and_verbatim_export(void)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_psd_file src;
    pc_document doc;
    pc_artboard_ref *boards = NULL;
    size_t nb = 0;
    pc_buf out;
    pc_psd_file f;
    if (!corpus_read("corpus/psd/psd-tools/gradient-sizes.psd", &bytes)) {
        return;
    }
    src = parse(&bytes);
    doc = from_psd(&bytes);
    must(pc_document_artboards(&doc, &boards, &nb, NULL));
    CHECK_EQ_UINT(nb, 3);
    {
        static const pc_rect want[3] = {{0, 0, 64, 64}, {0, 73, 64, 105}, {73, 0, 105, 64}};
        for (size_t w = 0; w < 3; w++) {
            bool found = false;
            for (size_t i = 0; i < nb; i++) {
                found = found || pc_rect_eq(boards[i].artboard->rect, want[w]);
            }
            CHECK(found);
        }
        for (size_t i = 0; i < nb; i++) {
            CHECK(boards[i].artboard->background.kind == PC_ARTBOARD_WHITE);
        }
    }
    out = to_psd(&doc);
    f = parse(&out);
    for (size_t i = 0; i < src.layer_info.n; i++) {
        const pc_psd_block *s = pc_psd_layer_block(&src.layer_info.layers[i], "artb");
        char *name = NULL;
        const pc_psd_block *o;
        if (s == NULL) {
            continue;
        }
        must(pc_psd_layer_name(&src.layer_info.layers[i], &name, NULL));
        o = layer_block(&f, name, "artb");
        CHECK(o != NULL && bytes_eq(&o->data, &s->data));
        free(name);
    }
    pc_psd_file_fini(&f);
    pc_buf_fini(&out);
    /* A changed board is regenerated. */
    {
        pc_document edited;
        pc_artboard_ref *eb = NULL;
        size_t neb = 0;
        pc_artboard *a;
        pc_artboard want;
        pc_document back;
        pc_artboard_ref *bb = NULL;
        size_t nbb = 0;
        bool found = false;
        must(pc_document_clone(&doc, &edited, NULL));
        must(pc_document_artboards(&edited, &eb, &neb, NULL));
        a = pc_layer_artboard_mut(pc_document_layer_mut(&edited, eb[0].layer));
        free(eb);
        a->background.kind = PC_ARTBOARD_BLACK;
        a->rect.x0 += 5;
        a->rect.x1 += 5;
        must(pc_artboard_clone(a, &want, NULL));
        back = roundtrip(&edited);
        must(pc_document_artboards(&back, &bb, &nbb, NULL));
        for (size_t i = 0; i < nbb; i++) {
            found = found || pc_artboard_eq(bb[i].artboard, &want);
        }
        CHECK(found);
        free(bb);
        pc_artboard_fini(&want);
        pc_document_fini(&back);
        pc_document_fini(&edited);
    }
    free(boards);
    pc_document_fini(&doc);
    pc_psd_file_fini(&src);
    pc_buf_fini(&bytes);
}

static void push_comp(pc_document *d, uint32_t id, const char *name, const char *comment, bool v, bool p,
                      bool a, pc_comp_layer_state *states, size_t n)
{
    pc_layer_comp *c;
    d->layer_comps = realloc(d->layer_comps, (d->nlayer_comps + 1) * sizeof *d->layer_comps);
    c = &d->layer_comps[d->nlayer_comps++];
    memset(c, 0, sizeof *c);
    c->id = id;
    must(pc_str_set_c(&c->name, name, NULL));
    must(pc_str_set_c(&c->comment, comment, NULL));
    c->apply_visibility = v;
    c->apply_position = p;
    c->apply_appearance = a;
    c->states = states;
    c->nstates = n;
}

static pc_document synthetic(pc_sample_type depth)
{
    pc_document d = t_doc("s", 80, 40, PC_COLOR_MODE_RGB, depth);
    pc_pixel_format fmt = {PC_COLOR_MODE_RGB, depth, true};
    pc_layer *ka = xmalloc(sizeof *ka);
    pc_layer *kb = xmalloc(sizeof *kb);
    pc_layer g;
    pc_comp_layer_state *st = NULL;
    size_t n = 0;
    static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    static const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    must(pc_layer_raster(&ka[0], "A", fmt, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&ka[0]), pc_rect_make(4, 4, 20, 20), red, 4, NULL));
    must(pc_layer_raster(&kb[0], "B", fmt, NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&kb[0]), pc_rect_make(44, 4, 60, 20), blue, 4, NULL));
    g = board("Board 1", ka, 1, pc_rect_make(0, 0, 40, 40), PC_ARTBOARD_WHITE, PC_COLOR_WHITE, "Custom");
    t_push(&d, &g);
    g = board("Board 2", kb, 1, pc_rect_make(40, 0, 80, 40), PC_ARTBOARD_CUSTOM,
              pc_color_rgb(0.0f, 1.0f, 0.0f), "");
    t_push(&d, &g);
    must(pc_comp_capture_states(&d, &st, &n, NULL));
    push_comp(&d, 11, "Both", "all", true, true, false, st, n);
    d.layers[1].visible = false;
    d.layers[0].content.u.group.children[0].blend = PC_BLEND_MULTIPLY;
    must(pc_comp_capture_states(&d, &st, &n, NULL));
    for (size_t i = 0; i < n; i++) {
        if (st[i].layer == d.layers[0].id) {
            pc_comp_layer_state_fini(&st[i]);
            memmove(&st[i], &st[i + 1], (n - i - 1) * sizeof *st);
            n--;
            i--;
        }
    }
    push_comp(&d, 12, "One", "", true, false, true, st, n);
    d.has_last_applied_comp = true;
    d.last_applied_comp = 12;
    return d;
}

/* "name:vis:pos" of each state, sorted by layer name. */
static void named_states(pc_buf *b, const pc_document *doc, const pc_layer_comp *c)
{
    char lines[16][128];
    size_t n = 0;
    for (size_t i = 0; i < c->nstates && n < 16; i++) {
        const pc_comp_layer_state *s = &c->states[i];
        const pc_layer *l = pc_document_layer(doc, s->layer);
        (void)snprintf(lines[n++], sizeof lines[0], "%s:%d%d:%d:%d,%d", l != NULL ? pc_str_c(&l->name) : "?",
                       s->has_visible, s->visible, s->has_position, s->position[0], s->position[1]);
    }
    qsort(lines, n, sizeof lines[0], (int (*)(const void *, const void *))strcmp);
    for (size_t i = 0; i < n; i++) {
        sb(b, "%s;", lines[i]);
    }
}

static void boards_s(pc_buf *b, const pc_document *d)
{
    pc_artboard_ref *r = NULL;
    size_t n = 0;
    must(pc_document_artboards(d, &r, &n, NULL));
    for (size_t i = 0; i < n; i++) {
        sb(b, "%s=", pc_str_c(r[i].name));
        JW(b, pc_artboard, r[i].artboard);
        sb(b, ";");
    }
    free(r);
}

static void test_synthetic_psd_roundtrip_all_depths(void)
{
    static const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_document d = synthetic(depths[k]);
        pc_document back = roundtrip(&d);
        pc_buf x = PC_BUF_INIT;
        pc_buf y = PC_BUF_INIT;
        boards_s(&x, &d);
        boards_s(&y, &back);
        CHECK_EQ_STR(pc_buf_cstr(&y), pc_buf_cstr(&x));
        CHECK(back.nlayer_comps == 2 && back.has_last_applied_comp && back.last_applied_comp == 12);
        for (size_t i = 0; i < 2 && i < back.nlayer_comps; i++) {
            const pc_layer_comp *a = &d.layer_comps[i];
            const pc_layer_comp *b = &back.layer_comps[i];
            CHECK(a->id == b->id && pc_str_eq(&a->name, &b->name) && pc_str_eq(&a->comment, &b->comment) &&
                  pc_layer_comp_captured_info(a) == pc_layer_comp_captured_info(b));
            pc_buf_truncate(&x, 0);
            pc_buf_truncate(&y, 0);
            named_states(&x, &d, a);
            named_states(&y, &back, b);
            CHECK_EQ_STR(pc_buf_cstr(&y), pc_buf_cstr(&x));
        }
        /* Re-exporting the imported document is byte-stable for comps and artboards. */
        {
            pc_buf o1 = to_psd(&back);
            pc_document back2 = from_psd(&o1);
            pc_buf o2 = to_psd(&back2);
            pc_psd_file f1 = parse(&o1);
            pc_psd_file f2 = parse(&o2);
            const pc_psd_resource *r1 = resource(&f1, PC_IO_LAYER_COMPS, NULL);
            const pc_psd_resource *r2 = resource(&f2, PC_IO_LAYER_COMPS, NULL);
            CHECK(r1 != NULL && r2 != NULL && bytes_eq(&r1->data, &r2->data));
            for (size_t i = 0; i < f1.layer_info.n; i++) {
                static const char *const keys[2] = {"artb", "shmd"};
                char *name = NULL;
                must(pc_psd_layer_name(&f1.layer_info.layers[i], &name, NULL));
                for (size_t kk = 0; kk < 2; kk++) {
                    const pc_psd_block *a = pc_psd_layer_block(&f1.layer_info.layers[i], keys[kk]);
                    const pc_psd_block *b = layer_block(&f2, name, keys[kk]);
                    CHECK((a == NULL) == (b == NULL) && (a == NULL || bytes_eq(&a->data, &b->data)));
                }
                free(name);
            }
            pc_psd_file_fini(&f1);
            pc_psd_file_fini(&f2);
            pc_buf_fini(&o1);
            pc_buf_fini(&o2);
            pc_document_fini(&back2);
        }
        pc_buf_fini(&x);
        pc_buf_fini(&y);
        pc_document_fini(&back);
        pc_document_fini(&d);
    }
}

/* The .pcraft part of synthetic_pcraft_roundtrip_keeps_everything (the ZIP-reordering and
 * format-detection parts are I5's). */
static void test_synthetic_pcraft_roundtrip_keeps_comps_and_artboards(void)
{
    pc_document d = synthetic(PC_SAMPLE_TYPE_U16);
    pc_comp_layer_state *st = NULL;
    size_t n = 0;
    pc_io_export_result r;
    pc_io_import_result back;
    must(pc_comp_capture_states(&d, &st, &n, NULL));
    d.has_last_document_state = true;
    memset(&d.last_document_state, 0, sizeof d.last_document_state);
    must(pc_str_set_c(&d.last_document_state.name, "Last Document State", NULL));
    d.last_document_state.apply_visibility = true;
    d.last_document_state.apply_position = true;
    d.last_document_state.apply_appearance = true;
    d.last_document_state.states = st;
    d.last_document_state.nstates = n;
    must(pc_io_export(&d, "x.pcraft", NULL, &r, NULL));
    must(pc_io_import("x.pcraft", r.bytes, r.len, &back, NULL));
    CHECK(back.document.nlayer_comps == 2);
    for (size_t i = 0; i < 2 && i < back.document.nlayer_comps; i++) {
        CHECK(pc_layer_comp_eq(&back.document.layer_comps[i], &d.layer_comps[i]));
    }
    CHECK(back.document.has_last_document_state &&
          pc_layer_comp_eq(&back.document.last_document_state, &d.last_document_state));
    CHECK(back.document.last_applied_comp == 12);
    CHECK(pc_artboard_eq(pc_layer_artboard(&back.document.layers[1]), pc_layer_artboard(&d.layers[1])));
    pc_io_import_result_fini(&back);
    pc_io_export_result_fini(&r);
    pc_document_fini(&d);
}

/* A layer at (4, 4) whose shmd offsets it by (dx, 0) in comp 1, plus resource 1065. */
static pc_document doc_with_raw_comp_offset(int32_t dx)
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
    pc_layer_id lid;
    pc_document doc;
    pc_io4_desc(&s, "null", &st, NULL);
    pc_io4_put(&s, "enab", pc_psd_value_bool(false), &st, NULL);
    pc_io4_list_push(&ids, pc_psd_value_int(1), &st, NULL);
    pc_io4_put(&s, "compList", ids, &st, NULL);
    pc_io4_desc(&o, "null", &st, NULL);
    pc_io4_put(&o, "Hrzn", pc_psd_value_int(dx), &st, NULL);
    pc_io4_put(&o, "Vrtc", pc_psd_value_int(0), &st, NULL);
    pc_io4_put_obj(&s, "Ofst", &o, &st, NULL);
    must(st);
    must(pc_psd_value_object(&s, &item, NULL));
    pc_io4_list_push(&list, item, &st, NULL);
    pc_io4_desc(&d, "null", &st, NULL);
    pc_io4_put(&d, "layerSettings", list, &st, NULL);
    must(st);
    must(pc_psd_vdesc_to_bytes(&d, &b, NULL));
    pc_psd_desc_fini(&d);
    {
        /* Only the cmls item (the shared builder also adds a cust item). */
        pc_psd_meta_item it;
        pc_psd_bytes shmd;
        pc_layer l;
        static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        doc = t_doc("s", 32, 32, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        must(pc_layer_raster(&l, "A", PC_PIXEL_FORMAT_RGBA8, NULL));
        must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(4, 4, 8, 8), red, 4, NULL));
        must(pc_psd_meta_item_make("cmls", b.data, b.len, &it, NULL));
        must(pc_psd_write_shmd(&it, 1, &shmd, NULL));
        pc_psd_bytes_fini(&it.data);
        l.psd_blocks = xmalloc(sizeof *l.psd_blocks);
        memcpy(l.psd_blocks[0].key, "shmd", 4);
        must(pc_blob_new(shmd.data, shmd.len, &l.psd_blocks[0].data, NULL));
        l.npsd_blocks = 1;
        pc_psd_bytes_fini(&shmd);
        lid = l.id;
        (void)lid;
        t_push(&doc, &l);
    }
    pc_psd_bytes_fini(&b);
    pc_io4_desc(&comp, "Comp", &st, NULL);
    pc_io4_put_text(&comp, "Nm  ", "One", 3, true, &st, NULL);
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
    m_push_resource(&doc, PC_IO_LAYER_COMPS, b.data, b.len);
    {
        pc_io_comps c;
        must(pc_io_decode_comps(b.data, b.len, &doc, &c, NULL, NULL));
        doc.layer_comps = c.comps;
        doc.nlayer_comps = c.n;
        doc.has_last_applied_comp = c.has_last_applied;
        doc.last_applied_comp = c.last_applied;
        doc.has_last_document_state = c.has_last_state;
        doc.last_document_state = c.last_state;
    }
    pc_psd_bytes_fini(&b);
    return doc;
}

/* #1017: out-of-range comp offsets import and export without panicking. */
static void test_out_of_range_comp_offsets_import_and_export_without_panic(void)
{
    pc_document src = doc_with_raw_comp_offset(INT32_MAX);
    pc_buf bytes = to_psd(&src);
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_document imp = from_psd_w(&bytes, &w);
    pc_buf again;
    pc_psd_file a;
    pc_psd_file b;
    CHECK(pc_io_warnings_any(&w, "layer comp position"));
    CHECK(imp.nlayer_comps == 1 && pc_str_eq_c(&imp.layer_comps[0].name, "One") &&
          imp.layer_comps[0].nstates == 1);
    CHECK(imp.layer_comps[0].states[0].has_visible && !imp.layer_comps[0].states[0].visible &&
          !imp.layer_comps[0].states[0].has_position);
    again = to_psd(&imp);
    a = parse(&bytes);
    b = parse(&again);
    CHECK(layer_block(&a, "A", "shmd") != NULL && layer_block(&b, "A", "shmd") != NULL &&
          bytes_eq(&layer_block(&a, "A", "shmd")->data, &layer_block(&b, "A", "shmd")->data));
    pc_psd_file_fini(&a);
    pc_psd_file_fini(&b);
    pc_buf_fini(&again);
    pc_buf_fini(&bytes);
    pc_document_fini(&imp);
    pc_document_fini(&src);
    pc_io_warnings_fini(&w);
    {
        pc_document d = t_doc("s", 32, 32, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_layer l;
        pc_comp_layer_state *st = xmalloc(sizeof *st);
        pc_document back;
        static const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        must(pc_layer_raster(&l, "A", PC_PIXEL_FORMAT_RGBA8, NULL));
        must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(4, 4, 8, 8), red, 4, NULL));
        memset(st, 0, sizeof *st);
        st->layer = l.id;
        st->has_visible = true;
        st->has_position = true;
        st->position[0] = INT32_MIN;
        t_push(&d, &l);
        push_comp(&d, 1, "One", "", true, true, false, st, 1);
        back = roundtrip(&d);
        CHECK(back.layer_comps[0].states[0].has_visible && !back.layer_comps[0].states[0].visible &&
              !back.layer_comps[0].states[0].has_position);
        pc_document_fini(&back);
        pc_document_fini(&d);
    }
    {
        pc_document ok = doc_with_raw_comp_offset(6);
        pc_buf ob = to_psd(&ok);
        pc_document back = from_psd_w(&ob, &w);
        CHECK(!pc_io_warnings_any(&w, "layer comp"));
        CHECK(back.layer_comps[0].states[0].has_position && back.layer_comps[0].states[0].position[0] == 10 &&
              back.layer_comps[0].states[0].position[1] == 4);
        pc_io_warnings_fini(&w);
        pc_document_fini(&back);
        pc_buf_fini(&ob);
        pc_document_fini(&ok);
    }
}

/* ---------------------------------------------------------------- vector.rs */

/* Coordinates on a 64-px grid are exact in PSD's 8.24 fixed point for a 256-px canvas. */
static pc_path vpath(void)
{
    pc_path p;
    static const double tri[3][2] = {{32.0, 32.0}, {64.0, 32.0}, {64.0, 64.0}};
    static const double line[2][2] = {{200.0, 10.0}, {250.0, 60.0}};
    memset(&p, 0, sizeof p);
    p.subpaths = xmalloc(3 * sizeof *p.subpaths);
    p.nsubpaths = 3;
    p.subpaths[0].closed = true;
    p.subpaths[0].op = PC_PATH_COMBINE;
    p.subpaths[0].knots = xmalloc(3 * sizeof(pc_knot));
    p.subpaths[0].nknots = 3;
    p.subpaths[0].knots[0] = pc_knot_corner(16.0, 16.0);
    p.subpaths[0].knots[1] =
        pc_knot_smooth((pc_point){128.0, 32.0}, (pc_point){96.0, 16.0}, (pc_point){160.0, 48.0});
    p.subpaths[0].knots[2] = pc_knot_corner(64.0, 192.0);
    must(pc_subpath_polygon(tri, 3, &p.subpaths[1], NULL));
    p.subpaths[1].op = PC_PATH_SUBTRACT;
    must(pc_subpath_polyline(line, 2, &p.subpaths[2], NULL));
    p.subpaths[2].op = PC_PATH_EXCLUDE;
    return p;
}

static pc_document vdoc(void)
{
    pc_document d;
    must(pc_document_with_background(&d, "v", pc_size_make(256, 256), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                     PC_COLOR_WHITE, NULL));
    return d;
}

static void push_named_path(pc_document *d, const char *name, pc_path p)
{
    d->paths = realloc(d->paths, (d->npaths + 1) * sizeof *d->paths);
    memset(&d->paths[d->npaths], 0, sizeof *d->paths);
    must(pc_str_set_c(&d->paths[d->npaths].name, name, NULL));
    d->paths[d->npaths++].path = p;
}

static void test_saved_paths_work_path_and_clipping_roundtrip(void)
{
    pc_document d = vdoc();
    pc_path inv = vpath();
    pc_document b;
    pc_document b2;
    pc_document c;
    static const double tri[3][2] = {{0.0, 0.0}, {128.0, 0.0}, {128.0, 128.0}};
    push_named_path(&d, "Outline", vpath());
    inv.inverted = true;
    push_named_path(&d, "Inverted", inv);
    d.has_work_path = true;
    d.work_path.subpaths = xmalloc(sizeof *d.work_path.subpaths);
    d.work_path.nsubpaths = 1;
    must(pc_subpath_polygon(tri, 3, &d.work_path.subpaths[0], NULL));
    d.has_clipping_path = true;
    must(pc_str_set_c(&d.clipping_path.name, "Outline", NULL));
    b = roundtrip(&d);
    CHECK(b.npaths == 2 && pc_str_eq_c(&b.paths[0].name, "Outline"));
    CHECK(pc_path_eq(&b.paths[0].path, &d.paths[0].path) && pc_path_eq(&b.paths[1].path, &d.paths[1].path));
    CHECK(b.has_work_path && pc_path_eq(&b.work_path, &d.work_path));
    CHECK(b.has_clipping_path && pc_str_eq_c(&b.clipping_path.name, "Outline"));
    /* Unedited paths are written back byte-identical; edited ones are regenerated. */
    must(pc_document_clone(&b, &b2, NULL));
    pc_subpath_fini(&b2.paths[1].path.subpaths[2]);
    b2.paths[1].path.nsubpaths = 2;
    c = roundtrip(&b2);
    CHECK(c.paths[0].psd_raw != NULL && pc_blob_eq(c.paths[0].psd_raw, b.paths[0].psd_raw));
    CHECK(pc_path_eq(&c.paths[1].path, &b2.paths[1].path));
    /* Removing the clipping path drops the preserved resource. */
    pc_clipping_path_fini(&c.clipping_path);
    c.has_clipping_path = false;
    {
        pc_document e = roundtrip(&c);
        CHECK(!e.has_clipping_path);
        pc_document_fini(&e);
    }
    pc_document_fini(&c);
    pc_document_fini(&b2);
    pc_document_fini(&b);
    pc_document_fini(&d);
}

static pc_layer shape_layer(pc_shape_layer *sh)
{
    pc_layer_content c;
    pc_layer l;
    must(pc_vec_render_shape(sh, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 256, 256), &sh->cache, NULL));
    sh->has_cache = true;
    c.kind = PC_LAYER_SHAPE;
    c.u.shape = *sh;
    must(pc_layer_init(&l, "Shape", &c, NULL));
    return l;
}

static bool vec_blocks_eq(const pc_layer *a, const pc_layer *b)
{
    static const char *const keys[6] = {"vmsk", "vsms", "vstk", "vogk", "vscg", "SoCo"};
    size_t ia = 0;
    size_t ib = 0;
    for (;;) {
        while (ia < a->npsd_blocks) {
            bool hit = false;
            for (size_t k = 0; k < 6; k++) {
                hit = hit || memcmp(a->psd_blocks[ia].key, keys[k], 4) == 0;
            }
            if (hit) {
                break;
            }
            ia++;
        }
        while (ib < b->npsd_blocks) {
            bool hit = false;
            for (size_t k = 0; k < 6; k++) {
                hit = hit || memcmp(b->psd_blocks[ib].key, keys[k], 4) == 0;
            }
            if (hit) {
                break;
            }
            ib++;
        }
        if (ia == a->npsd_blocks || ib == b->npsd_blocks) {
            return ia == a->npsd_blocks && ib == b->npsd_blocks;
        }
        if (memcmp(a->psd_blocks[ia].key, b->psd_blocks[ib].key, 4) != 0 ||
            !pc_blob_eq(a->psd_blocks[ia].data, b->psd_blocks[ib].data)) {
            return false;
        }
        ia++;
        ib++;
    }
}

static void test_shape_layer_typed_roundtrip(void)
{
    pc_document d = vdoc();
    pc_shape_layer sh;
    pc_shape_layer ell;
    pc_layer l;
    pc_document b;
    pc_document c;
    static const float dashes[2] = {2.0f, 1.0f};
    memset(&sh, 0, sizeof sh);
    sh.path = vpath();
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
    sh.has_stroke = true;
    pc_shape_stroke_default(&sh.stroke);
    sh.stroke.width = 4.0f;
    sh.stroke.paint = pc_fill_solid(pc_color_rgb(0.0f, 0.0f, 1.0f));
    sh.stroke.opacity = 0.5f;
    sh.stroke.align = PC_STROKE_ALIGN_INSIDE;
    sh.stroke.cap = PC_LINE_CAP_ROUND;
    sh.stroke.join = PC_LINE_JOIN_ROUND;
    sh.stroke.dashes = xmalloc(sizeof dashes);
    memcpy(sh.stroke.dashes, dashes, sizeof dashes);
    sh.stroke.ndashes = 2;
    {
        pc_shape_layer copy;
        must(pc_shape_layer_clone(&sh, &copy, NULL));
        l = shape_layer(&copy);
        t_push(&d, &l);
    }
    memset(&ell, 0, sizeof ell);
    must(pc_vec_shape_ellipse(64.0, 64.0, 128.0, 64.0, &ell.path, NULL));
    ell.path.subpaths[0].op = PC_PATH_COMBINE;
    ell.has_stroke = true;
    pc_shape_stroke_default(&ell.stroke);
    ell.has_live = true;
    ell.live.kind = PC_LIVE_ELLIPSE;
    ell.live.rect[0] = 64.0;
    ell.live.rect[1] = 64.0;
    ell.live.rect[2] = 128.0;
    ell.live.rect[3] = 64.0;
    {
        pc_shape_layer copy;
        must(pc_shape_layer_clone(&ell, &copy, NULL));
        l = shape_layer(&copy);
        t_push(&d, &l);
    }
    b = roundtrip(&d);
    CHECK(b.layers[1].content.kind == PC_LAYER_SHAPE && b.layers[2].content.kind == PC_LAYER_SHAPE);
    {
        const pc_shape_layer *s = &b.layers[1].content.u.shape;
        const pc_shape_layer *e = &b.layers[2].content.u.shape;
        CHECK(pc_path_eq(&s->path, &sh.path));
        CHECK(s->has_fill && pc_fill_eq(&s->fill, &sh.fill));
        CHECK(s->has_stroke && s->stroke.width == 4.0f && s->stroke.align == PC_STROKE_ALIGN_INSIDE &&
              s->stroke.cap == PC_LINE_CAP_ROUND && s->stroke.join == PC_LINE_JOIN_ROUND &&
              s->stroke.ndashes == 2 && s->stroke.dashes[0] == 2.0f && s->stroke.dashes[1] == 1.0f);
        CHECK(fabsf(s->stroke.opacity - 0.5f) < 1e-6f);
        CHECK(s->has_cache);
        CHECK(!e->has_fill && e->has_live && pc_live_shape_eq(&e->live, &ell.live));
        /* Kappa handles are quantized to PSD's 8.24 fixed point. */
        CHECK(e->path.nsubpaths == ell.path.nsubpaths);
        for (size_t i = 0; i < e->path.nsubpaths && i < ell.path.nsubpaths; i++) {
            for (size_t k = 0; k < e->path.subpaths[i].nknots && k < ell.path.subpaths[i].nknots; k++) {
                const pc_knot *x = &e->path.subpaths[i].knots[k];
                const pc_knot *y = &ell.path.subpaths[i].knots[k];
                CHECK(fabs(x->anchor.x - y->anchor.x) < 1e-4 && fabs(x->anchor.y - y->anchor.y) < 1e-4 &&
                      fabs(x->in_ctrl.x - y->in_ctrl.x) < 1e-4 && fabs(x->out_ctrl.y - y->out_ctrl.y) < 1e-4);
            }
        }
    }
    /* A second round trip keeps every vector block byte-identical. */
    c = roundtrip(&b);
    for (size_t i = 1; i < 3; i++) {
        CHECK(vec_blocks_eq(&c.layers[i], &b.layers[i]));
    }
    pc_document_fini(&c);
    pc_document_fini(&b);
    pc_shape_layer_fini(&sh);
    pc_shape_layer_fini(&ell);
    pc_document_fini(&d);
}

static void test_vector_mask_roundtrip_and_removal(void)
{
    pc_document d = vdoc();
    pc_layer l;
    pc_path p;
    pc_vector_mask vm;
    pc_document b;
    pc_compose_buffer f;
    static const double rect[4][2] = {{0.0, 0.0}, {128.0, 0.0}, {128.0, 256.0}, {0.0, 256.0}};
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    memset(&p, 0, sizeof p);
    p.subpaths = xmalloc(sizeof *p.subpaths);
    p.nsubpaths = 1;
    must(pc_subpath_polygon(rect, 4, &p.subpaths[0], NULL));
    pc_vector_mask_init(&vm, &p);
    vm.linked = false;
    must(pc_layer_raster(&l, "pix", pc_document_pixel_format(&d), NULL));
    must(pc_surface_fill_rect(pc_layer_surface_mut(&l), pc_rect_make(0, 0, 256, 256), black, 4, NULL));
    l.has_vector_mask = true;
    must(pc_vector_mask_clone(&vm, &l.vector_mask, NULL));
    t_push(&d, &l);
    b = roundtrip(&d);
    CHECK(b.layers[1].has_vector_mask && pc_vector_mask_eq(&b.layers[1].vector_mask, &vm));
    {
        bool has = false;
        for (size_t i = 0; i < b.layers[1].npsd_blocks; i++) {
            has = has || memcmp(b.layers[1].psd_blocks[i].key, "vmsk", 4) == 0;
        }
        CHECK(has);
    }
    must(pc_compose_flatten(&b, &f, NULL));
    CHECK(f.px[10 * 256 + 10][0] < 0.01f && f.px[10 * 256 + 200][0] > 0.99f);
    free(f.px);
    {
        pc_document c;
        pc_document e;
        bool has = false;
        must(pc_document_clone(&b, &c, NULL));
        pc_vector_mask_fini(&c.layers[1].vector_mask);
        c.layers[1].has_vector_mask = false;
        e = roundtrip(&c);
        CHECK(!e.layers[1].has_vector_mask);
        for (size_t i = 0; i < e.layers[1].npsd_blocks; i++) {
            has = has || memcmp(e.layers[1].psd_blocks[i].key, "vmsk", 4) == 0 ||
                  memcmp(e.layers[1].psd_blocks[i].key, "vsms", 4) == 0;
        }
        CHECK(!has);
        pc_document_fini(&e);
        pc_document_fini(&c);
    }
    {
        pc_document g = vdoc();
        pc_layer *kids = xmalloc(sizeof *kids);
        pc_layer grp;
        pc_document gb;
        must(pc_layer_raster(&kids[0], "in", pc_document_pixel_format(&g), NULL));
        must(pc_layer_group(&grp, "G", kids, 1, NULL));
        grp.has_vector_mask = true;
        must(pc_vector_mask_clone(&vm, &grp.vector_mask, NULL));
        t_push(&g, &grp);
        gb = roundtrip(&g);
        CHECK(gb.layers[1].has_vector_mask);
        pc_document_fini(&gb);
        pc_document_fini(&g);
    }
    pc_vector_mask_fini(&vm);
    pc_document_fini(&b);
    pc_document_fini(&d);
}

/* Unedited corpus shapes and vector masks export with byte-identical vector blocks. */
static void corpus_files(const char *dir, char ***out, size_t *n);

static void test_corpus_vector_blocks_survive_roundtrip(void)
{
    char **files = NULL;
    size_t nf = 0;
    size_t checked = 0;
    static const char *const keys[6] = {"vmsk", "vsms", "vogk", "vstk", "vscg", "SoCo"};
    corpus_files("corpus/psd", &files, &nf);
    if (nf == 0) {
        (void)printf("  (corpus missing: vector corpus skipped)\n");
    }
    for (size_t i = 0; i < nf; i++) {
        pc_buf bytes = PC_BUF_INIT;
        pc_psd_file file;
        pc_document d;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_io_export_result out;
        pc_psd_file back;
        if (pc_file_read(files[i], (size_t)1 << 32, &bytes, NULL) != PC_OK ||
            pc_psd_file_parse(bytes.data, bytes.len, &file, NULL) != PC_OK) {
            pc_buf_fini(&bytes);
            continue;
        }
        must(pc_io_psd_to_document(&file, &d, &w, NULL));
        if (pc_io_export(&d, "x.psd", NULL, &out, NULL) == PC_OK) {
            must(pc_psd_file_parse(out.bytes, out.len, &back, NULL));
            for (size_t l = 0; file.has_layer_info && l < file.layer_info.n; l++) {
                const pc_psd_layer *la = &file.layer_info.layers[l];
                char *name = NULL;
                must(pc_psd_layer_name(la, &name, NULL));
                for (size_t k = 0; k < la->nblocks; k++) {
                    bool want = false;
                    bool found = false;
                    for (size_t q = 0; q < 6; q++) {
                        want = want || memcmp(la->blocks[k].key, keys[q], 4) == 0;
                    }
                    if (!want) {
                        continue;
                    }
                    checked++;
                    for (size_t m = 0; back.has_layer_info && m < back.layer_info.n && !found; m++) {
                        const pc_psd_layer *lb = &back.layer_info.layers[m];
                        if (!layer_named(lb, name)) {
                            continue;
                        }
                        for (size_t q = 0; q < lb->nblocks && !found; q++) {
                            found = memcmp(lb->blocks[q].key, la->blocks[k].key, 4) == 0 &&
                                    bytes_eq(&lb->blocks[q].data, &la->blocks[k].data);
                        }
                    }
                    if (!found) {
                        PC_T_FAIL("%s: %s/%.4s changed or missing", files[i], name,
                                  (const char *)la->blocks[k].key);
                    }
                }
                free(name);
            }
            for (size_t r = 0; r < file.nresources; r++) {
                bool found = false;
                if (file.resources[r].id < 2000 || file.resources[r].id > 2997) {
                    continue;
                }
                for (size_t q = 0; q < back.nresources && !found; q++) {
                    found = back.resources[q].id == file.resources[r].id &&
                            bytes_eq(&back.resources[q].data, &file.resources[r].data);
                }
                CHECK(found);
            }
            pc_psd_file_fini(&back);
            pc_io_export_result_fini(&out);
        }
        pc_io_warnings_fini(&w);
        pc_document_fini(&d);
        pc_psd_file_fini(&file);
        pc_buf_fini(&bytes);
    }
    (void)printf("  vector corpus: %zu vector blocks byte-identical after round trip\n", checked);
    for (size_t i = 0; i < nf; i++) {
        free(files[i]);
    }
    free(files);
}

static void corpus_files(const char *dir, char ***out, size_t *n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        struct stat st;
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.') {
            continue;
        }
        (void)snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            corpus_files(path, out, n);
        } else if (len > 4 && (strcasecmp(e->d_name + len - 4, ".psd") == 0 ||
                               strcasecmp(e->d_name + len - 4, ".psb") == 0)) {
            *out = realloc(*out, (*n + 1) * sizeof **out);
            (*out)[(*n)++] = pc_strndup(path, strlen(path));
        }
    }
    (void)closedir(d);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_corpus_notes_import_and_verbatim_export);
    RUN(test_measurement_scale_roundtrips_through_psd);
    RUN(test_a_note_icon_near_i32_max_saves_with_representable_geometry);
    RUN(test_an_unreadable_anno_block_warns_and_survives_a_save);
    RUN(test_guides_in_numbers_vertical_first_like_resource_1032);
    RUN(test_exported_artboard_indices_match_guides_resource);
    RUN(test_corpus_layer_comps_import_and_verbatim_export);
    RUN(test_corpus_artboards_import_and_verbatim_export);
    RUN(test_synthetic_psd_roundtrip_all_depths);
    RUN(test_synthetic_pcraft_roundtrip_keeps_comps_and_artboards);
    RUN(test_out_of_range_comp_offsets_import_and_export_without_panic);
    RUN(test_saved_paths_work_path_and_clipping_roundtrip);
    RUN(test_shape_layer_typed_roundtrip);
    RUN(test_vector_mask_roundtrip_and_removal);
    RUN(test_corpus_vector_blocks_survive_roundtrip);
    return pc_t_finish();
}
