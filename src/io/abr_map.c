/* Photoshop brushes (.abr) -> brush presets (port of crates/io/src/abr_map.rs). See abr_map.h.
 *
 * The mapper keeps a sticky status: allocation failures inside the per-section helpers are
 * recorded in m->st (first one wins) and checked after each brush, which keeps the mapping code
 * as flat as the reference's. */
#include "io/abr_map.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "base/utf8.h"
#include "io/maps_desc.h"

/* ---- sorted string sets (BTreeSet<String>) ---- */

typedef struct sset {
    pc_str *v;
    size_t n;
    size_t cap;
} sset;

static void sset_fini(sset *s)
{
    for (size_t i = 0; i < s->n; i++) {
        pc_str_fini(&s->v[i]);
    }
    free(s->v);
    memset(s, 0, sizeof *s);
}

/* Inserts s[0..len) unless present; takes nothing. */
static pc_status sset_insert(sset *s, const char *p, size_t len, pc_error *err)
{
    pc_str k = {(char *)(uintptr_t)p, len};
    size_t lo = 0;
    size_t hi = s->n;
    pc_str copy = PC_STR_INIT;
    pc_str *g;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = pc_str_cmp(&s->v[mid], &k);
        if (c == 0) {
            return PC_OK;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (pc_str_set(&copy, p, len, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    g = pc_grow_array(s->v, &s->cap, s->n + 1, sizeof *s->v);
    if (g == NULL) {
        pc_str_fini(&copy);
        return pc_io_nomem(err, "importing brushes");
    }
    s->v = g;
    memmove(&s->v[lo + 1], &s->v[lo], (s->n - lo) * sizeof *s->v);
    s->v[lo] = copy;
    s->n++;
    return PC_OK;
}

/* ---- descriptor helpers (abr_map's own: num also reads LargeInteger and drops non-finite) ---- */

static bool num(const pc_psd_desc *d, const char *k, double *out)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, k) : NULL;
    double x;
    if (v == NULL) {
        return false;
    }
    switch (v->kind) {
    case PC_PSD_V_DOUBLE:
    case PC_PSD_V_UNIT_FLOAT:
        x = v->d;
        break;
    case PC_PSD_V_INTEGER:
        x = (double)v->i;
        break;
    case PC_PSD_V_LARGE_INTEGER:
        x = (double)v->l;
        break;
    default:
        return false;
    }
    if (!isfinite(x)) {
        return false;
    }
    *out = x;
    return true;
}

/* A percentage as a 0..1 fraction. */
static bool pct(const pc_psd_desc *d, const char *k, float *out)
{
    double v;
    if (!num(d, k, &v)) {
        return false;
    }
    *out = pc_f64_to_f32(v / 100.0);
    return true;
}

static bool boolean(const pc_psd_desc *d, const char *k, bool *out)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, k) : NULL;
    if (v == NULL || v->kind != PC_PSD_V_BOOLEAN) {
        return false;
    }
    *out = v->b;
    return true;
}

static bool bool_or(const pc_psd_desc *d, const char *k, bool dflt)
{
    bool v = dflt;
    (void)boolean(d, k, &v);
    return v;
}

static float pct_or(const pc_psd_desc *d, const char *k, float dflt)
{
    float v = dflt;
    (void)pct(d, k, &v);
    return v;
}

/* Text value (lossy) into *out; *has false when absent. */
static pc_status text(const pc_psd_desc *d, const char *k, bool *has, pc_str *out, pc_error *err)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, k) : NULL;
    *has = v != NULL && v->kind == PC_PSD_V_TEXT;
    return *has ? pc_io4_ustr_lossy(&v->text, out, err) : PC_OK;
}

/* The enumerated value's bytes (borrowed) or NULL. */
static const uint8_t *enum_value(const pc_psd_desc *d, const char *k, size_t *len)
{
    return pc_io4_enum(d, k, len);
}

static bool bytes_is(const uint8_t *b, size_t len, const char *s)
{
    size_t n = strlen(s);
    return b != NULL && len == n && memcmp(b, s, n) == 0;
}

static pc_paint_mask_mode mask_mode(const uint8_t *code, size_t len, pc_paint_mask_mode dflt)
{
    static const struct {
        const char *code;
        pc_paint_mask_mode mode;
    } map[] = {
        {"Mltp", PC_PAINT_MASK_MULTIPLY},
        {"Sbtr", PC_PAINT_MASK_SUBTRACT},
        {"Drkn", PC_PAINT_MASK_DARKEN},
        {"Ovrl", PC_PAINT_MASK_OVERLAY},
        {"CDdg", PC_PAINT_MASK_COLOR_DODGE},
        {"CBrn", PC_PAINT_MASK_COLOR_BURN},
        {"linearBurn", PC_PAINT_MASK_LINEAR_BURN},
        {"hardMix", PC_PAINT_MASK_HARD_MIX},
        {"linearHeight", PC_PAINT_MASK_LINEAR_HEIGHT},
        {"height", PC_PAINT_MASK_HEIGHT},
        {"Hght", PC_PAINT_MASK_HEIGHT},
    };
    /* Enum bytes are compared after UTF-8 lossy decoding in the reference; the codes are ASCII,
     * so a byte comparison gives the same answers. */
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (bytes_is(code, len, map[i].code)) {
            return map[i].mode;
        }
    }
    return dflt;
}

