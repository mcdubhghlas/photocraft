//! L4 algo core reference vectors (`rustref algo-core-vectors`): enums and FilterParams serde,
//! Image sampling, the fxutil helpers, histograms, tone adjustments, resampling, transforms,
//! pyramids and the tiled filter driver. One `key value` line per case; values are FNV-1a hashes
//! of the outputs' bytes (f32/f64 bit patterns little-endian) or plain text. Replayed by
//! tests/algo/test_core_parity.c.
//!
//! The crate-private helpers (fxutil.rs, image::premultiply, noise::hash01, other2's colour
//! encodings) are verbatim copies below, marked; everything else calls photocraft-algo.
use photocraft_algo::histogram::{RgbHistogram, clipping};
use photocraft_algo::pyramid::{self, Level};
use photocraft_algo::resample::{Resample, crop_surface, resize_surface, resize_surface_in_canvas, scaled_rect, translate_surface};
use photocraft_algo::tone::{self, HdrToning, MatchColor, ShadowsHighlights};
use photocraft_algo::transform::{Homography, Interp, convex_quad, quad_size, quad_to_rect, warp_surface, warp_surface_clipped};
use photocraft_algo::*;
use photocraft_color::{ColorMode, PixelFormat, SampleType};
use photocraft_geom::Rect;
use photocraft_raster::{Surface, from_rgba_into, to_rgba};

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
fn hd(h: &mut u64, v: &[f64]) {
    for x in v {
        fnv(h, &x.to_bits().to_le_bytes());
    }
}
fn hi(h: &mut u64, v: &[i32]) {
    for x in v {
        fnv(h, &x.to_le_bytes());
    }
}
fn hu(h: &mut u64, v: &[u64]) {
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
/// A surface of `fmt` with pattern pixels over `r` (alpha varies unless `opaque`).
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
fn buf(n: usize, seed: i32) -> Vec<f32> {
    (0..n).map(|i| pat(i as i32, (i / 7) as i32, i % 3, seed)).collect()
}
fn rgba_px(n: usize, seed: i32) -> Vec<[f32; 4]> {
    (0..n).map(|i| [pat(i as i32, 1, 0, seed), pat(i as i32, 2, 1, seed), pat(i as i32, 3, 2, seed), alpha_at(i as i32, 0, seed)]).collect()
}
fn hpx(v: &[[f32; 4]]) -> u64 {
    let mut h = H0;
    for p in v {
        hf(&mut h, p);
    }
    h
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

// ---------------------------------------------------------------------------------------------
// Verbatim copies of crate-private helpers (crates/algo/src/fxutil.rs, image.rs, noise.rs, other2.rs).

fn premultiply(data: &mut [f32], ch: usize, alpha: bool) {
    if !alpha {
        return;
    }
    for px in data.chunks_exact_mut(ch) {
        let a = px[ch - 1];
        for v in px.iter_mut().take(ch - 1) {
            *v *= a;
        }
    }
}
fn unpremultiply(data: &mut [f32], ch: usize, alpha: bool) {
    if !alpha {
        return;
    }
    for px in data.chunks_exact_mut(ch) {
        let a = px[ch - 1];
        for v in px.iter_mut().take(ch - 1) {
            *v = if a > 1e-7 { *v / a } else { 0.0 };
        }
    }
}
fn hash01(x: i32, y: i32, c: u32, seed: u32) -> f32 {
    let mut h = (x as u32).wrapping_mul(0x8da6_b343) ^ (y as u32).wrapping_mul(0xd816_3841) ^ c.wrapping_mul(0xcb1a_b31f) ^ seed.wrapping_mul(0x9e37_79b9);
    h ^= h >> 16;
    h = h.wrapping_mul(0x7feb_352d);
    h ^= h >> 15;
    h = h.wrapping_mul(0x846c_a68b);
    h ^= h >> 16;
    (h >> 8) as f32 / (1u32 << 24) as f32
}
const MAXC: usize = 8;
fn fmt_of(ctx: &Ctx) -> PixelFormat {
    PixelFormat::new(ctx.mode, SampleType::F32, ctx.alpha)
}
fn rgba(ctx: &Ctx, px: &[f32]) -> [f32; 4] {
    to_rgba(&fmt_of(ctx), px)
}
fn set_rgba(ctx: &Ctx, px: &mut [f32], c: [f32; 4]) {
    let mut tmp = [0.0f32; MAXC];
    let n = from_rgba_into(&fmt_of(ctx), c, &mut tmp);
    let m = n.min(px.len());
    px[..m].copy_from_slice(&tmp[..m]);
}
fn native(ctx: &Ctx, c: [f32; 4]) -> [f32; MAXC] {
    let mut tmp = [0.0f32; MAXC];
    from_rgba_into(&fmt_of(ctx), c, &mut tmp);
    tmp
}
fn luma(ctx: &Ctx, px: &[f32]) -> f32 {
    match ctx.mode {
        ColorMode::Grayscale | ColorMode::Bitmap | ColorMode::Duotone => px[0],
        ColorMode::Lab => px[0],
        ColorMode::Rgb => 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2],
        _ => {
            let c = rgba(ctx, px);
            0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]
        }
    }
}
fn premul_window(src: &Image, win: Rect, alpha: bool) -> Vec<f32> {
    let n = src.ch;
    let mut v = Vec::with_capacity(win.width() as usize * win.height() as usize * n);
    for y in win.y0..win.y1 {
        for x in win.x0..win.x1 {
            let a = if alpha { src.get(x, y, n - 1) } else { 1.0 };
            for c in 0..n {
                let s = src.get(x, y, c);
                v.push(if alpha && c < n - 1 { s * a } else { s });
            }
        }
    }
    v
}
fn unpremul_px(px: &mut [f32], alpha: bool) {
    if !alpha {
        return;
    }
    let n = px.len();
    let a = px[n - 1];
    for v in px.iter_mut().take(n - 1) {
        *v = if a > 1e-7 { *v / a } else { 0.0 };
    }
}
fn box_rows_n(buf: &mut [f32], w: usize, n: usize, r: usize, scratch: &mut Vec<f32>) {
    if r == 0 || w == 0 {
        return;
    }
    let norm = 1.0 / (2 * r + 1) as f64;
    for row in buf.chunks_exact_mut(w * n) {
        scratch.clear();
        scratch.extend_from_slice(row);
        let at = |x: isize, c: usize| scratch[(x.clamp(0, w as isize - 1) as usize) * n + c] as f64;
        for c in 0..n {
            let mut acc: f64 = (-(r as isize)..=r as isize).map(|x| at(x, c)).sum();
            for x in 0..w {
                row[x * n + c] = (acc * norm) as f32;
                acc += at(x as isize + r as isize + 1, c) - at(x as isize - r as isize, c);
            }
        }
    }
}
fn box_cols_n(buf: &mut [f32], w: usize, h: usize, n: usize, r: usize, scratch: &mut Vec<f32>) {
    if r == 0 || h == 0 {
        return;
    }
    let norm = 1.0 / (2 * r + 1) as f64;
    for x in 0..w {
        scratch.clear();
        for y in 0..h {
            scratch.extend_from_slice(&buf[(y * w + x) * n..(y * w + x + 1) * n]);
        }
        let at = |y: isize, c: usize| scratch[(y.clamp(0, h as isize - 1) as usize) * n + c] as f64;
        for c in 0..n {
            let mut acc: f64 = (-(r as isize)..=r as isize).map(|y| at(y, c)).sum();
            for y in 0..h {
                buf[(y * w + x) * n + c] = (acc * norm) as f32;
                acc += at(y as isize + r as isize + 1, c) - at(y as isize - r as isize, c);
            }
        }
    }
}
fn box_blur_n(buf: &mut [f32], w: usize, h: usize, n: usize, r: usize) {
    let mut scratch = Vec::with_capacity(w.max(h) * n);
    box_rows_n(buf, w, n, r, &mut scratch);
    box_cols_n(buf, w, h, n, r, &mut scratch);
}
fn gauss_box_radii(sigma: f32) -> [usize; 3] {
    let n = 3.0f32;
    let w_ideal = (12.0 * sigma * sigma / n + 1.0).sqrt();
    let mut wl = w_ideal.floor() as i32;
    if wl % 2 == 0 {
        wl -= 1;
    }
    let wl = wl.max(1);
    let wu = wl + 2;
    let m = ((12.0 * sigma * sigma - n * (wl * wl) as f32 - 4.0 * n * wl as f32 - 3.0 * n) / (-4.0 * wl as f32 - 4.0)).round().clamp(0.0, n) as usize;
    let mut radii = std::array::from_fn(|i| if i < m { (wl as usize - 1) / 2 } else { (wu as usize - 1) / 2 });
    if radii.iter().all(|&r| r == 0) {
        radii[0] = 1;
    }
    radii
}
fn gauss_blur_n(buf: &mut [f32], w: usize, h: usize, n: usize, sigma: f32) {
    if sigma < 0.3 {
        return;
    }
    let mut scratch = Vec::with_capacity(w.max(h) * n);
    for r in gauss_box_radii(sigma) {
        box_rows_n(buf, w, n, r, &mut scratch);
    }
    for r in gauss_box_radii(sigma) {
        box_cols_n(buf, w, h, n, r, &mut scratch);
    }
}
fn cell_point(cx: i32, cy: i32, cell: f32, seed: u32, origin: (f32, f32)) -> (f32, f32) {
    (origin.0 + (cx as f32 + 0.1 + 0.8 * hash01(cx, cy, 11, seed)) * cell, origin.1 + (cy as f32 + 0.1 + 0.8 * hash01(cx, cy, 12, seed)) * cell)
}
fn smoothstep(e0: f32, e1: f32, x: f32) -> f32 {
    if e1 <= e0 {
        return if x >= e1 { 1.0 } else { 0.0 };
    }
    let t = ((x - e0) / (e1 - e0)).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}
