/* Fuzz every registered command with adversarial params on varied session states; a command must
 * always return a pc_status (PC_OK or an error), never crash, abort, corrupt memory or hang
 * (AGENTS.md Rule 9 / REWRITE_PLAN.md rule 9). Port of crates/engine/tests/panic_hunt.rs.
 *
 * Rust catches unwinding panics on a worker thread; C has no unwinding, so the never-crash signal
 * is simply that this process completes: a segfault, an assert/abort, or an ASan/UBSan report
 * fails the test by killing it (run this under ASan+UBSan for the strongest evidence). A per-command
 * SIGALRM watchdog turns a genuine hang into a named failure instead of a wedged run. Every
 * returned status is checked to be a valid enum value, and successful outputs are freed.
 *
 * The reference skips file.print / file.printOneCopy (they spool to CUPS); the C port registers
 * them, so main() calls pc_engine_print_disable() first (as the reference's panic hunt calls
 * print_cmds::disable_printing) -- the spool path then returns an error instead of reaching a
 * printer, and a dry run only renders a PDF in memory. The battery also carries no "path" key, so
 * no save/export command (nor the "output" of Print) writes a file. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h" /* pc_engine_print_disable */
#include "harness.h"

/* ---- hang watchdog ------------------------------------------------------------------------ */

static const char *volatile g_current_id; /* best-effort: the command the alarm would blame */
static volatile sig_atomic_t g_armed;

static void on_alarm(int sig)
{
    (void)sig;
    static const char pre[] = "\npanic_hunt: command hung (>20s): ";
    ssize_t w = write(2, pre, sizeof pre - 1);
    const char *id = g_current_id;
    if (id != NULL) {
        size_t n = 0;
        while (id[n] != '\0' && n < 200) {
            n++;
        }
        w = write(2, id, n);
    }
    w = write(2, "\n", 1);
    (void)w;
    _exit(2);
}

/* ---- session factories -------------------------------------------------------------------- */

static pc_engine_session *empty_session(void)
{
    pc_engine_session *s = NULL;
    if (pc_engine_session_new(&s, NULL) != PC_OK) {
        abort();
    }
    return s;
}

/* The reference fresh(): a small document, a fresh layer, a rectangular selection. */
static pc_engine_session *standard_session(void)
{
    pc_engine_session *s = empty_session();
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (pc_engine_session_execute_json(s, "file.new", "{\"width\":24,\"height\":16}", &out, NULL) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (pc_engine_session_execute_json(s, "layer.new.layer", "{}", &out, NULL) == PC_OK) {
        pc_json_value_fini(&out);
    }
    if (pc_engine_session_execute_json(s, "select.rect", "{\"x\":1,\"y\":1,\"width\":6,\"height\":5}", &out,
                                       NULL) == PC_OK) {
        pc_json_value_fini(&out);
    }
    return s;
}

/* A deeper, hostile document: a group with a masked raster child and a disabled-vector-mask child,
 * several extra layers, a whole-canvas selection, and the active layer pointed at a hidden layer
 * (exercises walk recursion, mask handling and the hidden-target guard). */
static pc_engine_session *rich_session(void)
{
    pc_engine_session *s = empty_session();
    pc_document doc;
    if (pc_document_with_background(&doc, "Rich", pc_size_make(40, 30), PC_COLOR_MODE_RGB,
                                    PC_SAMPLE_TYPE_U8, pc_color_rgba(1.0f, 1.0f, 1.0f, 1.0f), NULL) != PC_OK) {
        return s;
    }
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    /* A group of two children with masks. */
    pc_layer masked, vmasked;
    if (pc_layer_raster(&masked, "Masked", fmt, NULL) == PC_OK) {
        pc_layer_mask_reveal_all(&masked.mask);
        masked.has_mask = true;
    }
    if (pc_layer_raster(&vmasked, "VMasked", fmt, NULL) == PC_OK) {
        vmasked.has_vector_mask = true;
        memset(&vmasked.vector_mask, 0, sizeof vmasked.vector_mask);
        vmasked.vector_mask.enabled = false;
    }
    pc_layer *children = malloc(2 * sizeof *children);
    pc_layer_id gid = 0;
    bool have_group = false;
    if (children != NULL) {
        children[0] = masked;
        children[1] = vmasked;
        pc_layer group;
        if (pc_layer_group(&group, "G", children, 2, NULL) == PC_OK) {
            if (pc_document_insert_above(&doc, NULL, &group, &gid, NULL) == PC_OK) {
                have_group = true;
            }
        }
    }
    /* A hidden extra layer. */
    pc_layer hidden;
    pc_layer_id hid = 0;
    bool have_hidden = false;
    if (pc_layer_raster(&hidden, "Hidden", fmt, NULL) == PC_OK) {
        hidden.visible = false;
        if (pc_document_insert_above(&doc, NULL, &hidden, &hid, NULL) == PC_OK) {
            have_hidden = true;
        }
    }
    if (pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) != PC_OK) {
        return s;
    }
    /* Whole-canvas selection, then aim the active layer at the hidden layer. */
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (pc_engine_session_execute_json(s, "select.all", "{}", &out, NULL) == PC_OK) {
        pc_json_value_fini(&out);
    }
    (void)have_group;
    (void)gid;
    pc_engine_doc_state *d = pc_engine_session_active_mut(s);
    if (d != NULL && have_hidden) {
        d->has_active_layer = true;
        d->active_layer = hid;
    }
    return s;
}

