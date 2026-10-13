/* Parity with what crates/text gained after L13-L15 (resync R8): the `# r8` section of
 * tests/golden/text-vectors.txt (tools/rustref/src/text_r8_vectors.rs): face names, named faces,
 * charmap_for_style, localized family names, the Southeast Asian fallback lists, scripts.rs and
 * missing glyph protection. Lines that need a font file this machine lacks are skipped. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "harness.h"
#include "text/scripts.h"
#include "text/text.h"

#define GOLDEN "tests/golden/text-vectors.txt"
#define H0 0xcbf29ce484222325ull

static int skipped;

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *p = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= p[i];
        *h *= 0x100000001b3ull;
    }
}

static void hu32(uint64_t *h, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fnv(h, b, 4);
}

static void hu64(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}

static void hf32(uint64_t *h, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    hu32(h, u);
}

static void hu8(uint64_t *h, uint8_t v)
{
    fnv(h, &v, 1);
}

static uint32_t f32bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static void esc(pc_buf *o, const char *s, size_t n)
{
    size_t i;
    if (n == 0) {
        pc_buf_append_str(o, "\\-", NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b <= ' ' || b == '\\' || b == '|' || b >= 0x7f) {
            pc_buf_appendf(o, NULL, "\\x%02x", b);
        } else {
            pc_buf_append_byte(o, b, NULL);
        }
    }
}

static void escz(pc_buf *o, const char *s)
{
    esc(o, s, strlen(s));
}

static char *unesc(const char *t)
{
    size_t n = strlen(t), i, m = 0;
    char *o = malloc(n + 1);
    if (o == NULL) {
        return NULL;
    }
    if (strcmp(t, "\\-") == 0) {
        o[0] = '\0';
        return o;
    }
    for (i = 0; i < n; i++) {
        if (t[i] == '\\' && i + 3 < n + 1 && t[i + 1] == 'x') {
            char hx[3] = {t[i + 2], t[i + 3], 0};
            o[m++] = (char)strtoul(hx, NULL, 16);
            i += 3;
        } else {
            o[m++] = t[i];
        }
    }
    o[m] = '\0';
    return o;
}

/* ---- font files by spec (as text_vectors.rs font_files) ---- */

static pc_blob *bundled_blob(const char *name)
{
    size_t n, i;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    for (i = 0; i < n; i++) {
        if (strcmp(b[i].file, name) == 0) {
            pc_blob *o = NULL;
            pc_blob_new(b[i].data, b[i].len, &o, NULL);
            return o;
        }
    }
    return NULL;
}

static pc_blob *make_ttc(const pc_blob *const *fonts, size_t n)
{
    size_t head = 12 + 4 * n, at = (head + 3) & ~(size_t)3, offs[8], i, total;
    uint8_t *out;
    pc_blob *b = NULL;
    for (i = 0; i < n; i++) {
        offs[i] = at;
        at += (pc_blob_len(fonts[i]) + 3) & ~(size_t)3;
    }
    total = at;
    out = calloc(1, total);
    memcpy(out, "ttcf", 4);
    out[5] = 1;
    out[11] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        const uint8_t *src = pc_blob_data(fonts[i]);
        size_t len = pc_blob_len(fonts[i]), k, nt;
        uint8_t *dst = out + offs[i];
        out[12 + 4 * i] = (uint8_t)(offs[i] >> 24);
        out[13 + 4 * i] = (uint8_t)(offs[i] >> 16);
        out[14 + 4 * i] = (uint8_t)(offs[i] >> 8);
        out[15 + 4 * i] = (uint8_t)offs[i];
        memcpy(dst, src, len);
        nt = (size_t)((dst[4] << 8) | dst[5]);
        for (k = 0; k < nt; k++) {
            uint8_t *p = dst + 12 + 16 * k + 8;
            uint32_t o = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) +
                         (uint32_t)offs[i];
            p[0] = (uint8_t)(o >> 24);
            p[1] = (uint8_t)(o >> 16);
            p[2] = (uint8_t)(o >> 8);
            p[3] = (uint8_t)o;
        }
    }
    pc_blob_new(out, total, &b, NULL);
    free(out);
    return b;
}

