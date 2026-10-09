//! `rustref raw-vectors`: synthetic camera raws built by photocraft-raw's testgen, decoded and
//! developed; every line is checked by tests/raw/test_parity.c against the C port.
use photocraft_raw::testgen::*;
use photocraft_raw::*;

const H0: u64 = 0xcbf29ce484222325;
fn fnv(h: &mut u64, b: &[u8]) {
    for &x in b {
        *h ^= x as u64;
        *h = h.wrapping_mul(0x100000001b3);
    }
}
fn u64le(h: &mut u64, v: u64) { fnv(h, &v.to_le_bytes()); }
fn f64b(h: &mut u64, v: f64) { fnv(h, &v.to_bits().to_le_bytes()); }
fn f32b(h: &mut u64, v: f32) { fnv(h, &v.to_bits().to_le_bytes()); }
fn s(h: &mut u64, v: &str) { fnv(h, v.as_bytes()); fnv(h, &[0]); }
fn opt_s(h: &mut u64, v: &Option<String>) { s(h, v.as_deref().unwrap_or("-")); }
fn mat(h: &mut u64, m: &Mat3) { for r in m { for v in r { f64b(h, *v); } } }

fn err_msg(e: &RawError) -> String {
    let mut m = e.to_string();
    // The C error text is capped at 255 bytes.
    while m.len() > 255 { m.pop(); }
    m
}

pub fn sensor_hash(x: &Sensor) -> u64 {
    let mut h = H0;
    s(&mut h, x.format.name());
    opt_s(&mut h, &x.make);
    opt_s(&mut h, &x.model);
    for v in [x.width, x.height, x.samples] { u64le(&mut h, v as u64); }
    for v in &x.data { fnv(&mut h, &v.to_le_bytes()); }
    match &x.cfa {
        Some(c) => {
            u64le(&mut h, c.width as u64);
            u64le(&mut h, c.height as u64);
            fnv(&mut h, &c.colors);
            u64le(&mut h, c.origin_x as u64);
            u64le(&mut h, c.origin_y as u64);
        }
        None => fnv(&mut h, &[0xFF]),
    }
    match &x.linearization {
        Some(l) => { u64le(&mut h, l.len() as u64); for v in l { fnv(&mut h, &v.to_le_bytes()); } }
        None => fnv(&mut h, &[0xFF]),
    }
    u64le(&mut h, x.black.rows as u64);
    u64le(&mut h, x.black.cols as u64);
    for list in [&x.black.values, &x.black.delta_h, &x.black.delta_v] {
        u64le(&mut h, list.len() as u64);
        for v in list.iter() { f32b(&mut h, *v); }
    }
    for v in x.white { f32b(&mut h, v); }
    for r in [x.active, x.crop] { for v in [r.x, r.y, r.width, r.height] { u64le(&mut h, v as u64); } }
    u64le(&mut h, x.color.calibrations.len() as u64);
    for c in &x.color.calibrations {
        f64b(&mut h, c.temperature);
        mat(&mut h, &c.color_matrix);
        match &c.forward_matrix { Some(m) => { fnv(&mut h, &[1]); mat(&mut h, m); } None => fnv(&mut h, &[0]) }
        mat(&mut h, &c.camera_calibration);
    }
    for o in [x.color.analog_balance, x.color.as_shot_neutral, x.camera_wb] {
        match o { Some(v) => { fnv(&mut h, &[1]); for f in v { f64b(&mut h, f); } } None => fnv(&mut h, &[0]) }
    }
    match x.color.as_shot_white_xy { Some(v) => { fnv(&mut h, &[1]); for f in v { f64b(&mut h, f); } } None => fnv(&mut h, &[0]) }
    fnv(&mut h, &x.orientation.to_le_bytes());
    f64b(&mut h, x.baseline_exposure);
    u64le(&mut h, x.gain_maps.len() as u64);
    for g in &x.gain_maps {
        for v in [g.top, g.left, g.bottom, g.right, g.plane, g.planes, g.row_pitch, g.col_pitch, g.points_v, g.points_h, g.map_planes] {
            u64le(&mut h, v as u64);
        }
        for v in [g.spacing_v, g.spacing_h, g.origin_v, g.origin_h] { f64b(&mut h, v); }
        u64le(&mut h, g.gains.len() as u64);
        for v in &g.gains { f32b(&mut h, *v); }
    }
    u64le(&mut h, x.tone_curve.len() as u64);
    for p in &x.tone_curve { f32b(&mut h, p[0]); f32b(&mut h, p[1]); }
    for w in &x.warnings { s(&mut h, w); }
    h
}

