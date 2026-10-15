/* Swatch data model and colour management (port of crates/engine/src/presets/swatches.rs, E2b).
 *
 * A swatch keeps its colour in its own model (RGB/HSB/CMYK/Lab/gray, or a colour-book entry kept
 * verbatim). SwatchConv turns a swatch into the RGB working-space colour the foreground holds,
 * through photocraft-cms (relative colorimetric + BPC, Photoshop's default). Never crashes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cms/builtin.h"
#include "cms/cms.h"
#include "cms/profile.h"
#include "cms/transform.h"
#include "color/convert.h"
#include "engine_internal.h"
#include "presets.h"

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

static float finite_f(float v)
{
    return isfinite(v) ? v : 0.0f;
}
static float unit_f(float v)
{
    v = finite_f(v);
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ---- lifecycle + ops ---------------------------------------------------------------------- */

void pc_swatch_fini(pc_swatch *s)
{
    if (s == NULL) {
        return;
    }
    free(s->name);
    s->name = NULL;
}

pc_status pc_swatch_clone(const pc_swatch *src, pc_swatch *out, pc_error *err)
{
    *out = *src;
    out->name = dup_str(src->name);
    if (out->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "swatch");
    }
    return PC_OK;
}

static const char *sw_get_name(const void *item)
{
    return ((const pc_swatch *)item)->name;
}
static bool sw_set_name(void *item, const char *name)
{
    char *nm = dup_str(name);
    if (nm == NULL) {
        return false;
    }
    pc_swatch *s = item;
    free(s->name);
    s->name = nm;
    return true;
}
static void sw_fini(void *item)
{
    pc_swatch_fini(item);
}
static pc_status sw_clone(const void *src, void *dst, pc_error *err)
{
    return pc_swatch_clone(src, dst, err);
}

const pc_preset_item_ops *pc_swatch_ops(void)
{
    static const pc_preset_item_ops ops = {sizeof(pc_swatch), sw_get_name, sw_set_name, sw_fini, sw_clone};
    return &ops;
}

/* ---- kind / model ids --------------------------------------------------------------------- */

const char *pc_swatch_model_id(pc_swatch_model m)
{
    switch (m) {
    case PC_SWATCH_RGB:
        return "rgb";
    case PC_SWATCH_HSB:
        return "hsb";
    case PC_SWATCH_CMYK:
        return "cmyk";
    case PC_SWATCH_LAB:
        return "lab";
    case PC_SWATCH_GRAY:
        return "gray";
    default:
        return "book";
    }
}

const char *pc_swatch_kind_id(pc_swatch_kind k)
{
    switch (k) {
    case PC_SWATCH_GLOBAL:
        return "global";
    case PC_SWATCH_SPOT:
        return "spot";
    default:
        return "process";
    }
}

bool pc_swatch_kind_parse(const char *s, pc_swatch_kind *out)
{
    if (strcmp(s, "process") == 0 || strcmp(s, "normal") == 0) {
        *out = PC_SWATCH_PROCESS;
        return true;
    }
    if (strcmp(s, "global") == 0) {
        *out = PC_SWATCH_GLOBAL;
        return true;
    }
    if (strcmp(s, "spot") == 0) {
        *out = PC_SWATCH_SPOT;
        return true;
    }
    return false;
}

void pc_swatch_hsb_to_rgb(const float hsb[3], float out[3])
{
    float s = unit_f(hsb[1]);
    float v = unit_f(hsb[2]);
    float h = finite_f(hsb[0]);
    h = h - 360.0f * floorf(h / 360.0f); /* rem_euclid(360) */
    h /= 60.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    float r, g, b;
    unsigned seg = (unsigned)h;
    switch (seg) {
    case 0:
        r = c, g = x, b = 0.0f;
        break;
    case 1:
        r = x, g = c, b = 0.0f;
        break;
    case 2:
        r = 0.0f, g = c, b = x;
        break;
    case 3:
        r = 0.0f, g = x, b = c;
        break;
    case 4:
        r = x, g = 0.0f, b = c;
        break;
    default:
        r = c, g = 0.0f, b = x;
        break;
    }
    float m = v - c;
    out[0] = unit_f(r + m);
    out[1] = unit_f(g + m);
    out[2] = unit_f(b + m);
}

