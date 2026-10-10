//! `rustref paint2-vectors`: L3 paint (2/2) reference vectors (mixer, colour replacement,
//! retouch machinery, background eraser), checked line by line by tests/paint/test_parity2.c.
//! One case per line, tab-separated, keyword first. Floats are hex bits (f32 %08x, f64 %016x);
//! hashes are FNV-1a 64 over little-endian bits, NaNs canonicalised. Colours are 4 comma-separated
//! f32 bit patterns. BRUSH is an index into the "brush I JSON" lines (serde JSON) that come
//! first; points are as in paint_vectors.rs.
//!
//!   brush  I JSON                                 the brushes
//!   fpr    DAB HARD RECT HASH                     retouch::footprint (DAB = cx,cy,radius,alpha)
//!   sbnd   BRUSH PTS RECT                         stroke_bounds(dabs(stroke))
//!   cov    BRUSH PTS RECT HASH                    stroke_coverage
//!   acov   FMT SEL LOCK BRUSH PTS OPAC BLEND DMG HASH   stroke_coverage + apply_coverage (generated paint)
//!   mixpm  HASH                                   mix_premultiplied sweep
//!   dstk   FMT SEL LOCK BRUSH PTS HALO DMG HASH   apply_dab_stroke with a fixed test effect
//!   smudge FMT SEL BRUSH PTS STR FINGER MAX ALPHA DMG HASH   Smudge via apply_dab_stroke
//!   mixer  FMT SEL LOCK SAMPLE BRUSH PTS SET STATE DMG HASH STATE'   apply_mixer_stroke
//!   repl   FMT SEL LOCK BRUSH PTS SET DMG HASH    apply_color_replacement
//!   bger   FMT SEL BRUSH PTS SET DMG HASH         apply_background_eraser
//!   ser    TYPE JSON                              to_string of a value
//!   serr   SET JSON                               to_string of the ReplaceSettings SET (as in repl)
//!   serb   SET JSON                               to_string of the BgEraseSettings SET (as in bger)
//!   de     TYPE INPUT RESULT                      from_value(from_str::<Value>(INPUT)) or "ERR msg"
//!
//! FMT indexes `formats()`; the canvas is `canvas(FMT)`; SEL 1 adds `selection()`. SAMPLE is "-",
//! or a format index whose canvas is the sample surface. SET/STATE are '/'-separated fields.
use photocraft_color::{BlendMode, ColorMode, PixelFormat, SampleType};
use photocraft_geom::{Point, Rect};
use photocraft_paint::bg_erase::{apply_background_eraser, BgEraseSettings};
use photocraft_paint::brush::*;
use photocraft_paint::mixer::{apply_mixer_stroke, MixerSettings, MixerState};
use photocraft_paint::replace::{apply_color_replacement, Limits, ReplaceMode, ReplaceSettings, Sampling};
use photocraft_paint::retouch::{
    apply_coverage, apply_dab_stroke, footprint, mix_premultiplied, stroke_bounds, stroke_coverage, Region, Smudge,
};
use photocraft_paint::rng::mix64;
use photocraft_paint::tile::GrayTile;
use photocraft_paint::{dabs, Dab, Stroke, StrokePoint};
use photocraft_raster::{from_rgba, Surface};
use serde_json::Value as J;

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn b32(v: f32) -> u32 { if v.is_nan() { 0x7fc0_0000 } else { v.to_bits() } }
fn b64(v: f64) -> u64 { if v.is_nan() { 0x7ff8_0000_0000_0000 } else { v.to_bits() } }
fn f32b(h: &mut u64, v: f32) { fnv(h, &b32(v).to_le_bytes()); }
fn x32(v: f32) -> String { format!("{:08x}", b32(v)) }
fn x64(v: f64) -> String { format!("{:016x}", b64(v)) }
fn c4(c: [f32; 4]) -> String { c.iter().map(|v| x32(*v)).collect::<Vec<_>>().join(",") }
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
    vec![
        vec![pt(10.0, 40.0, 1.0), pt(190.0, 40.0, 1.0)],
        vec![pt(30.25, 30.5, 1.0)],
        vec![pt(35.0, 75.0, 1.0), pt(80.0, 70.0, 0.6)],
        (0..10)
            .map(|i| StrokePoint {
                time: i as f64 * 12.0,
                ..pt(15.0 + i as f64 * 17.0, 20.0 + (i as f64 * 0.8).sin() * 25.0 + i as f64 * 5.0, 0.15 + i as f32 * 0.09)
            })
            .collect(),
        vec![pt(150.0, 60.0, 1.0), pt(110.0, 50.0, 1.0), pt(95.0, 20.0, 0.5)],
        vec![],
    ]
}
fn hostile_sets() -> Vec<Vec<StrokePoint>> {
    vec![
        vec![pt(f64::NAN, 10.0, 1.0), pt(30.0, 10.0, f32::NAN)],
        vec![pt(-500.0, -500.0, 1.0)],
        vec![pt(40.0, 40.0, f32::INFINITY), pt(60.0, 45.0, -1.0)],
    ]
}

