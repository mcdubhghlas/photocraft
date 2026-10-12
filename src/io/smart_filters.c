/* Smart filters <-> filterFX list items (port of the filter half of crates/io/src/smart_map.rs),
 * plus I3's small descriptor helpers (smart_internal.h).
 *
 * Filters PhotoCraft implements map to their command and dialog parameters (the KNOWN table,
 * keyed by the Fltr class and filterID); Camera Raw goes through camera_raw_map (I5); anything
 * else becomes PC_IO_UNSUPPORTED_FILTER holding its descriptor as hex, with neutral blending so
 * hiding or re-blending the filter leaves its params alone. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "io/camera_raw_map.h"
#include "io/effects_map.h"
#include "io/smart_internal.h"
#include "io/smart_map.h"

/* ---- descriptor helpers ---- */

bool pc_io3_num(const pc_psd_value *v, double *out)
{
    if (v == NULL) {
        return false;
    }
    switch (v->kind) {
    case PC_PSD_V_DOUBLE:
    case PC_PSD_V_UNIT_FLOAT:
        *out = v->d;
        return true;
    case PC_PSD_V_INTEGER:
        *out = (double)v->i;
        return true;
    default:
        return false;
    }
}

bool pc_io3_num_of(const pc_psd_desc *d, const char *key, double *out)
{
    return d != NULL && pc_io3_num(pc_psd_desc_get(d, key), out);
}

const pc_psd_desc *pc_io3_get_desc(const pc_psd_desc *d, const char *key)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v != NULL && (v->kind == PC_PSD_V_DESCRIPTOR || v->kind == PC_PSD_V_GLOBAL_OBJECT)) {
        return v->desc;
    }
    return NULL;
}

const uint8_t *pc_io3_enum_of(const pc_psd_desc *d, const char *key, size_t *len)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v != NULL && v->kind == PC_PSD_V_ENUMERATED) {
        return pc_psd_id_bytes(&v->value, len);
    }
    return NULL;
}

pc_status pc_io3_text_of(const pc_psd_desc *d, const char *key, bool *found, char **out, pc_error *err)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v == NULL || v->kind != PC_PSD_V_TEXT) {
        *found = false;
        return PC_OK;
    }
    {
        pc_status st = pc_psd_ustr_to_utf8(&v->text, out, err);
        if (st == PC_OK) {
            *found = true;
        }
        return st;
    }
}

pc_psd_value pc_io3_unit(const char unit[4], double v)
{
    pc_psd_value x = pc_psd_value_double(v);
    x.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(x.unit, unit, 4);
    return x;
}

pc_status pc_io3_put(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_status st, pc_error *err)
{
    if (st != PC_OK) {
        pc_psd_value_fini(v);
        return st;
    }
    return pc_psd_desc_add(d, key, v, err);
}

pc_status pc_io3_put_obj(pc_psd_desc *d, const char *key, pc_psd_desc *sub, pc_status st, pc_error *err)
{
    pc_psd_value v;
    if (st != PC_OK) {
        pc_psd_desc_fini(sub);
        return st;
    }
    st = pc_psd_value_object(sub, &v, err);
    if (st != PC_OK) {
        return st;
    }
    return pc_psd_desc_add(d, key, &v, err);
}

pc_status pc_io3_set(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_error *err)
{
    for (size_t i = 0; i < d->n; i++) {
        if (pc_psd_id_is(&d->items[i].key, key)) {
            pc_psd_value_fini(&d->items[i].value);
            d->items[i].value = *v;
            memset(v, 0, sizeof *v);
            return PC_OK;
        }
    }
    return pc_psd_desc_add(d, key, v, err);
}

void pc_io3_remove(pc_psd_desc *d, const char *key)
{
    size_t k = 0;
    for (size_t i = 0; i < d->n; i++) {
        if (pc_psd_id_is(&d->items[i].key, key)) {
            pc_psd_id_fini(&d->items[i].key);
            pc_psd_value_fini(&d->items[i].value);
        } else {
            d->items[k++] = d->items[i];
        }
    }
    d->n = k;
}

pc_status pc_io3_to_hex(const uint8_t *b, size_t n, char **out, pc_error *err)
{
    static const char H[] = "0123456789abcdef";
    size_t len;
    char *s;
    if (!pc_size_mul(n, 2, &len) || len >= PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "limit exceeded: %zu bytes as hex", n);
    }
    s = pc_malloc(len + 1);
    if (s == NULL) {
        return pc_io_nomem(err, "writing hex");
    }
    for (size_t i = 0; i < n; i++) {
        s[2 * i] = H[b[i] >> 4];
        s[2 * i + 1] = H[b[i] & 15];
    }
    s[len] = '\0';
    *out = s;
    return PC_OK;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

pc_status pc_io3_from_hex(const char *s, size_t n, bool *ok, uint8_t **out, size_t *len, pc_error *err)
{
    uint8_t *b;
    *ok = false;
    if (n % 2 != 0) {
        return PC_OK;
    }
    for (size_t i = 0; i < n; i++) {
        if (hex_digit(s[i]) < 0) {
            return PC_OK;
        }
    }
    b = pc_malloc(n / 2 > 0 ? n / 2 : 1);
    if (b == NULL) {
        return pc_io_nomem(err, "reading hex");
    }
    for (size_t i = 0; i < n / 2; i++) {
        b[i] = (uint8_t)(hex_digit(s[2 * i]) << 4 | hex_digit(s[2 * i + 1]));
    }
    *out = b;
    *len = n / 2;
    *ok = true;
    return PC_OK;
}

/* ---- the filters we model ---- */

/* Photoshop filter -> PhotoCraft command, keyed by the Fltr class (and filterID). name is Nm as
 * Photoshop writes it (the menu item, with its ellipsis and mnemonic). */
