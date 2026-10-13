//! R3 (resync) additions to `rustref raw-vectors`: Fujifilm RAF, monochrome / deflate / lossy /
//! JPEG XL DNG, ProfileGainTableMap, LONG orientations, the Camera Raw tone sliders and black point,
//! retone, camera looks, embedded preview lists and colour spaces, and hostile sweeps over the new
//! samples. Printed after a `section r3` line; checked by tests/raw/test_parity2.c.
//!
//! Samples whose bytes come from an encoder the C tree doesn't share (zlib streams from flate2, JPEG
//! tiles from `image`, JPEG XL from zune-jpegxl) are written by `rustref raw-fixtures DIR` into
//! tests/fixtures/raw/<name>.bin; lossy ones also get <name>.zune (zune-jpeg's decoded samples, u16
//! LE) so the C test can measure libjpeg-turbo's difference.
use photocraft_raw::testgen::*;
use photocraft_raw::*;

use crate::raw_vectors::{H0, developed_hash, err_msg, f32b, f64b, file_hash, fnv, noise, sensor_hash, u64le};

/// How a sample is checked: built by the C testgen and compared exactly (`e`), loaded from a fixture
/// and compared exactly (`f`), a lossy fixture compared with a tolerance (`l`), or a JPEG XL fixture
/// (`j`: exact with libjxl, "unsupported" without).
#[derive(Clone, Copy, PartialEq)]
enum Kind {
    E,
    F,
    L,
    J,
}

impl Kind {
    fn letter(self) -> char {
        match self {
            Kind::E => 'e',
            Kind::F => 'f',
            Kind::L => 'l',
            Kind::J => 'j',
        }
    }
}

fn sensor_hash2(x: &Sensor) -> u64 {
    let mut h = H0;
    u64le(&mut h, sensor_hash(x));
    f32b(&mut h, x.black_point);
    h
}

fn jxl(samples: &[u16], w: usize, h: usize, channels: usize, bits: u16) -> Vec<u8> {
    use zune_core::{bit_depth::BitDepth, colorspace::ColorSpace, options::EncoderOptions};
    let (depth, bytes): (BitDepth, Vec<u8>) = match bits {
        8 => (BitDepth::Eight, samples.iter().map(|&v| v as u8).collect()),
        _ => (BitDepth::Sixteen, samples.iter().flat_map(|v| v.to_ne_bytes()).collect()),
    };
    let space = if channels == 1 { ColorSpace::Luma } else { ColorSpace::RGB };
    let mut out = Vec::new();
    zune_jpegxl::JxlSimpleEncoder::new(&bytes, EncoderOptions::new(w, h, space, depth)).encode(&mut out).unwrap();
    out
}

fn tiles(data: &[u16], w: usize, h: usize, spp: usize, tw: usize, th: usize) -> Vec<Vec<u16>> {
    let mut out = Vec::new();
    for ty in (0..h).step_by(th) {
        for tx in (0..w).step_by(tw) {
            let mut t = Vec::with_capacity(tw * th * spp);
            for y in 0..th {
                for x in 0..tw {
                    let at = ((ty + y).min(h - 1) * w + (tx + x).min(w - 1)) * spp;
                    t.extend_from_slice(&data[at..at + spp]);
                }
            }
            out.push(t);
        }
    }
    out
}

fn ramp(w: usize, h: usize, lo: u16, hi: u16) -> Vec<u16> {
    (0..w * h).map(|i| lo + ((u32::from(hi - lo) * (i % w) as u32) / (w as u32 - 1)) as u16).collect()
}

fn xtrans(packing: RafPacking, bits: u32) -> RafSpec {
    let (w, h) = (30, 18);
    let white = ((1u32 << bits) - 1) as u16;
    let mut s = RafSpec::new(w, h, mosaic_cfa(&scene(w, h), w, &XTRANS, 6, 256, white), XTRANS.to_vec());
    s.bits = bits;
    s.packing = packing;
    s.black = vec![256; 36];
    s.crop = Some([2, 2, 14, 26]);
    s
}

fn with_long_orientation(spec: &DngSpec, from: u16, to: u32) -> Vec<u8> {
    let mut b = spec.build();
    let short = [&[0x12, 0x01, 3, 0, 1, 0, 0, 0][..], &from.to_le_bytes(), &[0, 0]].concat();
    let at: Vec<usize> = b.windows(short.len()).enumerate().filter(|(_, w)| *w == short.as_slice()).map(|(i, _)| i).collect();
    assert_eq!(at.len(), 1);
    let long = [&[0x12, 0x01, 4, 0, 1, 0, 0, 0][..], &to.to_le_bytes()].concat();
    b[at[0]..at[0] + long.len()].copy_from_slice(&long);
    b
}

