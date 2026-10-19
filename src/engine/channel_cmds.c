/* Channels (port of crates/engine/src/channel_cmds.rs, E7): alpha/spot channels, Save/Load
 * Selection, Quick Mask, channel targeting and visibility, Split/Merge Channels, Merge Spot
 * Channel. See channel_cmds.h for the exported primitives and view accessors.
 *
 * The Channels-panel view state (ChannelView) lives on DocState.channel_view (NULL = default). The
 * layer-mask view (⌥-click) lives on the DocState mask-view fields (E5), so this ChannelView has
 * no layer_mask member -- channels_json merges in mask_view_cmds's layerMaskView through inspect.
 *
 * This module relocates three E7 hooks out of hooks.c: pc_engine_hook_fix_view (keep the view
 * valid after a document change), pc_engine_hook_inspect_channels (the "channels" inspect field)
 * and pc_engine_hook_restrict_to_color (keep the other colour channels after a restricted edit).
 * Apply Image / Calculations and the inject_target / color_restriction routing hooks need the
 * commands.rs paint/blend helpers (E4) and are deferred; see §8. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "color/color.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "algo/selection.h"
#include "vector/vector.h"

#include "brush_cmds.h" /* pc_engine_target_surface (mutable channel/mask/pixel target) */
#include "channel_cmds.h"
#include "channel_display.h"

/* ============================================================================================
 * View state (ChannelView)
 * ========================================================================================== */

enum { CT_COMPOSITE = 0, CT_COLOR = 1, CT_ALPHA = 2, CT_LAYER_MASK = 3, CT_VECTOR_MASK = 4 };

struct pc_engine_channel_view {
    int target_kind;
    size_t target_index; /* Color/Alpha index */
    bool *color_hidden;
    size_t ncolor_hidden;
    bool *alpha_visible;
    size_t nalpha_visible;
    bool quick_mask_hidden;
};

void pc_engine_channel_view_free(struct pc_engine_channel_view *v)
{
    if (v == NULL) {
        return;
    }
    free(v->color_hidden);
    free(v->alpha_visible);
    free(v);
}

pc_status pc_engine_channel_view_clone(const struct pc_engine_channel_view *source,
                                       struct pc_engine_channel_view **out, pc_error *err)
{
    if (!source) {
        *out = NULL;
        return PC_OK;
    }
    struct pc_engine_channel_view *next = calloc(1, sizeof *next);
    if (!next)
        return pc_fail(err, PC_ERR_NOMEM, "preview channels");
    *next = *source;
    next->color_hidden = NULL;
    next->alpha_visible = NULL;
    if (source->ncolor_hidden) {
        next->color_hidden = pc_malloc_array(source->ncolor_hidden, sizeof(bool));
        if (next->color_hidden)
            memcpy(next->color_hidden, source->color_hidden, source->ncolor_hidden * sizeof(bool));
    }
    if (source->nalpha_visible) {
        next->alpha_visible = pc_malloc_array(source->nalpha_visible, sizeof(bool));
        if (next->alpha_visible)
            memcpy(next->alpha_visible, source->alpha_visible, source->nalpha_visible * sizeof(bool));
    }
    if ((source->ncolor_hidden && !next->color_hidden) || (source->nalpha_visible && !next->alpha_visible)) {
        pc_engine_channel_view_free(next);
        return pc_fail(err, PC_ERR_NOMEM, "preview channels");
    }
    *out = next;
    return PC_OK;
}

/* Ensure st->channel_view exists (default on first use). NULL on OOM. */
static struct pc_engine_channel_view *cv_ensure(pc_engine_doc_state *st)
{
    if (st->channel_view == NULL) {
        st->channel_view = calloc(1, sizeof *st->channel_view);
    }
    return st->channel_view;
}

/* Resize a bool array to newn, zero-filling new entries. false on OOM (array unchanged). */
static bool bool_resize(bool **arr, size_t *n, size_t newn)
{
    if (newn == *n) {
        return true;
    }
    if (newn == 0) {
        free(*arr);
        *arr = NULL;
        *n = 0;
        return true;
    }
    bool *r = realloc(*arr, newn * sizeof(bool));
    if (r == NULL) {
        return false;
    }
    for (size_t i = *n; i < newn; i++) {
        r[i] = false;
    }
    *arr = r;
    *n = newn;
    return true;
}

static bool cv_color_visible(const struct pc_engine_channel_view *v, size_t k)
{
    if (v == NULL || k >= v->ncolor_hidden) {
        return true;
    }
    return !v->color_hidden[k];
}

static bool cv_alpha_shown(const struct pc_engine_channel_view *v, size_t i)
{
    if (v == NULL || i >= v->nalpha_visible) {
        return false;
    }
    return v->alpha_visible[i];
}

static size_t cv_visible_colors(const struct pc_engine_channel_view *v, size_t colors)
{
    size_t c = 0;
    for (size_t k = 0; k < colors; k++) {
        if (cv_color_visible(v, k)) {
            c++;
        }
    }
    return c;
}

static bool cv_set_color_visible(struct pc_engine_channel_view *v, size_t k, bool on)
{
    if (k >= v->ncolor_hidden && !bool_resize(&v->color_hidden, &v->ncolor_hidden, k + 1)) {
        return false;
    }
    v->color_hidden[k] = !on;
    return true;
}

static bool cv_set_alpha_visible(struct pc_engine_channel_view *v, size_t i, bool on)
{
    if (i >= v->nalpha_visible && !bool_resize(&v->alpha_visible, &v->nalpha_visible, i + 1)) {
        return false;
    }
    v->alpha_visible[i] = on;
    return true;
}

static void cv_clear_colors(struct pc_engine_channel_view *v)
{
    free(v->color_hidden);
    v->color_hidden = NULL;
    v->ncolor_hidden = 0;
}

static void cv_clear_alpha(struct pc_engine_channel_view *v)
{
    free(v->alpha_visible);
    v->alpha_visible = NULL;
    v->nalpha_visible = 0;
}

/* ============================================================================================
 * Channel names and formats
 * ========================================================================================== */

static pc_color_mode edit_mode(const pc_document *doc)
{
    return pc_document_pixel_format(doc).mode;
}

size_t pc_engine_color_count(const pc_document *doc)
{
    return pc_color_mode_channels(edit_mode(doc));
}

static const char *const RGB_NAMES[3] = {"Red", "Green", "Blue"};
static const char *const CMYK_NAMES[4] = {"Cyan", "Magenta", "Yellow", "Black"};
static const char *const LAB_NAMES[3] = {"Lightness", "a", "b"};
static const char *const GRAY_NAMES[1] = {"Gray"};

static const char *const *color_names(pc_color_mode mode, size_t *n)
{
    switch (mode) {
    case PC_COLOR_MODE_CMYK:
        *n = 4;
        return CMYK_NAMES;
    case PC_COLOR_MODE_LAB:
        *n = 3;
        return LAB_NAMES;
    case PC_COLOR_MODE_GRAYSCALE:
        *n = 1;
        return GRAY_NAMES;
    default:
        *n = 3;
        return RGB_NAMES;
    }
}

static const char *composite_name(pc_color_mode mode)
{
    switch (mode) {
    case PC_COLOR_MODE_CMYK:
        return "CMYK";
    case PC_COLOR_MODE_LAB:
        return "Lab";
    case PC_COLOR_MODE_GRAYSCALE:
        return "Gray";
    default:
        return "RGB";
    }
}

static pc_pixel_format channel_format(const pc_document *doc)
{
    pc_pixel_format f = {PC_COLOR_MODE_GRAYSCALE, doc->depth, false};
    return f;
}

/* Next free "`base` N" name into out[cap]. */
static void next_alpha_name(const pc_document *doc, const char *base, char *out, size_t cap)
{
    for (size_t nn = 1; nn <= doc->nchannels + 1; nn++) {
        char cand[128];
        (void)snprintf(cand, sizeof cand, "%s %zu", base, nn);
        bool taken = false;
        for (size_t i = 0; i < doc->nchannels; i++) {
            if (pc_str_eq_c(&doc->channels[i].name, cand)) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            (void)snprintf(out, cap, "%s", cand);
            return;
        }
    }
    (void)snprintf(out, cap, "%s", base);
}

/* ============================================================================================
 * Planes: one grayscale value per canvas pixel
 * ========================================================================================== */

/* Channel 0 of surface `s` over r as w*h floats (free()). *out NULL for empty. */
static pc_status read_plane(const pc_surface *s, pc_rect r, float **out, size_t *len, pc_error *err)
{
    *out = NULL;
    *len = 0;
    uint32_t w = pc_rect_width(r), h = pc_rect_height(r);
    size_t n = (size_t)w * (size_t)h;
    if (n == 0) {
        return PC_OK;
    }
    float *raw = NULL;
    size_t rawlen = 0;
    pc_status rc = pc_surface_read_region(s, r, &raw, &rawlen, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t ch = pc_surface_channels(s);
    float *v = malloc(n * sizeof(float));
    if (v == NULL) {
        free(raw);
        return pc_fail(err, PC_ERR_NOMEM, "plane");
    }
    if (ch > 1) {
        for (size_t i = 0; i < n; i++) {
            v[i] = raw[i * ch];
        }
    } else {
        memcpy(v, raw, n * sizeof(float));
    }
    free(raw);
    *out = v;
    *len = n;
    return PC_OK;
}

/* A GRAY surface in fmt from clamped plane values over r. out initialised on success. */
static pc_status plane_surface(const float *vals, size_t n, pc_rect r, pc_pixel_format fmt, pc_surface *out,
                               pc_error *err)
{
    pc_surface s;
    pc_surface_init(&s, fmt);
    float *clamped = malloc(n ? n * sizeof(float) : 1);
    if (clamped == NULL) {
        pc_surface_fini(&s);
        return pc_fail(err, PC_ERR_NOMEM, "plane surface");
    }
    for (size_t i = 0; i < n; i++) {
        float x = vals[i];
        clamped[i] = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    }
    pc_status rc = pc_surface_write_region(&s, r, clamped, n, err);
    free(clamped);
    if (rc != PC_OK) {
        pc_surface_fini(&s);
        return rc;
    }
    pc_surface_prune(&s);
    *out = s;
    return PC_OK;
}

static float luminosity(const float rgba[4])
{
    return 0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2];
}

