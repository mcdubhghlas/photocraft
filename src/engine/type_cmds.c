/* Type (text) commands and the cross-module type primitives (port of
 * crates/engine/src/type_cmds.rs, E11).
 *
 * Hosts the canvas-geometry content hooks for type layers
 * (pc_engine_hook_transform_text / pc_engine_hook_refresh_text), relocated from hooks.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/json.h"
#include "psd/types.h" /* pc_psd_bytes */
#include "text/engine.h"
#include "text/fonts.h"
#include "text/layout.h"
#include "text/layout_data.h"
#include "text/psd.h"
#include "text/render.h"

#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "type_cmds.h"
#include "type_extra_cmds.h" /* pc_engine_missing_font_warnings_for_layer */

/* ---- norm_text ---------------------------------------------------------------------------- */

char *pc_engine_norm_text(const char *s, size_t len, size_t *out_len)
{
    /* Replace "\r\n" and lone "\r" with "\n". The result is no longer than the input. */
    char *out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r') {
            out[j++] = '\n';
            if (i + 1 < len && s[i + 1] == '\n') {
                i++; /* swallow the \n of a \r\n pair */
            }
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    if (out_len != NULL) {
        *out_len = j;
    }
    return out;
}

/* ---- check_text_unlocked ------------------------------------------------------------------ */

pc_status pc_engine_check_text_unlocked(const pc_document *doc, pc_layer_id id, pc_error *err)
{
    const pc_layer *layer = pc_document_layer(doc, id);
    if (layer == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (pc_document_effective_locks(doc, id).all) {
        return pc_engine_err_other(err, "Could not complete your request because the layer \"%s\" is locked",
                                   pc_str_c(&layer->name));
    }
    return PC_OK;
}

/* ---- refresh ------------------------------------------------------------------------------ */

/* Rebuilds `t`'s PSD TySh block for `layout` (ink = layout bounds in text space). Best effort. */
static void rebuild_tysh(pc_text_layer *t, float dpi, const pc_text_layout *layout)
{
    float ink[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    (void)pc_text_layout_bounds(layout, ink);
    pc_psd_bytes bytes = {NULL, 0};
    if (pc_text_build_tysh(t, dpi, ink, &bytes, NULL) != PC_OK) {
        return;
    }
    pc_blob *blob = NULL;
    if (pc_blob_new(bytes.data, bytes.len, &blob, NULL) == PC_OK) {
        pc_blob_release(t->psd_raw);
        t->psd_raw = blob;
    }
    pc_psd_bytes_fini(&bytes);
}

void pc_engine_type_refresh(const pc_document *doc, pc_text_layer *t)
{
    float dpi = doc->resolution_dpi;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) != PC_OK) {
        return;
    }
    pc_text_layout layout;
    pc_rendered r;
    if (pc_text_engine_render(eng, t, dpi, fmt, &layout, &r, NULL) == PC_OK) {
        if (t->has_cache) {
            pc_surface_fini(&t->cache);
        }
        t->cache = r.surface; /* move out of r; do not pc_rendered_fini */
        t->has_cache = true;
        rebuild_tysh(t, dpi, &layout);
        pc_text_layout_fini(&layout);
    }
    pc_text_engine_shared_unlock();
}

/* retag_text: rebuild a type layer's TySh for its current transform, keeping its pixels. Only
 * when the layer already carries PSD data. */
static void retag_text(float dpi, pc_text_layer *t)
{
    if (t->psd_raw == NULL) {
        return;
    }
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) != PC_OK) {
        return;
    }
    pc_text_layout layout;
    if (pc_text_engine_layout(eng, t, dpi, &layout, NULL) == PC_OK) {
        rebuild_tysh(t, dpi, &layout);
        pc_text_layout_fini(&layout);
    }
    pc_text_engine_shared_unlock();
}

/* ---- replace_text ------------------------------------------------------------------------- */

/* A (len, style) run list over styles of `stride` bytes. Styles are stored by value; char styles
 * own heap data, so a style is cloned/freed through the vtable. */
typedef struct run_vtable {
    size_t stride;
    pc_status (*clone)(const void *src, void *dst, pc_error *err);
    void (*fini)(void *s);
    bool (*eq)(const void *a, const void *b);
} run_vtable;

typedef struct run_list {
    size_t *lens;
    unsigned char *styles; /* *n contiguous styles of vt->stride bytes */
    size_t n, cap;
    const run_vtable *vt;
} run_list;

static void rl_init(run_list *r, const run_vtable *vt)
{
    r->lens = NULL;
    r->styles = NULL;
    r->n = 0;
    r->cap = 0;
    r->vt = vt;
}

static void rl_fini(run_list *r)
{
    for (size_t i = 0; i < r->n; i++) {
        r->vt->fini(r->styles + i * r->vt->stride);
    }
    free(r->lens);
    free(r->styles);
    r->lens = NULL;
    r->styles = NULL;
    r->n = 0;
    r->cap = 0;
}

static void *rl_style(run_list *r, size_t i) { return r->styles + i * r->vt->stride; }

static bool rl_reserve(run_list *r, size_t want)
{
    if (want <= r->cap) {
        return true;
    }
    size_t cap = r->cap == 0 ? 4 : r->cap * 2;
    if (cap < want) {
        cap = want;
    }
    size_t *nl = realloc(r->lens, cap * sizeof *nl);
    if (nl == NULL) {
        return false;
    }
    r->lens = nl;
    unsigned char *ns = realloc(r->styles, cap * r->vt->stride);
    if (ns == NULL) {
        return false;
    }
    r->styles = ns;
    r->cap = cap;
    return true;
}

/* push_merge: extend the last run when its style equals `style`, else append a clone. */
static bool rl_push_merge(run_list *r, size_t len, const void *style)
{
    if (r->n > 0 && r->vt->eq(rl_style(r, r->n - 1), style)) {
        r->lens[r->n - 1] += len;
        return true;
    }
    if (!rl_reserve(r, r->n + 1)) {
        return false;
    }
    if (r->vt->clone(style, rl_style(r, r->n), NULL) != PC_OK) {
        return false;
    }
    r->lens[r->n] = len;
    r->n++;
    return true;
}