/// The new hostile samples of tests/hostile.rs (JPEG XL and deflate ones are fixtures).
fn hostile_r3() -> Vec<(String, Vec<u8>, Kind)> {
    let (w, h) = (24, 12);
    let cfa = mosaic(&scene(w, h), w, [0, 1, 1, 2], 64, 4000);
    let mut out = Vec::new();
    let rgb: Vec<u16> = scene(w, h).iter().flat_map(|p| p.map(|v| (v * 30000.0) as u16)).collect();
    let mut j = DngSpec::cfa(w, h, rgb.clone());
    j.samples = 3;
    j.storage = DngStorage::JxlStrips { rows: h };
    j.jxl_segments = vec![jxl(&rgb, w, h, 3, 16)];
    j.profile_gain_table_map = Some(vec![0; 32]);
    out.push(("h_jxl".to_string(), j.build(), Kind::J));
    let grey: Vec<u16> = scene(w, h).iter().map(|p| 64 + (p[1] * 3900.0) as u16).collect();
    let mut m = DngSpec::monochrome(w, h, grey);
    m.bits = 12;
    m.white = 4000;
    m.black = vec![64];
    m.storage = DngStorage::Lj92Tiles { width: 16, height: 8 };
    out.push(("h_mono_lj92".to_string(), m.build(), Kind::E));
    m.storage = DngStorage::DeflateStrips { rows: 5, predictor: 2 };
    out.push(("h_mono_deflate".to_string(), m.build(), Kind::F));
    let xt = mosaic_cfa(&scene(w, h), w, &XTRANS, 6, 256, 4000);
    for (name, packing) in [("h_raf_u16", RafPacking::U16 { le: true }), ("h_raf_lsb12", RafPacking::Lsb12)] {
        let raf =
            RafSpec { bits: 12, packing, black: vec![256; 36], crop: Some([1, 2, 10, 20]), orientation: 8, ..RafSpec::new(w, h, xt.clone(), XTRANS.to_vec()) };
        out.push((name.to_string(), raf.build(), Kind::E));
    }
    let bayer = RafSpec { packing: RafPacking::Words14, exposure_bias: Some((-100, 100)), ..RafSpec::new(w, h, cfa.clone(), vec![0, 1, 1, 2]) };
    out.push(("h_raf_words14".to_string(), bayer.build(), Kind::E));
    out
}

