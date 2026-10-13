/* Parity with crates/psd: replays `rustref psd-vectors` (recorded in tests/golden/psd-vectors.txt)
 * over the testgen cases, builder documents and the small writers. File bytes are compared
 * where no zlib stream is involved (zlib output differs from the reference's encoder, §7);
 * decoded planes, RGBA8 previews and layer trees are compared everywhere. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "harness.h"
#include "psd/builder.h"
#include "psd/filter_effects.h"
#include "psd/misc.h"
#include "psd/pixels.h"
#include "psd/slices.h"
#include "testgen.h"

#define FNV0 0xcbf29ce484222325u

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static void fnv_str(uint64_t *h, const char *s)
{
    fnv(h, s, strlen(s));
}

static pc_buf golden;

/* The expected line for `prefix\tname\t`, or NULL. */
static const char *expect(const char *kind, const char *name)
{
    char key[160];
    const char *p = (const char *)golden.data;
    const char *end = p + golden.len;
    size_t kl = *name != '\0' ? (size_t)snprintf(key, sizeof key, "%s\t%s\t", kind, name)
                               : (size_t)snprintf(key, sizeof key, "%s\t", kind);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > kl && memcmp(p, key, kl) == 0) {
            return p + kl;
        }
        p += len + 1;
    }
    return NULL;
}

static void tree_debug(const pc_psd_node *n, size_t count, pc_buf *o)
{
    size_t i;
    (void)pc_buf_append_byte(o, '[', NULL);
    for (i = 0; i < count; i++) {
        if (i > 0) {
            (void)pc_buf_append_str(o, ", ", NULL);
        }
        if (!n[i].is_group) {
            (void)pc_buf_appendf(o, NULL, "Layer { index: %zu }", n[i].index);
            continue;
        }
        (void)pc_buf_appendf(o, NULL, "Group { index: %zu, divider: ", n[i].index);
        if (n[i].has_divider) {
            (void)pc_buf_appendf(o, NULL, "Some(%zu)", n[i].divider);
        } else {
            (void)pc_buf_append_str(o, "None", NULL);
        }
        (void)pc_buf_append_str(o, ", children: ", NULL);
        tree_debug(n[i].children, n[i].n, o);
        (void)pc_buf_append_str(o, " }", NULL);
    }
    (void)pc_buf_append_byte(o, ']', NULL);
}

static bool uses_zlib(const pc_psd_file *f)
{
    size_t nl;
    size_t i;
    size_t k;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    if (f->image_compression == PC_PSD_ZIP || f->image_compression == PC_PSD_ZIP_PREDICT) {
        return true;
    }
    for (i = 0; i < nl; i++) {
        for (k = 0; k < layers[i].nchannels; k++) {
            uint16_t c = layers[i].channels[k].compression;
            if (layers[i].channels[k].has_compression && (c == PC_PSD_ZIP || c == PC_PSD_ZIP_PREDICT)) {
                return true;
            }
        }
    }
    return false;
}

static void hash_result(uint64_t *h, pc_status st, const pc_psd_bytes *b, const pc_error *e)
{
    if (st == PC_OK) {
        fnv(h, b->data, b->len);
    } else {
        fnv_str(h, e->msg);
    }
}