fn add_sample_premul(src: &Image, x: f32, y: f32, clip: Rect, alpha: bool, acc: &mut [f32]) {
    if !x.is_finite() || !y.is_finite() || clip.is_empty() {
        return;
    }
    let n = src.ch;
    let fx = (x - 0.5).clamp(clip.x0 as f32, (clip.x1 - 1) as f32);
    let fy = (y - 0.5).clamp(clip.y0 as f32, (clip.y1 - 1) as f32);
    let (x0, y0) = (fx.floor() as i32, fy.floor() as i32);
    let (x1, y1) = ((x0 + 1).min(clip.x1 - 1), (y0 + 1).min(clip.y1 - 1));
    let (ax, ay) = (fx - x0 as f32, fy - y0 as f32);
    let w = src.rect.width() as usize;
    let idx = |xx: i32, yy: i32| ((yy - src.rect.y0) as usize * w + (xx - src.rect.x0) as usize) * n;
    for (i, k) in [(idx(x0, y0), (1.0 - ax) * (1.0 - ay)), (idx(x1, y0), ax * (1.0 - ay)), (idx(x0, y1), (1.0 - ax) * ay), (idx(x1, y1), ax * ay)] {
        if k <= 0.0 {
            continue;
        }
        let px = &src.data[i..i + n];
        let a = if alpha { px[n - 1] * k } else { k };
        if alpha {
            for c in 0..n - 1 {
                acc[c] += px[c] * a;
            }
            acc[n - 1] += a;
        } else {
            for c in 0..n {
                acc[c] += px[c] * a;
            }
        }
    }
}
fn rgb_to_hsl(c: [f32; 3]) -> [f32; 3] {
    let (mx, mn) = (c[0].max(c[1]).max(c[2]), c[0].min(c[1]).min(c[2]));
    let d = mx - mn;
    let l = (mx + mn) / 2.0;
    let s = if d <= 0.0 { 0.0 } else { d / (1.0 - (2.0 * l - 1.0).abs()).max(1e-7) };
    [hue(c, mx, d), s.clamp(0.0, 1.0), l]
}
fn hue(c: [f32; 3], mx: f32, d: f32) -> f32 {
    if d <= 0.0 {
        return 0.0;
    }
    let h = if mx == c[0] {
        ((c[1] - c[2]) / d).rem_euclid(6.0)
    } else if mx == c[1] {
        (c[2] - c[0]) / d + 2.0
    } else {
        (c[0] - c[1]) / d + 4.0
    };
    h / 6.0
}
fn from_hcm(h: f32, chroma: f32, m: f32) -> [f32; 3] {
    let hp = h.rem_euclid(1.0) * 6.0;
    let x = chroma * (1.0 - (hp.rem_euclid(2.0) - 1.0).abs());
    let (r, g, b) = match hp as i32 {
        0 => (chroma, x, 0.0),
        1 => (x, chroma, 0.0),
        2 => (0.0, chroma, x),
        3 => (0.0, x, chroma),
        4 => (x, 0.0, chroma),
        _ => (chroma, 0.0, x),
    };
    [r + m, g + m, b + m]
}
fn hsb_to_rgb(c: [f32; 3]) -> [f32; 3] {
    let chroma = c[2] * c[1];
    from_hcm(c[0], chroma, c[2] - chroma)
}
fn hsl_to_rgb(c: [f32; 3]) -> [f32; 3] {
    let chroma = (1.0 - (2.0 * c[2] - 1.0).abs()) * c[1];
    from_hcm(c[0], chroma, c[2] - chroma / 2.0)
}
// End of verbatim copies.
// ---------------------------------------------------------------------------------------------

fn default_like<T: Default>(_: &T) -> T {
    T::default()
}
macro_rules! enum_line {
    ($name:expr, &[$first:expr $(, $v:expr)* $(,)?] $(,)?) => {{
        let names: Vec<String> = vec![serde_json::to_string(&$first).unwrap_or_default() $(, serde_json::to_string(&$v).unwrap_or_default())*];
        let def = serde_json::to_string(&default_like(&$first)).unwrap_or_default();
        println!("enum {} {} def={}", $name, names.join(","), def);
    }};
}

fn enums() {
    enum_line!("RadialMethod", &[RadialMethod::Spin, RadialMethod::Zoom]);
    enum_line!("RadialQuality", &[RadialQuality::Draft, RadialQuality::Good, RadialQuality::Best]);
    enum_line!("Distribution", &[Distribution::Uniform, Distribution::Gaussian]);
    enum_line!("UndefinedAreas", &[UndefinedAreas::Wrap, UndefinedAreas::Repeat, UndefinedAreas::Transparent]);
    enum_line!("Preserve", &[Preserve::Squareness, Preserve::Roundness]);
    enum_line!("SpherizeMode", &[SpherizeMode::Normal, SpherizeMode::HorizontalOnly, SpherizeMode::VerticalOnly]);
    enum_line!("WaveType", &[WaveType::Sine, WaveType::Triangle, WaveType::Square]);
    enum_line!("RippleSize", &[RippleSize::Small, RippleSize::Medium, RippleSize::Large]);
    enum_line!("PolarMode", &[PolarMode::RectangularToPolar, PolarMode::PolarToRectangular]);
    use MezzotintType as M;
    enum_line!(
        "MezzotintType",
        &[M::FineDots, M::MediumDots, M::GrainyDots, M::CoarseDots, M::ShortLines, M::MediumLines, M::LongLines, M::ShortStrokes, M::MediumStrokes, M::LongStrokes],
    );
    enum_line!("DiffuseMode", &[DiffuseMode::Normal, DiffuseMode::DarkenOnly, DiffuseMode::LightenOnly, DiffuseMode::Anisotropic]);
    enum_line!("ExtrudeType", &[ExtrudeType::Blocks, ExtrudeType::Pyramids]);
    enum_line!("TileFill", &[TileFill::Background, TileFill::Foreground, TileFill::Inverse, TileFill::Unaltered]);
    enum_line!("WindMethod", &[WindMethod::Wind, WindMethod::Blast, WindMethod::Stagger]);
    enum_line!("ZigZagStyle", &[ZigZagStyle::AroundCenter, ZigZagStyle::OutFromCenter, ZigZagStyle::PondRipples]);
    enum_line!("LensType", &[LensType::Zoom, LensType::Prime35, LensType::Prime105, LensType::MoviePrime]);
    enum_line!("LightKind", &[LightKind::Spot, LightKind::Point, LightKind::Infinite]);
    use TextureChannel as T;
    enum_line!("TextureChannel", &[T::None, T::Red, T::Green, T::Blue, T::Alpha, T::Luminance]);
    enum_line!("BlurQuality", &[BlurQuality::Low, BlurQuality::Medium, BlurQuality::High]);
    enum_line!("SmartBlurMode", &[SmartBlurMode::Normal, SmartBlurMode::EdgeOnly, SmartBlurMode::OverlayEdge]);
    enum_line!("DepthSource", &[DepthSource::None, DepthSource::Transparency, DepthSource::LayerMask]);
    use BlurShape as B;
    enum_line!("BlurShape", &[B::Circle, B::Ring, B::Square, B::Diamond, B::Triangle, B::Hexagon, B::Star, B::Heart, B::Cross]);
    enum_line!("HsbModel", &[HsbModel::Rgb, HsbModel::Hsb, HsbModel::Hsl]);
    // R5: Bicubic Smoother / Sharper / Automatic added upstream.
    enum_line!(
        "Resample",
        &[
            Resample::Nearest,
            Resample::Bilinear,
            Resample::Bicubic,
            Resample::BicubicSmoother,
            Resample::BicubicSharper,
            Resample::BicubicAutomatic,
            Resample::Lanczos,
            Resample::PreserveDetails
        ]
    );
    enum_line!("Interp", &[Interp::Nearest, Interp::Bilinear, Interp::Bicubic]);
    for s in ["nearest", "nearestNeighbor", "bilinear", "bicubic", "x", ""] {
        println!("interp_parse {:?} {}", s, serde_json::to_string(&Interp::parse(s)).unwrap_or_default());
    }
    println!("light_default {}", serde_json::to_string(&Light::default()).unwrap_or_default());
    println!("iris_default {}", serde_json::to_string(&IrisPin::default()).unwrap_or_default());
    println!("field_default {}", serde_json::to_string(&FieldPin::default()).unwrap_or_default());
    println!("spin_default {}", serde_json::to_string(&SpinPin::default()).unwrap_or_default());
    println!("path_default {}", serde_json::to_string(&BlurPath::default()).unwrap_or_default());
}

