//! L7 algo selection reference vectors (`rustref algo-select-vectors`): selection masks and
//! surfaces, magic wand, Color Range, modify, lasso polygons, the Magnetic Lasso tracer, Select and
//! Mask (matting), and the segmentation stack (max-flow, GMMs, SLIC, GrabCut, Quick Selection,
//! Sky, Select Subject, Focus Area). One `key value` line per case; values are FNV-1a hashes of
//! the outputs' bytes (f32/f64 bit patterns little-endian) or plain text. Replayed by
//! tests/algo/test_select_parity.c, which builds the same synthetic inputs.
use photocraft_algo::magnetic::{self, Settings, Tracer};
use photocraft_algo::matting::{self, RefineParams};
use photocraft_algo::segment::gmm::{self, Gmm};
use photocraft_algo::segment::maxflow::Graph;
use photocraft_algo::segment::{self, ImageSampler, Region, RgbImage, Rng, Sampler, SurfaceSampler};
use photocraft_algo::segment::{focus, grabcut, quick, sky, slic, subject};
use photocraft_algo::selection::{self, LabRange, RangeSample, SelectionMode};
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
fn ob(v: &[bool]) -> u64 {
    let mut h = H0;
    fnv(&mut h, &v.iter().map(|b| *b as u8).collect::<Vec<u8>>());
    h
}
fn o8(v: &[u8]) -> u64 {
    let mut h = H0;
    fnv(&mut h, v);
    h
}
fn opts(v: &[[f64; 2]]) -> u64 {
    let mut h = H0;
    hu(&mut h, &[v.len() as u64]);
    for p in v {
        hd(&mut h, p);
    }
    h
}
fn oreg(r: &Option<Region>) -> String {
    match r {
        None => "none".into(),
        Some(r) => {
            let mut h = H0;
            hrect(&mut h, r.bbox);
            fnv(&mut h, &r.mask);
            format!("{h:016x}")
        }
    }
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
fn osurf(s: &Option<Surface>) -> String {
    match s {
        None => "none".into(),
        Some(s) => format!("{:016x}", hs(s)),
    }
}
fn orgb(img: &RgbImage) -> u64 {
    let mut h = H0;
    hu(&mut h, &[img.w as u64, img.h as u64]);
    for p in &img.px {
        hf(&mut h, p);
    }
    h
}
fn ogmm(g: &Gmm) -> u64 {
    let mut h = H0;
    hu(&mut h, &[g.comps.len() as u64]);
    for c in &g.comps {
        hf(&mut h, &[c.weight]);
        hf(&mut h, &c.mean);
        for r in &c.cov {
            hf(&mut h, r);
        }
    }
    for i in 0..27 {
        let z = [(i % 3) as f32 / 2.0, ((i / 3) % 3) as f32 / 2.0, (i / 9) as f32 / 2.0];
        hd(&mut h, &[g.log_prob(z)]);
        hf(&mut h, &[g.neg_log(z)]);
        hu(&mut h, &[g.component_of(z) as u64]);
    }
    h
}

macro_rules! line {
    ($($t:tt)*) => { println!($($t)*) };
}

// ---------------------------------------------------------------------------------------------
// Synthetic inputs (tests/algo/test_select_parity.c builds the same).

/// Deterministic sample in 0..=1.
fn pat(x: i32, y: i32, c: usize, seed: i32) -> f32 {
    ((x as i64 * 7 + y as i64 * 13 + c as i64 * 29 + seed as i64 * 31).rem_euclid(97)) as f32 / 96.0
}
/// Blocky RGBA: flat patches with pattern noise, some transparent pixels.
fn rgba_img(w: i32, h: i32, seed: i32) -> Vec<[f32; 4]> {
    let mut v = Vec::new();
    for y in 0..h {
        for x in 0..w {
            let b = ((x / 4 + y / 3 + seed) % 3) as f32 / 2.0;
            let n = pat(x, y, 0, seed) * 0.05;
            let a = if (x * 3 + y * 5 + seed).rem_euclid(17) == 0 { 0.0 } else { 1.0 };
            v.push([b + n, 1.0 - b, (b * 0.5 + pat(x, y, 2, seed) * 0.1).min(1.0), a]);
        }
    }
    v
}
fn blobs(w: i32, h: i32) -> Vec<[u8; 4]> {
    (0..w * h)
        .map(|i| {
            let (x, y) = (i % w, i / w);
            let v = ((x * 7 + y * 3) % 23 + if (x / 5 + y / 4) % 3 == 0 { 120 } else { 0 }) as u8;
            [v, v / 2, 255 - v, if x % 11 == 0 { 128 } else { 255 }]
        })
        .collect()
}
/// A disc with a fractional fringe and a few specks.
fn mask_field(w: i32, h: i32, seed: i32) -> Vec<f32> {
    let (cx, cy, r) = (w as f32 * 0.45, h as f32 * 0.5, (w.min(h) as f32) * 0.3);
    let mut v = Vec::new();
    for y in 0..h {
        for x in 0..w {
            let (dx, dy) = (x as f32 + 0.5 - cx, y as f32 + 0.5 - cy);
            let d = (dx * dx + dy * dy).sqrt() - r;
            let mut m = (0.5 - d).clamp(0.0, 1.0);
            if (x * 5 + y * 7 + seed).rem_euclid(41) == 0 {
                m = 1.0;
            }
            v.push(m);
        }
    }
    v
}
/// The segment tests' disc scene.
fn disc_scene(w: usize, h: usize, cx: f32, cy: f32, r: f32, seed: u64) -> RgbImage {
    let mut rng = Rng::new(seed);
    let mut img = RgbImage::new(w, h);
    for y in 0..h {
        for x in 0..w {
            let (dx, dy) = (x as f32 + 0.5 - cx, y as f32 + 0.5 - cy);
            let inside = dx * dx + dy * dy <= r * r;
            let n = 0.05 * rng.normal();
            img.px[y * w + x] = if inside {
                let t = (((x + y) as f32 * 0.35).sin() * 0.5 + 0.5).clamp(0.0, 1.0);
                [0.85 + 0.05 * t + n, 0.2 + 0.3 * t + n, 0.15 + n]
            } else {
                [0.25 + n, 0.45 + n, 0.7 + n]
            }
            .map(|v| v.clamp(0.0, 1.0));
        }
    }
    img
}
/// The segment tests' two textured regions with an edge at x = 60.
fn two_regions(w: usize, h: usize) -> RgbImage {
    let mut rng = Rng::new(5);
    RgbImage::from_fn(w, h, |x, y| {
        let n = 0.04 * rng.normal();
        let t = ((x as f32 * 0.5).sin() * (y as f32 * 0.3).cos()) * 0.05;
        if x < 60 { [0.8 + t + n, 0.5 + n, 0.3 + t] } else { [0.2 + n, 0.35 + t, 0.6 + n] }.map(|v| v.clamp(0.0, 1.0))
    })
}
/// The sky tests' landscape.
fn landscape(w: usize, h: usize) -> RgbImage {
    let horizon = h * 3 / 5;
    RgbImage::from_fn(w, h, |x, y| {
        let (cx, cy, rr) = (w as f32 * 0.7, h as f32 * 0.35, h as f32 * 0.18);
        let in_crown = ((x as f32 - cx).powi(2) + (y as f32 - cy).powi(2)).sqrt() < rr;
        let in_trunk = (x as f32 - cx).abs() < 3.0 && y as f32 > cy && y < horizon;
        let n = (((x * 7919 + y * 104_729) % 97) as f32 / 97.0 - 0.5) * 0.25;
        if in_crown {
            [0.1 + n * 0.5, 0.35 + n, 0.1 + n * 0.5]
        } else if in_trunk {
            [0.3 + n * 0.3, 0.2, 0.1]
        } else if y < horizon {
            let t = y as f32 / horizon as f32;
            [0.25 + 0.4 * t, 0.5 + 0.3 * t, 0.9 + 0.05 * t]
        } else {
            [0.35 + n, 0.3 + n, 0.15 + n * 0.5]
        }
    })
}
/// A surface of `fmt` with pattern pixels over `r`.
fn surf(fmt: PixelFormat, r: Rect, seed: i32) -> Surface {
    let mut s = Surface::new(fmt);
    let n = fmt.channels();
    let mut v = Vec::new();
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            for c in 0..n {
                v.push(if fmt.alpha && c == n - 1 { ((x * 3 + y * 5 + seed).rem_euclid(7)) as f32 / 6.0 } else { pat(x, y, c, seed) });
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
        ("cmyka8", PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true)),
        ("laba16", PixelFormat::new(ColorMode::Lab, SampleType::U16, true)),
    ]
}

