/* Brush, pattern, swatch and gradient files: ports the tests of crates/psd/src/{aco,ase,grd,
 * patterns,abr}.rs and replays `rustref f9-vectors` (tests/golden/f9-vectors.txt): writer
 * output, parse results, every truncation and a byte flip at every position, compared by hash. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/bytes.h"
#include "base/file.h"
#include "harness.h"
#include "psd/abr.h"
#include "psd/aco.h"
#include "psd/ase.h"
#include "psd/descriptor.h"
#include "psd/grd.h"
#include "psd/patterns.h"

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

static pc_psd_str S(const char *s)
{
    pc_psd_str r = {NULL, 0};
    pc_psd_str_set(&r, s, strlen(s), NULL);
    return r;
}

static float f32_bits(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* ---- dumps (same layout as rustref) ---- */

static void d_u16(pc_buf *o, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    pc_buf_append(o, b, 2, NULL);
}

static void d_u32(pc_buf *o, uint32_t v)
{
    uint8_t b[4];
    pc_store_le32(b, v);
    pc_buf_append(o, b, 4, NULL);
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
        pc_buf_append(o, b, n, NULL);
    }
}

static void d_byte(pc_buf *o, uint8_t v)
{
    pc_buf_append_byte(o, v, NULL);
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

static void dump_patterns(pc_buf *o, const pc_psd_pattern *p, size_t n)
{
    size_t i;
    d_u32(o, (uint32_t)n);
    for (i = 0; i < n; i++) {
        dump_pattern(o, &p[i]);
    }
}

static void dump_aco(pc_buf *o, const pc_psd_aco *f)
{
    size_t i;
    int k;
    d_byte(o, f->has_names);
    for (i = 0; i < f->n; i++) {
        uint16_t sp;
        uint16_t v[4];
        d_str(o, f->v[i].name.s, f->v[i].name.len);
        pc_psd_aco_to_record(f->v[i].color, &sp, v);
        d_u16(o, sp);
        for (k = 0; k < 4; k++) {
            d_u16(o, v[k]);
        }
    }
}

static void dump_ase_swatch(pc_buf *o, const pc_psd_ase_swatch *s)
{
    static const int nc[5] = {3, 4, 3, 1, 0};
    int i;
    d_str(o, s->name.s, s->name.len);
    d_byte(o, (uint8_t)s->color.model);
    if (s->color.model == PC_PSD_ASE_OTHER) {
        pc_buf_append(o, s->color.tag, 4, NULL);
        d_str(o, s->color.data.data, s->color.data.len);
    }
    for (i = 0; i < nc[s->color.model]; i++) {
        d_f32(o, s->color.c[i]);
    }
    d_byte(o, (uint8_t)s->kind);
}

static void dump_ase(pc_buf *o, const pc_psd_ase *f)
{
    size_t i;
    size_t k;
    for (i = 0; i < f->n; i++) {
        const pc_psd_ase_entry *e = &f->entries[i];
        if (!e->group) {
            d_byte(o, 'S');
            dump_ase_swatch(o, &e->swatch);
        } else {
            d_byte(o, 'G');
            d_str(o, e->name.s, e->name.len);
            d_u32(o, (uint32_t)e->n);
            for (k = 0; k < e->n; k++) {
                dump_ase_swatch(o, &e->swatches[k]);
            }
        }
    }
}

static void dump_grd(pc_buf *o, const pc_psd_grd_gradient *g, size_t n)
{
    static const int nc[7] = {3, 3, 4, 3, 1, 0, 0};
    size_t i;
    size_t k;
    int c;
    for (i = 0; i < n; i++) {
        d_str(o, g[i].name.s, g[i].name.len);
        d_byte(o, g[i].noise);
        d_u32(o, (uint32_t)g[i].nstops);
        for (k = 0; k < g[i].nstops; k++) {
            const pc_psd_grd_stop *s = &g[i].stops[k];
            d_f32(o, s->location);
            d_f32(o, s->midpoint);
            d_byte(o, (uint8_t)s->color.model);
            for (c = 0; c < nc[s->color.model]; c++) {
                d_f32(o, s->color.c[c]);
            }
        }
        d_u32(o, (uint32_t)g[i].nopacity);
        for (k = 0; k < g[i].nopacity; k++) {
            d_f32(o, g[i].opacity[k].location);
            d_f32(o, g[i].opacity[k].opacity);
            d_f32(o, g[i].opacity[k].midpoint);
        }
    }
}

static void dump_sample(pc_buf *o, const pc_psd_abr_sample *s)
{
    d_str(o, s->id.s, s->id.len);
    d_u32(o, s->width);
    d_u32(o, s->height);
    d_u16(o, s->depth);
    d_str(o, s->data.data, s->data.len);
}

static void dump_abr(pc_buf *o, const pc_psd_abr *f)
{
    size_t i;
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
    dump_patterns(o, f->patterns, f->npatterns);
    d_u32(o, (uint32_t)f->npresets);
    for (i = 0; i < f->npresets; i++) {
        pc_psd_bytes b = {NULL, 0};
        pc_psd_desc_to_bytes(&f->presets[i], &b, NULL);
        d_str(o, b.data, b.len);
        pc_psd_bytes_fini(&b);
    }
    d_u32(o, (uint32_t)f->warnings.n);
    for (i = 0; i < f->warnings.n; i++) {
        d_str(o, f->warnings.v[i], strlen(f->warnings.v[i]));
    }
}

/* ---- parse wrappers: parse, dump on success ---- */

typedef pc_status (*parse_fn)(const uint8_t *d, size_t n, pc_buf *o, pc_error *e);

static pc_status p_aco(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_aco f;
    pc_status st = pc_psd_aco_parse(d, n, &f, e);
    if (st == PC_OK) {
        dump_aco(o, &f);
        pc_psd_aco_fini(&f);
    }
    return st;
}

static pc_status p_ase(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_ase f;
    pc_status st = pc_psd_ase_parse(d, n, &f, e);
    if (st == PC_OK) {
        dump_ase(o, &f);
        pc_psd_ase_fini(&f);
    }
    return st;
}

static pc_status p_grd(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_grd_gradient *g;
    size_t ng;
    pc_status st = pc_psd_grd_parse(d, n, &g, &ng, e);
    if (st == PC_OK) {
        dump_grd(o, g, ng);
        pc_psd_grd_free(g, ng);
    }
    return st;
}

static pc_status p_block(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_pattern *p;
    size_t np;
    pc_status st = pc_psd_parse_pattern_block(d, n, &p, &np, e);
    if (st == PC_OK) {
        dump_patterns(o, p, np);
        pc_psd_patterns_free(p, np);
    }
    return st;
}

static pc_status p_pat(const uint8_t *d, size_t n, pc_buf *o, pc_error *e)
{
    pc_psd_pattern *p;
    size_t np;
    pc_status st = pc_psd_parse_pat_file(d, n, &p, &np, e);
    if (st == PC_OK) {
        dump_patterns(o, p, np);
        pc_psd_patterns_free(p, np);
    }
    return st;
}

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

static void outcome(uint64_t *h, parse_fn fn, const uint8_t *d, size_t n)
{
    pc_buf o = PC_BUF_INIT;
    pc_error e;
    pc_buf_append(&o, "ok", 2, NULL);
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
    for (i = 0; i < n; i++) {
        memcpy(c, d, n);
        c[i] ^= 0x5a;
        outcome(h, fn, c, n);
    }
    free(c);
}

/* Rust `bytes_or`: "ok" + the bytes, or "err" + the message. */
static void bytes_or(uint64_t *h, pc_status st, const pc_psd_bytes *b, const pc_error *e)
{
    if (st == PC_OK) {
        fnv(h, "ok", 2);
        fnv(h, b->data, b->len);
    } else {
        fnv(h, "err", 3);
        fnv(h, e->msg, strlen(e->msg));
    }
}

/* ---- inputs ---- */

static pc_psd_aco_swatch aco_sw(const char *name, pc_psd_aco_kind kind, uint16_t space, uint16_t a, uint16_t b,
                                uint16_t c, uint16_t d)
{
    pc_psd_aco_swatch s;
    memset(&s, 0, sizeof s);
    s.name = S(name);
    s.color.kind = kind;
    s.color.space = space;
    s.color.v[0] = a;
    s.color.v[1] = b;
    s.color.v[2] = c;
    s.color.v[3] = d;
    return s;
}

#define ACO_N 7
static void aco_sample(pc_psd_aco_swatch s[ACO_N])
{
    s[0] = aco_sw("Pure Red", PC_PSD_ACO_RGB, 0, 65535, 0, 0, 0);
    s[1] = aco_sw("Hue", PC_PSD_ACO_HSB, 0, 12000, 65535, 40000, 0);
    s[2] = aco_sw("Cyan ink", PC_PSD_ACO_CMYK, 0, 65535, 0, 0, 1234);
    s[3] = aco_sw("Lab \xe2\x88\x92", PC_PSD_ACO_LAB, 0, 5321, (uint16_t)-12800, 12700, 0);
    s[4] = aco_sw("Gray", PC_PSD_ACO_GRAY, 0, 5000, 0, 0, 0);
    s[5] = aco_sw("Book \xe6\x97\xa5\xe6\x9c\xac", PC_PSD_ACO_OTHER, 3, 1, 2, 3, 4);
    s[6] = aco_sw("", PC_PSD_ACO_RGB, 0, 1, 2, 3, 0);
}

static void aco_free(pc_psd_aco_swatch *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_psd_str_fini(&s[i].name);
    }
}

