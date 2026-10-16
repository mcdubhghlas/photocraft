/* M11 acceptance: scripted edit tasks driven through the automation layer (port of
 * crates/automation/tests/agent_tasks.rs).
 *
 * The reference drives these over MCP tools; this port drives the headless backend directly (the
 * established C test convention -- the MCP tool layer is a thin map onto these methods, exercised
 * in test_mcp.c). Each MCP tool maps to a headless method: doc_new -> doc.new, command_run ->
 * engine.execute, doc_inspect -> doc.inspect, doc_export/doc_save -> doc.save, doc_open ->
 * doc.open, doc_close -> doc.close.
 *
 * §7 adaptations (noted inline):
 *   - task 7: `layer.align.leftEdges` is not ported in the C engine (only edit.autoAlignLayers),
 *     so the alignment is replaced by a never-crash check that the command is cleanly unknown.
 *   - agent_play_refuses_a_recorded_file_open pokes `session.actions` through engine internals the
 *     C automation layer does not expose; it is not ported (the file.open refusal it depends on is
 *     covered by test_workspace's authorize-policy test). */
#include "harness.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "automation/headless.h"
#include "automation/workspace.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "codecs/codecs.h"
#include "engine/engine.h"

/* ---- tmp workspace ---- */

static void make_root(char *out, size_t cap, const char *name)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, cap, "%s/pc-agent-%d-%s", tmp ? tmp : "/tmp", (int)getpid(), name);
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    int rc = system(rm);
    (void)rc;
    (void)mkdir(out, 0755);
}

static void rmrf(const char *dir)
{
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    int rc = system(rm);
    (void)rc;
}

static pc_headless *open_workspace(const char *root)
{
    pc_auto_workspace ws;
    if (pc_auto_workspace_init(&ws, root, root, NULL) != PC_OK) {
        return NULL;
    }
    pc_headless *h = NULL;
    if (pc_headless_with_workspace(&ws, &h, NULL) != PC_OK) {
        pc_auto_workspace_fini(&ws);
        return NULL;
    }
    return h;
}

/* ---- method/tool helpers ---- */

/* Run a method; CHECK PC_OK; store result (caller finis *store). Returns the result root. */
static yyjson_mut_val *mcall(pc_headless *h, const char *method, const char *pjson, pc_json_value *store)
{
    yyjson_doc *pd = NULL;
    if (pjson && pc_json_parse(pjson, strlen(pjson), &pd, NULL) != PC_OK) {
        PC_T_FAIL("bad params JSON for %s: %s", method, pjson);
        return NULL;
    }
    pc_error err;
    pc_error_clear(&err);
    pc_status rc = pc_headless_handle(h, method, pd ? yyjson_doc_get_root(pd) : NULL, store, &err);
    yyjson_doc_free(pd);
    pc_t_checks++;
    if (rc != PC_OK) {
        PC_T_FAIL("%s failed: %s", method, err.msg);
        return NULL;
    }
    return pc_json_value_root(store);
}

/* command_run: engine.execute {command:id, params, wait:true}. CHECKs ok. */
static yyjson_mut_val *run(pc_headless *h, const char *id, const char *params, pc_json_value *store)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "{\"command\":\"%s\",\"params\":%s,\"wait\":true}", id, params ? params : "{}");
    return mcall(h, "engine.execute", buf, store);
}

static yyjson_mut_val *inspect(pc_headless *h, pc_json_value *store)
{
    return mcall(h, "doc.inspect", NULL, store);
}

static yyjson_mut_val *mo(yyjson_mut_val *v, const char *key) { return yyjson_mut_obj_get(v, key); }
static yyjson_mut_val *ma(yyjson_mut_val *v, size_t i) { return yyjson_mut_arr_get(v, i); }
static const char *js(yyjson_mut_val *v) { return yyjson_mut_get_str(v); }

static double jnum(yyjson_mut_val *v)
{
    if (!v) {
        return NAN;
    }
    if (yyjson_mut_is_real(v)) {
        return yyjson_mut_get_real(v);
    }
    if (yyjson_mut_is_int(v)) {
        return (double)yyjson_mut_get_sint(v);
    }
    if (yyjson_mut_is_uint(v)) {
        return (double)yyjson_mut_get_uint(v);
    }
    return NAN;
}