/* ============================================================================================
 * Channel references (ChanRef)
 * ========================================================================================== */

enum {
    REF_COMPOSITE,
    REF_COLOR,
    REF_ALPHA,
    REF_QUICK_MASK,
    REF_SELECTION,
    REF_TRANSPARENCY,
    REF_LAYER_MASK,
    REF_VECTOR_MASK
};

typedef struct {
    int kind;
    size_t index;
} chan_ref;

static bool ieq(const char *a, size_t alen, const char *b)
{
    size_t bl = strlen(b);
    if (alen != bl) {
        return false;
    }
    for (size_t i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb + 32);
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/* Parse a channel reference from a JSON value. *out set on success. */
static bool parse_ref(yyjson_val *v, const pc_document *doc, chan_ref *out)
{
    if (v == NULL) {
        return false;
    }
    pc_color_mode mode = edit_mode(doc);
    if (yyjson_is_num(v)) {
        uint64_t i;
        if (pc_json_as_u64(v, &i)) {
            out->kind = REF_ALPHA;
            out->index = (size_t)i;
            return true;
        }
        return false;
    }
    if (yyjson_is_obj(v)) {
        uint64_t k;
        yyjson_val *cv = pc_json_get(v, "color");
        if (cv != NULL && pc_json_as_u64(cv, &k)) {
            out->kind = REF_COLOR;
            out->index = (size_t)k;
            return true;
        }
        yyjson_val *av = pc_json_get(v, "alpha");
        if (av != NULL && pc_json_as_u64(av, &k)) {
            out->kind = REF_ALPHA;
            out->index = (size_t)k;
            return true;
        }
        return false;
    }
    size_t slen = 0;
    const char *s = pc_json_as_str(v, &slen);
    if (s == NULL) {
        return false;
    }
    if (ieq(s, slen, "composite") || ieq(s, slen, "merged")) {
        out->kind = REF_COMPOSITE;
        return true;
    }
    if (ieq(s, slen, "quickmask") || ieq(s, slen, "quick mask")) {
        out->kind = REF_QUICK_MASK;
        return true;
    }
    if (ieq(s, slen, "selection")) {
        out->kind = REF_SELECTION;
        return true;
    }
    if (ieq(s, slen, "transparency")) {
        out->kind = REF_TRANSPARENCY;
        return true;
    }
    if (ieq(s, slen, "mask") || ieq(s, slen, "layermask")) {
        out->kind = REF_LAYER_MASK;
        return true;
    }
    if (ieq(s, slen, "vectormask") || ieq(s, slen, "vector mask")) {
        out->kind = REF_VECTOR_MASK;
        return true;
    }
    if (ieq(s, slen, composite_name(mode))) {
        out->kind = REF_COMPOSITE;
        return true;
    }
    size_t ncolors = 0;
    const char *const *names = color_names(mode, &ncolors);
    for (size_t k = 0; k < ncolors; k++) {
        if (ieq(s, slen, names[k])) {
            out->kind = (ncolors == 1) ? REF_COMPOSITE : REF_COLOR;
            out->index = k;
            return true;
        }
    }
    for (size_t i = 0; i < doc->nchannels; i++) {
        if (pc_str_eq_mem(&doc->channels[i].name, s, slen)) {
            out->kind = REF_ALPHA;
            out->index = i;
            return true;
        }
    }
    return false;
}

/* ============================================================================================
 * JSON param helpers
 * ========================================================================================== */

static const char *get_str(yyjson_val *p, const char *key, size_t *len)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return NULL;
    }
    return pc_json_as_str(v, len);
}

static bool get_bool(yyjson_val *p, const char *key, bool def)
{
    bool b;
    yyjson_val *v = pc_json_get(p, key);
    if (v != NULL && pc_json_as_bool(v, &b)) {
        return b;
    }
    return def;
}

static bool str_is(const char *s, size_t len, const char *lit)
{
    return s != NULL && strlen(lit) == len && memcmp(s, lit, len) == 0;
}

/* ============================================================================================
 * Sources (Load Selection, Save Selection scope)
 * ========================================================================================== */

/* Native pixel values of a layer (or merged) over the canvas. *out owned (free()), *n channels. */
static pc_status native_pixels(const pc_document *doc, bool has_layer, pc_layer_id layer, float **out,
                               size_t *out_npx, size_t *out_n, pc_error *err)
{
    pc_rect r = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t n = pc_pixel_format_channels(fmt);
    size_t npx = (size_t)pc_rect_width(r) * (size_t)pc_rect_height(r);
    *out = NULL;
    *out_npx = npx;
    *out_n = n;
    if (has_layer) {
        const pc_layer *l = pc_document_layer(doc, layer);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, layer);
        }
        const pc_surface *s = pc_layer_surface(l);
        if (s == NULL) {
            return pc_engine_err_other(err, "layer \"%s\" has no pixels", pc_str_c(&l->name));
        }
        pc_pixel_format sf = pc_surface_format(s);
        float *raw = NULL;
        size_t rawlen = 0;
        pc_status rc = pc_surface_read_region(s, r, &raw, &rawlen, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_pixel_format_eq(sf, fmt)) {
            *out = raw;
            return PC_OK;
        }
        size_t sc = pc_surface_channels(s);
        float *conv = calloc(npx ? npx * n : 1, sizeof(float));
        if (conv == NULL) {
            free(raw);
            return pc_fail(err, PC_ERR_NOMEM, "native pixels");
        }
        for (size_t i = 0; i < npx; i++) {
            float rgba[4];
            pc_to_rgba(sf, &raw[i * sc], sc, rgba);
            (void)pc_from_rgba(fmt, rgba, &conv[i * n]);
        }
        free(raw);
        *out = conv;
        return PC_OK;
    }
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render(doc, r, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    float *conv = calloc(npx ? npx * n : 1, sizeof(float));
    if (conv == NULL) {
        pc_compose_buffer_fini(&buf);
        return pc_fail(err, PC_ERR_NOMEM, "native pixels");
    }
    for (size_t i = 0; i < npx && i < buf.len; i++) {
        (void)pc_from_rgba(fmt, buf.px[i], &conv[i * n]);
    }
    pc_compose_buffer_fini(&buf);
    *out = conv;
    return PC_OK;
}

/* The "layer" param of a source: id, topmost name, or merged (has=false). */
static pc_status source_layer(const pc_document *doc, yyjson_val *p, const char *cmd, bool *has,
                              pc_layer_id *out, pc_error *err)
{
    *has = false;
    yyjson_val *v = pc_json_get(p, "layer");
    if (v == NULL) {
        return PC_OK;
    }
    uint64_t id;
    if (pc_json_as_u64(v, &id)) {
        *has = true;
        *out = id;
        return PC_OK;
    }
    size_t len = 0;
    const char *name = pc_json_as_str(v, &len);
    if (name == NULL || len == 0 || str_is(name, len, "merged")) {
        return PC_OK;
    }
    pc_walk w;
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    bool found = false;
    for (size_t i = w.len; i-- > 0;) {
        if (pc_str_eq_mem(&w.items[i].layer->name, name, len)) {
            *has = true;
            *out = w.items[i].layer->id;
            found = true;
            break;
        }
    }
    pc_walk_fini(&w);
    if (!found) {
        return pc_engine_err_bad_params(err, cmd, "no source layer named \"%.*s\"", (int)len, name);
    }
    return PC_OK;
}

/* Owned list of planes (each w*h floats). */
typedef struct {
    float **planes;
    size_t count;
    size_t n; /* values per plane */
} plane_list;

static void plane_list_fini(plane_list *pl)
{
    if (pl->planes == NULL) {
        return;
    }
    for (size_t i = 0; i < pl->count; i++) {
        free(pl->planes[i]);
    }
    free(pl->planes);
    pl->planes = NULL;
    pl->count = 0;
}