/* Preset-level keys this mapper understands (anything else is reported as unmapped). */
static const char *const KNOWN[] = {
    "Nm  ",
    "Brsh",
    "useTipDynamics",
    "flipX",
    "flipY",
    "brushProjection",
    "minimumDiameter",
    "minimumRoundness",
    "tiltScale",
    "szVr",
    "angleDynamics",
    "roundnessDynamics",
    "useScatter",
    "Spcn",
    "Cnt ",
    "bothAxes",
    "countDynamics",
    "scatterDynamics",
    "dualBrush",
    "brushGroup",
    "useTexture",
    "TxtC",
    "interpretation",
    "textureBlendMode",
    "textureDepth",
    "minimumDepth",
    "textureDepthDynamics",
    "Txtr",
    "textureScale",
    "InvT",
    "protectTexture",
    "textureBrightness",
    "textureContrast",
    "usePaintDynamics",
    "prVr",
    "opVr",
    "wtVr",
    "mxVr",
    "useColorDynamics",
    "clVr",
    "H   ",
    "Strt",
    "Brgh",
    "purity",
    "colorDynamicsPerTip",
    "Wtdg",
    "Nose",
    "Rpt ",
    "useLegacy",
    "useBrushPose",
    "overridePoseAngle",
    "overridePoseTiltX",
    "overridePoseTiltY",
    "overridePosePressure",
    "brushPosePressure",
    "brushPoseTiltX",
    "brushPoseTiltY",
    "brushPoseAngle",
    "toolOptions",
};

static bool known(const uint8_t *k, size_t len)
{
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        if (bytes_is(k, len, KNOWN[i])) {
            return true;
        }
    }
    return false;
}

/* ---- the mapper ---- */

typedef struct mapper {
    const pc_psd_abr *file;
    sset warnings;
    sset unknown;
    pc_status st;
    pc_error *err;
} mapper;

static void warn(mapper *m, const char *msg)
{
    if (m->st == PC_OK) {
        m->st = sset_insert(&m->warnings, msg, strlen(msg), m->err);
    }
}

static void warn_quoted(mapper *m, const char *pre, const pc_str *what, const char *post)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    if (m->st != PC_OK) {
        return;
    }
    st = pc_buf_append_str(&b, pre, m->err);
    if (st == PC_OK) {
        st = pc_buf_append(&b, pc_str_c(what), what->len, m->err);
    }
    if (st == PC_OK) {
        st = pc_buf_append_str(&b, post, m->err);
    }
    if (st == PC_OK) {
        st = sset_insert(&m->warnings, (const char *)b.data, b.len, m->err);
    }
    pc_buf_fini(&b);
    m->st = st;
}

/* Box-downsamples a w x h plane so neither side exceeds max; *v replaced when it shrinks. */
static pc_status fit(uint32_t *w, uint32_t *h, float **v, size_t *n, uint32_t max, pc_error *err)
{
    uint32_t ow = *w;
    uint32_t oh = *h;
    float k;
    uint32_t nw;
    uint32_t nh;
    float *out;
    if (ow <= max && oh <= max) {
        return PC_OK;
    }
    k = pc_rs_maxf_k((float)(ow > oh ? ow : oh) / (float)max, 1.0f);
    nw = pc_f32_to_u32(roundf((float)ow / k));
    nh = pc_f32_to_u32(roundf((float)oh / k));
    nw = nw < 1 ? 1 : (nw > max ? max : nw);
    nh = nh < 1 ? 1 : (nh > max ? max : nh);
    out = pc_malloc_array((size_t)nw * nh, sizeof *out);
    if (out == NULL) {
        return pc_io_nomem(err, "downsampling a brush tip");
    }
    for (uint64_t y = 0; y < nh; y++) {
        uint64_t y0 = y * oh / nh;
        uint64_t y1 = (y + 1) * oh / nh;
        y1 = y1 > y0 + 1 ? y1 : y0 + 1;
        y1 = y1 < oh ? y1 : oh;
        for (uint64_t x = 0; x < nw; x++) {
            uint64_t x0 = x * ow / nw;
            uint64_t x1 = (x + 1) * ow / nw;
            float sum = 0.0f;
            size_t cnt = 0;
            x1 = x1 > x0 + 1 ? x1 : x0 + 1;
            x1 = x1 < ow ? x1 : ow;
            for (uint64_t yy = y0; yy < y1; yy++) {
                for (uint64_t xx = x0; xx < x1; xx++) {
                    uint64_t at = yy * ow + xx;
                    if (at < *n) {
                        sum += (*v)[at];
                        cnt++;
                    }
                }
            }
            out[y * nw + x] = cnt > 0 ? sum / (float)cnt : 0.0f;
        }
    }
    free(*v);
    *v = out;
    *n = (size_t)nw * nh;
    *w = nw;
    *h = nh;
    return PC_OK;
}

static pc_status gray_tile(uint32_t w, uint32_t h, const float *v, size_t n, pc_paint_gray_tile *out,
                           pc_error *err)
{
    uint16_t *data = pc_malloc_array(n > 0 ? n : 1, sizeof *data);
    pc_status st;
    if (data == NULL) {
        return pc_io_nomem(err, "importing brushes");
    }
    for (size_t i = 0; i < n; i++) {
        data[i] = pc_paint_q16(v[i]);
    }
    st = pc_paint_gray_tile_from_u16(w, h, data, n, out, err);
    free(data);
    return st;
}

/* A sampled tip (downsampled to the tip limit) and its size (larger side). */
static void tip(mapper *m, const pc_psd_abr_sample *s, pc_paint_tip *out, float *size)
{
    float *v = NULL;
    size_t n = 0;
    uint32_t w = s->width;
    uint32_t h = s->height;
    pc_paint_tip t;
    pc_paint_tip_default(&t);
    *out = t;
    *size = (float)(s->width > s->height ? s->width : s->height);
    if (m->st != PC_OK) {
        return;
    }
    m->st = pc_psd_abr_sample_to_unit(s, &v, &n, m->err);
    if (m->st == PC_OK) {
        m->st = fit(&w, &h, &v, &n, PC_IO_ABR_MAX_TIP_EDGE, m->err);
    }
    if (m->st == PC_OK && (w != s->width || h != s->height)) {
        warn(m, "tips larger than 2500 px were downsampled");
    }
    if (m->st == PC_OK) {
        t.kind = PC_PAINT_TIP_SAMPLED;
        m->st = gray_tile(w, h, v, n, &t.tile, m->err);
        if (m->st == PC_OK) {
            *out = t;
        }
    }
    free(v);
}

