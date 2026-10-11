//! C1 compose core reference vectors (`rustref compose-core-vectors`), checked line by line by
//! tests/compose/test_core_parity.c. Seeded synthetic documents (every colour mode and depth,
//! mixed layer formats, every blend mode, opacity/fill incl. out-of-range values, groups with
//! pass-through and nesting, clipping groups, pixel masks with density/feather/disabled, vector
//! masks, solid fills, type layers (text gamma), artboards, channel restrictions, Blend If, spot
//! channels, selections) are built identically in both languages from an xorshift stream, and
//! every composite is hashed (FNV-1a over f32 bits, little-endian). Documents avoid the features
//! of C2 / C3 (adjustment layers, gradient / pattern fills, effects, knockouts, Blend Clipped
//! Layers as Group off, stroked shapes). Lines, tab-separated:
//!
//!   blend NAME HASH          psblend sweeps (blend_rgb, composite, composite_gamma, flags)
//!   layout NAME HASH         fill_layout / gradient_t sweeps
//!   misc NAME VALUE          band_rows_for, feather_sigma
//!   doc SEED K=HASH ...      renders of one document
//!   q SEED I K=V ...         queries on its top-level layer I
use photocraft_compose as c;
use photocraft_doc::{
    AlphaChannel, Artboard, ArtboardBackground, BlendMode, BlendRange, Color, ColorMode, Document, Fill, Layer,
    LayerContent, LayerMask, Path, PixelFormat, Rect, SampleType, ShapeLayer, Size, Subpath, Surface, TextLayer, VectorMask,
};

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
/// NaN sign and payload depend on operand order inside an expression (LLVM may commute an
/// addition), not on the formula, so every NaN hashes as the canonical quiet NaN.
fn hf(h: &mut u64, v: f32) {
    let bits = if v.is_nan() { 0x7fc0_0000 } else { v.to_bits() };
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

fn layer_format(r: &mut Rng, doc_fmt: PixelFormat) -> PixelFormat {
    match r.below(8) {
        0..=2 => doc_fmt,
        3 => PixelFormat::RGBA8,
        4 => PixelFormat::RGBA16,
        5 => PixelFormat::RGBA32F,
        6 => PixelFormat::new(ColorMode::Grayscale, DEPTHS[r.below(3) as usize], true),
        _ => PixelFormat::new([ColorMode::Cmyk, ColorMode::Lab][r.below(2) as usize], DEPTHS[r.below(3) as usize], true),
    }
}

fn pixel(r: &mut Rng, fmt: PixelFormat) -> Vec<f32> {
    let n = fmt.channels();
    let hdr = fmt.sample == SampleType::F32 && r.below(4) == 0;
    (0..n).map(|_| if hdr { r.unit() * 2.0 } else { r.unit() }).collect()
}

fn paint(r: &mut Rng, s: &mut Surface, w: i32, h: i32) {
    let fmt = s.format();
    for _ in 0..1 + r.below(3) {
        let x0 = r.range(-4, w + 4);
        let y0 = r.range(-4, h + 4);
        let x1 = x0 + r.range(1, w + 2);
        let y1 = y0 + r.range(1, h + 2);
        let px = pixel(r, fmt);
        s.fill_rect(Rect::new(x0, y0, x1, y1), &px);
    }
    for _ in 0..r.below(4) {
        let x = r.range(0, w);
        let y = r.range(0, h);
        let px = pixel(r, fmt);
        s.write_pixel(x, y, &px);
    }
}

fn gen_layers(r: &mut Rng, doc_fmt: PixelFormat, w: i32, h: i32, level: u32, n: u64) -> Vec<Layer> {
    let mut v = Vec::new();
    for i in 0..n {
        let mut l = gen_layer(r, doc_fmt, w, h, level);
        if i > 0 && r.below(4) == 0 {
            l.clipped = true;
        }
        v.push(l);
    }
    v
}

fn gen_layer(r: &mut Rng, doc_fmt: PixelFormat, w: i32, h: i32, level: u32) -> Layer {
    let kind = r.below(10);
    let mut l = match kind {
        6 | 7 if level < 3 => {
            let n = 1 + r.below(3);
            let kids = gen_layers(r, doc_fmt, w, h, level + 1, n);
            let mut g = Layer::group("g", kids);
            if r.below(2) == 0 {
                g.blend = BlendMode::Normal;
            }
            if r.below(5) == 0 {
                let LayerContent::Group(gr) = &mut g.content else { unreachable!() };
                let x0 = r.range(-2, w);
                let y0 = r.range(-2, h);
                let rect = Rect::new(x0, y0, x0 + r.range(1, w + 2), y0 + r.range(1, h + 2));
                let mut ab = Artboard::new(rect);
                ab.background = match r.below(4) {
                    0 => ArtboardBackground::White,
                    1 => ArtboardBackground::Black,
                    2 => ArtboardBackground::Transparent,
                    _ => ArtboardBackground::Custom(Color::rgba(r.unit(), r.unit(), r.unit(), r.unit())),
                };
                gr.artboard = Some(ab);
            }
            g
        }
        8 => {
            let c = Color::rgba(r.unit(), r.unit(), r.unit(), if r.below(2) == 0 { 1.0 } else { r.unit() });
            Layer::new("fill", LayerContent::Fill(Fill::Solid(c)))
        }
        9 if r.below(2) == 0 => {
            let mut s = Surface::new(PixelFormat::RGBA8);
            paint(r, &mut s, w, h);
            let mut t = TextLayer::default();
            t.cache = Some(s);
            Layer::new("type", LayerContent::Text(t))
        }
        9 => {
            // A filled shape without a stroke: its fill path shapes layer_shape (effect_outline).
            let k = 3 + r.below(2);
            let pts: Vec<(f64, f64)> =
                (0..k).map(|_| (f64::from(r.range(-2, w + 2)) + 0.5, f64::from(r.range(-2, h + 2)) + 0.25)).collect();
            let mut path = Path::new(vec![Subpath::polygon(&pts)]);
            path.inverted = r.below(5) == 0;
            let mut s = Surface::new(PixelFormat::RGBA8);
            paint(r, &mut s, w, h);
            let c = Color::rgba(r.unit(), r.unit(), r.unit(), r.unit());
            let sh = ShapeLayer { path, fill: if r.below(4) == 0 { None } else { Some(Fill::Solid(c)) }, cache: Some(s), ..Default::default() };
            Layer::new("shape", LayerContent::Shape(sh))
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
        l.blend = BlendMode::LAYER_MODES[r.below(27) as usize];
    }
    l.opacity = opacity(r);
    l.fill_opacity = opacity(r);
    if r.below(4) == 0 {
        let mut m = if r.below(2) == 0 { LayerMask::reveal_all() } else { LayerMask::hide_all() };
        let x0 = r.range(-2, w);
        let y0 = r.range(-2, h);
        let v = r.unit();
        m.surface.fill_rect(Rect::new(x0, y0, x0 + r.range(1, w), y0 + r.range(1, h)), &[v]);
        m.density = [1.0, 1.0, 0.5, r.unit()][r.below(4) as usize];
        m.feather = [0.0, 0.0, 1.5, 4.0][r.below(4) as usize];
        m.enabled = r.below(5) != 0;
        l.mask = Some(m);
    }
    if r.below(5) == 0 {
        let k = 3 + r.below(3);
        let pts: Vec<(f64, f64)> =
            (0..k).map(|_| (f64::from(r.range(-3, w + 3)) + 0.25 * r.below(4) as f64, f64::from(r.range(-3, h + 3)) + 0.5)).collect();
        let mut p = Path::new(vec![Subpath::polygon(&pts)]);
        p.inverted = r.below(4) == 0;
        let mut vm = VectorMask::new(p);
        vm.density = [1.0, 0.6][r.below(2) as usize];
        vm.feather = [0.0, 0.0, 2.0][r.below(3) as usize];
        vm.enabled = r.below(5) != 0;
        l.vector_mask = Some(vm);
    }
    if r.below(6) == 0 {
        l.excluded_channels = r.below(8) as u32;
    }
    if r.below(6) == 0 {
        let rng = |r: &mut Rng| {
            let b0 = r.below(200) as u8;
            let b1 = b0 + r.below(40) as u8;
            let w0 = 120 + r.below(100) as u8;
            let w1 = w0 + r.below(36) as u8;
            BlendRange { black: [b0, b1], white: [w0, w1] }
        };
        let i = r.below(4) as usize;
        let a = if r.below(2) == 0 { rng(r) } else { BlendRange::FULL };
        let b = if r.below(2) == 0 { rng(r) } else { BlendRange::FULL };
        l.blend_if.set(i, [a, b]);
    }
    if r.below(6) == 0 {
        l.advanced.transparency_shapes = false;
        l.advanced.blend_interior = true;
    }
    l
}

fn gen_doc(seed: u64) -> Document {
    let mut r = Rng(seed.wrapping_mul(0x9e3779b97f4a7c15) | 1);
    let mode = MODES[r.below(6) as usize];
    let depth = DEPTHS[r.below(3) as usize];
    let w = 9 + r.below(40) as u32;
    let h = 7 + r.below(30) as u32;
    let mut doc = Document::new("v", Size::new(w, h), mode, depth);
    let fmt = doc.pixel_format();
    let n = 1 + r.below(5);
    doc.layers = gen_layers(&mut r, fmt, w as i32, h as i32, 0, n);
    for _ in 0..r.below(3) {
        let mut s = Surface::new(PixelFormat::new(ColorMode::Grayscale, depth, false));
        paint(&mut r, &mut s, w as i32, h as i32);
        let mut ch = AlphaChannel::new("ink", s);
        if r.below(4) != 0 {
            ch.spot = Some((Color::rgb(r.unit(), r.unit(), r.unit()), [0.0, 1.0, r.unit()][r.below(3) as usize]));
        }
        doc.channels.push(ch);
    }
    if r.below(4) == 0 {
        let mut s = Surface::new(PixelFormat::GRAY8);
        paint(&mut r, &mut s, w as i32, h as i32);
        doc.selection = Some(s);
    }
    doc
}

fn doc_line(seed: u64) {
    let doc = gen_doc(seed);
    let mut r = Rng(seed ^ 0x5bd1e995);
    let b = doc.bounds();
    let (w, h) = (b.width(), b.height());
    let mut out = format!("doc\t{seed}");
    let flat = c::flatten(&doc);
    out += &format!("\tflat={:016x}", hbuf(&flat));
    let tiled = c::render_tiled(&doc, b, 7);
    out += &format!("\ttiled={:016x}", hbuf(&tiled));
    let x0 = r.range(-6, w as i32);
    let y0 = r.range(-6, h as i32);
    let sub = Rect::new(x0, y0, x0 + r.range(1, w as i32 + 8), y0 + r.range(1, h as i32 + 8));
    out += &format!("\tsub={:016x}", hbuf(&c::render_tiled(&doc, sub, 5)));
    let (rw, rh) = (1 + r.below(w as u64) as u32, 1 + r.below(h as u64) as u32);
    out += &format!("\treduced={:016x}", hbuf(&c::render_reduced(&doc, rw, rh)));
    let dx = r.range(-3, w as i32);
    let dy = r.range(-3, h as i32);
    let dmg = Rect::new(dx, dy, dx + r.range(1, 9), dy + r.range(1, 9));
    out += &format!("\tdamage={:016x}", hbuf(&c::render_reduced_damage(&doc, rw, rh, dmg)));
    let area = Rect::new(-5, -4, w as i32 + 6, h as i32 + 3);
    out += &format!("\tarea={:016x}", hbuf(&c::render_reduced_rect(&doc, area, (w + 11) / 3, (h + 7) / 2)));
    let t = c::thumbnail(&doc, 1 + r.below(20) as u32);
    let mut ht = H0;
    fnv(&mut ht, &t.width.to_le_bytes());
    fnv(&mut ht, &t.height.to_le_bytes());
    fnv(&mut ht, &t.pixels);
    out += &format!("\tthumb={ht:016x}");
    let bg = if r.below(2) == 0 { Some([r.unit(), r.unit(), r.unit()]) } else { None };
    let fmt = [doc.pixel_format(), PixelFormat::RGBA8, PixelFormat::RGBA32F][r.below(3) as usize];
    let s = c::flatten_to_surface(&doc, fmt, bg);
    let mut hs = H0;
    fnv(&mut hs, &s.to_interleaved(b));
    fnv(&mut hs, &(s.tile_count() as u64).to_le_bytes());
    out += &format!("\tsurface={hs:016x}");
    let p = c::proxy::proxy_document(&doc, 2);
    out += &format!("\tproxy={:016x}\tfaithful={}", hbuf(&c::flatten(&p)), c::proxy::proxy_faithful(&doc) as u8);
    let mut hl = H0;
    for l in &doc.layers {
        fnv(&mut hl, &hbuf(&c::render_layer(l, b)).to_le_bytes());
    }
    out += &format!("\tlayers={hl:016x}");
    if doc.mode == ColorMode::Multichannel {
        out += &format!("\tinks={:016x}", hbuf(&c::multichannel::inks(&doc, b)));
    }
    println!("{out}");
    for (i, l) in doc.layers.iter().enumerate() {
        let mut q = format!("q\t{seed}\t{i}");
        q += &format!("\tbounds={}", rs(c::layer_bounds(l, b)));
        q += &format!("\tcomp={}", c::composite_bounds(l, b).map_or("none".into(), rs));
        q += &format!("\tchange={}", c::change_bounds(l, b).map_or("none".into(), rs));
        q += &format!("\tto={}\tocc={}", c::transparent_outside(l) as u8, c::occludes_below(l, doc.mode) as u8);
        q += &format!("\tcw={}", c::channel_weights(l, doc.mode).map_or("none".into(), |w| format!("{:?}", w)));
        q += &format!("\tbi={}\tframe={}", c::blend_if_active(l, doc.mode) as u8, rs(c::fill_frame(l, b)));
        q += &format!("\tgamma={:08x}", c::text_gamma(l).to_bits());
        q += &format!("\tfeather={}", c::masks::has_feather(l) as u8);
        if let Some(m) = c::masks::combined_mask(l, b) {
            let mut v = Vec::new();
            m.read_region_into(b, &mut v);
            let mut hm = H0;
            for x in &v {
                hf(&mut hm, *x);
            }
            q += &format!("\tmask={hm:016x}");
        }
        let shape = c::layer_shape(&doc, l, b);
        let mut hsh = H0;
        for x in &shape {
            hf(&mut hsh, *x);
        }
        q += &format!("\tshape={hsh:016x}");
        println!("{q}");
    }
}

fn blend_sweeps() {
    let vals = [0.0f32, -0.0, 1e-5, 0.2, 0.25, 0.5, 0.6, 0.75, 1.0 - 1e-5, 1.0, 1.5, -0.1, f32::NAN, f32::INFINITY];
    let alphas = [0.0f32, 0.3, 1.0, 0.7];
    let mut hr = H0;
    let mut hr_hdr = H0;
    for m in BlendMode::LAYER_MODES {
        for &a in &vals {
            for &b in &vals {
                let cb = [a, b, 0.4];
                let cs = [b, 0.6, a];
                for v in c::psblend::blend_rgb(m, cb, cs) {
                    hf(&mut hr, v);
                }
                c::psblend::HDR.with(|f| f.set(true));
                for v in c::psblend::blend_rgb(m, cb, cs) {
                    hf(&mut hr_hdr, v);
                }
                c::psblend::HDR.with(|f| f.set(false));
            }
        }
    }
    println!("blend\trgb\t{hr:016x}");
    println!("blend\trgb_hdr\t{hr_hdr:016x}");
    for (name, lab, hdr) in [("comp", false, false), ("comp_lab", true, false), ("comp_hdr", false, true)] {
        c::psblend::LAB_MIX.with(|f| f.set(lab));
        c::psblend::HDR.with(|f| f.set(hdr));
        let mut hc = H0;
        let mut hg = H0;
        for m in BlendMode::LAYER_MODES {
            for &a in &vals {
                for &b in &vals {
                    for &aa in &alphas {
                        for &ab in &alphas {
                            for op in [1.0f32, 0.5, 0.0] {
                                let bd = [a, 0.3, b, ab];
                                let sr = [b, a, 0.8, aa];
                                for v in c::psblend::composite(m, bd, sr, op) {
                                    hf(&mut hc, v);
                                }
                                for g in [1.45f32, 2.2] {
                                    for v in c::psblend::composite_gamma(m, bd, sr, op, g) {
                                        hf(&mut hg, v);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        c::psblend::LAB_MIX.with(|f| f.set(false));
        c::psblend::HDR.with(|f| f.set(false));
        println!("blend\t{name}\t{hc:016x}");
        println!("blend\t{name}_gamma\t{hg:016x}");
    }
    let mut he = H0;
    for &v in &vals {
        for g in [1.0f32, 1.45, 2.2] {
            hf(&mut he, c::psblend::text_encode(v, g));
            hf(&mut he, c::psblend::text_decode(v, g));
        }
    }
    println!("blend\ttext_codec\t{he:016x}");
}

fn layout_sweeps() {
    use photocraft_doc::GradientStyle as G;
    let styles = [G::Linear, G::Radial, G::Angle, G::Reflected, G::Diamond];
    let angles = [0.0f32, 30.0, 45.0, 87.0, 90.0, -90.0, 135.0, 200.0, 1e30, f32::NAN];
    let scales = [1.0f32, 0.5, 1.5, 0.0, f32::INFINITY];
    let frames = [Rect::new(0, 0, 4, 4), Rect::new(0, 0, 7, 5), Rect::new(-5, 3, 40, 17), Rect::new(10, 10, 210, 210), Rect::new(3, 3, 3, 9)];
    let offsets = [(0.0f32, 0.0f32), (0.25, -0.1), (f32::NAN, 0.0)];
    let mut hl = H0;
    let mut ht = H0;
    let mut hg = H0;
    for s in styles {
        for &a in &angles {
            for &sc in &scales {
                for &f in &frames {
                    for &o in &offsets {
                        let (aa, ss, oo) = c::fill_layout::gradient_layout(s, a, sc, o, f);
                        for v in [aa, ss, oo.0, oo.1] {
                            hf(&mut hl, v);
                        }
                        for (x, y) in [(0.5f32, 0.5f32), (2.5, 1.5), (13.5, 8.5), (-3.0, 40.0)] {
                            for rev in [false, true] {
                                hf(&mut hg, c::effects::gradient_t(s, a, sc, rev, o, f, x, y));
                            }
                        }
                    }
                    for (x, y) in [(0.5f32, 0.5f32), (3.5, 2.5), (20.5, 10.5)] {
                        hf(&mut ht, c::fill_layout::fill_gradient_t(s, a, sc, false, f, x, y));
                    }
                }
            }
        }
    }
    println!("layout\tgradient_layout\t{hl:016x}");
    println!("layout\tfill_gradient_t\t{ht:016x}");
    println!("layout\tgradient_t\t{hg:016x}");
    let mut hu = H0;
    for &a in &angles {
        for (w, h) in [(1.0f32, 1.0f32), (600.0, 60.0), (29.0, 29.0), (0.5, 3.0)] {
            let (ch, len) = c::effects::gradient_units(a, w, h);
            hf(&mut hu, ch);
            hf(&mut hu, len);
        }
    }
    println!("layout\tgradient_units\t{hu:016x}");
}

fn misc() {
    for (w, req) in [(1u32, 0i32), (0, -5), (14_000, 0), (37, 100), (100_000, 0), (u32::MAX, 0), (5, i32::MAX), (5, 1)] {
        println!("misc\tband_rows_for({w},{req})\t{}", c::band_rows_for(w, req));
    }
    for f in [0.0f32, 1.5, 4.0, 1e6, -3.0, f32::NAN, f32::INFINITY] {
        println!("misc\tfeather_sigma({f:?})\t{:08x}", c::masks::feather_sigma(f).to_bits());
    }
}

pub fn run() {
    blend_sweeps();
    layout_sweeps();
    misc();
    for seed in 1..=400u64 {
        doc_line(seed);
    }
}
