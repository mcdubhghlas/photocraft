//! C3 compose layer-effect reference vectors (`rustref compose-fx-vectors`), checked line by line
//! by tests/compose/test_parity_fx.c. Everything is built identically in both languages from an
//! xorshift stream (every rng draw is its own statement, in order) and hashed with FNV-1a over
//! f32 bits (NaN canonical), little-endian. Lines, tab-separated:
//!
//!   unit NAME HASH           sweeps of the public effect helpers (kernels, contours, noise,
//!                            gradients, bevel geometry, outline share, distance fields)
//!   map SEED K=V ...         build_maps on a random shape + random effects (every kind, hostile
//!                            values), bytes, gradient-stroke frames and composite_with_effects
//!                            onto random backdrops (whole region, sub rect, offset backdrop)
//!   doc SEED K=HASH ...      renders of a seeded document whose layers carry effects
//!   q SEED I K=V ...         queries on its top-level layer I (bounds, shape, stroke frames)
//!
//! Documents 1..=300 use C1 + C3 features only; 1001..=1080 add gradient / pattern fills,
//! stroked shapes, masks that hide effects, knockouts and Blend Clipped Layers as Group off (C2).
use photocraft_compose as c;
use photocraft_compose::effects as fx;
use photocraft_doc::adjust::CurvePoint;
use photocraft_doc::effects::{Bevel, BevelContour, BevelStyle, BevelTechnique, BevelTexture, Satin};
use photocraft_doc::vector::{ShapeStroke, StrokeAlign};
use photocraft_doc::{
    Artboard, ArtboardBackground, BlendMode, Color, ColorMode, Contour, Document, Effect, Fill, FxCommon, FxPaint,
    GlobalLight, Glow, GlowSource, GlowTechnique, Gradient, GradientStyle, Layer, LayerContent, LayerMask, Path,
    Pattern, PixelFormat, Rect, SampleType, Shadow, ShapeLayer, Size, StrokeFx, StrokePosition, Subpath, Surface,
    TextLayer, VectorMask,
};

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn hf(h: &mut u64, v: f32) {
    let bits = if v.is_nan() { 0x7fc0_0000 } else { v.to_bits() };
    fnv(h, &bits.to_le_bytes());
}
fn hi(h: &mut u64, v: i32) {
    fnv(h, &v.to_le_bytes());
}
fn hpx(px: &[[f32; 4]]) -> u64 {
    let mut h = H0;
    for p in px {
        for v in p {
            hf(&mut h, *v);
        }
    }
    h
}
fn hbuf(b: &c::Buffer) -> u64 {
    let mut h = hpx(&b.px);
    for v in [b.rect.x0, b.rect.y0, b.rect.x1, b.rect.y1] {
        hi(&mut h, v);
    }
    h
}
fn hvec(v: &[f32]) -> u64 {
    let mut h = H0;
    for x in v {
        hf(&mut h, *x);
    }
    h
}
fn rs(r: Rect) -> String {
    format!("{},{},{},{}", r.x0, r.y0, r.x1, r.y1)
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.0 = x;
        x
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
    fn unit(&mut self) -> f32 {
        (self.next() >> 40) as f32 / 16777216.0
    }
    fn range(&mut self, lo: i32, hi: i32) -> i32 {
        lo + self.below((hi - lo) as u64) as i32
    }
}

const MODES: [ColorMode; 6] =
    [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab, ColorMode::Multichannel, ColorMode::Duotone];
const DEPTHS: [SampleType; 3] = [SampleType::U8, SampleType::U16, SampleType::F32];
const STYLES: [GradientStyle; 5] =
    [GradientStyle::Linear, GradientStyle::Radial, GradientStyle::Angle, GradientStyle::Reflected, GradientStyle::Diamond];
const BEVEL_STYLES: [BevelStyle; 5] =
    [BevelStyle::OuterBevel, BevelStyle::InnerBevel, BevelStyle::Emboss, BevelStyle::PillowEmboss, BevelStyle::StrokeEmboss];
const TECHNIQUES: [BevelTechnique; 3] = [BevelTechnique::Smooth, BevelTechnique::ChiselHard, BevelTechnique::ChiselSoft];
const POSITIONS: [StrokePosition; 3] = [StrokePosition::Outside, StrokePosition::Inside, StrokePosition::Center];

// ---------- effect generator ----------

fn color(r: &mut Rng) -> Color {
    match r.below(5) {
        0 | 1 => {
            let a = r.unit();
            let b = r.unit();
            let cc = r.unit();
            Color::rgb(a, b, cc)
        }
        2 => Color::gray(r.unit()),
        3 => {
            let a = r.unit();
            let b = r.unit();
            let cc = r.unit();
            let d = r.unit();
            Color { mode: ColorMode::Cmyk, c: [a, b, cc, d], alpha: 1.0 }
        }
        _ => {
            let a = r.unit();
            let b = r.unit();
            let cc = r.unit();
            Color { mode: ColorMode::Lab, c: [a, b, cc, 0.0], alpha: 1.0 }
        }
    }
}

fn blend(r: &mut Rng) -> BlendMode {
    if r.below(3) == 0 { BlendMode::Normal } else { BlendMode::LAYER_MODES[r.below(27) as usize] }
}

fn fx_opacity(r: &mut Rng) -> f32 {
    match r.below(7) {
        0 => 1.0,
        1 => 0.75,
        2 => 0.5,
        3 => r.unit(),
        4 => 0.0,
        5 => 1.5,
        _ => -0.2,
    }
}

fn common(r: &mut Rng) -> FxCommon {
    let b = blend(r);
    let o = fx_opacity(r);
    let mut cm = FxCommon::new(b, o);
    if r.below(12) == 0 {
        cm.enabled = false;
    }
    cm
}