static pc_paint_dynamic dynamic(mapper *m, const pc_psd_desc *d)
{
    pc_paint_dynamic r = pc_paint_dynamic_default();
    double v = 0.0;
    float f = 0.0f;
    if (d == NULL) {
        return r;
    }
    (void)num(d, "bVTy", &v);
    switch (pc_f64_to_i64(v)) {
    case 1:
        r.control = PC_PAINT_CONTROL_FADE;
        break;
    case 2:
        r.control = PC_PAINT_CONTROL_PEN_PRESSURE;
        break;
    case 3:
        r.control = PC_PAINT_CONTROL_PEN_TILT;
        break;
    case 4:
        r.control = PC_PAINT_CONTROL_STYLUS_WHEEL;
        break;
    case 5:
        r.control = PC_PAINT_CONTROL_INITIAL_DIRECTION;
        break;
    case 6:
        r.control = PC_PAINT_CONTROL_DIRECTION;
        break;
    case 7:
        warn(m, "\"Initial Rotation\" control mapped to Rotation");
        r.control = PC_PAINT_CONTROL_ROTATION;
        break;
    case 8:
        r.control = PC_PAINT_CONTROL_ROTATION;
        break;
    default:
        r.control = PC_PAINT_CONTROL_OFF;
        break;
    }
    f = 0.0f;
    (void)pct(d, "jitter", &f);
    r.jitter = pc_rs_maxf_k(f, 0.0f);
    r.fade_steps = num(d, "fStp", &v) ? pc_f64_to_u32(pc_clampd(v, 1.0, 9999.0)) : 25;
    r.minimum = pc_clampf(pct_or(d, "Mnm ", 0.0f), 0.0f, 1.0f);
    return r;
}

static bool str_eq_psd(const pc_str *a, const pc_psd_str *b)
{
    return a->len == b->len && (a->len == 0 || memcmp(a->ptr, b->s, a->len) == 0);
}

/* Tip-shape fields from a computedBrush / sampledBrush object; false when the sampled tip it
 * references is missing. *has_size false for computed tips. */
static bool tip_shape(mapper *m, const pc_psd_desc *t, pc_paint_tip *out, bool *has_size, float *size)
{
    bool has_id = false;
    pc_str id = PC_STR_INIT;
    const pc_psd_abr_sample *s = NULL;
    const pc_psd_abr *f = m->file;
    bool sampled = pc_psd_id_is(&t->class_id, "sampledBrush") || pc_psd_desc_get(t, "sampledData") != NULL;
    *has_size = false;
    if (!sampled) {
        pc_paint_tip_default(out);
        return true;
    }
    if (m->st == PC_OK) {
        m->st = text(t, "sampledData", &has_id, &id, m->err);
    }
    for (size_t i = 0; i < f->nsamples && s == NULL; i++) {
        if (str_eq_psd(&id, &f->samples[i].id)) {
            s = &f->samples[i];
        }
    }
    pc_str_fini(&id);
    if (s == NULL && f->nsamples == 1) {
        s = &f->samples[0];
    }
    if (s == NULL) {
        return false;
    }
    tip(m, s, out, size);
    *has_size = true;
    return true;
}

/* A pattern as a grayscale texture tile (luminance; transparent areas count as white); false
 * when it can't be read. */
static bool plane_of(const pc_psd_pattern *p, size_t c, size_t n, float **out, pc_status *st, pc_error *err)
{
    const pc_psd_bytes *ch;
    size_t have;
    float *v;
    if (c >= p->nchannels) {
        return false;
    }
    ch = &p->channels[c];
    switch (p->depth) {
    case 8:
        have = ch->len;
        break;
    case 16:
        have = ch->len / 2;
        break;
    case 32:
        have = ch->len / 4;
        break;
    default:
        return false;
    }
    if (have < n) {
        return false;
    }
    v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        *st = pc_io_nomem(err, "importing brush textures");
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const uint8_t *b = ch->data;
        if (p->depth == 8) {
            v[i] = (float)b[i] / 255.0f;
        } else if (p->depth == 16) {
            v[i] = (float)((unsigned)b[2 * i] << 8 | b[2 * i + 1]) / 65535.0f;
        } else {
            uint32_t u = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 |
                         (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
            float x;
            memcpy(&x, &u, sizeof x);
            v[i] = pc_clampf(x, 0.0f, 1.0f);
        }
    }
    *out = v;
    return true;
}