/// FilterParams serde: from_value (no positions; keys sorted, last duplicate wins) and to_string.
fn filter_serde() {
    let inputs: &[&str] = &[
        r#"{"filter":"gaussianBlur","radius":2.5}"#,
        r#"{"filter":"gaussianBlur","radius":1e-7}"#,
        r#"{"filter":"gaussianBlur","radius":3}"#,
        r#"{"filter":"gaussianBlur","radius":-0}"#,
        r#"{"filter":"gaussianBlur","radius":1e300}"#,
        r#"{"filter":"gaussianBlur","radius":0.1,"extra":[1,2]}"#,
        r#"{"filter":"gaussianBlur"}"#,
        r#"{"filter":"gaussianBlur","radius":"a"}"#,
        r#"{"filter":"gaussianBlur","radius":null}"#,
        r#"{"filter":"gaussianBlur","radius":true}"#,
        r#"{"filter":"gaussianBlur","radius":[]}"#,
        r#"{"filter":"gaussianBlur","radius":{}}"#,
        r#"{"filter":"gaussianBlur","radius":1,"radius":2}"#,
        r#"{"filter":"nope"}"#,
        r#"{"filter":0,"radius":4}"#,
        r#"{"filter":58}"#,
        r#"{"filter":59}"#,
        r#"{"filter":-1}"#,
        r#"{"filter":1.5}"#,
        r#"{"filter":true}"#,
        r#"{"filter":null}"#,
        r#"{"radius":1}"#,
        r#"{}"#,
        r#"[1]"#,
        r#""gaussianBlur""#,
        r#"null"#,
        r#"7"#,
        r#"{"filter":"minimum","radius":3}"#,
        r#"{"filter":"maximum","radius":2,"preserve":"roundness"}"#,
        r#"{"filter":"minimum","radius":3,"preserve":"round"}"#,
        r#"{"filter":"minimum","radius":3,"preserve":{"roundness":null}}"#,
        r#"{"filter":"minimum","radius":3,"preserve":{"roundness":1}}"#,
        r#"{"filter":"minimum","radius":3,"preserve":{"a":null,"b":null}}"#,
        r#"{"filter":"minimum","radius":3,"preserve":{}}"#,
        r#"{"filter":"minimum","radius":3,"preserve":1}"#,
        r#"{"filter":"radialBlur","amount":20.0,"method":"spin","centerX":0.5,"centerY":0.5}"#,
        r#"{"filter":"radialBlur","amount":20.0,"method":"zoom","quality":"best","centerX":0.25,"centerY":0.75}"#,
        r#"{"filter":"radialBlur","amount":20.0,"method":"spiral","centerX":0.5,"centerY":0.5}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":false,"seed":3}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":1,"seed":3}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":true,"seed":-1}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":true,"seed":1.5}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":true,"seed":5000000000}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"gaussian","monochromatic":true,"seed":4294967295}"#,
        r#"{"filter":"addNoise","amount":25,"distribution":"poisson","monochromatic":true,"seed":0}"#,
        r#"{"filter":"offset","horizontal":7,"vertical":-3,"undefined":"wrap"}"#,
        r#"{"filter":"offset","horizontal":3000000000,"vertical":0,"undefined":"wrap"}"#,
        r#"{"filter":"offset","horizontal":-3000000000,"vertical":0,"undefined":"wrap"}"#,
        r#"{"filter":"offset","horizontal":0.5,"vertical":0,"undefined":"wrap"}"#,
        r#"{"filter":"offset","horizontal":1,"vertical":0}"#,
        r#"{"filter":"colorHalftone","maxRadius":4,"angles":[108,162,90,45]}"#,
        r#"{"filter":"colorHalftone","maxRadius":4,"angles":[108,162,90]}"#,
        r#"{"filter":"colorHalftone","maxRadius":4,"angles":[108,162,90,45,1]}"#,
        r#"{"filter":"colorHalftone","maxRadius":4,"angles":[108,162,"x",45]}"#,
        r#"{"filter":"colorHalftone","maxRadius":4,"angles":5}"#,
        r#"{"filter":"lightingEffects","lights":[{}],"gloss":0,"metallic":0,"exposure":0,"ambience":10,"texture":"luminance","height":50,"whiteIsHigh":true}"#,
        r#"{"filter":"lightingEffects","lights":[{"kind":"point","x":0.1,"color":[0.5,0.25,1]},{"intensity":-20}],"gloss":1,"metallic":2,"exposure":3,"ambience":4,"texture":"none","height":5,"whiteIsHigh":false}"#,
        r#"{"filter":"lightingEffects","lights":[{"x":"a"}],"gloss":0,"metallic":0,"exposure":0,"ambience":10,"texture":"luminance","height":50,"whiteIsHigh":true}"#,
        r#"{"filter":"lightingEffects","lights":[3],"gloss":0,"metallic":0,"exposure":0,"ambience":10,"texture":"luminance","height":50,"whiteIsHigh":true}"#,
        r#"{"filter":"lightingEffects","lights":{},"gloss":0,"metallic":0,"exposure":0,"ambience":10,"texture":"luminance","height":50,"whiteIsHigh":true}"#,
        r#"{"filter":"lightingEffects","lights":[{"color":[1,2]}],"gloss":0,"metallic":0,"exposure":0,"ambience":10,"texture":"luminance","height":50,"whiteIsHigh":true}"#,
        r#"{"filter":"shear","points":[[0,0],[0.5,0.3],[1,0]],"undefined":"repeat"}"#,
        r#"{"filter":"shear","points":[],"undefined":"transparent"}"#,
        r#"{"filter":"shear","points":[[0,0],[1]],"undefined":"wrap"}"#,
        r#"{"filter":"shear","points":"x","undefined":"wrap"}"#,
        r#"{"filter":"irisBlur","pins":[{},{"x":0.1,"radiusX":0.2,"blur":3}]}"#,
        r#"{"filter":"fieldBlur","pins":[{"blur":40}]}"#,
        r#"{"filter":"spinBlur","pins":[{"blurAngle":30,"angle":5}]}"#,
        r#"{"filter":"pathBlur","paths":[{},{"points":[[0,0],[1,1],[0.5,0.25]],"speed":10,"taper":20}]}"#,
        r#"{"filter":"pathBlur","paths":[{"points":[]}]}"#,
        r#"{"filter":"custom","kernel":[0,0,0,0,0,0,0,1,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0],"scale":1,"offset":0}"#,
        r#"{"filter":"custom","kernel":[1,"a"],"scale":1,"offset":0}"#,
        r#"{"filter":"findEdges"}"#,
        r#"{"filter":"findEdges","x":1}"#,
        r#"{"filter":"invert"}"#,
        r#"{"filter":"displace","horizontal":10,"vertical":10,"stretch":true,"undefined":"wrap"}"#,
        r#"{"filter":"displace","horizontal":10,"vertical":10,"stretch":true,"undefined":"wrap","map":5}"#,
    ];
    for s in inputs {
        let v: serde_json::Value = match serde_json::from_str(s) {
            Ok(v) => v,
            Err(e) => {
                println!("de {}\tPARSE {}", s, e);
                continue;
            }
        };
        match serde_json::from_value::<FilterParams>(v) {
            Ok(p) => println!("de {}\t{}", s, serde_json::to_string(&p).unwrap_or_default()),
            Err(e) => println!("de {}\tERR {}", s, e),
        }
    }
    // Serialisation of awkward floats (f32 text, non-finite as null).
    for r in [0.1f32, 1e-7, 1e20, 3.4028235e38, -0.0, 16777216.0, f32::NAN, f32::INFINITY, 1.0 / 3.0] {
        println!("ser {}", serde_json::to_string(&FilterParams::GaussianBlur { radius: r }).unwrap_or_default());
    }
    println!(
        "ser {}",
        serde_json::to_string(&FilterParams::Offset { horizontal: i32::MIN, vertical: i32::MAX, undefined: UndefinedAreas::Transparent }).unwrap_or_default()
    );
    println!(
        "ser {}",
        serde_json::to_string(&FilterParams::AddNoise { amount: 1.5, distribution: Distribution::Uniform, monochromatic: true, seed: u32::MAX }).unwrap_or_default()
    );
    // Halos of the variants whose halo L4 defines (lib.rs), and labels.
    for p in [
        FilterParams::GaussianBlur { radius: 2.0 },
        FilterParams::GaussianBlur { radius: -1.0 },
        FilterParams::GaussianBlur { radius: f32::NAN },
        FilterParams::GaussianBlur { radius: 1e30 },
        FilterParams::BoxBlur { radius: 2.5 },
        FilterParams::MotionBlur { angle: 0.0, distance: -7.0 },
        FilterParams::Mosaic { cell_size: 0.2 },
        FilterParams::Emboss { angle: 1.0, height: -3.5, amount: 1.0 },
        FilterParams::FindEdges,
        FilterParams::Invert,
        FilterParams::Twirl { angle: 1.0 },
    ] {
        let h = match p.halo() {
            Halo::Radius(r) => format!("r{r}"),
            Halo::Bounds => "bounds".into(),
        };
        let content = Rect::new(10, 10, 20, 20);
        let r = Rect::new(0, 0, 40, 30);
        let mut hh = H0;
        hrect(&mut hh, output_area(&p, content, r, None));
        hrect(&mut hh, output_area(&p, content, r, Some(Rect::new(0, 0, 12, 12))));
        hrect(&mut hh, output_area(&p, Rect::EMPTY, r, None));
        println!("halo {} {} {} {:016x}", serde_json::to_string(&p).unwrap_or_default(), p.label(), h, hh);
    }
}