static pc_psd_ase_swatch ase_sw(const char *name, pc_psd_ase_model m, float a, float b, float c, float d,
                                pc_psd_ase_kind kind)
{
    pc_psd_ase_swatch s;
    memset(&s, 0, sizeof s);
    s.name = S(name);
    s.color.model = m;
    s.color.c[0] = a;
    s.color.c[1] = b;
    s.color.c[2] = c;
    s.color.c[3] = d;
    s.kind = kind;
    return s;
}

static pc_psd_ase ase_sample(void)
{
    pc_psd_ase f;
    static const uint8_t odd[6] = {1, 2, 3, 4, 5, 6};
    f.n = 3;
    f.entries = calloc(3, sizeof *f.entries);
    f.entries[0].swatch = ase_sw("Loose red", PC_PSD_ASE_RGB, 1.0f, 0.0f, 0.0f, 0.0f, PC_PSD_ASE_PROCESS);
    f.entries[1].group = true;
    f.entries[1].name = S("Brand \xe2\x9c\x93");
    f.entries[1].n = 4;
    f.entries[1].swatches = calloc(4, sizeof(pc_psd_ase_swatch));
    f.entries[1].swatches[0] = ase_sw("Ink", PC_PSD_ASE_CMYK, 0.1f, 0.2f, 0.3f, 0.4f, PC_PSD_ASE_SPOT);
    f.entries[1].swatches[1] = ase_sw("Lab", PC_PSD_ASE_LAB, 0.5321f, -12.25f, 40.5f, 0.0f, PC_PSD_ASE_GLOBAL);
    f.entries[1].swatches[2] = ase_sw("Mid gray", PC_PSD_ASE_GRAY, 0.5f, 0, 0, 0, PC_PSD_ASE_PROCESS);
    f.entries[1].swatches[3] = ase_sw("Odd", PC_PSD_ASE_OTHER, 0, 0, 0, 0, PC_PSD_ASE_PROCESS);
    memcpy(f.entries[1].swatches[3].color.tag, "HSV ", 4);
    pc_psd_bytes_set(&f.entries[1].swatches[3].color.data, odd, 6, NULL);
    f.entries[2].group = true;
    return f;
}

static pc_psd_grd_gradient *grd_sample(size_t *n)
{
    pc_psd_grd_gradient *g = calloc(2, sizeof *g);
    pc_psd_grd_stop *s = calloc(4, sizeof *s);
    pc_psd_grd_opacity *o = calloc(3, sizeof *o);
    g[0].name = S("Sunset \xe2\x9c\x93");
    s[0].midpoint = 0.5f;
    s[0].color.model = PC_PSD_GRD_FOREGROUND;
    s[1].location = 0.25f;
    s[1].midpoint = 0.3f;
    s[1].color.model = PC_PSD_GRD_RGB;
    s[1].color.c[0] = 1.0f;
    s[1].color.c[1] = 0.5f;
    s[2].location = 1.0f;
    s[2].midpoint = 0.5f;
    s[2].color.model = PC_PSD_GRD_BACKGROUND;
    s[3].location = 0.7f;
    s[3].midpoint = 0.1f;
    s[3].color.model = PC_PSD_GRD_LAB;
    s[3].color.c[0] = 50.0f;
    s[3].color.c[1] = 1.0f;
    s[3].color.c[2] = 2.0f;
    g[0].stops = s;
    g[0].nstops = 4;
    o[0].opacity = 1.0f;
    o[0].midpoint = 0.5f;
    o[1].location = 1.0f;
    o[1].opacity = 0.25f;
    o[1].midpoint = 0.5f;
    o[2].location = 0.333f;
    o[2].opacity = 0.777f;
    o[2].midpoint = 0.123f;
    g[0].opacity = o;
    g[0].nopacity = 3;
    g[1].name = S("Noise");
    g[1].noise = true;
    *n = 2;
    return g;
}

static pc_psd_pattern pat(uint16_t depth, bool alpha)
{
    pc_psd_pattern p;
    size_t bpp = depth / 8;
    size_t len = 5 * 3 * bpp;
    size_t i;
    int c;
    memset(&p, 0, sizeof p);
    p.mode = 3;
    p.width = 5;
    p.height = 3;
    p.name = S("Test \xe2\x9c\x93");
    p.id = S("0bd2d3ba-1234-11d4-8f8f-aabbccddeeff");
    p.depth = depth;
    p.nchannels = 3;
    p.channels = calloc(3, sizeof *p.channels);
    for (c = 0; c < 4; c++) {
        uint8_t s = (uint8_t)(c < 3 ? c + 1 : 9);
        pc_psd_bytes *dst = c < 3 ? &p.channels[c] : &p.alpha.v;
        if (c == 3 && !alpha) {
            break;
        }
        dst->data = malloc(len);
        dst->len = len;
        for (i = 0; i < len; i++) {
            dst->data[i] = (uint8_t)((uint8_t)(i * 7) + s);
        }
    }
    p.alpha.has = alpha;
    return p;
}

static pc_psd_abr_sample abr_sample(const char *id, uint32_t w, uint32_t h, uint16_t depth)
{
    pc_psd_abr_sample s;
    size_t bpp = depth / 8;
    size_t n = (size_t)w * h * bpp;
    size_t i;
    memset(&s, 0, sizeof s);
    s.id = S(id);
    s.width = w;
    s.height = h;
    s.depth = depth;
    s.data.data = malloc(n);
    s.data.len = n;
    for (i = 0; i < n; i++) {
        s.data.data[i] = (i / bpp) % 3 == 0 ? 255 : (uint8_t)((i % 7) * 30);
    }
    return s;
}

static void abr_legacy(uint16_t version, pc_psd_abr_legacy b[3])
{
    memset(b, 0, 3 * sizeof *b);
    b[0].spacing = 25;
    b[0].anti_alias = true;
    b[0].tip = PC_PSD_ABR_COMPUTED;
    b[0].diameter = 19;
    b[0].hardness = 80;
    b[0].angle = -30;
    b[0].roundness = 50;
    b[1].name = S(version == 2 ? "Leaf \xe2\x9c\x93" : "");
    b[1].spacing = 40;
    b[1].tip = PC_PSD_ABR_SAMPLED;
    b[1].sample = abr_sample("", 13, 9, 8);
    b[2].spacing = 10;
    b[2].anti_alias = true;
    b[2].tip = PC_PSD_ABR_SAMPLED;
    b[2].sample = abr_sample("", 4, 6, 16);
}

static void legacy_free(pc_psd_abr_legacy *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_psd_abr_legacy_fini(&b[i]);
    }
}

/* Descriptor helpers (each takes ownership of the values it is given). */
static void add(pc_psd_desc *d, const char *k, pc_psd_value v)
{
    CHECK_EQ_INT(pc_psd_desc_add(d, k, &v, NULL), PC_OK);
}

static pc_psd_value v_obj(pc_psd_desc d)
{
    pc_psd_value v;
    pc_psd_value_object(&d, &v, NULL);
    return v;
}

static pc_psd_value v_glob(pc_psd_desc d)
{
    pc_psd_value v = v_obj(d);
    v.kind = PC_PSD_V_GLOBAL_OBJECT;
    return v;
}

static pc_psd_value v_enum(const char *t, const char *x)
{
    pc_psd_value v;
    pc_psd_value_enum(t, x, &v, NULL);
    return v;
}

static pc_psd_value v_uf(const char *unit, double x)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, unit, 4);
    v.d = x;
    return v;
}

static pc_psd_value v_text(const char *s, bool nul)
{
    pc_psd_value v;
    pc_psd_value_text(s, nul, &v, NULL);
    return v;
}

static pc_psd_value v_large(int64_t x)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_LARGE_INTEGER;
    v.l = x;
    return v;
}

static pc_psd_desc D(const char *cls)
{
    pc_psd_desc d;
    pc_psd_desc_init(&d, cls, NULL);
    return d;
}

static pc_psd_value v_list(void)
{
    pc_psd_value v;
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_LIST;
    return v;
}

static void push(pc_psd_value *l, pc_psd_value v)
{
    pc_psd_value_list_push(l, &v, NULL);
}

