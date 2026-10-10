//! `rustref paint-vectors`: L2 paint (1/2) reference vectors, checked line by line by
//! tests/paint/test_parity.c. One case per line, tab-separated, keyword first. Floats are hex
//! bits (f32 %08x, f64 %016x); hashes are FNV-1a 64 over little-endian bits.
//!
//!   rng  SEED HASH                     rand01 / rand_signed over index x stream
//!   mix  X MIX64                       mix64
//!   hash2 SALT HASH                    hash2 over a coordinate grid (with extremes)
//!   seedb HEXBYTES SEED                seed_from_bytes
//!   q16  F32 U16                       GrayTile quantisation
//!   b64e HEXBYTES TEXT                 b64_encode
//!   b64d HEXTEXT HEXBYTES|ERR          b64_decode
//!   mips W H SEED HASH                 Mips::new levels, level_for and sample_clamped sweeps
//!   pat  STYLE SIZE SEED HASH          procedural::pattern (+ sample_wrap sweep)
//!   tip  NAME N SEED EXTRA HASH        procedural tips
//!   cov  HASH                          dab_coverage sweep
//!   mask MODE HASH                     mask_combine sweep
//!   grid X Y D CX CY SQ                grid_center / grid_square
//!   drect CX CY R RECT                 retouch::dab_rect
//!   ctl  HASH                          control_value / control_angle sweep
//!   color BRUSH HASH                   dynamic_color sweep
//!   smooth CFG ZOOM PTS HASH           smooth_points (CFG = amount bits, then the 4 flags)
//!   lsmooth AMOUNT PTS HASH            lib smooth
//!   dabs BRUSH OVR PTS N NDUAL HASH    DabGenerator (all at once)
//!   rast BRUSH OVR DAB DUAL RECT HASH  BrushContext::dab_rect + rasterize (+ footprint)
//!   tex  BRUSH HASH                    BrushContext::texture_at sweep
//!   stroke BRUSH OVR SCEN PTS DMG HASH render_stroke on a scenario surface
//!   inc  BRUSH SCEN PTS CHUNK RESULTS  incremental renderer (composites, tail, coverage, dirty)
//!   union BRUSH SCEN PTS DMG HASH      composite_union with a mirrored pass
//!   preset I JSON DMG HASH             built-in preset: serialisation and a rendered stroke
//!   pick A B RESULT                    picked_over / with_protected_texture / bounded_for_render
//!   ser  TYPE JSON                     to_string round trip
//!   serv TYPE JSON VALUE               to_string(to_value)
//!   de   TYPE INPUT RESULT             from_value(from_str::<Value>(INPUT)) or "ERR msg"
use photocraft_color::{BlendMode, ColorMode, PixelFormat, SampleType};
use photocraft_geom::{Point, Rect};
use photocraft_paint::brush::*;
use photocraft_paint::dynamics::{control_angle, control_value, dynamic_color, smooth_points, DabGenerator, StepInput};
use photocraft_paint::presets;
use photocraft_paint::procedural;
use photocraft_paint::render::{grid_center, grid_square, mask_combine, render_stroke, BrushContext, StrokeRenderer};
use photocraft_paint::rng::*;
use photocraft_paint::tile::{b64_decode, b64_encode, GrayTile, Mips};
use photocraft_paint::{dab_coverage, smooth, Dab, Stroke, StrokePoint};
use photocraft_raster::{from_rgba, Surface};
use serde_json::Value as J;

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn u64le(h: &mut u64, v: u64) { fnv(h, &v.to_le_bytes()); }
// NaNs are canonicalised: which NaN an operation propagates (sign, payload) depends on the operand
// order LLVM picks, which is not behaviour.
fn b32(v: f32) -> u32 { if v.is_nan() { 0x7fc0_0000 } else { v.to_bits() } }
fn b64(v: f64) -> u64 { if v.is_nan() { 0x7ff8_0000_0000_0000 } else { v.to_bits() } }
fn f64b(h: &mut u64, v: f64) { fnv(h, &b64(v).to_le_bytes()); }
fn f32b(h: &mut u64, v: f32) { fnv(h, &b32(v).to_le_bytes()); }
fn x32(v: f32) -> String { format!("{:08x}", b32(v)) }
fn x64(v: f64) -> String { format!("{:016x}", b64(v)) }
fn hex(b: &[u8]) -> String { if b.is_empty() { "-".into() } else { b.iter().map(|x| format!("{x:02x}")).collect() } }
fn rect_s(r: Rect) -> String { format!("{},{},{},{}", r.x0, r.y0, r.x1, r.y1) }
fn trunc(mut m: String) -> String {
    while m.len() > 255 {
        m.pop();
    }
    m
}
fn line(parts: &[String]) {
    let s = parts.join("\t");
    assert!(!s.contains('\n'));
    println!("{s}");
}
macro_rules! l {
    ($($e:expr),+ $(,)?) => { line(&[$(($e).to_string()),+]) };
}

fn dab_h(h: &mut u64, d: &Dab) {
    f64b(h, d.center.x);
    f64b(h, d.center.y);
    for v in [d.radius, d.alpha, d.angle, d.roundness] {
        f32b(h, v);
    }
    fnv(h, &[d.flip_x as u8, d.flip_y as u8]);
    f32b(h, d.opacity);
    for c in d.color {
        f32b(h, c);
    }
    f32b(h, d.depth);
    u64le(h, d.index);
    for v in [d.proj_angle, d.proj_scale, d.wet, d.mix] {
        f32b(h, v);
    }
}

fn pts_s(p: &[StrokePoint]) -> String {
    if p.is_empty() {
        return "-".into();
    }
    p.iter()
        .map(|p| {
            format!(
                "{},{},{},{},{},{},{},{}",
                x64(p.x),
                x64(p.y),
                x32(p.pressure),
                x32(p.tilt_x),
                x32(p.tilt_y),
                x32(p.rotation),
                x32(p.wheel),
                x64(p.time)
            )
        })
        .collect::<Vec<_>>()
        .join(";")
}

fn pt(x: f64, y: f64, pressure: f32) -> StrokePoint { StrokePoint::new(x, y, pressure) }

fn point_sets() -> Vec<Vec<StrokePoint>> {
    let mut v = Vec::new();
    v.push(vec![pt(10.0, 20.0, 1.0), pt(200.0, 20.0, 1.0)]);
    v.push(
        (0..12)
            .map(|i| StrokePoint {
                x: 15.0 + i as f64 * 17.5,
                y: if i % 2 == 0 { 30.0 } else { 70.25 },
                pressure: 0.1 + i as f32 * 0.075,
                tilt_x: -40.0 + i as f32 * 7.0,
                tilt_y: 25.0 - i as f32 * 4.5,
                rotation: i as f32 * 33.0,
                wheel: 1.0 - i as f32 * 0.08,
                time: i as f64 * 16.0,
            })
            .collect(),
    );
    v.push(vec![pt(50.5, 40.25, 0.7)]);
    v.push(
        (0..30)
            .map(|i| StrokePoint {
                time: i as f64 * 9.0 + if i > 15 { 120.0 } else { 0.0 },
                ..pt(20.0 + i as f64 * 8.0, 60.0 + (i as f64 * 0.37).sin() * 35.0, (i as f32 / 29.0).min(1.0))
            })
            .collect(),
    );
    v.push(vec![StrokePoint { time: 0.0, ..pt(80.0, 50.0, 1.0) }, StrokePoint { time: 300.0, ..pt(80.0, 50.0, 1.0) }]);
    v.push(vec![pt(-30.5, -12.25, 1.0), pt(40.0, -5.0, 0.5), pt(40.0, 90.0, 0.25)]);
    v
}

