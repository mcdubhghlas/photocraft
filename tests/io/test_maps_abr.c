/* Unit tests of chunk I4's abr_map (abr_map/tests.rs and the ABR part of tests/cancel.rs) and
 * never-crash tests of every I4 decoder over hostile blocks, resources and brush files. */
#include "harness.h"

#include "maps.h"
#include "io/maps_desc.h"
#include "paint/render.h"
#include "psd/slices.h"
#include "vector/vector.h"

static pc_path sample_path(void)
{
    pc_path p;
    static const double line[2][2] = {{1.0, 2.0}, {3.0, 4.0}};
    memset(&p, 0, sizeof p);
    p.subpaths = xmalloc(2 * sizeof *p.subpaths);
    p.nsubpaths = 2;
    p.subpaths[0].closed = true;
    p.subpaths[0].op = PC_PATH_COMBINE;
    p.subpaths[0].knots = xmalloc(3 * sizeof(pc_knot));
    p.subpaths[0].nknots = 3;
    p.subpaths[0].knots[0] = pc_knot_corner(10.0, 10.0);
    p.subpaths[0].knots[1] =
        pc_knot_smooth((pc_point){50.0, 20.0}, (pc_point){40.0, 10.0}, (pc_point){60.0, 30.0});
    p.subpaths[0].knots[2] = pc_knot_corner(20.0, 40.0);
    must(pc_subpath_polyline(line, 2, &p.subpaths[1], NULL));
    p.subpaths[1].op = PC_PATH_SUBTRACT;
    return p;
}

static pc_note note(const char *text, const char *author, double x, double y)
{
    pc_note n;
    pc_note_default(&n);
    must(pc_str_set_c(&n.text, text, NULL));
    must(pc_str_set_c(&n.author, author, NULL));
    n.position[0] = x;
    n.position[1] = y;
    return n;
}

/* ---------------------------------------------------------------- abr_map (abr_map/tests.rs) */

static pc_psd_abr_sample asample(const char *id, uint32_t w, uint32_t h, uint16_t depth)
{
    pc_psd_abr_sample s;
    size_t bpp = depth / 8;
    memset(&s, 0, sizeof s);
    must(pc_psd_str_set(&s.id, id, strlen(id), NULL));
    s.width = w;
    s.height = h;
    s.depth = depth;
    s.data.len = (size_t)w * h * bpp;
    s.data.data = xmalloc(s.data.len + 1);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        memset(s.data.data + i * bpp, i % 2 == 0 ? 255 : 0, bpp);
    }
    return s;
}

static pc_psd_value txt(const char *s)
{
    pc_psd_value v;
    must(pc_psd_value_text(s, true, &v, NULL));
    return v;
}

static pc_psd_value prc(double v)
{
    return pc_io4_unit("#Prc", v);
}

static pc_psd_value en(const char *ty, const char *v)
{
    pc_psd_value r;
    must(pc_psd_value_enum(ty, v, &r, NULL));
    return r;
}

static pc_psd_value obj_v(pc_psd_desc *d)
{
    pc_psd_value v;
    must(pc_psd_value_object(d, &v, NULL));
    return v;
}

static pc_psd_desc D(const char *cls)
{
    pc_psd_desc d;
    must(pc_psd_desc_init(&d, cls, NULL));
    return d;
}

static void put(pc_psd_desc *d, const char *k, pc_psd_value v)
{
    must(pc_psd_desc_add(d, k, &v, NULL));
}

static pc_psd_value var(int32_t control, double jitter, double minimum)
{
    pc_psd_desc d = D("brVr");
    put(&d, "bVTy", pc_psd_value_int(control));
    put(&d, "fStp", pc_psd_value_int(40));
    put(&d, "jitter", prc(jitter));
    put(&d, "Mnm ", prc(minimum));
    return obj_v(&d);
}

static pc_psd_desc sampled_tip(const char *id)
{
    pc_psd_desc d = D("sampledBrush");
    put(&d, "Dmtr", pc_io4_unit("#Pxl", 64.0));
    put(&d, "Angl", pc_io4_unit("#Ang", 30.0));
    put(&d, "Rndn", prc(80.0));
    put(&d, "Spcn", prc(40.0));
    put(&d, "Intr", pc_psd_value_bool(true));
    put(&d, "flipX", pc_psd_value_bool(true));
    put(&d, "flipY", pc_psd_value_bool(false));
    put(&d, "sampledData", txt(id));
    return d;
}