typedef pc_engine_session *(*factory_fn)(void);

/* ---- adversarial parameter battery -------------------------------------------------------- */

/* NULL means "no params" (a null yyjson_val). The rest are parsed once and reused (execute does
 * not mutate params). Covers: empty, the reference's negative/zero/huge/bad-id/empty-collection
 * objects, wrong top-level types, huge and tiny numbers, strings where numbers go, a long points
 * array, and a near-limit deep nesting. */
static const char *const BATTERY_SRC[] = {
    NULL,
    "{}",
    "{\"x\":-9,\"y\":-9,\"radius\":0,\"amount\":0,\"angle\":0,\"opacity\":0,\"scale\":0,\"levels\":0,"
    "\"tolerance\":0,\"gamma\":0,\"layer\":999999,\"channel\":999,\"index\":999999,\"count\":0}",
    "{\"x\":1e5,\"y\":-1e5,\"radius\":300,\"amount\":300,\"angle\":1e5,\"opacity\":-100,\"scale\":-5,"
    "\"gamma\":-1}",
    "{\"points\":[],\"from\":[0,0],\"to\":[0,0],\"stops\":[],\"matrix\":[0,0,0,0,0,0],\"colors\":[],"
    "\"name\":\"\",\"mode\":\"\",\"style\":\"\"}",
    "{\"width\":1000000000,\"height\":1000000000,\"radius\":1000000000,\"count\":1000000000,"
    "\"size\":1000000000}",
    "{\"radius\":1e300,\"amount\":-1e300,\"angle\":1e300,\"opacity\":1e300,\"gamma\":1e-300,"
    "\"scale\":1e300,\"speed\":1e300,\"x\":1e300,\"y\":-1e300}",
    "{\"x\":\"nope\",\"y\":true,\"radius\":\"big\",\"layer\":\"none\",\"color\":12,\"mode\":42,"
    "\"name\":[1,2],\"points\":\"x\",\"opacity\":\"full\"}",
    "[1,2,3]",
    "null",
    "42",
    "true",
    "\"string\"",
    /* A long points array for stroke/path commands. */
    "{\"points\":[[0,0],[1,1],[2,2],[3,3],[4,4],[5,5],[6,6],[7,7],[8,8],[9,9],[10,10],[11,11],"
    "[12,12],[13,13],[14,14],[15,15],[16,16],[17,17],[18,18],[19,19],[20,20],[21,21],[22,22],"
    "[23,23],[-1,-1],[1e9,1e9],[-1e9,-1e9]],\"size\":1e9,\"hardness\":5,\"speed\":1e20}",
};
enum { NBATTERY = sizeof BATTERY_SRC / sizeof BATTERY_SRC[0] };