typedef struct known {
    const char *cls;
    int32_t filter_id;
    const char *name;
    const char *command;
} known;

#define CODE(a, b, c, d)                                                                                     \
    ((int32_t)((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d)))

static const known KNOWN[14] = {
    {"GsnB", CODE('G', 's', 'n', 'B'), "Gaussian Blur...", "filter.blur.gaussianBlur"},
    {"boxblur", 840, "Box Blur...", "filter.blur.boxBlur"},
    {"MtnB", CODE('M', 't', 'n', 'B'), "Motion Blur...", "filter.blur.motionBlur"},
    {"UnsM", CODE('U', 'n', 's', 'M'), "Unsharp Mask...", "filter.sharpen.unsharpMask"},
    {"AdNs", CODE('A', 'd', 'N', 's'), "Add Noise...", "filter.noise.addNoise"},
    {"Mdn ", CODE('M', 'd', 'n', ' '), "Median...", "filter.noise.median"},
    {"HghP", CODE('H', 'g', 'h', 'P'), "High Pass...", "filter.other.highPass"},
    {"Mxm ", CODE('M', 'x', 'm', ' '), "Maximum...", "filter.other.maximum"},
    {"Mnm ", CODE('M', 'n', 'm', ' '), "Minimum...", "filter.other.minimum"},
    {"Msc ", CODE('M', 's', 'c', ' '), "Mosaic...", "filter.pixelate.mosaic"},
    {"Embs", CODE('E', 'm', 'b', 's'), "Emboss...", "filter.stylize.emboss"},
    {"Lvls", CODE('L', 'v', 'l', 's'), "Levels...", "image.adjustments.levels"},
    {"Crvs", CODE('C', 'r', 'v', 's'), "Curves...", "image.adjustments.curves"},
    /* Shadows/Highlights at its defaults stores no Fltr (hasoptions false). */
    {"adaptCorrect", 781, "Shado&ws/Highlights...", "image.adjustments.shadowsHighlights"},
};

/* RGB channel references in Levels/Curves (Chnl enum) and their parameter keys ("" = the
 * composite: top-level keys). */
static const char CHANNEL_CODES[4][5] = {"Cmps", "Rd  ", "Grn ", "Bl  "};
static const char *const CHANNEL_KEYS[4] = {"", "red", "green", "blue"};

static bool is_blur_family(const char *c)
{
    return strcmp(c, "GsnB") == 0 || strcmp(c, "boxblur") == 0 || strcmp(c, "Mdn ") == 0 ||
           strcmp(c, "HghP") == 0 || strcmp(c, "Mxm ") == 0 || strcmp(c, "Mnm ") == 0;
}

/* A finite number under key. */
static bool px(const pc_psd_desc *d, const char *key, double *out)
{
    double v;
    if (!pc_io3_num_of(d, key, &v) || !isfinite(v)) {
        return false;
    }
    *out = v;
    return true;
}

static bool int_of(const pc_psd_desc *d, const char *key, int32_t *out)
{
    const pc_psd_value *v = d != NULL ? pc_psd_desc_get(d, key) : NULL;
    if (v == NULL || v->kind != PC_PSD_V_INTEGER) {
        return false;
    }
    *out = v->i;
    return true;
}

static bool bool_or(const pc_psd_desc *d, const char *key, bool dflt)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    return v != NULL && v->kind == PC_PSD_V_BOOLEAN ? v->b : dflt;
}

/* The params key of a Levels/Curves record's Chnl reference, or NULL. */
static const char *channel_key(const pc_psd_desc *d)
{
    const pc_psd_value *v = pc_psd_desc_get(d, "Chnl");
    const uint8_t *b;
    size_t n;
    if (v == NULL || v->kind != PC_PSD_V_REFERENCE || v->nrefs == 0 ||
        v->refs[0].kind != PC_PSD_REF_ENUMERATED) {
        return NULL;
    }
    b = pc_psd_id_bytes(&v->refs[0].value, &n);
    for (size_t i = 0; i < 4; i++) {
        if (n == 4 && memcmp(b, CHANNEL_CODES[i], 4) == 0) {
            return CHANNEL_KEYS[i];
        }
    }
    return NULL;
}

/* The Adjs list when every entry is a descriptor. */
static const pc_psd_value *adjustment_list(const pc_psd_desc *d)
{
    const pc_psd_value *v = pc_psd_desc_get(d, "Adjs");
    if (v == NULL || v->kind != PC_PSD_V_LIST) {
        return NULL;
    }
    for (size_t i = 0; i < v->nlist; i++) {
        if (v->list[i].kind != PC_PSD_V_DESCRIPTOR) {
            return NULL;
        }
    }
    return v;
}

/* An exactly-two-integers list under key. */
static bool int_pair(const pc_psd_desc *a, const char *key, int32_t *lo, int32_t *hi)
{
    const pc_psd_value *v = pc_psd_desc_get(a, key);
    if (v == NULL || v->kind != PC_PSD_V_LIST || v->nlist != 2 || v->list[0].kind != PC_PSD_V_INTEGER ||
        v->list[1].kind != PC_PSD_V_INTEGER) {
        return false;
    }
    *lo = v->list[0].i;
    *hi = v->list[1].i;
    return true;
}

/* JSON building: false on out of memory. */
static bool jadd_real(yyjson_mut_doc *jd, yyjson_mut_val *o, const char *k, double v)
{
    return yyjson_mut_obj_add_real(jd, o, k, v);
}

static bool jadd_int(yyjson_mut_doc *jd, yyjson_mut_val *o, const char *k, int64_t v)
{
    return yyjson_mut_obj_add_sint(jd, o, k, v);
}

