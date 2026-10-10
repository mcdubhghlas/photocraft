//! TEMPORARY Rust reference dumper for the C17 rewrite (REWRITE_PLAN.md B4). Deleted in Z3.
//!
//!   rustref vectors                 serde_json / half / base64 vectors (tests/base/*)
//!   rustref geom-vectors            FNV hashes of warp/mesh/affine outputs (tests/geom/test_warp.c)
//!   rustref cms-vectors             FNV hashes of colour-management outputs (tests/cms/test_parity.c)
//!   rustref doc-vectors             document-model serde and logic vectors (tests/doc/test_parity.c)
//!   rustref unicode-alnum           char::is_alphanumeric ranges (src/base/unicode_alnum.c)
//!   rustref icc-info FILE           one line describing a parsed ICC profile (tests/cms/test_parity.c)
//!   rustref psd-info FILE           PSD/PSB structure as JSON (the F6-F9 parity contract)
//!   rustref decode FILE OUT.raw     codecs::decode: JSON metadata on stdout, raw samples to OUT.raw
//!   rustref composite FILE OUT.raw  io::import + compose::flatten: JSON on stdout, RGBA f32 LE to OUT.raw
//!
//! JSON objects are serde_json::Value (keys sorted). Failures print {"error": "..."} and exit 1,
//! so differential tests can compare errors too.
mod affinity_vectors;
mod doc_vectors;
mod raw_vectors;
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

/// Hash sweeps over crates/psd (tests/psd/test_parity.c replays them). Per testgen case: the
/// file bytes (compared by C only where no zlib stream is involved) and the decoded planes plus
/// RGBA8 previews (always compared).
fn psd_vectors() {
    use photocraft_psd::*;
    const F0: u64 = 0xcbf29ce484222325;
    for c in testgen::all_cases() {
        let f = &c.file;
        let mut hb = F0;
        fnv(&mut hb, &f.to_bytes().unwrap_or_default());
        let mut hd = F0;
        match f.decode_merged() {
            Ok(m) => fnv(&mut hd, &m),
            Err(e) => fnv(&mut hd, e.to_string().as_bytes()),
        }
        for l in f.layers() {
            for ch in &l.channels {
                match l.decode_channel(ch.id, f.header.depth, f.header.version) {
                    Ok(d) => fnv(&mut hd, &d),
                    Err(e) => fnv(&mut hd, e.to_string().as_bytes()),
                }
            }
        }
        match f.composite_rgba8() {
            Ok(img) => fnv(&mut hd, &img.data),
            Err(e) => fnv(&mut hd, e.to_string().as_bytes()),
        }
        for l in f.iter_layers() {
            match l.rgba8() {
                Ok(img) => {
                    fnv(&mut hd, &img.data);
                    fnv(&mut hd, &img.left.to_le_bytes());
                }
                Err(e) => fnv(&mut hd, e.to_string().as_bytes()),
            }
            if let Some(m) = l.user_mask() {
                match m {
                    Ok(img) => {
                        fnv(&mut hd, &img.data);
                        fnv(&mut hd, &[img.default_value]);
                    }
                    Err(e) => fnv(&mut hd, e.to_string().as_bytes()),
                }
            }
        }
        let tree = format!("{:?}", f.layer_tree());
        fnv(&mut hd, tree.as_bytes());
        println!("case\t{}\t{hb:016x}\t{hd:016x}", c.name);
    }
    // Builder documents (RLE and raw are deterministic).
    for (mode, depth) in [(ColorMode::Rgb, 8u16), (ColorMode::Rgb, 16), (ColorMode::Grayscale, 8), (ColorMode::Cmyk, 16)] {
        for comp in [Compression::Raw, Compression::Rle] {
            for version in [Version::Psd, Version::Psb] {
                let (w, h) = (7u32, 5u32);
                let spp = match mode { ColorMode::Rgb => 4, ColorMode::Grayscale => 2, _ => 5 };
                let px = |n: usize, seed: usize| -> PixelData {
                    let v8: Vec<u8> = (0..n * spp).map(|i| ((i * 31 + seed * 7) % 256) as u8).collect();
                    let v16: Vec<u16> = (0..n * spp).map(|i| ((i * 7919 + seed * 131) % 65536) as u16).collect();
                    match (mode, depth) {
                        (ColorMode::Rgb, 8) => PixelData::Rgba8(v8),
                        (ColorMode::Rgb, _) => PixelData::Rgba16(v16),
                        (ColorMode::Grayscale, 8) => PixelData::GrayA8(v8),
                        (ColorMode::Grayscale, _) => PixelData::GrayA16(v16),
                        (_, 8) => PixelData::Cmyka8(v8),
                        _ => PixelData::Cmyka16(v16),
                    }
                };
                let mut b = PsdBuilder::new(w, h).color_mode(mode).depth(depth).compression(comp).version(version).resolution(150.0).icc_profile(vec![1, 2, 3]);
                let mut l1 = LayerSpec::new("Bottom \u{e9}", -2, 1, 4, 3, px(12, 1));
                l1.mask = Some(MaskSpec { rect: Rect::from_xywh(0, 0, 3, 2), data: vec![0, 128, 255, 9, 8, 7], default_color: 255, disabled: true });
                l1.fill_opacity = Some(77);
                l1.blend_mode = BlendMode::Multiply;
                b.push_layer(l1);
                b.begin_group(GroupSpec { name: "Group".into(), blend_mode: BlendMode::Screen, opacity: 200, visible: false, open: false });
                let mut l2 = LayerSpec::new("In group", 1, 1, 2, 2, px(4, 2));
                l2.visible = false;
                l2.clipping = true;
                l2.opacity = 100;
                b.push_layer(l2);
                let _ = b.end_group();
                b.composite(px((w * h) as usize, 3));
                let mut hb = F0;
                match b.to_bytes() {
                    Ok(bytes) => fnv(&mut hb, &bytes),
                    Err(e) => fnv(&mut hb, e.to_string().as_bytes()),
                }
                println!("builder\t{mode:?} {depth} {comp:?} {version:?}\t{hb:016x}");
            }
        }
    }
    let mut hx = F0;
    fnv(&mut hx, &testgen::sample_descriptor().to_bytes());
    fnv(&mut hx, &hdr::default_toning_data());
    let sl = slices::SlicesResource {
        version: 6,
        bounds: [0, 0, 80, 100],
        group_name: "site".into(),
        slices: vec![
            slices::SliceRecord { id: 1, origin: 2, name: "logo".into(), kind: 1, rect: [20, 10, 60, 30], url: "u".into(), color: [255, 1, 2, 3], ..Default::default() },
            slices::SliceRecord { id: 2, origin: 1, layer_id: Some(7), kind: 0, rect: [5, 40, 25, 70], outsets: [1, 2, 3, 4], ..Default::default() },
        ],
    };
    fnv(&mut hx, &sl.to_bytes());
    fnv(&mut hx, &sl.to_descriptor_bytes());
    let sp = vec![path::PsdSubpath { closed: true, operation: 2, knots: vec![path::PsdKnot { linked: true, pre: (0.1, -0.2), anchor: (0.5, 0.5), post: (1.25, 3.0) }] }];
    fnv(&mut hx, &path::PathData::from_subpaths(&sp, true).to_bytes());
    let plane = |v: u8| filter_effects::EffectsPlane::encode(Compression::Rle, &[v; 16], 4, 4, 8).unwrap();
    let mut slots = vec![None; 4];
    slots[0] = Some(plane(10));
    slots[3] = Some(plane(255));
    let rect = Rect { top: 1, left: 2, bottom: 5, right: 6 };
    let fx = filter_effects::FilterEffects { version: 3, items: vec![filter_effects::FilterEffectsItem { id: "a-1".into(), version: 1, rect, depth: 8, max_channels: 2, slots, mask: Some((rect, plane(128))) }] };
    fnv(&mut hx, &fx.to_bytes());
    println!("misc\t{hx:016x}");
}