static pc_psd_desc full_preset(void)
{
    pc_psd_desc d = D("brushPreset");
    pc_psd_desc tip = sampled_tip("$tip-1");
    pc_psd_desc txtr = D("Ptrn");
    pc_psd_desc dual = D("dualBrush");
    pc_psd_desc dtip = D("computedBrush");
    pc_psd_desc to = D("currentToolOptions");
    put(&d, "Nm  ", txt("Kitchen Sink"));
    put(&d, "Brsh", obj_v(&tip));
    put(&d, "useTipDynamics", pc_psd_value_bool(true));
    put(&d, "flipX", pc_psd_value_bool(true));
    put(&d, "flipY", pc_psd_value_bool(false));
    put(&d, "minimumDiameter", prc(25.0));
    put(&d, "minimumRoundness", prc(10.0));
    put(&d, "tiltScale", prc(0.0));
    put(&d, "szVr", var(2, 20.0, 0.0));
    put(&d, "angleDynamics", var(6, 10.0, 0.0));
    put(&d, "roundnessDynamics", var(3, 5.0, 0.0));
    put(&d, "useScatter", pc_psd_value_bool(true));
    put(&d, "Cnt ", pc_psd_value_int(3));
    put(&d, "bothAxes", pc_psd_value_bool(true));
    put(&d, "countDynamics", var(0, 50.0, 0.0));
    put(&d, "scatterDynamics", var(1, 250.0, 0.0));
    put(&d, "useTexture", pc_psd_value_bool(true));
    put(&d, "TxtC", pc_psd_value_bool(true));
    put(&d, "InvT", pc_psd_value_bool(true));
    put(&d, "textureScale", prc(150.0));
    put(&d, "textureBrightness", pc_psd_value_int(-75));
    put(&d, "textureContrast", pc_psd_value_int(25));
    put(&d, "textureBlendMode", en("BlnM", "CBrn"));
    put(&d, "textureDepth", prc(60.0));
    put(&d, "minimumDepth", prc(20.0));
    put(&d, "textureDepthDynamics", var(2, 30.0, 0.0));
    put(&txtr, "Nm  ", txt("Grit"));
    put(&txtr, "Idnt", txt("pat-uuid"));
    put(&d, "Txtr", obj_v(&txtr));
    put(&dtip, "Dmtr", pc_io4_unit("#Pxl", 12.0));
    put(&dtip, "Hrdn", prc(50.0));
    put(&dual, "useDualBrush", pc_psd_value_bool(true));
    put(&dual, "Flip", pc_psd_value_bool(true));
    put(&dual, "Brsh", obj_v(&dtip));
    put(&dual, "BlnM", en("BlnM", "hardMix"));
    put(&dual, "Spcn", prc(30.0));
    put(&dual, "Cnt ", pc_psd_value_int(2));
    put(&dual, "bothAxes", pc_psd_value_bool(true));
    put(&dual, "scatterDynamics", var(0, 120.0, 0.0));
    put(&d, "dualBrush", obj_v(&dual));
    put(&d, "useColorDynamics", pc_psd_value_bool(true));
    put(&d, "clVr", var(2, 40.0, 0.0));
    put(&d, "H   ", prc(10.0));
    put(&d, "Strt", prc(20.0));
    put(&d, "Brgh", prc(30.0));
    put(&d, "purity", prc(-50.0));
    put(&d, "colorDynamicsPerTip", pc_psd_value_bool(false));
    put(&d, "usePaintDynamics", pc_psd_value_bool(true));
    put(&d, "opVr", var(2, 0.0, 10.0));
    put(&d, "prVr", var(1, 15.0, 5.0));
    put(&d, "wtVr", var(0, 0.0, 0.0));
    put(&d, "useBrushPose", pc_psd_value_bool(true));
    put(&d, "brushPoseTiltX", pc_psd_value_int(50));
    put(&d, "brushPoseAngle", pc_psd_value_int(90));
    put(&d, "overridePoseAngle", pc_psd_value_bool(true));
    put(&d, "Wtdg", pc_psd_value_bool(true));
    put(&d, "Nose", pc_psd_value_bool(true));
    put(&d, "Rpt ", pc_psd_value_bool(true));
    put(&d, "someFutureKey", pc_psd_value_bool(true));
    put(&to, "Opct", pc_psd_value_int(80));
    put(&to, "flow", pc_psd_value_int(60));
    put(&to, "smoothing", pc_psd_value_bool(true));
    put(&to, "smoothingValue", pc_psd_value_int(40));
    put(&to, "usePressureOverridesSize", pc_psd_value_bool(true));
    put(&d, "toolOptions", obj_v(&to));
    return d;
}