fn image_vectors() {
    // Image::read / read_clamped / crop / get / get_edge / sample.
    for (name, fmt) in formats() {
        let s = surf(fmt, Rect::new(-3, 2, 37, 29), 5, false);
        let mut h = H0;
        for r in [Rect::new(0, 0, 10, 10), Rect::new(-10, -5, 50, 40), Rect::new(30, 25, 300, 270), Rect::EMPTY] {
            let a = Image::read(&s, r);
            hf(&mut h, &a.data);
            for e in [Rect::new(0, 0, 30, 20), Rect::new(-3, 2, 37, 29), Rect::new(5, 5, 6, 6), Rect::new(100, 100, 110, 110)] {
                let b = Image::read_clamped(&s, r, e);
                hrect(&mut h, b.rect);
                hf(&mut h, &b.data);
            }
        }
        println!("image_read {} {:016x}", name, h);
    }
    let fmt = PixelFormat::new(ColorMode::Rgb, SampleType::F32, true);
    let s = surf(fmt, Rect::new(2, 3, 9, 8), 1, false);
    let img = Image::read(&s, Rect::new(2, 3, 9, 8));
    let areas = [Rect::new(2, 3, 9, 8), Rect::new(0, 0, 5, 5), Rect::EMPTY, Rect::new(3, 4, 4, 5)];
    for (ei, edge) in [Edge::Transparent, Edge::Repeat, Edge::Wrap].into_iter().enumerate() {
        let mut h = H0;
        for area in areas {
            for y in -6..15 {
                for x in -7..16 {
                    for c in 0..4 {
                        hf(&mut h, &[img.get_edge(x, y, c, edge, area)]);
                    }
                }
            }
        }
        println!("get_edge {} {:016x}", ei, h);
        let mut h = H0;
        for area in areas {
            for alpha in [false, true] {
                let mut y = -2.3f32;
                while y < 11.0 {
                    let mut x = -3.1f32;
                    while x < 12.0 {
                        let mut out = [0.0f32; 4];
                        img.sample(x, y, edge, area, alpha, &mut out);
                        hf(&mut h, &out);
                        x += 0.37;
                    }
                    y += 0.41;
                }
            }
        }
        println!("sample {} {:016x}", ei, h);
    }
    println!("crop {:016x} {:016x}", of(&img.crop(Rect::new(0, 0, 12, 10))), of(&img.crop(Rect::new(4, 4, 6, 7))));
    let mut v = img.data.clone();
    premultiply(&mut v, 4, true);
    let p = of(&v);
    unpremultiply(&mut v, 4, true);
    println!("premul {:016x} {:016x}", p, of(&v));
}

fn fxutil_vectors() {
    let mut h = H0;
    for y in -20..20 {
        for x in -20..20 {
            for (c, seed) in [(0u32, 0u32), (11, 7), (12, 0xffff_ffff), (3, 12345)] {
                hf(&mut h, &[hash01(x * 977, y * 131, c, seed)]);
            }
        }
    }
    hf(&mut h, &[hash01(i32::MIN, i32::MAX, u32::MAX, u32::MAX)]);
    println!("hash01 {:016x}", h);
    let mut h = H0;
    for cy in -5..5 {
        for cx in -5..5 {
            for (cell, seed) in [(6.0f32, 1u32), (17.5, 9)] {
                let p = cell_point(cx, cy, cell, seed, (3.5, -2.25));
                hf(&mut h, &[p.0, p.1]);
            }
        }
    }
    println!("cell_point {:016x}", h);
    let mut line = String::new();
    for sigma in [0.0f32, 0.2, 0.3, 0.5, 0.577, 0.6, 1.0, 1.5, 2.5, 7.3, 30.0, 1000.0, 1e9, f32::NAN, -5.0] {
        let r = gauss_box_radii(sigma);
        line.push_str(&format!(" {}:{},{},{}", sigma, r[0], r[1], r[2]));
    }
    println!("gauss_box_radii{}", line);
    for (w, hh, n) in [(17usize, 11usize, 1usize), (9, 23, 3), (1, 7, 4), (7, 1, 2), (40, 30, 5)] {
        let mut out = H0;
        for r in [0usize, 1, 2, 5, 30] {
            let mut b = buf(w * hh * n, r as i32);
            box_blur_n(&mut b, w, hh, n, r);
            hf(&mut out, &b);
        }
        for sigma in [0.2f32, 0.5, 1.0, 2.5, 7.3, 30.0] {
            let mut b = buf(w * hh * n, 3);
            gauss_blur_n(&mut b, w, hh, n, sigma);
            hf(&mut out, &b);
        }
        println!("box_gauss {}x{}x{} {:016x}", w, hh, n, out);
    }
    let mut h = H0;
    for e0 in [0.0f32, 0.25, 1.0] {
        for e1 in [0.0f32, 0.5, 1.0] {
            let mut x = -0.5f32;
            while x < 1.5 {
                hf(&mut h, &[smoothstep(e0, e1, x)]);
                x += 0.0625;
            }
        }
    }
    hf(&mut h, &[smoothstep(0.0, 1.0, f32::NAN)]);
    println!("smoothstep {:016x}", h);
    let fmt = PixelFormat::new(ColorMode::Rgb, SampleType::F32, true);
    let img = Image::read(&surf(fmt, Rect::new(0, 0, 9, 7), 2, false), Rect::new(0, 0, 9, 7));
    let mut h = H0;
    for clip in [Rect::new(0, 0, 9, 7), Rect::new(2, 1, 6, 5), Rect::new(4, 4, 5, 5), Rect::EMPTY] {
        for alpha in [false, true] {
            let mut acc = [0.0f32; 4];
            let mut y = -1.3f32;
            while y < 9.0 {
                let mut x = -1.7f32;
                while x < 11.0 {
                    add_sample_premul(&img, x, y, clip, alpha, &mut acc);
                    x += 0.53;
                }
                y += 0.61;
            }
            add_sample_premul(&img, f32::NAN, 1.0, clip, alpha, &mut acc);
            add_sample_premul(&img, 1.0, f32::INFINITY, clip, alpha, &mut acc);
            hf(&mut h, &acc);
        }
    }
    println!("add_sample_premul {:016x}", h);
    let mut h = H0;
    for win in [Rect::new(-2, -2, 11, 9), Rect::new(3, 3, 4, 4), Rect::EMPTY] {
        for alpha in [false, true] {
            let mut v = premul_window(&img, win, alpha);
            hf(&mut h, &v);
            for px in v.chunks_exact_mut(4) {
                unpremul_px(px, alpha);
            }
            hf(&mut h, &v);
        }
    }
    println!("premul_window {:016x}", h);
    // Colour access per model.
    for mode in [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab, ColorMode::Bitmap, ColorMode::Duotone, ColorMode::Indexed, ColorMode::Multichannel] {
        for alpha in [false, true] {
            let ctx = Ctx { bounds: Rect::new(0, 0, 10, 10), mode, alpha };
            let n = PixelFormat::new(mode, SampleType::F32, alpha).channels();
            let mut h = H0;
            for i in 0..64 {
                let px: Vec<f32> = (0..n).map(|c| pat(i, 3, c, 4)).collect();
                hf(&mut h, &[luma(&ctx, &px)]);
                hf(&mut h, &rgba(&ctx, &px));
                let c = [pat(i, 0, 0, 1), pat(i, 0, 1, 1), pat(i, 0, 2, 1), alpha_at(i, 0, 1)];
                let mut q = px.clone();
                set_rgba(&ctx, &mut q, c);
                hf(&mut h, &q);
                hf(&mut h, &native(&ctx, c));
            }
            println!("ctx_colour {:?} {} {:016x}", mode, alpha, h);
        }
    }
    let mut h = H0;
    for i in 0..400 {
        let c = [pat(i, 0, 0, 2), pat(i, 1, 1, 3), pat(i, 2, 2, 5)];
        let c = if i % 17 == 0 { [c[0], c[0], c[0]] } else if i % 13 == 0 { [c[0], c[0], c[2]] } else { c };
        let hsl = rgb_to_hsl(c);
        hf(&mut h, &hsl);
        hf(&mut h, &hsl_to_rgb(hsl));
        hf(&mut h, &hsb_to_rgb(c));
        hf(&mut h, &hsl_to_rgb([c[0] * 3.0 - 1.0, c[1], c[2]]));
    }
    println!("hsl {:016x}", h);
    let px = rgba_px(300, 4);
    let mut h = H0;
    for fuzz in [0.0f32, 10.0, 40.0, 200.0, -5.0] {
        hf(&mut h, &photocraft_algo::selection::color_range(&px, [0.5, 0.25, 0.75], fuzz));
    }
    println!("color_range {:016x}", h);
}