/* Planes of channel ref r (layer=has_layer/layer; active for transparency/mask defaults). */
static pc_status ref_planes(const pc_document *doc, bool has_layer, pc_layer_id layer, bool has_active,
                            pc_layer_id active, chan_ref r, const char *cmd, plane_list *out, pc_error *err)
{
    pc_rect area = pc_document_bounds(doc);
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    size_t colors = pc_engine_color_count(doc);
    out->planes = NULL;
    out->count = 0;
    out->n = npx;

    if (r.kind == REF_COMPOSITE || r.kind == REF_COLOR) {
        float *px = NULL;
        size_t pxn = 0, nch = 0;
        pc_status rc = native_pixels(doc, has_layer, layer, &px, &pxn, &nch, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (px == NULL) { /* empty canvas: nothing to read */
            npx = 0;
            nch = nch ? nch : 1;
        }
        if (r.kind == REF_COLOR) {
            if (r.index >= colors) {
                free(px);
                return pc_engine_err_other(err, "no colour channel %zu", r.index);
            }
            float *pl = calloc(npx ? npx : 1, sizeof(float));
            if (pl == NULL) {
                free(px);
                return pc_fail(err, PC_ERR_NOMEM, "plane");
            }
            for (size_t i = 0; i < npx; i++) {
                pl[i] = px[i * nch + r.index];
            }
            free(px);
            out->planes = malloc(sizeof(float *));
            if (out->planes == NULL) {
                free(pl);
                return pc_fail(err, PC_ERR_NOMEM, "planes");
            }
            out->planes[0] = pl;
            out->count = 1;
            return PC_OK;
        }
        out->planes = calloc(colors ? colors : 1, sizeof(float *));
        if (out->planes == NULL) {
            free(px);
            return pc_fail(err, PC_ERR_NOMEM, "planes");
        }
        for (size_t c = 0; c < colors; c++) {
            out->planes[c] = calloc(npx ? npx : 1, sizeof(float));
            if (out->planes[c] == NULL) {
                free(px);
                plane_list_fini(out);
                return pc_fail(err, PC_ERR_NOMEM, "plane");
            }
            out->count = c + 1;
            for (size_t i = 0; i < npx; i++) {
                out->planes[c][i] = px[i * nch + c];
            }
        }
        free(px);
        return PC_OK;
    }

    /* Single-plane references. */
    float *pl = NULL;
    size_t pln = 0;
    pc_status rc = PC_OK;
    switch (r.kind) {
    case REF_ALPHA:
        if (r.index >= doc->nchannels) {
            return pc_engine_err_other(err, "no alpha channel %zu", r.index);
        }
        rc = read_plane(&doc->channels[r.index].surface, area, &pl, &pln, err);
        break;
    case REF_QUICK_MASK:
        if (!doc->has_quick_mask) {
            return pc_engine_err_other(err, "not in Quick Mask mode");
        }
        rc = read_plane(&doc->quick_mask.surface, area, &pl, &pln, err);
        break;
    case REF_SELECTION:
        rc = pc_algo_selection_mask_from_surface(doc->has_selection ? &doc->selection : NULL, area, &pl, &pln,
                                                 err);
        break;
    case REF_TRANSPARENCY: {
        bool hl = has_layer ? true : has_active;
        pc_layer_id id = has_layer ? layer : active;
        if (!hl) {
            return pc_engine_err_other(err, "transparency needs a layer");
        }
        const pc_layer *l = pc_document_layer(doc, id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, id);
        }
        const pc_surface *s = pc_layer_surface(l);
        if (s != NULL) {
            float (*rgba)[4] = malloc(npx ? npx * sizeof(*rgba) : sizeof(*rgba));
            if (rgba == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "transparency");
            }
            rc = pc_surface_read_rgba_into(s, area, rgba, npx, err);
            if (rc == PC_OK) {
                pl = malloc(npx ? npx * sizeof(float) : 1);
                if (pl == NULL) {
                    free(rgba);
                    return pc_fail(err, PC_ERR_NOMEM, "transparency");
                }
                for (size_t i = 0; i < npx; i++) {
                    pl[i] = rgba[i][3];
                }
                pln = npx;
            }
            free(rgba);
        } else if (l->content.kind == PC_LAYER_SHAPE) {
            pc_vec_compiled_shape cs;
            rc = pc_vec_compiled_shape_new(&l->content.u.shape, PC_VEC_DEFAULT_TOLERANCE, area, &cs, err);
            if (rc == PC_OK) {
                float (*rgba)[4] = NULL;
                size_t rn = 0;
                rc = pc_vec_compiled_shape_render_rgba(&cs, area, &rgba, &rn, err);
                if (rc == PC_OK) {
                    pl = malloc(npx ? npx * sizeof(float) : 1);
                    if (pl == NULL) {
                        free(rgba);
                        pc_vec_compiled_shape_fini(&cs);
                        return pc_fail(err, PC_ERR_NOMEM, "transparency");
                    }
                    for (size_t i = 0; i < npx; i++) {
                        pl[i] = (i < rn) ? rgba[i][3] : 0.0f;
                    }
                    pln = npx;
                    free(rgba);
                }
                pc_vec_compiled_shape_fini(&cs);
            }
        } else {
            pl = malloc(npx ? npx * sizeof(float) : 1);
            if (pl == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "transparency");
            }
            for (size_t i = 0; i < npx; i++) {
                pl[i] = 1.0f;
            }
            pln = npx;
        }
        break;
    }
    case REF_LAYER_MASK: {
        bool hl = has_layer ? true : has_active;
        pc_layer_id id = has_layer ? layer : active;
        if (!hl) {
            return pc_engine_err_other(err, "layer mask needs a layer");
        }
        const pc_layer *l = pc_document_layer(doc, id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, id);
        }
        if (!l->has_mask) {
            return pc_engine_err_other(err, "layer \"%s\" has no mask", pc_str_c(&l->name));
        }
        rc = read_plane(&l->mask.surface, area, &pl, &pln, err);
        break;
    }
    case REF_VECTOR_MASK: {
        bool hl = has_layer ? true : has_active;
        pc_layer_id id = has_layer ? layer : active;
        if (!hl) {
            return pc_engine_err_other(err, "vector mask needs a layer");
        }
        const pc_layer *l = pc_document_layer(doc, id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, id);
        }
        if (!l->has_vector_mask) {
            return pc_engine_err_other(err, "layer \"%s\" has no vector mask", pc_str_c(&l->name));
        }
        /* The path's shape at full density, whatever the mask's enabled flag. */
        pc_vector_mask vm = l->vector_mask;
        vm.enabled = true;
        vm.density = 1.0f;
        vm.feather = 0.0f;
        pc_vec_compiled_mask cm;
        rc = pc_vec_compiled_mask_new(&vm, &cm, err);
        if (rc == PC_OK) {
            float *vals = NULL;
            size_t vn = 0;
            rc = pc_vec_compiled_mask_render(&cm, area, &vals, &vn, err);
            pc_vec_compiled_mask_fini(&cm);
            if (rc == PC_OK) {
                pl = vals;
                pln = vn;
            }
        }
        break;
    }
    default:
        return pc_engine_err_bad_params(err, cmd, "unknown `channel`");
    }
    if (rc != PC_OK) {
        free(pl);
        return rc;
    }
    if (pl == NULL) { /* empty area: a zero plane the size of the canvas (out->n) */
        pl = calloc(out->n ? out->n : 1, sizeof(float));
        if (pl == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "plane");
        }
        pln = out->n;
    }
    (void)pln;
    out->planes = malloc(sizeof(float *));
    if (out->planes == NULL) {
        free(pl);
        return pc_fail(err, PC_ERR_NOMEM, "planes");
    }
    out->planes[0] = pl;
    out->count = 1;
    return PC_OK;
}