static bool pattern_tile(const pc_psd_pattern *p, pc_paint_gray_tile *out, pc_status *st, pc_error *err)
{
    uint32_t w = p->width;
    uint32_t h = p->height;
    size_t n = (size_t)w * h;
    float *gray = NULL;
    float *a = NULL;
    float *b = NULL;
    float *c = NULL;
    bool ok = false;
    if (n == 0 || (uint64_t)w * h > SIZE_MAX / sizeof(float)) {
        return false;
    }
    if (p->mode == 3) {
        if (plane_of(p, 0, n, &a, st, err) && plane_of(p, 1, n, &b, st, err) &&
            plane_of(p, 2, n, &c, st, err)) {
            gray = a;
            for (size_t i = 0; i < n; i++) {
                gray[i] = 0.299f * a[i] + 0.587f * b[i] + 0.114f * c[i];
            }
            a = NULL;
        }
    } else if (p->mode == 2) {
        const pc_psd_bytes *idx = p->nchannels > 0 ? &p->channels[0] : NULL;
        if (p->palette.has && idx != NULL) {
            gray = pc_malloc_array(n, sizeof *gray);
            if (gray == NULL) {
                *st = pc_io_nomem(err, "importing brush textures");
            }
            for (size_t i = 0; gray != NULL && i < n; i++) {
                size_t k = (size_t)(i < idx->len ? idx->data[i] : 0) * 3;
                const pc_psd_bytes *pal = &p->palette.v;
                float r = (float)(k < pal->len ? pal->data[k] : 0) / 255.0f;
                float g = (float)(k + 1 < pal->len ? pal->data[k + 1] : 0) / 255.0f;
                float bl = (float)(k + 2 < pal->len ? pal->data[k + 2] : 0) / 255.0f;
                gray[i] = 0.299f * r + 0.587f * g + 0.114f * bl;
            }
        }
    } else if (p->mode == 4) {
        /* CMYK planes are stored inverted (0 = full ink). */
        if (plane_of(p, 3, n, &a, st, err) && plane_of(p, 0, n, &b, st, err)) {
            gray = b;
            for (size_t i = 0; i < n; i++) {
                gray[i] = pc_clampf(b[i] * a[i], 0.0f, 1.0f);
            }
            b = NULL;
        }
    } else {
        (void)plane_of(p, 0, n, &gray, st, err);
    }
    free(a);
    free(b);
    free(c);
    if (gray != NULL && *st == PC_OK) {
        *st = fit(&w, &h, &gray, &n, PC_IO_ABR_MAX_PATTERN_EDGE, err);
        if (*st == PC_OK) {
            *st = gray_tile(w, h, gray, n, out, err);
            ok = *st == PC_OK;
        }
    }
    free(gray);
    return ok;
}

static void pattern(mapper *m, const pc_psd_desc *t, pc_paint_pattern *out)
{
    pc_str id = PC_STR_INIT;
    pc_str name = PC_STR_INIT;
    bool has = false;
    const pc_psd_pattern *found = NULL;
    const pc_psd_abr *f = m->file;
    pc_paint_gray_tile tile;
    pc_paint_pattern_default(out);
    if (m->st == PC_OK) {
        m->st = text(t, "Idnt", &has, &id, m->err);
    }
    if (m->st == PC_OK) {
        m->st = text(t, "Nm  ", &has, &name, m->err);
    }
    for (size_t i = 0; i < f->npatterns && found == NULL && id.len > 0; i++) {
        if (str_eq_psd(&id, &f->patterns[i].id)) {
            found = &f->patterns[i];
        }
    }
    for (size_t i = 0; i < f->npatterns && found == NULL && name.len > 0; i++) {
        if (str_eq_psd(&name, &f->patterns[i].name)) {
            found = &f->patterns[i];
        }
    }
    if (m->st == PC_OK && found != NULL && pattern_tile(found, &tile, &m->st, m->err)) {
        out->kind = PC_PAINT_PATTERN_TILE;
        out->tile = tile;
    } else if (m->st == PC_OK) {
        warn_quoted(m, "texture pattern \"", name.len > 0 ? &name : &id,
                    "\" is not embedded in the file; a procedural paper texture stands in");
        out->kind = PC_PAINT_PATTERN_PROCEDURAL;
        out->style = PC_PAINT_STYLE_PAPER;
        out->size = 128;
        out->seed = 1;
    }
    pc_str_fini(&id);
    pc_str_fini(&name);
}

/* The first of the keys holding a percentage. */
static bool first_pct(const pc_psd_desc *d, const char *const *keys, size_t n, float *out)
{
    for (size_t i = 0; i < n; i++) {
        if (pct(d, keys[i], out)) {
            return true;
        }
    }
    return false;
}

/* Mixer Brush tool options captured with a preset (Wet, Load, Mix, Flow, Sample All Layers). Key
 * names are best effort (Photoshop doesn't document them); several spellings are read. */
static void mixer_options(const pc_psd_desc *to, pc_paint_mixer_settings *mx)
{
    static const char *const wk[3] = {"wetness", "Wtns", "wet"};
    static const char *const lk[3] = {"dryness", "load", "Ld  "};
    static const char *const mk[3] = {"mix", "mixRatio", "Mx  "};
    static const char *const sk[4] = {"sampleMerged", "useAllLayers", "sampleAllLayers", "Mrgd"};
    float wet = mx->wet;
    float load = mx->load;
    float mix = mx->mix;
    bool hw = first_pct(to, wk, 3, &wet);
    bool hl = first_pct(to, lk, 3, &load);
    bool hm = first_pct(to, mk, 3, &mix);
    if (!hw && !hl && !hm) {
        return;
    }
    mx->wet = pc_clampf(wet, 0.0f, 1.0f);
    mx->load = pc_clampf(load, 0.0f, 1.0f);
    mx->mix = pc_clampf(mix, 0.0f, 1.0f);
    mx->flow = pc_clampf(pct_or(to, "flow", mx->flow), 0.0f, 1.0f);
    for (size_t i = 0; i < 4; i++) {
        bool v;
        if (boolean(to, sk[i], &v)) {
            mx->sample_all_layers = v;
            break;
        }
    }
}