static void test_testgen_cases(void)
{
    size_t n;
    size_t i;
    int compared_bytes = 0;
    tg_case *cases = tg_all_cases(&n);
    CHECK_EQ_UINT(n, 210);
    for (i = 0; i < n; i++) {
        const pc_psd_file *f = &cases[i].file;
        const char *want = expect("case", cases[i].name);
        uint64_t hb = FNV0;
        uint64_t hd = FNV0;
        pc_psd_bytes b = {NULL, 0};
        pc_psd_image8 img;
        pc_psd_node *roots = NULL;
        size_t nroots = 0;
        pc_buf tree = PC_BUF_INIT;
        pc_error e;
        size_t nl;
        size_t li;
        size_t k;
        char got[40];
        pc_status st;
        const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
        pc_t_name = cases[i].name;
        if (want == NULL) {
            PC_T_FAIL("%s", "no golden line");
            continue;
        }
        if (pc_psd_file_to_bytes(f, &b, &e) == PC_OK) {
            fnv(&hb, b.data, b.len);
        }
        pc_psd_bytes_fini(&b);
        st = pc_psd_file_decode_merged(f, &b, &e);
        hash_result(&hd, st, &b, &e);
        pc_psd_bytes_fini(&b);
        for (li = 0; li < nl; li++) {
            for (k = 0; k < layers[li].nchannels; k++) {
                st = pc_psd_layer_decode_channel(&layers[li], layers[li].channels[k].id, f->header.depth,
                                                 f->header.version, &b, &e);
                hash_result(&hd, st, &b, &e);
                pc_psd_bytes_fini(&b);
            }
        }
        st = pc_psd_composite_rgba8(f, &img, &e);
        hash_result(&hd, st, st == PC_OK ? &img.data : NULL, &e);
        if (st == PC_OK) {
            pc_psd_image8_fini(&img);
        }
        for (li = 0; li < nl; li++) {
            st = pc_psd_layer_rgba8(f, li, &img, &e);
            if (st == PC_OK) {
                uint8_t le[4];
                fnv(&hd, img.data.data, img.data.len);
                pc_store_le32(le, (uint32_t)img.left);
                fnv(&hd, le, 4);
                pc_psd_image8_fini(&img);
            } else {
                fnv_str(&hd, e.msg);
            }
            st = pc_psd_layer_user_mask(f, li, &img, &e);
            if (st == PC_OK) {
                fnv(&hd, img.data.data, img.data.len);
                fnv(&hd, &img.default_value, 1);
                pc_psd_image8_fini(&img);
            } else if (st != PC_ERR_NOT_FOUND || strcmp(e.msg, "layer has no mask channel") != 0) {
                fnv_str(&hd, e.msg);
            }
        }
        CHECK_EQ_INT(pc_psd_layer_tree(f, &roots, &nroots, NULL), PC_OK);
        tree_debug(roots, nroots, &tree);
        fnv(&hd, tree.data, tree.len);
        pc_buf_fini(&tree);
        pc_psd_nodes_free(roots, nroots);
        (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)hd);
        if (strncmp(want + 17, got, 16) != 0) {
            PC_T_FAIL("decoded hash %s, want %.16s", got, want + 17);
        }
        if (!uses_zlib(f)) {
            (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)hb);
            if (strncmp(want, got, 16) != 0) {
                PC_T_FAIL("bytes hash %s, want %.16s", got, want);
            }
            compared_bytes++;
        }
    }
    (void)printf("(%d of %zu cases byte-compared) ", compared_bytes, n);
    tg_free_cases(cases, n);
}

static pc_psd_pixels make_px(uint16_t mode, uint16_t depth, size_t n, size_t seed, void **owned)
{
    size_t spp = mode == PC_PSD_MODE_RGB ? 4 : mode == PC_PSD_MODE_GRAYSCALE ? 2 : 5;
    size_t i;
    pc_psd_pixels p;
    p.len = n * spp;
    if (depth == 8) {
        uint8_t *v = malloc(p.len + 1);
        for (i = 0; v != NULL && i < p.len; i++) {
            v[i] = (uint8_t)((i * 31 + seed * 7) % 256);
        }
        *owned = v;
        p.kind = mode == PC_PSD_MODE_RGB ? PC_PSD_PX_RGBA8 : mode == PC_PSD_MODE_GRAYSCALE ? PC_PSD_PX_GRAYA8 : PC_PSD_PX_CMYKA8;
    } else {
        uint16_t *v = malloc(2 * p.len + 2);
        for (i = 0; v != NULL && i < p.len; i++) {
            v[i] = (uint16_t)((i * 7919 + seed * 131) % 65536);
        }
        *owned = v;
        p.kind = mode == PC_PSD_MODE_RGB ? PC_PSD_PX_RGBA16 : mode == PC_PSD_MODE_GRAYSCALE ? PC_PSD_PX_GRAYA16 : PC_PSD_PX_CMYKA16;
    }
    p.data = *owned;
    return p;
}

/* R2 `readback` lines: a builder document parsed again, its merged planes, the RGBA8 composite
 * (the white matte removed) and every layer's RGBA8. */
static uint64_t readback(const pc_psd_bytes *bytes)
{
    uint64_t h = FNV0;
    pc_psd_file f;
    pc_psd_bytes m = {NULL, 0};
    pc_psd_image8 img;
    pc_error e;
    size_t nl = 0;
    size_t i;
    if (pc_psd_file_parse(bytes->data, bytes->len, &f, &e) != PC_OK) {
        fnv_str(&h, e.msg);
        return h;
    }
    if (pc_psd_file_decode_merged(&f, &m, &e) == PC_OK) {
        fnv(&h, m.data, m.len);
        pc_psd_bytes_fini(&m);
    } else {
        fnv_str(&h, e.msg);
    }
    if (pc_psd_composite_rgba8(&f, &img, &e) == PC_OK) {
        fnv(&h, img.data.data, img.data.len);
        pc_psd_image8_fini(&img);
    } else {
        fnv_str(&h, e.msg);
    }
    (void)pc_psd_file_layers(&f, &nl);
    for (i = 0; i < nl; i++) {
        if (pc_psd_layer_rgba8(&f, i, &img, &e) == PC_OK) {
            fnv(&h, img.data.data, img.data.len);
            pc_psd_image8_fini(&img);
        } else {
            fnv_str(&h, e.msg);
        }
    }
    pc_psd_file_fini(&f);
    return h;
}

