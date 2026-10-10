//! L5 algo reference vectors (`rustref algo-blur-vectors`): the blur, sharpen, noise and denoise
//! filters (FilterParams serde text, halos, kernels, the tiled driver over nine formats with
//! tilings, selections and canvas extents, hostile non-finite parameters), Select > Modify Smooth
//! and Feather, and the FFT Motion Blur. One `key value` line per case; values are FNV-1a hashes of
//! the outputs' f32 bits (little-endian; NaNs canonical) or plain text. `fft` lines carry raw f32 bits instead of a
//! hash: the C port uses pocketfft where the reference uses rustfft, so those samples are compared
//! with a tolerance. Replayed by tests/algo/test_blur_parity.c, which reads the `case` lines' JSON.
use photocraft_algo::*;
use photocraft_color::{ColorMode, PixelFormat, SampleType};
use photocraft_geom::Rect;
use photocraft_raster::Surface;

const H0: u64 = 0xcbf29ce484222325;

fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
/// NaNs hash as one canonical value: their sign and payload depend on the compiler's operand order.
fn hf(h: &mut u64, v: &[f32]) {
    for x in v {
        let bits = if x.is_nan() { 0x7fc0_0000 } else { x.to_bits() };
        fnv(h, &bits.to_le_bytes());
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
fn of(v: &[f32]) -> u64 {
    let mut h = H0;
    hf(&mut h, v);
    h
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

/// The parameter sweep, as FilterParams JSON (the C replay parses the same text).
const CASES: &[&str] = &[
    r#"{"filter":"gaussianBlur","radius":0.01}"#,
    r#"{"filter":"gaussianBlur","radius":0.5}"#,
    r#"{"filter":"gaussianBlur","radius":1}"#,
    r#"{"filter":"gaussianBlur","radius":2.5}"#,
    r#"{"filter":"gaussianBlur","radius":4}"#,
    r#"{"filter":"gaussianBlur","radius":4.5}"#,
    r#"{"filter":"gaussianBlur","radius":9.3}"#,
    r#"{"filter":"gaussianBlur","radius":25}"#,
    r#"{"filter":"boxBlur","radius":0.4}"#,
    r#"{"filter":"boxBlur","radius":1}"#,
    r#"{"filter":"boxBlur","radius":2}"#,
    r#"{"filter":"boxBlur","radius":4}"#,
    r#"{"filter":"boxBlur","radius":5}"#,
    r#"{"filter":"boxBlur","radius":6}"#,
    r#"{"filter":"boxBlur","radius":17.6}"#,
    r#"{"filter":"boxBlur","radius":40}"#,
    r#"{"filter":"motionBlur","angle":0,"distance":1}"#,
    r#"{"filter":"motionBlur","angle":30,"distance":7}"#,
    r#"{"filter":"motionBlur","angle":90,"distance":6}"#,
    r#"{"filter":"motionBlur","angle":-45,"distance":12.5}"#,
    r#"{"filter":"motionBlur","angle":137,"distance":33.5}"#,
    r#"{"filter":"motionBlur","angle":200,"distance":3}"#,
    r#"{"filter":"motionBlur","angle":0,"distance":0.3}"#,
    r#"{"filter":"motionBlur","angle":17,"distance":-9}"#,
    r#"{"filter":"motionBlur","angle":45,"distance":70}"#,
    r#"{"filter":"radialBlur","amount":20,"method":"spin","quality":"good","centerX":0.5,"centerY":0.5}"#,
    r#"{"filter":"radialBlur","amount":30,"method":"zoom","quality":"good","centerX":0.3,"centerY":0.6}"#,
    r#"{"filter":"radialBlur","amount":100,"method":"spin","quality":"draft","centerX":0.5,"centerY":0.5}"#,
    r#"{"filter":"radialBlur","amount":80,"method":"zoom","quality":"best","centerX":0.9,"centerY":0.1}"#,
    r#"{"filter":"radialBlur","amount":5,"method":"spin","quality":"best","centerX":0.2,"centerY":0.8}"#,
    r#"{"filter":"radialBlur","amount":10,"method":"zoom","centerX":0.5,"centerY":0.5}"#,
    r#"{"filter":"surfaceBlur","radius":3,"threshold":20}"#,
    r#"{"filter":"surfaceBlur","radius":1,"threshold":2}"#,
    r#"{"filter":"surfaceBlur","radius":5,"threshold":60}"#,
    r#"{"filter":"surfaceBlur","radius":12,"threshold":15}"#,
    r#"{"filter":"surfaceBlur","radius":10,"threshold":255}"#,
    r#"{"filter":"surfaceBlur","radius":0,"threshold":10}"#,
    r#"{"filter":"unsharpMask","amount":150,"radius":1.5,"threshold":2}"#,
    r#"{"filter":"unsharpMask","amount":50,"radius":1,"threshold":0}"#,
    r#"{"filter":"unsharpMask","amount":200,"radius":2,"threshold":255}"#,
    r#"{"filter":"unsharpMask","amount":500,"radius":4.5,"threshold":10}"#,
    r#"{"filter":"unsharpMask","amount":80,"radius":0.04,"threshold":0}"#,
    r#"{"filter":"smartSharpen","amount":100,"radius":1,"reduceNoise":20}"#,
    r#"{"filter":"smartSharpen","amount":250,"radius":3,"reduceNoise":0}"#,
    r#"{"filter":"smartSharpen","amount":50,"radius":0.5,"reduceNoise":100}"#,
    r#"{"filter":"highPass","radius":3}"#,
    r#"{"filter":"highPass","radius":0.5}"#,
    r#"{"filter":"highPass","radius":10}"#,
    r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":false,"seed":3}"#,
    r#"{"filter":"addNoise","amount":50,"distribution":"uniform","monochromatic":true,"seed":1}"#,
    r#"{"filter":"addNoise","amount":400,"distribution":"uniform","monochromatic":false,"seed":7}"#,
    r#"{"filter":"addNoise","amount":10,"distribution":"gaussian","monochromatic":true,"seed":0}"#,
    r#"{"filter":"median","radius":1}"#,
    r#"{"filter":"median","radius":2}"#,
    r#"{"filter":"median","radius":3}"#,
    r#"{"filter":"median","radius":7.4}"#,
    r#"{"filter":"dustAndScratches","radius":1,"threshold":255}"#,
    r#"{"filter":"dustAndScratches","radius":2,"threshold":10}"#,
    r#"{"filter":"dustAndScratches","radius":4,"threshold":50}"#,
    r#"{"filter":"dustAndScratches","radius":1,"threshold":0}"#,
    r#"{"filter":"reduceNoise","strength":6,"preserveDetails":60,"reduceColorNoise":45,"sharpenDetails":25,"removeJpegArtifact":false}"#,
    r#"{"filter":"reduceNoise","strength":10,"preserveDetails":0,"reduceColorNoise":100,"sharpenDetails":100,"removeJpegArtifact":true}"#,
    r#"{"filter":"reduceNoise","strength":0,"preserveDetails":0,"reduceColorNoise":0,"sharpenDetails":0,"removeJpegArtifact":true}"#,
    r#"{"filter":"reduceNoise","strength":3,"preserveDetails":100,"reduceColorNoise":0,"sharpenDetails":0,"removeJpegArtifact":false}"#,
    r#"{"filter":"reduceNoise","strength":0,"preserveDetails":0,"reduceColorNoise":50,"sharpenDetails":0,"removeJpegArtifact":false}"#,
    r#"{"filter":"smartBlur","radius":3,"threshold":25,"quality":"high","mode":"normal"}"#,
    r#"{"filter":"smartBlur","radius":5,"threshold":10,"quality":"low","mode":"edgeOnly"}"#,
    r#"{"filter":"smartBlur","radius":2,"threshold":40,"quality":"medium","mode":"overlayEdge"}"#,
    r#"{"filter":"smartBlur","radius":30,"threshold":100,"quality":"high","mode":"normal"}"#,
    r#"{"filter":"lensBlur","radius":15,"blades":6,"curvature":50,"rotation":30,"depth":"none","focalDistance":0,"invertDepth":false,"brightness":0,"threshold":255,"noise":0,"distribution":"uniform","monochromatic":false,"seed":0}"#,
    r#"{"filter":"lensBlur","radius":8,"blades":3,"curvature":0,"rotation":0,"depth":"transparency","focalDistance":128,"invertDepth":false,"brightness":50,"threshold":200,"noise":10,"distribution":"gaussian","monochromatic":false,"seed":4}"#,
    r#"{"filter":"lensBlur","radius":20,"blades":8,"curvature":100,"rotation":45,"depth":"layerMask","focalDistance":40,"invertDepth":true,"brightness":100,"threshold":0,"noise":30,"distribution":"uniform","monochromatic":true,"seed":9}"#,
    r#"{"filter":"lensBlur","radius":4,"blades":12,"curvature":20,"rotation":-90,"depth":"layerMask","focalDistance":255,"invertDepth":false,"brightness":20,"threshold":100,"noise":5,"distribution":"gaussian","monochromatic":true,"seed":2}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"circle"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"ring"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"square"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"diamond"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"triangle"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"hexagon"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"star"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"heart"}"#,
    r#"{"filter":"shapeBlur","radius":5,"shape":"cross"}"#,
    r#"{"filter":"shapeBlur","radius":2.5,"shape":"circle"}"#,
    r#"{"filter":"shapeBlur","radius":25,"shape":"square"}"#,
    r#"{"filter":"shapeBlur","radius":12,"shape":"star"}"#,
];

/// Lens Blur's caller-supplied depth (channel 0) over `r`.
fn depth_map(r: Rect) -> std::sync::Arc<Image> {
    let mut img = Image::new(r, 1);
    let w = r.width() as usize;
    for (i, v) in img.data.iter_mut().enumerate() {
        *v = pat(r.x0 + (i % w) as i32, r.y0 + (i / w) as i32, 0, 5);
    }
    std::sync::Arc::new(img)
}

fn parse(json: &str) -> FilterParams {
    let mut p: FilterParams = serde_json::from_str(json).unwrap_or(FilterParams::Invert);
    if let FilterParams::LensBlur { depth: DepthSource::LayerMask, depth_map: m, .. } = &mut p {
        *m = Some(depth_map(Rect::new(-30, -30, 70, 60)));
    }
    p
}

fn halo_text(h: Halo) -> String {
    match h {
        Halo::Radius(r) => format!("r{r}"),
        Halo::Bounds => "bounds".into(),
    }
}

fn case_lines() {
    for (i, json) in CASES.iter().enumerate() {
        let p = parse(json);
        println!("case {i} {}", serde_json::to_string(&p).unwrap_or_default());
        println!("halo {i} {} {}", halo_text(p.halo()), p.label());
    }
}

fn apply_lines() {
    let r = Rect::new(0, 0, 40, 30);
    let sel_grad = surf(PixelFormat::GRAY8, Rect::new(-5, -5, 45, 35), 3, false);
    let sel_none = Surface::new(PixelFormat::GRAY8);
    for (name, fmt) in formats() {
        let s = surf(fmt, Rect::new(-4, 2, 37, 33), 9, false);
        for (i, json) in CASES.iter().enumerate() {
            let p = parse(json);
            let mut h = H0;
            let area = output_area(&p, s.content_bounds(), r, None);
            hrect(&mut h, area);
            fnv(&mut h, &hs(&apply_tiled(&s, &p, area, r, None, 256, None)).to_le_bytes());
            fnv(&mut h, &hs(&apply_tiled(&s, &p, area, r, None, 7, None)).to_le_bytes());
            fnv(&mut h, &hs(&apply(&s, &p, area, r, Some(&sel_grad))).to_le_bytes());
            fnv(&mut h, &hs(&apply(&s, &p, area, r, Some(&sel_none))).to_le_bytes());
            fnv(&mut h, &hs(&apply_in(&s, &p, area, r, None, r)).to_le_bytes());
            fnv(&mut h, &hs(&apply_tiled(&s, &p, Rect::new(-9, -9, 50, 50), Rect::new(5, 5, 25, 20), None, 9, Some(Rect::new(-2, -2, 30, 31)))).to_le_bytes());
            println!("apply {name} {i} {:016x}", h);
        }
    }
}

/// The filter with its first numeric parameter replaced.
fn with_first(p: &FilterParams, v: f32) -> FilterParams {
    let mut p = p.clone();
    match &mut p {
        FilterParams::GaussianBlur { radius }
        | FilterParams::BoxBlur { radius }
        | FilterParams::HighPass { radius }
        | FilterParams::Median { radius }
        | FilterParams::SurfaceBlur { radius, .. }
        | FilterParams::DustAndScratches { radius, .. }
        | FilterParams::SmartBlur { radius, .. }
        | FilterParams::LensBlur { radius, .. }
        | FilterParams::ShapeBlur { radius, .. } => *radius = v,
        FilterParams::MotionBlur { angle, .. } => *angle = v,
        FilterParams::RadialBlur { amount, .. }
        | FilterParams::UnsharpMask { amount, .. }
        | FilterParams::SmartSharpen { amount, .. }
        | FilterParams::AddNoise { amount, .. } => *amount = v,
        FilterParams::ReduceNoise { strength, .. } => *strength = v,
        _ => {}
    }
    p
}

fn kernel_lines() {
    let bounds = Rect::new(-6, -4, 34, 26);
    let s = surf(PixelFormat::new(ColorMode::Rgb, SampleType::F32, true), Rect::new(-40, -40, 80, 70), 4, false);
    let ctx = Ctx { bounds, mode: ColorMode::Rgb, alpha: true };
    let out = Rect::new(2, 3, 21, 15);
    for (i, json) in CASES.iter().enumerate() {
        let base = parse(json);
        for (vi, v) in [None, Some(f32::NAN), Some(f32::NEG_INFINITY), Some(-1e30)].into_iter().enumerate() {
            let p = match v {
                None => base.clone(),
                Some(v) => with_first(&base, v),
            };
            let halo = p.halo();
            let src = match halo {
                Halo::Radius(r) => Image::read(&s, out.inflate(r)),
                Halo::Bounds => Image::read(&s, bounds.union(&out)),
            };
            let data = kernel(&p, &src, out, &ctx);
            println!("kernel {i} {vi} {} {:016x}", halo_text(halo), of(&data));
        }
    }
}

/// Masks (C: the same integer formulas): binary, soft, a disc, sparse dots and a rectangle.
fn masks(w: usize, h: usize, seed: i32) -> Vec<(&'static str, Vec<f32>)> {
    let at = |i: usize| ((i % w) as i32, (i / w) as i32);
    let n = w * h;
    let binary = (0..n).map(|i| if pat(at(i).0, at(i).1, 0, seed) < 0.5 { 1.0 } else { 0.0 }).collect();
    let soft = (0..n).map(|i| pat(at(i).0 * 3, at(i).1, 1, seed)).collect();
    let (cx, cy, rad) = (w as f32 * 0.4, h as f32 * 0.55, (w.min(h) as f32 * 0.4).max(1.0));
    let blob = (0..n)
        .map(|i| {
            let (x, y) = (at(i).0 as f32 + 0.5, at(i).1 as f32 + 0.5);
            (rad - ((x - cx) * (x - cx) + (y - cy) * (y - cy)).sqrt()).clamp(0.0, 1.0)
        })
        .collect();
    let sparse = (0..n).map(|i| if (at(i).0 * 7 + at(i).1 * 11 + seed).rem_euclid(53) == 0 { 1.0 } else { 0.0 }).collect();
    let rect = (0..n).map(|i| if (i % w) * 3 >= w && (i / w) * 4 < h * 3 { 1.0 } else { 0.0 }).collect();
    vec![("binary", binary), ("soft", soft), ("blob", blob), ("sparse", sparse), ("rect", rect)]
}

fn selection_lines() {
    for (w, h, seed) in [(97usize, 61usize, 1), (40, 150, 2), (13, 7, 3), (1, 1, 4)] {
        for (kind, m) in masks(w, h, seed) {
            for r in [0.0f32, 0.4, 1.0, 2.0, 5.0, 16.0, 50.0, 500.0] {
                println!("smooth {w}x{h} {kind} {r} {:016x}", of(&selection::smooth(&m, w, h, r)));
            }
            for r in [0.1f32, 0.3, 1.0, 2.5, 8.0, 9.0, 17.0, 25.0, 50.0, 120.0, 1000.0] {
                println!("feather {w}x{h} {kind} {r} {:016x}", of(&selection::feather(&m, w, h, r)));
            }
        }
    }
    let m: Vec<f32> = (0..50 * 40).map(|i| (i % 7) as f32 / 6.0).collect();
    for r in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY, -5.0, 1e30] {
        println!("smooth hostile {r} {:016x}", of(&selection::smooth(&m, 50, 40, r)));
        println!("feather hostile {r} {:016x}", of(&selection::feather(&m, 50, 40, r)));
    }
    println!("smooth mismatched {:016x}", of(&selection::smooth(&m, 49, 40, 5.0)));
    println!("feather mismatched {:016x}", of(&selection::feather(&m, 50, 41, 5.0)));
}

/// FFT Motion Blur: wide streaks over 1500 x 1000 take the FFT path (with enough threads); the
/// samples are printed as raw bits for a tolerance comparison.
fn fft_lines() {
    let area = Rect::new(0, 0, 1500, 1000);
    let sel = surf(PixelFormat::GRAY8, Rect::new(-5, -5, 1505, 1005), 3, false);
    let cases: [(&str, PixelFormat, f32, f32, bool); 3] = [
        ("rgba-30-1000", PixelFormat::new(ColorMode::Rgb, SampleType::F32, true), 30.0, 1000.0, false),
        ("graya-0-2000", PixelFormat::new(ColorMode::Grayscale, SampleType::F32, true), 0.0, 2000.0, false),
        ("rgba-45-900-sel-extent", PixelFormat::new(ColorMode::Rgb, SampleType::F32, true), 45.0, 900.0, true),
    ];
    for (name, fmt, angle, distance, extras) in cases {
        let s = surf(fmt, area, 11, false);
        let p = FilterParams::MotionBlur { angle, distance };
        let out = if extras { apply_tiled(&s, &p, area, area, Some(&sel), 1024, Some(area)) } else { apply_tiled(&s, &p, area, area, None, 1024, None) };
        let n = fmt.channels();
        let mut line = format!("fft {name} {n}");
        for k in 0..97i32 {
            let (x, y) = ((k * 131).rem_euclid(1500), (k * 71).rem_euclid(1000));
            for v in out.pixel(x, y).iter().take(n) {
                line.push_str(&format!(" {:08x}", v.to_bits()));
            }
        }
        println!("{line}");
    }
}

pub fn run() {
    case_lines();
    apply_lines();
    kernel_lines();
    selection_lines();
    fft_lines();
}
