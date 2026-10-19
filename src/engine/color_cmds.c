/* Colour-management state, conversion primitives and commands (port of
 * crates/engine/src/color_cmds.rs, E8).
 *
 * Provides the Session-wide colour state (pc_color_state), the Image > Mode / Convert to Profile
 * conversion engine (pc_engine_convert_document, pc_engine_color_convert_mode -- the E3/E4
 * unblockers), the document/working/composite profile resolvers, and the Edit > Assign/Convert/
 * Color Settings and View > Proof Setup/Proof Colors/Gamut Warning commands.
 *
 * Never crashes on any params/document/profile state (REWRITE_PLAN.md rule 9): untrusted specs,
 * sizes and profile bytes are validated; conversions that fail leave the document unchanged.
 *
 * Deferred (see color_cmds.h): the colour-managed canvas display path (display_color.rs /
 * proof_sim.rs). The commands report a headless monitor (sRGB), matching the reference with no
 * platform display reader. */
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "display_color.h"
#include "base/file.h"
#include "base/mem.h"
#include "base/str.h"
#include "cms/builtin.h"
#include "cms/gamut.h"
#include "cms/transform.h"
#include "color/convert.h"
#include "compose/compose.h"
#include "compose/psblend.h"
#include "doc/effects.h"
#include "doc/fill.h"
#include "doc/mode.h"
#include "doc/text.h"
#include "doc/vector.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "color_cmds.h"
#include "display_color.h"
#include "engine_internal.h"

/* ============================================================== colour space helpers */

static uint32_t cs_space(pc_color_mode mode)
{
    switch (mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        return PC_CS_GRAY;
    case PC_COLOR_MODE_CMYK:
        return PC_CS_CMYK;
    case PC_COLOR_MODE_LAB:
        return PC_CS_LAB;
    default:
        return PC_CS_RGB;
    }
}

static bool space_mode(uint32_t cs, pc_color_mode *out)
{
    switch (cs) {
    case PC_CS_RGB:
        *out = PC_COLOR_MODE_RGB;
        return true;
    case PC_CS_GRAY:
        *out = PC_COLOR_MODE_GRAYSCALE;
        return true;
    case PC_CS_CMYK:
        *out = PC_COLOR_MODE_CMYK;
        return true;
    case PC_CS_LAB:
        *out = PC_COLOR_MODE_LAB;
        return true;
    default:
        return false;
    }
}

/* Rust `format!("{:?}", ColorSpace)`. */
static const char *cs_debug_name(uint32_t cs)
{
    switch (cs) {
    case PC_CS_RGB:
        return "Rgb";
    case PC_CS_GRAY:
        return "Gray";
    case PC_CS_CMYK:
        return "Cmyk";
    case PC_CS_LAB:
        return "Lab";
    case PC_CS_XYZ:
        return "Xyz";
    case PC_CS_LUV:
        return "Luv";
    case PC_CS_YCBCR:
        return "YCbCr";
    case PC_CS_YXY:
        return "Yxy";
    case PC_CS_HSV:
        return "Hsv";
    case PC_CS_HLS:
        return "Hls";
    default:
        return "Other";
    }
}

static size_t mode_color_channels(pc_color_mode mode)
{
    return pc_color_mode_channels(mode);
}

const pc_profile *pc_engine_working_profile(pc_color_mode mode)
{
    const pc_profile *p = pc_builtin_default_for(cs_space(mode));
    return p != NULL ? p : pc_builtin_profile(PC_BUILTIN_SRGB);
}

static char *dupstr(const char *s)
{
    return pc_strndup(s, strlen(s));
}

/* ASCII case-insensitive suffix match. */
static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) {
        return false;
    }
    const char *a = s + (n - m);
    for (size_t i = 0; i < m; i++) {
        char x = a[i], y = suffix[i];
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x - 'A' + 'a');
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y - 'A' + 'a');
        }
        if (x != y) {
            return false;
        }
    }
    return true;
}

/* Parse ICC bytes into *out (owned); *ok = whether it parsed. Never crashes. */
static void profile_from_blob(const pc_blob *b, pc_profile *out, bool *ok)
{
    *ok = false;
    const uint8_t *data = pc_blob_data(b);
    size_t len = pc_blob_len(b);
    if (data == NULL || len == 0) {
        return;
    }
    if (pc_profile_parse(data, len, out, NULL) == PC_OK) {
        *ok = true;
    }
}

pc_status pc_engine_document_profile(const pc_document *doc, pc_profile *out, pc_error *err)
{
    uint32_t space = cs_space(doc->mode);
    if (doc->icc_profile != NULL) {
        pc_profile emb;
        bool ok = false;
        profile_from_blob(doc->icc_profile, &emb, &ok);
        if (ok) {
            if (emb.space == space) {
                *out = emb;
                return PC_OK;
            }
            pc_profile_fini(&emb);
        }
    }
    return pc_profile_clone(pc_engine_working_profile(doc->mode), out, err);
}