fn samples_r3() -> Vec<(String, Vec<u8>, Kind)> {
    let mut v = hostile_r3();
    let mut add = |n: &str, b: Vec<u8>, k: Kind| v.push((n.to_string(), b, k));
    // tests/raf.rs
    for (name, packing, bits) in [
        ("raf_u16le_14", RafPacking::U16 { le: true }, 14),
        ("raf_u16be_16", RafPacking::U16 { le: false }, 16),
        ("raf_lsb12", RafPacking::Lsb12, 12),
        ("raf_words14", RafPacking::Words14, 14),
    ] {
        add(name, xtrans(packing, bits).build(), Kind::E);
    }
    let mut spec = xtrans(RafPacking::U16 { le: true }, 14);
    spec.exposure_bias = Some((-72, 100));
    spec.orientation = 6;
    add("raf_meta", spec.build(), Kind::E);
    let (w, h) = (36, 24);
    let rgb: Vec<[f32; 3]> = vec![[0.4 / 2.0, 0.4, 0.4 / 1.5]; w * h];
    let spec = RafSpec { black: vec![256], ..RafSpec::new(w, h, mosaic_cfa(&rgb, w, &XTRANS, 6, 256, 16383), XTRANS.to_vec()) };
    add("raf_grey", spec.build(), Kind::E);
    let (w, h) = (24, 16);
    for (cname, cfa) in [("rggb", [0u8, 1, 1, 2]), ("bggr", [2, 1, 1, 0])] {
        for (pname, packing, bits) in [("lsb12", RafPacking::Lsb12, 12), ("words14", RafPacking::Words14, 14), ("u16", RafPacking::U16 { le: true }, 14)] {
            let white = ((1u32 << bits) - 1) as u16;
            let data = mosaic_cfa(&scene(w, h), w, &cfa, 2, 64, white);
            let spec = RafSpec { bits, packing, black: vec![64, 64, 64, 64], ..RafSpec::new(w, h, data, cfa.to_vec()) };
            add(&format!("raf_{cname}_{pname}"), spec.build(), Kind::E);
        }
    }
    let mut spec = xtrans(RafPacking::Compressed, 14);
    add("raf_compressed", spec.build(), Kind::E);
    spec.packing = RafPacking::U16 { le: true };
    spec.cfa_tiff = false;
    add("raf_nocfa", spec.build(), Kind::E);
    add("raf_badlayout", RafSpec::new(8, 4, vec![100; 32], vec![0, 0, 2, 2]).build(), Kind::E);
    let b = xtrans(RafPacking::U16 { le: true }, 14).build();
    add("raf_trunc", b[..b.len() - 7].to_vec(), Kind::E);

    // tests/dng.rs: monochrome LinearRaw
    let (w, h) = (37, 23);
    for (name, bits, storage, kind) in [
        ("mono_strips16", 16u16, DngStorage::Strips { rows: 5 }, Kind::E),
        ("mono_packed12", 12, DngStorage::Strips { rows: 4 }, Kind::E),
        ("mono_b8", 8, DngStorage::Strips { rows: 23 }, Kind::E),
        ("mono_lj92s", 14, DngStorage::Lj92Strips { rows: 6 }, Kind::E),
        ("mono_lj92t", 16, DngStorage::Lj92Tiles { width: 16, height: 8 }, Kind::E),
        ("mono_deflate", 16, DngStorage::DeflateStrips { rows: 7, predictor: 2 }, Kind::F),
        ("mono_lossy", 16, DngStorage::LossyJpegTiles { width: 37, height: 23 }, Kind::L),
    ] {
        let white = ((1u32 << bits) - 1) as u16;
        let mut spec = DngSpec::monochrome(w, h, ramp(w, h, 0, white));
        spec.bits = bits;
        spec.white = u32::from(white);
        spec.storage = storage;
        add(name, spec.build(), kind);
    }
    let (w, h) = (32, 4);
    let mut spec = DngSpec::monochrome(w, h, ramp(w, h, 1000, 60000));
    spec.black = vec![1000];
    spec.white = 60000;
    add("mono_levels", spec.build(), Kind::E);
    let (w, h) = (16, 6);
    let base = DngSpec::monochrome(w, h, ramp(w, h, 100, 4000));
    let rat = |v: &[f64]| Val::SRational(v.iter().map(|x| ((x * 10000.0) as i32, 10000)).collect());
    let variants: Vec<DngSpec> = vec![
        DngSpec { ifd0_extra: vec![(50721, rat(&[0.3, 0.6, 0.1])), (50778, Val::Short(vec![21]))], ..base.clone() },
        DngSpec { ifd0_extra: vec![(50721, rat(&[1.0]))], ..base.clone() },
        DngSpec { ifd0_extra: vec![(50728, Val::Rational(vec![(1, 1)]))], ..base.clone() },
        DngSpec { black: vec![100, 120], ..base.clone() },
        DngSpec { black: vec![100, 110, 120], ..base.clone() },
        DngSpec { black: vec![100, 101, 102, 103], black_repeat: (2, 2), ..base.clone() },
        DngSpec { black: vec![100], black_repeat: (2, 2), ..base.clone() },
        DngSpec { color_matrix1: Some((21, [3.2, -1.5, -0.5, -1.0, 1.9, 0.04, 0.06, -0.2, 1.06])), ..base.clone() },
        DngSpec { as_shot_neutral: Some([0.5, 1.0, 0.7]), ..base.clone() },
    ];
    for (i, s) in variants.iter().enumerate() {
        add(&format!("mono_odd{i}"), s.build(), Kind::E);
    }
    let two = DngSpec { samples: 2, data: ramp(2 * w, h, 100, 4000), ..base.clone() };
    add("mono_two", two.build(), Kind::E);

    // deflate (compression 8)
    let (w, h) = (48, 40);
    for (i, (bits, storage, be, samples)) in [
        (16u16, DngStorage::DeflateStrips { rows: 16, predictor: 1 }, false, 1usize),
        (16, DngStorage::DeflateStrips { rows: 16, predictor: 2 }, false, 1),
        (12, DngStorage::DeflateStrips { rows: 12, predictor: 2 }, false, 1),
        (16, DngStorage::DeflateStrips { rows: 16, predictor: 2 }, true, 1),
        (16, DngStorage::DeflateStrips { rows: 40, predictor: 2 }, false, 3),
    ]
    .into_iter()
    .enumerate()
    {
        let mut spec = DngSpec::cfa(w, h, noise(w * h * samples, bits));
        spec.samples = samples;
        spec.bits = bits;
        spec.storage = storage;
        spec.big_endian = be;
        spec.white = (1 << bits) - 1;
        add(&format!("deflate{i}"), spec.build(), Kind::F);
    }

    // JPEG XL (compression 52546)
    let (w, h) = (37, 23);
    for (i, (spp, bits, tiled)) in [(3usize, 16u16, false), (3, 16, true), (1, 16, false), (1, 16, true), (3, 8, false), (1, 8, true)].into_iter().enumerate() {
        let data = noise(w * h * spp, bits);
        let mut spec = DngSpec::cfa(w, h, data.clone());
        spec.samples = spp;
        spec.bits = bits;
        spec.white = (1u32 << bits) - 1;
        if tiled {
            spec.storage = DngStorage::JxlTiles { width: 16, height: 16 };
            spec.jxl_segments = tiles(&data, w, h, spp, 16, 16).iter().map(|t| jxl(t, 16, 16, spp, bits)).collect();
        } else {
            let rows = 10;
            spec.storage = DngStorage::JxlStrips { rows };
            spec.jxl_segments = (0..h)
                .step_by(rows)
                .map(|y| (y, (y + rows).min(h) - y))
                .map(|(y, r)| jxl(&data[y * w * spp..(y + r) * w * spp], w, r, spp, bits))
                .collect();
        }
        add(&format!("jxl{i}"), spec.build(), Kind::J);
    }
    let (w, h) = (24, 16);
    let data: Vec<u16> = scene(w, h).iter().flat_map(|p| p.map(|v| (v * 20000.0) as u16)).collect();
    let mut spec = DngSpec::cfa(w, h, data.clone());
    spec.samples = 3;
    spec.storage = DngStorage::JxlStrips { rows: h };
    spec.jxl_segments = vec![jxl(&data, w, h, 3, 16)];
    spec.baseline_exposure = Some(1.0);
    add("jxl_dev", spec.build(), Kind::J);
    let (w, h) = (16, 8);
    let data = noise(w * h * 3, 16);
    let mut spec = DngSpec::cfa(w, h, data.clone());
    spec.samples = 3;
    spec.storage = DngStorage::JxlStrips { rows: h };
    spec.jxl_segments = vec![jxl(&data[..w * 4 * 3], w, 4, 3, 16)];
    add("jxl_short", spec.build(), Kind::J);
    spec.jxl_segments = vec![jxl(&data[..w * h], w, h, 1, 16)];
    add("jxl_grey", spec.build(), Kind::J);
    spec.jxl_segments = vec![vec![0xff, 0x0a, 1, 2, 3, 4, 5]];
    add("jxl_junk", spec.build(), Kind::J);

    // ProfileGainTableMap, LONG orientations
    let mut spec = DngSpec::cfa(8, 8, vec![500; 64]);
    spec.profile_gain_table_map = Some(vec![0; 64]);
    add("pgtm", spec.build(), Kind::E);
    let mut spec = DngSpec::cfa(8, 6, vec![500; 48]);
    spec.orientation = 2;
    add("orient_long", with_long_orientation(&spec, 2, 65_538), Kind::E);
    add("orient_long6", with_long_orientation(&spec, 2, 6), Kind::E);

    // lossy JPEG tiles (compression 34892)
    let (w, h) = (32, 32);
    let data: Vec<u16> = (0..w * h).map(|i| 4096 + (i / w) as u16 * 16).collect();
    let mut spec = DngSpec::cfa(w, h, data);
    spec.storage = DngStorage::LossyJpegTiles { width: 16, height: 16 };
    add("lossy_cfa", spec.build(), Kind::L);
    let data: Vec<u16> = (0..w * h * 3).map(|i| 4096 + ((i / (w * 3)) as u16 * 16) + (i % 3) as u16 * 64 * 256).collect();
    let mut spec = DngSpec::cfa(w, h, data);
    spec.samples = 3;
    spec.storage = DngStorage::LossyJpegTiles { width: 16, height: 16 };
    add("lossy_rgb", spec.build(), Kind::L);

    // a larger scene for the tone sliders (a work copy with more than 2000 pixels)
    let (w, h) = (300, 200);
    let mut spec = DngSpec::cfa(w, h, mosaic(&scene(w, h), w, [0, 1, 1, 2], 0, 65535));
    spec.as_shot_neutral = Some([1.0; 3]);
    add("big", spec.build(), Kind::E);
    v
}