fn histogram_vectors() {
    let mut px = rgba_px(500, 6);
    for (i, p) in px.iter_mut().enumerate() {
        match i % 23 {
            0 => p[0] = f32::NAN,
            1 => p[3] = f32::INFINITY,
            2 => p[1] = -0.5,
            3 => p[2] = 3.0,
            4 => p[0] = 1.0,
            5 => p[1] = 0.0,
            6 => p[3] = -1.0,
            7 => p[2] = f32::NEG_INFINITY,
            _ => {}
        }
    }
    for n in [0usize, 1, 50, 500] {
        let h = RgbHistogram::from_rgba(&px[..n]);
        let mut x = H0;
        for c in &h.channels {
            hu(&mut x, c);
        }
        hu(&mut x, &[h.samples, h.transparent, h.invalid]);
        hu(&mut x, &h.underflow);
        hu(&mut x, &h.overflow);
        hu(&mut x, &h.shadows);
        hu(&mut x, &h.highlights);
        println!("histogram {} {:016x} {} {} {}", n, x, h.samples, h.transparent, h.invalid);
    }
    let mut x = H0;
    for p in &px {
        let (s, hh) = clipping(*p);
        fnv(&mut x, &[s, hh]);
    }
    println!("clipping {:016x}", x);
}

fn ramp(w: usize, h: usize) -> Vec<[f32; 4]> {
    (0..w * h)
        .map(|i| {
            let x = (i % w) as f32 / (w - 1) as f32;
            [x, x * 0.8, x * 0.6 + 0.1, 1.0]
        })
        .collect()
}

fn tone_vectors() {
    let (w, h) = (37usize, 23usize);
    let base = rgba_px(w * h, 8);
    let sh_cases = [
        ShadowsHighlights::default(),
        ShadowsHighlights { black_clip: 0.0, white_clip: 0.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 0.0, highlight_amount: 80.0, black_clip: 0.0, white_clip: 0.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 70.0, shadow_radius: 3.0, highlight_amount: 40.0, highlight_radius: 9.0, color: -40.0, midtone: 35.0, black_clip: 2.0, white_clip: 5.0, ..Default::default() },
        ShadowsHighlights { shadow_tone: 0.0, highlight_tone: 0.0, midtone: -100.0, black_clip: 50.0, white_clip: 60.0, ..Default::default() },
    ];
    for (i, p) in sh_cases.iter().enumerate() {
        let mut a = base.clone();
        tone::shadows_highlights(&mut a, w, h, p);
        let mut r = ramp(32, 8);
        tone::shadows_highlights(&mut r, 32, 8, p);
        println!("shadows_highlights {} {:016x} {:016x}", i, hpx(&a), hpx(&r));
    }
    for (i, (color, fuzz, hue, sat, light)) in
        [([1.0f32, 0.0, 0.0], 40.0f32, 120.0f32, 0.0f32, 0.0f32), ([0.5, 0.4, 0.3], 80.0, -45.0, 30.0, -20.0), ([0.2, 0.9, 0.1], 200.0, 400.0, -60.0, 70.0)].into_iter().enumerate()
    {
        let mut a = base.clone();
        let m = tone::replace_color(&mut a, color, fuzz, hue, sat, light);
        println!("replace_color {} {:016x} {:016x}", i, hpx(&a), of(&m));
    }
    let mask: Vec<f32> = (0..w * h / 2).map(|i| pat(i as i32, 0, 0, 9)).collect();
    let st = |s: Option<tone::LabStats>| match s {
        Some(s) => format!("{:016x}", of(&[s.mean, s.std].concat())),
        None => "none".into(),
    };
    println!("lab_stats {} {} {} {}", st(tone::lab_stats(&base, None)), st(tone::lab_stats(&base, Some(&mask))), st(tone::lab_stats(&[], None)), st(tone::lab_stats(&base[..3], Some(&[0.0, 0.0]))));
    let target = tone::lab_stats(&base, None).unwrap_or(tone::LabStats { mean: [0.0; 3], std: [0.0; 3] });
    let src = tone::lab_stats(&ramp(16, 4), None).unwrap_or(target);
    for (i, o) in [
        MatchColor::default(),
        MatchColor { neutralize: true, ..Default::default() },
        MatchColor { luminance: 150.0, intensity: 20.0, fade: 30.0, neutralize: false },
        MatchColor { luminance: 1.0, intensity: 200.0, fade: 100.0, neutralize: true },
    ]
    .iter()
    .enumerate()
    {
        let mut a = base.clone();
        tone::match_color(&mut a, &target, Some(&src), o);
        let mut b = base.clone();
        tone::match_color(&mut b, &target, None, o);
        println!("match_color {} {:016x} {:016x}", i, hpx(&a), hpx(&b));
    }
    let hdr_cases = [
        HdrToning::default(),
        HdrToning { strength: 2.0, detail: 0.0, saturation: 0.0, ..Default::default() },
        HdrToning { strength: 0.0, detail: 0.0, saturation: 0.0, curve: vec![(0.0, 1.0), (1.0, 0.0)], ..Default::default() },
        HdrToning { radius: 3.0, gamma: 0.4, exposure: 1.5, detail: 120.0, shadow: 60.0, highlight: 40.0, vibrance: 50.0, saturation: -30.0, curve: vec![(0.7, 0.9), (0.2, 0.1), (f32::NAN, 0.5), (0.2, 0.3), (-0.0, 0.05), (0.0, 0.0)], ..Default::default() },
        HdrToning { radius: 500.0, strength: 4.0, gamma: 0.0, exposure: -5.0, detail: -100.0, curve: vec![(0.5, 0.5)], ..Default::default() },
    ];
    for (i, p) in hdr_cases.iter().enumerate() {
        let mut a = base.clone();
        tone::hdr_toning(&mut a, w, h, p);
        let mut r = ramp(40, 10);
        tone::hdr_toning(&mut r, 40, 10, p);
        println!("hdr_toning {} {:016x} {:016x}", i, hpx(&a), hpx(&r));
    }
}

fn resample_vectors() {
    let filters = [Resample::Nearest, Resample::Bilinear, Resample::Bicubic, Resample::Lanczos, Resample::PreserveDetails];
    let src_rect = Rect::new(-5, 3, 61, 37);
    for (name, fmt) in formats() {
        let s = surf(fmt, src_rect, 2, false);
        for (fi, f) in filters.iter().enumerate() {
            let mut h = H0;
            let mut hp = H0;
            for (kx, ky) in [(0.013, 0.013), (0.37, 0.37), (0.5, 0.5), (1.0, 1.0), (1.37, 1.37), (2.0, 2.0), (2.3, 0.7), (1.0, 0.01)] {
                let preserve_up = *f == Resample::PreserveDetails && (kx > 1.0 || ky > 1.0);
                let o = resize_surface(&s, kx, ky, *f);
                if preserve_up {
                    fnv(&mut hp, &hs(&o).to_le_bytes());
                } else {
                    fnv(&mut h, &hs(&o).to_le_bytes());
                }
            }
            println!("resize {} {} {:016x}", name, fi, h);
            if *f == Resample::PreserveDetails {
                println!("resize_pd {} {:016x}", name, hp);
            }
        }
    }
    // Canvas-edge repeat, masks with a white default, chunked reads, sparse sources.
    let canvas = Rect::new(0, 0, 30, 20);
    for (name, fmt) in formats() {
        let mut s = Surface::new(fmt);
        let px: Vec<f32> = (0..fmt.channels()).map(|c| [0.2, 0.6, 1.0, 1.0, 0.5][c]).collect();
        s.fill_rect(canvas, &px);
        let inner = surf(fmt, Rect::new(10, 5, 20, 15), 4, false);
        for (fi, f) in filters.iter().enumerate() {
            if *f == Resample::PreserveDetails {
                continue;
            }
            let mut h = H0;
            for k in [2.0, 1.37, 0.5, 0.11] {
                fnv(&mut h, &hs(&resize_surface_in_canvas(&s, k, k, *f, canvas)).to_le_bytes());
                fnv(&mut h, &hs(&resize_surface_in_canvas(&inner, k, k * 1.1, *f, canvas)).to_le_bytes());
            }
            println!("resize_canvas {} {} {:016x}", name, fi, h);
        }
    }
    let mut mask = Surface::with_default(PixelFormat::GRAY8, &[1.0]);
    mask.fill_rect(Rect::new(0, 0, 40, 30), &[0.0]);
    mask.fill_rect(Rect::new(500, 900, 530, 940), &[0.25]);
    let mut sparse = Surface::new(PixelFormat::RGBA32F);
    sparse.fill_rect(Rect::new(0, 0, 256, 256), &[1.0, 1.0, 1.0, 1.0]);
    sparse.fill_rect(Rect::new(99_840, 99_840, 100_096, 100_096), &[0.5, 0.25, 1.0, 1.0]);
    for (i, f) in filters.iter().take(4).enumerate() {
        let mut h = H0;
        for k in [0.013, 0.37, 1.6] {
            fnv(&mut h, &hs(&resize_surface(&mask, k, k, *f)).to_le_bytes());
        }
        let k = 1.0 / 100_096.0;
        fnv(&mut h, &hs(&resize_surface(&sparse, k, k, *f)).to_le_bytes());
        fnv(&mut h, &hs(&resize_surface(&sparse, 1000.0 / 100_096.0, k, *f)).to_le_bytes());
        println!("resize_sparse {} {:016x}", i, h);
    }
    for (kx, ky) in [(f64::NAN, 1.0), (0.0, 1.0), (-1.0, -1.0), (1.0, -0.5)] {
        let o = resize_surface(&mask, kx, ky, Resample::Bicubic);
        println!("resize_degenerate {} {} {:016x}", kx, ky, hs(&o));
    }
    let mut h = H0;
    for r in [Rect::new(-3, -3, 3, 3), Rect::new(0, 0, 1, 1), Rect::new(-100, 5, 7, 900), Rect::EMPTY] {
        for (sx, sy) in [(0.5, 0.5), (1.0 / 3.0, 3.0), (1e-9, 1e9), (2.5, 0.013)] {
            hrect(&mut h, scaled_rect(r, sx, sy));
        }
    }
    println!("scaled_rect {:016x}", h);
    let s = surf(PixelFormat::RGBA8, Rect::new(0, 0, 64, 32), 3, false);
    let mut h = H0;
    for (dx, dy) in [(5, -3), (256, 0), (-300, 512), (0, 0)] {
        fnv(&mut h, &hs(&translate_surface(&s, dx, dy)).to_le_bytes());
    }
    for keep in [Rect::new(0, 0, 10, 10), Rect::new(-5, 20, 70, 25), Rect::EMPTY] {
        fnv(&mut h, &hs(&crop_surface(&s, keep)).to_le_bytes());
    }
    println!("translate_crop {:016x}", h);
}