static void shape_dynamics(mapper *m, const pc_psd_desc *d, pc_paint_brush *b)
{
    pc_paint_shape_dynamics *sd = &b->shape_dynamics;
    sd->enabled = true;
    sd->size = dynamic(m, pc_io4_obj(d, "szVr"));
    sd->size.minimum = pc_clampf(pct_or(d, "minimumDiameter", sd->size.minimum), 0.0f, 1.0f);
    sd->roundness = dynamic(m, pc_io4_obj(d, "roundnessDynamics"));
    sd->roundness.minimum = pc_clampf(pct_or(d, "minimumRoundness", sd->roundness.minimum), 0.0f, 1.0f);
    sd->angle = dynamic(m, pc_io4_obj(d, "angleDynamics"));
    sd->flip_x_jitter = bool_or(d, "flipX", false);
    sd->flip_y_jitter = bool_or(d, "flipY", false);
    sd->tilt_scale = pc_clampf(pct_or(d, "tiltScale", 0.0f), 0.0f, 2.0f);
    sd->brush_projection = bool_or(d, "brushProjection", false);
}

static uint32_t count_of(const pc_psd_desc *d)
{
    double v;
    return num(d, "Cnt ", &v) ? pc_f64_to_u32(pc_clampd(v, 1.0, 16.0)) : 1;
}

static void texture(mapper *m, const pc_psd_desc *d, pc_paint_brush *b)
{
    pc_paint_texture *t = &b->texture;
    double v = 0.0;
    size_t len = 0;
    const uint8_t *code;
    const pc_psd_desc *p;
    t->enabled = true;
    t->each_tip = bool_or(d, "TxtC", false);
    t->invert = bool_or(d, "InvT", false);
    t->scale = pc_clampf(pct_or(d, "textureScale", 1.0f), 0.01f, 10.0f);
    v = 0.0;
    (void)num(d, "textureBrightness", &v);
    t->brightness = pc_f64_to_f32(pc_clampd(v / 150.0, -1.0, 1.0));
    v = 0.0;
    (void)num(d, "textureContrast", &v);
    t->contrast = pc_f64_to_f32(pc_clampd(v / 50.0, -1.0, 2.0));
    code = enum_value(d, "textureBlendMode", &len);
    t->mode = mask_mode(code, len, PC_PAINT_MASK_MULTIPLY);
    t->depth = pc_clampf(pct_or(d, "textureDepth", 1.0f), 0.0f, 1.0f);
    t->depth_jitter = dynamic(m, pc_io4_obj(d, "textureDepthDynamics"));
    t->depth_jitter.minimum = pc_clampf(pct_or(d, "minimumDepth", t->depth_jitter.minimum), 0.0f, 1.0f);
    p = pc_io4_obj(d, "Txtr");
    if (p != NULL) {
        pc_paint_pattern pat;
        pattern(m, p, &pat);
        pc_paint_pattern_fini(&t->pattern);
        t->pattern = pat;
    }
}

static void dual_brush(mapper *m, const pc_psd_desc *db, pc_paint_brush *b)
{
    pc_paint_dual_brush dual;
    const pc_psd_desc *t = pc_io4_obj(db, "Brsh");
    size_t len = 0;
    const uint8_t *code;
    double v;
    float f;
    pc_paint_dual_brush_default(&dual);
    dual.enabled = true;
    if (t != NULL) {
        pc_paint_tip tp;
        bool has_size = false;
        float size = 0.0f;
        if (tip_shape(m, t, &tp, &has_size, &size)) {
            pc_paint_tip_fini(&dual.tip);
            dual.tip = tp;
            if (num(t, "Dmtr", &v)) {
                dual.size = pc_f64_to_f32(v);
            } else if (has_size) {
                dual.size = size;
            }
            dual.size = pc_clampf(dual.size, 1.0f, 5000.0f);
        } else {
            warn(m, "a dual brush tip is missing from the file; a round tip stands in");
        }
        dual.hardness = pc_clampf(pct_or(t, "Hrdn", 1.0f), 0.0f, 1.0f);
        dual.roundness = pc_clampf(pct_or(t, "Rndn", 1.0f), 0.01f, 1.0f);
        v = 0.0;
        (void)num(t, "Angl", &v);
        dual.angle = pc_f64_to_f32(v);
        dual.spacing = pc_clampf(pct_or(t, "Spcn", dual.spacing), 0.01f, 10.0f);
    }
    code = enum_value(db, "BlnM", &len);
    dual.mode = mask_mode(code, len, PC_PAINT_MASK_MULTIPLY);
    dual.flip = bool_or(db, "Flip", false);
    if (pct(db, "Spcn", &f)) {
        dual.spacing = pc_clampf(f, 0.01f, 10.0f);
    }
    dual.scatter = dynamic(m, pc_io4_obj(db, "scatterDynamics")).jitter;
    dual.both_axes = bool_or(db, "bothAxes", false);
    dual.count = count_of(db);
    pc_paint_dual_brush_assign(&b->dual_brush, &dual);
    pc_paint_dual_brush_fini(&dual);
}

static void tool_options(mapper *m, const pc_psd_desc *to, pc_paint_brush *b)
{
    float f;
    bool v;
    size_t len = 0;
    const uint8_t *md;
    if (pct(to, "Opct", &f)) {
        b->opacity = pc_clampf(f, 0.0f, 1.0f);
    }
    if (pct(to, "flow", &f)) {
        b->flow = pc_clampf(f, 0.0f, 1.0f);
    }
    b->pressure_size = bool_or(to, "usePressureOverridesSize", false);
    b->pressure_opacity = bool_or(to, "usePressureOverridesOpacity", false);
    if (bool_or(to, "smoothing", true) && pct(to, "smoothingValue", &f)) {
        b->smoothing.amount = pc_clampf(f, 0.0f, 1.0f);
    }
    if (boolean(to, "smoothingRadiusMode", &v)) {
        b->smoothing.pulled_string = v;
    }
    if (boolean(to, "smoothingCatchup", &v)) {
        b->smoothing.catch_up = v;
    }
    if (boolean(to, "smoothingCatchupAtEnd", &v)) {
        b->smoothing.catch_up_on_end = v;
    }
    if (boolean(to, "smoothingZoomCompensation", &v)) {
        b->smoothing.adjust_for_zoom = v;
    }
    mixer_options(to, &b->mixer);
    md = enum_value(to, "Md  ", &len);
    if (md != NULL && !bytes_is(md, len, "Nrml")) {
        warn(m, "tool blend modes stored with presets are not applied");
    }
}

