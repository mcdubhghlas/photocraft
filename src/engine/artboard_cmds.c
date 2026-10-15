/* Artboards: portable primitives (port of crates/engine/src/artboard_cmds.rs, E6). The commands
 * are deferred (see artboard_cmds.h); only the size presets and the raster-PDF writer are here. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/buf.h"
#include "doc/comps.h"
#include "doc/doc.h"

#include "artboard_cmds.h"
#include "engine_internal.h"

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

const pc_engine_command *pc_engine_artboard_specs(size_t *n)
{
    static const char *const VIEW[] = {"View"};
    static const pc_engine_command specs[] = {
        {"view.clearSelectedArtboardGuides", "Clear Selected Artboard Guides", VIEW, 1, NULL,
         "{} (guides inside the active artboard)", has_active_artboard, run_clear_artboard_guides, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