// ---------------------------------------------------------------------------------------------

fn selection_cases() {
    for s in ["replace", "add", "subtract", "intersect", "x", ""] {
        line!("sel.mode.{s} {:?}", SelectionMode::parse(s));
    }
    // Surfaces <-> masks, combine.
    let areas = [Rect::new(0, 0, 40, 30), Rect::new(-37, 250, 300, 263), Rect::new(250, -3, 262, 300)];
    for (ai, area) in areas.iter().enumerate() {
        let (w, h) = (area.width() as i32, area.height() as i32);
        let m = mask_field(w, h, ai as i32);
        let old = selection::mask_to_surface(&m, *area);
        line!("sel.to_surface.{ai} {:016x} {}", hs(&old), old.tile_count());
        line!("sel.from_surface.{ai} {:016x}", of(&selection::mask_from_surface(Some(&old), *area)));
        let rgba = surf(PixelFormat::new(ColorMode::Rgb, SampleType::U16, true), *area, 3);
        line!("sel.from_surface_rgba.{ai} {:016x}", of(&selection::mask_from_surface(Some(&rgba), *area)));
        let new: Vec<f32> = (0..w * h).map(|i| pat(i % w, i / w, 1, ai as i32) * 1.2 - 0.1).collect();
        for mode in [SelectionMode::Replace, SelectionMode::Add, SelectionMode::Subtract, SelectionMode::Intersect] {
            for (pi, prev) in [None, Some(&old)].into_iter().enumerate() {
                line!("sel.combine.{ai}.{mode:?}.{pi} {}", osurf(&selection::combine(prev, &new, *area, mode)));
            }
        }
    }
    line!("sel.combine.empty {}", osurf(&selection::combine(None, &[], Rect::EMPTY, SelectionMode::Replace)));
    // Magic wand (float) and wand_region (8-bit).
    let area = Rect::new(3, -2, 40, 27);
    let (w, h) = (37, 29);
    let px = rgba_img(w, h, 2);
    for (k, (seed, tol, cont, aa)) in
        [((10, 5), 10.0, true, true), ((10, 5), 30.0, false, false), ((20, 20), 0.0, true, false), ((39, 26), 80.0, true, true), ((50, 3), 5.0, true, true)]
            .into_iter()
            .enumerate()
    {
        line!("sel.wand.{k} {:016x}", of(&selection::magic_wand(&px, area, seed, tol, cont, aa)));
    }
    let img = blobs(w, h);
    for (k, (seed, tol, cont, aa)) in
        [((10, 5), 10.0, true, true), ((10, 5), 10.0, true, false), ((20, 20), 40.0, false, true), ((4, -2), 0.0, true, true), ((30, 10), 255.0, true, true), ((0, 0), 1.0, true, true)]
            .into_iter()
            .enumerate()
    {
        let r = selection::wand_region(&img, area, seed, tol, cont, aa);
        line!("sel.wand_region.{k} {}", oreg(&r));
        if let Some(r) = &r {
            let mut old = Surface::new(PixelFormat::GRAY8);
            old.fill_rect(Rect::new(5, 0, 25, 20), &[1.0]);
            old.fill_rect(Rect::new(8, 8, 12, 10), &[0.4]);
            for mode in [SelectionMode::Replace, SelectionMode::Add, SelectionMode::Subtract, SelectionMode::Intersect] {
                line!("sel.combine_region.{k}.{mode:?} {}", osurf(&selection::combine_region(Some(&old), Some(r), mode)));
                line!("sel.combine_region_none.{k}.{mode:?} {}", osurf(&selection::combine_region(None, Some(r), mode)));
            }
        }
    }
    // rgba8_image over formats.
    for (name, fmt) in formats() {
        let s = surf(fmt, Rect::new(-5, -3, 30, 300), 4);
        let v = selection::rgba8_image(&s, Rect::new(-7, -4, 28, 290));
        let flat: Vec<u8> = v.iter().flatten().copied().collect();
        line!("sel.rgba8.{name} {:016x}", o8(&flat));
    }
    // Color Range variants.
    let px = rgba_img(16, 9, 5);
    for (k, fz) in [0.0f32, 10.0, 40.0, 200.0, -5.0, f32::NAN].into_iter().enumerate() {
        line!("sel.color_range.{k} {:016x}", of(&selection::color_range(&px, [0.5, 0.5, 0.1], fz)));
        let samples = [
            RangeSample { color: [0.0, 1.0, 0.0], at: Some((2.0, 3.0)) },
            RangeSample { color: [1.0, 0.0, 0.05], at: None },
            RangeSample { color: [0.5, 0.5, 0.25], at: Some((12.0, 1.0)) },
        ];
        for (li, loc) in [None, Some(4.0f32), Some(0.5)].into_iter().enumerate() {
            line!("sel.color_samples.{k}.{li} {:016x}", of(&selection::color_range_samples(&px, 16, &samples, fz, loc)));
        }
    }
    let lab_px: Vec<[f32; 4]> =
        (0..12 * 7).map(|i| [pat(i, 1, 0, 3) * 100.0, pat(i, 2, 1, 3) * 80.0 - 40.0, pat(i, 3, 2, 3) * 60.0 - 30.0, if i % 13 == 0 { 0.0 } else { 1.0 }]).collect();
    let range = LabRange::spanning(&[[50.0, 0.0, -5.0], [60.0, 10.0, 5.0]]).unwrap();
    for (k, fz) in [0.0f32, 1.0, 10.0, 40.0, 150.0].into_iter().enumerate() {
        for grid8 in [false, true] {
            line!("sel.lab.{k}.{grid8} {:016x}", of(&selection::color_range_lab(&lab_px, 12, &range, fz, grid8, None)));
            let at = [(1.0, 1.0), (10.0, 5.0)];
            line!("sel.lab_loc.{k}.{grid8} {:016x}", of(&selection::color_range_lab(&lab_px, 12, &range, fz, grid8, Some((5.0, &at)))));
        }
        let mut h = H0;
        for p in &lab_px {
            hf(&mut h, &[selection::lab_range_coverage([p[0], p[1], p[2]], &range, fz), selection::lab_range_coverage_8bit([p[0], p[1], p[2]], &range, fz)]);
            hf(&mut h, &selection::quantize_lab8([p[0], p[1], p[2]]));
        }
        line!("sel.lab_cov.{k} {h:016x}");
    }
    let mut h = H0;
    for i in 0..60 {
        let r0 = LabRange { lo: [40.0, -10.0, 0.0], hi: [60.0, 10.0, 20.0] };
        let m = [pat(i, 0, 0, 1) * 100.0, pat(i, 0, 1, 1) * 60.0 - 30.0, pat(i, 0, 2, 1) * 60.0 - 20.0];
        let fz = [0.0, 10.0, 40.0, 80.0, f32::NAN][(i % 5) as usize];
        let r = r0.subtract(&m, fz).grow(&[m[0] * 0.5, m[1], -m[2]]);
        hf(&mut h, &r.lo);
        hf(&mut h, &r.hi);
    }
    line!("sel.lab_subtract {h:016x}");
    for c in [0.0f32, 60.0, 120.0, 180.0, 240.0, 300.0] {
        line!("sel.hue.{c} {:016x}", of(&selection::hue_range(&px, c)));
    }
    for (k, (lo, hi_, f)) in [(0.0, 65.0, 20.0), (190.0, 255.0, 20.0), (105.0, 150.0, 0.0), (50.0, 40.0, -1.0)].into_iter().enumerate() {
        line!("sel.tone.{k} {:016x}", of(&selection::tone_range(&px, lo, hi_, f)));
    }
    // Modify.
    for (k, (w, h)) in [(20usize, 20usize), (37, 23), (1, 9), (64, 1)].into_iter().enumerate() {
        let m = mask_field(w as i32, h as i32, k as i32);
        let inside: Vec<bool> = m.iter().map(|v| *v >= 0.5).collect();
        line!("sel.edt.{k} {:016x}", of(&selection::edt(&inside, w, h)));
        for r in [0.0f32, 1.0, 2.5, 7.0] {
            line!("sel.expand.{k}.{r} {:016x}", of(&selection::expand(&m, w, h, r)));
            line!("sel.contract.{k}.{r} {:016x}", of(&selection::contract(&m, w, h, r)));
            line!("sel.border.{k}.{r} {:016x}", of(&selection::border(&m, w, h, r)));
        }
    }
    let empty_inside = vec![false; 12];
    line!("sel.edt.none {:016x}", of(&selection::edt(&empty_inside, 4, 3)));
    // Polygons.
    let polys: Vec<Vec<(f32, f32)>> = vec![
        vec![(2.0, 2.0), (12.0, 2.0), (12.0, 12.0), (2.0, 12.0)],
        vec![(0.0, 0.0), (10.0, 0.0), (0.0, 10.0)],
        vec![(3.3, -4.0), (25.7, 6.1), (14.2, 19.9), (-3.0, 11.5), (9.0, 8.0)],
        vec![(5.0, 5.0), (15.0, 5.0), (5.0, 15.0), (15.0, 15.0)],
        vec![(0.0, 0.0), (1.0, 1.0)],
    ];
    for (k, p) in polys.iter().enumerate() {
        for aa in [true, false] {
            line!("sel.polygon.{k}.{aa} {:016x}", of(&selection::polygon(p, Rect::new(-2, -1, 20, 18), aa)));
        }
    }
}