static pc_psd_bytes grd_models(void)
{
    pc_psd_value stops = v_list();
    pc_psd_value trns = v_list();
    pc_psd_value items = v_list();
    pc_psd_desc d;
    pc_psd_desc c;
    pc_psd_desc root;
    pc_psd_bytes vd = {NULL, 0};
    pc_psd_bytes out = {NULL, 0};
    /* HSB */
    c = D("HSBC");
    add(&c, "H   ", v_uf("#Ang", 210.5));
    add(&c, "Strt", pc_psd_value_double(40.0));
    add(&c, "Brgh", pc_psd_value_int(77));
    d = D("Clrt");
    add(&d, "Clr ", v_obj(c));
    add(&d, "Type", v_enum("Clry", "UsrS"));
    add(&d, "Lctn", pc_psd_value_int(0));
    add(&d, "Mdpn", pc_psd_value_int(50));
    push(&stops, v_obj(d));
    /* CMYK */
    c = D("CMYC");
    add(&c, "Cyn ", pc_psd_value_double(10.25));
    add(&c, "Mgnt", pc_psd_value_double(20.0));
    add(&c, "Ylw ", pc_psd_value_double(30.0));
    add(&c, "Blck", pc_psd_value_double(40.5));
    d = D("Clrt");
    add(&d, "Clr ", v_obj(c));
    add(&d, "Lctn", pc_psd_value_int(1024));
    add(&d, "Mdpn", pc_psd_value_int(33));
    push(&stops, v_obj(d));
    /* Lab */
    c = D("LbCl");
    add(&c, "Lmnc", pc_psd_value_double(53.2));
    add(&c, "A   ", pc_psd_value_double(-12.5));
    add(&c, "B   ", pc_psd_value_double(40.0));
    d = D("Clrt");
    add(&d, "Clr ", v_obj(c));
    add(&d, "Lctn", v_large(2048));
    push(&stops, v_obj(d));
    /* Gray */
    c = D("Grsc");
    add(&c, "Gry ", pc_psd_value_double(25.0));
    d = D("Clrt");
    add(&d, "Clr ", v_obj(c));
    add(&d, "Lctn", pc_psd_value_double(5000.0));
    push(&stops, v_obj(d));
    /* RGB as a global object */
    c = D("RGBC");
    add(&c, "Rd  ", pc_psd_value_double(255.0));
    add(&c, "Grn ", pc_psd_value_double(128.5));
    add(&c, "Bl  ", pc_psd_value_double(0.0));
    d = D("Clrt");
    add(&d, "Clr ", v_glob(c));
    add(&d, "Lctn", pc_psd_value_int(-20));
    add(&d, "Mdpn", pc_psd_value_double(1e300));
    push(&stops, v_obj(d));
    /* unknown class */
    d = D("Clrt");
    add(&d, "Clr ", v_obj(D("XXXX")));
    push(&stops, v_obj(d));
    d = D("Clrt");
    add(&d, "Type", v_enum("Clry", "FrgC"));
    add(&d, "Lctn", pc_psd_value_int(4096));
    push(&stops, v_obj(d));
    d = D("Clrt");
    add(&d, "Type", v_enum("Clry", "BckC"));
    add(&d, "Mdpn", pc_psd_value_int(-5));
    push(&stops, v_obj(d));
    push(&stops, pc_psd_value_int(5));
    c = D("RGBC");
    add(&c, "Rd  ", pc_psd_value_double(pc_f64_from_bits(0x7ff8000000000000u)));
    d = D("Clrt");
    add(&d, "Clr ", v_obj(c));
    push(&stops, v_obj(d));
    push(&stops, v_obj(D("Clrt")));
    /* transparency */
    d = D("TrnS");
    add(&d, "Opct", v_uf("#Prc", 50.0));
    add(&d, "Lctn", pc_psd_value_int(0));
    add(&d, "Mdpn", pc_psd_value_int(50));
    push(&trns, v_obj(d));
    push(&trns, v_obj(D("TrnS")));
    push(&trns, pc_psd_value_int(3));
    d = D("TrnS");
    add(&d, "Opct", pc_psd_value_double(250.0));
    add(&d, "Lctn", pc_psd_value_double(-3.0));
    push(&trns, v_obj(d));
    /* gradients */
    d = D("Grdn");
    add(&d, "Nm  ", v_text("Models \xe2\x9c\x93", true));
    add(&d, "GrdF", v_enum("GrdF", "CstS"));
    add(&d, "Clrs", stops);
    add(&d, "Trns", trns);
    c = D("Grdn");
    add(&c, "Grad", v_obj(d));
    push(&items, v_obj(c));
    push(&items, pc_psd_value_int(1));
    push(&items, v_obj(D("Grdn")));
    d = D("Grdn");
    add(&d, "Nm  ", v_text("G", false));
    add(&d, "GrdF", v_enum("GrdF", "ClNs"));
    c = D("Grdn");
    add(&c, "Grad", v_glob(d));
    push(&items, v_obj(c));
    root = D("null");
    add(&root, "GrdL", items);
    pc_psd_vdesc_to_bytes(&root, &vd, NULL);
    pc_psd_desc_fini(&root);
    out.len = 6 + vd.len;
    out.data = malloc(out.len);
    memcpy(out.data, "8BGR\0\x05", 6);
    memcpy(out.data + 6, vd.data, vd.len);
    pc_psd_bytes_fini(&vd);
    return out;
}

static pc_psd_desc preset_desc(void)
{
    pc_psd_desc p = D("brushPreset");
    pc_psd_desc b = D("sampledBrush");
    add(&p, "Nm  ", v_text("Grass", true));
    add(&b, "sampledData", v_text("$abc", true));
    add(&p, "Brsh", v_obj(b));
    return p;
}

static uint8_t lcg_byte(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return (uint8_t)(*seed >> 24);
}

/* ---- golden replay ---- */

static pc_buf golden;
static int compared;

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
                pc_t_name = name;
                PC_T_FAIL("hash %s, want %.16s", got, p + kl);
            }
            return;
        }
        p += len + 1;
    }
    pc_t_name = name;
    PC_T_FAIL("no golden line");
}

static void test_parity_aco(void)
{
    pc_psd_aco_swatch s[ACO_N];
    pc_psd_bytes b = {NULL, 0};
    uint64_t h = FNV0;
    pc_error e;
    pc_psd_aco_swatch *many;
    pc_psd_aco_swatch longname;
    static const uint8_t v1[24] = {0, 1, 0, 2, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 8, 0x13, 0x88, 0, 0, 0, 0, 0, 0};
    static const uint8_t v2[22] = {0, 2, 0, 1, 0, 0, 0, 1, 0, 2, 0, 3, 0, 0, 0, 0, 0, 2, 0, 'O', 0, 'K'};
    uint32_t seed = 0x12345678u;
    uint8_t junk[302];
    int len;
    int i;
    aco_sample(s);
    CHECK_EQ_INT(pc_psd_aco_write(s, ACO_N, &b, NULL), PC_OK);
    sweep(&h, b.data, b.len, p_aco);
    expect("aco sample", h);
    pc_psd_bytes_fini(&b);
    aco_free(s, ACO_N);
    h = FNV0;
    sweep(&h, v1, sizeof v1, p_aco);
    sweep(&h, v2, sizeof v2, p_aco);
    longname = aco_sw("", PC_PSD_ACO_GRAY, 0, 0, 0, 0, 0);
    {
        char *x = malloc(5001);
        memset(x, 'x', 5000);
        x[5000] = '\0';
        pc_psd_str_set(&longname.name, x, 5000, NULL);
        free(x);
    }
    pc_psd_aco_write(&longname, 1, &b, NULL);
    fnv(&h, b.data, b.len);
    outcome(&h, p_aco, b.data, b.len);
    pc_psd_bytes_fini(&b);
    aco_free(&longname, 1);
    many = calloc(65536, sizeof *many);
    for (i = 0; i < 65536; i++) {
        many[i].color.kind = PC_PSD_ACO_GRAY;
    }
    bytes_or(&h, pc_psd_aco_write(many, 65536, &b, &e), &b, &e);
    free(many);
    for (len = 0; len < 300; len++) {
        junk[0] = 0;
        junk[1] = 1;
        for (i = 0; i < len; i++) {
            junk[2 + i] = lcg_byte(&seed);
        }
        outcome(&h, p_aco, junk + 2, (size_t)len);
        outcome(&h, p_aco, junk, (size_t)len + 2);
    }
    expect("aco misc", h);
}

