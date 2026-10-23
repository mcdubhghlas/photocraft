/* photocraft-perfbench: a headless C benchmark driver for the perf/budgets.toml scenarios
 * (REWRITE_PLAN.md Z1 / §6 T1). It builds a representative document for each scenario, times the
 * hot path through the public engine + compositor APIs (no GPU, no window), and reports p50/p95/max
 * plus peak RSS. `tools/perf.sh` builds and runs this; `tools/perf.sh --update-baseline` publishes
 * its --json output as perf/baseline.json.
 *
 *   photocraft-perfbench [--quick] [--reps N] [--only ID,ID,...] [--json FILE] [--list]
 *
 * Scenarios with no C counterpart yet (GPU-only, UI-side previews, brush-tool strokes, etc.) are
 * reported as "n/a (<why>)" and are never given a fabricated number. Scenarios whose operation is
 * reachable but fails at run time are reported as "error (<message>)", also never faked.
 *
 * This is a dev tool: it must never crash on any input and never fail the build. Every engine call
 * goes through the never-crash command dispatch; a NULL document or a bad param comes back as a
 * pc_status we record, not a signal.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <yyjson.h>

#include "automation/headless.h"
#include "base/json.h"
#include "base/status.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "geom/geom.h"
#include "ops/ops.h"

/* ---- global knobs ---------------------------------------------------------------------------- */

static int g_quick = 0;    /* small synthetic documents; budgets don't apply */
static int g_reps_override = 0;

/* ---- timing ---------------------------------------------------------------------------------- */

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* The percentile the Rust bench used: nearest-rank on the sorted samples. */
static double pct(const double *sorted, size_t n, double p)
{
    if (n == 0) {
        return 0.0;
    }
    if (n == 1) {
        return sorted[0];
    }
    double rank = p / 100.0 * (double)(n - 1);
    size_t lo = (size_t)rank;
    double frac = rank - (double)lo;
    if (lo + 1 >= n) {
        return sorted[n - 1];
    }
    return sorted[lo] + frac * (sorted[lo + 1] - sorted[lo]);
}

/* ---- engine helpers -------------------------------------------------------------------------- */

