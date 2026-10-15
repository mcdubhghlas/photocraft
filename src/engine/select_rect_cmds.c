/* The Marquee tools' selection primitive (port of the select.rect command in
 * crates/engine/src/commands.rs, E4). Rectangular / Elliptical / Single Row / Single Column, with
 * replace|add|subtract|intersect, anti-aliasing and feathering. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "algo/selection_blur.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "engine_internal.h"
#include "fill_cmds.h"

#define CMD "select.rect"

/* Replace the document's selection with `surf` (moved; the caller must not use it after), or clear
 * it when `set` is false. */
static void set_selection(pc_document *doc, pc_surface *surf, bool set)
{
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (set) {
        doc->selection = *surf;
        doc->has_selection = true;
    } else if (surf != NULL) {
        pc_surface_fini(surf);
    }
}

static float combine_max(void *ctx, float a, float b)
{
    (void)ctx;
    return a > b ? a : b;
}
static float combine_sub(void *ctx, float a, float b)
{
    (void)ctx;
    return a * (1.0f - b);
}
static float combine_min(void *ctx, float a, float b)
{
    (void)ctx;
    return a < b ? a : b;
}

struct rect_ctx {
    pc_rect r;
    bool ellipse;
    bool aa;
    float feather;
    char mode[16];
};

static pc_status rect_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct rect_ctx *c = vctx;
    pc_rect area = pc_document_bounds(doc);
    pc_algo_selection_mode mode = pc_algo_selection_mode_parse(c->mode);

    pc_surface shape;
    if (c->ellipse) {
        pc_status rc = pc_algo_selection_ellipse_surface(c->r, area, c->aa, &shape, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        pc_surface_init(&shape, PC_PIXEL_FORMAT_GRAY8);
        float one = 1.0f;
        pc_status rc = pc_surface_fill_rect(&shape, pc_rect_intersect(c->r, area), &one, 1, err);
        if (rc != PC_OK) {
            pc_surface_fini(&shape);
            return rc;
        }
    }

    if (c->feather > 0.0f) {
        float *m = NULL;
        size_t mlen = 0;
        pc_status rc = pc_algo_selection_mask_from_surface(&shape, area, &m, &mlen, err);
        pc_surface_fini(&shape);
        if (rc != PC_OK) {
            return rc;
        }
        size_t w = pc_rect_width(area), h = pc_rect_height(area);
        float *fe = NULL;
        rc = pc_algo_selection_feather(m, mlen, w, h, c->feather, &fe, err);
        free(m);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface combined;
        bool some = false;
        const pc_surface *old = doc->has_selection ? &doc->selection : NULL;
        rc = pc_algo_selection_combine(old, fe, fe != NULL ? w * h : 0, area, mode, &combined, &some, err);
        free(fe);
        if (rc != PC_OK) {
            return rc;
        }
        /* Subtract/Intersect from nothing (or anything that feathers away to nothing) leaves no
         * selection, exactly as the non-feather branch below does. Honouring `some` here instead
         * of forcing `true` is what crates/engine's empty_selection_combine regression guards. */
        set_selection(doc, &combined, some);
        return PC_OK;
    }

    pc_surface combined;
    bool have_old = doc->has_selection;
    if ((mode == PC_ALGO_SELECTION_ADD || mode == PC_ALGO_SELECTION_SUBTRACT ||
         mode == PC_ALGO_SELECTION_INTERSECT) &&
        have_old) {
        pc_algo_combine_fn f = mode == PC_ALGO_SELECTION_ADD
                                   ? combine_max
                                   : (mode == PC_ALGO_SELECTION_SUBTRACT ? combine_sub : combine_min);
        pc_status rc = pc_algo_selection_combine_surfaces(&doc->selection, &shape, area, f, NULL, &combined, err);
        pc_surface_fini(&shape);
        if (rc != PC_OK) {
            return rc;
        }
    } else if ((mode == PC_ALGO_SELECTION_SUBTRACT || mode == PC_ALGO_SELECTION_INTERSECT) && !have_old) {
        pc_surface_fini(&shape);
        pc_surface_init(&combined, PC_PIXEL_FORMAT_GRAY8);
    } else {
        combined = shape; /* move */
    }
    bool nonempty = !pc_rect_is_empty(pc_surface_content_bounds(&combined));
    set_selection(doc, &combined, nonempty);
    return PC_OK;
}

static pc_status run_select_rect(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    struct rect_ctx ctx = {0};
    int32_t v[4];
    const char *keys[4] = {"x", "y", "width", "height"};
    for (int i = 0; i < 4; i++) {
        bool present = false;
        pc_status rc = pc_engine_param_int_i32(CMD, p, keys[i], &present, &v[i], err);
        if (rc != PC_OK) {
            return rc;
        }
        if (!present) {
            return pc_engine_err_bad_params(err, CMD, "missing `%s`", keys[i]);
        }
    }
    int32_t w = v[2] < 0 ? 0 : v[2];
    int32_t h = v[3] < 0 ? 0 : v[3];
    ctx.r = pc_rect_from_xywh(v[0], v[1], (uint32_t)w, (uint32_t)h);

    yyjson_val *mv = pc_json_get(p, "mode");
    size_t mlen = 0;
    const char *ms = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
    snprintf(ctx.mode, sizeof ctx.mode, "%.*s", (int)(ms != NULL ? mlen : 7), ms != NULL ? ms : "replace");

    yyjson_val *ev = pc_json_get(p, "ellipse");
    ctx.ellipse = ev != NULL && yyjson_is_bool(ev) && yyjson_get_bool(ev);

    const char *step;
    yyjson_val *lv = pc_json_get(p, "line");
    if (lv != NULL && !yyjson_is_null(lv)) {
        size_t llen = 0;
        const char *ls = pc_json_as_str(lv, &llen);
        if (ls != NULL && llen == 3 && memcmp(ls, "row", 3) == 0) {
            step = "Single Row Marquee";
        } else if (ls != NULL && llen == 6 && memcmp(ls, "column", 6) == 0) {
            step = "Single Column Marquee";
        } else {
            return pc_engine_err_bad_params(err, CMD, "unknown `line` (row|column)");
        }
    } else if (ctx.ellipse) {
        step = "Elliptical Marquee";
    } else {
        step = "Rectangular Marquee";
    }

    yyjson_val *av = pc_json_get(p, "antiAlias");
    ctx.aa = av == NULL || !yyjson_is_bool(av) || yyjson_get_bool(av);

    yyjson_val *fv = pc_json_get(p, "feather");
    double fe = 0.0;
    if (fv != NULL && pc_json_as_f64(fv, &fe)) {
        if (fe < 0.0) {
            fe = 0.0;
        } else if (fe > 1000.0) {
            fe = 1000.0;
        }
        ctx.feather = (float)fe;
    }

    pc_status rc = pc_engine_session_edit(s, step, rect_edit, &ctx, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static const pc_engine_command SPECS[] = {
    {"select.rect", "Rectangular Selection", NULL, 0, NULL,
     "{\"x\":i32,\"y\":i32,\"width\":u32,\"height\":u32,\"mode\":\"replace|add|subtract|intersect\"="
     "\"replace\",\"ellipse\":bool=false,\"line\":\"row|column\"?,\"antiAlias\":bool=true,\"feather\":"
     "px=0}",
     pc_engine_has_doc, run_select_rect, true},
};

const pc_engine_command *pc_engine_select_rect_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