pc_status pc_engine_composite_profile(const pc_document *doc, pc_profile *out, pc_error *err)
{
    uint32_t space = cs_space(doc->mode);
    if (space == PC_CS_RGB) {
        return pc_engine_document_profile(doc, out, err);
    }
    if (space == PC_CS_GRAY) {
        pc_profile dp;
        if (pc_engine_document_profile(doc, &dp, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        bool ok = false;
        pc_status rc = pc_profile_gray_as_rgb(&dp, out, &ok, NULL);
        pc_profile_fini(&dp);
        if (rc == PC_OK && ok) {
            return PC_OK;
        }
        bool ok2 = false;
        if (pc_profile_gray_as_rgb(pc_builtin_profile(PC_BUILTIN_SGRAY), out, &ok2, NULL) == PC_OK && ok2) {
            return PC_OK;
        }
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    }
    return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
}

/* Can `p` be both source and destination for `intent`? */
static bool rgb_usable(const pc_profile *p, pc_intent intent)
{
    pc_cms_stages st;
    pc_pcs pcs;
    memset(&st, 0, sizeof st);
    if (pc_profile_pcs_to_device(p, intent, &st, &pcs, NULL) != PC_OK) {
        return false;
    }
    pc_cms_stages_fini(&st);
    memset(&st, 0, sizeof st);
    if (pc_profile_device_to_pcs(p, intent, &st, &pcs, NULL) != PC_OK) {
        return false;
    }
    pc_cms_stages_fini(&st);
    return true;
}

/* Resolve a non-"working*" spec: built-in id/description, "document", or an .icc/.icm path. */
static pc_status resolve_fixed(const char *spec, const pc_document *doc, pc_profile *out, pc_error *err)
{
    if (strcmp(spec, "document") == 0) {
        if (doc == NULL) {
            return pc_engine_err_no_document(err);
        }
        return pc_engine_document_profile(doc, out, err);
    }
    pc_builtin b;
    if (pc_builtin_from_id(spec, strlen(spec), &b)) {
        return pc_profile_clone(pc_builtin_profile(b), out, err);
    }
    /* §7: the reference has a Builtin::DotGain20Compat (Photoshop's default working gray, gamma
     * 1.732); the ported cms (F3/F4) does not expose it. So the default working-gray spec
     * "dot-gain-20-compat" (and its aliases) would be unresolvable and reject the default Color
     * Settings. Map it to the cms gray default (sGray) so the settings stay valid and resolvable;
     * the gray tone curve then differs from Photoshop's by design until cms adds the profile.
     * Owner: cms (F3/F4) / coordinator. */
    if (ends_with_ci(spec, "dot-gain-20-compat") || ends_with_ci(spec, "dotgain20") ||
        ends_with_ci(spec, "dotgain20compat") || ends_with_ci(spec, "dotgain")) {
        const pc_profile *g = pc_builtin_default_for(PC_CS_GRAY);
        return pc_profile_clone(g != NULL ? g : pc_builtin_profile(PC_BUILTIN_SGRAY), out, err);
    }
    bool is_path = ends_with_ci(spec, ".icc") || ends_with_ci(spec, ".icm") || strchr(spec, '/') != NULL ||
                   strchr(spec, '\\') != NULL;
    if (is_path) {
        pc_buf bytes = PC_BUF_INIT;
        if (pc_file_read(spec, PC_MAX_ALLOC, &bytes, NULL) != PC_OK) {
            return pc_engine_err_other(err, "cannot read profile `%s`", spec);
        }
        pc_status rc = pc_profile_parse(bytes.data, bytes.len, out, NULL);
        pc_buf_fini(&bytes);
        if (rc != PC_OK) {
            return pc_engine_err_other(err, "colour management: cannot parse profile `%s`", spec);
        }
        return PC_OK;
    }
    return pc_engine_err_other(
        err,
        "unknown profile `%s` (built-ins: srgb, display-p3, adobe-rgb-compat, prophoto-compat, "
        "linear-srgb, rec2020, gray-gamma-2.2, sgray, lab-d50, coated-cmyk, or a path to an .icc file)",
        spec);
}

static pc_status color_working(const pc_color_state *cs, pc_color_mode mode, pc_profile *out, pc_error *err)
{
    uint32_t space = cs_space(mode);
    const char *spec = NULL;
    if (space == PC_CS_RGB) {
        spec = cs->settings.working_rgb;
    } else if (space == PC_CS_CMYK) {
        spec = cs->settings.working_cmyk;
    } else if (space == PC_CS_GRAY) {
        spec = cs->settings.working_gray;
    }
    if (spec != NULL && spec[0] != '\0') {
        pc_profile p;
        memset(&p, 0, sizeof p);
        if (resolve_fixed(spec, NULL, &p, NULL) == PC_OK) {
            bool good = p.space == space &&
                        (space != PC_CS_RGB || rgb_usable(&p, pc_color_settings_intent(&cs->settings)));
            if (good) {
                *out = p;
                return PC_OK;
            }
            pc_profile_fini(&p);
        }
    }
    return pc_profile_clone(pc_engine_working_profile(mode), out, err);
}

pc_status pc_engine_resolve_profile(const pc_color_state *cs, const char *spec, const pc_document *doc,
                                    bool has_mode, pc_color_mode mode, pc_profile *out, pc_error *err)
{
    if (strcmp(spec, "working") == 0 || strcmp(spec, "default") == 0) {
        pc_color_mode m = has_mode ? mode : (doc != NULL ? doc->mode : PC_COLOR_MODE_RGB);
        return color_working(cs, m, out, err);
    }
    if (strcmp(spec, "working-cmyk") == 0 || strcmp(spec, "workingCmyk") == 0) {
        return color_working(cs, PC_COLOR_MODE_CMYK, out, err);
    }
    if (strcmp(spec, "working-rgb") == 0 || strcmp(spec, "workingRgb") == 0) {
        return color_working(cs, PC_COLOR_MODE_RGB, out, err);
    }
    if (strcmp(spec, "working-gray") == 0 || strcmp(spec, "workingGray") == 0) {
        return color_working(cs, PC_COLOR_MODE_GRAYSCALE, out, err);
    }
    return resolve_fixed(spec, doc, out, err);
}

/* ============================================================== ColorSettings */

const char *pc_color_policy_id(pc_color_policy p)
{
    switch (p) {
    case PC_COLOR_POLICY_CONVERT:
        return "convert";
    case PC_COLOR_POLICY_OFF:
        return "off";
    default:
        return "preserve";
    }
}

bool pc_color_policy_parse(const char *s, pc_color_policy *out)
{
    if (strcmp(s, "preserve") == 0 || strcmp(s, "preserveEmbedded") == 0) {
        *out = PC_COLOR_POLICY_PRESERVE;
    } else if (strcmp(s, "convert") == 0 || strcmp(s, "convertToWorking") == 0) {
        *out = PC_COLOR_POLICY_CONVERT;
    } else if (strcmp(s, "off") == 0) {
        *out = PC_COLOR_POLICY_OFF;
    } else {
        return false;
    }
    return true;
}

pc_status pc_color_settings_default(pc_color_settings *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->working_rgb = dupstr(pc_builtin_id(PC_BUILTIN_SRGB));
    out->working_cmyk = dupstr(pc_builtin_id(PC_BUILTIN_COATED_CMYK));
    /* Photoshop's default working gray is Dot Gain 20%. The ported cms exposes no such built-in
     * (§7): "dot-gain-20-compat" falls back to the gray default (sGray) through color_working. */
    out->working_gray = dupstr("dot-gain-20-compat");
    out->policy_rgb = PC_COLOR_POLICY_PRESERVE;
    out->policy_cmyk = PC_COLOR_POLICY_PRESERVE;
    out->policy_gray = PC_COLOR_POLICY_PRESERVE;
    out->ask_on_mismatch = true;
    out->ask_on_paste = true;
    out->ask_on_missing = false;
    out->intent = dupstr(pc_intent_id(PC_INTENT_RELATIVE));
    out->bpc = true;
    out->dither = true;
    out->blend_text_gamma = PC_PSBLEND_TEXT_GAMMA;
    out->monitor_profile = dupstr("auto");
    if (out->working_rgb == NULL || out->working_cmyk == NULL || out->working_gray == NULL ||
        out->intent == NULL || out->monitor_profile == NULL) {
        pc_color_settings_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "color settings");
    }
    return PC_OK;
}

void pc_color_settings_fini(pc_color_settings *c)
{
    if (c == NULL) {
        return;
    }
    free(c->working_rgb);
    free(c->working_cmyk);
    free(c->working_gray);
    free(c->intent);
    free(c->monitor_profile);
    memset(c, 0, sizeof *c);
}

pc_status pc_color_settings_clone(const pc_color_settings *c, pc_color_settings *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->working_rgb = dupstr(c->working_rgb ? c->working_rgb : "");
    out->working_cmyk = dupstr(c->working_cmyk ? c->working_cmyk : "");
    out->working_gray = dupstr(c->working_gray ? c->working_gray : "");
    out->intent = dupstr(c->intent ? c->intent : "");
    out->monitor_profile = dupstr(c->monitor_profile ? c->monitor_profile : "");
    out->policy_rgb = c->policy_rgb;
    out->policy_cmyk = c->policy_cmyk;
    out->policy_gray = c->policy_gray;
    out->ask_on_mismatch = c->ask_on_mismatch;
    out->ask_on_paste = c->ask_on_paste;
    out->ask_on_missing = c->ask_on_missing;
    out->bpc = c->bpc;
    out->dither = c->dither;
    out->blend_text_gamma = c->blend_text_gamma;
    if (out->working_rgb == NULL || out->working_cmyk == NULL || out->working_gray == NULL ||
        out->intent == NULL || out->monitor_profile == NULL) {
        pc_color_settings_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "color settings");
    }
    return PC_OK;
}

static bool streq0(const char *a, const char *b)
{
    return strcmp(a ? a : "", b ? b : "") == 0;
}

bool pc_color_settings_eq(const pc_color_settings *a, const pc_color_settings *b)
{
    return streq0(a->working_rgb, b->working_rgb) && streq0(a->working_cmyk, b->working_cmyk) &&
           streq0(a->working_gray, b->working_gray) && streq0(a->intent, b->intent) &&
           streq0(a->monitor_profile, b->monitor_profile) && a->policy_rgb == b->policy_rgb &&
           a->policy_cmyk == b->policy_cmyk && a->policy_gray == b->policy_gray &&
           a->ask_on_mismatch == b->ask_on_mismatch && a->ask_on_paste == b->ask_on_paste &&
           a->ask_on_missing == b->ask_on_missing && a->bpc == b->bpc && a->dither == b->dither &&
           a->blend_text_gamma == b->blend_text_gamma;
}

pc_intent pc_color_settings_intent(const pc_color_settings *c)
{
    pc_intent i;
    if (c->intent != NULL && pc_intent_parse(c->intent, strlen(c->intent), &i)) {
        return i;
    }
    return PC_INTENT_RELATIVE;
}

pc_status pc_color_settings_validate(const pc_color_settings *c, pc_error *err)
{
    struct {
        uint32_t space;
        const char *spec;
    } pairs[3] = {{PC_CS_RGB, c->working_rgb}, {PC_CS_CMYK, c->working_cmyk}, {PC_CS_GRAY, c->working_gray}};
    for (size_t k = 0; k < 3; k++) {
        pc_profile p;
        memset(&p, 0, sizeof p);
        if (resolve_fixed(pairs[k].spec ? pairs[k].spec : "", NULL, &p, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        uint32_t got = p.space;
        bool wrong_space = got != pairs[k].space;
        bool bad_rgb = false;
        if (!wrong_space && pairs[k].space == PC_CS_RGB) {
            bad_rgb = !rgb_usable(&p, pc_color_settings_intent(c));
        }
        pc_profile_fini(&p);
        if (wrong_space) {
            return pc_fail(err, PC_ERR_INVALID, "working space `%s` is %s, not %s", pairs[k].spec,
                           cs_debug_name(got), cs_debug_name(pairs[k].space));
        }
        if (bad_rgb) {
            return pc_fail(err, PC_ERR_INVALID, "working RGB space `%s` cannot be a destination for %s",
                           pairs[k].spec, pc_intent_id(pc_color_settings_intent(c)));
        }
    }
    const char *mon = c->monitor_profile ? c->monitor_profile : "";
    if (mon[0] != '\0' && strcmp(mon, "auto") != 0) {
        pc_profile p;
        memset(&p, 0, sizeof p);
        if (resolve_fixed(mon, NULL, &p, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        uint32_t sp = p.space;
        pc_profile_fini(&p);
        if (sp != PC_CS_RGB) {
            return pc_fail(err, PC_ERR_INVALID, "monitor profile `%s` is %s, not RGB", mon,
                           cs_debug_name(sp));
        }
    }
    pc_intent dummy;
    if (c->intent == NULL || !pc_intent_parse(c->intent, strlen(c->intent), &dummy)) {
        return pc_fail(err, PC_ERR_INVALID, "unknown intent `%s` (perceptual|relative|saturation|absolute)",
                       c->intent ? c->intent : "");
    }
    return PC_OK;
}

/* ============================================================== ColorState */

void pc_engine_color_init(pc_color_state *cs)
{
    memset(cs, 0, sizeof *cs);
    (void)pc_color_settings_default(&cs->settings, NULL);
}

static void proof_setup_fini(pc_proof_setup *s)
{
    if (s == NULL) {
        return;
    }
    free(s->name);
    pc_profile_fini(&s->profile);
    memset(s, 0, sizeof *s);
}

void pc_engine_color_fini(pc_color_state *cs)
{
    if (cs == NULL) {
        return;
    }
    pc_color_settings_fini(&cs->settings);
    for (size_t i = 0; i < cs->nproofs; i++) {
        proof_setup_fini(&cs->proofs[i].view.setup);
    }
    free(cs->proofs);
    free(cs->hdr);
    memset(cs, 0, sizeof *cs);
}

static pc_status proof_view_default(pc_proof_view *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->setup.name = dupstr("working-cmyk");
    if (out->setup.name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "proof");
    }
    if (pc_profile_clone(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), &out->setup.profile, err) != PC_OK) {
        free(out->setup.name);
        out->setup.name = NULL;
        return PC_ERR_NOMEM;
    }
    out->setup.intent = PC_INTENT_RELATIVE;
    out->setup.bpc = true;
    out->setup.simulate_paper = false;
    out->setup.kind = 0;
    out->enabled = false;
    out->gamut_warning = false;
    out->gamut_threshold = PC_GAMUT_DEFAULT_THRESHOLD;
    return PC_OK;
}

static pc_proof_view *color_proof_mut(pc_color_state *cs, pc_doc_id doc, pc_error *err)
{
    for (size_t i = 0; i < cs->nproofs; i++) {
        if (cs->proofs[i].doc == doc) {
            return &cs->proofs[i].view;
        }
    }
    pc_color_proof_entry *g = pc_grow_array(cs->proofs, &cs->cap_proofs, cs->nproofs + 1, sizeof *g);
    if (g == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "proof");
        return NULL;
    }
    cs->proofs = g;
    pc_color_proof_entry *e = &cs->proofs[cs->nproofs];
    e->doc = doc;
    if (proof_view_default(&e->view, err) != PC_OK) {
        return NULL;
    }
    cs->nproofs++;
    return &e->view;
}

/* ============================================================== conversion */

typedef struct color_cvt {
    pc_color_mode from;
    pc_color_mode to;
    const pc_transform *t;
} color_cvt;

static void convert_color(pc_color *c, const color_cvt *cv)
{
    if (c->mode != cv->from) {
        return;
    }
    size_t n = mode_color_channels(cv->from);
    float in[4] = {0, 0, 0, 0};
    for (size_t k = 0; k < n && k < 4; k++) {
        in[k] = c->c[k];
    }
    float o[16];
    memset(o, 0, sizeof o);
    pc_transform_eval(cv->t, in, o);
    size_t m = mode_color_channels(cv->to);
    float nc[4] = {0, 0, 0, 0};
    for (size_t d = 0; d < m && d < 4; d++) {
        float v = o[d];
        nc[d] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); /* clamp; NaN -> 0 */
    }
    c->mode = cv->to;
    c->c[0] = nc[0];
    c->c[1] = nc[1];
    c->c[2] = nc[2];
    c->c[3] = nc[3];
}