/* ---- param / display / json --------------------------------------------------------------- */

static int hexv(char c)
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

bool pc_swatch_color_from_param(yyjson_val *v, pc_swatch_color *out, char msg[256])
{
    memset(out, 0, sizeof *out);
    size_t slen = 0;
    const char *s = pc_json_as_str(v, &slen);
    if (s != NULL) {
        /* trim + strip '#' */
        size_t a = 0, b = slen;
        while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
            a++;
        }
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
            b--;
        }
        const char *h = s + a;
        size_t hn = b - a;
        if (hn > 0 && h[0] == '#') {
            h++;
            hn--;
        }
        bool ok = hn == 6;
        for (size_t i = 0; ok && i < 6; i++) {
            if (hexv(h[i]) < 0) {
                ok = false;
            }
        }
        if (!ok) {
            (void)snprintf(msg, 256, "bad colour `%.*s` (\"#rrggbb\" or {\"model\",\"values\"})",
                           (int)slen, s);
            return false;
        }
        out->model = PC_SWATCH_RGB;
        for (int i = 0; i < 3; i++) {
            out->v[i] = (float)(hexv(h[i * 2]) * 16 + hexv(h[i * 2 + 1])) / 255.0f;
        }
        return true;
    }
    size_t mlen = 0;
    const char *model = pc_json_as_str(pc_json_get(v, "model"), &mlen);
    if (model == NULL) {
        (void)snprintf(msg, 256, "a colour needs `model` (rgb|hsb|cmyk|lab|gray) and `values`");
        return false;
    }
    yyjson_val *arr = pc_json_get(v, "values");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        (void)snprintf(msg, 256, "a colour needs `values`");
        return false;
    }
    float vals[8];
    size_t nv = 0, idx, max;
    yyjson_val *el;
    yyjson_arr_foreach(arr, idx, max, el) {
        double d;
        if (!pc_json_as_f64(el, &d) || !isfinite(d) || fabs(d) > (double)3.4028234663852886e38) {
            (void)snprintf(msg, 256, "colour values must be finite numbers within f32 range");
            return false;
        }
        if (nv < 8) {
            vals[nv] = (float)d;
        }
        nv++;
    }
#define NEED(k)                                                                   \
    do {                                                                          \
        if (nv != (k)) {                                                          \
            (void)snprintf(msg, 256, "a %.*s colour has %d values, got %zu",      \
                           (int)mlen, model, (int)(k), nv);                       \
            return false;                                                         \
        }                                                                         \
    } while (0)
#define PCT(x) (((x) / 100.0f) < 0.0f ? 0.0f : (((x) / 100.0f) > 1.0f ? 1.0f : ((x) / 100.0f)))
    if (mlen == 3 && memcmp(model, "rgb", 3) == 0) {
        NEED(3);
        out->model = PC_SWATCH_RGB;
        for (int i = 0; i < 3; i++) {
            float x = vals[i] / 255.0f;
            out->v[i] = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        }
    } else if (mlen == 3 && memcmp(model, "hsb", 3) == 0) {
        NEED(3);
        out->model = PC_SWATCH_HSB;
        float h = vals[0] - 360.0f * floorf(vals[0] / 360.0f);
        out->v[0] = h;
        out->v[1] = PCT(vals[1]);
        out->v[2] = PCT(vals[2]);
    } else if (mlen == 4 && memcmp(model, "cmyk", 4) == 0) {
        NEED(4);
        out->model = PC_SWATCH_CMYK;
        for (int i = 0; i < 4; i++) {
            out->v[i] = PCT(vals[i]);
        }
    } else if (mlen == 3 && memcmp(model, "lab", 3) == 0) {
        NEED(3);
        out->model = PC_SWATCH_LAB;
        float l = vals[0] < 0.0f ? 0.0f : (vals[0] > 100.0f ? 100.0f : vals[0]);
        float aa = vals[1] < -128.0f ? -128.0f : (vals[1] > 127.0f ? 127.0f : vals[1]);
        float bb = vals[2] < -128.0f ? -128.0f : (vals[2] > 127.0f ? 127.0f : vals[2]);
        out->v[0] = l;
        out->v[1] = aa;
        out->v[2] = bb;
    } else if (mlen == 4 && memcmp(model, "gray", 4) == 0) {
        NEED(1);
        out->model = PC_SWATCH_GRAY;
        out->v[0] = PCT(vals[0]);
    } else {
        (void)snprintf(msg, 256, "unknown colour model `%.*s` (rgb|hsb|cmyk|lab|gray)", (int)mlen, model);
        return false;
    }