fn gtile(w: u32, h: u32, seed: u64) -> GrayTile {
    let data = (0..(w as u64 * h as u64)).map(|i| (mix64(seed.wrapping_mul(1000).wrapping_add(i)) >> 48) as u16).collect();
    GrayTile { width: w, height: h, data }
}

fn base() -> BrushSettings {
    BrushSettings { pressure_size: false, spacing: 0.25, seed: 7, size: 20.0, hardness: 1.0, ..Default::default() }
}

fn brushes() -> Vec<BrushSettings> {
    let b = base;
    vec![
        b(),
        BrushSettings { size: 26.0, hardness: 0.0, ..b() },
        BrushSettings { size: 14.0, hardness: 0.6, opacity: 0.5, flow: 0.7, ..b() },
        BrushSettings { tip: TipShape::Sampled(gtile(12, 9, 5)), size: 22.0, angle: 35.0, ..b() },
        BrushSettings {
            transfer: Transfer {
                enabled: true,
                opacity: Dynamic::controlled(Control::PenPressure),
                wetness: Dynamic::controlled(Control::PenPressure),
                mix: Dynamic { jitter: 1.0, minimum: 0.25, ..Default::default() },
                ..Default::default()
            },
            size: 18.0,
            hardness: 0.8,
            ..b()
        },
        BrushSettings {
            scattering: Scattering { enabled: true, scatter: Dynamic::jitter(1.2), both_axes: true, count: 2, ..Default::default() },
            shape_dynamics: ShapeDynamics { enabled: true, size: Dynamic { jitter: 0.5, minimum: 0.3, ..Default::default() }, ..Default::default() },
            size: 16.0,
            ..b()
        },
        BrushSettings {
            texture: Texture { enabled: true, pattern: Pattern::Procedural { style: PatternStyle::Noise, size: 32, seed: 3 }, depth: 0.7, ..Default::default() },
            size: 24.0,
            hardness: 0.5,
            ..b()
        },
        BrushSettings { pressure_size: true, size: 30.0, spacing: 0.4, hardness: 0.3, ..b() },
    ]
}
/// Brushes travel as their index into `brushes()` (listed once as "brush I JSON" lines).
fn bj(b: &BrushSettings) -> String { brushes().iter().position(|x| x == b).unwrap().to_string() }

fn formats() -> Vec<PixelFormat> {
    let f = PixelFormat::new;
    use ColorMode::*;
    use SampleType::*;
    vec![
        f(Rgb, U8, true),
        f(Rgb, U16, true),
        f(Rgb, F32, true),
        f(Grayscale, U8, true),
        f(Cmyk, U8, true),
        f(Rgb, U8, false),
        f(Lab, U16, true),
        f(Grayscale, U16, false),
        f(Cmyk, U8, false),
        f(Grayscale, F32, true),
    ]
}

