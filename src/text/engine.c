/* The text engine (see engine.h). */
#include "text/engine.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/utf8.h"
#include "text/glyphs.h"
#include "text/warp.h"

struct pc_text_engine {
    pc_fontdb *fonts;
    pc_text_layouter *layouter;
};

static pc_status make(bool system, pc_text_engine **out, pc_error *err)
{
    pc_text_engine *e = calloc(1, sizeof *e);
    pc_status st;
    if (e == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "text engine: out of memory");
    }
    st = system ? pc_fontdb_with_system_fonts(&e->fonts, err) : pc_fontdb_new(&e->fonts, err);
    if (st == PC_OK) {
        st = pc_text_layouter_new(&e->layouter, err);
    }
    if (st != PC_OK) {
        pc_text_engine_free(e);
        return st;
    }
    *out = e;
    return PC_OK;
}

pc_status pc_text_engine_new(pc_text_engine **out, pc_error *err)
{
    return make(false, out, err);
}

pc_status pc_text_engine_with_system_fonts(pc_text_engine **out, pc_error *err)
{
    return make(true, out, err);
}

void pc_text_engine_free(pc_text_engine *e)
{
    if (e == NULL) {
        return;
    }
    pc_text_layouter_free(e->layouter);
    pc_fontdb_free(e->fonts);
    free(e);
}

pc_fontdb *pc_text_engine_fonts(pc_text_engine *e)
{
    return e->fonts;
}

pc_status pc_text_engine_layout(pc_text_engine *e, const pc_text_layer *layer, float dpi, pc_text_layout *out,
                                pc_error *err)
{
    if (e == NULL || layer == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text engine: invalid arguments");
    }
    return pc_text_layouter_layout(e->layouter, e->fonts, layer, dpi, out, err);
}

/* The gap between the clusters starting at a and b on one line (b.x - a.x). */
static bool gap(const pc_text_layout *l, size_t a, size_t b, float *out)
{
    const pc_cluster_info *ca = NULL, *cb = NULL;
    size_t i;
    for (i = 0; i < l->nclusters && ca == NULL; i++) {
        if (l->clusters[i].start == a) {
            ca = &l->clusters[i];
        }
    }
    for (i = 0; i < l->nclusters && cb == NULL; i++) {
        if (l->clusters[i].start == b) {
            cb = &l->clusters[i];
        }
    }
    if (ca == NULL || cb == NULL || ca->line != cb->line || ca->rtl) {
        return false;
    }
    *out = cb->x - ca->x;
    return true;
}

pc_status pc_text_engine_pair_kerning(pc_text_engine *e, const pc_text_layer *layer, float dpi, size_t at,
                                      bool *has, float *kerning, pc_error *err)
{
    const char *text;
    size_t len, next, i, start = 0, acc = 0, nruns = 0, nout = 0;
    uint32_t ch, nch;
    pc_text_run *runs = NULL, *out_runs = NULL;
    pc_text_layout l;
    float with, without, em = 0.0f, k;
    bool found = false;
    pc_text_layer plain;
    pc_status st;
    if (e == NULL || layer == NULL || has == NULL || kerning == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text engine: invalid arguments");
    }
    *has = false;
    text = pc_str_c(&layer->text);
    len = layer->text.len;
    /* layer.text.get(at..)?.chars().next()? */
    if (at >= len || ((unsigned char)text[at] & 0xC0) == 0x80) {
        return PC_OK;
    }
    next = at;
    if (!pc_utf8_next((const uint8_t *)text, len, &next, &ch)) {
        return PC_OK;
    }
    if (ch == '\n' || ch == '\r' || next >= len) {
        return PC_OK;
    }
    {
        size_t j = next;
        if (!pc_utf8_next((const uint8_t *)text, len, &j, &nch) || nch == '\n' || nch == '\r') {
            return PC_OK;
        }
    }
    st = pc_text_engine_layout(e, layer, dpi, &l, err);
    if (st != PC_OK) {
        return st;
    }
    found = gap(&l, at, next, &with);
    pc_text_layout_fini(&l);
    if (!found) {
        return PC_OK;
    }
    st = pc_text_layer_char_runs(layer, &runs, &nruns, err);
    if (st != PC_OK) {
        return st;
    }
    found = false;
    for (i = 0; i < nruns; i++) {
        acc += runs[i].len;
        if (at < acc) {
            em = runs[i].style.size_pt * (dpi > 0.0f ? dpi / 72.0f : 1.0f);
            found = true;
            break;
        }
    }
    if (!found || isnan(em) || em <= 0.0f) {
        pc_text_runs_free(runs, nruns);
        return PC_OK;
    }
    /* The same layer with the pair's first character unkerned. */
    out_runs = calloc(3 * nruns + 1, sizeof *out_runs);
    if (out_runs == NULL) {
        pc_text_runs_free(runs, nruns);
        return pc_fail(err, PC_ERR_NOMEM, "text engine: out of memory");
    }
    for (i = 0; i < nruns && st == PC_OK; i++) {
        size_t end = start + runs[i].len, cuts[4], c;
        cuts[0] = start;
        cuts[1] = at < start ? start : at > end ? end : at;
        cuts[2] = next < start ? start : next > end ? end : next;
        cuts[3] = end;
        for (c = 0; c < 3 && st == PC_OK; c++) {
            if (cuts[c + 1] > cuts[c]) {
                pc_text_run *r = &out_runs[nout];
                r->len = cuts[c + 1] - cuts[c];
                st = pc_char_style_clone(&runs[i].style, &r->style, err);
                if (st == PC_OK) {
                    nout++;
                    if (c == 1) {
                        r->style.kerning = PC_KERNING_OFF;
                        r->style.kern = 0.0f;
                    }
                }
            }
        }
        start = end;
    }
    pc_text_runs_free(runs, nruns);
    if (st != PC_OK) {
        pc_text_runs_free(out_runs, nout);
        return st;
    }
    plain = *layer;
    plain.runs = out_runs;
    plain.nruns = nout;
    st = pc_text_engine_layout(e, &plain, dpi, &l, err);
    pc_text_runs_free(out_runs, nout);
    if (st != PC_OK) {
        return st;
    }
    found = gap(&l, at, next, &without);
    pc_text_layout_fini(&l);
    if (!found) {
        return PC_OK;
    }
    k = (with - without) / em * 1000.0f;
    if (isfinite(k)) {
        *has = true;
        *kerning = k;
    }
    return PC_OK;
}

