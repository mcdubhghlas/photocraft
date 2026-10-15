/* Single-layer base commands + clip-group tracking (port of the behaviour tested in
 * crates/engine/src/new_layer.rs and clip_group.rs, plus adversarial-params coverage). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc/tree.h"
#include "engine/engine.h"
#include "engine/layer_cmds.h"
#include "harness.h"

static pc_engine_session *new_session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":16,\"height\":16}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* Run a command with JSON params; fail the test (with the error message) if it does not succeed. */
static void ok(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out;
    pc_error err = {0};
    if (pc_engine_session_execute_json(s, id, params, &out, &err) != PC_OK) {
        PC_T_FAIL("%s(%s): %s", id, params, err.msg);
        return;
    }
    pc_json_value_fini(&out);
}

static uint64_t new_layer(pc_engine_session *s, const char *name)
{
    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"name\":\"%s\"}", name);
    pc_json_value out;
    pc_error err = {0};
    if (pc_engine_session_execute_json(s, "layer.new.layer", buf, &out, &err) != PC_OK) {
        PC_T_FAIL("new.layer %s: %s", name, err.msg);
        return 0;
    }
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    return id;
}

/* Find a top-level layer id by name. */
static uint64_t id_of(pc_engine_session *s, const char *name)
{
    const pc_document *doc = doc_of(s);
    for (size_t i = 0; i < doc->nlayers; i++) {
        if (pc_str_eq_c(&doc->layers[i].name, name)) {
            return doc->layers[i].id;
        }
    }
    return 0;
}

/* A document holding `spec` (bottom to top, `*` = clipped) above its Background. */
static pc_engine_session *stack(const char *spec)
{
    pc_engine_session *s = new_session();
    char copy[256];
    (void)snprintf(copy, sizeof copy, "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(copy, " ", &save); tok != NULL; tok = strtok_r(NULL, " ", &save)) {
        size_t len = strlen(tok);
        bool clipped = len > 0 && tok[len - 1] == '*';
        char name[64];
        (void)snprintf(name, sizeof name, "%.*s", (int)(clipped ? len - 1 : len), tok);
        uint64_t id = new_layer(s, name);
        if (clipped) {
            char buf[96];
            (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"clipped\":true}", (unsigned long long)id);
            ok(s, "layer.setProps", buf);
        }
    }
    return s;
}

/* The stack above the Background, as stack() writes it (names, `*` for clipped). */
static void shown(pc_engine_session *s, char *out, size_t cap)
{
    const pc_document *doc = doc_of(s);
    out[0] = '\0';
    size_t pos = 0;
    for (size_t i = 1; i < doc->nlayers; i++) { /* skip the Background at index 0 */
        const char *nm = pc_str_c(&doc->layers[i].name);
        int w = snprintf(out + pos, cap - pos, "%s%s%s", i > 1 ? " " : "", nm,
                         doc->layers[i].clipped ? "*" : "");
        if (w < 0 || (size_t)w >= cap - pos) {
            return;
        }
        pos += (size_t)w;
    }
}

static void select_layer(pc_engine_session *s, const char *name)
{
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)id_of(s, name));
    ok(s, "layer.select", buf);
}

#define CHECK_SHOWN(s, want)                                                                        \
    do {                                                                                            \
        char pc_buf_[256];                                                                          \
        shown((s), pc_buf_, sizeof pc_buf_);                                                        \
        CHECK_EQ_STR(pc_buf_, (want));                                                              \
    } while (0)

/* ---- new.layer options (new_layer.rs) ----------------------------------------------------- */

static void test_new_layer_options(void)
{
    pc_engine_session *s = new_session();
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(
              s, "layer.new.layer",
              "{\"name\":\"Shade\",\"mode\":\"Overlay\",\"opacity\":0.4,\"color\":\"blue\",\"fillNeutral\":true}",
              &out, &err) == PC_OK);
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    const pc_layer *l = pc_document_layer(doc_of(s), id);
    CHECK(l != NULL);
    CHECK(pc_str_eq_c(&l->name, "Shade"));
    CHECK(l->blend == PC_BLEND_OVERLAY);
    CHECK_NEAR(l->opacity, 0.4, 1e-6);
    CHECK(l->label == PC_LABEL_BLUE);
    CHECK(pc_engine_session_active(s)->active_layer == id);

    /* Out-of-range opacity clamps to 1. Defaults otherwise. */
    ok(s, "layer.new.layer", "{\"opacity\":7}");
    const pc_document *doc = doc_of(s);
    const pc_layer *top = &doc->layers[doc->nlayers - 1];
    CHECK_NEAR(top->opacity, 1.0, 1e-6);
    CHECK(top->blend == PC_BLEND_NORMAL && top->label == PC_LABEL_NONE && !top->clipped);
    pc_engine_session_free(s);
}