fn size(r: &mut Rng) -> f32 {
    match r.below(12) {
        0 => 0.0,
        1 => 1.0,
        2 => 2.0,
        3 => 3.5,
        4 => 5.0,
        5 => 8.0,
        6 => r.unit() * 12.0,
        7 => 16.0,
        8 => -3.0,
        9 => [f32::NAN, f32::INFINITY, 1e30, 600.0][r.below(4) as usize],
        _ => 4.0,
    }
}

fn angle(r: &mut Rng) -> f32 {
    match r.below(8) {
        0 => 0.0,
        1 => 90.0,
        2 => 120.0,
        3 => -45.0,
        4 => r.unit() * 360.0,
        5 => 200.0,
        6 => [f32::NAN, f32::INFINITY, 1e20][r.below(3) as usize],
        _ => 30.0,
    }
}

fn frac(r: &mut Rng) -> f32 {
    match r.below(6) {
        0 => 0.0,
        1 => 1.0,
        2 => 0.5,
        3 => r.unit(),
        4 => [f32::NAN, -0.5, 2.0][r.below(3) as usize],
        _ => 0.25,
    }
}

fn pt(input: f32, output: f32) -> CurvePoint {
    CurvePoint { input, output }
}

fn contour(r: &mut Rng) -> Contour {
    let points = match r.below(7) {
        0..=2 => return Contour::Linear,
        3 => vec![pt(0.0, 0.0), pt(0.5, 1.0), pt(1.0, 0.0)],
        4 => {
            let k = 1 + r.below(5);
            let mut v = Vec::new();
            for _ in 0..k {
                let a = r.unit();
                let b = r.unit();
                v.push(pt(a, b));
            }
            v
        }
        5 => vec![pt(0.0, 0.0), pt(0.1, 1.0), pt(0.2, 0.0), pt(0.2, 0.5), pt(1.0, 1.0)],
        _ => match r.below(4) {
            0 => Vec::new(),
            1 => vec![pt(0.3, 0.8)],
            2 => vec![pt(f32::NAN, 0.5), pt(0.0, 1.0), pt(1.0, 0.0)],
            _ => vec![pt(1.0, 1.0), pt(0.0, 0.0), pt(0.5, 2.0), pt(0.25, -1.0)],
        },
    };
    Contour::Custom { name: "c".into(), points, corners: Vec::new() }
}

fn gradient(r: &mut Rng) -> Gradient {
    let n = r.below(5);
    let mut stops = Vec::new();
    for _ in 0..n {
        let pos = match r.below(5) {
            0 => 0.0,
            1 => 1.0,
            2 => [f32::NAN, 0.5][r.below(2) as usize],
            _ => r.unit(),
        };
        let cc = color(r);
        stops.push((pos, cc));
    }
    let m = r.below(3);
    let mut opacity_stops = Vec::new();
    for _ in 0..m {
        let pos = r.unit();
        let o = frac(r);
        opacity_stops.push((pos, o));
    }
    let style = STYLES[r.below(5) as usize];
    let a = angle(r);
    let scale = match r.below(5) {
        0 => 1.0,
        1 => 0.5,
        2 => 1.5,
        3 => r.unit() * 2.0,
        _ => [0.0, f32::NAN][r.below(2) as usize],
    };
    let reverse = r.below(2) == 0;
    let align = r.below(2) == 0;
    let offset = if r.below(3) == 0 {
        let ox = r.unit() - 0.5;
        let oy = r.unit() - 0.5;
        (ox, oy)
    } else {
        (0.0, 0.0)
    };
    Gradient { stops, opacity_stops, style, angle: a, scale, reverse, align, offset }
}

fn pattern_ref(r: &mut Rng, pats: &[Pattern]) -> (String, String) {
    let k = r.below(4);
    if pats.is_empty() || k == 3 {
        return ("missing".into(), "nope".into());
    }
    let p = &pats[r.below(pats.len() as u64) as usize];
    match k {
        0 => (p.name.clone(), p.id.clone()),
        1 => (String::new(), p.id.clone()),
        _ => (p.name.clone(), String::new()),
    }
}

fn paint(r: &mut Rng, pats: &[Pattern]) -> FxPaint {
    match r.below(4) {
        0 | 1 => FxPaint::Color(color(r)),
        2 => FxPaint::Gradient(gradient(r)),
        _ => {
            let (name, id) = pattern_ref(r, pats);
            let scale = match r.below(4) {
                0 => 1.0,
                1 => 0.5,
                2 => 2.0,
                _ => [0.0, f32::NAN][r.below(2) as usize],
            };
            FxPaint::Pattern { name, id, scale }
        }
    }
}

fn fx_noise(r: &mut Rng) -> f32 {
    match r.below(5) {
        0 => r.unit(),
        1 => [f32::NAN, 2.0][r.below(2) as usize],
        _ => 0.0,
    }
}

fn phase(r: &mut Rng) -> (f32, f32) {
    if r.below(3) == 0 {
        let a = r.unit() * 8.0;
        let b = r.unit() * 8.0;
        (a, b)
    } else {
        (0.0, 0.0)
    }
}

fn shadow(r: &mut Rng) -> Shadow {
    let common = common(r);
    let color = color(r);
    let angle = angle(r);
    let use_global_light = r.below(2) == 0;
    let distance = size(r);
    let spread = frac(r);
    let size = size(r);
    let contour = contour(r);
    let noise = fx_noise(r);
    let knocks_out = r.below(3) != 0;
    Shadow { common, color, angle, use_global_light, distance, spread, size, contour, anti_alias: false, noise, knocks_out }
}

