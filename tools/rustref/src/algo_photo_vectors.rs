//! L9 algo photo reference vectors (`rustref algo-photo-vectors`): Camera Raw develop, HDR
//! merge / tone mapping, lens correction, perspective warp, vanishing point, adaptive wide angle
//! and Relight. One `key ... value` line per case; values are FNV-1a hashes of the outputs' bytes
//! (f32/f64 bit patterns little-endian). Settings that have serde are carried as compact JSON in
//! the line itself, so tests/algo/test_photo_parity.c replays them without a copy of the list.
use photocraft_algo::camera_raw::{self, CameraRaw};
use photocraft_algo::hdr::{self, MergeOptions, ToneMethod};
use photocraft_algo::lens::{self, EdgeMode, LensCorrection, LensMap};
use photocraft_algo::perspective::{self, PerspectiveMap, Plane, Straighten};
use photocraft_algo::tone::HdrToning;
use photocraft_algo::vanishing::{self, Edge, Scene, VpPlane};
use photocraft_algo::wideangle::{self, Camera, WideAngle};
use photocraft_algo::{FilterParams, apply, output_area};
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
fn hu(h: &mut u64, v: &[u64]) {
    for x in v {
        fnv(h, &x.to_le_bytes());
    }
}
fn hi(h: &mut u64, v: &[i32]) {
    for x in v {
        fnv(h, &x.to_le_bytes());
    }
}
fn hs(s: &Surface) -> u64 {
    let mut h = H0;
    let cb = s.content_bounds();
    hi(&mut h, &[cb.x0, cb.y0, cb.x1, cb.y1]);
    hf(&mut h, &s.read_region(cb));
    hf(&mut h, &s.default_pixel());
    fnv(&mut h, &[s.format().alpha as u8, s.channels() as u8]);
    h
}
fn hpx(v: &[[f32; 4]]) -> u64 {
    let mut h = H0;
    for p in v {
        hf(&mut h, p);
    }
    h
}

