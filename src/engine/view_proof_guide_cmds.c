/* View > Proof Setup simulations, View > 32-bit Preview Options and the View > guide commands
 * (port of crates/engine/src/proof_sim.rs, and the guide specs from edit_cmds.rs::specs and
 * guide_layout.rs / file_cmds.rs::new_guide_layout, layer L5).
 *
 * Landed here:
 *   - view.proofSetup.{workingCyanPlate,workingMagentaPlate,workingYellowPlate,workingBlackPlate,
 *     workingCmyPlate,legacyMacintoshRgb,colorBlindnessProtanopia,colorBlindnessDeuteranopia}:
 *     select a simulated proof and turn Proof Colors on (proof_sim::set_kind / run_kind).
 *   - view.thirtyTwoBitPreviewOptions: exposure / gamma / highlight-compression for 32-bit docs
 *     (proof_sim::preview_32).
 *   - view.newGuide / view.clearGuides: add or clear ruler guides (edit_cmds::guide_cmd).
 *   - view.newGuideLayout: columns, rows, margins and gutters of guides (guide_layout::calculate).
 *
 * DEFERRED, matching color_cmds.h's E8 note (REWRITE_PLAN.md §8): the display-LUT / dichromat /
 * plate / Legacy-Mac / 32-bit-preview SIMULATION *rendering* (proof_sim::display_lut and the
 * srgb/dichromat math) is a UI-canvas concern linked by no engine command; it stays unported.
 * These commands still set the proof *faithfully*: pc_proof_setup.kind, which color_cmds.c only
 * ever left at 0 (Profile), now carries one distinct value per reference ProofKind id, and
 * view.proofSetup.* sets it and enables Proof Colors exactly as set_kind does. The proof view is
 * the Session's pc_color_state (color_cmds.h); its lookup/create helper is private to color_cmds.c
 * (static color_proof_mut), so the same tiny lookup-or-default is reproduced below (vpg_proof_mut)
 * rather than editing the shared color_cmds.c. The 32-bit preview options have no storage field in
 * pc_color_state (that field is part of the deferred display path and color_cmds.h is a shared
 * file), so the merged per-document HDR state is kept in this module's own small table; the command
 * validates, clamps and reports the values just as preview_32 does.
 *
 * Never crash on hostile params (AGENTS.md rule 9): every enabled()/run() validates params and doc
 * state and returns an error, never aborts. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "cms/builtin.h"
#include "cms/cms.h"
#include "cms/gamut.h"
#include "cms/profile.h"
#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"

#include "color_cmds.h"
#include "engine_internal.h"
#include "prefs.h"
#include "view_proof_guide_cmds.h"

/* ---- proof-view lookup (mirror of color_cmds.c's static color_proof_mut) ------------------- */

/* The distinct simulation kinds, one per reference ProofKind id (proof_sim::ProofKind::id). 0 is
 * Profile, matching color_cmds.c's pc_proof_setup.kind default (a plain profile proof). */
enum {
    VPG_KIND_PROFILE = 0,  /* ProofKind::Profile (set by the existing view.proofSetup) */
    VPG_KIND_PLATE_CYAN,   /* Plate(0) */
    VPG_KIND_PLATE_MAGENTA,/* Plate(1) */
    VPG_KIND_PLATE_YELLOW, /* Plate(2) */
    VPG_KIND_PLATE_BLACK,  /* Plate(3) */
    VPG_KIND_CMY_PLATE,
    VPG_KIND_LEGACY_MAC_RGB,
    VPG_KIND_PROTANOPIA,
    VPG_KIND_DEUTERANOPIA
};

static const char *kind_id(int kind)
{
    switch (kind) {
    case VPG_KIND_PLATE_CYAN:
        return "workingCyanPlate";
    case VPG_KIND_PLATE_MAGENTA:
        return "workingMagentaPlate";
    case VPG_KIND_PLATE_YELLOW:
        return "workingYellowPlate";
    case VPG_KIND_PLATE_BLACK:
        return "workingBlackPlate";
    case VPG_KIND_CMY_PLATE:
        return "workingCmyPlate";
    case VPG_KIND_LEGACY_MAC_RGB:
        return "legacyMacintoshRgb";
    case VPG_KIND_PROTANOPIA:
        return "colorBlindnessProtanopia";
    case VPG_KIND_DEUTERANOPIA:
        return "colorBlindnessDeuteranopia";
    default:
        return "profile";
    }
}

static bool kind_is_plate(int kind)
{
    return kind == VPG_KIND_PLATE_CYAN || kind == VPG_KIND_PLATE_MAGENTA ||
           kind == VPG_KIND_PLATE_YELLOW || kind == VPG_KIND_PLATE_BLACK ||
           kind == VPG_KIND_CMY_PLATE;
}