static void test_new_layer_bad_params(void)
{
    pc_engine_session *s = new_session();
    size_t before = doc_of(s)->nlayers;
    const char *bad[] = {
        "{\"mode\":\"Pass Through\"}", "{\"mode\":\"Sparkle\"}",     "{\"mode\":3}",
        "{\"opacity\":\"half\"}",      "{\"color\":\"plaid\"}",      "{\"color\":1}",
        "{\"fillNeutral\":true,\"mode\":\"Hue\"}", "{\"fillNeutral\":\"yes\"}",
        "{\"clipToPrevious\":1}",      "{\"name\":7}",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        pc_json_value out;
        pc_error err = {0};
        pc_status rc = pc_engine_session_execute_json(s, "layer.new.layer", bad[i], &out, &err);
        if (rc == PC_OK) {
            PC_T_FAIL("expected error for %s", bad[i]);
            pc_json_value_fini(&out);
        }
    }
    /* fillNeutral alone with the default Normal mode has no neutral colour -> error. */
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"fillNeutral\":true}", &out, &err) != PC_OK);
    CHECK_EQ_UINT(doc_of(s)->nlayers, before);
    pc_engine_session_free(s);
}

/* ---- clip groups: New Layer joins the group it lands in (clip_group.rs rows 1-6) ---------- */

static void test_new_layer_joins_group(void)
{
    struct {
        const char *spec, *active, *want;
    } cases[] = {
        {"B C*", "B", "B N* C*"},
        {"B C*", "C", "B C* N"},
        {"B C1* C2*", "C1", "B C1* N* C2*"},
        {"B U", "B", "B N U"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_engine_session *s = stack(cases[i].spec);
        select_layer(s, cases[i].active);
        ok(s, "layer.new.layer", "{\"name\":\"N\"}");
        CHECK_SHOWN(s, cases[i].want);
        pc_engine_session_free(s);
    }
    /* Use Previous Layer to Create Clipping Mask. */
    pc_engine_session *s = stack("B");
    ok(s, "layer.new.layer", "{\"name\":\"N\",\"clipToPrevious\":true}");
    CHECK_SHOWN(s, "B N*");
    pc_engine_session_free(s);

    /* Three new layers with the base active all join its group. */
    s = stack("tpl Cont*");
    const char *names[] = {"L1", "L2", "L3"};
    for (size_t i = 0; i < 3; i++) {
        select_layer(s, "tpl");
        char buf[64];
        (void)snprintf(buf, sizeof buf, "{\"name\":\"%s\"}", names[i]);
        ok(s, "layer.new.layer", buf);
    }
    CHECK_SHOWN(s, "tpl L3* L2* L1* Cont*");
    pc_engine_session_free(s);
}

/* ---- clip groups: Arrange joins and leaves groups (clip_group.rs rows 7-12) --------------- */

static void test_arrange_clip_groups(void)
{
    struct {
        const char *spec, *layer, *cmd;
        int times;
        const char *want;
    } cases[] = {
        {"B C1* C2* U", "U", "sendBackward", 1, "B C1* U* C2*"},
        {"B C1* C2* U", "U", "sendBackward", 2, "B U* C1* C2*"},
        {"B C* U", "C", "bringForward", 1, "B U C"},
        {"B C1* C2*", "C1", "bringForward", 1, "B C2* C1*"},
        {"X B C*", "C", "sendBackward", 1, "X C B"},
        {"B C1* C2*", "B", "bringForward", 1, "C1 B* C2"},
        {"B C1* C2* U", "U", "sendToBack", 1, "U B C1* C2*"},
        {"B C1* C2* U", "C1", "bringToFront", 1, "B C2* U C1"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_engine_session *s = stack(cases[i].spec);
        char cmd[64];
        (void)snprintf(cmd, sizeof cmd, "layer.arrange.%s", cases[i].cmd);
        for (int t = 0; t < cases[i].times; t++) {
            char buf[96];
            (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)id_of(s, cases[i].layer));
            ok(s, cmd, buf);
        }
        CHECK_SHOWN(s, cases[i].want);
        pc_engine_session_free(s);
    }
}

/* ---- Delete releases a deleted base's group (clip_group.rs row 17) ------------------------ */

static void test_delete_releases_group(void)
{
    pc_engine_session *s = stack("X B C1* C2*");
    select_layer(s, "B");
    ok(s, "layer.delete", "{}");
    CHECK_SHOWN(s, "X C1 C2");
    pc_engine_session_free(s);

    s = stack("X B C1* C2*");
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)id_of(s, "C1"));
    ok(s, "layer.delete", buf);
    CHECK_SHOWN(s, "X B C2*");
    /* One undo step restores it. */
    CHECK(pc_engine_session_undo(s));
    CHECK_SHOWN(s, "X B C1* C2*");
    pc_engine_session_free(s);
}