/// A colourful test canvas: green field, blue right half, a red wall, a generated gradient patch
/// with varying alpha, and a cleared (transparent where possible) hole.
fn canvas(fi: usize) -> Surface {
    let f = formats()[fi];
    let mut s = Surface::new(f);
    s.fill_rect(Rect::new(-20, -20, 220, 120), &from_rgba(&f, [0.2, 0.6, 0.2, 1.0]));
    s.fill_rect(Rect::new(100, -20, 220, 120), &from_rgba(&f, [0.1, 0.1, 0.5, 1.0]));
    s.fill_rect(Rect::new(60, -20, 66, 120), &from_rgba(&f, [0.9, 0.1, 0.1, 1.0]));
    let r = Rect::new(120, 30, 180, 90);
    let mut data = Vec::new();
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            let (u, v) = ((x - r.x0) as f32, (y - r.y0) as f32);
            let a = if v < 40.0 { 1.0 } else { 0.3 + ((x % 5) as f32) * 0.15 };
            data.extend(from_rgba(&f, [u / 59.0, v / 59.0, (((x + y) % 7) as f32) / 6.0, a]));
        }
    }
    s.write_region(r, &data);
    s.fill_rect(Rect::new(20, 60, 50, 90), &vec![0.0; f.channels()]);
    s
}

fn selection() -> Surface {
    let mut s = Surface::new(PixelFormat::GRAY8);
    s.fill_rect(Rect::new(0, 0, 100, 200), &[1.0]);
    s.fill_rect(Rect::new(100, 0, 160, 200), &[0.5]);
    s
}
fn sel_of(on: bool) -> Option<Surface> { on.then(selection) }

const HR: Rect = Rect { x0: -40, y0: -40, x1: 240, y1: 140 };
fn surf_h(s: &Surface) -> String {
    let mut h = H0;
    let mut b = s.to_interleaved(HR);
    if s.format().sample == SampleType::F32 {
        for c in b.chunks_mut(4) {
            let v = f32::from_le_bytes([c[0], c[1], c[2], c[3]]);
            c.copy_from_slice(&b32(v).to_le_bytes());
        }
    }
    fnv(&mut h, &b);
    format!("{h:016x}")
}
fn floats_h(v: &[f32]) -> String {
    let mut h = H0;
    for x in v {
        f32b(&mut h, *x);
    }
    format!("{h:016x}")
}
fn stroke(b: &BrushSettings, p: &[StrokePoint]) -> Stroke { Stroke { brush: b.clone(), points: p.to_vec() } }

fn fpr_lines() {
    let dabs = [
        (10.0, 10.0, 5.0, 1.0),
        (10.5, -3.25, 7.3, 0.6),
        (-20.7, 4.1, 0.4, 1.0),
        (0.0, 0.0, 0.0, 1.0),
        (3.3, 3.3, 12.0, f32::NAN),
        (3.3, 3.3, f32::NAN, 1.0),
        (5.0, 5.0, -3.0, 1.0),
        (f64::NAN, 2.0, 4.0, 1.0),
    ];
    for (i, &(x, y, r, a)) in dabs.iter().enumerate() {
        for hard in [1.0f32, 0.5, 0.0, f32::NAN, 1.7] {
            let d = Dab::round(Point::new(x, y), r, a);
            let fp = footprint(&d, i, hard);
            l!("fpr", format!("{},{},{},{}", x64(x), x64(y), x32(r), x32(a)), x32(hard), rect_s(fp.rect), floats_h(&fp.cov));
        }
    }
}

/// Generated paint image over `r` (native channels; alpha varies).
fn gen_paint(fmt: &PixelFormat, r: Rect) -> Region {
    let n = fmt.channels();
    let mut p = Region::new(r, n);
    let mut i = 0;
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            for c in 0..n {
                p.data[i] = if fmt.alpha && c == n - 1 {
                    0.25 + ((x + y).rem_euclid(4)) as f32 * 0.25
                } else {
                    ((x * 7 + y * 13 + c as i32 * 5).rem_euclid(17)) as f32 / 16.0
                };
                i += 1;
            }
        }
    }
    p
}

fn test_effect(work: &mut Region, fp: &photocraft_paint::retouch::Footprint) {
    for y in fp.rect.y0..fp.rect.y1 {
        for x in fp.rect.x0..fp.rect.x1 {
            if !work.rect.contains(x, y) {
                continue;
            }
            let k = fp.at(x, y);
            if k <= 0.0 {
                continue;
            }
            let px = work.px_mut(x, y);
            for c in 0..px.len() {
                px[c] += (((fp.index + c) % 4) as f32 / 3.0 - px[c]) * k * 0.5;
            }
        }
    }
}

