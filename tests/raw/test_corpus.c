/* Real camera raws from corpus/pixls (raw.pixls.us, public domain; `tools/corpus.sh fetch pixls`):
 * the oracles of crates/raw/tests/raw_corpus.rs, and parity with tests/golden/raw-pixls-vectors.txt
 * (`rustref raw-pixls-vectors corpus/pixls`: decode, previews, colour space and a developed
 * 512 x 512 centre crop). Skips cleanly without the corpus. Developing every file at full size (the
 * reference's second test; a 10480 x 7794 RW2 among them) runs with PHOTOCRAFT_SLOW_TESTS=1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raw/raw.h"
#include "rawhash.h"

#define DIR "corpus/pixls"

typedef struct want {
    const char *file;
    pc_raw_format format;
    const char *make;
    const char *model;
    size_t w, h;
    uint8_t cfa[4];
    float black[4];
    size_t nblack;
    float white;
} want;

static const want WANTS[6] = {
    {"IMG_4059.CR2",
     PC_RAW_CR2,
     "Canon",
     "Canon PowerShot SX50 HS",
     4176,
     3062,
     {0, 1, 1, 2},
     {128, 128, 128, 128},
     4,
     4095},
    {"CRW_4061.DNG",
     PC_RAW_DNG,
     "Canon",
     "Canon PowerShot SX50 HS",
     4176,
     3062,
     {0, 1, 1, 2},
     {127},
     1,
     4095},
    {"JD1_8203.NEF", PC_RAW_NEF, "NIKON CORPORATION", "NIKON D3", 4288, 2844, {0, 1, 1, 2}, {0}, 1, 4095},
    {"DSC00009.ARW", PC_RAW_ARW, "SONY", "DSC-RX0", 4832, 3224, {0, 1, 1, 2}, {512}, 1, 16301},
    {"P1000475.RW2",
     PC_RAW_RW2,
     "Panasonic",
     "DC-G9",
     10480,
     7794,
     {0, 1, 1, 2},
     {127, 128, 128, 127},
     4,
     4095},
    {"E_1__C106743_gredos.ORF",
     PC_RAW_ORF,
     "OLYMPUS CORPORATION",
     "E-1",
     2624,
     1966,
     {1, 0, 2, 1},
     {65, 65, 65, 65},
     4,
     4095},
};

static uint8_t *slurp(const char *name, size_t *n)
{
    char path[256];
    FILE *f;
    uint8_t *b = NULL;
    long len;
    (void)snprintf(path, sizeof path, DIR "/%s", name);
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
        (b = malloc((size_t)len)) != NULL && fread(b, 1, (size_t)len, f) != (size_t)len) {
        free(b);
        b = NULL;
    }
    (void)fclose(f);
    *n = b != NULL ? (size_t)len : 0;
    return b;
}

static bool have_corpus(void)
{
    FILE *f = fopen(DIR "/IMG_4059.CR2", "rb");
    if (f == NULL) {
        return false;
    }
    (void)fclose(f);
    return true;
}

static void test_decode_matches_the_measured_oracles(void)
{
    size_t i;
    for (i = 0; i < 6; i++) {
        const want *w = &WANTS[i];
        size_t n;
        uint8_t *b = slurp(w->file, &n);
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_sensor s;
        pc_error e;
        size_t k;
        if (b == NULL || pc_raw_decode(b, n, &lim, &s, &e) != PC_OK) {
            PC_T_FAIL("%s does not decode", w->file);
            free(b);
            continue;
        }
        CHECK_EQ_INT(s.format, w->format);
        CHECK_EQ_STR(s.make, w->make);
        CHECK_EQ_STR(s.model, w->model);
        CHECK_EQ_UINT(s.width, w->w);
        CHECK_EQ_UINT(s.height, w->h);
        CHECK(s.has_cfa && s.cfa.width == 2 && s.cfa.height == 2 && memcmp(s.cfa.colors, w->cfa, 4) == 0);
        CHECK_EQ_UINT(s.black.nvalues, w->nblack);
        for (k = 0; k < w->nblack && k < s.black.nvalues; k++) {
            CHECK(s.black.values[k] == w->black[k]);
        }
        for (k = 0; k < 3; k++) {
            CHECK_NEAR(s.white[k], w->white, 0.5);
        }
        pc_raw_sensor_fini(&s);
        free(b);
    }
}

static void test_developing_produces_the_crop_in_16bit_rgb(void)
{
    size_t i;
    const char *slow = getenv("PHOTOCRAFT_SLOW_TESTS");
    if (slow == NULL || strcmp(slow, "1") != 0) {
        printf("  full-size development skipped (PHOTOCRAFT_SLOW_TESTS=1 runs it)\n");
        return;
    }
    for (i = 0; i < 6; i++) {
        const want *w = &WANTS[i];
        size_t n;
        uint8_t *b = slurp(w->file, &n);
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_sensor s;
        pc_raw_developed d;
        pc_raw_developed d2;
        pc_error e;
        uint16_t lo = 65535;
        uint16_t hi = 0;
        bool estimated = strcmp(w->file, "IMG_4059.CR2") == 0 || strcmp(w->file, "DSC00009.ARW") == 0;
        size_t k;
        if (b == NULL || pc_raw_decode(b, n, &lim, &s, &e) != PC_OK) {
            PC_T_FAIL("%s does not decode", w->file);
            free(b);
            continue;
        }
        free(b);
        if (pc_raw_develop_sensor(&s, &o, &d, &e) != PC_OK) {
            PC_T_FAIL("%s: %s", w->file, e.msg);
            pc_raw_sensor_fini(&s);
            continue;
        }
        CHECK_EQ_UINT(d.width, s.crop.width);
        CHECK_EQ_UINT(d.height, s.crop.height);
        CHECK_EQ_UINT(d.nrgb, (size_t)d.width * d.height * 3);
        for (k = 0; k < d.nrgb; k++) {
            lo = d.rgb[k] < lo ? d.rgb[k] : lo;
            hi = d.rgb[k] > hi ? d.rgb[k] : hi;
        }
        CHECK(lo < (estimated ? 16384 : 4096));
        CHECK(hi > 65535 / 4);
        if (pc_raw_develop_sensor(&s, &o, &d2, &e) == PC_OK) {
            CHECK(d2.nrgb == d.nrgb && memcmp(d.rgb, d2.rgb, d.nrgb * 2) == 0);
            pc_raw_developed_fini(&d2);
        }
        pc_raw_developed_fini(&d);
        pc_raw_sensor_fini(&s);
    }
}

static void test_packed_orf_reports_the_documented_gap(void)
{
    size_t n;
    uint8_t *b = slurp("_7061961_copy.ORF", &n);
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_raw_preview p;
    pc_error e;
    if (b == NULL) {
        PC_T_FAIL("_7061961_copy.ORF is missing");
        return;
    }
    CHECK(pc_raw_decode(b, n, &lim, &s, &e) == PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "Olympus compressed");
    CHECK(pc_raw_embedded_preview(b, n, &p));
    free(b);
}

/* Parity with tests/golden/raw-pixls-vectors.txt. */
static void test_reference_parity(void)
{
    FILE *f = fopen("tests/golden/raw-pixls-vectors.txt", "r");
    char expected_[1024];
    char got[2048];
    char cur[128] = "";
    uint8_t *b = NULL;
    size_t n = 0;
    pc_raw_sensor s;
    bool have_s = false;
    size_t lines = 0;
    if (f == NULL) {
        PC_T_FAIL("cannot open tests/golden/raw-pixls-vectors.txt");
        return;
    }
    while (fgets(expected_, sizeof expected_, f) != NULL) {
        char kind[32];
        char name[128];
        char method[32];
        size_t l = strlen(expected_);
        while (l > 0 && (expected_[l - 1] == '\n' || expected_[l - 1] == '\r')) {
            expected_[--l] = '\0';
        }
        if (sscanf(expected_, "%31s %127s", kind, name) != 2) {
            continue;
        }
        if (strcmp(name, cur) != 0) {
            free(b);
            if (have_s) {
                pc_raw_sensor_fini(&s);
                have_s = false;
            }
            (void)snprintf(cur, sizeof cur, "%s", name);
            b = slurp(name, &n);
            if (b == NULL) {
                PC_T_FAIL("%s is missing", name);
                break;
            }
        }
        lines++;
        if (strcmp(kind, "pfile") == 0) {
            uint64_t h = H0;
            fnv(&h, b, n);
            (void)snprintf(got, sizeof got, "pfile %s %zu %016llx", name, n, (unsigned long long)h);
        } else if (strcmp(kind, "ppreviews") == 0) {
            pc_raw_preview p[PC_RAW_MAX_PREVIEWS];
            size_t np = pc_raw_embedded_previews(b, n, p, PC_RAW_MAX_PREVIEWS);
            size_t o = (size_t)snprintf(got, sizeof got, "ppreviews %s %zu", name, np);
            size_t k;
            for (k = 0; k < np && o < sizeof got; k++) {
                o += (size_t)snprintf(got + o, sizeof got - o, " %ux%u:%zu", (unsigned)p[k].width,
                                      (unsigned)p[k].height, p[k].len);
            }
        } else if (strcmp(kind, "pcspace") == 0) {
            pc_raw_preview_color_space c;
            (void)snprintf(got, sizeof got, "pcspace %s %s", name,
                           pc_raw_embedded_preview_color_space(b, n, &c)
                               ? (c == PC_RAW_PREVIEW_SRGB ? "Srgb" : "AdobeRgb")
                               : "none");
        } else if (strcmp(kind, "pdecode") == 0) {
            pc_raw_limits lim = pc_raw_limits_default();
            pc_error e;
            if (pc_raw_decode(b, n, &lim, &s, &e) == PC_OK) {
                uint64_t h = H0;
                pc_raw_rect c = s.crop;
                size_t cw = (c.width < 512 ? c.width : 512) & ~(size_t)1;
                size_t ch = (c.height < 512 ? c.height : 512) & ~(size_t)1;
                have_s = true;
                u64le(&h, sensor_hash(&s));
                f32b(&h, s.black_point);
                (void)snprintf(got, sizeof got, "pdecode %s %016llx", name, (unsigned long long)h);
                s.crop = pc_raw_rect_make(c.x + ((c.width - cw) / 2 & ~(size_t)1),
                                          c.y + ((c.height - ch) / 2 & ~(size_t)1), cw, ch);
            } else {
                (void)snprintf(got, sizeof got, "pdecode %s err %s", name, e.msg);
            }
        } else if (strcmp(kind, "pdevelop") == 0 && have_s &&
                   sscanf(expected_, "%*s %*s %31s", method) == 1) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_developed d;
            pc_error e;
            o.demosaic = strcmp(method, "bilinear") == 0 ? PC_RAW_BILINEAR : PC_RAW_AHD;
            if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                (void)snprintf(got, sizeof got, "pdevelop %s %s %016llx", name, method,
                               (unsigned long long)developed_hash(&d));
                pc_raw_developed_fini(&d);
            } else {
                (void)snprintf(got, sizeof got, "pdevelop %s %s err %s", name, method, e.msg);
            }
        } else {
            (void)snprintf(got, sizeof got, "(no line for) %s", expected_);
        }
        CHECK(strcmp(expected_, got) == 0);
        if (strcmp(expected_, got) != 0) {
            PC_T_FAIL("\n  expected_ %s\n  got  %s", expected_, got);
        }
    }
    (void)fclose(f);
    free(b);
    if (have_s) {
        pc_raw_sensor_fini(&s);
    }
    CHECK(lines > 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!have_corpus()) {
        printf("corpus/pixls not found (tools/corpus.sh fetch pixls); skipped\n");
        return 0;
    }
    RUN(test_decode_matches_the_measured_oracles);
    RUN(test_developing_produces_the_crop_in_16bit_rgb);
    RUN(test_packed_orf_reports_the_documented_gap);
    RUN(test_reference_parity);
    return pc_t_finish();
}