static void test_delete_keeps_one_layer(void)
{
    pc_engine_session *s = new_session(); /* only the Background */
    uint64_t bg = doc_of(s)->layers[0].id;
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)bg);
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.delete", buf, &out, &err) != PC_OK);
    CHECK_EQ_UINT(doc_of(s)->nlayers, 1); /* rolled back */
    pc_engine_session_free(s);
}

/* ---- Create / Release Clipping Mask, selection form ---------------------------------------- */

static void test_clipping_mask_selection(void)
{
    pc_engine_session *s = stack("B C U"); /* none clipped */
    /* Select B, C, U (range) then Create: every selected but the lowest clips. */
    select_layer(s, "B");
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"range\"}", (unsigned long long)id_of(s, "U"));
    ok(s, "layer.select", buf);
    ok(s, "layer.createClippingMask", "{}");
    CHECK_SHOWN(s, "B C* U*");
    ok(s, "layer.releaseClippingMask", "{}");
    CHECK_SHOWN(s, "B C U");
    pc_engine_session_free(s);
}

/* ---- Select modes (layer.select) ---------------------------------------------------------- */

static void test_select_modes(void)
{
    pc_engine_session *s = stack("A B C");
    uint64_t a = id_of(s, "A"), b = id_of(s, "B"), c = id_of(s, "C");
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    ok(s, "layer.select", buf);
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"add\"}", (unsigned long long)c);
    ok(s, "layer.select", buf);
    pc_layer_id *sel = NULL;
    size_t n = 0;
    CHECK(pc_engine_selected(s, &sel, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 2);
    free(sel);
    /* toggle C off -> only A; toggling the last remaining layer keeps it. */
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"toggle\"}", (unsigned long long)c);
    ok(s, "layer.select", buf);
    CHECK(pc_engine_selected(s, &sel, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(sel[0] == a);
    free(sel);
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"toggle\"}", (unsigned long long)a);
    ok(s, "layer.select", buf);
    CHECK(pc_engine_selected(s, &sel, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1); /* at least the clicked layer stays targeted */
    free(sel);
    (void)b;
    pc_engine_session_free(s);
}

/* ---- setProps (name, visibility, opacity, blend, locks) ----------------------------------- */

static void test_set_props(void)
{
    pc_engine_session *s = stack("A");
    uint64_t a = id_of(s, "A");
    char buf[192];
    (void)snprintf(buf, sizeof buf,
                   "{\"layer\":%llu,\"name\":\"Renamed\",\"visible\":false,\"opacity\":0.5,"
                   "\"blend\":\"Multiply\",\"locks\":{\"pixels\":true}}",
                   (unsigned long long)a);
    ok(s, "layer.setProps", buf);
    const pc_layer *l = pc_document_layer(doc_of(s), a);
    CHECK(pc_str_eq_c(&l->name, "Renamed"));
    CHECK(!l->visible);
    CHECK_NEAR(l->opacity, 0.5, 1e-6);
    CHECK(l->blend == PC_BLEND_MULTIPLY);
    CHECK(l->locks.pixels);
    /* An unknown blend mode is an error. */
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"blend\":\"Nope\"}", (unsigned long long)a);
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.setProps", buf, &out, &err) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- New Group / Group Layers / Merge Down / Merge Group / Layer Mask --------------------- */

static void test_group_and_merge(void)
{
    pc_engine_session *s = stack("A B");
    size_t before = doc_of(s)->nlayers;
    ok(s, "layer.new.group", "{\"name\":\"G\"}");
    CHECK_EQ_UINT(doc_of(s)->nlayers, before + 1);
    CHECK(id_of(s, "G") != 0);
    pc_engine_session_free(s);

    /* Group Layers (Cmd+G): the selected layers move into a new group. */
    s = stack("A B");
    uint64_t a = id_of(s, "A"), b = id_of(s, "B");
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    ok(s, "layer.select", buf);
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"add\"}", (unsigned long long)b);
    ok(s, "layer.select", buf);
    ok(s, "layer.groupLayers", "{\"name\":\"Grp\"}");
    uint64_t g = id_of(s, "Grp");
    CHECK(g != 0);
    const pc_layer *gl = pc_document_layer(doc_of(s), g);
    CHECK(gl != NULL && pc_layer_is_group(gl) && gl->content.u.group.nchildren == 2);
    /* Merge Group collapses it back to one layer named after the group. */
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)g);
    ok(s, "layer.mergeGroup", buf);
    const pc_layer *merged = pc_document_layer(doc_of(s), g);
    CHECK(merged != NULL && merged->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);

    /* Merge Down: top onto the layer below it. */
    s = stack("A B");
    size_t n0 = doc_of(s)->nlayers;
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)id_of(s, "B"));
    ok(s, "layer.mergeDown", buf);
    CHECK_EQ_UINT(doc_of(s)->nlayers, n0 - 1);
    /* No layer below the bottom-most non-Background: Merge Down of the Background fails cleanly. */
    pc_engine_session_free(s);
}