static void convert_fill(pc_fill *f, const color_cvt *cv)
{
    switch (f->kind) {
    case PC_FILL_SOLID:
        convert_color(&f->u.solid, cv);
        break;
    case PC_FILL_GRADIENT:
        for (size_t i = 0; i < f->u.gradient.nstops; i++) {
            convert_color(&f->u.gradient.stops[i].color, cv);
        }
        break;
    default:
        break;
    }
}

static void paint_colors(pc_fx_paint *p, const color_cvt *cv)
{
    switch (p->kind) {
    case PC_FX_PAINT_COLOR:
        convert_color(&p->u.color, cv);
        break;
    case PC_FX_PAINT_GRADIENT:
        for (size_t i = 0; i < p->u.gradient.nstops; i++) {
            convert_color(&p->u.gradient.stops[i].color, cv);
        }
        break;
    default:
        break;
    }
}

static void convert_layer_colors(pc_layer *l, const color_cvt *cv)
{
    for (size_t i = 0; i < l->effects.nitems; i++) {
        pc_effect *e = &l->effects.items[i];
        switch (e->kind) {
        case PC_FX_DROP_SHADOW:
        case PC_FX_INNER_SHADOW:
            convert_color(&e->u.shadow.color, cv);
            break;
        case PC_FX_OUTER_GLOW:
        case PC_FX_INNER_GLOW:
            paint_colors(&e->u.glow.paint, cv);
            break;
        case PC_FX_STROKE:
            paint_colors(&e->u.stroke.paint, cv);
            break;
        case PC_FX_COLOR_OVERLAY:
            convert_color(&e->u.color_overlay.color, cv);
            break;
        case PC_FX_GRADIENT_OVERLAY:
            for (size_t j = 0; j < e->u.gradient_overlay.gradient.nstops; j++) {
                convert_color(&e->u.gradient_overlay.gradient.stops[j].color, cv);
            }
            break;
        case PC_FX_SATIN:
            convert_color(&e->u.satin.color, cv);
            break;
        case PC_FX_BEVEL_EMBOSS:
            convert_color(&e->u.bevel.highlight_color, cv);
            convert_color(&e->u.bevel.shadow_color, cv);
            break;
        default:
            break;
        }
    }
    switch (l->content.kind) {
    case PC_LAYER_FILL:
        convert_fill(&l->content.u.fill, cv);
        break;
    case PC_LAYER_TEXT:
        convert_color(&l->content.u.text.color, cv);
        for (size_t r = 0; r < l->content.u.text.nruns; r++) {
            convert_color(&l->content.u.text.runs[r].style.color, cv);
        }
        break;
    case PC_LAYER_SHAPE:
        if (l->content.u.shape.has_fill) {
            convert_fill(&l->content.u.shape.fill, cv);
        }
        if (l->content.u.shape.has_stroke) {
            convert_fill(&l->content.u.shape.stroke.paint, cv);
        }
        break;
    case PC_LAYER_GROUP:
        for (size_t c = 0; c < l->content.u.group.nchildren; c++) {
            convert_layer_colors(&l->content.u.group.children[c], cv);
        }
        break;
    default:
        break;
    }
}

typedef void (*surface_cb)(pc_surface *surf, void *ctx);

static void for_each_surface(pc_layer *layers, size_t n, surface_cb cb, void *ctx)
{
    for (size_t i = 0; i < n; i++) {
        pc_layer *l = &layers[i];
        if (l->has_fill_cache) {
            cb(&l->fill_cache.surface, ctx);
        }
        switch (l->content.kind) {
        case PC_LAYER_RASTER:
            cb(&l->content.u.raster, ctx);
            break;
        case PC_LAYER_TEXT:
            if (l->content.u.text.has_cache) {
                cb(&l->content.u.text.cache, ctx);
            }
            break;
        case PC_LAYER_SHAPE:
            if (l->content.u.shape.has_cache) {
                cb(&l->content.u.shape.cache, ctx);
            }
            break;
        case PC_LAYER_SMART:
            if (l->content.u.smart.has_cache) {
                cb(&l->content.u.smart.cache, ctx);
            }
            break;
        case PC_LAYER_GROUP:
            for_each_surface(l->content.u.group.children, l->content.u.group.nchildren, cb, ctx);
            break;
        default:
            break;
        }
    }
}

static pc_sample_kind sample_kind(pc_sample_type s)
{
    switch (s) {
    case PC_SAMPLE_TYPE_U16:
        return PC_SAMPLE_U16;
    case PC_SAMPLE_TYPE_F32:
        return PC_SAMPLE_F32;
    default:
        return PC_SAMPLE_U8;
    }
}

/* Convert one surface's colour channels with `t` into `to` (keeping the source depth; alpha
 * copied). Falls back to the generic per-pixel convert when the channels don't line up. *out
 * initialised on success. §7: tile sharing (#4531) is not preserved (values identical; perf-only). */