#undef NEED
#undef PCT
    return true;
}

size_t pc_swatch_color_display_values(const pc_swatch_color *c, double out[5])
{
#define R(v, k) (round((double)finite_f(v) * (k) * 1000.0) / 1000.0)
    switch (c->model) {
    case PC_SWATCH_RGB:
        out[0] = R(c->v[0], 255.0);
        out[1] = R(c->v[1], 255.0);
        out[2] = R(c->v[2], 255.0);
        return 3;
    case PC_SWATCH_HSB:
        out[0] = R(c->v[0], 1.0);
        out[1] = R(c->v[1], 100.0);
        out[2] = R(c->v[2], 100.0);
        return 3;
    case PC_SWATCH_CMYK:
        for (int i = 0; i < 4; i++) {
            out[i] = R(c->v[i], 100.0);
        }
        return 4;
    case PC_SWATCH_LAB:
        out[0] = R(c->v[0], 1.0);
        out[1] = R(c->v[1], 1.0);
        out[2] = R(c->v[2], 1.0);
        return 3;
    case PC_SWATCH_GRAY:
        out[0] = R(c->v[0], 100.0);
        return 1;
    default:
        out[0] = (double)c->book_space;
        for (int i = 0; i < 4; i++) {
            out[i + 1] = (double)c->book_values[i];
        }
        return 5;
    }
#undef R
}

yyjson_mut_val *pc_swatch_color_to_json(yyjson_mut_doc *d, const pc_swatch_color *c)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "model", pc_swatch_model_id(c->model));
    double vals[5];
    size_t n = pc_swatch_color_display_values(c, vals);
    if (c->model == PC_SWATCH_GRAY) {
        /* serde serialises a single-value gray as the scalar, not a one-element array. */
        yyjson_mut_obj_add_real(d, o, "values", vals[0]);
    } else {
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        for (size_t i = 0; i < n; i++) {
            yyjson_mut_arr_add_real(d, arr, vals[i]);
        }
        yyjson_mut_obj_add_val(d, o, "values", arr);
    }
    return o;
}

void pc_swatch_hex(const float rgb[3], char buf[8])
{
    unsigned r = (unsigned)lround((double)unit_f(rgb[0]) * 255.0);
    unsigned g = (unsigned)lround((double)unit_f(rgb[1]) * 255.0);
    unsigned b = (unsigned)lround((double)unit_f(rgb[2]) * 255.0);
    (void)snprintf(buf, 8, "#%02x%02x%02x", r & 0xff, g & 0xff, b & 0xff);
}

/* ---- SwatchConv (CMS) --------------------------------------------------------------------- */

struct pc_swatch_conv {
    pc_transform *cmyk;
    pc_transform *lab;
    pc_transform *gray;
};

static uint32_t cs_space(pc_color_mode mode)
{
    switch (mode) {
    case PC_COLOR_MODE_GRAYSCALE:
        return PC_CS_GRAY;
    case PC_COLOR_MODE_CMYK:
        return PC_CS_CMYK;
    case PC_COLOR_MODE_LAB:
        return PC_CS_LAB;
    default:
        return PC_CS_RGB;
    }
}