fn hostile_point_sets() -> Vec<Vec<StrokePoint>> {
    vec![
        vec![],
        vec![pt(1e12, 5.0, 1.0)],
        vec![pt(f64::NAN, f64::NAN, 1.0)],
        vec![pt(f64::INFINITY, 0.0, 1.0)],
        vec![pt(10.0, 10.0, 1.0), pt(f64::NAN, 10.0, 1.0), pt(30.0, 10.0, f32::NAN)],
        vec![StrokePoint { time: f64::NAN, ..pt(10.0, 10.0, 1.0) }, StrokePoint { time: 50.0, ..pt(40.0, 10.0, f32::INFINITY) }],
        vec![StrokePoint { tilt_x: f32::NAN, tilt_y: f32::INFINITY, rotation: f32::NEG_INFINITY, wheel: f32::NAN, ..pt(20.0, 20.0, -3.0) }],
        vec![pt(-3e9, 7.0, 1.0)],
    ]
}

fn gtile(w: u32, h: u32, seed: u64) -> GrayTile {
    let mut i = 0u64;
    let mut data = Vec::new();
    for _ in 0..(w as u64 * h as u64) {
        data.push((mix64(seed.wrapping_mul(1000).wrapping_add(i)) >> 48) as u16);
        i += 1;
    }
    GrayTile { width: w, height: h, data }
}

fn base() -> BrushSettings {
    BrushSettings {
        pressure_size: false,
        spacing: 0.25,
        seed: 7,
        color: [0.8, 0.3, 0.1, 0.9],
        background: [0.1, 0.4, 0.9, 1.0],
        ..Default::default()
    }
}

fn brushes() -> Vec<BrushSettings> {
    let b = base;
    let mut v = vec![BrushSettings::default(), b(), BrushSettings { size: 30.0, hardness: 0.0, ..b() }];
    v.push(BrushSettings { hardness: 0.5, roundness: 0.3, angle: 30.0, size: 26.0, ..b() });
    v.push(BrushSettings { aliased: true, size: 9.0, hardness: 0.0, ..b() });
    v.push(BrushSettings { aliased: true, size: 4.0, ..b() });
    v.push(BrushSettings { aliased: true, size: 1.0, ..b() });
    v.push(BrushSettings { tip: TipShape::Sampled(gtile(16, 12, 3)), size: 25.0, angle: 60.0, flip_x: true, ..b() });
    v.push(BrushSettings { tip: TipShape::Sampled(gtile(32, 32, 4)), size: 3.0, ..b() });
    v.push(BrushSettings { tip: TipShape::Sampled(gtile(9, 20, 5)), aliased: true, size: 14.0, roundness: 0.6, ..b() });
    v.push(BrushSettings {
        size: 24.0,
        shape_dynamics: ShapeDynamics {
            enabled: true,
            size: Dynamic { jitter: 0.7, minimum: 0.2, ..Default::default() },
            angle: Dynamic::jitter(1.0),
            roundness: Dynamic { jitter: 0.5, minimum: 0.3, ..Default::default() },
            flip_x_jitter: true,
            flip_y_jitter: true,
            ..Default::default()
        },
        ..b()
    });
    v.push(BrushSettings {
        shape_dynamics: ShapeDynamics { enabled: true, size: Dynamic { control: Control::Fade, fade_steps: 10, minimum: 0.1, ..Default::default() }, ..Default::default() },
        ..b()
    });
    v.push(BrushSettings {
        size: 30.0,
        roundness: 0.7,
        shape_dynamics: ShapeDynamics {
            enabled: true,
            size: Dynamic { control: Control::PenTilt, minimum: 0.6, ..Default::default() },
            tilt_scale: 1.5,
            brush_projection: true,
            ..Default::default()
        },
        ..b()
    });
    for c in [Control::Direction, Control::InitialDirection, Control::Rotation, Control::PenTilt, Control::PenPressure, Control::StylusWheel, Control::Fade] {
        v.push(BrushSettings {
            roundness: 0.4,
            size: 18.0,
            shape_dynamics: ShapeDynamics { enabled: true, angle: Dynamic { jitter: 0.1, control: c, fade_steps: 7, minimum: 0.0 }, ..Default::default() },
            ..b()
        });
    }
    v.push(BrushSettings {
        scattering: Scattering { enabled: true, scatter: Dynamic::jitter(2.0), both_axes: false, count: 4, count_jitter: Dynamic::jitter(0.6) },
        ..b()
    });
    v.push(BrushSettings {
        scattering: Scattering {
            enabled: true,
            scatter: Dynamic { jitter: 1.3, control: Control::PenPressure, ..Default::default() },
            both_axes: true,
            count: 3,
            count_jitter: Dynamic { control: Control::Fade, fade_steps: 5, ..Default::default() },
        },
        ..b()
    });
    v.push(BrushSettings {
        transfer: Transfer {
            enabled: true,
            opacity: Dynamic::controlled(Control::PenPressure),
            flow: Dynamic::jitter(0.8),
            wetness: Dynamic::controlled(Control::PenPressure),
            mix: Dynamic { jitter: 1.0, minimum: 0.25, ..Default::default() },
        },
        ..b()
    });
    v.push(BrushSettings { pressure_opacity: true, pressure_size: true, flow: 0.6, ..b() });
    v.push(BrushSettings {
        color_dynamics: ColorDynamics {
            enabled: true,
            per_tip: true,
            fg_bg: Dynamic::jitter(1.0),
            hue_jitter: 0.6,
            saturation_jitter: 0.4,
            brightness_jitter: 0.3,
            purity: 0.4,
        },
        ..b()
    });
    v.push(BrushSettings {
        color_dynamics: ColorDynamics {
            enabled: true,
            per_tip: false,
            fg_bg: Dynamic { control: Control::Fade, fade_steps: 6, jitter: 0.3, ..Default::default() },
            hue_jitter: 1.0,
            purity: -0.5,
            ..Default::default()
        },
        ..b()
    });
    v.push(BrushSettings {
        size: 30.0,
        texture: Texture { enabled: true, pattern: Pattern::Procedural { style: PatternStyle::Noise, size: 64, seed: 3 }, depth: 0.7, ..Default::default() },
        ..b()
    });
    v.push(BrushSettings {
        size: 28.0,
        texture: Texture {
            enabled: true,
            each_tip: true,
            mode: MaskMode::HardMix,
            scale: 0.6,
            brightness: 0.2,
            contrast: -0.3,
            invert: true,
            depth: 0.9,
            depth_jitter: Dynamic { jitter: 0.8, minimum: 0.2, ..Default::default() },
            ..Default::default()
        },
        ..b()
    });
    v.push(BrushSettings {
        size: 30.0,
        texture: Texture {
            enabled: true,
            pattern: Pattern::Procedural { style: PatternStyle::Canvas, size: 33, seed: 9 },
            mode: MaskMode::Subtract,
            scale: 2.5,
            depth: 0.8,
            ..Default::default()
        },
        ..b()
    });
    for (i, m) in [
        MaskMode::Darken,
        MaskMode::Overlay,
        MaskMode::ColorDodge,
        MaskMode::ColorBurn,
        MaskMode::LinearBurn,
        MaskMode::LinearHeight,
        MaskMode::Height,
        MaskMode::Multiply,
    ]
    .into_iter()
    .enumerate()
    {
        v.push(BrushSettings {
            size: 24.0,
            hardness: 0.4,
            texture: Texture {
                enabled: true,
                pattern: Pattern::Procedural { style: PatternStyle::Dots, size: 16 + i as u32 * 5, seed: i as u32 },
                mode: m,
                each_tip: i % 2 == 0,
                depth: 0.85,
                ..Default::default()
            },
            ..b()
        });
    }
    v.push(BrushSettings {
        size: 26.0,
        texture: Texture { enabled: true, pattern: Pattern::Tile(gtile(7, 5, 11)), scale: 1.0, depth: 1.0, ..Default::default() },
        ..b()
    });
    v.push(BrushSettings {
        size: 30.0,
        dual_brush: DualBrush {
            enabled: true,
            mode: MaskMode::Darken,
            size: 8.0,
            spacing: 2.0,
            hardness: 0.8,
            roundness: 0.6,
            angle: 20.0,
            scatter: 1.0,
            both_axes: true,
            count: 2,
            flip: true,
            ..Default::default()
        },
        ..b()
    });
    v.push(BrushSettings {
        size: 34.0,
        dual_brush: DualBrush { enabled: true, tip: TipShape::Sampled(gtile(12, 12, 21)), size: 15.0, spacing: 0.7, scatter: 0.5, ..Default::default() },
        ..b()
    });
    v.push(BrushSettings { noise: true, hardness: 0.0, size: 30.0, ..b() });
    v.push(BrushSettings { wet_edges: true, hardness: 0.2, size: 30.0, ..b() });
    v.push(BrushSettings { wet_edges: true, tip: TipShape::Sampled(gtile(10, 10, 31)), size: 22.0, ..b() });
    v.push(BrushSettings { build_up: true, build_up_rate: 40.0, flow: 0.2, ..b() });
    v.push(BrushSettings { spacing_enabled: false, ..b() });
    v.push(BrushSettings { smoothing: Smoothing { amount: 0.6, ..Default::default() }, ..b() });
    v.push(BrushSettings { smoothing: Smoothing { amount: 0.3, pulled_string: true, ..Default::default() }, ..b() });
    v.push(BrushSettings {
        pose: Pose { enabled: true, pressure: 0.4, tilt_x: 30.0, tilt_y: -20.0, rotation: 45.0, override_pressure: true, override_tilt: true, override_rotation: true },
        shape_dynamics: ShapeDynamics { enabled: true, size: Dynamic::controlled(Control::PenPressure), brush_projection: true, ..Default::default() },
        ..b()
    });
    v.push(BrushSettings { erase: true, ..b() });
    v.push(BrushSettings { mode: BlendMode::Multiply, color: [0.3, 0.6, 0.9, 1.0], ..b() });
    v.push(BrushSettings { mode: BlendMode::Dissolve, opacity: 0.6, size: 26.0, ..b() });
    for m in [BlendMode::Screen, BlendMode::Hue, BlendMode::Luminosity, BlendMode::Difference, BlendMode::LinearDodge] {
        v.push(BrushSettings { mode: m, size: 22.0, hardness: 0.6, ..b() });
    }
    v.push(BrushSettings { opacity: 0.5, flow: 0.3, size: 24.0, ..b() });
    v.push(BrushSettings { color_dynamics: ColorDynamics { enabled: true, fg_bg: Dynamic::jitter(0.7), ..Default::default() }, erase: true, ..b() });
    v
}

