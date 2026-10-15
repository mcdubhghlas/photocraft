/* Gradient presets: the GradientPreset model, built-in gradient groups, JSON, parsing and the
 * Gradient-tool current-gradient stop edits (port of crates/engine/src/presets/gradients.rs, E2b).
 *
 * Built-in colours are our own. Numbers are emitted as f64 reals: serde_json widens every f32 to
 * f64, so the JSON parity depends on the f64 shortest-round-trip form. Never crashes. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "engine_internal.h"
#include "presets.h"

static char *dup_str(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

static float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static int hexval(char c)
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

/* Parse "#rrggbb" or "rrggbb" into rgb 0..1 (bad digits -> 0). */
static void hex_rgb(const char *s, float out[3])
{
    if (s[0] == '#') {
        s++;
    }
    for (int k = 0; k < 3; k++) {
        int hi = hexval(s[k * 2]);
        int lo = hexval(s[k * 2 + 1]);
        int v = (hi < 0 ? 0 : hi) * 16 + (lo < 0 ? 0 : lo);
        out[k] = (float)v / 255.0f;
    }
}

static void hex_of(const float c[3], char buf[8])
{
    unsigned r = (unsigned)lroundf(clamp01(c[0]) * 255.0f);
    unsigned g = (unsigned)lroundf(clamp01(c[1]) * 255.0f);
    unsigned b = (unsigned)lroundf(clamp01(c[2]) * 255.0f);
    (void)snprintf(buf, 8, "#%02x%02x%02x", r & 0xff, g & 0xff, b & 0xff);
}

/* ---- lifecycle + ops ---------------------------------------------------------------------- */

void pc_gradient_preset_fini(pc_gradient_preset *g)
{
    if (g == NULL) {
        return;
    }
    free(g->name);
    free(g->stops);
    free(g->opacity);
    g->name = NULL;
    g->stops = NULL;
    g->opacity = NULL;
    g->nstops = 0;
    g->nopacity = 0;
}