fn mixer_set_s(m: &MixerSettings) -> String { format!("{}/{}/{}/{}", x32(m.wet), x32(m.load), x32(m.mix), x32(m.flow)) }
fn state_s(s: &MixerState) -> String {
    format!("{}/{}/{}", s.reservoir.map_or("-".into(), c4), x32(s.amount), s.pickup.map_or("-".into(), c4))
}
fn repl_s(r: &ReplaceSettings) -> String {
    let m = serde_json::to_value(r.mode).unwrap();
    let s = serde_json::to_value(r.sampling).unwrap();
    let l = serde_json::to_value(r.limits).unwrap();
    format!("{}/{}/{}/{}/{}/{}/{}", m.as_str().unwrap(), s.as_str().unwrap(), l.as_str().unwrap(), x32(r.tolerance), r.anti_alias as u8, c4(r.color), c4(r.background))
}
fn bg_s(r: &BgEraseSettings) -> String {
    let s = serde_json::to_value(r.sampling).unwrap();
    let l = serde_json::to_value(r.limits).unwrap();
    format!("{}/{}/{}/{}/{}/{}", s.as_str().unwrap(), l.as_str().unwrap(), x32(r.tolerance), r.protect_foreground as u8, c4(r.foreground), c4(r.background))
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
        "ReplaceMode" => go!(ReplaceMode),
        "Sampling" => go!(Sampling),
        "Limits" => go!(Limits),
        "ReplaceSettings" => go!(ReplaceSettings),
        "BgEraseSettings" => go!(BgEraseSettings),
        _ => panic!(),
    };
    assert!(!input.contains('\t') && !r.contains('\n') && !r.contains('\t'));
    l!("de", ty, input, r);
}