fn transform_vectors() {
    let r = [10.0, 20.0, 110.0, 70.0];
    let quads = [
        [[0.0, 0.0], [200.0, 10.0], [180.0, 150.0], [-20.0, 120.0]],
        [[10.0, 20.0], [110.0, 20.0], [110.0, 70.0], [10.0, 70.0]],
        [[30.0, -10.0], [30.0, 30.0], [10.0, 30.0], [10.0, -10.0]],
        [[0.0, 0.0], [0.0, 0.0], [0.0, 0.0], [0.0, 0.0]],
        [[0.0, 0.0], [10.0, 0.0], [20.0, 0.0], [30.0, 0.0]],
        [[5.0, 5.0], [6.0, 5.5], [5.5, 6.5], [5.0, 6.0]],
    ];
    let mut h = H0;
    for q in quads {
        match Homography::rect_to_quad(r, q) {
            Some(m) => {
                hd(&mut h, &m.0);
                match m.inverse() {
                    Some(inv) => {
                        hd(&mut h, &inv.0);
                        hd(&mut h, &m.mul(&inv).0);
                    }
                    None => fnv(&mut h, b"noinv"),
                }
                for (x, y) in [(10.0, 20.0), (90.0, 60.0), (-1e6, 3.0), (55.5, 1e-9)] {
                    let p = m.apply(x, y);
                    hd(&mut h, &[p.0, p.1, m.local_scale(x, y)]);
                }
            }
            None => fnv(&mut h, b"none"),
        }
    }
    for rr in [[0.0, 0.0, 0.0, 10.0], [5.0, 5.0, 1.0, 9.0]] {
        fnv(&mut h, if Homography::rect_to_quad(rr, quads[0]).is_some() { b"some" } else { b"none" });
    }
    println!("homography {:016x}", h);
    let fmts = [
        ("rgba8", PixelFormat::RGBA8),
        ("rgba16", PixelFormat::new(ColorMode::Rgb, SampleType::U16, true)),
        ("rgba32", PixelFormat::RGBA32F),
        ("rgb8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, false)),
        ("gray8", PixelFormat::GRAY8),
        ("cmyka8", PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true)),
    ];
    let src_r = Rect::new(4, 4, 44, 24);
    let ms: Vec<(&str, Homography)> = vec![
        ("identity", Homography::IDENTITY),
        ("translate", Homography([1.0, 0.0, 7.0, 0.0, 1.0, -3.0, 0.0, 0.0, 1.0])),
        ("subpixel", Homography([1.0, 0.0, 7.25, 0.0, 1.0, -3.6, 0.0, 0.0, 1.0])),
        ("scale2", Homography::rect_to_quad([4.0, 4.0, 44.0, 24.0], [[0.0, 0.0], [80.0, 0.0], [80.0, 40.0], [0.0, 40.0]]).unwrap_or(Homography::IDENTITY)),
        ("rotate", Homography::rect_to_quad([4.0, 4.0, 44.0, 24.0], [[34.0, -6.0], [34.0, 34.0], [14.0, 34.0], [14.0, -6.0]]).unwrap_or(Homography::IDENTITY)),
        ("perspective", Homography::rect_to_quad([4.0, 4.0, 44.0, 24.0], [[0.0, 0.0], [70.0, 10.0], [60.0, 50.0], [-5.0, 40.0]]).unwrap_or(Homography::IDENTITY)),
        ("shrink8", Homography::rect_to_quad([4.0, 4.0, 44.0, 24.0], [[0.0, 0.0], [5.0, 0.0], [5.0, 2.5], [0.0, 2.5]]).unwrap_or(Homography::IDENTITY)),
        ("shrink3", Homography::rect_to_quad([4.0, 4.0, 44.0, 24.0], [[1.0, 1.0], [14.0, 2.0], [13.0, 8.0], [0.0, 7.0]]).unwrap_or(Homography::IDENTITY)),
        ("flip", Homography([-1.0, 0.0, 50.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0])),
        ("singular", Homography([1.0, 2.0, 0.0, 2.0, 4.0, 0.0, 0.0, 0.0, 1.0])),
        ("infinite", Homography([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0])),
        ("far", Homography([1.0, 0.0, 3.0e6, 0.0, 1.0, -2.0e6, 0.0, 0.0, 1.0])),
    ];
    for (name, fmt) in fmts {
        let s = surf(fmt, src_r, 7, false);
        for (mn, m) in &ms {
            let mut h = H0;
            for interp in [Interp::Nearest, Interp::Bilinear, Interp::Bicubic] {
                fnv(&mut h, &warp_surface(&s, src_r, m, interp).as_ref().map_or(u64::MAX, hs).to_le_bytes());
            }
            fnv(&mut h, &warp_surface(&s, Rect::EMPTY, m, Interp::Bicubic).as_ref().map_or(u64::MAX, hs).to_le_bytes());
            fnv(&mut h, &warp_surface(&s, Rect::new(10, 6, 30, 20), m, Interp::Bilinear).as_ref().map_or(u64::MAX, hs).to_le_bytes());
            println!("warp {} {} {:016x}", name, mn, h);
        }
    }
}

fn level(w: usize, h: usize, ch: usize, seed: i32) -> Level {
    Level { w, h, px: buf(w * h * ch, seed) }
}