fn glow(r: &mut Rng, pats: &[Pattern]) -> Glow {
    let common = common(r);
    let paint = paint(r, pats);
    let technique = if r.below(2) == 0 { GlowTechnique::Softer } else { GlowTechnique::Precise };
    let spread = frac(r);
    let size = size(r);
    let contour = contour(r);
    let range = match r.below(5) {
        0 => 0.5,
        1 => 1.0,
        2 => r.unit(),
        3 => [0.0, f32::NAN, -1.0][r.below(3) as usize],
        _ => 0.25,
    };
    let noise = fx_noise(r);
    let source = if r.below(2) == 0 { GlowSource::Edge } else { GlowSource::Center };
    Glow { common, paint, technique, spread, size, contour, anti_alias: false, range, jitter: 0.0, noise, source }
}

fn bevel(r: &mut Rng, pats: &[Pattern]) -> Bevel {
    let enabled = r.below(12) != 0;
    let style = BEVEL_STYLES[r.below(5) as usize];
    let technique = TECHNIQUES[r.below(3) as usize];
    let depth = match r.below(5) {
        0 => 1.0,
        1 => 2.5,
        2 => r.unit() * 10.0,
        3 => [0.0, f32::NAN, -1.0][r.below(3) as usize],
        _ => 0.5,
    };
    let up = r.below(3) != 0;
    let size = size(r);
    let soften = match r.below(4) {
        0 => 0.0,
        1 => 2.0,
        2 => r.unit() * 6.0,
        _ => [f32::NAN, 1e30][r.below(2) as usize],
    };
    let angle = angle(r);
    let altitude = match r.below(4) {
        0 => 30.0,
        1 => r.unit() * 90.0,
        2 => 0.0,
        _ => [90.0, f32::NAN][r.below(2) as usize],
    };
    let use_global_light = r.below(2) == 0;
    let gloss_contour = contour(r);
    let highlight = common(r);
    let highlight_color = color(r);
    let shadow = common(r);
    let shadow_color = color(r);
    let bc = if r.below(3) == 0 {
        let cc = contour(r);
        let range = frac(r);
        Some(BevelContour { contour: cc, range, anti_alias: false })
    } else {
        None
    };
    let texture = if r.below(3) == 0 {
        let (name, id) = pattern_ref(r, pats);
        let scale = match r.below(3) {
            0 => 1.0,
            1 => 0.5,
            _ => 2.0,
        };
        let depth = match r.below(3) {
            0 => 1.0,
            1 => -0.5,
            _ => r.unit() * 2.0,
        };
        let invert = r.below(2) == 0;
        let link = r.below(2) == 0;
        let phase = phase(r);
        Some(BevelTexture { name, id, scale, depth, invert, link, phase })
    } else {
        None
    };
    Bevel {
        enabled,
        style,
        technique,
        depth,
        up,
        size,
        soften,
        angle,
        altitude,
        use_global_light,
        gloss_contour,
        highlight,
        highlight_color,
        shadow,
        shadow_color,
        contour: bc,
        texture,
    }
}

fn effect(r: &mut Rng, pats: &[Pattern]) -> Effect {
    match r.below(10) {
        0 => Effect::DropShadow(shadow(r)),
        1 => Effect::InnerShadow(shadow(r)),
        2 => Effect::OuterGlow(glow(r, pats)),
        3 => Effect::InnerGlow(glow(r, pats)),
        4 => {
            let common = common(r);
            let size = size(r);
            let position = POSITIONS[r.below(3) as usize];
            let paint = paint(r, pats);
            Effect::Stroke(StrokeFx { common, size, position, paint })
        }
        5 => {
            let common = common(r);
            let color = color(r);
            Effect::ColorOverlay { common, color }
        }
        6 => {
            let common = common(r);
            let gradient = gradient(r);
            Effect::GradientOverlay { common, gradient, dither: false }
        }
        7 => {
            let common = common(r);
            let (name, id) = pattern_ref(r, pats);
            let scale = match r.below(4) {
                0 => 1.0,
                1 => 0.5,
                2 => 2.0,
                _ => [0.0, f32::NAN][r.below(2) as usize],
            };
            let angle = angle(r);
            let link = r.below(4) != 0;
            let phase = phase(r);
            Effect::PatternOverlay { common, name, id, scale, angle, link, phase }
        }
        8 => {
            let common = common(r);
            let color = color(r);
            let angle = angle(r);
            let distance = size(r);
            let size = size(r);
            let contour = contour(r);
            let invert = r.below(2) == 0;
            Effect::Satin(Satin { common, color, angle, distance, size, contour, anti_alias: true, invert })
        }
        _ => Effect::BevelEmboss(bevel(r, pats)),
    }
}

fn effects(r: &mut Rng, pats: &[Pattern]) -> Vec<Effect> {
    let n = 1 + r.below(4);
    let mut v = Vec::new();
    for _ in 0..n {
        v.push(effect(r, pats));
    }
    v
}

fn patterns(r: &mut Rng) -> Vec<Pattern> {
    let n = r.below(4);
    let mut v = Vec::new();
    for i in 0..n {
        let w = 1 + r.below(5) as u32;
        let h = 1 + r.below(5) as u32;
        let fmt = match r.below(5) {
            0 | 1 => PixelFormat::RGBA8,
            2 => PixelFormat::RGBA16,
            3 => PixelFormat::new(ColorMode::Grayscale, SampleType::U8, true),
            _ => PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true),
        };
        let mut s = Surface::new(fmt);
        let ch = fmt.channels();
        for y in 0..h as i32 {
            for x in 0..w as i32 {
                let mut px = Vec::new();
                for k in 0..ch {
                    let u = r.unit();
                    // Alpha mostly opaque.
                    px.push(if k + 1 == ch && u > 0.3 { 1.0 } else { u });
                }
                s.write_pixel(x, y, &px);
            }
        }
        v.push(Pattern::new(format!("p{i}"), s, w, h));
    }
    v
}

