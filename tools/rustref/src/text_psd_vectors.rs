//! L15 PSD type layers / EngineData reference vectors (`rustref text-psd-vectors`). Owned by L15.
//!
//! Every line is `<kind> <args> => <result>`; tests/text/test_psd_parity.c recomputes the result
//! from the args and compares the text. Binary inputs are hex; strings in results are escaped
//! with `esc` (bytes <= ' ', `\ | , : [ ] { } "` and >= 0x7f as `\xNN`, empty as `\-`); floats
//! are hex bits; hashes are FNV-1a 64. Results longer than 300 bytes are printed as
//! `#<fnv hex>/<len>`.
//!
//! Kinds:
//!   ed <hex>                      EngineData parse: dump | write length, hash, round trip, stable
//!   edbase <name> <hex>           a sweep base (no result line)
//!   edcut <name> <cut>            parse(base[..cut])
//!   edmut <name> <pos> <byte>     parse(base with base[pos] = byte)
//!   edreal <f64 bits>             write(Real) text
//!   edint <i64>                   write(Int) text
//!   edstr <hex utf8>              write(String) text;  edname <hex utf8>: write(Name) text
//!   layer <name> <json>           a text layer (TextLayer fields as serde JSON; psd_raw "@ref")
//!   build <name> <dpi> <ink|->    build_tysh: len hash | text_layer_from_tysh dump
//!   bed <layer> <hex template|->  build_engine_data: dump of the tree (short)
//!   imp <name> <hex tysh>         import: layer dump | blank, named | style sheets dump
//!   impcut / impmut               sweeps over an imp base (short results)
//!   txt2 <tysh ref> <hex>         parse_txt2 + apply_txt2 on text_layer_from_tysh(ref)
//!   wss <tysh ref> <layer> <dpi> <json styles>   write_style_sheets: len hash | sheets | layer
//!   corpusfile <path>             number of type layers;  corpus <path> <layer>: see corpus()
//!
//! `@ref` names an earlier `build` (its 72-dpi bytes when dpi is 72) or `imp` result.

use std::collections::HashMap;

use photocraft_doc::text::{CharStyle, ParagraphStyle, TextAlign, TextRun};
use photocraft_doc::text_styles::{diff_attrs, CharacterStyleDef, ParagraphStyleDef};
use photocraft_doc::{TextLayer, TextStyles};
use photocraft_psd::descriptor::{Descriptor, Id, UnicodeString, Value as D};
use photocraft_text::engine_data::{self as ed, Value as E};
use photocraft_text::psd::{self, TySh};
use photocraft_text::psd_styles;

const H0: u64 = 0xcbf29ce484222325;

fn fnv64(b: &[u8]) -> u64 {
    let mut h = H0;
    for &x in b {
        h ^= u64::from(x);
        h = h.wrapping_mul(0x100000001b3);
    }
    h
}
fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{x:02x}"));
    }
    if s.is_empty() {
        s.push('-');
    }
    s
}
fn esc(b: &[u8]) -> String {
    let mut o = String::new();
    for &c in b {
        if c <= b' ' || c >= 0x7f || b"\\|,:[]{}\"".contains(&c) {
            o.push_str(&format!("\\x{c:02x}"));
        } else {
            o.push(c as char);
        }
    }
    if o.is_empty() {
        o.push_str("\\-");
    }
    o
}
fn short(s: String) -> String {
    if s.len() > 300 { format!("#{:016x}/{}", fnv64(s.as_bytes()), s.len()) } else { s }
}
fn lcg(s: &mut u64) -> u64 {
    *s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *s >> 33
}
macro_rules! js {
    ($v:expr) => {
        serde_json::to_string($v).unwrap()
    };
}

// ---------- EngineData ----------

fn dump(v: &E, o: &mut String) {
    match v {
        E::Dict(items) => {
            o.push('{');
            for (i, (k, x)) in items.iter().enumerate() {
                if i > 0 {
                    o.push(',');
                }
                o.push_str(&esc(k.as_bytes()));
                o.push(':');
                dump(x, o);
            }
            o.push('}');
        }
        E::Array(a) => {
            o.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    o.push(',');
                }
                dump(x, o);
            }
            o.push(']');
        }
        E::String(s) => {
            o.push('s');
            o.push_str(&esc(s.as_bytes()));
        }
        E::Int(i) => o.push_str(&format!("i{i}")),
        E::Real(r) => o.push_str(&format!("r{:016x}", r.to_bits())),
        E::Bool(b) => o.push(if *b { 'T' } else { 'F' }),
        E::Name(n) => {
            o.push('n');
            o.push_str(&esc(n.as_bytes()));
        }
    }
}
fn dumps(v: &E) -> String {
    let mut s = String::new();
    dump(v, &mut s);
    s
}

fn ed_result(input: &[u8]) -> String {
    match ed::parse(input) {
        Ok(v) => {
            let w = ed::write(&v);
            let back = ed::parse(&w);
            let rt = back.as_ref().is_ok_and(|b| *b == v);
            let stable = back.as_ref().is_ok_and(|b| ed::write(b) == w);
            format!("{} | w{} {:016x} {}{}", short(dumps(&v)), w.len(), fnv64(&w), u8::from(rt), u8::from(stable))
        }
        Err(e) => format!("E {}", esc(e.to_string().as_bytes())),
    }
}

/// `U"text"` → `(FE FF utf16be)` with escapes; everything else verbatim.
fn src(s: &str) -> Vec<u8> {
    let mut o = Vec::new();
    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'U' && b.get(i + 1) == Some(&b'"') {
            let end = s[i + 2..].find('"').map(|e| i + 2 + e).unwrap();
            let lit = s[i + 2..end].replace("\\r", "\r").replace("\\n", "\n").replace("\\t", "\t");
            o.extend_from_slice(&utf16_lit(&lit));
            i = end + 1;
        } else {
            o.push(b[i]);
            i += 1;
        }
    }
    o
}
fn utf16_lit(s: &str) -> Vec<u8> {
    let mut o = vec![b'('];
    let mut raw = vec![0xFE, 0xFF];
    for u in s.encode_utf16() {
        raw.extend_from_slice(&u.to_be_bytes());
    }
    for c in raw {
        if matches!(c, b'(' | b')' | b'\\') {
            o.push(b'\\');
        }
        o.push(c);
    }
    o.push(b')');
    o
}

const SAMPLE: &[u8] = b"\n\n<<\n\t/EngineDict\n\t<<\n\t\t/Editor\n\t\t<<\n\t\t\t/Text (\xFE\xFF\x00H\x00i\x00\\)\x00\r)\n\t\t>>\n\t\t/Nums [ 1.0 .5 -2 0 ]\n\t\t/Flag true\n\t\t/Kind /Roman\n\t\t/Runs [\n\t\t<<\n\t\t\t/A 1\n\t\t>>\n\t\t]\n\t>>\n>>";

