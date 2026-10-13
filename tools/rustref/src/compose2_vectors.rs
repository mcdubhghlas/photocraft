//! C2 compose reference vectors (`rustref compose2-vectors`), checked line by line by
//! tests/compose/test_parity2.c: adjustment layers (all 17 kinds, Levels/Curves in RGB/CMYK/Lab
//! tone spaces, Hue/Saturation ranges, Color Lookup tables, Gradient Map dither), gradient fills
//! (every style, opacity stops, midpoints, dither, align, offsets), pattern fills (scale, angle,
//! phase, link, missing patterns), stroked shapes split into fill and stroke, and Advanced
//! Blending (knockout none/shallow/deep, Blend Clipped Layers as Group off, Transparency Shapes
//! Layer off, the mask-hides-effects switches on layers without effects), in seeded documents of
//! every colour mode and depth with masks, opacity, clipping and pass-through groups, plus
//! function sweeps of adjust.rs, gradient_fill.rs, pattern.rs and shape_split.rs. Documents carry
//! no layer effects (C3). Every random draw is a separate `let`, so the C mirror can follow the
//! order exactly. Hashes are FNV-1a over f32 bits (NaN canonical), little-endian. Lines:
//!
//!   fn NAME HASH             function sweeps
//!   doc SEED K=HASH ...      renders of one document
//!   q SEED I K=V ...         queries on its top-level layer I
use photocraft_compose as c;
use photocraft_compose::adjust::{self, Transfer};
use photocraft_compose::{gradient_fill as gf, pattern, shape_split};
use photocraft_doc::adjust::{CurvePoint, HueRange, LevelsChannel, ToneSpace};
use photocraft_doc::vector::{ShapeStroke, StrokeAlign};
use photocraft_doc::{
    Adjustment, BlendMode, BlendRange, Color, ColorMode, Document, Fill, GradientStyle, Knockout, Layer, LayerContent,
    LayerMask, Path, Pattern, PixelFormat, Rect, SampleType, ShapeLayer, Size, Subpath, Surface, TextLayer, VectorMask,
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
fn hd(h: &mut u64, v: f64) {
    let bits = if v.is_nan() { 0x7ff8_0000_0000_0000 } else { v.to_bits() };
    fnv(h, &bits.to_le_bytes());
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
    hrect(&mut h, b.rect);
    h
}
fn hrect(h: &mut u64, r: Rect) {
    for v in [r.x0, r.y0, r.x1, r.y1] {
        fnv(h, &v.to_le_bytes());
    }
}
fn rs(r: Rect) -> String {
    format!("{},{},{},{}", r.x0, r.y0, r.x1, r.y1)
}
fn hsurf(s: &Surface, r: Rect) -> u64 {
    let mut h = H0;
    fnv(&mut h, &s.to_interleaved(r));
    h
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
    /// unit() scaled to lo..hi.
    fn span(&mut self, lo: f32, hi: f32) -> f32 {
        let u = self.unit();
        lo + u * (hi - lo)
    }
}

const MODES: [ColorMode; 6] =
    [ColorMode::Rgb, ColorMode::Grayscale, ColorMode::Cmyk, ColorMode::Lab, ColorMode::Multichannel, ColorMode::Duotone];
const DEPTHS: [SampleType; 3] = [SampleType::U8, SampleType::U16, SampleType::F32];
const STYLES: [GradientStyle; 5] =
    [GradientStyle::Linear, GradientStyle::Radial, GradientStyle::Angle, GradientStyle::Reflected, GradientStyle::Diamond];
const SPACES: [ToneSpace; 3] = [ToneSpace::Rgb, ToneSpace::Cmyk, ToneSpace::Lab];

fn opacity(r: &mut Rng) -> f32 {
    let k = r.below(10);
    match k {
        0..=4 => 1.0,
        5 => 0.5,
        6 => 0.25,
        7 => r.unit(),
        8 => 0.0,
        _ => {
            let j = r.below(2);
            if j == 0 { 1.5 } else { -0.25 }
        }
    }
}

fn layer_format(r: &mut Rng, doc_fmt: PixelFormat) -> PixelFormat {
    let k = r.below(8);
    match k {
        0..=2 => doc_fmt,
        3 => PixelFormat::RGBA8,
        4 => PixelFormat::RGBA16,
        5 => PixelFormat::RGBA32F,
        6 => {
            let d = r.below(3);
            PixelFormat::new(ColorMode::Grayscale, DEPTHS[d as usize], true)
        }
        _ => {
            let m = r.below(2);
            let d = r.below(3);
            PixelFormat::new([ColorMode::Cmyk, ColorMode::Lab][m as usize], DEPTHS[d as usize], true)
        }
    }
}

fn pixel(r: &mut Rng, fmt: PixelFormat) -> Vec<f32> {
    let n = fmt.channels();
    let hdr = fmt.sample == SampleType::F32 && r.below(4) == 0;
    let mut v = Vec::new();
    for _ in 0..n {
        let u = r.unit();
        v.push(if hdr { u * 2.0 } else { u });
    }
    v
}

fn paint(r: &mut Rng, s: &mut Surface, w: i32, h: i32) {
    let fmt = s.format();
    let k = 1 + r.below(3);
    for _ in 0..k {
        let x0 = r.range(-4, w + 4);
        let y0 = r.range(-4, h + 4);
        let x1 = x0 + r.range(1, w + 2);
        let y1 = y0 + r.range(1, h + 2);
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

/// A colour in RGB, grey, CMYK or Lab with random alpha.
fn color(r: &mut Rng) -> Color {
    let k = r.below(6);
    let c0 = r.unit();
    let c1 = r.unit();
    let c2 = r.unit();
    let c3 = r.unit();
    let a = if r.below(2) == 0 { 1.0 } else { r.unit() };
    let mode = match k {
        0..=2 => ColorMode::Rgb,
        3 => ColorMode::Grayscale,
        4 => ColorMode::Cmyk,
        _ => ColorMode::Lab,
    };
    let c = match mode {
        ColorMode::Rgb | ColorMode::Lab => [c0, c1, c2, 0.0],
        ColorMode::Grayscale => [c0, 0.0, 0.0, 0.0],
        _ => [c0, c1, c2, c3],
    };
    Color { mode, c, alpha: a }
}

fn levels_channel(r: &mut Rng) -> LevelsChannel {
    if r.below(4) == 0 {
        return LevelsChannel::default();
    }
    let in_black = r.span(0.0, 0.4);
    let in_white = r.span(0.6, 1.0);
    let u = r.unit();
    let k = r.below(5);
    let gamma = match k {
        0 => 1.0,
        1 => 0.5,
        2 => 1.78,
        3 => 2.5,
        _ => u * 3.0,
    };
    let out_black = r.span(0.0, 0.2);
    let out_white = r.span(0.8, 1.0);
    LevelsChannel { in_black, in_white, gamma, out_black, out_white }
}

fn curve(r: &mut Rng) -> Vec<CurvePoint> {
    let n = r.below(6);
    let mut v = Vec::new();
    for i in 0..n {
        let dup = i > 0 && r.below(5) == 0;
        let input = if dup { v.last().map_or(0.0, |p: &CurvePoint| p.input) } else { r.unit() };
        let output = r.span(-0.1, 1.1);
        v.push(CurvePoint { input, output });
    }
    v
}

fn adjustment(r: &mut Rng) -> Adjustment {
    let kind = r.below(17);
    match kind {
        0 => {
            let brightness = r.span(-150.0, 150.0);
            let contrast = r.span(-120.0, 120.0);
            let legacy = r.below(2) == 0;
            Adjustment::BrightnessContrast { brightness, contrast, legacy }
        }
        1 => {
            let master = levels_channel(r);
            let a = levels_channel(r);
            let b = levels_channel(r);
            let cc = levels_channel(r);
            let s = r.below(3);
            let black = levels_channel(r);
            Adjustment::Levels { master, per_channel: [a, b, cc], space: SPACES[s as usize], black }
        }
        2 => {
            let master = curve(r);
            let a = curve(r);
            let b = curve(r);
            let cc = curve(r);
            let s = r.below(3);
            let black = curve(r);
            Adjustment::Curves { master, per_channel: [a, b, cc], space: SPACES[s as usize], black, pencil: false }
        }
        3 => {
            let exposure = r.span(-2.0, 2.0);
            let offset = r.span(-0.2, 0.2);
            let k = r.below(4);
            let gamma = [1.0, 0.5, 2.2, 0.0][k as usize];
            Adjustment::Exposure { exposure, offset, gamma }
        }
        4 => {
            let vibrance = if r.below(3) == 0 { 0.0 } else { r.span(-100.0, 100.0) };
            let saturation = if r.below(3) == 0 { 0.0 } else { r.span(-100.0, 100.0) };
            Adjustment::Vibrance { vibrance, saturation }
        }
        5 => {
            let hue = r.span(-180.0, 180.0);
            let saturation = r.span(-100.0, 100.0);
            let lightness = r.span(-100.0, 100.0);
            let colorize = r.below(4) == 0;
            let mut ranges = HueRange::defaults();
            for g in ranges.iter_mut() {
                if r.below(3) == 0 {
                    g.hue = r.span(-180.0, 180.0);
                    g.saturation = r.span(-100.0, 100.0);
                    g.lightness = r.span(-100.0, 100.0);
                }
            }
            Adjustment::HueSaturation { hue, saturation, lightness, colorize, ranges }
        }
        6 => {
            let mut v = [0.0f32; 9];
            for x in v.iter_mut() {
                *x = r.span(-100.0, 100.0);
            }
            let preserve_luminosity = r.below(2) == 0;
            Adjustment::ColorBalance {
                shadows: [v[0], v[1], v[2]],
                midtones: [v[3], v[4], v[5]],
                highlights: [v[6], v[7], v[8]],
                preserve_luminosity,
            }
        }
        7 => {
            let mut weights = [0.0f32; 6];
            for x in weights.iter_mut() {
                *x = r.span(-100.0, 200.0);
            }
            let tint = if r.below(2) == 0 {
                let a = r.unit();
                let b = r.unit();
                let cc = r.unit();
                Some([a, b, cc])
            } else {
                None
            };
            Adjustment::BlackWhite { weights, tint }
        }
        8 => {
            let a = r.unit();
            let b = r.unit();
            let cc = r.unit();
            let density = r.span(-0.2, 1.2);
            let preserve_luminosity = r.below(2) == 0;
            Adjustment::PhotoFilter { color: [a, b, cc], density, preserve_luminosity }
        }
        9 => {
            let mut matrix = [[0.0f32; 4]; 3];
            for row in matrix.iter_mut() {
                for x in row.iter_mut() {
                    *x = r.span(-2.0, 2.0);
                }
            }
            let monochrome = r.below(3) == 0;
            Adjustment::ChannelMixer { matrix, monochrome }
        }
        10 => {
            let n = 2 + r.below(4) as u32;
            let len = (n * n * n * 3) as usize;
            let mut t = Vec::with_capacity(len);
            for _ in 0..len {
                t.push(r.unit());
            }
            let k = r.below(6);
            let (lut, size) = match k {
                0 => (None, n),
                1 => (Some(std::sync::Arc::new(t)), n + 1),
                _ => (Some(std::sync::Arc::new(t)), n),
            };
            let tetrahedral = r.below(2) == 0;
            let dither = r.below(2) == 0;
            Adjustment::ColorLookup { name: "lut".into(), lut, size, tetrahedral, dither }
        }
        11 => Adjustment::Invert,
        12 => {
            let levels = r.below(260) as u32;
            Adjustment::Posterize { levels }
        }
        13 => {
            let level = r.unit();
            Adjustment::Threshold { level }
        }
        14 => {
            let n = r.below(4);
            let mut stops = Vec::new();
            for _ in 0..n {
                let p = r.unit();
                let a = r.unit();
                let b = r.unit();
                let cc = r.unit();
                stops.push((p, [a, b, cc]));
            }
            let reverse = r.below(2) == 0;
            let dither = r.below(2) == 0;
            Adjustment::GradientMap { stops, reverse, dither }
        }
        15 => {
            let relative = r.below(2) == 0;
            let mut adjustments = [[0.0f32; 4]; 9];
            for row in adjustments.iter_mut() {
                if r.below(2) == 0 {
                    for x in row.iter_mut() {
                        *x = r.span(-100.0, 100.0);
                    }
                }
            }
            Adjustment::SelectiveColor { relative, adjustments }
        }
        _ => Adjustment::Unsupported { psd_key: "xxxx".into(), raw: vec![1, 2, 3] },
    }
}

fn gradient_fill(r: &mut Rng) -> Fill {
    let n = r.below(4);
    let mut stops = Vec::new();
    for _ in 0..n {
        let p = r.span(-0.1, 1.1);
        let col = color(r);
        stops.push((p, col));
    }
    let u = r.unit();
    let k = r.below(6);
    let angle = match k {
        0 => 0.0,
        1 => 30.0,
        2 => 45.0,
        3 => 90.0,
        4 => -37.0,
        _ => u * 720.0 - 360.0,
    };
    let u = r.unit();
    let k = r.below(4);
    let scale = match k {
        0 => 1.0,
        1 => 0.5,
        2 => 1.7,
        _ => u * 3.0,
    };
    let s = r.below(5);
    let reverse = r.below(2) == 0;
    let mut opacity_stops = Vec::new();
    if r.below(2) == 0 {
        let m = r.below(4);
        for _ in 0..m {
            let p = r.unit();
            let o = r.unit();
            opacity_stops.push((p, o));
        }
    }
    let mut midpoints = Vec::new();
    if r.below(2) == 0 {
        let m = r.below(4);
        for _ in 0..m {
            midpoints.push(r.unit());
        }
    }
    let offset = if r.below(3) == 0 {
        let a = r.span(-0.5, 0.5);
        let b = r.span(-0.5, 0.5);
        (a, b)
    } else {
        (0.0, 0.0)
    };
    let dither = r.below(3) == 0;
    let align = r.below(2) == 0;
    Fill::Gradient { stops, angle, scale, style: STYLES[s as usize], reverse, opacity_stops, midpoints, offset, dither, align }
}

fn pattern_fill(r: &mut Rng, npatterns: usize) -> Fill {
    let k = r.below(npatterns as u64 + 1);
    let name = if (k as usize) < npatterns { format!("p{k}") } else { "nope".to_string() };
    let u = r.unit();
    let k = r.below(4);
    let scale = match k {
        0 => 1.0,
        1 => 0.5,
        2 => 2.0,
        _ => u * 3.0,
    };
    let u = r.unit();
    let k = r.below(4);
    let angle = match k {
        0 => 0.0,
        1 => 90.0,
        2 => 30.0,
        _ => u * 360.0,
    };
    let link = r.below(2) == 0;
    let phase = if r.below(2) == 0 {
        let a = r.span(-4.0, 4.0);
        let b = r.span(-4.0, 4.0);
        (a, b)
    } else {
        (0.0, 0.0)
    };
    Fill::Pattern { name, scale, id: String::new(), angle, link, phase }
}

fn polygon(r: &mut Rng, w: i32, h: i32) -> Path {
    let k = 3 + r.below(2);
    let mut pts = Vec::new();
    for _ in 0..k {
        let x = r.range(-2, w + 2);
        let y = r.range(-2, h + 2);
        pts.push((f64::from(x) + 0.5, f64::from(y) + 0.25));
    }
    let mut path = Path::new(vec![Subpath::polygon(&pts)]);
    path.inverted = r.below(5) == 0;
    path
}

fn stroked_shape(r: &mut Rng, w: i32, h: i32) -> Layer {
    let path = polygon(r, w, h);
    let k = r.below(4);
    let fill = match k {
        0 => None,
        1 => Some(gradient_fill(r)),
        _ => {
            let col = color(r);
            Some(Fill::Solid(col))
        }
    };
    let k = r.below(4);
    let width = [1.0, 3.0, 0.25, 6.0][k as usize];
    let col = color(r);
    let opacity = if r.below(3) == 0 { r.unit() } else { 1.0 };
    let a = r.below(3);
    let align = [StrokeAlign::Inside, StrokeAlign::Center, StrokeAlign::Outside][a as usize];
    let stroke = ShapeStroke { width, paint: Fill::Solid(col), opacity, align, ..ShapeStroke::default() };
    let cache = if r.below(2) == 0 {
        let mut s = Surface::new(PixelFormat::RGBA8);
        paint(r, &mut s, w, h);
        Some(s)
    } else {
        None
    };
    let sh = ShapeLayer { path, fill, stroke: Some(stroke), cache, ..Default::default() };
    Layer::new("stroked", LayerContent::Shape(sh))
}

fn gen_layers(r: &mut Rng, doc_fmt: PixelFormat, w: i32, h: i32, level: u32, n: u64, np: usize) -> Vec<Layer> {
    let mut v = Vec::new();
    for i in 0..n {
        let mut l = gen_layer(r, doc_fmt, w, h, level, np);
        if i > 0 && r.below(4) == 0 {
            l.clipped = true;
        }
        v.push(l);
    }
    v
}

fn gen_layer(r: &mut Rng, doc_fmt: PixelFormat, w: i32, h: i32, level: u32, np: usize) -> Layer {
    let kind = r.below(12);
    let mut l = match kind {
        4 | 5 => {
            let a = adjustment(r);
            Layer::new("adj", LayerContent::Adjustment(a))
        }
        6 if level < 3 => {
            let n = 1 + r.below(3);
            let kids = gen_layers(r, doc_fmt, w, h, level + 1, n, np);
            let mut g = Layer::group("g", kids);
            if r.below(2) == 0 {
                g.blend = BlendMode::Normal;
            }
            g
        }
        6 | 7 => {
            let f = gradient_fill(r);
            Layer::new("gradient", LayerContent::Fill(f))
        }
        8 => {
            let f = pattern_fill(r, np);
            Layer::new("pattern", LayerContent::Fill(f))
        }
        9 => {
            let col = color(r);
            Layer::new("fill", LayerContent::Fill(Fill::Solid(col)))
        }
        10 => stroked_shape(r, w, h),
        11 => {
            let mut s = Surface::new(PixelFormat::RGBA8);
            paint(r, &mut s, w, h);
            let mut t = TextLayer::default();
            t.cache = Some(s);
            Layer::new("type", LayerContent::Text(t))
        }
        _ => {
            let fmt = layer_format(r, doc_fmt);
            let mut l = Layer::raster("px", fmt);
            if let Some(s) = l.surface_mut() {
                paint(r, s, w, h);
            }
            l
        }
    };
    if r.below(8) == 0 {
        l.visible = false;
    }
    if r.below(2) == 0 {
        let m = r.below(27);
        l.blend = BlendMode::LAYER_MODES[m as usize];
    }
    l.opacity = opacity(r);
    l.fill_opacity = opacity(r);
    if r.below(4) == 0 {
        let mut m = if r.below(2) == 0 { LayerMask::reveal_all() } else { LayerMask::hide_all() };
        let x0 = r.range(-2, w);
        let y0 = r.range(-2, h);
        let v = r.unit();
        let x1 = x0 + r.range(1, w);
        let y1 = y0 + r.range(1, h);
        m.surface.fill_rect(Rect::new(x0, y0, x1, y1), &[v]);
        let k = r.below(3);
        m.feather = [0.0, 0.0, 2.5][k as usize];
        m.enabled = r.below(6) != 0;
        l.mask = Some(m);
    }
    if r.below(6) == 0 {
        let p = polygon(r, w, h);
        let mut vm = VectorMask::new(p);
        vm.enabled = r.below(6) != 0;
        l.vector_mask = Some(vm);
    }
    if r.below(8) == 0 {
        l.excluded_channels = r.below(8) as u32;
    }
    if r.below(8) == 0 {
        let i = r.below(4) as usize;
        let b0 = r.below(200) as u8;
        let b1 = b0 + r.below(40) as u8;
        let w0 = 120 + r.below(100) as u8;
        let w1 = w0 + r.below(36) as u8;
        let a = BlendRange { black: [b0, b1], white: [w0, w1] };
        let under = r.below(2) == 0;
        l.blend_if.set(i, if under { [BlendRange::FULL, a] } else { [a, BlendRange::FULL] });
    }
    if r.below(5) == 0 {
        let k = r.below(2);
        l.advanced.knockout = if k == 0 { Knockout::Shallow } else { Knockout::Deep };
    }
    if r.below(5) == 0 {
        l.advanced.blend_clipped = false;
    }
    if r.below(6) == 0 {
        l.advanced.transparency_shapes = false;
    }
    if r.below(6) == 0 {
        l.advanced.blend_interior = true;
    }
    if r.below(8) == 0 {
        l.advanced.layer_mask_hides_effects = true;
    }
    if r.below(8) == 0 {
        l.advanced.vector_mask_hides_effects = true;
    }
    l
}

fn gen_doc(seed: u64) -> Document {
    let mut r = Rng(seed.wrapping_mul(0x9e3779b97f4a7c15) | 1);
    let mode = MODES[r.below(6) as usize];
    let depth = DEPTHS[r.below(3) as usize];
    let w = 9 + r.below(30) as u32;
    let h = 7 + r.below(24) as u32;
    let mut doc = Document::new("v", Size::new(w, h), mode, depth);
    let fmt = doc.pixel_format();
    let np = r.below(3) as usize;
    for i in 0..np {
        let pw = 1 + r.below(5) as u32;
        let ph = 1 + r.below(5) as u32;
        let k = r.below(5);
        let pf = match k {
            0 => PixelFormat::RGBA8,
            1 => PixelFormat::RGBA16,
            2 => PixelFormat::RGBA32F,
            3 => PixelFormat::new(ColorMode::Grayscale, SampleType::U8, true),
            _ => PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true),
        };
        let mut s = Surface::new(pf);
        paint(&mut r, &mut s, pw as i32, ph as i32);
        doc.patterns.push(Pattern::new(format!("p{i}"), s, pw, ph));
    }
    let mut layers = Vec::new();
    if r.below(3) == 0 {
        let mut bg = Layer::raster("Background", fmt);
        let px = pixel(&mut r, fmt);
        if let Some(s) = bg.surface_mut() {
            s.fill_rect(Rect::new(0, 0, w as i32, h as i32), &px);
        }
        bg.locks.transparency = true;
        bg.visible = r.below(6) != 0;
        layers.push(bg);
    }
    let n = 1 + r.below(5);
    layers.extend(gen_layers(&mut r, fmt, w as i32, h as i32, 0, n, np));
    doc.layers = layers;
    doc
}

fn doc_line(seed: u64) {
    let doc = gen_doc(seed);
    let mut r = Rng(seed ^ 0x5bd1e995);
    let b = doc.bounds();
    let (w, h) = (b.width(), b.height());
    let mut out = format!("doc\t{seed}");
    out += &format!("\tflat={:016x}", hbuf(&c::flatten(&doc)));
    out += &format!("\ttiled={:016x}", hbuf(&c::render_tiled(&doc, b, 7)));
    let x0 = r.range(-6, w as i32);
    let y0 = r.range(-6, h as i32);
    let x1 = x0 + r.range(1, w as i32 + 8);
    let y1 = y0 + r.range(1, h as i32 + 8);
    out += &format!("\tsub={:016x}", hbuf(&c::render_tiled(&doc, Rect::new(x0, y0, x1, y1), 5)));
    let rw = 1 + r.below(w as u64) as u32;
    let rh = 1 + r.below(h as u64) as u32;
    out += &format!("\treduced={:016x}", hbuf(&c::render_reduced(&doc, rw, rh)));
    let dx = r.range(-3, w as i32);
    let dy = r.range(-3, h as i32);
    let dx1 = dx + r.range(1, 9);
    let dy1 = dy + r.range(1, 9);
    out += &format!("\tdamage={:016x}", hbuf(&c::render_reduced_damage(&doc, rw, rh, Rect::new(dx, dy, dx1, dy1))));
    let area = Rect::new(-5, -4, w as i32 + 6, h as i32 + 3);
    out += &format!("\tarea={:016x}", hbuf(&c::render_reduced_rect(&doc, area, (w + 11) / 3, (h + 7) / 2)));
    let t = c::thumbnail(&doc, 1 + r.below(20) as u32);
    let mut ht = H0;
    fnv(&mut ht, &t.width.to_le_bytes());
    fnv(&mut ht, &t.height.to_le_bytes());
    fnv(&mut ht, &t.pixels);
    out += &format!("\tthumb={ht:016x}");
    let k = r.below(3);
    let fmt = [doc.pixel_format(), PixelFormat::RGBA8, PixelFormat::RGBA32F][k as usize];
    let s = c::flatten_to_surface(&doc, fmt, None);
    let mut hs = H0;
    fnv(&mut hs, &s.to_interleaved(b));
    fnv(&mut hs, &(s.tile_count() as u64).to_le_bytes());
    out += &format!("\tsurface={hs:016x}");
    let p = c::proxy::proxy_document(&doc, 2);
    out += &format!("\tproxy={:016x}", hbuf(&c::flatten(&p)));
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
        q += &format!("\tto={}\tocc={}", c::transparent_outside(l) as u8, c::occludes_below(l, doc.mode) as u8);
        let clipped: Vec<Layer> = doc.layers[i + 1..].iter().take_while(|x| x.clipped && !l.clipped).cloned().collect();
        q += &format!(
            "\tadv={}\tshapeless={}\tbg={}",
            c::advanced::advanced_active(l, &clipped) as u8,
            c::advanced::shapeless(l) as u8,
            c::advanced::is_background(l) as u8
        );
        q += &format!("\tframe={}", rs(c::fill_frame(l, b)));
        if let LayerContent::Fill(f) = &l.content {
            q += &format!("\tfill={:016x}", hbuf(&c::render_fill_content(l, f, b, &doc.patterns)));
        }
        let shape = c::layer_shape(&doc, l, b);
        let mut hsh = H0;
        for x in &shape {
            hf(&mut hsh, *x);
        }
        q += &format!("\tshape={hsh:016x}");
        if let LayerContent::Shape(sh) = &l.content {
            match shape_split::split(sh, b, doc.depth) {
                Some((fs, ss)) => q += &format!("\tsplit={:016x},{:016x}", hsurf(&fs, b), hsurf(&ss, b)),
                None => q += "\tsplit=none",
            }
        }
        println!("{q}");
    }
}

// ---- function sweeps ----

const VALS: [f32; 16] = [0.0, -0.0, 1e-5, 0.1, 0.2, 0.25, 0.3, 0.5, 0.6, 0.75, 0.9, 1.0 - 1e-5, 1.0, 1.5, -0.1, f32::NAN];

/// A 13 x 7 buffer at (-3, 5) with every VALS combination, varied alpha (incl. 0).
fn test_buffer() -> c::Buffer {
    let rect = Rect::new(-3, 5, 10, 12);
    let mut px = Vec::new();
    for i in 0..91usize {
        let a = VALS[i % 16];
        let b = VALS[(i / 3) % 16];
        let cc = VALS[(i * 7 + 3) % 16];
        let alpha = [1.0, 0.5, 0.0, 1.0, f32::NAN, 1.0][i % 6];
        px.push([a, b, cc, alpha]);
    }
    c::Buffer { rect, px }
}

fn adjust_sweeps() {
    let transfers = [Transfer::Srgb, Transfer::Gamma(1.732), Transfer::Gamma(1.0)];
    let depths = [None, Some(SampleType::U8), Some(SampleType::U16), Some(SampleType::F32)];
    let mut ha = H0;
    let mut hl = H0;
    for seed in 1..=600u64 {
        let mut r = Rng(seed.wrapping_mul(0x2545f4914f6cdd1d) | 1);
        let a = adjustment(&mut r);
        let mut h = H0;
        for t in transfers {
            for d in depths {
                let mut b = test_buffer();
                adjust::apply_depth(&a, &mut b, t, d);
                fnv(&mut h, &hpx(&b.px).to_le_bytes());
            }
        }
        fnv(&mut ha, &h.to_le_bytes());
        if seed % 100 == 0 {
            println!("fn\tapply_depth_{seed}\t{ha:016x}");
        }
        if matches!(a, Adjustment::Levels { .. } | Adjustment::Curves { .. }) {
            for d in depths {
                for row in adjust::tone_luts_depth(&a, d).iter() {
                    for v in row {
                        hf(&mut hl, *v);
                    }
                }
            }
        }
    }
    println!("fn\ttone_luts_depth\t{hl:016x}");
    // Curves with unsorted, duplicate, NaN and out-of-range points.
    let pts = |v: &[(f32, f32)]| v.iter().map(|&(input, output)| CurvePoint { input, output }).collect::<Vec<_>>();
    let curves = [
        pts(&[]),
        pts(&[(0.5, 0.5)]),
        pts(&[(0.0, 0.0), (1.0, 1.0)]),
        pts(&[(1.0, 0.2), (0.0, 0.9), (0.5, 0.1)]),
        pts(&[(0.2, 0.3), (0.2, 0.9), (0.2000001, 0.4), (0.8, 0.7)]),
        pts(&[(0.1, 1.5), (0.4, -0.5), (0.6, 1.2), (0.9, -0.2)]),
        pts(&[(f32::NAN, 0.5), (0.3, 0.2), (0.7, 0.8)]),
        pts(&[(0.3, f32::NAN), (0.6, 0.5)]),
        pts(&[(-0.5, 0.1), (1.5, 0.9)]),
    ];
    let mut hc = H0;
    for cv in &curves {
        for v in adjust::curve_lut(cv) {
            hf(&mut hc, v);
        }
    }
    println!("fn\tcurve_lut\t{hc:016x}");
    let chans = [
        LevelsChannel::default(),
        LevelsChannel { in_black: 0.2, in_white: 0.8, gamma: 1.0, out_black: 0.0, out_white: 1.0 },
        LevelsChannel { in_black: 44.0 / 255.0, in_white: 214.0 / 255.0, gamma: 1.78, out_black: 0.1, out_white: 0.9 },
        LevelsChannel { in_black: 0.5, in_white: 0.5, gamma: 0.0, out_black: 0.0, out_white: 1.0 },
        LevelsChannel { in_black: 0.6, in_white: 0.3, gamma: 3.0, out_black: 1.0, out_white: 0.0 },
        LevelsChannel { in_black: 0.0, in_white: 1.0, gamma: f32::NAN, out_black: 0.0, out_white: 1.0 },
        LevelsChannel { in_black: 15.0 / 255.0, in_white: 230.0 / 255.0, gamma: 1.3, out_black: 0.04, out_white: 0.96 },
    ];
    let quanta = [None, Some(255.0f32), Some(32768.0), Some(0.0), Some(f32::NAN), Some(-3.0), Some(1.0)];
    let mut hq = H0;
    let mut hfl = H0;
    for ch in &chans {
        for &v in &VALS {
            for q in quanta {
                hf(&mut hq, adjust::levels_q(ch, v, q));
            }
            hf(&mut hfl, adjust::levels_float(ch, v));
        }
    }
    println!("fn\tlevels_q\t{hq:016x}");
    println!("fn\tlevels_float\t{hfl:016x}");
    let mut hb = H0;
    let mut hk = H0;
    for b in [-150.0f32, -50.0, 0.0, 37.0, 150.0, 400.0, f32::NAN] {
        for &v in &VALS {
            hf(&mut hb, adjust::modern_brightness(v, b));
        }
    }
    for k in [-50.0f32, -25.0, 0.0, 50.0, 100.0, 128.0, f32::NAN] {
        for &v in &VALS {
            hf(&mut hk, adjust::modern_contrast(v, k));
        }
    }
    println!("fn\tmodern_brightness\t{hb:016x}");
    println!("fn\tmodern_contrast\t{hk:016x}");
    let mut hp = H0;
    for n in (0..300u32).step_by(7).chain([u32::MAX, 255, 256, 2, 1]) {
        for &v in &VALS {
            hf(&mut hp, adjust::posterize(v, n));
        }
    }
    println!("fn\tposterize\t{hp:016x}");
    let mut hh = H0;
    let mut hs = H0;
    let mut hbw = H0;
    let mut hsel = H0;
    let mut hvib = H0;
    let weights = [[40.0f32, 60.0, 40.0, 60.0, 20.0, 80.0], [100.0, -50.0, 0.0, 200.0, 30.0, 70.0]];
    let mut sel = [[0.0f32; 4]; 9];
    for (i, row) in sel.iter_mut().enumerate() {
        for (j, x) in row.iter_mut().enumerate() {
            *x = ((i * 4 + j) as f32 * 37.0) % 200.0 - 100.0;
        }
    }
    let transfers = [Transfer::Srgb, Transfer::Gamma(1.732), Transfer::Gamma(2.2), Transfer::Gamma(1.0)];
    for i in 0..16usize {
        for j in 0..16usize {
            for k in [0usize, 3, 7, 12, 15] {
                let col = [VALS[i], VALS[j], VALS[k]];
                let (h, s, l) = adjust::rgb_to_hsl(col);
                for v in [h, s, l] {
                    hf(&mut hh, v);
                }
                for v in adjust::hsl_to_rgb(col[0], col[1], col[2]) {
                    hf(&mut hh, v);
                }
                for (hue, sa, li, cz) in [(37.0f32, 0.5f32, 0.0f32, false), (-200.0, -1.0, 0.7, false), (400.0, 0.1, -0.5, true), (0.0, -0.3, 0.0, true)] {
                    for v in adjust::hue_saturation(col, hue, sa, li, cz) {
                        hf(&mut hs, v);
                    }
                }
                for w in &weights {
                    hf(&mut hbw, adjust::black_white_gray(col, w));
                }
                for v in adjust::selective_color_weights(col) {
                    hf(&mut hsel, v);
                }
                for rel in [false, true] {
                    for v in adjust::selective_color(col, rel, &sel) {
                        hf(&mut hsel, v);
                    }
                }
                for (vv, ss) in [(0.5f32, 0.0f32), (-0.7, 0.0), (0.0, -1.0), (1.0, 0.6), (-0.3, -0.4)] {
                    for t in transfers {
                        for hi in [1.0f32, f32::INFINITY] {
                            for v in adjust::vibrance_px(col, vv, ss, t, hi) {
                                hf(&mut hvib, v);
                            }
                        }
                    }
                    for v in adjust::vibrance_lin(col, vv) {
                        hf(&mut hvib, v);
                    }
                }
            }
        }
    }
    println!("fn\thsl\t{hh:016x}");
    println!("fn\thue_saturation\t{hs:016x}");
    println!("fn\tblack_white_gray\t{hbw:016x}");
    println!("fn\tselective_color\t{hsel:016x}");
    println!("fn\tvibrance\t{hvib:016x}");
    let mut hy = H0;
    for y in -5..5 {
        for x in -5..5 {
            hf(&mut hy, adjust::bayer4(x, y));
        }
    }
    println!("fn\tbayer4\t{hy:016x}");
    let mut hlut = H0;
    let mut r = Rng(0x1234_5678_9abc_def1);
    for n in [2usize, 3, 5] {
        let mut t = Vec::new();
        for _ in 0..n * n * n * 3 {
            t.push(r.unit());
        }
        for i in 0..16usize {
            for j in 0..16usize {
                let col = [VALS[i], VALS[j], VALS[(i + j) % 16]];
                for tet in [false, true] {
                    for v in adjust::lut3d_sample(&t, n, col, tet) {
                        hf(&mut hlut, v);
                    }
                }
            }
        }
    }
    println!("fn\tlut3d_sample\t{hlut:016x}");
    let mut hpf = H0;
    for col in [[1.0f32, 0.5, 0.0], [0.0, 0.3, 0.9], [2.0, -1.0, f32::NAN]] {
        for d in [0.0f32, 0.25, 1.0, 1.5, f32::NAN, f32::INFINITY] {
            for ny in [false, true] {
                for row in adjust::photo_filter_matrix(col, d, ny) {
                    for v in row {
                        hf(&mut hpf, v);
                    }
                }
            }
        }
    }
    println!("fn\tphoto_filter_matrix\t{hpf:016x}");
    let mut hr = H0;
    for seed in 1..=20u64 {
        let mut r = Rng(seed * 77 + 1);
        let mut ranges = HueRange::defaults();
        for g in ranges.iter_mut() {
            if r.below(2) == 0 {
                g.hue = r.span(-180.0, 180.0);
                g.saturation = r.span(-100.0, 100.0);
                g.lightness = r.span(-100.0, 100.0);
                let b0 = r.span(-60.0, 400.0);
                g.bounds = [b0, b0 + 10.0, b0 + 40.0, b0 + 70.0];
            }
        }
        for row in adjust::hue_range_tables(&ranges).iter() {
            for v in row {
                hf(&mut hr, *v);
            }
        }
    }
    println!("fn\thue_range_tables\t{hr:016x}");
}

fn gradient_sweeps() {
    let mut hr = H0;
    let mut hd_ = H0;
    let ts = [-0.5f32, 0.0, 0.1, 0.25, 0.33, 0.5, 0.75, 0.999, 1.0, 1.2, f32::NAN];
    let frames = [Rect::new(0, 0, 8, 8), Rect::new(-3, 2, 20, 9), Rect::new(5, 5, 5, 9), Rect::new(1, 1, 2, 2)];
    for seed in 1..=300u64 {
        let mut r = Rng(seed.wrapping_mul(0x9e3779b97f4a7c15) ^ 0xabcdef);
        let f = gradient_fill(&mut r);
        if let Some(ramp) = gf::Ramp::new(&f) {
            for &t in &ts {
                for v in ramp.sample(t) {
                    hf(&mut hr, v);
                }
            }
        }
        let fr = frames[(seed % 4) as usize];
        fnv(&mut hd_, &hpx(&gf::render(&f, Rect::new(-2, 1, 9, 6), fr)).to_le_bytes());
        if seed % 100 == 0 {
            println!("fn\tgradient_render_{seed}\t{hd_:016x}");
        }
    }
    println!("fn\tramp_sample\t{hr:016x}");
    let mut hm = H0;
    for &u in &ts {
        for m in [0.0f32, 0.05, 0.25, 0.5, 0.5000001, 0.75, 0.95, 1.0, f32::NAN] {
            hf(&mut hm, gf::midpoint_remap(u, m));
        }
    }
    for y in -3..3 {
        for x in -3..3 {
            let mut c4 = [0.5f32, 0.0, 1.0, 0.7];
            gf::dither(&mut c4, x * 1000, y * 77);
            for v in c4 {
                hf(&mut hm, v);
            }
        }
    }
    println!("fn\tmidpoint_dither\t{hm:016x}");
    let mut hh = H0;
    let hframes = [Rect::new(10, -4, 210, 96), Rect::new(0, 0, 1, 1), Rect::new(3, 3, 3, 3)];
    for s in STYLES {
        for fr in hframes {
            for a in [0.0f32, 30.0, 90.0, -135.0, 1e9] {
                for sc in [1.0f32, 0.0, 2.5] {
                    for off in [(0.0f32, 0.0f32), (0.3, -0.2)] {
                        let (p, q) = gf::handles(s, a, sc, off, fr);
                        for v in [p[0], p[1], q[0], q[1]] {
                            hf(&mut hh, v);
                        }
                        let (a2, s2, o2) = gf::from_handles(s, p, q, fr, 17.0);
                        for v in [a2, s2, o2.0, o2.1] {
                            hf(&mut hh, v);
                        }
                    }
                }
            }
            let (a2, s2, o2) = gf::from_handles(s, [4.0, 4.0], [4.0, 4.0], fr, 17.0);
            for v in [a2, s2, o2.0, o2.1] {
                hf(&mut hh, v);
            }
        }
    }
    println!("fn\thandles\t{hh:016x}");
}

fn pattern_sweeps() {
    let mut ht = H0;
    let mut hm = H0;
    let coords = [0.0f64, 0.5, 1.25, -0.75, 3.999, 7.5, -1e9, 1e30, -1e30, f64::NAN, f64::INFINITY, 2.5e18];
    for seed in 1..=12u64 {
        let mut r = Rng(seed * 1009 + 7);
        let pw = 1 + r.below(6) as u32;
        let ph = 1 + r.below(6) as u32;
        let k = r.below(4);
        let pf = match k {
            0 => PixelFormat::RGBA8,
            1 => PixelFormat::RGBA32F,
            2 => PixelFormat::new(ColorMode::Grayscale, SampleType::U16, true),
            _ => PixelFormat::new(ColorMode::Lab, SampleType::U8, true),
        };
        let mut s = Surface::new(pf);
        paint(&mut r, &mut s, pw as i32, ph as i32);
        let pat = Pattern::new("t", s, pw, ph);
        let Some(tile) = pattern::Tile::new(&pat) else { continue };
        for &u in &coords {
            for &v in &coords {
                for x in tile.sample(u, v) {
                    hf(&mut ht, x);
                }
            }
        }
        for (link, phase, scale, angle) in [(true, (0.0f32, 0.0f32), 1.0f32, 0.0f32), (false, (2.5, -1.0), 0.5, 30.0), (true, (0.0, 3.0), 0.0, 90.0), (false, (0.0, 0.0), 2.0, -45.0), (true, (1.0, 1.0), f32::NAN, f32::NAN)] {
            let place = pattern::Placement::new(Rect::new(2, -3, 11, 9), link, phase, scale, angle);
            fnv(&mut ht, &hpx(&pattern::render(&tile, &place, Rect::new(-3, -2, 6, 5))).to_le_bytes());
            let anch = pattern::Placement::anchored((1.5, -2.25), link, phase, scale, angle);
            for &x in &coords[..6] {
                let (u, v) = anch.map(x, x * 0.5 - 1.0);
                hd(&mut hm, u);
                hd(&mut hm, v);
            }
        }
    }
    let empty = pattern::Placement::new(Rect::EMPTY, true, (1.0, 2.0), 1.0, 10.0);
    let (u, v) = empty.map(3.0, 4.0);
    hd(&mut hm, u);
    hd(&mut hm, v);
    println!("fn\tpattern_tiles\t{ht:016x}");
    println!("fn\tplacement\t{hm:016x}");
}

fn split_sweeps() {
    let mut h = H0;
    for seed in 1..=40u64 {
        let mut r = Rng(seed * 31 + 5);
        let w = 6 + r.below(20) as i32;
        let hh = 5 + r.below(16) as i32;
        let d = r.below(3);
        let l = stroked_shape(&mut r, w, hh);
        let LayerContent::Shape(sh) = &l.content else { continue };
        let canvas = Rect::new(0, 0, w, hh);
        match shape_split::split(sh, canvas, DEPTHS[d as usize]) {
            Some((fs, ss)) => {
                fnv(&mut h, &hsurf(&fs, canvas).to_le_bytes());
                fnv(&mut h, &hsurf(&ss, canvas).to_le_bytes());
            }
            None => fnv(&mut h, b"none"),
        }
    }
    println!("fn\tshape_split\t{h:016x}");
}

pub fn run() {
    adjust_sweeps();
    gradient_sweeps();
    pattern_sweeps();
    split_sweeps();
    for seed in 1..=400u64 {
        doc_line(seed);
    }
    r9_sweeps();
}

/// R9 (resync): functionality added upstream after C2, appended so earlier lines keep their
/// positions: special-eight Fill blending over every mode (incl. the Paint.NET ones), pencil
/// Curves, contour corners, Lab-document adjustments and native CMYK flattening.
fn r9_sweeps() {
    use photocraft_compose::psblend;
    let modes: Vec<BlendMode> = BlendMode::layer_modes().collect();
    let mut h = H0;
    let mut hc = H0;
    for (flag, (lab, hdr)) in [(false, false), (true, false), (false, true)].into_iter().enumerate() {
        psblend::with_lab_mix(lab, || {
            psblend::with_hdr(hdr, || {
                for &m in &modes {
                    for i in 0..16usize {
                        for j in [0usize, 3, 7, 11, 12, 13, 15] {
                            let b = [VALS[i], VALS[j], VALS[(i + j) % 16], [1.0, 0.5, 0.0, 0.75][flag % 4]];
                            let t = [VALS[j], VALS[(i * 3) % 16], VALS[i], [1.0, 0.25, 1.0, 0.6][i % 4]];
                            for fill in [0.0f32, 0.3, 0.5, 1.0, f32::NAN] {
                                for v in psblend::blend_rgb_fill(m, [b[0], b[1], b[2]], [t[0], t[1], t[2]], fill) {
                                    hf(&mut h, v);
                                }
                                for (op, g) in [(1.0f32, 1.0f32), (0.6, 1.0), (0.8, 1.45)] {
                                    for v in psblend::composite_fill(m, b, t, op, fill, g) {
                                        hf(&mut h, v);
                                    }
                                }
                            }
                            for v in psblend::composite(m, b, t, 0.7) {
                                hf(&mut hc, v);
                            }
                        }
                    }
                }
            })
        });
    }
    println!("fn\tr9_fill_blend\t{h:016x}");
    println!("fn\tr9_paint_net_composite\t{hc:016x}");

    let mut hp = H0;
    let mut hk = H0;
    for seed in 1..=300u64 {
        let mut r = Rng(seed.wrapping_mul(0x9e3779b97f4a7c15) | 1);
        let pts = curve(&mut r);
        for v in adjust::curve_map_lut(&pts) {
            hf(&mut hp, v);
        }
        let corners: Vec<bool> = (0..pts.len() + r.below(2) as usize).map(|_| r.below(2) == 0).collect();
        for v in adjust::contour_curve_lut(&pts, &corners) {
            hf(&mut hk, v);
        }
    }
    println!("fn\tr9_curve_map_lut\t{hp:016x}");
    println!("fn\tr9_contour_curve_lut\t{hk:016x}");

    let depths = [None, Some(SampleType::U8), Some(SampleType::U16), Some(SampleType::F32)];
    let mut ha = H0;
    let mut hl = H0;
    for seed in 1..=300u64 {
        let mut r = Rng(seed.wrapping_mul(0x2545f4914f6cdd1d) | 1);
        let mut a = adjustment(&mut r);
        if let Adjustment::Curves { pencil, .. } = &mut a {
            *pencil = r.below(2) == 0;
        }
        for d in depths {
            let mut b = test_buffer();
            adjust::apply_in(&a, &mut b, Transfer::Srgb, d, ColorMode::Lab);
            fnv(&mut ha, &hpx(&b.px).to_le_bytes());
        }
        if matches!(a, Adjustment::Curves { .. }) {
            for row in adjust::tone_luts(&a).iter() {
                for v in row {
                    hf(&mut hl, *v);
                }
            }
        }
    }
    println!("fn\tr9_apply_in_lab\t{ha:016x}");
    println!("fn\tr9_pencil_tone_luts\t{hl:016x}");

    for seed in 1..=40u64 {
        let mut r = Rng(seed.wrapping_mul(0x51_7cc1_b727_220a) | 1);
        let depth = if r.below(2) == 0 { SampleType::U8 } else { SampleType::U16 };
        let mut doc = Document::new("n", Size::new(1 + r.below(40) as u32, 1 + r.below(30) as u32), ColorMode::Cmyk, depth);
        let fmt = PixelFormat::new(ColorMode::Cmyk, depth, true);
        for _ in 0..1 + r.below(3) {
            let mut l = Layer::raster("ink", fmt);
            let x0 = r.below(30) as i32 - 5;
            let y0 = r.below(20) as i32 - 5;
            let px = [r.unit(), r.unit(), r.unit(), r.unit(), [1.0, 0.5, 0.0, 0.3][r.below(4) as usize]];
            l.surface_mut().unwrap().fill_rect(Rect::new(x0, y0, x0 + 1 + r.below(30) as i32, y0 + 1 + r.below(20) as i32), &px);
            l.opacity = [1.0, 0.6, 0.0][r.below(3) as usize];
            l.fill_opacity = [1.0, 0.5][r.below(2) as usize];
            l.visible = r.below(6) != 0;
            if r.below(8) == 0 {
                l.blend = BlendMode::Multiply;
            }
            doc.layers.push(l);
        }
        let paper = r.below(2) == 0;
        let rect = Rect::new(-2, -1, doc.size.width as i32 + 3, doc.size.height as i32 + 1);
        let out = match photocraft_compose::native::flatten_cmyk(&doc, rect, fmt, paper) {
            Some(s) => {
                let mut h = H0;
                let mut v = Vec::new();
                s.read_region_into(rect, &mut v);
                for x in v {
                    hf(&mut h, x);
                }
                format!("{h:016x}\t{}", rs(s.content_bounds()))
            }
            None => "none".into(),
        };
        println!("native\t{seed}\t{out}");
    }
}