/// Writes the fixture samples (and the zune-jpeg decodes of the lossy ones) into dir.
pub fn raw_fixtures(dir: &str) {
    std::fs::create_dir_all(dir).unwrap();
    for (name, b, kind) in samples_r3() {
        if kind == Kind::E {
            continue;
        }
        std::fs::write(format!("{dir}/{name}.bin"), &b).unwrap();
        if kind == Kind::L {
            let s = decode(&b, &Limits::default()).unwrap();
            let bytes: Vec<u8> = s.data.iter().flat_map(|v| v.to_le_bytes()).collect();
            std::fs::write(format!("{dir}/{name}.zune"), bytes).unwrap();
        }
    }
}

fn tone_opts() -> Vec<(&'static str, Tone)> {
    let n = Tone::NEUTRAL;
    vec![
        ("sh+100", Tone { shadows: 100.0, ..n }),
        ("sh-60", Tone { shadows: -60.0, ..n }),
        ("hi-100", Tone { highlights: -100.0, ..n }),
        ("hi+50", Tone { highlights: 50.0, ..n }),
        ("cl+100", Tone { clarity: 100.0, ..n }),
        ("cl-100", Tone { clarity: -100.0, ..n }),
        ("tx+100", Tone { texture: 100.0, ..n }),
        ("tx-100", Tone { texture: -100.0, ..n }),
        ("dh+100", Tone { dehaze: 100.0, ..n }),
        ("dh-100", Tone { dehaze: -100.0, ..n }),
        ("mix", Tone { shadows: -60.0, highlights: 40.0, clarity: 50.0, texture: -40.0, dehaze: 30.0 }),
    ]
}

fn look_hash(l: &CameraLook) -> u64 {
    let mut h = H0;
    crate::raw_vectors::mat(&mut h, &l.to_xyz);
    match &l.tone {
        Some(t) => {
            let k = t.knots();
            u64le(&mut h, k.len() as u64);
            for p in k {
                f64b(&mut h, p[0]);
                f64b(&mut h, p[1]);
            }
        }
        None => fnv(&mut h, &[0xFF]),
    }
    f64b(&mut h, l.delta_e);
    f64b(&mut h, l.delta_e_fallback);
    h
}

// ---- the synthetic camera of look.rs's tests ----