/// Hostile overrides: (name, bits) pairs applied after the JSON brush (non-finite floats can't be JSON).
fn overrides() -> Vec<Vec<(&'static str, f32)>> {
    vec![
        vec![("size", f32::INFINITY)],
        vec![("size", f32::NAN)],
        vec![("size", 1e30)],
        vec![("size", -5.0)],
        vec![("size", 0.0)],
        vec![("dual.size", f32::INFINITY), ("dual.enabled", 1.0)],
        vec![("scatter", 1e38), ("scatter.enabled", 1.0)],
        vec![("scatter", f32::NAN), ("scatter.enabled", 1.0)],
        vec![("scatter", f32::INFINITY), ("scatter.enabled", 1.0), ("dual.scatter", f32::INFINITY), ("dual.enabled", 1.0)],
        vec![("spacing", 0.0)],
        vec![("spacing", f32::NAN)],
        vec![("spacing", -1.0)],
        vec![("rate", f32::NAN), ("buildup", 1.0)],
        vec![("rate", 0.0), ("buildup", 1.0)],
        vec![("hardness", f32::NAN)],
        vec![("roundness", 0.0)],
        vec![("roundness", f32::NAN)],
        vec![("opacity", f32::NAN)],
        vec![("flow", f32::INFINITY)],
        vec![("angle", f32::INFINITY)],
        vec![("smooth", f32::NAN)],
        vec![("smooth", 1.0), ("pulled", 1.0)],
        vec![("color0", f32::NAN), ("color3", 2.0)],
    ]
}

fn apply_ovr(b: &mut BrushSettings, o: &[(&str, f32)]) {
    for &(k, v) in o {
        match k {
            "size" => b.size = v,
            "dual.size" => b.dual_brush.size = v,
            "dual.enabled" => b.dual_brush.enabled = v != 0.0,
            "scatter" => b.scattering.scatter.jitter = v,
            "scatter.enabled" => b.scattering.enabled = v != 0.0,
            "dual.scatter" => b.dual_brush.scatter = v,
            "spacing" => b.spacing = v,
            "rate" => b.build_up_rate = v,
            "buildup" => b.build_up = v != 0.0,
            "hardness" => b.hardness = v,
            "roundness" => b.roundness = v,
            "opacity" => b.opacity = v,
            "flow" => b.flow = v,
            "angle" => b.angle = v,
            "smooth" => b.smoothing.amount = v,
            "pulled" => b.smoothing.pulled_string = v != 0.0,
            "color0" => b.color[0] = v,
            "color3" => b.color[3] = v,
            _ => panic!("{k}"),
        }
    }
}

fn ovr_s(o: &[(&str, f32)]) -> String {
    if o.is_empty() {
        return "-".into();
    }
    o.iter().map(|(k, v)| format!("{k}={}", x32(*v))).collect::<Vec<_>>().join(",")
}

const HR: Rect = Rect { x0: -40, y0: -40, x1: 360, y1: 200 };

fn surf_h(s: &Surface) -> u64 {
    let mut h = H0;
    let mut b = s.to_interleaved(HR);
    if s.format().sample == SampleType::F32 {
        for c in b.chunks_mut(4) {
            let v = f32::from_le_bytes([c[0], c[1], c[2], c[3]]);
            c.copy_from_slice(&b32(v).to_le_bytes());
        }
    }
    fnv(&mut h, &b);
    h
}