fn magnetic_cases() {
    let disc = |x: i32, y: i32| -> [u8; 4] {
        let v = if (f64::from(x) + 0.5 - 50.0).hypot(f64::from(y) + 0.5 - 50.0) < 30.0 { 255 } else { 0 };
        [v, v, v, 255]
    };
    let mut fetch = |r: Rect| -> Vec<[u8; 4]> { (r.y0..r.y1).flat_map(|y| (r.x0..r.x1).map(move |x| (x, y))).map(|(x, y)| disc(x, y)).collect() };
    let mut t = Tracer::new(Rect::new(0, 0, 100, 100));
    let mut h = H0;
    for i in 0..40 {
        let p = [f64::from(pat(i, 1, 0, 2)) * 100.0, f64::from(pat(i, 2, 1, 2)) * 100.0];
        for s in [Settings::default(), Settings::new(3.0, 0.5), Settings::new(30.0, 0.02)] {
            match t.snap(&mut fetch, p, s) {
                Some(q) => hd(&mut h, &q),
                None => hu(&mut h, &[7]),
            }
        }
    }
    line!("mag.snap.disc {h:016x}");
    let s = Settings::default();
    let a = t.snap(&mut fetch, [50.0, 21.0], s).unwrap();
    let b = t.snap(&mut fetch, [79.0, 50.0], s).unwrap();
    line!("mag.trace.disc {:016x}", opts(&t.trace(&mut fetch, a, b, &[], s)));
    line!("mag.trace.disc_guide {:016x}", opts(&t.trace(&mut fetch, [20.0, 50.0], [50.0, 80.0], &[[25.0, 70.0], [35.0, 78.0]], Settings::new(12.0, 0.2))));
    line!("mag.cached.disc {}", t.cached_pixels());
    // Steps and gradients.
    let step = |x: i32, y: i32| -> [u8; 4] {
        let v = [60u8, 100, 255][usize::from(y >= 20) + usize::from(y >= 40)];
        [v, (v / 2).wrapping_add((x % 7) as u8), v, if x % 13 == 0 { 200 } else { 255 }]
    };
    let mut fetch2 = |r: Rect| -> Vec<[u8; 4]> { (r.y0..r.y1).flat_map(|y| (r.x0..r.x1).map(move |x| (x, y))).map(|(x, y)| step(x, y)).collect() };
    let mut t2 = Tracer::new(Rect::new(0, 0, 300, 60));
    line!("mag.trace.step {:016x}", opts(&t2.trace(&mut fetch2, [10.0, 22.0], [290.0, 18.0], &[[60.0, 23.0], [150.0, 25.0], [200.0, 15.0]], Settings::new(10.0, 0.1))));
    line!("mag.trace.step_long {:016x}", opts(&t2.trace(&mut fetch2, [1.0, 41.0], [299.0, 39.0], &[], Settings::new(6.0, 0.05))));
    line!("mag.trace.hostile {:016x}", opts(&t2.trace(&mut fetch2, [-1e300, -1e9], [1e9, 1e300], &[[f64::NAN, 3.0], [1e12, -5.0]], Settings { width: f64::NAN, contrast: f32::NAN })));
    let mut bad = |_: Rect| vec![[255u8; 4]; 3];
    let mut t3 = Tracer::new(Rect::new(0, 0, 300, 300));
    line!("mag.trace.bad {:016x}", opts(&t3.trace(&mut bad, [10.0, 10.0], [290.0, 280.0], &[], s)));
    // Polyline helpers.
    let g: Vec<[f64; 2]> = (0..50).map(|i| [f64::from(i) * 3.0, f64::from(pat(i, 0, 0, 1)) * 2.0]).collect();
    for tol in [0.0, 0.3, 1.0, 5.0] {
        line!("mag.simplify.{tol} {:016x}", opts(&magnetic::simplify(&g, tol)));
    }
    line!("mag.length {:016x}", {
        let mut h = H0;
        hd(&mut h, &[magnetic::length(&g), magnetic::length(&[]), magnetic::length(&g[..1])]);
        h
    });
}