pub fn developed_hash(d: &Developed) -> u64 {
    let mut h = H0;
    fnv(&mut h, &d.width.to_le_bytes());
    fnv(&mut h, &d.height.to_le_bytes());
    for v in &d.rgb { fnv(&mut h, &v.to_le_bytes()); }
    s(&mut h, d.info.format.name());
    opt_s(&mut h, &d.info.make);
    opt_s(&mut h, &d.info.model);
    u64le(&mut h, d.info.sensor_width as u64);
    u64le(&mut h, d.info.sensor_height as u64);
    opt_s(&mut h, &d.info.cfa);
    for v in d.info.wb_multipliers { f64b(&mut h, v); }
    fnv(&mut h, &d.info.orientation.to_le_bytes());
    f64b(&mut h, d.info.baseline_exposure);
    for w in &d.warnings { s(&mut h, w); }
    h
}

/// Deterministic sensor-like samples below 2^bits (tests/dng.rs).
fn noise(n: usize, bits: u16) -> Vec<u16> {
    let mut s = 0x9E37_79B9_7F4A_7C15u64;
    let max = (1u32 << bits) - 1;
    (0..n)
        .map(|i| {
            s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            ((i as u32 * 13 + (s >> 40) as u32 % 200) % (max + 1)) as u16
        })
        .collect()
}

