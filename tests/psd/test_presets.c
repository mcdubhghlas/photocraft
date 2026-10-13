/* Ports the unit tests of crates/psd/src/shc.rs (contour presets) and the preset-hierarchy
 * (`phry`) tests of crates/psd/src/abr.rs. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "harness.h"
#include "psd/abr.h"
#include "psd/descriptor.h"
#include "psd/internal.h"
#include "psd/patterns.h"
#include "psd/shc.h"

/* ---- shc.rs ---- */

static void put16(pc_buf *b, uint16_t v)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    (void)pc_buf_append(b, x, 2, NULL);
}

static void put32(pc_buf *b, uint32_t v)
{
    uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    (void)pc_buf_append(b, x, 4, NULL);
}

static void test_shc_round_trips_and_rejects_hostile_input(void)
{
    pc_psd_shc_point pts[3] = {{0.0f, 0.0f, true}, {128.0f, 255.0f, false}, {255.0f, 0.0f, true}};
    pc_psd_shc_contour cone;
    pc_psd_shc_contour *back = NULL;
    pc_psd_bytes bytes = {NULL, 0};
    pc_buf v1 = PC_BUF_INIT;
    size_t n = 0;
    size_t cut;
    memset(&cone, 0, sizeof cone);
    CHECK_EQ_INT(pc_psd_str_set(&cone.name, "Cone", 4, NULL), PC_OK);
    cone.points = pts;
    cone.npoints = 3;
    CHECK_EQ_INT(pc_psd_shc_write(&cone, 1, &bytes, NULL), PC_OK);
    CHECK(bytes.len >= 4 && memcmp(bytes.data, "8BFS", 4) == 0);
    CHECK_EQ_INT(pc_psd_shc_parse(bytes.data, bytes.len, &back, &n, NULL), PC_OK);
    CHECK_EQ_UINT(n, 1);
    if (n == 1) {
        CHECK(pc_psd_shc_contour_eq(&back[0], &cone));
    }
    pc_psd_shc_free(back, n);

    /* Version 1 without corner flags, with a name count that leaves out the null. */
    (void)pc_buf_append(&v1, "8BFS", 4, NULL);
    put16(&v1, 1);
    put32(&v1, 1);
    put32(&v1, 1);
    put32(&v1, 2);
    put16(&v1, 'O');
    put16(&v1, 'K');
    put16(&v1, 0);
    put16(&v1, 2);
    put16(&v1, 0);
    put16(&v1, 0);
    put16(&v1, 255);
    put16(&v1, 255);
    put32(&v1, 0);
    put32(&v1, 255);
    CHECK_EQ_INT(pc_psd_shc_parse(v1.data, v1.len, &back, &n, NULL), PC_OK);
    if (n == 1) {
        CHECK_EQ_STR(pc_psd_str_c(&back[0].name), "OK");
        CHECK(back[0].npoints == 2 && back[0].points[0].continuous && back[0].points[1].continuous);
        CHECK(back[0].points[1].input == 255.0f);
    }
    pc_psd_shc_free(back, n);

    /* Truncations, a wrong signature and absurd counts are errors, never crashes. */
    for (cut = 0; cut < bytes.len; cut++) {
        uint8_t *copy = malloc(cut + 1);
        if (copy != NULL) {
            memcpy(copy, bytes.data, cut);
            back = NULL;
            CHECK(pc_psd_shc_parse(copy, cut, &back, &n, NULL) != PC_OK);
            free(copy);
        }
    }
    CHECK(pc_psd_shc_parse((const uint8_t *)"8BFX\0\x01\0\0\0\0", 10, &back, &n, NULL) != PC_OK);
    CHECK(pc_psd_shc_parse((const uint8_t *)"8BFS\0\x01\xff\xff\xff\xff", 10, &back, &n, NULL) != PC_OK);
    {
        pc_psd_shc_contour many;
        pc_psd_shc_point *p = calloc(PC_PSD_SHC_MAX_POINTS + 1, sizeof *p);
        pc_psd_bytes b = {NULL, 0};
        memset(&many, 0, sizeof many);
        many.points = p;
        many.npoints = PC_PSD_SHC_MAX_POINTS + 1;
        CHECK_EQ_INT(pc_psd_shc_write(&many, 1, &b, NULL), PC_ERR_LIMIT);
        free(p);
    }
    /* Every byte mutated: errors or values, never a crash. */
    for (cut = 0; cut < bytes.len; cut++) {
        uint8_t keep = bytes.data[cut];
        bytes.data[cut] ^= 0x5a;
        if (pc_psd_shc_parse(bytes.data, bytes.len, &back, &n, NULL) == PC_OK) {
            pc_psd_shc_free(back, n);
        }
        bytes.data[cut] = keep;
    }
    /* Non-finite and out-of-range levels write as 0 / clamped. */
    {
        pc_psd_shc_point odd[2] = {{NAN, INFINITY, true}, {-5.0f, 300.0f, false}};
        pc_psd_shc_contour c;
        pc_psd_bytes b = {NULL, 0};
        memset(&c, 0, sizeof c);
        c.points = odd;
        c.npoints = 2;
        CHECK_EQ_INT(pc_psd_shc_write(&c, 1, &b, NULL), PC_OK);
        CHECK_EQ_INT(pc_psd_shc_parse(b.data, b.len, &back, &n, NULL), PC_OK);
        if (n == 1 && back[0].npoints == 2) {
            CHECK(back[0].points[0].input == 0.0f && back[0].points[0].output == 0.0f);
            CHECK(back[0].points[1].input == 0.0f && back[0].points[1].output == 255.0f);
        }
        pc_psd_shc_free(back, n);
        pc_psd_bytes_fini(&b);
    }
    pc_psd_str_fini(&cone.name);
    pc_psd_bytes_fini(&bytes);
    pc_buf_fini(&v1);
}