fn matting_cases() {
    let (w, h) = (23usize, 17usize);
    let src: Vec<f32> = (0..w * h).map(|i| pat(i as i32 % 23, i as i32 / 23, 0, 4)).collect();
    for r in [0usize, 1, 2, 5, 30] {
        line!("mat.box_mean.{r} {:016x}", of(&matting::box_mean(&src, w, h, r)));
    }
    let img = RgbImage::from_fn(w, h, |x, y| [pat(x as i32, y as i32, 0, 1), pat(x as i32, y as i32, 1, 1), pat(x as i32, y as i32, 2, 1)]);
    let guide: Vec<f32> = img.px.iter().map(|p| 0.3 * p[0] + 0.6 * p[1] + 0.1 * p[2]).collect();
    let m = mask_field(w as i32, h as i32, 2);
    for r in [1usize, 3] {
        for eps in [1e-4f32, 1e-2] {
            line!("mat.gf_gray.{r}.{eps} {:016x}", of(&matting::guided_filter_gray(&guide, &m, w, h, r, eps)));
            line!("mat.gf_color.{r}.{eps} {:016x}", of(&matting::guided_filter_color(&img, &m, r, eps)));
        }
    }
    for sigma in [0.0f32, 0.5, 1.3, 4.0] {
        line!("mat.gauss.{sigma} {:016x}", of(&matting::gaussian_blur(&m, w, h, sigma)));
    }
    for r in [0usize, 1, 2, 5] {
        for grow in [true, false] {
            line!("mat.morph.{r}.{grow} {:016x}", of(&matting::morph(&m, w, h, r, grow)));
        }
    }
    for r in [1usize, 2, 4] {
        line!("mat.edge_width.{r} {:016x}", of(&matting::edge_width(&img, r)));
    }
    let params = [
        RefineParams { radius: 10.0, ..Default::default() },
        RefineParams { radius: 6.0, smart_radius: true, ..Default::default() },
        RefineParams { feather: 3.0, contrast: 40.0, ..Default::default() },
        RefineParams { smooth: 50.0, shift_edge: 60.0, ..Default::default() },
        RefineParams { radius: 3.0, shift_edge: -50.0, smooth: 20.0, feather: 1.0, contrast: 10.0, smart_radius: false },
        RefineParams::default(),
    ];
    let (bw, bh) = (60usize, 30usize);
    let edge = RgbImage::from_fn(bw, bh, |x, y| {
        let t = ((x as f32 - 30.0) / 4.0).clamp(-1.0, 1.0) * 0.5 + 0.5 + pat(x as i32, y as i32, 0, 6) * 0.02;
        [0.9 * (1.0 - t) + 0.1 * t, 0.8 * (1.0 - t) + 0.2 * t, 0.2 * (1.0 - t) + 0.6 * t]
    });
    let bm: Vec<f32> = (0..bw * bh).map(|i| if (i % bw) < 30 + (i / bw) % 3 { 1.0 } else { 0.0 }).collect();
    for (k, p) in params.iter().enumerate() {
        line!("mat.halo.{k} {} {}", p.halo(), p.spread());
        line!("mat.refine.{k} {:016x}", of(&matting::refine_buffer(Some(&edge), &bm, bw, bh, p)));
        line!("mat.refine_noimg.{k} {:016x}", of(&matting::refine_buffer(None, &bm, bw, bh, p)));
    }
    // Tiled refine through a sampler and a region reader.
    let (fw, fh) = (300usize, 280usize);
    let big = disc_scene(fw, fh, 150.0, 140.0, 90.0, 7);
    let sampler = ImageSampler { img: &big, origin: (0, 0) };
    let mut mask = vec![0u8; fw * fh];
    for y in 0..fh {
        for x in 0..fw {
            let (dx, dy) = (x as f32 + 0.5 - 150.0, y as f32 + 0.5 - 140.0);
            mask[y * fw + x] = if dx * dx + dy * dy < 88.0 * 88.0 { 255 } else if dx * dx + dy * dy < 92.0 * 92.0 { 128 } else { 0 };
        }
    }
    let reg = Region { bbox: Rect::new(0, 0, fw as i32, fh as i32), mask };
    let canvas = reg.bbox;
    for (k, p) in params.iter().enumerate() {
        let r = matting::refine_mask(&sampler, &matting::region_reader(&reg), Rect::new(40, 30, 260, 250), canvas, p);
        line!("mat.refine_mask.{k} {}", oreg(&r));
    }
    let surf_mask = matting::region_surface(&reg);
    line!("mat.region_surface {:016x}", hs(&surf_mask));
    let r = matting::refine_mask(&sampler, &matting::surface_reader(&surf_mask), Rect::new(40, 30, 260, 250), Rect::new(10, 10, 290, 270), &params[2]);
    line!("mat.refine_mask.surface {}", oreg(&r));
    // Decontaminate / masked copy over formats.
    let alpha_mask: Vec<u8> = (0..40 * 24).map(|i| ((i % 40) * 255 / 39).min(255) as u8).map(|v| if v < 90 { 0 } else if v > 200 { 255 } else { v }).collect();
    let alpha = Region { bbox: Rect::new(-5, 3, 35, 27), mask: alpha_mask };
    for (name, fmt) in formats() {
        let s = surf(fmt, Rect::new(-20, -10, 50, 40), 8);
        for (k, (rad, amt)) in [(3.0f32, 100.0f32), (1.0, 50.0), (6.5, 75.0)].into_iter().enumerate() {
            line!("mat.decon.{name}.{k} {:016x}", hs(&matting::decontaminate(&s, &alpha, rad, amt)));
        }
        line!("mat.masked.{name} {:016x}", hs(&matting::masked_copy(&s, &alpha)));
    }
}