static pc_psd_pattern grit(void)
{
    pc_psd_pattern p;
    static const uint8_t px[8] = {0, 64, 128, 255, 255, 128, 64, 0};
    memset(&p, 0, sizeof p);
    p.mode = 1;
    p.width = 4;
    p.height = 2;
    must(pc_psd_str_set(&p.name, "Grit", 4, NULL));
    must(pc_psd_str_set(&p.id, "pat-uuid", 8, NULL));
    p.depth = 8;
    p.channels = xmalloc(sizeof *p.channels);
    p.nchannels = 1;
    memset(p.channels, 0, sizeof *p.channels);
    must(pc_psd_bytes_set(&p.channels[0], px, 8, NULL));
    return p;
}

static pc_io_abr_import read_ok(const pc_psd_bytes *b, const char *group)
{
    pc_io_abr_import imp;
    must(pc_io_read_abr(b->data, b->len, group, &imp, NULL));
    return imp;
}

static bool warned(const pc_io_abr_import *imp, const char *needle)
{
    return pc_io_warnings_any(&imp->warnings, needle);
}

static void test_v6_settings_map_onto_brush_sections(void)
{
    for (uint16_t sub = 1; sub <= 2; sub++) {
        pc_psd_abr_sample s = asample("$tip-1", 16, 8, 8);
        pc_psd_pattern g = grit();
        pc_psd_desc presets[2];
        pc_psd_desc tip = D("computedBrush");
        pc_psd_bytes bytes;
        pc_io_abr_import imp;
        const pc_paint_brush *b;
        const uint16_t *td;
        presets[0] = full_preset();
        presets[1] = D("brushPreset");
        put(&presets[1], "Nm  ", txt("Round 19"));
        put(&tip, "Dmtr", pc_io4_unit("#Pxl", 19.0));
        put(&tip, "Hrdn", prc(0.0));
        put(&tip, "Spcn", prc(10.0));
        put(&presets[1], "Brsh", obj_v(&tip));
        must(pc_psd_abr_write_v6(sub, &s, 1, &g, 1, presets, 2, true, &bytes, NULL));
        imp = read_ok(&bytes, "Test");
        CHECK_EQ_UINT(imp.presets.len, 2);
        CHECK(pc_str_eq_c(&imp.presets.v[0].name, "Kitchen Sink") &&
              pc_str_eq_c(&imp.presets.v[0].group, "Test") && !imp.presets.v[0].builtin);
        b = &imp.presets.v[0].brush;
        CHECK(b->tip.kind == PC_PAINT_TIP_SAMPLED && b->tip.tile.width == 16 && b->tip.tile.height == 8);
        td = pc_paint_gray_tile_samples(&b->tip.tile);
        CHECK(td != NULL && td[0] == 65535 && td[1] == 0);
        CHECK(b->size == 64.0f && b->angle == 30.0f && b->roundness == 0.8f && b->spacing == 0.4f &&
              b->flip_x && !b->flip_y);
        CHECK(b->shape_dynamics.enabled && b->shape_dynamics.flip_x_jitter &&
              !b->shape_dynamics.flip_y_jitter);
        CHECK(b->shape_dynamics.size.control == PC_PAINT_CONTROL_PEN_PRESSURE &&
              b->shape_dynamics.size.jitter == 0.2f && b->shape_dynamics.size.minimum == 0.25f);
        CHECK(b->shape_dynamics.angle.control == PC_PAINT_CONTROL_DIRECTION &&
              b->shape_dynamics.roundness.control == PC_PAINT_CONTROL_PEN_TILT &&
              b->shape_dynamics.roundness.minimum == 0.1f);
        CHECK(b->scattering.enabled && b->scattering.both_axes && b->scattering.count == 3 &&
              b->scattering.scatter.jitter == 2.5f &&
              b->scattering.scatter.control == PC_PAINT_CONTROL_FADE &&
              b->scattering.scatter.fade_steps == 40 && b->scattering.count_jitter.jitter == 0.5f);
        CHECK(b->texture.enabled && b->texture.each_tip && b->texture.invert && b->texture.scale == 1.5f &&
              b->texture.brightness == -0.5f && b->texture.contrast == 0.25f &&
              b->texture.mode == PC_PAINT_MASK_COLOR_BURN && b->texture.depth == 0.6f);
        CHECK(b->texture.depth_jitter.jitter == 0.3f && b->texture.depth_jitter.minimum == 0.2f);
        CHECK(b->texture.pattern.kind == PC_PAINT_PATTERN_TILE && b->texture.pattern.tile.width == 4 &&
              b->texture.pattern.tile.height == 2 &&
              pc_paint_gray_tile_samples(&b->texture.pattern.tile)[3] == 65535);
        CHECK(b->dual_brush.enabled && b->dual_brush.flip && b->dual_brush.both_axes &&
              b->dual_brush.tip.kind == PC_PAINT_TIP_ROUND);
        CHECK(b->dual_brush.size == 12.0f && b->dual_brush.hardness == 0.5f &&
              b->dual_brush.mode == PC_PAINT_MASK_HARD_MIX && b->dual_brush.spacing == 0.3f &&
              b->dual_brush.count == 2 && b->dual_brush.scatter == 1.2f);
        CHECK(b->color_dynamics.enabled && !b->color_dynamics.per_tip &&
              b->color_dynamics.fg_bg.jitter == 0.4f && b->color_dynamics.hue_jitter == 0.1f &&
              b->color_dynamics.saturation_jitter == 0.2f && b->color_dynamics.brightness_jitter == 0.3f &&
              b->color_dynamics.purity == -0.5f);
        CHECK(b->transfer.enabled && b->transfer.opacity.control == PC_PAINT_CONTROL_PEN_PRESSURE &&
              b->transfer.opacity.minimum == 0.1f && b->transfer.flow.control == PC_PAINT_CONTROL_FADE &&
              b->transfer.flow.jitter == 0.15f);
        CHECK(b->pose.enabled && b->pose.override_rotation && !b->pose.override_tilt &&
              b->pose.tilt_x == 45.0f && b->pose.rotation == 90.0f);
        CHECK(b->wet_edges && b->noise && b->build_up);
        CHECK(b->opacity == 0.8f && b->flow == 0.6f && b->smoothing.amount == 0.4f && b->pressure_size);
        CHECK(warned(&imp, "someFutureKey"));
        b = &imp.presets.v[1].brush;
        CHECK(b->tip.kind == PC_PAINT_TIP_ROUND && b->size == 19.0f && b->hardness == 0.0f &&
              b->spacing == 0.1f && !b->pressure_size);
        pc_io_abr_import_fini(&imp);
        pc_psd_bytes_fini(&bytes);
        pc_psd_desc_fini(&presets[0]);
        pc_psd_desc_fini(&presets[1]);
        pc_psd_pattern_fini(&g);
        pc_psd_abr_sample_fini(&s);
    }
}