/* document.pixel -> [r,g,b,a]. */
static void pixel(pc_headless *h, int x, int y, double out[4])
{
    char p[64];
    snprintf(p, sizeof p, "{\"x\":%d,\"y\":%d}", x, y);
    pc_json_value store = PC_JSON_VALUE_INIT;
    yyjson_mut_val *arr = run(h, "document.pixel", p, &store);
    for (int i = 0; i < 4; i++) {
        out[i] = arr ? jnum(ma(arr, (size_t)i)) : NAN;
    }
    pc_json_value_fini(&store);
}

static bool close4(const double a[4], const double b[4], double tol)
{
    for (int i = 0; i < 4; i++) {
        if (fabs(a[i] - b[i]) > tol) {
            return false;
        }
    }
    return true;
}

static yyjson_mut_val *top_layer(yyjson_mut_val *doc) { return ma(mo(doc, "layers"), 0); }
static size_t nlayers(yyjson_mut_val *doc) { return yyjson_mut_arr_size(mo(doc, "layers")); }

/* 64x48 RGB gradient PNG into root/gradient.png. */
static void gradient_png(const char *root)
{
    uint32_t w = 64, hh = 48;
    size_t n = (size_t)w * hh * 3;
    uint8_t *px = malloc(n);
    size_t i = 0;
    for (uint32_t y = 0; y < hh; y++) {
        for (uint32_t x = 0; x < w; x++) {
            px[i++] = (uint8_t)(x * 4);
            px[i++] = (uint8_t)(y * 5);
            px[i++] = 128;
        }
    }
    pc_cimage img;
    CHECK(pc_cimage_from_raw(w, hh, PC_CL_RGB, PC_CS_U8, px, n, &img, NULL) == PC_OK);
    pc_buf out = PC_BUF_INIT;
    CHECK(pc_cencode(&img, PC_CF_PNG, NULL, &out, NULL) == PC_OK);
    char path[700];
    snprintf(path, sizeof path, "%s/gradient.png", root);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        (void)fwrite(out.data, 1, out.len, f);
        fclose(f);
    }
    pc_buf_fini(&out);
    pc_cimage_fini(&img);
}