const SRGB_TO_XYZ: [[f64; 3]; 3] = [[0.4124564, 0.3575761, 0.1804375], [0.2126729, 0.7151522, 0.0721750], [0.0193339, 0.1191920, 0.9503041]];
fn inv3(m: [[f64; 3]; 3]) -> [[f64; 3]; 3] {
    let [[a, b, c], [d, e, f], [g, h, i]] = m;
    let det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    [
        [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
        [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
        [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det],
    ]
}
fn flat(m: [[f64; 3]; 3]) -> [f64; 9] { [m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]] }

fn cr2_spec(w: usize, h: usize, comps: usize, slices: Vec<usize>) -> Cr2Spec {
    let (l, t) = (16, 4);
    let rgb = scene(w - l, h - t);
    let img = mosaic(&rgb, w - l, [0, 1, 1, 2], 512, 13000);
    let mut data = vec![512u16; w * h];
    for y in t..h { for x in l..w { data[y * w + x] = img[(y - t) * (w - l) + (x - l)]; } }
    Cr2Spec { width: w, height: h, data, precision: 14, components: comps, slices, borders: Some([l as u16, t as u16, w as u16 - 1, h as u16 - 1]), wb_rggb: Some([2048, 1024, 1024, 1536]), orientation: 1, model_id: None }
}

fn cr2_phase_spec(cfa: [u8; 4], l: usize, t: usize, model_id: Option<u32>) -> Cr2Spec {
    let (w, h) = (192usize, 96usize);
    let wb = [2.0f32, 1.0, 1.5];
    let colourful = scene(w, h);
    let rgb: Vec<[f32; 3]> = (0..w * h)
        .map(|i| { let p = if i % w >= w / 2 { [0.4; 3] } else { colourful[i] }; [p[0] / wb[0], p[1] / wb[1], p[2] / wb[2]] })
        .collect();
    let mut data = mosaic(&rgb, w, cfa, 512, 13000);
    for y in 0..h { for x in 0..w { if x < l || y < t { data[y * w + x] = 512; } } }
    Cr2Spec { width: w, height: h, data, precision: 14, components: 2, slices: vec![96, 96], borders: Some([l as u16, t as u16, w as u16 - 1, h as u16 - 1]), wb_rggb: Some([2048, 1024, 1024, 1536]), orientation: 1, model_id }
}

/// tests/hostile.rs samples().
pub fn hostile() -> Vec<Vec<u8>> {
    let (w, h) = (24, 12);
    let cfa = mosaic(&scene(w, h), w, [0, 1, 1, 2], 64, 4000);
    let mut out = Vec::new();
    let mut d = DngSpec::cfa(w, h, cfa.clone());
    d.bits = 12;
    d.white = 4000;
    d.black = vec![64];
    d.as_shot_neutral = Some([0.5, 1.0, 0.7]);
    d.color_matrix1 = Some((21, [3.2, -1.5, -0.5, -1.0, 1.9, 0.04, 0.06, -0.2, 1.06]));
    out.push(d.build());
    d.storage = DngStorage::Lj92Tiles { width: 16, height: 8 };
    d.active_area = Some([1, 2, 11, 22]);
    d.default_crop = Some(([1, 1], [16, 8]));
    out.push(d.build());
    d.storage = DngStorage::Strips { rows: 5 };
    d.big_endian = true;
    out.push(d.build());
    let mut data = vec![600u16; 40 * 10];
    for (i, v) in data.iter_mut().enumerate() { *v = 600 + (i as u16 * 7) % 3000; }
    out.push(Cr2Spec { width: 40, height: 10, data, precision: 14, components: 2, slices: vec![16, 16, 8], borders: Some([8, 2, 39, 9]), wb_rggb: Some([2000, 1024, 1024, 1500]), orientation: 8, model_id: None }.build());
    out.push(tiff_ep("NIKON", w, h, &cfa, [0, 1, 1, 2], 12, vec![]));
    let codes = mosaic(&scene(64, 6), 64, [0, 1, 1, 2], 256, 1900);
    out.push(sony_craw(64, 6, &codes, [8000, 10400, 12900, 14100]));
    out.push(rw2(30, 8, &mosaic(&scene(30, 8), 30, [0, 1, 1, 2], 128, 4095), 12));
    out.push(orf(w, h, &cfa));
    out
}

pub fn samples() -> Vec<(String, Vec<u8>)> {
    let mut v: Vec<(String, Vec<u8>)> = Vec::new();
    for (i, b) in hostile().into_iter().enumerate() { v.push((format!("hostile{i}"), b)); }
    let (w, h) = (37, 23);
    let cases: Vec<(&str, u16, DngStorage, bool)> = vec![
        ("dng16", 16, DngStorage::Strips { rows: 5 }, false),
        ("dng16be", 16, DngStorage::Strips { rows: 23 }, true),
        ("dng12", 12, DngStorage::Strips { rows: 4 }, false),
        ("dng10be", 10, DngStorage::Strips { rows: 7 }, true),
        ("dng8", 8, DngStorage::Strips { rows: 9 }, false),
        ("dngtiles14", 14, DngStorage::Lj92Tiles { width: 16, height: 8 }, false),
        ("dngtiles16be", 16, DngStorage::Lj92Tiles { width: 32, height: 32 }, true),
        ("dnglj12", 12, DngStorage::Lj92Strips { rows: 6 }, false),
    ];
    for (name, bits, storage, be) in cases {
        let mut spec = DngSpec::cfa(w, h, noise(w * h, bits));
        spec.bits = bits;
        spec.storage = storage;
        spec.big_endian = be;
        spec.white = (1 << bits) - 1;
        v.push((name.into(), spec.build()));
    }
    let mut spec = DngSpec::cfa(20, 10, noise(600, 16));
    spec.samples = 3;
    spec.storage = DngStorage::Lj92Tiles { width: 16, height: 16 };
    v.push(("linear".into(), spec.build()));
    let mut spec = DngSpec::cfa(40, 30, vec![1000; 1200]);
    spec.bits = 14;
    spec.white = 15000;
    spec.black = vec![100, 101, 102, 103];
    spec.black_repeat = (2, 2);
    spec.active_area = Some([2, 4, 28, 36]);
    spec.default_crop = Some(([2, 1], [20, 16]));
    spec.cfa = [1, 0, 2, 1];
    spec.as_shot_neutral = Some([0.5, 1.0, 0.8]);
    spec.baseline_exposure = Some(0.5);
    spec.orientation = 6;
    spec.linearization = Some((0..4096u32).map(|v| (v * 3).min(65535) as u16).collect());
    v.push(("meta".into(), spec.build()));
    // colour matrix development
    let (cw, ch) = (48, 32);
    let gains = [0.5f32, 1.0, 0.7];
    let patches: [[f32; 3]; 4] = [[0.18, 0.18, 0.18], [0.40, 0.10, 0.05], [0.05, 0.30, 0.10], [0.10, 0.12, 0.45]];
    let rgb: Vec<[f32; 3]> = (0..cw * ch)
        .map(|i| { let p = patches[((i % cw) / (cw / 2)) + 2 * ((i / cw) / (ch / 2))]; [p[0] * gains[0], p[1] * gains[1], p[2] * gains[2]] })
        .collect();
    let mut spec = DngSpec::cfa(cw, ch, mosaic(&rgb, cw, [0, 1, 1, 2], 256, 16383));
    spec.bits = 14;
    spec.black = vec![256];
    spec.white = 16383;
    spec.color_matrix1 = Some((21, flat(inv3(SRGB_TO_XYZ))));
    spec.as_shot_neutral = Some(gains.map(f64::from));
    v.push(("cm_strips".into(), { let mut s2 = spec.clone(); s2.storage = DngStorage::Strips { rows: 32 }; s2.build() }));
    v.push(("cm_tiles".into(), { let mut s2 = spec.clone(); s2.storage = DngStorage::Lj92Tiles { width: 16, height: 16 }; s2.build() }));
    // forward matrix
    let rgb = vec![[0.3f32, 0.6, 0.45]; 256];
    let mut spec = DngSpec::cfa(16, 16, mosaic(&rgb, 16, [0, 1, 1, 2], 0, 65535));
    spec.color_matrix1 = Some((17, flat(inv3(SRGB_TO_XYZ))));
    spec.color_matrix2 = Some((21, flat(inv3(SRGB_TO_XYZ))));
    let fm = [[0.4360747, 0.3850649, 0.1430804], [0.2225045, 0.7168786, 0.0606169], [0.0139322, 0.0971045, 0.7141733]];
    spec.forward_matrix1 = Some(flat(fm));
    spec.forward_matrix2 = Some(flat(fm));
    spec.as_shot_neutral = Some([0.5, 1.0, 0.75]);
    v.push(("forward".into(), spec.build()));
    // gain map + a skipped opcode
    let mut spec = DngSpec::cfa(16, 12, vec![16384; 192]);
    spec.as_shot_neutral = Some([1.0; 3]);
    spec.opcode_list2 = Some(gain_map_opcode_list([0, 0, 12, 16], 1, [[2.0, 1.5], [1.25, 0.5]]));
    v.push(("gainmap".into(), spec.build()));
    // grey world
    let rgb = vec![[0.2f32, 0.4, 0.3]; 256];
    v.push(("greyworld".into(), DngSpec::cfa(16, 16, mosaic(&rgb, 16, [0, 1, 1, 2], 0, 65535)).build()));
    // smooth scene, BGGR
    let mut spec = DngSpec::cfa(64, 48, mosaic(&scene(64, 48), 64, [2, 1, 1, 0], 0, 65535));
    spec.cfa = [2, 1, 1, 0];
    spec.as_shot_neutral = Some([1.0; 3]);
    v.push(("scene".into(), spec.build()));
    // clipped highlights
    let mut spec = DngSpec::cfa(8, 8, vec![65535; 64]);
    spec.as_shot_neutral = Some([0.5, 1.0, 0.6]);
    v.push(("clipped".into(), spec.build()));
    // CR2
    for (name, comps, slices) in [("cr2_a", 2, vec![32, 32, 24]), ("cr2_b", 4, vec![40, 48]), ("cr2_c", 2, vec![]), ("cr2_d", 4, vec![88])] {
        v.push((name.into(), cr2_spec(88, 30, comps, slices).build()));
    }
    for cfa in [[0u8, 1, 1, 2], [1, 2, 0, 1]] {
        for (l, t) in [(16, 4), (17, 5)] {
            v.push((format!("cr2phase_{}{}{}{}_{l}_{t}", cfa[0], cfa[1], cfa[2], cfa[3]), cr2_phase_spec(cfa, l, t, None).build()));
        }
    }
    for id in [Some(0x8000_0250u32), Some(0x8000_0001), None] {
        let mut spec = cr2_phase_spec([0, 1, 1, 2], 16, 4, id);
        spec.data.iter_mut().for_each(|v| *v = 512);
        v.push((format!("cr2flat_{:x}", id.unwrap_or(0)), spec.build()));
    }
    // TIFF/EP
    let data = mosaic(&scene(24, 16), 24, [2, 1, 1, 0], 0, 4095);
    v.push(("nef_like".into(), tiff_ep("NIKON CORPORATION", 24, 16, &data, [2, 1, 1, 0], 12, vec![])));
    let data = mosaic(&scene(24, 16), 24, [0, 1, 1, 2], 512, 16383);
    let extra = vec![
        (0x7310, Val::Short(vec![512, 512, 512, 512])),
        (0x7313, Val::Short(vec![2048, 1024, 1024, 1536])),
        (50717, Val::Short(vec![16383])),
        (50719, Val::Long(vec![2, 2])),
        (50720, Val::Long(vec![20, 12])),
    ];
    v.push(("arw_like".into(), tiff_ep("SONY", 24, 16, &data, [0, 1, 1, 2], 14, extra)));
    // vendor codecs
    let codes = mosaic(&scene(640, 8), 640, [0, 1, 1, 2], 256, 1900);
    v.push(("craw".into(), sony_craw(640, 8, &codes, [8000, 10400, 12900, 14100])));
    let mut s = 12345u32;
    let codes: Vec<u16> = (0..64 * 8).map(|_| { s = s.wrapping_mul(1103515245).wrapping_add(12345); ((s >> 16) % 2000) as u16 }).collect();
    v.push(("craw_wide".into(), sony_craw(64, 8, &codes, [8000, 10400, 12900, 14100])));
    v.push(("rw2_12".into(), rw2(120, 300, &mosaic(&scene(120, 300), 120, [0, 1, 1, 2], 128, 4095), 12)));
    v.push(("rw2_14".into(), rw2(117, 260, &mosaic(&scene(117, 260), 117, [0, 1, 1, 2], 128, 16383), 14)));
    v.push(("orf".into(), orf(40, 24, &mosaic(&scene(40, 24), 40, [1, 0, 2, 1], 64, 4095))));
    v
}

fn file_hash(b: &[u8]) -> u64 { let mut h = H0; fnv(&mut h, b); h }

/// One decode (and, on success, a bilinear develop at 64 MiB) folded into h.
fn exercise(h: &mut u64, b: &[u8]) {
    let limits = Limits { max_alloc: 64 << 20, ..Limits::default() };
    fnv(h, &[identify(b).map(|f| f as u8).unwrap_or(0xFF)]);
    match embedded_preview(b) { Some(p) => { u64le(h, p.width as u64); u64le(h, p.height as u64); u64le(h, p.jpeg.len() as u64); } None => fnv(h, &[0xEE]) }
    match decode(b, &limits) {
        Ok(sn) => {
            u64le(h, sensor_hash(&sn));
            match develop_sensor(&sn, &DevelopOptions { demosaic: Demosaic::Bilinear, limits, ..Default::default() }) {
                Ok(d) => u64le(h, developed_hash(&d)),
                Err(e) => s(h, &err_msg(&e)),
            }
        }
        Err(e) => s(h, &err_msg(&e)),
    }
}

pub fn raw_vectors() {
    let all = samples();
    for (name, b) in &all {
        println!("file {name} {} {:016x}", b.len(), file_hash(b));
        match decode(b, &Limits::default()) {
            Ok(sn) => {
                println!("decode {name} {:016x}", sensor_hash(&sn));
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
    // Development options on a few files.
    for name in ["hostile0", "meta", "greyworld", "linear", "gainmap"] {
        let b = &all.iter().find(|(n, _)| n == name).unwrap().1;
        let sn = decode(b, &Limits::default()).unwrap();
        let opts: [(&str, DevelopOptions); 5] = [
            ("auto", DevelopOptions { white_balance: WhiteBalance::Auto, ..Default::default() }),
            ("mult", DevelopOptions { white_balance: WhiteBalance::Multipliers([2.0, 1.0, 4.0 / 3.0]), ..Default::default() }),
            ("badmult", DevelopOptions { white_balance: WhiteBalance::Multipliers([0.0, 1.0, 1.0]), ..Default::default() }),
            ("exposure", DevelopOptions { exposure: 1.0, ..Default::default() }),
            ("noorient", DevelopOptions { orient: false, exposure: -0.7, ..Default::default() }),
        ];
        for (oname, o) in opts {
            match develop_sensor(&sn, &o) {
                Ok(d) => println!("option {name} {oname} {:016x}", developed_hash(&d)),
                Err(e) => println!("option {name} {oname} err {}", err_msg(&e)),
            }
        }
    }
    // Truncation and mutation sweeps over the hostile samples.
    for (i, b) in hostile().iter().enumerate() {
        let mut h = H0;
        for n in (0..b.len()).step_by(3) { exercise(&mut h, &b[..n]); }
        println!("truncate hostile{i} {:016x}", h);
        let mut h = H0;
        let mut st = 0xDEAD_BEEF_u64 ^ i as u64;
        let mut next = || { st = st.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); st >> 33 };
        for _ in 0..150 {
            let mut c = b.clone();
            for _ in 0..1 + next() % 6 {
                let at = next() as usize % c.len();
                c[at] = match next() % 4 { 0 => 0xFF, 1 => 0, _ => next() as u8 };
            }
            exercise(&mut h, &c);
        }
        println!("mutate hostile{i} {:016x}", h);
    }
}