static void test_missing_pattern_and_tip_are_reported(void)
{
    pc_psd_abr_sample s[2] = {asample("$tip-1", 4, 4, 16), asample("$tip-2", 4, 4, 8)};
    pc_psd_desc presets[2];
    pc_psd_desc tip = sampled_tip("$nowhere");
    pc_psd_bytes bytes;
    pc_io_abr_import imp;
    presets[0] = full_preset();
    presets[1] = D("brushPreset");
    put(&presets[1], "Nm  ", txt("Orphan"));
    put(&presets[1], "Brsh", obj_v(&tip));
    must(pc_psd_abr_write_v6(2, s, 2, NULL, 0, presets, 2, false, &bytes, NULL));
    imp = read_ok(&bytes, "x");
    CHECK_EQ_UINT(imp.presets.len, 1);
    CHECK(imp.presets.v[0].brush.texture.pattern.kind == PC_PAINT_PATTERN_PROCEDURAL);
    CHECK(warned(&imp, "Grit") && warned(&imp, "Orphan"));
    pc_io_abr_import_fini(&imp);
    pc_psd_bytes_fini(&bytes);
    pc_psd_desc_fini(&presets[0]);
    pc_psd_desc_fini(&presets[1]);
    pc_psd_abr_sample_fini(&s[0]);
    pc_psd_abr_sample_fini(&s[1]);
}

static void legacy_brushes(pc_psd_abr_legacy b[2])
{
    memset(b, 0, 2 * sizeof *b);
    b[0].spacing = 25;
    b[0].anti_alias = true;
    b[0].tip = PC_PSD_ABR_COMPUTED;
    b[0].diameter = 13;
    b[0].hardness = 50;
    b[0].angle = 20;
    b[0].roundness = 60;
    must(pc_psd_str_set(&b[1].name, "Dune Grass", 10, NULL));
    b[1].spacing = 0;
    b[1].anti_alias = false;
    b[1].tip = PC_PSD_ABR_SAMPLED;
    b[1].sample = asample("", 30, 10, 8);
}