fn light(r: &mut Rng) -> GlobalLight {
    let a = angle(r);
    let alt = match r.below(4) {
        0 => 30.0,
        1 => r.unit() * 90.0,
        2 => 60.0,
        _ => [0.0, 90.0, f32::NAN][r.below(3) as usize],
    };
    GlobalLight { angle: a, altitude: alt }
}

// ---------- unit sweeps ----------

fn unit_sweeps() {
    let widths = [0.0f32, 0.5, 1.0, 1.5, 2.0, 3.5, 4.0, 5.0, 8.25, 41.0, 511.7, 512.0, 1e30, f32::INFINITY, -f32::INFINITY, f32::NAN, -3.0];
    let mut h = H0;
    for w in widths {
        let (rad, k) = fx::tent_kernel(w);
        hi(&mut h, rad);
        hi(&mut h, k.len() as i32);
        for v in k {
            hf(&mut h, v);
        }
    }
    println!("unit\ttent_kernel\t{h:016x}");

    let mut r = Rng(0x1234_5678);
    let mut h = H0;
    let ranges = [1.0f32, 0.5, 0.01, 0.0, -1.0, f32::NAN, f32::INFINITY, 0.999, 2.0, 0.3];
    for _ in 0..40 {
        let cc = contour(&mut r);
        for range in ranges {
            match fx::ranged_lut(&cc, range) {
                None => hi(&mut h, -1),
                Some(l) => {
                    hi(&mut h, l.len() as i32);
                    for v in l {
                        hf(&mut h, v);
                    }
                }
            }
        }
    }
    println!("unit\tranged_lut\t{h:016x}");

    let mut h = H0;
    for y in [-3i64, -1, 0, 1, 2, 77, 1 << 31, -(1 << 40), i64::MAX, i64::MIN] {
        for x in [-2i64, 0, 1, 5, 1000, 1 << 33, i64::MIN] {
            hf(&mut h, fx::hash_noise(x, y));
        }
    }
    println!("unit\thash_noise\t{h:016x}");

    let mut r = Rng(0x9abc_def1);
    let mut h = H0;
    let ts = [0.0f32, 1.0, 0.5, 0.25, -1.0, 2.0, f32::NAN, 0.999, 1e-7];
    for _ in 0..60 {
        let g = gradient(&mut r);
        for t in ts {
            for v in fx::sample_gradient(&g, t) {
                hf(&mut h, v);
            }
        }
    }
    println!("unit\tsample_gradient\t{h:016x}");

    let vals = [0.0f32, 1.0, 0.5, 2.5, -1.0, 4.0, 16.0, 1e30, f32::INFINITY, f32::NAN, 0.25, 0.75];
    let mut h = H0;
    for &a in &vals {
        for &b in &vals {
            let (x, y) = fx::spread_split(a, b);
            hf(&mut h, x);
            hf(&mut h, y);
            hf(&mut h, fx::outline_share(a, b));
        }
        hf(&mut h, fx::glow_gradient_gain(a));
    }
    println!("unit\tsplit_share_gain\t{h:016x}");

    let mut h = H0;
    let mut r = Rng(0x7777);
    for _ in 0..200 {
        let b = bevel(&mut r, &[]);
        let g = fx::bevel_geom(&b);
        hi(&mut h, g.paint as i32);
        hf(&mut h, g.width);
        hf(&mut h, g.depth);
        hi(&mut h, g.pillow as i32);
        hf(&mut h, g.chisel_soft);
    }
    // R0: upstream bevel_chisel_h panics for a NaN size on some paths (f32::clamp with a NaN
    // bound: Outer hard with dout > 0); such inputs hash the bytes "panic" instead of a value.
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    for p in [fx::BevelPaint::Inner, fx::BevelPaint::Outer, fx::BevelPaint::Both] {
        for &s in &vals {
            for &din in &vals {
                for &dout in &[0.0f32, 1.0, 3.0, -0.5, f32::NAN] {
                    match std::panic::catch_unwind(|| fx::bevel_chisel_h(p, s, din, dout, false)) {
                        Ok(v) => hf(&mut h, v),
                        Err(_) => fnv(&mut h, b"panic"),
                    }
                }
            }
        }
    }
    std::panic::set_hook(prev_hook);
    println!("unit\tbevel_geom\t{h:016x}");

    let mut h = H0;
    for &s in &vals {
        for p in POSITIONS {
            let st = StrokeFx { common: FxCommon::new(BlendMode::Normal, 1.0), size: s, position: p, paint: FxPaint::Color(Color::BLACK) };
            let (a, b) = fx::stroke_widths(&st);
            hf(&mut h, a);
            hf(&mut h, b);
        }
    }
    println!("unit\tstroke_widths\t{h:016x}");

    let mut r = Rng(0x5151);
    let kinds = [
        fx::FieldKind::Outside,
        fx::FieldKind::Inside,
        fx::FieldKind::StrokeOutside,
        fx::FieldKind::ChokeInside,
        fx::FieldKind::StrokeInside,
        fx::FieldKind::StrokeOutsideVector,
    ];
    for (ki, k) in kinds.iter().enumerate() {
        let mut h = H0;
        let mut rr = Rng(0x5151 + ki as u64);
        for _ in 0..12 {
            let (w, hh) = match rr.below(6) {
                0 => (0usize, 5usize),
                1 => (1, 1),
                _ => (1 + rr.below(30) as usize, 1 + rr.below(20) as usize),
            };
            let a = shape(&mut rr, w, hh);
            for v in fx::distance_field(*k, a, w, hh, f32::INFINITY) {
                hf(&mut h, v);
            }
        }
        println!("unit\tdistance_field_{ki}\t{h:016x}");
    }
    let _ = r.next();
}

