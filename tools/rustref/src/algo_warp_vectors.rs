//! L11 algo Liquify / Puppet Warp reference vectors (`rustref algo-warp-vectors`). One line per
//! case, fields separated by tabs: the case kind, its inputs (JSON as written below, or f64 bit
//! patterns in hex for values JSON can't hold) and the result (FNV-1a hash of the outputs' bytes,
//! f32/f64 bits little-endian, or plain text). Replayed by tests/algo/test_warp_parity.c, which
//! reads the inputs from the golden file itself.
use photocraft_algo::liquify::{LiquifyField, LiquifyStroke, LiquifyTool, ProxyImage, apply_liquify, auto_cell};
use photocraft_algo::puppet::{
    PuppetDensity, PuppetMesh, PuppetMode, PuppetPin, PuppetSolver, PuppetWarp, build_mesh, deform, puppet_warp,
};
use photocraft_algo::transform::Interp;
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
// NaNs hash as one canonical value: their sign and payload depend on how the compiler orders
// commutative operands, which neither Rust nor C pins down.
fn hf(h: &mut u64, v: &[f32]) {
    for x in v {
        let b = if x.is_nan() { 0x7fc0_0000 } else { x.to_bits() };
        fnv(h, &b.to_le_bytes());
    }
}
fn hd(h: &mut u64, v: &[f64]) {
    for x in v {
        let b = if x.is_nan() { 0x7ff8_0000_0000_0000 } else { x.to_bits() };
        fnv(h, &b.to_le_bytes());
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
fn hx(v: f64) -> String {
    format!("{:016x}", v.to_bits())
}
fn rect_s(r: Rect) -> String {
    format!("{},{},{},{}", r.x0, r.y0, r.x1, r.y1)
}

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

fn format_named(name: &str) -> PixelFormat {
    match name {
        "rgba8" => PixelFormat::new(ColorMode::Rgb, SampleType::U8, true),
        "rgba16" => PixelFormat::new(ColorMode::Rgb, SampleType::U16, true),
        "rgba32" => PixelFormat::new(ColorMode::Rgb, SampleType::F32, true),
        "rgb8" => PixelFormat::new(ColorMode::Rgb, SampleType::U8, false),
        "gray8" => PixelFormat::new(ColorMode::Grayscale, SampleType::U8, false),
        "graya16" => PixelFormat::new(ColorMode::Grayscale, SampleType::U16, true),
        "cmyka8" => PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true),
        _ => PixelFormat::new(ColorMode::Lab, SampleType::F32, true),
    }
}
const FORMATS: [&str; 8] = ["rgba8", "rgba16", "rgba32", "rgb8", "gray8", "graya16", "cmyka8", "laba32"];

fn hfield(h: &mut u64, f: &LiquifyField) {
    hd(h, &[f.cell]);
    hu(h, &[f.w as u64, f.h as u64]);
    for v in &f.d {
        hf(h, v);
    }
    hf(h, &f.freeze);
}

/// Stroke lists (JSON) replayed on fields; each is run on every bounds/cell combination.
const STROKES: &[&str] = &[
    r#"[]"#,
    r#"[{"tool":"forwardWarp","size":40,"points":[[40,32],[52,32]]}]"#,
    r#"[{"tool":"forwardWarp","size":30,"points":[[20,20,1],[60,40,0.5],[61,40.5],[70,20,0.25]]}]"#,
    r#"[{"tool":"pushLeft","size":40,"points":[[48,44],[48,20]]}]"#,
    r#"[{"tool":"twirlCw","size":50,"points":[[48,32,0.5],[50,33,1.0],[52,34]]}]"#,
    r#"[{"tool":"twirlCcw","size":40,"rate":60,"points":[[48,32],[48,32],[48,32]]}]"#,
    r#"[{"tool":"pucker","size":40,"points":[[48,32],[48,32],[48,32],[48,32]]}]"#,
    r#"[{"tool":"bloat","size":40,"density":0,"points":[[48,32],[49,32],[50,33]]}]"#,
    r#"[{"tool":"forwardWarp","size":40,"points":[[30,32],[45,32]]},{"tool":"reconstruct","size":80,"points":[[40,32],[40,32],[40,32],[40,32],[40,32]]}]"#,
    r#"[{"tool":"forwardWarp","size":12,"points":[[40,32],[50,32]]},{"tool":"smooth","size":60,"points":[[45,32],[45,32],[45,32],[46,32]]}]"#,
    r#"[{"tool":"freeze","size":60,"density":0,"points":[[40,32]]},{"tool":"forwardWarp","size":40,"points":[[30,32],[45,32]]},{"tool":"thaw","size":30,"points":[[40,32],[44,30]]}]"#,
    r#"[{"tool":"forwardWarp","size":40,"points":[[30,32],[45,32]]},{"tool":"reconstructAll","amount":40},{"tool":"freezeAll"},{"tool":"invertFreeze"},{"tool":"freeze","size":20,"points":[[40,30]]},{"tool":"invertFreeze"},{"tool":"forwardWarp","size":50,"points":[[35,30],[50,40]]},{"tool":"thawAll"},{"tool":"reconstructAll"}]"#,
    r#"[{"tool":"lassoMask","points":[[10,8],[70,8],[70,20],[30,22.5],[33,40],[80,41],[60,58],[9,55]]},{"tool":"forwardWarp","size":60,"points":[[20,30],[60,35]]}]"#,
    r#"[{"tool":"freezeAll"},{"tool":"lassoMask","amount":0,"points":[[20,20],[40,20],[40,40],[20,40]]},{"tool":"bloat","size":80,"points":[[30,30],[30,30]]}]"#,
    r#"[{"tool":"lassoMask","points":[[-1e15,-1e15],[1e15,-1e15],[1e15,1e15],[-1e15,1e15]]}]"#,
    r#"[{"tool":"lassoMask","points":[[10,1e12],[50,1e12],[30,2e12]]},{"tool":"lassoMask","points":[[10,10],[50,50]]}]"#,
    r#"[{"tool":"forwardWarp","size":30,"points":[[0,0],[1e15,0]]},{"tool":"bloat","size":30,"points":[[5,-1e15],[5,1e15]]}]"#,
    r#"[{"tool":"forwardWarp","size":30,"points":[[-1e308,5],[1e308,5]]},{"tool":"pucker","size":2,"pressure":50,"points":[[12.3,7.7],[12.3,7.7,0.3]]}]"#,
    r#"[{"tool":"forwardWarp","size":15000,"points":[[40,30],[41,30]]},{"tool":"twirlCw","size":1,"points":[[40,30]]}]"#,
    r#"[{"tool":"forwardWarp","size":40,"pressure":0,"points":[[40,30],[60,30]]},{"tool":"bloat","size":40,"pressure":250,"rate":-5,"density":300,"points":[[40,30,7],[40,30,-1]]}]"#,
    r#"[{"tool":"forwardWarp","size":40,"points":[[40,30]]},{"tool":"bloat","size":40,"points":[[40],[40,30,0.5,9],[41,31]]}]"#,
    r#"[{"tool":"forwardWarp","size":200,"points":[[-50,-40],[150,120],[-20,140]]},{"tool":"smooth","size":200,"rate":100,"points":[[40,30],[40,30]]}]"#,
    r#"[{"tool":"reconstruct","size":40,"points":[[40,30],[40,30]]},{"tool":"thaw","size":40,"points":[[40,30]]}]"#,
];
const BOUNDS: [Rect; 3] = [Rect::new(0, 0, 96, 64), Rect::new(-7, 3, 89, 67), Rect::new(5, 5, 6, 9)];
const CELLS: [f64; 4] = [1.0, 2.0, 3.5, 64.0];

fn strokes_of(js: &str) -> Vec<LiquifyStroke> {
    serde_json::from_str(js).expect("valid stroke json")
}

fn field_case(b: Rect, cell: f64, js: &str) {
    let strokes = strokes_of(js);
    let mut f = LiquifyField::new(b, cell);
    let mut h = H0;
    for s in &strokes {
        hrect(&mut h, f.apply_stroke(s));
    }
    hfield(&mut h, &f);
    // Sampling over a grid of points, in and outside the field.
    for j in -3..=12 {
        for i in -3..=16 {
            let (x, y) = (f64::from(b.x0) + i as f64 * 7.3 - 2.0, f64::from(b.y0) + j as f64 * 6.1 - 2.5);
            hd(&mut h, &f.sample(x, y));
            hf(&mut h, &[f.freeze_at(x, y)]);
        }
    }
    hf(&mut h, &[f.max_displacement()]);
    fnv(&mut h, &[f.is_identity() as u8]);
    println!("field\t{}\t{}\t{}\t{:016x}", rect_s(b), hx(cell), js, h);
}

/// Hostile segment walks (values JSON can't carry): tool index, size, a, b as f64 bits.
fn segment_cases() {
    let n = f64::NAN;
    let inf = f64::INFINITY;
    let pts: [([f64; 3], [f64; 3]); 12] = [
        ([10.0, 20.0, 1.0], [70.0, 50.0, 0.5]),
        ([-900.0, 30.0, 1.0], [40.0, 31.0, 1.0]),
        ([40.0, 30.0, 1.0], [40.0, 4000.0, 0.2]),
        ([-500.0, -500.0, 1.0], [600.0, 600.0, 1.0]),
        ([-300.0, -9.0, 1.0], [300.0, -9.0, 1.0]),
        ([88.5, -50.0, 1.0], [88.5, 120.0, 1.0]),
        ([0.0, 0.0, 1.0], [0.25, 0.1, 1.0]),
        ([n, 10.0, 1.0], [20.0, 10.0, 1.0]),
        ([10.0, 10.0, n], [20.0, 12.0, 1.0]),
        ([1e18, 5.0, 1.0], [-1e18, 5.0, 1.0]),
        ([30.0, 30.0, 1.0], [30.0, 30.0, 1.0]),
        ([30.0, 30.0, 1.0], [1e300, 30.0, 0.5]),
    ];
    let tools = [
        LiquifyTool::ForwardWarp,
        LiquifyTool::Reconstruct,
        LiquifyTool::Smooth,
        LiquifyTool::TwirlCw,
        LiquifyTool::Pucker,
        LiquifyTool::Bloat,
        LiquifyTool::PushLeft,
        LiquifyTool::Freeze,
        LiquifyTool::Thaw,
        LiquifyTool::LassoMask,
        LiquifyTool::FreezeAll,
    ];
    let sizes = [6.0, 40.0, n, inf, 0.0, 1e9];
    for (ti, &tool) in tools.iter().enumerate() {
        for &size in &sizes {
            for (a, b) in pts {
                // The reference panics on a dab centred past ~1e19 px whose brush reaches the
                // first field rows (a wrapped snapshot index); the C port returns nothing there.
                if a[0] == 30.0 && b[0] == 1e300 && !(size < 100.0) {
                    continue;
                }
                let mut f = LiquifyField::new(Rect::new(-7, 3, 89, 67), 2.0);
                // Some displacement first, so Reconstruct and Smooth have something to undo.
                f.stroke_segment(&LiquifyStroke::new(LiquifyTool::ForwardWarp, 30.0), [20.0, 20.0, 1.0], [60.0, 40.0, 1.0]);
                let s = LiquifyStroke::new(tool, size);
                let mut h = H0;
                hrect(&mut h, f.stroke_begin(&s, a));
                hrect(&mut h, f.stroke_segment(&s, a, b));
                hfield(&mut h, &f);
                println!(
                    "segment\t{}\t{}\t{},{},{}\t{},{},{}\t{:016x}",
                    ti,
                    hx(size),
                    hx(a[0]),
                    hx(a[1]),
                    hx(a[2]),
                    hx(b[0]),
                    hx(b[1]),
                    hx(b[2]),
                    h
                );
            }
        }
    }
}

fn apply_cases() {
    let js = [
        r#"[{"tool":"forwardWarp","size":40,"points":[[40,32],[52,32]]}]"#,
        r#"[{"tool":"twirlCw","size":50,"points":[[48,32,0.5],[50,33,1.0],[52,34]]},{"tool":"bloat","size":30,"points":[[20,20],[20,20]]}]"#,
        r#"[{"tool":"forwardWarp","size":200,"points":[[-50,-40],[150,120]]}]"#,
        r#"[{"tool":"pucker","size":30,"points":[[300,260],[300,260],[300,260]]},{"tool":"forwardWarp","size":60,"points":[[250,250],[270,280]]}]"#,
        r#"[]"#,
    ];
    let bounds = [Rect::new(0, 0, 96, 64), Rect::new(-3, -5, 330, 290)];
    for name in FORMATS {
        let fmt = format_named(name);
        for (bi, &b) in bounds.iter().enumerate() {
            for opaque in [false, true] {
                let src = surf(fmt, Rect::new(b.x0 + 2, b.y0 + 1, b.x1 - 3, b.y1 - 2), bi as i32 + 3, opaque);
                for j in js {
                    let f = LiquifyField::from_strokes(b, auto_cell(b), &strokes_of(j));
                    println!("apply\t{}\t{}\t{}\t{}\t{:016x}", name, rect_s(b), opaque as u8, j, hs(&apply_liquify(&src, &f)));
                }
            }
        }
    }
}

/// R6 (resync): ProxyImage::render_full_resolution, a full render then a partial update.
fn fullres_cases() {
    let js = r#"[{"tool":"bloat","size":40,"points":[[48,32],[48,32],[48,32],[48,32]]},{"tool":"forwardWarp","size":30,"points":[[20,20],[30,25],[60,40]]}]"#;
    let canvas = Rect::new(0, 0, 96, 64);
    for name in ["rgba8", "rgb8", "graya16", "cmyka8", "rgba32"] {
        let fmt = format_named(name);
        let src = surf(fmt, Rect::new(-10, -10, 140, 90), 5, false);
        let f = LiquifyField::from_strokes(canvas, 2.0, &strokes_of(js));
        for (bounds, region) in [
            (canvas, canvas),
            (canvas, Rect::new(10, 12, 50, 40)),
            (Rect::new(-5, 3, 131, 80), Rect::new(0, 0, 200, 200)),
            (canvas, Rect::new(90, 60, 200, 200)),
            (canvas, Rect::new(200, 200, 300, 300)),
        ] {
            let b = bounds.intersect(&f.bounds);
            let mut out = vec![[0u8; 4]; (b.width() * b.height()) as usize];
            let mut scratch = Vec::new();
            let ok = ProxyImage::render_full_resolution(&src, &f, bounds, region, &mut out, &mut scratch);
            let mut h = H0;
            hu(&mut h, &[ok as u64]);
            for o in &out {
                fnv(&mut h, o);
            }
            println!("fullres\t{}\t{}\t{}\t{:016x}", name, rect_s(bounds), rect_s(region), h);
        }
    }
}

fn proxy_cases() {
    let js = r#"[{"tool":"bloat","size":40,"points":[[48,32],[48,32],[48,32],[48,32]]},{"tool":"forwardWarp","size":30,"points":[[20,20],[30,25]]}]"#;
    for name in ["rgba8", "rgb8", "graya16", "cmyka8"] {
        let fmt = format_named(name);
        for b in [Rect::new(0, 0, 96, 64), Rect::new(-5, 3, 131, 80), Rect::new(0, 0, 0, 10)] {
            let src = surf(fmt, Rect::new(-10, -10, 140, 90), 5, false);
            for max_side in [0usize, 1, 7, 40, 96, 1000] {
                let f = LiquifyField::from_strokes(b, 2.0, &strokes_of(js));
                let p = ProxyImage::new(&src, b, max_side);
                let mut h = H0;
                hd(&mut h, &[p.scale]);
                hu(&mut h, &[p.w as u64, p.h as u64]);
                for v in &p.px {
                    hf(&mut h, v);
                }
                let mut out = vec![[0u8; 4]; p.w * p.h];
                p.render(&f, [0, 0, p.w, p.h], &mut out);
                p.render(&f, [1, 2, 5, 3], &mut out);
                for o in &out {
                    fnv(&mut h, o);
                }
                for r in [Rect::new(0, 0, 10, 10), Rect::new(-100, -100, 5000, 5000), Rect::new(i32::MIN, 3, i32::MAX, 9), b] {
                    let q = p.proxy_rect(r);
                    hu(&mut h, &q.map(|v| v as u64));
                }
                println!("proxy\t{}\t{}\t{}\t{:016x}", name, rect_s(b), max_side, h);
            }
        }
    }
}

fn json_cases() {
    let strokes = [
        r#"[]"#,
        r#"[{}]"#,
        r#"[{"tool":"bloat","size":12.5,"density":1,"pressure":-0,"rate":3e2,"points":[[1,2],[3,4,0.5],[],[1,2,3,4]],"amount":null}]"#,
        r#"[{"tool":"reconstructAll","amount":40,"extra":[1,2]}]"#,
        r#"[{"tool":{"thaw":null}}]"#,
        r#"[{"tool":{"thaw":1}}]"#,
        r#"[{"tool":{"thaw":null,"freeze":null}}]"#,
        r#"[{"tool":"warp"}]"#,
        r#"[{"tool":7}]"#,
        r#"[{"size":"big"}]"#,
        r#"[{"size":true,"tool":"nope"}]"#,
        r#"[{"points":[[1,"x"]]}]"#,
        r#"[{"points":{"a":1}}]"#,
        r#"[{"points":[1,2]}]"#,
        r#"[{"amount":"x"}]"#,
        r#"[["pucker",30]]"#,
        r#"[["pucker",30,50,100,80,[[1,2]],5,9]]"#,
        r#"[[]]"#,
        r#"["x"]"#,
        r#"{"tool":"bloat"}"#,
        r#"[{"size":1,"size":2}]"#,
        r#"[{"size":18446744073709551615,"rate":-9223372036854775808}]"#,
    ];
    for s in strokes {
        let v: serde_json::Value = serde_json::from_str(s).expect("json");
        let out = match serde_json::from_value::<Vec<LiquifyStroke>>(v) {
            Ok(st) => format!("ok:{}", serde_json::to_string(&st).expect("ser")),
            Err(e) => format!("err:{e}"),
        };
        println!("json.strokes\t{}\t{}", s, out);
    }
    let pins = [
        r#"[]"#,
        r#"[{"src":[1,2],"dst":[3,4.5]}]"#,
        r#"[{"src":[1,2],"dst":[3,4],"rotate":90,"depth":-3},{"dst":[0,0],"src":[1e300,-0],"rotate":null,"depth":0}]"#,
        r#"[{"src":[1,2]}]"#,
        r#"[{"dst":[1,2]}]"#,
        r#"[{"src":[1],"dst":[1,2]}]"#,
        r#"[{"src":[1,2,3],"dst":[1,2]}]"#,
        r#"[{"src":"a","dst":[1,2]}]"#,
        r#"[{"src":[1,2],"dst":[1,null]}]"#,
        r#"[{"src":[1,2],"dst":[1,2],"depth":1.5}]"#,
        r#"[{"src":[1,2],"dst":[1,2],"depth":3000000000}]"#,
        r#"[{"src":[1,2],"dst":[1,2],"depth":-3000000000}]"#,
        r#"[{"src":[1,2],"dst":[1,2],"rotate":"x"}]"#,
        r#"[[[1,2],[3,4]]]"#,
        r#"[[[1,2]]]"#,
        r#"[[[1,2],[3,4],null,2,9]]"#,
        r#"[7]"#,
        r#"{"src":[1,2]}"#,
    ];
    for s in pins {
        let v: serde_json::Value = serde_json::from_str(s).expect("json");
        let out = match serde_json::from_value::<Vec<PuppetPin>>(v) {
            Ok(p) => format!("ok:{}", serde_json::to_string(&p).expect("ser")),
            Err(e) => format!("err:{e}"),
        };
        println!("json.pins\t{}\t{}", s, out);
    }
    let warps = [
        r#"{}"#,
        r#"{"pins":[{"src":[1,2],"dst":[3,4]}],"mode":"rigid","density":"more","expansion":-5}"#,
        r#"{"mode":"Rigid"}"#,
        r#"{"density":{"fewer":null},"expansion":"x"}"#,
        r#"[[],"distort","fewer",3]"#,
        r#"[]"#,
        r#"null"#,
    ];
    for s in warps {
        let v: serde_json::Value = serde_json::from_str(s).expect("json");
        let out = match serde_json::from_value::<PuppetWarp>(v) {
            Ok(p) => format!("ok:{}", serde_json::to_string(&p).expect("ser")),
            Err(e) => format!("err:{e}"),
        };
        println!("json.warp\t{}\t{}", s, out);
    }
}

fn misc_cases() {
    for t in LiquifyTool::ALL.iter().chain(&[LiquifyTool::ReconstructAll, LiquifyTool::FreezeAll, LiquifyTool::ThawAll, LiquifyTool::InvertFreeze]) {
        println!("tool\t{}\t{}\t{}\t{}", serde_json::to_string(t).expect("ser"), t.label(), t.is_global() as u8, t.is_stationary() as u8);
    }
    for (i, size) in [100.0, 0.5, 1.0, 15000.0, 15000.5, 1e21, 1.5e-7, f64::NAN, f64::INFINITY, -0.0, 3578819939694.65625].iter().enumerate() {
        for pts in [vec![], vec![vec![1.0, 2.0]], vec![vec![1.0, 2.0, f64::NAN]], vec![vec![1.0]], vec![vec![1.0, 2.0, 3.0, 4.0]], vec![vec![f64::INFINITY, 0.0]]] {
            let mut s = LiquifyStroke::new(LiquifyTool::Bloat, *size);
            s.points = pts.clone();
            let r = match s.validate() {
                Ok(()) => "ok".to_string(),
                Err(e) => e,
            };
            println!("validate\t{}\t{}\t{}", hx(*size), pts.iter().map(|p| p.iter().map(|v| hx(*v)).collect::<Vec<_>>().join(",")).collect::<Vec<_>>().join(";"), r);
            let _ = i;
        }
    }
    for b in [Rect::new(0, 0, 2049, 2050), Rect::new(0, 0, 2050, 2050), Rect::new(0, 0, 0, 0), Rect::new(-5, -5, 100000, 3)] {
        println!("autocell\t{}\t{}", rect_s(b), hx(auto_cell(b)));
    }
    for c in [0.0, 0.5, 1.0, 2.5, 64.0, 100.0, f64::NAN, f64::INFINITY, -3.0] {
        let f = LiquifyField::new(Rect::new(-3, 2, 50, 41), c);
        println!("fieldnew\t{}\t{}\t{}\t{}", hx(c), hx(f.cell), f.w, f.h);
    }
    for s in ["rigid", "Normal", "DISTORT", "x", "", "normall"] {
        println!("mode\t{}\t{:?}", s, PuppetMode::parse(s));
    }
    for s in ["fewer", "FewerPoints", "normal", "more", "morePOINTS", "many", ""] {
        println!("density\t{}\t{:?}", s, PuppetDensity::parse(s));
    }
}

/// The L shape of the reference's tests (a vertical bar and a foot).
fn blob(st: SampleType, alpha: bool) -> Surface {
    let mut s = Surface::new(PixelFormat::new(ColorMode::Rgb, st, alpha));
    for y in 10..70 {
        for x in 20..36 {
            let px = [((x + y) % 7) as f32 / 6.0, 0.5, (y as f32) / 80.0, 1.0];
            s.write_pixel(x, y, &px[..if alpha { 4 } else { 3 }]);
        }
    }
    for y in 56..70 {
        for x in 36..70 {
            let px = [0.2, ((x * 3) % 5) as f32 / 4.0, 0.8, 1.0];
            s.write_pixel(x, y, &px[..if alpha { 4 } else { 3 }]);
        }
    }
    s
}
/// Two separate islands with soft alpha (components without pins stay put).
fn islands() -> Surface {
    let mut s = Surface::new(PixelFormat::new(ColorMode::Rgb, SampleType::F32, true));
    for y in 0..40 {
        for x in 0..120 {
            let inside = (x < 40 && y > 5) || (x > 80 && y < 30);
            if inside {
                s.write_pixel(x, y, &[pat(x, y, 0, 1), pat(x, y, 1, 1), pat(x, y, 2, 1), 0.03 + (x % 5) as f32 * 0.2]);
            }
        }
    }
    s
}
fn surface_named(name: &str) -> Surface {
    match name {
        "blob8" => blob(SampleType::U8, true),
        "blob32" => blob(SampleType::F32, true),
        "blobrgb" => blob(SampleType::U8, false),
        _ => islands(),
    }
}

fn hmesh(h: &mut u64, m: &PuppetMesh) {
    hd(h, &[m.spacing]);
    hu(h, &[m.verts.len() as u64, m.tris.len() as u64]);
    for v in &m.verts {
        hd(h, v);
    }
    for t in &m.tris {
        hu(h, &t.map(|i| i as u64));
    }
}

fn mesh_cases() {
    let n = f64::NAN;
    for name in ["blob8", "blob32", "blobrgb", "islands"] {
        let s = surface_named(name);
        let cb = s.content_bounds();
        for b in [cb, Rect::new(0, 0, 0, 0), Rect::new(15, 5, 80, 40)] {
            for (di, d) in [PuppetDensity::Fewer, PuppetDensity::Normal, PuppetDensity::More].iter().enumerate() {
                for e in [-1e9, -200.0, -10.0, -3.0, 0.0, 2.0, 7.5, 20.0, n] {
                    let m = build_mesh(&s, b, *d, e);
                    let mut h = H0;
                    hmesh(&mut h, &m);
                    println!("mesh\t{}\t{}\t{}\t{}\t{:016x}", name, rect_s(b), di, hx(e), h);
                }
            }
        }
    }
}

/// Pin sets (JSON) solved on the meshes in every mode.
const PINS: &[&str] = &[
    r#"[]"#,
    r#"[{"src":[28,30],"dst":[38,25]}]"#,
    r#"[{"src":[28,20],"dst":[28,20]},{"src":[60,62],"dst":[60,62]}]"#,
    r#"[{"src":[28,14],"dst":[10,30]},{"src":[64,62],"dst":[70,50]},{"src":[28,62],"dst":[28,62]}]"#,
    r#"[{"src":[28,30],"dst":[28,30],"rotate":90}]"#,
    r#"[{"src":[28,20],"dst":[30,22],"rotate":-45,"depth":2},{"src":[60,62],"dst":[66,50],"depth":1},{"src":[28,62],"dst":[20,70],"rotate":400}]"#,
    r#"[{"src":[30,30],"dst":[30,30]},{"src":[30,30],"dst":[50,40]}]"#,
    r#"[{"src":[25,20],"dst":[25,20]},{"src":[25,40],"dst":[35,40]},{"src":[25,60],"dst":[45,60]}]"#,
    r#"[{"src":[500,500],"dst":[510,480]},{"src":[-40,-40],"dst":[-40,-40],"rotate":10}]"#,
    r#"[{"src":[10,20],"dst":[15,25]},{"src":[100,10],"dst":[90,15],"depth":5}]"#,
];

fn solve_cases() {
    for name in ["blob8", "islands"] {
        let s = surface_named(name);
        let b = s.content_bounds();
        for (di, d) in [PuppetDensity::Fewer, PuppetDensity::Normal].iter().enumerate() {
            for js in PINS {
                let pins: Vec<PuppetPin> = serde_json::from_str(js).expect("pins");
                let src: Vec<[f64; 2]> = pins.iter().map(|p| p.src).collect();
                let dst: Vec<[f64; 2]> = pins.iter().map(|p| p.dst).collect();
                let fixed: Vec<bool> = pins.iter().map(|p| p.rotate.is_some()).collect();
                let rot: Vec<Option<f64>> = pins.iter().map(|p| p.rotate).collect();
                let depths: Vec<i32> = pins.iter().map(|p| p.depth).collect();
                let solver = PuppetSolver::new(build_mesh(&s, b, *d, 2.0), &src, &fixed);
                for (mi, mode) in [PuppetMode::Rigid, PuppetMode::Normal, PuppetMode::Distort].iter().enumerate() {
                    for iters in [0usize, 1, 7] {
                        let (v, e) = solver.solve(&dst, &rot, *mode, iters, None);
                        let mut h = H0;
                        for p in &v {
                            hd(&mut h, p);
                        }
                        hd(&mut h, &e);
                        hd(&mut h, &[solver.energy(&v, &dst, &rot)]);
                        // Warm start from the result, with one pin dropped from dst / rotations.
                        let (v2, e2) = solver.solve(&dst[..dst.len().saturating_sub(1)], &rot, *mode, 2, Some(&v));
                        for p in &v2 {
                            hd(&mut h, p);
                        }
                        hd(&mut h, &e2);
                        let order = solver.draw_order(&depths, &src);
                        hu(&mut h, &order.iter().map(|&t| t as u64).collect::<Vec<_>>());
                        println!("solve\t{}\t{}\t{}\t{}\t{}\t{:016x}", name, di, mi, iters, js, h);
                    }
                }
            }
        }
    }
}

/// Hand-made meshes: a vertex numbering that makes the band too wide for the Cholesky factor
/// (conjugate gradients), obtuse and degenerate triangles, an empty mesh, NaN pins.
fn custom_cases() {
    let n = f64::NAN;
    // 80 x 80 vertex grid with a scrambled numbering (bandwidth ~ n): too wide to factor, CG.
    let scrambled = grid_mesh(80, 3.0, true);
    let small = grid_mesh(12, 5.0, false);
    let obtuse = PuppetMesh {
        verts: vec![[0.0, 0.0], [10.0, 0.0], [5.0, 0.5], [5.0, -8.0], [20.0, 1.0], [20.0, 1.0], [30.0, 30.0]],
        tris: vec![[0, 1, 2], [0, 3, 1], [1, 4, 2], [1, 4, 5], [4, 5, 5]],
        spacing: 1.0,
    };
    type Case = (&'static str, PuppetMesh, Vec<[f64; 2]>, Vec<[f64; 2]>, Vec<Option<f64>>, usize, usize);
    let cases: Vec<Case> = vec![
        ("scrambled", scrambled, vec![[30.0, 30.0], [200.0, 210.0]], vec![[40.0, 20.0], [190.0, 230.0]], vec![None, Some(30.0)], 1, 1),
        ("obtuse", obtuse.clone(), vec![[5.0, 0.2], [20.0, 1.0]], vec![[6.0, 3.0], [25.0, 1.0]], vec![None, None], 3, 3),
        ("obtuse-rot", obtuse, vec![[5.0, -2.0]], vec![[5.0, -1.0]], vec![Some(15.0)], 3, 3),
        ("empty", PuppetMesh::default(), vec![[10.0, 10.0]], vec![[12.0, 10.0]], vec![Some(5.0)], 3, 3),
        ("nan-src", small.clone(), vec![[n, 30.0], [20.0, 20.0]], vec![[40.0, 20.0], [25.0, 20.0]], vec![None, None], 3, 3),
        ("nan-dst", small.clone(), vec![[30.0, 30.0]], vec![[n, 20.0]], vec![Some(n)], 3, 1),
        ("coincident", small.clone(), vec![[30.0, 30.0], [30.0, 30.0], [30.0, 30.0]], vec![[30.0, 30.0], [40.0, 30.0], [30.0, 40.0]], vec![None, None, None], 3, 3),
        ("colinear", small, vec![[10.0, 10.0], [20.0, 10.0], [30.0, 10.0]], vec![[10.0, 10.0], [20.0, 30.0], [30.0, 10.0]], vec![None, None, None], 3, 3),
    ];
    for (name, mesh, src, dst, rot, nmodes, iters) in cases {
        let fixed: Vec<bool> = rot.iter().map(|r| r.is_some()).collect();
        let solver = PuppetSolver::new(mesh, &src, &fixed);
        for (mi, mode) in [PuppetMode::Rigid, PuppetMode::Normal, PuppetMode::Distort].iter().enumerate().take(nmodes) {
            let (v, e) = solver.solve(&dst, &rot, *mode, iters, None);
            let mut h = H0;
            for p in &v {
                hd(&mut h, p);
            }
            hd(&mut h, &e);
            println!("custom\t{}\t{}\t{}\t{:016x}", name, mi, iters, h);
        }
    }
}

/// A g x g vertex grid (spacing sp), optionally with a scrambled vertex numbering.
fn grid_mesh(g: usize, sp: f64, scramble: bool) -> PuppetMesh {
    let perm: Vec<usize> = (0..g * g).map(|i| if scramble { (i * 2654435761usize) % (g * g) } else { i }).collect();
    let mut verts = vec![[0.0; 2]; g * g];
    for j in 0..g {
        for i in 0..g {
            verts[perm[j * g + i]] = [i as f64 * sp, j as f64 * sp];
        }
    }
    let mut tris = Vec::new();
    for j in 0..g - 1 {
        for i in 0..g - 1 {
            let (a, b, c, d) = (perm[j * g + i], perm[j * g + i + 1], perm[(j + 1) * g + i + 1], perm[(j + 1) * g + i]);
            tris.push([a, b, c]);
            tris.push([a, c, d]);
        }
    }
    PuppetMesh { verts, tris, spacing: sp }
}

fn warp_cases() {
    let warps = [
        r#"{"pins":[]}"#,
        r#"{"pins":[{"src":[28,20],"dst":[28,20]},{"src":[60,62],"dst":[60,62]}]}"#,
        r#"{"pins":[{"src":[28,30],"dst":[38,25]}],"mode":"rigid"}"#,
        r#"{"pins":[{"src":[28,30],"dst":[28,30],"rotate":90}],"mode":"rigid"}"#,
        r#"{"pins":[{"src":[28,14],"dst":[10,30]},{"src":[64,62],"dst":[70,50]},{"src":[28,62],"dst":[28,62]}],"mode":"distort","density":"more"}"#,
        r#"{"pins":[{"src":[28,20],"dst":[50,60],"depth":3},{"src":[60,62],"dst":[30,30]},{"src":[28,62],"dst":[28,62],"depth":-1}],"density":"fewer"}"#,
        r#"{"pins":[{"src":[28,30],"dst":[38,25]}],"expansion":-200}"#,
        r#"{"pins":[{"src":[28,20],"dst":[28,20]},{"src":[60,62],"dst":[1048576,1048576]}]}"#,
        r#"{"pins":[{"src":[28,20],"dst":[28,20]},{"src":[60,62],"dst":[-1e7,62]}]}"#,
        r#"{"pins":[{"src":[10,30],"dst":[20,30]},{"src":[100,10],"dst":[100,20]}],"expansion":10}"#,
    ];
    for name in ["blob8", "blob32", "blobrgb", "islands"] {
        let s = surface_named(name);
        let b = s.content_bounds();
        for js in warps {
            let w: PuppetWarp = serde_json::from_str(js).expect("warp");
            for (ii, interp) in [Interp::Nearest, Interp::Bilinear, Interp::Bicubic].iter().enumerate() {
                let out = match puppet_warp(&s, b, &w, *interp) {
                    Some(o) => format!("{:016x}", hs(&o)),
                    None => "none".to_string(),
                };
                println!("warp\t{}\t{}\t{}\t{}", name, ii, js, out);
            }
            let (solver, v, order) = deform(&s, b, &w, 5);
            let mut h = H0;
            hmesh(&mut h, &solver.mesh);
            for p in &v {
                hd(&mut h, p);
            }
            hu(&mut h, &order.iter().map(|&t| t as u64).collect::<Vec<_>>());
            println!("deform\t{}\t{}\t{:016x}", name, js, h);
            println!("identity\t{}\t{}", js, w.is_identity() as u8);
        }
    }
}

pub fn run() {
    misc_cases();
    json_cases();
    for b in BOUNDS {
        for c in CELLS {
            for js in STROKES {
                field_case(b, c, js);
            }
        }
    }
    segment_cases();
    apply_cases();
    proxy_cases();
    fullres_cases();
    mesh_cases();
    solve_cases();
    custom_cases();
    warp_cases();
}