/// Hash sweeps over crates/psd/src/tiff.rs (tests/psd/test_tiff.c replays them).
fn tiff_vectors() {
    use photocraft_psd::tiff::{self, ByteOrder, ImageSourceData};
    use photocraft_psd::*;
    const F0: u64 = 0xcbf29ce484222325;
    for c in testgen::all_cases() {
        let d = ImageSourceData::from_psd(&c.file);
        let mut h = F0;
        for order in [ByteOrder::Big, ByteOrder::Little] {
            match d.to_bytes(order) {
                Ok((b, w)) => {
                    fnv(&mut h, &b);
                    for x in &w {
                        fnv(&mut h, x.as_bytes());
                    }
                    match ImageSourceData::from_bytes(&b) {
                        Ok((back, w2)) => {
                            for x in &w2 {
                                fnv(&mut h, x.as_bytes());
                            }
                            fnv(&mut h, &back.to_bytes(ByteOrder::Big).map(|x| x.0).unwrap_or_default());
                            fnv(&mut h, &back.to_bytes(ByteOrder::Little).map(|x| x.0).unwrap_or_default());
                        }
                        Err(e) => fnv(&mut h, e.to_string().as_bytes()),
                    }
                }
                Err(e) => fnv(&mut h, e.to_string().as_bytes()),
            }
        }
        let res = tiff::resources_to_bytes(&c.file.resources).unwrap_or_default();
        fnv(&mut h, &res);
        match tiff::transcode::transcode_resources(&res, ByteOrder::Big, ByteOrder::Little) {
            Ok((l, _)) => fnv(&mut h, &l),
            Err(e) => fnv(&mut h, e.to_string().as_bytes()),
        }
        println!("tiff\t{}\t{h:016x}", c.name);
    }
}

// ---- F9: brushes, patterns, swatches, gradients ----