/// A random effect shape: transparent, then 1–3 rectangles (opaque or partly), some soft edges.
fn shape(r: &mut Rng, w: usize, h: usize) -> Vec<f32> {
    let mut v = vec![0.0f32; w * h];
    if w == 0 || h == 0 {
        return v;
    }
    let n = 1 + r.below(3);
    for _ in 0..n {
        let x0 = r.below(w as u64) as usize;
        let y0 = r.below(h as u64) as usize;
        let x1 = x0 + 1 + r.below((w - x0) as u64) as usize;
        let y1 = y0 + 1 + r.below((h - y0) as u64) as usize;
        let a = if r.below(3) == 0 { r.unit() } else { 1.0 };
        for y in y0..y1 {
            for x in x0..x1 {
                v[y * w + x] = a;
            }
        }
    }
    let k = r.below(6);
    for _ in 0..k {
        let i = r.below((w * h) as u64) as usize;
        v[i] = r.unit();
    }
    v
}

// ---------- maps ----------

fn layer_kind(r: &mut Rng, w: i32, h: i32) -> Layer {
    match r.below(5) {
        0 | 1 => Layer::raster("l", PixelFormat::RGBA8),
        2 | 3 => {
            let k = 3 + r.below(2);
            let mut pts = Vec::new();
            for _ in 0..k {
                let x = r.range(-2, w + 2);
                let y = r.range(-2, h + 2);
                pts.push((f64::from(x) + 0.5, f64::from(y) + 0.25));
            }
            let path = Path::new(vec![Subpath::polygon(&pts)]);
            let filled = r.below(4) != 0;
            let sh = ShapeLayer { path, fill: if filled { Some(Fill::Solid(Color::rgb(1.0, 0.0, 0.0))) } else { None }, ..Default::default() };
            Layer::new("shape", LayerContent::Shape(sh))
        }
        _ => Layer::new("type", LayerContent::Text(TextLayer::default())),
    }
}

fn opacity(r: &mut Rng) -> f32 {
    match r.below(10) {
        0..=4 => 1.0,
        5 => 0.5,
        6 => 0.25,
        7 => r.unit(),
        8 => 0.0,
        _ => [1.5, -0.25][r.below(2) as usize],
    }
}

fn map_line(seed: u64) {
    let mut r = Rng(seed.wrapping_mul(0x2545_f491_4f6c_dd1d) | 1);
    let w = 1 + r.below(40) as i32;
    let h = 1 + r.below(30) as i32;
    let x0 = r.range(-20, 20);
    let y0 = r.range(-20, 20);
    let rect = Rect::new(x0, y0, x0 + w, y0 + h);
    let shp = shape(&mut r, w as usize, h as usize);
    let pats = patterns(&mut r);
    let mut layer = layer_kind(&mut r, w, h);
    layer.effects.items = effects(&mut r, &pats);
    if r.below(4) == 0 {
        let a = r.unit() * w as f32;
        let b = r.unit() * h as f32;
        layer.effects.reference = Some((f64::from(x0) + f64::from(a), f64::from(y0) + f64::from(b)));
    }
    layer.opacity = opacity(&mut r);
    layer.fill_opacity = opacity(&mut r);
    if r.below(2) == 0 {
        layer.blend = BlendMode::LAYER_MODES[r.below(27) as usize];
    }
    if r.below(5) == 0 {
        layer.advanced.transparency_shapes = false;
    }
    if r.below(4) == 0 {
        layer.advanced.blend_interior = true;
    }
    let lt = light(&mut r);
    let anchor = if r.below(2) == 0 {
        let a = r.unit() * 10.0;
        let b = r.unit() * 10.0;
        (f64::from(x0) + f64::from(a), f64::from(y0) + f64::from(b))
    } else {
        (f64::from(x0), f64::from(y0))
    };
    let tex = fx::TextureCtx { rect, patterns: &pats, anchor };
    // R0: upstream build_maps can panic on hostile bevel values (bevel_chisel_h: f32::clamp with a
    // NaN bound); such cases print "map SEED panic" and stop there.
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let built = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| fx::build_maps(&layer, shp.clone(), rect, &lt, &tex)));
    std::panic::set_hook(prev_hook);
    let Ok(maps) = built else {
        println!("map\t{seed}\tpanic");
        return;
    };
    let mut out = format!("map\t{seed}\tbytes={}", maps.bytes());
    let mut hfr = H0;
    for e in &layer.effects.items {
        if let Effect::Stroke(st) = e {
            match maps.stroke_frame(st) {
                Some(f) => {
                    for v in [f.x0, f.y0, f.x1, f.y1] {
                        hi(&mut hfr, v);
                    }
                }
                None => hi(&mut hfr, -7),
            }
        }
    }
    out += &format!("\tframes={hfr:016x}");
    // Content: random colours with the shape's alpha (sometimes fainter).
    let faint = r.below(3) == 0;
    let mut content = c::Buffer::transparent(rect);
    for i in 0..content.px.len() {
        let a = r.unit();
        let b = r.unit();
        let cc = r.unit();
        let k = if faint { r.unit() } else { 1.0 };
        content.px[i] = [a, b, cc, shp[i] * k];
    }
    let lb = if r.below(2) == 0 {
        rect
    } else {
        let lx = r.range(x0, x0 + w);
        let ly = r.range(y0, y0 + h);
        let lw = r.range(1, w + 1);
        let lh = r.range(1, h + 1);
        Rect::new(lx, ly, lx + lw, ly + lh)
    };
    let bg = [r.unit(), 0.0, 0.0, 0.0];
    let bg = {
        let g = r.unit();
        let b = r.unit();
        let a = if r.below(3) == 0 { 0.0 } else { r.unit() };
        [bg[0], g, b, a]
    };
    let mut back = c::Buffer::filled(rect, bg);
    fx::composite_with_effects(&layer, &content, &mut back, &maps, lb, &pats);
    out += &format!("\tfull={:016x}", hbuf(&back));
    // A sub rect of the region (content cropped), the backdrop over the same rect.
    let sx = r.range(x0 - 3, x0 + w);
    let sy = r.range(y0 - 3, y0 + h);
    let sw = r.range(1, w + 4);
    let sh = r.range(1, h + 4);
    let sub = Rect::new(sx, sy, sx + sw, sy + sh);
    let mut csub = c::Buffer::transparent(sub);
    for y in sub.y0..sub.y1 {
        for x in sub.x0..sub.x1 {
            if x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1 {
                let i = ((y - sub.y0) * sw + (x - sub.x0)) as usize;
                csub.px[i] = content.get(x, y);
            }
        }
    }
    let mut bsub = c::Buffer::filled(sub, bg);
    fx::composite_with_effects(&layer, &csub, &mut bsub, &maps, lb, &pats);
    out += &format!("\tsub={:016x}", hbuf(&bsub));
    // A backdrop offset from the content's rect.
    let ox = r.range(-4, 5);
    let oy = r.range(-4, 5);
    let brect = Rect::new(sub.x0 + ox, sub.y0 + oy, sub.x1 + ox + 1, sub.y1 + oy);
    let mut boff = c::Buffer::filled(brect, bg);
    fx::composite_with_effects(&layer, &csub, &mut boff, &maps, lb, &pats);
    out += &format!("\toff={:016x}", hbuf(&boff));
    println!("{out}");
}