/* Run one command to completion (jobs waited on). params_json may be NULL. */
static pc_status px_exec(pc_headless *h, const char *id, const char *params_json, pc_error *err)
{
    yyjson_doc *p = NULL;
    yyjson_val *root = NULL;
    if (params_json != NULL) {
        if (pc_json_parse(params_json, strlen(params_json), &p, err) != PC_OK) {
            return err->code;
        }
        root = yyjson_doc_get_root(p);
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_headless_command_start(h, id, root, true, &out, err);
    pc_json_value_fini(&out);
    if (p != NULL) {
        yyjson_doc_free(p);
    }
    return rc;
}

/* file.new of the given geometry. */
static pc_status px_new(pc_headless *h, uint32_t w, uint32_t hgt, int depth, const char *mode,
                        const char *bg, pc_error *err)
{
    char buf[256];
    snprintf(buf, sizeof buf, "{\"width\":%u,\"height\":%u,\"depth\":%d,\"mode\":\"%s\",\"background\":\"%s\"}",
             w, hgt, depth, mode, bg);
    return px_exec(h, "file.new", buf, err);
}

/* The CPU compositor refresh (the oracle): flatten the active document. Stands in for the GPU
 * canvas refresh the Rust "+ refresh" rows measured. */
static pc_status px_refresh(pc_headless *h, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(h->session);
    if (st == NULL || st->doc == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no active document to refresh");
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_compose_buffer buf = {0};
    pc_status rc = pc_compose_flatten(doc, &buf, err);
    pc_compose_buffer_fini(&buf);
    return rc;
}

/* Add n filled raster layers and leave the top one active. */
static pc_status px_add_layers(pc_headless *h, int n, pc_error *err)
{
    for (int i = 0; i < n; i++) {
        if (px_exec(h, "layer.new.layer", "{}", err) != PC_OK) {
            return err->code;
        }
        /* Best-effort fill so the composite does real work; a fill failure is not fatal. */
        pc_error ignore;
        pc_error_clear(&ignore);
        (void)px_exec(h, "edit.fillForeground", NULL, &ignore);
    }
    return PC_OK;
}

/* ---- scenario descriptor --------------------------------------------------------------------- */

struct scn;
typedef pc_status (*px_fn)(pc_headless *h, const struct scn *s, pc_error *err);

typedef struct scn {
    const char *id;
    const char *title;
    const char *na;          /* non-NULL => report n/a with this reason, never run */
    px_fn setup;             /* build the document (untimed); may be NULL */
    px_fn pre;               /* per-rep untimed preparation (e.g. a first duplicate); may be NULL */
    px_fn step;              /* the timed operation */
    /* geometry used by the generic setups */
    uint32_t w, hgt;
    int depth;
    const char *mode, *bg;
    int nlayers;
    /* generic ST command */
    const char *cmd, *params;
    int refresh;             /* flatten after the timed command */
    int reps;
} scn;

static uint32_t qd(uint32_t v)
{
    /* In --quick, shrink a dimension so the area is ~1 MP, keeping aspect roughly. */
    if (!g_quick) {
        return v;
    }
    uint32_t r = v / 6;
    return r < 64 ? 64 : r;
}

static int qlayers(int n)
{
    if (!g_quick) {
        return n;
    }
    return n > 8 ? 8 : n;
}

/* ---- generic setups/steps -------------------------------------------------------------------- */

static pc_status setup_layered(pc_headless *h, const scn *s, pc_error *err)
{
    if (px_new(h, qd(s->w), qd(s->hgt), s->depth, s->mode, s->bg, err) != PC_OK) {
        return err->code;
    }
    return px_add_layers(h, qlayers(s->nlayers), err);
}

static pc_status setup_selection(pc_headless *h, const scn *s, pc_error *err)
{
    if (setup_layered(h, s, err) != PC_OK) {
        return err->code;
    }
    return px_exec(h, "select.all", NULL, err);
}

/* A selection with a hole in the middle, for Content-Aware Fill. */
static pc_status setup_hole(pc_headless *h, const scn *s, pc_error *err)
{
    if (px_new(h, qd(s->w), qd(s->hgt), s->depth, s->mode, s->bg, err) != PC_OK) {
        return err->code;
    }
    (void)px_exec(h, "edit.fillForeground", NULL, err);
    pc_error_clear(err);
    uint32_t w = qd(s->w), hh = qd(s->hgt);
    char buf[160];
    int32_t hx = (int32_t)w / 2 - 100, hy = (int32_t)hh / 2 - 100;
    snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":%d,\"width\":200,\"height\":200}", hx, hy);
    return px_exec(h, "select.rect", buf, err);
}

static pc_status step_cmd(pc_headless *h, const scn *s, pc_error *err)
{
    pc_status rc = px_exec(h, s->cmd, s->params, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (s->refresh) {
        rc = px_refresh(h, err);
    }
    return rc;
}

static pc_status step_refresh_only(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    return px_refresh(h, err);
}

/* undo / redo */
static pc_status pre_move(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    return px_exec(h, "layer.translate", "{\"dx\":10,\"dy\":0}", err);
}
static pc_status step_undo(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    if (!pc_engine_session_undo(h->session)) {
        return pc_fail(err, PC_ERR_INVALID, "nothing to undo");
    }
    return px_refresh(h, err);
}
static pc_status pre_opacity_then_undo(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    if (px_exec(h, "layer.setProps", "{\"opacity\":50}", err) != PC_OK) {
        return err->code;
    }
    if (!pc_engine_session_undo(h->session)) {
        return pc_fail(err, PC_ERR_INVALID, "nothing to undo");
    }
    return PC_OK;
}
static pc_status step_redo(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    if (!pc_engine_session_redo(h->session)) {
        return pc_fail(err, PC_ERR_INVALID, "nothing to redo");
    }
    return px_refresh(h, err);
}

/* duplicate */
static pc_status step_duplicate(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    pc_status rc = px_exec(h, "layer.duplicate", "{\"inPlace\":true}", err);
    if (rc != PC_OK) {
        return rc;
    }
    return px_refresh(h, err);
}
static pc_status pre_duplicate(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    return px_exec(h, "layer.duplicate", "{\"inPlace\":true}", err);
}

/* paste */
static pc_status pre_copy(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    if (px_exec(h, "select.all", NULL, err) != PC_OK) {
        return err->code;
    }
    return px_exec(h, "edit.copy", NULL, err);
}
static pc_status pre_copy_paste(pc_headless *h, const scn *s, pc_error *err)
{
    if (pre_copy(h, s, err) != PC_OK) {
        return err->code;
    }
    return px_exec(h, "edit.paste", NULL, err);
}
static pc_status step_paste(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    pc_status rc = px_exec(h, "edit.paste", NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    return px_refresh(h, err);
}

/* open / save a synthesized layered PSD */
static pc_status save_tmp_psd(pc_headless *h, const scn *s, char *path_out, size_t cap, pc_error *err)
{
    if (setup_layered(h, s, err) != PC_OK) {
        return err->code;
    }
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    snprintf(path_out, cap, "%s/pc-perf-%s-%d.psd", tmp, s->id, (int)getpid());
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_headless_save(h, -1, path_out, "psd", NULL, &out, err);
    pc_json_value_fini(&out);
    return rc;
}
static pc_status step_save_psd(pc_headless *h, const scn *s, pc_error *err)
{
    char path[1024];
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    snprintf(path, sizeof path, "%s/pc-perf-%s-%d.psd", tmp, s->id, (int)getpid());
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_headless_save(h, -1, path, "psd", NULL, &out, err);
    pc_json_value_fini(&out);
    (void)remove(path);
    return rc;
}
/* The open scenario's setup saves a PSD; the timed step opens it. */
static char g_open_path[1024];
static pc_status setup_make_psd(pc_headless *h, const scn *s, pc_error *err)
{
    return save_tmp_psd(h, s, g_open_path, sizeof g_open_path, err);
}
static pc_status step_open_psd(pc_headless *h, const scn *s, pc_error *err)
{
    (void)s;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_headless_open(h, g_open_path, &out, err);
    pc_json_value_fini(&out);
    if (rc == PC_OK) {
        rc = px_refresh(h, err);
    }
    (void)remove(g_open_path); /* setup re-creates it each rep; never leave a fixture behind */
    return rc;
}

/* plain single-layer document (for filters / adjustments / big refresh) */
static pc_status setup_plain(pc_headless *h, const scn *s, pc_error *err)
{
    if (px_new(h, qd(s->w), qd(s->hgt), s->depth, s->mode, s->bg, err) != PC_OK) {
        return err->code;
    }
    (void)px_exec(h, "edit.fillForeground", NULL, err);
    pc_error_clear(err);
    return PC_OK;
}

/* ---- the scenario table ---------------------------------------------------------------------- */
/* Geometry notes: layered = 20 MP (5000x4000) RGB8; big16 = 15000x10000 16-bit; big32 = 32-bit
 * 4000x3000; sel24/plain = 24-36 MP; many = 150-layer 2000x1500; big196 = 14000x14000. --quick
 * shrinks every dimension and caps layer counts. */

#define LAYERED  5000, 4000, 8, "rgb", "white", 6
#define BIG16    15000, 10000, 16, "rgb", "white", 2
#define BIG32    4000, 3000, 32, "rgb", "white", 20
#define SEL24    6000, 4000, 8, "rgb", "white", 2
#define MANY     2000, 1500, 8, "rgb", "white", 150
#define PLAIN36  6000, 6000, 8, "rgb", "white", 0
#define BIG196   14000, 14000, 8, "rgb", "white", 0

static scn SCENARIOS[] = {
    /* ---- #209 interactive edits on a layered 20 MP document ---- */
    {"P1", "Move a layer + refresh", NULL, setup_layered, NULL, step_cmd, LAYERED,
     "layer.translate", "{\"dx\":10,\"dy\":0}", 1, 15},
    {"P2", "Change layer opacity + refresh", NULL, setup_layered, NULL, step_cmd, LAYERED,
     "layer.setProps", "{\"opacity\":50}", 1, 15},
    {"P3", "Change layer fill + refresh", NULL, setup_layered, NULL, step_cmd, LAYERED,
     "layer.setProps", "{\"fill\":50}", 1, 15},
    {"P4", "Change blend mode + refresh", NULL, setup_layered, NULL, step_cmd, LAYERED,
     "layer.setProps", "{\"blend\":\"multiply\"}", 1, 15},
    {"P5", "Toggle visibility + refresh", NULL, setup_layered, NULL, step_cmd, LAYERED,
     "layer.setProps", "{\"visible\":false}", 1, 15},
    {"P6", "Undo a move + refresh", NULL, setup_layered, pre_move, step_undo, LAYERED, NULL, NULL, 0, 15},
    {"P7", "Redo an opacity change + refresh", NULL, setup_layered, pre_opacity_then_undo, step_redo,
     LAYERED, NULL, NULL, 0, 15},
    {"P8", "Duplicate a layer (first) + refresh", NULL, setup_layered, NULL, step_duplicate, LAYERED,
     NULL, NULL, 0, 7},
    {"P9", "Duplicate a layer (repeat) + refresh", NULL, setup_layered, pre_duplicate, step_duplicate,
     LAYERED, NULL, NULL, 0, 7},
    {"P10", "Paste a layer (first) + refresh", NULL, setup_layered, pre_copy, step_paste, LAYERED,
     NULL, NULL, 0, 7},
    {"P11", "Paste a layer (repeat) + refresh", NULL, setup_layered, pre_copy_paste, step_paste,
     LAYERED, NULL, NULL, 0, 7},
    {"P12", "Nudge 1 px in a 150-layer document + refresh", NULL, setup_layered, NULL, step_cmd, MANY,
     "layer.translate", "{\"dx\":1,\"dy\":0}", 1, 10},
    {"P51", "New layer in a 150-layer document + refresh", NULL, setup_layered, NULL, step_cmd, MANY,
     "layer.new.layer", "{}", 1, 10},
    {"P13", "Brush dab on a 16-bit 15000x10000 document", "no C op (brush dab is a UI tool stroke, not an engine command)",
     NULL, NULL, NULL, BIG16, NULL, NULL, 0, 0},
    {"P14", "Move a layer on a 16-bit 15000x10000 document + refresh", NULL, setup_layered, NULL,
     step_cmd, BIG16, "layer.translate", "{\"dx\":10,\"dy\":0}", 1, 7},
    {"P15", "Zoom / pan frame time", "not measurable (GPU canvas frame; no headless frame harness)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P16", "Brush dab on a 32-bit 20-layer document", "no C op (brush dab is a UI tool stroke, not an engine command)",
     NULL, NULL, NULL, BIG32, NULL, NULL, 0, 0},
    {"P17", "Move a layer on a 32-bit 20-layer document + refresh", NULL, setup_layered, NULL, step_cmd,
     BIG32, "layer.translate", "{\"dx\":10,\"dy\":0}", 1, 7},

    /* ---- #210 long operations ---- */
    {"P18", "Open a 20 MP layered PSD (first pixels)", NULL, setup_make_psd, NULL, step_open_psd,
     LAYERED, NULL, NULL, 0, 5},
    {"P19", "Open a 20 MP layered PSD (editable)", NULL, setup_make_psd, NULL, step_open_psd, LAYERED,
     NULL, NULL, 0, 5},
    {"P20", "Open a 2 GB PSD / 4-5 GB PSB", "not measurable (no generated multi-GB fixtures)", NULL,
     NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P21", "Content-Aware Fill on 10 MP (200 px hole)", NULL, setup_hole, NULL, step_cmd,
     4000, 2500, 8, "rgb", "white", 0, "edit.contentAwareFill", NULL, 0, 5},
    {"P22", "Spot Healing stroke on 10 MP", "no C op (Spot Healing is a UI tool stroke)", NULL, NULL,
     NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P23", "Warp / Puppet / Perspective preview at 4K", "not measurable (UI tool preview; no headless frame harness)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P24", "Import 500 brushes (longest UI-thread step)", "no C op (brush import is driven by the preset store sync; no scenario harness yet)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P25", "Cancel a Gaussian Blur job on 24 MP", "no C op (cancellation-latency harness not ported)", NULL,
     NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P49", "Cancel a Content-Aware Fill job on 24 MP", "no C op (cancellation-latency harness not ported)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P50", "App frame time while a 24 MP blur job runs", "not measurable (needs the GUI frame loop)", NULL,
     NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},

    /* ---- #211 slow algorithms ---- */
    {"P26", "Select > Modify > Smooth r 50 on 24 MP", NULL, setup_selection, NULL, step_cmd, SEL24,
     "select.modify.smooth", "{\"radius\":50}", 0, 7},
    {"P27", "Select > Modify > Feather r 50 on 24 MP", NULL, setup_selection, NULL, step_cmd, SEL24,
     "select.modify.feather", "{\"radius\":50}", 0, 7},
    {"P28", "Content-Aware Scale 24 MP to 90% width", NULL, setup_plain, NULL, step_cmd, SEL24,
     "edit.contentAwareScale", "{\"scaleX\":0.9}", 0, 3},
    {"P29", "Select Subject on 24 MP (repeat)", NULL, setup_plain, NULL, step_cmd, SEL24,
     "select.subject", NULL, 0, 5},
    {"P30", "Liquify 800 px brush at full resolution", "no C op (Liquify is a UI tool brush preview)", NULL,
     NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P31", "Camera Raw exposure change (proxy)", "not measurable (Camera Raw proxy update is a UI-side scope pipeline)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P32", "Brush release on a long stroke", "no C op (brush stroke is a UI tool)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P33", "Brush dab on A4 300 ppi CMYK", "no C op (brush dab is a UI tool stroke)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P34", "Brush preview memory on an 8K viewport", "not measurable (UI-side preview buffers)", NULL,
     NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P35", "Marquee drag on 24 MP with Alt/Shift", "not measurable (UI-side drag preview)", NULL, NULL,
     NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P36", "Camera Raw exposure change with all SDR scopes", "not measurable (UI-side scope pipeline)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},

    /* ---- tracked (no budget): regressions only ---- */
    {"P40", "Brush dab 60 px on 36 MP", "no C op (brush dab is a UI tool stroke)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P41", "Gaussian Blur r 40 on 36 MP + refresh", NULL, setup_plain, NULL, step_cmd, PLAIN36,
     "filter.blur.gaussianBlur", "{\"radius\":40}", 1, 5},
    {"P42", "Curves apply on 36 MP + refresh", NULL, setup_plain, NULL, step_cmd, PLAIN36,
     "image.adjustments.curves", "{}", 1, 5},
    {"P43", "Save .psd, 36 MP layered", NULL, setup_layered, NULL, step_save_psd, PLAIN36, NULL, NULL,
     0, 5},
    {"P44", "Open .psd, 20 MP layered + refresh", NULL, setup_make_psd, NULL, step_open_psd, LAYERED,
     NULL, NULL, 0, 5},
    {"P45", "Full refresh with layer effects, 36 MP (GPU)", "no C op (GPU compositor path; the CPU oracle is P48)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P46", "Brush dab on a layer with effects, 36 MP (GPU)", "no C op (GPU compositor path)", NULL, NULL,
     NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P47", "Font-size drag step, 50 type layers", "no C op (type relayout is driven by the UI type tool)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P48", "Full CPU refresh of a 14000x14000 document", NULL, setup_plain, NULL, step_refresh_only,
     BIG196, NULL, NULL, 0, 3},
    {"P52", "Chisel bevel size 8, ellipse (GPU)", "no C op (GPU layer-effect shader)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P53", "Chisel bevel size 50, ellipse (GPU)", "no C op (GPU layer-effect shader)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P54", "Chisel bevel size 250, ellipse (GPU)", "no C op (GPU layer-effect shader)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P55", "Chisel bevel size 8, text (GPU)", "no C op (GPU layer-effect shader)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P56", "Chisel bevel size 50, text (GPU)", "no C op (GPU layer-effect shader)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P57", "Chisel bevel size 250, text (GPU)", "no C op (GPU layer-effect shader)", NULL, NULL, NULL,
     0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P58", "Chisel bevel size 8, ellipse (CPU)", "no C op (bevel layer-effect CPU timing harness not ported)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
    {"P59", "Chisel bevel size 8, text (CPU)", "no C op (bevel layer-effect CPU timing harness not ported)",
     NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0},
};
static const size_t N_SCN = sizeof SCENARIOS / sizeof SCENARIOS[0];

/* ---- a measured result ----------------------------------------------------------------------- */

typedef struct {
    int ran;          /* 0 = n/a or error */
    int errored;
    char msg[320];    /* n/a reason or error message (room for a "setup: " prefix) */
    size_t n;
    double p50, p95, max;
    long peak_rss_kb;
} result;

static long peak_rss_kb(void)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return 0;
    }
    return ru.ru_maxrss; /* KiB on Linux */
}

static void run_scenario(const scn *s, result *r)
{
    memset(r, 0, sizeof *r);
    if (s->na != NULL) {
        snprintf(r->msg, sizeof r->msg, "%s", s->na);
        return;
    }
    int reps = g_reps_override > 0 ? g_reps_override : (s->reps > 0 ? s->reps : 5);
    double *samples = calloc((size_t)reps, sizeof *samples);
    if (samples == NULL) {
        r->errored = 1;
        snprintf(r->msg, sizeof r->msg, "out of memory allocating samples");
        return;
    }
    size_t got = 0;
    for (int i = 0; i < reps; i++) {
        pc_headless *h = NULL;
        pc_error err;
        pc_error_clear(&err);
        if (pc_headless_trusted_local(&h, &err) != PC_OK) {
            r->errored = 1;
            snprintf(r->msg, sizeof r->msg, "session: %s", err.msg);
            break;
        }
        pc_status rc = PC_OK;
        if (s->setup != NULL) {
            rc = s->setup(h, s, &err);
        }
        if (rc == PC_OK && s->pre != NULL) {
            rc = s->pre(h, s, &err);
        }
        if (rc != PC_OK) {
            r->errored = 1;
            snprintf(r->msg, sizeof r->msg, "setup: %s", err.msg);
            pc_headless_free(h);
            break;
        }
        double t0 = now_ms();
        rc = s->step(h, s, &err);
        double t1 = now_ms();
        if (rc != PC_OK) {
            r->errored = 1;
            snprintf(r->msg, sizeof r->msg, "%s", err.msg);
            pc_headless_free(h);
            break;
        }
        samples[got++] = t1 - t0;
        pc_headless_free(h);
    }
    if (got > 0 && !r->errored) {
        qsort(samples, got, sizeof *samples, cmp_double);
        r->ran = 1;
        r->n = got;
        r->p50 = pct(samples, got, 50.0);
        r->p95 = pct(samples, got, 95.0);
        r->max = samples[got - 1];
        r->peak_rss_kb = peak_rss_kb();
    }
    free(samples);
}

/* ---- machine info for the JSON report -------------------------------------------------------- */

static void cpu_model(char *out, size_t cap)
{
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "model name", 10) == 0) {
            char *c = strchr(line, ':');
            if (c != NULL) {
                c++;
                while (*c == ' ') {
                    c++;
                }
                size_t len = strlen(c);
                while (len > 0 && (c[len - 1] == '\n' || c[len - 1] == ' ')) {
                    len--;
                }
                snprintf(out, cap, "%.*s", (int)len, c);
            }
            break;
        }
    }
    fclose(f);
}