/* ---- abr.rs: phry ---- */

static pc_psd_desc named(const char *n)
{
    pc_psd_desc d;
    pc_psd_value v;
    (void)pc_psd_desc_init(&d, "brushPreset", NULL);
    if (pc_psd_value_text(n, true, &v, NULL) == PC_OK) {
        (void)pc_psd_desc_add(&d, "Nm  ", &v, NULL);
    }
    return d;
}

/* Hierarchy tokens: 'G' + name opens, "P" is a preset, "E" closes. */
static pc_psd_bytes phry_bytes(const char *const *toks, size_t n)
{
    pc_psd_value list;
    pc_psd_desc root;
    pc_psd_bytes out = {NULL, 0};
    size_t i;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (i = 0; i < n; i++) {
        pc_psd_desc d;
        pc_psd_value v;
        if (toks[i][0] == 'G') {
            (void)pc_psd_desc_init(&d, "Grup", NULL);
            if (pc_psd_value_text(toks[i] + 1, true, &v, NULL) == PC_OK) {
                (void)pc_psd_desc_add(&d, "Nm  ", &v, NULL);
            }
            if (pc_psd_value_text("c9ee8fd5-eb7d-924c-9e95-ddc98e40bb6c", true, &v, NULL) == PC_OK) {
                (void)pc_psd_desc_add(&d, "zuid", &v, NULL);
            }
        } else {
            (void)pc_psd_desc_init(&d, toks[i][0] == 'P' ? "preset" : "groupEnd", NULL);
        }
        if (pc_psd_value_object(&d, &v, NULL) == PC_OK) {
            (void)pc_psd_value_list_push(&list, &v, NULL);
        }
    }
    (void)pc_psd_desc_init(&root, "null", NULL);
    (void)pc_psd_desc_add(&root, "hierarchy", &list, NULL);
    (void)pc_psd_vdesc_to_bytes(&root, &out, NULL);
    pc_psd_desc_fini(&root);
    return out;
}

static void append_section(pc_buf *b, const char key[4], const uint8_t *data, size_t len)
{
    size_t pad = (4 - len % 4) % 4;
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    (void)pc_buf_append(b, "8BIM", 4, NULL);
    (void)pc_buf_append(b, key, 4, NULL);
    put32(b, (uint32_t)(len + pad));
    (void)pc_buf_append(b, data, len, NULL);
    (void)pc_buf_append(b, zeros, pad, NULL);
}

/* A v6 file with n presets B0..B(n-1), then a hand-made phry section. */
static pc_buf with_phry(size_t n, const char *const *toks, size_t ntoks)
{
    pc_psd_desc *presets = calloc(n > 0 ? n : 1, sizeof *presets);
    pc_psd_bytes file = {NULL, 0};
    pc_psd_bytes ph;
    pc_buf b = PC_BUF_INIT;
    size_t i;
    for (i = 0; presets != NULL && i < n; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "B%zu", i);
        presets[i] = named(name);
    }
    (void)pc_psd_abr_write_v6(2, NULL, 0, NULL, 0, presets, n, false, &file, NULL);
    (void)pc_buf_append(&b, file.data, file.len, NULL);
    ph = phry_bytes(toks, ntoks);
    append_section(&b, "phry", ph.data, ph.len);
    pc_psd_bytes_fini(&ph);
    pc_psd_bytes_fini(&file);
    for (i = 0; presets != NULL && i < n; i++) {
        pc_psd_desc_fini(&presets[i]);
    }
    free(presets);
    return b;
}

/* "A/B" style path of folder i ("" at the top level). */
static void path_of(const pc_psd_abr *f, size_t i, char *out, size_t cap)
{
    size_t k;
    size_t at = 0;
    out[0] = '\0';
    if (i >= f->nfolders) {
        return;
    }
    for (k = 0; k < f->folders[i].n; k++) {
        int w = snprintf(out + at, cap - at, "%s%s", k > 0 ? "/" : "", pc_psd_str_c(&f->folders[i].names[k]));
        if (w < 0 || (size_t)w >= cap - at) {
            return;
        }
        at += (size_t)w;
    }
}

static bool has_warning(const pc_psd_abr *f, const char *needle)
{
    size_t i;
    for (i = 0; i < f->warnings.n; i++) {
        if (strstr(f->warnings.v[i], needle) != NULL) {
            return true;
        }
    }
    return false;
}

#define NT(a) (sizeof(a) / sizeof(a)[0])

static void check_paths(const pc_buf *b, const char *const *want, size_t n, bool no_warnings)
{
    pc_psd_abr f;
    size_t i;
    char got[512];
    CHECK_EQ_INT(pc_psd_abr_parse(b->data, b->len, &f, NULL), PC_OK);
    CHECK_EQ_UINT(f.nfolders, n);
    for (i = 0; i < n && i < f.nfolders; i++) {
        path_of(&f, i, got, sizeof got);
        CHECK_EQ_STR(got, want[i]);
    }
    if (no_warnings) {
        CHECK_EQ_UINT(f.warnings.n, 0);
    }
    pc_psd_abr_fini(&f);
}

static void test_phry_flat_folders_like_photoshop_25(void)
{
    static const char *const toks[] = {"GGeneral", "P",          "P", "E", "GDry Media",       "P",
                                       "E",        "GWet Media", "P", "E", "GSpecial Effects", "P",
                                       "E"};
    static const char *const want[] = {"General", "General", "Dry Media", "Wet Media", "Special Effects"};
    pc_buf b = with_phry(5, toks, NT(toks));
    check_paths(&b, want, NT(want), true);
    pc_buf_fini(&b);
}

