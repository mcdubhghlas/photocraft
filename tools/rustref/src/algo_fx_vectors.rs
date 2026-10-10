//! L6 algo filters reference vectors (`rustref algo-fx-vectors`): the distort, stylize,
//! pixelate, render, Blur Gallery, other and video filters, Filter Gallery, the procedural
//! renderers (render2), Trap and mesh warps. One `key value` line per case; values are FNV-1a
//! hashes (f32/f64 bit patterns little-endian) or plain text. Replayed by
//! tests/algo/test_fx_parity.c, which reads the filter parameters back from the JSON in the lines
//! themselves (`fx`/`fxk` lines), so the case lists live here only.
use photocraft_algo::render2::{self, FlameSpec, FrameSpec, Prim, Shape, TreeSpec};
use photocraft_algo::transform::{Homography, Interp};
use photocraft_algo::*;
use photocraft_color::{ColorMode, PixelFormat, SampleType};
use photocraft_geom::warp::{BezierMesh, Warp, WarpStyle};
use photocraft_geom::{Affine, Rect};
use photocraft_raster::Surface;
use std::sync::Arc;

const H0: u64 = 0xcbf29ce484222325;

fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn hf(h: &mut u64, v: &[f32]) {
    for x in v {
        fnv(h, &x.to_bits().to_le_bytes());
    }
}
fn hi(h: &mut u64, v: &[i32]) {
    for x in v {
        fnv(h, &x.to_le_bytes());
    }
}
fn hrect(h: &mut u64, r: Rect) {
    hi(h, &[r.x0, r.y0, r.x1, r.y1]);
}
fn hu(h: &mut u64, v: u64) {
    fnv(h, &v.to_le_bytes());
}
/// Content bounds, the samples inside them and the default pixel.
fn hs(s: &Surface) -> u64 {
    let mut h = H0;
    let cb = s.content_bounds();
    hrect(&mut h, cb);
    hf(&mut h, &s.read_region(cb));
    hf(&mut h, &s.default_pixel());
    fnv(&mut h, &[s.format().alpha as u8, s.channels() as u8]);
    h
}

/// Deterministic sample in 0..=1 (C: the same integer formula).
fn pat(x: i32, y: i32, c: usize, seed: i32) -> f32 {
    ((x as i64 * 7 + y as i64 * 13 + c as i64 * 29 + seed as i64 * 31).rem_euclid(97)) as f32 / 96.0
}
fn alpha_at(x: i32, y: i32, seed: i32) -> f32 {
    ((x as i64 * 3 + y as i64 * 5 + seed as i64).rem_euclid(7)) as f32 / 6.0
}
fn surf(fmt: PixelFormat, r: Rect, seed: i32, opaque: bool) -> Surface {
    let mut s = Surface::new(fmt);
    let n = fmt.channels();
    let mut v = Vec::new();
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            for c in 0..n {
                v.push(if fmt.alpha && c == n - 1 { if opaque { 1.0 } else { alpha_at(x, y, seed) } } else { pat(x, y, c, seed) });
            }
        }
    }
    s.write_region(r, &v);
    s
}