static void test_parity_ase(void)
{
    pc_psd_ase f = ase_sample();
    pc_psd_bytes b = {NULL, 0};
    uint64_t h = FNV0;
    pc_error e;
    static const uint8_t lenient[] = "ASEF\0\x01\0\0\0\0\0\x04\xc0\x02\0\0\0\0\x77\x77\0\0\0\x03\x09\x09\x09\xc0\x01\0\0\0"
                                     "\x06\0\x02\0G\0\0\0\x01\0\0\0\x0e\0\x02\0A\0\0Gray\x3e\x80\0\0";
    uint32_t seed = 0xDEADBEEFu;
    uint8_t buf[12 + 400];
    int len;
    int i;
    CHECK_EQ_INT(pc_psd_ase_write(&f, &b, NULL), PC_OK);
    sweep(&h, b.data, b.len, p_ase);
    expect("ase sample", h);
    pc_psd_bytes_fini(&b);
    pc_psd_ase_fini(&f);
    h = FNV0;
    f.n = 1;
    f.entries = calloc(1, sizeof *f.entries);
    f.entries[0].swatch =
        ase_sw("n", PC_PSD_ASE_RGB, f32_bits(0x7fc00000u), f32_bits(0x7f800000u), 0.5f, 0, PC_PSD_ASE_PROCESS);
    pc_psd_ase_write(&f, &b, NULL);
    sweep(&h, b.data, b.len, p_ase);
    pc_psd_bytes_fini(&b);
    pc_psd_ase_fini(&f);
    sweep(&h, lenient, sizeof lenient - 1, p_ase);
    f.n = 1;
    f.entries = calloc(1, sizeof *f.entries);
    f.entries[0].group = true;
    f.entries[0].name = S("g");
    f.entries[0].n = 100001;
    f.entries[0].swatches = calloc(100001, sizeof(pc_psd_ase_swatch));
    for (i = 0; i < 100001; i++) {
        f.entries[0].swatches[i].color.model = PC_PSD_ASE_GRAY;
        f.entries[0].swatches[i].kind = PC_PSD_ASE_PROCESS;
    }
    bytes_or(&h, pc_psd_ase_write(&f, &b, &e), &b, &e);
    pc_psd_ase_fini(&f);
    for (len = 0; len < 400; len++) {
        memcpy(buf, "ASEF\0\x01\0\0\0\0\0\x10", 12);
        for (i = 0; i < len; i++) {
            buf[12 + i] = lcg_byte(&seed);
        }
        outcome(&h, p_ase, buf, 12 + (size_t)len);
        outcome(&h, p_ase, buf + 12, (size_t)len);
    }
    expect("ase misc", h);
}

static void test_parity_grd(void)
{
    size_t n;
    pc_psd_grd_gradient *g = grd_sample(&n);
    pc_psd_bytes b = {NULL, 0};
    uint64_t h = FNV0;
    static const struct {
        const char *d;
        size_t n;
    } bad[5] = {{"", 0}, {"8BG", 3}, {"8BGR\0", 5}, {"8BGR\0\x03", 6}, {"8BPS\0\x05", 6}};
    int i;
    CHECK_EQ_INT(pc_psd_grd_write(g, n, &b, NULL), PC_OK);
    sweep(&h, b.data, b.len, p_grd);
    expect("grd sample", h);
    pc_psd_bytes_fini(&b);
    pc_psd_grd_free(g, n);
    h = FNV0;
    b = grd_models();
    sweep(&h, b.data, b.len, p_grd);
    pc_psd_bytes_fini(&b);
    pc_psd_grd_write(NULL, 0, &b, NULL);
    outcome(&h, p_grd, b.data, b.len);
    pc_psd_bytes_fini(&b);
    for (i = 0; i < 5; i++) {
        outcome(&h, p_grd, (const uint8_t *)bad[i].d, bad[i].n);
    }
    expect("grd models", h);
}

static void set_i32(uint8_t *b, size_t at, int32_t v)
{
    pc_store_be32(b + at, (uint32_t)v);
}