static void test_phry_nested_folders_and_top_level_presets(void)
{
    static const char *const toks[] = {"P", "GAnimals", "GBirds", "P", "E", "P", "GFish", "P", "E", "E", "P"};
    static const char *const want[] = {"", "Animals/Birds", "Animals", "Animals/Fish", ""};
    static const char *const flat[] = {"P", "P"};
    pc_buf b = with_phry(5, toks, NT(toks));
    pc_psd_abr f;
    check_paths(&b, want, NT(want), true);
    pc_buf_fini(&b);
    /* No folder at all: no hierarchy. */
    b = with_phry(2, flat, NT(flat));
    CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK_EQ_UINT(f.nfolders, 0);
    pc_psd_abr_fini(&f);
    pc_buf_fini(&b);
}

static void test_phry_mismatches_become_warnings(void)
{
    static const char *const stray[] = {"E", "GA", "P", "E", "E", "GB", "P"};
    static const char *const stray_want[] = {"A", "B"};
    static const char *const few[] = {"GA", "P", "E"};
    static const char *const few_want[] = {"A", "", ""};
    static const char *const extra[] = {"GA", "P", "P", "P", "E"};
    static const char *const good_toks[] = {"GA", "P", "GB", "P", "E", "E", "P"};
    pc_psd_abr f;
    pc_buf b = with_phry(2, stray, NT(stray));
    check_paths(&b, stray_want, NT(stray_want), false);
    CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK(has_warning(&f, "folder end"));
    pc_psd_abr_fini(&f);
    pc_buf_fini(&b);
    /* Fewer preset tokens than presets: the rest are at the top level. */
    b = with_phry(3, few, NT(few));
    check_paths(&b, few_want, NT(few_want), false);
    CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK(has_warning(&f, "list 1 brushes but the file holds 3"));
    pc_psd_abr_fini(&f);
    pc_buf_fini(&b);
    /* More preset tokens than presets: extras ignored. */
    b = with_phry(1, extra, NT(extra));
    CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK_EQ_UINT(f.nfolders, 1);
    CHECK_EQ_UINT(f.npresets, 1);
    CHECK(f.warnings.n > 0);
    pc_psd_abr_fini(&f);
    pc_buf_fini(&b);
    /* A depth bomb: folders deeper than the limit merge into their ancestor at the limit. */
    {
        size_t nt = 10000 + 1 + 10000 + 3;
        const char **toks = calloc(nt, sizeof *toks);
        size_t i;
        size_t k = 0;
        if (toks != NULL) {
            for (i = 0; i < 10000; i++) {
                toks[k++] = "Gdeep";
            }
            toks[k++] = "P";
            for (i = 0; i < 10000; i++) {
                toks[k++] = "E";
            }
            toks[k++] = "Gnext";
            toks[k++] = "P";
            toks[k++] = "E";
            b = with_phry(2, toks, k);
            CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
            CHECK_EQ_UINT(f.nfolders, 2);
            if (f.nfolders == 2) {
                char got[64];
                CHECK_EQ_UINT(f.folders[0].n, PC_PSD_ABR_MAX_FOLDER_DEPTH);
                path_of(&f, 1, got, sizeof got);
                CHECK_EQ_STR(got, "next");
            }
            CHECK(has_warning(&f, "deeper"));
            pc_psd_abr_fini(&f);
            pc_buf_fini(&b);
            free(toks);
        }
    }
    /* An unreadable hierarchy: imported flat, with a warning. */
    {
        pc_psd_desc x = named("x");
        pc_psd_bytes file = {NULL, 0};
        static const uint8_t junk[] = {0, 0, 0, 16, 0xff, 0xff};
        CHECK_EQ_INT(pc_psd_abr_write_v6(2, NULL, 0, NULL, 0, &x, 1, false, &file, NULL), PC_OK);
        b = (pc_buf)PC_BUF_INIT;
        (void)pc_buf_append(&b, file.data, file.len, NULL);
        append_section(&b, "phry", junk, sizeof junk);
        CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
        CHECK(f.nfolders == 0 && f.npresets == 1);
        CHECK(has_warning(&f, "phry"));
        pc_psd_abr_fini(&f);
        pc_buf_fini(&b);
        pc_psd_bytes_fini(&file);
        pc_psd_desc_fini(&x);
    }
    /* Truncations and bit flips of a file with a hierarchy never crash. */
    {
        pc_buf good = with_phry(3, good_toks, NT(good_toks));
        size_t i;
        for (i = 0; i < good.len; i++) {
            if (pc_psd_abr_parse(good.data, i, &f, NULL) == PC_OK) {
                pc_psd_abr_fini(&f);
            }
        }
        for (i = 0; i < good.len; i++) {
            good.data[i] ^= 0x5a;
            if (pc_psd_abr_parse(good.data, good.len, &f, NULL) == PC_OK) {
                pc_psd_abr_fini(&f);
            }
            good.data[i] ^= 0x5a;
        }
        pc_buf_fini(&good);
    }
}