fn segment_core_cases() {
    // RgbImage, samplers.
    let rgba = rgba_img(13, 11, 1);
    let img = RgbImage::from_rgba(&rgba, 13, 11);
    line!("seg.from_rgba {:016x}", orgb(&img));
    for step in [1usize, 2, 3, 5, 20] {
        line!("seg.downsample.{step} {:016x}", orgb(&img.downsample(step)));
    }
    let scene = disc_scene(150, 133, 70.0, 60.0, 40.0, 2);
    line!("seg.disc_scene {:016x}", orgb(&scene));
    let s = ImageSampler { img: &scene, origin: (5, 3) };
    for (k, (r, step)) in [(Rect::new(5, 3, 155, 136), 4usize), (Rect::new(0, 0, 160, 140), 3), (Rect::new(20, 30, 21, 200), 7), (Rect::new(10, 10, 60, 50), 1)].into_iter().enumerate() {
        line!("seg.rgb_scaled.{k} {:016x}", orgb(&s.rgb_scaled(r, step)));
    }
    for (name, fmt) in formats() {
        let sf = surf(fmt, Rect::new(0, 0, 40, 30), 2);
        line!("seg.surface_sampler.{name} {:016x}", orgb(&SurfaceSampler(&sf).rgb_scaled(Rect::new(-3, -2, 41, 33), 2)));
    }
    for (r, m) in [(Rect::new(0, 0, 640, 480), 20_000usize), (Rect::new(0, 0, 100, 100), 1_000_000), (Rect::new(0, 0, 7, 3), 0)] {
        line!("seg.scale_for.{}x{}.{m} {}", r.width(), r.height(), segment::scale_for(r, m));
    }
    line!("seg.beta {:016x}", of(&[segment::contrast_beta(&scene), segment::contrast_beta(&RgbImage::new(4, 4)), segment::contrast_beta(&RgbImage::new(0, 0))]));
    // Rng.
    let mut rng = Rng::new(42);
    let mut h = H0;
    for _ in 0..50 {
        hu(&mut h, &[rng.next_u64()]);
        hf(&mut h, &[rng.f32(), rng.normal()]);
    }
    line!("seg.rng {h:016x}");
    // Max-flow on random graphs.
    let mut rng = Rng::new(7);
    let mut h = H0;
    for trial in 0..60 {
        let n = 3 + (trial % 12);
        let mut g = Graph::with_capacity(n, n * 3);
        for i in 0..n {
            let (s, t) = ((rng.f32() * 10.0).round(), (rng.f32() * 10.0).round());
            let (s, t) = (if rng.f32() < 0.4 { 0.0 } else { s }, if rng.f32() < 0.4 { 0.0 } else { t });
            g.add_tweights(i, s, t);
        }
        for i in 0..n {
            for j in i + 1..n {
                if rng.f32() < 0.35 {
                    let (a, b) = ((rng.f32() * 8.0).round() + rng.f32() * 0.25, (rng.f32() * 8.0).round());
                    g.add_edge(i, j, a, b);
                }
            }
        }
        hd(&mut h, &[g.maxflow()]);
        for i in 0..n {
            fnv(&mut h, &[g.in_source(i) as u8]);
        }
        hf(&mut h, &[g.residual_out_of_source_side()]);
    }
    line!("seg.maxflow {h:016x}");
    // Grid cut, components, cleaning.
    let (w, hh) = (scene.w, scene.h);
    let fg = Gmm::fit(&scene.px[..3000], 3, gmm::DEFAULT_REG).unwrap();
    let bg = Gmm::fit(&scene.px[15000..], 4, gmm::DEFAULT_REG).unwrap();
    let (cf, cb) = segment::data_costs(&scene, &fg, &bg);
    line!("seg.data_costs {:016x} {:016x}", of(&cf), of(&cb));
    let mut fixed = vec![segment::FREE; w * hh];
    fixed[60 * w + 70] = segment::HARD_FG;
    fixed[5 * w + 5] = segment::HARD_BG;
    fixed[120 * w + 140] = segment::HARD_BG;
    let beta = segment::contrast_beta(&scene);
    let cut = segment::grid_cut(&scene, &cf, &cb, &fixed, 50.0, beta);
    line!("seg.grid_cut {:016x}", ob(&cut));
    let zeros = vec![0.0; w * hh];
    line!("seg.grid_cut_zero {:016x}", ob(&segment::grid_cut(&scene, &zeros, &zeros, &fixed, 50.0, beta)));
    let (lab, sizes) = segment::components(&cut, w, hh);
    let mut h = H0;
    hu(&mut h, &sizes.iter().map(|v| *v as u64).collect::<Vec<_>>());
    fnv(&mut h, &lab.iter().flat_map(|v| v.to_le_bytes()).collect::<Vec<u8>>());
    line!("seg.components {h:016x}");
    let pm: Vec<bool> = (0..w * hh).map(|i| pat((i % w) as i32, (i / w) as i32, 0, 3) > 0.55).collect();
    let seeds: Vec<bool> = (0..w * hh).map(|i| i % 97 == 0).collect();
    line!("seg.keep_seeded {:016x}", ob(&segment::keep_seeded(&pm, &seeds, w, hh)));
    for (frac, hole) in [(0.1f32, 0.002f32), (0.0, 0.5), (0.5, 0.0)] {
        line!("seg.clean.{frac}.{hole} {:016x}", ob(&segment::clean_mask(&pm, w, hh, frac, hole)));
        line!("seg.clean_cut.{frac}.{hole} {:016x}", ob(&segment::clean_mask(&cut, w, hh, frac, hole)));
    }
    let mut m8: Vec<u8> = cut.iter().map(|b| if *b { 255 } else { 0 }).collect();
    segment::antialias_u8(&mut m8, w, hh);
    line!("seg.antialias {:016x}", o8(&m8));
    let r = segment::trim_region(Region { bbox: Rect::new(-4, 7, -4 + w as i32, 7 + hh as i32), mask: m8.clone() });
    line!("seg.trim {}", oreg(&r));
    line!("seg.trim_empty {}", oreg(&segment::trim_region(Region { bbox: Rect::new(0, 0, 3, 2), mask: vec![0; 6] })));
    // finish_region with and without models, step 1 and > 1.
    let low: Vec<bool> = (0..38 * 34).map(|i| {
        let (x, y) = ((i % 38) as f32 + 0.5 - 17.5, (i / 38) as f32 + 0.5 - 15.0);
        x * x + y * y < 100.0
    }).collect();
    let s0 = ImageSampler { img: &scene, origin: (0, 0) };
    line!("seg.finish.step4 {}", oreg(&segment::finish_region(&s0, Rect::new(0, 0, 150, 133), 4, &low, 38, 34, Some((&fg, &bg)))));
    line!("seg.finish.step4_nomodel {}", oreg(&segment::finish_region(&s0, Rect::new(0, 0, 150, 133), 4, &low, 38, 34, None)));
    line!("seg.finish.step1 {}", oreg(&segment::finish_region(&s0, Rect::new(0, 0, 38, 34), 1, &low, 38, 34, None)));
    // Subsample.
    let v: Vec<[f32; 3]> = scene.px[..1000].to_vec();
    for m in [1000usize, 999, 7, 1] {
        let s = segment::subsample(&v, m);
        let mut h = H0;
        hu(&mut h, &[s.len() as u64]);
        for p in &s {
            hf(&mut h, p);
        }
        line!("seg.subsample.{m} {h:016x}");
    }
    // GMM fitting.
    let mut rng = Rng::new(3);
    let truth = [([0.2f32, 0.3, 0.8], 0.03f32, 0.5f32), ([0.8, 0.2, 0.1], 0.05, 0.3), ([0.5, 0.9, 0.4], 0.02, 0.2)];
    let mut samples = Vec::new();
    for (mean, sd, wt) in truth {
        for _ in 0..(wt * 3000.0) as usize {
            samples.push(mean.map(|m| m + sd * rng.normal()));
        }
    }
    for k in [1usize, 3, 5] {
        let g = Gmm::fit(&samples, k, gmm::DEFAULT_REG).unwrap();
        line!("seg.gmm.fit.{k} {:016x}", ogmm(&g));
        line!("seg.gmm.refit.{k} {:016x}", ogmm(&g.refit(&samples).unwrap()));
    }
    line!("seg.gmm.fit_em0 {:016x}", ogmm(&Gmm::fit_em(&samples, 4, 1e-4, 0).unwrap()));
    line!("seg.gmm.fit_em6 {:016x}", ogmm(&Gmm::fit_em(&samples, 2, 1e-3, 6).unwrap()));
    line!("seg.gmm.few {:016x}", ogmm(&Gmm::fit(&samples[..2], 5, gmm::DEFAULT_REG).unwrap()));
    line!("seg.gmm.flat {}", Gmm::fit(&vec![[0.5f32; 3]; 10], 3, 0.0).map_or("none".to_string(), |g| format!("{:016x}", ogmm(&g))));
    line!("seg.gmm.empty {}", Gmm::fit(&[], 3, 0.0).is_none());
    line!("seg.gmm.broad {:016x}", ogmm(&quick::broad_model()));
    let labels: Vec<usize> = (0..samples.len()).map(|i| i % 4).collect();
    line!("seg.gmm.labels {:016x}", ogmm(&Gmm::from_labels(&samples, &labels, 4, gmm::DEFAULT_REG).unwrap()));
    for k in [1usize, 2, 7, 40] {
        let c = gmm::kmeans(&samples, k, 10);
        let mut h = H0;
        hu(&mut h, &[c.len() as u64]);
        for p in &c {
            hf(&mut h, p);
        }
        line!("seg.kmeans.{k} {h:016x}");
    }
}