/* The working profile for `mode`: the session's (resolved; must match the space) or the built-in.
 * On success returns a borrowed profile; when it allocated one *tmp holds it (*owned true). */
static const pc_profile *working(const pc_engine_session *s, pc_color_mode mode, pc_profile *tmp,
                                 bool *owned)
{
    *owned = false;
    if (s != NULL) {
        const char *spec = mode == PC_COLOR_MODE_CMYK      ? s->color.settings.working_cmyk
                           : mode == PC_COLOR_MODE_GRAYSCALE ? s->color.settings.working_gray
                                                             : s->color.settings.working_rgb;
        pc_error e = {0};
        if (spec != NULL &&
            pc_engine_resolve_profile(&s->color, spec, NULL, true, mode, tmp, &e) == PC_OK) {
            if (tmp->space == cs_space(mode)) {
                *owned = true;
                return tmp;
            }
            pc_profile_fini(tmp);
        }
    }
    return pc_engine_working_profile(mode);
}

static pc_swatch_conv *conv_with(const pc_engine_session *s)
{
    pc_swatch_conv *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return NULL;
    }
    pc_profile rgb_tmp;
    bool rgb_owned = false;
    const pc_profile *rgb = working(s, PC_COLOR_MODE_RGB, &rgb_tmp, &rgb_owned);
    pc_transform_options opts = pc_transform_options_default();
    opts.intent = PC_INTENT_RELATIVE;
    opts.bpc = true;

    pc_profile tmp;
    bool owned = false;
    pc_error e = {0};
    const pc_profile *cmyk_src = working(s, PC_COLOR_MODE_CMYK, &tmp, &owned);
    (void)pc_transform_cached(cmyk_src, rgb, opts, &c->cmyk, &e);
    if (owned) {
        pc_profile_fini(&tmp);
    }
    const pc_profile *lab_src = pc_builtin_profile(PC_BUILTIN_LAB_D50);
    (void)pc_transform_cached(lab_src, rgb, opts, &c->lab, &e);
    const pc_profile *gray_src = working(s, PC_COLOR_MODE_GRAYSCALE, &tmp, &owned);
    (void)pc_transform_cached(gray_src, rgb, opts, &c->gray, &e);
    if (owned) {
        pc_profile_fini(&tmp);
    }
    if (rgb_owned) {
        pc_profile_fini(&rgb_tmp);
    }
    return c;
}

pc_swatch_conv *pc_swatch_conv_new(const pc_engine_session *s)
{
    return conv_with(s);
}
pc_swatch_conv *pc_swatch_conv_builtin(void)
{
    return conv_with(NULL);
}

void pc_swatch_conv_free(pc_swatch_conv *c)
{
    if (c == NULL) {
        return;
    }
    pc_transform_release(c->cmyk);
    pc_transform_release(c->lab);
    pc_transform_release(c->gray);
    free(c);
}

static bool eval3(const pc_transform *t, const float *in, float out[3])
{
    if (t == NULL) {
        return false;
    }
    float o[16] = {0};
    pc_transform_eval(t, in, o);
    out[0] = unit_f(o[0]);
    out[1] = unit_f(o[1]);
    out[2] = unit_f(o[2]);
    return true;
}