/* Default proof view for a document that has none yet (color_cmds.c::proof_view_default). */
static pc_status proof_view_default(pc_proof_view *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    out->setup.name = pc_strndup("working-cmyk", strlen("working-cmyk"));
    if (out->setup.name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "proof");
    }
    if (pc_profile_clone(pc_builtin_profile(PC_BUILTIN_COATED_CMYK), &out->setup.profile, err) != PC_OK) {
        free(out->setup.name);
        out->setup.name = NULL;
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    out->setup.intent = PC_INTENT_RELATIVE;
    out->setup.bpc = true;
    out->setup.simulate_paper = false;
    out->setup.kind = VPG_KIND_PROFILE;
    out->enabled = false;
    out->gamut_warning = false;
    out->gamut_threshold = PC_GAMUT_DEFAULT_THRESHOLD;
    return PC_OK;
}

/* The proof view for `doc`, creating a default one if there is none (color_cmds.c::color_proof_mut). */
static pc_proof_view *vpg_proof_mut(pc_color_state *cs, pc_doc_id doc, pc_error *err)
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

static void obj_add_desc(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *key, const pc_profile *p)
{
    (void)yyjson_mut_obj_add_strncpy(d, obj, key, p->description, p->description_len);
}

/* ---- proof-setup variants (proof_sim::set_kind / run_kind) --------------------------------- */

static pc_status set_kind(pc_engine_session *s, int kind, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_doc_id id = doc->id;

    /* Plates print the working CMYK, like Photoshop; other kinds leave the profile alone. */
    bool plates = kind_is_plate(kind);
    pc_profile working;
    memset(&working, 0, sizeof working);
    bool have_working = false;
    if (plates) {
        if (pc_engine_resolve_profile(&s->color, "working-cmyk", doc, true, PC_COLOR_MODE_CMYK,
                                      &working, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_INVALID;
        }
        have_working = true;
    }

    pc_proof_view *pvw = vpg_proof_mut(&s->color, id, err);
    if (pvw == NULL) {
        if (have_working) {
            pc_profile_fini(&working);
        }
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (have_working) {
        char *name = pc_strndup("working-cmyk", strlen("working-cmyk"));
        if (name == NULL) {
            pc_profile_fini(&working);
            return pc_fail(err, PC_ERR_NOMEM, "proof");
        }
        pc_profile_fini(&pvw->setup.profile);
        pvw->setup.profile = working; /* move */
        free(pvw->setup.name);
        pvw->setup.name = name;
    }
    pvw->setup.kind = kind;
    pvw->enabled = true;

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "proof", kind_id(kind));
    obj_add_desc(d, root, "profile", &pvw->setup.profile);
    yyjson_mut_obj_add_bool(d, root, "proofColors", true);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_cyan_plate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_PLATE_CYAN, out, err);
}
static pc_status run_magenta_plate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_PLATE_MAGENTA, out, err);
}
static pc_status run_yellow_plate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_PLATE_YELLOW, out, err);
}
static pc_status run_black_plate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_PLATE_BLACK, out, err);
}
static pc_status run_cmy_plate(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_CMY_PLATE, out, err);
}
static pc_status run_legacy_mac(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_LEGACY_MAC_RGB, out, err);
}
static pc_status run_protanopia(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_PROTANOPIA, out, err);
}
static pc_status run_deuteranopia(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return set_kind(s, VPG_KIND_DEUTERANOPIA, out, err);
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return PC_OK;
}

/* ---- 32-bit Preview Options (proof_sim::preview_32) ---------------------------------------- */

/* Merged per-document HDR preview state. The reference keeps this in ColorState::hdr; here it is a
 * small module table because pc_color_state has no HDR field (it is part of the deferred display
 * path and color_cmds.h is a shared file). Bounded: at most VPG_HDR_MAX documents, oldest reused. */
typedef struct {
    pc_doc_id doc;
    bool used;
    bool highlight_compression;
    float exposure;
    float gamma;
} vpg_hdr_entry;

enum { VPG_HDR_MAX = 256 };
static vpg_hdr_entry g_hdr[VPG_HDR_MAX];
static size_t g_hdr_next; /* round-robin slot for a brand-new document */

static vpg_hdr_entry *hdr_slot(pc_doc_id id)
{
    for (size_t i = 0; i < VPG_HDR_MAX; i++) {
        if (g_hdr[i].used && g_hdr[i].doc == id) {
            return &g_hdr[i];
        }
    }
    for (size_t i = 0; i < VPG_HDR_MAX; i++) {
        if (!g_hdr[i].used) {
            g_hdr[i].used = true;
            g_hdr[i].doc = id;
            g_hdr[i].highlight_compression = false;
            g_hdr[i].exposure = 0.0f;
            g_hdr[i].gamma = 1.0f;
            return &g_hdr[i];
        }
    }
    /* Full: reuse a slot round-robin (its old document's options reset to the default). */
    vpg_hdr_entry *e = &g_hdr[g_hdr_next];
    g_hdr_next = (g_hdr_next + 1) % VPG_HDR_MAX;
    e->used = true;
    e->doc = id;
    e->highlight_compression = false;
    e->exposure = 0.0f;
    e->gamma = 1.0f;
    return e;
}