fn segment_algo_cases() {
    // SLIC.
    let mut h = H0;
    for i in 0..64 {
        hf(&mut h, &slic::rgb_to_lab([pat(i, 0, 0, 1), pat(i, 0, 1, 1), pat(i, 0, 2, 1)]));
    }
    hf(&mut h, &slic::rgb_to_lab([0.0, 0.0, 0.0]));
    hf(&mut h, &slic::rgb_to_lab([1.0, 1.0, 1.0]));
    hf(&mut h, &slic::rgb_to_lab([0.01, 0.02, 0.03]));
    line!("seg.rgb_to_lab {h:016x}");
    let scene = disc_scene(120, 90, 60.0, 45.0, 22.0, 8);
    for (k, (target, m, iters)) in [(64usize, 10.0f32, 10usize), (16, 20.0, 3), (400, 5.0, 8), (1, 10.0, 1)].into_iter().enumerate() {
        let sp = slic::slic(&scene, target, m, iters);
        let mut h = H0;
        hu(&mut h, &[sp.count as u64]);
        hf(&mut h, &[sp.step]);
        fnv(&mut h, &sp.labels.iter().flat_map(|v| v.to_le_bytes()).collect::<Vec<u8>>());
        line!("seg.slic.{k} {h:016x}");
    }
    // GrabCut.
    let (img, _) = (disc_scene(120, 100, 60.0, 50.0, 30.0, 1), ());
    let mut tri = vec![grabcut::BG; img.w * img.h];
    for y in 12..88 {
        for x in 22..98 {
            tri[y * img.w + x] = grabcut::PR_FG;
        }
    }
    let models = grabcut::grabcut(&img, &mut tri, 8);
    line!("seg.grabcut {:016x} {}", o8(&tri), models.map_or("none".into(), |(f, b)| format!("{:016x} {:016x}", ogmm(&f), ogmm(&b))));
    let mut tri2 = vec![grabcut::PR_FG; 64];
    line!("seg.grabcut_onesided {}", grabcut::grabcut(&RgbImage::new(8, 8), &mut tri2, 3).is_none());
    let s = ImageSampler { img: &img, origin: (0, 0) };
    line!("seg.object_select.full {}", oreg(&grabcut::object_select(&s, Rect::new(0, 0, 120, 100), Rect::new(22, 12, 98, 88), 1_000_000)));
    let big = disc_scene(320, 240, 165.0, 125.0, 75.0, 3);
    let sb = ImageSampler { img: &big, origin: (0, 0) };
    line!("seg.object_select.band {}", oreg(&grabcut::object_select(&sb, Rect::new(0, 0, 320, 240), Rect::new(80, 40, 250, 210), 8_000)));
    line!("seg.object_select.all {}", oreg(&grabcut::object_select(&s, Rect::new(0, 0, 120, 100), Rect::new(-5, -5, 200, 200), 1_000_000)));
    line!("seg.object_select.tiny {}", oreg(&grabcut::object_select(&s, Rect::new(0, 0, 120, 100), Rect::new(5, 5, 6, 60), 1_000_000)));
    // Quick selection.
    let tr = two_regions(140, 100);
    line!("seg.two_regions {:016x}", orgb(&tr));
    let seeds = quick::stroke_mask(&[(20.0, 50.0)], 4.0, 140, 100);
    line!("seg.stroke_mask {:016x}", ob(&seeds));
    line!("seg.stroke_mask2 {:016x}", ob(&quick::stroke_mask(&[(3.5, 4.0), (30.25, 20.0), (-5.0, 60.0)], 2.5, 40, 50)));
    line!("seg.geodesic {:016x}", of(&quick::geodesic(&tr, &seeds)));
    let (qf, qb, low) = quick::segment(&tr, &seeds);
    line!("seg.quick_segment {:016x} {:016x} {:016x}", ogmm(&qf), ogmm(&qb), ob(&low));
    let st = ImageSampler { img: &tr, origin: (0, 0) };
    line!("seg.quick.grow {}", oreg(&quick::quick_select(&st, Rect::new(0, 0, 140, 100), &[(20.0, 50.0), (35.0, 50.0)], 8.0, quick::WORK_PX)));
    line!("seg.quick.off {}", oreg(&quick::quick_select(&st, Rect::new(0, 0, 140, 100), &[(3e9, 0.0)], 8.0, quick::WORK_PX)));
    let qs = disc_scene(300, 250, 150.0, 125.0, 90.0, 4);
    let sq = ImageSampler { img: &qs, origin: (0, 0) };
    line!("seg.quick.down {}", oreg(&quick::quick_select(&sq, Rect::new(0, 0, 300, 250), &[(130.0, 125.0), (170.0, 125.0)], 20.0, 5_000)));
    // Sky.
    let mut h = H0;
    for i in 0..125 {
        hf(&mut h, &[sky::colour_prior([(i % 5) as f32 / 4.0, ((i / 5) % 5) as f32 / 4.0, (i / 25) as f32 / 4.0])]);
    }
    line!("seg.sky.prior {h:016x}");
    let land = landscape(160, 100);
    line!("seg.sky.landscape {:016x}", orgb(&land));
    line!("seg.sky.likelihood {:016x}", of(&sky::likelihood(&land)));
    for t in [0.0f32, 50.0, 100.0] {
        line!("seg.sky.low.{t} {}", sky::sky_mask_low(&land, t).map_or("none".into(), |m| format!("{:016x}", ob(&m))));
    }
    let sl = ImageSampler { img: &land, origin: (0, 0) };
    for (k, p) in [sky::SkyParams::default(), sky::SkyParams { threshold: 20.0, softness: 0.0 }, sky::SkyParams { threshold: 80.0, softness: 100.0 }].into_iter().enumerate() {
        line!("seg.sky.select.{k} {}", oreg(&sky::select_sky(&sl, Rect::new(0, 0, 160, 100), p)));
    }
    let overcast = RgbImage::from_fn(64, 64, |_, y| if y < 30 { [0.85, 0.86, 0.88] } else { [0.2, 0.3, 0.1] });
    line!("seg.sky.overcast {}", oreg(&sky::select_sky(&ImageSampler { img: &overcast, origin: (0, 0) }, Rect::new(0, 0, 64, 64), sky::SkyParams::default())));
    // Subject.
    line!("seg.saliency {:016x}", of(&subject::saliency(&scene)));
    let v: Vec<f32> = (0..500).map(|i| pat(i, 3, 1, 2)).collect();
    line!("seg.otsu {:016x}", of(&[subject::otsu(&v), subject::otsu(&[]), subject::otsu(&[0.3; 10]), subject::otsu(&[f32::NAN, 2.0, -1.0])]));
    let sub = disc_scene(200, 150, 100.0, 75.0, 40.0, 6);
    line!("seg.subject {}", oreg(&subject::select_subject(&ImageSampler { img: &sub, origin: (0, 0) }, Rect::new(0, 0, 200, 150))));
    // Focus.
    let mut rng = Rng::new(12);
    let (fw, fh) = (160usize, 100usize);
    let noise: Vec<f32> = (0..fw * fh).map(|_| rng.f32()).collect();
    let blurred = matting::box_mean(&noise, fw, fh, 4);
    let fimg = RgbImage::from_fn(fw, fh, |x, y| {
        let v = if x < 80 { noise[y * fw + x] } else { blurred[y * fw + x] };
        [v, v * 0.8, 0.3]
    });
    for n in [0.0f32, 0.4] {
        line!("seg.sharpness.{n} {:016x}", of(&focus::sharpness(&fimg, n)));
    }
    let sf = ImageSampler { img: &fimg, origin: (0, 0) };
    for (range, n) in [(0.5f32, 0.0f32), (0.1, 0.3), (0.9, 0.0)] {
        line!("seg.focus.{range}.{n} {}", oreg(&focus::focus_area(&sf, Rect::new(0, 0, fw as i32, fh as i32), range, n)));
    }
}

pub fn run() {
    selection_cases();
    magnetic_cases();
    matting_cases();
    segment_core_cases();
    segment_algo_cases();
}