fn ed_inputs() -> Vec<Vec<u8>> {
    let mut v: Vec<Vec<u8>> = vec![SAMPLE.to_vec()];
    for s in [
        "<< /A 1 /B -2.5 /C .5 /D 1e3 /E 1E-2 /F true /G false /H /Name /I (abc) /J [ 1 2 3 ] /K << >> /L [ ] >>",
        "<<>>",
        "[]",
        "[ ]",
        "<< /k/v >>",
        "<< / 1 >>",
        "<< /a [ << /b [ [ 1 ] [ ] ] >> ] /c [ (x) /y 1 ] >>",
        "<< /A 1 /A 2 >>",
        "[ 1 [ 2 [ 3 ] ] ]",
        "\0\0<<\0/A\t1\r\n>>",
        "<< /A 1 >> trailing garbage",
        "",
        " ",
        "<<",
        "<< /A",
        "<< /A >>",
        "<< 1 >>",
        "<< /A 1 > >>",
        "[",
        "[ 1",
        ">>",
        "]",
        ")",
        "<",
        "< /a >",
        "/abc",
        "/",
        "(abc)",
        "()",
        "(ab\\",
        "(ab",
        "(a\\)b\\\\c\\(d)",
        "<< /T (a\\nb) >>",
        "<< /T [ 1 2 ) >>",
        "12)",
        "true",
        "false",
        "True",
        "null",
        "U\"Hello world\"",
        "U\"日本語 😀 (x) \\\"",
        "<< /K U\"x\" /L [ U\"\" U\"a\" ] >>",
    ] {
        v.push(src(s));
    }
    for tok in [
        "0", "-0", "+7", "1.", ".5", "-.25", "5e", "1e5", "1e-5", "1E+2", "e5", ".", "-", "+", "1_000", "0x10", "inf", "-inf",
        "+inf", "Infinity", "INF", "nan", "-nan", "NaN", "9223372036854775807", "9223372036854775808", "-9223372036854775808",
        "-9223372036854775809", "123456789012345678901234567890", "1e400", "-1e400", "1e-400", "4.9e-324", "0.000001",
        "3.14159265358979323846264338327950288", "1.7976931348623157e308", "00012", "-00", "1.5.5", "1e5.5", "1e+", "+-1",
        "--1", "0.1e1", "1d5", "١٢",
    ] {
        v.push(format!("<< /v {tok} >>").into_bytes());
        v.push(tok.as_bytes().to_vec());
    }
    for raw in [
        &b"(\xFE\xFF\x00a\x00b)"[..],
        b"(\xFE\xFF\x00a\x00)",
        b"(\xFE\xFF\xD8\x00\x00a)",
        b"(\xFE\xFF\xDC\x00\x00a)",
        b"(\xFE\xFF\xD8\x3D\xDE\x00)",
        b"(\xFE\xFF\xD8\x3D)",
        b"(\xFE\xFF\x00\\\x29)",
        b"(\xe9\xff\x00)",
        b"(\xFE)",
        b"(\xFE\xFF)",
        b"(\xFF\xFE\x00a)",
        b"/a\xffb",
        b"/\xc3\xa9t\xc3",
        b"<< /\xff\xfe 1 >>",
        b"\xff",
        b"\xc3\xa9",
        b"1\x002",
        b"1\xff",
        b"<< /v 1\xff >>",
        b"<< /v \x80 >>",
    ] {
        v.push(raw.to_vec());
    }
    for depth in [63usize, 64, 65, 66, 200] {
        let mut a = vec![b'['; depth];
        a.extend(std::iter::repeat_n(b']', depth));
        v.push(a);
        let mut d = Vec::new();
        for _ in 0..depth {
            d.extend_from_slice(b"<< /a ");
        }
        d.extend_from_slice(b"1");
        for _ in 0..depth {
            d.extend_from_slice(b" >>");
        }
        v.push(d);
    }
    v
}

fn engine_section(big: &[u8]) {
    for input in ed_inputs() {
        println!("ed {} => {}", hex(&input), ed_result(&input));
    }
    for (name, base, step) in [("sample", SAMPLE, 1usize), ("big", big, 7)] {
        println!("edbase {name} {}", hex(base));
        let mut cut = 0;
        while cut <= base.len() {
            println!("edcut {name} {cut} => {}", short(ed_result(&base[..cut])));
            cut += step;
        }
        let mut seed = 0x5eed_u64 ^ base.len() as u64;
        for _ in 0..300 {
            let pos = (lcg(&mut seed) as usize) % base.len();
            let byte = match lcg(&mut seed) % 4 {
                0 => b"<>[]()/\\"[(lcg(&mut seed) % 8) as usize],
                1 => b"0.e-+ \x00"[(lcg(&mut seed) % 7) as usize],
                _ => (lcg(&mut seed) & 0xff) as u8,
            };
            let mut m = base.to_vec();
            m[pos] = byte;
            println!("edmut {name} {pos} {byte} => {}", short(ed_result(&m)));
        }
    }
    let mut reals: Vec<f64> = vec![
        0.0, -0.0, 1.0, -1.0, 0.5, -0.25, 12.125, 0.000004, -0.000004, 0.000005, 0.0000051, 0.000015, 0.015625, 0.000025,
        -0.015625, 0.03125, 1e15, 1e15 + 0.5, -1e15 - 0.25, 999999999999999.0, 999999999999999.5, 1e16, 1.5e16, 1e20, 1e300,
        f64::MAX, -f64::MAX, f64::MIN_POSITIVE, 5e-324, f64::NAN, -f64::NAN, f64::INFINITY, f64::NEG_INFINITY, 0.1, 0.2,
        1.0 / 3.0, 2.0 / 3.0, 123.456789, -123.4567849, 0.999999, 0.9999949, 0.99999951, 0.999995, 100.000005, 4503599627370495.5,
        2.5, 72.0 / 300.0, 300.0 / 72.0, 12.0 / 0.24,
    ];
    let mut seed = 77u64;
    for _ in 0..200 {
        let mant = (lcg(&mut seed) << 31 | lcg(&mut seed)) & ((1u64 << 52) - 1);
        let exp = 1023 - 30 + lcg(&mut seed) % 90;
        let sign = (lcg(&mut seed) & 1) << 63;
        reals.push(f64::from_bits(sign | exp << 52 | mant));
    }
    for _ in 0..60 {
        // Exact five-decimal ties and near-ties (k / 2^n).
        let n = 1 + lcg(&mut seed) % 20;
        let k = lcg(&mut seed) % 4096;
        reals.push(k as f64 / (1u64 << n) as f64);
    }
    for r in reals {
        println!("edreal {:016x} => {}", r.to_bits(), esc(&ed::write(&E::Real(r))));
    }
    for i in [0i64, -1, 1, 42, -1234567, i64::MIN, i64::MAX] {
        println!("edint {i} => {}", esc(&ed::write(&E::Int(i))));
    }
    for s in ["", "abc", "a(b)c\\", "é", "😀", "\0", "\r\n", "日本語", "\u{2829}\u{5c5c}\u{2928}", "\u{FFFD}x", "\u{10FFFF}"] {
        println!("edstr {} => {}", hex(s.as_bytes()), esc(&ed::write(&E::String(s.into()))));
        println!("edname {} => {}", hex(s.as_bytes()), esc(&ed::write(&E::Name(s.into()))));
    }
}

// ---------- layers ----------

