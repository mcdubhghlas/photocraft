/* Artboards: portable primitives (port of crates/engine/src/artboard_cmds.rs, E6). The commands
 * are deferred (see artboard_cmds.h); only the size presets and the raster-PDF writer are here. */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/comps.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "artboard_cmds.h"
#include "engine_internal.h"
#include "layer_cmds.h"
#include "vector_cmds.h"

/* Device/page size presets (name, width, height) in pixels (PRESETS). */
static const struct {
    const char *name;
    uint32_t w, h;
} PRESETS[] = {
    {"iPhone 14", 390, 844},        {"iPhone 14 Pro Max", 430, 932}, {"Android 1080p", 360, 640},
    {"iPad Pro 12.9", 1024, 1366},  {"Web 1280", 1280, 800},         {"Web 1366", 1366, 768},
    {"Web 1920", 1920, 1080},       {"Instagram Post", 1080, 1080},  {"Letter", 612, 792},
    {"A4", 595, 842},
};

bool pc_engine_artboard_preset_size(const char *name, uint32_t *w, uint32_t *h)
{
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof PRESETS / sizeof PRESETS[0]; i++) {
        if (strcasecmp(PRESETS[i].name, name) == 0) {
            *w = PRESETS[i].w;
            *h = PRESETS[i].h;
            return true;
        }
    }
    return false;
}

/* Append printf-formatted text (ASCII/Latin-1 only; the PDF body is byte-oriented). */
static pc_status appendf(pc_buf *b, pc_error *err, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return pc_fail(err, PC_ERR_INVALID, "PDF object too long");
    }
    return pc_buf_append(b, tmp, (size_t)n, err);
}

pc_status pc_engine_artboard_raster_pdf(const pc_engine_pdf_page *pages, size_t n, pc_buf *out,
                                        pc_error *err)
{
    pc_buf_init(out);
    /* Objects: 1 catalog, 2 page tree, then per page: page, contents, image. */
    size_t nobj = 2 + n * 3;
    size_t *offsets = NULL;
    if (n > (SIZE_MAX - 2) / 3 ||
        (offsets = calloc(nobj > 0 ? nobj : 1, sizeof *offsets)) == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t nused = 0;
    pc_status rc = pc_buf_append(out, "%PDF-1.4\n%\xe2\xe3\xcf\xd3\n", 14, err);

/* Record an object's offset and number (1-based), appending its "k 0 obj\n". */
#define OBJ_BEGIN()                                                                                \
    do {                                                                                           \
        offsets[nused] = out->len;                                                                 \
        nused++;                                                                                   \
        if (rc == PC_OK) {                                                                         \
            rc = appendf(out, err, "%zu 0 obj\n", nused);                                          \
        }                                                                                          \
    } while (0)
#define OBJ_END()                                                                                  \
    do {                                                                                           \
        if (rc == PC_OK) {                                                                         \
            rc = pc_buf_append(out, "\nendobj\n", 8, err);                                         \
        }                                                                                          \
    } while (0)

    OBJ_BEGIN();
    if (rc == PC_OK) {
        rc = pc_buf_append(out, "<< /Type /Catalog /Pages 2 0 R >>", 33, err);
    }
    OBJ_END();

    OBJ_BEGIN();
    if (rc == PC_OK) {
        rc = pc_buf_append(out, "<< /Type /Pages /Kids [", 23, err);
    }
    for (size_t i = 0; rc == PC_OK && i < n; i++) {
        rc = appendf(out, err, "%s%zu 0 R", i == 0 ? "" : " ", 3 + i * 3);
    }
    if (rc == PC_OK) {
        rc = appendf(out, err, "] /Count %zu >>", n);
    }
    OBJ_END();

    for (size_t i = 0; rc == PC_OK && i < n; i++) {
        double dpi = pages[i].dpi > 1.0f ? (double)pages[i].dpi : 1.0;
        double k = 72.0 / dpi;
        double pw = (double)pages[i].w * k;
        double ph = (double)pages[i].h * k;
        size_t contents = 4 + i * 3;
        size_t image = 5 + i * 3;
        OBJ_BEGIN();
        if (rc == PC_OK) {
            rc = appendf(out, err,
                         "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.3f %.3f] /Resources << "
                         "/XObject << /Im0 %zu 0 R >> >> /Contents %zu 0 R >>",
                         pw, ph, image, contents);
        }
        OBJ_END();

        char stream[128];
        int sn = snprintf(stream, sizeof stream, "q %.3f 0 0 %.3f 0 0 cm /Im0 Do Q", pw, ph);
        if (sn < 0 || (size_t)sn >= sizeof stream) {
            rc = pc_fail(err, PC_ERR_INVALID, "PDF stream too long");
        }
        OBJ_BEGIN();
        if (rc == PC_OK) {
            rc = appendf(out, err, "<< /Length %d >>\nstream\n%s\nendstream", sn, stream);
        }
        OBJ_END();

        OBJ_BEGIN();
        if (rc == PC_OK) {
            rc = appendf(out, err,
                         "<< /Type /XObject /Subtype /Image /Width %u /Height %u /ColorSpace "
                         "/DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %zu >>\nstream\n",
                         pages[i].w, pages[i].h, pages[i].jpeg_len);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append(out, pages[i].jpeg, pages[i].jpeg_len, err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append(out, "\nendstream", 10, err);
        }
        OBJ_END();
    }

    size_t xref = out->len;
    if (rc == PC_OK) {
        rc = appendf(out, err, "xref\n0 %zu\n0000000000 65535 f \n", nused + 1);
    }
    for (size_t i = 0; rc == PC_OK && i < nused; i++) {
        rc = appendf(out, err, "%010zu 00000 n \n", offsets[i]);
    }
    if (rc == PC_OK) {
        rc = appendf(out, err, "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n",
                     nused + 1, xref);
    }
    free(offsets);
    if (rc != PC_OK) {
        pc_buf_fini(out);
    }
    return rc;
#undef OBJ_BEGIN
#undef OBJ_END
}

/* ============================================================== View > Clear Selected Artboard
 * Guides. The rest of the artboard command surface (Layer > New > Artboard family, properties,
 * exports) is deferred (see artboard_cmds.h). */

/* The active layer's artboard (itself or its top-level ancestor); false when there is none. */
static bool active_artboard(const pc_engine_session *s, pc_layer_id *out)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return false;
    }
    return pc_document_artboard_of(pc_doc_arc_get(d->doc), d->active_layer, out);
}

static pc_status has_active_artboard(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id id;
    if (!active_artboard(s, &id)) {
        return pc_fail(reason, PC_ERR_INVALID, "select an artboard (or a layer inside one)");
    }
    return PC_OK;
}

static pc_status clear_guides_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                   pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    const pc_rect *r = vctx;
    size_t w = 0;
    for (size_t i = 0; i < doc->guides.nvertical; i++) {
        double x = doc->guides.vertical[i];
        if (!(x > (double)r->x0 && x < (double)r->x1)) {
            doc->guides.vertical[w++] = x;
        }
    }
    doc->guides.nvertical = w;
    w = 0;
    for (size_t i = 0; i < doc->guides.nhorizontal; i++) {
        double y = doc->guides.horizontal[i];
        if (!(y > (double)r->y0 && y < (double)r->y1)) {
            doc->guides.horizontal[w++] = y;
        }
    }
    doc->guides.nhorizontal = w;
    pc_guides_prune_colors(&doc->guides);
    return PC_OK;
}

