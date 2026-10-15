/* Image > Mode > Multichannel, and conversions out of it (port of
 * crates/engine/src/multichannel_cmds.rs).
 *
 * A Multichannel document has no layers: its image is a set of ink channels, kept as spot channels
 * in Document::channels (value = ink density, 1 = solid; the compositor prints them over white).
 * Like Photoshop the conversion flattens and turns each colour channel into an ink:
 *   RGB  -> Cyan, Magenta, Yellow (red/green/blue read as ink: density = 1 - value);
 *   CMYK -> Cyan, Magenta, Yellow, Black (the plates as they are);
 *   Lab  -> Alpha 1, Alpha 2, Alpha 3 (lightness, a, b, printed in black);
 *   Grayscale -> Black; Duotone -> one channel per ink (its curve applied).
 * Existing alpha channels stay alpha channels. convert_from reinterprets the channel data when the
 * count matches the target (3 -> RGB/Lab, 4 -> CMYK, 1 -> Grayscale); otherwise the printed look is
 * converted. */
#include <stdlib.h>
#include <string.h>

#include "base/blob.h"
#include "base/mem.h"
#include "color/color.h"
#include "compose/compose.h"
#include "compose/effects.h"
#include "doc/doc.h"
#include "doc/mode.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "engine_internal.h"

/* Display colours of the process inks (sRGB). */
#define MC_CYAN pc_color_rgb(0.0f, 0.682f, 0.937f)
#define MC_MAGENTA pc_color_rgb(0.925f, 0.0f, 0.549f)
#define MC_YELLOW pc_color_rgb(1.0f, 0.949f, 0.0f)
#define MC_BLACK pc_color_rgb(0.137f, 0.122f, 0.125f)

static bool mc_is_source_mode(pc_color_mode m)
{
    return m == PC_COLOR_MODE_RGB || m == PC_COLOR_MODE_CMYK || m == PC_COLOR_MODE_LAB ||
           m == PC_COLOR_MODE_GRAYSCALE || m == PC_COLOR_MODE_DUOTONE;
}

static pc_status mc_enabled(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    pc_color_mode mode = pc_doc_arc_get(d->doc)->mode;
    if (mc_is_source_mode(mode)) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID,
                   "Multichannel needs an RGB, CMYK, Lab, Grayscale or Duotone image (the document "
                   "is %s)",
                   pc_color_mode_name(mode));
}

/* Native colour planes of the flattened image (opaque over white): one array of `n` floats per
 * colour channel of the document's pixel format. A lone plain layer is read exactly; anything else
 * goes through the compositor. *out is `cc` owned float arrays; free with mc_free_planes. */
static pc_status mc_flattened_planes(const pc_document *doc, float ***out, size_t *out_cc, size_t *out_n,
                                     pc_error *err)
{
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t cc = pc_color_mode_channels(fmt.mode);
    pc_rect canvas = pc_document_bounds(doc);
    size_t n = (size_t)pc_rect_width(canvas) * (size_t)pc_rect_height(canvas);
    float white[PC_RASTER_MAX_CHANNELS];
    float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    (void)pc_from_rgba(fmt, one, white);

    float **planes = calloc(cc ? cc : 1, sizeof *planes);
    if (planes == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "multichannel planes");
    }
    for (size_t c = 0; c < cc; c++) {
        planes[c] = malloc(n ? n * sizeof(float) : 1);
        if (planes[c] == NULL) {
            for (size_t j = 0; j < c; j++) {
                free(planes[j]);
            }
            free(planes);
            return pc_fail(err, PC_ERR_NOMEM, "multichannel plane");
        }
    }

    const pc_surface *plain = NULL;
    if (doc->nlayers == 1) {
        const pc_layer *l = &doc->layers[0];
        if (l->visible && l->opacity >= 1.0f && l->fill_opacity >= 1.0f && !l->has_mask &&
            !l->has_vector_mask && !pc_compose_has_effects(l) && l->content.kind == PC_LAYER_RASTER) {
            const pc_surface *surf = &l->content.u.raster;
            if (pc_surface_format(surf).mode == fmt.mode) {
                plain = surf;
            }
        }
    }

    pc_status rc = PC_OK;
    if (plain != NULL) {
        size_t ch = pc_surface_channels(plain);
        bool alpha = pc_surface_format(plain).alpha;
        float *data = NULL;
        size_t len = 0;
        rc = pc_surface_read_region(plain, canvas, &data, &len, err);
        if (rc == PC_OK) {
            for (size_t i = 0; i < n; i++) {
                const float *px = &data[i * ch];
                float a = alpha ? px[ch - 1] : 1.0f;
                for (size_t c = 0; c < cc; c++) {
                    planes[c][i] = px[c] * a + white[c] * (1.0f - a);
                }
            }
            free(data);
        }
    } else {
        pc_compose_buffer buf;
        rc = pc_compose_flatten(doc, &buf, err);
        if (rc == PC_OK) {
            float bg[3] = {1.0f, 1.0f, 1.0f};
            pc_compose_buffer_over_background(&buf, bg);
            for (size_t i = 0; i < n && i < buf.len; i++) {
                float v[PC_RASTER_MAX_CHANNELS];
                (void)pc_from_rgba(fmt, buf.px[i], v);
                for (size_t c = 0; c < cc; c++) {
                    planes[c][i] = v[c];
                }
            }
            pc_compose_buffer_fini(&buf);
        }
    }
    if (rc != PC_OK) {
        for (size_t c = 0; c < cc; c++) {
            free(planes[c]);
        }
        free(planes);
        return rc;
    }
    *out = planes;
    *out_cc = cc;
    *out_n = n;
    return PC_OK;
}