mod rcol {
    pub type Mat3 = [[f64; 3]; 3];
    const IDENTITY: Mat3 = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
    pub const D50_XY: [f64; 2] = [0.3457, 0.3585];
    const D65_XY: [f64; 2] = [0.3127, 0.3290];
    pub const ROMM: [[f64; 2]; 3] = [[0.7347, 0.2653], [0.1596, 0.8404], [0.0366, 0.0001]];
    const REC709: [[f64; 2]; 3] = [[0.64, 0.33], [0.30, 0.60], [0.15, 0.06]];
    pub fn mul(a: &Mat3, b: &Mat3) -> Mat3 {
        let mut o = [[0.0; 3]; 3];
        for (i, row) in o.iter_mut().enumerate() {
            for (j, v) in row.iter_mut().enumerate() {
                *v = (0..3).map(|k| a[i][k] * b[k][j]).sum();
            }
        }
        o
    }
    pub fn apply(m: &Mat3, v: [f64; 3]) -> [f64; 3] {
        [0, 1, 2].map(|i| m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2])
    }
    fn diag(v: [f64; 3]) -> Mat3 {
        [[v[0], 0.0, 0.0], [0.0, v[1], 0.0], [0.0, 0.0, v[2]]]
    }
    pub fn invert(m: &Mat3) -> Option<Mat3> {
        let [[a, b, c], [d, e, f], [g, h, i]] = *m;
        let det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        if !det.is_finite() || det.abs() < 1e-12 {
            return None;
        }
        let r = 1.0 / det;
        let o = [
            [(e * i - f * h) * r, (c * h - b * i) * r, (b * f - c * e) * r],
            [(f * g - d * i) * r, (a * i - c * g) * r, (c * d - a * f) * r],
            [(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r],
        ];
        o.iter().flatten().all(|v| v.is_finite()).then_some(o)
    }
    fn xy_to_xyz(xy: [f64; 2]) -> [f64; 3] {
        let y = xy[1].max(1e-6);
        [xy[0] / y, 1.0, (1.0 - xy[0] - xy[1]) / y]
    }
    fn bradford(src: [f64; 3], dst: [f64; 3]) -> Mat3 {
        const M: Mat3 = [[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]];
        let Some(mi) = invert(&M) else { return IDENTITY };
        let s = apply(&M, src);
        let d = apply(&M, dst);
        if s.iter().any(|v| v.abs() < 1e-9) {
            return IDENTITY;
        }
        mul(&mi, &mul(&diag([d[0] / s[0], d[1] / s[1], d[2] / s[2]]), &M))
    }
    pub fn rgb_to_xyz(prim: [[f64; 2]; 3], white: [f64; 2]) -> Mat3 {
        let cols = prim.map(xy_to_xyz);
        let p: Mat3 = [[cols[0][0], cols[1][0], cols[2][0]], [cols[0][1], cols[1][1], cols[2][1]], [cols[0][2], cols[1][2], cols[2][2]]];
        let Some(pi) = invert(&p) else { return IDENTITY };
        let s = apply(&pi, xy_to_xyz(white));
        mul(&p, &diag(s))
    }
    pub fn xyz_d50_to_prophoto() -> Mat3 {
        invert(&rgb_to_xyz(ROMM, D50_XY)).unwrap_or(IDENTITY)
    }
    pub fn srgb_to_xyz_d50() -> Mat3 {
        mul(&bradford(xy_to_xyz(D65_XY), xy_to_xyz(D50_XY)), &rgb_to_xyz(REC709, D65_XY))
    }
}

const WB: [f64; 3] = [1.5, 1.0, 1.25];

fn patches(w: usize, h: usize) -> Vec<[f32; 3]> {
    let mut seed = 12345u32;
    let mut rnd = || {
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        (seed >> 8) as f32 / (1u32 << 24) as f32
    };
    let (pw, ph) = (12, 8);
    let colours: Vec<[f32; 3]> = (0..pw * ph).map(|_| [0.15 + 0.45 * rnd(), 0.15 + 0.45 * rnd(), 0.15 + 0.45 * rnd()]).collect();
    (0..w * h)
        .map(|i| {
            let (x, y) = (i % w, i / w);
            let (px, py) = (x * pw / w, y * ph / h);
            let ramp = 0.15 + 0.85 * ((x * pw) % w) as f32 / w as f32;
            colours[py * pw + px].map(|v| v * ramp)
        })
        .collect()
}

fn true_matrix() -> rcol::Mat3 {
    let k = [[1.35, -0.25, -0.1], [-0.15, 1.25, -0.1], [0.0, -0.3, 1.3]];
    rcol::mul(&rcol::mul(&rcol::xyz_d50_to_prophoto(), &rcol::srgb_to_xyz_d50()), &k)
}

const SHOT_W: usize = 480;
const SHOT_H: usize = 320;

fn shot_cam() -> Vec<[f32; 3]> {
    patches(SHOT_W, SHOT_H).iter().map(|p| [0, 1, 2].map(|k| p[k] / WB[k] as f32)).collect()
}

/// look.rs's shot(): the raw and its JPEG-like rendering (8-bit sRGB).
fn shot_raw() -> Vec<u8> {
    let data = mosaic(&shot_cam(), SHOT_W, [0, 1, 1, 2], 0, 4095);
    tiff_ep("NIKON CORPORATION", SHOT_W, SHOT_H, &data, [0, 1, 1, 2], 12, vec![])
}