/* R2 `unmatte` lines: RGB merged composites with alpha at 16 and 32 bits. */
static void test_unmatte(void)
{
    static const float alpha32[12] = {0.0f, 0.25f, 0.5f,  1.0f,   NAN,    -0.5f,
                                      1.5f, 1e-9f, 0.75f, 0.125f, 0.999f, 0.01f};
    static const uint16_t alpha16[12] = {0,   16384, 32768, 65535, 1,     65534,
                                         300, 40000, 49151, 8192,  65000, 655};
    uint16_t depth;
    for (depth = 16; depth <= 32; depth += 16) {
        const size_t n = 12;
        pc_psd_file f = tg_merged_only(PC_PSD_V1, PC_PSD_MODE_RGB, depth, PC_PSD_RAW, 4, 3);
        uint8_t native[4 * 12 * 4];
        size_t at = 0;
        size_t c;
        size_t i;
        uint64_t h = FNV0;
        pc_psd_image8 img;
        pc_error e;
        char name[8];
        char got[20];
        const char *want;
        f.header.channels = 4;
        for (c = 0; c < 4; c++) {
            for (i = 0; i < n; i++) {
                if (depth == 16) {
                    uint16_t v = c == 3 ? alpha16[i] : (uint16_t)((i * 7919 + c * 131) % 65536);
                    pc_store_be16(native + at, v);
                    at += 2;
                } else {
                    float v = c == 3 ? alpha32[i] : (float)((i * 13 + c * 7) % 12) * 0.1f - 0.1f;
                    uint32_t bits;
                    memcpy(&bits, &v, 4);
                    pc_store_be32(native + at, bits);
                    at += 4;
                }
            }
        }
        pc_psd_bytes_fini(&f.image_data);
        f.image_compression = PC_PSD_RAW;
        if (pc_psd_image_encode(PC_PSD_RAW, native, at, &f.header, &f.image_data, &e) == PC_OK) {
            if (pc_psd_composite_rgba8(&f, &img, &e) == PC_OK) {
                fnv(&h, img.data.data, img.data.len);
                pc_psd_image8_fini(&img);
            } else {
                fnv_str(&h, e.msg);
            }
        } else {
            fnv_str(&h, e.msg);
        }
        (void)snprintf(name, sizeof name, "%u", (unsigned)depth);
        (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)h);
        want = expect("unmatte", name);
        pc_t_name = name;
        CHECK(want != NULL && strncmp(want, got, 16) == 0);
        pc_psd_file_fini(&f);
    }
}

