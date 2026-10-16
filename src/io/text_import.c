/* Type layers read from a file, made ready for the canvas and the panels (port of
 * crates/io/src/text_import.rs). */
#include "io/text_import.h"

#include <stdlib.h>
#include <string.h>

#include "io/core.h"
#include "text/engine.h"
#include "text/warp.h"

/* Renders t from its model unless its ink would exceed PC_IO_TEXT_MAX_RENDER_PIXELS. The model
 * comes from an untrusted file: an absurd type size, scale or box must not make opening it
 * allocate gigabytes (the renderer's own guard is far larger). A layer over it keeps empty
 * pixels, as before, until it is edited. Text the layout refuses (invalid UTF-8, over its size
 * limit) is left without pixels the same way; only out of memory fails. */
static pc_status render_bounded(pc_text_engine *eng, pc_text_layer *t, float dpi, pc_pixel_format format,
                                pc_error *err)
{
    pc_text_layout layout;
    pc_text_deform warp;
    bool has_warp;
    pc_rect ink;
    pc_size sz;
    pc_rendered r;
    pc_status st = pc_text_engine_layout(eng, t, dpi, &layout, err);
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? st : PC_OK;
    }
    has_warp = pc_text_layout_deform(&layout, t, &warp);
    st = pc_text_ink_rect(&layout, &t->transform, has_warp ? &warp : NULL, &ink, err);
    if (st != PC_OK) {
        pc_text_layout_fini(&layout);
        if (has_warp) {
            pc_text_deform_fini(&warp);
        }
        return st;
    }
    sz = pc_rect_size(ink);
    if ((uint64_t)sz.width * (uint64_t)sz.height > PC_IO_TEXT_MAX_RENDER_PIXELS) {
        pc_pixel_format f = format;
        pc_text_layout_fini(&layout);
        if (has_warp) {
            pc_text_deform_fini(&warp);
        }
        f.alpha = true;
        if (t->has_cache) {
            pc_surface_fini(&t->cache);
        }
        pc_surface_init(&t->cache, f);
        t->has_cache = true;
        return PC_OK;
    }
    st = pc_text_rasterize(&layout, &t->transform, format, t->antialias, has_warp ? &warp : NULL, &r, err);
    pc_text_layout_fini(&layout);
    if (has_warp) {
        pc_text_deform_fini(&warp);
    }
    if (st != PC_OK) {
        return st == PC_ERR_NOMEM ? st : PC_OK;
    }
    if (t->has_cache) {
        pc_surface_fini(&t->cache);
    }
    t->cache = r.surface;
    t->has_cache = true;
    return PC_OK;
}

/* A run whose font came as a PostScript name (PSD files) gets the installed family that name
 * resolves to, as the layout does, so Missing Fonts and the Character panel see the real family
 * ("CCWildWords-Regular" is the family "CCWildWords", not the guessed "CC Wild Words"). */
static pc_status resolve_fonts(pc_fontdb *db, pc_text_layer *t, pc_error *err)
{
    bool changed = false;
    for (size_t i = 0; i < t->nruns; i++) {
        pc_char_style *s = &t->runs[i].style;
        pc_resolved_font f;
        pc_status st;
        if (!s->has_postscript_name ||
            pc_fontdb_has_family(db, pc_str_c(&s->font_family), s->font_family.len)) {
            continue;
        }
        st = pc_fontdb_resolve_postscript(db, pc_str_c(&s->postscript_name), s->postscript_name.len, &f, err);
        if (st != PC_OK) {
            if (st == PC_ERR_NOMEM) {
                return st;
            }
            continue;
        }
        if (f.exact || pc_fontdb_has_family(db, f.family, strlen(f.family))) {
            st = pc_str_set_c(&s->font_family, f.family, err);
            if (st != PC_OK) {
                pc_resolved_font_fini(&f);
                return st;
            }
            s->weight = f.weight;
            s->italic = f.italic;
            changed = true;
        }
        pc_resolved_font_fini(&f);
    }
    return changed ? pc_text_layer_sync_summary(t, err) : PC_OK;
}

pc_status pc_io_text_prepare(pc_document *doc, pc_error *err)
{
    pc_walk w;
    pc_text_engine *eng = NULL;
    float dpi = doc->resolution_dpi;
    pc_pixel_format format = pc_document_pixel_format(doc);
    bool any = false;
    pc_status st = pc_document_walk(doc, &w, err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < w.len && !any; i++) {
        any = w.items[i].layer->content.kind == PC_LAYER_TEXT;
    }
    if (!any) {
        pc_walk_fini(&w);
        return PC_OK;
    }
    st = pc_text_engine_shared_lock(&eng, err);
    if (st != PC_OK) {
        pc_walk_fini(&w);
        return st;
    }
    /* Layers are reached by path: preparing a type layer never changes the tree. */
    for (size_t i = 0; i < w.len && st == PC_OK; i++) {
        pc_layer *l;
        pc_text_layer *t;
        if (w.items[i].layer->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        l = pc_document_layer_at_mut(doc, w.paths + w.items[i].path_start, w.items[i].depth + 1);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        t = &l->content.u.text;
        st = resolve_fonts(pc_text_engine_fonts(eng), t, err);
        if (st == PC_OK && !t->has_cache) {
            st = render_bounded(eng, t, dpi, format, err);
        }
    }
    pc_text_engine_shared_unlock();
    pc_walk_fini(&w);
    return st;
}