fn f9_str(o: &mut Vec<u8>, b: &[u8]) {
    o.extend_from_slice(&(b.len() as u32).to_le_bytes());
    o.extend_from_slice(b);
}

fn f9_outcome<T>(h: &mut u64, r: photocraft_psd::Result<T>, dump: &dyn Fn(&T, &mut Vec<u8>)) {
    match r {
        Ok(v) => {
            let mut o = b"ok".to_vec();
            dump(&v, &mut o);
            fnv(h, &o);
        }
        Err(e) => {
            let m = e.to_string();
            fnv(h, b"err");
            fnv(h, &m.as_bytes()[..m.len().min(255)]);
        }
    }
}

/// The original, every truncation and every byte XOR 0x5a.
fn f9_sweep<T>(h: &mut u64, data: &[u8], parse: &dyn Fn(&[u8]) -> photocraft_psd::Result<T>, dump: &dyn Fn(&T, &mut Vec<u8>)) {
    fnv(h, data);
    f9_outcome(h, parse(data), dump);
    for n in 0..data.len() {
        f9_outcome(h, parse(&data[..n]), dump);
    }
    for i in 0..data.len() {
        let mut c = data.to_vec();
        c[i] ^= 0x5a;
        f9_outcome(h, parse(&c), dump);
    }
}

fn f9_pattern(p: &photocraft_psd::patterns::PsdPattern, o: &mut Vec<u8>) {
    o.extend_from_slice(&p.mode.to_le_bytes());
    o.extend_from_slice(&p.width.to_le_bytes());
    o.extend_from_slice(&p.height.to_le_bytes());
    f9_str(o, p.name.as_bytes());
    f9_str(o, p.id.as_bytes());
    o.push(p.palette.is_some() as u8);
    f9_str(o, p.palette.as_deref().unwrap_or_default());
    o.extend_from_slice(&p.depth.to_le_bytes());
    o.extend_from_slice(&(p.channels.len() as u32).to_le_bytes());
    for c in &p.channels {
        f9_str(o, c);
    }
    o.push(p.alpha.is_some() as u8);
    f9_str(o, p.alpha.as_deref().unwrap_or_default());
}

fn f9_patterns(v: &Vec<photocraft_psd::patterns::PsdPattern>, o: &mut Vec<u8>) {
    o.extend_from_slice(&(v.len() as u32).to_le_bytes());
    for p in v {
        f9_pattern(p, o);
    }
}

fn f9_pat(depth: u16, alpha: bool) -> photocraft_psd::patterns::PsdPattern {
    let (w, h) = (5u32, 3u32);
    let bpp = usize::from(depth / 8);
    let plane = |s: u8| (0..w as usize * h as usize * bpp).map(|i| (i as u8).wrapping_mul(7).wrapping_add(s)).collect::<Vec<u8>>();
    photocraft_psd::patterns::PsdPattern {
        mode: 3,
        width: w,
        height: h,
        name: "Test ✓".into(),
        id: "0bd2d3ba-1234-11d4-8f8f-aabbccddeeff".into(),
        palette: None,
        depth,
        channels: vec![plane(1), plane(2), plane(3)],
        alpha: alpha.then(|| plane(9)),
    }
}

fn f9_sample(o: &mut Vec<u8>, s: &photocraft_psd::abr::AbrSample) {
    f9_str(o, s.id.as_bytes());
    o.extend_from_slice(&s.width.to_le_bytes());
    o.extend_from_slice(&s.height.to_le_bytes());
    o.extend_from_slice(&s.depth.to_le_bytes());
    f9_str(o, &s.data);
}

fn f9_abr_sample(id: &str, w: u32, h: u32, depth: u16) -> photocraft_psd::abr::AbrSample {
    let bpp = usize::from(depth / 8);
    let data = (0..w as usize * h as usize * bpp).map(|i| if (i / bpp) % 3 == 0 { 255 } else { (i % 7) as u8 * 30 }).collect();
    photocraft_psd::abr::AbrSample { id: id.into(), width: w, height: h, depth, data }
}

