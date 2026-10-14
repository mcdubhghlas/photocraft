/* DPX (SMPTE 268M) read and write, against fixtures packed here independently of the codec:
 * every descriptor, bit depth and packing it reads, both byte orders, the header fields kept
 * through a round trip, the bit-depth choice on export and the files it refuses. Port of
 * crates/codecs/tests/dpx.rs, plus adversarial/truncated cases (never crash). */
#include "common.h"

enum { HEADER = 2048, ELEMENT0 = 780 };

static uint16_t be16(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---- a DPX file built field by field ---- */

typedef struct {
    size_t at;
    size_t len;
    const char *s;
} strfield;

typedef struct {
    bool big;
    uint32_t width;
    uint32_t height;
    uint8_t descriptor;
    uint8_t bits;
    uint16_t packing;
    uint16_t encoding;
    bool sign;
    uint8_t transfer;
    uint8_t colorimetric;
    uint16_t orientation;
    uint16_t elements;
    uint32_t frame_position;
    uint32_t frame_rate_bits;
    uint32_t time_code;
    const strfield *strings;
    size_t nstrings;
    uint16_t *codes; /* interleaved, top line first */
    size_t ncodes;
} fixture;

static int channels(uint8_t descriptor) { return descriptor == 6 ? 1 : descriptor == 50 ? 3 : 4; }

static fixture fx_new(uint8_t descriptor, uint8_t bits, uint16_t packing, uint32_t w, uint32_t h)
{
    fixture fx;
    uint32_t max = (1u << bits) - 1;
    size_t n = (size_t)w * h * (size_t)channels(descriptor);
    size_t i;
    memset(&fx, 0, sizeof fx);
    fx.big = true;
    fx.width = w;
    fx.height = h;
    fx.descriptor = descriptor;
    fx.bits = bits;
    fx.packing = packing;
    fx.encoding = 0;
    fx.transfer = 255;
    fx.colorimetric = 255;
    fx.orientation = 0;
    fx.elements = 1;
    fx.frame_position = 0xFFFFFFFFu;
    fx.frame_rate_bits = 0xFFFFFFFFu;
    fx.time_code = 0xFFFFFFFFu;
    fx.codes = malloc((n > 0 ? n : 1) * sizeof *fx.codes);
    fx.ncodes = n;
    for (i = 0; i < n; i++) {
        if (i == 0) {
            fx.codes[i] = 0;
        } else if (i == 1) {
            fx.codes[i] = (uint16_t)max;
        } else {
            fx.codes[i] = (uint16_t)(((uint64_t)i * 2654435761ull) % ((uint64_t)max + 1));
        }
    }
    return fx;
}

static void fx_free(fixture *fx) { free(fx->codes); }

static void put16(uint8_t *b, bool big, size_t at, uint16_t v)
{
    if (big) {
        b[at] = (uint8_t)(v >> 8);
        b[at + 1] = (uint8_t)v;
    } else {
        b[at] = (uint8_t)v;
        b[at + 1] = (uint8_t)(v >> 8);
    }
}

static void put32(uint8_t *b, bool big, size_t at, uint32_t v)
{
    if (big) {
        b[at] = (uint8_t)(v >> 24);
        b[at + 1] = (uint8_t)(v >> 16);
        b[at + 2] = (uint8_t)(v >> 8);
        b[at + 3] = (uint8_t)v;
    } else {
        b[at] = (uint8_t)v;
        b[at + 1] = (uint8_t)(v >> 8);
        b[at + 2] = (uint8_t)(v >> 16);
        b[at + 3] = (uint8_t)(v >> 24);
    }
}

/* Appends one line's code values, padded to a 32-bit word. */
static void fx_line(const fixture *fx, const uint16_t *codes, size_t n, pc_buf *out)
{
    size_t start = out->len;
    size_t written;
    size_t pad;
    size_t i;
    uint8_t word[4];
    if (fx->bits == 8) {
        for (i = 0; i < n; i++) {
            uint8_t c = (uint8_t)codes[i];
            (void)pc_buf_append(out, &c, 1, NULL);
        }
    } else if (fx->bits == 16) {
        for (i = 0; i < n; i++) {
            uint8_t b2[2];
            put16(b2, fx->big, 0, codes[i]);
            (void)pc_buf_append(out, b2, 2, NULL);
        }
    } else if (fx->bits == 10 && (fx->packing == 1 || fx->packing == 2)) {
        unsigned base = fx->packing == 1 ? 22u : 20u;
        for (i = 0; i < n; i += 3) {
            uint32_t w = 0;
            size_t j;
            for (j = 0; j < 3 && i + j < n; j++) {
                w |= (uint32_t)codes[i + j] << (base - 10u * (unsigned)j);
            }
            put32(word, fx->big, 0, w);
            (void)pc_buf_append(out, word, 4, NULL);
        }
    } else if (fx->bits == 12 && fx->packing == 1) {
        for (i = 0; i < n; i++) {
            uint8_t b2[2];
            put16(b2, fx->big, 0, (uint16_t)(codes[i] << 4));
            (void)pc_buf_append(out, b2, 2, NULL);
        }
    } else {
        /* One bit stream, high bits first, cut into 32-bit words (padded with zero bits). */
        uint64_t acc = 0;
        int have = 0;
        uint64_t mask = ((uint64_t)1 << fx->bits) - 1;
        for (i = 0; i < n; i++) {
            acc = (acc << fx->bits) | ((uint64_t)codes[i] & mask);
            have += fx->bits;
            while (have >= 32) {
                have -= 32;
                put32(word, fx->big, 0, (uint32_t)((acc >> have) & 0xFFFFFFFFu));
                (void)pc_buf_append(out, word, 4, NULL);
            }
        }
        if (have > 0) {
            put32(word, fx->big, 0, (uint32_t)((acc << (32 - have)) & 0xFFFFFFFFu));
            (void)pc_buf_append(out, word, 4, NULL);
        }
    }
    written = out->len - start;
    pad = (written % 4 != 0) ? 4 - written % 4 : 0;
    for (i = 0; i < pad; i++) {
        uint8_t z = 0;
        (void)pc_buf_append(out, &z, 1, NULL);
    }
}

static pc_buf fx_bytes(const fixture *fx)
{
    uint8_t h[HEADER];
    pc_buf out = PC_BUF_INIT;
    size_t per_line = (size_t)fx->width * (size_t)channels(fx->descriptor);
    size_t el;
    memset(h, 0, sizeof h);
    memcpy(h, fx->big ? "SDPX" : "XPDS", 4);
    put32(h, fx->big, 4, HEADER);
    memcpy(h + 8, "V2.0", 4);
    put32(h, fx->big, 24, 1664);
    put32(h, fx->big, 28, 384);
    put16(h, fx->big, 768, fx->orientation);
    put16(h, fx->big, 770, fx->elements);
    put32(h, fx->big, 772, fx->width);
    put32(h, fx->big, 776, fx->height);
    for (el = 0; el < (size_t)(fx->elements < 8 ? fx->elements : 8); el++) {
        size_t e = ELEMENT0 + el * 72;
        put32(h, fx->big, e, fx->sign ? 1u : 0u);
        h[e + 20] = fx->descriptor;
        h[e + 21] = fx->transfer;
        h[e + 22] = fx->colorimetric;
        h[e + 23] = fx->bits;
        put16(h, fx->big, e + 24, fx->packing);
        put16(h, fx->big, e + 26, fx->encoding);
        put32(h, fx->big, e + 28, HEADER);
    }
    for (el = 0; el < fx->nstrings; el++) {
        const strfield *s = &fx->strings[el];
        size_t sl = strlen(s->s);
        if (sl < s->len) {
            memcpy(h + s->at, s->s, sl);
        }
    }
    put32(h, fx->big, 1712, fx->frame_position);
    put32(h, fx->big, 1724, fx->frame_rate_bits);
    put32(h, fx->big, 1920, fx->time_code);
    (void)pc_buf_append(&out, h, HEADER, NULL);
    /* Lines are chunks of per_line code values (the last may be short, as Rust's chunks() does). */
    if (per_line > 0) {
        size_t off;
        for (off = 0; off < fx->ncodes; off += per_line) {
            size_t chunk = fx->ncodes - off < per_line ? fx->ncodes - off : per_line;
            fx_line(fx, fx->codes + off, chunk, &out);
        }
    }
    return out;
}

/* ---- helpers ---- */

static pc_clayout layout_of(uint8_t descriptor)
{
    return descriptor == 6 ? PC_CL_GRAY : descriptor == 50 ? PC_CL_RGB : PC_CL_RGBA;
}

/* The 16-bit sample a code value opens as: round(c * 65535 / (2^b - 1)). */
static uint16_t expected(uint16_t c, uint8_t bits)
{
    return (uint16_t)(((double)c * 65535.0 / (double)((1u << bits) - 1)) + 0.5);
}

static uint16_t img_u16(const pc_cimage *img, size_t i) { return ne16(img->data, i); }

static pc_cimage decode_fx(const fixture *fx)
{
    pc_buf b = fx_bytes(fx);
    pc_cimage img;
    pc_error e;
    pc_status st = pc_cdecode(b.data, b.len, &img, &e);
    if (st != PC_OK) {
        PC_T_FAIL("decode: %s", e.msg);
        pc_cimage_init(&img);
    }
    pc_buf_fini(&b);
    return img;
}

static bool fid_has(const pc_cimage *img, pc_cformat f, pc_cfidelity_kind kind)
{
    pc_cfidelity w[32];
    size_t n = pc_cfidelity_warnings(img, f, w, 32);
    size_t i;
    for (i = 0; i < n && i < 32; i++) {
        if (w[i].kind == kind) {
            return true;
        }
    }
    return false;
}

static bool has_text(const pc_cmeta *m, const char *k, const char *v)
{
    size_t i;
    for (i = 0; i < m->ntext; i++) {
        if (strcmp(m->text[i].key, k) == 0 && strcmp(m->text[i].value, v) == 0) {
            return true;
        }
    }
    return false;
}

/* Reads the code values of a big-endian file we wrote (packing 0 at 8/16, 1 at 10/12). */
static void written_codes(const uint8_t *b, uint8_t *bits_out, uint16_t *packing_out, uint16_t *codes, size_t cap,
                          size_t *ncodes)
{
    size_t w = be32(b + 772);
    size_t h = be32(b + 776);
    int nc = channels(b[ELEMENT0 + 20]);
    uint8_t bits = b[ELEMENT0 + 23];
    uint16_t packing = be16(b + ELEMENT0 + 24);
    size_t off = be32(b + ELEMENT0 + 28);
    size_t per_line = w * (size_t)nc;
    size_t stride = bits == 10 ? (per_line / 3 + (per_line % 3 != 0)) * 4
                               : (per_line * (bits == 8 ? 1 : 2) + 3) / 4 * 4;
    size_t y;
    size_t k = 0;
    for (y = 0; y < h; y++) {
        const uint8_t *line = b + off + y * stride;
        size_t i;
        for (i = 0; i < per_line && k < cap; i++) {
            uint16_t v;
            switch (bits) {
            case 8:
                v = line[i];
                break;
            case 10:
                v = (uint16_t)((be32(line + i / 3 * 4) >> (22 - 10 * (unsigned)(i % 3))) & 0x3FF);
                break;
            case 12:
                v = (uint16_t)(be16(line + i * 2) >> 4);
                break;
            default:
                v = be16(line + i * 2);
            }
            codes[k++] = v;
        }
    }
    *bits_out = bits;
    *packing_out = packing;
    *ncodes = k;
}

static pc_cencode_opts opts_bits(pc_dpx_bits bits)
{
    pc_cencode_opts o = pc_cencode_opts_default();
    o.dpx_bits = bits;
    return o;
}

static const struct {
    uint8_t bits;
    uint16_t packing;
} COMBOS[7] = {{8, 0}, {10, 0}, {10, 1}, {10, 2}, {12, 0}, {12, 1}, {16, 0}};

/* ---- tests ---- */

static void test_every_descriptor_depth_and_packing_opens(void)
{
    static const uint8_t DESC[3] = {6, 50, 51};
    size_t d;
    size_t c;
    for (d = 0; d < 3; d++) {
        for (c = 0; c < 7; c++) {
            fixture fx = fx_new(DESC[d], COMBOS[c].bits, COMBOS[c].packing, 5, 3);
            pc_buf b = fx_bytes(&fx);
            pc_cformat det;
            pc_cimage img = decode_fx(&fx);
            size_t i;
            char name[64];
            (void)snprintf(name, sizeof name, "descriptor %u, %u bits, packing %u", (unsigned)DESC[d],
                           (unsigned)COMBOS[c].bits, (unsigned)COMBOS[c].packing);
            pc_t_name = name;
            CHECK(pc_cformat_detect(b.data, b.len, &det) && det == PC_CF_DPX);
            CHECK(img.width == 5 && img.height == 3);
            CHECK(img.layout == layout_of(DESC[d]));
            CHECK(pc_clayout_has_alpha(img.layout) == (DESC[d] == 51));
            for (i = 0; i < fx.ncodes; i++) {
                uint16_t want = COMBOS[c].bits == 8    ? fx.codes[i]
                                : COMBOS[c].bits == 16 ? fx.codes[i]
                                                       : expected(fx.codes[i], COMBOS[c].bits);
                uint16_t got = COMBOS[c].bits == 8 ? img.data[i] : img_u16(&img, i);
                if (got != want) {
                    PC_T_FAIL("sample %zu: got %u want %u", i, (unsigned)got, (unsigned)want);
                    break;
                }
            }
            pc_t_checks++;
            pc_cimage_fini(&img);
            pc_buf_fini(&b);
            fx_free(&fx);
        }
    }
}

static void test_big_and_little_endian_open_identically(void)
{
    size_t c;
    for (c = 0; c < 7; c++) {
        fixture be = fx_new(50, COMBOS[c].bits, COMBOS[c].packing, 4, 2);
        fixture le = be;
        pc_buf bb = fx_bytes(&be);
        pc_buf lb;
        pc_cimage a;
        pc_cimage d;
        le.big = false;
        lb = fx_bytes(&le);
        CHECK(memcmp(lb.data, "XPDS", 4) == 0);
        a = decode_fx(&be);
        {
            pc_error e;
            if (pc_cdecode(lb.data, lb.len, &d, &e) != PC_OK) {
                PC_T_FAIL("le decode: %s", e.msg);
                pc_cimage_init(&d);
            }
        }
        CHECK(pc_cimage_eq(&a, &d));
        pc_cimage_fini(&a);
        pc_cimage_fini(&d);
        pc_buf_fini(&bb);
        pc_buf_fini(&lb);
        fx_free(&be);
    }
}

static void test_ten_bit_code_value_rounds(void)
{
    fixture fx = fx_new(6, 10, 1, 1, 1);
    pc_cimage img;
    fx.codes[0] = 512;
    img = decode_fx(&fx);
    CHECK(img_u16(&img, 0) == (uint16_t)(512.0 * 65535.0 / 1023.0 + 0.5));
    pc_cimage_fini(&img);
    fx_free(&fx);
}

static void test_ten_and_twelve_bit_written_back_at_depth(void)
{
    struct {
        uint8_t bits;
        pc_dpx_bits choice;
    } cases[2] = {{10, PC_DPX_BITS_10}, {12, PC_DPX_BITS_12}};
    size_t ci;
    uint16_t packing;
    for (ci = 0; ci < 2; ci++) {
        for (packing = 0; packing <= 1; packing++) {
            fixture fx = fx_new(50, cases[ci].bits, packing, 7, 3);
            pc_cimage img = decode_fx(&fx);
            pc_cencode_opts o = opts_bits(cases[ci].choice);
            pc_buf out = PC_BUF_INIT;
            uint8_t wbits;
            uint16_t wpacking;
            uint16_t codes[256];
            size_t nc;
            pc_error e;
            size_t i;
            if (pc_cencode(&img, PC_CF_DPX, &o, &out, &e) != PC_OK) {
                PC_T_FAIL("encode: %s", e.msg);
            }
            written_codes(out.data, &wbits, &wpacking, codes, 256, &nc);
            CHECK(wbits == cases[ci].bits && wpacking == 1);
            CHECK(nc == fx.ncodes);
            for (i = 0; i < fx.ncodes && i < nc; i++) {
                if (codes[i] != fx.codes[i]) {
                    PC_T_FAIL("%u bits packing %u: code %zu got %u want %u", (unsigned)cases[ci].bits,
                              (unsigned)packing, i, (unsigned)codes[i], (unsigned)fx.codes[i]);
                    break;
                }
            }
            pc_t_checks++;
            pc_cimage_fini(&img);
            pc_buf_fini(&out);
            fx_free(&fx);
        }
    }
}

static void test_sixteen_bit_round_trips_losslessly(void)
{
    fixture fx = fx_new(51, 16, 0, 6, 4);
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    uint8_t wbits;
    uint16_t wpacking;
    uint16_t codes[256];
    size_t nc;
    pc_error e;
    pc_cimage back;
    size_t i;
    fx.big = false;
    img = decode_fx(&fx);
    if (pc_cencode(&img, PC_CF_DPX, NULL, &out, &e) != PC_OK) {
        PC_T_FAIL("encode: %s", e.msg);
    }
    CHECK(memcmp(out.data, "SDPX", 4) == 0);
    written_codes(out.data, &wbits, &wpacking, codes, 256, &nc);
    CHECK(wbits == 16 && wpacking == 0 && nc == fx.ncodes);
    for (i = 0; i < fx.ncodes && i < nc; i++) {
        if (codes[i] != fx.codes[i]) {
            PC_T_FAIL("code %zu got %u want %u", i, (unsigned)codes[i], (unsigned)fx.codes[i]);
            break;
        }
    }
    pc_t_checks++;
    if (pc_cdecode(out.data, out.len, &back, &e) == PC_OK) {
        CHECK(back.len == img.len && memcmp(back.data, img.data, img.len) == 0);
        pc_cimage_fini(&back);
    } else {
        PC_T_FAIL("re-decode: %s", e.msg);
    }
    pc_cimage_fini(&img);
    pc_buf_fini(&out);
    fx_free(&fx);
}

static void test_log_frames_report_encoding(void)
{
    fixture fx = fx_new(50, 10, 1, 3, 2);
    pc_cimage img;
    char note[256];
    size_t i;
    fx.transfer = 3;
    fx.colorimetric = 1;
    img = decode_fx(&fx);
    CHECK(img.nwarnings == 1);
    if (img.nwarnings == 1) {
        pc_cwarning want = pc_cwarning_dpx(3, 1);
        CHECK(pc_cwarning_eq(&img.warnings[0], &want));
        pc_cwarning_text(&img.warnings[0], note, sizeof note);
        CHECK_CONTAINS(note, "logarithmic (3)");
        CHECK_CONTAINS(note, "printing density (1)");
    }
    for (i = 0; i < fx.ncodes; i++) {
        if (img_u16(&img, i) != expected(fx.codes[i], 10)) {
            PC_T_FAIL("sample %zu", i);
            break;
        }
    }
    pc_t_checks++;
    pc_cimage_fini(&img);
    fx_free(&fx);
}

static void test_only_first_element_imported(void)
{
    fixture fx = fx_new(6, 8, 0, 2, 2);
    pc_cimage img;
    char note[128];
    fx.elements = 2;
    img = decode_fx(&fx);
    CHECK(img.nwarnings == 1);
    if (img.nwarnings == 1) {
        pc_cwarning_text(&img.warnings[0], note, sizeof note);
        CHECK_EQ_STR(note, "only the first of 2 image elements was imported");
    }
    pc_cimage_fini(&img);
    fx_free(&fx);
}

static void test_listed_header_fields_survive_round_trip(void)
{
    static const strfield STR[4] = {{160, 100, "Resolve 20.1"},
                                    {260, 200, "Reel 7"},
                                    {460, 200, "(c) 2026 Studio"},
                                    {136, 24, "2026:10:12:09:30:00UTC"}};
    fixture fx = fx_new(50, 10, 1, 2, 2);
    pc_cimage img;
    pc_cencode_opts o = opts_bits(PC_DPX_BITS_10);
    pc_buf out = PC_BUF_INIT;
    pc_buf src;
    pc_error e;
    pc_cimage back;
    static const struct {
        size_t at;
        size_t len;
    } RANGES[8] = {{160, 100}, {260, 200}, {460, 200}, {136, 24}, {ELEMENT0 + 21, 2}, {1712, 4}, {1724, 4}, {1920, 4}};
    size_t r;
    float rate = 23.976f;
    fx.strings = STR;
    fx.nstrings = 4;
    fx.transfer = 3;
    fx.colorimetric = 1;
    fx.frame_position = 86400;
    memcpy(&fx.frame_rate_bits, &rate, 4);
    fx.time_code = 0x01020304u;
    src = fx_bytes(&fx);
    img = decode_fx(&fx);
    CHECK(has_text(&img.meta, "DPX:Creator", "Resolve 20.1"));
    CHECK(has_text(&img.meta, "DPX:Project", "Reel 7"));
    CHECK(has_text(&img.meta, "DPX:Copyright", "(c) 2026 Studio"));
    CHECK(has_text(&img.meta, "DPX:CreationTime", "2026:10:12:09:30:00UTC"));
    CHECK(has_text(&img.meta, "DPX:Transfer", "3"));
    CHECK(has_text(&img.meta, "DPX:Colorimetric", "1"));
    CHECK(has_text(&img.meta, "DPX:FramePosition", "86400"));
    CHECK(has_text(&img.meta, "DPX:FrameRate", "23.976"));
    CHECK(has_text(&img.meta, "DPX:TimeCode", "01:02:03:04"));
    if (pc_cencode(&img, PC_CF_DPX, &o, &out, &e) != PC_OK) {
        PC_T_FAIL("encode: %s", e.msg);
    }
    for (r = 0; r < 8; r++) {
        if (out.len < RANGES[r].at + RANGES[r].len ||
            memcmp(out.data + RANGES[r].at, src.data + RANGES[r].at, RANGES[r].len) != 0) {
            PC_T_FAIL("field at %zu differs", RANGES[r].at);
        }
        pc_t_checks++;
    }
    if (pc_cdecode(out.data, out.len, &back, &e) == PC_OK) {
        bool same = back.meta.ntext == img.meta.ntext;
        size_t i;
        for (i = 0; same && i < img.meta.ntext; i++) {
            same = strcmp(back.meta.text[i].key, img.meta.text[i].key) == 0 &&
                   strcmp(back.meta.text[i].value, img.meta.text[i].value) == 0;
        }
        CHECK(same);
        pc_cimage_fini(&back);
    } else {
        PC_T_FAIL("re-decode: %s", e.msg);
    }
    pc_cimage_fini(&img);
    pc_buf_fini(&out);
    pc_buf_fini(&src);
    fx_free(&fx);
}

static void test_chosen_bit_depth_sets_header_and_packing(void)
{
    uint16_t vals[30];
    pc_cimage img;
    size_t i;
    struct {
        pc_dpx_bits choice;
        uint8_t bits;
        uint16_t packing;
    } cases[4] = {{PC_DPX_BITS_8, 8, 0}, {PC_DPX_BITS_10, 10, 1}, {PC_DPX_BITS_12, 12, 1}, {PC_DPX_BITS_16, 16, 0}};
    size_t ci;
    uint8_t gray[3] = {0, 128, 255};
    pc_cimage gray8;
    pc_cimage flt;
    pc_error e;
    for (i = 0; i < 30; i++) {
        vals[i] = (uint16_t)(i * 2000);
    }
    if (pc_cimage_from_copy(5, 2, PC_CL_RGB, PC_CS_U16, vals, sizeof vals, &img, &e) != PC_OK) {
        PC_T_FAIL("from_copy");
        return;
    }
    for (ci = 0; ci < 4; ci++) {
        pc_cencode_opts o = opts_bits(cases[ci].choice);
        pc_buf out = PC_BUF_INIT;
        pc_cimage back;
        if (pc_cencode(&img, PC_CF_DPX, &o, &out, &e) != PC_OK) {
            PC_T_FAIL("encode: %s", e.msg);
        }
        CHECK(out.data[ELEMENT0 + 23] == cases[ci].bits);
        CHECK(be16(out.data + ELEMENT0 + 24) == cases[ci].packing);
        if (pc_cdecode(out.data, out.len, &back, &e) == PC_OK) {
            CHECK(back.width == 5 && back.height == 2 && back.layout == PC_CL_RGB);
            pc_cimage_fini(&back);
        } else {
            PC_T_FAIL("decode: %s", e.msg);
        }
        pc_buf_fini(&out);
    }
    /* Without a choice the depth follows the image. */
    if (pc_cimage_from_copy(3, 1, PC_CL_GRAY, PC_CS_U8, gray, sizeof gray, &gray8, &e) == PC_OK) {
        pc_buf out = PC_BUF_INIT;
        if (pc_cencode(&gray8, PC_CF_DPX, NULL, &out, &e) == PC_OK) {
            CHECK(out.data[ELEMENT0 + 23] == 8);
            pc_buf_fini(&out);
        } else {
            PC_T_FAIL("gray encode");
        }
        pc_cimage_fini(&gray8);
    }
    {
        pc_buf out = PC_BUF_INIT;
        if (pc_cencode(&img, PC_CF_DPX, NULL, &out, &e) == PC_OK) {
            CHECK(out.data[ELEMENT0 + 23] == 16);
            pc_buf_fini(&out);
        }
    }
    if (pc_cimage_convert(&img, PC_CL_RGB, PC_CS_F32, &flt, &e) == PC_OK) {
        pc_buf out = PC_BUF_INIT;
        if (pc_cencode(&flt, PC_CF_DPX, NULL, &out, &e) == PC_OK) {
            CHECK(out.data[ELEMENT0 + 23] == 16);
            pc_buf_fini(&out);
        }
        CHECK(fid_has(&flt, PC_CF_DPX, PC_FW_DEPTH_REDUCED));
        pc_cimage_fini(&flt);
    }
    pc_cimage_fini(&img);
}

static void test_gray_alpha_written_as_luma(void)
{
    uint8_t px[4] = {10, 128, 200, 255};
    pc_cimage img;
    pc_buf out = PC_BUF_INIT;
    pc_cimage back;
    pc_error e;
    if (pc_cimage_from_copy(2, 1, PC_CL_GRAYA, PC_CS_U8, px, sizeof px, &img, &e) != PC_OK) {
        PC_T_FAIL("from_copy");
        return;
    }
    if (pc_cencode(&img, PC_CF_DPX, NULL, &out, &e) != PC_OK) {
        PC_T_FAIL("encode: %s", e.msg);
    }
    CHECK(out.data[ELEMENT0 + 20] == 6);
    if (pc_cdecode(out.data, out.len, &back, &e) == PC_OK) {
        CHECK(back.layout == PC_CL_GRAY && back.len == 2 && back.data[0] == 10 && back.data[1] == 200);
        pc_cimage_fini(&back);
    } else {
        PC_T_FAIL("decode: %s", e.msg);
    }
    CHECK(fid_has(&img, PC_CF_DPX, PC_FW_ALPHA_DISCARDED));
    pc_cimage_fini(&img);
    pc_buf_fini(&out);
}

static void test_only_free_form_text_reported_dropped(void)
{
    uint8_t px[3] = {1, 2, 3};
    pc_cimage img;
    pc_error e;
    if (pc_cimage_from_copy(1, 1, PC_CL_RGB, PC_CS_U8, px, sizeof px, &img, &e) != PC_OK) {
        PC_T_FAIL("from_copy");
        return;
    }
    (void)pc_cmeta_add_text(&img.meta, "DPX:Project", 11, "Reel 7", 6, NULL);
    CHECK(!fid_has(&img, PC_CF_DPX, PC_FW_TEXT_DROPPED));
    (void)pc_cmeta_add_text(&img.meta, "Comment", 7, "from a PNG", 10, NULL);
    CHECK(fid_has(&img, PC_CF_DPX, PC_FW_TEXT_DROPPED));
    pc_cimage_fini(&img);
}

static void test_stored_orientation_turned_upright(void)
{
    fixture fx = fx_new(6, 8, 0, 3, 1);
    pc_cimage img;
    fx.codes[0] = 1;
    fx.codes[1] = 2;
    fx.codes[2] = 3;
    fx.orientation = 4;
    img = decode_fx(&fx);
    CHECK(img.width == 1 && img.height == 3);
    CHECK(img.len == 3 && img.data[0] == 1 && img.data[1] == 2 && img.data[2] == 3);
    pc_cimage_fini(&img);
    fx_free(&fx);
}

static bool decode_err(const uint8_t *b, size_t n)
{
    pc_cimage img;
    pc_status st = pc_cdecode_as(PC_CF_DPX, b, n, &img, NULL);
    if (st == PC_OK) {
        pc_cimage_fini(&img);
        return false;
    }
    return true;
}

static void test_unsupported_and_damaged_error(void)
{
    /* Unsupported element shapes. */
    {
        fixture fx = fx_new(50, 8, 0, 2, 2);
        pc_buf b;
        fx.descriptor = 100;
        b = fx_bytes(&fx);
        CHECK(decode_err(b.data, b.len)); /* ABGR descriptor */
        pc_buf_fini(&b);
        fx_free(&fx);
    }
    {
        fixture fx = fx_new(50, 10, 1, 2, 2);
        pc_buf b;
        fx.encoding = 1;
        b = fx_bytes(&fx);
        CHECK(decode_err(b.data, b.len)); /* run-length encoding */
        pc_buf_fini(&b);
        fx_free(&fx);
    }
    {
        fixture fx = fx_new(50, 16, 0, 2, 2);
        pc_buf b;
        fx.sign = true;
        b = fx_bytes(&fx);
        CHECK(decode_err(b.data, b.len)); /* signed samples */
        pc_buf_fini(&b);
        fx_free(&fx);
    }
    {
        fixture fx = fx_new(50, 16, 0, 2, 2);
        pc_buf b = fx_bytes(&fx);
        b.data[ELEMENT0 + 23] = 32;
        CHECK(decode_err(b.data, b.len)); /* 32-bit float */
        pc_buf_fini(&b);
        fx_free(&fx);
    }
    {
        fixture fx = fx_new(50, 12, 1, 2, 2);
        pc_buf b = fx_bytes(&fx);
        b.data[ELEMENT0 + 24] = 0;
        b.data[ELEMENT0 + 25] = 2; /* method B at 12 bits */
        CHECK(decode_err(b.data, b.len));
        pc_buf_fini(&b);
        fx_free(&fx);
    }
    /* Damaged / adversarial files: error, never crash. */
    {
        fixture fx = fx_new(51, 10, 1, 9, 9);
        pc_buf good = fx_bytes(&fx);
        pc_buf t;
        /* header cut short */
        CHECK(decode_err(good.data, 900));
        /* truncated element */
        CHECK(decode_err(good.data, good.len - 1));
        /* no image elements */
        pc_buf_init(&t);
        (void)pc_buf_append(&t, good.data, good.len, NULL);
        t.data[770] = 0;
        t.data[771] = 0;
        CHECK(decode_err(t.data, t.len));
        pc_buf_fini(&t);
        /* data offset past the end */
        pc_buf_init(&t);
        (void)pc_buf_append(&t, good.data, good.len, NULL);
        put32(t.data, true, ELEMENT0 + 28, 0xFFFFFFFEu);
        CHECK(decode_err(t.data, t.len));
        pc_buf_fini(&t);
        /* absurd size */
        pc_buf_init(&t);
        (void)pc_buf_append(&t, good.data, good.len, NULL);
        memset(t.data + 772, 0xFF, 8);
        CHECK(decode_err(t.data, t.len));
        pc_buf_fini(&t);
        /* magic alone */
        CHECK(decode_err((const uint8_t *)"SDPX", 4));
        pc_buf_fini(&good);
        fx_free(&fx);
    }
    /* Every truncation of a valid file, and a wrong bit depth: no crash. */
    {
        fixture fx = fx_new(50, 10, 2, 7, 5);
        pc_buf b = fx_bytes(&fx);
        size_t k;
        for (k = 0; k <= b.len; k++) {
            (void)decode_err(b.data, k);
        }
        {
            size_t bit;
            for (bit = 0; bit < b.len * 8; bit += 101) {
                b.data[bit / 8] ^= (uint8_t)(1u << (bit % 8));
                (void)decode_err(b.data, b.len);
                b.data[bit / 8] ^= (uint8_t)(1u << (bit % 8));
            }
        }
        CHECK(true);
        pc_buf_fini(&b);
        fx_free(&fx);
    }
}

int main(int argc, char **argv)
{
    RUN(test_every_descriptor_depth_and_packing_opens);
    RUN(test_big_and_little_endian_open_identically);
    RUN(test_ten_bit_code_value_rounds);
    RUN(test_ten_and_twelve_bit_written_back_at_depth);
    RUN(test_sixteen_bit_round_trips_losslessly);
    RUN(test_log_frames_report_encoding);
    RUN(test_only_first_element_imported);
    RUN(test_listed_header_fields_survive_round_trip);
    RUN(test_chosen_bit_depth_sets_header_and_packing);
    RUN(test_gray_alpha_written_as_luma);
    RUN(test_only_free_form_text_reported_dropped);
    RUN(test_stored_orientation_turned_upright);
    RUN(test_unsupported_and_damaged_error);
    return pc_t_finish();
}
