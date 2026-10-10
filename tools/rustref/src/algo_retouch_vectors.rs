//! L8 algo retouch reference vectors (`rustref algo-retouch-vectors`): stack statistics, the
//! Poisson / membrane solver, inpainting (completion, proximity match, texture synthesis),
//! Content-Aware Fill, seam carving, red-eye, the retouch kernels, the Magic Eraser and the
//! gradient / paint-bucket helpers. One `key value` line per case; values are FNV-1a hashes of the
//! outputs' bytes (f32 bit patterns little-endian) or plain numbers. Replayed by
//! tests/algo/test_retouch_parity.c.
use photocraft_algo::selection::Region;
use photocraft_algo::*;
use photocraft_color::blend::BlendMode;
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
fn hi(h: &mut u64, v: &[i32]) {
    for x in v {
        fnv(h, &x.to_le_bytes());
    }
}
fn of(v: &[f32]) -> u64 {
    let mut h = H0;
    hf(&mut h, v);
    h
}
fn hpx(v: &[[f32; 4]]) -> u64 {
    let mut h = H0;
    for p in v {
        hf(&mut h, p);
    }
    h
}
fn hus(v: &[usize]) -> u64 {
    let mut h = H0;
    for x in v {
        fnv(&mut h, &(*x as u64).to_le_bytes());
    }
    h
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
/// Interleaved w x h x ch pattern buffer.
fn img(w: usize, h: usize, ch: usize, seed: i32) -> Vec<f32> {
    let mut v = Vec::with_capacity(w * h * ch);
    for y in 0..h {
        for x in 0..w {
            for c in 0..ch {
                v.push(pat(x as i32, y as i32, c, seed));
            }
        }
    }
    v
}
/// A smooth image (gradient plus a soft sinusoid), less tie-heavy than `img`.
fn smooth(w: usize, h: usize, ch: usize, seed: i32) -> Vec<f32> {
    let mut v = Vec::with_capacity(w * h * ch);
    for y in 0..h {
        for x in 0..w {
            for c in 0..ch {
                let t = (x as f32 * 0.37 + y as f32 * 0.21 + c as f32 + seed as f32).sin() * 0.15;
                v.push(0.3 + 0.004 * x as f32 + 0.003 * y as f32 + 0.05 * c as f32 + t);
            }
        }
    }
    v
}
/// Noise quantised to `levels` values (xorshift32), full of exact ties.
fn noise(len: usize, levels: u32, seed: u32) -> Vec<f32> {
    let mut s = seed | 1;
    (0..len)
        .map(|_| {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            (s % levels) as f32 / (levels - 1) as f32
        })
        .collect()
}
fn disc(w: usize, h: usize, cx: f32, cy: f32, r: f32) -> Vec<bool> {
    (0..w * h).map(|i| ((i % w) as f32 - cx).hypot((i / w) as f32 - cy) < r).collect()
}
fn rect_mask(w: usize, h: usize, x0: usize, y0: usize, x1: usize, y1: usize) -> Vec<bool> {
    (0..w * h).map(|i| (x0..x1).contains(&(i % w)) && (y0..y1).contains(&(i / w))).collect()
}
/// SplitMix64 texture of the inpaint tests (gradient plus fine noise), 3 channels.
fn texture(w: usize, h: usize) -> Vec<f32> {
    let mut s: u64 = 7 ^ 0x9E37_79B9_7F4A_7C15;
    let mut next = || {
        s = s.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = s;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    };
    (0..w * h)
        .flat_map(|i| {
            let (x, y) = ((i % w) as f32, (i / w) as f32);
            let n = (next() % 1000) as f32 / 1000.0 * 0.04 - 0.02;
            [0.3 + 0.004 * x + n, 0.5 + 0.002 * y + n, 0.4 + n]
        })
        .collect()
}

fn stack_vectors() {
    use stack::Stat;
    let stats = [
        ("Entropy", Stat::Entropy),
        ("Kurtosis", Stat::Kurtosis),
        ("Maximum", Stat::Maximum),
        ("Mean", Stat::Mean),
        ("Median", Stat::Median),
        ("Minimum", Stat::Minimum),
        ("Range", Stat::Range),
        ("Skewness", Stat::Skewness),
        ("StandardDeviation", Stat::StandardDeviation),
        ("Summation", Stat::Summation),
        ("Variance", Stat::Variance),
    ];
    for count in [1usize, 2, 3, 5, 8] {
        // Layers of differing lengths; alpha pattern makes some samples transparent.
        let layers: Vec<Vec<[f32; 4]>> = (0..count)
            .map(|k| {
                let n = 40 + (k * 7) % 13;
                (0..n)
                    .map(|i| {
                        let (x, y) = (i as i32, k as i32);
                        [pat(x, y, 0, 3), pat(x, y, 1, 3) * 1.2 - 0.1, pat(x, y, 2, 3), alpha_at(x, y, 1)]
                    })
                    .collect()
            })
            .collect();
        for (name, s) in stats {
            let out = stack::combine(&layers, s);
            if s == Stat::Entropy {
                // The reference sums a HashMap in random order: compare at 1e-4.
                let q: Vec<i32> = out.iter().flat_map(|p| p.iter().map(|v| (v * 1e4).round() as i32)).collect();
                let mut h = H0;
                hi(&mut h, &q);
                println!("stack {name}/{count} {} {h:016x}", out.len());
            } else {
                println!("stack {name}/{count} {} {:016x}", out.len(), hpx(&out));
            }
        }
    }
}

fn poisson_vectors() {
    let cases: [(usize, usize, f32, f32, f32); 6] =
        [(80, 60, 40.0, 30.0, 20.0), (96, 72, 45.0, 36.0, 28.0), (5, 5, 2.0, 2.0, 1.5), (33, 17, 0.0, 8.0, 9.0), (7, 40, 3.0, 20.0, 15.0), (64, 64, 32.0, 32.0, 60.0)];
    for (i, &(w, h, cx, cy, r)) in cases.iter().enumerate() {
        let unknown = disc(w, h, cx, cy, r);
        let init: Vec<f32> = (0..w * h).map(|i| ((i % w) as f32 * 0.21).sin() * 0.3 + ((i / w) as f32 * 0.13).cos() * 0.2 + 0.5).collect();
        let mut v = init.clone();
        poisson::solve_membrane(w, h, &unknown, &mut v);
        println!("poisson membrane/{i} {:016x}", of(&v));
        if w * h <= 2000 {
            let mut v = init.clone();
            poisson::solve_membrane_reference(w, h, &unknown, &mut v, 1e-5, 300);
            println!("poisson reference/{i} {:016x}", of(&v));
        }
        for ch in [1usize, 3, 4] {
            let src = smooth(w, h, ch, 1);
            let dst = img(w, h, ch, 2);
            println!("poisson clone/{i}/{ch} {:016x}", of(&poisson::seamless_clone(w, h, ch, &src, &dst, &unknown)));
            println!("poisson fill/{i}/{ch} {:016x}", of(&poisson::membrane_fill(w, h, ch, &dst, &unknown)));
        }
    }
    // All unknown (nothing to interpolate from) and nothing unknown.
    let (w, h) = (20, 10);
    let src = img(w, h, 2, 4);
    println!("poisson fill/all {:016x}", of(&poisson::membrane_fill(w, h, 2, &src, &vec![true; w * h])));
    println!("poisson fill/none {:016x}", of(&poisson::membrane_fill(w, h, 2, &src, &vec![false; w * h])));
}

fn inpaint_vectors() {
    use inpaint::CompleteParams;
    let show = |key: &str, r: Option<Vec<f32>>| match r {
        Some(v) => println!("inpaint {key} {:016x}", of(&v)),
        None => println!("inpaint {key} none"),
    };
    // Textured blemish (3 channels), stripes (1 channel), an edge-touching hole (4 channels).
    let (w, h) = (48, 40);
    let tex = texture(w, h);
    let hole = disc(w, h, 24.0, 20.0, 7.0);
    show("complete/tex", inpaint::complete(w, h, 3, &tex, &hole, &CompleteParams::default()));
    for r in [1usize, 2] {
        let p = CompleteParams { patch_radius: r, seed: 5, ..Default::default() };
        show(&format!("complete/tex/r{r}"), inpaint::complete(w, h, 3, &tex, &hole, &p));
    }
    let (w2, h2) = (80, 80);
    let stripes: Vec<f32> = (0..w2 * h2).map(|i| if (i % w2 / 4) % 2 == 0 { 0.2 } else { 0.8 }).collect();
    show("complete/stripes", inpaint::complete(w2, h2, 1, &stripes, &disc(w2, h2, 40.0, 40.0, 7.0), &CompleteParams::default()));
    let im4 = smooth(40, 36, 4, 3);
    show("complete/edge", inpaint::complete(40, 36, 4, &im4, &rect_mask(40, 36, 0, 10, 9, 20), &CompleteParams::default()));
    // A pyramid with refine-only levels and a big hole.
    let (w3, h3) = (120, 100);
    let im3 = smooth(w3, h3, 2, 1);
    let p = CompleteParams { refine_extent: 8, em_iters: 4, ..Default::default() };
    show("complete/pyramid", inpaint::complete(w3, h3, 2, &im3, &disc(w3, h3, 60.0, 50.0, 20.0), &p));
    show("complete/pyramid-default", inpaint::complete(w3, h3, 2, &im3, &disc(w3, h3, 60.0, 50.0, 20.0), &CompleteParams::default()));
    show("complete/nohole", inpaint::complete(10, 8, 1, &img(10, 8, 1, 1), &vec![false; 80], &CompleteParams::default()));
    show("complete/allhole", inpaint::complete(10, 8, 1, &img(10, 8, 1, 1), &vec![true; 80], &CompleteParams::default()));
    show("complete/tiny", inpaint::complete(6, 6, 1, &img(6, 6, 1, 1), &disc(6, 6, 3.0, 3.0, 1.0), &CompleteParams::default()));

    // Proximity match.
    let (w, h) = (80, 60);
    let per: Vec<f32> = (0..w * h).map(|i| ((i % w) as f32 * std::f32::consts::TAU / 10.0).sin() * 0.4 + 0.5).collect();
    let hole = disc(w, h, 40.0, 30.0, 5.0);
    let sm = smooth(w, h, 3, 2);
    for ring in [0usize, 1, 3, 6] {
        for maxr in [-1i32, 0, 1, 5, 20, 47, 200] {
            let a = inpaint::best_offset(w, h, 1, &per, &hole, ring, maxr);
            let b = inpaint::best_offset(w, h, 3, &sm, &hole, ring, maxr);
            let f = |o: Option<(i32, i32)>| o.map_or("none".to_string(), |(x, y)| format!("{x},{y}"));
            println!("inpaint offset/{ring}/{maxr} {} {}", f(a), f(b));
        }
    }
    println!("inpaint offset/nohole {}", inpaint::best_offset(w, h, 1, &per, &vec![false; w * h], 3, 20).is_none());

    // Texture synthesis.
    let (w, h) = (64, 64);
    let tex = texture(w, h);
    for block in [0usize, 4, 8, 13] {
        for seed in [3u64, 9] {
            println!("inpaint synth/{block}/{seed} {:016x}", of(&inpaint::synthesize(w, h, 3, &tex, &disc(w, h, 32.0, 32.0, 6.0), block, seed)));
        }
    }
    println!("inpaint synth/fallback {:016x}", of(&inpaint::synthesize(10, 10, 3, &texture(10, 10), &disc(10, 10, 5.0, 5.0, 2.0), 8, 1)));
}

fn content_aware_vectors() {
    use content_aware::{FillOptions, color_level, fill, rotation_level};
    for l in ["none", "low", "medium", "high", "full", "x"] {
        println!("ca rotation/{l} {:016x}", of(&rotation_level(l)));
    }
    for l in ["none", "default", "high", "veryHigh", "x"] {
        println!("ca color/{l} {:08x}", color_level(l).to_bits());
    }
    let stripes = |w: usize, h: usize| -> (Vec<f32>, Vec<bool>) {
        let img: Vec<f32> = (0..w * h).map(|i| if (i % w) % 6 < 3 { 0.2 } else { 0.8 }).collect();
        let hole: Vec<bool> = (0..w * h).map(|i| (i % w).abs_diff(w / 2) < 4 && (i / w).abs_diff(h / 2) < 4).collect();
        (img, hole)
    };
    let (w, h) = (48, 32);
    let (im, hole) = stripes(w, h);
    let all = vec![true; w * h];
    println!("ca fill/stripes {:016x}", of(&fill(w, h, 1, &im, &hole, &all, &FillOptions::default())));
    println!("ca fill/stripes0 {:016x}", of(&fill(w, h, 1, &im, &hole, &all, &FillOptions { color_adaptation: 0.0, ..Default::default() })));
    let (w, h) = (40, 20);
    let lr: Vec<f32> = (0..w * h).map(|i| if i % w < 20 { 0.1 } else { 0.9 }).collect();
    let hole = rect_mask(w, h, 16, 6, 24, 14);
    let right: Vec<bool> = (0..w * h).map(|i| i % w >= 24).collect();
    println!("ca fill/right {:016x}", of(&fill(w, h, 1, &lr, &hole, &right, &FillOptions { color_adaptation: 0.0, ..Default::default() })));
    println!("ca fill/nosource {:016x}", of(&fill(w, h, 1, &lr, &hole, &vec![false; w * h], &FillOptions::default())));
    let ramp: Vec<f32> = (0..w * h).map(|i| (i % w) as f32 / w as f32).collect();
    let hole2 = rect_mask(w, h, 14, 5, 26, 15);
    println!("ca fill/ramp {:016x}", of(&fill(w, h, 1, &ramp, &hole2, &vec![true; w * h], &FillOptions { color_adaptation: 0.9, ..Default::default() })));
    let (w, h) = (32, 24);
    let (im, hole) = stripes(w, h);
    let rgb: Vec<f32> = im.iter().flat_map(|v| [*v, *v * 0.5, 1.0 - *v]).collect();
    let src = vec![true; w * h];
    for (key, opts) in [
        ("adapt", FillOptions { color_adaptation: 0.65, rotations: rotation_level("low"), scale: true, mirror: true, seed: 7 }),
        ("full", FillOptions { color_adaptation: 0.35, rotations: rotation_level("full"), scale: false, mirror: false, seed: 2 }),
        ("mirror", FillOptions { color_adaptation: 0.0, rotations: vec![], scale: false, mirror: true, seed: 1 }),
        ("scale", FillOptions { color_adaptation: 0.35, rotations: vec![33.0], scale: true, mirror: false, seed: 4 }),
    ] {
        println!("ca fill/{key} {:016x}", of(&fill(w, h, 3, &rgb, &hole, &src, &opts)));
    }
    println!("ca fill/nohole {:016x}", of(&fill(w, h, 3, &rgb, &vec![false; w * h], &src, &FillOptions::default())));
}

fn seam_vectors() {
    let mut seed = 1u32;
    let mut next = |n: usize| {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        seed as usize % n
    };
    for case in 0..60u32 {
        let (w, h, ch) = (1 + next(24), 1 + next(18), [1, 3, 4][next(3)]);
        let levels = [2, 5, 255, 65_536][next(4)];
        let im = noise(w * h * ch, levels, 11 + case);
        let protect = (next(3) == 0).then(|| noise(w * h, 3, 101 + case));
        let (nw, nh) = (1 + next(2 * w + 2), 1 + next(2 * h + 2));
        let out = seam::carve(w, h, ch, &im, protect.as_deref(), nw, nh);
        println!("seam carve/{case} {w}x{h}x{ch}->{nw}x{nh} {} {:016x}", out.len(), of(&out));
    }
    for case in 0..30u32 {
        let (w, h) = (1 + next(40), 1 + next(30));
        let e = noise(w * h, [2, 3, 1000][next(3)], 7 + case);
        println!("seam find/{case} {:016x}", hus(&seam::find_seam(w, h, &e)));
        let im = noise(w * h * 3, 7, 3 + case);
        println!("seam energy/{case} {:016x}", of(&seam::energy(w, h, 3, &im)));
    }
    let sm = smooth(37, 23, 3, 1);
    for (nw, nh) in [(20, 23), (37, 12), (60, 30), (1, 1), (74, 46)] {
        println!("seam smooth/{nw}x{nh} {:016x}", of(&seam::carve(37, 23, 3, &sm, None, nw, nh)));
        println!("seam bilinear/{nw}x{nh} {:016x}", of(&seam::resize_bilinear(37, 23, 3, &sm, nw, nh)));
    }
    let sk = img(17, 9, 4, 5);
    println!("seam skin {:016x}", of(&seam::skin_mask(17, 9, 4, &sk)));
    println!("seam skin/2ch {:016x}", of(&seam::skin_mask(17, 9, 2, &img(17, 9, 2, 5))));
    let skin_like: Vec<f32> = (0..64).flat_map(|i| [0.5 + (i % 8) as f32 * 0.06, 0.2 + (i / 8) as f32 * 0.05, 0.15 + (i % 5) as f32 * 0.05]).collect();
    println!("seam skin/tones {:016x}", of(&seam::skin_mask(8, 8, 3, &skin_like)));
}

fn retouch_vectors() {
    use retouch::*;
    let ranges = [("s", ToneRange::Shadows), ("m", ToneRange::Midtones), ("h", ToneRange::Highlights)];
    let vs: Vec<f32> = (0..30).map(|i| i as f32 / 24.0 - 0.1).chain([f32::NAN, f32::INFINITY, -0.0]).collect();
    for (rn, r) in ranges {
        for burn in [false, true] {
            let mut h = H0;
            for e in [-0.5f32, 0.0, 0.1, 0.5, 1.0, 2.0, f32::NAN] {
                for &v in &vs {
                    hf(&mut h, &[tone_curve(v, e, r, burn)]);
                }
            }
            println!("retouch curve/{rn}/{burn} {h:016x}");
            for protect in [false, true] {
                let mut h = H0;
                for e in [0.0f32, 0.2, 0.8, 1.0] {
                    for i in 0..60 {
                        let c = [pat(i, 1, 0, 2) * 1.1 - 0.05, pat(i, 2, 1, 2), pat(i, 3, 2, 2) * 0.9];
                        hf(&mut h, &dodge_burn(c, e, r, burn, protect));
                    }
                    hf(&mut h, &dodge_burn([0.0; 3], e, r, burn, protect));
                    hf(&mut h, &dodge_burn([1.2, 0.5, -0.1], e, r, burn, protect));
                }
                println!("retouch dodge/{rn}/{burn}/{protect} {h:016x}");
            }
        }
    }
    for sat in [false, true] {
        for vib in [false, true] {
            let mut h = H0;
            for a in [-1.0f32, 0.0, 0.3, 0.5, 1.0, 3.0] {
                for i in 0..60 {
                    let c = [pat(i, 1, 0, 4), pat(i, 2, 1, 4) * 1.05, pat(i, 3, 2, 4)];
                    hf(&mut h, &sponge(c, a, sat, vib));
                }
            }
            println!("retouch sponge/{sat}/{vib} {h:016x}");
        }
    }
    let mut h = H0;
    for i in 0..60 {
        let c = [pat(i, 1, 0, 6), pat(i, 2, 1, 6), pat(i, 3, 2, 6)];
        hf(&mut h, &[saturation(c), luma(c)]);
    }
    println!("retouch sat {h:016x}");
    let (w, hh) = (16usize, 14usize);
    for ch in [1usize, 3, 4] {
        let data = img(w, hh, ch, 3);
        let alpha = if ch == 4 { Some(3) } else { None };
        for (wi, win) in [(0, 0, 16, 14), (4, 4, 12, 12), (13, 2, 16, 5), (5, 5, 5, 9)].into_iter().enumerate() {
            for (si, sigma) in [0.3f32, 1.0, 1.5, 3.7, f32::NAN].into_iter().enumerate() {
                println!("retouch blur/{ch}/{wi}/{si} {:016x}", of(&local_blur(&data, w, hh, ch, alpha, win, sigma)));
            }
            for (ai, (amount, protect)) in [(1.0f32, false), (4.0, true), (-0.5, true)].into_iter().enumerate() {
                println!("retouch sharpen/{ch}/{wi}/{ai} {:016x}", of(&local_sharpen(&data, w, hh, ch, alpha, win, amount, protect)));
            }
        }
    }
}

fn redeye_vectors() {
    println!("redeye halo {}", redeye::halo_radius());
    let mut h = H0;
    for p in [-5.0f32, 0.0, 1.0, 10.0, 50.0, 99.9, 100.0, 150.0, f32::NAN, f32::INFINITY] {
        hf(&mut h, &[redeye::search_radius(p)]);
    }
    println!("redeye radius {h:016x}");
    for (i, p) in [-500.0f32, -5.0, 0.0, 1.0, 50.0, 100.0, 200.0, 205.0, 210.0, 1e9, f32::NAN].into_iter().enumerate() {
        println!("redeye side/{i} {}", redeye::unclamped_window_side(p).map_or(-1, i64::from));
    }
    let disc_px = |x: usize, y: usize, cx: f32, cy: f32, r: f32| (x as f32 - cx).hypot(y as f32 - cy) <= r;
    let portrait = |w: usize| -> Vec<[f32; 4]> {
        (0..w * w)
            .map(|i| {
                let (x, y) = (i % w, i / w);
                if disc_px(x, y, 16.0, 16.0, 1.2) {
                    [1.0, 1.0, 1.0, 1.0]
                } else if disc_px(x, y, 16.0, 16.0, 10.0) {
                    [0.95, 0.10, 0.10, 1.0]
                } else {
                    [0.38, 0.28, 0.16, 1.0]
                }
            })
            .collect()
    };
    for (pi, (pupil, darken)) in [(50.0f32, 100.0f32), (50.0, 50.0), (1.0, 50.0), (10.0, 0.0), (f32::NAN, f32::NAN), (100.0, 250.0)].into_iter().enumerate() {
        for (ci, (click, origin)) in [((16, 16), (0, 0)), ((20, 14), (0, 0)), ((116, 116), (100, 100)), ((60, 60), (0, 0))].into_iter().enumerate() {
            let mut px = portrait(32);
            let n = redeye::apply(&mut px, 32, 32, click, origin, pupil, darken);
            println!("redeye portrait/{pi}/{ci} {n} {:016x}", hpx(&px));
        }
    }
    let w = 128usize;
    let mut half: Vec<[f32; 4]> = (0..w * w).map(|i| if i % w < 64 { [0.95, 0.10, 0.10, 1.0] } else { [0.38, 0.28, 0.16, 1.0] }).collect();
    let n = redeye::apply(&mut half, w, w, (60, 64), (0, 0), 50.0, 50.0);
    println!("redeye half {n} {:016x}", hpx(&half));
    let mut noisy: Vec<[f32; 4]> = (0..40 * 30).map(|i| [pat(i, 0, 0, 1), pat(i, 0, 1, 1) * 0.5, pat(i, 0, 2, 1) * 0.4, 1.0]).collect();
    let n = redeye::apply(&mut noisy, 40, 30, (20, 15), (0, 0), 30.0, 70.0);
    println!("redeye noisy {n} {:016x}", hpx(&noisy));
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

/// A region over `bbox` with coverage from the pattern (some zero, some full).
fn region(bbox: Rect, seed: i32) -> Region {
    let (w, h) = (bbox.width() as usize, bbox.height() as usize);
    let mask = (0..w * h)
        .map(|i| {
            let v = pat(i as i32, (i / 5) as i32, 0, seed);
            if v < 0.2 { 0 } else if v > 0.7 { 255 } else { (v * 255.0) as u8 }
        })
        .collect();
    Region { bbox, mask }
}

fn erase_vectors() {
    let r = Rect::new(-20, -10, 300, 270);
    let mut sel = Surface::new(PixelFormat::GRAY8);
    sel.write_region(Rect::new(0, 0, 200, 200), &(0..200 * 200).map(|i| pat(i, i / 200, 0, 9)).collect::<Vec<f32>>());
    for (name, fmt) in formats() {
        for (ai, (amount, use_sel, lock)) in [(1.0f32, false, false), (0.5, false, false), (0.7, true, false), (0.6, false, true), (f32::NAN, false, false)].into_iter().enumerate() {
            let mut s = surf(fmt, r, 2, false);
            let lock_c = [0.9f32, 0.1, 0.4, 0.2];
            let d = erase::magic_erase(&mut s, &region(Rect::new(-5, 3, 270, 260), 4), amount, use_sel.then_some(&sel), lock.then_some(&lock_c[..]));
            let mut h = H0;
            hrect(&mut h, d);
            println!("erase {name}/{ai} {h:016x} {:016x}", hs(&s));
        }
    }
    // Mismatched mask and empty box.
    let mut s = surf(PixelFormat::RGBA8, Rect::new(0, 0, 8, 8), 1, true);
    let d = erase::magic_erase(&mut s, &Region { bbox: Rect::new(0, 0, 4, 4), mask: vec![255; 3] }, 1.0, None, None);
    println!("erase mismatch {} {:016x}", d.is_empty(), hs(&s));
}

fn paint_vectors() {
    use paint::*;
    let shapes = [("linear", GradientShape::Linear), ("radial", GradientShape::Radial), ("angle", GradientShape::Angle), ("reflected", GradientShape::Reflected), ("diamond", GradientShape::Diamond)];
    for (name, shape) in shapes {
        let mut h = H0;
        for (from, to) in [((0.0f32, 0.0f32), (10.0f32, 0.0f32)), ((3.5, -2.0), (-7.25, 11.0)), ((1.0, 1.0), (1.0, 1.0))] {
            for i in 0..80 {
                let (x, y) = ((i % 9) as f32 * 1.7 - 4.0, (i / 9) as f32 * 1.3 - 3.0);
                hf(&mut h, &[tool_gradient_t(shape, from, to, x, y)]);
            }
        }
        println!("paint t/{name} {h:016x}");
    }
    let stops3 = [(0.0f32, [0.0f32, 0.0, 0.0, 1.0]), (0.3, [1.0, 0.0, 0.0, 0.5]), (0.3, [0.0, 1.0, 0.0, 1.0]), (1.0, [1.0, 1.0, 1.0, 0.0])];
    let mut h = H0;
    for stops in [&stops3[..0], &stops3[..1], &stops3[..2], &stops3[..]] {
        for i in 0..25 {
            hf(&mut h, &sample_stops(stops, i as f32 / 20.0 - 0.1));
        }
    }
    println!("paint stops {h:016x}");
    let mut sel = Surface::new(PixelFormat::GRAY8);
    sel.write_region(Rect::new(0, 0, 150, 100), &(0..150 * 100).map(|i| pat(i, i / 150, 0, 3)).collect::<Vec<f32>>());
    let area = Rect::new(-10, -5, 290, 120);
    for (name, fmt) in formats() {
        for (key, shape, rev, dither, mode, use_sel, lock) in [
            ("a", GradientShape::Linear, false, false, BlendMode::Normal, false, false),
            ("b", GradientShape::Radial, true, true, BlendMode::Multiply, true, false),
            ("c", GradientShape::Angle, false, true, BlendMode::Screen, false, true),
            ("d", GradientShape::Diamond, true, false, BlendMode::Overlay, true, true),
        ] {
            let mut s = surf(fmt, Rect::new(0, 0, 260, 90), 5, false);
            paint_gradient(&mut s, area, (3.0, 4.0), (250.5, 80.0), shape, &stops3, rev, 0.8, mode, dither, use_sel.then_some(&sel), lock);
            println!("paint gradient/{name}/{key} {:016x}", hs(&s));
        }
        // Bucket: a two-tone surface filled from a seed.
        let mut s = Surface::new(fmt);
        let c1 = photocraft_raster::from_rgba(&fmt, [0.9, 0.9, 0.9, 1.0]);
        let c2 = photocraft_raster::from_rgba(&fmt, [0.1, 0.2, 0.3, 1.0]);
        s.fill_rect(Rect::new(0, 0, 40, 30), &c1);
        s.fill_rect(Rect::new(18, 0, 22, 30), &c2);
        let a = Rect::new(0, 0, 40, 30);
        for (bi, (contig, aa, tol)) in [(true, false, 10.0f32), (false, true, 30.0), (true, true, 0.0)].into_iter().enumerate() {
            let mut t = s.clone();
            let ok = bucket_fill(&mut t, a, (3, 4), tol, contig, aa, [1.0, 0.0, 0.0, 1.0], 0.75, Some(&sel), false);
            println!("paint bucket/{name}/{bi} {ok} {:016x}", hs(&t));
        }
        let mut t = s.clone();
        let ok = bucket_fill_src(&mut t, a, (30, 10), 5.0, true, false, 1.0, None, true, |x, y| [pat(x, y, 0, 1), pat(x, y, 1, 1), pat(x, y, 2, 1), 0.5]);
        println!("paint bucketsrc/{name} {ok} {:016x}", hs(&t));
        let mut t = s.clone();
        println!("paint bucket/{name}/outside {}", bucket_fill(&mut t, a, (50, 1), 10.0, true, false, [1.0, 0.0, 0.0, 1.0], 1.0, None, false));
        let mut t = s.clone();
        bucket_fill_region_src(&mut t, &region(Rect::new(-3, 4, 25, 20), 6), 0.5, Some(&sel), false, |_, _| [0.2, 0.4, 0.8, 1.0]);
        println!("paint region/{name} {:016x}", hs(&t));
    }
}

pub fn run() {
    stack_vectors();
    poisson_vectors();
    inpaint_vectors();
    content_aware_vectors();
    seam_vectors();
    retouch_vectors();
    redeye_vectors();
    erase_vectors();
    paint_vectors();
}