// ---------- documents ----------

fn layer_format(r: &mut Rng, doc_fmt: PixelFormat) -> PixelFormat {
    match r.below(6) {
        0..=2 => doc_fmt,
        3 => PixelFormat::RGBA8,
        4 => PixelFormat::RGBA16,
        _ => PixelFormat::RGBA32F,
    }
}

fn pixel(r: &mut Rng, fmt: PixelFormat) -> Vec<f32> {
    let n = fmt.channels();
    let mut v = Vec::new();
    for k in 0..n {
        let u = r.unit();
        v.push(if k + 1 == n && fmt.alpha && u > 0.4 { 1.0 } else { u });
    }
    v
}

fn paint_surface(r: &mut Rng, s: &mut Surface, w: i32, h: i32) {
    let fmt = s.format();
    let n = 1 + r.below(3);
    for _ in 0..n {
        let x0 = r.range(-3, w);
        let y0 = r.range(-3, h);
        let x1 = x0 + r.range(1, w / 2 + 3);
        let y1 = y0 + r.range(1, h / 2 + 3);
        let px = pixel(r, fmt);
        s.fill_rect(Rect::new(x0, y0, x1, y1), &px);
    }
    let k = r.below(4);
    for _ in 0..k {
        let x = r.range(0, w);
        let y = r.range(0, h);
        let px = pixel(r, fmt);
        s.write_pixel(x, y, &px);
    }
}

fn polygon(r: &mut Rng, w: i32, h: i32, k: u64) -> Path {
    let mut pts = Vec::new();
    for _ in 0..k {
        let x = r.range(-2, w + 2);
        let q = r.below(4);
        let y = r.range(-2, h + 2);
        pts.push((f64::from(x) + 0.25 * q as f64, f64::from(y) + 0.5));
    }
    Path::new(vec![Subpath::polygon(&pts)])
}

struct Gen<'a> {
    fmt: PixelFormat,
    w: i32,
    h: i32,
    pats: &'a [Pattern],
    c2: bool,
}

fn gen_layers(r: &mut Rng, g: &Gen, level: u32, n: u64) -> Vec<Layer> {
    let mut v = Vec::new();
    for i in 0..n {
        let mut l = gen_layer(r, g, level);
        if i > 0 && r.below(4) == 0 {
            l.clipped = true;
        }
        v.push(l);
    }
    v
}