static double rem_euclid(double a, double b)
{
    double r = fmod(a, b);
    return r < 0.0 ? r + fabs(b) : r;
}

/* A brushPreset descriptor as a brush; false when its sampled tip is missing. */
static bool preset(mapper *m, const pc_psd_desc *d, pc_paint_brush *out)
{
    pc_paint_brush b;
    const pc_psd_desc *t;
    const pc_psd_desc *db;
    const pc_psd_desc *to;
    bool on;
    double v;
    float f;
    for (size_t i = 0; i < d->n && m->st == PC_OK; i++) {
        size_t len = 0;
        const uint8_t *k = pc_psd_id_bytes(&d->items[i].key, &len);
        if (!known(k, len)) {
            pc_buf lossy = PC_BUF_INIT;
            m->st = pc_utf8_lossy(k, len, &lossy, m->err);
            if (m->st == PC_OK) {
                m->st = sset_insert(&m->unknown, (const char *)lossy.data, lossy.len, m->err);
            }
            pc_buf_fini(&lossy);
        }
    }
    pc_paint_brush_default(&b);
    b.pressure_size = false;
    b.spacing = 0.25f;
    t = pc_io4_obj(d, "Brsh");
    if (t != NULL) {
        bool has_size = false;
        float size = 0.0f;
        pc_paint_tip tp;
        if (!tip_shape(m, t, &tp, &has_size, &size)) {
            pc_paint_brush_fini(&b);
            return false;
        }
        pc_paint_tip_fini(&b.tip);
        b.tip = tp;
        if (num(t, "Dmtr", &v)) {
            b.size = pc_f64_to_f32(v);
        } else if (has_size) {
            b.size = size;
        }
        b.size = pc_clampf(b.size, 1.0f, 5000.0f);
        if (pct(t, "Hrdn", &f)) {
            b.hardness = pc_clampf(f, 0.0f, 1.0f);
        }
        v = 0.0;
        (void)num(t, "Angl", &v);
        b.angle = pc_f64_to_f32(v);
        b.roundness = pc_clampf(pct_or(t, "Rndn", 1.0f), 0.01f, 1.0f);
        /* Intr off = the Spacing checkbox is off: the pointer's speed sets the spacing. */
        b.spacing = pc_clampf(pct_or(t, "Spcn", 0.25f), 0.01f, 10.0f);
        b.spacing_enabled = !(boolean(t, "Intr", &on) && !on);
        b.flip_x = bool_or(t, "flipX", false);
        b.flip_y = bool_or(t, "flipY", false);
    }
    if (boolean(d, "useTipDynamics", &on) && on) {
        shape_dynamics(m, d, &b);
    }
    if (boolean(d, "useScatter", &on) && on) {
        b.scattering.enabled = true;
        b.scattering.scatter = dynamic(m, pc_io4_obj(d, "scatterDynamics"));
        b.scattering.both_axes = bool_or(d, "bothAxes", false);
        b.scattering.count = count_of(d);
        b.scattering.count_jitter = dynamic(m, pc_io4_obj(d, "countDynamics"));
    }
    if (boolean(d, "useTexture", &on) && on) {
        texture(m, d, &b);
    }
    b.protect_texture = bool_or(d, "protectTexture", false);
    db = pc_io4_obj(d, "dualBrush");
    if (db != NULL && boolean(db, "useDualBrush", &on) && on) {
        dual_brush(m, db, &b);
    }
    if (boolean(d, "useColorDynamics", &on) && on) {
        pc_paint_color_dynamics *c = &b.color_dynamics;
        c->enabled = true;
        c->per_tip = bool_or(d, "colorDynamicsPerTip", true);
        c->hue_jitter = pc_clampf(pct_or(d, "H   ", 0.0f), 0.0f, 1.0f);
        c->saturation_jitter = pc_clampf(pct_or(d, "Strt", 0.0f), 0.0f, 1.0f);
        c->brightness_jitter = pc_clampf(pct_or(d, "Brgh", 0.0f), 0.0f, 1.0f);
        c->purity = pc_clampf(pct_or(d, "purity", 0.0f), -1.0f, 1.0f);
        c->fg_bg = dynamic(m, pc_io4_obj(d, "clVr"));
    }
    if (boolean(d, "usePaintDynamics", &on) && on) {
        b.transfer.enabled = true;
        b.transfer.opacity = dynamic(m, pc_io4_obj(d, "opVr"));
        b.transfer.flow = dynamic(m, pc_io4_obj(d, "prVr"));
        b.transfer.wetness = dynamic(m, pc_io4_obj(d, "wtVr"));
        b.transfer.mix = dynamic(m, pc_io4_obj(d, "mxVr"));
    }
    /* Brush Pose (Photoshop's tilt is +-100 %; ours is +-90 degrees). */
    if (boolean(d, "useBrushPose", &on) && on) {
        pc_paint_pose *p = &b.pose;
        p->enabled = true;
        v = 0.0;
        (void)num(d, "brushPoseTiltX", &v);
        p->tilt_x = pc_f64_to_f32(pc_clampd(v * 0.9, -90.0, 90.0));
        v = 0.0;
        (void)num(d, "brushPoseTiltY", &v);
        p->tilt_y = pc_f64_to_f32(pc_clampd(v * 0.9, -90.0, 90.0));
        v = 0.0;
        (void)num(d, "brushPoseAngle", &v);
        p->rotation = pc_f64_to_f32(rem_euclid(v, 360.0));
        p->pressure = pc_clampf(pct_or(d, "brushPosePressure", 1.0f), 0.0f, 1.0f);
        p->override_tilt = bool_or(d, "overridePoseTiltX", false) || bool_or(d, "overridePoseTiltY", false);
        p->override_rotation = bool_or(d, "overridePoseAngle", false);
        p->override_pressure = bool_or(d, "overridePosePressure", false);
    }
    b.noise = bool_or(d, "Nose", false);
    b.wet_edges = bool_or(d, "Wtdg", false);
    b.build_up = bool_or(d, "Rpt ", false);
    /* Tool options captured with the preset ("Include tool settings"). */
    to = pc_io4_obj(d, "toolOptions");
    if (to != NULL) {
        tool_options(m, to, &b);
    }
    *out = b;
    return true;
}

