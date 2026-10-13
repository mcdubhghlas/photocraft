//! L10 algo analysis reference vectors (`rustref algo-analysis-vectors`): EXIF (with truncation
//! and byte-mutation sweeps), vectorscope, features (Harris, BRIEF, matching, fits, RANSAC,
//! registration), panorama (motions, placements, detection, pair matching, alignment for every
//! layout, photometric, seams, multi-band), quantize (palettes, dithers, bitmap methods) and scan
//! crop. One `key value` line per case; values are FNV-1a hashes of little-endian bit patterns or
//! plain numbers. Replayed by tests/algo/test_analysis_parity.c.
//!
//! photo_util.rs is crate-private: the functions checked here are verbatim copies (marked).
use photocraft_algo::exif::{self, CameraInfo};
use photocraft_algo::features::{self, Corner, Model};
use photocraft_algo::panorama::{self, AlignOptions, Layout, Motion, Placement, Projection, RoiImage};
use photocraft_algo::quantize::{self, BitmapMethod, Dither, Forced, HalftoneShape, PaletteKind};
use photocraft_algo::scancrop::{self, FoundPhoto};
use photocraft_algo::transform::Homography;
use photocraft_algo::vectorscope::{self, Vectorscope};

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
fn hu(h: &mut u64, v: u64) {
    fnv(h, &v.to_le_bytes());
}
fn hh(h: &mut u64, m: &Homography) {
    hd(h, &m.0);
}

// ---------------------------------------------------------------------------------------------
// Verbatim copies (crates/algo/src/photo_util.rs).

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    fn unit(&mut self) -> f64 {
        (self.next() >> 11) as f64 / (1u64 << 53) as f64
    }
}

fn solve_dense(n: usize, mut a: Vec<f64>, mut b: Vec<f64>) -> Option<Vec<f64>> {
    for col in 0..n {
        let piv = (col..n).max_by(|&i, &j| a[i * n + col].abs().total_cmp(&a[j * n + col].abs()))?;
        if a[piv * n + col].abs() < 1e-14 {
            return None;
        }
        if piv != col {
            for k in 0..n {
                a.swap(col * n + k, piv * n + k);
            }
            b.swap(col, piv);
        }
        let d = a[col * n + col];
        for r in col + 1..n {
            let f = a[r * n + col] / d;
            if f == 0.0 {
                continue;
            }
            for k in col..n {
                a[r * n + k] -= f * a[col * n + k];
            }
            b[r] -= f * b[col];
        }
    }
    let mut x = vec![0.0; n];
    for r in (0..n).rev() {
        let s: f64 = (r + 1..n).map(|k| a[r * n + k] * x[k]).sum();
        x[r] = (b[r] - s) / a[r * n + r];
    }
    x.iter().all(|v| v.is_finite()).then_some(x)
}

struct SparseLs {
    n: usize,
    rows: Vec<(Vec<(usize, f64)>, f64)>,
}
impl SparseLs {
    fn add(&mut self, row: Vec<(usize, f64)>, rhs: f64, w: f64) {
        self.rows.push((row.into_iter().map(|(i, c)| (i, c * w)).collect(), rhs * w));
    }
    fn solve(&self, x0: &[f64], iters: usize) -> Vec<f64> {
        let n = self.n;
        let mut x = x0.to_vec();
        let ax = |x: &[f64]| -> Vec<f64> { self.rows.iter().map(|(r, _)| r.iter().map(|&(i, c)| c * x[i]).sum()).collect() };
        let atv = |v: &[f64]| -> Vec<f64> {
            let mut out = vec![0.0; n];
            for ((r, _), vi) in self.rows.iter().zip(v) {
                for &(i, c) in r {
                    out[i] += c * vi;
                }
            }
            out
        };
        let mut r: Vec<f64> = self.rows.iter().zip(ax(&x)).map(|((_, b), a)| b - a).collect();
        let mut s = atv(&r);
        let mut p = s.clone();
        let mut gamma: f64 = s.iter().map(|v| v * v).sum();
        let g0 = gamma.max(1e-30);
        for _ in 0..iters {
            if gamma <= g0 * 1e-20 {
                break;
            }
            let q = ax(&p);
            let qq: f64 = q.iter().map(|v| v * v).sum();
            if qq <= 1e-300 {
                break;
            }
            let alpha = gamma / qq;
            for i in 0..n {
                x[i] += alpha * p[i];
            }
            for (ri, qi) in r.iter_mut().zip(&q) {
                *ri -= alpha * qi;
            }
            s = atv(&r);
            let g1: f64 = s.iter().map(|v| v * v).sum();
            let beta = g1 / gamma;
            gamma = g1;
            for i in 0..n {
                p[i] = s[i] + beta * p[i];
            }
        }
        x
    }
}