static void test_v1_v2_brushes_become_presets(void)
{
    pc_psd_abr_legacy b[2];
    legacy_brushes(b);
    for (uint16_t version = 1; version <= 2; version++) {
        pc_psd_bytes bytes;
        pc_io_abr_import imp;
        const pc_paint_preset *c;
        const pc_paint_preset *s;
        must(pc_psd_abr_write_v12(version, b, 2, true, &bytes, NULL));
        imp = read_ok(&bytes, "Old");
        CHECK(imp.version == version && imp.presets.len == 2);
        c = &imp.presets.v[0];
        CHECK(pc_str_eq_c(&c->name, "13 px Round"));
        CHECK(c->brush.size == 13.0f && c->brush.hardness == 0.5f && c->brush.angle == 20.0f &&
              c->brush.roundness == 0.6f && c->brush.spacing == 0.25f);
        s = &imp.presets.v[1];
        CHECK(pc_str_eq_c(&s->name, version == 2 ? "Dune Grass" : "Sampled Brush 2"));
        CHECK(s->brush.size == 30.0f && s->brush.spacing == 0.25f && s->brush.aliased);
        pc_io_abr_import_fini(&imp);
        pc_psd_bytes_fini(&bytes);
    }
    pc_psd_abr_legacy_fini(&b[0]);
    pc_psd_abr_legacy_fini(&b[1]);
}

static void test_v6_without_settings_lists_tips(void)
{
    pc_psd_abr_sample s[2] = {asample("$a", 8, 8, 8), asample("$b", 3, 9, 16)};
    pc_psd_bytes bytes;
    pc_io_abr_import imp;
    must(pc_psd_abr_write_v6(1, s, 2, NULL, 0, NULL, 0, true, &bytes, NULL));
    imp = read_ok(&bytes, "g");
    CHECK(imp.presets.len == 2 && imp.presets.v[0].brush.size == 8.0f && imp.presets.v[1].brush.size == 9.0f);
    pc_io_abr_import_fini(&imp);
    pc_psd_bytes_fini(&bytes);
    pc_psd_abr_sample_fini(&s[0]);
    pc_psd_abr_sample_fini(&s[1]);
}

static void test_big_tips_are_downsampled(void)
{
    pc_psd_abr_sample s;
    pc_psd_bytes bytes;
    pc_io_abr_import imp;
    memset(&s, 0, sizeof s);
    must(pc_psd_str_set(&s.id, "$big", 4, NULL));
    s.width = 3000;
    s.height = 1500;
    s.depth = 8;
    s.data.len = (size_t)3000 * 1500;
    s.data.data = xmalloc(s.data.len);
    memset(s.data.data, 200, s.data.len);
    must(pc_psd_abr_write_v6(2, &s, 1, NULL, 0, NULL, 0, true, &bytes, NULL));
    imp = read_ok(&bytes, "g");
    CHECK(imp.presets.v[0].brush.tip.kind == PC_PAINT_TIP_SAMPLED);
    CHECK(imp.presets.v[0].brush.tip.tile.width == PC_IO_ABR_MAX_TIP_EDGE &&
          imp.presets.v[0].brush.tip.tile.height == 1250);
    CHECK(imp.presets.v[0].brush.size == 3000.0f);
    CHECK(warned(&imp, "downsampled"));
    pc_io_abr_import_fini(&imp);
    pc_psd_bytes_fini(&bytes);
    pc_psd_abr_sample_fini(&s);
}