/* Reduce to one grayscale plane (composite -> luminosity). *out owned (free()). */
static pc_status gray_plane(const pc_document *doc, bool has_layer, pc_layer_id layer, bool has_active,
                            pc_layer_id active, chan_ref r, const char *cmd, float **out, size_t *n,
                            pc_error *err)
{
    plane_list pl = {0};
    pc_status rc = ref_planes(doc, has_layer, layer, has_active, active, r, cmd, &pl, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pl.count == 1) {
        *out = pl.planes[0];
        *n = pl.n;
        free(pl.planes);
        return PC_OK;
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t nch = pc_pixel_format_channels(fmt);
    float *g = calloc(pl.n ? pl.n : 1, sizeof(float));
    if (g == NULL) {
        plane_list_fini(&pl);
        return pc_fail(err, PC_ERR_NOMEM, "gray plane");
    }
    for (size_t i = 0; i < pl.n; i++) {
        float px[PC_RASTER_MAX_CHANNELS] = {0};
        for (size_t c = 0; c < pl.count && c < nch; c++) {
            px[c] = pl.planes[c][i];
        }
        if (fmt.alpha && pl.count < nch) {
            px[pl.count] = 1.0f;
        }
        float rgba[4];
        pc_to_rgba(fmt, px, nch, rgba);
        g[i] = luminosity(rgba);
    }
    plane_list_fini(&pl);
    *out = g;
    *n = pl.n;
    return PC_OK;
}

/* A source spec `{document?, layer?, channel, invert?}` over the active canvas. */
static pc_status source_planes(pc_engine_session *s, yyjson_val *p, const char *cmd, bool gray,
                               plane_list *out, pc_error *err)
{
    out->planes = NULL;
    out->count = 0;
    /* document index */
    size_t n = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &n);
    size_t di;
    int64_t dv;
    if (pc_engine_param_int(p, "document", &dv)) {
        if (dv < 0 || (uint64_t)dv >= n) {
            return pc_engine_err_other(err, "no open document %lld", (long long)dv);
        }
        di = (size_t)dv;
    } else {
        size_t a;
        if (!pc_engine_session_active_index(s, &a)) {
            return pc_engine_err_no_document(err);
        }
        di = a;
    }
    const pc_engine_doc_state *target = pc_engine_session_active(s);
    if (target == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *src = pc_doc_arc_get(docs[di].doc);
    const pc_document *tgt = pc_doc_arc_get(target->doc);
    if (src->size.width != tgt->size.width || src->size.height != tgt->size.height) {
        return pc_engine_err_bad_params(err, cmd,
                                        "source document \"%s\" is %ux%u; it must match the target size",
                                        pc_str_c(&src->name), src->size.width, src->size.height);
    }
    chan_ref r = {REF_COMPOSITE, 0};
    yyjson_val *cv = pc_json_get(p, "channel");
    if (cv != NULL && !parse_ref(cv, src, &r)) {
        return pc_engine_err_bad_params(err, cmd, "unknown `channel`");
    }
    bool has_layer = false;
    pc_layer_id layer = 0;
    pc_status rc = source_layer(src, p, cmd, &has_layer, &layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool src_active_has = docs[di].has_active_layer;
    pc_layer_id src_active = docs[di].active_layer;
    if (gray) {
        float *g = NULL;
        size_t gn = 0;
        rc = gray_plane(src, has_layer, layer, src_active_has, src_active, r, cmd, &g, &gn, err);
        if (rc != PC_OK) {
            return rc;
        }
        out->planes = malloc(sizeof(float *));
        if (out->planes == NULL) {
            free(g);
            return pc_fail(err, PC_ERR_NOMEM, "source");
        }
        out->planes[0] = g;
        out->count = 1;
        out->n = gn;
    } else {
        rc = ref_planes(src, has_layer, layer, src_active_has, src_active, r, cmd, out, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    if (out->count == 0) {
        plane_list_fini(out);
        return pc_engine_err_bad_params(err, cmd, "the source has no channels");
    }
    if (get_bool(p, "invert", false)) {
        for (size_t c = 0; c < out->count; c++) {
            for (size_t i = 0; i < out->n; i++) {
                out->planes[c][i] = 1.0f - out->planes[c][i];
            }
        }
    }
    return PC_OK;
}

/* ============================================================================================
 * Channel-target primitives (exported, pixel commands)
 * ========================================================================================== */

pc_engine_pixel_target pc_engine_target_of(yyjson_val *p, size_t *alpha_index)
{
    if (alpha_index != NULL) {
        *alpha_index = 0;
    }
    yyjson_val *t = pc_json_get(p, "target");
    if (t == NULL) {
        return PC_ENGINE_PIXTGT_PIXELS;
    }
    size_t len = 0;
    const char *str = pc_json_as_str(t, &len);
    if (str != NULL) {
        if (str_is(str, len, "mask")) {
            return PC_ENGINE_PIXTGT_MASK;
        }
        if (str_is(str, len, "quickMask")) {
            return PC_ENGINE_PIXTGT_QUICK_MASK;
        }
        return PC_ENGINE_PIXTGT_PIXELS;
    }
    if (yyjson_is_obj(t)) {
        uint64_t i;
        yyjson_val *cv = pc_json_get(t, "channel");
        if (cv != NULL && pc_json_as_u64(cv, &i)) {
            if (alpha_index != NULL) {
                *alpha_index = (size_t)i;
            }
            return PC_ENGINE_PIXTGT_ALPHA;
        }
    }
    return PC_ENGINE_PIXTGT_PIXELS;
}

bool pc_engine_is_channel_target(yyjson_val *p)
{
    pc_engine_pixel_target t = pc_engine_target_of(p, NULL);
    return t == PC_ENGINE_PIXTGT_ALPHA || t == PC_ENGINE_PIXTGT_QUICK_MASK;
}

pc_status pc_engine_read_target_surface(const pc_document *doc, bool has_layer, pc_layer_id layer,
                                        yyjson_val *p, const pc_surface **out, pc_error *err)
{
    size_t ai = 0;
    pc_engine_pixel_target t = pc_engine_target_of(p, &ai);
    switch (t) {
    case PC_ENGINE_PIXTGT_ALPHA:
        if (ai >= doc->nchannels) {
            return pc_engine_err_other(err, "no alpha channel %zu (document has %zu)", ai, doc->nchannels);
        }
        *out = &doc->channels[ai].surface;
        return PC_OK;
    case PC_ENGINE_PIXTGT_QUICK_MASK:
        if (!doc->has_quick_mask) {
            return pc_engine_err_other(err, "not in Quick Mask mode");
        }
        *out = &doc->quick_mask.surface;
        return PC_OK;
    case PC_ENGINE_PIXTGT_PIXELS: {
        if (!has_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, layer);
        const pc_surface *s = l != NULL ? pc_layer_surface(l) : NULL;
        if (s == NULL) {
            return pc_engine_err_no_layer(err, layer);
        }
        *out = s;
        return PC_OK;
    }
    case PC_ENGINE_PIXTGT_MASK:
    default: {
        if (!has_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, layer);
        if (l == NULL || !l->has_mask) {
            return pc_engine_err_no_layer(err, layer);
        }
        *out = &l->mask.surface;
        return PC_OK;
    }
    }
}

/* channel_cmds::materialize: make a surface explicit over `r` (read then write it back), so a
 * filter/plug-in that works a whole region sees every pixel (the default tiles become real). */
static pc_status materialize_region(pc_surface *s, pc_rect r, pc_error *err)
{
    float *v = NULL;
    size_t len = 0;
    pc_status rc = pc_surface_read_region(s, r, &v, &len, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_surface_write_region(s, r, v, len, err);
    free(v);
    return rc;
}

pc_status pc_engine_channel_surface_for_filter(pc_document *doc, bool has_layer, pc_layer_id layer,
                                               yyjson_val *p, pc_surface **out, pc_error *err)
{
    *out = NULL;
    if (pc_engine_target_of(p, NULL) == PC_ENGINE_PIXTGT_PIXELS) {
        return PC_OK; /* the layer's pixels: the caller handles it (Rust None) */
    }
    pc_surface *surf = NULL;
    bool lock = false; /* channel/mask targets are never transparency-locked */
    pc_status rc = pc_engine_target_surface(doc, has_layer, layer, p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = materialize_region(surf, pc_document_bounds(doc), err);
    if (rc != PC_OK) {
        return rc;
    }
    *out = surf;
    return PC_OK;
}

size_t pc_engine_targeted_color(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || d->channel_view == NULL) {
        return SIZE_MAX;
    }
    if (d->channel_view->target_kind == CT_COLOR) {
        return d->channel_view->target_index;
    }
    return SIZE_MAX;
}

bool pc_engine_targeted_channel(const pc_engine_session *s, bool *is_quick, size_t *alpha_index)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return false;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const struct pc_engine_channel_view *v = d->channel_view;
    int kind = v != NULL ? v->target_kind : CT_COMPOSITE;
    if (kind == CT_ALPHA && v->target_index < doc->nchannels) {
        if (is_quick != NULL) {
            *is_quick = false;
        }
        if (alpha_index != NULL) {
            *alpha_index = v->target_index;
        }
        return true;
    }
    if (kind == CT_COMPOSITE && doc->has_quick_mask) {
        if (is_quick != NULL) {
            *is_quick = true;
        }
        return true;
    }
    return false;
}

bool pc_engine_edits_channel(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return false;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const struct pc_engine_channel_view *v = d->channel_view;
    int kind = v != NULL ? v->target_kind : CT_COMPOSITE;
    if (doc->has_quick_mask && kind == CT_COMPOSITE) {
        return true;
    }
    return kind == CT_ALPHA && v->target_index < doc->nchannels;
}

size_t pc_engine_channel_visible_colors(const pc_engine_doc_state *d, size_t colors)
{
    return cv_visible_colors(d != NULL ? d->channel_view : NULL, colors);
}

bool pc_engine_channel_alpha_shown(const pc_engine_doc_state *d, size_t i)
{
    return cv_alpha_shown(d != NULL ? d->channel_view : NULL, i);
}

bool pc_engine_channel_quick_mask_hidden(const pc_engine_doc_state *d)
{
    return d != NULL && d->channel_view != NULL && d->channel_view->quick_mask_hidden;
}

/* ============================================================================================
 * Hooks relocated from hooks.c (E7)
 * ========================================================================================== */

static bool channel_routed(const char *id)
{
    if (!id) {
        return false;
    }
    if (!strncmp(id, "filter.", 7) || !strncmp(id, "image.adjustments.", 18)) {
        return true;
    }
    static const char *const ids[] = {"plugin.run",
                                      "paint.stroke",
                                      "paint.pencil",
                                      "paint.bucket",
                                      "paint.gradient",
                                      "paint.mixerBrush",
                                      "edit.fill",
                                      "image.applyImage",
                                      "edit.fillForeground",
                                      "edit.fillBackground",
                                      "edit.fillForegroundPreserve",
                                      "edit.fillBackgroundPreserve",
                                      "edit.fillHistory",
                                      "edit.copy",
                                      "edit.paste",
                                      "edit.pasteSpecial.pasteInPlace",
                                      "edit.pasteSpecial.pasteInto",
                                      "edit.pasteSpecial.pasteOutside",
                                      "edit.transform.rotate180",
                                      "edit.transform.rotate90Cw",
                                      "edit.transform.rotate90Ccw",
                                      "edit.transform.flipHorizontal",
                                      "edit.transform.flipVertical",
                                      "paint.cloneStamp",
                                      "paint.patternStamp",
                                      "paint.healingBrush",
                                      "paint.spotHealing",
                                      "paint.dodge",
                                      "paint.burn",
                                      "paint.sponge",
                                      "paint.blur",
                                      "paint.sharpen",
                                      "paint.smudge",
                                      "paint.historyBrush",
                                      "paint.redEye",
                                      "paint.remove"};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        if (!strcmp(id, ids[i])) {
            return true;
        }
    }
    return false;
}
bool pc_engine_targeted_vector_mask(const pc_engine_session *s)
{
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    return state && state->channel_view && state->channel_view->target_kind == CT_VECTOR_MASK;
}

bool pc_engine_targeted_mask(const pc_engine_session *session)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return false;
    }
    const struct pc_engine_channel_view *view = state->channel_view;
    if (view && view->target_kind == CT_LAYER_MASK) {
        return true;
    }
    return (!view || view->target_kind == CT_COMPOSITE) && state->has_layer_mask_view;
}
yyjson_mut_val *pc_engine_hook_inject_target(const pc_engine_session *session, const char *id,
                                             yyjson_mut_doc *doc, yyjson_mut_val *params)
{
    if (!channel_routed(id) || yyjson_mut_obj_get(params, "target")) {
        return params;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return params;
    }
    bool quick = false;
    size_t index = 0;
    yyjson_mut_val *target = NULL;
    if (pc_engine_targeted_channel(session, &quick, &index)) {
        if (quick) {
            target = yyjson_mut_str(doc, "quickMask");
        } else {
            target = yyjson_mut_obj(doc);
            if (!target || !yyjson_mut_obj_add_uint(doc, target, "channel", index)) {
                return NULL;
            }
        }
    } else if (pc_engine_targeted_mask(session)) {
        target = yyjson_mut_str(doc, "mask");
    } else {
        return params;
    }
    if (!target) {
        return NULL;
    }
    if (!yyjson_mut_is_obj(params)) {
        params = yyjson_mut_obj(doc);
    }
    if (!params || !yyjson_mut_obj_add_val(doc, params, "target", target)) {
        return NULL;
    }
    return params;
}
size_t pc_engine_hook_color_restriction(const pc_engine_session *session, const char *id, yyjson_val *params)
{
    if (!channel_routed(id) || pc_engine_is_channel_target(params) || pc_engine_is_mask_target(params)) {
        return SIZE_MAX;
    }
    return pc_engine_targeted_color(session);
}

/* pc_engine_hook_restrict_to_color: after a pixel edit limited to colour channel k, restore the
 * other colour channels (and the layer's transparency) of layer `id` from `before`. Only changed
 * tiles are touched (COW tile-pointer comparison). */
pc_status pc_engine_hook_restrict_to_color(const pc_document *before, pc_document *after, pc_layer_id id,
                                           size_t k, pc_error *err)
{
    const pc_layer *lo = pc_document_layer(before, id);
    pc_layer *ln = pc_document_layer_mut(after, id);
    if (lo == NULL || ln == NULL) {
        return PC_OK;
    }
    const pc_surface *old = pc_layer_surface(lo);
    pc_surface *new = pc_layer_surface_mut(ln);
    if (old == NULL || new == NULL) {
        return PC_OK;
    }
    if (!pc_pixel_format_eq(pc_surface_format(old), pc_surface_format(new))) {
        return PC_OK;
    }
    size_t n = pc_surface_channels(new);
    if (k >= n) {
        return PC_OK;
    }
    /* Collect changed tile coords: new tiles not shared with old, plus old tiles gone from new. */
    pc_tile *coords = NULL;
    size_t ncoords = 0, cap = 0;
    size_t cur = 0;
    pc_tile c;
    const pc_raster_tile *t;
    while (pc_surface_next_tile(new, &cur, &c, &t)) {
        const pc_raster_tile *ot = pc_surface_tile(old, c);
        if (ot != t) {
            if (ncoords == cap) {
                size_t nc = cap ? cap * 2 : 16;
                pc_tile *g = realloc(coords, nc * sizeof(pc_tile));
                if (g == NULL) {
                    free(coords);
                    return pc_fail(err, PC_ERR_NOMEM, "channel restriction coordinates");
                }
                coords = g;
                cap = nc;
            }
            coords[ncoords++] = c;
        }
    }
    cur = 0;
    while (pc_surface_next_tile(old, &cur, &c, &t)) {
        if (pc_surface_tile(new, c) == NULL) {
            if (ncoords == cap) {
                size_t nc = cap ? cap * 2 : 16;
                pc_tile *g = realloc(coords, nc * sizeof(pc_tile));
                if (g == NULL) {
                    free(coords);
                    return pc_fail(err, PC_ERR_NOMEM, "channel restriction coordinates");
                }
                coords = g;
                cap = nc;
            }
            coords[ncoords++] = c;
        }
    }
    for (size_t ci = 0; ci < ncoords; ci++) {
        pc_rect r = pc_tile_rect(coords[ci]);
        size_t npx = (size_t)pc_rect_width(r) * (size_t)pc_rect_height(r);
        float *o = NULL, *v = NULL;
        size_t ol = 0, vl = 0;
        pc_status status = pc_surface_read_region(old, r, &o, &ol, err);
        if (status != PC_OK) {
            free(coords);
            return status;
        }
        status = pc_surface_read_region(new, r, &v, &vl, err);
        if (status != PC_OK) {
            free(o);
            free(coords);
            return status;
        }
        for (size_t i = 0; i < npx; i++) {
            for (size_t ch = 0; ch < n; ch++) {
                if (ch != k) {
                    v[i * n + ch] = o[i * n + ch];
                }
            }
        }
        status = pc_surface_write_region(new, r, v, npx * n, err);
        free(o);
        free(v);
        if (status != PC_OK) {
            free(coords);
            return status;
        }
    }
    free(coords);
    pc_surface_prune(new);
    return PC_OK;
}

/* pc_engine_hook_fix_view: keep the Channels-panel view valid after a document change (undo,
 * deleted channels, mode changes). Mirrors channel_cmds::fix_view. */
void pc_engine_hook_fix_view(pc_engine_doc_state *st)
{
    /* The mask view (E5) is fixed first, as the reference's fix_view does. */
    pc_engine_hook_mask_view_fix(st);
    struct pc_engine_channel_view *v = st->channel_view;
    if (v == NULL) {
        return;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    size_t nch = doc->nchannels;
    size_t colors = pc_engine_color_count(doc);
    bool quick = doc->has_quick_mask;

    if (v->nalpha_visible > nch) {
        v->nalpha_visible = nch;
    }
    if (v->ncolor_hidden > colors) {
        v->ncolor_hidden = colors;
    }
    switch (v->target_kind) {
    case CT_ALPHA:
        if (v->target_index >= nch) {
            v->target_kind = CT_COMPOSITE;
        }
        break;
    case CT_COLOR:
        if (v->target_index >= colors || colors < 2) {
            v->target_kind = CT_COMPOSITE;
        }
        break;
    case CT_LAYER_MASK: {
        const pc_layer *l = st->has_active_layer ? pc_document_layer(doc, st->active_layer) : NULL;
        if (l == NULL || !l->has_mask) {
            v->target_kind = CT_COMPOSITE;
        }
        break;
    }
    case CT_VECTOR_MASK: {
        const pc_layer *l = st->has_active_layer ? pc_document_layer(doc, st->active_layer) : NULL;
        if (l == NULL || !l->has_vector_mask) {
            v->target_kind = CT_COMPOSITE;
        }
        break;
    }
    default:
        break;
    }

    /* An alpha/spot eye, Quick Mask, or the active layer's mask can be the only thing on the
     * canvas (so every colour eye can be off). Otherwise the colour channels come back. */
    bool shown = false;
    for (size_t i = 0; i < nch && !shown; i++) {
        if (cv_alpha_shown(v, i)) {
            shown = true;
        }
    }
    if (!shown && quick && !v->quick_mask_hidden) {
        shown = true;
    }
    if (!shown && st->has_active_layer) {
        const pc_layer *l = pc_document_layer(doc, st->active_layer);
        if (l != NULL && l->has_mask) {
            shown = true;
        }
    }
    if (cv_visible_colors(v, colors) == 0 && !shown) {
        cv_clear_colors(v);
    }
}

/* ============================================================================================
 * channels_json / inspect hook
 * ========================================================================================== */

static const char *indicates_name(pc_color_indicates ind)
{
    return ind == PC_INDICATES_SELECTED_AREAS ? "selectedAreas" : "maskedAreas";
}

static yyjson_mut_val *rgb_arr(yyjson_mut_doc *d, const pc_color *c)
{
    float rgb[3];
    pc_color_to_rgb(c, rgb);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    for (int i = 0; i < 3; i++) {
        yyjson_mut_arr_add_real(d, a, rgb[i]);
    }
    return a;
}

static yyjson_mut_val *target_json(yyjson_mut_doc *d, const struct pc_engine_channel_view *v)
{
    int kind = v != NULL ? v->target_kind : CT_COMPOSITE;
    yyjson_mut_val *o = yyjson_mut_obj(d);
    const char *k;
    bool has_index = false;
    switch (kind) {
    case CT_COLOR:
        k = "color";
        has_index = true;
        break;
    case CT_ALPHA:
        k = "alpha";
        has_index = true;
        break;
    case CT_LAYER_MASK:
        k = "layerMask";
        break;
    case CT_VECTOR_MASK:
        k = "vectorMask";
        break;
    default:
        k = "composite";
        break;
    }
    yyjson_mut_obj_add_str(d, o, "kind", k);
    if (has_index) {
        yyjson_mut_obj_add_uint(d, o, "index", v->target_index);
    }
    return o;
}

void pc_engine_hook_inspect_channels(yyjson_mut_doc *d, yyjson_mut_val *obj, const pc_engine_doc_state *st)
{
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_color_mode mode = edit_mode(doc);
    const struct pc_engine_channel_view *v = st->channel_view;
    size_t colors = pc_engine_color_count(doc);

    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "composite", composite_name(mode));

    size_t ncolors = 0;
    const char *const *names = color_names(mode, &ncolors);
    yyjson_mut_val *carr = yyjson_mut_arr(d);
    for (size_t k = 0; k < ncolors; k++) {
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, o, "name", names[k]);
        yyjson_mut_obj_add_bool(d, o, "visible", cv_color_visible(v, k));
        yyjson_mut_arr_add_val(carr, o);
    }
    yyjson_mut_obj_add_val(d, root, "colors", carr);

    yyjson_mut_val *aarr = yyjson_mut_arr(d);
    for (size_t i = 0; i < doc->nchannels; i++) {
        const pc_alpha_channel *c = &doc->channels[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, o, "index", i);
        yyjson_mut_obj_add_strcpy(d, o, "name", pc_str_c(&c->name));
        yyjson_mut_obj_add_bool(d, o, "visible", cv_alpha_shown(v, i));
        if (c->has_spot) {
            pc_color sc = c->spot_color;
            yyjson_mut_val *sp = yyjson_mut_obj(d);
            yyjson_mut_obj_add_val(d, sp, "color", rgb_arr(d, &sc));
            yyjson_mut_obj_add_real(d, sp, "solidity", c->spot_solidity);
            yyjson_mut_obj_add_val(d, o, "spot", sp);
        } else {
            yyjson_mut_obj_add_null(d, o, "spot");
        }
        yyjson_mut_obj_add_val(d, o, "color", rgb_arr(d, &c->color));
        yyjson_mut_obj_add_real(d, o, "opacity", c->opacity);
        yyjson_mut_obj_add_str(d, o, "indicates", indicates_name(c->indicates));
        yyjson_mut_arr_add_val(aarr, o);
    }
    yyjson_mut_obj_add_val(d, root, "alpha", aarr);

    if (doc->has_quick_mask) {
        yyjson_mut_val *q = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, q, "visible", !pc_engine_channel_quick_mask_hidden(st));
        yyjson_mut_obj_add_val(d, q, "color", rgb_arr(d, &doc->quick_mask.color));
        yyjson_mut_obj_add_real(d, q, "opacity", doc->quick_mask.opacity);
        yyjson_mut_obj_add_str(d, q, "indicates", indicates_name(doc->quick_mask.indicates));
        yyjson_mut_obj_add_val(d, root, "quickMask", q);
    } else {
        yyjson_mut_obj_add_null(d, root, "quickMask");
    }
    yyjson_mut_obj_add_val(d, root, "target", target_json(d, v));
    yyjson_mut_obj_add_bool(d, root, "compositeVisible", cv_visible_colors(v, colors) == colors);
    /* layerMaskView: mask view (E5) is reported top-level by inspect; the reference also nests it
     * here. Reuse the E5 hook to fill the same field inside "channels". */
    pc_engine_hook_inspect_mask_view(d, root, st);

    yyjson_mut_obj_add_val(d, obj, "channels", root);
}

