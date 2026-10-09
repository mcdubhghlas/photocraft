use base64::Engine;
use half::f16;
fn fnv(h: &mut u64, b: &[u8]) { for &x in b { *h ^= x as u64; *h = h.wrapping_mul(0x100000001b3); } }
fn lcg(s: &mut u64) -> u64 { *s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407); *s }
fn main() {
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