static void test_tilt_scale_projection_spacing_and_mixer_fields_map(void)
{
    pc_psd_desc presets[2];
    pc_psd_desc tip = D("computedBrush");
    pc_psd_desc to = D("currentToolOptions");
    pc_psd_desc plain_tip = D("computedBrush");
    pc_psd_bytes bytes;
    pc_io_abr_import imp;
    const pc_paint_brush *b;
    pc_paint_mixer_settings mdef;
    pc_paint_dynamic ddef = pc_paint_dynamic_default();
    put(&tip, "Dmtr", pc_io4_unit("#Pxl", 30.0));
    put(&tip, "Spcn", prc(35.0));
    put(&tip, "Intr", pc_psd_value_bool(false));
    presets[0] = D("brushPreset");
    put(&presets[0], "Nm  ", txt("Wet Pen"));
    put(&presets[0], "Brsh", obj_v(&tip));
    put(&presets[0], "useTipDynamics", pc_psd_value_bool(true));
    put(&presets[0], "szVr", var(3, 0.0, 0.0));
    put(&presets[0], "tiltScale", prc(150.0));
    put(&presets[0], "brushProjection", pc_psd_value_bool(true));
    put(&presets[0], "usePaintDynamics", pc_psd_value_bool(true));
    put(&presets[0], "wtVr", var(2, 30.0, 10.0));
    put(&presets[0], "mxVr", var(1, 45.0, 0.0));
    put(&to, "wetness", prc(80.0));
    put(&to, "dryness", prc(20.0));
    put(&to, "mix", prc(65.0));
    put(&to, "flow", prc(70.0));
    put(&to, "sampleMerged", pc_psd_value_bool(true));
    put(&presets[0], "toolOptions", obj_v(&to));
    presets[1] = D("brushPreset");
    put(&presets[1], "Nm  ", txt("Plain"));
    put(&plain_tip, "Dmtr", pc_io4_unit("#Pxl", 9.0));
    put(&presets[1], "Brsh", obj_v(&plain_tip));
    must(pc_psd_abr_write_v6(2, NULL, 0, NULL, 0, presets, 2, true, &bytes, NULL));
    imp = read_ok(&bytes, "Mixer");
    b = &imp.presets.v[0].brush;
    CHECK(!b->spacing_enabled && fabsf(b->spacing - 0.35f) < 1e-6f);
    CHECK(b->shape_dynamics.brush_projection && fabsf(b->shape_dynamics.tilt_scale - 1.5f) < 1e-6f &&
          b->shape_dynamics.size.control == PC_PAINT_CONTROL_PEN_TILT);
    CHECK(b->transfer.wetness.control == PC_PAINT_CONTROL_PEN_PRESSURE &&
          b->transfer.wetness.jitter == 0.3f && b->transfer.wetness.minimum == 0.1f);
    CHECK(b->transfer.mix.control == PC_PAINT_CONTROL_FADE && b->transfer.mix.jitter == 0.45f);
    CHECK(b->mixer.wet == 0.8f && b->mixer.load == 0.2f && b->mixer.mix == 0.65f && b->mixer.flow == 0.7f &&
          b->mixer.sample_all_layers);
    CHECK(!warned(&imp, "Tilt Scale") && !warned(&imp, "Projection") && !warned(&imp, "Mixer"));
    b = &imp.presets.v[1].brush;
    pc_paint_mixer_default(&mdef);
    CHECK(b->spacing_enabled && !b->shape_dynamics.brush_projection && b->shape_dynamics.tilt_scale == 0.0f);
    CHECK(pc_paint_mixer_eq(&b->mixer, &mdef));
    CHECK(pc_paint_dynamic_eq(&b->transfer.wetness, &ddef) && pc_paint_dynamic_eq(&b->transfer.mix, &ddef));
    pc_io_abr_import_fini(&imp);
    pc_psd_bytes_fini(&bytes);
    pc_psd_desc_fini(&presets[0]);
    pc_psd_desc_fini(&presets[1]);
}