/* The Channels panel state of the active document (for the channel.* result and list). */
static pc_status channels_result(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_result_null(out, err);
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    /* channels_json fills the "channels" key of root; the panel list is that inner object. */
    pc_engine_hook_inspect_channels(d, root, st);
    yyjson_mut_val *inner = yyjson_mut_obj_get(root, "channels");
    yyjson_mut_doc_set_root(d, inner != NULL ? inner : root);
    return PC_OK;
}

/* Bump the revision for a view-only change without dirtying a clean document. */
static void touch(pc_engine_doc_state *st)
{
    bool clean = st->saved_revision == st->revision;
    st->revision++;
    if (clean) {
        st->saved_revision = st->revision;
    }
    st->has_last_damage = false;
}

static void fix_view_now(pc_engine_doc_state *st)
{
    pc_engine_hook_fix_view(st);
    touch(st);
}

/* Make `t` (kind,index) the edit target and show it as Photoshop does when a row is clicked. */
static pc_status set_target(pc_engine_session *s, int kind, size_t index, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    size_t colors = pc_engine_color_count(doc);
    size_t nch = doc->nchannels;
    struct pc_engine_channel_view *v = cv_ensure(st);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "channel view");
    }
    v->target_kind = kind;
    v->target_index = index;
    switch (kind) {
    case CT_COMPOSITE:
        cv_clear_colors(v);
        cv_clear_alpha(v);
        break;
    case CT_LAYER_MASK:
    case CT_VECTOR_MASK:
        cv_clear_colors(v);
        break;
    case CT_COLOR:
        cv_clear_colors(v);
        for (size_t c = 0; c < colors; c++) {
            if (!cv_set_color_visible(v, c, c == index)) {
                return pc_fail(err, PC_ERR_NOMEM, "channel view");
            }
        }
        cv_clear_alpha(v);
        break;
    case CT_ALPHA:
        cv_clear_colors(v);
        for (size_t c = 0; c < colors; c++) {
            if (!cv_set_color_visible(v, c, false)) {
                return pc_fail(err, PC_ERR_NOMEM, "channel view");
            }
        }
        cv_clear_alpha(v);
        for (size_t c = 0; c < nch; c++) {
            if (!cv_set_alpha_visible(v, c, c == index)) {
                return pc_fail(err, PC_ERR_NOMEM, "channel view");
            }
        }
        break;
    default:
        break;
    }
    fix_view_now(st);
    return PC_OK;
}