fn pyramid_vectors() {
    for (w, h, ch) in [(37usize, 23usize, 2usize), (1, 1, 3), (1, 9, 1), (16, 1, 4), (64, 32, 1), (5, 5, 0)] {
        let l = level(w, h, ch, 3);
        let r = pyramid::reduce(&l, ch);
        let e = pyramid::expand(&l, ch, w * 2 + 1, h * 2);
        let g = pyramid::gaussian(w, h, ch, &l.px, 5);
        let lp = pyramid::laplacian(w, h, ch, &l.px, 4);
        let back = pyramid::collapse(&lp, ch);
        let mut hh = H0;
        hf(&mut hh, &r.px);
        fnv(&mut hh, &[r.w as u8, r.h as u8]);
        hf(&mut hh, &e);
        for x in g.iter().chain(&lp) {
            fnv(&mut hh, &(x.w as u32).to_le_bytes());
            fnv(&mut hh, &(x.h as u32).to_le_bytes());
            hf(&mut hh, &x.px);
        }
        hf(&mut hh, &back);
        println!("pyramid {}x{}x{} {} {:016x}", w, h, ch, g.len(), hh);
    }
    let mut line = String::new();
    for (w, h) in [(0usize, 0usize), (8, 100), (9, 9), (17, 1000), (100000, 100000), (5000, 3000)] {
        line.push_str(&format!(" {}", pyramid::auto_levels(w, h)));
    }
    println!("auto_levels{}", line);
    let (w, h) = (48usize, 40usize);
    for ch in [1usize, 3] {
        let a = buf(w * h * ch, 1);
        let b = buf(w * h * ch, 2);
        let c: Vec<f32> = (0..w * h * ch).map(|i| pat(i as i32, 5, 0, 3) * 0.5).collect();
        let wa: Vec<f32> = (0..w * h).map(|i| if i % w < 24 { 1.0 } else { 0.0 }).collect();
        let wb: Vec<f32> = wa.iter().map(|v| 1.0 - v).collect();
        let wc: Vec<f32> = (0..w * h).map(|i| pat(i as i32, 0, 0, 7)).collect();
        let mut hh = H0;
        for levels in [0usize, 1, 3, pyramid::auto_levels(w, h), 12] {
            hf(&mut hh, &pyramid::blend(w, h, ch, &[&a, &b], &[wa.clone(), wb.clone()], levels));
            hf(&mut hh, &pyramid::blend(w, h, ch, &[&a, &b, &c], &[wa.clone(), wb.clone(), wc.clone()], levels));
        }
        hf(&mut hh, &pyramid::blend(w, h, ch, &[], &[], 3));
        println!("blend {} {:016x}", ch, hh);
    }
    let checker = |x: usize, y: usize| if (x / 2 + y / 2) % 2 == 0 { 0.1f32 } else { 0.9 };
    for (w, h) in [(48usize, 48usize), (200, 130), (3, 2), (1, 1)] {
        let i0: Vec<f32> = (0..w * h).map(|i| if i % w < w / 2 { checker(i % w, i / w) } else { 0.5 }).collect();
        let i1: Vec<f32> = (0..w * h).map(|i| if i % w >= w / 2 { checker(i % w, i / w) } else { pat(i as i32, 1, 1, 1) }).collect();
        let ones = vec![1.0f32; w * h];
        let half: Vec<f32> = (0..w * h).map(|i| if (i / w) % 3 == 0 { 0.0 } else { 1.0 }).collect();
        let mut hh = H0;
        for wts in [pyramid::stack_weights(w, h, &[i0.clone(), i1.clone()], &[ones.clone(), half.clone()]), pyramid::panorama_weights(w, h, &[ones.clone(), half.clone(), i0.clone()])] {
            for p in &wts {
                hf(&mut hh, p);
            }
        }
        println!("weights {}x{} {:016x}", w, h, hh);
    }
}

/// The tiled driver, through the filters whose kernels the C test reimplements (Invert,
/// Solarize, Find Edges, Offset).
fn apply_vectors() {
    let r = Rect::new(0, 0, 40, 30);
    let filters = [
        ("invert", FilterParams::Invert),
        ("solarize", FilterParams::Solarize),
        ("findEdges", FilterParams::FindEdges),
        ("offsetWrap", FilterParams::Offset { horizontal: 7, vertical: -3, undefined: UndefinedAreas::Wrap }),
        ("offsetRepeat", FilterParams::Offset { horizontal: -45, vertical: 31, undefined: UndefinedAreas::Repeat }),
        ("offsetTransparent", FilterParams::Offset { horizontal: 5, vertical: 0, undefined: UndefinedAreas::Transparent }),
    ];
    let mut sel_half = Surface::with_default(PixelFormat::new(ColorMode::Grayscale, SampleType::F32, false), &[0.5]);
    sel_half.fill_rect(Rect::new(20, 0, 40, 10), &[1.0]);
    let sel_grad = surf(PixelFormat::GRAY8, Rect::new(-5, -5, 45, 35), 3, false);
    let sel_none = Surface::new(PixelFormat::GRAY8);
    for (name, fmt) in formats() {
        let s = surf(fmt, Rect::new(-4, 2, 37, 33), 9, false);
        let opaque = surf(fmt, r, 1, true);
        for (fname, p) in &filters {
            let mut h = H0;
            let area = output_area(p, s.content_bounds(), r, None);
            hrect(&mut h, area);
            for tile in [256, 7, 1, 64] {
                fnv(&mut h, &hs(&apply_tiled(&s, p, area, r, None, tile, None)).to_le_bytes());
            }
            fnv(&mut h, &hs(&apply(&s, p, area, r, None)).to_le_bytes());
            for sel in [&sel_half, &sel_grad, &sel_none] {
                fnv(&mut h, &hs(&apply(&s, p, area, r, Some(sel))).to_le_bytes());
            }
            fnv(&mut h, &hs(&apply_in(&s, p, area, r, None, r)).to_le_bytes());
            fnv(&mut h, &hs(&apply_in(&opaque, p, output_area(p, opaque.content_bounds(), r, None), r, Some(&sel_grad), r)).to_le_bytes());
            fnv(&mut h, &hs(&apply_tiled(&s, p, Rect::new(3, 3, 3, 9), r, None, 16, Some(r))).to_le_bytes());
            fnv(&mut h, &hs(&apply_tiled(&s, p, Rect::new(-9, -9, 50, 50), Rect::new(5, 5, 25, 20), None, 9, Some(Rect::new(-2, -2, 30, 31)))).to_le_bytes());
            println!("apply {} {} {:016x}", name, fname, h);
        }
    }
}

pub fn run() {
    enums();
    filter_serde();
    image_vectors();
    fxutil_vectors();
    histogram_vectors();
    tone_vectors();
    resample_vectors();
    transform_vectors();
    pyramid_vectors();
    apply_vectors();
    r5_vectors();
}