static pc_status is_32(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (pc_doc_arc_get(d->doc)->depth != PC_SAMPLE_TYPE_F32) {
        return pc_fail(reason, PC_ERR_INVALID, "32-bit Preview Options apply to 32-bit documents");
    }
    return PC_OK;
}

static pc_status preview_32(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "view.thirtyTwoBitPreviewOptions";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (pc_doc_arc_get(st->doc)->depth != PC_SAMPLE_TYPE_F32) {
        return pc_fail(err, PC_ERR_INVALID, "32-bit Preview Options apply to 32-bit documents");
    }
    pc_doc_id id = pc_doc_arc_get(st->doc)->id;
    /* Read the merged state into locals first, so a bad param leaves the stored state untouched. */
    vpg_hdr_entry *e = hdr_slot(id);
    bool hc = e->highlight_compression;
    float exposure = e->exposure;
    float gamma = e->gamma;

    yyjson_val *mv = pc_json_get(p, "method");
    if (mv != NULL && yyjson_is_str(mv)) {
        const char *m = yyjson_get_str(mv);
        if (m != NULL && (strcmp(m, "exposureGamma") == 0 || strcmp(m, "exposureAndGamma") == 0)) {
            hc = false;
        } else if (m != NULL && strcmp(m, "highlightCompression") == 0) {
            hc = true;
        } else {
            return pc_engine_err_bad_params(err, cmd, "method `%s` (exposureGamma|highlightCompression)",
                                            m != NULL ? m : "");
        }
    } else if (mv != NULL && !yyjson_is_null(mv)) {
        return pc_engine_err_bad_params(err, cmd, "method `` (exposureGamma|highlightCompression)");
    }
    yyjson_val *ev = pc_json_get(p, "exposure");
    double ed;
    if (ev != NULL && pc_json_as_f64(ev, &ed)) {
        if (ed < -20.0) {
            ed = -20.0;
        } else if (ed > 20.0) {
            ed = 20.0;
        }
        exposure = (float)ed;
    }
    yyjson_val *gv = pc_json_get(p, "gamma");
    double gd;
    if (gv != NULL && pc_json_as_f64(gv, &gd)) {
        if (gd < 0.1) {
            gd = 0.1;
        } else if (gd > 9.99) {
            gd = 9.99;
        }
        gamma = (float)gd;
    }

    e->highlight_compression = hc;
    e->exposure = exposure;
    e->gamma = gamma;

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "method", hc ? "highlightCompression" : "exposureGamma");
    yyjson_mut_obj_add_real(d, root, "exposure", (double)exposure);
    yyjson_mut_obj_add_real(d, root, "gamma", (double)gamma);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- guide length parsing (prefs::Unit::parse_length + guide_position) ---------------------- */

/* Guides further than this from the canvas origin are refused (edit_cmds::MAX_GUIDE_PX). */
#define VPG_MAX_GUIDE_PX 1000000.0

static double point_per_inch(const pc_engine_session *s)
{
    double ppi = 72.0; /* PostScript (the default) */
    pc_json_value v;
    memset(&v, 0, sizeof v);
    if (pc_engine_prefs_get(s, "unitsAndRulers.pointSize", &v, NULL) == PC_OK) {
        yyjson_mut_val *r = pc_json_value_root(&v);
        const char *str = r != NULL ? yyjson_mut_get_str(r) : NULL;
        if (str != NULL && strcmp(str, "traditional") == 0) {
            ppi = 72.27;
        }
    }
    pc_json_value_fini(&v);
    return ppi;
}

/* prefs::Unit::parse_length: a number with an optional unit suffix ("50%", "2.5 cm", "12pt",
 * "1,5 in", "100"). Returns true with *out_v the number, *has_unit / *out_unit the unit. */
static bool parse_length(const char *text, double *out_v, bool *has_unit, pc_pref_unit *out_unit)
{
    /* Trim leading/trailing ASCII whitespace. */
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == '\f' ||
           *text == '\v') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0) {
        char c = text[len - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            len--;
        } else {
            break;
        }
    }
    /* Split at the first char that is not part of a number. */
    size_t end = 0;
    while (end < len) {
        char c = text[end];
        if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == '-' || c == '+') {
            end++;
        } else {
            break;
        }
    }
    /* Copy the number part into a buffer, trimming space and mapping ',' to '.'. */
    char num[64];
    size_t ns = 0, ne = end;
    while (ns < ne && (text[ns] == ' ' || text[ns] == '\t')) {
        ns++;
    }
    while (ne > ns && (text[ne - 1] == ' ' || text[ne - 1] == '\t')) {
        ne--;
    }
    if (ne - ns == 0 || ne - ns >= sizeof num) {
        return false;
    }
    size_t k = 0;
    for (size_t i = ns; i < ne; i++) {
        num[k++] = text[i] == ',' ? '.' : text[i];
    }
    num[k] = '\0';
    char *tail = NULL;
    double v = strtod(num, &tail);
    if (tail == NULL || *tail != '\0' || !isfinite(v)) {
        return false;
    }
    /* The suffix after the number, trimmed. */
    size_t ss = end, se = len;
    while (ss < se && (text[ss] == ' ' || text[ss] == '\t')) {
        ss++;
    }
    while (se > ss && (text[se - 1] == ' ' || text[se - 1] == '\t')) {
        se--;
    }
    *out_v = v;
    if (se - ss == 0) {
        *has_unit = false;
        *out_unit = PC_UNIT_PIXELS;
        return true;
    }
    pc_pref_unit unit;
    if (!pc_pref_unit_from_suffix(text + ss, se - ss, &unit)) {
        return false;
    }
    *has_unit = true;
    *out_unit = unit;
    return true;
}