/* ============================================================================================
 * Enabled predicates
 * ========================================================================================== */

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_channels(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return pc_doc_arc_get(d->doc)->nchannels > 0
               ? PC_OK
               : pc_fail(reason, PC_ERR_INVALID, "the document has no alpha channels");
}

static pc_status has_selection(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return pc_doc_arc_get(d->doc)->has_selection ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no selection");
}

static pc_status has_spot(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    for (size_t i = 0; i < doc->nchannels; i++) {
        if (doc->channels[i].has_spot) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "the document has no spot channels");
}

static pc_status can_split(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (pc_engine_color_count(doc) + doc->nchannels < 2) {
        return pc_fail(reason, PC_ERR_INVALID, "only one channel to split");
    }
    return PC_OK;
}

/* Count open grayscale documents of the active document's size (active first). *out owned. */
static size_t mergeable(const pc_engine_session *s, size_t **out)
{
    *out = NULL;
    const pc_engine_doc_state *a = pc_engine_session_active(s);
    if (a == NULL) {
        return 0;
    }
    size_t ai;
    if (!pc_engine_session_active_index(s, &ai)) {
        return 0;
    }
    size_t n = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &n);
    const pc_document *ad = pc_doc_arc_get(a->doc);
    size_t *v = malloc(n ? n * sizeof(size_t) : 1);
    if (v == NULL) {
        return 0;
    }
    size_t count = 0;
    bool active_ok = ad->mode == PC_COLOR_MODE_GRAYSCALE;
    if (active_ok) {
        v[count++] = ai;
    }
    for (size_t i = 0; i < n; i++) {
        if (i == ai) {
            continue;
        }
        const pc_document *d = pc_doc_arc_get(docs[i].doc);
        if (d->mode == PC_COLOR_MODE_GRAYSCALE && d->size.width == ad->size.width &&
            d->size.height == ad->size.height) {
            v[count++] = i;
        }
    }
    *out = v;
    return count;
}

static pc_status can_merge(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    size_t *m = NULL;
    size_t k = mergeable(s, &m);
    free(m);
    if (k >= 2) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID,
                   "Merge Channels needs at least two open grayscale documents of the same size");
}

static pc_status has_layer_mask(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_layer *l =
        st->has_active_layer ? pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer) : NULL;
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (!l->has_mask) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer has no layer mask");
    }
    return PC_OK;
}

#include "channel_cmds_sel.inc"
#include "channel_cmds_manage.inc"

/* ============================================================================================
 * Apply Image / Calculations (channel_cmds::{apply_image, calculations})
 * ========================================================================================== */