/// R5 (upstream resync): Photoshop's bicubic family and two-pass plans, the new Shadows/Highlights,
/// Perspective Crop quads, clipped and horizon-crossing warps, float range kept by warps.
fn r5_vectors() {
    let all = [
        Resample::Nearest,
        Resample::Bilinear,
        Resample::Bicubic,
        Resample::BicubicSmoother,
        Resample::BicubicSharper,
        Resample::BicubicAutomatic,
        Resample::Lanczos,
        Resample::PreserveDetails,
    ];
    let scales = [(0.013, 0.013), (0.2, 0.2), (0.24, 0.3), (0.37, 0.37), (0.5, 0.5), (1.0, 1.0), (1.37, 1.37), (2.0, 2.0), (2.3, 0.7), (0.7, 2.3), (1.0, 0.01), (0.6, 1.0), (1.0, 1.6), (3.1, 3.1)];
    for (name, fmt) in formats() {
        let s = surf(fmt, Rect::new(-5, 3, 61, 37), 3, false);
        for (fi, f) in all.iter().enumerate() {
            let mut h = H0;
            for (kx, ky) in scales {
                fnv(&mut h, &hs(&resize_surface(&s, kx, ky, *f)).to_le_bytes());
            }
            println!("r5_resize {} {} {:016x}", name, fi, h);
        }
    }
    let canvas = Rect::new(0, 0, 30, 20);
    for (name, fmt) in formats() {
        let mut s = Surface::new(fmt);
        let px: Vec<f32> = (0..fmt.channels()).map(|c| [0.2, 0.6, 1.0, 1.0, 0.5][c]).collect();
        s.fill_rect(canvas, &px);
        let inner = surf(fmt, Rect::new(10, 5, 20, 15), 4, false);
        let edge = surf(fmt, Rect::new(0, 0, 30, 20), 5, true);
        for (fi, f) in all.iter().enumerate() {
            let mut h = H0;
            for (kx, ky) in [(2.0, 2.0), (1.37, 1.37), (0.5, 0.5), (0.11, 0.11), (0.2, 0.2), (0.6, 1.7), (1.7, 0.6), (0.3, 0.9)] {
                fnv(&mut h, &hs(&resize_surface_in_canvas(&s, kx, ky, *f, canvas)).to_le_bytes());
                fnv(&mut h, &hs(&resize_surface_in_canvas(&inner, kx, ky * 1.1, *f, canvas)).to_le_bytes());
                fnv(&mut h, &hs(&resize_surface_in_canvas(&edge, kx, ky, *f, canvas)).to_le_bytes());
            }
            println!("r5_resize_canvas {} {} {:016x}", name, fi, h);
        }
    }
    let mut mask = Surface::with_default(PixelFormat::GRAY8, &[1.0]);
    mask.fill_rect(Rect::new(0, 0, 40, 30), &[0.0]);
    mask.fill_rect(Rect::new(500, 900, 530, 940), &[0.25]);
    let mut sparse = Surface::new(PixelFormat::RGBA32F);
    sparse.fill_rect(Rect::new(0, 0, 256, 256), &[1.0, 1.0, 1.0, 1.0]);
    sparse.fill_rect(Rect::new(99_840, 99_840, 100_096, 100_096), &[0.5, 0.25, 1.0, 1.0]);
    for (fi, f) in all.iter().enumerate() {
        let mut h = H0;
        for k in [0.013, 0.37, 1.6] {
            fnv(&mut h, &hs(&resize_surface(&mask, k, k, *f)).to_le_bytes());
        }
        let k = 1.0 / 100_096.0;
        fnv(&mut h, &hs(&resize_surface(&sparse, k, k, *f)).to_le_bytes());
        fnv(&mut h, &hs(&resize_surface(&sparse, 1000.0 / 100_096.0, k, *f)).to_le_bytes());
        println!("r5_resize_sparse {} {:016x}", fi, h);
    }
    // Shadows/Highlights (measured on Photoshop 25.4 upstream).
    let sh = [
        ShadowsHighlights { color: 0.0, black_clip: 0.0, white_clip: 0.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 100.0, shadow_tone: 100.0, shadow_radius: 10.0, color: 0.0, black_clip: 0.0, white_clip: 0.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 15.0, shadow_tone: 2.5, shadow_radius: 0.1, highlight_amount: 60.0, highlight_tone: 73.0, highlight_radius: 0.2, ..Default::default() },
        ShadowsHighlights { shadow_amount: 50.0, shadow_radius: 12.0, highlight_amount: 50.0, highlight_radius: 12.0, color: 100.0, midtone: 100.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 1.0, highlight_amount: 99.0, highlight_radius: 40.0, color: -100.0, midtone: -60.0, black_clip: 0.5, white_clip: 0.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 0.0, highlight_amount: 0.0, color: 37.0, midtone: 12.5, black_clip: 0.0, white_clip: 3.0, ..Default::default() },
        ShadowsHighlights { shadow_amount: 80.0, shadow_tone: 95.0, shadow_radius: 2500.0, highlight_amount: 30.0, highlight_tone: 0.0, highlight_radius: 7.5, color: -15.0, ..Default::default() },
    ];
    let (w, h) = (61usize, 47usize);
    let mut big = rgba_px(w * h, 21);
    for (i, p) in big.iter_mut().enumerate() {
        if i % 13 == 0 {
            p[3] = 0.0;
        }
    }
    for (i, p) in sh.iter().enumerate() {
        let mut a = big.clone();
        tone::shadows_highlights(&mut a, w, h, p);
        let mut r = ramp(40, 9);
        tone::shadows_highlights(&mut r, 40, 9, p);
        let mut g: Vec<[f32; 4]> = (0..121 * 121).map(|k| if k == 60 * 121 + 60 { [0.25, 0.125, 0.0, 1.0] } else { [0.15, 0.15, 0.15, 1.0] }).collect();
        tone::shadows_highlights(&mut g, 121, 121, p);
        println!("r5_shadows_highlights {} {:016x} {:016x} {:016x}", i, hpx(&a), hpx(&r), hpx(&g));
    }
    // Perspective Crop quads.
    let quads = [
        [[30.0, 12.0], [140.0, 25.0], [128.0, 110.0], [14.0, 95.0]],
        [[0.0, 0.0], [50.0, 0.0], [100.0, 0.0], [0.0, 50.0]],
        [[0.0, 0.0], [50.0, 50.0], [50.0, 0.0], [0.0, 50.0]],
        [[0.0, 0.0], [0.0, 50.0], [50.0, 50.0], [50.0, 0.0]],
        [[0.0, 0.0], [f64::NAN, 0.0], [50.0, 50.0], [0.0, 50.0]],
        [[0.0, 0.0]; 4],
        [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
        [[0.0, 0.0], [1.5, 0.0], [1.5, 1.5], [0.0, 1.5]],
        [[10.0, 10.0], [1e9, 10.0], [1e9, 2e9], [10.0, 2e9]],
        [[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [99.99, 100.0]],
    ];
    let mut hq = H0;
    for q in &quads {
        fnv(&mut hq, if convex_quad(q) { b"c" } else { b"n" });
        let (qw, qh) = quad_size(q);
        hd(&mut hq, &[qw, qh]);
        for (ow, oh) in [(100.0, 80.0), (qw, qh), (0.0, 10.0), (f64::INFINITY, 5.0), (1e-3, 1e-3)] {
            match quad_to_rect(q, ow, oh) {
                Some(m) => {
                    hd(&mut hq, &m.0);
                    for c in q {
                        let p = m.apply(c[0], c[1]);
                        hd(&mut hq, &[p.0, p.1]);
                    }
                }
                None => fnv(&mut hq, b"none"),
            }
        }
    }
    println!("r5_quads {:016x}", hq);
    let mut hi_ = H0;
    for m in [
        Homography::IDENTITY,
        Homography([2.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 2.0]),
        Homography([1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
        Homography([1.0, 0.1, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
        Homography([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1e-13]),
        Homography([-1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0]),
        Homography([1.0 + 1e-10, 0.0, 1e-10, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
        Homography([f64::NAN, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]),
    ] {
        fnv(&mut hi_, if m.is_identity() { b"i" } else { b"n" });
    }
    println!("r5_is_identity {:016x}", hi_);
    // Clipped and horizon-crossing warps.
    let fmts = [
        ("rgba8", PixelFormat::RGBA8),
        ("rgba16", PixelFormat::new(ColorMode::Rgb, SampleType::U16, true)),
        ("rgba32", PixelFormat::RGBA32F),
        ("rgb8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, false)),
        ("gray32", PixelFormat::new(ColorMode::Grayscale, SampleType::F32, false)),
    ];
    let src_r = Rect::new(0, 0, 100, 100);
    let steep = Homography::rect_to_quad([0.0, 0.0, 100.0, 100.0], [[0.0, 0.0], [300.0, 0.0], [155.0, 155.0], [0.0, 300.0]]).unwrap_or(Homography::IDENTITY);
    let horizon = Homography([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.012, 0.0, 0.4]);
    let ms = [
        ("steep", steep),
        ("horizon", horizon),
        ("behind", Homography([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -0.01, -0.01, -0.5])),
        ("cross", Homography([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -0.012, 0.0, 0.6])),
        ("shrink", Homography::rect_to_quad([0.0, 0.0, 100.0, 100.0], [[3.0, 3.0], [20.0, 4.0], [19.0, 18.0], [2.0, 21.0]]).unwrap_or(Homography::IDENTITY)),
        ("translate", Homography([1.0, 0.0, 7.5, 0.0, 1.0, -3.25, 0.0, 0.0, 1.0])),
    ];
    for (name, fmt) in fmts {
        let mut s = surf(fmt, src_r, 9, false);
        if fmt.sample == SampleType::F32 {
            // Signed and HDR samples keep their range through the warp.
            let n = fmt.channels();
            let mut px = vec![2.0f32; n];
            if n > 1 {
                px[1] = -0.25;
            }
            if fmt.alpha {
                px[n - 1] = 0.5;
            }
            s.fill_rect(Rect::new(40, 40, 60, 60), &px);
        }
        for (mn, m) in &ms {
            let mut h = H0;
            for interp in [Interp::Nearest, Interp::Bilinear, Interp::Bicubic] {
                fnv(&mut h, &warp_surface(&s, src_r, m, interp).as_ref().map_or(u64::MAX, hs).to_le_bytes());
                for clip in [Rect::new(0, 0, 300, 300), Rect::new(100, 100, 200, 200), Rect::new(-50, -50, 10, 10), Rect::new(240, 0, 256, 512), Rect::EMPTY] {
                    fnv(&mut h, &warp_surface_clipped(&s, src_r, m, interp, clip).as_ref().map_or(u64::MAX, hs).to_le_bytes());
                }
            }
            println!("r5_warp {} {} {:016x}", name, mn, h);
        }
    }
    // Focus-stack fusion.
    let mut hl = H0;
    for (w, h) in [(64usize, 64usize), (48, 100), (2070, 1378), (8280, 5520), (100_000, 100_000), (0, 5), (1, 1), (63, 200)] {
        hu(&mut hl, &[pyramid::stack_levels(w, h) as u64]);
    }
    println!("r5_stack_levels {:016x}", hl);
    let (w, h) = (45usize, 37usize);
    for ch in [1usize, 3, 4] {
        let imgs: Vec<Vec<f32>> = (0..3).map(|k| buf(w * h * ch, 30 + k)).collect();
        let lumas: Vec<Vec<f32>> = (0..3).map(|k| buf(w * h, 40 + k)).collect();
        let alphas: Vec<Vec<f32>> = (0..3).map(|k| (0..w * h).map(|i| if (i + k as usize * 7) % 9 == 0 { 0.25 } else { 1.0 }).collect()).collect();
        let refs: Vec<&[f32]> = imgs.iter().map(|v| v.as_slice()).collect();
        let d = pyramid::StackFusion::default();
        let cases = [
            d,
            pyramid::StackFusion { halo: 0.0, ..d },
            pyramid::StackFusion { guide: 0, ..d },
            pyramid::StackFusion { energy_radius: 0, ..d },
            pyramid::StackFusion { energy_radius: 3, halo: 100.0, ..d },
            pyramid::StackFusion { levels: Some(1), ..d },
            pyramid::StackFusion { levels: Some(3), halo: 0.0, ..d },
            pyramid::StackFusion { levels: Some(40), guide: 9, ..d },
        ];
        for (i, p) in cases.iter().enumerate() {
            let mut hh = H0;
            for n in [1usize, 2, 3] {
                hf(&mut hh, &pyramid::fuse_stack(w, h, ch, &refs[..n], &lumas[..n], &alphas[..n], *p));
            }
            println!("r5_fuse_stack {} {} {:016x}", ch, i, hh);
        }
    }
}