/* levels_params: *ok false when a record isn't modelled. */
static bool levels_params(yyjson_mut_doc *jd, yyjson_mut_val *out, const pc_psd_desc *f, bool *ok)
{
    const pc_psd_value *list = adjustment_list(f);
    *ok = false;
    if (list == NULL) {
        return true;
    }
    for (size_t i = 0; i < list->nlist; i++) {
        const pc_psd_desc *a = list->list[i].desc;
        const char *key = channel_key(a);
        yyjson_mut_val *o;
        int32_t lo;
        int32_t hi;
        double g;
        if (key == NULL) {
            return true;
        }
        o = key[0] == '\0' ? out : yyjson_mut_obj(jd);
        if (o == NULL) {
            return false;
        }
        if (int_pair(a, "Inpt", &lo, &hi) &&
            (!jadd_int(jd, o, "inBlack", lo) || !jadd_int(jd, o, "inWhite", hi))) {
            return false;
        }
        if (px(a, "Gmm ", &g) && !jadd_real(jd, o, "gamma", g)) {
            return false;
        }
        if (int_pair(a, "Otpt", &lo, &hi) &&
            (!jadd_int(jd, o, "outBlack", lo) || !jadd_int(jd, o, "outWhite", hi))) {
            return false;
        }
        if (key[0] != '\0' && !yyjson_mut_obj_add_val(jd, out, key, o)) {
            return false;
        }
    }
    *ok = true;
    return true;
}

static bool curves_params(yyjson_mut_doc *jd, yyjson_mut_val *out, const pc_psd_desc *f, bool *ok)
{
    const pc_psd_value *list = adjustment_list(f);
    *ok = false;
    if (list == NULL) {
        return true;
    }
    for (size_t i = 0; i < list->nlist; i++) {
        const pc_psd_desc *a = list->list[i].desc;
        const char *key = channel_key(a);
        const pc_psd_value *pts;
        yyjson_mut_val *arr;
        if (key == NULL) {
            return true;
        }
        pts = pc_psd_desc_get(a, "Crv ");
        if (pts == NULL || pts->kind != PC_PSD_V_LIST) {
            return true;
        }
        arr = yyjson_mut_arr(jd);
        if (arr == NULL) {
            return false;
        }
        for (size_t j = 0; j < pts->nlist; j++) {
            double x;
            double y;
            yyjson_mut_val *pair;
            if (pts->list[j].kind != PC_PSD_V_DESCRIPTOR || !px(pts->list[j].desc, "Hrzn", &x) ||
                !px(pts->list[j].desc, "Vrtc", &y)) {
                return true;
            }
            pair = yyjson_mut_arr(jd);
            if (pair == NULL || !yyjson_mut_arr_add_real(jd, pair, x) ||
                !yyjson_mut_arr_add_real(jd, pair, y) || !yyjson_mut_arr_append(arr, pair)) {
                return false;
            }
        }
        if (!yyjson_mut_obj_add_val(jd, out, key[0] == '\0' ? "points" : key, arr)) {
            return false;
        }
    }
    *ok = true;
    return true;
}

/* known_params: our command and parameters for a known Photoshop filter; *cmd NULL when the
 * filter (or one of its settings) is not modelled. */
static pc_status known_params(const uint8_t *cls, size_t cls_len, const pc_psd_desc *f, const char **cmd,
                              pc_json_value *out, pc_error *err)
{
    const known *k = NULL;
    char c[16];
    yyjson_mut_doc *jd;
    yyjson_mut_val *o;
    bool ok = false;
    bool mem = true;
    pc_status st = PC_OK;
    *cmd = NULL;
    for (size_t i = 0; i < 14; i++) {
        if (strlen(KNOWN[i].cls) == cls_len && memcmp(KNOWN[i].cls, cls, cls_len) == 0) {
            k = &KNOWN[i];
        }
    }
    if (k == NULL || (f == NULL && strcmp(k->cls, "adaptCorrect") != 0)) {
        return PC_OK;
    }
    (void)snprintf(c, sizeof c, "%s", k->cls);
    jd = yyjson_mut_doc_new(NULL);
    o = jd != NULL ? yyjson_mut_obj(jd) : NULL;
    if (o == NULL) {
        yyjson_mut_doc_free(jd);
        return pc_io_nomem(err, "reading a smart filter");
    }
    if (f == NULL) {
        ok = true; /* adaptCorrect without options: {} */
    } else if (is_blur_family(c)) {
        double r;
        ok = px(f, "Rds ", &r);
        mem = !ok || jadd_real(jd, o, "radius", r);
    } else if (strcmp(c, "MtnB") == 0) {
        int32_t a;
        double d;
        ok = int_of(f, "Angl", &a) && px(f, "Dstn", &d);
        mem = !ok || (jadd_int(jd, o, "angle", a) && jadd_real(jd, o, "distance", d));
    } else if (strcmp(c, "UnsM") == 0) {
        double a;
        double r;
        int32_t t;
        ok = px(f, "Amnt", &a) && px(f, "Rds ", &r) && int_of(f, "Thsh", &t);
        mem = !ok || (jadd_real(jd, o, "amount", a) && jadd_real(jd, o, "radius", r) &&
                      jadd_int(jd, o, "threshold", t));
    } else if (strcmp(c, "AdNs") == 0) {
        size_t n = 0;
        const uint8_t *e = pc_io3_enum_of(f, "Dstr", &n);
        const char *dist = NULL;
        const pc_psd_value *mono = pc_psd_desc_get(f, "Mnch");
        double amount;
        int32_t seed = 0;
        if (e != NULL && n == 4 && memcmp(e, "Unfr", 4) == 0) {
            dist = "uniform";
        } else if (e != NULL && n == 4 && memcmp(e, "Gsn ", 4) == 0) {
            dist = "gaussian";
        }
        ok = dist != NULL && mono != NULL && mono->kind == PC_PSD_V_BOOLEAN && px(f, "Nose", &amount);
        if (ok) {
            (void)int_of(f, "FlRs", &seed);
            mem = jadd_real(jd, o, "amount", amount) && yyjson_mut_obj_add_str(jd, o, "distribution", dist) &&
                  yyjson_mut_obj_add_bool(jd, o, "monochromatic", mono->b) && jadd_int(jd, o, "seed", seed);
        }
    } else if (strcmp(c, "Msc ") == 0) {
        double s;
        ok = px(f, "ClSz", &s);
        mem = !ok || jadd_real(jd, o, "cellSize", s);
    } else if (strcmp(c, "Embs") == 0) {
        int32_t a;
        int32_t h;
        int32_t m;
        ok = int_of(f, "Angl", &a) && int_of(f, "Hght", &h) && int_of(f, "Amnt", &m);
        mem = !ok ||
              (jadd_int(jd, o, "angle", a) && jadd_int(jd, o, "height", h) && jadd_int(jd, o, "amount", m));
    } else if (strcmp(c, "Lvls") == 0) {
        mem = levels_params(jd, o, f, &ok);
    } else if (strcmp(c, "Crvs") == 0) {
        mem = curves_params(jd, o, f, &ok);
    }
    if (!mem) {
        st = pc_io_nomem(err, "reading a smart filter");
    } else if (ok) {
        st = pc_json_value_from_mut(o, out, err);
        if (st == PC_OK) {
            *cmd = k->command;
        }
    }
    yyjson_mut_doc_free(jd);
    return st;
}