fn layer_from(j: &str, named: &HashMap<String, Vec<u8>>) -> TextLayer {
    let v: serde_json::Value = serde_json::from_str(j).unwrap();
    let mut t = TextLayer::default();
    let get = |k: &str| v.get(k).cloned();
    if let Some(x) = get("text") {
        t.text = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("font_family") {
        t.font_family = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("size_pt") {
        t.size_pt = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("color") {
        t.color = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("transform") {
        t.transform = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("runs") {
        t.runs = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("paragraphs") {
        t.paragraphs = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("shape") {
        t.shape = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("orientation") {
        t.orientation = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("antialias") {
        t.antialias = serde_json::from_value(x).unwrap();
    }
    if let Some(x) = get("warp") {
        t.warp = serde_json::from_value(x).unwrap();
    }
    if let Some(serde_json::Value::String(r)) = get("psd_raw") {
        t.psd_raw = Some(std::sync::Arc::new(named[r.trim_start_matches('@')].clone()));
    }
    t
}

fn ldump(t: &TextLayer) -> String {
    let m: Vec<String> = t.transform.m.iter().map(|x| format!("{:016x}", x.to_bits())).collect();
    format!(
        "t={} f={} s={:08x} c={} m={} r={} p={} sh={} o={} a={} w={}",
        js!(&t.text),
        js!(&t.font_family),
        t.size_pt.to_bits(),
        js!(&t.color),
        m.join(","),
        js!(&t.runs),
        js!(&t.paragraphs),
        js!(&t.shape),
        js!(&t.orientation),
        js!(&t.antialias),
        js!(&t.warp)
    )
}

fn sdump(s: &psd_styles::PsdStyleSheets) -> String {
    let cs: Vec<String> = s.char_sheets.iter().map(|(i, n, c)| format!("{i}:{}:{}", js!(n), js!(c))).collect();
    let ps: Vec<String> = s.para_sheets.iter().map(|(i, n, p, c)| format!("{i}:{}:{}:{}", js!(n), js!(p), js!(c))).collect();
    format!(
        "nc={} np={} cs=[{}] ps=[{}] rc={} rl={} rp={}",
        js!(&s.normal_char),
        js!(&s.normal_para),
        cs.join(";"),
        ps.join(";"),
        js!(&s.run_char),
        js!(&s.run_char_lens),
        js!(&s.run_para)
    )
}

fn layer_cases() -> Vec<(&'static str, String)> {
    let mut v: Vec<(&'static str, String)> = vec![
        ("plain", r#"{"text":"Hello"}"#.into()),
        ("summary", r#"{"text":"Hi there","font_family":"Inter","size_pt":36,"color":{"mode":"Rgb","c":[0.2,0.4,0.6,0],"alpha":0.5}}"#.into()),
        ("empty", r#"{"text":""}"#.into()),
        ("ava", r#"{"text":"AVA","runs":[{"len":3,"style":{}}]}"#.into()),
        (
            "rich",
            r#"{"text":"Rich text\nsecond line","runs":[{"len":4,"style":{"font_family":"Inter","weight":700,"italic":true,"size_pt":24,"tracking":50.4,"leading_pt":30,"baseline_shift_pt":-2.5,"horizontal_scale":1.25,"vertical_scale":0.75,"underline":true,"strikethrough":true,"faux_bold":true,"faux_italic":true,"caps":"SmallCaps","ligatures":false,"discretionary_ligatures":true,"features":[{"tag":"swsh","value":1},{"tag":"frac","value":1},{"tag":"onum","value":0},{"tag":"ss01","value":1}],"color":{"mode":"Rgb","c":[1,0,0,0],"alpha":1}}},{"len":6,"style":{"font_family":"Times New Roman","weight":300,"caps":"AllCaps","features":[{"tag":"calt","value":1},{"tag":"salt","value":1},{"tag":"titl","value":1},{"tag":"ornm","value":1},{"tag":"ordn","value":1}]}},{"len":11,"style":{"postscript_name":"MyriadPro-BoldIt","font_family":"Myriad Pro","size_pt":9.5,"kern":-30}}],"paragraphs":[{"len":10,"style":{"align":"Center","first_line_indent_pt":12,"start_indent_pt":3.5,"end_indent_pt":4,"space_before_pt":6,"space_after_pt":8,"auto_leading":1.5,"hyphenate":true}},{"len":11,"style":{"align":"JustifyAll"}}]}"#.into(),
        ),
        (
            "paras",
            r#"{"text":"one\ntwo\nthree","paragraphs":[{"len":4,"style":{"align":"Right"}},{"len":4,"style":{"align":"JustifyLeft"}},{"len":5,"style":{"align":"JustifyCenter"}}]}"#.into(),
        ),
        ("paras2", r#"{"text":"a\nb","paragraphs":[{"len":2,"style":{"align":"JustifyRight"}}]}"#.into()),
        ("box", r#"{"text":"Box text that wraps","shape":{"Box":{"x":10.5,"y":-4,"width":300.25,"height":120}}}"#.into()),
        ("vertical", r#"{"text":"縦書きAB","orientation":"Vertical","runs":[{"len":11,"style":{"font_family":"Inter","size_pt":20}}]}"#.into()),
        ("warp", r#"{"text":"warped","warp":{"style":"warpArc","value":50,"horizontal_distortion":-10,"vertical_distortion":5.5,"horizontal":true}}"#.into()),
        ("warpv", r#"{"text":"warped","warp":{"style":"warpFlag","value":-35,"horizontal_distortion":10,"vertical_distortion":0,"horizontal":false}}"#.into()),
        ("warpnone", r#"{"text":"w","warp":{"style":"warpNone","value":0,"horizontal_distortion":0,"vertical_distortion":0,"horizontal":true}}"#.into()),
        ("cmyk", r#"{"text":"CMYK","runs":[{"len":4,"style":{"color":{"mode":"Cmyk","c":[0.1,0.2,0.3,0.4],"alpha":0.9}}}]}"#.into()),
        ("gray", r#"{"text":"Gray","runs":[{"len":4,"style":{"color":{"mode":"Grayscale","c":[0.5,0,0,0],"alpha":1}}}]}"#.into()),
        ("lab", r#"{"text":"Lab","runs":[{"len":3,"style":{"color":{"mode":"Lab","c":[0.5,0.6,0.4,0],"alpha":1}}}]}"#.into()),
        ("emoji", r#"{"text":"a😀b日本\nc","runs":[{"len":2,"style":{"size_pt":10}},{"len":4,"style":{"size_pt":11}},{"len":9,"style":{"size_pt":12}}]}"#.into()),
        ("toolong", r#"{"text":"abc","runs":[{"len":2,"style":{"size_pt":10}},{"len":50,"style":{"size_pt":11}}],"paragraphs":[{"len":99,"style":{"align":"Center"}}]}"#.into()),
        ("tooshort", r#"{"text":"abcdef","runs":[{"len":0,"style":{"size_pt":9}},{"len":2,"style":{"size_pt":10}}],"paragraphs":[{"len":1,"style":{"align":"Right"}}]}"#.into()),
        ("cr", "{\"text\":\"a\\rb\\nc\\r\"}".into()),
        ("transform", r#"{"text":"T","transform":{"m":[2,0.5,-0.5,2,100.25,-40]}}"#.into()),
        ("psname", r#"{"text":"PS","runs":[{"len":2,"style":{"postscript_name":"ArialMT","font_family":"Arial"}}]}"#.into()),
    ];
    for (i, w) in [100u16, 150, 151, 200, 250, 300, 350, 400, 450, 500, 550, 600, 650, 700, 750, 800, 850, 900, 1000].iter().enumerate() {
        let name: &'static str = Box::leak(format!("weight{i}").into_boxed_str());
        v.push((name, format!(r#"{{"text":"W","runs":[{{"len":1,"style":{{"weight":{w},"italic":{}}}}}]}}"#, i % 2 == 1)));
    }
    for (i, aa) in ["None", "Sharp", "Crisp", "Strong", "Smooth", "Windows", "WindowsLcd"].iter().enumerate() {
        let name: &'static str = Box::leak(format!("aa{i}").into_boxed_str());
        v.push((name, format!(r#"{{"text":"aa","antialias":"{aa}"}}"#)));
    }
    // Manual kerning cases (tests.rs psd_round_trips_manual_kerning) plus odd values.
    let kcases: [&[(usize, &str, f32)]; 8] = [
        &[(1, "Off", 100.0), (1, "Off", -50.0), (2, "Metrics", 0.0)],
        &[(1, "Metrics", 0.0), (1, "Off", 0.0), (2, "Metrics", 0.0)],
        &[(4, "Off", 0.0)],
        &[(2, "Metrics", 0.0), (1, "Off", 25.0), (1, "Metrics", 0.0)],
        &[(3, "Metrics", 0.0), (1, "Off", 300.0)],
        &[(1, "Optical", 12.4), (1, "Metrics", 12.6), (1, "Off", -0.4), (1, "Metrics", 99999.0)],
        &[(1, "Metrics", 0.5), (1, "Metrics", -0.5), (2, "Optical", 1.5)],
        &[(1, "Off", 0.0), (1, "Off", 0.0), (1, "Off", 0.0), (1, "Off", 0.0)],
    ];
    for (i, k) in kcases.iter().enumerate() {
        let runs: Vec<String> = k.iter().map(|(l, m, kern)| format!(r#"{{"len":{l},"style":{{"kerning":"{m}","kern":{kern}}}}}"#)).collect();
        let name: &'static str = Box::leak(format!("kern{i}").into_boxed_str());
        v.push((name, format!(r#"{{"text":"AVAT","runs":[{}]}}"#, runs.join(","))));
    }
    v
}

/// Builds `TySh` data for the given EngineData on a fixed base block (a `build_tysh` of "x").
fn base_tysh() -> TySh {
    let t = TextLayer { text: "x".into(), runs: vec![TextRun { len: 1, style: CharStyle::default() }], ..Default::default() };
    psd::parse_tysh(&psd::build_tysh(&t, 72.0, None)).unwrap()
}
fn set_item(d: &mut Descriptor, key: &str, v: D) {
    match d.items.iter_mut().find(|(k, _)| k.is(key)) {
        Some(e) => e.1 = v,
        None => d.items.push((Id::new(key), v)),
    }
}
fn remove_item(d: &mut Descriptor, key: &str) {
    d.items.retain(|(k, _)| !k.is(key));
}
fn enumv(t: &str, v: &str) -> D {
    D::Enumerated { type_id: Id::new(t), value: Id::new(v) }
}
fn with_ed(t: &mut TySh, ed_src: &str) {
    set_item(&mut t.text, "EngineData", D::RawData(src(ed_src)));
}

const ED_FULL: &str = r#"<< /EngineDict << /Editor << /Text U"Hello\rWorld\r" >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /Font 0 /FontSize 24 /AutoLeading false /Leading 30 /Tracking 20 /BaselineShift 1.5 /HorizontalScale .9 /VerticalScale 1.1 /FauxBold true /FauxItalic 1 /Underline true /Strikethrough 0 /Ligatures false /DLigatures true /Swash true /Fractions true /OldStyle false /FontCaps 1 /FillColor << /Type 2 /Values [ 1.0 .1 .2 .3 .4 ] >> >> >> >> << /StyleSheet << /StyleSheetData << /Font 1 /FontSize 12 /FontCaps 2 /AutoKerning false /Kerning 50 /FillColor << /Type 0 /Values [ .5 .25 ] >> >> >> >> ] /RunLengthArray [ 6 6 ] >> /ParagraphRun << /RunArray [ << /ParagraphSheet << /Properties << /Justification 2 /FirstLineIndent 10 /StartIndent 5 /EndIndent 6 /SpaceBefore 7 /SpaceAfter 8 /AutoLeading 1.5 /AutoHyphenate true >> >> >> << /ParagraphSheet << /Properties << /Justification 6 >> >> >> ] /RunLengthArray [ 6 6 ] >> /Rendered << /Shapes << /WritingDirection 0 /Children [ << /ShapeType 1 /Cookie << /Photoshop << /BoxBounds [ 0 0 200.5 100 ] >> >> >> ] >> >> >> /ResourceDict << /FontSet [ << /Name U"Arial-BoldMT" >> << /Name U"MyriadPro-It" >> ] /StyleSheetSet [ << /Name U"Normal RGB" /StyleSheetData << /Font 0 /FontSize 18 /FillColor << /Type 1 /Values [ 1 1 0 0 ] >> >> >> ] /ParagraphSheetSet [ << /Name U"Normal RGB" /Properties << /Justification 1 /AutoLeading 1.3 >> >> ] /TheNormalStyleSheet 0 /TheNormalParagraphSheet 0 >> >>"#;

const ED_SHEETS: &str = r#"<< /EngineDict << /Editor << /Text U"abcdef\rgh\r" >> /StyleRun << /RunArray [ << /StyleSheet << /Parent 1 /StyleSheetData << /FontSize 30 >> >> >> << /StyleSheet << /Name U"Emph" /StyleSheetData << >> >> >> << /StyleSheet << /Parent 0 /StyleSheetData << >> >> >> << /StyleSheet << /Parent 9 /StyleSheetData << >> >> >> << /StyleSheet << /Name U"Nope" /StyleSheetData << >> >> >> << /StyleSheet 5 >> ] /RunLengthArray [ 2 2 1 1 2 1 ] >> /ParagraphRun << /RunArray [ << /ParagraphSheet << /Parent 1 /Properties << >> >> >> << /ParagraphSheet << /Name U"Normal RGB" /Properties << >> >> >> << >> ] /RunLengthArray [ 7 3 ] >> >> /ResourceDict << /FontSet [ << /Name U"Inter-Regular" >> << /Name U"Inter-Bold" >> ] /StyleSheetSet [ << /Name U"Normal RGB" /StyleSheetData << /Font 0 /FontSize 12 /Underline false >> >> << /Name U"Big" /StyleSheetData << /FontSize 30 /Font 1 >> >> << /Name U"Emph" /StyleSheetData << /Underline true >> >> << /StyleSheetData << /Tracking 5 >> >> << /Name 7 >> ] /ParagraphSheetSet [ << /Name U"Normal RGB" /Properties << /Justification 0 >> >> << /Name U"Centered" /DefaultStyleSheet 1 /Properties << /Justification 2 >> >> << /Name U"Plain" /DefaultStyleSheet 0 /Properties << /SpaceBefore 4 >> >> << /Name U"Odd" /DefaultStyleSheet 99 >> << /Name U"Neg" /DefaultStyleSheet -1 /Properties 3 >> ] /TheNormalStyleSheet 0 /TheNormalParagraphSheet 0 >> >>"#;

fn imp_cases() -> Vec<(String, Vec<u8>)> {
    let mut out: Vec<(String, Vec<u8>)> = Vec::new();
    fn add(out: &mut Vec<(String, Vec<u8>)>, name: &str, t: &TySh) {
        out.push((name.to_string(), psd::write_tysh(t)));
    }
    macro_rules! push {
        ($n:expr, $t:expr) => {
            add(&mut out, $n, $t)
        };
    }
    let base = base_tysh();
    push!("base", &base);
    let eds: Vec<(&str, String)> = vec![
        ("full", ED_FULL.into()),
        ("sheets", ED_SHEETS.into()),
        ("sheets_docres", ED_SHEETS.replace("/ResourceDict", "/DocumentResources")),
        ("sheets_normal2", ED_SHEETS.replace("/TheNormalStyleSheet 0 /TheNormalParagraphSheet 0", "/TheNormalStyleSheet 2 /TheNormalParagraphSheet 1")),
        ("sheets_normal_oob", ED_SHEETS.replace("/TheNormalStyleSheet 0 /TheNormalParagraphSheet 0", "/TheNormalStyleSheet 50 /TheNormalParagraphSheet -1")),
        ("lens", r#"<< /EngineDict << /Editor << /Text U"a😀b日本語\r" >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /FontSize 10 >> >> >> << /StyleSheet << /StyleSheetData << /FontSize 11 >> >> >> << /StyleSheet << /StyleSheetData << /FontSize 12 >> >> >> << /StyleSheet << /StyleSheetData << /FontSize 13 >> >> >> << /StyleSheet << /StyleSheetData << /FontSize 14 >> >> >> ] /RunLengthArray [ -1 nan 2.5 1 1e30 ] >> /ParagraphRun << /RunArray [ << /ParagraphSheet << /Properties << /Justification 2.7 >> >> >> << /ParagraphSheet << /Properties << /Justification -1 >> >> >> ] /RunLengthArray [ 0 1e300 ] >> >> >>"#.into()),
        ("misaligned", r#"<< /EngineDict << /Editor << /Text U"abcd\r" >> /StyleRun << /RunArray [ << /Other 1 >> << /StyleSheet << /StyleSheetData << /FontSize 40 >> >> >> 7 ] /RunLengthArray [ 1 (x) 2 /n 2 ] >> >> >>"#.into()),
        ("fonts", r#"<< /EngineDict << /Editor << /Text U"abcde\r" >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /Font -1 >> >> >> << /StyleSheet << /StyleSheetData << /Font 99 >> >> >> << /StyleSheet << /StyleSheetData << /Font 1.9 >> >> >> << /StyleSheet << /StyleSheetData << /Font 2 >> >> >> << /StyleSheet << /StyleSheetData << /Font /Name >> >> >> ] /RunLengthArray [ 1 1 1 1 2 ] >> >> /ResourceDict << /FontSet [ << /Name U"TimesNewRomanPSMT" >> << /Name U"Helvetica-LightOblique" >> << /Other 1 >> ] >> >>"#.into()),
        ("shape3", r#"<< /EngineDict << /Editor << /Text U"x\r" >> /Rendered << /Shapes << /WritingDirection 2 /Children [ << /ShapeType 1 /Cookie << /Photoshop << /BoxBounds [ 1 2 3 ] >> >> >> ] >> >> >> >>"#.into()),
        ("shape_real", r#"<< /EngineDict << /Editor << /Text U"x\r" >> /Rendered << /Shapes << /WritingDirection 2.0 /Children [ << /ShapeType 1.0 /Cookie << /Photoshop << /BoxBounds [ -5 (a) 2.5 10 1e40 ] >> >> >> ] >> >> >> >>"#.into()),
        ("shape_box", r#"<< /EngineDict << /Editor << /Text U"x\r" >> /Rendered << /Shapes << /WritingDirection 1 /Children [ << /ShapeType 1 /Cookie << /Photoshop << /BoxBounds [ -5 2 2.5 1e40 ] >> >> >> 3 ] >> >> >> >>"#.into()),
        ("notdict", "[ 1 2 ]".into()),
        ("unparsable", "<< /A".into()),
        ("emptydict", "<< >>".into()),
        ("noeditor", "<< /EngineDict << >> >>".into()),
        ("blank", r#"<< /EngineDict << /Editor << /Text U" \r\n\t " >> >> >>"#.into()),
        ("blank2", r#"<< /EngineDict << /Editor << /Text U"  x " >> >> >>"#.into()),
        ("kernpairs", r#"<< /EngineDict << /Editor << /Text U"ABCDEF\r" >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /AutoKerning true /Kerning 0 >> >> >> << /StyleSheet << /StyleSheetData << /AutoKerning false /Kerning 50 >> >> >> << /StyleSheet << /StyleSheetData << /AutoKerning false /Kerning 0 >> >> >> << /StyleSheet << /StyleSheetData << /Kerning nan >> >> >> << /StyleSheet << /StyleSheetData << /Kerning -25.5 /AutoKerning 0 >> >> >> ] /RunLengthArray [ 1 1 2 1 2 ] >> >> /ResourceDict << /StyleSheetSet [ << /StyleSheetData << /AutoKerning false >> >> ] >> >>"#.into()),
        ("named_only", r#"<< /EngineDict << /Editor << /Text U"x\r" >> >> /ResourceDict << /StyleSheetSet [ << >> << >> ] /ParagraphSheetSet 5 >> >>"#.into()),
        ("named_para", r#"<< /EngineDict << /Editor << /Text U"x\r" >> >> /ResourceDict << /StyleSheetSet [ << >> ] /ParagraphSheetSet [ << >> << >> ] >> >>"#.into()),
        ("resnotdict", r#"<< /EngineDict << /Editor << /Text U"x\r" >> >> /ResourceDict 7 >>"#.into()),
        ("fillcolors", r#"<< /EngineDict << /Editor << /Text U"abcd\r" >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /FillColor << /Values [ ] >> >> >> >> << /StyleSheet << /StyleSheetData << /FillColor << /Values [ 1 ] >> >> >> >> << /StyleSheet << /StyleSheetData << /FillColor << /Values [ 1 2 3 ] >> >> >> >> << /StyleSheet << /StyleSheetData << /FillColor 5 >> >> >> << /StyleSheet << /StyleSheetData << /FillColor << /Values [ .5 (a) .25 1e300 -1e300 ] >> >> >> >> ] /RunLengthArray [ 1 1 1 1 1 ] >> >> >>"#.into()),
        ("bools", r#"<< /EngineDict << /Editor << /Text U"ab\r" >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << /FauxBold 2 /Ligatures 0 /AutoLeading 0 /Leading 14 /Underline (x) /FontCaps 1.9 /Titling 1 /Ornaments true /Ordinals false /ContextualLigatures true /StylisticAlternates 3 >> >> >> ] /RunLengthArray [ 3 ] >> >> >>"#.into()),
    ];
    for (name, e) in &eds {
        let mut t = base_tysh();
        with_ed(&mut t, e);
        push!(name, &t);
    }
    for (name, raw) in [
        ("latin1", &b"<< /EngineDict << /Editor << /Text (caf\xe9\r) >> >> >>"[..]),
        ("surrogates", b"<< /EngineDict << /Editor << /Text (\xFE\xFF\xD8\x00\x00a\xDC\x00\x00\r\x00b) >> >> >>"),
        ("oddbytes", b"<< /EngineDict << /Editor << /Text (\xFE\xFF\x00a\x00\r\x00) >> /StyleRun << /RunArray [ << /StyleSheet << /StyleSheetData << >> >> >> ] /RunLengthArray [ 9 ] >> >> >>"),
    ] {
        let mut t = base_tysh();
        set_item(&mut t.text, "EngineData", D::RawData(raw.to_vec()));
        push!(name, &t);
    }
    // Descriptor-level variations on the full engine data.
    let full = || {
        let mut t = base_tysh();
        with_ed(&mut t, ED_FULL);
        t
    };
    let mut t = full();
    remove_item(&mut t.text, "Ornt");
    push!("no_ornt", &t);
    let mut t = full();
    set_item(&mut t.text, "Ornt", enumv("Ornt", "Vrtc"));
    push!("vrtc", &t);
    let mut t = full();
    set_item(&mut t.text, "Ornt", D::Integer(1));
    with_ed(&mut t, &ED_FULL.replace("/WritingDirection 0", "/WritingDirection 2"));
    push!("ornt_int_wd2", &t);
    let mut t = full();
    remove_item(&mut t.text, "Ornt");
    with_ed(&mut t, &ED_FULL.replace("/WritingDirection 0", "/WritingDirection 2.0"));
    push!("no_ornt_wd2", &t);
    for aa in ["Anno", "antiAliasNone", "antiAliasSharp", "AnSh", "AnCr", "antiAliasCrisp", "AnSt", "antiAliasStrong", "antiAliasPlatformGray", "antiAliasPlatformLCD", "AnSm", "bogus"] {
        let mut t = full();
        set_item(&mut t.text, "AntA", enumv("Annt", aa));
        push!(&format!("aa_{aa}"), &t);
    }
    let mut t = full();
    set_item(&mut t.text, "AntA", D::Integer(3));
    push!("aa_int", &t);
    let mut t = full();
    remove_item(&mut t.text, "Txt ");
    push!("no_txt", &t);
    let mut t = full();
    set_item(&mut t.text, "Txt ", D::Text(UnicodeString(vec![0x41, 0, 0x42, 0xD800, 0x43, 0xDC00, 0xD83D, 0xDE00, 0x0D, 0, 0])));
    push!("txt_odd", &t);
    let mut t = base_tysh();
    set_item(&mut t.text, "Txt ", D::Text(UnicodeString::new_nul("desc\rtext")));
    remove_item(&mut t.text, "EngineData");
    push!("no_engine", &t);
    let mut t = base_tysh();
    set_item(&mut t.text, "EngineData", D::Integer(4));
    push!("engine_int", &t);
    // Warps.
    let warp = |items: Vec<(&str, D)>| {
        let mut d = Descriptor::new("warp");
        for (k, v) in items {
            d.items.push((Id::new(k), v));
        }
        d
    };
    let wcases: Vec<(&str, Descriptor)> = vec![
        ("warp_arc", warp(vec![("warpStyle", enumv("warpStyle", "warpArc")), ("warpValue", D::Double(20.0)), ("warpPerspective", D::UnitFloat { unit: *b"#Prc", value: -5.0 }), ("warpPerspectiveOther", D::Integer(3)), ("warpRotate", enumv("Ornt", "Vrtc"))])),
        ("warp_nostyle", warp(vec![("warpValue", D::Double(20.0))])),
        ("warp_badvals", warp(vec![("warpStyle", enumv("warpStyle", "warpWave")), ("warpValue", D::Boolean(true)), ("warpPerspective", D::Double(1e300)), ("warpRotate", D::Integer(1))])),
        ("warp_style_str", warp(vec![("warpStyle", D::Text(UnicodeString::new("warpArc")))])),
        ("warp_long_id", warp(vec![("warpStyle", enumv("warpStyle", "warpFishEye")), ("warpValue", D::Double(f64::NAN))])),
        ("warp_none", warp(vec![("warpStyle", enumv("warpStyle", "warpNone")), ("warpValue", D::Double(50.0))])),
    ];
    for (name, w) in wcases {
        let mut t = full();
        t.warp = Some(w);
        push!(name, &t);
    }
    let mut t = full();
    t.warp = None;
    push!("warp_absent", &t);
    // Legacy point origin (#1469) and its near misses.
    let rect = |class: &str, b: [f64; 4]| {
        D::Descriptor(
            Descriptor::new(class)
                .with("Left", D::UnitFloat { unit: *b"#Pnt", value: b[0] })
                .with("Top ", D::UnitFloat { unit: *b"#Pnt", value: b[1] })
                .with("Rght", D::UnitFloat { unit: *b"#Pnt", value: b[2] })
                .with("Btom", D::Double(b[3])),
        )
    };
    let legacy_src = TextLayer {
        text: "Name".into(),
        runs: vec![TextRun { len: 4, style: CharStyle { font_family: "Inter".into(), size_pt: 120.0, ..Default::default() } }],
        ..Default::default()
    };
    for (name, m, b) in [
        ("legacy", [4.1667, 0.0, 0.0, 4.1667, -32375.0, -32887.5], [8050.0, 8160.0, 8220.0, 8300.0]),
        ("legacy_small", [1.0, 0.0, 0.0, 1.0, 100.0, 100.0], [8050.0, 8160.0, 8220.0, 8300.0]),
        ("legacy_far", [1.0, 0.0, 0.0, 1.0, -5000.0, 0.0], [1000.0, 10.0, 1100.0, 50.0]),
        ("legacy_inverted", [4.1667, 0.0, 0.0, 4.1667, -32375.0, -32887.5], [8220.0, 8160.0, 8050.0, 8300.0]),
        ("legacy_nan", [f64::NAN, 0.0, 0.0, 4.1667, -32375.0, -32887.5], [8050.0, 8160.0, 8220.0, 8300.0]),
    ] {
        let mut t = psd::parse_tysh(&psd::build_tysh(&legacy_src, 72.0, None)).unwrap();
        t.transform = photocraft_geom::Affine { m };
        remove_item(&mut t.text, "bounds");
        remove_item(&mut t.text, "boundingBox");
        t.text.items.push((Id::new("bounds"), rect("bounds", b)));
        t.text.items.push((Id::new("boundingBox"), rect("boundingBox", b)));
        push!(name, &t);
    }
    let mut t = psd::parse_tysh(&psd::build_tysh(&legacy_src, 72.0, None)).unwrap();
    t.transform = photocraft_geom::Affine { m: [4.1667, 0.0, 0.0, 4.1667, -32375.0, -32887.5] };
    set_item(&mut t.text, "boundingBox", rect("boundingBox", [8050.0, 8160.0, 8220.0, 8300.0]));
    let mut e = psd::engine_data(&t.text).unwrap();
    if let Some(E::Array(children)) = e.get_mut("EngineDict").and_then(|d| d.get_mut("Rendered")).and_then(|r| r.get_mut("Shapes")).and_then(|s| s.get_mut("Children")) {
        if let Some(ps) = children.first_mut().and_then(|c| c.get_mut("Cookie")).and_then(|c| c.get_mut("Photoshop")) {
            ps.set("PointBase", E::Array(vec![E::Real(0.0), E::Real(1.0)]));
        }
    }
    set_item(&mut t.text, "EngineData", D::RawData(ed::write(&e)));
    push!("legacy_pointbase1", &t);
    // Trailing data variations (bounds, missing warp).
    let mut bytes = psd::write_tysh(&base);
    out.push(("tail_extra".into(), {
        let mut b = bytes.clone();
        b.extend_from_slice(&[1, 2, 3, 4, 5]);
        b
    }));
    bytes.truncate(bytes.len() - 6);
    out.push(("tail_cut".into(), bytes.clone()));
    let text_end = 52 + photocraft_psd::descriptor::VersionedDescriptor::parse_prefix(&bytes[52..]).unwrap().1;
    out.push(("no_warp".into(), bytes[..text_end].to_vec()));
    out.push(("no_warp_bounds".into(), {
        let mut b = bytes[..text_end].to_vec();
        b.extend_from_slice(&[0, 1, 0, 0, 0, 7, 0xFF, 0xFF, 0xFF, 0xF0, 0, 0]);
        b
    }));
    out.push(("short51".into(), bytes[..51].to_vec()));
    out.push(("short52".into(), bytes[..52].to_vec()));
    out
}

fn imp_result(data: &[u8], dpi: f32) -> String {
    let layer = psd::text_layer_from_tysh(data, dpi).map_or("NONE".to_string(), |l| ldump(&l));
    let sheets = psd_styles::read_style_sheets(data, dpi).map_or("NONE".to_string(), |s| sdump(&s));
    format!(
        "{} | b{} n{} | {}",
        layer,
        u8::from(psd::engine_text_is_blank(data)),
        u8::from(psd_styles::has_named_sheets(data)),
        sheets
    )
}

fn short_imp(data: &[u8]) -> String {
    let r = imp_result(data, 72.0);
    format!("#{:016x}/{}", fnv64(r.as_bytes()), r.len())
}

fn layer_section(named: &mut HashMap<String, Vec<u8>>) {
    let cases = layer_cases();
    let mut layers: HashMap<&str, TextLayer> = HashMap::new();
    for (name, json) in &cases {
        println!("layer {name} {json}");
        let t = layer_from(json, named);
        for (dpi, ink) in [(72.0f32, None), (300.0, Some([1.5f32, -2.25, 40.75, 20.0])), (0.0, None), (-5.0, None)] {
            let bytes = psd::build_tysh(&t, dpi, ink);
            let back = psd::text_layer_from_tysh(&bytes, dpi).map_or("NONE".into(), |l| ldump(&l));
            let inks = ink.map_or("-".into(), |i| i.iter().map(|v| format!("{:08x}", v.to_bits())).collect::<Vec<_>>().join(","));
            println!("build {name} {:08x} {inks} => {} {:016x} | {}", dpi.to_bits(), bytes.len(), fnv64(&bytes), back);
            if dpi == 72.0 {
                named.insert((*name).to_string(), bytes);
            }
        }
        layers.insert(*name, t);
    }
    // Templates: edit a layer whose psd_raw is an earlier build / import.
    let edits = [
        ("edit_rich", r#"{"text":"Edited\nnow","runs":[{"len":10,"style":{"size_pt":30,"postscript_name":"NewFont-Bold"}}],"psd_raw":"@rich"}"#),
        ("edit_full", r#"{"text":"Changed","psd_raw":"@full"}"#),
        ("edit_sheets", r#"{"text":"abcdef\ngh","psd_raw":"@sheets","runs":[{"len":3,"style":{"style_sheet":1}},{"len":6,"style":{"style_sheet":2}}]}"#),
        ("edit_notdict", r#"{"text":"x","psd_raw":"@notdict"}"#),
        ("edit_unparsable", r#"{"text":"x","psd_raw":"@unparsable"}"#),
        ("edit_noeditor", r#"{"text":"x","psd_raw":"@noeditor"}"#),
        ("edit_resnotdict", r#"{"text":"x","psd_raw":"@resnotdict"}"#),
        ("edit_docres", r#"{"text":"x","psd_raw":"@sheets_docres"}"#),
        ("edit_warpnone", r#"{"text":"x","psd_raw":"@warp_none"}"#),
        ("edit_warparc", r#"{"text":"x","psd_raw":"@warp_arc"}"#),
        ("edit_nowarp", r#"{"text":"x","psd_raw":"@no_warp"}"#),
        ("edit_garbage", r#"{"text":"x","psd_raw":"@short51"}"#),
        ("edit_legacy", r#"{"text":"Name","psd_raw":"@legacy","transform":{"m":[4.1667,0,0,4.1667,1166.9348,1112.7718]}}"#),
        ("edit_engineint", r#"{"text":"x","psd_raw":"@engine_int"}"#),
    ];
    for (name, json) in edits {
        println!("layer {name} {json}");
        let t = layer_from(json, named);
        for (dpi, ink) in [(72.0f32, None), (300.0, Some([8050.0f32, 8160.0, 8220.0, 8300.0]))] {
            let bytes = psd::build_tysh(&t, dpi, ink);
            let back = psd::text_layer_from_tysh(&bytes, dpi).map_or("NONE".into(), |l| ldump(&l));
            let inks = ink.map_or("-".into(), |i| i.iter().map(|v| format!("{:08x}", v.to_bits())).collect::<Vec<_>>().join(","));
            println!("build {name} {:08x} {inks} => {} {:016x} | {}", dpi.to_bits(), bytes.len(), fnv64(&bytes), back);
        }
    }
    // build_engine_data with odd templates.
    let tmpls: [Option<&str>; 8] = [
        None,
        Some("3"),
        Some("<< >>"),
        Some("<< /ResourceDict << >> >>"),
        Some("<< /EngineDict 5 /ResourceDict 7 >>"),
        Some(r#"<< /EngineDict << /Editor 3 /StyleRun [ 1 ] /ParagraphRun << /IsJoinable 9 >> /UseFractionalGlyphWidths false >> /DocumentResources << /FontSet [ << /Name U"X" >> ] >> >>"#),
        Some(r#"<< /ResourceDict << /FontSet [ << /Name U"Inter-Regular" >> << /Name 4 >> << >> ] /StyleSheetSet 1 /TheNormalStyleSheet 3 >> /DocumentResources 4 >>"#),
        Some("[ << >> ]"),
    ];
    for lname in ["ava", "rich", "empty", "box", "vertical", "kern0"] {
        for tm in tmpls {
            let tbytes = tm.map(src);
            let tv = tbytes.as_ref().map(|b| ed::parse(b).unwrap());
            let e = psd::build_engine_data(&layers[lname], tv, 96.0);
            let w = ed::write(&e);
            println!(
                "bed {lname} {} => {} | {} {:016x}",
                tbytes.as_ref().map_or("-".into(), |b| hex(b)),
                short(dumps(&e)),
                w.len(),
                fnv64(&w)
            );
        }
    }
}

fn import_section(named: &mut HashMap<String, Vec<u8>>) {
    let cases = imp_cases();
    for (name, data) in &cases {
        println!("imp {name} {} => {}", hex(data), imp_result(data, 72.0));
        println!("impdpi {name} 43960000 => {}", imp_result(data, 300.0));
        named.insert(name.clone(), data.clone());
    }
    let full = &named["full"].clone();
    let mut cut = 0;
    while cut <= full.len() {
        println!("impcut full {cut} => {}", short_imp(&full[..cut]));
        cut += if cut < 120 { 1 } else { 13 };
    }
    let mut seed = 0xfeed_u64;
    for _ in 0..400 {
        let pos = (lcg(&mut seed) as usize) % full.len();
        let byte = (lcg(&mut seed) & 0xff) as u8;
        let mut m = full.clone();
        m[pos] = byte;
        println!("impmut full {pos} {byte} => {}", short_imp(&m));
    }
}

fn txt2_with_modes(text: &str, modes: &[(usize, i64)]) -> Vec<u8> {
    let mut v = b"\n\n/98 << /0 14 >> /0 << >> /1 << /1 [ << /0 << /0 (\xfe\xff".to_vec();
    for u in format!("{text}\r").encode_utf16() {
        for x in u.to_be_bytes() {
            if matches!(x, b'(' | b')' | b'\\') {
                v.push(b'\\');
            }
            v.push(x);
        }
    }
    v.extend_from_slice(b") /6 << /0 [ ");
    for (len, m) in modes {
        v.extend_from_slice(format!("<< /0 << /0 << /0 (\u{fe}\u{ff}) /6 << /0 0 /11 {m} >> >> >> /1 {len} >> ").as_bytes());
    }
    v.extend_from_slice(b"] >> >> >> ] >>");
    v
}

fn with_text_index(tysh: &[u8], index: i32) -> Vec<u8> {
    let mut t = psd::parse_tysh(tysh).unwrap();
    t.text.items.retain(|(k, _)| !k.is("TextIndex"));
    t.text.items.push((Id::new("TextIndex"), D::Integer(index)));
    psd::write_tysh(&t)
}

fn txt2_section(named: &mut HashMap<String, Vec<u8>>) {
    let refs: Vec<(String, Vec<u8>)> = vec![
        ("ava_i0".into(), with_text_index(&named["ava"], 0)),
        ("ava_i3".into(), with_text_index(&named["ava"], 3)),
        ("ava_ineg".into(), with_text_index(&named["ava"], -1)),
        ("emoji_i1".into(), with_text_index(&named["emoji"], 1)),
        ("kern3_i0".into(), with_text_index(&named["kern3"], 0)),
    ];
    for (n, b) in &refs {
        println!("imp {n} {} => {}", hex(b), imp_result(b, 72.0));
        named.insert(n.clone(), b.clone());
    }
    let mut cases: Vec<(&str, Vec<u8>)> = vec![
        ("ava_i0", txt2_with_modes("AVA", &[(2, 2), (2, 0)])),
        ("ava_i0", txt2_with_modes("AVX", &[(4, 2)])),
        ("ava_i3", txt2_with_modes("AVA", &[(4, 2)])),
        ("ava_ineg", txt2_with_modes("AVA", &[(4, 2)])),
        ("ava_i0", b"<< /1 [ (x".to_vec()),
        ("ava_i0", Vec::new()),
        ("ava_i0", txt2_with_modes("AVA", &[(0, 2), (100, 2), (5, 9)])),
        ("ava_i0", txt2_with_modes("AVA", &[(1, 1), (1, 2), (1, 0), (1, 1)])),
        ("ava_i0", txt2_with_modes("AVA", &[])),
        ("ava_i0", b"/1 << /1 [ << /0 << /0 (\xfe\xff\x00A\x00V\x00A\x00\r) /6 << /0 [ << /1 2.5 /0 << /0 << /6 << /11 2.0 >> >> >> >> << /1 -1 >> << /1 nan /0 << /0 << /6 << /11 1 >> >> >> >> << /1 9 /0 << /0 << /6 << /11 (x) >> >> >> >> ] >> >> >> ] >>".to_vec()),
        ("ava_i0", b"/1 << /1 [ << /0 << /0 (\xfe\xff\x00A\x00V\x00A) /6 << /0 [ << /1 3 /0 << /0 << /6 << /11 2 >> >> >> >> ] >> >> >> ] >>".to_vec()),
        ("ava_i0", b"/1 << /1 [ << /0 << /0 /Name >> >> ] >>".to_vec()),
        ("ava_i0", b"/1 << /1 [ << /0 5 >> ] >>".to_vec()),
        ("ava_i0", b"/1 << /1 5 >>".to_vec()),
        ("ava_i0", b"/1 << /1 [ << /0 << /0 (\xfe\xff\x00A\x00V\x00A) /6 5 >> >> ] >>".to_vec()),
        ("emoji_i1", {
            let mut v = b"/1 << /1 [ << >> << /0 << /0 ".to_vec();
            v.extend(utf16_lit("a😀b日本\rc\r"));
            v.extend_from_slice(b" /6 << /0 [ << /1 3 /0 << /0 << /6 << /11 2 >> >> >> >> << /1 2 /0 << /0 << /6 << /11 0 >> >> >> >> << /1 9 /0 << /0 << /6 << /11 1 >> >> >> >> ] >> >> >> ] >>");
            v
        }),
        ("kern3_i0", txt2_with_modes("AVAT", &[(1, 2), (1, 0), (2, 2)])),
    ];
    cases.push(("full", txt2_with_modes("Hello\rWorld", &[(3, 2), (9, 0)])));
    for (r, data) in cases {
        let res = match psd::parse_txt2(&data) {
            None => "NOTXT2".to_string(),
            Some(txt2) => match psd::text_layer_from_tysh(&named[r], 72.0) {
                None => "NONE".to_string(),
                Some(mut l) => {
                    psd::apply_txt2(&mut l, &named[r], &txt2);
                    format!("{} {} {:08x} {}", js!(&l.runs), js!(&l.font_family), l.size_pt.to_bits(), js!(&l.color))
                }
            },
        };
        println!("txt2 {r} {} => {}", hex(&data), res);
    }
}

fn styles_section(named: &HashMap<String, Vec<u8>>) {
    // psd_styles.rs named_sheets_round_trip, plus variations.
    let mut styles = TextStyles::default();
    styles.character.push(CharacterStyleDef {
        id: 4,
        name: "Emphasis".into(),
        attrs: diff_attrs(&CharStyle { size_pt: 30.0, underline: true, ..Default::default() }, &CharStyle::default()),
    });
    styles.paragraph.push(ParagraphStyleDef {
        id: 2,
        name: "Centered".into(),
        para_attrs: diff_attrs(&ParagraphStyle { align: TextAlign::Center, ..Default::default() }, &ParagraphStyle::default()),
        ..Default::default()
    });
    let mut more = styles.clone();
    more.character.push(CharacterStyleDef {
        id: 9,
        name: "Bold Serif".into(),
        attrs: diff_attrs(&CharStyle { font_family: "Times New Roman".into(), weight: 700, italic: true, ..Default::default() }, &CharStyle::default()),
    });
    more.paragraph.push(ParagraphStyleDef {
        id: 5,
        name: "Indented".into(),
        para_attrs: diff_attrs(&ParagraphStyle { first_line_indent_pt: 18.0, ..Default::default() }, &ParagraphStyle::default()),
        char_attrs: diff_attrs(&CharStyle { size_pt: 9.0, ..Default::default() }, &CharStyle::default()),
    });
    more.basic.char_attrs = diff_attrs(&CharStyle { tracking: 10.0, ..Default::default() }, &CharStyle::default());
    let layer_json = r#"{"text":"plain bold","runs":[{"len":6,"style":{"font_family":"Inter"}},{"len":4,"style":{"font_family":"Inter","size_pt":30,"underline":true,"style_sheet":4}}],"paragraphs":[{"len":10,"style":{"align":"Center","style_sheet":2}}]}"#;
    let layer_json2 = r#"{"text":"a\nbc","runs":[{"len":1,"style":{"style_sheet":9}},{"len":3,"style":{"style_sheet":77}}],"paragraphs":[{"len":2,"style":{"style_sheet":5}},{"len":2,"style":{"style_sheet":2}}]}"#;
    let mut named2 = named.clone();
    let base_layer = layer_from(layer_json, named);
    named2.insert("rt".into(), psd::build_tysh(&base_layer, 72.0, None));
    let empty = TextStyles::default();
    let cases: Vec<(&str, &str, f32, &TextStyles)> = vec![
        ("rt", layer_json, 72.0, &styles),
        ("rt", layer_json, 300.0, &more),
        ("rt", layer_json2, 72.0, &more),
        ("rt", layer_json, 72.0, &empty),
        ("sheets", layer_json2, 72.0, &more),
        ("sheets_docres", layer_json, 72.0, &styles),
        ("full", layer_json, 72.0, &styles),
        ("resnotdict", layer_json, 72.0, &styles),
        ("notdict", layer_json, 72.0, &styles),
        ("no_engine", layer_json, 72.0, &styles),
        ("short51", layer_json, 72.0, &styles),
    ];
    println!("tyshhex rt {}", hex(&named2["rt"]));
    for (r, lj, dpi, st) in cases {
        let layer = layer_from(lj, named);
        let res = match psd_styles::write_style_sheets(&named2[r], &layer, st, dpi) {
            None => "NONE".to_string(),
            Some(out) => {
                let sh = psd_styles::read_style_sheets(&out, dpi).map_or("NONE".to_string(), |s| sdump(&s));
                let back = psd::text_layer_from_tysh(&out, dpi).map_or("NONE".into(), |l| ldump(&l));
                format!("{} {:016x} n{} | {} | {}", out.len(), fnv64(&out), u8::from(psd_styles::has_named_sheets(&out)), sh, back)
            }
        };
        println!("wss {r} {:08x} {} {} => {}", dpi.to_bits(), lj, js!(st), res);
    }
}

// ---------- corpus ----------

fn corpus() {
    for set in ["psd", "psd-tools", "photoshop"] {
        let Ok(manifest) = std::fs::read_to_string(format!("tools/corpus/{set}.sha256")) else { continue };
        for line in manifest.lines() {
            let Some(rel) = line.split_whitespace().nth(1) else { continue };
            let lower = rel.to_ascii_lowercase();
            if !(lower.ends_with(".psd") || lower.ends_with(".psb")) {
                continue;
            }
            let path = format!("{set}/{rel}");
            let Ok(data) = std::fs::read(format!("corpus/{path}")) else { continue };
            let Ok(file) = photocraft_psd::PsdFile::from_bytes(&data) else { continue };
            let txt2 = file.global_blocks.iter().find(|b| &b.key == b"Txt2").and_then(|b| psd::parse_txt2(&b.data));
            let typed: Vec<(usize, &[u8])> = file
                .layers()
                .iter()
                .enumerate()
                .filter_map(|(i, l)| l.blocks.iter().find(|b| &b.key == b"TySh").map(|b| (i, b.data.as_slice())))
                .collect();
            println!("corpusfile {path} => {} {}", typed.len(), u8::from(txt2.is_some()));
            for (i, d) in typed {
                println!("corpus {path} {i} => {}", corpus_result(d, txt2.as_ref()));
            }
        }
    }
}

fn h(s: &str) -> String {
    format!("{:016x}", fnv64(s.as_bytes()))
}

fn corpus_result(d: &[u8], txt2: Option<&E>) -> String {
    let edv = psd::parse_tysh(d).and_then(|t| psd::engine_data(&t.text));
    let ed_part = edv.as_ref().map_or("NONE".to_string(), |e| {
        let w = ed::write(e);
        format!("{}:{:016x}", w.len(), fnv64(&w))
    });
    let rt = psd::parse_tysh(d).map_or("NONE".to_string(), |t| {
        let w = psd::write_tysh(&t);
        format!("{}:{:016x}:{}", w.len(), fnv64(&w), u8::from(w == d))
    });
    let Some(mut l) = psd::text_layer_from_tysh(d, 72.0) else {
        return format!("NONE ed{ed_part} rt{rt}");
    };
    let plain = h(&ldump(&l));
    if let Some(t2) = txt2 {
        psd::apply_txt2(&mut l, d, t2);
    }
    let applied = h(&ldump(&l));
    l.psd_raw = Some(std::sync::Arc::new(d.to_vec()));
    let bt = psd::build_tysh(&l, 72.0, None);
    let back = psd::text_layer_from_tysh(&bt, 72.0).map_or("NONE".to_string(), |b| h(&ldump(&b)));
    let sheets = psd_styles::read_style_sheets(d, 72.0).map_or("NONE".to_string(), |s| h(&sdump(&s)));
    let wss = psd_styles::write_style_sheets(d, &l, &TextStyles::default(), 72.0).map_or("NONE".to_string(), |w| format!("{}:{:016x}", w.len(), fnv64(&w)));
    format!(
        "{plain} {applied} b{} n{} s{sheets} ed{ed_part} rt{rt} bt{}:{:016x} bb{back} ws{wss}",
        u8::from(psd::engine_text_is_blank(d)),
        u8::from(psd_styles::has_named_sheets(d)),
        bt.len(),
        fnv64(&bt)
    )
}

pub fn run() {
    let mut named: HashMap<String, Vec<u8>> = HashMap::new();
    // A large real-looking EngineData for the sweeps: the rich layer's.
    let rich = layer_cases().into_iter().find(|(n, _)| *n == "rich").unwrap().1;
    let big = ed::write(&psd::build_engine_data(&layer_from(&rich, &named), None, 72.0));
    println!("# l15 engine data");
    engine_section(&big);
    println!("# l15 imports");
    import_section(&mut named);
    println!("# l15 layers");
    layer_section(&mut named);
    println!("# l15 txt2");
    txt2_section(&mut named);
    println!("# l15 styles");
    styles_section(&named);
    println!("# l15 corpus");
    corpus();
}