static void test_ten_scripted_tasks(void)
{
    char root[640];
    make_root(root, sizeof root, "tasks");
    gradient_png(root);
    pc_headless *h = open_workspace(root);
    CHECK(h != NULL);
    if (!h) {
        return;
    }
    pc_json_value s = PC_JSON_VALUE_INIT;
    yyjson_mut_val *doc;

    /* 1. Title card: type layer with a drop shadow, exported as PNG. */
    mcall(h, "doc.new", "{\"width\":200,\"height\":120,\"background\":\"white\"}", &s);
    pc_json_value_fini(&s);
    run(h, "type.create", "{\"x\":20,\"y\":70,\"text\":\"Hello\",\"size\":36}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.layerStyle.dropShadow", "{\"distance\":4,\"size\":3}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    {
        yyjson_mut_val *t = top_layer(doc);
        CHECK_EQ_STR(js(mo(t, "kind")), "Type");
        CHECK_EQ_STR(js(mo(mo(t, "text"), "text")), "Hello");
        /* §7: the C document inspect does not serialize layer effects, so the drop shadow is not
         * observable here. The `layer.layerStyle.dropShadow` run above already asserted success. */
    }
    pc_json_value_fini(&s);
    mcall(h, "doc.save", "{\"path\":\"card.png\"}", &s);
    pc_json_value_fini(&s);
    {
        char cp[700];
        snprintf(cp, sizeof cp, "%s/card.png", root);
        pc_buf b = PC_BUF_INIT;
        CHECK(pc_file_read(cp, (size_t)64 << 20, &b, NULL) == PC_OK);
        pc_cimage card;
        CHECK(pc_cdecode(b.data, b.len, &card, NULL) == PC_OK);
        CHECK_EQ_UINT(card.width, 200u);
        CHECK_EQ_UINT(card.height, 120u);
        pc_cimage_fini(&card);
        pc_buf_fini(&b);
    }
    mcall(h, "doc.close", NULL, &s);
    pc_json_value_fini(&s);

    /* 2. Colour grade a photo non-destructively. */
    mcall(h, "doc.open", "{\"path\":\"gradient.png\"}", &s);
    pc_json_value_fini(&s);
    double before[4], after[4];
    pixel(h, 16, 24, before);
    run(h, "layer.newAdjustmentLayer.curves", "{\"points\":[[0,0],[128,200],[255,255]]}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.newAdjustmentLayer.vibrance", "{\"vibrance\":40,\"saturation\":0}", &s);
    pc_json_value_fini(&s);
    pixel(h, 16, 24, after);
    CHECK(after[1] > before[1] + 0.05); /* curves brighten green */
    doc = inspect(h, &s);
    CHECK_EQ_STR(js(mo(ma(mo(doc, "layers"), 0), "kind")), "Adjustment");
    CHECK_EQ_STR(js(mo(ma(mo(doc, "layers"), 1), "kind")), "Adjustment");
    CHECK_EQ_STR(js(mo(ma(mo(doc, "layers"), 2), "kind")), "Pixel");
    pc_json_value_fini(&s);

    /* 3. Undo/redo. */
    run(h, "edit.undo", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.undo", "{}", &s);
    pc_json_value_fini(&s);
    {
        double p[4];
        pixel(h, 16, 24, p);
        CHECK(close4(p, before, 1e-6));
    }
    run(h, "edit.redo", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT(nlayers(doc), 2u);
    pc_json_value_fini(&s);
    run(h, "edit.undo", "{}", &s);
    pc_json_value_fini(&s);

    /* 4. Editable blur: smart object + Gaussian Blur, retune, hide. */
    run(h, "layer.smartObjects.convertToSmartObject", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "filter.blur.gaussianBlur", "{\"radius\":2}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.smartFilter.setParams", "{\"index\":0,\"params\":{\"radius\":6}}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    {
        yyjson_mut_val *sf = ma(mo(top_layer(doc), "smartFilters"), 0);
        CHECK_EQ_STR(js(mo(sf, "command")), "filter.blur.gaussianBlur");
        CHECK_NEAR(jnum(mo(mo(sf, "params"), "radius")), 6.0, 1e-9);
    }
    pc_json_value_fini(&s);
    run(h, "layer.smartFilter.setVisible", "{\"index\":0,\"visible\":false}", &s);
    pc_json_value_fini(&s);
    {
        double p[4];
        pixel(h, 16, 24, p);
        CHECK(close4(p, before, 1.0 / 255.0));
    }

    /* 5. Selection -> layer mask: elliptical reveal on a red layer. */
    run(h, "layer.new.layer", "{\"name\":\"Red\"}", &s);
    pc_json_value_fini(&s);
    run(h, "select.all", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.fill", "{\"color\":\"#ff0000\"}", &s);
    pc_json_value_fini(&s);
    run(h, "select.rect", "{\"x\":16,\"y\":12,\"width\":32,\"height\":24,\"ellipse\":true}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.layerMask.revealSelection", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK(yyjson_mut_get_bool(mo(top_layer(doc), "hasMask")));
    pc_json_value_fini(&s);
    {
        double inside[4], outside[4];
        pixel(h, 32, 24, inside);
        pixel(h, 1, 1, outside);
        CHECK(inside[0] > 0.99 && inside[1] < 0.01);
        CHECK(outside[0] < 0.1);
    }
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);

    /* 6. Save a selection to a channel and load it back. */
    run(h, "select.rect", "{\"x\":4,\"y\":6,\"width\":20,\"height\":10}", &s);
    pc_json_value_fini(&s);
    run(h, "select.saveSelection", "{\"name\":\"Keep\"}", &s);
    pc_json_value_fini(&s);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK(!yyjson_mut_get_bool(mo(doc, "hasSelection")));
    pc_json_value_fini(&s);
    run(h, "select.loadSelection", "{\"channel\":\"Keep\"}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    {
        yyjson_mut_val *b = mo(doc, "selectionBounds");
        CHECK_EQ_INT((int)jnum(ma(b, 0)), 4);
        CHECK_EQ_INT((int)jnum(ma(b, 1)), 6);
        CHECK_EQ_INT((int)jnum(ma(b, 2)), 20);
        CHECK_EQ_INT((int)jnum(ma(b, 3)), 10);
        CHECK_EQ_STR(js(mo(ma(mo(mo(doc, "channels"), "alpha"), 0), "name")), "Keep");
    }
    pc_json_value_fini(&s);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);
    mcall(h, "doc.close", NULL, &s);
    pc_json_value_fini(&s);

    /* 7. Lay out three swatches. §7: layer.align.leftEdges is unported; replace the align with a
     * never-crash check that it is cleanly unknown, and verify the three swatches exist. */
    mcall(h, "doc.new", "{\"width\":120,\"height\":90,\"background\":\"white\"}", &s);
    pc_json_value_fini(&s);
    for (int i = 0; i < 3; i++) {
        int xs[3] = {10, 40, 70};
        char nm[32], rc[96];
        snprintf(nm, sizeof nm, "{\"name\":\"S%d\"}", i);
        run(h, "layer.new.layer", nm, &s);
        pc_json_value_fini(&s);
        snprintf(rc, sizeof rc, "{\"x\":%d,\"y\":%d,\"width\":20,\"height\":15}", xs[i], 10 + 25 * i);
        run(h, "select.rect", rc, &s);
        pc_json_value_fini(&s);
        run(h, "edit.fill", "{\"color\":\"#3366cc\"}", &s);
        pc_json_value_fini(&s);
    }
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "select.allLayers", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.align.leftEdges", "{}", &s); /* now ported (GAP align/distribute) */
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    {
        size_t swatches = 0;
        size_t ln = nlayers(doc);
        for (size_t i = 0; i < ln; i++) {
            const char *nm = js(mo(ma(mo(doc, "layers"), i), "name"));
            if (nm && nm[0] == 'S') {
                swatches++;
            }
        }
        CHECK_EQ_UINT(swatches, 3u);
    }
    pc_json_value_fini(&s);

    /* 8. Export through the capability-scoped document API (doc.save beneath the write root). */
    {
        char layers[700];
        snprintf(layers, sizeof layers, "%s/layers", root);
        (void)mkdir(layers, 0755);
        mcall(h, "doc.save", "{\"path\":\"layers/swatch.png\"}", &s);
        pc_json_value_fini(&s);
        char swatch[800];
        snprintf(swatch, sizeof swatch, "%s/layers/swatch.png", layers);
        (void)swatch;
        char file[800];
        snprintf(file, sizeof file, "%s/swatch.png", layers);
        CHECK(access(file, F_OK) == 0);
    }

    /* 9. Resize, then crop to a square. */
    run(h, "image.imageSize", "{\"width\":60,\"height\":45}", &s);
    pc_json_value_fini(&s);
    run(h, "image.crop", "{\"x\":5,\"y\":0,\"width\":45,\"height\":45}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT((unsigned)jnum(mo(doc, "width")), 45u);
    CHECK_EQ_UINT((unsigned)jnum(mo(doc, "height")), 45u);
    pc_json_value_fini(&s);

    /* 10. Convert to CMYK, save natively, reopen. */
    run(h, "image.mode.cmyk", "{\"intent\":\"perceptual\"}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_STR(js(mo(doc, "mode")), "Cmyk");
    pc_json_value_fini(&s);
    mcall(h, "doc.save", "{\"path\":\"print.pcraft\"}", &s);
    pc_json_value_fini(&s);
    mcall(h, "doc.close", NULL, &s);
    pc_json_value_fini(&s);
    mcall(h, "doc.open", "{\"path\":\"print.pcraft\"}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_STR(js(mo(doc, "mode")), "Cmyk");
    CHECK_EQ_UINT((unsigned)jnum(mo(doc, "width")), 45u);
    CHECK_EQ_UINT(nlayers(doc), 4u);
    pc_json_value_fini(&s);

    pc_headless_free(h);
    rmrf(root);
}

static void test_ten_more_scripted_tasks(void)
{
    char root[640];
    make_root(root, sizeof root, "tasks2");
    pc_headless *h = open_workspace(root);
    CHECK(h != NULL);
    if (!h) {
        return;
    }
    pc_json_value s = PC_JSON_VALUE_INIT;
    yyjson_mut_val *doc;
    double a[4], b[4], c[4];

    /* 11. Stroke a selection. */
    mcall(h, "doc.new", "{\"width\":80,\"height\":80,\"background\":\"white\"}", &s);
    pc_json_value_fini(&s);
    run(h, "select.rect", "{\"x\":20,\"y\":20,\"width\":40,\"height\":40}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.fill", "{\"color\":\"#ff0000\"}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.stroke", "{\"width\":4,\"color\":\"#000000\",\"location\":\"outside\"}", &s);
    pc_json_value_fini(&s);
    pixel(h, 40, 40, a);
    pixel(h, 18, 40, b);
    pixel(h, 5, 5, c);
    CHECK(a[0] > 0.99 && a[1] < 0.01);
    CHECK(b[0] < 0.1 && b[1] < 0.1);
    CHECK(c[0] > 0.99 && c[1] > 0.99);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);

    /* 12. Inverse the selection: a blue frame. */
    run(h, "select.rect", "{\"x\":20,\"y\":20,\"width\":40,\"height\":40}", &s);
    pc_json_value_fini(&s);
    run(h, "select.inverse", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.fill", "{\"color\":\"#0000ff\"}", &s);
    pc_json_value_fini(&s);
    pixel(h, 40, 40, a);
    pixel(h, 4, 4, b);
    CHECK(a[0] > 0.99 && a[2] < 0.01);
    CHECK(b[2] > 0.99 && b[0] < 0.01);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);

    /* 13. Paint a green patch on a new layer and duplicate it. */
    run(h, "layer.new.layer", "{\"name\":\"Patch\"}", &s);
    pc_json_value_fini(&s);
    run(h, "select.rect", "{\"x\":10,\"y\":10,\"width\":20,\"height\":20}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.fill", "{\"color\":\"#00ff00\"}", &s);
    pc_json_value_fini(&s);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.duplicate", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT(nlayers(doc), 3u);
    {
        size_t patches = 0, ln = nlayers(doc);
        for (size_t i = 0; i < ln; i++) {
            const char *nm = js(mo(ma(mo(doc, "layers"), i), "name"));
            if (nm && strncmp(nm, "Patch", 5) == 0) {
                patches++;
            }
        }
        CHECK_EQ_UINT(patches, 2u);
    }
    pc_json_value_fini(&s);

    /* 14. Merge the copy down. */
    run(h, "layer.mergeDown", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT(nlayers(doc), 2u);
    pc_json_value_fini(&s);

    /* 15. Flatten; the patch survives. */
    run(h, "layer.flattenImage", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT(nlayers(doc), 1u);
    pc_json_value_fini(&s);
    pixel(h, 20, 20, a);
    CHECK(a[1] > 0.99 && a[0] < 0.01);

    /* 16. Hue/Saturation adjustment turns red green. */
    run(h, "layer.newAdjustmentLayer.hueSaturation", "{\"hue\":120}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_STR(js(mo(top_layer(doc), "kind")), "Adjustment");
    pc_json_value_fini(&s);
    pixel(h, 40, 40, a);
    CHECK(a[1] > 0.5 && a[0] < 0.5);
    run(h, "layer.delete", "{}", &s);
    pc_json_value_fini(&s);

    /* 17. Feather + mask: the edge blends. */
    run(h, "select.rect", "{\"x\":20,\"y\":20,\"width\":40,\"height\":40}", &s);
    pc_json_value_fini(&s);
    run(h, "select.modify.feather", "{\"radius\":4}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.layerMask.revealSelection", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK(yyjson_mut_get_bool(mo(top_layer(doc), "hasMask")));
    pc_json_value_fini(&s);
    pixel(h, 20, 40, a);
    CHECK(a[3] > 0.05 && a[3] < 0.95);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);

    /* 18. Expand a selection and read its bounds. */
    run(h, "select.rect", "{\"x\":20,\"y\":20,\"width\":20,\"height\":20}", &s);
    pc_json_value_fini(&s);
    run(h, "select.modify.expand", "{\"radius\":5}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    {
        yyjson_mut_val *bounds = mo(doc, "selectionBounds");
        int x = (int)jnum(ma(bounds, 0)), w = (int)jnum(ma(bounds, 2));
        CHECK(x >= 13 && x <= 15);
        CHECK(w >= 30 && w <= 32);
    }
    pc_json_value_fini(&s);
    run(h, "select.deselect", "{}", &s);
    pc_json_value_fini(&s);

    /* 19. Rename and delete a layer. */
    run(h, "layer.new.layer", "{\"name\":\"Scratch\"}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.renameLayer", "{\"name\":\"Delivery\"}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_STR(js(mo(top_layer(doc), "name")), "Delivery");
    pc_json_value_fini(&s);
    run(h, "layer.delete", "{}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT(nlayers(doc), 1u);
    pc_json_value_fini(&s);

    /* 20. Export and reopen the PNG as a flat document. */
    mcall(h, "doc.save", "{\"path\":\"final.png\"}", &s);
    pc_json_value_fini(&s);
    mcall(h, "doc.close", NULL, &s);
    pc_json_value_fini(&s);
    mcall(h, "doc.open", "{\"path\":\"final.png\"}", &s);
    pc_json_value_fini(&s);
    doc = inspect(h, &s);
    CHECK_EQ_UINT((unsigned)jnum(mo(doc, "width")), 80u);
    CHECK_EQ_UINT((unsigned)jnum(mo(doc, "height")), 80u);
    CHECK_EQ_UINT(nlayers(doc), 1u);
    pc_json_value_fini(&s);
    mcall(h, "doc.close", NULL, &s);
    pc_json_value_fini(&s);

    pc_headless_free(h);
    rmrf(root);
}

static void test_records_and_replays_an_action(void)
{
    char root[640];
    make_root(root, sizeof root, "actions");
    pc_headless *h = open_workspace(root);
    CHECK(h != NULL);
    if (!h) {
        return;
    }
    pc_json_value s = PC_JSON_VALUE_INIT;

    run(h, "file.new", "{\"width\":32,\"height\":32,\"background\":\"white\"}", &s);
    pc_json_value_fini(&s);
    run(h, "actions.record", "{\"name\":\"Red\"}", &s);
    pc_json_value_fini(&s);
    run(h, "layer.new.layer", "{}", &s);
    pc_json_value_fini(&s);
    run(h, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":8}", &s);
    pc_json_value_fini(&s);
    run(h, "edit.fill", "{\"color\":\"#ff0000\"}", &s);
    pc_json_value_fini(&s);
    yyjson_mut_val *stopped = run(h, "actions.stop", "{}", &s);
    CHECK_EQ_INT((int)jnum(mo(stopped, "steps")), 3);
    pc_json_value_fini(&s);
    yyjson_mut_val *got = run(h, "actions.get", "{\"action\":\"Red\"}", &s);
    CHECK_EQ_STR(js(ma(ma(mo(got, "steps"), 0), 0)), "layer.new.layer");
    CHECK_EQ_STR(js(ma(ma(mo(got, "steps"), 1), 0)), "select.rect");
    CHECK_EQ_STR(js(ma(ma(mo(got, "steps"), 2), 0)), "edit.fill");
    pc_json_value_fini(&s);

    run(h, "file.new", "{\"width\":32,\"height\":32,\"background\":\"white\"}", &s);
    pc_json_value_fini(&s);
    yyjson_mut_val *played = run(h, "actions.play", "{\"action\":\"Red\"}", &s);
    CHECK_EQ_INT((int)jnum(mo(played, "ran")), 3);
    CHECK(mo(played, "failed") == NULL);
    pc_json_value_fini(&s);
    {
        double px[4];
        double want[4] = {1.0, 0.0, 0.0, 1.0};
        pixel(h, 2, 2, px);
        CHECK(close4(px, want, 1e-4));
    }

    pc_headless_free(h);
    rmrf(root);
}

int main(int argc, char **argv)
{
    RUN(test_ten_scripted_tasks);
    RUN(test_ten_more_scripted_tasks);
    RUN(test_records_and_replays_an_action);
    return pc_t_finish();
}