static pc_blob *spec_blob(const char *spec)
{
    static const char *const BAD[4] = {"", "ttcf", "ttcf\0\x01\0\0\0\0\0\x02\xff\xff\xff\xff\0\0\0\x10",
                                       "\0\x01\0\0\0\xff"};
    static const size_t BADLEN[4] = {0, 4, 20, 6};
    pc_blob *o = NULL;
    if (strncmp(spec, "bundled:", 8) == 0) {
        return bundled_blob(spec + 8);
    }
    if (strcmp(spec, "ttc:inter+mono") == 0) {
        pc_blob *a = bundled_blob("Inter-Regular.ttf"), *b = bundled_blob("JetBrainsMono-Regular.ttf"), *t;
        const pc_blob *v[2] = {a, b};
        t = make_ttc(v, 2);
        pc_blob_release(a);
        pc_blob_release(b);
        return t;
    }
    if (strncmp(spec, "bad", 3) == 0 && spec[3] >= '0' && spec[3] <= '3' && spec[4] == '\0') {
        pc_blob_new((const uint8_t *)BAD[spec[3] - '0'], BADLEN[spec[3] - '0'], &o, NULL);
        return o;
    }
    {
        pc_buf data = PC_BUF_INIT;
        if (pc_file_read(spec, (size_t)1 << 28, &data, NULL) == PC_OK) {
            pc_blob_new(data.data, data.len, &o, NULL);
        }
        pc_buf_fini(&data);
        return o;
    }
}

/* ---- the layout hash (text_vectors.rs hash_layout, aliases without their id) ---- */

static void hash_layout(const pc_text_layout *l, uint64_t h[6])
{
    size_t i, k;
    for (i = 0; i < 6; i++) {
        h[i] = H0;
    }
    for (i = 0; i < l->nfaces; i++) {
        const pc_glyph_face *f = &l->faces[i];
        hu64(&h[0], pc_blob_len(pc_font_blob(f->font)));
        hu32(&h[0], pc_font_index(f->font));
        hu32(&h[0], (uint32_t)f->ncoords);
        for (k = 0; k < f->ncoords; k++) {
            uint8_t b[2] = {(uint8_t)f->coords[k], (uint8_t)((uint16_t)f->coords[k] >> 8)};
            fnv(&h[0], b, 2);
        }
        hf32(&h[0], f->size_px);
        hu8(&h[0], f->embolden ? 1 : 0);
        hf32(&h[0], f->skew_deg);
    }
    for (i = 0; i < l->nglyphs; i++) {
        const pc_placed_glyph *g = &l->glyphs[i];
        hu32(&h[1], g->face);
        hu32(&h[1], g->id);
        hf32(&h[1], g->x);
        hf32(&h[1], g->y);
        hu32(&h[1], g->style);
        hu8(&h[1], (uint8_t)g->orient);
    }
    for (i = 0; i < l->ndecorations; i++) {
        const pc_decoration_rect *d = &l->decorations[i];
        hf32(&h[2], d->x0);
        hf32(&h[2], d->y0);
        hf32(&h[2], d->x1);
        hf32(&h[2], d->y1);
        hu32(&h[2], d->style);
    }
    for (i = 0; i < l->nlines; i++) {
        const pc_line_info *ln = &l->lines[i];
        hu64(&h[3], ln->start);
        hu64(&h[3], ln->end);
        hf32(&h[3], ln->baseline);
        hf32(&h[3], ln->x0);
        hf32(&h[3], ln->x1);
        hf32(&h[3], ln->ascent);
        hf32(&h[3], ln->descent);
        hu64(&h[3], ln->paragraph);
    }
    for (i = 0; i < l->nclusters; i++) {
        const pc_cluster_info *c = &l->clusters[i];
        hu64(&h[4], c->start);
        hu64(&h[4], c->end);
        hf32(&h[4], c->x);
        hf32(&h[4], c->advance);
        hu64(&h[4], c->line);
        hu8(&h[4], c->rtl ? 1 : 0);
    }
    for (i = 0; i < l->nstyles; i++) {
        const char *f = pc_str_c(&l->styles[i].font_family), *dash;
        size_t fl = l->styles[i].font_family.len;
        if (fl > 17 && memcmp(f, ".PhotoCraft-face-", 17) == 0 &&
            (dash = memchr(f + 17, '-', fl - 17)) != NULL) {
            fnv(&h[5], ".PhotoCraft-face-", 17);
            fnv(&h[5], dash + 1, fl - (size_t)(dash + 1 - f));
        } else {
            fnv(&h[5], f, fl);
        }
        hu32(&h[5], l->styles[i].weight);
        hu8(&h[5], l->styles[i].italic ? 1 : 0);
    }
}

/* ---- line kinds ---- */