fn gen_layer(r: &mut Rng, g: &Gen, level: u32) -> Layer {
    let (w, h) = (g.w, g.h);
    let kind = r.below(10);
    let mut l = match kind {
        6 if level < 2 => {
            let n = 1 + r.below(3);
            let kids = gen_layers(r, g, level + 1, n);
            let mut gl = Layer::group("g", kids);
            if r.below(2) == 0 {
                gl.blend = BlendMode::Normal;
            }
            if r.below(6) == 0 {
                let LayerContent::Group(gr) = &mut gl.content else { unreachable!() };
                let x0 = r.range(-2, w);
                let y0 = r.range(-2, h);
                let x1 = x0 + r.range(1, w + 2);
                let y1 = y0 + r.range(1, h + 2);
                let mut ab = Artboard::new(Rect::new(x0, y0, x1, y1));
                ab.background = if r.below(2) == 0 { ArtboardBackground::White } else { ArtboardBackground::Transparent };
                gr.artboard = Some(ab);
            }
            gl
        }
        7 => {
            if g.c2 && r.below(2) == 0 {
                let f = if r.below(2) == 0 {
                    let gr = gradient(r);
                    Fill::gradient(gr.stops, gr.angle, if gr.scale.is_finite() { gr.scale } else { 1.0 }, gr.style, gr.reverse)
                } else {
                    let (name, id) = pattern_ref(r, g.pats);
                    let scale = [1.0f32, 0.5, 2.0][r.below(3) as usize];
                    let angle = [0.0f32, 30.0][r.below(2) as usize];
                    Fill::Pattern { name, id, scale, angle, link: true, phase: (0.0, 0.0) }
                };
                Layer::new("fill", LayerContent::Fill(f))
            } else {
                let cc = color(r);
                Layer::new("fill", LayerContent::Fill(Fill::Solid(cc)))
            }
        }
        8 => {
            let mut s = Surface::new(PixelFormat::RGBA8);
            paint_surface(r, &mut s, w, h);
            let mut t = TextLayer::default();
            t.cache = Some(s);
            Layer::new("type", LayerContent::Text(t))
        }
        9 => {
            let k = 3 + r.below(2);
            let path = polygon(r, w, h, k);
            let filled = r.below(4) != 0;
            let cc = color(r);
            let mut sh = ShapeLayer { path, fill: if filled { Some(Fill::Solid(cc)) } else { None }, ..Default::default() };
            if g.c2 && r.below(2) == 0 {
                let width = [1.0f32, 3.0, 5.0][r.below(3) as usize];
                let align = [StrokeAlign::Center, StrokeAlign::Inside, StrokeAlign::Outside][r.below(3) as usize];
                let sc = color(r);
                sh.stroke = Some(ShapeStroke { width, align, paint: Fill::Solid(sc), ..Default::default() });
            }
            sh.cache = Some(photocraft_vector::render_shape(&sh, PixelFormat::RGBA8, Rect::new(0, 0, w, h)));
            Layer::new("shape", LayerContent::Shape(sh))
        }
        _ => {
            let fmt = layer_format(r, g.fmt);
            let mut l = Layer::raster("px", fmt);
            if let Some(s) = l.surface_mut() {
                paint_surface(r, s, w, h);
            }
            l
        }
    };
    if r.below(10) == 0 {
        l.visible = false;
    }
    if r.below(3) == 0 {
        l.blend = BlendMode::LAYER_MODES[r.below(27) as usize];
    }
    l.opacity = opacity(r);
    l.fill_opacity = opacity(r);
    if r.below(5) != 0 {
        l.effects.items = effects(r, g.pats);
        if r.below(10) == 0 {
            l.effects.enabled = false;
        }
        if r.below(4) == 0 {
            let a = r.unit() * w as f32;
            let b = r.unit() * h as f32;
            l.effects.reference = Some((f64::from(a), f64::from(b)));
        }
    }
    if r.below(5) == 0 {
        let mut m = if r.below(2) == 0 { LayerMask::reveal_all() } else { LayerMask::hide_all() };
        let x0 = r.range(-2, w);
        let y0 = r.range(-2, h);
        let v = r.unit();
        let x1 = x0 + r.range(1, w);
        let y1 = y0 + r.range(1, h);
        m.surface.fill_rect(Rect::new(x0, y0, x1, y1), &[v]);
        m.feather = [0.0f32, 0.0, 2.0][r.below(3) as usize];
        l.mask = Some(m);
    }
    if r.below(7) == 0 {
        let k = 3 + r.below(3);
        let p = polygon(r, w, h, k);
        l.vector_mask = Some(VectorMask::new(p));
    }
    if r.below(6) == 0 {
        l.advanced.transparency_shapes = false;
    }
    if r.below(5) == 0 {
        l.advanced.blend_interior = true;
    }
    if g.c2 {
        if r.below(4) == 0 {
            l.advanced.layer_mask_hides_effects = true;
        }
        if r.below(4) == 0 {
            l.advanced.vector_mask_hides_effects = true;
        }
        if r.below(6) == 0 {
            l.advanced.knockout = [photocraft_doc::Knockout::Shallow, photocraft_doc::Knockout::Deep][r.below(2) as usize];
        }
        if r.below(5) == 0 {
            l.advanced.blend_clipped = false;
        }
    }
    l
}

fn gen_doc(seed: u64) -> Document {
    let mut r = Rng(seed.wrapping_mul(0x9e3779b97f4a7c15) | 1);
    let mode = MODES[r.below(6) as usize];
    let depth = DEPTHS[r.below(3) as usize];
    let w = 6 + r.below(36) as u32;
    let h = 5 + r.below(28) as u32;
    let mut doc = Document::new("fx", Size::new(w, h), mode, depth);
    doc.patterns = patterns(&mut r);
    doc.global_light = light(&mut r);
    let g = Gen { fmt: doc.pixel_format(), w: w as i32, h: h as i32, pats: &doc.patterns.clone(), c2: seed > 1000 };
    let n = 1 + r.below(4);
    doc.layers = gen_layers(&mut r, &g, 0, n);
    doc
}