pub fn run() {
    let bs = brushes();
    let sets = point_sets();
    let hostile = hostile_sets();
    let nf = formats().len();

    for (i, b) in bs.iter().enumerate() {
        l!("brush", i, serde_json::to_string(b).unwrap());
    }

    // ---- footprints, bounds, coverage ----
    fpr_lines();
    for b in &bs {
        for p in sets.iter().chain(hostile.iter()) {
            let st = stroke(b, p);
            l!("sbnd", bj(b), pts_s(p), rect_s(stroke_bounds(&dabs(&st))));
            let (r, cov) = stroke_coverage(&st);
            l!("cov", bj(b), pts_s(p), rect_s(r), floats_h(&cov));
        }
    }

    // ---- apply_coverage ----
    let blends = [BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Color, BlendMode::Difference, BlendMode::Luminosity];
    for fi in 0..nf {
        for (bi, blend) in blends.iter().enumerate() {
            for (sel, lock, opac) in [(false, false, 1.0f32), (true, false, 0.7), (false, true, 0.5), (true, true, 2.0)] {
                let (b, p) = (&bs[(bi + fi) % bs.len()], &sets[(bi + fi) % 5]);
                let st = stroke(b, p);
                let (r, cov) = stroke_coverage(&st);
                let mut s = canvas(fi);
                let paint = gen_paint(&s.format(), r);
                let d = apply_coverage(&mut s, r, &cov, opac, sel_of(sel).as_ref(), lock, &paint, *blend);
                let bname = serde_json::to_value(blend).unwrap();
                l!("acov", fi, sel as u8, lock as u8, bj(b), pts_s(p), x32(opac), bname.as_str().unwrap(), rect_s(d), surf_h(&s));
            }
        }
    }

    // ---- mix_premultiplied ----
    {
        let mut h = H0;
        let vals = [0.0f32, 1.0, 0.5, 0.25, 1e-7, 0.9, f32::NAN, -0.5, 2.0];
        for a in [0usize, 1, 2, 3, 4, 7] {
            for (i, &x) in vals.iter().enumerate() {
                for (j, &y) in vals.iter().enumerate() {
                    for &k in &[0.0f32, 0.3, 1.0, f32::NAN, 1.5] {
                        let mut d = [x, y, vals[(i + j) % 9], vals[(i * 2 + 1) % 9]];
                        let s = [y, vals[(j + 3) % 9], x, vals[(i + 5) % 9], 0.4];
                        mix_premultiplied(&mut d, &s, a, k);
                        for v in d {
                            f32b(&mut h, v);
                        }
                    }
                }
            }
        }
        l!("mixpm", format!("{h:016x}"));
    }

    // ---- apply_dab_stroke + mix_back ----
    for fi in 0..nf {
        for (k, (sel, lock, halo)) in [(false, false, 0), (true, false, 3), (false, true, 0), (true, true, 2)].into_iter().enumerate() {
            for pi in [0usize, 2, 3] {
                let b = &bs[(fi + k) % bs.len()];
                let p = &sets[pi];
                let mut s = canvas(fi);
                let d = apply_dab_stroke(&mut s, &stroke(b, p), sel_of(sel).as_ref(), lock, halo, test_effect);
                l!("dstk", fi, sel as u8, lock as u8, bj(b), pts_s(p), halo, rect_s(d), surf_h(&s));
            }
        }
    }

    // ---- smudge ----
    let fingers: Vec<Option<Vec<f32>>> = vec![None, Some(vec![0.0, 0.0, 1.0, 1.0]), Some(vec![0.3]), Some(vec![0.9, 0.1, 0.2, 0.5, 0.7, 0.4])];
    let fs = |f: &Option<Vec<f32>>| f.as_ref().map_or("-".into(), |v| v.iter().map(|x| x32(*x)).collect::<Vec<_>>().join(","));
    for fi in 0..nf {
        let fmt = formats()[fi];
        let alpha_ix = fmt.alpha.then(|| fmt.mode.color_channels());
        for (k, strength) in [0.8f32, 0.0, 1.0, 0.5, f32::NAN, 0.3].into_iter().enumerate() {
            for (fj, finger) in fingers.iter().enumerate() {
                for alpha in [None, alpha_ix, Some(9)] {
                    if (k + fj) % 2 == 1 && alpha != alpha_ix {
                        continue;
                    }
                    let b = &bs[(fi + k + fj) % bs.len()];
                    let p = &sets[(k + fj) % 5];
                    let sel = (k + fj) % 3 == 2;
                    let maxs = [b.size, 40.0, 1.0][(fi + k) % 3];
                    let mut s = canvas(fi);
                    let mut sm = Smudge::new(strength, finger.clone(), maxs).with_alpha(alpha);
                    let d = apply_dab_stroke(&mut s, &stroke(b, p), sel_of(sel).as_ref(), false, 0, |w, fp| sm.dab(w, fp));
                    let a_s = alpha.map_or("-".into(), |a| a.to_string());
                    l!("smudge", fi, sel as u8, bj(b), pts_s(p), x32(strength), fs(finger), x32(maxs), a_s, rect_s(d), surf_h(&s));
                }
            }
        }
    }

    // ---- mixer ----
    let msets = [
        MixerSettings::default(),
        MixerSettings { wet: 0.0, load: 0.0, mix: 0.0, flow: 1.0, ..Default::default() },
        MixerSettings { wet: 1.0, load: 0.3, mix: 0.9, flow: 1.0, ..Default::default() },
        MixerSettings { wet: 1.0, load: 1.0, mix: 1.0, flow: 0.6, ..Default::default() },
        MixerSettings { wet: 0.2, load: 1.0, mix: 0.4, flow: 0.3, ..Default::default() },
        MixerSettings { wet: f32::NAN, load: 2.0, mix: -1.0, flow: f32::INFINITY, ..Default::default() },
    ];
    let states = [
        MixerState::default(),
        MixerState { reservoir: Some([1.0, 0.0, 0.0, 1.0]), amount: 1.0, pickup: None },
        MixerState { reservoir: Some([0.9, 0.8, 0.1, 0.5]), amount: 0.4, pickup: Some([0.0, 0.2, 1.0, 1.0]) },
        MixerState { reservoir: None, amount: 0.7, pickup: Some([0.5, 0.5, 0.5, 0.2]) },
        MixerState { reservoir: Some([0.3, 0.6, 0.9, 1.0]), amount: f32::NAN, pickup: None },
    ];
    for fi in 0..nf {
        for (mi, m) in msets.iter().enumerate() {
            for (si, st0) in states.iter().enumerate() {
                if (fi + mi + si) % 2 == 1 && fi > 2 {
                    continue;
                }
                let b = &bs[(fi + mi + si) % bs.len()];
                let p = &sets[(mi + si) % 5];
                let sel = (mi + si) % 4 == 3;
                let lock = (fi + si) % 5 == 4;
                let sample = match (mi + si + fi) % 3 {
                    0 => None,
                    1 => Some(fi),
                    _ => Some((fi + 3) % nf),
                };
                let sample_s = sample.map(canvas);
                let mut s = canvas(fi);
                let mut st = st0.clone();
                let d = apply_mixer_stroke(&mut s, sample_s.as_ref(), &stroke(b, p), m, &mut st, sel_of(sel).as_ref(), lock);
                let smp = sample.map_or("-".into(), |v| v.to_string());
                l!("mixer", fi, sel as u8, lock as u8, smp, bj(b), pts_s(p), mixer_set_s(m), state_s(st0), rect_s(d), surf_h(&s), state_s(&st));
            }
        }
    }
    for p in &hostile {
        let mut s = canvas(0);
        let mut st = states[2].clone();
        let d = apply_mixer_stroke(&mut s, None, &stroke(&bs[0], p), &msets[2], &mut st, None, false);
        l!("mixer", 0, 0, 0, "-", bj(&bs[0]), pts_s(p), mixer_set_s(&msets[2]), state_s(&states[2]), rect_s(d), surf_h(&s), state_s(&st));
    }

    // ---- colour replacement ----
    let modes = [ReplaceMode::Hue, ReplaceMode::Saturation, ReplaceMode::Color, ReplaceMode::Luminosity];
    let samplings = [Sampling::Continuous, Sampling::Once, Sampling::BackgroundSwatch];
    let limits = [Limits::Discontiguous, Limits::Contiguous, Limits::FindEdges];
    let repl = |fi: usize, sel: bool, lock: bool, b: &BrushSettings, p: &[StrokePoint], rs: &ReplaceSettings| {
        let mut s = canvas(fi);
        let d = apply_color_replacement(&mut s, &stroke(b, p), rs, sel_of(sel).as_ref(), lock);
        l!("repl", fi, sel as u8, lock as u8, bj(b), pts_s(p), repl_s(rs), rect_s(d), surf_h(&s));
    };
    for (i, mode) in modes.iter().enumerate() {
        for (j, sampling) in samplings.iter().enumerate() {
            for (k, lim) in limits.iter().enumerate() {
                let rs = ReplaceSettings {
                    mode: *mode,
                    sampling: *sampling,
                    limits: *lim,
                    tolerance: [0.3, 0.1, 0.6][(i + j + k) % 3],
                    anti_alias: (i + k) % 2 == 0,
                    color: [1.0, 0.0, 0.0, 1.0],
                    background: [0.1, 0.1, 0.5, 1.0],
                };
                repl(0, false, false, &bs[(i + j + k) % bs.len()], &sets[(i + j) % 5], &rs);
            }
        }
    }
    for fi in 0..nf {
        for (k, tol) in [0.3f32, 0.0, 0.05, 1.0, f32::NAN, -0.5].into_iter().enumerate() {
            let rs = ReplaceSettings {
                mode: modes[(fi + k) % 4],
                sampling: samplings[k % 3],
                limits: limits[(fi + k) % 3],
                tolerance: tol,
                anti_alias: k % 2 == 0,
                color: [0.2, 0.3, 0.9, 1.0],
                background: [0.2, 0.6, 0.2, 1.0],
            };
            repl(fi, k % 3 == 1, k % 4 == 3, &bs[(fi + k) % bs.len()], &sets[(fi + k) % 5], &rs);
        }
    }
    for p in &hostile {
        repl(0, false, false, &bs[0], p, &ReplaceSettings::default());
    }

    // ---- background eraser ----
    let bger = |fi: usize, sel: bool, b: &BrushSettings, p: &[StrokePoint], bs_: &BgEraseSettings| {
        let mut s = canvas(fi);
        let d = apply_background_eraser(&mut s, &stroke(b, p), bs_, sel_of(sel).as_ref());
        l!("bger", fi, sel as u8, bj(b), pts_s(p), bg_s(bs_), rect_s(d), surf_h(&s));
    };
    for fi in 0..nf {
        for (j, sampling) in samplings.iter().enumerate() {
            for (k, lim) in limits.iter().enumerate() {
                for (t, tol) in [0.5f32, 0.05, 0.0, 1.0, f32::NAN, f32::INFINITY].into_iter().enumerate() {
                    if (j + k + t + fi) % 3 != 0 {
                        continue;
                    }
                    let bes = BgEraseSettings {
                        sampling: *sampling,
                        limits: *lim,
                        tolerance: tol,
                        protect_foreground: (t + k) % 2 == 1,
                        foreground: [0.1, 0.1, 0.5, 1.0],
                        background: [[1.0; 4], [0.2, 0.6, 0.2, 1.0], [0.1, 0.1, 0.5, 1.0]][(j + t) % 3],
                    };
                    bger(fi, (j + t) % 4 == 3, &bs[(fi + j + k + t) % bs.len()], &sets[(j + k + t) % 5], &bes);
                }
            }
        }
    }
    for p in &hostile {
        bger(0, false, &bs[0], p, &BgEraseSettings::default());
    }

    // ---- serde ----
    for m in modes {
        l!("ser", "ReplaceMode", serde_json::to_string(&m).unwrap());
    }
    for s in samplings {
        l!("ser", "Sampling", serde_json::to_string(&s).unwrap());
    }
    for x in limits {
        l!("ser", "Limits", serde_json::to_string(&x).unwrap());
    }
    l!("ser", "ReplaceSettings", serde_json::to_string(&ReplaceSettings::default()).unwrap());
    l!("ser", "BgEraseSettings", serde_json::to_string(&BgEraseSettings::default()).unwrap());
    let odd = ReplaceSettings {
        mode: ReplaceMode::Luminosity,
        sampling: Sampling::BackgroundSwatch,
        limits: Limits::FindEdges,
        tolerance: 0.123,
        anti_alias: false,
        color: [0.1, 0.2, 0.3, 0.4],
        background: [f32::NAN, 1.0, 0.0, 1.0],
    };
    l!("serr", repl_s(&odd), serde_json::to_string(&odd).unwrap());
    let odd = BgEraseSettings { tolerance: f32::INFINITY, foreground: [0.5, -0.0, 1e-8, 1.0], ..Default::default() };
    l!("serb", bg_s(&odd), serde_json::to_string(&odd).unwrap());
    for (ty, input) in [
        ("ReplaceMode", r#""hue""#),
        ("ReplaceMode", r#""Color""#),
        ("ReplaceMode", r#"3"#),
        ("Sampling", r#""backgroundSwatch""#),
        ("Sampling", r#"{"once": null}"#),
        ("Limits", r#""findEdges""#),
        ("Limits", r#""edges""#),
        ("ReplaceSettings", r#"{}"#),
        ("ReplaceSettings", r#"{"mode": "saturation", "tolerance": 0.5, "antiAlias": false, "x": 1}"#),
        ("ReplaceSettings", r#"{"mode": "nope"}"#),
        ("ReplaceSettings", r#"{"color": [1, 2, 3]}"#),
        ("ReplaceSettings", r#"{"tolerance": "x"}"#),
        ("ReplaceSettings", r#"["hue", "once", "contiguous", 0.2, true, [0,0,0,1], [1,1,1,1]]"#),
        ("ReplaceSettings", r#"null"#),
        ("BgEraseSettings", r#"{}"#),
        ("BgEraseSettings", r#"{"sampling": "once", "limits": "discontiguous", "protectForeground": true, "foreground": [0.5, 0.5, 0.5, 1]}"#),
        ("BgEraseSettings", r#"{"limits": 2}"#),
        ("BgEraseSettings", r#"{"tolerance": null}"#),
        ("BgEraseSettings", r#"[]"#),
    ] {
        de_line(ty, input);
    }
}