/* ---- writing ---- */

static pc_status rgbc(double v, pc_psd_value *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value x;
    pc_status st = pc_psd_desc_init(&d, "RGBC", err);
    if (st != PC_OK) {
        return st;
    }
    x = pc_psd_value_double(v);
    st = pc_io3_put(&d, "Rd  ", &x, st, err);
    x = pc_psd_value_double(v);
    st = pc_io3_put(&d, "Grn ", &x, st, err);
    x = pc_psd_value_double(v);
    st = pc_io3_put(&d, "Bl  ", &x, st, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return pc_psd_value_object(&d, out, err);
}

static pc_status blend_options(const pc_smart_filter *f, pc_psd_value *out, pc_error *err)
{
    double opacity = isfinite(f->opacity) ? (double)(f->opacity < 0.0f   ? 0.0f
                                                     : f->opacity > 1.0f ? 1.0f
                                                                         : f->opacity) *
                                                100.0
                                          : 100.0;
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "blendOptions", err);
    if (st != PC_OK) {
        return st;
    }
    v = pc_io3_unit("#Prc", opacity);
    st = pc_io3_put(&d, "Opct", &v, st, err);
    if (st == PC_OK) {
        st = pc_io_blend_long_value(f->blend, &v, err);
        st = st == PC_OK ? pc_psd_desc_add(&d, "Md  ", &v, err) : st;
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return pc_psd_value_object(&d, out, err);
}

/* Parameter keys that are bookkeeping, not filter settings. */
static bool is_meta(const char *k, size_t n)
{
    return (n == 5 && memcmp(k, "layer", 5) == 0) || (n >= 2 && k[0] == '_' && k[1] == '_');
}

static yyjson_mut_val *jget(yyjson_mut_val *p, const char *key)
{
    return yyjson_mut_is_obj(p) ? yyjson_mut_obj_get(p, key) : NULL;
}

static bool jf64(yyjson_mut_val *v, double *out)
{
    if (v == NULL || !yyjson_mut_is_num(v)) {
        return false;
    }
    *out = yyjson_mut_get_num(v);
    return true;
}

static double pf(yyjson_mut_val *p, const char *key, double dflt)
{
    double v;
    return jf64(jget(p, key), &v) && isfinite(v) ? v : dflt;
}

static pc_psd_value pi(yyjson_mut_val *p, const char *key, double dflt)
{
    double v = round(pf(p, key, dflt));
    v = v<(double)INT32_MIN ? (double)INT32_MIN : v>(double) INT32_MAX ? (double)INT32_MAX : v;
    return pc_psd_value_int((int32_t)v);
}

static pc_status channel_ref(const char code[4], pc_psd_value *out, pc_error *err)
{
    pc_psd_ref_item *it = calloc(1, sizeof *it);
    pc_status st;
    if (it == NULL) {
        return pc_io_nomem(err, "writing a channel reference");
    }
    it->kind = PC_PSD_REF_ENUMERATED;
    st = pc_psd_ustr_from_utf8("", true, &it->cls.name, err);
    if (st == PC_OK) {
        st = pc_psd_id_make("Chnl", &it->cls.class_id, err);
    }
    if (st == PC_OK) {
        st = pc_psd_id_make("Chnl", &it->key, err);
    }
    it->value.is_code = true;
    memcpy(it->value.code, code, 4);
    memset(out, 0, sizeof *out);
    out->kind = PC_PSD_V_REFERENCE;
    out->refs = it;
    out->nrefs = 1;
    if (st != PC_OK) {
        pc_psd_value_fini(out);
    }
    return st;
}

/* An error that is the reference's Err(String): PC_ERR_UNSUPPORTED with the text. */
#define REASON(err, ...) pc_fail((err), PC_ERR_UNSUPPORTED, __VA_ARGS__)

/* The Adjs entries of a Levels or Curves filter: the composite first, then red, green, blue. */
static pc_status adjustments(yyjson_mut_val *p, bool curves, pc_psd_value *out, pc_error *err)
{
    static const char *const LV[] = {"inBlack",  "inWhite", "gamma", "outBlack",
                                     "outWhite", "red",     "green", "blue"};
    static const char *const CV[] = {"points", "red", "green", "blue"};
    const char *const *allowed = curves ? CV : LV;
    size_t nallowed = curves ? 4 : 8;
    yyjson_mut_val *k;
    yyjson_mut_obj_iter it;
    pc_psd_value list;
    pc_status st = PC_OK;
    if (!yyjson_mut_is_obj(p)) {
        return REASON(err, "parameters are not an object");
    }
    yyjson_mut_obj_iter_init(p, &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        const char *ks = yyjson_mut_get_str(k);
        size_t kn = yyjson_mut_get_len(k);
        bool ok = is_meta(ks, kn);
        for (size_t i = 0; i < nallowed && !ok; i++) {
            ok = strlen(allowed[i]) == kn && memcmp(allowed[i], ks, kn) == 0;
        }
        if (!ok) {
            return REASON(err, "setting `%.*s` has no Photoshop equivalent", (int)(kn > 200 ? 200 : kn), ks);
        }
    }
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (size_t c = 0; c < 4 && st == PC_OK; c++) {
        const char *key = CHANNEL_KEYS[c];
        yyjson_mut_val *o = key[0] == '\0' ? p : jget(p, key);
        pc_psd_desc a;
        pc_psd_value x;
        if (o == NULL) {
            continue;
        }
        if (!curves && key[0] == '\0') {
            static const char *const LK[] = {"inBlack", "inWhite", "gamma", "outBlack", "outWhite"};
            bool any = false;
            for (size_t i = 0; i < 5; i++) {
                any = any || jget(o, LK[i]) != NULL;
            }
            if (!any) {
                continue;
            }
        }
        if (curves) {
            yyjson_mut_val *src = key[0] == '\0' ? jget(p, "points") : o;
            if (!yyjson_mut_is_arr(src)) {
                continue;
            }
        }
        st = pc_psd_desc_init(&a, curves ? "CrvA" : "LvlA", err);
        if (st != PC_OK) {
            break;
        }
        st = channel_ref(CHANNEL_CODES[c], &x, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&a, "Chnl", &x, err);
        }
        if (st == PC_OK && curves) {
            yyjson_mut_val *src = key[0] == '\0' ? jget(p, "points") : o;
            pc_psd_value pts;
            yyjson_mut_arr_iter ai;
            yyjson_mut_val *q;
            memset(&pts, 0, sizeof pts);
            pts.kind = PC_PSD_V_LIST;
            yyjson_mut_arr_iter_init(src, &ai);
            while (st == PC_OK && (q = yyjson_mut_arr_iter_next(&ai)) != NULL) {
                double px0;
                double py0;
                pc_psd_desc pd;
                pc_psd_value pv;
                if (!yyjson_mut_is_arr(q) || !jf64(yyjson_mut_arr_get(q, 0), &px0) || !isfinite(px0) ||
                    !jf64(yyjson_mut_arr_get(q, 1), &py0) || !isfinite(py0)) {
                    st = REASON(err, "a curve point is not [input, output]");
                    break;
                }
                st = pc_psd_desc_init(&pd, "Pnt ", err);
                if (st != PC_OK) {
                    break;
                }
                pv = pc_psd_value_double(px0);
                st = pc_io3_put(&pd, "Hrzn", &pv, st, err);
                pv = pc_psd_value_double(py0);
                st = pc_io3_put(&pd, "Vrtc", &pv, st, err);
                if (st == PC_OK) {
                    st = pc_psd_value_object(&pd, &pv, err);
                } else {
                    pc_psd_desc_fini(&pd);
                }
                if (st == PC_OK) {
                    st = pc_psd_value_list_push(&pts, &pv, err);
                }
            }
            st = pc_io3_put(&a, "Crv ", &pts, st, err);
        } else if (st == PC_OK) {
            pc_psd_value in;
            pc_psd_value v1;
            memset(&in, 0, sizeof in);
            in.kind = PC_PSD_V_LIST;
            v1 = pi(o, "inBlack", 0.0);
            st = pc_psd_value_list_push(&in, &v1, err);
            v1 = pi(o, "inWhite", 255.0);
            st = st == PC_OK ? pc_psd_value_list_push(&in, &v1, err) : st;
            st = pc_io3_put(&a, "Inpt", &in, st, err);
            v1 = pc_psd_value_double(pf(o, "gamma", 1.0));
            st = pc_io3_put(&a, "Gmm ", &v1, st, err);
            if (st == PC_OK && (jget(o, "outBlack") != NULL || jget(o, "outWhite") != NULL)) {
                pc_psd_value ot;
                memset(&ot, 0, sizeof ot);
                ot.kind = PC_PSD_V_LIST;
                v1 = pi(o, "outBlack", 0.0);
                st = pc_psd_value_list_push(&ot, &v1, err);
                v1 = pi(o, "outWhite", 255.0);
                st = st == PC_OK ? pc_psd_value_list_push(&ot, &v1, err) : st;
                st = pc_io3_put(&a, "Otpt", &ot, st, err);
            }
        }
        if (st == PC_OK) {
            st = pc_psd_value_object(&a, &x, err);
            if (st == PC_OK) {
                st = pc_psd_value_list_push(&list, &x, err);
            }
        } else {
            pc_psd_desc_fini(&a);
        }
    }
    if (st != PC_OK) {
        pc_psd_value_fini(&list);
        return st;
    }
    *out = list;
    return PC_OK;
}