/* A guide `position` in document pixels. *has is false when there is no "position" key. */
static pc_status guide_position(const pc_engine_session *s, yyjson_val *p, bool vertical, bool *has,
                                double *px, pc_error *err)
{
    *has = false;
    yyjson_val *v = pc_json_get(p, "position");
    if (v == NULL) {
        return PC_OK;
    }
    double num;
    if (pc_json_as_f64(v, &num)) {
        *px = num;
    } else {
        const char *text = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
        if (text == NULL) {
            return pc_engine_err_other(
                err, "`position` must be a number of pixels or text with a unit");
        }
        double n;
        bool has_unit;
        pc_pref_unit unit;
        if (!parse_length(text, &n, &has_unit, &unit)) {
            return pc_engine_err_other(
                err, "`position` \"%s\" is not a length: give a number of pixels, or a number with px, "
                     "in, cm, mm, pt, pc or %%",
                text);
        }
        const pc_engine_doc_state *st = pc_engine_session_active(s);
        if (st == NULL) {
            return pc_engine_err_no_document(err);
        }
        const pc_document *doc = pc_doc_arc_get(st->doc);
        double extent = vertical ? (double)doc->size.width : (double)doc->size.height;
        *px = pc_pref_unit_to_px(unit, n, (double)doc->resolution_dpi, extent, point_per_inch(s));
    }
    if (!isfinite(*px) || fabs(*px) > VPG_MAX_GUIDE_PX) {
        return pc_engine_err_other(
            err, "`position` is out of range (at most 1000000 px from the canvas origin)");
    }
    *has = true;
    return PC_OK;
}

/* ---- guide colours (edit_cmds::GUIDE_COLORS / guide_color) ---------------------------------- */

static const struct {
    const char *name;
    uint8_t rgb[3];
} GUIDE_COLORS[] = {
    {"lightBlue", {0x33, 0x66, 0xff}},  {"lightRed", {0xff, 0x33, 0x33}},
    {"green", {0x33, 0xff, 0x33}},      {"mediumBlue", {0x33, 0x33, 0xff}},
    {"yellow", {0xff, 0xcc, 0x00}},     {"magenta", {0xff, 0x00, 0xff}},
    {"cyan", {0x4a, 0xff, 0xff}},       {"lightGray", {0xe7, 0xe7, 0xe7}},
    {"black", {0x00, 0x00, 0x00}},
};

static bool eq_ignore_ascii_case(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Resolve the optional guide `color`. *has is false when there is no "color" key. */
static pc_status guide_color(yyjson_val *p, bool *has, uint8_t rgb[3], pc_error *err)
{
    *has = false;
    yyjson_val *v = pc_json_get(p, "color");
    if (v == NULL) {
        return PC_OK;
    }
    const char *text = yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
    if (text == NULL) {
        return pc_engine_err_other(err, "`color` must be a colour name or \"#rrggbb\"");
    }
    for (size_t i = 0; i < sizeof GUIDE_COLORS / sizeof GUIDE_COLORS[0]; i++) {
        if (eq_ignore_ascii_case(GUIDE_COLORS[i].name, text)) {
            memcpy(rgb, GUIDE_COLORS[i].rgb, 3);
            *has = true;
            return PC_OK;
        }
    }
    if (pc_pref_parse_hex(text, strlen(text), rgb)) {
        *has = true;
        return PC_OK;
    }
    return pc_engine_err_other(
        err, "`color` \"%s\" is not a guide colour: use \"#rrggbb\" or one of lightBlue, lightRed, "
             "green, mediumBlue, yellow, magenta, cyan, lightGray, black",
        text);
}

/* ---- view.newGuide / view.clearGuides (edit_cmds::guide_cmd) -------------------------------- */

struct new_guide_ctx {
    bool vertical;
    double pos;
    bool has_color;
    uint8_t rgb[3];
};

static pc_status new_guide_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    struct new_guide_ctx *c = vctx;
    double **arr = c->vertical ? &doc->guides.vertical : &doc->guides.horizontal;
    size_t *n = c->vertical ? &doc->guides.nvertical : &doc->guides.nhorizontal;
    size_t cap = *n;
    double *grown = pc_grow_array(*arr, &cap, *n + 1, sizeof(double));
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "guide");
    }
    *arr = grown;
    (*arr)[(*n)++] = c->pos;
    if (c->has_color) {
        (void)pc_guides_set_color(&doc->guides, c->vertical, c->pos, c->rgb, err);
    }
    return PC_OK;
}