static size_t clampsz(size_t v, size_t lo, size_t hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Port of the Rust `adjust` closure: rebuild `src` after replacing bytes [a, b) with `ins` bytes,
 * placing the inserted length in the host run. trailing_paragraph picks a styled final empty run
 * as host. Writes the result into `out` (already rl_init'd). Returns false on OOM. */
static bool rl_adjust(run_list *out, const run_list *src, size_t a, size_t b, size_t ins,
                      bool trailing_paragraph)
{
    size_t total = 0;
    for (size_t i = 0; i < src->n; i++) {
        total += src->lens[i];
    }
    size_t host;
    if (trailing_paragraph && a == b && src->n > 0 && src->lens[src->n - 1] == 0 && a == total) {
        host = src->n - 1;
    } else {
        /* position() of the run containing offset a (Rust's rule). */
        host = 0;
        size_t at = 0;
        bool found = false;
        for (size_t i = 0; i < src->n; i++) {
            size_t s = at, e = at + src->lens[i];
            at = e;
            bool hit = (a == 0) ? (e > 0 || s == 0) : (a > s && a <= e);
            if (hit) {
                host = i;
                found = true;
                break;
            }
        }
        if (!found) {
            host = 0;
        }
    }
    size_t at = 0;
    for (size_t i = 0; i < src->n; i++) {
        size_t rs = at, re = at + src->lens[i];
        at = re;
        size_t keep = (clampsz(a, rs, re) - rs) + (re - clampsz(b, rs, re));
        size_t add = (i == host) ? ins : 0;
        if (!rl_push_merge(out, keep + add, (const unsigned char *)src->styles + i * src->vt->stride)) {
            return false;
        }
    }
    return true;
}

static pc_status char_clone(const void *src, void *dst, pc_error *err)
{
    return pc_char_style_clone(src, dst, err);
}
static void char_fini(void *s) { pc_char_style_fini(s); }
static bool char_eq(const void *a, const void *b) { return pc_char_style_eq(a, b); }
static pc_status para_clone(const void *src, void *dst, pc_error *err)
{
    (void)err;
    memcpy(dst, src, sizeof(pc_paragraph_style));
    return PC_OK;
}
static void para_fini(void *s) { (void)s; }
static bool para_eq(const void *a, const void *b) { return pc_paragraph_style_eq(a, b); }

static const run_vtable CHAR_VT = {sizeof(pc_char_style), char_clone, char_fini, char_eq};
static const run_vtable PARA_VT = {sizeof(pc_paragraph_style), para_clone, para_fini, para_eq};

static bool str_ends_break(const pc_str *s)
{
    return s->len > 0 && (s->ptr[s->len - 1] == '\r' || s->ptr[s->len - 1] == '\n');
}

void pc_engine_replace_text(pc_text_layer *t, size_t a, size_t b, const char *new_, size_t new_len)
{
    size_t tlen = t->text.len;
    a = a < tlen ? a : tlen;
    size_t bmin = b < tlen ? b : tlen;
    b = bmin > a ? bmin : a;

    size_t norm_len = 0;
    char *norm = pc_engine_norm_text(new_, new_len, &norm_len);
    if (norm == NULL) {
        return;
    }

    /* Load the current char and paragraph runs. */
    pc_text_run *cruns = NULL;
    size_t ncr = 0;
    pc_paragraph_run *pruns = NULL;
    size_t npr = 0;
    if (pc_text_layer_char_runs(t, &cruns, &ncr, NULL) != PC_OK ||
        pc_text_layer_paragraph_runs(t, &pruns, &npr, NULL) != PC_OK) {
        pc_text_runs_free(cruns, ncr);
        free(pruns);
        free(norm);
        return;
    }

    run_list cr, pr, cr2, pr2;
    rl_init(&cr, &CHAR_VT);
    rl_init(&pr, &PARA_VT);
    rl_init(&cr2, &CHAR_VT);
    rl_init(&pr2, &PARA_VT);
    bool ok = true;
    for (size_t i = 0; i < ncr && ok; i++) {
        ok = rl_push_merge(&cr, cruns[i].len, &cruns[i].style);
    }
    for (size_t i = 0; i < npr && ok; i++) {
        ok = rl_push_merge(&pr, pruns[i].len, &pruns[i].style);
    }

    /* first_c / first_p defaults for an empty result. */
    pc_char_style first_c;
    pc_char_style_default(&first_c);
    pc_paragraph_style first_p = pc_paragraph_style_default();
    if (ncr > 0) {
        pc_char_style_fini(&first_c);
        if (pc_char_style_clone(&cruns[0].style, &first_c, NULL) != PC_OK) {
            pc_char_style_default(&first_c);
        }
    }
    if (npr > 0) {
        first_p = pruns[0].style;
    }

    /* A styled trailing empty paragraph is remembered when the text ends with a break. */
    bool had_trailing = str_ends_break(&t->text);
    bool have_trailing_style = false;
    pc_paragraph_style trailing_style = pc_paragraph_style_default();
    if (had_trailing && npr > 0 && pruns[npr - 1].len == 0) {
        trailing_style = pruns[npr - 1].style;
        have_trailing_style = true;
    }

    if (ok) {
        ok = rl_adjust(&cr2, &cr, a, b, norm_len, false);
    }
    if (ok) {
        ok = rl_adjust(&pr2, &pr, a, b, norm_len, true);
    }

    /* Splice the text: text[0..a] + norm + text[b..]. */
    size_t new_total = a + norm_len + (tlen - b);
    char *buf = ok ? malloc(new_total + 1) : NULL;
    if (buf != NULL) {
        if (t->text.ptr != NULL) {
            memcpy(buf, t->text.ptr, a);
            memcpy(buf + a + norm_len, t->text.ptr + b, tlen - b);
        }
        memcpy(buf + a, norm, norm_len);
        buf[new_total] = '\0';
        if (pc_str_set(&t->text, buf, new_total, NULL) != PC_OK) {
            ok = false;
        }
        free(buf);
    } else {
        ok = false;
    }

    if (ok && str_ends_break(&t->text) && have_trailing_style) {
        ok = rl_push_merge(&pr2, 0, &trailing_style);
    }
    if (ok && cr2.n == 0) {
        ok = rl_push_merge(&cr2, t->text.len, &first_c);
    }
    if (ok && pr2.n == 0) {
        ok = rl_push_merge(&pr2, t->text.len, &first_p);
    }

    if (ok) {
        /* Commit the new runs into the layer. */
        pc_text_run *nr = malloc(cr2.n * sizeof *nr);
        pc_paragraph_run *np = malloc(pr2.n * sizeof *np);
        if (nr != NULL && np != NULL) {
            for (size_t i = 0; i < cr2.n; i++) {
                nr[i].len = cr2.lens[i];
                pc_char_style_default(&nr[i].style);
                pc_char_style_fini(&nr[i].style);
                if (pc_char_style_clone(rl_style(&cr2, i), &nr[i].style, NULL) != PC_OK) {
                    pc_char_style_default(&nr[i].style);
                }
            }
            for (size_t i = 0; i < pr2.n; i++) {
                np[i].len = pr2.lens[i];
                np[i].style = *(pc_paragraph_style *)rl_style(&pr2, i);
            }
            pc_text_runs_free(t->runs, t->nruns);
            free(t->paragraphs);
            t->runs = nr;
            t->nruns = cr2.n;
            t->paragraphs = np;
            t->nparagraphs = pr2.n;
            (void)pc_text_layer_sync_summary(t, NULL);
        } else {
            free(nr);
            free(np);
        }
    }

    pc_char_style_fini(&first_c);
    rl_fini(&cr);
    rl_fini(&pr);
    rl_fini(&cr2);
    rl_fini(&pr2);
    pc_text_runs_free(cruns, ncr);
    free(pruns);
    free(norm);
}

/* ---- relocated canvas_geom content hooks (were E1-safe noops in hooks.c) ------------------- */

void pc_engine_hook_transform_text(pc_layer *l, const pc_affine *a, float dpi)
{
    if (l->content.kind != PC_LAYER_TEXT) {
        return;
    }
    pc_text_layer *t = &l->content.u.text;
    t->transform = pc_affine_mul(*a, t->transform);
    retag_text(dpi, t);
}

void pc_engine_hook_refresh_text(const pc_document *snapshot, pc_layer *l)
{
    if (l->content.kind == PC_LAYER_TEXT) {
        pc_engine_type_refresh(snapshot, &l->content.u.text);
    }
}

/* ============================================================================================
 * Command surface (port of the type_cmds.rs command specs and their helpers). Routes through
 * src/text (shaping, layout, fonts) and the src/doc text model. Offsets in params and results
 * are character indices; this layer converts them to the UTF-8 byte offsets the primitives use.
 * ========================================================================================== */

/* ---- char / byte index conversion --------------------------------------------------------- */

size_t pc_engine_text_byte_at(const char *text, size_t len, size_t ci)
{
    size_t i = 0, c = 0;
    while (i < len && c < ci) {
        i++;
        while (i < len && ((unsigned char)text[i] & 0xC0) == 0x80) {
            i++;
        }
        c++;
    }
    return i;
}

size_t pc_engine_text_char_at(const char *text, size_t len, size_t bi)
{
    size_t end = bi < len ? bi : len, c = 0;
    for (size_t i = 0; i < end; i++) {
        if (((unsigned char)text[i] & 0xC0) != 0x80) {
            c++;
        }
    }
    return c;
}

/* ---- small param readers ------------------------------------------------------------------ */

static bool f32p(yyjson_val *p, const char *k, float *out)
{
    double v;
    if (!pc_json_as_f64(pc_json_get(p, k), &v)) {
        return false;
    }
    *out = (float)v;
    return true;
}

static int hex2(const char *s)
{
    int hi = -1, lo = -1;
    char a = s[0], b = s[1];
    if (a >= '0' && a <= '9') hi = a - '0';
    else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
    else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
    if (b >= '0' && b <= '9') lo = b - '0';
    else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
    else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
    if (hi < 0 || lo < 0) {
        return -1;
    }
    return hi * 16 + lo;
}

/* Rust type_cmds::color. */
static bool color_val(yyjson_val *v, pc_color *out)
{
    if (v == NULL) {
        return false;
    }
    if (yyjson_is_arr(v)) {
        size_t n = yyjson_arr_size(v);
        if (n < 3) {
            return false;
        }
        double c[4] = {0.0, 0.0, 0.0, 1.0};
        for (size_t i = 0; i < 4 && i < n; i++) {
            double x;
            c[i] = pc_json_as_f64(yyjson_arr_get(v, i), &x) ? x : (i == 3 ? 1.0 : 0.0);
        }
        *out = pc_color_rgba((float)c[0], (float)c[1], (float)c[2], n >= 4 ? (float)c[3] : 1.0f);
        return true;
    }
    if (yyjson_is_str(v)) {
        size_t len = 0;
        const char *s = pc_json_as_str(v, &len);
        if (s == NULL) {
            return false;
        }
        while (len > 0 && *s == '#') {
            s++;
            len--;
        }
        if (len == 6 || len == 8) {
            int r = hex2(s), g = hex2(s + 2), b = hex2(s + 4);
            if (r < 0 || g < 0 || b < 0) {
                return false;
            }
            int a = 255;
            if (len == 8) {
                a = hex2(s + 6);
                if (a < 0) {
                    return false;
                }
            }
            *out = pc_color_rgba((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, (float)a / 255.0f);
            return true;
        }
    }
    return false;
}

/* ---- validators (check_kerning / check_size_tracking / check_steps) ----------------------- */

static bool ascii_ieq(const char *s, size_t len, const char *lit)
{
    size_t i = 0;
    for (; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (lit[i] == '\0' || c != lit[i]) {
            return false;
        }
    }
    return lit[i] == '\0';
}

bool pc_engine_check_kerning(yyjson_val *p, char *emsg, size_t cap)
{
    yyjson_val *v = pc_json_get(p, "kerning");
    if (v == NULL) {
        return true;
    }
    if (yyjson_is_num(v)) {
        double k;
        if (pc_json_as_f64(v, &k) && k >= PC_TYPE_KERN_MIN && k <= PC_TYPE_KERN_MAX) {
            return true;
        }
        snprintf(emsg, cap, "kerning must be between %g and %g (1/1000 em)", PC_TYPE_KERN_MIN, PC_TYPE_KERN_MAX);
        return false;
    }
    if (yyjson_is_str(v)) {
        size_t len = 0;
        const char *s = pc_json_as_str(v, &len);
        if (s != NULL && (ascii_ieq(s, len, "metrics") || ascii_ieq(s, len, "optical") || ascii_ieq(s, len, "off") ||
                          ascii_ieq(s, len, "none") || ascii_ieq(s, len, "0"))) {
            return true;
        }
    }
    snprintf(emsg, cap, "kerning must be a number (1/1000 em) or \"metrics\", \"optical\" or \"off\"");
    return false;
}

bool pc_engine_check_size_tracking(yyjson_val *p, char *emsg, size_t cap)
{
    yyjson_val *v = pc_json_get(p, "size");
    if (v != NULL) {
        double x;
        if (!(pc_json_as_f64(v, &x) && isfinite(x) && x >= PC_TYPE_SIZE_MIN_PT && x <= PC_TYPE_SIZE_MAX_PT)) {
            snprintf(emsg, cap, "size must be between %g and %g pt", PC_TYPE_SIZE_MIN_PT, PC_TYPE_SIZE_MAX_PT);
            return false;
        }
    }
    v = pc_json_get(p, "tracking");
    if (v != NULL) {
        double x;
        if (!(pc_json_as_f64(v, &x) && isfinite(x) && x >= PC_TYPE_TRACKING_MIN && x <= PC_TYPE_TRACKING_MAX)) {
            snprintf(emsg, cap, "tracking must be between %g and %g (1/1000 em)", PC_TYPE_TRACKING_MIN,
                     PC_TYPE_TRACKING_MAX);
            return false;
        }
    }
    return true;
}

bool pc_engine_check_steps(yyjson_val *p, char *emsg, size_t cap)
{
    static const struct {
        const char *key;
        double max;
    } steps[3] = {{"leadingBy", 5000.0}, {"baselineShiftBy", 10000.0}, {"trackingBy", 11000.0}};
    for (size_t i = 0; i < 3; i++) {
        yyjson_val *v = pc_json_get(p, steps[i].key);
        if (v == NULL) {
            continue;
        }
        double by;
        if (!(pc_json_as_f64(v, &by) && isfinite(by) && fabs(by) <= steps[i].max)) {
            snprintf(emsg, cap, "%s must be a number between -%g and %g", steps[i].key, steps[i].max, steps[i].max);
            return false;
        }
    }
    return true;
}

/* ---- font resolution helpers (installed_postscript / fontStyle guess / named_face) -------- */

/* The PostScript name of the installed face of `family` at exactly weight/italic, into `out`.
 * Returns true when such a face has a PostScript name. */
static bool installed_postscript(const char *family, uint16_t weight, bool italic, pc_str *out)
{
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) != PC_OK) {
        return false;
    }
    pc_fontdb *db = pc_text_engine_fonts(eng);
    pc_face_info *faces = NULL;
    size_t n = 0;
    bool got = false;
    if (pc_fontdb_faces(db, family, strlen(family), &faces, &n, NULL) == PC_OK) {
        for (size_t i = 0; i < n; i++) {
            if (faces[i].italic == italic && (uint16_t)lround(faces[i].weight) == weight && faces[i].postscript_name) {
                if (pc_str_set_c(out, faces[i].postscript_name, NULL) == PC_OK) {
                    got = true;
                }
                break;
            }
        }
        pc_face_infos_free(faces, n);
    }
    pc_text_engine_shared_unlock();
    return got;
}

static void char_set_family(pc_char_style *s, const char *v, size_t len)
{
    (void)pc_str_set(&s->font_family, v, len, NULL);
    s->has_postscript_name = false;
    pc_str_fini(&s->postscript_name);
}

static void char_clear_style_name(pc_char_style *s)
{
    pc_str_fini(&s->font_style);
}

static void char_clear_postscript(pc_char_style *s)
{
    s->has_postscript_name = false;
    pc_str_fini(&s->postscript_name);
}

static void char_remove_variation(pc_char_style *s, const char *axis)
{
    for (size_t i = 0; i < s->nvariations;) {
        if (strcmp(pc_str_c(&s->variations[i].axis), axis) == 0) {
            pc_str_fini(&s->variations[i].axis);
            memmove(&s->variations[i], &s->variations[i + 1], (s->nvariations - i - 1) * sizeof s->variations[0]);
            s->nvariations--;
        } else {
            i++;
        }
    }
}

static void char_push_variation(pc_char_style *s, const char *axis, size_t alen, float value)
{
    pc_font_variation *nv = realloc(s->variations, (s->nvariations + 1) * sizeof *nv);
    if (nv == NULL) {
        return;
    }
    s->variations = nv;
    pc_font_variation *f = &s->variations[s->nvariations];
    f->axis = (pc_str){0};
    if (pc_str_set(&f->axis, axis, alen, NULL) != PC_OK) {
        return;
    }
    f->value = value;
    s->nvariations++;
}

static void char_remove_feature(pc_char_style *s, const char *tag, size_t tlen)
{
    for (size_t i = 0; i < s->nfeatures;) {
        if (strlen(pc_str_c(&s->features[i].tag)) == tlen && memcmp(pc_str_c(&s->features[i].tag), tag, tlen) == 0) {
            pc_str_fini(&s->features[i].tag);
            memmove(&s->features[i], &s->features[i + 1], (s->nfeatures - i - 1) * sizeof s->features[0]);
            s->nfeatures--;
        } else {
            i++;
        }
    }
}

static void char_push_feature(pc_char_style *s, const char *tag, size_t tlen, uint16_t value)
{
    pc_font_feature *nf = realloc(s->features, (s->nfeatures + 1) * sizeof *nf);
    if (nf == NULL) {
        return;
    }
    s->features = nf;
    pc_font_feature *f = &s->features[s->nfeatures];
    f->tag = (pc_str){0};
    if (pc_str_set(&f->tag, tag, tlen, NULL) != PC_OK) {
        return;
    }
    f->value = value;
    s->nfeatures++;
}

bool pc_engine_apply_char_props(pc_char_style *s, yyjson_val *p)
{
    bool any = false;
    bool font_key = pc_json_get(p, "font") || pc_json_get(p, "family") || pc_json_get(p, "fontStyle") ||
                    pc_json_get(p, "weight") || pc_json_get(p, "italic");

    yyjson_val *fv = pc_json_get(p, "font");
    if (fv == NULL) {
        fv = pc_json_get(p, "family");
    }
    size_t flen = 0;
    const char *fs = pc_json_as_str(fv, &flen);
    if (fs != NULL) {
        char_set_family(s, fs, flen);
        any = true;
    }

    size_t stlen = 0;
    const char *style = pc_json_as_str(pc_json_get(p, "fontStyle"), &stlen);
    if (style != NULL) {
        (void)pc_str_set(&s->font_style, style, stlen, NULL);
        char_clear_postscript(s);
        char_remove_variation(s, "wght");
        /* italic by substring of the lowercased style. */
        bool italic = false;
        for (size_t i = 0; i + 6 <= stlen; i++) {
            if (ascii_ieq(style + i, 6, "italic")) {
                italic = true;
                break;
            }
        }
        if (!italic) {
            for (size_t i = 0; i + 7 <= stlen; i++) {
                if (ascii_ieq(style + i, 7, "oblique")) {
                    italic = true;
                    break;
                }
            }
        }
        s->italic = italic;
        /* weight from guess_from_postscript("X-<style without spaces>"). */
        char buf[160];
        size_t j = 0;
        buf[j++] = 'X';
        buf[j++] = '-';
        for (size_t i = 0; i < stlen && j + 1 < sizeof buf; i++) {
            if (style[i] != ' ') {
                buf[j++] = style[i];
            }
        }
        buf[j] = '\0';
        pc_resolved_font rf = {0};
        if (pc_text_guess_from_postscript(buf, j, &rf, NULL) == PC_OK) {
            s->weight = rf.weight;
            pc_resolved_font_fini(&rf);
        }
        /* An installed named face overrides weight/italic/postscript. */
        pc_text_engine *eng = NULL;
        if (pc_text_engine_shared_lock(&eng, NULL) == PC_OK) {
            pc_face_info *face = NULL;
            bool found = false;
            char fam[256];
            size_t fl = s->font_family.len < sizeof fam - 1 ? s->font_family.len : sizeof fam - 1;
            memcpy(fam, pc_str_c(&s->font_family), fl);
            fam[fl] = '\0';
            char sty[160];
            size_t sl = stlen < sizeof sty - 1 ? stlen : sizeof sty - 1;
            memcpy(sty, style, sl);
            sty[sl] = '\0';
            if (pc_fontdb_named_face(pc_text_engine_fonts(eng), fam, sty, &face, &found, NULL) == PC_OK && found) {
                s->weight = (uint16_t)lround(face->weight);
                s->italic = face->italic;
                if (face->postscript_name) {
                    if (pc_str_set_c(&s->postscript_name, face->postscript_name, NULL) == PC_OK) {
                        s->has_postscript_name = true;
                    }
                }
                pc_face_infos_free(face, 1);
            }
            pc_text_engine_shared_unlock();
        }
        any = true;
    }

    double wv;
    if (pc_json_as_f64(pc_json_get(p, "weight"), &wv)) {
        double cl = wv < 1.0 ? 1.0 : wv > 1000.0 ? 1000.0 : wv;
        uint16_t weight = (uint16_t)cl;
        if (s->weight != weight) {
            char_clear_postscript(s);
            char_clear_style_name(s);
        }
        char_remove_variation(s, "wght");
        s->weight = weight;
        any = true;
    }

    bool bv;
    if (pc_json_as_bool(pc_json_get(p, "italic"), &bv)) {
        if (s->italic != bv) {
            char_clear_postscript(s);
            char_clear_style_name(s);
        }
        s->italic = bv;
        any = true;
    }

    size_t pslen = 0;
    const char *ps = pc_json_as_str(pc_json_get(p, "postscriptName"), &pslen);
    if (ps != NULL) {
        if (pc_str_set(&s->postscript_name, ps, pslen, NULL) == PC_OK) {
            s->has_postscript_name = true;
        }
        any = true;
    }

    if (font_key && !s->has_postscript_name) {
        char fam[256];
        size_t fl = s->font_family.len < sizeof fam - 1 ? s->font_family.len : sizeof fam - 1;
        memcpy(fam, pc_str_c(&s->font_family), fl);
        fam[fl] = '\0';
        if (installed_postscript(fam, s->weight, s->italic, &s->postscript_name)) {
            s->has_postscript_name = true;
        }
    }

    float sz;
    if (f32p(p, "size", &sz)) {
        s->size_pt = sz > 0.01f ? sz : 0.01f;
        any = true;
    }
    pc_color col;
    if (color_val(pc_json_get(p, "color"), &col)) {
        s->color = col;
        any = true;
    }
    float tr;
    if (f32p(p, "tracking", &tr)) {
        s->tracking = tr;
        any = true;
    }
    yyjson_val *lead = pc_json_get(p, "leading");
    if (lead != NULL) {
        if (yyjson_is_null(lead)) {
            s->has_leading = false;
            any = true;
        } else if (yyjson_is_str(lead)) {
            size_t ll = 0;
            const char *ls = pc_json_as_str(lead, &ll);
            if (ls != NULL && ll == 4 && memcmp(ls, "auto", 4) == 0) {
                s->has_leading = false;
                any = true;
            }
        } else if (yyjson_is_num(lead)) {
            double lv;
            if (pc_json_as_f64(lead, &lv)) {
                s->has_leading = true;
                s->leading_pt = (float)lv;
                any = true;
            }
        }
    }
    float v;
    if (f32p(p, "baselineShift", &v)) {
        s->baseline_shift_pt = v;
        any = true;
    }
    if (f32p(p, "horizontalScale", &v)) {
        float x = v / 100.0f;
        s->horizontal_scale = x > 0.01f ? x : 0.01f;
        any = true;
    }
    if (f32p(p, "verticalScale", &v)) {
        float x = v / 100.0f;
        s->vertical_scale = x > 0.01f ? x : 0.01f;
        any = true;
    }
    static const struct {
        const char *k;
        size_t off;
    } flags[6] = {{"underline", offsetof(pc_char_style, underline)},
                  {"strikethrough", offsetof(pc_char_style, strikethrough)},
                  {"fauxBold", offsetof(pc_char_style, faux_bold)},
                  {"fauxItalic", offsetof(pc_char_style, faux_italic)},
                  {"ligatures", offsetof(pc_char_style, ligatures)},
                  {"discretionaryLigatures", offsetof(pc_char_style, discretionary_ligatures)}};
    for (size_t i = 0; i < 6; i++) {
        bool b;
        if (pc_json_as_bool(pc_json_get(p, flags[i].k), &b)) {
            *(bool *)((char *)s + flags[i].off) = b;
            any = true;
        }
    }
    yyjson_val *kv = pc_json_get(p, "kerning");
    if (kv != NULL) {
        if (yyjson_is_num(kv)) {
            double k;
            if (pc_json_as_f64(kv, &k) && isfinite(k)) {
                double cl = k < PC_TYPE_KERN_MIN ? PC_TYPE_KERN_MIN : k > PC_TYPE_KERN_MAX ? PC_TYPE_KERN_MAX : k;
                s->kerning = PC_KERNING_OFF;
                s->kern = (float)cl;
                any = true;
            }
        } else if (yyjson_is_str(kv)) {
            size_t kl = 0;
            const char *ks = pc_json_as_str(kv, &kl);
            if (ks != NULL) {
                if (ascii_ieq(ks, kl, "optical")) {
                    s->kerning = PC_KERNING_OPTICAL;
                } else if (ascii_ieq(ks, kl, "none") || ascii_ieq(ks, kl, "off") || ascii_ieq(ks, kl, "0")) {
                    s->kerning = PC_KERNING_OFF;
                } else {
                    s->kerning = PC_KERNING_METRICS;
                }
                s->kern = 0.0f;
                any = true;
            }
        }
    }
    size_t cl = 0;
    const char *caps = pc_json_as_str(pc_json_get(p, "caps"), &cl);
    if (caps != NULL) {
        if ((cl == 5 && memcmp(caps, "small", 5) == 0) || (cl == 9 && memcmp(caps, "smallCaps", 9) == 0)) {
            s->caps = PC_CAPS_SMALL_CAPS;
        } else if ((cl == 3 && memcmp(caps, "all", 3) == 0) || (cl == 7 && memcmp(caps, "allCaps", 7) == 0)) {
            s->caps = PC_CAPS_ALL_CAPS;
        } else {
            s->caps = PC_CAPS_NORMAL;
        }
        any = true;
    }
    yyjson_val *feats = pc_json_get(p, "features");
    if (feats != NULL && yyjson_is_obj(feats)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(feats, idx, max, key, val) {
            size_t tl = 0;
            const char *tag = pc_json_as_str(key, &tl);
            if (tag == NULL) {
                continue;
            }
            uint64_t u;
            bool bb;
            uint16_t value = 1;
            if (pc_json_as_u64(val, &u)) {
                value = (uint16_t)u;
            } else if (pc_json_as_bool(val, &bb)) {
                value = bb ? 1 : 0;
            }
            char_remove_feature(s, tag, tl);
            char_push_feature(s, tag, tl, value);
        }
        any = true;
    }
    yyjson_val *vars = pc_json_get(p, "variations");
    if (vars != NULL && yyjson_is_obj(vars)) {
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(vars, idx, max, key, val) {
            size_t al = 0;
            const char *axis = pc_json_as_str(key, &al);
            if (axis == NULL) {
                continue;
            }
            char_remove_variation(s, axis);
            double x;
            if (pc_json_as_f64(val, &x)) {
                char_push_variation(s, axis, al, (float)x);
            }
        }
        any = true;
    }
    yyjson_val *lang = pc_json_get(p, "language");
    if (lang != NULL) {
        size_t ll = 0;
        const char *ls = pc_json_as_str(lang, &ll);
        if (ls != NULL) {
            if (pc_str_set(&s->language, ls, ll, NULL) == PC_OK) {
                s->has_language = true;
            }
        } else {
            s->has_language = false;
            pc_str_fini(&s->language);
        }
        any = true;
    }
    return any;
}

bool pc_engine_apply_para_props(pc_paragraph_style *s, yyjson_val *p)
{
    bool any = false;
    size_t al = 0;
    const char *align = pc_json_as_str(pc_json_get(p, "align"), &al);
    if (align != NULL) {
        if (al == 6 && memcmp(align, "center", 6) == 0) {
            s->align = PC_TEXT_ALIGN_CENTER;
        } else if (al == 5 && memcmp(align, "right", 5) == 0) {
            s->align = PC_TEXT_ALIGN_RIGHT;
        } else if ((al == 7 && memcmp(align, "justify", 7) == 0) || (al == 11 && memcmp(align, "justifyLeft", 11) == 0)) {
            s->align = PC_TEXT_ALIGN_JUSTIFY_LEFT;
        } else if (al == 13 && memcmp(align, "justifyCenter", 13) == 0) {
            s->align = PC_TEXT_ALIGN_JUSTIFY_CENTER;
        } else if (al == 12 && memcmp(align, "justifyRight", 12) == 0) {
            s->align = PC_TEXT_ALIGN_JUSTIFY_RIGHT;
        } else if (al == 10 && memcmp(align, "justifyAll", 10) == 0) {
            s->align = PC_TEXT_ALIGN_JUSTIFY_ALL;
        } else {
            s->align = PC_TEXT_ALIGN_LEFT;
        }
        any = true;
    }
    static const struct {
        const char *k;
        size_t off;
        bool pct;
    } keys[6] = {{"firstLineIndent", offsetof(pc_paragraph_style, first_line_indent_pt), false},
                 {"startIndent", offsetof(pc_paragraph_style, start_indent_pt), false},
                 {"endIndent", offsetof(pc_paragraph_style, end_indent_pt), false},
                 {"spaceBefore", offsetof(pc_paragraph_style, space_before_pt), false},
                 {"spaceAfter", offsetof(pc_paragraph_style, space_after_pt), false},
                 {"autoLeading", offsetof(pc_paragraph_style, auto_leading), true}};
    for (size_t i = 0; i < 6; i++) {
        float v;
        if (f32p(p, keys[i].k, &v)) {
            *(float *)((char *)s + keys[i].off) = keys[i].pct ? v / 100.0f : v;
            any = true;
        }
    }
    size_t dl = 0;
    const char *dir = pc_json_as_str(pc_json_get(p, "direction"), &dl);
    if (dir != NULL) {
        if (dl == 3 && memcmp(dir, "ltr", 3) == 0) {
            s->direction = PC_TEXT_DIR_LTR;
        } else if (dl == 3 && memcmp(dir, "rtl", 3) == 0) {
            s->direction = PC_TEXT_DIR_RTL;
        } else {
            s->direction = PC_TEXT_DIR_AUTO;
        }
        any = true;
    }
    bool hy;
    if (pc_json_as_bool(pc_json_get(p, "hyphenate"), &hy)) {
        s->hyphenate = hy;
        any = true;
    }
    return any;
}

/* ---- style_range / style_paragraphs ------------------------------------------------------- */

/* Replace t->runs from a CHAR_VT run_list (clones each style). */
static void commit_char_runs(pc_text_layer *t, run_list *rl)
{
    if (rl->n == 0) {
        return;
    }
    pc_text_run *nr = malloc(rl->n * sizeof *nr);
    if (nr == NULL) {
        return;
    }
    for (size_t i = 0; i < rl->n; i++) {
        nr[i].len = rl->lens[i];
        pc_char_style_default(&nr[i].style);
        pc_char_style_fini(&nr[i].style);
        if (pc_char_style_clone(rl_style(rl, i), &nr[i].style, NULL) != PC_OK) {
            pc_char_style_default(&nr[i].style);
        }
    }
    pc_text_runs_free(t->runs, t->nruns);
    t->runs = nr;
    t->nruns = rl->n;
    (void)pc_text_layer_sync_summary(t, NULL);
}

static bool push_merge_nz(run_list *r, size_t len, const void *style)
{
    return len == 0 ? true : rl_push_merge(r, len, style);
}

void pc_engine_style_range(pc_text_layer *t, size_t a, size_t b, pc_engine_char_style_mut_fn f, void *ud)
{
    if (b < a) {
        b = a;
    }
    pc_text_run *runs = NULL;
    size_t n = 0;
    if (pc_text_layer_char_runs(t, &runs, &n, NULL) != PC_OK) {
        return;
    }
    if (t->text.len == 0) {
        pc_char_style st;
        if (n > 0) {
            if (pc_char_style_clone(&runs[0].style, &st, NULL) != PC_OK) {
                pc_char_style_default(&st);
            }
        } else {
            pc_char_style_default(&st);
        }
        f(&st, ud);
        pc_text_run *nr = malloc(sizeof *nr);
        if (nr != NULL) {
            nr[0].len = 0;
            nr[0].style = st;
            pc_text_runs_free(t->runs, t->nruns);
            t->runs = nr;
            t->nruns = 1;
            (void)pc_text_layer_sync_summary(t, NULL);
        } else {
            pc_char_style_fini(&st);
        }
        pc_text_runs_free(runs, n);
        return;
    }
    run_list out;
    rl_init(&out, &CHAR_VT);
    bool ok = true;
    size_t at = 0;
    for (size_t i = 0; i < n && ok; i++) {
        size_t rs = at, re = at + runs[i].len;
        at = re;
        size_t ca = clampsz(a, rs, re), cb = clampsz(b, rs, re);
        ok = push_merge_nz(&out, ca - rs, &runs[i].style);
        if (ok) {
            pc_char_style tmp;
            if (pc_char_style_clone(&runs[i].style, &tmp, NULL) == PC_OK) {
                f(&tmp, ud);
                ok = push_merge_nz(&out, cb - ca, &tmp);
                pc_char_style_fini(&tmp);
            } else {
                ok = false;
            }
        }
        if (ok) {
            ok = push_merge_nz(&out, re - cb, &runs[i].style);
        }
    }
    if (ok) {
        commit_char_runs(t, &out);
    }
    rl_fini(&out);
    pc_text_runs_free(runs, n);
}

void pc_engine_style_paragraphs(pc_text_layer *t, size_t a, size_t b, pc_engine_para_style_mut_fn f, void *ud)
{
    pc_paragraph_run *old = NULL;
    size_t nold = 0;
    if (pc_text_layer_paragraph_runs(t, &old, &nold, NULL) != PC_OK) {
        return;
    }
    size_t *starts = nold > 0 ? malloc(nold * sizeof *starts) : NULL;
    if (nold > 0 && starts == NULL) {
        free(old);
        return;
    }
    size_t acc = 0;
    for (size_t i = 0; i < nold; i++) {
        starts[i] = acc;
        acc += old[i].len;
    }
    const char *text = pc_str_c(&t->text);
    size_t tlen = t->text.len;
    size_t *offs = NULL, np = 0;
    if (pc_text_split_paragraphs(text, tlen, &offs, &np, NULL) != PC_OK) {
        free(starts);
        free(old);
        return;
    }
    bool trailing_break = tlen > 0 && (text[tlen - 1] == '\r' || text[tlen - 1] == '\n');
    pc_paragraph_run *outv = NULL;
    size_t non = 0, cap = 0;
    bool ok = true;
    for (size_t k = 0; k < np && ok; k++) {
        size_t ps = offs[2 * k], pe = offs[2 * k + 1], plen = pe - ps;
        /* style_at(ps): the last paragraph whose start <= ps. */
        pc_paragraph_style st = nold > 0 ? old[0].style : pc_paragraph_style_default();
        for (size_t i = 0; i < nold; i++) {
            if (starts[i] <= ps) {
                st = old[i].style;
            }
        }
        bool terminal_empty = ps == pe;
        bool touches = (ps < b && pe > a) ||
                       (a == b && a >= ps && (a < pe || terminal_empty || (pe == tlen && !trailing_break)));
        if (touches) {
            f(&st, ud);
        }
        bool raw = terminal_empty && trailing_break;
        size_t use_len = raw ? 0 : plen;
        if (!raw && plen == 0 && non > 0) {
            /* push_merge skips a zero-length body; nothing to add. */
            continue;
        }
        if (!raw && non > 0 && pc_paragraph_style_eq(&outv[non - 1].style, &st)) {
            outv[non - 1].len += use_len;
            continue;
        }
        if (non == cap) {
            size_t nc = cap == 0 ? 4 : cap * 2;
            pc_paragraph_run *nv = realloc(outv, nc * sizeof *nv);
            if (nv == NULL) {
                ok = false;
                break;
            }
            outv = nv;
            cap = nc;
        }
        outv[non].len = use_len;
        outv[non].style = st;
        non++;
    }
    if (ok) {
        if (non == 0) {
            /* Keep one paragraph so the layer always has a paragraph style. */
            pc_paragraph_run *nv = realloc(outv, sizeof *nv);
            if (nv != NULL) {
                outv = nv;
                outv[0].len = 0;
                outv[0].style = nold > 0 ? old[0].style : pc_paragraph_style_default();
                non = 1;
            }
        }
        free(t->paragraphs);
        t->paragraphs = outv;
        t->nparagraphs = non;
    } else {
        free(outv);
    }
    free(offs);
    free(starts);
    free(old);
}

/* ---- layer naming ------------------------------------------------------------------------- */

size_t pc_engine_type_layer_name(const char *text, size_t len, char *out, size_t cap)
{
    size_t ls = 0;
    const char *chosen = NULL;
    size_t chosen_len = 0;
    while (ls <= len) {
        size_t le = ls;
        while (le < len && text[le] != '\n') {
            le++;
        }
        size_t a = ls, b = le;
        if (b > a && text[b - 1] == '\r') {
            b--;
        }
        while (a < b && (unsigned char)text[a] <= ' ') {
            a++;
        }
        while (b > a && (unsigned char)text[b - 1] <= ' ') {
            b--;
        }
        if (b > a) {
            chosen = text + a;
            chosen_len = b - a;
            break;
        }
        if (le >= len) {
            break;
        }
        ls = le + 1;
    }
    const char *name = chosen != NULL ? chosen : "Type Layer";
    size_t nl = chosen != NULL ? chosen_len : 10;
    /* At most 40 code points. */
    size_t bytes = 0, cps = 0;
    while (bytes < nl && cps < 40) {
        bytes++;
        while (bytes < nl && ((unsigned char)name[bytes] & 0xC0) == 0x80) {
            bytes++;
        }
        cps++;
    }
    size_t w = bytes < cap - 1 ? bytes : cap - 1;
    memcpy(out, name, w);
    out[w] = '\0';
    return w;
}

bool pc_engine_is_auto_named(const pc_layer *l)
{
    if (l->content.kind != PC_LAYER_TEXT) {
        return false;
    }
    const pc_text_layer *t = &l->content.u.text;
    char nm[256];
    pc_engine_type_layer_name(pc_str_c(&t->text), t->text.len, nm, sizeof nm);
    return strcmp(pc_str_c(&l->name), nm) == 0;
}

void pc_engine_type_defaults_fini(pc_engine_session *s)
{
    if (s == NULL || !s->has_type_defaults) {
        return;
    }
    pc_char_style_fini(&s->type_defaults_char);
    s->has_type_defaults = false;
}

/* ---- targeting + edit helpers ------------------------------------------------------------- */

static pc_status active_doc(const pc_engine_session *s, const pc_document **out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    *out = pc_doc_arc_get(st->doc);
    return PC_OK;
}

/* Rust type_cmds::layer_id: "layer" as a u64, else the active layer. */
static pc_status local_layer_id(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out, pc_error *err)
{
    uint64_t id;
    if (pc_json_as_u64(pc_json_get(p, "layer"), &id)) {
        *out = (pc_layer_id)id;
        return PC_OK;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL || !st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    *out = st->active_layer;
    return PC_OK;
}

static pc_status not_type_layer(pc_error *err, pc_layer_id id, const pc_layer *l)
{
    return pc_engine_err_other(err, "layer %llu is %s %s layer, not a type layer", (unsigned long long)id,
                               pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

static bool range_param(const char *text, size_t len, yyjson_val *p, size_t *oa, size_t *ob)
{
    yyjson_val *r = pc_json_get(p, "range");
    if (r != NULL && yyjson_is_arr(r) && yyjson_arr_size(r) == 2) {
        uint64_t a = 0, b;
        (void)pc_json_as_u64(yyjson_arr_get(r, 0), &a);
        if (!pc_json_as_u64(yyjson_arr_get(r, 1), &b)) {
            b = a;
        }
        uint64_t ca = a < b ? a : b, cb = a < b ? b : a;
        *oa = pc_engine_text_byte_at(text, len, (size_t)ca);
        *ob = pc_engine_text_byte_at(text, len, (size_t)cb);
        return true;
    }
    *oa = 0;
    *ob = len;
    return false;
}

static bool ids_contains(const pc_layer_id *v, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (v[i] == id) {
            return true;
        }
    }
    return false;
}

pc_status pc_engine_format_targets(const pc_engine_session *s, yyjson_val *p, pc_layer_id **out, size_t *n,
                                   pc_error *err)
{
    const pc_document *doc = NULL;
    if (active_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    bool single = pc_json_get(p, "layer") != NULL || (pc_json_get(p, "range") != NULL && pc_json_get(p, "layers") == NULL);
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (single) {
        pc_layer_id id = 0;
        if (local_layer_id(s, p, &id, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        ids = malloc(sizeof *ids);
        if (ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "targets");
        }
        ids[0] = id;
        nids = 1;
    } else if (pc_json_get(p, "layers") != NULL) {
        yyjson_val *arr = pc_json_get(p, "layers");
        if (!yyjson_is_arr(arr)) {
            return pc_engine_err_other(err, "layers must be an array of layer IDs");
        }
        size_t m = yyjson_arr_size(arr);
        ids = m > 0 ? malloc(m * sizeof *ids) : NULL;
        if (m > 0 && ids == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "targets");
        }
        for (size_t i = 0; i < m; i++) {
            uint64_t id;
            if (!pc_json_as_u64(yyjson_arr_get(arr, i), &id)) {
                free(ids);
                return pc_engine_err_other(err, "layers must contain layer IDs");
            }
            ids[nids++] = (pc_layer_id)id;
        }
    } else {
        const pc_engine_doc_state *st = pc_engine_session_active(s);
        if (pc_engine_doc_state_selected_layers(st, &ids, &nids, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    pc_layer_id *targets = nids > 0 ? malloc(nids * sizeof *targets) : NULL;
    size_t nt = 0;
    if (nids > 0 && targets == NULL) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "targets");
    }
    pc_status rc = PC_OK;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        if (l == NULL) {
            rc = pc_engine_err_no_layer(err, ids[i]);
            break;
        }
        if (l->content.kind == PC_LAYER_TEXT) {
            if (!ids_contains(targets, nt, ids[i])) {
                targets[nt++] = ids[i];
            }
        } else if (single) {
            rc = not_type_layer(err, ids[i], l);
            break;
        }
    }
    free(ids);
    if (rc != PC_OK) {
        free(targets);
        return rc;
    }
    if (nt == 0) {
        free(targets);
        return pc_engine_err_other(err, "select a type layer");
    }
    *out = targets;
    *n = nt;
    return PC_OK;
}

pc_status pc_engine_has_format_target(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id *ids = NULL;
    size_t n = 0;
    pc_status rc = pc_engine_format_targets(s, NULL, &ids, &n, reason);
    free(ids);
    return rc;
}

/* with_text_targets: one history step applying f to each format target's byte range. */
typedef struct {
    yyjson_val *p;
    pc_engine_text_target_fn f;
    void *ud;
    const pc_layer_id *ids;
    size_t nids;
} wtt_ctx;

static pc_status edit_text_targets(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    wtt_ctx *c = vctx;
    bool whole_each = pc_json_get(c->p, "layers") == NULL || pc_json_get(c->p, "layer") != NULL;
    for (size_t i = 0; i < c->nids; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        pc_text_layer *t = &l->content.u.text;
        size_t ba = 0, bb = t->text.len;
        if (whole_each) {
            range_param(pc_str_c(&t->text), t->text.len, c->p, &ba, &bb);
        }
        pc_status rc = c->f(t, ba, bb, c->ud, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_engine_type_refresh(doc, t);
    }
    return PC_OK;
}

pc_status pc_engine_with_text_targets(pc_engine_session *s, yyjson_val *p, const char *label,
                                      pc_engine_text_target_fn f, void *ud, pc_layer_id **out_ids,
                                      size_t *nids, pc_error *err)
{
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (pc_engine_format_targets(s, p, &ids, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    wtt_ctx ctx = {p, f, ud, ids, n};
    pc_status rc = pc_engine_session_edit(s, label, edit_text_targets, &ctx, err);
    if (rc != PC_OK) {
        free(ids);
        return rc;
    }
    if (out_ids != NULL) {
        *out_ids = ids;
        *nids = n;
    } else {
        free(ids);
    }
    return PC_OK;
}

/* with_text_layer: edit one type layer as one undo step and re-render it, renaming in the same
 * step (explicit name wins; else an auto-named layer follows its text). */
typedef pc_status (*te_edit_fn)(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err);

typedef struct {
    pc_layer_id id;
    te_edit_fn f;
    void *ud;
    bool has_name;
    const char *name;
    size_t namelen;
} wtl_ctx;

static pc_status edit_one_text(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    wtl_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    bool auto_named = pc_engine_is_auto_named(l);
    if (l->content.kind != PC_LAYER_TEXT) {
        return not_type_layer(err, c->id, l);
    }
    pc_text_layer *t = &l->content.u.text;
    pc_status rc = c->f(t, doc, c->ud, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_type_refresh(doc, t);
    if (c->has_name) {
        return pc_str_set(&l->name, c->name, c->namelen, err);
    }
    if (auto_named) {
        char nm[256];
        size_t w = pc_engine_type_layer_name(pc_str_c(&t->text), t->text.len, nm, sizeof nm);
        return pc_str_set(&l->name, nm, w, err);
    }
    return PC_OK;
}

static pc_status with_text_layer(pc_engine_session *s, pc_layer_id id, const char *label, bool has_name,
                                 const char *name, size_t namelen, te_edit_fn f, void *ud, pc_error *err)
{
    const pc_document *doc = NULL;
    if (active_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_check_text_unlocked(doc, id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    wtl_ctx ctx = {id, f, ud, has_name, name, namelen};
    return pc_engine_session_edit(s, label, edit_one_text, &ctx, err);
}

/* ---- type.info ---------------------------------------------------------------------------- */

pc_status pc_engine_type_info(const pc_engine_session *s, pc_layer_id id, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = NULL;
    if (active_doc(s, &doc, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_TEXT) {
        return pc_engine_err_other(err, "layer %llu is not a type layer", (unsigned long long)id);
    }
    const pc_text_layer *t = &l->content.u.text;
    const char *text = pc_str_c(&t->text);
    size_t tlen = t->text.len;
    float dpi = doc->resolution_dpi;

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_obj_add_strncpy(rd, root, "text", text, tlen);

    /* runs */
    pc_text_run *cr = NULL;
    size_t ncr = 0;
    (void)pc_text_layer_char_runs(t, &cr, &ncr, NULL);
    yyjson_mut_val *runs = yyjson_mut_arr(rd);
    size_t at = 0;
    for (size_t i = 0; i < ncr; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, o, "start", pc_engine_text_char_at(text, tlen, at));
        yyjson_mut_obj_add_uint(rd, o, "end", pc_engine_text_char_at(text, tlen, at + cr[i].len));
        yyjson_mut_obj_add_val(rd, o, "style", pc_char_style_to_json(rd, &cr[i].style, PC_DOC_JSON_VALUE));
        yyjson_mut_arr_append(runs, o);
        at += cr[i].len;
    }
    yyjson_mut_obj_add_val(rd, root, "runs", runs);
    pc_text_runs_free(cr, ncr);

    /* paragraphs */
    pc_paragraph_run *pr = NULL;
    size_t npr = 0;
    (void)pc_text_layer_paragraph_runs(t, &pr, &npr, NULL);
    yyjson_mut_val *paras = yyjson_mut_arr(rd);
    at = 0;
    for (size_t i = 0; i < npr; i++) {
        yyjson_mut_val *o = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, o, "start", pc_engine_text_char_at(text, tlen, at));
        yyjson_mut_obj_add_uint(rd, o, "end", pc_engine_text_char_at(text, tlen, at + pr[i].len));
        yyjson_mut_obj_add_val(rd, o, "style", pc_paragraph_style_to_json(rd, &pr[i].style, PC_DOC_JSON_VALUE));
        yyjson_mut_arr_append(paras, o);
        at += pr[i].len;
    }
    yyjson_mut_obj_add_val(rd, root, "paragraphs", paras);
    free(pr);

    /* shape / transform / antialias / orientation / warp */
    yyjson_mut_obj_add_val(rd, root, "shape", pc_text_shape_to_json(rd, &t->shape, PC_DOC_JSON_VALUE));
    yyjson_mut_val *tf = yyjson_mut_arr(rd);
    for (size_t i = 0; i < 6; i++) {
        yyjson_mut_arr_add_real(rd, tf, t->transform.m[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "transform", tf);
    yyjson_mut_obj_add_val(rd, root, "antialias", pc_antialias_to_json(rd, &t->antialias, PC_DOC_JSON_VALUE));
    yyjson_mut_obj_add_val(rd, root, "orientation", pc_orientation_to_json(rd, &t->orientation, PC_DOC_JSON_VALUE));
    if (t->has_warp) {
        yyjson_mut_obj_add_val(rd, root, "warp", pc_text_warp_to_json(rd, &t->warp, PC_DOC_JSON_VALUE));
    } else {
        yyjson_mut_obj_add_null(rd, root, "warp");
    }

    /* lines + pxPerPt (laid out) */
    yyjson_mut_val *lines = yyjson_mut_arr(rd);
    double px_per_pt = 0.0;
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) == PC_OK) {
        pc_text_layout layout;
        if (pc_text_engine_layout(eng, t, dpi, &layout, NULL) == PC_OK) {
            px_per_pt = layout.px_per_pt;
            for (size_t i = 0; i < layout.nlines; i++) {
                const pc_line_info *ln = &layout.lines[i];
                yyjson_mut_val *o = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_uint(rd, o, "start", pc_engine_text_char_at(text, tlen, ln->start));
                yyjson_mut_obj_add_uint(rd, o, "end", pc_engine_text_char_at(text, tlen, ln->end));
                yyjson_mut_obj_add_real(rd, o, "baseline", ln->baseline);
                yyjson_mut_obj_add_real(rd, o, "x0", ln->x0);
                yyjson_mut_obj_add_real(rd, o, "x1", ln->x1);
                yyjson_mut_obj_add_real(rd, o, "ascent", ln->ascent);
                yyjson_mut_obj_add_real(rd, o, "descent", ln->descent);
                yyjson_mut_arr_append(lines, o);
            }
            pc_text_layout_fini(&layout);
        }
        pc_text_engine_shared_unlock();
    }
    yyjson_mut_obj_add_val(rd, root, "lines", lines);

    if (t->has_cache) {
        pc_rect b = pc_surface_content_bounds(&t->cache);
        yyjson_mut_val *ba = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_int(rd, ba, b.x0);
        yyjson_mut_arr_add_int(rd, ba, b.y0);
        yyjson_mut_arr_add_int(rd, ba, b.x1);
        yyjson_mut_arr_add_int(rd, ba, b.y1);
        yyjson_mut_obj_add_val(rd, root, "bounds", ba);
    } else {
        yyjson_mut_obj_add_null(rd, root, "bounds");
    }
    yyjson_mut_obj_add_real(rd, root, "pxPerPt", px_per_pt);

    pc_strlist warn = {0};
    if (pc_engine_missing_font_warnings_for_layer(t, &warn, NULL) == PC_OK && warn.n > 0) {
        yyjson_mut_val *wa = yyjson_mut_arr(rd);
        for (size_t i = 0; i < warn.n; i++) {
            yyjson_mut_arr_add_strcpy(rd, wa, warn.v[i]);
        }
        yyjson_mut_obj_add_val(rd, root, "warnings", wa);
    }
    pc_strlist_fini(&warn);

    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- type.create helpers ------------------------------------------------------------------ */

static pc_status bad(pc_error *err, const char *cmd, const char *msg)
{
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Builds a new (not-yet-inserted) type layer from type.create's params. *out is left
 * uninitialised on error. */
static pc_status new_text_layer(const pc_engine_session *s, yyjson_val *p, const char *cmd, pc_text_layer *out,
                                pc_error *err)
{
    pc_orientation orient = PC_ORIENT_HORIZONTAL;
    yyjson_val *ov = pc_json_get(p, "orientation");
    if (ov != NULL) {
        size_t ol = 0;
        const char *os = pc_json_as_str(ov, &ol);
        if (os != NULL && ol == 10 && memcmp(os, "horizontal", 10) == 0) {
            orient = PC_ORIENT_HORIZONTAL;
        } else if (os != NULL && ol == 8 && memcmp(os, "vertical", 8) == 0) {
            orient = PC_ORIENT_VERTICAL;
        } else {
            return bad(err, cmd, "orientation must be horizontal or vertical");
        }
    }
    char emsg[256];
    if (!pc_engine_check_kerning(p, emsg, sizeof emsg)) {
        return bad(err, cmd, emsg);
    }
    if (!pc_engine_check_size_tracking(p, emsg, sizeof emsg)) {
        return bad(err, cmd, emsg);
    }
    size_t txtlen = 0;
    const char *txt = pc_json_as_str(pc_json_get(p, "text"), &txtlen);
    if (txt == NULL) {
        txt = "";
        txtlen = 0;
    }
    size_t nlen = 0;
    char *norm = pc_engine_norm_text(txt, txtlen, &nlen);
    if (norm == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, cmd);
    }

    pc_char_style style;
    pc_paragraph_style para;
    if (s->has_type_defaults) {
        if (pc_char_style_clone(&s->type_defaults_char, &style, err) != PC_OK) {
            free(norm);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        para = s->type_defaults_para;
    } else {
        pc_char_style_default(&style);
        (void)pc_str_set_c(&style.font_family, PC_TEXT_DEFAULT_FAMILY, NULL);
        para = pc_paragraph_style_default();
    }
    const pc_engine_tool_state *tools = pc_engine_session_tools(s);
    style.color = pc_color_rgba(tools->foreground[0], tools->foreground[1], tools->foreground[2],
                                tools->foreground[3]);
    (void)pc_engine_apply_char_props(&style, p);
    (void)pc_engine_apply_para_props(&para, p);

    pc_text_shape shape = {0};
    pc_affine transform;
    yyjson_val *box = pc_json_get(p, "box");
    if (box != NULL && yyjson_is_arr(box) && yyjson_arr_size(box) == 4) {
        double v[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < 4; i++) {
            (void)pc_json_as_f64(yyjson_arr_get(box, i), &v[i]);
        }
        if (v[2] <= 0.0 || v[3] <= 0.0) {
            pc_char_style_fini(&style);
            free(norm);
            return bad(err, cmd, "box width and height must be positive");
        }
        shape.is_box = true;
        shape.x = 0.0f;
        shape.y = 0.0f;
        shape.width = (float)v[2];
        shape.height = (float)v[3];
        transform = pc_affine_translate(v[0], v[1]);
    } else {
        float x = 0.0f, y = 0.0f;
        (void)f32p(p, "x", &x);
        (void)f32p(p, "y", &y);
        shape.is_box = false;
        transform = pc_affine_translate(x, y);
    }

    pc_text_layer_default(out);
    pc_text_runs_free(out->runs, out->nruns);
    out->runs = NULL;
    out->nruns = 0;
    free(out->paragraphs);
    out->paragraphs = NULL;
    out->nparagraphs = 0;
    if (pc_str_set(&out->text, norm, nlen, err) != PC_OK) {
        free(norm);
        pc_char_style_fini(&style);
        pc_text_layer_fini(out);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    free(norm);
    out->shape = shape;
    out->orientation = orient;
    out->transform = transform;
    out->runs = malloc(sizeof *out->runs);
    out->paragraphs = malloc(sizeof *out->paragraphs);
    if (out->runs == NULL || out->paragraphs == NULL) {
        pc_char_style_fini(&style);
        pc_text_layer_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, cmd);
    }
    out->runs[0].len = nlen;
    out->runs[0].style = style;
    out->nruns = 1;
    out->paragraphs[0].len = nlen;
    out->paragraphs[0].style = para;
    out->nparagraphs = 1;
    (void)pc_text_layer_sync_summary(out, NULL);
    return PC_OK;
}

typedef struct {
    pc_text_layer *t; /* moved into the document on success */
    const char *name;
    size_t namelen;
} create_ctx;

static pc_status edit_insert_text(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    create_ctx *c = vctx;
    pc_engine_type_refresh(doc, c->t);
    pc_str name = {0};
    if (pc_str_set(&name, c->name, c->namelen, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_content content = {0};
    content.kind = PC_LAYER_TEXT;
    content.u.text = *c->t;
    memset(c->t, 0, sizeof *c->t);
    pc_layer layer;
    pc_status rc = pc_layer_init(&layer, pc_str_c(&name), &content, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    pc_layer_id out_id;
    rc = pc_document_insert_above(doc, *ha ? a : NULL, &layer, &out_id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    *a = out_id;
    *ha = true;
    return PC_OK;
}

static pc_status run_create(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_text_layer t = {0};
    if (new_text_layer(s, p, "type.create", &t, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    char autobuf[256];
    size_t nlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (name == NULL) {
        nlen = pc_engine_type_layer_name(pc_str_c(&t.text), t.text.len, autobuf, sizeof autobuf);
        name = autobuf;
    }
    create_ctx ctx = {&t, name, nlen};
    pc_status rc = pc_engine_session_edit(s, "New Type Layer", edit_insert_text, &ctx, err);
    pc_text_layer_fini(&t); /* zeroed when moved in */
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    pc_layer_id id = st->active_layer;
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, id);

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (surf != NULL) {
        pc_rect b = pc_surface_content_bounds(surf);
        yyjson_mut_val *ba = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_int(rd, ba, b.x0);
        yyjson_mut_arr_add_int(rd, ba, b.y0);
        yyjson_mut_arr_add_int(rd, ba, b.x1);
        yyjson_mut_arr_add_int(rd, ba, b.y1);
        yyjson_mut_obj_add_val(rd, root, "bounds", ba);
    } else {
        yyjson_mut_obj_add_null(rd, root, "bounds");
    }
    if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
        pc_strlist warn = {0};
        if (pc_engine_missing_font_warnings_for_layer(&l->content.u.text, &warn, NULL) == PC_OK && warn.n > 0) {
            yyjson_mut_val *wa = yyjson_mut_arr(rd);
            for (size_t i = 0; i < warn.n; i++) {
                yyjson_mut_arr_add_strcpy(rd, wa, warn.v[i]);
            }
            yyjson_mut_obj_add_val(rd, root, "warnings", wa);
        }
        pc_strlist_fini(&warn);
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- style-apply callbacks ---------------------------------------------------------------- */

static void apply_char_cb(pc_char_style *st, void *ud) { (void)pc_engine_apply_char_props(st, (yyjson_val *)ud); }
static void apply_para_cb(pc_paragraph_style *st, void *ud) { (void)pc_engine_apply_para_props(st, (yyjson_val *)ud); }

/* Sets kerning from a bare JSON value (type.edit's range kerning). */
static void set_kern_cb(pc_char_style *st, void *ud)
{
    yyjson_val *kv = ud;
    if (yyjson_is_num(kv)) {
        double k;
        if (pc_json_as_f64(kv, &k) && isfinite(k)) {
            double cl = k < PC_TYPE_KERN_MIN ? PC_TYPE_KERN_MIN : k > PC_TYPE_KERN_MAX ? PC_TYPE_KERN_MAX : k;
            st->kerning = PC_KERNING_OFF;
            st->kern = (float)cl;
        }
    } else if (yyjson_is_str(kv)) {
        size_t kl = 0;
        const char *ks = pc_json_as_str(kv, &kl);
        if (ks != NULL) {
            if (ascii_ieq(ks, kl, "optical")) {
                st->kerning = PC_KERNING_OPTICAL;
            } else if (ascii_ieq(ks, kl, "none") || ascii_ieq(ks, kl, "off") || ascii_ieq(ks, kl, "0")) {
                st->kerning = PC_KERNING_OFF;
            } else {
                st->kerning = PC_KERNING_METRICS;
            }
            st->kern = 0.0f;
        }
    }
}

static void set_kernpair_cb(pc_char_style *st, void *ud)
{
    st->kerning = PC_KERNING_OFF;
    st->kern = *(float *)ud;
}

/* ---- step_char_metrics (leadingBy / baselineShiftBy / trackingBy) ------------------------- */

typedef struct {
    bool has_l, has_s, has_t;
    float lby, sby, tby, autol;
} step_ctx;

static void step_apply_cb(pc_char_style *st, void *ud)
{
    step_ctx *c = ud;
    if (c->has_l) {
        float a = (isfinite(c->autol) && c->autol > 0.0f) ? c->autol : 1.2f;
        float base = st->has_leading ? st->leading_pt : a * st->size_pt;
        if (!isfinite(base)) {
            base = 0.0f;
        }
        st->has_leading = true;
        st->leading_pt = clampf(base + c->lby, PC_TYPE_LEADING_MIN_PT, PC_TYPE_LEADING_MAX_PT);
    }
    if (c->has_s) {
        float base = isfinite(st->baseline_shift_pt) ? st->baseline_shift_pt : 0.0f;
        st->baseline_shift_pt = clampf(base + c->sby, -PC_TYPE_BASELINE_SHIFT_MAX_PT, PC_TYPE_BASELINE_SHIFT_MAX_PT);
    }
    if (c->has_t) {
        float base = isfinite(st->tracking) ? st->tracking : 0.0f;
        st->tracking = clampf(base + c->tby, (float)PC_TYPE_TRACKING_MIN, (float)PC_TYPE_TRACKING_MAX);
    }
}

static bool finite_f32p(yyjson_val *p, const char *k, float *out)
{
    float v;
    if (f32p(p, k, &v) && isfinite(v)) {
        *out = v;
        return true;
    }
    return false;
}

static bool step_char_metrics(pc_text_layer *t, size_t a, size_t b, yyjson_val *p)
{
    step_ctx base = {0};
    base.has_l = finite_f32p(p, "leadingBy", &base.lby);
    base.has_s = finite_f32p(p, "baselineShiftBy", &base.sby);
    base.has_t = finite_f32p(p, "trackingBy", &base.tby);
    if (!base.has_l && !base.has_s && !base.has_t) {
        return false;
    }
    pc_paragraph_run *pr = NULL;
    size_t npr = 0;
    (void)pc_text_layer_paragraph_runs(t, &pr, &npr, NULL);
    float default_auto = pc_paragraph_style_default().auto_leading;
    if (t->text.len == 0) {
        step_ctx c = base;
        c.autol = npr > 0 ? pr[0].style.auto_leading : default_auto;
        pc_engine_style_range(t, 0, 0, step_apply_cb, &c);
        free(pr);
        return true;
    }
    size_t at = 0;
    for (size_t i = 0; i < npr; i++) {
        size_t ps = at, pe = at + pr[i].len;
        at = pe;
        size_t pa = clampsz(a, ps, pe), pb = clampsz(b, ps, pe);
        if (pa < pb) {
            step_ctx c = base;
            c.autol = pr[i].style.auto_leading;
            pc_engine_style_range(t, pa, pb, step_apply_cb, &c);
        }
    }
    if (at < b) {
        step_ctx c = base;
        c.autol = default_auto;
        size_t pa = a > at ? a : at;
        pc_engine_style_range(t, pa, b, step_apply_cb, &c);
    }
    free(pr);
    return true;
}

/* ---- metricsAsShown: size/leading/leadingBy divided by the layer transform scale ----------- */

static pc_status shown_params(const pc_text_layer *t, yyjson_val *p, yyjson_doc **owned, yyjson_val **props,
                              pc_error *err)
{
    *owned = NULL;
    *props = p;
    bool as_shown = false;
    if (!(pc_json_as_bool(pc_json_get(p, "metricsAsShown"), &as_shown) && as_shown)) {
        return PC_OK;
    }
    const double *m = t->transform.m;
    double det = fabs(m[0] * m[3] - m[1] * m[2]);
    double scale = sqrt(det);
    if (!(isfinite(scale) && scale > 1e-3)) {
        scale = 1.0;
    }
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "type.setStyle");
    }
    yyjson_mut_val *root = yyjson_val_mut_copy(md, p);
    if (root == NULL || !yyjson_mut_is_obj(root)) {
        yyjson_mut_doc_free(md);
        return pc_fail(err, PC_ERR_NOMEM, "type.setStyle");
    }
    static const char *const keys[3] = {"size", "leading", "leadingBy"};
    for (size_t i = 0; i < 3; i++) {
        double value;
        if (!pc_json_as_f64(pc_json_get(p, keys[i]), &value)) {
            continue;
        }
        value /= scale;
        if (!isfinite(value) || fabs(value) > (double)FLT_MAX) {
            yyjson_mut_doc_free(md);
            return bad(err, "type.setStyle", "shown size and leading must convert to finite point values");
        }
        yyjson_mut_obj_remove_key(root, keys[i]);
        yyjson_mut_obj_add_real(md, root, keys[i], value);
    }
    pc_buf buf = {0};
    pc_status rc = pc_json_write(root, 0, &buf, err);
    yyjson_mut_doc_free(md);
    if (rc != PC_OK) {
        pc_buf_fini(&buf);
        return rc;
    }
    yyjson_doc *doc = NULL;
    rc = pc_json_parse((const char *)buf.data, buf.len, &doc, err);
    pc_buf_fini(&buf);
    if (rc != PC_OK) {
        return rc;
    }
    *owned = doc;
    *props = yyjson_doc_get_root(doc);
    return PC_OK;
}

/* ---- type.edit ---------------------------------------------------------------------------- */

typedef struct {
    yyjson_val *p;
    bool has_kp;
    uint64_t kp_at;
    double kp_by;
} edit_ctx;

static pc_status type_edit_cb(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err)
{
    edit_ctx *c = ud;
    yyjson_val *p = c->p;
    if (c->has_kp) {
        const char *text = pc_str_c(&t->text);
        size_t tlen = t->text.len;
        size_t n = pc_engine_text_char_at(text, tlen, tlen);
        if (c->kp_at == 0 || c->kp_at >= n) {
            return bad(err, "type.edit", "kernPair needs a caret between two characters");
        }
        size_t a = pc_engine_text_byte_at(text, tlen, (size_t)c->kp_at - 1);
        size_t b = pc_engine_text_byte_at(text, tlen, (size_t)c->kp_at);
        bool has = false;
        float kern = 0.0f;
        pc_text_engine *eng = NULL;
        if (pc_text_engine_shared_lock(&eng, NULL) == PC_OK) {
            (void)pc_text_engine_pair_kerning(eng, t, doc->resolution_dpi, a, &has, &kern, NULL);
            pc_text_engine_shared_unlock();
        }
        if (!has) {
            return bad(err, "type.edit", "no kerning pair at the caret (line break or line end)");
        }
        double value = (double)roundf(kern) + c->kp_by;
        value = value < PC_TYPE_KERN_MIN ? PC_TYPE_KERN_MIN : value > PC_TYPE_KERN_MAX ? PC_TYPE_KERN_MAX : value;
        float kv = (float)value;
        pc_engine_style_range(t, a, b, set_kernpair_cb, &kv);
    }
    yyjson_val *kerning = pc_json_get(p, "kerning");
    if (kerning != NULL) {
        size_t a, b;
        range_param(pc_str_c(&t->text), t->text.len, p, &a, &b);
        pc_engine_style_range(t, a, b, set_kern_cb, kerning);
    }
    size_t vl = 0;
    const char *v = pc_json_as_str(pc_json_get(p, "text"), &vl);
    if (v != NULL) {
        pc_engine_replace_text(t, 0, t->text.len, v, vl);
    }
    yyjson_val *rep = pc_json_get(p, "replace");
    if (rep != NULL) {
        const char *text = pc_str_c(&t->text);
        size_t tlen = t->text.len;
        uint64_t st0 = 0, en = 0;
        (void)pc_json_as_u64(pc_json_get(rep, "start"), &st0);
        bool hasend = pc_json_as_u64(pc_json_get(rep, "end"), &en);
        size_t a = pc_engine_text_byte_at(text, tlen, (size_t)st0);
        size_t b = hasend ? pc_engine_text_byte_at(text, tlen, (size_t)en) : tlen;
        if (b < a) {
            b = a;
        }
        size_t nl = 0;
        const char *ns = pc_json_as_str(pc_json_get(rep, "text"), &nl);
        if (ns == NULL) {
            ns = "";
            nl = 0;
        }
        pc_engine_replace_text(t, a, b, ns, nl);
    }
    yyjson_val *runs = pc_json_get(p, "runs");
    if (runs != NULL && yyjson_is_arr(runs)) {
        size_t idx, max;
        yyjson_val *r;
        yyjson_arr_foreach(runs, idx, max, r) {
            const char *text = pc_str_c(&t->text);
            size_t tlen = t->text.len;
            uint64_t st0 = 0, en = 0;
            (void)pc_json_as_u64(pc_json_get(r, "start"), &st0);
            bool hasend = pc_json_as_u64(pc_json_get(r, "end"), &en);
            size_t a = pc_engine_text_byte_at(text, tlen, (size_t)st0);
            size_t b = hasend ? pc_engine_text_byte_at(text, tlen, (size_t)en) : tlen;
            pc_engine_style_range(t, a, b, apply_char_cb, r);
        }
    }
    double lin[6];
    memcpy(lin, t->transform.m, sizeof lin);
    yyjson_val *box = pc_json_get(p, "box");
    if (box != NULL && yyjson_is_arr(box) && yyjson_arr_size(box) == 4) {
        double vv[4] = {0, 0, 0, 0};
        for (size_t i = 0; i < 4; i++) {
            (void)pc_json_as_f64(yyjson_arr_get(box, i), &vv[i]);
        }
        t->shape.is_box = true;
        t->shape.x = 0.0f;
        t->shape.y = 0.0f;
        t->shape.width = (float)(vv[2] > 1.0 ? vv[2] : 1.0);
        t->shape.height = (float)(vv[3] > 1.0 ? vv[3] : 1.0);
        t->transform.m[0] = lin[0];
        t->transform.m[1] = lin[1];
        t->transform.m[2] = lin[2];
        t->transform.m[3] = lin[3];
        t->transform.m[4] = vv[0];
        t->transform.m[5] = vv[1];
    }
    yyjson_val *pt = pc_json_get(p, "point");
    if (pt != NULL && yyjson_is_arr(pt) && yyjson_arr_size(pt) == 2) {
        double x = 0, y = 0;
        (void)pc_json_as_f64(yyjson_arr_get(pt, 0), &x);
        (void)pc_json_as_f64(yyjson_arr_get(pt, 1), &y);
        t->shape.is_box = false;
        t->transform.m[0] = lin[0];
        t->transform.m[1] = lin[1];
        t->transform.m[2] = lin[2];
        t->transform.m[3] = lin[3];
        t->transform.m[4] = x;
        t->transform.m[5] = y;
    }
    yyjson_val *mv = pc_json_get(p, "move");
    if (mv != NULL && yyjson_is_arr(mv) && yyjson_arr_size(mv) == 2) {
        double dx = 0, dy = 0;
        (void)pc_json_as_f64(yyjson_arr_get(mv, 0), &dx);
        (void)pc_json_as_f64(yyjson_arr_get(mv, 1), &dy);
        t->transform.m[4] += dx;
        t->transform.m[5] += dy;
    }
    yyjson_val *tm = pc_json_get(p, "transform");
    if (tm != NULL && yyjson_is_arr(tm) && yyjson_arr_size(tm) == 6) {
        for (size_t i = 0; i < 6; i++) {
            double x;
            if (pc_json_as_f64(yyjson_arr_get(tm, i), &x)) {
                t->transform.m[i] = x;
            }
        }
    }
    size_t al = 0;
    const char *aa = pc_json_as_str(pc_json_get(p, "antialias"), &al);
    if (aa != NULL) {
        if (al == 4 && memcmp(aa, "none", 4) == 0) {
            t->antialias = PC_AA_NONE;
        } else if (al == 5 && memcmp(aa, "sharp", 5) == 0) {
            t->antialias = PC_AA_SHARP;
        } else if (al == 5 && memcmp(aa, "crisp", 5) == 0) {
            t->antialias = PC_AA_CRISP;
        } else if (al == 6 && memcmp(aa, "strong", 6) == 0) {
            t->antialias = PC_AA_STRONG;
        } else {
            t->antialias = PC_AA_SMOOTH;
        }
    }
    return PC_OK;
}

static pc_status run_edit(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id = 0;
    if (local_layer_id(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    size_t nmlen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "name"), &nmlen);
    char emsg[256];
    if (!pc_engine_check_kerning(p, emsg, sizeof emsg)) {
        return bad(err, "type.edit", emsg);
    }
    if (!pc_engine_check_size_tracking(p, emsg, sizeof emsg)) {
        return bad(err, "type.edit", emsg);
    }
    yyjson_val *runs = pc_json_get(p, "runs");
    if (runs != NULL && yyjson_is_arr(runs)) {
        size_t idx, max;
        yyjson_val *r;
        yyjson_arr_foreach(runs, idx, max, r) {
            if (!pc_engine_check_kerning(r, emsg, sizeof emsg)) {
                return bad(err, "type.edit", emsg);
            }
            uint64_t start = 0, end;
            (void)pc_json_as_u64(pc_json_get(r, "start"), &start);
            if (pc_json_as_u64(pc_json_get(r, "end"), &end) && end < start) {
                return pc_engine_err_bad_params(err, "type.edit",
                                                "runs[%zu]: `end` (%llu) is before `start` (%llu)", idx,
                                                (unsigned long long)end, (unsigned long long)start);
            }
            if (!pc_engine_check_size_tracking(r, emsg, sizeof emsg)) {
                return bad(err, "type.edit", emsg);
            }
        }
    }
    edit_ctx ctx = {p, false, 0, 0.0};
    yyjson_val *kp = pc_json_get(p, "kernPair");
    if (kp != NULL) {
        uint64_t at;
        if (!pc_json_as_u64(pc_json_get(kp, "at"), &at)) {
            return bad(err, "type.edit", "kernPair.at must be a caret position (character index)");
        }
        double by;
        if (!(pc_json_as_f64(pc_json_get(kp, "by"), &by) && isfinite(by) && fabs(by) <= PC_TYPE_KERN_MAX)) {
            return bad(err, "type.edit", "kernPair.by must be a number of 1/1000 em");
        }
        ctx.has_kp = true;
        ctx.kp_at = at;
        ctx.kp_by = by;
    }
    const char *label = ctx.has_kp ? "Kerning" : "Edit Type";
    if (with_text_layer(s, id, label, name != NULL, name, nmlen, type_edit_cb, &ctx, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_type_info(s, id, out, err);
}

/* ---- type.setStyle ------------------------------------------------------------------------ */

static pc_status set_style_cb(pc_text_layer *t, size_t a, size_t b, void *ud, pc_error *err)
{
    yyjson_val *p = ud;
    yyjson_doc *owned = NULL;
    yyjson_val *props = NULL;
    if (shown_params(t, p, &owned, &props, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    char emsg[256];
    if (!pc_engine_check_size_tracking(props, emsg, sizeof emsg)) {
        yyjson_doc_free(owned);
        return bad(err, "type.setStyle", emsg);
    }
    pc_char_style probe;
    pc_char_style_default(&probe);
    bool any_char = pc_engine_apply_char_props(&probe, props);
    pc_char_style_fini(&probe);
    if (any_char) {
        pc_engine_style_range(t, a, b, apply_char_cb, props);
    }
    step_char_metrics(t, a, b, props);
    pc_paragraph_style pprobe = pc_paragraph_style_default();
    if (pc_engine_apply_para_props(&pprobe, p)) {
        pc_engine_style_paragraphs(t, a, b, apply_para_cb, p);
    }
    yyjson_doc_free(owned);
    return PC_OK;
}

static pc_status run_set_style(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    char emsg[256];
    if (!pc_engine_check_kerning(p, emsg, sizeof emsg)) {
        return bad(err, "type.setStyle", emsg);
    }
    if (!pc_engine_check_steps(p, emsg, sizeof emsg)) {
        return bad(err, "type.setStyle", emsg);
    }
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (pc_engine_with_text_targets(s, p, "Set Type Style", set_style_cb, p, &ids, &n, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (ids == NULL || n == 0) {
        free(ids);
        return pc_engine_err_other(err, "select a type layer");
    }
    pc_layer_id id = ids[0];
    free(ids);
    return pc_engine_type_info(s, id, out, err);
}

/* ---- type.rasterize ----------------------------------------------------------------------- */

static pc_status edit_rasterize_text(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    pc_layer_id id = *(pc_layer_id *)vctx;
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_TEXT) {
        return pc_engine_err_other(err, "layer %llu is not a type layer", (unsigned long long)id);
    }
    pc_text_layer *t = &l->content.u.text;
    if (!t->has_cache) {
        pc_engine_type_refresh(doc, t);
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_surface surf;
    if (t->has_cache) {
        surf = t->cache;
        t->has_cache = false;
    } else {
        pc_surface_init(&surf, fmt);
    }
    if (!pc_pixel_format_eq(pc_surface_format(&surf), fmt)) {
        pc_surface conv;
        if (pc_surface_convert(&surf, fmt, &conv, err) != PC_OK) {
            pc_surface_fini(&surf);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&surf);
        surf = conv;
    }
    pc_layer_content_fini(&l->content);
    l->content.kind = PC_LAYER_RASTER;
    l->content.u.raster = surf;
    /* Drop the editable-text PSD block. */
    for (size_t i = 0; i < l->npsd_blocks;) {
        if (memcmp(l->psd_blocks[i].key, "TySh", 4) == 0) {
            pc_blob_release(l->psd_blocks[i].data);
            memmove(&l->psd_blocks[i], &l->psd_blocks[i + 1],
                    (l->npsd_blocks - i - 1) * sizeof l->psd_blocks[0]);
            l->npsd_blocks--;
        } else {
            i++;
        }
    }
    return PC_OK;
}

static pc_status run_rasterize(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id = 0;
    if (local_layer_id(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (pc_engine_session_edit(s, "Rasterize Type", edit_rasterize_text, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "layer", id);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- type.info / type.fonts --------------------------------------------------------------- */

static pc_status run_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id = 0;
    if (local_layer_id(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_type_info(s, id, out, err);
}

static pc_status run_fonts(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    size_t flen = 0;
    const char *family = pc_json_as_str(pc_json_get(p, "family"), &flen);
    pc_text_engine *eng = NULL;
    if (pc_text_engine_shared_lock(&eng, NULL) != PC_OK) {
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    pc_fontdb *db = pc_text_engine_fonts(eng);
    if (family != NULL) {
        pc_face_info *faces = NULL;
        size_t n = 0;
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        if (pc_fontdb_faces(db, family, flen, &faces, &n, NULL) == PC_OK) {
            for (size_t i = 0; i < n; i++) {
                yyjson_mut_val *o = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_strcpy(rd, o, "family", faces[i].family ? faces[i].family : "");
                yyjson_mut_obj_add_strcpy(rd, o, "style", faces[i].style ? faces[i].style : "");
                if (faces[i].postscript_name) {
                    yyjson_mut_obj_add_strcpy(rd, o, "postscriptName", faces[i].postscript_name);
                } else {
                    yyjson_mut_obj_add_null(rd, o, "postscriptName");
                }
                yyjson_mut_obj_add_real(rd, o, "weight", faces[i].weight);
                yyjson_mut_obj_add_bool(rd, o, "italic", faces[i].italic);
                yyjson_mut_val *axs = yyjson_mut_arr(rd);
                for (size_t k = 0; k < faces[i].naxes; k++) {
                    yyjson_mut_val *tup = yyjson_mut_arr(rd);
                    yyjson_mut_arr_add_strcpy(rd, tup, faces[i].axes[k].tag);
                    yyjson_mut_arr_add_real(rd, tup, faces[i].axes[k].min);
                    yyjson_mut_arr_add_real(rd, tup, faces[i].axes[k].def);
                    yyjson_mut_arr_add_real(rd, tup, faces[i].axes[k].max);
                    yyjson_mut_arr_append(axs, tup);
                }
                yyjson_mut_obj_add_val(rd, o, "axes", axs);
                yyjson_mut_arr_append(arr, o);
            }
            pc_face_infos_free(faces, n);
        }
        yyjson_mut_obj_add_val(rd, root, "faces", arr);
    } else {
        pc_strlist fams = {0};
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        if (pc_fontdb_families(db, &fams, NULL) == PC_OK) {
            for (size_t i = 0; i < fams.n; i++) {
                yyjson_mut_arr_add_strcpy(rd, arr, fams.v[i]);
            }
        }
        pc_strlist_fini(&fams);
        yyjson_mut_obj_add_val(rd, root, "families", arr);
    }
    pc_text_engine_shared_unlock();
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- type size keys ----------------------------------------------------------------------- */

static pc_status en_has_type_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_other(reason, "no document open");
    }
    if (st->has_active_layer) {
        const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
        if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
            return PC_OK;
        }
    }
    return pc_engine_err_other(reason, "the active layer is not a type layer");
}

typedef struct {
    bool up;
    float by;
    yyjson_val *p;
    float delta; /* resolved inside the edit, read by the style callback */
} size_step;

static void set_size_cb(pc_char_style *st, void *ud)
{
    float delta = ((size_step *)ud)->delta;
    st->size_pt = clampf(st->size_pt + delta, (float)PC_TYPE_SIZE_MIN_PT, (float)PC_TYPE_SIZE_MAX_PT);
}

static pc_status step_size_cb(pc_text_layer *t, const pc_document *doc, void *ud, pc_error *err)
{
    (void)doc;
    (void)err;
    size_step *ss = ud;
    const double *m = t->transform.m;
    double det = fabs(m[0] * m[3] - m[1] * m[2]);
    float scale = (float)sqrt(det);
    if (!(isfinite(scale) && scale > 1e-3f)) {
        scale = 1.0f;
    }
    ss->delta = (ss->up ? ss->by : -ss->by) / scale;
    size_t a, b;
    range_param(pc_str_c(&t->text), t->text.len, ss->p, &a, &b);
    pc_engine_style_range(t, a, b, set_size_cb, ss);
    return PC_OK;
}

static pc_status step_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, bool up, float default_by,
                           pc_error *err)
{
    pc_layer_id id = 0;
    if (local_layer_id(s, p, &id, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    float by;
    if (!(f32p(p, "by", &by) && isfinite(by))) {
        by = default_by;
    }
    by = fabsf(by);
    if (by > (float)PC_TYPE_SIZE_MAX_PT) {
        by = (float)PC_TYPE_SIZE_MAX_PT;
    }
    size_step ss = {up, by, p, 0.0f};
    if (with_text_layer(s, id, "Change Type Size", false, NULL, 0, step_size_cb, &ss, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    return pc_engine_type_info(s, id, out, err);
}

static pc_status run_inc_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return step_size(s, p, out, true, 2.0f, err);
}
static pc_status run_dec_size(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return step_size(s, p, out, false, 2.0f, err);
}
static pc_status run_inc_size_more(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return step_size(s, p, out, true, 10.0f, err);
}
static pc_status run_dec_size_more(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return step_size(s, p, out, false, 10.0f, err);
}

/* ---- registry ----------------------------------------------------------------------------- */

static pc_status en_ok(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static const char *const MENU_LAYER_NEW[] = {"Layer", "New"};
static const char *const MENU_LAYER_RASTERIZE[] = {"Layer", "Rasterize"};

#define CHAR_PARAMS                                                                                                    \
    "\"font\":str,\"fontStyle\":str,\"weight\":100..900,\"italic\":bool,\"size\":0.1..=1296 "                          \
    "pt,\"color\":\"#rrggbb\"|[r,g,b,a],\"tracking\":-1000..=10000 (1/1000 em),\"leading\":pt|\"auto\","               \
    "\"baselineShift\":pt,\"horizontalScale\":%,\"verticalScale\":%,\"underline\":bool,\"strikethrough\":bool,"        \
    "\"fauxBold\":bool,\"fauxItalic\":bool,\"kerning\":1/1000em (manual, after each "                                  \
    "character)|\"metrics\"|\"optical\"|\"off\",\"caps\":\"normal|small|all\",\"ligatures\":bool,"                      \
    "\"discretionaryLigatures\":bool,\"features\":{\"ss01\":1},\"variations\":{\"wght\":650},\"language\":str"

static const char *const SIZE_PARAMS =
    "{\"layer\":id?,\"range\":[startChar,endChar]? (default all),\"by\":pt? (as the layer appears)}";

static const pc_engine_command SPECS[] = {
    {"type.create", "New Type Layer", MENU_LAYER_NEW, 2, NULL,
     "{\"x\":px,\"y\":px (baseline anchor of point text),\"text\":str,\"box\":[x,y,w,h]? (paragraph "
     "text),\"name\":str?,\"align\":\"left|center|right|justify\xe2\x80\xa6\"?,\"orientation\":\"horizontal|"
     "vertical\"=\"horizontal\", \xe2\x80\xa6""character keys: \"font\",\"size\":pt=12,\"color\",\xe2\x80\xa6}",
     pc_engine_has_doc, run_create, true},
    {"type.edit", "Edit Type", NULL, 0, NULL,
     "{\"layer\":id?,\"text\":str? (replace all, styles kept),\"replace\":{\"start\":char,\"end\":char,\"text\":str}?,"
     "\"runs\":[{\"start\":char,\"end\":char,\xe2\x80\xa6""character keys}]?,\"box\":[x,y,w,h]? (to paragraph "
     "text),\"point\":[x,y]? (to point text),\"move\":[dx,dy]?,\"transform\":[a,b,c,d,e,f]?,\"antialias\":\"none|"
     "sharp|crisp|strong|smooth\"?,\"name\":str?,\"kerning\":1/1000em|\"metrics\"|\"optical\"|\"off\"? (with "
     "\"range\":[startChar,endChar]?, default all),\"kernPair\":{\"at\":caretChar,\"by\":1/1000em}? (Alt+\xe2\x86\x90/"
     "\xe2\x86\x92: the pair before the caret becomes manual, its current kerning + by)}",
     pc_engine_has_doc, run_edit, true},
    {"type.setStyle", "Set Type Style", NULL, 0, NULL,
     "{\"layer\":id? or \"layers\":[id,\xe2\x80\xa6]? (default selected Type layers),\"range\":[startChar,endChar]? "
     "(single layer; default all),\"metricsAsShown\":bool=false (size/leading/leadingBy include each layer's "
     "transform scale), " CHAR_PARAMS
     ", relative steps from each character's own value: \"leadingBy\":pt (auto leading starts from its laid-out "
     "value; result 0.01..=5000),\"baselineShiftBy\":pt (result -5000..=5000),\"trackingBy\":1/1000 em (result "
     "-1000..=10000), paragraph keys: \"align\":\"left|center|right|justify|justifyCenter|justifyRight|justifyAll\","
     "\"firstLineIndent\":pt,\"startIndent\":pt,\"endIndent\":pt,\"spaceBefore\":pt,\"spaceAfter\":pt,\"autoLeading\":"
     "%,\"direction\":\"auto|ltr|rtl\",\"hyphenate\":bool}",
     pc_engine_has_doc, run_set_style, true},
    {"type.rasterize", "Rasterize Type", MENU_LAYER_RASTERIZE, 2, NULL, "{\"layer\":id?}", pc_engine_has_doc,
     run_rasterize, true},
    {"type.info", "Type Layer Info", NULL, 0, NULL,
     "{\"layer\":id?} \xe2\x86\x92 text, runs/paragraphs (char offsets + styles), shape, transform, laid-out lines "
     "(text space px), bounds",
     pc_engine_has_doc, run_info, false},
    {"type.fonts", "List Fonts", NULL, 0, NULL, "{\"family\":str? (faces of one family)}", en_ok, run_fonts, false},
    {"type.increaseSize", "Increase Type Size", NULL, 0, "Cmd+Shift+.", SIZE_PARAMS, en_has_type_layer, run_inc_size,
     true},
    {"type.decreaseSize", "Decrease Type Size", NULL, 0, "Cmd+Shift+,", SIZE_PARAMS, en_has_type_layer, run_dec_size,
     true},
    {"type.increaseSizeMore", "Increase Type Size by 10", NULL, 0, "Cmd+Alt+Shift+.", SIZE_PARAMS, en_has_type_layer,
     run_inc_size_more, true},
    {"type.decreaseSizeMore", "Decrease Type Size by 10", NULL, 0, "Cmd+Alt+Shift+,", SIZE_PARAMS, en_has_type_layer,
     run_dec_size_more, true},
};

const pc_engine_command *pc_engine_type_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