static double get_f64(yyjson_val *p, const char *key, double def)
{
    double d;
    yyjson_val *v = pc_json_get(p, key);
    if (v != NULL && pc_json_as_f64(v, &d) && isfinite(d)) {
        return d;
    }
    return def;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Apply Image / Calculations blending: separable layer modes plus Add and Subtract. */
typedef enum { BLEND_MODE, BLEND_ADD, BLEND_SUBTRACT } blending_kind;
typedef struct {
    blending_kind kind;
    pc_blend_mode mode;
    float scale, offset;
} blending;

static pc_status blending_parse(yyjson_val *p, const char *cmd, blending *out, pc_error *err)
{
    size_t nl = 0;
    const char *name = get_str(p, "blending", &nl);
    char buf[32];
    if (name == NULL) {
        name = "multiply";
        nl = strlen(name);
    }
    out->scale = (float)clampd(get_f64(p, "scale", 1.0), 1.0, 2.0);
    out->offset = (float)(clampd(get_f64(p, "offset", 0.0), -255.0, 255.0) / 255.0);
    if (nl < sizeof buf) {
        memcpy(buf, name, nl);
        buf[nl] = '\0';
        for (size_t i = 0; i < nl; i++) {
            if (buf[i] >= 'A' && buf[i] <= 'Z') {
                buf[i] = (char)(buf[i] - 'A' + 'a');
            }
        }
        if (strcmp(buf, "add") == 0) {
            out->kind = BLEND_ADD;
            return PC_OK;
        }
        if (strcmp(buf, "subtract") == 0) {
            out->kind = BLEND_SUBTRACT;
            return PC_OK;
        }
    }
    pc_blend_mode m;
    if (pc_engine_blend_from_str(name, nl, &m) && pc_blend_is_separable(m) && m != PC_BLEND_DISSOLVE &&
        m != PC_BLEND_PASS_THROUGH) {
        out->kind = BLEND_MODE;
        out->mode = m;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "unsupported blending `%.*s`", (int)nl, name);
}

static float blending_apply(const blending *b, float base, float top)
{
    float r;
    switch (b->kind) {
    case BLEND_ADD:
        r = (base + top) / b->scale + b->offset;
        break;
    case BLEND_SUBTRACT:
        r = (base - top) / b->scale + b->offset;
        break;
    default:
        r = pc_blend_channel(b->mode, base, top);
        break;
    }
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
}

/* Resolve a source spec: the nested `key` object, or a flat-form object built from
 * `<prefix>Document`/`Layer`/`Channel`/`Invert`. *scratch (owned yyjson_doc, free) is set when a
 * flat doc was built; *out is the spec val, or NULL when absent (= use defaults). */
static yyjson_val *source_spec(yyjson_val *p, const char *key, const char *prefix, yyjson_doc **scratch)
{
    *scratch = NULL;
    yyjson_val *nested = pc_json_get(p, key);
    if (nested != NULL) {
        return nested;
    }
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    if (md == NULL) {
        return NULL;
    }
    yyjson_mut_val *obj = yyjson_mut_obj(md);
    if (obj == NULL) {
        yyjson_mut_doc_free(md);
        return NULL;
    }
    static const char *const keys[4] = {"document", "layer", "channel", "invert"};
    static const char *const fields[4] = {"Document", "Layer", "Channel", "Invert"};
    bool any = false;
    for (size_t i = 0; i < 4; i++) {
        char fk[48];
        (void)snprintf(fk, sizeof fk, "%s%s", prefix, fields[i]);
        yyjson_val *fv = pc_json_get(p, fk);
        if (fv == NULL || yyjson_is_null(fv)) {
            continue;
        }
        if (yyjson_is_str(fv)) {
            size_t sl = 0;
            const char *sv = pc_json_as_str(fv, &sl);
            if (sv != NULL && (sl == 0 || (strcmp(keys[i], "layer") == 0 && str_is(sv, sl, "merged")))) {
                continue;
            }
        }
        yyjson_mut_val *copy = yyjson_val_mut_copy(md, fv);
        if (copy == NULL || !yyjson_mut_obj_add_val(md, obj, keys[i], copy)) {
            yyjson_mut_doc_free(md);
            return NULL;
        }
        any = true;
    }
    if (!any) {
        yyjson_mut_doc_free(md);
        return NULL;
    }
    yyjson_mut_doc_set_root(md, obj);
    yyjson_doc *imut = yyjson_mut_doc_imut_copy(md, NULL);
    yyjson_mut_doc_free(md);
    if (imut == NULL) {
        return NULL;
    }
    *scratch = imut;
    return yyjson_doc_get_root(imut);
}

/* The mask plane for Apply Image / Calculations, or none. *out owned (free), *has set. */
static pc_status mask_plane(pc_engine_session *s, yyjson_val *p, const char *cmd, float **out, size_t *outn,
                            bool *has, pc_error *err)
{
    *out = NULL;
    *outn = 0;
    *has = false;
    yyjson_doc *scratch = NULL;
    yyjson_val *m = source_spec(p, "mask", "mask", &scratch);
    if (m == NULL || yyjson_is_null(m)) {
        if (scratch != NULL) {
            yyjson_doc_free(scratch);
        }
        return PC_OK;
    }
    bool bv;
    if (pc_json_as_bool(m, &bv) && !bv) {
        if (scratch != NULL) {
            yyjson_doc_free(scratch);
        }
        return PC_OK;
    }
    yyjson_val *cv = pc_json_get(m, "channel");
    if (cv == NULL) {
        if (scratch != NULL) {
            yyjson_doc_free(scratch);
        }
        return PC_OK;
    }
    size_t cl = 0;
    const char *cs = pc_json_as_str(cv, &cl);
    if (cs != NULL && str_is(cs, cl, "none")) {
        if (scratch != NULL) {
            yyjson_doc_free(scratch);
        }
        return PC_OK;
    }
    plane_list pl = {0};
    pc_status rc = source_planes(s, m, cmd, true, &pl, err);
    if (scratch != NULL) {
        yyjson_doc_free(scratch);
    }
    if (rc != PC_OK) {
        return rc;
    }
    if (pl.planes == NULL || pl.count == 0 || pl.planes[0] == NULL) {
        plane_list_fini(&pl);
        return PC_OK; /* no usable mask plane (source has no channels): treat as no mask */
    }
    /* Take plane 0 (gray) and free the rest. */
    *out = pl.planes[0];
    *outn = pl.n;
    pl.planes[0] = NULL;
    plane_list_fini(&pl);
    *has = true;
    return PC_OK;
}

static pc_status has_apply_target(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_edits_channel(s)) {
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_layer *l =
        d->has_active_layer ? pc_document_layer(pc_doc_arc_get(d->doc), d->active_layer) : NULL;
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "Apply Image needs a pixel layer (active layer is %s %s layer)",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

struct apply_image_ctx {
    const blending *bl;
    float opacity;
    bool preserve;
    const plane_list *src;
    const float *mask; /* NULL = none */
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
};

static pc_status apply_image_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct apply_image_ctx *c = vctx;
    pc_rect area = pc_document_bounds(doc);
    float *sel = NULL;
    size_t seln = 0;
    pc_status rc = pc_algo_selection_mask_from_surface(doc->has_selection ? &doc->selection : NULL, area,
                                                       &sel, &seln, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* weight(i) */
#define WEIGHT(i) (c->opacity * (c->mask != NULL ? c->mask[i] : 1.0f) * (sel != NULL ? sel[i] : 1.0f))
    if (pc_engine_is_channel_target(c->p)) {
        pc_surface *surf = NULL;
        bool lock = false;
        rc = pc_engine_target_surface(doc, false, 0, c->p, &surf, &lock, err);
        if (rc != PC_OK) {
            free(sel);
            return rc;
        }
        float *base = NULL;
        size_t basen = 0;
        rc = read_plane(surf, area, &base, &basen, err);
        if (rc != PC_OK) {
            free(sel);
            return rc;
        }
        /* One source plane per target pixel (the mean of the source's planes), fully populated
         * over `basen` so every read below is initialised. */
        float *srcp = malloc((basen ? basen : 1) * sizeof *srcp);
        if (srcp == NULL) {
            free(base);
            free(sel);
            return pc_fail(err, PC_ERR_NOMEM, "apply image");
        }
        size_t sc = c->src->count;
        for (size_t i = 0; i < basen; i++) {
            float sum = 0.0f;
            for (size_t k = 0; k < sc; k++) {
                sum += (i < c->src->n) ? c->src->planes[k][i] : 0.0f;
            }
            srcp[i] = sc > 0 ? sum / (float)sc : 0.0f;
        }
        for (size_t i = 0; i < basen; i++) {
            float b = base[i];
            base[i] = b + (blending_apply(c->bl, b, srcp[i]) - b) * WEIGHT(i);
        }
        pc_surface newsurf;
        rc = plane_surface(base, basen, area, pc_surface_format(surf), &newsurf, err);
        free(base);
        free(srcp);
        free(sel);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(surf);
        *surf = newsurf;
        return PC_OK;
    }
    if (!c->has_layer) {
        free(sel);
        return pc_engine_err_other(err, "no active layer");
    }
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_surface *surf = NULL;
    rc = pc_engine_paint_surface(doc, c->layer, NULL, &surf, err);
    if (rc != PC_OK) {
        free(sel);
        return rc;
    }
    pc_pixel_format sf = pc_surface_format(surf);
    if (sf.mode != fmt.mode || sf.sample != fmt.sample || sf.alpha != fmt.alpha) {
        free(sel);
        return pc_engine_err_other(err, "the target layer's format doesn't match the document");
    }
    size_t n = pc_pixel_format_channels(sf);
    size_t colors = pc_color_mode_channels(sf.mode);
    if (n == 0 || colors == 0) {
        free(sel);
        return PC_OK;
    }
    float *v = NULL;
    size_t vlen = 0;
    rc = pc_surface_read_region(surf, area, &v, &vlen, err);
    if (rc != PC_OK) {
        free(sel);
        return rc;
    }
    size_t count = vlen / n;
    for (size_t i = 0; i < count; i++) {
        float *px = &v[i * n];
        float base_a = sf.alpha ? px[colors] : 1.0f;
        base_a = base_a < 0.0f ? 0.0f : (base_a > 1.0f ? 1.0f : base_a);
        float w = WEIGHT(i);
        if (c->preserve) {
            w *= base_a;
        }
        if (w <= 0.0f) {
            continue;
        }
        float out_a = c->preserve ? base_a : w + base_a * (1.0f - w);
        for (size_t ch = 0; ch < colors; ch++) {
            float top = ch < c->src->count ? c->src->planes[ch][i] : c->src->planes[0][i];
            float blended = top + (blending_apply(c->bl, px[ch], top) - top) * base_a;
            px[ch] = c->preserve ? px[ch] + (blended - px[ch]) * w
                                 : (w * blended + base_a * (1.0f - w) * px[ch]) / out_a;
        }
        if (sf.alpha) {
            px[colors] = out_a;
        }
    }
    rc = pc_surface_write_region(surf, area, v, vlen, err);
    free(v);
    free(sel);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    return PC_OK;
#undef WEIGHT
}

static pc_status run_apply_image(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "image.applyImage";
    blending bl;
    pc_status rc = blending_parse(p, cmd, &bl, err);
    if (rc != PC_OK) {
        return rc;
    }
    float opacity = (float)(clampd(get_f64(p, "opacity", 100.0), 0.0, 100.0) / 100.0);
    bool preserve = get_bool(p, "preserveTransparency", false);
    yyjson_doc *scratch = NULL;
    yyjson_val *spec = source_spec(p, "source", "source", &scratch);
    plane_list src = {0};
    rc = source_planes(s, spec, cmd, false, &src, err);
    if (scratch != NULL) {
        yyjson_doc_free(scratch);
    }
    if (rc != PC_OK) {
        return rc;
    }
    float *mask = NULL;
    size_t maskn = 0;
    bool has_mask = false;
    rc = mask_plane(s, p, cmd, &mask, &maskn, &has_mask, err);
    if (rc != PC_OK) {
        plane_list_fini(&src);
        return rc;
    }
    (void)maskn;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    struct apply_image_ctx c = {&bl,
                                opacity,
                                preserve,
                                &src,
                                has_mask ? mask : NULL,
                                d != NULL && d->has_active_layer,
                                d != NULL ? d->active_layer : 0,
                                p};
    rc = pc_engine_session_edit(s, "Apply Image", apply_image_edit, &c, err);
    plane_list_fini(&src);
    free(mask);
    if (rc != PC_OK) {
        return rc;
    }
    pc_json_value_fini(out);
    return PC_OK;
}

struct calc_new_ctx {
    const char *name; /* NULL = auto */
    const float *out;
    size_t n;
    size_t index; /* result */
};

static pc_status calc_new_channel_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                       pc_error *err)
{
    (void)ha;
    (void)active;
    struct calc_new_ctx *c = vctx;
    char nm[128];
    if (c->name != NULL) {
        (void)snprintf(nm, sizeof nm, "%s", c->name);
    } else {
        next_alpha_name(doc, "Alpha", nm, sizeof nm);
    }
    pc_rect area = pc_document_bounds(doc);
    pc_surface surf;
    pc_status rc = plane_surface(c->out, c->n, area, channel_format(doc), &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_alpha_channel ch;
    rc = pc_alpha_channel_init(&ch, nm, &surf, err);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    rc = append_channel(doc, &ch, err);
    if (rc != PC_OK) {
        pc_alpha_channel_fini(&ch);
        return rc;
    }
    c->index = doc->nchannels - 1;
    return PC_OK;
}

static pc_status calc_selection_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx,
                                     pc_error *err)
{
    (void)ha;
    (void)active;
    struct calc_new_ctx *c = vctx;
    pc_rect area = pc_document_bounds(doc);
    pc_surface combined;
    bool some = false;
    pc_status rc =
        pc_algo_selection_combine(NULL, c->out, c->n, area, PC_ALGO_SELECTION_REPLACE, &combined, &some, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
    }
    if (some) {
        doc->selection = combined;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&combined);
        doc->has_selection = false;
    }
    c->index = some ? 1 : 0;
    return PC_OK;
}