pc_status pc_gradient_preset_clone(const pc_gradient_preset *src, pc_gradient_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->name = dup_str(src->name);
    if (out->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    if (src->nstops > 0) {
        out->stops = malloc(src->nstops * sizeof *out->stops);
        if (out->stops == NULL) {
            pc_gradient_preset_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        memcpy(out->stops, src->stops, src->nstops * sizeof *out->stops);
        out->nstops = src->nstops;
    }
    if (src->nopacity > 0) {
        out->opacity = malloc(src->nopacity * sizeof *out->opacity);
        if (out->opacity == NULL) {
            pc_gradient_preset_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        memcpy(out->opacity, src->opacity, src->nopacity * sizeof *out->opacity);
        out->nopacity = src->nopacity;
    }
    return PC_OK;
}

static const char *g_get_name(const void *item)
{
    return ((const pc_gradient_preset *)item)->name;
}
static bool g_set_name(void *item, const char *name)
{
    char *nm = dup_str(name);
    if (nm == NULL) {
        return false;
    }
    pc_gradient_preset *g = item;
    free(g->name);
    g->name = nm;
    return true;
}
static void g_fini(void *item)
{
    pc_gradient_preset_fini(item);
}
static pc_status g_clone(const void *src, void *dst, pc_error *err)
{
    return pc_gradient_preset_clone(src, dst, err);
}

const pc_preset_item_ops *pc_gradient_preset_ops(void)
{
    static const pc_preset_item_ops ops = {sizeof(pc_gradient_preset), g_get_name, g_set_name, g_fini, g_clone};
    return &ops;
}

/* ---- builders ----------------------------------------------------------------------------- */

/* GradientPreset::new: evenly spaced RGB stops. */
static pc_status grad_new(const char *name, const char *const *hexes, size_t count,
                          pc_gradient_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->name = dup_str(name);
    if (out->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t n = (count < 2 ? 2 : count) - 1;
    out->stops = malloc(count * sizeof *out->stops);
    if (out->stops == NULL) {
        pc_gradient_preset_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    for (size_t i = 0; i < count; i++) {
        out->stops[i].loc = (float)((double)i / (double)n);
        out->stops[i].color.kind = PC_STOP_RGB;
        hex_rgb(hexes[i], out->stops[i].color.rgb);
    }
    out->nstops = count;
    return PC_OK;
}

pc_status pc_gradient_preset_fg_to_bg(pc_gradient_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->name = dup_str("Foreground to Background");
    if (out->name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    out->stops = malloc(2 * sizeof *out->stops);
    if (out->stops == NULL) {
        pc_gradient_preset_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    out->stops[0].loc = 0.0f;
    out->stops[0].color.kind = PC_STOP_FOREGROUND;
    out->stops[1].loc = 1.0f;
    out->stops[1].color.kind = PC_STOP_BACKGROUND;
    out->nstops = 2;
    return PC_OK;
}

/* Append a family "<prefix> NN" of evenly spaced gradients (each a NULL-terminated hex array). */
static pc_status push_family(pc_preset_groups *g, size_t gi, const char *prefix,
                             const char *const *const *sets, size_t nsets, pc_error *err)
{
    for (size_t i = 0; i < nsets; i++) {
        char name[128];
        (void)snprintf(name, sizeof name, "%s %02zu", prefix, i + 1);
        size_t c = 0;
        while (sets[i][c] != NULL) {
            c++;
        }
        pc_gradient_preset gp;
        if (grad_new(name, sets[i], c, &gp, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        if (pc_preset_group_push(g, gi, &gp, pc_gradient_preset_ops(), err) != PC_OK) {
            pc_gradient_preset_fini(&gp);
            return PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

#define FAM(prefix, ...)                                                               \
    do {                                                                               \
        const char *const *const _sets[] = {__VA_ARGS__};                              \
        size_t _n = sizeof _sets / sizeof _sets[0];                                    \
        if (push_family(out, out->n - 1, prefix, _sets, _n, &e) != PC_OK)              \
            goto oom;                                                                  \
    } while (0)

#define HX(...) ((const char *const[]){__VA_ARGS__, NULL})

static pc_status push_one(pc_preset_groups *out, size_t gi, const char *name,
                          const char *const *hexes, pc_error *err)
{
    size_t c = 0;
    while (hexes[c] != NULL) {
        c++;
    }
    pc_gradient_preset gp;
    if (grad_new(name, hexes, c, &gp, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (pc_preset_group_push(out, gi, &gp, pc_gradient_preset_ops(), err) != PC_OK) {
        pc_gradient_preset_fini(&gp);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

pc_status pc_gradients_builtin(pc_preset_groups *out, pc_error *err)
{
    pc_error e = {0};
    pc_preset_groups_init(out);

    /* Basics: Foreground to Background, Foreground to Transparent, Black/White. */
    if (pc_preset_groups_push(out, "Basics", &e) == NULL) {
        goto oom;
    }
    {
        pc_gradient_preset gp;
        if (pc_gradient_preset_fg_to_bg(&gp, &e) != PC_OK) {
            goto oom;
        }
        if (pc_preset_group_push(out, 0, &gp, pc_gradient_preset_ops(), &e) != PC_OK) {
            pc_gradient_preset_fini(&gp);
            goto oom;
        }
        /* Foreground to Transparent. */
        memset(&gp, 0, sizeof gp);
        gp.name = dup_str("Foreground to Transparent");
        gp.stops = malloc(2 * sizeof *gp.stops);
        gp.opacity = malloc(2 * sizeof *gp.opacity);
        if (gp.name == NULL || gp.stops == NULL || gp.opacity == NULL) {
            pc_gradient_preset_fini(&gp);
            goto oom;
        }
        gp.stops[0] = (pc_gradient_cstop){0.0f, {PC_STOP_FOREGROUND, {0, 0, 0}}};
        gp.stops[1] = (pc_gradient_cstop){1.0f, {PC_STOP_FOREGROUND, {0, 0, 0}}};
        gp.nstops = 2;
        gp.opacity[0] = (pc_gradient_ostop){0.0f, 1.0f};
        gp.opacity[1] = (pc_gradient_ostop){1.0f, 0.0f};
        gp.nopacity = 2;
        if (pc_preset_group_push(out, 0, &gp, pc_gradient_preset_ops(), &e) != PC_OK) {
            pc_gradient_preset_fini(&gp);
            goto oom;
        }
        if (push_one(out, 0, "Black, White", HX("#000000", "#ffffff"), &e) != PC_OK) {
            goto oom;
        }
    }

    if (pc_preset_groups_push(out, "Blues", &e) == NULL) {
        goto oom;
    }
    FAM("Blue", HX("#0b3d91", "#4fa3e0"), HX("#1c2a5a", "#3f6fd8", "#a8d4ff"), HX("#00b4d8", "#03045e"),
        HX("#5ec8f2", "#e6f6ff"), HX("#253b80", "#20b2aa"), HX("#89c2ff", "#1d4ed8", "#0a1a40"));

    if (pc_preset_groups_push(out, "Purples", &e) == NULL) {
        goto oom;
    }
    FAM("Purple", HX("#3c096c", "#c77dff"), HX("#5a189a", "#e0aaff"), HX("#240046", "#7b2cbf", "#f3d9ff"),
        HX("#9d4edd", "#4361ee"), HX("#6a0572", "#ab83a1"), HX("#2d0b59", "#ff6fd8"));

    if (pc_preset_groups_push(out, "Pinks", &e) == NULL) {
        goto oom;
    }
    FAM("Pink", HX("#ff4d8d", "#ffd1e1"), HX("#c9184a", "#ff8fab"), HX("#ff70a6", "#ffd670"),
        HX("#8e2c5a", "#f7a1c4", "#fff0f5"), HX("#ff5fa2", "#a855f7"), HX("#ffc2d6", "#ff2e63"));

    if (pc_preset_groups_push(out, "Reds", &e) == NULL) {
        goto oom;
    }
    FAM("Red", HX("#7f0000", "#ff3b30"), HX("#d00000", "#ffba08"), HX("#370617", "#9d0208", "#f48c06"),
        HX("#e5383b", "#f5f3f4"), HX("#a4161a", "#0b090a"), HX("#ff595e", "#ffca3a"));

    if (pc_preset_groups_push(out, "Oranges", &e) == NULL) {
        goto oom;
    }
    FAM("Orange", HX("#ff6d00", "#ffd60a"), HX("#e85d04", "#ffba08"), HX("#9c3d0b", "#ff9e40", "#fff1d6"),
        HX("#ff7b00", "#ff0054"), HX("#fb8500", "#023047"), HX("#ffb703", "#fb5607"));

    if (pc_preset_groups_push(out, "Greens", &e) == NULL) {
        goto oom;
    }
    FAM("Green", HX("#004b23", "#70e000"), HX("#2d6a4f", "#b7e4c7"), HX("#1b4332", "#40916c", "#d8f3dc"),
        HX("#38b000", "#ccff33"), HX("#0b6e4f", "#08a0a8"), HX("#606c38", "#dda15e"));

    if (pc_preset_groups_push(out, "Browns", &e) == NULL) {
        goto oom;
    }
    FAM("Brown", HX("#3e2412", "#a47148"), HX("#6f4518", "#e6ccb2"), HX("#2b1708", "#7f5539", "#ddb892"),
        HX("#8b5e34", "#ffd8a8"), HX("#582f0e", "#b6ad90"), HX("#4a3728", "#c08552", "#f3e9dc"));

    if (pc_preset_groups_push(out, "Grays", &e) == NULL) {
        goto oom;
    }
    FAM("Gray", HX("#212529", "#f8f9fa"), HX("#495057", "#dee2e6"), HX("#000000", "#6c757d", "#ffffff"),
        HX("#343a40", "#adb5bd", "#343a40"), HX("#8d99ae", "#2b2d42"), HX("#e9ecef", "#ced4da", "#868e96"));

    /* Rainbows. */
    if (pc_preset_groups_push(out, "Rainbows", &e) == NULL) {
        goto oom;
    }
    {
        size_t gi = out->n - 1;
        static const char *const rainbow_hex[] = {"#ec1313", "#ec6d13", "#ecc813", "#b6ec13", "#5bec13",
                                                   "#13ec25", "#13ec80", "#13ecda", "#13a4ec", "#1349ec",
                                                   "#3713ec", "#9213ec", "#ec13ec", NULL};
        if (push_one(out, gi, "Rainbow", rainbow_hex, &e) != PC_OK) {
            goto oom;
        }
        if (push_one(out, gi, "Pastel Rainbow",
                     HX("#f4afaf", "#f4cbaf", "#f4e8af", "#e2f4af", "#c6f4af", "#aff4b4", "#aff4d1",
                        "#aff4ee", "#afddf4", "#afc0f4", "#baaff4", "#d7aff4", "#f4aff4"),
                     &e) != PC_OK) {
            goto oom;
        }
        if (push_one(out, gi, "Neon Rainbow",
                     HX("#ff0000", "#ff6a00", "#ffd400", "#bfff00", "#55ff00", "#00ff15", "#00ff80",
                        "#00ffea", "#00aaff", "#0040ff", "#2a00ff", "#9500ff", "#ff00ff"),
                     &e) != PC_OK) {
            goto oom;
        }
        if (push_one(out, gi, "Oil Slick",
                     HX("#0b0f1a", "#1b2a4a", "#0f6f6f", "#1fae8a", "#5ac8a8", "#7a5cff", "#b13bd6",
                        "#e04a8f", "#f2a65a", "#3a2a6b", "#0b0f1a"),
                     &e) != PC_OK) {
            goto oom;
        }
        /* Rainbow Fade: the Rainbow ramp with transparent ends. */
        pc_gradient_preset gp;
        if (grad_new("Rainbow Fade", rainbow_hex, 13, &gp, &e) != PC_OK) {
            goto oom;
        }
        gp.opacity = malloc(4 * sizeof *gp.opacity);
        if (gp.opacity == NULL) {
            pc_gradient_preset_fini(&gp);
            goto oom;
        }
        gp.opacity[0] = (pc_gradient_ostop){0.0f, 0.0f};
        gp.opacity[1] = (pc_gradient_ostop){0.125f, 1.0f};
        gp.opacity[2] = (pc_gradient_ostop){0.875f, 1.0f};
        gp.opacity[3] = (pc_gradient_ostop){1.0f, 0.0f};
        gp.nopacity = 4;
        if (pc_preset_group_push(out, gi, &gp, pc_gradient_preset_ops(), &e) != PC_OK) {
            pc_gradient_preset_fini(&gp);
            goto oom;
        }
    }
    return PC_OK;
oom:
    pc_preset_groups_fini(out, pc_gradient_preset_ops());
    pc_preset_groups_init(out);
    if (err != NULL) {
        *err = e;
    }
    return PC_ERR_NOMEM;
}

/* ---- JSON --------------------------------------------------------------------------------- */

yyjson_mut_val *pc_gradient_preset_to_json(yyjson_mut_doc *d, const pc_gradient_preset *g)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL) {
        return NULL;
    }
    yyjson_mut_obj_add_strcpy(d, o, "name", g->name == NULL ? "" : g->name);
    yyjson_mut_val *stops = yyjson_mut_arr(d);
    for (size_t i = 0; i < g->nstops; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, (double)g->stops[i].loc);
        switch (g->stops[i].color.kind) {
        case PC_STOP_FOREGROUND:
            yyjson_mut_arr_add_strcpy(d, pair, "foreground");
            break;
        case PC_STOP_BACKGROUND:
            yyjson_mut_arr_add_strcpy(d, pair, "background");
            break;
        default: {
            char buf[8];
            hex_of(g->stops[i].color.rgb, buf);
            yyjson_mut_arr_add_strcpy(d, pair, buf);
            break;
        }
        }
        yyjson_mut_arr_append(stops, pair);
    }
    yyjson_mut_obj_add_val(d, o, "stops", stops);
    yyjson_mut_val *op = yyjson_mut_arr(d);
    for (size_t i = 0; i < g->nopacity; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, (double)g->opacity[i].loc);
        yyjson_mut_arr_add_real(d, pair, (double)lroundf(g->opacity[i].opacity * 100.0f));
        yyjson_mut_arr_append(op, pair);
    }
    yyjson_mut_obj_add_val(d, o, "transparency", op);
    return o;
}

/* ---- parsing ------------------------------------------------------------------------------ */

static bool is_hex6(const char *s, size_t len)
{
    if (len > 0 && s[0] == '#') {
        s++;
        len--;
    }
    if (len != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; i++) {
        if (hexval(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

static bool parse_stop_color(const char *s, size_t len, pc_stop_color *out)
{
    if (len == 10 && memcmp(s, "foreground", 10) == 0) {
        out->kind = PC_STOP_FOREGROUND;
        return true;
    }
    if (len == 10 && memcmp(s, "background", 10) == 0) {
        out->kind = PC_STOP_BACKGROUND;
        return true;
    }
    if (is_hex6(s, len)) {
        char tmp[8];
        size_t a = (len > 0 && s[0] == '#') ? 1 : 0;
        memcpy(tmp, s + a, 6);
        tmp[6] = 0;
        out->kind = PC_STOP_RGB;
        hex_rgb(tmp, out->rgb);
        return true;
    }
    return false;
}

static int cmp_cstop(const void *a, const void *b)
{
    float x = ((const pc_gradient_cstop *)a)->loc, y = ((const pc_gradient_cstop *)b)->loc;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static int cmp_ostop(const void *a, const void *b)
{
    float x = ((const pc_gradient_ostop *)a)->loc, y = ((const pc_gradient_ostop *)b)->loc;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* `transparency`: parse into *stops (owned, *n). Returns 1 present, 0 absent, -1 error. */
static int transparency_param(yyjson_val *p, const char *cmd, pc_gradient_ostop **stops, size_t *n,
                              pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "transparency");
    if (v == NULL) {
        return 0;
    }
    if (!yyjson_is_arr(v)) {
        (void)pc_engine_err_bad_params(err, cmd, "`transparency` is [[location, opacity %%], ...]");
        return -1;
    }
    size_t cnt = yyjson_arr_size(v);
    if (cnt > 1024) {
        (void)pc_engine_err_bad_params(err, cmd, "too many opacity stops (max 1024)");
        return -1;
    }
    pc_gradient_ostop *os = cnt > 0 ? malloc(cnt * sizeof *os) : NULL;
    if (cnt > 0 && os == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "opacity");
        return -1;
    }
    size_t idx, max;
    yyjson_val *el;
    size_t w = 0;
    yyjson_arr_foreach(v, idx, max, el) {
        if (!yyjson_is_arr(el) || yyjson_arr_size(el) != 2) {
            free(os);
            (void)pc_engine_err_bad_params(err, cmd, "an opacity stop is [location 0..1, opacity 0..100]");
            return -1;
        }
        double t, o;
        if (!pc_json_as_f64(yyjson_arr_get(el, 0), &t) || !pc_json_as_f64(yyjson_arr_get(el, 1), &o) ||
            !isfinite(t) || !isfinite(o)) {
            free(os);
            (void)pc_engine_err_bad_params(err, cmd, "opacity stops are numbers");
            return -1;
        }
        os[w].loc = clamp01((float)t);
        os[w].opacity = clamp01((float)(o / 100.0));
        w++;
    }
    if (w > 1) {
        qsort(os, w, sizeof *os, cmp_ostop);
    }
    *stops = os;
    *n = w;
    return 1;
}

pc_status pc_gradient_with_transparency(pc_gradient_preset *g, yyjson_val *p, const char *cmd, pc_error *err)
{
    pc_gradient_ostop *os = NULL;
    size_t n = 0;
    int r = transparency_param(p, cmd, &os, &n, err);
    if (r < 0) {
        return PC_ERR_INVALID;
    }
    if (r == 1) {
        free(g->opacity);
        g->opacity = os;
        g->nopacity = n;
    }
    return PC_OK;
}

pc_status pc_gradient_from_params(const char *name, yyjson_val *p, const char *cmd,
                                  pc_gradient_preset *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    yyjson_val *arr = pc_json_get(p, "stops");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return pc_engine_err_bad_params(err, cmd, "missing `stops`");
    }
    size_t cnt = yyjson_arr_size(arr);
    pc_gradient_cstop *stops = cnt > 0 ? malloc(cnt * sizeof *stops) : NULL;
    if (cnt > 0 && stops == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t w = 0, idx, max;
    yyjson_val *el;
    yyjson_arr_foreach(arr, idx, max, el) {
        double t;
        const char *c = NULL;
        size_t clen = 0;
        bool have_t = false;
        if (yyjson_is_arr(el) && yyjson_arr_size(el) == 2) {
            have_t = pc_json_as_f64(yyjson_arr_get(el, 0), &t);
            c = pc_json_as_str(yyjson_arr_get(el, 1), &clen);
        } else {
            c = pc_json_as_str(el, &clen);
            if (c != NULL) {
                t = cnt > 1 ? (double)idx / (double)(cnt - 1) : 0.0;
                have_t = true;
            }
        }
        if (!have_t || c == NULL) {
            free(stops);
            return pc_engine_err_bad_params(
                err, cmd, "stops are [location, \"#rrggbb\"|\"foreground\"|\"background\"]");
        }
        pc_stop_color sc;
        if (!parse_stop_color(c, clen, &sc)) {
            free(stops);
            return pc_engine_err_bad_params(err, cmd, "bad stop colour `%.*s`", (int)clen, c);
        }
        stops[w].loc = clamp01((float)t);
        stops[w].color = sc;
        w++;
    }
    if (w < 2) {
        free(stops);
        return pc_engine_err_bad_params(err, cmd, "a gradient needs at least 2 colour stops");
    }
    qsort(stops, w, sizeof *stops, cmp_cstop);
    out->name = dup_str(name);
    if (out->name == NULL) {
        free(stops);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    out->stops = stops;
    out->nstops = w;
    return pc_gradient_with_transparency(out, p, cmd, err);
}

/* ---- resolve + sampling (for stop edits) -------------------------------------------------- */

static void lerp4(const float a[4], const float b[4], float t, float out[4])
{
    for (int i = 0; i < 4; i++) {
        out[i] = a[i] + (b[i] - a[i]) * t;
    }
}

/* Piecewise-linear RGBA value of sorted stops at t. */
static void sample_rgba(const float (*locs), const float (*cols)[4], size_t n, float t, float out[4])
{
    if (n == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (t <= locs[0]) {
        memcpy(out, cols[0], 4 * sizeof(float));
        return;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (t <= locs[i + 1]) {
            float u = locs[i + 1] > locs[i] ? (t - locs[i]) / (locs[i + 1] - locs[i]) : 1.0f;
            lerp4(cols[i], cols[i + 1], u, out);
            return;
        }
    }
    memcpy(out, cols[n - 1], 4 * sizeof(float));
}

/* RGBA ramp of a preset (colours resolved, sorted, opacity merged). *out owned (free), *n stops. */
static pc_status resolve_ramp(const pc_gradient_preset *g, const float fg[4], const float bg[4],
                              float (**out_locs), float (**out_cols)[4], size_t *out_n, pc_error *err)
{
    size_t nc = g->nstops;
    float *clocs = malloc((nc < 2 ? 2 : nc) * sizeof *clocs);
    float(*cols)[4] = malloc((nc < 2 ? 2 : nc) * sizeof *cols);
    if (clocs == NULL || cols == NULL) {
        free(clocs);
        free(cols);
        return pc_fail(err, PC_ERR_NOMEM, "ramp");
    }
    size_t cn = 0;
    for (size_t i = 0; i < nc; i++) {
        clocs[cn] = clamp01(g->stops[i].loc);
        switch (g->stops[i].color.kind) {
        case PC_STOP_FOREGROUND:
            cols[cn][0] = fg[0];
            cols[cn][1] = fg[1];
            cols[cn][2] = fg[2];
            cols[cn][3] = 1.0f;
            break;
        case PC_STOP_BACKGROUND:
            cols[cn][0] = bg[0];
            cols[cn][1] = bg[1];
            cols[cn][2] = bg[2];
            cols[cn][3] = 1.0f;
            break;
        default:
            cols[cn][0] = g->stops[i].color.rgb[0];
            cols[cn][1] = g->stops[i].color.rgb[1];
            cols[cn][2] = g->stops[i].color.rgb[2];
            cols[cn][3] = 1.0f;
            break;
        }
        cn++;
    }
    /* sort by loc (stable insertion) */
    for (size_t i = 1; i < cn; i++) {
        float kl = clocs[i];
        float kc[4];
        memcpy(kc, cols[i], sizeof kc);
        size_t j = i;
        while (j > 0 && clocs[j - 1] > kl) {
            clocs[j] = clocs[j - 1];
            memcpy(cols[j], cols[j - 1], sizeof kc);
            j--;
        }
        clocs[j] = kl;
        memcpy(cols[j], kc, sizeof kc);
    }
    if (cn == 0) {
        clocs[0] = 0.0f;
        cols[0][0] = fg[0];
        cols[0][1] = fg[1];
        cols[0][2] = fg[2];
        cols[0][3] = 1.0f;
        clocs[1] = 1.0f;
        cols[1][0] = bg[0];
        cols[1][1] = bg[1];
        cols[1][2] = bg[2];
        cols[1][3] = 1.0f;
        cn = 2;
    }
    *out_locs = clocs;
    *out_cols = cols;
    *out_n = cn;
    return PC_OK;
}

/* ---- gradient.presets.stop edit ----------------------------------------------------------- */

static bool opt_u64(yyjson_val *p, const char *key, uint64_t *out)
{
    return pc_json_as_u64(pc_json_get(p, key), out);
}

pc_status pc_gradient_apply_preset_stop(const pc_gradient_preset *g, yyjson_val *p, const float fg[4],
                                        const float bg[4], const char *cmd, pc_gradient_preset *out,
                                        pc_error *err)
{
    if (pc_gradient_preset_clone(g, out, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    size_t alen = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &alen);
    if (action == NULL) {
        pc_gradient_preset_fini(out);
        return pc_engine_err_bad_params(err, cmd, "missing `action` (add|move|delete|color|opacity)");
    }
#define IS(s) (alen == strlen(s) && memcmp(action, s, alen) == 0)
    if (IS("midpoint")) {
        pc_gradient_preset_fini(out);
        return pc_engine_err_bad_params(
            err, cmd,
            "preset gradients keep midpoints at the centre (edit a Gradient Fill layer to move them)");
    }
    size_t klen = 0;
    const char *kind = pc_json_as_str(pc_json_get(p, "kind"), &klen);
    bool opacity_kind;
    if (kind == NULL || (klen == 5 && memcmp(kind, "color", 5) == 0)) {
        opacity_kind = false;
    } else if (klen == 7 && memcmp(kind, "opacity", 7) == 0) {
        opacity_kind = true;
    } else {
        pc_gradient_preset_fini(out);
        return pc_engine_err_bad_params(err, cmd, "unknown kind `%.*s` (color|opacity)", (int)klen, kind);
    }

    float *rlocs = NULL;
    float(*rcols)[4] = NULL;
    size_t rn = 0;
    if (resolve_ramp(g, fg, bg, &rlocs, &rcols, &rn, err) != PC_OK) {
        pc_gradient_preset_fini(out);
        return PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;

    if (opacity_kind) {
        size_t cap = out->nopacity < 2 ? 2 : out->nopacity + 1;
        pc_gradient_ostop *os = malloc(cap * sizeof *os);
        if (os == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "opacity");
            goto done;
        }
        size_t on;
        if (out->nopacity == 0) {
            os[0] = (pc_gradient_ostop){0.0f, 1.0f};
            os[1] = (pc_gradient_ostop){1.0f, 1.0f};
            on = 2;
        } else {
            memcpy(os, out->opacity, out->nopacity * sizeof *os);
            on = out->nopacity;
        }
        if (on > 1) {
            qsort(os, on, sizeof *os, cmp_ostop);
        }
        uint64_t iu;
        double locd, opd;
        bool have_loc = pc_json_as_f64(pc_json_get(p, "location"), &locd) && isfinite(locd);
        bool have_op = pc_json_as_f64(pc_json_get(p, "opacity"), &opd);
        float loc = have_loc ? clamp01((float)locd) : 0.0f;
        float opv = have_op ? clamp01((float)(opd / 100.0)) : 0.0f;
        if (IS("add")) {
            if (!have_loc) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "missing `location` (0..1)");
                goto done;
            }
            float a;
            if (have_op) {
                a = opv;
            } else {
                /* sample opacity from the colour ramp alpha at loc */
                float c[4];
                sample_rgba(rlocs, (const float(*)[4])rcols, rn, loc, c);
                a = c[3];
            }
            os[on++] = (pc_gradient_ostop){loc, a};
        } else if (IS("move")) {
            if (!opt_u64(p, "index", &iu)) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "missing `index` (a stop number from 0)");
                goto done;
            }
            if (!have_loc) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "missing `location` (0..1)");
                goto done;
            }
            if (iu >= on) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "no opacity stop %llu", (unsigned long long)iu);
                goto done;
            }
            os[iu].loc = loc;
        } else if (IS("delete")) {
            if (!opt_u64(p, "index", &iu)) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "missing `index` (a stop number from 0)");
                goto done;
            }
            if (iu >= on) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "no opacity stop %llu", (unsigned long long)iu);
                goto done;
            }
            if (on <= 2) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "a gradient keeps at least 2 opacity stops");
                goto done;
            }
            memmove(&os[iu], &os[iu + 1], (on - iu - 1) * sizeof *os);
            on--;
        } else if (IS("opacity")) {
            if (!opt_u64(p, "index", &iu)) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "missing `index` (a stop number from 0)");
                goto done;
            }
            if (!have_op) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "missing `opacity` (0..100)");
                goto done;
            }
            if (iu >= on) {
                free(os);
                rc = pc_engine_err_bad_params(err, cmd, "no opacity stop %llu", (unsigned long long)iu);
                goto done;
            }
            os[iu].opacity = opv;
        } else {
            free(os);
            rc = pc_engine_err_bad_params(err, cmd,
                                          "unknown action `%.*s` for opacity stops (add|move|delete|opacity)",
                                          (int)alen, action);
            goto done;
        }
        if (on > 1) {
            qsort(os, on, sizeof *os, cmp_ostop);
        }
        if (on > 1024) {
            free(os);
            rc = pc_engine_err_bad_params(err, cmd, "too many opacity stops (max 1024)");
            goto done;
        }
        bool all_opaque = true;
        for (size_t i = 0; i < on; i++) {
            if (fabsf(os[i].opacity - 1.0f) >= 1e-6f) {
                all_opaque = false;
                break;
            }
        }
        if (all_opaque) {
            on = 0;
        }
        free(out->opacity);
        out->opacity = os;
        out->nopacity = on;
        goto done;
    }

    /* colour stops */
    {
        size_t cap = out->nstops + 1;
        pc_gradient_cstop *cs = malloc(cap * sizeof *cs);
        if (cs == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "stops");
            goto done;
        }
        memcpy(cs, out->stops, out->nstops * sizeof *cs);
        size_t cn = out->nstops;
        if (cn > 1) {
            qsort(cs, cn, sizeof *cs, cmp_cstop);
        }
        uint64_t iu;
        double locd;
        bool have_loc = pc_json_as_f64(pc_json_get(p, "location"), &locd) && isfinite(locd);
        float loc = have_loc ? clamp01((float)locd) : 0.0f;
        yyjson_val *colv = pc_json_get(p, "color");
        if (IS("add")) {
            if (!have_loc) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "missing `location` (0..1)");
                goto done;
            }
            pc_stop_color sc;
            if (colv != NULL) {
                size_t sl = 0;
                const char *ss = pc_json_as_str(colv, &sl);
                if (ss == NULL || !parse_stop_color(ss, sl, &sc)) {
                    free(cs);
                    rc = pc_engine_err_bad_params(err, cmd, "a stop colour is \"#rrggbb\", \"foreground\" or \"background\"");
                    goto done;
                }
            } else {
                float c[4];
                sample_rgba(rlocs, (const float(*)[4])rcols, rn, loc, c);
                sc.kind = PC_STOP_RGB;
                sc.rgb[0] = c[0];
                sc.rgb[1] = c[1];
                sc.rgb[2] = c[2];
            }
            cs[cn].loc = loc;
            cs[cn].color = sc;
            cn++;
            if (cn > 1024) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "too many colour stops (max 1024)");
                goto done;
            }
        } else if (IS("move")) {
            if (!opt_u64(p, "index", &iu) || !have_loc) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, !have_loc ? "missing `location` (0..1)" : "missing `index` (a stop number from 0)");
                goto done;
            }
            if (iu >= cn) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "no colour stop %llu", (unsigned long long)iu);
                goto done;
            }
            cs[iu].loc = loc;
        } else if (IS("delete")) {
            if (!opt_u64(p, "index", &iu)) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "missing `index` (a stop number from 0)");
                goto done;
            }
            if (iu >= cn) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "no colour stop %llu", (unsigned long long)iu);
                goto done;
            }
            if (cn <= 2) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "a gradient keeps at least 2 colour stops");
                goto done;
            }
            memmove(&cs[iu], &cs[iu + 1], (cn - iu - 1) * sizeof *cs);
            cn--;
        } else if (IS("color")) {
            if (!opt_u64(p, "index", &iu)) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "missing `index` (a stop number from 0)");
                goto done;
            }
            if (colv == NULL) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "missing `color`");
                goto done;
            }
            size_t sl = 0;
            const char *ss = pc_json_as_str(colv, &sl);
            pc_stop_color sc;
            if (ss == NULL || !parse_stop_color(ss, sl, &sc)) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "a stop colour is \"#rrggbb\", \"foreground\" or \"background\"");
                goto done;
            }
            if (iu >= cn) {
                free(cs);
                rc = pc_engine_err_bad_params(err, cmd, "no colour stop %llu", (unsigned long long)iu);
                goto done;
            }
            cs[iu].color = sc;
        } else {
            free(cs);
            rc = pc_engine_err_bad_params(err, cmd, "unknown action `%.*s` (add|move|delete|color)",
                                          (int)alen, action);
            goto done;
        }
        if (cn > 1) {
            qsort(cs, cn, sizeof *cs, cmp_cstop);
        }
        free(out->stops);
        out->stops = cs;
        out->nstops = cn;
    }
done:
    free(rlocs);
    free(rcols);
    if (rc != PC_OK) {
        pc_gradient_preset_fini(out);
    }
    return rc;
#undef IS
}