/// Deterministic sample in 0..=1 (C: the same integer formula).
fn pat(x: i64, y: i64, c: i64, seed: i64) -> f32 {
    ((x * 7 + y * 13 + c * 29 + seed * 31).rem_euclid(97)) as f32 / 96.0
}
/// Straight RGBA pixels: a smooth gradient plus pattern texture.
fn photo(w: usize, h: usize, seed: i64) -> Vec<[f32; 4]> {
    (0..w * h)
        .map(|i| {
            let (x, y) = ((i % w) as i64, (i / w) as i64);
            let g = |c: i64| 0.15 + 0.6 * (x as f32 / w as f32) * (0.5 + 0.5 * (c as f32 / 2.0)) + 0.25 * pat(x, y, c, seed) * (y as f32 / h as f32);
            [g(0), g(1), g(2), 1.0]
        })
        .collect()
}
fn surf(fmt: PixelFormat, r: Rect, seed: i64, opaque: bool) -> Surface {
    let mut s = Surface::new(fmt);
    let n = fmt.channels();
    let mut v = Vec::new();
    for y in r.y0..r.y1 {
        for x in r.x0..r.x1 {
            for c in 0..n {
                let a = ((x as i64 * 3 + y as i64 * 5 + seed).rem_euclid(7)) as f32 / 6.0;
                v.push(if fmt.alpha && c == n - 1 { if opaque { 1.0 } else { a } } else { pat(x as i64, y as i64, c as i64, seed) });
            }
        }
    }
    s.write_region(r, &v);
    s
}
fn formats() -> Vec<(&'static str, PixelFormat)> {
    vec![
        ("rgba8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, true)),
        ("rgb8", PixelFormat::new(ColorMode::Rgb, SampleType::U8, false)),
        ("rgba32", PixelFormat::new(ColorMode::Rgb, SampleType::F32, true)),
        ("gray16", PixelFormat::new(ColorMode::Grayscale, SampleType::U16, false)),
        ("cmyka8", PixelFormat::new(ColorMode::Cmyk, SampleType::U8, true)),
        ("laba16", PixelFormat::new(ColorMode::Lab, SampleType::U16, true)),
    ]
}

// ---------------------------------------------------------------------------------------------

fn camera_raw_vectors() {
    let curves = [
        "[]",
        "[[0,0],[64,40],[192,220],[255,255]]",
        "[[255,255],[0,10],[128,90]]",
        "[[0,0],[0,255],[10,20],[10.0000001,30],[255,200]]",
        "[[0,30],[255,255]]",
        "[[50,50]]",
        "[[0,0],[100,100],[200,100],[255,0]]",
        "[[-20,5],[300,400],[128,128]]",
    ];
    for c in curves {
        let pts: Vec<[f32; 2]> = serde_json::from_str(c).unwrap();
        let mut h = H0;
        hf(&mut h, &camera_raw::curve_lut(&pts, 1024));
        hf(&mut h, &camera_raw::curve_lut(&pts, 2));
        let v = match camera_raw::validate_curve(&pts) {
            Ok(()) => "ok".to_string(),
            Err(e) => e.replace(' ', "_"),
        };
        println!("crlut {c} {v} {h:016x}");
    }
    let mut h = H0;
    let mut k = -400.0f32;
    while k < 800.0 {
        hf(&mut h, &camera_raw::band_weights(k));
        hf(&mut h, &camera_raw::white_balance_gains(k / 4.0, -k / 7.0));
        k += 0.37;
    }
    hf(&mut h, &camera_raw::band_weights(f32::NAN));
    println!("crband {h:016x}");

    let settings = [
        "{}",
        r#"{"temperature":50}"#,
        r#"{"temperature":-30,"tint":40,"exposure":0.7}"#,
        r#"{"exposure":-1.5}"#,
        r#"{"contrast":100}"#,
        r#"{"contrast":-60,"whites":50,"blacks":-50}"#,
        r#"{"highlights":-100,"shadows":100}"#,
        r#"{"highlights":60}"#,
        r#"{"clarity":80}"#,
        r#"{"clarity":-50,"texture":70}"#,
        r#"{"texture":-80,"pixelScale":0.25}"#,
        r#"{"dehaze":60}"#,
        r#"{"dehaze":-60}"#,
        r#"{"vibrance":100}"#,
        r#"{"saturation":-100}"#,
        r#"{"vibrance":-40,"saturation":30}"#,
        r#"{"curveShadows":50,"curveHighlights":-40,"curveSplits":[10,60,80]}"#,
        r#"{"curveLights":70,"curveDarks":-30,"curveSplits":[90,20,1]}"#,
        r#"{"pointCurve":[[0,30],[255,255]],"curveShadows":50}"#,
        r#"{"pointCurveRed":[[0,0],[128,160],[255,255]],"pointCurveBlue":[[0,20],[255,230]]}"#,
        r#"{"hslHue":[30,0,-20,0,50,0,0,10],"hslSat":[-100,20,0,0,0,40,0,0],"hslLum":[0,0,30,-40,0,0,20,0]}"#,
        r#"{"gradeShadows":{"hue":220,"sat":80},"gradeHighlights":{"hue":40,"sat":50,"lum":20}}"#,
        r#"{"gradeMidtones":{"hue":120,"sat":30,"lum":-30},"gradeGlobal":{"hue":300,"sat":20,"lum":10},"gradeBlending":80,"gradeBalance":-40}"#,
        r#"{"sharpenAmount":100}"#,
        r#"{"sharpenAmount":80,"sharpenRadius":2.5,"sharpenDetail":70,"sharpenMasking":40}"#,
        r#"{"noiseLuminance":80}"#,
        r#"{"noiseLuminance":40,"noiseLuminanceDetail":10,"noiseColor":60,"noiseColorDetail":90}"#,
        r#"{"noiseColor":100,"pixelScale":0.5}"#,
        r#"{"grainAmount":50,"seed":4}"#,
        r#"{"grainAmount":80,"grainSize":60,"grainRoughness":10,"seed":9,"pixelScale":0.3}"#,
        r#"{"vignetteAmount":-80}"#,
        r#"{"vignetteAmount":60,"vignetteMidpoint":20,"vignetteRoundness":-70,"vignetteFeather":90}"#,
        r#"{"vignetteAmount":-50,"vignetteHighlights":70,"vignetteRoundness":50,"vignetteStyle":"colorPriority"}"#,
        r#"{"vignetteAmount":-50,"vignetteStyle":"paintOverlay"}"#,
        r#"{"vignetteAmount":40,"vignetteStyle":"paintOverlay"}"#,
        r#"{"temperature":20,"exposure":0.3,"contrast":20,"highlights":-30,"shadows":30,"clarity":20,"dehaze":10,"vibrance":20,"curveLights":10,"hslSat":[10,0,0,0,0,0,0,0],"gradeGlobal":{"hue":30,"sat":10},"sharpenAmount":40,"noiseLuminance":20,"grainAmount":10,"vignetteAmount":-20}"#,
    ];
    for (w, h) in [(37usize, 23usize), (800, 40), (5, 3)] {
        for js in settings {
            let p: CameraRaw = serde_json::from_str(js).unwrap();
            for float in [false, true] {
                if float && (w != 37) {
                    continue;
                }
                let mut px = photo(w, h, 3);
                if float {
                    for (i, q) in px.iter_mut().enumerate() {
                        q[0] *= 1.0 + (i % 5) as f32 * 0.3;
                    }
                }
                camera_raw::develop(&mut px, w, h, &p, float);
                println!("cr {w} {h} {} {js} {:016x}", float as u8, hpx(&px));
            }
        }
    }
    // serde round trip (to_value of a parsed value)
    for js in [settings[0], settings[16], settings[22], settings[33], r#"{"seed":4294967295,"pixelScale":1e-7,"vignetteStyle":"x y"}"#] {
        let p: CameraRaw = serde_json::from_str(js).unwrap();
        println!("crjson {}", serde_json::to_string(&p).unwrap());
    }
}

// ---------------------------------------------------------------------------------------------

fn scene(w: usize, h: usize) -> Vec<[f32; 3]> {
    (0..w * h)
        .map(|i| {
            let (x, y) = ((i % w) as f32 / w as f32, (i / w) as f32 / h as f32);
            let base = 0.002 * 2f32.powf(x * 12.0);
            let tex = 1.0 + 0.3 * ((y * 40.0).sin());
            [base * tex, base * tex * 0.8, base * tex * 0.6]
        })
        .collect()
}
fn shoot(sc: &[[f32; 3]], dt: f32) -> Vec<[f32; 4]> {
    let cam = |e: f32| (e.max(0.0).powf(0.45)).min(1.0);
    sc.iter().map(|r| [cam(r[0] * dt), cam(r[1] * dt), cam(r[2] * dt), 1.0]).map(|p| p.map(|v| (v * 255.0).round() / 255.0)).collect()
}

fn hdr_vectors() {
    let (w, h) = (96usize, 40usize);
    let sc = scene(w, h);
    let dts = [0.25f32, 1.0, 4.0, 16.0];
    let mut shots: Vec<Vec<[f32; 4]>> = dts.iter().map(|d| shoot(&sc, *d)).collect();
    // A moving object in the last exposure and a negative / over-range pixel.
    for y in 10..20 {
        for x in 30..45 {
            shots[3][y * w + x] = [0.3, 0.2, 0.1, 1.0];
        }
    }
    shots[1][5] = [-0.5, 1.5, 0.5, 0.5];
    let refs: Vec<&[[f32; 4]]> = shots.iter().map(Vec::as_slice).collect();
    let est = hdr::estimate_exposures(&refs);
    let mut hh = H0;
    hd(&mut hh, &est);
    println!("hdr_est {hh:016x}");
    let grey: Vec<f32> = (0..128 * 96).map(|i| 0.5 + 0.4 * (((i % 128) as f32 * 0.21).sin() * (((i / 128) as f32) * 0.17).cos())).collect();
    for (dx, dy) in [(5i32, -3i32), (0, 0), (-7, 6), (12, 1)] {
        let sh: Vec<f32> = (0..128 * 96)
            .map(|i| {
                let (x, y) = ((i % 128) as i32 - dx, (i / 128) as i32 - dy);
                grey[(y.clamp(0, 95) as usize) * 128 + x.clamp(0, 127) as usize] * 0.6
            })
            .collect();
        for lv in [0usize, 2, 4, 6] {
            let (ox, oy) = hdr::mtb_offset(128, 96, &sh, &grey, lv);
            println!("hdr_mtb {dx} {dy} {lv} {ox} {oy}");
        }
    }
    let samples = hdr::sample_pixels(w, h, &shots[1], 160);
    let mut hh = H0;
    hu(&mut hh, &samples.iter().map(|v| *v as u64).collect::<Vec<_>>());
    println!("hdr_samples {} {hh:016x}", samples.len());
    let samples2 = hdr::sample_pixels(w, h, &shots[1], 7);
    let mut hh = H0;
    hu(&mut hh, &samples2.iter().map(|v| *v as u64).collect::<Vec<_>>());
    println!("hdr_samples7 {} {hh:016x}", samples2.len());
    let z: Vec<Vec<u8>> = (0..3).map(|j| (0..40).map(|i| ((i * 37 + j * 61) % 256) as u8).collect()).collect();
    for (lam, ld) in [(40.0f64, [-1.0f64, 0.0, 1.0]), (1.0, [0.0, 0.0, 0.0]), (0.0, [-2.0, 0.5, 3.0])] {
        let g = hdr::response_curve(&z, &ld, lam);
        let mut hh = H0;
        hd(&mut hh, &g);
        println!("hdr_resp {lam} {hh:016x}");
    }
    for (ghost, base, given) in [(false, None, false), (true, Some(1usize), false), (true, None, false), (true, Some(9), true), (false, Some(0), true)] {
        let response = given.then(|| {
            let g: Vec<f64> = (0..256).map(|k| ((k as f64 + 1.0) / 129.0).ln()).collect();
            [g.clone(), g.clone(), g]
        });
        let m = hdr::merge(w, h, &refs, &MergeOptions { exposures: dts.iter().map(|d| *d as f64).collect(), remove_ghosts: ghost, ghost_base: base, response });
        let mut hh = H0;
        for c in 0..3 {
            hd(&mut hh, &m.response[c]);
        }
        hd(&mut hh, &[m.ghost_fraction, m.stops]);
        println!("hdr_merge {} {} {} {} {:016x} {hh:016x}", ghost as u8, base.map_or(-1, |b| b as i64), given as u8, m.ghost_base, hpx(&m.px));
        if !ghost && !given {
            let tm = [
                ToneMethod::LocalAdaptation(HdrToning { radius: 7.0, strength: 0.52, ..Default::default() }),
                ToneMethod::ExposureGamma { exposure: 0.5, gamma: 1.4 },
                ToneMethod::HighlightCompression,
                ToneMethod::EqualizeHistogram,
            ];
            for (k, t) in tm.iter().enumerate() {
                let mut px = m.px.clone();
                px[7][3] = 0.0;
                hdr::tone_map(&mut px, w, h, t);
                println!("hdr_tone {k} {:016x}", hpx(&px));
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------

/// Lens settings table (C: the same table in test_photo_parity.c).
fn lens_case(i: usize) -> LensCorrection {
    let d = LensCorrection::default();
    match i {
        0 => d,
        1 => LensCorrection { distortion: 50.0, ..d },
        2 => LensCorrection { distortion: -60.0, ..d },
        3 => LensCorrection { distortion: -60.0, edge: EdgeMode::Extension, ..d },
        4 => LensCorrection { distortion: -40.0, edge: EdgeMode::Color([1.0, 0.0, 0.0, 1.0]), ..d },
        5 => LensCorrection { vignette_amount: 100.0, vignette_midpoint: 30.0, ..d },
        6 => LensCorrection { vignette_amount: -100.0, edge: EdgeMode::Extension, ..d },
        7 => LensCorrection { red_cyan: -100.0, blue_yellow: 80.0, ..d },
        8 => LensCorrection { vertical: -50.0, horizontal: 30.0, ..d },
        9 => LensCorrection { angle: 5.0, scale: 110.0, edge: EdgeMode::Color([0.2, 0.4, 0.6, 0.5]), ..d },
        10 => LensCorrection { profile: Some(lens::generic_profile(16.0)), ..d },
        _ => {
            let mut p = lens::generic_profile(135.0);
            p.ca_red = 0.002;
            p.ca_blue = -0.001;
            p.p1 = 0.01;
            p.p2 = -0.02;
            p.vignette[2] = 0.05;
            LensCorrection { profile: Some(p), correct_vignette: false, vertical: 80.0, ..d }
        }
    }
}
const LENS_CASES: usize = 12;

fn lens_vectors() {
    for f in [5.0f64, 12.0, 16.0, 30.0, 50.0, 85.0, 135.0, 200.0, 999.5, 2.5, -0.4] {
        let p = lens::generic_profile(f);
        let mut h = H0;
        hd(&mut h, &[p.k1, p.k2, p.p1, p.p2, p.vignette[0], p.vignette[1], p.vignette[2], p.ca_red, p.ca_blue]);
        println!("lens_prof {f} {} {h:016x}", p.name.replace(' ', "_"));
    }
    let frame = Rect::new(-3, 2, 157, 92);
    for i in 0..LENS_CASES {
        let lc = lens_case(i);
        let m = LensMap::new(&lc, frame);
        let mut h = H0;
        for y in (-10..110).step_by(7) {
            for x in (-10..170).step_by(9) {
                match m.sample(x as f64 + 0.25, y as f64 + 0.5) {
                    Some(s) => {
                        hd(&mut h, &[s.p[0], s.p[1], s.pr[0], s.pr[1], s.pb[0], s.pb[1]]);
                        hf(&mut h, &[s.gain]);
                    }
                    None => fnv(&mut h, &[0xee]),
                }
            }
        }
        let auto = lens::auto_scale(&lc, frame);
        println!("lens_map {i} {h:016x} {:016x}", auto.to_bits());
    }
    let mut h = H0;
    for k in 0..40 {
        let a = [k as f64 * 1.5, -(k as f64)];
        let b = [100.0 - k as f64 * 3.0, k as f64 * k as f64 * 0.1];
        hd(&mut h, &[lens::straighten_angle(a, b)]);
    }
    println!("lens_straight {h:016x}");
    for (name, fmt) in formats() {
        let (w, hh) = if name == "rgba8" { (150, 90) } else { (61, 43) };
        let r = Rect::new(0, 0, w, hh);
        let s = surf(fmt, r, 2, false);
        for i in 0..LENS_CASES {
            println!("lens {name} {i} {:016x}", hs(&lens::correct(&s, r, &lens_case(i))));
        }
    }
}

// ---------------------------------------------------------------------------------------------

fn persp_sets() -> Vec<Vec<Plane>> {
    vec![
        vec![Plane::identity([[10.0, 10.0], [90.0, 12.0], [85.0, 70.0], [12.0, 75.0]])],
        vec![Plane { src: [[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [0.0, 100.0]], dst: [[20.0, 0.0], [80.0, 0.0], [100.0, 100.0], [0.0, 100.0]] }],
        vec![
            Plane { src: [[0.0, 0.0], [50.0, 0.0], [50.0, 100.0], [0.0, 100.0]], dst: [[0.0, 10.0], [50.0, 0.0], [55.0, 100.0], [0.0, 90.0]] },
            Plane { src: [[50.2, 0.0], [100.0, 0.0], [100.0, 100.0], [50.0, 100.2]], dst: [[50.0, 0.0], [110.0, 20.0], [100.0, 80.0], [55.0, 100.0]] },
        ],
        vec![
            Plane { src: [[0.0, 0.0], [40.0, 0.0], [40.0, 40.0], [0.0, 40.0]], dst: [[3.0, 2.0], [41.0, -1.0], [44.0, 43.0], [-2.0, 38.0]] },
            Plane { src: [[40.0, 0.0], [80.0, 0.0], [80.0, 40.0], [40.0, 40.0]], dst: [[41.0, -1.0], [85.0, 5.0], [79.0, 45.0], [44.0, 43.0]] },
            Plane { src: [[0.0, 40.0], [40.0, 40.0], [40.0, 80.0], [0.0, 80.0]], dst: [[-2.0, 38.0], [44.0, 43.0], [42.0, 86.0], [1.0, 79.0]] },
        ],
    ]
}

fn persp_vectors() {
    for (k, set) in persp_sets().into_iter().enumerate() {
        let groups = perspective::linked_corners(&set);
        let mut h = H0;
        for g in &groups {
            for &(p, c) in g {
                hu(&mut h, &[p as u64, c as u64]);
            }
            fnv(&mut h, &[0xff]);
        }
        println!("persp_groups {k} {} {h:016x}", groups.len());
        let m = PerspectiveMap::new(&set).unwrap();
        let mut h = H0;
        for y in (-20..130).step_by(6) {
            for x in (-20..130).step_by(5) {
                let (a, b) = m.map(x as f64 + 0.3, y as f64 - 0.2);
                hd(&mut h, &[a, b]);
            }
        }
        println!("persp {k} {} {h:016x}", PerspectiveMap::is_identity(&set) as u8);
        for (mode, mn) in [(Straighten::Horizontal, "h"), (Straighten::Vertical, "v"), (Straighten::Auto, "auto")] {
            let mut s = set.clone();
            perspective::straighten(&mut s, mode);
            let mut h = H0;
            for p in &s {
                for c in p.src.iter().chain(&p.dst) {
                    hd(&mut h, c);
                }
                hd(&mut h, &perspective::plane_point(p, 0.3, 0.7, true));
                hd(&mut h, &perspective::plane_point(p, 0.6, 0.1, false));
            }
            println!("persp_straight {k} {mn} {h:016x}");
        }
    }
}

// ---------------------------------------------------------------------------------------------

fn floor_quad(f: f64, c: [f64; 2], rx: f64, ry: f64) -> VpPlane {
    let rot = |p: [f64; 3]| {
        let (s, co) = rx.sin_cos();
        let q = [p[0], co * p[1] - s * p[2], s * p[1] + co * p[2]];
        let (s2, c2) = ry.sin_cos();
        [c2 * q[0] + s2 * q[2], q[1], -s2 * q[0] + c2 * q[2]]
    };
    let corners = [[-1.0, -0.5, 0.0], [1.0, -0.5, 0.0], [1.0, 0.5, 0.0], [-1.0, 0.5, 0.0]].map(|p: [f64; 3]| {
        let r = rot(p);
        let z = r[2] + 4.0;
        [c[0] + f * r[0] / z, c[1] + f * (r[1] + 0.0) / z]
    });
    VpPlane { corners }
}

fn vp_vectors() {
    let c = [400.0, 300.0];
    for (k, (rx, ry)) in [(0.6f64, 0.4f64), (0.3, -0.5), (0.0, 0.0), (1.1, 0.2)].iter().enumerate() {
        let pl = floor_quad(500.0, c, *rx, *ry);
        let f = pl.focal(c);
        let fv = f.unwrap_or(-1.0);
        let lf = pl.lift(f.unwrap_or(800.0), c);
        let (lu, lv) = lf.lengths();
        let mut h = H0;
        for p in &lf.pts {
            hd(&mut h, p);
        }
        hd(&mut h, &[lu, lv]);
        println!("vp_plane {k} {:016x} {h:016x}", fv.to_bits());
        let mut sc = Scene::new(pl.clone(), c, None, 800.0);
        let mut h = H0;
        for (e, angle, depth) in [(Edge::Top, 90.0, 1.0), (Edge::Right, 70.0, 0.5), (Edge::Bottom, 120.0, 2.0), (Edge::Left, 90.0, 0.01)] {
            match sc.tear_off(0, e, angle, depth) {
                Some(i) => {
                    for q in &sc.planes[i].corners {
                        hd(&mut h, q);
                    }
                }
                None => fnv(&mut h, &[0xee]),
            }
        }
        let at = [[400.0, 300.0], [10.0, 10.0], [700.0, 500.0]];
        for p in at {
            hu(&mut h, &[sc.plane_at(p).map_or(u64::MAX, |v| v as u64)]);
        }
        println!("vp_tear {k} {} {h:016x}", sc.planes.len());
    }
    let sc = Scene::new(floor_quad(500.0, c, 0.6, 0.4), c, None, 800.0);
    let img = surf(PixelFormat::RGBA8, Rect::new(0, 0, 100, 50), 5, false);
    for (at, width) in [([0.25, 0.25], 0.5), ([0.0, 0.0], 1.0), ([0.6, 0.1], 0.3)] {
        let out = vanishing::paste(&sc, 0, &img, Rect::new(0, 0, 100, 50), at, width).unwrap();
        println!("vp_paste {} {} {width} {:016x}", at[0], at[1], hs(&out));
    }
    let h = sc.planes[0].homography().unwrap();
    for (fmt_name, fmt) in [("rgba8", PixelFormat::RGBA8), ("rgba32", PixelFormat::new(ColorMode::Rgb, SampleType::F32, true))] {
        let mut s = surf(fmt, Rect::new(150, 150, 650, 500), 1, true);
        let (sx, sy) = h.apply(0.5, 0.8);
        let pts: Vec<[f64; 2]> = (0..5).map(|k| {
            let (x, y) = h.apply(0.3 + k as f64 * 0.08, 0.3);
            [x, y]
        }).collect();
        let n = vanishing::clone_stroke(&sc, &mut s, [sx, sy], &pts, 14.0, 0.6, 0.8);
        println!("vp_clone {fmt_name} {n} {:016x}", hs(&s));
    }
}

// ---------------------------------------------------------------------------------------------

fn wide_vectors() {
    let frame = Rect::new(0, 0, 480, 320);
    let mut h = H0;
    for js in [r#"{"model":"fisheye","focalLength":8}"#, r#"{"model":"perspective"}"#, "{}", r#"{"model":"fullSpherical"}"#, r#"{"focalLength":10,"cropFactor":1.5}"#] {
        let p: WideAngle = serde_json::from_str(js).unwrap();
        for fr in [frame, Rect::new(-10, 5, 790, 405)] {
            let cam = Camera::new(&p, fr);
            hd(&mut h, &[cam.f, cam.c[0], cam.c[1], cam.size[0], cam.size[1]]);
            for (x, y) in [(10.0, 20.0), (300.0, 200.0), (470.0, 10.0), (-50.0, 900.0)] {
                let r = cam.ray(x, y);
                hd(&mut h, &r);
                match cam.pixel(r) {
                    Some(q) => hd(&mut h, &q),
                    None => fnv(&mut h, &[0xee]),
                }
            }
            for q in cam.arc([50.0, 60.0], [450.0, 70.0], 24) {
                hd(&mut h, &q);
            }
        }
        println!("wide_cam {js} {h:016x}");
    }
    let cases = [
        "{}",
        r#"{"scale":80}"#,
        r#"{"model":"fisheye","focalLength":8,"constraints":[{"a":[60,70],"b":[420,70],"orientation":"horizontal"},{"a":[80,60],"b":[80,260]}]}"#,
        r#"{"model":"fisheye","focalLength":12,"constraints":[{"a":[20,30],"b":[460,35],"orientation":"horizontal"}]}"#,
        r#"{"model":"perspective","constraints":[{"a":[100,20],"b":[110,300],"orientation":"vertical"},{"a":[30,250],"b":[400,280]}],"scale":110}"#,
        r#"{"model":"fullSpherical","constraints":[{"a":[10,100],"b":[300,120]}]}"#,
    ];
    for js in cases {
        let p: WideAngle = serde_json::from_str(js).unwrap();
        for fr in [frame, Rect::new(3, 4, 103, 37)] {
            let m = wideangle::solve(&p, fr);
            let mut h = H0;
            for (o, s) in &m.verts {
                hd(&mut h, o);
                hd(&mut h, s);
            }
            for c in &m.curves {
                for q in c {
                    hd(&mut h, q);
                }
                fnv(&mut h, &[0xff]);
            }
            hd(&mut h, &[m.residual]);
            for q in [[300.0, 250.0], [-5.0, 1000.0]] {
                hd(&mut h, &m.map(fr, q));
            }
            println!("wide {} {} {js} {} {} {h:016x}", fr.width(), fr.height(), m.nx, m.ny);
        }
        println!("widejson {}", serde_json::to_string(&p).unwrap());
    }
}

// ---------------------------------------------------------------------------------------------

fn relight_vectors() {
    let cases = [
        r#"{"filter":"relight","angle":45,"elevation":40,"intensity":40,"ambient":55,"warmth":0,"softness":25}"#,
        r#"{"filter":"relight","angle":180,"elevation":30,"intensity":90,"ambient":12,"warmth":0,"softness":15}"#,
        r#"{"filter":"relight","angle":-60,"elevation":80,"intensity":60,"ambient":30,"warmth":80,"softness":90}"#,
        r#"{"filter":"relight","angle":10,"elevation":0,"intensity":100,"ambient":100,"warmth":-70,"softness":1}"#,
        r#"{"filter":"relight","angle":0,"elevation":40,"intensity":0,"ambient":55,"warmth":0,"softness":25}"#,
    ];
    let r = Rect::new(0, 0, 40, 30);
    for js in cases {
        let p: FilterParams = serde_json::from_str(js).unwrap();
        for (name, fmt) in formats() {
            let s = surf(fmt, r, 4, name == "rgba8");
            for bounds in [r, Rect::new(0, 0, 300, 200)] {
                let area = output_area(&p, s.content_bounds(), bounds, None);
                let out = apply(&s, &p, area, bounds, None);
                println!("relight {name} {} {js} {:016x}", bounds.width(), hs(&out));
            }
        }
    }
    let big = Rect::new(0, 0, 1600, 1000);
    let mut h = H0;
    for soft in [-5.0f32, 0.0, 1.0, 12.5, 25.0, 50.0, 99.0, 100.0, 500.0, f32::NAN] {
        for b in [big, Rect::new(0, 0, 10, 4000), Rect::EMPTY] {
            let p = FilterParams::Relight { angle: 0.0, elevation: 10.0, intensity: 5.0, ambient: 0.0, warmth: 0.0, softness: soft };
            match p.halo_for(b) {
                photocraft_algo::Halo::Radius(v) => hi(&mut h, &[v]),
                photocraft_algo::Halo::Bounds => fnv(&mut h, &[0xbb]),
            }
        }
    }
    println!("relight_halo {h:016x}");
}

pub fn run() {
    camera_raw_vectors();
    hdr_vectors();
    lens_vectors();
    persp_vectors();
    vp_vectors();
    wide_vectors();
    relight_vectors();
}