static void mc_free_planes(float **planes, size_t cc)
{
    if (planes == NULL) {
        return;
    }
    for (size_t c = 0; c < cc; c++) {
        free(planes[c]);
    }
    free(planes);
}

/* A grayscale channel surface at the document's depth from `n` density values (clamped 0..1). */
static pc_status mc_channel_surface(const pc_document *doc, const float *vals, size_t n, pc_surface *out,
                                    pc_error *err)
{
    pc_pixel_format f = {PC_COLOR_MODE_GRAYSCALE, doc->depth, false};
    pc_surface s;
    pc_surface_init(&s, f);
    pc_rect canvas = pc_document_bounds(doc);
    float *clamped = malloc(n ? n * sizeof(float) : 1);
    if (clamped == NULL) {
        pc_surface_fini(&s);
        return pc_fail(err, PC_ERR_NOMEM, "channel surface");
    }
    for (size_t i = 0; i < n; i++) {
        float v = vals[i];
        clamped[i] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
    pc_status rc = pc_surface_write_region(&s, canvas, clamped, n, err);
    free(clamped);
    if (rc != PC_OK) {
        pc_surface_fini(&s);
        return rc;
    }
    pc_surface_prune(&s);
    *out = s;
    return PC_OK;
}

/* One ink of the flattened document: name (owned), display colour, density values (owned, `n`). */
struct mc_ink {
    char *name;
    pc_color color;
    float *vals;
};

static void mc_inks_free(struct mc_ink *inks, size_t count)
{
    if (inks == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(inks[i].name);
        free(inks[i].vals);
    }
    free(inks);
}

/* vals = inv ? 1-plane : plane (an owned copy). */
static float *mc_plane_copy(const float *plane, size_t n, bool inv)
{
    float *v = malloc(n ? n * sizeof(float) : 1);
    if (v == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        v[i] = inv ? 1.0f - plane[i] : plane[i];
    }
    return v;
}

/* The ink channels (name, display colour, densities) of the flattened document
 * (multichannel_cmds::inks_of). */
static pc_status mc_inks_of(const pc_document *doc, float **planes, size_t cc, size_t n,
                            struct mc_ink **out, size_t *out_count, pc_error *err)
{
    static const char *const PROCESS[] = {"Cyan", "Magenta", "Yellow", "Black"};
    const pc_color process_colors[] = {MC_CYAN, MC_MAGENTA, MC_YELLOW, MC_BLACK};
    struct mc_ink *inks = NULL;
    size_t count = 0;

    if (doc->mode == PC_COLOR_MODE_RGB || doc->mode == PC_COLOR_MODE_CMYK) {
        count = doc->mode == PC_COLOR_MODE_RGB ? 3 : 4;
        bool inv = doc->mode == PC_COLOR_MODE_RGB;
        inks = calloc(count, sizeof *inks);
        if (inks == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "inks");
        }
        for (size_t k = 0; k < count; k++) {
            inks[k].name = pc_strndup(PROCESS[k], strlen(PROCESS[k]));
            inks[k].color = process_colors[k];
            inks[k].vals = (k < cc) ? mc_plane_copy(planes[k], n, inv) : mc_plane_copy(planes[0], n, inv);
            if (inks[k].name == NULL || inks[k].vals == NULL) {
                mc_inks_free(inks, count);
                return pc_fail(err, PC_ERR_NOMEM, "inks");
            }
        }
    } else if (doc->mode == PC_COLOR_MODE_LAB) {
        count = cc;
        inks = calloc(count ? count : 1, sizeof *inks);
        if (inks == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "inks");
        }
        for (size_t k = 0; k < count; k++) {
            char buf[32];
            snprintf(buf, sizeof buf, "Alpha %zu", k + 1);
            inks[k].name = pc_strndup(buf, strlen(buf));
            inks[k].color = pc_color_rgb(0.0f, 0.0f, 0.0f);
            inks[k].vals = mc_plane_copy(planes[k], n, true);
            if (inks[k].name == NULL || inks[k].vals == NULL) {
                mc_inks_free(inks, count);
                return pc_fail(err, PC_ERR_NOMEM, "inks");
            }
        }
    } else if (doc->mode == PC_COLOR_MODE_DUOTONE && doc->has_duotone && doc->duotone.ninks > 0) {
        count = doc->duotone.ninks;
        inks = calloc(count, sizeof *inks);
        if (inks == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "inks");
        }
        for (size_t k = 0; k < count; k++) {
            const pc_duotone_ink *ink = &doc->duotone.inks[k];
            inks[k].name = pc_strndup(pc_str_c(&ink->name), strlen(pc_str_c(&ink->name)));
            inks[k].color = pc_color_rgb(ink->color[0], ink->color[1], ink->color[2]);
            inks[k].vals = malloc(n ? n * sizeof(float) : 1);
            if (inks[k].name == NULL || inks[k].vals == NULL) {
                mc_inks_free(inks, count);
                return pc_fail(err, PC_ERR_NOMEM, "inks");
            }
            for (size_t i = 0; i < n; i++) {
                inks[k].vals[i] = pc_duotone_ink_density(ink, 1.0f - planes[0][i]);
            }
        }
    } else {
        count = 1;
        inks = calloc(1, sizeof *inks);
        if (inks == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "inks");
        }
        inks[0].name = pc_strndup("Black", 5);
        inks[0].color = pc_color_rgb(0.0f, 0.0f, 0.0f);
        inks[0].vals = mc_plane_copy(planes[0], n, true);
        if (inks[0].name == NULL || inks[0].vals == NULL) {
            mc_inks_free(inks, count);
            return pc_fail(err, PC_ERR_NOMEM, "inks");
        }
    }
    *out = inks;
    *out_count = count;
    return PC_OK;
}