void pc_swatch_conv_rgb(const pc_swatch_conv *c, const pc_swatch_color *col, float out[3])
{
    switch (col->model) {
    case PC_SWATCH_RGB:
        out[0] = unit_f(col->v[0]);
        out[1] = unit_f(col->v[1]);
        out[2] = unit_f(col->v[2]);
        return;
    case PC_SWATCH_HSB:
        pc_swatch_hsb_to_rgb(col->v, out);
        return;
    case PC_SWATCH_CMYK: {
        float v[4] = {unit_f(col->v[0]), unit_f(col->v[1]), unit_f(col->v[2]), unit_f(col->v[3])};
        if (!eval3(c->cmyk, v, out)) {
            pc_cmyk_to_rgb(v, out);
        }
        return;
    }
    case PC_SWATCH_LAB: {
        float l = finite_f(col->v[0]), a = finite_f(col->v[1]), b = finite_f(col->v[2]);
        float enc[3] = {unit_f(l / 100.0f), unit_f((a + 128.0f) / 255.0f), unit_f((b + 128.0f) / 255.0f)};
        if (!eval3(c->lab, enc, out)) {
            float lab[3] = {l, a, b};
            pc_lab_to_srgb(lab, out);
        }
        return;
    }
    case PC_SWATCH_GRAY: {
        float level = 1.0f - unit_f(col->v[0]);
        float in[1] = {level};
        if (!eval3(c->gray, in, out)) {
            out[0] = out[1] = out[2] = level;
        }
        return;
    }
    default:
        out[0] = out[1] = out[2] = 0.5f;
        return;
    }
}

/* ---- built-in library --------------------------------------------------------------------- */