static void test_phry_builds_paths_only_for_the_files_presets(void)
{
    /* A small section listing 100,000 deep presets for a file with 2: two paths are built. */
    size_t nt = PC_PSD_ABR_MAX_FOLDER_DEPTH + 100000;
    const char **toks = calloc(nt, sizeof *toks);
    pc_psd_warnings w = {NULL, 0};
    pc_psd_abr_folder *folders = NULL;
    size_t nf = 0;
    pc_psd_bytes ph;
    size_t i;
    if (toks == NULL) {
        CHECK(toks != NULL);
        return;
    }
    for (i = 0; i < nt; i++) {
        toks[i] = i < PC_PSD_ABR_MAX_FOLDER_DEPTH ? "Gfolder" : "P";
    }
    ph = phry_bytes(toks, nt);
    CHECK_EQ_INT(psd_abr_read_phry(ph.data, ph.len, 2, &w, &folders, &nf, NULL), PC_OK);
    CHECK_EQ_UINT(nf, 2);
    if (nf == 2) {
        CHECK_EQ_UINT(folders[1].n, PC_PSD_ABR_MAX_FOLDER_DEPTH);
    }
    CHECK(w.n > 0 && strstr(w.v[w.n - 1], "list 100000 brushes but the file holds 2") != NULL);
    pc_psd_abr_folders_free(folders, nf);
    pc_psd_warnings_fini(&w);
    pc_psd_bytes_fini(&ph);
    free(toks);
    /* phry before desc still pairs with the file's presets. */
    {
        static const char *const t2[] = {"GA", "P", "E", "P"};
        pc_psd_desc p[2];
        pc_psd_bytes file = {NULL, 0};
        pc_buf b = PC_BUF_INIT;
        pc_psd_abr f;
        size_t at = 0;
        p[0] = named("a");
        p[1] = named("b");
        CHECK_EQ_INT(pc_psd_abr_write_v6(2, NULL, 0, NULL, 0, p, 2, false, &file, NULL), PC_OK);
        for (i = 0; i + 8 <= file.len; i++) {
            if (memcmp(file.data + i, "8BIMdesc", 8) == 0) {
                at = i;
                break;
            }
        }
        CHECK(at > 0);
        ph = phry_bytes(t2, NT(t2));
        (void)pc_buf_append(&b, file.data, at, NULL);
        append_section(&b, "phry", ph.data, ph.len);
        (void)pc_buf_append(&b, file.data + at, file.len - at, NULL);
        CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
        CHECK_EQ_UINT(f.nfolders, 2);
        if (f.nfolders == 2) {
            CHECK_EQ_UINT(f.folders[0].n, 1);
            CHECK_EQ_UINT(f.folders[1].n, 0);
        }
        CHECK_EQ_UINT(f.warnings.n, 0);
        pc_psd_abr_fini(&f);
        pc_buf_fini(&b);
        pc_psd_bytes_fini(&ph);
        pc_psd_bytes_fini(&file);
        pc_psd_desc_fini(&p[0]);
        pc_psd_desc_fini(&p[1]);
    }
}

static void test_phry_folder_paths_stay_within_their_byte_budget(void)
{
    /* Thousands of presets under 32 folders of 255-character names would need ~27 MB of paths. */
    char lng[257];
    size_t nt = PC_PSD_ABR_MAX_FOLDER_DEPTH + 3000;
    const char **toks = calloc(nt, sizeof *toks);
    pc_psd_warnings w = {NULL, 0};
    pc_psd_abr_folder *folders = NULL;
    size_t nf = 0;
    size_t held = 0;
    pc_psd_bytes ph;
    size_t i;
    size_t k;
    if (toks == NULL) {
        CHECK(toks != NULL);
        return;
    }
    lng[0] = 'G';
    memset(lng + 1, 'x', 255);
    lng[256] = '\0';
    for (i = 0; i < nt; i++) {
        toks[i] = i < PC_PSD_ABR_MAX_FOLDER_DEPTH ? lng : "P";
    }
    ph = phry_bytes(toks, nt);
    CHECK_EQ_INT(psd_abr_read_phry(ph.data, ph.len, 3000, &w, &folders, &nf, NULL), PC_OK);
    CHECK_EQ_UINT(nf, 3000);
    for (i = 0; i < nf; i++) {
        for (k = 0; k < folders[i].n; k++) {
            held += folders[i].names[k].len + 24;
        }
    }
    CHECK(held <= ((size_t)16 << 20));
    if (nf == 3000) {
        CHECK_EQ_UINT(folders[0].n, PC_PSD_ABR_MAX_FOLDER_DEPTH);
        CHECK_EQ_UINT(folders[0].names[0].len, 255);
        CHECK_EQ_UINT(folders[2999].n, 0);
    }
    CHECK(w.n > 0 && strstr(w.v[0], "too large") != NULL);
    pc_psd_abr_folders_free(folders, nf);
    pc_psd_warnings_fini(&w);
    pc_psd_bytes_fini(&ph);
    free(toks);
}

static void make_path(pc_psd_abr_folder *f, const char *a, const char *b)
{
    memset(f, 0, sizeof *f);
    f->names = calloc(2, sizeof *f->names);
    if (f->names == NULL) {
        return;
    }
    if (a != NULL) {
        (void)pc_psd_str_set(&f->names[f->n++], a, strlen(a), NULL);
    }
    if (b != NULL) {
        (void)pc_psd_str_set(&f->names[f->n++], b, strlen(b), NULL);
    }
}