/// Scenario surfaces: (target, selection, lock transparency).
fn scenario(i: usize) -> (Surface, Option<Surface>, bool) {
    let rgba = |s| PixelFormat::new(ColorMode::Rgb, s, true);
    let sel = || {
        let mut s = Surface::new(PixelFormat::GRAY8);
        s.fill_rect(Rect::new(0, 0, 100, 200), &[1.0]);
        s.fill_rect(Rect::new(100, 0, 160, 200), &[0.5]);
        s
    };
    let pre = |f: PixelFormat, r: Rect, c: [f32; 4]| {
        let mut s = Surface::new(f);
        s.fill_rect(r, &from_rgba(&f, c));
        s
    };
    match i {
        0 => (Surface::new(PixelFormat::RGBA8), None, false),
        1 => (Surface::new(PixelFormat::RGBA16), None, false),
        2 => (Surface::new(PixelFormat::RGBA32F), None, false),
        3 => (Surface::new(PixelFormat::GRAYA8), None, false),
        4 => (Surface::new(PixelFormat::CMYKA8), None, false),
        5 => {
            let f = PixelFormat::new(ColorMode::Rgb, SampleType::U8, false);
            (Surface::with_default(f, &from_rgba(&f, [1.0, 1.0, 1.0, 1.0])), None, false)
        }
        6 => (Surface::new(PixelFormat::new(ColorMode::Lab, SampleType::U16, true)), None, false),
        7 => (pre(rgba(SampleType::U8), Rect::new(0, 0, 120, 60), [0.2, 0.6, 0.9, 1.0]), None, true),
        8 => (pre(rgba(SampleType::F32), Rect::new(-50, -50, 350, 200), [0.5, 0.5, 0.5, 0.8]), None, false),
        9 => (Surface::new(PixelFormat::RGBA8), Some(sel()), false),
        10 => {
            let f = PixelFormat::new(ColorMode::Grayscale, SampleType::U16, false);
            (Surface::with_default(f, &[0.5]), None, false)
        }
        11 => {
            let f = PixelFormat::new(ColorMode::Cmyk, SampleType::U8, false);
            (Surface::with_default(f, &from_rgba(&f, [1.0, 1.0, 1.0, 1.0])), None, false)
        }
        12 => (pre(rgba(SampleType::U16), Rect::new(0, 0, 120, 60), [0.2, 0.6, 0.9, 1.0]), Some(sel()), false),
        _ => panic!(),
    }
}
const SCENARIOS: usize = 13;

fn bj(b: &BrushSettings) -> String { serde_json::to_string(b).unwrap() }

fn gen_all(b: &BrushSettings, pts: &[StrokePoint]) -> (Vec<Dab>, Vec<Dab>) {
    let mut g = DabGenerator::new(b, 1.0);
    let (mut d, mut u) = (Vec::new(), Vec::new());
    g.push(pts, &mut d, &mut u);
    g.finish(&mut d, &mut u);
    (d, u)
}

fn dabs_line(b: &BrushSettings, o: &[(&str, f32)], pts: &[StrokePoint]) {
    let mut bb = b.clone();
    apply_ovr(&mut bb, o);
    let (d, u) = gen_all(&bb, pts);
    let mut h = H0;
    for x in d.iter().chain(u.iter()) {
        dab_h(&mut h, x);
    }
    l!("dabs", bj(b), ovr_s(o), pts_s(pts), d.len(), u.len(), format!("{h:016x}"));
}

fn stroke_line(b: &BrushSettings, o: &[(&str, f32)], scen: usize, pts: &[StrokePoint]) {
    let mut bb = b.clone();
    apply_ovr(&mut bb, o);
    let (mut s, sel, lock) = scenario(scen);
    let dmg = render_stroke(&mut s, &bb, pts, sel.as_ref(), lock, 1.0);
    l!("stroke", bj(b), ovr_s(o), scen, pts_s(pts), rect_s(dmg), format!("{:016x}", surf_h(&s)));
}

fn dab_s(d: &Dab) -> String {
    format!(
        "{},{},{},{},{},{},{},{},{},{},{}",
        x64(d.center.x),
        x64(d.center.y),
        x32(d.radius),
        x32(d.alpha),
        x32(d.angle),
        x32(d.roundness),
        d.flip_x as u8,
        d.flip_y as u8,
        x32(d.depth),
        x32(d.proj_angle),
        x32(d.proj_scale)
    )
}

fn rast_line(b: &BrushSettings, o: &[(&str, f32)], d: &Dab, dual: bool) {
    let mut bb = b.clone();
    apply_ovr(&mut bb, o);
    let ctx = BrushContext::new(&bb);
    let r = ctx.dab_rect(d, dual);
    let mut h = H0;
    if (r.width() as u64) * (r.height() as u64) <= 1 << 22 {
        let mut v = Vec::new();
        ctx.rasterize(d, dual, r, &mut v);
        for x in &v {
            f32b(&mut h, *x);
        }
        if !dual {
            let f = ctx.footprint(d, 3);
            fnv(&mut h, rect_s(f.rect).as_bytes());
            u64le(&mut h, f.index as u64);
            for x in &f.cov {
                f32b(&mut h, *x);
            }
        }
    } else {
        fnv(&mut h, b"big");
    }
    l!("rast", bj(b), ovr_s(o), dab_s(d), dual as u8, rect_s(r), format!("{h:016x}"));
}

fn inc_line(b: &BrushSettings, scen: usize, pts: &[StrokePoint], chunk: usize) {
    let (s0, sel, lock) = scenario(scen);
    let fmt = s0.format();
    let pre = s0.clone();
    let mut live = s0.clone();
    let mut r = StrokeRenderer::new(b, Some(fmt), 1.0).record_dabs();
    let mut out = Vec::new();
    let mut hd = H0;
    for ch in pts.chunks(chunk.max(1)) {
        r.push(ch);
        let dirty = r.take_dirty_rect();
        out.push(rect_s(dirty));
        r.mark_dirty(dirty);
        let d = r.composite(&pre, &mut live, sel.as_ref(), lock, false);
        out.push(rect_s(d));
    }
    // Tail preview over the live composite.
    let mut tail_live = live.clone();
    match r.tail_preview() {
        Some(mut t) => {
            let d = t.composite(&pre, &mut tail_live, sel.as_ref(), lock, false);
            out.push(format!("T{}", rect_s(d)));
        }
        None => out.push("T-".into()),
    }
    out.push(format!("{:016x}", surf_h(&tail_live)));
    r.finish();
    let d = r.composite(&pre, &mut live, sel.as_ref(), lock, false);
    out.push(rect_s(d));
    out.push(format!("{:016x}", surf_h(&live)));
    let (cr, cv) = r.dense_coverage();
    for x in &cv {
        f32b(&mut hd, *x);
    }
    out.push(format!("{}:{hd:016x}", rect_s(cr)));
    let probe = Rect::new(-10, -10, 90, 50);
    let mut hc = H0;
    for x in r.coverage_in(probe) {
        f32b(&mut hc, x);
    }
    out.push(format!("{hc:016x}"));
    let mut hr = H0;
    for x in r.dabs() {
        dab_h(&mut hr, x);
    }
    out.push(format!("{}:{}:{hr:016x}", r.dab_count(), r.dabs().len()));
    l!("inc", bj(b), scen, pts_s(pts), chunk, out.join(" "));
}

fn union_line(b: &BrushSettings, scen: usize, pts: &[StrokePoint]) {
    let (s0, sel, lock) = scenario(scen);
    let fmt = s0.format();
    let pre = s0.clone();
    let mut target = s0.clone();
    let mirrored: Vec<StrokePoint> = pts.iter().map(|p| StrokePoint { x: 300.0 - p.x, ..*p }).collect();
    let mut a = StrokeRenderer::new(b, Some(fmt), 1.0);
    a.push(pts);
    a.finish();
    let mut m = StrokeRenderer::new(b, Some(fmt), 1.0);
    m.push(&mirrored);
    m.finish();
    let d = a.composite_union(&m, &pre, &mut target, sel.as_ref(), lock);
    l!("union", bj(b), scen, pts_s(pts), rect_s(d), format!("{:016x}", surf_h(&target)));
}

fn step(p: StrokePoint, dir: f32, init: f32, step: u64) -> StepInput { StepInput { point: p, direction: dir, initial_direction: init, step } }