fn shot_reference() -> (Vec<u8>, usize, usize) {
    let m = true_matrix();
    let to_srgb = rcol::invert(&rcol::srgb_to_xyz_d50()).unwrap();
    let pro_to_srgb = rcol::mul(&to_srgb, &rcol::rgb_to_xyz(rcol::ROMM, rcol::D50_XY));
    let f = |x: f64| 1.6 * x / (1.0 + 0.6 * x);
    let enc = |v: f64| {
        let v = v.clamp(0.0, 1.0);
        let e = if v <= 0.0031308 { 12.92 * v } else { 1.055 * v.powf(1.0 / 2.4) - 0.055 };
        (e * 255.0).round() as u8
    };
    let rgb = shot_cam()
        .iter()
        .flat_map(|p| {
            let bal = [0, 1, 2].map(|k| f64::from(p[k]) * WB[k]);
            let pro = rcol::apply(&m, bal).map(|v| f(v.clamp(0.0, 1.0)));
            rcol::apply(&pro_to_srgb, pro).map(enc)
        })
        .collect();
    (rgb, SHOT_W, SHOT_H)
}

fn print_look(name: &str, r: Result<CameraLook, LookRejected>) -> Option<CameraLook> {
    match r {
        Ok(l) => {
            println!("look {name} {:016x}", look_hash(&l));
            Some(l)
        }
        Err(e) => {
            let mut m = e.to_string();
            while m.len() > 255 {
                m.pop();
            }
            println!("look {name} err {m}");
            None
        }
    }
}

fn exercise2(h: &mut u64, b: &[u8]) {
    let limits = Limits { max_alloc: 64 << 20, ..Limits::default() };
    fnv(h, &[identify(b).map(|f| f as u8).unwrap_or(0xFF)]);
    let ps = embedded_previews(b);
    u64le(h, ps.len() as u64);
    for p in &ps {
        u64le(h, p.width as u64);
        u64le(h, p.height as u64);
        u64le(h, p.jpeg.len() as u64);
    }
    fnv(h, &[embedded_preview_color_space(b).map(|c| c as u8).unwrap_or(0xEE)]);
    match decode(b, &limits) {
        Ok(sn) => {
            u64le(h, sensor_hash2(&sn));
            let opts = DevelopOptions { demosaic: Demosaic::Bilinear, limits: limits.clone(), ..Default::default() };
            match develop_sensor(&sn, &opts) {
                Ok(d) => u64le(h, developed_hash(&d)),
                Err(e) => crate::raw_vectors::s(h, &err_msg(&e)),
            }
            let (w, hh) = (sn.crop.width.min(256), sn.crop.height.min(256));
            let rgb: Vec<u8> = (0..w * hh * 3).map(|i| (i * 7 % 251) as u8).collect();
            match fit_look(&sn, &opts, &Reference { width: w as u32, height: hh as u32, rgb: &rgb }) {
                Ok(l) => u64le(h, look_hash(&l)),
                Err(e) => {
                    // The C error text is capped at 255 bytes.
                    let mut m = e.to_string();
                    while m.len() > 255 {
                        m.pop();
                    }
                    crate::raw_vectors::s(h, &m)
                }
            }
        }
        Err(e) => crate::raw_vectors::s(h, &err_msg(&e)),
    }
}