/* Clear every layer of a document (Vec::clear). */
static void mc_clear_layers(pc_document *doc)
{
    for (size_t i = 0; i < doc->nlayers; i++) {
        pc_layer_fini(&doc->layers[i]);
    }
    free(doc->layers);
    doc->layers = NULL;
    doc->nlayers = 0;
}

/* Context carrying the ink names out of the undoable edit. */
struct mc_names {
    char **names;
    size_t count;
};

static pc_status edit_to_multichannel(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                      pc_error *err)
{
    struct mc_names *result = ctx;
    float **planes = NULL;
    size_t cc = 0, n = 0;
    struct mc_ink *inks = NULL;
    size_t count = 0;
    pc_alpha_channel *newch = NULL;
    pc_status rc = mc_flattened_planes(doc, &planes, &cc, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = mc_inks_of(doc, planes, cc, n, &inks, &count, err);
    if (rc != PC_OK) {
        goto cleanup_planes;
    }

    pc_pixel_format gray = {PC_COLOR_MODE_GRAYSCALE, doc->depth, false};
    size_t newcap = count + doc->nchannels;
    newch = calloc(newcap ? newcap : 1, sizeof *newch);
    if (newch == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "multichannel channels");
        goto cleanup_inks;
    }
    size_t built = 0;
    for (size_t k = 0; k < count; k++) {
        pc_surface surf;
        rc = mc_channel_surface(doc, inks[k].vals, n, &surf, err);
        if (rc != PC_OK) {
            goto cleanup_newch;
        }
        rc = pc_alpha_channel_init(&newch[k], inks[k].name, &surf, err);
        if (rc != PC_OK) {
            pc_surface_fini(&surf);
            goto cleanup_newch;
        }
        newch[k].has_spot = true;
        newch[k].spot_color = inks[k].color;
        newch[k].spot_solidity = 0.0f;
        built++;
    }
    /* Existing alpha / spot channels follow the inks, converted to the document depth (cloned so
     * the original channels stay intact until the edit commits). */
    for (size_t j = 0; j < doc->nchannels; j++) {
        rc = pc_alpha_channel_clone(&doc->channels[j], &newch[count + j], err);
        if (rc != PC_OK) {
            goto cleanup_newch;
        }
        built++;
        if (!pc_pixel_format_eq(pc_surface_format(&newch[count + j].surface), gray)) {
            pc_surface conv;
            rc = pc_surface_convert(&newch[count + j].surface, gray, &conv, err);
            if (rc != PC_OK) {
                goto cleanup_newch;
            }
            pc_surface_fini(&newch[count + j].surface);
            newch[count + j].surface = conv;
        }
    }

    /* Collect the ink names (ownership moved out of inks) before freeing them. */
    result->names = calloc(count ? count : 1, sizeof(char *));
    if (result->names == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "multichannel names");
        goto cleanup_newch;
    }
    for (size_t k = 0; k < count; k++) {
        result->names[k] = inks[k].name;
        inks[k].name = NULL;
    }
    result->count = count;

    /* Commit: replace channels, drop layers and the now-meaningless colour state. */
    for (size_t j = 0; j < doc->nchannels; j++) {
        pc_alpha_channel_fini(&doc->channels[j]);
    }
    free(doc->channels);
    doc->channels = newch;
    doc->nchannels = newcap;
    newch = NULL;

    mc_clear_layers(doc);
    *has_active = false;
    (void)active;
    doc->mode = PC_COLOR_MODE_MULTICHANNEL;
    if (doc->has_quick_mask) {
        pc_alpha_channel_fini(&doc->quick_mask);
        doc->has_quick_mask = false;
    }
    if (doc->has_color_table) {
        pc_color_table_fini(&doc->color_table);
        doc->has_color_table = false;
    }
    if (doc->has_duotone) {
        pc_duotone_fini(&doc->duotone);
        doc->has_duotone = false;
    }
    pc_blob_release(doc->icc_profile);
    doc->icc_profile = NULL;

    mc_inks_free(inks, count);
    mc_free_planes(planes, cc);
    return PC_OK;