static void test_garbage_is_an_error(void)
{
    pc_io_abr_import imp;
    pc_error e = {0};
    static const uint8_t short6[4] = {0, 6, 0, 2};
    CHECK(pc_io_read_abr((const uint8_t *)"", 0, "g", &imp, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "not a readable brush file (.abr)");
    CHECK(pc_io_read_abr((const uint8_t *)"8BPS not a brush", 16, "g", &imp, NULL) != PC_OK);
    CHECK(pc_io_read_abr(short6, 4, "g", &imp, NULL) != PC_OK);
}

static void test_sampled_tip_paints_the_brush_colour_never_black(void)
{
    pc_psd_abr_sample s = asample("$tip-1", 8, 8, 8);
    pc_psd_desc preset = D("brushPreset");
    pc_psd_desc tip = sampled_tip("$tip-1");
    pc_psd_bytes bytes;
    pc_io_abr_import imp;
    pc_paint_brush b;
    pc_surface surf;
    pc_paint_point pts[2];
    pc_rect damage;
    int painted = 0;
    put(&preset, "Nm  ", txt("Tip"));
    put(&preset, "Brsh", obj_v(&tip));
    must(pc_psd_abr_write_v6(2, &s, 1, NULL, 0, &preset, 1, false, &bytes, NULL));
    imp = read_ok(&bytes, "Test");
    pc_paint_brush_copy(&b, &imp.presets.v[0].brush);
    b.pressure_size = false;
    b.color[0] = 0.8f;
    b.color[1] = 0.1f;
    b.color[2] = 0.1f;
    b.color[3] = 1.0f;
    pc_surface_init(&surf, PC_PIXEL_FORMAT_RGBA32F);
    pts[0] = pc_paint_point_make(30.0, 32.5, 1.0f);
    pts[1] = pc_paint_point_make(34.0, 32.5, 1.0f);
    must(pc_paint_render_stroke(&surf, &b, pts, 2, NULL, false, 1.0f, &damage, NULL));
    for (int32_t y = 0; y < 64; y++) {
        for (int32_t x = 0; x < 64; x++) {
            float p[4];
            pc_surface_rgba(&surf, x, y, p);
            if (p[3] > 0.5f) {
                painted++;
                CHECK(fabsf(p[0] - 0.8f) < 0.05f && fabsf(p[1] - 0.1f) < 0.05f);
            }
        }
    }
    CHECK(painted > 0 && painted < 64 * 64);
    pc_surface_fini(&surf);
    pc_paint_brush_fini(&b);
    pc_io_abr_import_fini(&imp);
    pc_psd_bytes_fini(&bytes);
    pc_psd_desc_fini(&preset);
    pc_psd_abr_sample_fini(&s);
}

/* tests/cancel.rs cancelled_brush_import_fails. */
static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static void record(void *ctx, float p)
{
    float *last = ctx;
    if (p < *last) {
        last[1] = 1.0f; /* went backwards */
    }
    last[0] = p;
}

static void test_cancelled_brush_import_fails(void)
{
    pc_psd_abr_legacy b[2];
    pc_psd_bytes bytes;
    pc_io_abr_import imp;
    pc_interrupt cancel = {yes, NULL, NULL};
    float prog[2] = {0.0f, 0.0f};
    pc_interrupt watch = {NULL, record, prog};
    pc_error e = {0};
    legacy_brushes(b);
    must(pc_psd_abr_write_v12(2, b, 2, true, &bytes, NULL));
    imp = read_ok(&bytes, "g");
    CHECK_EQ_UINT(imp.presets.len, 2);
    pc_io_abr_import_fini(&imp);
    CHECK_EQ_INT(pc_io_read_abr_with(bytes.data, bytes.len, "g", &cancel, &imp, &e), PC_ERR_CANCELLED);
    CHECK_EQ_STR(e.msg, "cancelled");
    must(pc_io_read_abr_with(bytes.data, bytes.len, "g", &watch, &imp, NULL));
    CHECK(prog[0] == 1.0f && prog[1] == 0.0f);
    pc_io_abr_import_fini(&imp);
    pc_psd_bytes_fini(&bytes);
    pc_psd_abr_legacy_fini(&b[0]);
    pc_psd_abr_legacy_fini(&b[1]);
}

/* ---------------------------------------------------------------- never crash */

/* Every decoder over every truncation and a byte sweep of a valid input (run under ASan). */
static void hostile_sweep(const uint8_t *d, size_t len)
{
    uint8_t *m = xmalloc(len + 1);
    pc_document doc = t_doc("h", 16, 16, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    for (size_t cut = 0; cut <= len; cut++) {
        for (int mut = -1; mut < 3; mut++) {
            size_t n = cut;
            bool found;
            pc_path p;
            pc_vector_mask vm;
            pc_io_vstk v;
            pc_fill f;
            pc_live_shape ls;
            pc_artboard a;
            pc_note *notes = NULL;
            size_t nn = 0;
            pc_measurement_scale sc;
            pc_slices sl;
            pc_psd_pattern *ps = NULL;
            size_t np = 0;
            pc_pattern *pats = NULL;
            pc_io_abr_import imp;
            pc_io_comps comps;
            memcpy(m, d, len);
            if (mut >= 0) {
                if (cut == 0) {
                    continue;
                }
                m[(cut * 7 + (size_t)mut * 13) % cut] = mut == 0 ? 0xFF : mut == 1 ? 0x00 : 0x80;
            }
            if (pc_io_path_from_resource(m, n, 16, 16, &found, &p, NULL) == PC_OK && found) {
                pc_path_fini(&p);
            }
            if (pc_io_vector_mask_from_block(m, n, 16, 16, &found, &vm, NULL) == PC_OK && found) {
                pc_vector_mask_fini(&vm);
            }
            if (pc_io_parse_vstk(m, n, 72.0f, &found, &v, NULL) == PC_OK && found) {
                pc_io_vstk_fini(&v);
            }
            if (pc_io_fill_from_vscg(m, n, &found, &f, NULL) == PC_OK && found) {
                pc_fill_fini(&f);
            }
            (void)pc_io_live_from_vogk(m, n, &found, &ls, NULL);
            if (pc_io_parse_artboard(m, n, &found, &a, NULL) == PC_OK && found) {
                pc_artboard_fini(&a);
            }
            if (pc_io_parse_anno(m, n, &found, &notes, &nn, NULL) == PC_OK && found) {
                for (size_t i = 0; i < nn; i++) {
                    pc_note_fini(&notes[i]);
                }
                free(notes);
            }
            if (pc_io_scale_from_resource(m, n, &found, &sc, NULL) == PC_OK && found) {
                pc_measurement_scale_fini(&sc);
            }
            if (pc_io_slices_from_psd(m, n, &doc, &sl, NULL) == PC_OK) {
                pc_slices_fini(&sl);
            }
            if (pc_psd_parse_pattern_block(m, n, &ps, &np, NULL) == PC_OK) {
                for (size_t i = 0; i < np; i++) {
                    pc_pattern pt;
                    if (pc_io_pattern_from_psd(&ps[i], &found, &pt, NULL) == PC_OK && found) {
                        pc_pattern_fini(&pt);
                    }
                }
                pc_psd_patterns_free(ps, np);
            }
            if (pc_io_read_pat(m, n, &pats, &np, NULL) == PC_OK) {
                pc_io_patterns_free(pats, np);
            }
            if (pc_io_read_abr(m, n, "h", &imp, NULL) == PC_OK) {
                pc_io_abr_import_fini(&imp);
            }
            if (pc_io_decode_comps(m, n, &doc, &comps, NULL, NULL) == PC_OK) {
                pc_io_comps_fini(&comps);
            }
        }
    }
    pc_document_fini(&doc);
    free(m);
    CHECK(true);
}

static void test_hostile_inputs_never_crash(void)
{
    pc_path p = sample_path();
    pc_buf b = PC_BUF_INIT;
    {
        pc_psd_abr_legacy lb[2];
        pc_psd_bytes bytes;
        must(pc_io_vmsk_bytes(&p, 6, 64, 64, &b, NULL));
        hostile_sweep(b.data, b.len);
        must(pc_io_vstk_bytes(NULL, false, 72.0f, &b, NULL));
        hostile_sweep(b.data, b.len);
        legacy_brushes(lb);
        must(pc_psd_abr_write_v12(2, lb, 2, true, &bytes, NULL));
        hostile_sweep(bytes.data, bytes.len);
        pc_psd_bytes_fini(&bytes);
        pc_psd_abr_legacy_fini(&lb[0]);
        pc_psd_abr_legacy_fini(&lb[1]);
    }
    {
        pc_note n = note("hostile \xE2\x9C\x93", "x", 1.0, 2.0);
        must(pc_io_write_anno(&n, 1, NULL, 0, &b, NULL));
        hostile_sweep(b.data, b.len);
        pc_note_fini(&n);
    }
    /* Absurd sizes: an Anno count past the block, a pattern record claiming 30000 x 30000. */
    {
        static const uint8_t anno[8] = {0, 2, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF};
        bool found = true;
        pc_note *notes = NULL;
        size_t nn = 0;
        must(pc_io_parse_anno(anno, 8, &found, &notes, &nn, NULL));
        CHECK(!found);
    }
    /* Extreme models write without overflow (#939: a note far off the canvas). */
    {
        pc_note n = note("far", "", 1e300, -1e300);
        pc_live_shape ls;
        bool has = false;
        n.popup[0] = NAN;
        must(pc_io_write_anno(&n, 1, NULL, 0, &b, NULL));
        pc_note_fini(&n);
        memset(&ls, 0, sizeof ls);
        ls.kind = PC_LIVE_LINE;
        ls.from[0] = NAN;
        ls.to[1] = INFINITY;
        must(pc_io_vogk_bytes(&ls, NAN, &has, &b, NULL));
        CHECK(has);
    }
    pc_buf_fini(&b);
    pc_path_fini(&p);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_v6_settings_map_onto_brush_sections);
    RUN(test_missing_pattern_and_tip_are_reported);
    RUN(test_v1_v2_brushes_become_presets);
    RUN(test_v6_without_settings_lists_tips);
    RUN(test_big_tips_are_downsampled);
    RUN(test_tilt_scale_projection_spacing_and_mixer_fields_map);
    RUN(test_garbage_is_an_error);
    RUN(test_sampled_tip_paints_the_brush_colour_never_black);
    RUN(test_cancelled_brush_import_fails);
    RUN(test_hostile_inputs_never_crash);
    return pc_t_finish();
}