pub fn raw_vectors_r3() {
    println!("section r3");
    let all = samples_r3();
    for (name, b, kind) in &all {
        println!("file {name} {} {:016x} {}", b.len(), file_hash(b), kind.letter());
        if *kind == Kind::L {
            continue;
        }
        match decode(b, &Limits::default()) {
            Ok(sn) => {
                println!("decode {name} {:016x}", sensor_hash2(&sn));
                for m in [Demosaic::Bilinear, Demosaic::Mhc, Demosaic::Ahd] {
                    match develop_sensor(&sn, &DevelopOptions { demosaic: m, ..Default::default() }) {
                        Ok(d) => println!("develop {name} {} {:016x}", m.id(), developed_hash(&d)),
                        Err(e) => println!("develop {name} {} err {}", m.id(), err_msg(&e)),
                    }
                }
            }
            Err(e) => println!("decode {name} err {}", err_msg(&e)),
        }
    }
    let get = |n: &str| decode(&all.iter().find(|(m, _, _)| m == n).unwrap().1, &Limits::default()).unwrap();
    // White balance on monochrome levels.
    let sn = get("mono_levels");
    for (oname, o) in [
        ("auto", DevelopOptions { white_balance: WhiteBalance::Auto, ..Default::default() }),
        ("mult", DevelopOptions { white_balance: WhiteBalance::Multipliers([2.0, 1.0, 1.5]), ..Default::default() }),
        ("exposure", DevelopOptions { exposure: -1.0, ..Default::default() }),
    ] {
        match develop_sensor(&sn, &o) {
            Ok(d) => println!("option mono_levels {oname} {:016x}", developed_hash(&d)),
            Err(e) => println!("option mono_levels {oname} err {}", err_msg(&e)),
        }
    }
    // The tone sliders.
    for name in ["big", "h_raf_u16", "mono_strips16", "raf_meta", "h_mono_lj92"] {
        let sn = get(name);
        for (oname, tone) in tone_opts() {
            let o = DevelopOptions { demosaic: Demosaic::Bilinear, tone, ..Default::default() };
            match develop_sensor(&sn, &o) {
                Ok(d) => println!("tone {name} {oname} {:016x}", developed_hash(&d)),
                Err(e) => println!("tone {name} {oname} err {}", err_msg(&e)),
            }
        }
    }
    // The black point (a profile's factor) with and without the sliders.
    let mut sn = get("big");
    sn.black_point = 0.33;
    sn.tone_curve = vec![[0.01, 0.02], [0.1, 0.25], [1.0, 1.0]];
    for (oname, tone) in [("none", Tone::NEUTRAL), ("sh+100", Tone { shadows: 100.0, ..Tone::NEUTRAL }), ("dh+100", Tone { dehaze: 100.0, ..Tone::NEUTRAL })] {
        let o = DevelopOptions { demosaic: Demosaic::Bilinear, tone, ..Default::default() };
        match develop_sensor(&sn, &o) {
            Ok(d) => println!("blackpoint {oname} {:016x}", developed_hash(&d)),
            Err(e) => println!("blackpoint {oname} err {}", err_msg(&e)),
        }
    }
    // retone on tone.rs's test bands.
    let (w, h) = (300usize, 120usize);
    let curve = [[0.01f32, 0.02], [0.1, 0.25], [1.0, 1.0]];
    let y: Vec<f32> = (0..w * h).map(|i| [2f32.powi(-7), 2f32.powi(-4), 0.5][(i % w) * 3 / w]).collect();
    // The develop curve (log2 interpolation), as tone.rs's test encodes the image.
    let tonef = |v: f32| -> f32 {
        let (first, last) = (curve[0], curve[curve.len() - 1]);
        if v <= first[0] {
            return v * first[1] / first[0];
        }
        if v >= last[0] {
            return last[1];
        }
        let i = curve.partition_point(|p| p[0] <= v).clamp(1, curve.len() - 1);
        let ([x0, y0], [x1, y1]) = (curve[i - 1], curve[i]);
        let f = (v.log2() - x0.log2()) / (x1.log2() - x0.log2());
        y0 + (y1 - y0) * f
    };
    let enc = |v: f32| tonef(v).powf(1.0 / 1.8);
    let px: Vec<[f32; 4]> = y.iter().enumerate().map(|(i, v)| [enc(*v), enc(*v * 0.9), enc(*v * (1.0 + (i % 7) as f32 * 0.01)), 1.0]).collect();
    for (oname, tone) in std::iter::once(("neutral", Tone::NEUTRAL)).chain(tone_opts()) {
        let mut out = px.clone();
        retone(&mut out, w, h, &curve, tone);
        let mut hh = H0;
        for p in &out {
            for v in p {
                f32b(&mut hh, *v);
            }
        }
        println!("retone {oname} {hh:016x}");
    }
    // Camera looks (look.rs's tests).
    let sn = decode(&shot_raw(), &Limits::default()).unwrap();
    let (rgb, rw, rh) = shot_reference();
    let wb = DevelopOptions { white_balance: WhiteBalance::Multipliers(WB), ..Default::default() };
    let r = |w: usize, h: usize, rgb: &[u8]| fit_look(&sn, &wb, &Reference { width: w as u32, height: h as u32, rgb });
    if let Some(look) = print_look("shot", r(rw, rh, &rgb)) {
        match develop_sensor(&sn, &DevelopOptions { camera_look: Some(look), demosaic: Demosaic::Bilinear, ..wb.clone() }) {
            Ok(d) => println!("lookdev shot {:016x}", developed_hash(&d)),
            Err(e) => println!("lookdev shot err {}", err_msg(&e)),
        }
    }
    let mut so = sn.clone();
    so.orientation = 6;
    let mut up = vec![0u8; rgb.len()];
    for y2 in 0..rw {
        for x2 in 0..rh {
            // source_xy(6, w, h, x2, y2) = (y2, h - 1 - x2)
            let (x, y) = (y2, rh - 1 - x2);
            let (o, i) = ((y2 * rh + x2) * 3, (y * rw + x) * 3);
            up[o..o + 3].copy_from_slice(&rgb[i..i + 3]);
        }
    }
    print_look("shot_o6", fit_look(&so, &wb, &Reference { width: rh as u32, height: rw as u32, rgb: &up }));
    print_look("shot_o6_unrotated", fit_look(&so, &wb, &Reference { width: rw as u32, height: rh as u32, rgb: &rgb }));
    let mut seed = 7u32;
    let noise_rgb: Vec<u8> = (0..rgb.len())
        .map(|_| {
            seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
            (seed >> 16) as u8
        })
        .collect();
    print_look("noise", r(rw, rh, &noise_rgb));
    print_look("grey", r(rw, rh, &vec![118u8; rgb.len()]));
    print_look("square", r(320, 320, &rgb[..320 * 320 * 3]));
    print_look("short", r(rw, rh, &rgb[..rgb.len() - 1]));
    print_look("tiny", r(12, 8, &rgb[..12 * 8 * 3]));
    print_look("empty", r(0, 0, &[]));
    print_look("huge", fit_look(&sn, &wb, &Reference { width: u32::MAX, height: u32::MAX, rgb: &rgb }));
    let mut bad = sn.clone();
    bad.crop.width += 10;
    print_look("badcrop", fit_look(&bad, &wb, &Reference { width: rw as u32, height: rh as u32, rgb: &rgb }));
    print_look("badwb", fit_look(&sn, &DevelopOptions { white_balance: WhiteBalance::Multipliers([0.0, 1.0, 1.0]), ..Default::default() }, &Reference { width: rw as u32, height: rh as u32, rgb: &rgb }));
    for (i, to_xyz) in [[[f64::NAN; 3]; 3], [[0.0; 3]; 3], [[1e9, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]], [[0.4, 0.35, 0.18], [0.21, 0.72, 0.07], [0.02, 0.12, 0.8]]].into_iter().enumerate() {
        let look = CameraLook { to_xyz, tone: None, delta_e: 1.0, delta_e_fallback: 2.0 };
        match develop_sensor(&sn, &DevelopOptions { camera_look: Some(look), demosaic: Demosaic::Bilinear, ..wb.clone() }) {
            Ok(d) => println!("lookdev manual{i} {:016x}", developed_hash(&d)),
            Err(e) => println!("lookdev manual{i} err {}", err_msg(&e)),
        }
    }
    // Embedded previews and their colour space.
    for (name, b, _) in &all {
        let ps = embedded_previews(b);
        let mut line = format!("previews {name} {}", ps.len());
        for p in &ps {
            line += &format!(" {}x{}:{}", p.width, p.height, p.jpeg.len());
        }
        println!("{line}");
    }
    for (i, (cs, index, dng)) in [
        (Some(1u16), Some("R98"), false),
        (Some(0xFFFF), Some("R03"), false),
        (Some(0xFFFF), Some("R98"), false),
        (Some(0xFFFF), None, false),
        (Some(2), Some("R03"), false),
        (None, Some("R03"), false),
        (Some(0xFFFF), Some("R03"), true),
    ]
    .into_iter()
    .enumerate()
    {
        let mut t = TiffBuilder::default();
        let mut exif = Vec::new();
        if let Some(cs) = cs {
            exif.push((40961, Val::Short(vec![cs])));
        }
        if let Some(index) = index {
            let interop = t.ifd(vec![(1, Val::Ascii(index.into()))]);
            exif.push((40965, Val::Ifds(vec![interop])));
        }
        let exif = t.ifd(exif);
        let mut ifd0 = vec![(271, Val::Ascii("SONY".into())), (34665, Val::Ifds(vec![exif]))];
        if dng {
            ifd0.push((50706, Val::Byte(vec![1, 6, 0, 0])));
        }
        t.chain = vec![t.ifd(ifd0)];
        let b = t.build();
        println!("cspace {i} {}", embedded_preview_color_space(&b).map(|c| format!("{c:?}")).unwrap_or_else(|| "none".into()));
    }
    // Truncation and mutation sweeps over the new hostile samples built in C.
    for (name, b, kind) in hostile_r3() {
        if kind != Kind::E {
            continue;
        }
        let mut h = H0;
        for n in (0..b.len()).step_by(3) {
            exercise2(&mut h, &b[..n]);
        }
        println!("truncate {name} {h:016x}");
        let mut h = H0;
        let mut st = 0xDEAD_BEEF_u64 ^ name.len() as u64;
        let mut next = || {
            st = st.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            st >> 33
        };
        for _ in 0..150 {
            let mut c = b.clone();
            for _ in 0..1 + next() % 6 {
                let at = next() as usize % c.len();
                c[at] = match next() % 4 {
                    0 => 0xFF,
                    1 => 0,
                    _ => next() as u8,
                };
            }
            exercise2(&mut h, &c);
        }
        println!("mutate {name} {h:016x}");
    }
}