static pc_status push_rgb(pc_preset_groups *g, size_t gi, const char *name, int r, int grn, int b,
                          pc_error *err)
{
    pc_swatch sw;
    memset(&sw, 0, sizeof sw);
    sw.name = dup_str(name);
    if (sw.name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "swatch");
    }
    sw.color.model = PC_SWATCH_RGB;
    sw.color.v[0] = (float)r / 255.0f;
    sw.color.v[1] = (float)grn / 255.0f;
    sw.color.v[2] = (float)b / 255.0f;
    sw.kind = PC_SWATCH_PROCESS;
    if (pc_preset_group_push(g, gi, &sw, pc_swatch_ops(), err) != PC_OK) {
        pc_swatch_fini(&sw);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

/* Clip a NUL-terminated string in place to at most `maxchars` Unicode scalar values. */
static void utf8_clip(char *s, size_t maxchars)
{
    if (s == NULL) {
        return;
    }
    size_t chars = 0, i = 0;
    while (s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        size_t adv = 1;
        if (c >= 0xf0) {
            adv = 4;
        } else if (c >= 0xe0) {
            adv = 3;
        } else if (c >= 0xc0) {
            adv = 2;
        }
        if (chars == maxchars) {
            s[i] = '\0';
            return;
        }
        chars++;
        for (size_t k = 0; k < adv && s[i] != '\0'; k++) {
            i++;
        }
    }
}

void pc_engine_swatches_cap_library(pc_preset_groups *g)
{
    const pc_preset_item_ops *ops = pc_swatch_ops();
    if (g->n > PC_SWATCH_MAX_GROUPS) {
        for (size_t i = PC_SWATCH_MAX_GROUPS; i < g->n; i++) {
            pc_preset_group *gr = &g->v[i];
            for (size_t j = 0; j < gr->n; j++) {
                ops->fini((char *)gr->items + j * ops->size);
            }
            free(gr->items);
            free(gr->name);
        }
        g->n = PC_SWATCH_MAX_GROUPS;
    }
    size_t left = PC_SWATCH_MAX_SWATCHES;
    for (size_t i = 0; i < g->n; i++) {
        pc_preset_group *gr = &g->v[i];
        if (gr->n > left) {
            for (size_t j = left; j < gr->n; j++) {
                ops->fini((char *)gr->items + j * ops->size);
            }
            gr->n = left;
        }
        left -= gr->n;
        utf8_clip(gr->name, PC_SWATCH_MAX_NAME);
        for (size_t j = 0; j < gr->n; j++) {
            pc_swatch *sw = (pc_swatch *)((char *)gr->items + j * ops->size);
            utf8_clip(sw->name, PC_SWATCH_MAX_NAME);
        }
    }
}

pc_status pc_swatches_builtin(pc_preset_groups *out, pc_error *err)
{
    pc_error e = {0};
    pc_preset_groups_init(out);
    static const char *const hues[10] = {"Red",        "Orange", "Yellow", "Yellow Green", "Green",
                                          "Cyan Green", "Cyan",   "Blue",   "Violet",       "Magenta"};
    static const struct {
        const char *name;
        int v;
    } grays[10] = {{"Black", 0},    {"90% Gray", 26}, {"80% Gray", 51}, {"70% Gray", 77},
                   {"60% Gray", 102}, {"50% Gray", 128}, {"40% Gray", 153}, {"30% Gray", 179},
                   {"20% Gray", 204}, {"White", 255}};
    static const int pastel[10][3] = {{236, 128, 128}, {244, 176, 132}, {250, 224, 128}, {214, 240, 128},
                                      {150, 232, 150}, {128, 232, 200}, {128, 220, 240}, {128, 176, 244},
                                      {168, 144, 244}, {232, 144, 232}};
    static const int pure[10][3] = {{230, 40, 40}, {245, 120, 30}, {250, 210, 30}, {160, 220, 40},
                                    {40, 200, 80}, {30, 200, 170}, {30, 170, 230}, {40, 100, 230},
                                    {120, 70, 220}, {210, 50, 180}};
    static const int dark[10][3] = {{120, 20, 20}, {130, 60, 10}, {130, 110, 10}, {80, 120, 20},
                                    {20, 100, 40}, {10, 100, 90}, {10, 80, 120}, {20, 50, 120},
                                    {60, 30, 110}, {110, 20, 90}};
    static const struct {
        const char *name;
        int r, g, b;
    } exact[6] = {{"RGB Red", 255, 0, 0},     {"RGB Yellow", 255, 255, 0}, {"RGB Green", 0, 255, 0},
                  {"RGB Cyan", 0, 255, 255}, {"RGB Blue", 0, 0, 255},     {"RGB Magenta", 255, 0, 255}};

    if (pc_preset_groups_push(out, "Grays", &e) == NULL) {
        goto oom;
    }
    for (int i = 0; i < 10; i++) {
        if (push_rgb(out, 0, grays[i].name, grays[i].v, grays[i].v, grays[i].v, &e) != PC_OK) {
            goto oom;
        }
    }
    if (pc_preset_groups_push(out, "RGB", &e) == NULL) {
        goto oom;
    }
    for (int i = 0; i < 6; i++) {
        if (push_rgb(out, 1, exact[i].name, exact[i].r, exact[i].g, exact[i].b, &e) != PC_OK) {
            goto oom;
        }
    }
    if (pc_preset_groups_push(out, "Pastel", &e) == NULL) {
        goto oom;
    }
    for (int i = 0; i < 10; i++) {
        char nm[64];
        (void)snprintf(nm, sizeof nm, "Pastel %s", hues[i]);
        if (push_rgb(out, 2, nm, pastel[i][0], pastel[i][1], pastel[i][2], &e) != PC_OK) {
            goto oom;
        }
    }
    if (pc_preset_groups_push(out, "Pure", &e) == NULL) {
        goto oom;
    }
    for (int i = 0; i < 10; i++) {
        if (push_rgb(out, 3, hues[i], pure[i][0], pure[i][1], pure[i][2], &e) != PC_OK) {
            goto oom;
        }
    }
    if (pc_preset_groups_push(out, "Dark", &e) == NULL) {
        goto oom;
    }
    for (int i = 0; i < 10; i++) {
        char nm[64];
        (void)snprintf(nm, sizeof nm, "Dark %s", hues[i]);
        if (push_rgb(out, 4, nm, dark[i][0], dark[i][1], dark[i][2], &e) != PC_OK) {
            goto oom;
        }
    }
    return PC_OK;
oom:
    pc_preset_groups_fini(out, pc_swatch_ops());
    pc_preset_groups_init(out);
    if (err != NULL) {
        *err = e;
    }
    return PC_ERR_NOMEM;
}