fn formats() -> Vec<(&'static str, PixelFormat)> {
    vec![
        ("rgba8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, true)),
        ("rgba16", PixelFormat::new(ColorMode::Rgb, SampleType::U16, true)),
        ("rgba32", PixelFormat::new(ColorMode::Rgb, SampleType::F32, true)),
        ("rgb8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, false)),
        ("gray8", PixelFormat::new(ColorMode::Grayscale, SampleType::U8, false)),
        ("graya16", PixelFormat::new(ColorMode::Grayscale, SampleType::U16, true)),
        ("cmyka8", PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true)),
        ("laba16", PixelFormat::new(ColorMode::Lab, SampleType::U16, true)),
        ("laba32", PixelFormat::new(ColorMode::Lab, SampleType::F32, true)),
    ]
}
fn fmt_named(name: &str) -> PixelFormat {
    formats().into_iter().find(|(n, _)| *n == name).map(|(_, f)| f).unwrap_or(PixelFormat::RGBA8)
}

const R: Rect = Rect { x0: 0, y0: 0, x1: 40, y1: 30 };

/// The selections and surfaces every case runs against.
struct Env {
    sel_grad: Surface,
}

/// The apply sequence every filter case hashes (C: run_case).
fn run_case(env: &Env, fmt: PixelFormat, p: &FilterParams) -> u64 {
    let s = surf(fmt, Rect::new(-4, 2, 37, 33), 9, false);
    let opaque = surf(fmt, R, 1, true);
    let mut h = H0;
    let area = output_area(p, s.content_bounds(), R, None);
    hrect(&mut h, area);
    // Wide halos make tiny tiles slow; the results don't depend on the tiling either way.
    let small = match p.halo() {
        Halo::Radius(r) if r > 12 => 37,
        _ => 7,
    };
    for tile in [256, small, 64] {
        hu(&mut h, hs(&apply_tiled(&s, p, area, R, None, tile, None)));
    }
    hu(&mut h, hs(&apply(&s, p, area, R, Some(&env.sel_grad))));
    hu(&mut h, hs(&apply_in(&opaque, p, output_area(p, opaque.content_bounds(), R, None), R, None, R)));
    hu(&mut h, hs(&apply_tiled(&s, p, Rect::new(-9, -9, 50, 50), Rect::new(5, 5, 25, 20), None, 9, Some(Rect::new(-2, -2, 30, 31)))));
    h
}

fn light(kind: LightKind) -> Light {
    Light { kind, ..Light::default() }
}

/// The filter cases (everything but Displace maps and the Filter Gallery).
fn cases() -> Vec<FilterParams> {
    use FilterParams as F;
    let mut v = Vec::new();
    for r in [0.0, 0.4, 1.0, 1.5, 2.5, 3.0, 6.0, -2.0] {
        for preserve in [Preserve::Squareness, Preserve::Roundness] {
            v.push(F::Minimum { radius: r, preserve });
            v.push(F::Maximum { radius: r, preserve });
        }
    }
    for (horizontal, vertical, undefined) in [(7, -3, UndefinedAreas::Wrap), (-45, 31, UndefinedAreas::Repeat), (5, 0, UndefinedAreas::Transparent), (0, 0, UndefinedAreas::Wrap)] {
        v.push(F::Offset { horizontal, vertical, undefined });
    }
    for c in [0.0, 1.0, 1.5, 2.0, 6.0, 13.0, 50.0, -3.0] {
        v.push(F::Mosaic { cell_size: c });
    }
    for (angle, height, amount) in [(135.0, 2.0, 100.0), (0.0, 1.0, 250.0), (-30.0, 0.0, 50.0), (400.0, 7.5, -100.0), (90.0, -3.0, 500.0)] {
        v.push(F::Emboss { angle, height, amount });
    }
    v.extend([F::FindEdges, F::Solarize, F::Invert, F::Desaturate]);
    for a in [0.0, 90.0, -400.0, 999.0] {
        v.push(F::Twirl { angle: a });
    }
    for a in [0.0, 50.0, -100.0, 150.0, -35.0] {
        v.push(F::Pinch { amount: a });
    }
    for a in [0.0, 60.0, -60.0, 100.0, -100.0] {
        for mode in [SpherizeMode::Normal, SpherizeMode::HorizontalOnly, SpherizeMode::VerticalOnly] {
            v.push(F::Spherize { amount: a, mode });
        }
    }
    for (g, wt, und, seed) in [
        (3, WaveType::Triangle, UndefinedAreas::Repeat, 9),
        (1, WaveType::Sine, UndefinedAreas::Wrap, 0),
        (5, WaveType::Square, UndefinedAreas::Transparent, 77),
        (0, WaveType::Sine, UndefinedAreas::Repeat, 3),
        (200, WaveType::Triangle, UndefinedAreas::Wrap, 4),
    ] {
        v.push(F::Wave { generators: g, wavelength_min: 5.0, wavelength_max: 20.0, amplitude_min: 1.0, amplitude_max: 4.0, wave_type: wt, undefined: und, seed });
    }
    v.push(F::Wave {
        generators: 2,
        wavelength_min: 30.0,
        wavelength_max: 2.0,
        amplitude_min: -5.0,
        amplitude_max: 9.0,
        wave_type: WaveType::Sine,
        undefined: UndefinedAreas::Repeat,
        seed: 12,
    });
    for (a, size) in [(200.0, RippleSize::Small), (0.0, RippleSize::Large), (-500.0, RippleSize::Medium), (999.0, RippleSize::Large)] {
        v.push(F::Ripple { amount: a, size });
    }
    v.push(F::PolarCoordinates { mode: PolarMode::RectangularToPolar });
    v.push(F::PolarCoordinates { mode: PolarMode::PolarToRectangular });
    for (r, angles) in [(4.0, [108.0, 162.0, 90.0, 45.0]), (1.0, [0.0, 0.0, 0.0, 0.0]), (10.0, [15.0, 75.0, 0.0, 45.0]), (0.5, [-30.0, 400.0, 90.0, 45.0])] {
        v.push(F::ColorHalftone { max_radius: r, angles });
    }
    for (c, seed) in [(6.0, 1), (3.0, 2), (20.0, 3), (1.0, 4), (9.5, 0)] {
        v.push(F::Crystallize { cell_size: c, seed });
    }
    v.extend([F::Facet, F::Fragment]);
    for kind in [
        MezzotintType::FineDots,
        MezzotintType::MediumDots,
        MezzotintType::GrainyDots,
        MezzotintType::CoarseDots,
        MezzotintType::ShortLines,
        MezzotintType::MediumLines,
        MezzotintType::LongLines,
        MezzotintType::ShortStrokes,
        MezzotintType::MediumStrokes,
        MezzotintType::LongStrokes,
    ] {
        v.push(F::Mezzotint { kind, seed: 2 });
    }
    for (c, seed, bg) in [(5.0, 3, [1.0, 1.0, 1.0, 1.0]), (10.0, 1, [1.0, 0.0, 0.0, 1.0]), (2.0, 7, [0.2, 0.5, 0.9, 0.0]), (7.0, 9, [0.0, 0.0, 0.0, 0.5])] {
        v.push(F::Pointillize { cell_size: c, seed, background: bg });
    }
    for mode in [DiffuseMode::Normal, DiffuseMode::DarkenOnly, DiffuseMode::LightenOnly, DiffuseMode::Anisotropic] {
        v.push(F::Diffuse { mode, seed: 4 });
    }
    for (kind, size, depth, lb, sf, mi) in [
        (ExtrudeType::Blocks, 6.0, 20.0, false, false, false),
        (ExtrudeType::Blocks, 8.0, 40.0, false, true, false),
        (ExtrudeType::Blocks, 5.0, 40.0, true, false, true),
        (ExtrudeType::Blocks, 2.0, 1.0, true, true, true),
        (ExtrudeType::Pyramids, 6.0, 30.0, true, false, true),
        (ExtrudeType::Pyramids, 10.0, 100.0, false, false, false),
        (ExtrudeType::Pyramids, 1.0, -5.0, false, false, false),
    ] {
        v.push(F::Extrude { kind, size, depth, level_based: lb, solid_front: sf, mask_incomplete: mi, seed: 5 });
    }
    for (st, cl, sc, bd, li, an, sh) in [
        (3.0, 4.0, 1.0, 5.0, true, -60.0, 2.0),
        (5.0, 5.0, 2.0, 0.0, false, 0.0, 0.0),
        (0.1, 0.0, 0.1, 10.0, true, 120.0, 10.0),
        (10.0, 10.0, 10.0, 3.0, true, 45.0, 5.0),
    ] {
        v.push(F::OilPaint { stylization: st, cleanliness: cl, scale: sc, bristle_detail: bd, lighting: li, angle: an, shine: sh });
    }
    for (count, mo, fill) in [(4, 20.0, TileFill::Background), (3, 40.0, TileFill::Foreground), (10, 99.0, TileFill::Inverse), (1, 0.0, TileFill::Unaltered), (0, 50.0, TileFill::Background)] {
        v.push(F::Tiles { count, max_offset: mo, fill, foreground: [0.0, 1.0, 0.0, 1.0], background: [1.0, 1.0, 1.0, 1.0], seed: 6 });
    }
    for (level, upper) in [(128.0, true), (128.0, false), (30.0, true), (300.0, false), (-5.0, true)] {
        v.push(F::TraceContour { level, upper });
    }
    for method in [WindMethod::Wind, WindMethod::Blast, WindMethod::Stagger] {
        for from_right in [false, true] {
            v.push(F::Wind { method, from_right, seed: 7 });
        }
    }
    v.push(F::Displace { horizontal: 50.0, vertical: 50.0, stretch: true, undefined: UndefinedAreas::Wrap, map: None });
    for (pts, und) in [
        (vec![[0.0, 0.0], [0.5, 0.3], [1.0, 0.0]], UndefinedAreas::Repeat),
        (vec![[0.0, 0.1], [1.0, 0.1]], UndefinedAreas::Wrap),
        (vec![[0.9, -0.4], [0.1, 0.6], [0.5, 2.0], [0.5000001, 0.0], [-1.0, 0.2]], UndefinedAreas::Transparent),
        (vec![[0.3, 0.5]], UndefinedAreas::Wrap),
        (vec![], UndefinedAreas::Repeat),
        (vec![[0.0, -0.5], [0.25, 0.5], [0.5, -0.5], [0.75, 0.5], [1.0, -0.5]], UndefinedAreas::Wrap),
    ] {
        v.push(F::Shear { points: pts, undefined: und });
    }
    for (a, r, style) in [
        (50.0, 4.0, ZigZagStyle::PondRipples),
        (-80.0, 10.0, ZigZagStyle::AroundCenter),
        (100.0, 0.0, ZigZagStyle::OutFromCenter),
        (0.0, 5.0, ZigZagStyle::AroundCenter),
        (300.0, 25.0, ZigZagStyle::PondRipples),
    ] {
        v.push(F::ZigZag { amount: a, ridges: r, style });
    }
    for (var, st, seed) in [(16.0, 4.0, 8), (1.0, 64.0, 1), (64.0, 1.0, 2), (0.0, 100.0, 3)] {
        v.push(F::Fibers { variance: var, strength: st, seed, foreground: [0.0, 0.0, 0.0, 1.0], background: [1.0, 0.8, 0.2, 1.0] });
    }
    for (b, cx, cy, lens) in [
        (100.0, 0.3, 0.3, LensType::Zoom),
        (150.0, 0.25, 0.5, LensType::Prime105),
        (300.0, 0.8, 0.1, LensType::Prime35),
        (10.0, 0.5, 0.5, LensType::MoviePrime),
        (100.0, -0.5, 1.5, LensType::Prime35),
    ] {
        v.push(F::LensFlare { brightness: b, center_x: cx, center_y: cy, lens });
    }
    let point = Light { kind: LightKind::Point, x: 0.1, y: 0.5, z: 0.2, radius: 1.0, intensity: 50.0, ..Light::default() };
    let spot = Light { kind: LightKind::Spot, x: 0.5, y: 0.5, z: 0.3, target_x: 0.5, target_y: 0.5, cone: 20.0, ..Light::default() };
    let inf = Light { kind: LightKind::Infinite, angle: 30.0, elevation: 60.0, color: [1.0, 0.7, 0.4], intensity: -40.0, ..Light::default() };
    for (lights, gloss, metallic, exposure, ambience, texture, height, wih) in [
        (vec![Light::default()], 0.0, 0.0, 0.0, 10.0, TextureChannel::Luminance, 50.0, true),
        (vec![point.clone()], -100.0, 0.0, 0.0, 0.0, TextureChannel::None, 0.0, true),
        (vec![spot.clone()], 0.0, 0.0, 0.0, 0.0, TextureChannel::None, 0.0, true),
        (vec![inf.clone(), point.clone()], 60.0, 100.0, 40.0, -20.0, TextureChannel::Red, 80.0, false),
        (vec![light(LightKind::Spot), inf.clone(), spot.clone()], 100.0, -50.0, -100.0, 50.0, TextureChannel::Alpha, 100.0, true),
        (vec![point.clone()], 20.0, 20.0, 20.0, 20.0, TextureChannel::Green, 30.0, true),
        (vec![inf.clone()], 0.0, 0.0, 0.0, 0.0, TextureChannel::Blue, 60.0, false),
        (vec![], 0.0, 0.0, 0.0, 30.0, TextureChannel::None, 0.0, true),
    ] {
        v.push(F::LightingEffects { lights, gloss, metallic, exposure, ambience, texture, height, white_is_high: wih });
    }
    for (blur, cx, cy, angle, focus, tr) in [
        (6.0, 0.5, 0.5, 0.0, 0.1, 0.2),
        (0.0, 0.5, 0.5, 30.0, 0.1, 0.2),
        (12.0, 0.3, 0.7, 45.0, 0.0, 0.0),
        (3.0, 0.5, 0.2, -80.0, 0.3, 1.0),
    ] {
        v.push(F::TiltShift { blur, center_x: cx, center_y: cy, angle, focus, transition: tr });
    }
    v.push(F::IrisBlur { pins: vec![IrisPin { blur: 6.0, ..IrisPin::default() }] });
    v.push(F::IrisBlur { pins: vec![IrisPin { blur: 6.0, radius_x: 0.3, radius_y: 0.3, roundness: 50.0, ..IrisPin::default() }] });
    v.push(F::IrisBlur {
        pins: vec![
            IrisPin { x: 0.2, y: 0.3, blur: 4.0, roundness: 100.0, angle: 30.0, feather: 0.9, ..IrisPin::default() },
            IrisPin { x: 0.8, y: 0.6, blur: 9.0, radius_x: 0.1, radius_y: 0.4, angle: -45.0, feather: 0.0, ..IrisPin::default() },
        ],
    });
    v.push(F::IrisBlur { pins: vec![] });
    v.push(F::FieldBlur { pins: vec![FieldPin { x: 0.2, y: 0.2, blur: 0.0 }, FieldPin { x: 0.8, y: 0.8, blur: 6.0 }] });
    v.push(F::FieldBlur { pins: vec![FieldPin::default()] });
    v.push(F::FieldBlur { pins: vec![] });
    v.push(F::SpinBlur { pins: vec![SpinPin { blur_angle: 30.0, ..SpinPin::default() }] });
    v.push(F::SpinBlur { pins: vec![SpinPin { radius_x: 0.3, radius_y: 0.3, blur_angle: 40.0, ..SpinPin::default() }] });
    v.push(F::SpinBlur {
        pins: vec![
            SpinPin { x: 0.3, y: 0.4, radius_x: 0.2, radius_y: 0.35, angle: 20.0, blur_angle: 360.0 },
            SpinPin { x: 0.7, y: 0.6, radius_x: 0.3, radius_y: 0.1, angle: -70.0, blur_angle: 90.0 },
        ],
    });
    v.push(F::SpinBlur { pins: vec![] });
    v.push(F::PathBlur { paths: vec![BlurPath { points: vec![[0.1, 0.2], [0.9, 0.8]], speed: 8.0, taper: 30.0 }] });
    v.push(F::PathBlur { paths: vec![BlurPath { points: vec![[0.0, 0.5], [1.0, 0.5]], speed: 6.0, taper: 0.0 }] });
    v.push(F::PathBlur {
        paths: vec![
            BlurPath { points: vec![[0.1, 0.1], [0.5, 0.4], [0.9, 0.1]], speed: 20.0, taper: 100.0 },
            BlurPath { points: vec![[0.2, 0.9], [0.8, 0.7]], speed: -12.0, taper: 50.0 },
            BlurPath { points: vec![[0.5, 0.5]], speed: 30.0, taper: 0.0 },
        ],
    });
    v.push(F::PathBlur { paths: vec![BlurPath { speed: 0.0, ..BlurPath::default() }] });
    let mut sharpen = vec![0.0; 25];
    for (i, w) in [(7, -1.0), (11, -1.0), (12, 5.0), (13, -1.0), (17, -1.0)] {
        sharpen[i] = w;
    }
    let mut shift = vec![0.0; 25];
    shift[13] = 2.0;
    for (kernel, scale, offset) in [
        (sharpen, 1.0, 0.0),
        (shift, 2.0, 0.0),
        ((0..25).map(|i| if i == 12 { 1.0 } else { 0.0 }).collect(), 1.0, 51.0),
        (vec![1.0, 2.0, 3.0], 0.0, -20.0),
        ((0..30).map(|i| (i % 5) as f32 - 2.0).collect(), 4.0, 10.0),
        (vec![], 1.0, 0.0),
    ] {
        v.push(F::Custom { kernel, scale, offset });
    }
    for input in [HsbModel::Rgb, HsbModel::Hsb, HsbModel::Hsl] {
        for output in [HsbModel::Rgb, HsbModel::Hsb, HsbModel::Hsl] {
            v.push(F::HsbHsl { input, output });
        }
    }
    for ee in [false, true] {
        for it in [false, true] {
            v.push(F::DeInterlace { eliminate_even: ee, interpolate: it });
        }
    }
    v.push(F::NtscColors);
    v
}

/// Displace maps both sides build: (name, image).
fn maps() -> Vec<(&'static str, Arc<Image>)> {
    let solid = |v: [f32; 4]| Arc::new(Image { rect: Rect::new(0, 0, 8, 8), ch: 4, data: (0..64).flat_map(|_| v).collect() });
    let grad = |r: Rect, ch: usize| {
        let mut data = Vec::new();
        for y in r.y0..r.y1 {
            for x in r.x0..r.x1 {
                for c in 0..ch {
                    data.push(pat(x, y, c, 5));
                }
            }
        }
        Arc::new(Image { rect: r, ch, data })
    };
    vec![
        ("solid", solid([0.8, 0.3, 0.5, 1.0])),
        ("shift3", solid([0.5 + 3.0 / 64.0, 0.5, 0.5, 1.0])),
        ("grad4", grad(Rect::new(0, 0, 13, 9), 4)),
        ("grad1", grad(Rect::new(-3, 2, 7, 11), 1)),
        ("grad2", grad(Rect::new(5, 5, 6, 6), 2)),
        ("grad3", grad(Rect::new(0, 0, 50, 40), 3)),
    ]
}

fn displace_cases() -> Vec<(f32, f32, bool, UndefinedAreas)> {
    vec![
        (10.0, 10.0, true, UndefinedAreas::Wrap),
        (25.0, 0.0, false, UndefinedAreas::Wrap),
        (-60.0, 35.0, true, UndefinedAreas::Repeat),
        (999.0, -999.0, false, UndefinedAreas::Transparent),
        (0.0, 0.0, true, UndefinedAreas::Wrap),
    ]
}

fn filter_vectors(env: &Env) {
    for p in cases() {
        let j = serde_json::to_string(&p).unwrap();
        for (name, fmt) in formats() {
            println!("fx {} {} {:016x}", name, j, run_case(env, fmt, &p));
        }
        println!("label {} {}", j, p.label());
        let halo = match p.halo() {
            Halo::Bounds => "bounds".to_string(),
            Halo::Radius(r) => format!("r{r}"),
        };
        println!("halo {} {}", j, halo);
    }
    // Non-finite parameters (not expressible in JSON): field set after parsing.
    let nan_cases: Vec<(FilterParams, &str, f32)> = vec![
        (FilterParams::Minimum { radius: 1.0, preserve: Preserve::Squareness }, "radius", f32::NAN),
        (FilterParams::Maximum { radius: 1.0, preserve: Preserve::Roundness }, "radius", f32::NAN),
        (FilterParams::Mosaic { cell_size: 1.0 }, "cellSize", f32::NAN),
        (FilterParams::Mosaic { cell_size: 1.0 }, "cellSize", f32::INFINITY),
        (FilterParams::Emboss { angle: 10.0, height: 2.0, amount: 100.0 }, "angle", f32::NAN),
        (FilterParams::Emboss { angle: 10.0, height: 2.0, amount: 100.0 }, "height", f32::NAN),
        (FilterParams::Twirl { angle: 1.0 }, "angle", f32::NAN),
        (FilterParams::Twirl { angle: 1.0 }, "angle", f32::INFINITY),
        (FilterParams::Pinch { amount: 1.0 }, "amount", f32::NAN),
        (FilterParams::Spherize { amount: 1.0, mode: SpherizeMode::HorizontalOnly }, "amount", f32::NAN),
        (FilterParams::Ripple { amount: 1.0, size: RippleSize::Small }, "amount", f32::INFINITY),
        (FilterParams::ColorHalftone { max_radius: 1.0, angles: [0.0, 10.0, 20.0, 30.0] }, "maxRadius", f32::NAN),
        (FilterParams::ColorHalftone { max_radius: 1.0, angles: [0.0, 10.0, 20.0, 30.0] }, "maxRadius", f32::INFINITY),
        (FilterParams::Crystallize { cell_size: 1.0, seed: 1 }, "cellSize", f32::NAN),
        (FilterParams::Crystallize { cell_size: 1.0, seed: 1 }, "cellSize", f32::INFINITY),
        (FilterParams::Pointillize { cell_size: 1.0, seed: 1, background: [1.0; 4] }, "cellSize", f32::NAN),
        (FilterParams::Pointillize { cell_size: 1.0, seed: 1, background: [1.0; 4] }, "cellSize", f32::INFINITY),
        (FilterParams::Extrude { kind: ExtrudeType::Blocks, size: 6.0, depth: 20.0, level_based: false, solid_front: false, mask_incomplete: false, seed: 5 }, "depth", f32::NAN),
        (FilterParams::Tiles { count: 4, max_offset: 20.0, fill: TileFill::Background, foreground: [0.0; 4], background: [1.0; 4], seed: 6 }, "maxOffset", f32::NAN),
        (FilterParams::TraceContour { level: 1.0, upper: true }, "level", f32::NAN),
        (FilterParams::ZigZag { amount: 50.0, ridges: 4.0, style: ZigZagStyle::PondRipples }, "ridges", f32::NAN),
        (FilterParams::Fibers { variance: 16.0, strength: 4.0, seed: 8, foreground: [0.0; 4], background: [1.0; 4] }, "variance", f32::NAN),
        (FilterParams::LensFlare { brightness: 100.0, center_x: 0.3, center_y: 0.3, lens: LensType::Zoom }, "centerX", f32::NAN),
        (FilterParams::TiltShift { blur: 6.0, center_x: 0.5, center_y: 0.5, angle: 0.0, focus: 0.1, transition: 0.2 }, "angle", f32::NAN),
        (FilterParams::TiltShift { blur: 6.0, center_x: 0.5, center_y: 0.5, angle: 0.0, focus: 0.1, transition: 0.2 }, "focus", f32::INFINITY),
        (FilterParams::OilPaint { stylization: 3.0, cleanliness: 4.0, scale: 1.0, bristle_detail: 5.0, lighting: true, angle: -60.0, shine: 2.0 }, "angle", f32::NAN),
        (FilterParams::Custom { kernel: vec![0.0; 25], scale: 1.0, offset: 0.0 }, "scale", f32::NAN),
    ];
    for (p, field, val) in nan_cases {
        let mut v = serde_json::to_value(&p).unwrap();
        v[field] = serde_json::json!(1.0);
        let j = v.to_string();
        let p = set_field(&p, field, val);
        let vs = if val.is_nan() { "nan" } else { "inf" };
        for (name, fmt) in [("rgba8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, true)), ("cmyka8", PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true))] {
            println!("fxk {} {} {} {} {:016x}", name, field, vs, j, run_case(env, fmt, &p));
        }
    }
    for (mname, map) in maps() {
        for (h, vv, st, und) in displace_cases() {
            let p = FilterParams::Displace { horizontal: h, vertical: vv, stretch: st, undefined: und, map: Some(map.clone()) };
            let j = serde_json::to_string(&p).unwrap();
            for (name, fmt) in [formats()[0], formats()[2], formats()[4], formats()[6], formats()[7]] {
                println!("fxmap {} {} {} {:016x}", mname, name, j, run_case(env, fmt, &p));
            }
        }
    }
}

/// `p` with the f32 field `field` set to `val`.
fn set_field(p: &FilterParams, field: &str, val: f32) -> FilterParams {
    use FilterParams as F;
    let mut p = p.clone();
    match (&mut p, field) {
        (F::Minimum { radius, .. } | F::Maximum { radius, .. }, "radius") => *radius = val,
        (F::Mosaic { cell_size }, "cellSize") => *cell_size = val,
        (F::Emboss { angle, .. }, "angle") => *angle = val,
        (F::Emboss { height, .. }, "height") => *height = val,
        (F::Twirl { angle }, "angle") => *angle = val,
        (F::Pinch { amount }, "amount") => *amount = val,
        (F::Spherize { amount, .. }, "amount") => *amount = val,
        (F::Ripple { amount, .. }, "amount") => *amount = val,
        (F::ColorHalftone { max_radius, .. }, "maxRadius") => *max_radius = val,
        (F::Crystallize { cell_size, .. }, "cellSize") => *cell_size = val,
        (F::Pointillize { cell_size, .. }, "cellSize") => *cell_size = val,
        (F::Extrude { depth, .. }, "depth") => *depth = val,
        (F::Tiles { max_offset, .. }, "maxOffset") => *max_offset = val,
        (F::TraceContour { level, .. }, "level") => *level = val,
        (F::ZigZag { ridges, .. }, "ridges") => *ridges = val,
        (F::Fibers { variance, .. }, "variance") => *variance = val,
        (F::LensFlare { center_x, .. }, "centerX") => *center_x = val,
        (F::TiltShift { angle, .. }, "angle") => *angle = val,
        (F::TiltShift { focus, .. }, "focus") => *focus = val,
        (F::OilPaint { angle, .. }, "angle") => *angle = val,
        (F::Custom { scale, .. }, "scale") => *scale = val,
        _ => panic!("set_field {field}"),
    }
    p
}

/// serde of the L6 variants: `de <input>\t<output or ERR message>`.
fn serde_vectors() {
    let inputs = [
        r#"{"filter":"minimum","radius":3}"#,
        r#"{"filter":"maximum","radius":2,"preserve":"roundness"}"#,
        r#"{"filter":"offset","horizontal":1.5,"vertical":0,"undefined":"wrap"}"#,
        r#"{"filter":"mosaic"}"#,
        r#"{"filter":"mosaic","cellSize":"x"}"#,
        r#"{"filter":"emboss","angle":1,"height":2,"amount":3,"extra":true}"#,
        r#"{"filter":"findEdges","radius":3}"#,
        r#"{"filter":"solarize"}"#,
        r#"{"filter":"desaturate"}"#,
        r#"{"filter":"spherize","amount":10,"mode":"diagonal"}"#,
        r#"{"filter":"spherize","amount":10,"mode":"horizontalOnly"}"#,
        r#"{"filter":"wave","generators":-1,"wavelengthMin":1,"wavelengthMax":2,"amplitudeMin":1,"amplitudeMax":2,"waveType":"sine","undefined":"wrap","seed":0}"#,
        r#"{"filter":"wave","generators":3,"wavelengthMin":1,"wavelengthMax":2,"amplitudeMin":1,"amplitudeMax":2,"waveType":"square","undefined":"repeat","seed":4294967295}"#,
        r#"{"filter":"ripple","amount":100,"size":"large"}"#,
        r#"{"filter":"polarCoordinates","mode":"polarToRectangular"}"#,
        r#"{"filter":"polarCoordinates"}"#,
        r#"{"filter":"colorHalftone","maxRadius":4,"angles":[1,2,3]}"#,
        r#"{"filter":"crystallize","cellSize":6,"seed":1.0}"#,
        r#"{"filter":"facet"}"#,
        r#"{"filter":"fragment"}"#,
        r#"{"filter":"mezzotint","kind":"longStrokes","seed":2}"#,
        r#"{"filter":"mezzotint","kind":"dots","seed":2}"#,
        r#"{"filter":"pointillize","cellSize":5,"seed":3,"background":[1,1,1,1]}"#,
        r#"{"filter":"diffuse","mode":"anisotropic","seed":4}"#,
        r#"{"filter":"extrude","kind":"pyramids","size":6,"depth":30,"levelBased":true,"solidFront":0,"maskIncomplete":true,"seed":5}"#,
        r#"{"filter":"extrude","kind":"blocks","size":6,"depth":30,"levelBased":true,"solidFront":false,"maskIncomplete":true,"seed":5}"#,
        r#"{"filter":"oilPaint","stylization":3,"cleanliness":4,"scale":1,"bristleDetail":5,"lighting":true,"angle":-60,"shine":2}"#,
        r#"{"filter":"tiles","count":4,"maxOffset":20,"fill":"inverse","foreground":[0,0,0,1],"background":[1,1,1,1],"seed":6}"#,
        r#"{"filter":"traceContour","level":128,"upper":true}"#,
        r#"{"filter":"wind","method":"stagger","fromRight":true,"seed":7}"#,
        r#"{"filter":"displace","horizontal":10,"vertical":10,"stretch":true,"undefined":"wrap","map":[1,2]}"#,
        r#"{"filter":"shear","points":[[0,0],[1,0.5,3]],"undefined":"wrap"}"#,
        r#"{"filter":"zigZag","amount":50,"ridges":4,"style":"pondRipples"}"#,
        r#"{"filter":"zigzag","amount":50,"ridges":4,"style":"pondRipples"}"#,
        r#"{"filter":"fibers","variance":16,"strength":4,"seed":8,"foreground":[0,0,0,1],"background":[1,1,1,1]}"#,
        r#"{"filter":"lensFlare","brightness":100,"centerX":0.3,"centerY":0.3,"lens":"moviePrime"}"#,
        r#"{"filter":"lightingEffects","lights":[{"kind":"point","x":0.1}],"gloss":0,"metallic":0,"exposure":0,"ambience":10,"texture":"luminance","height":50,"whiteIsHigh":true}"#,
        r#"{"filter":"tiltShift","blur":6,"centerX":0.5,"centerY":0.5,"angle":0,"focus":0.1,"transition":0.2}"#,
        r#"{"filter":"irisBlur","pins":[{"blur":6},{}]}"#,
        r#"{"filter":"fieldBlur","pins":[{"x":0.2,"y":0.2,"blur":0}]}"#,
        r#"{"filter":"spinBlur","pins":[{"blurAngle":30}]}"#,
        r#"{"filter":"pathBlur","paths":[{"points":[[0.1,0.2],[0.9,0.8]],"speed":8,"taper":30}]}"#,
        r#"{"filter":"custom","kernel":[1,2,3],"scale":1,"offset":0}"#,
        r#"{"filter":"hsbHsl","input":"rgb","output":"hsl"}"#,
        r#"{"filter":"deInterlace","eliminateEven":false,"interpolate":true}"#,
        r#"{"filter":"ntscColors"}"#,
        r#"{"filter":"filterGallery","effects":[]}"#,
        r#"{"filter":"filterGallery"}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"cutout"}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"glass","values":{"distortion":7,"texture":2,"zz":-1,"invert":1}}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"neonGlow","color":[1,0,0,1],"foreground":[0,1,0,1],"background":[0,0,1,0.5]}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"nope"}]}"#,
        r#"{"filter":"filterGallery","effects":[{"values":{}}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"grain","values":{"intensity":"x"}}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"grain","values":[1]}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"grain","color":[1,2]}]}"#,
        r#"{"filter":"filterGallery","effects":{"filter":"grain"}}"#,
        r#"{"filter":"filterGallery","effects":["grain"]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":"sumiE","values":{"b":2,"a":1}},{"filter":"craquelure"}]}"#,
        r#"{"filter":"filterGallery","effects":[{"filter":{"texturizer":null}}]}"#,
    ];
    for s in inputs {
        let v: serde_json::Value = serde_json::from_str(s).unwrap();
        match serde_json::from_value::<FilterParams>(v) {
            Ok(p) => println!("de {}\t{} | {}", s, serde_json::to_string(&p).unwrap(), p.label()),
            Err(e) => println!("de {}\tERR {}", s, e),
        }
    }
}

/// Filter Gallery: the filter table, the params notation, GalleryEffect, and every effect run.
fn gallery_vectors(env: &Env) {
    for f in GalleryFilter::ALL {
        let ps: Vec<String> = f
            .params()
            .iter()
            .map(|p| match &p.kind {
                GalleryParamKind::Range { min, max, default } => format!("{}:{}..{}={}", p.key, min, max, default),
                GalleryParamKind::Choice(c) => format!("{}:{}", p.key, c.join("|")),
                GalleryParamKind::Bool(b) => format!("{}:bool={}", p.key, b),
            })
            .collect();
        println!(
            "gal {} {} {} [{}] {} colours={} from={:?}/{:?}/{:?}",
            f.key(),
            f.command_id(),
            f.category(),
            f.name(),
            ps.join(","),
            f.uses_colours(),
            GalleryFilter::from_key(f.key()).map(|g| g.key()),
            GalleryFilter::from_key(&f.name().to_ascii_uppercase()).map(|g| g.key()),
            GalleryFilter::from_key(f.command_id()).map(|g| g.key()),
        );
        let e = GalleryEffect::new(*f);
        println!("galnew {} {}", f.key(), serde_json::to_string(&e).unwrap());
    }
    for k in ["", "Cutout", "filter.gallery.", "filter.gallery.Cutout", "sumi-e", "SUMI-E", "chalk & charcoal", "conté crayon", "CONTÉ CRAYON"] {
        println!("galkey {:?} {:?}", k, GalleryFilter::from_key(k).map(|g| g.key()));
    }
    // set / set_choice / get / flag / choice.
    let mut e = GalleryEffect::new(GalleryFilter::Texturizer);
    let mut log = Vec::new();
    for (k, v) in [("relief", 500.0), ("relief", -3.0), ("relief", f32::NAN), ("texture", 2.6), ("texture", 9.0), ("texture", -1.0), ("invert", 0.5), ("invert", 0.0), ("light", 7.4), ("nope", 1.0)] {
        e.set(k, v);
        log.push(format!("{}={}/{}/{}/{}", k, e.get(k).to_bits(), e.flag(k), e.choice(k), e.values.len()));
    }
    for (k, n) in [("texture", "BRICK"), ("texture", "wood"), ("light", "bottomLeft"), ("relief", "x"), ("nope", "y")] {
        let ok = e.set_choice(k, n);
        log.push(format!("{}:{}={}/{}", k, n, ok, e.choice(k)));
    }
    let mut g = GalleryEffect::new(GalleryFilter::Grain);
    g.values.clear();
    log.push(format!("unset {} {} {} {}", g.get("intensity"), g.get("grainType"), g.choice("grainType"), g.get("nope")));
    g.values.insert("grainType".into(), 99.0);
    log.push(format!("oob {}", g.choice("grainType")));
    g.values.insert("grainType".into(), -5.0);
    log.push(format!("neg {}", g.choice("grainType")));
    println!("galset {}", log.join(" "));

    // Every effect at its defaults, a few formats and tilings.
    let fmts = [("rgba8", fmt_named("rgba8")), ("rgb8", fmt_named("rgb8")), ("rgba32", fmt_named("rgba32")), ("cmyka8", fmt_named("cmyka8")), ("laba16", fmt_named("laba16"))];
    for f in GalleryFilter::ALL {
        let p = FilterParams::FilterGallery { effects: vec![GalleryEffect::new(*f)] };
        let j = serde_json::to_string(&p).unwrap();
        for (name, fmt) in fmts {
            println!("fx {} {} {:016x}", name, j, run_case(env, fmt, &p));
        }
        println!("label {} {}", j, p.label());
    }
    // Every parameter at its extremes (and every choice) on one format.
    let s = surf(fmt_named("rgba32"), Rect::new(-4, 2, 37, 33), 9, false);
    for f in GalleryFilter::ALL {
        for prm in f.params() {
            let vals: Vec<f32> = match &prm.kind {
                GalleryParamKind::Range { min, max, .. } => vec![*min, *max, (min + max) / 2.0 + 0.3],
                GalleryParamKind::Choice(c) => (0..c.len()).map(|i| i as f32).collect(),
                GalleryParamKind::Bool(_) => vec![1.0],
            };
            for v in vals {
                let mut e = GalleryEffect::new(*f);
                e.set(prm.key, v);
                e.foreground = [0.9, 0.2, 0.1, 1.0];
                e.background = [0.1, 0.3, 0.8, 1.0];
                e.color = [0.2, 1.0, 0.4, 1.0];
                let p = FilterParams::FilterGallery { effects: vec![e] };
                let j = serde_json::to_string(&p).unwrap();
                let area = output_area(&p, s.content_bounds(), R, None);
                let mut h = H0;
                hrect(&mut h, area);
                hu(&mut h, hs(&apply_tiled(&s, &p, area, R, None, 256, None)));
                hu(&mut h, hs(&apply_tiled(&s, &p, area, R, None, 16, Some(R))));
                println!("fxg {} {:016x}", j, h);
            }
        }
    }
    // Out-of-range stored values (values are not clamped by serde) and stacks.
    let mut stacks: Vec<Vec<GalleryEffect>> = Vec::new();
    stacks.push(vec![GalleryEffect::new(GalleryFilter::Cutout), GalleryEffect::new(GalleryFilter::Glass), GalleryEffect::new(GalleryFilter::Texturizer)]);
    stacks.push(vec![GalleryEffect::new(GalleryFilter::SumiE), GalleryEffect::new(GalleryFilter::Craquelure)]);
    stacks.push(vec![]);
    for (f, k, v) in [
        (GalleryFilter::DryBrush, "brushSize", 25.0),
        (GalleryFilter::NeonGlow, "glowSize", -24.0),
        (GalleryFilter::PaintDaubs, "brushType", 3.0),
        (GalleryFilter::PaintDaubs, "brushType", 4.0),
        (GalleryFilter::Spatter, "sprayRadius", 30.0),
        (GalleryFilter::GraphicPen, "strokeLength", 0.0),
        (GalleryFilter::TornEdges, "contrast", 0.0),
        (GalleryFilter::Craquelure, "crackSpacing", 0.0),
        (GalleryFilter::Patchwork, "squareSize", -5.0),
        (GalleryFilter::StainedGlass, "cellSize", 0.0),
        (GalleryFilter::MosaicTiles, "tileSize", 0.0),
        (GalleryFilter::Grain, "grainType", 50.0),
        (GalleryFilter::HalftonePattern, "size", 0.0),
        (GalleryFilter::Cutout, "numberOfLevels", 1.0),
        (GalleryFilter::Watercolor, "brushDetail", 30.0),
    ] {
        let mut e = GalleryEffect::new(f);
        e.values.insert(k.to_string(), v);
        stacks.push(vec![e]);
    }
    for effects in stacks {
        let p = FilterParams::FilterGallery { effects };
        let j = serde_json::to_string(&p).unwrap();
        for (name, fmt) in [("rgba8", fmt_named("rgba8")), ("graya16", fmt_named("graya16"))] {
            println!("fx {} {} {:016x}", name, j, run_case(env, fmt, &p));
        }
        let halo = match p.halo() {
            Halo::Bounds => "bounds".to_string(),
            Halo::Radius(r) => format!("r{r}"),
        };
        println!("halo {} {}", j, halo);
        println!("label {} {}", j, p.label());
    }
}

fn hprims(prims: &[Prim]) -> u64 {
    let mut h = H0;
    for p in prims {
        match p.shape {
            Shape::Capsule { a, b, ra, rb } => {
                fnv(&mut h, &[0]);
                hf(&mut h, &[a.0, a.1, b.0, b.1, ra, rb]);
            }
            Shape::Ellipse { c, rx, ry, angle } => {
                fnv(&mut h, &[1]);
                hf(&mut h, &[c.0, c.1, rx, ry, angle]);
            }
        }
        hf(&mut h, &p.c0);
        hf(&mut h, &p.c1);
        hf(&mut h, &[p.soft]);
        fnv(&mut h, &[p.blend as u8]);
        hrect(&mut h, p.bounds());
    }
    h
}

/// Flame paths both sides build by name.
fn flame_paths(name: &str, canvas: Rect) -> Vec<Vec<(f32, f32)>> {
    match name {
        "arc" => vec![render2::default_flame_path(canvas)],
        "loop" => vec![vec![(50.0, 50.0), (250.0, 60.0), (240.0, 220.0), (60.0, 210.0), (50.2, 50.3)]],
        "line" => vec![vec![(20.0, 200.0), (380.0, 180.0)]],
        "dot" => vec![vec![(200.0, 250.0)]],
        "multi" => vec![vec![(10.0, 280.0), (100.0, 260.0)], vec![], vec![(300.0, 100.0), (300.0, 100.0), (390.0, 150.0)]],
        "short" => vec![vec![(10.0, 10.0), (10.4, 10.3)]],
        _ => vec![],
    }
}

fn render2_vectors() {
    let canvas = Rect::new(0, 0, 400, 300);
    println!("trees {}", render2::tree_type_names().join("|"));
    println!("frames {}", render2::FRAME_STYLES.join("|"));
    let mut r = render2::Rng::new(7, 0x1234);
    let rv: Vec<String> = (0..4).map(|_| format!("{:016x}/{:08x}/{:08x}/{:08x}", r.next_u64(), r.f().to_bits(), r.range(-3.0, 5.0).to_bits(), r.sym().to_bits())).collect();
    println!("rng {}", rv.join(" "));
    let mut hp = H0;
    for p in render2::default_flame_path(Rect::new(-7, 3, 91, 64)) {
        hf(&mut hp, &[p.0, p.1]);
    }
    println!("flamepath {:016x}", hp);
    let tree_specs: Vec<TreeSpec> = {
        let mut v = Vec::new();
        for k in 1..=34 {
            v.push(TreeSpec { base_tree_type: k, ..Default::default() });
        }
        v.push(TreeSpec { seed: 7, ..Default::default() });
        v.push(TreeSpec { base_tree_type: 0, light_direction: 9, leaves_amount: 100.0, leaves_size: 200.0, ..Default::default() });
        v.push(TreeSpec { base_tree_type: 99, light_direction: 0, leaves_amount: 0.0, branches_height: 300.0, branches_thickness: 50.0, ..Default::default() });
        v.push(TreeSpec { base_tree_type: 3, flat_shading: true, default_leaves: false, leaves_color: [0.9, 0.1, 0.1, 1.0], custom_branch_color: true, branches_color: [0.1, 0.1, 0.9, 1.0], ..Default::default() });
        v.push(TreeSpec { base_tree_type: 4, light_direction: 1, x: 0.2, y: 0.5, size: 0.3, leaves_amount: 70.0, ..Default::default() });
        v.push(TreeSpec { base_tree_type: 8, light_direction: 5, leaves_size: 10.0, size: 2.5, seed: 99, ..Default::default() });
        v.push(TreeSpec { base_tree_type: 31, light_direction: 2, size: 0.0, ..Default::default() });
        v.push(TreeSpec { base_tree_type: 12, light_direction: 4, branches_height: 10.0, seed: 3, ..Default::default() });
        v
    };
    for s in &tree_specs {
        let prims = render2::tree(s, canvas);
        println!("tree {} {} {:016x}", serde_json::to_string(s).unwrap(), prims.len(), hprims(&prims));
    }
    let mut frame_specs = Vec::new();
    for k in 1..=render2::FRAME_STYLES.len() as u32 {
        frame_specs.push(FrameSpec { frame: k, ..Default::default() });
        frame_specs.push(FrameSpec { frame: k, margin: 12.0, size: 90.0, arrangement: 10.0, lines: 3, thickness: 80.0, fade: 40.0, seed: 5, ..Default::default() });
    }
    frame_specs.push(FrameSpec { frame: 0, lines: 0, margin: -5.0, size: 0.0, arrangement: 200.0, ..Default::default() });
    frame_specs.push(FrameSpec { frame: 40, lines: 9, margin: 50.0, fade: 100.0, ..Default::default() });
    for s in &frame_specs {
        for c in [canvas, Rect::new(10, -20, 30, 300)] {
            let prims = render2::picture_frame(s, c);
            println!("frame {} {:?} {} {:016x}", serde_json::to_string(s).unwrap(), [c.x0, c.y0, c.x1, c.y1], prims.len(), hprims(&prims));
        }
    }
    let types = [
        render2::FlameType::OneFlameAlongPath,
        render2::FlameType::MultipleFlamesAlongPath,
        render2::FlameType::MultipleFlamesPathDirections,
        render2::FlameType::MultipleFlamesVariousLength,
        render2::FlameType::CandleLight,
        render2::FlameType::MultipleFlamesOneDirection,
    ];
    let mut flame_specs = Vec::new();
    for t in types {
        flame_specs.push(FlameSpec { flame_type: t, ..Default::default() });
    }
    flame_specs.push(FlameSpec {
        flame_type: render2::FlameType::MultipleFlamesOneDirection,
        angle: 45.0,
        interval: 30.0,
        adjust_interval_for_loops: false,
        color: Some([0.2, 0.5, 1.0, 1.0]),
        flame_style: render2::FlameStyle::Violent,
        flame_shape: render2::FlameShape::Pointed,
        randomize_length: true,
        quality: 4,
        seed: 42,
        ..Default::default()
    });
    flame_specs.push(FlameSpec {
        flame_style: render2::FlameStyle::Flat,
        flame_shape: render2::FlameShape::ToCenter,
        randomize_shapes: true,
        flame_lines: 100.0,
        flame_bottom_alignment: 100.0,
        quality: 0,
        length: 1000.0,
        width: 1.0,
        ..Default::default()
    });
    for shape in [render2::FlameShape::Spread, render2::FlameShape::Oval] {
        flame_specs.push(FlameSpec { flame_shape: shape, flame_type: render2::FlameType::MultipleFlamesPathDirections, quality: 9, turbulent: 100.0, jag: 100.0, opacity: 100.0, ..Default::default() });
    }
    for s in &flame_specs {
        for pn in ["arc", "loop", "line", "dot", "multi", "short", "none"] {
            let prims = render2::flame(s, &flame_paths(pn, canvas));
            println!("flame {} {} {} {:016x}", serde_json::to_string(s).unwrap(), pn, prims.len(), hprims(&prims));
        }
    }
    // Spec JSON: defaults and errors.
    for (kind, j) in [
        ("tree", r#"{}"#),
        ("tree", r#"{"baseTreeType":5,"leavesColor":[1,0,0,1],"zz":1}"#),
        ("tree", r#"{"seed":-1}"#),
        ("frame", r#"{"frame":3,"vineColor":[0,0,1,1]}"#),
        ("frame", r#"{"lines":"x"}"#),
        ("flame", r#"{"flameType":"candleLight","color":[1,0.5,0,1]}"#),
        ("flame", r#"{"color":null}"#),
        ("flame", r#"{"color":[1,2]}"#),
        ("flame", r#"{"flameShape":"square"}"#),
    ] {
        let out = match kind {
            "tree" => serde_json::from_str::<TreeSpec>(j).map(|s| serde_json::to_string(&s).unwrap()),
            "frame" => serde_json::from_str::<FrameSpec>(j).map(|s| serde_json::to_string(&s).unwrap()),
            _ => serde_json::from_str::<FlameSpec>(j).map(|s| serde_json::to_string(&s).unwrap()),
        };
        match out {
            Ok(s) => println!("spec {} {}\t{}", kind, j, s),
            Err(e) => {
                let v: Result<serde_json::Value, _> = serde_json::from_str(j);
                let e2 = match (kind, v) {
                    ("tree", Ok(v)) => serde_json::from_value::<TreeSpec>(v).err().map(|e| e.to_string()),
                    ("frame", Ok(v)) => serde_json::from_value::<FrameSpec>(v).err().map(|e| e.to_string()),
                    (_, Ok(v)) => serde_json::from_value::<FlameSpec>(v).err().map(|e| e.to_string()),
                    _ => None,
                };
                println!("spec {} {}\tERR {}", kind, j, e2.unwrap_or_else(|| e.to_string()));
            }
        }
    }
    // Compositing onto surfaces of every format, with and without a selection.
    let mut sel = Surface::new(PixelFormat::GRAY8);
    for y in 0..60 {
        for x in 0..80 {
            sel.write_pixel(x, y, &[((x + y) % 11) as f32 / 10.0]);
        }
    }
    let small = Rect::new(0, 0, 80, 60);
    let prim_sets: Vec<(&str, Vec<Prim>)> = vec![
        ("tree", render2::tree(&TreeSpec { base_tree_type: 14, ..Default::default() }, small)),
        ("frame", render2::picture_frame(&FrameSpec { frame: 4, fade: 30.0, ..Default::default() }, small)),
        ("flame", render2::flame(&FlameSpec::default(), &[render2::default_flame_path(small)])),
        (
            "mixed",
            vec![
                Prim::disc((20.0, 20.0), 8.0, [1.0, 0.0, 0.0, 1.0]),
                Prim::capsule((5.0, 50.0), (70.0, 10.0), 3.0, 0.2, [0.0, 1.0, 0.0, 0.6], [0.0, 0.0, 1.0, 1.0]),
                Prim::ellipse((60.0, 40.0), 12.0, 4.0, 0.7, [1.0, 1.0, 0.0, 1.0], [0.0, 1.0, 1.0, 0.2]),
                Prim::capsule((40.0, 30.0), (40.0, 30.0), 0.3, 0.3, [1.0, 1.0, 1.0, 1.0], [1.0, 1.0, 1.0, 1.0]),
                Prim::disc((f32::NAN, 2.0), 3.0, [1.0, 1.0, 1.0, 1.0]),
                Prim::ellipse((300.0, 30.0), 400.0, 2.0, 0.0, [0.5, 0.5, 0.5, 0.5], [0.5, 0.5, 0.5, 0.5]),
            ],
        ),
    ];
    for (pname, prims) in &prim_sets {
        for (fname, fmt) in formats() {
            let mut h = H0;
            for (clip, sel) in [(small, None), (Rect::new(10, 5, 300, 50), Some(&sel)), (Rect::new(-50, -50, 0, 0), None)] {
                let mut s = surf(fmt, Rect::new(-3, -2, 70, 50), 4, false);
                let used = render2::composite(&mut s, prims, clip, sel);
                hrect(&mut h, used);
                hu(&mut h, hs(&s));
            }
            println!("comp {} {} {:016x}", pname, fname, h);
        }
    }
}

fn trap_vectors() {
    let buf = |w: usize, h: usize, ch: usize, seed: i32| -> Vec<f32> {
        let mut v = Vec::new();
        for y in 0..h {
            for x in 0..w {
                for c in 0..ch {
                    let p = pat(x as i32, y as i32, c, seed);
                    v.push(if p < 0.4 { 0.0 } else { p });
                }
            }
        }
        v
    };
    for (w, h, ch, width, seed) in [(8, 4, 4, 1, 1), (17, 11, 5, 2, 2), (9, 9, 4, 0, 3), (5, 3, 3, 1, 4), (30, 2, 4, 40, 5), (1, 1, 4, 3, 6), (0, 4, 4, 1, 7)] {
        let mut v = buf(w, h, ch, seed);
        trap::trap(&mut v, w, h, ch, width);
        let mut hh = H0;
        hf(&mut hh, &v);
        println!("trap {} {} {} {} {} {:016x}", w, h, ch, width, seed, hh);
    }
}

fn warp_vectors() {
    let src_fmts = [("rgba8", fmt_named("rgba8")), ("rgb8", fmt_named("rgb8")), ("rgba32", fmt_named("rgba32")), ("graya16", fmt_named("graya16"))];
    let r = Rect::new(5, 3, 45, 27);
    let b = [5.0, 3.0, 45.0, 27.0];
    let interps = [("nearest", Interp::Nearest), ("bilinear", Interp::Bilinear), ("bicubic", Interp::Bicubic)];
    let maps: Vec<(&str, Box<dyn Fn(f64, f64) -> (f64, f64) + Sync>)> = vec![
        ("identity", Box::new(|x, y| (x, y))),
        ("shift", Box::new(|x, y| (x + 10.0, y - 2.0))),
        ("frac", Box::new(|x, y| (x + 0.37, y + 0.81))),
        ("rotate", Box::new(|x, y| (25.0 + (x - 25.0) * 0.8 - (y - 15.0) * 0.6, 15.0 + (x - 25.0) * 0.6 + (y - 15.0) * 0.8))),
        ("scale", Box::new(|x, y| (x * 1.7 - 9.0, y * 0.6 + 4.0))),
        ("fold", Box::new(|x, y| (50.0 - (x - 25.0).abs() * 1.3, y))),
        ("arc", Box::new(move |x, y| Warp::preset(WarpStyle::Arc, 50.0, b).map(x, y))),
        ("twist", Box::new(move |x, y| Warp::preset(WarpStyle::Twist, -40.0, b).map(x, y))),
        ("fisheye", Box::new(move |x, y| Warp::preset(WarpStyle::Fisheye, 70.0, b).map(x, y))),
        ("flat", Box::new(|x, _| (x, 10.0))),
        ("nan", Box::new(|x, y| if x > 20.0 { (f64::NAN, y) } else { (x, y) })),
        ("far", Box::new(|x, y| (x + 3.0e6, y - 1.0e7))),
    ];
    for (mname, f) in &maps {
        for (fname, fmt) in src_fmts {
            for (iname, interp) in interps {
                let src = surf(fmt, Rect::new(0, 0, 50, 30), 3, fname == "rgb8");
                let out = warp::warp_mesh_surface(&src, r, f.as_ref(), interp);
                println!("warp {} {} {} {:016x}", mname, fname, iname, hs(&out));
            }
        }
    }
    let empty = surf(fmt_named("rgba8"), Rect::new(0, 0, 10, 10), 1, false);
    println!("warp_empty {:016x}", hs(&warp::warp_mesh_surface(&empty, Rect::EMPTY, &|x, y| (x, y), Interp::Bilinear)));
    // Triangles directly: overlapping (later wins), degenerate and out-of-range source rects.
    let verts = vec![
        ([2.0, 2.0], [0.0, 0.0]),
        ([30.0, 4.0], [40.0, 0.0]),
        ([6.0, 25.0], [0.0, 30.0]),
        ([28.0, 28.0], [40.0, 30.0]),
        ([10.0, 10.0], [10.0, 10.0]),
        ([10.0, 10.0], [20.0, 20.0]),
    ];
    let tris = vec![[0, 1, 2], [1, 3, 2], [0, 3, 4], [4, 4, 5], [5, 4, 4]];
    for (fname, fmt) in src_fmts {
        for (iname, interp) in interps {
            let src = surf(fmt, Rect::new(0, 0, 50, 30), 6, false);
            let mut h = H0;
            for sr in [Rect::new(0, 0, 50, 30), Rect::new(10, 5, 20, 15), Rect::new(60, 60, 70, 70)] {
                hu(&mut h, hs(&warp::warp_triangles(&src, sr, &verts, &tris, interp)));
            }
            println!("tris {} {} {:016x}", fname, iname, h);
        }
    }
    // Gray masks/selections keep their default.
    for (gname, fmt, def) in [("gray8", PixelFormat::GRAY8, 1.0f32), ("grayf", PixelFormat::new(ColorMode::Grayscale, SampleType::F32, false), 0.25)] {
        let mut m = Surface::with_default(fmt, &[def]);
        m.fill_rect(Rect::new(4, 4, 10, 10), &[0.0]);
        for x in 12..20 {
            m.write_pixel(x, 6, &[(x - 12) as f32 / 8.0]);
        }
        for (mname, f) in &maps {
            for (iname, interp) in interps {
                println!("wgray {} {} {} {:016x}", gname, mname, iname, hs(&warp::warp_mesh_gray(&m, f.as_ref(), interp)));
            }
        }
        let empty = Surface::with_default(fmt, &[def]);
        println!("wgray_empty {} {:016x}", gname, hs(&warp::warp_mesh_gray(&empty, &|x, y| (x, y), Interp::Bilinear)));
    }
    // place_source / place_source_projective.
    let aff = [
        ("ident", Affine { m: [1.0, 0.0, 0.0, 1.0, 0.0, 0.0] }),
        ("intshift", Affine { m: [1.0, 0.0, 0.0, 1.0, 12.0, -7.0] }),
        ("fracshift", Affine { m: [1.0, 0.0, 0.0, 1.0, 12.5, -7.0] }),
        ("scale", Affine { m: [0.5, 0.0, 0.0, 1.5, 3.0, 2.0] }),
        ("rot", Affine { m: [0.8, 0.6, -0.6, 0.8, 10.0, 0.0] }),
    ];
    let warps: Vec<(&str, Option<Warp>)> = vec![
        ("none", None),
        ("identity", Some(Warp::none(b))),
        ("bulge", Some(Warp::preset(WarpStyle::Bulge, 30.0, b))),
        ("mesh", Some(Warp::custom(BezierMesh::identity(b, 2, 1).map_points(|p| [p[0] + (p[1] - 3.0) * 0.2, p[1]]), b))),
    ];
    for (fname, fmt) in [src_fmts[0], src_fmts[2]] {
        let src = surf(fmt, r, 8, false);
        for (aname, t) in &aff {
            for (wname, w) in &warps {
                println!("place {} {} {} {:016x}", fname, aname, wname, hs(&warp::place_source(&src, r, t, w.as_ref())));
            }
        }
        let homs = [
            ("ident", Homography::IDENTITY),
            ("persp", Homography([1.1, 0.1, 2.0, 0.05, 0.9, 1.0, 0.002, 0.001, 1.0])),
        ];
        for (hname, hm) in &homs {
            for (wname, w) in &warps {
                println!("placep {} {} {} {:016x}", fname, hname, wname, hs(&warp::place_source_projective(&src, r, hm, w.as_ref())));
            }
        }
    }
}

pub fn run() {
    let mut sel_grad = Surface::new(PixelFormat::GRAY8);
    {
        let r = Rect::new(-5, -5, 45, 35);
        let mut v = Vec::new();
        for y in r.y0..r.y1 {
            for x in r.x0..r.x1 {
                v.push(pat(x, y, 0, 3));
            }
        }
        sel_grad.write_region(r, &v);
    }
    let env = Env { sel_grad };
    let t = std::time::Instant::now();
    serde_vectors();
    filter_vectors(&env);
    eprintln!("filters {:?}", t.elapsed());
    gallery_vectors(&env);
    eprintln!("gallery {:?}", t.elapsed());
    render2_vectors();
    eprintln!("render2 {:?}", t.elapsed());
    trap_vectors();
    warp_vectors();
    eprintln!("warp {:?}", t.elapsed());
}