/// `rustref raw-pixls-vectors DIR`: the pixls corpus (corpus/pixls): decode, previews, colour space and
/// a developed 512 x 512 centre crop.
pub fn raw_pixls_vectors(dir: &str) {
    let mut names: Vec<String> = std::fs::read_dir(dir).unwrap().filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| !n.ends_with(".md")).collect();
    names.sort();
    for name in names {
        let b = std::fs::read(format!("{dir}/{name}")).unwrap();
        println!("pfile {name} {} {:016x}", b.len(), file_hash(&b));
        let ps = embedded_previews(&b);
        let mut line = format!("ppreviews {name} {}", ps.len());
        for p in &ps {
            line += &format!(" {}x{}:{}", p.width, p.height, p.jpeg.len());
        }
        println!("{line}");
        println!("pcspace {name} {}", embedded_preview_color_space(&b).map(|c| format!("{c:?}")).unwrap_or_else(|| "none".into()));
        match decode(&b, &Limits::default()) {
            Ok(mut sn) => {
                println!("pdecode {name} {:016x}", sensor_hash2(&sn));
                let c = sn.crop;
                let (cw, ch) = (c.width.min(512) & !1, c.height.min(512) & !1);
                sn.crop = Rect::new(c.x + ((c.width - cw) / 2 & !1), c.y + ((c.height - ch) / 2 & !1), cw, ch);
                for m in [Demosaic::Bilinear, Demosaic::Ahd] {
                    match develop_sensor(&sn, &DevelopOptions { demosaic: m, ..Default::default() }) {
                        Ok(d) => println!("pdevelop {name} {} {:016x}", m.id(), developed_hash(&d)),
                        Err(e) => println!("pdevelop {name} {} err {}", m.id(), err_msg(&e)),
                    }
                }
            }
            Err(e) => println!("pdecode {name} err {}", err_msg(&e)),
        }
    }
}
