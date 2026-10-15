/* E11 Type menu (type_extra_cmds.c): anti-alias/orientation/opentype/convert/warp/lorem/update/
 * fonts/defaults + adversarial. type.attachToPath/detachFromPath are deferred (not registered). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(300, 160), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status ex(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    return pc_engine_session_execute_json(s, id, json, out, NULL);
}

static uint64_t uint_of(pc_json_value *o, const char *k)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(o), k);
    return v != NULL ? yyjson_mut_get_uint(v) : 0;
}

static uint64_t create(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.create", json, &out) == PC_OK);
    uint64_t id = uint_of(&out, "layer");
    pc_json_value_fini(&out);
    return id;
}

static const pc_text_layer *tl(pc_engine_session *s, uint64_t id)
{
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    return &l->content.u.text;
}

static void test_aa_orientation(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":20,\"y\":70,\"text\":\"Aa\",\"size\":36}");
    char p[64];
    snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.antiAlias.none", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(tl(s, id)->antialias == PC_AA_NONE);
    CHECK(ex(s, "type.orientation.vertical", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(tl(s, id)->orientation == PC_ORIENT_VERTICAL);
    CHECK(pc_engine_session_undo(s));
    CHECK(tl(s, id)->orientation == PC_ORIENT_HORIZONTAL);
    pc_engine_session_free(s);
}

static void test_opentype(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":10,\"y\":40,\"text\":\"fifl\",\"size\":24}");
    char p[64];
    snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.openType.standardLigatures", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!tl(s, id)->runs[0].style.ligatures); /* was on by default, toggled off */
    CHECK(ex(s, "type.openType.swash", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_convert_and_workpath(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":30,\"y\":70,\"text\":\"Hi\",\"size\":40}");
    char p[64];
    snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.createWorkPath", p, &out) == PC_OK);
    CHECK(uint_of(&out, "subpaths") > 0);
    pc_json_value_fini(&out);
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc)->has_work_path);
    CHECK(ex(s, "type.convertToShape", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    CHECK(l->content.kind == PC_LAYER_SHAPE);
    pc_engine_session_free(s);
}

static void test_point_paragraph(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":60,\"y\":70,\"text\":\"Hello world\",\"size\":24}");
    char p[64];
    snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(ex(s, "type.convertToParagraphText", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(tl(s, id)->shape.is_box);
    CHECK(ex(s, "type.convertToPointText", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!tl(s, id)->shape.is_box);
    pc_engine_session_free(s);
}

static void test_warp_lorem_update_defaults(void)
{
    pc_engine_session *s = mk();
    uint64_t id = create(s, "{\"x\":20,\"y\":60,\"text\":\"Warp me\",\"size\":24}");
    char p[128];
    pc_json_value out = PC_JSON_VALUE_INIT;
    snprintf(p, sizeof p, "{\"layer\":%llu,\"style\":\"arc\",\"bend\":40}", (unsigned long long)id);
    CHECK(ex(s, "type.warpText", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(tl(s, id)->has_warp);
    /* save then load defaults */
    snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    CHECK(ex(s, "type.saveDefaultTypeStyles", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(s->has_type_defaults);
    CHECK(ex(s, "type.loadDefaultTypeStyles", p, &out) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ex(s, "type.updateAllTextLayers", "{}", &out) == PC_OK);
    CHECK(uint_of(&out, "updated") >= 1);
    pc_json_value_fini(&out);
    /* lorem into a new layer */
    CHECK(ex(s, "type.pasteLoremIpsum", "{\"new\":true}", &out) == PC_OK);
    pc_json_value_fini(&out);
    /* resolve missing fonts (none) */
    CHECK(ex(s, "type.resolveMissingFonts", "{}", &out) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static const char *const IDS[] = {
    "type.antiAlias.none", "type.antiAlias.sharp", "type.antiAlias.crisp", "type.antiAlias.strong",
    "type.antiAlias.smooth", "type.antiAlias.windowsLcd", "type.antiAlias.windows", "type.orientation.horizontal",
    "type.orientation.vertical", "type.openType.standardLigatures", "type.openType.contextualAlternates",
    "type.openType.discretionaryLigatures", "type.openType.swash", "type.openType.oldstyle",
    "type.openType.stylisticAlternates", "type.openType.titlingAlternates", "type.openType.ornaments",
    "type.openType.ordinals", "type.openType.fractions", "type.createWorkPath", "type.convertToShape",
    "type.rasterizeTypeLayer", "type.convertToParagraphText", "type.convertToPointText", "type.warpText",
    "type.updateAllTextLayers", "type.replaceAllMissingFonts", "type.resolveMissingFonts", "type.pasteLoremIpsum",
    "type.saveDefaultTypeStyles", "type.loadDefaultTypeStyles",
};
static const char *const HOSTILE[] = {
    "{}", "null", "[]", "{\"layer\":-1}", "{\"layer\":99999}", "{\"layer\":\"no\"}", "{\"layers\":5}",
    "{\"layers\":[\"x\"]}", "{\"on\":\"x\"}", "{\"range\":[9,0]}", "{\"style\":\"nope\"}", "{\"bend\":1e99}",
    "{\"map\":5}", "{\"at\":-1}", "{\"new\":\"x\"}",
};

static void test_adversarial(void)
{
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        pc_engine_session *bare = NULL;
        CHECK(pc_engine_session_new(&bare, NULL) == PC_OK);
        pc_engine_session *s = mk();
        (void)create(s, "{\"x\":10,\"y\":40,\"text\":\"Aa Bb\",\"size\":16}");
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_json_value o1 = PC_JSON_VALUE_INIT, o2 = PC_JSON_VALUE_INIT;
            if (ex(bare, IDS[i], HOSTILE[j], &o1) == PC_OK) {
                pc_json_value_fini(&o1);
            }
            if (ex(s, IDS[i], HOSTILE[j], &o2) == PC_OK) {
                pc_json_value_fini(&o2);
            }
        }
        pc_engine_session_free(bare);
        pc_engine_session_free(s);
    }
}

static void test_attach_deferred(void)
{
    /* type.attachToPath / detachFromPath are permanently deferred (not registered). */
    CHECK(pc_engine_command_find("type.attachToPath") == NULL);
    CHECK(pc_engine_command_find("type.detachFromPath") == NULL);
}

int main(int argc, char **argv)
{
    setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_aa_orientation);
    RUN(test_opentype);
    RUN(test_convert_and_workpath);
    RUN(test_point_paragraph);
    RUN(test_warp_lorem_update_defaults);
    RUN(test_attach_deferred);
    RUN(test_adversarial);
    return pc_t_finish();
}