/* fltr_for: the Fltr descriptor of a modelled filter (*has false = written without options). */
static pc_status fltr_for(const known *k, yyjson_mut_val *p, bool *has, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st;
    const char *c = k->cls;
    if (strcmp(c, "adaptCorrect") == 0) {
        if (yyjson_mut_is_obj(p)) {
            yyjson_mut_obj_iter it;
            yyjson_mut_val *key;
            yyjson_mut_obj_iter_init(p, &it);
            while ((key = yyjson_mut_obj_iter_next(&it)) != NULL) {
                if (!is_meta(yyjson_mut_get_str(key), yyjson_mut_get_len(key))) {
                    return REASON(err, "only default Shadows/Highlights settings are written to PSD");
                }
            }
        }
        *has = false;
        return PC_OK;
    }
    if (is_blur_family(c)) {
        yyjson_mut_val *pr = jget(p, "preserve");
        if (yyjson_mut_is_str(pr) && yyjson_mut_equals_str(pr, "roundness")) {
            return REASON(err, "\xE2\x80\x9Cpreserve roundness\xE2\x80\x9D has no Photoshop equivalent");
        }
    }
    st = pc_psd_desc_init(&d, c, err);
    if (st != PC_OK) {
        return st;
    }
    if (is_blur_family(c)) {
        v = pc_io3_unit("#Pxl", pf(p, "radius", 1.0));
        st = pc_io3_put(&d, "Rds ", &v, st, err);
    } else if (strcmp(c, "MtnB") == 0) {
        v = pi(p, "angle", 0.0);
        st = pc_io3_put(&d, "Angl", &v, st, err);
        v = pc_io3_unit("#Pxl", pf(p, "distance", 10.0));
        st = pc_io3_put(&d, "Dstn", &v, st, err);
    } else if (strcmp(c, "UnsM") == 0) {
        v = pc_io3_unit("#Prc", pf(p, "amount", 50.0));
        st = pc_io3_put(&d, "Amnt", &v, st, err);
        v = pc_io3_unit("#Pxl", pf(p, "radius", 1.0));
        st = pc_io3_put(&d, "Rds ", &v, st, err);
        v = pi(p, "threshold", 0.0);
        st = pc_io3_put(&d, "Thsh", &v, st, err);
    } else if (strcmp(c, "AdNs") == 0) {
        yyjson_mut_val *dist = jget(p, "distribution");
        yyjson_mut_val *mono = jget(p, "monochromatic");
        bool gauss = yyjson_mut_is_str(dist) && yyjson_mut_equals_str(dist, "gaussian");
        st = pc_psd_value_enum("Dstr", gauss ? "Gsn " : "Unfr", &v, err);
        st = pc_io3_put(&d, "Dstr", &v, st, err);
        v = pc_io3_unit("#Prc", pf(p, "amount", 12.5));
        st = pc_io3_put(&d, "Nose", &v, st, err);
        v = pc_psd_value_bool(yyjson_mut_is_bool(mono) && yyjson_mut_get_bool(mono));
        st = pc_io3_put(&d, "Mnch", &v, st, err);
        v = pi(p, "seed", 0.0);
        st = pc_io3_put(&d, "FlRs", &v, st, err);
    } else if (strcmp(c, "Msc ") == 0) {
        v = pc_io3_unit("#Pxl", pf(p, "cellSize", 10.0));
        st = pc_io3_put(&d, "ClSz", &v, st, err);
    } else if (strcmp(c, "Embs") == 0) {
        v = pi(p, "angle", 135.0);
        st = pc_io3_put(&d, "Angl", &v, st, err);
        v = pi(p, "height", 3.0);
        st = pc_io3_put(&d, "Hght", &v, st, err);
        v = pi(p, "amount", 100.0);
        st = pc_io3_put(&d, "Amnt", &v, st, err);
    } else if (strcmp(c, "Lvls") == 0 || strcmp(c, "Crvs") == 0) {
        st = adjustments(p, strcmp(c, "Crvs") == 0, &v, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&d, "Adjs", &v, err);
        }
    } else {
        st = REASON(err, "not a Photoshop filter");
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *has = true;
    *out = d;
    return PC_OK;
}