/* ---- files ---- */

void pc_io_abr_import_fini(pc_io_abr_import *i)
{
    if (i == NULL) {
        return;
    }
    pc_paint_preset_list_fini(&i->presets);
    pc_io_warnings_fini(&i->warnings);
    memset(i, 0, sizeof *i);
}

typedef struct plist {
    pc_paint_preset_list *l;
    size_t cap;
} plist;

/* Appends a preset taking over *brush (released on failure). */
static pc_status push_preset(plist *pl, const char *name, size_t nlen, pc_paint_brush *brush,
                             const char *group, pc_error *err)
{
    pc_paint_preset p;
    pc_paint_preset *g;
    pc_status st;
    pc_paint_preset_init(&p);
    pc_paint_brush_fini(&p.brush);
    p.brush = *brush;
    st = pc_str_set(&p.name, name, nlen, err);
    if (st == PC_OK) {
        st = pc_str_set_c(&p.group, group, err);
    }
    if (st != PC_OK) {
        pc_paint_preset_fini(&p);
        return st;
    }
    g = pc_grow_array(pl->l->v, &pl->cap, pl->l->len + 1, sizeof *g);
    if (g == NULL) {
        pc_paint_preset_fini(&p);
        return pc_io_nomem(err, "importing brushes");
    }
    pl->l->v = g;
    g[pl->l->len++] = p;
    return PC_OK;
}

static pc_status step(const pc_interrupt *ctl, size_t i, float total, pc_error *err)
{
    pc_status st = pc_interrupt_check(ctl, err);
    if (st == PC_OK) {
        pc_interrupt_progress(ctl, (float)i / total);
    }
    return st;
}

/* The trimmed name, or NULL when it is empty after trimming. */
static const char *trimmed(const char *s, size_t len, size_t *out_len)
{
    size_t start = 0;
    size_t n = pc_utf8_trim(s, len, &start);
    *out_len = n;
    return n > 0 ? s + start : NULL;
}

static pc_status map_legacy(mapper *m, const pc_psd_abr *f, const char *group, const pc_interrupt *ctl,
                            float total, plist *pl, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < f->nlegacy && st == PC_OK; i++) {
        const pc_psd_abr_legacy *b = &f->legacy[i];
        pc_paint_brush br;
        char dflt[64];
        const char *name;
        size_t nlen = 0;
        st = step(ctl, i, total, err);
        if (st != PC_OK) {
            break;
        }
        pc_paint_brush_default(&br);
        br.pressure_size = false;
        br.spacing = b->spacing == 0 ? 0.25f : (float)b->spacing / 100.0f;
        if (b->tip == PC_PSD_ABR_COMPUTED) {
            br.size = (float)(b->diameter > 1 ? b->diameter : 1);
            br.hardness = pc_clampf((float)b->hardness / 100.0f, 0.0f, 1.0f);
            br.angle = (float)b->angle;
            br.roundness = pc_clampf((float)b->roundness / 100.0f, 0.01f, 1.0f);
            (void)snprintf(dflt, sizeof dflt, "%u px Round", (unsigned)b->diameter);
        } else {
            pc_paint_tip tp;
            float size = 0.0f;
            tip(m, &b->sample, &tp, &size);
            pc_paint_tip_fini(&br.tip);
            br.tip = tp;
            br.size = size;
            br.aliased = !b->anti_alias;
            (void)snprintf(dflt, sizeof dflt, "Sampled Brush %zu", i + 1);
        }
        if (m->st != PC_OK) {
            pc_paint_brush_fini(&br);
            st = m->st;
            break;
        }
        name = trimmed(pc_psd_str_c(&b->name), b->name.len, &nlen);
        if (name == NULL) {
            name = dflt;
            nlen = strlen(dflt);
        }
        st = push_preset(pl, name, nlen, &br, group, err);
    }
    return st;
}

static pc_status map_samples(mapper *m, const pc_psd_abr *f, const char *group, const pc_interrupt *ctl,
                             float total, plist *pl, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < f->nsamples && st == PC_OK; i++) {
        pc_paint_brush br;
        pc_paint_tip tp;
        float size = 0.0f;
        char name[64];
        st = step(ctl, i, total, err);
        if (st != PC_OK) {
            break;
        }
        tip(m, &f->samples[i], &tp, &size);
        if (m->st != PC_OK) {
            st = m->st;
            break;
        }
        pc_paint_brush_default(&br);
        br.pressure_size = false;
        br.spacing = 0.25f;
        br.size = size;
        pc_paint_tip_fini(&br.tip);
        br.tip = tp;
        (void)snprintf(name, sizeof name, "Sampled Brush %zu", i + 1);
        st = push_preset(pl, name, strlen(name), &br, group, err);
    }
    return st;
}

