//! TEMPORARY Rust reference dumper for the C17 rewrite (REWRITE_PLAN.md B4). Deleted in Z3.
//!
//!   rustref vectors                 serde_json / half / base64 vectors (tests/base/*)
//!   rustref geom-vectors            FNV hashes of warp/mesh/affine outputs (tests/geom/test_warp.c)
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

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let a: Vec<&str> = args.iter().map(String::as_str).collect();
    match a.as_slice() {
        ["vectors"] => vectors(),
        ["geom-vectors"] => geom_vectors(),
        ["psd-info", f] => psd_info(f),
        ["decode", f, out] => decode(f, out),
        ["composite", f, out] => composite(f, out),
        _ => {
            eprintln!("usage: rustref vectors | psd-info FILE | decode FILE OUT.raw | composite FILE OUT.raw");
            std::process::exit(2)
        }
    }
}