static void test_parity_patterns(void)
{
    static const uint16_t depths[3] = {8, 16, 32};
    pc_psd_pattern extra[4];
    pc_psd_bytes b = {NULL, 0};
    pc_error e;
    uint64_t h;
    int d;
    int a;
    int i;
    for (d = 0; d < 3; d++) {
        for (a = 0; a < 2; a++) {
            pc_psd_pattern ps[2];
            char name[32];
            ps[0] = pat(depths[d], a != 0);
            ps[1] = pat(depths[d], false);
            pc_psd_str_fini(&ps[1].name);
            ps[1].name = S("Second");
            h = FNV0;
            CHECK_EQ_INT(pc_psd_write_pattern_block(ps, 2, &b, NULL), PC_OK);
            sweep(&h, b.data, b.len, p_block);
            pc_psd_bytes_fini(&b);
            CHECK_EQ_INT(pc_psd_write_pat_file(ps, 2, &b, NULL), PC_OK);
            sweep(&h, b.data, b.len, p_pat);
            pc_psd_bytes_fini(&b);
            (void)snprintf(name, sizeof name, "pat d%u a%d", (unsigned)depths[d], a);
            expect(name, h);
            pc_psd_pattern_fini(&ps[0]);
            pc_psd_pattern_fini(&ps[1]);
        }
    }
    /* indexed, short palette, CMYK 16 + alpha, 1-bit gray */
    extra[0] = pat(8, false);
    extra[0].mode = 2;
    pc_psd_bytes_fini(&extra[0].channels[1]);
    pc_psd_bytes_fini(&extra[0].channels[2]);
    extra[0].nchannels = 1;
    extra[0].palette.has = true;
    extra[0].palette.v.len = 768;
    extra[0].palette.v.data = malloc(768);
    for (i = 0; i < 768; i++) {
        extra[0].palette.v.data[i] = (uint8_t)(i * 3 % 256);
    }
    pc_psd_pattern_clone(&extra[0], &extra[1], NULL);
    {
        static const uint8_t ten[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        pc_psd_bytes_set(&extra[1].palette.v, ten, 10, NULL);
        pc_psd_str_fini(&extra[1].name);
        extra[1].name = S("short palette");
    }
    extra[2] = pat(16, true);
    extra[2].mode = 4;
    extra[2].channels = realloc(extra[2].channels, 4 * sizeof *extra[2].channels);
    extra[2].channels[3].len = 30;
    extra[2].channels[3].data = malloc(30);
    memset(extra[2].channels[3].data, 0x42, 30);
    extra[2].nchannels = 4;
    memset(&extra[3], 0, sizeof extra[3]);
    extra[3].mode = 1;
    extra[3].width = 5;
    extra[3].height = 3;
    extra[3].name = S("bits");
    extra[3].depth = 1;
    extra[3].nchannels = 1;
    extra[3].channels = calloc(1, sizeof *extra[3].channels);
    {
        static const uint8_t bits[3] = {0xA8, 0x50, 0xF8};
        pc_psd_bytes_set(&extra[3].channels[0], bits, 3, NULL);
    }
    h = FNV0;
    for (i = 0; i < 4; i++) {
        CHECK_EQ_INT(pc_psd_write_pattern_block(&extra[i], 1, &b, NULL), PC_OK);
        sweep(&h, b.data, b.len, p_block);
        pc_psd_bytes_fini(&b);
        pc_psd_pattern_fini(&extra[i]);
    }
    for (d = 0; d < 3; d++) {
        pc_psd_pattern p = pat(depths[d], true);
        size_t vma = 4 + 16 + 7 * 2 + 1 + 36 + 8;
        pc_psd_bytes orig = {NULL, 0};
        uint8_t *m;
        pc_psd_write_pattern_block(&p, 1, &orig, NULL);
        m = malloc(orig.len);
        memcpy(m, orig.data, orig.len);
        set_i32(m, vma, -2);
        set_i32(m, vma + 4, -3);
        set_i32(m, vma + 8, 4);
        set_i32(m, vma + 12, 5);
        outcome(&h, p_block, m, orig.len);
        if (depths[d] == 32) {
            /* Over the decode budget; at 8 and 16 bits it fits and would allocate gigabytes. */
            memcpy(m, orig.data, orig.len);
            set_i32(m, vma + 8, 30000);
            set_i32(m, vma + 12, 30000);
            outcome(&h, p_block, m, orig.len);
        }
        memcpy(m, orig.data, orig.len);
        set_i32(m, vma + 4, INT32_MIN);
        set_i32(m, vma + 12, INT32_MAX);
        outcome(&h, p_block, m, orig.len);
        memcpy(m, orig.data, orig.len);
        set_i32(m, vma + 8, 0);
        outcome(&h, p_block, m, orig.len);
        free(m);
        pc_psd_bytes_fini(&orig);
        pc_psd_pattern_fini(&p);
    }
    {
        pc_psd_pattern p = pat(8, false);
        size_t vma = 4 + 16 + 7 * 2 + 1 + 36 + 8;
        p.mode = 1;
        pc_psd_bytes_fini(&p.channels[1]);
        pc_psd_bytes_fini(&p.channels[2]);
        p.nchannels = 1;
        pc_psd_write_pattern_block(&p, 1, &b, NULL);
        set_i32(b.data, vma + 8, 1000);
        set_i32(b.data, vma + 12, 900);
        outcome(&h, p_block, b.data, b.len);
        pc_psd_bytes_fini(&b);
        pc_psd_pattern_fini(&p);
    }
    outcome(&h, p_pat, (const uint8_t *)"8BPS\0\x01", 6);
    outcome(&h, p_pat, (const uint8_t *)"8BP", 3);
    outcome(&h, p_pat, (const uint8_t *)"", 0);
    {
        pc_psd_pattern p = pat(8, false);
        p.width = 40000;
        bytes_or(&h, pc_psd_write_pattern_block(&p, 1, &b, &e), &b, &e);
        pc_psd_bytes_fini(&b);
        pc_psd_pattern_fini(&p);
    }
    expect("pat misc", h);
}

static void test_parity_abr(void)
{
    pc_psd_abr_legacy br[3];
    pc_psd_abr_sample samples[2];
    pc_psd_desc preset = preset_desc();
    pc_psd_bytes b = {NULL, 0};
    pc_error e;
    uint64_t h;
    uint16_t version;
    int rle;
    uint16_t sub;
    char name[32];
    size_t i;
    for (version = 1; version <= 2; version++) {
        for (rle = 0; rle < 2; rle++) {
            abr_legacy(version, br);
            h = FNV0;
            CHECK_EQ_INT(pc_psd_abr_write_v12(version, br, 3, rle != 0, &b, NULL), PC_OK);
            sweep(&h, b.data, b.len, p_abr);
            pc_psd_bytes_fini(&b);
            legacy_free(br, 3);
            (void)snprintf(name, sizeof name, "abr v%u rle%d", (unsigned)version, rle);
            expect(name, h);
        }
    }
    samples[0] = abr_sample("$abc", 20, 11, 8);
    samples[1] = abr_sample("$def", 5, 5, 16);
    for (sub = 1; sub <= 2; sub++) {
        for (rle = 0; rle < 2; rle++) {
            pc_psd_pattern p = pat(8, true);
            h = FNV0;
            CHECK_EQ_INT(pc_psd_abr_write_v6(sub, samples, 2, &p, rle ? 1 : 0, &preset, 1, rle != 0, &b, NULL), PC_OK);
            sweep(&h, b.data, b.len, p_abr);
            pc_psd_bytes_fini(&b);
            pc_psd_pattern_fini(&p);
            (void)snprintf(name, sizeof name, "abr v6.%u rle%d", (unsigned)sub, rle);
            expect(name, h);
        }
    }
    h = FNV0;
    {
        static const uint8_t a1[4] = {0, 3, 0, 0};
        static const uint8_t a2[4] = {0, 6, 0, 9};
        static const uint8_t a3[4] = {0, 2, 0xff, 0xff};
        static const uint8_t a4[10] = {0, 1, 0, 1, 0, 3, 0, 0, 0, 0};
        static const uint8_t a5[16] = {0, 6, 0, 1, '8', 'B', 'I', 'M', 'z', 'z', 'z', 'z', 0, 0, 0, 0};
        outcome(&h, p_abr, a1, 0);
        outcome(&h, p_abr, a1, 4);
        outcome(&h, p_abr, a2, 4);
        outcome(&h, p_abr, a3, 4);
        outcome(&h, p_abr, a4, 10);
        outcome(&h, p_abr, a5, 16);
    }
    {
        pc_psd_abr_legacy lb;
        memset(&lb, 0, sizeof lb);
        lb.spacing = 1;
        lb.anti_alias = true;
        lb.tip = PC_PSD_ABR_SAMPLED;
        lb.sample = abr_sample("", 2, 2, 8);
        lb.sample.width = PC_PSD_ABR_MAX_EDGE + 1;
        bytes_or(&h, pc_psd_abr_write_v12(1, &lb, 1, false, &b, &e), &b, &e);
        pc_psd_bytes_fini(&b);
        pc_psd_abr_legacy_fini(&lb);
    }
    bytes_or(&h, pc_psd_abr_write_v12(3, NULL, 0, false, &b, &e), &b, &e);
    bytes_or(&h, pc_psd_abr_write_v6(3, NULL, 0, NULL, 0, NULL, 0, false, &b, &e), &b, &e);
    for (i = 2; i-- > 0;) {
        float *u;
        size_t n;
        size_t k;
        pc_psd_abr_sample_to_unit(&samples[i], &u, &n, NULL);
        for (k = 0; k < n; k++) {
            fnv(&h, &u[k], 4);
        }
        free(u);
    }
    expect("abr misc", h);
    pc_psd_abr_sample_fini(&samples[0]);
    pc_psd_abr_sample_fini(&samples[1]);
    pc_psd_desc_fini(&preset);
}

/* ---- aco.rs tests ---- */

static void test_aco(void)
{
    pc_psd_aco_swatch s[ACO_N];
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes b2 = {NULL, 0};
    pc_psd_aco f;
    pc_psd_aco_color c;
    size_t i;
    size_t n;
    static const uint16_t cy[4] = {0, 65535, 65535, 65535};
    static const uint16_t lab[4] = {10000, 0xFF38, 200, 0};
    static const uint16_t g[4] = {10000, 0, 0, 0};
    static const uint16_t o[4] = {7, 8, 9, 10};
    aco_sample(s);
    CHECK_EQ_INT(pc_psd_aco_write(s, ACO_N, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_aco_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK(f.has_names && f.n == ACO_N);
    for (i = 0; i < ACO_N && i < f.n; i++) {
        CHECK(pc_psd_aco_swatch_eq(&f.v[i], &s[i]));
    }
    CHECK_EQ_INT(pc_psd_aco_write(f.v, f.n, &b2, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&b, &b2));
    pc_psd_aco_fini(&f);
    pc_psd_bytes_fini(&b2);
    /* the spec encodings */
    c = pc_psd_aco_from_record(2, cy);
    CHECK(c.kind == PC_PSD_ACO_CMYK && c.v[0] == 65535 && c.v[1] == 0 && c.v[2] == 0 && c.v[3] == 0);
    c = pc_psd_aco_from_record(7, lab);
    CHECK(c.kind == PC_PSD_ACO_LAB && c.v[0] == 10000 && (int16_t)c.v[1] == -200 && (int16_t)c.v[2] == 200);
    c = pc_psd_aco_from_record(8, g);
    CHECK(c.kind == PC_PSD_ACO_GRAY && c.v[0] == 10000);
    c = pc_psd_aco_from_record(9, o);
    CHECK(c.kind == PC_PSD_ACO_OTHER && c.space == 9 && memcmp(c.v, o, sizeof o) == 0);
    /* every truncation inside a section fails (a cut exactly after v1 reads v1) */
    n = 4 + ACO_N * 10;
    for (i = 0; i < b.len; i++) {
        pc_status st = pc_psd_aco_parse(b.data, i, &f, NULL);
        if (st == PC_OK) {
            pc_psd_aco_fini(&f);
        }
        if (i == n || (i > n && i < n + 4)) {
            CHECK_EQ_INT(st, PC_OK);
        } else {
            CHECK(st != PC_OK);
        }
    }
    CHECK(pc_psd_aco_parse((const uint8_t *)"", 0, &f, NULL) != PC_OK);
    CHECK(pc_psd_aco_parse((const uint8_t *)"\0\3\0\0", 4, &f, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(pc_psd_aco_parse((const uint8_t *)"\0\1\xff\xff", 4, &f, NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    aco_free(s, ACO_N);
}

static void test_aco_versions_and_long_names(void)
{
    static const uint8_t v1[28] = {0, 1, 0, 2, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 8, 0x13, 0x88, 0, 0, 0, 0, 0, 0};
    static const uint8_t v2[22] = {0, 2, 0, 1, 0, 0, 0, 1, 0, 2, 0, 3, 0, 0, 0, 0, 0, 2, 0, 'O', 0, 'K'};
    pc_psd_aco f;
    pc_psd_aco_swatch longname = aco_sw("", PC_PSD_ACO_GRAY, 0, 0, 0, 0, 0);
    pc_psd_bytes b = {NULL, 0};
    char *x = malloc(5000);
    CHECK_EQ_INT(pc_psd_aco_parse(v1, 24, &f, NULL), PC_OK);
    CHECK(!f.has_names && f.n == 2 && f.v[0].color.kind == PC_PSD_ACO_RGB && f.v[0].color.v[0] == 65535 &&
          f.v[1].color.kind == PC_PSD_ACO_GRAY && f.v[1].color.v[0] == 5000);
    pc_psd_aco_fini(&f);
    CHECK_EQ_INT(pc_psd_aco_parse(v1, 28, &f, NULL), PC_OK); /* trailing padding */
    CHECK_EQ_UINT(f.n, 2);
    pc_psd_aco_fini(&f);
    CHECK_EQ_INT(pc_psd_aco_parse(v2, 22, &f, NULL), PC_OK);
    CHECK(f.n == 1 && strcmp(pc_psd_str_c(&f.v[0].name), "OK") == 0 && f.v[0].color.v[2] == 3);
    pc_psd_aco_fini(&f);
    {
        static const uint8_t bad[20] = {0, 2, 0, 1, 0, 0, 0, 1, 0, 2, 0, 3, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 65};
        CHECK(pc_psd_aco_parse(bad, 20, &f, NULL) != PC_OK);
    }
    memset(x, 'x', 5000);
    pc_psd_str_set(&longname.name, x, 5000, NULL);
    free(x);
    pc_psd_aco_write(&longname, 1, &b, NULL);
    CHECK_EQ_INT(pc_psd_aco_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK_EQ_UINT(f.v[0].name.len, PC_PSD_ACO_MAX_NAME_UNITS);
    pc_psd_aco_fini(&f);
    pc_psd_bytes_fini(&b);
    aco_free(&longname, 1);
}

/* ---- ase.rs tests ---- */

static void test_ase(void)
{
    pc_psd_ase f = ase_sample();
    pc_psd_ase back;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes b2 = {NULL, 0};
    size_t i;
    CHECK_EQ_INT(pc_psd_ase_write(&f, &b, NULL), PC_OK);
    CHECK(memcmp(b.data, "ASEF", 4) == 0 && pc_load_be32(b.data + 8) == 9);
    CHECK_EQ_INT(pc_psd_ase_parse(b.data, b.len, &back, NULL), PC_OK);
    CHECK(pc_psd_ase_eq(&back, &f));
    CHECK_EQ_UINT(pc_psd_ase_swatch_count(&back), 5);
    CHECK_EQ_INT(pc_psd_ase_write(&back, &b2, NULL), PC_OK);
    CHECK(pc_psd_bytes_eq(&b, &b2));
    pc_psd_ase_fini(&back);
    for (i = 0; i < b.len; i++) {
        CHECK(pc_psd_ase_parse(b.data, i, &back, NULL) != PC_OK);
    }
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEX\0\1\0\0\0\0\0\0", 12, &back, NULL) != PC_OK);
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEF\0\2\0\0\0\0\0\0", 12, &back, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEF\0\1\0\0\xff\xff\xff\xff", 12, &back, NULL) != PC_OK);
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEF\0\1\0\0\0\0\0\1\0\1\xff\xff\xff\xff", 18, &back, NULL) != PC_OK);
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEF\0\1\0\0\0\0\0\1\0\1\0\0\0\4\xff\xff\0A", 22, &back, NULL) !=
          PC_OK);
    CHECK(pc_psd_ase_parse((const uint8_t *)"ASEF\0\1\0\0\0\0\0\1\0\1\0\0\0\x0a\0\1\0\0RGB \0\0", 28, &back,
                           NULL) != PC_OK);
    pc_psd_bytes_fini(&b);
    pc_psd_bytes_fini(&b2);
    pc_psd_ase_fini(&f);
}

static void test_ase_lenient_and_non_finite(void)
{
    static const uint8_t lenient[] = "ASEF\0\x01\0\0\0\0\0\x04\xc0\x02\0\0\0\0\x77\x77\0\0\0\x03\x09\x09\x09\xc0\x01\0\0\0"
                                     "\x06\0\x02\0G\0\0\0\x01\0\0\0\x0e\0\x02\0A\0\0Gray\x3e\x80\0\0";
    pc_psd_ase f;
    pc_psd_bytes b = {NULL, 0};
    CHECK_EQ_INT(pc_psd_ase_parse(lenient, sizeof lenient - 1, &f, NULL), PC_OK);
    CHECK(f.n == 1 && f.entries[0].group && strcmp(pc_psd_str_c(&f.entries[0].name), "G") == 0 &&
          f.entries[0].n == 1 && f.entries[0].swatches[0].color.model == PC_PSD_ASE_GRAY &&
          f.entries[0].swatches[0].color.c[0] == 0.25f && f.entries[0].swatches[0].kind == PC_PSD_ASE_PROCESS);
    pc_psd_ase_fini(&f);
    f.n = 1;
    f.entries = calloc(1, sizeof *f.entries);
    f.entries[0].swatch = ase_sw("n", PC_PSD_ASE_RGB, f32_bits(0x7fc00000u), INFINITY, 0.5f, 0, PC_PSD_ASE_PROCESS);
    pc_psd_ase_write(&f, &b, NULL);
    pc_psd_ase_fini(&f);
    CHECK_EQ_INT(pc_psd_ase_parse(b.data, b.len, &f, NULL), PC_OK);
    CHECK(f.entries[0].swatch.color.c[0] == 0.0f && f.entries[0].swatch.color.c[1] == 0.0f &&
          f.entries[0].swatch.color.c[2] == 0.5f);
    pc_psd_ase_fini(&f);
    pc_psd_bytes_fini(&b);
}

/* ---- grd.rs tests ---- */

static void test_grd(void)
{
    size_t n;
    size_t nb;
    size_t i;
    pc_psd_grd_gradient *g = grd_sample(&n);
    pc_psd_grd_gradient *back;
    pc_psd_bytes b = {NULL, 0};
    pc_psd_bytes e = {NULL, 0};
    /* As the Rust sample: no Lab stop (written as black RGB) and no third opacity stop (its
     * location does not survive the 0..4096 integer field). */
    g[0].nstops = 3;
    g[0].nopacity = 2;
    CHECK_EQ_INT(pc_psd_grd_write(g, n, &b, NULL), PC_OK);
    CHECK_EQ_INT(pc_psd_grd_parse(b.data, b.len, &back, &nb, NULL), PC_OK);
    CHECK(nb == 2 && pc_psd_grd_gradient_eq(&back[0], &g[0]) && pc_psd_grd_gradient_eq(&back[1], &g[1]));
    pc_psd_grd_free(back, nb);
    CHECK(pc_psd_grd_parse((const uint8_t *)"", 0, &back, &nb, NULL) != PC_OK);
    CHECK(pc_psd_grd_parse((const uint8_t *)"8BGR\0\3", 6, &back, &nb, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(pc_psd_grd_parse((const uint8_t *)"8BPS\0\5", 6, &back, &nb, NULL) != PC_OK);
    for (i = 0; i < b.len; i++) {
        if (pc_psd_grd_parse(b.data, i, &back, &nb, NULL) == PC_OK) {
            pc_psd_grd_free(back, nb);
        }
        b.data[i] ^= 0x5a;
        if (pc_psd_grd_parse(b.data, b.len, &back, &nb, NULL) == PC_OK) {
            pc_psd_grd_free(back, nb);
        }
        b.data[i] ^= 0x5a;
    }
    pc_psd_grd_write(NULL, 0, &e, NULL);
    CHECK(pc_psd_grd_parse(e.data, e.len, &back, &nb, NULL) == PC_ERR_FORMAT);
    pc_psd_bytes_fini(&e);
    pc_psd_bytes_fini(&b);
    g[0].nstops = 4;
    g[0].nopacity = 3;
    pc_psd_grd_free(g, n);
}

/* ---- patterns.rs tests ---- */

static size_t vma_offset(size_t start, const pc_psd_pattern *p)
{
    size_t nu;
    pc_psd_ustr s;
    pc_psd_ustr_from_utf8n(pc_psd_str_c(&p->name), p->name.len, false, &s, NULL);
    nu = s.n;
    pc_psd_ustr_fini(&s);
    return start + 16 + (nu + 1) * 2 + 1 + p->id.len + (p->mode == 2 ? 768 : 0) + 8;
}

static void test_patterns_round_trip(void)
{
    static const uint16_t depths[3] = {8, 16, 32};
    int d;
    int a;
    for (d = 0; d < 3; d++) {
        for (a = 0; a < 2; a++) {
            pc_psd_pattern ps[2];
            pc_psd_pattern *got = NULL;
            size_t n = 0;
            pc_psd_bytes blk = {NULL, 0};
            pc_psd_bytes f = {NULL, 0};
            pc_psd_bytes again = {NULL, 0};
            ps[0] = pat(depths[d], a != 0);
            ps[1] = pat(depths[d], false);
            pc_psd_str_fini(&ps[1].name);
            ps[1].name = S("Second");
            CHECK_EQ_INT(pc_psd_write_pattern_block(ps, 2, &blk, NULL), PC_OK);
            CHECK_EQ_UINT(blk.len % 4, 0);
            CHECK_EQ_INT(pc_psd_parse_pattern_block(blk.data, blk.len, &got, &n, NULL), PC_OK);
            CHECK(n == 2 && pc_psd_pattern_eq(&got[0], &ps[0]) && pc_psd_pattern_eq(&got[1], &ps[1]));
            CHECK_EQ_INT(pc_psd_write_pattern_block(got, n, &again, NULL), PC_OK);
            CHECK(pc_psd_bytes_eq(&again, &blk));
            pc_psd_patterns_free(got, n);
            CHECK_EQ_INT(pc_psd_write_pat_file(ps, 2, &f, NULL), PC_OK);
            CHECK_EQ_INT(pc_psd_parse_pat_file(f.data, f.len, &got, &n, NULL), PC_OK);
            CHECK(n == 2 && pc_psd_pattern_eq(&got[0], &ps[0]) && pc_psd_pattern_eq(&got[1], &ps[1]));
            pc_psd_patterns_free(got, n);
            pc_psd_bytes_fini(&blk);
            pc_psd_bytes_fini(&f);
            pc_psd_bytes_fini(&again);
            pc_psd_pattern_fini(&ps[0]);
            pc_psd_pattern_fini(&ps[1]);
        }
    }
    {
        pc_psd_pattern *got = (pc_psd_pattern *)&golden;
        size_t n = 9;
        uint8_t k[4];
        CHECK_EQ_INT(pc_psd_parse_pattern_block(NULL, 0, &got, &n, NULL), PC_OK);
        CHECK(got == NULL && n == 0);
        CHECK(pc_psd_parse_pat_file((const uint8_t *)"8BPS\0\1", 6, &got, &n, NULL) != PC_OK);
        pc_psd_pattern_block_key(8, k);
        CHECK(memcmp(k, "Patt", 4) == 0);
        pc_psd_pattern_block_key(16, k);
        CHECK(memcmp(k, "Pat2", 4) == 0);
        pc_psd_pattern_block_key(32, k);
        CHECK(memcmp(k, "Pat3", 4) == 0);
    }
}

/* An ABR v6 file holding p; *vma is the pattern's VMA rect offset inside it. */
static pc_psd_bytes abr_with(const pc_psd_pattern *p, size_t *vma)
{
    pc_psd_abr_sample s = abr_sample("", 1, 1, 8);
    pc_psd_bytes b = {NULL, 0};
    size_t i;
    s.data.data[0] = 0;
    CHECK_EQ_INT(pc_psd_abr_write_v6(1, &s, 1, p, 1, NULL, 0, true, &b, NULL), PC_OK);
    pc_psd_abr_sample_fini(&s);
    for (i = 0; i + 4 <= b.len; i++) {
        if (memcmp(b.data + i, "patt", 4) == 0) {
            break;
        }
    }
    *vma = vma_offset(i + 8, p) + 4;
    return b;
}

static void hostile_rect(const pc_psd_pattern *p, size_t k1, int32_t v1, size_t k2, int32_t v2, pc_status want,
                         const char *msg)
{
    pc_psd_bytes blk = {NULL, 0};
    pc_psd_bytes f = {NULL, 0};
    pc_psd_bytes abr;
    pc_psd_pattern *got = NULL;
    pc_psd_abr parsed;
    pc_error e;
    size_t n;
    size_t vma;
    size_t i;
    bool warned = false;
    pc_psd_write_pattern_block(p, 1, &blk, NULL);
    vma = vma_offset(4, p);
    set_i32(blk.data, vma + k1, v1);
    set_i32(blk.data, vma + k2, v2);
    CHECK_EQ_INT(pc_psd_parse_pattern_block(blk.data, blk.len, &got, &n, &e), want);
    if (msg != NULL) {
        CHECK(strcmp(e.msg, msg) == 0);
    }
    pc_psd_write_pat_file(p, 1, &f, NULL);
    vma = vma_offset(10, p);
    set_i32(f.data, vma + k1, v1);
    set_i32(f.data, vma + k2, v2);
    CHECK_EQ_INT(pc_psd_parse_pat_file(f.data, f.len, &got, &n, &e), want);
    if (msg != NULL) {
        CHECK(strcmp(e.msg, msg) == 0);
    }
    abr = abr_with(p, &vma);
    set_i32(abr.data, vma + k1, v1);
    set_i32(abr.data, vma + k2, v2);
    CHECK_EQ_INT(pc_psd_abr_parse(abr.data, abr.len, &parsed, NULL), PC_OK);
    CHECK_EQ_UINT(parsed.npatterns, 0);
    for (i = 0; i < parsed.warnings.n; i++) {
        warned |= strstr(parsed.warnings.v[i], "embedded patterns unreadable") != NULL;
    }
    CHECK(warned);
    pc_psd_abr_fini(&parsed);
    pc_psd_bytes_fini(&abr);
    pc_psd_bytes_fini(&blk);
    pc_psd_bytes_fini(&f);
}

static void test_patterns_hostile(void)
{
    pc_psd_pattern p = pat(8, false);
    pc_psd_pattern cmyk = pat(16, true);
    pc_psd_pattern rgb32 = pat(32, false);
    pc_psd_bytes blk = {NULL, 0};
    pc_psd_pattern *got = NULL;
    size_t n;
    size_t cuts[4];
    size_t i;
    /* rectangle endpoints whose difference overflows */
    hostile_rect(&p, 4, INT32_MIN, 12, INT32_MAX, PC_ERR_LIMIT, NULL);
    /* a huge rectangle with a tiny payload: refused before the first plane is allocated */
    hostile_rect(&rgb32, 8, 30000, 12, 30000, PC_ERR_LIMIT, "limit exceeded: pattern data exceeds MAX_DECODED_BYTES");
    cmyk.mode = 4;
    cmyk.channels = realloc(cmyk.channels, 4 * sizeof *cmyk.channels);
    cmyk.channels[3].data = calloc(30, 1);
    cmyk.channels[3].len = 30;
    cmyk.nchannels = 4;
    hostile_rect(&cmyk, 8, 30000, 12, 30000, PC_ERR_LIMIT, "limit exceeded: pattern data exceeds MAX_DECODED_BYTES");
    /* zero-size rectangle */
    pc_psd_write_pattern_block(&p, 1, &blk, NULL);
    set_i32(blk.data, vma_offset(4, &p) + 8, 0);
    CHECK(pc_psd_parse_pattern_block(blk.data, blk.len, &got, &n, NULL) != PC_OK);
    pc_psd_bytes_fini(&blk);
    /* truncations */
    {
        pc_psd_pattern a = pat(8, true);
        pc_psd_write_pattern_block(&a, 1, &blk, NULL);
        cuts[0] = 3;
        cuts[1] = 10;
        cuts[2] = 40;
        cuts[3] = blk.len - 5;
        for (i = 0; i < 4; i++) {
            if (pc_psd_parse_pattern_block(blk.data, cuts[i], &got, &n, NULL) == PC_OK) {
                pc_psd_patterns_free(got, n);
            }
        }
        pc_psd_bytes_fini(&blk);
        pc_psd_pattern_fini(&a);
    }
    pc_psd_pattern_fini(&p);
    pc_psd_pattern_fini(&cmyk);
    pc_psd_pattern_fini(&rgb32);
}

static void test_patterns_offset_channels(void)
{
    static const uint16_t depths[3] = {8, 16, 32};
    int d;
    for (d = 0; d < 3; d++) {
        pc_psd_pattern o = pat(depths[d], true);
        pc_psd_bytes blk = {NULL, 0};
        pc_psd_pattern *got = NULL;
        size_t n = 0;
        size_t vma = vma_offset(4, &o);
        size_t bps = depths[d] / 8;
        size_t c;
        size_t x;
        size_t y;
        pc_psd_write_pattern_block(&o, 1, &blk, NULL);
        set_i32(blk.data, vma, -2);
        set_i32(blk.data, vma + 4, -3);
        set_i32(blk.data, vma + 8, 4);
        set_i32(blk.data, vma + 12, 5);
        CHECK_EQ_INT(pc_psd_parse_pattern_block(blk.data, blk.len, &got, &n, NULL), PC_OK);
        CHECK(n == 1 && got[0].width == 8 && got[0].height == 6 && got[0].alpha.has);
        for (c = 0; n == 1 && c < 4; c++) {
            const pc_psd_bytes *act = c < 3 ? &got[0].channels[c] : &got[0].alpha.v;
            const pc_psd_bytes *exp = c < 3 ? &o.channels[c] : &o.alpha.v;
            for (y = 0; y < 6; y++) {
                for (x = 0; x < 8; x++) {
                    const uint8_t *a = act->data + (y * 8 + x) * bps;
                    if (y >= 2 && y < 5 && x >= 3) {
                        CHECK(memcmp(a, exp->data + ((y - 2) * 5 + (x - 3)) * bps, bps) == 0);
                    } else {
                        size_t k;
                        for (k = 0; k < bps; k++) {
                            CHECK(a[k] == 0);
                        }
                    }
                }
            }
        }
        pc_psd_patterns_free(got, n);
        pc_psd_bytes_fini(&blk);
        pc_psd_pattern_fini(&o);
    }
}

static void test_patterns_place_plane(void)
{
    static const uint16_t depths[4] = {1, 8, 16, 32};
    pc_psd_layout src = {1, 5, 2, 1, PC_PSD_V1};
    pc_psd_layout dst = {1, 8, 4, 1, PC_PSD_V1};
    pc_psd_bytes plane = {NULL, 0};
    static const uint8_t in[2] = {0xA8, 0x50};
    static const uint8_t want[4] = {0, 0x2A, 0x14, 0};
    int d;
    pc_psd_bytes_set(&plane, in, 2, NULL);
    CHECK_EQ_INT(pc_psd_pattern_place_plane(&plane, &src, 2, 1, &dst, NULL), PC_OK);
    CHECK(plane.len == 4 && memcmp(plane.data, want, 4) == 0);
    pc_psd_bytes_fini(&plane);
    /* full-size channels keep the decoded allocation */
    for (d = 0; d < 4; d++) {
        pc_psd_layout l = {1, 5, 3, depths[d], PC_PSD_V1};
        size_t len;
        size_t i;
        uint8_t *orig;
        uint8_t expect_byte = depths[d] == 1 ? 0x50 : 0x55;
        pc_psd_layout_decoded_len(&l, &len, NULL);
        plane.data = malloc(len);
        plane.len = len;
        memset(plane.data, 0x55, len);
        orig = plane.data;
        CHECK_EQ_INT(pc_psd_pattern_place_plane(&plane, &l, 0, 0, &l, NULL), PC_OK);
        CHECK(plane.data == orig);
        for (i = 0; i < plane.len; i++) {
            CHECK(plane.data[i] == expect_byte);
        }
        pc_psd_bytes_fini(&plane);
    }
}

static void test_patterns_large_padding_allowed(void)
{
    pc_psd_pattern p = pat(8, false);
    pc_psd_bytes blk = {NULL, 0};
    pc_psd_pattern *got = NULL;
    size_t n = 0;
    size_t vma;
    size_t y;
    p.mode = 1;
    pc_psd_bytes_fini(&p.channels[1]);
    pc_psd_bytes_fini(&p.channels[2]);
    p.nchannels = 1;
    pc_psd_write_pattern_block(&p, 1, &blk, NULL);
    vma = vma_offset(4, &p);
    /* More than 64 MiB of implicit padding is valid. */
    set_i32(blk.data, vma + 8, 8193);
    set_i32(blk.data, vma + 12, 8193);
    CHECK_EQ_INT(pc_psd_parse_pattern_block(blk.data, blk.len, &got, &n, NULL), PC_OK);
    if (n == 1) {
        const pc_psd_bytes *pl = &got[0].channels[0];
        CHECK_EQ_UINT(pl->len, (size_t)8193 * 8193);
        for (y = 0; y < 3; y++) {
            CHECK(memcmp(pl->data + y * 8193, p.channels[0].data + y * 5, 5) == 0);
        }
        CHECK(pl->data[5] == 0 && pl->data[pl->len - 1] == 0);
    }
    pc_psd_patterns_free(got, n);
    pc_psd_bytes_fini(&blk);
    pc_psd_pattern_fini(&p);
}

/* ---- abr.rs tests ---- */

static void test_abr_v12(void)
{
    uint16_t version;
    int rle;
    for (version = 1; version <= 2; version++) {
        for (rle = 0; rle < 2; rle++) {
            pc_psd_abr_legacy br[3];
            pc_psd_bytes b = {NULL, 0};
            pc_psd_abr f;
            size_t i;
            abr_legacy(version, br);
            CHECK_EQ_INT(pc_psd_abr_write_v12(version, br, 3, rle != 0, &b, NULL), PC_OK);
            CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
            CHECK(f.version == version && f.nlegacy == 3 && f.warnings.n == 0);
            for (i = 0; i < 3 && i < f.nlegacy; i++) {
                CHECK(pc_psd_abr_legacy_eq(&f.legacy[i], &br[i]));
            }
            pc_psd_abr_fini(&f);
            pc_psd_bytes_fini(&b);
            legacy_free(br, 3);
        }
    }
}

static void test_abr_v6(void)
{
    pc_psd_abr_sample s[2];
    pc_psd_desc preset = preset_desc();
    uint16_t sub;
    int rle;
    float *u;
    size_t n;
    s[0] = abr_sample("$abc", 20, 11, 8);
    s[1] = abr_sample("$def", 5, 5, 16);
    for (sub = 1; sub <= 2; sub++) {
        for (rle = 0; rle < 2; rle++) {
            pc_psd_bytes b = {NULL, 0};
            pc_psd_abr f;
            CHECK_EQ_INT(pc_psd_abr_write_v6(sub, s, 2, NULL, 0, &preset, 1, rle != 0, &b, NULL), PC_OK);
            CHECK_EQ_INT(pc_psd_abr_parse(b.data, b.len, &f, NULL), PC_OK);
            CHECK(f.version == 6 && f.subversion == sub && f.nsamples == 2 && f.npresets == 1 && f.warnings.n == 0);
            CHECK(f.nsamples == 2 && pc_psd_abr_sample_eq(&f.samples[0], &s[0]) &&
                  pc_psd_abr_sample_eq(&f.samples[1], &s[1]));
            CHECK(f.npresets == 1 && pc_psd_desc_eq(&f.presets[0], &preset));
            pc_psd_abr_fini(&f);
            pc_psd_bytes_fini(&b);
        }
    }
    pc_psd_abr_sample_to_unit(&s[1], &u, &n, NULL);
    CHECK_EQ_UINT(n, 25);
    free(u);
    pc_psd_abr_sample_fini(&s[0]);
    pc_psd_abr_sample_fini(&s[1]);
    pc_psd_desc_fini(&preset);
}

static void test_abr_hostile(void)
{
    pc_psd_abr f;
    pc_psd_abr_sample s = abr_sample("$a", 30, 30, 8);
    pc_psd_desc preset = D("brushPreset");
    pc_psd_abr_legacy lb;
    pc_psd_bytes good = {NULL, 0};
    pc_psd_bytes b = {NULL, 0};
    size_t i;
    CHECK(pc_psd_abr_parse((const uint8_t *)"", 0, &f, NULL) != PC_OK);
    CHECK(pc_psd_abr_parse((const uint8_t *)"\0\3\0\0", 4, &f, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(pc_psd_abr_parse((const uint8_t *)"\0\6\0\x09", 4, &f, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(pc_psd_abr_parse((const uint8_t *)"\0\2\xff\xff", 4, &f, NULL) != PC_OK);
    memset(&lb, 0, sizeof lb);
    lb.tip = PC_PSD_ABR_SAMPLED;
    lb.sample = abr_sample("", 2, 2, 8);
    lb.sample.width = PC_PSD_ABR_MAX_EDGE + 1;
    CHECK(pc_psd_abr_write_v12(1, &lb, 1, false, &b, NULL) == PC_ERR_LIMIT);
    pc_psd_abr_legacy_fini(&lb);
    CHECK_EQ_INT(pc_psd_abr_write_v6(2, &s, 1, NULL, 0, &preset, 1, true, &good, NULL), PC_OK);
    for (i = 0; i < good.len; i++) {
        if (pc_psd_abr_parse(good.data, i, &f, NULL) == PC_OK) {
            pc_psd_abr_fini(&f);
        }
        good.data[i] ^= 0xa5;
        if (pc_psd_abr_parse(good.data, good.len, &f, NULL) == PC_OK) {
            pc_psd_abr_fini(&f);
        }
        good.data[i] ^= 0xa5;
    }
    pc_psd_bytes_fini(&good);
    pc_psd_abr_sample_fini(&s);
    pc_psd_desc_fini(&preset);
}

int main(int argc, char **argv)
{
    if (pc_file_read("tests/golden/f9-vectors.txt", (size_t)1 << 20, &golden, NULL) != PC_OK) {
        (void)printf("tests/golden/f9-vectors.txt missing\n");
        return 1;
    }
    RUN(test_parity_aco);
    RUN(test_parity_ase);
    RUN(test_parity_grd);
    RUN(test_parity_patterns);
    RUN(test_parity_abr);
    (void)printf("(%d golden lines matched) ", compared);
    CHECK_EQ_INT(compared, 22);
    RUN(test_aco);
    RUN(test_aco_versions_and_long_names);
    RUN(test_ase);
    RUN(test_ase_lenient_and_non_finite);
    RUN(test_grd);
    RUN(test_patterns_round_trip);
    RUN(test_patterns_hostile);
    RUN(test_patterns_offset_channels);
    RUN(test_patterns_place_plane);
    RUN(test_patterns_large_padding_allowed);
    RUN(test_abr_v12);
    RUN(test_abr_v6);
    RUN(test_abr_hostile);
    pc_buf_fini(&golden);
    return pc_t_finish();
}