static pc_status run_clear_artboard_guides(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                           pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id id;
    if (!active_artboard(s, &id)) {
        return pc_engine_err_other(err, "no artboard selected");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    const pc_artboard *a = l != NULL ? pc_layer_artboard(l) : NULL;
    if (a == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_rect r = a->rect;
    size_t n = 0;
    for (size_t i = 0; i < doc->guides.nvertical; i++) {
        double x = doc->guides.vertical[i];
        if (x > (double)r.x0 && x < (double)r.x1) {
            n++;
        }
    }
    for (size_t i = 0; i < doc->guides.nhorizontal; i++) {
        double y = doc->guides.horizontal[i];
        if (y > (double)r.y0 && y < (double)r.y1) {
            n++;
        }
    }
    if (n > 0) {
        pc_status rc = pc_engine_session_edit(s, "Clear Selected Artboard Guides", clear_guides_edit, &r, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, o);
    (void)yyjson_mut_obj_add_uint(rd, o, "cleared", n);
    return PC_OK;
}

/* ============================================================== Layer > New > Artboard family,
 * artboard properties, Add Artboard Beside and File > Export > Artboards (ET3). */

#define AB_GAP 100               /* gap Photoshop leaves between a new board and the last one */
#define AB_MAX_DIM 300000        /* document_preset_cmds::MAX_DIMENSION */

static pc_status ab_bad(pc_error *err, const char *cmd, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_engine_err_bad_params(err, cmd, "%s", msg);
}

static const char *ab_str(yyjson_val *p, const char *key)
{
    yyjson_val *v = pc_json_get(p, key);
    return v != NULL ? yyjson_get_str(v) : NULL; /* NULL when absent or not a string */
}

static bool ab_color_parse(yyjson_val *v, pc_color *out)
{
    if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        float c[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        size_t m = yyjson_arr_size(v);
        for (size_t i = 0; i < 4 && i < m; i++) {
            double d;
            if (pc_json_as_f64(yyjson_arr_get(v, i), &d)) {
                c[i] = (float)d;
            }
        }
        *out = pc_color_rgba(c[0], c[1], c[2], c[3]);
        return true;
    }
    size_t sl = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &sl) : NULL;
    if (s != NULL) {
        float c[4];
        if (pc_engine_parse_hex(s, sl, c)) {
            *out = pc_color_rgba(c[0], c[1], c[2], c[3]);
            return true;
        }
    }
    return false;
}

/* The background param: *present stays false when neither "background" nor "color" is given. */
static pc_status ab_background(yyjson_val *p, const char *cmd, bool *present,
                              pc_artboard_background *bg, pc_error *err)
{
    *present = false;
    bool has_custom = false;
    pc_artboard_background custom = {PC_ARTBOARD_CUSTOM, {0}};
    yyjson_val *cv = pc_json_get(p, "color");
    if (cv != NULL) { /* any present value, even null, means custom (default white) */
        pc_color c;
        custom.custom = ab_color_parse(cv, &c) ? c : pc_color_rgba(1.0f, 1.0f, 1.0f, 1.0f);
        has_custom = true;
    }
    const char *b = ab_str(p, "background");
    if (b == NULL) {
        if (has_custom) {
            *bg = custom;
            *present = true;
        }
        return PC_OK;
    }
    if (strcmp(b, "white") == 0) {
        bg->kind = PC_ARTBOARD_WHITE;
    } else if (strcmp(b, "black") == 0) {
        bg->kind = PC_ARTBOARD_BLACK;
    } else if (strcmp(b, "transparent") == 0) {
        bg->kind = PC_ARTBOARD_TRANSPARENT;
    } else if (strcmp(b, "custom") == 0 || strcmp(b, "other") == 0) {
        if (has_custom) {
            *bg = custom;
        } else {
            bg->kind = PC_ARTBOARD_WHITE;
        }
    } else {
        return ab_bad(err, cmd, "unknown background `%s` (white|black|transparent|custom)", b);
    }
    *present = true;
    return PC_OK;
}

/* A board rect from [x, y, w, h] (i64), or a bad-params error when an edge leaves the i32 range. */
static pc_status board_rect(const char *cmd, int64_t x, int64_t y, int64_t w, int64_t h, pc_rect *out,
                            pc_error *err)
{
    if (w <= 0 || h <= 0) {
        return ab_bad(err, cmd, "the artboard size must be positive (width = %lld, height = %lld)",
                      (long long)w, (long long)h);
    }
    int64_t x1 = x + w, y1 = y + h;
    if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX || x1 < INT32_MIN ||
        x1 > INT32_MAX || y1 < INT32_MIN || y1 > INT32_MAX) {
        return ab_bad(err, cmd,
                      "the artboard [x, y, width, height] = [%lld, %lld, %lld, %lld] reaches "
                      "outside the 32-bit coordinate range",
                      (long long)x, (long long)y, (long long)w, (long long)h);
    }
    *out = pc_rect_make((int32_t)x, (int32_t)y, (int32_t)x1, (int32_t)y1);
    return PC_OK;
}

/* `rect` as [x, y, w, h] or x/y/width/height over `base` ([x, y, w, h]). */
static pc_status rect_param(yyjson_val *p, const int64_t base[4], const char *cmd, pc_rect *out,
                            pc_error *err)
{
    yyjson_val *ra = pc_json_get(p, "rect");
    if (ra != NULL && yyjson_is_arr(ra)) {
        if (yyjson_arr_size(ra) != 4) {
            return ab_bad(err, cmd, "\"rect\" must be [x, y, width, height] with a positive size");
        }
        int64_t v[4];
        for (size_t i = 0; i < 4; i++) {
            yyjson_val *e = yyjson_arr_get(ra, i);
            double d;
            if (pc_json_as_i64(e, &v[i])) {
                continue;
            }
            if (pc_json_as_f64(e, &d) && isfinite(d)) {
                v[i] = (int64_t)llround(d);
            } else {
                return ab_bad(err, cmd, "\"rect\" must be [x, y, width, height] with a positive size");
            }
        }
        return board_rect(cmd, v[0], v[1], v[2], v[3], out, err);
    }
    bool present = false;
    int32_t xi = 0, yi = 0;
    int64_t x = base[0], y = base[1], w = base[2], h = base[3];
    if (pc_engine_param_int_i32(cmd, p, "x", &present, &xi, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (present) {
        x = (int64_t)xi;
    }
    if (pc_engine_param_int_i32(cmd, p, "y", &present, &yi, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (present) {
        y = (int64_t)yi;
    }
    int64_t tmp;
    if (pc_engine_param_int(p, "width", &tmp)) {
        w = tmp;
    }
    if (pc_engine_param_int(p, "height", &tmp)) {
        h = tmp;
    }
    return board_rect(cmd, x, y, w, h, out, err);
}

static pc_status artboard_result(pc_json_value *out, pc_layer_id id, pc_rect r, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", id);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, arr, r.x0);
    yyjson_mut_arr_add_int(d, arr, r.y0);
    yyjson_mut_arr_add_uint(d, arr, pc_rect_width(r));
    yyjson_mut_arr_add_uint(d, arr, pc_rect_height(r));
    yyjson_mut_obj_add_val(d, root, "rect", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static void next_artboard_name(const pc_document *doc, char *out, size_t cap)
{
    pc_artboard_ref *refs = NULL;
    size_t n = 0;
    (void)pc_document_artboards(doc, &refs, &n, NULL);
    for (size_t cand = 1; cand <= n + 1; cand++) {
        char buf[32];
        int bn = snprintf(buf, sizeof buf, "Artboard %zu", cand);
        if (bn < 0) {
            continue;
        }
        bool taken = false;
        for (size_t i = 0; i < n; i++) {
            const pc_str *nm = refs[i].name;
            if (nm != NULL && nm->len == (size_t)bn && memcmp(nm->ptr, buf, nm->len) == 0) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            (void)snprintf(out, cap, "%s", buf);
            free(refs);
            return;
        }
    }
    (void)snprintf(out, cap, "Artboard");
    free(refs);
}

static void set_artboard(pc_layer *l, pc_rect rect, pc_artboard_background bg, const char *preset)
{
    if (l->content.kind != PC_LAYER_GROUP) {
        return;
    }
    pc_group *g = &l->content.u.group;
    if (g->has_artboard) {
        pc_artboard_fini(&g->artboard);
    }
    pc_artboard_init(&g->artboard, rect); /* white, no preset */
    g->artboard.background = bg;
    if (preset != NULL && preset[0] != '\0') {
        (void)pc_str_set(&g->artboard.preset, preset, strlen(preset), NULL);
    }
    g->has_artboard = true;
    g->expanded = true;
    l->clipped = false; /* artboards composite like pass-through groups */
}

/* Grow the canvas so every artboard fits (artboard_cmds::fit_canvas). */
static void fit_canvas(pc_document *doc)
{
    pc_artboard_ref *refs = NULL;
    size_t n = 0;
    if (pc_document_artboards(doc, &refs, &n, NULL) != PC_OK) {
        return;
    }
    int64_t x0 = 0, x1 = doc->size.width, y0 = 0, y1 = doc->size.height;
    for (size_t i = 0; i < n; i++) {
        pc_rect r = refs[i].artboard->rect;
        if (r.x0 < x0) {
            x0 = r.x0;
        }
        if (r.x1 > x1) {
            x1 = r.x1;
        }
        if (r.y0 < y0) {
            y0 = r.y0;
        }
        if (r.y1 > y1) {
            y1 = r.y1;
        }
    }
    free(refs);
    int32_t sx = (x1 - x0 <= AB_MAX_DIM && -x0 <= INT32_MAX) ? (int32_t)(-x0) : 0;
    int32_t sy = (y1 - y0 <= AB_MAX_DIM && -y0 <= INT32_MAX) ? (int32_t)(-y0) : 0;
    if ((sx != 0 || sy != 0) && pc_engine_translate_doc(doc, "artboard", sx, sy, NULL) != PC_OK) {
        sx = 0;
        sy = 0;
    }
    int64_t nw = x1 + sx, nh = y1 + sy;
    if (nw < 1) {
        nw = 1;
    }
    if (nw > AB_MAX_DIM) {
        nw = AB_MAX_DIM;
    }
    if (nh < 1) {
        nh = 1;
    }
    if (nh > AB_MAX_DIM) {
        nh = AB_MAX_DIM;
    }
    pc_size ns = pc_size_make((uint32_t)nw, (uint32_t)nh);
    if (ns.width != doc->size.width || ns.height != doc->size.height) {
        doc->size = ns;
        pc_engine_canvas_refresh(doc, false); /* Refresh::Shapes */
    }
}

/* ---- enablement --------------------------------------------------------------------------- */

static pc_status has_artboards(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!pc_document_has_artboards(pc_doc_arc_get(d->doc))) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no artboards");
    }
    return PC_OK;
}

static pc_status has_plain_group(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (!pc_layer_is_group(l) || pc_layer_artboard(l) != NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "select a layer group");
    }
    bool top = false;
    for (size_t i = 0; i < doc->nlayers; i++) {
        if (doc->layers[i].id == d->active_layer) {
            top = true;
            break;
        }
    }
    if (!top) {
        return pc_fail(reason, PC_ERR_INVALID,
                       "artboards must be top-level: select a group that is not nested");
    }
    return PC_OK;
}

static pc_status has_free_layers(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (d->nselected == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "select one or more layers");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    for (size_t i = 0; i < d->nselected; i++) {
        pc_layer_id ab;
        if (pc_document_artboard_of(doc, d->selected_layers[i], &ab)) {
            return pc_fail(reason, PC_ERR_INVALID, "the selection is already in an artboard");
        }
    }
    return PC_OK;
}

static pc_status export_artboards_en(const pc_engine_session *s, pc_error *reason)
{
    /* native() is always available off the web; the document just needs artboards. */
    return has_artboards(s, reason);
}

/* ---- Layer > New > Artboard --------------------------------------------------------------- */

typedef struct {
    pc_rect rect;
    pc_artboard_background bg;
    bool has_name;
    char name[256];
    char preset[64];
    pc_layer_id id; /* out */
} new_artboard_ctx;

static pc_status new_artboard_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                   pc_error *err)
{
    (void)ha;
    new_artboard_ctx *c = vctx;
    char namebuf[256];
    const char *name = c->name;
    if (!c->has_name) {
        next_artboard_name(doc, namebuf, sizeof namebuf);
        name = namebuf;
    }
    pc_layer g;
    if (pc_layer_group(&g, name, NULL, 0, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    set_artboard(&g, c->rect, c->bg, c->preset);
    pc_layer_id id = 0;
    if (pc_document_insert_above(doc, NULL, &g, &id, err) != PC_OK) {
        pc_layer_fini(&g);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    fit_canvas(doc);
    c->id = id;
    *active = id;
    return PC_OK;
}

static pc_status run_new_artboard(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.new.artboard";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const char *preset = ab_str(p, "preset");
    if (preset == NULL) {
        preset = "";
    }
    uint32_t pw = doc->size.width, ph = doc->size.height;
    if (preset[0] != '\0' && !pc_engine_artboard_preset_size(preset, &pw, &ph)) {
        return ab_bad(err, cmd, "unknown preset `%s`", preset);
    }
    /* Default placement: right of the rightmost board, else the canvas origin. */
    pc_artboard_ref *refs = NULL;
    size_t nref = 0;
    (void)pc_document_artboards(doc, &refs, &nref, NULL);
    bool has_right = false;
    int64_t right = 0, top = 0;
    for (size_t i = 0; i < nref; i++) {
        pc_rect r = refs[i].artboard->rect;
        if (!has_right || r.x1 > right) {
            right = r.x1;
        }
        if (!has_right || r.y0 < top) {
            top = r.y0;
        }
        has_right = true;
    }
    free(refs);
    int64_t base[4];
    if (has_right) {
        base[0] = right + AB_GAP;
        base[1] = top;
    } else {
        base[0] = 0;
        base[1] = 0;
    }
    base[2] = (int64_t)pw;
    base[3] = (int64_t)ph;
    yyjson_val *rectv = pc_json_get(p, "rect");
    yyjson_val *xv = pc_json_get(p, "x");
    bool explicit_x = rectv != NULL || (xv != NULL && !yyjson_is_null(xv));
    if (has_right && !explicit_x && base[0] > INT32_MAX) {
        return ab_bad(err, cmd,
                      "no room for a new artboard right of the rightmost one (its right edge is x = "
                      "%lld); pass \"x\" to place it",
                      (long long)right);
    }
    new_artboard_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    pc_status rc;
    /* A preset decides the size (dialogs send the canvas size alongside it). */
    if (preset[0] == '\0' || rectv != NULL) {
        rc = rect_param(p, base, cmd, &ctx.rect, err);
    } else {
        /* Only x/y apply; width/height come from the preset. Build a params-like read. */
        int64_t b2[4] = {base[0], base[1], base[2], base[3]};
        int32_t xi = 0, yi = 0;
        bool pr = false;
        if (pc_engine_param_int_i32(cmd, p, "x", &pr, &xi, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        if (pr) {
            b2[0] = xi;
        }
        if (pc_engine_param_int_i32(cmd, p, "y", &pr, &yi, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        if (pr) {
            b2[1] = yi;
        }
        rc = board_rect(cmd, b2[0], b2[1], b2[2], b2[3], &ctx.rect, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    bool has_bg = false;
    pc_artboard_background bg = {PC_ARTBOARD_WHITE, {0}};
    rc = ab_background(p, cmd, &has_bg, &bg, err);
    if (rc != PC_OK) {
        return rc;
    }
    ctx.bg = bg; /* default white when absent */
    const char *nm = ab_str(p, "name");
    if (nm != NULL && nm[0] != '\0') {
        ctx.has_name = true;
        (void)snprintf(ctx.name, sizeof ctx.name, "%s", nm);
    }
    (void)snprintf(ctx.preset, sizeof ctx.preset, "%s", preset);
    rc = pc_engine_session_edit(s, "New Artboard", new_artboard_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id ids[1] = {ctx.id};
    (void)pc_engine_set_selection(s, ids, 1, true, ctx.id, true, ctx.id, NULL);
    return artboard_result(out, ctx.id, ctx.rect, err);
}

/* ---- Layer > New > Artboard from Group ---------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    bool has_bg;
    pc_artboard_background bg;
    pc_rect rect; /* out */
} from_group_ctx;

static pc_status from_group_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    (void)ha;
    from_group_ctx *c = vctx;
    bool ok = false;
    for (size_t i = 0; i < doc->nlayers; i++) {
        const pc_layer *t = &doc->layers[i];
        if (t->id == c->id && pc_layer_is_group(t) && pc_layer_artboard(t) == NULL) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        return pc_engine_err_other(err, "artboards are made from a top-level layer group");
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_rect rect;
    if (!pc_engine_layer_bounds(l, &rect)) {
        rect = pc_document_bounds(doc);
    }
    pc_artboard_background bg = c->has_bg ? c->bg : (pc_artboard_background){PC_ARTBOARD_WHITE, {0}};
    set_artboard(l, rect, bg, "");
    fit_canvas(doc);
    c->rect = rect;
    *active = c->id;
    return PC_OK;
}

static pc_status run_from_group(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    from_group_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    uint64_t lv;
    yyjson_val *lp = pc_json_get(p, "layer");
    if (lp != NULL && pc_json_as_u64(lp, &lv)) {
        ctx.id = (pc_layer_id)lv;
    } else if (d->has_active_layer) {
        ctx.id = d->active_layer;
    } else {
        return pc_engine_err_other(err, "no active layer");
    }
    pc_status rc = ab_background(p, "layer.new.artboardFromGroup", &ctx.has_bg, &ctx.bg, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, "Artboard from Group", from_group_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return artboard_result(out, ctx.id, ctx.rect, err);
}

/* ---- Layer > New > Artboard from Layers --------------------------------------------------- */

typedef struct {
    pc_layer_id *sel;
    size_t nsel;
    bool has_name;
    char name[256];
    bool has_bg;
    pc_artboard_background bg;
    pc_layer_id id;   /* out */
    pc_rect rect;     /* out */
} from_layers_ctx;

static pc_status from_layers_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                  pc_error *err)
{
    (void)ha;
    from_layers_ctx *c = vctx;
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (pc_engine_top_level(doc, c->sel, c->nsel, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (nids == 0) {
        free(ids);
        return pc_engine_err_other(err, "select layers that are not in an artboard");
    }
    for (size_t i = 0; i < nids; i++) {
        pc_layer_id ab;
        if (pc_document_artboard_of(doc, ids[i], &ab)) {
            free(ids);
            return pc_engine_err_other(err, "select layers that are not in an artboard");
        }
    }
    pc_layer *children = pc_malloc_array(nids, sizeof(pc_layer));
    if (children == NULL) {
        free(ids);
        return pc_fail(err, PC_ERR_NOMEM, "Artboard from Layers");
    }
    size_t nc = 0;
    pc_rect rect = PC_RECT_EMPTY;
    bool have_rect = false;
    for (size_t i = 0; i < nids; i++) {
        if (pc_document_remove(doc, ids[i], &children[nc], err) != PC_OK) {
            pc_layer_id missing = ids[i];
            for (size_t j = 0; j < nc; j++) {
                pc_layer_fini(&children[j]);
            }
            free(children);
            free(ids);
            return pc_engine_err_no_layer(err, missing);
        }
        pc_rect lb;
        if (pc_engine_layer_bounds(&children[nc], &lb)) {
            rect = have_rect ? pc_rect_union(rect, lb) : lb;
            have_rect = true;
        }
        nc++;
    }
    free(ids);
    if (!have_rect) {
        rect = pc_document_bounds(doc);
    }
    char namebuf[256];
    const char *name = c->name;
    if (!c->has_name) {
        next_artboard_name(doc, namebuf, sizeof namebuf);
        name = namebuf;
    }
    pc_layer g;
    if (pc_layer_group(&g, name, children, nc, err) != PC_OK) {
        for (size_t j = 0; j < nc; j++) {
            pc_layer_fini(&children[j]);
        }
        free(children);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_artboard_background bg = c->has_bg ? c->bg : (pc_artboard_background){PC_ARTBOARD_WHITE, {0}};
    set_artboard(&g, rect, bg, "");
    pc_layer_id gid = 0;
    if (pc_document_insert_above(doc, NULL, &g, &gid, err) != PC_OK) {
        pc_layer_fini(&g);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    size_t depth = 0;
    if (pc_document_max_group_depth(doc, &depth, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (depth > PC_MAX_GROUP_DEPTH) {
        return pc_engine_err_other(err, "Artboard from Layers would nest layers deeper than %d groups",
                                   PC_MAX_GROUP_DEPTH);
    }
    fit_canvas(doc);
    c->id = gid;
    c->rect = rect;
    *active = gid;
    return PC_OK;
}

static pc_status run_from_layers(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    from_layers_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    if (pc_engine_selected(s, &ctx.sel, &ctx.nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    const char *nm = ab_str(p, "name");
    if (nm != NULL) {
        ctx.has_name = true;
        (void)snprintf(ctx.name, sizeof ctx.name, "%s", nm);
    }
    pc_status rc = ab_background(p, "layer.new.artboardFromLayers", &ctx.has_bg, &ctx.bg, err);
    if (rc != PC_OK) {
        free(ctx.sel);
        return rc;
    }
    rc = pc_engine_session_edit(s, "Artboard from Layers", from_layers_edit, &ctx, err);
    free(ctx.sel);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id ids[1] = {ctx.id};
    (void)pc_engine_set_selection(s, ids, 1, true, ctx.id, true, ctx.id, NULL);
    return artboard_result(out, ctx.id, ctx.rect, err);
}

/* ---- layer.artboard.set (Edit Artboard) --------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    pc_rect rect;
    int32_t dx, dy;
    bool move_contents;
    bool has_name;
    char name[256];
    bool has_bg;
    pc_artboard_background bg;
    bool has_preset;
    char preset[64];
    bool size_changed; /* rect size differs from the old one */
} set_props_ctx;

static pc_status set_props_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                pc_error *err)
{
    (void)ha;
    (void)active;
    set_props_ctx *c = vctx;
    if (c->move_contents && (c->dx != 0 || c->dy != 0)) {
        pc_document snap;
        if (pc_document_clone(doc, &snap, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        pc_layer *l = pc_document_layer_mut(doc, c->id);
        if (l == NULL) {
            pc_document_fini(&snap);
            return pc_engine_err_no_layer(err, c->id);
        }
        pc_engine_translate_layer(&snap, l, c->dx, c->dy);
        pc_engine_translate_vectors(&snap, l, (double)c->dx, (double)c->dy);
        pc_document_fini(&snap);
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    if (c->has_name) {
        (void)pc_str_set(&l->name, c->name, strlen(c->name), NULL);
    }
    pc_artboard *a = pc_layer_artboard_mut(l);
    if (a == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    a->rect = c->rect;
    if (c->has_bg) {
        a->background = c->bg;
    }
    if (c->has_preset) {
        (void)pc_str_set(&a->preset, c->preset, strlen(c->preset), NULL);
    } else if (c->size_changed) {
        (void)pc_str_set(&a->preset, "", 0, NULL); /* a hand-edited size no longer matches */
    }
    fit_canvas(doc);
    return PC_OK;
}

static pc_status run_set_props(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.artboard.set";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    set_props_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    uint64_t lv;
    yyjson_val *lp = pc_json_get(p, "layer");
    if (lp != NULL && pc_json_as_u64(lp, &lv)) {
        ctx.id = (pc_layer_id)lv;
    } else if (!active_artboard(s, &ctx.id)) {
        return ab_bad(err, cmd, "no artboard selected");
    }
    const pc_layer *l = pc_document_layer(doc, ctx.id);
    const pc_artboard *old = l != NULL ? pc_layer_artboard(l) : NULL;
    if (old == NULL) {
        return ab_bad(err, cmd, "the layer is not an artboard");
    }
    pc_rect oldrect = old->rect;
    int64_t base[4] = {oldrect.x0, oldrect.y0, pc_rect_width(oldrect), pc_rect_height(oldrect)};
    const char *preset = ab_str(p, "preset");
    if (preset != NULL && preset[0] != '\0') {
        uint32_t pw, ph;
        if (!pc_engine_artboard_preset_size(preset, &pw, &ph)) {
            return ab_bad(err, cmd, "unknown preset `%s`", preset);
        }
        base[2] = pw;
        base[3] = ph;
    }
    pc_status rc = rect_param(p, base, cmd, &ctx.rect, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = ab_background(p, cmd, &ctx.has_bg, &ctx.bg, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *nm = ab_str(p, "name");
    if (nm != NULL) {
        ctx.has_name = true;
        (void)snprintf(ctx.name, sizeof ctx.name, "%s", nm);
    }
    if (preset != NULL) {
        ctx.has_preset = true;
        (void)snprintf(ctx.preset, sizeof ctx.preset, "%s", preset);
    }
    yyjson_val *mc = pc_json_get(p, "moveContents");
    ctx.move_contents = (mc == NULL || !yyjson_is_bool(mc)) ? true : yyjson_get_bool(mc);
    int64_t dx = (int64_t)ctx.rect.x0 - (int64_t)oldrect.x0;
    int64_t dy = (int64_t)ctx.rect.y0 - (int64_t)oldrect.y0;
    if (dx >= INT32_MIN && dx <= INT32_MAX && dy >= INT32_MIN && dy <= INT32_MAX) {
        ctx.dx = (int32_t)dx;
        ctx.dy = (int32_t)dy;
    } else if (!ctx.move_contents) {
        ctx.dx = 0;
        ctx.dy = 0;
    } else {
        return ab_bad(err, cmd,
                      "moving the artboard with its contents by (%lld, %lld) is outside the 32-bit "
                      "range; move it in smaller steps or pass \"moveContents\": false",
                      (long long)dx, (long long)dy);
    }
    ctx.size_changed =
        pc_rect_width(ctx.rect) != pc_rect_width(oldrect) || pc_rect_height(ctx.rect) != pc_rect_height(oldrect);
    rc = pc_engine_session_edit(s, "Edit Artboard", set_props_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return artboard_result(out, ctx.id, ctx.rect, err);
}

/* ---- layer.artboard.addBeside ------------------------------------------------------------- */

typedef struct {
    pc_layer_id id;
    pc_rect rect;    /* placement before the document shift */
    int32_t sx, sy;  /* document shift so the board fits from the origin */
    bool duplicate;
    pc_artboard_background bg;
    char preset[64];
    pc_layer_id nid; /* out */
    pc_rect outrect; /* out (shifted) */
} add_beside_ctx;

static pc_status add_beside_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    (void)ha;
    add_beside_ctx *c = vctx;
    const char *cmd = "layer.artboard.addBeside";
    if (c->sx != 0 || c->sy != 0) {
        pc_status rc = pc_engine_translate_doc(doc, cmd, c->sx, c->sy, err);
        if (rc != PC_OK) {
            return rc;
        }
        int64_t nw = (int64_t)doc->size.width + c->sx, nh = (int64_t)doc->size.height + c->sy;
        if (nw < 1) {
            nw = 1;
        }
        if (nw > AB_MAX_DIM) {
            nw = AB_MAX_DIM;
        }
        if (nh < 1) {
            nh = 1;
        }
        if (nh > AB_MAX_DIM) {
            nh = AB_MAX_DIM;
        }
        doc->size = pc_size_make((uint32_t)nw, (uint32_t)nh);
        pc_engine_canvas_refresh(doc, false);
    }
    int64_t x0 = (int64_t)c->rect.x0 + c->sx, y0 = (int64_t)c->rect.y0 + c->sy;
    int64_t x1 = (int64_t)c->rect.x1 + c->sx, y1 = (int64_t)c->rect.y1 + c->sy;
    if (x0 < INT32_MIN || x1 > INT32_MAX || y0 < INT32_MIN || y1 > INT32_MAX) {
        return ab_bad(err, cmd, "the new artboard is outside the 32-bit coordinate range");
    }
    pc_rect rect = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
    pc_layer_id nid = 0;
    if (c->duplicate) {
        const pc_layer *src = pc_document_layer(doc, c->id);
        const pc_artboard *sab = src != NULL ? pc_layer_artboard(src) : NULL;
        if (sab == NULL) {
            return pc_engine_err_no_layer(err, c->id);
        }
        int32_t dx = rect.x0 - sab->rect.x0, dy = rect.y0 - sab->rect.y0;
        pc_layer copy;
        if (pc_engine_layer_copy(doc, c->id, &copy, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (pc_document_insert_above(doc, &c->id, &copy, &nid, err) != PC_OK) {
            pc_layer_fini(&copy);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        /* Grow the canvas first, so shapes on the board re-render uncut where they land. */
        int64_t gw = (int64_t)doc->size.width, gh = (int64_t)doc->size.height;
        if (rect.x1 > gw) {
            gw = rect.x1;
        }
        if (rect.y1 > gh) {
            gh = rect.y1;
        }
        if (gw > AB_MAX_DIM) {
            gw = AB_MAX_DIM;
        }
        if (gh > AB_MAX_DIM) {
            gh = AB_MAX_DIM;
        }
        doc->size = pc_size_make((uint32_t)(gw < 1 ? 1 : gw), (uint32_t)(gh < 1 ? 1 : gh));
        pc_document snap;
        if (pc_document_clone(doc, &snap, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        pc_layer *l = pc_document_layer_mut(doc, nid);
        if (l == NULL) {
            pc_document_fini(&snap);
            return pc_engine_err_no_layer(err, nid);
        }
        pc_engine_translate_layer(&snap, l, dx, dy);
        pc_engine_translate_vectors(&snap, l, (double)dx, (double)dy);
        pc_document_fini(&snap);
    } else {
        char namebuf[256];
        next_artboard_name(doc, namebuf, sizeof namebuf);
        pc_layer g;
        if (pc_layer_group(&g, namebuf, NULL, 0, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        set_artboard(&g, rect, c->bg, c->preset);
        if (pc_document_insert_above(doc, &c->id, &g, &nid, err) != PC_OK) {
            pc_layer_fini(&g);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
    }
    fit_canvas(doc);
    c->nid = nid;
    c->outrect = rect;
    *active = nid;
    return PC_OK;
}

static pc_status run_add_beside(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "layer.artboard.addBeside";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    add_beside_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    uint64_t lv;
    yyjson_val *lp = pc_json_get(p, "layer");
    if (lp != NULL && pc_json_as_u64(lp, &lv)) {
        ctx.id = (pc_layer_id)lv;
    } else if (!active_artboard(s, &ctx.id)) {
        return ab_bad(err, cmd, "no artboard selected");
    }
    const pc_layer *l = pc_document_layer(doc, ctx.id);
    const pc_artboard *src = l != NULL ? pc_layer_artboard(l) : NULL;
    if (src == NULL) {
        return ab_bad(err, cmd, "the layer is not an artboard");
    }
    ctx.bg = src->background;
    (void)snprintf(ctx.preset, sizeof ctx.preset, "%s",
                   src->preset.ptr != NULL ? src->preset.ptr : "");
    const char *side = ab_str(p, "side");
    if (side == NULL) {
        side = "right";
    }
    yyjson_val *dv = pc_json_get(p, "duplicate");
    ctx.duplicate = (dv != NULL && yyjson_is_bool(dv)) ? yyjson_get_bool(dv) : false;
    int64_t x0 = src->rect.x0, y0 = src->rect.y0, w = pc_rect_width(src->rect), h = pc_rect_height(src->rect);
    int64_t gap = AB_GAP;
    int64_t x, y;
    if (strcmp(side, "right") == 0) {
        x = x0 + w + gap;
        y = y0;
    } else if (strcmp(side, "left") == 0) {
        x = x0 - gap - w;
        y = y0;
    } else if (strcmp(side, "bottom") == 0) {
        x = x0;
        y = y0 + h + gap;
    } else if (strcmp(side, "top") == 0) {
        x = x0;
        y = y0 - gap - h;
    } else {
        return ab_bad(err, cmd, "unknown side `%s` (left|right|top|bottom)", side);
    }
    pc_artboard_ref *refs = NULL;
    size_t nref = 0;
    (void)pc_document_artboards(doc, &refs, &nref, NULL);
    for (size_t round = 0; round <= nref; round++) {
        bool blocked = false;
        int64_t edge = 0;
        bool have_edge = false;
        for (size_t i = 0; i < nref; i++) {
            pc_rect b = refs[i].artboard->rect;
            if ((int64_t)b.x0 < x + w && (int64_t)b.x1 > x && (int64_t)b.y0 < y + h && (int64_t)b.y1 > y) {
                blocked = true;
                int64_t e;
                if (strcmp(side, "right") == 0) {
                    e = b.x1;
                    if (!have_edge || e > edge) {
                        edge = e;
                    }
                } else if (strcmp(side, "left") == 0) {
                    e = b.x0;
                    if (!have_edge || e < edge) {
                        edge = e;
                    }
                } else if (strcmp(side, "bottom") == 0) {
                    e = b.y1;
                    if (!have_edge || e > edge) {
                        edge = e;
                    }
                } else {
                    e = b.y0;
                    if (!have_edge || e < edge) {
                        edge = e;
                    }
                }
                have_edge = true;
            }
        }
        if (!blocked) {
            break;
        }
        if (strcmp(side, "right") == 0) {
            x = edge + gap;
        } else if (strcmp(side, "left") == 0) {
            x = edge - gap - w;
        } else if (strcmp(side, "bottom") == 0) {
            y = edge + gap;
        } else {
            y = edge - gap - h;
        }
    }
    free(refs);
    pc_status rc = board_rect(cmd, x, y, w, h, &ctx.rect, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* The canvas starts at (0, 0): a board past it moves everything right/down instead. */
    int64_t sx = ctx.rect.x0 < 0 ? -(int64_t)ctx.rect.x0 : 0;
    int64_t sy = ctx.rect.y0 < 0 ? -(int64_t)ctx.rect.y0 : 0;
    if (sx > INT32_MAX || sy > INT32_MAX) {
        return ab_bad(err, cmd, "no room for an artboard %s of \"%.*s\"", side, (int)l->name.len,
                      l->name.ptr);
    }
    ctx.sx = (int32_t)sx;
    ctx.sy = (int32_t)sy;
    rc = pc_engine_session_edit(s, ctx.duplicate ? "Duplicate Artboard" : "New Artboard",
                                add_beside_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer_id ids[1] = {ctx.nid};
    (void)pc_engine_set_selection(s, ids, 1, true, ctx.nid, true, ctx.nid, NULL);
    return artboard_result(out, ctx.nid, ctx.outrect, err);
}

/* ---- File > Export > Artboards ------------------------------------------------------------ */

/* One artboard as its own document (artboard_cmds::artboard_document). *present is false when `id`
 * is not an artboard; an error when the board is too far from the origin to move. */
static pc_status artboard_document(const pc_document *doc, pc_layer_id id, bool *present,
                                   pc_document *out, pc_error *err)
{
    *present = false;
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return PC_OK;
    }
    const pc_artboard *ab = pc_layer_artboard(l);
    if (ab == NULL) {
        return PC_OK;
    }
    pc_rect r = ab->rect;
    if (r.x0 == INT32_MIN || r.y0 == INT32_MIN) {
        return pc_engine_err_other(err, "artboard at (%d, %d) is too far from the origin to export",
                                   r.x0, r.y0);
    }
    int32_t dx = -r.x0, dy = -r.y0;
    pc_layer group;
    if (pc_layer_clone(l, &group, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    group.visible = true;
    pc_engine_translate_layer(doc, &group, dx, dy);
    pc_engine_translate_vectors(doc, &group, (double)dx, (double)dy);
    pc_document one;
    if (pc_document_clone(doc, &one, err) != PC_OK) {
        pc_layer_fini(&group);
        return PC_ERR_NOMEM;
    }
    /* name = the board's name */
    pc_str nm = {0};
    if (pc_str_clone(&l->name, &nm, NULL) == PC_OK) {
        pc_str_fini(&one.name);
        one.name = nm;
    }
    one.size = pc_size_make(pc_rect_width(r), pc_rect_height(r));
    /* layers = [group] */
    for (size_t i = 0; i < one.nlayers; i++) {
        pc_layer_fini(&one.layers[i]);
    }
    free(one.layers);
    one.layers = pc_malloc_array(1, sizeof(pc_layer));
    if (one.layers == NULL) {
        one.nlayers = 0;
        pc_layer_fini(&group);
        pc_document_fini(&one);
        return pc_fail(err, PC_ERR_NOMEM, "artboard export");
    }
    one.layers[0] = group;
    one.nlayers = 1;
    /* clear channels, selection, quick mask, comps */
    for (size_t i = 0; i < one.nchannels; i++) {
        pc_alpha_channel_fini(&one.channels[i]);
    }
    free(one.channels);
    one.channels = NULL;
    one.nchannels = 0;
    if (one.has_selection) {
        pc_surface_fini(&one.selection);
        one.has_selection = false;
    }
    if (one.has_quick_mask) {
        pc_alpha_channel_fini(&one.quick_mask);
        one.has_quick_mask = false;
    }
    for (size_t i = 0; i < one.nlayer_comps; i++) {
        pc_layer_comp_fini(&one.layer_comps[i]);
    }
    free(one.layer_comps);
    one.layer_comps = NULL;
    one.nlayer_comps = 0;
    one.has_last_applied_comp = false;
    if (one.has_last_document_state) {
        pc_layer_comp_fini(&one.last_document_state);
        one.has_last_document_state = false;
    }
    /* guides: shift into the board and drop those outside it */
    double *nv = NULL, *nh = NULL;
    size_t nvn = 0, nhn = 0;
    if (one.guides.nvertical > 0) {
        nv = pc_malloc_array(one.guides.nvertical, sizeof(double));
    }
    if (one.guides.nhorizontal > 0) {
        nh = pc_malloc_array(one.guides.nhorizontal, sizeof(double));
    }
    if ((one.guides.nvertical == 0 || nv != NULL) && (one.guides.nhorizontal == 0 || nh != NULL)) {
        for (size_t i = 0; i < one.guides.nvertical; i++) {
            double g = one.guides.vertical[i] - (double)r.x0;
            if (g > 0.0 && g < (double)pc_rect_width(r)) {
                nv[nvn++] = g;
            }
        }
        for (size_t i = 0; i < one.guides.nhorizontal; i++) {
            double g = one.guides.horizontal[i] - (double)r.y0;
            if (g > 0.0 && g < (double)pc_rect_height(r)) {
                nh[nhn++] = g;
            }
        }
        free(one.guides.vertical);
        free(one.guides.horizontal);
        one.guides.vertical = nv;
        one.guides.nvertical = nvn;
        one.guides.horizontal = nh;
        one.guides.nhorizontal = nhn;
        pc_guides_prune_colors(&one.guides);
    } else {
        free(nv);
        free(nh);
    }
    *out = one;
    *present = true;
    return PC_OK;
}

/* The artboards an export covers, top of the Layers panel first; "artboards":[ids] filters. */
static pc_status chosen_artboards(const pc_document *doc, yyjson_val *p, pc_layer_id **out, size_t *n,
                                  pc_error *err)
{
    *out = NULL;
    *n = 0;
    pc_artboard_ref *refs = NULL;
    size_t nref = 0;
    if (pc_document_artboards(doc, &refs, &nref, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = nref > 0 ? pc_malloc_array(nref, sizeof(pc_layer_id)) : NULL;
    if (nref > 0 && ids == NULL) {
        free(refs);
        return pc_fail(err, PC_ERR_NOMEM, "artboards");
    }
    size_t count = 0;
    yyjson_val *want = pc_json_get(p, "artboards");
    bool filter = want != NULL && yyjson_is_arr(want);
    /* refs are bottom-to-top; emit top first (reverse). */
    for (size_t k = nref; k > 0; k--) {
        pc_layer_id id = refs[k - 1].layer;
        if (filter) {
            bool match = false;
            size_t wn = yyjson_arr_size(want);
            for (size_t i = 0; i < wn; i++) {
                uint64_t wv;
                if (pc_json_as_u64(yyjson_arr_get(want, i), &wv) && (pc_layer_id)wv == id) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                continue;
            }
        }
        ids[count++] = id;
    }
    free(refs);
    *out = ids;
    *n = count;
    return PC_OK;
}

static pc_status run_artboards_to_files(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    const char *cmd = "file.export.artboardsToFiles";
    const char *dir = ab_str(p, "dir");
    if (dir == NULL) {
        dir = ab_str(p, "output");
    }
    if (dir == NULL) {
        return ab_bad(err, cmd, "`dir` (the output folder) is required");
    }
    const char *fmt = ab_str(p, "format");
    if (fmt == NULL) {
        fmt = "png";
    }
    while (*fmt == '.') {
        fmt++;
    }
    char fmtl[32];
    size_t fl = 0;
    for (; fmt[fl] != '\0' && fl < sizeof fmtl - 1; fl++) {
        char ch = fmt[fl];
        fmtl[fl] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
    }
    fmtl[fl] = '\0';
    pc_engine_save_opts save;
    if (pc_engine_save_opts_from_params(s, p, cmd, &save, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    char prefixbuf[512];
    const char *prefix = ab_str(p, "prefix");
    if (prefix == NULL) {
        pc_engine_file_stem(doc->name.ptr != NULL ? doc->name.ptr : "", prefixbuf, sizeof prefixbuf);
        prefix = prefixbuf;
    }
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    pc_status rc = chosen_artboards(doc, p, &ids, &nids, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(ids);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *files = yyjson_mut_arr(rd);
    size_t written = 0;
    for (size_t i = 0; i < nids; i++) {
        bool present = false;
        pc_document one;
        rc = artboard_document(doc, ids[i], &present, &one, err);
        if (rc != PC_OK) {
            break;
        }
        if (!present) {
            continue;
        }
        pc_buf namebuf = PC_BUF_INIT;
        if (prefix[0] != '\0') {
            rc = pc_engine_file_sanitize(&namebuf, prefix, err);
            if (rc == PC_OK) {
                rc = pc_buf_append_byte(&namebuf, (uint8_t)'_', err);
            }
        }
        if (rc == PC_OK) {
            rc = pc_engine_file_sanitize(&namebuf, one.name.ptr != NULL ? one.name.ptr : "", err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append_byte(&namebuf, (uint8_t)'.', err);
        }
        if (rc == PC_OK) {
            rc = pc_buf_append(&namebuf, fmtl, strlen(fmtl), err);
        }
        char *path = NULL;
        if (rc == PC_OK) {
            path = pc_engine_file_join(dir, pc_buf_cstr(&namebuf));
            if (path == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "artboard path");
            }
        }
        pc_buf_fini(&namebuf);
        if (rc == PC_OK) {
            rc = pc_engine_save_doc(&one, path, &save, NULL, err);
        }
        if (rc == PC_OK) {
            yyjson_mut_arr_add_strcpy(rd, files, path);
            written++;
        }
        free(path);
        pc_document_fini(&one);
        if (rc != PC_OK) {
            break;
        }
    }
    free(ids);
    if (rc != PC_OK) {
        pc_json_value_fini(out);
        return rc;
    }
    if (written == 0) {
        pc_json_value_fini(out);
        return ab_bad(err, cmd, "no matching artboards");
    }
    yyjson_mut_obj_add_val(rd, root, "files", files);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

static pc_status run_artboards_to_pdf(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    const char *cmd = "file.export.artboardsToPdf";
    const char *path = ab_str(p, "path");
    if (path == NULL) {
        return ab_bad(err, cmd, "`path` (a .pdf file) is required");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    double quality = 10.0;
    yyjson_val *qv = pc_json_get(p, "quality");
    if (qv != NULL) {
        double q;
        if (pc_json_as_f64(qv, &q) && isfinite(q)) {
            quality = q;
        }
    }
    pc_engine_save_opts save = pc_engine_save_opts_default();
    save.has_quality = true;
    save.quality = quality;
    float dpi = doc->resolution_dpi;
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    pc_status rc = chosen_artboards(doc, p, &ids, &nids, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_pdf_page *pages = nids > 0 ? pc_malloc_array(nids, sizeof(pc_engine_pdf_page)) : NULL;
    pc_buf *jpegs = nids > 0 ? pc_malloc_array(nids, sizeof(pc_buf)) : NULL;
    if (nids > 0 && (pages == NULL || jpegs == NULL)) {
        free(ids);
        free(pages);
        free(jpegs);
        return pc_fail(err, PC_ERR_NOMEM, "artboard pages");
    }
    pc_pixel_format rgb = PC_PIXEL_FORMAT_RGBA8;
    size_t npages = 0;
    for (size_t i = 0; i < nids; i++) {
        bool present = false;
        pc_document one;
        rc = artboard_document(doc, ids[i], &present, &one, err);
        if (rc != PC_OK) {
            break;
        }
        if (!present) {
            continue;
        }
        /* Flatten over white into an opaque RGB document, then JPEG-encode it. */
        pc_surface surf;
        float white[3] = {1.0f, 1.0f, 1.0f};
        rc = pc_compose_flatten_to_surface(&one, rgb, white, &surf, err);
        if (rc != PC_OK) {
            pc_document_fini(&one);
            break;
        }
        pc_document flat;
        if (pc_document_init(&flat, one.name.ptr != NULL ? one.name.ptr : "page", one.size,
                             PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, err) != PC_OK) {
            pc_surface_fini(&surf);
            pc_document_fini(&one);
            rc = PC_ERR_NOMEM;
            break;
        }
        pc_layer bg;
        if (pc_layer_raster(&bg, "Background", rgb, err) != PC_OK) {
            pc_surface_fini(&surf);
            pc_document_fini(&flat);
            pc_document_fini(&one);
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            break;
        }
        pc_surface *bgs = pc_layer_surface_mut(&bg);
        if (bgs != NULL) {
            pc_surface_fini(bgs);
            *bgs = surf; /* move */
        } else {
            pc_surface_fini(&surf);
        }
        pc_layer_id tmpid = 0;
        if (pc_document_insert_above(&flat, NULL, &bg, &tmpid, err) != PC_OK) {
            pc_layer_fini(&bg);
            pc_document_fini(&flat);
            pc_document_fini(&one);
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            break;
        }
        pc_buf jpeg = PC_BUF_INIT;
        rc = pc_engine_encode(&flat, "page.jpg", &save, &jpeg, NULL, err);
        uint32_t ow = one.size.width, oh = one.size.height;
        pc_document_fini(&flat);
        pc_document_fini(&one);
        if (rc != PC_OK) {
            pc_buf_fini(&jpeg);
            break;
        }
        jpegs[npages] = jpeg;
        pages[npages].w = ow;
        pages[npages].h = oh;
        pages[npages].dpi = dpi;
        pages[npages].jpeg = (const uint8_t *)jpeg.data;
        pages[npages].jpeg_len = jpeg.len;
        npages++;
    }
    if (rc == PC_OK && npages == 0) {
        rc = ab_bad(err, cmd, "no matching artboards");
    }
    pc_buf pdf = PC_BUF_INIT;
    if (rc == PC_OK) {
        rc = pc_engine_artboard_raster_pdf(pages, npages, &pdf, err);
    }
    if (rc == PC_OK) {
        rc = pc_engine_write_file(path, pdf.data, pdf.len, err);
    }
    pc_buf_fini(&pdf);
    for (size_t i = 0; i < npages; i++) {
        pc_buf_fini(&jpegs[i]);
    }
    free(jpegs);
    free(pages);
    free(ids);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, root, "path", path);
    yyjson_mut_obj_add_uint(rd, root, "pages", npages);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

const pc_engine_command *pc_engine_artboard_specs(size_t *n)
{
    static const char *const VIEW[] = {"View"};
    static const char *const NEW[] = {"Layer", "New"};
    static const char *const EXPORT[] = {"File", "Export"};
    static const pc_engine_command specs[] = {
        {"layer.new.artboard", "Artboard\xE2\x80\xA6", NEW, 2, NULL,
         "{\"rect\":[x,y,w,h]? | \"x\",\"y\",\"width\",\"height\"? (default: canvas size, right of "
         "the last board),\"preset\":\"iPhone 14|Web 1920|A4|\xE2\x80\xA6\"?,\"name\":str?,"
         "\"background\":\"white|black|transparent|custom\"=\"white\",\"color\":[r,g,b]|\"#rrggbb\"? "
         "(custom)} -> {layer, rect}",
         pc_engine_has_doc, run_new_artboard, true},
        {"layer.new.artboardFromGroup", "Artboard from Group\xE2\x80\xA6", NEW, 2, NULL,
         "{\"layer\":id? (a top-level group; default active),\"background\":\xE2\x80\xA6?} -> "
         "{layer, rect}",
         has_plain_group, run_from_group, true},
        {"layer.new.artboardFromLayers", "Artboard from Layers\xE2\x80\xA6", NEW, 2, NULL,
         "{\"name\":str?,\"background\":\xE2\x80\xA6?} (the selected layers) -> {layer, rect}",
         has_free_layers, run_from_layers, true},
        {"layer.artboard.set", "Edit Artboard", NULL, 0, NULL,
         "{\"layer\":id? (default: the active artboard),\"x\",\"y\",\"width\",\"height\"?|\"rect\":"
         "[x,y,w,h]?,\"preset\":str?,\"background\":\"white|black|transparent|custom\"?,\"color\":"
         "\xE2\x80\xA6?,\"name\":str?,\"moveContents\":bool=true} -> {layer, rect}",
         has_active_artboard, run_set_props, true},
        {"view.clearSelectedArtboardGuides", "Clear Selected Artboard Guides", VIEW, 1, NULL,
         "{} (guides inside the active artboard)", has_active_artboard, run_clear_artboard_guides, true},
        {"layer.artboard.addBeside", "Add Artboard Beside", NULL, 0, NULL,
         "{\"layer\":id? (default: the active artboard),\"side\":\"left|right|top|bottom\"="
         "\"right\",\"duplicate\":bool=false} -> {layer, rect}",
         has_active_artboard, run_add_beside, true},
        {"file.export.artboardsToFiles", "Artboards to Files\xE2\x80\xA6", EXPORT, 2, NULL,
         "{\"dir\":folder,\"format\":\"png|jpg|psd|tiff|\xE2\x80\xA6\"=\"png\",\"prefix\":str="
         "document name (\"\" = none),\"artboards\":[id]? (default all),\"quality\":0..12?} -> "
         "{files}",
         export_artboards_en, run_artboards_to_files, true},
        {"file.export.artboardsToPdf", "Artboards to PDF\xE2\x80\xA6", EXPORT, 2, NULL,
         "{\"path\":str (.pdf),\"artboards\":[id]?,\"quality\":0..12=10} -> {path, pages} (one "
         "raster page per board)",
         export_artboards_en, run_artboards_to_pdf, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