pc_status pc_text_engine_render(pc_text_engine *e, const pc_text_layer *layer, float dpi,
                                pc_pixel_format format, pc_text_layout *layout, pc_rendered *out,
                                pc_error *err)
{
    pc_text_layout l;
    pc_style_warp w;
    bool warped;
    pc_status st;
    if (e == NULL || layer == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text engine: invalid arguments");
    }
    st = pc_text_engine_layout(e, layer, dpi, &l, err);
    if (st != PC_OK) {
        return st;
    }
    warped = layer->has_warp && pc_text_layout_warp(&l, &layer->warp, &w);
    st = pc_text_rasterize(&l, &layer->transform, format, layer->antialias, warped ? &w : NULL, out, err);
    if (st == PC_OK && layout != NULL) {
        *layout = l;
    } else {
        pc_text_layout_fini(&l);
    }
    return st;
}

pc_status pc_text_engine_render_layer(pc_text_engine *e, pc_text_layer *layer, float dpi,
                                      pc_pixel_format format, pc_rect *rect, pc_error *err)
{
    pc_rendered r;
    pc_status st = pc_text_engine_render(e, layer, dpi, format, NULL, &r, err);
    if (st != PC_OK) {
        return st;
    }
    if (layer->has_cache) {
        pc_surface_fini(&layer->cache);
    }
    layer->cache = r.surface;
    layer->has_cache = true;
    if (rect != NULL) {
        *rect = r.rect;
    }
    return PC_OK;
}

pc_status pc_text_glyph_preview(pc_text_engine *e, const pc_char_style *style, uint32_t c, uint32_t px,
                                uint32_t *out_px, uint8_t **alpha, pc_error *err)
{
    pc_text_layer layer;
    pc_text_run run;
    pc_buf text = PC_BUF_INIT;
    pc_text_layout l;
    pc_status st;
    if (e == NULL || style == NULL || out_px == NULL || alpha == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "glyph preview: invalid arguments");
    }
    px = pc_glyph_preview_size(px);
    st = pc_utf8_put(&text, c, err);
    if (st != PC_OK) {
        return st;
    }
    pc_text_layer_default(&layer);
    memset(&run, 0, sizeof run);
    st = pc_char_style_clone(style, &run.style, err);
    if (st != PC_OK) {
        pc_buf_fini(&text);
        return st;
    }
    run.len = text.len;
    run.style.size_pt = (float)px * 0.72f;
    run.style.underline = false;
    run.style.strikethrough = false;
    run.style.tracking = 0.0f;
    run.style.baseline_shift_pt = 0.0f;
    layer.text.ptr = (char *)text.data;
    layer.text.len = text.len;
    layer.runs = &run;
    layer.nruns = 1;
    st = pc_text_engine_layout(e, &layer, 72.0f, &l, err);
    pc_char_style_fini(&run.style);
    pc_buf_fini(&text);
    if (st != PC_OK) {
        return st;
    }
    st = pc_glyph_preview_from_layout(&l, px, out_px, alpha, err);
    pc_text_layout_fini(&l);
    return st;
}

/* ---- the shared engine ---- */

static pthread_once_t shared_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t shared_mu = PTHREAD_MUTEX_INITIALIZER;
static pc_text_engine *shared_engine;

static void shared_init(void)
{
    const char *no = getenv("PHOTOCRAFT_NO_SYSTEM_FONTS");
    pc_text_engine *e = NULL;
    if (make(no == NULL, &e, NULL) == PC_OK) {
        shared_engine = e;
    }
}

pc_status pc_text_engine_shared_lock(pc_text_engine **out, pc_error *err)
{
    if (out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text engine: invalid arguments");
    }
    (void)pthread_once(&shared_once, shared_init);
    if (shared_engine == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "text engine: the shared engine could not be created");
    }
    (void)pthread_mutex_lock(&shared_mu);
    *out = shared_engine;
    return PC_OK;
}

void pc_text_engine_shared_unlock(void)
{
    (void)pthread_mutex_unlock(&shared_mu);
}