static void test_phry_round_trips_through_the_writer(void)
{
    pc_psd_desc presets[6];
    pc_psd_abr_folder folders[6];
    pc_psd_abr_folder empty[6];
    pc_psd_bytes bytes = {NULL, 0};
    pc_psd_bytes plain = {NULL, 0};
    pc_psd_bytes none = {NULL, 0};
    pc_psd_abr f;
    size_t i;
    for (i = 0; i < 6; i++) {
        char name[8];
        (void)snprintf(name, sizeof name, "B%zu", i);
        presets[i] = named(name);
    }
    make_path(&folders[0], NULL, NULL);
    make_path(&folders[1], "Set", "Ink");
    make_path(&folders[2], "Set", "Ink");
    make_path(&folders[3], "Set", NULL);
    make_path(&folders[4], "Other", NULL);
    make_path(&folders[5], "Set", "Ink");
    memset(empty, 0, sizeof empty);
    CHECK_EQ_INT(pc_psd_abr_write_v6_folders(2, NULL, 0, NULL, 0, presets, 6, folders, 6, true, &bytes, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_psd_abr_parse(bytes.data, bytes.len, &f, NULL), PC_OK);
    CHECK_EQ_UINT(f.npresets, 6);
    for (i = 0; i < 6 && i < f.npresets; i++) {
        CHECK(pc_psd_desc_eq(&f.presets[i], &presets[i]));
    }
    CHECK_EQ_UINT(f.nfolders, 6);
    for (i = 0; i < 6 && i < f.nfolders; i++) {
        CHECK(pc_psd_abr_folder_eq(&f.folders[i], &folders[i]));
    }
    CHECK_EQ_UINT(f.warnings.n, 0);
    pc_psd_abr_fini(&f);
    /* No folders: no phry section, same bytes as write_v6. */
    CHECK_EQ_INT(pc_psd_abr_write_v6_folders(2, NULL, 0, NULL, 0, presets, 6, empty, 6, true, &none, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_psd_abr_write_v6(2, NULL, 0, NULL, 0, presets, 6, true, &plain, NULL), PC_OK);
    CHECK(none.len == plain.len && memcmp(none.data, plain.data, none.len) == 0);
    /* Mismatched lengths are an error. */
    {
        pc_psd_bytes x = {NULL, 0};
        CHECK_EQ_INT(pc_psd_abr_write_v6_folders(2, NULL, 0, NULL, 0, presets, 6, folders, 2, true, &x, NULL),
                     PC_ERR_INVALID);
    }
    for (i = 0; i < 6; i++) {
        pc_psd_abr_folder_fini(&folders[i]);
        pc_psd_desc_fini(&presets[i]);
    }
    pc_psd_bytes_fini(&bytes);
    pc_psd_bytes_fini(&plain);
    pc_psd_bytes_fini(&none);
}

/* ---- golden parity: the f9-vectors lines R2 added (abr phry, abr phry misc, shc sample, shc misc)
 * with the dumps and sweeps of test_assets.c (same layout as tools/rustref f9_vectors) ---- */

#define FNV0 0xcbf29ce484222325u

static pc_buf golden;
static int compared;

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static void d_u16(pc_buf *o, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    (void)pc_buf_append(o, b, 2, NULL);
}

static void d_u32(pc_buf *o, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    (void)pc_buf_append(o, b, 4, NULL);
}

static void d_f32(pc_buf *o, float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    d_u32(o, u);
}

static void d_str(pc_buf *o, const void *b, size_t n)
{
    d_u32(o, (uint32_t)n);
    if (n > 0) {
        (void)pc_buf_append(o, b, n, NULL);
    }
}

static void d_byte(pc_buf *o, uint8_t v)
{
    (void)pc_buf_append_byte(o, v, NULL);
}

static void dump_sample(pc_buf *o, const pc_psd_abr_sample *s)
{
    d_str(o, s->id.s, s->id.len);
    d_u32(o, s->width);
    d_u32(o, s->height);
    d_u16(o, s->depth);
    d_str(o, s->data.data, s->data.len);
}

static void dump_pattern(pc_buf *o, const pc_psd_pattern *p)
{
    size_t i;
    d_u32(o, p->mode);
    d_u32(o, p->width);
    d_u32(o, p->height);
    d_str(o, p->name.s, p->name.len);
    d_str(o, p->id.s, p->id.len);
    d_byte(o, p->palette.has);
    d_str(o, p->palette.v.data, p->palette.v.len);
    d_u16(o, p->depth);
    d_u32(o, (uint32_t)p->nchannels);
    for (i = 0; i < p->nchannels; i++) {
        d_str(o, p->channels[i].data, p->channels[i].len);
    }
    d_byte(o, p->alpha.has);
    d_str(o, p->alpha.v.data, p->alpha.v.len);
}

/* abr_dump of rustref plus the folders (abr_folders_dump). */
static void dump_abr(pc_buf *o, const pc_psd_abr *f)
{
    size_t i;
    size_t k;
    d_u16(o, f->version);
    d_u16(o, f->subversion);
    d_u32(o, (uint32_t)f->nlegacy);
    for (i = 0; i < f->nlegacy; i++) {
        const pc_psd_abr_legacy *b = &f->legacy[i];
        d_str(o, b->name.s, b->name.len);
        d_u16(o, b->spacing);
        d_byte(o, b->anti_alias);
        if (b->tip == PC_PSD_ABR_COMPUTED) {
            d_byte(o, 0);
            d_u16(o, b->diameter);
            d_u16(o, b->hardness);
            d_u16(o, (uint16_t)b->angle);
            d_u16(o, b->roundness);
        } else {
            d_byte(o, 1);
            dump_sample(o, &b->sample);
        }
    }
    d_u32(o, (uint32_t)f->nsamples);
    for (i = 0; i < f->nsamples; i++) {
        dump_sample(o, &f->samples[i]);
    }
    d_u32(o, (uint32_t)f->npatterns);
    for (i = 0; i < f->npatterns; i++) {
        dump_pattern(o, &f->patterns[i]);
    }
    d_u32(o, (uint32_t)f->npresets);
    for (i = 0; i < f->npresets; i++) {
        pc_psd_bytes b = {NULL, 0};
        (void)pc_psd_desc_to_bytes(&f->presets[i], &b, NULL);
        d_str(o, b.data, b.len);
        pc_psd_bytes_fini(&b);
    }
    d_u32(o, (uint32_t)f->warnings.n);
    for (i = 0; i < f->warnings.n; i++) {
        d_str(o, f->warnings.v[i], strlen(f->warnings.v[i]));
    }
    d_u32(o, (uint32_t)f->nfolders);
    for (i = 0; i < f->nfolders; i++) {
        d_u32(o, (uint32_t)f->folders[i].n);
        for (k = 0; k < f->folders[i].n; k++) {
            d_str(o, f->folders[i].names[k].s, f->folders[i].names[k].len);
        }
    }
}

typedef pc_status (*parse_fn)(const uint8_t *d, size_t n, pc_buf *o, pc_error *e);

static pc_status p_abr(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_abr f;
    pc_status st = pc_psd_abr_parse(d, n, &f, e);
    if (st == PC_OK) {
        dump_abr(o, &f);
        pc_psd_abr_fini(&f);
    }
    return st;
}

static pc_status p_shc(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_shc_contour *c = NULL;
    size_t nc = 0;
    size_t i;
    size_t k;
    pc_status st = pc_psd_shc_parse(d, n, &c, &nc, e);
    if (st != PC_OK) {
        return st;
    }
    d_u32(o, (uint32_t)nc);
    for (i = 0; i < nc; i++) {
        d_str(o, c[i].name.s, c[i].name.len);
        d_u32(o, (uint32_t)c[i].npoints);
        for (k = 0; k < c[i].npoints; k++) {
            d_f32(o, c[i].points[k].input);
            d_f32(o, c[i].points[k].output);
            d_byte(o, c[i].points[k].continuous);
        }
    }
    pc_psd_shc_free(c, nc);
    return PC_OK;
}

static void outcome(uint64_t *h, parse_fn fn, const uint8_t *d, size_t n)
{
    pc_buf o = PC_BUF_INIT;
    pc_error e;
    (void)pc_buf_append(&o, "ok", 2, NULL);
    if (fn(d, n, &o, &e) == PC_OK) {
        fnv(h, o.data, o.len);
    } else {
        fnv(h, "err", 3);
        fnv(h, e.msg, strlen(e.msg));
    }
    pc_buf_fini(&o);
}

static void sweep(uint64_t *h, const uint8_t *d, size_t n, parse_fn fn)
{
    uint8_t *c = malloc(n > 0 ? n : 1);
    size_t i;
    fnv(h, d, n);
    outcome(h, fn, d, n);
    for (i = 0; i < n; i++) {
        outcome(h, fn, d, i);
    }
    for (i = 0; c != NULL && i < n; i++) {
        memcpy(c, d, n);
        c[i] ^= 0x5a;
        outcome(h, fn, c, n);
    }
    free(c);
}

static void bytes_or(uint64_t *h, pc_status st, pc_psd_bytes *b, const pc_error *e)
{
    if (st == PC_OK) {
        fnv(h, "ok", 2);
        fnv(h, b->data, b->len);
        pc_psd_bytes_fini(b);
    } else {
        fnv(h, "err", 3);
        fnv(h, e->msg, strlen(e->msg));
    }
}

static void expect(const char *name, uint64_t h)
{
    char key[160];
    char got[20];
    const char *p = (const char *)golden.data;
    const char *end = p + golden.len;
    size_t kl = (size_t)snprintf(key, sizeof key, "f9\t%s\t", name);
    (void)snprintf(got, sizeof got, "%016llx", (unsigned long long)h);
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl != NULL ? (size_t)(nl - p) : (size_t)(end - p);
        if (len >= kl + 16 && memcmp(p, key, kl) == 0) {
            compared++;
            if (memcmp(p + kl, got, 16) != 0) {
                PC_T_FAIL("%s: hash %s, want %.16s", name, got, p + kl);
            }
            return;
        }
        p += len + 1;
    }
    PC_T_FAIL("%s: no golden line", name);
}

static pc_psd_abr_sample abr_sample(const char *id, uint32_t w, uint32_t h, uint16_t depth)
{
    pc_psd_abr_sample s;
    size_t bpp = depth / 8u;
    size_t n = (size_t)w * h * bpp;
    size_t i;
    memset(&s, 0, sizeof s);
    (void)pc_psd_str_set(&s.id, id, strlen(id), NULL);
    s.width = w;
    s.height = h;
    s.depth = depth;
    s.data.data = malloc(n);
    s.data.len = s.data.data != NULL ? n : 0;
    for (i = 0; i < s.data.len; i++) {
        s.data.data[i] = (i / bpp) % 3 == 0 ? 255 : (uint8_t)((i % 7) * 30);
    }
    return s;
}

static void set_path(pc_psd_abr_folder *f, size_t n, const char *const *names)
{
    size_t i;
    memset(f, 0, sizeof *f);
    f->names = calloc(n > 0 ? n : 1, sizeof *f->names);
    for (i = 0; f->names != NULL && i < n; i++) {
        (void)pc_psd_str_set(&f->names[i], names[i], strlen(names[i]), NULL);
        f->n++;
    }
}

static pc_buf base_file(size_t n)
{
    pc_psd_desc *p = calloc(n > 0 ? n : 1, sizeof *p);
    pc_psd_bytes file = {NULL, 0};
    pc_buf b = PC_BUF_INIT;
    size_t i;
    for (i = 0; p != NULL && i < n; i++) {
        char name[32];
        (void)snprintf(name, sizeof name, "B%zu", i);
        p[i] = named(name);
    }
    (void)pc_psd_abr_write_v6(2, NULL, 0, NULL, 0, p, n, false, &file, NULL);
    (void)pc_buf_append(&b, file.data, file.len, NULL);
    pc_psd_bytes_fini(&file);
    for (i = 0; p != NULL && i < n; i++) {
        pc_psd_desc_fini(&p[i]);
    }
    free(p);
    return b;
}

static pc_buf base_with(size_t n, const char *const *toks, size_t nt)
{
    pc_buf b = base_file(n);
    pc_psd_bytes ph = phry_bytes(toks, nt);
    append_section(&b, "phry", ph.data, ph.len);
    pc_psd_bytes_fini(&ph);
    return b;
}

static void test_parity_abr_phry(void)
{
    static const char *const set_ink[2] = {"Set", "Ink \xe2\x9c\x93"};
    static const char *const set[1] = {"Set"};
    static const char *const other[1] = {"Other"};
    pc_psd_desc presets[6];
    pc_psd_abr_folder folders[6];
    pc_psd_abr_folder empty[6];
    pc_psd_abr_sample sample = abr_sample("$abc", 20, 11, 8);
    pc_psd_bytes b = {NULL, 0};
    pc_error e;
    uint64_t h = FNV0;
    size_t i;
    for (i = 0; i < 6; i++) {
        char name[8];
        (void)snprintf(name, sizeof name, "B%zu", i);
        presets[i] = named(name);
    }
    set_path(&folders[0], 0, NULL);
    set_path(&folders[1], 2, set_ink);
    set_path(&folders[2], 2, set_ink);
    set_path(&folders[3], 1, set);
    set_path(&folders[4], 1, other);
    set_path(&folders[5], 2, set_ink);
    memset(empty, 0, sizeof empty);
    CHECK_EQ_INT(pc_psd_abr_write_v6_folders(2, &sample, 1, NULL, 0, presets, 6, folders, 6, true, &b, NULL),
                 PC_OK);
    sweep(&h, b.data, b.len, p_abr);
    pc_psd_bytes_fini(&b);
    expect("abr phry", h);

    h = FNV0;
    {
        static const char *const c0[] = {"P",     "GAnimals", "GBirds", "P", "E", "P",
                                         "GFish", "P",        "E",      "E", "P"};
        static const char *const c1[] = {"E", "GA", "P", "E", "E", "GB", "P"};
        static const char *const c2[] = {"GA", "P", "E"};
        static const char *const c3[] = {"GA", "P", "P", "P", "E"};
        static const char *const c6[] = {"P", "P"};
        static const char *const c9[] = {"GA", "P", "E", "P"};
        static const uint8_t junk[6] = {0, 0, 0, 16, 0xff, 0xff};
        char deep[40][8];
        const char *deep_toks[85];
        size_t nd = 0;
        pc_buf longname = PC_BUF_INIT;
        const char *c5[5];
        pc_buf cases[10];
        for (i = 0; i < 40; i++) {
            (void)snprintf(deep[i], sizeof deep[i], "Gd%zu", i);
            deep_toks[nd++] = deep[i];
        }
        deep_toks[nd++] = "P";
        for (i = 0; i < 40; i++) {
            deep_toks[nd++] = "E";
        }
        deep_toks[nd++] = "G   ";
        deep_toks[nd++] = "P";
        deep_toks[nd++] = "E";
        deep_toks[nd++] = "P";
        (void)pc_buf_append(&longname, "G  ", 3, NULL);
        for (i = 0; i < 300; i++) {
            (void)pc_buf_append(&longname, "\xc3\xa9", 2, NULL);
        }
        (void)pc_buf_append(&longname, "  ", 2, NULL);
        (void)pc_buf_append_byte(&longname, 0, NULL);
        c5[0] = (const char *)longname.data;
        c5[1] = "P";
        c5[2] = "E";
        c5[3] = "G";
        c5[4] = "P";
        cases[0] = base_with(5, c0, sizeof c0 / sizeof *c0);
        cases[1] = base_with(2, c1, sizeof c1 / sizeof *c1);
        cases[2] = base_with(3, c2, sizeof c2 / sizeof *c2);
        cases[3] = base_with(1, c3, sizeof c3 / sizeof *c3);
        cases[4] = base_with(3, deep_toks, nd);
        cases[5] = base_with(2, c5, 5);
        cases[6] = base_with(2, c6, 2);
        cases[7] = base_file(1);
        append_section(&cases[7], "phry", junk, sizeof junk);
        cases[8] = base_file(1);
        {
            pc_psd_desc nul;
            pc_psd_bytes vb = {NULL, 0};
            (void)pc_psd_desc_init(&nul, "null", NULL);
            (void)pc_psd_vdesc_to_bytes(&nul, &vb, NULL);
            append_section(&cases[8], "phry", vb.data, vb.len);
            pc_psd_bytes_fini(&vb);
            pc_psd_desc_fini(&nul);
        }
        {
            pc_buf f = base_file(2);
            pc_buf s2 = PC_BUF_INIT;
            pc_psd_bytes ph = phry_bytes(c9, 4);
            size_t at = 0;
            for (i = 0; i + 8 <= f.len; i++) {
                if (memcmp(f.data + i, "8BIMdesc", 8) == 0) {
                    at = i;
                    break;
                }
            }
            cases[9] = (pc_buf)PC_BUF_INIT;
            (void)pc_buf_append(&cases[9], f.data, at, NULL);
            append_section(&s2, "phry", ph.data, ph.len);
            (void)pc_buf_append(&cases[9], s2.data, s2.len, NULL);
            (void)pc_buf_append(&cases[9], f.data + at, f.len - at, NULL);
            pc_buf_fini(&s2);
            pc_buf_fini(&f);
            pc_psd_bytes_fini(&ph);
        }
        for (i = 0; i < 10; i++) {
            outcome(&h, p_abr, cases[i].data, cases[i].len);
        }
        sweep(&h, cases[0].data, cases[0].len, p_abr);
        for (i = 0; i < 10; i++) {
            pc_buf_fini(&cases[i]);
        }
        pc_buf_fini(&longname);
    }
    bytes_or(&h, pc_psd_abr_write_v6_folders(2, NULL, 0, NULL, 0, presets, 6, folders, 2, true, &b, &e), &b,
             &e);
    bytes_or(&h, pc_psd_abr_write_v6_folders(2, NULL, 0, NULL, 0, presets, 6, empty, 6, true, &b, &e), &b,
             &e);
    {
        char names[40][8];
        const char *ptrs[40];
        static const char *const f0x[2] = {"f0", "x"};
        pc_psd_abr_folder two[2];
        for (i = 0; i < 40; i++) {
            (void)snprintf(names[i], sizeof names[i], "f%zu", i);
            ptrs[i] = names[i];
        }
        set_path(&two[0], 40, ptrs);
        set_path(&two[1], 2, f0x);
        bytes_or(&h, pc_psd_abr_write_v6_folders(1, NULL, 0, NULL, 0, presets, 2, two, 2, false, &b, &e), &b,
                 &e);
        pc_psd_abr_folder_fini(&two[0]);
        pc_psd_abr_folder_fini(&two[1]);
    }
    expect("abr phry misc", h);
    for (i = 0; i < 6; i++) {
        pc_psd_abr_folder_fini(&folders[i]);
        pc_psd_desc_fini(&presets[i]);
    }
    pc_psd_abr_sample_fini(&sample);
}

static void test_parity_shc(void)
{
    pc_psd_shc_point cone[3] = {{0.0f, 0.0f, true}, {128.0f, 255.0f, false}, {255.0f, 0.0f, true}};
    pc_psd_shc_point odd[3] = {{300.0f, -5.0f, true}, {NAN, INFINITY, false}, {127.5f, 0.49f, true}};
    pc_psd_shc_contour c[3];
    pc_psd_bytes b = {NULL, 0};
    pc_error e;
    uint64_t h = FNV0;
    size_t i;
    memset(c, 0, sizeof c);
    (void)pc_psd_str_set(&c[0].name, "Cone \xe2\x9c\x93", 8, NULL);
    c[0].points = cone;
    c[0].npoints = 3;
    (void)pc_psd_str_set(&c[2].name, "Odd", 3, NULL);
    c[2].points = odd;
    c[2].npoints = 3;
    CHECK_EQ_INT(pc_psd_shc_write(c, 3, &b, NULL), PC_OK);
    sweep(&h, b.data, b.len, p_shc);
    pc_psd_bytes_fini(&b);
    expect("shc sample", h);
    h = FNV0;
    {
        static const uint8_t v1[] = {'8', 'B', 'F', 'S', 0,   1, 0, 0,   0, 2, 0, 0, 0, 1, 0,  0, 0,
                                     2,   0,   'O', 0,   'K', 0, 0, 0,   2, 0, 0, 0, 0, 1, 0,  0, 255,
                                     0,   0,   0,   0,   0,   0, 0, 255, 0, 0, 0, 1, 0, 0, 0,  0, 0,
                                     0,   0,   1,   0,   9,   0, 9, 0,   0, 0, 0, 0, 0, 0, 255};
        static const char *const misc[6] = {
            "",
            "8BFX\0\x01\0\0\0\0",
            "8BFS\0\x02\0\0\0\0",
            "8BFS\0\x01\xff\xff\xff\xff",
            "8BFS\0\x01\0\0\0\0",
            "8BFS\0\x01\0\0\0\x01\0\0\0\x03\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0"};
        static const size_t lens[6] = {0, 10, 10, 10, 10, 32};
        pc_psd_shc_contour many;
        pc_psd_shc_point *mp = calloc(PC_PSD_SHC_MAX_POINTS + 1, sizeof *mp);
        pc_psd_shc_point one = {1.0f, 2.0f, true};
        pc_psd_shc_contour lng;
        pc_buf emoji = PC_BUF_INIT;
        sweep(&h, v1, sizeof v1, p_shc);
        for (i = 0; i < 6; i++) {
            outcome(&h, p_shc, (const uint8_t *)misc[i], lens[i]);
        }
        memset(&many, 0, sizeof many);
        for (i = 0; mp != NULL && i <= PC_PSD_SHC_MAX_POINTS; i++) {
            mp[i].continuous = true;
        }
        many.points = mp;
        many.npoints = PC_PSD_SHC_MAX_POINTS + 1;
        bytes_or(&h, pc_psd_shc_write(&many, 1, &b, &e), &b, &e);
        free(mp);
        for (i = 0; i < 600; i++) {
            (void)pc_buf_append(&emoji, "\xF0\x9F\x98\x80", 4, NULL);
        }
        memset(&lng, 0, sizeof lng);
        (void)pc_psd_str_set(&lng.name, (const char *)emoji.data, emoji.len, NULL);
        lng.points = &one;
        lng.npoints = 1;
        bytes_or(&h, pc_psd_shc_write(&lng, 1, &b, &e), &b, &e);
        pc_psd_str_fini(&lng.name);
        pc_buf_fini(&emoji);
    }
    expect("shc misc", h);
    pc_psd_str_fini(&c[0].name);
    pc_psd_str_fini(&c[2].name);
}

int main(int argc, char **argv)
{
    if (pc_file_read("tests/golden/f9-vectors.txt", (size_t)1 << 20, &golden, NULL) != PC_OK) {
        (void)printf("tests/golden/f9-vectors.txt missing\n");
        return 1;
    }
    RUN(test_parity_abr_phry);
    RUN(test_parity_shc);
    (void)printf("(%d golden lines matched) ", compared);
    CHECK_EQ_INT(compared, 4);
    RUN(test_shc_round_trips_and_rejects_hostile_input);
    RUN(test_phry_flat_folders_like_photoshop_25);
    RUN(test_phry_nested_folders_and_top_level_presets);
    RUN(test_phry_mismatches_become_warnings);
    RUN(test_phry_builds_paths_only_for_the_files_presets);
    RUN(test_phry_folder_paths_stay_within_their_byte_budget);
    RUN(test_phry_round_trips_through_the_writer);
    pc_buf_fini(&golden);
    return pc_t_finish();
}