static void test_builder_documents(void)
{
    static const uint16_t MODES[4][2] = {{PC_PSD_MODE_RGB, 8}, {PC_PSD_MODE_RGB, 16}, {PC_PSD_MODE_GRAYSCALE, 8},
                                         {PC_PSD_MODE_CMYK, 16}};
    static const char *const MN[4] = {"Rgb", "Rgb", "Grayscale", "Cmyk"};
    static const uint8_t mdata[6] = {0, 128, 255, 9, 8, 7};
    static const uint8_t icc[3] = {1, 2, 3};
    int m;
    int c;
    int v;
    for (m = 0; m < 4; m++) {
        for (c = 0; c < 2; c++) {
            for (v = 0; v < 2; v++) {
                uint16_t mode = MODES[m][0];
                uint16_t depth = MODES[m][1];
                pc_psd_builder *b = pc_psd_builder_new(7, 5);
                void *o1 = NULL;
                void *o2 = NULL;
                void *o3 = NULL;
                pc_psd_layer_spec l1;
                pc_psd_layer_spec l2;
                pc_psd_mask_spec ms;
                pc_psd_group_spec g;
                pc_psd_bytes bytes = {NULL, 0};
                pc_error e;
                uint64_t hb = FNV0;
                uint64_t hr = FNV0;
                char name[64];
                char got[20];
                const char *want;
                if (b == NULL) {
                    PC_T_FAIL("%s", "builder");
                    continue;
                }
                pc_psd_builder_color_mode(b, mode);
                pc_psd_builder_depth(b, depth);
                pc_psd_builder_compression(b, c == 0 ? PC_PSD_RAW : PC_PSD_RLE);
                pc_psd_builder_version(b, v == 0 ? PC_PSD_V1 : PC_PSD_V2);
                CHECK_EQ_INT(pc_psd_builder_resolution(b, 150.0, NULL), PC_OK);
                CHECK_EQ_INT(pc_psd_builder_icc(b, icc, 3, NULL), PC_OK);
                l1 = pc_psd_layer_spec_make("Bottom \xC3\xA9", -2, 1, 4, 3, make_px(mode, depth, 12, 1, &o1));
                ms.rect = pc_psd_rect_from_xywh(0, 0, 3, 2);
                ms.data = mdata;
                ms.len = 6;
                ms.default_color = 255;
                ms.disabled = true;
                l1.mask = &ms;
                l1.has_fill_opacity = true;
                l1.fill_opacity = 77;
                memcpy(l1.blend, "mul ", 4);
                CHECK_EQ_INT(pc_psd_builder_push_layer(b, &l1, NULL), PC_OK);
                g = pc_psd_group_spec_make("Group");
                memcpy(g.blend, "scrn", 4);
                g.opacity = 200;
                g.visible = false;
                g.open = false;
                CHECK_EQ_INT(pc_psd_builder_begin_group(b, &g, NULL), PC_OK);
                l2 = pc_psd_layer_spec_make("In group", 1, 1, 2, 2, make_px(mode, depth, 4, 2, &o2));
                l2.visible = false;
                l2.clipping = true;
                l2.opacity = 100;
                CHECK_EQ_INT(pc_psd_builder_push_layer(b, &l2, NULL), PC_OK);
                CHECK_EQ_INT(pc_psd_builder_end_group(b, NULL), PC_OK);
                CHECK_EQ_INT(pc_psd_builder_composite(b, make_px(mode, depth, 35, 3, &o3), NULL), PC_OK);
                if (pc_psd_builder_to_bytes(b, &bytes, &e) == PC_OK) {
                    fnv(&hb, bytes.data, bytes.len);
                    hr = readback(&bytes);
                } else {
                    fnv_str(&hb, e.msg);
                }
                (void)snprintf(name, sizeof name, "%s %u %s %s", MN[m], (unsigned)depth, c == 0 ? "Raw" : "Rle",
                               v == 0 ? "Psd" : "Psb");
                (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)hb);
                want = expect("builder", name);
                pc_t_name = name;
                CHECK(want != NULL && strncmp(want, got, 16) == 0);
                (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)hr);
                want = expect("readback", name);
                CHECK(want != NULL && strncmp(want, got, 16) == 0);
                pc_psd_bytes_fini(&bytes);
                pc_psd_builder_free(b);
                free(o1);
                free(o2);
                free(o3);
            }
        }
    }
}