static pc_status run_calculations(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "image.calculations";
    blending bl;
    pc_status rc = blending_parse(p, cmd, &bl, err);
    if (rc != PC_OK) {
        return rc;
    }
    float opacity = (float)(clampd(get_f64(p, "opacity", 100.0), 0.0, 100.0) / 100.0);
    yyjson_doc *sc1 = NULL, *sc2 = NULL;
    yyjson_val *spec1 = source_spec(p, "source1", "source1", &sc1);
    plane_list s1 = {0};
    rc = source_planes(s, spec1, cmd, true, &s1, err);
    if (sc1 != NULL) {
        yyjson_doc_free(sc1);
    }
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_val *spec2 = source_spec(p, "source2", "source2", &sc2);
    plane_list s2 = {0};
    rc = source_planes(s, spec2, cmd, true, &s2, err);
    if (sc2 != NULL) {
        yyjson_doc_free(sc2);
    }
    if (rc != PC_OK) {
        plane_list_fini(&s1);
        return rc;
    }
    float *mask = NULL;
    size_t maskn = 0;
    bool has_mask = false;
    rc = mask_plane(s, p, cmd, &mask, &maskn, &has_mask, err);
    if (rc != PC_OK) {
        plane_list_fini(&s1);
        plane_list_fini(&s2);
        return rc;
    }
    (void)maskn;
    /* Source 1 is blended onto Source 2. */
    size_t n = s2.n < s1.n ? s2.n : s1.n;
    float *result = malloc((n ? n : 1) * sizeof *result);
    if (result == NULL) {
        plane_list_fini(&s1);
        plane_list_fini(&s2);
        free(mask);
        return pc_fail(err, PC_ERR_NOMEM, cmd);
    }
    for (size_t i = 0; i < n; i++) {
        float b = s2.planes[0][i], t = s1.planes[0][i];
        float m = has_mask && mask != NULL ? mask[i] : 1.0f;
        result[i] = b + (blending_apply(&bl, b, t) - b) * opacity * m;
    }
    plane_list_fini(&s1);
    plane_list_fini(&s2);
    free(mask);

    size_t rl = 0;
    const char *resname = get_str(p, "result", &rl);
    char nmbuf[128];
    const char *name = NULL;
    size_t nml = 0;
    const char *nm = get_str(p, "name", &nml);
    if (nm != NULL && nml < sizeof nmbuf) {
        memcpy(nmbuf, nm, nml);
        nmbuf[nml] = '\0';
        name = nmbuf;
    }
    yyjson_mut_doc *rd = NULL;
    if (resname == NULL || str_is(resname, rl, "newChannel")) {
        struct calc_new_ctx c = {name, result, n, 0};
        rc = pc_engine_session_edit(s, "Calculations", calc_new_channel_edit, &c, err);
        free(result);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "channel", c.index);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    if (str_is(resname, rl, "newDocument")) {
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        if (d == NULL) {
            free(result);
            return pc_engine_err_no_document(err);
        }
        pc_document nd;
        rc = gray_document(name != NULL ? name : "Untitled", pc_doc_arc_get(d->doc), result, n, &nd, err);
        free(result);
        if (rc != PC_OK) {
            return rc;
        }
        size_t di = 0;
        rc = pc_engine_session_add_document(s, &nd, NULL, &di, err);
        if (rc != PC_OK) {
            pc_document_fini(&nd);
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, root, "document", di);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    if (str_is(resname, rl, "selection")) {
        struct calc_new_ctx c = {NULL, result, n, 0};
        rc = pc_engine_session_edit(s, "Calculations", calc_selection_edit, &c, err);
        free(result);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_bool(rd, root, "selected", c.index != 0);
        yyjson_mut_doc_set_root(rd, root);
        return PC_OK;
    }
    free(result);
    return pc_engine_err_bad_params(err, cmd, "unknown result `%.*s` (newChannel|newDocument|selection)",
                                    (int)rl, resname != NULL ? resname : "");
}

/* ============================================================================================
 * Registry
 * ========================================================================================== */

static const pc_engine_command SPECS[] = {
    {"select.saveSelection", "Save Selection…", (const char *const[]){"Select"}, 1, NULL,
     "{\"name\":str?,\"channel\":\"new\"|index|name=\"new\",\"document\":index?,\"operation\":\"new|"
     "replace|add|subtract|intersect\"=\"new\"}",
     has_selection, run_save_selection, true},
    {"select.loadSelection", "Load Selection…", (const char *const[]){"Select"}, 1, NULL,
     "{\"channel\":index|name|\"composite\"|\"red|green|blue|…\"|\"transparency\"|\"mask\"|"
     "\"vectorMask\"|\"quickMask\"|\"selection\"|\"targeted\",\"layer\":id?,\"document\":index?,"
     "\"invert\":bool=false,\"operation\":\"new|add|subtract|intersect\"=\"new\"}",
     has_doc, run_load_selection, true},
    {"select.editInQuickMaskMode", "Edit in Quick Mask Mode", (const char *const[]){"Select"}, 1, "Q",
     "{\"on\":bool?=toggle}", has_doc, run_quick_mask, true},
    {"image.applyImage", "Apply Image…", (const char *const[]){"Image"}, 1, NULL,
     "{\"source\":{\"document\":index?,\"layer\":id|name|\"merged\"=\"merged\",\"channel\":"
     "\"composite\"|\"red|…\"|index|\"selection\"|\"transparency\"|\"mask\"=\"composite\",\"invert\":"
     "bool=false},\"blending\":\"normal|multiply|screen|…|add|subtract\"=\"multiply\",\"opacity\":0.."
     "100=100,\"scale\":1..2=1,\"offset\":-255..255=0,\"preserveTransparency\":bool=false,\"mask\":"
     "{\"document\",\"layer\",\"channel\",\"invert\"}?}",
     has_apply_target, run_apply_image, true},
    {"image.calculations", "Calculations…", (const char *const[]){"Image"}, 1, NULL,
     "{\"source1\":{\"document\",\"layer\",\"channel\",\"invert\"},\"source2\":{…},\"blending\":"
     "\"multiply|…\"=\"multiply\",\"opacity\":0..100=100,\"scale\":1..2=1,\"offset\":-255..255=0,"
     "\"mask\":{…}?,\"result\":\"newChannel|newDocument|selection\"=\"newChannel\",\"name\":str?}",
     has_doc, run_calculations, true},
    {"channel.new", "New Channel…", NULL, 0, NULL,
     "{\"name\":str?,\"fill\":\"black|white|selection\"=\"black\",\"color\":\"#rrggbb\"=\"#ff0000\","
     "\"opacity\":0..100=50,\"indicates\":\"masked|selected\"=\"masked\"}",
     has_doc, run_new_channel, true},
    {"channel.newSpot", "New Spot Channel…", NULL, 0, NULL,
     "{\"name\":str?,\"color\":\"#rrggbb\"=\"#ff0000\",\"solidity\":0..100=0,\"fromSelection\":bool="
     "true}",
     has_doc, run_new_spot, true},
    {"channel.duplicate", "Duplicate Channel…", NULL, 0, NULL,
     "{\"channel\":index|name|\"red|…\"|\"composite\"|\"quickMask\",\"name\":str?,\"document\":index|"
     "\"new\"?=active,\"invert\":bool=false}",
     has_doc, run_duplicate_channel, true},
    {"channel.delete", "Delete Channel", NULL, 0, NULL, "{\"channel\":index|name?=targeted}", has_channels,
     run_delete_channel, true},
    {"channel.rename", "Rename Channel", NULL, 0, NULL, "{\"channel\":index|name,\"name\":str}", has_channels,
     run_rename_channel, true},
    {"channel.move", "Reorder Channel", NULL, 0, NULL, "{\"channel\":index|name,\"to\":index}", has_channels,
     run_move_channel, true},
    {"channel.target", "Target Channel", NULL, 0, NULL,
     "{\"channel\":\"composite\"|\"mask\"|\"vectorMask\"|\"red|green|blue|…\"|index|name=\"composite\"}",
     has_doc, run_target_cmd, true},
    {"channel.setVisible", "Channel Visibility", NULL, 0, NULL,
     "{\"channel\":\"composite\"|\"red|…\"|index|name|\"quickMask\",\"visible\":bool?=toggle}", has_doc,
     run_set_visible, true},
    {"channel.options", "Channel Options…", NULL, 0, NULL,
     "{\"channel\":index|name|\"quickMask\",\"name\":str?,\"indicates\":\"masked|selected|spot\"?,"
     "\"color\":\"#rrggbb\"?,\"opacity\":0..100?,\"solidity\":0..100?}",
     has_doc, run_options, true},
    {"channel.mergeSpot", "Merge Spot Channel", NULL, 0, NULL,
     "{\"channel\":index|name?=targeted or first spot}", has_spot, run_merge_spot, true},
    {"channel.split", "Split Channels", NULL, 0, NULL, "{\"closeOriginal\":bool=true}", can_split, run_split,
     true},
    {"channel.merge", "Merge Channels…", NULL, 0, NULL,
     "{\"mode\":\"rgb|cmyk|lab\"=\"rgb\",\"documents\":[index,…]?=open grayscale docs of the active "
     "size,\"name\":str?}",
     can_merge, run_merge, true},
    {"channel.list", "Channels", NULL, 0, NULL, "{}", has_doc, run_channel_list, false},
    {"channel.target.composite", "Target Composite Channel", NULL, 0, "Cmd+2", "{}", has_doc,
     run_target_composite, true},
    {"channel.target.layerMask", "Target Layer Mask", NULL, 0, "Cmd+\\", "{}", has_layer_mask,
     run_target_layer_mask, true},
    {"select.loadLuminosity", "Load Luminosity", NULL, 0, "Cmd+Alt+2", "{}", has_doc, run_load_luminosity,
     true},
    {"channel.target.slot3", "Target Channel 3", NULL, 0, "Cmd+3", "{}", slot_ok3, run_slot3, true},
    {"channel.target.slot4", "Target Channel 4", NULL, 0, "Cmd+4", "{}", slot_ok4, run_slot4, true},
    {"channel.target.slot5", "Target Channel 5", NULL, 0, "Cmd+5", "{}", slot_ok5, run_slot5, true},
    {"channel.target.slot6", "Target Channel 6", NULL, 0, "Cmd+6", "{}", slot_ok6, run_slot6, true},
    {"channel.target.slot7", "Target Channel 7", NULL, 0, "Cmd+7", "{}", slot_ok7, run_slot7, true},
    {"channel.target.slot8", "Target Channel 8", NULL, 0, "Cmd+8", "{}", slot_ok8, run_slot8, true},
    {"channel.target.slot9", "Target Channel 9", NULL, 0, "Cmd+9", "{}", slot_ok9, run_slot9, true},
};

const pc_engine_command *pc_engine_channel_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}

bool pc_engine_channel_color_shown(const pc_engine_doc_state *d, size_t index)
{
    return cv_color_visible(d ? d->channel_view : NULL, index);
}
uint64_t pc_engine_channel_display_key(const pc_engine_doc_state *d)
{
    if (!d) {
        return 0;
    }
    uint64_t key = d->revision ^ d->active_layer;
    const struct pc_engine_channel_view *v = d->channel_view;
    if (v) {
        for (size_t i = 0; i < v->ncolor_hidden; i++) {
            key = (key ^ (uint64_t)v->color_hidden[i]) * UINT64_C(1099511628211);
        }
        for (size_t i = 0; i < v->nalpha_visible; i++) {
            key = (key ^ (uint64_t)v->alpha_visible[i]) * UINT64_C(1099511628211);
        }
        key = (key ^ (uint64_t)v->quick_mask_hidden) * UINT64_C(1099511628211);
    }
    key ^= d->has_layer_mask_view ? (d->layer_mask_view_layer * UINT64_C(6364136223846793005)) ^
                                        (uint64_t)d->layer_mask_view_mode
                                  : 0;
    return key;
}

void pc_engine_quick_mask_display(const pc_engine_session *session, float color[3], float *opacity)
{
    if (color) {
        pc_color_to_rgb(&session->quick_mask_color, color);
    }
    if (opacity) {
        *opacity = session->quick_mask_opacity;
    }
}