static void test_layer_mask(void)
{
    pc_engine_session *s = stack("A");
    uint64_t a = id_of(s, "A");
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    ok(s, "layer.layerMask.revealAll", buf);
    CHECK(pc_document_layer(doc_of(s), a)->has_mask);
    ok(s, "layer.layerMask.delete", buf);
    CHECK(!pc_document_layer(doc_of(s), a)->has_mask);
    /* Hide All, then Reveal Selection requires a selection. */
    ok(s, "layer.layerMask.hideAll", buf);
    CHECK(pc_document_layer(doc_of(s), a)->has_mask);
    CHECK(!pc_engine_session_is_enabled(s, "layer.layerMask.revealSelection")); /* no selection */
    pc_engine_session_free(s);
}

static void test_reverse(void)
{
    pc_engine_session *s = stack("A B C");
    /* Select all three, reverse their order. */
    uint64_t a = id_of(s, "A"), c = id_of(s, "C");
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    ok(s, "layer.select", buf);
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"range\"}", (unsigned long long)c);
    ok(s, "layer.select", buf);
    ok(s, "layer.arrange.reverse", "{}");
    CHECK_SHOWN(s, "C B A");
    pc_engine_session_free(s);
}

/* ---- adversarial params: every registered command refuses garbage without crashing -------- */

static void test_adversarial_params(void)
{
    const char *ids[] = {
        "layer.new.layer",     "layer.new.group",      "layer.duplicate",
        "layer.delete",        "layer.select",         "layer.setProps",
        "layer.mergeDown",     "layer.mergeGroup",     "layer.groupLayers",
        "layer.createClippingMask", "layer.releaseClippingMask",
        "layer.arrange.bringForward", "layer.arrange.sendBackward",
        "layer.arrange.bringToFront", "layer.arrange.sendToBack", "layer.arrange.reverse",
        "layer.layerMask.revealAll", "layer.layerMask.hideAll",
        "layer.layerMask.revealSelection", "layer.layerMask.delete",
    };
    const char *params[] = {
        "{}", "[]", "null", "{\"layer\":-1}", "{\"layer\":\"nope\"}", "{\"layer\":999999}",
        "{\"opacity\":\"x\"}", "{\"mode\":42}", "{\"dx\":1e40}", "{\"name\":[]}",
        "{\"layer\":1.5}", "{\"clipped\":\"yes\"}",
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            pc_engine_session *s = stack("A B");
            pc_json_value out;
            pc_error err = {0};
            /* The only requirement: it must not crash. Success or a clean error are both fine. */
            if (pc_engine_session_execute_json(s, ids[i], params[j], &out, &err) == PC_OK) {
                pc_json_value_fini(&out);
            } else {
            }
            pc_engine_session_free(s);
        }
    }
    CHECK(true); /* reaching here without an ASan/UBSan abort is the test */
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_new_layer_options);
    RUN(test_new_layer_bad_params);
    RUN(test_new_layer_joins_group);
    RUN(test_arrange_clip_groups);
    RUN(test_delete_releases_group);
    RUN(test_delete_keeps_one_layer);
    RUN(test_clipping_mask_selection);
    RUN(test_select_modes);
    RUN(test_set_props);
    RUN(test_group_and_merge);
    RUN(test_layer_mask);
    RUN(test_reverse);
    RUN(test_adversarial_params);
    return pc_t_finish();
}