static pc_status clear_guides_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                   pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)vctx;
    (void)err;
    pc_guides_fini(&doc->guides);
    return PC_OK;
}

static pc_status run_new_guide(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_val *ov = pc_json_get(p, "orientation");
    const char *orient = ov != NULL && yyjson_is_str(ov) ? yyjson_get_str(ov) : NULL;
    bool vertical = orient != NULL && strcmp(orient, "vertical") == 0;
    struct new_guide_ctx c;
    memset(&c, 0, sizeof c);
    c.vertical = vertical;
    bool has_pos;
    if (guide_position(s, p, vertical, &has_pos, &c.pos, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    if (!has_pos) {
        return pc_engine_err_other(err, "missing `position`");
    }
    if (guide_color(p, &c.has_color, c.rgb, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    pc_status rc = pc_engine_session_edit(s, "New Guide", new_guide_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_clear_guides(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    if (pc_engine_session_active(s) == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_status rc = pc_engine_session_edit(s, "Clear Guides", clear_guides_edit, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

/* ---- view.newGuideLayout (guide_layout::calculate / apply) ---------------------------------- */

#define VPG_MAX_COUNT 1000u
#define VPG_MAX_EDGES 16000u

/* A growable list of doubles (added axes and the final merged axes). */
typedef struct {
    double *v;
    size_t n;
    size_t cap;
} dvec;

static void dvec_fini(dvec *d)
{
    free(d->v);
    d->v = NULL;
    d->n = d->cap = 0;
}

/* guide_layout::push_unique: append unless within 0.01 of an existing value; reject non-finite. */
static pc_status dvec_push_unique(dvec *d, double value, const char *cmd, pc_error *err)
{
    if (!isfinite(value)) {
        return pc_engine_err_bad_params(err, cmd, "guide coordinates exceed the supported range");
    }
    for (size_t i = 0; i < d->n; i++) {
        if (fabs(d->v[i] - value) < 0.01) {
            return PC_OK;
        }
    }
    double *g = pc_grow_array(d->v, &d->cap, d->n + 1, sizeof(double));
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, cmd);
    }
    d->v = g;
    d->v[d->n++] = value;
    return PC_OK;
}

/* guide_layout::axis: both edges of every column (or row) across [start, end]. */
static pc_status axis(double start, double end, uint64_t count, double fixed, double gap, bool center,
                      dvec *out, const char *cmd, pc_error *err)
{
    if (count == 0) {
        return PC_OK;
    }
    double available = end - start;
    double n = (double)count;
    double width = fixed > 0.0 ? fixed : (available - (n - 1.0) * gap) / n;
    double total = n * width + (n - 1.0) * gap;
    if (width <= 0.0 || !isfinite(total) || total > available + 0.001) {
        return pc_engine_err_bad_params(err, cmd,
                                        "count, size and gutter must fit inside the target margins");
    }
    double origin = (center && fixed > 0.0) ? start + (available - total) / 2.0 : start;
    for (uint64_t i = 0; i < count; i++) {
        double a = origin + (double)i * (width + gap);
        pc_status rc = dvec_push_unique(out, a, cmd, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = dvec_push_unique(out, a + width, cmd, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* guide_layout::count: a whole number, 0 when absent, at most VPG_MAX_COUNT. */
static pc_status gl_count(yyjson_val *p, const char *key, uint64_t *out, const char *cmd, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        *out = 0;
        return PC_OK;
    }
    uint64_t n;
    if (!pc_json_as_u64(v, &n)) {
        return pc_engine_err_bad_params(err, cmd, "%s must be a whole number", key);
    }
    if (n > VPG_MAX_COUNT) {
        return pc_engine_err_bad_params(err, cmd, "%s must be %u or fewer (got %llu)", key,
                                        VPG_MAX_COUNT, (unsigned long long)n);
    }
    *out = n;
    return PC_OK;
}

/* guide_layout::number: a finite non-negative number, `def` when absent or null. */
static pc_status gl_number(yyjson_val *p, const char *key, double def, double *out, const char *cmd,
                           pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL || yyjson_is_null(v)) {
        *out = def;
        return PC_OK;
    }
    double n;
    if (!pc_json_as_f64(v, &n) || !isfinite(n) || n < 0.0) {
        return pc_engine_err_bad_params(err, cmd, "%s must be a finite non-negative number", key);
    }
    *out = n;
    return PC_OK;
}

/* guide_layout::flag: a boolean, false when absent. */
static pc_status gl_flag(yyjson_val *p, const char *key, bool *out, const char *cmd, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        *out = false;
        return PC_OK;
    }
    bool b;
    if (!pc_json_as_bool(v, &b)) {
        return pc_engine_err_bad_params(err, cmd, "%s must be a boolean", key);
    }
    *out = b;
    return PC_OK;
}

/* guide_layout::selected_artboards: document rects of every selected artboard. *out owned (free). */
static pc_status selected_artboards(const pc_engine_session *s, pc_rect **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_doc_state_selected_layers(st, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    /* The distinct artboard layer ids that contain a selected layer. */
    pc_layer_id *ab = nsel > 0 ? pc_malloc_array(nsel, sizeof *ab) : NULL;
    if (nsel > 0 && ab == NULL) {
        free(sel);
        return pc_fail(err, PC_ERR_NOMEM, "guides");
    }
    size_t nab = 0;
    for (size_t i = 0; i < nsel; i++) {
        pc_layer_id board;
        if (pc_document_artboard_of(doc, sel[i], &board)) {
            bool seen = false;
            for (size_t j = 0; j < nab; j++) {
                if (ab[j] == board) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                ab[nab++] = board;
            }
        }
    }
    free(sel);
    pc_status rc = PC_OK;
    pc_artboard_ref *refs = NULL;
    size_t nref = 0;
    if (pc_document_artboards(doc, &refs, &nref, err) != PC_OK) {
        free(ab);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_rect *rects = nab > 0 ? pc_malloc_array(nab, sizeof *rects) : NULL;
    if (nab > 0 && rects == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "guides");
        goto done;
    }
    size_t nr = 0;
    for (size_t i = 0; i < nref; i++) {
        for (size_t j = 0; j < nab; j++) {
            if (refs[i].layer == ab[j]) {
                rects[nr++] = refs[i].artboard->rect;
                break;
            }
        }
    }
    *out = rects;
    *n = nr;
done:
    free(refs);
    free(ab);
    return rc;
}

static pc_status gl_targets(const pc_engine_session *s, yyjson_val *p, pc_rect **out, size_t *n,
                            const char *cmd, pc_error *err)
{
    *out = NULL;
    *n = 0;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    yyjson_val *tv = pc_json_get(p, "target");
    const char *target = "document";
    if (tv != NULL) {
        target = yyjson_is_str(tv) ? yyjson_get_str(tv) : NULL;
        if (target == NULL) {
            return pc_engine_err_bad_params(err, cmd, "target must be a string");
        }
    }
    if (strcmp(target, "document") == 0) {
        pc_rect *r = pc_malloc_array(1, sizeof *r);
        if (r == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, cmd);
        }
        r[0] = pc_document_bounds(pc_doc_arc_get(st->doc));
        *out = r;
        *n = 1;
        return PC_OK;
    }
    if (strcmp(target, "selectedArtboards") == 0) {
        pc_rect *boards = NULL;
        size_t nb = 0;
        if (selected_artboards(s, &boards, &nb, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (nb == 0) {
            free(boards);
            return pc_engine_err_bad_params(err, cmd, "select an artboard or a layer inside one");
        }
        *out = boards;
        *n = nb;
        return PC_OK;
    }
    return pc_engine_err_bad_params(err, cmd, "target must be document or selectedArtboards");
}

struct layout_ctx {
    pc_guides guides; /* the final guide set to commit (moved into the doc on edit) */
    bool moved;
};

static pc_status layout_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                             pc_error *err)
{
    (void)has_active;
    (void)active;
    (void)err;
    struct layout_ctx *c = vctx;
    pc_guides_fini(&doc->guides);
    doc->guides = c->guides; /* move */
    c->moved = true;
    return PC_OK;
}

/* Retain only the guides whose coordinate falls OUTSIDE every target rect (clearExisting with
 * target selectedArtboards: matches Clear Selected Artboard Guides). */
static void retain_outside(double *v, size_t *n, const pc_rect *bounds, size_t nb, bool vertical)
{
    size_t k = 0;
    for (size_t i = 0; i < *n; i++) {
        bool inside = false;
        for (size_t b = 0; b < nb; b++) {
            double lo = vertical ? (double)bounds[b].x0 : (double)bounds[b].y0;
            double hi = vertical ? (double)bounds[b].x1 : (double)bounds[b].y1;
            if (v[i] >= lo && v[i] <= hi) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            v[k++] = v[i];
        }
    }
    *n = k;
}

static pc_status run_new_guide_layout(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    const char *cmd = "view.newGuideLayout";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_status rc = PC_OK;
    dvec added_v = {0}, added_h = {0};
    pc_rect *bounds = NULL;
    size_t nbounds = 0;
    struct layout_ctx ctx;
    memset(&ctx, 0, sizeof ctx);

    uint64_t cols = 0, rows = 0;
    double width = 0, height = 0, gap = 0, row_gap = 0, margin[4] = {0};
    bool center = false, clear = false;
    if ((rc = gl_count(p, "columns", &cols, cmd, err)) != PC_OK ||
        (rc = gl_count(p, "rows", &rows, cmd, err)) != PC_OK ||
        (rc = gl_number(p, "width", 0.0, &width, cmd, err)) != PC_OK ||
        (rc = gl_number(p, "height", 0.0, &height, cmd, err)) != PC_OK ||
        (rc = gl_number(p, "gutter", 0.0, &gap, cmd, err)) != PC_OK ||
        (rc = gl_number(p, "rowGutter", gap, &row_gap, cmd, err)) != PC_OK ||
        (rc = gl_flag(p, "centerColumns", &center, cmd, err)) != PC_OK ||
        (rc = gl_flag(p, "clearExisting", &clear, cmd, err)) != PC_OK) {
        goto cleanup;
    }
    /* margin: a single number for all four edges, or [top, left, bottom, right]. */
    yyjson_val *mv = pc_json_get(p, "margin");
    if (mv != NULL && yyjson_is_arr(mv)) {
        if (yyjson_arr_size(mv) != 4) {
            rc = pc_engine_err_bad_params(err, cmd, "margin must contain top, left, bottom and right");
            goto cleanup;
        }
        for (size_t i = 0; i < 4; i++) {
            yyjson_val *el = yyjson_arr_get(mv, i);
            double m = 0;
            if (el == NULL || !pc_json_as_f64(el, &m) || !isfinite(m) || m < 0.0) {
                rc = pc_engine_err_bad_params(err, cmd, "margins must be finite non-negative numbers");
                goto cleanup;
            }
            margin[i] = m;
        }
    } else {
        double m = 0;
        if ((rc = gl_number(p, "margin", 0.0, &m, cmd, err)) != PC_OK) {
            goto cleanup;
        }
        margin[0] = margin[1] = margin[2] = margin[3] = m;
    }

    if ((rc = gl_targets(s, p, &bounds, &nbounds, cmd, err)) != PC_OK) {
        goto cleanup;
    }
    /* Bound the aggregate work (guide_layout::MAX_EDGES). */
    {
        uint64_t edges = cols + rows + 4;
        edges = edges > UINT64_MAX / 2 ? UINT64_MAX : edges * 2;
        uint64_t nb = (uint64_t)nbounds;
        edges = (nb != 0 && edges > UINT64_MAX / nb) ? UINT64_MAX : edges * nb;
        if (edges > VPG_MAX_EDGES) {
            rc = pc_engine_err_bad_params(err, cmd,
                                          "the selected targets would create too many guide lines");
            goto cleanup;
        }
    }

    bool any_margin = margin[0] != 0.0 || margin[1] != 0.0 || margin[2] != 0.0 || margin[3] != 0.0;
    double top = margin[0], left = margin[1], bottom = margin[2], right = margin[3];
    for (size_t b = 0; b < nbounds; b++) {
        double x0 = (double)bounds[b].x0 + left, y0 = (double)bounds[b].y0 + top;
        double x1 = (double)bounds[b].x1 - right, y1 = (double)bounds[b].y1 - bottom;
        if (x0 >= x1 || y0 >= y1) {
            rc = pc_engine_err_bad_params(err, cmd, "margins must leave space inside the target");
            goto cleanup;
        }
        if (any_margin) {
            if ((rc = dvec_push_unique(&added_v, x0, cmd, err)) != PC_OK ||
                (rc = dvec_push_unique(&added_v, x1, cmd, err)) != PC_OK ||
                (rc = dvec_push_unique(&added_h, y0, cmd, err)) != PC_OK ||
                (rc = dvec_push_unique(&added_h, y1, cmd, err)) != PC_OK) {
                goto cleanup;
            }
        }
        if ((rc = axis(x0, x1, cols, width, gap, center, &added_v, cmd, err)) != PC_OK ||
            (rc = axis(y0, y1, rows, height, row_gap, center, &added_h, cmd, err)) != PC_OK) {
            goto cleanup;
        }
    }
    if (added_v.n == 0 && added_h.n == 0) {
        rc = pc_engine_err_bad_params(err, cmd, "give \"columns\", \"rows\" or a \"margin\"");
        goto cleanup;
    }

    /* Build the final guide set: the current guides, optionally cleared, plus the added axes. */
    if (pc_guides_clone(&pc_doc_arc_get(st->doc)->guides, &ctx.guides, err) != PC_OK) {
        rc = err != NULL ? err->code : PC_ERR_NOMEM;
        goto cleanup;
    }
    if (clear) {
        yyjson_val *tv = pc_json_get(p, "target");
        const char *target = tv != NULL && yyjson_is_str(tv) ? yyjson_get_str(tv) : NULL;
        if (target != NULL && strcmp(target, "selectedArtboards") == 0) {
            retain_outside(ctx.guides.vertical, &ctx.guides.nvertical, bounds, nbounds, true);
            retain_outside(ctx.guides.horizontal, &ctx.guides.nhorizontal, bounds, nbounds, false);
            pc_guides_prune_colors(&ctx.guides);
        } else {
            pc_guides_fini(&ctx.guides);
            memset(&ctx.guides, 0, sizeof ctx.guides);
        }
    }
    {
        dvec fv = {ctx.guides.vertical, ctx.guides.nvertical, ctx.guides.nvertical};
        for (size_t i = 0; i < added_v.n; i++) {
            if ((rc = dvec_push_unique(&fv, added_v.v[i], cmd, err)) != PC_OK) {
                ctx.guides.vertical = fv.v;
                ctx.guides.nvertical = fv.n;
                goto cleanup;
            }
        }
        ctx.guides.vertical = fv.v;
        ctx.guides.nvertical = fv.n;
        dvec fh = {ctx.guides.horizontal, ctx.guides.nhorizontal, ctx.guides.nhorizontal};
        for (size_t i = 0; i < added_h.n; i++) {
            if ((rc = dvec_push_unique(&fh, added_h.v[i], cmd, err)) != PC_OK) {
                ctx.guides.horizontal = fh.v;
                ctx.guides.nhorizontal = fh.n;
                goto cleanup;
            }
        }
        ctx.guides.horizontal = fh.v;
        ctx.guides.nhorizontal = fh.n;
    }

    rc = pc_engine_session_edit(s, "New Guide Layout", layout_edit, &ctx, err);
    if (rc != PC_OK) {
        goto cleanup;
    }

    {
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            goto cleanup;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "vertical", added_v.n);
        yyjson_mut_obj_add_uint(d, root, "horizontal", added_h.n);
        yyjson_mut_doc_set_root(d, root);
    }

cleanup:
    if (!ctx.moved) {
        pc_guides_fini(&ctx.guides);
    }
    dvec_fini(&added_v);
    dvec_fini(&added_h);
    free(bounds);
    return rc;
}

/* ---- specs --------------------------------------------------------------------------------- */

static const char *const MENU_PROOF[] = {"View", "Proof Setup"};
static const char *const MENU_VIEW[] = {"View"};

#define PROOF_PARAMS "{} (sets the proof and turns Proof Colors on)"

static const pc_engine_command SPECS[] = {
    {"view.proofSetup.workingCyanPlate", "Working Cyan Plate", MENU_PROOF, 2, NULL, PROOF_PARAMS,
     has_doc, run_cyan_plate, false},
    {"view.proofSetup.workingMagentaPlate", "Working Magenta Plate", MENU_PROOF, 2, NULL, PROOF_PARAMS,
     has_doc, run_magenta_plate, false},
    {"view.proofSetup.workingYellowPlate", "Working Yellow Plate", MENU_PROOF, 2, NULL, PROOF_PARAMS,
     has_doc, run_yellow_plate, false},
    {"view.proofSetup.workingBlackPlate", "Working Black Plate", MENU_PROOF, 2, NULL, PROOF_PARAMS,
     has_doc, run_black_plate, false},
    {"view.proofSetup.workingCmyPlate", "Working CMY Plate", MENU_PROOF, 2, NULL, PROOF_PARAMS,
     has_doc, run_cmy_plate, false},
    {"view.proofSetup.legacyMacintoshRgb", "Legacy Macintosh RGB", MENU_PROOF, 2, NULL, PROOF_PARAMS,
     has_doc, run_legacy_mac, false},
    {"view.proofSetup.colorBlindnessProtanopia", "Color Blindness \xE2\x80\x94 Protanopia-type",
     MENU_PROOF, 2, NULL, PROOF_PARAMS, has_doc, run_protanopia, false},
    {"view.proofSetup.colorBlindnessDeuteranopia", "Color Blindness \xE2\x80\x94 Deuteranopia-type",
     MENU_PROOF, 2, NULL, PROOF_PARAMS, has_doc, run_deuteranopia, false},
    {"view.thirtyTwoBitPreviewOptions", "32-bit Preview Options\xE2\x80\xA6", MENU_VIEW, 1, NULL,
     "{\"method\":\"exposureGamma|highlightCompression\"=\"exposureGamma\",\"exposure\":-20..20=0,"
     "\"gamma\":0.1..9.99=1}",
     is_32, preview_32, false},
    {"view.clearGuides", "Clear Guides", MENU_VIEW, 1, NULL, "{}", has_doc, run_clear_guides, true},
    {"view.newGuide", "New Guide\xE2\x80\xA6", MENU_VIEW, 1, NULL,
     "{\"orientation\":\"horizontal|vertical\",\"position\":px or \"<number><unit>\",\"color\":"
     "\"lightBlue|lightRed|green|mediumBlue|yellow|magenta|cyan|lightGray|black\" or \"#rrggbb\"?} "
     "(units px, in, cm, mm, pt, pc or %; % is of the width for a vertical guide, the height for a "
     "horizontal one; without a color the guide uses the guide colour in Preferences)",
     has_doc, run_new_guide, true},
    {"view.newGuideLayout", "New Guide Layout\xE2\x80\xA6", MENU_VIEW, 1, NULL,
     "{\"columns\":n=0,\"width\":px?,\"gutter\":px=0,\"rows\":n=0,\"height\":px?,\"rowGutter\":px="
     "gutter,\"margin\":px|[top,left,bottom,right]=0,\"centerColumns\":bool=false,\"clearExisting\":"
     "bool=false,\"target\":\"document|selectedArtboards\"=\"document\"}",
     has_doc, run_new_guide_layout, true},
};

const pc_engine_command *pc_engine_view_proof_guide_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