fn hash01(x: i64, y: i64, seed: u64) -> f32 {
    let mut z = (x as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ (y as u64).wrapping_mul(0xC2B2_AE3D_27D4_EB4F) ^ seed.wrapping_mul(0x1656_67B1_9E37_79F9);
    z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^= z >> 31;
    (z >> 40) as f32 / (1u64 << 24) as f32
}
fn srgb_to_linear(v: f32) -> f32 {
    if v <= 0.04045 { v / 12.92 } else { ((v + 0.055) / 1.055).powf(2.4) }
}
fn linear_to_srgb(v: f32) -> f32 {
    if v <= 0.003_130_8 { v * 12.92 } else { 1.055 * v.powf(1.0 / 2.4) - 0.055 }
}
fn bilinear(buf: &[f32], w: usize, h: usize, ch: usize, c: usize, x: f32, y: f32) -> f32 {
    let x = x.clamp(0.0, (w - 1) as f32);
    let y = y.clamp(0.0, (h - 1) as f32);
    let (x0, y0) = (x.floor() as usize, y.floor() as usize);
    let (x1, y1) = ((x0 + 1).min(w - 1), (y0 + 1).min(h - 1));
    let (tx, ty) = (x - x0 as f32, y - y0 as f32);
    let p = |xx: usize, yy: usize| buf[(yy * w + xx) * ch + c];
    let top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
    let bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
    top + (bot - top) * ty
}
fn catmull_rom(t: f32) -> [f32; 4] {
    let t2 = t * t;
    let t3 = t2 * t;
    [-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2]
}

// ---------------------------------------------------------------------------------------------
// Test images (the crate's test helpers; C: tests/algo/analysis_gen.h).

fn texture(w: usize, h: usize) -> Vec<f32> {
    let mut rng = Rng(42);
    let mut img = vec![0.3f32; w * h];
    for _ in 0..140 {
        let (cx, cy) = (rng.unit() * w as f64, rng.unit() * h as f64);
        let r = 3.0 + rng.unit() * 9.0;
        let v = rng.unit() as f32;
        let sq = rng.unit() > 0.5;
        for y in 0..h {
            for x in 0..w {
                let (dx, dy) = (x as f64 - cx, y as f64 - cy);
                let inside = if sq { dx.abs() < r && dy.abs() < r * 0.6 } else { dx.hypot(dy) < r };
                if inside {
                    img[y * w + x] = v;
                }
            }
        }
    }
    img
}
fn warp(w: usize, hgt: usize, img: &[f32], inv: &Homography) -> Vec<f32> {
    let mut out = vec![0.3f32; w * hgt];
    for y in 0..hgt {
        for x in 0..w {
            let (sx, sy) = inv.apply(x as f64, y as f64);
            if sx < 0.0 || sy < 0.0 || sx >= (w - 1) as f64 || sy >= (hgt - 1) as f64 {
                continue;
            }
            let (x0, y0) = (sx.floor() as usize, sy.floor() as usize);
            let (tx, ty) = ((sx - x0 as f64) as f32, (sy - y0 as f64) as f32);
            let p = |xx: usize, yy: usize| img[yy * w + xx];
            let top = p(x0, y0) + (p(x0 + 1, y0) - p(x0, y0)) * tx;
            let bot = p(x0, y0 + 1) + (p(x0 + 1, y0 + 1) - p(x0, y0 + 1)) * tx;
            out[y * w + x] = top + (bot - top) * ty;
        }
    }
    out
}
fn scene(w: usize, h: usize, seed: u64) -> Vec<f32> {
    let mut rng = Rng(seed);
    let mut img: Vec<f32> = (0..w * h).map(|i| 0.25 + 0.2 * ((i % w) as f32 / w as f32)).collect();
    for _ in 0..(w * h / 900).max(40) {
        let cx = (rng.next() % w as u64) as f64;
        let cy = (rng.next() % h as u64) as f64;
        let r = 3.0 + (rng.next() % 9) as f64;
        let v = (rng.next() % 1000) as f32 / 1000.0;
        let sq = rng.next().is_multiple_of(2);
        let (x0, x1) = ((cx - r).max(0.0) as usize, ((cx + r) as usize + 1).min(w));
        let (y0, y1) = ((cy - r).max(0.0) as usize, ((cy + r) as usize + 1).min(h));
        for y in y0..y1 {
            for x in x0..x1 {
                let (dx, dy) = (x as f64 - cx, y as f64 - cy);
                if if sq { dx.abs() < r && dy.abs() < r * 0.6 } else { dx.hypot(dy) < r } {
                    img[y * w + x] = v;
                }
            }
        }
    }
    img
}
fn view(scene: &[f32], sw: usize, sh: usize, tw: usize, th: usize, map: impl Fn(f64, f64) -> (f64, f64)) -> Vec<f32> {
    (0..tw * th)
        .map(|i| {
            let (x, y) = map((i % tw) as f64, (i / tw) as f64);
            if x < 0.0 || y < 0.0 || x > (sw - 1) as f64 || y > (sh - 1) as f64 { 0.0 } else { bilinear(scene, sw, sh, 1, 0, x as f32, y as f32) }
        })
        .collect()
}
fn pan_map(f: f64, tw: usize, th: usize, sw: usize, sh: usize, deg: f64) -> impl Fn(f64, f64) -> (f64, f64) {
    move |x, y| {
        let (dx, dy) = (x - tw as f64 / 2.0, y - th as f64 / 2.0);
        let th_ = dx.atan2(f) + deg.to_radians();
        let hh = dy / dx.hypot(f);
        (sw as f64 / 2.0 + f * th_, sh as f64 / 2.0 + f * hh)
    }
}
fn scan(w: usize, h: usize, photos: &[FoundPhoto]) -> Vec<[f32; 4]> {
    let mut px = vec![[0.97f32, 0.97, 0.96, 1.0]; w * h];
    for (k, p) in photos.iter().enumerate() {
        let (s, c) = p.angle.to_radians().sin_cos();
        for y in 0..h {
            for x in 0..w {
                let (dx, dy) = (x as f64 + 0.5 - p.center[0], y as f64 + 0.5 - p.center[1]);
                let (u, v) = (c * dx + s * dy, -s * dx + c * dy);
                if u.abs() < p.width / 2.0 && v.abs() < p.height / 2.0 {
                    let t = ((u * 0.05).sin() * (v * 0.07).cos()) as f32;
                    px[y * w + x] = [0.3 + 0.2 * t, 0.2 + 0.1 * k as f32, 0.5 - 0.2 * t, 1.0];
                }
            }
        }
    }
    px
}
/// Deterministic RGBA pixels (C: the same integer formula).
fn rgba(n: usize, seed: u64) -> Vec<[f32; 4]> {
    let mut rng = Rng(seed);
    (0..n)
        .map(|i| {
            let a = if i % 11 == 0 { 0.2 } else if i % 13 == 0 { 0.0 } else { 1.0 };
            [(rng.next() % 1001) as f32 / 1000.0, (rng.next() % 1001) as f32 / 1000.0, (rng.next() % 1001) as f32 / 1000.0, a]
        })
        .collect()
}

// ---------------------------------------------------------------------------------------------

fn photo_util_vectors() {
    let mut h = H0;
    for n in 1..7usize {
        let mut rng = Rng(n as u64 * 31);
        for _ in 0..20 {
            let a: Vec<f64> = (0..n * n).map(|_| rng.unit() * 2.0 - 1.0).collect();
            let b: Vec<f64> = (0..n).map(|_| rng.unit() * 10.0 - 5.0).collect();
            match solve_dense(n, a, b) {
                Some(x) => hd(&mut h, &x),
                None => hu(&mut h, 7),
            }
        }
    }
    hd(&mut h, &solve_dense(3, vec![1.0, 2.0, 3.0, 2.0, 4.0, 6.0, 1.0, 0.0, 1.0], vec![1.0, 2.0, 3.0]).unwrap_or_default());
    println!("pu_solve_dense {h:016x}");
    let mut h = H0;
    for iters in [0usize, 1, 3, 10, 50] {
        let mut sp = SparseLs { n: 4, rows: Vec::new() };
        let mut rng = Rng(5);
        for k in 0..30 {
            let row = vec![(k % 4, rng.unit()), ((k + 1) % 4, rng.unit() - 0.5), ((k * 3) % 4, 0.25)];
            sp.add(row, rng.unit() * 3.0, 0.5 + (k % 3) as f64);
        }
        hd(&mut h, &sp.solve(&[0.0, 1.0, 0.0, -1.0], iters));
    }
    println!("pu_sparse {h:016x}");
    let mut h = H0;
    for i in -20i64..20 {
        hf(&mut h, &[hash01(i * 37, -i * 11, i as u64 * 1_000_003), hash01(i, i, u64::MAX - i as u64)]);
    }
    for k in 0..=200 {
        let v = k as f32 / 100.0 - 0.5;
        hf(&mut h, &[srgb_to_linear(v), linear_to_srgb(v)]);
        hf(&mut h, &catmull_rom(v));
    }
    let buf: Vec<f32> = (0..5 * 4 * 2).map(|i| (i * 7 % 11) as f32 / 10.0).collect();
    for k in 0..60 {
        let (x, y) = (k as f32 * 0.17 - 1.0, k as f32 * 0.09 - 0.5);
        hf(&mut h, &[bilinear(&buf, 5, 4, 2, k % 2, x, y)]);
    }
    println!("pu_misc {h:016x}");
}

fn info_hash(h: &mut u64, i: &CameraInfo) {
    // R5: date_taken (new upstream field) hashed with the other strings.
    for s in [&i.make, &i.model, &i.lens, &i.date_taken] {
        match s {
            Some(s) => {
                hu(h, s.len() as u64);
                fnv(h, s.as_bytes());
            }
            None => hu(h, u64::MAX),
        }
    }
    for v in [i.exposure_time, i.f_number, i.iso, i.focal_length, i.focal_length_35mm] {
        match v {
            Some(v) => hd(h, &[v]),
            None => hu(h, u64::MAX - 1),
        }
    }
}

fn exif_infos() -> Vec<CameraInfo> {
    vec![
        CameraInfo {
            make: Some("Generic".into()),
            model: Some("Camera 1".into()),
            lens: Some("24-70mm".into()),
            exposure_time: Some(0.004),
            f_number: Some(8.0),
            iso: Some(200.0),
            focal_length: Some(35.0),
            focal_length_35mm: Some(52.0),
            date_taken: None,
        },
        CameraInfo { make: Some("Ab".into()), iso: Some(70000.0), exposure_time: Some(1.0 / 3.0), ..Default::default() },
        CameraInfo { model: Some(" x \u{e9}\0y ".into()), f_number: Some(-2.0), focal_length_35mm: Some(0.0), ..Default::default() },
        CameraInfo::default(),
        CameraInfo { lens: Some("L".repeat(40)), focal_length: Some(1e12), iso: Some(f64::NAN), ..Default::default() },
        // R5: DateTimeOriginal.
        CameraInfo { make: Some("Make".into()), date_taken: Some("2024:02:17 12:40:19".into()), exposure_time: Some(0.004), ..Default::default() },
    ]
}

fn exif_vectors() {
    for (k, info) in exif_infos().iter().enumerate() {
        let b = exif::build(info);
        let mut h = H0;
        fnv(&mut h, &b);
        println!("exif_build_{k} {} {h:016x}", b.len());
        let r = exif::read(&b);
        let mut h = H0;
        info_hash(&mut h, &r);
        let f = r.exposure_factor().map_or(u64::MAX, f64::to_bits);
        println!("exif_read_{k} {h:016x} {f:016x}");
        // Truncations.
        let mut h = H0;
        for cut in 0..=b.len() {
            info_hash(&mut h, &exif::read(&b[..cut]));
        }
        println!("exif_cut_{k} {h:016x}");
        // Byte mutations (every position, a few values).
        let mut h = H0;
        for i in 0..b.len() {
            for v in [0u8, 1, 2, 5, 0x7f, 0x80, 0xff] {
                let mut m = b.clone();
                m[i] = v;
                info_hash(&mut h, &exif::read(&m));
            }
        }
        println!("exif_mut_{k} {h:016x}");
        // Big-endian twin: byte-swap the structure by re-encoding (MM header on the same bytes).
        let mut m = b.clone();
        if m.len() >= 4 {
            m[..4].copy_from_slice(b"MM\0*");
        }
        let mut h = H0;
        info_hash(&mut h, &exif::read(&m));
        let mut pre = b"Exif\0\0".to_vec();
        pre.extend_from_slice(&b);
        info_hash(&mut h, &exif::read(&pre));
        println!("exif_misc_{k} {h:016x}");
    }
    // Random garbage behind valid headers.
    let mut h = H0;
    let mut rng = Rng(99);
    for k in 0..400 {
        let n = (rng.next() % 200) as usize;
        let mut b: Vec<u8> = (0..n).map(|_| (rng.next() % 256) as u8).collect();
        if b.len() >= 8 {
            b[..4].copy_from_slice(if k % 2 == 0 { b"II*\0" } else { b"MM\0*" });
            let off = (rng.next() % 24) as u32;
            if k % 2 == 0 { b[4..8].copy_from_slice(&off.to_le_bytes()) } else { b[4..8].copy_from_slice(&off.to_be_bytes()) }
        }
        info_hash(&mut h, &exif::read(&b));
    }
    println!("exif_garbage {h:016x}");
}

fn vectorscope_vectors() {
    let mut px = rgba(5000, 3);
    px.push([f32::NAN, 0.0, 0.0, 1.0]);
    px.push([2.0, -1.0, 0.5, 1.0]);
    px.push([f32::INFINITY, 0.0, 0.0, 1.0]);
    let v = Vectorscope::from_rgba(&px);
    let mut h = H0;
    for c in &v.counts {
        fnv(&mut h, &c.to_le_bytes());
    }
    println!("vectorscope {} {h:016x}", v.samples);
    let mut h = H0;
    let mut rng = Rng(8);
    for _ in 0..2000 {
        let c = [(rng.next() % 2001) as f32 / 1000.0 - 0.5, (rng.next() % 1001) as f32 / 1000.0, (rng.next() % 1001) as f32 / 1000.0];
        let (a, b) = vectorscope::hue_saturation(c);
        hf(&mut h, &[a, b]);
    }
    println!("hue_saturation {h:016x}");
}

fn corners_hash(h: &mut u64, c: &[Corner]) {
    hu(h, c.len() as u64);
    for k in c {
        hf(h, &[k.x, k.y, k.score]);
    }
}

fn features_vectors() {
    let (w, h_) = (96usize, 80usize);
    let img = texture(w, h_);
    let valid: Vec<bool> = (0..w * h_).map(|i| (i % w) < 70 || (i / w) > 40).collect();
    for (max, md) in [(50usize, 6.0f32), (600, 4.0), (0, 3.0), (10, 0.5), (1000, 30.0)] {
        let mut h = H0;
        corners_hash(&mut h, &features::harris(w, h_, &img, max, md, None));
        corners_hash(&mut h, &features::harris(w, h_, &img, max, md, Some(&valid)));
        println!("harris_{max}_{md} {h:016x}");
    }
    let c = features::harris(w, h_, &img, 60, 5.0, None);
    for upright in [false, true] {
        let mut h = H0;
        for d in features::describe(w, h_, &img, &c, upright) {
            for x in d {
                hu(&mut h, x);
            }
        }
        println!("describe_{upright} {h:016x}");
    }
    // Matching between the texture and a shifted copy.
    let shift = Homography([1.0, 0.0, 5.0, 0.0, 1.0, -3.0, 0.0, 0.0, 1.0]);
    let mov = warp(w, h_, &img, &shift.inverse().unwrap_or(Homography::IDENTITY));
    let cm = features::harris(w, h_, &mov, 60, 5.0, None);
    let (da, db) = (features::describe(w, h_, &img, &c, false), features::describe(w, h_, &mov, &cm, false));
    for ratio in [0.5f32, 0.8, 1.0] {
        let m = features::match_descriptors(&db, &da, ratio);
        let mut h = H0;
        for (a, b) in &m {
            hu(&mut h, *a as u64);
            hu(&mut h, *b as u64);
        }
        println!("match_{ratio} {} {h:016x}", m.len());
    }
    // Fits on noisy point sets.
    let mut rng = Rng(77);
    let mut h = H0;
    for n in [0usize, 1, 2, 3, 4, 5, 8, 20] {
        let src: Vec<[f64; 2]> = (0..n).map(|_| [rng.unit() * 100.0, rng.unit() * 80.0]).collect();
        let t = Homography([1.02, 0.04, 3.0, -0.03, 0.97, -6.0, 0.0002, -0.0001, 1.0]);
        let dst: Vec<[f64; 2]> = src
            .iter()
            .map(|p| {
                let (x, y) = t.apply(p[0], p[1]);
                [x + rng.unit() * 0.1, y - rng.unit() * 0.1]
            })
            .collect();
        for model in [Model::Translation, Model::Similarity, Model::Homography] {
            match features::fit(model, &src, &dst) {
                Some(m) => hh(&mut h, &m),
                None => hu(&mut h, 3),
            }
        }
        for motion in [Motion::Translation, Motion::Euclidean, Motion::Similarity, Motion::Homography] {
            match panorama::fit_motion(motion, &src, &dst) {
                Some(m) => hh(&mut h, &m),
                None => hu(&mut h, 4),
            }
        }
    }
    println!("fits {h:016x}");
    // RANSAC with outliers, several seeds / models; minimal sets.
    let mut h = H0;
    let mut src = Vec::new();
    let mut dst = Vec::new();
    let mut rng = Rng(12);
    for i in 0..60 {
        let p = [rng.unit() * 200.0, rng.unit() * 150.0];
        src.push(p);
        dst.push(if i % 5 == 0 { [rng.unit() * 200.0, rng.unit() * 150.0] } else { [p[0] * 0.99 - p[1] * 0.02 + 7.0, p[0] * 0.02 + p[1] * 0.99 - 3.0] });
    }
    for seed in [0u64, 1, 7, 12345] {
        for model in [Model::Translation, Model::Similarity, Model::Homography] {
            for (iters, thr) in [(1usize, 1.0f64), (50, 2.0), (300, 0.5)] {
                match features::ransac(model, &src, &dst, iters, thr, seed) {
                    Some((m, inl)) => {
                        hh(&mut h, &m);
                        hu(&mut h, inl.len() as u64);
                        for i in inl {
                            hu(&mut h, i as u64);
                        }
                    }
                    None => hu(&mut h, 9),
                }
            }
        }
        for motion in [Motion::Translation, Motion::Euclidean, Motion::Similarity, Motion::Homography] {
            for k in [1usize, 2, 4, 60] {
                match panorama::ransac_motion(motion, &src[..k], &dst[..k], 40, 1.5, seed) {
                    Some((m, inl)) => {
                        hh(&mut h, &m);
                        hu(&mut h, inl.len() as u64);
                    }
                    None => hu(&mut h, 10),
                }
            }
        }
        for k in [1usize, 2, 4] {
            for model in [Model::Translation, Model::Similarity, Model::Homography] {
                match features::ransac(model, &src[..k], &dst[..k], 5, 1.5, seed) {
                    Some((m, inl)) => {
                        hh(&mut h, &m);
                        hu(&mut h, inl.len() as u64);
                    }
                    None => hu(&mut h, 11),
                }
            }
        }
    }
    println!("ransac {h:016x}");
    // Registration.
    let (w, h_) = (192usize, 160usize);
    let reference = texture(w, h_);
    let (c4, s4) = (4f64.to_radians().cos() * 1.03, 4f64.to_radians().sin() * 1.03);
    let (cx, cy) = (w as f64 / 2.0, h_ as f64 / 2.0);
    let tfs = [
        Homography([1.0, 0.0, 9.0, 0.0, 1.0, -6.0, 0.0, 0.0, 1.0]),
        Homography([c4, -s4, cx - c4 * cx + s4 * cy, s4, c4, cy - s4 * cx - c4 * cy, 0.0, 0.0, 1.0]),
        Homography([1.03, 0.02, -4.0, -0.01, 0.98, 3.0, 0.0001, 0.00005, 1.0]),
    ];
    let mask: Vec<bool> = (0..w * h_).map(|i| (i % w) > 10).collect();
    for (k, t) in tfs.iter().enumerate() {
        let mov = warp(w, h_, &reference, &t.inverse().unwrap_or(Homography::IDENTITY));
        for model in [Model::Translation, Model::Similarity, Model::Homography] {
            let mut h = H0;
            match features::register(w, h_, &reference, &mov, None, None, model) {
                Some((m, n)) => {
                    hh(&mut h, &m);
                    hu(&mut h, n as u64);
                }
                None => hu(&mut h, 1),
            }
            match features::register(w, h_, &reference, &mov, Some(&mask), Some(&mask), model) {
                Some((m, n)) => {
                    hh(&mut h, &m);
                    hu(&mut h, n as u64);
                }
                None => hu(&mut h, 1),
            }
            println!("register_{k}_{model:?} {h:016x}");
        }
    }
}

fn placement_hash(h: &mut u64, p: &Placement) {
    hd(h, &[p.focal, p.center[0], p.center[1], p.k1]);
    hh(h, &p.h);
    hu(h, p.projection as u64);
}

fn placements_vectors() {
    let mut h = H0;
    for projection in [Projection::Plane, Projection::Cylinder, Projection::Sphere] {
        for k1 in [0.0, 0.05, -0.2] {
            for focal in [50.0, 400.0] {
                let p = Placement { projection, focal, center: [200.0, 150.0], k1, h: Homography([0.98, -0.1, 30.0, 0.1, 0.98, -12.0, 0.0001, 0.0, 1.0]) };
                for i in 0..40 {
                    let (x, y) = (i as f64 * 11.0 - 20.0, i as f64 * 7.5 - 10.0);
                    let q = p.project(x, y);
                    hd(&mut h, &q);
                    match p.unproject(q[0], q[1]) {
                        Some((a, b)) => hd(&mut h, &[a, b]),
                        None => hu(&mut h, 2),
                    }
                    let (u, v) = p.forward(x, y);
                    hd(&mut h, &[u, v]);
                    match p.inverse(u, v) {
                        Some((a, b)) => hd(&mut h, &[a, b]),
                        None => hu(&mut h, 3),
                    }
                    match p.unproject(x * 3.0, y * 3.0) {
                        Some((a, b)) => hd(&mut h, &[a, b]),
                        None => hu(&mut h, 4),
                    }
                }
                placement_hash(&mut h, &p.scaled(2.5));
                placement_hash(&mut h, &p.translated(-3.0, 4.5));
                hd(&mut h, &p.bounds(400.0, 300.0));
            }
        }
    }
    println!("placements {h:016x}");
    let mut h = H0;
    let mut rng = Rng(4);
    for _ in 0..200 {
        let m: Vec<f64> = (0..9).map(|k| if k == 8 { 1.0 } else { rng.unit() * 2.0 - 1.0 }).collect();
        let hm = Homography([m[0], m[1], m[2] * 50.0, m[3], m[4], m[5] * 50.0, m[6] * 0.01, m[7] * 0.01, m[8]]);
        match panorama::focal_from_homography(&hm, [160.0, 120.0], [150.0, 100.0]) {
            Some(f) => hd(&mut h, &[f]),
            None => hu(&mut h, 5),
        }
    }
    println!("focal_from_h {h:016x}");
}

fn feats_hash(h: &mut u64, f: &panorama::ImageFeatures) {
    hu(h, f.w as u64);
    hu(h, f.h as u64);
    hu(h, f.points.len() as u64);
    for p in &f.points {
        hd(h, p);
    }
    for d in &f.desc {
        for x in d {
            hu(h, *x);
        }
    }
}

fn alignment_line(name: &str, a: Option<panorama::Alignment>) {
    let mut h = H0;
    match a {
        Some(a) => {
            hu(&mut h, a.layout as u64);
            hu(&mut h, a.reference as u64);
            for p in &a.placements {
                match p {
                    Some(p) => placement_hash(&mut h, p),
                    None => hu(&mut h, 6),
                }
            }
            hd(&mut h, &[a.focal, a.k1, a.rms]);
            for (x, y, z) in &a.pairs {
                hu(&mut h, *x as u64);
                hu(&mut h, *y as u64);
                hu(&mut h, *z as u64);
            }
            println!("{name} {} {h:016x}", a.layout.name());
        }
        None => println!("{name} none"),
    }
}

fn panorama_vectors() {
    // Three translated crops (as the crate's tests).
    let (sw, sh) = (640, 300);
    let sc = scene(sw, sh, 3);
    let (tw, th) = (260, 220);
    let offs = [(20.0, 30.0), (190.0, 40.0), (360.0, 25.0)];
    let imgs: Vec<Vec<f32>> = offs.iter().map(|&(ox, oy)| view(&sc, sw, sh, tw, th, |x, y| (x + ox, y + oy))).collect();
    let mut h = H0;
    for im in &imgs {
        hf(&mut h, im);
    }
    println!("pano_views {h:016x}");
    let prepared: Vec<panorama::PreparedImage> = imgs.iter().map(|im| (tw, th, im.clone(), None)).collect();
    let feats = panorama::detect_many(&prepared, 700);
    let mut h = H0;
    for f in &feats {
        feats_hash(&mut h, f);
    }
    println!("pano_detect {h:016x}");
    let matches = panorama::match_pairs(&feats);
    let mut h = H0;
    for m in &matches {
        hu(&mut h, m.a as u64);
        hu(&mut h, m.b as u64);
        for p in m.pts_a.iter().chain(&m.pts_b) {
            hd(&mut h, p);
        }
        hh(&mut h, &m.homography);
    }
    println!("pano_match {} {h:016x}", matches.len());
    for layout in [Layout::Auto, Layout::Perspective, Layout::Cylindrical, Layout::Spherical, Layout::Collage, Layout::Reposition] {
        for (reference, focal, geometric) in [(Some(1usize), None, false), (None, Some(300.0), true), (Some(9), Some(0.5), false)] {
            let a = panorama::align(&feats, &matches, &AlignOptions { layout, focal, reference, geometric });
            alignment_line(&format!("pano_align_{}_{:?}_{}", layout.name(), reference, geometric), a);
        }
    }
    // Rotating camera (cylindrical scene) with Auto and Spherical.
    let f = 300.0f64;
    let (sw, sh) = (1400, 500);
    let sc = scene(sw, sh, 9);
    let (tw, th) = (320, 240);
    let feats2: Vec<panorama::ImageFeatures> =
        [-25f64, 0.0, 25.0].iter().map(|&deg| panorama::detect(tw, th, &view(&sc, sw, sh, tw, th, pan_map(f, tw, th, sw, sh, deg)), None, 700)).collect();
    let m2 = panorama::match_pairs(&feats2);
    println!("pano_rot_match {}", m2.len());
    for layout in [Layout::Auto, Layout::Cylindrical, Layout::Spherical, Layout::Perspective] {
        for geometric in [false, true] {
            let a = panorama::align(&feats2, &m2, &AlignOptions { layout, focal: None, reference: None, geometric });
            alignment_line(&format!("pano_rot_{}_{geometric}", layout.name()), a);
        }
    }
    // Wide pan: perspective is too stretched, so Auto falls back to the cylinder.
    let fw = 250.0f64;
    let feats3: Vec<panorama::ImageFeatures> =
        [-70f64, -35.0, 0.0, 35.0, 70.0].iter().map(|&deg| panorama::detect(tw, th, &view(&sc, sw, sh, tw, th, pan_map(fw, tw, th, sw, sh, deg)), None, 700)).collect();
    let m3 = panorama::match_pairs(&feats3);
    println!("pano_wide_match {}", m3.len());
    for layout in [Layout::Auto, Layout::Perspective, Layout::Spherical] {
        for geometric in [false, true] {
            let a = panorama::align(&feats3, &m3, &AlignOptions { layout, focal: None, reference: None, geometric });
            alignment_line(&format!("pano_wide_{}_{geometric}", layout.name()), a);
        }
    }
    // Validity masks in detection.
    let valid: Vec<bool> = (0..tw * th).map(|i| (i % tw) > 40).collect();
    let mut h = H0;
    feats_hash(&mut h, &panorama::detect(tw, th, &view(&sc, sw, sh, tw, th, |x, y| (x + 30.0, y + 20.0)), Some(&valid), 300));
    println!("pano_detect_valid {h:016x}");
}

fn roi(x0: i32, y0: i32, w: usize, h: usize, ch: usize, seed: u64, gain: f32) -> RoiImage {
    let px: Vec<f32> = (0..w * h * ch)
        .map(|i| {
            let p = i / ch;
            let (x, y) = ((p % w) as i32 + x0, (p / w) as i32 + y0);
            (0.3 + 0.2 * ((x as f32 * 0.05).sin() * (y as f32 * 0.07).cos()).abs() + 0.05 * (i % ch) as f32) * gain
        })
        .collect();
    let mut rng = Rng(seed);
    let alpha: Vec<f32> = (0..w * h).map(|i| if i % 97 == 0 { 0.5 } else if rng.next() % 50 == 0 { 0.0 } else { 1.0 }).collect();
    RoiImage { x0, y0, w, h, ch, px, alpha }
}

fn blend_vectors() {
    // Photometric.
    for ch in [1usize, 3] {
        let imgs = vec![roi(0, 0, 200, 100, ch, 1, 1.0), roi(100, 10, 200, 100, ch, 2, 1.3), roi(50, 60, 150, 90, ch, 3, 0.8)];
        let pls: Vec<Placement> = imgs
            .iter()
            .map(|im| Placement {
                projection: Projection::Plane,
                focal: 200.0,
                center: [im.w as f64 / 2.0, im.h as f64 / 2.0],
                k1: 0.0,
                h: Homography([1.0, 0.0, im.x0 as f64 + im.w as f64 / 2.0, 0.0, 1.0, im.y0 as f64 + im.h as f64 / 2.0, 0.0, 0.0, 1.0]),
            })
            .collect();
        let sizes: Vec<(usize, usize)> = imgs.iter().map(|im| (im.w, im.h)).collect();
        for (gain, vig, geo) in [(true, false, true), (true, true, true), (false, true, true), (true, true, false), (false, false, true)] {
            let p = panorama::photometric(&imgs, if geo { Some((pls.as_slice(), sizes.as_slice())) } else { None }, gain, vig);
            let mut h = H0;
            for g in &p.gains {
                hf(&mut h, g);
            }
            hd(&mut h, &p.vignette);
            hf(&mut h, &[p.factor(1, 0, 0.3), p.factor(2, ch - 1, 0.9)]);
            println!("photometric_{ch}_{gain}_{vig}_{geo} {h:016x}");
        }
    }
    // Seams over three overlapping images.
    let (w, h_) = (150usize, 60usize);
    let luma: Vec<Vec<f32>> = (0..3).map(|k| (0..w * h_).map(|i| 0.4 + 0.1 * k as f32 + if (i % w) % 17 == k * 3 { 0.4 } else { 0.0 } + ((i / w) as f32 * 0.01)).collect()).collect();
    let cover: Vec<Vec<bool>> = vec![
        (0..w * h_).map(|i| i % w < 70).collect(),
        (0..w * h_).map(|i| i % w >= 40 && i % w < 120).collect(),
        (0..w * h_).map(|i| i % w >= 90 || i / w > 45).collect(),
    ];
    for order in [vec![0usize, 1, 2], vec![2, 0, 1], vec![1, 1, 0]] {
        let l = panorama::seam_labels(w, h_, &luma, &cover, &order);
        let mut h = H0;
        for v in &l {
            fnv(&mut h, &v.to_le_bytes());
        }
        println!("seams_{order:?} {h:016x}");
    }
    let vcover: Vec<Vec<bool>> = vec![(0..w * h_).map(|i| i / w < 35).collect(), (0..w * h_).map(|i| i / w >= 20).collect()];
    let l = panorama::seam_labels(w, h_, &luma[..2], &vcover, &[0, 1]);
    let mut h = H0;
    for v in &l {
        fnv(&mut h, &v.to_le_bytes());
    }
    println!("seams_vertical {h:016x}");
    // Multi-band.
    for ch in [1usize, 3] {
        let imgs = vec![roi(0, 0, 80, 40, ch, 4, 1.0), roi(40, 5, 80, 40, ch, 5, 1.2), roi(-10, 20, 50, 30, ch, 6, 0.9)];
        let wts: Vec<Vec<f32>> = imgs.iter().enumerate().map(|(k, im)| (0..im.w * im.h).map(|i| ((i * (k + 3)) % 7) as f32 / 6.0).collect()).collect();
        for levels in [0usize, 1, 2, 4, 7] {
            let (out, cov) = panorama::multiband(120, 50, &imgs, &wts, levels);
            let mut h = H0;
            hf(&mut h, &out);
            hf(&mut h, &cov);
            println!("multiband_{ch}_{levels} {h:016x}");
        }
    }
    println!("blend_levels {} {} {} {}", panorama::blend_levels(10, 10), panorama::blend_levels(100, 40), panorama::blend_levels(5000, 3000), panorama::blend_levels(33, 2000));
}

fn pal_hash(h: &mut u64, p: &[[u8; 3]]) {
    hu(h, p.len() as u64);
    for c in p {
        fnv(h, c);
    }
}

fn quantize_vectors() {
    let mut h = H0;
    pal_hash(&mut h, &quantize::web_palette());
    pal_hash(&mut h, &quantize::mac_palette());
    pal_hash(&mut h, &quantize::windows_palette());
    for n in [0usize, 1, 7, 8, 26, 27, 64, 100, 216, 256, 1000] {
        pal_hash(&mut h, &quantize::uniform_palette(n));
    }
    println!("fixed_palettes {h:016x}");
    let grad: Vec<[f32; 4]> = (0..40 * 40).map(|i| [(i % 40) as f32 / 39.0, (i / 40) as f32 / 39.0, 0.3, 1.0]).collect();
    let noisy = rgba(3000, 21);
    let few: Vec<[f32; 4]> = (0..500).map(|i| [[0.1, 0.2, 0.3], [0.9, 0.1, 0.1], [0.5, 0.5, 0.5]][i % 3]).map(|c| [c[0], c[1], c[2], 1.0]).collect();
    let kinds = [
        PaletteKind::Exact,
        PaletteKind::SystemMac,
        PaletteKind::SystemWindows,
        PaletteKind::Web,
        PaletteKind::Uniform,
        PaletteKind::Perceptual,
        PaletteKind::Selective,
        PaletteKind::Adaptive,
    ];
    for (name, px) in [("grad", &grad), ("noisy", &noisy), ("few", &few)] {
        for kind in kinds {
            let mut h = H0;
            for colors in [0usize, 2, 3, 8, 16, 64, 255, 256, 999] {
                for forced in [Forced::None, Forced::BlackWhite, Forced::Primaries, Forced::Web] {
                    match quantize::build_palette(px, kind, colors, forced) {
                        Ok(p) => pal_hash(&mut h, &p),
                        Err(e) => fnv(&mut h, e.as_bytes()),
                    }
                }
            }
            println!("palette_{name}_{kind:?} {h:016x}");
        }
    }
    let pal = quantize::build_palette(&noisy, PaletteKind::Adaptive, 16, Forced::None).unwrap_or_default();
    for dither in [Dither::None, Dither::Diffusion, Dither::Pattern, Dither::Noise] {
        for amount in [0.0f32, 0.5, 1.0, 7.0] {
            for (w, transparent) in [(60usize, None), (60, Some(3usize)), (0, Some(300)), (7, None), (1, Some(15))] {
                let mut px = noisy.clone();
                let idx = quantize::quantize(&mut px, w, &pal, dither, amount, transparent);
                let mut h = H0;
                fnv(&mut h, &idx);
                for p in &px {
                    hf(&mut h, p);
                }
                println!("quantize_{dither:?}_{amount}_{w}_{transparent:?} {h:016x}");
            }
        }
    }
    let gray: Vec<f32> = (0..50 * 37).map(|i| ((i % 50) as f32 / 49.0) * 0.8 + ((i / 50) % 5) as f32 * 0.05).collect();
    let mut methods = vec![BitmapMethod::Threshold, BitmapMethod::Pattern, BitmapMethod::Diffusion];
    for shape in ["round", "ellipse", "line", "square", "diamond", "cross", "bogus"] {
        for (cell, angle) in [(6.0f32, 45.0f32), (1.0, -30.0), (13.5, 0.0), (4.0, 400.0)] {
            methods.push(BitmapMethod::Halftone { cell, angle, shape: HalftoneShape::from_id(shape).unwrap_or(HalftoneShape::Round) });
        }
    }
    for (k, m) in methods.iter().enumerate() {
        for w in [50usize, 37, 1] {
            let mut g = gray.clone();
            quantize::to_bitmap(&mut g, w, *m);
            let mut h = H0;
            hf(&mut h, &g);
            println!("bitmap_{k}_{w} {h:016x}");
        }
    }
    let mut h = H0;
    for y in 0..9 {
        for x in 0..9 {
            hf(&mut h, &[quantize::bayer8(x, y)]);
        }
    }
    println!("bayer8 {h:016x}");
}

fn photo_line(name: &str, f: &[FoundPhoto]) {
    let mut h = H0;
    for p in f {
        hd(&mut h, &[p.center[0], p.center[1], p.width, p.height, p.angle, p.area]);
        for c in p.corners() {
            hd(&mut h, &c);
        }
    }
    println!("{name} {} {h:016x}", f.len());
}

fn scancrop_vectors() {
    let truth = [
        FoundPhoto { center: [150.0, 120.0], width: 180.0, height: 120.0, angle: 7.0, area: 0.0 },
        FoundPhoto { center: [450.0, 130.0], width: 160.0, height: 130.0, angle: -12.0, area: 0.0 },
        FoundPhoto { center: [300.0, 330.0], width: 220.0, height: 110.0, angle: 3.0, area: 0.0 },
    ];
    let (w, h) = (600, 440);
    photo_line("scancrop_basic", &scancrop::find_photos(w, h, &scan(w, h, &truth)));
    let big = [
        FoundPhoto { center: [500.0, 400.0], width: 700.0, height: 500.0, angle: 30.0, area: 0.0 },
        FoundPhoto { center: [1300.0, 600.0], width: 500.0, height: 800.0, angle: -44.0, area: 0.0 },
        FoundPhoto { center: [900.0, 1000.0], width: 300.0, height: 200.0, angle: 89.0, area: 0.0 },
    ];
    let (w, h) = (1700, 1200);
    photo_line("scancrop_big", &scancrop::find_photos(w, h, &scan(w, h, &big)));
    let (w, h) = (300, 200);
    photo_line("scancrop_one", &scancrop::find_photos(w, h, &scan(w, h, &truth[..1])));
    photo_line("scancrop_noise", &scancrop::find_photos(120, 90, &rgba(120 * 90, 5)));
    photo_line("scancrop_blank", &scancrop::find_photos(w, h, &vec![[1.0, 1.0, 1.0, 1.0]; w * h]));
}

pub fn run() {
    photo_util_vectors();
    exif_vectors();
    vectorscope_vectors();
    features_vectors();
    placements_vectors();
    panorama_vectors();
    blend_vectors();
    quantize_vectors();
    scancrop_vectors();
    r5_quantize_vectors();
}

/// R5 (upstream resync): palettes above 64 entries go through the exact k-d tree nearest search
/// (quantize/nearest.rs); HalftoneShape::from_id is an Option now.
fn r5_quantize_vectors() {
    let noisy = rgba(3000, 23);
    let mut grad: Vec<[f32; 4]> = (0..93 * 61).map(|i| [(i % 93) as f32 / 92.0, (i / 93) as f32 / 60.0, 0.3, 1.0]).collect();
    for (i, p) in grad.iter_mut().enumerate() {
        p[2] = (i % 71) as f32 / 70.0;
        p[3] = [0.0, 0.2, 0.7, 1.0][i % 4];
    }
    let pals = [
        ("sel256", quantize::build_palette(&grad, PaletteKind::Selective, 256, Forced::BlackWhite).unwrap_or_default()),
        ("ada128", quantize::build_palette(&noisy, PaletteKind::Adaptive, 128, Forced::None).unwrap_or_default()),
        ("per65", quantize::build_palette(&noisy, PaletteKind::Perceptual, 65, Forced::Primaries).unwrap_or_default()),
        ("mac", quantize::mac_palette()),
        ("web", quantize::web_palette()),
    ];
    for (pn, pal) in &pals {
        let mut hp = H0;
        pal_hash(&mut hp, pal);
        println!("r5_palette {pn} {} {hp:016x}", pal.len());
        for (xn, src, w) in [("grad", &grad, 93usize), ("noisy", &noisy, 50)] {
            let mut h = H0;
            for dither in [Dither::None, Dither::Diffusion, Dither::Pattern, Dither::Noise] {
                for transparent in [None, Some(1usize)] {
                    let mut px = src.clone();
                    let idx = quantize::quantize(&mut px, w, pal, dither, 0.75, transparent);
                    fnv(&mut h, &idx);
                    for p in &px {
                        hf(&mut h, p);
                    }
                }
            }
            println!("r5_quantize {pn} {xn} {h:016x}");
        }
    }
    let mut h = H0;
    for id in ["round", "ellipse", "line", "square", "diamond", "cross", "x", "", "Round"] {
        fnv(&mut h, format!("{:?}", HalftoneShape::from_id(id)).as_bytes());
    }
    println!("r5_halftone_from_id {h:016x}");
}