fn ctl_steps() -> Vec<StepInput> {
    let vals = [0.0f32, -0.0, 0.3, 1.0, 1.7, -0.4, 89.0, 400.0, -720.5, f32::NAN, f32::INFINITY, f32::NEG_INFINITY];
    let mut v = Vec::new();
    for (i, &a) in vals.iter().enumerate() {
        for (j, &b) in vals.iter().enumerate() {
            let p = StrokePoint { pressure: a, tilt_x: b, tilt_y: a * 2.0, rotation: b * 3.0, wheel: a - b, ..pt(0.0, 0.0, 1.0) };
            v.push(step(p, a * 0.1, b * 0.05, (i * 13 + j * 7) as u64));
        }
    }
    v.push(step(pt(0.0, 0.0, 1.0), 0.0, 0.0, u64::MAX));
    v
}

fn json_cases() -> Vec<(&'static str, String)> {
    let mut v: Vec<(&'static str, String)> = Vec::new();
    for s in [
        r#"{}"#,
        r#"{"size": 50, "scattering": {"enabled": true, "scatter": {"jitter": 2}}, "pressure_size": false}"#,
        r#"{"size": 12, "shapeDynamics": {"enabled": true}, "transfer": {"enabled": true}}"#,
        r#"{"pressureSize": true, "pressure_size": false}"#,
        r#"{"pressure_opacity": true}"#,
        r#"{"size": "big"}"#,
        r#"{"size": null}"#,
        r#"{"size": true, "hardness": "x"}"#,
        r#"{"seed": -1}"#,
        r#"{"seed": 1.5}"#,
        r#"{"seed": 18446744073709551615}"#,
        r#"{"seed": 18446744073709551616}"#,
        r#"{"color": [1, 0, 0]}"#,
        r#"{"color": [1, 0, 0, 1, 5]}"#,
        r#"{"color": [1, "a", 0, 1]}"#,
        r#"{"color": {"r": 1}}"#,
        r#"{"mode": "Multiply"}"#,
        r#"{"mode": "multiply"}"#,
        r#"{"mode": {"Screen": null}}"#,
        r#"{"mode": 3}"#,
        r#"{"tip": "round"}"#,
        r#"{"tip": "sampled"}"#,
        r#"{"tip": {"round": null}}"#,
        r#"{"tip": {"round": 1}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": [0.0, 1.0]}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 2, "data": [0.0, 1.0]}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": "AAD//w=="}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": "AA D/\n/w=="}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": "AA*D//w=="}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": "AAD//w="}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": ["a"]}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "height": 1, "data": null}}}"#,
        r#"{"tip": {"sampled": {"width": 2, "data": [0.5, 0.5]}}}"#,
        r#"{"tip": {"sampled": {"width": -2, "height": 1, "data": "%%%"}}}"#,
        r#"{"tip": {"sampled": [2, 1, [0.25, 2.0]]}}"#,
        r#"{"tip": {"sampled": [2, 1]}}"#,
        r#"{"tip": {"sampled": [2, 1, [0, 1], 4]}}"#,
        r#"{"tip": {"sampled": 5}}"#,
        r#"{"tip": {"sampled": {"width": 0, "height": 0, "data": []}}}"#,
        r#"{"tip": {"sampled": {"width": 1, "height": 1, "data": "AAAA"}}}"#,
        r#"{"tip": {"cone": null}}"#,
        r#"{"tip": {"round": null, "sampled": null}}"#,
        r#"{"tip": {}}"#,
        r#"{"tip": 7}"#,
        r#"{"texture": {"enabled": true, "pattern": {"procedural": {"style": "dots", "size": 16, "seed": 2}}}}"#,
        r#"{"texture": {"pattern": {"procedural": {"style": "dots", "size": 16}}}}"#,
        r#"{"texture": {"pattern": {"procedural": {"style": "waves", "size": 16, "seed": 1}}}}"#,
        r#"{"texture": {"pattern": {"procedural": [1, 2, 3]}}}"#,
        r#"{"texture": {"pattern": "procedural"}}"#,
        r#"{"texture": {"pattern": "tile"}}"#,
        r#"{"texture": {"pattern": {"tile": {"width": 1, "height": 1, "data": [0.5]}}, "mode": "linearHeight"}}"#,
        r#"{"texture": {"mode": "screen"}}"#,
        r#"{"shapeDynamics": {"size": {"control": "penTilt", "fadeSteps": 3, "minimum": 0.5}}}"#,
        r#"{"shapeDynamics": {"size": {"control": "pen"}}}"#,
        r#"{"shapeDynamics": {"size": {"fadeSteps": 4294967296}}}"#,
        r#"{"shapeDynamics": {"size": {"fadeSteps": -1}}}"#,
        r#"{"shapeDynamics": {"size": [0.5, "fade", 3, 0.1]}}"#,
        r#"{"shapeDynamics": {"size": [0.5, "fade"]}}"#,
        r#"{"shapeDynamics": {"size": [0.5, "fade", 3, 0.1, 9]}}"#,
        r#"{"shapeDynamics": [true]}"#,
        r#"{"shapeDynamics": 3}"#,
        r#"{"locks": {"texture": true, "smoothing": 1}}"#,
        r#"{"mixer": {"wet": 0.9, "sampleAllLayers": true}}"#,
        r#"{"smoothing": {"amount": 0.5, "catchUp": false}}"#,
        r#"{"dualBrush": {"enabled": true, "count": 3.0}}"#,
        r#"{"colorDynamics": {"fgBg": {"jitter": 1e40}}}"#,
        r#"{"size": -0.0, "angle": 1e-50, "flow": 3.4028235e38, "opacity": 1e39}"#,
        r#"{"unknownField": [1, 2, {"x": null}], "size": 3}"#,
        r#"[20.0, 1.0]"#,
        r#"[]"#,
        r#""brush""#,
        r#"null"#,
    ] {
        v.push(("BrushSettings", s.into()));
    }
    for s in [
        r#"{"name": "A", "brush": {}}"#,
        r#"{"name": "A", "brush": {}, "builtin": true, "group": "G"}"#,
        r#"{"name": "A", "brush": {}, "group": ""}"#,
        r#"{"brush": {}}"#,
        r#"{"name": "A"}"#,
        r#"{"name": 5, "brush": {}}"#,
        r#"{"name": "A", "brush": {}, "builtin": "yes"}"#,
        r#"["N", {"size": 3}]"#,
        r#"["N", {"size": 3}, true, "grp"]"#,
        r#"["N"]"#,
        r#"["N", {}, false, "", 1]"#,
        r#"{"name": "é \"q\"\\\n\t\u0001", "brush": {"size": 1}}"#,
    ] {
        v.push(("BrushPreset", s.into()));
    }
    for s in [
        r#"{"brush": {}, "points": []}"#,
        r#"{"brush": {}, "points": [{"x": 1, "y": 2}, {"pressure": 0.5, "tiltX": 3, "time": 7.25}]}"#,
        r#"{"brush": {}}"#,
        r#"{"points": []}"#,
        r#"{"brush": {}, "points": {}}"#,
        r#"{"brush": {}, "points": [1]}"#,
        r#"{"brush": {}, "points": [[1, 2, 0.5]]}"#,
        r#"[{}, []]"#,
        r#"[{}]"#,
    ] {
        v.push(("Stroke", s.into()));
    }
    for s in [
        r#"{"width": 2, "height": 1, "data": [0.0, 1.0]}"#,
        r#"{"width": 3, "height": 1, "data": [0.1, -1.0, 7]}"#,
        r#"{"data": "AQIDBAUG", "width": 3, "height": 1}"#,
        r#"{"data": "AQIDBAUGBw==", "width": 3, "height": 1}"#,
        r#"{"data": "AQI", "width": 1, "height": 1}"#,
        r#"{"data": "AQ==", "width": 1, "height": 1}"#,
        r#"{"data": "====", "width": 1, "height": 1}"#,
        r#"{"data": "", "width": 0, "height": 5}"#,
        r#"{"width": 4294967295, "height": 4294967295, "data": []}"#,
        r#"{"width": 4294967296, "height": 1, "data": []}"#,
        r#"{"width": 1.0, "height": 1, "data": [1]}"#,
        r#"{"height": 1, "data": 3}"#,
        r#"{"width": 1, "height": 1}"#,
        r#"{}"#,
        r#"[1, 1, "AAA="]"#,
        r#"[1, 1.5, "AAA="]"#,
        r#"[1]"#,
        r#""tile""#,
    ] {
        v.push(("GrayTile", s.into()));
    }
    for s in [r#"{}"#, r#"{"x": -0, "y": 1e300, "pressure": 2, "wheel": -1, "time": -5}"#, r#"[1, 2]"#, r#"[1, 2, 3, 4, 5, 6, 7, 8, 9]"#, r#"{"x": "1"}"#] {
        v.push(("StrokePoint", s.into()));
    }
    for s in [r#""off""#, r#""direction""#, r#"{"fade": null}"#, r#"{"fade": {}}"#, r#""Fade""#, r#"[]"#] {
        v.push(("Control", s.into()));
    }
    for s in [r#"{"jitter": 1}"#, r#"{"control": "rotation", "minimum": "0"}"#] {
        v.push(("Dynamic", s.into()));
    }
    for s in [r#"{"procedural": {"style": "noise", "size": 1, "seed": 0}}"#, r#"{"procedural": {"size": 1, "seed": 0, "style": "paper", "x": 1}}"#, r#"{"tile": [1, 1, [0.25]]}"#, r#"{"procedural": null}"#] {
        v.push(("Pattern", s.into()));
    }
    v
}

fn de_line(ty: &str, input: &str) {
    let val: J = serde_json::from_str(input).unwrap();
    macro_rules! go {
        ($t:ty) => {
            match serde_json::from_value::<$t>(val) {
                Ok(x) => serde_json::to_string(&x).unwrap(),
                Err(e) => format!("ERR {}", trunc(e.to_string())),
            }
        };
    }
    let r = match ty {
        "BrushSettings" => go!(BrushSettings),
        "BrushPreset" => go!(BrushPreset),
        "Stroke" => go!(Stroke),
        "GrayTile" => go!(GrayTile),
        "StrokePoint" => go!(StrokePoint),
        "Control" => go!(Control),
        "Dynamic" => go!(Dynamic),
        "Pattern" => go!(Pattern),
        _ => panic!(),
    };
    assert!(!input.contains('\n') && !input.contains('\t') && !r.contains('\n') && !r.contains('\t'));
    l!("de", ty, input, r);
}

pub fn run() {
    // ---- rng ----
    for seed in [0u64, 1, 7, 42, 0xD0A1, u64::MAX, 0x123456789ABCDEF0] {
        let mut h = H0;
        for i in (0..300u64).chain([u64::MAX, u64::MAX - 1, 1 << 63]) {
            for st in 0..22u64 {
                f32b(&mut h, rand01(seed, i, st));
                f32b(&mut h, rand_signed(seed, i, st));
            }
        }
        l!("rng", seed, format!("{h:016x}"));
    }
    for x in [0u64, 1, 0x9E3779B97F4A7C15, u64::MAX, 12345678901234567890] {
        l!("mix", x, mix64(x));
    }
    for salt in [0u64, 0x6E6F697365, 7, u64::MAX] {
        let mut h = H0;
        for y in (-40..40).chain([i32::MIN, i32::MAX, -1]) {
            for x in (-40..40).chain([i32::MIN, i32::MAX, -1]) {
                f32b(&mut h, hash2(x, y, salt));
            }
        }
        l!("hash2", salt, format!("{h:016x}"));
    }
    for b in [&b""[..], b"a", b"points [1,2]", &[0u8, 255, 128, 7][..]] {
        l!("seedb", hex(b), seed_from_bytes(b));
    }
    // ---- tiles ----
    for v in [0.0f32, -0.0, 1.0, 0.5, 0.25, 1e-6, 0.999999, 1.5, -2.0, f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 0.00000763, 0.5000076] {
        l!("q16", x32(v), GrayTile::from_f32(1, 1, &[v]).data[0]);
    }
    for b in [&b""[..], b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar", &[0u8, 255, 254, 1, 2][..]] {
        l!("b64e", hex(b), b64_encode(b));
    }
    for s in ["", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", " Zm9v\nYmFy\t", "Zm9vY", "Zm=v", "Zg===", "====", "Z===", "Zm9\x0bv", "Zh==", "AA\rAA\x0c", "Zg!=", "ab$d"] {
        let r = match b64_decode(s) {
            Some(v) => hex(&v),
            None => "ERR".into(),
        };
        l!("b64d", hex(s.as_bytes()), r);
    }
    for (w, h, seed) in [(1u32, 1u32, 1u64), (2, 1, 2), (16, 16, 3), (17, 5, 4), (1, 33, 5), (64, 64, 6), (13, 13, 7)] {
        let t = gtile(w, h, seed);
        let m = Mips::new(&t);
        let mut hh = H0;
        u64le(&mut hh, m.levels.len() as u64);
        for (lw, lh, d) in &m.levels {
            u64le(&mut hh, *lw as u64);
            u64le(&mut hh, *lh as u64);
            for x in d {
                f32b(&mut hh, *x);
            }
        }
        for px in [0.0f32, 0.5, 1.0, 2.0, 3.0, 7.9, 8.0, 100.0, f32::NAN, f32::INFINITY, -4.0, 1e-30] {
            u64le(&mut hh, m.level_for(px) as u64);
        }
        for lv in 0..m.levels.len() {
            for i in -3..=23 {
                for j in -3..=23 {
                    f32b(&mut hh, m.sample_clamped(lv, i as f32 / 20.0, j as f32 / 20.0));
                }
            }
            f32b(&mut hh, m.sample_clamped(lv, f32::NAN, 0.5));
            f32b(&mut hh, m.sample_clamped(lv, 1e30, -1e30));
        }
        let tf: u64 = t.to_f32().iter().map(|v| v.to_bits() as u64).sum();
        u64le(&mut hh, tf);
        f32b(&mut hh, t.get(w - 1, h - 1));
        l!("mips", w, h, seed, format!("{hh:016x}"));
    }
    for style in [PatternStyle::Noise, PatternStyle::Canvas, PatternStyle::Paper, PatternStyle::Dots] {
        for (size, seed) in [(0u32, 0u32), (8, 1), (33, 2), (64, 3), (128, 1), (2000, 5), (100, u32::MAX)] {
            let p = procedural::pattern(style, size, seed);
            let mut h = H0;
            u64le(&mut h, p.width as u64);
            for x in &p.data {
                f32b(&mut h, *x);
            }
            for i in -20..20 {
                f32b(&mut h, p.sample_wrap(i as f32 * 3.7, i as f32 * -11.3));
            }
            f32b(&mut h, p.sample_wrap(f32::NAN, f32::INFINITY));
            f32b(&mut h, p.sample_wrap(1e30, -1e30));
            l!("pat", serde_json::to_string(&style).unwrap().trim_matches('"'), size, seed, format!("{h:016x}"));
        }
    }
    let tiph = |t: &GrayTile| {
        let mut h = H0;
        u64le(&mut h, t.width as u64);
        u64le(&mut h, t.height as u64);
        for x in &t.data {
            fnv(&mut h, &x.to_le_bytes());
        }
        format!("{h:016x}")
    };
    for (n, seed) in [(0u32, 0u32), (1, 1), (7, 3), (48, 1), (64, 7), (96, 8)] {
        l!("tip", "chalk", n, seed, 0, tiph(&procedural::chalk_tip(n, seed)));
        l!("tip", "bristle", n, seed, 0, tiph(&procedural::bristle_tip(n, seed)));
        l!("tip", "charcoal", n, seed, 0, tiph(&procedural::charcoal_tip(n, seed)));
        l!("tip", "leaf", n, 0, 0, tiph(&procedural::leaf_tip(n)));
        l!("tip", "sponge", n, seed, 0, tiph(&procedural::sponge_tip(n, seed)));
        l!("tip", "star", n, 0, 0, tiph(&procedural::star_tip(n)));
        for extra in [0u32, 1, 12, 40] {
            l!("tip", "spatter", n, seed, extra, tiph(&procedural::spatter_tip(n, seed, extra)));
            l!("tip", "grass", n, seed, extra, tiph(&procedural::grass_tip(n, seed, extra)));
            l!("tip", "rake", n, seed, extra, tiph(&procedural::rake_tip(n, seed, extra)));
        }
    }
    // ---- coverage, masks, grid ----
    let sweep = [0.0f32, -0.0, 0.1, 0.25, 0.5, 0.75, 0.999, 1.0, 1.2, 3.0, -0.3, 9.9, 10.0, 10.4, 10.5, f32::NAN, f32::INFINITY, f32::NEG_INFINITY, 1e-4];
    let mut h = H0;
    for &d in &sweep {
        for &r in &[0.5f32, 1.0, 10.0, 0.0, -1.0, 2500.0, f32::NAN, f32::INFINITY, 0.0005] {
            for &hd in &[0.0f32, 0.5, 1.0, 0.9999, f32::NAN, -1.0, 2.0] {
                f32b(&mut h, dab_coverage(d * r.abs().max(1.0), r, hd));
            }
        }
    }
    l!("cov", format!("{h:016x}"));
    for m in [
        MaskMode::Multiply,
        MaskMode::Subtract,
        MaskMode::Darken,
        MaskMode::Overlay,
        MaskMode::ColorDodge,
        MaskMode::ColorBurn,
        MaskMode::LinearBurn,
        MaskMode::HardMix,
        MaskMode::LinearHeight,
        MaskMode::Height,
    ] {
        let mut h = H0;
        for &v in &sweep {
            for &t in &sweep {
                for &d in &[0.0f32, 0.3, 0.8, 1.0, f32::NAN, 1.5] {
                    f32b(&mut h, mask_combine(m, v, t, d));
                }
            }
        }
        l!("mask", serde_json::to_string(&m).unwrap().trim_matches('"'), format!("{h:016x}"));
    }
    for (x, y, d) in [
        (10.0, 5.0, 1.0f32),
        (10.9, 5.2, 3.0),
        (10.4, 5.6, 2.0),
        (10.2, 5.7, 4.0),
        (-3.5, -2.5, 2.0),
        (-3.5, -2.5, 5.0),
        (f64::NAN, 0.0, f32::NAN),
        (1e300, -1e300, 7.0),
        (2.5, 3.5, 0.2),
        (7.0, 7.0, f32::INFINITY),
        (0.5, 0.5, 1e9),
        (0.5, 0.5, -3.0),
        (4.5, -4.5, 6.4999),
    ] {
        let (cx, cy) = grid_center(x, y, d);
        let sq = grid_square(x, y, d);
        l!("grid", x64(x), x64(y), x32(d), x32(cx), x32(cy), sq.iter().map(|v| x64(*v)).collect::<Vec<_>>().join(","));
    }
    for (c, r) in [
        (f64::INFINITY, 10.0f32),
        (f64::NEG_INFINITY, 10.0),
        (f64::NAN, 10.0),
        (1e300, 10.0),
        (-1e300, 10.0),
        (f64::from(i32::MAX), 10.0),
        (f64::from(i32::MIN), 10.0),
        (5.0, f32::INFINITY),
        (5.0, f32::NAN),
        (5.5, 3.2),
        (-7.25, 0.0),
        (100.0, 3e9),
    ] {
        let d = Dab::round(Point::new(c, c), r, 1.0);
        l!("drect", x64(c), x64(c), x32(r), rect_s(photocraft_paint::retouch::dab_rect(&d)));
    }
    // ---- dynamics ----
    let mut h = H0;
    for s in ctl_steps() {
        for c in [Control::Off, Control::Fade, Control::PenPressure, Control::PenTilt, Control::StylusWheel, Control::Rotation, Control::InitialDirection, Control::Direction] {
            for fs in [0u32, 1, 25, 400] {
                f32b(&mut h, control_value(c, fs, &s));
                f32b(&mut h, control_angle(c, fs, &s));
            }
        }
    }
    l!("ctl", format!("{h:016x}"));
    let cbr = brushes();
    for b in [&cbr[24], &cbr[25], &BrushSettings { color: [0.0, 0.0, 0.0, 1.0], background: [1.0, 1.0, 1.0, 0.0], ..cbr[24].clone() }] {
        let mut h = H0;
        for (k, s) in ctl_steps().iter().enumerate().step_by(5) {
            for c in dynamic_color(b, s, 99, k as u64) {
                f32b(&mut h, c);
            }
        }
        l!("color", bj(b), format!("{h:016x}"));
    }
    let sets = point_sets();
    let zig: Vec<StrokePoint> = (0..40).map(|i| StrokePoint { time: i as f64 * 7.0, ..pt(i as f64 * 5.0, if i % 2 == 0 { 0.0 } else { 6.0 }, 1.0) }).collect();
    for cfg in [
        Smoothing::default(),
        Smoothing { amount: 0.8, ..Default::default() },
        Smoothing { amount: 0.8, catch_up_on_end: false, ..Default::default() },
        Smoothing { amount: 0.8, catch_up: false, ..Default::default() },
        Smoothing { amount: 0.2, pulled_string: true, catch_up_on_end: false, ..Default::default() },
        Smoothing { amount: 0.5, pulled_string: true, adjust_for_zoom: false, ..Default::default() },
        Smoothing { amount: 1.5, ..Default::default() },
        Smoothing { amount: f32::NAN, ..Default::default() },
    ] {
        for zoom in [1.0f32, 2.0, 0.0, f32::NAN] {
            for pts in [&zig, &sets[1], &sets[3]] {
                let out = smooth_points(&cfg, zoom, pts);
                let mut h = H0;
                for p in &out {
                    for v in [p.x, p.y, p.time] {
                        f64b(&mut h, v);
                    }
                    for v in [p.pressure, p.tilt_x, p.tilt_y, p.rotation, p.wheel] {
                        f32b(&mut h, v);
                    }
                }
                let c = format!(
                    "{},{},{},{},{}",
                    x32(cfg.amount),
                    cfg.pulled_string as u8,
                    cfg.catch_up as u8,
                    cfg.catch_up_on_end as u8,
                    cfg.adjust_for_zoom as u8
                );
                l!("smooth", c, x32(zoom), pts_s(pts), format!("{}:{h:016x}", out.len()));
            }
        }
    }
    for amount in [0.0f32, 0.5, 0.95, 2.0, f32::NAN] {
        let out = smooth(&zig, amount);
        let mut h = H0;
        for p in &out {
            f64b(&mut h, p.x);
            f64b(&mut h, p.y);
        }
        l!("lsmooth", x32(amount), pts_s(&zig), format!("{h:016x}"));
    }
    // ---- dabs ----
    let bs = brushes();
    for b in &bs {
        for pts in &sets {
            dabs_line(b, &[], pts);
        }
    }
    for o in overrides() {
        for (bi, pi) in [(1usize, 0usize), (38, 3), (43, 4), (45, 1)] {
            dabs_line(&bs[bi], &o, &sets[pi]);
        }
    }
    for pts in hostile_point_sets() {
        for bi in [1usize, 20, 38, 43, 44, 45] {
            dabs_line(&bs[bi], &[], &pts);
        }
    }
    // ---- rasterize ----
    let rdabs = {
        let mut v = vec![Dab::round(Point::new(50.0, 50.0), 20.0, 1.0), Dab::round(Point::new(10.3, -4.7), 3.3, 0.6)];
        let mut d = Dab::round(Point::new(50.5, 50.5), 12.0, 0.8);
        d.roundness = 0.25;
        d.angle = 0.7;
        d.flip_x = true;
        d.depth = 0.4;
        v.push(d);
        let mut d = Dab::round(Point::new(30.0, 40.0), 15.0, 1.0);
        d.proj_scale = 0.4;
        d.proj_angle = -1.1;
        d.flip_y = true;
        v.push(d);
        v.push(Dab::round(Point::new(5.0, 5.0), 0.2, 1.0));
        v.push(Dab::round(Point::new(f64::NAN, 3.0), 4.0, 1.0));
        let mut d = Dab::round(Point::new(7.5, 7.5), 6.0, 1.0);
        d.roundness = 0.0;
        v.push(d);
        v.push(Dab::round(Point::new(1e12, 5.0), 4.0, 1.0));
        v
    };
    for b in &bs {
        for d in &rdabs {
            rast_line(b, &[], d, false);
        }
        if b.dual_brush.enabled {
            for d in &rdabs {
                rast_line(b, &[], d, true);
            }
        }
    }
    for o in [vec![("hardness", f32::NAN)], vec![("roundness", f32::NAN)], vec![("size", f32::INFINITY)]] {
        rast_line(&bs[2], &o, &rdabs[0], false);
    }
    for b in bs.iter().filter(|b| b.texture.enabled) {
        let ctx = BrushContext::new(b);
        let mut h = H0;
        for y in (-70..70).chain([i32::MIN, i32::MAX]) {
            for x in (-70..70).step_by(3).chain([i32::MIN, i32::MAX]) {
                f32b(&mut h, ctx.texture_at(x, y));
            }
        }
        l!("tex", bj(b), format!("{h:016x}"));
    }
    // ---- strokes ----
    for (i, b) in bs.iter().enumerate() {
        stroke_line(b, &[], i % SCENARIOS, &sets[i % sets.len()]);
        stroke_line(b, &[], (i * 5 + 3) % SCENARIOS, &sets[(i + 3) % sets.len()]);
    }
    for scen in 0..SCENARIOS {
        for bi in [1usize, 2, 4, 7, 21, 24, 27, 38, 41, 48, 49, 51, 56] {
            stroke_line(&bs[bi], &[], scen, &sets[3]);
        }
    }
    for o in overrides() {
        stroke_line(&bs[1], &o, 0, &sets[0]);
        stroke_line(&bs[38], &o, 2, &sets[3]);
    }
    for pts in hostile_point_sets() {
        for bi in [1usize, 24, 50] {
            stroke_line(&bs[bi], &[], 2, &pts);
        }
    }
    // ---- incremental, union ----
    for (i, b) in bs.iter().enumerate().step_by(3) {
        inc_line(b, i % SCENARIOS, &sets[3], 1 + i % 7);
    }
    for bi in [38usize, 45, 24] {
        inc_line(&bs[bi], 1, &sets[1], 4);
        inc_line(&bs[bi], 9, &sets[2], 1);
    }
    for (i, b) in bs.iter().enumerate().step_by(4) {
        union_line(b, (i + 2) % SCENARIOS, &sets[i % sets.len()]);
    }
    // ---- presets ----
    let ps = presets::builtin();
    let ppts: Vec<StrokePoint> = (0..20).map(|i| StrokePoint { time: i as f64 * 16.0, ..pt(40.0 + i as f64 * 8.0, 60.0, 0.8) }).collect();
    for (i, p) in ps.iter().enumerate() {
        let mut s = Surface::new(PixelFormat::RGBA8);
        let dmg = render_stroke(&mut s, &p.brush, &ppts, None, false, 1.0);
        l!("preset", i, serde_json::to_string(p).unwrap(), rect_s(dmg), format!("{:016x}", surf_h(&s)));
    }
    let chalk = presets::find(&ps, "chalk").unwrap().brush.clone();
    let canvas = presets::find(&ps, "Canvas Texture").unwrap().brush.clone();
    let mut cur = BrushSettings {
        scattering: Scattering { enabled: true, count: 5, ..Default::default() },
        transfer: Transfer { enabled: true, wetness: Dynamic::jitter(0.4), ..Default::default() },
        noise: true,
        smoothing: Smoothing { amount: 0.4, ..Default::default() },
        build_up_rate: 77.0,
        ..base()
    };
    for locks in [
        SectionLocks::default(),
        SectionLocks { scattering: true, transfer: true, noise: true, ..Default::default() },
        SectionLocks {
            shape_dynamics: true,
            scattering: true,
            texture: true,
            dual_brush: true,
            color_dynamics: true,
            transfer: true,
            pose: true,
            noise: true,
            wet_edges: true,
            build_up: true,
            smoothing: true,
            protect_texture: true,
        },
    ] {
        cur.locks = locks;
        l!("pick", "over", bj(&chalk), bj(&cur), bj(&chalk.clone().picked_over(&cur)));
        l!("pick", "over", bj(&canvas), bj(&cur), bj(&canvas.clone().picked_over(&cur)));
    }
    let prot = BrushSettings { protect_texture: true, ..chalk.clone() };
    l!("pick", "protect", bj(&canvas), bj(&prot), bj(&canvas.clone().with_protected_texture(&prot)));
    l!("pick", "protect", bj(&canvas), bj(&chalk), bj(&canvas.clone().with_protected_texture(&chalk)));
    l!("pick", "over", bj(&canvas), bj(&prot), bj(&canvas.clone().picked_over(&prot)));
    for o in overrides() {
        let mut b = bs[38].clone();
        apply_ovr(&mut b, &o);
        let r = b.bounded_for_render();
        let mut h = H0;
        for v in [r.size, r.dual_brush.size, r.scattering.scatter.jitter, r.dual_brush.scatter] {
            f32b(&mut h, v);
        }
        l!("pick", "bounded", bj(&bs[38]), ovr_s(&o), format!("{h:016x}"));
    }
    // ---- serde ----
    for b in &bs {
        l!("ser", "BrushSettings", bj(b));
    }
    l!("ser", "Stroke", serde_json::to_string(&Stroke { brush: bs[9].clone(), points: sets[1].clone() }).unwrap());
    l!("ser", "BrushPreset", serde_json::to_string(&BrushPreset { name: "x".into(), brush: bs[2].clone(), builtin: false, group: String::new() }).unwrap());
    l!("ser", "GrayTile", serde_json::to_string(&gtile(5, 3, 1)).unwrap());
    for b in [&bs[1], &bs[7], &bs[24], &bs[40]] {
        l!("serv", "BrushSettings", bj(b), serde_json::to_string(&serde_json::to_value(b).unwrap()).unwrap());
    }
    l!(
        "serv",
        "BrushPreset",
        serde_json::to_string(&ps[20]).unwrap(),
        serde_json::to_string(&serde_json::to_value(&ps[20]).unwrap()).unwrap()
    );
    for (ty, s) in json_cases() {
        de_line(ty, &s);
    }
}