fn f9_vectors() {
    use photocraft_psd::descriptor::{Descriptor, Id, UnicodeString, Value, VersionedDescriptor};
    use photocraft_psd::{abr, aco, ase, grd, patterns};
    const F0: u64 = 0xcbf29ce484222325;
    let line = |name: &str, h: u64| println!("f9\t{name}\t{h:016x}");
    let bytes_or = |h: &mut u64, r: photocraft_psd::Result<Vec<u8>>| f9_outcome(h, r, &|b: &Vec<u8>, o: &mut Vec<u8>| o.extend_from_slice(b));

    // ---- aco ----
    let aco_dump = |f: &aco::AcoFile, o: &mut Vec<u8>| {
        o.push(f.has_names as u8);
        for s in &f.swatches {
            f9_str(o, s.name.as_bytes());
            let (sp, v) = s.color.to_record();
            o.extend_from_slice(&sp.to_le_bytes());
            for x in v {
                o.extend_from_slice(&x.to_le_bytes());
            }
        }
    };
    let aco_sample = vec![
        aco::AcoSwatch { name: "Pure Red".into(), color: aco::AcoColor::Rgb([65535, 0, 0]) },
        aco::AcoSwatch { name: "Hue".into(), color: aco::AcoColor::Hsb([12000, 65535, 40000]) },
        aco::AcoSwatch { name: "Cyan ink".into(), color: aco::AcoColor::Cmyk([65535, 0, 0, 1234]) },
        aco::AcoSwatch { name: "Lab −".into(), color: aco::AcoColor::Lab { l: 5321, a: -12800, b: 12700 } },
        aco::AcoSwatch { name: "Gray".into(), color: aco::AcoColor::Gray(5000) },
        aco::AcoSwatch { name: "Book 日本".into(), color: aco::AcoColor::Other { space: 3, values: [1, 2, 3, 4] } },
        aco::AcoSwatch { name: String::new(), color: aco::AcoColor::Rgb([1, 2, 3]) },
    ];
    let mut h = F0;
    let b = aco::write(&aco_sample).unwrap();
    f9_sweep(&mut h, &b, &aco::parse, &aco_dump);
    line("aco sample", h);
    let mut h = F0;
    let v1 = [0u8, 1, 0, 2, 0, 0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 8, 0x13, 0x88, 0, 0, 0, 0, 0, 0];
    f9_sweep(&mut h, &v1, &aco::parse, &aco_dump);
    let mut v2 = vec![0u8, 2, 0, 1, 0, 0, 0, 1, 0, 2, 0, 3, 0, 0];
    v2.extend_from_slice(&[0, 0, 0, 2, 0, b'O', 0, b'K']);
    f9_sweep(&mut h, &v2, &aco::parse, &aco_dump);
    let long = aco::AcoSwatch { name: "x".repeat(5000), color: aco::AcoColor::Gray(0) };
    let b = aco::write(&[long]).unwrap();
    fnv(&mut h, &b);
    f9_outcome(&mut h, aco::parse(&b), &aco_dump);
    let many = vec![aco::AcoSwatch { name: String::new(), color: aco::AcoColor::Gray(0) }; 65536];
    bytes_or(&mut h, aco::write(&many));
    let mut seed = 0x1234_5678u32;
    for len in 0..300 {
        let junk: Vec<u8> = (0..len)
            .map(|_| {
                seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (seed >> 24) as u8
            })
            .collect();
        f9_outcome(&mut h, aco::parse(&junk), &aco_dump);
        let mut v = vec![0, 1];
        v.extend_from_slice(&junk);
        f9_outcome(&mut h, aco::parse(&v), &aco_dump);
    }
    line("aco misc", h);

    // ---- ase ----
    let sw_dump = |s: &ase::AseSwatch, o: &mut Vec<u8>| {
        f9_str(o, s.name.as_bytes());
        let put = |o: &mut Vec<u8>, v: &[f32]| {
            for x in v {
                o.extend_from_slice(&x.to_bits().to_le_bytes());
            }
        };
        match &s.color {
            ase::AseColor::Rgb(c) => { o.push(0); put(o, c); }
            ase::AseColor::Cmyk(c) => { o.push(1); put(o, c); }
            ase::AseColor::Lab(c) => { o.push(2); put(o, c); }
            ase::AseColor::Gray(g) => { o.push(3); put(o, &[*g]); }
            ase::AseColor::Other { model, data } => { o.push(4); o.extend_from_slice(model); f9_str(o, data); }
        }
        o.push(match s.kind { ase::AseKind::Global => 0, ase::AseKind::Spot => 1, ase::AseKind::Process => 2 });
    };
    let ase_dump = |f: &ase::AseFile, o: &mut Vec<u8>| {
        for e in &f.entries {
            match e {
                ase::AseEntry::Swatch(s) => { o.push(b'S'); sw_dump(s, o); }
                ase::AseEntry::Group { name, swatches } => {
                    o.push(b'G');
                    f9_str(o, name.as_bytes());
                    o.extend_from_slice(&(swatches.len() as u32).to_le_bytes());
                    for s in swatches {
                        sw_dump(s, o);
                    }
                }
            }
        }
    };
    let sw = |name: &str, color: ase::AseColor, kind: ase::AseKind| ase::AseSwatch { name: name.into(), color, kind };
    let ase_sample = ase::AseFile {
        entries: vec![
            ase::AseEntry::Swatch(sw("Loose red", ase::AseColor::Rgb([1.0, 0.0, 0.0]), ase::AseKind::Process)),
            ase::AseEntry::Group {
                name: "Brand ✓".into(),
                swatches: vec![
                    sw("Ink", ase::AseColor::Cmyk([0.1, 0.2, 0.3, 0.4]), ase::AseKind::Spot),
                    sw("Lab", ase::AseColor::Lab([0.5321, -12.25, 40.5]), ase::AseKind::Global),
                    sw("Mid gray", ase::AseColor::Gray(0.5), ase::AseKind::Process),
                    sw("Odd", ase::AseColor::Other { model: *b"HSV ", data: vec![1, 2, 3, 4, 5, 6] }, ase::AseKind::Process),
                ],
            },
            ase::AseEntry::Group { name: String::new(), swatches: vec![] },
        ],
    };
    let mut h = F0;
    let b = ase::write(&ase_sample).unwrap();
    f9_sweep(&mut h, &b, &ase::parse, &ase_dump);
    line("ase sample", h);
    let mut h = F0;
    let nf = ase::AseFile { entries: vec![ase::AseEntry::Swatch(sw("n", ase::AseColor::Rgb([f32::NAN, f32::INFINITY, 0.5]), ase::AseKind::Process))] };
    let b = ase::write(&nf).unwrap();
    f9_sweep(&mut h, &b, &ase::parse, &ase_dump);
    let lenient: &[u8] = b"ASEF\0\x01\0\0\0\0\0\x04\xc0\x02\0\0\0\0\x77\x77\0\0\0\x03\x09\x09\x09\xc0\x01\0\0\0\x06\0\x02\0G\0\0\0\x01\0\0\0\x0e\0\x02\0A\0\0Gray\x3e\x80\0\0";
    f9_sweep(&mut h, lenient, &ase::parse, &ase_dump);
    let many = ase::AseFile { entries: vec![ase::AseEntry::Group { name: "g".into(), swatches: vec![sw("", ase::AseColor::Gray(0.0), ase::AseKind::Process); 100_001] }] };
    bytes_or(&mut h, ase::write(&many));
    let mut seed = 0xDEAD_BEEFu32;
    for len in 0..400 {
        let junk: Vec<u8> = (0..len)
            .map(|_| {
                seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (seed >> 24) as u8
            })
            .collect();
        let mut v = b"ASEF\0\x01\0\0\0\0\0\x10".to_vec();
        v.extend_from_slice(&junk);
        f9_outcome(&mut h, ase::parse(&v), &ase_dump);
        f9_outcome(&mut h, ase::parse(&junk), &ase_dump);
    }
    line("ase misc", h);

    // ---- grd ----
    let grd_dump = |v: &Vec<grd::GrdGradient>, o: &mut Vec<u8>| {
        for g in v {
            f9_str(o, g.name.as_bytes());
            o.push(g.noise as u8);
            o.extend_from_slice(&(g.stops.len() as u32).to_le_bytes());
            for s in &g.stops {
                o.extend_from_slice(&s.location.to_bits().to_le_bytes());
                o.extend_from_slice(&s.midpoint.to_bits().to_le_bytes());
                let (m, c): (u8, Vec<f32>) = match s.color {
                    grd::GrdColor::Rgb(c) => (0, c.to_vec()),
                    grd::GrdColor::Hsb(c) => (1, c.to_vec()),
                    grd::GrdColor::Cmyk(c) => (2, c.to_vec()),
                    grd::GrdColor::Lab(c) => (3, c.to_vec()),
                    grd::GrdColor::Gray(g) => (4, vec![g]),
                    grd::GrdColor::Foreground => (5, vec![]),
                    grd::GrdColor::Background => (6, vec![]),
                };
                o.push(m);
                for x in c {
                    o.extend_from_slice(&x.to_bits().to_le_bytes());
                }
            }
            o.extend_from_slice(&(g.opacity.len() as u32).to_le_bytes());
            for (a, b, c) in &g.opacity {
                for x in [a, b, c] {
                    o.extend_from_slice(&x.to_bits().to_le_bytes());
                }
            }
        }
    };
    let grd_sample = vec![
        grd::GrdGradient {
            name: "Sunset ✓".into(),
            noise: false,
            stops: vec![
                grd::GrdStop { location: 0.0, midpoint: 0.5, color: grd::GrdColor::Foreground },
                grd::GrdStop { location: 0.25, midpoint: 0.3, color: grd::GrdColor::Rgb([1.0, 0.5, 0.0]) },
                grd::GrdStop { location: 1.0, midpoint: 0.5, color: grd::GrdColor::Background },
                grd::GrdStop { location: 0.7, midpoint: 0.1, color: grd::GrdColor::Lab([50.0, 1.0, 2.0]) },
            ],
            opacity: vec![(0.0, 1.0, 0.5), (1.0, 0.25, 0.5), (0.333, 0.777, 0.123)],
        },
        grd::GrdGradient { name: "Noise".into(), noise: true, ..Default::default() },
    ];
    let mut h = F0;
    let b = grd::write(&grd_sample);
    f9_sweep(&mut h, &b, &grd::parse, &grd_dump);
    line("grd sample", h);
    let en = |t: &str, v: &str| Value::Enumerated { type_id: Id::new(t), value: Id::new(v) };
    let uf = |u: &[u8; 4], v: f64| Value::UnitFloat { unit: *u, value: v };
    let stop = |clr: Option<Value>, extra: Vec<(&str, Value)>| {
        let mut d = Descriptor::new("Clrt");
        if let Some(c) = clr {
            d = d.with("Clr ", c);
        }
        for (k, v) in extra {
            d = d.with(k, v);
        }
        Value::Descriptor(d)
    };
    let stops = vec![
        stop(
            Some(Value::Descriptor(Descriptor::new("HSBC").with("H   ", uf(b"#Ang", 210.5)).with("Strt", Value::Double(40.0)).with("Brgh", Value::Integer(77)))),
            vec![("Type", en("Clry", "UsrS")), ("Lctn", Value::Integer(0)), ("Mdpn", Value::Integer(50))],
        ),
        stop(
            Some(Value::Descriptor(
                Descriptor::new("CMYC").with("Cyn ", Value::Double(10.25)).with("Mgnt", Value::Double(20.0)).with("Ylw ", Value::Double(30.0)).with("Blck", Value::Double(40.5)),
            )),
            vec![("Lctn", Value::Integer(1024)), ("Mdpn", Value::Integer(33))],
        ),
        stop(
            Some(Value::Descriptor(Descriptor::new("LbCl").with("Lmnc", Value::Double(53.2)).with("A   ", Value::Double(-12.5)).with("B   ", Value::Double(40.0)))),
            vec![("Lctn", Value::LargeInteger(2048))],
        ),
        stop(Some(Value::Descriptor(Descriptor::new("Grsc").with("Gry ", Value::Double(25.0)))), vec![("Lctn", Value::Double(5000.0))]),
        stop(
            Some(Value::GlobalObject(Descriptor::new("RGBC").with("Rd  ", Value::Double(255.0)).with("Grn ", Value::Double(128.5)).with("Bl  ", Value::Double(0.0)))),
            vec![("Lctn", Value::Integer(-20)), ("Mdpn", Value::Double(1e300))],
        ),
        stop(Some(Value::Descriptor(Descriptor::new("XXXX"))), vec![]),
        stop(None, vec![("Type", en("Clry", "FrgC")), ("Lctn", Value::Integer(4096))]),
        stop(None, vec![("Type", en("Clry", "BckC")), ("Mdpn", Value::Integer(-5))]),
        Value::Integer(5),
        stop(Some(Value::Descriptor(Descriptor::new("RGBC").with("Rd  ", Value::Double(f64::from_bits(0x7ff8_0000_0000_0000))))), vec![]),
        stop(None, vec![]),
    ];
    let trns = vec![
        Value::Descriptor(Descriptor::new("TrnS").with("Opct", uf(b"#Prc", 50.0)).with("Lctn", Value::Integer(0)).with("Mdpn", Value::Integer(50))),
        Value::Descriptor(Descriptor::new("TrnS")),
        Value::Integer(3),
        Value::Descriptor(Descriptor::new("TrnS").with("Opct", Value::Double(250.0)).with("Lctn", Value::Double(-3.0))),
    ];
    let grad = Descriptor::new("Grdn")
        .with("Nm  ", Value::Text(UnicodeString::new_nul("Models ✓")))
        .with("GrdF", en("GrdF", "CstS"))
        .with("Clrs", Value::List(stops))
        .with("Trns", Value::List(trns));
    let items = vec![
        Value::Descriptor(Descriptor::new("Grdn").with("Grad", Value::Descriptor(grad))),
        Value::Integer(1),
        Value::Descriptor(Descriptor::new("Grdn")),
        Value::Descriptor(Descriptor::new("Grdn").with("Grad", Value::GlobalObject(Descriptor::new("Grdn").with("Nm  ", Value::Text(UnicodeString::new("G"))).with("GrdF", en("GrdF", "ClNs"))))),
    ];
    let mut models = b"8BGR\0\x05".to_vec();
    models.extend_from_slice(&VersionedDescriptor::new(Descriptor::new("null").with("GrdL", Value::List(items))).to_bytes());
    let mut h = F0;
    f9_sweep(&mut h, &models, &grd::parse, &grd_dump);
    f9_outcome(&mut h, grd::parse(&grd::write(&[])), &grd_dump);
    for b in [&b""[..], b"8BG", b"8BGR\0", b"8BGR\0\x03", b"8BPS\0\x05"] {
        f9_outcome(&mut h, grd::parse(b), &grd_dump);
    }
    line("grd models", h);

    // ---- patterns ----
    let pblock = |b: &[u8]| patterns::parse_pattern_block(b);
    let pfile = |b: &[u8]| patterns::parse_pat_file(b);
    for depth in [8u16, 16, 32] {
        for alpha in [false, true] {
            let ps = vec![f9_pat(depth, alpha), patterns::PsdPattern { name: "Second".into(), ..f9_pat(depth, false) }];
            let mut h = F0;
            let blk = patterns::write_pattern_block(&ps).unwrap();
            f9_sweep(&mut h, &blk, &pblock, &f9_patterns);
            let f = patterns::write_pat_file(&ps).unwrap();
            f9_sweep(&mut h, &f, &pfile, &f9_patterns);
            line(&format!("pat d{depth} a{}", alpha as u8), h);
        }
    }
    let mut extra = Vec::new();
    let mut indexed = f9_pat(8, false);
    indexed.mode = 2;
    indexed.channels.truncate(1);
    indexed.palette = Some((0..768u32).map(|i| (i * 3 % 256) as u8).collect());
    extra.push(indexed.clone());
    indexed.palette = Some(vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    indexed.name = "short palette".into();
    extra.push(indexed);
    let mut cmyk = f9_pat(16, true);
    cmyk.mode = 4;
    cmyk.channels.push(vec![0x42; 5 * 3 * 2]);
    extra.push(cmyk);
    extra.push(patterns::PsdPattern {
        mode: 1,
        width: 5,
        height: 3,
        name: "bits".into(),
        id: String::new(),
        palette: None,
        depth: 1,
        channels: vec![vec![0b1010_1000, 0b0101_0000, 0b1111_1000]],
        alpha: None,
    });
    let mut h = F0;
    for p in &extra {
        let blk = patterns::write_pattern_block(std::slice::from_ref(p)).unwrap();
        f9_sweep(&mut h, &blk, &pblock, &f9_patterns);
    }
    let set = |b: &mut [u8], at: usize, v: i32| b[at..at + 4].copy_from_slice(&v.to_be_bytes());
    for depth in [8u16, 16, 32] {
        let p = f9_pat(depth, true);
        let mut blk = patterns::write_pattern_block(std::slice::from_ref(&p)).unwrap();
        let vma = 4 + 16 + (p.name.encode_utf16().count() + 1) * 2 + 1 + p.id.len() + 8;
        let orig = blk.clone();
        for (k, v) in [(0usize, -2i32), (4, -3), (8, 4), (12, 5)] {
            set(&mut blk, vma + k, v);
        }
        f9_outcome(&mut h, patterns::parse_pattern_block(&blk), &f9_patterns);
        if depth == 32 {
            // Over the decode budget; at 8 and 16 bits it fits and would allocate gigabytes.
            let mut huge = orig.clone();
            set(&mut huge, vma + 8, 30_000);
            set(&mut huge, vma + 12, 30_000);
            f9_outcome(&mut h, patterns::parse_pattern_block(&huge), &f9_patterns);
        }
        let mut bad = orig.clone();
        set(&mut bad, vma + 4, i32::MIN);
        set(&mut bad, vma + 12, i32::MAX);
        f9_outcome(&mut h, patterns::parse_pattern_block(&bad), &f9_patterns);
        let mut zero = orig.clone();
        set(&mut zero, vma + 8, 0);
        f9_outcome(&mut h, patterns::parse_pattern_block(&zero), &f9_patterns);
    }
    let mut big = f9_pat(8, false);
    big.mode = 1;
    big.channels.truncate(1);
    let mut blk = patterns::write_pattern_block(std::slice::from_ref(&big)).unwrap();
    let vma = 4 + 16 + (big.name.encode_utf16().count() + 1) * 2 + 1 + big.id.len() + 8;
    set(&mut blk, vma + 8, 1000);
    set(&mut blk, vma + 12, 900);
    f9_outcome(&mut h, patterns::parse_pattern_block(&blk), &f9_patterns);
    for bad in [&b"8BPS\0\x01"[..], b"8BP", b""] {
        f9_outcome(&mut h, patterns::parse_pat_file(bad), &f9_patterns);
    }
    let mut toolarge = f9_pat(8, false);
    toolarge.width = 40_000;
    bytes_or(&mut h, patterns::write_pattern_block(&[toolarge]));
    line("pat misc", h);

    // ---- abr ----
    let abr_dump = |f: &abr::AbrFile, o: &mut Vec<u8>| {
        o.extend_from_slice(&f.version.to_le_bytes());
        o.extend_from_slice(&f.subversion.to_le_bytes());
        o.extend_from_slice(&(f.legacy.len() as u32).to_le_bytes());
        for b in &f.legacy {
            f9_str(o, b.name.as_bytes());
            o.extend_from_slice(&b.spacing.to_le_bytes());
            o.push(b.anti_alias as u8);
            match &b.tip {
                abr::LegacyTip::Computed { diameter, hardness, angle, roundness } => {
                    o.push(0);
                    for v in [*diameter, *hardness, *angle as u16, *roundness] {
                        o.extend_from_slice(&v.to_le_bytes());
                    }
                }
                abr::LegacyTip::Sampled(s) => {
                    o.push(1);
                    f9_sample(o, s);
                }
            }
        }
        o.extend_from_slice(&(f.samples.len() as u32).to_le_bytes());
        for s in &f.samples {
            f9_sample(o, s);
        }
        f9_patterns(&f.patterns, o);
        o.extend_from_slice(&(f.presets.len() as u32).to_le_bytes());
        for d in &f.presets {
            f9_str(o, &d.to_bytes());
        }
        o.extend_from_slice(&(f.warnings.len() as u32).to_le_bytes());
        for w in &f.warnings {
            f9_str(o, w.as_bytes());
        }
    };
    for version in [1u16, 2] {
        for rle in [false, true] {
            let brushes = vec![
                abr::LegacyBrush { name: String::new(), spacing: 25, anti_alias: true, tip: abr::LegacyTip::Computed { diameter: 19, hardness: 80, angle: -30, roundness: 50 } },
                abr::LegacyBrush {
                    name: if version == 2 { "Leaf ✓".into() } else { String::new() },
                    spacing: 40,
                    anti_alias: false,
                    tip: abr::LegacyTip::Sampled(f9_abr_sample("", 13, 9, 8)),
                },
                abr::LegacyBrush { name: String::new(), spacing: 10, anti_alias: true, tip: abr::LegacyTip::Sampled(f9_abr_sample("", 4, 6, 16)) },
            ];
            let mut h = F0;
            let b = abr::write_v12(version, &brushes, rle).unwrap();
            f9_sweep(&mut h, &b, &abr::parse, &abr_dump);
            line(&format!("abr v{version} rle{}", rle as u8), h);
        }
    }
    let samples = vec![f9_abr_sample("$abc", 20, 11, 8), f9_abr_sample("$def", 5, 5, 16)];
    let preset = Descriptor::new("brushPreset")
        .with("Nm  ", Value::Text(UnicodeString::new_nul("Grass")))
        .with("Brsh", Value::Descriptor(Descriptor::new("sampledBrush").with("sampledData", Value::Text(UnicodeString::new_nul("$abc")))));
    for sub in [1u16, 2] {
        for rle in [false, true] {
            let pats: Vec<patterns::PsdPattern> = if rle { vec![f9_pat(8, true)] } else { vec![] };
            let mut h = F0;
            let b = abr::write_v6(sub, &samples, &pats, std::slice::from_ref(&preset), rle).unwrap();
            f9_sweep(&mut h, &b, &abr::parse, &abr_dump);
            line(&format!("abr v6.{sub} rle{}", rle as u8), h);
        }
    }
    let mut h = F0;
    for b in [&b""[..], &[0, 3, 0, 0], &[0, 6, 0, 9], &[0, 2, 0xff, 0xff], &[0, 1, 0, 1, 0, 3, 0, 0, 0, 0], &[0, 6, 0, 1, b'8', b'B', b'I', b'M', b'z', b'z', b'z', b'z', 0, 0, 0, 0]] {
        f9_outcome(&mut h, abr::parse(b), &abr_dump);
    }
    let mut s = f9_abr_sample("", 2, 2, 8);
    s.width = abr::MAX_EDGE + 1;
    bytes_or(&mut h, abr::write_v12(1, &[abr::LegacyBrush { name: String::new(), spacing: 1, anti_alias: true, tip: abr::LegacyTip::Sampled(s) }], false));
    bytes_or(&mut h, abr::write_v12(3, &[], false));
    bytes_or(&mut h, abr::write_v6(3, &[], &[], &[], false));
    for v in samples[1].to_unit() {
        fnv(&mut h, &v.to_bits().to_le_bytes());
    }
    for v in samples[0].to_unit() {
        fnv(&mut h, &v.to_bits().to_le_bytes());
    }
    line("abr misc", h);
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

/// Ranges of char::is_alphanumeric as "LO HI" hex lines (src/base/unicode_alnum.c).
fn unicode_alnum() {
    let mut start: Option<u32> = None;
    for cp in 0..=0x11_0000u32 {
        let yes = char::from_u32(cp).is_some_and(char::is_alphanumeric);
        match (yes, start) {
            (true, None) => start = Some(cp),
            (false, Some(s)) => { println!("{s:X} {:X}", cp - 1); start = None; }
            _ => {}
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let a: Vec<&str> = args.iter().map(String::as_str).collect();
    match a.as_slice() {
        ["vectors"] => vectors(),
        ["geom-vectors"] => geom_vectors(),
        ["cms-vectors"] => cms_vectors(),
        ["color-vectors"] => color_vectors(),
        ["psd-vectors"] => psd_vectors(),
        ["tiff-vectors"] => tiff_vectors(),
        ["f9-vectors"] => f9_vectors(),
        ["raster-vectors"] => raster_vectors(),
        ["raw-vectors"] => raw_vectors::raw_vectors(),
        ["affinity-vectors", d] => affinity_vectors::affinity_vectors(d),
        ["affinity-mutations", f] => affinity_vectors::affinity_mutations(f),
        ["doc-vectors"] => doc_vectors::doc_vectors(),
        ["unicode-alnum"] => unicode_alnum(),
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