static pc_status map_presets(mapper *m, const pc_psd_abr *f, const char *group, const pc_interrupt *ctl,
                             float total, plist *pl, pc_error *err)
{
    pc_status st = PC_OK;
    for (size_t i = 0; i < f->npresets && st == PC_OK; i++) {
        const pc_psd_desc *d = &f->presets[i];
        pc_str nm = PC_STR_INIT;
        bool has = false;
        char dflt[64];
        const char *name = NULL;
        size_t nlen = 0;
        pc_paint_brush br;
        bool ok;
        st = step(ctl, i, total, err);
        if (st == PC_OK) {
            st = text(d, "Nm  ", &has, &nm, err);
        }
        if (st != PC_OK) {
            pc_str_fini(&nm);
            break;
        }
        if (has) {
            name = trimmed(pc_str_c(&nm), nm.len, &nlen);
        }
        if (name == NULL) {
            (void)snprintf(dflt, sizeof dflt, "Brush %zu", i + 1);
            name = dflt;
            nlen = strlen(dflt);
        }
        ok = preset(m, d, &br);
        if (m->st != PC_OK) {
            if (ok) {
                pc_paint_brush_fini(&br);
            }
            st = m->st;
        } else if (ok) {
            st = push_preset(pl, name, nlen, &br, group, err);
        } else {
            pc_str q = {(char *)(uintptr_t)name, nlen};
            warn_quoted(m, "\"", &q, "\": its sampled tip is missing from the file; skipped");
            st = m->st;
        }
        pc_str_fini(&nm);
    }
    return st;
}

pc_status pc_io_abr_map_file_with(const pc_psd_abr *f, const char *group, const pc_interrupt *ctl,
                                  pc_io_abr_import *out, pc_error *err)
{
    pc_io_abr_import r;
    plist pl;
    mapper m;
    size_t most = f->nlegacy;
    float total;
    pc_status st;
    memset(&r, 0, sizeof r);
    memset(&m, 0, sizeof m);
    r.version = f->version;
    m.file = f;
    m.err = err;
    pl.l = &r.presets;
    pl.cap = 0;
    if (group == NULL) {
        group = "";
    }
    most = f->nsamples > most ? f->nsamples : most;
    most = f->npresets > most ? f->npresets : most;
    total = (float)(most > 1 ? most : 1);
    st = pc_io_warnings_extend(&r.warnings, "", f->warnings.v, f->warnings.n, err);
    if (st == PC_OK) {
        if (f->nlegacy > 0) {
            st = map_legacy(&m, f, group, ctl, total, &pl, err);
        } else if (f->npresets == 0) {
            st = map_samples(&m, f, group, ctl, total, &pl, err);
        } else {
            st = map_presets(&m, f, group, ctl, total, &pl, err);
        }
    }
    for (size_t i = 0; i < m.warnings.n && st == PC_OK; i++) {
        st = pc_io_warn(&r.warnings, err, "%s", pc_str_c(&m.warnings.v[i]));
    }
    if (st == PC_OK && m.unknown.n > 0) {
        pc_buf b = PC_BUF_INIT;
        st = pc_buf_append_str(&b, "settings without a PhotoCraft equivalent were ignored: ", err);
        for (size_t i = 0; i < m.unknown.n && st == PC_OK; i++) {
            if (i > 0) {
                st = pc_buf_append_str(&b, ", ", err);
            }
            if (st == PC_OK) {
                st = pc_buf_append(&b, m.unknown.v[i].ptr, m.unknown.v[i].len, err);
            }
        }
        if (st == PC_OK) {
            st = pc_io_warn(&r.warnings, err, "%s", pc_buf_cstr(&b));
        }
        pc_buf_fini(&b);
    }
    sset_fini(&m.warnings);
    sset_fini(&m.unknown);
    if (st != PC_OK) {
        pc_io_abr_import_fini(&r);
        return st;
    }
    pc_interrupt_progress(ctl, 1.0f);
    *out = r;
    return PC_OK;
}

pc_status pc_io_abr_map_file(const pc_psd_abr *f, const char *group, pc_io_abr_import *out, pc_error *err)
{
    return pc_io_abr_map_file_with(f, group, NULL, out, err);
}

/* The mapping stage's view of the caller's interrupt: same cancel, progress scaled to 10-100 %. */
static bool outer_cancelled(void *ctx)
{
    return pc_interrupt_cancelled((const pc_interrupt *)ctx);
}

static void outer_progress(void *ctx, float p)
{
    pc_interrupt_progress((const pc_interrupt *)ctx, 0.1f + 0.9f * p);
}

pc_status pc_io_read_abr_with(const uint8_t *bytes, size_t len, const char *group, const pc_interrupt *ctl,
                              pc_io_abr_import *out, pc_error *err)
{
    pc_psd_abr f;
    pc_interrupt inner;
    pc_status st = pc_psd_abr_parse(bytes != NULL ? bytes : (const uint8_t *)"", len, &f, err);
    if (st != PC_OK) {
        if (st != PC_ERR_NOMEM && err != NULL) {
            char msg[sizeof err->msg];
            memcpy(msg, err->msg, sizeof msg);
            msg[sizeof msg - 1] = '\0';
            (void)pc_fail(err, st, "not a readable Photoshop brush file: %s", msg);
        }
        return st;
    }
    /* Parsing is the small part; mapping (tip decoding) reports 10-100 %. */
    pc_interrupt_progress(ctl, 0.1f);
    inner.cancel = outer_cancelled;
    inner.progress = outer_progress;
    inner.ctx = (void *)(uintptr_t)ctl;
    st = pc_io_abr_map_file_with(&f, group, ctl != NULL ? &inner : NULL, out, err);
    pc_psd_abr_fini(&f);
    return st;
}

pc_status pc_io_read_abr(const uint8_t *bytes, size_t len, const char *group, pc_io_abr_import *out,
                         pc_error *err)
{
    return pc_io_read_abr_with(bytes, len, group, NULL, out, err);
}