static void write_json(FILE *f, const result *results)
{
    struct utsname un;
    memset(&un, 0, sizeof un);
    uname(&un);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGE_SIZE);
    unsigned long long ram = (pages > 0 && psz > 0) ? (unsigned long long)pages * (unsigned long long)psz : 0;
    char cpu[256];
    cpu_model(cpu, sizeof cpu);
    char date[32];
    time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    const char *sha = pc_engine_git_sha();

    char class[256];
    snprintf(class, sizeof class, "%s-%s-c-perfbench", un.sysname, un.machine);
    for (char *p = class; *p; p++) {
        *p = (char)((*p >= 'A' && *p <= 'Z') ? *p - 'A' + 'a' : (*p == ' ' ? '-' : *p));
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"schema\": 1,\n");
    fprintf(f, "  \"machine_class\": \"%s\",\n", class);
    fprintf(f, "  \"machine\": {\n");
    fprintf(f, "    \"arch\": \"%s\",\n", un.machine);
    fprintf(f, "    \"class\": \"%s\",\n", class);
    fprintf(f, "    \"cores\": %ld,\n", cores > 0 ? cores : 0);
    fprintf(f, "    \"cpu\": \"%s\",\n", cpu[0] ? cpu : "unknown");
    fprintf(f, "    \"gpu_adapter\": \"none (CPU oracle)\",\n");
    fprintf(f, "    \"os\": \"%s\",\n", un.sysname);
    fprintf(f, "    \"os_version\": \"%s\",\n", un.release);
    fprintf(f, "    \"ram_bytes\": %llu\n", ram);
    fprintf(f, "  },\n");
    fprintf(f, "  \"modes\": {\n");
    fprintf(f, "    \"%s\": {\n", g_quick ? "quick" : "full");
    fprintf(f, "      \"commit\": \"%s\",\n", sha ? sha : "");
    fprintf(f, "      \"date\": \"%s\",\n", date);
    fprintf(f, "      \"driver\": \"photocraft-perfbench (C; CPU oracle; no GPU)\",\n");
    {
        double la1 = 0, la2 = 0, la3 = 0;
        FILE *lf = fopen("/proc/loadavg", "r");
        if (lf != NULL) {
            if (fscanf(lf, "%lf %lf %lf", &la1, &la2, &la3) != 3) {
                la1 = la2 = la3 = 0;
            }
            fclose(lf);
        }
        fprintf(f, "      \"load_avg_start\": [\n        %.2f,\n        %.2f,\n        %.2f\n      ],\n",
                la1, la2, la3);
    }
    /* n/a and errored scenarios, recorded honestly (never a fabricated number). Emitted BEFORE
     * "scenarios" so tools/scorecard.sh's indentation-sensitive parser never mistakes these ids
     * for measured rows. */
    fprintf(f, "      \"not_measured\": {\n");
    int first = 1;
    for (size_t i = 0; i < N_SCN; i++) {
        if (results[i].ran) {
            continue;
        }
        if (!first) {
            fprintf(f, ",\n");
        }
        first = 0;
        const char *kind = results[i].errored ? "error" : "n/a";
        fprintf(f, "        \"%s\": {\"status\": \"%s\", \"reason\": \"", SCENARIOS[i].id, kind);
        for (const char *c = results[i].msg; *c; c++) {
            if (*c == '"' || *c == '\\') {
                fputc('\\', f);
            }
            if (*c == '\n') {
                fputs("\\n", f);
            } else {
                fputc(*c, f);
            }
        }
        fprintf(f, "\"}");
    }
    fprintf(f, "\n      },\n");
    /* Measured scenarios last, one field per line at the indentation tools/scorecard.sh expects
     * (ind 8 for the id, ind 10 for each field). */
    fprintf(f, "      \"scenarios\": {\n");
    first = 1;
    for (size_t i = 0; i < N_SCN; i++) {
        if (!results[i].ran) {
            continue;
        }
        if (!first) {
            fprintf(f, ",\n");
        }
        first = 0;
        const result *r = &results[i];
        fprintf(f, "        \"%s\": {\n", SCENARIOS[i].id);
        fprintf(f, "          \"n\": %zu,\n", r->n);
        fprintf(f, "          \"p50_ms\": %.6f,\n", r->p50);
        fprintf(f, "          \"p95_ms\": %.6f,\n", r->p95);
        fprintf(f, "          \"max_ms\": %.6f,\n", r->max);
        fprintf(f, "          \"peak_rss_bytes\": %lld,\n", (long long)r->peak_rss_kb * 1024);
        fprintf(f, "          \"gpu_bytes\": null\n");
        fprintf(f, "        }");
    }
    fprintf(f, "\n      }\n");
    fprintf(f, "    }\n");
    fprintf(f, "  }\n");
    fprintf(f, "}\n");
}