static void names_of(pc_fontdb *db, const char *fam, pc_buf *o)
{
    pc_face_info *f = NULL;
    size_t n = 0, i;
    CHECK(pc_fontdb_faces(db, fam, strlen(fam), &f, &n, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        pc_buf_append_byte(o, ' ', NULL);
        escz(o, f[i].style);
        pc_buf_append_byte(o, '|', NULL);
        if (f[i].postscript_name != NULL) {
            escz(o, f[i].postscript_name);
        } else {
            pc_buf_append_byte(o, '-', NULL);
        }
        pc_buf_appendf(o, NULL, "|%08x|%d", f32bits(f[i].weight), f[i].italic ? 1 : 0);
    }
    pc_face_infos_free(f, n);
}

static const char *const *families_list(const char *name)
{
    return strcmp(name, "thai") == 0    ? PC_TEXT_THAI_FAMILIES
           : strcmp(name, "khmer") == 0 ? PC_TEXT_KHMER_FAMILIES
           : strcmp(name, "lao") == 0   ? PC_TEXT_LAO_FAMILIES
                                        : PC_TEXT_MYANMAR_FAMILIES;
}

static const char *const SCRIPT_NAMES[PC_SCRIPT_COUNT] = {
    "Thai",     "Lao",      "Khmer",    "Myanmar",    "Tibetan",   "Arabic",   "Hebrew",
    "Armenian", "Georgian", "Ethiopic", "Devanagari", "Bengali",   "Gurmukhi", "Gujarati",
    "Oriya",    "Tamil",    "Telugu",   "Kannada",    "Malayalam", "Sinhala"};

static const char *const SCRIPT_FILES[] = {
    "/usr/share/fonts/google-noto/NotoSansThai-Bold.ttf",
    "/usr/share/fonts/google-noto-vf/NotoSansThai[wght].ttf",
    "/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansThaiLooped-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansThaana-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/lohit-devanagari/Lohit-Devanagari.ttf",
    "/usr/share/fonts/Lohit-Tamil.ttf",
    "/mnt/c/Windows/Fonts/LeelawUI.ttf",
    "/mnt/c/Windows/Fonts/Nirmala.ttc",
    "/System/Library/Fonts/Supplemental/Thonburi.ttc",
    "/System/Library/Fonts/Supplemental/Kohinoor.ttc",
    "/usr/share/fonts/README.txt",
    "/a/b/Noto_Sans Arabic-Italic.OTF",
    "/a/b/notosansarabic-Condensed.ttf",
    "/a/notosansarabic.ttf",
    "/a-b/NotoSansArabic.ttf",
    "/a/b/NotoSansArabicUI[wdth,wght].ttf",
    "/x/Tahoma.TTF",
    "/x/.ttf",
    "/x/arial-Black.ttf",
    "/x/arial.ttf.bak",
    "/x/SegoeUI-Light.ttf",
    "/x/segoeui.ttf",
    "/x/Kalinga-Bold.ttf",
    "/x/Lohit-Odia.ttf",
    "/x/lohit-odia.ttf",
    "/x/Padauk.ttf",
    "/x/Padauk-Regular.ttf",
    "/x/padauk[wght].ttf",
    "rel/Amiri-Regular.ttf",
    "rel//./Amiri-Regular.ttf",
    "rel/Amiri-Regular.ttf",
};

static void lay_line(pc_text_engine *e, pc_buf *o)
{
    pc_text_layer t;
    pc_text_layout l;
    uint64_t h[6];
    pc_text_layer_default(&t);
    CHECK(pc_str_set_c(&t.text, "Latin \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D",
                       NULL) == PC_OK);
    CHECK(pc_str_set_c(&t.font_family, "Inter", NULL) == PC_OK);
    t.size_pt = 20.0f;
    if (pc_text_engine_layout(e, &t, 72.0f, &l, NULL) == PC_OK) {
        hash_layout(&l, h);
        pc_buf_appendf(o, NULL, "%zu %zu %zu %016llx %016llx %016llx %016llx %016llx %016llx", l.nfaces,
                       l.nglyphs, l.nlines, (unsigned long long)h[0], (unsigned long long)h[1],
                       (unsigned long long)h[2], (unsigned long long)h[3], (unsigned long long)h[4],
                       (unsigned long long)h[5]);
        pc_text_layout_fini(&l);
    }
    pc_text_layer_fini(&t);
}

static void test_golden(void)
{
    pc_buf g = PC_BUF_INIT;
    char *p, *nl;
    bool in = false;
    int lines = 0;
    pc_fontdb *db0 = NULL;
    pc_text_engine *mgp = NULL;
    bool mgp_ok = true;
    if (pc_file_read(GOLDEN, (size_t)1 << 26, &g, NULL) != PC_OK) {
        PC_T_FAIL("cannot read %s", GOLDEN);
        return;
    }
    CHECK(pc_buf_append_byte(&g, 0, NULL) == PC_OK);
    CHECK(pc_fontdb_new(&db0, NULL) == PC_OK);
    for (p = (char *)g.data; p != NULL && *p != '\0'; p = nl != NULL ? nl + 1 : NULL) {
        char kind[32], *args, *want;
        pc_buf got = PC_BUF_INIT;
        bool skip = false;
        nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        if (p[0] == '#') {
            in = strcmp(p, "# r8") == 0;
            continue;
        }
        if (!in || sscanf(p, "%31s", kind) != 1) {
            continue;
        }
        args = p + strlen(kind) + 1;
        want = strstr(p, " => ");
        want = want != NULL ? want + 4 : (char *)"";
        lines++;
        if (strcmp(kind, "r8families") == 0) {
            char name[16];
            const char *const *l;
            sscanf(args, "%15s", name);
            for (l = families_list(name); *l != NULL; l++) {
                if (l != families_list(name)) {
                    pc_buf_append_byte(&got, ' ', NULL);
                }
                escz(&got, *l);
            }
        } else if (strcmp(kind, "r8names") == 0) {
            char tag[512], famt[512], *fam;
            sscanf(args, "%511s %511s", tag, famt);
            fam = unesc(famt);
            if (strcmp(tag, "bundled") == 0) {
                names_of(db0, fam, &got);
            } else {
                pc_blob *b = spec_blob(tag);
                pc_fontdb *d = NULL;
                if (b == NULL) {
                    skip = true;
                } else {
                    CHECK(pc_fontdb_new(&d, NULL) == PC_OK);
                    CHECK(pc_fontdb_register_font_data(d, pc_blob_data(b), pc_blob_len(b), NULL, NULL) ==
                          PC_OK);
                    names_of(d, fam, &got);
                    pc_fontdb_free(d);
                    pc_blob_release(b);
                }
            }
            free(fam);
        } else if (strcmp(kind, "r8named") == 0) {
            char ft[256], st[256], *fam, *sty;
            pc_face_info *f = NULL;
            bool found = false;
            sscanf(args, "%255s %255s", ft, st);
            fam = unesc(ft);
            sty = unesc(st);
            CHECK(pc_fontdb_named_face(db0, fam, sty, &f, &found, NULL) == PC_OK);
            if (found) {
                escz(&got, f->family);
                pc_buf_appendf(&got, NULL, " %08x %d ", f32bits(f->weight), f->italic ? 1 : 0);
                if (f->postscript_name != NULL) {
                    escz(&got, f->postscript_name);
                } else {
                    pc_buf_append_byte(&got, '-', NULL);
                }
                pc_face_infos_free(f, 1);
            } else {
                pc_buf_append_str(&got, "none", NULL);
            }
            free(fam);
            free(sty);
        } else if (strcmp(kind, "r8cmstyle") == 0) {
            int i = atoi(args);
            pc_char_style s;
            pc_fontdb *d = NULL;
            uint32_t *cm = NULL;
            size_t n = 0, k;
            uint64_t h = H0;
            pc_char_style_default(&s);
            CHECK(pc_fontdb_new(&d, NULL) == PC_OK);
            pc_str_set_c(&s.font_family, i == 3 ? "JetBrains Mono" : i == 4 ? "No Such" : "Inter", NULL);
            if (i == 0) {
                pc_str_set_c(&s.font_style, "SemiBold", NULL);
                s.weight = 600;
            } else if (i == 1) {
                s.has_postscript_name = true;
                pc_str_set_c(&s.postscript_name, "Inter-Medium", NULL);
            } else if (i == 2) {
                pc_str_set_c(&s.font_style, "Regular", NULL);
                s.weight = 700;
            }
            CHECK(pc_fontdb_charmap_for_style(d, &s, &cm, &n, NULL) == PC_OK);
            for (k = 0; k < n; k++) {
                hu32(&h, cm[k]);
            }
            pc_buf_appendf(&got, NULL, "%zu %016llx", n, (unsigned long long)h);
            free(cm);
            pc_char_style_fini(&s);
            pc_fontdb_free(d);
        } else if (strcmp(kind, "r8locname") == 0) {
            static const char *const N[8][4] = {
                {"MS Gothic", "\xEF\xBC\xAD\xEF\xBC\xB3 \xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF",
                 NULL, NULL},
                {"Microsoft YaHei", "\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91",
                 "\xE5\xBE\xAE\xE8\xBB\x9F\xE9\x9B\x85\xE9\xBB\x91", NULL},
                {"Arial", NULL, NULL, NULL},
                {"Arial", "Arial", NULL, NULL},
                {"Y", "  ", NULL, NULL},
                {NULL, NULL, NULL, NULL},
                {"Y", "Z", "W", "V"},
                {"\xE3\x80\x80\t", "X", "Q", NULL},
            };
            static const char *const L[8][4] = {
                {"en-US", "ja-JP", NULL, NULL}, {"en", "zh-CN", "zh-TW", NULL}, {"en-US", NULL, NULL, NULL},
                {"en-GB", "de-DE", NULL, NULL}, {NULL, "ja-JP", NULL, NULL},    {NULL, NULL, NULL, NULL},
                {"EN_gb", "", "fr", "ja"},      {"ja", "ja", "ko-KR", NULL},
            };
            static const char *const EN[8] = {"MS Gothic", "Microsoft YaHei", "Arial", "Arial", "X", "X", "X",
                                              "X"};
            static const size_t CNT[8] = {2, 3, 1, 2, 2, 0, 4, 3};
            int i = atoi(args);
            if (i >= 0 && i < 8) {
                const char *r = pc_text_localized_name(EN[i], N[i], L[i], CNT[i]);
                if (r != NULL) {
                    escz(&got, r);
                } else {
                    pc_buf_append_byte(&got, '-', NULL);
                }
            }
        } else if (strcmp(kind, "r8localized") == 0) {
            static const char *const FILES[] = {
                "/usr/share/fonts/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/dejavu/DejaVuSerif-BoldItalic.ttf",
                "/usr/share/fonts/liberation-fonts/LiberationSans-Regular.ttf",
                "/usr/share/fonts/corefonts/arial.ttf",
                "/usr/share/fonts/corefonts/georgia.ttf",
                "/usr/share/fonts/corefonts/times.ttf",
                "/usr/share/fonts/corefonts/verdana.ttf",
                "/usr/share/fonts/urw-fonts/NimbusSans-Regular.otf",
                "/usr/share/fonts/eb-garamond/EBGaramond12-Italic.otf",
                "/usr/share/fonts/noto-emoji/NotoEmoji-VariableFont_wght.ttf",
                "/usr/share/fonts/vlgothic/VL-Gothic-Regular.ttf",
            };
            pc_fontdb *d = NULL;
            pc_strlist out = {NULL, 0};
            size_t i, k, *ord;
            pc_blob *tt = spec_blob("ttc:inter+mono");
            CHECK(pc_fontdb_new(&d, NULL) == PC_OK);
            CHECK(pc_fontdb_register_font_data(d, pc_blob_data(tt), pc_blob_len(tt), NULL, NULL) == PC_OK);
            pc_blob_release(tt);
            for (i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
                pc_blob *b = spec_blob(FILES[i]);
                if (b != NULL) {
                    pc_fontdb_register_font_data(d, pc_blob_data(b), pc_blob_len(b), NULL, NULL);
                    pc_blob_release(b);
                }
            }
            CHECK(pc_fontdb_localized_family_names(d, &out, NULL) == PC_OK);
            ord = calloc(out.n / 2 + 1, sizeof *ord);
            for (i = 0; i < out.n / 2; i++) {
                ord[i] = i;
            }
            for (i = 1; i < out.n / 2; i++) { /* sort pairs by (english, localized) */
                for (k = i; k > 0 && strcmp(out.v[2 * ord[k - 1]], out.v[2 * ord[k]]) > 0; k--) {
                    size_t t = ord[k];
                    ord[k] = ord[k - 1];
                    ord[k - 1] = t;
                }
            }
            for (i = 0; i < out.n / 2; i++) {
                if (i > 0) {
                    pc_buf_append_byte(&got, ' ', NULL);
                }
                escz(&got, out.v[2 * ord[i]]);
                pc_buf_append_byte(&got, '=', NULL);
                escz(&got, out.v[2 * ord[i] + 1]);
            }
            free(ord);
            pc_strlist_fini(&out);
            pc_fontdb_free(d);
        } else if (strcmp(kind, "r8sclassify") == 0) {
            uint64_t h = H0;
            uint32_t c;
            for (c = 0; c < 0x20000; c++) {
                pc_text_script s;
                uint8_t k = 255;
                if (!(c >= 0xD800 && c <= 0xDFFF) && pc_text_script_classify(c, &s)) {
                    k = (uint8_t)s;
                }
                hu8(&h, k);
            }
            pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)h);
        } else if (strcmp(kind, "r8scand") == 0) {
            char name[32];
            int s;
            sscanf(args, "%31s", name);
            for (s = 0; s < PC_SCRIPT_COUNT; s++) {
                if (strcmp(SCRIPT_NAMES[s], name) == 0) {
                    pc_strlist out = {NULL, 0};
                    size_t k;
                    CHECK(pc_text_script_candidates((pc_text_script)s, SCRIPT_FILES,
                                                    sizeof SCRIPT_FILES / sizeof SCRIPT_FILES[0], &out,
                                                    NULL) == PC_OK);
                    for (k = 0; k < out.n; k++) {
                        if (k > 0) {
                            pc_buf_append_byte(&got, ' ', NULL);
                        }
                        escz(&got, out.v[k]);
                    }
                    pc_strlist_fini(&out);
                }
            }
        } else if (strcmp(kind, "r8covering") == 0) {
            static const uint32_t CPS[9] = {'A', 0x0E2A, 0x0631, 0x05E9, 0x0928, 0x65E5, 0xE9, 0, 0x1F600};
            char spec[512], *sp;
            pc_blob *b;
            sscanf(args, "%511s", spec);
            sp = unesc(spec);
            b = spec_blob(sp);
            if (b == NULL && strncmp(sp, "bad", 3) != 0) {
                skip = true;
            } else {
                int k;
                for (k = 0; k < 9; k++) {
                    uint32_t face;
                    if (b != NULL && pc_text_covering_face(pc_blob_data(b), pc_blob_len(b), CPS[k], &face)) {
                        pc_buf_appendf(&got, NULL, " %u", face);
                    } else {
                        pc_buf_append_str(&got, " -", NULL);
                    }
                }
            }
            pc_blob_release(b);
            free(sp);
        } else if (strcmp(kind, "r8mgp") == 0) {
            static const char *const SYS[2] = {"/usr/share/fonts/vlgothic/VL-Gothic-Regular.ttf",
                                               "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
            int k;
            CHECK(pc_text_engine_new(&mgp, NULL) == PC_OK);
            for (k = 0; k < 2; k++) {
                pc_blob *b = spec_blob(SYS[k]);
                if (b == NULL) {
                    mgp_ok = false;
                } else {
                    pc_fontdb_register_font_data(pc_text_engine_fonts(mgp), pc_blob_data(b), pc_blob_len(b),
                                                 NULL, NULL);
                    pc_blob_release(b);
                }
            }
            CHECK_EQ_STR(args, pc_fontdb_missing_glyph_protection(pc_text_engine_fonts(mgp)) ? "1" : "0");
            pc_buf_fini(&got);
            continue;
        } else if (strcmp(kind, "r8lay") == 0) {
            char tag[32];
            sscanf(args, "%31s", tag);
            if (mgp == NULL || !mgp_ok) {
                skip = true;
            } else {
                pc_fontdb_set_missing_glyph_protection(pc_text_engine_fonts(mgp),
                                                       strcmp(tag, "mgp_off") != 0);
                lay_line(mgp, &got);
            }
        } else {
            PC_T_FAIL("unknown r8 line kind %s", kind);
        }
        if (skip) {
            skipped++;
        } else {
            /* `=>{list}` lines: the list's leading space is the arrow's. */
            const char *gs = pc_buf_cstr(&got);
            if (gs[0] == ' ') {
                gs++;
            }
            pc_t_checks++;
            if (strcmp(gs, want) != 0) {
                PC_T_FAIL("%s: got \"%.300s\" want \"%.300s\"", p, gs, want);
            }
        }
        pc_buf_fini(&got);
    }
    (void)printf("%d r8 golden lines, %d skipped (fonts not installed)\n", lines, skipped);
    CHECK(lines > 50);
    pc_text_engine_free(mgp);
    pc_fontdb_free(db0);
    pc_buf_fini(&g);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_golden);
    return pc_t_finish();
}