cleanup_newch:
    for (size_t k = 0; k < built; k++) {
        pc_alpha_channel_fini(&newch[k]);
    }
    free(newch);
cleanup_inks:
    mc_inks_free(inks, count);
cleanup_planes:
    mc_free_planes(planes, cc);
    return rc;
}

static pc_status run_to_multichannel(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    struct mc_names result = {NULL, 0};
    pc_status rc = pc_engine_session_edit(s, "Multichannel", edit_to_multichannel, &result, err);
    if (rc != PC_OK) {
        if (result.names != NULL) {
            for (size_t i = 0; i < result.count; i++) {
                free(result.names[i]);
            }
            free(result.names);
        }
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        for (size_t i = 0; i < result.count; i++) {
            free(result.names[i]);
        }
        free(result.names);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "mode", "multichannel");
    yyjson_mut_val *names = yyjson_mut_arr(d);
    for (size_t i = 0; i < result.count; i++) {
        yyjson_mut_arr_add_strcpy(d, names, result.names[i]);
        free(result.names[i]);
    }
    free(result.names);
    yyjson_mut_obj_add_val(d, root, "channels", names);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* The lower-case name of a mode, as Rust's `format!("{mode:?}").to_lowercase()` prints it. */
static const char *mc_mode_lower(pc_color_mode m)
{
    switch (m) {
    case PC_COLOR_MODE_RGB:
        return "rgb";
    case PC_COLOR_MODE_CMYK:
        return "cmyk";
    case PC_COLOR_MODE_LAB:
        return "lab";
    case PC_COLOR_MODE_GRAYSCALE:
        return "grayscale";
    default:
        return "multichannel";
    }
}

/* ---- convert OUT of Multichannel (called by color_cmds::convert_mode, E8) ------------------- */

struct mc_from_ctx {
    pc_color_mode mode;
    const char *how; /* "channels" or "appearance" (static) */
};

static pc_status edit_convert_from(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                   pc_error *err)
{
    struct mc_from_ctx *fc = ctx;
    pc_color_mode mode = fc->mode;
    pc_rect canvas = pc_document_bounds(doc);
    size_t n = (size_t)pc_rect_width(canvas) * (size_t)pc_rect_height(canvas);

    /* Partition channels into inks (spot) and others, keeping order. */
    pc_alpha_channel *inks = calloc(doc->nchannels ? doc->nchannels : 1, sizeof *inks);
    pc_alpha_channel *others = calloc(doc->nchannels ? doc->nchannels : 1, sizeof *others);
    if (inks == NULL || others == NULL) {
        free(inks);
        free(others);
        return pc_fail(err, PC_ERR_NOMEM, "mode change");
    }
    size_t nink = 0, noth = 0;
    for (size_t j = 0; j < doc->nchannels; j++) {
        if (doc->channels[j].has_spot) {
            inks[nink++] = doc->channels[j];
        } else {
            others[noth++] = doc->channels[j];
        }
    }
    /* The original container is consumed: its elements now live in inks[]/others[]. */
    free(doc->channels);
    doc->channels = NULL;
    doc->nchannels = 0;

    bool reinterpret = ((mode == PC_COLOR_MODE_RGB || mode == PC_COLOR_MODE_LAB) && nink == 3) ||
                       (mode == PC_COLOR_MODE_CMYK && nink == 4) ||
                       (mode == PC_COLOR_MODE_GRAYSCALE && nink == 1);

    pc_status rc = PC_OK;
    pc_compose_buffer shown = {0};
    bool have_shown = false;
    if (!reinterpret) {
        /* The printed look, rendered before the inks leave the document. */
        pc_document d2;
        rc = pc_document_clone(doc, &d2, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        /* d2.channels <- clones of the inks. */
        free(d2.channels);
        d2.channels = calloc(nink ? nink : 1, sizeof(pc_alpha_channel));
        if (d2.channels == NULL) {
            d2.nchannels = 0;
            pc_document_fini(&d2);
            rc = pc_fail(err, PC_ERR_NOMEM, "mode change");
            goto cleanup;
        }
        d2.nchannels = 0;
        for (size_t k = 0; k < nink; k++) {
            rc = pc_alpha_channel_clone(&inks[k], &d2.channels[k], err);
            if (rc != PC_OK) {
                pc_document_fini(&d2);
                goto cleanup;
            }
            d2.nchannels++;
        }
        rc = pc_compose_flatten(&d2, &shown, err);
        pc_document_fini(&d2);
        if (rc != PC_OK) {
            goto cleanup;
        }
        have_shown = true;
    }

    doc->mode = mode;
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    size_t cc = pc_color_mode_channels(fmt.mode);
    size_t stride = cc + 1;
    float *data = calloc(n ? n * stride : 1, sizeof(float));
    if (data == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "mode change");
        goto cleanup;
    }
    if (have_shown) {
        for (size_t i = 0; i < n && i < shown.len; i++) {
            float v[PC_RASTER_MAX_CHANNELS];
            float rgba[4] = {shown.px[i][0], shown.px[i][1], shown.px[i][2], 1.0f};
            (void)pc_from_rgba(fmt, rgba, v);
            for (size_t c = 0; c < stride; c++) {
                data[i * stride + c] = v[c];
            }
        }
    } else {
        for (size_t k = 0; k < nink; k++) {
            float *vals = NULL;
            size_t len = 0;
            rc = pc_surface_read_region(&inks[k].surface, canvas, &vals, &len, err);
            if (rc != PC_OK) {
                free(data);
                goto cleanup;
            }
            size_t step = pc_surface_channels(&inks[k].surface);
            for (size_t i = 0; i < n; i++) {
                float v = vals[i * step];
                data[i * stride + k] = (mode == PC_COLOR_MODE_CMYK) ? v : 1.0f - v;
            }
            free(vals);
        }
        for (size_t i = 0; i < n; i++) {
            data[i * stride + cc] = 1.0f;
        }
    }

    pc_surface surf;
    pc_surface_init(&surf, fmt);
    rc = pc_surface_write_region(&surf, canvas, data, n * stride, err);
    free(data);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        goto cleanup;
    }
    pc_surface_prune(&surf);

    pc_layer_content content = {0};
    content.kind = PC_LAYER_RASTER;
    content.u.raster = surf;
    pc_layer bg;
    rc = pc_layer_init(&bg, "Background", &content, err);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        goto cleanup;
    }
    bg.locks.transparency = true;
    bg.locks.position = true;
    pc_layer_id bg_id = bg.id;

    mc_clear_layers(doc);
    doc->layers = malloc(sizeof(pc_layer));
    if (doc->layers == NULL) {
        pc_layer_fini(&bg);
        rc = pc_fail(err, PC_ERR_NOMEM, "mode change");
        goto cleanup;
    }
    doc->layers[0] = bg;
    doc->nlayers = 1;
    *has_active = true;
    *active = bg_id;

    /* The non-ink channels stay; the inks are consumed. */
    doc->channels = calloc(noth ? noth : 1, sizeof(pc_alpha_channel));
    if (doc->channels == NULL && noth > 0) {
        rc = pc_fail(err, PC_ERR_NOMEM, "mode change");
        goto cleanup;
    }
    for (size_t j = 0; j < noth; j++) {
        doc->channels[j] = others[j];
    }
    doc->nchannels = noth;

    fc->how = reinterpret ? "channels" : "appearance";
    if (have_shown) {
        pc_compose_buffer_fini(&shown);
    }
    for (size_t k = 0; k < nink; k++) {
        pc_alpha_channel_fini(&inks[k]);
    }
    /* The `others` elements are now owned by the document; free only the two scratch arrays (no
     * failure path follows, so the cleanup label is never reached from here). */
    free(inks);
    free(others);
    return PC_OK;