/* ---- main ------------------------------------------------------------------------------------ */

static int selected(const char *only, const char *id)
{
    if (only == NULL) {
        return 1;
    }
    size_t idlen = strlen(id);
    const char *p = only;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        if (seg == idlen && strncmp(p, id, seg) == 0) {
            return 1;
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *json_path = NULL;
    const char *only = NULL;
    int list = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--quick") == 0) {
            g_quick = 1;
        } else if (strcmp(argv[i], "--list") == 0) {
            list = 1;
        } else if (strcmp(argv[i], "--reps") == 0 && i + 1 < argc) {
            g_reps_override = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            only = argv[++i];
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            json_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: photocraft-perfbench [--quick] [--reps N] [--only ID,ID] [--json FILE] [--list]\n");
            return 0;
        } else {
            fprintf(stderr, "perfbench: ignoring unknown argument '%s'\n", argv[i]);
        }
    }

    if (list) {
        for (size_t i = 0; i < N_SCN; i++) {
            printf("%-5s %-55s %s\n", SCENARIOS[i].id, SCENARIOS[i].title,
                   SCENARIOS[i].na ? "n/a" : "measurable");
        }
        return 0;
    }

    result *results = calloc(N_SCN, sizeof *results);
    if (results == NULL) {
        fprintf(stderr, "perfbench: out of memory\n");
        return 1;
    }

    printf("photocraft-perfbench (%s mode; CPU oracle, no GPU)\n", g_quick ? "quick" : "full");
    printf("%-5s %-48s %8s %9s %9s %6s  %s\n", "id", "scenario", "p50(ms)", "p95(ms)", "max(ms)", "reps",
           "status");
    int mapped = 0, measured = 0, na = 0, errored = 0;
    for (size_t i = 0; i < N_SCN; i++) {
        if (!selected(only, SCENARIOS[i].id)) {
            continue;
        }
        if (SCENARIOS[i].na == NULL) {
            mapped++;
        }
        run_scenario(&SCENARIOS[i], &results[i]);
        const result *r = &results[i];
        char title[49];
        snprintf(title, sizeof title, "%s", SCENARIOS[i].title);
        if (r->ran) {
            measured++;
            printf("%-5s %-48s %8.2f %9.2f %9.2f %6zu  ok\n", SCENARIOS[i].id, title, r->p50, r->p95,
                   r->max, r->n);
        } else if (r->errored) {
            errored++;
            printf("%-5s %-48s %8s %9s %9s %6s  error: %s\n", SCENARIOS[i].id, title, "-", "-", "-", "-",
                   r->msg);
        } else {
            na++;
            printf("%-5s %-48s %8s %9s %9s %6s  n/a: %s\n", SCENARIOS[i].id, title, "-", "-", "-", "-",
                   r->msg);
        }
        fflush(stdout);
    }
    printf("\nsummary: %d measurable, %d measured ok, %d errored, %d n/a (no C op / not measurable)\n",
           mapped, measured, errored, na);

    if (json_path != NULL) {
        FILE *f = fopen(json_path, "w");
        if (f == NULL) {
            fprintf(stderr, "perfbench: cannot write %s\n", json_path);
            free(results);
            return 1;
        }
        write_json(f, results);
        fclose(f);
        printf("wrote %s\n", json_path);
    }

    free(results);
    return 0;
}