static void test_misc_writers(void)
{
    uint64_t hx = FNV0;
    pc_psd_desc d = tg_sample_descriptor();
    pc_psd_bytes b = {NULL, 0};
    pc_psd_slices sl;
    pc_psd_subpath sp;
    pc_psd_knot kn;
    pc_psd_path path;
    pc_psd_filter_effects fx;
    pc_psd_fx_item it;
    pc_psd_rect rect = {1, 2, 5, 6};
    uint8_t ten[16];
    uint8_t full[16];
    uint8_t half[16];
    char got[20];
    const char *want = expect("misc", "");
    CHECK_EQ_INT(pc_psd_vdesc_to_bytes(&d, &b, NULL), PC_OK);
    fnv(&hx, b.data, b.len);
    pc_psd_bytes_fini(&b);
    pc_psd_desc_fini(&d);
    CHECK_EQ_INT(pc_psd_default_toning_data(&b, NULL), PC_OK);
    fnv(&hx, b.data, b.len);
    pc_psd_bytes_fini(&b);
    memset(&sl, 0, sizeof sl);
    sl.version = 6;
    sl.bounds[2] = 80;
    sl.bounds[3] = 100;
    sl.group_name = "site";
    sl.n = 2;
    sl.slices = calloc(2, sizeof *sl.slices);
    if (sl.slices == NULL || pc_psd_slice_init(&sl.slices[0], NULL) != PC_OK ||
        pc_psd_slice_init(&sl.slices[1], NULL) != PC_OK) {
        PC_T_FAIL("%s", "slices");
        return;
    }
    sl.slices[0].id = 1;
    sl.slices[0].origin = 2;
    free(sl.slices[0].name);
    sl.slices[0].name = strdup("logo");
    sl.slices[0].kind = 1;
    sl.slices[0].rect[0] = 20;
    sl.slices[0].rect[1] = 10;
    sl.slices[0].rect[2] = 60;
    sl.slices[0].rect[3] = 30;
    free(sl.slices[0].url);
    sl.slices[0].url = strdup("u");
    sl.slices[0].color[0] = 255;
    sl.slices[0].color[1] = 1;
    sl.slices[0].color[2] = 2;
    sl.slices[0].color[3] = 3;
    sl.slices[1].id = 2;
    sl.slices[1].origin = 1;
    sl.slices[1].has_layer_id = true;
    sl.slices[1].layer_id = 7;
    sl.slices[1].rect[0] = 5;
    sl.slices[1].rect[1] = 40;
    sl.slices[1].rect[2] = 25;
    sl.slices[1].rect[3] = 70;
    sl.slices[1].outsets[0] = 1;
    sl.slices[1].outsets[1] = 2;
    sl.slices[1].outsets[2] = 3;
    sl.slices[1].outsets[3] = 4;
    CHECK_EQ_INT(pc_psd_slices_to_bytes(&sl, &b, NULL), PC_OK);
    fnv(&hx, b.data, b.len);
    pc_psd_bytes_fini(&b);
    CHECK_EQ_INT(pc_psd_slices_to_descriptor_bytes(&sl, &b, NULL), PC_OK);
    fnv(&hx, b.data, b.len);
    pc_psd_bytes_fini(&b);
    sl.group_name = NULL; /* static string */
    pc_psd_slices_fini(&sl);
    kn.linked = true;
    kn.pre[0] = 0.1;
    kn.pre[1] = -0.2;
    kn.anchor[0] = 0.5;
    kn.anchor[1] = 0.5;
    kn.post[0] = 1.25;
    kn.post[1] = 3.0;
    sp.closed = true;
    sp.operation = 2;
    sp.knots = &kn;
    sp.n = 1;
    CHECK_EQ_INT(pc_psd_path_from_subpaths(&sp, 1, true, &path, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_path_to_bytes(&path, &b, NULL), PC_OK);
    fnv(&hx, b.data, b.len);
    pc_psd_bytes_fini(&b);
    pc_psd_path_fini(&path);
    memset(ten, 10, 16);
    memset(full, 255, 16);
    memset(half, 128, 16);
    memset(&it, 0, sizeof it);
    it.id = strdup("a-1");
    it.version = 1;
    it.rect = rect;
    it.depth = 8;
    it.max_channels = 2;
    it.nslots = 4;
    it.slots = calloc(4, sizeof *it.slots);
    it.slot_written = calloc(4, sizeof *it.slot_written);
    if (it.id == NULL || it.slots == NULL || it.slot_written == NULL) {
        PC_T_FAIL("%s", "alloc");
        return;
    }
    CHECK_EQ_INT(pc_psd_fx_plane_encode(PC_PSD_RLE, ten, 16, 4, 4, 8, &it.slots[0], NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_fx_plane_encode(PC_PSD_RLE, full, 16, 4, 4, 8, &it.slots[3], NULL), PC_OK);
    it.slot_written[0] = true;
    it.slot_written[3] = true;
    it.has_mask = true;
    it.mask_rect = rect;
    CHECK_EQ_INT(pc_psd_fx_plane_encode(PC_PSD_RLE, half, 16, 4, 4, 8, &it.mask, NULL), PC_OK);
    fx.version = 3;
    fx.items = &it;
    fx.n = 1;
    CHECK_EQ_INT(pc_psd_filter_effects_to_bytes(&fx, &b, NULL), PC_OK);
    fnv(&hx, b.data, b.len);
    pc_psd_bytes_fini(&b);
    pc_psd_fx_item_fini(&it);
    (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)hx);
    CHECK(want != NULL && strncmp(want, got, 16) == 0);
}

int main(int argc, char **argv)
{
    if (pc_file_read("tests/golden/psd-vectors.txt", (size_t)1 << 20, &golden, NULL) != PC_OK) {
        (void)printf("tests/golden/psd-vectors.txt missing\n");
        return 1;
    }
    RUN(test_testgen_cases);
    RUN(test_builder_documents);
    RUN(test_misc_writers);
    RUN(test_unmatte);
    pc_buf_fini(&golden);
    return pc_t_finish();
}