cleanup:
    if (have_shown) {
        pc_compose_buffer_fini(&shown);
    }
    for (size_t k = 0; k < nink; k++) {
        pc_alpha_channel_fini(&inks[k]);
    }
    for (size_t j = 0; j < noth; j++) {
        pc_alpha_channel_fini(&others[j]);
    }
    free(inks);
    free(others);
    return rc;
}

pc_status pc_engine_multichannel_convert_from(pc_engine_session *s, pc_color_mode mode, yyjson_val *p,
                                              pc_json_value *out, pc_error *err)
{
    (void)p;
    if (!(mode == PC_COLOR_MODE_RGB || mode == PC_COLOR_MODE_CMYK || mode == PC_COLOR_MODE_LAB ||
          mode == PC_COLOR_MODE_GRAYSCALE)) {
        return pc_engine_err_other(err, "a Multichannel document can't be converted to %s",
                                   pc_color_mode_name(mode));
    }
    struct mc_from_ctx fc = {mode, "appearance"};
    pc_status rc = pc_engine_session_edit(s, "Mode Change", edit_convert_from, &fc, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "mode", mc_mode_lower(mode));
    yyjson_mut_obj_add_str(d, root, "from", "multichannel");
    yyjson_mut_obj_add_str(d, root, "converted", fc.how);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

pc_status pc_engine_multichannel_ink_names(const pc_document *doc, char ***out, size_t *n, pc_error *err)
{
    size_t count = 0;
    for (size_t j = 0; j < doc->nchannels; j++) {
        if (doc->channels[j].has_spot) {
            count++;
        }
    }
    char **names = calloc(count ? count : 1, sizeof(char *));
    if (names == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "ink names");
    }
    size_t k = 0;
    for (size_t j = 0; j < doc->nchannels; j++) {
        if (doc->channels[j].has_spot) {
            const char *nm = pc_str_c(&doc->channels[j].name);
            names[k] = pc_strndup(nm, strlen(nm));
            if (names[k] == NULL) {
                for (size_t i = 0; i < k; i++) {
                    free(names[i]);
                }
                free(names);
                return pc_fail(err, PC_ERR_NOMEM, "ink names");
            }
            k++;
        }
    }
    *out = names;
    *n = count;
    return PC_OK;
}

static const char *const MENU_IMAGE_MODE[] = {"Image", "Mode"};

const pc_engine_command *pc_engine_multichannel_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"image.mode.multichannel", "Multichannel", MENU_IMAGE_MODE, 2, NULL,
         "{} (flattens; RGB \xe2\x86\x92 Cyan/Magenta/Yellow, CMYK \xe2\x86\x92 "
         "Cyan/Magenta/Yellow/Black, Lab \xe2\x86\x92 Alpha 1\xe2\x80\x93"
         "3, Grayscale \xe2\x86\x92 Black, Duotone \xe2\x86\x92 its inks)",
         mc_enabled, run_to_multichannel, true},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