fn doc_line(seed: u64) {
    let doc = gen_doc(seed);
    let mut r = Rng(seed ^ 0x0bad_cafe);
    let b = doc.bounds();
    let (w, h) = (b.width() as i32, b.height() as i32);
    let mut out = format!("doc\t{seed}");
    out += &format!("\tflat={:016x}", hbuf(&c::flatten(&doc)));
    out += &format!("\ttiled={:016x}", hbuf(&c::render_tiled(&doc, b, 7)));
    let x0 = r.range(-6, w);
    let y0 = r.range(-6, h);
    let x1 = x0 + r.range(1, w + 8);
    let y1 = y0 + r.range(1, h + 8);
    out += &format!("\tsub={:016x}", hbuf(&c::render_tiled(&doc, Rect::new(x0, y0, x1, y1), 5)));
    let mut hl = H0;
    for l in &doc.layers {
        fnv(&mut hl, &hbuf(&c::render_layer(l, b)).to_le_bytes());
    }
    out += &format!("\tlayers={hl:016x}");
    println!("{out}");
    for (i, l) in doc.layers.iter().enumerate() {
        let mut q = format!("q\t{seed}\t{i}");
        q += &format!("\tbounds={}", rs(c::layer_bounds(l, b)));
        q += &format!("\tcomp={}", c::composite_bounds(l, b).map_or("none".into(), rs));
        q += &format!("\tchange={}", c::change_bounds(l, b).map_or("none".into(), rs));
        q += &format!("\tmargin={}", fx::margin(l));
        let shape = c::layer_shape(&doc, l, b);
        q += &format!("\tshape={:016x}", hvec(&shape));
        let mut hs = H0;
        for e in &l.effects.items {
            if let Effect::Stroke(st) = e {
                match c::stroke_frame(&doc, l, st) {
                    Some(f) => {
                        for v in [f.x0, f.y0, f.x1, f.y1] {
                            hi(&mut hs, v);
                        }
                    }
                    None => hi(&mut hs, -7),
                }
            }
        }
        q += &format!("\tsframes={hs:016x}");
        println!("{q}");
    }
}

/// R0: upstream compose can panic on hostile bevel values (bevel_chisel_h: f32::clamp with a NaN
/// bound); such documents print "doc SEED panic" (doc_line prints only at its end).
fn doc_line_guarded(seed: u64) {
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    let r = std::panic::catch_unwind(|| doc_line(seed));
    std::panic::set_hook(prev_hook);
    if r.is_err() {
        println!("doc\t{seed}\tpanic");
    }
}

pub fn run() {
    unit_sweeps();
    for seed in 1..=500u64 {
        map_line(seed);
    }
    for seed in 1..=300u64 {
        doc_line_guarded(seed);
    }
    for seed in 1001..=1080u64 {
        doc_line_guarded(seed);
    }
    r9_sweeps();
}

/// R9 (resync): functionality added upstream after C3, appended so earlier lines keep their
/// positions: Chisel Soft heights, the bevel distance fields (edges placed by coverage, with a
/// reach), contours with corner points, the satin and bevel contour tables.
fn r9_sweeps() {
    let vals = [0.0f32, 1.0, 0.5, 2.5, -1.0, 4.0, 16.0, 1e30, f32::INFINITY, f32::NAN, 0.25, 0.75, -1.5];
    let mut h = H0;
    let prev_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(|_| {}));
    for p in [fx::BevelPaint::Inner, fx::BevelPaint::Outer, fx::BevelPaint::Both] {
        for &sz in &vals {
            for &din in &vals {
                for &dout in &[0.0f32, 1.0, 3.0, -0.5, f32::NAN] {
                    match std::panic::catch_unwind(|| fx::bevel_chisel_h(p, sz, din, dout, true)) {
                        Ok(v) => hf(&mut h, v),
                        Err(_) => fnv(&mut h, b"panic"),
                    }
                }
            }
        }
    }
    std::panic::set_hook(prev_hook);
    println!("unit\tr9_chisel_soft\t{h:016x}");

    for (ki, kind) in [fx::FieldKind::BevelInside, fx::FieldKind::BevelOutside].into_iter().enumerate() {
        for reach in [0.0f32, 3.0, 12.0, f32::INFINITY] {
            let mut rr = Rng(0x7a7a + ki as u64);
            let mut h = H0;
            for _ in 0..10 {
                let w = 4 + rr.below(60) as usize;
                let hh = 4 + rr.below(50) as usize;
                let a = aa_shape(&mut rr, w, hh);
                for v in fx::distance_field(kind, a, w, hh, reach) {
                    hf(&mut h, v);
                }
            }
            println!("unit\tr9_field_{ki}_{reach}\t{h:016x}");
        }
    }

    let mut hs = H0;
    let mut hb = H0;
    let mut rr = Rng(0x5a71);
    for _ in 0..200 {
        let mut c = contour(&mut rr);
        if let Contour::Custom { points, corners, .. } = &mut c {
            *corners = (0..points.len() + rr.below(2) as usize).map(|_| rr.below(2) == 0).collect();
        }
        match fx::satin_lut(&c) {
            Some(l) => {
                for v in l {
                    hf(&mut hs, v);
                }
            }
            None => fnv(&mut hs, b"none"),
        }
        let range = rr.unit();
        for v in fx::bevel_contour_lut(&photocraft_doc::BevelContour { contour: c, range, anti_alias: false }) {
            hf(&mut hb, v);
        }
    }
    println!("unit\tr9_satin_lut\t{hs:016x}");
    println!("unit\tr9_bevel_contour_lut\t{hb:016x}");
}

/// An anti-aliased disc or oblique bar (coverage by the distance to its edge, one pixel wide).
fn aa_shape(r: &mut Rng, w: usize, h: usize) -> Vec<f32> {
    let disc = r.below(2) == 0;
    let cx = r.unit() * w as f32;
    let cy = r.unit() * h as f32;
    let size = 2.0 + r.unit() * (w.min(h) as f32 / 2.0);
    let ang = r.unit() * 6.2831855;
    let (sn, cs) = ang.sin_cos();
    let mut v = vec![0.0f32; w * h];
    for y in 0..h {
        for x in 0..w {
            let (px, py) = (x as f32 + 0.5 - cx, y as f32 + 0.5 - cy);
            let d = if disc { (px * px + py * py).sqrt() - size } else { (-px * sn + py * cs).abs() - size / 2.0 };
            v[y * w + x] = (0.5 - d).clamp(0.0, 1.0);
        }
    }
    v
}