static pc_status convert_surface(const pc_surface *s, pc_color_mode from, pc_pixel_format to,
                                 const pc_transform *t, pc_surface *out, pc_error *err)
{
    pc_pixel_format sf = pc_surface_format(s);
    pc_pixel_format tofmt = to;
    tofmt.sample = sf.sample; /* keep the source depth */
    size_t ss = pc_pixel_format_channels(sf);
    size_t ds = pc_pixel_format_channels(tofmt);
    if (sf.mode != from || pc_transform_inputs(t) != mode_color_channels(from) ||
        pc_transform_outputs(t) != mode_color_channels(to.mode) || sf.alpha != to.alpha) {
        return pc_surface_convert(s, to, out, err);
    }
    float dp[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(s, dp);
    float dpo[PC_RASTER_MAX_CHANNELS];
    memset(dpo, 0, sizeof dpo);
    if (pc_transform_convert_f32(t, dp, ss, ss, dpo, ds, ds, true, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_surface_init_default(out, tofmt, dpo, ds);

    pc_tile_entry *entries = NULL;
    size_t ntiles = 0;
    if (pc_surface_tiles_sorted(s, &entries, &ntiles, err) != PC_OK) {
        pc_surface_fini(out);
        return PC_ERR_NOMEM;
    }
    size_t tile_px = (size_t)PC_TILE_SIZE * (size_t)PC_TILE_SIZE;
    size_t sbytes = (sf.sample == PC_SAMPLE_TYPE_U8) ? 1 : (sf.sample == PC_SAMPLE_TYPE_U16 ? 2 : 4);
    pc_sample_kind kind = sample_kind(sf.sample);
    pc_status rc = PC_OK;
    for (size_t i = 0; i < ntiles; i++) {
        const uint8_t *src = pc_raster_tile_bytes(entries[i].tile);
        uint8_t *dst = NULL;
        if (pc_surface_tile_mut(out, entries[i].coord, &dst, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        pc_convert_job job = {src, tile_px * ss * sbytes, dst, tile_px * ds * sbytes};
        if (pc_transform_convert_bytes_many(t, kind, &job, 1, ss, ds, true, err) != PC_OK) {
            rc = PC_ERR_INVALID;
            break;
        }
    }
    pc_tile_entries_free(entries, ntiles);
    if (rc != PC_OK) {
        pc_surface_fini(out);
        return rc;
    }
    pc_surface_prune(out);
    return PC_OK;
}

/* The RGB surface of a gray surface's Duotone ink colours (same depth). */
static pc_status ink_surface(const pc_surface *s, const pc_duotone *d, pc_surface *out, pc_error *err)
{
    pc_pixel_format sf = pc_surface_format(s);
    if (sf.mode != PC_COLOR_MODE_GRAYSCALE) {
        return pc_surface_clone(s, out, err);
    }
    pc_pixel_format to = sf;
    to.mode = PC_COLOR_MODE_RGB;
    size_t ss = pc_pixel_format_channels(sf);
    size_t ds = pc_pixel_format_channels(to);
    size_t sbpp = pc_pixel_format_bytes_per_pixel(sf);
    size_t dbpp = pc_pixel_format_bytes_per_pixel(to);

    float dp[PC_RASTER_MAX_CHANNELS];
    pc_surface_default_pixel(s, dp);
    float dpo[PC_RASTER_MAX_CHANNELS];
    memset(dpo, 0, sizeof dpo);
    float rgb[3];
    pc_duotone_render(d, dp[0], rgb);
    dpo[0] = rgb[0];
    dpo[1] = rgb[1];
    dpo[2] = rgb[2];
    if (ss > 1 && ds > 3) {
        dpo[3] = dp[1];
    }
    pc_surface_init_default(out, to, dpo, ds);

    pc_tile_entry *entries = NULL;
    size_t ntiles = 0;
    if (pc_surface_tiles_sorted(s, &entries, &ntiles, err) != PC_OK) {
        pc_surface_fini(out);
        return PC_ERR_NOMEM;
    }
    size_t tile_px = (size_t)PC_TILE_SIZE * (size_t)PC_TILE_SIZE;
    pc_status rc = PC_OK;
    for (size_t i = 0; i < ntiles; i++) {
        const uint8_t *src = pc_raster_tile_bytes(entries[i].tile);
        uint8_t *dst = NULL;
        if (pc_surface_tile_mut(out, entries[i].coord, &dst, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            break;
        }
        for (size_t px = 0; px < tile_px; px++) {
            float g = pc_read_sample(src, tile_px * sbpp, sf.sample, px * ss);
            float orgb[3];
            pc_duotone_render(d, g, orgb);
            pc_write_sample(dst, tile_px * dbpp, to.sample, px * ds + 0, orgb[0]);
            pc_write_sample(dst, tile_px * dbpp, to.sample, px * ds + 1, orgb[1]);
            pc_write_sample(dst, tile_px * dbpp, to.sample, px * ds + 2, orgb[2]);
            if (ss > 1 && ds > 3) {
                pc_write_sample(dst, tile_px * dbpp, to.sample, px * ds + 3,
                                pc_read_sample(src, tile_px * sbpp, sf.sample, px * ss + 1));
            }
        }
    }
    pc_tile_entries_free(entries, ntiles);
    if (rc != PC_OK) {
        pc_surface_fini(out);
        return rc;
    }
    pc_surface_prune(out);
    return PC_OK;
}

typedef struct ink_ctx {
    const pc_duotone *d;
    pc_status rc;
} ink_ctx;

static void ink_cb(pc_surface *surf, void *vp)
{
    ink_ctx *ic = vp;
    if (ic->rc != PC_OK) {
        return;
    }
    pc_surface inked;
    pc_status rc = ink_surface(surf, ic->d, &inked, NULL);
    if (rc != PC_OK) {
        ic->rc = rc;
        return;
    }
    pc_surface_fini(surf);
    *surf = inked;
}

/* Turn a gray/duotone colour into its printed ink RGB. */
static void ink_color(pc_layer *l, const pc_duotone *d);
static void ink_color_fill(pc_fill *f, const pc_duotone *d)
{
    pc_color *c = NULL;
    if (f->kind == PC_FILL_SOLID) {
        c = &f->u.solid;
    }
    if (c != NULL && (c->mode == PC_COLOR_MODE_GRAYSCALE || c->mode == PC_COLOR_MODE_DUOTONE)) {
        float rgb[3];
        pc_duotone_render(d, c->c[0], rgb);
        float a = c->alpha;
        *c = pc_color_rgba(rgb[0], rgb[1], rgb[2], a);
    }
}

static void ink_color(pc_layer *l, const pc_duotone *d)
{
    if (l->content.kind == PC_LAYER_FILL) {
        ink_color_fill(&l->content.u.fill, d);
    } else if (l->content.kind == PC_LAYER_GROUP) {
        for (size_t i = 0; i < l->content.u.group.nchildren; i++) {
            ink_color(&l->content.u.group.children[i], d);
        }
    }
}

/* A Duotone document turned into the sRGB document it displays as. */
static pc_status ink_duotone(pc_document *doc, pc_error *err)
{
    if (doc->mode != PC_COLOR_MODE_DUOTONE || !doc->has_duotone || doc->duotone.ninks == 0) {
        return PC_OK;
    }
    /* Take the duotone out of the document (Rust `take_if`). */
    pc_duotone d = doc->duotone;
    doc->has_duotone = false;
    memset(&doc->duotone, 0, sizeof doc->duotone);

    ink_ctx ic = {&d, PC_OK};
    for_each_surface(doc->layers, doc->nlayers, ink_cb, &ic);
    if (ic.rc == PC_OK) {
        for (size_t i = 0; i < doc->nlayers; i++) {
            ink_color(&doc->layers[i], &d);
        }
        doc->mode = PC_COLOR_MODE_RGB;
        pc_buf bytes = PC_BUF_INIT;
        if (pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_SRGB), &bytes, NULL) == PC_OK) {
            pc_blob *blob = NULL;
            if (pc_blob_new(bytes.data, bytes.len, &blob, NULL) == PC_OK) {
                pc_blob_release(doc->icc_profile);
                doc->icc_profile = blob;
            }
        }
        pc_buf_fini(&bytes);
    }
    pc_duotone_fini(&d);
    return ic.rc != PC_OK ? pc_fail(err, PC_ERR_NOMEM, "duotone conversion") : PC_OK;
}

typedef struct conv_ctx {
    pc_color_mode from;
    pc_pixel_format fmt;
    const pc_transform *t;
    pc_status rc;
} conv_ctx;

static void conv_cb(pc_surface *surf, void *vp)
{
    conv_ctx *cc = vp;
    if (cc->rc != PC_OK) {
        return;
    }
    pc_pixel_format f = cc->fmt;
    f.alpha = pc_surface_format(surf).alpha;
    pc_surface converted;
    pc_status rc = convert_surface(surf, cc->from, f, cc->t, &converted, NULL);
    if (rc != PC_OK) {
        cc->rc = rc;
        return;
    }
    pc_surface_fini(surf);
    *surf = converted;
}

pc_status pc_engine_convert_document(pc_document *doc, const pc_profile *dst, pc_intent intent, bool bpc,
                                     pc_error *err)
{
    pc_color_mode to_mode;
    if (!space_mode(dst->space, &to_mode)) {
        return pc_engine_err_other(err, "cannot convert to a %s profile", cs_debug_name(dst->space));
    }
    if (to_mode != PC_COLOR_MODE_GRAYSCALE) {
        if (ink_duotone(doc, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    pc_profile src;
    if (pc_engine_document_profile(doc, &src, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_color_mode from_mode = pc_document_pixel_format(doc).mode;
    pc_transform *t = NULL;
    pc_status rc = pc_transform_new(&src, dst, intent, bpc, &t, err);
    pc_profile_fini(&src);
    if (rc != PC_OK) {
        return pc_engine_err_other(err, "colour management: cannot build the mode conversion");
    }
    pc_pixel_format fmt = {to_mode, doc->depth, true};
    conv_ctx cc = {from_mode, fmt, t, PC_OK};
    for_each_surface(doc->layers, doc->nlayers, conv_cb, &cc);
    if (cc.rc != PC_OK) {
        pc_transform_release(t);
        return cc.rc;
    }
    color_cvt cv = {from_mode, to_mode, t};
    for (size_t i = 0; i < doc->nlayers; i++) {
        convert_layer_colors(&doc->layers[i], &cv);
    }
    pc_transform_release(t);

    doc->mode = to_mode;
    pc_buf bytes = PC_BUF_INIT;
    if (pc_profile_to_bytes(dst, &bytes, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_blob *blob = NULL;
    rc = pc_blob_new(bytes.data, bytes.len, &blob, err);
    pc_buf_fini(&bytes);
    if (rc != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_blob_release(doc->icc_profile);
    doc->icc_profile = blob;
    if (doc->has_color_table) {
        pc_color_table_fini(&doc->color_table);
        doc->has_color_table = false;
    }
    if (doc->has_duotone) {
        pc_duotone_fini(&doc->duotone);
        doc->has_duotone = false;
    }
    return PC_OK;
}

/* ============================================================== gamut */

static pc_status gamut_mask(const pc_document *doc, const pc_proof_setup *setup, float threshold,
                            uint8_t **out_mask, size_t *out_count, pc_error *err)
{
    pc_profile comp;
    if (pc_engine_composite_profile(doc, &comp, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_gamut_check *check = NULL;
    pc_status rc = pc_gamut_check_new(&comp, &setup->profile, threshold, &check, err);
    pc_profile_fini(&comp);
    if (rc != PC_OK) {
        return pc_engine_err_other(err, "colour management: cannot build the gamut check");
    }
    pc_compose_buffer buf;
    if (pc_compose_flatten(doc, &buf, err) != PC_OK) {
        pc_gamut_check_free(check);
        return PC_ERR_NOMEM;
    }
    uint8_t *mask = pc_calloc_array(buf.len ? buf.len : 1, 1);
    if (mask == NULL) {
        pc_compose_buffer_fini(&buf);
        pc_gamut_check_free(check);
        return pc_fail(err, PC_ERR_NOMEM, "gamut mask");
    }
    size_t count = 0;
    for (size_t i = 0; i < buf.len; i++) {
        float rgb[3] = {buf.px[i][0], buf.px[i][1], buf.px[i][2]};
        if (buf.px[i][3] > 0.0f && pc_gamut_check_out_of_gamut(check, rgb)) {
            mask[i] = 255;
            count++;
        }
    }
    pc_compose_buffer_fini(&buf);
    pc_gamut_check_free(check);
    *out_mask = mask;
    *out_count = count;
    return PC_OK;
}

/* ============================================================== shared param/result helpers */

static void obj_add_desc(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const pc_profile *p)
{
    char *tmp = pc_strndup(p->description, p->description_len);
    yyjson_mut_obj_add_strcpy(d, obj, key, tmp != NULL ? tmp : "");
    free(tmp);
}

static pc_status intent_or(yyjson_val *p, pc_intent def, pc_intent *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "intent");
    const char *s = v != NULL ? yyjson_get_str(v) : NULL;
    if (v == NULL || s == NULL) {
        *out = def;
        return PC_OK;
    }
    if (!pc_intent_parse(s, strlen(s), out)) {
        return pc_engine_err_other(err, "unknown intent `%s` (perceptual|relative|saturation|absolute)", s);
    }
    return PC_OK;
}

static bool bool_param(yyjson_val *p, const char *k, bool d)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v != NULL && yyjson_is_bool(v)) {
        return yyjson_get_bool(v);
    }
    return d;
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

/* ============================================================== convert_mode */

typedef struct mode_edit_ctx {
    const pc_profile *dst;
    pc_intent intent;
    bool bpc;
} mode_edit_ctx;

static pc_status mode_edit_fn(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                              pc_error *err)
{
    (void)has_active;
    (void)active;
    mode_edit_ctx *c = ctx;
    return pc_engine_convert_document(doc, c->dst, c->intent, c->bpc, err);
}

static pc_status mode_result(pc_json_value *out, const pc_profile *dst, pc_intent intent, bool bpc,
                             pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    obj_add_desc(d, root, "profile", dst);
    yyjson_mut_obj_add_str(d, root, "intent", pc_intent_id(intent));
    yyjson_mut_obj_add_bool(d, root, "bpc", bpc);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

pc_status pc_engine_color_convert_mode(pc_engine_session *s, pc_color_mode mode, yyjson_val *p,
                                       pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    if (doc->mode == PC_COLOR_MODE_MULTICHANNEL && mode != PC_COLOR_MODE_MULTICHANNEL) {
        return pc_engine_multichannel_convert_from(s, mode, p, out, err);
    }
    yyjson_val *prof = pc_json_get(p, "profile");
    const char *prof_spec = prof != NULL ? yyjson_get_str(prof) : NULL;
    if (doc->mode == mode && prof_spec == NULL) {
        return pc_engine_result_null(out, err);
    }
    pc_color_state *cs = &s->color;
    pc_profile dst;
    pc_status rc;
    if (prof_spec != NULL) {
        rc = pc_engine_resolve_profile(cs, prof_spec, doc, true, mode, &dst, err);
    } else {
        rc = color_working(cs, mode, &dst, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_color_mode want = mode;
    if (mode == PC_COLOR_MODE_BITMAP || mode == PC_COLOR_MODE_DUOTONE) {
        want = PC_COLOR_MODE_GRAYSCALE;
    } else if (mode == PC_COLOR_MODE_INDEXED || mode == PC_COLOR_MODE_MULTICHANNEL) {
        want = PC_COLOR_MODE_RGB;
    }
    pc_color_mode dst_mode;
    if (!space_mode(dst.space, &dst_mode) || dst_mode != want) {
        pc_status e =
            pc_engine_err_other(err, "profile `%.*s` is %s, not %s", (int)dst.description_len,
                                dst.description, cs_debug_name(dst.space), pc_color_mode_name(mode));
        pc_profile_fini(&dst);
        return e;
    }
    pc_intent intent;
    if (intent_or(p, pc_color_settings_intent(&cs->settings), &intent, err) != PC_OK) {
        pc_profile_fini(&dst);
        return PC_ERR_INVALID;
    }
    bool bpc = bool_param(p, "bpc", cs->settings.bpc);
    mode_edit_ctx ctx = {&dst, intent, bpc};
    rc = pc_engine_session_edit(s, "Mode Change", mode_edit_fn, &ctx, err);
    if (rc != PC_OK) {
        pc_profile_fini(&dst);
        return rc;
    }
    pc_status r2 = mode_result(out, &dst, intent, bpc, err);
    pc_profile_fini(&dst);
    return r2;
}

/* ============================================================== Edit > Assign Profile */

typedef struct assign_edit_ctx {
    pc_blob *bytes; /* NULL = clear */
} assign_edit_ctx;

static pc_status assign_edit_fn(pc_document *doc, bool *ha, pc_layer_id *a, void *ctx, pc_error *err)
{
    (void)ha;
    (void)a;
    (void)err;
    assign_edit_ctx *c = ctx;
    pc_blob_release(doc->icc_profile);
    doc->icc_profile = c->bytes != NULL ? pc_blob_retain(c->bytes) : NULL;
    return PC_OK;
}

static pc_status run_assign_profile(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *pv = pc_json_get(p, "profile");
    const char *spec = pv != NULL ? yyjson_get_str(pv) : NULL;
    if (spec == NULL) {
        return pc_engine_err_bad_params(err, "edit.assignProfile", "missing `profile`");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_blob *bytes = NULL;
    char *desc = NULL;
    if (strcmp(spec, "none") != 0) {
        pc_profile prof;
        if (pc_engine_resolve_profile(&s->color, spec, doc, false, PC_COLOR_MODE_RGB, &prof, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
        if (prof.space != cs_space(doc->mode)) {
            pc_status e = pc_engine_err_other(err, "profile `%.*s` is %s; the document is %s",
                                              (int)prof.description_len, prof.description,
                                              cs_debug_name(prof.space), pc_color_mode_name(doc->mode));
            pc_profile_fini(&prof);
            return e;
        }
        pc_buf b = PC_BUF_INIT;
        if (pc_profile_to_bytes(&prof, &b, err) != PC_OK) {
            pc_profile_fini(&prof);
            return PC_ERR_NOMEM;
        }
        pc_status rc = pc_blob_new(b.data, b.len, &bytes, err);
        pc_buf_fini(&b);
        desc = pc_strndup(prof.description, prof.description_len);
        pc_profile_fini(&prof);
        if (rc != PC_OK) {
            free(desc);
            return PC_ERR_NOMEM;
        }
    } else {
        desc = dupstr("none");
    }
    assign_edit_ctx ctx = {bytes};
    pc_status rc = pc_engine_session_edit(s, "Assign Profile", assign_edit_fn, &ctx, err);
    pc_blob_release(bytes);
    if (rc != PC_OK) {
        free(desc);
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(desc);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "profile", desc != NULL ? desc : "");
    yyjson_mut_doc_set_root(d, root);
    free(desc);
    return PC_OK;
}

/* ============================================================== Edit > Convert to Profile */

static pc_color_mode dst_mode_of(const pc_profile *p)
{
    pc_color_mode m;
    return space_mode(p->space, &m) ? m : PC_COLOR_MODE_RGB;
}

static pc_status run_convert_to_profile(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    yyjson_val *pv = pc_json_get(p, "profile");
    const char *spec = pv != NULL ? yyjson_get_str(pv) : NULL;
    if (spec == NULL) {
        return pc_engine_err_bad_params(err, "edit.convertToProfile", "missing `profile`");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_profile dst;
    if (pc_engine_resolve_profile(&s->color, spec, doc, false, PC_COLOR_MODE_RGB, &dst, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_intent intent;
    if (intent_or(p, pc_color_settings_intent(&s->color.settings), &intent, err) != PC_OK) {
        pc_profile_fini(&dst);
        return PC_ERR_INVALID;
    }
    bool bpc = bool_param(p, "bpc", s->color.settings.bpc);
    mode_edit_ctx ctx = {&dst, intent, bpc};
    pc_status rc = pc_engine_session_edit(s, "Convert to Profile", mode_edit_fn, &ctx, err);
    if (rc != PC_OK) {
        pc_profile_fini(&dst);
        return rc;
    }
    const pc_engine_doc_state *st2 = pc_engine_session_active(s);
    pc_color_mode newmode = st2 != NULL ? pc_doc_arc_get(st2->doc)->mode : dst_mode_of(&dst);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_profile_fini(&dst);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    obj_add_desc(d, root, "profile", &dst);
    yyjson_mut_obj_add_str(d, root, "mode", pc_color_mode_name(newmode));
    yyjson_mut_obj_add_str(d, root, "intent", pc_intent_id(intent));
    yyjson_mut_obj_add_bool(d, root, "bpc", bpc);
    yyjson_mut_doc_set_root(d, root);
    pc_profile_fini(&dst);
    return PC_OK;
}

/* ============================================================== Edit > Profile Info */

static pc_status run_profile_info(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const pc_document *doc = st != NULL ? pc_doc_arc_get(st->doc) : NULL;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_val *pv = pc_json_get(p, "profile");
    const char *spec = pv != NULL ? yyjson_get_str(pv) : NULL;
    if (spec != NULL) {
        pc_profile prof;
        if (resolve_fixed(spec, doc, &prof, err) != PC_OK) {
            pc_json_value_fini(out);
            return PC_ERR_INVALID;
        }
        yyjson_mut_val *j = yyjson_mut_obj(d);
        obj_add_desc(d, j, "description", &prof);
        yyjson_mut_obj_add_str(d, j, "colorSpace", cs_debug_name(prof.space));
        yyjson_mut_obj_add_val(d, root, "profile", j);
        pc_profile_fini(&prof);
    } else if (doc != NULL) {
        pc_profile prof;
        if (pc_engine_document_profile(doc, &prof, err) != PC_OK) {
            pc_json_value_fini(out);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *j = yyjson_mut_obj(d);
        obj_add_desc(d, j, "description", &prof);
        yyjson_mut_obj_add_str(d, j, "colorSpace", cs_debug_name(prof.space));
        yyjson_mut_obj_add_bool(d, j, "embedded", doc->icc_profile != NULL);
        yyjson_mut_obj_add_val(d, root, "profile", j);
        pc_profile_fini(&prof);
    } else {
        yyjson_mut_obj_add_null(d, root, "profile");
    }
    yyjson_mut_val *bi = yyjson_mut_arr(d);
    for (int b = 0; b < PC_BUILTIN_COUNT; b++) {
        const pc_profile *bp = pc_builtin_profile((pc_builtin)b);
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, o, "id", pc_builtin_id((pc_builtin)b));
        yyjson_mut_obj_add_str(d, o, "description", pc_builtin_description((pc_builtin)b));
        yyjson_mut_obj_add_str(d, o, "colorSpace", cs_debug_name(bp->space));
        yyjson_mut_arr_append(bi, o);
    }
    yyjson_mut_obj_add_val(d, root, "builtins", bi);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ============================================================== color.profileMismatch */

static pc_status run_profile_mismatch(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *av = pc_json_get(p, "action");
    const char *action = av != NULL ? yyjson_get_str(av) : NULL;
    if (action == NULL) {
        action = "preserve";
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_color_mode mode = pc_doc_arc_get(st->doc)->mode;
    yyjson_mut_doc *d = NULL;
    if (strcmp(action, "preserve") == 0 || strcmp(action, "useEmbedded") == 0) {
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, root, "action", "kept");
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    if (strcmp(action, "convert") == 0 || strcmp(action, "convertToWorking") == 0) {
        pc_profile dst;
        if (color_working(&s->color, mode, &dst, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        mode_edit_ctx ctx = {&dst, pc_color_settings_intent(&s->color.settings), s->color.settings.bpc};
        pc_status rc = pc_engine_session_edit(s, "Convert to Working RGB", mode_edit_fn, &ctx, err);
        if (rc != PC_OK) {
            pc_profile_fini(&dst);
            return rc;
        }
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            pc_profile_fini(&dst);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, root, "action", "converted");
        obj_add_desc(d, root, "profile", &dst);
        yyjson_mut_doc_set_root(d, root);
        pc_profile_fini(&dst);
        return PC_OK;
    }
    if (strcmp(action, "discard") == 0 || strcmp(action, "off") == 0) {
        assign_edit_ctx ctx = {NULL};
        pc_status rc = pc_engine_session_edit(s, "Discard Profile", assign_edit_fn, &ctx, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, root, "action", "discarded");
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    if (strcmp(action, "assignWorking") == 0) {
        pc_profile w;
        if (color_working(&s->color, mode, &w, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        pc_buf b = PC_BUF_INIT;
        if (pc_profile_to_bytes(&w, &b, err) != PC_OK) {
            pc_profile_fini(&w);
            return PC_ERR_NOMEM;
        }
        pc_blob *blob = NULL;
        pc_status rc = pc_blob_new(b.data, b.len, &blob, err);
        pc_buf_fini(&b);
        if (rc != PC_OK) {
            pc_profile_fini(&w);
            return PC_ERR_NOMEM;
        }
        assign_edit_ctx ctx = {blob};
        rc = pc_engine_session_edit(s, "Assign Profile", assign_edit_fn, &ctx, err);
        pc_blob_release(blob);
        if (rc != PC_OK) {
            pc_profile_fini(&w);
            return rc;
        }
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            pc_profile_fini(&w);
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, root, "action", "assigned");
        obj_add_desc(d, root, "profile", &w);
        yyjson_mut_doc_set_root(d, root);
        pc_profile_fini(&w);
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, "color.profileMismatch",
                                    "unknown action `%s` (preserve|convert|discard|assignWorking)", action);
}

/* ============================================================== View > Proof Setup/Colors/Gamut */

static pc_status run_proof_setup(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_doc_id id = doc->id;
    yyjson_val *pv = pc_json_get(p, "profile");
    const char *name = pv != NULL ? yyjson_get_str(pv) : NULL;
    if (name == NULL) {
        name = "working-cmyk";
    }
    pc_profile profile;
    if (pc_engine_resolve_profile(&s->color, name, doc, true, PC_COLOR_MODE_CMYK, &profile, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_intent intent;
    if (intent_or(p, PC_INTENT_RELATIVE, &intent, err) != PC_OK) {
        pc_profile_fini(&profile);
        return PC_ERR_INVALID;
    }
    bool bpc = bool_param(p, "bpc", true);
    bool paper = bool_param(p, "simulatePaper", false);
    char *name_owned = dupstr(name);
    pc_proof_view *pvw = color_proof_mut(&s->color, id, err);
    if (pvw == NULL || name_owned == NULL) {
        free(name_owned);
        pc_profile_fini(&profile);
        return PC_ERR_NOMEM;
    }
    proof_setup_fini(&pvw->setup);
    pvw->setup.name = name_owned;
    pvw->setup.profile = profile; /* move */
    pvw->setup.intent = intent;
    pvw->setup.bpc = bpc;
    pvw->setup.simulate_paper = paper;
    pvw->setup.kind = 0;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    obj_add_desc(d, root, "profile", &pvw->setup.profile);
    yyjson_mut_obj_add_str(d, root, "intent", pc_intent_id(intent));
    yyjson_mut_obj_add_bool(d, root, "bpc", bpc);
    yyjson_mut_obj_add_bool(d, root, "simulatePaper", paper);
    yyjson_mut_obj_add_bool(d, root, "proofColors", pvw->enabled);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_proof_colors(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_doc_id id = pc_doc_arc_get(st->doc)->id;
    pc_proof_view *pvw = color_proof_mut(&s->color, id, err);
    if (pvw == NULL) {
        return PC_ERR_NOMEM;
    }
    yyjson_val *on = pc_json_get(p, "on");
    pvw->enabled = (on != NULL && yyjson_is_bool(on)) ? yyjson_get_bool(on) : !pvw->enabled;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "on", pvw->enabled);
    obj_add_desc(d, root, "profile", &pvw->setup.profile);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_gamut_warning(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_doc_id id = doc->id;
    pc_proof_view *pvw = color_proof_mut(&s->color, id, err);
    if (pvw == NULL) {
        return PC_ERR_NOMEM;
    }
    yyjson_val *on = pc_json_get(p, "on");
    pvw->gamut_warning = (on != NULL && yyjson_is_bool(on)) ? yyjson_get_bool(on) : !pvw->gamut_warning;
    yyjson_val *thr = pc_json_get(p, "threshold");
    if (thr != NULL && yyjson_is_num(thr)) {
        double t = yyjson_get_num(thr);
        pvw->gamut_threshold = (float)(t > 0.0 ? t : 0.0);
    }
    /* Build a setup with an optional profile override for the gamut check. */
    pc_proof_setup setup;
    memset(&setup, 0, sizeof setup);
    yyjson_val *prof = pc_json_get(p, "profile");
    const char *override = prof != NULL ? yyjson_get_str(prof) : NULL;
    if (override != NULL) {
        if (resolve_fixed(override, doc, &setup.profile, err) != PC_OK) {
            return PC_ERR_INVALID;
        }
    } else if (pc_profile_clone(&pvw->setup.profile, &setup.profile, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    bool enabled = pvw->gamut_warning;
    float threshold = pvw->gamut_threshold;
    uint8_t *mask = NULL;
    size_t count = 0;
    pc_status rc = gamut_mask(doc, &setup, threshold, &mask, &count, err);
    if (rc != PC_OK) {
        pc_profile_fini(&setup.profile);
        return rc;
    }
    free(mask);
    size_t total = (size_t)doc->size.width * (size_t)doc->size.height;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_profile_fini(&setup.profile);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "on", enabled);
    obj_add_desc(d, root, "profile", &setup.profile);
    yyjson_mut_obj_add_real(d, root, "threshold", (double)threshold);
    yyjson_mut_obj_add_uint(d, root, "outOfGamut", count);
    yyjson_mut_obj_add_real(d, root, "fraction", (double)count / (double)(total ? total : 1));
    yyjson_mut_doc_set_root(d, root);
    pc_profile_fini(&setup.profile);
    return PC_OK;
}

/* ============================================================== Edit > Color Settings */

/* Build the camelCase ColorSettings JSON object (serde form). */
static yyjson_mut_val *settings_to_mut(const pc_color_settings *c, yyjson_mut_doc *d)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "workingRgb", c->working_rgb ? c->working_rgb : "");
    yyjson_mut_obj_add_strcpy(d, o, "workingCmyk", c->working_cmyk ? c->working_cmyk : "");
    yyjson_mut_obj_add_strcpy(d, o, "workingGray", c->working_gray ? c->working_gray : "");
    yyjson_mut_obj_add_str(d, o, "policyRgb", pc_color_policy_id(c->policy_rgb));
    yyjson_mut_obj_add_str(d, o, "policyCmyk", pc_color_policy_id(c->policy_cmyk));
    yyjson_mut_obj_add_str(d, o, "policyGray", pc_color_policy_id(c->policy_gray));
    yyjson_mut_obj_add_bool(d, o, "askOnMismatch", c->ask_on_mismatch);
    yyjson_mut_obj_add_bool(d, o, "askOnPaste", c->ask_on_paste);
    yyjson_mut_obj_add_bool(d, o, "askOnMissing", c->ask_on_missing);
    yyjson_mut_obj_add_strcpy(d, o, "intent", c->intent ? c->intent : "");
    yyjson_mut_obj_add_bool(d, o, "bpc", c->bpc);
    yyjson_mut_obj_add_bool(d, o, "dither", c->dither);
    yyjson_mut_obj_add_real(d, o, "blendTextGamma", (double)c->blend_text_gamma);
    yyjson_mut_obj_add_strcpy(d, o, "monitorProfile", c->monitor_profile ? c->monitor_profile : "");
    return o;
}

pc_status pc_engine_color_settings_to_mut(const pc_color_state *cs, yyjson_mut_doc *d, yyjson_mut_val **out)
{
    *out = settings_to_mut(&cs->settings, d);
    return *out != NULL ? PC_OK : PC_ERR_NOMEM;
}

static bool set_string(char **slot, const char *v); /* defined with the colorSettings command */

/* prefs.get for a colorSettings path: `subpath` is "" (the whole object) or a camelCase key. */
pc_status pc_engine_color_settings_get(const pc_color_state *cs, const char *subpath, pc_json_value *out,
                                       pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "color");
    }
    yyjson_mut_val *obj = settings_to_mut(&cs->settings, d);
    yyjson_mut_val *v = (subpath[0] == '\0') ? obj : yyjson_mut_obj_get(obj, subpath);
    if (v == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOT_FOUND, "unknown preference `colorSettings.%s`", subpath);
    }
    pc_status rc = pc_json_value_from_mut(v, out, err);
    yyjson_mut_doc_free(d);
    return rc;
}

/* prefs.reset for one colorSettings key: copy the default value of `key` into `next`. "" resets
 * the whole object. PC_ERR_NOT_FOUND for an unknown key. */
pc_status pc_engine_color_settings_reset_key(pc_color_settings *next, const char *key, pc_error *err)
{
    pc_color_settings def;
    if (pc_color_settings_default(&def, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    if (key[0] == '\0') {
        pc_color_settings tmp = *next;
        if (pc_color_settings_clone(&def, next, err) != PC_OK) {
            *next = tmp;
            rc = PC_ERR_NOMEM;
        } else {
            pc_color_settings_fini(&tmp);
        }
    } else if (strcmp(key, "workingRgb") == 0) {
        if (!set_string(&next->working_rgb, def.working_rgb))
            rc = PC_ERR_NOMEM;
    } else if (strcmp(key, "workingCmyk") == 0) {
        if (!set_string(&next->working_cmyk, def.working_cmyk))
            rc = PC_ERR_NOMEM;
    } else if (strcmp(key, "workingGray") == 0) {
        if (!set_string(&next->working_gray, def.working_gray))
            rc = PC_ERR_NOMEM;
    } else if (strcmp(key, "monitorProfile") == 0) {
        if (!set_string(&next->monitor_profile, def.monitor_profile))
            rc = PC_ERR_NOMEM;
    } else if (strcmp(key, "intent") == 0) {
        if (!set_string(&next->intent, def.intent))
            rc = PC_ERR_NOMEM;
    } else if (strcmp(key, "policyRgb") == 0) {
        next->policy_rgb = def.policy_rgb;
    } else if (strcmp(key, "policyCmyk") == 0) {
        next->policy_cmyk = def.policy_cmyk;
    } else if (strcmp(key, "policyGray") == 0) {
        next->policy_gray = def.policy_gray;
    } else if (strcmp(key, "askOnMismatch") == 0) {
        next->ask_on_mismatch = def.ask_on_mismatch;
    } else if (strcmp(key, "askOnPaste") == 0) {
        next->ask_on_paste = def.ask_on_paste;
    } else if (strcmp(key, "askOnMissing") == 0) {
        next->ask_on_missing = def.ask_on_missing;
    } else if (strcmp(key, "bpc") == 0) {
        next->bpc = def.bpc;
    } else if (strcmp(key, "dither") == 0) {
        next->dither = def.dither;
    } else if (strcmp(key, "blendTextGamma") == 0) {
        next->blend_text_gamma = def.blend_text_gamma;
    } else {
        rc = pc_fail(err, PC_ERR_NOT_FOUND, "unknown preference `colorSettings.%s`", key);
    }
    pc_color_settings_fini(&def);
    return rc;
}

/* prefs.set for a colorSettings path, into `next`: `subpath` is "" (a whole object value) or a
 * camelCase key. Returns an error on an invalid value. */
pc_status pc_engine_color_settings_set_path(pc_color_settings *next, const char *subpath, yyjson_val *value,
                                            pc_error *err)
{
    if (subpath[0] == '\0') {
        if (value == NULL || !yyjson_is_obj(value)) {
            return pc_engine_err_bad_params(err, "prefs.set", "`colorSettings` needs an object");
        }
        size_t idx, max;
        yyjson_val *k, *v;
        yyjson_obj_foreach(value, idx, max, k, v)
        {
            const char *key = yyjson_get_str(k);
            if (key != NULL && pc_engine_color_settings_apply(next, key, v, err) != PC_OK) {
                return PC_ERR_INVALID;
            }
        }
        return PC_OK;
    }
    return pc_engine_color_settings_apply(next, subpath, value, err);
}

/* Replace a settings string field (dup). */
static bool set_string(char **slot, const char *v)
{
    char *n = dupstr(v);
    if (n == NULL) {
        return false;
    }
    free(*slot);
    *slot = n;
    return true;
}

/* Apply one camelCase sub-key of colorSettings into `next`. Returns false (and fills err) on an
 * invalid value. Used by edit.colorSettings and prefs.set colorSettings.*. */
pc_status pc_engine_color_settings_apply(pc_color_settings *next, const char *key, yyjson_val *v,
                                         pc_error *err)
{
    const char *cmd = "edit.colorSettings";
    const char *sv = yyjson_get_str(v);
    if (strcmp(key, "workingRgb") == 0) {
        if (sv && !set_string(&next->working_rgb, sv)) {
            return pc_fail(err, PC_ERR_NOMEM, "color");
        }
    } else if (strcmp(key, "workingCmyk") == 0) {
        if (sv && !set_string(&next->working_cmyk, sv)) {
            return pc_fail(err, PC_ERR_NOMEM, "color");
        }
    } else if (strcmp(key, "workingGray") == 0) {
        if (sv && !set_string(&next->working_gray, sv)) {
            return pc_fail(err, PC_ERR_NOMEM, "color");
        }
    } else if (strcmp(key, "monitorProfile") == 0) {
        if (sv && !set_string(&next->monitor_profile, sv)) {
            return pc_fail(err, PC_ERR_NOMEM, "color");
        }
    } else if (strcmp(key, "policyRgb") == 0 || strcmp(key, "policyCmyk") == 0 ||
               strcmp(key, "policyGray") == 0) {
        pc_color_policy pol;
        if (sv == NULL || !pc_color_policy_parse(sv, &pol)) {
            return pc_engine_err_bad_params(err, cmd, "`%s` must be preserve|convert|off", key);
        }
        if (strcmp(key, "policyRgb") == 0) {
            next->policy_rgb = pol;
        } else if (strcmp(key, "policyCmyk") == 0) {
            next->policy_cmyk = pol;
        } else {
            next->policy_gray = pol;
        }
    } else if (strcmp(key, "askOnMismatch") == 0 || strcmp(key, "askOnPaste") == 0 ||
               strcmp(key, "askOnMissing") == 0 || strcmp(key, "bpc") == 0 || strcmp(key, "dither") == 0) {
        if (yyjson_is_bool(v)) {
            bool b = yyjson_get_bool(v);
            if (strcmp(key, "askOnMismatch") == 0) {
                next->ask_on_mismatch = b;
            } else if (strcmp(key, "askOnPaste") == 0) {
                next->ask_on_paste = b;
            } else if (strcmp(key, "askOnMissing") == 0) {
                next->ask_on_missing = b;
            } else if (strcmp(key, "bpc") == 0) {
                next->bpc = b;
            } else {
                next->dither = b;
            }
        }
    } else if (strcmp(key, "intent") == 0) {
        if (sv) {
            pc_intent it;
            if (pc_intent_parse(sv, strlen(sv), &it)) {
                if (!set_string(&next->intent, pc_intent_id(it))) {
                    return pc_fail(err, PC_ERR_NOMEM, "color");
                }
            } else if (!set_string(&next->intent, sv)) {
                return pc_fail(err, PC_ERR_NOMEM, "color");
            }
        }
    } else if (strcmp(key, "blendTextGamma") == 0) {
        if (yyjson_is_bool(v)) {
            next->blend_text_gamma = yyjson_get_bool(v) ? PC_PSBLEND_TEXT_GAMMA : 1.0f;
        } else if (yyjson_is_num(v)) {
            double g = yyjson_get_num(v);
            if (!(g >= 1.0 && g <= 2.2)) {
                return pc_engine_err_bad_params(err, cmd, "`blendTextGamma` must be 1.0..2.2 or a bool");
            }
            next->blend_text_gamma = (float)g;
        } else {
            return pc_engine_err_bad_params(err, cmd, "`blendTextGamma` must be 1.0..2.2 or a bool");
        }
    }
    /* Unknown keys are ignored (lenient). */
    return PC_OK;
}

/* Load a whole colorSettings object into the state leniently (invalid fields keep defaults). Used
 * by prefs load and the "colorSettings" whole-object prefs.set. */
pc_status pc_engine_color_settings_load(pc_color_state *cs, yyjson_val *obj, pc_error *err)
{
    if (obj == NULL || !yyjson_is_obj(obj)) {
        return PC_OK;
    }
    pc_color_settings next;
    if (pc_color_settings_clone(&cs->settings, &next, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(obj, idx, max, k, v)
    {
        const char *key = yyjson_get_str(k);
        if (key != NULL) {
            (void)pc_engine_color_settings_apply(&next, key, v, NULL); /* lenient */
        }
    }
    if (pc_color_settings_validate(&next, NULL) != PC_OK) {
        pc_color_settings_fini(&next);
        return PC_OK; /* keep current on invalid (lenient load) */
    }
    pc_color_settings_fini(&cs->settings);
    cs->settings = next;
    pc_psblend_set_text_gamma(cs->settings.blend_text_gamma);
    return PC_OK;
}

/* Resolve the headless monitor profile (no platform display reader): auto -> sRGB fallback, a
 * manual spec -> resolved (RGB) or sRGB fallback. *out owned; *source & reason borrowed. */
static pc_status color_monitor(const pc_color_state *cs, pc_profile *out, const char **source,
                               const char **reason, pc_error *err)
{
    const char *spec = cs->settings.monitor_profile ? cs->settings.monitor_profile : "";
    bool is_auto = spec[0] == '\0' || strcmp(spec, "auto") == 0;
    *reason = NULL;
    if (is_auto) {
        *source = "fallback";
        *reason = "this platform doesn't report display profiles";
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    }
    pc_profile p;
    memset(&p, 0, sizeof p);
    if (resolve_fixed(spec, NULL, &p, NULL) != PC_OK) {
        *source = "fallback";
        *reason = "the monitor profile could not be read";
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    }
    bool ok = p.space == PC_CS_RGB;
    if (ok) {
        pc_transform *t = NULL;
        if (pc_transform_new(pc_builtin_profile(PC_BUILTIN_SRGB), &p, PC_INTENT_RELATIVE, true, &t, NULL) ==
            PC_OK) {
            pc_transform_release(t);
        } else {
            ok = false;
        }
    }
    if (!ok) {
        pc_profile_fini(&p);
        *source = "fallback";
        *reason = "the monitor profile cannot be used as a display profile";
        return pc_profile_clone(pc_builtin_profile(PC_BUILTIN_SRGB), out, err);
    }
    *source = "manual";
    *out = p;
    return PC_OK;
}

pc_status pc_engine_display_monitor(const pc_engine_session *s, pc_profile *out, pc_error *err)
{
    const char *source = NULL, *reason = NULL;
    if (!s || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing display session/profile");
    }
    return color_monitor(&s->color, out, &source, &reason, err);
}

typedef struct cs_edit_ctx {
    pc_color_settings next; /* moved into the state on success */
} cs_edit_ctx;

static pc_status run_color_settings(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_color_settings next;
    if (pc_json_get(p, "reset") != NULL && bool_param(p, "reset", false)) {
        if (pc_color_settings_default(&next, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    } else if (pc_color_settings_clone(&s->color.settings, &next, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    static const char *const KEYS[] = {"workingRgb", "workingCmyk",   "workingGray", "monitorProfile",
                                       "policyRgb",  "policyCmyk",    "policyGray",  "askOnMismatch",
                                       "askOnPaste", "askOnMissing",  "bpc",         "dither",
                                       "intent",     "blendTextGamma"};
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) {
        yyjson_val *v = pc_json_get(p, KEYS[i]);
        if (v != NULL) {
            if (pc_engine_color_settings_apply(&next, KEYS[i], v, err) != PC_OK) {
                pc_color_settings_fini(&next);
                return PC_ERR_INVALID;
            }
        }
    }
    if (pc_color_settings_validate(&next, err) != PC_OK) {
        pc_color_settings_fini(&next);
        return PC_ERR_INVALID;
    }
    if (!pc_color_settings_eq(&next, &s->color.settings)) {
        pc_psblend_set_text_gamma(next.blend_text_gamma);
        pc_color_settings_fini(&s->color.settings);
        s->color.settings = next;
        s->prefs.rev++; /* persisted with the preferences */
    } else {
        pc_color_settings_fini(&next);
    }

    return pc_engine_color_diagnostics(s, out, err);
}
pc_status pc_engine_color_diagnostics(const pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    if (!s || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing color diagnostics output");
    /* Reply. */
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "settings", settings_to_mut(&s->color.settings, d));
    /* working descriptions */
    yyjson_mut_val *wk = yyjson_mut_obj(d);
    const pc_color_mode modes[3] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_GRAYSCALE};
    const char *wkeys[3] = {"rgb", "cmyk", "gray"};
    for (int i = 0; i < 3; i++) {
        pc_profile wp;
        if (color_working(&s->color, modes[i], &wp, NULL) == PC_OK) {
            obj_add_desc(d, wk, wkeys[i], &wp);
            pc_profile_fini(&wp);
        } else {
            yyjson_mut_obj_add_strcpy(d, wk, wkeys[i], "");
        }
    }
    yyjson_mut_obj_add_val(d, root, "working", wk);
    /* monitor (headless) */
    pc_profile mon;
    const char *source = "fallback", *reason = NULL;
    if (color_monitor(&s->color, &mon, &source, &reason, NULL) == PC_OK) {
        obj_add_desc(d, root, "monitor", &mon);
        yyjson_mut_obj_add_bool(d, root, "monitorDetected", false);
        yyjson_mut_val *ms = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, ms, "requested",
                                  s->color.settings.monitor_profile && s->color.settings.monitor_profile[0]
                                      ? s->color.settings.monitor_profile
                                      : "auto");
        yyjson_mut_obj_add_str(d, ms, "source", source);
        obj_add_desc(d, ms, "profile", &mon);
        char fp[20];
        uint64_t h = 0;
        (void)pc_profile_content_hash(&mon, &h, NULL);
        snprintf(fp, sizeof fp, "%016llx", (unsigned long long)h);
        yyjson_mut_obj_add_strcpy(d, ms, "fingerprint", fp);
        if (reason != NULL) {
            yyjson_mut_obj_add_strcpy(d, ms, "reason", reason);
        }
        yyjson_mut_obj_add_val(d, root, "monitorStatus", ms);
        pc_profile_fini(&mon);
    }
    yyjson_mut_obj_add_val(d, root, "displays", yyjson_mut_arr(d));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ============================================================== specs */

static const char *const MENU_EDIT[] = {"Edit"};
static const char *const MENU_VIEW[] = {"View"};

const pc_engine_command *pc_engine_color_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"edit.assignProfile", "Assign Profile\xe2\x80\xa6", MENU_EDIT, 1, NULL,
         "{\"profile\":\"working|srgb|display-p3|adobe-rgb-compat|prophoto-compat|linear-srgb|rec2020|"
         "gray-gamma-2.2|sgray|dot-gain-20-compat|lab-d50|coated-cmyk|none\" (or a path to an .icc file)}",
         has_doc, run_assign_profile, true},
        {"edit.convertToProfile", "Convert to Profile\xe2\x80\xa6", MENU_EDIT, 1, NULL,
         "{\"profile\":\"srgb|display-p3|adobe-rgb-compat|prophoto-compat|linear-srgb|rec2020|gray-gamma-2.2|"
         "sgray|lab-d50|coated-cmyk|working\" (or a path to an .icc file),\"intent\":\"perceptual|relative|"
         "saturation|absolute\"=\"relative\",\"bpc\":bool=true}",
         has_doc, run_convert_to_profile, true},
        {"edit.colorSettings", "Color Settings\xe2\x80\xa6", MENU_EDIT, 1, "Cmd+Shift+K",
         "{\"workingRgb\":\"srgb|display-p3|adobe-rgb-compat|prophoto-compat|linear-srgb|rec2020\","
         "\"workingCmyk\":\"coated-cmyk\",\"workingGray\":\"dot-gain-20-compat|sgray|gray-gamma-2.2\","
         "\"policyRgb\":\"preserve|convert|off\",\"policyCmyk\":\"preserve|convert|off\",\"policyGray\":"
         "\"preserve|convert|off\",\"askOnMismatch\":bool=true,\"askOnPaste\":bool=true,\"askOnMissing\":"
         "bool=false,\"intent\":\"relative|perceptual|saturation|absolute\",\"blendTextGamma\":1.0..2.2|"
         "bool=1.45,\"bpc\":bool=true,\"dither\":bool=true,\"monitorProfile\":\"auto|srgb|display-p3|"
         "adobe-rgb-compat|prophoto-compat|rec2020\",\"reset\":bool=false}",
         pc_engine_always, run_color_settings, true},
        {"color.profileMismatch", "Embedded Profile Mismatch", NULL, 0, NULL,
         "{\"action\":\"preserve|convert|discard|assignWorking\"}", has_doc, run_profile_mismatch, true},
        {"edit.profileInfo", "Profile Info", NULL, 0, NULL,
         "{\"profile\":\"<builtin id>|document|/path/to/profile.icc\"=document}", pc_engine_always,
         run_profile_info, false},
        {"view.proofSetup", "Proof Setup\xe2\x80\xa6", NULL, 0, NULL,
         "{\"profile\":\"working-cmyk|srgb|display-p3|adobe-rgb-compat|prophoto-compat|linear-srgb|rec2020|"
         "gray-gamma-2.2|sgray|lab-d50|coated-cmyk\" (or a path to an .icc file)=\"working-cmyk\",\"intent\":"
         "\"perceptual|relative|saturation|absolute\"=\"relative\",\"bpc\":bool=true,\"simulatePaper\":"
         "bool=false}",
         has_doc, run_proof_setup, false},
        {"view.proofColors", "Proof Colors", MENU_VIEW, 1, "Cmd+Y", "{\"on\":bool=toggle}", has_doc,
         run_proof_colors, false},
        {"view.gamutWarning", "Gamut Warning", MENU_VIEW, 1, "Cmd+Shift+Y",
         "{\"on\":bool=toggle,\"threshold\":deltaE=4,\"profile\":\"<proof profile override>\"}", has_doc,
         run_gamut_warning, false},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