/* Build the deep-nesting param ("{\"a\":{\"a\":...0...}}", 120 levels < serde's 127 limit). */
static char *make_deep(size_t levels)
{
    char *buf = malloc(levels * 6 + 2);
    if (buf == NULL) {
        return NULL;
    }
    size_t p = 0;
    for (size_t i = 0; i < levels; i++) {
        memcpy(buf + p, "{\"a\":", 5);
        p += 5;
    }
    buf[p++] = '0';
    for (size_t i = 0; i < levels; i++) {
        buf[p++] = '}';
    }
    buf[p] = '\0';
    return buf;
}

static bool valid_status(pc_status s)
{
    switch (s) {
    case PC_OK:
    case PC_ERR_INVALID:
    case PC_ERR_NOMEM:
    case PC_ERR_IO:
    case PC_ERR_FORMAT:
    case PC_ERR_UNSUPPORTED:
    case PC_ERR_LIMIT:
    case PC_ERR_CANCELLED:
    case PC_ERR_NOT_FOUND:
        return true;
    }
    return false;
}

/* Run id with params on a fresh session from `make`; assert it returns a valid status and never
 * crashes. The watchdog is armed around the call. */
static void fuzz_one(factory_fn make, const char *id, yyjson_val *params)
{
    pc_engine_session *s = make();
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    g_current_id = id;
    if (g_armed) {
        (void)alarm(20);
    }
    pc_status rc = pc_engine_session_execute(s, id, params, &out, &err);
    if (g_armed) {
        (void)alarm(0);
    }
    if (!valid_status(rc)) {
        PC_T_FAIL("%s returned a bogus status %d", id, (int)rc);
    }
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

static void fuzz_registry(void)
{
    static const factory_fn FACTORIES[] = {empty_session, standard_session, rich_session};
    enum { NF = sizeof FACTORIES / sizeof FACTORIES[0] };

    /* Parse the battery once. */
    yyjson_doc *docs[NBATTERY] = {0};
    yyjson_val *vals[NBATTERY] = {0};
    for (size_t i = 0; i < NBATTERY; i++) {
        if (BATTERY_SRC[i] == NULL) {
            vals[i] = NULL;
            continue;
        }
        pc_error e = {0};
        if (pc_json_parse(BATTERY_SRC[i], strlen(BATTERY_SRC[i]), &docs[i], &e) != PC_OK) {
            PC_T_FAIL("battery item %zu failed to parse: %s", i, e.msg);
            docs[i] = NULL;
            vals[i] = NULL;
        } else {
            vals[i] = yyjson_doc_get_root(docs[i]);
        }
    }
    /* The deep-nesting value (parsed separately so the depth is explicit). */
    char *deep = make_deep(120);
    yyjson_doc *deep_doc = NULL;
    yyjson_val *deep_val = NULL;
    if (deep != NULL && pc_json_parse(deep, strlen(deep), &deep_doc, NULL) == PC_OK) {
        deep_val = yyjson_doc_get_root(deep_doc);
    }

    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    CHECK(all != NULL && n > 0);
    for (size_t c = 0; c < n; c++) {
        const char *id = all[c].id;
        for (size_t f = 0; f < NF; f++) {
            for (size_t p = 0; p < NBATTERY; p++) {
                fuzz_one(FACTORIES[f], id, vals[p]);
            }
            if (deep_val != NULL) {
                fuzz_one(FACTORIES[f], id, deep_val);
            }
        }
    }
    (void)printf("panic_hunt: fuzzed %zu commands x %d factories x %d params\n", n, NF, NBATTERY + 1);

    for (size_t i = 0; i < NBATTERY; i++) {
        yyjson_doc_free(docs[i]);
    }
    yyjson_doc_free(deep_doc);
    free(deep);
}

int main(int argc, char **argv)
{
    /* Arm the hang watchdog unless the platform lacks it. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    g_armed = (sigaction(SIGALRM, &sa, NULL) == 0);

    /* Never reach a printer while fuzzing file.print / file.printOneCopy. */
    pc_engine_print_disable();

    RUN(fuzz_registry);
    return pc_t_finish();
}