/* Sets blendOptions and enab from the filter (template items keep their place). */
static pc_status set_blend_enab(pc_psd_desc *d, const pc_smart_filter *f, pc_error *err)
{
    pc_psd_value v;
    pc_status st = blend_options(f, &v, err);
    if (st == PC_OK) {
        st = pc_io3_set(d, "blendOptions", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_bool(f->visible);
        st = pc_io3_set(d, "enab", &v, err);
    }
    return st;
}

pc_status pc_io_item_for_filter(const pc_smart_filter *f, pc_psd_desc *out, pc_error *err)
{
    yyjson_mut_val *p = pc_json_value_root(&f->params);
    const known *k = NULL;
    pc_psd_desc d;
    pc_psd_desc fl;
    bool has = false;
    pc_psd_value v;
    pc_status st;
    if (pc_str_eq_c(&f->command, PC_IO_CAMERA_RAW_COMMAND)) {
        st = pc_io_camera_raw_export_item(&f->params, &d, err);
        if (st == PC_OK) {
            st = set_blend_enab(&d, f, err);
            if (st != PC_OK) {
                pc_psd_desc_fini(&d);
            }
        }
        if (st == PC_OK) {
            *out = d;
        }
        return st;
    }
    if (pc_str_eq_c(&f->command, PC_IO_UNSUPPORTED_FILTER)) {
        yyjson_mut_val *hex = jget(p, "psd");
        uint8_t *raw = NULL;
        size_t n = 0;
        bool ok = false;
        pc_error pe;
        if (!yyjson_mut_is_str(hex)) {
            return REASON(err, "its Photoshop data is missing");
        }
        st = pc_io3_from_hex(yyjson_mut_get_str(hex), yyjson_mut_get_len(hex), &ok, &raw, &n, err);
        if (st != PC_OK) {
            return st;
        }
        if (!ok) {
            return REASON(err, "its Photoshop data is missing");
        }
        st = pc_psd_desc_parse(raw, n, &d, &pe);
        free(raw);
        if (st == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
        }
        if (st != PC_OK) {
            return REASON(err, "its Photoshop data is unreadable (%s)", pe.msg);
        }
        st = set_blend_enab(&d, f, err);
        if (st != PC_OK) {
            pc_psd_desc_fini(&d);
            return st;
        }
        *out = d;
        return PC_OK;
    }
    for (size_t i = 0; i < 14; i++) {
        if (pc_str_eq_c(&f->command, KNOWN[i].command)) {
            k = &KNOWN[i];
            break;
        }
    }
    if (k == NULL) {
        return REASON(err, "Photoshop has no equivalent filter");
    }
    st = fltr_for(k, p, &has, &fl, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_psd_desc_init(&d, "filterFX", err);
    if (st != PC_OK) {
        if (has) {
            pc_psd_desc_fini(&fl);
        }
        return st;
    }
    st = pc_psd_value_text(k->name, true, &v, err);
    st = pc_io3_put(&d, "Nm  ", &v, st, err);
    if (st == PC_OK) {
        st = blend_options(f, &v, err);
        st = pc_io3_put(&d, "blendOptions", &v, st, err);
    }
    v = pc_psd_value_bool(f->visible);
    st = pc_io3_put(&d, "enab", &v, st, err);
    v = pc_psd_value_bool(has);
    st = pc_io3_put(&d, "hasoptions", &v, st, err);
    if (st == PC_OK) {
        st = rgbc(0.0, &v, err);
        st = pc_io3_put(&d, "FrgC", &v, st, err);
    }
    if (st == PC_OK) {
        st = rgbc(255.0, &v, err);
        st = pc_io3_put(&d, "BckC", &v, st, err);
    }
    if (has) {
        st = pc_io3_put_obj(&d, "Fltr", &fl, st, err);
    }
    v = pc_psd_value_int(k->filter_id);
    st = pc_io3_put(&d, "filterID", &v, st, err);
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

/* The unsupported-filter fallback: name without '&', filterID and the neutral descriptor. */
static pc_status unsupported_params(const pc_psd_desc *item, int32_t filter_id, pc_json_value *out,
                                    pc_error *err)
{
    pc_smart_filter neutral_f;
    pc_psd_desc neutral;
    pc_psd_bytes bytes = {NULL, 0};
    char *name = NULL;
    char *hex = NULL;
    bool found = false;
    yyjson_mut_doc *jd = NULL;
    yyjson_mut_val *o = NULL;
    pc_status st = pc_io3_text_of(item, "Nm  ", &found, &name, err);
    if (st != PC_OK) {
        return st;
    }
    memset(&neutral_f, 0, sizeof neutral_f);
    neutral_f.blend = PC_BLEND_NORMAL;
    neutral_f.opacity = 1.0f;
    neutral_f.visible = true;
    st = pc_psd_desc_clone(item, &neutral, err);
    if (st != PC_OK) {
        free(name);
        return st;
    }
    for (size_t i = 0; i < neutral.n && st == PC_OK; i++) {
        if (pc_psd_id_is(&neutral.items[i].key, "blendOptions")) {
            pc_psd_value v;
            st = blend_options(&neutral_f, &v, err);
            if (st == PC_OK) {
                pc_psd_value_fini(&neutral.items[i].value);
                neutral.items[i].value = v;
            }
        } else if (pc_psd_id_is(&neutral.items[i].key, "enab")) {
            pc_psd_value_fini(&neutral.items[i].value);
            neutral.items[i].value = pc_psd_value_bool(true);
        }
    }
    if (st == PC_OK) {
        st = pc_psd_desc_to_bytes(&neutral, &bytes, err);
    }
    pc_psd_desc_fini(&neutral);
    if (st == PC_OK) {
        st = pc_io3_to_hex(bytes.data, bytes.len, &hex, err);
    }
    pc_psd_bytes_fini(&bytes);
    if (st == PC_OK) {
        /* name.replace('&', "") */
        size_t k = 0;
        for (size_t i = 0; found && name[i] != '\0'; i++) {
            if (name[i] != '&') {
                name[k++] = name[i];
            }
        }
        if (found) {
            name[k] = '\0';
        }
        jd = yyjson_mut_doc_new(NULL);
        o = jd != NULL ? yyjson_mut_obj(jd) : NULL;
        if (o == NULL || !yyjson_mut_obj_add_strcpy(jd, o, "name", found ? name : "") ||
            !yyjson_mut_obj_add_sint(jd, o, "filterID", filter_id) ||
            !yyjson_mut_obj_add_strcpy(jd, o, "psd", hex)) {
            st = pc_io_nomem(err, "reading a smart filter");
        } else {
            st = pc_json_value_from_mut(o, out, err);
        }
    }
    yyjson_mut_doc_free(jd);
    free(hex);
    free(name);
    return st;
}

pc_status pc_io_filter_from_item(const pc_psd_desc *item, pc_smart_filter *out, pc_error *err)
{
    const pc_psd_desc *opts = pc_io3_get_desc(item, "blendOptions");
    const pc_psd_desc *fltr = pc_io3_get_desc(item, "Fltr");
    pc_smart_filter f;
    double o;
    size_t n = 0;
    const uint8_t *md = pc_io3_enum_of(opts, "Md  ", &n);
    const uint8_t *cls = NULL;
    size_t cls_len = 0;
    int32_t filter_id = 0;
    const char *cmd = NULL;
    pc_json_value params = PC_JSON_VALUE_INIT;
    pc_status st = PC_OK;
    memset(&f, 0, sizeof f);
    f.opacity = 1.0f;
    if (px(opts, "Opct", &o)) {
        double v = o / 100.0;
        f.opacity = (float)(v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v);
    }
    f.blend = PC_BLEND_NORMAL;
    if (md == NULL || !pc_io_blend_from_id(md, n, &f.blend)) {
        f.blend = PC_BLEND_NORMAL;
    }
    f.visible = bool_or(item, "enab", true);
    (void)int_of(item, "filterID", &filter_id);
    if (fltr != NULL) {
        cls = pc_psd_id_bytes(&fltr->class_id, &cls_len);
    } else {
        for (size_t i = 0; i < 14; i++) {
            if (KNOWN[i].filter_id == filter_id) {
                cls = (const uint8_t *)KNOWN[i].cls;
                cls_len = strlen(KNOWN[i].cls);
                break;
            }
        }
    }
    if (cls != NULL && cls_len == strlen(PC_IO_CAMERA_RAW_CLASS) &&
        memcmp(cls, PC_IO_CAMERA_RAW_CLASS, cls_len) == 0) {
        bool found = false;
        st = pc_io_camera_raw_import_params(item, &found, &params, err);
        cmd = st == PC_OK && found ? PC_IO_CAMERA_RAW_COMMAND : NULL;
    } else if (cls != NULL) {
        st = known_params(cls, cls_len, fltr, &cmd, &params, err);
    }
    if (st == PC_OK && cmd == NULL) {
        cmd = PC_IO_UNSUPPORTED_FILTER;
        st = unsupported_params(item, filter_id, &params, err);
    }
    if (st == PC_OK) {
        st = pc_str_set_c(&f.command, cmd, err);
    }
    if (st != PC_OK) {
        pc_json_value_fini(&params);
        pc_str_fini(&f.command);
        return st;
    }
    f.params = params;
    *out = f;
    return PC_OK;
}

/* ---- stacks ---- */

bool pc_io_filter_stack_eq(const pc_io_filter_stack *a, const pc_io_filter_stack *b)
{
    if (a->enabled != b->enabled || a->mask_enabled != b->mask_enabled || a->mask_linked != b->mask_linked ||
        a->nfilters != b->nfilters) {
        return false;
    }
    for (size_t i = 0; i < a->nfilters; i++) {
        if (!pc_smart_filter_eq(&a->filters[i], &b->filters[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_io_filter_stack_clone(const pc_io_filter_stack *s, pc_io_filter_stack *out, pc_error *err)
{
    pc_io_filter_stack c = *s;
    c.filters = NULL;
    c.nfilters = 0;
    if (s->nfilters > 0) {
        c.filters = pc_calloc_array(s->nfilters, sizeof *c.filters);
        if (c.filters == NULL) {
            return pc_io_nomem(err, "copying smart filters");
        }
    }
    for (size_t i = 0; i < s->nfilters; i++) {
        pc_status st = pc_smart_filter_clone(&s->filters[i], &c.filters[i], err);
        if (st != PC_OK) {
            pc_io_filter_stack_fini(&c);
            return st;
        }
        c.nfilters++;
    }
    *out = c;
    return PC_OK;
}

pc_status pc_io_filter_stack_read(const pc_psd_desc *fx, pc_io_filter_stack *out, pc_error *err)
{
    const pc_psd_value *list = pc_psd_desc_get(fx, "filterFXList");
    pc_io_filter_stack s;
    pc_io_filter_stack_init(&s);
    s.enabled = bool_or(fx, "enab", true);
    s.mask_enabled = bool_or(fx, "filterMaskEnable", true);
    s.mask_linked = bool_or(fx, "filterMaskLinked", false);
    if (list != NULL && list->kind == PC_PSD_V_LIST && list->nlist > 0) {
        s.filters = pc_calloc_array(list->nlist, sizeof *s.filters);
        if (s.filters == NULL) {
            return pc_io_nomem(err, "reading smart filters");
        }
        for (size_t i = 0; i < list->nlist; i++) {
            pc_status st;
            if (list->list[i].kind != PC_PSD_V_DESCRIPTOR) {
                continue;
            }
            st = pc_io_filter_from_item(list->list[i].desc, &s.filters[s.nfilters], err);
            if (st != PC_OK) {
                pc_io_filter_stack_fini(&s);
                return st;
            }
            s.nfilters++;
        }
    }
    *out = s;
    return PC_OK;
}

pc_status pc_io_filter_fx(const pc_io_filter_stack *stack, const char *layer, pc_io_warnings *warnings,
                          pc_psd_desc *out, pc_error *err)
{
    pc_psd_value list;
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = PC_OK;
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (size_t i = 0; i < stack->nfilters && st == PC_OK; i++) {
        pc_psd_desc item;
        pc_error fe;
        st = pc_io_item_for_filter(&stack->filters[i], &item, &fe);
        if (st == PC_ERR_UNSUPPORTED) {
            st = pc_io_warn(warnings, err, "layer \"%s\": smart filter %s was not written to PSD: %s", layer,
                            pc_str_c(&stack->filters[i].command), fe.msg);
        } else if (st != PC_OK) {
            if (err != NULL) {
                *err = fe;
            }
        } else {
            st = pc_psd_value_object(&item, &v, err);
            if (st == PC_OK) {
                st = pc_psd_value_list_push(&list, &v, err);
            }
        }
    }
    if (st == PC_OK) {
        st = pc_psd_desc_init(&d, "filterFXStyle", err);
        if (st == PC_OK) {
            v = pc_psd_value_bool(stack->enabled);
            st = pc_io3_put(&d, "enab", &v, st, err);
            v = pc_psd_value_bool(true);
            st = pc_io3_put(&d, "validAtPosition", &v, st, err);
            v = pc_psd_value_bool(stack->mask_enabled);
            st = pc_io3_put(&d, "filterMaskEnable", &v, st, err);
            v = pc_psd_value_bool(stack->mask_linked);
            st = pc_io3_put(&d, "filterMaskLinked", &v, st, err);
            v = pc_psd_value_bool(true);
            st = pc_io3_put(&d, "filterMaskExtendWithWhite", &v, st, err);
            st = pc_io3_put(&d, "filterFXList", &list, st, err);
            if (st != PC_OK) {
                pc_psd_desc_fini(&d);
            }
        } else {
            pc_psd_value_fini(&list);
        }
    } else {
        pc_psd_value_fini(&list);
    }
    if (st == PC_OK) {
        *out = d;
    }
    return st;
}
