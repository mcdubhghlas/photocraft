//! TEMPORARY Rust reference dumper for the C17 rewrite (REWRITE_PLAN.md B4). Deleted in Z3.
//!
//!   rustref vectors                 serde_json / half / base64 vectors (tests/base/*)
//!   rustref geom-vectors            FNV hashes of warp/mesh/affine outputs (tests/geom/test_warp.c)
//!   rustref cms-vectors             FNV hashes of colour-management outputs (tests/cms/test_parity.c)
//!   rustref icc-info FILE           one line describing a parsed ICC profile (tests/cms/test_parity.c)
//!   rustref psd-info FILE           PSD/PSB structure as JSON (the F6-F9 parity contract)
//!   rustref decode FILE OUT.raw     codecs::decode: JSON metadata on stdout, raw samples to OUT.raw
//!   rustref composite FILE OUT.raw  io::import + compose::flatten: JSON on stdout, RGBA f32 LE to OUT.raw
//!
//! JSON objects are serde_json::Value (keys sorted). Failures print {"error": "..."} and exit 1,
//! so differential tests can compare errors too.
use base64::Engine;
use half::f16;
fn fnv(h: &mut u64, b: &[u8]) { for &x in b { *h ^= x as u64; *h = h.wrapping_mul(0x100000001b3); } }
fn lcg(s: &mut u64) -> u64 { *s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); *s }
fn vectors() {
    // f16 -> f32 / f64, all 65536 patterns
    let (mut a, mut b) = (0xcbf29ce484222325u64, 0xcbf29ce484222325u64);
    for i in 0..=0xFFFFu16 { let h = f16::from_bits(i); fnv(&mut a, &h.to_f32().to_bits().to_le_bytes()); fnv(&mut b, &h.to_f64().to_bits().to_le_bytes()); }
    println!("f16_to_f32 {:016x}\nf16_to_f64 {:016x}", a, b);
    // f32/f64 -> f16 over 2M LCG patterns
    let (mut c, mut d, mut s) = (0xcbf29ce484222325u64, 0xcbf29ce484222325u64, 1u64);
    for _ in 0..2_000_000 { let r = lcg(&mut s); fnv(&mut c, &f16::from_f32(f32::from_bits((r >> 32) as u32)).to_bits().to_le_bytes()); fnv(&mut d, &f16::from_f64(f64::from_bits(r)).to_bits().to_le_bytes()); }
    println!("f32_to_f16 {:016x}\nf64_to_f16 {:016x}", c, d);
    // JSON number formatting over 200k LCG doubles/floats (finite only)
    let (mut e, mut f, mut s2) = (0xcbf29ce484222325u64, 0xcbf29ce484222325u64, 7u64);
    let mut n = 0;
    while n < 200_000 { let r = lcg(&mut s2); let x = f64::from_bits(r); let y = f32::from_bits((r >> 32) as u32);
        if x.is_finite() { fnv(&mut e, serde_json::to_string(&x).unwrap().as_bytes()); fnv(&mut e, b"|"); }
        if y.is_finite() { fnv(&mut f, serde_json::to_string(&y).unwrap().as_bytes()); fnv(&mut f, b"|"); }
        n += 1; }
    println!("json_f64 {:016x}\njson_f32 {:016x}", e, f);
    for x in [0.0f64, -0.0, 1.0, 0.1, 1e20, 1e-7, 5e-324, 1e16, 1e15, 123.0, 1.5e300, 0.000123, 1e-5, 1e-6, 1.2345e-6, 9.999e15, 1.7976931348623157e308, 3e-5, 1e100, 2.5e-100, 123456.789, f64::NAN] {
        println!("f64 {:?} {}", x, serde_json::to_string(&x).unwrap()); }
    for x in [0.1f32, 1e12, 1e13, 1e-6, 1e-7, 3.4028235e38, 1.0e-45, 16777216.0, 0.3] { println!("f32 {:?} {}", x, serde_json::to_string(&x).unwrap()); }
    // serde Value: pretty, compact, key order, escapes
    let v: serde_json::Value = serde_json::from_str(r#"{"b":1,"a":[1,2.5,{"z":null,"y":true}],"a":"dup","s":"q\"\\\u0001\u007f\n\t/é","e":{},"l":[],"n":-12,"u":18446744073709551615}"#).unwrap();
    println!("compact {}", serde_json::to_string(&v).unwrap());
    println!("pretty {}", serde_json::to_string_pretty(&v).unwrap().replace('\n', "\\n"));
    for t in ["", "  \n ", "[1,]", "{\"a\" 1}", "[1] x", "tru", "\"\\ud800\"", "01", "[1e]", "\"a\u{1}\"", &"[".repeat(127), &format!("{}{}", "[".repeat(127), "]".repeat(127)), &"[".repeat(128)] {
        match serde_json::from_str::<serde_json::Value>(t) { Ok(_) => println!("parse ok {:?}", t.len()), Err(e) => println!("parse err {:?} {}", if t.len() > 20 { "<long>" } else { t }, e) } }
    for t in ["", "QQ==", "QQ", "QQ=", "Q", "QR==", "QUJD", "QUJDRA==", "QU=D", "Q===", "QUJD====", "QUJ!", "!UJD", "QUJDR===", "Zm9vYmFy", "Zm9vYg"] {
        match base64::engine::general_purpose::STANDARD.decode(t) { Ok(v) => println!("b64 {:?} ok {:?}", t, v), Err(e) => println!("b64 {:?} err {}", t, e) } }
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

fn fourcc(b: &[u8; 4]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

fn read(path: &str) -> Vec<u8> {
    std::fs::read(path).unwrap_or_else(|e| fail(&format!("{path}: {e}")))
}

fn fail(msg: &str) -> ! {
    println!("{}", serde_json::json!({ "error": msg }));
    std::process::exit(1)
}

fn psd_info(path: &str) {
    use serde_json::json;
    let f = photocraft_psd::file::PsdFile::from_bytes(&read(path)).unwrap_or_else(|e| fail(&e.to_string()));
    let h = &f.header;
    let resources: Vec<_> = f.resources.iter().map(|r| json!({"sig": fourcc(&r.signature), "id": r.id, "name_hex": hex(&r.name), "len": r.data.len()})).collect();
    let layers: Vec<_> = f.layer_info.as_ref().map(|li| li.layers.iter().map(|l| json!({
        "name_hex": hex(&l.name),
        "name": String::from_utf8_lossy(&l.name),
        "rect": [l.rect.top, l.rect.left, l.rect.bottom, l.rect.right],
        "blend": fourcc(&l.blend_mode.key()),
        "opacity": l.opacity,
        "clipping": l.clipping,
        "flags": l.flags.0,
        "channels": l.channels.iter().map(|c| json!({"id": c.id, "compression": c.compression.map(|x| x.as_u16()), "len": c.data.len()})).collect::<Vec<_>>(),
        "blocks": l.blocks.iter().map(|b| json!({"sig": fourcc(&b.signature), "key": fourcc(&b.key), "len": b.data.len()})).collect::<Vec<_>>(),
    })).collect::<Vec<_>>()).unwrap_or_default();
    let out = json!({
        "version": if h.version.is_psb() { 2 } else { 1 },
        "channels": h.channels, "width": h.width, "height": h.height, "depth": h.depth, "color_mode": h.color_mode.as_u16(),
        "color_mode_data_len": f.color_mode_data.len(),
        "resources": resources,
        "has_layer_info": f.layer_info.is_some(),
        "merged_alpha": f.layer_info.as_ref().map(|li| li.merged_alpha),
        "layers": layers,
        "global_blocks": f.global_blocks.iter().map(|b| json!({"sig": fourcc(&b.signature), "key": fourcc(&b.key), "len": b.data.len()})).collect::<Vec<_>>(),
        "image_compression": f.image_data.compression.as_u16(),
        "image_len": f.image_data.data.len(),
    });
    println!("{out}");
}

fn decode(path: &str, raw: &str) {
    let img = photocraft_codecs::decode(&read(path)).unwrap_or_else(|e| fail(&e.to_string()));
    std::fs::write(raw, img.data()).unwrap_or_else(|e| fail(&format!("{raw}: {e}")));
    println!("{}", serde_json::json!({
        "width": img.width(), "height": img.height(),
        "layout": format!("{:?}", img.layout()), "sample": format!("{:?}", img.sample_type()),
        "bytes": img.data().len(), "icc_len": img.icc.as_ref().map(|i| i.len()),
        "warnings": img.warnings.iter().map(|w| w.to_string()).collect::<Vec<_>>(),
    }));
}

fn composite(path: &str, raw: &str) {
    let r = photocraft_io::import(path, &read(path)).unwrap_or_else(|e| fail(&e.to_string()));
    let buf = photocraft_compose::flatten(&r.document);
    let mut bytes = Vec::with_capacity(buf.px.len() * 16);
    for p in &buf.px {
        for c in p {
            bytes.extend_from_slice(&c.to_le_bytes());
        }
    }
    std::fs::write(raw, &bytes).unwrap_or_else(|e| fail(&format!("{raw}: {e}")));
    println!("{}", serde_json::json!({
        "x0": buf.rect.x0, "y0": buf.rect.y0, "x1": buf.rect.x1, "y1": buf.rect.y1,
        "warnings": r.warnings,
    }));
}

/// The same sweep as tests/geom/test_warp.c; each hash covers the raw little-endian f64 bits.
fn geom_vectors() {
    use photocraft_geom::warp::{BezierMesh, Warp, WarpStyle};
    use photocraft_geom::{Affine, Point};
    let put = |h: &mut u64, v: f64| fnv(h, &v.to_bits().to_le_bytes());
    let b = [10.0, 20.0, 110.0, 70.0];
    // 1. Every style x bend x distortion x orientation over a grid reaching outside the box.
    let mut hs = 0xcbf29ce484222325u64;
    for style in WarpStyle::all() {
        for bend in [-100.0, -37.5, 0.0, 25.0, 100.0] {
            for (hd, vd) in [(0.0, 0.0), (30.0, -20.0)] {
                for vertical in [false, true] {
                    let w = Warp { style, bend, h_distort: hd, v_distort: vd, vertical, bounds: b, mesh: None };
                    for j in -2..=12 {
                        for i in -2..=12 {
                            let x = 10.0 + 100.0 * i as f64 / 10.0;
                            let y = 20.0 + 50.0 * j as f64 / 10.0;
                            let (mx, my) = w.map(x, y);
                            put(&mut hs, mx);
                            put(&mut hs, my);
                        }
                    }
                    fnv(&mut hs, &[w.is_identity() as u8]);
                    for v in w.output_bounds() {
                        put(&mut hs, v);
                    }
                }
            }
        }
    }
    println!("styles {hs:016x}");
    // 2. Meshes: fit, eval, splits, param_at, merge, bounds, preset fit, affine composition.
    let mut hm = 0xcbf29ce484222325u64;
    let grid = |m: &BezierMesh, h: &mut u64| {
        for j in 0..=20 {
            for i in 0..=20 {
                let p = m.eval(i as f64 / 20.0, j as f64 / 20.0);
                put(h, p[0]);
                put(h, p[1]);
            }
        }
    };
    let pts = |m: &BezierMesh, h: &mut u64| {
        for p in &m.points {
            put(h, p[0]);
            put(h, p[1]);
        }
    };
    let mut m = BezierMesh::identity(b, 1, 1);
    m.points[5] = [50.0, 10.0];
    m.points[10] = [70.0, 65.0];
    grid(&m, &mut hm);
    fnv(&mut hm, &[m.split_u(0.3) as u8, m.split_v(0.6) as u8, m.split_u(0.75) as u8, m.split_u(0.3) as u8]);
    pts(&m, &mut hm);
    grid(&m, &mut hm);
    for (s, t) in [(0.42, 0.77), (0.05, 0.95), (0.5, 0.5), (0.9, 0.1), (0.3, 0.6)] {
        let (ps, pt) = m.param_at(m.eval(s, t));
        put(&mut hm, ps);
        put(&mut hm, pt);
    }
    fnv(&mut hm, &[m.remove_split_u(1) as u8, m.remove_split_v(1) as u8, m.remove_split_u(9) as u8]);
    pts(&m, &mut hm);
    for v in m.control_bounds() {
        put(&mut hm, v);
    }
    let bulge = Warp::preset(WarpStyle::Bulge, 40.0, b);
    pts(&bulge.to_mesh(4, 4), &mut hm);
    let t = bulge.then_affine([0.9, 0.1, -0.2, 1.1, 5.0, -3.0]);
    for j in 0..=10 {
        for i in 0..=10 {
            let (x, y) = t.map(10.0 + 10.0 * i as f64, 20.0 + 5.0 * j as f64);
            put(&mut hm, x);
            put(&mut hm, y);
        }
    }
    fnv(&mut hm, &[t.is_identity() as u8, Warp::custom(BezierMesh::identity(b, 3, 2), b).is_identity() as u8]);
    println!("mesh {hm:016x}");
    // 3. Affine.
    let mut ha = 0xcbf29ce484222325u64;
    let a = Affine::translate(5.0, -3.0).then(&Affine::rotate(0.7)).then(&Affine::scale(2.5));
    let inv = a.inverse().unwrap_or(Affine::IDENTITY);
    for v in a.m.iter().chain(inv.m.iter()) {
        put(&mut ha, *v);
    }
    let q = inv.apply(a.apply(Point::new(12.0, -7.5)));
    put(&mut ha, q.x);
    put(&mut ha, q.y);
    put(&mut ha, a.determinant());
    println!("affine {ha:016x}");
}

struct Lcg(u64);
impl Lcg {
    fn next(&mut self) -> f32 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 40) as f32 / 16777216.0
    }
}

fn h_f32(h: &mut u64, v: &[f32]) {
    for x in v {
        fnv(h, &x.to_bits().to_le_bytes());
    }
}

fn h_f64(h: &mut u64, v: f64) {
    fnv(h, &v.to_bits().to_le_bytes());
}

/// Sweep one transform: exact and table evaluation, 8- and 16-bit buffers.
fn sweep_transform(h: &mut u64, t: &photocraft_cms::Transform) {
    let (ni, no) = (t.inputs(), t.outputs());
    let mut rng = Lcg(ni as u64 * 31 + no as u64);
    let mut out = [0f32; 16];
    for i in 0..64 {
        let mut inp = [0f32; 16];
        for v in inp.iter_mut().take(ni) {
            *v = if i == 0 { 0.0 } else if i == 1 { 1.0 } else { rng.next() };
        }
        t.eval(&inp[..ni], &mut out);
        h_f32(h, &out[..no]);
        t.eval_fast(&inp[..ni], &mut out);
        h_f32(h, &out[..no]);
    }
    let src8: Vec<u8> = (0..256 * (ni + 1)).map(|i| ((i * 37 + 11) % 256) as u8).collect();
    let mut dst8 = vec![0u8; 256 * (no + 1)];
    t.convert_u8(&src8, ni + 1, &mut dst8, no + 1, true);
    fnv(h, &dst8);
    let src16: Vec<u16> = (0..64 * ni).map(|i| ((i * 2654435761usize) % 65536) as u16).collect();
    let mut dst16 = vec![0u16; 64 * no];
    t.convert_u16(&src16, ni, &mut dst16, no, false);
    for v in dst16 {
        fnv(h, &v.to_le_bytes());
    }
}

fn cms_vectors() {
    use photocraft_cms::lutfile;
    use photocraft_cms::synth::{self, CmykModel, CmykParams};
    use photocraft_cms::{Builtin, Clut, Curve, GamutCheck, Intent, Lut3d, Transform};
    // 1. Built-in profile bytes.
    for b in Builtin::ALL {
        println!("builtin-{} {}", b.id(), blake3::hash(&b.profile().to_bytes()).to_hex());
    }
    // 2. Transforms between a set of built-ins, every intent, with and without BPC.
    let set = [Builtin::Srgb, Builtin::AdobeRgbCompat, Builtin::ProPhotoCompat, Builtin::GrayGamma22, Builtin::SGray, Builtin::LabD50, Builtin::CoatedCmyk];
    let mut ht = 0xcbf29ce484222325u64;
    for s in set {
        for d in set {
            for intent in Intent::ALL {
                for bpc in [false, true] {
                    match Transform::new(s.profile(), d.profile(), intent, bpc) {
                        Ok(t) => sweep_transform(&mut ht, &t),
                        Err(e) => fnv(&mut ht, e.to_string().as_bytes()),
                    }
                }
            }
        }
    }
    println!("transforms {ht:016x}");
    // 3. Proofs.
    let mut hp = 0xcbf29ce484222325u64;
    for sim in [false, true] {
        for intent in Intent::ALL {
            let t = Transform::proof(Builtin::Srgb.profile(), Builtin::CoatedCmyk.profile(), Builtin::Srgb.profile(), intent, true, sim).unwrap();
            sweep_transform(&mut hp, &t);
        }
    }
    println!("proofs {hp:016x}");
    // 4. Curves.
    let mut hc = 0xcbf29ce484222325u64;
    let curves = [
        Curve::Identity,
        Curve::Gamma(2.2),
        Curve::Gamma(0.0),
        photocraft_cms::curve::srgb_trc(),
        Curve::Parametric { kind: 1, p: [2.0, 0.9, 0.1, 0.0, 0.0, 0.0, 0.0] },
        Curve::Parametric { kind: 2, p: [2.0, 0.9, 0.1, 0.05, 0.0, 0.0, 0.0] },
        Curve::Parametric { kind: 3, p: [2.4, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0] },
        Curve::Parametric { kind: 4, p: [2.4, 0.9, 0.05, 0.08, 0.04, 0.01, 0.002] },
        Curve::Parametric { kind: 4, p: [2.4, 0.0, 0.05, 0.08, 0.04, 0.01, 0.002] },
        Curve::Table(vec![0.0, 0.1, 0.1, 0.5, 0.9, 1.0, 1.0]),
        Curve::Table(vec![1.0, 0.5, 0.0]),
        Curve::Table(vec![0.3]),
    ];
    for c in &curves {
        for i in -20..=140 {
            let x = i as f64 / 100.0;
            h_f64(&mut hc, c.eval64(x));
            h_f64(&mut hc, c.eval_inverse64(x));
            h_f32(&mut hc, &[c.eval(x as f32), c.eval_inverse(x as f32)]);
        }
        fnv(&mut hc, &[c.is_identity() as u8, c.is_monotonic() as u8]);
        h_f32(&mut hc, &c.sample(17));
    }
    println!("curves {hc:016x}");
    // 5. CLUTs.
    let mut hl = 0xcbf29ce484222325u64;
    let f = |p: &[f32], o: &mut [f32]| {
        let s: f32 = p.iter().enumerate().map(|(i, v)| v * (i as f32 + 1.0) * 0.1).sum();
        for (k, ov) in o.iter_mut().enumerate() {
            *ov = (s * (k as f32 + 1.0)).sin() * 0.5 + 0.5;
        }
    };
    for grid in [vec![9], vec![5, 6], vec![9, 9, 9], vec![5, 4, 3, 6], vec![3, 3, 3, 3, 3]] {
        let c = Clut::sample(grid.clone(), 3, f);
        let mut rng = Lcg(grid.len() as u64);
        let mut o = [0f32; 3];
        for _ in 0..200 {
            let inp: Vec<f32> = (0..grid.len()).map(|_| rng.next() * 1.2 - 0.1).collect();
            c.eval(&inp, &mut o);
            h_f32(&mut hl, &o);
            c.eval_multilinear(&inp, &mut o);
            h_f32(&mut hl, &o);
        }
        h_f32(&mut hl, &c.data);
    }
    println!("cluts {hl:016x}");
    // 6. Black points and same_colors.
    let mut hb = 0xcbf29ce484222325u64;
    for b in Builtin::ALL {
        for intent in Intent::ALL {
            for input in [false, true] {
                h_f64(&mut hb, photocraft_cms::black_point(b.profile(), intent, input).unwrap_or(-1.0));
            }
        }
        for o in Builtin::ALL {
            fnv(&mut hb, &[b.profile().same_colors(o.profile()) as u8]);
        }
        fnv(&mut hb, &[b.profile().is_matrix_shaper() as u8]);
    }
    let gray = Builtin::SGray.profile().gray_as_rgb().unwrap();
    fnv(&mut hb, &gray.to_bytes());
    let mut v2 = Builtin::Srgb.profile().clone();
    let table = |c: &Curve| Curve::Table((0..1024).map(|i| ((c.eval64(i as f64 / 1023.0) * 65535.0).round() / 65535.0) as f32).collect());
    let trc = v2.trc.clone().unwrap();
    v2.trc = Some([table(&trc[0]), table(&trc[1]), table(&trc[2])]);
    v2.version = (2, 0x10);
    let v2 = v2.with_encoded_bytes();
    fnv(&mut hb, &v2.to_bytes());
    fnv(&mut hb, &[v2.same_colors(Builtin::Srgb.profile()) as u8]);
    println!("profiles {hb:016x}");
    // 7. Gamut check and display LUT.
    let mut hg = 0xcbf29ce484222325u64;
    let g = GamutCheck::new(Builtin::Srgb.profile(), Builtin::CoatedCmyk.profile(), 4.0).unwrap();
    let mut rng = Lcg(99);
    for _ in 0..500 {
        let p = [rng.next(), rng.next(), rng.next()];
        h_f32(&mut hg, &[g.delta_e(&p)]);
    }
    let t = Transform::new(Builtin::Srgb.profile(), Builtin::DisplayP3.profile(), Intent::RelativeColorimetric, false).unwrap();
    let lut = Lut3d::from_transform(&t, 17);
    fnv(&mut hg, &lut.to_rgba16f_bytes());
    fnv(&mut hg, &lut.to_rgba8());
    for _ in 0..200 {
        let p = [rng.next(), rng.next(), rng.next()];
        h_f32(&mut hg, &lut.sample(p));
    }
    println!("gamut-lut3d {hg:016x}");
    // 8. LUT files.
    let mut hf = 0xcbf29ce484222325u64;
    for (id, _) in lutfile::BUILTIN {
        let l = lutfile::builtin(id).unwrap();
        h_f32(&mut hf, &l.data);
        fnv(&mut hf, lutfile::write_cube(&l).as_bytes());
    }
    let texts: [(&str, &str); 6] = [
        ("a.cube", "TITLE \"t\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"),
        ("b.cube", "LUT_1D_SIZE 3\nLUT_1D_INPUT_RANGE 0.1 0.9\n0 0 0\n0.25 0.5 0.75\n1 1 1\n"),
        ("c.cube", "DOMAIN_MIN 0 0 0.5\nDOMAIN_MAX 1 1 1\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n"),
        ("d.3dl", "0 512 1023\n0 0 0\n0 0 1023\n0 1023 0\n0 1023 1023\n1023 0 0\n1023 0 1023\n1023 1023 0\n1023 1023 1023\n"),
        ("e.cube", "LUT_3D_SIZE 2\n0 0 0\n"),
        ("f.cube", "DOMAIN_MIN 1 0 0\nDOMAIN_MAX 1 1 1\nLUT_3D_SIZE 2\n"),
    ];
    for (name, text) in texts {
        match lutfile::parse(name, text.as_bytes()) {
            Ok(l) => {
                h_f32(&mut hf, &l.data);
                fnv(&mut hf, l.title.as_bytes());
                h_f32(&mut hf, &l.domain_min);
                h_f32(&mut hf, &l.domain_max);
            }
            Err(e) => fnv(&mut hf, e.0.as_bytes()),
        }
    }
    println!("lutfiles {hf:016x}");
    // 9. Synthetic CMYK: model, separations, a small profile.
    let mut hs = 0xcbf29ce484222325u64;
    let p = CmykParams { grid_a2b: 5, grid_b2a: 5, ..CmykParams::default() };
    let m = CmykModel::coated(p.tvi);
    let mut rng = Lcg(7);
    for _ in 0..50 {
        let c = [rng.next() as f64, rng.next() as f64, rng.next() as f64, rng.next() as f64];
        for v in m.lab(c) {
            h_f64(&mut hs, v);
        }
    }
    for target in [[50.0, 0.0, 0.0], [70.0, 10.0, 10.0], [30.0, -20.0, 40.0], [95.0, 0.0, 0.0], [10.0, 0.0, 0.0]] {
        let (cmyk, de) = synth::separate(&m, &p, target, None);
        for v in cmyk {
            h_f64(&mut hs, v);
        }
        h_f64(&mut hs, de);
    }
    let prof = synth::cmyk_profile(&p);
    println!("synth {hs:016x}");
    println!("synth-small-profile {}", blake3::hash(&prof.to_bytes()).to_hex());
}

/// Hash sweeps over crates/color (tests/color/test_parity.c replays them).
fn color_vectors() {
    use photocraft_color::blend::{self, BlendMode};
    use photocraft_color::convert;
    use photocraft_color::{Color, ColorMode, SampleType};
    const F0: u64 = 0xcbf29ce484222325;
    println!("lab-k {:08x}", (3.0 * (6.0f32 / 29.0).powi(2)).to_bits());
    println!("lab-eps {:08x}", (6.0f32 / 29.0).powi(3).to_bits());
    let modes: Vec<BlendMode> = std::iter::once(BlendMode::PassThrough).chain(BlendMode::LAYER_MODES).collect();
    let v: [f32; 19] = [-0.5, -0.0, 0.0, 1e-5, 0.1, 0.25, 0.3, 0.5, 0.500_000_06, 0.6, 0.75, 0.9, 1.0 - 1e-5, 1.0, 1.5, 2.0, f32::MAX, f32::INFINITY, f32::NAN];
    let mut h = F0;
    for &m in &modes {
        for &cb in &v {
            for &cs in &v {
                h_f32(&mut h, &[blend::blend_channel(m, cb, cs), blend::blend_channel_hdr(m, cb, cs)]);
            }
        }
    }
    println!("blend-channel {h:016x}");
    let mut r = Lcg(7);
    let (mut hr, mut hc) = (F0, F0);
    for &m in &modes {
        for i in 0..3000 {
            let mut t = |k: f32| if i % 7 == 0 { (r.next() * 1.4 - 0.2) * k } else { r.next() };
            let cb = [t(1.0), t(1.0), t(1.0)];
            let cs = [t(1.0), t(1.0), t(1.0)];
            let (ab, as_, op) = (t(1.0), t(1.0), t(1.0));
            h_f32(&mut hr, &blend::blend_rgb(m, cb, cs));
            h_f32(&mut hr, &blend::blend_rgb_hdr(m, cb, cs));
            let ab = if i % 11 == 0 { 0.0 } else { ab };
            h_f32(&mut hc, &blend::composite(m, [cb[0], cb[1], cb[2], ab], [cs[0], cs[1], cs[2], as_], op));
        }
        // Equal channels and grays exercise the sort ties and the clip guards.
        for &g in &v {
            h_f32(&mut hr, &blend::blend_rgb(m, [g, g, 0.5], [0.5, g, g]));
            h_f32(&mut hr, &blend::set_lum([g, 0.2, g], 0.5));
        }
    }
    println!("blend-rgb {hr:016x}");
    println!("composite {hc:016x}");
    let mut ht = F0;
    for i in 0..=65535u32 {
        let x = i as f32 / 65535.0;
        h_f32(&mut ht, &[convert::srgb_to_linear(x), convert::linear_to_srgb(x)]);
    }
    for &x in &v {
        h_f32(&mut ht, &[convert::srgb_to_linear(x), convert::linear_to_srgb(x)]);
    }
    println!("transfer {ht:016x}");
    let (mut hl, mut hn, mut hm, mut hg) = (F0, F0, F0, F0);
    let mut r = Lcg(11);
    for i in 0..20000 {
        let rgb = [r.next() * 1.2 - 0.1, r.next() * 1.2 - 0.1, r.next() * 1.2 - 0.1];
        let lab = [r.next() * 100.0, r.next() * 256.0 - 128.0, r.next() * 256.0 - 128.0];
        h_f32(&mut hl, &convert::srgb_to_lab(rgb));
        h_f32(&mut hl, &convert::lab_to_srgb(lab));
        let c4 = [r.next(), r.next(), r.next(), r.next()];
        h_f32(&mut hn, &convert::cmyk_to_rgb_naive(c4));
        h_f32(&mut hn, &convert::rgb_to_cmyk_naive(rgb));
        h_f32(&mut hg, &[convert::rgb_to_gray(rgb)]);
        if i < 5000 {
            h_f32(&mut hm, &convert::cmyk_to_rgb(c4));
            h_f32(&mut hm, &convert::rgb_to_cmyk(rgb));
        }
    }
    h_f32(&mut hn, &convert::rgb_to_cmyk_naive([0.0, 0.0, 0.0]));
    println!("lab {hl:016x}");
    println!("naive-cmyk {hn:016x}");
    println!("managed-cmyk {hm:016x}");
    println!("gray {hg:016x}");
    // A document CMYK space (the test's synthetic uncoated-like profile).
    let params = photocraft_cms::synth::CmykParams { description: "Test Uncoated".into(), tvi: [0.26, 0.26, 0.26, 0.3], grid_a2b: 5, grid_b2a: 9, ..Default::default() };
    let bytes = std::sync::Arc::new(photocraft_cms::synth::cmyk_profile(&params).to_bytes().to_vec());
    let space = convert::CmykSpace::for_profile(Some(&bytes));
    let mut hd = F0;
    let mut r = Lcg(13);
    convert::with_cmyk_space(space.as_ref(), || {
        for _ in 0..3000 {
            let c4 = [r.next(), r.next(), r.next(), r.next()];
            let rgb = [r.next() * 1.2 - 0.1, r.next(), r.next()];
            h_f32(&mut hd, &convert::cmyk_to_rgb(c4));
            h_f32(&mut hd, &convert::rgb_to_cmyk(rgb));
        }
    });
    println!("document-cmyk {hd:016x}");
    let mut hx = F0;
    for y in -60..60 {
        for x in -60..60 {
            h_f32(&mut hx, &[photocraft_color::dither_noise(x, y)]);
        }
    }
    for (x, y) in [(i32::MIN, i32::MAX), (i32::MAX, 0), (12345, -98765)] {
        h_f32(&mut hx, &[photocraft_color::dither_noise(x, y)]);
    }
    println!("dither {hx:016x}");
    let mut hs = F0;
    let mut r = Lcg(17);
    for s in SampleType::ALL {
        let mut buf = vec![0u8; 4 * 64];
        for (i, val) in v.iter().copied().chain((0..40).map(|_| r.next() * 1.4 - 0.2)).enumerate() {
            let idx = i % 64;
            photocraft_color::write_sample(&mut buf, s, idx, val);
            h_f32(&mut hs, &[photocraft_color::read_sample(&buf, s, idx)]);
        }
        fnv(&mut hs, &buf);
    }
    println!("samples {hs:016x}");
    let mut hq = F0;
    let mut r = Lcg(19);
    let cmodes = [ColorMode::Bitmap, ColorMode::Grayscale, ColorMode::Indexed, ColorMode::Rgb, ColorMode::Cmyk, ColorMode::Lab, ColorMode::Multichannel, ColorMode::Duotone];
    for &mode in &cmodes {
        for _ in 0..500 {
            let c = Color { mode, c: [r.next(), r.next(), r.next(), r.next()], alpha: r.next() * 1.2 - 0.1 };
            h_f32(&mut hq, &c.to_rgb());
            fnv(&mut hq, &c.to_rgba8());
        }
    }
    println!("color {hq:016x}");
}

/// Hash sweeps over crates/raster (tests/raster/test_parity.c replays them).
fn raster_vectors() {
    use photocraft_color::{ColorMode, PixelFormat, SampleType};
    use photocraft_geom::{Rect, TileCoord};
    use photocraft_raster::{Surface, convert_pixel, to_rgba};
    const F0: u64 = 0xcbf29ce484222325;
    let h_rect = |h: &mut u64, r: Rect| {
        for v in [r.x0, r.y0, r.x1, r.y1] {
            fnv(h, &v.to_le_bytes());
        }
    };
    let fmts = [
        PixelFormat::RGBA8,
        PixelFormat::RGBA16,
        PixelFormat::RGBA32F,
        PixelFormat::GRAY8,
        PixelFormat::GRAYA8,
        PixelFormat::CMYKA8,
        PixelFormat::new(ColorMode::Lab, SampleType::F32, true),
        PixelFormat::new(ColorMode::Lab, SampleType::U16, false),
        PixelFormat::new(ColorMode::Grayscale, SampleType::U16, false),
    ];
    for (fi, &fmt) in fmts.iter().enumerate() {
        let n = fmt.channels();
        let mut h = F0;
        let mut r = Lcg(100 + fi as u64);
        let mut s = Surface::with_default(fmt, &vec![0.25; n]);
        let wr = Rect::new(-300, -20, 300, 40);
        let data: Vec<f32> = (0..wr.width() as usize * wr.height() as usize * n).map(|_| r.next() * 1.2 - 0.1).collect();
        s.write_region(wr, &data);
        let px: Vec<f32> = (0..n).map(|_| r.next()).collect();
        s.fill_rect(Rect::new(500, 100, 900, 700), &px);
        for (x, y) in [(-1, -1), (0, 0), (255, 255), (256, 256), (-257, 5), (100_000, -100_000)] {
            let p: Vec<f32> = (0..n).map(|_| r.next() * 1.4 - 0.2).collect();
            s.write_pixel(x, y, &p);
        }
        let big = Rect::new(-310, -30, 910, 710);
        h_f32(&mut h, &s.read_region(big));
        fnv(&mut h, &s.to_interleaved(big));
        h_rect(&mut h, s.content_bounds());
        h_rect(&mut h, s.tile_bounds());
        fnv(&mut h, &(s.tile_count() as u64).to_le_bytes());
        let rr = Rect::new(-300, -20, 600, 300);
        let mut rgba = vec![[0.0f32; 4]; (rr.width() * rr.height()) as usize];
        s.read_rgba_into(rr, &mut rgba);
        for p in &rgba {
            h_f32(&mut h, p);
        }
        let mut rgba8 = vec![[0u8; 4]; (rr.width() * rr.height()) as usize];
        s.read_rgba8_into(rr, &mut rgba8);
        for p in &rgba8 {
            fnv(&mut h, p);
        }
        for (y, x0, step) in [(15, -520, 7), (-1, -1, 1), (30, 3, 64), (500, 0, 9), (5, i32::MAX - 1000, 7), (39, -301, 300), (0, 0, -5)] {
            let mut out = vec![0.0f32; 300];
            s.sample_row_strided(y, x0, step, n - 1, &mut out);
            h_f32(&mut h, &out);
        }
        for (x, y) in [(-1, -1), (299, 39), (300, 40), (600, 600), (100_000, -100_000), (i32::MIN, i32::MAX)] {
            h_f32(&mut h, &s.pixel(x, y));
            h_f32(&mut h, &s.rgba(x, y));
            h_f32(&mut h, &[s.sample_channel(x, y, 0)]);
        }
        println!("raster-{fi} {h:016x}");
        let mut ht = F0;
        let content = s.content_bounds();
        for (dx, dy) in [(0, 0), (256, -512), (1, 0), (-3, 7), (300, -257), (-1000, 999)] {
            let t = s.translated(dx, dy, content);
            fnv(&mut ht, &t.to_interleaved(wr.translate(dx, dy)));
            fnv(&mut ht, &(t.tile_count() as u64).to_le_bytes());
            h_rect(&mut ht, t.content_bounds());
        }
        println!("translated-{fi} {ht:016x}");
        let mut hc = F0;
        for &to in &fmts {
            let c = s.convert(to);
            h_f32(&mut hc, &c.read_region(Rect::new(-20, -20, 300, 60)));
            h_f32(&mut hc, &c.default_pixel());
            fnv(&mut hc, &(c.tile_count() as u64).to_le_bytes());
            h_f32(&mut hc, &convert_pixel(&fmt, &to, &px));
            h_f32(&mut hc, &to_rgba(&fmt, &px));
        }
        println!("convert-{fi} {hc:016x}");
        let mut hp = F0;
        let mut q = s.clone();
        let mut tiles = q.take_tiles(Rect::new(-10, -10, 600, 300));
        fnv(&mut hp, &(tiles.len() as u64).to_le_bytes());
        for (c, t) in &mut tiles {
            let b = std::sync::Arc::make_mut(t).bytes_mut();
            let i = ((c.tx.unsigned_abs() * 7 + c.ty.unsigned_abs() * 13) as usize) % b.len();
            b[i] = b[i].wrapping_add(1);
        }
        q.put_tiles(tiles);
        fnv(&mut hp, &q.to_interleaved(Rect::new(-260, -20, 600, 300)));
        q.fill_rect(Rect::new(-300, -20, 300, 40), &vec![0.25; n]);
        q.prune();
        fnv(&mut hp, &(q.tile_count() as u64).to_le_bytes());
        h_rect(&mut hp, q.content_bounds());
        let _ = TileCoord::new(0, 0);
        println!("tiles-{fi} {hp:016x}");
    }
}

/// One line describing a parsed ICC profile and how it converts (or the error).
fn icc_info(path: &str) {
    use photocraft_cms::{Builtin, Intent, Profile, Transform};
    let bytes = read(path);
    print!("{}\t{path}\t", blake3::hash(&bytes).to_hex());
    let p = match Profile::parse(&bytes) {
        Ok(p) => p,
        Err(e) => {
            println!("error={e}");
            return;
        }
    };
    let mut h = 0xcbf29ce484222325u64;
    for intent in Intent::ALL {
        for bpc in [false, true] {
            for dst in [Builtin::LabD50, Builtin::Srgb] {
                match Transform::new(&p, dst.profile(), intent, bpc) {
                    Ok(t) => sweep_transform(&mut h, &t),
                    Err(e) => fnv(&mut h, e.to_string().as_bytes()),
                }
                match Transform::new(dst.profile(), &p, intent, bpc) {
                    Ok(t) => sweep_transform(&mut h, &t),
                    Err(e) => fnv(&mut h, e.to_string().as_bytes()),
                }
            }
        }
    }
    let luts: String = p.a2b.iter().chain(p.b2a.iter()).map(|l| match l {
        None => '-',
        Some(l) => match l.kind {
            photocraft_cms::profile::LutKind::Lut8 => '8',
            photocraft_cms::profile::LutKind::Lut16 => 'w',
            photocraft_cms::profile::LutKind::Ab => 'a',
        },
    }).collect();
    println!(
        "desc={}|cprt={}|ver={}.{}|class={:08x}|space={:08x}|pcs={}|intent={}|matrix={}|trc={}|gray={}|luts={}|wtpt={:?}|hash={h:016x}",
        p.description, p.copyright, p.version.0, p.version.1, p.class.sig(), p.color_space.sig(),
        if p.pcs == photocraft_cms::Pcs::Lab { "lab" } else { "xyz" }, p.rendering_intent as u32,
        p.matrix.is_some() as u8, p.trc.is_some() as u8, p.gray_trc.is_some() as u8, luts,
        p.white_point.map(f64::to_bits)
    );
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let a: Vec<&str> = args.iter().map(String::as_str).collect();
    match a.as_slice() {
        ["vectors"] => vectors(),
        ["geom-vectors"] => geom_vectors(),
        ["cms-vectors"] => cms_vectors(),
        ["color-vectors"] => color_vectors(),
        ["raster-vectors"] => raster_vectors(),
        ["icc-info", f] => icc_info(f),
        ["psd-info", f] => psd_info(f),
        ["decode", f, out] => decode(f, out),
        ["composite", f, out] => composite(f, out),
        _ => {
            eprintln!("usage: rustref vectors | psd-info FILE | decode FILE OUT.raw | composite FILE OUT.raw");
            std::process::exit(2)
        }
    }
}
